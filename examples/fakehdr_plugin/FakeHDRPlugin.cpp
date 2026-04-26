/*
FakeHDR Plugin for UEVR
=======================
A UEVR C++ plugin that applies a FakeHDR post-processing effect to VR frames.
Based on CeeJay.dk's FakeHDR ReShade effect.

The effect is applied to the UE4 scene render target in on_pre_render_vr_framework
(both DX11 and DX12), which fires just BEFORE UEVR copies the render target to
VR eye textures.  This ensures the effect is visible in both the VR headset and
desktop mirror.

Includes an ImGui settings panel with enable/disable, parameter sliders, reset.

v1.1.0: DX11 path now applies effect to the UE4 scene render target (via
         StereoHook API) instead of the swapchain backbuffer.  Handles typeless
         texture formats for RTV/SRV creation.
v1.0.9: UEVR core now handles resource state management (ENGINE_SRC_COLOR ↔ RENDER_TARGET
         bracketing), SEH crash protection, RT validation, and command list lifecycle.
         The plugin just records GPU commands on UEVR's command list — no crash protection
         or synchronisation code needed on the plugin side.

UEVR plugin wrapper: MIT license

Original shader:
HDR by Christian Cann Schuldt Jensen ~ CeeJay.dk
Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/FakeHDR.fx
From the crosire/reshade-shaders community collection. No explicit license
was provided in the original file or repository.
*/


#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

// Faithful port of CeeJay.dk's FakeHDR. BloomMip is fixed to 0 (full-res)
// because the SinglePassEffect runtime does not request a mip chain on the
// scene snapshot. Visually identical to the previous DX12 path which also
// used full-res bloom.
static const char* g_fakehdr_ps_src = R"(
cbuffer HDRParams : register(b0) {
    float HDRPower;
    float Radius1;
    float Radius2;
    float _pad0;
    float2 PixelSize;
    float2 _pad1;
};

Texture2D Scene : register(t0);
SamplerState LinearSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target {
    float2 tc = input.TexCoord;
    float3 color = Scene.SampleLevel(LinearSampler, tc, 0).rgb;

    float3 bloom1 = 0;
    bloom1 += Scene.SampleLevel(LinearSampler, tc + float2( 1.5, -1.5) * Radius1 * PixelSize, 0).rgb;
    bloom1 += Scene.SampleLevel(LinearSampler, tc + float2(-1.5, -1.5) * Radius1 * PixelSize, 0).rgb;
    bloom1 += Scene.SampleLevel(LinearSampler, tc + float2( 1.5,  1.5) * Radius1 * PixelSize, 0).rgb;
    bloom1 += Scene.SampleLevel(LinearSampler, tc + float2(-1.5,  1.5) * Radius1 * PixelSize, 0).rgb;
    bloom1 += Scene.SampleLevel(LinearSampler, tc + float2( 0.0, -2.5) * Radius1 * PixelSize, 0).rgb;
    bloom1 += Scene.SampleLevel(LinearSampler, tc + float2( 0.0,  2.5) * Radius1 * PixelSize, 0).rgb;
    bloom1 += Scene.SampleLevel(LinearSampler, tc + float2(-2.5,  0.0) * Radius1 * PixelSize, 0).rgb;
    bloom1 += Scene.SampleLevel(LinearSampler, tc + float2( 2.5,  0.0) * Radius1 * PixelSize, 0).rgb;
    bloom1 *= 0.005;

    float3 bloom2 = 0;
    bloom2 += Scene.SampleLevel(LinearSampler, tc + float2( 1.5, -1.5) * Radius2 * PixelSize, 0).rgb;
    bloom2 += Scene.SampleLevel(LinearSampler, tc + float2(-1.5, -1.5) * Radius2 * PixelSize, 0).rgb;
    bloom2 += Scene.SampleLevel(LinearSampler, tc + float2( 1.5,  1.5) * Radius2 * PixelSize, 0).rgb;
    bloom2 += Scene.SampleLevel(LinearSampler, tc + float2(-1.5,  1.5) * Radius2 * PixelSize, 0).rgb;
    bloom2 += Scene.SampleLevel(LinearSampler, tc + float2( 0.0, -2.5) * Radius2 * PixelSize, 0).rgb;
    bloom2 += Scene.SampleLevel(LinearSampler, tc + float2( 0.0,  2.5) * Radius2 * PixelSize, 0).rgb;
    bloom2 += Scene.SampleLevel(LinearSampler, tc + float2(-2.5,  0.0) * Radius2 * PixelSize, 0).rgb;
    bloom2 += Scene.SampleLevel(LinearSampler, tc + float2( 2.5,  0.0) * Radius2 * PixelSize, 0).rgb;
    bloom2 *= 0.010;

    float dist = Radius2 - Radius1;
    float3 HDR = (color + (bloom2 - bloom1)) * dist;
    float3 blend = HDR + color;
    color = pow(abs(blend), abs(HDRPower)) + HDR;
    return float4(saturate(color), 1.0);
}
)";

