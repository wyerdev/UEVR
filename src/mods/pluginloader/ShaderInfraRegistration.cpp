// [fork-file] Ours — not in praydog/UEVR. Free to edit; no merge risk.
#include "ShaderInfraRegistration.hpp"

#include <filesystem>
#include <string_view>
#include <vector>

#include <wrl.h>

#include <spdlog/spdlog.h>

#include "Framework.hpp"

namespace fs = std::filesystem;

namespace uevr::shader_infra {

// Single source of truth for the shader settings subdirectory (host side).
// Plugin DLLs mirror this via examples/renderlib/plugin_assets.hpp.
// If this path changes, update that header too.
static constexpr std::string_view SHADER_SETTINGS_DIR_NAME = "shader_settings";

// Build-line variant IDs whose data was scoped under `data/plugins/<variant>/`
// before data became shared. Only plugin DLLs are variant-scoped now, so these
// dirs are folded back. Add a new build line's ID here when it is created.
static constexpr std::string_view LEGACY_VARIANT_DIR_NAMES[] = {
    "reshade",
    "reshade-afw",
    "reshade-afw-joeyhodge",
};

// Move `src` to `dest`, merging directories recursively. An existing
// destination file always wins and the source is discarded, so the first build
// line launched after the upgrade adopts the data and later ones no-op.
static void merge_move(const fs::path& src, const fs::path& dest) {
    std::error_code ec;

    if (fs::is_directory(src, ec)) {
        fs::create_directories(dest, ec);
        std::error_code scan_ec;
        for (const auto& entry : fs::directory_iterator(src, scan_ec)) {
            merge_move(entry.path(), dest / entry.path().filename());
        }
        // Only succeeds once the dir is empty, which is what we want.
        fs::remove(src, ec);
        return;
    }

    if (fs::exists(dest, ec)) {
        fs::remove(src, ec);
        return;
    }

    fs::rename(src, dest, ec);
    if (ec) spdlog::warn("[PluginLoader] Could not migrate {}: {}", src.string(), ec.message());
}

// Fold `<plugins_dir>/<variant>/**` back into `<plugins_dir>/**`.
static void fold_variant_dirs(const fs::path& plugins_dir) {
    std::error_code ec;
    if (!fs::exists(plugins_dir, ec)) return;

    for (const auto& name : LEGACY_VARIANT_DIR_NAMES) {
        const auto variant_dir = plugins_dir / name;
        if (!fs::is_directory(variant_dir, ec)) continue;

        spdlog::info("[PluginLoader] Folding legacy variant data dir {} into shared layout",
            variant_dir.string());
        merge_move(variant_dir, plugins_dir);
    }
}

void migrate_legacy_data_dirs() {
    // Runs every launch — handles partial migration (e.g. interrupted first run).
    const auto plugins_dir = Framework::get_persistent_dir() / "data" / "plugins";

    std::error_code ec;
    if (fs::exists(plugins_dir, ec)) {
        // Pre-`shader_settings/` layout: loose `*_settings.txt` in data/plugins.
        const auto shader_settings_dir = plugins_dir / SHADER_SETTINGS_DIR_NAME;
        std::vector<fs::path> loose_settings;

        std::error_code scan_ec;
        for (const auto& entry : fs::directory_iterator(plugins_dir, scan_ec)) {
            if (!entry.is_regular_file()) continue;
            auto fname = entry.path().filename().string();
            if (fname.size() > 13 && fname.substr(fname.size() - 13) == "_settings.txt") {
                loose_settings.push_back(entry.path());
            }
        }

        if (!loose_settings.empty()) {
            spdlog::info("[PluginLoader] Migrating {} loose shader settings file(s) into {}",
                loose_settings.size(), SHADER_SETTINGS_DIR_NAME);
            fs::create_directories(shader_settings_dir, ec);
            for (const auto& src : loose_settings) {
                merge_move(src, shader_settings_dir / src.filename());
            }
        }
    }

    fold_variant_dirs(plugins_dir);

    // Global (cross-game) presets and shader assets live beside the per-game
    // dir, under the shared UEVR folder. Mirrors PresetSystem.cpp.
    fold_variant_dirs(Framework::get_persistent_dir() / ".." / "UEVR" / "data" / "plugins");
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
