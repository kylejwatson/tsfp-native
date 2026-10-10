@echo off
rem play.bat [route]
rem   No route: just start the game.
rem   seqs\<route>.seq exists: drive the menus with it (RECOMP_INPUT_SEQ), then play.
rem   recordings\<route>.rec exists: replay that recording (RECOMP_INPUT_REPLAY);
rem   the pad and mouse are yours again when it ends.
setlocal
set "HERE=%~dp0"
if not "%~1"=="" (
  if exist "%HERE%seqs\%~1.seq" (
    set "RECOMP_INPUT_SEQ=@%HERE%seqs\%~1.seq"
  ) else if exist "%HERE%recordings\%~1.rec" (
    set "RECOMP_INPUT_REPLAY=%HERE%recordings\%~1.rec"
  ) else (
    echo No seqs\%~1.seq or recordings\%~1.rec
    exit /b 1
  )
)
start "" "%HERE%build\futureperfect_recomp.exe"
