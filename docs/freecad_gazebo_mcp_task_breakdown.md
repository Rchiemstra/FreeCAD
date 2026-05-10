# FreeCAD Gazebo MCP Task Breakdown

Source docs:

- [freecad_gazebo_mcp_plan.md](freecad_gazebo_mcp_plan.md)
- [FreeCAD Model Simulation Pipeline Integration.md](FreeCAD%20Model%20Simulation%20Pipeline%20Integration.md)
- [diagrams/freecad_gazebo_mcp_component_architecture.puml](diagrams/freecad_gazebo_mcp_component_architecture.puml)
- [diagrams/freecad_gazebo_mcp_deployment_docker.puml](diagrams/freecad_gazebo_mcp_deployment_docker.puml)
- [diagrams/freecad_gazebo_mcp_mcp_tool_surface.puml](diagrams/freecad_gazebo_mcp_mcp_tool_surface.puml)
- [diagrams/freecad_gazebo_mcp_sequence_human_in_workbench.puml](diagrams/freecad_gazebo_mcp_sequence_human_in_workbench.puml)
- [diagrams/freecad_gazebo_mcp_sequence_iteration_loop.puml](diagrams/freecad_gazebo_mcp_sequence_iteration_loop.puml)

Last reviewed: 2026-05-10
Last updated: 2026-05-11 (Gazebo status/screenshot FreeCAD panel task)

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

## Docker E2E (no-human acceptance)

Implements § *Dockerized No-Human E2E Validation Plan* in [FreeCAD Model Simulation Pipeline Integration.md](FreeCAD%20Model%20Simulation%20Pipeline%20Integration.md).

### Target command (Linux Docker engine)

```powershell
docker compose -f docker/compose.e2e.yml up --build --abort-on-container-exit --exit-code-from e2e
```

### What is implemented

| Stage | Implementation |
| --- | --- |
| Base image | `docker/Dockerfile.e2e` — `ros:jazzy-ros-base-noble` + **Gazebo Harmonic** (OSRF apt) + **freecad-daily** PPA + **RobotCAD** clone at `/root/.local/share/FreeCAD/Mod/RobotCAD` → `/opt/robotcad` |
| MCP venv | `/opt/mcp-venv` core deps; **runtime** `pip install -e` on bind-mounted `tools/mcp/{gazebo-mcp,freecad-mcp,ros-mcp-server}` |
| Driver script | `e2e/run_e2e.sh` — versions, `${FREECAD_CMD}` RobotCAD import (`e2e/check_robotcad_freecad.py`), `e2e/stage_export.sh`, `check_urdf`, Xvfb, background **`gz sim -s worlds/empty_world.sdf`**, **`e2e/mcp_smoke.py`**, **`python -m runner.runner run-all --dir tests/scenarios_e2e`** |
| Runner ↔ Gazebo | **`bridge/gz_cli_bridge`** via **`E2E_BRIDGE_MODULE=gz_cli`** — spawn uses **`gz service -s /world/<world>/create`** with **`gz.msgs.EntityFactory`** / **`sdf_filename`** (URDF path); world defaults to **`GZ_SIM_WORLD_NAME=empty_world`**. MCP **`gazebo_spawn_model`** still does not forward **`model_xml`** today. |
| MCP smoke | **`e2e/mcp_smoke.py`** — reuses **`test_all_mcp.MCPClientStdio`** against all three servers |
| Scenario | **`tests/scenarios_e2e/e2e_smoke.yaml`** — lightweight assertions (no joint torque / reach goals) |

### Engineering choices / findings

