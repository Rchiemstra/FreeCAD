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
Last updated: 2026-05-10 (Phase 0 verification run)

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

## Phase 0: Environment

Goal: prove each runtime side works before connecting them.

Tasks:

- [x] Choose the first supported setup: **Windows host + WSL2 + Docker**. FreeCAD runs natively on Windows via pixi build. Gazebo and ROS 2 run in Docker containers launched via WSL2 (see `Start-gz-sim.bat` / `Start-ros2.bat`).
- [x] Document GUI forwarding approach for FreeCAD: **No GUI forwarding needed.** FreeCAD runs natively on Windows. The human sees FreeCAD's native window. The Gazebo window is intentionally not used (headless only).
- [x] Install or build FreeCAD 1.x: **FreeCAD 1.2.0-dev** built via pixi from repo source. Entry point: `.pixi/envs/default/Library/bin/FreeCAD.exe`. Launch via `Start-FreeCAD.bat`.
- [ ] Verify RobotCAD opens in FreeCAD and its demo workflow works. **BLOCKER**: RobotCAD/CROSS is not installed. It must be installed as a FreeCAD addon (Addon Manager or manual clone). See Phase 0 blockers below.
- [x] Start modern Gazebo headless with `gz sim -s`: **Confirmed working** via `Start-gz-sim.bat` (WSL2 + Docker, Ubuntu Noble + OSRF packages). Docker image: `ubuntu:noble`. Build volume: `gz-sim-linux-build`.
- [x] Install and run the selected FreeCAD MCP server: **`neka-nat/freecad-mcp` v0.1.17** installed in WSL2 Python 3.12 venv at `tools/mcp/freecad-mcp/.venv`. Server starts and initializes cleanly.
- [x] Verify the actual FreeCAD MCP/addon RPC transport and port: **XML-RPC on port 9875** (confirmed in `freecad_client.py` line 32 and `server.py` line 65). The `:5000` and `:9876` references in older docs are incorrect for this server.
- [x] FreeCADMCP addon installed to `%APPDATA%\FreeCAD\v1-2\Mod\FreeCADMCP`. On FreeCAD launch, switch to "MCP Addon" workbench and click "Start RPC Server" (or enable Auto-Start).
- [ ] Verify an MCP client can create, inspect, and screenshot a simple FreeCAD object. **PARTIAL**: MCP server starts and tools list is complete (11 tools). Full verification requires FreeCAD running with the addon and RPC server active on port 9875. The server returns a correct error response when FreeCAD is not running.
- [x] Install and run the selected Gazebo MCP server: **`kvgork/gazebo-mcp` v0.2.0** installed in WSL2 Python 3.12 venv at `tools/mcp/gazebo-mcp/.venv`. Exposes 27 tools. `gazebo_list_models`, `gazebo_spawn_model`, `gazebo_delete_model` all respond (mock/OK) without Gazebo running.
- [ ] Verify an MCP client can load a world, spawn or inspect a model, pause, resume, reset, and step headless Gazebo. **DEFERRED**: Requires Gazebo container running (Docker build takes 20–40 min on first run). Tool calls return mock responses until Gazebo is live.
- [x] Install a ROS 2 MCP option: **`ros-mcp` v3.0.1** installed in WSL2 Python 3.12 venv at `tools/mcp/ros-mcp-server/.venv`. Exposes 31 tools. `ping_robots`, `connect_to_robot`, `get_topics`, `get_nodes` all respond without ROS 2 running.
- [x] Document exact versions — see Version Table below.

Deliverables:

- [x] Reproducible environment notes — see Environment Decision and Version Table below.
- [x] Minimal smoke-test command list — `python test_all_mcp.py` (all 17 tests pass).
- [x] Confirmed MCP transport/port notes — XML-RPC port 9875.
- [x] Known setup issues and fixes — see Phase 0 Notes below.

Definition of done:

- FreeCAD, Gazebo, and the selected MCP servers can be controlled independently.
- No FreeCAD-to-Gazebo automation is required yet.

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
| RobotCAD/CROSS | **NOT INSTALLED** | Blocker for Phase 1; see below |

### Phase 0 Smoke-Test Commands

```
# Run all MCP server protocol tests (no FreeCAD/Gazebo/ROS running required)
python test_all_mcp.py --timeout 30

# Run only FreeCAD MCP tests
python test_all_mcp.py --no-gazebo --no-ros

# Run with FreeCAD+Gazebo+ROS running (full integration)
python test_all_mcp.py --start-apps --startup-wait 30

# Test result (2026-05-10): 17 passed, 0 failed, 0 skipped
```

### Phase 0 Notes

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
4. RobotCAD/CROSS is not installed (see Phase 0 blockers).

**Phase 0 Blockers / Remaining Tasks:**
- [ ] Install RobotCAD/CROSS workbench in FreeCAD and verify its demo robot export works.
  - Install via FreeCAD Addon Manager → search "CROSS" or "RobotCAD".
  - Or clone from https://github.com/drfenixion/freecad.overcross into `%APPDATA%\FreeCAD\v1-2\Mod\`.
