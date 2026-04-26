// Phase A scaffold for the .uevrpreset settings system.
//
// Lives in its own translation unit (rather than inline in PluginLoader.cpp)
// so the diff against upstream praydog/UEVR for this feature is one tiny
// include + two function-pointer initializers + one tick call. Everything
// substantive happens here. See docs/preset-format-plan.md.
//
// No plugin uses this yet; the auto-saved file is written but not read.
// Legacy per-plugin *_settings.txt I/O is unaffected.

#pragma once

#include "uevr/API.h"

#include <filesystem>
#include <string>
#include <vector>

namespace uevr::settings_registry {

// C ABI entry points exported back to plugins via UEVR_PluginFunctions.
// Bound from PluginLoader.cpp's g_plugin_functions initializer.
void register_settings_serializer(const UEVR_SettingsSerializer* serializer, void* user_data);
void notify_settings_changed(void* user_data);

// Called once per frame from PluginLoader::on_present(). Cheap fast path
// (single relaxed atomic load) when nothing is dirty.
void tick_debounce();

// Path to <persistent>/data/plugins/shader_settings/auto.uevrpreset.
std::filesystem::path auto_preset_path();

// Force an immediate save (no debounce). Used on shutdown / preset overwrite.
bool flush_now();

// Save current settings of all registered serializers to a named preset file
// (header includes name="<display>"). Returns true on success.
bool save_named_preset(const std::filesystem::path& path, const std::string& display_name);

// Apply a preset file to all currently-registered serializers:
//   1. reset_to_defaults() on every serializer.
//   2. Parse the file. For each [section]/key=value, dispatch to the matching
//      serializer's deserialize(). Unknown sections / unknown keys are ignored.
// Sections in the file but not registered are silently skipped (forward compat).
// If `path` does not exist or cannot be parsed, all serializers still get
// reset_to_defaults() called.
void apply_preset_file(const std::filesystem::path& path);

// Convenience: apply auto_preset_path() — used at startup after all plugins'
// on_initialize() (and thus their register_settings_serializer calls) have
// finished, so settings persist across plugin reloads with the new format.
void apply_auto_preset();

// Drops the in-memory registry and clears any pending dirty flag. Called
// during attempt_unload_plugins() before plugins' DLLs are freed so we don't
// keep stale function pointers / user_data.
void clear();

// Has at least one plugin registered? Used to decide whether to flush on
// shutdown (avoid clobbering an existing auto.uevrpreset with an empty file
// when no plugin migrated yet).
bool empty();

// Live query: does the registered serializer for `section_name` currently
// report `enabled = 1`? Case-insensitive on section name. Returns false if
// no matching serializer is registered or no `enabled` key is emitted.
// Used by the host UI to color the sidebar without polling files on disk.
bool is_section_enabled(const std::string& section_name);

// Reset every registered plugin to its compiled-in defaults, then force
// `enabled = 0`. Marks the registry dirty so the next debounce tick
// persists the change to auto.uevrpreset. Used by the "Disable all" UI
// button (replaces the old "All Off" data preset). No-op if no plugins
// are registered.
void disable_all();

} // namespace uevr::settings_registry
