// [fork-file] Ours — not in praydog/UEVR. Free to edit; no merge risk.
// Free-function shims around d3d12::CommandContext / d3d12::TextureContext
// so the fork-only behavior (SEH-protected execute, recovery, discard,
// in-place TextureContext update) lives outside the upstream-owned
// d3d12/ headers and .cpp files.  See docs/active/shader-infra-refactor-plan.md §4.
//
// Files we own (under src/mods/pluginloader/) include this header directly.
// Upstream-owned files keep a single `[fork]` include + call site each.

#pragma once

#include <optional>

#include <d3d12.h>
#include <dxgiformat.h>

namespace d3d12 {
struct CommandContext;
struct TextureContext;
}

namespace uevr::d3d12_helpers {

// SEH-protected variant of d3d12::CommandContext::execute().  Replaces the
// pre-refactor inline submit body so a malformed command list crashing inside
// ID3D12CommandQueue::ExecuteCommandLists is caught and the allocator/list
// are restored to a clean open state for the next frame.
//
// Acquires ctx.mtx internally; safe to call without holding the lock.
void execute_safe(d3d12::CommandContext& ctx);

// Reset allocator+list into a clean open state after a failed execute or a
// discarded command list.  Caller must hold ctx.mtx (or call via discard() /
// execute_safe(), which take the lock).
void recover_from_failed_execute(d3d12::CommandContext& ctx);

// Discard any recorded commands without submitting; resets the command list
// to a clean open state for the next frame.  Used by PluginCommandListD3D12
// when the scene RT is invalidated mid-frame.
void discard(d3d12::CommandContext& ctx);

// Swap a TextureContext's underlying resource in-place, overwriting the RTV
// and SRV in the existing descriptor heaps.  Falls back to ctx.setup() when
// the heaps don't exist yet.  Cheaper than full setup() because it avoids
// re-allocating descriptor heap slots; safe across format/dimension changes
// since CreateRenderTargetView / CreateShaderResourceView overwrite the
// descriptor regardless.
bool update_texture(
    d3d12::TextureContext& ctx,
    ID3D12Device* device,
    ID3D12Resource* rsrc,
    std::optional<DXGI_FORMAT> rtv_format = std::nullopt,
    std::optional<DXGI_FORMAT> srv_format = std::nullopt,
    const wchar_t* name = L"TextureContext object");

} // namespace uevr::d3d12_helpers
