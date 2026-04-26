/*
LUT Plugin for UEVR
===================
Color grading via a 2D LUT (lookup table) PNG, applied to the UE scene render
target in `on_pre_render_vr_framework_dx{11,12}`.

Based on the ReShade LUT shader (`LUT.fx`) from crosire/reshade-shaders (slim
branch):
  https://github.com/crosire/reshade-shaders/blob/slim/Shaders/LUT.fx

Original LUT shader:
  Marty's LUT shader 1.0 for ReShade 3.0
  Copyright (c) 2008-2016 Marty McFly (Pascal Gilcher)
  License: no explicit grant in the file header; ported with credit per
    fork precedent (see docs/shader-candidates.md — same treatment as the
    CeeJay.dk shaders).

UEVR plugin wrapper: MIT license (C++ wrapper code ONLY)

LUT layout: a horizontal strip of 32 tiles, each 32x32 pixels (final image is
1024x32 RGBA8). Each tile encodes one blue slice; red is X within the tile,
green is Y, blue selects the tile. The shader samples two adjacent tiles and
linearly interpolates on blue.

LUT discovery (via `enumerate_shader_assets`):
  - All `lut_*.png` files in `<UEVR>/data/plugins/shader_assets/`         (shipped presets)
  - All `lut_*.png` files in `<persistent>/data/plugins/shader_settings/` (per-game; shadows global by name)
  - Plus `lut.png` (the legacy default name) if it exists in either location.

User picks the active preset via a combo + button row in the plugin UI; selection
is hot-applied via `EffectRuntime::replace_external_texture_png()` (no restart),
and persisted by filename in `lut_settings.txt`.

First runtime consumer of `examples/renderlib/effects/`.
*/

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "plugin_assets.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_lut_ps_src = R"(
cbuffer LUTParams : register(b0) {
    float Amount;
    float _pad0;
    float _pad1;
    float _pad2;
};

Texture2D Scene : register(t0);
Texture2D LUT   : register(t1);
SamplerState LinearSampler : register(s0);

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

#define LUT_TileSize   32.0
#define LUT_TileAmount 32.0

float4 main(PSInput input) : SV_Target {
    float4 src = Scene.Sample(LinearSampler, input.TexCoord);
    float3 c   = saturate(src.rgb);

    float blue_pos = c.b * (LUT_TileAmount - 1.0);
    float floor_b  = floor(blue_pos);
    float ceil_b   = min(floor_b + 1.0, LUT_TileAmount - 1.0);
    float frac_b   = blue_pos - floor_b;

    // Coordinates inside a single tile (red = X, green = Y), with half-texel inset
    // so linear sampling stays inside the tile.
    float u_in_tile = (c.r * (LUT_TileSize - 1.0) + 0.5) / LUT_TileSize;
    float v         = (c.g * (LUT_TileSize - 1.0) + 0.5) / LUT_TileSize;

    float u_a = (u_in_tile + floor_b) / LUT_TileAmount;
    float u_b = (u_in_tile + ceil_b)  / LUT_TileAmount;

    float3 col_a = LUT.Sample(LinearSampler, float2(u_a, v)).rgb;
    float3 col_b = LUT.Sample(LinearSampler, float2(u_b, v)).rgb;
    float3 lut_color = lerp(col_a, col_b, frac_b);

    return float4(lerp(src.rgb, lut_color, Amount), src.a);
}
)";

struct LUTParamsCB {
    float Amount;
    float pad[3];
};

static constexpr const char* LUT_VERSION = "1.1.0";

// Filename-pattern for discovered presets. Matches `lut_warm.png`, `lut_cinematic.png`, etc.
// `lut.png` (no underscore) is added separately as a legacy default.
static constexpr const wchar_t* LUT_PREFIX    = L"lut_";
static constexpr const wchar_t* LUT_EXTENSION = L".png";
static constexpr const wchar_t* LUT_DEFAULT   = L"lut.png";