- **Single container** first — matches the integration doc and avoids cross-container ROS DDS setup for v1.
- **`bridge/gazebo_bridge.py`**: native Linux uses local `.venv/bin/gazebo-mcp-server` with package `cwd`; Windows keeps **WSL** launch; fixed MCP tool name **`gazebo_get_simulation_status`** in `wait_for_ready`.
- **`runner/executor.py`**: live bridge loads **`gz_cli_bridge`** only when **`E2E_BRIDGE_MODULE`** is enabled; default **`None`** preserves mock-injected unit tests. **`_urdf_path`** prefers **`generated/<robot>/<robot>.urdf`** when staged/exported, otherwise **`robots/<robot>.urdf`**.
- **`SIM_RUNS_DIR`** env wired through **`runner.runner` CLI** for `run` / `run-all`.
- **Export honesty**: without **`robots/arm_2dof.FCStd`**, staging copies the checked-in URDF after RobotCAD **import** succeeds — full mesh-export-through-RobotCAD remains blocked until an FCStd is committed.
- **Shell scripts for Linux**: **`e2e/*.sh`** checked in as **LF** (`.gitattributes`); **CRLF** breaks shebang execution when Compose runs `bash -lc /workspace/e2e/run_e2e.sh`.
- **ROS environment**: **`set -u`** before **`source /opt/ros/jazzy/setup.bash`** trips on unset `AMENT_*` variables — bracket the source with **`set +u`** / **`set -u`**.
- **URDF spawn on gz-sim 8**: **`gz model --spawn-file`** is not available; use **`gz service -s /world/<world>/create`** with **`gz.msgs.EntityFactory`** and **`sdf_filename`** (Harmonic tutorial). Pre-delete uses **`/world/<world>/remove`**; “entity not found” on first run is benign.

### Blockers / next steps

- [ ] Add **`robots/arm_2dof.FCStd`** with `Cross::*` entities so `e2e/export_robotcad_fcstd.py` runs a real exporter path.
- [ ] Optionally extend **gazebo-mcp** `spawn_model_wrapper` to pass **`model_xml`** through to `model_management.spawn_model`.
- **Agent validation**: `python -m pytest tests` → **151 passed**, **6 skipped** (2026-05-10). **`docker compose -f docker/compose.e2e.yml up --abort-on-container-exit --exit-code-from e2e`** exercised end-to-end on **Docker Desktop Linux engine** after fixing CRLF shebangs (`e2e/*.sh` **LF** via `.gitattributes`), **`set +u` around `/opt/ros/jazzy/setup.bash`**, replacing **`ros2 --version`**, and URDF spawn via **`gz service … /world/empty_world/create`** (not **`gz model --spawn-file`**, absent on gz CLI 8.x).

### `.dockerignore` note

`tools/mcp/` is **included** in the Docker build context again so **both** `docker/Dockerfile.e2e` and **`docker/compose.pytest.yml`** see MCP submodules (pytest image context grows).

## Phase 0: Environment

Goal: prove each runtime side works before connecting them.

Tasks:

