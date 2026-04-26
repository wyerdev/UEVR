// DX12 backend for the effect runtime.
//
// UEVR opens its own command context for plugins and dispatches them sequentially
// against `m_plugin_pre_render_ctx`. The runtime fetches the recording command
// list via `StereoHook::get_pre_render_command_list()`. The scene RT enters in
// RENDER_TARGET state and must be left in RENDER_TARGET state when execute()
// returns (UEVR's `restore_plugin_rt` assumes that).
//
// Generalizes the proven CAS DX12 path to multi-pass + multi-input + intermediate
// RTs + external textures.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "effect_runtime.hpp"
#include "effect_internal.hpp"

#include "uevr/API.h"
#include "uevr/API.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")

namespace uevr::fx {

namespace {

template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

constexpr UINT k_max_srvs_per_pass = 8;
// Per-pass resources are ring-buffered so multiple execute() invocations on a
// single UEVR command list (e.g. native stereo fix dispatches the plugin twice
// per frame, once per eye) don't stomp each other's not-yet-executed draws.
// Sized to comfortably cover dual-eye dispatch + a few frames in flight.
constexpr UINT k_invocation_ring = 8;

// Mip-gen ring — each mip-pair render uses one slot of k_max_srvs_per_pass
// descriptors. The ring is sized to outrun the GPU comfortably even when both
// the scene snapshot and several IntRTs auto-generate mips per frame, dispatched
// twice for native stereo fix.
constexpr UINT k_mipgen_ring = 256;

// Internal box-downsample shader — bilinear sample at the dest center UV reads
// the 2x2 source-texel block and averages, which is exactly the box filter used
// by D3D11 GenerateMips(). Bound at register t0; ignores all other root params.
static const char* k_mipgen_ps = R"(
Texture2D    Src           : register(t0);
SamplerState LinearSampler : register(s0);
struct PSI { float4 P : SV_Position; float2 UV : TEXCOORD0; };
float4 main(PSI i) : SV_Target { return Src.SampleLevel(LinearSampler, i.UV, 0); }
)";

class DX12Backend : public EffectBackend {
public:
    void execute(const std::vector<RTDesc>&                rt_descs,
                 const std::vector<std::filesystem::path>& ext_tex_paths,
                 const std::vector<PassDesc>&              passes,
                 int                                       snapshot_mips,
                 uint64_t                                  pass_mask) override;

private:
    ComPtr<ID3D12Device>        m_device;
    ComPtr<ID3D12RootSignature> m_root_sig;
    ComPtr<ID3DBlob>            m_vs_blob;
    UINT                        m_srv_descriptor_size = 0;
    UINT                        m_rtv_descriptor_size = 0;

    struct PassPSO {
        const char*               key    = nullptr;
        DXGI_FORMAT               format = DXGI_FORMAT_UNKNOWN;
        int                       cs_sel = -1;        // scene_decode_cache_selector(cs, opt_in)
        ComPtr<ID3D12PipelineState> pso;
    };
    std::vector<PassPSO> m_pso_cache;

    struct PassResources {
        // CB ring: k_invocation_ring chunks of cb_chunk bytes each.
        ComPtr<ID3D12Resource>       cb;
        uint8_t*                     cb_mapped = nullptr;
        size_t                       cb_chunk  = 0;     // 256-byte aligned chunk size
        // SRV ring: shader-visible, k_invocation_ring * k_max_srvs_per_pass descriptors.
        ComPtr<ID3D12DescriptorHeap> srv_heap;
        ComPtr<ID3D12DescriptorHeap> rtv_heap; // non-shader-visible, 1 slot (RTV consumed at record time)
    };
    std::vector<PassResources> m_pass_res;
    UINT64 m_invocation_count = 0;

    struct IntRT {
        UINT                         w = 0, h = 0;
        DXGI_FORMAT                  fmt = DXGI_FORMAT_UNKNOWN;
        UINT                         mip_levels = 1;
        bool                         auto_gen_mips = false;
        ComPtr<ID3D12Resource>       tex;
        ComPtr<ID3D12DescriptorHeap> srv_heap;          // non-shader-visible, full-mip-chain SRV
        ComPtr<ID3D12DescriptorHeap> rtv_heap;          // non-shader-visible (mip 0 RTV)
        ComPtr<ID3D12DescriptorHeap> per_mip_srv_heap;  // single-mip SRV per slot, mip_levels entries
        ComPtr<ID3D12DescriptorHeap> per_mip_rtv_heap;  // mip_levels RTVs
        D3D12_RESOURCE_STATES        state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    };

