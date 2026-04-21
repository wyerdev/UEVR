# Shader Port Candidates for UEVR VR Post-Processing

Evaluation of ReShade shaders worth porting as UEVR C++ plugins, prioritized for VR use cases.

## Already Ported (16 plugins)

| # | Shader | Category |
|---|--------|----------|
| 01 | LevelsPlus | Color Correction |
| 02 | LiftGammaGain | Color Correction |
| 03 | Tonemap | Color Correction |
| 04 | Curves | Color Grading |
| 05 | FakeHDR | Color Grading |
| 06 | DPX | Color Grading |
| 07 | Technicolor | Color Grading |
| 08 | Colourfulness | Color Grading |
| 09 | Vibrance | Color Grading |
| 10 | FilmGrain2 | Detail & Film Effects |
| 11 | HSLShift | Color Grading |
| 12 | FilmicPass | Color Grading |
| 13 | Clarity | Detail & Sharpening |
| 14 | CAS | Detail & Sharpening |
| 15 | LumaSharpen | Detail & Sharpening |
| 16 | Deband | Cleanup & Correction |

## Feasibility Analysis

### Portability Requirements
- Must be a **single-pass** pixel shader (our pipeline does one fullscreen triangle per plugin)
- Must NOT require **depth buffer** access (not available in pre-render callback)
- Must NOT require **lookup textures** (AreaTex, SearchTex, etc.)
- Must have a **permissive license** (MIT, BSD, or no-explicit-license with attribution)

### Evaluated Shaders

| Shader | Passes | Depth? | License | Portable? | Notes |
|--------|--------|--------|---------|-----------|-------|
| **Vignette** | 1 | No | CeeJay.dk (no explicit license) | **YES** | Simple UV-distance calculation |
| **ColorMatrix** | 1 | No | CeeJay.dk (no explicit license) | **YES** | 3x3 matrix multiply |
| ~~Deband~~ | 1 | Optional (off default) | MIT (Niklas Haas, ReShade port by JPulowski) | **PORTED** | Now plugin #16 |
| ~~Vibrance~~ | 1 | No | CeeJay.dk (no explicit license) | **PORTED** | Already plugin #09 |
| SMAA | 3-4 | Optional | iMMERSE: **Proprietary**; SweetFX: MIT (uses SMAA.fxh from iryoku/smaa) | **NO** | Multi-pass blocker: needs 3 intermediate RTs + stencil + embedded LUT textures |
| MXAO | Multi | **Required** | iMMERSE: **Proprietary** | **NO** | Depth required + proprietary |
| Bloom | Multi | No | iMMERSE: **Proprietary** | **NO** | Needs multi-pass downsample/blur mip chain |
| FXAA | 1 | No | NVIDIA public domain | Maybe | Single pass but complex; quality questionable without proper gamma handling |
| ChromaticAberration | 1 | Optional | prod80: MIT | Maybe | Inverse CA correction is non-standard; needs research |

### Conclusions

**Why these 16 shaders?** All 16 are purely mathematical — they operate on pixel colors using math (curves, thresholds, hashing, kernel sampling) with no external texture files. Many ReShade shaders require embedded lookup textures (LUTs, noise maps, area/search textures for AA), which our plugin pipeline doesn't support. Our architecture is a single fullscreen triangle pass per plugin: copy the scene RT to a staging SRV, run one pixel shader, write back. No intermediate render targets, no texture loading, no multi-pass. Every shader we ported works within that constraint.

**Not portable — blocked by architecture:**
- **SMAA**: Requires 3–4 render passes (edge detection → blend weight → neighborhood blending), a stencil buffer, and two embedded lookup textures (AreaTex 160x560, SearchTex 64x16). Our pipeline does one fullscreen triangle per plugin with no intermediate RTs. The MIT-licensed SweetFX version uses `SMAA.fxh` from iryoku/smaa which is BSD, but the multi-pass requirement is the real blocker — not the license.
- **MXAO**: Screen-space ambient occlusion requires depth buffer access. The depth buffer is not available in the UEVR pre-render callback. Also iMMERSE-proprietary licensed.
- **Bloom**: Requires a multi-pass downsample/upsample mip chain (typically 6+ passes). Cannot be done in a single fullscreen triangle. Also iMMERSE-proprietary licensed.

**Maybe — needs more investigation:**
- **FXAA**: Technically single-pass and NVIDIA public domain, but quality is questionable without proper linear/gamma handling, and the shader is complex. Lower priority since CAS already handles sharpening well.
- **ChromaticAberration (prod80)**: MIT licensed, single-pass. But the inverse CA correction mode is non-standard and needs research to verify it works without depth.

## Next Candidates

Remaining portable shaders that could be added in future batches:
- **Vignette**: Comfort vignette for VR locomotion sickness (trivially simple)
- **ColorMatrix**: Full 3x3 color transform for headset color correction

### Notes
- All plugins must support both DX11 and DX12 paths.
- Pipeline order matters: color correction → color grading → detail/sharpening/film effects.
- **CAS vs LumaSharpen**: CAS is the better general default for VR (TAA blur recovery + contrast boost, minimal artifacts). LumaSharpen is better for clean color-preserving sharpening. They can be stacked — CAS for contrast recovery, then light LumaSharpen for luma crispness.
