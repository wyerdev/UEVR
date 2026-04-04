# Attempt 3: Robust Level-Load Crash Fix for UEVR

## Status: VEH PATH ABANDONED, D3D12 HARDENING PARTIALLY IMPLEMENTED
All VEH-level approaches to fix gameplay transition crashes have been exhausted. The shipped work from this attempt is a conservative D3D12 hardening pass in `D3D12Component.cpp`; the broader VEH and stub-object ideas were abandoned. See "Attempt Results" below for details.

## TL;DR
UEVR crashes during some loads because it holds raw `FRHITexture2D*` pointers to render targets that UE can destroy or recreate at any time, and because the XR nullification + VEH approach cannot reliably separate legitimate XR crashes from gameplay-transition corruption. The practical release fix was: (1) guard D3D12 native resource access, (2) proactively invalidate and rebuild stale render-target state when UE swaps native resources, and (3) rate-limit the resulting recovery logs so users do not get spammed during recovery loops. Rare non-D3D12 transition crashes can still remain.

## Root Cause Analysis
During level transitions, multiple subsystems become fragile simultaneously:

1. **Dangling render target pointer** (PRIMARY):
   - `VRRenderTargetManager_Base::render_target` is a raw `FRHITexture2D*` — no refcount held
   - UE can destroy/recreate the underlying texture during loads without going through UEVR's `texture_hook_callback`
   - When `D3D12Component::on_frame()` calls `ue4_texture->get_native_resource()` on the stale pointer → AV

2. **Scene capture UTexture GC'd**:
   - The scene capture UTexture stored via `UObjectReference` can be garbage collected during level change
   - `get_scene_capture_render_target()` has try/catch but timing-dependent races remain

3. **VEH can't handle all code paths**:
   - During loads, new code paths may access the nullified HMD pointer
   - System DLLs, dynamic JIT code, or inlined BP functions may AV in ways VEH can't identify or patch
   - Original praydog VEH has same limitation → original UEVR also crashes

4. **OpenXR frame desync**:
   - If Present stops during a long load, `xrBeginFrame` may have been called without matching `xrEndFrame`
   - When Present resumes, frame state may be inconsistent

These are timing-dependent races, explaining why the crash is intermittent ("sometimes").

Historical note: the "Steps" section below is the original plan as written during investigation. The authoritative shipped outcome is in "Attempt Results", which documents the conservative D3D12 subset that was actually implemented.

## Steps

### Phase 0: Fix VEH Heuristic False Positives (CRITICAL, do first)

**Problem**: The heuristic fallback (Step C) accepts ANY AV in any game module — no check on the fault address. This means `0xFFFFFFFFFFFFFFFF` (-1, a return-invalid sentinel) gets treated as "null-page AV" and permanently patched. The register zeroing then cascades into more bad addresses (`0x3`, `0x7`, `0xb00000037`), each getting permanently patched, corrupting game code and crashing.

**Evidence**: Log shows patches #9-#16 all fired within 17ms during a dialogue transition. #9-#11 had fault addr `0xFFFFFFFFFFFFFFFF`, which is NOT null-derived. #12-#16 were cascades from the register zeroing.

**Fix**:
1. **Add null-page address filter to heuristic** — only accept `accessed_address < 0x10000` in the heuristic (Step C). This rejects `-1`, kernel addresses, and garbage pointers. Step A (instruction trace) is unaffected — if code directly loads from an XR vtable offset, it's verified regardless.
2. **Add cascade detection** — track rapid-fire patches (same millisecond). If >3 heuristic patches fire within 50ms, stop patching and reject further heuristic attempts until next on_frame tick.
3. **Fix the log message** — change "null-page AV" to accurately reflect what was checked.

### Phase 1: SEH-Protect D3D12 Resource Access (blocks nothing)

