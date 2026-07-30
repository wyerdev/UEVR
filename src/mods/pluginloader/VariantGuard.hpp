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

#include <Windows.h>

namespace uevr::variant_guard {
// Variant-scoped plugin directory beneath `base` (which is a UEVR persistent
// dir, or the shared UEVR dir for global plugins).
std::filesystem::path plugin_dir(const std::filesystem::path& base);

// Diagnostic: records which directories discovery actually looked at.
void log_scan(const std::filesystem::path& dir, bool is_directory);

// Identity handshake for an already-loaded plugin module. On rejection this
// logs the reason, records it in `load_errors` under the plugin's stem, frees
// the module and returns false. The caller simply skips the plugin.
bool accept_plugin(HMODULE module, const std::filesystem::path& path,
    std::map<std::string, std::string>& load_errors);
} // namespace uevr::variant_guard
