/*
BlackCrush Plugin for UEVR
===========================
Author: wyermsu
License: MIT (see below)

A UEVR C++ plugin that deepens near-black shadows without touching midtones or
highlights. Designed for micro-OLED headsets (e.g. Pimax Crystal Light) where
SDR content can leave a faint "grey floor" in dark scenes.

Algorithm:
  luma  = dot(color, BT.709)
  blend = 1 - smoothstep(0, threshold, luma)   -- 1.0 at black, 0.0 at threshold
  crushed = pow(color, power)                   -- power > 1 darkens
  out   = lerp(color, crushed, blend * strength)

Any pixel with luma >= threshold is mathematically unmodified.

Parameters (all adjustable in the UEVR UI):
  Enabled   -- on/off toggle
  Threshold -- luminance above which the effect fades to zero (default 0.15 = ~15%)
  Power     -- curve steepness; 1.0 = no change, 2.0 = strong crush (default 1.8)
  Strength  -- overall blend intensity 0-1 (default 0.85)

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

-------------------------------------------------------------------------------
MIT License

Copyright (c) 2026 wyermsu

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
-------------------------------------------------------------------------------
*/


#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_bc_ps_src = R"(
cbuffer BlackCrushParams : register(b0) {
    float Threshold;
    float Power;
    float Strength;
    float _pad;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target
{
    float3 color = Scene.Sample(PointSampler, input.TexCoord).rgb;
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    float blend = 1.0 - smoothstep(0.0, Threshold, luma);
    float3 crushed = pow(max(color, 0.0), Power);
    color = lerp(color, crushed, blend * Strength);
    return float4(color, 1.0);
}
)";

struct BlackCrushParamsCB {
    float Threshold;
    float Power;
    float Strength;
    float _pad;
};
static_assert(sizeof(BlackCrushParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* BC_VERSION = "1.1.0";
static constexpr float BC_DEFAULT_THRESHOLD = 0.05f;
static constexpr float BC_DEFAULT_POWER     = 1.25f;
static constexpr float BC_DEFAULT_STRENGTH  = 0.35f;

class BlackCrushPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled   = false;
    float m_threshold = BC_DEFAULT_THRESHOLD;
    float m_power     = BC_DEFAULT_POWER;
    float m_strength  = BC_DEFAULT_STRENGTH;

    fx::SinglePassEffect<BlackCrushParamsCB> m_fx{ g_bc_ps_src };

    void on_initialize() override {
        API::get()->log_info("[BlackCrush] Plugin initialized (v%s)", BC_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable ---
    std::string preset_section_name() const override { return "BlackCrush"; }
    int render_order() const override { return 300; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",   m_enabled ? "1" : "0"},
            {"threshold", std::to_string(m_threshold)},
            {"power",     std::to_string(m_power)},
            {"strength",  std::to_string(m_strength)},
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
        get_float("threshold", m_threshold, 0.01f, 0.50f);
        get_float("power",     m_power,     1.0f,  4.0f);
        get_float("strength",  m_strength,  0.0f,  1.0f);
    }

    void reset_to_defaults() override {
        m_enabled   = false;
        m_threshold = BC_DEFAULT_THRESHOLD;
        m_power     = BC_DEFAULT_POWER;
        m_strength  = BC_DEFAULT_STRENGTH;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Black Crush Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", BC_VERSION);
            ImGui::TextWrapped("Deepens near-black shadows without touching midtones. Designed for micro-OLED headsets to eliminate the grey floor in dark scenes.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##BC", &m_enabled);

            ImGui::Separator();

            changed |= ImGui::SliderFloat("Threshold##BC", &m_threshold, 0.01f, 0.50f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Luminance above this value is left untouched (0.15 = 15%%).\nIncrease to crush deeper into the midtones.");
            ImGui::SameLine();
            if (ImGui::Button("Reset##BC_threshold")) { m_threshold = BC_DEFAULT_THRESHOLD; changed = true; }

            changed |= ImGui::SliderFloat("Power##BC", &m_power, 1.0f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Curve steepness inside the shadow band.\n1.0 = no effect. 2.0 = noticeable. 3.0+ = aggressive.");
            ImGui::SameLine();
            if (ImGui::Button("Reset##BC_power")) { m_power = BC_DEFAULT_POWER; changed = true; }

            changed |= ImGui::SliderFloat("Strength##BC", &m_strength, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overall blend intensity. 1.0 = full effect, 0.5 = half-blended.");
            ImGui::SameLine();
            if (ImGui::Button("Reset##BC_strength")) { m_strength = BC_DEFAULT_STRENGTH; changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##BC")) {
                m_threshold = BC_DEFAULT_THRESHOLD;
                m_power     = BC_DEFAULT_POWER;
                m_strength  = BC_DEFAULT_STRENGTH;
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        BlackCrushParamsCB cb{};
        cb.Threshold = m_threshold;
        cb.Power     = m_power;
        cb.Strength  = m_strength;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<BlackCrushPlugin> g_plugin{ new BlackCrushPlugin() };
