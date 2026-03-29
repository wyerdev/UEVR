# FakeHDR Plugin for UEVR

A UEVR C++ plugin that applies a FakeHDR post-processing effect to VR frames.
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
   swapchain. This plugin runs inside UEVR's existing VR render callback with
   zero extra copies beyond the required SRV copy.

## How it works

This plugin hooks into UEVR's `on_post_render_vr_framework_dx11` callback,
which fires **per-eye** after the engine renders but before submission to the VR
compositor. For each eye:

1. Copies the eye texture to a staging SRV (so we can sample it)
2. Runs a full-screen triangle with the FakeHDR pixel shader
3. Writes the result back to the eye's RTV

The shader is compiled at runtime via `D3DCompile` — no pre-compiled `.cso`
files needed.

## Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| HDRPower  | 1.30    | Strength of the HDR tonemapping curve |
| Radius1   | 0.793   | Inner bloom sample radius |
| Radius2   | 0.87    | Outer bloom sample radius (higher = stronger + brighter) |

To change these, modify `m_hdr_power`, `m_radius1`, `m_radius2` in the plugin
source and rebuild. A future version could expose these via UEVR's ImGui overlay.

## Building

The plugin is built as part of the UEVR project:

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --target fakehdr_plugin
```

Output: `build/Release/FakeHDRPlugin.dll`

## Installation

1. Copy `FakeHDRPlugin.dll` to your UEVR game profile's `plugins/` folder
2. Launch the game with UEVR
3. FakeHDR is applied automatically to both eyes

## Limitations

- **D3D11 only** — D3D12 support is stubbed but not yet implemented
- Parameters are compile-time constants (no runtime UI yet)
- The effect is a simple 8-tap bloom approximation, not true HDR tonemapping

## Credits

- Original FakeHDR effect by Christian Cann Schuldt Jensen (CeeJay.dk)
- UEVR by praydog