    struct SceneSlot {
        ID3D12Resource*              identity = nullptr; // weak
        UINT                         w = 0, h = 0;
        DXGI_FORMAT                  fmt = DXGI_FORMAT_UNKNOWN;
        UINT                         mip_levels = 1;
        ComPtr<ID3D12Resource>       copy_tex;          // PIXEL_SHADER_RESOURCE most of the time
        ComPtr<ID3D12DescriptorHeap> copy_srv_heap;     // non-shader-visible, full-mip-chain SRV
        ComPtr<ID3D12DescriptorHeap> per_mip_srv_heap;  // non-shader-visible, mip_levels descriptors (single-mip SRV per slot)
        ComPtr<ID3D12DescriptorHeap> per_mip_rtv_heap;  // non-shader-visible, mip_levels RTVs
        // Intermediate RTs are owned per-slot so native-stereo-fix's per-eye dispatch
        // (different scene sizes on the same in-flight cmd list) doesn't destroy a
        // resource the GPU is still about to read on the previous invocation's draws.
        std::vector<IntRT>           int_rts;
    };
    SceneSlot m_scene_slots[2]{};

    // Backend-resident RTs shared across SceneSlots — see
    // `RTDesc::shared_across_scene_slots`. Indexed by RT id; only populated
    // for opted-in descs.
    std::vector<IntRT> m_shared_rts;

    struct ExtTex {
        bool                         tried = false;
        std::filesystem::path        loaded_path;       // for hot-swap detection
        ComPtr<ID3D12Resource>       tex;
        ComPtr<ID3D12Resource>       upload;            // kept alive for backend lifetime (small)
        ComPtr<ID3D12DescriptorHeap> srv_heap;          // non-shader-visible source
    };
    std::vector<ExtTex> m_ext_textures;

    // Mip-gen support — dedicated dummy CB (root sig requires CBV at b0 even
    // when the mip-gen shader doesn't reference it) and a shader-visible SRV
    // ring used by mip-gen draws.
    ComPtr<ID3D12Resource>       m_dummy_cb;
    ComPtr<ID3D12DescriptorHeap> m_mipgen_srv_heap;   // shader-visible, k_mipgen_ring slots
    UINT                         m_mipgen_invocation = 0;

    bool ensure_static_state(ID3D12Device* device);
    bool ensure_pso(ID3D12Device* device, const char* ps_hlsl, DXGI_FORMAT fmt,
                    SceneRTColorSpace cs, bool decode_opt_in, ID3D12PipelineState** out);
    bool ensure_pass_resources(ID3D12Device* device, size_t pi, size_t cb_size);
    SceneSlot* ensure_scene_slot(ID3D12Device* device, ID3D12Resource* native, UINT w, UINT h, DXGI_FORMAT fmt, UINT mip_levels);
    bool ensure_int_rt(ID3D12Device* device, SceneSlot* scene, size_t idx, const RTDesc& desc);
    // Returns the IntRT slot for `idx` — backend-shared pool when the desc opts
    // in, otherwise the scene's per-slot vector. Both grown to fit `idx` if needed.
    IntRT& int_rt_slot(SceneSlot* scene, size_t idx, const RTDesc& desc) {
        auto& store = desc.shared_across_scene_slots ? m_shared_rts : scene->int_rts;
        if (store.size() <= idx) store.resize(idx + 1);
        return store[idx];
    }
    bool ensure_ext_tex(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, size_t idx,
                        const std::filesystem::path& path);
    // Renders mips 1..mip_levels-1 from mip 0 of `tex`. Mip 0 must enter in
    // PIXEL_SHADER_RESOURCE; subres 1..N-1 must enter in RENDER_TARGET. On exit,
    // ALL subresources are PIXEL_SHADER_RESOURCE.
    void generate_mips(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex,
                       UINT base_w, UINT base_h, UINT mip_levels, DXGI_FORMAT fmt,
                       D3D12_CPU_DESCRIPTOR_HANDLE per_mip_srvs,
                       D3D12_CPU_DESCRIPTOR_HANDLE per_mip_rtvs);

