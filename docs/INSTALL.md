# How to Install

## Prerequisites

- A VR headset (SteamVR or OpenXR)
- [.NET 6.0 SDK](https://dotnet.microsoft.com/en-us/download/dotnet/6.0) (x64 Windows installer)
- The matching UEVR Nightly build for your game's Unreal Engine version

## Steps

1. **Download the base UEVR Nightly**
   - Go to the [UEVR Nightly releases](https://github.com/praydog/UEVR-nightly/releases)
   - Download the latest release zip

2. **Download this fork's release**
   - Go to the [Patched Fork releases](https://github.com/wyerdev/UEVR/releases)
   - Download the latest release zip
   - **Overwrite** the nightly files with this fork's files (this replaces the main UEVR DLL and adds the plugins)

3. **Launch your game**
   - Open `UEVRInjector.exe`
   - Launch the target game
   - Select the game from the process dropdown
   - Choose your runtime (OpenVR or OpenXR)
   - Click Inject

4. **Configure plugins and presets**
   - Press **Insert** or **L3+R3** on a gamepad to open the UEVR menu
   - Plugins appear in the sidebar — enable/disable individually and adjust settings
   - Or load a preset from the Presets section to get started quickly

## File Locations

| What | Where |
|------|-------|
| Main UEVR files | Wherever you extracted the nightly zip |
| Plugins (.dll) | `%APPDATA%/UnrealVRMod/uevr/Plugins/` |
| Presets | `%APPDATA%/UnrealVRMod/uevr/presets/` |
| Per-game settings | `%APPDATA%/UnrealVRMod/<game_executable>/` |

## Tips

- **Disable HDR** in-game — it still works, but the image will be darker than usual
- **Run as administrator** if the game doesn't appear in the process list
- **Disable overlays** that may cause crashes (Rivatuner, ASUS software, Razer software, Overwolf, etc.)
- **Disable DLSS Frame Generation** if present — it often causes crashes
- Consider disabling **Hardware Accelerated GPU Scheduling** in Windows Graphics settings
- Pass `-nohmd` to the game's command line if it has existing VR plugins

## Updating

When a new nightly is released, download both the new nightly **and** the matching fork release, then overwrite again. Your per-game settings and saved presets are preserved — they live in `%APPDATA%` and are not overwritten.
