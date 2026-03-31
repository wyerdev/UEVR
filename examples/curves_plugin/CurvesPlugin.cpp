/*
Curves Plugin for UEVR
=======================
A UEVR C++ plugin that applies CeeJay.dk's Curves contrast effect to VR frames.
Uses S-curves to increase contrast without clipping highlights and shadows.

Applied to the UE4 scene render target in on_pre_render_vr_framework (DX11/DX12),
which fires BEFORE UEVR copies the render target to VR eye textures.

Includes an ImGui settings panel with enable/disable, mode/formula selectors,
contrast slider, and reset button.

UEVR plugin wrapper: MIT license

Original shader:
Curves by Christian Cann Schuldt Jensen ~ CeeJay.dk
Source: https://github.com/byxor/thug-pro-reshade/blob/master/THUG%20Pro/reshade-shaders/Shaders/Curves.fx
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
// Embedded HLSL source (compiled at runtime via D3DCompile)
// ============================================================================

static const char* g_curves_vs_src = R"(
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

// Faithful port of CeeJay.dk's Curves.fx from ReShade
static const char* g_curves_ps_src = R"(
cbuffer CurvesParams : register(b0) {
    int Mode;       // 0=Luma, 1=Chroma, 2=Both
    int Formula;    // 0-10 curve types
    float Contrast; // -1.0 to 1.0
    float _Pad0;
};

Texture2D SceneTexture : register(t0);
SamplerState PointSampler : register(s0);

struct PSInput {
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    float4 colorInput = SceneTexture.Sample(PointSampler, input.TexCoord);
    float3 lumCoeff = float3(0.2126, 0.7152, 0.0722);
    float Contrast_blend = Contrast;
    static const float PI = 3.1415927;

    // Calculate Luma and Chroma
    float luma = dot(lumCoeff, colorInput.rgb);
    float3 chroma = colorInput.rgb - luma;

    // Which value to put through the contrast formula
    float3 x;
    if (Mode == 0)
        x = luma;
    else if (Mode == 1) {
        x = chroma;
        x = x * 0.5 + 0.5;
    } else
        x = colorInput.rgb;

    // Curve formulas — use [branch] + if/else to only execute selected formula
    [branch] if (Formula == 0) { // Sine
        x = sin(PI * 0.5 * x);
        x *= x;
    }
    else if (Formula == 1) { // Abs split
        x = x - 0.5;
        x = (x / (0.5 + abs(x))) + 0.5;
    }
    else if (Formula == 2) { // Smoothstep
        x = x * x * (3.0 - 2.0 * x);
    }
    else if (Formula == 3) { // Exp formula
        x = (1.0524 * exp(6.0 * x) - 1.05248) / (exp(6.0 * x) + 20.0855);
    }
    else if (Formula == 4) { // Simplified Catmull-Rom (0,0,1,1)
        x = x * (x * (1.5 - x) + 0.5);
        Contrast_blend = Contrast * 2.0;
    }
    else if (Formula == 5) { // Perlin's Smootherstep
        x = x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
    }
    else if (Formula == 6) { // Abs add
        x = x - 0.5;
        x = x / ((abs(x) * 1.25) + 0.375) + 0.5;
    }
    else if (Formula == 7) { // Technicolor Cinestyle
        x = (x * (x * (x * (x * (x * (x * (1.6 * x - 7.2) + 10.8) - 4.2) - 3.6) + 2.7) - 1.8) + 2.7) * x * x;
    }
    else if (Formula == 8) { // Parabola
        x = -0.5 * (x * 2.0 - 1.0) * (abs(x * 2.0 - 1.0) - 2.0) + 0.5;
    }
    else if (Formula == 9) { // Half-circles
        float3 xstep = step(x, 0.5);
        float3 xstep_shift = (xstep - 0.5);
        float3 shifted_x = x + xstep_shift;
        x = abs(xstep - sqrt(-shifted_x * shifted_x + shifted_x)) - xstep_shift;
        Contrast_blend = Contrast * 0.5;
    }
    else if (Formula == 10) { // Polynomial split
        float3 a = x * x * 2.0;
        float3 b = (2.0 * -x + 4.0) * x - 1.0;
        x = (x < 0.5) ? a : b;
    }

    // Joining of Luma and Chroma
    if (Mode == 0) {
        x = lerp(luma, x, Contrast_blend);
        colorInput.rgb = x + chroma;
    } else if (Mode == 1) {
        x = x * 2.0 - 1.0;
        float3 color = luma + x;
        colorInput.rgb = lerp(colorInput.rgb, color, Contrast_blend);
    } else {
        float3 color = x;
        colorInput.rgb = lerp(colorInput.rgb, color, Contrast_blend);
    }

    return colorInput;
}
)";


// ============================================================================
// Constant buffer layout (must match HLSL cbuffer CurvesParams)
// ============================================================================
struct CurvesParamsCB {
    int Mode;
    int Formula;
    float Contrast;
    float _pad0;
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
static constexpr int   DEFAULT_MODE     = 0;     // Luma
static constexpr int   DEFAULT_FORMULA  = 4;     // Simplified Catmull-Rom
static constexpr float DEFAULT_CONTRAST = 0.65f;
static constexpr const char* CURVES_VERSION = "1.0.0";

static const char* g_mode_names[] = { "Luma", "Chroma", "Both Luma and Chroma" };
static const char* g_formula_names[] = {
    "Sine", "Abs split", "Smoothstep", "Exp formula",
    "Simplified Catmull-Rom", "Perlin's Smootherstep", "Abs add",
    "Technicolor Cinestyle", "Parabola", "Half-circles", "Polynomial split"
};

// ============================================================================
// Curves Plugin
// ============================================================================
class CurvesPlugin : public uevr::Plugin {
public:
    // Tunable parameters
    int   m_mode     = DEFAULT_MODE;
    int   m_formula  = DEFAULT_FORMULA;
    float m_contrast = DEFAULT_CONTRAST;
    bool  m_enabled  = false;

    // D3D11 effect resources
    ComPtr<ID3D11DeviceContext>        m_dx11_ctx;
    ComPtr<ID3D11VertexShader>         m_vs;
    ComPtr<ID3D11PixelShader>          m_ps;
    ComPtr<ID3D11Buffer>               m_cb;
    ComPtr<ID3D11SamplerState>         m_sampler;
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
                    API::get()->log_info("[Curves] DX11 cache hit: slot %d (%ux%u)", (int)i, w, h);
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
        ComPtr<ID3D12DescriptorHeap> rtv_heap;  // RTV written per-frame on native target
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
    CurvesParamsCB* m_dx12_cb_mapped = nullptr;
    DXGI_FORMAT m_dx12_rt_format = DXGI_FORMAT_UNKNOWN;

    void on_dllmain() override {}

    void on_initialize() override {
        API::get()->log_info("[Curves] Plugin initialized");
        load_settings();
    }

    // ========================================================================
    // Settings persistence
    // ========================================================================
    std::filesystem::path get_settings_path() {
        return API::get()->get_persistent_dir(L"curves_settings.txt");
    }

    void save_settings() {
        try {
            std::ofstream f(get_settings_path());
            if (f.is_open()) {
                f << m_enabled << "\n" << m_mode << "\n" << m_formula << "\n" << m_contrast << "\n";
            }
        } catch (...) {}
    }

    void load_settings() {
        try {
            std::ifstream f(get_settings_path());
            if (f.is_open()) {
                int enabled_int;
                if (f >> enabled_int >> m_mode >> m_formula >> m_contrast) {
                    m_enabled = (enabled_int != 0);
                    // Clamp loaded values
                    if (m_mode < 0 || m_mode > 2) m_mode = DEFAULT_MODE;
                    if (m_formula < 0 || m_formula > 10) m_formula = DEFAULT_FORMULA;
                    if (m_contrast < -1.0f) m_contrast = -1.0f;
                    if (m_contrast > 1.0f) m_contrast = 1.0f;
                    API::get()->log_info("[Curves] Loaded settings: enabled=%d mode=%d formula=%d contrast=%.2f",
                        m_enabled, m_mode, m_formula, m_contrast);
                }
            }
        } catch (...) {}
    }

    // ========================================================================
    // on_draw_ui: draw settings inside the UEVR menu (Plugins page)
    // ========================================================================
    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Curves Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", CURVES_VERSION);
            bool changed = false;

            changed |= ImGui::Checkbox("Enabled", &m_enabled);

            if (ImGui::BeginCombo("Mode", g_mode_names[m_mode])) {
                for (int i = 0; i < 3; i++) {
                    bool selected = (m_mode == i);
                    if (ImGui::Selectable(g_mode_names[i], selected)) { m_mode = i; changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (ImGui::BeginCombo("Formula", g_formula_names[m_formula])) {
                for (int i = 0; i < 11; i++) {
                    bool selected = (m_formula == i);
                    if (ImGui::Selectable(g_formula_names[i], selected)) { m_formula = i; changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            changed |= ImGui::SliderFloat("Contrast", &m_contrast, -1.0f, 1.0f, "%.2f");

            if (ImGui::Button("Reset to Defaults")) {
                m_mode     = DEFAULT_MODE;
                m_formula  = DEFAULT_FORMULA;
                m_contrast = DEFAULT_CONTRAST;
                changed = true;
            }

            if (changed) {
                save_settings();
            }
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

        // Update CB
        if (m_dx12_cb_mapped) {
            m_dx12_cb_mapped->Mode = m_mode;
            m_dx12_cb_mapped->Formula = m_formula;
            m_dx12_cb_mapped->Contrast = m_contrast;
            m_dx12_cb_mapped->_pad0 = 0;
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

        // Transition back
        barriers[0].Transition.pResource = native;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.pResource = ts.copy_tex.Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(2, barriers);

        // Step 2: Create RTV on native target (CPU descriptor write — essentially free)
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = resolved_format;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(native, &rtv_desc, ts.rtv_heap->GetCPUDescriptorHandleForHeapStart());

        // Draw Curves: read copy_tex (SRV) -> write native (RTV) — no hazard, different resources
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

        apply_curves_to_resource_dx11(native);
    }

    void on_present() override {}
    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {}

    void on_device_reset() override {
        API::get()->log_info("[Curves] Device reset");
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
    // DX11: Apply Curves to UE4 scene render target
    // ========================================================================
    bool init_shaders_dx11(ID3D11Device* device) {
        if (!device) return false;

        ComPtr<ID3DBlob> vs_blob, vs_err;
        if (FAILED(D3DCompile(g_curves_vs_src, strlen(g_curves_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs_blob, &vs_err))) return false;
        ComPtr<ID3DBlob> ps_blob, ps_err;
        if (FAILED(D3DCompile(g_curves_ps_src, strlen(g_curves_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps_blob, &ps_err))) return false;

        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs))) return false;
        if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps))) return false;

        D3D11_BUFFER_DESC cb_desc{}; cb_desc.ByteWidth = sizeof(CurvesParamsCB);
        cb_desc.Usage = D3D11_USAGE_DYNAMIC; cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &m_cb))) return false;

        // Point sampler — Curves doesn't need bilinear filtering (single sample per pixel)
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &m_sampler))) return false;

        m_shader_ready = true;
        API::get()->log_info("[Curves] DX11 shaders ready");
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
        API::get()->log_info("[Curves] DX11 copy texture created: %ux%u", ts.width, ts.height);
        return true;
    }

    void apply_curves_to_resource_dx11(ID3D11Texture2D* target) {
        const auto rd = API::get()->param()->renderer;
        auto device = (ID3D11Device*)rd->device;
        if (!device || !target) return;

        D3D11_TEXTURE2D_DESC target_desc{};
        target->GetDesc(&target_desc);
        if (target_desc.Width == 0 || target_desc.Height == 0) return;

        // Log bind flags once for diagnostics
        if (!m_dx11_logged_flags) {
            m_dx11_logged_flags = true;
            API::get()->log_info("[Curves] DX11 RT: %ux%u fmt=%u bind=0x%x",
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
            auto* p = static_cast<CurvesParamsCB*>(mapped.pData);
            p->Mode = m_mode; p->Formula = m_formula; p->Contrast = m_contrast; p->_pad0 = 0;
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
            API::get()->log_info("[Curves] DX11 effect applied: %ux%u", ts.width, ts.height);
        }

        ID3D11ShaderResourceView* null_srv = nullptr;
        ctx->PSSetShaderResources(0, 1, &null_srv);
    }

    // ========================================================================
    // DX12: Apply Curves to UE4 scene render target
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

        // Point sampler — Curves reads exactly one texel per pixel
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
            if (sig_err) API::get()->log_error("[Curves DX12] Root sig: %s", (const char*)sig_err->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&m_dx12_root_sig)))) return false;

        // Shaders
        ComPtr<ID3DBlob> vsb, vse, psb, pse;
        if (FAILED(D3DCompile(g_curves_vs_src, strlen(g_curves_vs_src), "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &vse))) return false;
        if (FAILED(D3DCompile(g_curves_ps_src, strlen(g_curves_ps_src), "PS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &pse))) return false;

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
            API::get()->log_error("[Curves DX12] PSO failed for format %u", (unsigned)rt_format);
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
        API::get()->log_info("[Curves DX12] Pipeline ready (format %u)", (unsigned)rt_format);
        return true;
    }

    bool ensure_dx12_copy_texture(ID3D12Device* device, ID3D12Resource* src, DX12TargetState& ts) {
        auto sd = src->GetDesc();
        if (ts.copy_tex && ts.width == (UINT)sd.Width && ts.height == sd.Height) return true;
        ts.reset();

        const auto resolved_fmt = resolve_typeless_format(sd.Format);
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Only need copy_tex — we render directly to the native target (no intermediate result_tex)
        D3D12_RESOURCE_DESC copy_desc = sd;
        copy_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        copy_desc.Format = resolved_fmt;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &copy_desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&ts.copy_tex)))) {
            API::get()->log_error("[Curves DX12] Failed to create copy texture");
            return false;
        }

        // SRV heap
        D3D12_DESCRIPTOR_HEAP_DESC shd{}; shd.NumDescriptors = 1;
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&ts.srv_heap)))) { ts.reset(); return false; }

        // RTV heap (descriptor written per-frame on native target)
        D3D12_DESCRIPTOR_HEAP_DESC rhd{}; rhd.NumDescriptors = 1; rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&ts.rtv_heap)))) { ts.reset(); return false; }

        // SRV for copy texture
        D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = resolved_fmt;
        svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        svd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(ts.copy_tex.Get(), &svd, ts.srv_heap->GetCPUDescriptorHandleForHeapStart());
        // RTV descriptor written per-frame in render function

        ts.width = (UINT)sd.Width; ts.height = sd.Height;
        API::get()->log_info("[Curves DX12] Copy texture ready: %ux%u fmt %u", ts.width, ts.height, (unsigned)resolved_fmt);
        return true;
    }
};

// ============================================================================
// Plugin entry point
// ============================================================================
std::unique_ptr<CurvesPlugin> g_plugin{new CurvesPlugin()};
