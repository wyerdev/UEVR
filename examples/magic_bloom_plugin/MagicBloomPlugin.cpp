/*
MagicBloom Plugin for UEVR
==========================
Faithful port of luluco250's MagicBloom.fx from Stormshade revision
6dad6589fe505e998b01295dc6c647b031386e74.

Exact source:
  https://github.com/Otakumouse/stormshade/blob/6dad6589fe505e998b01295dc6c647b031386e74/v4.X/reshade-shaders/Shader%20Library/Recommended/MagicBloom.fx

Required texture:
  https://github.com/Otakumouse/stormshade/blob/6dad6589fe505e998b01295dc6c647b031386e74/v4.X/reshade-shaders/Textures/MagicBloom_Dirt.png

The source's MIT text is shipped in MagicBloomShader-LICENSE.txt.
*/

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"
#include "plugin_assets.hpp"

using namespace uevr;

static constexpr const char* MAGIC_BLOOM_VERSION = "1.0.0";
static constexpr int MAGIC_BLOOM_ADAPT_RESOLUTION = 256;
static constexpr int MAGIC_BLOOM_MIP_LEVELS = 9;

static constexpr float DEFAULT_BLOOM_INTENSITY = 1.0f;
static constexpr float DEFAULT_BLOOM_THRESHOLD = 2.0f;
static constexpr float DEFAULT_DIRT_INTENSITY = 0.0f;
static constexpr float DEFAULT_EXPOSURE = 0.5f;
static constexpr float DEFAULT_ADAPT_SPEED = 0.1f;
static constexpr float DEFAULT_ADAPT_SENSITIVITY = 1.0f;
static constexpr float DEFAULT_ADAPT_CLIP_MIN = 0.0f;
static constexpr float DEFAULT_ADAPT_CLIP_MAX = 1.0f;
static constexpr int DEFAULT_ADAPT_PRECISION = 2;
static constexpr int DEFAULT_DEBUG = 0;
static constexpr bool DEFAULT_SMOOTH_BLOOM_UPSAMPLING = false;

#pragma pack(push, 4)
struct MagicBloomCB {
    float PixelSize[2];
    float fBloom_Intensity;
    float fBloom_Threshold;

    float fDirt_Intensity;
    float fExposure;
    float fAdapt_Speed;
    float fAdapt_Sensitivity;

    float f2Adapt_Clip[2];
    int32_t iAdapt_Precision;
    uint32_t iDebug;
    uint32_t iSmoothBloomUpsampling;
};
#pragma pack(pop)
static_assert(sizeof(MagicBloomCB) == 52, "MagicBloomCB must be 52 bytes");

#define MAGIC_BLOOM_HLSL R"(
cbuffer MagicBloomCB : register(b0) {
    float2 PixelSize;
    float fBloom_Intensity;
    float fBloom_Threshold;
    float fDirt_Intensity;
    float fExposure;
    float fAdapt_Speed;
    float fAdapt_Sensitivity;
    float2 f2Adapt_Clip;
    int iAdapt_Precision;
    uint iDebug;
    uint iSmoothBloomUpsampling;
};
SamplerState LinearSampler : register(s0);
SamplerState PointMinMagMipLinearSampler : register(s1);
struct PSI { float4 P : SV_Position; float2 uv : TEXCOORD0; };

float3 blur(Texture2D source, float2 uv, float scale) {
    float2 ps = PixelSize * scale;
    static const float kernel[9] = {
        0.0269955, 0.0647588, 0.120985, 0.176033, 0.199471,
        0.176033, 0.120985, 0.0647588, 0.0269955
    };
    static const float accum = 1.02352;
    float gaussian_weight = 0.0;
    float3 col = 0.0;
    [unroll]
    for (int x = -4; x <= 4; ++x) {
        for (int y = -4; y <= 4; ++y) {
            gaussian_weight = kernel[x + 4] * kernel[y + 4];
            col += source.Sample(LinearSampler, uv + ps * float2(x, y)).rgb * gaussian_weight;
        }
    }
    return col * accum;
}

