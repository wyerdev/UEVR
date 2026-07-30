// [fork-file] Ours — not in praydog/UEVR. Free to edit; no merge risk.
#include <d3d12.h>

#include "../VR.hpp"
#include "../PluginLoader.hpp"
#include "../vr/D3D12Component.hpp"
#include "../vr/FFakeStereoRenderingHook.hpp"

#include "FFakeStereoRenderingFunctions.hpp"
#include "PluginRenderDispatch.hpp"

namespace uevr::plugin_dispatch {

void on_present_dx11(VR& vr) {
    PluginLoader::get()->on_pre_render_vr_framework_dx11();

    // Native stereo fix: also dispatch for scene capture RT so plugins
    // process both eyes without needing native-stereo awareness.
    if (vr.is_native_stereo_fix_enabled()) {
        if (auto capture = uevr::stereo_hook::get_scene_capture_render_target(); capture != nullptr) {
            uevr::stereo_hook::set_scene_render_target_override(capture);
            PluginLoader::get()->on_pre_render_vr_framework_dx11();
            uevr::stereo_hook::set_scene_render_target_override(nullptr);
        }
    }
}

void on_present_dx12(VR& vr) {
    auto& d3d12 = vr.d3d12();

    // Skip plugin VR pre-render dispatch when the scene render target
    // is unavailable.  This happens during 2D-screen-mode transitions
    // and level loads where UE destroys/recreates the RT.  Dispatching
    // plugins with a stale or in-flux RT causes D3D12 access violations.
    bool dispatch_plugins = !vr.is_using_2d_screen() && d3d12.plugin_cl().is_allowed();

    auto& stereo_hook = vr.get_fake_stereo_hook();

    // Resolve the main scene RT for barrier management.
    ID3D12Resource* main_native_rt = nullptr;
    if (dispatch_plugins && stereo_hook != nullptr) {
        if (auto rtm = stereo_hook->get_render_target_manager(); rtm != nullptr) {
            auto rt = rtm->get_render_target();
            if (rt != nullptr) {
                main_native_rt = (ID3D12Resource*)rt->get_native_resource();
            }
            if (main_native_rt == nullptr) {
                dispatch_plugins = false;
            }
        } else {
            dispatch_plugins = false;
        }
    }

    if (!dispatch_plugins) {
        return;
    }

    // Single command list for all plugin dispatches.
    // Resource state transitions bracket each dispatch so plugins
    // see the RT in RENDER_TARGET state and UEVR's on_frame()
    // copy sees ENGINE_SRC_COLOR afterwards.
    d3d12.plugin_cl().begin();
    d3d12.plugin_cl().prepare_rt(main_native_rt);

    PluginLoader::get()->on_pre_render_vr_framework_dx12();

    d3d12.plugin_cl().restore_rt(main_native_rt);

    // Native stereo fix: also dispatch for scene capture RT.
    // Recorded on the same command list — D3D12 runtime keeps
    // resources alive through the command allocator until GPU
    // execution completes, so no separate submit cycle needed.
    if (vr.is_native_stereo_fix_enabled()) {
        auto rtm = stereo_hook->get_render_target_manager();
        if (rtm != nullptr) {
            auto capture = rtm->get_scene_capture_render_target();
            if (capture != nullptr) {
                auto capture_native = (ID3D12Resource*)capture->get_native_resource();
                if (capture_native != nullptr) {
                    d3d12.plugin_cl().prepare_rt(capture_native);

                    uevr::stereo_hook::set_scene_render_target_override((UEVR_FRHITexture2DHandle)capture);
                    PluginLoader::get()->on_pre_render_vr_framework_dx12();
                    uevr::stereo_hook::set_scene_render_target_override(nullptr);

                    d3d12.plugin_cl().restore_rt(capture_native);
                }
            }
        }
    }

    d3d12.plugin_cl().end();
}

}
