// Internal helpers shared by the DX11 and DX12 backends.
// Not a public header — do not include from plugin code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include "effect_runtime.hpp"

#include "uevr/API.h"
#include "uevr/API.hpp"

namespace uevr::fx {

// Defined here (not effect_runtime.hpp) so backend TUs can derive from it.
// Plugins never see this type.
class EffectBackend {
public:
    virtual ~EffectBackend() = default;
    virtual void execute(const std::vector<RTDesc>&                rt_descs,
                         const std::vector<std::filesystem::path>& ext_tex_paths,
                         const std::vector<PassDesc>&              passes,
                         int                                       snapshot_mips,
                         uint64_t                                  pass_mask = ~uint64_t(0)) = 0;
};

std::unique_ptr<EffectBackend> make_backend_d3d11();
std::unique_ptr<EffectBackend> make_backend_d3d12();

namespace detail {
    void set_scene_size(unsigned w, unsigned h);
    // Set by each backend on every execute() so EffectRuntime::scene_rt_colorspace()
    // / scene_rt_format_name() can be queried from plugin UI code.
    void set_scene_rt_format(DXGI_FORMAT fmt);
    SceneRTColorSpace classify_scene_rt_colorspace(DXGI_FORMAT fmt);
    const char*       dxgi_format_name(DXGI_FORMAT fmt);
    // Logs once per scene-RT-identity change. Pass any pointer that uniquely
    // identifies the underlying engine resource (e.g. ID3D11Texture2D* /
    // ID3D12Resource*). No-op if `identity` matches the previous call.
    void log_scene_rt_identity_change(const void* identity, DXGI_FORMAT fmt, unsigned w, unsigned h);
}

} // namespace uevr::fx

namespace uevr::fx::detail {

// Lifted verbatim from the existing 16 plugins' identical helper.
inline DXGI_FORMAT resolve_typeless_format(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_FLOAT;
        default:                                 return fmt;
    }
}

