/*
FXAA Plugin for UEVR
====================
Port of SweetFX FXAA to UEVR's fullscreen-triangle pipeline.
Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

Based on the ReShade FXAA effect by CeeJayDK:
  https://github.com/CeeJayDK/SweetFX/blob/master/Shaders/SweetFX/FXAA.fx
  https://github.com/CeeJayDK/SweetFX/blob/master/Shaders/SweetFX/FXAA.fxh
  License: MIT

Original FXAA algorithm:
  NVIDIA FXAA 3.11 by Timothy Lottes
  https://github.com/lyntel/GraphicsSamples/blob/master/samples/es3-kepler/FXAA/FXAA3_11.h

This software contains source code provided by NVIDIA Corporation.
*/

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static constexpr const char* FXAA_VERSION = "3.11.0";

static constexpr float DEFAULT_SUBPIX = 0.25f;
static constexpr float DEFAULT_EDGE_THRESHOLD = 0.125f;
static constexpr float DEFAULT_EDGE_THRESHOLD_MIN = 0.0f;

struct FXAACB {
    float Subpix;
    float EdgeThreshold;
    float EdgeThresholdMin;
    float _pad0;
    float RcpFrame[2];
    float _pad1[2];
};
static_assert(sizeof(FXAACB) == 32, "FXAACB must be 32 bytes");

static const char* g_fxaa_luma_ps_src = R"(
Texture2D Scene : register(t0);
SamplerState LinearSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target
{
    float4 color = Scene.SampleLevel(LinearSampler, input.TexCoord, 0.0);
    float luma = sqrt(dot(color.rgb * color.rgb, float3(0.299, 0.587, 0.114)));
    return float4(color.rgb, luma);
}
)";

static const char* g_fxaa_quality_ps_src = R"(
cbuffer FXAACB : register(b0) {
    float Subpix;
    float EdgeThreshold;
    float EdgeThresholdMin;
    float _pad0;
    float2 RcpFrame;
    float2 _pad1;
};

Texture2D RGBL : register(t0);
SamplerState LinearSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

#define FXAA_PC 1
#define FXAA_HLSL_5 1
#define FXAA_GREEN_AS_LUMA 0
#define FXAA_DISCARD 0
#define FXAA_GATHER4_ALPHA 1
#define FXAA_QUALITY__PRESET 15

#define FXAA_QUALITY__PS 8
#define FXAA_QUALITY__P0 1.0
#define FXAA_QUALITY__P1 1.5
#define FXAA_QUALITY__P2 2.0
#define FXAA_QUALITY__P3 2.0
#define FXAA_QUALITY__P4 2.0
#define FXAA_QUALITY__P5 2.0
#define FXAA_QUALITY__P6 4.0
#define FXAA_QUALITY__P7 12.0

#define FxaaBool bool
#define FxaaDiscard clip(-1)
#define FxaaFloat float
#define FxaaFloat2 float2
#define FxaaFloat3 float3
#define FxaaFloat4 float4
#define FxaaHalf half
#define FxaaHalf2 half2
#define FxaaHalf3 half3
#define FxaaHalf4 half4
#define FxaaSat(x) saturate(x)
#define FxaaInt2 int2
struct FxaaTex { SamplerState smpl; Texture2D tex; };
#define FxaaTexTop(t, p) t.tex.SampleLevel(t.smpl, p, 0.0)
#define FxaaTexOff(t, p, o, r) t.tex.SampleLevel(t.smpl, p, 0.0, o)
#define FxaaTexAlpha4(t, p) t.tex.GatherAlpha(t.smpl, p)
#define FxaaTexOffAlpha4(t, p, o) t.tex.GatherAlpha(t.smpl, p, o)
#define FxaaTexGreen4(t, p) t.tex.GatherGreen(t.smpl, p)
#define FxaaTexOffGreen4(t, p, o) t.tex.GatherGreen(t.smpl, p, o)

FxaaFloat FxaaLuma(FxaaFloat4 rgba) { return rgba.w; }

