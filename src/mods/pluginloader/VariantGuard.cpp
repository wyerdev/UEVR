// [fork-file] Ours — not in praydog/UEVR. Free to edit; no merge risk.
#include "VariantGuard.hpp"

#include <string_view>

#include <spdlog/spdlog.h>

namespace uevr::variant_guard {

std::vector<plugin_directory> plugin_directories(const std::filesystem::path& persistent_dir) {
    const auto global_root = persistent_dir / ".." / "UEVR";

    return {
        {global_root / "plugins", plugin_directory_kind::ordinary},
        {global_root / "plugins" / "shaders", plugin_directory_kind::shared_shaders},
        {persistent_dir / "plugins", plugin_directory_kind::ordinary},
        {persistent_dir / "plugins" / "shaders", plugin_directory_kind::shared_shaders},
    };
}

bool is_shader_plugin(const std::filesystem::path& path) {
    return std::string_view{path.filename().string()}.ends_with("Shader.dll");
}

void log_scan(const std::filesystem::path& dir, bool is_directory) {
    if (is_directory) {
        spdlog::info("[PluginLoader] Scanning {}", dir.string());
    } else {
        spdlog::info("[PluginLoader] Skipping {} (not a directory)", dir.string());
    }
}

} // namespace uevr::variant_guard
