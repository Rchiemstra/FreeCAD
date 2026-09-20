# Hetzner FreeCAD CI cheatsheet

This is the repeatable recipe used to validate the `fix/mcp-limits` FreeCAD
and freecad-mcp work on a temporary Hetzner server. Keep source changes in the
Windows checkout; use the server only as a disposable build and test worker.

## Connect

From PowerShell:

```powershell
ssh -i "$env:USERPROFILE\.ssh\ssh-hetzner" `
  -o IdentitiesOnly=yes `
  root@SERVER_IPV4
```

Never copy the private key or a Hetzner API token into the repository or the
server workspace.

## Recommended host and limits

The successful run used a Hetzner `ccx33` host with 8 vCPUs and about 32 GiB
RAM. Docker had no swap and remained safe with four to five isolated FreeCAD
test processes.

Choose concurrency from both CPU and available RAM:

```text
native_test_jobs = min(4, max(1, cpu_count / 2), max(1, available_ram_gib / 6))
compile_jobs     = min(cpu_count, max(1, available_ram_gib / 2))
```

That selects four native jobs and eight compile jobs on the `ccx33`, but only
one native job and three compile jobs in a 6 GiB WSL VM. An explicit
`FREECAD_CI_JOBS` override should win when the automatic choice is unsuitable.

## Disposable workspace layout

```text
/srv/freecad-ci/FreeCAD       parent checkout and build/debug
/srv/freecad-ci/freecad-mcp   standalone MCP checkout for Python-only jobs
freecad-ci-pip-cache          persistent Docker pip-cache volume
freecad-ci-ccache             persistent Docker ccache volume
127.0.0.1:5001               disposable local image registry
```

Example bootstrap:

```bash
mkdir -p /srv/freecad-ci
git clone --recurse-submodules https://github.com/Rchiemstra/FreeCAD.git \
  /srv/freecad-ci/FreeCAD
git clone https://github.com/Rchiemstra/freecad-mcp.git \
  /srv/freecad-ci/freecad-mcp
git -C /srv/freecad-ci/FreeCAD submodule update --init --recursive
docker volume create freecad-ci-pip-cache
docker volume create freecad-ci-ccache
docker run -d --restart unless-stopped --name freecad-ci-registry \
  -p 127.0.0.1:5001:5000 registry:2
```

## Sync uncommitted test changes

Stream only known files. Do not copy `.git`, the full Windows worktree, build
outputs, or secrets.

```bash
tar -C /mnt/c/Users/Rchie/Music/FreeCAD -cf - \
  ci/woodpecker/freecad-mcp-core-shards.py \
  tests/architecture/test_generic_isolated_recompute.py |
ssh -i /mnt/c/Users/Rchie/.ssh/ssh-hetzner root@SERVER_IPV4 \
  'tar -C /srv/freecad-ci/FreeCAD -xf -'

tar -C /mnt/c/Users/Rchie/Music/FreeCAD/tools/mcp/freecad-mcp -cf - \
  tests/test_find_subshapes_filters.py \
  tests/test_inspect_geometry_subshape.py |
ssh -i /mnt/c/Users/Rchie/.ssh/ssh-hetzner root@SERVER_IPV4 \
  'tar -C /srv/freecad-ci/freecad-mcp -xf -'
```

Repeat the MCP archive for the parent checkout at
`/srv/freecad-ci/FreeCAD/tools/mcp/freecad-mcp`; one tar stream cannot be
consumed by two remote extraction commands.

## Test order

Use the scripts under `ci/woodpecker/` rather than reimplementing CI commands.
The PR test matrix is:

1. submodule, MCP-pin, lint, Pixi-lock, and CI-image gates;
2. debug configure and build;
3. parent C++ unit, `FreeCADCmd -t 0` integration, and GUI e2e;
4. MCP lint, Python unit, and real Git-sidecar integration;
5. MCP load preflight;
6. MCP core shards (`collab`, `sketch`, `part`, `rest`) and MCP e2e.

The native core shards are independent after preflight. To run them in
parallel, give every process a private copy of the small MCP checkout while
sharing `/srv/freecad-ci/FreeCAD/build/debug`. This prevents collisions in
`results_core.xml` and `ci_rc_core.txt`.

