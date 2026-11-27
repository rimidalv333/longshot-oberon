@echo off
rem Longshot Oberon: the Windows builder. Builds the PS5 title folder PPSA93339 from your own game file, with no WSL and nothing
rem to install: LLVM 18.1.8, Python 3.12 and the build's helpers come in tools\, the source and the PS5 libraries in the release
rem zip beside this file.
rem
rem   build.cmd [your oot.nup] [--jobs N] [--out DIR] [--skip-hash-check]
rem
rem Or put your oot.nup in this folder and double-click it: the .nup beside build.cmd is used (no path to set anywhere), and
rem the options are the ones in build_args.txt (one line, e.g. --jobs 3) if it is there.
rem A step that is done is not done again: after an interrupted run, run it again. The whole output is also in build.log.
setlocal EnableExtensions
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set "PY=%HERE%\tools\python\python.exe"
set "SRC=%HERE%\longshot_oberon"
cd /d "%HERE%"
if not exist "%PY%" (
    echo build: the tools folder is missing ^(tools\python\python.exe^): unpack the whole builder, not only this file
    goto :fail
)

rem ---- the settings this release is built and tested with (the same as the Linux/WSL build's command line) ----
if not defined RX_FP_NATIVE set "RX_FP_NATIVE=1"
if not defined PS5_PGO set "PS5_PGO=%SRC%\ps5\profiles\vk.profdata"

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

rem ---- the release: unpacked once, beside this file ----
if exist "%SRC%\ps5\win_build.py" goto :unpacked
set "ZIP="
for %%z in ("%HERE%\longshot-oberon-*.zip") do set "ZIP=%%~fz"
if not defined ZIP (
    echo build: no release zip ^(longshot-oberon-*.zip^) beside build.cmd
    goto :fail
)
echo unpacking the release zip ...
"%PY%" -I -c "import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "%ZIP%" "%HERE%"
if errorlevel 1 goto :fail
:unpacked

"%PY%" -I -u "%SRC%\ps5\win_build.py" "%GAME%" --tools "%HERE%\tools" --log "%HERE%\build.log" %REST%
if errorlevel 1 goto :fail
echo.
echo Done. The title folder is %SRC%\dist\PPSA93339 ^(build.log has the whole output^).
echo build finished > "%HERE%\build.status"
pause
exit /b 0

:fail
echo.
echo The build stopped. build.log beside build.cmd has the whole output.
echo build failed > "%HERE%\build.status"
pause
exit /b 1
