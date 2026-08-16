/*
Adaptive Sharpen Plugin for UEVR
================================
Faithful two-pass port of the exact AdaptiveSharpen.fx source supplied for this
plugin. The source expects full-range gamma light, so scene colorspace decoding
is intentionally disabled.

Source:
  https://github.com/byxor/thug-pro-reshade/blob/dbc1a73df3f817959760732038cab90400fd3a96/THUG%20Pro/reshade-shaders/Shaders/AdaptiveSharpen.fx
*/

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"
#include "effects/scene_warning.hpp"

using namespace uevr;

static constexpr const char* ADAPTIVE_SHARPEN_VERSION = "1.0.0";

static const char* g_adaptive_sharpen_preamble = R"(
cbuffer AdaptiveSharpenCB : register(b0) {
    float curve_height;
    float curveslope;
    float L_overshoot;
    float L_compr_low;
    float L_compr_high;
    float D_overshoot;
    float D_compr_low;
    float D_compr_high;
    float scale_lim;
    float scale_cs;
    float pm_p;
    float _padding0;
    float2 PixelSize;
    float2 _padding1;
};

Texture2D Scene : register(t0);
Texture2D Pass0 : register(t1);
SamplerState LinearSampler : register(s0);

struct PSInput {
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

#ifndef fast_ops
#define fast_ops 1
#endif

#define sqr(a)         ((a) * (a))
#define max4(a,b,c,d)  (max(max(a, b), max(c, d)))
#define texc(x,y)      (PixelSize * float2(x, y) + tex)
#define getB(x,y)      (saturate(Scene.Sample(LinearSampler, texc(x, y)).rgb))
#define getT(x,y)      (Pass0.Sample(LinearSampler, texc(x, y)).xy)
#define soft_if(a,b,c) (saturate((a + b + c + 0.056) * rcp(abs(maxedge) + 0.03) - 0.85))
#if (fast_ops == 1)
#define soft_lim(v,s)  (saturate(abs(v / s) * (27 + sqr(v / s)) / (27 + 9 * sqr(v / s))) * s)
#else
#define soft_lim(v,s)  ((exp(2 * min(abs(v), s * 24) / s) - 1) / (exp(2 * min(abs(v), s * 24) / s) + 1) * s)
#endif
#define wpmean(a,b,w)  (pow(abs(w) * pow(abs(a), pm_p) + abs(1 - w) * pow(abs(b), pm_p), (1.0 / pm_p)))
#define b_diff(pix)    (abs(blur - c[pix]))
#if (fast_ops == 1)
#define min_overshoot  (min(abs(L_overshoot), abs(D_overshoot)))
#define fskip_th       (0.114 * pow(min_overshoot, 0.676) + 3.20e-4)
#else
#define fskip_th       (0.000110882)
#endif
#define mdiff(a,b,c,d,e,f,g) (abs(luma[g] - luma[a]) + abs(luma[g] - luma[b]) \
                             + abs(luma[g] - luma[c]) + abs(luma[g] - luma[d]) \
                             + 0.5 * (abs(luma[g] - luma[e]) + abs(luma[g] - luma[f])))
)";

static const std::string g_adaptive_sharpen_pass0 = std::string(g_adaptive_sharpen_preamble) + R"(
float2 main(PSInput input) : SV_Target
{
    float2 tex = input.TexCoord;

    float3 c[13] = {
        getB( 0, 0), getB(-1,-1), getB( 0,-1), getB( 1,-1), getB(-1, 0),
        getB( 1, 0), getB(-1, 1), getB( 0, 1), getB( 1, 1), getB( 0,-2),
        getB(-2, 0), getB( 2, 0), getB( 0, 2)
    };

    float luma = sqrt(dot(float3(0.2558, 0.6511, 0.0931), sqr(c[0])));
    float3 blur = (2 * (c[2] + c[4] + c[5] + c[7]) + (c[1] + c[3] + c[6] + c[8]) + 4 * c[0]) / 16;
    float c_comp = saturate(4.0 / 15.0 + 0.9 * exp2(dot(blur, -37.0 / 15.0)));

    float edge = length(1.38 * b_diff(0)
                      + 1.15 * (b_diff(2) + b_diff(4) + b_diff(5) + b_diff(7))
                      + 0.92 * (b_diff(1) + b_diff(3) + b_diff(6) + b_diff(8))
                      + 0.23 * (b_diff(9) + b_diff(10) + b_diff(11) + b_diff(12)));

    return float2(edge * c_comp, luma);
}
)";

