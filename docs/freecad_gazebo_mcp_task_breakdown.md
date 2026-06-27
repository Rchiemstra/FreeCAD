# FreeCAD Gazebo MCP Task Breakdown

Source docs:

- [freecad_gazebo_mcp_plan.md](freecad_gazebo_mcp_plan.md)
- [FreeCAD Model Simulation Pipeline Integration.md](FreeCAD%20Model%20Simulation%20Pipeline%20Integration.md)
- [diagrams/freecad_gazebo_mcp_component_architecture.puml](diagrams/freecad_gazebo_mcp_component_architecture.puml)
- [diagrams/freecad_gazebo_mcp_deployment_docker.puml](diagrams/freecad_gazebo_mcp_deployment_docker.puml)
- [diagrams/freecad_gazebo_mcp_mcp_tool_surface.puml](diagrams/freecad_gazebo_mcp_mcp_tool_surface.puml)
- [diagrams/freecad_gazebo_mcp_sequence_human_in_workbench.puml](diagrams/freecad_gazebo_mcp_sequence_human_in_workbench.puml)
- [diagrams/freecad_gazebo_mcp_sequence_iteration_loop.puml](diagrams/freecad_gazebo_mcp_sequence_iteration_loop.puml)

Last reviewed: 2026-05-29
Last updated: 2026-05-29 (Phase 6 — export cache + FCStd hashing)

## Purpose

Build a repeatable robot simulation and regression-test workflow where FreeCAD is the human-facing cockpit, Gazebo runs headless as the physics backend, and MCP exposes FreeCAD, Gazebo, and eventually ROS 2 to an LLM-driven workflow.

The core product is a robot test rig, not MCP plumbing by itself.

1. Design or modify a robot in FreeCAD.
2. Define robot semantics through RobotCAD/CROSS: links, joints, inertias, sensors, controllers, visuals, and collisions.
3. Export generated simulation artifacts: URDF/SDF, meshes, ROS 2 package files, and launch/config files.
4. Run headless Gazebo scenarios.
5. Evaluate scenario assertions and quantitative metrics.
6. Show live simulation state, logs, plots, and pass/fail results inside FreeCAD.

## Working Assumptions

- FreeCAD `.FCStd` files are the source of truth.
- RobotCAD/CROSS is the v1 export backbone.
- Gazebo consumes generated URDF/SDF and mesh artifacts only.
- The human user stays inside FreeCAD.
- Gazebo runs headless with `gz sim -s`.
- ROS 2 is part of the intended stack because RobotCAD, Gazebo control, ros2_control, and ROS MCP workflows all expect it.
- Start with the RobotCAD Docker image or a single-host Docker setup to avoid ROS 2 dependency drift.
- MCP is the LLM control path; the FreeCAD Simulation Workbench is the human control path.
- Generated exports and simulation runs must be reproducible from source inputs.
- Scenario tests live in the project repo next to the models.
- V1 should keep tools and assertion language small, typed, and deterministic.

## Milestones

| Milestone | Target Outcome | Depends On |
| --- | --- | --- |
| Phase 0: Environment | FreeCAD MCP, Gazebo MCP, RobotCAD, ROS 2, and headless Gazebo work independently | Docker or local ROS 2 setup |
| Phase 1: Manual End-to-End | A toy robot exports from FreeCAD and simulates in headless Gazebo | RobotCAD export path |
| Phase 2: Automated Bridge | MCP tools can export, spawn, and run basic handoff flows | Phase 1 friction list |
| Phase 3: Simulation Workbench Viewer | FreeCAD animates live Gazebo state without opening Gazebo GUI | Stable Gazebo lifecycle and state topics |
| Phase 4: Test Runner | Scenarios run headlessly and produce pass/fail results | Scenario schema and assertion vocabulary |
| Phase 4.5: ROS 2 Control and Telemetry | Controller-in-the-loop scenarios can publish commands, inspect topics, and collect telemetry | Stable Phase 4 runner |
| Phase 5: Iteration Loops | LLM can change design parameters and rerun tests | Reliable export cache and test runner |
| Phase 6: Hardening | The system is reproducible, permissioned, typed, and robust | Earlier phases complete |
| Phase 7: Scale-Out Validation | Randomized environments, metrics dashboards, and CI test suites | Hardened test runner |
| **Docker E2E** | One compose command runs export smoke, MCP servers, gz sim, scenarios → `sim_runs/` | Linux Docker engine; see section below |

## MVP status (2026-05-29)

**MVP = repeatable robot test rig:** RobotCAD export → headless Gazebo spawn → scenario pass/fail, with FreeCAD workbench UI and LLM bridge paths. **Not in MVP:** Phase 4.5 ROS control-in-the-loop, Phase 6 hardening, Phase 7 scale-out.

### Phase completion (checklist tasks only)

| Phase | Done | Deferred (post-MVP) | Open / partial | **MVP %** |
| --- | ---: | ---: | ---: | ---: |
| 0 Environment | 14 | 0 | 0 | **100%** |
| 1 Manual E2E | 9 | 0 | 2 partial | **82%** |
| 2 Automated bridge | 9 | 2 | 0 | **100%** |
| 3 Sim workbench | 9 | 2 | 1 | **75%** |
| 4 Test runner | 13 | 0 | 0 | **100%** |
| 4.5 ROS control | 0 | — | 8 | **N/A** (post-MVP) |
| 5 Iteration loops | 6 | 2 | 0 | **100%** |
| 6 Hardening | 6 | — | 6 | **~50%** (in progress) |

**Overall MVP rig (Phases 0–5, excluding deferred rows):** **~91%** task checklist complete.

**Evidence (this pass):**

| Check | Result |
| --- | --- |
| Offline pytest (CI gate: `-m "not gazebo and not freecad and not needs_freecad"`) | **184 passed**, 7 deselected live (2026-05-29) |
| Docker E2E strict (`compose.e2e.yml` / `scripts/ci/run_docker_e2e.sh`) | **exit 0** — CI script + `robot-sim-ci.yml` job `docker-e2e` |
| Live handoff (`RUN_GAZEBO_LIVE=1`, Windows+WSL) | **OK** — `gazebo_connected` + spawn via `GAZEBO_SPAWN_VIA_GZ_CLI` (when stack up) |

**MVP definition-of-done (honest):**

| Criterion | Status |
| --- | --- |
| Export `arm_2dof` from FCStd (RobotCAD) | **Met** — FreeCADCmd + Docker E2E |
| Spawn exported URDF in headless Gazebo | **Met** — `gz_cli_bridge` / Docker E2E |
| Run scenario assertions headlessly | **Met** — `runner` + `e2e_smoke` |
| MCP servers start; bridge/handoff API | **Met** — offline + live/Docker |
| Human watches robot move in FreeCAD 3D view | **Partial** — workbench code + live transport exist; not re-verified this pass |
| ROS 2 command scenarios | **Not MVP** — Phase 4.5 |

### Shortest path: MVP → Phase 6 hardening

