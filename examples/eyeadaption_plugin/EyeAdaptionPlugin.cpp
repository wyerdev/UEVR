/*
EyeAdaption Plugin for UEVR
===========================

Faithful port of brussell1/Shaders EyeAdaption.fx v2.32 to UEVR's effect
runtime. Applied to the UE scene render target in on_pre_render_vr_framework
(DX11/DX12).

Original shader:
  EyeAdaption by brussell
  https://github.com/brussell1/Shaders/blob/master/Shaders/EyeAdaption.fx
  License: CC BY 4.0

Credits from the original shader:
  luluco250 - luminance get/store code from Magic Bloom

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Source faithfulness notes:
  - The four source passes are preserved in order: Luma, AvgLuma, Adaption,
    StoreAvgLuma.
  - All fAdp_* uniforms are surfaced with the original default values and ranges.
  - The source's `fAdp_YAxisFocalPoint.xx` sample coordinate is preserved exactly,
    even though the UI label says Y axis.
  - TexLuma is R8 256x256 with 7 mips and auto mip generation, matching the
    source texture declaration.
  - TexAvgLuma and TexAvgLumaLast are 1x1 R16F instead of full backbuffer-sized
    R16F. This is mathematically identical because the source AvgLuma and
    StoreAvgLuma passes write the same scalar to every pixel and downstream
    passes only sample at (0,0).
*/

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"
#include "effects/scene_warning.hpp"

using namespace uevr;

static constexpr const char* EYEADAPTION_VERSION = "2.32.0";

static constexpr float DEFAULT_ADP_DELAY              = 1.6f;
static constexpr float DEFAULT_ADP_TRIGGER_RADIUS     = 6.0f;
static constexpr float DEFAULT_ADP_Y_AXIS_FOCAL_POINT = 0.5f;
static constexpr float DEFAULT_ADP_EQUILIBRIUM        = 0.5f;
static constexpr float DEFAULT_ADP_STRENGTH           = 1.0f;
static constexpr float DEFAULT_ADP_BRIGHTEN_HIGHLIGHTS= 0.1f;
static constexpr float DEFAULT_ADP_BRIGHTEN_MIDTONES  = 0.2f;
static constexpr float DEFAULT_ADP_BRIGHTEN_SHADOWS   = 0.1f;
static constexpr float DEFAULT_ADP_DARKEN_HIGHLIGHTS  = 0.1f;
static constexpr float DEFAULT_ADP_DARKEN_MIDTONES    = 0.2f;
static constexpr float DEFAULT_ADP_DARKEN_SHADOWS     = 0.1f;

#pragma pack(push, 4)
struct EyeAdaptionCB {
    float fAdp_Delay;
    float fAdp_TriggerRadius;
    float fAdp_YAxisFocalPoint;
    float fAdp_Equilibrium;

    float fAdp_Strength;
    float fAdp_BrightenHighlights;
    float fAdp_BrightenMidtones;
    float fAdp_BrightenShadows;

    float fAdp_DarkenHighlights;
    float fAdp_DarkenMidtones;
    float fAdp_DarkenShadows;
    float Frametime;

    int32_t DebugMode;
    int32_t _pad0;
    int32_t _pad1;
    int32_t _pad2;
};
#pragma pack(pop)
static_assert(sizeof(EyeAdaptionCB) == 64, "EyeAdaptionCB must be 64 bytes");

#define EYEADAPTION_HLSL_PREAMBLE R"(
cbuffer EyeAdaptionCB : register(b0) {
    float fAdp_Delay;
    float fAdp_TriggerRadius;
    float fAdp_YAxisFocalPoint;
    float fAdp_Equilibrium;
    float fAdp_Strength;
    float fAdp_BrightenHighlights;
    float fAdp_BrightenMidtones;
    float fAdp_BrightenShadows;
    float fAdp_DarkenHighlights;
    float fAdp_DarkenMidtones;
    float fAdp_DarkenShadows;
    float Frametime;
    int DebugMode;
    int _pad0; int _pad1; int _pad2;
};
SamplerState LinearSampler : register(s0);
struct PSI { float4 P : SV_Position; float2 uv : TEXCOORD0; };
#define LumCoeff float3(0.212656, 0.715158, 0.072186)

