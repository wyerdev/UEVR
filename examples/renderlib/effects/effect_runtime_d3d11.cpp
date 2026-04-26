// DX11 backend for the effect runtime. Implements the same proven pipeline
// the existing 16 plugins use (snapshot scene → bind SRV → fullscreen tri →
// write back to scene RT) but generalized to handle:
//   - Multiple input SRVs per pass (scene snapshot, intermediate RTs, external textures)
//   - Multiple passes in declared order
//   - Intermediate RTs sized relative to the scene RT
//   - Per-pass cbuffers
//
// DX11 has no plugin command list — everything runs on the immediate context.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "effect_runtime.hpp"
#include "effect_internal.hpp"

#include "uevr/API.h"
#include "uevr/API.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

#pragma comment(lib, "d3dcompiler.lib")

namespace uevr::fx {

namespace {

template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

class DX11Backend : public EffectBackend {
public:
    void execute(const std::vector<RTDesc>&                rt_descs,
                 const std::vector<std::filesystem::path>& ext_tex_paths,
                 const std::vector<PassDesc>&              passes,
                 int                                       snapshot_mips,
                 uint64_t                                  pass_mask) override;

private:
    ComPtr<ID3D11Device>         m_device;
    ComPtr<ID3D11DeviceContext>  m_ctx;
    ComPtr<ID3D11VertexShader>   m_vs;
    ComPtr<ID3D11SamplerState>   m_sampler_linear;

    struct PassPS {
        const char*               key      = nullptr;   // ps_hlsl pointer used as identity
        DXGI_FORMAT               format   = DXGI_FORMAT_UNKNOWN;
        int                       cs_sel   = -1;        // scene_decode_cache_selector(cs, opt_in)
        ComPtr<ID3D11PixelShader> ps;
    };
    std::vector<PassPS> m_ps_cache;

    struct IntRT {
        UINT                              w = 0, h = 0;
        DXGI_FORMAT                       fmt = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D11Texture2D>           tex;
        ComPtr<ID3D11ShaderResourceView>  srv;
        ComPtr<ID3D11RenderTargetView>    rtv;
    };

    struct SceneSlot {
        ID3D11Texture2D*                  identity = nullptr; // weak
        UINT                              w = 0, h = 0;
        DXGI_FORMAT                       fmt = DXGI_FORMAT_UNKNOWN;
        UINT                              mip_levels = 1;
        ComPtr<ID3D11Texture2D>           copy_tex;
        ComPtr<ID3D11ShaderResourceView>  copy_srv;
        ComPtr<ID3D11RenderTargetView>    rtv;
        // Per-size cache of intermediates so native-stereo-fix's two scene sizes
        // each keep their own resources alive across alternating dispatches.
        std::vector<IntRT>                int_rts;
    };
    SceneSlot m_scene_slots[2]{};

    // Backend-resident RTs shared across SceneSlots. Indexed by RT id; only
    // populated for RTDescs with `shared_across_scene_slots = true`. See
    // `RTDesc::shared_across_scene_slots` for rationale (native-stereo-fix
    // history convergence).
    std::vector<IntRT> m_shared_rts;

    struct ExtTex {
        bool                              tried = false;
        std::filesystem::path             loaded_path;     // for hot-swap detection
        ComPtr<ID3D11ShaderResourceView>  srv;
    };
    std::vector<ExtTex> m_ext_textures;

    std::vector<ComPtr<ID3D11Buffer>> m_pass_cbs;
    std::vector<size_t>               m_pass_cb_sizes;

