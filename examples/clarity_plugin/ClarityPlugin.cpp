/*
Clarity Plugin for UEVR
========================
A UEVR C++ plugin that applies Ioxa's Clarity effect to VR frames.
Local contrast enhancement via unsharp mask on luminance, with multiple
blend modes (Soft Light, Overlay, Hard Light, Multiply, Vivid Light,
Linear Light, Addition).

The original shader uses multi-pass separable Gaussian blur at half/quarter
resolution. This port implements a single-pass 9-tap Gaussian approximation on
luma for real-time VR performance, producing visually equivalent results.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Original shader:
  Clarity v1.5 by Ioxa
  Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/Clarity.fx
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

static const char* g_clarity_ps_src = R"(
cbuffer ClarityParams : register(b0) {
    int   ClarityRadius;
    float ClarityOffset;
    int   ClarityBlendMode;
    float ClarityStrength;
    float ClarityDarkIntensity;
    float ClarityLightIntensity;
    int   ClarityBlendIfDark;
    int   ClarityBlendIfLight;
    float2 PixelSize;
    float2 _pad0;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

static const float3 LumaCoeff = float3(0.32786885, 0.655737705, 0.0163934436);

float GaussianBlurLuma(float2 uv, float2 dir) {
    float center = dot(Scene.Sample(PointSampler, uv).rgb, LumaCoeff);
    float scale = ClarityOffset;
    float offsets[4] = { 1.4118, 3.2941, 5.1765, 7.0 };
    float weights[4] = { 0.2270, 0.1945, 0.1216, 0.0541 };
    float centerWeight = 0.2042;
    float radiusScale = 1.0 + ClarityRadius * 1.5;
    float result = center * centerWeight;
    float totalWeight = centerWeight;
    [unroll] for (int i = 0; i < 4; i++) {
        float2 offset = dir * offsets[i] * scale * radiusScale * PixelSize;
        float s0 = dot(Scene.Sample(PointSampler, uv + offset).rgb, LumaCoeff);
        float s1 = dot(Scene.Sample(PointSampler, uv - offset).rgb, LumaCoeff);
        result += (s0 + s1) * weights[i];
        totalWeight += 2.0 * weights[i];
    }
    return result / totalWeight;
}

float4 main(PSInput input) : SV_Target {
    float3 orig = Scene.Sample(PointSampler, input.TexCoord).rgb;
    float luma = dot(orig, LumaCoeff);
    float3 chroma = orig / max(luma, 0.001);

    float blurH = GaussianBlurLuma(input.TexCoord, float2(1.0, 0.0));
    float blurV = GaussianBlurLuma(input.TexCoord, float2(0.0, 1.0));
    float blurred = (blurH + blurV) * 0.5;

    float sharp = 1.0 - blurred;
    sharp = (luma + sharp) * 0.5;

    float sharpMin = lerp(0.0, 1.0, smoothstep(0.0, 1.0, sharp));
    float sharpMax = sharpMin;
    sharpMin = lerp(sharp, sharpMin, ClarityDarkIntensity);
    sharpMax = lerp(sharp, sharpMax, ClarityLightIntensity);
    sharp = lerp(sharpMin, sharpMax, step(0.5, sharp));

    float blended = sharp;
    if (ClarityBlendMode == 0) {
        blended = lerp(2*luma*sharp + luma*luma*(1.0-2*sharp),
                       2*luma*(1.0-sharp)+pow(luma,0.5)*(2*sharp-1.0), step(0.49,sharp));
    } else if (ClarityBlendMode == 1) {
        blended = lerp(2*luma*sharp, 1.0 - 2*(1.0-luma)*(1.0-sharp), step(0.50,luma));
    } else if (ClarityBlendMode == 2) {
        blended = lerp(2*luma*sharp, 1.0 - 2*(1.0-luma)*(1.0-sharp), step(0.50,sharp));
    } else if (ClarityBlendMode == 3) {
        blended = saturate(2 * luma * sharp);
    } else if (ClarityBlendMode == 4) {
        blended = lerp(2*luma*sharp, luma/(2*(1-sharp)+0.001), step(0.5,sharp));
    } else if (ClarityBlendMode == 5) {
        blended = luma + 2.0*sharp - 1.0;
    } else {
        blended = saturate(luma + (sharp - 0.5));
    }

    if (ClarityBlendIfDark > 0 || ClarityBlendIfLight < 255) {
        float blendIfD = (ClarityBlendIfDark / 255.0) + 0.0001;
        float blendIfL = (ClarityBlendIfLight / 255.0) - 0.0001;
        float mix = dot(orig, 0.333333);
        float mask = 1.0;
        if (ClarityBlendIfDark > 0)
            mask = lerp(0.0, 1.0, smoothstep(blendIfD - blendIfD*0.2, blendIfD + blendIfD*0.2, mix));
        if (ClarityBlendIfLight < 255)
            mask = lerp(mask, 0.0, smoothstep(blendIfL - blendIfL*0.2, blendIfL + blendIfL*0.2, mix));
        blended = lerp(luma, blended, mask);
    }

    float finalLuma = lerp(luma, blended, ClarityStrength);
    float3 result = finalLuma * chroma;
    return float4(saturate(result), 1.0);
}
)";

struct ClarityParamsCB {
    int   ClarityRadius;
    float ClarityOffset;
    int   ClarityBlendMode;
    float ClarityStrength;
    float ClarityDarkIntensity;
    float ClarityLightIntensity;
    int   ClarityBlendIfDark;
    int   ClarityBlendIfLight;
    float PixelSize[2];
    float _pad0[2];
};
static_assert(sizeof(ClarityParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* CL_VERSION = "1.1.0";
static constexpr int   CL_DEFAULT_RADIUS         = 3;
static constexpr float CL_DEFAULT_OFFSET         = 2.0f;
static constexpr int   CL_DEFAULT_BLEND_MODE     = 2; // Hard Light
static constexpr float CL_DEFAULT_STRENGTH       = 0.4f;
static constexpr float CL_DEFAULT_DARK_INT       = 0.4f;
static constexpr float CL_DEFAULT_LIGHT_INT      = 0.0f;
static constexpr int   CL_DEFAULT_BLEND_IF_DARK  = 50;
static constexpr int   CL_DEFAULT_BLEND_IF_LIGHT = 205;

static const char* g_clarity_blend_names[] = {
    "Soft Light", "Overlay", "Hard Light", "Multiply", "Vivid Light", "Linear Light", "Addition"
};

class ClarityPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled         = false;
    int   m_radius          = CL_DEFAULT_RADIUS;
    float m_offset          = CL_DEFAULT_OFFSET;
    int   m_blend_mode      = CL_DEFAULT_BLEND_MODE;
    float m_strength        = CL_DEFAULT_STRENGTH;
    float m_dark_intensity  = CL_DEFAULT_DARK_INT;
    float m_light_intensity = CL_DEFAULT_LIGHT_INT;
    int   m_blend_if_dark   = CL_DEFAULT_BLEND_IF_DARK;
    int   m_blend_if_light  = CL_DEFAULT_BLEND_IF_LIGHT;

    fx::SinglePassEffect<ClarityParamsCB> m_fx{ g_clarity_ps_src };

    void on_initialize() override {
        API::get()->log_info("[Clarity] Plugin initialized (v%s)", CL_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "Clarity"; }
    int render_order() const override { return 1600; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",          m_enabled ? "1" : "0"},
            {"radius",           std::to_string(m_radius)},
            {"offset",           std::to_string(m_offset)},
            {"blend_mode",       std::to_string(m_blend_mode)},
            {"strength",         std::to_string(m_strength)},
            {"dark_intensity",   std::to_string(m_dark_intensity)},
            {"light_intensity",  std::to_string(m_light_intensity)},
            {"blend_if_dark",    std::to_string(m_blend_if_dark)},
            {"blend_if_light",   std::to_string(m_blend_if_light)},
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
        auto get_int = [&](const char* k, int& out, int lo, int hi) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try {
                int v = std::stoi(it->second);
                if (v < lo) v = lo;
                if (v > hi) v = hi;
                out = v;
            } catch (...) {}
        };
        auto it = kv.find("enabled");
        if (it != kv.end()) m_enabled = (it->second != "0" && !it->second.empty());
        get_int  ("radius",          m_radius,          0, 4);
        get_float("offset",          m_offset,          1.0f, 5.0f);
        get_int  ("blend_mode",      m_blend_mode,      0, 6);
        get_float("strength",        m_strength,        0.0f, 1.0f);
        get_float("dark_intensity",  m_dark_intensity,  0.0f, 1.0f);
        get_float("light_intensity", m_light_intensity, 0.0f, 1.0f);
        get_int  ("blend_if_dark",   m_blend_if_dark,   0, 255);
        get_int  ("blend_if_light",  m_blend_if_light,  0, 255);
    }

    void reset_to_defaults() override {
        m_enabled         = false;
        m_radius          = CL_DEFAULT_RADIUS;
        m_offset          = CL_DEFAULT_OFFSET;
        m_blend_mode      = CL_DEFAULT_BLEND_MODE;
        m_strength        = CL_DEFAULT_STRENGTH;
        m_dark_intensity  = CL_DEFAULT_DARK_INT;
        m_light_intensity = CL_DEFAULT_LIGHT_INT;
        m_blend_if_dark   = CL_DEFAULT_BLEND_IF_DARK;
        m_blend_if_light  = CL_DEFAULT_BLEND_IF_LIGHT;
    }
    // ----------------------------------------------------------------------

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Clarity Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", CL_VERSION);
            ImGui::TextWrapped("Local-contrast pop. Bigger blur radius = broader 'haze removal' feel; smaller = pure micro-contrast. Push too far and edges halo. Subtle wins.");
            bool ch = false;
            ch |= ImGui::Checkbox("Enabled##CLA", &m_enabled);

            ch |= ImGui::SliderInt("Radius",  &m_radius, 0, 4);
            ImGui::SameLine(); if (ImGui::Button("Reset##CLA_radius"))   { m_radius   = CL_DEFAULT_RADIUS;   ch = true; }

            ch |= ImGui::SliderFloat("Offset", &m_offset, 1.0f, 5.0f, "%.2f");
            ImGui::SameLine(); if (ImGui::Button("Reset##CLA_offset"))   { m_offset   = CL_DEFAULT_OFFSET;   ch = true; }

            if (ImGui::Combo("Blend Mode", &m_blend_mode, g_clarity_blend_names, IM_ARRAYSIZE(g_clarity_blend_names))) ch = true;
            ImGui::SameLine(); if (ImGui::Button("Reset##CLA_blend"))    { m_blend_mode = CL_DEFAULT_BLEND_MODE; ch = true; }

            ch |= ImGui::SliderFloat("Strength",        &m_strength,        0.0f, 1.0f, "%.2f");
            ImGui::SameLine(); if (ImGui::Button("Reset##CLA_strength")) { m_strength = CL_DEFAULT_STRENGTH; ch = true; }

            ch |= ImGui::SliderFloat("Dark Intensity",  &m_dark_intensity,  0.0f, 1.0f, "%.2f");
            ImGui::SameLine(); if (ImGui::Button("Reset##CLA_dark"))     { m_dark_intensity  = CL_DEFAULT_DARK_INT;  ch = true; }

            ch |= ImGui::SliderFloat("Light Intensity", &m_light_intensity, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine(); if (ImGui::Button("Reset##CLA_light"))    { m_light_intensity = CL_DEFAULT_LIGHT_INT; ch = true; }

            ch |= ImGui::SliderInt("Blend If Dark",  &m_blend_if_dark,  0, 255);
            ImGui::SameLine(); if (ImGui::Button("Reset##CLA_bid"))      { m_blend_if_dark  = CL_DEFAULT_BLEND_IF_DARK;  ch = true; }

            ch |= ImGui::SliderInt("Blend If Light", &m_blend_if_light, 0, 255);
            ImGui::SameLine(); if (ImGui::Button("Reset##CLA_bil"))      { m_blend_if_light = CL_DEFAULT_BLEND_IF_LIGHT; ch = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##CLA")) {
                m_radius          = CL_DEFAULT_RADIUS;
                m_offset          = CL_DEFAULT_OFFSET;
                m_blend_mode      = CL_DEFAULT_BLEND_MODE;
                m_strength        = CL_DEFAULT_STRENGTH;
                m_dark_intensity  = CL_DEFAULT_DARK_INT;
                m_light_intensity = CL_DEFAULT_LIGHT_INT;
                m_blend_if_dark   = CL_DEFAULT_BLEND_IF_DARK;
                m_blend_if_light  = CL_DEFAULT_BLEND_IF_LIGHT;
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

        ClarityParamsCB cb{};
        cb.ClarityRadius         = m_radius;
        cb.ClarityOffset         = m_offset;
        cb.ClarityBlendMode      = m_blend_mode;
        cb.ClarityStrength       = m_strength;
        cb.ClarityDarkIntensity  = m_dark_intensity;
        cb.ClarityLightIntensity = m_light_intensity;
        cb.ClarityBlendIfDark    = m_blend_if_dark;
        cb.ClarityBlendIfLight   = m_blend_if_light;
        cb.PixelSize[0]          = 1.0f / (float)w;
        cb.PixelSize[1]          = 1.0f / (float)h;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<ClarityPlugin> g_plugin{ new ClarityPlugin() };
