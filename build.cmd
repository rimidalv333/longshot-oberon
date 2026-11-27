@echo off
rem Longshot Oberon: the Windows builder (no WSL). Builds the PS5 title folder PPSA93339 from your own game file, in this
rem folder, the same as build.sh does on Linux/WSL, with the same eboot.bin from the same release zip. Nothing to install: the
rem Windows tools (LLVM 18.1.8, Python 3.12 with the build's packages, a64emit.dll, zbic.dll, ps5-native-tool.exe) come in
rem deps\windows\, the rest of what the build needs in deps\, as for build.sh.
rem
rem   build.cmd [your oot.nup] [--jobs N] [--out DIR] [--skip-hash-check]
rem
rem Or put your oot.nup in this folder, beside build.cmd, and double-click it: the .nup beside it is used (no path to set
rem anywhere), and the options are the ones in build_args.txt (one line, e.g. --jobs 3) if it is there.
rem A step that is done is not done again: after an interrupted run, run it again. The whole output is also in build.log.
setlocal EnableExtensions
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set "TOOLS=%HERE%\deps\windows"
set "PY=%TOOLS%\python\python.exe"
cd /d "%HERE%"
if not exist "%HERE%\ps5\win_build.py" (
    echo build: ps5\win_build.py is missing: run build.cmd from the unzipped release folder
    goto :fail
)
if not exist "%PY%" (
    echo build: the Windows tools are missing ^(deps\windows\python\python.exe^): build from the release zip, which carries
    echo them in deps\windows\, not from a git checkout ^(deps\ is not in git^)
    goto :fail
)

rem ---- the settings this release is built and tested with (build.sh sets the same) ----
if not defined RX_FP_NATIVE set "RX_FP_NATIVE=1"
if not defined PS5_PGO set "PS5_PGO=%HERE%\ps5\profiles\vk.profdata"

rem ---- the game file ----
set "GAME="
set "REST="
if not "%~1"=="" if /i not "%~x1"=="" (
    set "GAME=%~f1"
    shift
)
:args
if "%~1"=="" goto :argsdone
set "REST=%REST% %1"
shift
goto :args
:argsdone
if not defined REST if exist "%HERE%\build_args.txt" set /p REST=<"%HERE%\build_args.txt"
set "NUPS=0"
if not defined GAME for %%f in ("%HERE%\*.nup") do (
    set "GAME=%%~ff"
    set /a NUPS+=1
)
if %NUPS% GTR 1 (
    echo build: more than one .nup beside build.cmd: keep one there, or drag the one to use onto build.cmd
    goto :fail
)
if not defined GAME (
    echo build: no game file: put your oot.nup beside build.cmd ^(this folder^), or drag it onto build.cmd
    goto :fail
)

"%PY%" -I -u "%HERE%\ps5\win_build.py" "%GAME%" --tools "%TOOLS%" --log "%HERE%\build.log" %REST%
if errorlevel 1 goto :fail
echo.
echo Done. The title folder is %HERE%\dist\PPSA93339 ^(build.log has the whole output^).
echo build finished > "%HERE%\build.status"
pause
exit /b 0

:fail
echo.
echo The build stopped. build.log beside build.cmd has the whole output.
echo build failed > "%HERE%\build.status"
pause
exit /b 1
