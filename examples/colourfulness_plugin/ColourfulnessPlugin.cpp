/*
Colourfulness Plugin for UEVR
==============================
A UEVR C++ plugin that applies bacondither's Colourfulness effect to VR frames.
Attempt to increase or decrease colourfulness/saturation in a perceptually
uniform way, using a weighted power mean to soft-limit saturation changes
and prevent clipping.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license

Original shader license (BSD 2-Clause):
Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/Colourfulness.fx

Copyright (c) 2016-2018, bacondither
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer
   in this position and unchanged.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

// Faithful port of bacondither's Colourfulness v2018-11-12 (fast_luma path)
static const char* g_colourfulness_ps_src = R"(
cbuffer ColourParams : register(b0) {
    float Colourfulness;
    float LimLuma;
    int   EnableDither;
    int   ColNoise;
    float BackbufferBits;
    float3 _pad0;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float3 soft_lim(float3 v, float3 s) { return (v * s) * rsqrt(s * s + v * v); }
float3 wpmean(float3 a, float3 b, float w) {
    float3 sa = sqrt(abs(a));
    float3 sb = sqrt(abs(b));
    return pow(abs(w) * sa + abs(1.0 - w) * sb, 2.0);
}
static const float3 lumacoeff = float3(0.2558, 0.6511, 0.0931);

float4 main(PSInput input) : SV_Target {
    float4 vpos = input.Position;
    float3 c0 = Scene.Sample(PointSampler, input.TexCoord).rgb;
    float luma = sqrt(dot(saturate(c0 * abs(c0)), lumacoeff));
    c0 = saturate(c0);
    float3 diff_luma = c0 - luma;
    float3 c_diff = diff_luma * (Colourfulness + 1.0) - diff_luma;
    if (Colourfulness > 0.0) {
        float3 rlc_diff = clamp((c_diff * 1.2) + c0, -0.0001, 1.0001) - c0;
        float maxC = abs(max(diff_luma.r, max(diff_luma.g, diff_luma.b)));
        float minC = abs(min(diff_luma.r, min(diff_luma.g, diff_luma.b)));
        float poslim = (1.0002 - luma) / (maxC + 0.0001);
        float neglim = (luma + 0.0002) / (minC + 0.0001);
        float3 diffmax = diff_luma * min(min(poslim, neglim), 32.0) - diff_luma;
        c_diff = soft_lim(c_diff, max(wpmean(diffmax, rlc_diff, LimLuma), 1e-7));
    }
    if (EnableDither) {
        const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
        float xy_magic = vpos.x * magic.x + vpos.y * magic.y;
        float noise = (frac(magic.z * frac(xy_magic)) - 0.5) / (exp2(BackbufferBits) - 1.0);
        c_diff += ColNoise ? float3(-noise, noise, -noise) : noise;
    }
    return float4(saturate(c0 + c_diff), 1.0);
}
)";

struct ColourParamsCB {
    float Colourfulness;
    float LimLuma;
    int   EnableDither;
    int   ColNoise;
    float BackbufferBits;
    float _pad0[3];
};
static_assert(sizeof(ColourParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* COLOURFULNESS_VERSION = "1.1.0";
static constexpr float DEFAULT_COLOURFULNESS = 0.4f;
static constexpr float DEFAULT_LIM_LUMA      = 0.7f;
static constexpr float DEFAULT_BB_BITS       = 10.0f;
static constexpr bool  DEFAULT_DITHER        = false;
static constexpr bool  DEFAULT_COLNOISE      = true;

class ColourfulnessPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled       = false;
    float m_colourfulness = DEFAULT_COLOURFULNESS;
    float m_lim_luma      = DEFAULT_LIM_LUMA;
    bool  m_enable_dither = DEFAULT_DITHER;
    bool  m_col_noise     = DEFAULT_COLNOISE;
    float m_bb_bits       = DEFAULT_BB_BITS;

    fx::SinglePassEffect<ColourParamsCB> m_fx{ g_colourfulness_ps_src };

    void on_initialize() override {
        API::get()->log_info("[Colourfulness] Plugin initialized (v%s)", COLOURFULNESS_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "Colourfulness"; }
    int render_order() const override { return 1000; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",        m_enabled ? "1" : "0"},
            {"colourfulness",  std::to_string(m_colourfulness)},
            {"lim_luma",       std::to_string(m_lim_luma)},
            {"enable_dither",  m_enable_dither ? "1" : "0"},
            {"col_noise",      m_col_noise ? "1" : "0"},
            {"bb_bits",        std::to_string(m_bb_bits)},
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
        auto get_bool = [&](const char* k, bool& out) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            out = (it->second != "0" && !it->second.empty());
        };
        get_bool("enabled",       m_enabled);
        get_float("colourfulness", m_colourfulness, -1.0f, 2.0f);
        get_float("lim_luma",      m_lim_luma,       0.1f, 1.0f);
        get_bool("enable_dither", m_enable_dither);
        get_bool("col_noise",     m_col_noise);
        get_float("bb_bits",       m_bb_bits,        1.0f, 32.0f);
    }

    void reset_to_defaults() override {
        m_enabled       = false;
        m_colourfulness = DEFAULT_COLOURFULNESS;
        m_lim_luma      = DEFAULT_LIM_LUMA;
        m_enable_dither = DEFAULT_DITHER;
        m_col_noise     = DEFAULT_COLNOISE;
        m_bb_bits       = DEFAULT_BB_BITS;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Colourfulness Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", COLOURFULNESS_VERSION);
            ImGui::TextWrapped("Smarter saturation. Boosts color where there's room without clipping highlights or shadows. Less prone to neon shifts than Vibrance. Pushing past 1.0 may oversaturate skin tones.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##COL", &m_enabled);

            changed |= ImGui::SliderFloat("Colourfulness", &m_colourfulness, -1.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Negative = desaturate, 0 = neutral, positive = boost");
            ImGui::SameLine();
            if (ImGui::Button("Reset##COL_amt")) { m_colourfulness = DEFAULT_COLOURFULNESS; changed = true; }

            changed |= ImGui::SliderFloat("Luma Limit", &m_lim_luma, 0.1f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher = more aggressive clipping near luma extremes");
            ImGui::SameLine();
            if (ImGui::Button("Reset##COL_lim")) { m_lim_luma = DEFAULT_LIM_LUMA; changed = true; }

            changed |= ImGui::Checkbox("Enable Dithering", &m_enable_dither);
            ImGui::SameLine();
            if (ImGui::Button("Reset##COL_dith")) { m_enable_dither = DEFAULT_DITHER; changed = true; }

            if (m_enable_dither) {
                changed |= ImGui::Checkbox("Coloured Noise", &m_col_noise);
                ImGui::SameLine();
                if (ImGui::Button("Reset##COL_noise")) { m_col_noise = DEFAULT_COLNOISE; changed = true; }

                changed |= ImGui::SliderFloat("Backbuffer Bits", &m_bb_bits, 1.0f, 16.0f, "%.0f");
                ImGui::SameLine();
                if (ImGui::Button("Reset##COL_bits")) { m_bb_bits = DEFAULT_BB_BITS; changed = true; }
            }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##COL")) {
                m_colourfulness = DEFAULT_COLOURFULNESS;
                m_lim_luma      = DEFAULT_LIM_LUMA;
                m_enable_dither = DEFAULT_DITHER;
                m_col_noise     = DEFAULT_COLNOISE;
                m_bb_bits       = DEFAULT_BB_BITS;
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        ColourParamsCB cb{};
        cb.Colourfulness  = m_colourfulness;
        cb.LimLuma        = m_lim_luma;
        cb.EnableDither   = m_enable_dither ? 1 : 0;
        cb.ColNoise       = m_col_noise ? 1 : 0;
        cb.BackbufferBits = m_bb_bits;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<ColourfulnessPlugin> g_plugin{ new ColourfulnessPlugin() };
