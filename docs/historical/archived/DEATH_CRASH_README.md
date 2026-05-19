# [HISTORICAL] UEVR Death Crash — High On Life (Oregon-Win64-Shipping)

[2026-04] Unsolved. Kept for reference.

## Status: UNSOLVED

Reference note: this document is archived for the High On Life death-crash investigation. No fix from this document was shipped as part of the current Creatures of Ava / D3D12 hardening work.

## The Problem

When the player dies in High On Life with UEVR injected, the game's render thread dies silently. The game process stays alive but VR stops and the game hangs. This happens every time, 100% reproducible.

## What Happens (Timeline from Logs)

1. Game runs normally — D3D12 render path works, all pointers valid, VR frames submitted
2. Player dies → UE destroys game objects (player, camera, world actors)
3. **~3,111 access violations fire in a burst** within milliseconds (all null-page faults in game code)
4. OpenXR session state changes: state 5 → 4 → 5 (STOPPING → READY → STOPPING)
5. Render thread stops calling Present entirely
6. Framework detects no Present for 5+ seconds, tries D3D rehook but can't recover
7. Game hangs with "Windows message hook is still intact" repeating

## Key Log Evidence (April 1, 2026 test)

```
[13:04:35.440] VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 5
[13:04:35.475] VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 4
[13:04:36.212] VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 5
[13:04:37.068] [VEH Stats] AVs=3111 | hmd_nonnull=3108 high_addr=1 already=1 | deep=1 patched=0 rejected=1 | non_av=16
[13:04:40.789] Windows message hook is still intact, ignoring...
[13:05:20.875] Last chance encountered for hooking 
[13:05:21.875] Reached max D3D rehook attempts (0) after init
```

## Root Cause Analysis

UEVR replaces UE's stereo rendering device with a fake one (FFakeStereoRenderingHook). During death:

1. UE destroys and recreates rendering objects (player controller, camera, pawn, etc.)
2. The render thread still has UEVR's fake stereo device in UE's rendering pipeline
3. Code paths that reference the stereo device chain into game objects that are now null/destroyed
4. This causes thousands of AVs as the null propagates through deep engine call chains
5. The render thread terminates (UE's own SEH can't recover from this volume of exceptions)

