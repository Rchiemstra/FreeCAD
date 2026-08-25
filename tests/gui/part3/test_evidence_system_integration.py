"""Parent FreeCAD boundary for the tracked MCP evidence bootstrap.

This test is deliberately offline: it starts only the approved Python 3.11
interpreter against a disposable package signed by a non-authoritative key.
"""
from __future__ import annotations

import base64
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import os
import sys
import importlib
import importlib.util

PYTHON = Path(sys.executable).resolve()
REPOSITORY = Path(__file__).resolve().parents[3]
BOOTSTRAP = REPOSITORY / "tools/mcp/freecad-mcp/src/freecad_mcp/evidence_system/trusted_bootstrap.py"
EVIDENCE_SOURCE = REPOSITORY / "tools/mcp/freecad-mcp/src/freecad_mcp/evidence_system"


def _tracked_run_isolated():
    """Load the tracked MCP package by its derived file location, not sys.path."""
    package = EVIDENCE_SOURCE.parent
    if "freecad_mcp" not in sys.modules:
        spec = importlib.util.spec_from_file_location("freecad_mcp", package / "__init__.py", submodule_search_locations=[str(package)])
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec); sys.modules["freecad_mcp"] = module; spec.loader.exec_module(module)
    return importlib.import_module("freecad_mcp.evidence_system.launcher").run_isolated

_Q = 2**255 - 19
_D = (-121665 * pow(121666, _Q - 2, _Q)) % _Q
_L = 2**252 + 27742317777372353535851937790883648493
_B = (15112221349535400772501151409588531511454012693041857206046113283949847762202, 46316835694926478169428394003475163141307993866256225615783033603165251855960)


def _add(first, second):
    x, y = first; u, v = second
    return ((x*v+y*u)*pow(1+_D*x*u*y*v, _Q-2, _Q)%_Q, (y*v+x*u)*pow(1-_D*x*u*y*v, _Q-2, _Q)%_Q)


def _multiply(point, scalar):
    result = (0, 1)
    while scalar:
        if scalar & 1: result = _add(result, point)
        point = _add(point, point); scalar >>= 1
    return result


def _encode(point):
    x, y = point
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _test_sign(message: bytes):
    digest = hashlib.sha512(bytes(range(32))).digest()
    scalar = (int.from_bytes(digest[:32], "little") & ((1 << 254) - 8)) | (1 << 254)
    public = _encode(_multiply(_B, scalar))
    nonce = int.from_bytes(hashlib.sha512(digest[32:] + message).digest(), "little") % _L
    encoded = _encode(_multiply(_B, nonce))
    challenge = int.from_bytes(hashlib.sha512(encoded + public + message).digest(), "little") % _L
    return public, encoded + ((nonce + challenge * scalar) % _L).to_bytes(32, "little")


