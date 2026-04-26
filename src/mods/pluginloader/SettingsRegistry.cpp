// Phase A scaffold for the .uevrpreset settings system. See SettingsRegistry.hpp.

#include "SettingsRegistry.hpp"
#include "PresetIni.hpp"

#include "Framework.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace uevr::settings_registry {

// Single source of truth for the shader settings subdirectory must match
// what PluginLoader.cpp uses (SHADER_SETTINGS_DIR_NAME). Duplicated as a
// constant here so this file doesn't have to include PluginLoader.cpp's
// internals — co-location risk acknowledged: if the host renames the
// directory, both copies must change. See user-memory co-location rule.
static constexpr const char* kShaderSettingsDirName = "shader_settings";

namespace {

struct Registered {
    UEVR_SettingsSerializer vtable; // shallow copy of plugin-supplied struct
    void* user_data;
    int   render_order;
};

std::mutex s_mtx;
// Keyed on section name so duplicate registrations are loud and visible.
std::map<std::string, Registered> s_serializers;
// Reverse lookup so notify_settings_changed(user_data) can find the section
// without re-querying the plugin (calling back into plugin code while
// holding the mutex would be a reentrancy hazard).
std::map<void*, std::string> s_user_data_to_section;
std::atomic<bool> s_dirty{false};
std::atomic<long long> s_dirty_at_ms{0}; // steady_clock ms since epoch

constexpr long long kDebounceMs = 500;

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path auto_preset_path_impl() {
    return Framework::get_persistent_dir() / "data" / "plugins"
        / kShaderSettingsDirName / "auto.uevrpreset";
}

bool flush_to_disk_with_header(const std::filesystem::path& target,
                                const std::vector<std::pair<std::string, std::string>>& header) {
    // Snapshot under the lock so plugin callbacks run lock-free.
    std::vector<std::pair<std::string, Registered>> snapshot;
    {
        std::lock_guard lock{s_mtx};
        snapshot.reserve(s_serializers.size());
        for (const auto& [k, v] : s_serializers) snapshot.emplace_back(k, v);
    }

    std::sort(snapshot.begin(), snapshot.end(),
        [](const auto& a, const auto& b) {
            if (a.second.render_order != b.second.render_order) {
                return a.second.render_order < b.second.render_order;
            }
            return a.first < b.first;
        });

    using namespace uevr::preset_ini;
    std::vector<SectionEmit> sections;
    sections.reserve(snapshot.size());

    struct Ctx { Section* data; std::vector<std::string>* order; };

    for (const auto& [name, reg] : snapshot) {
        SectionEmit se;
        se.name = name;
        Ctx ctx{ &se.data, &se.key_order };
        if (reg.vtable.serialize == nullptr) {
            sections.push_back(std::move(se));
            continue;
        }
        try {
            reg.vtable.serialize(reg.user_data,
                [](const char* k, const char* v, void* c) {
                    if (k == nullptr) return;
                    auto* ctx = static_cast<Ctx*>(c);
                    std::string key = k;
                    std::string val = v != nullptr ? v : "";
                    if (ctx->data->find(key) == ctx->data->end()) {
                        ctx->order->push_back(key);
                    }
                    (*ctx->data)[key] = std::move(val);
                }, &ctx);
        } catch (...) {
            spdlog::error("[PluginLoader] Plugin '{}' serialize() threw — skipping section", name);
            continue;
        }
        sections.push_back(std::move(se));
    }

    if (!uevr::preset_ini::write_file(target, header, sections)) {
        spdlog::warn("[PluginLoader] Failed to write preset file: {}", target.string());
        return false;
    }
    return true;
}

void flush_to_disk() {
    std::vector<std::pair<std::string, std::string>> header{
        {"format", "1"},
        {"name",   "auto"},
    };
    flush_to_disk_with_header(auto_preset_path_impl(), header);
}

} // namespace

void register_settings_serializer(const UEVR_SettingsSerializer* serializer, void* user_data) {
    if (serializer == nullptr || serializer->get_section_name == nullptr) {
        spdlog::error("[Plugin] register_settings_serializer: null serializer or missing get_section_name");
        return;
    }

    const char* raw = nullptr;
    try {
        raw = serializer->get_section_name(user_data);
    } catch (...) {
        spdlog::error("[Plugin] register_settings_serializer: get_section_name() threw");
        return;
    }
    if (raw == nullptr || raw[0] == '\0') {
        spdlog::error("[Plugin] register_settings_serializer: empty section name");
        return;
    }
    std::string section{raw};

    int order = 0;
    if (serializer->get_render_order != nullptr) {
        try { order = serializer->get_render_order(user_data); } catch (...) { order = 0; }
    }

    std::lock_guard lock{s_mtx};
    auto [it, inserted] = s_serializers.try_emplace(
        section, Registered{*serializer, user_data, order});
    if (!inserted) {
        spdlog::error("[Plugin] register_settings_serializer: duplicate section '{}' — second registration rejected",
                      section);
        return;
    }
    s_user_data_to_section[user_data] = section;
    spdlog::info("[Plugin] Registered settings serializer for section '{}' (order={})", section, order);
}

void notify_settings_changed(void* user_data) {
    {
        std::lock_guard lock{s_mtx};
        if (s_user_data_to_section.find(user_data) == s_user_data_to_section.end()) {
            // Unknown plugin — silently ignore. Could be a Phase E hot-reload
            // edge case where the plugin was unregistered concurrently.
            return;
        }
    }
    s_dirty_at_ms.store(now_ms(), std::memory_order_relaxed);
    s_dirty.store(true, std::memory_order_relaxed);
}

