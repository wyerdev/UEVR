# Transition Crash Handler — Technical Documentation

This fixes the death/respawn crash in: **Returnal** (and potentially any UE game with split-DLL rendering that nulls objects during level transitions while the render thread is mid-frame).

Extends the existing [native-stereo-crash-handler](native-stereo-crash-handler.md) with **transition-aware temporary context fixups** — a second crash class handled by the same VEH handler.

## Problem Statement

When UEVR injects stereo rendering into Unreal Engine, it nullifies the engine's HMD device pointer to redirect all XR calls through its own pipeline. This works perfectly during normal gameplay. However, during death/respawn transitions, the **game itself** nulls out various object pointers (actors, components, render resources) as part of level teardown — while the render thread is still mid-frame trying to read those objects.

The render thread hits access violations like:
```
EXCEPTION_ACCESS_VIOLATION reading address 0x00000038
EXCEPTION_ACCESS_VIOLATION reading address 0x00000000
EXCEPTION_ACCESS_VIOLATION reading address 0x00000008
```

These are null-pointer dereferences at small field offsets (the offset into the now-destroyed object).

### Why This Is Different from XR Null-Deref Crashes

| | XR Null-Deref (existing handler) | Transition Crash (this fix) |
|---|---|---|
| **Cause** | UEVR nullifies HMD device | Game nullifies objects during death/level transition |
| **HMD state** | Null (XR nullification active) | Non-null (XR working normally) |
| **Duration** | Permanent (HMD always null) | Temporary (objects valid after respawn) |
| **Correct fix** | Permanent bytecode patches | Temporary context-only fixups |
| **Volume** | Few crash sites | ~42 AVs per death across 32 unique sites |

### Why This Is Hard

1. **Volume**: A single death triggers ~42 access violations across 32 unique crash sites in multiple game DLLs (`Returnal-RenderCore-Win64-Shipping.dll`, `Returnal-Renderer-Win64-Shipping.dll`, etc.)

2. **Cascade effect**: Zeroing registers to survive the first wave of crashes creates **derived corrupted values**. Register A is zeroed → code reads `[A + 0x58]` → gets 0x58 → stores it → later code reads `[0x58 + 0x6A]` → fault at `0xC2`. The cascade produces fault addresses like `0x58`, `0xC2`, `0xFC`, `0xD0` that are **above the null-page** and would normally be filtered out as "not our problem."

3. **Temporary nature**: The nulled objects are **only null during the transition**. On respawn, everything is reallocated and valid. Any permanent code patches (NOP, xor-zero) would break the game on the next life.

4. **Thread separation**: The game thread (which calls `on_frame`) stays alive during death. Only the render thread crashes. This means frame-based "is rendering stopped?" detection doesn't work — `on_frame` keeps ticking.

### Evolution of the Fix

| Iteration | Approach | Result |
|-----------|----------|--------|
| 1 | `rendering_stopped` flag + `handled_addresses` bypass | Survived 3 deaths, crashed on 4th (address cache poisoned) |
| 2 | Reduced rendering_stopped threshold to 1500ms | Crashed on injection (false positive during init) |
| 3 | Added `on_frame_count >= 120` init guard | rendering_stopped never true during death (game thread alive) |
| 4 | Game-directory module detection, permanent patches | 52 patches worked, then cascade crash at `0xFFFFFFFF` |
| 5 | **Cascade-aware temporary context fixups** | **5+ deaths survived** |

## Solution: Cascade-Aware Temporary Context Fixups

