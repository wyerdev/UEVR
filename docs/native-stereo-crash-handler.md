# Native Stereo Crash Handler — Technical Documentation

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

## Solution: Vectored Exception Handler with Multi-Stage Verification

A **Vectored Exception Handler (VEH)** is registered at UEVR startup. When any access violation occurs, the handler runs a verification pipeline to determine if the crash was caused by UEVR's XR nullification. If verified, it:

1. Fixes the CPU context so execution can continue safely
2. Permanently patches the faulting instruction so it never crashes again

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
│ Gate 1: Deduplication (handled_addresses│
│   set — O(1) lookup)                    │
│   Already processed? → CONTINUE_SEARCH  │
└────────────────┬────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────┐
│ Gate 2: Re-entrancy Guard               │
│   atomic<bool> handler_active           │
│   Prevents recursive stack-walk crashes │
└────────────────┬────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────┐
│ Gate 3: Basic Validation                │
│   Null RIP? IsBadReadPtr? Can decode?   │
│   Failures cached in handled_addresses  │
└────────────────┬────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────┐
│ Gate 4: Instruction Analysis            │
│   Decode with bddisasm                  │
│   Identify REG ← MEM pattern           │
│   Extract dest_reg for context fixup    │
└────────────────┬────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────┐
│ Gate 5: HMD Null Check                  │
│   Is engine->hmd_device actually null?  │
│   If not null → not our crash           │
└────────────────┬────────────────────────┘
                 │
                 ▼
        ┌────────────────────┐
        │ VERIFICATION STEPS │
        └────────┬───────────┘
                 │
    ┌────────────┼────────────────┐
    ▼            ▼                ▼
  Step A      Step B           Step C
 Backward    Stack Walk      Heuristic
 Reg Trace   + Forward Scan   Fallback
