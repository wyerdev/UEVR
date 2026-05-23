# Plan: Depth-Aware Effects for VR — ATTEMPTED

**Original date:** [2026-05-22]
**Moved to historical:** [2026-05-22]
**Status:** [HISTORICAL] — P0 plumbing shipped; depth activation **blocked**
on per-engine ABI variance in `FRenderTargetPool::FindFreeElement`. Branch
preserved separately for future revisit. See §10 below.
**Research basis:** [../research/depth-buffer-glamur-research.md](../research/depth-buffer-glamur-research.md)
**Goal (original):** Add depth-buffer support to `uevr::fx::EffectRuntime`
and ship the specific depth-aware plugins that improve VR image quality and
horror mood — nothing more.

---

## 0. Status summary (why this is historical)

The P0 *plumbing* (`INPUT_DEPTH` wiring through `EffectRuntime` DX11+DX12,
plugin-API `get_render_target_texture`, `depth_debug_plugin` skeleton) all
built, deployed, and is sitting in tree as reusable infrastructure. None of
it is harmful in isolation.

What blocks the plan is the *upstream* depth-pool hook
([src/mods/vr/RenderTargetPoolHook.cpp](../../src/mods/vr/RenderTargetPoolHook.cpp)).
Enabling **VR → Engine-specific options → Enable Depth-based Latency Reduction**
in UEVR's UI crashes Jedi Survivor (Respawn UE 4.27 fork) immediately on the
first call into the hooked function. We tried three increasingly aggressive
fixes (§10.2) and none of them held. The root cause is that
`FRenderTargetPool::FindFreeElement`'s compiled signature varies per UE
version *and* per engine fork, and we lack ground-truth disassembly to write
a fix that works universally. Until that's done, depth-based features cannot
be enabled on Respawn UE 4.27 titles without process-killing AVs, and we
can't fully validate P0 (§2.6 visual gate) on a UE4 title where depth was
supposed to work.

**Subsequent depth-aware plugin phases (P1 SSAO, P2 vignette, P3 fog) are
not started.** They depend on P0 visual confirmation, which depends on
depth being reliably available, which depends on the hook not crashing.

The plumbing & debug plugin shipped commits stay in tree. The
`RenderTargetPoolHook.{cpp,hpp}` rewrite + probe is being preserved on a
separate branch by the user for a future attempt.

---

## 1. Motivation & scope

We have a deep suite of color-only post-processing already (tonemap, LUT,
curves, sharpen, FXAA, blackcrush, eye-adaptation, …). What we **don't**
have is anything that uses the depth buffer. The depth buffer is *supposed*
to be captured by UEVR (`SceneDepthZ` via `RenderTargetPoolHook`) but the
hook crashes on the engine forks we'd most like to support.

The three effects below are the ones that actually move the needle in VR
specifically. Everything else from typical "shader suites" (DOF, fake GI,
heavy bloom, film grain) is either neutral or actively bad in a headset and
is explicitly **out of scope**.

### In scope (this plan)

1. **P0 — Depth plumbing in `EffectRuntime` + `depth_debug_plugin`.** Shared
   infrastructure. The debug plugin is the verification gate: we visually
   confirm depth is being sampled correctly (orientation, range, alignment
   with color) on real games before writing any effect that consumes it.
2. **P1 — `ssao_plugin`.** Real depth-based ambient occlusion (Glamarye-AO
   algorithm, MIT). Biggest single "pop" win in VR.
3. **P2 — `vignette_plugin`.** Depth-aware center darkening + edge falloff.
   Biggest single horror-mood win in VR.
4. **P3 — `depth_fog_plugin`.** Distance-based desaturate + tint. Atmosphere
   for horror without needing volumetric fog.

**Hard rule:** P1–P3 do not start until `depth_debug_plugin` shows a
correct-looking depth image on the test games. If depth looks wrong
(inverted, zero, garbage, misaligned), we fix P0 — we don't paper over it
in the effect shaders.

