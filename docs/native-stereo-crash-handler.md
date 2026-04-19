# Native Stereo Crash Handler — Technical Documentation

This document covers the VEH-based Native Stereo crash handling path. The separate D3D12 load/transition hardening added later in `D3D12Component.cpp` is related, but it is outside the scope of this document.

This fixes the crashes in: Creatures of Ava
In the first room where we meet the NPC.

## Problem Statement

When UEVR activates **Native Stereo** mode, it nullifies the Unreal Engine's internal XR/HMD device pointers at specific offsets from the engine singleton:

| Offset | Field | Size |
|--------|-------|------|
| `engine + 0xe08` | `stereo_device` (TWeakPtr) | 16 bytes |
| `engine + 0xe18` | `hmd_device` (raw pointer) | 8 bytes |
| `engine + 0xe20` | `secondary_device` (raw pointer) | 8 bytes |

This is intentional — UEVR replaces the engine's XR pipeline with its own `FFakeStereoRendering` implementation. However, **engine code throughout the codebase still dereferences these pointers without null checks**, causing cascading access violations (exception code `0xC0000005`) that crash the game.

These crashes are not caused by a single callsite. The null pointer propagates through deep call chains — a function reads `engine->hmd_device`, stores the null in a local, passes it to another function, which eventually dereferences it. The crash site can be dozens of calls away from the original null load, and the accessed address may not even be in the null page (it can be a stale/garbage value derived from arithmetic on the null).

### Why a Simple Null Check Won't Work

- There is no single dereference site to patch — crashes occur at **many different locations** across the engine binary
- The offsets vary by UE version, so a static list of addresses is not viable
- The crash sites are in compiled engine code (not modifiable source), often inlined or optimized

## Crash Handler Modes

The VEH crash handler is configurable via the UEVR menu -> Debug, under **Stereo Hook Options → Crash Handler Mode** (requires restart to take effect):

| Mode | Value | Behavior |
|------|-------|----------|
| **Original (Nightly)** | 0 | Praydog's stock VEH handler from upstream UEVR. No transition crash handling, no ScopedScriptCall guard, no diagnostics. |
| **Enhanced (Experimental)** | 1 | Full enhanced handler as described below: transition crash support, ScopedScriptCall guard, rejected/temp-fixup caches, game-directory module detection. |
| **Enhanced (Experimental, Debug)** | 2 | Same as Enhanced, plus periodic VEH stats dumped to the log every ~5 seconds (300 frames). |
| **Disabled** | 3 | No VEH handler registered at all. Game will crash on any XR null dereference. For debugging only. |

Default is **Original (Nightly)** (mode 0). The architecture described in this document applies to modes 1 and 2 (Enhanced).

## Solution: Vectored Exception Handler with Multi-Stage Verification

A **Vectored Exception Handler (VEH)** is registered at UEVR startup. When any access violation occurs, the handler runs a verification pipeline to determine if the crash was caused by UEVR's XR nullification. If verified, it:

1. Fixes the CPU context so execution can continue safely
2. Permanently patches the faulting instruction so it never crashes again

> **Note:** The architecture below describes the **Enhanced** handler (modes 1–2). The Original handler (mode 0) uses praydog's upstream logic with a simpler pipeline.

### Handler Architecture

```
Exception occurs
    │
    ▼
┌─────────────────────────────────────────┐
│ Gate 0: Exception Type Filter           │
│   Only handle EXCEPTION_ACCESS_VIOLATION│
│   Log non-AV crash exceptions with flush│
└────────────────┬────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────┐
│ Gate 1: HMD + Transition Classification │
│   HMD valid? → only handle transition   │
│   crashes (game DLL + null-page/cascade)│
│   HMD null? → XR crash, continue        │
└────────────────┬────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────┐
│ Gate 2: Fault Address Filter            │
│   fault > 0x10000?                      │
│   Bypassed when HMD null (XR active) —  │
│   null-derived arithmetic can produce   │
│   any fault address (e.g. 0xFFFFFFFF)   │
│   Bypassed for transition cascades      │
└────────────────┬────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────┐
│ Gate 3a: handled_addresses              │
│   Already permanently patched?          │
│   → CONTINUE_SEARCH (code rewritten)    │
├─────────────────────────────────────────┤
│ Gate 3b: rejected_addresses             │
│   Already rejected as non-XR?           │
│   → CONTINUE_SEARCH (instant)           │
├─────────────────────────────────────────┤
│ Gate 3c: temp_fixup_cache               │
│   Dynamic-code fixup cached?            │
│   → Zero reg, advance RIP,             │
│     CONTINUE_EXECUTION (fast path)      │
└────────────────┬────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────┐
│ Gate 4: Re-entrancy Guard               │
│   atomic<bool> handler_active           │
│   Prevents recursive handler crashes    │
└────────────────┬────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────┐
│ Gate 5: Basic Validation + Decode       │
│   Null RIP? IsBadReadPtr? Can decode?   │
│   Failures cached in handled_addresses  │
│   Identify REG ← MEM pattern           │
│   Extract dest_reg for context fixup    │
└────────────────┬────────────────────────┘
                 │
                 ▼
        ┌────────────────────┐
        │ VERIFICATION STEPS │
        └────────┬───────────┘
                 │
    ┌────────────┼───────────┐
    ▼                        ▼
  Step A                  Step C
 Backward              Heuristic
 Reg Trace              Fallback
 (20 instr)        (game module check)
```

