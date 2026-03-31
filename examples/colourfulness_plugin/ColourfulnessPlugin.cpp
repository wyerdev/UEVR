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
// Embedded HLSL source
// ============================================================================

static const char* g_colourfulness_vs_src = R"(
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

// Faithful port of bacondither's Colourfulness v2018-11-12
// Using fast_luma=1 path (rapid approx of sRGB gamma)
// Dithering controlled via CB bools; temporal_dither disabled (no random uniform in plugin)
static const char* g_colourfulness_ps_src = R"(
cbuffer ColourParams : register(b0) {
    float Colourfulness;   // -1.0 to 2.0, 0 = neutral
    float LimLuma;         // 0.1 to 1.0
    int   EnableDither;    // 0 or 1
    int   ColNoise;        // 0 or 1 (coloured dither noise)
    float BackbufferBits;  // typically 8 or 10
    float3 _pad0;
};

Texture2D SceneTexture : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput {
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

// Sigmoid function
float3 soft_lim(float3 v, float3 s)
{
    return (v * s) * rsqrt(s * s + v * v);
}

// Weighted power mean, p = 0.5
float3 wpmean(float3 a, float3 b, float w)
{
    float3 sa = sqrt(abs(a));
    float3 sb = sqrt(abs(b));
    return pow(abs(abs(w) * sa + abs(1.0 - w) * sb), 2.0);
}

// Mean of Rec. 709 & 601 luma coefficients
static const float3 lumacoeff = float3(0.2558, 0.6511, 0.0931);

float4 main(PSInput input) : SV_Target
{
    float4 vpos = input.Position;

    // fast_luma path
    float3 c0 = SceneTexture.Sample(PointSampler, input.TexCoord).rgb;
    float luma = sqrt(dot(saturate(c0 * abs(c0)), lumacoeff));
    c0 = saturate(c0);

    // Calc colour saturation change
    float3 diff_luma = c0 - luma;
    float3 c_diff = diff_luma * (Colourfulness + 1.0) - diff_luma;

    if (Colourfulness > 0.0)
    {
        // 120% of c_diff clamped to max visible range + overshoot
        float3 rlc_diff = clamp((c_diff * 1.2) + c0, -0.0001, 1.0001) - c0;

        // Calc max saturation-increase without altering RGB ratios
        float3 ad = abs(diff_luma);
        float maxC = max(ad.r, max(ad.g, ad.b));
        float minC = min(ad.r, min(ad.g, ad.b));

        float poslim = (1.0002 - luma) / (maxC + 0.0001);
        float neglim = (luma + 0.0002) / (minC + 0.0001);

        float3 diffmax = diff_luma * min(min(poslim, neglim), 32.0) - diff_luma;

        // Soft limit diff
        c_diff = soft_lim(c_diff, max(wpmean(diffmax, rlc_diff, LimLuma), 1e-7));
    }

    if (EnableDither)
    {
        // Interleaved gradient noise by Jorge Jimenez
        const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
        float xy_magic = vpos.x * magic.x + vpos.y * magic.y;
        float noise = (frac(magic.z * frac(xy_magic)) - 0.5) / (exp2(BackbufferBits) - 1.0);
        c_diff += ColNoise ? float3(-noise, noise, -noise) : noise;
    }

    return float4(saturate(c0 + c_diff), 1.0);
}
)";

// ============================================================================
// Constant buffer layout (must match HLSL cbuffer ColourParams)
// ============================================================================
struct ColourParamsCB {
    float Colourfulness;
    float LimLuma;
    int   EnableDither;
    int   ColNoise;
    float BackbufferBits;
    float _pad0[3];
};

// ============================================================================
// Helper: resolve TYPELESS formats
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
static constexpr float DEFAULT_COLOURFULNESS  = 0.4f;
static constexpr float DEFAULT_LIM_LUMA       = 0.7f;
static constexpr float DEFAULT_BB_BITS        = 10.0f;  // R10G10B10A2 in UEVR
static constexpr const char* COLOURFULNESS_VERSION = "1.0.0";

