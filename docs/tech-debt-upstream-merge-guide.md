# Tech Debt & Upstream Merge Guide

## Overview

Our fork modifies **6 core upstream files** (plus adds new files). When praydog pushes updates, merging can range from trivial to painful depending on which files he touched. This document explains the risk per file, why D3D12Component.cpp is the worst, and how to handle merges.

## Will Praydog Change These Files?

**Yes, regularly.** UEVR is actively maintained. New Unreal Engine versions (5.5, 5.6, 6.0+) force updates to the rendering pipeline, stereo hooks, and D3D12 submission code. Praydog also fixes game-specific crashes, adds features, and hardens existing code.

### Upstream Activity (commits per file, all time)

| File | Total Commits | 2024 | 2025 | Our Hunks | Merge Risk |
|------|--------------|------|------|-----------|------------|
| FFakeStereoRenderingHook.cpp | 339 | ~50 | ~40 | 4 | **LOW** |
| D3D12Component.cpp | 81 | 2 | 13 | 20 | **HIGH** |
| Framework.cpp | 81 | ~15 | ~10 | 12 | **MEDIUM** |
| PluginLoader.cpp | 73 | ~15 | ~10 | 14 | **LOW-MEDIUM** |
| VR.cpp | 193 | ~30 | ~20 | ~3 | **LOW** |
| FFakeStereoRenderingHook.hpp | (same file) | — | — | 1 | **LOW** |

### What Triggers Upstream Changes

- **New UE versions** (5.5, 5.6, 6.0): stereo rendering API changes, new RHI backends, changed vtable layouts → FFakeStereoRenderingHook.cpp and D3D12Component.cpp
- **Game-specific crashes**: null pointer fixes, resource lifetime issues → D3D12Component.cpp, FFakeStereoRenderingHook.cpp
- **GPU driver updates**: Nvidia Streamline/DLSS changes, AMD FSR updates → D3D12Component.cpp
- **New features**: native stereo fix, new rendering modes, new plugin APIs → D3D12Component.cpp, PluginLoader.cpp
- **OpenXR/OpenVR updates**: swapchain handling, projection changes → D3D12Component.cpp, VR.cpp

## File-by-File Breakdown

### FFakeStereoRenderingHook.cpp — LOW RISK

Despite being praydog's most-changed file (339 commits), our modifications are **well-isolated**:

- **4 hunks only**: `#include <mutex>`, crash handler mode UI combo, new data structures at file scope, and the VEH handler rewrite at the bottom
- The VEH handler is a **single contiguous block** at the end of the file. Praydog's changes are almost always in the stereo hook scanning/setup functions (middle of the file)
- **Merge strategy**: Usually auto-merges cleanly. If conflict, it's in the UI section — easy to resolve.

### D3D12Component.cpp — HIGH RISK (Tech Debt)

This is the problem file. **280 lines added, 28 modified, scattered across 20 hunks inside `on_frame()`**.

**What it does**: Grabs the game's rendered image each frame and sends it to the VR headset via OpenXR/OpenVR.

**What we added**:
1. `safe_get_native_resource()` / `safe_get_resource_desc()` — SEH wrappers to survive null/invalid render targets during level transitions
2. `g_native_rt_tracking` — detects when UE4 reallocates the render target mid-session
3. `invalidate_rt_state` / `invalidate_ui_state` / `invalidate_scene_capture_state` — lambdas that reset cached state when RT changes
4. Plugin pre-render dispatch (`begin_plugin_pre_render` / `end_plugin_pre_render`) — lets DX12 shader plugins (FakeHDR) modify the image before VR submission
5. Various null-checks and early-returns sprinkled through `on_frame()`

**Why it's messy**: Instead of one clean entry point, these safety wrappers and the plugin dispatch are **interwoven into praydog's `on_frame()` function** at 20 different spots. Every time praydog changes `on_frame()` (which he does for crash fixes, new features, Streamline compat), there's a high chance of merge conflicts in code that's mixed with ours.

**Why we can't easily fix it**: The safety wrappers MUST surround specific resource accesses inside `on_frame()`. You can't just add a pre/post hook — the SEH protection needs to wrap individual `GetDesc()` and `get_native_resource()` calls that happen at specific points in praydog's render pipeline. The plugin pre-render dispatch similarly needs to fire at a precise point between "got the render target" and "copied to eye textures."

