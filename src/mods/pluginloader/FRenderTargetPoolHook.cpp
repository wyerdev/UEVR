#include <sdk/StereoStuff.hpp>

#include "../mods/VR.hpp"
#include "FRenderTargetPoolHook.hpp"

namespace uevr {
void render_target_pool_hook::activate() {
    const auto& vr = VR::get();
    if (auto& hook = vr->get_render_target_pool_hook(); hook != nullptr) {
        hook->activate();
    }
}

UEVR_IPooledRenderTargetHandle render_target_pool_hook::get_render_target(const wchar_t* name) {
    const auto& vr = VR::get();
    if (auto& hook = vr->get_render_target_pool_hook(); hook != nullptr) {
        return (UEVR_IPooledRenderTargetHandle)hook->get_render_target(name);
    }

    return nullptr;
}

// [fork] Resolves the underlying FRHITexture2D* from an IPooledRenderTarget*.
// Mirrors the internal RenderTargetPoolHook::get_texture<T>() access pattern
// (rt->item.texture.texture) but returns the higher-level RHI handle so the
// plugin API can stay D3D-agnostic. Required by renderlib's INPUT_DEPTH path.
UEVR_FRHITexture2DHandle render_target_pool_hook::get_render_target_texture(UEVR_IPooledRenderTargetHandle handle) {
    if (handle == nullptr) {
        return nullptr;
    }

    auto* rt = (IPooledRenderTarget*)handle;
    return (UEVR_FRHITexture2DHandle)rt->item.texture.texture;
}

UEVR_FRenderTargetPoolHookFunctions render_target_pool_hook::functions {
    .activate = &render_target_pool_hook::activate,
    .get_render_target = &render_target_pool_hook::get_render_target,
    .get_render_target_texture = &render_target_pool_hook::get_render_target_texture
};
}