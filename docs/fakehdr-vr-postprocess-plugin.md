# FakeHDR VR Post-Process Plugin — Technical Documentation

A UEVR C++ plugin that applies CeeJay.dk's FakeHDR post-processing effect directly to VR eye textures. Unlike ReShade (which only affects the desktop mirror), this plugin modifies the UE render target **before** UEVR copies it to VR, so the effect is visible in-headset.

Required a new UEVR core API callback (`on_pre_render_vr_framework_dx11/dx12`) and several DX12 workarounds for UE's TYPELESS render targets and missing `ALLOW_RENDER_TARGET` flag.

## Problem Statement

ReShade's FakeHDR effect hooks the swapchain `Present()` call, which means it modifies the desktop backbuffer **after** UEVR has already copied the UE render target to VR eye textures. The VR headset never sees the effect.

### Why ReShade Can't Work in VR with UEVR

UEVR's render pipeline executes in this order inside `VR::on_present()`:

```
UE renders scene → UE render target
         │
         ▼
VR::on_present()
    │
    ├─ D3D12Component::on_frame()     ← copies UE RT to VR eye textures
    │       │
    │       ├─ CopyResource (UE RT → left eye)
    │       ├─ CopyResource (UE RT → right eye)
    │       └─ xrEndFrame / VRCompositor::Submit
    │
    ├─ PluginLoader::on_present()     ← plugin callbacks fire HERE
    │       │
    │       └─ ReShade hooks here too (via swapchain Present)
    │
    └─ IDXGISwapChain::Present()      ← desktop mirror
```

VR submission happens **before** any plugin `on_present` callback or ReShade hook. Modifying the backbuffer at this point only affects the desktop mirror window.

## Solution: Pre-VR-Submit Callback + Intermediate Texture Pipeline

### New UEVR Core API: `on_pre_render_vr_framework`

Added a new callback pair that fires **before** `D3D12Component::on_frame()` / `D3D11Component::on_frame()`:

```
UE renders scene → UE render target
         │
         ▼
VR::on_present()
    │
    ├─ on_pre_render_vr_framework_dx12()  ← NEW: plugin modifies UE RT
    │       │
    │       └─ FakeHDR processes the render target in-place
    │
    ├─ D3D12Component::on_frame()          ← copies MODIFIED RT to VR eyes
    │
    ├─ on_post_render_vr_framework_dx12()  ← existing: ImGui overlay
    │
    └─ PluginLoader::on_present()          ← desktop ImGui
```

#### Files Modified in UEVR Core

| File | Change |
|------|--------|
| `include/uevr/API.h` | Added `UEVR_OnPreRenderVRFrameworkDX11Cb` / `DX12Cb` typedefs, function pointer types, and entries in `UEVR_PluginCallbacks` struct |
| `include/uevr/Plugin.hpp` | Added `on_pre_render_vr_framework_dx11()` / `dx12()` virtual methods and callback registration in `uevr_plugin_initialize` |
| `src/Mod.hpp` | Added virtual method stubs for `on_pre_render_vr_framework_dx11()` / `dx12()` |
| `src/mods/PluginLoader.hpp` | Added storage vectors, `add_` methods, dispatch method declarations, callback list entries |
| `src/mods/PluginLoader.cpp` | Added namespace functions, `g_plugin_callbacks` entries, dispatch implementations, `add_` implementations |
| `src/mods/VR.cpp` | Added dispatch loop iterating `g_framework->get_mods()->get_mods()` right before `m_d3d11.on_frame(this)` / `m_d3d12.on_frame(this)` |

### DX12 Rendering Pipeline

The plugin cannot render directly into the UE render target because:

1. **TYPELESS format**: UE5 creates render targets as `DXGI_FORMAT_B8G8R8A8_TYPELESS` — D3D12 does not allow creating RTVs, SRVs, or PSOs with TYPELESS formats
2. **Missing ALLOW_RENDER_TARGET flag**: The UE render target's `D3D12_RESOURCE_DESC::Flags` does not include `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET` — creating an RTV for it causes a D3D12 runtime exception

The solution uses a **three-texture pipeline** with two intermediate textures:

```
UE Render Target (TYPELESS, no ALLOW_RENDER_TARGET)
    │
    │  CopyResource
    ▼
copy_tex (UNORM, SRV input, no special flags)
    │
    │  Fullscreen triangle draw (FakeHDR shader)
    ▼
result_tex (UNORM, ALLOW_RENDER_TARGET, our own texture)
    │
    │  CopyResource
    ▼
UE Render Target (restored to original state)
```

#### Resource State Transitions per Frame

