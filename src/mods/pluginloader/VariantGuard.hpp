// [fork-file] Ours — not in praydog/UEVR. Free to edit; no merge risk.
#pragma once

// [fork] variant-isolation: keeps every variant-aware decision out of the
// upstream PluginLoader. See include/uevr/Variant.hpp for the identity itself.
//
// Several independent builds of this fork install into the same
// %APPDATA%\UnrealVRMod tree. Plugin discovery is therefore qualified by the
// build variant, and each candidate DLL must additionally prove it was built
// for this variant before it is kept.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <Windows.h>

namespace uevr::variant_guard {
enum class plugin_directory_kind {
    ordinary,
    shared_shaders,
};

struct plugin_directory {
    std::filesystem::path path;
    plugin_directory_kind kind;
};

// Global directories are returned before per-game directories so the loader
// can keep the global copy when the same plugin exists in both locations.
std::vector<plugin_directory> plugin_directories(const std::filesystem::path& persistent_dir);

bool is_shader_plugin(const std::filesystem::path& path);

// Diagnostic: records which directories discovery actually looked at.
void log_scan(const std::filesystem::path& dir, bool is_directory);
} // namespace uevr::variant_guard
