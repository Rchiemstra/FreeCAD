# FreeCAD Gazebo MCP Task Breakdown

Source docs:

- [freecad_gazebo_mcp_plan.md](freecad_gazebo_mcp_plan.md)
- [FreeCAD Model Simulation Pipeline Integration.md](FreeCAD%20Model%20Simulation%20Pipeline%20Integration.md)
- [diagrams/freecad_gazebo_mcp_component_architecture.puml](diagrams/freecad_gazebo_mcp_component_architecture.puml)
- [diagrams/freecad_gazebo_mcp_deployment_docker.puml](diagrams/freecad_gazebo_mcp_deployment_docker.puml)
- [diagrams/freecad_gazebo_mcp_mcp_tool_surface.puml](diagrams/freecad_gazebo_mcp_mcp_tool_surface.puml)
- [diagrams/freecad_gazebo_mcp_sequence_human_in_workbench.puml](diagrams/freecad_gazebo_mcp_sequence_human_in_workbench.puml)
- [diagrams/freecad_gazebo_mcp_sequence_iteration_loop.puml](diagrams/freecad_gazebo_mcp_sequence_iteration_loop.puml)

Last reviewed: 2026-05-11
Last updated: 2026-05-11 (Phase 6: structured JSONL + **gazebo-mcp stdio reconnect** — `bridge/gazebo_bridge.py`, `bridge/structured_log.py`, `tests/test_gazebo_session_reconnect.py`, Docker pytest **191** passed / **6** skipped)

## Completion status (rollup)

Use this table for a quick read on what is **done in repo / CI**, what is **partial**, and what still needs **human or live-sim** time.

| Phase | Status | Notes |
| --- | --- | --- |
| Phase 0: Environment | **Partial** | MCP smoke + versions documented; RobotCAD GUI demo + optional live Gazebo MCP on WSL still manual. **Docker E2E** validates Harmonic + spawn + scenarios without the WSL gz build. |
| Phase 1: Manual E2E | **Partial** | Placeholder URDF + `empty_world.sdf` + friction list + `reach_top_shelf.yaml` draft; **no committed `arm_2dof.FCStd`** yet; full RobotCAD export path blocked on model + workbench verification. |
| Phase 2: Automated bridge | **Partial** | `bridge/`, schemas, handoff, offline tests; **`export_urdf`** still blocked without RobotCAD inside FreeCAD. **`gz_cli_bridge`** used in E2E for reliable URDF spawn. |
| Phase 3: Sim Workbench | **Partial** | **Gazebo Status** panel + **manual GUI smoke checklist** (§ *FreeCAD GUI smoke*); **live** `gz.msgs.Image` verified in **Docker E2E** only. Host FreeCAD click-test follows that checklist when a GUI is available. |
| Phase 4: Test runner | **Mostly met offline** | Runner, assertions, results with hashes, workbench panel, `tests/scenarios_e2e/e2e_smoke.yaml`; live RTF/joint telemetry fidelity tied to running `gz sim`. |
| Phase 4.5: ROS 2 control | **Not started** | `ros-mcp` installed; controller-in-the-loop scenarios not built out. |
| Phase 5: Iteration loops | **Mostly met offline** | `iteration/` + tests; screenshots / auto-commit policy deferred. |
| Phase 6: Hardening | **Partial** | Runtime + MCP policy + **structured JSONL** + **gazebo-mcp stdio session lifecycle** (bounded session-start retries, read-only transport reconnect, JSONL `session_*` / `reconnect_*`); **full stack** restart (Docker ports, ROS daemon) still manual. |
| Phase 7: Scale-out | **Not started** | Placeholder tasks only. |
| Docker E2E | **Implemented** | `docker/compose.e2e.yml` + `e2e/run_e2e.sh`; see **Docker E2E** section. |

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

One-off run (same script as `up`, without compose lifecycle): `docker compose -f docker/compose.e2e.yml run --rm e2e`

**Runtime / version pins** for this stack (ROS 2 distro, Gazebo family, base images, MCP versions, env vars): **`config/runtime_manifest.yaml`**. **Bridge MCP write policy** (allowed artifact roots, tool read/mutate classification, optional deny): **`config/mcp_permissions.yaml`** — default Docker E2E leaves mutating gazebo-mcp calls enabled; screenshots still land under **`sim_runs/`** via **`E2E_RUN_DIR`**. **Structured JSONL** (MCP calls, captures, permissions, runner): **``<E2E_RUN_DIR>/logs/structured.jsonl``** (see **`bridge/structured_log.py`**, set by **`e2e/run_e2e.sh`**).

Example JSONL event (one line) from an MCP tools/call via **`GazeboSession`**:

```json
{"schema": 1, "event": "mcp_tool_call", "component": "bridge.gazebo_bridge", "tool": "gazebo_list_sensors", "ok": true, "duration_ms": 42.5, "permission_gate": "read_only", "ts_unix": 1715432100.12, "ts_iso": "2026-05-11T12:34:56Z"}
```

### Verified run log (2026-05-11, Docker Desktop Linux engine)

