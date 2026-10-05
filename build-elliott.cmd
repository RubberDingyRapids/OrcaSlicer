@echo off
REM Build driver for this fork on Windows using the portable tools in
REM C:\dev\tools (no admin needed) and Visual Studio 2022 Build Tools.
REM See BUILD-NOTES-ELLIOTT.md. Paths are absolute on purpose: the compiled
REM dependencies attached to the GitHub release embed C:\dev\OrcaSlicer.
REM
REM   build-elliott.cmd deps     build the dependencies (1-2 hours, rarely needed)
REM   build-elliott.cmd slicer   build OrcaSlicer and install into build\OrcaSlicer
REM   build-elliott.cmd all      both
REM
REM Output: C:\dev\OrcaSlicer\build\OrcaSlicer\orca-slicer.exe (portable layout)

setlocal
set "TOOLS=C:\dev\tools"
REM cmake first so build_win.bat does not pick up the one bundled with Strawberry Perl.
set "PATH=%TOOLS%\cmake-4.4.4-windows-x86_64\bin;%TOOLS%\ninja;%TOOLS%\perl\perl\site\bin;%TOOLS%\perl\perl\bin;%PATH%;%TOOLS%\perl\c\bin"

REM Some shells set this, which stops cmd finding scripts in the current directory.
set NoDefaultCurrentDirectoryInExePath=

REM The bundled CPython (deps/python3) needs a host Python >= 3.10 to bootstrap its
REM build (deps only). Point this at any python.exe on the machine.
if "%HOST_PYTHON%"=="" set "HOST_PYTHON=%LOCALAPPDATA%\Python\pythoncore-3.14-64\python.exe"

cd /d C:\dev\OrcaSlicer || exit /b 1

set "what=%~1"
if "%what%"=="" set "what=slicer"

if /i "%what%"=="deps" goto :deps
if /i "%what%"=="slicer" goto :slicer
if /i "%what%"=="all" goto :deps
echo Unknown action "%what%". Use deps, slicer or all.
exit /b 2

:deps
echo === [%date% %time%] Building dependencies ===
call "C:\dev\OrcaSlicer\build_win.bat" -d --msvc --msbuild --deps-dir C:\dev\OrcaSlicer\deps\build
if errorlevel 1 (echo DEPS BUILD FAILED & exit /b 1)
if /i "%what%"=="deps" goto :done

:slicer
echo === [%date% %time%] Building OrcaSlicer ===
call "C:\dev\OrcaSlicer\build_win.bat" -s -i --msvc --msbuild --deps-dir C:\dev\OrcaSlicer\deps\build --build-dir C:\dev\OrcaSlicer\build
if errorlevel 1 (echo SLICER BUILD FAILED & exit /b 1)

:done
echo === [%date% %time%] Done ===
exit /b 0
