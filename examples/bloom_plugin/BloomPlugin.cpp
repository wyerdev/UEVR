/*
Bloom Plugin for UEVR
=====================
Faithful port of crosire/legacy Bloom.fx — ~30 uniforms, 7 intermediate render
targets, 10 passes, 2 external textures (LensDB, LensSprite). Bloom + AnamFlare
+ Lenz + Chapman lens flares + Godrays + LensDirt + Flare combine into a single
multi-pass effect graph.

Original shader:
  Bloom and lens flares for ReShade
  Copyright (c) 2009-2015 Gilcher Pascal aka Marty McFly
  Source: legacy crosire/reshade-shaders (community collection)
  Mirror used for code reference:
    https://github.com/Matsilagi/reshade-shaders/blob/master/Shaders/Bloom.fx
  No explicit license was provided in the original file or repository.
  All rights remain with the original author. Ported with credit per the
  fork precedent documented in docs/shader-candidates.md (same treatment as
  the CeeJay.dk shaders). See 19_BloomShader-LICENSE.txt for full notice.

Bundled assets (LensDB.png, LensSprite.png) are vendored from the Matsilagi
mirror under the same "all rights reserved" terms as the shader itself.

Shader math is verbatim from the ReShade source. Format-correct GPU views handle
gamma round-trip; if the scene RT is a plain *_UNORM the colorspace warning fires
in the UI. All four `*_DEPTH_CHECK` macros are 0 (their default in the original)
because UEVR doesn't expose a depth buffer to plugins.

The runtime auto-snapshots scene → mip chain (5 levels) so passes that sample
BackBuffer at LOD 1/3/4 (Lenz/Chapman/Godrays/AnamFlare bright pass) get correct
downsampled data.

UEVR plugin wrapper code: MIT license (C++ wrapper code ONLY).
*/

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "plugin_assets.hpp"
#include "effects/effect_runtime.hpp"
#include "effects/scene_warning.hpp"

using namespace uevr;

static constexpr const char* BLOOM_VERSION = "1.0.0";

// ---------------------------------------------------------------------------
// Constant buffer mirroring all ~30 uniforms. Layout matches HLSL natural
// packing: scalars pack into 4-per-row, float3 occupies 12 bytes within a row
// (next scalar may pack into the remaining 4 bytes), float2 occupies 8.
// 14 rows × 16 bytes = 224 bytes total.
// ---------------------------------------------------------------------------
#pragma pack(push, 4)
struct BloomCB {
    // Row 0
    float    PixelSize[2];
    float    AspectRatio;
    float    fBloomThreshold;
    // Row 1
    float    fBloomAmount;
    float    fBloomSaturation;
    float    fLensdirtIntensity;
    float    fLensdirtSaturation;
    // Row 2
    float    fBloomTint[3];
    int32_t  iBloomMixmode;       // packs into row 2 .w
    // Row 3
    float    fLensdirtTint[3];
    int32_t  iLensdirtMixmode;    // packs into row 3 .w
    // Row 4
    float    fAnamFlareThreshold;
    float    fAnamFlareWideness;
    float    fAnamFlareAmount;
    float    fAnamFlareCurve;
    // Row 5
    float    fAnamFlareColor[3];
    int32_t  bAnamFlareEnable;    // packs into row 5 .w
    // Row 6
    float    fLenzIntensity;
    float    fLenzThreshold;
    int32_t  bLenzEnable;
    int32_t  bChapFlareEnable;
    // Row 7
    float    fChapFlareTreshold;
    int32_t  iChapFlareCount;
    float    fChapFlareDispersal;
    float    fChapFlareSize;
    // Row 8
    float    fChapFlareCA[3];
    float    fChapFlareIntensity; // packs into row 8 .w
    // Row 9
    int32_t  bGodrayEnable;
    float    fGodrayDecay;
    float    fGodrayWeight;
    float    fGodrayDensity;
    // Row 10
    float    fGodrayThreshold;
    float    fGodrayExposure;
    int32_t  iGodraySamples;
    float    fFlareLuminance;
    // Row 11
    float    fFlareBlur;
    float    fFlareIntensity;
    int32_t  bLensdirtEnable;
    float    _pad0;
    // Row 12
    float    fFlareTint[3];
    float    _pad1;
};
#pragma pack(pop)
static_assert(sizeof(BloomCB) == 13 * 16, "BloomCB must be 208 bytes (13 rows of 16)");

// ---------------------------------------------------------------------------
// HLSL — common preamble shared by all 10 passes. Each pass appends its own
// pixel shader entry point. cbuffer field order MUST match the C++ struct above.
// ---------------------------------------------------------------------------
#define BLOOM_HLSL_PREAMBLE R"(
cbuffer BloomCB : register(b0) {
    float2 PixelSize;
    float  AspectRatio;
    float  fBloomThreshold;
    float  fBloomAmount;
    float  fBloomSaturation;
    float  fLensdirtIntensity;
    float  fLensdirtSaturation;
    float3 fBloomTint;
    int    iBloomMixmode;
    float3 fLensdirtTint;
    int    iLensdirtMixmode;
    float  fAnamFlareThreshold;
    float  fAnamFlareWideness;
    float  fAnamFlareAmount;
    float  fAnamFlareCurve;
    float3 fAnamFlareColor;
    int    bAnamFlareEnable;
    float  fLenzIntensity;
    float  fLenzThreshold;
    int    bLenzEnable;
    int    bChapFlareEnable;
    float  fChapFlareTreshold;
    int    iChapFlareCount;
    float  fChapFlareDispersal;
    float  fChapFlareSize;
    float3 fChapFlareCA;
    float  fChapFlareIntensity;
    int    bGodrayEnable;
    float  fGodrayDecay;
    float  fGodrayWeight;
    float  fGodrayDensity;
    float  fGodrayThreshold;
    float  fGodrayExposure;
    int    iGodraySamples;
    float  fFlareLuminance;
    float  fFlareBlur;
    float  fFlareIntensity;
    int    bLensdirtEnable;
    float  _pad0;
    float3 fFlareTint;
    float  _pad1;
};
SamplerState LinearSampler : register(s0);
struct PSI { float4 P : SV_Position; float2 uv : TEXCOORD0; };

