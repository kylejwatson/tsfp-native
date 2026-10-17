@echo off
rem build_vs.bat         -> build\     the game linked into the executable (development)
rem build_vs.bat module  -> build-mod\ the runtime executable + futureperfect_game.dll,
rem                         the shareable form (make_kit.sh packages it)
set BDIR=build
set MODULE=OFF
if /i "%1"=="module" (set BDIR=build-mod& set MODULE=ON)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cmake -S . -B %BDIR% -G Ninja -DCMAKE_MAKE_PROGRAM="C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_GAME_MODULE=%MODULE% 2>&1 || exit /b 1
cmake --build %BDIR% 2>&1
