#!/usr/bin/env python3
"""Cook generated chroma sheets into runtime atlases and emit the manifest.

Masters in assets/graphics/source/ are immutable. Every runtime image is
produced from its master in ONE step — no derivative is ever resampled from
another derivative.

Sprites are located by connected component on non-chroma pixels rather than by
cropping a fixed grid, because generated sheets do not reliably land on the
grid they were asked for: the units-array sheet came back with irregular cell
sizes and offsets while the fixtures sheet gridded cleanly. Component
extraction handles both without per-sheet special cases.

Requires Pillow and NumPy.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "graphics" / "source"
ATLASES = ROOT / "assets" / "graphics" / "atlases"
BITMAPS = ROOT / "assets" / "graphics" / "bitmaps"
MANIFEST = ROOT / "assets" / "graphics" / "manifest.json"

CHROMA_TOLERANCE = 72      # distance in RGB space treated as background
DESPILL_STRENGTH = 0.85
MIN_COMPONENT_PIXELS = 900  # ignore specks and JPEG-ish noise


@dataclass(frozen=True)
class SheetSpec:
    sheet_id: str
    source: str
    columns: int
    rows: int
    cell: tuple[int, int]
    row_labels: tuple[str, ...]
    column_labels: tuple[str, ...]
    chroma: tuple[int, int, int]
    brief: str


SHEETS: tuple[SheetSpec, ...] = (
    SheetSpec(
        "units-array", "units-array.png", 3, 3, (32, 32),
        ("walker/shielded/breacher", "scout/splitter/suppressor",
         "repair/siege/controller"),
        ("left", "middle", "right"),
        (255, 0, 255),
        "Nine Array machine units, one per cell, silhouette-first."),
    SheetSpec(
        "fixtures-holdout", "fixtures-holdout.png", 3, 3, (32, 32),
        ("rail-spike/thermite-charge/entangler",
         "decoy-beacon/jammer-mast/emp-coil",
         "floodlight-array/block-workshop/spare"),
        ("left", "middle", "right"),
        (255, 0, 255),
        "Nine improvised resident fixtures built from household hardware."),
    SheetSpec(
        "fixtures-cordon", "fixtures-cordon.png", 3, 3, (32, 32),
        ("interdiction-turret/suppression-mortar/containment-field",
         "compliance-broadcast/aerial-interceptor/stun-net-emitter",
         "optical-sweep/sector-node/spare"),
        ("left", "middle", "right"),
        (255, 0, 255),
        "Nine machined Array emplacements, cold palette."),
    SheetSpec(
        "units-civilian", "units-civilian.png", 3, 3, (32, 32),
        ("resident/shield-line/barricade-crew",
         "camera-drone/scatter-group/shouter",
         "medic/convoy/assembly"),
        ("left", "middle", "right"),
        (255, 0, 255),
        "Nine civilian groups, warm ordinary palette."),
    SheetSpec(
        "terrain", "terrain-tiles.png", 4, 4, (16, 16),
        ("road", "pads-a", "pads-b", "props"),
        tuple(f"c{i}" for i in range(4)),
        (255, 0, 255),
        "Road, pad states, kerb and props."),
)


def chroma_mask(rgb: np.ndarray, chroma: tuple[int, int, int]) -> np.ndarray:
    """True where the pixel is background.

    Tests hue relationships rather than distance to an exact colour. Generated
    sheets dither their chroma field — the magenta backgrounds come back as a
    mottled texture spanning a wide RGB range — so a fixed tolerance around
    #FF00FF leaves most of the background opaque. What holds across the whole
    dithered field is the channel relationship: magenta keeps red and blue
    high and green suppressed.
    """
    r = rgb[:, :, 0].astype(np.int16)
    g = rgb[:, :, 1].astype(np.int16)
    b = rgb[:, :, 2].astype(np.int16)

    if chroma == (255, 0, 255):
        return (r > 90) & (b > 90) & (g < np.minimum(r, b) * 0.72)
    if chroma == (0, 255, 0):
        return (g > 90) & (r < g * 0.72) & (b < g * 0.72)

    target = np.array(chroma, dtype=np.int16)
    delta = rgb.astype(np.int16) - target
    return np.sqrt((delta.astype(np.float32) ** 2).sum(axis=2)) < CHROMA_TOLERANCE


def despill(rgb: np.ndarray, chroma: tuple[int, int, int]) -> np.ndarray:
    """Pull the chroma cast out of edge pixels that survived the key."""
    out = rgb.astype(np.float32).copy()
    if chroma == (255, 0, 255):          # magenta: clamp R and B toward G
        limit = out[:, :, 1] + (255.0 - out[:, :, 1]) * (1.0 - DESPILL_STRENGTH)
        for channel in (0, 2):
            excess = out[:, :, channel] > limit
            out[excess, channel] = limit[excess]
    elif chroma == (0, 255, 0):          # green: clamp G toward max(R,B)
        limit = np.maximum(out[:, :, 0], out[:, :, 2])
        limit = limit + (255.0 - limit) * (1.0 - DESPILL_STRENGTH)
        excess = out[:, :, 1] > limit
        out[excess, 1] = limit[excess]
    return np.clip(out, 0, 255).astype(np.uint8)


def components(mask: np.ndarray) -> list[tuple[int, int, int, int]]:
    """Bounding boxes of connected foreground regions, reading order.

    Iterative flood fill — a recursive one blows the stack on a 1024px sheet.
    """
    height, width = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    boxes: list[tuple[int, int, int, int]] = []

    for start_y in range(height):
        for start_x in range(width):
            if not mask[start_y, start_x] or seen[start_y, start_x]:
                continue
            stack = [(start_y, start_x)]
            seen[start_y, start_x] = True
            min_x = max_x = start_x
            min_y = max_y = start_y
            count = 0
            while stack:
                y, x = stack.pop()
                count += 1
                if x < min_x: min_x = x
                if x > max_x: max_x = x
                if y < min_y: min_y = y
                if y > max_y: max_y = y
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < height and 0 <= nx < width:
                        if mask[ny, nx] and not seen[ny, nx]:
                            seen[ny, nx] = True
                            stack.append((ny, nx))
            if count >= MIN_COMPONENT_PIXELS:
                boxes.append((min_x, min_y, max_x + 1, max_y + 1))
    return boxes


def attach_fragments(boxes: list[tuple[int, int, int, int]],
                     wanted: int) -> list[tuple[int, int, int, int]]:
    """Reduce components to exactly `wanted` subjects.

    A single subject is frequently several components — a tripod's legs, an
    antenna's guy wires, a machine's separated sensor masts all flood-fill
    apart, so a raw component count over-counts subjects badly (the fixtures
    sheet yielded 16 for 9). A pure proximity merge does not work either: the
    gap that correctly joins a tripod's legs to its body also chains adjacent
    sprites together once they nearly fill their cells (that same sheet
    collapsed to 1).

    What is reliable is the subject count, which the prompt fixes. Take the
    `wanted` largest components as seeds — a subject's body is always larger
    than its own detached fragments — then attach every remaining component to
    the nearest seed. This needs no gap threshold and no grid assumption, so it
    survives the irregular layouts generated sheets come back with.
    """
    if len(boxes) <= wanted:
        return list(boxes)

    def area(b: tuple[int, int, int, int]) -> int:
        return (b[2] - b[0]) * (b[3] - b[1])

    ranked = sorted(boxes, key=area, reverse=True)
    seeds = [list(b) for b in ranked[:wanted]]

    for box in ranked[wanted:]:
        cx = (box[0] + box[2]) / 2.0
        cy = (box[1] + box[3]) / 2.0
        best, best_d = 0, None
        for index, seed in enumerate(seeds):
            sx = (seed[0] + seed[2]) / 2.0
            sy = (seed[1] + seed[3]) / 2.0
            d = (cx - sx) ** 2 + (cy - sy) ** 2
            if best_d is None or d < best_d:
                best, best_d = index, d
        seed = seeds[best]
        seed[0] = min(seed[0], box[0])
        seed[1] = min(seed[1], box[1])
        seed[2] = max(seed[2], box[2])
        seed[3] = max(seed[3], box[3])

    return [tuple(seed) for seed in seeds]


def reading_order(boxes: list[tuple[int, int, int, int]]
                  ) -> list[tuple[int, int, int, int]]:
    """Row-major order, banding by vertical overlap rather than by a grid."""
    if not boxes:
        return []
    ordered = sorted(boxes, key=lambda b: (b[1], b[0]))
    bands: list[list[tuple[int, int, int, int]]] = []
    for box in ordered:
        placed = False
        for band in bands:
            top = min(b[1] for b in band)
            bottom = max(b[3] for b in band)
            centre = (box[1] + box[3]) / 2.0
            if top <= centre <= bottom:
                band.append(box)
                placed = True
                break
        if not placed:
            bands.append([box])
    bands.sort(key=lambda band: min(b[1] for b in band))
    result: list[tuple[int, int, int, int]] = []
    for band in bands:
        result.extend(sorted(band, key=lambda b: b[0]))
    return result


def fit_cell(sprite: Image.Image, cell: tuple[int, int]) -> Image.Image:
    """Scale to fit inside the cell preserving aspect, then centre it."""
    cw, ch = cell
    sw, sh = sprite.size
    scale = min(cw / sw, ch / sh)
    tw = max(1, int(round(sw * scale)))
    th = max(1, int(round(sh * scale)))
    resized = sprite.resize((tw, th), Image.LANCZOS)
    out = Image.new("RGBA", cell, (0, 0, 0, 0))
    out.paste(resized, ((cw - tw) // 2, (ch - th) // 2))
    return out


def cook(spec: SheetSpec, verbose: bool) -> dict | None:
    src = SOURCE / spec.source
    if not src.is_file():
        print(f"  skip {spec.sheet_id}: {src.name} not generated yet")
        return None

    image = Image.open(src).convert("RGB")
    rgb = np.array(image)
    background = chroma_mask(rgb, spec.chroma)
    cleaned = despill(rgb, spec.chroma)

    raw = components(~background)
    cells = reading_order(attach_fragments(raw, spec.columns * spec.rows))
    wanted = spec.columns * spec.rows
    filled = min(len(cells), wanted)
    if verbose:
        print(f"  {spec.sheet_id}: {len(raw)} components -> "
              f"{len(cells)} subjects, want {wanted}")
    if filled == 0:
        print(f"  FAIL {spec.sheet_id}: no sprites found after keying")
        return None

    cw, ch = spec.cell
    atlas = Image.new("RGBA", (spec.columns * cw, spec.rows * ch), (0, 0, 0, 0))
    rgba = np.dstack([cleaned, np.where(background, 0, 255).astype(np.uint8)])
    full = Image.fromarray(rgba, mode="RGBA")

    for index, box in enumerate(cells[:wanted]):
        sprite = full.crop(box)
        cell_image = fit_cell(sprite, spec.cell)
        col = index % spec.columns
        row = index // spec.columns
        atlas.paste(cell_image, (col * cw, row * ch))

    ATLASES.mkdir(parents=True, exist_ok=True)
    out_path = ATLASES / f"{spec.sheet_id}.png"
    atlas.save(out_path)

    return {
        "id": spec.sheet_id,
        "path": str(out_path.relative_to(ROOT)),
        "source": str(src.relative_to(ROOT)),
        "sha256": hashlib.sha256(out_path.read_bytes()).hexdigest(),
        "sha256_source": hashlib.sha256(src.read_bytes()).hexdigest(),
        "alpha_required": True,
        "grid": {
            "columns": spec.columns, "rows": spec.rows,
            "width": spec.columns * cw, "height": spec.rows * ch,
            "cell_width": cw, "cell_height": ch,
        },
        "row_labels": list(spec.row_labels),
        "column_labels": list(spec.column_labels),
        "sprites_found": filled,
        "prompt_brief": spec.brief,
    }


def cook_backdrop() -> dict | None:
    src = SOURCE / "maple-loop-ground.png"
    if not src.is_file():
        return None
    target = (480, 240)
    image = Image.open(src).convert("RGB").resize(target, Image.LANCZOS)
    out_dir = BITMAPS / "levels"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "maple-loop.png"
    image.save(out_path)
    return {
        "id": "maple-loop",
        "path": str(out_path.relative_to(ROOT)),
        "source": str(src.relative_to(ROOT)),
        "sha256": hashlib.sha256(out_path.read_bytes()).hexdigest(),
        "sha256_source": hashlib.sha256(src.read_bytes()).hexdigest(),
        "alpha_required": False,
        "grid": {"columns": 1, "rows": 1, "width": target[0],
                 "height": target[1], "cell_width": target[0],
                 "cell_height": target[1]},
        "prompt_brief": ("Ground cover only — lawns, houses, driveways, "
                         "fences. The lane is carried by terrain tiles so "
                         "painted road and authored map cannot disagree."),
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.check:
        if not MANIFEST.is_file():
            print("graphics: no manifest yet", file=sys.stderr)
            return 1
        data = json.loads(MANIFEST.read_text(encoding="utf-8"))
        bad = 0
        for entry in data.get("atlases", []) + data.get("bitmaps", []):
            path = ROOT / entry["path"]
            if not path.is_file():
                print(f"graphics: missing {entry['path']}", file=sys.stderr)
                bad += 1
                continue
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest != entry["sha256"]:
                print(f"graphics: {entry['path']} checksum drift",
                      file=sys.stderr)
                bad += 1
            with Image.open(path) as image:
                grid = entry["grid"]
                if image.size != (grid["width"], grid["height"]):
                    print(f"graphics: {entry['path']} is {image.size}, "
                          f"manifest says {grid['width']}x{grid['height']}",
                          file=sys.stderr)
                    bad += 1
        if bad:
            return 1
        print(f"graphics OK: {len(data.get('atlases', []))} atlases, "
              f"{len(data.get('bitmaps', []))} bitmaps")
        return 0

    print("cooking graphics")
    atlases = [entry for entry in
               (cook(spec, args.verbose) for spec in SHEETS) if entry]
    bitmaps = [entry for entry in (cook_backdrop(),) if entry]

    manifest = {
        "schema_version": 1,
        "game": "pleb-tower",
        "generated_at": "2026-07-28",
        "generator": {
            "name": "Google Gemini gemini-3-pro-image",
            "mode": "text-to-image from project-authored prompts",
            "postprocess": ("magenta chroma key, despill, connected-component "
                            "sprite extraction, aspect-preserving fit to an "
                            "exact cell grid"),
        },
        "provenance": {
            "original_project_material_only": True,
            "statement": ("All prompts were original Pleb Tower concepts "
                          "written for this project. No reference image from "
                          "any other work was supplied. This is a provenance "
                          "record, not a legal opinion."),
        },
        "atlases": atlases,
        "bitmaps": bitmaps,
    }
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"graphics: wrote {MANIFEST.relative_to(ROOT)} "
          f"({len(atlases)} atlases, {len(bitmaps)} bitmaps)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
