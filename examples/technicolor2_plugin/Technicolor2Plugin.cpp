/*
Technicolor2 Plugin for UEVR
=============================
A UEVR C++ plugin that applies Prod80's Technicolor2 colour grading effect
(optimized by CeeJay.dk). Stylized cross-channel colour mixing that darkens
and intensifies colours, with brightness and saturation controls.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Original shader:
  Technicolor2 version 1.0
  Original by Prod80
  Optimized by CeeJay.dk
  Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/Technicolor2.fx
  No explicit license was provided in the original file or repository.
  All rights remain with the original authors.
*/


#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

// Faithful 1:1 port of Prod80/CeeJay.dk's Technicolor2.fx.
// The algorithm inverts the colour, does cross-channel multiplication via
// swizzles (.grg, .bbr), multiplies by ColorStrength, applies Brightness,
// subtracts the cross-terms back, then lerps by Strength with a final
// Saturation control.
static const char* g_tech2_ps_src = R"(
cbuffer Technicolor2Params : register(b0) {
    float3 ColorStrength;
    float  Brightness;
    float  Saturation;
    float  Strength;
    float2 _pad0;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target {
    float3 color = saturate(Scene.Sample(PointSampler, input.TexCoord).rgb);

    float3 temp = 1.0 - color;
    float3 target  = temp.grg;
    float3 target2 = temp.bbr;
    float3 temp2 = color * target;
    temp2 *= target2;

    temp  = temp2 * ColorStrength;
    temp2 *= Brightness;

    target  = temp.grg;
    target2 = temp.bbr;

    temp  = color - target;
    temp  += temp2;
    temp2 = temp - target2;

    color = lerp(color, temp2, Strength);
    color = lerp(dot(color, 0.333), color, Saturation);

    return float4(color, 1.0);
}
)";

struct Technicolor2CB {
    float ColorStrength[3];
    float Brightness;
    float Saturation;
    float Strength;
    float _pad0[2];
};
static_assert(sizeof(Technicolor2CB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* TECH2_VERSION = "1.0.0";
static constexpr float TECH2_DEFAULT_COLOR_R    = 0.2f;
static constexpr float TECH2_DEFAULT_COLOR_G    = 0.2f;
static constexpr float TECH2_DEFAULT_COLOR_B    = 0.2f;
static constexpr float TECH2_DEFAULT_BRIGHTNESS = 1.0f;
static constexpr float TECH2_DEFAULT_SATURATION = 1.0f;
static constexpr float TECH2_DEFAULT_STRENGTH   = 1.0f;

class Technicolor2Plugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled    = false;
    float m_color_r    = TECH2_DEFAULT_COLOR_R;
    float m_color_g    = TECH2_DEFAULT_COLOR_G;
    float m_color_b    = TECH2_DEFAULT_COLOR_B;
    float m_brightness = TECH2_DEFAULT_BRIGHTNESS;
    float m_saturation = TECH2_DEFAULT_SATURATION;
    float m_strength   = TECH2_DEFAULT_STRENGTH;

    fx::SinglePassEffect<Technicolor2CB> m_fx{ g_tech2_ps_src };

    void on_initialize() override {
        API::get()->log_info("[Technicolor2] Plugin initialized (v%s)", TECH2_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "Technicolor2"; }
    int render_order() const override { return 950; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",    m_enabled ? "1" : "0"},
            {"color_r",    std::to_string(m_color_r)},
            {"color_g",    std::to_string(m_color_g)},
            {"color_b",    std::to_string(m_color_b)},
            {"brightness", std::to_string(m_brightness)},
            {"saturation", std::to_string(m_saturation)},
            {"strength",   std::to_string(m_strength)},
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
        get_float("color_r",    m_color_r,    0.0f, 1.0f);
        get_float("color_g",    m_color_g,    0.0f, 1.0f);
        get_float("color_b",    m_color_b,    0.0f, 1.0f);
        get_float("brightness", m_brightness, 0.5f, 1.5f);
        get_float("saturation", m_saturation, 0.0f, 1.5f);
        get_float("strength",   m_strength,   0.0f, 1.0f);
    }

    void reset_to_defaults() override {
        m_enabled    = false;
        m_color_r    = TECH2_DEFAULT_COLOR_R;
        m_color_g    = TECH2_DEFAULT_COLOR_G;
        m_color_b    = TECH2_DEFAULT_COLOR_B;
        m_brightness = TECH2_DEFAULT_BRIGHTNESS;
        m_saturation = TECH2_DEFAULT_SATURATION;
        m_strength   = TECH2_DEFAULT_STRENGTH;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Technicolor2 Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", TECH2_VERSION);
            ImGui::TextWrapped("Stylized colour grading that darkens and intensifies colours. Tends to oversaturate — use Saturation to compensate. Different from Technicolor v1 (film emulation).");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##tech2", &m_enabled);

            float color[3] = { m_color_r, m_color_g, m_color_b };
            if (ImGui::ColorEdit3("Color Strength##tech2", color)) {
                m_color_r = color[0];
                m_color_g = color[1];
                m_color_b = color[2];
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher means darker and more intense colors");

            changed |= ImGui::DragFloat("Brightness##tech2", &m_brightness, 0.01f, 0.5f, 1.5f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher means brighter image");
            ImGui::SameLine();
            if (ImGui::Button("Reset##tech2_bright")) { m_brightness = TECH2_DEFAULT_BRIGHTNESS; changed = true; }

            changed |= ImGui::DragFloat("Saturation##tech2", &m_saturation, 0.01f, 0.0f, 1.5f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Additional saturation control since this effect tends to oversaturate the image");
            ImGui::SameLine();
            if (ImGui::Button("Reset##tech2_sat")) { m_saturation = TECH2_DEFAULT_SATURATION; changed = true; }

            changed |= ImGui::DragFloat("Strength##tech2", &m_strength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjust the strength of the effect");
            ImGui::SameLine();
            if (ImGui::Button("Reset##tech2_str")) { m_strength = TECH2_DEFAULT_STRENGTH; changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##tech2")) {
                m_color_r    = TECH2_DEFAULT_COLOR_R;
                m_color_g    = TECH2_DEFAULT_COLOR_G;
                m_color_b    = TECH2_DEFAULT_COLOR_B;
                m_brightness = TECH2_DEFAULT_BRIGHTNESS;
                m_saturation = TECH2_DEFAULT_SATURATION;
                m_strength   = TECH2_DEFAULT_STRENGTH;
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        Technicolor2CB cb{};
        cb.ColorStrength[0] = m_color_r;
        cb.ColorStrength[1] = m_color_g;
        cb.ColorStrength[2] = m_color_b;
        cb.Brightness       = m_brightness;
        cb.Saturation       = m_saturation;
        cb.Strength         = m_strength;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<Technicolor2Plugin> g_plugin{ new Technicolor2Plugin() };
