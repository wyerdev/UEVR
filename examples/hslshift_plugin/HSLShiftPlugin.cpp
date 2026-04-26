/*
HSLShift Plugin for UEVR
=========================
A UEVR C++ plugin that applies kingeric1992's HSLShift effect to VR frames.
Per-hue color shifting: remap individual color ranges (Red, Orange, Yellow,
Green, Cyan, Blue, Purple, Magenta) to new hues/saturations.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Original shader:
  HSL Processing Shader by kingeric1992
  Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/HSLShift.fx
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

// Faithful port of kingeric1992's HSLShift.fx
static const char* g_hslshift_ps_src = R"(
cbuffer HSLShiftParams : register(b0) {
    float3 HUERed;      float _pad0;
    float3 HUEOrange;   float _pad1;
    float3 HUEYellow;   float _pad2;
    float3 HUEGreen;    float _pad3;
    float3 HUECyan;     float _pad4;
    float3 HUEBlue;     float _pad5;
    float3 HUEPurple;   float _pad6;
    float3 HUEMagenta;  float _pad7;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

static const float HSL_Threshold_Base  = 0.05;
static const float HSL_Threshold_Curve = 1.0;

float3 RGB_to_HSL(float3 color) {
    float3 HSL = 0.0;
    float M = max(color.r, max(color.g, color.b));
    float C = M - min(color.r, min(color.g, color.b));
    HSL.z = M - 0.5 * C;
    if (C != 0.0) {
        float3 Delta = (color.brg - color.rgb) / C + float3(2.0, 4.0, 6.0);
        Delta *= step(M, color.gbr);
        HSL.x = frac(max(Delta.r, max(Delta.g, Delta.b)) / 6.0);
        HSL.y = (HSL.z == 1) ? 0.0 : C / (1 - abs(2 * HSL.z - 1));
    }
    return HSL;
}

float3 Hue_to_RGB(float h) {
    return saturate(float3(abs(h * 6.0 - 3.0) - 1.0,
                           2.0 - abs(h * 6.0 - 2.0),
                           2.0 - abs(h * 6.0 - 4.0)));
}

float3 HSL_to_RGB(float3 HSL) {
    return (Hue_to_RGB(HSL.x) - 0.5) * (1.0 - abs(2.0 * HSL.z - 1.0)) * HSL.y + HSL.z;
}

float4 main(PSInput input) : SV_Target {
    float3 color = Scene.Sample(PointSampler, input.TexCoord).rgb;
    float3 hsl = RGB_to_HSL(color);

    float4 node[9];
    node[0] = float4(HUERed,     0.0);
    node[1] = float4(HUEOrange,  30.0);
    node[2] = float4(HUEYellow,  60.0);
    node[3] = float4(HUEGreen,   120.0);
    node[4] = float4(HUECyan,    180.0);
    node[5] = float4(HUEBlue,    240.0);
    node[6] = float4(HUEPurple,  270.0);
    node[7] = float4(HUEMagenta, 300.0);
    node[8] = float4(HUERed,     360.0);

    int base = 0;
    for (int i = 0; i < 8; i++)
        if (node[i].a < hsl.r * 360.0) base = i;

    float w = saturate((hsl.r * 360.0 - node[base].a) / (node[base+1].a - node[base].a));

    float3 H0 = RGB_to_HSL(node[base].rgb);
    float3 H1 = RGB_to_HSL(node[base+1].rgb);
    H1.x += (H1.x < H0.x) ? 1.0 : 0.0;

    float3 shift = frac(lerp(H0, H1, w));
    float ww = max(hsl.g, 0.0) * max(1.0 - hsl.b, 0.0);
    shift.b = (shift.b - 0.5) * (pow(ww, HSL_Threshold_Curve) * (1.0 - HSL_Threshold_Base) + HSL_Threshold_Base) * 2.0;

    float3 result = HSL_to_RGB(saturate(float3(shift.r, hsl.g * (shift.g * 2.0), hsl.b * (1.0 + shift.b))));
    return float4(result, 1.0);
}
)";

struct HSLShiftParamsCB {
    float HUERed[3];      float _pad0;
    float HUEOrange[3];   float _pad1;
    float HUEYellow[3];   float _pad2;
    float HUEGreen[3];    float _pad3;
    float HUECyan[3];     float _pad4;
    float HUEBlue[3];     float _pad5;
    float HUEPurple[3];   float _pad6;
    float HUEMagenta[3];  float _pad7;
};
static_assert(sizeof(HSLShiftParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* HSL_VERSION = "1.1.0";
static constexpr float HSL_DEFAULT_RED[3]     = { 0.75f, 0.25f, 0.25f };
static constexpr float HSL_DEFAULT_ORANGE[3]  = { 0.75f, 0.50f, 0.25f };
static constexpr float HSL_DEFAULT_YELLOW[3]  = { 0.75f, 0.75f, 0.25f };
static constexpr float HSL_DEFAULT_GREEN[3]   = { 0.25f, 0.75f, 0.25f };
static constexpr float HSL_DEFAULT_CYAN[3]    = { 0.25f, 0.75f, 0.75f };
static constexpr float HSL_DEFAULT_BLUE[3]    = { 0.25f, 0.25f, 0.75f };
static constexpr float HSL_DEFAULT_PURPLE[3]  = { 0.50f, 0.25f, 0.75f };
static constexpr float HSL_DEFAULT_MAGENTA[3] = { 0.75f, 0.25f, 0.75f };

class HSLShiftPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled = false;
    float m_red[3]     = { HSL_DEFAULT_RED[0],     HSL_DEFAULT_RED[1],     HSL_DEFAULT_RED[2]     };
    float m_orange[3]  = { HSL_DEFAULT_ORANGE[0],  HSL_DEFAULT_ORANGE[1],  HSL_DEFAULT_ORANGE[2]  };
    float m_yellow[3]  = { HSL_DEFAULT_YELLOW[0],  HSL_DEFAULT_YELLOW[1],  HSL_DEFAULT_YELLOW[2]  };
    float m_green[3]   = { HSL_DEFAULT_GREEN[0],   HSL_DEFAULT_GREEN[1],   HSL_DEFAULT_GREEN[2]   };
    float m_cyan[3]    = { HSL_DEFAULT_CYAN[0],    HSL_DEFAULT_CYAN[1],    HSL_DEFAULT_CYAN[2]    };
    float m_blue[3]    = { HSL_DEFAULT_BLUE[0],    HSL_DEFAULT_BLUE[1],    HSL_DEFAULT_BLUE[2]    };
    float m_purple[3]  = { HSL_DEFAULT_PURPLE[0],  HSL_DEFAULT_PURPLE[1],  HSL_DEFAULT_PURPLE[2]  };
    float m_magenta[3] = { HSL_DEFAULT_MAGENTA[0], HSL_DEFAULT_MAGENTA[1], HSL_DEFAULT_MAGENTA[2] };

    fx::SinglePassEffect<HSLShiftParamsCB> m_fx{ g_hslshift_ps_src };

    void on_initialize() override {
        API::get()->log_info("[HSLShift] Plugin initialized (v%s)", HSL_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "HSLShift"; }
    int render_order() const override { return 1200; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        auto put3 = [](std::vector<std::pair<std::string,std::string>>& out, const char* prefix, const float v[3]) {
            out.push_back({std::string(prefix) + "_r", std::to_string(v[0])});
            out.push_back({std::string(prefix) + "_g", std::to_string(v[1])});
            out.push_back({std::string(prefix) + "_b", std::to_string(v[2])});
        };
        std::vector<std::pair<std::string,std::string>> out;
        out.push_back({"enabled", m_enabled ? "1" : "0"});
        put3(out, "red",     m_red);
        put3(out, "orange",  m_orange);
        put3(out, "yellow",  m_yellow);
        put3(out, "green",   m_green);
        put3(out, "cyan",    m_cyan);
        put3(out, "blue",    m_blue);
        put3(out, "purple",  m_purple);
        put3(out, "magenta", m_magenta);
        return out;
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const std::string& k, float& out, float lo, float hi) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try {
                float v = std::stof(it->second);
                if (v < lo) v = lo;
                if (v > hi) v = hi;
                out = v;
            } catch (...) {}
        };
        auto get3 = [&](const char* prefix, float v[3]) {
            get_float(std::string(prefix) + "_r", v[0], 0.0f, 1.0f);
            get_float(std::string(prefix) + "_g", v[1], 0.0f, 1.0f);
            get_float(std::string(prefix) + "_b", v[2], 0.0f, 1.0f);
        };
        auto it = kv.find("enabled");
        if (it != kv.end()) m_enabled = (it->second != "0" && !it->second.empty());
        get3("red",     m_red);
        get3("orange",  m_orange);
        get3("yellow",  m_yellow);
        get3("green",   m_green);
        get3("cyan",    m_cyan);
        get3("blue",    m_blue);
        get3("purple",  m_purple);
        get3("magenta", m_magenta);
    }

    void reset_to_defaults() override {
        m_enabled = false;
        reset3(m_red,     HSL_DEFAULT_RED);
        reset3(m_orange,  HSL_DEFAULT_ORANGE);
        reset3(m_yellow,  HSL_DEFAULT_YELLOW);
        reset3(m_green,   HSL_DEFAULT_GREEN);
        reset3(m_cyan,    HSL_DEFAULT_CYAN);
        reset3(m_blue,    HSL_DEFAULT_BLUE);
        reset3(m_purple,  HSL_DEFAULT_PURPLE);
        reset3(m_magenta, HSL_DEFAULT_MAGENTA);
    }
    // ----------------------------------------------------------------------

    static void reset3(float dst[3], const float src[3]) { dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; }

    void color_row(const char* label, const char* tooltip, float rgb[3], const char* reset_id, const float defaults[3], bool& changed) {
        changed |= ImGui::ColorEdit3(label, rgb);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
        if (ImGui::Button(reset_id)) { reset3(rgb, defaults); changed = true; }
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("HSLShift Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", HSL_VERSION);
            ImGui::TextWrapped("Remaps each of 8 hue regions to a target color. Powerful for grading skin/sky/foliage independently. Subtle shifts work better than extreme ones.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##HSL", &m_enabled);
            color_row("Red##HSL",     "Hue band for reds",     m_red,     "Reset##HSL_red",     HSL_DEFAULT_RED,     changed);
            color_row("Orange##HSL",  "Hue band for oranges",  m_orange,  "Reset##HSL_orange",  HSL_DEFAULT_ORANGE,  changed);
            color_row("Yellow##HSL",  "Hue band for yellows",  m_yellow,  "Reset##HSL_yellow",  HSL_DEFAULT_YELLOW,  changed);
            color_row("Green##HSL",   "Hue band for greens",   m_green,   "Reset##HSL_green",   HSL_DEFAULT_GREEN,   changed);
            color_row("Cyan##HSL",    "Hue band for cyans",    m_cyan,    "Reset##HSL_cyan",    HSL_DEFAULT_CYAN,    changed);
            color_row("Blue##HSL",    "Hue band for blues",    m_blue,    "Reset##HSL_blue",    HSL_DEFAULT_BLUE,    changed);
            color_row("Purple##HSL",  "Hue band for purples",  m_purple,  "Reset##HSL_purple",  HSL_DEFAULT_PURPLE,  changed);
            color_row("Magenta##HSL", "Hue band for magentas", m_magenta, "Reset##HSL_magenta", HSL_DEFAULT_MAGENTA, changed);

            ImGui::Spacing();
            if (ImGui::Button("Reset All##HSL")) {
                reset3(m_red,     HSL_DEFAULT_RED);
                reset3(m_orange,  HSL_DEFAULT_ORANGE);
                reset3(m_yellow,  HSL_DEFAULT_YELLOW);
                reset3(m_green,   HSL_DEFAULT_GREEN);
                reset3(m_cyan,    HSL_DEFAULT_CYAN);
                reset3(m_blue,    HSL_DEFAULT_BLUE);
                reset3(m_purple,  HSL_DEFAULT_PURPLE);
                reset3(m_magenta, HSL_DEFAULT_MAGENTA);
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        HSLShiftParamsCB cb{};
        for (int i = 0; i < 3; ++i) {
            cb.HUERed[i]     = m_red[i];
            cb.HUEOrange[i]  = m_orange[i];
            cb.HUEYellow[i]  = m_yellow[i];
            cb.HUEGreen[i]   = m_green[i];
            cb.HUECyan[i]    = m_cyan[i];
            cb.HUEBlue[i]    = m_blue[i];
            cb.HUEPurple[i]  = m_purple[i];
            cb.HUEMagenta[i] = m_magenta[i];
        }
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<HSLShiftPlugin> g_plugin{ new HSLShiftPlugin() };
