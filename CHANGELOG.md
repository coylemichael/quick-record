# Changelog

## [1.1.1] - 2026-01-03

### Fixed
- **Video playback speed accuracy** - Videos now play at correct real-time speed
  - Issue: Video content played faster than real-time (e.g., 10 seconds of content in 6 seconds)
  - Root cause: Frame timestamps were based on frame count rather than actual wall-clock time
  - Solution: Each frame now gets a real wall-clock timestamp from capture time
  - Inspired by ReplaySorcery's timestamp approach using `av_gettime_relative()`

- **Buffer duration accuracy** - Replay buffer now contains exactly the configured duration
  - Issue: 15-second buffer was producing 26-second videos
  - Root cause: Eviction used ideal frame durations (16.67ms at 60fps) but actual capture rate was ~34fps
  - Solution: Eviction now uses timestamp difference: `newest_timestamp - oldest_timestamp > max_duration`

- **Frame duration calculation** - Each frame's duration is now the real gap since the previous frame
  - Prevents timing drift in playback
  - Clamped to 25%-400% of ideal duration to handle timing glitches

### Technical Details
- Timestamp chain: Capture → Encoder → Buffer → Muxer all use real wall-clock timestamps
- Timestamps normalized to start at 0 when saving (first frame's timestamp becomes 0)
- Added diagnostic logging showing actual vs target FPS during capture

---

## [1.1.0] - 2026-01-03

### Changed
- **Phase 1-3 Architecture Refactoring** - Modular, maintainable codebase
  - Separated concerns into dedicated modules
  - Improved code organization and single responsibility

---

## [1.0.0] - 2026-01-03

### Added
- **ShadowPlay-style Instant Replay Buffer** - RAM-based H.264 encoding with on-demand MP4 muxing
  - Configurable duration (1 second to 20 minutes)
  - Configurable frame rate (15/30/60 FPS)
  - Aspect ratio options: Native, 16:9, 21:9, 4:3, 1:1
  - Per-monitor capture source selection
  - Hotkey-triggered save (default: F4)
  - RAM usage estimate displayed in settings UI
- Real-time H.264 encoding using Media Foundation Transform (MFT)
  - Software encoder (H264 Encoder MFT) for maximum compatibility
  - BGRA to NV12 color space conversion (BT.601)
  - Low-latency encoding mode for minimal capture delay
- Circular sample buffer for encoded H.264 NAL units
  - Duration-based eviction (keeps exactly N seconds)
  - Keyframe-aligned eviction for clean seeking
  - Thread-safe with critical section locking
- H.264 passthrough muxing to MP4 container
  - No re-encoding on save (instant, <500ms)
  - Precise frame timestamps to prevent timing drift
  - Proper keyframe marking for seeking

### Fixed
- Video duration accuracy: Precise timestamp calculation prevents cumulative rounding errors
  - Old: `timestamp = frame * (10000000/fps)` accumulated 1-second drift over 15 seconds
  - New: `timestamp = (frame * 10000000) / fps` maintains exact timing
- Aspect ratio cropping for ultra-wide monitors (5120x1440 → 2560x1440 for 16:9)

### Technical Details
- Three-component architecture:
  - `h264_encoder.c` - IMFTransform-based H.264 encoding to memory
  - `sample_buffer.c` - Circular buffer + passthrough MP4 muxing
  - `replay_buffer.c` - Capture orchestration and thread management
- Debug logging to `replay_debug.txt` for troubleshooting

---

## [0.9.1] - 2025-12-31

### Added
- Stop recording widget with timer display (MM:SS format counting up)
- Click-to-stop functionality on the timer widget
- Subtle hover effect on stop recording button
- Anti-aliased red recording indicator dot using GDI+
- Vertical divider separating timer from "Stop Recording" text

### Fixed
- Timer widget and border now excluded from screen capture (WDA_EXCLUDEFROMCAPTURE)
- All overlay windows created off-screen to prevent capture artifacts
- Improved window positioning to avoid black rectangle artifacts

### Changed
- Modern dark themed stop recording indicator with rounded corners
- Consistent Segoe UI font across all UI elements

## [0.9] - 2025-12-07

### Added
- Initial release
- Area, Window, Monitor, and All Monitors capture modes
- MP4 (H.264), AVI, and WMV output formats
- Quality presets: Low, Medium, High, Lossless
- DXGI Desktop Duplication for hardware-accelerated capture
- Windows 11 Snipping Tool-style UI
- Settings panel with format, quality, time limit, save location
- Single-instance mutex for macro key/Stream Deck toggle support
- Mouse cursor capture option
- Recording border overlay option
- Configurable time limit (hours/minutes/seconds)
- Auto-save with timestamp filenames
- INI-based configuration persistence
