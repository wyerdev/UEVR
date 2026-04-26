/*
LumaSharpen Plugin for UEVR
=============================
Port of CeeJay.dk's LumaSharpen to UEVR's fullscreen-triangle pipeline.
Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

Sharpens in luminance only to avoid color fringing, with configurable
sampling patterns and clamped sharpening to prevent halo artifacts.

Original shader:
  LumaSharpen v1.5.0 by Christian Cann Schuldt Jensen ~ CeeJay.dk
  Source: https://github.com/CeeJayDK/SweetFX/blob/master/Shaders/SweetFX/LumaSharpen.fx
  No explicit license was provided in the original file.
  All rights remain with the original author.

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)
*/


#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_lumasharpen_ps_src = R"(
cbuffer LumaSharpenParams : register(b0) {
    float SharpStrength;
    float SharpClamp;
    int   Pattern;
    float OffsetBias;
    int   ShowSharpen;
    float _pad0;
    float2 PixelSize;
};

Texture2D Scene : register(t0);
SamplerState LinearSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

static const float3 CoefLuma = float3(0.2126, 0.7152, 0.0722);

float4 main(PSInput input) : SV_Target {
    float2 uv = input.TexCoord;
    float3 ori = Scene.Sample(LinearSampler, uv).rgb;

    float3 sharp_strength_luma = CoefLuma * SharpStrength;
    float3 blur_ori;

    if (Pattern == 0) {
        blur_ori  = Scene.Sample(LinearSampler, uv + (PixelSize / 3.0) * OffsetBias).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv - (PixelSize / 3.0) * OffsetBias).rgb;
        blur_ori *= 0.5;
        sharp_strength_luma *= 1.5;
    } else if (Pattern == 1) {
        blur_ori  = Scene.Sample(LinearSampler, uv + float2( PixelSize.x, -PixelSize.y) * 0.5 * OffsetBias).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv - PixelSize * 0.5 * OffsetBias).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv + PixelSize * 0.5 * OffsetBias).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv - float2( PixelSize.x, -PixelSize.y) * 0.5 * OffsetBias).rgb;
        blur_ori *= 0.25;
    } else if (Pattern == 2) {
        blur_ori  = Scene.Sample(LinearSampler, uv + PixelSize * float2(0.4, -1.2) * OffsetBias).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv - PixelSize * float2(1.2,  0.4) * OffsetBias).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv + PixelSize * float2(1.2,  0.4) * OffsetBias).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv - PixelSize * float2(0.4, -1.2) * OffsetBias).rgb;
        blur_ori *= 0.25;
        sharp_strength_luma *= 0.51;
    } else {
        blur_ori  = Scene.Sample(LinearSampler, uv + float2(0.5 * PixelSize.x, -PixelSize.y * OffsetBias)).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv + float2(OffsetBias * -PixelSize.x, 0.5 * -PixelSize.y)).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv + float2(OffsetBias *  PixelSize.x, 0.5 *  PixelSize.y)).rgb;
        blur_ori += Scene.Sample(LinearSampler, uv + float2(0.5 * -PixelSize.x, PixelSize.y * OffsetBias)).rgb;
        blur_ori *= 0.25;
        sharp_strength_luma *= 0.666;
    }

    float3 sharp = ori - blur_ori;
    float4 sharp_strength_luma_clamp = float4(sharp_strength_luma * (0.5 / SharpClamp), 0.5);
    float sharp_luma = saturate(dot(float4(sharp, 1.0), sharp_strength_luma_clamp));
    sharp_luma = (SharpClamp * 2.0) * sharp_luma - SharpClamp;
    float3 outputcolor = ori + sharp_luma;
    if (ShowSharpen) {
        outputcolor = saturate(0.5 + (sharp_luma * 4.0)).rrr;
    }
    return float4(saturate(outputcolor), 1.0);
}
)";