```bash
SHARD_ROOT=$(mktemp -d /srv/freecad-ci/mcp-part.XXXXXX)
cp -a /srv/freecad-ci/freecad-mcp/. "$SHARD_ROOT/"

docker run --rm --name freecad-mcp-core-part \
  --cpus=3 --memory=10g \
  -e CI_WORKSPACE=/workspace \
  -e MARKER=core \
  -e CORE_SHARD=part \
  -e FREECAD_MCP_REQUIRE_NATIVE_COLLABORATION=1 \
  -v /srv/freecad-ci/FreeCAD:/workspace \
  -v "$SHARD_ROOT":/workspace/tools/mcp/freecad-mcp \
  -w /workspace \
  127.0.0.1:5001/freecad-ci-mcp:24.04 \
  sh ci/woodpecker/freecad-mcp-freecad-tests.sh
```

Do not use pytest-xdist inside one embedded FreeCAD process. Parallelize with
isolated FreeCAD processes instead.

## Fast failure gates

Before paying for long native tests:

```bash
python3 ci/woodpecker/freecad-mcp-core-shards.py \
  --root tools/mcp/freecad-mcp --check
sh ci/woodpecker/freecad-mcp-lint.sh
sh ci/woodpecker/freecad-mcp-unit-tests.sh
```

Add a collection-only marker check before the full MCP e2e lane. Required
headless e2e must collect runnable tests with no non-xfail skips. Tests that
require a provisioned native GUI plus instance-manifest HMAC handshake belong
to the existing opt-in `session_e2e` marker.

## Monitor without flooding logs

```bash
docker stats --no-stream
free -h
uptime
docker ps --format 'table {{.Names}}\t{{.Status}}\t{{.RunningFor}}'
```

Stop adding jobs if available RAM approaches 6 GiB; stop the newest optional
job if it approaches 4 GiB. CPU saturation is expected and desirable.

## Successful reference run (2026-09-20)

Source snapshots:

- FreeCAD: `74be375c70cde9f857a3176a8c15ef2c6ab7ee56`
- freecad-mcp: `6d816dd4a76238e835fa68d8cf8fd97aa9cb68a6`

Results before any commit or push:

- debug build: 8,518 targets, green in 41 minutes;
- parent C++ unit suite: green;
- parent integration: 3,199 tests, green;
- parent GUI e2e: all configured GUI modules green;
- MCP lint/type gates: green, 379 mypy source files;
- MCP Python unit: 12,073 passed, 1 expected xfail;
- MCP Git-sidecar integration: 1 passed;
- architecture regression suite: 19 passed;
- MCP core collab: 59 passed;
- MCP core sketch: 256 passed;
- MCP core part: 335 passed;
- MCP core rest: 106 passed;
- MCP e2e: 128 passed, zero skips.

## Cleanup and stop billing

First preserve all needed results and source changes locally. Remove disposable
containers or workspaces only if the server will be retained:

```bash
docker rm -f freecad-ci-registry 2>/dev/null || true
docker volume rm freecad-ci-pip-cache freecad-ci-ccache 2>/dev/null || true
```

Powering off a Hetzner Cloud server does not necessarily stop billing. Resolve
the exact server by IPv4 and delete that exact server through the provider API:

```bash
SERVER_IPV4=203.0.113.10
SERVER_ID=$(hcloud server list -o json |
  jq -r --arg ip "$SERVER_IPV4" \
    '.[] | select(.public_net.ipv4.ip == $ip) | .id')
test -n "$SERVER_ID"
hcloud server describe "$SERVER_ID" -o json |
  jq '{id, name, status, ipv4: .public_net.ipv4.ip}'
hcloud server delete "$SERVER_ID"
```

Finally verify that no server with that ID or IPv4 remains in the project:

```bash
hcloud server list -o json |
  jq -e --arg id "$SERVER_ID" --arg ip "$SERVER_IPV4" \
    'all(.[]; (.id | tostring) != $id and .public_net.ipv4.ip != $ip)'
```
