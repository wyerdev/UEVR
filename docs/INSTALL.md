

# How to Install

## Before You Start

- **Antivirus:** UEVR injects into game processes, which antivirus software often flags as suspicious. Your AV will likely quarantine or delete UEVR files. **Add the UEVR folder to your antivirus exclusion list** before extracting.
- **Frame Generation:** Disable DLSS/FSR Frame Generation in-game — it causes severe issues with VR.
- **DLSS Upscaling:** Recommended for performance. If your game supports it, enable DLSS Super Resolution. With DLSS 4, you can use [DLSS Swapper](https://github.com/beeradmoern/dlss-swapper) to upgrade the game's DLSS version.

## Steps

1. **Download the [UEVR Nightly](https://github.com/praydog/UEVR-nightly/releases)** and extract it
2. **Download the [Patched Fork release](https://github.com/wyerdev/UEVR/releases)** (must match your nightly version) and **overwrite** the nightly files with it

### Install Shaders

3. **Download the Shaders zip** from the same [release page](https://github.com/wyerdev/UEVR/releases)
4. Install shaders using **one** of these options:
   - **All games (recommended):** Run `install-plugins.bat` from the extracted zip
   - **Single game only:** See [Manual install](#manual-install-optional) below

### Play

5. **Inject as usual** and open the UEVR menu (**Insert** or **L3+R3**) to configure shaders or load a preset

> **Note:** These shaders only work with this patched fork. They will **not** load on stock UEVR nightly — the version guard safely skips them.

### Manual install (optional)

Instead of `install-plugins.bat`, you can copy files manually:

**Global (all games):** Copy `*Shader.dll` and `*-LICENSE.txt` to `%APPDATA%\UnrealVRMod\UEVR\plugins\`

**Per-game (one game only):** Copy them to `%APPDATA%\UnrealVRMod\<game_executable>\plugins\` instead (e.g. `Oregon-Win64-Shipping\plugins\`)

**Presets:** Copy the `shipping_presets` folder to `%APPDATA%\UnrealVRMod\UEVR\data\plugins\shipping_presets\`

## File Locations

| What | Where |
|------|-------|
| Global shaders | `%APPDATA%\UnrealVRMod\UEVR\plugins\` — loaded for **all** games |
| Per-game shaders | `%APPDATA%\UnrealVRMod\<game_executable>\plugins\` — loaded for that game only |
| User presets | `%APPDATA%\UnrealVRMod\uevr\data\plugins\presets\` |
| Built-in presets | `%APPDATA%\UnrealVRMod\UEVR\data\plugins\shipping_presets\` — overwritten on update |
| Per-game settings | `%APPDATA%\UnrealVRMod\<game_executable>\data\plugins\` |

## Uninstalling

Run `uninstall-plugins.bat` from the release folder or from `%APPDATA%\UnrealVRMod\UEVR\plugins\`. Removes all shader DLLs, licenses, and built-in presets.

## Updating

Download both the new nightly and the matching fork release, overwrite again, and re-run `install-plugins.bat`.