| Check | Result |
| --- | --- |
| Compose | `docker compose -f docker/compose.e2e.yml run --rm e2e` → **exit 0** |
| Pytest image (same commit) | `docker compose -f docker/compose.pytest.yml run --rm pytest` → **191 passed**, **6 skipped** |
| Bridge smoke artifacts | Under **`sim_runs/e2e_<UTC>/`**: `console.log`, **`logs/structured.jsonl`** (JSONL: MCP calls, sensor discovery, captures, permissions), **`bridge_gazebo_mcp_smoke/summary.json`** (includes **`camera_source_mode`**, **`image_width_reported` / `image_height_reported`**, **`gz_image_topic`**, **`sensor_catalog`**, PNG IHDR cross-check), **`status.json`**, **`sensors.json`**, **`screenshots/*.png`**. Example: `sim_runs/e2e_20260511T015019Z/bridge_gazebo_mcp_smoke/screenshots/gazebo_cam_20260511_015310.png` (**~1.8 KB** compressed real frame — not 1×1 mock). |
| Scenario | `e2e_smoke` **PASS** (gz_cli spawn + assertions) |

**Repeatability:** `e2e/run_e2e.sh` runs **`git submodule update --init --depth 1`** for `tools/mcp/{gazebo-mcp,freecad-mcp,ros-mcp-server}` before `pip install -e` (first run may populate those directories on the **host** bind-mount).

### Gap (honest)

- **Non-camera sensors** in **gazebo-mcp** `auto` mode: LiDAR / IMU entries in **`list_sensors`** are still **mock placeholders** when no ROS topic bridge is configured; only **cameras** are discovered from **`gz topic -l`** + **`gz.transport`** today.
- **MCP venv + gz Python bindings**: **`fetch_live_camera_frame`** appends **`/usr/lib/python3/dist-packages`** (and **`…/python3.12/…`**) to **`sys.path`** so **`/opt/mcp-venv`** can `import gz.transport13`; **`e2e/run_e2e.sh`** also appends that path to **`PYTHONPATH`** for subprocess consistency.
- **FreeCAD Simulation Workbench GUI** (Gazebo Status dock): manual checklist in **§ FreeCAD GUI smoke** below; automated agent runner had **no FreeCAD on `PATH`** (2026-05-11). Panel shows **`source: live`** and **`size: WxH`** after capture when the bridge returns metadata.

### FreeCAD GUI smoke: Gazebo Status dock (manual checklist)

**Agent note (2026-05-11):** On the Windows CI/agent machine used for this update, **`FreeCAD` / `FreeCADCmd`** were not on **`PATH`** and the default **WSL** distro did not expose **`freecadcmd-daily`**, so **clicks were not executed here**. The steps below are the authoritative procedure for a human developer; they mirror the already-green **Docker E2E** bridge path (`get_simulation_status` → `list_gazebo_sensors` → `capture_camera_snapshot` with **`camera_source_mode: live`**, **320×240** PNG).

**Reference FreeCAD build:** The Docker E2E image reports **FreeCAD 1.1.0, Revision 43087 (Git)** — use the same lineage or newer on the host.

**Backend (must match E2E):** Headless **Gazebo Sim 8** with **`worlds/e2e_world.sdf`**, started with **`gz sim -r -s …`** so camera topics publish. **gazebo-mcp** must see **`gz topic -l`** and **`gz.msgs.Image`** (see **`GAZEBO_MCP_SENSOR_MODE`**, **`PYTHONPATH`** / system **`gz`** bindings in **`e2e/run_e2e.sh`**). On Windows, **`bridge.gazebo_bridge`** launches **gazebo-mcp-server** via **WSL** by default — run **Gazebo + MCP** in **that same WSL distro**, or set **`GAZEBO_MCP_CMD`** to a custom launcher if you use Docker-only MCP.

| Step | Action | Pass criteria |
| --- | --- | --- |
| 1 | Install **SimWorkbench** per **`InitGui.py`**: for **in-repo** dev, keep **`addons/SimWorkbench/`** under the git root so **`InitGui.py`** can reach **`bridge/`** via **`join(addon_dir, '..', '..')`**. If you copy only **`SimWorkbench/`** into **`%APPDATA%\\FreeCAD\\<channel>\\Mod\\`**, you must also put the **git repo root** on **`PYTHONPATH`** (or adjust **`_repo_root`**) so **`import bridge`** resolves. | FreeCAD starts; **Report view** shows no **`No module named 'bridge'`** on workbench switch. |
| 2 | In **WSL** (same distro MCP uses): start **`gz sim -r -s <repo>/worlds/e2e_world.sdf`** (plus **`DISPLAY`** / **Xvfb** if headless). Export **`GZ_SIM_WORLD_NAME=empty_world`**, **`GAZEBO_MCP_SENSOR_MODE=auto`**, and the same **`PYTHONPATH`** / **`MCP_VENV`** pattern as **`e2e/run_e2e.sh`** if your MCP venv cannot `import gz` alone. | **`gz topic -l`** includes **`…/sensor/e2e_camera/image`**. |
| 3 | Launch **FreeCAD (GUI)**. Switch workbench to **Simulation Workbench**. | **Gazebo status & screenshot** dock appears (title *Gazebo status & screenshot*); **Transport** line updates (Connecting / Connected / …). |
| 4 | Click **Refresh status**. | **Simulation status (JSON)** text box shows readable JSON from **`get_simulation_status`**; heading **MCP: ok** when the subprocess succeeds. |
| 5 | (Optional) Set **Sensor** field to **`e2e_camera`** or leave blank for auto-pick. | — |
| 6 | Click **Capture screenshot**. | Path line includes **`Saved:`** … **`source: live`**, **`size: 320×240`**, and **`topic:`** … **`e2e_camera/image`**; **preview** shows the frame; on disk **`sim_runs/screenshots/gazebo_cam_<UTC>.png`** is a **320×240** PNG (not a **1×1** mock). |
| 7 | (Optional) Save a **screenshot of the FreeCAD window** or note the **`sim_runs/...`** path for your run log. | Attach path under **`sim_runs/`** in PR / issue text if sharing. |

