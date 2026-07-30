---
description: Clean code and fork maintenance guidelines for UEVR-build (wyerdev fork of praydog/UEVR)
applyTo: '**/*.{cpp,hpp,h,c,toml}'
---

# UEVR-build Clean Code Guidelines

## Fork maintenance — minimize upstream diff

This repo is a fork of `praydog/UEVR`. The upstream is actively developed.
Every line we change in an upstream file is a future merge conflict. Keep the
diff surface as small as possible.

### Rules

1. **New functionality goes in new files.** Create your own `.cpp`/`.hpp` in a
   subdirectory (e.g. `src/mods/pluginloader/`, `include/uevr/`). These files
   are ours — we can edit them freely without merge risk.

2. **Upstream files get only glue.** When you must touch an upstream file
   (e.g. `PluginLoader.cpp`, `Framework.cpp`, `API.h`), limit the change to
   the absolute minimum:
   - One `#include` for your new header.
   - One function-pointer assignment or one-line call to bridge into your code.
   - A small data member addition if unavoidable.
   
   Do NOT refactor, reformat, reorder, or "improve" upstream code while you're
   there.

3. **Mark upstream edits.** When adding glue to an upstream file, add a brief
   comment so the change is easy to find and re-apply after a merge:
   ```cpp
   // [fork] preset system glue — see SettingsRegistry.hpp
   ```

4. **Never move or rename upstream files.** If an upstream file is in the wrong
   place for your taste, work around it.

5. **Never reformat upstream code.** No whitespace changes, no style fixes, no
   `clang-format` on upstream files. Format only your own new files.

6. **Shader plugins are self-contained.** Each plugin lives in its own
   `examples/<name>_plugin/` directory with its own `.cpp`, license file, and
   optional `assets/` folder. They link against the public plugin API only —
   no direct `#include` of upstream internals.

7. **Build system changes are additive.** New `[target.*]` sections in
   `cmake.toml` for new plugins — don't rewrite existing sections.

### Examples of the pattern

Good (new file + one-line glue):
```
// New file: src/mods/pluginloader/SettingsRegistry.cpp  — all logic here
// Upstream edit in PluginLoader.cpp: one #include + one call:
//   #include "pluginloader/SettingsRegistry.hpp"
//   uevr::settings_registry::apply_auto_preset();  // [fork]
```

Bad:
```
// Rewriting 200 lines of PluginLoader::on_initialize_d3d_thread() to add
// preset support inline — guaranteed merge conflict on next upstream update.
```

## Porting ReShade shaders

Shader plugins are 1:1 ports of existing ReShade `.fx` shaders. Accuracy is
non-negotiable — a wrong algorithm or incorrect math produces visually wrong
output that is hard to catch in review.

### Rules

1. **Always verify against the actual source code.** Before porting, find and
   read the original `.fx` file online. Never reconstruct the algorithm from
   memory, documentation, or descriptions. If you cannot find the source, say
   so — do not guess.

2. **Port the math exactly.** Every multiply, lerp, saturate, swizzle, and
   constant must match the original. Do not "simplify", "optimize", or
   "improve" the algorithm unless the user explicitly asks. A 1:1 port that
   looks ugly in C++ is better than a clean port that produces different output.

3. **Preserve all original parameters.** Default values, min/max ranges, and
   parameter names must match the original shader. If the original has
   `float Strength = 1.0` with range `[0.0, 1.0]`, the port must too.

4. **Credits and licensing must be verified.** Read the actual license file or
   header comment in the original repository. Do not assume a license — if the
   original has no explicit license, state that clearly. Always credit the
   original author(s) exactly as they appear in the source file.

5. **Include a license file per plugin.** Each plugin directory must contain a
   `*-LICENSE.txt` with the original shader's license text or attribution. This
   file is deployed alongside the DLL.

6. **Document the source URL.** The plugin's header comment must include the
   URL to the exact source file that was ported, so anyone can verify the port.