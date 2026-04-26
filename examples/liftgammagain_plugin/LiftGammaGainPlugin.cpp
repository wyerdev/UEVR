/*
LiftGammaGain Plugin for UEVR
===============================
A UEVR C++ plugin that applies the Lift Gamma Gain color correction effect to VR frames.
Separate shadow/midtone/highlight per-channel RGB control.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Original shader:
  Lift Gamma Gain version 1.1
  by 3an and CeeJay.dk
  Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/LiftGammaGain.fx
  From the crosire/reshade-shaders community collection.
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

// Faithful port of 3an & CeeJay.dk's Lift Gamma Gain v1.1
static const char* g_lgg_ps_src = R"(
cbuffer LGGParams : register(b0) {
    float3 RGB_Lift;
    float  _pad0;
    float3 RGB_Gamma;
    float  _pad1;
    float3 RGB_Gain;
    float  _pad2;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target
{
    float3 color = Scene.Sample(PointSampler, input.TexCoord).rgb;
    color = color * (1.5 - 0.5 * RGB_Lift) + 0.5 * RGB_Lift - 0.5;
    color = saturate(color);
    color *= RGB_Gain;
    color = pow(abs(color), 1.0 / RGB_Gamma);
    return float4(saturate(color), 1.0);
}
)";

struct LGGParamsCB {
    float RGB_Lift[3];
    float _pad0;
    float RGB_Gamma[3];
    float _pad1;
    float RGB_Gain[3];
    float _pad2;
};
static_assert(sizeof(LGGParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* LGG_VERSION       = "1.1.0";
static constexpr float LGG_DEFAULT_LIFT[3]     = { 1.0f, 1.0f, 1.0f };
static constexpr float LGG_DEFAULT_GAMMA[3]    = { 1.0f, 1.0f, 1.0f };
static constexpr float LGG_DEFAULT_GAIN[3]     = { 1.0f, 1.0f, 1.0f };

class LiftGammaGainPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled = false;
    float m_lift[3]  = { LGG_DEFAULT_LIFT[0],  LGG_DEFAULT_LIFT[1],  LGG_DEFAULT_LIFT[2]  };
    float m_gamma[3] = { LGG_DEFAULT_GAMMA[0], LGG_DEFAULT_GAMMA[1], LGG_DEFAULT_GAMMA[2] };
    float m_gain[3]  = { LGG_DEFAULT_GAIN[0],  LGG_DEFAULT_GAIN[1],  LGG_DEFAULT_GAIN[2]  };

    fx::SinglePassEffect<LGGParamsCB> m_fx{ g_lgg_ps_src };

    void on_initialize() override {
        API::get()->log_info("[LiftGammaGain] Plugin initialized (v%s)", LGG_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable ---
    std::string preset_section_name() const override { return "LiftGammaGain"; }
    int render_order() const override { return 200; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled", m_enabled ? "1" : "0"},
            {"lift_r",  std::to_string(m_lift[0])},
            {"lift_g",  std::to_string(m_lift[1])},
            {"lift_b",  std::to_string(m_lift[2])},
            {"gamma_r", std::to_string(m_gamma[0])},
            {"gamma_g", std::to_string(m_gamma[1])},
            {"gamma_b", std::to_string(m_gamma[2])},
            {"gain_r",  std::to_string(m_gain[0])},
            {"gain_g",  std::to_string(m_gain[1])},
            {"gain_b",  std::to_string(m_gain[2])},
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
        get_float("lift_r",  m_lift[0],  0.0f,  2.0f);
        get_float("lift_g",  m_lift[1],  0.0f,  2.0f);
        get_float("lift_b",  m_lift[2],  0.0f,  2.0f);
        get_float("gamma_r", m_gamma[0], 0.01f, 2.0f);
        get_float("gamma_g", m_gamma[1], 0.01f, 2.0f);
        get_float("gamma_b", m_gamma[2], 0.01f, 2.0f);
        get_float("gain_r",  m_gain[0],  0.0f,  2.0f);
        get_float("gain_g",  m_gain[1],  0.0f,  2.0f);
        get_float("gain_b",  m_gain[2],  0.0f,  2.0f);
    }

    void reset_to_defaults() override {
        m_enabled = false;
        m_lift[0]  = LGG_DEFAULT_LIFT[0];  m_lift[1]  = LGG_DEFAULT_LIFT[1];  m_lift[2]  = LGG_DEFAULT_LIFT[2];
        m_gamma[0] = LGG_DEFAULT_GAMMA[0]; m_gamma[1] = LGG_DEFAULT_GAMMA[1]; m_gamma[2] = LGG_DEFAULT_GAMMA[2];
        m_gain[0]  = LGG_DEFAULT_GAIN[0];  m_gain[1]  = LGG_DEFAULT_GAIN[1];  m_gain[2]  = LGG_DEFAULT_GAIN[2];
    }

    static void reset3(float dst[3], const float src[3]) { dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Lift Gamma Gain", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", LGG_VERSION);
            ImGui::TextWrapped("Fine-tune shadows, midtones, and highlights separately per RGB channel. Use if LevelsPlus alone isn't enough. Gain can clip highlights if pushed high.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##LGG", &m_enabled);

            changed |= ImGui::DragFloat3("Lift (Shadows)", m_lift, 0.01f, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjust shadows per R/G/B channel");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LGG_lift")) { reset3(m_lift, LGG_DEFAULT_LIFT); changed = true; }

            changed |= ImGui::DragFloat3("Gamma (Midtones)", m_gamma, 0.01f, 0.01f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjust midtones per R/G/B channel");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LGG_gamma")) { reset3(m_gamma, LGG_DEFAULT_GAMMA); changed = true; }

            changed |= ImGui::DragFloat3("Gain (Highlights)", m_gain, 0.01f, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adjust highlights per R/G/B channel");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LGG_gain")) { reset3(m_gain, LGG_DEFAULT_GAIN); changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##LGG")) {
                reset3(m_lift,  LGG_DEFAULT_LIFT);
                reset3(m_gamma, LGG_DEFAULT_GAMMA);
                reset3(m_gain,  LGG_DEFAULT_GAIN);
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        LGGParamsCB cb{};
        for (int i = 0; i < 3; ++i) {
            cb.RGB_Lift[i]  = m_lift[i];
            cb.RGB_Gamma[i] = m_gamma[i];
            cb.RGB_Gain[i]  = m_gain[i];
        }
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<LiftGammaGainPlugin> g_plugin{ new LiftGammaGainPlugin() };
