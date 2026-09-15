@REM OrcaSlicer build script for Windows with VS auto-detect
@echo off
set WP=%CD%
set _START_TIME=%TIME%

@REM Default target architecture to the host CPU arch; override by passing
@REM "x64" or "arm64" as an argument. PROCESSOR_ARCHITEW6432 covers a 32-bit
@REM shell running on a 64-bit OS, where PROCESSOR_ARCHITECTURE reads "x86".
set arch=x64
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" set arch=ARM64
if /I "%PROCESSOR_ARCHITEW6432%"=="ARM64" set arch=ARM64
if /I "%1"=="arm64" set arch=ARM64
if /I "%2"=="arm64" set arch=ARM64
if /I "%1"=="x64" set arch=x64
if /I "%2"=="x64" set arch=x64

@REM Check for Ninja Multi-Config option (-x)
set USE_NINJA=0
for %%a in (%*) do (
    if "%%a"=="-x" set USE_NINJA=1
)

@REM Check for clang-cl option (-l). Combined with -x it also builds the deps with
@REM clang-cl; on the Visual Studio generator it applies to the slicer only, because
@REM the dependency sub-builds have no toolset to inherit and stay on MSVC.
set CLANG_ARG=
set TOOLSET_ARG=
for %%a in (%*) do (
    if "%%a"=="-l" (
        set CLANG_ARG=-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
        set TOOLSET_ARG=-T ClangCL
    )
)

@REM Check for unit-tests option ("tests")
set BUILD_TESTS=OFF
for %%a in (%*) do (
    if /I "%%a"=="tests" set BUILD_TESTS=ON
)

if "%USE_NINJA%"=="1" (
    echo Using Ninja Multi-Config generator
    set CMAKE_GENERATOR="Ninja Multi-Config"
    set VS_VERSION=Ninja
    goto :generator_ready
)

@REM Detect Visual Studio version using msbuild
echo Detecting Visual Studio version using msbuild...

@REM Try to get MSBuild version - the output format varies by VS version
set VS_MAJOR=
for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
    for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
    set MSBUILD_OUTPUT=%%i
    goto :version_found
)

@REM Alternative method for newer MSBuild versions
if "%VS_MAJOR%"=="" (
    for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
        for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
        set MSBUILD_OUTPUT=%%i
        goto :version_found
    )
)

:version_found
echo MSBuild version detected: %MSBUILD_OUTPUT%
echo Major version: %VS_MAJOR%

if "%VS_MAJOR%"=="" (
    echo Error: Could not determine Visual Studio version from msbuild
    echo Please ensure Visual Studio and MSBuild are properly installed
    exit /b 1
)

if "%VS_MAJOR%"=="16" (
    set VS_VERSION=2019
    set CMAKE_GENERATOR="Visual Studio 16 2019"
) else if "%VS_MAJOR%"=="17" (
    set VS_VERSION=2022
    set CMAKE_GENERATOR="Visual Studio 17 2022"
) else if "%VS_MAJOR%"=="18" (
    set VS_VERSION=2026
    set CMAKE_GENERATOR="Visual Studio 18 2026"
) else (
    echo Error: Unsupported Visual Studio version: %VS_MAJOR%
    echo Supported versions: VS2019 (16.8+^), VS2022 (17.x^), VS2026 (18.x^)
    exit /b 1
)

echo Detected Visual Studio %VS_VERSION% (version %VS_MAJOR%)
echo Using CMake generator: %CMAKE_GENERATOR%

:generator_ready

@REM Pack deps
if "%1"=="pack" (
    setlocal ENABLEDELAYEDEXPANSION
    cd %WP%/deps/build
    if "%arch%"=="ARM64" cd %WP%/deps/build-arm64
    for /f "tokens=2-4 delims=/ " %%a in ('date /t') do set build_date=%%c%%b%%a
    echo packing deps: OrcaSlicer_dep_win-!arch!_!build_date!_vs!VS_VERSION!.zip

    %WP%/tools/7z.exe a OrcaSlicer_dep_win-!arch!_!build_date!_vs!VS_VERSION!.zip OrcaSlicer_dep
    goto :done
)

