// uevr::fx — multi-pass effect runtime for UEVR plugins.
//
// Centralizes the ~250 lines of identical DX11+DX12 boilerplate that every
// existing single-pass shader plugin reimplements. Designed to scale from
// "one shader, scene-in scene-out" (LUT, CAS, Vibrance, …) up to multi-pass
// graphs with intermediate render targets (Bloom, AdaptiveTonemapper, …).
//
// Plugins call `EffectRuntime::execute()` from `on_pre_render_vr_framework_dx{11,12}`.
// The backend is auto-selected from `API::get()->param()->renderer->renderer_type`.
//
// See `docs/effect-runtime-plan.md` for the architecture rationale.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <dxgiformat.h>

namespace uevr::fx {

// Magic RT id meaning "the scene render target supplied by UEVR".
// Legal as both an input (auto-snapshot SRV) and an output (write back to scene).
inline constexpr int INPUT_SCENE  = -1;
inline constexpr int OUTPUT_SCENE = -1;

// Magic offset for external-texture ids returned by `load_external_texture_png`.
// Texture ids are >= EXTERNAL_TEX_BASE; intermediate RT ids returned by
// `declare_rt` are 0..N-1; INPUT_SCENE/OUTPUT_SCENE is -1.
inline constexpr int EXTERNAL_TEX_BASE = 1000000;

// Classification of the active scene RT's colorspace, surfaced so plugins can
// warn the user when the format is ambiguous (DXGI provides no metadata to
// distinguish a linear `*_UNORM` RT from a gamma-encoded one). A faithful
// ReShade-style port assumes linear math in the pixel shader; the runtime
// relies on format-correct GPU views (sRGB-typed views auto-decode/encode) for
// gamma round-trip — but for plain UNORM there is no such guarantee.
enum class SceneRTColorSpace {
    Unknown,         // unrecognised or not yet sampled
    LinearFloat,     // *_FLOAT  — safe, no encode/decode needed
    SRGBTyped,       // *_UNORM_SRGB — GPU does sRGB<->linear automatically via the typed view
    AmbiguousUNORM,  // plain *_UNORM / R10G10B10A2_UNORM — could be linear or gamma-encoded
};

// Per-pass execution cadence within a swapchain frame. UEVR's native-stereo-fix
// dispatches the renderer hook twice per frame (once per eye) into the SAME
// command list. Some passes (e.g. an eye-adaptation EMA history update) are
// global frame state and must run exactly ONCE per swapchain frame to remain
// faithful to the upstream ReShade behavior; others (e.g. a per-eye tonemap)
// must run on every dispatch.
//
// The runtime resets its per-frame dispatch counter on the first `on_present`
// after construction and increments it on every `execute()` call. Passes flagged
// `OncePerFrame` are skipped on dispatches ≥ 2 of the current frame; their
// outputs MUST be on RTs flagged `RTDesc::shared_across_scene_slots = true` so
// later-dispatch passes can read what the first dispatch wrote.
enum class Cadence {
    EveryDispatch, // default — runs on every execute() call (every eye)
    OncePerFrame,  // runs only on the first execute() of each swapchain frame
};

struct RTDesc {
    enum class SizeMode { Backbuffer, BackbufferDiv, Fixed };
    SizeMode    size_mode  = SizeMode::Backbuffer;
    int         w_or_div   = 1;
    int         h_or_div   = 1;
    DXGI_FORMAT format     = DXGI_FORMAT_R16G16B16A16_FLOAT;
    int         mip_levels = 1;
    bool        persistent = false;   // not freed between frames; identity-keyed on scene RT
    // When true the runtime generates mips 1..mip_levels-1 after every pass that
    // writes to mip 0 of this RT. DX11 uses ID3D11DeviceContext::GenerateMips();
    // DX12 runs a PS-based 2x box-downsample chain. mip_levels must be >= 2.
    bool        auto_generate_mips = false;
    // When true, this RT is allocated once at the backend level and shared across
    // all SceneSlots, instead of being instantiated per-slot. Use for persistent
    // history RTs (e.g. AdaptiveTonemapper LastAdapt) so that native-stereo-fix's
    // dual scene-RT dispatch converges to a single shared adaptation value rather
    // than producing per-eye drift. Per-frame scratch RTs (read+written within a
    // single execute()) MUST remain per-slot to avoid races between dispatches.
    // Implies `persistent` semantics for sizing (size locked at first allocation);
    // pair with `size_mode = Fixed` to be size-stable across scene-RT changes.
    bool        shared_across_scene_slots = false;
};

struct PassDesc {
    // HLSL pixel-shader source. Pointer must be stable across the plugin's lifetime
    // (string literal or static const); used as PSO cache key.
    const char*       ps_hlsl       = nullptr;
    std::vector<int>  inputs;                                  // RT ids or external-texture ids
    int               output        = OUTPUT_SCENE;            // RT id (OUTPUT_SCENE allowed)
    int               output_mip    = 0;
    const void*       cb_data       = nullptr;
    std::size_t       cb_size       = 0;
    DXGI_FORMAT       output_format_override = DXGI_FORMAT_UNKNOWN;
    // Opt-in: HDR-luminance shaders (AdaptiveTonemapper, EyeAdaption, ...) that
    // do log/pow/ACES/Reinhard math need scene values in linear space. When this
    // flag is true and the scene RT is `AmbiguousUNORM`, the runtime injects
    //   #define fx_decode_scene(c)  pow(max((c), 0.0), 2.2)
    //   #define fx_encode_scene(c)  pow(max((c), 0.0), 1.0/2.2)
    // into the HLSL preamble; for `LinearFloat` and `SRGBTyped` the macros are
    // identity. The pass's PS must wrap scene reads with `fx_decode_scene(...)`
    // and (if writing back to scene) the final color with `fx_encode_scene(...)`.
    // Default false so the 16 already-shipped LDR/format-agnostic plugins do
    // not regress (they sample scene directly with no encode/decode).
    bool              needs_scene_colorspace_decode = false;
    // Per-frame execution cadence. See `Cadence`. Default `EveryDispatch`.
    Cadence           cadence = Cadence::EveryDispatch;
};

// Opaque backend state. Defined in effect_runtime_d3d{11,12}.cpp.
class EffectBackend;

class EffectRuntime {
public:
    EffectRuntime();
    ~EffectRuntime();

