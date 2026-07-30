#!/usr/bin/env python3
"""Headless balance simulation for Pleb Tower.

Runs the authored waves against scripted build orders and reports what actually
happens: leaks by type, integrity remaining, currency curve, fixtures lost, and
clear time. Balance is a test result, not an opinion.

This is a deliberate second implementation of the combat and economy rules,
driven by the same content/campaigns.json the runtime compiles. Two independent
implementations that agree are evidence; one implementation checking itself is
not. `--cross-check` compares against the C runtime's own trace.

Standard library only. Python 3.10+.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTENT = ROOT / "content" / "campaigns.json"

STEP = 1.0 / 60.0
STEP_COST = 16
STEP_CAP = 900.0          # seconds; a wave that runs this long has stalled


# ------------------------------------------------------------------ board --

class Board:
    def __init__(self, doc: dict, campaign: dict) -> None:
        m = doc["map"]
        self.cols = m["columns"]
        self.rows = m["rows"]
        self.lane: set[tuple[int, int]] = set()
        for seg in m["lane"]:
            x0, y0 = seg["from"]["x"], seg["from"]["y"]
            x1, y1 = seg["to"]["x"], seg["to"]["y"]
            if x0 == x1:
                for y in range(min(y0, y1), max(y0, y1) + 1):
                    self.lane.add((x0, y))
            else:
                for x in range(min(x0, x1), max(x0, x1) + 1):
                    self.lane.add((x, y0))
        self.pads = {p["id"]: (p["x"], p["y"]) for p in m["pads"]}
        spawn = m["endpoints"][campaign["spawn"]]
        goal = m["endpoints"][campaign["goal"]]
        self.spawn = (spawn["x"], spawn["y"])
        self.goal = (goal["x"], goal["y"])
        self.distance = self._dijkstra()
        self.path = self._walk()

    def _dijkstra(self) -> dict[tuple[int, int], int]:
        dist = {self.goal: 0}
        settled: set[tuple[int, int]] = set()
        while True:
            best, pick = None, None
            for cell in self.lane:
                if cell in settled or cell not in dist:
                    continue
                if best is None or dist[cell] < best:
                    best, pick = dist[cell], cell
            if pick is None:
                break
            settled.add(pick)
            cx, cy = pick
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nxt = (cx + dx, cy + dy)
                if nxt not in self.lane or nxt in settled:
                    continue
                cand = best + STEP_COST
                if nxt not in dist or cand < dist[nxt]:
                    dist[nxt] = cand
        return dist

    def _walk(self) -> list[tuple[int, int]]:
        """The single route from spawn to goal, in order."""
        cell = self.spawn
        route = [cell]
        while cell != self.goal:
            cx, cy = cell
            best, pick = self.distance[cell], None
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nxt = (cx + dx, cy + dy)
                if nxt in self.distance and self.distance[nxt] < best:
                    best, pick = self.distance[nxt], nxt
            if pick is None:
                break
            cell = pick
            route.append(cell)
        return route

    def progress_to_xy(self, cells: float) -> tuple[float, float]:
        if cells <= 0:
            return (float(self.spawn[0]), float(self.spawn[1]))
        index = min(int(cells), len(self.path) - 1)
        return (float(self.path[index][0]), float(self.path[index][1]))

    def route_cells(self) -> int:
        return len(self.path) - 1


# ------------------------------------------------------------------ state --

@dataclass
class Unit:
    kind: str
    integrity: float
    shield: float
    shield_idle: float = 0.0
    progress: float = 0.0        # cells travelled along the route
    serial: int = 0
    hold: float = 0.0
    stun: float = 0.0
    emit_timer: float = 0.0
    threshold_fired: bool = False
    emitted_total: int = 0
    air_distance: float = 0.0


@dataclass
class Fixture:
    kind: str
    tier: int
    pad: int
    mode: str = "first"
    cooldown: float = 0.0
    integrity: float = 0.0
    integrity_max: float = 0.0
    invested: int = 0
    damage_scale: float = 1.0
    range_scale: float = 1.0
    currency_tick: int = 0


@dataclass
class Report:
    order: str
    campaign: str
    cleared: bool = False
    integrity: int = 0
    currency: int = 0
    earned: int = 0
    fixtures_lost: int = 0
    seconds: float = 0.0
    last_wave: int = 0
    leaks: dict[str, int] = field(default_factory=dict)
    per_wave: list[dict] = field(default_factory=list)


# ------------------------------------------------------------------- sim --

class Simulation:
    def __init__(self, doc: dict, campaign_id: str, order: list[tuple]) -> None:
        self.doc = doc
        self.campaign = next(c for c in doc["campaigns"]
                             if c["id"] == campaign_id)
        self.board = Board(doc, self.campaign)
        self.econ = doc["economy"]
        self.units_by_id = {u["id"]: u for u in self.campaign["units"]}
        self.fixtures_by_id = {f["id"]: f for f in self.campaign["fixtures"]}
        self.order = order
        self.report = Report(order="", campaign=campaign_id)

        self.integrity = float(self.campaign["starting_integrity"])
        self.currency = float(self.campaign["starting_currency"])
        self.earned = 0
        self.fixtures: dict[int, Fixture] = {}
        self.live: list[Unit] = []
        self.serial = 0
        self.fixtures_lost = 0
        self.clock = 0.0

    # -- economy ---------------------------------------------------------
    def spend(self, amount: int) -> bool:
        if self.currency < amount:
            return False
        self.currency -= amount
        return True

    def award(self, amount: int) -> None:
        self.currency += amount
        self.earned += amount

    # -- building --------------------------------------------------------
    def apply_order_for_wave(self, wave_number: int) -> None:
        for entry in self.order:
            when, pad, kind, tier = entry
            if when != wave_number:
                continue
            self.build(pad, kind, tier)

    def build(self, pad: int, kind: str, target_tier: int) -> None:
        spec = self.fixtures_by_id[kind]
        fixture = self.fixtures.get(pad)
        if fixture is None:
            cost = spec["tiers"][0]["cost"]
            if not self.spend(cost):
                return
            fixture = Fixture(kind=kind, tier=0, pad=pad, invested=cost,
                              integrity=spec["tiers"][0]["integrity"],
                              integrity_max=spec["tiers"][0]["integrity"])
            self.fixtures[pad] = fixture
        while fixture.tier < target_tier and fixture.tier + 1 < 3:
            cost = spec["tiers"][fixture.tier + 1]["cost"]
            if not self.spend(cost):
                break
            fixture.tier += 1
            fixture.invested += cost
            tier = spec["tiers"][fixture.tier]
            fixture.integrity_max = tier["integrity"]
            fixture.integrity = tier["integrity"]
        self.refresh_support()

    def refresh_support(self) -> None:
        supports = [f for f in self.fixtures.values()
                    if self.fixtures_by_id[f.kind]["role"] == "support"]
        for fixture in self.fixtures.values():
            if self.fixtures_by_id[fixture.kind]["role"] == "support":
                continue
            fx, fy = self.board.pads[fixture.pad]
            best_dmg, best_rng, best_tick = 0.0, 0.0, 0
            for support in supports:
                sx, sy = self.board.pads[support.pad]
                tier = self.fixtures_by_id[support.kind]["tiers"][support.tier]
                if math.dist((fx, fy), (sx, sy)) <= tier["radius"]:
                    # Largest single bonus wins; bonuses never stack.
                    best_dmg = max(best_dmg, tier["damage_buff"])
                    best_rng = max(best_rng, tier["range_buff"])
                    best_tick = max(best_tick, tier["currency_tick"])
            fixture.damage_scale = 1.0 + best_dmg
            fixture.range_scale = 1.0 + best_rng
            fixture.currency_tick = best_tick

    # -- combat ----------------------------------------------------------
    def unit_xy(self, unit: Unit) -> tuple[float, float]:
        spec = self.units_by_id[unit.kind]
        if spec["air"]:
            sx, sy = self.board.spawn
            gx, gy = self.board.goal
            total = math.dist((sx, sy), (gx, gy))
            t = min(1.0, unit.air_distance / total) if total else 1.0
            return (sx + (gx - sx) * t, sy + (gy - sy) * t)
        return self.board.progress_to_xy(unit.progress)

    def unit_progress_metric(self, unit: Unit) -> float:
        spec = self.units_by_id[unit.kind]
        if spec["air"]:
            gx, gy = self.board.goal
            x, y = self.unit_xy(unit)
            return -math.dist((x, y), (gx, gy))
        return unit.progress

    def acquire(self, fixture: Fixture) -> Unit | None:
        spec = self.fixtures_by_id[fixture.kind]
        tier = spec["tiers"][fixture.tier]
        if tier["damage"] <= 0:
            return None
        reach = tier["range"] * fixture.range_scale
        fx, fy = self.board.pads[fixture.pad]
        targets = tier["targets"]
        best: Unit | None = None
        best_key = None
        for unit in self.live:
            uspec = self.units_by_id[unit.kind]
            if uspec["air"] and targets not in ("air", "ground_air"):
                continue
            if not uspec["air"] and targets not in ("ground", "ground_air"):
                continue
            x, y = self.unit_xy(unit)
            distance = math.dist((fx, fy), (x, y))
            if distance > reach:
                continue
            if fixture.mode == "first":
                key = (-self.unit_progress_metric(unit), unit.serial)
            elif fixture.mode == "last":
                key = (self.unit_progress_metric(unit), unit.serial)
            elif fixture.mode == "strongest":
                key = (-unit.integrity, unit.serial)
            else:
                key = (distance, unit.serial)
            if best_key is None or key < best_key:
                best, best_key = unit, key
        return best

    def apply_damage(self, unit: Unit, damage: float, dtype: str) -> None:
        spec = self.units_by_id[unit.kind]
        if dtype != "pierce":
            damage -= spec["armor"]
        damage = max(1.0, damage)
        unit.shield_idle = 0.0
        if unit.shield > 0:
            taken = damage * (0.5 if dtype == "splash" else 1.0)
            absorbed = min(unit.shield, taken)
            unit.shield -= absorbed
            damage -= absorbed / (0.5 if dtype == "splash" else 1.0)
            if damage <= 0:
                return
        unit.integrity -= damage

    def kill(self, unit: Unit) -> None:
        spec = self.units_by_id[unit.kind]
        tick = max((f.currency_tick for f in self.fixtures.values()), default=0)
        self.award(spec["currency"] + tick)
        hook = spec.get("on_death")
        if hook:
            for _ in range(hook["count"]):
                self.spawn_unit(hook["spawn"], unit.progress,
                                integrity=hook.get("integrity_override"))

    def spawn_unit(self, kind: str, progress: float = 0.0,
                   integrity: float | None = None) -> None:
        spec = self.units_by_id[kind]
        self.serial += 1
        self.live.append(Unit(
            kind=kind,
            integrity=float(integrity if integrity is not None
                            else spec["integrity"]),
            shield=float(spec["shield"]),
            progress=progress,
            serial=self.serial))

    # -- the wave --------------------------------------------------------
    def interleave(self, wave: dict) -> list[str]:
        """Deterministic proportional interleave — the same rule the runtime
        uses. Highest remaining ratio goes next; ties break on group order."""
        groups = [[g["type"], g["count"], 0] for g in wave["groups"]]
        total = sum(g[1] for g in groups)
        out: list[str] = []
        for _ in range(total):
            best_index, best_score = 0, None
            for index, (_, count, emitted) in enumerate(groups):
                if emitted >= count:
                    continue
                score = (emitted + 1) / count
                if best_score is None or score < best_score:
                    best_index, best_score = index, score
            out.append(groups[best_index][0])
            groups[best_index][2] += 1
        return out

    def run_wave(self, wave: dict) -> dict:
        queue = self.interleave(wave)
        emitted = 0
        spawn_timer = 0.0
        elapsed = 0.0
        leaks: dict[str, int] = {}
        route_cells = self.board.route_cells()

        while (emitted < len(queue) or self.live) and elapsed < STEP_CAP:
            elapsed += STEP
            self.clock += STEP
            spawn_timer -= STEP
            if emitted < len(queue) and spawn_timer <= 0.0:
                self.spawn_unit(queue[emitted])
                emitted += 1
                spawn_timer = wave["interval"]

            # movement
            for unit in list(self.live):
                spec = self.units_by_id[unit.kind]
                if unit.hold > 0:
                    unit.hold -= STEP
                    continue
                if unit.stun > 0:
                    unit.stun -= STEP
                    continue
                unit.shield_idle += STEP
                if spec["shield"] and unit.shield_idle >= spec["shield_regen_delay"]:
                    unit.shield = float(spec["shield"])
                if spec["air"]:
                    unit.air_distance += spec["speed"] * STEP
                    gx, gy = self.board.goal
                    sx, sy = self.board.spawn
                    if unit.air_distance >= math.dist((sx, sy), (gx, gy)):
                        leaks[unit.kind] = leaks.get(unit.kind, 0) + 1
                        self.integrity -= spec["mass"]
                        self.live.remove(unit)
                else:
                    unit.progress += spec["speed"] * STEP
                    if unit.progress >= route_cells:
                        leaks[unit.kind] = leaks.get(unit.kind, 0) + 1
                        self.integrity -= spec["mass"]
                        self.live.remove(unit)
                emit = spec.get("emit")
                if emit and unit in self.live:
                    unit.emit_timer += STEP
                    cap = emit.get("max_total", 0)
                    if unit.emit_timer >= emit["period"] and (
                            not cap or unit.emitted_total < cap):
                        unit.emit_timer = 0.0
                        for _ in range(emit["count"]):
                            if cap and unit.emitted_total >= cap:
                                break
                            unit.emitted_total += 1
                            self.spawn_unit(emit["spawn"], unit.progress)

            # auras
            for unit in self.live:
                spec = self.units_by_id[unit.kind]
                aura = spec.get("aura")
                if not aura:
                    continue
                ux, uy = self.unit_xy(unit)
                for other in self.live:
                    ox, oy = self.unit_xy(other)
                    if math.dist((ux, uy), (ox, oy)) <= aura["radius"]:
                        cap = float(self.units_by_id[other.kind]["integrity"])
                        other.integrity = min(
                            cap, other.integrity + aura["amount_per_second"] * STEP)

            # fixtures fire
            for fixture in self.fixtures.values():
                spec = self.fixtures_by_id[fixture.kind]
                tier = spec["tiers"][fixture.tier]
                role = spec["role"]
                if role in ("support", "vision", "reroute"):
                    continue
                fixture.cooldown -= STEP
                if fixture.cooldown > 0 or tier["rate"] <= 0:
                    continue
                target = self.acquire(fixture)
                if target is None:
                    continue
                fixture.cooldown = 1.0 / tier["rate"]
                if role == "control":
                    if not self.units_by_id[target.kind]["hardened"]:
                        target.hold = tier["hold_seconds"]
                    continue
                if role == "disable":
                    if not self.units_by_id[target.kind]["hardened"]:
                        target.stun = tier["stun_seconds"]
                    continue
                damage = tier["damage"] * fixture.damage_scale
                self.apply_damage(target, damage, tier["damage_type"])
                if tier["damage_type"] == "splash":
                    tx, ty = self.unit_xy(target)
                    for other in self.live:
                        if other is target:
                            continue
                        ox, oy = self.unit_xy(other)
                        if math.dist((tx, ty), (ox, oy)) <= tier["splash_radius"]:
                            self.apply_damage(other, damage, "splash")

            # fixture attacks + deaths
            for unit in list(self.live):
                spec = self.units_by_id[unit.kind]
                attack = spec.get("attack")
                if attack and self.fixtures:
                    ux, uy = self.unit_xy(unit)
                    reach = attack.get("range", 1.0)
                    for fixture in list(self.fixtures.values()):
                        px, py = self.board.pads[fixture.pad]
                        if math.dist((ux, uy), (px, py)) <= reach:
                            fixture.integrity -= attack["damage_per_second"] * STEP
                            if fixture.integrity <= 0:
                                del self.fixtures[fixture.pad]
                                self.fixtures_lost += 1
                                self.refresh_support()
                            break
                threshold = spec.get("on_threshold")
                if threshold and not unit.threshold_fired:
                    if unit.integrity <= spec["integrity"] * threshold["integrity_fraction"]:
                        unit.threshold_fired = True
                        for _ in range(threshold["count"]):
                            self.spawn_unit(threshold["spawn"], unit.progress)
                if unit.integrity <= 0:
                    self.live.remove(unit)
                    self.kill(unit)

            # support repair
            for fixture in self.fixtures.values():
                spec = self.fixtures_by_id[fixture.kind]
                if spec["role"] != "support":
                    continue
                tier = spec["tiers"][fixture.tier]
                sx, sy = self.board.pads[fixture.pad]
                for other in self.fixtures.values():
                    ox, oy = self.board.pads[other.pad]
                    if math.dist((sx, sy), (ox, oy)) <= tier["radius"]:
                        other.integrity = min(
                            other.integrity_max,
                            other.integrity + tier["repair_per_second"] * STEP)

            if self.integrity <= 0:
                break

        return {"wave": wave["index"], "leaks": leaks, "seconds": elapsed,
                "integrity": max(0, int(self.integrity)),
                "currency": int(self.currency),
                "fixtures": len(self.fixtures)}

    def run(self, name: str) -> Report:
        report = Report(order=name, campaign=self.campaign["id"])
        for wave in self.campaign["waves"]:
            self.apply_order_for_wave(wave["index"])
            self.award(self.econ["stipend_base"]
                       + self.econ["stipend_per_wave"] * wave["index"])
            result = self.run_wave(wave)
            report.per_wave.append(result)
            for kind, count in result["leaks"].items():
                report.leaks[kind] = report.leaks.get(kind, 0) + count
            report.last_wave = wave["index"]
            if self.integrity <= 0:
                break
        report.cleared = self.integrity > 0
        report.integrity = max(0, int(self.integrity))
        report.currency = int(self.currency)
        report.earned = self.earned
        report.fixtures_lost = self.fixtures_lost
        report.seconds = self.clock
        return report


# --------------------------------------------------------------- orders ---
# (wave_number, pad_id, fixture_id, target_tier)

HOLDOUT_ORDERS: dict[str, list[tuple]] = {
    # Best play: saturate the Curb Hairpin first (pads 3-6 each cover the
    # row-12 and row-8 runs, so every ground unit is engaged twice), then
    # pad 10 which reaches three lane segments, then the Long Run.
    "optimal": [
        (1, 4, "rail-spike", 0), (1, 5, "rail-spike", 0),
        (2, 6, "rail-spike", 0), (2, 3, "rail-spike", 0),
        (3, 4, "rail-spike", 1), (3, 5, "rail-spike", 1),
        (4, 10, "rail-spike", 1), (4, 6, "rail-spike", 1),
        (5, 11, "thermite-charge", 0),
        (6, 12, "jammer-mast", 0),
        (7, 9, "rail-spike", 1), (7, 10, "rail-spike", 2),
        (8, 13, "jammer-mast", 1), (8, 8, "block-workshop", 0),
        (9, 4, "rail-spike", 2), (9, 5, "rail-spike", 2),
        (10, 16, "thermite-charge", 1), (10, 8, "block-workshop", 1),
        (11, 17, "rail-spike", 1), (11, 6, "rail-spike", 2),
        (12, 15, "thermite-charge", 2), (12, 12, "jammer-mast", 2),
        (13, 18, "rail-spike", 2), (13, 3, "rail-spike", 2),
        (14, 14, "block-workshop", 2), (14, 9, "rail-spike", 2),
        (15, 19, "rail-spike", 2), (15, 17, "rail-spike", 2),
        (15, 11, "thermite-charge", 2), (15, 20, "rail-spike", 2),
    ],
    # Only the cheapest damage role, never upgraded. Must die to the Breacher.
    "naive-rapid": [
        (n, pad, "rail-spike", 0)
        for n, pad in enumerate([4, 5, 6, 3, 10, 9, 11, 12, 13, 8], start=1)
    ],
    # A competent ground board with no answer to air. Must leak at wave 6.
    "no-antiair": [
        (1, 4, "rail-spike", 0), (1, 5, "rail-spike", 0),
        (2, 6, "rail-spike", 1), (3, 3, "rail-spike", 1),
        (4, 10, "rail-spike", 1), (5, 11, "thermite-charge", 1),
        (6, 9, "rail-spike", 1), (7, 16, "thermite-charge", 1),
        (8, 17, "rail-spike", 1), (9, 18, "rail-spike", 1),
    ],
    # Damage but no Workshop and no Floodlight: Suppressors eat the board.
    "no-support": [
        (1, 4, "rail-spike", 0), (1, 5, "rail-spike", 0),
        (2, 6, "rail-spike", 1), (3, 3, "rail-spike", 1),
        (4, 10, "rail-spike", 2), (5, 11, "thermite-charge", 1),
        (6, 12, "jammer-mast", 1), (7, 9, "rail-spike", 2),
        (8, 13, "jammer-mast", 1), (9, 16, "thermite-charge", 1),
        (10, 17, "rail-spike", 2), (11, 18, "rail-spike", 2),
        (12, 15, "thermite-charge", 2), (13, 19, "rail-spike", 2),
    ],
    # Proves the no-stacking rule: support alone kills nothing.
    "workshop-farm": [
        (1, 4, "rail-spike", 0), (2, 8, "block-workshop", 1),
        (3, 9, "block-workshop", 1), (4, 10, "block-workshop", 1),
        (5, 11, "block-workshop", 1), (6, 12, "block-workshop", 1),
    ],
    # Plausible new-player play: right ideas, slower and less concentrated.
    "first-time": [
        (1, 4, "rail-spike", 0), (1, 5, "rail-spike", 0),
        (2, 11, "rail-spike", 0), (3, 6, "rail-spike", 1),
        (4, 3, "rail-spike", 1), (5, 12, "thermite-charge", 0),
        (6, 13, "jammer-mast", 0), (7, 10, "rail-spike", 1),
        (8, 16, "rail-spike", 1), (9, 8, "block-workshop", 0),
        (10, 17, "thermite-charge", 1), (11, 9, "rail-spike", 2),
        (12, 18, "rail-spike", 1), (13, 15, "thermite-charge", 1),
        (14, 14, "rail-spike", 2), (15, 19, "rail-spike", 2),
    ],
}

CORDON_ORDERS: dict[str, list[tuple]] = {
    # CORDON runs the map backwards, so the Approach (pads 19-23) is the FIRST
    # line and the Hairpin is the last. There is no double-coverage opening,
    # which is why this campaign starts with more Allocation.
    "cordon-optimal": [
        # The Curb Hairpin (pads 3-6) is the strongest ground on this map in
        # EITHER direction: each pad reaches both the row-8 and row-12 runs, so
        # every ground unit is engaged twice. In CORDON it is the last line
        # rather than the first, which is fine — a kill before the goal is a
        # kill. Building at the Approach instead is the trap this order avoids.
        (1, 4, "interdiction-turret", 0), (1, 5, "interdiction-turret", 0),
        (2, 6, "interdiction-turret", 0), (2, 3, "interdiction-turret", 0),
        (3, 4, "interdiction-turret", 1), (3, 5, "interdiction-turret", 1),
        (4, 13, "aerial-interceptor", 0), (4, 6, "interdiction-turret", 1),
        (5, 10, "interdiction-turret", 1), (5, 3, "interdiction-turret", 1),
        (6, 9, "interdiction-turret", 1), (6, 8, "sector-node", 0),
        (7, 11, "suppression-mortar", 0), (7, 10, "interdiction-turret", 2),
        (8, 12, "aerial-interceptor", 1), (8, 4, "interdiction-turret", 2),
        (9, 5, "interdiction-turret", 2), (9, 8, "sector-node", 1),
        (10, 16, "suppression-mortar", 1), (10, 6, "interdiction-turret", 2),
        (11, 9, "interdiction-turret", 2), (11, 3, "interdiction-turret", 2),
        (12, 17, "interdiction-turret", 2), (12, 11, "suppression-mortar", 2),
    ],
}


def format_report(report: Report) -> str:
    status = "CLEARED" if report.cleared else f"LOST at wave {report.last_wave}"
    leaks = sum(report.leaks.values())
    return (f"  {report.order:<16} {status:<20} "
            f"integrity {report.integrity:>2}/20  "
            f"earned {report.earned:>5}  unspent {report.currency:>5}  "
            f"leaks {leaks:>3}  fixtures lost {report.fixtures_lost:>2}  "
            f"{report.seconds / 60.0:>5.1f} min")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--order")
    parser.add_argument("--campaign", default="holdout")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    doc = json.loads(CONTENT.read_text(encoding="utf-8"))
    reports: list[Report] = []

    def run(campaign_id: str, name: str, order: list[tuple]) -> None:
        sim = Simulation(doc, campaign_id, order)
        report = sim.run(name)
        reports.append(report)

    if args.order:
        table = HOLDOUT_ORDERS if args.campaign == "holdout" else CORDON_ORDERS
        if args.order not in table:
            print(f"unknown order {args.order!r}", file=sys.stderr)
            return 2
        run(args.campaign, args.order, table[args.order])
    else:
        for name, order in HOLDOUT_ORDERS.items():
            run("holdout", name, order)
        for name, order in CORDON_ORDERS.items():
            run("cordon", name, order)

    print("HOLDOUT")
    for report in reports:
        if report.campaign == "holdout":
            print(format_report(report))
    print("CORDON")
    for report in reports:
        if report.campaign == "cordon":
            print(format_report(report))

    if args.verbose:
        for report in reports:
            print(f"\n{report.campaign}/{report.order}")
            for row in report.per_wave:
                print(f"    wave {row['wave']:>2}  "
                      f"integrity {row['integrity']:>2}  "
                      f"currency {row['currency']:>5}  "
                      f"fixtures {row['fixtures']:>2}  "
                      f"{row['seconds']:>5.1f}s  leaks {row['leaks']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
