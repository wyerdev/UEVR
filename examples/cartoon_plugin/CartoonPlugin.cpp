/*
Cartoon Plugin for UEVR
========================
Port of CeeJay.dk's Cartoon effect to UEVR's fullscreen-triangle pipeline.
Creates an outline effect that makes the image look more cartoonish by
detecting edges via diagonal luminance differences and darkening them.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

Original shader:
  Cartoon by Christian Cann Schuldt Jensen ~ CeeJay.dk
  Based on the Auto Toon cg shader from the Dolphin emulator.
  Source: https://github.com/CeeJayDK/SweetFX/blob/master/Shaders/SweetFX/Cartoon.fx
  License: MIT (repository-wide, Copyright (c) 2014 CeeJayDK)
  See 13_5_CartoonShader-LICENSE.txt for full notice.

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)
*/


#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_cartoon_ps_src = R"(
cbuffer CartoonParams : register(b0) {
    float Power;
    float EdgeSlope;
    float2 PixelSize;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

static const float3 CoefLuma = float3(0.2126, 0.7152, 0.0722);

float4 main(PSInput input) : SV_Target {
    float2 uv = input.TexCoord;
    float3 color = Scene.Sample(PointSampler, uv).rgb;

    // Diagonal edge detection
    float diff1 = dot(CoefLuma, Scene.Sample(PointSampler, uv + PixelSize).rgb);
    diff1 = dot(float4(CoefLuma, -1.0), float4(Scene.Sample(PointSampler, uv - PixelSize).rgb, diff1));

    float diff2 = dot(CoefLuma, Scene.Sample(PointSampler, uv + PixelSize * float2(1, -1)).rgb);
    diff2 = dot(float4(CoefLuma, -1.0), float4(Scene.Sample(PointSampler, uv + PixelSize * float2(-1, 1)).rgb, diff2));

    float edge = dot(float2(diff1, diff2), float2(diff1, diff2));

    return float4(saturate(pow(abs(edge), EdgeSlope) * -Power + color), 1.0);
}
)";

struct CartoonParamsCB {
    float Power;
    float EdgeSlope;
    float PixelSize[2];
};
static_assert(sizeof(CartoonParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* CARTOON_VERSION     = "1.0.0";
static constexpr float DEFAULT_POWER             = 1.5f;
static constexpr float DEFAULT_EDGE_SLOPE        = 1.5f;

class CartoonPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled    = false;
    float m_power      = DEFAULT_POWER;
    float m_edge_slope = DEFAULT_EDGE_SLOPE;

    fx::SinglePassEffect<CartoonParamsCB> m_fx{ g_cartoon_ps_src };

    void on_initialize() override {
        API::get()->log_info("[Cartoon] Plugin initialized (v%s)", CARTOON_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "Cartoon"; }
    int render_order() const override { return 1350; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",    m_enabled ? "1" : "0"},
            {"power",      std::to_string(m_power)},
            {"edge_slope", std::to_string(m_edge_slope)},
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
        get_float("power",      m_power,      0.1f, 10.0f);
        get_float("edge_slope", m_edge_slope, 0.1f, 6.0f);
    }

    void reset_to_defaults() override {
        m_enabled    = false;
        m_power      = DEFAULT_POWER;
        m_edge_slope = DEFAULT_EDGE_SLOPE;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Cartoon Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", CARTOON_VERSION);
            ImGui::TextWrapped(
                "Stylized toon outlining. It detects diagonal luminance changes and darkens those edges, which works best on clean shapes, foliage silhouettes, props, and character outlines. "
                "Start with the defaults, raise Power until outlines are visible, then raise Edge Slope only if too many texture details are being outlined.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##Cartoon", &m_enabled);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Apply the toon edge darkening pass. Leave off for realistic games unless you want a deliberate stylized look.");

            changed |= ImGui::SliderFloat("Power##CTN", &m_power, 0.1f, 10.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Outline strength. Higher makes detected edges darker and more obvious; too high can crush fine detail.");
            ImGui::SameLine();
            if (ImGui::Button("Reset##CTN_power")) { m_power = DEFAULT_POWER; changed = true; }

            changed |= ImGui::SliderFloat("Edge Slope##CTN", &m_edge_slope, 0.1f, 6.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edge selectivity. Lower catches more texture/noise; higher keeps mostly strong silhouettes. May need more Power to compensate.");
            ImGui::SameLine();
            if (ImGui::Button("Reset##CTN_slope")) { m_edge_slope = DEFAULT_EDGE_SLOPE; changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##CTN")) {
                m_power      = DEFAULT_POWER;
                m_edge_slope = DEFAULT_EDGE_SLOPE;
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restore the Cartoon source-default tuning.");
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        const unsigned w = fx::EffectRuntime::scene_width();
        const unsigned h = fx::EffectRuntime::scene_height();
        if (w == 0 || h == 0) return;
        CartoonParamsCB cb{};
        cb.Power       = m_power;
        cb.EdgeSlope   = m_edge_slope;
        cb.PixelSize[0] = 1.0f / (float)w;
        cb.PixelSize[1] = 1.0f / (float)h;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<CartoonPlugin> g_plugin{ new CartoonPlugin() };