struct HDRParamsCB {
    float HDRPower;
    float Radius1;
    float Radius2;
    float _pad0;
    float PixelSize[2];
    float _pad1[2];
};
static_assert(sizeof(HDRParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* FAKEHDR_VERSION    = "1.2.0";
static constexpr float FAKEHDR_DEFAULT_POWER    = 1.30f;
static constexpr float FAKEHDR_DEFAULT_RADIUS1  = 0.793f;
static constexpr float FAKEHDR_DEFAULT_RADIUS2  = 0.87f;

class FakeHDRPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled   = false;
    float m_hdr_power = FAKEHDR_DEFAULT_POWER;
    float m_radius1   = FAKEHDR_DEFAULT_RADIUS1;
    float m_radius2   = FAKEHDR_DEFAULT_RADIUS2;

    fx::SinglePassEffect<HDRParamsCB> m_fx{ g_fakehdr_ps_src };

    void on_initialize() override {
        API::get()->log_info("[FakeHDR] Plugin initialized (v%s)", FAKEHDR_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "FakeHDR"; }
    int render_order() const override { return 600; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",   m_enabled ? "1" : "0"},
            {"hdr_power", std::to_string(m_hdr_power)},
            {"radius1",   std::to_string(m_radius1)},
            {"radius2",   std::to_string(m_radius2)},
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
        get_float("hdr_power", m_hdr_power, 0.0f, 8.0f);
        get_float("radius1",   m_radius1,   0.0f, 8.0f);
        get_float("radius2",   m_radius2,   0.0f, 8.0f);
    }

    void reset_to_defaults() override {
        m_enabled   = false;
        m_hdr_power = FAKEHDR_DEFAULT_POWER;
        m_radius1   = FAKEHDR_DEFAULT_RADIUS1;
        m_radius2   = FAKEHDR_DEFAULT_RADIUS2;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("FakeHDR Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", FAKEHDR_VERSION);
            ImGui::TextWrapped("Easiest way to make any game look good. Deepens darks and makes colors pop via local tone mapping bloom. Enhances detail without clipping.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##FakeHDR", &m_enabled);

            changed |= ImGui::DragFloat("HDR Power", &m_hdr_power, 0.01f, 0.0f, 8.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tone-mapping strength. Higher = more pronounced HDR effect");
            ImGui::SameLine();
            if (ImGui::Button("Reset##FakeHDR_power")) { m_hdr_power = FAKEHDR_DEFAULT_POWER; changed = true; }

            changed |= ImGui::DragFloat("Radius 1", &m_radius1, 0.001f, 0.0f, 8.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Inner bloom sample distance. Affects fine detail");
            ImGui::SameLine();
            if (ImGui::Button("Reset##FakeHDR_r1")) { m_radius1 = FAKEHDR_DEFAULT_RADIUS1; changed = true; }

            changed |= ImGui::DragFloat("Radius 2", &m_radius2, 0.001f, 0.0f, 8.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Outer bloom sample distance. Affects broad glow");
            ImGui::SameLine();
            if (ImGui::Button("Reset##FakeHDR_r2")) { m_radius2 = FAKEHDR_DEFAULT_RADIUS2; changed = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##FakeHDR")) {
                m_hdr_power = FAKEHDR_DEFAULT_POWER;
                m_radius1   = FAKEHDR_DEFAULT_RADIUS1;
                m_radius2   = FAKEHDR_DEFAULT_RADIUS2;
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        const unsigned w = fx::EffectRuntime::scene_width();
        const unsigned h = fx::EffectRuntime::scene_height();
        if (w == 0 || h == 0) return;
        HDRParamsCB cb{};
        cb.HDRPower     = m_hdr_power;
        cb.Radius1      = m_radius1;
        cb.Radius2      = m_radius2;
        cb.PixelSize[0] = 1.0f / (float)w;
        cb.PixelSize[1] = 1.0f / (float)h;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<FakeHDRPlugin> g_plugin{ new FakeHDRPlugin() };