    EffectRuntime(const EffectRuntime&) = delete;
    EffectRuntime& operator=(const EffectRuntime&) = delete;

    // Declares an intermediate render target. Returns its id (>= 0).
    int declare_rt(const RTDesc& desc);

    // Loads a PNG from disk (RGBA8). Returns an external-texture id (>= EXTERNAL_TEX_BASE)
    // usable in `PassDesc::inputs`, or -1 on failure. Texture is uploaded lazily on the
    // next `execute()` once the renderer device is known.
    int load_external_texture_png(const std::filesystem::path& path);

    // Hot-swap the source path for a previously-loaded external texture id (returned
    // by `load_external_texture_png`). The next `execute()` detects the path change
    // and reloads transparently — no plugin restart, no pass-graph rebuild needed.
    // No-op if `id` is invalid or refers to the same path already loaded.
    void replace_external_texture_png(int id, const std::filesystem::path& path);

    // Replaces the pass list. Call once at init or whenever pass parameters change shape.
    // (Per-frame cbuffer values should be updated via PassDesc::cb_data; the runtime
    // re-reads cb_data each execute.)
    //
    // THREADING CONTRACT: must be called from `Plugin::on_initialize()` (or any
    // other non-renderer-hook context). The first call in a DLL lazily registers
    // an on_present callback via `cbs->on_present(...)`, which takes a writer
    // lock on PluginLoader's `m_api_cb_mtx`. The renderer hooks
    // (`on_pre/post_render_vr_framework_dx{11,12}`) and `on_present` itself hold
    // a reader lock on that same mutex while iterating callbacks, so calling
    // `set_passes()` from inside any of those will deadlock the same thread.
    void set_passes(std::vector<PassDesc> passes);

    // Request the scene snapshot to carry an N-level mip chain. Mips 1..N-1 are
    // regenerated each frame after the per-frame snapshot copy. Required by
    // ports like Bloom that sample BackBuffer at non-zero LOD. Default is 1
    // (no mip chain). Call once at init; takes effect on next execute().
    void request_scene_snapshot_mips(int n);

    // Releases all GPU resources. Call from `on_device_reset`.
    void release_resources();

