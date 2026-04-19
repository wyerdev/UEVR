# VEH Crash Handler — Iteration History

Records every approach tried during the Creatures of Ava VEH debugging session (April 2026), what worked, what failed, and why. **Consult this before making further VEH changes** to avoid repeating mistakes.

## The Original Problem

Creatures of Ava crashed at a specific point (meeting an NPC in the first room) after recent commits. The game had been working 2-3 days prior.

**Root cause:** The `fault_address > 0x10000` filter in the VEH handler rejected legitimate XR crashes where null-derived pointer arithmetic produced non-null-page fault addresses (e.g., `0xFFFFFFFFFFFFFFFF`).

## Iteration Timeline

### Fix 1: High-Address Filter Bypass ✅ COMMITTED

**Change:** Added `&& hmd_is_valid` to the `fault_address > 0x10000` gate so XR crashes (HMD null) bypass it entirely.

**Result:** Fixed the original Creatures of Ava crash. This fix is correct and committed.

### Fix 2: Dynamic Code Handling (Heap/JIT Crashes) — Original Form ❌ REMOVED, Simpler Form Re-added

**Change:** Added `is_dynamic_xr_crash` flag — when HMD is null and crash is at a non-module address (Blueprint VM, JIT), apply temporary context fixup. Originally capped at 64 dynamic fixups.

**Result:** Game ran handling 35 crashes, then crashed during save game load. The temp fixups were firing VEH exceptions every frame forever, and skipping individual instructions in dynamic code left corrupted registers that cascaded into fatal crashes.

**Current status:** The original aggressive form was removed and replaced with a simpler version: non-module crashes when HMD is null are classified as `is_dynamic_xr_crash`, receive a temporary context fixup (not permanent patch), and are cached in `temp_fixup_cache`. Capped at **256 entries**. This simpler form avoids the cascade issues because it doesn't attempt deep analysis or function-level bailouts.

**Lesson:** Don't try to handle dynamic code crashes by skipping individual instructions. The instruction stream in heap memory is unpredictable. Simple temp fixups with caching are the sustainable approach.

### Fix 3: Bad Call Target Handler ❌ REMOVED

**Change:** When `RIP == fault_address` (CPU tried to execute code at the fault address itself — a bad function pointer call), NOP the CALL instruction at the call site.

**Result v1 (permanent NOP):** Broke save/load — the CALL was on a shared code path used by both XR and non-XR callers. NOP'ing it removed the function call for everyone.

**Result v2 (temp only, no permanent NOP):** Slightly more stable but still fragile. Random crashes after 46 handled exceptions.

**Lesson:** Never permanently NOP a CALL instruction based on a single failure. The same CALL may serve multiple callers with valid pointers.

### Fix 4: Dynamic Code Function Bailout ❌ REMOVED

**Change:** For crashes in dynamic code, walk RSP to find a return address in the game module and bail out to it (set RAX=0, RIP=return_addr, RSP past the return address).

**Result:** Crashed on inject — system DLL exception at `0x7FFF118E2A96` during early init was classified as dynamic code and bailed out of.

**Lesson:** Early-init exceptions in system DLLs look the same as dynamic code crashes (non-module address, HMD null). The `past_init` guard was added to fix this, but the whole approach was fundamentally fragile.

### Fix 5: past_init Guard ✅ KEPT (but not sufficient alone)

**Change:** Added `past_init` check (120+ frames) to both bad-call-target and dynamic-code handlers.

**Result:** Injection crash fixed, but game crashed when opening menu — 45+ permanent patches applied during a menu-triggered crash burst, corrupting game state.

**Lesson:** The menu open triggers many new crash sites at once. Permanently patching all of them during a cascade is dangerous.

### Fix 6: Hard Cap on Permanent Patches (MAX=16) ❌ BAD APPROACH

**Change:** Cap permanent patches at 16. Beyond that, use temp fixups cached in `temp_fixup_cache`.

**Result:** Massive stutter — temp-fixup addresses fire VEH exceptions **every single frame** (kernel transition each time). With 30+ temp-fixup sites, that's ~30 kernel transitions per frame. Gets progressively worse as more sites accumulate.

**Lesson:** Any address that faults every frame MUST be permanently patched. Temp fixups are only sustainable for addresses that fault rarely (transitions).

### Fix 7: Rate Limiter + Promotion ❌ BAD APPROACH

**Change:** Replace hard cap with rate limiter (max 1 permanent patch per 50ms). Temp-fixup cache entries get promoted to permanent patches gradually when the burst settles.

**Result:** Same stutter — promotion was too slow. 30+ sites stuttering for seconds before getting promoted.

**Lesson:** Rate-limiting permanent patches solves the wrong problem. The cascade already happened; throttling the fix just prolongs the pain.

### Fix 8: Remove Step B (Stack Walk) ✅ MAJOR FIX

**Change:** Removed the stack walk + forward scan verification step entirely.

**Result:** Eliminated the primary stutter source. Step B decoded up to 8 callers × 500 instructions = **4,000 instruction decodes per VEH exception**. With 45 crash sites, that was ~180,000 instruction decodes in one frame.

**Why Step B was unnecessary:** Step C (heuristic: "is it in a game module? → treat as XR") catches everything Step B caught. Step B just added expensive verification before arriving at the same conclusion. Praydog's original handler never had Step B.

**Lesson:** Verification is not free. The cost per VEH invocation matters enormously when dozens of sites are discovered simultaneously.

### Fix 9: Remove spdlog::flush() from Success Path ✅ KEPT

