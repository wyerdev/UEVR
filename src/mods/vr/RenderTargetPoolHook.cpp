#include <spdlog/spdlog.h>

#include <Windows.h>

#include <utility/Scan.hpp>
#include <utility/String.hpp>

#include <sdk/FRenderTargetPool.hpp>
#include <sdk/EngineModule.hpp>
#include <sdk/threading/RHIThreadWorker.hpp>

#include "../VR.hpp"
#include "../../utility/Logging.hpp"
#include "RenderTargetPoolHook.hpp"

RenderTargetPoolHook* g_hook{nullptr};

namespace {
// [fork] VirtualQuery-based readability check. Used to validate the `name`
// pointer in on_post_find_free_element before touching it as a wide string,
// because some UE 4.27 fork variants (notably Jedi Survivor's Respawn engine)
// compile FRenderTargetPool::FindFreeElement without the FRHICommandList&
// parameter. With the wrong ABI assumed, `name` is whatever uninitialized
// stack data sat at [RSP+0x28] and walking it as a wstring AVs. Validating
// keeps the game alive at the cost of depth not working on that title.
bool is_readable(const void* p, size_t bytes) noexcept {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
        | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    // Ensure the requested span fits within the queried region.
    const auto region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const auto need_end = reinterpret_cast<uintptr_t>(p) + bytes;
    return need_end <= region_end;
}

// Validate that `name` looks like a real pool-name wide string. UE pool names
// are short ASCII identifiers (e.g. "SceneDepthZ", "SceneColor"). We require:
//   - the first 8 bytes are readable
//   - the first wchar is a printable ASCII range char
//   - some terminator (or another printable char) within 256 chars
bool looks_like_pool_name(const wchar_t* name) noexcept {
    if (!is_readable(name, sizeof(wchar_t) * 2)) return false;
    if (name[0] < 0x20 || name[0] > 0x7E) return false;
    // Scan up to 256 wchars for a null terminator without leaving the page.
    for (size_t i = 0; i < 256; ++i) {
        if (!is_readable(name + i, sizeof(wchar_t))) return false;
        if (name[i] == 0) return true;
        // Tolerate any printable BMP code point in identifiers.
        if (name[i] < 0x20) return false;
    }
    return false; // unreasonably long, treat as garbage
}
} // namespace


RenderTargetPoolHook::RenderTargetPoolHook() {
    g_hook = this;
}

void RenderTargetPoolHook::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    if (!m_attempted_hook && VR::get()->is_depth_enabled()) {
        m_wants_activate = true;
    }

    if (!m_attempted_hook && m_wants_activate) {
        m_attempted_hook = true;
        m_hooked = hook();
    }
}

bool RenderTargetPoolHook::hook() {
    SPDLOG_INFO("Attempting to hook RenderTargetPool::FindFreeElement");

    const auto is_ue5 = VR::get()->get_fake_stereo_hook()->has_double_precision();
    const auto find_free_element = sdk::FRenderTargetPool::get_find_free_element_fn(is_ue5);

    if (!find_free_element) {
        SPDLOG_ERROR("Failed to find FRenderTargetPool::FindFreeElement, cannot hook");
        return false;
    }

    /*if (VR::get()->get_fake_stereo_hook()->has_double_precision()) {
        spdlog::error("Render target pool hook is temporarily disabled on UE5, sorry :(");
        return false;
    }*/

    SPDLOG_INFO("Performing hook...");

    if (is_ue5) {
        m_find_free_element_hook = safetyhook::create_inline((void*)*find_free_element, find_free_element_hook_ue5);
    } else {
        m_find_free_element_hook = safetyhook::create_inline((void*)*find_free_element, find_free_element_hook);
    }

    if (m_find_free_element_hook) {
        SPDLOG_INFO("Successfully hooked RenderTargetPool::FindFreeElement");
    } else {
        SPDLOG_ERROR("Failed to hook RenderTargetPool::FindFreeElement");
    }

    return true;
}

