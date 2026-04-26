/*
Adaptive Tonemapper Plugin for UEVR
===================================
Port of luluco250/FXShaders AdaptiveTonemapper.fx (with embedded ACES.fxh) to
UEVR's effect runtime. Applied to the UE scene render target in
on_pre_render_vr_framework (DX11/DX12).

Based on the ReShade shader by luluco250 (Lucas Melo):
  https://github.com/luluco250/FXShaders/blob/master/Shaders/AdaptiveTonemapper.fx

Original AdaptiveTonemapper shader:
  Copyright (c) 2017 Lucas Melo
  License: MIT (per repository LICENSE,
    https://github.com/luluco250/FXShaders/blob/master/LICENSE)

Embedded ACES.fxh (Baking Lab):
  by MJP and David Neubelt; ACES fit by Stephen Hill (@self_shadow)
  https://github.com/luluco250/FXShaders/blob/master/Shaders/ACES.fxh
  License: MIT (per file header)

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Three-pass effect with a persistent 1x1 R32F history texture for temporal
eye-adaptation:

  Pass 1: GetSmall   — luminance dot to 256x256 R32F + temporal lerp w/ history.
                       Runtime auto-generates 9-mip chain afterward.
  Pass 2: SaveAdapt  — sample SmallTex at LOD (9 - Precision) → write 1x1 history.
  Pass 3: Main       — tonemap (Reinhard / Uncharted2 / ACES) the scene by
                       exposure derived from history; optional white-point fix.

The original VS computed `inv_white` and `exposure` per-vertex and passed them via
TEXCOORD1/TEXCOORD2. Our backend uses a fixed UV-only fullscreen VS, so we
compute both inside MainPS (mathematically identical for a fullscreen quad).

KNOWN ISSUE — mode-dependent brightness (2026-04-26)
----------------------------------------------------
The same `adapt`/`exposure` values produce a visibly brighter scene in
"Synchronized Sequential" than in "Native Stereo" (~1 decade / ~3.3 EV
shift on the calibration-bars debug visualizer). Investigation outcome:

  * The `[fx]` runtime hands plugins the SAME scene RT in both modes
    (`B8G8R8A8_UNORM 8136x4016 -> AmbiguousUNORM`, single identity logged
    across many mode switches).
  * The plugin's own pipeline (FrameTime gating, EMA via LastAdapt, MainPS
    binding LastAdapt) is a faithful ReShade port — verified per-mode via
    log-sampled FrameTime sequences.
  * The divergence is DOWNSTREAM of our hook in
    `D3D12Component::on_frame()`: synced_sequential reports
    `is_using_afr=1` (verified in log.txt) and is composited via the
    AFR_LEFT_EYE/AFR_RIGHT_EYE swapchain copy path; native_stereo
    (`is_using_afr=0`) is composited via the DOUBLE_WIDE swapchain copy.
    All swapchains use `B8G8R8A8_UNORM_SRGB` but the two copy paths
    apparently apply different sRGB gamma encoding when reinterpreting
    our `B8G8R8A8_UNORM` writes for the SRGB swapchain.
  * Confirmed via the Debug TreeNode "Calibration bars" visualizer
    (writes fixed reference colors → still differs per mode → proves
    the difference is post-our-hook).

This is a UEVR core compositing issue, not a plugin/EMA issue. Fixing it
likely requires harmonising the AFR vs non-AFR swapchain-copy paths in
`src/mods/vr/D3D12Component.cpp` (e.g. consistent format reinterpretation
on the source SRV, or shader-copy in both branches).

The Debug TreeNode (heatmap visualizers + calibration bars) is kept in
the release build for future investigation of similar issues.
*/

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"
#include "effects/scene_warning.hpp"

using namespace uevr;

static constexpr const char* ATM_VERSION         = "1.0.0";
static constexpr int          ATM_SMALL_TEX_SIZE = 256;
static constexpr int          ATM_MIP_LEVELS     = 9; // log2(256) + 1

