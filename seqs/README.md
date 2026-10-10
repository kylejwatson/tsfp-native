Input routes for `play.bat <route>` and `RECOMP_INPUT_SEQ=@seqs/<route>.seq`.

A `.seq` is a timed script (see `RECOMP_INPUT_SEQ` in
`xboxrecomp/src/hle/input_host.c`): `<ms>:<control>[+<control>...][:<hold ms>]`,
one or more per line, `#` comments. Controls: `a b x y black white lt rt start
back up down left right`, `lstick_*`/`rstick_*` (`_up _down _left _right`), and
`mouse_up/down/left/right` (1000 counts a second while held). The clock starts
at the title's first pad read. The scripts assume the "Player 1" profile at
index 1.

Recordings (`record.bat <name>` -> `recordings/<name>.rec`, ignored by git) are
what you actually pressed, replayed exactly with `play.bat <name>`; the pad
comes back to you when the recording ends.