FxaaFloat4 FxaaPixelShader(
    FxaaFloat2 pos,
    FxaaFloat4 fxaaConsolePosPos,
    FxaaTex tex,
    FxaaTex fxaaConsole360TexExpBiasNegOne,
    FxaaTex fxaaConsole360TexExpBiasNegTwo,
    FxaaFloat2 fxaaQualityRcpFrame,
    FxaaFloat4 fxaaConsoleRcpFrameOpt,
    FxaaFloat4 fxaaConsoleRcpFrameOpt2,
    FxaaFloat4 fxaaConsole360RcpFrameOpt2,
    FxaaFloat fxaaQualitySubpix,
    FxaaFloat fxaaQualityEdgeThreshold,
    FxaaFloat fxaaQualityEdgeThresholdMin,
    FxaaFloat fxaaConsoleEdgeSharpness,
    FxaaFloat fxaaConsoleEdgeThreshold,
    FxaaFloat fxaaConsoleEdgeThresholdMin,
    FxaaFloat4 fxaaConsole360ConstDir
) {
/*--------------------------------------------------------------------------*/
    FxaaFloat2 posM;
    posM.x = pos.x;
    posM.y = pos.y;
    #if (FXAA_GATHER4_ALPHA == 1)
        #if (FXAA_DISCARD == 0)
            FxaaFloat4 rgbyM = FxaaTexTop(tex, posM);
            #if (FXAA_GREEN_AS_LUMA == 0)
                #define lumaM rgbyM.w
            #else
                #define lumaM rgbyM.y
            #endif
        #endif
        #if (FXAA_GREEN_AS_LUMA == 0)
            FxaaFloat4 luma4A = FxaaTexAlpha4(tex, posM);
            FxaaFloat4 luma4B = FxaaTexOffAlpha4(tex, posM, FxaaInt2(-1, -1));
        #else
            FxaaFloat4 luma4A = FxaaTexGreen4(tex, posM);
            FxaaFloat4 luma4B = FxaaTexOffGreen4(tex, posM, FxaaInt2(-1, -1));
        #endif
        #if (FXAA_DISCARD == 1)
            #define lumaM luma4A.w
        #endif
        #define lumaE luma4A.z
        #define lumaS luma4A.x
        #define lumaSE luma4A.y
        #define lumaNW luma4B.w
        #define lumaN luma4B.z
        #define lumaW luma4B.x
    #else
        FxaaFloat4 rgbyM = FxaaTexTop(tex, posM);
        #if (FXAA_GREEN_AS_LUMA == 0)
            #define lumaM rgbyM.w
        #else
            #define lumaM rgbyM.y
        #endif
        FxaaFloat lumaS = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 0, 1), fxaaQualityRcpFrame.xy));
        FxaaFloat lumaE = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 1, 0), fxaaQualityRcpFrame.xy));
        FxaaFloat lumaN = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 0,-1), fxaaQualityRcpFrame.xy));
        FxaaFloat lumaW = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2(-1, 0), fxaaQualityRcpFrame.xy));
    #endif
/*--------------------------------------------------------------------------*/
    FxaaFloat maxSM = max(lumaS, lumaM);
    FxaaFloat minSM = min(lumaS, lumaM);
    FxaaFloat maxESM = max(lumaE, maxSM);
    FxaaFloat minESM = min(lumaE, minSM);
    FxaaFloat maxWN = max(lumaN, lumaW);
    FxaaFloat minWN = min(lumaN, lumaW);
    FxaaFloat rangeMax = max(maxWN, maxESM);
    FxaaFloat rangeMin = min(minWN, minESM);
    FxaaFloat rangeMaxScaled = rangeMax * fxaaQualityEdgeThreshold;
    FxaaFloat range = rangeMax - rangeMin;
    FxaaFloat rangeMaxClamped = max(fxaaQualityEdgeThresholdMin, rangeMaxScaled);
    FxaaBool earlyExit = range < rangeMaxClamped;
/*--------------------------------------------------------------------------*/
    if(earlyExit)
        #if (FXAA_DISCARD == 1)
            FxaaDiscard;
        #else
            return rgbyM;
        #endif
/*--------------------------------------------------------------------------*/
    #if (FXAA_GATHER4_ALPHA == 0)
        FxaaFloat lumaNW = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2(-1,-1), fxaaQualityRcpFrame.xy));
        FxaaFloat lumaSE = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 1, 1), fxaaQualityRcpFrame.xy));
        FxaaFloat lumaNE = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 1,-1), fxaaQualityRcpFrame.xy));
        FxaaFloat lumaSW = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2(-1, 1), fxaaQualityRcpFrame.xy));
    #else
        FxaaFloat lumaNE = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2(1, -1), fxaaQualityRcpFrame.xy));
        FxaaFloat lumaSW = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2(-1, 1), fxaaQualityRcpFrame.xy));
    #endif