```

### Step A: Backward Register Trace

Walks **backward** up to 20 instructions from the crash site within the same function. Tracks which register produced the null value through register-to-register `mov` chains, looking for a memory load from an XR offset (`[reg + 0xe08/0xe18/0xe20]`).

**When it works:** The null dereference is close to the original XR pointer load — typically within 5-10 instructions.

**When it fails:** The null was passed as a function argument, stored in a local variable across a call boundary, or the load happened in a completely different function.

### Step B: Stack Walk + Forward Scan

Scans the stack for return addresses (up to 48 qwords, 8 unique callers). For each valid return address:

1. Verifies it's preceded by a `CALL` instruction
2. Finds the function start using `utility::find_function_start()`
3. Forward-scans up to 500 instructions looking for any memory operand referencing an XR offset

**When it works:** One of the callers in the chain originally loaded the XR pointer.

**When it fails:** The call chain is deeper than 8 unique callers, or the XR load happened in a function that has already returned and been popped from the stack.

### Step C: Heuristic Fallback

If Steps A and B fail, the handler applies a heuristic: **if our XR nullification is active AND the crash is in the game executable module**, it is overwhelmingly likely to be caused by our nullification.

This is sound because:
- The handler only activates when `engine->hmd_device == nullptr` (Gate 5)
- Crashes in the game module during XR nullification are almost always from propagated null dereferences
- A cap of **128 heuristic patches** prevents runaway patching

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

### For Other Instructions (e.g., `cmp [rax+0x8], rcx`)

1. **NOP the entire instruction** (replace all bytes with `0x90`)
2. **Advance RIP** past it

### Following CALL Suppression

If the instruction immediately after the patched one is a `CALL` (common pattern: load vtable pointer, then call through it), the CALL is also NOP'd to prevent calling through a null vtable.

## Safety Properties

### What Prevents False Positives?

1. **Gate 5 (HMD null check):** Handler only activates when UEVR has actually nullified the XR pointers
2. **Game module check:** Only patches crashes in the game executable, never in system DLLs or the OS
3. **Steps A & B:** Provide high-confidence causal verification when they succeed
4. **128-patch cap on heuristic:** Limits exposure from Step C
5. **One-shot semantics:** Each address is processed exactly once — all exit paths (successful patch, rejected by deep analysis, validation failure) insert into `handled_addresses` before returning

### What About Real Crashes?

| Scenario | Handler Behavior |
|----------|-----------------|
| Crash outside game module | Passed through (Gate: game module check) |
| HMD pointer is not null | Passed through (Gate 5) |
| Verified as non-XR by trace/stack walk | Passed through |
| Non-AV exception (stack overflow, heap corruption) | Logged and passed through (Gate 0) |
| AV in game module while HMD is null, but genuine bug | Potentially caught by heuristic — risk bounded by 128-patch cap |

Real crashes will still propagate normally, with one caveat: during active XR nullification, a genuine null-pointer bug in the game module *could* be incorrectly attributed to XR. This is an acceptable tradeoff because:
- Such bugs are rare during XR nullification (the engine is otherwise stable)
- The alternative is a guaranteed crash that makes the game unplayable
- The 128-patch limit prevents unbounded silent corruption

## Performance Characteristics

- **No per-frame cost:** The handler only runs on actual exceptions (hardware trap)
- **Once-per-address:** Each crash site is patched permanently; subsequent executions hit NOP/xor (zero overhead)
- **Typical session:** 9–30 patches total, all applied in the first few seconds of gameplay
- **Stack walk:** O(48 × 500) instruction decodes in worst case, but only on first occurrence of each crash site

## Current Status and Known Limitations

### Working

- Gameplay is stable — all XR null-deref crashes during normal gameplay are caught and patched
- No progressive stutter or frame drops from patched code
- Handler correctly passes through non-XR crashes

### Known Issue: Save/Load Crash

When loading a new save file, the game crashes after approximately 27 patches. The crash manifests as a **silent process termination** — the last log entry is "Crash handled successfully" with no further handler activity. Possible causes under investigation:

1. **Different exception type** not caught by Gate 0 (e.g., C++ exception, SEH from save system)
2. **Different thread** where the handler's re-entrancy guard is blocking processing
3. **State corruption** from NOP'd instructions that only manifests during save/load (different code path uses the same patched functions but expects non-null return values)
4. **Windows Error Reporting** or the CRT terminating the process before VEH runs

## Bug Fixes

### Rejected Address Caching (Calisto Protocol Fix)

**Problem:** The deep analysis rejection path and Gate 3 validation failures returned `EXCEPTION_CONTINUE_SEARCH` without inserting the exception address into `handled_addresses`. When a crash address was repeatedly rejected (e.g., a crash in a system DLL outside the game directory), the VEH handler would re-analyze the same address on every fault — an infinite loop that hung the game.

This was discovered because **The Callisto Protocol** crashed on startup: 108 AVs with the same RIP (`0x7ffd5ea42a96`, outside the game directory) hitting the handler repeatedly, with `rejected_not_xr` climbing and the game frozen.

Praydog's original handler cached addresses at the top (`ignored_addresses.insert(exception_address)` before any analysis), giving one-shot semantics. Our rewritten handler only inserted on the success/patch path.

**Fix:** Added `handled_addresses.insert(exception_address)` in three rejection paths:
1. Gate 3: `IsBadReadPtr` or null RIP → cache and `CONTINUE_SEARCH`
2. Gate 3: Instruction decode failure → cache and `CONTINUE_SEARCH`
3. Deep analysis rejection (`!verified`) → cache and `CONTINUE_SEARCH`

This ensures every address is processed at most once, matching the documented one-shot safety property.

## File Location

Handler code: `src/mods/vr/FFakeStereoRenderingHook.cpp`, starting at the `AddVectoredExceptionHandler` call (approximately line 3830).

## Dependencies

- **bddisasm:** x86 instruction decoder (NDR_RAX..NDR_R15, ND_OP_MEM, ND_OP_REG, ND_REG_GPR)
- **kananlib:** `utility::decode_one()`, `utility::resolve_instruction()`, `utility::find_function_start()`, `utility::get_executable()`, `utility::get_module_size()`
- **spdlog:** Logging with explicit flush at critical points
- **Patch::create():** UEVR's memory patching utility (handles VirtualProtect, stores original bytes for cleanup)