### Out of scope (explicit non-goals)

- **Depth of Field.** Fights HMD eye accommodation, often nauseating. Don't
  ship for VR.
- **Fake GI / global color bounce.** Brightens dark scenes → kills horror
  mood. Limited benefit for "pop" vs. cost.
- **Screen-space reflections.** Needs GBuffer normals (not exposed by RT
  pool). Materially harder than depth-only effects; defer indefinitely.
- **Heavy bloom / chromatic aberration / film grain plugins.** Existing
  bloom is enough; the rest are anti-VR.
- **Porting the whole Glamarye suite.** We're cherry-picking the AO portion
  only. FXAA/Sharpen/DOF/Fake-GI are not in scope.

---

## 2. Phase P0 — Depth plumbing in EffectRuntime

This is the dominant cost of the whole plan. Once it lands, P1–P3 are small
shader-side work.

**Order of work inside P0:** build the plumbing (§2.2–§2.5) and the debug
validator (§2.1) together, then **stop and visually verify** with the debug
plugin on the test games before declaring P0 done. The debug plugin exists
specifically so we never build a real effect on top of broken depth.

### 2.1 Validator: `depth_debug_plugin` (the verification gate) — [2026-05-22] **CODE LANDED, visual gate blocked by §10**

First plugin to ship. Single-pass, reads `INPUT_DEPTH`, writes
`OUTPUT_SCENE`. Modes selectable in plugin UI:

- **Linear gradient** — `float3(fx_sample_depth_01(uv).xxx)`. Near should be
  black, far should be white. If inverted, `fx_reversed_z` is wrong.
- **Banded** — `frac(fx_sample_depth_linear(uv) * 0.1)` colored by band
  index. Catches non-linear / wrong-near-far cases.
- **Edge overlay** — depth edges (`abs(ddx)+abs(ddy)` of linear depth)
  composited over the scene at low alpha. Confirms depth aligns with color
  geometry (no half-pixel offset, no eye-swap).
- **Per-eye check** — solid tint left half = left eye, right half = right
  eye. Confirms we are sampling the correct per-eye depth, not eye 0 twice.

**Disabled by default**, enabled via plugin UI. Lives in
`examples/depth_debug_plugin/`. Acceptance for this plugin is the same as
the P0 acceptance in §2.6 — they are the same gate.

**[2026-05-22] Shipped:** [examples/depth_debug_plugin/DepthDebugPlugin.cpp](../../examples/depth_debug_plugin/DepthDebugPlugin.cpp).
Disabled by default. ImGui `Mode` combo selects between linear gradient,
banded (rainbow palette, `BandSize` slider in metres), edge overlay
(`EdgeStrength` slider over composited scene), and per-eye tint (left=red,
right=green) modes. Pass is `{INPUT_SCENE, INPUT_DEPTH} → OUTPUT_SCENE`,
so `depth_slot = 1` (point/clamp sampler at s1 in both backends). Registered
as `[target.depth_debug_plugin]` in [cmake.toml](../../cmake.toml). DLL
output name `DepthDebug.dll`. Visual verification on real games is blocked
by §10.

### 2.2 Changes to [examples/renderlib/effects/effect_runtime.hpp](../../examples/renderlib/effects/effect_runtime.hpp) — [2026-05-22] **DONE**

- Add `inline constexpr int INPUT_DEPTH = -2;` next to `INPUT_SCENE`.
- Document: read-only, per-eye, automatically linearized via injected helper.
- Document: passes that bind `INPUT_DEPTH` get an auto-injected HLSL preamble
  exposing `fx_sample_depth_linear(uv)` and a `fx_depth_info` cbuffer.

### 2.3 Changes to [examples/renderlib/effects/effect_runtime_d3d11.cpp](../../examples/renderlib/effects/effect_runtime_d3d11.cpp) — [2026-05-22] **DONE**

- Plumbing: extended plugin API with
  `RenderTargetPoolHook::get_render_target_texture()` to extract the
  underlying `FRHITexture2D*` (4-file additive change, `[fork]`-marked).
