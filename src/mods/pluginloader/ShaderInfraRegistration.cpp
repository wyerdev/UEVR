// [fork-file] Ours — not in praydog/UEVR. Free to edit; no merge risk.
#include "ShaderInfraRegistration.hpp"

#include <filesystem>
#include <vector>

#include <wrl.h>

#include <spdlog/spdlog.h>

#include "Framework.hpp"
#include "uevr/Variant.hpp"

namespace fs = std::filesystem;

namespace uevr::shader_infra {

// Single source of truth for the shader settings subdirectory (host side).
// Plugin DLLs mirror this via examples/renderlib/plugin_assets.hpp.
// If this path changes, update that header too.
static constexpr std::string_view SHADER_SETTINGS_DIR_NAME = "shader_settings";

void migrate_shader_settings_dir() {
    // Migration into this build's variant-scoped settings dir. Runs every
    // launch — handles partial migration (e.g. interrupted first run).
    //
    // Two legacy layouts are absorbed, in order:
    //   1. data/plugins/*_settings.txt              (pre shader_settings/)
    //   2. data/plugins/shader_settings/*           (pre variant scoping)
    // Both are unqualified, so they cannot be attributed to a variant. They are
    // moved, not copied: the first variant launched after the upgrade adopts
    // them, and later variants simply start from defaults.
    const auto plugins_dir = Framework::get_persistent_dir() / "data" / "plugins";
    const auto new_dir = plugins_dir / UEVR_VARIANT_ID / SHADER_SETTINGS_DIR_NAME;
    if (!fs::exists(plugins_dir)) return;

    std::vector<fs::path> to_migrate;

    std::error_code scan_ec;
    for (const auto& entry : fs::directory_iterator(plugins_dir, scan_ec)) {
        if (!entry.is_regular_file()) continue;
        auto fname = entry.path().filename().string();
        if (fname.size() > 13 && fname.substr(fname.size() - 13) == "_settings.txt") {
            to_migrate.push_back(entry.path());
        }
    }

    const auto legacy_shader_settings_dir = plugins_dir / SHADER_SETTINGS_DIR_NAME;
    if (fs::exists(legacy_shader_settings_dir)) {
        for (const auto& entry : fs::directory_iterator(legacy_shader_settings_dir, scan_ec)) {
            if (entry.is_regular_file()) {
                to_migrate.push_back(entry.path());
            }
        }
    }

    if (to_migrate.empty()) return;

    spdlog::info("[PluginLoader] Migrating {} legacy shader settings file(s) into variant '{}'",
        to_migrate.size(), UEVR_VARIANT_ID);

    fs::create_directories(new_dir);
    for (const auto& src : to_migrate) {
        const auto dest = new_dir / src.filename();
        if (fs::exists(dest)) {
            // Already migrated previously; remove orphan in old location.
            std::error_code ec; fs::remove(src, ec);
            if (!ec) spdlog::info("[PluginLoader] Removed orphan shader settings: {}", src.filename().string());
        } else {
            std::error_code ec; fs::rename(src, dest, ec);
            if (!ec) spdlog::info("[PluginLoader] Migrated shader settings: {}", src.filename().string());
        }
    }

    // Drop the now-empty unqualified dir so it stops looking authoritative.
    std::error_code ec;
    fs::remove(legacy_shader_settings_dir, ec);
}

// D3D11 pipeline state objects — created once, bound before every plugin
// dispatch so plugins inherit a known-good baseline regardless of what
// state the UE renderer left on the immediate context.
static Microsoft::WRL::ComPtr<ID3D11RasterizerState>   s_plugin_rasterizer;
static Microsoft::WRL::ComPtr<ID3D11DepthStencilState> s_plugin_depth_stencil;
static Microsoft::WRL::ComPtr<ID3D11BlendState>        s_plugin_blend;
static bool s_plugin_dx11_state_ready = false;

bool dx11_ensure_state(ID3D11Device* device) {
    if (s_plugin_dx11_state_ready) return true;
    if (!device) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    if (FAILED(device->CreateRasterizerState(&rd, &s_plugin_rasterizer))) return false;

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&dd, &s_plugin_depth_stencil))) return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &s_plugin_blend))) return false;

    s_plugin_dx11_state_ready = true;
    return true;
}

void dx11_bind_state(ID3D11DeviceContext* ctx) {
    ctx->RSSetState(s_plugin_rasterizer.Get());
    ctx->OMSetDepthStencilState(s_plugin_depth_stencil.Get(), 0);
    float blend_factor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(s_plugin_blend.Get(), blend_factor, 0xFFFFFFFF);
}

void dx11_release_state() {
    // Release framework-managed D3D11 state objects — they hold refs to the old device.
    s_plugin_rasterizer.Reset();
    s_plugin_depth_stencil.Reset();
    s_plugin_blend.Reset();
    s_plugin_dx11_state_ready = false;
}

// Isolated into its own function because __try/__except cannot coexist with
// C++ objects that have destructors (like std::shared_lock) in the same scope.
bool dx12_invoke_pre_render_seh(UEVR_OnPreRenderVRFrameworkDX12Cb cb) {
    __try {
        cb();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void dispatch_pre_render_dx11(const std::vector<UEVR_OnPreRenderVRFrameworkDX11Cb>& cbs) {
    if (auto& hook = g_framework->get_d3d11_hook(); hook != nullptr) {
        if (auto* device = hook->get_device(); device != nullptr) {
            if (dx11_ensure_state(device)) {
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
                device->GetImmediateContext(&ctx);
                if (ctx) {
                    dx11_bind_state(ctx.Get());
                }
            }
        }
    }

    for (auto&& cb : cbs) {
        try {
            cb();
        } catch (...) {
            spdlog::error("[APIProxy] Exception occurred in on_pre_render_vr_framework_dx11 callback; one of the plugins has an error.");
            continue;
        }
    }
}

void dispatch_pre_render_dx12(const std::vector<UEVR_OnPreRenderVRFrameworkDX12Cb>& cbs) {
    for (auto&& cb : cbs) {
        if (!dx12_invoke_pre_render_seh(cb)) {
            spdlog::error("[APIProxy] Access violation in on_pre_render_vr_framework_dx12 callback; one of the plugins has an error.");
            continue;
        }
    }
}

} // namespace uevr::shader_infra