```
Step 1: Copy UE RT → copy_tex
    UE RT:     RENDER_TARGET → COPY_SOURCE
    copy_tex:  PIXEL_SHADER_RESOURCE → COPY_DEST
    [CopyResource]

Step 2: Render FakeHDR from copy_tex → result_tex
    copy_tex:  COPY_DEST → PIXEL_SHADER_RESOURCE
    result_tex: already in RENDER_TARGET
    [DrawInstanced(3, 1, 0, 0)]

Step 3: Copy result_tex → UE RT
    result_tex: RENDER_TARGET → COPY_SOURCE
    UE RT:      COPY_SOURCE → COPY_DEST
    [CopyResource]

Step 4: Restore states
    result_tex: COPY_SOURCE → RENDER_TARGET
    UE RT:      COPY_DEST → RENDER_TARGET (initial_state)
```

#### DX12 Resources Created

| Resource | Type | Format | Flags | Purpose |
|----------|------|--------|-------|---------|
| `m_dx12_copy_tex` | Texture2D | Resolved UNORM | None | SRV input (scene snapshot) |
| `m_dx12_result_tex` | Texture2D | Resolved UNORM | `ALLOW_RENDER_TARGET` | RTV output for shader |
| `m_dx12_cb` | Buffer (256B) | Unknown | Upload heap | Persistently-mapped constant buffer |
| `m_dx12_srv_heap` | Descriptor heap | CBV_SRV_UAV | Shader-visible | SRV for copy_tex |
| `m_dx12_rtv_heap` | Descriptor heap | RTV | — | RTV for result_tex |
| `m_dx12_cmd_alloc` | Command allocator | Direct | — | Own allocator (not shared with UE) |
| `m_dx12_cmd_list` | Graphics command list | Direct | — | Records all barriers + draw |
| `m_dx12_fence` | Fence | — | — | CPU-GPU sync between frames |
| `m_dx12_root_sig` | Root signature | — | — | CBV(b0) + SRV table(t0) + static sampler |
| `m_dx12_pso` | Pipeline state | — | — | VS + PS, TRIANGLE, 1 RT |

### TYPELESS Format Resolution

UE5 commonly uses TYPELESS formats for render targets. The plugin resolves them to typed equivalents:

| TYPELESS Format | Resolved Format |
|----------------|-----------------|
| `R8G8B8A8_TYPELESS` | `R8G8B8A8_UNORM` |
| `B8G8R8A8_TYPELESS` | `B8G8R8A8_UNORM` |
| `B8G8R8X8_TYPELESS` | `B8G8R8X8_UNORM` |
| `R10G10B10A2_TYPELESS` | `R10G10B10A2_UNORM` |
| `R16G16B16A16_TYPELESS` | `R16G16B16A16_FLOAT` |
| `R32G32B32A32_TYPELESS` | `R32G32B32A32_FLOAT` |
| `R32_TYPELESS` | `R32_FLOAT` |
| `R16_TYPELESS` | `R16_FLOAT` |

## The FakeHDR Shader

Faithful port of CeeJay.dk's FakeHDR.fx from ReShade. The algorithm:

1. Sample the scene color at the current texel
2. Compute **bloom1**: 8 bilinear taps at offsets scaled by `Radius1`, weighted by 0.005
3. Compute **bloom2**: 8 bilinear taps at offsets scaled by `Radius2`, weighted by 0.010
4. Tone-map: `HDR = (color + bloom2 - bloom1) × (Radius2 - Radius1)`
5. Final: `output = pow(abs(HDR + color), abs(HDRPower)) + HDR`

### Tap Pattern (per bloom radius)

```
         ○ (0, -2.5)
        / \
(-1.5,-1.5) (1.5,-1.5)
      |       |
(-2.5, 0) ● (2.5, 0)     ● = center texel
      |       |
(-1.5, 1.5) (1.5, 1.5)
        \ /
         ○ (0, 2.5)
```

8 taps × 2 radii = **16 bilinear texture samples** per pixel.

### Default Parameters

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| HDR Power | 1.30 | 0.0 – 8.0 | Tone-mapping exponent |
| Radius 1 | 0.793 | 0.0 – 8.0 | Inner bloom sample radius (pixels × value) |
| Radius 2 | 0.87 | 0.0 – 8.0 | Outer bloom sample radius (pixels × value) |

### Shader Compilation

Shaders are compiled at runtime via `D3DCompile` (shader model 5.0). The HLSL source is embedded as string literals in the plugin — no external `.hlsl` files needed.

## ImGui Settings Panel

The plugin renders an ImGui window with:
- **Enable/Disable** checkbox (toggles effect without unloading)
- **HDR Power**, **Radius 1**, **Radius 2** sliders
- **Reset to Defaults** button

The ImGui frame is built in `on_pre_engine_tick` (game thread), then rendered:
- **Desktop**: via `on_present()` using the plugin_renderlib `g_d3d12` helpers
- **VR overlay**: via `on_post_render_vr_framework_dx12()` using UEVR's shared command list

## Performance Cost

