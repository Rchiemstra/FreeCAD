# SPDX-License-Identifier: LGPL-2.1-or-later
"""CI image input keys and isolated registry contract.

pytest -q tests/architecture/test_ci_image_identity.py
FREECAD_CI_REGISTRY_TESTS=1 pytest -q tests/architecture/test_ci_image_identity.py
The opt-in suite starts exactly one disposable, memory-limited registry. It
uploads tiny scratch manifests via the registry API; no kaniko/apt builds.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
import yaml
from urllib.parse import urljoin
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
INPUTS = (
    "package/ubuntu/Dockerfile",
    "package/ubuntu/install-apt-packages.sh",
    "package/ubuntu/neon-archive-keyring.asc",
)
MCP_INPUTS = ("tools/mcp/freecad-mcp/Dockerfile.ci",
              "tools/mcp/freecad-mcp/pyproject.toml")
REFRESH = "ci/images-refresh.txt"
REVISION = "d3ae8b128f179a4ba29dd1c6d997539aec122711"


class ShellFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="td12-images-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in (*INPUTS, ".woodpecker/ci.yml", ".woodpecker/build-images.yml"):
            dest = self.root / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, dest)
        for name in (*MCP_INPUTS, REFRESH):
            dest = self.root / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text("FROM scratch\n" if name.endswith("Dockerfile.ci") else "fixture\n")
        shutil.copytree(ROOT / "ci/woodpecker", self.root / "ci/woodpecker")
        shutil.copyfile(ROOT / "ci/build-images.sh", self.root / "ci/build-images.sh")
        self.git("init", "-q")
        self.git("update-index", "--add", "--cacheinfo",
                 f"160000,{REVISION},tools/mcp/freecad-mcp")
        self.script("key", "update")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True,
                              capture_output=True, text=True).stdout

    def script(self, name, *args, success=True, env=None):
        result = subprocess.run(["sh", f"ci/woodpecker/ci-images-{name}.sh", *args],
                                cwd=self.root, env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def tags(self):
        return self.script("key", "tags").splitlines()


class KeyTests(ShellFixture):
    def test_key_framing_and_determinism(self):
        checksums = "".join(
            hashlib.sha256((self.root / name).read_bytes()).hexdigest()
            + "  " + name + "\n" for name in (*INPUTS, REFRESH))
        deps = hashlib.sha256(("deps-v2\n" + checksums).encode()).hexdigest()[:12]
        mcp_checksums = "".join(
            hashlib.sha256((self.root / name).read_bytes()).hexdigest()
            + "  " + name + "\n" for name in MCP_INPUTS)
        mcp = hashlib.sha256(f"mcp-v2\n{deps}\n{mcp_checksums}".encode()).hexdigest()[:12]
        self.assertEqual(self.tags(), [f"24.04-{deps}", f"24.04-{mcp}"])
        self.assertEqual(self.tags(), self.tags())

    def test_every_deps_input_changes_both_keys(self):
        baseline = self.tags()
        for name in INPUTS:
            with self.subTest(name=name):
                p = self.root / name
                original = p.read_bytes()
                p.write_bytes(original + b"\n# changed\n")
                changed = self.tags()
                self.assertNotEqual(baseline[0], changed[0])
                self.assertNotEqual(baseline[1], changed[1])
                p.write_bytes(original)

    def test_gitlink_and_mcp_source_changes_keep_keys(self):
        baseline = self.tags()
        self.git("update-index", "--cacheinfo",
                 f"160000,{'a' * 40},tools/mcp/freecad-mcp")
        (self.root / "tools/mcp/freecad-mcp/src").mkdir()
        (self.root / "tools/mcp/freecad-mcp/src/change.py").write_text("# source only")
        self.assertEqual(baseline, self.tags())

    def test_each_mcp_dependency_content_changes_only_mcp_key(self):
        for name in MCP_INPUTS:
            baseline = self.tags()
            p = self.root / name
            p.write_text(p.read_text() + "# dependency change\n")
            self.assertEqual(baseline[0], self.tags()[0])
            self.assertNotEqual(baseline[1], self.tags()[1])

    def test_missing_inputs_fail_closed(self):
        for name in (*INPUTS, *MCP_INPUTS, REFRESH):
            p = self.root / name
            original = p.read_bytes()
            p.unlink()
            self.script("key", "tags", success=False)
            p.write_bytes(original)

    def test_repository_refresh_changes_both_keys(self):
        baseline = self.tags()
        (self.root / REFRESH).write_text("next refresh\n")
        self.assertNotEqual(baseline[0], self.tags()[0])
        self.assertNotEqual(baseline[1], self.tags()[1])

    def test_gates_wait_for_mcp_checkout(self):
        for workflow, prerequisite in (("ci.yml", "all-submodules-check"),
                                       ("build-images.yml", "init-mcp-submodule")):
            steps = yaml.safe_load((ROOT / ".woodpecker" / workflow).read_text())["steps"]
            gate = next(step for step in steps if step["name"] == "ci-images-check")
            self.assertIn(prerequisite, gate["depends_on"])

    def test_keyed_shards_preserve_failure_continuation(self):
        workflow = yaml.safe_load((ROOT / ".woodpecker/ci.yml").read_text())
        steps = {step["name"]: step for step in workflow["steps"]}
        def ancestors(name):
            return {name}.union(*(ancestors(dep) for dep in steps[name].get("depends_on", [])))
        for name, step in steps.items():
            if "freecad-ci-" not in step["image"]:
                continue
            self.assertIn("ci-images-ready", ancestors(name))
        for shard, previous in (("sketch", "collab"), ("part", "sketch"),
                                ("rest", "part")):
            step = steps[f"freecad-mcp-core-{shard}"]
            self.assertEqual(step["depends_on"], [f"freecad-mcp-core-{previous}"])
            self.assertEqual(step["when"], [
                {"event": ["tag", "manual"], "status": ["success", "failure"]},
                {"event": "pull_request", "status": ["success", "failure"]}])
        self.assertTrue(all(dep in steps for step in steps.values()
                            for dep in step.get("depends_on", [])))

    def test_arch_entry_provisions_yaml_parser(self):
        bindir = self.root / "bin"
        bindir.mkdir()
        python = bindir / "python3"
        python.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\" >> python-calls\n"
                          "case \"$*\" in *'import pytest'*) case \"$*\" in *yaml*) exit 1;; esac;; esac\n")
        python.chmod(0o755)
        result = subprocess.run(["sh", "ci/woodpecker/freecad-nonblocking-tests.sh"],
                                cwd=self.root, capture_output=True, text=True,
                                env={**os.environ, "TIER": "arch",
                                     "PATH": f"{bindir}:{os.environ['PATH']}"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PyYAML", (self.root / "python-calls").read_text())

    def test_refresh_and_host_migration_documented(self):
        docs = ROOT / "ci/woodpecker/ci-images.md"
        self.assertTrue(docs.exists(), "repository-owned image migration notes missing")
        text = docs.read_text()
        for phrase in ("same host", "remote", "ci/build-images.sh", "upstream",
                       "ci/images-refresh.txt", "eviction", "lock", "24.04"):
            self.assertIn(phrase, text)
        self.assertTrue((ROOT / REFRESH).exists())

    def test_mismatch_repair_and_update_is_idempotent(self):
        self.script("key", "update")
        p = self.root / INPUTS[1]
        p.write_text(p.read_text() + "\n# test change\n")
        self.assertIn("ci-images-key.sh update", self.script("key", "check", success=False))
        self.script("key", "update")
        self.script("key", "check")
        before = (self.root / ".woodpecker/ci.yml").read_bytes()
        self.script("key", "update")
        self.assertEqual(before, (self.root / ".woodpecker/ci.yml").read_bytes())

    def test_each_workflow_must_agree(self):
        for name in ("ci.yml", "build-images.yml"):
            p = self.root / ".woodpecker" / name
            original = p.read_text()
            p.write_text(original.replace(self.tags()[1], "24.04-deadbeef0000"))
            self.assertIn(name, self.script("key", "check", success=False))
            p.write_text(original)

    def test_mismatch_fails_before_registry_access(self):
        p = self.root / INPUTS[0]
        p.write_text(p.read_text() + "\n# changed\n")
        self.assertIn("stale deps image key", self.script("check", success=False, env={
            **os.environ, "FREECAD_CI_REGISTRY": "http://127.0.0.1:1"}))
        self.assertFalse((self.root / ".ci-images").exists())

    def test_unrelated_change_keeps_keys(self):
        baseline = self.tags()
        (self.root / "ci/unrelated.txt").write_text("not a dependency input")
        self.assertEqual(baseline, self.tags())


@unittest.skipUnless(os.environ.get("FREECAD_CI_REGISTRY_TESTS") == "1",
                     "opt in to the disposable Docker registry suite")
class RegistryTests(ShellFixture):
    @classmethod
    def setUpClass(cls):
        cls.container = subprocess.run([
            "docker", "run", "--rm", "--memory=512m", "--cpus=1", "-d",
            "-p", "127.0.0.1::5000", "-e", "REGISTRY_STORAGE_DELETE_ENABLED=true",
            "registry:2"], check=True, capture_output=True, text=True).stdout.strip()
        cls.addClassCleanup(subprocess.run, ["docker", "stop", cls.container],
                            check=True, capture_output=True)
        endpoint = subprocess.run(["docker", "port", cls.container, "5000/tcp"],
                                  check=True, capture_output=True, text=True).stdout.strip()
        assert endpoint.startswith("127.0.0.1:") and not endpoint.endswith(":5001")
        cls.registry = "http://" + endpoint
        for _ in range(50):
            try:
                with urlopen(cls.registry + "/v2/", timeout=2):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            raise AssertionError("throwaway registry did not start")

    def setUp(self):
        super().setUp()
        docker_config = self.root / "docker-config"
        docker_config.mkdir()
        (docker_config / "config.json").write_text("{}")
        self.env = {**os.environ, "DOCKER_CONFIG": str(docker_config),
                    "FREECAD_CI_REGISTRY": self.registry,
                    "FREECAD_CI_PULL_HOST": self.registry.removeprefix("http://"),
                    "FREECAD_CI_VERIFY_ATTEMPTS": "1"}

    def api(self, method, path, data=None, content_type=None):
        headers = {"Accept": "application/vnd.docker.distribution.manifest.v2+json, "
                   "application/vnd.oci.image.manifest.v1+json"}
        if content_type:
            headers["Content-Type"] = content_type
        with urlopen(Request(urljoin(self.registry, path), data=data,
                             headers=headers, method=method), timeout=5) as response:
            return dict(response.headers)

    def publish(self, repo, tag, marker):
        config = json.dumps({"architecture": "amd64", "os": "linux",
                             "rootfs": {"type": "layers", "diff_ids": []},
                             "config": {"Labels": {"test": marker}}}).encode()
        digest = "sha256:" + hashlib.sha256(config).hexdigest()
        location = self.api("POST", f"/v2/{repo}/blobs/uploads/", b"")["Location"]
        self.api("PUT", location + "&digest=" + digest, config, "application/octet-stream")
        manifest = json.dumps({"schemaVersion": 2,
                               "mediaType": "application/vnd.docker.distribution.manifest.v2+json",
                               "config": {"mediaType": "application/vnd.docker.container.image.v1+json",
                                          "size": len(config), "digest": digest},
                               "layers": []}).encode()
        self.api("PUT", f"/v2/{repo}/manifests/{tag}", manifest,
                 "application/vnd.docker.distribution.manifest.v2+json")
        return "sha256:" + hashlib.sha256(manifest).hexdigest()

    def prepare(self):
        p = self.root / REFRESH
        p.write_text(self.id() + str(time.time_ns()))
        self.script("key", "update")
        self.deps, self.mcp = self.tags()
        self.env.update({"FREECAD_CI_CHECK_ATTEMPTS": "1",
                         "FREECAD_CI_CHECK_INTERVAL": "0.01",
                         "FREECAD_CI_PUBLISH_LOCK_ROOT": str(self.root / "locks"),
                         "CRANE_BIN": os.environ.get("CRANE_BIN", shutil.which("crane") or "crane")})
        fake = self.root / "executor"
        fake.write_text("#!/usr/bin/env python3\n" + FAKE_EXECUTOR)
        fake.chmod(0o755)
        self.env["KANIKO_EXECUTOR"] = str(fake)
        self.script("check", env=self.env)

    def build(self, which):
        return self.script("build", which, env=self.env)

    def test_parent_digest_invalidation(self):
        self.prepare()
        old = self.publish("freecad-ci-deps", self.deps, "old")
        self.build("mcp")
        old_mcp = self.api("GET", f"/v2/freecad-ci-mcp/manifests/{self.mcp}")["Docker-Content-Digest"]
        self.assertIn(old, self.script("verify", env=self.env))
        self.api("DELETE", f"/v2/freecad-ci-deps/manifests/{old}")
        self.script("check", env=self.env)
        self.assertTrue((self.root / ".ci-images/mcp-missing").exists())
        self.build("deps")
        new_parent = self.api("GET", f"/v2/freecad-ci-deps/manifests/{self.deps}")["Docker-Content-Digest"]
        output = self.build("mcp")
        self.assertIn(f"replacing stale-parent MCP tag {self.mcp}", output)
        self.assertIn(f"old MCP digest={old_mcp}", output)
        self.assertIn(f"old parent={old}", output)
        self.assertIn(f"new parent={new_parent}", output)
        self.script("verify", env=self.env)
        new_mcp = self.api("GET", f"/v2/freecad-ci-mcp/manifests/{self.mcp}")["Docker-Content-Digest"]
        self.assertNotEqual(old_mcp, new_mcp)
        identity = (self.root / ".ci-images/identity.txt").read_text()
        self.assertIn(new_mcp, identity)
        self.assertIn(f"deps parent {new_parent}", identity)
        self.assertNotIn(old_mcp, identity)
        builds = (self.root / "executor-args").read_text()
        self.assertEqual(builds.count("org.freecad.ci.deps-digest="), 2)

    def test_matching_parent_is_not_rebuilt_or_overwritten(self):
        self.prepare()
        self.build("deps")
        self.build("mcp")
        before = self.api("GET", f"/v2/freecad-ci-mcp/manifests/{self.mcp}")["Docker-Content-Digest"]
        builds = (self.root / "executor-args").read_text()
        self.env["KANIKO_EXECUTOR"] = "/must-not-build"
        self.assertIn("using it", self.build("mcp"))
        self.assertEqual(builds, (self.root / "executor-args").read_text())
        self.assertEqual(before, self.api("GET", f"/v2/freecad-ci-mcp/manifests/{self.mcp}")["Docker-Content-Digest"])
        self.assertIn(before, self.script("verify", env=self.env))

    def test_unlabelled_existing_mcp_is_never_overwritten(self):
        self.prepare()
        self.build("deps")
        before = self.publish("freecad-ci-mcp", self.mcp, "unlabelled")
        self.script("build", "mcp", success=False, env=self.env)
        self.assertEqual(before, self.api("GET", f"/v2/freecad-ci-mcp/manifests/{self.mcp}")["Docker-Content-Digest"])

    def test_host_lock_directory_is_shared_with_local_user(self):
        self.prepare()
        self.build("deps")
        self.assertEqual((self.root / "locks").stat().st_mode & 0o7777, 0o1777)

    def test_noop_repairs_aliases_without_executor(self):
        self.prepare()
        self.build("deps")
        self.build("mcp")
        # Point the aliases elsewhere, then ensure the no-op paths repair them.
        self.publish("freecad-ci-deps", "24.04", "other")
        self.publish("freecad-ci-mcp", "24.04", "other")
        self.env["KANIKO_EXECUTOR"] = "/must-not-build"
        self.build("deps")
        self.build("mcp")
        for which, tag in (("deps", self.deps), ("mcp", self.mcp)):
            self.assertEqual(self.api("GET", f"/v2/freecad-ci-{which}/manifests/{tag}")["Docker-Content-Digest"],
                             self.api("GET", f"/v2/freecad-ci-{which}/manifests/24.04")["Docker-Content-Digest"])

    def test_concurrent_builds_preserve_first_publication(self):
        self.concurrent_builds("deps")

    def test_concurrent_stale_parent_repairs_preserve_first_publication(self):
        self.concurrent_builds("mcp")

    def concurrent_builds(self, which):
        self.prepare()
        if which == "mcp":
            old = self.publish("freecad-ci-deps", self.deps, "old")
            self.build("mcp")
            self.api("DELETE", f"/v2/freecad-ci-deps/manifests/{old}")
            self.build("deps")
        # Both builds reach a filesystem barrier before publication.
        env = {**self.env, "TEST_BARRIER": "1"}
        other = self.root / "second"
        shutil.copytree(self.root, other, ignore=shutil.ignore_patterns("second", ".git"))
        env2 = {**env, "KANIKO_EXECUTOR": str(other / "executor"), "TEST_MARKER": "second"}
        procs = [subprocess.Popen(["sh", "ci/woodpecker/ci-images-build.sh", which],
                                 cwd=root, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True) for root, e in ((self.root, env), (other, env2))]
        for proc in procs:
            self.addCleanup(lambda p=proc: p.poll() is None and p.kill())
        for _ in range(300):
            if (self.root / "built").exists() and (other / "built").exists():
                break
            time.sleep(.01)
        (self.root / "release").touch()
        first = procs[0].communicate(timeout=20)[0]
        (other / "release").touch()
        second = procs[1].communicate(timeout=20)[0]
        self.assertEqual([p.returncode for p in procs], [0, 0], first + second)
        self.assertIn("appeared", second)
        if which == "mcp":
            self.assertIn("replacing stale-parent MCP tag", first)
            self.assertNotIn("replacing stale-parent MCP tag", second)
            self.script("verify", env=self.env)
        # Both actually built, but the second candidate must never be published.
        output = subprocess.check_output([self.env["CRANE_BIN"], "config", "--insecure",
                    f"{self.env['FREECAD_CI_PULL_HOST']}/freecad-ci-{which}:{self.deps if which == 'deps' else self.mcp}"],
                    text=True, env=self.env)
        self.assertNotIn('"test":"second"', output.replace(" ", ""))

    def test_grace_loop_waits_and_is_bounded(self):
        self.prepare()
        self.publish("freecad-ci-deps", self.deps, "grace")
        self.build("mcp")
        # Fake sleep logs the actual bounded polling without a long wait.
        bindir = self.root / "bin"
        bindir.mkdir()
        sleep = bindir / "sleep"
        sleep.write_text("#!/bin/sh\necho waited >> waits\n")
        sleep.chmod(0o755)
        # Missing keyed manifests must trigger three checks, not wait forever.
        digest = self.api("GET", f"/v2/freecad-ci-deps/manifests/{self.deps}")["Docker-Content-Digest"]
        self.api("DELETE", f"/v2/freecad-ci-deps/manifests/{digest}")
        env = {**self.env, "CI_PIPELINE_EVENT": "manual", "CI_IMAGES_WORKFLOW": "consumer",
               "FREECAD_CI_CHECK_ATTEMPTS": "3", "PATH": f"{bindir}:{os.environ['PATH']}"}
        output = self.script("check", env=env)
        self.assertIn("concurrent build-images", output)
        self.assertEqual((self.root / "waits").read_text().count("waited"), 2)

    def test_gate_rejects_unlabelled_mcp_and_records_eviction(self):
        self.prepare()
        self.publish("freecad-ci-deps", self.deps, "deps")
        self.publish("freecad-ci-mcp", self.mcp, "unlabelled")
        self.script("verify", success=False, env=self.env)
        self.assertFalse((self.root / ".ci-images/identity.txt").exists())
        # Also documents the remaining mid-pipeline eviction window.
        digest = self.api("GET", f"/v2/freecad-ci-mcp/manifests/{self.mcp}")["Docker-Content-Digest"]
        self.api("DELETE", f"/v2/freecad-ci-mcp/manifests/{digest}")
        self.build("mcp")
        self.script("verify", env=self.env)
        digest = self.api("GET", f"/v2/freecad-ci-deps/manifests/{self.deps}")["Docker-Content-Digest"]
        self.api("DELETE", f"/v2/freecad-ci-deps/manifests/{digest}")
        self.script("verify", success=False, env=self.env)
        self.assertFalse((self.root / ".ci-images/identity.txt").exists())

    def test_local_builder_uses_same_publication_contract(self):
        self.prepare()
        bindir = self.root / "bin"
        bindir.mkdir()
        docker = bindir / "docker"
        docker.write_text("#!/usr/bin/env python3\n" + FAKE_DOCKER)
        docker.chmod(0o755)
        env = {**self.env, "PATH": f"{bindir}:{os.environ['PATH']}",
               "REGISTRY": self.env["FREECAD_CI_PULL_HOST"]}
        for _ in range(2):
            result = subprocess.run(["bash", "ci/build-images.sh"], cwd=self.root,
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "docker-builds").read_text().count("build"), 2)
        self.assertIn("org.freecad.ci.deps-digest=", (self.root / "docker-args").read_text())


# Both mock builders emit a tiny Docker archive. No Dockerfile is executed.
ARCHIVE_WRITER = r'''
import hashlib, io, json, os, tarfile
from pathlib import Path
def archive(path, parent):
    config = json.dumps({"architecture":"amd64", "os":"linux",
        "rootfs":{"type":"layers", "diff_ids":[]},
        "config":{"Labels":{"test":os.getenv("TEST_MARKER", "first"),
                               "org.freecad.ci.deps-digest":parent}}}).encode()
    name = hashlib.sha256(config).hexdigest() + ".json"
    manifest = json.dumps([{"Config":name,"RepoTags":["test:latest"],"Layers":[]}]).encode()
    with tarfile.open(path, "w") as tar:
        for filename, data in ((name, config), ("manifest.json", manifest)):
            info = tarfile.TarInfo(filename); info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
'''
FAKE_EXECUTOR = ARCHIVE_WRITER + r'''
import sys, time
args = sys.argv[1:]
with open("executor-args", "a") as log: log.write("\n".join(args) + "\n")
parent = next((a.split("org.freecad.ci.deps-digest=",1)[1] for a in args
               if "org.freecad.ci.deps-digest=" in a), "")
path = next((a.split("=",1)[1] for a in args if a.startswith("--tar-path=")), "unused.tar")
archive(path, parent)
Path("built").touch()
if os.getenv("TEST_BARRIER"):
    deadline = time.monotonic() + 8
    while not Path("release").exists() and time.monotonic() < deadline: time.sleep(.01)
'''
FAKE_DOCKER = ARCHIVE_WRITER + r'''
import sys
args = sys.argv[1:]
with open("docker-args", "a") as log: log.write("\n".join(args) + "\n")
if args[0] == "build":
    with open("docker-builds", "a") as log: log.write("build\n")
    parent = next((a.split("org.freecad.ci.deps-digest=",1)[1] for a in args
                   if "org.freecad.ci.deps-digest=" in a), "")
    archive("docker-image.tar", parent)
elif args[0] == "save":
    Path(args[args.index("-o")+1]).write_bytes(Path("docker-image.tar").read_bytes())
else: raise SystemExit("unexpected docker operation: " + repr(args))
'''

if __name__ == "__main__":
    unittest.main()