- [x] Choose the first supported setup: **Windows host + WSL2 + Docker**. FreeCAD runs natively on Windows via pixi build. Gazebo and ROS 2 run in Docker containers launched via WSL2 (see `Start-gz-sim.bat` / `Start-ros2.bat`).
- [x] Document GUI forwarding approach for FreeCAD: **No GUI forwarding needed.** FreeCAD runs natively on Windows. The human sees FreeCAD's native window. The Gazebo window is intentionally not used (headless only).
- [x] Install or build FreeCAD 1.x: **FreeCAD 1.2.0-dev** built via pixi from repo source. Entry point: `.pixi/envs/default/Library/bin/FreeCAD.exe`. Launch via `Start-FreeCAD.bat`.
- [ ] Verify RobotCAD opens in FreeCAD and its demo workflow works. **BLOCKER (human)**: requires installing the workbench and running FreeCAD. Use **`scripts/install_robotcad_cross.ps1`** (or Addon Manager → “CROSS” / “RobotCAD”), restart FreeCAD, enable the workbench, then run the demo/export smoke test.
- [x] Start modern Gazebo headless with `gz sim -s`: **Confirmed working** via `Start-gz-sim.bat` (WSL2 + Docker, Ubuntu Noble + OSRF packages). Docker image: `ubuntu:noble`. Build volume: `gz-sim-linux-build`.
- [x] Install and run the selected FreeCAD MCP server: **`neka-nat/freecad-mcp` v0.1.17** installed in WSL2 Python 3.12 venv at `tools/mcp/freecad-mcp/.venv`. Server starts and initializes cleanly.
- [x] Verify the actual FreeCAD MCP/addon RPC transport and port: **XML-RPC on port 9875** (confirmed in `freecad_client.py` line 32 and `server.py` line 65). The `:5000` and `:9876` references in older docs are incorrect for this server.
- [x] FreeCADMCP addon installed to `%APPDATA%\FreeCAD\v1-2\Mod\FreeCADMCP`. On FreeCAD launch, switch to "MCP Addon" workbench and click "Start RPC Server" (or enable Auto-Start).
- [x] Verify an MCP client can create, inspect, and screenshot a simple FreeCAD object. **Automated when FreeCAD RPC is up**: `test_all_mcp.py` runs `create_document` → `create_object` → **`get_object`** → **`get_view`** → `execute_code` → `delete_object`. With FreeCAD closed, the suite still checks that `list_documents` fails gracefully via MCP.
- [x] Install and run the selected Gazebo MCP server: **`kvgork/gazebo-mcp` v0.2.0** installed in WSL2 Python 3.12 venv at `tools/mcp/gazebo-mcp/.venv`. Exposes 27 tools. `gazebo_list_models`, `gazebo_spawn_model`, `gazebo_delete_model` all respond (mock/OK) without Gazebo running.
- [ ] Verify an MCP client can load a world, spawn or inspect a model, pause, resume, reset, and step headless Gazebo. **DEFERRED**: Requires Gazebo container running (Docker build takes 20–40 min on first run). Tool calls return mock responses until Gazebo is live.
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
- **Remaining gaps (documented, not claimed done):** RobotCAD workbench demo/export in GUI; full Gazebo MCP lifecycle against a running headless `gz sim` after Docker build.

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
| RobotCAD/CROSS | **Install via script** | **`scripts/install_robotcad_cross.ps1`** → `%APPDATA%\FreeCAD\v1-2\Mod\freecad.overcross`. GUI/demo verification still pending (Phase 1 blocker until confirmed). |

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
- **Still manual / environment-dependent:** RobotCAD demo in GUI; full Gazebo MCP lifecycle vs live `gz sim` after Docker build.

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
- [ ] **RobotCAD GUI verification**: open FreeCAD, confirm CROSS/RobotCAD workbench loads, run demo/export (install helper: `scripts/install_robotcad_cross.ps1`).
- [ ] Run headless Gazebo end-to-end (first `Start-gz-sim.bat` run takes 20–40 min to build).
- [ ] With FreeCAD + MCP RPC active, manually confirm **`test_all_mcp.py` prints PASS** for `get_object` / `get_view` (same checks as CI agent run below).

**Phase 0 definition-of-done status:** MCP servers start and expose tools under WSL Python 3.12; FreeCAD-side verification is **automated whenever XML-RPC is reachable**; RobotCAD and live Gazebo MCP against `gz sim` remain **manual / environment-dependent** and are tracked as blockers above — not claimed as finished here.

## Phase 1: Manual End-to-End

Goal: manually complete the full design-export-simulate path and capture every friction point.

Tasks:

- [x] Pick one toy robot: **2-DOF planar arm** (`arm_2dof`). Simple enough to verify joint frames, inertias, and basic simulation without controller complexity.
- [ ] Model the robot in FreeCAD using RobotCAD/CROSS conventions. **BLOCKER**: RobotCAD/CROSS not yet installed. A hand-crafted URDF placeholder exists at `robots/arm_2dof.urdf` to unblock Gazebo testing.
- [ ] Define links, joints, limits, visuals, collisions, sensors, controllers, and inertias. **Partially done in placeholder URDF** (2 revolute joints, cylinder visual/collision geometry, approximate inertias). Full definition pending RobotCAD.
- [x] Establish the unit and frame convention: FreeCAD mm (internal), generated sim metres, +Z up, REP-103 naming. Documented in `robots/arm_2dof.urdf` header.
- [ ] Assign materials and densities before inertia export. **Pending**: requires FreeCAD + RobotCAD. Placeholder uses approximate cylinder inertia values.
- [ ] Generate simplified collision geometry. **Partial**: placeholder URDF uses primitive cylinders (correct for simple arm). RobotCAD path will need V-HACD for mesh-based collision.
- [ ] Export the robot through RobotCAD/CROSS to URDF/SDF and ROS 2 package artifacts. **BLOCKER**: RobotCAD not installed.
- [x] Create or select one simple world: `worlds/empty_world.sdf` — ground plane + sun + Bullet physics at 1 ms step.
- [ ] Load the exported robot into headless Gazebo. **DEFERRED**: depends on Gazebo Docker being live and robot URDF validated.
- [ ] Run a short simulation and inspect pose, joint, sensor, contact, and RTF output. **DEFERRED**: depends on above.
- [x] Record issues with units, coordinate frames, etc. — see Phase 1 Friction List below.
- [x] Decide which manual steps must become automation in Phase 2. — see Phase 1 Friction List below.

Deliverables:

- [x] Toy robot URDF placeholder: `robots/arm_2dof.urdf` (hand-crafted; to be replaced by RobotCAD export from `robots/arm_2dof.FCStd`).
- [x] One simple world: `worlds/empty_world.sdf`.
- [ ] `.FCStd` source file (pending FreeCAD + RobotCAD).
- [ ] Generated URDF/SDF from RobotCAD (pending RobotCAD installation).
- [ ] One manually executed scenario (pending Gazebo live run).
- [x] Friction list — see below.

Definition of done:

- A robot designed in FreeCAD runs in headless Gazebo.
- The manual process is documented well enough to repeat.

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
- [x] Add `export_urdf(robot_name, out_dir)` — `bridge/freecad_bridge.py`; calls FreeCAD via XML-RPC `execute_code()` with RobotCAD Python API. Fails cleanly with blocker message when RobotCAD not installed.
- [x] Add `export_sdf_world(world_name, out_dir)` — `bridge/freecad_bridge.py`; validates + stages SDF to generated/. Works now (hand-crafted SDFs).
- [x] Add `compute_inertia_check(robot_name)` and material checks — `bridge/freecad_bridge.py`; inspects FreeCAD document for density assignments.
- [ ] Add collision simplification checks. **DEFERRED**: requires FreeCAD + RobotCAD + mesh analysis. Will add in Phase 3 export pipeline.
- [x] Implement a handoff helper — `bridge/handoff.py`; `export_and_spawn()` orchestrates validate→stage world→wait for Gazebo→spawn in 5 steps. Short-circuits cleanly at each blocker.
- [ ] Add export caching keyed by document hash. **DEFERRED**: premature until RobotCAD export works end-to-end.
- [x] Normalize mesh paths — `bridge/validate.py` `validate_urdf()` detects absolute mesh paths (friction point #5).
- [x] Keep MCP tools coarse-grained — bridge module uses single-call API; LLM agents call `export_and_spawn()` not individual low-level steps.
- [x] Write smoke tests — `tests/test_bridge.py`: 30 offline tests pass; 6 live tests auto-skip when FreeCAD/Gazebo not running.

Deliverables:

- [x] Project manifest schema: `config/schemas/project.schema.yaml`
- [x] Scenario schema: `config/schemas/scenario.schema.yaml`
- [x] Bridge Python package: `bridge/` (project.py, validate.py, freecad_bridge.py, gazebo_bridge.py, handoff.py)
- [x] FreeCAD-to-Gazebo handoff helper: `bridge/handoff.export_and_spawn()`
- [x] Smoke tests: `tests/test_bridge.py` — 30 passed, 6 skipped (live)
- [x] pytest.ini with custom marks (freecad, gazebo)

Definition of done:

- An MCP client can export from FreeCAD and spawn into Gazebo without manual file copying.
- **Partially met**: `export_sdf_world` + `spawn_model` path works for hand-crafted assets; `export_urdf` blocked by RobotCAD installation.

### Phase 2 Notes

- **Design decision**: Bridge module communicates with FreeCAD via direct XML-RPC (same protocol as freecad-mcp client) rather than going through the MCP stdio layer. This is simpler and faster for Python-to-Python calls.
- **Design decision**: Gazebo bridge uses a subprocess MCPClientStdio session per call (not a persistent daemon). Acceptable overhead for Phase 2; Phase 6 can optimise with a persistent connection.
- **Design decision**: No modifications to the upstream MCP server submodules (`tools/mcp/freecad-mcp`, `tools/mcp/gazebo-mcp`). The bridge layer sits above them and calls through their existing APIs. This keeps the submodules cleanly updateable.
- **PyYAML dependency**: Added to Windows Python environment (pip install pyyaml). Not yet in a requirements file — add `requirements-bridge.txt` in Phase 3.
- **Blocker**: `export_urdf()` requires RobotCAD/CROSS installed in FreeCAD. The function fails cleanly and returns a descriptive error message. All 30 offline tests pass without RobotCAD.

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
- [ ] Add Gazebo Status/Screenshot panel — custom FreeCAD panel that shows Gazebo status plus refreshed screenshot snapshots from the headless simulation; **not** an embedded live Gazebo GUI viewport.
- [x] Add Sensor Plots panel (`panels/sensor_plots.py`) — joint position/velocity/effort table, RTF display.
- [x] Add Run Library panel (`panels/run_library.py`) — browses sim_runs/, shows pass/fail status.
- [ ] Add Project Browser panel — deferred to Phase 4 (overlaps with Test Runner UI).
- [x] Add MCP Activity Log panel (`panels/mcp_log.py`) — scrolling audit log of agent tool calls.
- [ ] Verify the human can run and watch a simulation in FreeCAD without opening Gazebo GUI — **blocked**: requires live Gazebo Docker (run `Start-gz-sim.bat`).
- [x] Tests: `tests/test_sim_workbench.py` — 22 tests, all offline; 52 total tests pass.

Deliverables:

- [x] Simulation Workbench addon (`addons/SimWorkbench/`).
- [x] Live State Bridge (`transport.py` + `state_bridge.py`).
- [x] Basic controls and viewer panels (Sim Controls, Scenario Picker, Sensor Plots, Run Library, MCP Log).
- [x] Shared Gazebo transport library (`transport.py`).
- [ ] Camera Viewer — deferred.
- [ ] Gazebo Status/Screenshot panel — status readout plus snapshot image refresh; no embedded Gazebo viewport.
- [ ] Project Browser — deferred.
- [x] Addon install helper (`install_addon.py`).

Definition of done:

- A human can open FreeCAD, switch to the Simulation Workbench, press play, and watch the simulated robot move in FreeCAD's 3D view.
- **Partially met**: All addon code is written and tested offline. Live end-to-end verification blocked by Gazebo Docker not yet started.

### Phase 3 Notes

- **Design decision**: Workbench communicates with Gazebo via `bridge.gazebo_bridge` (same bridge module as the handoff pipeline). No separate ROS 2 Python bindings needed on Windows — all ROS 2 interaction goes through WSL2 via subprocess MCP session.
- **Design decision**: Transport uses a QTimer (10 Hz) rather than a background thread. This avoids threading bugs in FreeCAD's Qt event loop. 10 Hz is sufficient for visual feedback; bump to 30 Hz if needed.
- **Design decision**: FreeCAD Placements are updated directly (not via a FreeCAD feature/document recompute). This is the fastest path for live animation; it does not create an undo history entry.
- **Design decision**: State-to-placement scale = 1000 (Gazebo metres → FreeCAD mm). Configurable via `StateBridge(scale=...)`.
- **Design decision**: A Gazebo Status/Screenshot panel may show periodic screenshots or rendered camera snapshots from Gazebo, but Gazebo remains headless and the panel is not a real embedded Gazebo GUI.
- **Blocker**: Live end-to-end test (play + watch robot move) blocked by Gazebo Docker not running. Run `Start-gz-sim.bat` to build and start.
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
- **Met**: Full offline pipeline works end-to-end with mock bridge. Live runs blocked by Gazebo Docker.

### Phase 4 Notes

- **Design decision**: `runner/` is a standalone Python package; it does NOT require FreeCAD to be installed. The `execute_code` hook in the FreeCAD MCP server is the only coupling point.
- **Design decision**: `run_test()` accepts a `bridge_module` parameter, making it fully unit-testable with mock Gazebo state.
- **Design decision**: result.yaml includes `input_hashes` (SHA-256 of scenario YAML, robot URDF, world SDF). No random seeds in v1 since Gazebo uses deterministic physics by default.
- **Test count**: 96 passed, 6 skipped (live) across all test files.
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
- **Met**: Full offline pipeline works end-to-end with mock bridge. Live runs blocked by Gazebo Docker.

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

- [ ] Pin FreeCAD, RobotCAD/CROSS, ROS 2, Gazebo, Python, Docker, and MCP server versions.
- [ ] Add input hashes to generated outputs and simulation results.
- [ ] Separate read-only and write-capable MCP tools.
- [ ] Add permission prompts or policy controls for write operations.
- [ ] Enforce typed schemas for MCP tools, scenario YAML, project manifests, and result files.
- [ ] Improve Gazebo restart behavior to avoid stale processes, port conflicts, and ROS 2 daemon issues.
- [ ] Add structured logging across FreeCAD workbench actions, MCP calls, exports, ROS 2 interactions, and sim runs.
- [ ] Add collision mesh simplification, likely V-HACD or the RobotCAD-supported equivalent.
- [ ] Add materials and density management for accurate inertias.
- [ ] Add physics-engine and step-size recording for Gazebo runs.
- [ ] Add CI-friendly headless test execution.
- [ ] Add multi-robot and controller bring-up support only after the single-robot path is stable.

Deliverables:

- [ ] Version-pinned runtime.
- [ ] Reproducible result metadata.
- [ ] Permission model.
- [ ] Robust restart path.
- [ ] CI-ready test command.

Definition of done:

- Tests can be rerun reliably and produce explainable, comparable results.

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
- [ ] What write operations should the MCP client be allowed to perform? **Pending**: defer to Phase 6 hardening.
- [x] Which FreeCAD MCP transport and port are canonical for this project? **Decision: XML-RPC on `localhost:9875`.** Confirmed from `freecad_client.py` and `rpc_server.py`.

## Immediate Next Tasks

1. ~~Confirm the environment target and write the setup decision down.~~ **Done** — Windows + WSL2 + Docker (see Phase 0 Environment Decision).
2. ~~Resolve the FreeCAD MCP addon transport and port mismatch in the docs.~~ **Done** — XML-RPC port 9875 confirmed.
3. **Next**: Run **`scripts/install_robotcad_cross.ps1`**, open FreeCAD, enable RobotCAD/CROSS, and verify demo robot export into headless Gazebo.
4. Run headless Gazebo end-to-end once the Docker container is built (first `Start-gz-sim.bat` run).
5. Start FreeCAD with MCP addon active; run `python test_all_mcp.py` with FreeCAD live to get full integration test coverage.
6. Build or choose the toy robot for the first manual end-to-end scenario (Phase 1).
7. Draft the first `reach_top_shelf.yaml` scenario.
8. Record all friction from the manual flow before writing bridge automation.

## Tracking Notes

- Keep the original plan as the architecture narrative.
- Use this file as the working task checklist.
- Promote repeated friction from Phase 1 into explicit Phase 2 implementation tasks.
- Keep generated artifacts and simulation run outputs reproducible from source inputs.
- Avoid expanding the assertion language too early; a small fixed vocabulary is easier to trust and debug.
- Keep the human path inside FreeCAD and the LLM path through MCP.
- Prefer coarse, typed MCP tools over many tiny tool calls.
- Treat ROS 2 control, VLM/image pipelines, randomized environments, and adaptive communication diagnostics as staged additions after the basic test rig works.