### Per-Frame GPU Work

| Operation | Cost |
|-----------|------|
| 2× `CopyResource` (full-resolution texture copies) | Bandwidth-bound, ~0.1ms each at 3400×2000 |
| 1× Fullscreen triangle draw, 16 bilinear samples | ALU-trivial, bandwidth-bound, ~0.1ms |
| 4× `ResourceBarrier` transitions | Negligible |
| **Total** | **~0.3–0.5ms GPU time** at typical VR resolution |

### Per-Frame CPU Work

| Operation | Cost |
|-----------|------|
| Fence wait (`WaitForSingleObject`) | Blocks until previous frame's GPU commands complete |
| Command list record (Reset + barriers + draw + Close) | ~0.01ms |
| CB update (write 32 bytes to mapped upload buffer) | Negligible |

### Why It's Cheap

- The effect runs **once per frame**, not per-eye. It modifies the single UE render target before UEVR splits it into left/right copies
- The desktop mirror reads the same already-modified render target — no additional work
- No compute shaders, no UAVs, no multi-pass — single fullscreen triangle
- The constant buffer is persistently mapped (no `Map`/`Unmap` per frame)

### Comparison to ReShade FakeHDR

The visual result is **identical** at the same parameter values. The shader math is an exact port.

| | ReShade FakeHDR | UEVR FakeHDR Plugin |
|---|---|---|
| Injection point | Post-present (swapchain backbuffer) | Pre-VR-submit (UE render target) |
| Visible in VR? | No (desktop only) | Yes (both eyes + desktop) |
| Extra textures | 1 (backbuffer copy) | 2 (copy_tex + result_tex) |
| Command submission | ReShade framework | Own command allocator + list + fence |
| Shader compile | ReShade effect file parser | `D3DCompile` at runtime |
| Per-frame cost | ~0.2ms | ~0.3–0.5ms (extra copy for ALLOW_RENDER_TARGET workaround) |
| TYPELESS handling | Handled by ReShade | Manual `resolve_typeless_format()` |
| Settings UI | ReShade overlay | ImGui (VR + desktop) |

## Stability

### Frame Skip

The plugin skips the first 60 frames (`SKIP_FRAMES = 60`) after the pre-render callback starts firing. This avoids crashes during VR/D3D12 initialization when `D3D12Component::setup()` hasn't completed and the render target may be in an inconsistent state.

### SEH Protection

The `on_pre_render_vr_framework_dx12()` callback wraps the apply function in `__try/__except(EXCEPTION_EXECUTE_HANDLER)`. If a D3D12 runtime exception occurs, the plugin logs the exception code and **auto-disables** to prevent crash loops.

### HRESULT Checks

All command list operations (`Reset`, `Close`) check return values and bail early on failure.

## Evolution of the Fix

| Iteration | Approach | Result |
|-----------|----------|--------|
| 1 | D3D11 effect on swapchain backbuffer in `on_present()` | Worked on desktop, not visible in VR |
| 2 | D3D12 effect on backbuffer in `on_present()` | Same — VR submission happens before plugin callbacks |
| 3 | Effect in `on_post_render_vr_framework_dx12()` | Received IMGUI overlay RT, not the game scene |
| 4 | New `on_pre_render_vr_framework_dx12` callback in UEVR core | Plugin fires before VR copy — correct timing |
| 5 | Render directly into UE RT | Crash: format 90 (`B8G8R8A8_TYPELESS`) — can't create PSO/SRV/RTV with TYPELESS |
| 6 | Added `resolve_typeless_format()` helper | Crash persisted: UE RT lacks `ALLOW_RENDER_TARGET` flag — can't create RTV |
| 7 | **Three-texture pipeline** with own `result_tex` (ALLOW_RENDER_TARGET) | **Working in VR** |

## File Layout

```
examples/fakehdr_plugin/
    FakeHDRPlugin.cpp        — Complete plugin (~820 lines)

include/uevr/
    API.h                    — Pre-render callback types added to C API
    Plugin.hpp               — Pre-render virtual methods + registration

src/
    Mod.hpp                  — Virtual method stubs
    mods/
        PluginLoader.hpp     — Storage + dispatch declarations
        PluginLoader.cpp     — Namespace functions + dispatch + add_ implementations
        VR.cpp               — Dispatch loop before D3D11/D3D12 on_frame()

cmake.toml                   — [target.fakehdr_plugin] with type = "plugin"
```

## Build

```bash
cmake --build build --config Release --target fakehdr_plugin
```

Output: `build/Release/FakeHDRPlugin.dll`

Deploy to: `<game_profile>/plugins/FakeHDRPlugin.dll`

The UEVR core (`UEVRBackend.dll`) must also be rebuilt if the pre-render callback API changes are not already present:

```bash
cmake --build build --config Release --target uevr --clean-first
```
