@echo off
REM Ultra Lightweight Screen Recorder - Build Script
REM Usage: build.bat [debug|release]
REM   build.bat        - Release build (optimized)
REM   build.bat debug  - Debug build (symbols, no optimization)
REM   build.bat release - Release build (explicit)

setlocal enabledelayedexpansion

REM Check for debug flag
set BUILD_TYPE=release
if /i "%1"=="debug" set BUILD_TYPE=debug

REM Check if MSVC is already in PATH (e.g., from GitHub Actions ilammy/msvc-dev-cmd)
where cl.exe >nul 2>&1
if %ERRORLEVEL%==0 (
    echo MSVC already in PATH, skipping vcvars setup
    goto :build
)

REM Find Visual Studio
if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else (
    echo.
    echo Error: Visual Studio Build Tools not found.
    echo.
    set /p INSTALL_VS="Would you like to install VS Build Tools via winget? [Y/n]: "
    if /i "!INSTALL_VS!"=="n" (
        echo.
        echo To install manually, run:
        echo   winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --quiet"
        echo.
        exit /b 1
    )
    echo.
    echo Installing Visual Studio 2022 Build Tools...
    winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --quiet"
    if %ERRORLEVEL% neq 0 (
        echo.
        echo Installation failed. Please install manually from:
        echo   https://visualstudio.microsoft.com/visual-cpp-build-tools/
        exit /b 1
    )
    echo.
    echo Build Tools installed. Please restart your terminal and run build.bat again.
    exit /b 0
)

:build

REM Create output directory
if not exist "bin" mkdir bin

REM Compile resource file (icon)
rc.exe /nologo /fo "bin\lwsr.res" "src\lwsr.rc"

REM Source files (single source of truth)
set SOURCES=src\main.c src\config.c src\capture.c src\encoder.c src\overlay.c src\action_toolbar.c src\border.c src\replay_buffer.c src\nvenc_encoder.c src\sample_buffer.c src\mp4_muxer.c src\util.c src\logger.c src\audio_device.c src\audio_capture.c src\aac_encoder.c src\gpu_converter.c

REM Resource file
set RESOURCES=bin\lwsr.res

REM Libraries
set LIBS=user32.lib gdi32.lib d3d11.lib dxgi.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib shell32.lib comdlg32.lib comctl32.lib dwmapi.lib winmm.lib propsys.lib oleaut32.lib strmiids.lib

if "%BUILD_TYPE%"=="debug" (
    echo Building Lightweight Screen Recorder [DEBUG]...
    cl.exe /nologo /Od /Zi /MDd ^
        /W4 ^
        /D "DEBUG" /D "_DEBUG" /D "WIN32" /D "_WINDOWS" /D "_CRT_SECURE_NO_WARNINGS" /D "NVENCAPI=__stdcall" ^
        /Fe"bin\lwsr.exe" ^
        /Fd"bin\lwsr.pdb" ^
        %SOURCES% ^
        /link /DEBUG /SUBSYSTEM:WINDOWS ^
        %LIBS% %RESOURCES%
) else (
    echo Building Lightweight Screen Recorder [RELEASE]...
    cl.exe /nologo /O2 /GL /GS- /MD ^
        /W3 ^
        /D "NDEBUG" /D "WIN32" /D "_WINDOWS" /D "_CRT_SECURE_NO_WARNINGS" /D "NVENCAPI=__stdcall" ^
        /Fe"bin\lwsr.exe" ^
        %SOURCES% ^
        /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF ^
        %LIBS% %RESOURCES%
)

if %ERRORLEVEL% neq 0 (
    echo Build failed!
    exit /b 1
)

REM Clean up intermediate files
del *.obj >nul 2>&1
del bin\lwsr.res >nul 2>&1

echo.
echo Build successful! Output: bin\lwsr.exe [%BUILD_TYPE%]
echo.
echo Usage:
echo   - Run lwsr.exe to start recording
echo   - Run lwsr.exe again (or press your macro key) to stop recording
echo   - ESC to cancel selection
echo.

endlocal
