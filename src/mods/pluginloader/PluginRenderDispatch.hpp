// Per-present plugin pre-render dispatch.
//
// Extracted from VR::on_present() to keep src/mods/VR.cpp diff-clean against
// upstream praydog/UEVR (and against the joeyhodge fork's heavy churn in the
// same function). The two entry points below replace ~80 lines of inline DX11
// and DX12 plugin dispatch with two ~1-line call sites in VR.cpp.
//
// Behavior is byte-for-byte equivalent to the pre-refactor inline blocks:
//   - DX11: call on_pre_render_vr_framework_dx11(), then if the native-stereo
//     fix is enabled and a scene-capture RT exists, re-dispatch with the
//     scene_render_target override pointed at the capture RT.
//   - DX12: skip when in 2D-screen mode or when the D3D12Component has
//     suspended plugin dispatch (force_reset flow). Otherwise bracket the
//     main scene RT with prepare/restore barriers, dispatch plugins, then
//     when native-stereo fix is enabled repeat for the scene-capture RT on
//     the same plugin command list.

#pragma once

class VR;

namespace uevr::plugin_dispatch {

void on_present_dx11(VR& vr);
void on_present_dx12(VR& vr);

}
