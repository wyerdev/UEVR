#pragma once

#include "uevr/API.h"

namespace uevr {
namespace stereo_hook {
UEVR_FRHITexture2DHandle get_scene_render_target();
UEVR_FRHITexture2DHandle get_ui_render_target();
UEVR_FRHITexture2DHandle get_scene_capture_render_target();
void* get_pre_render_command_list();

// Temporarily override what get_scene_render_target() returns.
// Used by VR.cpp to transparently re-dispatch plugin callbacks for
// the scene capture RT without plugins needing native-stereo awareness.
void set_scene_render_target_override(UEVR_FRHITexture2DHandle override_rt);

extern UEVR_FFakeStereoRenderingHookFunctions functions;
}
}