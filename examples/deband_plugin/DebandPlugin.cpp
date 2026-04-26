/*
Deband Plugin for UEVR
======================
Port of Deband shader to UEVR's fullscreen-triangle pipeline.
Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

Based on the ReShade port by JPulowski of haasn's deband shader:
  https://github.com/crosire/reshade-shaders/blob/slim/Shaders/Deband.fx

Original deband algorithm:
  Copyright (c) 2015 Niklas Haas
  https://github.com/haasn/gentoo-conf/blob/xor/home/nand/.mpv/shaders/deband-pre.glsl
  License: MIT

Ordered dithering algorithm by CeeJay.dk.

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)
*/


#include <algorithm>
#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_deband_ps_src = R"(
cbuffer DebandParams : register(b0) {
    float SDThreshold;
    float WeberThreshold;
    float Radius;
    int   Iterations;
    float RandomSeed;
    int   EnableWeber;
    int   EnableStdDev;
    int   DebugOutput;
    float2 PixelSize;
    float2 ScreenSize;
};

Texture2D Scene : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float rand(float x) { return frac(x / 41.0); }
float permute(float x) { return ((34.0 * x + 1.0) * x) % 289.0; }

float4 main(PSInput input) : SV_Target {
    float2 uv = input.TexCoord;
    float3 ori = Scene.Sample(PointSampler, uv).rgb;

    float3 m = float3(uv + 1.0, RandomSeed + 1.0);
    float h = permute(permute(permute(m.x) + m.y) + m.z);

    float dir = rand(permute(h)) * 6.2831853;
    float2 o; sincos(dir, o.y, o.x);

    float2 pt = 0;
    float dist;
    for (int i = 1; i <= 4; i++) {
        if (i > Iterations) break;
        dist = rand(h) * Radius * (float)i;
        pt = dist * PixelSize;
        h = permute(h);
    }

    float3 ref0 = Scene.Sample(PointSampler, uv + pt * o).rgb;
    float3 ref1 = Scene.Sample(PointSampler, uv - pt * o).rgb;
    float3 ref2 = Scene.Sample(PointSampler, uv + pt * float2(-o.y,  o.x)).rgb;
    float3 ref3 = Scene.Sample(PointSampler, uv + pt * float2( o.y, -o.x)).rgb;

    float3 mean = (ori + ref0 + ref1 + ref2 + ref3) * 0.2;
    float3 k = abs(ori - mean);
    k += abs(ref0 - mean); k += abs(ref1 - mean);
    k += abs(ref2 - mean); k += abs(ref3 - mean);
    k = k * 0.2 / mean;

    float3 sd = pow(ref0 - ori, 2) + pow(ref1 - ori, 2) + pow(ref2 - ori, 2) + pow(ref3 - ori, 2);
    sd = sqrt(sd * 0.25);

    float3 output;
    if (DebugOutput == 2)
        output = float3(0.0, 1.0, 0.0);
    else
        output = (ref0 + ref1 + ref2 + ref3) * 0.25;

    bool3 banding_map = (bool3)true;
    if (DebugOutput != 1) {
        if (EnableWeber)
            banding_map = banding_map && (k <= WeberThreshold * (float)Iterations);
        if (EnableStdDev)
            banding_map = banding_map && (sd <= SDThreshold * (float)Iterations);
    }

    float grid_position = frac(dot(uv, ScreenSize * float2(1.0 / 16.0, 10.0 / 36.0) + 0.25));
    float dither_shift = 0.25 * (1.0 / 255.0);
    float3 dither_shift_RGB = float3(dither_shift, -dither_shift, dither_shift);
    dither_shift_RGB = lerp(2.0 * dither_shift_RGB, -2.0 * dither_shift_RGB, grid_position);

    float3 result = banding_map ? output + dither_shift_RGB : ori;
    return float4(result, 1.0);
}
)";

