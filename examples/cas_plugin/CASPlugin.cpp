/*
CAS (Contrast Adaptive Sharpening) Plugin for UEVR
====================================================
Port of AMD FidelityFX CAS to UEVR's fullscreen-triangle pipeline.
Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

Based on the ReShade pixel shader port by SLSNe, optimized by Marty McFly + CeeJay.dk:
  https://github.com/CeeJayDK/SweetFX/blob/master/Shaders/SweetFX/CAS.fx

Original CAS algorithm:
  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
  MIT License (see original ffx_cas.h for full text)

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)
*/


#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_cas_ps_src = R"(
cbuffer CASParams : register(b0) {
    float Contrast;
    float Sharpening;
    float2 PixelSize;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target {
    float2 uv = input.TexCoord;
    float3 a = Scene.Sample(PointSampler, uv + float2(-1.0, -1.0) * PixelSize).rgb;
    float3 b = Scene.Sample(PointSampler, uv + float2( 0.0, -1.0) * PixelSize).rgb;
    float3 c = Scene.Sample(PointSampler, uv + float2( 1.0, -1.0) * PixelSize).rgb;
    float3 d = Scene.Sample(PointSampler, uv + float2(-1.0,  0.0) * PixelSize).rgb;
    float3 e = Scene.Sample(PointSampler, uv).rgb;
    float3 f = Scene.Sample(PointSampler, uv + float2( 1.0,  0.0) * PixelSize).rgb;
    float3 g = Scene.Sample(PointSampler, uv + float2(-1.0,  1.0) * PixelSize).rgb;
    float3 h = Scene.Sample(PointSampler, uv + float2( 0.0,  1.0) * PixelSize).rgb;
    float3 i = Scene.Sample(PointSampler, uv + float2( 1.0,  1.0) * PixelSize).rgb;

    float3 mnRGB  = min(min(min(d, e), min(f, b)), h);
    float3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, i)));
    mnRGB += mnRGB2;

    float3 mxRGB  = max(max(max(d, e), max(f, b)), h);
    float3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, i)));
    mxRGB += mxRGB2;

    float3 rcpMRGB = rcp(mxRGB);
    float3 ampRGB = saturate(min(mnRGB, 2.0 - mxRGB) * rcpMRGB);

    ampRGB = rsqrt(ampRGB);

    float peak = -3.0 * Contrast + 8.0;
    float3 wRGB = -rcp(ampRGB * peak);

    float3 rcpWeightRGB = rcp(4.0 * wRGB + 1.0);

    float3 window = (b + d) + (f + h);
    float3 outColor = saturate((window * wRGB + e) * rcpWeightRGB);

    return float4(lerp(e, outColor, Sharpening), 1.0);
}
)";

struct CASParamsCB {
    float Contrast;
    float Sharpening;
    float PixelSize[2];
};
static_assert(sizeof(CASParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* CAS_VERSION = "1.1.0";
static constexpr float CAS_DEFAULT_CONTRAST   = 0.0f;
static constexpr float CAS_DEFAULT_SHARPENING = 1.0f;

class CASPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled    = false;
    float m_contrast   = CAS_DEFAULT_CONTRAST;
    float m_sharpening = CAS_DEFAULT_SHARPENING;

    fx::SinglePassEffect<CASParamsCB> m_fx{ g_cas_ps_src };

    void on_initialize() override {
        API::get()->log_info("[CAS] Plugin initialized (v%s)", CAS_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "CAS"; }
    int render_order() const override { return 1500; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",    m_enabled ? "1" : "0"},
            {"contrast",   std::to_string(m_contrast)},
            {"sharpening", std::to_string(m_sharpening)},
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
        auto it = kv.find("enabled");
        if (it != kv.end()) m_enabled = (it->second != "0" && !it->second.empty());
        get_float("contrast",   m_contrast,   0.0f, 1.0f);
        get_float("sharpening", m_sharpening, 0.0f, 1.0f);
    }

    void reset_to_defaults() override {
        m_enabled    = false;
        m_contrast   = CAS_DEFAULT_CONTRAST;
        m_sharpening = CAS_DEFAULT_SHARPENING;
    }
    // ----------------------------------------------------------------------

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("CAS Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", CAS_VERSION);
            ImGui::TextWrapped("AMD's contrast-adaptive sharpening. Sharpens edges without amplifying noise. Less haloing than LumaSharpen. Pair with FSR/temporal upscalers.");
            bool ch = false;
            ch |= ImGui::Checkbox("Enabled##CAS", &m_enabled);

            ch |= ImGui::SliderFloat("Contrast",   &m_contrast,   0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher = more sharpening on high-contrast edges");
            ImGui::SameLine();
            if (ImGui::Button("Reset##CAS_contrast")) { m_contrast = CAS_DEFAULT_CONTRAST; ch = true; }

            ch |= ImGui::SliderFloat("Sharpening", &m_sharpening, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Final blend strength with original");
            ImGui::SameLine();
            if (ImGui::Button("Reset##CAS_sharp")) { m_sharpening = CAS_DEFAULT_SHARPENING; ch = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##CAS")) {
                m_contrast = CAS_DEFAULT_CONTRAST;
                m_sharpening = CAS_DEFAULT_SHARPENING;
                ch = true;
            }
            if (ch) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        const auto w = fx::EffectRuntime::scene_width();
        const auto h = fx::EffectRuntime::scene_height();
        if (w == 0 || h == 0) return;

        CASParamsCB cb{};
        cb.Contrast     = m_contrast;
        cb.Sharpening   = m_sharpening;
        cb.PixelSize[0] = 1.0f / (float)w;
        cb.PixelSize[1] = 1.0f / (float)h;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<CASPlugin> g_plugin{ new CASPlugin() };