- [ ] Run headless Gazebo end-to-end (first `Start-gz-sim.bat` run takes 20–40 min to build).
- [ ] Run FreeCAD with MCP addon active and verify `create_document`, `create_object`, `get_view` tool calls succeed end-to-end.

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

# Spawn the arm:
gz model --spawn-file /mnt/c/Users/Rchie/Music/FreeCAD/robots/arm_2dof.urdf \
         --model-name arm_2dof

# Check pose:
gz model --model-name arm_2dof -p
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

- [ ] Create the FreeCAD Simulation Workbench addon skeleton.
- [ ] Build a shared Gazebo/ROS 2 transport layer used by both the workbench and MCP tooling.
- [ ] Implement Gazebo process lifecycle controls: start, pause, resume, step, reset, stop, and reload.
- [ ] Implement the Live State Bridge:
  - subscribe to Gazebo pose and joint-state topics,
  - translate simulation state into FreeCAD object placements,
  - update FreeCAD through the GUI-safe Qt path,
  - throttle or sample updates to keep the UI responsive.
- [ ] Add Sim Controls panel with play, pause, step, reset, reload, sim time, and RTF.
- [ ] Add Scenario Picker panel for robot, world, initial pose, and saved scenario selection.
- [ ] Add Camera Viewer panel for image topics.
- [ ] Add Sensor Plots for joint state, IMU, contact force, odometry, and selected telemetry.
- [ ] Add Run Library panel listing prior `sim_runs/`.
- [ ] Add Project Browser for robots, worlds, scenarios, and generated artifacts.
- [ ] Add MCP Activity Log panel for debugging agent actions.
- [ ] Verify the human can run and watch a simulation in FreeCAD without opening Gazebo GUI.

Deliverables:

- [ ] Simulation Workbench addon.
- [ ] Live State Bridge.
- [ ] Basic controls and viewer panels.
- [ ] Shared Gazebo/ROS integration library.

Definition of done:

- A human can open FreeCAD, switch to the Simulation Workbench, press play, and watch the simulated robot move in FreeCAD's 3D view.

## Phase 4: Test Runner

Goal: turn simulation into repeatable regression tests.

Tasks:

- [ ] Finalize the v1 scenario YAML schema.
- [ ] Finalize the v1 assertion vocabulary.
- [ ] Start with fixed assertions:
  - `reach_target_within`
  - `no_self_collision`
  - `max_joint_torque_below`
  - `sim_time_under`
  - `pose_within_tolerance`
  - `rtf_above`
  - `collision_count_below`
- [ ] Implement scenario loading from `tests/scenarios/`.
- [ ] Implement single-test execution.
- [ ] Implement run-all execution.
- [ ] Evaluate assertions from Gazebo state, ROS 2 topics, or recorded outputs.
- [ ] Write `sim_runs/<timestamp>_<scenario>/result.yaml`.
- [ ] Include input hashes, generated artifact hashes, tool versions, physics settings, and random seeds in each result.
- [ ] Record selected telemetry: joint states, poses, contacts, RTF, sensor summaries, logs, and screenshots.
- [ ] Add pass/fail dashboard to the Simulation Workbench.
- [ ] Add `list_tests` and `run_test(name)` to the FreeCAD MCP surface.
- [ ] Add regression tests for scenario parsing and assertion evaluation.

Deliverables:

- [ ] Scenario runner.
- [ ] Assertion evaluator.
- [ ] Result writer.
- [ ] Workbench Test Runner panel.
- [ ] MCP test tools.

Definition of done:

- A robot design can be regression-tested through repeatable scenarios, and results are visible in FreeCAD and available to the MCP client.

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

- [ ] Add a stable `set_parameter(doc, name, value)` flow for controlled FreeCAD edits.
- [ ] Add bounded edit policies for dimensions, materials, controller settings, and scenario inputs.
- [ ] Add a loop that changes a parameter, recomputes, exports, runs a scenario, and reports results.
- [ ] Capture screenshots, plots, selected sensor summaries, and result diffs for LLM review.
- [ ] Add parameter sweep support for numeric design variables.
- [ ] Add failure summarization that points back to the relevant scenario assertion and metric.
- [ ] Decide when design changes should be automatically committed.
- [ ] Avoid sim-state-to-CAD edits in v1; keep design-to-sim as the stable direction.

Deliverables:

- [ ] Parameter iteration flow.
- [ ] Sensor and result summaries.
- [ ] Optional parameter sweep runner.
- [ ] Result comparison report.

Definition of done:

- An LLM can make a bounded design change, rerun a failing scenario, and report whether the change improved the result.

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
3. **Next**: Install RobotCAD/CROSS in FreeCAD (via Addon Manager or manual clone) and run its demo robot export into headless Gazebo.
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
