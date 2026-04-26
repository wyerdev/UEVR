/*
DPX Plugin for UEVR
====================
A UEVR C++ plugin that applies Loadus's DPX/Cineon film stock color effect to VR frames.
Emulates Cineon film stock color science with RGB curve, contrast, saturation, colorfulness.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Original shader:
  DPX/Cineon shader
  by Loadus
  Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/DPX.fx
  From the crosire/reshade-shaders community collection.
  No explicit license was provided in the original file or repository.
  All rights remain with the original author.
*/


#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

// Faithful port of Loadus's DPX/Cineon shader.
static const char* g_dpx_ps_src = R"(
cbuffer DPXParams : register(b0) {
    float3 RGB_Curve;
    float  Contrast;
    float3 RGB_C;
    float  Saturation;
    float  Colorfulness;
    float  Strength;
    float2 _pad;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target
{
    static const float3x3 XYZ = float3x3(
        0.5003033835433160,  0.3380975732227390,  0.1645897795458570,
        0.2579688942747580,  0.6761952591447060,  0.0658358459823868,
        0.0234517888692628,  0.1126992737203000,  0.8668396731242010
    );
    static const float3x3 RGB = float3x3(
         2.6714711726599600, -1.2672360578624100, -0.4109956021722270,
        -1.0251070293466400,  1.9840911624108900,  0.0439502493584124,
         0.0610009456429445, -0.2236707508128630,  1.1590210416706100
    );

    float3 inputColor = Scene.Sample(PointSampler, input.TexCoord).rgb;

    float3 B = inputColor;
    B = B * (1.0 - Contrast) + (0.5 * Contrast);

    float3 Btemp = (1.0 / (1.0 + exp(RGB_Curve / 2.0)));
    B = ((1.0 / (1.0 + exp(-RGB_Curve * (B - RGB_C)))) / (-2.0 * Btemp + 1.0)) + (-Btemp / (-2.0 * Btemp + 1.0));

    float value = max(max(B.r, B.g), B.b);
    float3 color = B / value;
    color = pow(abs(color), 1.0 / Colorfulness);

    float3 c0 = color * value;
    c0 = mul(XYZ, c0);
    float luma = dot(c0, float3(0.30, 0.59, 0.11));
    c0 = (1.0 - Saturation) * luma + Saturation * c0;
    c0 = mul(RGB, c0);

    return float4(lerp(inputColor, c0, Strength), 1.0);
}
)";