1. **Wrap `D3D12Component::on_frame()` inner body in `__try/__except`**
   - After the `setup()` call and device/queue/swapchain retrieval, wrap the render target access + copying logic
   - On EXCEPTION_ACCESS_VIOLATION: log the fault addr, set `rtm->render_target = nullptr` and `rtm->ui_target = nullptr`, set `m_force_reset = true`, return `VRCompositorError_None`
   - This prevents any stale-pointer crash from killing the game

2. **Wrap `VR::on_present()` D3D12 plugin dispatch in `__try/__except`**
   - The `main_native_rt` resolution already has null checks, but SEH catches dangling-pointer cases
   - On exception: skip plugin dispatch, log, continue to `on_frame()` 

3. **Wrap `D3D12Component::on_frame()` scene capture access in `__try/__except`**
   - `scene_capture->get_native_resource()` can fault if the scene capture UTexture was GC'd between the `valid()` check and the call
   - On exception: reset `m_scene_capture_tex`, continue

### Phase 2: Proactive Stale RT Detection (*parallel with Phase 1*)

4. **Track last-known native resource pointer in D3D12Component**
   - Add `ID3D12Resource* m_last_known_native_rt = nullptr` member
   - Each frame, compare `ue4_texture->get_native_resource()` result against cached value
   - If the pointer CHANGED → the RT was recreated → clear `m_game_tex`, reset copy state, update cache
   - If the FRHITexture2D* itself changed from last frame → texture_hook_callback already handles this (the cached pointer in rtm is updated)

5. **Add `get_native_resource()` null check BEFORE `on_frame()` proceeds**
   - After `ue4_texture = rtm->get_render_target()`, check `ue4_texture->get_native_resource()` explicitly
   - If null, skip frame (return early) — the SEH from step 1 is backup

### Phase 3: Transition Detection & OpenXR Keepalive (*depends on Phase 1*)

6. **Track "transition state" in D3D12Component**
   - Add `uint32_t m_frames_without_valid_rt = 0` counter
   - Increment each frame where RT is null/stale; reset to 0 on successful frame
   - If counter > 30 → we're in a transition, log this state

7. **During transition: destroy scene capture proactively**
   - When transition detected, call `rtm->destroy_scene_capture()` 
   - Set a "recreate after N stable frames" flag to avoid immediate re-creation attempts during the transition

8. **OpenXR frame keepalive during load stalls**
   - In the `hook_monitor` thread (runs every 500ms), check if `last_on_frame_tick` is stale (>3 seconds)
   - If stale AND OpenXR is active: submit a minimal xrEndFrame (black projection layer) to keep the session alive
   - Requires careful synchronization — only do this if frame_began is true and we haven't already ended it
   - When Present resumes, `fix_frame()` handles re-synchronization

### Phase 4: Reduce VEH Dependence (long-term, *depends on Phase 1-2 validation*)

9. **Audit IXRTrackingSystemHook vtable for null-returning functions**
   - List all vtable functions that return nullptr
   - For functions where callers don't check for null (identified via VEH logs), return a static stub object instead
   - Priority: `GetHMDDevice()` already returns `&g_hook->m_hmd_device` (good), check `GetStereoRenderingDevice()`, `GetXRCamera()`, `GetViewExtension()`

10. **Close the nullification gap**
    - Currently `*(void**)potential_hmd_device = nullptr` happens before `IXRTrackingSystemHook` installs its stub
    - Move the VEH handler registration BEFORE the nullification, not after
    - Or: don't nullify at all — install the tracking system hook FIRST, then swap the pointer atomically
    - This eliminates the window where the pointer is null and any thread can AV