    static D3D12_CPU_DESCRIPTOR_HANDLE offset(D3D12_CPU_DESCRIPTOR_HANDLE h, UINT i, UINT inc) {
        h.ptr += SIZE_T{i} * inc; return h;
    }
};

bool DX12Backend::ensure_static_state(ID3D12Device* device) {
    if (m_device.Get() != device) {
        m_device = device;
        m_root_sig.Reset();
        m_vs_blob.Reset();
        m_pso_cache.clear();
        m_pass_res.clear();
        for (auto& s : m_scene_slots) s = {};
        m_shared_rts.clear();
        m_ext_textures.clear();
        m_dummy_cb.Reset();
        m_mipgen_srv_heap.Reset();
        m_mipgen_invocation = 0;
        m_srv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    if (!m_root_sig) {
        D3D12_DESCRIPTOR_RANGE1 srv_range{};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = k_max_srvs_per_pass;
        srv_range.BaseShaderRegister = 0;
        srv_range.Flags              = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        D3D12_ROOT_PARAMETER1 rp[2]{};
        rp[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        rp[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        rp[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        ss.MaxLOD           = D3D12_FLOAT32_MAX;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd{};
        rsd.Version                       = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rsd.Desc_1_1.NumParameters        = 2;
        rsd.Desc_1_1.pParameters          = rp;
        rsd.Desc_1_1.NumStaticSamplers    = 1;
        rsd.Desc_1_1.pStaticSamplers      = &ss;
        ComPtr<ID3DBlob> sb, se;
        if (FAILED(D3D12SerializeVersionedRootSignature(&rsd, &sb, &se))) {
            if (se) uevr::API::get()->log_error("[fx/dx12] root sig: %s", (const char*)se->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sb->GetBufferPointer(), sb->GetBufferSize(),
                                               IID_PPV_ARGS(&m_root_sig)))) return false;
    }
    if (!m_vs_blob) {
        ComPtr<ID3DBlob> vse;
        if (FAILED(D3DCompile(detail::k_fullscreen_vs, std::strlen(detail::k_fullscreen_vs),
                              "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &m_vs_blob, &vse))) return false;
    }
    if (!m_dummy_cb) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = 256;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&m_dummy_cb)))) return false;
    }
    if (!m_mipgen_srv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = k_mipgen_ring * k_max_srvs_per_pass;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_mipgen_srv_heap)))) return false;
    }
    return true;
}

bool DX12Backend::ensure_pso(ID3D12Device* device, const char* ps_hlsl, DXGI_FORMAT fmt,
                              SceneRTColorSpace cs, bool decode_opt_in,
                              ID3D12PipelineState** out) {
    const int cs_sel = detail::scene_decode_cache_selector(cs, decode_opt_in);
    for (auto& e : m_pso_cache) {
        if (e.key == ps_hlsl && e.format == fmt && e.cs_sel == cs_sel) {
            *out = e.pso.Get();
            return e.pso != nullptr;
        }
    }
    std::string combined;
    const char* src_ptr = ps_hlsl;
    size_t      src_len = std::strlen(ps_hlsl);
    if (decode_opt_in) {
        const char* macros = detail::scene_decode_macro_block(cs);
        combined.reserve(std::strlen(macros) + src_len);
        combined.append(macros);
        combined.append(ps_hlsl, src_len);
        src_ptr = combined.c_str();
        src_len = combined.size();
    }
    ComPtr<ID3DBlob> psb, pse;
    if (FAILED(D3DCompile(src_ptr, src_len, "PS", nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &psb, &pse))) {
        if (pse) uevr::API::get()->log_error("[fx/dx12] PS compile (cs_sel=%d, fmt=%s): %s",
                                              cs_sel, detail::dxgi_format_name(fmt),
                                              (const char*)pse->GetBufferPointer());
        m_pso_cache.push_back({ ps_hlsl, fmt, cs_sel, nullptr });
        return false;
    }
    PassPSO entry{ ps_hlsl, fmt, cs_sel, {} };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature                       = m_root_sig.Get();
    pd.VS                                   = { m_vs_blob->GetBufferPointer(), m_vs_blob->GetBufferSize() };
    pd.PS                                   = { psb->GetBufferPointer(), psb->GetBufferSize() };
    pd.RasterizerState.FillMode             = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode             = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask                           = UINT_MAX;
    pd.PrimitiveTopologyType                = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets                     = 1;
    pd.RTVFormats[0]                        = fmt;
    pd.SampleDesc.Count                     = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&entry.pso)))) {
        m_pso_cache.push_back(std::move(entry));
        return false;
    }
    *out = entry.pso.Get();
    m_pso_cache.push_back(std::move(entry));
    return true;
}

bool DX12Backend::ensure_pass_resources(ID3D12Device* device, size_t pi, size_t cb_size) {
    if (m_pass_res.size() <= pi) m_pass_res.resize(pi + 1);
    auto& r = m_pass_res[pi];

    if (!r.srv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = k_invocation_ring * k_max_srvs_per_pass;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&r.srv_heap)))) return false;
    }
    if (!r.rtv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&r.rtv_heap)))) return false;
    }

    if (cb_size > 0) {
        const size_t aligned = (cb_size + 255u) & ~size_t{255u}; // 256-byte alignment for CBV
        if (!r.cb || r.cb_chunk < aligned) {
            r.cb.Reset(); r.cb_mapped = nullptr;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width            = aligned * k_invocation_ring;
            bd.Height           = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels        = 1;
            bd.Format           = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&r.cb)))) return false;
            r.cb->Map(0, nullptr, reinterpret_cast<void**>(&r.cb_mapped));
            r.cb_chunk = aligned;
        }
    }
    return true;
}