float4 GaussBlur22(float2 coord, Texture2D tex, float mult, float lodlevel, bool isBlurVert) {
    float4 sum = 0;
    float2 axis = isBlurVert ? float2(0,1) : float2(1,0);
    const float weight[11] = {
        0.082607, 0.080977, 0.076276, 0.069041, 0.060049,
        0.050187, 0.040306, 0.031105, 0.023066, 0.016436, 0.011254
    };
    [unroll] for (int i = -10; i < 11; i++) {
        float currweight = weight[abs(i)];
        sum += tex.SampleLevel(LinearSampler, coord + axis * (float)i * PixelSize * mult, lodlevel) * currweight;
    }
    return sum;
}
)"

// ---------------------------------------------------------------------------
// Pass 0: BloomPass0 — scene → Bloom1 (full res). Brightpass + 4-tap.
// Inputs: t0=Scene
// ---------------------------------------------------------------------------
static const char* g_ps_bloom0 = BLOOM_HLSL_PREAMBLE R"(
Texture2D Scene : register(t0);
float4 main(PSI i) : SV_Target {
    float4 bloom = 0.0;
    const float2 offset[4] = { float2(1,1), float2(1,1), float2(-1,1), float2(-1,-1) };
    [unroll] for (int n = 0; n < 4; n++) {
        float2 bloomuv = offset[n] * PixelSize * 2 + i.uv;
        float4 t = Scene.SampleLevel(LinearSampler, bloomuv, 0);
        t.w   = max(0, dot(t.xyz, 0.333) - fAnamFlareThreshold);
        t.xyz = max(0, t.xyz - fBloomThreshold);
        bloom += t;
    }
    return bloom * 0.25;
}
)";

// ---------------------------------------------------------------------------
// Pass 1: BloomPass1 — Bloom1 → Bloom2 (full res). 8-tap.
// Inputs: t0=Bloom1
// ---------------------------------------------------------------------------
static const char* g_ps_bloom1 = BLOOM_HLSL_PREAMBLE R"(
Texture2D Src : register(t0);
float4 main(PSI i) : SV_Target {
    float4 bloom = 0.0;
    const float2 offset[8] = {
        float2(1,1), float2(0,-1), float2(-1,1), float2(-1,-1),
        float2(0,1), float2(0,-1), float2(1,0), float2(-1,0)
    };
    [unroll] for (int n = 0; n < 8; n++) {
        float2 bloomuv = offset[n] * PixelSize * 4 + i.uv;
        bloom += Src.SampleLevel(LinearSampler, bloomuv, 0);
    }
    return bloom * 0.125;
}
)";

// ---------------------------------------------------------------------------
// Pass 2: BloomPass2 — Bloom2 → Bloom3 (/2 res). 8-tap.
// Inputs: t0=Bloom2
// ---------------------------------------------------------------------------
static const char* g_ps_bloom2 = BLOOM_HLSL_PREAMBLE R"(
Texture2D Src : register(t0);
float4 main(PSI i) : SV_Target {
    float4 bloom = 0.0;
    const float2 offset[8] = {
        float2(0.707, 0.707), float2(0.707,-0.707), float2(-0.707,0.707), float2(-0.707,-0.707),
        float2(0,1), float2(0,-1), float2(1,0), float2(-1,0)
    };
    [unroll] for (int n = 0; n < 8; n++) {
        float2 bloomuv = offset[n] * PixelSize * 8 + i.uv;
        bloom += Src.SampleLevel(LinearSampler, bloomuv, 0);
    }
    return bloom * 0.5;
}
)";

// ---------------------------------------------------------------------------
// Pass 3: BloomPass3 — Bloom3 → Bloom4 (/4 res). H-blur.
// Inputs: t0=Bloom3
// ---------------------------------------------------------------------------
static const char* g_ps_bloom3 = BLOOM_HLSL_PREAMBLE R"(
Texture2D Src : register(t0);
float4 main(PSI i) : SV_Target {
    float4 bloom = GaussBlur22(i.uv, Src, 16, 0, false);
    bloom.w   *= fAnamFlareAmount;
    bloom.xyz *= fBloomAmount;
    return bloom;
}
)";

// ---------------------------------------------------------------------------
// Pass 4: BloomPass4 — Bloom4 → Bloom5 (/8 res). V-blur xyz, H-blur w.
// Inputs: t0=Bloom4
// ---------------------------------------------------------------------------
static const char* g_ps_bloom4 = BLOOM_HLSL_PREAMBLE R"(
Texture2D Src : register(t0);
float4 main(PSI i) : SV_Target {
    float4 bloom;
    bloom.xyz = GaussBlur22(i.uv, Src, 16, 0, true).xyz * 2.5;
    bloom.w   = GaussBlur22(i.uv, Src, 32 * fAnamFlareWideness, 0, false).w * 2.5;
    return bloom;
}
)";

// ---------------------------------------------------------------------------
// Pass 5: LensFlarePass0 — Scene → LensFlare1 (/2 res).
// Lenz + Chapman + Godrays + AnamFlare bright-pass into one accumulator.
// Inputs: t0=Scene
// ---------------------------------------------------------------------------
static const char* g_ps_lensflare0 = BLOOM_HLSL_PREAMBLE R"(
Texture2D Scene : register(t0);

float3 GetDnB(float2 coords) {
    return max(0, dot(Scene.SampleLevel(LinearSampler, coords, 4).rgb, 0.333) - fChapFlareTreshold) * fChapFlareIntensity;
}
float3 GetDistortedTex(float2 sample_center, float2 sample_vector, float3 distortion) {
    float2 final_vector = sample_center + sample_vector * min(min(distortion.r, distortion.g), distortion.b);
    if (final_vector.x > 1.0 || final_vector.y > 1.0 || final_vector.x < -1.0 || final_vector.y < -1.0)
        return float3(0,0,0);
    return float3(
        GetDnB(sample_center + sample_vector * distortion.r).r,
        GetDnB(sample_center + sample_vector * distortion.g).g,
        GetDnB(sample_center + sample_vector * distortion.b).b);
}
float3 GetBrightPass(float2 coords) {
    float3 c  = Scene.Sample(LinearSampler, coords).rgb;
    float3 bC = max(c - fFlareLuminance.xxx, 0.0);
    float bright = dot(bC, 1.0);
    bright = smoothstep(0.0, 0.5, bright);
    return lerp(0.0, c, bright);
}
float3 GetAnamorphicSample(float2 coords, float blur) {
    coords = 2.0 * coords - 1.0;
    coords.x /= -blur;
    coords = 0.5 * coords + 0.5;
    return GetBrightPass(coords);
}

