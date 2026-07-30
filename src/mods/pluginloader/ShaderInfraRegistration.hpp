// Shader-plugin host infrastructure: things called from inside PluginLoader
// methods that were originally inlined in upstream PluginLoader.cpp as fork
// additions. Moving them here keeps the upstream-owned file's diff small.

#pragma once

#include <vector>

#include <d3d11.h>

#include "uevr/API.h"

namespace uevr::shader_infra {

// Migrate legacy `<persistent>/data/plugins/*_settings.txt` into the
// `shader_settings/` subdirectory. Idempotent; safe to run every launch.
void migrate_shader_settings_dir();

// D3D11 baseline pipeline state for plugin dispatch. UE5 games can leave
// scissor test, depth test, or exotic blend modes active on the immediate
// context, which breaks fullscreen shader draws. We bind a known-good
// baseline before invoking plugin pre-render callbacks. PSOs are created
// once, then reused; on_device_reset() must call dx11_release_state().
bool dx11_ensure_state(ID3D11Device* device);
void dx11_bind_state(ID3D11DeviceContext* ctx);
void dx11_release_state();

// SEH wrapper around a single DX12 pre-render plugin callback.
// __try/__except cannot coexist with C++ objects that have destructors
// (e.g. std::shared_lock) in the same scope, so the dispatch loop calls
// this helper and treats `false` as "this callback faulted, log and skip".
bool dx12_invoke_pre_render_seh(UEVR_OnPreRenderVRFrameworkDX12Cb cb);

// Full pre-render dispatch: ensure+bind D3D11 baseline state, then iterate
// `cbs` invoking each via try/catch. Caller must hold the appropriate
// shared_lock on the callback list while this runs (so `cbs` stays stable).
void dispatch_pre_render_dx11(const std::vector<UEVR_OnPreRenderVRFrameworkDX11Cb>& cbs);

// Full pre-render dispatch for DX12: iterates `cbs` invoking each through
// the SEH wrapper. Caller holds the lock.
void dispatch_pre_render_dx12(const std::vector<UEVR_OnPreRenderVRFrameworkDX12Cb>& cbs);

} // namespace uevr::shader_infra