**Change:** Removed `spdlog::default_logger()->flush()` from the successful crash-handling path.

**Result:** Eliminated synchronous disk I/O on every VEH invocation. With 45 crashes, that was 45 disk flushes in one frame.

**Lesson:** Never do sync I/O in a VEH handler's hot path. Logs flush on their own schedule.

### Fix 10: Non-REG-dest Temp Fixups Only ❌ BAD APPROACH

**Change:** Only permanently patch REG-dest instructions (xor reg,reg is semantically correct). CMP, CALL, and store instructions get temp fixups only.

**Rationale:** NOP'ing CMP removes flag-setting (wrong branches). NOP'ing CALL removes function execution (deadlocks).

**Result:** Stutter returned — the CMP and CALL instructions fault every frame forever (same as Fix 6). The `already=2881` in the VEH stats showed ~500 temp-fixup VEH exceptions per second.

**Lesson:** ALL game-module XR crash sites fault every frame forever (the XR pointer is always null). They ALL need permanent patches. A `CMP [null_chain]` will never successfully read memory. A `CALL [null_chain]` will never call a valid function. NOP is correct for all of them.

### Fix 11: Rejected Address Caching ✅ KEPT

**Change:** Added separate `rejected_addresses` set. Previously rejected addresses were either:
- Added to `handled_addresses` (Fix pre-11): caused re-entry loop since CONTINUE_SEARCH on a permanently-cached address still faults
- Not cached at all (attempted fix): caused 7,271 deep analyses of the same system DLL address

**Result:** System DLL address at `0x7FFF118E2A96` is rejected once, then instantly bypassed on all subsequent faults.

**Lesson:** Rejected addresses need their own cache, separate from patched addresses.

### Fix 12: Remove Proactive CALL Skip ✅ KEPT

**Change:** Removed the code that looked ahead at the next instruction and pre-emptively skipped CALL instructions following a patched load.

**Result:** Menu open/close and save/load became more stable. Each instruction now faults independently and gets its own VEH invocation with its own permanent patch.

**Lesson:** Never NOP an instruction that hasn't faulted. If a CALL through a zeroed base register faults, it gets its own VEH exception and its own patch. Proactive patching removes instructions with unknown side effects.

## Current Architecture (Post All Fixes)

```
Exception occurs
    │
    ├─ Not AV? → CONTINUE_SEARCH
    ├─ HMD valid + not transition? → CONTINUE_SEARCH
    ├─ High addr + HMD valid + not cascade? → CONTINUE_SEARCH
    ├─ In handled_addresses? → CONTINUE_SEARCH (permanently patched)
    ├─ In rejected_addresses? → CONTINUE_SEARCH (rejected, instant)
    ├─ In temp_fixup_cache? → fast fixup + CONTINUE_EXECUTION
    ├─ Re-entrant? → CONTINUE_SEARCH
    │
    ▼
Deep Analysis (once per address):
    Step A: Backward trace (20 instructions, cheap)
    Step C: Heuristic (game module check)
    │
    ├─ Transition crash → temp fixup + cache
    ├─ Dynamic code → temp fixup + cache
    ├─ Game module XR → PERMANENT PATCH + handled_addresses
    └─ Rejected → rejected_addresses + CONTINUE_SEARCH
```

**Key design principle:** Every address that faults every frame MUST get a permanent patch. Temp fixups trigger VEH exceptions on every fault (~10μs each, but kernel transitions aggregate into visible stutter at scale).

## Remaining Issues NOT Fixable in VEH

### System DLL Crash at 0x7FFF118E2A96

- Occurs intermittently during 3rd+ save load
- Fault address 0x7FFF0058 — deep in system/XR runtime code
- VEH correctly rejects it (not in game directory)
- The game process terminates — no handler (VEH or SEH) catches it
- Occurred with 0, 10, and 14 patches — not correlated with our patching
- Likely an XR runtime bug during resource teardown/reload

### D3D Present Hook Loss

- After load transitions, UEVR sometimes loses its D3D Present hook
- Log shows "Windows message hook is still intact" + "Last chance encountered for hooking" in a loop
- Game appears frozen (rendering stops)
- Separate UEVR subsystem issue, not related to VEH handler

### What Would Fix These?

1. **System DLL crash:** Investigate the XR runtime crash (OpenXR loader? GPU driver?). May need to handle the rejected address by context-fixing it instead of passing through. Or may need to catch a non-AV exception type.

2. **D3D hook loss:** The D3D hook re-acquisition logic in Framework.cpp needs investigation. The `Reached max D3D rehook attempts (0)` message suggests the rehook counter is already exhausted or disabled.

## Post-VEH Follow-Up: D3D12 Hardening

After the VEH work hit a limit, the release path moved to conservative D3D12 hardening in `src/mods/vr/D3D12Component.cpp` instead of more VEH experimentation. See [ATTEMPT_3_LOAD_CRASH_FIX_PLAN.md](ATTEMPT_3_LOAD_CRASH_FIX_PLAN.md) for the full plan and results.

Implemented:
- safe native-resource lookups for `FRHITexture2D` targets
- safe `GetDesc()` reads for `ID3D12Resource`
- invalidation and rebuild when UE swaps native scene or scene-capture resources
- reset of native-resource tracking on `on_reset()`
- rate-limited invalidation logs to avoid user log spam

Result:
- better resilience to stale-resource failures during some load/transition windows
- no evidence that the D3D12 hardening caused the rare cutscene-start crash investigated later
- no justification for more speculative VEH changes in this release
