/*
LevelsPlus Plugin for UEVR
===========================
A UEVR C++ plugin that applies LevelsPlus post-processing to VR frames.
Based on CeeJay.dk's Levels v1.8.3 with ACES tonemapping by MJP/Stephen Hill.

Features:
- RGB input/output black & white points with gamma
- Color range shift (for TV→PC range expansion)
- Three ACES tonemapping modes
- Highlight clipping visualisation (debug)

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license

Original shader licenses:
Levels v1.8.3 by Christian Cann Schuldt Jensen ~ CeeJay.dk
updated to 1.3+ by Kirill Yarovoy ~ v00d00m4n
Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/LevelsPlus.fx
From the crosire/reshade-shaders community collection. No explicit license
was provided for the Levels portion.

ACES tonemapping code from Baking Lab by MJP and David Neubelt
http://mynameismjp.wordpress.com/
All code licensed under the MIT license.
The code was originally written by Stephen Hill (@self_shadow), who deserves all
credit for coming up with this fit and implementing it.
*/


#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_levelsplus_ps_src = R"(
cbuffer LevelsParams : register(b0) {
    float3 InputBlackPoint;   float _pad0;
    float3 InputWhitePoint;   float _pad1;
    float3 InputGamma;        float _pad2;
    float3 OutputBlackPoint;  float _pad3;
    float3 OutputWhitePoint;  float _pad4;
    float3 ColorRangeShift;   float ColorRangeShiftSw;
    float3 ACESLumPct;        int   ACESMode;
    int   EnableLevels;       int   HighlightClipping;
    int2  _pad5;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

static const float3x3 ACESInputMat = float3x3(
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777
);
static const float3x3 ACESOutputMat = float3x3(
     1.60475, -0.53108, -0.07367,
    -0.10208,  1.10813, -0.00605,
    -0.00327, -0.07276,  1.07602
);

float3 RRTAndODTFit(float3 v) {
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}
float3 ACESFitted(float3 color) {
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
}
float3 ACESFilmRec2020old(float3 color) {
    float Slope = 15.8f, Toe = 2.12f, Shoulder = 1.2f, BlackClip = 5.92f, WhiteClip = 1.9f;
    color = color * ACESLumPct * 0.005f;
    return (color * (Slope * color + Toe)) / (color * (Shoulder * color + BlackClip) + WhiteClip);
}
float3 ACESFilmRec2020(float3 color) {
    float Slope = 0.98, Toe = 0.3, Shoulder = 0.22, BlackClip = 0, WhiteClip = 0.025;
    color = color * ACESLumPct * 0.005f;
    return (color * (Slope * color + Toe)) / (color * (Shoulder * color + BlackClip) + WhiteClip);
}

float4 main(PSInput input) : SV_Target {
    float3 InputColor = Scene.Sample(PointSampler, input.TexCoord).rgb;
    float3 OutputColor = InputColor;

    if (EnableLevels) {
        OutputColor = pow(
            max(((InputColor + (ColorRangeShift * ColorRangeShiftSw)) - InputBlackPoint)
                / (InputWhitePoint - InputBlackPoint), 0.0),
            InputGamma
        ) * (OutputWhitePoint - OutputBlackPoint) + OutputBlackPoint;
    }
    if (ACESMode == 1)      OutputColor = ACESFilmRec2020old(OutputColor);
    else if (ACESMode == 2) OutputColor = ACESFilmRec2020(OutputColor);
    else if (ACESMode == 3) OutputColor = ACESFitted(OutputColor);

    if (HighlightClipping) {
        float3 Clipped = OutputColor;
        Clipped = any(OutputColor > saturate(OutputColor)) ? float3(1.0, 1.0, 0.0) : Clipped;
        Clipped = all(OutputColor > saturate(OutputColor)) ? float3(1.0, 0.0, 0.0) : Clipped;
        Clipped = any(OutputColor < saturate(OutputColor)) ? float3(0.0, 1.0, 1.0) : Clipped;
        Clipped = all(OutputColor < saturate(OutputColor)) ? float3(0.0, 0.0, 1.0) : Clipped;
        OutputColor = Clipped;
    }
    return float4(saturate(OutputColor), 1.0);
}
)";