**Note:** Step B (stack walk + forward scan of up to 8 callers × 500 instructions) was
removed — it was the primary cause of stutter during crash cascades. See
[VEH Iteration History](veh-iteration-history.md) for details.

### Step A: Backward Register Trace

Walks **backward** up to 20 instructions from the crash site within the same function. Tracks which register produced the null value through register-to-register `mov` chains, looking for a memory load from an XR offset (`[reg + 0xe08/0xe18/0xe20]`).

**When it works:** The null dereference is close to the original XR pointer load — typically within 5-10 instructions.

**When it fails:** The null was passed as a function argument, stored in a local variable across a call boundary, or the load happened in a completely different function.

### Step B: Stack Walk + Forward Scan (REMOVED)

This step was removed because it was the primary cause of stutter during crash cascades. It decoded up to 8 functions × 500 instructions = 4,000 instruction decodes per VEH exception. With 45 crash sites discovered simultaneously, that was ~180,000 instruction decodes in a single frame. See [VEH Iteration History](veh-iteration-history.md) for the full analysis.

### Step C: Heuristic Fallback

If Step A fails, the handler applies a heuristic based on crash location:

**Game module crashes:** If our XR nullification is active AND the crash is in the game executable or any DLL in the game directory, it is overwhelmingly likely to be caused by our nullification. A cap of **128 heuristic patches** prevents runaway patching. Additionally, the heuristic is suppressed when inside a Lua script call (`uevr::g_is_in_script_call > 0`, tracked by `ScopedScriptCall` in the Lua API). AVs during script execution are more likely caused by bad Lua property access than XR nullification, so they are rejected and cached in `rejected_addresses` instead of permanently patched.