The AVs are NOT in UEVR code — they're in the game's own code (`hmd_nonnull=3108` means UEVR's HMD pointer is valid, so the VR device itself is fine). The crashes come from the game trying to render stereo views when the objects needed to compute those views no longer exist.

## What Was Tried (and Failed)

### 1. VEH Transition Crash Handling (multiple iterations)
- **Approach**: Detect "transition crashes" in VEH handler (HMD non-null + null-page fault in game DLL), apply temporary context fixups (zero register + advance RIP)
- **Result**: Each zeroed register produced derived bad addresses → cascade of hundreds more AVs → game state corruption → delayed crash
- **Variations tried**: Escalation thresholds, cascade detection, C++ exception handling, RtlVirtualUnwind leaf unwind, system DLL spillover. All failed.
- **Key lesson**: VEH runs BEFORE SEH. Intercepting exceptions that UE's own SEH could handle corrupts game state worse than letting them through.

### 2. Transition Detection + Render Skip
- **Approach**: Check `engine->get_world()` and `engine->get_localplayer()` in `on_pre_engine_tick`, set `m_in_transition` flag, skip VR frame submission in `on_present`
- **Result**: Crashed on injection. Property lookups (`get_property<>`) crash during early engine init. Also required adding members to VR.hpp which shifts class layout for 20+ translation units — incremental builds produce stale object files.
- **Key lesson**: Cross-thread signaling (game thread → render thread) is a race condition by design. The render thread can crash BEFORE the game thread detects the transition.

### 3. Simply Removing VEH Transition Handling (current state)
- **Approach**: When HMD is valid, always return CONTINUE_SEARCH from VEH
- **Result**: Level load works. Death still kills the render thread — the 3,111 AVs overwhelm UE's SEH and the thread terminates.

## What Was NOT Tried

### A. Temporarily Disabling the Fake Stereo Device During Transitions
The FFakeStereoRenderingHook replaces engine vtable entries to intercept stereo rendering calls. If these hooks were temporarily uninstalled (restoring original vtable pointers) during death transitions, UE's render thread would stop calling into UEVR's stereo code entirely. The AVs wouldn't happen because UE would use its original (disabled) stereo rendering path.

**Challenge**: Requires detecting transition start/end reliably from code that runs BEFORE the render thread hits the bad path. The game thread's `on_pre_engine_tick` runs asynchronously from the render thread.

### B. Making the Fake Stereo Device Return Early / No-Op During Transitions
Instead of unhooking, the fake stereo device's hooked functions (`calculate_stereo_view_offset`, `get_stereo_projection_matrix`, etc.) could detect the transition state and return immediately without modifying view data. This would prevent the cascade into null game objects.

**Challenge**: Same detection problem as above. Also, returning early from these hooks may itself cause UE rendering issues if it expects specific outputs.

### C. Wrapping the Stereo Hook Callbacks in SEH
Each vtable hook callback in FFakeStereoRenderingHook could be wrapped in `__try/__except`. If an AV happens during the callback, the SEH handler catches it and returns a safe default instead of letting the exception propagate.

**Challenge**: These are vtable hooks called by UE's render thread. SEH unwind may not work correctly in hooked vtable calls. Also, 3,111 exceptions per death event even if caught would have performance implications.

### D. Hooking at a Higher Level (DrawWindow_RenderThread / Slate)
Instead of hooking individual stereo rendering functions, hook the Slate render thread entry point and skip the entire render pass during transitions. This would prevent UE from even trying to do stereo rendering during death.

### E. Investigating if Stock UEVR Has This Bug
It's unknown whether stock praydog/UEVR (master branch) also crashes during death in High On Life. If it does, this is a known limitation. If it doesn't, the crash was introduced by our feat/fakeHdr branch changes.

## Architecture Notes

### Render Pipeline
```
VR::on_present() [render thread, from DXGI Present hook]
  → FFakeStereoRenderingHook::on_frame()
  → D3D12Component::on_frame()
    → get_render_target_manager()->get_render_target() [VerifiedFTexture2D with vtable check]
    → get_native_resource() [vtable call on FRHITexture2D]
    → Copy to VR eye textures and submit to OpenXR
```

### Stereo View Hooks (called BY UE render thread)
```
UE Render Thread → FFakeStereoRenderingHook::calculate_stereo_view_offset()
  → VR::on_pre_calculate_stereo_view_offset() [pure math, no game object access]
  → UObjectHook::on_pre_calculate_stereo_view_offset() [accesses m_camera_attach.object if set]
  → Applies HMD rotation/position to view_location/view_rotation
```

### VEH Handler
```
FFakeStereoRenderingHook.cpp ~line 4035
  - Returns CONTINUE_SEARCH when HMD is valid (VR active)
  - Only handles crashes when HMD pointer is null (XR nullification active)
  - Permanent patches: xor reg,reg + NOP for identified XR-related null dereferences
```

### Key Files
- `src/mods/VR.cpp` — VR::on_present(), on_pre_engine_tick()
- `src/mods/vr/D3D12Component.cpp` — D3D12Component::on_frame(), texture submission
- `src/mods/vr/FFakeStereoRenderingHook.cpp` — VEH handler, stereo view hooks
- `src/mods/vr/FFakeStereoRenderingHook.hpp` — VerifiedFTexture2D, VRRenderTargetManager_Base
- `src/Framework.cpp` — D3D rehook logic, window message loop

### Build
- `cmd //c build.bat` from project root
- Deploy: `bash deploy.sh` → copies to `A:/UEVR/uevr 2026-01-13 (1127) - Mine/`
- Game log: `C:/Users/[username]/AppData/Roaming/UnrealVRMod/Oregon-Win64-Shipping/log.txt`
- Clean build required when modifying headers: `cmake --build build --config Release --clean-first`

### Critical Build Lesson
Adding members to VR.hpp or Framework.hpp changes class layout. CMake incremental builds may NOT rebuild all 20+ translation units that include these headers. This causes stale object files with wrong offsets → crash at runtime. Either use `--clean-first` or put new state in file-scope `static` variables in .cpp files.
