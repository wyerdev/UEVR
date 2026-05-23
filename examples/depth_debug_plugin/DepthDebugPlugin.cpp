// Depth debug plugin (P0 verification gate for INPUT_DEPTH plumbing).
// See docs/active/plan-depth-fx.md §2.1.
//
// Visualizes UE's SceneDepthZ via four modes:
//   0 = Linear 0..1 gradient
//   1 = Banded linear depth (8m bands, rainbow)
//   2 = Edge overlay (depth discontinuities on top of scene)
//   3 = Per-eye check (red tint on left half, green on right) — verifies
//       per-eye depth is being received correctly when native stereo is on.
//
// Disabled by default. Compiles for both DX11 and DX12 backends.

#include <memory>

#include "imgui/imgui_impl_win32.h"
#include "uevr/Plugin.hpp"
#include "uevr/PluginSettings.hpp"
#include "effects/effect_runtime.hpp"

using namespace uevr;

static const char* g_dd_ps_src = R"(
cbuffer DepthDebugCB : register(b0) {
    uint   Mode;          // 0..3
    float  BandSize;      // metres per band (mode 1)
    float  EdgeStrength;  // mode 2
    float  _pad0;
};

Texture2D    Scene        : register(t0);
SamplerState SceneSampler : register(s0);

// fx_depth_tex / fx_depth_smp / fx_depth_info auto-injected by runtime.

struct PSInput { float4 Position : SV_Position; float2 TexCoord : TEXCOORD0; };

static const float3 k_palette[8] = {
    float3(1.0, 0.0, 0.0),
    float3(1.0, 0.5, 0.0),
    float3(1.0, 1.0, 0.0),
    float3(0.0, 1.0, 0.0),
    float3(0.0, 1.0, 1.0),
    float3(0.0, 0.5, 1.0),
    float3(0.5, 0.0, 1.0),
    float3(1.0, 0.0, 1.0),
};

float4 main(PSInput input) : SV_Target
{
    float2 uv = input.TexCoord;
    float3 scene = Scene.Sample(SceneSampler, uv).rgb;

    if (Mode == 0u) {
        // Linear gradient in [0,1] of (clamped) linear depth.
        float d01 = fx_sample_depth_01(uv);
        return float4(d01.xxx, 1.0);
    }
    else if (Mode == 1u) {
        // Banded linear depth — every `BandSize` units cycles palette.
        float dlin = fx_sample_depth_linear(uv);
        float band = BandSize > 0.0 ? BandSize : 8.0;
        uint idx = (uint)floor(dlin / band) & 7u;
        return float4(k_palette[idx], 1.0);
    }
    else if (Mode == 2u) {
        // Edge overlay: highlight depth discontinuities atop scene.
        float dc = fx_sample_depth_linear(uv);
        float dx = abs(ddx(dc));
        float dy = abs(ddy(dc));
        float edge = saturate((dx + dy) * EdgeStrength);
        return float4(lerp(scene, float3(1.0, 0.2, 0.2), edge), 1.0);
    }
    else {
        // Per-eye tint: left half red-biased, right half green-biased.
        // Each eye sees a full-viewport draw, so this should look like
        // left eye fully red-tinted, right eye fully green-tinted when
        // per-eye depth/pass dispatch is correct.
        float d01 = fx_sample_depth_01(uv);
        float3 tint = (uv.x < 0.5) ? float3(1.0, 0.2, 0.2) : float3(0.2, 1.0, 0.2);
        return float4(d01 * tint, 1.0);
    }
}
)";

struct DepthDebugCB {
    uint32_t Mode;
    float    BandSize;
    float    EdgeStrength;
    float    _pad0;
};
static_assert(sizeof(DepthDebugCB) == 16, "CB must be 16 bytes");

static constexpr const char* DD_VERSION = "0.1.0";
static constexpr float DD_DEFAULT_BAND = 8.0f;
static constexpr float DD_DEFAULT_EDGE = 0.05f;

class DepthDebugPlugin : public uevr::Plugin, public uevr::settings::Serializable {
public:
    bool     m_enabled       = false;
    int      m_mode          = 0;
    float    m_band_size     = DD_DEFAULT_BAND;
    float    m_edge_strength = DD_DEFAULT_EDGE;