float4 main(PSI i) : SV_Target {
    float4 lens = 0;

    // Lenz
    if (bLenzEnable) {
        const float3 lfoffset[19] = {
            float3(0.9, 0.01, 4),    float3(0.7, 0.25, 25),  float3(0.3, 0.25, 15),
            float3(1, 1.0, 5),       float3(-0.15, 20, 1),   float3(-0.3, 20, 1),
            float3(6, 6, 6),         float3(7, 7, 7),        float3(8, 8, 8),
            float3(9, 9, 9),         float3(0.24, 1, 10),    float3(0.32, 1, 10),
            float3(0.4, 1, 10),      float3(0.5, -0.5, 2),   float3(2, 2, -5),
            float3(-5, 0.2, 0.2),    float3(20, 0.5, 0),     float3(0.4, 1, 10),
            float3(0.00001, 10, 20)
        };
        const float3 lffactors[19] = {
            float3(1.5, 1.5, 0),     float3(0, 1.5, 0),      float3(0, 0, 1.5),
            float3(0.2, 0.25, 0),    float3(0.15, 0, 0),     float3(0, 0, 0.15),
            float3(1.4, 0, 0),       float3(1, 1, 0),        float3(0, 1, 0),
            float3(0, 0, 1.4),       float3(1, 0.3, 0),      float3(1, 1, 0),
            float3(0, 2, 4),         float3(0.2, 0.1, 0),    float3(0, 0, 1),
            float3(1, 1, 0),         float3(1, 1, 0),        float3(0, 0, 0.2),
            float3(0.012, 0.313, 0.588)
        };
        float2 lfcoord = 0;
        float3 lenstemp = 0;
        float2 distfact = i.uv - 0.5;
        distfact.x *= AspectRatio;
        [unroll] for (int n = 0; n < 19; n++) {
            lfcoord = lfoffset[n].x * distfact;
            lfcoord *= pow(2.0 * length(distfact), lfoffset[n].y * 3.5);
            lfcoord *= lfoffset[n].z;
            lfcoord = 0.5 - lfcoord;
            float2 tempfact = (lfcoord - 0.5) * 2;
            float templensmult = clamp(1.0 - dot(tempfact, tempfact), 0, 1);
            float3 lt = dot(Scene.SampleLevel(LinearSampler, lfcoord, 1).rgb, 0.333);
            lt = max(0, lt - fLenzThreshold);
            lt *= lffactors[n] * templensmult;
            lenstemp += lt;
        }
        lens.rgb += lenstemp * fLenzIntensity;
    }

    // Chapman Lens
    if (bChapFlareEnable) {
        float2 sample_vector = (float2(0.5, 0.5) - i.uv) * fChapFlareDispersal;
        float2 halo_vector   = normalize(sample_vector) * fChapFlareSize;
        float3 chaplens = GetDistortedTex(i.uv + halo_vector, halo_vector, fChapFlareCA * 2.5);
        for (int j = 0; j < iChapFlareCount; ++j) {
            float2 foffset = sample_vector * float(j);
            chaplens += GetDistortedTex(i.uv + foffset, foffset, fChapFlareCA);
        }
        chaplens *= 1.0 / iChapFlareCount;
        lens.xyz += chaplens;
    }

    // Godrays
    if (bGodrayEnable) {
        const float2 ScreenLightPos = float2(0.5, 0.5);
        float2 texcoord2     = i.uv;
        float2 deltaTexCoord = (texcoord2 - ScreenLightPos);
        deltaTexCoord *= 1.0 / (float)iGodraySamples * fGodrayDensity;
        float illuminationDecay = 1.0;
        for (int g = 0; g < iGodraySamples; g++) {
            texcoord2 -= deltaTexCoord;
            float4 sample2 = Scene.SampleLevel(LinearSampler, texcoord2, 0);
            sample2.w  = saturate(dot(sample2.xyz, 0.3333) - fGodrayThreshold);
            sample2.r *= 1.00;
            sample2.g *= 0.95;
            sample2.b *= 0.85;
            sample2   *= illuminationDecay * fGodrayWeight;
            lens.rgb += sample2.xyz * sample2.w;
            illuminationDecay *= fGodrayDecay;
        }
    }

    // Anamorphic flare
    if (bAnamFlareEnable) {
        float3 anamFlare = 0;
        const float gw[5] = { 0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162 };
        [unroll] for (int z = -4; z < 5; z++) {
            anamFlare += GetAnamorphicSample(i.uv + float2(0, z * PixelSize.y * 2), fFlareBlur)
                         * fFlareTint * gw[abs(z)];
        }
        lens.xyz += anamFlare * fFlareIntensity;
    }
    return lens;
}
)";

// ---------------------------------------------------------------------------
// Pass 6: LensFlarePass1 — LensFlare1 → LensFlare2. V-blur.
// Inputs: t0=LensFlare1
// ---------------------------------------------------------------------------
static const char* g_ps_lensflare1 = BLOOM_HLSL_PREAMBLE R"(
Texture2D Src : register(t0);
float4 main(PSI i) : SV_Target {
    return GaussBlur22(i.uv, Src, 2, 0, true);
}
)";

// ---------------------------------------------------------------------------
// Pass 7: LensFlarePass2 — LensFlare2 → LensFlare1. H-blur.
// Inputs: t0=LensFlare2
// ---------------------------------------------------------------------------
static const char* g_ps_lensflare2 = BLOOM_HLSL_PREAMBLE R"(
Texture2D Src : register(t0);
float4 main(PSI i) : SV_Target {
    return GaussBlur22(i.uv, Src, 2, 0, false);
}
)";