**Caveats:** The panel imports **PySide2** (matches common Windows FreeCAD builds). If your build only ships **PySide6**, the dock may fail to load until imports are updated. **LiDAR/IMU** live paths are **out of scope** for this smoke (camera-only live stack).

| Stage | Implementation |
| --- | --- |
| Base image | `docker/Dockerfile.e2e` — `ros:jazzy-ros-base-noble` + **Gazebo Harmonic** (OSRF apt) + **freecad-daily** PPA + **RobotCAD** clone at `/root/.local/share/FreeCAD/Mod/RobotCAD` → `/opt/robotcad` |
| MCP venv | `/opt/mcp-venv` core deps; **runtime** `pip install -e` on bind-mounted `tools/mcp/{gazebo-mcp,freecad-mcp,ros-mcp-server}` |
| Driver script | `e2e/run_e2e.sh` — versions, `${FREECAD_CMD}` RobotCAD import (`e2e/check_robotcad_freecad.py`), `e2e/stage_export.sh`, `check_urdf`, Xvfb, background **`gz sim -r -s worlds/e2e_world.sdf`** (**`-r`** runs sim so camera sensors publish), **`PYTHONPATH`** includes **`/usr/lib/python3/dist-packages`** for gz-msgs/transport in the MCP venv, **`e2e/mcp_smoke.py`**, **`e2e/bridge_gazebo_mcp_smoke.py`** ( **`get_simulation_status` / `list_gazebo_sensors` / `capture_camera_snapshot`** → `sim_runs/e2e_<UTC>/bridge_gazebo_mcp_smoke/` ), **`python -m runner.runner run-all --dir tests/scenarios_e2e`** |
| Bridge panel smoke | **`e2e/bridge_gazebo_mcp_smoke.py`** — requires **`camera_source_mode=live`**, PNG IHDR **≥ 64×64**, file **> 400 B**; artifacts under **`$E2E_RUN_DIR/bridge_gazebo_mcp_smoke/`** |
| Runner ↔ Gazebo | **`bridge/gz_cli_bridge`** via **`E2E_BRIDGE_MODULE=gz_cli`** — spawn uses **`gz service -s /world/<world>/create`** with **`gz.msgs.EntityFactory`** / **`sdf_filename`** (URDF path); world defaults to **`GZ_SIM_WORLD_NAME=empty_world`**. MCP **`gazebo_spawn_model`** still does not forward **`model_xml`** today. |
| MCP smoke | **`e2e/mcp_smoke.py`** — reuses **`test_all_mcp.MCPClientStdio`** against all three servers |
| Scenario | **`tests/scenarios_e2e/e2e_smoke.yaml`** — lightweight assertions (no joint torque / reach goals) |

### Engineering choices / findings

- **Single container** first — matches the integration doc and avoids cross-container ROS DDS setup for v1.
- **`bridge/gazebo_bridge.py`**: resolves **gazebo-mcp-server** via **`MCP_VENV`** (Docker `/opt/mcp-venv`) when present, else submodule **`.venv`**; Windows keeps **WSL** launch; **`GazeboSession`** builds the argv at session start (not import time) so compose env is respected.
- **`runner/executor.py`**: live bridge loads **`gz_cli_bridge`** only when **`E2E_BRIDGE_MODULE`** is enabled; default **`None`** preserves mock-injected unit tests. **`_urdf_path`** prefers **`generated/<robot>/<robot>.urdf`** when staged/exported, otherwise **`robots/<robot>.urdf`**.
- **`SIM_RUNS_DIR`** env wired through **`runner.runner` CLI** for `run` / `run-all`.
- **Export honesty**: without **`robots/arm_2dof.FCStd`**, staging copies the checked-in URDF after RobotCAD **import** succeeds — full mesh-export-through-RobotCAD remains blocked until an FCStd is committed.
- **Shell scripts for Linux**: **`e2e/*.sh`** checked in as **LF** (`.gitattributes`); **CRLF** breaks shebang execution when Compose runs `bash -lc /workspace/e2e/run_e2e.sh`.
- **ROS environment**: **`set -u`** before **`source /opt/ros/jazzy/setup.bash`** trips on unset `AMENT_*` variables — bracket the source with **`set +u`** / **`set -u`**.
- **URDF spawn on gz-sim 8**: **`gz model --spawn-file`** is not available; use **`gz service -s /world/<world>/create`** with **`gz.msgs.EntityFactory`** and **`sdf_filename`** (Harmonic tutorial). Pre-delete uses **`/world/<world>/remove`**; “entity not found” on first run is benign.
- **Headless rendering**: `LIBGL_ALWAYS_SOFTWARE=1` by default in **`e2e/run_e2e.sh`** after Xvfb starts — improves **ogre2** stability for **`gz-sim-sensors-system`** in Docker.
- **Gazebo must be running for camera frames**: use **`gz sim -r -s …`** in E2E so sensors publish; paused sim produces no `gz.msgs.Image` callbacks.

