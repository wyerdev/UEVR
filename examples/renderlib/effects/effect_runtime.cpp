// Backend-agnostic dispatch for `EffectRuntime`. Owns the user's declared
// graph (RTDescs, external-texture paths, PassDescs), constructs a backend
// lazily on first execute() based on UEVR renderer type, and forwards.

#include "effect_runtime.hpp"
#include "effect_internal.hpp"

#include "uevr/API.h"
#include "uevr/API.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>

namespace uevr::fx {

namespace {
// SEH-guarded backend dispatch. Plain function — no C++ objects with destructors,
// no `try/catch`, so MSVC allows `__try/__except`. Returns true if the backend
// completed without an SEH fault, false if we caught one.
//
// Why we need this: scene RT can become stale during VR-mode toggles, level
// transitions, and headset-off operation. The native pre-render callback fires
// faster than UEVR's on_device_reset() can tear us down, so the backend may
// reach D3D12 calls with a freed scene resource. We swallow the AV here so
// the game keeps rendering and the log doesn't drown in `[APIProxy] Access
// violation in on_pre_render_vr_framework_dx12 callback` lines.
bool dispatch_backend_seh(EffectBackend* backend,
                          const std::vector<RTDesc>& rts,
                          const std::vector<std::filesystem::path>& exts,
                          const std::vector<PassDesc>& passes,
                          int snapshot_mips,
                          uint64_t pass_mask) {
    __try {
        backend->execute(rts, exts, passes, snapshot_mips, pass_mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // anonymous namespace

// Last scene-RT dimensions set by whichever backend most-recently executed.
// Exposed via EffectRuntime::scene_width()/scene_height() for plugin cbuffers.
namespace detail {
    thread_local unsigned tls_scene_w = 0;
    thread_local unsigned tls_scene_h = 0;
    thread_local DXGI_FORMAT tls_scene_fmt = DXGI_FORMAT_UNKNOWN;
    void set_scene_size(unsigned w, unsigned h) { tls_scene_w = w; tls_scene_h = h; }
    void set_scene_rt_format(DXGI_FORMAT fmt)   { tls_scene_fmt = fmt; }

    SceneRTColorSpace classify_scene_rt_colorspace(DXGI_FORMAT fmt) {
        switch (fmt) {
            // Float formats — always linear, no encode needed.
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
            case DXGI_FORMAT_R11G11B10_FLOAT:
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R32G32_FLOAT:
            case DXGI_FORMAT_R16_FLOAT:
            case DXGI_FORMAT_R32_FLOAT:
                return SceneRTColorSpace::LinearFloat;
            // sRGB-typed UNORM — the GPU view does sRGB<->linear automatically.
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                return SceneRTColorSpace::SRGBTyped;
            // Plain UNORM — ambiguous (DXGI metadata can't tell linear vs gamma).
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8X8_UNORM:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return SceneRTColorSpace::AmbiguousUNORM;
            default:
                return SceneRTColorSpace::Unknown;
        }
    }

    const char* dxgi_format_name(DXGI_FORMAT fmt) {
        switch (fmt) {
            case DXGI_FORMAT_UNKNOWN:                return "<unknown>";
            case DXGI_FORMAT_R8G8B8A8_UNORM:         return "R8G8B8A8_UNORM";
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:    return "R8G8B8A8_UNORM_SRGB";
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:      return "R8G8B8A8_TYPELESS";
            case DXGI_FORMAT_B8G8R8A8_UNORM:         return "B8G8R8A8_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:    return "B8G8R8A8_UNORM_SRGB";
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:      return "B8G8R8A8_TYPELESS";
            case DXGI_FORMAT_B8G8R8X8_UNORM:         return "B8G8R8X8_UNORM";
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:    return "B8G8R8X8_UNORM_SRGB";
            case DXGI_FORMAT_R10G10B10A2_UNORM:      return "R10G10B10A2_UNORM";
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:   return "R10G10B10A2_TYPELESS";
            case DXGI_FORMAT_R16G16B16A16_FLOAT:     return "R16G16B16A16_FLOAT";
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:  return "R16G16B16A16_TYPELESS";
            case DXGI_FORMAT_R32G32B32A32_FLOAT:     return "R32G32B32A32_FLOAT";
            case DXGI_FORMAT_R32G32B32A32_TYPELESS:  return "R32G32B32A32_TYPELESS";
            case DXGI_FORMAT_R11G11B10_FLOAT:        return "R11G11B10_FLOAT";
            case DXGI_FORMAT_R16G16_FLOAT:           return "R16G16_FLOAT";
            case DXGI_FORMAT_R32G32_FLOAT:           return "R32G32_FLOAT";
            case DXGI_FORMAT_R16_FLOAT:              return "R16_FLOAT";
            case DXGI_FORMAT_R32_FLOAT:              return "R32_FLOAT";
            default:                                  return "<other>";
        }
    }

    void log_scene_rt_identity_change(const void* identity, DXGI_FORMAT fmt, unsigned w, unsigned h) {
        // File-scope (process-wide) so identity dedup works across plugins/eyes.
        // Native-stereo-fix dispatches per-eye with TWO DIFFERENT scene RTs (main + scene_capture),
        // alternating every frame. A single-slot "last seen" dedup churns and spams the log;
        // instead remember every (identity, fmt, w, h) combo we have ever logged and emit once
        // per unique combo. Bounded by the small number of distinct scene RTs UE hands us
        // (typically 2 with native-stereo-fix, 1 otherwise).
        struct Key {
            const void* identity;
            DXGI_FORMAT fmt;
            unsigned    w;
            unsigned    h;
            bool operator==(const Key& o) const noexcept {
                return identity == o.identity && fmt == o.fmt && w == o.w && h == o.h;
            }
        };
        struct KeyHash {
            size_t operator()(const Key& k) const noexcept {
                size_t h = reinterpret_cast<size_t>(k.identity);
                h ^= static_cast<size_t>(k.fmt) * 0x9E3779B97F4A7C15ull;
                h ^= (static_cast<size_t>(k.w) << 16) ^ static_cast<size_t>(k.h);
                return h;
            }
        };
        static std::unordered_set<Key, KeyHash> s_seen;
        if (!s_seen.insert(Key{identity, fmt, w, h}).second) return;
        const char* cs_name = nullptr;
        switch (classify_scene_rt_colorspace(fmt)) {
            case SceneRTColorSpace::LinearFloat:    cs_name = "LinearFloat"; break;
            case SceneRTColorSpace::SRGBTyped:      cs_name = "SRGBTyped";   break;
            case SceneRTColorSpace::AmbiguousUNORM: cs_name = "AmbiguousUNORM"; break;
            default:                                cs_name = "Unknown";     break;
        }
        if (auto& api = uevr::API::get()) {
            api->log_info("[fx] scene RT: %s %ux%u -> %s", dxgi_format_name(fmt), w, h, cs_name);
        }
    }
}

// Forward decls for the per-frame dispatch registry helpers (defined below).
void register_runtime_for_present(EffectRuntime* rt);
void unregister_runtime_for_present(EffectRuntime* rt);

EffectRuntime::EffectRuntime()  = default;
EffectRuntime::~EffectRuntime() {
    // Best-effort: if this instance is in the on-present registry, drop it so
    // the static callback does not deref a destroyed object. Plugins that own
    // EffectRuntime as a member are typically destroyed during DLL unload, by
    // which point UEVR's callback dispatcher is no longer running — but the
    // unregister is cheap and avoids a use-after-free if the order ever flips.
    unregister_runtime_for_present(this);
}

namespace {
    // Plugin-DLL-scoped registry of live EffectRuntime instances, walked by the
    // single on_present callback we register lazily on the first execute() call.
    // Each plugin DLL that links renderlib gets its own registry + its own
    // on_present registration — fine, the host calls all registered callbacks.
    std::mutex                            g_runtime_registry_mtx;
    std::vector<EffectRuntime*>           g_runtime_registry;
    std::atomic<bool>                     g_present_hook_registered{false};

    void on_present_dispatch_runtimes() {
        // Reset per-frame dispatch counters across every live runtime in this
        // DLL so the next frame's first execute() is recognized as dispatch 0.
        std::lock_guard<std::mutex> lk(g_runtime_registry_mtx);
        // Diagnostic: confirm the DLL's on_present callback is firing, plus
        // its rate (first call, then every Nth). If we never see this line in
        // the log, the DLL never registered its on_present and Cadence is dead.
        static uint64_t s_calls = 0;
        ++s_calls;
        const bool log_now = (s_calls == 1) || (s_calls == 60) || (s_calls == 600) || (s_calls % 6000 == 0);
        if (log_now) {
            if (auto& api = uevr::API::get()) {
                api->log_info("[fx-diag] on_present_dispatch_runtimes call #%llu, runtimes=%zu",
                              (unsigned long long)s_calls, g_runtime_registry.size());
            }
        }
        for (auto* rt : g_runtime_registry) {
            rt->_internal_reset_dispatch_in_frame();
        }
    }
}

void register_runtime_for_present(EffectRuntime* rt) {
    std::lock_guard<std::mutex> lk(g_runtime_registry_mtx);
    for (auto* existing : g_runtime_registry) if (existing == rt) return;
    g_runtime_registry.push_back(rt);
}

void unregister_runtime_for_present(EffectRuntime* rt) {
    std::lock_guard<std::mutex> lk(g_runtime_registry_mtx);
    g_runtime_registry.erase(
        std::remove(g_runtime_registry.begin(), g_runtime_registry.end(), rt),
        g_runtime_registry.end());
}

int EffectRuntime::declare_rt(const RTDesc& desc) {
    m_rt_descs.push_back(desc);
    return static_cast<int>(m_rt_descs.size() - 1);
}

int EffectRuntime::load_external_texture_png(const std::filesystem::path& path) {
    m_ext_tex_paths.push_back(path);
    return EXTERNAL_TEX_BASE + static_cast<int>(m_ext_tex_paths.size() - 1);
}

void EffectRuntime::replace_external_texture_png(int id, const std::filesystem::path& path) {
    const int idx = id - EXTERNAL_TEX_BASE;
    if (idx < 0 || static_cast<size_t>(idx) >= m_ext_tex_paths.size()) return;
    if (m_ext_tex_paths[idx] == path) return;
    m_ext_tex_paths[idx] = path;
    // The backend's ExtTex slot tracks the loaded path; mismatch on next execute()
    // triggers an automatic reset+reload. No explicit invalidation signal needed.
}

void EffectRuntime::set_passes(std::vector<PassDesc> passes) {
    m_passes = std::move(passes);

    // Lazily register the per-frame dispatch counter reset hook here, NOT in
    // execute(). set_passes() is called from the plugin's on_initialize() (or
    // any other non-render-hook context), so cbs->on_present() can safely
    // acquire its writer lock. Calling it from inside execute() would deadlock
    // because the renderer hook holds a shared_lock on the same mutex.
    if (!m_present_hook_registered) {
        m_present_hook_registered = true;
        register_runtime_for_present(this);
        // First runtime in this DLL also installs the on_present callback that
        // dispatches the per-frame counter reset to every registered runtime.
        const bool first_in_dll = !g_present_hook_registered.exchange(true);
        if (first_in_dll) {
            if (auto& a = uevr::API::get()) {
                if (auto* cbs = a->param()->callbacks) {
                    cbs->on_present(&on_present_dispatch_runtimes);
                }
            }
        }
    }
}

void EffectRuntime::request_scene_snapshot_mips(int n) {
    if (n < 1) n = 1;
    m_snapshot_mips = n;
}

void EffectRuntime::release_resources() {
    m_backend.reset();
}

void EffectRuntime::execute() {
    // Compute the per-call pass mask from each pass's declared cadence and the
    // current per-frame dispatch index. This is the recommended path — plugins
    // declare PassDesc::cadence once and never think about per-eye dispatch.
    uint64_t mask = 0;
    const size_t n = std::min<size_t>(m_passes.size(), 64);
    for (size_t i = 0; i < n; ++i) {
        const bool run = (m_passes[i].cadence == Cadence::EveryDispatch) ||
                         (m_dispatch_in_frame == 0);
        if (run) mask |= (uint64_t(1) << i);
    }
    execute(mask);
}

void EffectRuntime::execute(uint64_t pass_mask) {
    // m_dispatch_in_frame is reset to 0 by on_present_dispatch_runtimes
    // (registered in set_passes()); the first execute() after a present is
    // dispatch 0, second is dispatch 1, etc.
    const unsigned dispatch_idx_pre = m_dispatch_in_frame;
    ++m_dispatch_in_frame;

    auto& api    = uevr::API::get();
    const auto rd = (api ? api->param()->renderer : nullptr);

    // ============== DIAGNOSTIC: per-frame cadence tracing ==============
    // Goal: prove or disprove that the per-frame Cadence::OncePerFrame gating
    // works correctly across rendering modes (native stereo, synced sequential,
    // AFR). Captures: dispatch index OBSERVED at entry, pass_mask we will run,
    // scene RT identity for this dispatch, plus a periodic summary line.
    //
    // First k_diag_verbose_calls calls log every dispatch in detail; afterwards
    // we emit one summary line every ~2 seconds with histograms accumulated
    // since the last summary.
    constexpr uint64_t k_diag_verbose_calls = 60;
    ++m_diag_total_dispatches;
    ++m_diag_period_dispatches;
    if (dispatch_idx_pre < 8) ++m_diag_period_idx_seen[dispatch_idx_pre];
    else                      ++m_diag_period_idx_seen[8];
    // Sample the live scene RT pointer for this dispatch (independent of the
    // backend's own logging) — this is the "what UEVR is asking us to process
    // right now" signal. Cheap, just a virtual call.
    void* scene_rt_ptr = nullptr;
    if (auto sr = uevr::API::StereoHook::get_scene_render_target()) {
        scene_rt_ptr = sr->get_native_resource();
    }
    if (scene_rt_ptr && m_diag_period_rt_count < 4) {
        bool seen = false;
        for (int i = 0; i < m_diag_period_rt_count; ++i) {
            if (m_diag_period_rts[i] == scene_rt_ptr) { seen = true; break; }
        }
        if (!seen) m_diag_period_rts[m_diag_period_rt_count++] = scene_rt_ptr;
    }

    if (api && m_diag_total_dispatches <= k_diag_verbose_calls) {
        api->log_info("[fx-diag] execute n=%llu dispatch_idx_pre=%u mask=0x%llx scene_rt=%p tid=%u",
                      (unsigned long long)m_diag_total_dispatches,
                      dispatch_idx_pre,
                      (unsigned long long)pass_mask,
                      scene_rt_ptr,
                      (unsigned)::GetCurrentThreadId());
    }
    // Periodic summary heartbeat after the verbose phase.
    if (api && m_diag_total_dispatches > k_diag_verbose_calls) {
        const uint64_t now_ms = static_cast<uint64_t>(GetTickCount64());
        if (m_diag_last_summary_ms == 0) m_diag_last_summary_ms = now_ms;
        if (now_ms - m_diag_last_summary_ms >= 2000) {
            const uint64_t dt = now_ms - m_diag_last_summary_ms;
            api->log_info("[fx-diag] %llums: dispatches=%llu resets=%llu | "
                          "exec_idx[0..8]=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu | "
                          "frame_dispatch_count[0..8]=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu | "
                          "rts=%d:%p,%p,%p,%p",
                          (unsigned long long)dt,
                          (unsigned long long)m_diag_period_dispatches,
                          (unsigned long long)m_diag_period_resets,
                          (unsigned long long)m_diag_period_idx_seen[0],
                          (unsigned long long)m_diag_period_idx_seen[1],
                          (unsigned long long)m_diag_period_idx_seen[2],
                          (unsigned long long)m_diag_period_idx_seen[3],
                          (unsigned long long)m_diag_period_idx_seen[4],
                          (unsigned long long)m_diag_period_idx_seen[5],
                          (unsigned long long)m_diag_period_idx_seen[6],
                          (unsigned long long)m_diag_period_idx_seen[7],
                          (unsigned long long)m_diag_period_idx_seen[8],
                          (unsigned long long)m_diag_frames_with_dispatch_count[0],
                          (unsigned long long)m_diag_frames_with_dispatch_count[1],
                          (unsigned long long)m_diag_frames_with_dispatch_count[2],
                          (unsigned long long)m_diag_frames_with_dispatch_count[3],
                          (unsigned long long)m_diag_frames_with_dispatch_count[4],
                          (unsigned long long)m_diag_frames_with_dispatch_count[5],
                          (unsigned long long)m_diag_frames_with_dispatch_count[6],
                          (unsigned long long)m_diag_frames_with_dispatch_count[7],
                          (unsigned long long)m_diag_frames_with_dispatch_count[8],
                          m_diag_period_rt_count,
                          m_diag_period_rts[0], m_diag_period_rts[1],
                          m_diag_period_rts[2], m_diag_period_rts[3]);
            // Reset period accumulators (frame-dispatch histogram is cumulative
            // for full-session signal so we don't reset it here).
            m_diag_period_dispatches = 0;
            m_diag_period_resets     = 0;
            for (auto& v : m_diag_period_idx_seen) v = 0;
            for (auto& p : m_diag_period_rts) p = nullptr;
            m_diag_period_rt_count = 0;
            m_diag_last_summary_ms = now_ms;
        }
    }
    // ============== END DIAGNOSTIC ==============

    // Early-exit reason logger — one line per state change. Helps diagnose
    // "I enabled the plugin but see nothing" without spamming.
    auto log_exit = [&](int reason, const char* msg) {
        if (reason == m_last_exit_reason) return;
        m_last_exit_reason = reason;
        if (api) api->log_info("[fx] execute() exit: %s", msg);
    };

    if (m_passes.empty()) { log_exit(0, "no passes set"); return; }
    if (rd == nullptr)    { log_exit(1, "renderer param is null"); return; }

    // Lazily construct the backend matching the active renderer. Recreate if the
    // renderer type changed (e.g. device-reset across DX11/DX12 — defensive).
    // m_last_renderer_type is a member (NOT thread_local): UEVR can dispatch
    // the renderer hook on different threads across the two per-frame eyes,
    // and thread-local state would cause every dispatch to see "changed" and
    // rebuild the backend, wiping shared RTs (LastAdapt history etc.).
    if (m_backend == nullptr || m_last_renderer_type != static_cast<int>(rd->renderer_type)) {
        m_backend.reset();
        if (rd->renderer_type == UEVR_RENDERER_D3D11) {
            api->log_info("[fx] creating DX11 backend (passes=%zu, snapshot_mips=%d)",
                          m_passes.size(), m_snapshot_mips);
            m_backend = make_backend_d3d11();
        } else if (rd->renderer_type == UEVR_RENDERER_D3D12) {
            api->log_info("[fx] creating DX12 backend (passes=%zu, snapshot_mips=%d)",
                          m_passes.size(), m_snapshot_mips);
            m_backend = make_backend_d3d12();
        } else {
            log_exit(2, "unsupported renderer type"); return;
        }
        m_last_renderer_type = static_cast<int>(rd->renderer_type);
    }

    if (m_backend == nullptr) { log_exit(3, "backend creation failed"); return; }

    log_exit(99, "dispatching backend->execute()");

    if (!dispatch_backend_seh(m_backend.get(), m_rt_descs, m_ext_tex_paths, m_passes, m_snapshot_mips, pass_mask)) {
        // Backend AV'd — almost always a stale scene-RT/cmd-list across a VR or
        // device transition. Drop the backend so it gets rebuilt fresh on the
        // next call instead of repeatedly re-entering corrupt state.
        // Rate-limited log: once per fault burst (we'll see one line per real
        // transition, not 60 per second).
        const uint64_t now_ms = static_cast<uint64_t>(GetTickCount64());
        if (now_ms - m_last_fault_log_ms > 1000) {
            m_last_fault_log_ms = now_ms;
            if (api) {
                api->log_warn("[fx] backend faulted in execute(); rebuilding on next frame "
                              "(usually a VR/device transition; safe to ignore unless persistent)");
            }
        }
        m_backend.reset();
        m_last_renderer_type = -1;
        m_last_exit_reason = -2; // re-log on next frame
    }
}

// Populate TLS from the live scene RT. Without this, plugins that gate on
// `scene_width()/height() != 0` before calling execute() never run — execute()
// is what writes the TLS. We re-query every call (not just when TLS is 0) so
// the values stay correct across:
//   - native-stereo-fix dual dispatch (two RTs of different sizes per frame)
//   - render-mode switches (Native <-> Synced Sequential <-> AFR)
//   - in-game resolution / DLSS / FSR changes
//   - device reset
//   - cross-thread reads (TLS would otherwise be stale on other threads)
// GetDesc() is a trivial inline accessor on a cached descriptor; cost is negligible.
static void prime_scene_size_from_api() {
    auto& api = uevr::API::get();
    if (!api) return;
    auto p = api->param();
    if (!p || !p->renderer) return;
    auto rt = uevr::API::StereoHook::get_scene_render_target();
    if (!rt) return;
    auto* native = rt->get_native_resource();
    if (!native) return;
    if (p->renderer->renderer_type == UEVR_RENDERER_D3D11) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(native)->GetDesc(&d);
        if (d.Width && d.Height) {
            detail::tls_scene_w = d.Width;
            detail::tls_scene_h = d.Height;
            detail::tls_scene_fmt = d.Format;
        }
    } else if (p->renderer->renderer_type == UEVR_RENDERER_D3D12) {
        auto rd = static_cast<ID3D12Resource*>(native)->GetDesc();
        if (rd.Width && rd.Height) {
            detail::tls_scene_w = static_cast<unsigned>(rd.Width);
            detail::tls_scene_h = rd.Height;
            detail::tls_scene_fmt = rd.Format;
        }
    }
}

unsigned EffectRuntime::scene_width()  { prime_scene_size_from_api(); return detail::tls_scene_w; }
unsigned EffectRuntime::scene_height() { prime_scene_size_from_api(); return detail::tls_scene_h; }

SceneRTColorSpace EffectRuntime::scene_rt_colorspace() {
    return detail::classify_scene_rt_colorspace(detail::tls_scene_fmt);
}
const char* EffectRuntime::scene_rt_format_name() {
    return detail::dxgi_format_name(detail::tls_scene_fmt);
}

} // namespace uevr::fx