// The same fullscreen-triangle VS used by every existing plugin.
inline constexpr const char* k_fullscreen_vs = R"(
struct VSOutput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };
VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput o;
    o.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    o.Position = float4(o.TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

// HLSL macro block prepended to a pass's PS source when
// `PassDesc::needs_scene_colorspace_decode == true`. Defines:
//   fx_decode_scene(c) — linearize scene sample (UNORM gamma -> linear)
//   fx_encode_scene(c) — re-encode linear color before write to scene
// For LinearFloat / SRGBTyped (or unknown — be conservative, don't double-apply
// gamma when we can't tell) the macros are identity. For AmbiguousUNORM they
// apply approximate gamma 2.2. Pointer-stable string literals so the PSO/PS
// cache key (which extends to include the colorspace selector) remains valid.
inline const char* scene_decode_macro_block(SceneRTColorSpace cs) {
    switch (cs) {
        case SceneRTColorSpace::AmbiguousUNORM:
            return
                "#define fx_decode_scene(c) pow(max((c), 0.0), 2.2)\n"
                "#define fx_encode_scene(c) pow(max((c), 0.0), 1.0/2.2)\n";
        case SceneRTColorSpace::LinearFloat:
        case SceneRTColorSpace::SRGBTyped:
        case SceneRTColorSpace::Unknown:
        default:
            return
                "#define fx_decode_scene(c) (c)\n"
                "#define fx_encode_scene(c) (c)\n";
    }
}

// Cache-key selector derived from (opt_in, colorspace). When opt_in is false
// the selector is a single shared sentinel so the cache entry is reused across
// colorspace changes (no preamble injected = no recompile needed).
inline int scene_decode_cache_selector(SceneRTColorSpace cs, bool opt_in) {
    return opt_in ? static_cast<int>(cs) : -1;
}

// ---------------------------------------------------------------------------
// Depth plumbing (INPUT_DEPTH). Used by both backends.
// ---------------------------------------------------------------------------

// Allow-listed conversion from a UE SceneDepthZ typeless/DSV format to the
// matching R-channel SRV format. Returns DXGI_FORMAT_UNKNOWN for formats we
// don't recognize as depth, in which case the runtime skips depth binding for
// the affected pass (logged once, debug visualization will be wrong but no
// crash). Allow-list keeps us safe against UE versions that may use unusual
// depth formats.
inline DXGI_FORMAT scene_depth_format_to_srv(DXGI_FORMAT fmt) {
    switch (fmt) {
        // D24S8 family — stencil sits in X8, depth read via R24_UNORM.
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        // D32 float — most common in UE5.
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        // D32 float + S8 stencil.
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        // D16
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

// CB layout matching `cbuffer fx_depth_info` in the injected HLSL preamble.
// Bound at register b1 (pass user CB stays at b0). 16-byte aligned.
struct DepthInfoCB {
    float    z_near      = 0.1f;
    float    z_far       = 10000.0f;
    uint32_t reversed_z  = 1;
    uint32_t perspective = 1;
};
static_assert(sizeof(DepthInfoCB) == 16, "DepthInfoCB must be 16 bytes");

// Populates `out` with depth-linearization parameters derived from the active
// UE projection matrix. P0 assumptions documented in the cbuffer comment:
// reversed-Z + perspective are hardcoded true; near is read from the matrix
// (column 3 row 2, matches UEVR's `update_matrices()` glm column-major layout);
// far is a synthetic constant because UEVR's projection is INFINITE-FAR
// (no finite far stored in the matrix). 10000 (UU) is the visualization range.
inline void extract_depth_info_cb(DepthInfoCB& out) {
    out = {};
    auto& api = uevr::API::get();
    if (!api) return;
    const auto proj = uevr::API::VR::get_ue_projection_matrix(uevr::API::VR::Eye::LEFT);
    // glm column-major: m[col][row]. UEVR stores nearz at m[3][2].
    const float n = proj.m[3][2];
    if (n > 1e-5f && n < 1e5f) out.z_near = n;
}

// HLSL preamble injected when a pass requests INPUT_DEPTH. Provides:
//   Texture2D fx_depth_tex                    bound at register tN
//   SamplerState fx_depth_smp                 bound at register sN (point/clamp)
//   cbuffer fx_depth_info : register(b1)      { z_near, z_far, reversed_z, perspective }
//   float fx_linearize_depth(float z)         depth-buffer value -> view-space units
//   float fx_sample_depth_linear(float2 uv)   sample + linearize
//   float fx_sample_depth_01(float2 uv)       sample + linearize + normalize to [0,1]
//
// `depth_srv_slot` is the t-register slot the runtime binds depth at
// (immediately after the pass's color inputs).
//
// NOTE on the linearization formula: UE typically uses INFINITE-FAR reverse-Z,
// for which the exact form is `z_view = z_near / z`. We use the finite-far
// reverse-Z form (z01 = 1-z; z_view = near*far / (far - z01*(far-near))) so the
// same code path works for both conventions; for the infinite-far case z_far
// only affects the long-distance saturation in fx_sample_depth_01.
inline std::string depth_preamble_block(unsigned depth_srv_slot) {
    const std::string n = std::to_string(depth_srv_slot);
    std::string s;
    s.reserve(1024);
    s += "Texture2D fx_depth_tex : register(t" + n + ");\n";
    s += "SamplerState fx_depth_smp : register(s" + n + ");\n";
    s += "cbuffer fx_depth_info : register(b1) {\n"
         "    float fx_z_near;\n"
         "    float fx_z_far;\n"
         "    uint  fx_reversed_z;\n"
         "    uint  fx_perspective;\n"
         "};\n"
         "float fx_linearize_depth(float z) {\n"
         "    float z01 = fx_reversed_z ? (1.0 - z) : z;\n"
         "    if (fx_perspective == 0) return lerp(fx_z_near, fx_z_far, z01);\n"
         "    float denom = fx_z_far - z01 * (fx_z_far - fx_z_near);\n"
         "    return (fx_z_near * fx_z_far) / max(denom, 1e-6);\n"
         "}\n"
         "float fx_sample_depth_linear(float2 uv) {\n"
         "    float z = fx_depth_tex.SampleLevel(fx_depth_smp, uv, 0).r;\n"
         "    return fx_linearize_depth(z);\n"
         "}\n"
         "float fx_sample_depth_01(float2 uv) {\n"
         "    float lin = fx_sample_depth_linear(uv);\n"
         "    return saturate((lin - fx_z_near) / max(fx_z_far - fx_z_near, 1e-3));\n"
         "}\n";
    return s;
}

// Decoded RGBA8 image, owned by `texture_loader`. Used by both backends to
// upload to a GPU texture on first execute().
struct DecodedImage {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba8;     // tightly packed, width*height*4 bytes
};

// Loads a PNG/JPG/etc. from disk into RGBA8. Returns empty image (width==0)
// on failure. Implemented in texture_loader.cpp via stb_image.
DecodedImage load_image_rgba8(const wchar_t* path_utf16);

} // namespace uevr::fx::detail
