/*
Vibrance Plugin for UEVR
=========================
A UEVR C++ plugin that applies CeeJay.dk's Vibrance effect to VR frames.
Intelligently boosts saturation — pixels with little color get a larger boost
than pixels that are already saturated.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Original shader:
  Vibrance by Christian Cann Schuldt Jensen ~ CeeJay.dk
  Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/Vibrance.fx
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

// Faithful port of CeeJay.dk's Vibrance v1.1
static const char* g_vibrance_ps_src = R"(
cbuffer VibranceParams : register(b0) {
    float  Vibrance;
    float3 VibranceRGBBalance;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target
{
    float3 color = Scene.Sample(PointSampler, input.TexCoord).rgb;

    float3 coefLuma = float3(0.212656, 0.715158, 0.072186);
    float luma = dot(coefLuma, color);

    float max_color = max(color.r, max(color.g, color.b));
    float min_color = min(color.r, min(color.g, color.b));
    float color_saturation = max_color - min_color;

    float3 coeffVibrance = VibranceRGBBalance * Vibrance;
    color = lerp(luma, color, 1.0 + (coeffVibrance * (1.0 - (sign(coeffVibrance) * color_saturation))));

    return float4(color, 1.0);
}
)";

// HLSL packing: float Vibrance @ 0..3, float3 VibranceRGBBalance @ 4..15 (fits
// in same 16-byte register because 4 + 12 = 16). Total cbuffer size: 16 bytes.
struct VibranceParamsCB {
    float Vibrance;
    float VibranceRGBBalance[3];
};
static_assert(sizeof(VibranceParamsCB) == 16, "VibranceParamsCB must be 16 bytes");

static constexpr const char* VIB_VERSION = "1.1.0";

// Defaults — single source of truth shared by reset_to_defaults, the
// per-field "Reset" buttons, and the global "Reset All".
static constexpr float VIB_DEFAULT_VIBRANCE   = 0.15f;
static constexpr float VIB_DEFAULT_BALANCE[3] = { 1.0f, 1.0f, 1.0f };

class VibrancePlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled = false;
    float m_vibrance = VIB_DEFAULT_VIBRANCE;
    float m_balance[3] = { VIB_DEFAULT_BALANCE[0], VIB_DEFAULT_BALANCE[1], VIB_DEFAULT_BALANCE[2] };

    fx::SinglePassEffect<VibranceParamsCB> m_fx{ g_vibrance_ps_src };

    void on_initialize() override {
        API::get()->log_info("[Vibrance] Plugin initialized (v%s)", VIB_VERSION);
        m_fx.init();
        // Register with host preset system. Settings will be applied via
        // the deserialize_settings path during apply_auto_preset() which
        // runs after every plugin's on_initialize.
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "Vibrance"; }
    int render_order() const override { return 1000; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",   m_enabled ? "1" : "0"},
            {"vibrance",  std::to_string(m_vibrance)},
            {"balance_r", std::to_string(m_balance[0])},
            {"balance_g", std::to_string(m_balance[1])},
            {"balance_b", std::to_string(m_balance[2])},
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
        get_float("vibrance",  m_vibrance,   -1.0f, 1.0f);
        get_float("balance_r", m_balance[0],  0.0f, 10.0f);
        get_float("balance_g", m_balance[1],  0.0f, 10.0f);
        get_float("balance_b", m_balance[2],  0.0f, 10.0f);
    }

    void reset_to_defaults() override {
        m_enabled  = false;
        m_vibrance = VIB_DEFAULT_VIBRANCE;
        m_balance[0] = VIB_DEFAULT_BALANCE[0];
        m_balance[1] = VIB_DEFAULT_BALANCE[1];
        m_balance[2] = VIB_DEFAULT_BALANCE[2];
    }
    // ----------------------------------------------------------------------

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Vibrance Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", VIB_VERSION);
            ImGui::TextWrapped("Boosts unsaturated colors more than saturated ones. Avoids clipping. Good for making dull games pop without oversaturating skin tones.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##Vib", &m_enabled);

            changed |= ImGui::DragFloat("Vibrance", &m_vibrance, 0.01f, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Intelligently saturates (positive) or desaturates (negative)");
            ImGui::SameLine();
            if (ImGui::Button("Reset##Vib_vibrance")) { m_vibrance = VIB_DEFAULT_VIBRANCE; changed = true; }

            changed |= ImGui::DragFloat3("RGB Balance", m_balance, 0.1f, 0.0f, 10.0f, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Per-channel vibrance multiplier");
            ImGui::SameLine();
            if (ImGui::Button("Reset##Vib_balance")) {
                m_balance[0] = VIB_DEFAULT_BALANCE[0];
                m_balance[1] = VIB_DEFAULT_BALANCE[1];
                m_balance[2] = VIB_DEFAULT_BALANCE[2];
                changed = true;
            }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##Vib")) {
                m_vibrance   = VIB_DEFAULT_VIBRANCE;
                m_balance[0] = VIB_DEFAULT_BALANCE[0];
                m_balance[1] = VIB_DEFAULT_BALANCE[1];
                m_balance[2] = VIB_DEFAULT_BALANCE[2];
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        VibranceParamsCB cb{};
        cb.Vibrance = m_vibrance;
        cb.VibranceRGBBalance[0] = m_balance[0];
        cb.VibranceRGBBalance[1] = m_balance[1];
        cb.VibranceRGBBalance[2] = m_balance[2];
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<VibrancePlugin> g_plugin{ new VibrancePlugin() };
