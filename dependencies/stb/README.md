# stb_image (vendored)

Single-header PNG/JPG/etc. image loader by Sean Barrett.

- Upstream: https://github.com/nothings/stb
- File: `stb_image.h` (v2.30, fetched from `master`)
- License: dual-licensed **MIT OR Public Domain (Unlicense)** — full license text is preserved at the bottom of the header file. Either license alternative is compatible with this fork's MIT distribution.

Used only by `examples/renderlib/effects/texture_loader.cpp` to decode external textures (LUTs, lens-dirt, etc.) for plugin shaders. Not exposed in any public plugin header.

To update: replace the file contents with a fresh copy from upstream `master`. Do not strip the trailing license block.
