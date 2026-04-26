/*
Curves Plugin for UEVR
=======================
A UEVR C++ plugin that applies CeeJay.dk's Curves contrast effect to VR frames.
Uses S-curves to increase contrast without clipping highlights and shadows.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12),
which fires BEFORE UEVR copies the render target to VR eye textures.

Includes an ImGui settings panel with enable/disable, mode/formula selectors,
contrast slider, and reset button.

UEVR plugin wrapper: MIT license

Original shader:
Curves by Christian Cann Schuldt Jensen ~ CeeJay.dk
Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/Curves.fx
From the crosire/reshade-shaders community collection. No explicit license
was provided in the original file or repository.
*/


#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_curves_ps_src = R"(
cbuffer CurvesParams : register(b0) {
    int Mode;
    int Formula;
    float Contrast;
    float _pad0;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target {
    float4 colorInput = Scene.Sample(PointSampler, input.TexCoord);
    float3 lumCoeff = float3(0.2126, 0.7152, 0.0722);
    float Contrast_blend = Contrast;
    static const float PI = 3.1415927;

    float luma = dot(lumCoeff, colorInput.rgb);
    float3 chroma = colorInput.rgb - luma;

    float3 x;
    if (Mode == 0)
        x = luma;
    else if (Mode == 1) {
        x = chroma; x = x * 0.5 + 0.5;
    } else
        x = colorInput.rgb;

    [branch] if (Formula == 0) { x = sin(PI * 0.5 * x); x *= x; }
    else if (Formula == 1) { x = x - 0.5; x = (x / (0.5 + abs(x))) + 0.5; }
    else if (Formula == 2) { x = x * x * (3.0 - 2.0 * x); }
    else if (Formula == 3) { x = (1.0524 * exp(6.0 * x) - 1.05248) / (exp(6.0 * x) + 20.0855); }
    else if (Formula == 4) { x = x * (x * (1.5 - x) + 0.5); Contrast_blend = Contrast * 2.0; }
    else if (Formula == 5) { x = x * x * x * (x * (x * 6.0 - 15.0) + 10.0); }
    else if (Formula == 6) { x = x - 0.5; x = x / ((abs(x) * 1.25) + 0.375) + 0.5; }
    else if (Formula == 7) { x = (x*(x*(x*(x*(x*(x*(1.6*x - 7.2) + 10.8) - 4.2) - 3.6) + 2.7) - 1.8) + 2.7) * x * x; }
    else if (Formula == 8) { x = -0.5 * (x * 2.0 - 1.0) * (abs(x * 2.0 - 1.0) - 2.0) + 0.5; }
    else if (Formula == 9) {
        float3 xstep = step(x, 0.5); float3 xstep_shift = (xstep - 0.5);
        float3 shifted_x = x + xstep_shift;
        x = abs(xstep - sqrt(-shifted_x * shifted_x + shifted_x)) - xstep_shift;
        Contrast_blend = Contrast * 0.5;
    } else if (Formula == 10) {
        float3 a = x * x * 2.0; float3 b = (2.0 * -x + 4.0) * x - 1.0;
        x = (x < 0.5) ? a : b;
    }

    if (Mode == 0) {
        x = lerp(luma, x, Contrast_blend);
        colorInput.rgb = x + chroma;
    } else if (Mode == 1) {
        x = x * 2.0 - 1.0;
        float3 color = luma + x;
        colorInput.rgb = lerp(colorInput.rgb, color, Contrast_blend);
    } else {
        colorInput.rgb = lerp(colorInput.rgb, x, Contrast_blend);
    }
    return colorInput;
}
)";

struct CurvesParamsCB {
    int   Mode;
    int   Formula;
    float Contrast;
    float _pad0;
};
static_assert(sizeof(CurvesParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* CURVES_VERSION = "1.1.0";
static constexpr int   DEFAULT_MODE     = 0;
static constexpr int   DEFAULT_FORMULA  = 4;
static constexpr float DEFAULT_CONTRAST = 0.65f;

static const char* g_curves_mode_names[] = { "Luma", "Chroma", "Both" };
static const char* g_curves_formula_names[] = {
    "Sine", "Abs split", "Smoothstep", "Exp formula",
    "Simplified Catmull-Rom", "Perlin's Smootherstep", "Abs add",
    "Technicolor Cinestyle", "Parabola", "Half-circles", "Polynomial split"
};

class CurvesPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled  = false;
    int   m_mode     = DEFAULT_MODE;
    int   m_formula  = DEFAULT_FORMULA;
    float m_contrast = DEFAULT_CONTRAST;

    fx::SinglePassEffect<CurvesParamsCB> m_fx{ g_curves_ps_src };

    void on_initialize() override {
        API::get()->log_info("[Curves] Plugin initialized (v%s)", CURVES_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable ---
    std::string preset_section_name() const override { return "Curves"; }
    int render_order() const override { return 600; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",  m_enabled ? "1" : "0"},
            {"mode",     std::to_string(m_mode)},
            {"formula",  std::to_string(m_formula)},
            {"contrast", std::to_string(m_contrast)},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* k, float& out, float lo, float hi) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { float v = std::stof(it->second); if (v<lo) v=lo; if (v>hi) v=hi; out = v; } catch (...) {}
        };
        auto get_int = [&](const char* k, int& out, int lo, int hi) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { int v = std::stoi(it->second); if (v<lo) v=lo; if (v>hi) v=hi; out = v; } catch (...) {}
        };
        auto it = kv.find("enabled");
        if (it != kv.end()) m_enabled = (it->second != "0" && !it->second.empty());
        get_int("mode",     m_mode,    0,  2);
        get_int("formula",  m_formula, 0, 10);
        get_float("contrast", m_contrast, -1.0f, 1.0f);
    }

    void reset_to_defaults() override {
        m_enabled  = false;
        m_mode     = DEFAULT_MODE;
        m_formula  = DEFAULT_FORMULA;
        m_contrast = DEFAULT_CONTRAST;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Curves Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", CURVES_VERSION);
            ImGui::TextWrapped("11 mathematical contrast curves applied to luma, chroma, or RGB. Use after LevelsPlus for cinematic feel. Different formulas have different roll-off; experiment.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##CRV", &m_enabled);

            if (ImGui::Combo("Mode", &m_mode, g_curves_mode_names, 3)) changed = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channel: Luma, Chroma, or Both (full RGB)");
            ImGui::SameLine();
            if (ImGui::Button("Reset##CRV_mode")) { m_mode = DEFAULT_MODE; changed = true; }

            if (ImGui::Combo("Formula", &m_formula, g_curves_formula_names, IM_ARRAYSIZE(g_curves_formula_names)))
                changed = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Different curve shapes give different contrast feel");
            ImGui::SameLine();
            if (ImGui::Button("Reset##CRV_formula")) { m_formula = DEFAULT_FORMULA; changed = true; }

            changed |= ImGui::SliderFloat("Contrast", &m_contrast, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Negative = reduce contrast, positive = increase");
            ImGui::SameLine();
            if (ImGui::Button("Reset##CRV_contrast")) { m_contrast = DEFAULT_CONTRAST; changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##CRV")) {
                m_mode = DEFAULT_MODE; m_formula = DEFAULT_FORMULA; m_contrast = DEFAULT_CONTRAST;
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        CurvesParamsCB cb{};
        cb.Mode = m_mode;
        cb.Formula = m_formula;
        cb.Contrast = m_contrast;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<CurvesPlugin> g_plugin{ new CurvesPlugin() };