void RenderTargetPoolHook::on_post_find_free_element_raw(
    sdk::FRenderTargetPool* pool,
    uintptr_t slot_rdx,
    uintptr_t slot_r8,
    uintptr_t slot_r9,
    uintptr_t slot_stack0)
{
    // Right now we are only using this for depth
    // and on some games it will crash if we mess with anything
    // so, TODO: fix the games that crash with depth enabled
    if (!m_wants_activate) {
        std::scoped_lock _{g_hook->m_mutex};
        m_render_targets.clear();
        return;
    }

    // [fork] Universal layout probe. FRenderTargetPool::FindFreeElement comes
    // in two shapes across UE engine versions and forks:
    //
    //   (A) UE4 classic:    pool, FRHICommandList&, Desc, Out, Name, ...
    //                        -> name at slot_stack0, out at slot_r9
    //   (B) UE5 / Respawn:  pool, Desc, Out, Name, ...
    //                        -> name at slot_r9, out at slot_r8
    //
    // praydog originally picks A vs B from VR::is_ue5() (has_double_precision),
    // but that signal is wrong for some UE 4.27 forks (Respawn / Jedi Survivor
    // uses shape B despite being UE4) and we have no reliable static way to
    // tell. Instead, on the first call where one of the candidate slots holds
    // a wide string that looks like a real pool name ("SceneDepthZ", etc.),
    // lock in that layout for the session. Pool names are compile-time
    // constants in the engine's read-only data, so this is safe and one-shot.
    //
    // If neither slot validates we skip the call (no map mutation) and try
    // again next call; this keeps the game alive even on a not-yet-known
    // layout instead of dereferencing garbage.
    enum Layout : int { LAYOUT_UNKNOWN, LAYOUT_UE4_CLASSIC, LAYOUT_RESPAWN_UE5 };
    static Layout s_layout = LAYOUT_UNKNOWN;

    const auto* name_if_classic = reinterpret_cast<const wchar_t*>(slot_stack0);
    const auto* name_if_respawn = reinterpret_cast<const wchar_t*>(slot_r9);

    if (s_layout == LAYOUT_UNKNOWN) {
        const bool classic_valid = looks_like_pool_name(name_if_classic);
        const bool respawn_valid = looks_like_pool_name(name_if_respawn);

        if (respawn_valid && !classic_valid) {
            s_layout = LAYOUT_RESPAWN_UE5;
            SPDLOG_INFO("[RenderTargetPoolHook] Detected pool-shape: UE5/Respawn (no FRHICommandList& parameter)");
        } else if (classic_valid && !respawn_valid) {
            s_layout = LAYOUT_UE4_CLASSIC;
            SPDLOG_INFO("[RenderTargetPoolHook] Detected pool-shape: classic UE4 (with FRHICommandList& parameter)");
        } else if (classic_valid && respawn_valid) {
            // Vanishingly unlikely (both slots happen to point at printable
            // wide strings). Prefer Respawn/UE5 since it's the more common
            // shape on the engines that ship the inline FRHICommandList
            // variant out (UE5 and newer UE4.27 forks).
            s_layout = LAYOUT_RESPAWN_UE5;
            SPDLOG_WARN("[RenderTargetPoolHook] Both candidate slots validate as pool names; defaulting to UE5/Respawn layout");
        } else {
            // Neither slot looks like a wide string yet. Skip without
            // mutating state; next call may give us a usable name.
            return;
        }
    }

    const wchar_t* name = (s_layout == LAYOUT_RESPAWN_UE5) ? name_if_respawn : name_if_classic;
    auto* out = (s_layout == LAYOUT_RESPAWN_UE5)
        ? reinterpret_cast<TRefCountPtr<IPooledRenderTarget>*>(slot_r8)
        : reinterpret_cast<TRefCountPtr<IPooledRenderTarget>*>(slot_r9);

    if (name == nullptr) {
        return;
    }

    // Per-call revalidation. Engines call this thousands of times and an
    // occasional caller may pass a different shape (intra-process variation
    // is rare but possible across plugin DLLs). Cheap VirtualQuery, no
    // strncmp, no allocation.
    if (!looks_like_pool_name(name)) {
        return;
    }

    std::scoped_lock _{g_hook->m_mutex};

    if (out != nullptr) {
        g_hook->m_render_targets[name] = out->reference;
    } else {
        g_hook->m_render_targets.erase(name);
    }

    if (!g_hook->m_seen_names.contains(name)) {
        g_hook->m_seen_names.insert(name);
        SPDLOG_INFO("FRenderTargetPool::FindFreeElement called with name {}", utility::narrow(name));
    }
}

bool RenderTargetPoolHook::find_free_element_hook(
    sdk::FRenderTargetPool* pool,
    uintptr_t slot_rdx,
    uintptr_t slot_r8,
    uintptr_t slot_r9,
    uintptr_t slot_stack0,
    uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10)
{
    SPDLOG_INFO_ONCE("FRenderTargetPool::FindFreeElement (UE4) called for the first time!");

    // [fork] Forward arguments bit-positionally. We do not interpret the slot
    // semantics here — that's what makes the trampoline call safe regardless
    // of which compiled FindFreeElement signature this title uses (with or
    // without FRHICommandList&). on_post_find_free_element_raw probes the
    // slots to figure out which interpretation applies.
    const auto result = g_hook->m_find_free_element_hook.call<bool>(
        pool, slot_rdx, slot_r8, slot_r9, slot_stack0, a6, a7, a8, a9, a10);

    SPDLOG_INFO_ONCE("Finished calling FRenderTargetPool::FindFreeElement!");

    g_hook->on_post_find_free_element_raw(pool, slot_rdx, slot_r8, slot_r9, slot_stack0);

    return result;
}

bool RenderTargetPoolHook::find_free_element_hook_ue5(
    sdk::FRenderTargetPool* pool,
    uintptr_t slot_rdx,
    uintptr_t slot_r8,
    uintptr_t slot_r9,
    uintptr_t slot_stack0,
    uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10)
{
    SPDLOG_INFO_ONCE("FRenderTargetPool::FindFreeElement (UE5) called for the first time!");

    // [fork] Same raw forwarding as the UE4 path. NB: also fixes a praydog
    // bug where the previous UE5 hook dropped slot_stack0 when calling the
    // trampoline (passed 9 args instead of 10), corrupting the last stack
    // parameter that the real function expected.
    const auto result = g_hook->m_find_free_element_hook.call<bool>(
        pool, slot_rdx, slot_r8, slot_r9, slot_stack0, a6, a7, a8, a9, a10);

    SPDLOG_INFO_ONCE("Finished calling FRenderTargetPool::FindFreeElement! (UE5)");

    g_hook->on_post_find_free_element_raw(pool, slot_rdx, slot_r8, slot_r9, slot_stack0);

    return result;
}