// Convert "lut_warm_sunset.png" -> "Warm Sunset" for display.
// "lut.png" -> "Default".
static std::string preset_display_name(const std::wstring& filename) {
    if (filename == LUT_DEFAULT) return "Default";
    std::wstring stem = filename;
    auto dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem.erase(dot);
    if (stem.rfind(LUT_PREFIX, 0) == 0) stem.erase(0, std::wcslen(LUT_PREFIX));
    std::string out;
    out.reserve(stem.size());
    bool capitalize = true;
    for (wchar_t wc : stem) {
        if (wc == L'_' || wc == L'-') { out.push_back(' '); capitalize = true; continue; }
        char c = static_cast<char>(wc);   // ASCII filenames only
        if (capitalize && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out.push_back(c);
        capitalize = false;
    }
    return out.empty() ? "Default" : out;
}

class LUTPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool  m_enabled = false;
    float m_amount  = 1.0f;

    LUTParamsCB    m_cb{};
    fx::EffectRuntime m_runtime;
    int            m_lut_tex_id = -1;
    bool           m_passes_set = false;

    // Discovered presets and the user's current selection.
    std::vector<ShaderAsset> m_presets;
    std::vector<std::string> m_preset_labels;     // parallel to m_presets, for ImGui::Combo
    int                      m_selected = -1;
    std::wstring             m_selected_filename;  // persisted

    void on_initialize() override {
        API::get()->log_info("[LUT] Plugin initialized (v%s)", LUT_VERSION);
        rescan_presets();
        configure_runtime();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable -------------------------------------
    std::string preset_section_name() const override { return "LUT"; }
    int render_order() const override { return 1800; }

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        std::string utf8;
        utf8.reserve(m_selected_filename.size());
        for (wchar_t wc : m_selected_filename) utf8.push_back(static_cast<char>(wc));
        return {
            {"enabled",  m_enabled ? "1" : "0"},
            {"amount",   std::to_string(m_amount)},
            {"lut_file", utf8},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto it = kv.find("enabled");
        if (it != kv.end()) m_enabled = (it->second != "0" && !it->second.empty());
        it = kv.find("amount");
        if (it != kv.end()) {
            try { float v = std::stof(it->second); if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f; m_amount = v; } catch (...) {}
        }
        it = kv.find("lut_file");
        if (it != kv.end()) {
            std::wstring wfn;
            wfn.reserve(it->second.size());
            for (char c : it->second) wfn.push_back(static_cast<wchar_t>(c));
            m_selected_filename = wfn;
            // Re-resolve selection against currently-discovered presets and
            // hot-apply the texture if the runtime is already configured.
            for (size_t i = 0; i < m_presets.size(); ++i) {
                if (m_presets[i].filename == m_selected_filename) {
                    int idx = static_cast<int>(i);
                    if (m_passes_set && m_lut_tex_id >= 0) {
                        m_runtime.replace_external_texture_png(m_lut_tex_id, m_presets[idx].path);
                    }
                    m_selected = idx;
                    break;
                }
            }
        }
    }

    void reset_to_defaults() override {
        m_enabled = false;
        m_amount  = 1.0f;
        m_selected_filename.clear();
    }
    // ----------------------------------------------------------------------

    // Build the preset list. Includes all `lut_*.png` discovered under the standard
    // shader-asset directories (per-game shadows global by filename), plus `lut.png`
    // if present (legacy default).
    void rescan_presets() {
        m_presets       = enumerate_shader_assets(LUT_PREFIX, LUT_EXTENSION);
        // Inject legacy default at the top if it exists.
        auto legacy = resolve_shader_asset_path(LUT_DEFAULT);
        if (!legacy.empty()) {
            ShaderAsset def{ LUT_DEFAULT, legacy };
            m_presets.insert(m_presets.begin(), std::move(def));
        }
        m_preset_labels.clear();
        m_preset_labels.reserve(m_presets.size());
        for (auto const& p : m_presets) m_preset_labels.push_back(preset_display_name(p.filename));

        // Resolve selection: persisted filename > first available > none.
        m_selected = -1;
        if (!m_selected_filename.empty()) {
            for (size_t i = 0; i < m_presets.size(); ++i) {
                if (m_presets[i].filename == m_selected_filename) { m_selected = static_cast<int>(i); break; }
            }
        }
        if (m_selected < 0 && !m_presets.empty()) {
            m_selected = 0;
            m_selected_filename = m_presets[0].filename;
        }
    }

    void configure_runtime() {
        if (m_passes_set || m_presets.empty() || m_selected < 0) return;
        m_lut_tex_id = m_runtime.load_external_texture_png(m_presets[m_selected].path);
        fx::PassDesc pass;
        pass.ps_hlsl = g_lut_ps_src;
        pass.inputs  = { fx::INPUT_SCENE, m_lut_tex_id };
        pass.output  = fx::OUTPUT_SCENE;
        pass.cb_data = &m_cb;
        pass.cb_size = sizeof(m_cb);
        m_runtime.set_passes({ pass });
        m_passes_set = true;
    }

    // Hot-apply a different preset. The framework re-uploads the texture on the
    // next execute() — no plugin reload, no restart.
    void apply_preset(int idx) {
        if (idx < 0 || idx >= static_cast<int>(m_presets.size())) return;
        m_selected          = idx;
        m_selected_filename = m_presets[idx].filename;
        if (m_passes_set && m_lut_tex_id >= 0) {
            m_runtime.replace_external_texture_png(m_lut_tex_id, m_presets[idx].path);
        }
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("LUT Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("v%s", LUT_VERSION);
            ImGui::TextWrapped(
                "Color grading via a 1024x32 LUT PNG. Add presets as `lut_<name>.png` in:\n"
                "  Global  : <UEVR>/data/plugins/shader_assets/   (all games)\n"
                "  Per-game: <persistent>/data/plugins/shader_settings/   (override)");

            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##LUT", &m_enabled);
            changed |= ImGui::DragFloat("Amount##LUT", &m_amount, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blend between original (0) and LUT-graded (1)");

            // Preset combo + quick-pick button row.
            if (m_presets.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No LUT files found.");
            } else {
                int sel = m_selected;
                std::vector<const char*> labels;
                labels.reserve(m_preset_labels.size());
                for (auto& s : m_preset_labels) labels.push_back(s.c_str());
                if (ImGui::Combo("Preset##LUT", &sel, labels.data(), static_cast<int>(labels.size()))) {
                    apply_preset(sel);
                    changed = true;
                }
                // Button row: one button per preset, wraps via SameLine + CalcItemWidth.
                const float avail = ImGui::GetContentRegionAvail().x;
                float row_w = 0.0f;
                for (int i = 0; i < static_cast<int>(m_presets.size()); ++i) {
                    const auto& label = m_preset_labels[i];
                    const float btn_w = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f + 6.0f;
                    if (i > 0 && row_w + btn_w < avail) ImGui::SameLine();
                    else                                row_w = 0.0f;
                    row_w += btn_w + ImGui::GetStyle().ItemSpacing.x;
                    const bool active = (i == m_selected);
                    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if (ImGui::Button((label + "##LUTBtn" + std::to_string(i)).c_str())) {
                        apply_preset(i);
                        changed = true;
                    }
                    if (active) ImGui::PopStyleColor();
                }
            }

            ImGui::Spacing();
            if (ImGui::Button("Reset Amount##LUT")) { m_amount = 1.0f; changed = true; }
            ImGui::SameLine();
            if (ImGui::Button("Rescan Presets##LUT")) {
                rescan_presets();
                if (m_selected >= 0 && m_passes_set && m_lut_tex_id >= 0) {
                    m_runtime.replace_external_texture_png(m_lut_tex_id, m_presets[m_selected].path);
                }
            }
            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        configure_runtime();
        if (!m_passes_set) return;     // no presets discovered — nothing to apply
        m_cb.Amount = m_amount;
        m_runtime.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_runtime.release_resources(); }
};

std::unique_ptr<LUTPlugin> g_plugin{ new LUTPlugin() };
