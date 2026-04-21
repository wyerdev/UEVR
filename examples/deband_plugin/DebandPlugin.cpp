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

static const char* g_deband_vs_src = R"(
struct VSOutput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };
VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput o; o.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    o.Position = float4(o.TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); return o;
}
)";

/*
   Port of Deband shader by haasn / JPulowski.
   Detects color banding via standard deviation + Weber ratio analysis,
   replaces banded pixels with averaged neighbors, applies ordered dithering.
   Depth-based filtering removed (not available in UEVR pre-render callback).
*/
static const char* g_deband_ps_src = R"(
cbuffer DebandParams : register(b0) {
    float SDThreshold;    // Standard deviation threshold for banding detection
    float WeberThreshold; // Weber ratio threshold for banding detection
    float Radius;         // Search radius in pixels
    int   Iterations;     // Number of debanding steps (1-4)
    float RandomSeed;     // Per-frame random seed (0..1)
    int   EnableWeber;    // 1=enabled
    int   EnableStdDev;   // 1=enabled
    int   DebugOutput;    // 0=none, 1=LPF, 2=banding map
    float2 PixelSize;     // 1/width, 1/height
    float2 ScreenSize;    // width, height
};

Texture2D SceneTexture : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

float rand(float x) { return frac(x / 41.0); }
float permute(float x) { return ((34.0 * x + 1.0) * x) % 289.0; }

