/*
FilmicPass Plugin for UEVR
===========================
A UEVR C++ plugin that applies the FilmicPass effect to VR frames.
Common color adjustments to mimic a cinema-like look: sigmoid curve per-channel,
bleach bypass, saturation/fade, per-channel gamma, and soft-light blending.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Original shader:
  FilmicPass by an unknown author (from the standard ReShade shader pack).
  Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/FilmicPass.fx
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

static const char* g_filmicpass_ps_src = R"(
cbuffer FilmicPassParams : register(b0) {
    float Strength;
    float Fade;
    float Contrast;
    float Linearization;
    float Bleach;
    float Saturation;
    float RedCurve;
    float GreenCurve;
    float BlueCurve;
    float BaseCurve;
    float BaseGamma;
    float EffectGamma;
    float EffectGammaR;
    float EffectGammaG;
    float EffectGammaB;
    float _pad0;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

static const float3 LumCoeff = float3(0.212656, 0.715158, 0.072186);

float4 main(PSInput input) : SV_Target {
    float3 B = Scene.Sample(PointSampler, input.TexCoord).rgb;
    float3 H = 0.01;

    B = saturate(B);
    B = pow(B, Linearization);
    B = lerp(H, B, Contrast);

    float A = dot(B, LumCoeff);
    float3 D = A;

    B = pow(abs(B), 1.0 / BaseGamma);

    float a = RedCurve, b = GreenCurve, c = BlueCurve, d = BaseCurve;
    float y = 1.0 / (1.0 + exp(a / 2.0));
    float z = 1.0 / (1.0 + exp(b / 2.0));
    float w = 1.0 / (1.0 + exp(c / 2.0));
    float v = 1.0 / (1.0 + exp(d / 2.0));

    float3 C = B;
    D.r = (1.0 / (1.0 + exp(-a * (D.r - 0.5))) - y) / (1.0 - 2.0 * y);
    D.g = (1.0 / (1.0 + exp(-b * (D.g - 0.5))) - z) / (1.0 - 2.0 * z);
    D.b = (1.0 / (1.0 + exp(-c * (D.b - 0.5))) - w) / (1.0 - 2.0 * w);

    D = pow(abs(D), 1.0 / EffectGamma);

    float3 Di = 1.0 - D;
    D = lerp(D, Di, Bleach);

    D.r = pow(abs(D.r), 1.0 / EffectGammaR);
    D.g = pow(abs(D.g), 1.0 / EffectGammaG);
    D.b = pow(abs(D.b), 1.0 / EffectGammaB);

    if (D.r < 0.5) C.r = (2.0*D.r - 1.0) * (B.r - B.r*B.r) + B.r;
    else            C.r = (2.0*D.r - 1.0) * (sqrt(B.r) - B.r) + B.r;
    if (D.g < 0.5) C.g = (2.0*D.g - 1.0) * (B.g - B.g*B.g) + B.g;
    else            C.g = (2.0*D.g - 1.0) * (sqrt(B.g) - B.g) + B.g;
    if (D.b < 0.5) C.b = (2.0*D.b - 1.0) * (B.b - B.b*B.b) + B.b;
    else            C.b = (2.0*D.b - 1.0) * (sqrt(B.b) - B.b) + B.b;

    float3 F = lerp(B, C, Strength);
    F = (1.0 / (1.0 + exp(-d * (F - 0.5))) - v) / (1.0 - 2.0 * v);

    float r2R = 1.0 - Saturation; float g2R = Saturation;        float b2R = Saturation;
    float r2G = Saturation;        float g2G = (1.0 - Fade) - Saturation; float b2G = Fade + Saturation;
    float r2B = Saturation;        float g2B = Fade + Saturation; float b2B = (1.0 - Fade) - Saturation;

    float3 iF = F;
    F.r = iF.r*r2R + iF.g*g2R + iF.b*b2R;
    F.g = iF.r*r2G + iF.g*g2G + iF.b*b2G;
    F.b = iF.r*r2B + iF.g*g2B + iF.b*b2B;

    float N = dot(F, LumCoeff);
    float3 Cn = F;
    if (N < 0.5) Cn = (2.0*N - 1.0) * (F - F*F) + F;
    else         Cn = (2.0*N - 1.0) * (sqrt(F) - F) + F;
    Cn = pow(max(Cn, 0), 1.0 / Linearization);

    float3 Fn = lerp(B, Cn, Strength);
    return float4(Fn, 1.0);
}
)";

struct FilmicPassParamsCB {
    float Strength;
    float Fade;
    float Contrast;
    float Linearization;
    float Bleach;
    float Saturation;
    float RedCurve;
    float GreenCurve;
    float BlueCurve;
    float BaseCurve;
    float BaseGamma;
    float EffectGamma;
    float EffectGammaR;
    float EffectGammaG;
    float EffectGammaB;
    float _pad0;
};
static_assert(sizeof(FilmicPassParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* FP_VERSION = "1.1.0";
static constexpr float FP_DEFAULT_STRENGTH       = 0.85f;
static constexpr float FP_DEFAULT_FADE           = 0.4f;
static constexpr float FP_DEFAULT_CONTRAST       = 1.0f;
static constexpr float FP_DEFAULT_LINEARIZATION  = 0.5f;
static constexpr float FP_DEFAULT_BLEACH         = 0.0f;
static constexpr float FP_DEFAULT_SATURATION     = -0.15f;
static constexpr float FP_DEFAULT_RED_CURVE      = 1.0f;
static constexpr float FP_DEFAULT_GREEN_CURVE    = 1.0f;
static constexpr float FP_DEFAULT_BLUE_CURVE     = 1.0f;
static constexpr float FP_DEFAULT_BASE_CURVE     = 1.5f;
static constexpr float FP_DEFAULT_BASE_GAMMA     = 1.0f;
static constexpr float FP_DEFAULT_EFFECT_GAMMA   = 0.65f;
static constexpr float FP_DEFAULT_EFFECT_GAMMA_R = 1.0f;
static constexpr float FP_DEFAULT_EFFECT_GAMMA_G = 1.0f;
static constexpr float FP_DEFAULT_EFFECT_GAMMA_B = 1.0f;

class FilmicPassPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled        = false;
    float m_strength       = FP_DEFAULT_STRENGTH;
    float m_fade           = FP_DEFAULT_FADE;
    float m_contrast       = FP_DEFAULT_CONTRAST;
    float m_linearization  = FP_DEFAULT_LINEARIZATION;
    float m_bleach         = FP_DEFAULT_BLEACH;
    float m_saturation     = FP_DEFAULT_SATURATION;
    float m_red_curve      = FP_DEFAULT_RED_CURVE;
    float m_green_curve    = FP_DEFAULT_GREEN_CURVE;
    float m_blue_curve     = FP_DEFAULT_BLUE_CURVE;
    float m_base_curve     = FP_DEFAULT_BASE_CURVE;
    float m_base_gamma     = FP_DEFAULT_BASE_GAMMA;
    float m_effect_gamma   = FP_DEFAULT_EFFECT_GAMMA;
    float m_effect_gamma_r = FP_DEFAULT_EFFECT_GAMMA_R;
    float m_effect_gamma_g = FP_DEFAULT_EFFECT_GAMMA_G;
    float m_effect_gamma_b = FP_DEFAULT_EFFECT_GAMMA_B;

    fx::SinglePassEffect<FilmicPassParamsCB> m_fx{ g_filmicpass_ps_src };

    void on_initialize() override {
        API::get()->log_info("[FilmicPass] Plugin initialized (v%s)", FP_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "FilmicPass"; }
    int render_order() const override { return 1300; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",        m_enabled ? "1" : "0"},
            {"strength",       std::to_string(m_strength)},
            {"fade",           std::to_string(m_fade)},
            {"contrast",       std::to_string(m_contrast)},
            {"linearization",  std::to_string(m_linearization)},
            {"bleach",         std::to_string(m_bleach)},
            {"saturation",     std::to_string(m_saturation)},
            {"red_curve",      std::to_string(m_red_curve)},
            {"green_curve",    std::to_string(m_green_curve)},
            {"blue_curve",     std::to_string(m_blue_curve)},
            {"base_curve",     std::to_string(m_base_curve)},
            {"base_gamma",     std::to_string(m_base_gamma)},
            {"effect_gamma",   std::to_string(m_effect_gamma)},
            {"effect_gamma_r", std::to_string(m_effect_gamma_r)},
            {"effect_gamma_g", std::to_string(m_effect_gamma_g)},
            {"effect_gamma_b", std::to_string(m_effect_gamma_b)},
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
        get_float("strength",       m_strength,       0.05f, 1.5f);
        get_float("fade",           m_fade,           0.0f,  0.6f);
        get_float("contrast",       m_contrast,       0.5f,  2.0f);
        get_float("linearization",  m_linearization,  0.5f,  2.0f);
        get_float("bleach",         m_bleach,        -0.5f,  1.0f);
        get_float("saturation",     m_saturation,    -1.0f,  1.0f);
        get_float("red_curve",      m_red_curve,      0.0f,  2.0f);
        get_float("green_curve",    m_green_curve,    0.0f,  2.0f);
        get_float("blue_curve",     m_blue_curve,     0.0f,  2.0f);
        get_float("base_curve",     m_base_curve,     0.0f,  2.0f);
        get_float("base_gamma",     m_base_gamma,     0.7f,  2.0f);
        get_float("effect_gamma",   m_effect_gamma,   0.0f,  2.0f);
        get_float("effect_gamma_r", m_effect_gamma_r, 0.0f,  2.0f);
        get_float("effect_gamma_g", m_effect_gamma_g, 0.0f,  2.0f);
        get_float("effect_gamma_b", m_effect_gamma_b, 0.0f,  2.0f);
    }

    void reset_to_defaults() override {
        m_enabled        = false;
        m_strength       = FP_DEFAULT_STRENGTH;
        m_fade           = FP_DEFAULT_FADE;
        m_contrast       = FP_DEFAULT_CONTRAST;
        m_linearization  = FP_DEFAULT_LINEARIZATION;
        m_bleach         = FP_DEFAULT_BLEACH;
        m_saturation     = FP_DEFAULT_SATURATION;
        m_red_curve      = FP_DEFAULT_RED_CURVE;
        m_green_curve    = FP_DEFAULT_GREEN_CURVE;
        m_blue_curve     = FP_DEFAULT_BLUE_CURVE;
        m_base_curve     = FP_DEFAULT_BASE_CURVE;
        m_base_gamma     = FP_DEFAULT_BASE_GAMMA;
        m_effect_gamma   = FP_DEFAULT_EFFECT_GAMMA;
        m_effect_gamma_r = FP_DEFAULT_EFFECT_GAMMA_R;
        m_effect_gamma_g = FP_DEFAULT_EFFECT_GAMMA_G;
        m_effect_gamma_b = FP_DEFAULT_EFFECT_GAMMA_B;
    }
    // ----------------------------------------------------------------------

    bool slider(const char* label, const char* tooltip, float* v, float lo, float hi, float def, const char* reset_id) {
        bool ch = ImGui::SliderFloat(label, v, lo, hi, "%.2f");
        if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
        if (ImGui::Button(reset_id)) { *v = def; ch = true; }
        return ch;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Filmic Pass Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", FP_VERSION);
            ImGui::TextWrapped("Cinematic film tone mapping. Per-channel sigmoid curves + bleach + fade matrix. Heavier than LevelsPlus, more stylized. Strength below 1.0 keeps it subtle.");
            bool ch = false;
            ch |= ImGui::Checkbox("Enabled##FP", &m_enabled);
            ch |= slider("Strength",        "Overall blend with original", &m_strength,       0.05f, 1.5f, FP_DEFAULT_STRENGTH,       "Reset##FP_strength");
            ch |= slider("Fade",            "Mid-tone fade",               &m_fade,           0.0f,  0.6f, FP_DEFAULT_FADE,           "Reset##FP_fade");
            ch |= slider("Contrast",        "Pre-curve contrast",          &m_contrast,       0.5f,  2.0f, FP_DEFAULT_CONTRAST,       "Reset##FP_contrast");
            ch |= slider("Linearization",   "Input gamma",                 &m_linearization,  0.5f,  2.0f, FP_DEFAULT_LINEARIZATION,  "Reset##FP_linearization");
            ch |= slider("Bleach",          "Bleach bypass amount",        &m_bleach,        -0.5f,  1.0f, FP_DEFAULT_BLEACH,         "Reset##FP_bleach");
            ch |= slider("Saturation",      "Final saturation matrix",     &m_saturation,    -1.0f,  1.0f, FP_DEFAULT_SATURATION,     "Reset##FP_saturation");
            ImGui::Separator();
            ImGui::Text("Sigmoid Curves");
            ch |= slider("Red Curve",       nullptr, &m_red_curve,      0.0f, 2.0f, FP_DEFAULT_RED_CURVE,      "Reset##FP_red");
            ch |= slider("Green Curve",     nullptr, &m_green_curve,    0.0f, 2.0f, FP_DEFAULT_GREEN_CURVE,    "Reset##FP_green");
            ch |= slider("Blue Curve",      nullptr, &m_blue_curve,     0.0f, 2.0f, FP_DEFAULT_BLUE_CURVE,     "Reset##FP_blue");
            ch |= slider("Base Curve",      nullptr, &m_base_curve,     0.0f, 2.0f, FP_DEFAULT_BASE_CURVE,     "Reset##FP_base");
            ImGui::Separator();
            ImGui::Text("Gammas");
            ch |= slider("Base Gamma",      nullptr, &m_base_gamma,     0.7f, 2.0f, FP_DEFAULT_BASE_GAMMA,     "Reset##FP_basegamma");
            ch |= slider("Effect Gamma",    nullptr, &m_effect_gamma,   0.0f, 2.0f, FP_DEFAULT_EFFECT_GAMMA,   "Reset##FP_effectgamma");
            ch |= slider("Effect Gamma R",  nullptr, &m_effect_gamma_r, 0.0f, 2.0f, FP_DEFAULT_EFFECT_GAMMA_R, "Reset##FP_egr");
            ch |= slider("Effect Gamma G",  nullptr, &m_effect_gamma_g, 0.0f, 2.0f, FP_DEFAULT_EFFECT_GAMMA_G, "Reset##FP_egg");
            ch |= slider("Effect Gamma B",  nullptr, &m_effect_gamma_b, 0.0f, 2.0f, FP_DEFAULT_EFFECT_GAMMA_B, "Reset##FP_egb");

            ImGui::Spacing();
            if (ImGui::Button("Reset All##FP")) {
                m_strength       = FP_DEFAULT_STRENGTH;
                m_fade           = FP_DEFAULT_FADE;
                m_contrast       = FP_DEFAULT_CONTRAST;
                m_linearization  = FP_DEFAULT_LINEARIZATION;
                m_bleach         = FP_DEFAULT_BLEACH;
                m_saturation     = FP_DEFAULT_SATURATION;
                m_red_curve      = FP_DEFAULT_RED_CURVE;
                m_green_curve    = FP_DEFAULT_GREEN_CURVE;
                m_blue_curve     = FP_DEFAULT_BLUE_CURVE;
                m_base_curve     = FP_DEFAULT_BASE_CURVE;
                m_base_gamma     = FP_DEFAULT_BASE_GAMMA;
                m_effect_gamma   = FP_DEFAULT_EFFECT_GAMMA;
                m_effect_gamma_r = FP_DEFAULT_EFFECT_GAMMA_R;
                m_effect_gamma_g = FP_DEFAULT_EFFECT_GAMMA_G;
                m_effect_gamma_b = FP_DEFAULT_EFFECT_GAMMA_B;
                ch = true;
            }
            if (ch) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        FilmicPassParamsCB cb{};
        cb.Strength = m_strength;       cb.Fade = m_fade;
        cb.Contrast = m_contrast;       cb.Linearization = m_linearization;
        cb.Bleach = m_bleach;           cb.Saturation = m_saturation;
        cb.RedCurve = m_red_curve;      cb.GreenCurve = m_green_curve;
        cb.BlueCurve = m_blue_curve;    cb.BaseCurve = m_base_curve;
        cb.BaseGamma = m_base_gamma;    cb.EffectGamma = m_effect_gamma;
        cb.EffectGammaR = m_effect_gamma_r;
        cb.EffectGammaG = m_effect_gamma_g;
        cb.EffectGammaB = m_effect_gamma_b;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<FilmicPassPlugin> g_plugin{ new FilmicPassPlugin() };