DX12Backend::SceneSlot* DX12Backend::ensure_scene_slot(ID3D12Device* device, ID3D12Resource* native,
                                                       UINT w, UINT h, DXGI_FORMAT fmt, UINT mip_levels) {
    SceneSlot* slot = nullptr;
    for (auto& s : m_scene_slots) if (s.identity == native) { slot = &s; break; }
    if (!slot) {
        for (auto& s : m_scene_slots) if (s.identity == nullptr) { slot = &s; break; }
        if (!slot) slot = &m_scene_slots[1];
    }

    const auto rf = detail::resolve_typeless_format(fmt);
    if (slot->identity != native || slot->w != w || slot->h != h || slot->fmt != rf || slot->mip_levels != mip_levels) {
        *slot = {};
        // Mirror the source RT's descriptor exactly so CopyResource is valid for
        // any layout the engine throws at us (single Texture2D, Texture2DArray
        // for native stereo, mipped RTs, etc.). Strip RT/UAV flags unless we
        // need to render-to-mip for snapshot mip-gen.
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = native->GetDesc();
        td.Format    = rf;
        td.MipLevels = static_cast<UINT16>(mip_levels);
        td.Flags     = (mip_levels > 1)
                         ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                         : D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&slot->copy_tex)))) return nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&slot->copy_srv_heap)))) return nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format                       = rf;
        svd.ViewDimension                = D3D12_SRV_DIMENSION_TEXTURE2D;
        svd.Shader4ComponentMapping      = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        svd.Texture2D.MipLevels          = mip_levels;
        device->CreateShaderResourceView(slot->copy_tex.Get(), &svd,
                                         slot->copy_srv_heap->GetCPUDescriptorHandleForHeapStart());
        if (mip_levels > 1) {
            D3D12_DESCRIPTOR_HEAP_DESC ms{}; ms.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; ms.NumDescriptors = mip_levels;
            if (FAILED(device->CreateDescriptorHeap(&ms, IID_PPV_ARGS(&slot->per_mip_srv_heap)))) return nullptr;
            D3D12_DESCRIPTOR_HEAP_DESC mr{}; mr.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; mr.NumDescriptors = mip_levels;
            if (FAILED(device->CreateDescriptorHeap(&mr, IID_PPV_ARGS(&slot->per_mip_rtv_heap)))) return nullptr;
            for (UINT i = 0; i < mip_levels; ++i) {
                D3D12_SHADER_RESOURCE_VIEW_DESC s{};
                s.Format                          = rf;
                s.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
                s.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                s.Texture2D.MostDetailedMip       = i;
                s.Texture2D.MipLevels             = 1;
                device->CreateShaderResourceView(slot->copy_tex.Get(), &s,
                                                 offset(slot->per_mip_srv_heap->GetCPUDescriptorHandleForHeapStart(), i, m_srv_descriptor_size));
                D3D12_RENDER_TARGET_VIEW_DESC r{};
                r.Format             = rf;
                r.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
                r.Texture2D.MipSlice = i;
                device->CreateRenderTargetView(slot->copy_tex.Get(), &r,
                                               offset(slot->per_mip_rtv_heap->GetCPUDescriptorHandleForHeapStart(), i, m_rtv_descriptor_size));
            }
        }
        slot->identity = native; slot->w = w; slot->h = h; slot->fmt = rf; slot->mip_levels = mip_levels;
    }
    return slot;
}