## Relevant Files
- `src/mods/vr/D3D12Component.cpp` — `on_frame()` at line 28: wrap in SEH, add stale RT detection, add transition counter
- `src/mods/vr/D3D12Component.hpp` — add `m_last_known_native_rt`, `m_frames_without_valid_rt` members
- `src/mods/vr/VR.cpp` — `on_present()` at line 2059: SEH around plugin dispatch, OpenXR keepalive call
- `src/mods/vr/FFakeStereoRenderingHook.cpp` — `setup_view_extensions()` ~line 3996: reorder nullification vs VEH registration; `on_frame()` ~line 228: transition state logging
- `src/Framework.cpp` — `hook_monitor()` at line 68: add OpenXR keepalive during load stalls
- `src/mods/vr/runtimes/OpenXR.cpp` — add `submit_keepalive_frame()` method for empty frame submission
- `src/mods/vr/IXRTrackingSystemHook.cpp` — audit vtable functions for null returns

## Verification
1. **Build**: `./build.bat` then `./deploy.sh`
2. **Test Creatures of Ava**: Load a save, play through a death/transition, verify no crash. Repeat 10+ times.
3. **Test level transitions**: Load into level → travel to different level → verify survival
4. **Check log.txt**: Verify SEH catches are logged (should appear during loads), no permanent error spiral
5. **Check VEH stats**: veh_crash_dump.txt should show FEWER total AVs (stub objects reducing VEH load)
6. **Test original UEVR games known to crash**: If user can name others, test those too
7. **Regression**: Normal gameplay should be unaffected — SEH should NOT fire during normal play

## Decisions
- SEH (`__try/__except`) is Windows-specific but UEVR is Windows-only, so this is fine
- OpenXR keepalive frames may cause a brief black flash in the headset during loads — acceptable, better than crash
- Phase 4 (VEH reduction) is optional for the initial fix but recommended for long-term stability
- We keep the VEH handler as a safety net even with all improvements

## Further Considerations
1. **FRHITexture2D refcounting**: Ideal fix would store render target in FTexture2DRHIRef (TRefCountPtr) to prevent destruction while UEVR holds a reference. However, this requires calling AddRef/Release through UE's internal refcount mechanism which isn't easily accessible from the SDK. Recommend investigating if `FRHITexture2D::AddRef()` is virtual and accessible.
2. **D3D11 path**: This plan focuses on D3D12. The D3D11 path in `D3D11Component.cpp` has similar patterns and should get the same SEH treatment if D3D11 games also crash during loads.
3. **Thread safety of `rtm->render_target`**: This is written from the render thread (texture_hook_callback) and read from the present thread (on_frame). Currently no synchronization. Consider `std::atomic<FRHITexture2D*>` or a simple spinlock.

---

## Attempt Results

### Phase 0: VEH Heuristic Address Filter — FAILED

**What we did**: Added `accessed_address <= 0x10000` filter to the heuristic (Step C) so only null-page AVs get patched. Also added cascade detection (3+ patches in 50ms = blocked).

