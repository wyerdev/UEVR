/*
FGFXLargeScalePerceptualObscuranceIrradiance Plugin for UEVR
============================================================

Faithful port of Alex Tuduran's FGFXLargeScalePerceptualObscuranceIrradiance.fx
v0.7.1 default configuration to UEVR's effect runtime.

Original shader:
    FGFXLargeScalePerceptualObscuranceIrradiance - Large Scale Perceptual Obscurance and Irradiance
  Source: https://github.com/AlexTuduran/FGFX/blob/main/Shaders/FGFXLargeScalePerceptualObscuranceIrradiance.fx
  License: MIT (per FGFX repository LICENSE)

Source faithfulness notes:
  - Default preprocessor configuration is preserved: LSPOIRR_AUTO_GAIN_ENABLED=1,
    LSPOIRR_CASCADE_3_ON=0, LSPOIRR_SRGB=0, and custom neutral point disabled.
  - ReShade texture sizes using BUFFER_WIDTH >> N map to EffectRuntime
    BackbufferDiv targets with exact integer division by 2, 4, 8, and 16.
  - The technique pass order is preserved exactly for the enabled default passes.
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

using namespace uevr;

static constexpr const char* LSPOIRR_VERSION = "0.7.1";

static constexpr bool  DEFAULT_EFFECT_ENABLED = true;
static constexpr float DEFAULT_EFFECT_INTENSITY = 0.9f;
static constexpr float DEFAULT_OCCLUSION_INTENSITY = 1.0f;
static constexpr float DEFAULT_IRRADIANCE_INTENSITY = 1.0f;
static constexpr float DEFAULT_OCCLUSION_IRRADIANCE_THRESHOLD = 0.5f;
static constexpr float DEFAULT_EFFECT_RADIUS = 0.65f;
static constexpr float DEFAULT_EFFECT_SATURATION = 0.1f;
static constexpr float DEFAULT_OCCLUSION_IRRADIANCE_RECOVERY = 0.75f;
static constexpr float DEFAULT_AUTO_GAIN = 0.5f;
static constexpr float DEFAULT_GAMMA = 1.0f;
static constexpr float DEFAULT_GAIN = 1.0f;
static constexpr float DEFAULT_CONTRAST = 1.0f;
static constexpr float DEFAULT_SATURATION = 1.0f;
static constexpr int   DEFAULT_DEBUG_TYPE = 0;

static constexpr float OLED_EFFECT_INTENSITY = 0.30f;
static constexpr float OLED_OCCLUSION_INTENSITY = 0.25f;
static constexpr float OLED_IRRADIANCE_INTENSITY = 0.45f;
static constexpr float OLED_OCCLUSION_IRRADIANCE_THRESHOLD = 0.50f;
static constexpr float OLED_EFFECT_RADIUS = 0.55f;
static constexpr float OLED_EFFECT_SATURATION = 0.04f;
static constexpr float OLED_OCCLUSION_IRRADIANCE_RECOVERY = 0.15f;
static constexpr float OLED_AUTO_GAIN = 0.15f;
static constexpr float OLED_GAMMA = 1.0f;
static constexpr float OLED_GAIN = 1.0f;
static constexpr float OLED_CONTRAST = 1.0f;
static constexpr float OLED_SATURATION = 1.0f;

enum RTId : int {
    RT_HalfBlur = 0,
    RT_QuadBlur,
    RT_OctoBlur,
    RT_HexaBlur,
    RT_HBlur,
    RT_VBlur,
    RT_ShortBlur,
    RT_BlurMax,
    RT_BlurMaxHistory,
    RT_BlurMaxHistoryTemp,
};

#pragma pack(push, 4)
struct LSPOIrrCB {
    int32_t LSPOIrrEffectEnabled;
    int32_t LSPOIrrDebugType;
    float   PixelSize[2];

    float   AspectRatio;
    float   LSPOIrrEffectIntensity;
    float   LSPOIrrOcclusionIntensity;
    float   LSPOIrrIrradianceIntensity;

    float   LSPOIrrOclusionIrradianceThreshold;
    float   LSPOIrrEffectRadius;
    float   LSPOIrrEffectSaturation;
    float   LSPOIrrOcclusionIrradianceRecovery;

    float   LSPOIrrAutoGain;
    float   LSPOIrrGamma;
    float   LSPOIrrGain;
    float   LSPOIrrContrast;

    float   LSPOIrrSaturation;
    float   _pad0;
    float   _pad1;
    float   _pad2;
};
#pragma pack(pop)
static_assert(sizeof(LSPOIrrCB) == 80, "LSPOIrrCB must be 80 bytes");

#define LSPOIRR_HLSL_PREAMBLE R"(
cbuffer LSPOIrrCB : register(b0) {
    int    LSPOIrrEffectEnabled;
    int    LSPOIrrDebugType;
    float2 PixelSize;
    float  AspectRatio;
    float  LSPOIrrEffectIntensity;
    float  LSPOIrrOcclusionIntensity;
    float  LSPOIrrIrradianceIntensity;
    float  LSPOIrrOclusionIrradianceThreshold;
    float  LSPOIrrEffectRadius;
    float  LSPOIrrEffectSaturation;
    float  LSPOIrrOcclusionIrradianceRecovery;
    float  LSPOIrrAutoGain;
    float  LSPOIrrGamma;
    float  LSPOIrrGain;
    float  LSPOIrrContrast;
    float  LSPOIrrSaturation;
    float  _pad0; float _pad1; float _pad2;
};
SamplerState LinearSampler : register(s0);
struct PSI { float4 P : SV_Position; float2 uv : TEXCOORD0; };

static const float  ___BLUR_SAMPLE_OFFSET_CASCADE_0___ = 1.0;
static const float  ___BLUR_SAMPLE_OFFSET_CASCADE_1___ = 3.0;
static const float  ___BLUR_SAMPLE_OFFSET_CASCADE_2___ = 9.0;
static const float  ___MAX_CHANNEL_COMPENSATION___ = 1.5;
static const int    ___MAX_BLUR_NUM_SAMPLES___ = 7;
static const float  ___MAX_BLUR_POS_SAMPLE_START___ = 0.35;
static const float  ___MAX_BLUR_POS_SAMPLE_STEP___ = 0.05;
static const int    ___MAX_BLUR_NUM_TOTAL_SAMPLES___ = ___MAX_BLUR_NUM_SAMPLES___ * ___MAX_BLUR_NUM_SAMPLES___;
static const float  ___MAX_BLUR_NUM_TOTAL_SAMPLES_RCP___ = 1.0 / ___MAX_BLUR_NUM_TOTAL_SAMPLES___;
static const int    ___BUFFER_SIZE_DIVIDER___ = 1 << 4;
static const float  ___ONE_THIRD___ = 1.0 / 3.0;
static const float  ___STEP_MULTIPLIER___ = 1.5;
static const float  ___BUFFER_SIZE_DIVIDER_COMPENSATION_OFFSET___ = ___BUFFER_SIZE_DIVIDER___ * ___STEP_MULTIPLIER___;
static const float  LSPOIRR_AUTO_GAIN_SPEED = 0.04;
static const float  LSPOIRR_BLUR_MAX_RECIPROCAL_THRESHOLD = 0.05;

static const int ___LSPOIRR_DEBUG_NONE___ = 0x00;
static const int ___LSPOIRR_DEBUG_NO_INTENSITY___ = 0x01;
static const int ___LSPOIRR_DEBUG_NO_TONING___ = 0x02;
static const int ___LSPOIRR_DEBUG_RAW_BLUR___ = 0x03;
static const int ___LSPOIRR_DEBUG_SATURATED_BLUR___ = 0x04;
static const int ___LSPOIRR_DEBUG_GAINED_BLUR___ = 0x05;
static const int ___LSPOIRR_DEBUG_SCALED_BLUR___ = 0x06;
static const int ___LSPOIRR_DEBUG_OCCLUSION_IRRADIANCE_MAP___ = 0x07;
static const int ___LSPOIRR_DEBUG_BLUR_MAX_SAMPLES_POSITIONS___ = 0x08;
static const int ___LSPOIRR_DEBUG_BLUR_MAX___ = 0x09;
static const int ___LSPOIRR_DEBUG_BLUR_GAIN___ = 0x0A;
static const int ___LSPOIRR_DEBUG_RECOVERY_BLUR___ = 0x0B;
static const int ___LSPOIRR_DEBUG_SCALED_RECOVERY_BLUR___ = 0x0C;
static const int ___LSPOIRR_DEBUG_RECOVERY_OCCLUSION_IRRADIANCE_MAP___ = 0x0D;

float3 SaturateColor(float3 color, float saturation) {
    float grey = (color.r + color.g + color.b) * ___ONE_THIRD___;
    return lerp(float3(grey, grey, grey), color, saturation);
}

float ComputeColorMaxChannel(float3 color) {
    return max(color.r, max(color.g, color.b));
}

float OverlayBlend(float a, float b) {
    [branch]
    if (a < 0.5) return a * b * 2.0;
    return 1.0 - (1.0 - a) * (1.0 - b) * 2.0;
}

float3 OverlayBlend(float3 a, float3 b) {
    return float3(OverlayBlend(a.r, b.r), OverlayBlend(a.g, b.g), OverlayBlend(a.b, b.b));
}

float ScaleOcclusionAndIrradiance(float overlayValue, float occlusionIntensity, float irradianceIntensity) {
    return 0.5 + (overlayValue - 0.5) * (overlayValue < 0.5 ? occlusionIntensity : irradianceIntensity);
}

float3 ScaleOcclusionAndIrradiance(float3 overlayValue, float occlusionIntensity, float irradianceIntensity) {
    return float3(
        ScaleOcclusionAndIrradiance(overlayValue.r, occlusionIntensity, irradianceIntensity),
        ScaleOcclusionAndIrradiance(overlayValue.g, occlusionIntensity, irradianceIntensity),
        ScaleOcclusionAndIrradiance(overlayValue.b, occlusionIntensity, irradianceIntensity));
}

float ThresholdedScaleOcclusionAndIrradiance(float overlayValue, float occlusionIntensity, float irradianceIntensity) {
    if (overlayValue <= LSPOIrrOclusionIrradianceThreshold) {
        return 0.5 + occlusionIntensity * (overlayValue - LSPOIrrOclusionIrradianceThreshold) / (LSPOIrrOclusionIrradianceThreshold * 2.0);
    }
    return 0.5 + irradianceIntensity * (overlayValue - LSPOIrrOclusionIrradianceThreshold) / ((1.0 - LSPOIrrOclusionIrradianceThreshold) * 2.0);
}

float3 ThresholdedScaleOcclusionAndIrradiance(float3 overlayValue, float occlusionIntensity, float irradianceIntensity) {
    return float3(
        ThresholdedScaleOcclusionAndIrradiance(overlayValue.r, occlusionIntensity, irradianceIntensity),
        ThresholdedScaleOcclusionAndIrradiance(overlayValue.g, occlusionIntensity, irradianceIntensity),
        ThresholdedScaleOcclusionAndIrradiance(overlayValue.b, occlusionIntensity, irradianceIntensity));
}

float2 ScaledBufferDividerCompensationOffset() {
    return ___BUFFER_SIZE_DIVIDER_COMPENSATION_OFFSET___ * PixelSize;
}

float3 HBlur(float2 texcoord, float blurSampleOffset, Texture2D srcTex) {
    float offset = ScaledBufferDividerCompensationOffset().x * blurSampleOffset * LSPOIrrEffectRadius;
    float3 color = srcTex.SampleLevel(LinearSampler, texcoord, 0).rgb;
    color += srcTex.SampleLevel(LinearSampler, float2(texcoord.x - offset, texcoord.y), 0).rgb;
    color += srcTex.SampleLevel(LinearSampler, float2(texcoord.x + offset, texcoord.y), 0).rgb;
    color *= ___ONE_THIRD___;
    return color;
}

float3 VBlur(float2 texcoord, float blurSampleOffset, Texture2D srcTex) {
    float offset = ScaledBufferDividerCompensationOffset().y * blurSampleOffset * LSPOIrrEffectRadius;
    float3 color = srcTex.SampleLevel(LinearSampler, texcoord, 0).rgb;
    color += srcTex.SampleLevel(LinearSampler, float2(texcoord.x, texcoord.y - offset), 0).rgb;
    color += srcTex.SampleLevel(LinearSampler, float2(texcoord.x, texcoord.y + offset), 0).rgb;
    color *= ___ONE_THIRD___;
    return color;
}

float ComputeBlurGain(float blurMax, float reciprocalThreshold) {
    [branch]
    if (blurMax <= reciprocalThreshold) return blurMax / (reciprocalThreshold * reciprocalThreshold);
    return 1.0 / blurMax;
}

float ComputeBlurMaxChannel(Texture2D vBlurTex) {
    float maxChannel = 0.0;
    float2 uv = float2(___MAX_BLUR_POS_SAMPLE_START___, ___MAX_BLUR_POS_SAMPLE_START___);
    [unroll] for (int y = 0; y < ___MAX_BLUR_NUM_SAMPLES___; ++y) {
        uv.x = ___MAX_BLUR_POS_SAMPLE_START___;
        [unroll] for (int x = 0; x < ___MAX_BLUR_NUM_SAMPLES___; ++x) {
            maxChannel = max(maxChannel, ComputeColorMaxChannel(vBlurTex.SampleLevel(LinearSampler, uv, 0).rgb));
            uv.x += ___MAX_BLUR_POS_SAMPLE_STEP___;
        }
        uv.y += ___MAX_BLUR_POS_SAMPLE_STEP___;
    }
    maxChannel *= ___MAX_CHANNEL_COMPENSATION___;
    return maxChannel;
}

float3 DrawBlurMaxSamplesPositions(float2 texcoord) {
    float3 color = 0.0;
    float2 uv = float2(___MAX_BLUR_POS_SAMPLE_START___, ___MAX_BLUR_POS_SAMPLE_START___);
    [unroll] for (int y = 0; y < ___MAX_BLUR_NUM_SAMPLES___; ++y) {
        uv.x = ___MAX_BLUR_POS_SAMPLE_START___;
        [unroll] for (int x = 0; x < ___MAX_BLUR_NUM_SAMPLES___; ++x) {
            float xDist = (uv.x - texcoord.x) * AspectRatio;
            float yDist = uv.y - texcoord.y;
            float dist = sqrt(xDist * xDist + yDist * yDist);
            dist = 1.0 - dist;
            dist = saturate(dist);
            dist = pow(dist, 100.0);
            dist = dist > 0.5 ? 0.5 : 0.0;
            color += float3(dist, 0.0, 0.0);
            uv.x += ___MAX_BLUR_POS_SAMPLE_STEP___;
        }
        uv.y += ___MAX_BLUR_POS_SAMPLE_STEP___;
    }
    return color;
}

float4 Out(float3 color) { return float4(color, 1.0); }
)"

static const char* g_ps_copy_bb = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D Scene : register(t0);
float4 main(PSI i) : SV_Target { return Out(Scene.SampleLevel(LinearSampler, i.uv, 0).rgb); }
)";

static const char* g_ps_copy_half = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D HalfBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(HalfBlurTex.SampleLevel(LinearSampler, i.uv, 0).rgb); }
)";

static const char* g_ps_copy_quad = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D QuadBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(QuadBlurTex.SampleLevel(LinearSampler, i.uv, 0).rgb); }
)";

static const char* g_ps_copy_octo = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D OctoBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(OctoBlurTex.SampleLevel(LinearSampler, i.uv, 0).rgb); }
)";

static const char* g_ps_copy_hexa = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D HexaBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(HexaBlurTex.SampleLevel(LinearSampler, i.uv, 0).rgb); }
)";

static const char* g_ps_hblur_c0 = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D VBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(HBlur(i.uv, ___BLUR_SAMPLE_OFFSET_CASCADE_0___, VBlurTex)); }
)";

static const char* g_ps_vblur_c0 = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D HBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(VBlur(i.uv, ___BLUR_SAMPLE_OFFSET_CASCADE_0___, HBlurTex)); }
)";

static const char* g_ps_hblur_c1 = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D VBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(HBlur(i.uv, ___BLUR_SAMPLE_OFFSET_CASCADE_1___, VBlurTex)); }
)";

static const char* g_ps_vblur_c1 = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D HBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(VBlur(i.uv, ___BLUR_SAMPLE_OFFSET_CASCADE_1___, HBlurTex)); }
)";

static const char* g_ps_hblur_c2 = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D VBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(HBlur(i.uv, ___BLUR_SAMPLE_OFFSET_CASCADE_2___, VBlurTex)); }
)";

static const char* g_ps_vblur_c2 = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D HBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(VBlur(i.uv, ___BLUR_SAMPLE_OFFSET_CASCADE_2___, HBlurTex)); }
)";

static const char* g_ps_copy_vblur = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D VBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return Out(VBlurTex.SampleLevel(LinearSampler, i.uv, 0).rgb); }
)";

static const char* g_ps_compute_blur_max = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D VBlurTex : register(t0);
float4 main(PSI i) : SV_Target { return float4(ComputeBlurMaxChannel(VBlurTex), 0.0, 0.0, 0.0); }
)";

static const char* g_ps_blend_blur_max = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D BlurMaxTex : register(t0);
Texture2D BlurMaxHistoryTex : register(t1);
float4 main(PSI i) : SV_Target {
    float blurMax = BlurMaxTex.SampleLevel(LinearSampler, i.uv, 0).r;
    float blurMaxHistory = BlurMaxHistoryTex.SampleLevel(LinearSampler, i.uv, 0).r;
    blurMaxHistory = lerp(blurMaxHistory, blurMax, LSPOIRR_AUTO_GAIN_SPEED);
    return float4(blurMaxHistory, 0.0, 0.0, 0.0);
}
)";

static const char* g_ps_copy_blur_max_history_temp = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D BlurMaxHistoryTempTex : register(t0);
float4 main(PSI i) : SV_Target { return float4(BlurMaxHistoryTempTex.SampleLevel(LinearSampler, i.uv, 0).r, 0.0, 0.0, 0.0); }
)";

static const char* g_ps_lspoirr = LSPOIRR_HLSL_PREAMBLE R"(
Texture2D Scene : register(t0);
Texture2D VBlurTex : register(t1);
Texture2D BlurMaxHistoryTex : register(t2);
Texture2D ShortBlurTex : register(t3);
float4 main(PSI i) : SV_Target {
    float4 scene = Scene.SampleLevel(LinearSampler, i.uv, 0);
    float3 color = scene.rgb;
    if (LSPOIrrEffectEnabled == 0) return scene;

    float3 finalColor = color;
    float3 overlayColor = VBlurTex.SampleLevel(LinearSampler, i.uv, 0).rgb;
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_RAW_BLUR___) return float4(overlayColor, scene.a);

    overlayColor = SaturateColor(overlayColor, LSPOIrrEffectSaturation);
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_SATURATED_BLUR___) return float4(overlayColor, scene.a);
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_OCCLUSION_IRRADIANCE_MAP___) {
        return float4(lerp(color, float3(1.0, 1.0, 1.0) - step(overlayColor, float3(0.5, 0.5, 0.5)), 0.65), scene.a);
    }

    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_BLUR_MAX_SAMPLES_POSITIONS___) {
        float3 samplesPositionColor = DrawBlurMaxSamplesPositions(i.uv);
        return float4(samplesPositionColor.r < 0.01 ? color : samplesPositionColor, scene.a);
    }

    float blurMax = BlurMaxHistoryTex.SampleLevel(LinearSampler, i.uv, 0).r;
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_BLUR_MAX___) return float4(blurMax.xxx, scene.a);

    float blurGain = ComputeBlurGain(blurMax, LSPOIRR_BLUR_MAX_RECIPROCAL_THRESHOLD);
    blurGain = clamp(blurGain, 1.0, 4.0);
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_BLUR_GAIN___) return float4(blurGain.xxx, scene.a);

    blurGain = lerp(1.0, blurGain, LSPOIrrAutoGain);
    overlayColor *= blurGain;
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_GAINED_BLUR___) return float4(overlayColor, scene.a);

    overlayColor = ThresholdedScaleOcclusionAndIrradiance(overlayColor, LSPOIrrOcclusionIntensity, LSPOIrrIrradianceIntensity);
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_SCALED_BLUR___) return float4(overlayColor, scene.a);

    finalColor = OverlayBlend(finalColor, overlayColor);

    float3 recoveryOverlayColor = ShortBlurTex.SampleLevel(LinearSampler, i.uv, 0).rgb;
    recoveryOverlayColor = SaturateColor(recoveryOverlayColor, 0.0);
    recoveryOverlayColor = 1.0 - recoveryOverlayColor;
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_RECOVERY_BLUR___) return float4(recoveryOverlayColor, scene.a);
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_RECOVERY_OCCLUSION_IRRADIANCE_MAP___) {
        return float4(lerp(color, float3(1.0, 1.0, 1.0) - step(recoveryOverlayColor, float3(0.5, 0.5, 0.5)), 0.65), scene.a);
    }

    recoveryOverlayColor = (recoveryOverlayColor - 0.5) * LSPOIrrOcclusionIrradianceRecovery + 0.5;
    recoveryOverlayColor = ScaleOcclusionAndIrradiance(recoveryOverlayColor, LSPOIrrIrradianceIntensity, LSPOIrrOcclusionIntensity);
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_SCALED_RECOVERY_BLUR___) return float4(recoveryOverlayColor, scene.a);

    finalColor = OverlayBlend(finalColor, recoveryOverlayColor);
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_NO_TONING___) return float4(finalColor, scene.a);

    finalColor = pow(max(0.0, finalColor), LSPOIrrGamma);
    finalColor *= LSPOIrrGain;
    finalColor = (finalColor - 0.5) * LSPOIrrContrast + 0.5;
    finalColor = SaturateColor(finalColor, LSPOIrrSaturation);
    if (LSPOIrrDebugType == ___LSPOIRR_DEBUG_NO_INTENSITY___) return float4(finalColor, scene.a);

    finalColor = lerp(color, finalColor, LSPOIrrEffectIntensity);
    return float4(finalColor, scene.a);
}
)";

class FGFXLargeScalePerceptualObscuranceIrradiancePlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled = false;
    bool  m_effect_enabled = DEFAULT_EFFECT_ENABLED;
    float m_effect_intensity = DEFAULT_EFFECT_INTENSITY;
    float m_occlusion_intensity = DEFAULT_OCCLUSION_INTENSITY;
    float m_irradiance_intensity = DEFAULT_IRRADIANCE_INTENSITY;
    float m_occlusion_irradiance_threshold = DEFAULT_OCCLUSION_IRRADIANCE_THRESHOLD;
    float m_effect_radius = DEFAULT_EFFECT_RADIUS;
    float m_effect_saturation = DEFAULT_EFFECT_SATURATION;
    float m_occlusion_irradiance_recovery = DEFAULT_OCCLUSION_IRRADIANCE_RECOVERY;
    float m_auto_gain = DEFAULT_AUTO_GAIN;
    float m_gamma = DEFAULT_GAMMA;
    float m_gain = DEFAULT_GAIN;
    float m_contrast = DEFAULT_CONTRAST;
    float m_saturation = DEFAULT_SATURATION;
    int   m_debug_type = DEFAULT_DEBUG_TYPE;

    LSPOIrrCB m_cb{};
    fx::EffectRuntime m_runtime;
    bool m_passes_set = false;

    void on_initialize() override {
        API::get()->log_info("[FGFXLargeScalePerceptualObscuranceIrradiance] Plugin initialized (v%s)", LSPOIRR_VERSION);
        configure_runtime();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    std::string preset_section_name() const override { return "FGFXLargeScalePerceptualObscuranceIrradiance"; }
    int render_order() const override { return 0; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled", m_enabled ? "1" : "0"},
            {"effect_enabled", m_effect_enabled ? "1" : "0"},
            {"effect_intensity", std::to_string(m_effect_intensity)},
            {"occlusion_intensity", std::to_string(m_occlusion_intensity)},
            {"irradiance_intensity", std::to_string(m_irradiance_intensity)},
            {"occlusion_irradiance_threshold", std::to_string(m_occlusion_irradiance_threshold)},
            {"effect_radius", std::to_string(m_effect_radius)},
            {"effect_saturation", std::to_string(m_effect_saturation)},
            {"occlusion_irradiance_recovery", std::to_string(m_occlusion_irradiance_recovery)},
            {"auto_gain", std::to_string(m_auto_gain)},
            {"gamma", std::to_string(m_gamma)},
            {"gain", std::to_string(m_gain)},
            {"contrast", std::to_string(m_contrast)},
            {"saturation", std::to_string(m_saturation)},
            {"debug_type", std::to_string(m_debug_type)},
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
        get_bool("enabled", m_enabled);
        get_bool("effect_enabled", m_effect_enabled);
        get_float("effect_intensity", m_effect_intensity);
        get_float("occlusion_intensity", m_occlusion_intensity);
        get_float("irradiance_intensity", m_irradiance_intensity);
        get_float("occlusion_irradiance_threshold", m_occlusion_irradiance_threshold);
        get_float("effect_radius", m_effect_radius);
        get_float("effect_saturation", m_effect_saturation);
        get_float("occlusion_irradiance_recovery", m_occlusion_irradiance_recovery);
        get_float("auto_gain", m_auto_gain);
        get_float("gamma", m_gamma);
        get_float("gain", m_gain);
        get_float("contrast", m_contrast);
        get_float("saturation", m_saturation);
        get_int("debug_type", m_debug_type);
        clamp_settings();
    }

    void reset_to_defaults() override {
        m_enabled = false;
        restore_original_defaults();
    }

    void restore_original_defaults() {
        m_effect_enabled = DEFAULT_EFFECT_ENABLED;
        m_effect_intensity = DEFAULT_EFFECT_INTENSITY;
        m_occlusion_intensity = DEFAULT_OCCLUSION_INTENSITY;
        m_irradiance_intensity = DEFAULT_IRRADIANCE_INTENSITY;
        m_occlusion_irradiance_threshold = DEFAULT_OCCLUSION_IRRADIANCE_THRESHOLD;
        m_effect_radius = DEFAULT_EFFECT_RADIUS;
        m_effect_saturation = DEFAULT_EFFECT_SATURATION;
        m_occlusion_irradiance_recovery = DEFAULT_OCCLUSION_IRRADIANCE_RECOVERY;
        m_auto_gain = DEFAULT_AUTO_GAIN;
        m_gamma = DEFAULT_GAMMA;
        m_gain = DEFAULT_GAIN;
        m_contrast = DEFAULT_CONTRAST;
        m_saturation = DEFAULT_SATURATION;
        m_debug_type = DEFAULT_DEBUG_TYPE;
    }

    void apply_oled_vr_preset() {
        m_enabled = true;
        m_effect_enabled = true;
        m_effect_intensity = OLED_EFFECT_INTENSITY;
        m_occlusion_intensity = OLED_OCCLUSION_INTENSITY;
        m_irradiance_intensity = OLED_IRRADIANCE_INTENSITY;
        m_occlusion_irradiance_threshold = OLED_OCCLUSION_IRRADIANCE_THRESHOLD;
        m_effect_radius = OLED_EFFECT_RADIUS;
        m_effect_saturation = OLED_EFFECT_SATURATION;
        m_occlusion_irradiance_recovery = OLED_OCCLUSION_IRRADIANCE_RECOVERY;
        m_auto_gain = OLED_AUTO_GAIN;
        m_gamma = OLED_GAMMA;
        m_gain = OLED_GAIN;
        m_contrast = OLED_CONTRAST;
        m_saturation = OLED_SATURATION;
        m_debug_type = DEFAULT_DEBUG_TYPE;
    }

    void clamp_settings() {
        m_effect_intensity = std::clamp(m_effect_intensity, 0.0f, 1.0f);
        m_occlusion_intensity = std::clamp(m_occlusion_intensity, 0.0f, 1.0f);
        m_irradiance_intensity = std::clamp(m_irradiance_intensity, 0.0f, 1.0f);
        m_occlusion_irradiance_threshold = std::clamp(m_occlusion_irradiance_threshold, 0.004f, 0.996f);
        m_effect_radius = std::clamp(m_effect_radius, 0.25f, 1.0f);
        m_effect_saturation = std::clamp(m_effect_saturation, 0.0f, 1.0f);
        m_occlusion_irradiance_recovery = std::clamp(m_occlusion_irradiance_recovery, 0.0f, 1.0f);
        m_auto_gain = std::clamp(m_auto_gain, 0.0f, 1.0f);
        m_gamma = std::clamp(m_gamma, 0.10f, 4.0f);
        m_gain = std::clamp(m_gain, 0.0f, 4.0f);
        m_contrast = std::clamp(m_contrast, 0.0f, 1.0f);
        m_saturation = std::clamp(m_saturation, 0.0f, 2.0f);
        m_debug_type = std::clamp(m_debug_type, 0, 13);
    }

    int declare_rt(int div, DXGI_FORMAT format, bool shared = false, bool persistent = false) {
        fx::RTDesc rt{};
        rt.size_mode = fx::RTDesc::SizeMode::BackbufferDiv;
        rt.w_or_div = div;
        rt.h_or_div = div;
        rt.format = format;
        rt.shared_across_scene_slots = shared;
        rt.persistent = persistent;
        return m_runtime.declare_rt(rt);
    }

    void configure_runtime() {
        if (m_passes_set) return;

        declare_rt(2,  DXGI_FORMAT_R16G16B16A16_FLOAT);                         // HalfBlurTex
        declare_rt(4,  DXGI_FORMAT_R16G16B16A16_FLOAT);                         // QuadBlurTex
        declare_rt(8,  DXGI_FORMAT_R16G16B16A16_FLOAT);                         // OctoBlurTex
        declare_rt(16, DXGI_FORMAT_R16G16B16A16_FLOAT);                         // HexaBlurTex
        declare_rt(16, DXGI_FORMAT_R16G16B16A16_FLOAT);                         // HBlurTex
        declare_rt(16, DXGI_FORMAT_R16G16B16A16_FLOAT);                         // VBlurTex
        declare_rt(16, DXGI_FORMAT_R16G16B16A16_FLOAT);                         // ShortBlurTex
        declare_rt(16, DXGI_FORMAT_R16_FLOAT, true);                            // BlurMaxTex
        declare_rt(16, DXGI_FORMAT_R16_FLOAT, true, true);                      // BlurMaxHistoryTex
        declare_rt(16, DXGI_FORMAT_R16_FLOAT, true);                            // BlurMaxHistoryTempTex

        std::vector<fx::PassDesc> passes;
        passes.reserve(24);
        auto add_pass = [&](const char* hlsl, std::vector<int> inputs, int output, fx::Cadence cadence = fx::Cadence::EveryDispatch) {
            fx::PassDesc p;
            p.ps_hlsl = hlsl;
            p.inputs = std::move(inputs);
            p.output = output;
            p.cb_data = &m_cb;
            p.cb_size = sizeof(m_cb);
            p.cadence = cadence;
            passes.push_back(std::move(p));
        };

        add_pass(g_ps_copy_bb, { fx::INPUT_SCENE }, RT_HalfBlur);
        add_pass(g_ps_copy_half, { RT_HalfBlur }, RT_QuadBlur);
        add_pass(g_ps_copy_quad, { RT_QuadBlur }, RT_OctoBlur);
        add_pass(g_ps_copy_octo, { RT_OctoBlur }, RT_HexaBlur);
        add_pass(g_ps_copy_hexa, { RT_HexaBlur }, RT_VBlur);
        add_pass(g_ps_hblur_c0, { RT_VBlur }, RT_HBlur);
        add_pass(g_ps_vblur_c0, { RT_HBlur }, RT_VBlur);
        add_pass(g_ps_hblur_c0, { RT_VBlur }, RT_HBlur);
        add_pass(g_ps_vblur_c0, { RT_HBlur }, RT_VBlur);
        add_pass(g_ps_hblur_c0, { RT_VBlur }, RT_HBlur);
        add_pass(g_ps_vblur_c0, { RT_HBlur }, RT_VBlur);
        add_pass(g_ps_hblur_c1, { RT_VBlur }, RT_HBlur);
        add_pass(g_ps_vblur_c1, { RT_HBlur }, RT_VBlur);
        add_pass(g_ps_hblur_c2, { RT_VBlur }, RT_HBlur);
        add_pass(g_ps_vblur_c2, { RT_HBlur }, RT_VBlur);
        add_pass(g_ps_copy_vblur, { RT_VBlur }, RT_ShortBlur);
        add_pass(g_ps_hblur_c2, { RT_VBlur }, RT_HBlur);
        add_pass(g_ps_vblur_c2, { RT_HBlur }, RT_VBlur);
        add_pass(g_ps_hblur_c0, { RT_VBlur }, RT_HBlur);
        add_pass(g_ps_vblur_c0, { RT_HBlur }, RT_VBlur);
        add_pass(g_ps_compute_blur_max, { RT_VBlur }, RT_BlurMax, fx::Cadence::OncePerFrame);
        add_pass(g_ps_blend_blur_max, { RT_BlurMax, RT_BlurMaxHistory }, RT_BlurMaxHistoryTemp, fx::Cadence::OncePerFrame);
        add_pass(g_ps_copy_blur_max_history_temp, { RT_BlurMaxHistoryTemp }, RT_BlurMaxHistory, fx::Cadence::OncePerFrame);
        add_pass(g_ps_lspoirr, { fx::INPUT_SCENE, RT_VBlur, RT_BlurMaxHistory, RT_ShortBlur }, fx::OUTPUT_SCENE);

        m_runtime.set_passes(std::move(passes));
        m_passes_set = true;
        API::get()->log_info("[FGFXLargeScalePerceptualObscuranceIrradiance] configure_runtime: passes=24");
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
        if (ImGui::CollapsingHeader("FGFX Large Scale Perceptual Obscurance/Irradiance", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s - Large Scale Perceptual Obscurance and Irradiance", LSPOIRR_VERSION);
            ImGui::TextWrapped(
                "Large-radius scene blur blended back as low-frequency occlusion and light bounce. It can add depth and body before the color-correction chain, "
                "but it is easy to overdo in VR. Start from OLED VR or lower Effect Intensity first, then balance Occlusion, Irradiance, and Recovery until dark areas gain shape without looking dirty.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##LSPOIrr", &m_enabled);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run the whole multi-pass FGFX effect. This is heavier than simple color shaders.");
            changed |= ImGui::Checkbox("Effect Enabled##LSPOIrr", &m_effect_enabled);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Source shader bypass inside the final pass. Useful for comparing runtime cost and debug outputs while keeping resources active.");

            if (ImGui::TreeNodeEx("Effect Settings##LSPOIrr", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= slider_with_reset("Effect Intensity##LSPOIrr", &m_effect_intensity, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_EFFECT_INTENSITY, "Reset##LSPOIrr_effect_intensity",
                                             "Final blend with the original scene. Lower this first if the whole image looks processed or heavy.");
                changed |= slider_with_reset("Occlusion Intensity##LSPOIrr", &m_occlusion_intensity, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_OCCLUSION_INTENSITY, "Reset##LSPOIrr_occlusion",
                                             "How strongly broad dark structure darkens the scene. Too high can make walls and foliage look stained.");
                changed |= slider_with_reset("Irradiance Intensity##LSPOIrr", &m_irradiance_intensity, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_IRRADIANCE_INTENSITY, "Reset##LSPOIrr_irradiance",
                                             "How strongly broad bright structure lifts nearby color. Raise for soft bounce-light feel; lower if bright areas bloom or haze.");
                changed |= slider_with_reset("Occlusion / Irradiance Threshold##LSPOIrr", &m_occlusion_irradiance_threshold, 0.001f, 0.004f, 0.996f, "%.3f", DEFAULT_OCCLUSION_IRRADIANCE_THRESHOLD, "Reset##LSPOIrr_threshold",
                                             "Split point between darkening and brightening. Lower favors irradiance; higher favors occlusion.");
                changed |= slider_with_reset("Effect Radius##LSPOIrr", &m_effect_radius, 0.01f, 0.25f, 1.0f, "%.2f", DEFAULT_EFFECT_RADIUS, "Reset##LSPOIrr_radius",
                                             "Size of the large blur footprint. Bigger feels more global and softer; smaller tracks local shapes more tightly.");
                changed |= slider_with_reset("Effect Saturation##LSPOIrr", &m_effect_saturation, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_EFFECT_SATURATION, "Reset##LSPOIrr_effect_saturation",
                                             "Color kept in the blurred overlay. Low values are safer in VR; high values can tint shadows and highlights.");
                changed |= slider_with_reset("Occlusion-Irradiance Recovery##LSPOIrr", &m_occlusion_irradiance_recovery, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_OCCLUSION_IRRADIANCE_RECOVERY, "Reset##LSPOIrr_recovery",
                                             "Counter-blend from the short blur that restores local readability. Raise if occlusion crushes detail; lower for a stronger moodier look.");
                ImGui::TreePop();
            }

            if (ImGui::TreeNodeEx("Toning Settings##LSPOIrr", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= slider_with_reset("Auto-Gain##LSPOIrr", &m_auto_gain, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_AUTO_GAIN, "Reset##LSPOIrr_auto_gain",
                                             "Automatic normalization of the blur before blending. Lower if the effect pumps between scenes; higher keeps it visible across lighting changes.");
                changed |= slider_with_reset("Gamma##LSPOIrr", &m_gamma, 0.01f, 0.10f, 4.0f, "%.2f", DEFAULT_GAMMA, "Reset##LSPOIrr_gamma",
                                             "Tone the FGFX result before blending. Below 1 brightens the overlay; above 1 darkens it.");
                changed |= slider_with_reset("Gain##LSPOIrr", &m_gain, 0.01f, 0.0f, 4.0f, "%.2f", DEFAULT_GAIN, "Reset##LSPOIrr_gain",
                                             "Brightness multiplier for the FGFX result. Adjust after intensity/radius, not as the first control.");
                changed |= slider_with_reset("Contrast##LSPOIrr", &m_contrast, 0.01f, 0.0f, 1.0f, "%.2f", DEFAULT_CONTRAST, "Reset##LSPOIrr_contrast",
                                             "Contrast applied to the FGFX result. Lower is flatter and safer; higher can make the overlay harsher.");
                changed |= slider_with_reset("Saturation##LSPOIrr", &m_saturation, 0.01f, 0.0f, 2.0f, "%.2f", DEFAULT_SATURATION, "Reset##LSPOIrr_saturation",
                                             "Final saturation of the FGFX-toned result. Keep near 1 unless the effect is visibly draining or tinting color.");
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Debug##LSPOIrr")) {
                static const char* items[] = {
                    "None", "No Intensity", "No Toning", "Raw Blur", "Saturated Blur",
                    "Gained Blur", "Scaled Blur", "Occlusion - Irradiance Map",
                    "Blur Max Samples Positions", "Blur Max", "Blur Gain", "Recovery Blur",
                    "Scaled Recovery Blur", "Recovery Occlusion - Irradiance Map"
                };
                changed |= ImGui::Combo("Debug Type##LSPOIrr", &m_debug_type, items, IM_ARRAYSIZE(items));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visualize internal blur, gain, threshold, and recovery stages. Use None for normal play.");
                ImGui::TreePop();
            }

            if (ImGui::Button("Reset All##LSPOIrr")) {
                restore_original_defaults();
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restore FGFX source-default tuning.");
            ImGui::SameLine();
            if (ImGui::Button("OLED VR##LSPOIrr")) {
                apply_oled_vr_preset();
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Conservative VR starting point: lower intensity, lower occlusion, less saturation, and gentler auto-gain.");
            if (changed) {
                clamp_settings();
                uevr::settings::notify_changed(*this, API::get()->param());
            }
        }
    }

    void update_cb() {
        clamp_settings();
        const unsigned w = fx::EffectRuntime::scene_width();
        const unsigned h = fx::EffectRuntime::scene_height();
        m_cb.LSPOIrrEffectEnabled = m_effect_enabled ? 1 : 0;
        m_cb.LSPOIrrDebugType = m_debug_type;
        m_cb.PixelSize[0] = w > 0 ? 1.0f / static_cast<float>(w) : 0.0f;
        m_cb.PixelSize[1] = h > 0 ? 1.0f / static_cast<float>(h) : 0.0f;
        m_cb.AspectRatio = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
        m_cb.LSPOIrrEffectIntensity = m_effect_intensity;
        m_cb.LSPOIrrOcclusionIntensity = m_occlusion_intensity;
        m_cb.LSPOIrrIrradianceIntensity = m_irradiance_intensity;
        m_cb.LSPOIrrOclusionIrradianceThreshold = m_occlusion_irradiance_threshold;
        m_cb.LSPOIrrEffectRadius = m_effect_radius;
        m_cb.LSPOIrrEffectSaturation = m_effect_saturation;
        m_cb.LSPOIrrOcclusionIrradianceRecovery = m_occlusion_irradiance_recovery;
        m_cb.LSPOIrrAutoGain = m_auto_gain;
        m_cb.LSPOIrrGamma = m_gamma;
        m_cb.LSPOIrrGain = m_gain;
        m_cb.LSPOIrrContrast = m_contrast;
        m_cb.LSPOIrrSaturation = m_saturation;
    }

    void run_impl() {
        if (!m_enabled || !m_passes_set) return;
        update_cb();
        m_runtime.execute();
    }

    void run() {
        __try {
            run_impl();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static uint64_t s_last_av_log = 0;
            const uint64_t now_ms = static_cast<uint64_t>(GetTickCount64());
            if (now_ms - s_last_av_log > 1000) {
                s_last_av_log = now_ms;
                API::get()->log_warn("[FGFXLargeScalePerceptualObscuranceIrradiance] SEH exception 0x%lx in run_impl()", (unsigned long)GetExceptionCode());
            }
        }
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_runtime.release_resources(); }
};

std::unique_ptr<FGFXLargeScalePerceptualObscuranceIrradiancePlugin> g_plugin{ new FGFXLargeScalePerceptualObscuranceIrradiancePlugin() };