    // Executes the pass graph against the current scene RT. No-op if no passes are set
    // or the renderer is in an unsupported state. Backend chosen from UEVR renderer type.
    void execute();

    // Same as execute() but only dispatches passes whose bit is set in `pass_mask`
    // (bit i corresponds to pass index i; supports up to 64 passes). Useful for
    // multi-pass effects with native-stereo-fix where only some passes (e.g. an
    // adaptation history update) should run once per frame, while the main
    // tonemap pass runs for both eyes. Snapshot phase always runs.
    //
    // NOTE: prefer declarative `PassDesc::cadence` over manual masks. The plain
    // `execute()` overload computes the mask from each pass's cadence + the
    // runtime-maintained per-frame dispatch counter, so plugin code does not
    // need to know about per-eye dispatch arithmetic.
    void execute(uint64_t pass_mask);

    // Convenience accessors for common cbuffer values (valid only during execute()).
    static unsigned scene_width();
    static unsigned scene_height();

    // True when the next execute() call will be the first dispatch of the
    // current swapchain frame. Resets to true after every on_present.
    // Plugins use this to update wall-clock-per-frame state (e.g. FrameTime
    // for temporal EMAs) so the value is independent of how many times the
    // renderer hook fires per HMD frame (which differs across
    // Native Stereo / Synchronized Sequential / Alternating-AFR modes).
    // Faithfulness contract: in ReShade these values are per-game-frame; in
    // VR we treat one HMD-frame Present as the equivalent boundary.
    bool is_first_dispatch_in_frame() const { return m_dispatch_in_frame == 0; }

    // Classification of the most-recently-seen scene RT format. Updated on each
    // execute() — read after on_pre_render_vr_framework_dx{11,12} has run at
    // least once. Returns Unknown before then.
    static SceneRTColorSpace scene_rt_colorspace();
    // Static C-string with the DXGI format name (e.g. "R16G16B16A16_FLOAT").
    // Returns "<unknown>" before any execute() has run.
    static const char*       scene_rt_format_name();

    // Internal: invoked by the runtime's own on_present callback to reset the
    // per-frame dispatch counter at swapchain frame boundaries. Public only
    // because the callback lives in an anonymous namespace in the cpp; not
    // intended to be called by plugins.
    //
    // The callback also captures the pre-reset value into the diagnostic
    // histogram (m_diag_*) so we can see, in summary logs, how many dispatches
    // each just-finished frame actually contained. This is the key signal for
    // diagnosing per-frame-cadence bugs across native_stereo / synced_sequential
    // / AFR rendering modes.
    void _internal_reset_dispatch_in_frame() {
        // Pre-reset diagnostic capture.
        if (m_dispatch_in_frame < 8) {
            ++m_diag_frames_with_dispatch_count[m_dispatch_in_frame];
        } else {
            ++m_diag_frames_with_dispatch_count[8]; // ">=8" bucket
        }
        ++m_diag_present_resets;
        ++m_diag_period_resets;
        m_dispatch_in_frame = 0;
    }

private:
    std::unique_ptr<EffectBackend> m_backend;
    // Storage for declarations is owned here so backends can be torn down + recreated
    // without losing the user's declared graph.
    std::vector<RTDesc>             m_rt_descs;
    std::vector<std::filesystem::path> m_ext_tex_paths;
    std::vector<PassDesc>           m_passes;
    int                             m_snapshot_mips = 1;
    // Per-frame dispatch bookkeeping. `m_dispatch_in_frame` increments on every
    // `execute()` call and is reset to 0 by an on_present callback that the
    // runtime registers lazily on first execute(). Used by `Cadence::OncePerFrame`
    // to gate passes to the first dispatch of each swapchain frame.
    unsigned                        m_dispatch_in_frame = 0;
    bool                            m_present_hook_registered = false;
    // Active backend's renderer type (UEVR_RENDERER_D3D11 / D3D12), or -1 if no
    // backend constructed yet. NOT thread_local — UEVR can dispatch the renderer
    // hook on different threads across the two per-frame eyes; thread-local
    // state would cause every dispatch to think the type changed, tearing down
    // and rebuilding the backend (and wiping shared RTs) on every call.
    int                             m_last_renderer_type = -1;
    // Diagnostic / rate-limiting state. Members (not thread_local) so values
    // are stable across dispatches that may land on different threads.
    int                             m_last_exit_reason     = -2;
    uint64_t                        m_last_fault_log_ms    = 0;

