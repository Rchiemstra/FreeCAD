# Docker E2E reproducibility

Canonical pins: [`config/runtime-versions.lock.yaml`](../config/runtime-versions.lock.yaml).

## Pinned layers

| Layer | Mechanism |
| --- | --- |
| ROS base | `FROM ros:jazzy-ros-base-noble@sha256:…` in `docker/Dockerfile.e2e` |
| FreeCAD / Gazebo apt | `ARG` + `apt-get install pkg=version` |
| RobotCAD | `ROBOTCAD_GIT_REF` git fetch/checkout |
| PyPI (MCP venv) | `requirements-mcp-e2e.txt` |
| Robot source | `robots/arm_2dof.FCStd` SHA-256 in lock file |

Each E2E run writes `sim_runs/e2e_<timestamp>/versions.yaml` with observed versions and drift vs the lock.

## Strict version check (default in E2E)

`E2E_VERSION_STRICT=1` (default in `docker/compose.e2e.yml`) makes `e2e/record_runtime_versions.py` **fail the run** when:

- `robots/arm_2dof.FCStd` hash/size differs from the lock
- RobotCAD commit differs
- Any `docker_e2e.apt_versions` package differs from `dpkg-query`
- MCP venv PyPI packages differ from `pypi` in the lock
- Built-in base image ref (`/etc/e2e-base-image.ref`) differs from `base_image_digest` in the lock

Warnings (non-fatal unless strict extended later): CLI-reported FreeCAD/Gazebo semver vs lock prefixes.

Set `E2E_VERSION_STRICT=0` to record drift as warnings only.

## Controlled base-image digest update

When you intentionally want a newer `ros:jazzy-ros-base-noble`:

1. Pull and note digest:
   ```bash
   docker pull ros:jazzy-ros-base-noble
   docker image inspect ros:jazzy-ros-base-noble --format '{{index .RepoDigests 0}}'
   ```
2. Update **both**:
   - `docker/Dockerfile.e2e` — `FROM ros:jazzy-ros-base-noble@sha256:<new>`
   - `config/runtime-versions.lock.yaml` — `docker_e2e.base_image_digest`
   - `/etc/e2e-base-image.ref` line written in Dockerfile `RUN` (search `e2e-base-image.ref`)
3. Rebuild and run strict E2E (no `generated/`):
   ```bash
   rm -rf generated
   docker compose -f docker/compose.e2e.yml build --no-cache
   docker compose -f docker/compose.e2e.yml up --abort-on-container-exit --exit-code-from e2e
   ```
4. If apt versions changed, update `docker_e2e.apt_versions` in the lock from `sim_runs/e2e_*/versions.yaml` → `apt_versions`, and matching `ARG` defaults in `Dockerfile.e2e`.
5. Set `updated:` date in the lock file.

Helper (after a passing E2E):

```bash
python scripts/sync_runtime_lock_from_versions.py sim_runs/e2e_<timestamp>/versions.yaml
```

(Dry-run: add `--check` to print diffs only.)

## Controlled apt version update

When FreeCAD daily or Gazebo Harmonic archives move forward:

1. Build once without exact apt pins (or query versions inside a throwaway container):
   ```bash
   docker run --rm freecad-gazebo-mcp-e2e:latest \
     dpkg-query -W -f='${Package}=${Version}\n' freecad-daily gz-harmonic gz-sim8-cli
   ```
2. Copy versions into `config/runtime-versions.lock.yaml` → `docker_e2e.apt_versions` and `Dockerfile.e2e` `ARG` lines.
3. Re-run strict E2E from a clean `generated/`.
