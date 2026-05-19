# VR Post-Processing Shaders — Technical Documentation (last confirmed: 2026-05-10)

A suite of UEVR C++ shaders that apply ReShade-based post-processing effects directly to VR eye textures. Unlike ReShade (which only affects the desktop mirror), these shaders modify the UE render target **before** UEVR copies it to VR, so effects are visible in-headset.

> **Scope of this document:** technical/architecture reference for the shader plugin suite — DX11/DX12 pipeline, TYPELESS handling, the shared `EffectRuntime`, and the diff against upstream UEVR. For per-shader user-facing guidance (what each shader does, when to use it, performance tiers, sharpening comparison), see the [main README](../../README.md). For installation and preset paths, see [docs/reference/INSTALL.md](INSTALL.md). For per-shader upstream attribution, see the `*-LICENSE.txt` next to each `*Plugin.cpp`.

All shader plugins share the DX11/DX12 rendering pipeline and the new UEVR core API callbacks (`on_pre_render_vr_framework_dx11/dx12` and `on_draw_ui`) described below, plus several DX12 workarounds for UE's TYPELESS render targets and missing `ALLOW_RENDER_TARGET` flag.

## Plugin Load Order

Plugins are loaded by DLL filename in lexical order; each plugin's numeric prefix (`OUTPUT_NAME` in [cmake.toml](../cmake.toml)) encodes its position in the chain (scene shape → color correction → grading → AA cleanup → detail/sharpening → final cleanup). Each plugin also reports a sparse `render_order()` used by presets and the status UI. All shipped plugins ship disabled by default and run on the shared `EffectRuntime`.

## Problem Statement

ReShade-style post-process injectors hook the swapchain `Present()` call, which means they modify the desktop backbuffer **after** UEVR has already copied the UE render target to VR eye textures. The VR headset never sees the effect.

### Why ReShade-Style Injection Can't Work in VR with UEVR

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

## Solution: Pre-VR-Submit Callback + Shared Effect Runtime

Two layers cooperate:

1. **UEVR core** adds a pre-VR-submit callback pair (`on_pre_render_vr_framework_dx11/dx12`) plus an ImGui-injection callback (`on_draw_ui`). It also owns the per-dispatch resource-state bracket, the plugin command-list lifecycle, RT validation, SEH isolation, and device-reset notifications.
2. **`EffectRuntime`** (in `examples/renderlib/effects/`) is a shared per-plugin pass-graph runtime that sits on top of those callbacks. Plugins declare passes/RTs/textures and forward each `on_pre_render_vr_framework_dx{11,12}` into `runtime.execute()`. The runtime owns the scene snapshot, intermediate RTs, descriptor heaps / state caches, PSO compilation, per-pass barriers, and per-frame cadence bookkeeping.

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
| `src/mods/PluginLoader.hpp` | Added storage vectors, `add_` methods, dispatch method declarations, callback list entries for pre-render and draw_ui |
| `src/mods/PluginLoader.cpp` | Added namespace functions, `g_plugin_callbacks` entries, dispatch implementations, `add_` implementations; SEH wrapper (`invoke_dx12_pre_render_callback_seh`) isolates each plugin callback so a single AV doesn't crash the frame |
| `src/mods/VR.cpp` | Resource state bracketing around plugin dispatch (`prepare_plugin_rt` / `restore_plugin_rt`), RT validation gate, native stereo dispatch on single command list |
| `src/mods/vr/D3D12Component.hpp` | `prepare_plugin_rt()` / `restore_plugin_rt()` API, `begin/end_plugin_pre_render()`, `get_plugin_command_list()`, `m_plugin_pre_render_ctx` command context |
| `src/mods/vr/D3D12Component.cpp` | Resource state barrier implementations, `PluginLoader::on_device_reset()` call in `setup()` to notify plugins on pipeline rebuild, plugin command list lifecycle |
| `src/mods/vr/d3d12/CommandContext.cpp` | SEH-wrapped `execute()`, `recover_from_failed_execute()`, `discard()` for stale command list cleanup |
| `src/mods/vr/d3d12/TextureContext.cpp` | `update_texture()` for in-place descriptor heap reuse (avoids heap thrashing during backbuffer swaps) |

### New UEVR Core API: `on_draw_ui`

Added alongside the pre-render callbacks. `PluginLoader::on_draw_ui()` (called when the user opens the Plugins page in the UEVR sidebar) iterates all registered `on_draw_ui` callbacks after rendering its own plugin list. Plugins inject ImGui widgets into the UEVR menu without managing a separate ImGui context, so settings render on both the desktop and the VR overlay automatically. Same wiring shape as the pre-render callbacks: typedef in `include/uevr/API.h`, virtual in `include/uevr/Plugin.hpp`, vector + `add_` + dispatch in `src/mods/PluginLoader.{hpp,cpp}`.

