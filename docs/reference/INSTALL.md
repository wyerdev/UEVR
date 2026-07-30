

# How to Install (last confirmed: 2026-07-30)

## Choose Your Build Line

This fork is published as more than one build line. Each one patches a
**different upstream UEVR core**, so pick the one you want *before* downloading
— the core zip and the shaders zip must come from the same line.

| Build line | Upstream core it patches | Filename / folder ID | Get it |
|---|---|---|---|
| **UEVR ReShade** (the main one) | [praydog/UEVR](https://github.com/praydog/UEVR) `master` | `reshade` | [Latest ReShade release](https://github.com/wyerdev/UEVR/releases/latest) · [all](https://github.com/wyerdev/UEVR/releases?q=uevr-reshade-release) |
| **UEVR ReShade AFW** | [PureDark/UEVR](https://github.com/PureDark/UEVR) `AFW` — asynchronous frame warp | `reshade-afw` | [all AFW releases](https://github.com/wyerdev/UEVR/releases?q=uevr-reshade-afw-release) |

Use the links above rather than scrolling the [releases page](https://github.com/wyerdev/UEVR/releases)
— releases from all build lines are interleaved there by date, so the newest
release of your line can be buried. The **Latest** badge always goes to the
main ReShade line; that is on purpose and does not mean the AFW line is
outdated.

The build lines **do not conflict**. Each one installs its shaders into its own
`plugins\<id>\` folder and only loads plugins from that folder, so you can keep
both installed side by side. Everywhere below, replace `<id>` with your build
line's ID from the table (e.g. `reshade`).

## Before You Start

- **Antivirus:** UEVR injects into game processes, which antivirus software often flags as suspicious. Your AV will likely quarantine or delete UEVR files. **Add the UEVR folder to your antivirus exclusion list** before extracting.
- **Frame Generation:** Disable DLSS/FSR Frame Generation in-game — it causes severe issues with VR.
- **DLSS Upscaling:** Recommended for performance. If your game supports it, enable DLSS Super Resolution. With DLSS 4, you can use [DLSS Swapper](https://github.com/beeradmoore/dlss-swapper) to upgrade the game's DLSS version.

## Steps

1. **Download the upstream build your build line patches**, and extract it.
   It is not the same download for every line:

   | Build line | Base download |
   |---|---|
   | **UEVR ReShade** | [praydog UEVR Nightly](https://github.com/praydog/UEVR-nightly/releases) |
   | **UEVR ReShade AFW** | [PureDark UEVR AFW release](https://github.com/PureDark/UEVR/releases) — also the only source of `PDAFWPlugin.dll`, which the AFW line needs |

   Every release body names the exact base build it was made against, and links
   straight to it. Use that link rather than picking the newest one yourself.
2. **Download the core zip from that same release** and **overwrite** the files
   from step 1 with it

### Install Shaders

3. **Download the shaders zip from the same release** — shaders from one build line will not load on another
4. Install shaders using **one** of these options:
   - **All games (recommended):** Run `install-plugins.bat` from the extracted zip
   - **Single game only:** See [Manual install](#manual-install-optional) below

### Play

5. **Inject as usual** and open the UEVR menu (**Insert** or **L3+R3**) to configure shaders or load a preset

> **Note:** These shaders only work with this patched fork, and only with the
> build line they were released for. They will **not** load on stock UEVR
> nightly, or on a different build line — UEVR just ignores them, nothing
> breaks.

### Manual install (optional)

Instead of `install-plugins.bat`, you can copy files manually:

**Global (all games):** Copy `*Shader.dll` and `*-LICENSE.txt` to `%APPDATA%\UnrealVRMod\UEVR\plugins\<id>\`

**Per-game (one game only):** Copy them to `%APPDATA%\UnrealVRMod\<game_executable>\plugins\<id>\` instead (e.g. `Oregon-Win64-Shipping\plugins\reshade\`)

**Presets:** Copy the shipped `.uevrpreset` files from the zip's `shipping_presets\` (or `presets\`) folder to `%APPDATA%\UnrealVRMod\UEVR\data\plugins\<id>\shipping_presets\`.

## LUT Customization

The LUT shader ships with several built-in presets (warm, cool, cinematic, bleach, default). Switch between them in the LUT shader's UI panel — no file changes required.

To add your own LUT, drop a `lut_<name>.png` (1024×32 horizontal-tile RGBA8) into one of:

- **Global (all games):** `%APPDATA%\UnrealVRMod\UEVR\data\plugins\<id>\shader_assets\`
- **Per-game (overrides global by filename):** `%APPDATA%\UnrealVRMod\<game_executable>\data\plugins\<id>\shader_settings\`

It will appear in the LUT plugin's preset dropdown after a rescan.

## File Locations

| What | Where |
|------|-------|
| Global shaders | `%APPDATA%\UnrealVRMod\UEVR\plugins\<id>\` — loaded for **all** games |
| Per-game shaders | `%APPDATA%\UnrealVRMod\<game_executable>\plugins\<id>\` — loaded for that game only |
| User presets | `%APPDATA%\UnrealVRMod\UEVR\data\plugins\<id>\presets\` — `.uevrpreset` files shared across games |
| Built-in presets | `%APPDATA%\UnrealVRMod\UEVR\data\plugins\<id>\shipping_presets\` — `.uevrpreset` files overwritten on update |
| Per-game settings | `%APPDATA%\UnrealVRMod\<game_executable>\data\plugins\<id>\shader_settings\auto.uevrpreset` |

## Uninstalling

Run `uninstall-plugins.bat` from the release folder or from `%APPDATA%\UnrealVRMod\UEVR\plugins\<id>\` (copied there by the installer). It only removes its own build line's files; other build lines are left alone. Removes all shader DLLs, licenses, built-in presets, **user-saved presets, per-game shader settings, and active preset selections**.

## Updating

Download both the new nightly and the matching fork release **for the same build line**, overwrite again, and re-run `install-plugins.bat`.

## Credits

This fork exists because of other people's work. What each release ships:

- **UEVR itself** — created by [Praydog](https://github.com/praydog). Every build line here is a patched [praydog/UEVR](https://github.com/praydog/UEVR) core; that code is Praydog's work.
- **Asynchronous frame warp (AFW)** — by [PureDark](https://github.com/PureDark), from [PureDark/UEVR](https://github.com/PureDark/UEVR). Shipped in the **AFW** build line.
- **Shader effects** — ported from [crosire's ReShade](https://github.com/crosire/reshade) shaders. Original authors: CeeJay.dk, AMD, SLSNe, Marty McFly, 3an, DKT70, Loadus, Martins Upitis, bacondither, Ioxa, kingeric1992, Niklas Haas (haasn), JPulowski, luluco250, brussell1, Alex Tuduran, and Timothy Lottes / NVIDIA. This fork does **not** ship the ReShade runtime, injector, or shader compiler — selected shaders are re-implemented as native UEVR plugins. Each shader's `*-LICENSE.txt` in the shaders zip credits its original author and source; the sources are in [examples/](../../examples/).

The crash fixes, the plugin loader and preset system, keeping the build lines
separate from each other, and the shader ports are this fork's own work.
