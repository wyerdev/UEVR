/*
Technicolor Plugin for UEVR
============================
A UEVR C++ plugin that applies DKT70's Technicolor effect (optimized by CeeJay.dk).
Simulates a two-strip technicolor process using cyan/magenta/yellow filters.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12),
which fires BEFORE UEVR copies the render target to VR eye textures.

UEVR plugin wrapper: MIT license

Original shader:
Technicolor version 1.1
Original by DKT70
Optimized by CeeJay.dk
Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/Technicolor.fx
From the crosire/reshade-shaders community collection. No explicit license
was provided in the original file or repository.
*/


#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

// Faithful port of DKT70/CeeJay.dk's Technicolor.fx.
// CB carries InvNegPower = 1 / (RGBNegativeAmount * Power) precomputed CPU-side
// to avoid the per-pixel division.
static const char* g_tech_ps_src = R"(
cbuffer TechnicolorParams : register(b0) {
    float3 InvNegPower;
    float  Strength;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target {
    float3 tcol = Scene.Sample(PointSampler, input.TexCoord).rgb;

    static const float3 cyanfilter      = float3(0.0, 1.30, 1.0);
    static const float3 magentafilter   = float3(1.0, 0.0, 1.05);
    static const float3 yellowfilter    = float3(1.6, 1.6, 0.05);
    static const float2 redorangefilter = float2(1.05, 0.620);
    static const float2 greenfilter     = float2(0.30, 1.0);
    float2 magentafilter2 = magentafilter.rb;

    float2 negative_mul_r = tcol.rg * InvNegPower.r;
    float2 negative_mul_g = tcol.rg * InvNegPower.g;
    float2 negative_mul_b = tcol.rb * InvNegPower.b;

    float3 output_r = dot(redorangefilter, negative_mul_r).xxx + cyanfilter;
    float3 output_g = dot(greenfilter,     negative_mul_g).xxx + magentafilter;
    float3 output_b = dot(magentafilter2,  negative_mul_b).xxx + yellowfilter;

    float3 result = lerp(tcol, output_r * output_g * output_b, Strength);
    return float4(result, 1.0);
}
)";

struct TechnicolorCB {
    float InvNegPower[3];
    float Strength;
};
static_assert(sizeof(TechnicolorCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* TECH_VERSION = "1.1.0";
static constexpr float TECH_DEFAULT_POWER    = 4.0f;
static constexpr float TECH_DEFAULT_NEG_R    = 0.88f;
static constexpr float TECH_DEFAULT_NEG_G    = 0.88f;
static constexpr float TECH_DEFAULT_NEG_B    = 0.88f;
static constexpr float TECH_DEFAULT_STRENGTH = 0.4f;

class TechnicolorPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled  = false;
    float m_power    = TECH_DEFAULT_POWER;
    float m_neg_r    = TECH_DEFAULT_NEG_R;
    float m_neg_g    = TECH_DEFAULT_NEG_G;
    float m_neg_b    = TECH_DEFAULT_NEG_B;
    float m_strength = TECH_DEFAULT_STRENGTH;

    fx::SinglePassEffect<TechnicolorCB> m_fx{ g_tech_ps_src };

    void on_initialize() override {
        API::get()->log_info("[Technicolor] Plugin initialized (v%s)", TECH_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "Technicolor"; }
    int render_order() const override { return 900; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",  m_enabled ? "1" : "0"},
            {"power",    std::to_string(m_power)},
            {"neg_r",    std::to_string(m_neg_r)},
            {"neg_g",    std::to_string(m_neg_g)},
            {"neg_b",    std::to_string(m_neg_b)},
            {"strength", std::to_string(m_strength)},
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
        get_float("power",    m_power,    0.0f, 8.0f);
        get_float("neg_r",    m_neg_r,    0.0f, 1.0f);
        get_float("neg_g",    m_neg_g,    0.0f, 1.0f);
        get_float("neg_b",    m_neg_b,    0.0f, 1.0f);
        get_float("strength", m_strength, 0.0f, 1.0f);
    }

    void reset_to_defaults() override {
        m_enabled  = false;
        m_power    = TECH_DEFAULT_POWER;
        m_neg_r    = TECH_DEFAULT_NEG_R;
        m_neg_g    = TECH_DEFAULT_NEG_G;
        m_neg_b    = TECH_DEFAULT_NEG_B;
        m_strength = TECH_DEFAULT_STRENGTH;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Technicolor Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", TECH_VERSION);
            ImGui::TextWrapped("Emulates 2-strip Technicolor (old Hollywood look). Strong color shift — teal shadows, warm highlights. Use sparingly.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##tech", &m_enabled);

            changed |= ImGui::DragFloat("Power", &m_power, 0.1f, 0.0f, 8.0f, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overall intensity of the Technicolor effect");
            ImGui::SameLine();
            if (ImGui::Button("Reset##tech_power")) { m_power = TECH_DEFAULT_POWER; changed = true; }

            changed |= ImGui::DragFloat("Negative R", &m_neg_r, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Red channel film negative. Higher = less red in dark areas");
            ImGui::SameLine();
            if (ImGui::Button("Reset##tech_negr")) { m_neg_r = TECH_DEFAULT_NEG_R; changed = true; }

            changed |= ImGui::DragFloat("Negative G", &m_neg_g, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Green channel film negative. Higher = less green in dark areas");
            ImGui::SameLine();
            if (ImGui::Button("Reset##tech_negg")) { m_neg_g = TECH_DEFAULT_NEG_G; changed = true; }

            changed |= ImGui::DragFloat("Negative B", &m_neg_b, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blue channel film negative. Higher = less blue in dark areas");
            ImGui::SameLine();
            if (ImGui::Button("Reset##tech_negb")) { m_neg_b = TECH_DEFAULT_NEG_B; changed = true; }

            changed |= ImGui::DragFloat("Strength", &m_strength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blend between original (0) and full Technicolor (1)");
            ImGui::SameLine();
            if (ImGui::Button("Reset##tech_strength")) { m_strength = TECH_DEFAULT_STRENGTH; changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##tech")) {
                m_power    = TECH_DEFAULT_POWER;
                m_neg_r    = TECH_DEFAULT_NEG_R;
                m_neg_g    = TECH_DEFAULT_NEG_G;
                m_neg_b    = TECH_DEFAULT_NEG_B;
                m_strength = TECH_DEFAULT_STRENGTH;
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        auto safe_inv = [](float neg, float pwr) {
            float d = neg * pwr;
            return d > 1e-4f ? 1.0f / d : 0.0f;
        };
        TechnicolorCB cb{};
        cb.InvNegPower[0] = safe_inv(m_neg_r, m_power);
        cb.InvNegPower[1] = safe_inv(m_neg_g, m_power);
        cb.InvNegPower[2] = safe_inv(m_neg_b, m_power);
        cb.Strength       = m_strength;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<TechnicolorPlugin> g_plugin{ new TechnicolorPlugin() };