static const std::string g_adaptive_sharpen_pass1 = std::string(g_adaptive_sharpen_preamble) + R"(
float3 main(PSInput input) : SV_Target
{
    float2 tex = input.TexCoord;
    float3 origsat = getB(0, 0);

    float2 d[25] = {
        getT( 0, 0), getT(-1,-1), getT( 0,-1), getT( 1,-1), getT(-1, 0),
        getT( 1, 0), getT(-1, 1), getT( 0, 1), getT( 1, 1), getT( 0,-2),
        getT(-2, 0), getT( 2, 0), getT( 0, 2), getT( 0, 3), getT( 1, 2),
        getT(-1, 2), getT( 3, 0), getT( 2, 1), getT( 2,-1), getT(-3, 0),
        getT(-2, 1), getT(-2,-1), getT( 0,-3), getT( 1,-2), getT(-1,-2)
    };

    float maxedge = max4(max4(d[1].x, d[2].x, d[3].x, d[4].x),
                         max4(d[5].x, d[6].x, d[7].x, d[8].x),
                         max4(d[9].x, d[10].x, d[11].x, d[12].x), d[0].x);

    float sbe = soft_if(d[2].x, d[9].x, d[22].x) * soft_if(d[7].x, d[12].x, d[13].x)
              + soft_if(d[4].x, d[10].x, d[19].x) * soft_if(d[5].x, d[11].x, d[16].x)
              + soft_if(d[1].x, d[24].x, d[21].x) * soft_if(d[8].x, d[14].x, d[17].x)
              + soft_if(d[3].x, d[23].x, d[18].x) * soft_if(d[6].x, d[20].x, d[15].x);

#if (fast_ops == 1)
    float2 cs = lerp(float2(L_compr_low, D_compr_low),
                     float2(L_compr_high, D_compr_high), saturate(1.091 * sbe - 2.282));
#else
    float2 cs = lerp(float2(L_compr_low, D_compr_low),
                     float2(L_compr_high, D_compr_high), smoothstep(2, 3.1, sbe));
#endif

    float luma[25] = {
        d[0].y, d[1].y, d[2].y, d[3].y, d[4].y,
        d[5].y, d[6].y, d[7].y, d[8].y, d[9].y,
        d[10].y, d[11].y, d[12].y, d[13].y, d[14].y,
        d[15].y, d[16].y, d[17].y, d[18].y, d[19].y,
        d[20].y, d[21].y, d[22].y, d[23].y, d[24].y
    };

    const float3 W1 = float3(0.5, 1.0, 1.41421356237);
    const float3 W2 = float3(0.86602540378, 1.0, 0.54772255751);
#if (fast_ops == 1)
    float3 dW = sqr(lerp(W1, W2, saturate(2.4 * d[0].x - 0.82)));
#else
    float3 dW = sqr(lerp(W1, W2, smoothstep(0.3, 0.8, d[0].x)));
#endif

    float mdiff_c0 = 0.02 + 3 * (abs(luma[0] - luma[2]) + abs(luma[0] - luma[4])
                               + abs(luma[0] - luma[5]) + abs(luma[0] - luma[7])
                               + 0.25 * (abs(luma[0] - luma[1]) + abs(luma[0] - luma[3])
                                       + abs(luma[0] - luma[6]) + abs(luma[0] - luma[8])));

    float weights[12] = {
        min(mdiff_c0 / mdiff(24, 21, 2, 4, 9, 10, 1), dW.y),
        dW.x,
        min(mdiff_c0 / mdiff(23, 18, 5, 2, 9, 11, 3), dW.y),
        dW.x,
        dW.x,
        min(mdiff_c0 / mdiff(4, 20, 15, 7, 10, 12, 6), dW.y),
        dW.x,
        min(mdiff_c0 / mdiff(5, 7, 17, 14, 12, 11, 8), dW.y),
        min(mdiff_c0 / mdiff(2, 24, 23, 22, 1, 3, 9), dW.z),
        min(mdiff_c0 / mdiff(20, 19, 21, 4, 1, 6, 10), dW.z),
        min(mdiff_c0 / mdiff(17, 5, 18, 16, 3, 8, 11), dW.z),
        min(mdiff_c0 / mdiff(13, 15, 7, 14, 6, 8, 12), dW.z)
    };

    weights[0] = (max(max((weights[8] + weights[9]) / 4, weights[0]), 0.25) + weights[0]) / 2;
    weights[2] = (max(max((weights[8] + weights[10]) / 4, weights[2]), 0.25) + weights[2]) / 2;
    weights[5] = (max(max((weights[9] + weights[11]) / 4, weights[5]), 0.25) + weights[5]) / 2;
    weights[7] = (max(max((weights[10] + weights[11]) / 4, weights[7]), 0.25) + weights[7]) / 2;

    float lowthrsum = 0;
    float weightsum = 0;
    float neg_laplace = 0;

    [unroll]
    for (int pix = 0; pix < 12; ++pix) {
#if (fast_ops == 1)
        float lowthr = clamp(13.2 * d[pix + 1].x - 0.221, 0.01, 1);
        neg_laplace += sqr(luma[pix + 1]) * (weights[pix] * lowthr);
#else
        float t = saturate((d[pix + 1].x - 0.01) / 0.09);
        float lowthr = t * t * (2.97 - 1.98 * t) + 0.01;
        neg_laplace += pow(abs(luma[pix + 1]) + 0.06, 2.4) * (weights[pix] * lowthr);
#endif
        weightsum += weights[pix] * lowthr;
        lowthrsum += lowthr / 12;
    }

#if (fast_ops == 1)
    neg_laplace = sqrt(neg_laplace / weightsum);
#else
    neg_laplace = pow(abs(neg_laplace / weightsum), (1.0 / 2.4)) - 0.06;
#endif
    float sharpen_val = curve_height / (curve_height * curveslope * pow(abs(d[0].x), 3.5) + 0.625);
    float sharpdiff = (d[0].y - neg_laplace) * (lowthrsum * sharpen_val + 0.01);

    [branch]
    if (abs(sharpdiff) > fskip_th) {
        float temp;
        int i;
        int ii;

        [unroll]
        for (i = 0; i < 24; i += 2) {
            temp = luma[i];
            luma[i] = min(luma[i], luma[i + 1]);
            luma[i + 1] = max(temp, luma[i + 1]);
        }
        [unroll]
        for (ii = 24; ii > 0; ii -= 2) {
            temp = luma[0];
            luma[0] = min(luma[0], luma[ii]);
            luma[ii] = max(temp, luma[ii]);
            temp = luma[24];
            luma[24] = max(luma[24], luma[ii - 1]);
            luma[ii - 1] = min(temp, luma[ii - 1]);
        }

        [unroll]
        for (i = 1; i < 23; i += 2) {
            temp = luma[i];
            luma[i] = min(luma[i], luma[i + 1]);
            luma[i + 1] = max(temp, luma[i + 1]);
        }
        [unroll]
        for (ii = 23; ii > 1; ii -= 2) {
            temp = luma[1];
            luma[1] = min(luma[1], luma[ii]);
            luma[ii] = max(temp, luma[ii]);
            temp = luma[23];
            luma[23] = max(luma[23], luma[ii - 1]);
            luma[ii - 1] = min(temp, luma[ii - 1]);
        }

#if (fast_ops != 1)
        [unroll]
        for (i = 2; i < 22; i += 2) {
            temp = luma[i];
            luma[i] = min(luma[i], luma[i + 1]);
            luma[i + 1] = max(temp, luma[i + 1]);
        }
        [unroll]
        for (ii = 22; ii > 2; ii -= 2) {
            temp = luma[2];
            luma[2] = min(luma[2], luma[ii]);
            luma[ii] = max(temp, luma[ii]);
            temp = luma[22];
            luma[22] = max(luma[22], luma[ii - 1]);
            luma[ii - 1] = min(temp, luma[ii - 1]);
        }
#endif

#if (fast_ops == 1)
        float nmax = (max(luma[23], d[0].y) * 2 + luma[24]) / 3;
        float nmin = (min(luma[1], d[0].y) * 2 + luma[0]) / 3;
        float min_dist = min(abs(nmax - d[0].y), abs(d[0].y - nmin));
        float pos_scale = min_dist + L_overshoot;
        float neg_scale = min_dist + D_overshoot;
#else
        float nmax = (max(luma[22] + luma[23] * 2, d[0].y * 3) + luma[24]) / 4;
        float nmin = (min(luma[2] + luma[1] * 2, d[0].y * 3) + luma[0]) / 4;
        float min_dist = min(abs(nmax - d[0].y), abs(d[0].y - nmin));
        float pos_scale = min_dist + min(L_overshoot, 1.0001 - min_dist - d[0].y);
        float neg_scale = min_dist + min(D_overshoot, 0.0001 + d[0].y - min_dist);
#endif

        pos_scale = min(pos_scale, scale_lim * (1 - scale_cs) + pos_scale * scale_cs);
        neg_scale = min(neg_scale, scale_lim * (1 - scale_cs) + neg_scale * scale_cs);

        sharpdiff = wpmean(max(sharpdiff, 0), soft_lim(max(sharpdiff, 0), pos_scale), cs.x)
                  - wpmean(min(sharpdiff, 0), soft_lim(min(sharpdiff, 0), neg_scale), cs.y);
    }

    float sharpdiff_lim = saturate(d[0].y + sharpdiff) - d[0].y;
    float satmul = (d[0].y + max(sharpdiff_lim * 0.9, sharpdiff_lim) * 1.03 + 0.03) / (d[0].y + 0.03);
    float3 res = d[0].y + (sharpdiff_lim * 3 + sharpdiff) / 4 + (origsat - d[0].y) * satmul;

    return saturate(res);
}
)";