// ---------------------------------------------------------------------------
// Pass 8: LightingCombine — combines everything back into Scene.
// Inputs: t0=Scene, t1=Bloom3, t2=Bloom5, t3=Dirt, t4=Sprite, t5=LensFlare1
// ---------------------------------------------------------------------------
static const char* g_ps_combine = BLOOM_HLSL_PREAMBLE R"(
Texture2D Scene      : register(t0);
Texture2D BloomMid   : register(t1);  // Bloom3 (/2)
Texture2D BloomFar   : register(t2);  // Bloom5 (/8)
Texture2D Dirt       : register(t3);
Texture2D Sprite     : register(t4);
Texture2D LensFlare  : register(t5);

float4 main(PSI i) : SV_Target {
    float4 color = Scene.Sample(LinearSampler, i.uv);

    // Bloom
    float3 colorbloom = 0;
    colorbloom += BloomMid.Sample(LinearSampler, i.uv).rgb * 1.0;
    colorbloom += BloomFar.Sample(LinearSampler, i.uv).rgb * 9.0;
    colorbloom *= 0.1;
    colorbloom = saturate(colorbloom);
    float colorbloomgray = dot(colorbloom, 0.333);
    colorbloom = lerp(colorbloomgray, colorbloom, fBloomSaturation);
    colorbloom *= fBloomTint;

    if (iBloomMixmode == 0)      color.rgb += colorbloom;
    else if (iBloomMixmode == 1) color.rgb = 1 - (1 - color.rgb) * (1 - colorbloom);
    else if (iBloomMixmode == 2) color.rgb = max(0.0, max(color.rgb,
        lerp(color.rgb, (1 - (1 - saturate(colorbloom)) * (1 - saturate(colorbloom))), 1.0)));
    else if (iBloomMixmode == 3) color.rgb = max(color.rgb, colorbloom);

    // Anamorphic flare
    if (bAnamFlareEnable) {
        float3 anamflare = BloomFar.Sample(LinearSampler, i.uv).w * 2 * fAnamFlareColor;
        anamflare = max(anamflare, 0.0);
        color.rgb += pow(anamflare, 1.0 / fAnamFlareCurve);
    }

    // Lens dirt
    if (bLensdirtEnable) {
        float lensdirtmult = dot(BloomFar.Sample(LinearSampler, i.uv).rgb, 0.333);
        float3 dirttex = Dirt.Sample(LinearSampler, i.uv).rgb;
        float3 lensdirt = dirttex * lensdirtmult * fLensdirtIntensity;
        lensdirt = lerp(dot(lensdirt.xyz, 0.333), lensdirt.xyz, fLensdirtSaturation);
        if (iLensdirtMixmode == 0)      color.rgb += lensdirt;
        else if (iLensdirtMixmode == 1) color.rgb = 1 - (1 - color.rgb) * (1 - lensdirt);
        else if (iLensdirtMixmode == 2) color.rgb = max(0.0, max(color.rgb,
            lerp(color.rgb, (1 - (1 - saturate(lensdirt)) * (1 - saturate(lensdirt))), 1.0)));
        else if (iLensdirtMixmode == 3) color.rgb = max(color.rgb, lensdirt);
    }

    // Lens flares
    if (bAnamFlareEnable || bLenzEnable || bGodrayEnable || bChapFlareEnable) {
        float3 lensflareSample = LensFlare.Sample(LinearSampler, i.uv).rgb;
        float3 lensflareMask;
        lensflareMask  = Sprite.Sample(LinearSampler, i.uv + float2( 0.5,  0.5) * PixelSize).rgb;
        lensflareMask += Sprite.Sample(LinearSampler, i.uv + float2(-0.5,  0.5) * PixelSize).rgb;
        lensflareMask += Sprite.Sample(LinearSampler, i.uv + float2( 0.5, -0.5) * PixelSize).rgb;
        lensflareMask += Sprite.Sample(LinearSampler, i.uv + float2(-0.5, -0.5) * PixelSize).rgb;
        color.rgb += lensflareMask * 0.25 * lensflareSample;
    }
    return color;
}
)";

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------
class BloomPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool m_enabled = false;

    // Bloom group
    int   m_iBloomMixmode      = 2;
    float m_fBloomThreshold    = 0.8f;
    float m_fBloomAmount       = 0.8f;
    float m_fBloomSaturation   = 0.8f;
    float m_fBloomTint[3]      = { 0.7f, 0.8f, 1.0f };

    // Lens dirt group
    bool  m_bLensdirtEnable     = false;
    int   m_iLensdirtMixmode    = 1;
    float m_fLensdirtIntensity  = 0.4f;
    float m_fLensdirtSaturation = 2.0f;
    float m_fLensdirtTint[3]    = { 1.0f, 1.0f, 1.0f };

    // Anamorphic flare group
    bool  m_bAnamFlareEnable    = false;
    float m_fAnamFlareThreshold = 0.9f;
    float m_fAnamFlareWideness  = 2.4f;
    float m_fAnamFlareAmount    = 14.5f;
    float m_fAnamFlareCurve     = 1.2f;
    float m_fAnamFlareColor[3]  = { 0.012f, 0.313f, 0.588f };

    // Lenz group
    bool  m_bLenzEnable     = false;
    float m_fLenzIntensity  = 1.0f;
    float m_fLenzThreshold  = 0.8f;

    // Chapman group
    bool  m_bChapFlareEnable    = false;
    float m_fChapFlareTreshold  = 0.90f;
    int   m_iChapFlareCount     = 15;
    float m_fChapFlareDispersal = 0.25f;
    float m_fChapFlareSize      = 0.45f;
    float m_fChapFlareCA[3]     = { 0.00f, 0.01f, 0.02f };
    float m_fChapFlareIntensity = 100.0f;

    // Godray group
    bool  m_bGodrayEnable    = false;
    float m_fGodrayDecay     = 0.99f;
    float m_fGodrayExposure  = 1.0f;
    float m_fGodrayWeight    = 1.25f;
    float m_fGodrayDensity   = 1.0f;
    float m_fGodrayThreshold = 0.9f;
    int   m_iGodraySamples   = 128;

    // Flare group
    float m_fFlareLuminance = 0.095f;
    float m_fFlareBlur      = 200.0f;
    float m_fFlareIntensity = 2.07f;
    float m_fFlareTint[3]   = { 0.137f, 0.216f, 1.0f };

    BloomCB           m_cb{};
    fx::EffectRuntime m_runtime;
    int               m_dirt_id   = -1;
    int               m_sprite_id = -1;
    bool              m_passes_set = false;

    // RT ids returned by declare_rt
    int m_bloom1 = -1, m_bloom2 = -1, m_bloom3 = -1, m_bloom4 = -1, m_bloom5 = -1;
    int m_lens1  = -1, m_lens2  = -1;

    void on_initialize() override {
        API::get()->log_info("[Bloom] Plugin initialized (v%s)", BLOOM_VERSION);
        configure_runtime();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "Bloom"; }
    int render_order() const override { return 2000; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",                m_enabled            ? "1" : "0"},
            {"bloom_mixmode",          std::to_string(m_iBloomMixmode)},
            {"bloom_threshold",        std::to_string(m_fBloomThreshold)},
            {"bloom_amount",           std::to_string(m_fBloomAmount)},
            {"bloom_saturation",       std::to_string(m_fBloomSaturation)},
            {"bloom_tint_r",           std::to_string(m_fBloomTint[0])},
            {"bloom_tint_g",           std::to_string(m_fBloomTint[1])},
            {"bloom_tint_b",           std::to_string(m_fBloomTint[2])},
            {"lensdirt_enable",        m_bLensdirtEnable    ? "1" : "0"},
            {"lensdirt_mixmode",       std::to_string(m_iLensdirtMixmode)},
            {"lensdirt_intensity",     std::to_string(m_fLensdirtIntensity)},
            {"lensdirt_saturation",    std::to_string(m_fLensdirtSaturation)},
            {"lensdirt_tint_r",        std::to_string(m_fLensdirtTint[0])},
            {"lensdirt_tint_g",        std::to_string(m_fLensdirtTint[1])},
            {"lensdirt_tint_b",        std::to_string(m_fLensdirtTint[2])},
            {"anam_flare_enable",      m_bAnamFlareEnable   ? "1" : "0"},
            {"anam_flare_threshold",   std::to_string(m_fAnamFlareThreshold)},
            {"anam_flare_wideness",    std::to_string(m_fAnamFlareWideness)},
            {"anam_flare_amount",      std::to_string(m_fAnamFlareAmount)},
            {"anam_flare_curve",       std::to_string(m_fAnamFlareCurve)},
            {"anam_flare_color_r",     std::to_string(m_fAnamFlareColor[0])},
            {"anam_flare_color_g",     std::to_string(m_fAnamFlareColor[1])},
            {"anam_flare_color_b",     std::to_string(m_fAnamFlareColor[2])},
            {"lenz_enable",            m_bLenzEnable        ? "1" : "0"},
            {"lenz_intensity",         std::to_string(m_fLenzIntensity)},
            {"lenz_threshold",         std::to_string(m_fLenzThreshold)},
            {"chap_flare_enable",      m_bChapFlareEnable   ? "1" : "0"},
            {"chap_flare_threshold",   std::to_string(m_fChapFlareTreshold)},
            {"chap_flare_count",       std::to_string(m_iChapFlareCount)},
            {"chap_flare_dispersal",   std::to_string(m_fChapFlareDispersal)},
            {"chap_flare_size",        std::to_string(m_fChapFlareSize)},
            {"chap_flare_ca_r",        std::to_string(m_fChapFlareCA[0])},
            {"chap_flare_ca_g",        std::to_string(m_fChapFlareCA[1])},
            {"chap_flare_ca_b",        std::to_string(m_fChapFlareCA[2])},
            {"chap_flare_intensity",   std::to_string(m_fChapFlareIntensity)},
            {"godray_enable",          m_bGodrayEnable      ? "1" : "0"},
            {"godray_decay",           std::to_string(m_fGodrayDecay)},
            {"godray_exposure",        std::to_string(m_fGodrayExposure)},
            {"godray_weight",          std::to_string(m_fGodrayWeight)},
            {"godray_density",         std::to_string(m_fGodrayDensity)},
            {"godray_threshold",       std::to_string(m_fGodrayThreshold)},
            {"godray_samples",         std::to_string(m_iGodraySamples)},
            {"flare_luminance",        std::to_string(m_fFlareLuminance)},
            {"flare_blur",             std::to_string(m_fFlareBlur)},
            {"flare_intensity",        std::to_string(m_fFlareIntensity)},
            {"flare_tint_r",           std::to_string(m_fFlareTint[0])},
            {"flare_tint_g",           std::to_string(m_fFlareTint[1])},
            {"flare_tint_b",           std::to_string(m_fFlareTint[2])},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto get_float = [&](const char* k, float& out) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { out = std::stof(it->second); } catch (...) {}
        };
        auto get_int = [&](const char* k, int& out) {
            auto it = kv.find(k);
            if (it == kv.end()) return;
            try { out = std::stoi(it->second); } catch (...) {}
        };
        auto get_bool = [&](const char* k, bool& out) {
            auto it = kv.find(k);
            if (it != kv.end()) out = (it->second != "0" && !it->second.empty());
        };
        get_bool("enabled",                m_enabled);
        get_int("bloom_mixmode",           m_iBloomMixmode);
        get_float("bloom_threshold",       m_fBloomThreshold);
        get_float("bloom_amount",          m_fBloomAmount);
        get_float("bloom_saturation",      m_fBloomSaturation);
        get_float("bloom_tint_r",          m_fBloomTint[0]);
        get_float("bloom_tint_g",          m_fBloomTint[1]);
        get_float("bloom_tint_b",          m_fBloomTint[2]);
        get_bool("lensdirt_enable",        m_bLensdirtEnable);
        get_int("lensdirt_mixmode",        m_iLensdirtMixmode);
        get_float("lensdirt_intensity",    m_fLensdirtIntensity);
        get_float("lensdirt_saturation",   m_fLensdirtSaturation);
        get_float("lensdirt_tint_r",       m_fLensdirtTint[0]);
        get_float("lensdirt_tint_g",       m_fLensdirtTint[1]);
        get_float("lensdirt_tint_b",       m_fLensdirtTint[2]);
        get_bool("anam_flare_enable",      m_bAnamFlareEnable);
        get_float("anam_flare_threshold",  m_fAnamFlareThreshold);
        get_float("anam_flare_wideness",   m_fAnamFlareWideness);
        get_float("anam_flare_amount",     m_fAnamFlareAmount);
        get_float("anam_flare_curve",      m_fAnamFlareCurve);
        get_float("anam_flare_color_r",    m_fAnamFlareColor[0]);
        get_float("anam_flare_color_g",    m_fAnamFlareColor[1]);
        get_float("anam_flare_color_b",    m_fAnamFlareColor[2]);
        get_bool("lenz_enable",            m_bLenzEnable);
        get_float("lenz_intensity",        m_fLenzIntensity);
        get_float("lenz_threshold",        m_fLenzThreshold);
        get_bool("chap_flare_enable",      m_bChapFlareEnable);
        get_float("chap_flare_threshold",  m_fChapFlareTreshold);
        get_int("chap_flare_count",        m_iChapFlareCount);
        get_float("chap_flare_dispersal",  m_fChapFlareDispersal);
        get_float("chap_flare_size",       m_fChapFlareSize);
        get_float("chap_flare_ca_r",       m_fChapFlareCA[0]);
        get_float("chap_flare_ca_g",       m_fChapFlareCA[1]);
        get_float("chap_flare_ca_b",       m_fChapFlareCA[2]);
        get_float("chap_flare_intensity",  m_fChapFlareIntensity);
        get_bool("godray_enable",          m_bGodrayEnable);
        get_float("godray_decay",          m_fGodrayDecay);
        get_float("godray_exposure",       m_fGodrayExposure);
        get_float("godray_weight",         m_fGodrayWeight);
        get_float("godray_density",        m_fGodrayDensity);
        get_float("godray_threshold",      m_fGodrayThreshold);
        get_int("godray_samples",          m_iGodraySamples);
        get_float("flare_luminance",       m_fFlareLuminance);
        get_float("flare_blur",            m_fFlareBlur);
        get_float("flare_intensity",       m_fFlareIntensity);
        get_float("flare_tint_r",          m_fFlareTint[0]);
        get_float("flare_tint_g",          m_fFlareTint[1]);
        get_float("flare_tint_b",          m_fFlareTint[2]);
    }

    void reset_to_defaults() override {
        m_enabled              = false;
        m_iBloomMixmode        = 2;
        m_fBloomThreshold      = 0.8f;
        m_fBloomAmount         = 0.8f;
        m_fBloomSaturation     = 0.8f;
        m_fBloomTint[0]        = 0.7f; m_fBloomTint[1] = 0.8f; m_fBloomTint[2] = 1.0f;
        m_bLensdirtEnable      = false;
        m_iLensdirtMixmode     = 1;
        m_fLensdirtIntensity   = 0.4f;
        m_fLensdirtSaturation  = 2.0f;
        m_fLensdirtTint[0]     = 1.0f; m_fLensdirtTint[1] = 1.0f; m_fLensdirtTint[2] = 1.0f;
        m_bAnamFlareEnable     = false;
        m_fAnamFlareThreshold  = 0.9f;
        m_fAnamFlareWideness   = 2.4f;
        m_fAnamFlareAmount     = 14.5f;
        m_fAnamFlareCurve      = 1.2f;
        m_fAnamFlareColor[0]   = 0.012f; m_fAnamFlareColor[1] = 0.313f; m_fAnamFlareColor[2] = 0.588f;
        m_bLenzEnable          = false;
        m_fLenzIntensity       = 1.0f;
        m_fLenzThreshold       = 0.8f;
        m_bChapFlareEnable     = false;
        m_fChapFlareTreshold   = 0.90f;
        m_iChapFlareCount      = 15;
        m_fChapFlareDispersal  = 0.25f;
        m_fChapFlareSize       = 0.45f;
        m_fChapFlareCA[0]      = 0.00f; m_fChapFlareCA[1] = 0.01f; m_fChapFlareCA[2] = 0.02f;
        m_fChapFlareIntensity  = 100.0f;
        m_bGodrayEnable        = false;
        m_fGodrayDecay         = 0.99f;
        m_fGodrayExposure      = 1.0f;
        m_fGodrayWeight        = 1.25f;
        m_fGodrayDensity       = 1.0f;
        m_fGodrayThreshold     = 0.9f;
        m_iGodraySamples       = 128;
        m_fFlareLuminance      = 0.095f;
        m_fFlareBlur           = 200.0f;
        m_fFlareIntensity      = 2.07f;
        m_fFlareTint[0]        = 0.137f; m_fFlareTint[1] = 0.216f; m_fFlareTint[2] = 1.0f;
    }
    // ----------------------------------------------------------------------

    void configure_runtime() {
        if (m_passes_set) return;

        // 7 intermediate render targets, all RGBA16F.
        fx::RTDesc full{};      full.size_mode = fx::RTDesc::SizeMode::Backbuffer;
        fx::RTDesc half{};      half.size_mode = fx::RTDesc::SizeMode::BackbufferDiv; half.w_or_div = 2; half.h_or_div = 2;
        fx::RTDesc quart{};     quart.size_mode = fx::RTDesc::SizeMode::BackbufferDiv; quart.w_or_div = 4; quart.h_or_div = 4;
        fx::RTDesc eighth{};    eighth.size_mode = fx::RTDesc::SizeMode::BackbufferDiv; eighth.w_or_div = 8; eighth.h_or_div = 8;

        m_bloom1 = m_runtime.declare_rt(full);
        m_bloom2 = m_runtime.declare_rt(full);
        m_bloom3 = m_runtime.declare_rt(half);
        m_bloom4 = m_runtime.declare_rt(quart);
        m_bloom5 = m_runtime.declare_rt(eighth);
        m_lens1  = m_runtime.declare_rt(half);
        m_lens2  = m_runtime.declare_rt(half);

        // Snapshot scene with 5 mips so Lenz/Chapman/AnamFlare can sample at LOD 1/4.
        m_runtime.request_scene_snapshot_mips(5);

        // External textures shipped under examples/bloom_plugin/assets/.
        // resolve_shader_asset_path searches per-game shader_settings/ first, then global shader_assets/.
        auto dirt_path   = resolve_shader_asset_path(L"LensDB.png");
        auto sprite_path = resolve_shader_asset_path(L"LensSprite.png");
        if (!dirt_path.empty())   m_dirt_id   = m_runtime.load_external_texture_png(dirt_path);
        if (!sprite_path.empty()) m_sprite_id = m_runtime.load_external_texture_png(sprite_path);

        std::vector<fx::PassDesc> passes;
        passes.reserve(10);

        auto make_pass = [&](const char* hlsl, std::vector<int> inputs, int output) {
            fx::PassDesc p;
            p.ps_hlsl = hlsl;
            p.inputs  = std::move(inputs);
            p.output  = output;
            p.cb_data = &m_cb;
            p.cb_size = sizeof(m_cb);
            passes.push_back(std::move(p));
        };

        make_pass(g_ps_bloom0,     { fx::INPUT_SCENE },                                                        m_bloom1);
        make_pass(g_ps_bloom1,     { m_bloom1 },                                                                m_bloom2);
        make_pass(g_ps_bloom2,     { m_bloom2 },                                                                m_bloom3);
        make_pass(g_ps_bloom3,     { m_bloom3 },                                                                m_bloom4);
        make_pass(g_ps_bloom4,     { m_bloom4 },                                                                m_bloom5);
        make_pass(g_ps_lensflare0, { fx::INPUT_SCENE },                                                        m_lens1);
        make_pass(g_ps_lensflare1, { m_lens1 },                                                                 m_lens2);
        make_pass(g_ps_lensflare2, { m_lens2 },                                                                 m_lens1);
        make_pass(g_ps_combine,    { fx::INPUT_SCENE, m_bloom3, m_bloom5, m_dirt_id, m_sprite_id, m_lens1 },   fx::OUTPUT_SCENE);

        m_runtime.set_passes(std::move(passes));
        m_passes_set = true;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Bloom + Lens Flares", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s — Marty McFly Bloom.fx port", BLOOM_VERSION);
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                "EXPERIMENTAL — does not work in VR (LDR pipeline, no HDR source values to bloom from).");
            fx::draw_scene_rt_colorspace_warning();

            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##Bloom", &m_enabled);

            if (ImGui::TreeNodeEx("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* mix_items = "Linear add\0Screen add\0Screen/Lighten/Opacity\0Lighten\0";
                changed |= ImGui::Combo("Mix Mode##Bloom", &m_iBloomMixmode, mix_items);
                changed |= ImGui::DragFloat("Threshold##Bloom",  &m_fBloomThreshold,  0.001f, 0.1f, 1.0f);
                changed |= ImGui::DragFloat("Amount##Bloom",     &m_fBloomAmount,     0.01f,  0.0f, 20.0f);
                changed |= ImGui::DragFloat("Saturation##Bloom", &m_fBloomSaturation, 0.01f,  0.0f, 2.0f);
                changed |= ImGui::ColorEdit3("Tint##Bloom", m_fBloomTint);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Lens Dirt")) {
                changed |= ImGui::Checkbox("Enable##Lensdirt", &m_bLensdirtEnable);
                const char* mix_items = "Linear add\0Screen add\0Screen/Lighten/Opacity\0Lighten\0";
                changed |= ImGui::Combo("Mix Mode##Lensdirt", &m_iLensdirtMixmode, mix_items);
                changed |= ImGui::DragFloat("Intensity##Lensdirt",  &m_fLensdirtIntensity,  0.01f, 0.0f, 2.0f);
                changed |= ImGui::DragFloat("Saturation##Lensdirt", &m_fLensdirtSaturation, 0.01f, 0.0f, 2.0f);
                changed |= ImGui::ColorEdit3("Tint##Lensdirt", m_fLensdirtTint);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Anamorphic Flare")) {
                changed |= ImGui::Checkbox("Enable##Anam", &m_bAnamFlareEnable);
                changed |= ImGui::DragFloat("Threshold##Anam", &m_fAnamFlareThreshold, 0.001f, 0.1f, 1.0f);
                changed |= ImGui::DragFloat("Wideness##Anam",  &m_fAnamFlareWideness,  0.01f,  1.0f, 2.5f);
                changed |= ImGui::DragFloat("Amount##Anam",    &m_fAnamFlareAmount,    0.1f,   1.0f, 20.0f);
                changed |= ImGui::DragFloat("Curve##Anam",     &m_fAnamFlareCurve,     0.01f,  1.0f, 2.0f);
                changed |= ImGui::ColorEdit3("Color##Anam", m_fAnamFlareColor);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Lenz")) {
                changed |= ImGui::Checkbox("Enable##Lenz", &m_bLenzEnable);
                changed |= ImGui::DragFloat("Intensity##Lenz", &m_fLenzIntensity, 0.01f, 0.2f, 3.0f);
                changed |= ImGui::DragFloat("Threshold##Lenz", &m_fLenzThreshold, 0.01f, 0.6f, 1.0f);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Chapman Flare")) {
                changed |= ImGui::Checkbox("Enable##Chap", &m_bChapFlareEnable);
                changed |= ImGui::DragFloat("Threshold##Chap",  &m_fChapFlareTreshold,  0.001f, 0.7f,  0.99f);
                changed |= ImGui::DragInt("Count##Chap",        &m_iChapFlareCount,     1.0f,   1,     20);
                changed |= ImGui::DragFloat("Dispersal##Chap",  &m_fChapFlareDispersal, 0.01f,  0.25f, 1.0f);
                changed |= ImGui::DragFloat("Size##Chap",       &m_fChapFlareSize,      0.01f,  0.20f, 0.80f);
                changed |= ImGui::DragFloat3("Chromatic Ab##Chap", m_fChapFlareCA, 0.001f, -0.5f, 0.5f);
                changed |= ImGui::DragFloat("Intensity##Chap",  &m_fChapFlareIntensity, 0.5f,   5.0f,  200.0f);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Godrays")) {
                changed |= ImGui::Checkbox("Enable##Godray", &m_bGodrayEnable);
                changed |= ImGui::DragFloat("Decay##Godray",     &m_fGodrayDecay,     0.0001f, 0.5f, 0.9999f, "%.4f");
                changed |= ImGui::DragFloat("Exposure##Godray",  &m_fGodrayExposure,  0.01f,   0.7f, 1.5f);
                changed |= ImGui::DragFloat("Weight##Godray",    &m_fGodrayWeight,    0.01f,   0.8f, 1.7f);
                changed |= ImGui::DragFloat("Density##Godray",   &m_fGodrayDensity,   0.01f,   0.2f, 2.0f);
                changed |= ImGui::DragFloat("Threshold##Godray", &m_fGodrayThreshold, 0.01f,   0.6f, 1.0f);
                changed |= ImGui::DragInt("Samples##Godray",     &m_iGodraySamples,   1.0f,    8,    256);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Flare (AnamSamples)")) {
                changed |= ImGui::DragFloat("Luminance##Flare", &m_fFlareLuminance, 0.001f, 0.0f,  1.0f);
                changed |= ImGui::DragFloat("Blur##Flare",      &m_fFlareBlur,      1.0f,   1.0f,  10000.0f);
                changed |= ImGui::DragFloat("Intensity##Flare", &m_fFlareIntensity, 0.01f,  0.20f, 5.0f);
                changed |= ImGui::ColorEdit3("Tint##Flare", m_fFlareTint);
                ImGui::TreePop();
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void update_cb() {
        const unsigned w = fx::EffectRuntime::scene_width();
        const unsigned h = fx::EffectRuntime::scene_height();
        m_cb.PixelSize[0]   = (w > 0) ? 1.0f / static_cast<float>(w) : 0.0f;
        m_cb.PixelSize[1]   = (h > 0) ? 1.0f / static_cast<float>(h) : 0.0f;
        m_cb.AspectRatio    = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
        m_cb.fBloomThreshold    = m_fBloomThreshold;
        m_cb.fBloomAmount       = m_fBloomAmount;
        m_cb.fBloomSaturation   = m_fBloomSaturation;
        m_cb.fLensdirtIntensity = m_fLensdirtIntensity;
        m_cb.fLensdirtSaturation= m_fLensdirtSaturation;
        m_cb.fBloomTint[0]      = m_fBloomTint[0];
        m_cb.fBloomTint[1]      = m_fBloomTint[1];
        m_cb.fBloomTint[2]      = m_fBloomTint[2];
        m_cb.iBloomMixmode      = m_iBloomMixmode;
        m_cb.fLensdirtTint[0]   = m_fLensdirtTint[0];
        m_cb.fLensdirtTint[1]   = m_fLensdirtTint[1];
        m_cb.fLensdirtTint[2]   = m_fLensdirtTint[2];
        m_cb.iLensdirtMixmode   = m_iLensdirtMixmode;
        m_cb.fAnamFlareThreshold= m_fAnamFlareThreshold;
        m_cb.fAnamFlareWideness = m_fAnamFlareWideness;
        m_cb.fAnamFlareAmount   = m_fAnamFlareAmount;
        m_cb.fAnamFlareCurve    = m_fAnamFlareCurve;
        m_cb.fAnamFlareColor[0] = m_fAnamFlareColor[0];
        m_cb.fAnamFlareColor[1] = m_fAnamFlareColor[1];
        m_cb.fAnamFlareColor[2] = m_fAnamFlareColor[2];
        m_cb.bAnamFlareEnable   = m_bAnamFlareEnable ? 1 : 0;
        m_cb.fLenzIntensity     = m_fLenzIntensity;
        m_cb.fLenzThreshold     = m_fLenzThreshold;
        m_cb.bLenzEnable        = m_bLenzEnable ? 1 : 0;
        m_cb.bChapFlareEnable   = m_bChapFlareEnable ? 1 : 0;
        m_cb.fChapFlareTreshold = m_fChapFlareTreshold;
        m_cb.iChapFlareCount    = m_iChapFlareCount;
        m_cb.fChapFlareDispersal= m_fChapFlareDispersal;
        m_cb.fChapFlareSize     = m_fChapFlareSize;
        m_cb.fChapFlareCA[0]    = m_fChapFlareCA[0];
        m_cb.fChapFlareCA[1]    = m_fChapFlareCA[1];
        m_cb.fChapFlareCA[2]    = m_fChapFlareCA[2];
        m_cb.fChapFlareIntensity= m_fChapFlareIntensity;
        m_cb.bGodrayEnable      = m_bGodrayEnable ? 1 : 0;
        m_cb.fGodrayDecay       = m_fGodrayDecay;
        m_cb.fGodrayWeight      = m_fGodrayWeight;
        m_cb.fGodrayDensity     = m_fGodrayDensity;
        m_cb.fGodrayThreshold   = m_fGodrayThreshold;
        m_cb.fGodrayExposure    = m_fGodrayExposure;
        m_cb.iGodraySamples     = m_iGodraySamples;
        m_cb.fFlareLuminance    = m_fFlareLuminance;
        m_cb.fFlareBlur         = m_fFlareBlur;
        m_cb.fFlareIntensity    = m_fFlareIntensity;
        m_cb.bLensdirtEnable    = m_bLensdirtEnable ? 1 : 0;
        m_cb.fFlareTint[0]      = m_fFlareTint[0];
        m_cb.fFlareTint[1]      = m_fFlareTint[1];
        m_cb.fFlareTint[2]      = m_fFlareTint[2];
    }

    void run() {
        if (!m_enabled) return;
        if (!m_passes_set) configure_runtime();
        if (!m_passes_set) return;
        update_cb();
        m_runtime.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_runtime.release_resources(); }
};

std::unique_ptr<BloomPlugin> g_plugin{ new BloomPlugin() };
