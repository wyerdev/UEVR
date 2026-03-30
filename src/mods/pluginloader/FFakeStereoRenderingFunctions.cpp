#include "uevr/API.h"

#include "mods/VR.hpp"
#include "FFakeStereoRenderingFunctions.hpp"

namespace uevr {

static UEVR_FRHITexture2DHandle s_scene_rt_override = nullptr;

void stereo_hook::set_scene_render_target_override(UEVR_FRHITexture2DHandle override_rt) {
    s_scene_rt_override = override_rt;
}

UEVR_FRHITexture2DHandle stereo_hook::get_scene_render_target() {
    // If an override is set (e.g. UEVR re-dispatching for scene capture RT),
    // return it instead of the real scene RT.
    if (s_scene_rt_override != nullptr) {
        return s_scene_rt_override;
    }

    const auto& vr = VR::get();
    if (auto& hook = vr->get_fake_stereo_hook(); hook != nullptr) {
        auto rtm = hook->get_render_target_manager();
        if (auto rtm = hook->get_render_target_manager(); rtm != nullptr) {
            return (UEVR_FRHITexture2DHandle)rtm->get_render_target();
        }
    }

    return nullptr;
}

UEVR_FRHITexture2DHandle stereo_hook::get_ui_render_target() {
    const auto& vr = VR::get();
    if (auto& hook = vr->get_fake_stereo_hook(); hook != nullptr) {
        auto rtm = hook->get_render_target_manager();
        if (auto rtm = hook->get_render_target_manager(); rtm != nullptr) {
            return (UEVR_FRHITexture2DHandle)rtm->get_ui_target();
        }
    }

    return nullptr;
}

UEVR_FRHITexture2DHandle stereo_hook::get_scene_capture_render_target() {
    const auto& vr = VR::get();
    if (auto& hook = vr->get_fake_stereo_hook(); hook != nullptr) {
        if (auto rtm = hook->get_render_target_manager(); rtm != nullptr) {
            return (UEVR_FRHITexture2DHandle)rtm->get_scene_capture_render_target();
        }
    }

    return nullptr;
}

void* stereo_hook::get_pre_render_command_list() {
    const auto& vr = VR::get();
    if (vr) {
        return (void*)vr->d3d12().get_plugin_command_list();
    }
    return nullptr;
}

UEVR_FFakeStereoRenderingHookFunctions stereo_hook::functions {
    .get_scene_render_target = &stereo_hook::get_scene_render_target,
    .get_ui_render_target = &stereo_hook::get_ui_render_target,
    .get_scene_capture_render_target = &stereo_hook::get_scene_capture_render_target,
    .get_pre_render_command_list = &stereo_hook::get_pre_render_command_list
};
}