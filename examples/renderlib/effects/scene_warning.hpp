#pragma once

// Tiny ImGui helper that surfaces a per-game warning when the scene render-target
// format is gamma-encoded (or has unknown/ambiguous colorspace), so end users can
// report it. Effect plugins should call this once near the top of their settings
// UI so the message is visible even when the rest of the panel is collapsed.
//
// Requires <imgui.h> to be included by the translation unit BEFORE this header.

#include "effect_runtime.hpp"

namespace uevr::fx {

inline void draw_scene_rt_colorspace_warning() {
    const auto cs = EffectRuntime::scene_rt_colorspace();
    if (cs == SceneRTColorSpace::AmbiguousUNORM || cs == SceneRTColorSpace::Unknown) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "Scene RT format may be gamma-encoded (%s); colors may look wrong. Please report this game.",
            EffectRuntime::scene_rt_format_name());
    }
}

} // namespace uevr::fx