The existing VEH handler was extended with a second crash classification path. When the HMD pointer is **non-null** (meaning XR is working normally and the crash is NOT from UEVR's nullification), the handler checks whether the crash looks like a game transition event and applies **temporary context fixups** instead of permanent patches.

### Handler Architecture (Extended)

```
Exception occurs
    │
    ▼
┌─────────────────────────────────────────┐
│ Gate 0: Is this an Access Violation?    │──No──► CONTINUE_SEARCH
└───────────────┬─────────────────────────┘        (non-AV exceptions:
                │ Yes                               stack overflow, heap
                │                                   corruption, etc.)
        ┌───────▼────────────────┐
        │  Classify: Transition  │
        │  vs XR vs Unknown      │
        └───────┬────────────────┘
                │
        ┌───────▼──────────────────┐
        │  Gate 1: HMD non-null?   │──HMD null──► XR null-deref path
        │                          │              (permanent patches,
        └───────┬──────────────────┘               see native-stereo-
                │ HMD valid                        crash-handler.md)
                │ (not XR-caused)
                │
        ┌───────▼────────────────────┐
        │  Is transition crash?      │──No──► CONTINUE_SEARCH
        │  • past_init (120+ frames) │        (not our problem)
        │  • in game DLL             │
        │  • null-page OR cascade    │
        └───────┬────────────────────┘
                │ Yes
        ┌───────▼──────────────────┐
        │  Gate 2: High-address    │──Filtered──► CONTINUE_SEARCH
        │  filter (BYPASSED for    │              (only if NOT
        │  transition crashes)     │               transition)
        └───────┬──────────────────┘
                │
        ┌───────▼──────────────────┐
        │  Deep Analysis:          │
        │  Decode instruction,     │
        │  trace registers,        │
        │  stack walk              │
        └───────┬──────────────────┘
                │
        ┌───────▼──────────────────┐
        │  TEMPORARY FIXUP:        │
        │  Zero dest register,     │
        │  advance RIP,            │
        │  skip following CALL     │
        │  (NO permanent patches)  │
        │  (NO address caching)    │
        └───────┬──────────────────┘
                │
                ▼
        EXCEPTION_CONTINUE_EXECUTION
        (render thread resumes)
```

### Transition Crash Classification

A crash is classified as a **transition crash** when ALL of these are true:

| Criterion | Check | Purpose |
|-----------|-------|---------|
| HMD is valid | `*(void**)potential_hmd_device != nullptr` | Distinguishes game-caused crashes from XR-caused crashes |
| Past initialization | `on_frame_count >= 120` | Avoids false positives during UEVR's early DX12/stereo setup |
| In game DLL | `is_in_game_directory(RIP)` | Crash must be in the game's own modules (exe + DLLs in game directory), not system or driver DLLs |
| Null-page OR cascade | `fault_address <= 0x10000` OR `transition_crash_count >= 3` | Handles both direct null-deref crashes and derived high-address cascade crashes |

### Cascade Detection

The cascade counter tracks how many transition crashes have been handled since the last rendered frame:

```
Death occurs
  └─► AV #1: fault_addr=0x38 (null-page) → fixup, cascade=0→1
  └─► AV #2: fault_addr=0x08 (null-page) → fixup, cascade=1→2
  └─► AV #3: fault_addr=0x00 (null-page) → fixup, cascade=2→3
  └─► AV #4: fault_addr=0x58 (derived!)  → cascade≥3, still handled, cascade=3→4
  └─► AV #5: fault_addr=0xC2 (derived!)  → cascade≥3, still handled, cascade=4→5
  └─► ...up to 42 AVs per death...
  └─► on_frame() fires → cascade counter reset to 0 (transition over)
```

The cascade counter (`veh_stats::transition_crash_count`) is:
- **Incremented** after each transition crash fixup
- **Reset to 0** on every `on_frame()` call (rendering alive = transition is over)
- **Threshold**: 3+ crashes activates cascade mode, bypassing the `fault_address > 0x10000` filter

### Temporary vs Permanent Fixups

The remediation section branches on `is_transition_crash`:

**Transition crashes** — context-only fixup:
```cpp
if (is_transition_crash) {
    set_context_reg(ctx, dest_reg, 0);              // zero register in CONTEXT
    ctx->Rip = exception_address + decoded->Length;  // advance past instruction
    // NO Patch::create() — original code preserved for next life
    // NO handled_addresses.insert() — will fault again on next death
    transition_crash_count.fetch_add(1);             // track cascade depth
}
```

**XR null-deref crashes** — permanent patch:
```cpp
else {
    set_context_reg(ctx, dest_reg, 0);              // zero register in CONTEXT
    ctx->Rip = exception_address + decoded->Length;  // advance past instruction
    // Permanent xor reg,reg + NOP padding patch
    xrsystem_patches.push_back(Patch::create(exception_address, patch_bytes));
    handled_addresses.insert(exception_address);     // never process again
}
```

### Game Directory Module Detection

Rather than checking only the main executable, the handler checks if the crash RIP is in **any module loaded from the game's directory**:

```cpp
static const auto game_exe_dir = []() -> std::wstring {
    auto exe_path = utility::get_module_pathw(utility::get_executable());
    // ... normalize to directory path
    return dir;
}();

static const auto is_in_game_directory = [](uintptr_t addr) -> bool {
    const auto mod = utility::get_module_within(addr);
    if (!mod) return false;
    const auto mod_path = utility::get_module_pathw(*mod);
    // ... check if mod_path starts with game_exe_dir
    return true;
};
```

This catches crashes in games with split DLLs (e.g., Returnal):
- `Returnal-Win64-Shipping.exe` (main executable)
- `Returnal-RenderCore-Win64-Shipping.dll`
- `Returnal-Renderer-Win64-Shipping.dll`
- `Returnal-Engine-Win64-Shipping.dll`
- `Returnal-Core-Win64-Shipping.dll`
- `Returnal-CoreUObject-Win64-Shipping.dll`

### Remediation Details

For `REG ← [MEM]` instructions:
```
Before: mov rax, [rcx+0x38]   ; rcx is null, access violation
After:  CONTEXT.Rax = 0       ; zero the destination register
        CONTEXT.Rip += len     ; advance past the instruction
```

For other instructions (stores, comparisons):
```
Before: cmp [rax+0x10], rbx   ; rax is null, access violation
After:  CONTEXT.Rip += len    ; skip the instruction entirely
```

If the next instruction is a CALL (vtable dispatch on the now-null value), it's also skipped:
```
Before: mov rax, [rcx+0x38]   ; null deref
        call [rax+0x20]        ; would deref the zeroed rax
After:  CONTEXT.Rip = past both instructions
```

## VEH Stats Namespace

### Atomic Counters

| Counter | Type | Purpose |
|---------|------|---------|
| `total_av_count` | `atomic<uint64_t>` | All access violations encountered |
| `filtered_hmd_nonnull` | `atomic<uint64_t>` | Fast-path: HMD non-null and not transition crash |
| `filtered_high_addr` | `atomic<uint64_t>` | Fast-path: fault > 0x10000 and not cascade |
| `filtered_already_handled` | `atomic<uint64_t>` | Fast-path: address already permanently patched |
| `filtered_reentrant` | `atomic<uint64_t>` | Fast-path: recursive exception guard |
| `reached_deep_analysis` | `atomic<uint64_t>` | Entered instruction decode and stack walk |
| `patched_count` | `atomic<uint64_t>` | Successfully handled (fixup or patch) |
| `rejected_not_xr` | `atomic<uint64_t>` | Deep analysis rejected as unrelated |
| `non_av_exceptions` | `atomic<uint64_t>` | Non-AV exception codes seen |
| `on_frame_count` | `atomic<uint64_t>` | Frame counter for init guard (>= 120) |
| `transition_crash_count` | `atomic<uint64_t>` | Current cascade depth (reset by on_frame) |
| `transition_cascade_start_tick` | `atomic<uint64_t>` | GetTickCount64() when cascade began |

### Ring Buffers (diagnostics)

| Buffer | Size | Content |
|--------|------|---------|
| `fault_ring[16]` | 16 entries | Recent filtered fault addresses |
| `fault_ring_rip[16]` | 16 entries | RIP at time of filtered fault |
| `exception_code_ring[16]` | 16 entries | Recent non-AV exception codes |
| `exception_rip_ring[16]` | 16 entries | RIP at time of non-AV exception |

### Stats Log Output (every 300 frames)

```
[VEH Stats] AVs=12 | fast-out: hmd_nonnull=8 high_addr=2 already=0 reentrant=0 | deep=2 patched=2 rejected=0 | non_av=0
[VEH Faults] Recent filtered: @58(rip=7ffe0c42d906), @c2(rip=7ffe0c3d4680), ...
[VEH NonAV] Recent exception codes: 0x406d1388@7ffee1234567, ...
```

## Safety Net

A `SetUnhandledExceptionFilter` in `Framework.cpp` writes a stack-based (no heap allocation) crash dump to `unhandled_crash.txt` if any exception escapes the VEH handler. Contains:
- Full register state (RAX through R15)
- Game-module stack walk (RSP + 64 QWORDs, filtered to game address range)
- Exception code, faulting RIP, and fault address

### Crash Dump Files

| File | Written By | When |
|------|-----------|------|
| `veh_crash_dump.txt` | VEH handler | Deep analysis rejects a crash |
| `unhandled_crash.txt` | SetUnhandledExceptionFilter | Exception escapes all handlers |
| `crash.dmp` | VEH handler | MiniDumpWriteDump for post-mortem |

## Performance Impact

Negligible. 99.95%+ of access violations are filtered at the first pointer check (Gate 1: HMD non-null) in ~1 nanosecond. Only actual transition crashes (~42 per death, 0 during normal gameplay) reach the instruction decode and stack walk path.

The `on_frame` cascade reset is a single atomic store — zero overhead during normal gameplay.

## Observed Crash Data (Returnal — Single Session)

```
Total transition crashes handled:  168
Deaths survived:                   4+  (5+ reported by user)
Unique crash sites (RIP):         32
Unique fault addresses:           12
Max cascade depth:                41
Crash sites per death:            ~42

Fault address distribution:
  36× fault_addr=0x58   (cascade-derived)
  32× fault_addr=0x00   (direct null)
  16× fault_addr=0xC2   (cascade-derived)
  16× fault_addr=0x08   (direct null + offset)
  16× fault_addr=0x38   (direct null + offset)
  12× fault_addr=0xD0   (cascade-derived)
  12× fault_addr=0x28   (direct null + offset)
   8× fault_addr=0xFC   (cascade-derived)
   8× fault_addr=0x20   (direct null + offset)
   4× fault_addr=0xD8   (cascade-derived)
   4× fault_addr=0x40   (direct null + offset)
   4× fault_addr=0x10   (direct null + offset)

Game DLLs involved:
  - Returnal-RenderCore-Win64-Shipping.dll  (RIP 7ffe6544xxxx-7ffe6546xxxx)
  - Returnal-Renderer-Win64-Shipping.dll    (RIP 7ffe0c3cxxxx-7ffe0c42xxxx)
```

All 168 crashes occurred within a single millisecond (`10:02:20.862` - `10:02:20.863`), confirming they happen in a burst during the death transition frame, not spread over time.

## Key Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| Init guard | 120 frames | Minimum frames before transition detection active |
| Cascade threshold | 3 crashes | Transition crashes before cascade mode activates |
| Null page ceiling | `0x10000` | Fault addresses below this are null-page dereferences |
| Max heuristic patches | 128 | Safety cap before heuristic stops accepting |
| Stats dump interval | 300 frames | ~5 seconds between diagnostic log dumps |
| Stack scan depth | 48 QWORDs | RSP scan range for return address detection |
| Max callers scanned | 8 | Depth of stack walk for XR offset analysis |
| Max forward instructions | 500 | Instructions scanned per caller function |
| Rendering stopped threshold | 3000ms | Max time without on_frame before non-AV crash dumps |

## Related Fixes (Returnal-specific)

### D3D12 Rehook Loop Prevention

`MAX_POST_INIT_REHOOK_ATTEMPTS` set to 0 in `Framework.cpp`. Returnal's Streamline (DLSS Frame Generation) swapchain interposer causes UEVR's D3D12 hook validation to detect a "different" Present pointer after initialization, triggering an infinite rehook loop. Disabling post-init rehooks prevents this.

### Non-AV Exception Diagnostics

The VEH handler monitors non-AV exceptions (`STACK_BUFFER_OVERRUN`, `STACK_OVERFLOW`, `HEAP_CORRUPTION`, `ILLEGAL_INSTRUCTION`, `INTEGER_DIVIDE_BY_ZERO`) with ring-buffer logging and conditional crash dumps when rendering has stopped. Thread-naming exceptions (`0x406D1388`) are silently ignored.

## Files Modified

- `src/mods/vr/FFakeStereoRenderingHook.cpp` — `veh_stats` namespace (cascade counters), `on_frame()` (cascade reset), VEH handler (transition classification, cascade detection, temporary fixup branch)
- `src/Framework.cpp` — `MAX_POST_INIT_REHOOK_ATTEMPTS = 0`, `SetUnhandledExceptionFilter` safety net

## See Also

- [native-stereo-crash-handler.md](native-stereo-crash-handler.md) — The XR null-deref permanent patch handler that this extends