float3 tonemap(float3 col, float exposure) {
    static const float A = 0.15;
    static const float B = 0.50;
    static const float C = 0.10;
    static const float D = 0.20;
    static const float E = 0.02;
    static const float F = 0.30;
    static const float W = 11.2;
    col *= exposure;
    col = ((col * (A * col + C * B) + D * E) / (col * (A * col + B) + D * F)) - E / F;
    static const float white = 1.0 / (((W * (A * W + C * B) + D * E) / (W * (A * W + B) + D * F)) - E / F);
    col *= white;
    return col;
}

float3 blend_screen(float3 a, float3 b) {
    return 1.0 - (1.0 - a) * (1.0 - b);
}

float cubic_weight(float x) {
    x = abs(x);
    return x <= 1.0 ? (1.5 * x * x * x - 2.5 * x * x + 1.0) :
           (x < 2.0 ? (-0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0) : 0.0);
}

float3 sample_smooth(Texture2D source, float2 uv) {
    uint width = 0;
    uint height = 0;
    source.GetDimensions(width, height);
    float2 size = float2(width, height);
    float2 texel = 1.0 / size;
    float2 coord = uv * size - 0.5;
    float2 base = floor(coord);
    float2 fraction = coord - base;
    float3 color = 0.0;
    [unroll]
    for (int y = -1; y <= 2; ++y) {
        float weight_y = cubic_weight((float)y - fraction.y);
        [unroll]
        for (int x = -1; x <= 2; ++x) {
            float weight = cubic_weight((float)x - fraction.x) * weight_y;
            color += source.SampleLevel(LinearSampler,
                                        (base + float2((float)x, (float)y) + 0.5) * texel,
                                        0.0).rgb * weight;
        }
    }
    return color;
}

float3 sample_bloom(Texture2D source, float2 uv) {
    if (iSmoothBloomUpsampling != 0) return sample_smooth(source, uv);
    return source.Sample(LinearSampler, uv).rgb;
}

)"

static const char* g_ps_blur1 = MAGIC_BLOOM_HLSL R"(
Texture2D Source : register(t0);
float4 main(PSI i) : SV_Target {
    float3 col = blur(Source, i.uv, 2.0);
    col = pow(abs(col), fBloom_Threshold);
    col *= fBloom_Intensity;
    return float4(col, 1.0);
}
)";

static const char* g_ps_blur2 = MAGIC_BLOOM_HLSL R"(
Texture2D Source : register(t0);
float4 main(PSI i) : SV_Target { return float4(blur(Source, i.uv, 4.0), 1.0); }
)";

static const char* g_ps_blur3 = MAGIC_BLOOM_HLSL R"(
Texture2D Source : register(t0);
float4 main(PSI i) : SV_Target { return float4(blur(Source, i.uv, 8.0), 1.0); }
)";

static const char* g_ps_blur4 = MAGIC_BLOOM_HLSL R"(
Texture2D Source : register(t0);
float4 main(PSI i) : SV_Target { return float4(blur(Source, i.uv, 8.0), 1.0); }
)";

static const char* g_ps_blur5 = MAGIC_BLOOM_HLSL R"(
Texture2D Source : register(t0);
float4 main(PSI i) : SV_Target { return float4(blur(Source, i.uv, 16.0), 1.0); }
)";

static const char* g_ps_blur6 = MAGIC_BLOOM_HLSL R"(
Texture2D Source : register(t0);
float4 main(PSI i) : SV_Target { return float4(blur(Source, i.uv, 32.0), 1.0); }
)";

static const char* g_ps_blur7 = MAGIC_BLOOM_HLSL R"(
Texture2D Source : register(t0);
float4 main(PSI i) : SV_Target { return float4(blur(Source, i.uv, 64.0), 1.0); }
)";

static const char* g_ps_blur8 = MAGIC_BLOOM_HLSL R"(
Texture2D Source : register(t0);
float4 main(PSI i) : SV_Target { return float4(blur(Source, i.uv, 128.0), 1.0); }
)";