float4 main(PSInput input) : SV_Target {
    float2 uv = input.TexCoord;
    float3 ori = SceneTexture.Sample(PointSampler, uv).rgb;

    // Initialize PRNG by hashing position + random seed
    float3 m = float3(uv + 1.0, RandomSeed + 1.0);
    float h = permute(permute(permute(m.x) + m.y) + m.z);

    // Compute random angle
    float dir = rand(permute(h)) * 6.2831853;
    float2 o;
    sincos(dir, o.y, o.x);

    // Distance calculations
    float2 pt;
    float dist;
    for (int i = 1; i <= 4; i++) {
        if (i > Iterations) break;
        dist = rand(h) * Radius * (float)i;
        pt = dist * PixelSize;
        h = permute(h);
    }

    // Sample at quarter-turn intervals around the source pixel
    float3 ref0 = SceneTexture.Sample(PointSampler, uv + pt * o).rgb;
    float3 ref1 = SceneTexture.Sample(PointSampler, uv - pt * o).rgb;
    float3 ref2 = SceneTexture.Sample(PointSampler, uv + pt * float2(-o.y, o.x)).rgb;
    float3 ref3 = SceneTexture.Sample(PointSampler, uv + pt * float2(o.y, -o.x)).rgb;

    // Calculate Weber ratio: local contrast relative to mean
    float3 mean = (ori + ref0 + ref1 + ref2 + ref3) * 0.2;
    float3 k = abs(ori - mean);
    k += abs(ref0 - mean);
    k += abs(ref1 - mean);
    k += abs(ref2 - mean);
    k += abs(ref3 - mean);
    k = k * 0.2 / mean;

    // Calculate standard deviation
    float3 sd = pow(ref0 - ori, 2) + pow(ref1 - ori, 2) + pow(ref2 - ori, 2) + pow(ref3 - ori, 2);
    sd = sqrt(sd * 0.25);

    // Generate smoothed output
    float3 output;
    if (DebugOutput == 2)
        output = float3(0.0, 1.0, 0.0);
    else
        output = (ref0 + ref1 + ref2 + ref3) * 0.25;

    // Build binary banding map
    bool3 banding_map = (bool3)true;
    if (DebugOutput != 1) {
        if (EnableWeber)
            banding_map = banding_map && (k <= WeberThreshold * (float)Iterations);
        if (EnableStdDev)
            banding_map = banding_map && (sd <= SDThreshold * (float)Iterations);
    }

    // Ordered dithering (CeeJay.dk)
    float grid_position = frac(dot(uv, ScreenSize * float2(1.0 / 16.0, 10.0 / 36.0) + 0.25));
    float dither_shift = 0.25 * (1.0 / 255.0); // 8-bit color depth
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

static constexpr const char* DEBAND_VERSION = "1.0.0";

class DebandPlugin : public uevr::Plugin {
public:
    float m_sd_threshold = 0.007f;
    float m_weber_threshold = 0.04f;
    float m_radius = 24.0f;
    int   m_iterations = 1;
    bool  m_enable_weber = true;
    bool  m_enable_stddev = true;
    int   m_debug_output = 0;
    bool  m_enabled = false;
    UINT  m_frame_count = 0;

    // DX11
    ComPtr<ID3D11DeviceContext> m_dx11_ctx;
    ComPtr<ID3D11VertexShader>  m_vs; ComPtr<ID3D11PixelShader> m_ps;
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

    // DX12
    ComPtr<ID3D12RootSignature> m_dx12_root_sig; ComPtr<ID3D12PipelineState> m_dx12_pso; ComPtr<ID3D12Resource> m_dx12_cb;
    DebandParamsCB* m_dx12_cb_mapped = nullptr; DXGI_FORMAT m_dx12_rt_format = DXGI_FORMAT_UNKNOWN; bool m_dx12_ready = false;
    struct DX12TargetState {
        ComPtr<ID3D12Resource> copy_tex; ComPtr<ID3D12DescriptorHeap> srv_heap; ComPtr<ID3D12DescriptorHeap> rtv_heap;
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
    void on_initialize() override { API::get()->log_info("[Deband] Plugin initialized"); load_settings(); }

    std::filesystem::path get_settings_path() {
        return API::get()->get_persistent_dir() / L"data" / L"plugins" / L"shader_settings" / L"deband_settings.txt";
    }

    void save_settings() {
        try { std::filesystem::create_directories(get_settings_path().parent_path()); std::ofstream f(get_settings_path()); if (!f.is_open()) return;
            f << m_enabled << "\n" << m_sd_threshold << "\n" << m_weber_threshold << "\n"
              << m_radius << "\n" << m_iterations << "\n" << m_enable_weber << "\n"
              << m_enable_stddev << "\n" << m_debug_output << "\n";
        } catch (...) {}
    }

    void load_settings() {
        try { std::ifstream f(get_settings_path()); if (!f.is_open()) return;
            int e; if (f >> e) m_enabled = (e != 0);
            f >> m_sd_threshold >> m_weber_threshold >> m_radius >> m_iterations;
            int ew, es; f >> ew >> es; m_enable_weber = (ew != 0); m_enable_stddev = (es != 0);
            f >> m_debug_output;
            m_sd_threshold = max(0.0f, min(0.5f, m_sd_threshold));
            m_weber_threshold = max(0.0f, min(2.0f, m_weber_threshold));
            m_radius = max(1.0f, min(32.0f, m_radius));
            m_iterations = max(1, min(4, m_iterations));
            m_debug_output = max(0, min(2, m_debug_output));
        } catch (...) {}
    }

    void fill_cb(DebandParamsCB* p, UINT w, UINT h) {
        p->SDThreshold = m_sd_threshold;
        p->WeberThreshold = m_weber_threshold;
        p->Radius = m_radius;
        p->Iterations = m_iterations;
        p->RandomSeed = (float)(m_frame_count % 32767) / 32767.0f;
        p->EnableWeber = m_enable_weber ? 1 : 0;
        p->EnableStdDev = m_enable_stddev ? 1 : 0;
        p->DebugOutput = m_debug_output;
        p->PixelSize[0] = 1.0f / (float)w; p->PixelSize[1] = 1.0f / (float)h;
        p->ScreenSize[0] = (float)w; p->ScreenSize[1] = (float)h;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Deband Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", DEBAND_VERSION);
            ImGui::TextWrapped("Removes color banding artifacts in gradients (common on VR panels). Detects flat regions via standard deviation + Weber ratio, smooths them, and applies ordered dithering.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##Deband", &m_enabled);
            ImGui::Separator();
            ImGui::Text("Banding Analysis");
            changed |= ImGui::Checkbox("Weber Ratio##Deband", &m_enable_weber);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Calculates local pixel intensity relative to average background intensity");
            changed |= ImGui::DragFloat("Weber Threshold##Deband", &m_weber_threshold, 0.001f, 0.0f, 2.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower = more aggressive banding detection");
            changed |= ImGui::Checkbox("Standard Deviation##Deband", &m_enable_stddev);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Calculates nearby pixels' intensity deviation from the current pixel");
            changed |= ImGui::DragFloat("StdDev Threshold##Deband", &m_sd_threshold, 0.0001f, 0.0f, 0.5f, "%.4f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower = more aggressive banding detection");
            ImGui::Separator();
            ImGui::Text("Detection & Removal");
            changed |= ImGui::DragFloat("Radius##Deband", &m_radius, 1.0f, 1.0f, 32.0f, "%.0f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search radius. Higher finds more gradients but lower smooths more aggressively");
            changed |= ImGui::DragInt("Iterations##Deband", &m_iterations, 1, 1, 4);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Debanding steps per sample. More = less banding but slower");
            ImGui::Separator();
            const char* debug_items[] = { "None", "Blurred (LPF)", "Banding Map" };
            changed |= ImGui::Combo("Debug View##Deband", &m_debug_output, debug_items, 3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("None = normal output. Blurred = shows the smoothing. Banding Map = highlights detected banding regions");
            if (ImGui::Button("Reset##Deband")) { m_sd_threshold = 0.007f; m_weber_threshold = 0.04f; m_radius = 24.0f; m_iterations = 1; m_enable_weber = true; m_enable_stddev = true; m_debug_output = 0; changed = true; }
            if (changed) save_settings();
        }
    }

    void on_pre_render_vr_framework_dx11() override {
        if (!m_enabled) return;
        m_frame_count++;
        const auto rd = API::get()->param()->renderer; if (rd->renderer_type != UEVR_RENDERER_D3D11) return;
        auto scene_rt = API::StereoHook::get_scene_render_target(); if (!scene_rt) return;
        auto native = (ID3D11Texture2D*)scene_rt->get_native_resource(); if (!native) return;
        apply_effect_dx11(native);
    }

    void on_pre_render_vr_framework_dx12() override {
        if (!m_enabled) return;
        m_frame_count++;
        const auto rd = API::get()->param()->renderer; if (rd->renderer_type != UEVR_RENDERER_D3D12 || !rd->device) return;
        auto scene_rt = API::StereoHook::get_scene_render_target(); if (!scene_rt) return;
        auto native = (ID3D12Resource*)scene_rt->get_native_resource(); if (!native) return;
        auto desc = native->GetDesc(); if (desc.Width == 0 || desc.Height == 0) return;
        auto device = (ID3D12Device*)rd->device; auto rf = resolve_typeless_format(desc.Format);
        if (!m_dx12_ready || m_dx12_rt_format != rf) { if (m_dx12_ready) release_resources(); if (!init_dx12(device, desc.Format)) return; }
        auto& ts = find_dx12_target((UINT)desc.Width, desc.Height);
        if (!ensure_dx12_copy(device, native, ts)) return;
        auto cmd = (ID3D12GraphicsCommandList*)API::StereoHook::get_pre_render_command_list(); if (!cmd) return;
        if (m_dx12_cb_mapped) fill_cb(m_dx12_cb_mapped, ts.width, ts.height);
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
        if (m_dx12_cb && m_dx12_cb_mapped) { m_dx12_cb->Unmap(0, nullptr); }
        m_dx12_cb_mapped = nullptr;
        m_dx12_pso.Reset(); m_dx12_root_sig.Reset(); m_dx12_cb.Reset();
        for (auto& ts : m_dx12_targets) ts.reset(); m_dx12_ready = false; m_dx12_rt_format = DXGI_FORMAT_UNKNOWN;
    }

    bool init_shaders_dx11(ID3D11Device* device) {
        ComPtr<ID3DBlob> vsb, vse, psb, pse;
        if (FAILED(D3DCompile(g_deband_vs_src, strlen(g_deband_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &vse))) return false;
        if (FAILED(D3DCompile(g_deband_ps_src, strlen(g_deband_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &pse))) { if (pse) API::get()->log_error("[Deband] PS: %s", (const char*)pse->GetBufferPointer()); return false; }
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs))) return false;
        if (FAILED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps))) return false;
        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(DebandParamsCB); cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
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
        D3D11_MAPPED_SUBRESOURCE mapped{}; if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) { fill_cb((DebandParamsCB*)mapped.pData, ts.width, ts.height); ctx->Unmap(m_cb.Get(), 0); }
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
        if (FAILED(D3DCompile(g_deband_vs_src, strlen(g_deband_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &vse))) return false;
        if (FAILED(D3DCompile(g_deband_ps_src, strlen(g_deband_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &pse))) return false;
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

std::unique_ptr<DebandPlugin> g_plugin{new DebandPlugin()};
