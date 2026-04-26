/*
Tonemap Plugin for UEVR
========================
A UEVR C++ plugin that applies CeeJay.dk's Tonemap effect to VR frames.
Gamma, exposure, saturation, bleach bypass, and defog controls.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Original shader:
  Tonemap version 1.1
  by Christian Cann Schuldt Jensen ~ CeeJay.dk
  Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/Tonemap.fx
  From the crosire/reshade-shaders community collection.
  No explicit license was provided in the original file or repository.
  All rights remain with the original author.
*/


#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

// Faithful port of CeeJay.dk's Tonemap v1.1
static const char* g_tonemap_ps_src = R"(
cbuffer TonemapParams : register(b0) {
    float Gamma;
    float Exposure;
    float Saturation;
    float Bleach;
    float Defog;
    float3 FogColor;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target
{
    float3 color = Scene.Sample(PointSampler, input.TexCoord).rgb;

    color = saturate(color - Defog * FogColor * 2.55);
    color *= pow(2.0f, Exposure);
    color = pow(color, Gamma);

    const float3 coefLuma = float3(0.2126, 0.7152, 0.0722);
    float lum = dot(coefLuma, color);
    float L = saturate(10.0 * (lum - 0.45));
    float3 A2 = Bleach * color;
    float3 result1 = 2.0f * color * lum;
    float3 result2 = 1.0f - 2.0f * (1.0f - lum) * (1.0f - color);
    float3 newColor = lerp(result1, result2, L);
    float3 mixRGB = A2 * newColor;
    color += ((1.0f - A2) * mixRGB);

    float3 middlegray = dot(color, 1.0 / 3.0);
    float3 diffcolor = color - middlegray;
    color = (color + diffcolor * Saturation) / (1.0 + (diffcolor * Saturation));

    return float4(color, 1.0);
}
)";

