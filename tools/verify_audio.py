#!/usr/bin/env python3
"""Audit Pleb Tower's shipped audio bank using only the Python standard library."""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
import subprocess
import sys
import wave
from collections import defaultdict
from pathlib import Path
from typing import Any, Sequence


SAMPLE_RATE = 44_100
GAME_ROOT = Path(__file__).resolve().parents[1]
MIN_RMS = 1.0e-5
MAX_PEAK = 0.999
MAX_STEP_RATIO = 1.0
MAX_LEVEL_DRIFT = 4.0


class AuditError(ValueError):
    """One or more release-audio invariants failed."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _object(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise AuditError(f"JSON root must be an object: {path}")
    return payload


def _safe_relative(value: object, *, suffix: str | None = None) -> Path:
    if not isinstance(value, str) or not value:
        raise AuditError(f"invalid relative path: {value!r}")
    result = Path(value)
    if result.is_absolute() or ".." in result.parts:
        raise AuditError(f"unsafe relative path: {value!r}")
    if suffix is not None and result.suffix.lower() != suffix:
        raise AuditError(f"path must end in {suffix}: {value!r}")
    return result


def _read_pcm(path: Path) -> list[float]:
    with path.open("rb") as handle:
        header = handle.read(12)
    if len(header) != 12 or header[:4] != b"RIFF" or header[8:] != b"WAVE":
        raise AuditError(f"{path}: container is not RIFF/WAVE")
    try:
        with wave.open(str(path), "rb") as wav:
            if wav.getnchannels() != 1:
                raise AuditError(f"{path}: expected mono")
            if wav.getframerate() != SAMPLE_RATE:
                raise AuditError(f"{path}: expected {SAMPLE_RATE} Hz")
            if wav.getsampwidth() != 2:
                raise AuditError(f"{path}: expected signed 16-bit PCM")
            if wav.getcomptype() != "NONE":
                raise AuditError(f"{path}: compressed WAV is not supported")
            count = wav.getnframes()
            raw = wav.readframes(count)
            if len(raw) != count * 2:
                raise AuditError(f"{path}: truncated PCM payload")
    except (EOFError, wave.Error) as error:
        raise AuditError(f"{path}: invalid WAV: {error}") from error
    integers = array.array("h")
    integers.frombytes(raw)
    if sys.byteorder != "little":
        integers.byteswap()
    return [value / 32768.0 for value in integers]


def _metrics(samples: Sequence[float]) -> dict[str, float | int]:
    peak = max((abs(value) for value in samples), default=0.0)
    rms = math.sqrt(
        sum(value * value for value in samples) / max(1, len(samples))
    )
    return {
        "frames": len(samples),
        "duration_seconds": len(samples) / SAMPLE_RATE,
        "peak": peak,
        "rms": rms,
    }


def _seam(samples: Sequence[float]) -> dict[str, float | bool]:
    if len(samples) < 2:
        raise AuditError("loop has fewer than two samples")
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
    return {
        "step": step,
        "largest_step": largest,
        "step_ratio": ratio,
        "head_rms": head,
        "tail_rms": tail,
        "level_ratio": level,
        "seamless": (
            ratio <= MAX_STEP_RATIO
            and 1.0 / MAX_LEVEL_DRIFT <= level <= MAX_LEVEL_DRIFT
        ),
    }


def _close(
    actual: float,
    expected: object,
    tolerance: float,
    context: str,
) -> None:
    if not isinstance(expected, (int, float)) or isinstance(expected, bool):
        raise AuditError(f"{context}: manifest value is not numeric")
    if not math.isfinite(float(expected)):
        raise AuditError(f"{context}: manifest value is not finite")
    if abs(actual - float(expected)) > tolerance:
        raise AuditError(
            f"{context}: measured {actual:.10g}, manifest {float(expected):.10g}"
        )


def _verify_format(manifest: dict[str, Any]) -> None:
    expected = {
        "container": "RIFF/WAVE",
        "encoding": "signed 16-bit PCM little-endian",
        "sample_rate": SAMPLE_RATE,
        "channels": 1,
        "bits_per_sample": 16,
    }
    if manifest.get("format") != expected:
        raise AuditError("manifest format contract is not the runtime PCM contract")


def _provider_file(
    generator_root: Path,
    logical: str,
    catalog: dict[str, Any],
) -> Path:
    alias = catalog.get("aliases", {}).get(logical)
    if isinstance(alias, dict):
        stored = _safe_relative(alias.get("file"))
        return generator_root / "python_sound_generator" / "assets" / stored
    return (
        generator_root
        / "python_sound_generator"
        / "generators"
        / _safe_relative(logical)
    )


def _verify_sources(
    manifest: dict[str, Any],
    provenance: dict[str, Any],
    artifact_sources: set[str],
    generator_root: Path,
) -> None:
    effects = provenance.get("effects")
    if not isinstance(effects, dict):
        raise AuditError("provenance has no effects ledger")
    rows = effects.get("recording_sources")
    ledgers = effects.get("authoritative_ledgers")
    if not isinstance(rows, dict) or not isinstance(ledgers, dict):
        raise AuditError("effects recording-source ledger is malformed")
    if set(rows) != artifact_sources:
        missing = sorted(artifact_sources - set(rows))
        extra = sorted(set(rows) - artifact_sources)
        raise AuditError(
            f"source ledger mismatch: missing={missing} extra={extra}"
        )

    catalog_path = (
        generator_root
        / "python_sound_generator"
        / "assets"
        / "catalog.json"
    )
    catalog = _object(catalog_path)
    for logical, row in rows.items():
        _safe_relative(logical, suffix=".wav")
        if not isinstance(row, dict):
            raise AuditError(f"source row is not an object: {logical}")
        source_path = _provider_file(generator_root, logical, catalog)
        if not source_path.is_file():
            raise AuditError(f"recording source is missing: {logical}")
        if source_path.stat().st_size != row.get("bytes"):
            raise AuditError(f"recording byte count mismatch: {logical}")
        if _sha256(source_path) != row.get("sha256"):
            raise AuditError(f"recording checksum mismatch: {logical}")
        ledger_name = row.get("provenance_ledger")
        ledger_relative = _safe_relative(ledger_name, suffix=".json")
        if ledger_name not in ledgers:
            raise AuditError(f"source has no embedded owning ledger: {logical}")
        ledger_path = (
            generator_root / "python_sound_generator" / ledger_relative
        )
        if not ledger_path.is_file():
            raise AuditError(f"owning source ledger is missing: {ledger_name}")
        current = _object(ledger_path)
        if current != ledgers[ledger_name]:
            raise AuditError(f"embedded source ledger drifted: {ledger_name}")
        # The owning ledger must actually enumerate the exact input, not just
        # assert a collection-level licence.
        if Path(logical).name not in json.dumps(current, sort_keys=True):
            raise AuditError(
                f"owning ledger does not enumerate source: {logical}"
            )

    collections = provenance.get("collections")
    if not isinstance(collections, list):
        raise AuditError("source ledger collections must be an array")
    allowed = {"CC0", "CC0 1.0", "Public Domain"}
    for collection in collections:
        if not isinstance(collection, dict):
            raise AuditError("source collection is not an object")
        if collection.get("license") not in allowed:
            raise AuditError(
                f"unapproved source licence: {collection.get('license')!r}"
            )
        source_page = collection.get("source_page")
        owner = collection.get("owner_provenance")
        if not (
            isinstance(source_page, str) and source_page.startswith("https://")
        ) and not (
            isinstance(owner, str)
            and _safe_relative(owner, suffix=".json").name == "provenance.json"
        ):
            raise AuditError("source collection has no HTTPS page or owner")
    if provenance.get("missing") or provenance.get("invalid"):
        raise AuditError("source ledger reports missing or invalid inputs")

    counts = manifest.get("counts")
    if not isinstance(counts, dict):
        raise AuditError("manifest counts are missing")
    if counts.get("recording_inputs") != len(artifact_sources):
        raise AuditError("manifest recording-input count does not reconcile")


def _verify_cues(
    manifest: dict[str, Any],
    artifacts: list[dict[str, Any]],
    stable: dict[str, Any],
) -> None:
    cues = manifest.get("cues")
    if not isinstance(cues, dict):
        raise AuditError("manifest cues must be an object")
    frozen_cues = stable.get("cues")
    frozen_scenes = stable.get("scenes")
    if not isinstance(frozen_cues, dict) or not isinstance(frozen_scenes, dict):
        raise AuditError("stable_ids.json has no cue/scene maps")

    sfx_names = {name for name in cues if not name.startswith("music.")}
    if sfx_names != set(frozen_cues):
        raise AuditError("manifest does not exactly cover frozen SFX cue ids")
    music_names = {
        name.removeprefix("music.")
        for name in cues
        if name.startswith("music.")
    }
    if music_names != set(frozen_scenes):
        raise AuditError("manifest does not exactly cover frozen music scenes")
    for name, stable_id in frozen_cues.items():
        row = cues.get(name)
        if not isinstance(row, dict) or row.get("stable_id") != stable_id:
            raise AuditError(f"stable cue id mismatch: {name}")
    for scene, scene_id in frozen_scenes.items():
        row = cues.get(f"music.{scene}")
        if not isinstance(row, dict) or row.get("scene_id") != scene_id:
            raise AuditError(f"stable scene id mismatch: {scene}")

    files_by_cue: dict[str, list[str]] = defaultdict(list)
    variations_by_cue: dict[str, list[int]] = defaultdict(list)
    for artifact in artifacts:
        cue_id = artifact.get("cue_id")
        filename = artifact.get("file")
        variation = artifact.get("variation")
        if not isinstance(cue_id, str) or cue_id not in cues:
            raise AuditError(f"artifact has unknown cue id: {cue_id!r}")
        if not isinstance(filename, str):
            raise AuditError(f"artifact has invalid file: {filename!r}")
        if not isinstance(variation, int) or variation < 1:
            raise AuditError(f"artifact has invalid variation: {filename}")
        files_by_cue[cue_id].append(filename)
        variations_by_cue[cue_id].append(variation)

    for cue_id, row in cues.items():
        if not isinstance(row, dict):
            raise AuditError(f"cue index row is not an object: {cue_id}")
        files = row.get("files")
        if not isinstance(files, list) or any(
            not isinstance(item, str) for item in files
        ):
            raise AuditError(f"cue has invalid file list: {cue_id}")
        if files != files_by_cue.get(cue_id, []):
            raise AuditError(f"cue file list does not reconcile: {cue_id}")
        variations = row.get("variations")
        if variations != len(files):
            raise AuditError(f"cue variation count does not reconcile: {cue_id}")
        if sorted(variations_by_cue.get(cue_id, [])) != list(
            range(1, len(files) + 1)
        ):
            raise AuditError(f"cue variations are not contiguous: {cue_id}")

    # Explicitly enforce the task's fatigue rule for known >1 Hz events.
    high_rate = {
        "ui.cursor",
        "ui.invalid",
        "build.mode",
        "fire.rapid",
        "fire.antiair",
        "impact.impact",
        "impact.pierce",
        "impact.splash",
        "impact.deflect",
        "impact.shield_break",
        "impact.hardened",
        "unit.spawn",
        "unit.death",
        "unit.split",
        "unit.emit",
        "unit.suppressor_halt",
        "fixture.hit",
        "fixture.critical",
        "fixture.destroyed",
        "fixture.repaired",
        "state.integrity_lost",
    }
    for cue_id in high_rate:
        if cues[cue_id].get("variations") != 3:
            raise AuditError(f"high-rate cue needs three variations: {cue_id}")


def _run_shared_audit(
    manifest_path: Path,
    provenance_path: Path,
    tools_root: Path,
) -> str:
    package = tools_root / "src" / "kilix_game_tools"
    if not (package / "__main__.py").is_file():
        raise AuditError(f"kilix-game-tools checkout is missing: {tools_root}")
    environment = os.environ.copy()
    existing = environment.get("PYTHONPATH")
    environment["PYTHONPATH"] = (
        str(tools_root / "src")
        if not existing
        else f"{tools_root / 'src'}{os.pathsep}{existing}"
    )
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "kilix_game_tools",
            "validate-audio",
            str(manifest_path),
            str(provenance_path),
        ],
        cwd=GAME_ROOT,
        env=environment,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise AuditError(f"kilix_game_tools validate-audio failed: {detail}")
    return result.stdout.strip()


def verify(
    audio_root: Path,
    generator_root: Path,
    tools_root: Path,
) -> None:
    audio_root = audio_root.expanduser().resolve()
    manifest_path = audio_root / "manifest.json"
    manifest = _object(manifest_path)
    if manifest.get("schema_version") != 1:
        raise AuditError("unsupported audio manifest schema")
    if manifest.get("game") != "pleb-tower":
        raise AuditError("audio manifest belongs to a different game")
    if manifest.get("source_licensing") != "CC0 1.0":
        raise AuditError("recording-source policy is not CC0-only")
    _verify_format(manifest)

    provenance_name = _safe_relative(
        manifest.get("provenance"), suffix=".json"
    )
    provenance_path = audio_root / provenance_name
    provenance = _object(provenance_path)
    if provenance.get("schema_version") != 1:
        raise AuditError("unsupported audio source-ledger schema")
    if provenance.get("game") != "pleb-tower":
        raise AuditError("audio provenance belongs to a different game")

    stable_index = manifest.get("stable_ids")
    if not isinstance(stable_index, dict):
        raise AuditError("manifest has no stable-id index")
    stable_path = GAME_ROOT / _safe_relative(stable_index.get("file"))
    if _sha256(stable_path) != stable_index.get("sha256"):
        raise AuditError("frozen stable-id file checksum mismatch")
    stable = _object(stable_path)

    score_index = manifest.get("score_source")
    if not isinstance(score_index, dict):
        raise AuditError("manifest has no retained score index")
    score_path = GAME_ROOT / _safe_relative(score_index.get("file"))
    score_hash = _sha256(score_path)
    if score_hash != score_index.get("sha256"):
        raise AuditError("retained score checksum mismatch")
    score_provenance = provenance.get("score")
    if (
        not isinstance(score_provenance, dict)
        or score_provenance.get("sha256") != score_hash
    ):
        raise AuditError("retained score does not reconcile with provenance")

    artifact_value = manifest.get("artifacts")
    if not isinstance(artifact_value, list) or any(
        not isinstance(item, dict) for item in artifact_value
    ):
        raise AuditError("manifest artifacts must be an array of objects")
    artifacts: list[dict[str, Any]] = artifact_value
    if not artifacts:
        raise AuditError("audio manifest has no artifacts")

    seen: set[Path] = set()
    artifact_sources: set[str] = set()
    total_frames = 0
    loop_count = 0
    for artifact in artifacts:
        relative = _safe_relative(artifact.get("file"), suffix=".wav")
        if relative in seen:
            raise AuditError(f"duplicate artifact path: {relative}")
        seen.add(relative)
        path = audio_root / relative
        if not path.is_file():
            raise AuditError(f"manifest WAV is missing: {relative}")
        if _sha256(path) != artifact.get("sha256"):
            raise AuditError(f"WAV checksum mismatch: {relative}")

        samples = _read_pcm(path)
        measured = _metrics(samples)
        total_frames += int(measured["frames"])
        _close(
            float(measured["duration_seconds"]),
            artifact.get("duration_seconds"),
            1.0e-6,
            f"{relative} duration",
        )
        if measured["frames"] != artifact.get("frames"):
            raise AuditError(f"{relative}: frame count mismatch")
        _close(
            float(measured["peak"]),
            artifact.get("peak"),
            1.0e-8,
            f"{relative} peak",
        )
        _close(
            float(measured["rms"]),
            artifact.get("rms"),
            1.0e-8,
            f"{relative} RMS",
        )
        if float(measured["peak"]) >= MAX_PEAK:
            raise AuditError(f"{relative}: clipping or no mastering headroom")
        if float(measured["rms"]) < MIN_RMS:
            raise AuditError(f"{relative}: inaudible RMS")

        sources = artifact.get("sources")
        if not isinstance(sources, list) or any(
            not isinstance(source, str) for source in sources
        ):
            raise AuditError(f"{relative}: invalid recording-source list")
        if artifact.get("recording_sources") != sources:
            raise AuditError(f"{relative}: source aliases do not reconcile")
        artifact_sources.update(sources)

        looped = artifact.get("loop")
        if not isinstance(looped, bool):
            raise AuditError(f"{relative}: loop flag is not boolean")
        if looped:
            loop_count += 1
            recorded = artifact.get("seam")
            if not isinstance(recorded, dict):
                raise AuditError(f"{relative}: loop has no seam metrics")
            measured_seam = _seam(samples)
            for key in (
                "step",
                "largest_step",
                "step_ratio",
                "head_rms",
                "tail_rms",
                "level_ratio",
            ):
                _close(
                    float(measured_seam[key]),
                    recorded.get(key),
                    1.0e-8,
                    f"{relative} seam {key}",
                )
            if measured_seam["seamless"] is not True:
                raise AuditError(f"{relative}: measured loop seam failed")
            if recorded.get("seamless") is not True:
                raise AuditError(f"{relative}: manifest marks seam failed")
        elif "seam" in artifact:
            raise AuditError(f"{relative}: non-loop unexpectedly has seam metrics")

    inventory = {
        path.relative_to(audio_root)
        for path in audio_root.rglob("*.wav")
        if path.is_file()
    }
    if inventory != seen:
        unlisted = sorted(path.as_posix() for path in inventory - seen)
        missing = sorted(path.as_posix() for path in seen - inventory)
        raise AuditError(
            f"WAV inventory mismatch: unlisted={unlisted} missing={missing}"
        )

    _verify_cues(manifest, artifacts, stable)
    _verify_sources(
        manifest,
        provenance,
        artifact_sources,
        generator_root.expanduser().resolve(),
    )

    counts = manifest.get("counts")
    cues = manifest.get("cues")
    if not isinstance(counts, dict) or not isinstance(cues, dict):
        raise AuditError("manifest cue/count indexes are malformed")
    expected_counts = {
        "logical_cues": len(cues),
        "sfx_cues": len(stable["cues"]),
        "music_scenes": len(stable["scenes"]),
        "wav_files": len(artifacts),
        "seamless_loops": loop_count,
        "recording_inputs": len(artifact_sources),
    }
    for name, expected in expected_counts.items():
        if counts.get(name) != expected:
            raise AuditError(
                f"manifest count mismatch for {name}: "
                f"{counts.get(name)!r} != {expected}"
            )
    actual_seconds = total_frames / SAMPLE_RATE
    _close(
        actual_seconds,
        counts.get("total_duration_seconds"),
        0.001,
        "bank total duration",
    )

    shared_report = _run_shared_audit(
        manifest_path,
        provenance_path,
        tools_root.expanduser().resolve(),
    )
    print(shared_report)
    print(
        f"PASS audio-bank cues={len(cues)} wavs={len(artifacts)} "
        f"loops={loop_count} sources={len(artifact_sources)} "
        f"seconds={actual_seconds:.3f} "
        f"mib={sum((audio_root / path).stat().st_size for path in seen) / 1048576:.2f}"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--audio-root",
        type=Path,
        default=GAME_ROOT / "assets/audio",
        help="generated production audio root",
    )
    parser.add_argument(
        "--generator-root",
        type=Path,
        default=(
            GAME_ROOT.parents[1]
            / "kilix-apps"
            / "python-sound-generator"
        ),
        help="python_sound_generator checkout used to audit exact source bytes",
    )
    parser.add_argument(
        "--tools-root",
        type=Path,
        default=GAME_ROOT / "third_party/kilix-game-tools",
        help="kilix-game-tools checkout",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    try:
        verify(
            arguments.audio_root,
            arguments.generator_root,
            arguments.tools_root,
        )
    except (
        AuditError,
        FileNotFoundError,
        json.JSONDecodeError,
        OSError,
    ) as error:
        print(f"verify_audio.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
