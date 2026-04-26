Initial prompt: we are working on porting re-shade shaders to UEVR. But we ran into multi pass problems. Can you analyse UEVR and native stereo and native stereo fix. How is this done? How does it affect each eye etc. Native stereo fix was made because some game effects would only show up in 1 eye, so I think it somehow copies these over two the other eye. But there are still a few things missing, some shadows and some lights. Are you able to see what is still missing in native stere fix to make all effects visible in both eyes? Investigate this independently of our re-shade porting, we want do understand native stereo and native stereo fix compelelty before we move on to re-shade shaders issue. You can read through that first just to have an idea what that problem is

# Native Stereo and Native Stereo Fix in UEVR

A code-grounded analysis of how UEVR's native-stereo path works, what the
"Native Stereo Fix" toggle actually does to the engine, and the categories of
effects that are still missing in one eye after the fix is enabled. All file
references are to this repo (the praydog/UEVR fork).

---

## 1. The three rendering modes

[VR.hpp](../src/mods/VR.hpp#L29) defines:

- `NATIVE_STEREO = 0` — the engine's own stereo pipeline. Both eyes render into
  one **double-wide** scene render target via `IStereoRendering` (left half =
  eye 0, right half = eye 1). This is the only mode that gets the engine's full
  lighting/PP work, but stock UE rendering is also full of "do it only for the
  primary stereo view" branches.
- `SYNCHRONIZED` — UEVR re-purposes the engine's stereo path but
  resynchronizes pose-per-eye.
- `AFR` — alternating frames (left frame, right frame). Cheaper, no
  native-stereo tricks needed, but hits ghosting/temporal issues (hence
  `is_ghosting_fix_enabled`).

`is_native_stereo_fix_enabled()` is just `m_native_stereo_fix && !is_using_afr()`
— the "fix" is a stack of hacks layered **on top of** the native-stereo
pipeline. There is also a sub-toggle, `is_native_stereo_fix_same_pass_enabled()`
(default **on**), which changes the strategy in a critical way (see §3b).

---

## 2. The core problem the fix addresses

In stock UE4/UE5 native stereo, the right view
(`EStereoscopicPass::eSSP_SECONDARY` / `eSSP_RIGHT_EYE`) is treated as a "cheap
clone" of the primary view. Many engine subsystems have explicit branches like:

```cpp
if (View.StereoPass <= eSSP_PRIMARY) { /* do real work */ }
else { /* reuse primary's result */ }
```

…or "frame uniqueness" gates keyed off the view family's frame counter. So the
right eye historically receives:

- Stale/copied **translucency lighting volume**, **volumetric fog**,
  **SkyAtmosphere LUTs**, **distance-field AO setup**, **eye adaptation**,
  sometimes **planar reflections** and **lens flares**.
- Anything the engine considers "expensive, do it once per family and reuse for
  the secondary view".

praydog's native-stereo-fix forces the engine to actually do that work twice,
once per eye, by lying to the renderer.

---

## 3. The trick, end-to-end

The fix is implemented in three places that work together.

### (a) `BeginRenderViewFamily` — the double dispatch

[FFakeStereoRenderingHook.cpp `begin_render_viewfamily_real`](../src/mods/vr/FFakeStereoRenderingHook.cpp#L3380)

When the fix is on:

1. The `FSceneViewFamily` arrives with `views.count == 2` (left + right).
2. UEVR sets `views.count = 1` so the renderer thinks it's processing a
   **single primary view**.
3. Real `BeginRenderingViewFamily` is called → engine renders the left eye
   **into the engine's normal scene RT** (the double-wide one) using its full
   primary-view code path.
4. The runtime VR state is **cloned** from the previous frame slot to the next
   slot in the pose queue (`pose_queue[now]=pose_queue[last]` for OpenVR;
   `pipeline_states` for OpenXR) so both dispatches think they're the same XR
   frame.
5. UEVR then:
   - **Swaps the view family's render target** to a separately allocated
     `FTextureRenderTargetResource` — the *scene_capture* RT
     (`rtm->get_scene_capture_render_target()`). It's a separate texture (not
     the right half of the double-wide) for two reasons:
     - **Perf**: re-binding RT1 here would tank perf — "the engine is still
       working on the old render target" (RT1 still has pending work in the
       UE render graph + GPU pipeline; rebinding it as RT for a second BRVF
       pass forces flushes / resource hazards).
     - **The engine assumes a full RT, not a half**: the second BRVF call
       goes through the engine's *primary-view* code path. Primary views
       render into a render target sized to the view, starting at (0,0).
       They write the whole RT, not "the right half of some other RT". So a
       single-eye-sized texture is needed for the engine to render into
       normally. RT2's pixels later land in RT1's right half via the
       composite copy in (c).
   - Calls `scene->decrement_frame_count()` — fakes that the new pass is the
     same frame, fixing **motion vectors** for the right eye.
   - **Swaps `views[0] ↔ views[1]`** so now the *right* view is the one the
     renderer processes.
   - Calls real BRVF again → engine renders the right eye into the
     scene_capture RT.
   - Swaps back, restores the original RT.
