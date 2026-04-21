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

#include <memory>
#include <fstream>
#include <string>

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#include "imgui/imgui_impl_win32.h"

#include "uevr/Plugin.hpp"

using namespace uevr;
template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

// ============================================================================
// Embedded HLSL source (compiled at runtime via D3DCompile)
// ============================================================================

static const char* g_filmgrain2_vs_src = R"(
struct VSOutput {
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

// Faithful port of martinsh's FilmGrain2 from ReShade
// Replaced ReShade built-ins with CB values: Timer, ScreenSize, AspectRatio
static const char* g_filmgrain2_ps_src = R"(
cbuffer GrainParams : register(b0) {
    float GrainAmount;   // 0.0 - 0.2
    float ColorAmount;   // 0.0 - 1.0
    float LumAmount;     // 0.0 - 1.0
    float GrainSize;     // 1.5 - 2.5
    float Timer;         // milliseconds (from GetTickCount)
    float2 ScreenSize;   // width, height in pixels
    float AspectRatio;   // width / height
};

Texture2D SceneTexture : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput {
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

float4 rnm(in float2 tc)
{
    float noise = sin(dot(tc, float2(12.9898, 78.233))) * 43758.5453;
    float noiseR = frac(noise) * 2.0 - 1.0;
    float noiseG = frac(noise * 1.2154) * 2.0 - 1.0;
    float noiseB = frac(noise * 1.3453) * 2.0 - 1.0;
    float noiseA = frac(noise * 1.3647) * 2.0 - 1.0;
    return float4(noiseR, noiseG, noiseB, noiseA);
}

float pnoise3D(in float3 p)
{
    static const float permTexUnit = 1.0 / 256.0;
    static const float permTexUnitHalf = 0.5 / 256.0;

    float3 pi = permTexUnit * floor(p) + permTexUnitHalf;
    float3 pf = frac(p);

    // Noise contributions from (x=0, y=0), z=0 and z=1
    float perm00 = rnm(pi.xy).a;
    float3 grad000 = rnm(float2(perm00, pi.z)).rgb * 4.0 - 1.0;
    float n000 = dot(grad000, pf);
    float3 grad001 = rnm(float2(perm00, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
    float n001 = dot(grad001, pf - float3(0.0, 0.0, 1.0));

    // Noise contributions from (x=0, y=1), z=0 and z=1
    float perm01 = rnm(pi.xy + float2(0.0, permTexUnit)).a;
    float3 grad010 = rnm(float2(perm01, pi.z)).rgb * 4.0 - 1.0;
    float n010 = dot(grad010, pf - float3(0.0, 1.0, 0.0));
    float3 grad011 = rnm(float2(perm01, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
    float n011 = dot(grad011, pf - float3(0.0, 1.0, 1.0));

    // Noise contributions from (x=1, y=0), z=0 and z=1
    float perm10 = rnm(pi.xy + float2(permTexUnit, 0.0)).a;
    float3 grad100 = rnm(float2(perm10, pi.z)).rgb * 4.0 - 1.0;
    float n100 = dot(grad100, pf - float3(1.0, 0.0, 0.0));
    float3 grad101 = rnm(float2(perm10, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
    float n101 = dot(grad101, pf - float3(1.0, 0.0, 1.0));

    // Noise contributions from (x=1, y=1), z=0 and z=1
    float perm11 = rnm(pi.xy + float2(permTexUnit, permTexUnit)).a;
    float3 grad110 = rnm(float2(perm11, pi.z)).rgb * 4.0 - 1.0;
    float n110 = dot(grad110, pf - float3(1.0, 1.0, 0.0));
    float3 grad111 = rnm(float2(perm11, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
    float n111 = dot(grad111, pf - float3(1.0, 1.0, 1.0));

    // Blend contributions along x
    float fade_x = pf.x * pf.x * pf.x * (pf.x * (pf.x * 6.0 - 15.0) + 10.0);
    float4 n_x = lerp(float4(n000, n001, n010, n011), float4(n100, n101, n110, n111), fade_x);

    // Blend contributions along y
    float fade_y = pf.y * pf.y * pf.y * (pf.y * (pf.y * 6.0 - 15.0) + 10.0);
    float2 n_xy = lerp(n_x.xy, n_x.zw, fade_y);

    // Blend contributions along z
    float fade_z = pf.z * pf.z * pf.z * (pf.z * (pf.z * 6.0 - 15.0) + 10.0);
    float n_xyz = lerp(n_xy.x, n_xy.y, fade_z);

    return n_xyz;
}

float2 coordRot(in float2 tc, in float angle)
{
    float rotX = ((tc.x * 2.0 - 1.0) * AspectRatio * cos(angle)) - ((tc.y * 2.0 - 1.0) * sin(angle));
    float rotY = ((tc.y * 2.0 - 1.0) * cos(angle)) + ((tc.x * 2.0 - 1.0) * AspectRatio * sin(angle));
    rotX = ((rotX / AspectRatio) * 0.5 + 0.5);
    rotY = rotY * 0.5 + 0.5;
    return float2(rotX, rotY);
}

float4 main(PSInput input) : SV_Target
{
    float2 texCoord = input.TexCoord;
    float3 rotOffset = float3(1.425, 3.892, 5.835);

    float2 rotCoordsR = coordRot(texCoord, Timer + rotOffset.x);
    float3 noise = pnoise3D(float3(rotCoordsR * ScreenSize / GrainSize, 0.0)).xxx;

    if (ColorAmount > 0)
    {
        float2 rotCoordsG = coordRot(texCoord, Timer + rotOffset.y);
        float2 rotCoordsB = coordRot(texCoord, Timer + rotOffset.z);
        noise.g = lerp(noise.r, pnoise3D(float3(rotCoordsG * ScreenSize / GrainSize, 1.0)), ColorAmount);
        noise.b = lerp(noise.r, pnoise3D(float3(rotCoordsB * ScreenSize / GrainSize, 2.0)), ColorAmount);
    }

    float3 col = SceneTexture.Sample(PointSampler, texCoord).rgb;

    const float3 lumcoeff = float3(0.299, 0.587, 0.114);
    float luminance = lerp(0.0, dot(col, lumcoeff), LumAmount);
    float lum = smoothstep(0.2, 0.0, luminance);
    lum += luminance;

    noise = lerp(noise, 0.0, pow(lum, 4.0));
    col = col + noise * GrainAmount;

    return float4(saturate(col), 1.0);
}
)";

// ============================================================================
// Constant buffer layout (must match HLSL cbuffer GrainParams)
// ============================================================================
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

// ============================================================================
// Helper: resolve TYPELESS formats to typed equivalents for views/PSOs
// ============================================================================
static DXGI_FORMAT resolve_typeless_format(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_FLOAT;
        default: return fmt;
    }
}

// ============================================================================
// Default parameter values
// ============================================================================
static constexpr float DEFAULT_GRAIN_AMOUNT = 0.05f;
static constexpr float DEFAULT_COLOR_AMOUNT = 0.6f;
static constexpr float DEFAULT_LUM_AMOUNT   = 1.0f;
static constexpr float DEFAULT_GRAIN_SIZE   = 1.6f;
static constexpr const char* FILMGRAIN2_VERSION = "1.0.0";

// ============================================================================
// FilmGrain2 Plugin
// ============================================================================
class FilmGrain2Plugin : public uevr::Plugin {
public:
    // Tunable parameters
    float m_grain_amount = DEFAULT_GRAIN_AMOUNT;
    float m_color_amount = DEFAULT_COLOR_AMOUNT;
    float m_lum_amount   = DEFAULT_LUM_AMOUNT;
    float m_grain_size   = DEFAULT_GRAIN_SIZE;
    bool  m_enabled      = false;

    // D3D11 effect resources
    ComPtr<ID3D11DeviceContext>      m_dx11_ctx;
    ComPtr<ID3D11VertexShader>       m_vs;
    ComPtr<ID3D11PixelShader>        m_ps;
    ComPtr<ID3D11Buffer>             m_cb;
    ComPtr<ID3D11SamplerState>       m_sampler;
    bool m_shader_ready = false;
    bool m_dx11_logged_flags = false;
    bool m_dx11_first_apply_logged = false;

    // DX11 per-target state — cached by texture dimensions (2-slot cache)
    struct DX11TargetState {
        ComPtr<ID3D11Texture2D>          copy_tex;
        ComPtr<ID3D11ShaderResourceView> copy_srv;
        ComPtr<ID3D11RenderTargetView>   rtv;
        ID3D11Texture2D*                 rtv_tex = nullptr;
        UINT width  = 0;
        UINT height = 0;
        bool cache_hit_logged = false;

        void reset() {
            copy_tex.Reset(); copy_srv.Reset(); rtv.Reset();
            rtv_tex = nullptr; width = 0; height = 0;
            cache_hit_logged = false;
        }
    };
    static constexpr size_t MAX_DX11_TARGETS = 2;
    DX11TargetState m_dx11_targets[MAX_DX11_TARGETS];

    DX11TargetState& find_dx11_target_state(UINT w, UINT h) {
        for (size_t i = 0; i < MAX_DX11_TARGETS; ++i) {
            auto& ts = m_dx11_targets[i];
            if (ts.width == w && ts.height == h) {
                if (!ts.cache_hit_logged) {
                    ts.cache_hit_logged = true;
                    API::get()->log_info("[FilmGrain2] DX11 cache hit: slot %d (%ux%u)", (int)i, w, h);
                }
                return ts;
            }
        }
        for (auto& ts : m_dx11_targets)
            if (ts.width == 0) return ts;
        m_dx11_targets[1].reset();
        return m_dx11_targets[1];
    }

    // DX12 effect resources — shared across targets
    ComPtr<ID3D12RootSignature>  m_dx12_root_sig;
    ComPtr<ID3D12PipelineState>  m_dx12_pso;
    ComPtr<ID3D12Resource>       m_dx12_cb;

    struct DX12TargetState {
        ComPtr<ID3D12Resource>       copy_tex;
        ComPtr<ID3D12DescriptorHeap> srv_heap;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        UINT width  = 0;
        UINT height = 0;

        void reset() {
            copy_tex.Reset();
            srv_heap.Reset(); rtv_heap.Reset();
            width = 0; height = 0;
        }
    };
    static constexpr size_t MAX_TARGET_STATES = 2;
    DX12TargetState m_dx12_targets[MAX_TARGET_STATES];

    DX12TargetState& find_target_state(UINT w, UINT h) {
        for (auto& ts : m_dx12_targets)
            if (ts.width == w && ts.height == h) return ts;
        for (auto& ts : m_dx12_targets)
            if (ts.width == 0) return ts;
        m_dx12_targets[1].reset();
        return m_dx12_targets[1];
    }

    bool m_dx12_ready = false;
    GrainParamsCB* m_dx12_cb_mapped = nullptr;
    DXGI_FORMAT m_dx12_rt_format = DXGI_FORMAT_UNKNOWN;

    void on_dllmain() override {}

    void on_initialize() override {
        API::get()->log_info("[FilmGrain2] Plugin initialized");
        load_settings();
    }

    // ========================================================================
    // Settings persistence
    // ========================================================================
    std::filesystem::path get_settings_path() {
        return API::get()->get_persistent_dir() / L"data" / L"plugins" / L"shader_settings" / L"filmgrain2_settings.txt";
    }

    void save_settings() {
        try {
            std::filesystem::create_directories(get_settings_path().parent_path());
            std::ofstream f(get_settings_path());
            if (f.is_open()) {
                f << m_enabled << "\n" << m_grain_amount << "\n" << m_color_amount << "\n"
                  << m_lum_amount << "\n" << m_grain_size << "\n";
            }
        } catch (...) {}
    }

    void load_settings() {
        try {
            std::ifstream f(get_settings_path());
            if (f.is_open()) {
                int enabled_int;
                if (f >> enabled_int >> m_grain_amount >> m_color_amount >> m_lum_amount >> m_grain_size) {
                    m_enabled = (enabled_int != 0);
                    // Clamp loaded values
                    m_grain_amount = max(0.0f, min(0.2f, m_grain_amount));
                    m_color_amount = max(0.0f, min(1.0f, m_color_amount));
                    m_lum_amount   = max(0.0f, min(1.0f, m_lum_amount));
                    m_grain_size   = max(1.5f, min(2.5f, m_grain_size));
                    API::get()->log_info("[FilmGrain2] Loaded settings: enabled=%d grain=%.3f color=%.2f lum=%.2f size=%.2f",
                        m_enabled, m_grain_amount, m_color_amount, m_lum_amount, m_grain_size);
                }
            }
        } catch (...) {}
    }

    // ========================================================================
    // on_draw_ui
    // ========================================================================
    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("FilmGrain2 Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", FILMGRAIN2_VERSION);
            ImGui::TextWrapped("Adds photographic film grain. Hides color banding in dark areas (common on VR panels). Keep it subtle — high values look noisy.");
            bool changed = false;

            changed |= ImGui::Checkbox("Enabled", &m_enabled);
            changed |= ImGui::DragFloat("Grain Amount", &m_grain_amount, 0.001f, 0.0f, 0.2f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overall grain intensity. Keep low (0.02-0.05) for subtle effect");
            changed |= ImGui::DragFloat("Color Amount", &m_color_amount, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = monochrome grain, 1 = full color grain");
            changed |= ImGui::DragFloat("Luminance Amount", &m_lum_amount, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much grain varies with brightness. 1 = more grain in dark areas");
            changed |= ImGui::DragFloat("Grain Size", &m_grain_size, 0.01f, 1.5f, 2.5f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Size of grain particles. Higher = coarser grain");

            if (ImGui::Button("Reset to Defaults")) {
                m_grain_amount = DEFAULT_GRAIN_AMOUNT;
                m_color_amount = DEFAULT_COLOR_AMOUNT;
                m_lum_amount   = DEFAULT_LUM_AMOUNT;
                m_grain_size   = DEFAULT_GRAIN_SIZE;
                changed = true;
            }

            if (changed) {
                save_settings();
            }
        }
    }

    // ========================================================================
    // Timer helper — milliseconds, used as seed for grain animation
    // ========================================================================
    float get_timer_ms() {
        return (float)(GetTickCount64() % 1000000);
    }

    // ========================================================================
    // on_pre_render_vr_framework_dx12
    // ========================================================================
    void on_pre_render_vr_framework_dx12() override {
        if (!m_enabled) return;

        const auto renderer_data = API::get()->param()->renderer;
        if (renderer_data->renderer_type != UEVR_RENDERER_D3D12) return;
        if (!renderer_data->device) return;

        auto scene_rt = API::StereoHook::get_scene_render_target();
        if (!scene_rt) return;

        auto native = (ID3D12Resource*)scene_rt->get_native_resource();
        if (!native) return;
        auto desc = native->GetDesc();
        if (desc.Width == 0 || desc.Height == 0) return;

        auto device = (ID3D12Device*)renderer_data->device;
        const auto resolved_format = resolve_typeless_format(desc.Format);

        if (!m_dx12_ready || m_dx12_rt_format != resolved_format) {
            if (m_dx12_ready) release_effect_resources();
            if (!init_dx12_pipeline(device, desc.Format)) return;
        }
        auto& ts = find_target_state((UINT)desc.Width, desc.Height);
        if (!ensure_dx12_copy_texture(device, native, ts)) return;

        auto cmd = (ID3D12GraphicsCommandList*)API::StereoHook::get_pre_render_command_list();
        if (!cmd) return;

        // Update CB
        if (m_dx12_cb_mapped) {
            m_dx12_cb_mapped->GrainAmount = m_grain_amount;
            m_dx12_cb_mapped->ColorAmount = m_color_amount;
            m_dx12_cb_mapped->LumAmount = m_lum_amount;
            m_dx12_cb_mapped->GrainSize = m_grain_size;
            m_dx12_cb_mapped->Timer = get_timer_ms();
            m_dx12_cb_mapped->ScreenSizeX = (float)ts.width;
            m_dx12_cb_mapped->ScreenSizeY = (float)ts.height;
            m_dx12_cb_mapped->AspectRatio = (float)ts.width / (float)ts.height;
        }

        // Step 1: Copy target -> copy_tex (for SRV sampling)
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = native;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = ts.copy_tex.Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(2, barriers);
        cmd->CopyResource(ts.copy_tex.Get(), native);

        // Transition back
        barriers[0].Transition.pResource = native;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.pResource = ts.copy_tex.Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(2, barriers);

        // Step 2: Create RTV on native target
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = resolved_format;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(native, &rtv_desc, ts.rtv_heap->GetCPUDescriptorHandleForHeapStart());

        // Draw FilmGrain2: read copy_tex (SRV) -> write native (RTV)
        cmd->SetPipelineState(m_dx12_pso.Get());
        cmd->SetGraphicsRootSignature(m_dx12_root_sig.Get());
        cmd->SetGraphicsRootConstantBufferView(0, m_dx12_cb->GetGPUVirtualAddress());
        ID3D12DescriptorHeap* heaps[] = { ts.srv_heap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootDescriptorTable(1, ts.srv_heap->GetGPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = ts.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
        D3D12_VIEWPORT vp{}; vp.Width = (float)ts.width; vp.Height = (float)ts.height; vp.MaxDepth = 1.0f;
        cmd->RSSetViewports(1, &vp);
        D3D12_RECT scissor{}; scissor.right = (LONG)ts.width; scissor.bottom = (LONG)ts.height;
        cmd->RSSetScissorRects(1, &scissor);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    // ========================================================================
    // on_pre_render_vr_framework_dx11
    // ========================================================================
    void on_pre_render_vr_framework_dx11() override {
        if (!m_enabled) return;

        const auto renderer_data = API::get()->param()->renderer;
        if (renderer_data->renderer_type != UEVR_RENDERER_D3D11) return;

        auto scene_rt = API::StereoHook::get_scene_render_target();
        if (!scene_rt) return;

        auto native = (ID3D11Texture2D*)scene_rt->get_native_resource();
        if (!native) return;

        apply_filmgrain2_to_resource_dx11(native);
    }

    void on_present() override {}
    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {}

    void on_device_reset() override {
        API::get()->log_info("[FilmGrain2] Device reset");
        release_effect_resources();
    }

    void on_post_render_vr_framework_dx11(
        ID3D11DeviceContext* context, ID3D11Texture2D* texture, ID3D11RenderTargetView* rtv) override {}
    void on_post_render_vr_framework_dx12(
        ID3D12GraphicsCommandList* command_list, ID3D12Resource* rt, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {}

private:

    // ========================================================================
    // Resource cleanup
    // ========================================================================
    void release_effect_resources() {
        // DX11
        m_dx11_ctx.Reset();
        m_vs.Reset(); m_ps.Reset(); m_cb.Reset(); m_sampler.Reset();
        for (auto& ts : m_dx11_targets) ts.reset();
        m_shader_ready = false;
        m_dx11_logged_flags = false;
        m_dx11_first_apply_logged = false;

        // DX12
        m_dx12_pso.Reset(); m_dx12_root_sig.Reset(); m_dx12_cb.Reset();
        for (auto& ts : m_dx12_targets) ts.reset();
        m_dx12_ready = false; m_dx12_cb_mapped = nullptr; m_dx12_rt_format = DXGI_FORMAT_UNKNOWN;
    }

    // ========================================================================
    // DX11: Apply FilmGrain2 to UE4 scene render target
    // ========================================================================
    bool init_shaders_dx11(ID3D11Device* device) {
        if (!device) return false;

        ComPtr<ID3DBlob> vs_blob, vs_err;
        if (FAILED(D3DCompile(g_filmgrain2_vs_src, strlen(g_filmgrain2_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs_blob, &vs_err))) return false;
        ComPtr<ID3DBlob> ps_blob, ps_err;
        if (FAILED(D3DCompile(g_filmgrain2_ps_src, strlen(g_filmgrain2_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps_blob, &ps_err))) {
            if (ps_err) API::get()->log_error("[FilmGrain2] PS compile: %s", (const char*)ps_err->GetBufferPointer());
            return false;
        }

        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs))) return false;
        if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps))) return false;

        D3D11_BUFFER_DESC cb_desc{}; cb_desc.ByteWidth = sizeof(GrainParamsCB);
        cb_desc.Usage = D3D11_USAGE_DYNAMIC; cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &m_cb))) return false;

        // Point sampler — FilmGrain2 reads exactly one texel per pixel (no filtering needed)
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &m_sampler))) return false;

        m_shader_ready = true;
        API::get()->log_info("[FilmGrain2] DX11 shaders ready");
        return true;
    }

    bool ensure_dx11_copy_texture(ID3D11Device* device, ID3D11Texture2D* src, DX11TargetState& ts) {
        D3D11_TEXTURE2D_DESC src_desc{}; src->GetDesc(&src_desc);
        const auto resolved_fmt = resolve_typeless_format(src_desc.Format);
        if (ts.copy_tex && ts.width == src_desc.Width && ts.height == src_desc.Height) return true;
        ts.reset();

        D3D11_TEXTURE2D_DESC cd = src_desc;
        cd.Format = resolved_fmt;
        cd.MipLevels = 1;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.MiscFlags = 0;
        cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0;
        if (FAILED(device->CreateTexture2D(&cd, nullptr, &ts.copy_tex))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC svd{}; svd.Format = resolved_fmt;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; svd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(ts.copy_tex.Get(), &svd, &ts.copy_srv))) { ts.reset(); return false; }

        ts.width = src_desc.Width; ts.height = src_desc.Height;
        API::get()->log_info("[FilmGrain2] DX11 copy texture created: %ux%u", ts.width, ts.height);
        return true;
    }

    void apply_filmgrain2_to_resource_dx11(ID3D11Texture2D* target) {
        const auto rd = API::get()->param()->renderer;
        auto device = (ID3D11Device*)rd->device;
        if (!device || !target) return;

        D3D11_TEXTURE2D_DESC target_desc{};
        target->GetDesc(&target_desc);
        if (target_desc.Width == 0 || target_desc.Height == 0) return;

        if (!m_dx11_logged_flags) {
            m_dx11_logged_flags = true;
            API::get()->log_info("[FilmGrain2] DX11 RT: %ux%u fmt=%u bind=0x%x",
                target_desc.Width, target_desc.Height, target_desc.Format,
                target_desc.BindFlags);
        }

        if (!m_dx11_ctx) device->GetImmediateContext(&m_dx11_ctx);
        auto ctx = m_dx11_ctx.Get();

        if (!m_shader_ready && !init_shaders_dx11(device)) return;

        auto& ts = find_dx11_target_state(target_desc.Width, target_desc.Height);
        if (!ensure_dx11_copy_texture(device, target, ts)) return;

        if (ts.rtv_tex != target) {
            ts.rtv.Reset();
            const auto resolved_fmt = resolve_typeless_format(target_desc.Format);
            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format = resolved_fmt;
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtv_desc.Texture2D.MipSlice = 0;
            if (FAILED(device->CreateRenderTargetView(target, &rtv_desc, &ts.rtv))) return;
            ts.rtv_tex = target;
        }

        // Update constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* p = static_cast<GrainParamsCB*>(mapped.pData);
            p->GrainAmount = m_grain_amount;
            p->ColorAmount = m_color_amount;
            p->LumAmount = m_lum_amount;
            p->GrainSize = m_grain_size;
            p->Timer = get_timer_ms();
            p->ScreenSizeX = (float)ts.width;
            p->ScreenSizeY = (float)ts.height;
            p->AspectRatio = (float)ts.width / (float)ts.height;
            ctx->Unmap(m_cb.Get(), 0);
        }

        ctx->CopyResource(ts.copy_tex.Get(), target);

        D3D11_VIEWPORT vp{}; vp.Width = (float)ts.width; vp.Height = (float)ts.height; vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtv_raw = ts.rtv.Get();
        ctx->OMSetRenderTargets(1, &rtv_raw, nullptr);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        ctx->PSSetShader(m_ps.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, ts.copy_srv.GetAddressOf());
        ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
        ctx->Draw(3, 0);

        if (!m_dx11_first_apply_logged) {
            m_dx11_first_apply_logged = true;
            API::get()->log_info("[FilmGrain2] DX11 effect applied: %ux%u", ts.width, ts.height);
        }

        ID3D11ShaderResourceView* null_srv = nullptr;
        ctx->PSSetShaderResources(0, 1, &null_srv);
    }

    // ========================================================================
    // DX12: Apply FilmGrain2 to UE4 scene render target
    // ========================================================================
    bool init_dx12_pipeline(ID3D12Device* device, DXGI_FORMAT rt_format) {
        if (m_dx12_ready) return true;
        if (!device) return false;

        rt_format = resolve_typeless_format(rt_format);

        // Root signature
        D3D12_DESCRIPTOR_RANGE1 srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        D3D12_ROOT_PARAMETER1 rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // Point sampler — no filtering needed
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1.NumParameters = 2; rsd.Desc_1_1.pParameters = rp;
        rsd.Desc_1_1.NumStaticSamplers = 1; rsd.Desc_1_1.pStaticSamplers = &ss;

        ComPtr<ID3DBlob> sig_blob, sig_err;
        if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &sig_blob, &sig_err))) {
            if (sig_err) API::get()->log_error("[FilmGrain2 DX12] Root sig: %s", (const char*)sig_err->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&m_dx12_root_sig)))) return false;

        // Shaders
        ComPtr<ID3DBlob> vsb, vse, psb, pse;
        if (FAILED(D3DCompile(g_filmgrain2_vs_src, strlen(g_filmgrain2_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &vse))) return false;
        if (FAILED(D3DCompile(g_filmgrain2_ps_src, strlen(g_filmgrain2_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &pse))) {
            if (pse) API::get()->log_error("[FilmGrain2 DX12] PS compile: %s", (const char*)pse->GetBufferPointer());
            return false;
        }

        // PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_dx12_root_sig.Get();
        pd.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() };
        pd.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = rt_format;
        pd.SampleDesc.Count = 1;

        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_dx12_pso)))) {
            API::get()->log_error("[FilmGrain2 DX12] PSO failed for format %u", (unsigned)rt_format);
            return false;
        }

        // CB (upload heap, persistently mapped)
        D3D12_HEAP_PROPERTIES uh{}; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbd{}; cbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbd.Width = 256; cbd.Height = 1; cbd.DepthOrArraySize = 1; cbd.MipLevels = 1;
        cbd.Format = DXGI_FORMAT_UNKNOWN; cbd.SampleDesc.Count = 1; cbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &cbd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_dx12_cb)))) return false;
        m_dx12_cb->Map(0, nullptr, (void**)&m_dx12_cb_mapped);

        m_dx12_rt_format = rt_format;
        m_dx12_ready = true;
        API::get()->log_info("[FilmGrain2 DX12] Pipeline ready (format %u)", (unsigned)rt_format);
        return true;
    }

    bool ensure_dx12_copy_texture(ID3D12Device* device, ID3D12Resource* src, DX12TargetState& ts) {
        auto sd = src->GetDesc();
        if (ts.copy_tex && ts.width == (UINT)sd.Width && ts.height == sd.Height) return true;
        ts.reset();

        const auto resolved_fmt = resolve_typeless_format(sd.Format);
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC copy_desc = sd;
        copy_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        copy_desc.Format = resolved_fmt;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &copy_desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&ts.copy_tex)))) {
            API::get()->log_error("[FilmGrain2 DX12] Failed to create copy texture");
            return false;
        }

        // SRV heap
        D3D12_DESCRIPTOR_HEAP_DESC shd{}; shd.NumDescriptors = 1;
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&ts.srv_heap)))) { ts.reset(); return false; }

        // RTV heap
        D3D12_DESCRIPTOR_HEAP_DESC rhd{}; rhd.NumDescriptors = 1; rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&ts.rtv_heap)))) { ts.reset(); return false; }

        // SRV for copy texture
        D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = resolved_fmt;
        svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        svd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(ts.copy_tex.Get(), &svd, ts.srv_heap->GetCPUDescriptorHandleForHeapStart());

        ts.width = (UINT)sd.Width; ts.height = sd.Height;
        API::get()->log_info("[FilmGrain2 DX12] Copy texture ready: %ux%u fmt %u", ts.width, ts.height, (unsigned)resolved_fmt);
        return true;
    }
};

// ============================================================================
// Plugin entry point
// ============================================================================
std::unique_ptr<FilmGrain2Plugin> g_plugin{new FilmGrain2Plugin()};
