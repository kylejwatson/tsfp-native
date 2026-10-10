@echo off
rem record.bat <name> [route]
rem   Plays the game and writes everything player 1 presses (pad, keys, mouse)
rem   to recordings\<name>.rec. Get to where you want to be, then close the
rem   window; afterwards `play.bat <name>` replays it. With a route, that route
rem   drives the start (and is part of the recording).
setlocal
set "HERE=%~dp0"
if "%~1"=="" (
  echo usage: record.bat ^<name^> [route]
  exit /b 1
)
if not exist "%HERE%recordings" mkdir "%HERE%recordings"
set "RECOMP_INPUT_RECORD=%HERE%recordings\%~1.rec"
if not "%~2"=="" if exist "%HERE%seqs\%~2.seq" set "RECOMP_INPUT_SEQ=@%HERE%seqs\%~2.seq"
start "" "%HERE%build\futureperfect_recomp.exe"
