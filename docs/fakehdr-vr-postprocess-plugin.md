# VR Post-Processing Shaders — Technical Documentation

13 UEVR C++ shaders that apply ReShade-based post-processing effects directly to VR eye textures. Unlike ReShade (which only affects the desktop mirror), these shaders modify the UE render target **before** UEVR copies it to VR, so effects are visible in-headset.

The plugin architecture, DX11/DX12 rendering pipeline, and UEVR core API changes were designed for FakeHDR (CeeJay.dk's FakeHDR.fx) and are shared by all 13 plugins.

Required new UEVR core API callbacks (`on_pre_render_vr_framework_dx11/dx12` and `on_draw_ui`) and several DX12 workarounds for UE's TYPELESS render targets and missing `ALLOW_RENDER_TARGET` flag.

## Shaders

### Color Correction

| # | Shader | Based On | What It Does | When to Use It |
|---|--------|----------|--------------|----------------|
| 01 | LevelsPlus | Levels.fx (prod80) | Black/white point, per-channel gamma, ACES tone mapping | **Fix grey/washed-out blacks** — the #1 VR problem. Start here. |
| 02 | LiftGammaGain | LiftGammaGain.fx (prod80) | Shadow/midtone/highlight RGB lift, gamma, gain | Fine-tune shadow/midtone/highlight color separately. Use when LevelsPlus isn't enough. |
| 03 | Tonemap | Tonemap.fx (prod80) | Gamma, exposure, saturation, bleach bypass, defog | Adjust overall brightness/saturation. Bleach bypass for desaturated film look. Defog to remove haze. |

### Color Grading

| # | Shader | Based On | What It Does | When to Use It |
|---|--------|----------|--------------|----------------|
| 04 | Curves | Curves.fx (CeeJay.dk) | Luma/chroma contrast S-curve with multiple formulas | Add contrast — brights get brighter, darks get darker. Subtle but effective. |
| 05 | FakeHDR | FakeHDR.fx (CeeJay.dk) | Local tone mapping via dual-radius bloom | "HDR look" — brightens dark details without blowing out highlights. |
| 06 | DPX | DPX.fx (Loadus) | Cineon film stock color emulation | Warm cinematic color shift. Good for games that look too cold/digital. |
| 07 | Technicolor | Technicolor2.fx (prod80) | 2-strip Technicolor color grading | Old Hollywood look — teal shadows, warm highlights. Use sparingly. |
| 08 | Colourfulness | Colourfulness.fx (prod80) | Saturation enhancement with luma limiting | Boost saturation without clipping already-saturated colors. |
| 09 | Vibrance | Vibrance.fx (Jeanseb) | Intelligent saturation boost (boosts dull colors more) | Make dull games pop without oversaturating skin tones. |
| 11 | HSL Shift | HSLShift.fx (kingeric1992) | Per-hue color remapping (8 color zones) | Remap individual hues — e.g. shift reds toward orange, make greens more vivid. |
| 12 | Filmic Pass | FilmicPass.fx (ReShade standard) | Sigmoid curves, bleach bypass, fade, per-channel gamma | Full cinematic color processing — more control than Tonemap for specific film looks. |

### Detail & Film Effects

| # | Shader | Based On | What It Does | When to Use It |
|---|--------|----------|--------------|----------------|
| 10 | FilmGrain2 | FilmGrain2.fx (Martins Upitis) | Photographic film grain overlay | Hide color banding in dark areas (common on VR panels). Keep subtle. |
| 13 | Clarity | Clarity.fx (Ioxa) | Local contrast enhancement with blend mode selection | Makes textures/details pop without changing colors. **Very effective in VR** where things look flat. |

All plugins share the same architecture:
- DX11 and DX12 dual-path rendering
- 2-slot texture cache (copy + result) with `PointSampler` and fullscreen triangle vertex shader
- Copy→draw pattern: copy scene to SRV texture, run pixel shader, copy result back
- Runtime `D3DCompile` — no pre-compiled `.cso` files needed
- ImGui settings panel in the UEVR menu sidebar (via `on_draw_ui` callback)
- Auto-save settings per game to `%APPDATA%/UnrealVRMod/<game>/data/plugins/<name>_settings.txt`
- All disabled by default (`m_enabled = false`)

### Plugin Load Order

Plugins are loaded in DLL name alphabetical order. Numeric prefixes (`01_` through `13_`) ensure:
- Color correction runs first (01–03)
- Color grading runs in the middle (04–09)
- Detail & film effects run last (10–13): FilmGrain2 → HSL Shift → Filmic Pass → Clarity

### Preset System

The PluginLoader manages a preset system for saving/loading all plugin settings at once:
- **Local presets**: `%APPDATA%/UnrealVRMod/<game>/data/plugins/presets/<name>/` — per-game
- **Global presets**: `%APPDATA%/UnrealVRMod/uevr/data/plugins/presets/<name>/` — shared across games
- **Built-in presets**: `%APPDATA%/UnrealVRMod/UEVR/data/plugins/shipping_presets/` — read-only, overwritten on update
- Each preset folder contains copies of all `*_settings.txt` files
- Active preset tracking persists per game via `active_preset.txt`
- Ships with 6 built-in presets (All Off, VR Fix - Black Levels, VR Essentials, Cinematic, Vivid, HDR Look)

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

Added a new callback pair that fires **before** `D3D12Component::on_frame()` / `D3D11Component::on_frame()`.

The DX11 and DX12 paths have different complexity levels:
- **DX11**: No resource state management needed — D3D11 tracks states implicitly. Plugin gets the immediate context, copies, draws, and restores pipeline state.
- **DX12**: UEVR core brackets each dispatch with explicit resource state transitions. Plugin records commands on UEVR's command list.

#### DX11 Pipeline

```
UE renders scene → UE render target
         │
         ▼
VR::on_present()
    │
    ├─ on_pre_render_vr_framework_dx11()  ← plugin modifies UE RT
    │       │
    │       └─ [if native stereo: dispatched again for capture RT]
    │
    ├─ D3D11Component::on_frame()          ← copies MODIFIED RT to VR eyes
    │
    ├─ on_post_render_vr_framework_dx11()  ← existing: ImGui overlay
    │
    └─ PluginLoader::on_present()          ← desktop ImGui
```

#### DX12 Pipeline

```
UE renders scene → UE render target
         │
         ▼
VR::on_present()
    │
    ├─ begin_plugin_pre_render()           ← open command list + fence wait
    │       │
    │       ├─ prepare_plugin_rt(main_rt)  ← ENGINE_SRC_COLOR → RENDER_TARGET
    │       │
    │       ├─ on_pre_render_vr_framework_dx12()  ← plugin modifies UE RT
    │       │
    │       ├─ restore_plugin_rt(main_rt)  ← RENDER_TARGET → ENGINE_SRC_COLOR
    │       │
    │       ├─ [if native stereo:]
    │       │   ├─ prepare_plugin_rt(capture_rt) ← state transition
    │       │   ├─ on_pre_render_vr_framework_dx12()  ← plugin processes second eye
    │       │   └─ restore_plugin_rt(capture_rt) ← state transition
    │       │
    │       └─ end_plugin_pre_render()     ← submit + validate RT still exists
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
| `include/uevr/API.h` | Added `UEVR_OnPreRenderVRFrameworkDX11Cb` / `DX12Cb` and `UEVR_OnDrawUICb` typedefs, function pointer types, and entries in `UEVR_PluginCallbacks` struct |
| `include/uevr/Plugin.hpp` | Added `on_pre_render_vr_framework_dx11()` / `dx12()` and `on_draw_ui()` virtual methods and callback registration in `uevr_plugin_initialize` |
| `src/Mod.hpp` | Added virtual method stubs for `on_pre_render_vr_framework_dx11()` / `dx12()` |
| `src/mods/PluginLoader.hpp` | Added storage vectors, `add_` methods, dispatch method declarations, callback list entries for pre-render and draw_ui |
| `src/mods/PluginLoader.cpp` | Added namespace functions, `g_plugin_callbacks` entries, dispatch implementations, `add_` implementations; SEH wrapper (`invoke_dx12_pre_render_callback_seh`) isolates each plugin callback so a single AV doesn't crash the frame |
| `src/mods/VR.cpp` | Resource state bracketing around plugin dispatch (`prepare_plugin_rt` / `restore_plugin_rt`), RT validation gate, native stereo dispatch on single command list |
| `src/mods/vr/D3D12Component.hpp` | `prepare_plugin_rt()` / `restore_plugin_rt()` API, `begin/end_plugin_pre_render()`, `get_plugin_command_list()`, `m_plugin_pre_render_ctx` command context |
| `src/mods/vr/D3D12Component.cpp` | Resource state barrier implementations, `PluginLoader::on_device_reset()` call in `setup()` to notify plugins on pipeline rebuild, plugin command list lifecycle |
| `src/mods/vr/d3d12/CommandContext.cpp` | SEH-wrapped `execute()`, `recover_from_failed_execute()`, `discard()` for stale command list cleanup |
| `src/mods/vr/d3d12/TextureContext.cpp` | `update_texture()` for in-place descriptor heap reuse (avoids heap thrashing during backbuffer swaps) |

### DX12 Rendering Pipeline

The plugin cannot render directly into the UE render target on DX12 because:

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

UEVR core ensures the UE render target is in `RENDER_TARGET` state when plugins are called
(via `prepare_plugin_rt()`) and restores it to `ENGINE_SRC_COLOR` afterwards (via `restore_plugin_rt()`).
This means:
- **Plugins can safely assume the RT starts in `RENDER_TARGET` state**
- UEVR's `on_frame()` copy always sees the RT in `ENGINE_SRC_COLOR` (shader resource) state
- There is no resource state mismatch — the D3D12 spec requirement that "before and after states of consecutive ResourceBarrier calls must agree" is always satisfied

The FakeHDR plugin's internal transitions (within the RENDER_TARGET → RENDER_TARGET bracket):

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
    UE RT:      COPY_DEST → RENDER_TARGET (returned to UEVR bracket state)
```

#### DX12 Resources Created

| Resource | Type | Format | Flags | Purpose |
|----------|------|--------|-------|---------|
| `m_dx12_copy_tex` | Texture2D | Resolved UNORM | None | SRV input (scene snapshot) |
| `m_dx12_result_tex` | Texture2D | Resolved UNORM | `ALLOW_RENDER_TARGET` | RTV output for shader |
| `m_dx12_cb` | Buffer (256B) | Unknown | Upload heap | Persistently-mapped constant buffer |
| `m_dx12_srv_heap` | Descriptor heap | CBV_SRV_UAV | Shader-visible | SRV for copy_tex |
| `m_dx12_rtv_heap` | Descriptor heap | RTV | — | RTV for result_tex |
| `m_dx12_root_sig` | Root signature | — | — | CBV(b0) + SRV table(t0) + static sampler |
| `m_dx12_pso` | Pipeline state | — | — | VS + PS, TRIANGLE, 1 RT |

The plugin does **not** own any command infrastructure (allocator, command list, fence, queue). UEVR provides the command list via `get_pre_render_command_list()` and manages its full lifecycle (open, submit, fence, reset).

### DX11 Rendering Pipeline

The DX11 path is simpler than DX12:
- **No explicit resource state management** — D3D11 handles state tracking internally
- **No TYPELESS format issues for the draw** — D3D11 allows creating RTVs/SRVs with explicit format on a typeless resource (the plugin uses `resolve_typeless_format()` to supply the correct typed format)
- **No `ALLOW_RENDER_TARGET` issue** — D3D11 render target creation is more permissive
- **Two-texture pipeline** — no intermediate `result_tex` needed; the FakeHDR shader draws directly back to the target via a typed RTV

```
UE Render Target (may be TYPELESS)
    │
    │  CopyResource
    ▼
copy_tex (typed UNORM, SRV input)
    │
    │  Fullscreen triangle draw (FakeHDR shader)
    ▼
UE Render Target (typed RTV, drawn in-place)
```

The plugin saves and restores all D3D11 pipeline state (render targets, viewport, shaders, samplers, SRVs, constant buffers, topology, input layout) around the draw to avoid corrupting UEVR or UE's rendering state.

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

The plugin draws its settings directly inside the **UEVR menu** via the new `on_draw_ui` plugin callback. Settings appear under a **"FakeHDR Settings"** collapsing header on the **Plugins** sidebar page (accessible via `Insert` key → Plugins).

Widgets:
- **Enable/Disable** checkbox (toggles effect without unloading)
- **HDR Power**, **Radius 1**, **Radius 2** `InputFloat` fields with **-/+** step buttons (step = 0.01, fast step = 0.1)
- **Reset to Defaults** button

Because the UI is part of UEVR's own ImGui frame, the plugin does not manage its own ImGui context, window, or rendering pipeline. The settings are visible on both the desktop and the VR overlay automatically.

### New UEVR Core API: `on_draw_ui`

Added alongside the pre-render callbacks. `PluginLoader::on_draw_ui()` (called when the user opens the Plugins page in the UEVR sidebar) now iterates all registered `on_draw_ui` callbacks after rendering its own plugin list. This lets any C++ plugin inject ImGui widgets into the UEVR menu without managing a separate ImGui context.

| File | Change |
|------|--------|
| `include/uevr/API.h` | Added `UEVR_OnDrawUICb` / `UEVR_OnDrawUIFn` typedefs and entry in `UEVR_PluginCallbacks` |
| `include/uevr/Plugin.hpp` | Added `on_draw_ui()` virtual method and registration lambda |
| `src/mods/PluginLoader.hpp` | Added `m_on_draw_ui_cbs` vector, `add_on_draw_ui()` method, callback list entry |
| `src/mods/PluginLoader.cpp` | Added `uevr::on_draw_ui` namespace function, `g_plugin_callbacks` entry, `add_on_draw_ui` impl, dispatch loop in `on_draw_ui()` |

## Performance Cost

### Per-Frame GPU Work

| Operation | Cost |
|-----------|------|
| 2× `ResourceBarrier` (UEVR state bracketing: ENGINE_SRC_COLOR ↔ RENDER_TARGET) | Negligible (GPU state metadata, no pipeline stall) |
| 2× `CopyResource` (full-resolution texture copies) | Bandwidth-bound, ~0.1ms each at 3400×2000 |
| 1× Fullscreen triangle draw, 16 bilinear samples | ALU-trivial, bandwidth-bound, ~0.1ms |
| 4× `ResourceBarrier` (plugin internal transitions) | Negligible |
| **Total (DX12)** | **~0.3–0.5ms GPU time** at typical VR resolution |

The DX11 path is slightly cheaper:

| Operation | Cost |
|-----------|------|
| 1× `CopyResource` (scene snapshot to copy_tex) | ~0.1ms |
| 1× Fullscreen triangle draw | ~0.1ms |
| Pipeline state save/restore | Negligible |
| **Total (DX11)** | **~0.2–0.3ms GPU time** |

### Per-Frame CPU Work

| Operation | Cost |
|-----------|------|
| Fence wait (`WaitForSingleObject` in `begin_plugin_pre_render`) | Blocks until previous frame's plugin GPU commands complete |
| RT validation (pointer dereferences in `end_plugin_pre_render`) | Negligible |
| SEH wrappers (`invoke_dx12_pre_render_callback_seh`, `execute_command_list_seh`) | **Zero cost on success path** — x64 Windows uses table-based unwinding, no frame setup or teardown in the non-exception path |
| Command list record (barriers + draw, no Reset/Close — UEVR manages lifecycle) | ~0.01ms |
| CB update (write 32 bytes to mapped upload buffer) | Negligible |

### Why It's Cheap

- The effect runs **once per frame** per eye RT (twice with native stereo). It modifies the UE render target before UEVR copies it to VR eyes
- The desktop mirror reads the same already-modified render target — no additional work
- No compute shaders, no UAVs, no multi-pass — single fullscreen triangle
- The constant buffer is persistently mapped (no `Map`/`Unmap` per frame)
- SEH exception handling on x64 Windows is truly zero-overhead in the success path (unlike x86 SEH which pushes/pops frame records)
- All safety infrastructure (RT validation, SEH, discard path) involves no GPU work and no allocations — only pointer checks and state flags

### No Memory Leaks

- `recover_from_failed_execute()` resets the command allocator+list back to an open state after SEH catches a failure — no orphaned GPU objects
- `discard()` resets without submitting — recorded commands (barriers, draws) are freed with the allocator reset; referenced resources (copy_tex, result_tex) stay alive in plugin ComPtrs
- `prepare_plugin_rt()` / `restore_plugin_rt()` allocate nothing — they record barriers on the existing command list
- `on_device_reset()` triggers `release_effect_resources()` → `.Reset()` on all ComPtrs and target states
- `TextureContext::update_texture()` does `texture.Reset()` before assigning the new resource — no leaked reference count

### Comparison to ReShade FakeHDR

The visual result is **identical** at the same parameter values. The shader math is an exact port.

| | ReShade FakeHDR | UEVR FakeHDR Plugin (DX12) | UEVR FakeHDR Plugin (DX11) |
|---|---|---|---|
| Injection point | Post-present (swapchain backbuffer) | Pre-VR-submit (UE render target) | Pre-VR-submit (UE render target) |
| Visible in VR? | No (desktop only) | Yes (both eyes + desktop) | Yes (both eyes + desktop) |
| Extra textures | 1 (backbuffer copy) | 2 (copy_tex + result_tex) | 1 (copy_tex) |
| Command submission | ReShade framework | UEVR-managed command list | Immediate context |
| Shader compile | ReShade effect file parser | `D3DCompile` at runtime | `D3DCompile` at runtime |
| Per-frame cost | ~0.2ms | ~0.3–0.5ms (extra copy for ALLOW_RENDER_TARGET workaround) | ~0.2–0.3ms |
| TYPELESS handling | Handled by ReShade | Manual `resolve_typeless_format()` | Manual `resolve_typeless_format()` |
| Settings UI | ReShade overlay | UEVR menu (Plugins page) | UEVR menu (Plugins page) |

## Stability

### UEVR Core Safety Nets

The following protections are built into UEVR's plugin dispatch pipeline:

| Layer | Mechanism | What it prevents |
|-------|-----------|-----------------|
| **Resource state management** | `prepare_plugin_rt()` / `restore_plugin_rt()` bracket each dispatch with `ENGINE_SRC_COLOR ↔ RENDER_TARGET` transitions | GPU hangs / TDR from resource state mismatch (plugins hardcoding RENDER_TARGET as "before" state) |
| **RT validation gate** | `VR.cpp` checks `is_using_2d_screen()` and resolves the native RT pointer before dispatch | Plugin dispatch with stale/null RT during 2D mode or level loads |
| **RT validation on submit** | `end_plugin_pre_render()` re-validates the RT before `execute()` — calls `discard()` if RT was destroyed between callback and submission | Submitting D3D12 commands referencing freed resources |
| **Plugin dispatch gate** | `is_plugin_dispatch_allowed()` returns false while `m_force_reset` is set (pipeline being rebuilt) | Plugin dispatch before D3D12 pipeline is ready |
| **Plugin reset notification** | `D3D12Component::setup()` calls `PluginLoader::on_device_reset()` after pipeline rebuild | Stale plugin-side resources (cached textures, descriptor heaps) after renderer reset |
| **SEH on callbacks** | `invoke_dx12_pre_render_callback_seh()` wraps each plugin callback in `__try/__except` | One crashing plugin taking down the entire frame |
| **SEH on command submit** | `CommandContext::execute()` wraps Close+ExecuteCommandLists+Signal in `__try/__except` | D3D12 runtime exceptions during GPU submission |
| **Command list recovery** | `recover_from_failed_execute()` resets allocator+list to clean open state after a failed submit | Permanently broken command context after a single failure |
| **Command list discard** | `discard()` cleans up without submitting when the RT was invalidated mid-frame | Stale GPU commands after scene transitions |

### Plugin-Side Protection

The plugin itself is deliberately simple — all crash protection and lifecycle management lives in UEVR core. The plugin only implements:

#### Null Checks

Standard API contract validation — null checks on `scene_rt`, `native`, `device`, `cmd` return values before proceeding.

#### HRESULT Checks

All D3D12 resource creation (`CreateCommittedResource`, `CreateDescriptorHeap`, etc.) and pipeline init (`CreateGraphicsPipelineState`, `CreateRootSignature`) check return values and bail early on failure.

#### Device Reset Handling

The plugin implements `on_device_reset()` which calls `release_effect_resources()` — clearing the PSO, root signature, constant buffer, and all per-target-state cached textures/heaps. This is triggered both by the original Framework reset path and by `D3D12Component::setup()` pipeline rebuilds.

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
| 8 | Integrated UI into UEVR menu via new `on_draw_ui` plugin callback | Settings in Plugins sidebar page, no separate window |
| 9 | SEH wrappers + RT validation gate for 2D mode/transition crashes | Prevented AVs during meditation/level loads in Jedi Survivor |
| 10 | `TextureContext::update_texture()` for in-place heap reuse | Eliminated descriptor heap thrashing during backbuffer changes |
| 11 | **Resource state bracketing** (`prepare/restore_plugin_rt`) | **Fixed TDR/system reboot** — root cause was `ENGINE_SRC_COLOR` vs `RENDER_TARGET` state mismatch between UEVR and plugins |
| 12 | `PluginLoader::on_device_reset()` from `D3D12Component::setup()` | Plugins clear stale caches when UEVR rebuilds the D3D12 pipeline |
| 13 | **DX11: scene render target** via `StereoHook::get_scene_render_target()` | DX11 path now applies to UE RT instead of swapchain backbuffer — **working in VR on DX11 games** |

## File Layout

```
examples/
    colourfulness_plugin/    — ColourfulnessPlugin.cpp + LICENSE
    curves_plugin/           — CurvesPlugin.cpp + LICENSE
    dpx_plugin/              — DPXPlugin.cpp + LICENSE
    fakehdr_plugin/          — FakeHDRPlugin.cpp + README.md + LICENSE + shaders/
    filmgrain2_plugin/       — FilmGrain2Plugin.cpp + LICENSE
    levelsplus_plugin/       — LevelsPlusPlugin.cpp + LICENSE
    liftgammagain_plugin/    — LiftGammaGainPlugin.cpp + LICENSE
    technicolor_plugin/      — TechnicolorPlugin.cpp + LICENSE
    tonemap_plugin/          — TonemapPlugin.cpp + LICENSE
    vibrance_plugin/         — VibrancePlugin.cpp + LICENSE
    hslshift_plugin/         — HSLShiftPlugin.cpp + LICENSE
    filmicpass_plugin/       — FilmicPassPlugin.cpp + LICENSE
    clarity_plugin/          — ClarityPlugin.cpp + LICENSE

presets/                     — Shipping presets (6 folders, 13 settings files each)

include/uevr/
    API.h                    — Pre-render + draw_ui callback types added to C API
    Plugin.hpp               — Pre-render + draw_ui virtual methods + registration

src/
    Framework.cpp            — Sidebar alignment (left-aligned + sub-entry indentation)
    Mod.hpp                  — Virtual method stubs
    mods/
        PluginLoader.hpp     — Storage + dispatch + preset system declarations
        PluginLoader.cpp     — Plugin dispatch + preset save/load/delete + status display + on_draw_ui
        VR.cpp               — Resource state bracketing + RT validation gate + dispatch loop
        vr/
            D3D12Component.hpp — prepare/restore_plugin_rt API + plugin command context
            D3D12Component.cpp — Barrier implementations + on_device_reset notification in setup()
            d3d12/
                CommandContext.cpp  — SEH-wrapped execute + recover + discard
                TextureContext.cpp  — update_texture() for in-place heap reuse

cmake.toml                   — All 13 plugin targets with numeric-prefixed OUTPUT_NAMEs
deploy.sh                    — Build deployment script (DLLs + plugins + licenses + presets)
```

## Build

```bash
# Build a single plugin
cmake --build build --config Release --target fakehdr_plugin

# Build all plugins (targets defined in cmake.toml)
cmake --build build --config Release

# Rebuild UEVR core (needed if API headers changed)
cmake --build build --config Release --target uevr --clean-first
```

Output: `build/Release/01_LevelsPlusShader.dll` through `build/Release/13_ClarityShader.dll`

Deploy plugins + licenses + presets:
```bash
bash deploy.sh
```

Plugins are deployed to `%APPDATA%/UnrealVRMod/UEVR/plugins/` (global). Shipping presets are deployed to `%APPDATA%/UnrealVRMod/UEVR/shipping_presets/` (always overwritten — these are built-in, not user presets).