    DepthDebugCB    m_cb{};
    fx::EffectRuntime m_runtime;
    bool            m_initialized = false;

    void init_passes() {
        if (m_initialized) return;
        m_initialized = true;
        fx::PassDesc pass;
        pass.ps_hlsl = g_dd_ps_src;
        pass.inputs  = { fx::INPUT_SCENE, fx::INPUT_DEPTH }; // depth at slot 1
        pass.output  = fx::OUTPUT_SCENE;
        pass.cb_data = &m_cb;
        pass.cb_size = sizeof(DepthDebugCB);
        m_runtime.set_passes({ pass });
    }

    void on_initialize() override {
        API::get()->log_info("[DepthDebug] Plugin initialized (v%s)", DD_VERSION);
        init_passes();
        uevr::settings::register_with_host(*this, API::get()->param());
    }

    // --- uevr::settings::Serializable ---
    std::string preset_section_name() const override { return "DepthDebug"; }
    int render_order() const override { return 50; } // run early so other plugins overwrite if disabled-by-default mistake

    std::vector<std::pair<std::string, std::string>> serialize_settings() const override {
        return {
            {"enabled",       m_enabled ? "1" : "0"},
            {"mode",          std::to_string(m_mode)},
            {"band_size",     std::to_string(m_band_size)},
            {"edge_strength", std::to_string(m_edge_strength)},
        };
    }

    void deserialize_settings(const std::map<std::string, std::string>& kv) override {
        auto it = kv.find("enabled");
        if (it != kv.end()) m_enabled = (it->second != "0" && !it->second.empty());
        it = kv.find("mode");
        if (it != kv.end()) { try { int v = std::stoi(it->second); if (v<0) v=0; if (v>3) v=3; m_mode = v; } catch (...) {} }
        auto get_float = [&](const char* k, float& out, float lo, float hi) {
            auto i = kv.find(k);
            if (i == kv.end()) return;
            try { float v = std::stof(i->second); if (v<lo) v=lo; if (v>hi) v=hi; out = v; } catch (...) {}
        };
        get_float("band_size",     m_band_size,     0.1f, 1000.0f);
        get_float("edge_strength", m_edge_strength, 0.0f, 10.0f);
    }

    void reset_to_defaults() override {
        m_enabled       = false;
        m_mode          = 0;
        m_band_size     = DD_DEFAULT_BAND;
        m_edge_strength = DD_DEFAULT_EDGE;
    }

    void on_draw_ui() override {
        if (ImGui::CollapsingHeader("Depth Debug")) {
            ImGui::TextDisabled("v%s — P0 depth plumbing verification", DD_VERSION);
            ImGui::TextWrapped("Visualizes SceneDepthZ. Disable for normal play.");
            bool changed = false;
            changed |= ImGui::Checkbox("Enabled##DD", &m_enabled);

            const char* modes[] = {
                "0: Linear gradient",
                "1: Banded linear",
                "2: Edge overlay",
                "3: Per-eye tint",
            };
            if (ImGui::Combo("Mode##DD", &m_mode, modes, IM_ARRAYSIZE(modes))) changed = true;

            if (m_mode == 1) {
                changed |= ImGui::SliderFloat("Band size (units)##DD", &m_band_size, 0.5f, 200.0f, "%.1f");
            } else if (m_mode == 2) {
                changed |= ImGui::SliderFloat("Edge strength##DD", &m_edge_strength, 0.0f, 1.0f, "%.3f");
            }

            if (changed) uevr::settings::notify_changed(*this, API::get()->param());
        }
    }

    void run() {
        if (!m_enabled) return;
        m_cb.Mode         = static_cast<uint32_t>(m_mode);
        m_cb.BandSize     = m_band_size;
        m_cb.EdgeStrength = m_edge_strength;
        m_cb._pad0        = 0.0f;
        m_runtime.execute();
    }

    void on_pre_render_vr_framework_dx11() override { run(); }
    void on_pre_render_vr_framework_dx12() override { run(); }
    void on_device_reset() override { m_runtime.release_resources(); }
};

std::unique_ptr<DepthDebugPlugin> g_plugin{ new DepthDebugPlugin() };
