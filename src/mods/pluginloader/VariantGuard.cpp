// [fork-file] Ours — not in praydog/UEVR. Free to edit; no merge risk.
#include "VariantGuard.hpp"

#include <string_view>

#include <spdlog/spdlog.h>

#include "uevr/Variant.hpp"

namespace uevr::variant_guard {

std::filesystem::path plugin_dir(const std::filesystem::path& base) {
    return base / "plugins" / UEVR_VARIANT_ID;
}

void log_scan(const std::filesystem::path& dir, bool is_directory) {
    if (is_directory) {
        spdlog::info("[PluginLoader] Scanning {} (variant '{}')", dir.string(), UEVR_VARIANT_ID);
    } else {
        spdlog::info("[PluginLoader] Skipping {} (not a directory)", dir.string());
    }
}

bool accept_plugin(HMODULE module, const std::filesystem::path& path,
    std::map<std::string, std::string>& load_errors)
{
    // A DLL sitting in our variant directory still has to say it was built for
    // this variant, otherwise it is a stale copy or a manual mistake. A
    // pre-variant plugin has no such export and is rejected the same way.
    using variant_id_fn = const char* (*)();
    const auto variant_id_export = (variant_id_fn)GetProcAddress(module, UEVR_PLUGIN_VARIANT_EXPORT);

    if (variant_id_export == nullptr) {
        spdlog::error("[PluginLoader] Rejected {}: no '{}' export (foreign or pre-variant plugin)",
            path.string(), UEVR_PLUGIN_VARIANT_EXPORT);
        load_errors.emplace(path.stem().string(), "Not built for variant '" UEVR_VARIANT_ID "'");
        FreeLibrary(module);
        return false;
    }

    const auto plugin_variant = variant_id_export();

    if (plugin_variant == nullptr || std::string_view{plugin_variant} != UEVR_VARIANT_ID) {
        spdlog::error("[PluginLoader] Rejected {}: variant '{}' does not match backend variant '{}'",
            path.string(), plugin_variant != nullptr ? plugin_variant : "<null>", UEVR_VARIANT_ID);
        load_errors.emplace(path.stem().string(), "Variant mismatch");
        FreeLibrary(module);
        return false;
    }

    return true;
}

} // namespace uevr::variant_guard