**Why it hasn't mattered yet**: This code only runs for DX12 games. The original target (Creatures of Ava) is DX11, so these changes never execute for it. But they're needed for FakeHDR and other DX12 shader plugins.

### Framework.cpp — MEDIUM RISK

- **12 hunks**, scattered but mostly in distinct areas
- Rehook limiter (always-on, prevents Streamline infinite rehook loop)
- UI drawing additions (crash handler dropdown, sidebar toggle)
- Praydog changes this file moderately often for new features
- **Merge strategy**: Most hunks are additive blocks. Conflicts likely in `on_draw_ui()` if praydog adds new UI elements nearby.

### PluginLoader.cpp — LOW-MEDIUM RISK

- **14 hunks** but most are **large additive blocks** (preset system, sidebar, pre-render callback plumbing)
- Only a few small insertions into existing functions
- Praydog adds new plugin API callbacks here regularly → could conflict with our callback additions
- **Merge strategy**: Additive blocks auto-merge well. Watch for conflicts in `g_plugin_callbacks` struct and callback registration functions.

### VR.cpp — LOW RISK

- **~3 hunks**, all additive (plugin pre-render dispatch calls)
- Just inserts `begin/end_plugin_pre_render()` calls in the right spots
- **Merge strategy**: Almost always auto-merges.

## How to Handle an Upstream Merge

### Step 1: Fetch and preview

```bash
git fetch upstream
git log --oneline HEAD..upstream/master  # see what changed
git log --oneline HEAD..upstream/master -- src/mods/vr/D3D12Component.cpp  # check the danger file
```

### Step 2: Attempt the merge

```bash
git merge upstream/master
```

### Step 3: If conflicts

**Easy files** (FFakeStereoRenderingHook, VR, PluginLoader): Open the conflict, keep both changes (ours are additive blocks that don't overlap with praydog's typical changes). Verify the merge makes sense.

**D3D12Component.cpp**: This is where it gets hard.

1. Open the conflicted file and read EVERY conflict marker carefully
2. Understand what praydog changed and why (read his commit messages)
3. Our SEH wrappers (`safe_get_native_resource`, `safe_get_resource_desc`) must continue to wrap the same resource access calls — if praydog moved or restructured those calls, the wrappers need to follow
4. The plugin pre-render dispatch (`begin_plugin_pre_render`/`end_plugin_pre_render`) must fire between "render target acquired" and "copy to eye textures" — if praydog restructured `on_frame()`, find the new correct insertion point
5. **Do a clean build with `--clean-first`** — D3D12Component.cpp layout changes can cause stale object files

### Step 4: Test

- DX11 game: verify crash handler still works (our primary use case)
- DX12 game (if available): verify FakeHDR/shader plugins still work, no crashes on level transitions

### Step 5: Update BASE_NIGHTLY

See [updating-base-nightly.md](updating-base-nightly.md).

## D3D12Component.cpp — Detailed Hunk Analysis

Our 20 hunks break down into 4 categories. Understanding this is critical for both merging and for the planned refactor.

### Category A — New code OUTSIDE existing functions (0 merge risk)

These are self-contained additions between praydog's functions. Git never conflicts on these.

1. **Anonymous namespace** at file top (lines 25-68 in our version):
   - `struct NativeRtTracking` + `g_native_rt_tracking` + `reset_native_rt_tracking()`
   - `safe_get_native_resource(FRHITexture2D*)` — SEH wrapper around `get_native_resource()`
   - `safe_get_resource_desc(ID3D12Resource*, D3D12_RESOURCE_DESC&)` — SEH wrapper around `GetDesc()`
   - **38 lines**, inserted before `on_frame()` definition

2. **Plugin pre-render methods** (between `on_post_present()` and `on_reset()`):
   - `begin_plugin_pre_render()` — opens CommandContext, waits for previous frame's GPU work
   - `end_plugin_pre_render()` — validates RT still exists, submits or discards command list
   - `get_plugin_command_list()` — returns the open command list for plugins to record into
   - `wait_for_plugin_gpu_work()` — fence wait, called before plugin unload
   - `prepare_plugin_rt(ID3D12Resource*)` — transitions scene RT from `ENGINE_SRC_COLOR` → `RENDER_TARGET`
   - `restore_plugin_rt(ID3D12Resource*)` — transitions back to `ENGINE_SRC_COLOR`
   - **~120 lines**, all new methods on D3D12Component

