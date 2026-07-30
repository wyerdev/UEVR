// See D3D12Helpers.hpp for context.
//
// The SEH-protected submit was previously a file-static inside
// src/mods/vr/d3d12/CommandContext.cpp; the in-place texture update was a
// member of d3d12::TextureContext.  Both are co-located here so the
// upstream-owned d3d12/ files stay close to praydog/UEVR — see
// docs/active/shader-infra-refactor-plan.md §4.

#include <mutex>

#include <spdlog/spdlog.h>
#include <utility/String.hpp>

#include "Framework.hpp"

#include "../vr/d3d12/CommandContext.hpp"
#include "../vr/d3d12/TextureContext.hpp"

#include "D3D12Helpers.hpp"

namespace uevr::d3d12_helpers {

// __try / __except cannot coexist with C++ objects that have destructors
// (e.g. std::scoped_lock) in the same function — kept as a free helper.
static bool seh_submit(
    ID3D12GraphicsCommandList* cmd_list,
    ID3D12CommandQueue* command_queue,
    ID3D12Fence* fence,
    UINT64& fence_value,
    HANDLE fence_event,
    bool& waiting_for_fence)
{
    __try {
        if (FAILED(cmd_list->Close())) {
            return false;
        }

        ID3D12CommandList* const cmd_lists[] = {cmd_list};
        command_queue->ExecuteCommandLists(1, cmd_lists);
        command_queue->Signal(fence, ++fence_value);
        fence->SetEventOnCompletion(fence_value, fence_event);
        waiting_for_fence = true;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void recover_from_failed_execute(d3d12::CommandContext& ctx) {
    // Reset the command allocator and list into a clean open state
    // so the next frame's wait() + recording works normally.
    // This may fail if the device is in a removed state — that's fine,
    // the next frame will detect it via setup() or wait().
    if (ctx.cmd_allocator) {
        ctx.cmd_allocator->Reset();
    }
    if (ctx.cmd_list && ctx.cmd_allocator) {
        ctx.cmd_list->Reset(ctx.cmd_allocator.Get(), nullptr);
    }
    ctx.waiting_for_fence = false;
}

void execute_safe(d3d12::CommandContext& ctx) {
    std::scoped_lock _{ctx.mtx};

    if (!ctx.has_commands) {
        return;
    }

    ctx.has_commands = false;

    auto command_queue = g_framework->get_d3d12_hook()->get_command_queue();

    if (!seh_submit(
            ctx.cmd_list.Get(),
            command_queue,
            ctx.fence.Get(),
            ctx.fence_value,
            ctx.fence_event,
            ctx.waiting_for_fence))
    {
        spdlog::warn("[VR] Command list execution failed or caught exception ({}), recovering.", utility::narrow(ctx.internal_name));
        recover_from_failed_execute(ctx);
    }
}

void discard(d3d12::CommandContext& ctx) {
    std::scoped_lock _{ctx.mtx};

    if (ctx.has_commands) {
        spdlog::info("[VR] Discarding plugin command list ({}) — scene RT invalidated.", utility::narrow(ctx.internal_name));
        ctx.has_commands = false;
        recover_from_failed_execute(ctx);
    }
}

bool update_texture(
    d3d12::TextureContext& ctx,
    ID3D12Device* device,
    ID3D12Resource* rsrc,
    std::optional<DXGI_FORMAT> rtv_format,
    std::optional<DXGI_FORMAT> srv_format,
    const wchar_t* name)
{
    if (rsrc == nullptr) {
        return false;
    }

    // If heaps don't exist yet, fall back to full setup.
    if (ctx.rtv_heap == nullptr || ctx.rtv_heap->Heap() == nullptr ||
        ctx.srv_heap == nullptr || ctx.srv_heap->Heap() == nullptr) {
        return ctx.setup(device, rsrc, rtv_format, srv_format, name);
    }

    // Heaps already exist. Swap the texture and overwrite the view
    // descriptors in-place — no heap allocation needed. RTV/SRV heap
    // slots are format-agnostic; CreateRenderTargetView/CreateShaderResourceView
    // writes the new view into the existing descriptor regardless of
    // resource format or dimension changes.
    ctx.texture.Reset();
    ctx.texture = rsrc;
    rsrc->SetName(name);

    // Overwrite RTV in existing heap.
    if (rtv_format) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = (DXGI_FORMAT)*rtv_format;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtv_desc.Texture2D.MipSlice = 0;
        rtv_desc.Texture2D.PlaneSlice = 0;
        device->CreateRenderTargetView(ctx.texture.Get(), &rtv_desc, ctx.get_rtv());
    } else {
        device->CreateRenderTargetView(ctx.texture.Get(), nullptr, ctx.get_rtv());
    }

    // Overwrite SRV in existing heap.
    if (srv_format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = (DXGI_FORMAT)*srv_format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.PlaneSlice = 0;
        srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(ctx.texture.Get(), &srv_desc, ctx.get_srv_cpu());
    } else {
        device->CreateShaderResourceView(ctx.texture.Get(), nullptr, ctx.get_srv_cpu());
    }

    return true;
}

} // namespace uevr::d3d12_helpers
