## Branch Review: `feat/fakeHdr`

### Overall Assessment
This is a well-architected feature branch that adds a **full post-processing plugin pipeline** to UEVR with DX11/DX12 support, a preset system, and 13 shader effect plugins. The core GPU infrastructure (command list management, resource state transitions, plugin dispatch timing) is thoughtfully designed and reflects hard-won knowledge of D3D12 pitfalls.

---

### Strengths

1. **Correct plugin dispatch timing** — `on_pre_render_vr_framework` fires *before* `D3D12Component::on_frame()` copies the RT to eye textures, so effects are visible in VR. This is the right place in the pipeline.

2. **Single command queue** — Plugins record on UEVR's command list via `get_pre_render_command_list()` instead of managing their own queues. This avoids cross-queue synchronization nightmares, which your memory notes confirm caused BSODs in earlier iterations.

3. **SEH crash isolation** — `invoke_dx12_pre_render_callback_seh()` wraps DX12 plugin callbacks in `__try/__except`, correctly separated from C++ destructors. Good defensive pattern for third-party plugin code.

4. **GPU drain before `FreeLibrary`** — `wait_for_plugin_gpu_work()` in `attempt_unload_plugins()` prevents TDR/BSOD from destroying plugin-owned COM objects while the GPU is mid-flight.

5. **Resource state bracket** — `prepare_plugin_rt()` / `restore_plugin_rt()` ensures plugins always see `RENDER_TARGET` state and UEVR's copy sees `ENGINE_SRC_COLOR`. Clean contract.

6. **ABI-compatible API extension** — New callbacks (`on_pre_render`, `on_draw_ui`) appended at the end of `UEVR_PluginCallbacks` struct, preserving binary compatibility with existing plugins.

7. **`TextureContext::update_texture()`** — Reuses existing descriptor heaps instead of reallocating. Good performance optimization for per-frame backbuffer changes.

8. **`CommandContext::discard()`** + `recover_from_failed_execute()` — Proper fallback for when RT becomes invalid between dispatch and submission during level transitions.

---

### Issues to Address

#### Critical

1. **Backup file committed** — FFakeStereoRenderingHook - Copy.cpp.bak (8,471 lines) is checked into the branch. This is a full copy of an unrelated source file and should be removed from the repo.

2. **`delete_preset()` uses `remove_all()` on a path partially derived from user input** — The preset name comes from ImGui's `InputText` buffer or disk-enumerated directory names. While the paths are constructed via `presets_dir / name`, a maliciously crafted preset directory name containing `..` could escape the preset sandbox. Consider validating that the canonical preset path starts with the expected presets directory:
   ```cpp
   auto canonical = std::filesystem::canonical(preset_path);
   if (!canonical.string().starts_with(std::filesystem::canonical(presets_dir).string())) {
       // reject
   }
   ```

3. **Version number `2.420`** — API.h: `UEVR_PLUGIN_VERSION_MINOR 420`. This is a meme number that may confuse version comparison logic (master is at `39`). If this is a fork-only version, consider a different scheme (e.g. `100+` or a fork identifier) to avoid collisions with upstream.

#### Moderate

4. **Hardcoded personal path in deploy.sh** — deploy.sh: `DST="A:/UEVR/uevr 2026-01-13 (1127) - Mine"`. This should be parameterized or documented as needing local customization. Will fail for anyone else who clones the repo.

5. **`s_scene_rt_override` is thread-unsafe** — FFakeStereoRenderingFunctions.cpp: `s_scene_rt_override` is a raw static pointer with no synchronization. It's set/read on the render thread path, which is likely single-threaded in practice, but should have at minimum a comment documenting the threading assumption, or use `std::atomic`.

6. **`on_draw_ui_plugin_names` lifetime mismatch** — In PluginLoader.cpp L1965, `get_sidebar_entries()` creates `std::string` references from `m_on_draw_ui_plugin_names` under a `shared_lock`. But the returned `entries` vector contains copies (`SidebarEntryInfo` with `std::string`), so the lock is released before the strings are used in the framework's draw loop. This is actually fine due to the copy, but worth a comment since it looks like a use-after-lock-release at first glance.

7. **No sanitization on `m_preset_name_buf`** — The 128-byte ImGui text input is used directly as a filesystem directory name. Characters like `\`, `/`, `:`, `<`, `>`, `|`, `*`, `?` would create invalid paths on Windows. Should strip or reject invalid path characters before `save_preset()`.

#### Minor / Cleanup

8. **`PluginLoader::on_device_reset()` called from `D3D12Component::setup()`** — D3D12Component.cpp L1410: This creates a circular dependency (VR component → PluginLoader). It works because the include is already there, but consider whether this should be dispatched through the mod framework instead.

9. **DX11 pre-render has no SEH wrapper** — `on_pre_render_vr_framework_dx11()` uses only `try/catch(...)` while the DX12 path uses `__try/__except`. DX11 access violations from a bad plugin would crash the process. Consider matching the DX12 pattern.

10. **Empty virtual overrides in FakeHDRPlugin** — `on_present()`, `on_pre_engine_tick()`, `on_post_render_vr_framework_dx11/dx12()` are overridden with empty bodies. These already have empty defaults in Plugin.hpp — no need to override.

11. **Runtime shader compilation** — All 13 plugins compile HLSL at runtime via `D3DCompile()`. This adds startup latency (especially x13 plugins). Consider pre-compiling shaders as CSO blobs embedded in the DLL, with `D3DCompile()` as a fallback.

12. **File-scope statics for active preset tracking** — PluginLoader.cpp L2132-L2136: `s_active_preset_name`, `s_active_preset_dir`, etc. These avoid header changes (per your memory note about class layout sensitivity), which is the right call for this repo. Just note that this makes the state invisible to debuggers examining the `PluginLoader` instance.

13. **Missing newline at end of file** — FFakeStereoRenderingFunctions.cpp and CommandContext.cpp are missing trailing newlines.

---

### Summary

| Category | Count |
|----------|-------|
| Critical | 3 |
| Moderate | 4 |
| Minor | 6 |

The architecture is solid. The main concerns are: remove the `.bak` file before merge, sanitize preset names, and validate the `delete_preset` path. The GPU pipeline work (command list ownership, resource state management, crash recovery) is production-quality.