### Blockers / next steps

- [ ] Add **`robots/arm_2dof.FCStd`** with `Cross::*` entities so `e2e/export_robotcad_fcstd.py` runs a real exporter path.
- [ ] Optionally extend **gazebo-mcp** `spawn_model_wrapper` to pass **`model_xml`** through to `model_management.spawn_model`.
- [x] **Live gz camera path** in **gazebo-mcp** (see **`gazebo_mcp.gz_live_camera`**); extend to **LiDAR/IMU** via gz or ROS when needed.
- **CI / agent validation (2026-05-11):** `docker compose -f docker/compose.pytest.yml run --rm pytest` → **191 passed**, **6 skipped**. **`docker compose -f docker/compose.e2e.yml run --rm e2e`** → **OK** (includes **`bridge_gazebo_mcp_smoke.py`** + scenarios). **FreeCAD GUI:** checklist only — no **`FreeCAD`** on agent **`PATH`**. Older notes on CRLF shebangs, **`set +u`** around ROS setup, and **`gz service … /create`** remain applicable.

### `.dockerignore` note

`tools/mcp/**` stays in the build context for pytest/E2E. **Colcon** `install/`, `build/`, `log/` trees under MCP submodules are **ignored** (see root `.dockerignore`) so Docker Desktop does not fail on odd files when submodules were built locally.

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
- [ ] Verify an MCP client can load a world, spawn or inspect a model, pause, resume, reset, and step headless Gazebo. **Partially covered**: **Docker E2E** runs `gz sim -s`, spawn via `gz service … /world/empty_world/create`, and `e2e/mcp_smoke.py` against live MCP servers. **Still optional for WSL**: first `Start-gz-sim.sh` source build is long; gazebo-mcp tool calls against that stack remain manual.
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
| Docker E2E image (`Dockerfile.e2e`) | ROS 2 **Jazzy** + **Gazebo Harmonic** + FreeCAD daily | Reproducible Linux CI path; differs from WSL “rolling” dev container |
| **Runtime manifest (CI pins)** | `config/runtime_manifest.yaml` | Single list for Docker base images, ROS/Gazebo/Python/MCP versions, env vars; reconciles vs `project.yaml` |
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
# CI pins: config/runtime_manifest.yaml
docker compose -f docker/compose.pytest.yml build
docker compose -f docker/compose.pytest.yml run --rm pytest

# Full Docker E2E (Linux engine — ROS + Gazebo + FreeCAD daily; first build is large)
docker compose -f docker/compose.e2e.yml up --build --abort-on-container-exit --exit-code-from e2e
# Equivalent one-shot:
docker compose -f docker/compose.e2e.yml run --rm e2e
# E2E writes: sim_runs/e2e_<UTC>/console.log and bridge_gazebo_mcp_smoke/{summary,status,sensors}.json + screenshots/
# Optional: gazebo-mcp sensor source — auto (default, live gz cameras when `gz` + bindings available), mock, live
#   export GAZEBO_MCP_SENSOR_MODE=mock   # e.g. host pytest without Gazebo

