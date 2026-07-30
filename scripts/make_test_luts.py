"""Generate shipped LUT PNGs for the LUT plugin (Marty McFly 32-tile horizontal layout, 1024x32 RGBA8).

Outputs into `examples/lut_plugin/assets/`:
  - lut.png             : legacy default name (kept for backwards compatibility);
                          identity grade so first-launch shows no surprise tint.
  - lut_<preset>.png    : selectable presets enumerated by the plugin via
                          `enumerate_shader_assets(L"lut_", L".png")`. Filename
                          stem (after `lut_`) is what shows up in the UI combo,
                          word-cased ("warm_sunset" -> "Warm Sunset").

deploy.sh / install-plugins.bat ship everything in `examples/<plugin>/assets/`
into `<UEVR_root>/data/plugins/shader_assets/` so they appear under the global
preset list for every game.
"""
from PIL import Image
from pathlib import Path

W, H = 1024, 32  # 32 tiles, each 32x32

def make_lut(transform):
    img = Image.new("RGBA", (W, H))
    px = img.load()
    for tile in range(32):
        b = tile / 31.0
        for y in range(32):
            g = y / 31.0
            for x in range(32):
                r = x / 31.0
                rr, gg, bb = transform(r, g, b)
                px[tile * 32 + x, y] = (
                    int(round(max(0.0, min(1.0, rr)) * 255)),
                    int(round(max(0.0, min(1.0, gg)) * 255)),
                    int(round(max(0.0, min(1.0, bb)) * 255)),
                    255,
                )
    return img

def identity(r, g, b):
    return r, g, b

def warm(r, g, b):
    # Boost reds, lift greens slightly, suppress blues — visible warm/orange tint.
    return r ** 0.85, g ** 0.95, b ** 1.4

def cool(r, g, b):
    # Inverse of warm — push blues, suppress reds.
    return r ** 1.4, g ** 1.05, b ** 0.8

def cinematic(r, g, b):
    # Teal-and-orange: lift shadows toward teal, push highlights toward orange.
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    teal_orange = luma  # 0=shadow, 1=highlight
    rr = r * (0.9 + 0.25 * teal_orange) + 0.04 * (1.0 - teal_orange)
    gg = g * (0.95 + 0.05 * teal_orange) + 0.06 * (1.0 - teal_orange)
    bb = b * (0.85 - 0.15 * teal_orange) + 0.10 * (1.0 - teal_orange)
    return rr, gg, bb

def bleach(r, g, b):
    # Bleach-bypass: high contrast, desaturated.
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    contrast = (luma - 0.5) * 1.4 + 0.5
    return (r * 0.5 + contrast * 0.5,
            g * 0.5 + contrast * 0.5,
            b * 0.5 + contrast * 0.5)

PRESETS = {
    # `lut.png` is the legacy default name; shows up in the UI as "Default".
    # Identity grade so first-launch with Enabled=on shows no surprise tint.
    "lut.png":            identity,
    "lut_warm.png":       warm,
    "lut_cool.png":       cool,
    "lut_cinematic.png":  cinematic,
    "lut_bleach.png":     bleach,
}

if __name__ == "__main__":
    repo = Path(__file__).resolve().parent.parent
    dst = repo / "examples" / "lut_plugin" / "assets"
    dst.mkdir(parents=True, exist_ok=True)
    for name, fn in PRESETS.items():
        out = dst / name
        make_lut(fn).save(out)
        print(f"Wrote {out}")

