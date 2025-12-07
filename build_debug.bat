@echo off
REM Debug build with symbols
setlocal

if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else (
    echo Error: Visual Studio not found.
    exit /b 1
)

if not exist "bin" mkdir bin

echo Building Debug version...

cl.exe /nologo /Od /Zi /MDd ^
    /W4 ^
    /D "DEBUG" /D "_DEBUG" /D "WIN32" /D "_WINDOWS" ^
    /Fe"bin\lwsr_debug.exe" ^
    /Fd"bin\lwsr_debug.pdb" ^
    src\main.c src\config.c src\capture.c src\encoder.c src\overlay.c ^
    /link /DEBUG /SUBSYSTEM:WINDOWS ^
    user32.lib gdi32.lib d3d11.lib dxgi.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib shell32.lib comdlg32.lib comctl32.lib dwmapi.lib

if %ERRORLEVEL% neq 0 (
    echo Build failed!
    exit /b 1
)

del *.obj >nul 2>&1

echo Debug build complete: bin\lwsr_debug.exe
endlocal
