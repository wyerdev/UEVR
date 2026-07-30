// FakeHDR Pixel Shader for UEVR
// Based on CeeJay.dk's FakeHDR ReShade effect
// Ported to a standalone HLSL pixel shader for D3D11 full-screen quad rendering.

cbuffer HDRParams : register(b0)
{
    float HDRPower;   // Default: 1.30
    float Radius1;    // Default: 0.793
    float Radius2;    // Default: 0.87
    float _Pad0;
    float2 PixelSize; // 1.0 / texture dimensions
    float2 _Pad1;
};

Texture2D SceneTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct PSInput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    float2 texcoord = input.TexCoord;
    
    float3 color = SceneTexture.Sample(LinearSampler, texcoord).rgb;

    // First bloom pass (radius1)
    float3 bloom_sum1 = float3(0.0, 0.0, 0.0);
    bloom_sum1 += SceneTexture.Sample(LinearSampler, texcoord + float2( 1.5, -1.5) * Radius1 * PixelSize).rgb;
    bloom_sum1 += SceneTexture.Sample(LinearSampler, texcoord + float2(-1.5, -1.5) * Radius1 * PixelSize).rgb;
    bloom_sum1 += SceneTexture.Sample(LinearSampler, texcoord + float2( 1.5,  1.5) * Radius1 * PixelSize).rgb;
    bloom_sum1 += SceneTexture.Sample(LinearSampler, texcoord + float2(-1.5,  1.5) * Radius1 * PixelSize).rgb;
    bloom_sum1 += SceneTexture.Sample(LinearSampler, texcoord + float2( 0.0, -2.5) * Radius1 * PixelSize).rgb;
    bloom_sum1 += SceneTexture.Sample(LinearSampler, texcoord + float2( 0.0,  2.5) * Radius1 * PixelSize).rgb;
    bloom_sum1 += SceneTexture.Sample(LinearSampler, texcoord + float2(-2.5,  0.0) * Radius1 * PixelSize).rgb;
    bloom_sum1 += SceneTexture.Sample(LinearSampler, texcoord + float2( 2.5,  0.0) * Radius1 * PixelSize).rgb;
    bloom_sum1 *= 0.005;

    // Second bloom pass (radius2)
    float3 bloom_sum2 = float3(0.0, 0.0, 0.0);
    bloom_sum2 += SceneTexture.Sample(LinearSampler, texcoord + float2( 1.5, -1.5) * Radius2 * PixelSize).rgb;
    bloom_sum2 += SceneTexture.Sample(LinearSampler, texcoord + float2(-1.5, -1.5) * Radius2 * PixelSize).rgb;
    bloom_sum2 += SceneTexture.Sample(LinearSampler, texcoord + float2( 1.5,  1.5) * Radius2 * PixelSize).rgb;
    bloom_sum2 += SceneTexture.Sample(LinearSampler, texcoord + float2(-1.5,  1.5) * Radius2 * PixelSize).rgb;
    bloom_sum2 += SceneTexture.Sample(LinearSampler, texcoord + float2( 0.0, -2.5) * Radius2 * PixelSize).rgb;
    bloom_sum2 += SceneTexture.Sample(LinearSampler, texcoord + float2( 0.0,  2.5) * Radius2 * PixelSize).rgb;
    bloom_sum2 += SceneTexture.Sample(LinearSampler, texcoord + float2(-2.5,  0.0) * Radius2 * PixelSize).rgb;
    bloom_sum2 += SceneTexture.Sample(LinearSampler, texcoord + float2( 2.5,  0.0) * Radius2 * PixelSize).rgb;
    bloom_sum2 *= 0.010;

    float dist = Radius2 - Radius1;
    float3 HDR = (color + (bloom_sum2 - bloom_sum1)) * dist;
    float3 blend = HDR + color;
    color = pow(abs(blend), abs(HDRPower)) + HDR;
    
    return float4(saturate(color), 1.0);
}