**Total Category A: ~160 of our 280 added lines. Will NEVER conflict.**

### Category B — 1-line mechanical replacements (almost always auto-merge)

These replace a single unsafe call with a safe wrapper on the same line. Git merges these correctly unless praydog changes that exact line.

| Hunk | What it replaces | Location |
|------|-----------------|----------|
| `safe_get_native_resource(ue4_texture)` | `(ID3D12Resource*)ue4_texture->get_native_resource()` | `on_frame()` backbuffer acquisition |
| `safe_get_native_resource(ue4_texture)` | same | `setup()` backbuffer acquisition |
| `safe_get_native_resource(ue4_texture)` | same | `OpenXR::create_swapchains()` |
| `safe_get_native_resource(scene_capture)` | `(ID3D12Resource*)scene_capture->get_native_resource()` | `on_frame()` scene capture |
| `safe_get_resource_desc(backbuffer, desc)` | `backbuffer->GetDesc()` | `on_frame()` texture setup |
| `safe_get_resource_desc(real_backbuffer, desc)` | `real_backbuffer->GetDesc()` | `setup()` |
| `safe_get_resource_desc(backbuffer, desc)` | `backbuffer->GetDesc()` | `setup()` |
| `safe_get_resource_desc(scene_depth_tex, desc)` | `scene_depth_tex->GetDesc()` | `on_reset()` |
| `ui_target_native` reuse (2 spots) | repeated `(ID3D12Resource*)ui_target->get_native_resource()` | `on_frame()` UI copy |
| `update_texture()` rename (2 spots) | `setup()` | `draw_spectator_view()`, `clear_backbuffer()` |

**Total Category B: ~12 hunks, each changing 1-3 lines. Low conflict risk.**

### Category C — Multi-line insertions INSIDE `on_frame()` (THE PROBLEM)

These are the hunks that cause painful merge conflicts. They're multi-line blocks inserted into the middle of praydog's control flow.

1. **Invalidation lambdas** (inserted right after `auto& hook = g_framework->get_d3d12_hook();`):
   ```cpp
   const auto& ffsr = VR::get()->m_fake_stereo_hook;  // moved from later
   auto invalidate_rt_state = [&](const char* reason) { ... };      // 10 lines
   auto invalidate_ui_state = [&](const char* reason) { ... };      // 10 lines
   auto invalidate_scene_capture_state = [&](const char* reason) { ... }; // 7 lines
   ```
   **34 lines inserted**

2. **RT change detection** (inserted after backbuffer acquisition):
   ```cpp
   const auto native_scene_rt = backbuffer.Get();
   if (native_scene_rt != nullptr && ... != g_native_rt_tracking.scene_rt) {
       invalidate_rt_state("UE render target native resource changed");
       return vr::VRCompositorError_None;
   }
   g_native_rt_tracking.scene_rt = native_scene_rt;
   ```
   **15 lines inserted**

3. **Scene capture change detection** (inside `if (vr->is_native_stereo_fix_enabled())`):
   ```cpp
   if (scene_capture != nullptr && scene_capture_rt == nullptr) { invalidate... }
   if (scene_capture_rt != nullptr && ... != g_native_rt_tracking.scene_capture_rt) { invalidate... }
   g_native_rt_tracking.scene_capture_rt = scene_capture_rt;
   ```
   **15 lines inserted**

4. **UI null-check restructure** (inside `if (ui_target != nullptr)`):
   - Added `if (ui_target_native == nullptr) { invalidate_ui_state(...); return; }`
   - Changed `native` variable to `ui_target_native` (computed earlier)
   - Removed the `else if (native == nullptr)` branch (redundant with early return)
   **~10 lines changed**

**Total Category C: 4 hunks, ~75 lines. These cause merge conflicts when praydog touches `on_frame()`.**

### Category D — Small additions to `on_reset()` and `setup()` (low risk)

