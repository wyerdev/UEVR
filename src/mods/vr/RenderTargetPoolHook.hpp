#pragma once

#include <mutex>
#include <wrl.h>

#include "../../Mod.hpp"

#include <SafetyHook.hpp>

#include <sdk/RHICommandList.hpp>
#include <sdk/StereoStuff.hpp>

namespace sdk {
class FRenderTargetPool;
class FPooledRenderTargetDesc;
}

class RenderTargetPoolHook : public ModComponent {
public:
    RenderTargetPoolHook();
    void on_pre_engine_tick(sdk::UGameEngine* engine, float delta) override;
    void activate() {
        m_wants_activate = true;
    }

    IPooledRenderTarget* get_render_target(const std::wstring& name) {
        std::scoped_lock _{m_mutex};
        if (auto it = m_render_targets.find(name); it != m_render_targets.end()) {
            return it->second;
        }

        return nullptr;
    }

    template<typename T>
    Microsoft::WRL::ComPtr<T> get_texture(const std::wstring& name) {
        std::scoped_lock _{m_mutex};
        if (auto it = m_render_targets.find(name); it != m_render_targets.end()) {
            const auto& rt = it->second;
            const auto& tex = rt->item.texture.texture;

            if (tex == nullptr) {
                return nullptr;
            }

            auto native_resource = (T*)tex->get_native_resource();

            if (native_resource == nullptr) {
                return nullptr;
            }

            return native_resource;
        }

        return nullptr;
    }

private:
    bool hook();

    // [fork] Both UE4-classic and UE5/Respawn-UE4 layouts of
    // FRenderTargetPool::FindFreeElement are handled by a single raw-slot
    // post-callback. The two hook trampolines forward arguments
    // bit-positionally regardless of semantic interpretation; the post-callback
    // probes which slot holds a valid wide-string pool name and caches the
    // detected layout per session. This makes depth tracking work on titles
    // whose engine fork compiled FindFreeElement without the FRHICommandList&
    // parameter (e.g. Respawn UE 4.27 / Jedi Survivor) without crashing.
    static bool find_free_element_hook(
        sdk::FRenderTargetPool* pool,
        uintptr_t slot_rdx,
        uintptr_t slot_r8,
        uintptr_t slot_r9,
        uintptr_t slot_stack0,
        uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10);

    static bool find_free_element_hook_ue5(
        sdk::FRenderTargetPool* pool,
        uintptr_t slot_rdx,
        uintptr_t slot_r8,
        uintptr_t slot_r9,
        uintptr_t slot_stack0,
        uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10);

    void on_post_find_free_element_raw(
        sdk::FRenderTargetPool* pool,
        uintptr_t slot_rdx,
        uintptr_t slot_r8,
        uintptr_t slot_r9,
        uintptr_t slot_stack0);

    bool m_attempted_hook{false};
    bool m_hooked{false};
    bool m_wants_activate{false};

    std::recursive_mutex m_mutex{};
    SafetyHookInline m_find_free_element_hook{};
    std::unordered_map<std::wstring, IPooledRenderTarget*> m_render_targets{};
    std::unordered_set<std::wstring> m_seen_names{};
};