set debug=OFF
set debuginfo=OFF
if "%1"=="debug" set debug=ON
if "%2"=="debug" set debug=ON
if "%1"=="debuginfo" set debuginfo=ON
if "%2"=="debuginfo" set debuginfo=ON
if "%debug%"=="ON" (
    set build_type=Debug
    set build_dir=build-dbg
) else (
    if "%debuginfo%"=="ON" (
        set build_type=RelWithDebInfo
        set build_dir=build-dbginfo
    ) else (
        set build_type=Release
        set build_dir=build
    )
)
if "%arch%"=="ARM64" set build_dir=%build_dir%-arm64
echo build type set to %build_type%, arch=%arch%

setlocal DISABLEDELAYEDEXPANSION
cd deps
mkdir %build_dir%
cd %build_dir%
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"

if "%1"=="slicer" (
    GOTO :slicer
)
echo "building deps.."
if defined CLANG_ARG if "%USE_NINJA%"=="0" echo Note: -l needs -x for the dependencies; building them with MSVC.

echo on
REM Set minimum CMake policy to avoid <3.5 errors
set CMAKE_POLICY_VERSION_MINIMUM=3.5
if "%USE_NINJA%"=="1" (
    cmake ../ -G %CMAKE_GENERATOR% %CLANG_ARG% -DCMAKE_BUILD_TYPE=%build_type%
    cmake --build . --config %build_type% --target deps
) else (
    cmake ../ -G %CMAKE_GENERATOR% -A %arch% -DCMAKE_BUILD_TYPE=%build_type%
    cmake --build . --config %build_type% --target deps -- -m
)
@echo off

if "%1"=="deps" goto :done

:slicer
call :check_linux_bridge_runtime_inputs
if errorlevel 1 exit /b 1

echo "building Orca Slicer..."
cd %WP%
mkdir %build_dir%
cd %build_dir%

echo on
set CMAKE_POLICY_VERSION_MINIMUM=3.5
if "%USE_NINJA%"=="1" (
    cmake .. -G %CMAKE_GENERATOR% %CLANG_ARG% -DORCA_TOOLS=ON %SIG_FLAG% -DBUILD_TESTS=%BUILD_TESTS% -DCMAKE_BUILD_TYPE=%build_type%
    cmake --build . --config %build_type% --target all
) else (
    cmake .. -G %CMAKE_GENERATOR% -A %arch% %TOOLSET_ARG% -DORCA_TOOLS=ON %SIG_FLAG% -DBUILD_TESTS=%BUILD_TESTS% -DCMAKE_BUILD_TYPE=%build_type%
    cmake --build . --config %build_type% --target ALL_BUILD -- -m
)
@echo off
cd ..
call scripts/run_gettext.bat
cd %build_dir%
cmake --build . --target install --config %build_type%
if errorlevel 1 exit /b 1
call :copy_linux_bridge_runtime
if errorlevel 1 exit /b 1

:done
@echo off
for /f "tokens=1-3 delims=:.," %%a in ("%_START_TIME: =0%") do set /a "_start_s=%%a*3600+%%b*60+%%c"
for /f "tokens=1-3 delims=:.," %%a in ("%TIME: =0%") do set /a "_end_s=%%a*3600+%%b*60+%%c"
set /a "_elapsed=_end_s - _start_s"
if %_elapsed% lss 0 set /a "_elapsed+=86400"
set /a "_hours=_elapsed / 3600"
set /a "_remainder=_elapsed - _hours * 3600"
set /a "_mins=_remainder / 60"
set /a "_secs=_remainder - _mins * 60"
echo.
echo Build completed in %_hours%h %_mins%m %_secs%s
exit /b 0