float AdaptionDelta(float luma, float strengthMidtones, float strengthShadows, float strengthHighlights)
{
    float midtones = (4.0 * strengthMidtones - strengthHighlights - strengthShadows) * luma * (1.0 - luma);
    float shadows = strengthShadows * (1.0 - luma);
    float highlights = strengthHighlights * luma;
    float delta = midtones + shadows + highlights;
    return delta;
}
)"

static const char* g_ps_luma = EYEADAPTION_HLSL_PREAMBLE R"(
Texture2D Scene : register(t0);
float4 main(PSI i) : SV_Target
{
    float4 color = Scene.SampleLevel(LinearSampler, i.uv, 0);
    color.xyz = fx_decode_scene(color.xyz);
    float luma = dot(color.xyz, LumCoeff);
    return float4(luma, 0.0, 0.0, 0.0);
}
)";

static const char* g_ps_avg_luma = EYEADAPTION_HLSL_PREAMBLE R"(
Texture2D Luma : register(t0);
Texture2D AvgLumaLast : register(t1);
float4 main(PSI i) : SV_Target
{
    float avgLumaCurrFrame = Luma.SampleLevel(LinearSampler, fAdp_YAxisFocalPoint.xx, fAdp_TriggerRadius).x;
    float avgLumaLastFrame = AvgLumaLast.SampleLevel(LinearSampler, float2(0.0, 0.0), 0).x;
    float delay = sign(fAdp_Delay) * saturate(0.815 + fAdp_Delay / 10.0 - Frametime / 1000.0);
    float avgLuma = lerp(avgLumaCurrFrame, avgLumaLastFrame, delay);
    return float4(avgLuma, 0.0, 0.0, 0.0);
}
)";

static const char* g_ps_adaption = EYEADAPTION_HLSL_PREAMBLE R"(
Texture2D Scene : register(t0);
Texture2D AvgLuma : register(t1);
float4 main(PSI i) : SV_Target
{
    float4 color = Scene.SampleLevel(LinearSampler, i.uv, 0);
    float avgLuma = AvgLuma.SampleLevel(LinearSampler, float2(0.0, 0.0), 0).x;

    color.xyz = fx_decode_scene(color.xyz);
    color.xyz = pow(saturate(color.xyz), 1.0/2.2);
    float luma = dot(color.xyz, LumCoeff);
    float3 chroma = color.xyz - luma;

    float avgLumaAdjusted = lerp(avgLuma, 1.4 * avgLuma / (0.4 + avgLuma), fAdp_Equilibrium);
    float delta = 0;

    float curve = fAdp_Strength * 10.0 * pow(abs(avgLumaAdjusted - 0.5), 4.0);
    if (avgLumaAdjusted < 0.5) {
        delta = AdaptionDelta(luma, fAdp_BrightenMidtones, fAdp_BrightenShadows, fAdp_BrightenHighlights);
    } else {
        delta = -AdaptionDelta(luma, fAdp_DarkenMidtones, fAdp_DarkenShadows, fAdp_DarkenHighlights);
    }
    delta *= curve;

    luma += delta;
    color.xyz = saturate(luma + chroma);
    color.xyz = pow(color.xyz, 2.2);
    color.xyz = fx_encode_scene(color.xyz);

    if (DebugMode == 1) return float4(avgLuma.xxx, color.a);
    return color;
}
)";

static const char* g_ps_store_avg_luma = EYEADAPTION_HLSL_PREAMBLE R"(
Texture2D AvgLuma : register(t0);
float4 main(PSI i) : SV_Target
{
    float avgLuma = AvgLuma.SampleLevel(LinearSampler, float2(0.0, 0.0), 0).x;
    return float4(avgLuma, 0.0, 0.0, 0.0);
}
)";

class EyeAdaptionPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool m_enabled = false;

    float m_fAdp_Delay              = DEFAULT_ADP_DELAY;
    float m_fAdp_TriggerRadius      = DEFAULT_ADP_TRIGGER_RADIUS;
    float m_fAdp_YAxisFocalPoint    = DEFAULT_ADP_Y_AXIS_FOCAL_POINT;
    float m_fAdp_Equilibrium        = DEFAULT_ADP_EQUILIBRIUM;
    float m_fAdp_Strength           = DEFAULT_ADP_STRENGTH;
    float m_fAdp_BrightenHighlights = DEFAULT_ADP_BRIGHTEN_HIGHLIGHTS;
    float m_fAdp_BrightenMidtones   = DEFAULT_ADP_BRIGHTEN_MIDTONES;
    float m_fAdp_BrightenShadows    = DEFAULT_ADP_BRIGHTEN_SHADOWS;
    float m_fAdp_DarkenHighlights   = DEFAULT_ADP_DARKEN_HIGHLIGHTS;
    float m_fAdp_DarkenMidtones     = DEFAULT_ADP_DARKEN_MIDTONES;
    float m_fAdp_DarkenShadows      = DEFAULT_ADP_DARKEN_SHADOWS;
    int   m_DebugMode               = 0;

    EyeAdaptionCB    m_cb{};
    fx::EffectRuntime m_runtime;
    int               m_luma_id = -1;
    int               m_avg_luma_id = -1;
    int               m_avg_luma_last_id = -1;
    bool              m_passes_set = false;
    uint64_t          m_last_frame_ms = 0;

    void on_initialize() override {
        API::get()->log_info("[EyeAdaption] Plugin initialized (v%s)", EYEADAPTION_VERSION);
        configure_runtime();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "EyeAdaption"; }
    int render_order() const override { return 450; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled", m_enabled ? "1" : "0"},
            {"fAdp_Delay", std::to_string(m_fAdp_Delay)},
            {"fAdp_TriggerRadius", std::to_string(m_fAdp_TriggerRadius)},
            {"fAdp_YAxisFocalPoint", std::to_string(m_fAdp_YAxisFocalPoint)},
            {"fAdp_Equilibrium", std::to_string(m_fAdp_Equilibrium)},
            {"fAdp_Strength", std::to_string(m_fAdp_Strength)},
            {"fAdp_BrightenHighlights", std::to_string(m_fAdp_BrightenHighlights)},
            {"fAdp_BrightenMidtones", std::to_string(m_fAdp_BrightenMidtones)},
            {"fAdp_BrightenShadows", std::to_string(m_fAdp_BrightenShadows)},
            {"fAdp_DarkenHighlights", std::to_string(m_fAdp_DarkenHighlights)},
            {"fAdp_DarkenMidtones", std::to_string(m_fAdp_DarkenMidtones)},
            {"fAdp_DarkenShadows", std::to_string(m_fAdp_DarkenShadows)},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* k, float& out) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { out = std::stof(it->second); } catch (...) {}
        };
        auto get_bool = [&](const char* k, bool& out) {
            auto it = kv.find(k);
            if (it != kv.end()) out = (it->second != "0" && !it->second.empty());
        };
        get_bool("enabled", m_enabled);
        get_float("fAdp_Delay", m_fAdp_Delay);
        get_float("fAdp_TriggerRadius", m_fAdp_TriggerRadius);
        get_float("fAdp_YAxisFocalPoint", m_fAdp_YAxisFocalPoint);
        get_float("fAdp_Equilibrium", m_fAdp_Equilibrium);
        get_float("fAdp_Strength", m_fAdp_Strength);
        get_float("fAdp_BrightenHighlights", m_fAdp_BrightenHighlights);
        get_float("fAdp_BrightenMidtones", m_fAdp_BrightenMidtones);
        get_float("fAdp_BrightenShadows", m_fAdp_BrightenShadows);
        get_float("fAdp_DarkenHighlights", m_fAdp_DarkenHighlights);
        get_float("fAdp_DarkenMidtones", m_fAdp_DarkenMidtones);
        get_float("fAdp_DarkenShadows", m_fAdp_DarkenShadows);
        clamp_settings();
    }

    void reset_to_defaults() override {
        m_enabled = false;
        restore_original_defaults();
    }

    void restore_original_defaults() {
        m_fAdp_Delay              = DEFAULT_ADP_DELAY;
        m_fAdp_TriggerRadius      = DEFAULT_ADP_TRIGGER_RADIUS;
        m_fAdp_YAxisFocalPoint    = DEFAULT_ADP_Y_AXIS_FOCAL_POINT;
        m_fAdp_Equilibrium        = DEFAULT_ADP_EQUILIBRIUM;
        m_fAdp_Strength           = DEFAULT_ADP_STRENGTH;
        m_fAdp_BrightenHighlights = DEFAULT_ADP_BRIGHTEN_HIGHLIGHTS;
        m_fAdp_BrightenMidtones   = DEFAULT_ADP_BRIGHTEN_MIDTONES;
        m_fAdp_BrightenShadows    = DEFAULT_ADP_BRIGHTEN_SHADOWS;
        m_fAdp_DarkenHighlights   = DEFAULT_ADP_DARKEN_HIGHLIGHTS;
        m_fAdp_DarkenMidtones     = DEFAULT_ADP_DARKEN_MIDTONES;
        m_fAdp_DarkenShadows      = DEFAULT_ADP_DARKEN_SHADOWS;
    }

    void clamp_settings() {
        m_fAdp_Delay              = std::clamp(m_fAdp_Delay, 0.0f, 2.0f);
        m_fAdp_TriggerRadius      = std::clamp(m_fAdp_TriggerRadius, 1.0f, 7.0f);
        m_fAdp_YAxisFocalPoint    = std::clamp(m_fAdp_YAxisFocalPoint, 0.0f, 1.0f);
        m_fAdp_Equilibrium        = std::clamp(m_fAdp_Equilibrium, 0.0f, 1.0f);
        m_fAdp_Strength           = std::clamp(m_fAdp_Strength, 0.0f, 2.0f);
        m_fAdp_BrightenHighlights = std::clamp(m_fAdp_BrightenHighlights, 0.0f, 1.0f);
        m_fAdp_BrightenMidtones   = std::clamp(m_fAdp_BrightenMidtones, 0.0f, 1.0f);
        m_fAdp_BrightenShadows    = std::clamp(m_fAdp_BrightenShadows, 0.0f, 1.0f);
        m_fAdp_DarkenHighlights   = std::clamp(m_fAdp_DarkenHighlights, 0.0f, 1.0f);
        m_fAdp_DarkenMidtones     = std::clamp(m_fAdp_DarkenMidtones, 0.0f, 1.0f);
        m_fAdp_DarkenShadows      = std::clamp(m_fAdp_DarkenShadows, 0.0f, 1.0f);
    }

    void configure_runtime() {
        if (m_passes_set) return;

        fx::RTDesc luma_rt{};
        luma_rt.size_mode = fx::RTDesc::SizeMode::Fixed;
        luma_rt.w_or_div = 256;
        luma_rt.h_or_div = 256;
        luma_rt.format = DXGI_FORMAT_R8_UNORM;
        luma_rt.mip_levels = 7;
        luma_rt.auto_generate_mips = true;
        luma_rt.shared_across_scene_slots = true;
        m_luma_id = m_runtime.declare_rt(luma_rt);

        fx::RTDesc avg_luma_rt{};
        avg_luma_rt.size_mode = fx::RTDesc::SizeMode::Fixed;
        avg_luma_rt.w_or_div = 1;
        avg_luma_rt.h_or_div = 1;
        avg_luma_rt.format = DXGI_FORMAT_R16_FLOAT;
        avg_luma_rt.shared_across_scene_slots = true;
        m_avg_luma_id = m_runtime.declare_rt(avg_luma_rt);

        fx::RTDesc avg_luma_last_rt{};
        avg_luma_last_rt.size_mode = fx::RTDesc::SizeMode::Fixed;
        avg_luma_last_rt.w_or_div = 1;
        avg_luma_last_rt.h_or_div = 1;
        avg_luma_last_rt.format = DXGI_FORMAT_R16_FLOAT;
        avg_luma_last_rt.persistent = true;
        avg_luma_last_rt.shared_across_scene_slots = true;
        m_avg_luma_last_id = m_runtime.declare_rt(avg_luma_last_rt);

        std::vector<fx::PassDesc> passes;
        passes.reserve(4);

        auto make_pass = [&](const char* hlsl, std::vector<int> inputs, int output, bool decode,
                              fx::Cadence cadence) {
            fx::PassDesc p;
            p.ps_hlsl = hlsl;
            p.inputs = std::move(inputs);
            p.output = output;
            p.cb_data = &m_cb;
            p.cb_size = sizeof(m_cb);
            p.needs_scene_colorspace_decode = decode;
            p.cadence = cadence;
            passes.push_back(std::move(p));
        };

        make_pass(g_ps_luma, { fx::INPUT_SCENE }, m_luma_id, true, fx::Cadence::OncePerFrame);
        make_pass(g_ps_avg_luma, { m_luma_id, m_avg_luma_last_id }, m_avg_luma_id, false, fx::Cadence::OncePerFrame);
        make_pass(g_ps_adaption, { fx::INPUT_SCENE, m_avg_luma_id }, fx::OUTPUT_SCENE, true, fx::Cadence::EveryDispatch);
        make_pass(g_ps_store_avg_luma, { m_avg_luma_id }, m_avg_luma_last_id, false, fx::Cadence::OncePerFrame);

        m_runtime.set_passes(std::move(passes));
        m_passes_set = true;
        API::get()->log_info("[EyeAdaption] configure_runtime: luma=%d avg=%d last=%d passes=4",
                             m_luma_id, m_avg_luma_id, m_avg_luma_last_id);
    }

    bool slider_with_reset(const char* label, float* value, float step, float minv, float maxv,
                           const char* fmt, float default_value, const char* reset_id,
                           const char* tooltip) {
        bool changed = ImGui::DragFloat(label, value, step, minv, maxv, fmt);
        if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
        if (ImGui::Button(reset_id)) { *value = default_value; changed = true; }
        return changed;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("EyeAdaption", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s - brussell EyeAdaption.fx port", EYEADAPTION_VERSION);
            ImGui::TextWrapped(
                "Exposure compensation that reacts to scene brightness over time, like eyes adjusting when you move between bright and dark spaces. "
                "Use it when you want adaptive brightness without AdaptiveTonemapper's extra tone curve. Start with Strength around 0.5-1.0, then tune brightening and darkening separately.");
            fx::draw_scene_rt_colorspace_warning();

            bool changed = false;
            const bool prev_enabled = m_enabled;
            changed |= ImGui::Checkbox("Enabled##EyeAdaption", &m_enabled);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Apply adaptive exposure compensation. Best tested by walking from a bright area into a dark one.");
            if (m_enabled != prev_enabled) {
                API::get()->log_info("[EyeAdaption] enabled=%d (scene RT: %s, colorspace=%d)",
                                      m_enabled ? 1 : 0,
                                      fx::EffectRuntime::scene_rt_format_name(),
                                      static_cast<int>(fx::EffectRuntime::scene_rt_colorspace()));
            }

            if (ImGui::TreeNodeEx("General Settings##EyeAdaption", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= slider_with_reset("Adaption Delay##EyeAdaption", &m_fAdp_Delay, 0.01f, 0.0f, 2.0f, "%.2f", DEFAULT_ADP_DELAY, "Reset##EyeAdaption_delay",
                                             "How slowly exposure catches up. Lower reacts faster; higher is smoother and less jumpy.");
                changed |= slider_with_reset("Adaption TriggerRadius##EyeAdaption", &m_fAdp_TriggerRadius, 0.1f, 1.0f, 7.0f, "%.1f", DEFAULT_ADP_TRIGGER_RADIUS, "Reset##EyeAdaption_radius",
                                             "Which mip level is sampled for average luminance. Lower is more local; higher averages more of the whole view.");
                changed |= slider_with_reset("Y Axis Focal Point##EyeAdaption", &m_fAdp_YAxisFocalPoint, 0.001f, 0.0f, 1.0f, "%.3f", DEFAULT_ADP_Y_AXIS_FOCAL_POINT, "Reset##EyeAdaption_yfocal",
                                             "Source shader focal coordinate for luminance sampling. 0.5 watches screen center; move only if adaptation follows the wrong part of the image.");
                changed |= slider_with_reset("Adaption Equilibrium##EyeAdaption", &m_fAdp_Equilibrium, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_ADP_EQUILIBRIUM, "Reset##EyeAdaption_equilibrium",
                                             "Bias for the target luminance curve. Higher pushes the effect toward stronger correction around very dark or bright scenes.");
                changed |= slider_with_reset("Adaption Strength##EyeAdaption", &m_fAdp_Strength, 0.01f, 0.0f, 2.0f, "%.2f", DEFAULT_ADP_STRENGTH, "Reset##EyeAdaption_strength",
                                             "Overall adaptive exposure strength. Reduce first if the scene pumps or feels unstable.");
                ImGui::TreePop();
            }

            if (ImGui::TreeNodeEx("Brightening##EyeAdaption", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= slider_with_reset("Brighten Highlights##EyeAdaption", &m_fAdp_BrightenHighlights, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_ADP_BRIGHTEN_HIGHLIGHTS, "Reset##EyeAdaption_bh",
                                             "How much bright parts lift when the scene is too dark. Keep low to avoid glowing skies or UI.");
                changed |= slider_with_reset("Brighten Midtones##EyeAdaption", &m_fAdp_BrightenMidtones, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_ADP_BRIGHTEN_MIDTONES, "Reset##EyeAdaption_bm",
                                             "Main dark-scene lift. Increase this before shadows if caves are readable but characters still look dim.");
                changed |= slider_with_reset("Brighten Shadows##EyeAdaption", &m_fAdp_BrightenShadows, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_ADP_BRIGHTEN_SHADOWS, "Reset##EyeAdaption_bs",
                                             "Shadow lift in dark scenes. Too much raises black levels and can undo LevelsPlus/BlackCrush tuning.");
                ImGui::TreePop();
            }

            if (ImGui::TreeNodeEx("Darkening##EyeAdaption", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= slider_with_reset("Darken Highlights##EyeAdaption", &m_fAdp_DarkenHighlights, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_ADP_DARKEN_HIGHLIGHTS, "Reset##EyeAdaption_dh",
                                             "Highlight reduction in bright scenes. Raise if sunlit areas or snow are uncomfortable.");
                changed |= slider_with_reset("Darken Midtones##EyeAdaption", &m_fAdp_DarkenMidtones, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_ADP_DARKEN_MIDTONES, "Reset##EyeAdaption_dm",
                                             "Main bright-scene dimming. Raise this before darkening shadows if outdoor scenes are too hot.");
                changed |= slider_with_reset("Darken Shadows##EyeAdaption", &m_fAdp_DarkenShadows, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_ADP_DARKEN_SHADOWS, "Reset##EyeAdaption_ds",
                                             "Shadow dimming in bright scenes. Keep conservative or dark interiors after bright areas can feel crushed.");
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Debug##EyeAdaption")) {
                changed |= ImGui::RadioButton("Off##EyeAdaption_dbg", &m_DebugMode, 0);
                changed |= ImGui::RadioButton("Show averaged luminance##EyeAdaption_dbg", &m_DebugMode, 1);
                ImGui::TreePop();
            }

            if (ImGui::Button("Reset All##EyeAdaption")) {
                restore_original_defaults();
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restore brussell EyeAdaption.fx default values.");
            if (changed) {
                clamp_settings();
                uevr::settings::notify_changed(*this, API::get()->param());
            }
        }
    }

    void update_cb() {
        clamp_settings();
        m_cb.fAdp_Delay              = m_fAdp_Delay;
        m_cb.fAdp_TriggerRadius      = m_fAdp_TriggerRadius;
        m_cb.fAdp_YAxisFocalPoint    = m_fAdp_YAxisFocalPoint;
        m_cb.fAdp_Equilibrium        = m_fAdp_Equilibrium;
        m_cb.fAdp_Strength           = m_fAdp_Strength;
        m_cb.fAdp_BrightenHighlights = m_fAdp_BrightenHighlights;
        m_cb.fAdp_BrightenMidtones   = m_fAdp_BrightenMidtones;
        m_cb.fAdp_BrightenShadows    = m_fAdp_BrightenShadows;
        m_cb.fAdp_DarkenHighlights   = m_fAdp_DarkenHighlights;
        m_cb.fAdp_DarkenMidtones     = m_fAdp_DarkenMidtones;
        m_cb.fAdp_DarkenShadows      = m_fAdp_DarkenShadows;
        m_cb.DebugMode               = m_DebugMode;

        if (m_runtime.is_first_dispatch_in_frame()) {
            const uint64_t now_ms = static_cast<uint64_t>(GetTickCount64());
            const uint64_t dt_ms = (m_last_frame_ms == 0) ? 16 : (now_ms - m_last_frame_ms);
            m_last_frame_ms = now_ms;
            m_cb.Frametime = static_cast<float>(dt_ms);
        }
    }

    void run_impl() {
        if (!m_enabled) return;
        if (!m_passes_set) return;
        update_cb();
        m_runtime.execute();
    }

    void run() {
        __try {
            run_impl();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static uint64_t s_last_av_log = 0;
            const uint64_t now_ms = static_cast<uint64_t>(GetTickCount64());
            if (now_ms - s_last_av_log > 1000) {
                s_last_av_log = now_ms;
                API::get()->log_warn("[EyeAdaption] SEH exception 0x%lx in run_impl()",
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

std::unique_ptr<EyeAdaptionPlugin> g_plugin{ new EyeAdaptionPlugin() };