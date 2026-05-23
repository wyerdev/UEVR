#pragma once

#include "uevr/API.h"

namespace uevr {
namespace render_target_pool_hook {
void activate();
UEVR_IPooledRenderTargetHandle get_render_target(const wchar_t* name);
// [fork] depth plumbing — resolve pooled RT to its FRHITexture2D.
UEVR_FRHITexture2DHandle get_render_target_texture(UEVR_IPooledRenderTargetHandle handle);
extern UEVR_FRenderTargetPoolHookFunctions functions;
}
}