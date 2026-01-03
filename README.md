# Quick Record

Ultra-lightweight screen recorder for Windows. ~30KB executable, no dependencies.

## Features

- **Capture modes**: Area, Window, Monitor, All Monitors
- **Output formats**: MP4 (H.264), AVI, WMV
- **Quality presets**: Low, Medium, High, Lossless
- **Hardware accelerated** via DXGI Desktop Duplication
- **Macro key support**: Run again to stop recording (Stream Deck compatible)
- **Instant Replay Buffer** (ShadowPlay-style):
  - Continuously buffers last N seconds in RAM
  - Press hotkey (F4) to save the replay as MP4
  - Configurable duration: 1 second to 20 minutes
  - Configurable FPS: 30, 60, or 120
  - Aspect ratio options: Native, 16:9, 21:9, 4:3, 1:1, etc.
  - RAM-based H.264 encoding (no disk I/O until save)
  - Instant save (<500ms) via passthrough muxing

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
