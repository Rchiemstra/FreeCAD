"""Collect the committed experiment table from the launcher's raw JSONL files."""

import csv
import json
from pathlib import Path

root = Path(__file__).resolve().parent
columns = [
    "phase",
    "mode",
    "feature",
    "refine",
    "spacing",
    "each",
    "undo",
    "hasher",
    "drop_maps",
    "n",
    "peak_mib",
    "wall_seconds",
    "recompute_seconds",
    "heap_mib",
    "valid_features",
    "outcome",
]
with (root / "memory_measurements.csv").open("w") as stream:
    writer = csv.DictWriter(stream, fieldnames=columns)
    writer.writeheader()
    for path in sorted((root / "results").glob("*.jsonl")):
        if not path.stem.startswith(
            ("before-", "after-", "diagnostic-", "cmd-no-undo", "cmd-no-hasher")
        ):
            continue
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        process = next((row for row in rows if row["stage"] == "process"), None)
        if process is None:
            continue
        config = process["config"]
        done = next((row for row in rows if row["stage"] == "recomputed"), None)
        created = next((row for row in rows if row["stage"] == "created"), None)
        error = next((row for row in rows if row["stage"] == "error"), None)
        outcome = process["stopped"] or ("error" if error else "complete" if done else "failed")
        if done and any("Invalid" in state for state in done["states"]):
            outcome = "allocation/recompute failure"
        if path.stem.startswith(("cmd-no-", "diagnostic-before-")):
            phase = "diagnostic-before"
        elif path.stem.startswith("diagnostic-after-"):
            phase = "diagnostic-after"
        else:
            phase = path.stem.split("-", 1)[0]
        writer.writerow(
            dict(
                phase=phase,
                **config,
                peak_mib=round(process["peak_kib"] / 1024, 2),
                wall_seconds=round(process["wall_seconds"], 3),
                recompute_seconds=(
                    round(done["elapsed"] - created["elapsed"], 3) if done and created else ""
                ),
                heap_mib=round(done["heap_bytes"] / 2**20, 2) if done else "",
                valid_features=(
                    sum("Invalid" not in state for state in done["states"]) if done else ""
                ),
                outcome=outcome
            )
        )