struct DebandParamsCB {
    float SDThreshold;
    float WeberThreshold;
    float Radius;
    int   Iterations;
    float RandomSeed;
    int   EnableWeber;
    int   EnableStdDev;
    int   DebugOutput;
    float PixelSize[2];
    float ScreenSize[2];
};
static_assert(sizeof(DebandParamsCB) % 16 == 0, "CB must be 16-byte aligned");

static constexpr const char* DEBAND_VERSION = "1.1.0";
static constexpr float DB_DEFAULT_SD     = 0.007f;
static constexpr float DB_DEFAULT_WEBER  = 0.04f;
static constexpr float DB_DEFAULT_RADIUS = 24.0f;
static constexpr int   DB_DEFAULT_ITER   = 1;
static constexpr bool  DB_DEFAULT_WEBER_ON = true;
static constexpr bool  DB_DEFAULT_SD_ON    = true;
static constexpr int   DB_DEFAULT_DEBUG  = 0;

static const char* g_deband_debug_names[] = { "None", "LPF only", "Banding map" };

class DebandPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled         = false;
    float m_sd_threshold    = DB_DEFAULT_SD;
    float m_weber_threshold = DB_DEFAULT_WEBER;
    float m_radius          = DB_DEFAULT_RADIUS;
    int   m_iterations      = DB_DEFAULT_ITER;
    bool  m_enable_weber    = DB_DEFAULT_WEBER_ON;
    bool  m_enable_stddev   = DB_DEFAULT_SD_ON;
    int   m_debug_output    = DB_DEFAULT_DEBUG;
    unsigned m_frame_count  = 0;

    fx::SinglePassEffect<DebandParamsCB> m_fx{ g_deband_ps_src };

    void on_initialize() override {
        API::get()->log_info("[Deband] Plugin initialized (v%s)", DEBAND_VERSION);
        m_fx.init();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "Deband"; }
    int render_order() const override { return 1900; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",          m_enabled       ? "1" : "0"},
            {"sd_threshold",    std::to_string(m_sd_threshold)},
            {"weber_threshold", std::to_string(m_weber_threshold)},
            {"radius",          std::to_string(m_radius)},
            {"iterations",      std::to_string(m_iterations)},
            {"enable_weber",    m_enable_weber  ? "1" : "0"},
            {"enable_stddev",   m_enable_stddev ? "1" : "0"},
            {"debug_output",    std::to_string(m_debug_output)},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* k, float& out, float lo, float hi) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { float v = std::stof(it->second); if (v < lo) v = lo; if (v > hi) v = hi; out = v; } catch (...) {}
        };
        auto get_int = [&](const char* k, int& out, int lo, int hi) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { int v = std::stoi(it->second); if (v < lo) v = lo; if (v > hi) v = hi; out = v; } catch (...) {}
        };
        auto get_bool = [&](const char* k, bool& out) {
            auto it = kv.find(k);
            if (it != kv.end()) out = (it->second != "0" && !it->second.empty());
        };
        get_bool("enabled",          m_enabled);
        get_float("sd_threshold",    m_sd_threshold,    0.0f,  0.5f);
        get_float("weber_threshold", m_weber_threshold, 0.0f,  2.0f);
        get_float("radius",          m_radius,          1.0f, 32.0f);
        get_int("iterations",        m_iterations,      1,     4);
        get_bool("enable_weber",     m_enable_weber);
        get_bool("enable_stddev",    m_enable_stddev);
        get_int("debug_output",      m_debug_output,    0,     2);
    }

    void reset_to_defaults() override {
        m_enabled         = false;
        m_sd_threshold    = DB_DEFAULT_SD;
        m_weber_threshold = DB_DEFAULT_WEBER;
        m_radius          = DB_DEFAULT_RADIUS;
        m_iterations      = DB_DEFAULT_ITER;
        m_enable_weber    = DB_DEFAULT_WEBER_ON;
        m_enable_stddev   = DB_DEFAULT_SD_ON;
        m_debug_output    = DB_DEFAULT_DEBUG;
    }
    // ----------------------------------------------------------------------

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Deband Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", DEBAND_VERSION);
            ImGui::TextWrapped("Smooths color banding (visible 'rings' in skies/gradients) by averaging detected flat regions and adding subtle dither. Hits perf hardest of any shader here. Use only when you actually see banding.");
            bool ch = false;
            ch |= ImGui::Checkbox("Enabled##DB", &m_enabled);

            ch |= ImGui::SliderFloat("Std-Dev Threshold", &m_sd_threshold, 0.0f, 0.5f, "%.4f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower = more aggressive banding detection");
            ImGui::SameLine(); if (ImGui::Button("Reset##DB_sd"))     { m_sd_threshold = DB_DEFAULT_SD; ch = true; }

            ch |= ImGui::SliderFloat("Weber Threshold",  &m_weber_threshold, 0.0f, 2.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Local-contrast threshold relative to mean");
            ImGui::SameLine(); if (ImGui::Button("Reset##DB_weber"))  { m_weber_threshold = DB_DEFAULT_WEBER; ch = true; }

            ch |= ImGui::SliderFloat("Radius",  &m_radius, 1.0f, 32.0f, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search radius in pixels");
            ImGui::SameLine(); if (ImGui::Button("Reset##DB_radius")) { m_radius = DB_DEFAULT_RADIUS; ch = true; }

            ch |= ImGui::SliderInt("Iterations", &m_iterations, 1, 4);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("More iterations = stronger smoothing, more cost");
            ImGui::SameLine(); if (ImGui::Button("Reset##DB_iter"))   { m_iterations = DB_DEFAULT_ITER; ch = true; }

            ch |= ImGui::Checkbox("Enable Weber Detection",  &m_enable_weber);
            ImGui::SameLine(); if (ImGui::Button("Reset##DB_we"))     { m_enable_weber = DB_DEFAULT_WEBER_ON; ch = true; }

            ch |= ImGui::Checkbox("Enable Std-Dev Detection", &m_enable_stddev);
            ImGui::SameLine(); if (ImGui::Button("Reset##DB_se"))     { m_enable_stddev = DB_DEFAULT_SD_ON; ch = true; }

            if (ImGui::Combo("Debug Output", &m_debug_output, g_deband_debug_names, 3)) ch = true;
            ImGui::SameLine(); if (ImGui::Button("Reset##DB_dbg"))    { m_debug_output = DB_DEFAULT_DEBUG; ch = true; }

            ImGui::Spacing();
            if (ImGui::Button("Reset All##DB")) {
                m_sd_threshold    = DB_DEFAULT_SD;
                m_weber_threshold = DB_DEFAULT_WEBER;
                m_radius          = DB_DEFAULT_RADIUS;
                m_iterations      = DB_DEFAULT_ITER;
                m_enable_weber    = DB_DEFAULT_WEBER_ON;
                m_enable_stddev   = DB_DEFAULT_SD_ON;
                m_debug_output    = DB_DEFAULT_DEBUG;
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

        m_frame_count++;
        DebandParamsCB cb{};
        cb.SDThreshold    = m_sd_threshold;
        cb.WeberThreshold = m_weber_threshold;
        cb.Radius         = m_radius;
        cb.Iterations     = m_iterations;
        cb.RandomSeed     = (float)(m_frame_count % 32767u) / 32767.0f;
        cb.EnableWeber    = m_enable_weber  ? 1 : 0;
        cb.EnableStdDev   = m_enable_stddev ? 1 : 0;
        cb.DebugOutput    = m_debug_output;
        cb.PixelSize[0]   = 1.0f / (float)w;
        cb.PixelSize[1]   = 1.0f / (float)h;
        cb.ScreenSize[0]  = (float)w;
        cb.ScreenSize[1]  = (float)h;
        m_fx.set_cb(cb);
        m_fx.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_fx.release_resources(); }
};

std::unique_ptr<DebandPlugin> g_plugin{ new DebandPlugin() };