/*--------------------------------------------------------------------------*/
    FxaaFloat lumaNS = lumaN + lumaS;
    FxaaFloat lumaWE = lumaW + lumaE;
    FxaaFloat subpixRcpRange = 1.0/range;
    FxaaFloat subpixNSWE = lumaNS + lumaWE;
    FxaaFloat edgeHorz1 = (-2.0 * lumaM) + lumaNS;
    FxaaFloat edgeVert1 = (-2.0 * lumaM) + lumaWE;
/*--------------------------------------------------------------------------*/
    FxaaFloat lumaNESE = lumaNE + lumaSE;
    FxaaFloat lumaNWNE = lumaNW + lumaNE;
    FxaaFloat edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
    FxaaFloat edgeVert2 = (-2.0 * lumaN) + lumaNWNE;
/*--------------------------------------------------------------------------*/
    FxaaFloat lumaNWSW = lumaNW + lumaSW;
    FxaaFloat lumaSWSE = lumaSW + lumaSE;
    FxaaFloat edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
    FxaaFloat edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
    FxaaFloat edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
    FxaaFloat edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
    FxaaFloat edgeHorz = abs(edgeHorz3) + edgeHorz4;
    FxaaFloat edgeVert = abs(edgeVert3) + edgeVert4;
/*--------------------------------------------------------------------------*/
    FxaaFloat subpixNWSWNESE = lumaNWSW + lumaNESE;
    FxaaFloat lengthSign = fxaaQualityRcpFrame.x;
    FxaaBool horzSpan = edgeHorz >= edgeVert;
    FxaaFloat subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
/*--------------------------------------------------------------------------*/
    if(!horzSpan) lumaN = lumaW;
    if(!horzSpan) lumaS = lumaE;
    if(horzSpan) lengthSign = fxaaQualityRcpFrame.y;
    FxaaFloat subpixB = (subpixA * (1.0/12.0)) - lumaM;
/*--------------------------------------------------------------------------*/
    FxaaFloat gradientN = lumaN - lumaM;
    FxaaFloat gradientS = lumaS - lumaM;
    FxaaFloat lumaNN = lumaN + lumaM;
    FxaaFloat lumaSS = lumaS + lumaM;
    FxaaBool pairN = abs(gradientN) >= abs(gradientS);
    FxaaFloat gradient = max(abs(gradientN), abs(gradientS));
    if(pairN) lengthSign = -lengthSign;
    FxaaFloat subpixC = FxaaSat(abs(subpixB) * subpixRcpRange);
/*--------------------------------------------------------------------------*/
    FxaaFloat2 posB;
    posB.x = posM.x;
    posB.y = posM.y;
    FxaaFloat2 offNP;
    offNP.x = (!horzSpan) ? 0.0 : fxaaQualityRcpFrame.x;
    offNP.y = ( horzSpan) ? 0.0 : fxaaQualityRcpFrame.y;
    if(!horzSpan) posB.x += lengthSign * 0.5;
    if( horzSpan) posB.y += lengthSign * 0.5;
/*--------------------------------------------------------------------------*/
    FxaaFloat2 posN;
    posN.x = posB.x - offNP.x * FXAA_QUALITY__P0;
    posN.y = posB.y - offNP.y * FXAA_QUALITY__P0;
    FxaaFloat2 posP;
    posP.x = posB.x + offNP.x * FXAA_QUALITY__P0;
    posP.y = posB.y + offNP.y * FXAA_QUALITY__P0;
    FxaaFloat subpixD = ((-2.0)*subpixC) + 3.0;
    FxaaFloat lumaEndN = FxaaLuma(FxaaTexTop(tex, posN));
    FxaaFloat subpixE = subpixC * subpixC;
    FxaaFloat lumaEndP = FxaaLuma(FxaaTexTop(tex, posP));
/*--------------------------------------------------------------------------*/
    if(!pairN) lumaNN = lumaSS;
    FxaaFloat gradientScaled = gradient * 1.0/4.0;
    FxaaFloat lumaMM = lumaM - lumaNN * 0.5;
    FxaaFloat subpixF = subpixD * subpixE;
    FxaaBool lumaMLTZero = lumaMM < 0.0;
