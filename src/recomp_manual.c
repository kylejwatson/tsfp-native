/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdint.h>
#include <stdlib.h>   /* getenv, exit: the spin verdict below */
#include <string.h>   /* strcmp: RECOMP_ICALL_FATAL */
#include <math.h>     /* floorf, fabsf: mounted-gun angle limits */

/* One diagnostic report, one piece.
 *
 * Each report below is a dozen fprintf calls and the CRT takes its lock per
 * call, so with more than one guest thread running the lines interleave in
 * the middle of a report. Dino Crisis 3 runs six workers once it is past its
 * menus, and its log came out like this:
 *
 *   callers: 0x004DB424 <- 0x002705B4 <- 0x002705B4[ICALL] unresolved jump
 *   target 0x41B0B870 -- 1 time(s) (total calls: 6930990)
 *
 * -- two threads' reports spliced together, with a caller list that stops
 * mid-sentence and an address that belongs to neither line as read. These
 * diagnostics exist to be read during exactly the multi-threaded bring-up
 * that breaks them.
 *
 * The CRT exposes the lock these calls were already taking one at a time, so
 * hold it across the whole report instead. */
#if defined(_MSC_VER)
#  define RECOMP_DIAG_LOCK()   _lock_file(stderr)
#  define RECOMP_DIAG_UNLOCK() _unlock_file(stderr)
#elif defined(__unix__) || defined(__APPLE__)
#  define RECOMP_DIAG_LOCK()   flockfile(stderr)
#  define RECOMP_DIAG_UNLOCK() funlockfile(stderr)
#else
#  define RECOMP_DIAG_LOCK()   ((void)0)
#  define RECOMP_DIAG_UNLOCK() ((void)0)
#endif

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* These are defined thread-local in xbox_memory_layout.c. Declaring them
 * without the same storage class here does not fail to link -- it silently
 * resolves to different storage, so every read gets 0. That is why the log
 * below reported no call site: not because the guest esp was stale, but
 * because this file was not reading the guest esp at all. */
#if defined(_MSC_VER)
#  define RECOMP_MANUAL_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_MANUAL_TLS __thread
#else
#  define RECOMP_MANUAL_TLS _Thread_local
#endif

extern RECOMP_MANUAL_TLS uint32_t g_eax;
/* The guest stack pointer. At the moment an indirect call is refused, the
 * caller has already pushed its guest return address, so the top of the
 * guest stack is the call site -- the one thing the old log did not say. */
extern RECOMP_MANUAL_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;
/* The lifted code sections, so a value on the guest stack can be told
 * apart from data when naming the callers of a refused call. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;
/* The esp the dispatch macro captured. Not g_esp, which is stale by the time
 * a refused call is reported. Zero when the title's generated header predates
 * this, and the log then says it has no callers rather than inventing them. */
extern RECOMP_MANUAL_TLS uint32_t g_icall_saved_esp;
/* Which dispatch form was refused: 0 unknown, 1 call, 2 jump. Unknown
 * means this title was lifted before the macros published it. */
extern RECOMP_MANUAL_TLS uint32_t g_icall_dispatch_form;

/* ── TimeSplitters: Future Perfect overrides ───────────────── */

/* sub_000245C0: the MSVC CRT memcpy (cdecl: dst, src, n; returns dst).
 *
 * Replaced because it enters its unrolled `movsb` tail with a computed
 * `jmp ecx` to a point in the middle of the function (0x24604..0x2460C), which
 * the lifter cannot translate: the load stopped at "[ICALL] unresolved jump
 * target 0x0002460C". A native copy is also much faster than the lifted
 * string/MMX loops. memmove, since a caller may rely on the overlap-safe
 * behaviour the original had for forward-overlapping blocks. */
void sub_000245C0(void)
{
    uint32_t dst = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp + 4);
    uint32_t src = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp + 8);
    uint32_t n   = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp + 12);

    memmove((uint8_t *)g_xbox_mem_offset + dst,
            (uint8_t *)g_xbox_mem_offset + src, n);
    g_eax = dst;
    g_esp += 4;     /* the `ret` */
}

