// Plugin pre-render command list for D3D12.
//
// Owns the dedicated d3d12::CommandContext that plugins record into during
// on_pre_render_vr_framework_dx12(), plus the dispatch-allowed gate and the
// RT bracketing barriers.  Lives here (not on vrmod::D3D12Component) so the
// upstream-owned D3D12Component header stays close to praydog/UEVR — see
// docs/active/shader-infra-refactor-plan.md §3.
//
// D3D12Component holds a single PluginCommandListD3D12 by value and forwards
// the three lifecycle hooks (on_reset / on_force_reset / on_setup_complete);
// every other call site lives in src/mods/pluginloader/ already.

#pragma once

#include <d3d12.h>

#include "../vr/d3d12/CommandContext.hpp"

class VR;

namespace uevr {

class PluginCommandListD3D12 {
public:
    // Lifecycle hooks called by vrmod::D3D12Component:
    //  - on_reset(): drain in-flight GPU work and release D3D12 resources.
    //  - on_force_reset(): suspend plugin dispatch until on_setup_complete().
    //  - on_setup_complete(): re-enable plugin dispatch after a successful
    //    D3D12Component::setup().
    void on_reset();
    void on_force_reset() noexcept { m_dispatch_allowed = false; }
    void on_setup_complete() noexcept { m_dispatch_allowed = true; }

    // Plugin dispatch gate (callers ask before begin()).
    bool is_allowed() const noexcept { return m_dispatch_allowed; }

    // Per-present frame: open the cmd list, then close+submit at end.
    // begin() lazy-creates the underlying CommandContext on first call.
    void begin();
    void end();

    // Underlying ID3D12GraphicsCommandList* for plugins to record into.
    // Returns nullptr if begin() was not called or context is not ready.
    ID3D12GraphicsCommandList* get();

    // RT state bracketing around a plugin dispatch.  Transitions the RT
    // from UEVR's ENGINE_SRC_COLOR (shader-resource) state to RENDER_TARGET
    // before plugins see it and back afterwards so UEVR's on_frame() copy
    // sees the state it expects.
    void prepare_rt(ID3D12Resource* rt);
    void restore_rt(ID3D12Resource* rt);

    // Drain in-flight GPU work without releasing resources.  Called from
    // PluginLoader before FreeLibrary so plugin-owned D3D12 resources are
    // not in use when the plugin DLL unmaps.
    void wait_for_gpu();

private:
    d3d12::CommandContext m_ctx{};
    bool m_active{false};
    bool m_dispatch_allowed{false};
};

} // namespace uevr