    bool ensure_static_state(ID3D11Device* device);
    bool ensure_pass_ps(ID3D11Device* device, const char* ps_hlsl, DXGI_FORMAT fmt,
                        SceneRTColorSpace cs, bool decode_opt_in, ID3D11PixelShader** out_ps);
    SceneSlot* ensure_scene_slot(ID3D11Device* device, ID3D11Texture2D* native, UINT w, UINT h, DXGI_FORMAT fmt, UINT mip_levels);
    bool ensure_int_rt(ID3D11Device* device, SceneSlot* scene, size_t idx, const RTDesc& desc);
    // Returns the IntRT slot for `idx` — backend-shared pool when the desc opts in,
    // otherwise the scene's per-slot vector. Both grown to fit `idx` if needed.
    IntRT& int_rt_slot(SceneSlot* scene, size_t idx, const RTDesc& desc) {
        auto& store = desc.shared_across_scene_slots ? m_shared_rts : scene->int_rts;
        if (store.size() <= idx) store.resize(idx + 1);
        return store[idx];
    }
    bool ensure_ext_tex(ID3D11Device* device, size_t idx, const std::filesystem::path& path);
    bool ensure_pass_cb(ID3D11Device* device, size_t idx, size_t cb_size);
};

bool DX11Backend::ensure_static_state(ID3D11Device* device) {
    if (m_device.Get() != device) {
        m_device = device;
        m_ctx.Reset();
        m_vs.Reset();
        m_sampler_linear.Reset();
        m_ps_cache.clear();
        for (auto& s : m_scene_slots) s = {};
        m_shared_rts.clear();
        for (auto& e : m_ext_textures) e = {};
        m_pass_cbs.clear();
        m_pass_cb_sizes.clear();
    }
    if (!m_ctx) device->GetImmediateContext(&m_ctx);
    if (!m_vs) {
        ComPtr<ID3DBlob> vsb, vse;
        if (FAILED(D3DCompile(detail::k_fullscreen_vs, std::strlen(detail::k_fullscreen_vs),
                              "VS", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &vse))) return false;
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs))) return false;
    }
    if (!m_sampler_linear) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &m_sampler_linear))) return false;
    }
    return true;
}

bool DX11Backend::ensure_pass_ps(ID3D11Device* device, const char* ps_hlsl, DXGI_FORMAT fmt,
                                  SceneRTColorSpace cs, bool decode_opt_in,
                                  ID3D11PixelShader** out_ps) {
    const int cs_sel = detail::scene_decode_cache_selector(cs, decode_opt_in);
    for (auto& e : m_ps_cache) {
        if (e.key == ps_hlsl && e.format == fmt && e.cs_sel == cs_sel) {
            *out_ps = e.ps.Get();
            return e.ps != nullptr;
        }
    }
    // When the pass opts in, prepend the decode/encode macro block. Otherwise
    // compile the source verbatim (zero behavior change for existing plugins).
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
        if (pse) uevr::API::get()->log_error("[fx/dx11] PS compile (cs_sel=%d, fmt=%s): %s",
                                              cs_sel, detail::dxgi_format_name(fmt),
                                              (const char*)pse->GetBufferPointer());
        m_ps_cache.push_back({ ps_hlsl, fmt, cs_sel, nullptr });
        return false;
    }
    PassPS entry{ ps_hlsl, fmt, cs_sel, {} };
    if (FAILED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &entry.ps))) {
        m_ps_cache.push_back(std::move(entry));
        return false;
    }
    *out_ps = entry.ps.Get();
    m_ps_cache.push_back(std::move(entry));
    return true;
}

DX11Backend::SceneSlot* DX11Backend::ensure_scene_slot(ID3D11Device* device, ID3D11Texture2D* native,
                                                       UINT w, UINT h, DXGI_FORMAT fmt, UINT mip_levels) {
    SceneSlot* slot = nullptr;
    for (auto& s : m_scene_slots) if (s.identity == native) { slot = &s; break; }
    if (!slot) {
        for (auto& s : m_scene_slots) if (s.identity == nullptr) { slot = &s; break; }
        if (!slot) slot = &m_scene_slots[1]; // evict
    }

    const auto rf = detail::resolve_typeless_format(fmt);
    if (slot->identity != native || slot->w != w || slot->h != h || slot->fmt != rf || slot->mip_levels != mip_levels) {
        *slot = {};
        D3D11_TEXTURE2D_DESC cd{};
        cd.Width = w; cd.Height = h; cd.MipLevels = mip_levels; cd.ArraySize = 1;
        cd.Format = rf; cd.SampleDesc.Count = 1; cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (mip_levels > 1) {
            // GenerateMips() requires both RT-bind and the auto-gen misc flag.
            cd.BindFlags |= D3D11_BIND_RENDER_TARGET;
            cd.MiscFlags  = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        }
        if (FAILED(device->CreateTexture2D(&cd, nullptr, &slot->copy_tex))) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format = rf; svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; svd.Texture2D.MipLevels = mip_levels;
        if (FAILED(device->CreateShaderResourceView(slot->copy_tex.Get(), &svd, &slot->copy_srv))) return nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = rf; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateRenderTargetView(native, &rd, &slot->rtv))) return nullptr;
        slot->identity = native; slot->w = w; slot->h = h; slot->fmt = rf; slot->mip_levels = mip_levels;
    }
    return slot;
}