// ---------------------------------------------------------------------------
// CB — 48 bytes, three 16-byte rows, HLSL natural packing.
// Row 0: AdaptRange.xy / AdaptTime / AdaptSensitivity
// Row 1: AdaptFocalPoint.xy / Amount / Exposure
// Row 2: TonemapOperator(i) / FixWhitePoint(i) / AdaptPrecision(i) / FrameTime
// ---------------------------------------------------------------------------
#pragma pack(push, 4)
struct TonemapCB {
    float   AdaptRange[2];
    float   AdaptTime;
    float   AdaptSensitivity;

    float   AdaptFocalPoint[2];
    float   Amount;
    float   Exposure;

    int32_t TonemapOperator;
    int32_t FixWhitePoint;
    int32_t AdaptPrecision;
    float   FrameTime;

    int32_t DebugVisualizeAdapt; // 0 = normal, 1 = output flat grey = adapt, 2 = output flat grey = exposure-after-clamp
    int32_t _pad0; int32_t _pad1; int32_t _pad2;
};
#pragma pack(pop)
static_assert(sizeof(TonemapCB) == 64, "TonemapCB must be 64 bytes");

// ---------------------------------------------------------------------------
// HLSL preamble — cbuffer + helpers + ACES.
// ---------------------------------------------------------------------------
#define ATM_HLSL_PREAMBLE R"(
cbuffer TonemapCB : register(b0) {
    float2 AdaptRange;
    float  AdaptTime;
    float  AdaptSensitivity;
    float2 AdaptFocalPoint;
    float  Amount;
    float  Exposure;
    int    TonemapOperator;
    int    FixWhitePoint;
    int    AdaptPrecision;
    float  FrameTime;
    int    DebugVisualizeAdapt;
    int    _pad0; int _pad1; int _pad2;
};
SamplerState LinearSampler : register(s0);
struct PSI { float4 P : SV_Position; float2 uv : TEXCOORD0; };

static const float3 LumaWeights = float3(0.299, 0.587, 0.114);
static const int    AdaptMipLevels = )" "9" R"(;

static const float3x3 ACESInputMat = float3x3(
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777);
static const float3x3 ACESOutputMat = float3x3(
     1.60475, -0.53108, -0.07367,
    -0.10208,  1.10813, -0.00605,
    -0.00327, -0.07276,  1.07602);

float3 RRTAndODTFit(float3 v) {
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}
float3 ACESFitted(float3 color) {
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
}
float3 reinhard(float3 color) { return color / (1.0 + color); }
float3 uncharted2_tonemap(float3 col, float exposure) {
    static const float A = 0.15;
    static const float B = 0.50;
    static const float C = 0.10;
    static const float D = 0.20;
    static const float E = 0.02;
    static const float F = 0.30;
    static const float W = 11.2;
    col *= exposure;
    col = ((col * (A * col + C * B) + D * E) / (col * (A * col + B) + D * F)) - E / F;
    static const float white = 1.0 / (((W * (A * W + C * B) + D * E) / (W * (A * W + B) + D * F)) - E / F);
    col *= white;
    return col;
}
float3 tonemap(float3 color, float exposure) {
    if (TonemapOperator == 0) return reinhard(color * exposure);
    if (TonemapOperator == 1) return uncharted2_tonemap(color, exposure);
    if (TonemapOperator == 2) return ACESFitted(color * exposure);
    return float3(0,0,0);
}
)"

