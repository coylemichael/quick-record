@echo off
REM Ultra Lightweight Screen Recorder - Build Script
REM Requires Visual Studio Build Tools or full VS installation

setlocal

REM Find Visual Studio
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else (
    echo Error: Visual Studio not found. Please install Visual Studio Build Tools.
    exit /b 1
)

REM Create output directory
if not exist "bin" mkdir bin

REM Compile with optimizations
echo Building Lightweight Screen Recorder...

cl.exe /nologo /O2 /GL /GS- /MD ^
    /W3 ^
    /D "NDEBUG" /D "WIN32" /D "_WINDOWS" /D "_CRT_SECURE_NO_WARNINGS" ^
    /Fe"bin\lwsr.exe" ^
    src\main.c src\config.c src\capture.c src\encoder.c src\overlay.c ^
    /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF ^
    user32.lib gdi32.lib d3d11.lib dxgi.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib shell32.lib comdlg32.lib comctl32.lib dwmapi.lib

if %ERRORLEVEL% neq 0 (
    echo Build failed!
    exit /b 1
)

REM Clean up intermediate files
del *.obj >nul 2>&1

echo.
echo Build successful! Output: bin\lwsr.exe
echo.
echo Usage:
echo   - Run lwsr.exe to start recording
echo   - Run lwsr.exe again (or press your macro key) to stop recording
echo   - ESC to cancel selection
echo.

endlocal
