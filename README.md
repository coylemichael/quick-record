# Light Weight Screen Recorder

Lightweight screen recorder for Windows with instant replay. Buffer as much as your RAM allows and save replays instantly. Requires an NVIDIA GPU.

## Quick Start

1. Download the [latest release](https://github.com/coylemichael/light-weight-screen-recorder/releases/latest) and run `lwsr.exe`
2. Configure settings via the gear icon

**To record:** Select capture mode → hit record → stop when done

**To save a replay:** Press F4 (default, changable in settings) anytime (buffer runs in background)

</p>

> [!IMPORTANT]
> The replay buffer stores encoded video in RAM. Higher durations and resolutions use more memory:
>
> | Duration | Resolution | Approx. RAM |
> |----------|------------|-------------|
> | 15 sec   | 1080p 30fps | ~50 MB     |
> | 1 min    | 1080p 30fps | ~200 MB    |
> | 5 min    | 1080p 60fps | ~1.5 GB    |
> | 20 min   | 1440p 60fps | ~8 GB      |
>
> If you're running low on memory, reduce the replay duration or resolution.

<p align="center">
  <img src="static/overlay.png" alt="LWSR Toolbar">


## Build

<details>
<summary>Build from source</summary>

Requires Visual Studio Build Tools (MSVC). The build script will prompt to install automatically if not found.

```batch
build.bat
```

Output: `bin\lwsr.exe`

</details>

## Verification

- ✅ **Attestation** - Releases are built on GitHub Actions with [build provenance](https://docs.github.com/en/actions/security-guides/using-artifact-attestations-to-establish-provenance-for-builds)
- ✅ **SHA256 hash** - Each release includes a hash for integrity verification
- ✅ **Open source** - Audit the code or build it yourself