def test_parent_invokes_tracked_mcp_bootstrap_without_ignored_results(tmp_path):
    package = tmp_path / "diagnostic"; package.mkdir()
    runner = b"from freecad_mcp.evidence_system.runner import bootstrap_entrypoint\ndef main(request):\n    return bootstrap_entrypoint(request)\n"
    public, _ = _test_sign(b"")
    reviewer = hashlib.sha256(public).hexdigest()
    mounts = [
        {"Type": "bind", "Source": str(package), "Destination": "/diagnostic", "RW": False},
        {"Type": "bind", "Source": str(BOOTSTRAP), "Destination": "/trusted/bootstrap.py", "RW": False},
        {"Type": "bind", "Source": str(REPOSITORY), "Destination": "/repo", "RW": False},
        {"Type": "bind", "Source": str(REPOSITORY / "tools/mcp/freecad-mcp"), "Destination": "/build", "RW": False},
        {"Type": "bind", "Source": str(tmp_path / "out"), "Destination": "/out", "RW": True},
    ]
    docker_launch = [str(PYTHON), "run", "--network", "none", "--read-only", "--tmpfs", "/tmp:rw,nosuid,nodev,size=2g"] + [item for row in mounts for item in ("--mount", f"type=bind,src={row['Source']},dst={row['Destination']}" + ("" if row["RW"] else ",readonly"))] + ["sha256:" + "e" * 64]
    executor_command = [str(PYTHON), "-I", "-S", "-B", str(tmp_path / "offline_executor.py")]
    command_contract = {
        "outer": [str(PYTHON), "-I", "-S", "-B", str(BOOTSTRAP), str(package), "--reviewer-sha256", reviewer, "--interpreter-sha256", hashlib.sha256(PYTHON.read_bytes()).hexdigest(), "--run-id", "P3-WP27", "--attempt-id", "parent-integration-test", "--sequence", "44", "--scope", "tracked-evidence-scope/44", "--", "--integration"],
        "executor": executor_command,
        "docker": docker_launch,
    }
    policy = {
        "run_id": "P3-WP27", "attempt_id": "parent-integration-test", "sequence": 44,
        "reviewer_key": reviewer, "scope": "tracked-evidence-scope/44", "interpreter": str(PYTHON),
        "outer_argv": command_contract["outer"], "executor_argv": command_contract["executor"],
        "docker_argv": docker_launch, "environment": {}, "container_environment": {}, "mounts": mounts,
        "sources": {}, "binaries": {"host_interpreter": hashlib.sha256(PYTHON.read_bytes()).hexdigest()}, "container_entrypoint": ["/usr/bin/python3", "-I", "-S", "-B", "/trusted/bootstrap.py"], "container_cmd": [],
    }
    inspect = {
        "Config": {"Image": "sha256:" + "e" * 64, "Entrypoint": policy["container_entrypoint"], "Cmd": [], "Env": []},
        "HostConfig": {"NetworkMode": "none", "ReadonlyRootfs": True, "Tmpfs": {"/tmp": "rw,nosuid,nodev,size=2g"}},
        "Mounts": mounts,
    }
    offline_executor = tmp_path / "offline_executor.py"
    offline_executor.write_text(
        "from datetime import datetime, timezone\nimport hashlib,json,sys\nfrom pathlib import Path\nwith Path(__file__).with_suffix('.marker').open('a',encoding='utf-8') as marker: marker.write(sys.argv[1]+'\\n')\n"
        "request=json.loads(open(sys.argv[3],encoding='utf-8').read())\nbinding=request['binding']; policy=request['policy']\n"
        "if sys.argv[1]=='--cleanup-request': print(json.dumps({'passed':True,'errors':[]})); raise SystemExit(0)\n"
        "if sys.argv[1]=='--preflight-request':\n"
        " values={'package':binding['package_manifest'],'authorization':{'authorization_sha256':binding['authorization_sha256'],'signature_sha256':binding['signature_sha256']},'configured_candidate':binding['configured_candidate'],'raw_candidate':binding['raw_candidate'],'repository':binding['repository'],'sources':policy['sources'],'binaries':policy['binaries'],'image':binding['image'],'output_freshness':True,'conflicting_processes':[],'port':{'available':True},'cache':{'clean':True},'resolved_outer_command':policy['outer_argv'],'resolved_executor_command':policy['executor_argv'],'resolved_docker_command':policy['docker_argv'],'environment':policy['environment'],'mounts':policy['mounts'],'timestamp_freshness':True}\n"
        " checks=[{'id':name,'status':'PASS','binding':binding,'value':values[name]} for name in ('package','authorization','configured_candidate','raw_candidate','repository','sources','binaries','image','output_freshness','conflicting_processes','port','cache','resolved_outer_command','resolved_executor_command','resolved_docker_command','environment','mounts','timestamp_freshness')]\n"
        " print(json.dumps({'schema_version':44,'binding':binding,'observed_utc':datetime.now(timezone.utc).isoformat(),'passed':True,'checks':checks,'commands':{'outer':policy['outer_argv'],'executor':policy['executor_argv'],'docker':policy['docker_argv']},'output_fresh':True},sort_keys=True)); raise SystemExit(0)\n"
        "output=__import__('pathlib').Path(request['output']); ident='b'*64; inspect={'Id':ident,'Config':{'Image':binding['image'],'Entrypoint':['/usr/bin/python3','-I','-S','-B','/trusted/bootstrap.py'],'Cmd':[],'Env':[]},'HostConfig':{'NetworkMode':'none','ReadonlyRootfs':True,'Tmpfs':{'/tmp':'rw,nosuid,nodev,size=2g'}},'Mounts':policy['mounts']}; raw=json.dumps(inspect,sort_keys=True,separators=(',',':')); container={'execution':binding,'container_id':ident,'raw_inspect_sha256':hashlib.sha256(raw.encode()).hexdigest()}\n"
        "for name,phase in (('gdb-resolution.json','gdb_resolution'),('localization-result.json','localization'),('child-terminal-result.json','terminal')):\n with (output/name).open('x',encoding='utf-8') as record: record.write(json.dumps({'schema_version':44,'binding':container,'phase':phase,'status':'SUCCEEDED'},sort_keys=True))\n"
        "inspect['_raw_bytes']=raw; print(json.dumps({'execution':{'status':'SUCCEEDED','docker':{'launch':policy['docker_argv'],'inspect':inspect,'kernel_tmpfs':'rw,nosuid,nodev,size=2g'}},'parent_exit':0,'container_id':ident,'raw_inspect_sha256':container['raw_inspect_sha256']},sort_keys=True))\n",
        encoding="utf-8",
    )
    runtime = {"policy": policy, "executor_command": executor_command, "executor_sha256": hashlib.sha256(offline_executor.read_bytes()).hexdigest()}
    config = json.dumps({"runner": "runner.py", "command_contract": command_contract, "runtime": runtime}, sort_keys=True, separators=(",", ":")).encode()
    governed = {"runner.py": runner, "evidence-config.json": config, "freecad_mcp/__init__.py": b""}
    for source in EVIDENCE_SOURCE.glob("*.py"):
        governed[f"freecad_mcp/evidence_system/{source.name}"] = source.read_bytes()
    for name, payload in governed.items():
        destination = package / name; destination.parent.mkdir(parents=True, exist_ok=True); destination.write_bytes(payload)
    manifest_value = {
        "schema_version": 1,
        "files": {name: hashlib.sha256(payload).hexdigest() for name, payload in governed.items()},
        "directories": ["freecad_mcp", "freecad_mcp/evidence_system"],
    }
    manifest = json.dumps(manifest_value, sort_keys=True, separators=(",", ":")).encode()
    public, manifest_signature = _test_sign(manifest)
    now = datetime.now(timezone.utc)
    command_hash = hashlib.sha256(json.dumps(command_contract, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    authorization_value = {
        "schema_version": 2, "status": "AUTHORIZED", "run_id": "P3-WP27", "attempt_id": "parent-integration-test", "sequence": 44,
        "nonce": "a" * 64, "output_root": str(tmp_path / "out"), "configured_candidate": "b" * 64, "raw_candidate": "c" * 64,
        "repository": "d" * 64, "image": "sha256:" + "e" * 64, "package_manifest": hashlib.sha256(manifest).hexdigest(),
        "trusted_bootstrap": hashlib.sha256(BOOTSTRAP.read_bytes()).hexdigest(), "commands": command_hash,
        "scope": "tracked-evidence-scope/44", "reviewer_key": hashlib.sha256(public).hexdigest(),
        "not_before_utc": (now - timedelta(seconds=1)).isoformat(), "issued_utc": now.isoformat(), "expires_utc": (now + timedelta(minutes=5)).isoformat(),
    }
    authorization = json.dumps(authorization_value, sort_keys=True, separators=(",", ":")).encode()
    _, authorization_signature = _test_sign(authorization)
    wire = b"\0\0\0\vssh-ed25519\0\0\0 " + public
    files = {
        "package-manifest.json": manifest,
        "package-manifest.sig": base64.b64encode(manifest_signature),
        "review-authorization.json": authorization,
        "review-authorization.sig": base64.b64encode(authorization_signature),
        "reviewer.pub": b"ssh-ed25519 " + base64.b64encode(wire),
    }
    for name, payload in files.items():
        (package / name).write_bytes(payload)
    environment = {key: os.environ[key] for key in ("SystemRoot", "WINDIR", "ComSpec", "TEMP", "TMP") if os.environ.get(key)}
    completed = _tracked_run_isolated()(BOOTSTRAP, package, ["--integration"], {**environment, "TEMP": str(tmp_path), "TMP": str(tmp_path)}, hashlib.sha256(BOOTSTRAP.read_bytes()).hexdigest(), hashlib.sha256(PYTHON.read_bytes()).hexdigest(), hashlib.sha256(public).hexdigest(), "P3-WP27", "parent-integration-test", 44, "tracked-evidence-scope/44", timeout=30)
    assert completed.returncode == 0, completed.stderr
    assert json.loads(completed.stdout) == {"issue": None, "passed": True}
    assert offline_executor.with_suffix(".marker").read_text(encoding="utf-8").splitlines() == ["--preflight-request", "--evidence-request", "--cleanup-request"]
    verdict = json.loads((tmp_path / "out" / "final-verdict.json").read_text())
    assert verdict["result"] == "PASS"
    assert {entry["path"] for entry in json.loads((tmp_path / "out" / "artifact-ledger.json").read_text())["entries"].values()} == {"gdb-resolution.json", "localization-result.json", "child-terminal-result.json", "outer-execution.json"}
