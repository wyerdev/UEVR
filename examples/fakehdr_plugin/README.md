# FakeHDR Shader for UEVR

A UEVR C++ shader that applies a FakeHDR post-processing effect to VR frames.
Based on CeeJay.dk's FakeHDR ReShade effect, ported to work natively in UEVR's
per-eye VR rendering pipeline.

## Why not just use ReShade?

Standard ReShade has several issues with VR:

1. **ReShade applies to the flat swapchain**, not the per-eye VR textures.
   UEVR renders to stereo textures that get submitted directly to the VR
   compositor (OpenXR/OpenVR). ReShade hooks the swapchain `Present()` call,
   which only affects the spectator/desktop mirror — the HMD never sees it.

2. **Double-application / wrong resolution**: If ReShade does somehow run, it
   can process the double-wide texture (both eyes side-by-side) as a single
   image, causing bloom kernels to bleed across the eye boundary.

3. **Performance**: ReShade adds a full extra post-process pass on the
   swapchain. This shader runs inside UEVR's existing VR render callback with
   zero extra copies beyond the required SRV copy.

## How it works

This shader hooks into UEVR's `on_pre_render_vr_framework_dx11/dx12` callbacks,
which fire **per-eye** after the engine renders but before submission to the VR
compositor. For each eye:

1. Copies the eye texture to a staging SRV (so we can sample it)
2. Runs a full-screen triangle with the FakeHDR pixel shader
3. Writes the result back to the eye's RTV

The shader is compiled at runtime via `D3DCompile` — no pre-compiled `.cso`
files needed.

## Parameters

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| HDR Power | 1.30 | 0.0 – 8.0 | Strength of the HDR tonemapping curve |
| Radius 1 | 0.793 | 0.0 – 8.0 | Inner bloom sample radius |
| Radius 2 | 0.87 | 0.0 – 8.0 | Outer bloom sample radius (higher = stronger + brighter) |

All parameters are adjustable at runtime via the UEVR menu (Insert key → FakeHDR sidebar entry).
Settings are auto-saved per game to `data/plugins/fakehdr_settings.txt`.

## Building

The plugin is built as part of the UEVR project:

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target fakehdr_plugin
```

Output: `build/Release/05_FakeHDRShader.dll`

## Installation

1. Copy `05_FakeHDRShader.dll` to `%APPDATA%/UnrealVRMod/UEVR/plugins/` (global, all games) or `%APPDATA%/UnrealVRMod/<game_executable>/plugins/` (per-game)
2. Launch the game with UEVR
3. Open UEVR menu (Insert or L3+R3) → FakeHDR sidebar entry → Enable

This plugin is one of 15 ReShade-ported post-processing plugins. See the [main README](../../README.md) and [technical docs](../../docs/fakehdr-vr-postprocess-plugin.md) for the full suite.

## Credits

- Original FakeHDR effect by Christian Cann Schuldt Jensen (CeeJay.dk)
- UEVR by praydog