/* sub_0025D520 (player_apply_look, re/names.json): mouse look.
 *
 * Wrapped, not replaced: the generated body runs first and integrates the
 * stick into the camera angles as it always did, then the mouse movement
 * since the last frame is added to them directly -- degrees per count, no
 * stick, so no deadzone, response curve or acceleration ramp between the
 * hand and the aim. The pad keeps working beside it.
 *
 * What the stick path does that is kept: it only turns while the player has
 * control and in the movement states that look at all, the yaw wraps into
 * [0,360) and the pitch is clamped to +-g_pitch_limit (60), and the turn is
 * scaled by the camera's fov over its default one so a zoomed scope does not
 * swing wildly. Not kept: the 0.65 slow-down while aiming (aim_turn_scale),
 * which is there for a thumb, and auto-aim.
 *
 * Pitch: positive looks up; mouse y grows downwards, so it is subtracted.
 * Yaw: grows to the left (counter-clockwise from above), so mouse x, which
 * grows to the right, is subtracted too (it was added at first: inverted).
 * Checked in Brain Drain with RECOMP_INPUT_SEQ's mouse steps: 1000 counts
 * right turned 55.00 degrees, 300 up raised the view 16.50 to the ceiling. */
#include "recomp_mouse.h"
#include "tsfp_names.h"

extern void sub_0025D520_gen(void);

#define GUEST_U32(va) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(va)))
#define GUEST_F32(va) (*(volatile float *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(va)))
#define GUEST_S8(va)  (*(volatile int8_t *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(va)))

/* Mounted guns and seats, the two move states the walking path above skips.
 *
 * 0x80 (player_look_mounted): the stick turns the *object*, not the player:
 * its data (object+0x7C) holds pitch +0x1C, which the stick y is subtracted
 * from, and yaw +0x24, which the stick x is added to, each kept in its arc
 * by angle_limit; +0x5C scales both when +0x08 has 0x1000000. Objects of
 * type 1 and 9 are not turned at all. 0x40 (player_look_seated): the stick
 * turns Player.yaw/pitch, which are then held inside the object's arcs
 * measured from its heading (object+0x70). Both use the same degrees per
 * count as on foot; the limits are reapplied after the mouse is added, the
 * way the game applies them after the stick. */
#define TSFP_CHARDATA_MOUNT      0xC14u  /* character data -> object used */
#define TSFP_OBJECT_DATA         0x07Cu
#define TSFP_OBJECT_HEADING      0x070u
#define TSFP_MOUNT_FLAGS         0x008u
#define TSFP_MOUNT_TYPE          0x010u
#define TSFP_MOUNT_PITCH         0x01Cu
#define TSFP_MOUNT_YAW           0x024u
#define TSFP_MOUNT_PITCH_LO      0x034u  /* lo == hi: the axis is fixed */
#define TSFP_MOUNT_PITCH_HI      0x038u
#define TSFP_MOUNT_YAW_LO        0x03Cu
#define TSFP_MOUNT_YAW_HI        0x040u
#define TSFP_MOUNT_SENSITIVITY   0x05Cu

static float tsfp_wrap180(float a)
{
    return a - floorf((a + 180.0f) / 360.0f) * 360.0f;
}

/* angle_limit (0x00153F90), for the lo != hi case its callers use it in. */
static float tsfp_angle_limit(float a, float lo, float hi)
{
    if (fabsf(hi - lo) >= 360.0f)
        return tsfp_wrap180(a);
    if (a < lo)
        return lo;
    if (a > hi)
        return hi;
    return a;
}