// ============================================================================
// Colourfulness Plugin
// ============================================================================
class ColourfulnessPlugin : public uevr::Plugin {
public:
    float m_colourfulness = DEFAULT_COLOURFULNESS;
    float m_lim_luma      = DEFAULT_LIM_LUMA;
    bool  m_enable_dither = false;
    bool  m_col_noise     = true;
    float m_bb_bits       = DEFAULT_BB_BITS;
    bool  m_enabled       = false;

    // D3D11 effect resources
    ComPtr<ID3D11DeviceContext>      m_dx11_ctx;
    ComPtr<ID3D11VertexShader>       m_vs;
    ComPtr<ID3D11PixelShader>        m_ps;
    ComPtr<ID3D11Buffer>             m_cb;
    ComPtr<ID3D11SamplerState>       m_sampler;
    bool m_shader_ready = false;
    bool m_dx11_logged_flags = false;
    bool m_dx11_first_apply_logged = false;

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
                    API::get()->log_info("[Colourfulness] DX11 cache hit: slot %d (%ux%u)", (int)i, w, h);
                }
                return ts;
            }
        }
        for (auto& ts : m_dx11_targets)
            if (ts.width == 0) return ts;
        m_dx11_targets[1].reset();
        return m_dx11_targets[1];
    }

    // DX12 effect resources
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
    ColourParamsCB* m_dx12_cb_mapped = nullptr;
    DXGI_FORMAT m_dx12_rt_format = DXGI_FORMAT_UNKNOWN;

    void on_dllmain() override {}

    void on_initialize() override {
        API::get()->log_info("[Colourfulness] Plugin initialized");
        load_settings();
    }

    // ========================================================================
    // Settings persistence
    // ========================================================================
    std::filesystem::path get_settings_path() {
        return API::get()->get_persistent_dir(L"colourfulness_settings.txt");
    }

    void save_settings() {
        try {
            std::ofstream f(get_settings_path());
            if (f.is_open()) {
                f << m_enabled << "\n" << m_colourfulness << "\n" << m_lim_luma << "\n"
                  << m_enable_dither << "\n" << m_col_noise << "\n" << m_bb_bits << "\n";
            }
        } catch (...) {}
    }

    void load_settings() {
        try {
            std::ifstream f(get_settings_path());
            if (f.is_open()) {
                int enabled_int, dither_int, colnoise_int;
                if (f >> enabled_int >> m_colourfulness >> m_lim_luma
                      >> dither_int >> colnoise_int >> m_bb_bits)
                {
                    m_enabled = (enabled_int != 0);
                    m_enable_dither = (dither_int != 0);
                    m_col_noise = (colnoise_int != 0);
                    m_colourfulness = max(-1.0f, min(2.0f, m_colourfulness));
                    m_lim_luma = max(0.1f, min(1.0f, m_lim_luma));
                    m_bb_bits = max(1.0f, min(32.0f, m_bb_bits));
                    API::get()->log_info("[Colourfulness] Loaded settings: enabled=%d colour=%.2f lim=%.2f dither=%d",
                        m_enabled, m_colourfulness, m_lim_luma, m_enable_dither);
                }
            }
        } catch (...) {}
    }

    // ========================================================================
    // Helper: fill CB data
    // ========================================================================
    void fill_cb(ColourParamsCB* p) {
        p->Colourfulness = m_colourfulness;
        p->LimLuma = m_lim_luma;
        p->EnableDither = m_enable_dither ? 1 : 0;
        p->ColNoise = m_col_noise ? 1 : 0;
        p->BackbufferBits = m_bb_bits;
        p->_pad0[0] = 0; p->_pad0[1] = 0; p->_pad0[2] = 0;
    }

    // ========================================================================
    // on_draw_ui
    // ========================================================================
    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Colourfulness Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", COLOURFULNESS_VERSION);
            bool changed = false;

            changed |= ImGui::Checkbox("Enabled", &m_enabled);
            changed |= ImGui::SliderFloat("Colourfulness", &m_colourfulness, -1.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Degree of colourfulness, 0 = neutral");
            changed |= ImGui::SliderFloat("Luma Limiter", &m_lim_luma, 0.1f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower values allow more change near clipping");

            ImGui::Separator();
            ImGui::Text("Dither");
            changed |= ImGui::Checkbox("Enable Dither", &m_enable_dither);
            if (m_enable_dither) {
                changed |= ImGui::Checkbox("Coloured Noise", &m_col_noise);
                changed |= ImGui::SliderFloat("Backbuffer Bits", &m_bb_bits, 1.0f, 32.0f, "%.0f");
            }

            if (ImGui::Button("Reset to Defaults")) {
                m_colourfulness = DEFAULT_COLOURFULNESS;
                m_lim_luma = DEFAULT_LIM_LUMA;
                m_enable_dither = false;
                m_col_noise = true;
                m_bb_bits = DEFAULT_BB_BITS;
                changed = true;
            }

            if (changed) save_settings();
        }
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

        if (m_dx12_cb_mapped) fill_cb(m_dx12_cb_mapped);

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

        barriers[0].Transition.pResource = native;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.pResource = ts.copy_tex.Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(2, barriers);

        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = resolved_format;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(native, &rtv_desc, ts.rtv_heap->GetCPUDescriptorHandleForHeapStart());

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

        apply_colourfulness_to_resource_dx11(native);
    }

    void on_present() override {}
    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {}

    void on_device_reset() override {
        API::get()->log_info("[Colourfulness] Device reset");
        release_effect_resources();
    }

    void on_post_render_vr_framework_dx11(
        ID3D11DeviceContext* context, ID3D11Texture2D* texture, ID3D11RenderTargetView* rtv) override {}
    void on_post_render_vr_framework_dx12(
        ID3D12GraphicsCommandList* command_list, ID3D12Resource* rt, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {}

private:

    void release_effect_resources() {
        m_dx11_ctx.Reset();
        m_vs.Reset(); m_ps.Reset(); m_cb.Reset(); m_sampler.Reset();
        for (auto& ts : m_dx11_targets) ts.reset();
        m_shader_ready = false;
        m_dx11_logged_flags = false;
        m_dx11_first_apply_logged = false;

        m_dx12_pso.Reset(); m_dx12_root_sig.Reset(); m_dx12_cb.Reset();
        for (auto& ts : m_dx12_targets) ts.reset();
        m_dx12_ready = false; m_dx12_cb_mapped = nullptr; m_dx12_rt_format = DXGI_FORMAT_UNKNOWN;
    }

    // ========================================================================
    // DX11
    // ========================================================================
    bool init_shaders_dx11(ID3D11Device* device) {
        if (!device) return false;

        ComPtr<ID3DBlob> vs_blob, vs_err;
        if (FAILED(D3DCompile(g_colourfulness_vs_src, strlen(g_colourfulness_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs_blob, &vs_err))) return false;
        ComPtr<ID3DBlob> ps_blob, ps_err;
        if (FAILED(D3DCompile(g_colourfulness_ps_src, strlen(g_colourfulness_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps_blob, &ps_err))) {
            if (ps_err) API::get()->log_error("[Colourfulness] PS compile: %s", (const char*)ps_err->GetBufferPointer());
            return false;
        }

        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs))) return false;
        if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps))) return false;

        D3D11_BUFFER_DESC cb_desc{}; cb_desc.ByteWidth = sizeof(ColourParamsCB);
        cb_desc.Usage = D3D11_USAGE_DYNAMIC; cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &m_cb))) return false;

        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &m_sampler))) return false;

        m_shader_ready = true;
        API::get()->log_info("[Colourfulness] DX11 shaders ready");
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
        API::get()->log_info("[Colourfulness] DX11 copy texture created: %ux%u", ts.width, ts.height);
        return true;
    }

    void apply_colourfulness_to_resource_dx11(ID3D11Texture2D* target) {
        const auto rd = API::get()->param()->renderer;
        auto device = (ID3D11Device*)rd->device;
        if (!device || !target) return;

        D3D11_TEXTURE2D_DESC target_desc{};
        target->GetDesc(&target_desc);
        if (target_desc.Width == 0 || target_desc.Height == 0) return;

        if (!m_dx11_logged_flags) {
            m_dx11_logged_flags = true;
            API::get()->log_info("[Colourfulness] DX11 RT: %ux%u fmt=%u bind=0x%x",
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

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            fill_cb(static_cast<ColourParamsCB*>(mapped.pData));
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
            API::get()->log_info("[Colourfulness] DX11 effect applied: %ux%u", ts.width, ts.height);
        }

        ID3D11ShaderResourceView* null_srv = nullptr;
        ctx->PSSetShaderResources(0, 1, &null_srv);
    }

    // ========================================================================
    // DX12
    // ========================================================================
    bool init_dx12_pipeline(ID3D12Device* device, DXGI_FORMAT rt_format) {
        if (m_dx12_ready) return true;
        if (!device) return false;

        rt_format = resolve_typeless_format(rt_format);

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
            if (sig_err) API::get()->log_error("[Colourfulness DX12] Root sig: %s", (const char*)sig_err->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&m_dx12_root_sig)))) return false;

        ComPtr<ID3DBlob> vsb, vse, psb, pse;
        if (FAILED(D3DCompile(g_colourfulness_vs_src, strlen(g_colourfulness_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &vse))) return false;
        if (FAILED(D3DCompile(g_colourfulness_ps_src, strlen(g_colourfulness_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &pse))) {
            if (pse) API::get()->log_error("[Colourfulness DX12] PS compile: %s", (const char*)pse->GetBufferPointer());
            return false;
        }

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
            API::get()->log_error("[Colourfulness DX12] PSO failed for format %u", (unsigned)rt_format);
            return false;
        }

        D3D12_HEAP_PROPERTIES uh{}; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbd{}; cbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbd.Width = 256; cbd.Height = 1; cbd.DepthOrArraySize = 1; cbd.MipLevels = 1;
        cbd.Format = DXGI_FORMAT_UNKNOWN; cbd.SampleDesc.Count = 1; cbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &cbd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_dx12_cb)))) return false;
        m_dx12_cb->Map(0, nullptr, (void**)&m_dx12_cb_mapped);

        m_dx12_rt_format = rt_format;
        m_dx12_ready = true;
        API::get()->log_info("[Colourfulness DX12] Pipeline ready (format %u)", (unsigned)rt_format);
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
            API::get()->log_error("[Colourfulness DX12] Failed to create copy texture");
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC shd{}; shd.NumDescriptors = 1;
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&ts.srv_heap)))) { ts.reset(); return false; }

        D3D12_DESCRIPTOR_HEAP_DESC rhd{}; rhd.NumDescriptors = 1; rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&ts.rtv_heap)))) { ts.reset(); return false; }

        D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = resolved_fmt;
        svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        svd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(ts.copy_tex.Get(), &svd, ts.srv_heap->GetCPUDescriptorHandleForHeapStart());

        ts.width = (UINT)sd.Width; ts.height = sd.Height;
        API::get()->log_info("[Colourfulness DX12] Copy texture ready: %ux%u fmt %u", ts.width, ts.height, (unsigned)resolved_fmt);
        return true;
    }
};

// ============================================================================
// Plugin entry point
// ============================================================================
std::unique_ptr<ColourfulnessPlugin> g_plugin{new ColourfulnessPlugin()};