:resolve_rootfs_tar
if defined PJARCZAK_ROOTFS_TAR exit /b 0
if defined PJARCZAK_WSL_ROOTFS_TAR (
    if exist "%PJARCZAK_WSL_ROOTFS_TAR%" (
        set "PJARCZAK_ROOTFS_TAR=%PJARCZAK_WSL_ROOTFS_TAR%"
        exit /b 0
    )
    echo Missing file from PJARCZAK_WSL_ROOTFS_TAR: %PJARCZAK_WSL_ROOTFS_TAR%
    exit /b 1
)
if exist "%WP%\tools\pjarczak_bambu_runtime\rootfs\windows-wsl2-rootfs.tar" (
    set "PJARCZAK_ROOTFS_TAR=%WP%\tools\pjarczak_bambu_runtime\rootfs\windows-wsl2-rootfs.tar"
    exit /b 0
)
if exist "%WP%\tools\pjarczak_bambu_runtime\windows-wsl2-rootfs.tar" (
    set "PJARCZAK_ROOTFS_TAR=%WP%\tools\pjarczak_bambu_runtime\windows-wsl2-rootfs.tar"
    exit /b 0
)
echo Missing windows-wsl2-rootfs.tar under tools\pjarczak_bambu_runtime (or set PJARCZAK_WSL_ROOTFS_TAR).
exit /b 1

:check_linux_bridge_runtime_inputs
set "HOST_RUNTIME_DIR=%WP%\tools\pjarczak_bambu_linux_host\runtime\linux-x86_64"
if not exist "%HOST_RUNTIME_DIR%\pjarczak_bambu_linux_host" (
    echo No Linux host runtime under %HOST_RUNTIME_DIR% - building without the bundled runtime
    set "PJARCZAK_RUNTIME_SKIP=1"
    exit /b 0
)
call :resolve_rootfs_tar
if errorlevel 1 exit /b 1
for %%f in (pjarczak_bambu_linux_host pjarczak_bambu_linux_host_abi1 pjarczak_bambu_linux_host_abi0 ca-certificates.crt slicer_base64.cer) do (
    if not exist "%HOST_RUNTIME_DIR%\%%f" (
        echo Missing host runtime file: %HOST_RUNTIME_DIR%\%%f
        exit /b 1
    )
)
echo Host runtime preflight OK: %HOST_RUNTIME_DIR% / %PJARCZAK_ROOTFS_TAR%
exit /b 0

:copy_linux_bridge_runtime
if defined PJARCZAK_RUNTIME_SKIP exit /b 0
set "INSTALL_DIR=%WP%\%build_dir%\OrcaSlicer"
set "HOST_RUNTIME_DIR=%WP%\tools\pjarczak_bambu_linux_host\runtime\linux-x86_64"
if not defined PJARCZAK_ROOTFS_TAR (
    call :resolve_rootfs_tar
    if errorlevel 1 exit /b 1
)
if not exist "%INSTALL_DIR%" (
    echo Missing install directory: %INSTALL_DIR%
    exit /b 1
)
if not exist "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" (
    if exist "%WP%\%build_dir%\src\%build_type%\pjarczak_bambu_networking_bridge.dll" copy /Y "%WP%\%build_dir%\src\%build_type%\pjarczak_bambu_networking_bridge.dll" "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" >nul
)
if not exist "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" (
    if exist "%WP%\%build_dir%\pjarczak_bambu_networking_bridge.dll" copy /Y "%WP%\%build_dir%\pjarczak_bambu_networking_bridge.dll" "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" >nul
)
if not exist "%INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll" (
    echo Missing bridge DLL in install output: %INSTALL_DIR%\pjarczak_bambu_networking_bridge.dll
    exit /b 1
)
xcopy "%HOST_RUNTIME_DIR%\*" "%INSTALL_DIR%\\" /I /Y >nul
if errorlevel 4 (
    echo Failed to copy host runtime files into %INSTALL_DIR%
    exit /b 1
)
copy /Y "%PJARCZAK_ROOTFS_TAR%" "%INSTALL_DIR%\windows-wsl2-rootfs.tar" >nul
if errorlevel 1 exit /b 1
for %%f in (pjarczak_wsl_run_host.sh install_runtime.ps1 install_runtime.cmd verify_runtime.ps1 verify_runtime.cmd pjarczak_wsl_distro.txt pjarczak_plugin_cache_subdir.txt) do (
    copy /Y "%WP%\tools\pjarczak_bambu_runtime\wsl\%%f" "%INSTALL_DIR%\%%f" >nul
    if errorlevel 1 exit /b 1
)
copy /Y "%WP%\tools\pjarczak_bambu_runtime\release\assemble_windows_runtime_bundle.ps1" "%INSTALL_DIR%\assemble_windows_runtime_bundle.ps1" >nul
if errorlevel 1 exit /b 1
exit /b 0