static void tsfp_mouse_look_mounted(uint32_t state, uint32_t mount,
                                    float dyaw, float dpitch)
{
    uint32_t d = GUEST_U32(mount + TSFP_OBJECT_DATA);
    float lo, hi;

    if (!d)
        return;
    if (state == 0x80) {
        uint32_t type = GUEST_U32(d + TSFP_MOUNT_TYPE);
        float yaw, pitch;

        if (type == 1 || type == 9)
            return;
        if (GUEST_U32(d + TSFP_MOUNT_FLAGS) & 0x1000000u) {
            dyaw *= GUEST_F32(d + TSFP_MOUNT_SENSITIVITY);
            dpitch *= GUEST_F32(d + TSFP_MOUNT_SENSITIVITY);
        }
        /* The object's pitch runs the other way to the player's. */
        pitch = GUEST_F32(d + TSFP_MOUNT_PITCH) + dpitch;
        yaw = GUEST_F32(d + TSFP_MOUNT_YAW) + dyaw;
        lo = GUEST_F32(d + TSFP_MOUNT_PITCH_LO);
        hi = GUEST_F32(d + TSFP_MOUNT_PITCH_HI);
        pitch = lo == hi ? lo : tsfp_angle_limit(pitch, lo, hi);
        lo = GUEST_F32(d + TSFP_MOUNT_YAW_LO);
        hi = GUEST_F32(d + TSFP_MOUNT_YAW_HI);
        yaw = lo == hi ? lo : tsfp_angle_limit(yaw, lo, hi);
        GUEST_F32(d + TSFP_MOUNT_PITCH) = pitch;
        GUEST_F32(d + TSFP_MOUNT_YAW) = yaw;
    } else {
        uint32_t p = GUEST_U32(TSFP_G_PLAYER);
        float heading = GUEST_F32(mount + TSFP_OBJECT_HEADING);
        float yaw = tsfp_wrap180(GUEST_F32(p + TSFP_PLAYER_YAW) + dyaw - heading);
        float pitch = tsfp_wrap180(GUEST_F32(p + TSFP_PLAYER_PITCH) - dpitch);

        lo = GUEST_F32(d + TSFP_MOUNT_PITCH_LO);
        hi = GUEST_F32(d + TSFP_MOUNT_PITCH_HI);
        if (lo != hi)
            pitch = tsfp_angle_limit(pitch, lo, hi);
        lo = GUEST_F32(d + TSFP_MOUNT_YAW_LO);
        hi = GUEST_F32(d + TSFP_MOUNT_YAW_HI);
        if (lo != hi)
            yaw = tsfp_angle_limit(yaw, lo, hi);
        GUEST_F32(p + TSFP_PLAYER_YAW) = tsfp_wrap180(yaw + heading);
        GUEST_F32(p + TSFP_PLAYER_PITCH) = pitch;
    }
}

static void tsfp_mouse_look(void)
{
    uint32_t p, cam, state, mode;
    long dx, dy;
    float scale = 1.0f, yaw, pitch, limit;
    int control;

    /* Taken every frame the look runs, used or not: what arrives while the
     * player has no control (a cutscene, a death) is dropped, not saved up. */
    if (!recomp_mouse_take_motion(&dx, &dy))
        return;
    p = GUEST_U32(TSFP_G_PLAYER);
    if (!p || GUEST_U32(p + TSFP_PLAYER_PAD_INDEX) != 0)
        return;                                 /* the mouse is player 1's */
    state = GUEST_U32(p + TSFP_PLAYER_MOVE_STATE);
    if (state == 0x20 || state == 0x100)
        return;                                 /* states that do not look */
    cam = GUEST_U32(TSFP_G_CAMERA);
    mode = cam ? GUEST_U32(cam + TSFP_CAMERA_MODE) : 0;
    control = GUEST_S8(p + TSFP_PLAYER_FLAGS) < 0 ||
              (mode && GUEST_U32(mode) == 0x18 && state != 0x400 && state != 0x200);
    if (!control)
        return;
    if (cam) {
        float def = (GUEST_U32(cam + TSFP_CAMERA_MODE_A) == 0 && GUEST_U32(cam + TSFP_CAMERA_MODE_B) == 1)
                    ? GUEST_F32(TSFP_G_FOV_DEFAULT_ALT) : GUEST_F32(TSFP_G_FOV_DEFAULT);
        float fov = GUEST_F32(cam + TSFP_CAMERA_FOV);
        if (def > 1.0f && fov > 1.0f && fov <= def)
            scale = fov / def;
    }

    if (state == 0x40 || state == 0x80) {
        uint32_t ch = GUEST_U32(p + TSFP_PLAYER_CHARACTER);
        uint32_t cd = ch ? GUEST_U32(ch + TSFP_OBJECT_DATA) : 0;
        uint32_t mount = cd ? GUEST_U32(cd + TSFP_CHARDATA_MOUNT) : 0;
        if (mount)
            tsfp_mouse_look_mounted(state, mount,
                -(float)recomp_mouse_counts_to_degrees(dx) * scale,
                (float)recomp_mouse_counts_to_degrees(dy) * scale);
        return;
    }

    yaw =GUEST_F32(p + TSFP_PLAYER_YAW) - (float)recomp_mouse_counts_to_degrees(dx) * scale;
    while (yaw < 0.0f)
        yaw += 360.0f;
    while (yaw >= 360.0f)
        yaw -= 360.0f;
    limit = GUEST_F32(TSFP_G_PITCH_LIMIT);
    pitch = GUEST_F32(p + TSFP_PLAYER_PITCH) - (float)recomp_mouse_counts_to_degrees(dy) * scale;
    if (pitch > limit)
        pitch = limit;
    if (pitch < -limit)
        pitch = -limit;
    GUEST_F32(p + TSFP_PLAYER_YAW) = yaw;
    GUEST_F32(p + TSFP_PLAYER_PITCH) = pitch;
}

