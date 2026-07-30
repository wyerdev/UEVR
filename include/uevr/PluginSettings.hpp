// Plugin settings serializer — C++ adapter over the C ABI defined in API.h.
//
// A plugin author subclasses Serializable, fills in the five virtual methods,
// and calls register_with_host(*this, param) once during uevr_plugin_initialize.
// The host then drives save/load of the plugin's settings into the shared
// .uevrpreset file. Plugins should call notify_changed() from their UI
// handlers when a setting changes; the host debounces and writes once.
//
// Header-only by design — every plugin DLL gets its own copy of the
// trampolines, no shared symbol problems across DLL boundaries.

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

extern "C" {
    #include "API.h"
}

namespace uevr::settings {

class Serializable {
public:
    virtual ~Serializable() = default;

    // Stable preset section name (e.g. "CAS"). Decoupled from DLL filename;
    // renaming the DLL must NOT invalidate existing presets. Must be unique
    // across all loaded plugins — the host rejects duplicates with an error.
    virtual std::string preset_section_name() const = 0;

    // Render order in the post-process pipeline. Sparse integer (100, 200,
    // 300...) so a new plugin can be slotted between two existing ones
    // without renumbering. Lower runs earlier.
    virtual int render_order() const = 0;

    // Serialize current settings as (key, value) string pairs. Keys must be
    // stable across plugin versions (renaming a key = breaking change).
    virtual std::vector<std::pair<std::string, std::string>> serialize_settings() const = 0;

    // Apply a key/value map to settings. Missing keys keep their current
    // value. Unknown keys must be ignored (forward-compat with older builds
    // that read presets containing keys this build doesn't know about yet).
    virtual void deserialize_settings(const std::map<std::string, std::string>& kv) = 0;

    // Reset all settings to their defaults. Host calls this BEFORE replaying
    // a preset so plugins not in the preset end up at defaults.
    virtual void reset_to_defaults() = 0;
};

namespace detail {

// Trampolines — bridge the C function pointers in UEVR_SettingsSerializer to
// the C++ virtual methods on Serializable. Static storage for the returned
// section name string is per-plugin per-Serializable instance, owned via a
// thread-local cache keyed on the user_data pointer.

inline std::string& section_name_cache(Serializable* self) {
    // One cache slot per Serializable*. Plugins typically have one
    // Serializable instance, so the map stays tiny. Keeps the returned
    // const char* alive between successive get_section_name calls without
    // requiring the plugin to manage storage itself.
    static thread_local std::map<Serializable*, std::string> cache;
    return cache[self];
}

inline const char* trampoline_get_section_name(void* user_data) {
    auto* self = static_cast<Serializable*>(user_data);
    auto& slot = section_name_cache(self);
    slot = self->preset_section_name();
    return slot.c_str();
}

inline int trampoline_get_render_order(void* user_data) {
    return static_cast<Serializable*>(user_data)->render_order();
}

inline void trampoline_serialize(void* user_data, UEVR_SettingsSerializeEmitFn emit, void* ctx) {
    auto* self = static_cast<Serializable*>(user_data);
    for (const auto& [k, v] : self->serialize_settings()) {
        emit(k.c_str(), v.c_str(), ctx);
    }
}

inline void trampoline_deserialize(void* user_data, const char* key, const char* value) {
    if (key == nullptr) return;
    std::map<std::string, std::string> kv;
    kv.emplace(key, value != nullptr ? value : "");
    static_cast<Serializable*>(user_data)->deserialize_settings(kv);
}

inline void trampoline_reset_to_defaults(void* user_data) {
    static_cast<Serializable*>(user_data)->reset_to_defaults();
}

inline const UEVR_SettingsSerializer& vtable_for() {
    static const UEVR_SettingsSerializer s_vtable{
        &trampoline_get_section_name,
        &trampoline_get_render_order,
        &trampoline_serialize,
        &trampoline_deserialize,
        &trampoline_reset_to_defaults,
    };
    return s_vtable;
}

} // namespace detail

// Register `s` with the host. Call once from uevr_plugin_initialize. Returns
// true on success. The Serializable instance must outlive the plugin (a
// member of the plugin singleton is fine).
inline bool register_with_host(Serializable& s, const UEVR_PluginInitializeParam* param) {
    if (param == nullptr || param->functions == nullptr) return false;
    auto* fn = param->functions->register_settings_serializer;
    if (fn == nullptr) return false; // older host without preset support
    fn(&detail::vtable_for(), static_cast<void*>(&s));
    return true;
}

// Notify the host that `s`'s settings changed. Cheap to call repeatedly —
// the host debounces.
inline void notify_changed(Serializable& s, const UEVR_PluginInitializeParam* param) {
    if (param == nullptr || param->functions == nullptr) return;
    auto* fn = param->functions->notify_settings_changed;
    if (fn == nullptr) return;
    fn(static_cast<void*>(&s));
}

} // namespace uevr::settings
