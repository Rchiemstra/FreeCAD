"""Compare one factor at a time in all three execution modes."""

import json
import sys
from measure_chain import ROOT, run

prefix = sys.argv[1]
variants = {
    "base": {},
    "no-refine": {"refine": False},
    "touch": {"spacing": "touch"},
    "separate": {"spacing": "separate"},
    "pad": {"feature": "pad"},
    "each": {"each": True},
}
for mode in ("cmd", "offscreen", "wslg"):
    for label, variant in variants.items():
        for n in (5, 10, 20, 30, 40):
            config = dict(mode=mode, n=n, feature="box", refine=True, spacing="overlap", each=False)
            config.update(variant)
            path = ROOT / "research/results" / f"{prefix}-{mode}-{label}-{n}.jsonl"
            if not path.exists():
                run(config, path)
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            done = next((row for row in rows if row["stage"] == "recomputed"), None)
            if done is None or any("Invalid" in state for state in done["states"]):
                print("Stopping series on allocation/recompute failure", flush=True)
                break