void tick_debounce() {
    if (!s_dirty.load(std::memory_order_relaxed)) return;
    const auto dirty_ms = s_dirty_at_ms.load(std::memory_order_relaxed);
    if (now_ms() - dirty_ms < kDebounceMs) return;
    // Clear before writing so a notification arriving mid-write re-arms us.
    s_dirty.store(false, std::memory_order_relaxed);
    flush_to_disk();
}

std::filesystem::path auto_preset_path() {
    return auto_preset_path_impl();
}

bool flush_now() {
    s_dirty.store(false, std::memory_order_relaxed);
    return flush_to_disk_with_header(auto_preset_path_impl(),
        { {"format", "1"}, {"name", "auto"} });
}

bool save_named_preset(const std::filesystem::path& path, const std::string& display_name) {
    return flush_to_disk_with_header(path,
        { {"format", "1"}, {"name", display_name} });
}

void apply_preset_file(const std::filesystem::path& path) {
    // Snapshot the registry so plugin callbacks run without our mutex held.
    std::vector<std::pair<std::string, Registered>> snapshot;
    {
        std::lock_guard lock{s_mtx};
        snapshot.reserve(s_serializers.size());
        for (const auto& [k, v] : s_serializers) snapshot.emplace_back(k, v);
    }

    // 1. reset_to_defaults() on every serializer so plugins not present in
    //    the file (or with missing keys) end up at defaults.
    for (const auto& [name, reg] : snapshot) {
        if (reg.vtable.reset_to_defaults == nullptr) continue;
        try { reg.vtable.reset_to_defaults(reg.user_data); }
        catch (...) {
            spdlog::error("[PluginLoader] Plugin '{}' reset_to_defaults() threw", name);
        }
    }

    // 2. Parse the file (empty Document if missing) and dispatch matching
    //    sections to their serializers.
    auto doc = uevr::preset_ini::parse_file(path);
    if (doc.sections.empty()) {
        spdlog::info("[PluginLoader] Preset '{}' is empty or missing — defaults applied", path.string());
        return;
    }

    // Build name->Registered lookup once.
    std::map<std::string, const Registered*> by_name;
    for (const auto& [name, reg] : snapshot) by_name[name] = &reg;

    int applied_sections = 0;
    int applied_keys = 0;
    for (const auto& [section_name, kvs] : doc.sections) {
        auto it = by_name.find(section_name);
        if (it == by_name.end()) continue; // unknown plugin — silently skip
        const auto* reg = it->second;
        if (reg->vtable.deserialize == nullptr) continue;
        ++applied_sections;
        for (const auto& [k, v] : kvs) {
            try {
                reg->vtable.deserialize(reg->user_data, k.c_str(), v.c_str());
                ++applied_keys;
            } catch (...) {
                spdlog::error("[PluginLoader] Plugin '{}' deserialize('{}') threw — skipping",
                              section_name, k);
            }
        }
    }
    spdlog::info("[PluginLoader] Applied preset '{}': {} section(s), {} key(s)",
                 path.string(), applied_sections, applied_keys);
}

void apply_auto_preset() {
    apply_preset_file(auto_preset_path_impl());
}

void clear() {
    std::lock_guard lock{s_mtx};
    s_serializers.clear();
    s_user_data_to_section.clear();
    s_dirty.store(false, std::memory_order_relaxed);
}

bool empty() {
    std::lock_guard lock{s_mtx};
    return s_serializers.empty();
}

bool is_section_enabled(const std::string& section_name) {
    // Case-insensitive lookup against registered section names.
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    };

    Registered reg{};
    bool found = false;
    {
        std::lock_guard lock{s_mtx};
        for (const auto& [name, r] : s_serializers) {
            if (ieq(name, section_name)) { reg = r; found = true; break; }
        }
    }
    if (!found || reg.vtable.serialize == nullptr) return false;

    // Walk the serializer once, capturing only the `enabled` key.
    struct Ctx { bool found = false; bool enabled = false; };
    Ctx ctx;
    try {
        reg.vtable.serialize(reg.user_data,
            [](const char* k, const char* v, void* c) {
                if (k == nullptr) return;
                auto* cx = static_cast<Ctx*>(c);
                if (cx->found) return;
                if (std::string_view{k} == "enabled") {
                    cx->found = true;
                    cx->enabled = (v != nullptr && v[0] != '0');
                }
            }, &ctx);
    } catch (...) {
        return false;
    }
    return ctx.enabled;
}

void disable_all() {
    // Reset every registered plugin to its compiled-in defaults, then force
    // enabled=0. After this, re-enabling any plugin starts from a clean
    // default state (matches user expectation: "off" means a fresh slate).
    std::vector<Registered> snapshot;
    {
        std::lock_guard lock{s_mtx};
        snapshot.reserve(s_serializers.size());
        for (const auto& [_, r] : s_serializers) snapshot.push_back(r);
    }
    bool any = false;
    for (const auto& reg : snapshot) {
        try {
            if (reg.vtable.reset_to_defaults != nullptr) {
                reg.vtable.reset_to_defaults(reg.user_data);
            }
            if (reg.vtable.deserialize != nullptr) {
                reg.vtable.deserialize(reg.user_data, "enabled", "0");
            }
            any = true;
        } catch (...) {
            // Plugin threw — skip; other plugins still get reset.
        }
    }
    if (any) {
        s_dirty.store(true, std::memory_order_relaxed);
        s_dirty_at_ms.store(now_ms(), std::memory_order_relaxed);
    }
}

} // namespace uevr::settings_registry