## EffectRuntime

`EffectRuntime` (`examples/renderlib/effects/effect_runtime.{hpp,cpp,_d3d11.cpp,_d3d12.cpp}`) is the shared layer that every shipped shader plugin uses. It centralizes the DX11/DX12 boilerplate that was previously duplicated across plugins and scales from one-pass scene-in/scene-out effects up to multi-pass graphs with intermediate RTs, persistent histories, generated mip chains, and external PNG inputs. The header is the canonical reference; this section describes the model.

### Pass Graph Model

A plugin's effect is a list of `PassDesc`s plus optional `RTDesc` declarations:

- **Magic IDs**: `INPUT_SCENE` / `OUTPUT_SCENE` (= `-1`) refer to the scene RT supplied by UEVR; `declare_rt(...)` returns ids `0..N-1` for intermediate RTs; `load_external_texture_png(...)` returns ids `>= EXTERNAL_TEX_BASE`. All three id spaces are legal in `PassDesc::inputs`; `INPUT_SCENE` and an intermediate-RT id are legal in `PassDesc::output`.
- **Pass execution**: the runtime auto-snapshots the scene into a runtime-owned typed UNORM/FLOAT texture, then for each pass binds the requested input SRVs (and a per-pass cbuffer at `b0` if `cb_data`/`cb_size` are set), sets the output RTV, and issues a fullscreen-triangle draw. After the last pass the result is copied back into the scene RT (DX12) or written in place (DX11).
- **Vertex shader, sampler, root signature, PSO, descriptor heaps**: all owned and cached by the runtime. Plugins only supply pixel-shader HLSL.

### Render Target Declarations

`RTDesc` controls how an intermediate RT is sized and reused:

- `size_mode`: `Backbuffer` (matches scene size), `BackbufferDiv` (divides by `w_or_div` / `h_or_div`), or `Fixed` (literal `w_or_div` × `h_or_div`).
- `format`, `mip_levels`.
- `auto_generate_mips`: regenerate mips 1..N−1 after every pass that writes to mip 0 (DX11 uses `GenerateMips`; DX12 runs a PS-based 2× box-downsample chain).
- `persistent`: not freed between frames (identity-keyed on the scene RT).
- `shared_across_scene_slots`: allocated once at the backend level and shared across all per-eye scene slots. Used for persistent histories (e.g. `AdaptiveTonemapper`'s `LastAdapt`) so native-stereo-fix's dual scene-RT dispatch converges to a single shared value rather than producing per-eye drift. Per-frame scratch RTs (read+written within a single `execute()`) MUST remain per-slot.

A snapshot mip chain on the *scene* input is requested separately via `request_scene_snapshot_mips(n)`; the runtime regenerates those mips after the per-frame snapshot copy.

### Per-Frame Cadence

UEVR's renderer hook can fire more than once per HMD frame (native-stereo-fix dispatches the hook twice into the same command list, once per eye). The runtime tracks a per-frame dispatch counter, reset on every `on_present` (registered lazily by the runtime's first `set_passes()` call), and supports a per-pass `Cadence`:

- `Cadence::EveryDispatch` (default) — runs on every `execute()` call (every eye).
- `Cadence::OncePerFrame` — runs only on the first `execute()` of each swapchain frame. Outputs of these passes MUST live on `shared_across_scene_slots = true` RTs so the second-eye dispatch can read what the first wrote.

Plugins can also pass an explicit `pass_mask` to `execute(uint64_t)` for fine-grained control, but declarative `Cadence` is preferred so plugin code stays free of per-eye dispatch arithmetic.

### Scene-RT Colorspace Handling

DXGI provides no metadata to distinguish a linear `*_UNORM` RT from a gamma-encoded one. The runtime classifies each scene RT into `SceneRTColorSpace::{LinearFloat, SRGBTyped, AmbiguousUNORM, Unknown}` and exposes the result via `scene_rt_colorspace()` / `scene_rt_format_name()` for diagnostic UI.

Passes that need linear scene values for HDR-luminance math (e.g. `AdaptiveTonemapper`, `EyeAdaption`) opt in with `PassDesc::needs_scene_colorspace_decode = true`. When the scene is `AmbiguousUNORM`, the runtime injects `fx_decode_scene` / `fx_encode_scene` macros (`pow(c, 2.2)` / `pow(c, 1/2.2)`) into the HLSL preamble; for `LinearFloat` and `SRGBTyped` (where the typed view does sRGB conversion automatically) the macros are identity. The pass HLSL must wrap scene reads/writes accordingly. Default is `false` so format-agnostic LDR plugins do not regress.

### Single-Pass Wrapper

The 90% case — one HLSL string, scene-in, scene-out, one cbuffer struct — is wrapped by `SinglePassEffect<CB>`:

```cpp
struct MyCB { float a, b; float pad[2]; };
static const char* g_ps = R"(...)";
class MyPlugin : public uevr::Plugin {
    fx::SinglePassEffect<MyCB> m_fx{ g_ps };
    void on_initialize() override            { m_fx.init(); }
    void on_pre_render_vr_framework_dx11()   override { run(); }
    void on_pre_render_vr_framework_dx12()   override { run(); }
    void on_device_reset()                   override { m_fx.release_resources(); }
    void run() {
        if (!m_enabled) return;
        m_fx.set_cb({ m_a, m_b, {0,0} });
        m_fx.execute();
    }
};
```

Multi-pass plugins (e.g. `AdaptiveTonemapper`, `EyeAdaption`, `FXAA`, `FGFXLargeScalePerceptualObscuranceIrradiance`, `LUT`, retained `Bloom`) use `EffectRuntime` directly: declare RTs with `declare_rt`, build a `std::vector<PassDesc>`, and call `set_passes(std::move(passes))`.

### Threading Contract

`set_passes()` MUST be called from `Plugin::on_initialize()` (or any other non-renderer-hook context). The first call lazily registers an `on_present` callback via `cbs->on_present(...)`, which takes a writer lock on `PluginLoader::m_api_cb_mtx`. The renderer hooks (`on_pre/post_render_vr_framework_dx{11,12}`) and `on_present` itself hold a reader lock on that same mutex while iterating callbacks, so calling `set_passes()` from inside any of those would deadlock the same thread. The same constraint propagates through `SinglePassEffect::init()`.

### Plugin Responsibility Surface

Each plugin owns:

- Its HLSL pixel-shader source (string literal) and per-pass cbuffer layout.
- ImGui state + an `on_draw_ui()` body for the settings panel.
- Settings persistence via `uevr::settings::Serializable`.
- Forwarding `on_pre_render_vr_framework_dx{11,12}` → `runtime.execute()` and `on_device_reset()` → `runtime.release_resources()`.

Everything else — device/command-list acquisition, scene snapshot, TYPELESS resolution, RT/heap/PSO creation, per-pass state transitions, mip generation, per-frame cadence — is owned by the runtime.

### TYPELESS / ALLOW_RENDER_TARGET (DX12 constraints)

UE5 creates render targets with TYPELESS formats and without `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET`. D3D12 rejects RTV/SRV/PSO creation against TYPELESS resources, and rejects RTV creation entirely without `ALLOW_RENDER_TARGET`. The runtime works around both by allocating its own typed scene snapshot (with `ALLOW_RENDER_TARGET`), running the plugin's pass graph against runtime-owned RTs, and copying the final result back into the UE RT. DX11 has neither constraint — the runtime resolves typeless formats via `resolve_typeless_format()` and draws straight into the UE RT after taking a typed snapshot for the pass to sample from.

Resolved formats:

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

Plugins themselves are thin (see EffectRuntime → Plugin Responsibility Surface). The only crash-relevant code in a plugin is null-checking API contract values before forwarding into the runtime, and forwarding `on_device_reset()` so the runtime can drop its DX11/DX12 resources.

## File Layout

Deployed components:

- `examples/<name>_plugin/` — one folder per shipped shader plugin, each containing `<Name>Plugin.cpp` and a `*-LICENSE.txt`. `lut_plugin/` also ships an `assets/` PNG; `fakehdr_plugin/` keeps a separate `README.md` and `shaders/` directory.
- `examples/renderlib/effects/` — shared `EffectRuntime` source (built into every plugin DLL, not deployed standalone).
- `presets/` — shipping `.uevrpreset` files, deployed to `shipping_presets/`.
- `examples/example_plugin/` — developer example, not deployed.

UEVR-core diff against upstream praydog/UEVR is enumerated in [Files Modified in UEVR Core](#files-modified-in-uevr-core) above; see [cmake.toml](../cmake.toml) for the canonical plugin target list and [deploy.sh](../deploy.sh) for what gets packaged into a release.

## Build

```bash
cmd.exe //c "build.bat"
```

Output includes the release shader DLLs such as `00_FGFXLargeScalePerceptualObscuranceIrradianceShader.dll`, the color/detail/cleanup shader suite, and their license files. `20_BloomShader.dll` may also be built locally, but is retained as reference/stress-test material rather than shipped in the normal release set.

Deploy plugins + licenses + presets:
```bash
bash deploy.sh
```

Plugins are deployed to `%APPDATA%/UnrealVRMod/UEVR/plugins/` (global). Shipping `.uevrpreset` files are deployed to `%APPDATA%/UnrealVRMod/UEVR/data/plugins/shipping_presets/` (always overwritten — these are built-in, not user presets).