/*--------------------------------------------------------------------------*/
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    FxaaBool doneN = abs(lumaEndN) >= gradientScaled;
    FxaaBool doneP = abs(lumaEndP) >= gradientScaled;
    if(!doneN) posN.x -= offNP.x * FXAA_QUALITY__P1;
    if(!doneN) posN.y -= offNP.y * FXAA_QUALITY__P1;
    FxaaBool doneNP = (!doneN) || (!doneP);
    if(!doneP) posP.x += offNP.x * FXAA_QUALITY__P1;
    if(!doneP) posP.y += offNP.y * FXAA_QUALITY__P1;
/*--------------------------------------------------------------------------*/
    if(doneNP) {
        if(!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN.xy));
        if(!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP.xy));
        if(!doneN) lumaEndN = lumaEndN - lumaNN * 0.5;
        if(!doneP) lumaEndP = lumaEndP - lumaNN * 0.5;
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
        if(!doneN) posN.x -= offNP.x * FXAA_QUALITY__P2;
        if(!doneN) posN.y -= offNP.y * FXAA_QUALITY__P2;
        doneNP = (!doneN) || (!doneP);
        if(!doneP) posP.x += offNP.x * FXAA_QUALITY__P2;
        if(!doneP) posP.y += offNP.y * FXAA_QUALITY__P2;
/*--------------------------------------------------------------------------*/
        if(doneNP) {
            if(!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN.xy));
            if(!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP.xy));
            if(!doneN) lumaEndN = lumaEndN - lumaNN * 0.5;
            if(!doneP) lumaEndP = lumaEndP - lumaNN * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
            doneP = abs(lumaEndP) >= gradientScaled;
            if(!doneN) posN.x -= offNP.x * FXAA_QUALITY__P3;
            if(!doneN) posN.y -= offNP.y * FXAA_QUALITY__P3;
            doneNP = (!doneN) || (!doneP);
            if(!doneP) posP.x += offNP.x * FXAA_QUALITY__P3;
            if(!doneP) posP.y += offNP.y * FXAA_QUALITY__P3;
/*--------------------------------------------------------------------------*/
            if(doneNP) {
                if(!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN.xy));
                if(!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP.xy));
                if(!doneN) lumaEndN = lumaEndN - lumaNN * 0.5;
                if(!doneP) lumaEndP = lumaEndP - lumaNN * 0.5;
                doneN = abs(lumaEndN) >= gradientScaled;
                doneP = abs(lumaEndP) >= gradientScaled;
                if(!doneN) posN.x -= offNP.x * FXAA_QUALITY__P4;
                if(!doneN) posN.y -= offNP.y * FXAA_QUALITY__P4;
                doneNP = (!doneN) || (!doneP);
                if(!doneP) posP.x += offNP.x * FXAA_QUALITY__P4;
                if(!doneP) posP.y += offNP.y * FXAA_QUALITY__P4;
/*--------------------------------------------------------------------------*/
                if(doneNP) {
                    if(!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN.xy));
                    if(!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP.xy));
                    if(!doneN) lumaEndN = lumaEndN - lumaNN * 0.5;
                    if(!doneP) lumaEndP = lumaEndP - lumaNN * 0.5;
                    doneN = abs(lumaEndN) >= gradientScaled;
                    doneP = abs(lumaEndP) >= gradientScaled;
                    if(!doneN) posN.x -= offNP.x * FXAA_QUALITY__P5;
                    if(!doneN) posN.y -= offNP.y * FXAA_QUALITY__P5;
                    doneNP = (!doneN) || (!doneP);
                    if(!doneP) posP.x += offNP.x * FXAA_QUALITY__P5;
                    if(!doneP) posP.y += offNP.y * FXAA_QUALITY__P5;
/*--------------------------------------------------------------------------*/
                    if(doneNP) {
                        if(!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN.xy));
                        if(!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP.xy));
                        if(!doneN) lumaEndN = lumaEndN - lumaNN * 0.5;
                        if(!doneP) lumaEndP = lumaEndP - lumaNN * 0.5;
                        doneN = abs(lumaEndN) >= gradientScaled;
                        doneP = abs(lumaEndP) >= gradientScaled;
                        if(!doneN) posN.x -= offNP.x * FXAA_QUALITY__P6;
                        if(!doneN) posN.y -= offNP.y * FXAA_QUALITY__P6;
                        doneNP = (!doneN) || (!doneP);
                        if(!doneP) posP.x += offNP.x * FXAA_QUALITY__P6;
                        if(!doneP) posP.y += offNP.y * FXAA_QUALITY__P6;
