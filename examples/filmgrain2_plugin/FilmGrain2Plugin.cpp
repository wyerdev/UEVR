/*
FilmGrain2 Plugin for UEVR
===========================
A UEVR C++ plugin that applies a film grain post-processing effect to VR frames.
Based on Martins Upitis (martinsh) Film Grain v1.1 with perlin noise by toneburst.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12),
which fires BEFORE UEVR copies the render target to VR eye textures.

Includes an ImGui settings panel with enable/disable, parameter sliders, reset.

UEVR plugin wrapper: MIT license

Original shader license:
Film Grain post-process shader v1.1
Martins Upitis (martinsh) devlog-martinsh.blogspot.com 2013
Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/FilmGrain2.fx
Uses perlin noise shader by toneburst from
http://machinesdontcare.wordpress.com/2009/06/25/3d-perlin-noise-sphere-vertex-shader-sourcecode/

This work is licensed under a Creative Commons Attribution 3.0 Unported License.
So you are free to share, modify and adapt it for your needs, and even use it
for commercial use.
*/


#include <Windows.h>
#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_filmgrain2_ps_src = R"(
cbuffer GrainParams : register(b0) {
    float GrainAmount;
    float ColorAmount;
    float LumAmount;
    float GrainSize;
    float Timer;
    float2 ScreenSize;
    float AspectRatio;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 rnm(in float2 tc) {
    float noise = sin(dot(tc, float2(12.9898, 78.233))) * 43758.5453;
    float noiseR = frac(noise) * 2.0 - 1.0;
    float noiseG = frac(noise * 1.2154) * 2.0 - 1.0;
    float noiseB = frac(noise * 1.3453) * 2.0 - 1.0;
    float noiseA = frac(noise * 1.3647) * 2.0 - 1.0;
    return float4(noiseR, noiseG, noiseB, noiseA);
}

float pnoise3D(in float3 p) {
    static const float permTexUnit = 1.0 / 256.0;
    static const float permTexUnitHalf = 0.5 / 256.0;
    float3 pi = permTexUnit * floor(p) + permTexUnitHalf;
    float3 pf = frac(p);

    float perm00 = rnm(pi.xy).a;
    float3 grad000 = rnm(float2(perm00, pi.z)).rgb * 4.0 - 1.0;
    float n000 = dot(grad000, pf);
    float3 grad001 = rnm(float2(perm00, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
    float n001 = dot(grad001, pf - float3(0.0, 0.0, 1.0));

    float perm01 = rnm(pi.xy + float2(0.0, permTexUnit)).a;
    float3 grad010 = rnm(float2(perm01, pi.z)).rgb * 4.0 - 1.0;
    float n010 = dot(grad010, pf - float3(0.0, 1.0, 0.0));
    float3 grad011 = rnm(float2(perm01, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
    float n011 = dot(grad011, pf - float3(0.0, 1.0, 1.0));

    float perm10 = rnm(pi.xy + float2(permTexUnit, 0.0)).a;
    float3 grad100 = rnm(float2(perm10, pi.z)).rgb * 4.0 - 1.0;
    float n100 = dot(grad100, pf - float3(1.0, 0.0, 0.0));
    float3 grad101 = rnm(float2(perm10, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
    float n101 = dot(grad101, pf - float3(1.0, 0.0, 1.0));

    float perm11 = rnm(pi.xy + float2(permTexUnit, permTexUnit)).a;
    float3 grad110 = rnm(float2(perm11, pi.z)).rgb * 4.0 - 1.0;
    float n110 = dot(grad110, pf - float3(1.0, 1.0, 0.0));
    float3 grad111 = rnm(float2(perm11, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
    float n111 = dot(grad111, pf - float3(1.0, 1.0, 1.0));

    float fade_x = pf.x * pf.x * pf.x * (pf.x * (pf.x * 6.0 - 15.0) + 10.0);
    float4 n_x = lerp(float4(n000, n001, n010, n011), float4(n100, n101, n110, n111), fade_x);

    float fade_y = pf.y * pf.y * pf.y * (pf.y * (pf.y * 6.0 - 15.0) + 10.0);
    float2 n_xy = lerp(n_x.xy, n_x.zw, fade_y);

    float fade_z = pf.z * pf.z * pf.z * (pf.z * (pf.z * 6.0 - 15.0) + 10.0);
    return lerp(n_xy.x, n_xy.y, fade_z);
}

float2 coordRot(in float2 tc, in float angle) {
    float rotX = ((tc.x * 2.0 - 1.0) * AspectRatio * cos(angle)) - ((tc.y * 2.0 - 1.0) * sin(angle));
    float rotY = ((tc.y * 2.0 - 1.0) * cos(angle)) + ((tc.x * 2.0 - 1.0) * AspectRatio * sin(angle));
    rotX = ((rotX / AspectRatio) * 0.5 + 0.5);
    rotY = rotY * 0.5 + 0.5;
    return float2(rotX, rotY);
}

float4 main(PSInput input) : SV_Target {
    float2 texCoord = input.TexCoord;
    float3 rotOffset = float3(1.425, 3.892, 5.835);

    float2 rotCoordsR = coordRot(texCoord, Timer + rotOffset.x);
    float3 noise = pnoise3D(float3(rotCoordsR * ScreenSize / GrainSize, 0.0)).xxx;

    if (ColorAmount > 0) {
        float2 rotCoordsG = coordRot(texCoord, Timer + rotOffset.y);
        float2 rotCoordsB = coordRot(texCoord, Timer + rotOffset.z);
        noise.g = lerp(noise.r, pnoise3D(float3(rotCoordsG * ScreenSize / GrainSize, 1.0)), ColorAmount);
        noise.b = lerp(noise.r, pnoise3D(float3(rotCoordsB * ScreenSize / GrainSize, 2.0)), ColorAmount);
    }

    float3 col = Scene.Sample(PointSampler, texCoord).rgb;
    const float3 lumcoeff = float3(0.299, 0.587, 0.114);
    float luminance = lerp(0.0, dot(col, lumcoeff), LumAmount);
    float lum = smoothstep(0.2, 0.0, luminance);
    lum += luminance;

    noise = lerp(noise, 0.0, pow(lum, 4.0));
    col = col + noise * GrainAmount;
    return float4(saturate(col), 1.0);
}
)";

struct GrainParamsCB {
    float GrainAmount;
    float ColorAmount;
    float LumAmount;
    float GrainSize;
    float Timer;
    float ScreenSizeX;
    float ScreenSizeY;
    float AspectRatio;
};
static_assert(sizeof(GrainParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* FILMGRAIN2_VERSION = "1.1.0";
static constexpr float DEFAULT_GRAIN_AMOUNT = 0.05f;
static constexpr float DEFAULT_COLOR_AMOUNT = 0.6f;
static constexpr float DEFAULT_LUM_AMOUNT   = 1.0f;
static constexpr float DEFAULT_GRAIN_SIZE   = 1.6f;

class FilmGrain2Plugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled      = false;
    float m_grain_amount = DEFAULT_GRAIN_AMOUNT;
    float m_color_amount = DEFAULT_COLOR_AMOUNT;
    float m_lum_amount   = DEFAULT_LUM_AMOUNT;
    float m_grain_size   = DEFAULT_GRAIN_SIZE;

    fx::SinglePassEffect<GrainParamsCB> m_fx{ g_filmgrain2_ps_src };

    void on_initialize() override {
        API::get()->log_info("[FilmGrain2] Plugin initialized (v%s)", FILMGRAIN2_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "FilmGrain2"; }
    int render_order() const override { return 1100; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",      m_enabled ? "1" : "0"},
            {"grain_amount", std::to_string(m_grain_amount)},
            {"color_amount", std::to_string(m_color_amount)},
            {"lum_amount",   std::to_string(m_lum_amount)},
            {"grain_size",   std::to_string(m_grain_size)},
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
        get_float("grain_amount", m_grain_amount, 0.0f, 0.2f);
        get_float("color_amount", m_color_amount, 0.0f, 1.0f);
        get_float("lum_amount",   m_lum_amount,   0.0f, 1.0f);
        get_float("grain_size",   m_grain_size,   1.5f, 2.5f);
    }

    void reset_to_defaults() override {
        m_enabled      = false;
        m_grain_amount = DEFAULT_GRAIN_AMOUNT;
        m_color_amount = DEFAULT_COLOR_AMOUNT;
        m_lum_amount   = DEFAULT_LUM_AMOUNT;
        m_grain_size   = DEFAULT_GRAIN_SIZE;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("FilmGrain2 Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", FILMGRAIN2_VERSION);
            ImGui::TextWrapped("Animated film grain via 3D Perlin noise. Subtle adds realism, heavy looks like compression noise. Pair with FilmicPass for retro feel.");
            bool ch = false;
            ch |= ImGui::Checkbox("Enabled##FG2", &m_enabled);

            ch |= ImGui::SliderFloat("Grain Amount", &m_grain_amount, 0.0f, 0.2f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overall grain intensity");
            ImGui::SameLine(); if (ImGui::Button("Reset##FG2_amt"))   { m_grain_amount = DEFAULT_GRAIN_AMOUNT; ch = true; }

            ch |= ImGui::SliderFloat("Color Amount", &m_color_amount, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = monochrome grain, 1 = fully chromatic");
            ImGui::SameLine(); if (ImGui::Button("Reset##FG2_col"))   { m_color_amount = DEFAULT_COLOR_AMOUNT; ch = true; }

            ch |= ImGui::SliderFloat("Luma Amount",  &m_lum_amount,   0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much grain follows scene luminance (less in shadows)");
            ImGui::SameLine(); if (ImGui::Button("Reset##FG2_lum"))   { m_lum_amount = DEFAULT_LUM_AMOUNT; ch = true; }

            ch |= ImGui::SliderFloat("Grain Size",   &m_grain_size,   1.5f, 2.5f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Larger = chunkier grain");
            ImGui::SameLine(); if (ImGui::Button("Reset##FG2_size"))  { m_grain_size = DEFAULT_GRAIN_SIZE; ch = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##FG2")) {
                m_grain_amount = DEFAULT_GRAIN_AMOUNT;
                m_color_amount = DEFAULT_COLOR_AMOUNT;
                m_lum_amount   = DEFAULT_LUM_AMOUNT;
                m_grain_size   = DEFAULT_GRAIN_SIZE;
                ch = true;
            }
            if (ch) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        const auto w = fx::EffectRuntime::scene_width();
        const auto h = fx::EffectRuntime::scene_height();
        if (w == 0 || h == 0) return;

        GrainParamsCB cb{};
        cb.GrainAmount = m_grain_amount;
        cb.ColorAmount = m_color_amount;
        cb.LumAmount   = m_lum_amount;
        cb.GrainSize   = m_grain_size;
        cb.Timer       = (float)(GetTickCount64() % 1000000ULL);
        cb.ScreenSizeX = (float)w;
        cb.ScreenSizeY = (float)h;
        cb.AspectRatio = (float)w / (float)h;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<FilmGrain2Plugin> g_plugin{ new FilmGrain2Plugin() };
