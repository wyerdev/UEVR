
# UEVR (Patched Fork)

Goal of this fork: Fix some game crashes + Port essential ReShade shaders to fix washed-out colors and grey blacks in VR.

**Credits:** 
- Built on [praydog's UEVR](https://github.com/praydog/UEVR) ([Original UEVR README below](#original-uevr-readme)). 
- Shaders ported from ReShade originals by CeeJay.dk, prod80, Loadus, Martins Upitis, and Jeanseb ([license files](examples/)).



## [How to Install](docs/INSTALL.md)

## What's New in This Fork

### Crash Fixes

- Fix crashes in Creatures of Ava — [technical details](docs/native-stereo-crash-handler.md)
- Fix crashes on death in Returnal — [technical details](docs/transition-crash-handler.md)

### 10 ReShade Post-Processing Plugins

VR headsets often show washed-out colors and grey blacks compared to a flat monitor. This fork includes 10 ReShade shaders re-implemented as native UEVR C++ plugins that apply **directly to VR eye textures** (not just the desktop mirror), fixing these issues.

| # | Plugin | Based On | What It Does |
|---|--------|----------|--------------|
| 01 | LevelsPlus | Levels.fx (prod80) | Black/white point, per-channel gamma — **fixes grey/washed-out blacks** |
| 02 | LiftGammaGain | LiftGammaGain.fx (prod80) | Shadow/midtone/highlight RGB lift, gamma, gain |
| 03 | Tonemap | Tonemap.fx (prod80) | Gamma, exposure, saturation, bleach bypass, defog |
| 04 | Curves | Curves.fx (CeeJay.dk) | Luma/chroma contrast S-curve |
| 05 | FakeHDR | FakeHDR.fx (CeeJay.dk) | Local tone mapping via dual-radius bloom — [technical docs](docs/fakehdr-vr-postprocess-plugin.md) |
| 06 | DPX | DPX.fx (Loadus) | Cineon film stock color emulation |
| 07 | Technicolor | Technicolor2.fx (prod80) | 2-strip Technicolor color grading |
| 08 | Colourfulness | Colourfulness.fx (prod80) | Saturation enhancement with luma limiting |
| 09 | Vibrance | Vibrance.fx (Jeanseb) | Intelligent saturation boost |
| 10 | FilmGrain2 | FilmGrain2.fx (Martins Upitis) | Photographic film grain overlay |

All plugins are **disabled by default**. Enable them individually in the UEVR menu sidebar, or load a preset (see below). Plugins are loaded in numeric order (01→10): levels/color correction first, grain last. Settings are saved per-game automatically.

### Presets

Don't want to configure each plugin manually? Load a preset instead:

| Preset | What It Enables | Best For |
|--------|----------------|----------|
| **All Off** | Nothing | Reset everything back to defaults |
| **VR Fix - Black Levels** | LevelsPlus | Quick fix for grey/washed-out blacks |
| **VR Essentials** | LevelsPlus + Vibrance | Black fix + subtle color boost |
| **Cinematic** | LevelsPlus + Tonemap + Curves + DPX | Warm, film-like look |
| **Vivid** | LevelsPlus + Vibrance + Colourfulness | Punchy, saturated colors |
| **HDR Look** | LevelsPlus + FakeHDR + Colourfulness | Local tone mapping + enhanced color |

You can also save your own presets — **per-game** (local) or **shared across all games** (global). Use the **Quick Save** buttons if you're in VR with a gamepad and can't type a name.

### Other Improvements

- Left-aligned sidebar entries with sub-entry indentation

---

## Building Plugins

```bash
cmake --build build --config Release --target <plugin_name>
```

Or build the full project. Plugin DLLs output to `build/Release/`. Deploy to `%APPDATA%/UnrealVRMod/uevr/Plugins/`.

### Licenses

Each plugin includes a LICENSE.txt crediting the original ReShade shader author. All original shaders are open source (BSD, MIT, or public domain). See individual files in `examples/*/`.

---

# Original UEVR README

> Everything below is the original README from [praydog/UEVR](https://github.com/praydog/UEVR).

# UEVR ![build](https://github.com/praydog/UEVR/actions/workflows/dev-release.yml/badge.svg)

Universal Unreal Engine VR Mod (4/5)

## Supported Engine Versions

4.8 - 5.4

## Links

- [Download (Stable release)](https://github.com/praydog/UEVR/releases)
- [Download (Nightly release)](https://github.com/praydog/UEVR-nightly/releases/latest)
- [Documentation](https://praydog.github.io/uevr-docs)
- [Flat2VR Discord](https://flat2vr.com)

## Features

- Full 6DOF support out of the box (HMD movement)
- Full stereoscopic 3D out of the box
- Native UE4/UE5 stereo rendering system
- Frontend GUI for easy process injection
- Supports OpenVR and OpenXR runtimes
- 3 rendering modes: Native Stereo, Synchronized Sequential, and Alternating/AFR
- Automatic handling of most in-game UI so it is projected into 3D space
- Optional 3DOF motion controls out of the box in many games, essentially emulating a semi-native VR experience
- Optional roomscale movement in many games, moving the player character itself in 3D space along with the headset
- User-authored UI-based system for adding motion controls and first person to games that don't support them
- In-game menu with shortcuts for adjusting settings
- Access to various CVars for fixing broken shaders/effects/performance issues
- Optional depth buffer integration for improved latency on some headsets
- Per-game configurations
- [C++ Plugin system](https://praydog.github.io/uevr-docs/plugins/getting_started.html) and [Blueprint support](https://praydog.github.io/uevr-docs/plugins/blueprint.html) for modders to add additional features like motion controls

## Getting Started

Before launching, ensure you have installed .NET 6.0 SDK. It should tell you where to install it upon first open, but if not, you can [download it from here](https://dotnet.microsoft.com/en-us/download/dotnet/6.0). Most people should click x64 in the top left table, under the Installers column, next to windows.

Download the latest release from the [Releases page](https://github.com/praydog/UEVR/releases)

1. Launch UEVRInjector.exe
2. Launch the target game
3. Locate the game in the process dropdown list
4. Select your desired runtime (OpenVR/OpenXR)
5. Toggle existing VR plugin nullification (if necessary)
6. Configure pre-injection settings
7. Inject

## To-dos before injection

1. Disable HDR (it will still work without it, but the game will be darker than usual if it is)
2. Start as administrator if the game is not visible in the list
3. Pass `-nohmd` to the game's command line and/or delete VR plugins from the game directory if the game contains any existing VR plugins
4. Disable any overlays that may conflict and cause crashes (Rivatuner, ASUS software, Razer software, Overwolf, etc...)
5. Disable graphical options in-game that may cause crashes or severe issues like DLSS Frame Generation
6. Consider disabling `Hardware Accelerated GPU Scheduling` in your Windows `Graphics settings`

## In-Game Menu

Press the **Insert** key or **L3+R3** on an XInput based controller to access the in-game menu, which opens by default at startup. With the menu open, hold **RT** for various shortcuts:

- RT + Left Stick: Move the camera left/right/forward/back
- RT + Right Stick: Move the camera up/down
- RT + B: Reset camera offset
- RT + Y: Recenter view
- RT + X: Reset standing origin

## Quick overview of rendering methods

### Native Stereo

When it works, it looks the best, performs the best (usually). Can cause crashes or graphical bugs if the game does not play well with it.

Temporal effects like TAA are fully intact. DLSS/FSR2 usually work completely fine with no ghosting in this mode.

Fully synchronized eye rendering. Works with the majority of games. Uses the actual stereo rendering pipeline in the Unreal Engine to achieve a stereoscopic image.

### Synchronized Sequential

A form of AFR. Can fix many rendering bugs that are introduced with Native Stereo. Renders two frames **sequentially** in a **synchronized** fashion on the same engine tick.

Fully synchronized eye rendering. Game world does not advance time between frames.

Looks normal but temporal effects like TAA will have ghosting/doubling effect. Motion blur will need to be turned off.

This is the first alternative option that should be used if Native Stereo is not working as expected or you are encountering graphical bugs.

**Skip Draw** skips the viewport draw on the next engine tick. Usually works the best but sometimes particle effects may not play at the correct speed.

**Skip Tick** skips the next engine tick entirely. Usually buggy but does fix particle effects and sometimes brings higher performance.

### AFR

Alternated Frame Rendering. Renders each eye on separate frames in an alternating fashion, with the game world advancing time in between frames. Causes eye desyncs and usually nausea along with it.

Not synchronized. Generally should not be used unless the other two are unusable in some way.