bool DX12Backend::ensure_int_rt(ID3D12Device* device, SceneSlot* scene, size_t idx, const RTDesc& desc) {
    auto& rt = int_rt_slot(scene, idx, desc);

    UINT w = scene->w, h = scene->h;
    switch (desc.size_mode) {
        case RTDesc::SizeMode::Backbuffer:    break;
        case RTDesc::SizeMode::BackbufferDiv: w = std::max(1u, scene->w / std::max(1, desc.w_or_div));
                                              h = std::max(1u, scene->h / std::max(1, desc.h_or_div)); break;
        case RTDesc::SizeMode::Fixed:         w = std::max(1, desc.w_or_div);
                                              h = std::max(1, desc.h_or_div); break;
    }
    if (rt.tex && rt.w == w && rt.h == h && rt.fmt == desc.format) return true;
    if (desc.persistent && rt.tex) return true;

    rt = {};
    const UINT mips = static_cast<UINT>(std::max(1, desc.mip_levels));
    const bool gen_mips = desc.auto_generate_mips && mips > 1;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = w; td.Height = h;
    td.DepthOrArraySize = 1; td.MipLevels = static_cast<UINT16>(mips);
    td.Format           = desc.format;
    td.SampleDesc.Count = 1;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                               IID_PPV_ARGS(&rt.tex)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC sd{};
    sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sd.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&rt.srv_heap)))) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format                  = desc.format;
    svd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    svd.Texture2D.MipLevels     = mips;
    device->CreateShaderResourceView(rt.tex.Get(), &svd, rt.srv_heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rd.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rt.rtv_heap)))) return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format        = desc.format;
    rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(rt.tex.Get(), &rtvd, rt.rtv_heap->GetCPUDescriptorHandleForHeapStart());
    if (gen_mips) {
        D3D12_DESCRIPTOR_HEAP_DESC ms{}; ms.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; ms.NumDescriptors = mips;
        if (FAILED(device->CreateDescriptorHeap(&ms, IID_PPV_ARGS(&rt.per_mip_srv_heap)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC mr{}; mr.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; mr.NumDescriptors = mips;
        if (FAILED(device->CreateDescriptorHeap(&mr, IID_PPV_ARGS(&rt.per_mip_rtv_heap)))) return false;
        for (UINT i = 0; i < mips; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format                    = desc.format;
            s.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            s.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Texture2D.MostDetailedMip = i;
            s.Texture2D.MipLevels       = 1;
            device->CreateShaderResourceView(rt.tex.Get(), &s,
                offset(rt.per_mip_srv_heap->GetCPUDescriptorHandleForHeapStart(), i, m_srv_descriptor_size));
            D3D12_RENDER_TARGET_VIEW_DESC r{};
            r.Format             = desc.format;
            r.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
            r.Texture2D.MipSlice = i;
            device->CreateRenderTargetView(rt.tex.Get(), &r,
                offset(rt.per_mip_rtv_heap->GetCPUDescriptorHandleForHeapStart(), i, m_rtv_descriptor_size));
        }
    }
    rt.w = w; rt.h = h; rt.fmt = desc.format; rt.mip_levels = mips; rt.auto_gen_mips = gen_mips;
    rt.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    return true;
}

void DX12Backend::generate_mips(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex,
                                UINT base_w, UINT base_h, UINT mip_levels, DXGI_FORMAT fmt,
                                D3D12_CPU_DESCRIPTOR_HANDLE per_mip_srvs,
                                D3D12_CPU_DESCRIPTOR_HANDLE per_mip_rtvs) {
    if (mip_levels < 2) return;

    // Acquire / cache mip-gen PSO for this format.
    ID3D12PipelineState* mipgen_pso = nullptr;
    if (!ensure_pso(m_device.Get(), k_mipgen_ps, fmt, SceneRTColorSpace::Unknown, false, &mipgen_pso) || mipgen_pso == nullptr) return;

    cmd->SetGraphicsRootSignature(m_root_sig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetPipelineState(mipgen_pso);
    cmd->SetGraphicsRootConstantBufferView(0, m_dummy_cb->GetGPUVirtualAddress());
    ID3D12DescriptorHeap* heaps[] = { m_mipgen_srv_heap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    auto barrier_subres = [&](UINT mip, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = tex;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = mip;
        cmd->ResourceBarrier(1, &b);
    };

    for (UINT i = 1; i < mip_levels; ++i) {
        // Allocate one ring slot of k_max_srvs_per_pass descriptors; replicate the
        // source-mip SRV across all slots so the descriptor table is fully populated.
        const UINT slot = (m_mipgen_invocation++) % k_mipgen_ring;
        D3D12_CPU_DESCRIPTOR_HANDLE dst_base = offset(
            m_mipgen_srv_heap->GetCPUDescriptorHandleForHeapStart(),
            slot * k_max_srvs_per_pass, m_srv_descriptor_size);
        D3D12_CPU_DESCRIPTOR_HANDLE src = offset(per_mip_srvs, i - 1, m_srv_descriptor_size);
        for (UINT j = 0; j < k_max_srvs_per_pass; ++j) {
            m_device->CopyDescriptorsSimple(1, offset(dst_base, j, m_srv_descriptor_size),
                                            src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_mipgen_srv_heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64{slot} * k_max_srvs_per_pass * m_srv_descriptor_size;

        // Contract: ALL mips entered PSR. Transition dest mip i: PSR -> RT before
        // rendering. (Source mip i-1 stays PSR.)
        barrier_subres(i, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        UINT mw = std::max(1u, base_w >> i);
        UINT mh = std::max(1u, base_h >> i);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = offset(per_mip_rtvs, i, m_rtv_descriptor_size);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->SetGraphicsRootDescriptorTable(1, gpu);
        D3D12_VIEWPORT vp{}; vp.Width = (float)mw; vp.Height = (float)mh; vp.MaxDepth = 1.0f;
        cmd->RSSetViewports(1, &vp);
        D3D12_RECT sc{}; sc.right = mw; sc.bottom = mh;
        cmd->RSSetScissorRects(1, &sc);
        cmd->DrawInstanced(3, 1, 0, 0);

        // Transition newly-rendered mip i: RT -> PSR so the next iteration's draw
        // (or downstream sampling) can read it.
        barrier_subres(i, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

bool DX12Backend::ensure_ext_tex(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, size_t idx,
                                  const std::filesystem::path& path) {
    if (m_ext_textures.size() <= idx) m_ext_textures.resize(idx + 1);
    auto& e = m_ext_textures[idx];
    // Hot-swap: if the requested path differs from what was loaded, reset the slot.
    // ComPtr release drops our CPU ref; D3D12 keeps the resource alive until any
    // in-flight command list referencing it has completed (deferred destruction).
    if (e.tried && e.loaded_path != path) { e = {}; }
    if (e.tried) return e.tex != nullptr;
    e.tried = true;
    e.loaded_path = path;

    auto img = detail::load_image_rgba8(path.c_str());
    if (img.width == 0) {
        uevr::API::get()->log_warn("[fx/dx12] failed to load texture: %ls", path.c_str());
        return false;
    }

    D3D12_HEAP_PROPERTIES hp_def{}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES hp_up{};  hp_up.Type  = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = img.width; td.Height = img.height;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    if (FAILED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&e.tex)))) return false;

    UINT64 row_bytes = static_cast<UINT64>(img.width) * 4;
    UINT64 row_pitch = (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~UINT64{D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1};
    UINT64 upload_size = row_pitch * static_cast<UINT64>(img.height);

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = upload_size;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &bd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&e.upload)))) { e.tex.Reset(); return false; }

    void* mapped = nullptr;
    e.upload->Map(0, nullptr, &mapped);
    auto* dst = static_cast<uint8_t*>(mapped);
    for (int y = 0; y < img.height; ++y) {
        std::memcpy(dst + y * row_pitch, img.rgba8.data() + y * row_bytes, row_bytes);
    }
    e.upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = e.upload.Get();
    src.Type                          = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset        = 0;
    src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width    = img.width;
    src.PlacedFootprint.Footprint.Height   = img.height;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(row_pitch);
    D3D12_TEXTURE_COPY_LOCATION dst_loc{}; dst_loc.pResource = e.tex.Get();
    dst_loc.Type                      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex          = 0;
    cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = e.tex.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&e.srv_heap)))) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    svd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    svd.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(e.tex.Get(), &svd, e.srv_heap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

void DX12Backend::execute(const std::vector<RTDesc>&                rt_descs,
                          const std::vector<std::filesystem::path>& ext_tex_paths,
                          const std::vector<PassDesc>&              passes,
                          int                                       snapshot_mips,
                          uint64_t                                  pass_mask) {
    static int s_last_exit = -2;
    auto exit_log = [&](int reason, const char* msg) {
        if (reason == s_last_exit) return;
        s_last_exit = reason;
        if (auto& a = uevr::API::get()) a->log_info("[fx/dx12] execute() exit: %s", msg);
    };

    if (passes.empty()) { exit_log(0, "no passes"); return; }

    auto& api = uevr::API::get();
    auto rd  = api->param()->renderer;
    if (rd->renderer_type != UEVR_RENDERER_D3D12 || rd->device == nullptr) {
        exit_log(1, "renderer not DX12 or device null"); return;
    }
    auto device = static_cast<ID3D12Device*>(rd->device);

    auto scene_rt_obj = uevr::API::StereoHook::get_scene_render_target();
    if (!scene_rt_obj) { exit_log(2, "scene_render_target object null"); return; }
    auto scene_native = static_cast<ID3D12Resource*>(scene_rt_obj->get_native_resource());
    if (!scene_native) { exit_log(3, "scene native resource null"); return; }
    auto sd = scene_native->GetDesc();
    if (sd.Width == 0 || sd.Height == 0) { exit_log(4, "scene dims zero"); return; }

    auto cmd = static_cast<ID3D12GraphicsCommandList*>(uevr::API::StereoHook::get_pre_render_command_list());
    if (!cmd) { exit_log(5, "pre_render_command_list null"); return; }

    if (!ensure_static_state(device)) { exit_log(6, "ensure_static_state failed"); return; }
    UINT snap_mips = static_cast<UINT>(std::max(1, snapshot_mips));
    auto* scene = ensure_scene_slot(device, scene_native, static_cast<UINT>(sd.Width), sd.Height, sd.Format, snap_mips);
    if (!scene) { exit_log(7, "ensure_scene_slot failed"); return; }

    exit_log(99, "running passes");

    // Pick a ring slot for this invocation. The same UEVR command list may carry
    // multiple plugin dispatches (native stereo fix = one per eye), and the GPU
    // executes them all later — so per-pass shader-visible descriptors and CB
    // writes for *this* invocation must not alias the previous one's.
    const UINT invocation_slot = static_cast<UINT>(m_invocation_count++ % k_invocation_ring);

    detail::set_scene_size(scene->w, scene->h);
    detail::set_scene_rt_format(scene->fmt);
    detail::log_scene_rt_identity_change(scene_native, scene->fmt, scene->w, scene->h);

    // Snapshot scene -> copy_tex (mip 0). For multi-mip snapshots, regenerate the
    // chain on copy_tex via PS-based 2x box-downsample.
    {
        // Scene RT (full subres) PSR/RT -> COPY_SOURCE; copy_tex mip 0 PSR -> COPY_DEST.
        D3D12_RESOURCE_BARRIER b[2]{};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource   = scene_native;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.pResource   = scene->copy_tex.Get();
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.Subresource = (snap_mips > 1) ? UINT{0} : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(2, b);

        if (snap_mips > 1) {
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = scene_native;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = scene->copy_tex.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        } else {
            cmd->CopyResource(scene->copy_tex.Get(), scene_native);
        }

        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(2, b);

        if (snap_mips > 1) {
            // All mips of copy_tex are now PSR. generate_mips handles per-mip transitions.
            generate_mips(cmd, scene->copy_tex.Get(), scene->w, scene->h, snap_mips, scene->fmt,
                          scene->per_mip_srv_heap->GetCPUDescriptorHandleForHeapStart(),
                          scene->per_mip_rtv_heap->GetCPUDescriptorHandleForHeapStart());
        }
    }

    cmd->SetGraphicsRootSignature(m_root_sig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (size_t pi = 0; pi < passes.size(); ++pi) {
        // Honor caller-supplied pass mask (used by plugins to skip passes
        // on the second native-stereo-fix dispatch — see EffectRuntime::execute(mask)).
        if (pi < 64 && (pass_mask & (uint64_t(1) << pi)) == 0) continue;
        const auto& p = passes[pi];

        // Resolve output.
        ID3D12Resource* out_res = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE out_rtv{};
        UINT out_w = 0, out_h = 0;
        DXGI_FORMAT out_fmt = DXGI_FORMAT_UNKNOWN;
        bool out_is_scene = false;
        IntRT* out_int = nullptr;

        if (p.output == OUTPUT_SCENE) {
            out_res = scene_native; out_w = scene->w; out_h = scene->h;
            out_fmt = (p.output_format_override != DXGI_FORMAT_UNKNOWN) ? p.output_format_override : scene->fmt;
            out_is_scene = true;
        } else if (p.output >= 0 && static_cast<size_t>(p.output) < rt_descs.size()) {
            if (!ensure_int_rt(device, scene, p.output, rt_descs[p.output])) {
                static int s_skip = -1; if (s_skip != (int)pi) { s_skip = (int)pi;
                    api->log_warn("[fx/dx12] pass %zu skipped: ensure_int_rt failed (rt %d)", pi, p.output); }
                continue;
            }
            out_int = &int_rt_slot(scene, p.output, rt_descs[p.output]);
            out_res = out_int->tex.Get();
            out_w = out_int->w; out_h = out_int->h;
            out_fmt = (p.output_format_override != DXGI_FORMAT_UNKNOWN) ? p.output_format_override : out_int->fmt;
        } else {
            static int s_skip = -1; if (s_skip != (int)pi) { s_skip = (int)pi;
                api->log_warn("[fx/dx12] pass %zu skipped: invalid output id %d", pi, p.output); }
            continue;
        }

        ID3D12PipelineState* pso = nullptr;
        const auto pass_cs = detail::classify_scene_rt_colorspace(scene->fmt);
        if (!ensure_pso(device, p.ps_hlsl, out_fmt, pass_cs,
                         p.needs_scene_colorspace_decode, &pso) || pso == nullptr) {
            static int s_skip = -1; if (s_skip != (int)pi) { s_skip = (int)pi;
                api->log_warn("[fx/dx12] pass %zu skipped: PSO compile/create failed (out_fmt=%s)",
                              pi, detail::dxgi_format_name(out_fmt)); }
            continue;
        }
        if (!ensure_pass_resources(device, pi, p.cb_size)) {
            static int s_skip = -1; if (s_skip != (int)pi) { s_skip = (int)pi;
                api->log_warn("[fx/dx12] pass %zu skipped: ensure_pass_resources failed", pi); }
            continue;
        }
        auto& res = m_pass_res[pi];

        // Upload cbuffer into this invocation's ring slot.
        if (p.cb_size > 0 && p.cb_data != nullptr && res.cb_mapped && res.cb_chunk > 0) {
            std::memcpy(res.cb_mapped + invocation_slot * res.cb_chunk, p.cb_data, p.cb_size);
        }

        // Resolve up to k_max_srvs_per_pass input SRVs and copy into this invocation's
        // ring slot of the pass's shader-visible heap.
        D3D12_CPU_DESCRIPTOR_HANDLE dst_base = offset(
            res.srv_heap->GetCPUDescriptorHandleForHeapStart(),
            invocation_slot * k_max_srvs_per_pass, m_srv_descriptor_size);
        const size_t n_in = std::min<size_t>(p.inputs.size(), k_max_srvs_per_pass);
        bool inputs_ok = true;
        for (size_t i = 0; i < n_in; ++i) {
            const int id = p.inputs[i];
            D3D12_CPU_DESCRIPTOR_HANDLE src{};
            if (id == INPUT_SCENE) {
                src = scene->copy_srv_heap->GetCPUDescriptorHandleForHeapStart();
            } else if (id >= EXTERNAL_TEX_BASE) {
                const size_t ext_idx = static_cast<size_t>(id - EXTERNAL_TEX_BASE);
                if (ext_idx >= ext_tex_paths.size() ||
                    !ensure_ext_tex(device, cmd, ext_idx, ext_tex_paths[ext_idx])) {
                    inputs_ok = false; break;
                }
                src = m_ext_textures[ext_idx].srv_heap->GetCPUDescriptorHandleForHeapStart();
            } else if (id >= 0 && static_cast<size_t>(id) < rt_descs.size()) {
                auto& store = rt_descs[id].shared_across_scene_slots ? m_shared_rts : scene->int_rts;
                if (static_cast<size_t>(id) >= store.size()) { inputs_ok = false; break; }
                auto& rt = store[id];
                if (!rt.tex) { inputs_ok = false; break; }
                if (rt.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource   = rt.tex.Get();
                    b.Transition.StateBefore = rt.state;
                    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    cmd->ResourceBarrier(1, &b);
                    rt.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                }
                src = rt.srv_heap->GetCPUDescriptorHandleForHeapStart();
            } else {
                inputs_ok = false; break;
            }
            device->CopyDescriptorsSimple(1, offset(dst_base, static_cast<UINT>(i), m_srv_descriptor_size),
                                          src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        if (!inputs_ok) continue;
        // Pad remaining slots with the first SRV to keep the descriptor table fully populated
        // (range was declared with k_max_srvs_per_pass descriptors).
        if (n_in == 0) continue;
        for (size_t i = n_in; i < k_max_srvs_per_pass; ++i) {
            device->CopyDescriptorsSimple(1, offset(dst_base, static_cast<UINT>(i), m_srv_descriptor_size),
                                          dst_base, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        // Output transition.
        if (out_is_scene) {
            // Scene is already RENDER_TARGET (UEVR's prepare_plugin_rt). Build RTV into pass slot.
            D3D12_RENDER_TARGET_VIEW_DESC rtvd{};
            rtvd.Format        = out_fmt;
            rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            out_rtv = res.rtv_heap->GetCPUDescriptorHandleForHeapStart();
            device->CreateRenderTargetView(out_res, &rtvd, out_rtv);
        } else {
            if (out_int->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = out_int->tex.Get();
                b.Transition.StateBefore = out_int->state;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cmd->ResourceBarrier(1, &b);
                out_int->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            out_rtv = out_int->rtv_heap->GetCPUDescriptorHandleForHeapStart();
        }

        cmd->SetPipelineState(pso);
        if (p.cb_size > 0 && res.cb && res.cb_chunk > 0) {
            cmd->SetGraphicsRootConstantBufferView(
                0, res.cb->GetGPUVirtualAddress() + invocation_slot * res.cb_chunk);
        }
        ID3D12DescriptorHeap* heaps[] = { res.srv_heap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        D3D12_GPU_DESCRIPTOR_HANDLE table_base = res.srv_heap->GetGPUDescriptorHandleForHeapStart();
        table_base.ptr += UINT64{invocation_slot} * k_max_srvs_per_pass * m_srv_descriptor_size;
        cmd->SetGraphicsRootDescriptorTable(1, table_base);
        cmd->OMSetRenderTargets(1, &out_rtv, FALSE, nullptr);

        D3D12_VIEWPORT vp{};
        vp.Width = static_cast<float>(out_w); vp.Height = static_cast<float>(out_h); vp.MaxDepth = 1.0f;
        cmd->RSSetViewports(1, &vp);
        D3D12_RECT scissor{}; scissor.right = out_w; scissor.bottom = out_h;
        cmd->RSSetScissorRects(1, &scissor);
        cmd->DrawInstanced(3, 1, 0, 0);

        // If the output RT was declared with auto_generate_mips, transition all
        // subresources to PSR and rebuild the chain. Final state: all subres PSR.
        if (out_int && out_int->auto_gen_mips && out_int->mip_levels > 1) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = out_int->tex.Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmd->ResourceBarrier(1, &b);
            out_int->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

            generate_mips(cmd, out_int->tex.Get(), out_int->w, out_int->h,
                          out_int->mip_levels, out_int->fmt,
                          out_int->per_mip_srv_heap->GetCPUDescriptorHandleForHeapStart(),
                          out_int->per_mip_rtv_heap->GetCPUDescriptorHandleForHeapStart());

            // Restore pass-loop state (descriptor heaps, root sig, primitive topology
            // are all the same; PSO will be re-set for the next pass anyway).
        }
    }

    // UEVR's restore_plugin_rt expects the scene RT in RENDER_TARGET state — we never
    // transitioned it away after the snapshot, so nothing to do.
}

} // namespace

std::unique_ptr<EffectBackend> make_backend_d3d12() {
    return std::unique_ptr<EffectBackend>(new DX12Backend());
}

} // namespace uevr::fx