static const char* g_ps_blend = MAGIC_BLOOM_HLSL R"(
Texture2D Scene : register(t0);
Texture2D Bloom1 : register(t1);
Texture2D Bloom2 : register(t2);
Texture2D Bloom3 : register(t3);
Texture2D Bloom4 : register(t4);
Texture2D Bloom5 : register(t5);
Texture2D Bloom6 : register(t6);
Texture2D Bloom7 : register(t7);
Texture2D Bloom8 : register(t8);
Texture2D Adapt : register(t9);
Texture2D Dirt : register(t10);
float4 main(PSI i) : SV_Target {
    float3 col = Scene.Sample(LinearSampler, i.uv).rgb;
    float3 bloom = sample_bloom(Bloom1, i.uv)
                 + sample_bloom(Bloom2, i.uv)
                 + sample_bloom(Bloom3, i.uv)
                 + sample_bloom(Bloom4, i.uv)
                 + sample_bloom(Bloom5, i.uv)
                 + sample_bloom(Bloom6, i.uv)
                 + sample_bloom(Bloom7, i.uv)
                 + sample_bloom(Bloom8, i.uv);
    static const float bloom_accum = 1.0 / 8.0;
    bloom *= bloom_accum;
    float exposure = fExposure / max(Adapt.Sample(PointMinMagMipLinearSampler, float2(0.0, 0.0)).x, 0.00001);
    bloom = tonemap(bloom, exposure);
    float3 dirt = Dirt.Sample(LinearSampler, i.uv).rgb;
    dirt *= fDirt_Intensity;
    bloom = blend_screen(bloom, dirt * bloom);
    col = blend_screen(col, bloom);
    col = iDebug == 1 ? bloom : col;
    return float4(col, 1.0);
}
)";

static const char* g_ps_get_small = MAGIC_BLOOM_HLSL R"(
Texture2D Scene : register(t0);
float main(PSI i) : SV_Target {
    return dot(Scene.Sample(LinearSampler, i.uv).rgb, float3(0.2126, 0.7152, 0.0722));
}
)";

static const char* g_ps_get_adapt = MAGIC_BLOOM_HLSL R"(
Texture2D Small : register(t0);
Texture2D LastAdapt : register(t1);
float main(PSI i) : SV_Target {
    float curr = Small.SampleLevel(LinearSampler, float2(0.5, 0.5), 9 - iAdapt_Precision).x;
    curr *= fAdapt_Sensitivity;
    curr = clamp(curr, f2Adapt_Clip.x, f2Adapt_Clip.y);
    float last = LastAdapt.Sample(PointMinMagMipLinearSampler, float2(0.0, 0.0)).x;
    return lerp(last, curr, fAdapt_Speed);
}
)";

static const char* g_ps_save_adapt = MAGIC_BLOOM_HLSL R"(
Texture2D Adapt : register(t0);
float main(PSI i) : SV_Target {
    return Adapt.Sample(PointMinMagMipLinearSampler, float2(0.0, 0.0)).x;
}
)";

class MagicBloomPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool m_enabled = false;
    float m_fBloom_Intensity = DEFAULT_BLOOM_INTENSITY;
    float m_fBloom_Threshold = DEFAULT_BLOOM_THRESHOLD;
    float m_fDirt_Intensity = DEFAULT_DIRT_INTENSITY;
    float m_fExposure = DEFAULT_EXPOSURE;
    float m_fAdapt_Speed = DEFAULT_ADAPT_SPEED;
    float m_fAdapt_Sensitivity = DEFAULT_ADAPT_SENSITIVITY;
    float m_f2Adapt_Clip[2] = {DEFAULT_ADAPT_CLIP_MIN, DEFAULT_ADAPT_CLIP_MAX};
    int m_iAdapt_Precision = DEFAULT_ADAPT_PRECISION;
    int m_iDebug = DEFAULT_DEBUG;
    bool m_smooth_bloom_upsampling = DEFAULT_SMOOTH_BLOOM_UPSAMPLING;

    MagicBloomCB m_cb{};
    fx::EffectRuntime m_runtime;
    int m_bloom[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int m_small = -1;
    int m_adapt = -1;
    int m_last_adapt = -1;
    int m_dirt = -1;
    bool m_passes_set = false;
    bool m_asset_ready = false;

    void on_initialize() override {
        API::get()->log_info("[MagicBloom] Plugin initialized (v%s)", MAGIC_BLOOM_VERSION);
        configure_runtime();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "MagicBloom"; }
    int render_order() const override { return 2025; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled", m_enabled ? "1" : "0"},
            {"fBloom_Intensity", std::to_string(m_fBloom_Intensity)},
            {"fBloom_Threshold", std::to_string(m_fBloom_Threshold)},
            {"fDirt_Intensity", std::to_string(m_fDirt_Intensity)},
            {"fExposure", std::to_string(m_fExposure)},
            {"fAdapt_Speed", std::to_string(m_fAdapt_Speed)},
            {"fAdapt_Sensitivity", std::to_string(m_fAdapt_Sensitivity)},
            {"f2Adapt_Clip.0", std::to_string(m_f2Adapt_Clip[0])},
            {"f2Adapt_Clip.1", std::to_string(m_f2Adapt_Clip[1])},
            {"iAdapt_Precision", std::to_string(m_iAdapt_Precision)},
            {"iDebug", std::to_string(m_iDebug)},
            {"smooth_bloom_upsampling", m_smooth_bloom_upsampling ? "1" : "0"},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* key, float& value) {
            auto it = kv.find(key);
            if (it == kv.end()) return;
            try { value = std::stof(it->second); } catch (...) {}
        };
        auto get_int = [&](const char* key, int& value) {
            auto it = kv.find(key);
            if (it == kv.end()) return;
            try { value = std::stoi(it->second); } catch (...) {}
        };
        auto enabled = kv.find("enabled");
        if (enabled != kv.end()) m_enabled = enabled->second != "0" && !enabled->second.empty();
        get_float("fBloom_Intensity", m_fBloom_Intensity);
        get_float("fBloom_Threshold", m_fBloom_Threshold);
        get_float("fDirt_Intensity", m_fDirt_Intensity);
        get_float("fExposure", m_fExposure);
        get_float("fAdapt_Speed", m_fAdapt_Speed);
        get_float("fAdapt_Sensitivity", m_fAdapt_Sensitivity);
        get_float("f2Adapt_Clip.0", m_f2Adapt_Clip[0]);
        get_float("f2Adapt_Clip.1", m_f2Adapt_Clip[1]);
        get_int("iAdapt_Precision", m_iAdapt_Precision);
        get_int("iDebug", m_iDebug);
        auto smooth_bloom_upsampling = kv.find("smooth_bloom_upsampling");
        if (smooth_bloom_upsampling != kv.end()) {
            m_smooth_bloom_upsampling = smooth_bloom_upsampling->second != "0" &&
                                        !smooth_bloom_upsampling->second.empty();
        }
        clamp_settings();
    }

    void reset_to_defaults() override {
        m_enabled = false;
        m_fBloom_Intensity = DEFAULT_BLOOM_INTENSITY;
        m_fBloom_Threshold = DEFAULT_BLOOM_THRESHOLD;
        m_fDirt_Intensity = DEFAULT_DIRT_INTENSITY;
        m_fExposure = DEFAULT_EXPOSURE;
        m_fAdapt_Speed = DEFAULT_ADAPT_SPEED;
        m_fAdapt_Sensitivity = DEFAULT_ADAPT_SENSITIVITY;
        m_f2Adapt_Clip[0] = DEFAULT_ADAPT_CLIP_MIN;
        m_f2Adapt_Clip[1] = DEFAULT_ADAPT_CLIP_MAX;
        m_iAdapt_Precision = DEFAULT_ADAPT_PRECISION;
        m_iDebug = DEFAULT_DEBUG;
        m_smooth_bloom_upsampling = DEFAULT_SMOOTH_BLOOM_UPSAMPLING;
    }

    void clamp_settings() {
        m_fBloom_Intensity = std::clamp(m_fBloom_Intensity, 0.0f, 10.0f);
        m_fBloom_Threshold = std::clamp(m_fBloom_Threshold, 1.0f, 10.0f);
        m_fDirt_Intensity = std::clamp(m_fDirt_Intensity, 0.0f, 1.0f);
        m_fExposure = std::clamp(m_fExposure, 0.0f, 1.0f);
        m_fAdapt_Speed = std::clamp(m_fAdapt_Speed, 0.001f, 1.0f);
        m_fAdapt_Sensitivity = std::clamp(m_fAdapt_Sensitivity, 0.0f, 3.0f);
        m_f2Adapt_Clip[0] = std::clamp(m_f2Adapt_Clip[0], 0.0f, 1.0f);
        m_f2Adapt_Clip[1] = std::clamp(m_f2Adapt_Clip[1], 0.0f, 1.0f);
        m_iAdapt_Precision = std::clamp(m_iAdapt_Precision, 0, MAGIC_BLOOM_MIP_LEVELS);
        m_iDebug = std::clamp(m_iDebug, 0, 1);
    }

    int declare_div_rt(int divisor) {
        fx::RTDesc rt{};
        rt.size_mode = fx::RTDesc::SizeMode::BackbufferDiv;
        rt.w_or_div = divisor;
        rt.h_or_div = divisor;
        rt.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        return m_runtime.declare_rt(rt);
    }

    void configure_runtime() {
        if (m_passes_set) return;

        for (int i = 0, divisor = 2; i < 8; ++i, divisor *= 2) {
            m_bloom[i] = declare_div_rt(divisor);
        }

        fx::RTDesc small_rt{};
        small_rt.size_mode = fx::RTDesc::SizeMode::Fixed;
        small_rt.w_or_div = MAGIC_BLOOM_ADAPT_RESOLUTION;
        small_rt.h_or_div = MAGIC_BLOOM_ADAPT_RESOLUTION;
        small_rt.format = DXGI_FORMAT_R32_FLOAT;
        small_rt.mip_levels = MAGIC_BLOOM_MIP_LEVELS;
        small_rt.auto_generate_mips = true;
        m_small = m_runtime.declare_rt(small_rt);

        fx::RTDesc adapt{};
        adapt.size_mode = fx::RTDesc::SizeMode::Fixed;
        adapt.w_or_div = 1;
        adapt.h_or_div = 1;
        adapt.format = DXGI_FORMAT_R32_FLOAT;
        m_adapt = m_runtime.declare_rt(adapt);

        fx::RTDesc last_adapt = adapt;
        last_adapt.persistent = true;
        last_adapt.shared_across_scene_slots = true;
        m_last_adapt = m_runtime.declare_rt(last_adapt);

        const auto dirt_path = resolve_shader_asset_path(L"MagicBloom_Dirt.png");
        if (dirt_path.empty()) {
            API::get()->log_error("[MagicBloom] Missing MagicBloom_Dirt.png");
            return;
        }
        m_dirt = m_runtime.load_external_texture_png(dirt_path, fx::ExternalTextureSizeMode::Scene);
        m_asset_ready = true;

        std::vector<fx::PassDesc> passes;
        passes.reserve(12);
        auto make_pass = [&](const char* hlsl, std::vector<int> inputs, int output) {
            fx::PassDesc pass{};
            pass.ps_hlsl = hlsl;
            pass.inputs = std::move(inputs);
            pass.output = output;
            pass.cb_data = &m_cb;
            pass.cb_size = sizeof(m_cb);
            passes.push_back(std::move(pass));
        };

        make_pass(g_ps_blur1, {fx::INPUT_SCENE}, m_bloom[0]);
        make_pass(g_ps_blur2, {m_bloom[0]}, m_bloom[1]);
        make_pass(g_ps_blur3, {m_bloom[1]}, m_bloom[2]);
        make_pass(g_ps_blur4, {m_bloom[2]}, m_bloom[3]);
        make_pass(g_ps_blur5, {m_bloom[3]}, m_bloom[4]);
        make_pass(g_ps_blur6, {m_bloom[4]}, m_bloom[5]);
        make_pass(g_ps_blur7, {m_bloom[5]}, m_bloom[6]);
        make_pass(g_ps_blur8, {m_bloom[6]}, m_bloom[7]);
        make_pass(g_ps_blend,
                  {fx::INPUT_SCENE, m_bloom[0], m_bloom[1], m_bloom[2], m_bloom[3],
                   m_bloom[4], m_bloom[5], m_bloom[6], m_bloom[7], m_adapt, m_dirt},
                  fx::OUTPUT_SCENE);
        make_pass(g_ps_get_small, {fx::INPUT_SCENE}, m_small);
        make_pass(g_ps_get_adapt, {m_small, m_last_adapt}, m_adapt);
        make_pass(g_ps_save_adapt, {m_adapt}, m_last_adapt);

        m_runtime.set_scene_snapshot_mode(fx::SceneSnapshotMode::BeforeEveryPass);
        m_runtime.set_passes(std::move(passes));
        m_passes_set = true;
        API::get()->log_info(
            "[MagicBloom] configure_runtime: bloom=%d,%d,%d,%d,%d,%d,%d,%d small=%d adapt=%d last=%d dirt=%d passes=12",
            m_bloom[0], m_bloom[1], m_bloom[2], m_bloom[3], m_bloom[4], m_bloom[5],
            m_bloom[6], m_bloom[7], m_small, m_adapt, m_last_adapt, m_dirt);
    }

    void on_draw_ui() override {
        if (!ImGui::CollapsingHeader("Magic Bloom", ImGuiTreeNodeFlags_DefaultOpen)) return;
        ImGui::TextDisabled("v%s based on MagicBloom.fx (see MagicBloomShader-LICENSE.txt)", MAGIC_BLOOM_VERSION);
        if (!m_asset_ready) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "Missing MagicBloom_Dirt.png");

        bool changed = false;
        const bool was_enabled = m_enabled;
        changed |= ImGui::Checkbox("Enabled##MagicBloom", &m_enabled);
        if (m_enabled != was_enabled) {
            API::get()->log_info("[MagicBloom] enabled=%d scene=%s colorspace=%d asset_ready=%d",
                                 m_enabled ? 1 : 0,
                                 fx::EffectRuntime::scene_rt_format_name(),
                                 static_cast<int>(fx::EffectRuntime::scene_rt_colorspace()),
                                 m_asset_ready ? 1 : 0);
        }
        changed |= ImGui::SliderFloat("Bloom Intensity##MagicBloom", &m_fBloom_Intensity, 0.0f, 10.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Reset##MagicBloom_bloom_intensity")) { m_fBloom_Intensity = DEFAULT_BLOOM_INTENSITY; changed = true; }
        changed |= ImGui::DragFloat("Bloom Threshold##MagicBloom", &m_fBloom_Threshold, 0.1f, 1.0f, 10.0f, "%.1f");
        ImGui::SameLine();
        if (ImGui::Button("Reset##MagicBloom_bloom_threshold")) { m_fBloom_Threshold = DEFAULT_BLOOM_THRESHOLD; changed = true; }
        changed |= ImGui::SliderFloat("Dirt Intensity##MagicBloom", &m_fDirt_Intensity, 0.0f, 1.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Reset##MagicBloom_dirt_intensity")) { m_fDirt_Intensity = DEFAULT_DIRT_INTENSITY; changed = true; }
        changed |= ImGui::SliderFloat("Exposure##MagicBloom", &m_fExposure, 0.0f, 1.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Reset##MagicBloom_exposure")) { m_fExposure = DEFAULT_EXPOSURE; changed = true; }
        changed |= ImGui::DragFloat("Adaptation Speed##MagicBloom", &m_fAdapt_Speed, 0.001f, 0.001f, 1.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Reset##MagicBloom_adapt_speed")) { m_fAdapt_Speed = DEFAULT_ADAPT_SPEED; changed = true; }
        changed |= ImGui::SliderFloat("Adapt Sensitivity##MagicBloom", &m_fAdapt_Sensitivity, 0.0f, 3.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Reset##MagicBloom_adapt_sensitivity")) { m_fAdapt_Sensitivity = DEFAULT_ADAPT_SENSITIVITY; changed = true; }
        changed |= ImGui::SliderFloat2("Adaptation Min/Max##MagicBloom", m_f2Adapt_Clip, 0.0f, 1.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Reset##MagicBloom_adapt_clip")) {
            m_f2Adapt_Clip[0] = DEFAULT_ADAPT_CLIP_MIN;
            m_f2Adapt_Clip[1] = DEFAULT_ADAPT_CLIP_MAX;
            changed = true;
        }
        changed |= ImGui::SliderInt("Adaptation Precision##MagicBloom", &m_iAdapt_Precision, 0, MAGIC_BLOOM_MIP_LEVELS);
        ImGui::SameLine();
        if (ImGui::Button("Reset##MagicBloom_adapt_precision")) { m_iAdapt_Precision = DEFAULT_ADAPT_PRECISION; changed = true; }
        const char* debug_items = "None\0Display Bloom Texture\0";
        changed |= ImGui::Combo("Debug Options##MagicBloom", &m_iDebug, debug_items);
        ImGui::SameLine();
        if (ImGui::Button("Reset##MagicBloom_debug")) { m_iDebug = DEFAULT_DEBUG; changed = true; }
        changed |= ImGui::Checkbox("Smooth Bloom Upsampling##MagicBloom", &m_smooth_bloom_upsampling);
        ImGui::Spacing();
        if (ImGui::Button("Reset All##MagicBloom")) {
            const bool enabled = m_enabled;
            reset_to_defaults();
            m_enabled = enabled;
            changed = true;
        }
        if (changed) {
            clamp_settings();
            uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void update_cb() {
        const unsigned width = fx::EffectRuntime::scene_width();
        const unsigned height = fx::EffectRuntime::scene_height();
        m_cb.PixelSize[0] = width > 0 ? 1.0f / static_cast<float>(width) : 0.0f;
        m_cb.PixelSize[1] = height > 0 ? 1.0f / static_cast<float>(height) : 0.0f;
        m_cb.fBloom_Intensity = m_fBloom_Intensity;
        m_cb.fBloom_Threshold = m_fBloom_Threshold;
        m_cb.fDirt_Intensity = m_fDirt_Intensity;
        m_cb.fExposure = m_fExposure;
        m_cb.fAdapt_Speed = m_fAdapt_Speed;
        m_cb.fAdapt_Sensitivity = m_fAdapt_Sensitivity;
        m_cb.f2Adapt_Clip[0] = m_f2Adapt_Clip[0];
        m_cb.f2Adapt_Clip[1] = m_f2Adapt_Clip[1];
        m_cb.iAdapt_Precision = m_iAdapt_Precision;
        m_cb.iDebug = static_cast<uint32_t>(m_iDebug);
        m_cb.iSmoothBloomUpsampling = m_smooth_bloom_upsampling ? 1u : 0u;
    }

    void run_impl() {
        if (!m_enabled || !m_passes_set || !m_asset_ready) return;
        update_cb();
        m_runtime.execute();
    }

    void run() {
        __try {
            run_impl();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static uint64_t last_log = 0;
            const uint64_t now = static_cast<uint64_t>(GetTickCount64());
            if (now - last_log > 1000) {
                last_log = now;
                API::get()->log_warn("[MagicBloom] SEH exception 0x%lx", (unsigned long)GetExceptionCode());
            }
        }
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_runtime.release_resources(); }
};

std::unique_ptr<MagicBloomPlugin> g_plugin{new MagicBloomPlugin()};