- `on_reset()`: Added `m_last_checked_native = nullptr; reset_native_rt_tracking(); m_plugin_pre_render_ctx.reset(); m_plugin_pre_render_active = false;` — 5 lines
- `setup()`: Added `PluginLoader::get()->on_device_reset();` at end — 3 lines

**Total Category D: ~8 lines. Low conflict risk (end-of-function additions).**

## The Clean Refactor: `validate_frame_resources()`

When praydog next changes `on_frame()` and causes conflicts, perform this refactor to collapse Category C from 4 scattered multi-line blocks into 1 function call.

### The idea

Extract all RT acquisition + SEH protection + change detection + invalidation into a single method that returns a validated resource bundle. The `on_frame()` function gets ONE insertion instead of 4:

```cpp
// New struct — defined in D3D12Component.hpp or in the anonymous namespace
struct ValidatedFrameResources {
    ID3D12Resource* scene_native{};       // SEH-safe, change-detected
    ID3D12Resource* ui_native{};          // SEH-safe
    ID3D12Resource* scene_capture_native{};  // SEH-safe, change-detected
    D3D12_RESOURCE_DESC scene_desc{};     // SEH-safe GetDesc result
};

// New method on D3D12Component
// Returns nullopt if any critical resource faulted (already invalidated inside)
std::optional<ValidatedFrameResources> D3D12Component::validate_frame_resources(VR* vr) {
    const auto& ffsr = vr->m_fake_stereo_hook;
    ValidatedFrameResources res{};

    // --- Scene RT acquisition + SEH + change detection ---
    auto ue4_texture = ffsr->get_render_target_manager()->get_render_target();
    if (ue4_texture != nullptr) {
        res.scene_native = safe_get_native_resource(ue4_texture);
        if (res.scene_native == nullptr) {
            do_invalidate_rt_state(ffsr, "UE render target native resource is null or faulted");
            return std::nullopt;
        }
    }

    // RT pointer changed — UE reallocated
    if (res.scene_native != nullptr &&
        m_game_tex.texture.Get() != nullptr &&
        g_native_rt_tracking.scene_rt != nullptr &&
        res.scene_native != g_native_rt_tracking.scene_rt)
    {
        g_native_rt_tracking.scene_rt = res.scene_native;
        do_invalidate_rt_state(ffsr, "UE render target native resource changed");
        return std::nullopt;
    }
    if (res.scene_native != nullptr) {
        g_native_rt_tracking.scene_rt = res.scene_native;
    }

    // --- Scene RT descriptor ---
    if (res.scene_native != nullptr) {
        if (!safe_get_resource_desc(res.scene_native, res.scene_desc)) {
            do_invalidate_rt_state(ffsr, "backbuffer descriptor faulted");
            return std::nullopt;
        }
    }

    // --- UI target ---
    auto ui_target = ffsr->get_render_target_manager()->get_ui_target();
    res.ui_native = safe_get_native_resource(ui_target);
    if (ui_target != nullptr && res.ui_native == nullptr) {
        do_invalidate_ui_state(ffsr, "UI native resource is null or faulted");
        return std::nullopt;
    }

    // --- Scene capture (native stereo fix) ---
    if (vr->is_native_stereo_fix_enabled()) {
        auto scene_capture = ffsr->get_render_target_manager()->get_scene_capture_render_target();
        res.scene_capture_native = safe_get_native_resource(scene_capture);

        if (scene_capture != nullptr && res.scene_capture_native == nullptr) {
            do_invalidate_scene_capture_state(ffsr, "scene capture faulted");
        }

        if (res.scene_capture_native != nullptr &&
            m_scene_capture_tex.texture.Get() != nullptr &&
            g_native_rt_tracking.scene_capture_rt != nullptr &&
            res.scene_capture_native != g_native_rt_tracking.scene_capture_rt)
        {
            do_invalidate_scene_capture_state(ffsr, "scene capture changed");
        }
        g_native_rt_tracking.scene_capture_rt = res.scene_capture_native;
    }

    return res;
}
```

