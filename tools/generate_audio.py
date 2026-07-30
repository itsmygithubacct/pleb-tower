#!/usr/bin/env python3
"""Build Pleb Tower's deterministic production audio bank.

The retained MiniMax score is decoded locally; this script never calls a
network service.  Sound effects are rendered through the approved providers in
the workspace kilix-apps/python_sound_generator checkout.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import random
import subprocess
import sys
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Sequence


SAMPLE_RATE = 44_100
BASE_SEED = 0x504C4542544F5745  # "PLEBTOWE"; stable-v2 derives every take.
GAME_ROOT = Path(__file__).resolve().parents[1]
SCORE_SOURCE = Path("assets/audio/source/minimax/block-evening.mp3")
STABLE_IDS = Path("content/stable_ids.json")
FOLD_SECONDS = 4.0

# These broad windows locate the six authored passages without freezing exact
# cut times.  The actual boundary inside each window is selected from the
# decoded score's spectral/energy novelty, then refined to a nearby zero
# crossing.  They intentionally leave room for encoder padding and a future
# lossless replacement of the exact same score.
BOUNDARY_SEARCH_WINDOWS = (
    (12.0, 28.0),
    (42.0, 75.0),
    (90.0, 120.0),
    (120.0, 140.0),
    (150.0, 175.0),
)


@dataclass(frozen=True, slots=True)
class Layer:
    provider: str
    cue: str
    gain: float = 1.0
    offset: float = 0.0
    options: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True, slots=True)
class CueSpec:
    cue_id: str
    description: str
    layers: tuple[Layer, ...]
    variations: int = 1
    peak_db: float = -6.0
    priority: int = 50
    max_instances: int = 4
    cooldown_ms: int = 0


def layer(
    provider: str,
    cue: str,
    gain: float = 1.0,
    offset: float = 0.0,
    **options: Any,
) -> Layer:
    return Layer(provider, cue, gain, offset, options)


def cue(
    cue_id: str,
    description: str,
    *layers: Layer,
    variations: int = 1,
    peak_db: float = -6.0,
    priority: int = 50,
    max_instances: int = 4,
    cooldown_ms: int = 0,
) -> CueSpec:
    return CueSpec(
        cue_id,
        description,
        tuple(layers),
        variations,
        peak_db,
        priority,
        max_instances,
        cooldown_ms,
    )


def _sfx_specs() -> tuple[CueSpec, ...]:
    """Map every frozen gameplay cue to one or more approved providers."""

    specs = (
        # Interface -------------------------------------------------------
        cue(
            "ui.cursor",
            "Move one menu or build selection.",
            layer("ui", "menu_move", style="scifi"),
            variations=3,
            peak_db=-12.0,
            cooldown_ms=30,
        ),
        cue(
            "ui.confirm",
            "Confirm a menu choice.",
            layer("ui", "menu_accept", style="scifi"),
            peak_db=-9.0,
            priority=65,
        ),
        cue(
            "ui.cancel",
            "Cancel or close an interface.",
            layer("ui", "menu_cancel", style="scifi"),
            peak_db=-10.0,
            priority=65,
        ),
        cue(
            "ui.invalid",
            "Reject an unavailable action.",
            layer("ui", "menu_invalid", style="scifi"),
            variations=3,
            peak_db=-9.0,
            priority=70,
            cooldown_ms=90,
        ),
        # Building: warm, improvised resident-side sounds ----------------
        cue(
            "build.place",
            "Residents place a fixture on a pad.",
            layer("quirky-rpg", "gadget_use", style="suburban", intensity=0.72),
            peak_db=-7.0,
        ),
        cue(
            "build.upgrade",
            "A fixture gains a tier.",
            layer("quirky-rpg", "level_up", style="suburban", intensity=0.76),
            peak_db=-6.0,
            priority=60,
        ),
        cue(
            "build.sell",
            "Allocation is returned for a sold fixture.",
            layer("quirky-rpg", "atm", style="suburban", intensity=0.68),
            peak_db=-8.0,
        ),
        cue(
            "build.repair",
            "A damaged fixture is repaired.",
            layer("quirky-rpg", "gadget_repair", style="suburban", intensity=0.72),
            peak_db=-7.0,
        ),
        cue(
            "build.mode",
            "Cycle a fixture targeting mode.",
            layer("cyberpunk", "terminal_key", style="corporate", intensity=0.58),
            variations=3,
            peak_db=-11.0,
            cooldown_ms=35,
        ),
        cue(
            "build.reroute",
            "The Decoy Beacon gathers a lane group.",
            layer("cyberpunk", "scan_ping", style="noir", intensity=0.66),
            peak_db=-8.0,
            priority=62,
        ),
        # Fixture fire ----------------------------------------------------
        cue(
            "fire.rapid",
            "Rapid improvised gun report.",
            layer("weapons", "fire", weapon="smg", intensity=0.66),
            variations=3,
            peak_db=-5.0,
            max_instances=6,
            cooldown_ms=28,
        ),
        cue(
            "fire.artillery",
            "Heavy cul-de-sac artillery report.",
            layer("weapons", "fire", weapon="artillery", intensity=0.90),
            peak_db=-2.5,
            priority=70,
            max_instances=3,
        ),
        cue(
            "fire.control",
            "Control fixture broadcasts its command packet.",
            layer("cyberpunk", "program_attack", style="corporate", intensity=0.68),
            peak_db=-7.0,
        ),
        cue(
            "fire.antiair",
            "Fast anti-air autocannon report.",
            layer("weapons", "fire", weapon="autocannon", intensity=0.70),
            variations=3,
            peak_db=-4.5,
            max_instances=6,
            cooldown_ms=28,
        ),
        cue(
            "fire.disable",
            "Disabling electromagnetic pulse.",
            layer("cyberpunk", "emp_blast", style="electric", intensity=0.78),
            peak_db=-4.0,
            priority=68,
        ),
        # Impacts: recording-driven contact where material matters --------
        cue(
            "impact.impact",
            "Conventional projectile contact on machine plate.",
            layer(
                "projectiles",
                "impact",
                projectile="bullet",
                material="metal",
                intensity=0.66,
            ),
            variations=3,
            peak_db=-5.0,
            max_instances=8,
            cooldown_ms=18,
        ),
        cue(
            "impact.pierce",
            "Heavy penetrator contact on machine plate.",
            layer(
                "projectiles",
                "impact",
                projectile="shell",
                material="metal",
                intensity=0.78,
            ),
            variations=3,
            peak_db=-3.5,
            max_instances=6,
            cooldown_ms=22,
        ),
        cue(
            "impact.splash",
            "Splash-damage projectile burst.",
            layer(
                "projectiles",
                "impact",
                projectile="shell",
                material="water",
                intensity=0.72,
            ),
            layer("weapons", "explosion", 0.35, explosive="grenade", intensity=0.58),
            variations=3,
            peak_db=-3.0,
            max_instances=5,
            cooldown_ms=25,
        ),
        cue(
            "impact.deflect",
            "Armour deflects a projectile.",
            layer(
                "projectiles",
                "impact",
                0.38,
                projectile="bullet",
                material="metal",
                intensity=0.55,
            ),
            layer(
                "weapons",
                "ricochet",
                0.88,
                projectile="bullet",
                material="metal",
                intensity=0.70,
            ),
            variations=3,
            peak_db=-5.0,
            max_instances=7,
            cooldown_ms=18,
        ),
        cue(
            "impact.shield_break",
            "Machine shield collapses.",
            layer("quirky-rpg", "shield_break", style="psychedelic", intensity=0.82),
            variations=3,
            peak_db=-4.0,
            priority=65,
        ),
        cue(
            "impact.hardened",
            "Projectile glances from hardened armour.",
            layer(
                "projectiles",
                "impact",
                projectile="bullet",
                material="stone",
                intensity=0.66,
            ),
            variations=3,
            peak_db=-5.0,
            max_instances=7,
            cooldown_ms=18,
        ),
        # Array units -----------------------------------------------------
        cue(
            "unit.spawn",
            "Array unit resolves into the lane.",
            layer("cyberpunk", "ice_spawn", style="corporate", intensity=0.64),
            variations=3,
            peak_db=-8.0,
            max_instances=6,
            cooldown_ms=25,
        ),
        cue(
            "unit.death",
            "Array chassis shuts down.",
            layer("cyberpunk", "runner_downed", style="noir", intensity=0.68),
            variations=3,
            peak_db=-6.0,
            max_instances=8,
            cooldown_ms=20,
        ),
        cue(
            "unit.split",
            "Splitter chassis becomes two contacts.",
            layer("quirky-rpg", "mimic", style="psychedelic", intensity=0.72),
            variations=3,
            peak_db=-6.0,
            max_instances=6,
            cooldown_ms=25,
        ),
        cue(
            "unit.boss_entrance",
            "Siege Frame enters the block.",
            layer("ambience", "mechanical", 0.30, seconds=2.0, intensity=0.88),
            layer("quirky-rpg", "boss_start", 0.92, style="psychedelic", intensity=0.88),
            peak_db=-2.5,
            priority=95,
            max_instances=1,
        ),
        cue(
            "unit.boss_split",
            "Boss armour separates into new chassis.",
            layer("quirky-rpg", "strange_event", style="psychedelic", intensity=0.84),
            layer("cyberpunk", "ice_spawn", 0.48, 0.05, style="noir", intensity=0.70),
            peak_db=-3.5,
            priority=85,
        ),
        cue(
            "unit.emit",
            "Siege Frame emits escorts.",
            layer("cyberpunk", "transport_arrive", style="corporate", intensity=0.72),
            variations=3,
            peak_db=-5.0,
            max_instances=4,
            cooldown_ms=35,
        ),
        cue(
            "unit.suppressor_halt",
            "Suppressor halts and acquires a fixture.",
            layer("cyberpunk", "overwatch_ready", style="noir", intensity=0.68),
            variations=3,
            peak_db=-7.0,
            priority=62,
        ),
        # Fixtures --------------------------------------------------------
        cue(
            "fixture.hit",
            "Machine fire strikes a resident fixture.",
            layer("combat", "strike", weapon="mace", target="plate"),
            variations=3,
            peak_db=-5.0,
            max_instances=7,
            cooldown_ms=20,
        ),
        cue(
            "fixture.critical",
            "A fixture crosses its critical-integrity threshold.",
            layer("ui", "warning", style="acoustic"),
            variations=3,
            peak_db=-6.0,
            priority=78,
            max_instances=2,
        ),
        cue(
            "fixture.destroyed",
            "An improvised fixture comes apart.",
            layer("quirky-rpg", "gadget_break", 0.82, style="suburban", intensity=0.86),
            layer("ambience", "rockfall", 0.42, 0.025, intensity=0.38),
            variations=3,
            peak_db=-2.5,
            priority=88,
        ),
        cue(
            "fixture.repaired",
            "A fixture repair completes.",
            layer("quirky-rpg", "gadget_repair", 0.94, style="suburban", intensity=0.78),
            layer("water", "bubble", 0.12, 0.04, mass="droplet", intensity=0.42),
            variations=3,
            peak_db=-6.0,
            priority=60,
        ),
        # Game state ------------------------------------------------------
        cue(
            "state.wave_incoming",
            "The next Array wave enters.",
            layer("cyberpunk", "combat_start", style="corporate", intensity=0.80),
            peak_db=-4.0,
            priority=90,
            max_instances=1,
        ),
        cue(
            "state.wave_cleared",
            "The current wave is clear.",
            layer("quirky-rpg", "run_success", style="suburban", intensity=0.76),
            peak_db=-5.0,
            priority=85,
            max_instances=1,
        ),
        cue(
            "state.integrity_lost",
            "A machine leaks through and block integrity falls.",
            layer("quirky-rpg", "player_hit", style="suburban", intensity=0.76),
            variations=3,
            peak_db=-4.5,
            priority=82,
        ),
        cue(
            "state.integrity_critical",
            "Block integrity becomes critical.",
            layer("ui", "warning", 0.68, style="scifi"),
            layer(
                "quirky-rpg",
                "mortal_warning",
                0.72,
                0.04,
                style="suburban",
                intensity=0.84,
            ),
            peak_db=-3.0,
            priority=96,
            max_instances=1,
        ),
        cue(
            "state.victory",
            "The block survives the campaign.",
            layer("ui", "win", 0.58, style="acoustic"),
            layer("quirky-rpg", "victory", 0.82, 0.06, style="suburban", intensity=0.88),
            peak_db=-2.5,
            priority=100,
            max_instances=1,
        ),
        cue(
            "state.defeat",
            "Block integrity reaches zero.",
            layer("ui", "lose", 0.62, style="acoustic"),
            layer("quirky-rpg", "party_down", 0.78, 0.06, style="suburban", intensity=0.84),
            peak_db=-3.5,
            priority=100,
            max_instances=1,
        ),
        cue(
            "state.pause",
            "Pause the simulation.",
            layer("ui", "pause", style="scifi"),
            peak_db=-8.0,
            priority=90,
            max_instances=1,
        ),
        cue(
            "state.campaign_unlock",
            "Unlock the Cordon campaign.",
            layer("quirky-rpg", "secret_found", style="suburban", intensity=0.86),
            peak_db=-4.0,
            priority=98,
            max_instances=1,
        ),
    )
    return specs


class SoundGenerator:
    """Narrow adapter over the eight approved soundgen providers."""

    def __init__(self, root: Path) -> None:
        self.root = root.expanduser().resolve()
        package = self.root / "python_sound_generator"
        if not (package / "__init__.py").is_file():
            raise ValueError(f"not a python_sound_generator checkout: {self.root}")
        sys.path.insert(0, str(self.root))

        import numpy as np
        from python_sound_generator._audio import assets, dsp, loops, runtime
        from python_sound_generator.generators import cyberpunk_sound_generator
        from python_sound_generator.generators import projectile_sounds
        from python_sound_generator.generators import quirky_rpg_sound_generator
        from python_sound_generator.generators import ui_game_state_generator
        from python_sound_generator.generators import water_sounds_generator
        from python_sound_generator.generators import weapons_destruction_generator
        from python_sound_generator.generators.combat_generators.lib import (
            cues as combat_cues,
        )
        from python_sound_generator.generators.environment_ambience_generator import (
            Options as AmbienceOptions,
            render as render_ambience,
        )

        self.np = np
        self.assets = assets
        self.dsp = dsp
        self.loops = loops
        self.runtime = runtime
        self.ui = ui_game_state_generator
        self.weapons = weapons_destruction_generator
        self.combat = combat_cues
        self.projectiles = projectile_sounds
        self.water = water_sounds_generator
        self.cyberpunk = cyberpunk_sound_generator
        self.quirky = quirky_rpg_sound_generator
        self.AmbienceOptions = AmbienceOptions
        self.render_ambience = render_ambience

    def revision(self) -> str:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.root,
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    @staticmethod
    def _as_list(samples: Sequence[float]) -> list[float]:
        return [float(value) for value in samples]

    @staticmethod
    def _unused(provider: str, options: dict[str, Any]) -> None:
        if options:
            raise ValueError(f"unused {provider} options: {sorted(options)}")

    def render_layer(
        self, item: Layer, seed: int
    ) -> tuple[list[float], tuple[str, ...]]:
        options = dict(item.options)
        provider = item.provider
        if provider == "ui":
            style = self.ui.get_style(str(options.pop("style", "scifi")))
            step = int(options.pop("step", 0))
            self._unused(provider, options)
            samples = self.ui.render(
                self.ui.CUES[item.cue],
                style,
                self.np.random.default_rng(seed),
                step,
            )
            return self._as_list(samples), ()
        if provider == "weapons":
            weapon = self.weapons.FIREARMS[str(options.pop("weapon", "rifle"))]
            projectile = self.weapons.PROJECTILES[
                str(options.pop("projectile", "bullet"))
            ]
            explosive = self.weapons.EXPLOSIVES[
                str(options.pop("explosive", "grenade"))
            ]
            material = self.weapons.MATERIALS[
                str(options.pop("material", "metal"))
            ]
            native = self.weapons.Options(
                weapon=weapon,
                projectile=projectile,
                explosive=explosive,
                material=material,
                shots=int(options.pop("shots", 4)),
                intensity=float(options.pop("intensity", 0.85)),
            )
            self._unused(provider, options)
            samples = self.weapons.render(item.cue, native, random.Random(seed))
            return self._as_list(samples), ()
        if provider == "combat":
            native = self.combat.Options(
                weapon=self.combat.WEAPONS[
                    str(options.pop("weapon", "mace"))
                ],
                target=self.combat.TARGETS[
                    str(options.pop("target", "plate"))
                ],
                surface=str(options.pop("surface", "stone")),
                armored=bool(options.pop("armored", True)),
            )
            self._unused(provider, options)
            samples = self.combat.render(item.cue, native, random.Random(seed))
            return self._as_list(samples), ()
        if provider == "projectiles":
            native = self.projectiles.Options(
                projectile=self.projectiles.PROJECTILES[
                    str(options.pop("projectile", "bullet"))
                ],
                material=self.projectiles.MATERIALS[
                    str(options.pop("material", "metal"))
                ],
                intensity=float(options.pop("intensity", 0.85)),
            )
            self._unused(provider, options)
            result = self.projectiles.render(
                item.cue, native, random.Random(seed)
            )
        elif provider == "water":
            native = self.water.Options(
                mass=self.water.MASSES[str(options.pop("mass", "body"))],
                intensity=float(options.pop("intensity", 0.8)),
                seconds=options.pop("seconds", None),
            )
            self._unused(provider, options)
            result = self.water.render(
                item.cue, native, self.np.random.default_rng(seed)
            )
        elif provider == "cyberpunk":
            result = self.cyberpunk.generate_with_sources(
                item.cue,
                style=str(options.pop("style", "electric")),
                intensity=float(options.pop("intensity", 0.85)),
                foley_mix=float(options.pop("foley_mix", 0.0)),
                seed=seed,
            )
            self._unused(provider, options)
        elif provider == "quirky-rpg":
            result = self.quirky.generate_with_sources(
                item.cue,
                style=str(options.pop("style", "suburban")),
                intensity=float(options.pop("intensity", 0.85)),
                foley_mix=float(options.pop("foley_mix", 0.0)),
                seed=seed,
            )
            self._unused(provider, options)
        elif provider == "ambience":
            native = self.AmbienceOptions(**options)
            samples = self.render_ambience(
                item.cue, native, random.Random(seed)
            )
            return self._as_list(samples), ()
        else:
            raise ValueError(f"unapproved audio provider {provider!r}")
        return self._as_list(result.samples), tuple(result.sources)

    def recording_source(
        self, provider: str, source: str
    ) -> tuple[str, Path, str]:
        """Resolve a reported recording to its logical owner and ledger."""

        if source.startswith(
            ("generator:", "generated-space:", "suppression-balance:")
        ):
            raise ValueError("procedural markers are not recording sources")

        owner_by_provider = {
            "projectiles": "projectile_sounds",
            "water": "water_sounds_generator",
            "cyberpunk": "cyberpunk_sound_generator",
            "quirky-rpg": "quirky_rpg_sound_generator",
        }
        generators = (
            self.root / "python_sound_generator" / "generators"
        )
        catalog = self.assets.AssetCatalog.default()

        candidates: list[tuple[str, Path]] = []
        if "/" in source:
            logical = Path(source).as_posix()
            physical = catalog.resolve(generators / logical)
            if physical.is_file():
                candidates.append((logical, physical))
        else:
            preferred = owner_by_provider.get(provider)
            if preferred is not None:
                logical = f"{preferred}/sources/{source}"
                physical = catalog.resolve(generators / logical)
                if physical.is_file():
                    candidates.append((logical, physical))
            if not candidates:
                for key in sorted(catalog.aliases):
                    if key.endswith(f"/sources/{source}"):
                        candidates.append(
                            (key, catalog.resolve(generators / key))
                        )
                for physical in sorted(generators.glob(f"*/sources/{source}")):
                    logical = physical.relative_to(generators).as_posix()
                    candidates.append((logical, physical.resolve()))

        unique: dict[tuple[str, str], tuple[str, Path]] = {}
        for logical, physical in candidates:
            unique[(logical, str(physical.resolve()))] = (
                logical,
                physical.resolve(),
            )
        candidates = list(unique.values())
        if not candidates:
            raise ValueError(f"cannot resolve recording source {source!r}")

        # Prefer the named provider.  Otherwise equivalent shared paths can
        # legitimately have several logical aliases, so take the sorted first.
        candidates.sort(key=lambda item: item[0])
        logical, physical = candidates[0]
        preferred = owner_by_provider.get(provider)
        for candidate in candidates:
            if preferred and candidate[0].startswith(f"{preferred}/"):
                logical, physical = candidate
                break

        alias = catalog.aliases.get(logical)
        if alias is not None and alias.provenance:
            ledger = alias.provenance
        else:
            owner = Path(logical).parts[0]
            ledger = f"generators/{owner}/sources/provenance.json"
        ledger_path = (
            self.root / "python_sound_generator" / ledger
        )
        if not ledger_path.is_file():
            raise ValueError(f"missing source ledger for {logical}: {ledger}")
        return logical, physical, ledger


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _decode_score(source: Path) -> array.array[float]:
    result = subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-nostdin",
            "-i",
            str(source),
            "-map",
            "0:a:0",
            "-ac",
            "1",
            "-ar",
            str(SAMPLE_RATE),
            "-f",
            "f32le",
            "-c:a",
            "pcm_f32le",
            "pipe:1",
        ],
        check=True,
        capture_output=True,
    )
    decoded: array.array[float] = array.array("f")
    decoded.frombytes(result.stdout)
    if sys.byteorder != "little":
        decoded.byteswap()
    if len(decoded) < SAMPLE_RATE:
        raise ValueError("decoded score is unexpectedly short")
    return decoded


def _detect_score_boundaries(
    generator: SoundGenerator, score: array.array[float]
) -> tuple[list[int], list[dict[str, float]]]:
    """Detect large phrase changes using broad-band before/after novelty."""

    np = generator.np
    values = np.asarray(score, dtype=np.float32)
    hop = SAMPLE_RATE // 2
    frame_count = len(values) // hop
    frames = values[: frame_count * hop].reshape(frame_count, hop)
    rms = np.sqrt(np.mean(frames * frames, axis=1) + 1.0e-12)

    # Decimate once for analysis only.  Eight logarithmic bands plus level are
    # enough to distinguish orchestration changes without importing a music
    # analysis package.
    decimated = frames[:, : hop - (hop % 2) : 2]
    spectrum = np.abs(
        np.fft.rfft(
            decimated * np.hanning(decimated.shape[1]),
            axis=1,
        )
    )
    frequencies = np.fft.rfftfreq(decimated.shape[1], 2.0 / SAMPLE_RATE)
    edges = np.geomspace(40.0, 10_000.0, 9)
    bands = np.stack(
        [
            np.log10(
                spectrum[:, (frequencies >= low) & (frequencies < high)]
                .mean(axis=1)
                + 1.0e-9
            )
            for low, high in zip(edges[:-1], edges[1:])
        ],
        axis=1,
    )
    features = np.column_stack((np.log10(rms), bands))
    features = (features - features.mean(axis=0)) / (
        features.std(axis=0) + 1.0e-9
    )

    context = 8  # four seconds on either side
    novelty = np.zeros(frame_count)
    for index in range(context, frame_count - context):
        before = features[index - context : index].mean(axis=0)
        after = features[index : index + context].mean(axis=0)
        novelty[index] = np.linalg.norm(before - after)

    boundaries: list[int] = []
    rows: list[dict[str, float]] = []
    for lower, upper in BOUNDARY_SEARCH_WINDOWS:
        first = max(context, round(lower * 2.0))
        last = min(frame_count - context, round(upper * 2.0))
        if first >= last:
            raise ValueError("score is too short for boundary search windows")
        frame = max(range(first, last), key=lambda index: float(novelty[index]))
        coarse = frame * hop

        # Move at most 30 ms to the quietest zero crossing so the reported cut
        # is sample-accurate rather than merely half-second accurate.
        radius = round(0.030 * SAMPLE_RATE)
        start = max(1, coarse - radius)
        stop = min(len(values) - 1, coarse + radius)
        crossing = min(
            range(start, stop),
            key=lambda index: (
                abs(float(values[index])),
                abs(float(values[index] - values[index - 1])),
            ),
        )
        boundaries.append(crossing)
        rows.append(
            {
                "seconds": round(crossing / SAMPLE_RATE, 6),
                "novelty": round(float(novelty[frame]), 6),
                "search_start_seconds": lower,
                "search_end_seconds": upper,
            }
        )
    if boundaries != sorted(boundaries) or len(set(boundaries)) != len(boundaries):
        raise ValueError("detected score boundaries are not strictly increasing")
    return boundaries, rows


def _fold_loop(
    generator: SoundGenerator,
    score: array.array[float],
    start: int,
    end: int,
    peak_db: float,
) -> tuple[list[float], float]:
    """Adapt Legend of Kilix's after-boundary-over-opening phrase fold."""

    frame_count = end - start
    fade_frames = min(
        round(FOLD_SECONDS * SAMPLE_RATE),
        frame_count // 3,
        len(score) - end,
    )
    if frame_count < 2 or fade_frames < 2:
        raise ValueError("score section is too short to fold")
    output = [float(value) for value in score[start:end]]
    for index in range(fade_frames):
        ratio = index / max(1, fade_frames - 1)
        output[index] = (
            output[index] * math.sqrt(ratio)
            + float(score[end + index]) * math.sqrt(1.0 - ratio)
        )

    # Phrase changes can be click-continuous yet several dB apart.  Match the
    # folded head's 50 ms energy to the section tail, while leaving the exact
    # first sample at unity gain: the wrap therefore remains the original
    # adjacent source pair.  The gain settles over 1.5 ms, then returns to
    # unity across the rest of the folded phrase.
    seam_width = min(frame_count // 2, round(0.05 * SAMPLE_RATE))
    head_rms = math.sqrt(
        sum(value * value for value in output[:seam_width]) / seam_width
    )
    tail_rms = math.sqrt(
        sum(value * value for value in output[-seam_width:]) / seam_width
    )
    match_gain = max(0.05, min(20.0, tail_rms / max(head_rms, 1.0e-12)))
    settle_frames = min(fade_frames // 4, round(0.0015 * SAMPLE_RATE))
    for index in range(fade_frames):
        if index < settle_frames:
            phase = index / max(1, settle_frames - 1)
            smooth = phase * phase * (3.0 - 2.0 * phase)
            gain = 1.0 + (match_gain - 1.0) * smooth
        else:
            phase = (index - settle_frames) / max(
                1, fade_frames - settle_frames - 1
            )
            smooth = phase * phase * (3.0 - 2.0 * phase)
            gain = match_gain + (1.0 - match_gain) * smooth
        output[index] *= gain
    return (
        generator.loops.master(output, peak_db, drive=1.12),
        fade_frames / SAMPLE_RATE,
    )


def _one_shot_music(
    generator: SoundGenerator,
    score: array.array[float],
    start: int,
    end: int,
    peak_db: float,
) -> list[float]:
    samples = [float(value) for value in score[start:end]]
    return generator.dsp.master(
        samples,
        peak_db,
        drive=1.12,
        fade_in_ms=8.0,
        fade_out_ms=650.0,
    )


def _read_pcm(path: Path) -> list[float]:
    with path.open("rb") as handle:
        header = handle.read(12)
    if header[:4] != b"RIFF" or header[8:12] != b"WAVE":
        raise ValueError(f"not RIFF/WAVE: {path}")
    with wave.open(str(path), "rb") as wav:
        if (
            wav.getnchannels() != 1
            or wav.getframerate() != SAMPLE_RATE
            or wav.getsampwidth() != 2
            or wav.getcomptype() != "NONE"
        ):
            raise ValueError(f"wrong generated WAV format: {path}")
        frames = array.array("h")
        frames.frombytes(wav.readframes(wav.getnframes()))
    if sys.byteorder != "little":
        frames.byteswap()
    return [value / 32768.0 for value in frames]


def _metrics(path: Path) -> tuple[list[float], dict[str, Any]]:
    samples = _read_pcm(path)
    peak = max((abs(value) for value in samples), default=0.0)
    rms = math.sqrt(
        sum(value * value for value in samples) / max(1, len(samples))
    )
    return samples, {
        "frames": len(samples),
        "duration_seconds": round(len(samples) / SAMPLE_RATE, 6),
        "peak": round(peak, 8),
        "rms": round(rms, 8),
        "sha256": _sha256(path),
    }


def _seam(samples: Sequence[float]) -> dict[str, Any]:
    if len(samples) < 2:
        raise ValueError("loop needs at least two PCM samples")
    largest = max(
        abs(samples[index] - samples[index - 1])
        for index in range(1, len(samples))
    )
    step = abs(samples[0] - samples[-1])
    width = max(1, min(len(samples) // 2, round(0.05 * SAMPLE_RATE)))
    head = math.sqrt(sum(value * value for value in samples[:width]) / width)
    tail = math.sqrt(sum(value * value for value in samples[-width:]) / width)
    ratio = step / largest if largest > 1.0e-12 else 0.0
    level = tail / head if head > 1.0e-12 else math.inf
    seamless = ratio <= 1.0 and 0.25 <= level <= 4.0
    return {
        "step": round(step, 8),
        "largest_step": round(largest, 8),
        "step_ratio": round(ratio, 8),
        "head_rms": round(head, 8),
        "tail_rms": round(tail, 8),
        "level_ratio": round(level, 8),
        "seamless": seamless,
    }


def _mix(
    generator: SoundGenerator,
    rendered: Sequence[tuple[Layer, list[float]]],
    peak_db: float,
) -> list[float]:
    length = max(
        round(item.offset * SAMPLE_RATE) + len(samples)
        for item, samples in rendered
    )
    output = [0.0] * length
    for item, samples in rendered:
        generator.dsp.add_at(output, item.offset, samples, item.gain)
    return generator.dsp.master(
        output,
        peak_db,
        drive=1.12,
        fade_in_ms=0.5,
        fade_out_ms=24.0,
    )


def _sfx_filename(spec: CueSpec, variation: int) -> Path:
    group, stem = spec.cue_id.split(".", 1)
    suffix = f"_{variation:02d}" if spec.variations > 1 else ""
    return Path("sfx") / group / f"{stem}{suffix}.wav"


def _provenance(
    generator: SoundGenerator,
    existing: dict[str, Any],
    revision: str,
    base_seed: int,
    sources: dict[str, tuple[Path, str]],
) -> dict[str, Any]:
    """Preserve the pinned score record and append the effects source ledger."""

    score_path = GAME_ROOT / SCORE_SOURCE
    score = existing.get("score")
    if not isinstance(score, dict):
        raise ValueError("audio provenance has no pinned score record")
    if score.get("sha256") != _sha256(score_path):
        raise ValueError("retained score checksum differs from provenance")

    catalog = generator.assets.AssetCatalog.default()
    generators = generator.root / "python_sound_generator" / "generators"
    source_rows: dict[str, Any] = {}
    ledgers: dict[str, Any] = {}
    for logical, (physical, ledger) in sorted(sources.items()):
        ledger_path = generator.root / "python_sound_generator" / ledger
        payload = json.loads(ledger_path.read_text(encoding="utf-8"))
        ledgers.setdefault(ledger, payload)
        source_rows[logical] = {
            "sha256": _sha256(physical),
            "bytes": physical.stat().st_size,
            "provenance_ledger": ledger,
        }
        alias = catalog.aliases.get(logical)
        if alias is not None:
            source_rows[logical]["storage"] = (
                "shared content-addressed asset catalog"
            )
            source_rows[logical]["catalog_file"] = alias.file
        else:
            local = generators / logical
            source_rows[logical]["storage"] = (
                "provider-owned source file"
                if local.resolve() == physical.resolve()
                else "provider-shared source file"
            )

    collections: list[dict[str, Any]] = []
    seen_collections: set[tuple[str, str]] = set()
    for ledger, payload in sorted(ledgers.items()):
        for collection in payload.get("collections", []):
            if not isinstance(collection, dict):
                raise ValueError(f"invalid collection in {ledger}")
            key = (ledger, str(collection.get("name", "")))
            if key in seen_collections:
                continue
            seen_collections.add(key)
            summary = {
                key: collection[key]
                for key in (
                    "name",
                    "author",
                    "license",
                    "source_page",
                    "original_archive",
                    "original_archive_sha256",
                )
                if key in collection
            }
            summary["owner_provenance"] = ledger
            collections.append(summary)

    result = dict(existing)
    result["effects"] = {
        "generator": "python_sound_generator (soundgen)",
        "status": "generated",
        "revision": revision,
        "base_seed": base_seed,
        "seed_scheme": "stable-v2",
        "statement": (
            "Procedurally synthesised or built from the approved "
            "CC0/public-domain recordings enumerated in this ledger."
        ),
        "recording_sources": source_rows,
        "authoritative_ledgers": ledgers,
    }
    # These three fields are the source-ledger interface consumed by
    # kilix_game_tools validate-audio.
    result["collections"] = collections
    result["missing"] = []
    result["invalid"] = []
    return result


def build(
    generator_root: Path,
    output: Path,
    base_seed: int,
    *,
    verbose: bool = True,
) -> None:
    generator = SoundGenerator(generator_root)
    output = output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)

    stable_path = GAME_ROOT / STABLE_IDS
    stable_payload = json.loads(stable_path.read_text(encoding="utf-8"))
    stable_cues = stable_payload.get("cues")
    stable_scenes = stable_payload.get("scenes")
    if not isinstance(stable_cues, dict) or not isinstance(stable_scenes, dict):
        raise ValueError("content/stable_ids.json has no cue/scene maps")

    specs = _sfx_specs()
    spec_ids = [item.cue_id for item in specs]
    if len(spec_ids) != len(set(spec_ids)):
        raise ValueError("duplicate SFX cue specification")
    if set(spec_ids) != set(stable_cues):
        missing = sorted(set(stable_cues) - set(spec_ids))
        extra = sorted(set(spec_ids) - set(stable_cues))
        raise ValueError(f"frozen cue coverage mismatch: missing={missing} extra={extra}")

    score_path = GAME_ROOT / SCORE_SOURCE
    provenance_path = output / "provenance.json"
    existing_provenance = json.loads(
        provenance_path.read_text(encoding="utf-8")
    )
    expected_score_hash = existing_provenance.get("score", {}).get("sha256")
    if _sha256(score_path) != expected_score_hash:
        raise ValueError("retained MiniMax score failed its pinned checksum")

    score = _decode_score(score_path)
    boundaries, boundary_rows = _detect_score_boundaries(generator, score)
    points = [0, *boundaries, len(score)]
    # The score's dark defeat passage precedes its confrontation and bright
    # resolution, so source order is intentionally not scene-id order.
    source_order = ("title", "build", "wave", "defeat", "boss", "victory")
    section_by_scene = {
        name: (points[index], points[index + 1])
        for index, name in enumerate(source_order)
    }
    music_levels = {
        "title": -11.0,
        "build": -10.0,
        "wave": -8.5,
        "boss": -7.0,
        "victory": -6.0,
        "defeat": -7.5,
    }
    looping_scenes = {"title", "build", "wave", "boss"}

    artifacts: list[dict[str, Any]] = []
    cue_index: dict[str, Any] = {}
    expected_files: set[Path] = set()
    used_sources: dict[str, tuple[Path, str]] = {}

    total_sfx_files = sum(item.variations for item in specs)
    total_files = total_sfx_files + len(stable_scenes)
    completed = 0

    for scene in ("title", "build", "wave", "boss", "victory", "defeat"):
        if scene not in stable_scenes:
            raise ValueError(f"missing frozen scene id: {scene}")
        start, end = section_by_scene[scene]
        looped = scene in looping_scenes
        if looped:
            samples, folded = _fold_loop(
                generator, score, start, end, music_levels[scene]
            )
        else:
            samples = _one_shot_music(
                generator, score, start, end, music_levels[scene]
            )
            folded = 0.0
        relative = Path("music") / f"{scene}.wav"
        target = output / relative
        expected_files.add(target)
        generator.dsp.write_wav(target, samples)
        pcm, measured = _metrics(target)
        row: dict[str, Any] = {
            "cue_id": f"music.{scene}",
            "file": relative.as_posix(),
            "variation": 1,
            **measured,
            "loop": looped,
            "sources": [],
            "recording_sources": [],
            "layers": [
                {
                    "provider": "minimax_music",
                    "source_cue": "block-evening",
                    "source": SCORE_SOURCE.as_posix(),
                    "start_seconds": round(start / SAMPLE_RATE, 6),
                    "end_seconds": round(end / SAMPLE_RATE, 6),
                    "fold_seconds": round(folded, 6),
                }
            ],
        }
        if looped:
            row["seam"] = _seam(pcm)
            if not row["seam"]["seamless"]:
                raise ValueError(f"music loop seam failed: {scene}")
        artifacts.append(row)
        cue_index[f"music.{scene}"] = {
            "description": f"{scene.capitalize()} score scene.",
            "scene_id": int(stable_scenes[scene]),
            "files": [relative.as_posix()],
            "variations": 1,
            "loop": looped,
            "bus": "music",
            "gain": 0.62,
            "priority": 10 if looped else 100,
            "max_instances": 1,
            "cooldown_ms": 0,
        }
        completed += 1
        if verbose:
            print(f"[{completed:02d}/{total_files:02d}] {relative}", flush=True)

    for spec in specs:
        files: list[str] = []
        for variation in range(1, spec.variations + 1):
            relative = _sfx_filename(spec, variation)
            target = output / relative
            expected_files.add(target)
            rendered: list[tuple[Layer, list[float]]] = []
            layer_rows: list[dict[str, Any]] = []
            artifact_sources: list[str] = []
            for layer_index, item in enumerate(spec.layers, start=1):
                seed = generator.runtime.derive_seed(
                    base_seed,
                    provider=item.provider,
                    cue=item.cue,
                    filename=f"{spec.cue_id}/layer-{layer_index}",
                    variation=variation,
                )
                samples, reported_sources = generator.render_layer(item, seed)
                normalised: list[str] = []
                procedural: list[str] = []
                for source in reported_sources:
                    if source.startswith(
                        (
                            "generator:",
                            "generated-space:",
                            "suppression-balance:",
                        )
                    ):
                        procedural.append(source)
                        continue
                    logical, physical, ledger = generator.recording_source(
                        item.provider, source
                    )
                    normalised.append(logical)
                    used_sources[logical] = (physical, ledger)
                artifact_sources.extend(normalised)
                rendered.append((item, samples))
                layer_rows.append(
                    {
                        "provider": item.provider,
                        "source_cue": item.cue,
                        "seed": seed,
                        "gain": item.gain,
                        "offset_seconds": item.offset,
                        "options": item.options,
                        "recording_sources": list(dict.fromkeys(normalised)),
                        "procedural_sources": list(dict.fromkeys(procedural)),
                    }
                )
            samples = _mix(generator, rendered, spec.peak_db)
            generator.dsp.write_wav(target, samples)
            _, measured = _metrics(target)
            unique_sources = list(dict.fromkeys(artifact_sources))
            artifacts.append(
                {
                    "cue_id": spec.cue_id,
                    "file": relative.as_posix(),
                    "variation": variation,
                    **measured,
                    "loop": False,
                    "sources": unique_sources,
                    "recording_sources": unique_sources,
                    "layers": layer_rows,
                }
            )
            files.append(relative.as_posix())
            completed += 1
            if verbose:
                print(
                    f"[{completed:02d}/{total_files:02d}] {relative}",
                    flush=True,
                )
        cue_index[spec.cue_id] = {
            "description": spec.description,
            "stable_id": int(stable_cues[spec.cue_id]),
            "files": files,
            "variations": spec.variations,
            "loop": False,
            "bus": "sfx",
            "gain": 1.0,
            "priority": spec.priority,
            "max_instances": spec.max_instances,
            "cooldown_ms": spec.cooldown_ms,
        }

    for stale in output.rglob("*.wav"):
        if stale not in expected_files:
            stale.unlink()

    revision = generator.revision()
    provenance = _provenance(
        generator,
        existing_provenance,
        revision,
        base_seed,
        used_sources,
    )
    provenance_path.write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    total_duration = sum(float(row["duration_seconds"]) for row in artifacts)
    music_duration = sum(
        float(row["duration_seconds"])
        for row in artifacts
        if str(row["cue_id"]).startswith("music.")
    )
    manifest = {
        "schema_version": 1,
        "game": "pleb-tower",
        "generator": {
            "name": "python-sound-generator",
            "revision": revision,
            "base_seed": base_seed,
            "seed_scheme": "stable-v2",
            "rebuild": "python3 tools/generate_audio.py",
        },
        "format": {
            "container": "RIFF/WAVE",
            "encoding": "signed 16-bit PCM little-endian",
            "sample_rate": SAMPLE_RATE,
            "channels": 1,
            "bits_per_sample": 16,
        },
        # Required by the shared validator; this describes recording inputs,
        # not the separately-provenanced original generated score.
        "source_licensing": "CC0 1.0",
        "source_licensing_scope": (
            "All recording inputs used by effects are CC0/public domain. "
            "The original generated score has its own pinned provenance."
        ),
        "source_policy": (
            "Original pinned score, procedural synthesis, and catalogued "
            "CC0/public-domain recording inputs only."
        ),
        "commercial_reference_audio_used": False,
        "freesound_download_required": False,
        "stable_ids": {
            "file": STABLE_IDS.as_posix(),
            "sha256": _sha256(stable_path),
        },
        "score_source": {
            "file": SCORE_SOURCE.as_posix(),
            "sha256": _sha256(score_path),
            "decoded_duration_seconds": round(len(score) / SAMPLE_RATE, 6),
            "boundary_detection": {
                "method": (
                    "four-second before/after broad-band spectral and RMS "
                    "novelty, refined to a nearby zero crossing"
                ),
                "analysis_hop_seconds": 0.5,
                "boundaries": boundary_rows,
                "source_section_order": list(source_order),
            },
        },
        "coverage": {
            "frozen_cues": sorted(stable_cues),
            "frozen_scenes": sorted(stable_scenes),
            "approved_providers": [
                "ui",
                "weapons",
                "combat",
                "projectiles",
                "water",
                "cyberpunk",
                "quirky-rpg",
                "ambience",
            ],
        },
        "counts": {
            "logical_cues": len(cue_index),
            "sfx_cues": len(specs),
            "music_scenes": len(stable_scenes),
            "wav_files": len(artifacts),
            "seamless_loops": sum(bool(row["loop"]) for row in artifacts),
            "recording_inputs": len(used_sources),
            "total_duration_seconds": round(total_duration, 3),
            "music_duration_seconds": round(music_duration, 3),
            "sfx_duration_seconds": round(total_duration - music_duration, 3),
        },
        "buses": {
            "music": {"default_gain": 0.62},
            "sfx": {"default_gain": 1.0},
        },
        "cues": cue_index,
        "artifacts": artifacts,
        "provenance": "provenance.json",
    }
    manifest_path = output / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    print(f"sha256 {_sha256(manifest_path)}  manifest.json")
    print(f"sha256 {_sha256(provenance_path)}  provenance.json")
    print(
        f"Generated {len(artifacts)} WAV files for {len(cue_index)} logical "
        f"cues ({total_duration:.3f} s) using {len(used_sources)} vetted "
        "recording inputs."
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--generator-root",
        type=Path,
        default=(
            GAME_ROOT.parents[1]
            / "kilix-apps"
            / "python_sound_generator"
        ),
        help="python_sound_generator checkout",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=GAME_ROOT / "assets/audio",
        help="production audio output directory",
    )
    parser.add_argument("--seed", type=int, default=BASE_SEED)
    parser.add_argument(
        "--quiet", action="store_true", help="suppress per-file progress"
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    try:
        build(
            arguments.generator_root,
            arguments.out,
            arguments.seed,
            verbose=not arguments.quiet,
        )
    except (
        FileNotFoundError,
        json.JSONDecodeError,
        OSError,
        RuntimeError,
        ValueError,
        subprocess.CalledProcessError,
    ) as error:
        print(f"generate_audio.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
