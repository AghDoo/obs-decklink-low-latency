# OBS DeckLink Low Latency

An independent Windows OBS output plugin prototype for synchronous Blackmagic DeckLink HDMI video and audio.

## Target workflow

`OBS A Program → DeckLink Mini Monitor → HDMI → AVerMedia GC551G2 → OBS B`

Initial target configuration:

- DeckLink Mini Monitor
- 1920×1080 at 24 FPS
- BGRA video output
- Stereo, 48 kHz, 16-bit PCM HDMI audio
- OBS Studio 33.0.1 on Windows x64

## Current implementation

- Registers a separate output type: `obs_decklink_low_latency_output`
- Provides a separate OBS frontend dock for selecting the device and mode, and starting or stopping Program output.
- Installs alongside OBS's built-in DeckLink module.
- Reuses the pinned OBS 33.0.1 DeckLink device discovery and device-mode implementation from the downloaded OBS source dependency.
- Uses `DisplayVideoFrameSync()` on a dedicated output thread. The synchronous path bypasses scheduled playback and its preroll.
- Holds at most one pending video frame. A newer frame replaces it when the output thread is still busy.
- Sends 48 kHz / 16-bit interleaved stereo PCM through `WriteAudioSamplesSync()`. The audio queue is bounded, and partial writes are retried while the block is fresh.
- Logs the first accepted video and audio submissions, and summarizes video drops, driver errors and audio writes on stop.

## OBS 33 plugin installation

Download the Windows x64 `modules` artifact from the GitHub Actions run. Close OBS and copy both extracted folders into `C:\ProgramData\obs-studio\plugins\`:

```text
C:\ProgramData\obs-studio\plugins\
├── obs-decklink-low-latency\
│   ├── obs-decklink-low-latency.dll
│   └── data\locale\en-US.ini
└── obs-decklink-low-latency-ui\
    ├── obs-decklink-low-latency-ui.dll
    └── data\locale\
        ├── en-US.ini
        └── zh-TW.ini
```

Restart OBS and open **Docks → DeckLink Low Latency Output**. You can dock the panel by dragging its title bar. The **Tools** menu can also open the panel. Select the device and video mode, then click **Start output**.

Enable **Automatically start output when OBS opens** to use the saved device and mode after OBS starts, including when the dock is hidden. The saved device and mode must be available. Disable auto-start in the built-in DeckLink output if it uses the same device.

## Build environment

- OBS Studio source dependency: 33.0.1
- Visual Studio: 2026
- Windows SDK: 10.0.26100
- CMake: 4.2 or newer
- Windows x64 only for this prototype
- GitHub-hosted runner: `windows-2025-vs2026`
- Qt 6 Widgets and OBS Frontend API for the UI module

The DeckLink COM interfaces are generated at build time with MIDL from the SDK IDL files included in the pinned OBS source dependency. The project does not vendor a second copy of the DeckLink SDK.

## Validation status

**Prototype implementation; hardware validation is pending.**

Hardware verification uses the same Mini Monitor and GC551G2 chain for both this plugin and OBS's built-in scheduled output.

1. Confirming device discovery and supported 1080p24 mode enumeration.
2. Verifying continuous HDMI video and stereo audio.
3. Comparing end-to-end latency, dropped or repeated frames, and A/V offset against the built-in scheduled output.
4. Verifying repeated start/stop cycles and OBS startup with automatic output enabled.
