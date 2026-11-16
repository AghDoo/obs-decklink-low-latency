# OBS DeckLink Low Latency

An independent OBS Studio output plugin project focused on reducing avoidable buffering in Blackmagic DeckLink HDMI output.

## Target workflow

`OBS A Program → DeckLink Mini Monitor HD → HDMI → AVerMedia GC551G2 → OBS B`

Initial target configuration:

- DeckLink Mini Monitor HD (BMDPCB218B)
- 1920×1080 at 24000/1001 FPS (23.976)
- BGRA video output
- Stereo HDMI audio
- OBS Studio 33.0.1 on Windows x64

## Design goals

- Add a separate OBS output plugin; do not replace or overwrite OBS's built-in DeckLink module.
- Keep the official DeckLink output available as a fallback.
- Inspect the device-reported minimum preroll depth before offering lower-latency overrides.
- Treat hardware preroll depth and CPU frame-buffer rotation as separate mechanisms.
- Preserve synchronized stereo audio.
- Measure end-to-end latency through the GC551G2 rather than assuming that a smaller software queue guarantees a specific latency.

## OBS 33 plugin installation

The Windows package is intended to use the OBS 33 third-party plugin layout:

```text
C:\ProgramData\obs-studio\plugins\obs-decklink-low-latency\
├── obs-decklink-low-latency.dll
└── data\
```

The plugin is independent of the existing built-in DeckLink DLL.

## Build environment

- OBS Studio sources: 33.0.1
- Visual Studio: 2026
- Windows SDK: 10.0.26100
- CMake: 4.2 or newer
- Qt 6 and OBS Frontend API enabled

## Current status

**Foundation stage only.** This commit updates the project identity, OBS 33 build dependencies, Windows toolchain, and plugin installation layout. The DeckLink output engine and user interface have not been implemented yet.
