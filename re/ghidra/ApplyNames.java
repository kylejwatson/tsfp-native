// Applies re/names.json to the open program: struct types first (category
// /tsfp), then function names, signatures and plate comments, then global
// labels, types and comments. Safe to run again: everything is set, not
// added, and a name that is already right is left alone.
//
// Run through re/names.py ghidra (ghidra-cli), or from the Script Manager
// with the path to names.json as the argument.
//@category TSFP

import java.io.FileReader;
import java.util.Map;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.util.data.DataTypeParser;
import ghidra.util.data.DataTypeParser.AllowedDataTypes;

public class ApplyNames extends GhidraScript {

    private static final CategoryPath CAT = new CategoryPath("/tsfp");
    private DataTypeManager dtm;
    private DataTypeParser parser;
    private int renamed, created, typed, failed;

    private String str(JsonObject o, String k) {
        JsonElement e = o.get(k);
        return e == null || e.isJsonNull() ? null : e.getAsString();
    }

    private long num(String s) {
        return Long.decode(s);
    }

    private DataType type(String text) {
        if (text == null)
            return null;
        try {
            return parser.parse(text.trim());
        } catch (Exception e) {
            printerr("  type not understood: " + text + " (" + e.getMessage() + ")");
            failed++;
            return null;
        }
    }

    private void structs(JsonArray arr) throws Exception {
        // Two passes, so a field may name another of our structs.
        for (JsonElement el : arr) {
            JsonObject s = el.getAsJsonObject();
            String name = str(s, "name");
            if (dtm.getDataType(CAT, name) == null) {
                String size = str(s, "size");
                dtm.addDataType(new StructureDataType(CAT, name, size == null ? 0 : (int) num(size)),
                                DataTypeConflictHandler.KEEP_HANDLER);
            }
        }
        for (JsonElement el : arr) {
            JsonObject s = el.getAsJsonObject();
            Structure st = (Structure) dtm.getDataType(CAT, str(s, "name"));
            for (JsonElement fe : s.getAsJsonArray("fields")) {
                JsonObject f = fe.getAsJsonObject();
                int off = (int) num(str(f, "off"));
                DataType t = type(str(f, "type"));
                if (t == null)
                    continue;
                // An empty structure reports a length of 1 but has no bytes.
                int have = st.isZeroLength() ? 0 : st.getLength();
                if (off + t.getLength() > have)
                    st.growStructure(off + t.getLength() - have);
                st.replaceAtOffset(off, t, t.getLength(), str(f, "name"), str(f, "notes"));
            }
            if (str(s, "notes") != null)
                st.setDescription(str(s, "notes"));
            typed++;
        }
    }

    private void functions(JsonArray arr) throws Exception {
        for (JsonElement el : arr) {
            JsonObject o = el.getAsJsonObject();
            Address a = toAddr(num(str(o, "addr")));
            String name = str(o, "name");
            Function f = getFunctionAt(a);
            if (f == null) {
                f = createFunction(a, name);
                if (f == null) {
                    printerr("  no function could be made at " + a + " for " + name);
                    failed++;
                    continue;
                }
                created++;
            } else if (!f.getName().equals(name)) {
                f.setName(name, SourceType.USER_DEFINED);
                renamed++;
            }
            String sig = str(o, "sig");
            if (sig != null) {
                try {
                    FunctionDefinitionDataType def = CParserUtils.parseSignature(null, currentProgram, sig, true);
                    if (def != null) {
                        def.setName(name);
                        new ApplyFunctionSignatureCmd(a, def, SourceType.USER_DEFINED).applyTo(currentProgram);
                    }
                } catch (Exception e) {
                    printerr("  signature not applied for " + name + ": " + e.getMessage());
                    failed++;
                }
            }
            String notes = str(o, "notes");
            if (notes != null)
                f.setComment(notes + (str(o, "source") != null ? "\n[" + str(o, "source") + "]" : ""));
        }
    }

    private void globals(JsonArray arr) throws Exception {
        for (JsonElement el : arr) {
            JsonObject o = el.getAsJsonObject();
            Address a = toAddr(num(str(o, "addr")));
            String name = str(o, "name");
            Symbol prim = getSymbolAt(a);
            if (prim == null || !prim.getName().equals(name)) {
                Symbol s = createLabel(a, name, true, SourceType.USER_DEFINED);
                if (s != null)
                    s.setPrimary();
                renamed++;
            }
            DataType t = type(str(o, "type"));
            if (t != null) {
                try {
                    clearListing(a, a.add(Math.max(1, t.getLength()) - 1));
                    createData(a, t);
                } catch (Exception e) {
                    printerr("  type not applied at " + a + " (" + name + "): " + e.getMessage());
                    failed++;
                }
            }
            String notes = str(o, "notes");
            if (notes != null)
                setPlateComment(a, notes);
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String path = args.length > 0 ? args[0] : askFile("names.json", "Apply").getAbsolutePath();
        JsonObject doc = JsonParser.parseReader(new FileReader(path)).getAsJsonObject();

        dtm = currentProgram.getDataTypeManager();
        parser = new DataTypeParser(dtm, dtm, null, AllowedDataTypes.ALL);

        if (doc.has("structs"))
            structs(doc.getAsJsonArray("structs"));
        if (doc.has("functions"))
            functions(doc.getAsJsonArray("functions"));
        if (doc.has("globals"))
            globals(doc.getAsJsonArray("globals"));
        println(String.format("ApplyNames: %d renamed, %d functions created, %d structs, %d problems",
                              renamed, created, typed, failed));
    }
}