1. ~~**ODE mesh collision on `col_end_effector_.dae`**~~ — **Done (2026-05-29):** `bridge/urdf_for_gazebo.py` replaces end-effector **collision** trimesh with a **0.025 m sphere** at spawn time; **visual** `col_end_effector_.dae` unchanged. See [Collision / mesh policy](#collision--mesh-policy-gazebo-spawn) below. Other link collision meshes still use RobotCAD exports until V-HACD.
2. ~~**`robots/arm_2dof.FCStd` in git or CI artifact**~~ — **Done:** git-tracked + SHA-256 in `config/runtime-versions.lock.yaml`; optional `ROBOTS_ARM_2DOF_FCSTD_URL` / `e2e/fetch_robot_source.sh`.
3. ~~**Pin versions + `requirements-bridge.txt`**~~ — **Done (partial):** lock file + `requirements-mcp-e2e.txt`; E2E writes `sim_runs/e2e_*/versions.yaml`. Host FreeCAD/Gazebo still rolling.
4. ~~**Gazebo lifecycle hardening**~~ — **Done (partial 2026-05-29):** normalized `empty_world`, shared env/scripts, stop/restart/smoke — see [gazebo-lifecycle.md](gazebo-lifecycle.md).
5. **Sim workbench live smoke** — one documented “press play, see arm move” run with `Start-gz-sim` + workbench (closes Phase 3 human DoD).
6. **Commit `generated/` policy** — gitignore vs LFS vs CI-only export (Phase 6 reproducibility).
7. **Input hashes in export cache** — Phase 2 deferred item; needed for trustworthy iteration loops at scale.

## Docker E2E (no-human acceptance)

Implements § *Dockerized No-Human E2E Validation Plan* in [FreeCAD Model Simulation Pipeline Integration.md](FreeCAD%20Model%20Simulation%20Pipeline%20Integration.md).

### Target command (Linux Docker engine)

```powershell
docker compose -f docker/compose.e2e.yml up --build --abort-on-container-exit --exit-code-from e2e
```

### What is implemented

| Stage | Implementation |
| --- | --- |
| Base image | `docker/Dockerfile.e2e` — `ros:jazzy-ros-base-noble@sha256:eac11a52…` + pinned **Gazebo Harmonic** / **freecad-daily** apt; reproducibility: [docker-e2e-reproducibility.md](docker-e2e-reproducibility.md) |
| MCP venv | `/opt/mcp-venv` from `requirements-mcp-e2e.txt`; **runtime** `pip install -e` on bind-mounted `tools/mcp/{gazebo-mcp,freecad-mcp,ros-mcp-server}` |
| Driver script | **`e2e/run_e2e.sh`** — strict RobotCAD export (**no placeholder**): **`e2e/export_robotcad_cmd.sh`** (FreeCADCmd **`-c`** inline, avoids CRLF on `.py` scripts), **`e2e/assert_robotcad_export.sh`** (nested URDF + **`package://`** + **`/models`** symlink), **`e2e/run_gazebo_scenarios.sh e2e_smoke`** |
| Placeholder-only test | **`e2e/run_e2e_placeholder_fallback.sh`** + **`tests/scenarios_e2e/e2e_placeholder_fallback.yaml`** — compose profile **`fallback`** (`e2e-placeholder-fallback` service) |
| Runner ↔ Gazebo | **`bridge/gz_cli_bridge`** — spawn nested exported URDF via **`gz service /world/empty_world/create`** + **`prepare_urdf_for_gazebo`** (path rewrite + collision policy) + **`GZ_SIM_RESOURCE_PATH=/models`** |
| MCP smoke | **`e2e/mcp_smoke.py`** |
| Scenario | **`tests/scenarios_e2e/e2e_smoke.yaml`** — requires RobotCAD export path (main E2E) |

### Engineering choices / findings

- **Single container** first — matches the integration doc and avoids cross-container ROS DDS setup for v1.
- **`bridge/gazebo_bridge.py`**: native Linux uses local `.venv/bin/gazebo-mcp-server` with package `cwd`; Windows keeps **WSL** launch; MCP tools use **`gazebo_*`** names (`gazebo_spawn_sdf` for URDF/XML spawn, `gazebo_get_model_state`, pause/unpause/reset).
- **`runner/executor.py`**: live bridge loads **`gz_cli_bridge`** only when **`E2E_BRIDGE_MODULE`** is enabled; default **`None`** preserves mock-injected unit tests. **`_urdf_path`** prefers RobotCAD nested **`generated/.../arm_2dof_description/.../urdf/<robot>.urdf`**, then flat generated, then **`robots/<robot>.urdf`**.
- **`SIM_RUNS_DIR`** env wired through **`runner.runner` CLI** for `run` / `run-all`.
- **Main E2E is strict**: **`stage_export.sh`** fails without **`generated/.../arm_2dof_description/.../urdf/arm_2dof.urdf`**; placeholder copy only in **`run_e2e_placeholder_fallback.sh`**. Export uses **`e2e/export_robotcad_cmd.sh`** (FreeCADCmd **`-c`**) because bind-mounted **`export_robotcad_fcstd.py`** with CRLF is parsed as shell. Image must symlink **`freecad.overcross`** (see `Dockerfile.e2e`).
- **Shell scripts for Linux**: **`e2e/*.sh`** checked in as **LF** (`.gitattributes`); **CRLF** breaks shebang execution when Compose runs `bash -lc /workspace/e2e/run_e2e.sh`.
- **ROS environment**: **`set -u`** before **`source /opt/ros/jazzy/setup.bash`** trips on unset `AMENT_*` variables — bracket the source with **`set +u`** / **`set -u`**.
- **URDF spawn on gz-sim 8**: **`gz model --spawn-file`** is not available; use **`gz service -s /world/<world>/create`** with **`gz.msgs.EntityFactory`** and **`sdf_filename`** (Harmonic tutorial). Pre-delete uses **`/world/<world>/remove`**; “entity not found” on first run is benign.

### Blockers / next steps

- [x] Add **`robots/arm_2dof.FCStd`** with `Cross::*` entities — required for main E2E (mount into container); commit optional if CI supplies FCStd another way.
- [x] **`export_urdf` via FreeCADCmd** — `scripts/export_arm_2dof_fcstd.py` + `bridge/freecad_bridge.export_urdf_cmd()` (batch, no MCP timeout). MCP `execute_code` remains optional fallback.
- [ ] Optionally extend **gazebo-mcp** `spawn_model_wrapper` to pass **`model_xml`** through to `model_management.spawn_model` (bridge now uses **`gazebo_spawn_sdf`** with **`sdf_xml`** instead).
- **Agent validation (2026-05-29)**: **`docker compose -f docker/compose.e2e.yml up --build --abort-on-container-exit --exit-code-from e2e`** → **exit 0**, **`E2E finished OK (RobotCAD export + spawn)`**, **`[PASS] e2e_smoke — 4/4`**.
- **RobotCAD mesh export in E2E (2026-05-29)**: **PASS** — nested URDF + **`meshes/col_end_effector_.dae`**; spawn via **`gz_cli_bridge`** with **`prepare_urdf_for_gazebo`**. **ODE trimesh abort on shutdown:** **fixed** — collision mesh policy replaces `col_end_effector` collision with sphere; log `sim_runs/e2e_20260529T141257Z/console.log` has no `assertion "vertices"` / `Aborted` lines.
- **Collision / mesh policy:** see dedicated section below.
- **Placeholder fallback (optional)**: `docker compose -f docker/compose.e2e.yml --profile fallback up --abort-on-container-exit --exit-code-from e2e-placeholder-fallback`

### Reproducibility status (2026-05-29)

| Item | Status | Location |
| --- | --- | --- |
| Robot source `arm_2dof.FCStd` | **Tracked in git** (73 883 B, SHA-256 locked) | `robots/arm_2dof.FCStd`, `config/runtime-versions.lock.yaml` |
| CI fetch when FCStd absent | **Supported** | `ROBOTS_ARM_2DOF_FCSTD_URL` → `e2e/fetch_robot_source.sh` |
| PyPI pins (bridge / tests / E2E MCP venv) | **Pinned** | `requirements-bridge.txt`, `requirements-dev.txt`, `requirements-mcp-e2e.txt` |
| ROS base image digest | **Pinned** | `FROM ros:jazzy-ros-base-noble@sha256:eac11a52…` + `docker_e2e.base_image_digest` |
| FreeCAD / Gazebo apt | **Pinned** (`dpkg` versions) | `docker_e2e.apt_versions` + `Dockerfile.e2e` `ARG` install |
| Version gate at E2E end | **Strict by default** | `E2E_VERSION_STRICT=1` → `e2e/record_runtime_versions.py` fails on drift |
| Per-run audit trail | **Written** | `sim_runs/e2e_*/versions.yaml` (`apt_versions`, `drift_errors`, `drift_warnings`) |
| RobotCAD clone in E2E image | **Pinned commit** | `c3ac92843892b2b19eedfb7d536da81458e819b3` (`ROBOTCAD_GIT_REF`) |
| `generated/` / `sim_runs/` | **Gitignored** (export-only artifacts) | `.gitignore` |
| Digest / apt bump workflow | **Documented** | [docker-e2e-reproducibility.md](docker-e2e-reproducibility.md) |

**Verify locally:**

```powershell
python e2e\verify_robot_source.py
pip install -r requirements-bridge.txt -r requirements-dev.txt
Remove-Item -Recurse -Force generated -ErrorAction SilentlyContinue
docker compose -f docker/compose.e2e.yml build
docker compose -f docker/compose.e2e.yml up --abort-on-container-exit --exit-code-from e2e
```

### Remaining version-pinning gaps (Phase 6)

| Gap | Risk | Mitigation path |
| --- | --- | --- |
| **Host** FreeCAD (pixi 1.2-dev), Gazebo (WSL source build), ROS rolling | Local dev ≠ Docker E2E | Separate host lock doc or Nix/pixi env export; not gated by E2E |
| **MCP server** editable `pip install -e` at E2E runtime | Submodule HEAD can drift | Pin submodule SHAs in lock + `git checkout` in `run_e2e.sh` (not done yet) |
| **Transitive apt** deps of `freecad-daily` / `gz-harmonic` | Patch updates on same metapackage version | Pin full `apt_versions` set or generate from `dpkg -l` snapshot |
| **RobotCAD pip** deps installed inside FreeCADCmd export | Unpinned numpy/scipy in container | Record in `versions.yaml` or freeze FreeCAD AdditionalPythonPackages |
| **`ros:jazzy-ros-base-noble` tag** on Docker Hub | Tag can move before digest update | Always use `@sha256:` in Dockerfile (done); re-pull when bumping digest |
| **CI GitHub Actions** | **Wired** | [`.github/workflows/robot-sim-ci.yml`](../.github/workflows/robot-sim-ci.yml) + [docs/ci.md](ci.md) |
| **Input hashes** on export cache | Stale `generated/` if FCStd unchanged | Phase 2 deferred — FCStd SHA already strict |

### CI status (2026-05-29)

| Gate | CI job | Local command |
| --- | --- | --- |
| Offline pytest | `offline-pytest` | `bash scripts/ci/run_offline_pytest.sh` |
| Lifecycle smoke | `lifecycle-smoke` | `bash scripts/ci/run_lifecycle_smoke.sh` |
| Strict Docker E2E | `docker-e2e` | `bash scripts/ci/run_docker_e2e.sh` |
| All gates | workflow `robot-sim-ci` | `bash scripts/ci/run_local.sh` |

**Policies:** `CI=true`, `RUN_GAZEBO_LIVE` unset/0, `E2E_VERSION_STRICT=1`. Live `@pytest.mark.gazebo` / `freecad` / `needs_freecad` excluded via marker filter + `tests/conftest.py`.

**Remaining CI gaps:**

| Gap | Notes |
| --- | --- |
| Docker layer cache / image digest pin in GHA | E2E rebuilds image each run; use registry cache later |
| `smoke_gz_lifecycle.sh --docker` in CI | Needs pre-started stack; keep manual |
| Windows-hosted GHA | Linux-only; matches Docker E2E target |
| Scheduled nightly | Only `workflow_dispatch` + push/PR today |
| FreeCAD upstream CI | Separate from `robot-sim-ci.yml` (path-filtered) |

See [ci.md](ci.md).

### Gazebo lifecycle status (2026-05-29)

| Item | Status | Reference |
| --- | --- | --- |
| Canonical world + SDF | **Normalized** — `empty_world` / `worlds/empty_world.sdf` for E2E + live fast/source | [gazebo-lifecycle.md](gazebo-lifecycle.md) |
| Entry points | **Documented** — `Start-gz-sim.bat`, `Start-gz-sim-fast.bat`, `Start-gazebo-bridge.bat`, `Stop-gz-stack.bat` | same |
| Shared env / cleanup | **Implemented** — `gazebo_lifecycle_common.sh`, `stop_gz_stack.sh`, `ensure_gz_sim_headless.sh` | `bridge/gazebo_lifecycle.py` |
| ros_gz vs gz CLI | **Documented** — spawn via gz CLI; MCP sim via ros_gz when bridge healthy | config/gazebo-lifecycle.env.example |
| Offline tests / smoke | **Added** — `tests/test_gazebo_lifecycle.py`, `scripts/smoke_gz_lifecycle.sh` | |

**Remaining lifecycle gaps:**

| Gap | Notes |
| --- | --- |
| CI `smoke_gz_lifecycle.sh --docker` | Offline smoke in CI; `--docker` probe remains manual |
| MCP pause/step/reset on all Windows hosts | ros_gz `parameter_bridge` can still hang |
| Source `Start-gz-sim.bat` first-build time | 20–40 min; prefer `Start-gz-sim-fast.bat` daily |
| Workbench live play smoke | Not re-verified this pass |
| Unified `wait_gz_ready` helper in handoff | Retries in bridge; no single wait script |

### Collision / mesh policy (Gazebo spawn)

Applied in **`bridge/urdf_for_gazebo.py`** via **`prepare_urdf_for_gazebo()`** (used by **`gz_cli_bridge`**, **`gazebo_bridge.spawn_model`**, and any path that spawns RobotCAD exports). **On-disk RobotCAD export is not modified**; spawn uses a prepared copy when XML changes.

| Layer | Source | Gazebo spawn behavior |
| --- | --- | --- |
| **Visual** | RobotCAD `meshes/*.dae` (e.g. `col_end_effector_.dae`) | **Keep** — `package://` → `file:///models/arm_2dof_description/...` |
| **Collision** | RobotCAD `meshes/col_end_effector_.dae` on `end_effector` link | **Replace** — `<collision><geometry><mesh … col_end_effector…>` → `<sphere radius="0.025"/>` (matches placeholder `robots/arm_2dof.urdf` end-effector visual scale) |
| **Other links** | RobotCAD collision meshes | **Keep** until V-HACD / further hardening |

**Environment:**

| Variable | Default | Meaning |
| --- | --- | --- |
| `GAZEBO_COLLISION_MESH_POLICY` | `replace_end_effector_mesh` | Replace only meshes whose filename contains `col_end_effector` |
| | `keep` | Disable collision rewriting (debug; may reproduce ODE abort) |
| | `all_mesh_to_sphere` | Replace every collision `<mesh>` with 0.025 m sphere |
| `GAZEBO_URDF_USE_CONTAINER_PATHS` | `1` | Rewrite `package://arm_2dof_description/...` → `file:///models/arm_2dof_description/...` |

**Rationale:** Gazebo Harmonic + ODE/DART aborts building a trimesh from RobotCAD’s `col_end_effector_.dae` collision export (`assertion "vertices" failed`). Visual mesh is fine; only collision needs a primitive for stable headless CI.

**Tests:** `tests/test_bridge.py::TestUrdfForGazebo` — collision replacement + `keep` policy.

### `.dockerignore` note

`tools/mcp/` is **included** in the Docker build context again so **both** `docker/Dockerfile.e2e` and **`docker/compose.pytest.yml`** see MCP submodules (pytest image context grows).

## Phase 0: Environment

Goal: prove each runtime side works before connecting them.

Tasks:

- [x] Choose the first supported setup: **Windows host + WSL2 + Docker**. FreeCAD runs natively on Windows via pixi build. Gazebo and ROS 2 run in Docker containers launched via WSL2 (see `Start-gz-sim.bat` / `Start-ros2.bat`).
- [x] Document GUI forwarding approach for FreeCAD: **No GUI forwarding needed.** FreeCAD runs natively on Windows. The human sees FreeCAD's native window. The Gazebo window is intentionally not used (headless only).
- [x] Install or build FreeCAD 1.x: **FreeCAD 1.2.0-dev** built via pixi from repo source. Entry point: `.pixi/envs/default/Library/bin/FreeCAD.exe`. Launch via `Start-FreeCAD.bat`.
- [x] Verify RobotCAD/CROSS imports — **`scripts/install_robotcad_cross.ps1`** + **`scripts/verify_robotcad_cross.py`** via FreeCADCmd → `freecad.cross` **v1.0.1**. **`check_robotcad()`** returns **ok=True** when GUI FreeCAD + RPC **:9875** are up (after namespace/`freecad.utils` path fix in `bridge/freecad_bridge.py`).
- [x] Automated URDF export — **FreeCADCmd batch** (`export_urdf_cmd` / `scripts/export_arm_2dof_fcstd.py`). MCP RPC path optional (`prefer_cmd=False` or `export_arm_2dof_rpc.py`).
- [x] Start modern Gazebo headless with `gz sim -s`: **Confirmed working** via `Start-gz-sim.bat` (WSL2 + Docker, Ubuntu Noble + OSRF packages). Docker image: `ubuntu:noble`. Build volume: `gz-sim-linux-build`.
- [x] Install and run the selected FreeCAD MCP server: **`neka-nat/freecad-mcp` v0.1.17** installed in WSL2 Python 3.12 venv at `tools/mcp/freecad-mcp/.venv`. Server starts and initializes cleanly.
- [x] Verify the actual FreeCAD MCP/addon RPC transport and port: **XML-RPC on port 9875** (confirmed in `freecad_client.py` line 32 and `server.py` line 65). The `:5000` and `:9876` references in older docs are incorrect for this server.
- [x] FreeCADMCP addon installed to `%APPDATA%\FreeCAD\v1-2\Mod\FreeCADMCP`. On FreeCAD launch, switch to "MCP Addon" workbench and click "Start RPC Server" (or enable Auto-Start).
- [x] Verify an MCP client can create, inspect, and screenshot a simple FreeCAD object. **Automated when FreeCAD RPC is up**: `test_all_mcp.py` runs `create_document` → `create_object` → **`get_object`** → **`get_view`** → `execute_code` → `delete_object`. With FreeCAD closed, the suite still checks that `list_documents` fails gracefully via MCP.
- [x] Install and run the selected Gazebo MCP server: **`kvgork/gazebo-mcp` v0.2.0** installed in WSL2 Python 3.12 venv at `tools/mcp/gazebo-mcp/.venv`. Exposes 27 tools. `gazebo_list_models`, `gazebo_spawn_model`, `gazebo_delete_model` all respond (mock/OK) without Gazebo running.
- [x] Verify an MCP client can load a world, spawn or inspect a model, pause, resume, reset, and step headless Gazebo. **Met (2026-05-29)** — Docker E2E (`gz_cli_bridge`, `e2e_smoke`); live Windows path via `GAZEBO_SPAWN_VIA_GZ_CLI` + `gazebo_spawn_sdf`. Full MCP pause/step matrix not exercised; unpause + spawn + scenario telemetry covered.
- [x] Install a ROS 2 MCP option: **`ros-mcp` v3.0.1** installed in WSL2 Python 3.12 venv at `tools/mcp/ros-mcp-server/.venv`. Exposes 31 tools. `ping_robots`, `connect_to_robot`, `get_topics`, `get_nodes` all respond without ROS 2 running.
- [x] Document exact versions — see Version Table below.

Deliverables:

- [x] Reproducible environment notes — see Environment Decision and Version Table below.
- [x] Minimal smoke-test command list — `python test_all_mcp.py` (see Smoke-Test Commands and recorded run below).
- [x] Confirmed MCP transport/port notes — XML-RPC port 9875.
- [x] Known setup issues and fixes — see Phase 0 Notes below.
- [x] RobotCAD/CROSS install helper — `scripts/install_robotcad_cross.ps1`.

Definition of done:

- FreeCAD, Gazebo, and the selected MCP servers can be controlled independently (MCP protocol + mock/offline paths verified in CI/agent runs).
- No FreeCAD-to-Gazebo automation is required yet.
- **Remaining gaps (non-MVP):** optional GUI RobotCAD walkthrough; full Gazebo MCP tool matrix (pause/step/reset) via ros_gz on Windows — spawn path uses gz CLI where ros_gz hangs.

### Phase 0 Environment Decision

**Selected setup: Windows host + WSL2 + Docker**

| Layer | Choice | Rationale |
|---|---|---|
| FreeCAD | Windows-native (pixi build) | Already available in repo; avoids X11 forwarding |
| FreeCAD GUI forwarding | None needed | Runs on Windows; human sees native window |
| Gazebo | Docker in WSL2 (`ubuntu:noble` + OSRF packages) | Avoids host ROS 2 install; reproducible |
| ROS 2 | Docker in WSL2 (`ubuntu:noble`, rolling distro) | Same container approach as Gazebo |
| MCP servers | WSL2 Python 3.12 venvs | freecad-mcp requires Python ≥3.12; WSL provides it |
| FreeCAD MCP transport | XML-RPC on `localhost:9875` | Confirmed from `freecad_client.py` |

### Phase 0 Version Table

| Component | Version | Notes |
|---|---|---|
| Host OS | Windows 11 | FreeCAD runs here |
| WSL2 distro | Ubuntu 24.04.1 LTS (Noble) | Default: `Ubuntu-24.04` |
| Python (WSL2) | 3.12.3 | Used for all MCP server venvs |
| Python (Windows) | 3.10.6 | Insufficient for freecad-mcp (needs ≥3.12) |
| FreeCAD | 1.2.0-dev | Built from repo via pixi; `version.json` |
| freecad-mcp | 0.1.17 | `neka-nat/freecad-mcp`; MCP SDK 1.27.1 |
| gazebo-mcp | 0.2.0 | `kvgork/gazebo-mcp`; 27 tools exposed |
| ros-mcp | 3.0.1 | `tools/mcp/ros-mcp-server`; 31 tools exposed |
| Docker | 29.4.2 | Available in WSL2 |
| Gazebo | headless in Docker | Ubuntu Noble + OSRF; built from `src/3rdParty/gz-sim` |
| ROS 2 | rolling (Noble) | Built in Docker from `src/3rdParty/ros2` |
| RobotCAD/CROSS (OVERCROSS) | **v1.0.1** (`freecad.cross`) | **`scripts/install_robotcad_cross.ps1`** → `%APPDATA%\FreeCAD\v1-2\Mod\freecad.overcross`; junction `…/data/Mod/freecad.robotcad` → overcross; `git submodule update --init`. Verified via FreeCADCmd + `check_robotcad()` (RPC). |

### Phase 0 Smoke-Test Commands

```
# RobotCAD / CROSS — reproducible clone into FreeCAD Mod (Windows PowerShell)
.\scripts\install_robotcad_cross.ps1

# Run all MCP server protocol tests (no FreeCAD/Gazebo/ROS running required)
python test_all_mcp.py --timeout 30

# Run only FreeCAD MCP tests
python test_all_mcp.py --no-gazebo --no-ros

# Run with FreeCAD+Gazebo+ROS running (full integration)
python test_all_mcp.py --start-apps --startup-wait 30

# Offline pytest in Docker (Linux image — same skips as host; no FreeCAD/Gazebo inside)
docker compose -f docker/compose.pytest.yml build
docker compose -f docker/compose.pytest.yml run --rm pytest

# Full Docker E2E (Linux engine — ROS + Gazebo + FreeCAD daily; first build is large)
docker compose -f docker/compose.e2e.yml up --build --abort-on-container-exit --exit-code-from e2e

# Recorded run (2026-05-10): python test_all_mcp.py --timeout 30
#   Total: 17 passed, 0 failed, 0 skipped (FreeCAD XML-RPC not running — expected WARN then graceful list_documents).
#   When FreeCAD is running with the MCP addon RPC server on port 9875, the same command additionally exercises
#   create_document → create_object → get_object → get_view → execute_code → delete_object (full MCP integration path).
```

### Phase 0 Notes

**Completion pass (2026-05-10):**

- **`scripts/install_robotcad_cross.ps1`** clones/updates `drfenixion/freecad.overcross` into `%APPDATA%\FreeCAD\v1-2\Mod\freecad.overcross` (enable the workbench in FreeCAD after install).
- **`test_all_mcp.py`** now calls **`get_object`** and **`get_view`** after **`create_object`** when XML-RPC on **9875** is reachable (automates create / inspect / screenshot).
- **Offline pytest:** `python -m pytest tests -q` — recorded **151 passed, 6 skipped** (2026-05-10). Same suite runs in **`docker compose -f docker/compose.pytest.yml run --rm pytest`** (Python **3.12-bookworm** image; `.dockerignore` skips `src/3rdParty/*` and caches; **`tools/mcp/` stays in context** so images can `pip install -e` the MCP packages).
- **Still manual / environment-dependent:** RobotCAD demo in GUI; Sim workbench “watch robot move” smoke on Windows host.

**MCP transport resolution:**
- The FreeCAD MCP addon (`addon/FreeCADMCP/rpc_server/rpc_server.py`) runs `SimpleXMLRPCServer` on port **9875** (confirmed).
- The MCP server Python process (`freecad_mcp.server`) connects to `http://localhost:9875` via `xmlrpc.client`.
- Any references to ports 5000 or 9876 in old docs are incorrect for this server.

**FreeCADMCP addon installation:**
- Addon source: `tools/mcp/freecad-mcp/addon/FreeCADMCP/`
- Installed to: `%APPDATA%\FreeCAD\v1-2\Mod\FreeCADMCP\`
- After installing, restart FreeCAD, switch to "MCP Addon" workbench, click "Start RPC Server" (or enable Auto-Start in the FreeCAD MCP menu).

**Known setup issues:**
1. Windows Python 3.10/3.11 is too old for `freecad-mcp` (requires ≥3.12). The test suite uses WSL2 Python 3.12 automatically.
2. Gazebo Docker build is slow (20–40 min on first run). Subsequent runs use the `gz-sim-linux-build` Docker volume.
3. `geometry_msgs` is a ROS 2 package not on PyPI. The test suite installs a minimal stub into the gazebo-mcp venv so the server can import without a full ROS 2 installation.
4. RobotCAD/CROSS: use **`scripts/install_robotcad_cross.ps1`**, then confirm the workbench and demo in FreeCAD (still pending until manually verified).

**Phase 0 Blockers / Remaining Tasks:**
- [x] **RobotCAD/CROSS import** — install script + FreeCADCmd verify + `check_robotcad()` over RPC.
- [x] **RobotCAD automated URDF export** — FreeCADCmd batch (`scripts/export_arm_2dof_fcstd.py`).
- [x] **Headless Gazebo end-to-end** — Docker E2E + `scripts/run_gz_sim_fast.sh` / `Start-gz-sim.bat` (first source build still 20–40 min).
- [ ] With FreeCAD + MCP RPC active, manually confirm **`test_all_mcp.py` prints PASS** for `get_object` / `get_view` (optional; automated when RPC up).

**Phase 0 definition-of-done status:** **Met for MVP** — MCP servers, RobotCAD export, and headless Gazebo spawn proven in Docker E2E and live gz-cli path.

## Phase 1: Manual End-to-End

Goal: manually complete the full design-export-simulate path and capture every friction point.

Tasks:

- [x] Pick one toy robot: **2-DOF planar arm** (`arm_2dof`). Simple enough to verify joint frames, inertias, and basic simulation without controller complexity.
- [x] Model the robot in FreeCAD using RobotCAD/CROSS conventions — **`robots/arm_2dof.FCStd`** generated from placeholder URDF (`scripts/build_arm_2dof_fcstd_rpc.py`). Placeholder URDF remains at `robots/arm_2dof.urdf` for Gazebo until export completes.
- [ ] Define links, joints, limits, visuals, collisions, sensors, controllers, and inertias. **Partially done in placeholder URDF** (2 revolute joints, cylinder visual/collision geometry, approximate inertias). Full definition pending RobotCAD.
- [x] Establish the unit and frame convention: FreeCAD mm (internal), generated sim metres, +Z up, REP-103 naming. Documented in `robots/arm_2dof.urdf` header.
- [ ] Assign materials and densities before inertia export. **Pending**: requires FreeCAD + RobotCAD. Placeholder uses approximate cylinder inertia values.
- [ ] Generate simplified collision geometry. **Partial**: placeholder URDF uses primitive cylinders (correct for simple arm). RobotCAD path will need V-HACD for mesh-based collision.
- [x] Export the robot through RobotCAD/CROSS to URDF/SDF and ROS 2 package artifacts. **Done (batch)**: `generated/arm_2dof/arm_2dof_description/.../urdf/arm_2dof.urdf` via FreeCADCmd; validated with `bridge.validate.validate_urdf`.
- [x] Create or select one simple world: `worlds/empty_world.sdf` — ground plane + sun + Bullet physics at 1 ms step.
- [x] Load the exported robot into headless Gazebo. **Met (2026-05-29)** — Docker E2E + `gz_cli_bridge` spawn of nested RobotCAD URDF; live `GAZEBO_SPAWN_VIA_GZ_CLI`.
- [x] Run a short simulation and inspect pose, joint, sensor, contact, and RTF output. **Met (2026-05-29)** — `e2e_smoke` records EE pose + RTF; full joint streams not required for smoke.
- [x] Record issues with units, coordinate frames, etc. — see Phase 1 Friction List below.
- [x] Decide which manual steps must become automation in Phase 2. — see Phase 1 Friction List below.

Deliverables:

- [x] Toy robot URDF placeholder: `robots/arm_2dof.urdf` (hand-crafted; to be replaced by RobotCAD export from `robots/arm_2dof.FCStd`).
- [x] One simple world: `worlds/empty_world.sdf`.
- [x] `.FCStd` source file — `robots/arm_2dof.FCStd` (local; commit optional).
- [x] Generated URDF from RobotCAD — `generated/arm_2dof/arm_2dof_description/arm_2dof_description/urdf/arm_2dof.urdf` (+ meshes, ROS package templates).
- [x] One executed scenario — **`e2e_smoke`** in Docker E2E (automated; replaces manual-only gate for MVP).
- [x] Friction list — see below.

Definition of done:

- A robot designed in FreeCAD runs in headless Gazebo. **Met** via RobotCAD export + Docker E2E / live spawn (manual GUI walkthrough optional).
- The manual process is documented well enough to repeat. **Met** — friction list + `e2e/` + `Start-gz-sim.bat` / compose docs.

### Phase 1 Friction List

These friction points were identified from analysis of the pipeline (to be validated once the full stack runs):

| # | Area | Issue | Phase 2 Automation Target |
|---|---|---|---|
| 1 | Units | FreeCAD uses mm internally; URDF/SDF must be in metres. RobotCAD handles conversion but it must be verified on each export. | `export_urdf` output unit check |
| 2 | Joint frames | FreeCAD body-fixed joint axes must be expressed as world-frame unit vectors in URDF. Easy to get wrong on non-axis-aligned joints. | Post-export joint axis validation |
| 3 | Inertia accuracy | FreeCAD only computes correct inertia tensors if material density is assigned per body. Missing density → zero/garbage inertia → unstable sim. | `compute_inertia` check + materials library |
| 4 | Collision meshes | Exported visual STL meshes are typically too high-poly for physics. Need primitive or V-HACD approximation. | Collision simplification step in export pipeline |
| 5 | Mesh paths | RobotCAD generates relative mesh paths; must be relocatable within the repo layout. Absolute paths break portability. | Path normalization in `export_urdf` |
| 6 | ROS 2 package | RobotCAD generates a full ROS 2 package with launchers. The launcher paths assume a specific workspace layout. | `export_urdf` to normalize package paths |
| 7 | Gazebo lifecycle | `gz sim -s` running in Docker; startup takes 2–5 s after container start. MCP spawn calls before startup → connection error. | Startup health check + retry in handoff helper |
| 8 | Port conflicts | If Gazebo container is restarted without full teardown, gz-transport ports may conflict. | Robust restart path (Phase 6) |
| 9 | URDF → SDF | Gazebo can accept URDF, but SDF is preferred for Gazebo-specific features (sensors, plugins). Check which format gazebo-mcp expects. | `spawn_model` to accept both URDF and SDF |

### Phase 1 Gazebo Validation Commands (once Gazebo Docker is running)

```bash
# From WSL, after Start-gz-sim.bat has built and started the container:

# Validate URDF (requires check_urdf from urdfdom):
check_urdf /mnt/c/Users/Rchie/Music/FreeCAD/robots/arm_2dof.urdf

# Load world headlessly:
gz sim -s /mnt/c/Users/Rchie/Music/FreeCAD/worlds/empty_world.sdf &

# Spawn the arm (Harmonic / gz-sim 8 — use the world's create service, not ``gz model --spawn-file``):
gz service -s /world/empty_world/create \
  --reqtype gz.msgs.EntityFactory --reptype gz.msgs.Boolean --timeout 15000 \
  --req 'name: "arm_2dof", sdf_filename: "/mnt/c/Users/Rchie/Music/FreeCAD/robots/arm_2dof.urdf"'

# Check pose:
gz model -m arm_2dof -p
```

```python
# Via gazebo-mcp (MCP client):
# Load world:
gazebo_spawn_world(sdf_path="worlds/empty_world.sdf")
# Spawn arm:
gazebo_spawn_model(model_name="arm_2dof", urdf_path="robots/arm_2dof.urdf",
                   pose={"position": {"x": 0, "y": 0, "z": 0}})
# Get state:
gazebo_get_model_state(model_name="arm_2dof")
```

## Phase 2: Automated Bridge

Goal: automate the handoff between FreeCAD source files and headless Gazebo.

Tasks:

- [x] Define the initial project repo layout — done in Phase 0; `project.yaml` exists.
- [x] Draft the `project.yaml` schema — `config/schemas/project.schema.yaml` (JSON Schema / YAML syntax).
- [x] Draft the scenario YAML schema — `config/schemas/scenario.schema.yaml`.
- [x] Add `export_urdf(robot_name, out_dir)` — `bridge/freecad_bridge.py`; calls FreeCAD via XML-RPC `execute_code()` with RobotCAD Python API. Parses inner snippet `success` from RPC `Output:` (not only outer RPC `success`). Fails cleanly with blocker message when RobotCAD not installed.
- [x] Add `export_sdf_world(world_name, out_dir)` — `bridge/freecad_bridge.py`; validates + stages SDF to generated/. Works now (hand-crafted SDFs).
- [x] Add `compute_inertia_check(robot_name)` and material checks — `bridge/freecad_bridge.py`; inspects FreeCAD document for density assignments.
- [ ] Add collision simplification checks. **DEFERRED**: requires FreeCAD + RobotCAD + mesh analysis. Will add in Phase 3 export pipeline.
- [x] Implement a handoff helper — `bridge/handoff.py`; `export_and_spawn()` orchestrates validate→stage world→wait for Gazebo→spawn in 5 steps. Short-circuits cleanly at each blocker. **`resolve_robot_urdf()`** prefers RobotCAD export at `generated/<robot>/.../urdf/<robot>.urdf` before `robots/<robot>.urdf`; exports from FCStd only when generated URDF is missing and `skip_freecad_export=False`.
- [ ] Add export caching keyed by document hash. **DEFERRED**: premature until RobotCAD export works end-to-end.
- [x] Normalize mesh paths — `bridge/validate.py` `validate_urdf()` detects absolute mesh paths (friction point #5).
- [x] Keep MCP tools coarse-grained — bridge module uses single-call API; LLM agents call `export_and_spawn()` not individual low-level steps.
- [x] Write smoke tests — `tests/test_bridge.py`: **46+** offline tests; live tests behind `RUN_GAZEBO_LIVE=1`.

Deliverables:

- [x] Project manifest schema: `config/schemas/project.schema.yaml`
- [x] Scenario schema: `config/schemas/scenario.schema.yaml`
- [x] Bridge Python package: `bridge/` (project.py, validate.py, freecad_bridge.py, gazebo_bridge.py, handoff.py)
- [x] FreeCAD-to-Gazebo handoff helper: `bridge/handoff.export_and_spawn()`
- [x] Smoke tests: `tests/test_bridge.py` — 30 passed, 6 skipped (live)
- [x] pytest.ini with custom marks (freecad, gazebo)

Definition of done:

- An MCP client can export from FreeCAD and spawn into Gazebo without manual file copying. **Met (2026-05-29)** — `export_urdf_cmd`, `handoff.export_and_spawn`, Docker E2E, live gz-cli spawn.

### Phase 2 Notes

- **Design decision**: Bridge module communicates with FreeCAD via direct XML-RPC (same protocol as freecad-mcp client) rather than going through the MCP stdio layer. This is simpler and faster for Python-to-Python calls.
- **Design decision**: Gazebo bridge uses a subprocess MCPClientStdio session per call (not a persistent daemon). Acceptable overhead for Phase 2; Phase 6 can optimise with a persistent connection.
- **Design decision**: No modifications to the upstream MCP server submodules (`tools/mcp/freecad-mcp`, `tools/mcp/gazebo-mcp`). The bridge layer sits above them and calls through their existing APIs. This keeps the submodules cleanly updateable.
- **PyYAML dependency**: Added to Windows Python environment (pip install pyyaml). Not yet in a requirements file — add `requirements-bridge.txt` in Phase 3.
- **RobotCAD/CROSS (2026-05-29)**: **`export_urdf_cmd()`** / `scripts/robotcad_headless.py` export via **FreeCADCmd** (preferred). `export_urdf(prefer_cmd=True)` uses batch first, MCP RPC second. Headless patches: `export_urdf` `ignore` bug, overwrite dialog, `MOD_PATH`, git/controllers skip.
- **`execute_code` layering (2026-05-29)**: FreeCAD RPC returns `success=True` when Python ran without exception; RobotCAD snippets print `repr({success, message, ...})` to stdout. `bridge/freecad_bridge.py` parses that **inner** dict from the `Output:` section so `export_urdf` / `check_robotcad` / `compute_inertia_check` report `ok=False` when CROSS is missing even though the RPC call succeeded.

## Phase 3: Simulation Workbench Viewer

Goal: make FreeCAD the simulation viewer and operator cockpit.

Tasks:

- [x] Create the FreeCAD Simulation Workbench addon skeleton (`addons/SimWorkbench/`).
- [x] Build a shared Gazebo/ROS 2 transport layer (`transport.py`) used by both the workbench and MCP tooling.
- [x] Implement Gazebo process lifecycle controls: start, pause, resume, step, reset (via SimWorkbenchCoordinator → bridge.gazebo_bridge).
- [x] Implement the Live State Bridge (`state_bridge.py`):
  - polls Gazebo pose and joint-state via GazeboTransport at 10 Hz,
  - translates simulation state (metres + quaternion) into FreeCAD Placements (mm + RPY),
  - updates FreeCAD via Qt timer (GUI-safe), no threads in v1.
- [x] Add Sim Controls panel (`panels/sim_controls.py`) — play, pause, step, reset, sim time, RTF readout, connection status indicator.
- [x] Add Scenario Picker panel (`panels/scenario_picker.py`) — robot/world/scenario selection with combo boxes.
- [ ] Add Camera Viewer panel — deferred: requires live ROS 2 image topic; blocked by Gazebo Docker.
- [x] Add Gazebo Status/Screenshot panel — `addons/SimWorkbench/panels/gazebo_status.py`: Gazebo status via **`gazebo_get_simulation_status`**, MCP service lines (**`bridge/mcp_status.py`**), screenshot placeholder until a camera topic exists; **not** an embedded Gazebo GUI viewport.
- [x] Add Sensor Plots panel (`panels/sensor_plots.py`) — joint position/velocity/effort table, RTF display.
- [x] Add Run Library panel (`panels/run_library.py`) — browses sim_runs/, shows pass/fail status.
- [ ] Add Project Browser panel — deferred to Phase 4 (overlaps with Test Runner UI).
- [x] Add MCP Activity Log panel (`panels/mcp_log.py`) — scrolling audit log of agent tool calls.
- [ ] Verify the human can run and watch a simulation in FreeCAD without opening Gazebo GUI — **open (post-MVP polish)** — transport + workbench exist; needs one host smoke with live Gazebo (Phase 6 blocker #5).
- [x] Tests: `tests/test_sim_workbench.py` — 22 tests, all offline; 52 total tests pass.

Deliverables:

- [x] Simulation Workbench addon (`addons/SimWorkbench/`).
- [x] Live State Bridge (`transport.py` + `state_bridge.py`).
- [x] Basic controls and viewer panels (Sim Controls, Scenario Picker, Sensor Plots, Run Library, MCP Log).
- [x] Shared Gazebo transport library (`transport.py`).
- [ ] Camera Viewer — deferred.
- [x] Gazebo Status/Screenshot panel — status readout + MCP reachability + snapshot placeholder; no embedded Gazebo viewport.
- [ ] Project Browser — deferred.
- [x] Addon install helper (`install_addon.py`).

Definition of done:

- A human can open FreeCAD, switch to the Simulation Workbench, press play, and watch the simulated robot move in FreeCAD's 3D view.
- **Partially met**: Addon + state bridge **offline-tested**; Gazebo **live** on host/Docker — human play-button smoke not recorded this pass.

### Phase 3 Notes

- **Design decision**: Workbench communicates with Gazebo via `bridge.gazebo_bridge` (same bridge module as the handoff pipeline). No separate ROS 2 Python bindings needed on Windows — all ROS 2 interaction goes through WSL2 via subprocess MCP session.
- **Design decision**: Transport uses a QTimer (10 Hz) rather than a background thread. This avoids threading bugs in FreeCAD's Qt event loop. 10 Hz is sufficient for visual feedback; bump to 30 Hz if needed.
- **Design decision**: FreeCAD Placements are updated directly (not via a FreeCAD feature/document recompute). This is the fastest path for live animation; it does not create an undo history entry.
- **Design decision**: State-to-placement scale = 1000 (Gazebo metres → FreeCAD mm). Configurable via `StateBridge(scale=...)`.
- **Design decision**: A Gazebo Status/Screenshot panel may show periodic screenshots or rendered camera snapshots from Gazebo, but Gazebo remains headless and the panel is not a real embedded Gazebo GUI.
- **Follow-up**: Record workbench play + watch smoke on Windows with `Start-gz-sim.bat` or fast stack (closes human DoD).
- **Blocker**: Camera Viewer blocked by live ROS 2 image topics not available. Deferred.
- **Installation**: Run `python addons/SimWorkbench/install_addon.py` to install into FreeCAD's Mod directory, then restart FreeCAD.
- **Commit**: `phase 3: simulation workbench addon`

## Phase 4: Test Runner

Goal: turn simulation into repeatable regression tests.

Tasks:

- [x] Finalize the v1 scenario YAML schema (`config/schemas/scenario.schema.yaml`, loader in `runner/scenario.py`).
- [x] Finalize the v1 assertion vocabulary (7 types, all implemented in `runner/assertions.py`).
- [x] Start with fixed assertions:
  - [x] `reach_target_within`
  - [x] `no_self_collision`
  - [x] `max_joint_torque_below`
  - [x] `sim_time_under`
  - [x] `pose_within_tolerance`
  - [x] `rtf_above`
  - [x] `collision_count_below`
- [x] Implement scenario loading from `tests/scenarios/` (`runner/scenario.load_scenario()`).
- [x] Implement single-test execution (`runner/runner.run_test(name)`).
- [x] Implement run-all execution (`runner/runner.run_all_tests()`).
- [x] Evaluate assertions from recorded telemetry (`runner/assertions.evaluate_all()`).
- [x] Write `sim_runs/<timestamp>_<scenario>/result.yaml` (`runner/result.write_result()`).
- [x] Include input hashes (scenario YAML SHA-256, robot URDF SHA-256, world SDF SHA-256), tool versions, in each result.
- [x] Record telemetry: joint states, EE poses, contacts, RTF (`runner/executor.py`).
- [x] Add pass/fail Test Runner panel to Simulation Workbench (`addons/SimWorkbench/panels/test_runner_panel.py`).
- [x] `list_tests()` and `run_test(name)` available via `runner.runner` — callable from FreeCAD `execute_code()`.
- [x] Regression tests for scenario parsing and assertion evaluation (`tests/test_runner.py` — 44 tests).

Deliverables:

- [x] Scenario runner (`runner/runner.py`).
- [x] Assertion evaluator (`runner/assertions.py`).
- [x] Result writer (`runner/result.py` — YAML with hashes/versions).
- [x] Workbench Test Runner panel (`addons/SimWorkbench/panels/test_runner_panel.py`).
- [x] `list_tests` / `run_test` accessible via `execute_code` MCP surface.
- [ ] CLI entry point: `python -m runner.runner list/run/run-all` ✓ (coded, not separately tested live).

Definition of done:

- A robot design can be regression-tested through repeatable scenarios, and results are visible in FreeCAD and available to the MCP client.
- **Met**: Offline mock bridge + **Docker E2E** live `gz_cli_bridge` + `sim_runs/` results.

### Phase 4 Notes

- **Design decision**: `runner/` is a standalone Python package; it does NOT require FreeCAD to be installed. The `execute_code` hook in the FreeCAD MCP server is the only coupling point.
- **Design decision**: `run_test()` accepts a `bridge_module` parameter, making it fully unit-testable with mock Gazebo state.
- **Design decision**: result.yaml includes `input_hashes` (SHA-256 of scenario YAML, robot URDF, world SDF). No random seeds in v1 since Gazebo uses deterministic physics by default.
- **Test count**: **172+** offline focused tests pass (2026-05-29); live tests skip unless `RUN_GAZEBO_LIVE=1`.
- **Commit**: `phase 4: test runner, assertion evaluator, result writer`

## Phase 4.5: ROS 2 Control and Telemetry

Goal: support controller-in-the-loop tests without bloating the v1 runner.

Tasks:

- [ ] Choose the initial ROS 2 MCP bridge: minimal command publisher, rosbridge-based server, LCAS, WiseVision, or another candidate.
- [ ] Add topic/action discovery for the RobotCAD-generated ROS 2 package.
- [ ] Add controlled publishing for `/cmd_vel`, joint commands, or action goals.
- [ ] Add read-only tools for topics, services, actions, node graph, and message schemas.
- [ ] Support Nav2 or ros2_control only after simple command publishing works.
- [ ] Record robot-perceived telemetry separately from Gazebo ground truth.
- [ ] Add VLM/image retrieval path only after camera topics are stable.

Deliverables:

- [ ] ROS 2 MCP selection note.
- [ ] Controller-in-the-loop scenario example.
- [ ] Telemetry capture format.

Definition of done:

- One scenario can command the robot through ROS 2 control interfaces and evaluate both ground-truth and robot-perceived telemetry.

## Phase 5: Iteration Loops

Goal: support agent-driven design changes and repeated simulation checks.

Tasks:

- [x] Add a stable `set_parameter(doc, name, value)` flow for controlled FreeCAD edits (`iteration/parameter.py`).
- [x] Add bounded edit policies for dimensions, materials, controller settings, and scenario inputs (`iteration/policy.py`).
- [x] Add a loop that changes a parameter, recomputes, exports, runs a scenario, and reports results (`iteration/loop.py`).
- [x] Add failure summarization that points back to the relevant scenario assertion and metric (`iteration/report.summarize_failure()`).
- [x] Add parameter sweep support for numeric design variables (`iteration/sweep.py`, `SweepRunner.linspace/arange/sweep()`).
- [x] Add result comparison and diff report for LLM review (`iteration/report.compare_results()`, `diff_results()`).
- [ ] Capture screenshots, plots, selected sensor summaries — deferred to Phase 6 (requires live Gazebo).
- [ ] Decide when design changes should be automatically committed — deferred to Phase 6 hardening decision.
- [x] Avoid sim-state-to-CAD edits in v1; keep design-to-sim as the stable direction — enforced by Policy (one-directional).

Deliverables:

- [x] Parameter iteration flow (`iteration/loop.py` — `IterationLoop.run_once()` / `sweep()`).
- [x] Bounded edit policy (`iteration/policy.py` — `Policy`, `ParameterRule`, `DEFAULT_ARM_2DOF_POLICY`).
- [x] Parameter get/set (`iteration/parameter.py` — `get_parameter()`, `set_parameter()` via XML-RPC code gen).
- [x] Optional parameter sweep runner (`iteration/sweep.py` — `SweepRunner`, `linspace()`, `arange()`).
- [x] Result comparison report (`iteration/report.py` — `compare_results()`, `diff_results()`, `summarize_failure()`).
- [x] Offline tests: `tests/test_iteration.py` — 55 tests, all pass.
- [ ] Sensor and result summaries with screenshots — deferred (needs live Gazebo).

Definition of done:

- An LLM can make a bounded design change, rerun a failing scenario, and report whether the change improved the result.
- **Met (offline)**: mock bridge + iteration tests. **Live export+sim**: via Docker E2E / handoff when stack up; export cache still deferred.

### Phase 5 Notes

- **Design decision**: `set_parameter` generates Python code strings and sends them via XML-RPC `execute_code`. Supports two modes: (a) spreadsheet alias lookup (scan all Spreadsheet::Sheet objects) and (b) dot notation `"Object.Property"` for direct property access.
- **Design decision**: `Policy.check_all(dict)` + `Policy.clamp_all(dict)` / `Policy.snap_all(dict)` are the primary dict-based interfaces. Single-param `check/clamp/snap(name, value)` are also public.
- **Design decision**: `IterationLoop._export_urdf()` failure is non-fatal; loop continues with existing URDF on disk. This allows offline testing without RobotCAD/CROSS installed.
- **Design decision**: `IterationLoop._set_params()` returns an error string (not raises); `run_once()` wraps all failure modes in `IterationResult.error` — no exception propagates to the LLM caller.
- **DEFAULT_ARM_2DOF_POLICY**: 4 rules (link1_length 0.1–0.8 m, link2_length 0.1–0.6 m, link1_mass 0.1–5.0 kg, link2_mass 0.1–3.0 kg), step=0.05 m / 0.1 kg.
- **pytest.ini updated**: Added `testpaths = tests` to prevent pytest from walking into submodules (tools/mcp/gazebo-mcp/scripts etc.) and collecting foreign test files.
- **Test count**: 151 passed, 6 skipped (live) across all test files.
- **Commit**: `phase 5: iteration loops, parameter policies, sweep runner`


## Phase 6: Hardening

Goal: make the system trustworthy enough for repeated use.

Tasks:

- [x] Pin FreeCAD, RobotCAD/CROSS, ROS 2, Gazebo, Python, Docker, and MCP server versions. **Partial (2026-05-29):** Docker E2E base `@sha256`, apt `dpkg` pins, strict `record_runtime_versions.py`; host/WSL/MCP-submodule gaps remain (see [Remaining version-pinning gaps](#remaining-version-pinning-gaps-phase-6)).
- [x] Add input hashes to generated outputs and simulation results. **Done (2026-05-29):** scenario + URDF/SDF + **FCStd** in ``input_hashes`` / ``metadata.source_hashes``; see [Export cache status](#export-cache-status-2026-05-29).
- [x] Separate read-only and write-capable MCP tools. **Done (2026-05-29):** registry in `bridge/permissions.py`; matrix in [permissions-and-write-surface.md](permissions-and-write-surface.md).
- [x] Add permission prompts or policy controls for write operations. **Partial:** `BRIDGE_WRITE_POLICY` + `CI` → `generated_only`; interactive MCP prompts **not** implemented.
- [x] Enforce typed schemas for MCP tools, scenario YAML, project manifests, and result files. **Partial:** project/scenario/result YAML via `bridge/schema_validate.py` + `jsonschema`; MCP tool input schemas **not** wired.
- [x] Improve Gazebo restart behavior to avoid stale processes, port conflicts, and ROS 2 daemon issues. **Partial (2026-05-29):** `Stop-gz-stack.bat`, `ensure_gz_sim_headless.sh`, `restart_ros_gz_bridge.sh`, normalized world/containers — [gazebo-lifecycle.md](gazebo-lifecycle.md).
- [x] Add structured logging across FreeCAD workbench actions, MCP calls, exports, ROS 2 interactions, and sim runs. **Partial (2026-05-29):** ``bridge/run_context.py`` → ``sim_runs/<run_id>/run.log`` + ``run_events.yaml``; wired in runner, handoff, export, spawn, executor, E2E; SimWorkbench configures logging; ROS MCP **not** wired.
- [x] Add collision mesh simplification for Gazebo spawn. **Partial (2026-05-29):** `prepare_urdf_for_gazebo` replaces `col_end_effector` collision mesh with sphere; V-HACD / other links still open.
- [ ] Add materials and density management for accurate inertias.
- [ ] Add physics-engine and step-size recording for Gazebo runs.
- [x] Add CI-friendly headless test execution. **Done (2026-05-29):** `robot-sim-ci.yml` — offline pytest, lifecycle smoke, strict Docker E2E; local `scripts/ci/run_local.sh`.
- [ ] Add multi-robot and controller bring-up support only after the single-robot path is stable.

Deliverables:

- [x] Version-pinned runtime. **Partial** — Docker E2E + PyPI; host stack documented in Phase 0 table only.
- [x] Reproducible result metadata. **Partial** — ``result.yaml`` ``metadata`` block (policy, paths, lifecycle, E2E env); see [Logging and metadata status](#logging-and-metadata-status-2026-05-29).
- [x] Permission model. **Partial** — policy layer + docs; no UI prompt.
- [ ] Robust restart path.
- [x] CI-ready test command. **Done** — `bash scripts/ci/run_local.sh` / per-gate scripts; see [ci.md](ci.md).

Definition of done:

- Tests can be rerun reliably and produce explainable, comparable results.

### Permission and schema status (2026-05-29)

| Item | Status | Notes |
| --- | --- | --- |
| Read vs write registry | **Done** | `WriteOperation` enum + `docs/permissions-and-write-surface.md` |
| Write policy | **Done** | `BRIDGE_WRITE_POLICY=allow\|deny\|generated_only`; CI → `generated_only` |
| Guards on bridge/runner writes | **Done** | `export_urdf`, `export_sdf_world`, `spawn_model`, sim control, `write_result`, temp URDF |
| Interactive MCP prompts | **Open** | Use `deny` in untrusted clients |
| `project.schema.yaml` | **Enforced** | `load_project()` |
| `scenario.schema.yaml` | **Enforced** | `load_scenario()` (+ dataclass checks) |
| `result.schema.yaml` | **Enforced** | `write_result()` |
| MCP tool JSON schemas | **Open** | gazebo-mcp / freecad-mcp tool defs not validated in-repo |
| Offline tests | **Done** | `tests/test_permissions.py`, `tests/test_schema_validation.py` |

### Logging and metadata status (2026-05-29)

| Item | Status | Notes |
| --- | --- | --- |
| Per-run log file | **Done** | `sim_runs/<run_id>/run.log` via `bridge/run_context.py` |
| Structured events | **Done** | `sim_runs/<run_id>/run_events.yaml` (lifecycle, handoff, export, log mirror) |
| `result.yaml` metadata | **Done** | `write_policy`, `paths`, `lifecycle_events`, `file_hashes`, `e2e_run_dir` |
| Input hashes | **Done** | scenario + URDF/SDF + `fcstd` + metadata file hashes |
| Runner / executor | **Done** | `begin_run` / `finalize_run` in `run_test` |
| Handoff / export / spawn | **Done** | `handoff`, `freecad_bridge`, `gazebo_bridge`, `gz_cli_bridge` |
| Docker E2E | **Done** | `SIM_RUNS_DIR` → `E2E_RUN_DIR`; scenario subdirs under e2e folder |
| SimWorkbench | **Partial** | `configure_logging()` on test run; panel actions **not** fully instrumented |
| ROS MCP / physics step | **Open** | not in v1 run context |

### Export cache status (2026-05-29)

| Item | Status | Notes |
| --- | --- | --- |
| FCStd document hash | **Done** | SHA-256 via `bridge/export_cache.py` + `bridge/runtime_versions.py` |
| Policy/version cache key | **Done** | RobotCAD commit, CROSS version, cache schema, exporter id |
| Cache storage | **Done** | `generated/<robot>/.export_cache/entries/<key>/` |
| `export_urdf_cmd` / `export_urdf` | **Done** | restore before FreeCADCmd; store after export |
| `resolve_robot_urdf` / handoff | **Done** | cache restore before `needs_export` |
| Structured `export_cache` events | **Done** | `cache_hit`, `cache_miss`, `cache_store`, `cache_invalidated` |
| `metadata.source_hashes.fcstd` | **Done** | in `result.yaml` via run context |
| Env controls | **Done** | `BRIDGE_EXPORT_CACHE`, `BRIDGE_EXPORT_CACHE_INVALIDATE` |
| Offline tests | **Done** | `tests/test_export_cache.py` |
| E2E `stage_export.sh` | **Unchanged** | still `rm -rf generated` for strict clean export (cache repopulates on next host run) |

See [export-cache.md](export-cache.md).

## Phase 7: Scale-Out Validation

Goal: move from one-off scenarios to statistically useful simulation campaigns.

Tasks:

- [ ] Add randomized scenario seeds for obstacle positions, terrain, lighting, friction, and initial poses.
- [ ] Add aggregate metrics:
  - Real-Time Factor (RTF)
  - Mean Time to Traverse (MTT)
  - Total Collisions (TC)
  - Velocity Over Rough Terrain (VORT)
  - Success Rate (SR)
- [ ] Add batch execution across many worlds or generated variants.
- [ ] Add summary dashboards for scenario families.
- [ ] Add a small "100-room" validation suite before considering larger randomized campaigns.
- [ ] Treat energy, adaptive communication, DDS QoS, and network degradation tests as advanced diagnostics, not v1 requirements.

Deliverables:

- [ ] Batch scenario runner.
- [ ] Metrics summary files.
- [ ] Randomized environment suite.
- [ ] Dashboard or report view.

Definition of done:

- The same robot can be evaluated across many reproducible scenario variants with comparable metrics.

## Decisions Needed Before Coding

- [x] Is the first target single-user local development, team development, or CI automation? **Decision: single-user local development first.** CI automation is Phase 6.
- [x] Which environment is the first supported path: Docker, WSL2, native Linux, or Windows-native? **Decision: Windows-native FreeCAD + WSL2 + Docker for Gazebo/ROS 2.**
- [ ] Which Gazebo release is the target: Harmonic, Ionic, or another modern gz-sim release? **Pending**: the `Start-gz-sim.sh` builds from `src/3rdParty/gz-sim` source; the exact release tag needs to be confirmed from the submodule.
- [ ] Are v1 tests kinematic only, controller-in-the-loop, or both? **Pending**: defer to Phase 4 decision point.
- [ ] What is the minimum useful v1 assertion set? **Pending**: defer to Phase 4 decision point. Candidates: `reach_target_within`, `no_self_collision`, `max_joint_torque_below`, `sim_time_under`, `pose_within_tolerance`, `rtf_above`.
- [ ] Which ROS 2 MCP bridge is the first supported bridge? **Working assumption**: `ros-mcp` v3.0.1 (already installed at `tools/mcp/ros-mcp-server`). Revisit in Phase 4.5.
- [x] Will Gazebo always run on the same machine as FreeCAD? **Decision: yes for v1.** Both on the same Windows host (Gazebo in WSL2/Docker, FreeCAD on Windows).
- [ ] Are generated artifacts ignored, checked in, or stored through Git LFS? **Working assumption**: `generated/` and `sim_runs/` are gitignored. Finalize in Phase 2 when the repo layout is established.
- [x] What write operations should the MCP client be allowed to perform? **Decision (2026-05-29):** documented write surface; default `allow` locally, `generated_only` in CI; see [permissions-and-write-surface.md](permissions-and-write-surface.md).
- [x] Which FreeCAD MCP transport and port are canonical for this project? **Decision: XML-RPC on `localhost:9875`.** Confirmed from `freecad_client.py` and `rpc_server.py`.

## Immediate Next Tasks (post-MVP → Phase 6)

1. ~~Fix **ODE trimesh** on `col_end_effector_.dae`~~ — **done** (spawn-time sphere collision; see [Collision / mesh policy](#collision--mesh-policy-gazebo-spawn)).
2. ~~**Commit or CI-fetch** `robots/arm_2dof.FCStd`~~ — **done** (see [Reproducibility status](#reproducibility-status-2026-05-29)).
3. ~~**Pin versions** + `requirements-bridge.txt`~~ — **done (Docker E2E)** — digest + apt + strict gate; see [docker-e2e-reproducibility.md](docker-e2e-reproducibility.md).
4. **Workbench live smoke** — play in SimWorkbench with live Gazebo on host.
5. Draft **`reach_top_shelf.yaml`** (or next real scenario beyond `e2e_smoke`).
6. ~~Wire **GitHub Actions**~~ — **done** — [`.github/workflows/robot-sim-ci.yml`](../.github/workflows/robot-sim-ci.yml).
7. ~~Optional: export cache keyed by FCStd hash~~ — **done** — [export-cache.md](export-cache.md).

### RobotCAD / `arm_2dof` export — verified vs remaining (2026-05-29)

| Item | Status |
| --- | --- |
| `scripts/install_robotcad_cross.ps1` | **OK** — clone, pip deps, submodule init, pixi `freecad.robotcad` junction |
| `scripts/verify_robotcad_cross.py` (FreeCADCmd) | **OK** — `freecad.cross` v1.0.1 |
| `check_robotcad()` (RPC) | **OK** when FreeCAD + :9875 |
| `robots/arm_2dof.FCStd` | **Tracked** — 73 883 B, SHA-256 in `config/runtime-versions.lock.yaml` |
| `scripts/build_arm_2dof_fcstd_rpc.py` | **OK** — requires GUI FreeCAD + RPC |
| `export_urdf_cmd` / `scripts/export_arm_2dof_fcstd.py` | **OK** — FreeCADCmd batch (~6–15 s locally) |
| `generated/.../urdf/arm_2dof.urdf` | **OK** — `validate_urdf` passes |
| `bridge/handoff.resolve_robot_urdf` / `export_and_spawn(skip_freecad_export=True)` | **OK (offline)** — resolves `arm_2dof\arm_2dof_description\...\urdf\arm_2dof.urdf`; validate + stage_world pass |
| `scripts/ensure_gazebo_mcp_venv.sh` | **OK** — recreates WSL `.venv` when entry-point shebang is stale |
| `scripts/ensure_ros_gz_bridge.sh` / `Start-gazebo-bridge.bat` | **OK** — `ros-gz-bridge` sidecar on `gz-sim-sever` network, world `empty` |
| Live Gazebo handoff (`gazebo_ready`, real `gazebo_connected`) | **OK (2026-05-29)** — with `Start-gz-sim.bat` + `Start-gazebo-bridge.bat`, `GAZEBO_MCP_DOCKER=1`, `GAZEBO_WORLD_NAME=empty`; `RUN_GAZEBO_LIVE=1 pytest …::test_full_handoff` validates RobotCAD URDF resolve + real sim status |
| Live spawn (`spawn_model` / full handoff spawn step) | **OK (2026-05-29)** — `GAZEBO_SPAWN_VIA_GZ_CLI=1` (default with `GAZEBO_MCP_DOCKER=1`) uses `docker exec gz-sim-sever gz service /world/empty/create` with `sdf_filename`; RobotCAD package bind-mount `generated/.../arm_2dof_description` → `/models/arm_2dof_description`; `bridge/urdf_for_gazebo.py` rewrites `package://` → `file:///models/...`; `scripts/run_gz_sim_fast.sh` + `scripts/restart_ros_gz_bridge.sh` for headless `empty` world. **Note:** ros_gz `parameter_bridge` service calls still hang on this host; MCP status/unpause may use ROS path while spawn uses gz CLI. |
| `export_urdf` MCP fallback | **Optional** — may hit MCP **120 s** timeout; use `prefer_cmd=False` only when GUI+RPC needed |
| Docker E2E (`compose.e2e.yml`, strict) | **PASS exit 0 (2026-05-29)** — RobotCAD nested URDF + `package://` meshes + `/models` mount + `e2e_smoke` 4/4 + gz_cli spawn |
| Docker E2E placeholder fallback | **Separate** — `run_e2e_placeholder_fallback.sh` / profile `fallback`; not used by main acceptance path |

## Tracking Notes

- Keep the original plan as the architecture narrative.
- Use this file as the working task checklist.
- Promote repeated friction from Phase 1 into explicit Phase 2 implementation tasks.
- Keep generated artifacts and simulation run outputs reproducible from source inputs.
- Avoid expanding the assertion language too early; a small fixed vocabulary is easier to trust and debug.
- Keep the human path inside FreeCAD and the LLM path through MCP.
- Prefer coarse, typed MCP tools over many tiny tool calls.
- Treat ROS 2 control, VLM/image pipelines, randomized environments, and adaptive communication diagnostics as staged additions after the basic test rig works.
