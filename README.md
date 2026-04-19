
# UEVR (Patched Fork)

Goal of this fork: Fix some game crashes + Port essential ReShade shaders to fix washed-out colors and grey blacks in VR.

**Credits:** 
- Built on [praydog's UEVR](https://github.com/praydog/UEVR) ([Original UEVR README below](#original-uevr-readme)). 
- Shaders ported from ReShade originals by CeeJay.dk, 3an, DKT70, Loadus, Martins Upitis, bacondither, Ioxa, and kingeric1992 ([license files](examples/)).

# [How to Install](docs/INSTALL.md)

# What's New in This Fork

## Crash Fixes

- Fix the main Native Stereo crash in Creatures of Ava and improve D3D12 load/transition resilience — [technical details](docs/native-stereo-crash-handler.md) and [load-transition notes](docs/ATTEMPT_3_LOAD_CRASH_FIX_PLAN.md)
- Fix crashes on death in Returnal — [technical details](docs/transition-crash-handler.md)

Rare cutscene or 3P transition crashes may still remain in some games. The D3D12 work in this fork is defensive hardening, not a blanket fix for every transition failure mode.

## UEVR Plugins: ReShade Post-Processing Shaders
- Fix washed-out colors and grey blacks

VR headsets often show **washed-out colors and grey blacks** compared to a flat monitor. This fork includes 13 ReShade shaders to fix this, re-implemented as native UEVR C++ plugins that apply **directly to VR eye textures** (not just the desktop mirror), fixing these issues.

### **Performance**

Performance impact is small (hopefully) — typically **1–2 ms** on DX12, more on DX11. 

It varies by title: some games show no loss at all, others a bit more. And maybe depends on GPU as well.

The visual improvement is well worth the small performance impact.

### **Where to start:** 
- **LevelsPlus** fixes grey blacks (the #1 VR issue) and 
- **FakeHDR** is the easiest way to make any game look good
Try one or both.

**Color Correction (fix what's broken)**

These clip by design — they remap the tonal range, trading some shadow/highlight detail for better contrast. Almost always worth it.

| # | Shader | When to Use It |
|---|--------|----------------|
| 01 | **LevelsPlus** | **Fix grey/washed-out blacks.** The #1 VR problem. Remaps black/white points so darks are actually dark and whites are actually bright. Trades some shadow detail for deeper blacks — almost always worth it. Also has per-channel gamma and optional ACES tone mapping. Start here. |
| 02 | **LiftGammaGain** | Fine-tune shadows, midtones, and highlights separately. Use if LevelsPlus alone isn't enough — e.g. shadows are too blue, or highlights are too warm. Gain can clip highlights if pushed high. |
| 03 | **Tonemap** | Adjust overall gamma, exposure, and saturation. Exposure can clip highlights; defog subtracts color. Also has bleach bypass (desaturated high-contrast film look). |

**Color Grading (make it look good)**

These change the overall look and feel of the image. Detail-safe — they enhance without clipping (except Filmic Pass, which can clip at extreme settings).

| # | Shader | When to Use It |
|---|--------|----------------|
| 04 | **Curves** | Add contrast using S-curves. Redistributes contrast within the existing range without clipping. Multiple curve formulas (Luma, Chroma, etc). Subtle but effective. |
| 05 | **FakeHDR** | **Easiest way to make any game look good.** Deepens darks and makes colors pop via local tone mapping bloom. Enhances detail without clipping. [Technical docs](docs/fakehdr-vr-postprocess-plugin.md). |
| 06 | **DPX** | Emulates Cineon film stock. Gives a warm, cinematic color shift with a strength slider. Good for games that look too cold/digital. |
| 07 | **Technicolor** | Emulates 2-strip Technicolor (old Hollywood look). Strong color shift — teal shadows, warm highlights. Use sparingly. |
| 08 | **Colourfulness** | Boosts color saturation with a built-in limiter that prevents clipping. Smarter than just cranking saturation. |
| 09 | **Vibrance** | Boosts unsaturated colors more than saturated ones. Mathematically avoids clipping. Good for making dull games pop without oversaturating skin tones. |
| 11 | **HSL Shift** | Remap individual colors to different hues. E.g. make greens more vivid, shift reds toward orange, cool down skin tones. 8 color zones you can shift independently. Changes color direction, not intensity. |
| 12 | **Filmic Pass** | Full cinematic color processing: sigmoid curves per RGB channel, bleach bypass, fade, saturation, and per-channel gamma. Can clip at extreme settings. More control than Tonemap — use when you want a specific film look. |

**Detail & Film Effects (finishing touches)**

| # | Shader | When to Use It |
|---|--------|----------------|
| 10 | **FilmGrain2** | Adds subtle photographic film grain. Hides color banding in dark areas (common on VR panels). Keep it subtle — high values look noisy. |
| 13 | **Clarity** | Local contrast enhancement — makes textures and details pop without changing colors or clipping. Works like sharpening but on mid-frequency detail. Multiple blend modes (Soft Light, Overlay, Hard Light, etc). **Very effective in VR** where things often look flat. |

All shaders are **disabled by default**. Enable them individually in the UEVR menu sidebar, or load a preset (see below). Shaders are loaded in numeric order (01→13). Settings are saved per-game automatically.

### Presets

Don't want to configure each shader manually? Load a preset instead:

| Preset | What It Enables | Best For |
|--------|----------------|----------|
| **All Off** | Nothing | Reset everything back to defaults |
| **VR Fix - Black Levels** | LevelsPlus | Quick fix for grey/washed-out blacks |
| **VR Essentials** | LevelsPlus + Vibrance | Black fix + subtle color boost |
| **Cinematic** | LevelsPlus + Tonemap + Curves + DPX | Warm, film-like look |
| **Vivid** | LevelsPlus + Vibrance + Colourfulness | Punchy, saturated colors |
| **HDR Look** | LevelsPlus + FakeHDR + Colourfulness | Local tone mapping + enhanced color |

You can also save your own presets — **per-game** (local) or **shared across all games** (global).

---

### Building Plugins

```bash
cmake --build build --config Release --target <plugin_name>
```

Or build the full project. Plugin DLLs output to `build/Release/`. Deploy to `%APPDATA%/UnrealVRMod/UEVR/plugins/` (global) or `%APPDATA%/UnrealVRMod/<game_executable>/plugins/` (per-game).

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