struct TonemapParamsCB {
    float Gamma;
    float Exposure;
    float Saturation;
    float Bleach;
    float Defog;
    float FogColor[3];
};
static_assert(sizeof(TonemapParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* TM_VERSION        = "1.1.0";
static constexpr float TM_DEFAULT_GAMMA        = 1.0f;
static constexpr float TM_DEFAULT_EXPOSURE     = 0.0f;
static constexpr float TM_DEFAULT_SATURATION   = 0.0f;
static constexpr float TM_DEFAULT_BLEACH       = 0.0f;
static constexpr float TM_DEFAULT_DEFOG        = 0.0f;
static constexpr float TM_DEFAULT_FOG_COLOR[3] = { 0.0f, 0.0f, 1.0f };

class TonemapPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled    = false;
    float m_gamma      = TM_DEFAULT_GAMMA;
    float m_exposure   = TM_DEFAULT_EXPOSURE;
    float m_saturation = TM_DEFAULT_SATURATION;
    float m_bleach     = TM_DEFAULT_BLEACH;
    float m_defog      = TM_DEFAULT_DEFOG;
    float m_fog_color[3] = { TM_DEFAULT_FOG_COLOR[0], TM_DEFAULT_FOG_COLOR[1], TM_DEFAULT_FOG_COLOR[2] };

    fx::SinglePassEffect<TonemapParamsCB> m_fx{ g_tonemap_ps_src };

    void on_initialize() override {
        API::get()->log_info("[Tonemap] Plugin initialized (v%s)", TM_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable ---
    std::string preset_section_name() const override { return "Tonemap"; }
    int render_order() const override { return 300; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",    m_enabled ? "1" : "0"},
            {"gamma",      std::to_string(m_gamma)},
            {"exposure",   std::to_string(m_exposure)},
            {"saturation", std::to_string(m_saturation)},
            {"bleach",     std::to_string(m_bleach)},
            {"defog",      std::to_string(m_defog)},
            {"fog_color_r", std::to_string(m_fog_color[0])},
            {"fog_color_g", std::to_string(m_fog_color[1])},
            {"fog_color_b", std::to_string(m_fog_color[2])},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* k, float& out, float lo, float hi) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { float v = std::stof(it->second); if (v<lo) v=lo; if (v>hi) v=hi; out = v; } catch (...) {}
        };
        auto it = kv.find("enabled");
        if (it != kv.end()) m_enabled = (it->second != "0" && !it->second.empty());
        get_float("gamma",       m_gamma,       0.0f,  2.0f);
        get_float("exposure",    m_exposure,   -1.0f,  1.0f);
        get_float("saturation",  m_saturation, -1.0f,  1.0f);
        get_float("bleach",      m_bleach,      0.0f,  1.0f);
        get_float("defog",       m_defog,       0.0f,  1.0f);
        get_float("fog_color_r", m_fog_color[0], 0.0f, 1.0f);
        get_float("fog_color_g", m_fog_color[1], 0.0f, 1.0f);
        get_float("fog_color_b", m_fog_color[2], 0.0f, 1.0f);
    }

    void reset_to_defaults() override {
        m_enabled    = false;
        m_gamma      = TM_DEFAULT_GAMMA;
        m_exposure   = TM_DEFAULT_EXPOSURE;
        m_saturation = TM_DEFAULT_SATURATION;
        m_bleach     = TM_DEFAULT_BLEACH;
        m_defog      = TM_DEFAULT_DEFOG;
        m_fog_color[0] = TM_DEFAULT_FOG_COLOR[0];
        m_fog_color[1] = TM_DEFAULT_FOG_COLOR[1];
        m_fog_color[2] = TM_DEFAULT_FOG_COLOR[2];
    }

    void reset_fog_color() {
        m_fog_color[0] = TM_DEFAULT_FOG_COLOR[0];
        m_fog_color[1] = TM_DEFAULT_FOG_COLOR[1];
        m_fog_color[2] = TM_DEFAULT_FOG_COLOR[2];
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Tonemap Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", TM_VERSION);
            ImGui::TextWrapped("Adjust gamma, exposure, and saturation. Also has bleach bypass (desaturated high-contrast film look). Exposure can clip highlights; defog subtracts color.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##TM", &m_enabled);

            changed |= ImGui::DragFloat("Gamma", &m_gamma, 0.01f, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjust gamma. <1 = brighter, >1 = darker.");
            ImGui::SameLine();
            if (ImGui::Button("Reset##TM_gamma")) { m_gamma = TM_DEFAULT_GAMMA; changed = true; }

            changed |= ImGui::DragFloat("Exposure", &m_exposure, 0.01f, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjust exposure. Positive = brighter.");
            ImGui::SameLine();
            if (ImGui::Button("Reset##TM_exp")) { m_exposure = TM_DEFAULT_EXPOSURE; changed = true; }

            changed |= ImGui::DragFloat("Saturation##TM", &m_saturation, 0.01f, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color intensity. Negative = desaturated, positive = more vivid");
            ImGui::SameLine();
            if (ImGui::Button("Reset##TM_sat")) { m_saturation = TM_DEFAULT_SATURATION; changed = true; }

            changed |= ImGui::DragFloat("Bleach", &m_bleach, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brightens shadows and fades colors");
            ImGui::SameLine();
            if (ImGui::Button("Reset##TM_bleach")) { m_bleach = TM_DEFAULT_BLEACH; changed = true; }

            changed |= ImGui::DragFloat("Defog", &m_defog, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove color tint (fog)");
            ImGui::SameLine();
            if (ImGui::Button("Reset##TM_defog")) { m_defog = TM_DEFAULT_DEFOG; changed = true; }

            changed |= ImGui::ColorEdit3("Defog Color", m_fog_color);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The color to subtract when defog > 0");
            ImGui::SameLine();
            if (ImGui::Button("Reset##TM_fogcolor")) { reset_fog_color(); changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##TM")) {
                m_gamma      = TM_DEFAULT_GAMMA;
                m_exposure   = TM_DEFAULT_EXPOSURE;
                m_saturation = TM_DEFAULT_SATURATION;
                m_bleach     = TM_DEFAULT_BLEACH;
                m_defog      = TM_DEFAULT_DEFOG;
                reset_fog_color();
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        TonemapParamsCB cb{};
        cb.Gamma       = m_gamma;
        cb.Exposure    = m_exposure;
        cb.Saturation  = m_saturation;
        cb.Bleach      = m_bleach;
        cb.Defog       = m_defog;
        cb.FogColor[0] = m_fog_color[0];
        cb.FogColor[1] = m_fog_color[1];
        cb.FogColor[2] = m_fog_color[2];
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<TonemapPlugin> g_plugin{ new TonemapPlugin() };