The `do_invalidate_*` helpers become private methods instead of lambdas:
```cpp
void D3D12Component::do_invalidate_rt_state(const FFakeStereoRenderingHook* ffsr, const char* reason) {
    SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] Invalidating D3D12 render target state: {}", reason);
    m_game_tex.reset();
    m_game_ui_tex.reset();
    m_scene_capture_tex.reset();
    m_last_checked_native = nullptr;
    m_force_reset = true;
    if (ffsr != nullptr) ffsr->set_should_recreate_textures(true);
}
// (same pattern for do_invalidate_ui_state, do_invalidate_scene_capture_state)
```

### What changes in `on_frame()` after the refactor

**Before (current, 20 hunks):**
```
on_frame():
  [praydog code]
  [34-line lambda block]          ← Category C hunk 1
  [praydog backbuffer acquisition]
  [our safe wrapper replacement]  ← Category B
  [15-line RT change detection]   ← Category C hunk 2
  [praydog code...]
  [our safe desc replacement]     ← Category B
  ... 15 more scattered hunks ...
```

**After refactor (~10 hunks):**
```
on_frame():
  [praydog code]
  auto resources = validate_frame_resources(vr);   ← ONE insertion (Category C gone)
  if (!resources) return vr::VRCompositorError_None;
  [praydog backbuffer acquisition — DELETED, moved into validate]
  [praydog code uses resources->scene_native]       ← Category B (1-line replacements remain)
  ... only 1-line replacements from here on ...
```

### Hunk count after refactor

| Category | Before | After | Notes |
|----------|--------|-------|-------|
| A (new methods outside functions) | ~4 hunks | ~5 hunks | +validate method + invalidate helpers |
| B (1-line replacements in on_frame) | ~12 hunks | ~8 hunks | Some eliminated (backbuffer acq moved) |
| C (multi-line blocks in on_frame) | **4 hunks** | **1 hunk** | The whole point |
| D (on_reset + setup additions) | ~2 hunks | ~2 hunks | Unchanged |
| **Total** | **~20** | **~14-16** | But only **1** problematic hunk vs 4 |

The critical improvement: the 4 multi-line blocks that cause merge conflicts (75 lines scattered through `on_frame()`) become 1 two-line insertion at the top.

### When to do this refactor

- **Trigger**: Next time praydog pushes a commit that touches `on_frame()` in D3D12Component.cpp and causes merge conflicts
- **Do NOT do it preemptively** — the current code works and is deployed
- **Requires**: DX12 game testing to validate (the refactor changes resource acquisition flow)
- **Build note**: Use `--clean-first` after this refactor — it changes the header (adds `ValidatedFrameResources` struct and new method declarations)

### Step-by-step execution plan

1. Resolve the merge conflict with praydog's changes FIRST (get his code working)
2. Add `ValidatedFrameResources` struct to D3D12Component.hpp (or anonymous namespace in .cpp)
3. Add `validate_frame_resources()`, `do_invalidate_rt_state()`, `do_invalidate_ui_state()`, `do_invalidate_scene_capture_state()` as methods
4. Replace the 4 Category C blocks in `on_frame()` with `auto resources = validate_frame_resources(vr);`
5. Update remaining Category B replacements to use `resources->scene_native` / `resources->ui_native` instead of local variables
6. Remove the `invalidate_*` lambdas and the `const auto& ffsr` line from `on_frame()` (now inside validate method)
7. Build with `--clean-first`, test on a DX12 game
8. Verify diff: should show ~14-16 hunks with only 1 multi-line insertion in `on_frame()`

## Summary

| Scenario | Impact |
|----------|--------|
| Praydog fixes a DX12 crash | **D3D12Component conflict likely**. Need manual merge of on_frame(). Consider doing the refactor at this point. |
| Praydog adds UE 5.6/6.0 stereo support | FFakeStereoRenderingHook changes, probably auto-merges (our VEH handler is at the bottom) |
| Praydog adds new plugin API | PluginLoader conflict possible in callback struct, easy to resolve |
| Praydog restructures on_frame() | **Worst case**. Every one of our 20 hunks needs manual repositioning. DO the refactor. |
| Epic ships new RHI backend | D3D12Component gets major surgery → our changes may need full rewrite for that file |

The honest assessment: **most merges will be fine**. The only real pain point is D3D12Component.cpp's `on_frame()`, and the `validate_frame_resources()` refactor is the clean fix — it collapses 4 scattered multi-line blocks into 1 two-line call at the top. Do it the next time praydog forces a conflict in that function.
