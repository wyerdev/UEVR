# Plan: AdaptiveTonemapper faithfulness fixes

Two-step plan to make `AdaptiveTonemapper` behave the same as the upstream
ReShade shader (`luluco250/FXShaders/Shaders/AdaptiveTonemapper.fx`) inside
UEVR's per-eye dispatch + UNORM-scene-RT pipeline.

Each step is atomic and independently shippable. Build green required before
declaring either complete; user verifies in-game between steps so the two
variables (adaptation *speed* vs absolute *brightness*) can be diagnosed
independently.

---

## Background — what differs from ReShade

Upstream is grounded by reading
[AdaptiveTonemapper.fx](https://raw.githubusercontent.com/luluco250/FXShaders/master/Shaders/AdaptiveTonemapper.fx)
(verified Apr 25 2026):

- `TonemapOperator = 2 (ACES)`, `Amount = 1.0`, `Exposure = 0.0`,
  `FixWhitePoint = true`, `AdaptRange = float2(0.0, 1.0)`, `AdaptTime = 1.0`,
  `AdaptSensitivity = 1.0`, `AdaptPrecision = 0`, `AdaptFocalPoint = (0.5, 0.5)`.
  Our defaults match.
- ReShade samples the swapchain backbuffer through samplers declared with
  `SRGBTexture = true` and writes back with `SRGBWriteEnable = true`. The GPU
  does the **real** sRGB ↔ linear curve (piecewise: linear segment for
  `x < 0.04045`, gamma 2.4 above). Inside the shader, values are linear.
- ReShade runs the technique once per swapchain frame against the single
  backbuffer.

Two real differences in our pipeline:

1. **Per-eye dispatch.** UEVR's native-stereo-fix calls our renderer hook twice
   per frame against two different scene RTs (main + scene_capture). Our
   `EffectRuntime::execute()` runs all three passes both times. `SaveAdapt`
   advances the adaptation EMA twice per wall-clock frame ⇒ **adaptation runs
   at 2× the configured speed** and is biased toward the second eye's content.
2. **Scene RT format ≠ swapchain.** JediSurvivor's scene RT is plain
   `B8G8R8A8_UNORM` (not `_SRGB`). We currently approximate the sRGB curve
   with `pow(2.2)`. Real sRGB has a linear toe near 0; `pow(2.2)` underestimates
   shadow luminance ⇒ smaller `adapt` ⇒ larger `exposure /= adapt` ⇒ image too
   bright. Per-pixel error is small but `AdaptiveTonemapper` divides by the
   adapted luminance, so the error cascades.

Other LDR shaders (LUT, vibrance, curves, lift-gamma-gain) are stateless
display-space transforms. A small gamma misestimate just shifts colors slightly.
`AdaptiveTonemapper` is uniquely sensitive because of the division.

---

## Step 1 — Faithful per-frame adaptation (single-frame scope) — BUILT + DEPLOYED LOCALLY (not yet committed) Apr 26 2026

**Goal.** `GetSmall` + `SaveAdapt` run **once per wall-clock frame total**
(matching ReShade's once-per-swapchain semantics). Both eyes' `Main` pass reads
the same adapted luminance.

**What was implemented (better than the original plan):** instead of plugin-side
frame-id tracking + `execute(pass_mask)` calls, the runtime itself was refactored
to own per-frame dispatch state (the **Cadence refactor / Option A**, see
`docs/effect-runtime-plan.md` Phase 3.7). Plugins now declare each pass's cadence
declaratively (`Cadence::OncePerFrame` for GetSmall+SaveAdapt,
`Cadence::EveryDispatch` for Main); the runtime computes the per-dispatch pass
mask itself and resets the per-frame counter from a lazily-registered
`on_present` hook (per-DLL registry; no upstream UEVR API change).

Additionally:
- `EffectRuntime::is_first_dispatch_in_frame()` helper added so plugin-side
  per-frame state (e.g. FrameTime sampling) gates correctly. AdaptiveTonemapper
  uses it to sample wall-clock dt only on the first dispatch of each HMD frame —
  otherwise sequential/AFR (which fire renderer hooks at ~2× wall-clock rate)
  halve the EMA alpha and shift steady-state exposure.
- `Main` PS now binds `LastAdapt : t1` (smoothed history), not `SmallTex` mip 0
  (raw current-frame luma). This is the faithful ReShade port — without it the
  effect still works but skips the temporal smoothing.
- SmallTex stays per-slot (per-frame scratch); LastAdapt remains shared across
  scene slots from Phase 3.6.

**Verification.** Build green. Deployed locally to `A:/UEVR/...`. User-tested
in Creatures of Ava across Native Stereo + Synchronized Sequential + AFR.
Per-HMD-frame FrameTime sampling verified via one-shot 30-sample log
(subsequently removed once confirmed). Not yet committed / merged.

### Closed sub-investigation: mode-dependent brightness (Apr 26 2026)

After Step 1 + Phase 3.6 + Phase 3.5 all shipped, AdaptiveTonemapper was visibly
brighter in "Synchronized Sequential" than in "Native Stereo" (~3.3 EV shift).
Investigation outcome (full notes in `AdaptiveTonemapperPlugin.cpp` header
comment):

- **Not** an EMA / FrameTime / pipeline issue. Verified by adding a permanent
  Debug TreeNode with 6 heatmap visualizers (log-scaled blue→red color ramp
  across 5 decades) + a 7th "calibration bars" mode that writes fixed reference
  RGB values directly to the output RT.
- The calibration bars **still differed per mode** — conclusive proof that the
  difference is DOWNSTREAM of our hook, in UEVR's compositing.
- Root cause: `D3D12Component::on_frame()` takes the AFR composite path
  (`AFR_LEFT_EYE` / `AFR_RIGHT_EYE` swapchains) when `is_using_afr() == true`
  (true for Synchronized Sequential per VR.cpp logging) and the non-AFR
  `DOUBLE_WIDE` swapchain path otherwise. All swapchains use
  `B8G8R8A8_UNORM_SRGB` but the two `m_openxr.copy(...)` paths apparently apply
  different sRGB gamma encoding when reinterpreting the `B8G8R8A8_UNORM` source.
- Affects ALL shader plugins, not just AdaptiveTonemapper. Documented as a
  UEVR-core issue; fix is `D3D12Component.cpp` work, not plugin work, and is
  parked on the next-steps backlog (low priority — user impact is mode-switch
  brightness shift, mitigated by per-mode preset selection).
- The Debug TreeNode + calibration bars are kept in the release build (zero
  cost when disabled — a few uniform-branch compares against a cbuffer int) for
  future investigation of similar pipeline-tail issues.

---

## Step 2 — Phase 3.5: real sRGB in `effect_internal.hpp`

**Goal.** Replace the `pow(2.2)` approximation in `fx_decode_scene` /
`fx_encode_scene` with the real piecewise sRGB curve, and use
sRGB-typed SRV/RTV views when the scene RT format is `*_UNORM_SRGB` so the
GPU does the conversion (matching ReShade's `SRGBTexture = true`).

**Code grounding.**
- `examples/renderlib/effects/effect_internal.hpp::scene_decode_macro_block(SceneRTColorSpace)`
  currently returns `pow(2.2)` for `AmbiguousUNORM` and identity otherwise.
- `examples/renderlib/effects/effect_internal.hpp::resolve_typeless_format()`
  collapses `*_UNORM_SRGB` typeless formats to plain `*_UNORM` — this is what
  blocks SRGBTyped behavior from working today (see
  `docs/effect-runtime-plan.md` Phase 3 §"sRGB decision").

**Changes.**

1. Replace the macro block in
   `effect_internal.hpp::scene_decode_macro_block()`:

   For `AmbiguousUNORM` (and we conservatively also handle `Unknown` the same
   way — assume sRGB, the dominant convention; warning still surfaces in UI):

   ```hlsl
   float3 fx_srgb_to_linear(float3 c) {
       float3 lo = c / 12.92;
       float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
       return (c <= 0.04045) ? lo : hi;
   }
   float3 fx_linear_to_srgb(float3 c) {
       float3 lo = c * 12.92;
       float3 hi = 1.055 * pow(max(c, 0.0), 1.0/2.4) - 0.055;
       return (c <= 0.0031308) ? lo : hi;
   }
   #define fx_decode_scene(c) fx_srgb_to_linear(c)
   #define fx_encode_scene(c) fx_linear_to_srgb(c)
   ```

   For `LinearFloat` and `SRGBTyped`: identity macros (GPU view does the
   conversion in the SRGBTyped case).

2. Audit `resolve_typeless_format()`. Today it maps
   `R8G8B8A8_TYPELESS → R8G8B8A8_UNORM` and similarly for B8G8R8A8. The fix is
   trickier than a single-line change because typeless tells us nothing about
   whether the engine intended sRGB or not. Options:
   - **Don't change it.** Continue producing `_UNORM` views. The runtime warning
     remains. Real sRGB curve in HLSL still gives a better approximation than
     `pow(2.2)`.
   - **Pass through fully-typed `*_UNORM_SRGB` formats** when the engine RT
     declares them directly (not typeless). For these, the runtime creates
     sRGB-typed SRV+RTV, and the macros are identity. This is the "ReShade
     parity" path.

   Recommended: do both. `resolve_typeless_format()` stays defensive (typeless
   ⇒ plain UNORM). New helper `prefer_srgb_view_for(DXGI_FORMAT)` returns the
   sRGB-typed sibling when the engine format is already sRGB-typed; backends
   call it when creating snapshot SRV + scene RTV. Classification stays the
   same (`SRGBTyped` for `*_UNORM_SRGB`, `AmbiguousUNORM` for plain `_UNORM`).

3. PSO/PS cache key already includes the colorspace selector
   (`scene_decode_cache_selector(cs, opt_in)`). Macro block is a string
   literal pointer — pointer-stable ⇒ cache invalidation is automatic when
   the colorspace classification changes (e.g. across a device reset that
   moves to a different game).

**Verify.**
- Build green.
- Static check: the new `fx_srgb_to_linear` round-trips through
  `fx_linear_to_srgb` to within 1 LSB at 8-bit. Could add a one-off unit test
  via `pylanceRunCodeSnippet` if desired, but a 5-line by-hand check at three
  sample points (0.0, 0.04, 0.5) is enough.
- User deploys + tests JediSurvivor. Expected: image brightness much closer
  to ReShade's behavior. If still off, the residual is genuinely "scene RT is
  not sRGB-encoded" — surface the existing red `AmbiguousUNORM` warning more
  prominently.

**Non-goals.**
- Auto-detecting whether plain `_UNORM` is sRGB or linear — DXGI provides no
  metadata to distinguish. The warning suffices.
- Per-shader opt-out — `needs_scene_colorspace_decode` already gates this on
  the consuming shader; existing 16 LDR plugins don't opt in and so don't
  regress.

---

## Order of operations (locked)

1. ~~Step 1, build green, deploy, user verifies adaptation speed.~~ Built + deployed locally Apr 26 2026, user-verified across all 3 VR modes. NOT YET COMMITTED.
2. Step 2, build green, deploy, user verifies brightness. PENDING.
3. Update `docs/effect-runtime-plan.md` Phase 3.5 entry to reflect what
   actually shipped (real piecewise vs `pow(2.2)`, the new
   `prefer_srgb_view_for` helper if added).
4. Update repo memory (`/memories/repo/uevr-build-next-steps.md`) with status.

## Decisions / non-goals

- Faithful = match ReShade's behavior, not "tune defaults". `Amount`,
  `AdaptRange`, etc. stay at upstream values.
- The Bloom plugin cleanup and Preset Phase B+C remain on the next-steps
  backlog and are independent of this work. The 16-plugin migration sweep
  is **CODE-COMPLETE Apr 26 2026** (Phase 3.9, see
  `docs/effect-runtime-plan.md`).