struct LevelsPlusCB {
    float InputBlackPoint[3];   float _pad0;
    float InputWhitePoint[3];   float _pad1;
    float InputGamma[3];        float _pad2;
    float OutputBlackPoint[3];  float _pad3;
    float OutputWhitePoint[3];  float _pad4;
    float ColorRangeShift[3];   float ColorRangeShiftSw;
    float ACESLumPct[3];        int   ACESMode;
    int   EnableLevels;         int   HighlightClipping;
    int   _pad5[2];
};
static_assert(sizeof(LevelsPlusCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* LEVELSPLUS_VERSION = "1.1.0";
static constexpr float LP_DEFAULT_IN_BLACK[3]   = { 16.0f/255.0f, 18.0f/255.0f, 20.0f/255.0f };
static constexpr float LP_DEFAULT_IN_WHITE[3]   = { 233.0f/255.0f, 222.0f/255.0f, 211.0f/255.0f };
static constexpr float LP_DEFAULT_IN_GAMMA[3]   = { 1.0f, 1.0f, 1.0f };
static constexpr float LP_DEFAULT_OUT_BLACK[3]  = { 0.0f, 0.0f, 0.0f };
static constexpr float LP_DEFAULT_OUT_WHITE[3]  = { 1.0f, 1.0f, 1.0f };
static constexpr float LP_DEFAULT_RANGE_SHIFT[3]= { 0.0f, 0.0f, 0.0f };
static constexpr int   LP_DEFAULT_RANGE_SHIFT_SW= 0;
static constexpr int   LP_DEFAULT_ACES_MODE     = 0;
static constexpr float LP_DEFAULT_ACES_LUM[3]   = { 100.0f, 100.0f, 100.0f };
static constexpr bool  LP_DEFAULT_ENABLE_LEVELS = true;
static constexpr bool  LP_DEFAULT_HIGHLIGHT_CLIP= false;

static const char* g_aces_names[]  = { "None", "ACES Old (Rec2020)", "ACES (Rec2020)", "ACES Fitted" };
static const char* g_shift_names[] = { "Downshift (-1)", "Disabled (0)", "Upshift (+1)" };

class LevelsPlusPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled            = false;
    bool  m_enable_levels      = LP_DEFAULT_ENABLE_LEVELS;
    bool  m_highlight_clipping = LP_DEFAULT_HIGHLIGHT_CLIP;
    int   m_range_shift_sw     = LP_DEFAULT_RANGE_SHIFT_SW;
    int   m_aces_mode          = LP_DEFAULT_ACES_MODE;

    float m_in_black[3]    = { LP_DEFAULT_IN_BLACK[0],    LP_DEFAULT_IN_BLACK[1],    LP_DEFAULT_IN_BLACK[2]    };
    float m_in_white[3]    = { LP_DEFAULT_IN_WHITE[0],    LP_DEFAULT_IN_WHITE[1],    LP_DEFAULT_IN_WHITE[2]    };
    float m_in_gamma[3]    = { LP_DEFAULT_IN_GAMMA[0],    LP_DEFAULT_IN_GAMMA[1],    LP_DEFAULT_IN_GAMMA[2]    };
    float m_out_black[3]   = { LP_DEFAULT_OUT_BLACK[0],   LP_DEFAULT_OUT_BLACK[1],   LP_DEFAULT_OUT_BLACK[2]   };
    float m_out_white[3]   = { LP_DEFAULT_OUT_WHITE[0],   LP_DEFAULT_OUT_WHITE[1],   LP_DEFAULT_OUT_WHITE[2]   };
    float m_range_shift[3] = { LP_DEFAULT_RANGE_SHIFT[0], LP_DEFAULT_RANGE_SHIFT[1], LP_DEFAULT_RANGE_SHIFT[2] };
    float m_aces_lum[3]    = { LP_DEFAULT_ACES_LUM[0],    LP_DEFAULT_ACES_LUM[1],    LP_DEFAULT_ACES_LUM[2]    };

    fx::SinglePassEffect<LevelsPlusCB> m_fx{ g_levelsplus_ps_src };

    void on_initialize() override {
        API::get()->log_info("[LevelsPlus] Plugin initialized (v%s)", LEVELSPLUS_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable ---
    std::string preset_section_name() const override { return "LevelsPlus"; }
    int render_order() const override { return 100; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",            m_enabled ? "1" : "0"},
            {"enable_levels",      m_enable_levels ? "1" : "0"},
            {"highlight_clipping", m_highlight_clipping ? "1" : "0"},
            {"range_shift_sw",     std::to_string(m_range_shift_sw)},
            {"aces_mode",          std::to_string(m_aces_mode)},
            {"in_black_r",  std::to_string(m_in_black[0])},
            {"in_black_g",  std::to_string(m_in_black[1])},
            {"in_black_b",  std::to_string(m_in_black[2])},
            {"in_white_r",  std::to_string(m_in_white[0])},
            {"in_white_g",  std::to_string(m_in_white[1])},
            {"in_white_b",  std::to_string(m_in_white[2])},
            {"in_gamma_r",  std::to_string(m_in_gamma[0])},
            {"in_gamma_g",  std::to_string(m_in_gamma[1])},
            {"in_gamma_b",  std::to_string(m_in_gamma[2])},
            {"out_black_r", std::to_string(m_out_black[0])},
            {"out_black_g", std::to_string(m_out_black[1])},
            {"out_black_b", std::to_string(m_out_black[2])},
            {"out_white_r", std::to_string(m_out_white[0])},
            {"out_white_g", std::to_string(m_out_white[1])},
            {"out_white_b", std::to_string(m_out_white[2])},
            {"range_shift_r", std::to_string(m_range_shift[0])},
            {"range_shift_g", std::to_string(m_range_shift[1])},
            {"range_shift_b", std::to_string(m_range_shift[2])},
            {"aces_lum_r", std::to_string(m_aces_lum[0])},
            {"aces_lum_g", std::to_string(m_aces_lum[1])},
            {"aces_lum_b", std::to_string(m_aces_lum[2])},
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
        auto get_bool = [&](const char* k, bool& out) {
            auto it = kv.find(k);
            if (it != kv.end()) out = (it->second != "0" && !it->second.empty());
        };
        get_bool("enabled",            m_enabled);
        get_bool("enable_levels",      m_enable_levels);
        get_bool("highlight_clipping", m_highlight_clipping);
        get_int("range_shift_sw", m_range_shift_sw, -1, 1);
        get_int("aces_mode",      m_aces_mode,       0, 3);
        get_float("in_black_r",  m_in_black[0],  0.0f, 1.0f);
        get_float("in_black_g",  m_in_black[1],  0.0f, 1.0f);
        get_float("in_black_b",  m_in_black[2],  0.0f, 1.0f);
        get_float("in_white_r",  m_in_white[0],  0.0f, 1.0f);
        get_float("in_white_g",  m_in_white[1],  0.0f, 1.0f);
        get_float("in_white_b",  m_in_white[2],  0.0f, 1.0f);
        get_float("in_gamma_r",  m_in_gamma[0],  0.01f, 10.0f);
        get_float("in_gamma_g",  m_in_gamma[1],  0.01f, 10.0f);
        get_float("in_gamma_b",  m_in_gamma[2],  0.01f, 10.0f);
        get_float("out_black_r", m_out_black[0], 0.0f, 1.0f);
        get_float("out_black_g", m_out_black[1], 0.0f, 1.0f);
        get_float("out_black_b", m_out_black[2], 0.0f, 1.0f);
        get_float("out_white_r", m_out_white[0], 0.0f, 1.0f);
        get_float("out_white_g", m_out_white[1], 0.0f, 1.0f);
        get_float("out_white_b", m_out_white[2], 0.0f, 1.0f);
        get_float("range_shift_r", m_range_shift[0], 0.0f, 1.0f);
        get_float("range_shift_g", m_range_shift[1], 0.0f, 1.0f);
        get_float("range_shift_b", m_range_shift[2], 0.0f, 1.0f);
        get_float("aces_lum_r", m_aces_lum[0], 0.0f, 200.0f);
        get_float("aces_lum_g", m_aces_lum[1], 0.0f, 200.0f);
        get_float("aces_lum_b", m_aces_lum[2], 0.0f, 200.0f);
    }

    void reset_to_defaults() override {
        m_enabled            = false;
        m_enable_levels      = LP_DEFAULT_ENABLE_LEVELS;
        m_highlight_clipping = LP_DEFAULT_HIGHLIGHT_CLIP;
        m_range_shift_sw     = LP_DEFAULT_RANGE_SHIFT_SW;
        m_aces_mode          = LP_DEFAULT_ACES_MODE;
        for (int i = 0; i < 3; ++i) {
            m_in_black[i]    = LP_DEFAULT_IN_BLACK[i];
            m_in_white[i]    = LP_DEFAULT_IN_WHITE[i];
            m_in_gamma[i]    = LP_DEFAULT_IN_GAMMA[i];
            m_out_black[i]   = LP_DEFAULT_OUT_BLACK[i];
            m_out_white[i]   = LP_DEFAULT_OUT_WHITE[i];
            m_range_shift[i] = LP_DEFAULT_RANGE_SHIFT[i];
            m_aces_lum[i]    = LP_DEFAULT_ACES_LUM[i];
        }
    }

    static void reset3(float dst[3], const float src[3]) { dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("LevelsPlus Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", LEVELSPLUS_VERSION);
            ImGui::TextWrapped("The #1 fix for VR. Remaps black/white points so darks are actually dark and whites are bright. Trades some shadow detail for deeper blacks — almost always worth it. Has per-channel gamma and optional ACES tone mapping. Start here.");
            bool changed = false;

            changed |= ImGui::Checkbox("Enabled##LP", &m_enabled);

            changed |= ImGui::Checkbox("Enable Levels##LP", &m_enable_levels);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the levels remapping (black/white point + gamma)");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LP_enable_levels")) { m_enable_levels = LP_DEFAULT_ENABLE_LEVELS; changed = true; }

            if (m_enable_levels) {
                ImGui::Separator();
                ImGui::Text("Input Levels");

                changed |= ImGui::ColorEdit3("Input Black Point", m_in_black);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Colors darker than this become pure black");
                ImGui::SameLine();
                if (ImGui::Button("Reset##LP_inblack")) { reset3(m_in_black, LP_DEFAULT_IN_BLACK); changed = true; }

                changed |= ImGui::ColorEdit3("Input White Point", m_in_white);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Colors brighter than this become pure white");
                ImGui::SameLine();
                if (ImGui::Button("Reset##LP_inwhite")) { reset3(m_in_white, LP_DEFAULT_IN_WHITE); changed = true; }

                changed |= ImGui::DragFloat3("Input Gamma", m_in_gamma, 0.01f, 0.01f, 10.0f, "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Per-channel midtone adjustment. <1 = brighter mids, >1 = darker mids");
                ImGui::SameLine();
                if (ImGui::Button("Reset##LP_ingamma")) { reset3(m_in_gamma, LP_DEFAULT_IN_GAMMA); changed = true; }

                ImGui::Separator();
                ImGui::Text("Output Levels");

                changed |= ImGui::ColorEdit3("Output Black Point", m_out_black);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Darkest output value. Raise to lighten shadows");
                ImGui::SameLine();
                if (ImGui::Button("Reset##LP_outblack")) { reset3(m_out_black, LP_DEFAULT_OUT_BLACK); changed = true; }

                changed |= ImGui::ColorEdit3("Output White Point", m_out_white);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brightest output value. Lower to dim highlights");
                ImGui::SameLine();
                if (ImGui::Button("Reset##LP_outwhite")) { reset3(m_out_white, LP_DEFAULT_OUT_WHITE); changed = true; }

                ImGui::Separator();
                ImGui::Text("Color Range Shift");

                changed |= ImGui::ColorEdit3("Range Shift", m_range_shift);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shifts the midpoint of each color channel");
                ImGui::SameLine();
                if (ImGui::Button("Reset##LP_rangeshift")) { reset3(m_range_shift, LP_DEFAULT_RANGE_SHIFT); changed = true; }

                int shift_idx = m_range_shift_sw + 1;
                if (ImGui::Combo("Shift Direction", &shift_idx, g_shift_names, 3)) {
                    m_range_shift_sw = shift_idx - 1;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Subtract = shift toward black, Add = shift toward white, Off = no shift");
                ImGui::SameLine();
                if (ImGui::Button("Reset##LP_shiftdir")) { m_range_shift_sw = LP_DEFAULT_RANGE_SHIFT_SW; changed = true; }
            }

            ImGui::Separator();
            ImGui::Text("ACES Tonemapping");
            if (ImGui::Combo("ACES Mode", &m_aces_mode, g_aces_names, 4)) changed = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off = linear levels only. Film/RRT/RRT+ = cinematic tone curve");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LP_acesmode")) { m_aces_mode = LP_DEFAULT_ACES_MODE; changed = true; }

            if (m_aces_mode > 0) {
                changed |= ImGui::DragFloat3("ACES Luminance %", m_aces_lum, 1.0f, 0.0f, 200.0f, "%.0f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Per-channel luminance weight for ACES. Higher = brighter in that channel");
                ImGui::SameLine();
                if (ImGui::Button("Reset##LP_aceslum")) { reset3(m_aces_lum, LP_DEFAULT_ACES_LUM); changed = true; }
            }

            ImGui::Separator();
            changed |= ImGui::Checkbox("Highlight Clipping (debug)", &m_highlight_clipping);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shows clipped pixels: blue=black, yellow=white, red=both");
            ImGui::SameLine();
            if (ImGui::Button("Reset##LP_clip")) { m_highlight_clipping = LP_DEFAULT_HIGHLIGHT_CLIP; changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##LP")) {
                m_enable_levels      = LP_DEFAULT_ENABLE_LEVELS;
                m_highlight_clipping = LP_DEFAULT_HIGHLIGHT_CLIP;
                m_range_shift_sw     = LP_DEFAULT_RANGE_SHIFT_SW;
                m_aces_mode          = LP_DEFAULT_ACES_MODE;
                reset3(m_in_black,    LP_DEFAULT_IN_BLACK);
                reset3(m_in_white,    LP_DEFAULT_IN_WHITE);
                reset3(m_in_gamma,    LP_DEFAULT_IN_GAMMA);
                reset3(m_out_black,   LP_DEFAULT_OUT_BLACK);
                reset3(m_out_white,   LP_DEFAULT_OUT_WHITE);
                reset3(m_range_shift, LP_DEFAULT_RANGE_SHIFT);
                reset3(m_aces_lum,    LP_DEFAULT_ACES_LUM);
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        LevelsPlusCB cb{};
        for (int i = 0; i < 3; ++i) {
            cb.InputBlackPoint[i]  = m_in_black[i];
            cb.InputWhitePoint[i]  = m_in_white[i];
            cb.InputGamma[i]       = m_in_gamma[i];
            cb.OutputBlackPoint[i] = m_out_black[i];
            cb.OutputWhitePoint[i] = m_out_white[i];
            cb.ColorRangeShift[i]  = m_range_shift[i];
            cb.ACESLumPct[i]       = m_aces_lum[i];
        }
        cb.ColorRangeShiftSw  = (float)m_range_shift_sw;
        cb.ACESMode           = m_aces_mode;
        cb.EnableLevels       = m_enable_levels ? 1 : 0;
        cb.HighlightClipping  = m_highlight_clipping ? 1 : 0;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<LevelsPlusPlugin> g_plugin{ new LevelsPlusPlugin() };
