#!/usr/bin/env python3
"""Generate romfs/ui-skin-light.png from romfs/ui-skin-dark.png.

The 3DS skin is a 512x512 texture atlas whose region coordinates are
declared in source/ui_skin.c. We remap the neutral chrome to light
grays and hue-shift the gold selection/header accents toward light
blue, leaving region geometry and the alpha channel untouched so the
nine-slice renderer keeps working as-is.

Usage:  python3 scripts/generate_light_skin.py
"""

from PIL import Image
import numpy as np

SRC = "romfs/ui-skin-dark.png"
DST = "romfs/ui-skin-light.png"

# (name, x, y, w, h, mode)
#   'neutral'  — luminance remap [30,150] -> [208,250]
#   'select'   — gold -> light iOS row-selection blue, luminance preserved
#   'force'    — neutral remap regardless of saturation (overlay glows)
#   'header'   — gold top-edge -> light blue accent line, body -> neutral
REGIONS = [
    ("PANEL",           0,   0, 512, 256, "neutral"),
    ("BUTTON",          0, 272, 192,  80, "neutral"),
    ("BUTTON_ACTIVE", 208, 272, 192,  80, "neutral"),
    ("SELECTION",       0, 368, 256,  64, "select"),
    ("HEADER",        256, 368, 256,  64, "header"),
    ("PROGRESS",        0, 464, 256,  32, "force"),
    ("BUTTON_PRESSED",272, 448, 128,  64, "force"),
    ("FOOTER",        400, 448, 112,  64, "neutral"),
    # Dots (DOT_CYAN/GREEN/ORANGE ...) are monochrome alpha masks drawn
    # with a solid tint at runtime — leave them untouched.
]

SAT_THRESH = 40.0  # below this the pixel is treated as neutral chrome


def remap_region(rgb, mode):
    """rgb: float64 (H, W, 3) in [0,255]. Returns transformed rgb."""
    lum = (rgb[..., 0] + rgb[..., 1] + rgb[..., 2]) / 3.0
    mx = rgb.max(axis=-1)
    mn = rgb.min(axis=-1)
    sat = mx - mn

    if mode == "select":
        # Gold -> light iOS row-selection blue, preserving the vertical
        # gradient. Floor the luminance so dark feathered edges fade to
        # light instead of leaving a dark smudge.
        lumf = np.maximum(lum, 185.0)
        out = np.empty_like(rgb)
        out[..., 0] = np.minimum(255.0, lumf * 0.88)
        out[..., 1] = np.minimum(255.0, lumf * 0.95)
        out[..., 2] = np.minimum(255.0, lumf * 1.09)
        return out

    if mode == "header":
        # Warm/gold pixels (high saturation, red dominates blue) become a
        # light blue accent line; the rest go through the neutral remap.
        # Floor the luminance so dark gold feathers stay light blue too.
        warm = (sat >= SAT_THRESH) & (rgb[..., 2] < rgb[..., 0])
        lumf = np.maximum(lum, 170.0)
        out = np.empty_like(rgb)
        out[..., 0] = np.minimum(255.0, lumf * 0.81)
        out[..., 1] = np.minimum(255.0, lumf * 1.09)
        out[..., 2] = np.minimum(255.0, lumf * 1.54)
        n = neutral_remap(rgb, lum)
        out = np.where(warm[..., None], out, n)
        return out

    # neutral / force
    if mode == "force":
        # Glow overlays: uniform near-white shape; the runtime blue tint
        # defines the color, the preserved alpha defines the coverage.
        out = np.empty_like(rgb)
        out[..., 0] = out[..., 1] = out[..., 2] = 245.0
        return out

    chrome = sat < SAT_THRESH
    out = np.where(chrome[..., None], neutral_remap(rgb, lum), rgb)
    return out


def neutral_remap(rgb, lum):
    """Dark chrome [30,150] -> light chrome [208,250]; already-light -> 250.

    Output is a pure gray: scaling a non-neutral pixel amplifies its hue
    cast (a warm 28-saturation gray scales into visible yellow).
    """
    t = (lum - 30.0) / 120.0
    t = np.clip(t, 0.0, 1.0)
    new_lum = 208.0 + t * 42.0
    new_lum = np.where(lum >= 150.0, 250.0, new_lum)
    out = np.empty_like(rgb)
    out[..., 0] = out[..., 1] = out[..., 2] = new_lum
    return out


def main():
    img = Image.open(SRC).convert("RGBA")
    arr = np.asarray(img).astype(np.float64)  # (H, W, 4)
    rgb = arr[..., :3]
    for name, x, y, w, h, mode in REGIONS:
        sub = rgb[y:y + h, x:x + w].copy()
        sub = remap_region(sub, mode)
        rgb[y:y + h, x:x + w] = sub
        print(f"  remapped {name:14s} {w:3d}x{h:3d} mode={mode}")

    out = np.dstack((rgb, arr[..., 3])).astype(np.uint8)
    Image.fromarray(out, "RGBA").save(DST)
    print(f"Wrote {DST} ({img.width}x{img.height})")


if __name__ == "__main__":
    main()