6. `views.count` is restored to its original value at the end.

### (b) `FSceneView` constructor hook — coercing stereo_pass

[FFakeStereoRenderingHook.cpp ~L3220](../src/mods/vr/FFakeStereoRenderingHook.cpp#L3220)

When `same_pass` is enabled:

```cpp
if (init_options_stereo_pass > eSSP_PRIMARY) {
    init_options->set_stereo_pass(eSSP_PRIMARY);
    // also forces views->count = 0 during ctor to dodge UE5 stereo-special-case logic
}
```

This is the actual *trick*: the right view's `FSceneViewInitOptions` is
rewritten so the engine sees a **second primary view** instead of a secondary
view. That defeats every `if (StereoPass <= PRIMARY)` early-out in the
renderer, and the right eye now exercises *the same code paths* as the left.

### (c) Compositing — getting the right-eye RT into the headset image

[D3D11Component.cpp](../src/mods/vr/D3D11Component.cpp#L325) and the matching
DX12 component capture the scene_capture RT as an SRV
(`m_scene_capture_tex_ref`). At submit/composite time:

[D3D11Component.cpp ~L678 / ~L808](../src/mods/vr/D3D11Component.cpp#L678)
`CopySubresourceRegion(...)` copies the **left half of the scene_capture
texture** into the **right half of the double-wide engine RT** (or directly
into `m_right_eye_tex`). That's how the second-pass result reaches the right
eye image submitted to the headset.

### (d) Pre-render plugin dispatch — why our shader runtime sees it twice

[VR.cpp on_pre_render_vr_framework dispatch loop](../src/mods/VR.cpp#L2154) —
when the fix is on, after dispatching plugins for the main RT, UEVR calls
`set_scene_render_target_override(scene_capture)` and dispatches plugins
**again** for the scene capture RT. This is what forces `EffectRuntime` to see
two distinct `ID3D{11,12}Resource*` keys per frame (the "two SceneSlots"
fact captured in repo memory).

**Verified RT layout (JediSurvivor, Apr 25 2026 log; both `B8G8R8A8_UNORM`):**
- **Main scene RT — 8136×4016, double-wide stereo, both eyes side-by-side.**
  UEVR forces this in [`calculate_render_target_size`](FFakeStereoRenderingHook.cpp#L6716):
  `x = hmd_width * 2; y = hmd_height`. Left half = left eye, right half = right
  eye. UE renders both eyes into this RT itself; the right half is the one
  that comes out broken on affected games (missing lighting/lights/shadows/
  shadow decals — see §4).
- **scene_capture RT — 4068×4016, single-eye sized.** Allocated by
  `kismet_rendering->create_render_target_2d(world, hmd_width, hmd_height, …)`
  ([`FFakeStereoRenderingHook.cpp` ~L7779](FFakeStereoRenderingHook.cpp#L7779))
  and driven by the injected `USceneCaptureComponent2D` actor. The native-
  stereo-fix path renders the *right eye only* into this RT (via the swapped
  view family in (a)), then (c) composites its left half over the broken
  right half of the main RT.
- It is **not** "the right half of RT1" — it is a separately allocated UE
  `UTextureRenderTarget2D`, used as the source of correct right-eye pixels
  that *replace* RT1's broken right half later in the frame.

---

## 4. What's still missing in one eye — and why

The fix only catches things that are **gated on stereo_pass or driven by the
per-FSceneView render path**. Anything that happens **outside the per-view
rendering loop** — i.e. once per `FSceneViewFamily`, once per frame, or once
per `FSceneViewState` — still fires only for the first invocation. Concrete
categories to investigate, in roughly decreasing likelihood for "shadows +
lights missing":

### A. Per-frame work that completed during the first BRVF call

Some renderer subsystems set a "did this frame already" flag and short-circuit
on the second invocation:

- **Volumetric fog 3D texture** (`FVolumetricFogIntegration`) — keyed off the
  family's `FrameNumber`. The `decrement_frame_count()` call rewinds the
  *scene's* counter, but **`FSceneViewFamily::FrameNumber` and `GFrameNumber`
  are not touched**. So volumetric fog is computed against the left view origin
  and re-sampled on the right, producing the wrong scattering and missing
  god-rays/light shafts in the right eye.
- **Distance Field AO / Mesh Distance Field tile cull** — also frame-keyed.
- **Virtual Shadow Map page allocation / page-marking pass (UE5)** — page
  requests are *per view*, but the page table allocator runs once per frame.
  If the right view's page requests aren't picked up, shadow caster pages are
  missing → **missing/holey shadows in right eye, especially at distance**.
- **Lumen scene update / surface cache update** — once per frame, against the
  primary view origin.
- **Shadow projection compaction passes** — often gated on first view.
- **Hair strands cluster compute / Niagara GPU sim consume** — once per frame;
  second view sees empty buffers. Plausible cause for missing FX/lights.

### B. Per-`FSceneViewState` history that's the same pointer across both eyes

Each eye has its own `FSceneViewState` in true native stereo, so things like
TAA history, eye adaptation history, and Lumen final-gather temporal buffers
are per-eye. **But** when `same_pass` rewrites the right view's
`stereo_pass=PRIMARY`, some engine code chooses the view-state by
`(StereoPass, PlayerIndex)` and might pick the **left view's** state for the
right invocation. Symptoms: temporal flicker and brightness mismatch, not
strictly "missing", but worth checking.

### C. `View.StereoPass`/`View.StereoViewIndex` checks in *materials and shaders*

Some games use this in custom HLSL/material expressions to drive view-specific
effects (eye-attached HUD, water caustics, lens dirt). Forcing PRIMARY for both
eyes makes the right-eye branch never execute, which can manifest as "the
effect only shows in the left eye". That's the *opposite* failure mode from
what the fix targets and is why `same_pass` is a toggle: turning it off uses
the alternate branch (`init_options_stereo_pass > eSSP_PRIMARY` is left alone)
but then loses the primary-only effects.

### D. Instanced Stereo Rendering (ISR) / Mobile Multi-View

With `views.count = 1` the renderer doesn't take the ISR path, so shader
permutations that **only exist for ISR** (a few Niagara, hair, and some custom
postprocess materials) are skipped on the second pass. Result: those effects
render only into the half of the double-wide RT that the first invocation
produced.

### E. Effects rendered against the *primary* RT regardless of view

Anything that does its own `SetRenderTargets` to the engine's
BackBuffer/SceneColor pointer (rather than `View.SceneColorTexture`) writes to
the left RT both passes. Common offenders: third-party plugins, certain
Niagara renderers, screen-space decals if the project enabled the "cache to
scene color" path.

### F. Scene capture RT format/clear mismatches

[D3D11Component.cpp L339](../src/mods/vr/D3D11Component.cpp#L339) hard-codes
the scene_capture SRV/RTV format to `B8G8R8A8_UNORM`. If the game's scene RT
is HDR (`R10G10B10A2`, `R16G16B16A16_FLOAT`), the right eye loses HDR range —
high-intensity lights/specular highlights get clipped → "missing lights" in
the right eye. This is one to verify in the log; it prints
`Scene capture texture format:` once on identity change.

---

## 5. Concrete things to try / inspect, in priority order

1. **Compare the scene_capture RT format vs. the engine RT format** in
   `log.txt` after enabling fix. If the engine RT is `*_FLOAT` or
   `*_R10G10B10A2_UNORM` and we're forcing `B8G8R8A8_UNORM`, that's a direct
   cause of clipped highlights and missing lights in the right eye. Fix would
   be to mirror the engine RT's format when allocating the scene capture RT in
   `RenderTargetManager::create_scene_capture` (and update the SRV/RTV format
   in [D3D11Component.cpp L339](../src/mods/vr/D3D11Component.cpp#L339) / D3D12
   equivalent).
2. **Decrement `GFrameNumber` / `FSceneViewFamily::FrameNumber`** in addition
   to the scene's counter before the second BRVF call. This is the single
   highest-leverage change for getting the engine to redo per-frame caches
   (volumetric fog, VSM page table, DFAO tile compute).
3. **Reset the `FSceneViewState->LastRenderTime` / per-view `FrameNumber`** of
   the right view's view-state before the second invocation, so per-view-state
   "already did this frame" gates re-fire. UE has a
   `FSceneViewState::PrevFrameNumber` field in most versions.
4. **Verify whether ISR is being requested anywhere** when fix is on. Search
   game shader permutations for `ISR` and confirm those passes still draw to
   both halves; if not, those passes need to be coerced into non-ISR
   permutations (which is what the views.count=1 trick does already, but
   Niagara has separate codepaths).
5. **Toggle `same_pass` off and re-observe**: what *changes* between same_pass
   on/off identifies which effects depend on stereo_pass branches in materials
   vs. which depend on engine view-loop work. That bisection alone localizes
   most "still missing" effects.
6. **Check whether the right eye's view-state pointer differs from the left's**
   at the moment of the second BRVF call. Add a log of
   `views[0]->scene_view_state` for both invocations. If they're equal, that's
   a per-view-state aliasing bug that explains TAA/eye-adaptation/Lumen
   mismatches.

---

## 6. Tying back to multi-pass shader ports (separate concern, but relevant)

The shader work is exposed to all of the above because, with native_stereo_fix
on, `EffectRuntime` runs **twice per frame against two different scene RTs**
(per-`ID3D{11,12}Resource*` SceneSlot keying). Anything the engine fails to
render correctly on the second invocation (categories A–F above) is content
the shader pass can't recover, no matter how faithful the port. So
root-causing the missing engine effects is upstream of and necessary for the
multi-pass shader port to look symmetric.

---

# Part II — Deep dive: improving the Native Stereo Fix

A second pass through the code, after the §1–§6 overview, surfaced a smoking
gun and several concrete improvement levers. This section is **planning only**
— no implementation yet. Each item is grounded in an exact file/line.

## 7. The smoking gun: scene_capture RT is hard-coded LDR

The right-eye scene RT is created here:

[FFakeStereoRenderingHook.cpp ~L7780](../src/mods/vr/FFakeStereoRenderingHook.cpp#L7779)

```cpp
auto tgt_raw = kismet_rendering->create_render_target_2d(
    world, hmd_w, hmd_h, /*format=*/2, clear_color, /*create_mips=*/false);
```

`format=2` is UE's `ETextureRenderTargetFormat::RTF_RGBA8`. The
**right-eye scene RT is therefore an 8-bit LDR texture, regardless of what
the engine's main scene RT is**. Then both
[D3D11Component.cpp L339](../src/mods/vr/D3D11Component.cpp#L339) and
[D3D12Component.cpp L270](../src/mods/vr/D3D12Component.cpp#L270) bind that
texture as `B8G8R8A8_UNORM` to match.

What this means in practice:

- The left eye's scene path produces values >> 1.0 (volumetric god rays, sun
  disk, specular highlights, lens flares, bloom seed) because the engine RT
  is `R16G16B16A16_FLOAT` or `R10G10B10A2_UNORM` for HDR-on projects.
- The right eye's scene path writes to an 8-bit RT. The engine still does
  HDR math internally, but **`OMSetRenderTargets` with an 8-bit RT clamps
  the output at the ROP**. Highlights get clipped to 1.0; anything that
  *only shows up* because of HDR overbrights — lens flares, bloom feedback,
  sky shafts, specular sparks — is silently missing on the right eye.

This alone is the most likely root cause of the user-visible complaint
("missing lights / specular / lens flares / shafts in one eye").

**Fix shape (planning):**

- Engine call: change `format=2` → `format=5` (`RTF_RGBA16f`). One token.
  Ideal: query the engine's main RT format at allocation time and pick
  HDR vs LDR to match (handles the rare LDR-only project).
- D3D11 view bind: `m_scene_capture_tex_ref.set(scene_capture_rt,
  DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT)` at
  [D3D11Component.cpp L339](../src/mods/vr/D3D11Component.cpp#L339).
- D3D12 setup: `m_scene_capture_tex.setup(device, scene_capture_rt,
  DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, ...)` at
  [D3D12Component.cpp L270](../src/mods/vr/D3D12Component.cpp#L270).

Verification path: `Scene capture texture format:` is already logged at
[D3D11Component.cpp L337](../src/mods/vr/D3D11Component.cpp#L337) on identity
change, so we can confirm the new format from `log.txt` immediately.

## 8. `decrement_frame_count` only touches one of three counters

[FScene.hpp L31](../dependencies/submodules/UESDK/src/sdk/FScene.hpp#L31)
shows `decrement_frame_count()` rewinds **only** `FScene::FrameNumber` (offset
auto-discovered from `IncrementFrameNumber`'s prologue).

But the SDK *also* knows where `FSceneViewFamily::FrameNumber` lives:
[FFakeStereoRenderingHook.cpp L3533](../src/mods/vr/FFakeStereoRenderingHook.cpp#L3533)
reads `SceneViewExtensionAnalyzer::frame_count_offset` for diagnostics, but we
**never write it** when looping back for the second eye.

Per-frame engine subsystems gate on `FSceneViewFamily::FrameNumber` (and on
`GFrameCounter`, the global UE engine variable), not on `FScene::FrameNumber`.
Specifically the following caches/keys re-fire only if the family frame
number changes:

- **Volumetric fog 3D injection** (`FVolumetricFogIntegration::IntegrateVolume`)
  — caches "last frame integrated" by ViewFamily->FrameNumber. Same number on
  the second pass → skip → right eye samples the LEFT eye's fog volume.
- **Virtual Shadow Map page allocation (UE5)** — page table is allocated once
  per family-frame; second eye's page requests are dropped → missing/holey
  shadows in the right eye, especially distance & foliage.
- **Distance Field AO / Mesh Distance Field tile cull** — frame-keyed.
- **Hair strands GPU sim consume / Niagara consume** — "did I dispatch this
  frame".
- **Lumen scene update / surface cache** — once per family-frame, against the
  primary view origin.
- **Translucency Lighting Volume (TLV)** injection — 64³ volume cached per
  frame; second pass reads the LEFT-eye-positioned volume.
- **Sky atmosphere LUTs** — refreshed once per family-frame.
- **Mesh Draw Command (MDC) caches** — keyed on `(SceneRenderer, ViewFamily)`;
  second pass may hit a stale cache slot that excludes decal/Niagara draws.

Visible result: shadow holes that depend on VSM page faulting, missing
god-rays/light shafts, sky scattering wrong at the horizon, translucency-lit
FX (smoke, glass with point lights) wrong, Lumen indirect bounce wrong, and
some decal categories missing on the right eye.

**Fix shape (planning):**

- Add a sibling helper next to `FScene::decrement_frame_count` for the view
  family — the offset is already discovered. Call it in the `wants_swap`
  block at
  [FFakeStereoRenderingHook.cpp L3490](../src/mods/vr/FFakeStereoRenderingHook.cpp#L3490).
- Optional: also bump `GFrameCounter` (global, pattern-scannable from
  `Core` exports).
- Sledgehammer alternative: instead of decrementing, **bump
  `FSceneViewFamily::FrameNumber` *up* by a large random delta** on the
  second pass — defeats every "did this frame" gate at once without needing
  to enumerate them.

## 9. View-state aliasing under `same_pass`

[FFakeStereoRenderingHook.cpp L3220](../src/mods/vr/FFakeStereoRenderingHook.cpp#L3220)
overrides the right view's `init_options->stereo_pass` to `eSSP_PRIMARY`
*before* the engine's `FSceneView` constructor runs. The constructor likely
looks up an `FSceneViewState*` keyed off `(StereoPass, PlayerIndex)` (in
stock UE the right-eye state is on `eSSP_RIGHT_EYE`).

Effect: depending on UE version, the engine may now hand the right-eye view
the **left-eye's `FSceneViewState`**. That state holds:

- TAA history textures
- Eye adaptation (auto-exposure) histogram + interpolated exposure value
- Lumen final-gather temporal buffers
- SSR temporal accumulation
- Distance-field shadow temporal accumulation
- Heightfield fog accumulation

If both eyes share one history, you get inter-eye flicker / ghosting /
brightness mismatch *and* the second-rendered eye writes over the first eye's
history every frame, so neither converges.

**Cheap diagnostic:** turn `same_pass` off and watch which artifact category
changes. If `same_pass=off` regains some shadow/light fidelity, view-state
aliasing is real.

**Structural fix (planning):** in the sceneview ctor hook, after coercing
`stereo_pass=PRIMARY`, also swap in a stable second `FSceneViewState*` that
UEVR keeps alive (same lifetime pattern as the scene_capture component) so
the right invocation gets isolated temporal state. UE itself does this for
split-screen.

## 10. RDG transient allocator aliasing between FSceneRenderers

Both BRVF invocations record on the same render thread back-to-back, with no
intervening `FlushRenderingCommands()`. RDG's transient resource allocator
pools per-family textures and **reuses physical memory between
FSceneRenderers if their lifetimes don't overlap**. Several UE passes use
"import-and-skip-if-same-frame" logic: they look at
`View.Family->FrameNumber`, see it's the same, treat the resource as
already-written, and never re-issue the writes. The second eye then reads
garbage.

**Mitigation candidates (planning):**

- Insert `FlushRenderingCommands()` between the two BRVF calls. Cost: a
  render-thread stall (probably <1 ms) + may erode any pipeline overlap
  benefit. Worth measuring before shipping.
- The Tier-2 family-frame-number bump (§8) also breaks this aliasing as a
  side effect — likely the better lever.

## 11. Decals specifically

DBuffer / deferred-decal pass behavior depends on UE version:

- **UE4 (≤ 4.26):** DBuffer is allocated by `FSceneRenderer` per
  `FSceneViewFamily`. Each BRVF invocation creates its own `FSceneRenderer`
  so DBuffer should be re-allocated per pass — *unless* the RDG transient
  allocator pool reuses the same physical resource. If reused, the second
  pass sees stale DBuffer projected through the wrong eye's matrices →
  smeared/misaligned or missing shadow-decals on the right eye.
- **UE5:** DBuffer participates in RDG with explicit per-view registration;
  usually OK. But Mesh Decals via Niagara cache draw commands per
  `FMeshDrawCommandCache` keyed off `(SceneRenderer, ViewFamily)`. The
  second invocation may hit a cached entry that excludes decal draws because
  the first pass already "consumed" the cache slot.

**Diagnostic (no code):** is the failure decal-type-specific? Specifically,
do *deferred decals* (blood splats, projector decals) work but *mesh decals*
/ *Niagara decals* fail? That distinguishes RDG transient reuse from MDC
cache aliasing.

The blanket mitigation for both is again the family-frame-number bump (§8)
— it busts the MDC cache key.

## 12. ISR (Instanced Stereo Rendering) regression vector

By forcing `views.count = 1` before each BRVF call, the engine takes the
*non-ISR* code path. Most projects don't rely on ISR-only shader permutations
(ISR is an optimization, not a feature), but a few materials in newer UE5
templates have explicit `STEREO_INSTANCE_INDEX` branches that produce visible
content (eye-aligned post-process materials, stereoscopic VFX). Those paths
simply don't execute → missing on **both** eyes. Less likely to be the
"missing in one eye" complaint, but if you see something missing in *both*
eyes that the flat game has, this is the cause.

---

## 13. Concrete improvement plan, ordered by ROI

### Tier 1 — single biggest win

1. **Switch scene_capture RT format to HDR** (§7).
   - `create_scene_capture()` literal `format=2` → `format=5` (`RTF_RGBA16f`)
     in [FFakeStereoRenderingHook.cpp ~L7780](../src/mods/vr/FFakeStereoRenderingHook.cpp#L7779).
   - Bound D3D view formats → `R16G16B16A16_FLOAT` in
     [D3D11Component.cpp L339](../src/mods/vr/D3D11Component.cpp#L339) and
     [D3D12Component.cpp L270](../src/mods/vr/D3D12Component.cpp#L270).
   - Best: read the engine's main RT format at allocation time; pick
     HDR vs LDR to match.
   - **Predicted impact:** lens flares, sun/sky highlights, specular hot
     spots, volumetric god rays — visible in right eye for the first time.

### Tier 2 — single offset write, low risk

2. **Also rewind / bump `FSceneViewFamily::FrameNumber`** in the BRVF
   wants_swap block (§8). Offset is already discovered
   (`SceneViewExtensionAnalyzer::frame_count_offset`).
   - **Predicted impact:** at minimum fixes volumetric fog asymmetry and a
     chunk of TLV/Lumen-driven differences. Probably fixes VSM holes and
     decal staleness too.
3. Same idea for `GFrameCounter` if a stable offset can be located
   (pattern-scannable from `Core`).

### Tier 3 — view-state isolation

4. Detect whether forcing `stereo_pass=PRIMARY` causes the right view to
   share an `FSceneViewState*` with the left (§9). If yes, hand the right
   invocation a separately-allocated view-state (lifetime managed like the
   scene_capture component).
   - **Predicted impact:** fixes inter-eye TAA ghosting, eye-adaptation
     flicker, Lumen temporal mismatches, SSR shimmer.

### Tier 4 — RDG / cache invalidation

5. Try `FlushRenderingCommands()` between the two BRVF calls (§10). Measure
   perf. Toggle if shippable.
6. Sledgehammer: bump `FSceneViewFamily::FrameNumber` *up* by a large random
   delta on the second pass instead of keeping it equal — defeats every
   "did this frame" gate at once.

### Tier 5 — exposed UI toggles

7. Surface in the VR overlay so users can bisect their own game's failure
   without us reproducing every title:
   - Scene-capture HDR format (Auto / LDR / HDR16f)
   - "Bump family frame number on second pass" (off / decrement / increment)
   - "Flush RHI between eye passes"
   - "Force unique view-state for right eye"

---

## 14. Recommended order of operations when we start implementing

1. **Tier 1 first.** Highest user-visible win, smallest patch surface, no
   code-path divergence — only storage with more headroom. Verify via the
   already-emitted `Scene capture texture format:` log line.
2. **Tier 2 next.** Same patch window (BRVF wants_swap block + a sibling
   to `decrement_frame_count`). Offset already known, so it's a few lines.
3. Stop and play. Most user-visible "missing lights / shadows / decals"
   complaints should be gone or substantially reduced after Tier 1+2.
4. Tier 3 / 4 / 5 only if specific residual artifacts remain after Tier 1+2
   ship and we can name them with diagnostics.

## 15. Risks and unknowns

- HDR scene_capture RT memory cost: `hmd_w * hmd_h * 8 bytes` (vs 4 bytes
  for LDR). At 2160×2160 per eye that's ~37 MB instead of ~18 MB. Fine.
- Some games launch with a non-HDR engine RT (e.g. forward-shading mobile
  ports). Forcing HDR there is harmless but wasteful — hence "Auto" mode.
- `FSceneViewFamily::FrameNumber` decrement could *break* something that
  currently works because some game-side render features specifically rely
  on frame-number monotonicity. Toggle-gate it during validation.
- `GFrameCounter` write requires finding a global symbol pattern reliably
  across UE 4.18–5.5; not done yet. Keep optional.
- View-state isolation (§9) needs a stable allocation route. Reusing the
  scene_capture component's lifetime trick is the obvious approach but
  needs validation against UE5's `FSceneViewStateContainer`.

## 16. Scope boundaries

This document is **planning only**. No code is to be changed based on it
without an explicit go-ahead, and Tier 1 should land alone in its own
commit so its impact can be A/B tested cleanly against current builds.