- DX11 backend caches a per-identity SRV in `DepthSlot[2]`, allow-list
  reinterpret (`detail::scene_depth_format_to_srv`), per-pass `INPUT_DEPTH`
  branch in the SRV resolve loop, depth point/clamp sampler bound at the
  matching s-slot, `fx_depth_info` cbuffer uploaded + bound at b1.
- Preamble injected via `detail::depth_preamble_block(slot)` when a pass
  requests depth; PS cache keyed on `depth_slot` so same source compiled for
  different slot bindings is cached independently.
- Per-frame log lines split for each silent failure path (rt_obj null,
  rhi_tex null, native null, format allow-list miss, CreateDescriptorHeap
  fail). Success log on first SRV creation.

- When a pass's `inputs` contains `INPUT_DEPTH`:
  - Fetch `SceneDepthZ` via the existing plugin API
    (`uevr::API::RenderTargetPoolHook::get_render_target(L"SceneDepthZ")`).
    Cache per frame.
  - Inspect resource desc; create an SRV using the typeless-reinterpret
    format allow-list: `R24_UNORM_X8_TYPELESS` (D24S8 case) or `R32_FLOAT`
    (D32 case). Cache by `(resource, format)`. Log + skip on unknown format
    (never silent garbage).
  - Bind as the next SRV slot after color inputs.
- Lifecycle: depth RT can be reallocated by the engine (resolution changes).
  Cache invalidation: compare resource pointer each fetch; rebuild SRV on
  change.

### 2.4 Changes to [examples/renderlib/effects/effect_runtime_d3d12.cpp](../../examples/renderlib/effects/effect_runtime_d3d12.cpp) — [2026-05-22] **DONE**

