# stb image utilities (vendored)

Single-header PNG/JPG/etc. image loader by Sean Barrett.

- Upstream: https://github.com/nothings/stb
- File: `stb_image.h` (v2.30, fetched from `master`)
- License: dual-licensed **MIT OR Public Domain (Unlicense)** — full license text is preserved at the bottom of the header file. Either license alternative is compatible with this fork's MIT distribution.

`stb_image.h` decodes external textures (LUTs, lens dirt, etc.) for plugin shaders.
`stb_image_resize.h` is the public-domain code from stb commit
`e6afb9cbae4064da8c3e69af3ff5c4629579c1d2`, matching ReShade 4.2.1; comments
are stripped to follow this repository's attribution policy. It reproduces
ReShade's scene-sized external-texture conversion for exact ports.

Do not update `stb_image_resize.h` independently of the ReShade compatibility
contract in `docs/reference/shader-porting-guide.md`.