void sub_0025D520(void)
{
    sub_0025D520_gen();
    tsfp_mouse_look();
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /*
     * Rate-limited per target, reporting at 1, 10, 100 ...
     *
     * An unresolved target inside a loop is the normal case, not the rare
     * one: Mortal Kombat: Deadly Alliance produced 46843 of these for a
     * single address in one 30-second run, seventeen lines each. That buries
     * every other diagnostic in the log and says nothing the first report did
     * not. The progression is the useful part -- one line says a target was
     * never identified, a run of them says the title is stuck on it.
     */
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }

    RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] unresolved %starget 0x%08X -- %llu time(s) "
                    "(total calls: %llu)\n",
            g_icall_dispatch_form == 1 ? "call " :
            g_icall_dispatch_form == 2 ? "jump " : "",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    if (g_icall_dispatch_form == 2 && hits[i] == 1)
        fprintf(stderr, "  a jump, not a call: if this address is inside a "
                        "function rather than at its start, the guest is doing "
                        "its own control flow (a coroutine, a longjmp, or a "
                        "switch arm) and seeding it as a function will not "
                        "help\n");

    /*
     * Who called it. The lifted caller pushed its return address on the guest
     * stack, so code-range values above esp name the chain -- the same scan
     * the memory watchpoints use. It is a heuristic: stale return addresses
     * from earlier frames show up too, and the first entry is the reliable
     * one. It is still the difference between an address with no context and
     * a function to open, which is what an unresolved target needs.
     */
    {
        const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                             + g_icall_saved_esp;
        int shown = 0;
        fprintf(stderr, "  callers:");
        for (i = 0; g_xbox_mem_offset && g_icall_saved_esp
                    && i < 160 && shown < 6; i++) {
            uint32_t v;
            memcpy(&v, stack + (size_t)i * 4, sizeof v);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, " (none on the stack)");
        fprintf(stderr, "\n");
    }

    /* The recent-target ring buffer, once per address. What ran just before
     * an unresolved call is context for the first report and noise after. */
    if (hits[i] == 1) {
        fprintf(stderr, "  Recent ICALL targets:\n");
        for (i = 0; i < 16; i++) {
            int idx = (g_icall_trace_idx - 16 + i) & 15;
            if (g_icall_trace[idx])
                fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}
/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va, uint32_t saved_esp)
{
    /* A power of ten, because the rate limiter above only reaches this
     * function body at 1, 10, 100 ... and the verdict has to land on one of
     * those. A hundred thousand skips of one target is unambiguous: no title
     * makes progress through that. */
    enum { SLOTS = 16, SPIN_VERDICT = 100000 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    static int said_it;   /* the verdict below is said once, not once per target */
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    {
        /* The call site, read off the guest stack. Without it the log
         * says a wild pointer was skipped but not by whom, and the
         * caller is the only thing that leads anywhere.
         *
         * It comes from the esp the dispatch macro captured, not from
         * g_esp. The lifted call site pushes its return address onto the
         * *local* esp and only syncs g_esp at certain points, so g_esp is
         * stale here -- it read 0 exactly when a null target most needed
         * explaining, which sent three rounds of Jet Set Radio Future
         * chasing inferences instead of a call site. saved_esp is the value
         * before that push, so the return address is the dword below it.
         *
         * Only for the call form. A tail jump pushes no return address, so
         * that dword is whatever the frame happened to leave there: Tony
         * Hawk's Pro Skater 2X reported "from 0x00000008" and then "from
         * 0x00000004" for the same million skipped calls, and both are
         * plainly not code. Printing a number that cannot be an address as
         * though it were the caller is worse than printing nothing, because
         * it is the one field a reader goes to next.
         *
         * So: name the form, and where the top of the stack cannot answer,
         * scan for code-range values the way the unresolved-target logger
         * above does. Stale return addresses from earlier frames come back
         * too and the first is the reliable one, but a list of real code
         * addresses is a place to start and 0x00000008 is not. */
        uint32_t caller = 0;
        if (g_icall_dispatch_form != 2 && saved_esp >= 4 && g_xbox_mem_offset)
            caller = *(const uint32_t *)((const uint8_t *)g_xbox_mem_offset
                                         + (saved_esp - 4));
        if (caller < g_xbox_code_lo || caller >= g_xbox_code_hi)
            caller = 0;
        RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped "
                        "%llu time(s) via a %s",
                va, (unsigned long long)hits[i],
                g_icall_dispatch_form == 2 ? "jump" : "call");
        if (caller)
            fprintf(stderr, " from 0x%08X", caller);
        fprintf(stderr, " (null or wild function pointer, at call #%llu)\n",
                (unsigned long long)g_icall_count);

        /* The stack scan, once per address: the same heuristic the
         * unresolved-target logger uses, and the only thing that names a
         * caller when the return address is not where it should be. */
        if (hits[i] == 1) {
            const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                                 + saved_esp;
            int shown = 0, k;
            fprintf(stderr, "  callers:");
            for (k = 0; g_xbox_mem_offset && saved_esp
                        && k < 160 && shown < 6; k++) {
                uint32_t v;
                memcpy(&v, stack + (size_t)k * 4, sizeof v);
                if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                    fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                    shown++;
                }
            }
            if (!shown)
                fprintf(stderr, " (none on the stack)");
            fprintf(stderr, "\n");
        }

        /* RECOMP_ICALL_FATAL=1: fault here instead of skipping, so the crash
         * handler prints the guest call stack that reached this call.
         *
         * The caller printed above is read from the top of the guest stack,
         * which is right only when the lifted call site pushed a return
         * address there and nothing has since moved esp. When it reads 0 --
         * exactly the case where a null target most needs explaining -- the
         * line says a wild pointer was skipped and nothing about by whom. A
         * deliberate fault costs the run and buys the backtrace. */
        {
            static int fatal = -1;
            if (fatal < 0) {
                const char *v = getenv("RECOMP_ICALL_FATAL");
                fatal = v && *v && strcmp(v, "0") != 0;
            }
            if (fatal) {
                fprintf(stderr, "[ICALL] RECOMP_ICALL_FATAL: faulting here for "
                                "a backtrace\n");
                fflush(stderr);
                *(volatile int *)0 = 1;
            }
        }
    }

    /* Past a certain count this stops being a warning and becomes a verdict.
     *
     * A skipped call sets eax to 0 and returns. Zero is a fine answer for a
     * null function pointer in most code -- it reads as NULL, false, or
     * nothing -- but it is also S_OK, so in COM-shaped code a loop that
     * repeats while the result is non-negative can never leave. Panzer
     * Dragoon Orta hung exactly there, three billion skips in forty seconds,
     * with no frame ever presented.
     *
     * The value is deliberately left alone: there is no return value that is
     * right for both conventions, and guessing failure would break the
     * callers for which zero is correct. What can be fixed is the silence.
     * Say plainly that the title is hung, once, and say what to do about it.
     */
    if (hits[i] == SPIN_VERDICT && !said_it) {
        said_it = 1;
        fprintf(stderr,
            "[ICALL] ^^ this is a hang, not slow progress. One target has been\n"
            "        skipped %d times. The skip returns eax = 0, which is also\n"
            "        S_OK, so a loop testing for a non-negative result will\n"
            "        never exit. Try `py -3 -m tools.seed_from_log <log>` to\n"
            "        recover the target as a function, and read\n"
            "        docs/technical/memory-watchpoints.md for who wrote the\n"
            "        pointer. Set RECOMP_ICALL_SPIN_FATAL=1 to stop here\n"
            "        instead of spinning.\n", SPIN_VERDICT);
        fflush(stderr);
        if (getenv("RECOMP_ICALL_SPIN_FATAL")) {
            fprintf(stderr, "[ICALL] RECOMP_ICALL_SPIN_FATAL is set; exiting.\n");
            fflush(stderr);
            exit(3);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}