// Pass 1: GetSmall — Scene + LastAdapt (history) → SmallTex (mip 0)
//   t0 = Scene, t1 = LastAdapt (1x1 R32F)
// `fx_decode_scene` is supplied by the runtime preamble (linearizes UNORM scene).
static const char* g_ps_get_small = ATM_HLSL_PREAMBLE R"(
Texture2D Scene     : register(t0);
Texture2D LastAdapt : register(t1);
float4 main(PSI i) : SV_Target {
    float3 scene_lin = fx_decode_scene(Scene.SampleLevel(LinearSampler, i.uv, 0).rgb);
    float adapt = dot(scene_lin, LumaWeights);
    adapt *= AdaptSensitivity;
    float last = LastAdapt.Load(int3(0, 0, 0)).x;
    if (AdaptTime > 0.0) {
        adapt = lerp(last, adapt, saturate((FrameTime * 0.001) / AdaptTime));
    }
    return float4(adapt, 0, 0, 0);
}
)";

// Pass 2: SaveAdapt — sample SmallTex at the precision-selected LOD → 1x1 history
//   t0 = SmallTex (with auto-generated mips)
static const char* g_ps_save_adapt = ATM_HLSL_PREAMBLE R"(
Texture2D Small : register(t0);
float4 main(PSI i) : SV_Target {
    float lod = (float)(AdaptMipLevels - AdaptPrecision);
    float adapt = Small.SampleLevel(LinearSampler, AdaptFocalPoint, lod).x;
    return float4(adapt, 0, 0, 0);
}
)";

// Pass 3: Main — tonemap scene by adapted exposure
//   t0 = Scene, t1 = LastAdapt (1x1 R32F temporally-smoothed history),
//   t2 = SmallTex (current-frame downsampled luma + auto mip chain) — used
//        only for debug visualizers; ignored on the main tonemap path.
static const char* g_ps_main = ATM_HLSL_PREAMBLE R"(
Texture2D Scene     : register(t0);
Texture2D LastAdapt : register(t1);
Texture2D Small     : register(t2);

// Heatmap colormap: maps t in [0,1] to blue→cyan→green→yellow→red.
// Tiny variations in the input value are clearly visible as color shifts.
float3 heatmap(float t) {
    t = saturate(t);
    float3 c;
    if (t < 0.25)      { float k = t / 0.25;          c = lerp(float3(0,0,1),   float3(0,1,1),   k); }
    else if (t < 0.5)  { float k = (t - 0.25) / 0.25; c = lerp(float3(0,1,1),   float3(0,1,0),   k); }
    else if (t < 0.75) { float k = (t - 0.5)  / 0.25; c = lerp(float3(0,1,0),   float3(1,1,0),   k); }
    else               { float k = (t - 0.75) / 0.25; c = lerp(float3(1,1,0),   float3(1,0,0),   k); }
    return c;
}

// Map a positive value to [0,1] via log10 across `decades` from `min_val`.
// e.g. log_norm(v, 1e-4, 5) maps 1e-4..10 -> 0..1.
float log_norm(float v, float min_val, float decades) {
    return saturate((log2(max(v, 1e-30)) - log2(min_val)) / (decades * log2(10.0)));
}