/*--------------------------------------------------------------------------*/
                        if(doneNP) {
                            if(!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN.xy));
                            if(!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP.xy));
                            if(!doneN) lumaEndN = lumaEndN - lumaNN * 0.5;
                            if(!doneP) lumaEndP = lumaEndP - lumaNN * 0.5;
                            doneN = abs(lumaEndN) >= gradientScaled;
                            doneP = abs(lumaEndP) >= gradientScaled;
                            if(!doneN) posN.x -= offNP.x * FXAA_QUALITY__P7;
                            if(!doneN) posN.y -= offNP.y * FXAA_QUALITY__P7;
                            doneNP = (!doneN) || (!doneP);
                            if(!doneP) posP.x += offNP.x * FXAA_QUALITY__P7;
                            if(!doneP) posP.y += offNP.y * FXAA_QUALITY__P7;
/*--------------------------------------------------------------------------*/
                        }
                    }
                }
            }
        }
    }
/*--------------------------------------------------------------------------*/
    FxaaFloat dstN = posM.x - posN.x;
    FxaaFloat dstP = posP.x - posM.x;
    if(!horzSpan) dstN = posM.y - posN.y;
    if(!horzSpan) dstP = posP.y - posM.y;
/*--------------------------------------------------------------------------*/
    FxaaBool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    FxaaFloat spanLength = (dstP + dstN);
    FxaaBool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    FxaaFloat spanLengthRcp = 1.0/spanLength;
/*--------------------------------------------------------------------------*/
    FxaaBool directionN = dstN < dstP;
    FxaaFloat dst = min(dstN, dstP);
    FxaaBool goodSpan = directionN ? goodSpanN : goodSpanP;
    FxaaFloat subpixG = subpixF * subpixF;
    FxaaFloat pixelOffset = (dst * (-spanLengthRcp)) + 0.5;
    FxaaFloat subpixH = subpixG * fxaaQualitySubpix;
/*--------------------------------------------------------------------------*/
    FxaaFloat pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    FxaaFloat pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if(!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
    if( horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
    return FxaaFloat4(FxaaTexTop(tex, posM).xyz, lumaM);
}

float4 main(PSInput input) : SV_Target
{
    FxaaTex tex;
    tex.smpl = LinearSampler;
    tex.tex = RGBL;

    return float4(FxaaPixelShader(
        input.TexCoord,
        0,
        tex,
        tex,
        tex,
        RcpFrame,
        0,
        0,
        0,
        Subpix,
        EdgeThreshold,
        EdgeThresholdMin,
        0,
        0,
        0,
        0
    ).rgb, 1.0);
}
)";

class FXAAPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled = false;
    float m_subpix = DEFAULT_SUBPIX;
    float m_edge_threshold = DEFAULT_EDGE_THRESHOLD;
    float m_edge_threshold_min = DEFAULT_EDGE_THRESHOLD_MIN;

    FXAACB m_cb{};
    fx::EffectRuntime m_runtime;
    int m_rgbl_id = -1;
    bool m_passes_set = false;

    void on_initialize() override {
        API::get()->log_info("[FXAA] Plugin initialized (v%s)", FXAA_VERSION);
        configure_runtime();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "FXAA"; }
    int render_order() const override { return 1450; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled", m_enabled ? "1" : "0"},
            {"subpix", std::to_string(m_subpix)},
            {"edge_threshold", std::to_string(m_edge_threshold)},
            {"edge_threshold_min", std::to_string(m_edge_threshold_min)},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* key, float& out, float lo, float hi) {
            auto it = kv.find(key);
            if (it == kv.end()) return;
            try { out = std::clamp(std::stof(it->second), lo, hi); } catch (...) {}
        };
        auto it = kv.find("enabled");
        if (it != kv.end()) m_enabled = (it->second != "0" && !it->second.empty());
        get_float("subpix", m_subpix, 0.0f, 1.0f);
        get_float("edge_threshold", m_edge_threshold, 0.063f, 0.333f);
        get_float("edge_threshold_min", m_edge_threshold_min, 0.0f, 0.0833f);
    }

    void reset_to_defaults() override {
        m_enabled = false;
        restore_defaults();
    }

    void restore_defaults() {
        m_subpix = DEFAULT_SUBPIX;
        m_edge_threshold = DEFAULT_EDGE_THRESHOLD;
        m_edge_threshold_min = DEFAULT_EDGE_THRESHOLD_MIN;
    }

    void configure_runtime() {
        if (m_passes_set) return;

        fx::RTDesc rgbl{};
        rgbl.size_mode = fx::RTDesc::SizeMode::Backbuffer;
        rgbl.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        m_rgbl_id = m_runtime.declare_rt(rgbl);

        std::vector<fx::PassDesc> passes;
        passes.reserve(2);

        fx::PassDesc luma_pass;
        luma_pass.ps_hlsl = g_fxaa_luma_ps_src;
        luma_pass.inputs = { fx::INPUT_SCENE };
        luma_pass.output = m_rgbl_id;
        passes.push_back(std::move(luma_pass));

        fx::PassDesc fxaa_pass;
        fxaa_pass.ps_hlsl = g_fxaa_quality_ps_src;
        fxaa_pass.inputs = { m_rgbl_id };
        fxaa_pass.output = fx::OUTPUT_SCENE;
        fxaa_pass.cb_data = &m_cb;
        fxaa_pass.cb_size = sizeof(m_cb);
        passes.push_back(std::move(fxaa_pass));

        m_runtime.set_passes(std::move(passes));
        m_passes_set = true;
    }

    bool slider_with_reset(const char* label, float* value, float step, float minv, float maxv,
                           const char* fmt, float default_value, const char* reset_id,
                           const char* tooltip) {
        bool changed = ImGui::DragFloat(label, value, step, minv, maxv, fmt);
        if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
        if (ImGui::Button(reset_id)) { *value = default_value; changed = true; }
        return changed;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("FXAA", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s - NVIDIA FXAA 3.11 quality preset 15", FXAA_VERSION);
            ImGui::TextWrapped(
                "Fast approximate anti-aliasing for leftover stair-stepping after the game's own AA. "
                "Run it after LUT/color grading and before film grain or sharpening. Start with the defaults; "
                "raise Subpix only if edges still shimmer, and lower Edge Threshold if fine wires or foliage need more smoothing.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##FXAA", &m_enabled);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smooths jagged high-contrast edges. Leave off in games that already look soft.");
            changed |= slider_with_reset("Subpix##FXAA", &m_subpix, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_SUBPIX, "Reset##FXAA_subpix",
                                         "Subpixel aliasing cleanup. Higher values smooth crawling/shimmering edges more, but can soften texture detail.");
            changed |= slider_with_reset("Edge Threshold##FXAA", &m_edge_threshold, 0.001f, 0.063f, 0.333f, "%.3f", DEFAULT_EDGE_THRESHOLD, "Reset##FXAA_edge",
                                         "Main edge sensitivity. Lower = detects more edges and smooths more; higher = preserves more sharp detail.");
            changed |= slider_with_reset("Edge Threshold Min##FXAA", &m_edge_threshold_min, 0.001f, 0.0f, 0.0833f, "%.4f", DEFAULT_EDGE_THRESHOLD_MIN, "Reset##FXAA_edgemin",
                                         "Dark/low-contrast edge sensitivity. Raise if dark scenes get too soft; keep low if shadow edges still crawl.");

            if (ImGui::Button("Reset All##FXAA")) {
                restore_defaults();
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restore NVIDIA/SweetFX default tuning for this port.");
            if (changed) {
                m_subpix = std::clamp(m_subpix, 0.0f, 1.0f);
                m_edge_threshold = std::clamp(m_edge_threshold, 0.063f, 0.333f);
                m_edge_threshold_min = std::clamp(m_edge_threshold_min, 0.0f, 0.0833f);
                uevr::settings::notify_changed(*this, API::get()->param());
            }
        }
    }

    void update_cb() {
        const auto w = fx::EffectRuntime::scene_width();
        const auto h = fx::EffectRuntime::scene_height();
        m_cb.Subpix = m_subpix;
        m_cb.EdgeThreshold = m_edge_threshold;
        m_cb.EdgeThresholdMin = m_edge_threshold_min;
        m_cb.RcpFrame[0] = w > 0 ? 1.0f / static_cast<float>(w) : 0.0f;
        m_cb.RcpFrame[1] = h > 0 ? 1.0f / static_cast<float>(h) : 0.0f;
    }

    void run() {
        if (!m_enabled || !m_passes_set) return;
        const auto w = fx::EffectRuntime::scene_width();
        const auto h = fx::EffectRuntime::scene_height();
        if (w == 0 || h == 0) return;
        update_cb();
        m_runtime.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_runtime.release_resources(); }
};

std::unique_ptr<FXAAPlugin> g_plugin{ new FXAAPlugin() };