- Same SRV reinterpret rules as DX11 (shared `detail::scene_depth_format_to_srv` allow-list).
- **Barrier wrap not needed.** UEVR's existing OpenXR depth-copy code
  ([src/mods/vr/D3D12Component.cpp#L24](../../src/mods/vr/D3D12Component.cpp#L24))
  documents and uses `ENGINE_SRC_DEPTH = DEPTH_READ | NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE`
  as the assumed state of `SceneDepthZ` by the time the plugin command list
  runs. That state already includes `PIXEL_SHADER_RESOURCE`, so no
  transition is required before sampling — same convention as the existing
  copy path. If a future engine version changes this assumption it will
  surface as a D3D12 debug-layer warning at the depth sample site.
- Per-identity SRV cached in non-shader-visible `DepthSlot[2]` heap; the
  descriptor is copied into the pass's shader-visible SRV ring at draw time
  via `CopyDescriptorsSimple` (no extra heap needed).
- Root signature extended: added CBV at b1 (root param 2) for
  `fx_depth_info`, and static samplers s1..s7 (point/clamp) so any depth
  binding at slot ≥1 sees correct filtering. s0 remains linear/clamp for
  color. Pass PSO cache keyed on `depth_slot` so the injected preamble is
  matched correctly.
- Per-invocation `fx_depth_info` UPLOAD ring `m_depth_cb` (k_invocation_ring
  × 256-byte chunks) bound via `SetGraphicsRootConstantBufferView(2, ...)`.
- Passes that request `INPUT_DEPTH` are skipped if depth resolution fails
  (allow-list miss, missing pooled RT, null FRHITexture, etc.); logged once.

#### Original sketch (kept for reference):

- Same SRV reinterpret rules as DX11.
- **Barrier wrap** around each pass that binds depth:
  `DEPTH_WRITE → DEPTH_READ | PIXEL_SHADER_RESOURCE → DEPTH_WRITE`.
  Reference: existing OpenXR depth-copy barrier pattern in
  [src/mods/vr/D3D12Component.cpp](../../src/mods/vr/D3D12Component.cpp#L532-L582).
- Descriptor heap sizing must account for the depth SRV per pass.

### 2.5 Auto-injected HLSL preamble — [2026-05-22] **DONE** (`detail::depth_preamble_block` in [examples/renderlib/effects/effect_internal.hpp](../../examples/renderlib/effects/effect_internal.hpp))

The runtime prepends the following to any pass that lists `INPUT_DEPTH`:

```hlsl
cbuffer fx_depth_info : register(b1) {
    float fx_z_near;
    float fx_z_far;
    uint  fx_reversed_z;   // 1 for stock UE4/5
    uint  fx_perspective;  // 1 normally
};

Texture2D    fx_depth_tex : register(t<SLOT>);
SamplerState fx_depth_smp : register(s<SLOT>);

float fx_linearize_depth(float z) {
    float z01 = fx_reversed_z ? (1.0 - z) : z;
    if (!fx_perspective) return lerp(fx_z_near, fx_z_far, z01);
    return (fx_z_near * fx_z_far) / (fx_z_far - z01 * (fx_z_far - fx_z_near));
}

float fx_sample_depth_linear(float2 uv) {
    return fx_linearize_depth(fx_depth_tex.SampleLevel(fx_depth_smp, uv, 0).r);
}

// Normalized 0..1 view-distance for plugins that just want "how far".
float fx_sample_depth_01(float2 uv) {
    return saturate((fx_sample_depth_linear(uv) - fx_z_near) /
                    max(fx_z_far - fx_z_near, 1e-3));
}
```

`fx_z_near` / `fx_z_far` come from the active per-eye projection matrix via
`API::VR::get_ue_projection_matrix(eye)` ([include/uevr/API.h:653](../../include/uevr/API.h#L653)).
`fx_reversed_z = 1`, `fx_perspective = 1` for stock UE4/5; treat as constants
for the first cut.

### 2.6 P0 acceptance criteria (the gate) — **BLOCKED**

**[2026-05-22] Build & code-landed status:**
- [x] Builds for both DX11 and DX12 backends (clean `build.bat` succeeds).
- [x] `depth_debug_plugin` compiles and links as `DepthDebug.dll`.
- [ ] Linear-gradient mode visually verified on a UE4 + UE5 game — **blocked**
      by §10. Cannot enable UEVR's depth toggle on Jedi Survivor without crash;
      no other test game has been tried yet.
- [ ] Banded mode shows smoothly varying bands — blocked, see above.
- [ ] Edge-overlay aligns with scene geometry — blocked, see above.
- [ ] Per-eye mode confirms independent left/right depth — blocked, see above.
- [ ] No new TDRs / D3D validation warnings — N/A until depth runs.
- [ ] Zero overhead for plugins that don't bind `INPUT_DEPTH` (PSO cache key
      includes `depth_slot`; non-depth passes hit the same cached PSO as before).

- Builds for both DX11 and DX12 backends.
- `depth_debug_plugin` linear-gradient mode shows near=black → far=white on
  at least two `is_depth_enabled()`-compatible test games (recommended: one
  UE4, one UE5).
- Banded mode shows smoothly varying bands without discontinuities.
- Edge-overlay mode aligns with scene geometry — no half-pixel shift, no
  eye-swap.
- Per-eye mode confirms left/right eyes sample independent depth.
- No new TDRs / D3D validation warnings on the test set.
- Plugins that don't bind `INPUT_DEPTH` show zero overhead vs. before.

If any of the above fails, P0 is not done. Do not start P1.

---

## 3. Phase P1 — `ssao_plugin` (real AO, depth-based) — **NOT STARTED**

**Why first:** highest VR-quality return per line of code. Stereo vision
already gives parallax depth cues; corner shading is the missing one. Adding
real SSAO makes objects feel seated in the world.

### 3.1 Algorithm

Port of Glamarye's "Fast AO" (MIT, Robert Jessop). See research doc §2.2 for
why this specific algorithm — screen-space circle samples, no normals
required, no inverse-projection needed.

- Single fullscreen pass, reads `INPUT_SCENE` + `INPUT_DEPTH`, writes
  `OUTPUT_SCENE`.
- Pre-processor `FAST_AO_POINTS` (2–16, default 6) — sample count knob.
- Checkerboard dither: alternate pixels use half-radius circle.
- "At least half must be closer" gate so flat surfaces don't shade.
- Variance fuzz on adjacent samples for smoother shading.

### 3.2 UI knobs (match upstream names)

- `AO strength` (0..2, default 1)
- `AO shine` (0..1, default 0.1) — brightens convex shapes
- `AO radius` (0.1..5 % of screen, default 0.5)
- `AO max distance` (0..1 normalized, default 0.5) — avoid haloing in fog
- `Reduce AO in bright areas` (0..1, default 0.3)
- `FAST_AO_POINTS` (preprocessor, default 6)

### 3.3 License hygiene

Upstream MIT header (Robert Jessop / Alex Tuderan, 2021) goes at the top of
the embedded HLSL **string** in `SsaoPlugin.cpp`. The C++ wrapper file itself
inherits the UEVR-build repo LICENSE — no author attribution added per the
clean-code rules.

### 3.4 P1 acceptance criteria

- Visible AO shading in corners / under objects on the test games.
- No haloing in clear/skybox scenes (validates `AO max distance`).
- Off-by-default; enabling has ≤1ms cost at default settings (matches
  Glamarye's published Witcher-3 numbers at 1080p).

---

## 4. Phase P2 — `vignette_plugin` (depth-aware center darkening) — **NOT STARTED**

**Why for horror:** the headset already isolates the player from peripheral
vision; subtly darkening the edges + far field amplifies the
claustrophobia / tunnel-vision feeling that horror relies on. Far cheaper
than scripted vignettes and works on every UE game.

### 4.1 Algorithm

Single fullscreen pass. Two contributions multiplied:

1. **Screen-edge falloff** — classic radial vignette, configurable inner
   radius + outer radius + intensity. No depth needed.
2. **Depth-far darkening** — multiplied by `1 - lerp(0, intensity,
   smoothstep(near_t, far_t, fx_sample_depth_01(uv)))`. Pulls down distant
   pixels.

Both contributions optional; default preset for "horror" enables both at
moderate strength, default preset for "neutral pop" enables only edge
falloff at low strength.

### 4.2 UI knobs

- `Edge intensity` (0..1, default 0.2)
- `Edge inner radius` (0..1, default 0.5)
- `Edge outer radius` (0..1, default 1.0)
- `Depth darkening` (0..1, default 0 for "pop", 0.4 for "horror" preset)
- `Depth near` / `Depth far` (0..1 normalized, defaults 0.3 / 1.0)
- `Tint color` (RGB, default near-black) — optional warm/cool bias

### 4.3 P2 acceptance criteria

- Edge vignette works without depth (graceful degrade when depth unavailable
  / disabled).
- Depth darkening visibly affects far field on the test games.
- No banding in low-intensity gradients (use dither in shader).

---

## 5. Phase P3 — `depth_fog_plugin` (atmospheric murk) — **NOT STARTED**

**Why for horror:** real volumetric fog is expensive and game-specific;
screen-space depth-fog is dirt cheap and works everywhere. Lets us add
atmosphere to games whose fog is too weak (or absent) for horror mood.

### 5.1 Algorithm

Single fullscreen pass. Per pixel:

```
d        = fx_sample_depth_01(uv)
fog_amt  = saturate((d - fog_start) / max(fog_end - fog_start, 1e-3))
fog_amt  = pow(fog_amt, fog_curve)                          // shape
sat      = lerp(1, fog_saturation, fog_amt)                 // desaturate
hsv.s    *= sat
col      = lerp(col, fog_color, fog_amt * fog_intensity)    // tint
```

Optional height-fog mode is **out of scope** (requires world-space Y, which
requires inverse projection — adds complexity, low VR payoff).

### 5.2 UI knobs

- `Fog intensity` (0..1, default 0.3)
- `Fog start` (0..1 normalized, default 0.4)
- `Fog end` (0..1 normalized, default 1.0)
- `Fog curve` (0.5..3.0, default 1.5) — softness of the falloff
- `Fog saturation` (0..1, default 0.4) — pull color toward gray as it fades
- `Fog color` (RGB, default cool-gray for horror preset; near-white for
  neutral preset)

### 5.3 P3 acceptance criteria

- Visible distance-based desaturate + tint on test games.
- No banding (use dither).
- Doesn't darken close-up gameplay (verify `Fog start` default).

---

## 6. Optional preset bundles — **NOT STARTED**

Once P1–P3 ship, add two named preset bundles to the existing preset system:

- **`vr_pop`** — `ssao_plugin` default + `vignette_plugin` (edge only, low)
  + `blackcrush_plugin` mild + `cas_plugin` mild.
- **`vr_horror`** — `ssao_plugin` slightly strong + `vignette_plugin`
  (edge + depth darkening) + `depth_fog_plugin` (cool-gray, strong) +
  `blackcrush_plugin` aggressive + cool-bias `liftgammagain_plugin`.

Presets are configuration, not code. Land them only after P3.

---

## 7. Risks (carried from research doc)

1. **Known-broken-depth games.** Pre-existing `is_depth_enabled()`
   compatibility envelope; depth plugins inherit it. Cannot ship
   enabled-by-default. — **Confirmed worse than expected: see §10.**
2. **Depth format reinterpret.** Allow-list only; log+skip unknown formats.
3. **DX12 barrier interference.** Reuse the proven OpenXR-depth-copy pattern;
   validate with PIX.
4. **MIT header preservation** for the embedded Glamarye AO HLSL in P1.

---

## 8. Phasing & dependencies

```
P0 (depth plumbing + depth_debug_plugin)        ← VERIFICATION GATE
 │  └── visually confirm depth on test games before going further
 ├── P1 (ssao_plugin)             — independent of P2/P3
 ├── P2 (vignette_plugin)         — independent of P1/P3
 └── P3 (depth_fog_plugin)        — independent of P1/P2
        └── Preset bundles (config only, after all three ship)
```

P0 is the gate. P1/P2/P3 can land in any order once P0 is stable **and the
depth debug view has been visually confirmed on real games**.

---

## 9. Decision: deferred until user approval

This plan is `[PROPOSED]`. Land P0 first as a focused commit/PR; iterate
P1–P3 only after P0 is verified on at least two `is_depth_enabled()`-
compatible test games (recommended: one UE4, one UE5).

---

## 10. What we tried for the depth-pool hook, and where it broke — [2026-05-22]

### 10.1 Symptom

In Jedi Survivor (Respawn UE 4.27 fork), flipping
**VR → Engine-specific options → Enable Depth-based Latency Reduction** in
the UEVR menu causes:

1. `RenderTargetPoolHook::hook()` succeeds — AOB scan in module `Renderer`
   finds the function at a plausible address and `safetyhook` installs.
2. The first engine call into the hooked function reaches our hook function
   and the trampoline call to the original returns cleanly. Log shows both:
   - `FRenderTargetPool::FindFreeElement (UE4) called for the first time!`
   - `Finished calling FRenderTargetPool::FindFreeElement!`
3. Immediately after, an access violation fires at instruction
   `0x7ffe21ff8c7a`. UEVR's VEH null-deref handler
   ([src/mods/vr/FFakeStereoRenderingHook.cpp#L4084](../../src/mods/vr/FFakeStereoRenderingHook.cpp#L4084))
   catches it, but its "skip past the faulting instruction" heuristic
   reports `Previous instruction does not use the same register as the
   dereference` and the process dies hard. Log: tail of
   `C:\Users\obybe\AppData\Roaming\UnrealVRMod\JediSurvivor\log.txt` from
   17:28:37.380 onward.

Note: `0x7ffe...` is in the typical ASLR range for both system DLLs and
relocated user DLLs; we did not resolve which module that address belongs to.

### 10.2 What we tried, in order

1. **Defer plugin-side activation.** We removed
   `RenderTargetPoolHook::activate()` from `EffectRuntime::set_passes()`
   so that simply loading a plugin that lists `INPUT_DEPTH` doesn't install
   the hook; install only happens when the user flips UEVR's depth toggle.
   **Result:** game injects fine, but the original crash still happens
   the moment the user toggles depth on. **Kept in tree** because it's the
   right design regardless.

2. **Defensive validation in `on_post_find_free_element`.** Added
   `is_readable` (VirtualQuery) + `looks_like_pool_name` helpers
   (anonymous namespace in `RenderTargetPoolHook.cpp`). The post-callback
   was modified to validate the `name` pointer on first invocation; if
   `name` doesn't look like a wide-string pool name, log a clear error
   ("This title's compiled signature likely differs from the assumed UE4
   ABI…") and disable depth-pool tracking for the rest of the session.
   **Result hypothesis:** game survives, depth feature gracefully no-ops on
   Respawn. **Actual result:** never re-tested in isolation before moving
   to the next attempt — see §10.3 limitation.

3. **Global ABI-agnostic raw-slot probe.** Rewrote the two hook function
   signatures to take raw `uintptr_t` register/stack slots and forward them
   bit-positionally to the trampoline:
   ```
   find_free_element_hook(pool, slot_rdx, slot_r8, slot_r9, slot_stack0,
                          a6, a7, a8, a9, a10);
   ```
   The post-callback (`on_post_find_free_element_raw`) probes two candidate
   layouts on the first call, using `looks_like_pool_name` against both
   `slot_r9` (UE5/Respawn shape) and `slot_stack0` (UE4-classic shape), and
   locks in whichever validates. A side-benefit: this fixes a latent praydog
   bug where the UE5 path's trampoline call dropped the last stack arg
   (passed 9 args instead of 10). **Result:** still crashes Jedi Survivor
   in the same place — AV at `0x7ffe21ff8c7a` immediately after `Finished
   calling FRenderTargetPool::FindFreeElement!`. Probe was insufficient.

### 10.3 What's verified vs. what's guessed

**Verified (from log + code):**
- AOB scan locates `FindFreeElement` at a real function address in Jedi
  Survivor's `Renderer` module.
- `safetyhook` inline-hook install succeeds.
- The trampoline call to the original function returns without crashing,
  so the function pointer is correct.
- The AV happens *after* the trampoline returns, before subsequent log
  lines flush.

**Guessed (no ground truth):**
- That Respawn's UE 4.27 fork drops `FRHICommandList&` from
  `FindFreeElement`'s parameter list. This was inferred from the symptom,
  not confirmed by disassembly.
- That there are exactly two ABI layouts in the wild (UE4-classic with
  `cmd_list`, UE5/Respawn without). There may be more.
- Which register/stack slot holds `Name` vs. `Out` in Respawn — the probe
  in attempt (3) assumed two specific shapes; reality may be a third.
- The most likely actual culprit of the §10.2(3) crash: my probe never
  validates `out` (just `name`), so when the locked-in layout puts `out`
  at the wrong slot, `g_hook->m_render_targets[name] = out->reference;`
  dereferences garbage and AVs. Probe could also have mis-detected the
  layout if `slot_stack0`'s value happened to point into a committed page
  with printable wchars in the first bytes (a tighter probe would require
  a known-prefix match like `Scene`, `GBuffer`, `Velocity`, `Shadow`,
  `Translucency`, `ScreenSpace`, `HZB`).

We did not attach a debugger, disassemble the Jedi Survivor binary, or
inspect the `FRenderTargetPool` struct layout at runtime.

### 10.4 What would unblock this

In rough order of robustness:

1. **Disassemble `FindFreeElement` at the found address** in 2–3 UE titles
   (one stock UE4, one stock UE5, Jedi Survivor) and read the actual
   prologue + how it touches RCX/RDX/R8/R9/stack. That gives ground truth
   for which ABI shapes are real. Then either:
   - branch on a prologue pattern at hook-install time, OR
   - keep the raw-slot hook signature but pick layout based on a static
     pattern match instead of a runtime probe.

2. **Stop trusting the call's args entirely.** After each call, walk the
   `FRenderTargetPool` object's internal `TArray<FPooledRenderTarget>` (or
   whatever the field is called in this UE version) and read names from the
   targets themselves. Removes ABI sensitivity at the cost of requiring
   per-UE-version struct-layout reverse-engineering.

3. **Diagnostic-only logging pass** (cheap intermediate step): on the first
   N calls, dump the value at each of RCX/RDX/R8/R9/[rsp+0x28]/[rsp+0x30]
   plus VirtualQuery results for each, but never dereference. Run on Jedi
   Survivor — won't crash because no derefs. From the dump, definitively
   identify which slot holds `Name` and which holds `Out` in Respawn, then
   write a correct probe. Doesn't generalize, but at least handles each new
   game on first failure.

User mentioned they can obtain UE source in the future. Note that UE source
alone does not solve this — the question is *which compiled signature each
shipped title's binary actually uses*, which depends on the engine fork,
inline expansions, and compiler optimizations. UE source helps identify
plausible signature variants but doesn't tell us which variant a given .exe
was built from.

### 10.5 Branch preservation

The `RenderTargetPoolHook.{cpp,hpp}` rewrite (attempt 3 — raw-slot global
probe) is being preserved on a separate branch by the user for future
revisit. `examples/renderlib/effects/effect_runtime*.cpp` and
`examples/depth_debug_plugin/DepthDebugPlugin.cpp` changes (P0 plumbing +
debug plugin) remain on the main working branch — they are useful regardless
of whether depth-pool tracking is fixed.

### 10.6 Files touched (for future archaeology)

P0 plumbing (keep):
- [include/uevr/API.h](../../include/uevr/API.h) — `get_render_target_texture`
- [include/uevr/API.hpp](../../include/uevr/API.hpp) — C++ wrapper
- [src/mods/pluginloader/FRenderTargetPoolHook.cpp](../../src/mods/pluginloader/FRenderTargetPoolHook.cpp) — wired through
- [src/mods/pluginloader/FRenderTargetPoolHook.hpp](../../src/mods/pluginloader/FRenderTargetPoolHook.hpp) — wired through
- [examples/renderlib/effects/effect_runtime.hpp](../../examples/renderlib/effects/effect_runtime.hpp) — `INPUT_DEPTH = -2`
- [examples/renderlib/effects/effect_runtime.cpp](../../examples/renderlib/effects/effect_runtime.cpp) — `set_passes` no longer calls `activate()` (with `[fork]` comment)
- [examples/renderlib/effects/effect_runtime_d3d11.cpp](../../examples/renderlib/effects/effect_runtime_d3d11.cpp) — DX11 depth SRV + diagnostic logs
- [examples/renderlib/effects/effect_runtime_d3d12.cpp](../../examples/renderlib/effects/effect_runtime_d3d12.cpp) — DX12 depth SRV + diagnostic logs
- [examples/renderlib/effects/effect_internal.hpp](../../examples/renderlib/effects/effect_internal.hpp) — `depth_preamble_block`
- [examples/depth_debug_plugin/DepthDebugPlugin.cpp](../../examples/depth_debug_plugin/DepthDebugPlugin.cpp) — debug visualizer
- [cmake.toml](../../cmake.toml) — `[target.depth_debug_plugin]`

Depth-pool hook rewrite (preserved on branch, not in main):
- [src/mods/vr/RenderTargetPoolHook.cpp](../../src/mods/vr/RenderTargetPoolHook.cpp) — raw-slot probe (broken on Respawn)
- [src/mods/vr/RenderTargetPoolHook.hpp](../../src/mods/vr/RenderTargetPoolHook.hpp) — raw-slot signatures