float4 main(PSI i) : SV_Target {
    float exposure = exp2(Exposure);
    float adapt = LastAdapt.Load(int3(0, 0, 0)).x;
    adapt = clamp(adapt, AdaptRange.x, AdaptRange.y);
    exposure /= adapt;

    // ============================================================
    // Debug visualizers — isolate where mode-dependent brightness
    // enters the pipeline. Each returns a heatmap-colored screen so
    // small differences are visible. With a 5-decade log scale a 2x
    // value difference shows as a clear color shift.
    //   1: adapt (LastAdapt history) on log scale 1e-4..10
    //   2: 1/adapt (exposure scale) on log scale 1..1e5
    //   3: SmallTex mip0 at center (per-pixel current luma)
    //   4: SmallTex mip8 at center (full-screen avg current luma
    //      — this is the EMA INPUT; if THIS differs across modes
    //      the bug is upstream in the scene RT contents)
    //   5: tonemapped output luma (final pipeline, debug-coloured)
    //   6: |native_stereo - synced_seq adapt| reference card:
    //      shows fixed legend bars for visual calibration
    // ============================================================
    if (DebugVisualizeAdapt == 1) {
        return float4(heatmap(log_norm(adapt, 1e-4, 5.0)), 1.0);
    }
    if (DebugVisualizeAdapt == 2) {
        return float4(heatmap(log_norm(exposure, 1.0, 5.0)), 1.0);
    }
    if (DebugVisualizeAdapt == 3) {
        float v = Small.SampleLevel(LinearSampler, float2(0.5,0.5), 0).x;
        return float4(heatmap(log_norm(v, 1e-4, 5.0)), 1.0);
    }
    if (DebugVisualizeAdapt == 4) {
        float v = Small.SampleLevel(LinearSampler, float2(0.5,0.5), 8).x;
        return float4(heatmap(log_norm(v, 1e-4, 5.0)), 1.0);
    }
    if (DebugVisualizeAdapt == 5) {
        float4 c = Scene.SampleLevel(LinearSampler, i.uv, 0);
        float3 lin = fx_decode_scene(c.rgb);
        float3 tm = tonemap(lin, exposure);
        float lum = dot(tm, LumaWeights);
        return float4(heatmap(log_norm(lum, 1e-3, 4.0)), 1.0);
    }
    if (DebugVisualizeAdapt == 6) {
        // Calibration: 5 vertical bars, log-spaced reference values
        // 1e-4, 1e-3, 1e-2, 1e-1, 1.0 — lets you read off other
        // modes by eye-matching their color to a bar.
        float band = floor(i.uv.x * 5.0);
        float ref = pow(10.0, band - 4.0);
        return float4(heatmap(log_norm(ref, 1e-4, 5.0)), 1.0);
    }

    float inv_white = 1.0;
    if (FixWhitePoint != 0) {
        inv_white = rcp(tonemap(float3(1,1,1), exposure).x);
    }
    float4 color = Scene.SampleLevel(LinearSampler, i.uv, 0);
    float3 lin = fx_decode_scene(color.rgb);
    lin = lerp(lin, tonemap(lin, exposure) * inv_white, Amount);
    color.rgb = fx_encode_scene(lin);
    return color;
}
)";

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

// Default parameter values — mirror the original luluco250 AdaptiveTonemapper.fx
// uniform defaults so "Reset to Defaults" matches the upstream shader.
static constexpr int   DEFAULT_TONEMAP_OPERATOR = 2;     // ACES
static constexpr float DEFAULT_AMOUNT           = 1.0f;
static constexpr float DEFAULT_EXPOSURE         = 0.0f;
static constexpr bool  DEFAULT_FIX_WHITE_POINT  = true;
static constexpr float DEFAULT_ADAPT_RANGE_MIN  = 0.001f;
static constexpr float DEFAULT_ADAPT_RANGE_MAX  = 1.0f;
static constexpr float DEFAULT_ADAPT_TIME       = 1.0f;
static constexpr float DEFAULT_ADAPT_SENSITIVITY= 1.0f;
static constexpr int   DEFAULT_ADAPT_PRECISION  = 0;
static constexpr float DEFAULT_FOCAL_POINT_X    = 0.5f;
static constexpr float DEFAULT_FOCAL_POINT_Y    = 0.5f;

class AdaptiveTonemapperPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool m_enabled = false;

    int   m_TonemapOperator = DEFAULT_TONEMAP_OPERATOR;
    float m_Amount          = DEFAULT_AMOUNT;
    float m_Exposure        = DEFAULT_EXPOSURE;
    bool  m_FixWhitePoint   = DEFAULT_FIX_WHITE_POINT;
    float m_AdaptRange[2]   = { DEFAULT_ADAPT_RANGE_MIN, DEFAULT_ADAPT_RANGE_MAX };
    float m_AdaptTime       = DEFAULT_ADAPT_TIME;
    float m_AdaptSensitivity= DEFAULT_ADAPT_SENSITIVITY;
    int   m_AdaptPrecision  = DEFAULT_ADAPT_PRECISION;
    float m_AdaptFocalPoint[2] = { DEFAULT_FOCAL_POINT_X, DEFAULT_FOCAL_POINT_Y };
    int   m_DebugVisualizeAdapt = 0; // 0=normal, 1=show adapt as grey, 2=show exposure as grey

    TonemapCB         m_cb{};
    fx::EffectRuntime m_runtime;
    int               m_smalltex_id  = -1;
    int               m_lastadapt_id = -1;
    bool              m_passes_set   = false;
    uint64_t          m_last_frame_ms = 0;

    void on_initialize() override {
        API::get()->log_info("[AdaptiveTonemapper] Plugin initialized (v%s)", ATM_VERSION);
        configure_runtime();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "AdaptiveTonemapper"; }
    int render_order() const override { return 400; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",             m_enabled        ? "1" : "0"},
            {"tonemap_operator",    std::to_string(m_TonemapOperator)},
            {"amount",              std::to_string(m_Amount)},
            {"exposure",            std::to_string(m_Exposure)},
            {"fix_white_point",     m_FixWhitePoint  ? "1" : "0"},
            {"adapt_range.0",       std::to_string(m_AdaptRange[0])},
            {"adapt_range.1",       std::to_string(m_AdaptRange[1])},
            {"adapt_time",          std::to_string(m_AdaptTime)},
            {"adapt_sensitivity",   std::to_string(m_AdaptSensitivity)},
            {"adapt_precision",     std::to_string(m_AdaptPrecision)},
            {"adapt_focal_point.0", std::to_string(m_AdaptFocalPoint[0])},
            {"adapt_focal_point.1", std::to_string(m_AdaptFocalPoint[1])},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* k, float& out) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { out = std::stof(it->second); } catch (...) {}
        };
        auto get_int = [&](const char* k, int& out) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { out = std::stoi(it->second); } catch (...) {}
        };
        auto get_bool = [&](const char* k, bool& out) {
            auto it = kv.find(k);
            if (it != kv.end()) out = (it->second != "0" && !it->second.empty());
        };
        get_bool("enabled",              m_enabled);
        get_int("tonemap_operator",      m_TonemapOperator);
        get_float("amount",              m_Amount);
        get_float("exposure",            m_Exposure);
        get_bool("fix_white_point",      m_FixWhitePoint);
        get_float("adapt_range.0",       m_AdaptRange[0]);
        get_float("adapt_range.1",       m_AdaptRange[1]);
        get_float("adapt_time",          m_AdaptTime);
        get_float("adapt_sensitivity",   m_AdaptSensitivity);
        get_int("adapt_precision",       m_AdaptPrecision);
        get_float("adapt_focal_point.0", m_AdaptFocalPoint[0]);
        get_float("adapt_focal_point.1", m_AdaptFocalPoint[1]);
    }

    void reset_to_defaults() override {
        m_enabled            = false;
        m_TonemapOperator    = DEFAULT_TONEMAP_OPERATOR;
        m_Amount             = DEFAULT_AMOUNT;
        m_Exposure           = DEFAULT_EXPOSURE;
        m_FixWhitePoint      = DEFAULT_FIX_WHITE_POINT;
        m_AdaptRange[0]      = DEFAULT_ADAPT_RANGE_MIN;
        m_AdaptRange[1]      = DEFAULT_ADAPT_RANGE_MAX;
        m_AdaptTime          = DEFAULT_ADAPT_TIME;
        m_AdaptSensitivity   = DEFAULT_ADAPT_SENSITIVITY;
        m_AdaptPrecision     = DEFAULT_ADAPT_PRECISION;
        m_AdaptFocalPoint[0] = DEFAULT_FOCAL_POINT_X;
        m_AdaptFocalPoint[1] = DEFAULT_FOCAL_POINT_Y;
    }
    // ----------------------------------------------------------------------

    void configure_runtime() {
        if (m_passes_set) return;

        // SmallTex: 256x256 R32_FLOAT with 9-level mip chain, auto-generated after writes.
        // Shared across SceneSlots so dispatch 1's Main can read the chain that
        // dispatch 0's GetSmall wrote (the runtime gates GetSmall+SaveAdapt to
        // dispatch 0 only via PassDesc::cadence below).
        fx::RTDesc small_rt{};
        small_rt.size_mode                 = fx::RTDesc::SizeMode::Fixed;
        small_rt.w_or_div                  = ATM_SMALL_TEX_SIZE;
        small_rt.h_or_div                  = ATM_SMALL_TEX_SIZE;
        small_rt.format                    = DXGI_FORMAT_R32_FLOAT;
        small_rt.mip_levels                = ATM_MIP_LEVELS;
        small_rt.auto_generate_mips        = true;
        small_rt.shared_across_scene_slots = true;
        m_smalltex_id = m_runtime.declare_rt(small_rt);

        // LastAdaptTex: 1x1 R32_FLOAT, persists across frames (history).
        fx::RTDesc last_rt{};
        last_rt.size_mode  = fx::RTDesc::SizeMode::Fixed;
        last_rt.w_or_div   = 1;
        last_rt.h_or_div   = 1;
        last_rt.format     = DXGI_FORMAT_R32_FLOAT;
        last_rt.mip_levels = 1;
        last_rt.persistent = true;
        // Native-stereo-fix dispatches our renderer hook twice per frame against
        // two DIFFERENT scene RTs (main + scene_capture), each spawning its own
        // SceneSlot. With a per-slot LastAdapt, the two histories converge to
        // different steady-state exposures — a stable inter-eye brightness
        // mismatch. Promote LastAdapt to a single backend-resident texture so
        // both dispatches read+write the same adaptation value. SmallTex stays
        // per-slot (per-frame scratch — sharing it would race between dispatches).
        last_rt.shared_across_scene_slots = true;
        m_lastadapt_id  = m_runtime.declare_rt(last_rt);

        std::vector<fx::PassDesc> passes;
        passes.reserve(3);

        auto make_pass = [&](const char* hlsl, std::vector<int> inputs, int output, bool decode,
                              fx::Cadence cadence = fx::Cadence::EveryDispatch) {
            fx::PassDesc p;
            p.ps_hlsl = hlsl;
            p.inputs  = std::move(inputs);
            p.output  = output;
            p.cb_data = &m_cb;
            p.cb_size = sizeof(m_cb);
            p.needs_scene_colorspace_decode = decode;
            p.cadence = cadence;
            passes.push_back(std::move(p));
        };

        // GetSmall + SaveAdapt are global frame state: faithful to ReShade, the
        // EMA history step must advance exactly once per wall-clock frame, not
        // once per eye. The runtime gates them to dispatch 0 of each frame via
        // PassDesc::cadence; dispatch 1's Main reads the shared SmallTex/LastAdapt.
        // GetSmall reads INPUT_SCENE + LastAdapt → writes SmallTex (mip 0 + auto mips).
        make_pass(g_ps_get_small,  { fx::INPUT_SCENE, m_lastadapt_id }, m_smalltex_id,    true,  fx::Cadence::OncePerFrame);
        // SaveAdapt reads SmallTex (now full mip chain) → writes LastAdapt (history).
        make_pass(g_ps_save_adapt, { m_smalltex_id },                   m_lastadapt_id,   false, fx::Cadence::OncePerFrame);
        // Main reads INPUT_SCENE + LastAdapt + SmallTex → writes OUTPUT_SCENE.
        // SmallTex is bound only so the debug visualizers can sample it — the
        // production tonemap path uses LastAdapt only (faithful to ReShade).
        make_pass(g_ps_main,       { fx::INPUT_SCENE, m_lastadapt_id, m_smalltex_id }, fx::OUTPUT_SCENE, true,  fx::Cadence::EveryDispatch);

        m_runtime.set_passes(std::move(passes));
        m_passes_set = true;
        API::get()->log_info("[AdaptiveTonemapper] configure_runtime: smalltex_id=%d lastadapt_id=%d passes=3",
                              m_smalltex_id, m_lastadapt_id);
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Adaptive Tonemapper", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s — luluco250 AdaptiveTonemapper.fx port", ATM_VERSION);
            fx::draw_scene_rt_colorspace_warning();

            bool changed = false;
            const bool prev_enabled = m_enabled;
            changed |= ImGui::Checkbox("Enabled##ATM", &m_enabled);
            if (m_enabled != prev_enabled) {
                API::get()->log_info("[AdaptiveTonemapper] enabled=%d (scene RT: %s, colorspace=%d)",
                                      m_enabled ? 1 : 0,
                                      fx::EffectRuntime::scene_rt_format_name(),
                                      static_cast<int>(fx::EffectRuntime::scene_rt_colorspace()));
            }

            if (ImGui::TreeNodeEx("Tonemapping", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* operators = "Reinhard\0Filmic (Uncharted 2)\0ACES (Unreal Engine 4)\0";
                changed |= ImGui::Combo("Operator##ATM", &m_TonemapOperator, operators);
                changed |= ImGui::DragFloat("Amount##ATM",   &m_Amount,   0.01f, 0.0f,  2.0f);
                changed |= ImGui::DragFloat("Exposure##ATM", &m_Exposure, 0.01f, -6.0f, 6.0f, "%.2f f-stops");
                changed |= ImGui::Checkbox("Fix White Point##ATM", &m_FixWhitePoint);
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Adaptation", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= ImGui::DragFloat2("Range (min, max)##ATM", m_AdaptRange,    0.001f, 0.001f, 1.0f);
                changed |= ImGui::DragFloat("Time (s)##ATM",          &m_AdaptTime,    0.01f,  0.0f,   3.0f);
                changed |= ImGui::DragFloat("Sensitivity##ATM",       &m_AdaptSensitivity, 0.01f, 0.0f, 3.0f);
                changed |= ImGui::SliderInt("Precision##ATM",         &m_AdaptPrecision, 0, ATM_MIP_LEVELS,
                                             "%d (0=full screen, 9=focal pixel)");
                changed |= ImGui::DragFloat2("Focal Point##ATM",      m_AdaptFocalPoint, 0.001f, 0.0f, 1.0f);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Debug##ATM")) {
                ImGui::TextDisabled("Heatmap visualizers (log scale, 5 decades). Should look");
                ImGui::TextDisabled("identical across VR rendering modes.");
                ImGui::TextDisabled("Color: blue=low → cyan → green → yellow → red=high.");
                changed |= ImGui::RadioButton("Off##ATMdbg",                  &m_DebugVisualizeAdapt, 0);
                changed |= ImGui::RadioButton("adapt (LastAdapt history)##ATMdbg",  &m_DebugVisualizeAdapt, 1);
                changed |= ImGui::RadioButton("1/adapt (exposure scale)##ATMdbg",   &m_DebugVisualizeAdapt, 2);
                changed |= ImGui::RadioButton("SmallTex mip0 (current px)##ATMdbg", &m_DebugVisualizeAdapt, 3);
                changed |= ImGui::RadioButton("SmallTex mip8 (current avg)##ATMdbg",&m_DebugVisualizeAdapt, 4);
                changed |= ImGui::RadioButton("Tonemapped luma##ATMdbg",            &m_DebugVisualizeAdapt, 5);
                changed |= ImGui::RadioButton("Calibration bars##ATMdbg",           &m_DebugVisualizeAdapt, 6);
                ImGui::TreePop();
            }
            if (ImGui::Button("Reset to Defaults##ATM")) {
                m_TonemapOperator  = DEFAULT_TONEMAP_OPERATOR;
                m_Amount           = DEFAULT_AMOUNT;
                m_Exposure         = DEFAULT_EXPOSURE;
                m_FixWhitePoint    = DEFAULT_FIX_WHITE_POINT;
                m_AdaptRange[0]    = DEFAULT_ADAPT_RANGE_MIN;
                m_AdaptRange[1]    = DEFAULT_ADAPT_RANGE_MAX;
                m_AdaptTime        = DEFAULT_ADAPT_TIME;
                m_AdaptSensitivity = DEFAULT_ADAPT_SENSITIVITY;
                m_AdaptPrecision   = DEFAULT_ADAPT_PRECISION;
                m_AdaptFocalPoint[0] = DEFAULT_FOCAL_POINT_X;
                m_AdaptFocalPoint[1] = DEFAULT_FOCAL_POINT_Y;
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void update_cb() {
        m_cb.AdaptRange[0]      = m_AdaptRange[0];
        m_cb.AdaptRange[1]      = m_AdaptRange[1];
        m_cb.AdaptTime          = m_AdaptTime;
        m_cb.AdaptSensitivity   = m_AdaptSensitivity;
        m_cb.AdaptFocalPoint[0] = m_AdaptFocalPoint[0];
        m_cb.AdaptFocalPoint[1] = m_AdaptFocalPoint[1];
        m_cb.Amount             = m_Amount;
        m_cb.Exposure           = m_Exposure;
        m_cb.TonemapOperator    = m_TonemapOperator;
        m_cb.FixWhitePoint      = m_FixWhitePoint ? 1 : 0;
        m_cb.AdaptPrecision     = std::clamp(m_AdaptPrecision, 0, ATM_MIP_LEVELS);
        m_cb.DebugVisualizeAdapt= m_DebugVisualizeAdapt;
        // FrameTime: faithful to ReShade `frametime` (wall-clock ms between game
        // frames). Only sample on the first dispatch of each HMD frame —
        // otherwise in Synchronized Sequential / AFR (renderer hook fires twice
        // per HMD frame) the delta is ~half wall-clock, which halves the EMA
        // alpha in GetSmall and shifts steady-state exposure (mode-dependent
        // brightness bug). The cbuffer is uploaded per-dispatch but FrameTime
        // only matters to GetSmall, which is Cadence::OncePerFrame and runs on
        // the same first dispatch where we just refreshed it.
        // NOTE: do NOT use ImGui::GetIO() here — this runs in the renderer hook,
        // which has no active ImGui context (causes AV). Derive from tick count.
        if (m_runtime.is_first_dispatch_in_frame()) {
            const uint64_t now_ms = static_cast<uint64_t>(GetTickCount64());
            const uint64_t dt_ms = (m_last_frame_ms == 0) ? 16 : (now_ms - m_last_frame_ms);
            m_last_frame_ms = now_ms;
            m_cb.FrameTime  = static_cast<float>(dt_ms);
        }
    }

    void run_impl() {
        if (!m_enabled) return;
        if (!m_passes_set) return; // configure_runtime() is called from on_initialize(); never lazy here (would deadlock — see effect_runtime.hpp set_passes docs).
        update_cb();
        // Runtime decides per-pass cadence + dispatch arithmetic based on each
        // PassDesc::cadence (declared in configure_runtime()). Plugin does not
        // need to track per-eye dispatch state.
        m_runtime.execute();
    }

    void run() {
        // SEH guard so any access violation inside run_impl is caught and
        // logged instead of being silently swallowed by UEVR's outer catch.
        __try {
            run_impl();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static uint64_t s_last_av_log = 0;
            const uint64_t now2 = static_cast<uint64_t>(GetTickCount64());
            if (now2 - s_last_av_log > 1000) {
                s_last_av_log = now2;
                API::get()->log_warn("[AdaptiveTonemapper] SEH exception 0x%lx in run_impl()",
                                      (unsigned long)GetExceptionCode());
            }
        }
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override {
        m_runtime.release_resources();
        m_last_frame_ms = 0;
    }
};

std::unique_ptr<AdaptiveTonemapperPlugin> g_plugin{ new AdaptiveTonemapperPlugin() };
