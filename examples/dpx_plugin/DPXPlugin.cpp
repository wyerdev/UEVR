/*
DPX Plugin for UEVR
====================
A UEVR C++ plugin that applies Loadus's DPX/Cineon film stock color effect to VR frames.
Emulates Cineon film stock color science with RGB curve, contrast, saturation, colorfulness.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

Original shader:
  DPX/Cineon shader
  by Loadus
  Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/DPX.fx
  From the crosire/reshade-shaders community collection.
  No explicit license was provided in the original file or repository.
  All rights remain with the original author.
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

static const char* g_dpx_vs_src = R"(
struct VSOutput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };
VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput o; o.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    o.Position = float4(o.TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); return o;
}
)";

// Faithful port of Loadus's DPX/Cineon shader
static const char* g_dpx_ps_src = R"(
cbuffer DPXParams : register(b0) {
    float3 RGB_Curve;
    float  Contrast;
    float3 RGB_C;
    float  Saturation;
    float  Colorfulness;
    float  Strength;
    float2 _pad;
};

Texture2D SceneTexture : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float4 main(PSInput input) : SV_Target
{
    static const float3x3 XYZ = float3x3(
        0.5003033835433160,  0.3380975732227390,  0.1645897795458570,
        0.2579688942747580,  0.6761952591447060,  0.0658358459823868,
        0.0234517888692628,  0.1126992737203000,  0.8668396731242010
    );
    static const float3x3 RGB = float3x3(
         2.6714711726599600, -1.2672360578624100, -0.4109956021722270,
        -1.0251070293466400,  1.9840911624108900,  0.0439502493584124,
         0.0610009456429445, -0.2236707508128630,  1.1590210416706100
    );

    float3 inputColor = SceneTexture.Sample(PointSampler, input.TexCoord).rgb;

    float3 B = inputColor;
    B = B * (1.0 - Contrast) + (0.5 * Contrast);

    float3 Btemp = (1.0 / (1.0 + exp(RGB_Curve / 2.0)));
    B = ((1.0 / (1.0 + exp(-RGB_Curve * (B - RGB_C)))) / (-2.0 * Btemp + 1.0)) + (-Btemp / (-2.0 * Btemp + 1.0));

    float value = max(max(B.r, B.g), B.b);
    float3 color = B / value;
    color = pow(abs(color), 1.0 / Colorfulness);

    float3 c0 = color * value;
    c0 = mul(XYZ, c0);
    float luma = dot(c0, float3(0.30, 0.59, 0.11));
    c0 = (1.0 - Saturation) * luma + Saturation * c0;
    c0 = mul(RGB, c0);

    return float4(lerp(inputColor, c0, Strength), 1.0);
}
)";

struct DPXParamsCB {
    float RGB_Curve[3];
    float Contrast;
    float RGB_C[3];
    float Saturation;
    float Colorfulness;
    float Strength;
    float _pad[2];
};

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

static constexpr const char* DPX_VERSION = "1.0.0";

class DPXPlugin : public uevr::Plugin {
public:
    float m_rgb_curve[3] = {8.0f, 8.0f, 8.0f};
    float m_rgb_c[3] = {0.36f, 0.36f, 0.34f};
    float m_contrast = 0.1f, m_saturation = 3.0f, m_colorfulness = 2.5f, m_strength = 0.20f;
    bool  m_enabled = false;

    ComPtr<ID3D11DeviceContext> m_dx11_ctx;
    ComPtr<ID3D11VertexShader> m_vs; ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11Buffer> m_cb; ComPtr<ID3D11SamplerState> m_sampler;
    bool m_shader_ready = false;

    struct DX11TargetState {
        ComPtr<ID3D11Texture2D> copy_tex; ComPtr<ID3D11ShaderResourceView> copy_srv; ComPtr<ID3D11RenderTargetView> rtv;
        ID3D11Texture2D* rtv_tex = nullptr; UINT width = 0, height = 0;
        void reset() { copy_tex.Reset(); copy_srv.Reset(); rtv.Reset(); rtv_tex = nullptr; width = 0; height = 0; }
    };
    DX11TargetState m_dx11_targets[2];
    DX11TargetState& find_dx11_target(UINT w, UINT h) {
        for (auto& ts : m_dx11_targets) if (ts.width == w && ts.height == h) return ts;
        for (auto& ts : m_dx11_targets) if (ts.width == 0) return ts;
        m_dx11_targets[1].reset(); return m_dx11_targets[1];
    }

    ComPtr<ID3D12RootSignature> m_dx12_root_sig; ComPtr<ID3D12PipelineState> m_dx12_pso; ComPtr<ID3D12Resource> m_dx12_cb;
    DPXParamsCB* m_dx12_cb_mapped = nullptr; DXGI_FORMAT m_dx12_rt_format = DXGI_FORMAT_UNKNOWN; bool m_dx12_ready = false;
    struct DX12TargetState {
        ComPtr<ID3D12Resource> copy_tex; ComPtr<ID3D12DescriptorHeap> srv_heap, rtv_heap;
        UINT width = 0, height = 0;
        void reset() { copy_tex.Reset(); srv_heap.Reset(); rtv_heap.Reset(); width = 0; height = 0; }
    };
    DX12TargetState m_dx12_targets[2];
    DX12TargetState& find_dx12_target(UINT w, UINT h) {
        for (auto& ts : m_dx12_targets) if (ts.width == w && ts.height == h) return ts;
        for (auto& ts : m_dx12_targets) if (ts.width == 0) return ts;
        m_dx12_targets[1].reset(); return m_dx12_targets[1];
    }

    void on_dllmain() override {}
    void on_initialize() override { API::get()->log_info("[DPX] Plugin initialized"); load_settings(); }

    std::filesystem::path get_settings_path() {
        return API::get()->get_persistent_dir() / L"data" / L"plugins" / L"shader_settings" / L"dpx_settings.txt";
    }
    void save_settings() {
        try { std::filesystem::create_directories(get_settings_path().parent_path()); std::ofstream f(get_settings_path()); if (f.is_open())
            f << m_enabled << "\n" << m_rgb_curve[0] << " " << m_rgb_curve[1] << " " << m_rgb_curve[2] << "\n"
              << m_rgb_c[0] << " " << m_rgb_c[1] << " " << m_rgb_c[2] << "\n"
              << m_contrast << "\n" << m_saturation << "\n" << m_colorfulness << "\n" << m_strength << "\n";
        } catch (...) {}
    }
    void load_settings() {
        try { std::ifstream f(get_settings_path()); if (f.is_open()) { int e; if (f >> e) m_enabled = (e != 0);
            f >> m_rgb_curve[0] >> m_rgb_curve[1] >> m_rgb_curve[2];
            f >> m_rgb_c[0] >> m_rgb_c[1] >> m_rgb_c[2];
            f >> m_contrast >> m_saturation >> m_colorfulness >> m_strength;
        } } catch (...) {}
    }

    void fill_cb(DPXParamsCB* p) {
        memcpy(p->RGB_Curve, m_rgb_curve, sizeof(float)*3); p->Contrast = m_contrast;
        memcpy(p->RGB_C, m_rgb_c, sizeof(float)*3); p->Saturation = m_saturation;
        p->Colorfulness = m_colorfulness; p->Strength = m_strength;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("DPX Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", DPX_VERSION);
            ImGui::TextWrapped("Emulates Cineon film stock. Gives a warm, cinematic color shift. Good for games that look too cold or digital.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##DPX", &m_enabled);
            changed |= ImGui::DragFloat3("RGB Curve", m_rgb_curve, 0.1f, 1.0f, 15.0f, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cineon curve steepness per channel");
            changed |= ImGui::DragFloat3("RGB C", m_rgb_c, 0.01f, 0.2f, 0.5f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cineon curve center point per channel");
            changed |= ImGui::DragFloat("Contrast##DPX", &m_contrast, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contrast applied to the Cineon-graded image");
            changed |= ImGui::DragFloat("Saturation##DPX", &m_saturation, 0.1f, 0.0f, 8.0f, "%.1f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color saturation of the film emulation. Higher = more vivid");
            changed |= ImGui::DragFloat("Colorfulness", &m_colorfulness, 0.01f, 0.1f, 2.5f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much the Cineon curve colors deviate from neutral");
            changed |= ImGui::DragFloat("Strength##DPX", &m_strength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blend between original (0) and full DPX effect (1)");
            if (ImGui::Button("Reset##DPX")) { m_rgb_curve[0]=m_rgb_curve[1]=m_rgb_curve[2]=8.0f; m_rgb_c[0]=m_rgb_c[1]=0.36f; m_rgb_c[2]=0.34f; m_contrast=0.1f; m_saturation=3.0f; m_colorfulness=2.5f; m_strength=0.20f; changed=true; }
            if (changed) save_settings();
        }
    }

    void on_pre_render_vr_framework_dx11() override {
        if (!m_enabled) return;
        const auto rd = API::get()->param()->renderer; if (rd->renderer_type != UEVR_RENDERER_D3D11) return;
        auto scene_rt = API::StereoHook::get_scene_render_target(); if (!scene_rt) return;
        auto native = (ID3D11Texture2D*)scene_rt->get_native_resource(); if (!native) return;
        apply_effect_dx11(native);
    }

    void on_pre_render_vr_framework_dx12() override {
        if (!m_enabled) return;
        const auto rd = API::get()->param()->renderer; if (rd->renderer_type != UEVR_RENDERER_D3D12 || !rd->device) return;
        auto scene_rt = API::StereoHook::get_scene_render_target(); if (!scene_rt) return;
        auto native = (ID3D12Resource*)scene_rt->get_native_resource(); if (!native) return;
        auto desc = native->GetDesc(); if (desc.Width == 0 || desc.Height == 0) return;
        auto device = (ID3D12Device*)rd->device; auto rf = resolve_typeless_format(desc.Format);
        if (!m_dx12_ready || m_dx12_rt_format != rf) { if (m_dx12_ready) release_resources(); if (!init_dx12(device, desc.Format)) return; }
        auto& ts = find_dx12_target((UINT)desc.Width, desc.Height); if (!ensure_dx12_copy(device, native, ts)) return;
        auto cmd = (ID3D12GraphicsCommandList*)API::StereoHook::get_pre_render_command_list(); if (!cmd) return;
        if (m_dx12_cb_mapped) fill_cb(m_dx12_cb_mapped);
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barriers[0].Transition.pResource = native; barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barriers[1].Transition.pResource = ts.copy_tex.Get(); barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(2, barriers); cmd->CopyResource(ts.copy_tex.Get(), native);
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(2, barriers);
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{}; rtv_desc.Format = rf; rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(native, &rtv_desc, ts.rtv_heap->GetCPUDescriptorHandleForHeapStart());
        cmd->SetPipelineState(m_dx12_pso.Get()); cmd->SetGraphicsRootSignature(m_dx12_root_sig.Get());
        cmd->SetGraphicsRootConstantBufferView(0, m_dx12_cb->GetGPUVirtualAddress());
        ID3D12DescriptorHeap* heaps[] = { ts.srv_heap.Get() }; cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootDescriptorTable(1, ts.srv_heap->GetGPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = ts.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
        D3D12_VIEWPORT vp{}; vp.Width = (float)ts.width; vp.Height = (float)ts.height; vp.MaxDepth = 1.0f; cmd->RSSetViewports(1, &vp);
        D3D12_RECT scissor{}; scissor.right = (LONG)ts.width; scissor.bottom = (LONG)ts.height; cmd->RSSetScissorRects(1, &scissor);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); cmd->DrawInstanced(3, 1, 0, 0);
    }

    void on_present() override {}
    void on_pre_engine_tick(API::UGameEngine*, float) override {}
    void on_device_reset() override { release_resources(); }
    void on_post_render_vr_framework_dx11(ID3D11DeviceContext*, ID3D11Texture2D*, ID3D11RenderTargetView*) override {}
    void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_CPU_DESCRIPTOR_HANDLE*) override {}

private:
    void release_resources() {
        m_dx11_ctx.Reset(); m_vs.Reset(); m_ps.Reset(); m_cb.Reset(); m_sampler.Reset();
        for (auto& ts : m_dx11_targets) ts.reset(); m_shader_ready = false;
        m_dx12_pso.Reset(); m_dx12_root_sig.Reset(); m_dx12_cb.Reset();
        for (auto& ts : m_dx12_targets) ts.reset(); m_dx12_ready = false; m_dx12_cb_mapped = nullptr; m_dx12_rt_format = DXGI_FORMAT_UNKNOWN;
    }

    bool init_shaders_dx11(ID3D11Device* device) {
        ComPtr<ID3DBlob> vsb, vse, psb, pse;
        if (FAILED(D3DCompile(g_dpx_vs_src, strlen(g_dpx_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &vse))) return false;
        if (FAILED(D3DCompile(g_dpx_ps_src, strlen(g_dpx_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &pse))) { if (pse) API::get()->log_error("[DPX] PS: %s", (const char*)pse->GetBufferPointer()); return false; }
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs))) return false;
        if (FAILED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps))) return false;
        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(DPXParamsCB); cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cbd, nullptr, &m_cb))) return false;
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &m_sampler))) return false;
        m_shader_ready = true; return true;
    }

    bool ensure_dx11_copy(ID3D11Device* device, ID3D11Texture2D* src, DX11TargetState& ts) {
        D3D11_TEXTURE2D_DESC sd{}; src->GetDesc(&sd); if (ts.copy_tex && ts.width == sd.Width && ts.height == sd.Height) return true; ts.reset();
        auto rf = resolve_typeless_format(sd.Format); D3D11_TEXTURE2D_DESC cd = sd; cd.Format = rf; cd.MipLevels = 1; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.MiscFlags = 0; cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0;
        if (FAILED(device->CreateTexture2D(&cd, nullptr, &ts.copy_tex))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{}; svd.Format = rf; svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; svd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(ts.copy_tex.Get(), &svd, &ts.copy_srv))) { ts.reset(); return false; }
        ts.width = sd.Width; ts.height = sd.Height; return true;
    }

    void apply_effect_dx11(ID3D11Texture2D* target) {
        auto device = (ID3D11Device*)API::get()->param()->renderer->device; if (!device) return;
        D3D11_TEXTURE2D_DESC td{}; target->GetDesc(&td); if (td.Width == 0 || td.Height == 0) return;
        if (!m_dx11_ctx) device->GetImmediateContext(&m_dx11_ctx); auto ctx = m_dx11_ctx.Get();
        if (!m_shader_ready && !init_shaders_dx11(device)) return;
        auto& ts = find_dx11_target(td.Width, td.Height); if (!ensure_dx11_copy(device, target, ts)) return;
        if (ts.rtv_tex != target) { ts.rtv.Reset(); D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = resolve_typeless_format(td.Format); rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; if (FAILED(device->CreateRenderTargetView(target, &rd, &ts.rtv))) return; ts.rtv_tex = target; }
        D3D11_MAPPED_SUBRESOURCE mapped{}; if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) { fill_cb((DPXParamsCB*)mapped.pData); ctx->Unmap(m_cb.Get(), 0); }
        ctx->CopyResource(ts.copy_tex.Get(), target);
        D3D11_VIEWPORT vp{}; vp.Width = (float)ts.width; vp.Height = (float)ts.height; vp.MaxDepth = 1.0f; ctx->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtv_raw = ts.rtv.Get(); ctx->OMSetRenderTargets(1, &rtv_raw, nullptr);
        ctx->IASetInputLayout(nullptr); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(m_vs.Get(), nullptr, 0); ctx->PSSetShader(m_ps.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf()); ctx->PSSetShaderResources(0, 1, ts.copy_srv.GetAddressOf()); ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
        ctx->Draw(3, 0); ID3D11ShaderResourceView* null_srv = nullptr; ctx->PSSetShaderResources(0, 1, &null_srv);
    }

    bool init_dx12(ID3D12Device* device, DXGI_FORMAT rt_format) {
        if (m_dx12_ready) return true; rt_format = resolve_typeless_format(rt_format);
        D3D12_DESCRIPTOR_RANGE1 srv_range{}; srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv_range.NumDescriptors = 1; srv_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        D3D12_ROOT_PARAMETER1 rp[2]{}; rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor.ShaderRegister = 0; rp[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &srv_range; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC ss{}; ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER; ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{}; rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; rsd.Desc_1_1.NumParameters = 2; rsd.Desc_1_1.pParameters = rp; rsd.Desc_1_1.NumStaticSamplers = 1; rsd.Desc_1_1.pStaticSamplers = &ss;
        ComPtr<ID3DBlob> sb, se; if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &sb, &se))) return false;
        if (FAILED(device->CreateRootSignature(0, sb->GetBufferPointer(), sb->GetBufferSize(), IID_PPV_ARGS(&m_dx12_root_sig)))) return false;
        ComPtr<ID3DBlob> vsb, vse, psb, pse;
        if (FAILED(D3DCompile(g_dpx_vs_src, strlen(g_dpx_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &vse))) return false;
        if (FAILED(D3DCompile(g_dpx_ps_src, strlen(g_dpx_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &pse))) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = m_dx12_root_sig.Get(); pd.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() }; pd.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 1; pd.RTVFormats[0] = rt_format; pd.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_dx12_pso)))) return false;
        D3D12_HEAP_PROPERTIES uh{}; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbd{}; cbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; cbd.Width = 256; cbd.Height = 1; cbd.DepthOrArraySize = 1; cbd.MipLevels = 1; cbd.Format = DXGI_FORMAT_UNKNOWN; cbd.SampleDesc.Count = 1; cbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_dx12_cb)))) return false;
        m_dx12_cb->Map(0, nullptr, (void**)&m_dx12_cb_mapped); m_dx12_rt_format = rt_format; m_dx12_ready = true; return true;
    }

    bool ensure_dx12_copy(ID3D12Device* device, ID3D12Resource* src, DX12TargetState& ts) {
        auto sd = src->GetDesc(); if (ts.copy_tex && ts.width == (UINT)sd.Width && ts.height == sd.Height) return true; ts.reset();
        auto rf = resolve_typeless_format(sd.Format); D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC cd = sd; cd.Flags = D3D12_RESOURCE_FLAG_NONE; cd.Format = rf;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&ts.copy_tex)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC shd{}; shd.NumDescriptors = 1; shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&ts.srv_heap)))) { ts.reset(); return false; }
        D3D12_DESCRIPTOR_HEAP_DESC rhd{}; rhd.NumDescriptors = 1; rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&ts.rtv_heap)))) { ts.reset(); return false; }
        D3D12_SHADER_RESOURCE_VIEW_DESC svd{}; svd.Format = rf; svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; svd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(ts.copy_tex.Get(), &svd, ts.srv_heap->GetCPUDescriptorHandleForHeapStart());
        ts.width = (UINT)sd.Width; ts.height = sd.Height; return true;
    }
};

std::unique_ptr<DPXPlugin> g_plugin{new DPXPlugin()};
