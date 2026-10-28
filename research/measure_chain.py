"""Run one capped FreeCAD process at a time and retain raw RSS/time evidence."""

import argparse
import json
import os
from pathlib import Path
import resource
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def limit():
    resource.setrlimit(resource.RLIMIT_AS, (6000000 * 1024,) * 2)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(cfg, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(
        CHAIN_CONFIG=json.dumps(cfg),
        CHAIN_RESULT=str(output.resolve()),
        OMP_NUM_THREADS="1",
        OPENBLAS_NUM_THREADS="1",
        LP_NUM_THREADS="1",
        QTWEBENGINE_DISABLE_SANDBOX="1",
    )
    env["QT_QPA_PLATFORM"] = "offscreen" if cfg["mode"] == "offscreen" else "xcb"
    executable = "FreeCADCmd" if cfg["mode"] == "cmd" else "FreeCAD"
    macro = ROOT / "research/memory_chain.py"
    # GUI command-line macros use the FCMacro extension.
    gui_macro = ROOT / "research/memory_chain.FCMacro"
    if executable == "FreeCAD":
        gui_macro.write_text(macro.read_text())
        macro = gui_macro
    command = [
        str(ROOT / "build/relWithDebInfo/bin" / executable),
        "-u",
        str(output.with_suffix(".cfg").resolve()),
        str(macro),
    ]
    start = time.monotonic()
    peak = 0
    reason = None
    with output.with_suffix(".log").open("w") as log:
        process = subprocess.Popen(
            command,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            preexec_fn=limit,
            stdin=subprocess.DEVNULL,
        )
        while process.poll() is None:
            try:
                status = Path(f"/proc/{process.pid}/status").read_text()
                values = dict(line.split(":", 1) for line in status.splitlines())
                peak = max(peak, int(values.get("VmHWM", "0").split()[0]))
            except (FileNotFoundError, ProcessLookupError):
                pass
            if peak > 4600000 or time.monotonic() - start > 180:
                reason = "rss_cap" if peak > 4600000 else "timeout"
                process.kill()
            time.sleep(0.02)
        process.wait()
    summary = dict(
        config=cfg,
        wall_seconds=time.monotonic() - start,
        peak_kib=peak,
        exit=process.returncode,
        stopped=reason,
    )
    with output.open("a") as stream:
        stream.write(json.dumps(dict(stage="process", **summary)) + "\n")
    print(json.dumps(summary), flush=True)
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="{}")
    parser.add_argument("--label", default="baseline")
    parser.add_argument("--ns", default="5,10,20,30,40")
    args = parser.parse_args()
    config = dict(mode="cmd", feature="box", refine=True, spacing="overlap", each=False)
    config.update(json.loads(args.config))
    for n in map(int, args.ns.split(",")):
        cfg = dict(config, n=n)
        path = ROOT / "research/results" / f"{args.label}-{n}.jsonl"
        if path.exists():
            raise RuntimeError(f"Refusing to overwrite {path}")
        result = run(cfg, path)
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        if result["stopped"] or not any(row["stage"] == "recomputed" for row in rows):
            break
        if any(
            "Invalid" in state
            for row in rows
            if row["stage"] == "recomputed"
            for state in row["states"]
        ):
            print(
                "Stopping series after invalid features (including allocation failure).", flush=True
            )
            break