struct DPXParamsCB {
    float RGB_Curve[3];
    float Contrast;
    float RGB_C[3];
    float Saturation;
    float Colorfulness;
    float Strength;
    float _pad[2];
};
static_assert(sizeof(DPXParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* DPX_VERSION = "1.1.0";
static constexpr float DPX_DEFAULT_RGB_CURVE[3] = { 8.0f, 8.0f, 8.0f };
static constexpr float DPX_DEFAULT_RGB_C[3]     = { 0.36f, 0.36f, 0.34f };
static constexpr float DPX_DEFAULT_CONTRAST     = 0.1f;
static constexpr float DPX_DEFAULT_SATURATION   = 3.0f;
static constexpr float DPX_DEFAULT_COLORFULNESS = 2.5f;
static constexpr float DPX_DEFAULT_STRENGTH     = 0.20f;

class DPXPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled = false;
    float m_rgb_curve[3] = { DPX_DEFAULT_RGB_CURVE[0], DPX_DEFAULT_RGB_CURVE[1], DPX_DEFAULT_RGB_CURVE[2] };
    float m_rgb_c[3]     = { DPX_DEFAULT_RGB_C[0],     DPX_DEFAULT_RGB_C[1],     DPX_DEFAULT_RGB_C[2] };
    float m_contrast     = DPX_DEFAULT_CONTRAST;
    float m_saturation   = DPX_DEFAULT_SATURATION;
    float m_colorfulness = DPX_DEFAULT_COLORFULNESS;
    float m_strength     = DPX_DEFAULT_STRENGTH;

    fx::SinglePassEffect<DPXParamsCB> m_fx{ g_dpx_ps_src };

    void on_initialize() override {
        API::get()->log_info("[DPX] Plugin initialized (v%s)", DPX_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "DPX"; }
    int render_order() const override { return 700; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",       m_enabled ? "1" : "0"},
            {"rgb_curve_r",   std::to_string(m_rgb_curve[0])},
            {"rgb_curve_g",   std::to_string(m_rgb_curve[1])},
            {"rgb_curve_b",   std::to_string(m_rgb_curve[2])},
            {"rgb_c_r",       std::to_string(m_rgb_c[0])},
            {"rgb_c_g",       std::to_string(m_rgb_c[1])},
            {"rgb_c_b",       std::to_string(m_rgb_c[2])},
            {"contrast",      std::to_string(m_contrast)},
            {"saturation",    std::to_string(m_saturation)},
            {"colorfulness",  std::to_string(m_colorfulness)},
            {"strength",      std::to_string(m_strength)},
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
        get_float("rgb_curve_r",  m_rgb_curve[0],  1.0f, 15.0f);
        get_float("rgb_curve_g",  m_rgb_curve[1],  1.0f, 15.0f);
        get_float("rgb_curve_b",  m_rgb_curve[2],  1.0f, 15.0f);
        get_float("rgb_c_r",      m_rgb_c[0],      0.2f, 0.5f);
        get_float("rgb_c_g",      m_rgb_c[1],      0.2f, 0.5f);
        get_float("rgb_c_b",      m_rgb_c[2],      0.2f, 0.5f);
        get_float("contrast",     m_contrast,      0.0f, 1.0f);
        get_float("saturation",   m_saturation,    0.0f, 8.0f);
        get_float("colorfulness", m_colorfulness,  0.1f, 2.5f);
        get_float("strength",     m_strength,      0.0f, 1.0f);
    }

    void reset_to_defaults() override {
        m_enabled = false;
        m_rgb_curve[0] = DPX_DEFAULT_RGB_CURVE[0];
        m_rgb_curve[1] = DPX_DEFAULT_RGB_CURVE[1];
        m_rgb_curve[2] = DPX_DEFAULT_RGB_CURVE[2];
        m_rgb_c[0]     = DPX_DEFAULT_RGB_C[0];
        m_rgb_c[1]     = DPX_DEFAULT_RGB_C[1];
        m_rgb_c[2]     = DPX_DEFAULT_RGB_C[2];
        m_contrast     = DPX_DEFAULT_CONTRAST;
        m_saturation   = DPX_DEFAULT_SATURATION;
        m_colorfulness = DPX_DEFAULT_COLORFULNESS;
        m_strength     = DPX_DEFAULT_STRENGTH;
    }

    void reset_rgb_curve() {
        m_rgb_curve[0] = DPX_DEFAULT_RGB_CURVE[0];
        m_rgb_curve[1] = DPX_DEFAULT_RGB_CURVE[1];
        m_rgb_curve[2] = DPX_DEFAULT_RGB_CURVE[2];
    }
    void reset_rgb_c() {
        m_rgb_c[0] = DPX_DEFAULT_RGB_C[0];
        m_rgb_c[1] = DPX_DEFAULT_RGB_C[1];
        m_rgb_c[2] = DPX_DEFAULT_RGB_C[2];
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("DPX Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", DPX_VERSION);
            ImGui::TextWrapped("Emulates Cineon film stock. Gives a warm, cinematic color shift. Good for games that look too cold or digital.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##DPX", &m_enabled);

            changed |= ImGui::DragFloat3("RGB Curve", m_rgb_curve, 0.1f, 1.0f, 15.0f, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cineon curve steepness per channel");
            ImGui::SameLine();
            if (ImGui::Button("Reset##DPX_curve")) { reset_rgb_curve(); changed = true; }

            changed |= ImGui::DragFloat3("RGB C", m_rgb_c, 0.01f, 0.2f, 0.5f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cineon curve center point per channel");
            ImGui::SameLine();
            if (ImGui::Button("Reset##DPX_c")) { reset_rgb_c(); changed = true; }

            changed |= ImGui::DragFloat("Contrast##DPX", &m_contrast, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contrast applied to the Cineon-graded image");
            ImGui::SameLine();
            if (ImGui::Button("Reset##DPX_contrast")) { m_contrast = DPX_DEFAULT_CONTRAST; changed = true; }

            changed |= ImGui::DragFloat("Saturation##DPX", &m_saturation, 0.1f, 0.0f, 8.0f, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color saturation of the film emulation. Higher = more vivid");
            ImGui::SameLine();
            if (ImGui::Button("Reset##DPX_sat")) { m_saturation = DPX_DEFAULT_SATURATION; changed = true; }

            changed |= ImGui::DragFloat("Colorfulness", &m_colorfulness, 0.01f, 0.1f, 2.5f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much the Cineon curve colors deviate from neutral");
            ImGui::SameLine();
            if (ImGui::Button("Reset##DPX_colorful")) { m_colorfulness = DPX_DEFAULT_COLORFULNESS; changed = true; }

            changed |= ImGui::DragFloat("Strength##DPX", &m_strength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blend between original (0) and full DPX effect (1)");
            ImGui::SameLine();
            if (ImGui::Button("Reset##DPX_strength")) { m_strength = DPX_DEFAULT_STRENGTH; changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##DPX")) {
                reset_rgb_curve();
                reset_rgb_c();
                m_contrast     = DPX_DEFAULT_CONTRAST;
                m_saturation   = DPX_DEFAULT_SATURATION;
                m_colorfulness = DPX_DEFAULT_COLORFULNESS;
                m_strength     = DPX_DEFAULT_STRENGTH;
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        DPXParamsCB cb{};
        cb.RGB_Curve[0] = m_rgb_curve[0];
        cb.RGB_Curve[1] = m_rgb_curve[1];
        cb.RGB_Curve[2] = m_rgb_curve[2];
        cb.Contrast     = m_contrast;
        cb.RGB_C[0]     = m_rgb_c[0];
        cb.RGB_C[1]     = m_rgb_c[1];
        cb.RGB_C[2]     = m_rgb_c[2];
        cb.Saturation   = m_saturation;
        cb.Colorfulness = m_colorfulness;
        cb.Strength     = m_strength;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<DPXPlugin> g_plugin{ new DPXPlugin() };