    // -------- Per-frame cadence diagnostics --------
    // Verbose per-dispatch logs for the first N calls (covers ~1s @ 60fps),
    // then periodic summary heartbeats every ~2s. Kept in members so the data
    // survives device-resets / backend rebuilds and is accumulated across the
    // whole session.
    uint64_t                        m_diag_total_dispatches = 0;
    uint64_t                        m_diag_present_resets   = 0;
    // Histogram: how many wall-clock frames (per on_present reset) ended with
    // exactly N dispatches having happened in that frame. Index 0 = "frame had
    // no dispatches before reset", 1 = "one dispatch (typical synced single)",
    // 2 = "two dispatches (typical native stereo fix)", ... 8 = ">=8 bucket".
    uint64_t                        m_diag_frames_with_dispatch_count[9] = {0};
    // Per-period (between summary heartbeats) counters; reset each emission.
    uint64_t                        m_diag_period_dispatches  = 0;
    uint64_t                        m_diag_period_resets      = 0;
    // Histogram of dispatch_in_frame values OBSERVED at execute() entry.
    // Index = m_dispatch_in_frame value just before increment.
    // Tells us: did dispatch 0 / 1 / 2 / ... actually fire this period.
    uint64_t                        m_diag_period_idx_seen[9] = {0};
    // Up to 4 distinct scene-RT identities observed this period (cap to keep
    // log line bounded). Reset each emission.
    void*                           m_diag_period_rts[4]       = {nullptr,nullptr,nullptr,nullptr};
    int                             m_diag_period_rt_count     = 0;
    uint64_t                        m_diag_last_summary_ms     = 0;

    friend class EffectBackend;
};

// ----------------------------------------------------------------------------
// SinglePassEffect<CB> — the 90% case wrapper.
//
// Plugin usage:
//   struct MyCB { float a, b; float pad[2]; };
//   static const char* g_ps = R"(...)";
//   class MyPlugin : public uevr::Plugin {
//       fx::SinglePassEffect<MyCB> m_fx{g_ps};
//       void on_initialize() override { m_fx.init(); }
//       void on_pre_render_vr_framework_dx11() override { run(); }
//       void on_pre_render_vr_framework_dx12() override { run(); }
//       void run() {
//           if (!m_enabled) return;
//           m_fx.set_cb({m_a, m_b, {0,0}});
//           m_fx.execute();
//       }
//   };
//
// IMPORTANT: the ctor only stores `ps_hlsl`; the underlying `set_passes()` call
// is deferred to `init()` because plugin instances are typically constructed at
// DLL static-init time (before UEVR's API singleton is initialised) and
// `set_passes()` reaches `uevr::API::get()` to register an on_present callback.
// Calling `set_passes()` during DLL load AVs and the plugin fails to load.
// `init()` must be called from `Plugin::on_initialize()` (or any other
// non-renderer-hook context) — see the THREADING CONTRACT on
// `EffectRuntime::set_passes()`.
// ----------------------------------------------------------------------------
template <typename CB>
class SinglePassEffect {
public:
    explicit SinglePassEffect(const char* ps_hlsl) : m_ps_hlsl(ps_hlsl) {}

    // Configures the underlying runtime with a single scene-in/scene-out pass.
    // Call from `Plugin::on_initialize()`. Idempotent.
    void init() {
        if (m_initialized) return;
        m_initialized = true;
        PassDesc pass;
        pass.ps_hlsl = m_ps_hlsl;
        pass.inputs  = { INPUT_SCENE };
        pass.output  = OUTPUT_SCENE;
        pass.cb_data = &m_cb;
        pass.cb_size = sizeof(CB);
        m_runtime.set_passes({ pass });
    }

    void set_cb(const CB& cb) { m_cb = cb; }
    void execute()            { m_runtime.execute(); }
    void release_resources()  { m_runtime.release_resources(); }

private:
    const char*    m_ps_hlsl   = nullptr;
    bool           m_initialized = false;
    CB             m_cb{};
    EffectRuntime  m_runtime;
};

} // namespace uevr::fx