bool DX11Backend::ensure_int_rt(ID3D11Device* device, SceneSlot* scene, size_t idx, const RTDesc& desc) {
    auto& rt = int_rt_slot(scene, idx, desc);

    UINT w = scene->w, h = scene->h;
    switch (desc.size_mode) {
        case RTDesc::SizeMode::Backbuffer:    break;
        case RTDesc::SizeMode::BackbufferDiv: w = std::max(1u, scene->w / std::max(1, desc.w_or_div));
                                              h = std::max(1u, scene->h / std::max(1, desc.h_or_div)); break;
        case RTDesc::SizeMode::Fixed:         w = std::max(1, desc.w_or_div);
                                              h = std::max(1, desc.h_or_div); break;
    }
    const auto fmt = desc.format;

    if (rt.tex && rt.w == w && rt.h == h && rt.fmt == fmt) return true;
    if (desc.persistent && rt.tex) return true; // never resize persistent RTs once created

    rt = {};
    D3D11_TEXTURE2D_DESC cd{};
    cd.Width = w; cd.Height = h; cd.MipLevels = std::max(1, desc.mip_levels); cd.ArraySize = 1;
    cd.Format = fmt; cd.SampleDesc.Count = 1; cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (desc.auto_generate_mips && cd.MipLevels > 1) {
        cd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }
    if (FAILED(device->CreateTexture2D(&cd, nullptr, &rt.tex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = fmt; svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; svd.Texture2D.MipLevels = cd.MipLevels;
    if (FAILED(device->CreateShaderResourceView(rt.tex.Get(), &svd, &rt.srv))) return false;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = fmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateRenderTargetView(rt.tex.Get(), &rd, &rt.rtv))) return false;
    rt.w = w; rt.h = h; rt.fmt = fmt;
    return true;
}

bool DX11Backend::ensure_ext_tex(ID3D11Device* device, size_t idx, const std::filesystem::path& path) {
    if (m_ext_textures.size() <= idx) m_ext_textures.resize(idx + 1);
    auto& e = m_ext_textures[idx];
    // Hot-swap: if the requested path differs from what was loaded, reset the slot.
    if (e.tried && e.loaded_path != path) { e = {}; }
    if (e.tried) return e.srv != nullptr;
    e.tried = true;
    e.loaded_path = path;
    auto img = detail::load_image_rgba8(path.c_str());
    if (img.width == 0) {
        uevr::API::get()->log_warn("[fx/dx11] failed to load texture: %ls", path.c_str());
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = img.width; td.Height = img.height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = img.rgba8.data();
    sub.SysMemPitch = static_cast<UINT>(img.width) * 4;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &sub, &tex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = td.Format; svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; svd.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), &svd, &e.srv))) return false;
    return true;
}

bool DX11Backend::ensure_pass_cb(ID3D11Device* device, size_t idx, size_t cb_size) {
    if (cb_size == 0) return true;
    if (m_pass_cbs.size() <= idx) { m_pass_cbs.resize(idx + 1); m_pass_cb_sizes.resize(idx + 1, 0); }
    const size_t aligned = (cb_size + 15u) & ~size_t{15u};
    if (m_pass_cbs[idx] && m_pass_cb_sizes[idx] >= aligned) return true;
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = static_cast<UINT>(aligned);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_pass_cbs[idx].Reset();
    if (FAILED(device->CreateBuffer(&cbd, nullptr, &m_pass_cbs[idx]))) return false;
    m_pass_cb_sizes[idx] = aligned;
    return true;
}

