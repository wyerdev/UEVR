/*
Technicolor Plugin for UEVR
============================
A UEVR C++ plugin that applies DKT70's Technicolor effect (optimized by CeeJay.dk).
Simulates a two-strip technicolor process using cyan/magenta/yellow filters.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12),
which fires BEFORE UEVR copies the render target to VR eye textures.

UEVR plugin wrapper: MIT license

Original shader:
Technicolor version 1.1
Original by DKT70
Optimized by CeeJay.dk
Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/Technicolor.fx
From the crosire/reshade-shaders community collection. No explicit license
was provided in the original file or repository.
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
// Embedded HLSL
// ============================================================================

static const char* g_vs_src = R"(
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

// Faithful port of DKT70/CeeJay.dk's Technicolor.fx
static const char* g_ps_src = R"(
cbuffer TechnicolorParams : register(b0) {
    float3 InvNegPower; // precomputed 1/(RGBNegativeAmount * Power)
    float Strength;     // 0..1
};

Texture2D SceneTexture : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput {
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    float3 tcol = SceneTexture.Sample(PointSampler, input.TexCoord).rgb;

    static const float3 cyanfilter    = float3(0.0, 1.30, 1.0);
    static const float3 magentafilter = float3(1.0, 0.0, 1.05);
    static const float3 yellowfilter  = float3(1.6, 1.6, 0.05);
    static const float2 redorangefilter = float2(1.05, 0.620);
    static const float2 greenfilter     = float2(0.30, 1.0);
    float2 magentafilter2 = magentafilter.rb;

    float2 negative_mul_r = tcol.rg * InvNegPower.r;
    float2 negative_mul_g = tcol.rg * InvNegPower.g;
    float2 negative_mul_b = tcol.rb * InvNegPower.b;

    float3 output_r = dot(redorangefilter, negative_mul_r).xxx + cyanfilter;
    float3 output_g = dot(greenfilter, negative_mul_g).xxx + magentafilter;
    float3 output_b = dot(magentafilter2, negative_mul_b).xxx + yellowfilter;

    float3 result = lerp(tcol, output_r * output_g * output_b, Strength);
    return float4(result, 1.0);
}
)";


// ============================================================================
// Constant buffer layout
// ============================================================================
struct TechnicolorCB {
    float InvNegPower[3]; // precomputed 1/(neg*power) per channel
    float Strength;       // 0..1
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
// Defaults
// ============================================================================
static constexpr float DEFAULT_POWER    = 4.0f;
static constexpr float DEFAULT_NEG_R    = 0.88f;
static constexpr float DEFAULT_NEG_G    = 0.88f;
static constexpr float DEFAULT_NEG_B    = 0.88f;
static constexpr float DEFAULT_STRENGTH = 0.4f;
static constexpr const char* PLUGIN_VERSION = "1.0.0";

// ============================================================================
// Plugin class
// ============================================================================
class TechnicolorPlugin : public uevr::Plugin {
public:
    float m_power    = DEFAULT_POWER;
    float m_neg_r    = DEFAULT_NEG_R;
    float m_neg_g    = DEFAULT_NEG_G;
    float m_neg_b    = DEFAULT_NEG_B;
    float m_strength = DEFAULT_STRENGTH;
    bool  m_enabled  = false;

    // DX11
    ComPtr<ID3D11DeviceContext>      m_dx11_ctx;
    ComPtr<ID3D11VertexShader>       m_vs;
    ComPtr<ID3D11PixelShader>        m_ps;
    ComPtr<ID3D11Buffer>             m_cb;
    ComPtr<ID3D11SamplerState>       m_sampler;
    bool m_shader_ready = false;
    bool m_dx11_logged_flags = false;  // diagnostic: log RT bind flags once
    bool m_dx11_first_apply_logged = false;  // diagnostic: log first successful effect apply

    // DX11 per-target state — cached by texture dimensions (2-slot cache like DX12)
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
                    API::get()->log_info("[Technicolor] DX11 cache hit: slot %d (%ux%u)", (int)i, w, h);
                }
                return ts;
            }
        }
        for (auto& ts : m_dx11_targets)
            if (ts.width == 0) return ts;
        m_dx11_targets[1].reset();
        return m_dx11_targets[1];
    }

    // DX12
    ComPtr<ID3D12RootSignature> m_dx12_root_sig;
    ComPtr<ID3D12PipelineState> m_dx12_pso;
    ComPtr<ID3D12Resource>      m_dx12_cb;

    struct DX12TargetState {
        ComPtr<ID3D12Resource>       copy_tex;
        ComPtr<ID3D12DescriptorHeap> srv_heap;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;  // RTV written per-frame on native target
        UINT width = 0, height = 0;
        void reset() { copy_tex.Reset(); srv_heap.Reset(); rtv_heap.Reset(); width = 0; height = 0; }
    };
    static constexpr size_t MAX_TARGET_STATES = 2;
    DX12TargetState m_dx12_targets[MAX_TARGET_STATES];

    DX12TargetState& find_target_state(UINT w, UINT h) {
        for (auto& ts : m_dx12_targets) if (ts.width == w && ts.height == h) return ts;
        for (auto& ts : m_dx12_targets) if (ts.width == 0) return ts;
        m_dx12_targets[1].reset();
        return m_dx12_targets[1];
    }

    bool m_dx12_ready = false;
    TechnicolorCB* m_dx12_cb_mapped = nullptr;
    DXGI_FORMAT m_dx12_rt_format = DXGI_FORMAT_UNKNOWN;

    void on_dllmain() override {}

    void on_initialize() override {
        API::get()->log_info("[Technicolor] Plugin initialized");
        load_settings();
    }

    // ========================================================================
    // Settings
    // ========================================================================
    std::filesystem::path get_settings_path() {
        return API::get()->get_persistent_dir() / L"data" / L"plugins" / L"technicolor_settings.txt";
    }

    void save_settings() {
        try {
            std::filesystem::create_directories(get_settings_path().parent_path());
            std::ofstream f(get_settings_path());
            if (f.is_open()) {
                f << m_enabled << "\n" << m_power << "\n"
                  << m_neg_r << "\n" << m_neg_g << "\n" << m_neg_b << "\n"
                  << m_strength << "\n";
            }
        } catch (...) {}
    }

    void load_settings() {
        try {
            std::ifstream f(get_settings_path());
            if (f.is_open()) {
                int enabled_int;
                if (f >> enabled_int >> m_power >> m_neg_r >> m_neg_g >> m_neg_b >> m_strength) {
                    m_enabled = (enabled_int != 0);
                    if (m_power < 0.0f) m_power = 0.0f;
                    if (m_power > 8.0f) m_power = 8.0f;
                    m_neg_r = std::clamp(m_neg_r, 0.0f, 1.0f);
                    m_neg_g = std::clamp(m_neg_g, 0.0f, 1.0f);
                    m_neg_b = std::clamp(m_neg_b, 0.0f, 1.0f);
                    m_strength = std::clamp(m_strength, 0.0f, 1.0f);
                }
            }
        } catch (...) {}
    }

    // ========================================================================
    // ImGui UI
    // ========================================================================
    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Technicolor Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", PLUGIN_VERSION);
            ImGui::TextWrapped("Emulates 2-strip Technicolor (old Hollywood look). Teal shadows, warm highlights. Use sparingly.");
            bool changed = false;

            changed |= ImGui::Checkbox("Enabled##tech", &m_enabled);
            changed |= ImGui::SliderFloat("Power", &m_power, 0.0f, 8.0f, "%.1f");
            changed |= ImGui::SliderFloat("Negative R", &m_neg_r, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Negative G", &m_neg_g, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Negative B", &m_neg_b, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Strength", &m_strength, 0.0f, 1.0f, "%.2f");

            if (ImGui::Button("Reset to Defaults##tech")) {
                m_power = DEFAULT_POWER; m_neg_r = DEFAULT_NEG_R; m_neg_g = DEFAULT_NEG_G;
                m_neg_b = DEFAULT_NEG_B; m_strength = DEFAULT_STRENGTH;
                changed = true;
            }

            if (changed) save_settings();
        }
    }

    // ========================================================================
    // DX12 pre-render
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
            if (m_dx12_ready) release_resources();
            if (!init_dx12_pipeline(device, desc.Format)) return;
        }
        auto& ts = find_target_state((UINT)desc.Width, desc.Height);
        if (!ensure_dx12_textures(device, native, ts)) return;

        auto cmd = (ID3D12GraphicsCommandList*)API::StereoHook::get_pre_render_command_list();
        if (!cmd) return;

        if (m_dx12_cb_mapped) {
            auto safe_inv = [](float neg, float pwr) { float d = neg * pwr; return d > 1e-4f ? 1.0f / d : 0.0f; };
            m_dx12_cb_mapped->InvNegPower[0] = safe_inv(m_neg_r, m_power);
            m_dx12_cb_mapped->InvNegPower[1] = safe_inv(m_neg_g, m_power);
            m_dx12_cb_mapped->InvNegPower[2] = safe_inv(m_neg_b, m_power);
            m_dx12_cb_mapped->Strength = m_strength;
        }

        // Step 1: Copy target -> copy_tex
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

        // Step 2: Transition back — then render directly to native (no intermediate result_tex)
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(2, barriers);

        // Create RTV on native target (CPU descriptor write — essentially free)
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = resolved_format;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(native, &rtv_desc, ts.rtv_heap->GetCPUDescriptorHandleForHeapStart());

        // Draw: read copy_tex (SRV) -> write native (RTV) — no hazard, different resources
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
        // native left in RENDER_TARGET state — exactly what UEVR expects
    }

    // ========================================================================
    // DX11 pre-render
    // ========================================================================
    void on_pre_render_vr_framework_dx11() override {
        if (!m_enabled) return;

        const auto renderer_data = API::get()->param()->renderer;
        if (renderer_data->renderer_type != UEVR_RENDERER_D3D11) return;

        auto scene_rt = API::StereoHook::get_scene_render_target();
        if (!scene_rt) return;
        auto native = (ID3D11Texture2D*)scene_rt->get_native_resource();
        if (!native) return;

        apply_dx11(native);
    }

    void on_present() override {}
    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {}

    void on_device_reset() override {
        API::get()->log_info("[Technicolor] Device reset");
        release_resources();
    }

    void on_post_render_vr_framework_dx11(
        ID3D11DeviceContext* context, ID3D11Texture2D* texture, ID3D11RenderTargetView* rtv) override {}
    void on_post_render_vr_framework_dx12(
        ID3D12GraphicsCommandList* command_list, ID3D12Resource* rt, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {}

private:

    void release_resources() {
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
    // DX11 implementation
    // ========================================================================
    bool init_shaders_dx11(ID3D11Device* device) {
        ComPtr<ID3DBlob> vs_blob, vs_err;
        if (FAILED(D3DCompile(g_vs_src, strlen(g_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs_blob, &vs_err))) return false;
        ComPtr<ID3DBlob> ps_blob, ps_err;
        if (FAILED(D3DCompile(g_ps_src, strlen(g_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps_blob, &ps_err))) return false;

        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs))) return false;
        if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps))) return false;

        D3D11_BUFFER_DESC cb_desc{}; cb_desc.ByteWidth = sizeof(TechnicolorCB);
        cb_desc.Usage = D3D11_USAGE_DYNAMIC; cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &m_cb))) return false;

        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &m_sampler))) return false;

        m_shader_ready = true;
        API::get()->log_info("[Technicolor] DX11 shaders ready");
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
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.MiscFlags = 0; cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0;
        if (FAILED(device->CreateTexture2D(&cd, nullptr, &ts.copy_tex))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC svd{}; svd.Format = resolved_fmt;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; svd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(ts.copy_tex.Get(), &svd, &ts.copy_srv))) { ts.reset(); return false; }

        ts.width = src_desc.Width; ts.height = src_desc.Height;
        API::get()->log_info("[Technicolor] DX11 copy texture created: %ux%u", ts.width, ts.height);
        return true;
    }

    void apply_dx11(ID3D11Texture2D* target) {
        const auto rd = API::get()->param()->renderer;
        auto device = (ID3D11Device*)rd->device;
        if (!device || !target) return;

        D3D11_TEXTURE2D_DESC target_desc{}; target->GetDesc(&target_desc);
        if (target_desc.Width == 0 || target_desc.Height == 0) return;

        if (!m_dx11_ctx) device->GetImmediateContext(&m_dx11_ctx);
        auto ctx = m_dx11_ctx.Get();

        if (!m_shader_ready && !init_shaders_dx11(device)) return;

        // One-time diagnostic: log RT bind flags
        if (!m_dx11_logged_flags) {
            m_dx11_logged_flags = true;
            API::get()->log_info("[Technicolor] DX11 RT: %ux%u fmt=%u bind=0x%x",
                target_desc.Width, target_desc.Height, (unsigned)target_desc.Format,
                (unsigned)target_desc.BindFlags);
        }

        auto& ts = find_dx11_target_state(target_desc.Width, target_desc.Height);
        if (!ensure_dx11_copy_texture(device, target, ts)) return;

        if (ts.rtv_tex != target) {
            ts.rtv.Reset();
            const auto resolved_fmt = resolve_typeless_format(target_desc.Format);
            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format = resolved_fmt;
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(device->CreateRenderTargetView(target, &rtv_desc, &ts.rtv))) return;
            ts.rtv_tex = target;
        }

        // Update constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* p = static_cast<TechnicolorCB*>(mapped.pData);
            auto safe_inv = [](float neg, float pwr) { float d = neg * pwr; return d > 1e-4f ? 1.0f / d : 0.0f; };
            p->InvNegPower[0] = safe_inv(m_neg_r, m_power);
            p->InvNegPower[1] = safe_inv(m_neg_g, m_power);
            p->InvNegPower[2] = safe_inv(m_neg_b, m_power);
            p->Strength = m_strength;
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
            API::get()->log_info("[Technicolor] DX11 effect applied: %ux%u", ts.width, ts.height);
        }

        ID3D11ShaderResourceView* null_srv = nullptr;
        ctx->PSSetShaderResources(0, 1, &null_srv);
    }

    // ========================================================================
    // DX12 implementation
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
        if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &sig_blob, &sig_err))) return false;
        if (FAILED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&m_dx12_root_sig)))) return false;

        ComPtr<ID3DBlob> vsb, vse, psb, pse;
        if (FAILED(D3DCompile(g_vs_src, strlen(g_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &vse))) return false;
        if (FAILED(D3DCompile(g_ps_src, strlen(g_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &pse))) return false;

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

        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_dx12_pso)))) return false;

        D3D12_HEAP_PROPERTIES uh{}; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbd{}; cbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbd.Width = 256; cbd.Height = 1; cbd.DepthOrArraySize = 1; cbd.MipLevels = 1;
        cbd.Format = DXGI_FORMAT_UNKNOWN; cbd.SampleDesc.Count = 1; cbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &cbd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_dx12_cb)))) return false;
        m_dx12_cb->Map(0, nullptr, (void**)&m_dx12_cb_mapped);

        m_dx12_rt_format = rt_format;
        m_dx12_ready = true;
        API::get()->log_info("[Technicolor DX12] Pipeline ready");
        return true;
    }

    bool ensure_dx12_textures(ID3D12Device* device, ID3D12Resource* src, DX12TargetState& ts) {
        auto sd = src->GetDesc();
        if (ts.copy_tex && ts.width == (UINT)sd.Width && ts.height == sd.Height) return true;
        ts.reset();

        const auto resolved_fmt = resolve_typeless_format(sd.Format);
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Only need copy_tex — we render directly to the native target (no intermediate result_tex)
        D3D12_RESOURCE_DESC copy_desc = sd; copy_desc.Flags = D3D12_RESOURCE_FLAG_NONE; copy_desc.Format = resolved_fmt;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &copy_desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&ts.copy_tex)))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC shd{}; shd.NumDescriptors = 1;
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&ts.srv_heap)))) { ts.reset(); return false; }

        D3D12_DESCRIPTOR_HEAP_DESC rhd{}; rhd.NumDescriptors = 1; rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&ts.rtv_heap)))) { ts.reset(); return false; }

        D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = resolved_fmt; svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; svd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(ts.copy_tex.Get(), &svd, ts.srv_heap->GetCPUDescriptorHandleForHeapStart());
        // RTV descriptor written per-frame on native target in render function

        ts.width = (UINT)sd.Width; ts.height = sd.Height;
        return true;
    }
};

// ============================================================================
// Entry point
// ============================================================================
std::unique_ptr<TechnicolorPlugin> g_plugin{new TechnicolorPlugin()};
