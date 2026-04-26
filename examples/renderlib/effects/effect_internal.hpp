// Internal helpers shared by the DX11 and DX12 backends.
// Not a public header — do not include from plugin code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include "effect_runtime.hpp"

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
