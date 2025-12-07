# Quick Record

Ultra-lightweight screen recorder for Windows. ~30KB executable, no dependencies.

## Features

- **Capture modes**: Area, Window, Monitor, All Monitors
- **Output formats**: MP4 (H.264), AVI, WMV
- **Quality presets**: Low, Medium, High, Lossless
- **Hardware accelerated** via DXGI Desktop Duplication
- **Macro key support**: Run again to stop recording (Stream Deck compatible)

## Build

Requires Visual Studio Build Tools (MSVC).

```batch
build.bat
```

Output: `bin\lwsr.exe`

## Usage

1. Run `lwsr.exe`
2. Select capture mode
3. Click record or draw selection area
4. Run `lwsr.exe` again or press ESC to stop

## License

MIT
