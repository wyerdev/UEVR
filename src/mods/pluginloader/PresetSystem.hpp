// Plugin preset save/load/list/delete UI and IO. Owns the in-memory
// "active preset" tracking that survives across plugin reloads, and the
// `Plugin Presets` collapsing-header UI rendered inside PluginLoader's
// settings page. Extracted from PluginLoader.cpp so the upstream-owned
// file's fork delta shrinks to a 1-line `draw_preset_ui(...)` call.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace uevr::preset_system {

// Per-PluginLoader-instance UI state (text input buffer + last status line).
// Held as a member on PluginLoader so it persists across `on_draw_ui()` calls.
struct UIState {
    char        name_buf[128]{};
    std::string status{};
};

std::filesystem::path get_local_presets_dir();
std::filesystem::path get_global_presets_dir();
std::filesystem::path get_shipping_presets_dir();

// Returns names INCLUDING the .uevrpreset extension so callers can build
// a full path via `dir / name`.
std::vector<std::string> list_presets(const std::filesystem::path& dir);

bool save_preset(const std::filesystem::path& presets_dir,
                 const std::string& name,
                 std::string& status_out);

// Triggers the unload→copy→reload sequence required to apply a preset to
// already-running plugin DLLs. The two callbacks are invoked synchronously
// in the order: attempt_unload(); copy auto.uevrpreset; reload();
bool load_preset(const std::filesystem::path& preset_path,
                 std::string& status_out,
                 const std::function<void()>& attempt_unload,
                 const std::function<void()>& reload);

bool delete_preset(const std::filesystem::path& preset_path,
                   std::string& status_out);

// Renders the `Plugin Presets` collapsing-header section.
void draw_preset_ui(UIState& state,
                    const std::function<void()>& attempt_unload,
                    const std::function<void()>& reload);

} // namespace uevr::preset_system
