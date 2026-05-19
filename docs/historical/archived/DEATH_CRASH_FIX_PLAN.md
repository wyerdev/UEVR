# [HISTORICAL] UEVR Death/Transition Crash Fix — Architecture Plan

[2026-04] Failed plan, kept for reference only.

This plan failed and is kept for reference only. No implementation from this document is part of the current release path.

## Problem Statement

When playing High On Life (Oregon-Win64-Shipping) in VR with UEVR, the game crashes or hangs whenever the player dies. The render thread stops, VR goes black, and the game process either shows a crash dialog or hangs indefinitely.

This problem likely affects other UE games with heavy death/respawn transitions.

---

## Root Cause Analysis

### How UEVR Works (Normal Operation)

UEVR injects VR into UE games by:

1. **XR Device Nullification**: In `setup_view_extensions()`, UEVR nullifies the engine's `HMDDevice` and `XRSystem` pointers (`*(void**)potential_hmd_device = nullptr`). This prevents the engine from using a stale/uninitialized VR plugin.

2. **IXRTrackingSystem Replacement**: UEVR installs its own `IXRTrackingSystemHook` with a custom vtable that intercepts all XR calls and redirects them to UEVR's VR runtime (OpenXR/SteamVR).

3. **VEH Crash Handler**: Because the XR nullification propagates through engine code, many instructions try to dereference the null device pointer. A Vectored Exception Handler catches these access violations and patches them (xor reg + NOP) so they never fault again.

4. **Render Pipeline Hooks**:
   - `engine_tick_hook`: Inline hook on `UGameEngine::Tick` — runs game-thread logic, imgui frames
   - D3D12 Present hook: Triggers `on_frame_d3d12()` → `m_mods->on_present()` → `VR::on_present()` → `m_fake_stereo_hook->on_frame()` — the main render loop
   - RHI command vtable hook: Hijacks render thread commands to inject pose timing
   - Hook monitor thread: Detects when Present stops being called and can attempt rehooking

### What Happens During Death

When the player dies in High On Life, UE4 destroys and recreates game objects (actors, components, world context). During this transition:

1. **Game objects become null** temporarily on the render thread
2. Engine render code that references these objects crashes (access violations in game DLLs)
3. The HMD device pointer is still non-null (UEVR's XR system is alive) — so these are NOT from our XR nullification

### Why Our VEH Approach Failed

We attempted to extend the VEH handler to catch "transition crashes" (AVs where HMD is non-null but game objects are null). Over many iterations, every approach hit a wall:

| Approach | Result | Why It Failed |
|----------|--------|---------------|
| Per-instruction patching (xor+NOP) | Cascade of derived crashes | Zeroing one register corrupts the next 10 instructions |
| RtlVirtualUnwind (return null from function) | Caller throws C++ exception | Callers don't expect null return, throw `0xe06d7363` |
| Catching C++ exceptions during cascade | Still crashed | Some callers don't throw — they check null and gracefully exit the render loop |
| Multi-level unwind (8 frames deep) | Infinite hang | Unwound past the entire render pipeline — thread returned to wrong level |
| Cascade spillover (catch system DLL AVs) | Still crashed on death | Some deaths produce ZERO AVs — the game code checks for null objects and simply stops rendering |

### The Fundamental Problem

**The VEH handler operates at the wrong level of abstraction.** It tries to fix individual machine instructions, but the problem is architectural:

- **Problem**: Game objects are temporarily null during transitions, and the render thread accesses them
- **VEH solution**: Patch each crashing instruction as it faults
- **Reality**: There are UNLIMITED code paths that can fault. Some crash, some throw C++ exceptions, some silently return early. We can't catch them all.

Even if we caught every crash, **permanently patching instructions is destructive**. These instructions are correct during normal gameplay. Patching `mov rax, [rcx+0x28]` to `xor rax, rax` means the function always returns zero — even after respawn when objects are valid again. This corrupts rendering permanently.

---

## Findings Summary

### Death #1 (Transition crashes with RtlVirtualUnwind)
- 5 AVs caught and patched, then render thread died
- RtlVirtualUnwind returned null → caller threw C++ exception → thread killed
- C++ exception at `0x7ff82b0073fa` (KernelBase!RaiseException)

### Death #2 (Cascade threshold ≥3)
- Only 2 AVs caught (cascade threshold too high)
- 3rd AV had fault_addr > 0x10000, not classified as cascade → filtered → thread died

### Death #3 (Cascade threshold ≥1)
- 13 AVs caught and patched
- Crashes #1-6: RtlVirtualUnwind worked but all returned to same caller (iterator pattern)
- Crashes #7-12: No unwind info (leaf functions), fell back to zero+advance → cascade
- After patch #12, thread died

### Death #4 (Multi-level unwind, 8 frames)
- 1 AV caught, unwound 8 frames
- Returned to `7ff69df8af91` — way too high up the call chain
- Thread hung instead of crashing (returned into code that assumes render pipeline is set up)

### Death #5 (Single unwind + leaf fix)
- Crashed on injection (before death)
- 2 AVs caught, unwound correctly
- 3rd AV at `7FF82D842A96` in system DLL (cascade spillover) → not in game DLL → filtered → thread died

### Death #6 (With cascade spillover fix)
- **ZERO access violations during death**
- Game code checked for null objects, returned early
- Render thread simply stopped — no crashes, no exceptions, no AVs
- `on_frame` stopped being called; VEH handler had nothing to catch
- **This failure mode is fundamentally uncatchable by VEH**

---

## Architecture: Path Forward

### Strategy: Don't Fight the Crash — Survive the Transition

Instead of trying to patch individual crashing instructions, we should make UEVR's rendering pipeline survive the transition period gracefully.

### Option A: Null Guard at the Render Entry Point (RECOMMENDED)

**Concept**: Instead of catching crashes AFTER they happen, prevent them by detecting the transition state BEFORE the render thread enters dangerous code.

**How**:
1. **Detect the transition**: The game's `UWorld`, `UGameInstance`, or `APlayerController` become null during death. Monitor these in `engine_tick_hook` or `on_frame`.
2. **Suspend VR rendering**: When a transition is detected, temporarily skip the render pipeline (don't submit eye textures to OpenXR — just resubmit the last valid frame).
3. **Resume when objects return**: When the monitored objects become valid again, resume normal rendering.

**Implementation sketch**:
```cpp
// In engine_tick_hook or on_frame:
auto world = sdk::UEngine::get_first_local_player() ? ... : nullptr;
bool in_transition = (world == nullptr || player_controller == nullptr);

if (in_transition) {
    // Set a flag that VR::on_present() checks
    m_in_transition.store(true);
    // Resubmit last valid frame to OpenXR (keeps HMD alive, no judder)
    return;
}
m_in_transition.store(false);
```

**Pros**: Clean, no instruction patching, no VEH complexity, works for all failure modes
**Cons**: Need to identify the right UE objects to monitor; may briefly show stale frame

### Option B: Render Thread Try/Catch Wrapper

**Concept**: Wrap the render thread's entry point(s) in a structured exception handler (`__try/__except`) so that ANY crash during the render loop is caught and the iteration is skipped rather than killing the thread.

**How**:
1. The render thread enters UEVR code via the RHI command vtable hook (`hook_new_rhi_command`) and the D3D Present hook
2. Hook the game's render thread dispatch loop (or wrap our vtable-hijacked command) with `__try/__except`
3. On exception, log it, skip this frame, continue the loop

**Implementation locations**:
- `hooked_command_fn<N>()` — the vtable-hijacked RHI commands
- `VR::on_present()` — the Present-driven render path

**Pros**: Catches ALL exception types (AV, C++, etc.) without VEH
**Cons**: SEH can't catch all types cleanly on x64; may mask real bugs; doesn't prevent the underlying object destruction

### Option C: Freeze the Render Thread During Transitions

**Concept**: When the game thread detects a death/transition, temporarily suspend or gate the render thread so it doesn't touch game objects while they're being destroyed/recreated.

**How**:
1. Hook the game's death/transition mechanism (e.g., `UWorld::DestroyWorld`, `UGameInstance::Shutdown`, or the level streaming functions)
2. Set a flag that causes the render thread to spin-wait (or `SleepEx`)
3. Release when the new world/level is ready

**Pros**: Prevents all render-thread crashes by design
**Cons**: Requires game-specific hooks (may vary between UE versions); hard to get the timing right; may cause GPU stalls

### Option D: Replace XR Device Pointer with Safe Stub (Instead of Null)

**Concept**: Instead of nullifying the HMD device pointer (which is the root cause of ALL the VEH crashes), replace it with a stub object that has a full vtable of safe no-op functions.

**Currently**: `*(void**)potential_hmd_device = nullptr;` → engine code dereferences null → crash
**Proposed**: `*(void**)potential_hmd_device = &safe_stub;` → engine code calls stub → safe no-op return

**Note**: The commented-out code in `setup_view_extensions()` shows this was attempted:
```cpp
//**(void***)potential_hmd_device = replacement_vtable.data();
*(void**)potential_hmd_device = nullptr;
```
The replacement vtable approach was abandoned in favor of nullification + VEH. Revisiting it could eliminate the VEH handler entirely.

**Pros**: Eliminates VEH handler completely; no crashes at all during normal operation or transitions
**Cons**: Must handle ALL vtable entries for the specific UE version; some functions return complex types (SharedPtr, structs) that a simple `return nullptr` won't handle; Praydog chose null+VEH for a reason (likely compatibility across UE versions)

---

## Recommended Implementation Order

### Phase 1: Revert VEH Transition Code (Immediate)
Remove ALL transition crash handling code (escalation, cascade detection, RtlVirtualUnwind, C++ exception catching, cascade spillover). Keep only the original XR null-deref handling (praydog's baseline + our diagnostic counters). This stabilizes injection and normal gameplay.

### Phase 2: Implement Option A — Transition Detection + Render Skip
1. In `engine_tick_hook`, detect when key game objects go null
2. Set `m_in_transition` atomic flag
3. In `VR::on_present()`, check the flag. If set:
   - Skip `m_fake_stereo_hook->on_frame()`
   - Resubmit last valid eye texture to OpenXR
   - Continue calling `on_post_present` so the D3D hook stays alive
4. When objects return, clear the flag and resume

### Phase 3: Add SEH Wrapper as Safety Net (Optional)
Wrap the RHI command hook and on_present in `__try/__except` as a belt-and-suspenders measure. If any crash slips through detection, the render thread survives and retries next frame.

### Phase 4: Investigate Safe Stub (Long-term)
Research whether a full safe stub object for the XR device is feasible. This would eliminate both the VEH handler AND the transition problem, since the stub would handle null-object scenarios gracefully.

---

## Key Architecture Insight

The current architecture has a tension:

```
UEVR nullifies XR device → engine crashes → VEH catches and patches → permanent patches corrupt code
```

All the problems stem from **using null + crash-and-patch as the interface** between UEVR and the engine's XR subsystem. The VEH handler is clever but fundamentally fragile — it works for the known XR dereference paths but can't handle the combinatorial explosion of code paths during transitions.

The solution is to move the defense HIGHER in the stack:
- **Not**: catch crashes after they happen (VEH — reactive, incomplete)
- **Instead**: prevent the render thread from entering crashing code (transition detection — proactive, complete)

---

## Files to Modify

| File | Change |
|------|--------|
| `src/mods/vr/FFakeStereoRenderingHook.cpp` | Revert transition VEH code; add transition detection |
| `src/mods/VR.cpp` | Add transition flag check in `on_present()` |
| `src/mods/VR.hpp` | Add `m_in_transition` atomic member |
| `src/mods/vr/FFakeStereoRenderingHook.hpp` | Add transition detection state |

## UE Objects to Monitor for Transition Detection

These are the objects most likely to go null during death in UE games:
- `UWorld` (via `GEngine->GameViewport->GetWorld()`)
- `APlayerController` (via `UGameplayStatics::GetPlayerController()`)
- `APawn` (via `PlayerController->GetPawn()`)
- `ULocalPlayer` (via `UEngine::GetFirstLocalPlayer()`)

When any of these transition to null, we're in a death/level-load transition.
