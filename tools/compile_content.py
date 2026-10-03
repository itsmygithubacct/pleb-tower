#!/usr/bin/env python3
"""Compile content/campaigns.json into build/content_generated.h.

The game-local compiler owns game rules. It validates strictly and fails the
build rather than warning. Schema and the full rule list:
~/research/gpu_terminal/games/pleb-tower/engineering/CONTENT_SCHEMA.md

Standard library only. Python 3.10+.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTENT = ROOT / "content" / "campaigns.json"
STABLE_IDS = ROOT / "content" / "stable_ids.json"

SCHEMA_VERSION = 2
TIER_COUNT = 3
ROLES = ("rapid", "artillery", "control", "reroute",
         "antiair", "disable", "vision", "support")
DAMAGE_TYPES = ("none", "impact", "pierce", "splash")
TARGET_CLASSES = ("none", "ground", "air", "ground_air")
ATTACK_KINDS = ("none", "fixture", "passing")

ROLE_FIELDS = {
    "artillery": ("splash_radius",),
    "control": ("hold_seconds",),
    "reroute": ("pull_cells",),
    "disable": ("stun_seconds",),
    "vision": ("enemy_range_penalty", "enemy_accuracy_penalty"),
    "support": ("damage_buff", "range_buff", "currency_tick",
                "repair_per_second", "radius"),
}
ALL_ROLE_FIELDS = {f for fields in ROLE_FIELDS.values() for f in fields}

EXPECTED_WAVES = {"holdout": 15, "cordon": 12}


class ContentError(Exception):
    pass


def fail(rule: int, message: str) -> None:
    raise ContentError(f"rule {rule}: {message}")


# --------------------------------------------------------------- loading --

def load() -> dict:
    try:
        raw = CONTENT.read_text(encoding="utf-8")
    except OSError as exc:
        raise ContentError(f"cannot read {CONTENT}: {exc}") from exc
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ContentError(f"{CONTENT} is not valid JSON: {exc}") from exc


# ------------------------------------------------------------ validation --

def validate_structural(doc: dict) -> None:
    if doc.get("schema_version") != SCHEMA_VERSION:
        fail(2, f"schema_version must be exactly {SCHEMA_VERSION}")
    if doc.get("game") != "pleb-tower":
        fail(1, "game must be 'pleb-tower'")
    campaigns = doc.get("campaigns")
    if not isinstance(campaigns, list) or len(campaigns) != 2:
        fail(4, "exactly two campaigns required")
    roots = [c for c in campaigns if c.get("unlocked_by") is None]
    if len(roots) != 1:
        fail(4, "exactly one campaign must have unlocked_by: null")
    ids = [c["id"] for c in campaigns]
    if len(set(ids)) != len(ids):
        fail(1, "duplicate campaign ids")
    for c in campaigns:
        unlock = c.get("unlocked_by")
        if unlock is not None and unlock not in ids:
            fail(1, f"campaign {c['id']} unlocked_by unknown id {unlock!r}")
    maps = doc.get("maps")
    if not isinstance(maps, list) or not maps:
        fail(1, "at least one map is required")
    # Slots are part of the versioned records format. New catalog entries need
    # a save migration as well as content, so reordering cannot erase scores.
    if [m["id"] for m in maps] != ["maple-loop", "rail-yard"]:
        fail(1, "map catalog order must be maple-loop, rail-yard for records v2")
    for m in maps:
        if (m["columns"], m["rows"], m["cell_pixels"]) != (30, 15, 16):
            fail(5, "maps must use the 30 x 15, 16-pixel board")
        if sorted(p["id"] for p in m["pads"]) != list(range(1, len(m["pads"]) + 1)):
            fail(8, "pad ids must be contiguous and start at one")
        if not 1 <= len(m["pads"]) < 255:
            fail(8, "map must contain 1..254 pads")
        for pad in m["pads"]:
            if pad.get("tags"):
                fail(3, f"pad {pad['id']} tags must be empty")
        for cid, override in m.get("campaigns", {}).items():
            if cid not in ids or set(override) - {"waves", "starting_currency"}:
                fail(1, f"invalid campaign override in map {m['id']}")
    for c in campaigns:
        for f in c["fixtures"]:
            if f.get("branch") is not None:
                fail(3, f"fixture {f['id']} branch must be null in 1.0")
        for w in c["waves"]:
            if w.get("hp_scale") != 1.0:
                fail(3, f"{c['id']} wave {w['index']} hp_scale must be 1.0")


def rasterise_lane(m: dict) -> set[tuple[int, int]]:
    cols, rows = m["columns"], m["rows"]
    cells: set[tuple[int, int]] = set()
    for seg in m["lane"]:
        x0, y0 = seg["from"]["x"], seg["from"]["y"]
        x1, y1 = seg["to"]["x"], seg["to"]["y"]
        if x0 != x1 and y0 != y1:
            fail(5, f"lane segment {seg} is not axis-aligned")
        for (x, y) in ((x0, y0), (x1, y1)):
            if not (0 <= x < cols and 0 <= y < rows):
                fail(5, f"lane segment {seg} leaves the grid")
        if x0 == x1:
            for y in range(min(y0, y1), max(y0, y1) + 1):
                cells.add((x0, y))
        else:
            for x in range(min(x0, x1), max(x0, x1) + 1):
                cells.add((x, y0))
    return cells


def connected(cells: set[tuple[int, int]], start: tuple[int, int]) -> set:
    seen = {start}
    queue = deque([start])
    while queue:
        cx, cy = queue.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nxt = (cx + dx, cy + dy)
            if nxt in cells and nxt not in seen:
                seen.add(nxt)
                queue.append(nxt)
    return seen


def validate_map(doc: dict) -> set[tuple[int, int]]:
    m = doc["map"]
    lane = rasterise_lane(m)
    endpoints = m["endpoints"]
    for name, pt in endpoints.items():
        cell = (pt["x"], pt["y"])
        if cell not in lane:
            fail(6, f"endpoint {name} {cell} is not a lane cell")
    first = next(iter(endpoints.values()))
    reach = connected(lane, (first["x"], first["y"]))
    if reach != lane:
        fail(6, "lane segments do not form one connected set")
    for name, pt in endpoints.items():
        if (pt["x"], pt["y"]) not in reach:
            fail(7, f"endpoint {name} unreachable from the lane")

    seen_cells: set[tuple[int, int]] = set()
    seen_ids: set[int] = set()
    for pad in m["pads"]:
        cell = (pad["x"], pad["y"])
        if pad["id"] in seen_ids:
            fail(1, f"duplicate pad id {pad['id']}")
        seen_ids.add(pad["id"])
        if cell in lane:
            fail(8, f"pad {pad['id']} {cell} lies on a lane cell")
        if cell in seen_cells:
            fail(8, f"two pads share cell {cell}")
        seen_cells.add(cell)
        if not (0 <= pad["x"] < m["columns"] and 0 <= pad["y"] < m["rows"]):
            fail(8, f"pad {pad['id']} {cell} leaves the grid")
        nearest = min(((pad["x"] - lx) ** 2 + (pad["y"] - ly) ** 2)
                      for (lx, ly) in lane) ** 0.5
        if nearest > 6.0:
            fail(9, f"pad {pad['id']} is {nearest:.2f} cells from any lane")

    used = set()
    for c in doc["campaigns"]:
        for key in ("spawn", "goal"):
            if c[key] not in endpoints:
                fail(10, f"{c['id']} {key} names unknown endpoint {c[key]!r}")
        if c["spawn"] == c["goal"]:
            fail(10, f"{c['id']} spawn and goal are the same endpoint")
        used.add((c["spawn"], c["goal"]))
    if len(used) != len(doc["campaigns"]):
        fail(10, "campaigns must use distinct spawn/goal pairs")
    return lane


def validate_fixtures(campaign: dict, atlas_ids: set[str] | None) -> None:
    cid = campaign["id"]
    fixtures = campaign["fixtures"]
    if len(fixtures) != len(ROLES):
        fail(11, f"{cid} must define exactly {len(ROLES)} fixtures")
    roles = [f["role"] for f in fixtures]
    for role in ROLES:
        if roles.count(role) != 1:
            fail(11, f"{cid} role {role!r} appears {roles.count(role)} times")
    ids = [f["id"] for f in fixtures]
    if len(set(ids)) != len(ids):
        fail(1, f"{cid} duplicate fixture ids")

    for f in fixtures:
        tiers = f["tiers"]
        if len(tiers) != TIER_COUNT:
            fail(12, f"{cid}/{f['id']} needs exactly {TIER_COUNT} tiers")
        for key in ("damage", "range", "integrity"):
            values = [t[key] for t in tiers]
            if any(b < a for a, b in zip(values, values[1:])):
                fail(12, f"{cid}/{f['id']} {key} decreases across tiers")
        rates = [t["rate"] for t in tiers]
        if any(b < a for a, b in zip(rates, rates[1:])):
            fail(12, f"{cid}/{f['id']} rate decreases across tiers")
        for index, t in enumerate(tiers):
            if t["cost"] <= 0:
                fail(12, f"{cid}/{f['id']} tier {index} cost must be positive")
            if t["damage_type"] not in DAMAGE_TYPES:
                fail(1, f"{cid}/{f['id']} bad damage_type {t['damage_type']!r}")
            if t["targets"] not in TARGET_CLASSES:
                fail(1, f"{cid}/{f['id']} bad targets {t['targets']!r}")
            required = set(ROLE_FIELDS.get(f["role"], ()))
            present = {k for k in t if k in ALL_ROLE_FIELDS}
            if present != required:
                missing = sorted(required - present)
                extra = sorted(present - required)
                fail(13, f"{cid}/{f['id']} tier {index} role fields "
                         f"missing={missing} unexpected={extra}")
        if atlas_ids is not None and f["atlas"] not in atlas_ids:
            fail(14, f"{cid}/{f['id']} unknown atlas {f['atlas']!r}")


def validate_units(campaign: dict) -> None:
    cid = campaign["id"]
    units = campaign["units"]
    ids = [u["id"] for u in units]
    if len(set(ids)) != len(ids):
        fail(1, f"{cid} duplicate unit ids")
    known = set(ids)

    for u in units:
        for key in ("integrity", "speed", "mass", "currency"):
            if u[key] <= 0:
                fail(15, f"{cid}/{u['id']} {key} must be positive")
        for key in ("armor", "shield"):
            if u[key] < 0:
                fail(15, f"{cid}/{u['id']} {key} must be non-negative")
        attack = u.get("attack")
        if attack is not None and attack["kind"] not in ATTACK_KINDS[1:]:
            fail(1, f"{cid}/{u['id']} bad attack kind {attack['kind']!r}")
        for hook in ("on_death", "emit", "on_threshold"):
            spec = u.get(hook)
            if spec is not None and spec["spawn"] not in known:
                fail(1, f"{cid}/{u['id']} {hook} spawns unknown {spec['spawn']!r}")

    # Rule 16: spawn chains must be acyclic, directly or transitively.
    edges: dict[str, set[str]] = {u["id"]: set() for u in units}
    for u in units:
        for hook in ("on_death", "emit", "on_threshold"):
            spec = u.get(hook)
            if spec is not None:
                edges[u["id"]].add(spec["spawn"])
    colour: dict[str, int] = {}

    def visit(node: str, trail: list[str]) -> None:
        state = colour.get(node, 0)
        if state == 1:
            cycle = " -> ".join(trail + [node])
            fail(16, f"{cid} spawn chain is cyclic: {cycle}")
        if state == 2:
            return
        colour[node] = 1
        for nxt in sorted(edges[node]):
            visit(nxt, trail + [node])
        colour[node] = 2

    for u in units:
        visit(u["id"], [])

    # Rule 17: every unit must be damageable by some fixture tier.
    reachable_classes = set()
    for f in campaign["fixtures"]:
        for t in f["tiers"]:
            if t["damage"] > 0 and t["damage_type"] != "none":
                reachable_classes.add(t["targets"])
    hits_ground = any(c in ("ground", "ground_air") for c in reachable_classes)
    hits_air = any(c in ("air", "ground_air") for c in reachable_classes)
    for u in units:
        if u["air"] and not hits_air:
            fail(17, f"{cid}/{u['id']} is air and no fixture can damage air")
        if not u["air"] and not hits_ground:
            fail(17, f"{cid}/{u['id']} is ground and no fixture damages ground")

    # Rule 18: at least one and at most three attacking types.
    attackers = [u["id"] for u in units if u.get("attack") is not None]
    if not 1 <= len(attackers) <= 3:
        fail(18, f"{cid} has {len(attackers)} attacking unit types, want 1..3")


def validate_waves(campaign: dict) -> None:
    cid = campaign["id"]
    waves = campaign["waves"]
    expected = EXPECTED_WAVES.get(cid)
    if expected is not None and len(waves) != expected:
        fail(19, f"{cid} must have {expected} waves, found {len(waves)}")
    for position, w in enumerate(waves, start=1):
        if w["index"] != position:
            fail(19, f"{cid} wave index {w['index']} is not contiguous")

    known = {u["id"]: u for u in campaign["units"]}
    appeared: set[str] = set()
    integrity = campaign["starting_integrity"]
    teaching_waves = 3

    # Rule 21a: no single unit may cost more integrity than the player has.
    # The boss sits at exactly the cap on purpose — one boss leak ends a run
    # from full, which the design states explicitly. Anything above the cap is
    # an authoring slip, not a decision.
    for u in campaign["units"]:
        if u["mass"] > integrity:
            fail(21, f"{cid}/{u['id']} mass {u['mass']} exceeds starting "
                     f"integrity {integrity}")

    for w in waves:
        if w.get("hp_scale") != 1.0:
            fail(3, f"{cid} wave {w['index']} hp_scale must be 1.0")
        if not math.isfinite(w["interval"]) or w["interval"] <= 0:
            fail(19, f"{cid} wave {w['index']} needs a positive spawn interval")
        if not math.isfinite(w["build_seconds"]) or w["build_seconds"] < 0:
            fail(19, f"{cid} wave {w['index']} needs nonnegative build time")
        if not w["groups"]:
            fail(1, f"{cid} wave {w['index']} has no groups")
        for g in w["groups"]:
            if g["type"] not in known:
                fail(1, f"{cid} wave {w['index']} unknown type {g['type']!r}")
            if g["count"] <= 0:
                fail(1, f"{cid} wave {w['index']} group count must be positive")

        # Rule 21b: the teaching waves must be survivable even if entirely
        # missed, so a player learning the board cannot lose outright to them.
        # Later waves are expected to out-mass the integrity pool — that is
        # what makes them waves.
        if w["index"] <= teaching_waves:
            mass = sum(known[g["type"]]["mass"] * g["count"]
                       for g in w["groups"])
            if mass > integrity:
                fail(21, f"{cid} teaching wave {w['index']} total mass {mass} "
                         f"exceeds starting integrity {integrity}")

        for g in w["groups"]:
            appeared.add(g["type"])

    missing = sorted(set(known) - appeared)
    if missing:
        fail(20, f"{cid} unit types never appear in any wave: {missing}")


def load_atlas_ids() -> set[str] | None:
    manifest = ROOT / "assets" / "graphics" / "manifest.json"
    if not manifest.is_file():
        return None
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return {a["id"] for a in data.get("atlases", [])}


def validate_stable_ids(doc: dict) -> None:
    if not STABLE_IDS.is_file():
        return
    try:
        ids = json.loads(STABLE_IDS.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContentError(f"stable_ids.json unreadable: {exc}") from exc
    # Ids are unique per namespace, not globally: cues, scenes and flags are
    # separate spaces and legitimately both start at 1.
    for group, mapping in ids.items():
        if not isinstance(mapping, dict):
            continue
        seen: dict[int, str] = {}
        for name, value in mapping.items():
            if not isinstance(value, int) or value <= 0:
                fail(23, f"stable id {group}.{name} must be a positive integer")
            if value in seen:
                fail(23, f"stable id {value} reused by {group}.{seen[value]} "
                         f"and {group}.{name}")
            seen[value] = name


def map_campaigns(doc: dict, level: dict) -> list[dict]:
    return [dict(c, **level.get("campaigns", {}).get(c["id"], {}))
            for c in doc["campaigns"]]


def validate(doc: dict) -> list[set[tuple[int, int]]]:
    validate_structural(doc)
    lanes = [validate_map(dict(doc, map=m)) for m in doc["maps"]]
    atlas_ids = load_atlas_ids()
    for campaign in doc["campaigns"]:
        validate_fixtures(campaign, atlas_ids)
        validate_units(campaign)
        validate_waves(campaign)
    for level in doc["maps"]:
        for campaign in map_campaigns(doc, level):
            validate_waves(campaign)
            if campaign["starting_currency"] < 0:
                fail(1, "starting currency cannot be negative")
    validate_stable_ids(doc)
    return lanes


# -------------------------------------------------------------- emission --

def c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def tier_literal(role: str, tier: dict) -> str:
    def num(key: str, default: float = 0.0) -> str:
        return f"{float(tier.get(key, default)):.6f}f"

    fields = [
        f'.cost = {int(tier["cost"])}u',
        f'.damage = {int(tier["damage"])}',
        f'.rate = {float(tier["rate"]):.6f}f',
        f'.range = {float(tier["range"]):.6f}f',
        f'.integrity = {int(tier["integrity"])}',
        f'.damage_type = PT_DAMAGE_{tier["damage_type"].upper()}',
        f'.targets = PT_TARGETS_{tier["targets"].upper()}',
        f'.splash_radius = {num("splash_radius")}',
        f'.hold_seconds = {num("hold_seconds")}',
        f'.stun_seconds = {num("stun_seconds")}',
        f'.pull_cells = {int(tier.get("pull_cells", 0))}u',
        f'.enemy_range_penalty = {num("enemy_range_penalty")}',
        f'.enemy_accuracy_penalty = {num("enemy_accuracy_penalty")}',
        f'.damage_buff = {num("damage_buff")}',
        f'.range_buff = {num("range_buff")}',
        f'.currency_tick = {int(tier.get("currency_tick", 0))}u',
        f'.repair_per_second = {num("repair_per_second")}',
        f'.radius = {num("radius")}',
    ]
    del role
    return "{ " + ", ".join(fields) + " }"


def hook_literal(spec: dict | None, index_of: dict[str, int]) -> str:
    if spec is None:
        return ("{ .active = false, .spawn = 0u, .count = 0u, "
                ".max_total = 0u, .value = 0.0f }")
    value = spec.get("integrity_override",
                     spec.get("period", spec.get("integrity_fraction", 0.0)))
    return ("{ .active = true"
            f', .spawn = {index_of[spec["spawn"]]}u'
            f', .count = {int(spec.get("count", 0))}u'
            f', .max_total = {int(spec.get("max_total", 0))}u'
            f", .value = {float(value):.6f}f }}")


def emit(doc: dict, lanes: list[set[tuple[int, int]]]) -> str:
    m = doc["maps"][0]
    cols, rows = m["columns"], m["rows"]
    econ = doc["economy"]
    campaigns = doc["campaigns"]
    digest = hashlib.sha256(
        CONTENT.read_bytes()).hexdigest()

    out: list[str] = []
    add = out.append
    add("/* Generated by tools/compile_content.py — do not edit. */")
    add("#ifndef PT_CONTENT_GENERATED_H")
    add("#define PT_CONTENT_GENERATED_H")
    add("")
    add("#include <stdbool.h>")
    add("#include <stdint.h>")
    add("")
    stable_ids = json.loads(STABLE_IDS.read_text())
    for group, prefix in (("cues", "CUE"), ("scenes", "SCENE")):
        for name, value in stable_ids[group].items():
            symbol = name.upper().replace(".", "_").replace("-", "_")
            add(f"#define PT_{prefix}_{symbol} {value}u")
    add("")
    add(f"#define PT_COLUMNS {cols}")
    add(f"#define PT_ROWS {rows}")
    add(f'#define PT_CELL_PIXELS {m["cell_pixels"]}')
    add(f"#define PT_PAD_COUNT {max(len(m['pads']) for m in doc['maps'])}")
    add(f"#define PT_MAP_COUNT {len(doc['maps'])}")
    add(f"#define PT_CAMPAIGN_COUNT {len(campaigns)}")
    add(f"#define PT_MAX_TIER {TIER_COUNT}")
    add(f"#define PT_FIXTURES_PER_CAMPAIGN {len(ROLES)}")
    max_units = max(len(c["units"]) for c in campaigns)
    max_waves = max(len(c["waves"]) for c in campaigns)
    max_groups = max(len(w["groups"]) for m in doc["maps"]
                     for c in map_campaigns(doc, m) for w in c["waves"])
    add(f"#define PT_UNITS_PER_CAMPAIGN {max_units}")
    add(f"#define PT_MAX_WAVES {max_waves}")
    add(f"#define PT_MAX_WAVE_GROUPS {max_groups}")
    add("")
    add(f'#define PT_STIPEND_BASE {econ["stipend_base"]}u')
    add(f'#define PT_STIPEND_PER_WAVE {econ["stipend_per_wave"]}u')
    add(f'#define PT_EARLY_CALL_PER_SECOND {econ["early_call_per_second"]}u')
    add(f'#define PT_EARLY_CALL_CAP {econ["early_call_cap"]}u')
    add(f'#define PT_SELL_REFUND_PERCENT {econ["sell_refund_percent"]}u')
    add("#define PT_REPAIR_INTEGRITY_PER_UNIT "
        f'{3 * econ["repair_cost_per_3_integrity"]}')
    add("")
    add("typedef enum pt_damage_type {")
    for index, name in enumerate(DAMAGE_TYPES):
        add(f"    PT_DAMAGE_{name.upper()} = {index},")
    add("} pt_damage_type;")
    add("")
    add("typedef enum pt_target_class {")
    for index, name in enumerate(TARGET_CLASSES):
        add(f"    PT_TARGETS_{name.upper()} = {index},")
    add("} pt_target_class;")
    add("")
    add("typedef enum pt_fixture_role {")
    for index, name in enumerate(ROLES):
        add(f"    PT_ROLE_{name.upper()} = {index},")
    add(f"    PT_ROLE_COUNT = {len(ROLES)}")
    add("} pt_fixture_role;")
    add("")
    add("typedef enum pt_attack_kind {")
    for index, name in enumerate(ATTACK_KINDS):
        add(f"    PT_ATTACK_{name.upper()} = {index},")
    add("} pt_attack_kind;")
    add("")
    add("typedef struct pt_tier_def {")
    add("    uint32_t cost;")
    add("    int32_t  damage;")
    add("    float    rate;")
    add("    float    range;")
    add("    int32_t  integrity;")
    add("    uint8_t  damage_type;")
    add("    uint8_t  targets;")
    add("    float    splash_radius;")
    add("    float    hold_seconds;")
    add("    float    stun_seconds;")
    add("    uint16_t pull_cells;")
    add("    float    enemy_range_penalty;")
    add("    float    enemy_accuracy_penalty;")
    add("    float    damage_buff;")
    add("    float    range_buff;")
    add("    uint16_t currency_tick;")
    add("    float    repair_per_second;")
    add("    float    radius;")
    add("} pt_tier_def;")
    add("")
    add("typedef struct pt_fixture_def {")
    add("    const char *id;")
    add("    const char *name;")
    add("    uint8_t     role;")
    add("    uint16_t    atlas_row;")
    add("    pt_tier_def tiers[PT_MAX_TIER];")
    add("} pt_fixture_def;")
    add("")
    add("typedef struct pt_hook_def {")
    add("    bool     active;")
    add("    uint16_t spawn;")
    add("    uint16_t count;")
    add("    uint16_t max_total;  /* 0 = uncapped */")
    add("    float    value;")
    add("} pt_hook_def;")
    add("")
    add("typedef struct pt_unit_def {")
    add("    const char *id;")
    add("    const char *name;")
    add("    int32_t     integrity;")
    add("    int32_t     armor;")
    add("    int32_t     shield;")
    add("    float       shield_regen_delay;")
    add("    float       speed;")
    add("    bool        air;")
    add("    bool        hardened;")
    add("    uint16_t    mass;")
    add("    uint32_t    currency;")
    add("    uint16_t    atlas_row;")
    add("    uint8_t     attack_kind;")
    add("    float       attack_range;")
    add("    float       attack_dps;")
    add("    bool        attack_halts;")
    add("    pt_hook_def on_death;")
    add("    pt_hook_def emit;")
    add("    pt_hook_def on_threshold;")
    add("    float       aura_radius;")
    add("    float       aura_per_second;")
    add("} pt_unit_def;")
    add("")
    add("typedef struct pt_wave_group {")
    add("    uint16_t type;")
    add("    uint16_t count;")
    add("} pt_wave_group;")
    add("")
    add("typedef struct pt_wave_def {")
    add("    uint16_t      index;")
    add("    float         build_seconds;")
    add("    float         interval;")
    add("    uint16_t      group_count;")
    add("    uint16_t      total_units;")
    add("    uint32_t      first_appearance;   /* bitmask over unit indices */")
    add("    pt_wave_group groups[PT_MAX_WAVE_GROUPS];")
    add("} pt_wave_def;")
    add("")
    add("typedef struct pt_campaign_def {")
    add("    const char *id;")
    add("    const char *name;")
    add("    const char *currency_name;")
    add("    uint16_t    spawn_x, spawn_y;")
    add("    uint16_t    goal_x, goal_y;")
    add("    int32_t     starting_integrity;")
    add("    int32_t     starting_currency;")
    add("    int16_t     unlocked_by;         /* -1 = always unlocked */")
    add("    uint16_t    fixture_count;")
    add("    uint16_t    unit_count;")
    add("    uint16_t    wave_count;")
    add("    const pt_fixture_def *fixtures;")
    add("    const pt_unit_def    *units;")
    add("    const pt_wave_def    *waves;")
    add("} pt_campaign_def;")
    add("")
    add("typedef struct pt_pad_def {")
    add("    uint16_t id;")
    add("    uint16_t x, y;")
    add("} pt_pad_def;")
    add("")

    add("typedef struct pt_map_def {")
    add("    const char *id, *name, *description, *backdrop;")
    add("    uint16_t pad_count;")
    add("    bool runtime_foundations;")
    add("    const pt_pad_def *pads;")
    add("    const uint8_t (*lane)[PT_COLUMNS];")
    add("    const pt_campaign_def *campaigns;")
    add("} pt_map_def;")
    add("")

    campaign_index = {c["id"]: i for i, c in enumerate(campaigns)}

    for campaign in campaigns:
        cid = campaign["id"].replace("-", "_")
        unit_index = {u["id"]: i for i, u in enumerate(campaign["units"])}

        add(f"static const pt_fixture_def pt_fixtures_{cid}"
            f"[{len(campaign['fixtures'])}] = {{")
        for f in campaign["fixtures"]:
            add(f"    {{ .id = {c_string(f['id'])}, "
                f".name = {c_string(f['name'])}, "
                f".role = PT_ROLE_{f['role'].upper()}, "
                f".atlas_row = {f['atlas_row']}u, .tiers = {{")
            for tier in f["tiers"]:
                add(f"        {tier_literal(f['role'], tier)},")
            add("    } },")
        add("};")
        add("")

        add(f"static const pt_unit_def pt_units_{cid}"
            f"[{len(campaign['units'])}] = {{")
        for u in campaign["units"]:
            attack = u.get("attack") or {}
            aura = u.get("aura") or {}
            kind = attack.get("kind", "none")
            air = "true" if u["air"] else "false"
            hardened = "true" if u["hardened"] else "false"
            halts = "true" if attack.get("halts") else "false"
            attack_range = float(attack.get("range", 0.0))
            attack_dps = float(attack.get("damage_per_second", 0.0))
            aura_radius = float(aura.get("radius", 0.0))
            aura_rate = float(aura.get("amount_per_second", 0.0))
            parts = [
                f".id = {c_string(u['id'])}",
                f".name = {c_string(u['name'])}",
                f".integrity = {u['integrity']}",
                f".armor = {u['armor']}",
                f".shield = {u['shield']}",
                f".shield_regen_delay = {float(u['shield_regen_delay']):.6f}f",
                f".speed = {float(u['speed']):.6f}f",
                f".air = {air}",
                f".hardened = {hardened}",
                f".mass = {u['mass']}u",
                f".currency = {u['currency']}u",
                f".atlas_row = {u['atlas_row']}u",
                f".attack_kind = PT_ATTACK_{kind.upper()}",
                f".attack_range = {attack_range:.6f}f",
                f".attack_dps = {attack_dps:.6f}f",
                f".attack_halts = {halts}",
                f".on_death = {hook_literal(u.get('on_death'), unit_index)}",
                f".emit = {hook_literal(u.get('emit'), unit_index)}",
                ".on_threshold = "
                f"{hook_literal(u.get('on_threshold'), unit_index)}",
                f".aura_radius = {aura_radius:.6f}f",
                f".aura_per_second = {aura_rate:.6f}f",
            ]
            add("    { " + ", ".join(parts) + " },")
        add("};")
        add("")

    for m, lane in zip(doc["maps"], lanes):
        mid = m["id"].replace("-", "_")
        for campaign in map_campaigns(doc, m):
            cid = campaign["id"].replace("-", "_")
            unit_index = {u["id"]: i for i, u in enumerate(campaign["units"])}
            add(f"static const pt_wave_def pt_waves_{mid}_{cid}"
                f"[{len(campaign['waves'])}] = {{")
            appeared: set[str] = set()
            for w in campaign["waves"]:
                mask = 0
                for g in w["groups"]:
                    if g["type"] not in appeared:
                        mask |= 1 << unit_index[g["type"]]
                        appeared.add(g["type"])
                total = sum(int(g["count"]) for g in w["groups"])
                groups = ", ".join(
                    f'{{ {unit_index[g["type"]]}u, {int(g["count"])}u }}'
                    for g in w["groups"])
                add(f"    {{ .index = {w['index']}u, "
                    f".build_seconds = {float(w['build_seconds']):.6f}f, "
                    f".interval = {float(w['interval']):.6f}f, "
                    f".group_count = {len(w['groups'])}u, "
                    f".total_units = {total}u, "
                    f".first_appearance = 0x{mask:08x}u, "
                    f".groups = {{ {groups} }} }},")
            add("};")
            add("")

        add(f"static const pt_campaign_def pt_campaigns_{mid}[PT_CAMPAIGN_COUNT] = {{")
        for campaign in map_campaigns(doc, m):
            cid = campaign["id"].replace("-", "_")
            endpoints = m["endpoints"]
            spawn = endpoints[campaign["spawn"]]
            goal = endpoints[campaign["goal"]]
            unlock = campaign.get("unlocked_by")
            unlock_index = -1 if unlock is None else campaign_index[unlock]
            add("    { "
                f".id = {c_string(campaign['id'])}, "
                f".name = {c_string(campaign['name'])}, "
                f".currency_name = {c_string(campaign['currency_name'])}, "
                f".spawn_x = {spawn['x']}u, .spawn_y = {spawn['y']}u, "
                f".goal_x = {goal['x']}u, .goal_y = {goal['y']}u, "
                f".starting_integrity = {campaign['starting_integrity']}, "
                f".starting_currency = {campaign['starting_currency']}, "
                f".unlocked_by = {unlock_index}, "
                f".fixture_count = {len(campaign['fixtures'])}u, "
                f".unit_count = {len(campaign['units'])}u, "
                f".wave_count = {len(campaign['waves'])}u, "
                f".fixtures = pt_fixtures_{cid}, "
                f".units = pt_units_{cid}, "
                f".waves = pt_waves_{mid}_{cid} }},")
        add("};")
        add("")

        add(f"static const pt_pad_def pt_pads_{mid}[{len(m['pads'])}] = {{")
        for pad in sorted(m["pads"], key=lambda p: p["id"]):
            add(f"    {{ {pad['id']}u, {pad['x']}u, {pad['y']}u }},")
        add("};")
        add("")

        add(f"static const uint8_t pt_lane_{mid}[PT_ROWS][PT_COLUMNS] = {{")
        for y in range(rows):
            row = ", ".join("1" if (x, y) in lane else "0" for x in range(cols))
            add(f"    {{ {row} }},")
        add("};")
        add("")
    add("static const pt_map_def pt_maps[PT_MAP_COUNT] = {")
    for m in doc["maps"]:
        mid = m["id"].replace("-", "_")
        add("    { "
            f".id = {c_string(m['id'])}, .name = {c_string(m['name'])}, "
            f".description = {c_string(m['description'])}, "
            f".backdrop = {c_string(m['backdrop'].removeprefix('assets/'))}, "
            f".pad_count = {len(m['pads'])}u, .pads = pt_pads_{mid}, "
            f".runtime_foundations = {str(m.get('runtime_foundations', False)).lower()}, "
            f".lane = pt_lane_{mid}, .campaigns = pt_campaigns_{mid} }},")
    add("};")
    add("")
    add(f"static const char pt_content_sha256[] = {c_string(digest)};")
    add("")
    add("#endif /* PT_CONTENT_GENERATED_H */")
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------ main --

def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path,
                        default=ROOT / "build" / "content_generated.h")
    parser.add_argument("--check", action="store_true",
                        help="validate only; write nothing")
    args = parser.parse_args(argv)

    try:
        doc = load()
        lane = validate(doc)
    except ContentError as exc:
        print(f"content: {exc}", file=sys.stderr)
        return 1

    campaigns = doc["campaigns"]
    lane_cells = sum(map(len, lane))
    if args.check:
        print(f"content OK: {len(campaigns)} campaigns, "
              f"{len(doc['maps'])} maps, {lane_cells} lane cells, "
              + ", ".join(f"{c['id']}={len(c['waves'])}w" for c in campaigns))
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(emit(doc, lane), encoding="utf-8")
    print(f"content: wrote {args.out} ({lane_cells} lane cells)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
