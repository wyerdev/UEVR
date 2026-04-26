#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "uevr/Plugin.hpp"

// Shader asset path resolution for plugin DLLs.
//
// Two locations:
//   per-game: <persistent_dir>/data/plugins/shader_settings/<filename>
//   global:   <persistent_dir>/../UEVR/data/plugins/shader_assets/<filename>
//
// The global dir is populated by deploy.sh / install-plugins.bat from
// `examples/<plugin>/assets/`. Per-game files (dropped in by the user) shadow
// global ones with the same name. Layout mirrors PluginLoader.cpp's
// per-game-vs-global plugin DLL lookup; keep both sides in sync if it changes.

namespace uevr::assets::detail {
inline std::filesystem::path per_game_dir() {
    return uevr::API::get()->get_persistent_dir() / L"data" / L"plugins" / L"shader_settings";
}
inline std::filesystem::path global_dir() {
    return uevr::API::get()->get_persistent_dir() / L".." / L"UEVR"
        / L"data" / L"plugins" / L"shader_assets";
}
} // namespace uevr::assets::detail

// Resolve a shader asset filename to an absolute path. Returns empty path
// if neither the per-game nor the global location has the file.
inline std::filesystem::path resolve_shader_asset_path(const wchar_t* filename) {
    std::error_code ec;
    auto per_game = uevr::assets::detail::per_game_dir() / filename;
    if (std::filesystem::exists(per_game, ec)) return per_game;
    auto global = uevr::assets::detail::global_dir() / filename;
    if (std::filesystem::exists(global, ec)) return global;
    return {};
}

// One discovered shader asset (e.g. one LUT preset PNG).
struct ShaderAsset {
    std::wstring          filename;     // base filename, e.g. L"lut_warm.png"
    std::filesystem::path path;         // resolved absolute path actually loaded
};

// Enumerate all shader assets matching `extension` (e.g. L".png") with optional
// filename prefix (e.g. L"lut_"). Per-game files shadow same-named global files.
// Sorted alphabetically by filename so UI ordering is stable across launches.
inline std::vector<ShaderAsset> enumerate_shader_assets(const wchar_t* prefix,
                                                         const wchar_t* extension) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::map<std::wstring, fs::path> by_name;     // global first, per-game overwrites

    auto scan = [&](const fs::path& dir) {
        if (!fs::is_directory(dir, ec)) return;
        for (auto const& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const auto name = entry.path().filename().wstring();
            if (extension != nullptr && entry.path().extension().wstring() != extension) continue;
            if (prefix != nullptr && name.rfind(prefix, 0) != 0) continue;
            by_name[name] = entry.path();
        }
    };

    scan(uevr::assets::detail::global_dir());
    scan(uevr::assets::detail::per_game_dir());   // per-game shadows global

    std::vector<ShaderAsset> out;
    out.reserve(by_name.size());
    for (auto& kv : by_name) out.push_back({ kv.first, std::move(kv.second) });
    return out;
}