struct LumaSharpenParamsCB {
    float SharpStrength;
    float SharpClamp;
    int   Pattern;
    float OffsetBias;
    int   ShowSharpen;
    float _pad0;
    float PixelSize[2];
};
static_assert(sizeof(LumaSharpenParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* LS_VERSION              = "1.1.0";
static constexpr float LS_DEFAULT_SHARP_STRENGTH     = 0.65f;
static constexpr float LS_DEFAULT_SHARP_CLAMP        = 0.035f;
static constexpr int   LS_DEFAULT_PATTERN            = 1;
static constexpr float LS_DEFAULT_OFFSET_BIAS        = 1.0f;
static constexpr bool  LS_DEFAULT_SHOW_SHARPEN       = false;

class LumaSharpenPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled        = false;
    float m_sharp_strength = LS_DEFAULT_SHARP_STRENGTH;
    float m_sharp_clamp    = LS_DEFAULT_SHARP_CLAMP;
    int   m_pattern        = LS_DEFAULT_PATTERN;
    float m_offset_bias    = LS_DEFAULT_OFFSET_BIAS;
    bool  m_show_sharpen   = LS_DEFAULT_SHOW_SHARPEN;

    fx::SinglePassEffect<LumaSharpenParamsCB> m_fx{ g_lumasharpen_ps_src };

    void on_initialize() override {
        API::get()->log_info("[LumaSharpen] Plugin initialized (v%s)", LS_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "LumaSharpen"; }
    int render_order() const override { return 1600; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",        m_enabled ? "1" : "0"},
            {"sharp_strength", std::to_string(m_sharp_strength)},
            {"sharp_clamp",    std::to_string(m_sharp_clamp)},
            {"pattern",        std::to_string(m_pattern)},
            {"offset_bias",    std::to_string(m_offset_bias)},
            {"show_sharpen",   m_show_sharpen ? "1" : "0"},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* k, float& out, float lo, float hi) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try {
                float v = std::stof(it->second);
                if (v < lo) v = lo;
                if (v > hi) v = hi;
                out = v;
            } catch (...) {}
        };
        auto get_int = [&](const char* k, int& out, int lo, int hi) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try {
                int v = std::stoi(it->second);
                if (v < lo) v = lo;
                if (v > hi) v = hi;
                out = v;
            } catch (...) {}
        };
        auto it = kv.find("enabled");
        if (it != kv.end()) m_enabled = (it->second != "0" && !it->second.empty());
        get_float("sharp_strength", m_sharp_strength, 0.1f, 3.0f);
        get_float("sharp_clamp",    m_sharp_clamp,    0.0f, 1.0f);
        get_int  ("pattern",        m_pattern,        0,    3);
        get_float("offset_bias",    m_offset_bias,    0.0f, 6.0f);
        auto it2 = kv.find("show_sharpen");
        if (it2 != kv.end()) m_show_sharpen = (it2->second != "0" && !it2->second.empty());
    }

    void reset_to_defaults() override {
        m_enabled        = false;
        m_sharp_strength = LS_DEFAULT_SHARP_STRENGTH;
        m_sharp_clamp    = LS_DEFAULT_SHARP_CLAMP;
        m_pattern        = LS_DEFAULT_PATTERN;
        m_offset_bias    = LS_DEFAULT_OFFSET_BIAS;
        m_show_sharpen   = LS_DEFAULT_SHOW_SHARPEN;
    }
    // ----------------------------------------------------------------------

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("LumaSharpen Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", LS_VERSION);
            ImGui::TextWrapped("Sharpens in luminance only to avoid color fringing. 4 sampling patterns with adjustable strength and halo clamp. Best for fine detail recovery on top of CAS.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##LumaSharpen", &m_enabled);

            changed |= ImGui::DragFloat("Strength##LS", &m_sharp_strength, 0.01f, 0.1f, 3.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Strength of the sharpening effect");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LS_strength")) { m_sharp_strength = LS_DEFAULT_SHARP_STRENGTH; changed = true; }

            changed |= ImGui::DragFloat("Clamp##LS", &m_sharp_clamp, 0.001f, 0.0f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Limits maximum sharpening per pixel to prevent halos");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LS_clamp")) { m_sharp_clamp = LS_DEFAULT_SHARP_CLAMP; changed = true; }

            const char* pattern_items[] = {"Fast (7-tap)","Normal (9-tap)","Wider (17-tap)","Pyramid"};
            changed |= ImGui::Combo("Pattern##LS", &m_pattern, pattern_items, 4);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sampling pattern. Normal is recommended.");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LS_pattern")) { m_pattern = LS_DEFAULT_PATTERN; changed = true; }

            changed |= ImGui::DragFloat("Offset Bias##LS", &m_offset_bias, 0.1f, 0.0f, 6.0f, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjusts the radius of the sampling pattern");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LS_offset")) { m_offset_bias = LS_DEFAULT_OFFSET_BIAS; changed = true; }

            changed |= ImGui::Checkbox("Show Sharpen##LS", &m_show_sharpen);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visualize the sharpening mask (debug)");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LS_show")) { m_show_sharpen = LS_DEFAULT_SHOW_SHARPEN; changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##LS")) {
                m_sharp_strength = LS_DEFAULT_SHARP_STRENGTH;
                m_sharp_clamp    = LS_DEFAULT_SHARP_CLAMP;
                m_pattern        = LS_DEFAULT_PATTERN;
                m_offset_bias    = LS_DEFAULT_OFFSET_BIAS;
                m_show_sharpen   = LS_DEFAULT_SHOW_SHARPEN;
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        const unsigned w = fx::EffectRuntime::scene_width();
        const unsigned h = fx::EffectRuntime::scene_height();
        if (w == 0 || h == 0) return;
        LumaSharpenParamsCB cb{};
        cb.SharpStrength = m_sharp_strength;
        cb.SharpClamp    = m_sharp_clamp;
        cb.Pattern       = m_pattern;
        cb.OffsetBias    = m_offset_bias;
        cb.ShowSharpen   = m_show_sharpen ? 1 : 0;
        cb.PixelSize[0]  = 1.0f / (float)w;
        cb.PixelSize[1]  = 1.0f / (float)h;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<LumaSharpenPlugin> g_plugin{ new LumaSharpenPlugin() };
