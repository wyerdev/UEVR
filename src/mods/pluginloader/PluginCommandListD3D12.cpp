// [fork-file] Ours — not in praydog/UEVR. Free to edit; no merge risk.
// See PluginCommandListD3D12.hpp for context.
//
// Implementation moved verbatim from vrmod::D3D12Component::{begin,end,
// get,prepare,restore}_plugin_pre_render and ::wait_for_plugin_gpu_work.
// Behavior must remain byte-for-byte identical to the pre-refactor inline
// methods — the triple-buffered allocator + barrier ordering exist to fix
// specific stuttering / TDR cases (docs/active/shader-infra-refactor-plan.md
// §3) and are not free to redesign here.

#include <spdlog/spdlog.h>

#include "../../utility/Logging.hpp"

#include "../VR.hpp"
#include "../vr/D3D12Component.hpp"
#include "../vr/FFakeStereoRenderingHook.hpp"

#include "D3D12Helpers.hpp"
#include "PluginCommandListD3D12.hpp"

namespace uevr {

namespace {

constexpr auto ENGINE_SRC_COLOR =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// Mirror of vrmod::safe_get_native_resource() — duplicated here so we can
// SEH-guard the late RT-validity check in end() without pulling that
// anonymous-namespace helper out of D3D12Component.cpp.
ID3D12Resource* safe_get_native_resource(FRHITexture2D* texture) noexcept {
    if (texture == nullptr) {
        return nullptr;
    }

    __try {
        return (ID3D12Resource*)texture->get_native_resource();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace

void PluginCommandListD3D12::on_reset() {
    // Drain plugin pre-render work before releasing resources.
    m_ctx.reset();
    m_active = false;
}

void PluginCommandListD3D12::begin() {
    if (!is_allowed()) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Plugin dispatch deferred (dispatch_allowed=false)");
        return;
    }

    // Lazy setup — CommandContext::setup() only needs the device which is
    // available via g_framework before D3D12Component::setup() runs.
    if (!m_ctx.ready()) {
        if (!m_ctx.setup(L"Plugin Pre-Render")) {
            return;
        }
    }

    // Wait for previous frame's GPU work on this context, then reset allocator+list.
    m_ctx.wait(INFINITE);
    m_active = true;
}

void PluginCommandListD3D12::end() {
    if (!m_active) {
        return;
    }

    m_active = false;

    // Before submitting, verify the scene RT is still valid.
    // During scene transitions (e.g. 2d mode, level loads) UE may
    // destroy render targets between the plugin callback and submission.
    // If the RT is gone, discard the command list instead of submitting
    // stale GPU commands that would trigger D3D12 runtime exceptions.
    if (m_ctx.has_commands) {
        const auto vr = VR::get();
        const auto& ffsr = vr->get_fake_stereo_hook();
        bool rt_valid = false;

        if (ffsr != nullptr) {
            if (auto rtm = ffsr->get_render_target_manager(); rtm != nullptr) {
                if (auto rt = rtm->get_render_target(); rt != nullptr) {
                    if (safe_get_native_resource(rt) != nullptr) {
                        rt_valid = true;
                    }
                }
            }
        }

        if (!rt_valid) {
            uevr::d3d12_helpers::discard(m_ctx);
            return;
        }
    }

    // If any plugin recorded commands, close and submit on the game's queue.
    uevr::d3d12_helpers::execute_safe(m_ctx);
}

ID3D12GraphicsCommandList* PluginCommandListD3D12::get() {
    if (!m_active || !m_ctx.ready()) {
        return nullptr;
    }

    // Mark that commands have been recorded so execute() will submit.
    m_ctx.has_commands = true;
    return m_ctx.cmd_list.Get();
}

void PluginCommandListD3D12::wait_for_gpu() {
    if (m_ctx.ready()) {
        m_ctx.wait(INFINITE);
    }
}

void PluginCommandListD3D12::prepare_rt(ID3D12Resource* rt) {
    if (rt == nullptr || !m_active || !m_ctx.ready()) {
        return;
    }

    // Transition the scene RT from the state UE leaves it in (shader
    // resource) to RENDER_TARGET so plugins can assume a consistent
    // starting state.  This eliminates the resource-state mismatch
    // that causes GPU hangs / TDR when plugins hardcode RENDER_TARGET as
    // the "before" state in their own barriers.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt;
    barrier.Transition.StateBefore = ENGINE_SRC_COLOR;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_ctx.cmd_list->ResourceBarrier(1, &barrier);
    m_ctx.has_commands = true;
}

void PluginCommandListD3D12::restore_rt(ID3D12Resource* rt) {
    if (rt == nullptr || !m_active || !m_ctx.ready()) {
        return;
    }

    // Transition the scene RT back to ENGINE_SRC_COLOR (shader resource
    // state) so UEVR's on_frame() copy and UE's own pipeline see the
    // state they expect.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = ENGINE_SRC_COLOR;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_ctx.cmd_list->ResourceBarrier(1, &barrier);
    m_ctx.has_commands = true;
}

} // namespace uevr
