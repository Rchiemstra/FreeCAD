# SPDX-License-Identifier: LGPL-2.1-or-later
import json
import os
import sys

_UINT64_MAX = 18446744073709551615


def _emit_control(obj):
  payload = json.dumps(obj, separators=(",", ":"))
  print(f"FCGEO/1 {payload}", flush=True)


def _parse_launched_job_id_wire():
  """Return canonical launched jobId wire or None when unset (manual invocation)."""
  raw = os.environ.get("FCGEO_LAUNCHED_JOB_ID")
  if raw is None or raw == "":
    return None
  if not raw.isdigit():
    raise ValueError("FCGEO_LAUNCHED_JOB_ID must be a canonical decimal string")
  if raw == "0" or (len(raw) > 1 and raw[0] == "0"):
    raise ValueError("FCGEO_LAUNCHED_JOB_ID must be a canonical nonzero decimal string")
  if len(raw) > 20 or (len(raw) == 20 and raw > str(_UINT64_MAX)):
    raise ValueError("FCGEO_LAUNCHED_JOB_ID is out of uint64 range")
  value = int(raw, 10)
  if value == 0 or value > _UINT64_MAX:
    raise ValueError("FCGEO_LAUNCHED_JOB_ID must be a canonical nonzero uint64 decimal string")
  return raw


def _emit_python_error(code, message, launched_job_id_wire=None):
  _emit_control(
    {
      "type": "hello",
      "version": "1.0",
      "protocol": "FCGEO/1",
    }
  )
  err = {
    "type": "error",
    "code": code,
    "message": message,
  }
  if launched_job_id_wire is not None:
    err["jobId"] = launched_job_id_wire
  _emit_control(err)


def resolve_request_path(argv):
  """Locate the request.json path FreeCADCmd passed through to this script.

  Preferred form:
    FreeCADCmd --safe-mode GeometryWorker.py --pass /path/to/request.json

  Without --pass, FreeCAD treats trailing paths as documents to open, which
  breaks the worker protocol. Fall back to the last *.json argument only.

  Note: FreeCAD's Interpreter.runFile() executes scripts with __name__ set to
  the basename (not "__main__"), so this module must invoke main() at import.
  """
  if "--pass" in argv:
    idx = argv.index("--pass")
    if idx + 1 < len(argv):
      candidate = argv[idx + 1]
      if candidate.endswith(".json"):
        return candidate
  for arg in reversed(argv):
    if arg.endswith(".json"):
      return arg
  return None


def main():
  launched_job_id_wire = None
  try:
    launched_job_id_wire = _parse_launched_job_id_wire()
  except ValueError as exc:
    _emit_python_error("invalid_launched_job_id", str(exc))
    sys.exit(1)

  request_path = resolve_request_path(sys.argv)
  if not request_path:
    _emit_python_error(
      "missing_arg",
      "Usage: GeometryWorker.py --pass <request.json>",
      launched_job_id_wire,
    )
    sys.exit(1)

  import Part

  if hasattr(Part, "_runGeometryWorker"):
    res = Part._runGeometryWorker(request_path)
    sys.exit(res)

  _emit_python_error(
    "worker_binding_missing",
    "Part._runGeometryWorker is unavailable",
    launched_job_id_wire,
  )
  sys.exit(2)


# FreeCAD runFile() does not set __name__ to "__main__".
main()
