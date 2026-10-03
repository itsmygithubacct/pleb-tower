#!/usr/bin/env python3
"""Check authored strategies against the combat code in the shipping binary.

Reviewed winning orders live in content/build_orders.json. Each order is a
spending priority: unaffordable placements, upgrades and repairs are skipped, and destroyed
fixtures can be rebuilt by later entries. Waves are called early, as in the
player-flow tests. Deliberately incomplete strategies must still lose.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

from balance_sim import HOLDOUT_ORDERS

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    content = json.loads((ROOT / "content/campaigns.json").read_text())
    orders = json.loads((ROOT / "content/build_orders.json").read_text())
    failures = []
    for map_index, level in enumerate(content["maps"]):
        for campaign_index, campaign in enumerate(content["campaigns"]):
            priorities = orders["maps"][level["id"]][campaign["id"]]["optimal"]
            table = {"optimal": [(p["wave"], p["pad"], p["fixture"], 255 if p.get("action") == "repair" else p["tier"]) for p in priorities]}
            if campaign_index == 0 and map_index == 0:
                table.update({k: v for k, v in HOLDOUT_ORDERS.items() if k != "optimal"})
            kinds = {f["id"]: i for i, f in enumerate(campaign["fixtures"])}
            for name, priority in table.items():
                with tempfile.NamedTemporaryFile(mode="w", suffix=".order") as file:
                    for wave, pad, kind, tier in priority:
                        file.write(f"{wave} {pad} {kinds[kind]} {tier}\n")
                    file.flush()
                    result = subprocess.run([str(ROOT / "pleb-tower"), "--map", level["id"], "--simulate",
                                             str(campaign_index), file.name],
                                            capture_output=True, text=True, timeout=30)
                lines = result.stdout.splitlines()
                summary = next((line for line in lines if line.startswith("PT_SIM_RESULT")), None)
                if summary is None or result.returncode not in (0, 1):
                    failures.append(f"{campaign['id']}/{name}: runtime simulation failed {result.stderr}")
                    continue
                data = dict(re.findall(r"(\w+)=([^ ]+)", summary))
                print(f"{level['name']:<11} {campaign['name']:<7} {name:<15} {data['status']:<7} "
                      f"integrity {data['integrity']:<5} leaks {data['leak_count']:<3} "
                      f"lost {data['fixtures_lost']:<2} {float(data['seconds'])/60:.1f} min")
                if name == "optimal" and result.returncode != 0:
                    failures.append(f"{campaign['id']}: reviewed order must clear")
                if name in ("naive-rapid", "no-antiair", "no-support", "workshop-farm") and result.returncode != 1:
                    failures.append(f"{name}: missing role should have a consequence")
                if data["status"] not in ("CLEARED", "LOST") or any(
                        data[field] != "0" for field in ("unit_overflow", "projectile_overflow")):
                    failures.append(f"{campaign['id']}/{name}: stalled or overflowing")
                if args.verbose:
                    print(result.stdout)
    for failure in failures:
        print(f"FAIL {failure}")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