struct AdaptiveSharpenCB {
    float curve_height;
    float curveslope;
    float L_overshoot;
    float L_compr_low;
    float L_compr_high;
    float D_overshoot;
    float D_compr_low;
    float D_compr_high;
    float scale_lim;
    float scale_cs;
    float pm_p;
    float padding0;
    float PixelSize[2];
    float padding1[2];
};
static_assert(sizeof(AdaptiveSharpenCB) == 64, "AdaptiveSharpenCB must be 64 bytes");

static constexpr float AS_DEFAULT_CURVE_HEIGHT = 1.0f;
static constexpr float AS_DEFAULT_CURVE_SLOPE  = 0.5f;
static constexpr float AS_DEFAULT_L_OVERSHOOT  = 0.003f;
static constexpr float AS_DEFAULT_L_COMPR_LOW  = 0.167f;
static constexpr float AS_DEFAULT_L_COMPR_HIGH = 0.334f;
static constexpr float AS_DEFAULT_D_OVERSHOOT  = 0.009f;
static constexpr float AS_DEFAULT_D_COMPR_LOW  = 0.250f;
static constexpr float AS_DEFAULT_D_COMPR_HIGH = 0.500f;
static constexpr float AS_DEFAULT_SCALE_LIM    = 0.1f;
static constexpr float AS_DEFAULT_SCALE_CS     = 0.056f;
static constexpr float AS_DEFAULT_PM_P         = 0.7f;

class AdaptiveSharpenPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool m_enabled = false;
    float m_curve_height = AS_DEFAULT_CURVE_HEIGHT;
    float m_curveslope = AS_DEFAULT_CURVE_SLOPE;
    float m_L_overshoot = AS_DEFAULT_L_OVERSHOOT;
    float m_L_compr_low = AS_DEFAULT_L_COMPR_LOW;
    float m_L_compr_high = AS_DEFAULT_L_COMPR_HIGH;
    float m_D_overshoot = AS_DEFAULT_D_OVERSHOOT;
    float m_D_compr_low = AS_DEFAULT_D_COMPR_LOW;
    float m_D_compr_high = AS_DEFAULT_D_COMPR_HIGH;
    float m_scale_lim = AS_DEFAULT_SCALE_LIM;
    float m_scale_cs = AS_DEFAULT_SCALE_CS;
    float m_pm_p = AS_DEFAULT_PM_P;

    AdaptiveSharpenCB m_cb{};
    fx::EffectRuntime m_runtime;
    int m_pass0_id = -1;
    bool m_passes_set = false;
    bool m_logged_execution = false;

    void on_initialize() override {
        API::get()->log_info("[AdaptiveSharpen] Plugin initialized (v%s)", ADAPTIVE_SHARPEN_VERSION);
        configure_runtime();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "AdaptiveSharpen"; }
    int render_order() const override { return 1750; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",       m_enabled ? "1" : "0"},
            {"curve_height",  std::to_string(m_curve_height)},
            {"curveslope",    std::to_string(m_curveslope)},
            {"L_overshoot",   std::to_string(m_L_overshoot)},
            {"L_compr_low",   std::to_string(m_L_compr_low)},
            {"L_compr_high",  std::to_string(m_L_compr_high)},
            {"D_overshoot",   std::to_string(m_D_overshoot)},
            {"D_compr_low",   std::to_string(m_D_compr_low)},
            {"D_compr_high",  std::to_string(m_D_compr_high)},
            {"scale_lim",     std::to_string(m_scale_lim)},
            {"scale_cs",      std::to_string(m_scale_cs)},
            {"pm_p",          std::to_string(m_pm_p)},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* key, float& value, float minimum, float maximum) {
            auto it = kv.find(key);
            if (it == kv.end()) return;
            try {
                float parsed = std::stof(it->second);
                value = parsed < minimum ? minimum : (parsed > maximum ? maximum : parsed);
            } catch (...) {}
        };
        auto enabled = kv.find("enabled");
        if (enabled != kv.end()) m_enabled = enabled->second != "0" && !enabled->second.empty();
        get_float("curve_height", m_curve_height, 0.01f, 2.0f);
        get_float("curveslope", m_curveslope, 0.01f, 2.0f);
        get_float("L_overshoot", m_L_overshoot, 0.001f, 0.1f);
        get_float("L_compr_low", m_L_compr_low, 0.0f, 1.0f);
        get_float("L_compr_high", m_L_compr_high, 0.0f, 1.0f);
        get_float("D_overshoot", m_D_overshoot, 0.001f, 0.1f);
        get_float("D_compr_low", m_D_compr_low, 0.0f, 1.0f);
        get_float("D_compr_high", m_D_compr_high, 0.0f, 1.0f);
        get_float("scale_lim", m_scale_lim, 0.01f, 1.0f);
        get_float("scale_cs", m_scale_cs, 0.0f, 1.0f);
        get_float("pm_p", m_pm_p, 0.01f, 1.0f);
    }

    void reset_to_defaults() override {
        m_enabled = false;
        m_curve_height = AS_DEFAULT_CURVE_HEIGHT;
        m_curveslope = AS_DEFAULT_CURVE_SLOPE;
        m_L_overshoot = AS_DEFAULT_L_OVERSHOOT;
        m_L_compr_low = AS_DEFAULT_L_COMPR_LOW;
        m_L_compr_high = AS_DEFAULT_L_COMPR_HIGH;
        m_D_overshoot = AS_DEFAULT_D_OVERSHOOT;
        m_D_compr_low = AS_DEFAULT_D_COMPR_LOW;
        m_D_compr_high = AS_DEFAULT_D_COMPR_HIGH;
        m_scale_lim = AS_DEFAULT_SCALE_LIM;
        m_scale_cs = AS_DEFAULT_SCALE_CS;
        m_pm_p = AS_DEFAULT_PM_P;
    }

    void configure_runtime() {
        if (m_passes_set) return;

        fx::RTDesc pass0{};
        pass0.size_mode = fx::RTDesc::SizeMode::Backbuffer;
        pass0.format = DXGI_FORMAT_R16G16_FLOAT;
        m_pass0_id = m_runtime.declare_rt(pass0);

        fx::PassDesc first_pass;
        first_pass.ps_hlsl = g_adaptive_sharpen_pass0.c_str();
        first_pass.inputs = { fx::INPUT_SCENE };
        first_pass.output = m_pass0_id;
        first_pass.cb_data = &m_cb;
        first_pass.cb_size = sizeof(m_cb);

        fx::PassDesc second_pass;
        second_pass.ps_hlsl = g_adaptive_sharpen_pass1.c_str();
        second_pass.inputs = { fx::INPUT_SCENE, m_pass0_id };
        second_pass.output = fx::OUTPUT_SCENE;
        second_pass.cb_data = &m_cb;
        second_pass.cb_size = sizeof(m_cb);

        std::vector<fx::PassDesc> passes;
        passes.push_back(std::move(first_pass));
        passes.push_back(std::move(second_pass));
        m_runtime.set_passes(std::move(passes));
        m_passes_set = true;
        API::get()->log_info("[AdaptiveSharpen] configured passes=2 pass0_rt=%d format=R16G16_FLOAT fast_ops=1",
                             m_pass0_id);
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Adaptive Sharpen", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s based on AdaptiveSharpen.fx (see AdaptiveSharpenShader-LICENSE.txt)", ADAPTIVE_SHARPEN_VERSION);
            ImGui::TextWrapped("Adaptive edge-aware sharpening with anti-ringing compression. The source expects full-range gamma-light scene values.");
            fx::draw_scene_rt_colorspace_warning();

            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##AdaptiveSharpen", &m_enabled);
            changed |= ImGui::SliderFloat("Sharpening strength##AdaptiveSharpen", &m_curve_height, 0.01f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Main control of sharpening strength.");

            if (ImGui::TreeNode("Advanced##AdaptiveSharpen")) {
                changed |= ImGui::SliderFloat("Curve slope##AdaptiveSharpen", &m_curveslope, 0.01f, 2.0f, "%.2f");
                changed |= ImGui::SliderFloat("Light overshoot##AdaptiveSharpen", &m_L_overshoot, 0.001f, 0.1f, "%.3f");
                changed |= ImGui::SliderFloat("Light compression low##AdaptiveSharpen", &m_L_compr_low, 0.0f, 1.0f, "%.3f");
                changed |= ImGui::SliderFloat("Light compression high##AdaptiveSharpen", &m_L_compr_high, 0.0f, 1.0f, "%.3f");
                changed |= ImGui::SliderFloat("Dark overshoot##AdaptiveSharpen", &m_D_overshoot, 0.001f, 0.1f, "%.3f");
                changed |= ImGui::SliderFloat("Dark compression low##AdaptiveSharpen", &m_D_compr_low, 0.0f, 1.0f, "%.3f");
                changed |= ImGui::SliderFloat("Dark compression high##AdaptiveSharpen", &m_D_compr_high, 0.0f, 1.0f, "%.3f");
                changed |= ImGui::SliderFloat("Scale limit##AdaptiveSharpen", &m_scale_lim, 0.01f, 1.0f, "%.3f");
                changed |= ImGui::SliderFloat("Scale compression slope##AdaptiveSharpen", &m_scale_cs, 0.0f, 1.0f, "%.3f");
                changed |= ImGui::SliderFloat("Power mean p-value##AdaptiveSharpen", &m_pm_p, 0.01f, 1.0f, "%.2f");
                ImGui::TreePop();
            }

            if (ImGui::Button("Reset All##AdaptiveSharpen")) {
                reset_to_defaults();
                changed = true;
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        const unsigned width = fx::EffectRuntime::scene_width();
        const unsigned height = fx::EffectRuntime::scene_height();
        if (width == 0 || height == 0) return;

        m_cb.curve_height = m_curve_height;
        m_cb.curveslope = m_curveslope;
        m_cb.L_overshoot = m_L_overshoot;
        m_cb.L_compr_low = m_L_compr_low;
        m_cb.L_compr_high = m_L_compr_high;
        m_cb.D_overshoot = m_D_overshoot;
        m_cb.D_compr_low = m_D_compr_low;
        m_cb.D_compr_high = m_D_compr_high;
        m_cb.scale_lim = m_scale_lim;
        m_cb.scale_cs = m_scale_cs;
        m_cb.pm_p = m_pm_p;
        m_cb.PixelSize[0] = 1.0f / static_cast<float>(width);
        m_cb.PixelSize[1] = 1.0f / static_cast<float>(height);
        m_runtime.execute();

        if (!m_logged_execution) {
            API::get()->log_info("[AdaptiveSharpen] first execute width=%u height=%u scene_format=%s",
                                 width, height, fx::EffectRuntime::scene_rt_format_name());
            m_logged_execution = true;
        }
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }

    void on_device_reset() override {
        m_runtime.release_resources();
        m_logged_execution = false;
        API::get()->log_info("[AdaptiveSharpen] device reset: resources released");
    }
};

std::unique_ptr<AdaptiveSharpenPlugin> g_plugin{ new AdaptiveSharpenPlugin() };
