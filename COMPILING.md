# Compiling UEVR

## Necessary prerequisites

Your GitHub account must have access to the [EpicGames](https://github.com/EpicGames/) GitHub organization. If you do not have access, you will not be able to compile UEVR.

This is because the [UESDK](https://github.com/praydog/UESDK) submodule is a fork of the [EpicGames/UnrealEngine](https://github.com/EpicGames/UnrealEngine) repository. You must have an SSH key set up with your GitHub account and an SSH agent running in order to clone the UESDK submodule.

A C++23 compatible compiler is required. Visual Studio 2022 is recommended. Compilers other than MSVC have not been tested.

CMake is required.

## Compiling

###  Clone the repository

#### SSH
```
git clone git@github.com:praydog/UEVR.git
```

#### HTTPS
```
git clone https://github.com/praydog/UEVR
```

### Initialize the submodules

```
git submodule update --init --recursive
```

### Set up CMake

#### Command line

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build ./build --config Release --target uevr
```

#### VSCode

1. Install the [CMake Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) extension
2. Open the UEVR folder in VSCode
3. Press `Ctrl+Shift+P` and select `CMake: Configure`
4. When "Select a kit" appears, select `Visual Studio Community 2022 Release - amd64`
5. Select the desired build config (usually `Release` or `RelWithDebInfo`)
6. You should now be able to compile UEVR by pressing `Ctrl+Shift+P` and selecting `CMake: Build` or by pressing `F7`
### Building plugins

The 16 post-processing plugins are defined in `cmake.toml` and built alongside UEVR:

```
cmake --build ./build --config Release --target fakehdr_plugin
```

Or build all targets at once. Plugin DLLs are output to `build/Release/` with numeric prefixes (e.g. `05_FakeHDRShader.dll`).

To deploy plugins, licenses, and shipping presets:

```bash
bash deploy.sh
```

## Build-system gotchas

These are non-obvious failure modes that have cost real debugging cycles. Check them first when something compiles cleanly but behaves wrong at runtime.

### Stale `.obj` files when a public struct in a header changes

**Symptom:** the build reports `Build succeeded`, the DLL deploys, but at runtime the receiving side reads garbage out of a struct field — for example, an `int output` that should be `0` / `1` / `-1` reads back as `1952139316` (`0x74646174` = ASCII "tdat", i.e. fragments of bytes from a neighbouring field). No crash, no compiler warning, no link error.

**Root cause:** MSBuild's incremental dependency tracking does **not** reliably recompile every `.cpp` that `#include`s a header when only that header changes — particularly when the change is "added a field to a public struct." This produces an ABI mismatch within a single process: the producer translation unit (e.g. a plugin DLL) was compiled with the new struct layout, the consumer translation unit (e.g. a renderlib `.cpp` linked into `UEVRBackend.dll`) is still on the old layout. Field offsets diverge → silent data corruption at the call boundary.

This was hit during the Phase 3.6 cadence refactor: `examples/renderlib/effects/effect_runtime.hpp` gained a `Cadence` field on `PassDesc` and a `bool` field on `RTDesc`. The plugin DLL picked up the new layout. `effect_runtime_d3d12.obj` did not. Plugin wrote `p.output = 1`; backend read garbage; every pass after pass 0 was rejected with "invalid output id"; effect was silently invisible. Several debugging cycles were spent investigating logging, locking, TLS, and fmt parsing before checking obj timestamps.

**Detection:** any time you change a public struct in a header that is consumed across a DLL boundary (anything in `examples/renderlib/effects/`, anything in `include/uevr/`, anything in `src/mods/PluginLoader.hpp`'s public surface), compare timestamps after the build:

```bash
ls -la <changed-header>.hpp \
       build/<consumer-target>.dir/Release/*.obj
```

If the header is newer than the relevant `.obj` files, the build is broken regardless of what the build log said.

**Fix:** delete the stale objects and rebuild.

```bash
# Example: cadence refactor in effect_runtime.hpp
rm -f build/plugin_renderlib.dir/Release/effect_runtime*.obj
cmd.exe //c "build.bat"
```

Then re-deploy. `build.bat` does not do this for you.

A full `clean.bat` works but is wasteful — usually only the translation units that include the changed header from the same target miss the dependency edge. Plugin `.cpp` files in `examples/<plugin>/` appear to track headers correctly; the failure mode tends to be limited to translation units inside the same library/target as the changed header.

**Automated guard:** `build.bat` runs `scripts/check_stale_objs.ps1` before invoking MSBuild. The script finds the newest mtime across every first-party header in the repo (under `src/`, `include/`, `examples/`, `lua-api/`, `vr-plugin-nullifier/`, `side-projects/`) and, for every MSBuild target intermediate dir under `build/<target>.dir/<config>/`, deletes all `.obj` files in that dir if any of them is older than the newest header. Third-party targets that do not consume our headers (`glm`, `lua`, `imgui`, `spdlog`, `openvr`, `openxr_loader`, `safetyhook`, `sdkgenny`, `kananlib`, `asmjit`, `sdk-test`, `cmkr`, `uesdk`) are excluded by name so they don't get pointlessly rebuilt. **No per-header configuration is needed** — any change to any first-party header triggers the guard. If a new third-party target is added to `cmake.toml`, append its name to `$thirdPartyTargets` in the script.

**Cost of the guard:**
- No header touched: ~0.5–1s of file-stat overhead, no rebuilds — effectively free.
- One header touched: same scan, plus every first-party MSBuild target gets its `.obj` files invalidated and recompiled even if it doesn't transitively include the changed header. For an edit in `examples/renderlib/effects/`, that's ~16 plugin DLLs + `plugin_renderlib` + `uevr.dll` recompiled where MSBuild's tracking would have rebuilt only a handful of TUs. Cost: tens of seconds added to a "small change" rebuild.
- Clean build: no measurable overhead (no objs to scan).

**Future optimization (only if the over-rebuild becomes painful):** make the guard per-target precise instead of per-target wholesale. Two viable approaches, in order of complexity:

1. **Read MSBuild's `.tlog` files.** Each MSBuild target writes `build/<target>.dir/<config>/<target>.tlog/CL.read.1.tlog` containing the exact list of files each `.obj` depends on (from `/showIncludes` output). Parsing those gives a real header→obj graph, so the guard can delete only the objs whose dependency list contains a header newer than the obj itself. Same logic as MSBuild's own incremental tracking, run independently — catches the cases where MSBuild's tracking goes stale without falsely invalidating unrelated TUs.
2. **Scope by include graph statically.** For each first-party `.cpp`, run the preprocessor with `/showIncludes` once and cache the include set. Then on a guard pass, only invalidate TUs whose cached include set contains a modified header. More portable than `.tlog` parsing but requires running `cl.exe` over the source tree at least once.

Either approach would cut "edit one header, rebuild" from "all first-party targets" down to "just the TUs that actually include it" — i.e. roughly the speed MSBuild *should* have given us. Until rebuild time becomes a real friction point, the current wholesale-invalidation strategy is the right tradeoff: a few seconds of extra recompile per header edit in exchange for the ABI-mismatch class of bug being structurally impossible.

### Plugin-authoring pitfalls that look like build problems

These have all surfaced more than once and look at first glance like a build/deploy problem (silent failure, no crash) when in fact they're runtime API misuse.

| Pitfall | Symptom | Fix |
|---|---|---|
| **fmt double-parse in `Plugin::log_info`** | A log line containing a literal `{name}` produces no output at all. | `Plugin::log_info` runs the printf result through `spdlog::info("[Plugin] {}", str)`, which is fmt-style. Any literal `{...}` left in the formatted result is re-parsed as a placeholder; fmt throws and spdlog swallows it. **Never put `{` or `}` in `log_info` format strings or in any data you pass to it.** |
| **`static thread_local` state inside a renderer hook** | Per-frame state (caches, "did X change" flags) appears to reset randomly; resources get recreated every dispatch. | Native-stereo-fix games dispatch the renderer hook twice per frame, potentially from different threads. Each thread gets its own copy of any `static thread_local` variable and concludes the world looks "new." Use a regular instance member instead. |
| **Registering UEVR callbacks from inside a renderer hook** | Hard deadlock the first time the hook fires; game freezes at inject time, no crash dump. | `PluginLoader` holds a `shared_lock` on `m_api_cb_mtx` while iterating the callback list; `add_on_present` / `add_on_xr_*` etc. take a `unique_lock` on the same mutex. Register all callbacks from `Plugin::on_initialize()`, never from inside `on_pre_render_vr_framework_*` or any other callback. |