void DX11Backend::execute(const std::vector<RTDesc>&                rt_descs,
                          const std::vector<std::filesystem::path>& ext_tex_paths,
                          const std::vector<PassDesc>&              passes,
                          int                                       snapshot_mips,
                          uint64_t                                  pass_mask) {
    static int s_last_exit = -2;
    auto exit_log = [&](int reason, const char* msg) {
        if (reason == s_last_exit) return;
        s_last_exit = reason;
        if (auto& a = uevr::API::get()) a->log_info("[fx/dx11] execute() exit: %s", msg);
    };

    if (passes.empty()) { exit_log(0, "no passes"); return; }

    auto& api = uevr::API::get();
    auto rd  = api->param()->renderer;
    if (rd->renderer_type != UEVR_RENDERER_D3D11 || rd->device == nullptr) {
        exit_log(1, "renderer not DX11 or device null"); return;
    }
    auto device = static_cast<ID3D11Device*>(rd->device);

    auto scene_rt_obj = uevr::API::StereoHook::get_scene_render_target();
    if (!scene_rt_obj) { exit_log(2, "scene_render_target object null"); return; }
    auto scene_native = static_cast<ID3D11Texture2D*>(scene_rt_obj->get_native_resource());
    if (!scene_native) { exit_log(3, "scene native resource null"); return; }
    D3D11_TEXTURE2D_DESC sd{}; scene_native->GetDesc(&sd);
    if (sd.Width == 0 || sd.Height == 0) { exit_log(4, "scene dims zero"); return; }

    if (!ensure_static_state(device)) { exit_log(5, "ensure_static_state failed"); return; }
    UINT snap_mips = static_cast<UINT>(std::max(1, snapshot_mips));
    auto* scene = ensure_scene_slot(device, scene_native, sd.Width, sd.Height, sd.Format, snap_mips);
    if (!scene) { exit_log(6, "ensure_scene_slot failed"); return; }

    exit_log(99, "running passes");
    detail::set_scene_size(scene->w, scene->h);
    detail::set_scene_rt_format(scene->fmt);
    detail::log_scene_rt_identity_change(scene_native, scene->fmt, scene->w, scene->h);

    // Snapshot the scene into the copy texture once. All passes that read INPUT_SCENE
    // sample from this snapshot; the live scene RT is reserved for final write-back.
    if (snap_mips > 1) {
        // Multi-mip snapshot: source has 1 mip, dest has N — use CopySubresourceRegion
        // for mip 0, then auto-generate mips 1..N-1 via GenerateMips().
        m_ctx->CopySubresourceRegion(scene->copy_tex.Get(), 0, 0, 0, 0, scene_native, 0, nullptr);
        m_ctx->GenerateMips(scene->copy_srv.Get());
    } else {
        m_ctx->CopyResource(scene->copy_tex.Get(), scene_native);
    }

    auto ctx = m_ctx.Get();
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);

    for (size_t pi = 0; pi < passes.size(); ++pi) {
        // Honor caller-supplied pass mask (used by plugins to skip passes
        // on the second native-stereo-fix dispatch — see EffectRuntime::execute(mask)).
        if (pi < 64 && (pass_mask & (uint64_t(1) << pi)) == 0) continue;
        const auto& p = passes[pi];

        // Resolve output RTV + dimensions + format.
        ID3D11RenderTargetView* out_rtv = nullptr;
        UINT out_w = 0, out_h = 0;
        DXGI_FORMAT out_fmt = DXGI_FORMAT_UNKNOWN;
        if (p.output == OUTPUT_SCENE) {
            out_rtv = scene->rtv.Get(); out_w = scene->w; out_h = scene->h; out_fmt = scene->fmt;
        } else if (p.output >= 0 && static_cast<size_t>(p.output) < rt_descs.size()) {
            if (!ensure_int_rt(device, scene, p.output, rt_descs[p.output])) {
                static int s_skip = -1; if (s_skip != (int)pi) { s_skip = (int)pi;
                    api->log_warn("[fx/dx11] pass %zu skipped: ensure_int_rt failed (rt %d)", pi, p.output); }
                continue;
            }
            auto& rt = int_rt_slot(scene, p.output, rt_descs[p.output]);
            out_rtv = rt.rtv.Get(); out_w = rt.w; out_h = rt.h; out_fmt = rt.fmt;
        } else {
            static int s_skip = -1; if (s_skip != (int)pi) { s_skip = (int)pi;
                api->log_warn("[fx/dx11] pass %zu skipped: invalid output id %d", pi, p.output); }
            continue;
        }
        if (p.output_format_override != DXGI_FORMAT_UNKNOWN) out_fmt = p.output_format_override;

        ID3D11PixelShader* ps = nullptr;
        const auto pass_cs = detail::classify_scene_rt_colorspace(scene->fmt);
        if (!ensure_pass_ps(device, p.ps_hlsl, out_fmt, pass_cs,
                             p.needs_scene_colorspace_decode, &ps) || ps == nullptr) {
            static int s_skip = -1; if (s_skip != (int)pi) { s_skip = (int)pi;
                api->log_warn("[fx/dx11] pass %zu skipped: PS compile/create failed (out_fmt=%s)",
                              pi, detail::dxgi_format_name(out_fmt)); }
            continue;
        }

        // Resolve up to 8 input SRVs.
        constexpr UINT k_max_srvs = 8;
        ID3D11ShaderResourceView* srvs[k_max_srvs] = {};
        const size_t n_in = std::min<size_t>(p.inputs.size(), k_max_srvs);
        for (size_t i = 0; i < n_in; ++i) {
            const int id = p.inputs[i];
            if (id == INPUT_SCENE) {
                srvs[i] = scene->copy_srv.Get();
            } else if (id >= EXTERNAL_TEX_BASE) {
                const size_t ext_idx = static_cast<size_t>(id - EXTERNAL_TEX_BASE);
                if (ext_idx < ext_tex_paths.size() && ensure_ext_tex(device, ext_idx, ext_tex_paths[ext_idx])) {
                    srvs[i] = m_ext_textures[ext_idx].srv.Get();
                }
            } else if (id >= 0 && static_cast<size_t>(id) < rt_descs.size()) {
                auto& store = rt_descs[id].shared_across_scene_slots ? m_shared_rts : scene->int_rts;
                if (static_cast<size_t>(id) < store.size()) srvs[i] = store[id].srv.Get();
            }
        }

        // Upload cbuffer.
        ID3D11Buffer* cb_to_bind = nullptr;
        if (p.cb_size > 0 && p.cb_data != nullptr && ensure_pass_cb(device, pi, p.cb_size)) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(m_pass_cbs[pi].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, p.cb_data, p.cb_size);
                ctx->Unmap(m_pass_cbs[pi].Get(), 0);
                cb_to_bind = m_pass_cbs[pi].Get();
            }
        }

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(out_w); vp.Height = static_cast<float>(out_h); vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &out_rtv, nullptr);
        ctx->PSSetShader(ps, nullptr, 0);
        if (cb_to_bind) ctx->PSSetConstantBuffers(0, 1, &cb_to_bind);
        ctx->PSSetShaderResources(0, static_cast<UINT>(n_in), srvs);
        ctx->PSSetSamplers(0, 1, m_sampler_linear.GetAddressOf());
        ctx->Draw(3, 0);

        // Unbind SRVs so a subsequent pass can write to one of these textures.
        ID3D11ShaderResourceView* null_srvs[k_max_srvs] = {};
        ctx->PSSetShaderResources(0, static_cast<UINT>(n_in), null_srvs);

        // If the output RT was declared with auto_generate_mips, regenerate now
        // so subsequent passes that sample at non-zero LOD see fresh data.
        if (p.output >= 0 && static_cast<size_t>(p.output) < rt_descs.size()) {
            const auto& d = rt_descs[p.output];
            if (d.auto_generate_mips && d.mip_levels > 1) {
                ctx->GenerateMips(int_rt_slot(scene, p.output, d).srv.Get());
            }
        }
    }
}

} // namespace

std::unique_ptr<EffectBackend> make_backend_d3d11() {
    return std::unique_ptr<EffectBackend>(new DX11Backend());
}

} // namespace uevr::fx