**Dynamic code crashes:** UE also runs code in dynamically allocated memory (Blueprint VM, JIT'd code). When HMD is null and a crash occurs at a non-module address, the handler applies a **temporary context fixup** (no permanent patch to heap memory). These are capped at **256 dynamic fixups** and the address is cached so deep analysis only runs once per site.

This is sound because:
- The handler only activates when `engine->hmd_device == nullptr` (Gate 1)
- Crashes in the game module during XR nullification are almost always from propagated null dereferences
- A cap of **128 heuristic patches** (game modules) and **256 dynamic fixups** (non-module code) prevents runaway patching

**When it fails:** Extremely rare — a genuine engine bug that coincidentally crashes while XR is nullified. These would be caught by the 128-patch limit.

## Remediation: How Crashes Are Fixed

### For `REG ← MEM` Instructions (e.g., `mov rax, [rcx+0x10]`)

1. **Context fixup:** Zero the destination register in `CONTEXT` so downstream null checks see zero
2. **Advance RIP** past the faulting instruction
3. **Permanent patch:** Replace the instruction with `xor reg, reg` + NOP padding

Example:
```
BEFORE: 48 8B 41 10    mov rax, [rcx+0x10]     ← crashes
AFTER:  48 31 C0 90    xor rax, rax; nop        ← returns 0, never faults again
```

### For Other Instructions (e.g., `cmp [rax+0x8], rcx`, `call [rax+0x30]`)

1. **Context fixup:** Zero RAX (for null-safety after skipped CALLs), advance RIP
2. **Permanent patch:** NOP the entire instruction (replace all bytes with `0x90`)

This is correct because these instructions always operate through the null XR pointer chain. Since UEVR keeps the XR pointers permanently null, the target memory is always unmapped.

### Proactive CALL Skip (REMOVED)

Earlier versions proactively NOP'd the CALL instruction following a patched load (e.g., `mov rax, [rcx]; call [rax+0x20]`). This was removed because it caused state corruption — the CALL may have had important side effects (state updates, lock releases) and NOP'ing it broke save/load and menu operations. Each instruction now faults independently and gets its own VEH invocation.

## Safety Properties

### What Prevents False Positives?

1. **Gate 1 (HMD null check):** Handler only activates for XR patches when UEVR has actually nullified the XR pointers
2. **Game module check:** Only patches crashes in game modules (executable and DLLs in the game directory), never in system DLLs or the OS
3. **Step A:** Provides high-confidence causal verification when it succeeds
4. **128-patch cap on heuristic:** Limits exposure from Step C
5. **One-shot semantics:** Each address is processed exactly once:
   - Permanently patched addresses → `handled_addresses` (CONTINUE_SEARCH, code rewritten)
   - Rejected addresses → `rejected_addresses` (CONTINUE_SEARCH, instant)
   - Transition addresses → re-analyzed each time (code is valid between transitions)
   - Dynamic-code addresses → `temp_fixup_cache` (CONTINUE_EXECUTION, fast path)
   - Validation failures → `handled_addresses` (CONTINUE_SEARCH)

### What About Real Crashes?

| Scenario | Handler Behavior |
|----------|-----------------|
| Crash outside game module | Passed through (Gate: game module check) |
| HMD pointer is not null | Passed through (Gate 1) |
| Verified as non-XR by backward trace | Passed through |
| Non-AV exception (stack overflow, heap corruption) | Logged and passed through (Gate 0) |
| AV in game module while HMD is null, but genuine bug | Potentially caught by heuristic — risk bounded by 128-patch cap |

Real crashes will still propagate normally, with one caveat: during active XR nullification, a genuine null-pointer bug in the game module *could* be incorrectly attributed to XR. This is an acceptable tradeoff because:
- Such bugs are rare during XR nullification (the engine is otherwise stable)
- The alternative is a guaranteed crash that makes the game unplayable
- The 128-patch limit prevents unbounded silent corruption

## Performance Characteristics

- **No per-frame cost:** Permanently patched addresses never fault again (code is rewritten)
- **Once-per-address deep analysis:** Each crash site goes through full decode/trace/heuristic once
- **Rejected addresses bypass instantly:** Separate `rejected_addresses` set — no re-analysis
- **Temp fixups are fast:** Dynamic-code addresses cached in `temp_fixup_cache`, ~3μs per fault
- **No sync disk I/O on success path:** `spdlog::flush()` removed from success path to prevent stutter
- **Step A only:** ~20 instruction backward scan (µs), no stack walk
- **Typical session:** 8–15 permanent patches, all applied in the first few seconds of gameplay

## Current Status (April 2026)

### Working

- Gameplay is stable — all XR null-deref crashes during normal gameplay are caught and patched
- No stutter or frame drops from the VEH handler
- Handler correctly passes through non-XR crashes
- Rejected system DLL addresses cached for instant bypass
- Menu open/close works (no NOP'd CALLs breaking state)

### Known Issue: System DLL Crash During Load Transitions

When loading a save file (especially the 3rd+ load in a session), the game intermittently crashes at a system DLL address (`0x7FFF118E2A96`, fault `0x7FFF0058`). This crash:

- Is **correctly rejected** by the VEH handler (not in game directory)
- Has **nothing to do with our permanent patches** (occurred with 0 patches and 14 patches alike)
- Is likely an XR runtime or driver issue during resource teardown/reload
- Happens in the same system DLL every time, suggesting a specific API call fails intermittently
- May be related to D3D Present hook loss ("Windows message hook still intact" loop follows)

This is **not fixable** in the VEH handler alone — it's outside our modules and outside our control.

Later work added conservative D3D12-side hardening in `D3D12Component.cpp` to better survive stale native render-target/resource access during load transitions. That improves some load scenarios, but it does not change the VEH classification logic described here.

### Known Issue: D3D Hook Loss During Load

After load transitions, UEVR sometimes loses its D3D Present hook. The log shows repeated "Windows message hook is still intact, ignoring..." and "Last chance encountered for hooking" messages. The game freezes (rendering stops). This is a pre-existing UEVR issue in the D3D hook recovery subsystem, separate from the VEH handler.

## Iteration History

See [VEH Iteration History](veh-iteration-history.md) for a detailed record of all VEH approaches tried, what worked, what failed, and why.

## Bug Fixes

### Rejected Address Caching (Calisto Protocol Fix)

**Problem:** The deep analysis rejection path and Gate 3 validation failures returned `EXCEPTION_CONTINUE_SEARCH` without inserting the exception address into `handled_addresses`. When a crash address was repeatedly rejected (e.g., a crash in a system DLL outside the game directory), the VEH handler would re-analyze the same address on every fault — an infinite loop that hung the game.

**Fix:** Added `rejected_addresses` set (separate from `handled_addresses`). Rejected addresses are cached for instant CONTINUE_SEARCH on subsequent faults. Validation failures are still cached in `handled_addresses`.

### High-Address Filter Bypass (Creatures of Ava Fix)

**Problem:** The `fault_address > 0x10000` filter rejected legitimate XR crashes where null-derived pointer arithmetic produced non-null-page fault addresses (e.g., `0xFFFFFFFFFFFFFFFF` from `[null + -1]`).

**Fix:** Added `&& hmd_is_valid` to the high-address filter. When HMD is null (XR nullification active), the filter is bypassed entirely — any fault address is accepted for further analysis.

## File Location

Handler code: `src/mods/vr/FFakeStereoRenderingHook.cpp`, starting at the `AddVectoredExceptionHandler` calls in `setup_view_extensions()`. Search for `AddVectoredExceptionHandler` — there are two registrations (original and enhanced), selected by `m_crash_handler_mode`.

## Dependencies

- **bddisasm:** x86 instruction decoder (NDR_RAX..NDR_R15, ND_OP_MEM, ND_OP_REG, ND_REG_GPR)
- **kananlib:** `utility::decode_one()`, `utility::resolve_instruction()`, `utility::find_function_start()`, `utility::get_executable()`, `utility::get_module_size()`
- **spdlog:** Logging, including rate-limited recovery diagnostics on noisy paths
- **Patch::create():** UEVR's memory patching utility (handles VirtualProtect, stores original bytes for cleanup)