**Result**: Broke injection. Address `0xb87944ec` at instruction `7ff627e151a6` (previously working as patch #6) was rejected because `0xb87944ec > 0x10000`. This is a legitimate XR chain crash — the engine dereferences the null XR pointer, gets a stale heap address, and faults on it. The address is high but the root cause is our nullification.

**Lesson**: XR nullification doesn't just produce null-page faults. Engine code can traverse 2-3 levels of pointer indirection before faulting, producing high addresses from stale heap memory. Any address filter will reject these legitimate chain crashes.

**Reverted**: Both address filter and cascade detection were reverted together (untested separately).

### Phase 0 Addendum: Cascade Detection Alone — NOT TESTED

Cascade detection (3+ rapid patches = stop) was bundled with the address filter and reverted together. In theory it would protect gameplay without blocking injection. However, analysis showed the injection patches ALSO fire in rapid bursts:
- 11:58:13.805: patches #1-#5 (5 in same ms)
- 11:58:13.837: patches #6-#9 (4 in same ms)

A threshold of 3 would block patches #4-#5 and #8-#9 at injection — which are needed. There is no timing-based way to distinguish legitimate rapid patches from cascade corruption. **Not worth implementing.**

### Phase 4: Stub Objects (Close Nullification Gap) — FAILED

**What we did**: Modified `IXRTrackingSystemHook::pre_initialize()` to install stub vtable objects into the engine pointer immediately during construction, BEFORE the old XR object is nullified. This eliminates the null gap. Updated `FFakeStereoRenderingHook::setup_view_extensions()` to create IXRTrackingSystemHook FIRST, removing the explicit `*(void**)potential_hmd_device = nullptr`. Updated VEH `hmd_is_valid` check to compare against stub pointers instead of null.

**Result**: Crashed on injection (never happened before). The VEH crash dump showed the fatal crash was at `7fff118e2a96` (a system DLL) accessing `0x7FFF0058`. The stub's ~185 vtable entries return nullptr, and UE code that receives a non-null XR pointer happily calls through the vtable, gets nullptr results, and passes them into system DLLs that crash. The VEH can't patch system DLL code.

**Why null is safer than a stub**: UE has built-in null checks for the optional XR/HMD pointer. When the pointer is null, engine code takes its "no XR system" path and skips XR processing entirely. A non-null stub bypasses these null checks, sending garbage (nullptr returns) into code that assumes the XR system is fully functional. The null approach causes AVs in the game module (patchable by VEH); the stub approach causes AVs in system DLLs (not patchable).

**Reverted**: All three files (IXRTrackingSystemHook.cpp, .hpp, FFakeStereoRenderingHook.cpp) restored to baseline.

### Phases 1-2: Conservative D3D12 Hardening — PARTIALLY IMPLEMENTED

Implemented in `src/mods/vr/D3D12Component.cpp`:

- guarded `FRHITexture2D::get_native_resource()` calls with `safe_get_native_resource()`
- guarded `ID3D12Resource::GetDesc()` calls with `safe_get_resource_desc()`
- added targeted invalidation for render target, UI, and scene-capture state
- detect native render target and scene-capture resource changes and force a clean rebuild instead of continuing with stale state
- reset native-resource tracking on `on_reset()`
- rate-limit the new invalidation warnings to avoid flooding user logs during recovery loops

This is a conservative subset of the original plan. It hardens the D3D12 path against stale-resource faults during load/transition churn, but it does not attempt OpenXR keepalive or broader render-thread exception recovery.

### Phase 3: OpenXR Keepalive / Broader Transition Detection — NOT ATTEMPTED

OpenXR keepalive during long load stalls and broader transition-state tracking were not implemented for this release.

### Fundamental Limitation

The VEH handler cannot distinguish between:
- **Legitimate XR crashes**: Engine code dereferences the null XR pointer (directly or through pointer chains) — these NEED to be patched
- **Cascade corruption**: Register zeroing from a previous patch produces bad addresses that cascade into more AVs — these should NOT be patched

Both types: (a) occur in the same game module, (b) have identical fault patterns, (c) happen in rapid succession, (d) can produce both low and high fault addresses. No filtering heuristic (address range, timing, cascade count) can cleanly separate them.

### What's Working

The VEH handler improvements from prior sessions are retained and working:
- `rejected_addresses` cache — prevents infinite re-analysis of non-XR crashes (Calisto Protocol fix)
- `temp_fixup_cache` — fast-path for transition crash repeat handling
- Step B removal — stack-walking was unreliable and slow
- Heuristic fallback — permissive (any game module AV = patch), which is the only approach that works for all games
- Injection and level-load crashes are handled reliably across tested games

The D3D12 hardening from this attempt is also retained and shipped:
- null/fault-safe native resource lookups for scene, UI, and scene capture targets
- forced D3D12 rebuild when UE swaps the underlying native render target
- safer descriptor reads during setup and reset
- reduced log spam from repeated invalidation during recovery loops

### Observed Result After the D3D12 Pass

- logs showed transient D3D12 failures such as back-buffer lookup problems being recovered from without an immediate fatal crash
- one rare cinematic-start crash was still observed, but its log signature pointed back to the cutscene/3P transition chain rather than the new D3D12 hardening
- conclusion for release: keep the D3D12 hardening, stop pursuing more speculative VEH work