# Recorded run (2026-05-10): python test_all_mcp.py --timeout 30
#   Total: 17 passed, 0 failed, 0 skipped (FreeCAD XML-RPC not running — expected WARN then graceful list_documents).
#   When FreeCAD is running with the MCP addon RPC server on port 9875, the same command additionally exercises
#   create_document → create_object → get_object → get_view → execute_code → delete_object (full MCP integration path).
```

### Phase 0 Notes

**Completion pass (2026-05-10):**

- **`bridge/structured_log.py`** (2026-05-11): JSONL events for MCP timing, sensor/capture metadata, permission denials, SimWorkbench panel actions; E2E writes **`logs/structured.jsonl`** under **`E2E_RUN_DIR`**; runner writes alongside **`result.yaml`**.
- **`config/runtime_manifest.yaml`** (2026-05-11): single authoritative list for **Docker E2E + compose pytest** pins; `project.yaml` / WSL rows in the version table stay as **product / host** targets (see manifest **`reconciliation`**).
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
- [x] Example scenario YAML drafted: `tests/scenarios/reach_top_shelf.yaml` (targets `arm_2dof` + assertions; needs live Gazebo + exported robot to be a true manual run).
- [ ] One **manually witnessed** run of that scenario in WSL/Docker with GUI-less FreeCAD viewer (pending live stack + FCStd export).
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
- **Design decision**: Gazebo bridge uses a subprocess MCP client **per** `with GazeboSession()` block (short-lived). Phase 6 adds **bounded session-start retries** and **read-only transport reconnect** inside that block; persistent daemons and Docker-level restart remain future work.
- **Design decision**: No modifications to the upstream MCP server submodules (`tools/mcp/freecad-mcp`, `tools/mcp/gazebo-mcp`). The bridge layer sits above them and calls through their existing APIs. This keeps the submodules cleanly updateable.
- **Python deps**: Bridge stack is listed in **`requirements-bridge.txt`** (repo root); install alongside dev tools as needed.
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
- [x] Add Gazebo Status/Screenshot panel (`panels/gazebo_status_panel.py` + `panels/gazebo_status_logic.py`) — transport line + **Refresh status** + **Capture screenshot**; success line shows **`source:`** / **`size:`** / **`topic:`** when the bridge returns metadata. **Docker E2E:** `e2e/bridge_gazebo_mcp_smoke.py`. **FreeCAD GUI:** manual checklist **§ FreeCAD GUI smoke** (clicks not run in agent env without FreeCAD).
- [x] Add Sensor Plots panel (`panels/sensor_plots.py`) — joint position/velocity/effort table, RTF display.
- [x] Add Run Library panel (`panels/run_library.py`) — browses sim_runs/, shows pass/fail status.
- [ ] Add Project Browser panel — deferred to Phase 4 (overlaps with Test Runner UI).
- [x] Add MCP Activity Log panel (`panels/mcp_log.py`) — scrolling audit log of agent tool calls.
- [ ] Verify the human can run and watch a simulation in FreeCAD without opening Gazebo GUI — **blocked**: requires live Gazebo Docker (run `Start-gz-sim.bat`).
- [x] Tests: `tests/test_sim_workbench.py` — transport/state bridge + Gazebo status / MCP media helpers (offline). **Docker pytest (2026-05-11):** **161 passed**, **6 skipped** for full `tests/` tree (`docker compose -f docker/compose.pytest.yml build pytest && docker compose -f docker/compose.pytest.yml run --rm pytest`).

Deliverables:

- [x] Simulation Workbench addon (`addons/SimWorkbench/`).
- [x] Live State Bridge (`transport.py` + `state_bridge.py`).
- [x] Basic controls and viewer panels (Sim Controls, **Gazebo Status**, Scenario Picker, Sensor Plots, Run Library, MCP Log).
- [x] Shared Gazebo transport library (`transport.py`).
- [ ] Camera Viewer — deferred.
- [x] Gazebo Status/Screenshot panel — `panels/gazebo_status_panel.py` (plus `bridge.gazebo_bridge`: `get_simulation_status`, `list_gazebo_sensors`, `capture_camera_snapshot`, `pick_camera_sensor_from_mcp_list`).
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
- **Implemented (2026-05-11)**: **Gazebo Status** dock — combines `GazeboTransport` connection labels with MCP **Refresh status** / **Capture screenshot**; screenshots use **gazebo-mcp** `gazebo_get_sensor_data` with **live gz** `Image` when **`GAZEBO_MCP_SENSOR_MODE`** is **`auto`** or **`live`**. On success, the path line shows **`source:`**, **`size:`**, and **`topic:`** for quick **live vs mock** verification. **Docker pytest:** **161 passed**, **6 skipped**. **Docker E2E** requires **`camera_source_mode=live`** in **`bridge_gazebo_mcp_smoke/summary.json`**. **Gap:** non-camera sensors still use mock rows in **`auto`** mode. **FreeCAD GUI:** manual checklist in **§ FreeCAD GUI smoke**; the automated agent runner had **no FreeCAD** on **`PATH`** (documented 2026-05-11).
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
- [x] CLI entry point: `python -m runner.runner list` / `run` / `run-all` — covered by `tests/test_runner.py` and used by `e2e/run_e2e.sh` (`run-all --dir tests/scenarios_e2e`).

Definition of done:

- A robot design can be regression-tested through repeatable scenarios, and results are visible in FreeCAD and available to the MCP client.
- **Met**: Full offline pipeline works end-to-end with mock bridge. Live runs blocked by Gazebo Docker.

### Phase 4 Notes

- **Design decision**: `runner/` is a standalone Python package; it does NOT require FreeCAD to be installed. The `execute_code` hook in the FreeCAD MCP server is the only coupling point.
- **Design decision**: `run_test()` accepts a `bridge_module` parameter, making it fully unit-testable with mock Gazebo state.
- **Design decision**: result.yaml includes `input_hashes` (SHA-256 of scenario YAML, robot URDF, world SDF). No random seeds in v1 since Gazebo uses deterministic physics by default.
- **Test count**: full `pytest tests` totals are recorded in Phase 5 Notes (151 passed, 6 skipped as of 2026-05-10); `tests/test_runner.py` exercises the runner package.
- **Commit**: `phase 4: test runner, assertion evaluator, result writer`

## Phase 4.5: ROS 2 Control and Telemetry

Goal: support controller-in-the-loop tests without bloating the v1 runner.

Tasks:

- [x] Choose the initial ROS 2 MCP bridge — **use `ros-mcp` v3.0.1** (`tools/mcp/ros-mcp-server`) already wired for MCP smoke tests; re-evaluate only if a missing capability blocks a scenario (e.g. dedicated rosbridge deployment).
- [ ] Add topic/action discovery for the RobotCAD-generated ROS 2 package.
- [ ] Add controlled publishing for `/cmd_vel`, joint commands, or action goals.
- [ ] Add read-only tools for topics, services, actions, node graph, and message schemas.
- [ ] Support Nav2 or ros2_control only after simple command publishing works.
- [ ] Record robot-perceived telemetry separately from Gazebo ground truth.
- [ ] Add VLM/image retrieval path only after camera topics are stable.

Deliverables:

- [ ] ROS 2 MCP selection note — **stub:** default path is `ros-mcp` v3.0.1; document tool → topic mapping in `docs/` when the first controller scenario lands.
- [ ] Controller-in-the-loop scenario example.
- [ ] Telemetry capture format.

Definition of done:

- One scenario can command the robot through ROS 2 control interfaces and evaluate both ground-truth and robot-perceived telemetry.

### Phase 4.5 Notes

- **Depends on:** stable Phase 4 runner, a robot with a **RobotCAD-generated ROS 2 package** (or equivalent topic graph), and ROS 2 nodes running in the same DDS domain as the MCP client (often the Docker E2E image or WSL ROS container).
- **Suggested v1 slice:** publish a simple velocity or joint command for 1–2 seconds, assert pose/joint metrics from existing assertion types, and record a second telemetry stream (e.g. `/joint_states`) alongside Gazebo ground truth in `sim_runs/`.
- **Defer:** Nav2, `ros2_control` bring-up, and VLM/camera ingestion until `/cmd_vel` or joint trajectory publishing is reliable end-to-end.

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

- [x] Pin FreeCAD, RobotCAD/CROSS, ROS 2, Gazebo, Python, Docker, and MCP server versions in one place — **`config/runtime_manifest.yaml`** (authoritative for **Docker E2E + compose pytest**; `project.yaml` + this doc’s version table remain **product / host** narrative — see manifest **`reconciliation`**).
- [x] Add input hashes to simulation results — **`runner/result.py`** writes SHA-256 for scenario, URDF, world SDF into **`result.yaml`** (extend to “all generated outputs” when export pipeline is unified).
- [x] Separate read-only and write-capable MCP tools — **`config/mcp_permissions.yaml`** lists **read-only** gazebo-mcp tool names; all others are treated as **mutating**; **`tests/test_mcp_write_policy.py`** exercises classification + path gates + **`BRIDGE_MCP_DENY_MUTATING`** denial.
- [x] Add permission prompts or policy controls for write operations — **bridge policy** (no interactive UI yet): **`bridge/mcp_write_policy.py`** gates **`GazeboSession`** stdio calls, validates MCP path arguments, bounds **`capture_camera_snapshot`** / **`export_*`** output dirs to **`sim_runs/`** + **`generated/`** (optional **`BRIDGE_MCP_EXTRA_WRITE_ROOTS`**). **Deferred:** interactive prompts; **ros-mcp** / **freecad-mcp** stdio servers not wired through this helper yet (see manifest `deferred_servers`).
- [x] Enforce typed schemas for scenario YAML, project manifests — **`config/schemas/*.schema.yaml`** + loaders; **MCP tool** typing / JSON schemas for agents still TBD.
- [ ] Improve Gazebo **stack** restart behavior (Docker / gz-transport ports, ROS 2 daemon lifecycle) — **host / compose operations**, not covered by the MCP-only reconnect below.
- [x] **Gazebo MCP stdio reconnect / session lifecycle** — **`bridge/gazebo_bridge.GazeboSession`**: bounded subprocess start + MCP `initialize`, read-only `tools/call` transport retry, JSONL lifecycle events, concise UI errors; tests **`tests/test_gazebo_session_reconnect.py`**. Policy table: **§ Gazebo MCP reconnect policy (Phase 6)**.
- [x] Add structured logging across FreeCAD workbench actions, MCP calls, exports, ROS 2 interactions, and sim runs — **JSONL** via **`bridge/structured_log.py`** (MCP tool timing + permission gate, sensor discovery, simulation status, screenshot metadata, panel actions); **runner** appends **`scenario_run_result`** next to **`result.yaml`**; **E2E** sets **`BRIDGE_STRUCTLOG_PATH`** to **`logs/structured.jsonl`** (`e2e/run_e2e.sh`); **SimWorkbench** logs to the same file when env is set + **Report view** one-line summaries. **Deferred:** full ROS 2 interaction logging; `LOG_FORMAT=json` on root logger remains separate (see **`bridge/logging_config.py`**).
- [ ] Add collision mesh simplification, likely V-HACD or the RobotCAD-supported equivalent.
- [ ] Add materials and density management for accurate inertias.
- [ ] Add physics-engine and step-size recording for Gazebo runs (world SDF already sets step; record **actual** engine + step in `result.yaml`).
- [x] CI-friendly headless test execution — **`docker compose -f docker/compose.pytest.yml`** (pytest) and **`docker compose -f docker/compose.e2e.yml`** (full stack smoke).
- [ ] Add multi-robot and controller bring-up support only after the single-robot path is stable.

Deliverables:

- [x] Version-pinned runtime — **`config/runtime_manifest.yaml`** (referenced from `project.yaml`, Dockerfiles, compose files, `e2e/run_e2e.sh`, `requirements-bridge.txt`, and this doc); Dockerfiles remain the mechanical `FROM` source of truth.
- [x] Reproducible result metadata (hashes + versions in `result.yaml`; widen coverage over time).
- [x] Permission model — **`config/mcp_permissions.yaml`** + **`bridge/mcp_write_policy.py`** + **`tests/test_mcp_write_policy.py`** (filesystem roots + optional mutating-tool deny).
- [x] Structured JSONL — **`bridge/structured_log.py`** + **`tests/test_structured_log.py`**; E2E **`logs/structured.jsonl`**; runner per-run **`logs/structured.jsonl`**.
- [x] **Gazebo MCP stdio reconnect / session lifecycle** — bounded **session start** attempts (`BRIDGE_GAZEBO_MCP_SESSION_START_ATTEMPTS`, default **3**) and **read-only** `tools/call` transport retry after subprocess reconnect (`BRIDGE_GAZEBO_MCP_READONLY_TRANSPORT_RETRIES`, default **1** extra attempt). **Mutating** tools are **never** auto-retried. JSONL: `session_start`, `session_ready`, `session_error`, `reconnect_attempt`, `reconnect_success`, `reconnect_failed` via **`bridge/structured_log.log_gazebo_mcp_session_event`**. Tests: **`tests/test_gazebo_session_reconnect.py`**. UI: **`user_hint_for_gazebo_mcp_failure()`** short messages on **`GazeboResult`**. **Docker pytest 191 passed / 6 skipped**; **Docker E2E exit 0** (2026-05-11).
- [ ] Robust **full-stack** sim restart path (same item as container port / daemon scope above).
- [x] CI-ready test commands — pytest compose + E2E compose (Linux Docker engine).

Definition of done:

- Tests can be rerun reliably and produce explainable, comparable results.

### Gazebo MCP reconnect policy (Phase 6)

**Lifecycle (per `with GazeboSession()` block)**

1. **Session creation:** build argv (`GAZEBO_MCP_CMD`, `MCP_VENV`, or WSL/local venv), append JSONL `session_start`, spawn subprocess, wait for alive.
2. **First MCP call:** JSON-RPC `initialize` / `notifications/initialized`; on success append `session_ready`. On failure append `session_error`, stop subprocess, retry up to **`BRIDGE_GAZEBO_MCP_SESSION_START_ATTEMPTS`** (default **3**, max **8**) with capped exponential backoff (~0.35s × 2^n, max 2s).
3. **Tool failure:** JSON-RPC `tools/call` returns `error` or tool payload `isError` → **no transport reconnect** (application-level); existing `mcp_tool_call` / `mcp_tool_exception` JSONL remains.
4. **Process exit / broken pipe / timeout:** if the tool is **read-only** per **`config/mcp_permissions.yaml`**, up to **`BRIDGE_GAZEBO_MCP_READONLY_TRANSPORT_RETRIES`** (default **1**) **extra** `tools/call` attempts may run after **`reconnect_attempt`** → subprocess stop/start → **`reconnect_success`** or **`reconnect_failed`**.
5. **Reconnect or retry:** only **read-only** tools; **mutating** tools (`spawn_model`, `pause_simulation`, `reset_simulation`, …) are **not** retried (first attempt may have executed in Gazebo).
6. **Permanent failure:** exhausted start attempts or reconnect handshake fails → raise `RuntimeError`; public APIs wrap in **`GazeboResult`** with **`user_hint_for_gazebo_mcp_failure()`** text.

**Timeouts:** unchanged per public API (`wait_for_ready` delay/retries, `get_simulation_status(timeout=…)`, etc.); each `_MCPClient` still uses the session `timeout` for per-RPC recv.

**Operations intentionally not retried at transport layer:** any tool **not** in `gazebo_mcp_read_only_tools`, JSON-RPC/MCP **application** errors, permission denials (`MCPRepoReadDenied`, `MCPMutatingToolDenied`), and **full Docker / ROS daemon** restarts (separate Phase 6 stack item).

---

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

### Phase 7 Notes

- **Prerequisites:** Phase 6 batch runner, stable metrics in `result.yaml` or sidecar summaries, and deterministic seed handling in scenario schema (schema changes not yet defined).
- **Implementation sketch:** extend `runner/` with a `run-batch` mode (or external driver) that iterates scenario parameter grids, writes one directory per variant under `sim_runs/`, and aggregates SR/RTF/TC into a CSV or YAML summary for dashboards.

## Decisions Needed Before Coding

Historical checklist; most items are now **locked** for v1. Treat this as a decision log, not a blocking gate.

- [x] Is the first target single-user local development, team development, or CI automation? **Decision: single-user local development first.** CI automation is Phase 6.
- [x] Which environment is the first supported path: Docker, WSL2, native Linux, or Windows-native? **Decision: Windows-native FreeCAD + WSL2 + Docker for Gazebo/ROS 2.**
- [x] Which Gazebo release is the target? **Decision: dual track.** (1) **Reference for reproducible CI/E2E:** **Gazebo Harmonic** via `gz-harmonic` in **`docker/Dockerfile.e2e`**. (2) **Developer WSL path:** `Start-gz-sim.sh` builds **`src/3rdParty/gz-sim`** from source against OSRF packages (exact version follows the submodule + packages in the container — use `gz sim --version` when debugging drift).
- [x] Are v1 tests kinematic only, controller-in-the-loop, or both? **Decision: physics-style scenarios with a fixed assertion vocabulary in v1; controller-in-the-loop is Phase 4.5.** The runner already evaluates dynamics-related assertions (e.g. torque, collision) when telemetry is present.
- [x] What is the minimum useful v1 assertion set? **Decision: the seven types in Phase 4** (`reach_target_within`, `no_self_collision`, `max_joint_torque_below`, `sim_time_under`, `pose_within_tolerance`, `rtf_above`, `collision_count_below`), per `config/schemas/scenario.schema.yaml`.
- [x] Which ROS 2 MCP bridge is the first supported bridge? **Locked for v1:** **`ros-mcp` v3.0.1** at `tools/mcp/ros-mcp-server`. Phase 4.5 defines *how* scenarios command ROS 2.
- [x] Will Gazebo always run on the same machine as FreeCAD? **Decision: yes for v1.** Both on the same Windows host (Gazebo in WSL2/Docker, FreeCAD on Windows).
- [x] Are generated artifacts ignored, checked in, or stored through Git LFS? **Decision: `generated/` and `sim_runs/` are gitignored** (root `.gitignore`). Rebuild from sources; keep small hand-authored URDF/SDF under `robots/` / `worlds/` for smoke tests when needed.
- [ ] What write operations should the MCP client be allowed to perform? **Addressed (bridge)**: **`config/mcp_permissions.yaml`** + **`bridge/mcp_write_policy.py`** — approved roots **`sim_runs/`**, **`generated/`**; optional **`BRIDGE_MCP_DENY_MUTATING=1`**; upstream MCP servers unchanged.
- [x] Which FreeCAD MCP transport and port are canonical for this project? **Decision: XML-RPC on `localhost:9875`.** Confirmed from `freecad_client.py` and `rpc_server.py`.

## Immediate Next Tasks

1. ~~Confirm the environment target and write the setup decision down.~~ **Done** — Windows + WSL2 + Docker (see Phase 0 Environment Decision).
2. ~~Resolve the FreeCAD MCP addon transport and port mismatch in the docs.~~ **Done** — XML-RPC port 9875 confirmed.
3. **Highest leverage:** Add **`robots/arm_2dof.FCStd`** with Cross/RobotCAD entities and verify **`export_urdf` / RobotCAD** path (unblocks Phase 1 definition of done and honest E2E export).
4. Run **`scripts/install_robotcad_cross.ps1`**, open FreeCAD, enable RobotCAD/CROSS, and confirm demo/export (closes Phase 0 / Phase 1 human blockers).
5. ~~Draft the first `reach_top_shelf.yaml` scenario.~~ **Done** — `tests/scenarios/reach_top_shelf.yaml` (still needs a live run with exported robot).
6. On **Linux Docker engine**: keep **`docker compose -f docker/compose.e2e.yml up --build --abort-on-container-exit --exit-code-from e2e`** green after model/export changes.
7. Optionally run **WSL** headless Gazebo via **`Start-gz-sim.bat`** / `Start-gz-sim.sh` for parity with developer source-build path (long first build).
8. Start FreeCAD with MCP addon active; run **`python test_all_mcp.py`** to exercise **`get_object` / `get_view`** against live XML-RPC.
9. ~~Record friction from the manual flow.~~ **Done** for pre-live analysis — Phase 1 friction table; **refresh after** first full GUI + Gazebo run.

## Tracking Notes

- Keep the original plan as the architecture narrative.
- Use this file as the working task checklist.
- Promote repeated friction from Phase 1 into explicit Phase 2 implementation tasks.
- Keep generated artifacts and simulation run outputs reproducible from source inputs.
- Avoid expanding the assertion language too early; a small fixed vocabulary is easier to trust and debug.
- Keep the human path inside FreeCAD and the LLM path through MCP.
- Prefer coarse, typed MCP tools over many tiny tool calls.
- Treat ROS 2 control, VLM/image pipelines, randomized environments, and adaptive communication diagnostics as staged additions after the basic test rig works.

## Key repository paths (quick reference)

| Area | Path |
| --- | --- |
| Project manifest | `project.yaml` |
| **Structured JSONL (bridge / E2E / runner)** | `bridge/structured_log.py`; default file **`sim_runs/e2e_<UTC>/logs/structured.jsonl`** (E2E); **`sim_runs/<run_id>/logs/structured.jsonl`** (runner) |
| Schemas | `config/schemas/project.schema.yaml`, `config/schemas/scenario.schema.yaml` |
| Bridge / handoff | `bridge/` (`freecad_bridge.py`, `gazebo_bridge.py`, `gz_cli_bridge.py`, `handoff.py`, …) |
| Scenario runner | `runner/` |
| Iteration / sweeps | `iteration/` |
| Simulation Workbench addon | `addons/SimWorkbench/` (`panels/gazebo_status_panel.py`, `gazebo_status_logic.py`, …) |
| Example scenarios | `tests/scenarios/`, `tests/scenarios_e2e/e2e_smoke.yaml` |
| Robots / worlds | `robots/`, `worlds/` |
| Docker E2E | `docker/compose.e2e.yml`, `e2e/run_e2e.sh`, `e2e/bridge_gazebo_mcp_smoke.py`, `worlds/e2e_world.sdf`, `tools/mcp/gazebo-mcp/src/gazebo_mcp/gz_live_camera.py`, `tools/mcp/gazebo-mcp/src/gazebo_mcp/tools/sensor_tools.py` |
| Pytest in Docker | `docker/compose.pytest.yml` |
| MCP smoke (host) | `test_all_mcp.py` |
| RobotCAD install helper | `scripts/install_robotcad_cross.ps1` |
| Python deps (bridge) | `requirements-bridge.txt` |
