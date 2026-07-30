#include "PresetSystem.hpp"

#include <fstream>

#include <ShlObj.h>
#include <Windows.h>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include "Framework.hpp"
#include "SettingsRegistry.hpp"
#include "uevr/Variant.hpp"

namespace uevr::preset_system {

// Mirrors PluginLoader.cpp's SHADER_SETTINGS_DIR_NAME and
// SettingsRegistry.cpp's kShaderSettingsDirName. Single source of truth would
// require a new public header just for this constant; duplication noted.
static constexpr const char* kShaderSettingsDirName = "shader_settings";

// Active preset tracking — file-scope so it survives across plugin reloads
// without leaking into PluginLoader's class layout.
static std::string s_active_preset_name{};
static std::filesystem::path s_active_preset_dir{};
static bool s_active_preset_is_builtin = false;
static bool s_active_preset_loaded_from_disk = false;

static std::filesystem::path get_active_preset_file() {
    return Framework::get_persistent_dir() / "data" / "plugins" / UEVR_VARIANT_ID / "active_preset.txt";
}

static void save_active_preset_to_disk() {
    try {
        std::ofstream f(get_active_preset_file());
        if (!f.is_open()) return;
        f << s_active_preset_name << "\n";
        f << s_active_preset_dir.string() << "\n";
        f << (s_active_preset_is_builtin ? 1 : 0) << "\n";
    } catch (...) {}
}

static void restore_active_preset_from_disk() {
    if (s_active_preset_loaded_from_disk) return;
    s_active_preset_loaded_from_disk = true;
    try {
        std::ifstream f(get_active_preset_file());
        if (!f.is_open()) return;
        std::string name, dir_str, builtin_str;
        if (!std::getline(f, name) || name.empty()) return;
        if (!std::getline(f, dir_str)) return;
        if (!std::getline(f, builtin_str)) return;
        auto dir = std::filesystem::path(dir_str);
        // Verify the preset still exists on disk
        if (std::filesystem::exists(dir / (name + ".uevrpreset"))) {
            s_active_preset_name = name;
            s_active_preset_dir = dir;
            s_active_preset_is_builtin = (builtin_str == "1");
        }
    } catch (...) {}
}

std::filesystem::path get_local_presets_dir() {
    auto dir = Framework::get_persistent_dir() / "data" / "plugins" / UEVR_VARIANT_ID / "presets";
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path get_global_presets_dir() {
    wchar_t app_data_path[MAX_PATH]{};
    SHGetSpecialFolderPathW(0, app_data_path, CSIDL_APPDATA, false);
    auto dir = std::filesystem::path(app_data_path) / "UnrealVRMod" / "UEVR" / "data" / "plugins" / UEVR_VARIANT_ID / "presets";
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path get_shipping_presets_dir() {
    wchar_t app_data_path[MAX_PATH]{};
    SHGetSpecialFolderPathW(0, app_data_path, CSIDL_APPDATA, false);
    auto dir = std::filesystem::path(app_data_path) / "UnrealVRMod" / "UEVR" / "data" / "plugins" / UEVR_VARIANT_ID / "shipping_presets";
    // Don't create — this dir is managed by the release package, not created at runtime
    return dir;
}

std::vector<std::string> list_presets(const std::filesystem::path& dir) {
    std::vector<std::string> result;
    try {
        if (!std::filesystem::exists(dir)) return result;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const auto& p = entry.path();
            if (p.extension() != ".uevrpreset") continue;
            result.push_back(p.filename().string());
        }
        std::sort(result.begin(), result.end());
    } catch (...) {}
    return result;
}

bool save_preset(const std::filesystem::path& presets_dir, const std::string& name, std::string& status_out) {
    try {
        std::filesystem::create_directories(presets_dir);
        // Append the canonical extension if the user-supplied name lacks it.
        std::filesystem::path file_name = name;
        if (file_name.extension() != ".uevrpreset") {
            file_name = std::filesystem::path(name + ".uevrpreset");
        }
        const auto preset_path = presets_dir / file_name;

        std::string display = file_name.stem().string();
        if (!uevr::settings_registry::save_named_preset(preset_path, display)) {
            status_out = "Save failed: write error";
            return false;
        }

        status_out = "Saved \"" + display + "\"";
        spdlog::info("[PluginLoader] Saved preset to {}", preset_path.string());
        return true;
    } catch (const std::exception& e) {
        status_out = std::string("Save failed: ") + e.what();
        spdlog::error("[PluginLoader] Failed to save preset '{}': {}", name, e.what());
        return false;
    }
}

bool load_preset(const std::filesystem::path& preset_path, std::string& status_out,
                 const std::function<void()>& attempt_unload, const std::function<void()>& reload) {
    try {
        if (!std::filesystem::exists(preset_path)) {
            status_out = "Load failed: file not found";
            return false;
        }

        const auto persistent_dir = Framework::get_persistent_dir();
        const auto shader_settings_dir = persistent_dir / "data" / "plugins" / UEVR_VARIANT_ID / kShaderSettingsDirName;
        std::filesystem::create_directories(shader_settings_dir);
        const auto auto_path = shader_settings_dir / "auto.uevrpreset";

        auto name = preset_path.filename().string();
        s_active_preset_name = preset_path.stem().string();
        s_active_preset_dir = preset_path.parent_path();
        s_active_preset_is_builtin = (s_active_preset_dir == get_shipping_presets_dir());
        save_active_preset_to_disk();

        status_out = "Loaded \"" + name + "\" - reloading plugins...";
        spdlog::info("[PluginLoader] Loaded preset '{}', reloading plugins", name);

        // Unload first so attempt_unload_plugins()'s flush_now() drains the
        // current (about-to-be-replaced) registry to disk — then we copy the
        // chosen preset over auto.uevrpreset. If we copied first, flush_now()
        // would clobber it with the still-live old values.
        attempt_unload();
        std::filesystem::copy_file(preset_path, auto_path,
            std::filesystem::copy_options::overwrite_existing);
        reload();

        status_out = "Loaded \"" + name + "\"";
        return true;
    } catch (const std::exception& e) {
        status_out = std::string("Load failed: ") + e.what();
        spdlog::error("[PluginLoader] Failed to load preset: {}", e.what());
        return false;
    }
}

bool delete_preset(const std::filesystem::path& preset_path, std::string& status_out) {
    try {
        auto name = preset_path.filename().string();
        std::error_code ec;
        std::filesystem::remove(preset_path, ec);
        status_out = "Deleted \"" + name + "\"";
        spdlog::info("[PluginLoader] Deleted preset '{}'", name);
        if (s_active_preset_name == preset_path.stem().string() && s_active_preset_dir == preset_path.parent_path()) {
            s_active_preset_name.clear();
            s_active_preset_dir.clear();
            save_active_preset_to_disk();
        }
        return true;
    } catch (const std::exception& e) {
        status_out = std::string("Delete failed: ") + e.what();
        return false;
    }
}

void draw_preset_ui(UIState& state,
                    const std::function<void()>& attempt_unload,
                    const std::function<void()>& reload) {
    if (ImGui::CollapsingHeader("Plugin Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
        restore_active_preset_from_disk();
        // Auto-name helper: finds next available "Preset N" name (checks both local and global)
        auto next_auto_name = []() -> std::string {
            auto local = get_local_presets_dir();
            auto global = get_global_presets_dir();
            for (int i = 1; i < 1000; ++i) {
                auto name = "Preset " + std::to_string(i);
                auto file = name + ".uevrpreset";
                if (!std::filesystem::exists(local / file) && !std::filesystem::exists(global / file)) {
                    return name;
                }
            }
            return "Preset";
        };

        // Active preset indicator + overwrite
        if (!s_active_preset_name.empty()) {
            ImGui::Text("Active: %s%s", s_active_preset_name.c_str(), s_active_preset_is_builtin ? " (built-in)" : "");
            if (!s_active_preset_is_builtin) {
                ImGui::SameLine();
                if (ImGui::Button("Overwrite")) {
                    save_preset(s_active_preset_dir, s_active_preset_name, state.status);
                }
            }
        } else {
            ImGui::TextDisabled("No preset loaded");
        }

        ImGui::Spacing();

        // Save as new preset
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        ImGui::InputText("##PresetName", state.name_buf, sizeof(state.name_buf));

        // Sanitize to a safe single filename (no path separators, no reserved chars)
        auto sanitize_name = [](std::string s) -> std::string {
            std::erase_if(s, [](char c) {
                return c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
            });
            while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
            while (!s.empty() && (s.front() == '.' || s.front() == ' ')) s.erase(s.begin());
            return s;
        };

        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            auto dir = get_local_presets_dir();
            std::string name = sanitize_name((state.name_buf[0] != '\0') ? state.name_buf : next_auto_name());
            if (name.empty()) {
                state.status = "Invalid preset name.";
            } else {
                save_preset(dir, name, state.status);
                s_active_preset_name = name;
                s_active_preset_dir = dir;
                s_active_preset_is_builtin = false;
                save_active_preset_to_disk();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save Global")) {
            auto dir = get_global_presets_dir();
            std::string name = sanitize_name((state.name_buf[0] != '\0') ? state.name_buf : next_auto_name());
            if (name.empty()) {
                state.status = "Invalid preset name.";
            } else {
                save_preset(dir, name, state.status);
                s_active_preset_name = name;
                s_active_preset_dir = dir;
                s_active_preset_is_builtin = false;
                save_active_preset_to_disk();
            }
        }

        if (!state.status.empty()) {
            ImGui::TextWrapped("%s", state.status.c_str());
        }

        auto draw_preset_list = [&](const char* section_label, const std::filesystem::path& presets_dir) {
            auto presets = list_presets(presets_dir);
            if (presets.empty()) {
                ImGui::TextDisabled("  (none)");
                return;
            }
            for (const auto& name : presets) {
                ImGui::PushID((section_label + name).c_str());
                if (ImGui::Button("Load")) {
                    load_preset(presets_dir / name, state.status, attempt_unload, reload);
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete")) {
                    delete_preset(presets_dir / name, state.status);
                }
                ImGui::SameLine();
                ImGui::Text("%s", name.c_str());
                ImGui::PopID();
            }
        };

        ImGui::Spacing();
        ImGui::Text("Game Presets (this game):");
        draw_preset_list("local_", get_local_presets_dir());

        ImGui::Spacing();
        ImGui::Text("Global Presets (all games):");
        draw_preset_list("global_", get_global_presets_dir());

        // "Disable all" is intentionally a transient action, not a preset:
        // it resets every registered plugin to its compiled-in defaults
        // and forces enabled=0 (auto-saved via the normal debounce path).
        // Replaces the previous data-preset "All Off".
        ImGui::Spacing();
        if (ImGui::Button("Disable all shaders")) {
            uevr::settings_registry::disable_all();
            state.status = "All shaders disabled and reset to defaults";
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(disables every shader and resets all sliders to defaults)");

        // Built-in presets (shipped with the release, Load-only)
        auto shipping_dir = get_shipping_presets_dir();
        if (std::filesystem::exists(shipping_dir) && std::filesystem::is_directory(shipping_dir)) {
            auto shipping = list_presets(shipping_dir);
            if (!shipping.empty()) {
                ImGui::Spacing();
                ImGui::Text("Built-in Presets:");
                for (const auto& name : shipping) {
                    ImGui::PushID(("shipping_" + name).c_str());
                    if (ImGui::Button("Load")) {
                        load_preset(shipping_dir / name, state.status, attempt_unload, reload);
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", name.c_str());
                    ImGui::PopID();
                }
            }
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Tip: Select individual effects in the left menu under PluginLoader to tweak settings.");
    }
}

} // namespace uevr::preset_system
