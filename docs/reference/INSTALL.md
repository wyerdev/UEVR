

# How to Install (last confirmed: 2026-08-09)

## Choose Your Build Line

Each build line patches a **different upstream UEVR core**. Pick one *before*
downloading. Each release has one shader plugins zip containing that line's
backend files and the shared shader installation files.

| Build line | Upstream core it patches | Filename / folder ID | Get it |
|---|---|---|---|
| **UEVR ReShade Mainline** (the main one) | [praydog/UEVR](https://github.com/praydog/UEVR) `master` | `reshade` | [All Mainline releases](https://github.com/wyerdev/UEVR/releases?q=buildline1mainline) |
| **UEVR ReShade AFW** | [PureDark/UEVR](https://github.com/PureDark/UEVR) `AFW` — asynchronous frame warp | `reshade-afw` | [All AFW releases](https://github.com/wyerdev/UEVR/releases?q=buildline2afw) |
| **UEVR ReShade AFW + Joeyhodge** | [PureDark/UEVR](https://github.com/PureDark/UEVR) `Joey-Merged` — asynchronous frame warp on top of [joeyhodge/UEVR](https://github.com/joeyhodge/UEVR)'s UE 5.5-5.8 fixes | `reshade-afw-joeyhodge` | [All AFW + Joeyhodge releases](https://github.com/wyerdev/UEVR/releases?q=buildline3afwjoeyhodge) |

Use those links, not the [full releases page](https://github.com/wyerdev/UEVR/releases) —
lines are interleaved by date there, and the **Latest** badge always goes to
Mainline. Backend lines can be installed side by side. They all use the shared
shader directory, so the shader set is installed once for all lines.

> **Before you start:** add the UEVR folder to your **antivirus exclusions**
> (injection gets flagged), disable in-game **DLSS/FSR Frame Generation** (it
> breaks VR), and enable **DLSS Super Resolution** if available — optionally
> upgraded with [DLSS Swapper](https://github.com/beeradmoore/dlss-swapper).

## Steps

1. **Download the upstream build your build line patches**, and extract it.
   It is not the same download for every line:

   | Build line | Base download |
   |---|---|
   | **UEVR ReShade Mainline** | [praydog UEVR Nightly](https://github.com/praydog/UEVR-nightly/releases) |
   | **UEVR ReShade AFW** | [PureDark UEVR AFW release](https://github.com/PureDark/UEVR/releases) — also the only source of `PDAFWPlugin.dll`, which the AFW line needs |
   | **UEVR ReShade AFW + Joeyhodge** | the same [PureDark UEVR AFW release](https://github.com/PureDark/UEVR/releases), but the **joeyhodge-based** zip inside it — each AFW release ships both, and that zip is also the only source of `PDAFWPlugin.dll` |

   Every release body names the exact base build it was made against, and links
   straight to it. Use that link rather than picking the newest one yourself.
2. **Download the shader plugins zip from that same release** and extract it over
   the files from step 1, choosing **overwrite/replace**.

### Install shaders

3. Install shaders using **one** of these options:
   - **All games (recommended):** Run `install-plugins.bat` from the extracted zip
   - **Single game only:** See [Manual install](#manual-install-optional) below

### Play

4. **Inject as usual** and open the UEVR menu (**Insert** or **L3+R3**) to configure shaders or load a preset

> **Note:** These shaders work with this patched fork's loader. The shader DLLs
> are shared by the three build lines, but the backend files in each package
> remain specific to the selected line. Do not replace a line's backend with a
> different build line or with stock UEVR.

> **Not working on the AFW or AFW + Joeyhodge line?** Get the *original* build
> working first — install [PureDark](https://github.com/PureDark)'s (or
> [joeyhodge](https://github.com/joeyhodge)'s) unmodified release on its own and
> confirm your game runs with it.

### Manual install (optional)

Instead of `install-plugins.bat`, you can copy files manually:

**Global (all games):** Copy `*Shader.dll` and `*-LICENSE.txt` to `%APPDATA%\UnrealVRMod\UEVR\plugins\shaders\`

**Per-game (one game only):** Copy them to `%APPDATA%\UnrealVRMod\<game_executable>\plugins\shaders\` instead.

**Presets:** Copy the shipped `.uevrpreset` files from the zip's `shipping_presets\` (or `presets\`) folder to `%APPDATA%\UnrealVRMod\UEVR\data\plugins\shipping_presets\`.

## LUT Customization

The LUT shader ships with several built-in presets (warm, cool, cinematic, bleach, default). Switch between them in the LUT shader's UI panel — no file changes required.

To add your own LUT, drop a `lut_<name>.png` (1024×32 horizontal-tile RGBA8) into one of:

- **Global (all games):** `%APPDATA%\UnrealVRMod\UEVR\data\plugins\shader_assets\`
- **Per-game (overrides global by filename):** `%APPDATA%\UnrealVRMod\<game_executable>\data\plugins\shader_settings\`

It will appear in the LUT plugin's preset dropdown after a rescan.

## File Locations

Only the shader DLLs are scoped by build line ID. Presets, settings and shader
assets are shared, so switching build lines keeps your configuration.

| What | Where |
|------|-------|
| Global shaders | `%APPDATA%\UnrealVRMod\UEVR\plugins\shaders\` — loaded for **all** games |
| Per-game shaders | `%APPDATA%\UnrealVRMod\<game_executable>\plugins\shaders\` — loaded for that game only |
| User presets | `%APPDATA%\UnrealVRMod\UEVR\data\plugins\presets\` — `.uevrpreset` files shared across games |
| Built-in presets | `%APPDATA%\UnrealVRMod\UEVR\data\plugins\shipping_presets\` — `.uevrpreset` files overwritten on update |
| Per-game settings | `%APPDATA%\UnrealVRMod\<game_executable>\data\plugins\shader_settings\auto.uevrpreset` |

## Uninstalling

Run `uninstall-plugins.bat`, the shader-uninstaller, from the release folder or from the shared plugin directory (copied there by the shader-installer). It removes the shared shader DLLs and licenses for all build lines. Presets, shader settings and shader assets are shared and are only deleted once no shader installation remains.

## Updating

Download the new upstream base and the matching shader plugins zip **for the same
build line**, overwrite again, and re-run the shader-installer,
`install-plugins.bat`.

## Credits

**Huge thanks to Praydog, PureDark, joeyhodge, crosire, and the ReShade shader
authors — none of this exists without them.**

This fork stands on other people's work. It brings that work together and makes
the shaders usable inside UEVR. What each release ships:

- **UEVR itself** — by [Praydog](https://github.com/praydog). Every build line here is a patched [praydog/UEVR](https://github.com/praydog/UEVR) core.
- **Asynchronous frame warp (AFW)** — by [PureDark](https://github.com/PureDark), from [PureDark/UEVR](https://github.com/PureDark/UEVR). Shipped in the **AFW** and **AFW + Joeyhodge** build lines. The proprietary `PDAFWPlugin.dll` it needs is PureDark's and is distributed only by him — this fork never ships it.
- **UE 5.5-5.8 fixes** — by [joeyhodge](https://github.com/joeyhodge), from [joeyhodge/UEVR](https://github.com/joeyhodge/UEVR), merged into PureDark's `Joey-Merged` branch. Shipped in the **AFW + Joeyhodge** build line.
- **Shader effects** — by their original authors, written for [ReShade](https://github.com/crosire/reshade) by [crosire](https://github.com/crosire): CeeJay.dk, AMD, SLSNe, Marty McFly, 3an, DKT70, Loadus, Martins Upitis, bacondither, Ioxa, kingeric1992, Niklas Haas (haasn), JPulowski, luluco250, brussell1, Alex Tuduran, and Timothy Lottes / NVIDIA. This fork does **not** ship the ReShade runtime, injector, or shader compiler — selected shaders are re-implemented as native UEVR plugins. Each shader's `*-LICENSE.txt` in the shader plugins zip credits its original author and source; the sources are in [examples/](../../examples/).
- **This fork** — the crash fixes, the plugin loader and preset system, and the *port* of the shaders into native UEVR plugins. Nothing else on this list is this fork's work.
