# OBS DeckLink Low Latency

An independent Windows OBS output plugin prototype focused on reducing avoidable software-side buffering for Blackmagic DeckLink playback.

## Target workflow

`OBS A Program → DeckLink Mini Monitor HD → HDMI → AVerMedia GC551G2 → OBS B`

Initial target configuration:

- DeckLink Mini Monitor HD (BMDPCB218B)
- 1920×1080 at 24000/1001 FPS (23.976)
- BGRA video output
- Stereo, 48 kHz, 16-bit PCM HDMI audio
- OBS Studio 33.0.1 on Windows x64

## Current implementation

- Registers a separate output type: `obs_decklink_low_latency_output`
- Does not replace or overwrite OBS's built-in DeckLink module.
- Reuses the pinned OBS 33.0.1 DeckLink device discovery and device-mode implementation from the downloaded OBS source dependency.
- Uses an independent output scheduler and a bounded, mutex-protected frame-buffer ring.
- Offers a 2-buffer low-latency mode and a 3-buffer compatibility mode.
- When the software pool is full, replaces the oldest waiting frame with the newest frame rather than building a stale FIFO backlog.
- Reports software queue drops through OBS's output dropped-frame counter.
- Preserves stereo audio output at 48 kHz / 16-bit PCM.
- Reads the device-reported minimum preroll frame count and does not force the hardware below that value.

The software frame-buffer count and DeckLink hardware preroll are separate mechanisms. Selecting 2 buffers does **not** guarantee a 2-frame end-to-end delay. The actual hardware preroll is logged when output starts and may remain at 3 frames or higher.

Blackmagic documents `bmdDeckLinkConfigLowLatencyVideoOutput` as reducing latency on some older products, with no effect on newer products. It is not treated as a primary optimization for the Mini Monitor HD.

## OBS 33 plugin installation

The Windows package uses the OBS 33 third-party plugin layout:

```text
C:\ProgramData\obs-studio\plugins\obs-decklink-low-latency\
├── obs-decklink-low-latency.dll
└── data\
    └── locale\
        └── en-US.ini
```

The plugin is independent of the existing built-in DeckLink module.

## Build environment

- OBS Studio source dependency: 33.0.1
- Visual Studio: 2026
- Windows SDK: 10.0.26100
- CMake: 4.2 or newer
- Windows x64 only for this prototype

The DeckLink COM interfaces are generated at build time with MIDL from the SDK IDL files included in the pinned OBS source dependency. The project does not vendor a second copy of the DeckLink SDK.

## Validation status

**Prototype implementation; hardware validation is pending.**

The GitHub Actions Windows build must pass before a binary is considered usable. Runtime testing is still required on the Mini Monitor HD, including:

1. Confirming device discovery and supported 1080p23.976 mode enumeration.
2. Verifying continuous HDMI video and stereo audio.
3. Comparing the 2-buffer and 3-buffer modes for dropped frames and stability.
4. Measuring end-to-end latency through the GC551G2.
5. Confirming that OBS's built-in DeckLink output remains available as a fallback.

No end-to-end latency reduction is claimed until it is measured on the target hardware.
