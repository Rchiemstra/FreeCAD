# Plan: FreeCAD ↔ Gazebo Integration via MCP

**Goal:** Build a system for **testing robot designs in simulated environments**, where FreeCAD is the *only* UI the human ever sees (Fusion-360 style — switch a workbench, switch the workspace) and Gazebo is a **headless physics backend**. Both programs are also exposed to an LLM through MCP, so an agent can design, export, simulate, and assert against test scenarios.

The product is not "a CAD tool" or "a simulator" — it's **a test rig for robots**, with FreeCAD as the cockpit.

---

## 1. Vision in One Picture

```
                  ┌─────────────────────────┐
                  │  LLM Client (Claude)    │
                  └──────────┬──────────────┘
                             │ MCP
              ┌──────────────┴──────────────┐
              │                             │
     ┌────────▼─────────┐         ┌─────────▼────────────┐
     │  FreeCAD MCP     │         │   Gazebo MCP         │
     │  (design + repo) │         │   (headless sim)     │
     └────────┬─────────┘         └─────────┬────────────┘
              │ XML-RPC                      │ gz transport / ROS 2
   ┌──────────▼───────────────┐    ┌─────────▼────────┐
   │  FreeCAD GUI             │    │  gz-sim -s       │
   │  ┌─────────────────────┐ │    │  (no window)     │
   │  │ Modeling workbench  │ │    └─────────┬────────┘
   │  │ (Part / RobotCAD)   │ │              │
   │  ├─────────────────────┤ │◄─────────────┘ pose / sensors
   │  │ Simulation workbench│ │              (30 Hz live updates
   │  │ + Test Runner       │ │               into FreeCAD's view)
   │  └─────────────────────┘ │
   └────────┬─────────────────┘
            │     ▲
            │     │
            ▼     │ tests + results
   ┌──────────────────────────────────────┐
   │  Repo (Git)                          │
   │  robots/  worlds/  generated/        │
   │  tests/   sim_runs/                  │
   └──────────────────────────────────────┘

       Human user ⇄ FreeCAD only.
       LLM ⇄ both MCPs.
       gz-sim never shows a window.
```

**FreeCAD holds the canonical model** (`.FCStd`). **Gazebo only ever sees generated** URDF/SDF + meshes. **Tests live next to the models** in the same repo. The Simulation Workbench animates FreeCAD's own 3D view from live gz-sim data — there is no separate Gazebo window for the human to look at.

---

## 2. Existing Building Blocks (Reuse These)

You don't need to build any of this from zero.

**FreeCAD-side MCP servers (pick one as a base):**
- **neka-nat/freecad-mcp** — most mature. Two-process design: a standalone Python MCP server (started via `uvx`) talks XML-RPC to an addon running *inside* FreeCAD. Recommended starting point.
- **bonninr/freecad_mcp** — similar concept, simpler bridge.
- **contextform/freecad-mcp** — bundles an "AICopilot" workbench installer.

**FreeCAD → URDF/SDF export:**
- **RobotCAD (a.k.a. OVERCROSS)** — by far the most complete option. A FreeCAD workbench that lets you define links, joints, collisions, visuals, sensors, controllers, and exports a full ROS 2 description package with launchers for Gazebo and RViz. Ships with a Docker image that includes ROS 2 + Gazebo. **This is your export backbone.**
- *Alternatives if RobotCAD doesn't fit:* `Dave-Elec/freecad_to_gazebo` (CLI exporter), `maidenone/RobotCreator` (early dev).

**Gazebo-side MCP servers:**
- **kvgork/gazebo-mcp** — ROS 2 MCP server for Gazebo: spawn/despawn models, set/get model state, sensor data streaming (camera, lidar, IMU, etc.), pause/resume/reset, world properties. Has working examples.
- *Alternative:* `ros-mcp-server` — generic ROS 2 bridge over rosbridge WebSocket; useful if you want full ROS 2 access (topics, services, actions), not just Gazebo.

**Implication:** Phase 1 is mostly **plumbing** existing tools. The novel work is the **Simulation Workbench inside FreeCAD** (which doesn't exist yet) and the **Test Runner** that drives scenarios against assertions.

---

## 3. Architecture Decisions

A few choices to lock in before coding:

**3.1 Two MCP servers vs. one unified server.**
**Two servers**, with the LLM as orchestrator. Each program has its own runtime, failures stay isolated, mirrors how MCP is designed, and the LLM is already a perfectly good orchestrator. The MCP path is for the *agent*; the *human* path stays inside FreeCAD and does not need MCP at all.

**3.2 Gazebo flavor: classic vs. modern (gz-sim), and headless.**
Use **modern Gazebo (gz-sim, formerly Ignition)**, and run it **headless** (`gz sim -s`). Gazebo Classic is end-of-life. The Gazebo *window* is unnecessary because the human watches the simulation in FreeCAD's own 3D view (see §3.6). Keep the GUI mode as a debugging fallback only.

**3.3 ROS 2: required or optional?**
Strongly recommended. RobotCAD exports ROS 2 packages, and `gazebo-mcp` is ROS 2-based. Without ROS 2 you'd drop down to raw `gz` CLI / transport library — works, but more painful. Run everything in the **RobotCAD Docker image** to skip ROS 2 install pain.

**3.4 Units & coordinate frames.**
The silent killer. Standardize early:
- FreeCAD default: **mm**, +Z up, body-fixed frames per Assembly.
- URDF/SDF: **meters**, +Z up, REP-103 conventions (X-forward, Y-left).
- Pick a convention in your custom workbench and enforce it on every export. RobotCAD handles most of this — verify before customizing.

**3.5 Repo layout (FreeCAD as source of truth + tests as first-class).**
```
project_root/
├── .git/
├── robots/
│   ├── arm_v3.FCStd          ← source of truth (mechanical design)
│   └── arm_v3.FCStd.bak
├── worlds/
│   └── kitchen_v1.FCStd      ← environments are FreeCAD too
├── generated/                ← gitignored or LFS-tracked, rebuildable
│   ├── arm_v3/
│   │   ├── urdf/arm_v3.urdf
│   │   ├── meshes/*.stl
│   │   └── ros2_pkg/
│   └── kitchen_v1/world.sdf
├── tests/                    ← THE POINT OF THE WHOLE SYSTEM
│   ├── scenarios/
│   │   ├── reach_top_shelf.yaml
│   │   ├── pick_and_place_block.yaml
│   │   └── obstacle_avoidance.yaml
│   └── assertions/           ← optional; can also be inline in scenarios
├── sim_runs/                 ← test results, logs, sensor recordings
│   └── 2026-05-07_142301_reach_top_shelf/
│       ├── result.yaml       ← pass/fail + metrics
│       ├── ros2bag/
│       └── camera_frames/
└── project.yaml              ← repo manifest
```

`.FCStd` files and `tests/` are the only hand-edited artifacts. `generated/` and `sim_runs/` are derivative.

A scenario YAML, sketched:
```yaml
name: reach_top_shelf
robot: arm_v3
world: kitchen_v1
initial_pose: { x: 0, y: 0, z: 0, yaw: 0 }
goal:
  type: ee_pose
  target: { x: 0.6, y: 0.0, z: 1.8 }
  tolerance: 0.05
duration: 15.0
assertions:
  - type: reach_target_within
    seconds: 10
  - type: no_self_collision
  - type: max_joint_torque_below
    value: 25.0
```

**3.6 FreeCAD as the only UI (Fusion-style workspaces).**
The human never opens Gazebo. FreeCAD's workbenches act as Fusion's workspaces:

| Fusion workspace | FreeCAD equivalent |
|---|---|
| Design | Part / Part Design / Assembly |
| Robot description | RobotCAD |
| Simulation | **The new Simulation Workbench (this project)** |

Switching workbench is one click. The 3D document stays open. During simulation, a **Live State Bridge** subscribes to gz-sim's pose/joint topics and updates FreeCAD object Placements at ~30 Hz — the same robot model you designed becomes the simulation viewer. Camera and sensor data render in Qt panels next to the 3D view.

---

## 4. The MCP Tool Surfaces

These exist for the **LLM-driven** flow. The human-driven flow goes through the Simulation Workbench buttons directly, bypassing MCP entirely (though they share the same underlying transport into gz-sim).

**4.1 FreeCAD MCP (extends `neka-nat/freecad-mcp`):**

| Tool | Purpose |
|------|---------|
| `list_projects` | Enumerate `.FCStd` files in repo |
| `open_document(path)` | Load an FCStd into FreeCAD GUI |
| `get_object_tree(doc)` | Inspect model hierarchy |
| `set_parameter(doc, name, value)` | Edit a parametric dimension |
| `compute_inertia(link_name, density)` | Use FreeCAD's built-in mass/inertia |
| `add_joint(parent, child, type, axis, limits)` | Build kinematic chain |
| `export_urdf(robot_name, out_dir)` | Trigger RobotCAD export |
| `export_sdf_world(world_name, out_dir)` | Export environment as SDF |
| `screenshot(view, path)` | Visual feedback for the LLM |
| `git_commit(message)` | Snapshot the design state |
| `list_tests` / `run_test(name)` | Drive a scenario from the LLM side |

**4.2 Gazebo MCP (use `kvgork/gazebo-mcp`, extend as needed):**

| Tool | Purpose |
|------|---------|
| `load_world(sdf_path)` | Boot Gazebo with a world |
| `spawn_model(urdf_or_sdf, name, pose)` | Drop a robot in |
| `set_model_state(name, pose, twist)` | Teleport / set velocities |
| `get_model_state(name)` | Query pose / velocities |
| `step_sim(seconds)` / `pause` / `resume` / `reset` | Control time |
| `list_sensors(model)` / `get_sensor_data(name)` | Read camera / lidar / IMU |
| `apply_wrench(link, force, torque)` | Disturbance testing |
| `record(start/stop, path)` | Save sim logs |

**4.3 Glue at the LLM layer.**
The LLM composes flows like *"shorten the arm 5 cm, re-export, run the reach_top_shelf test, and report whether it passes."* No orchestrator daemon needed.

---

## 5. The Simulation Workbench

This is the **main novel piece** of the project. A FreeCAD addon that turns the application into a robot test rig. Sub-components (each a Qt panel or 3D-view integration):

- **Sim Controls** — Play / Pause / Step / Reset / Reload buttons, sim time + RTF display.
- **Scenario Picker** — choose world + robot + initial pose, or load a saved scenario from `tests/scenarios/`.
- **Test Runner** — *the most important panel.* Reads `tests/`, runs scenarios in headless gz-sim, evaluates assertions, writes `sim_runs/<name>/result.yaml`, shows a pass/fail dashboard. Single-test mode and "run all" mode.
- **Live State Bridge** — gz-transport subscriber (background thread). Translates pose / joint state messages into FreeCAD Placement updates at ~30 Hz so the FreeCAD 3D view animates the robot.
- **Camera Viewer** — Qt label widget that decodes incoming Image messages and displays them. One panel per camera sensor.
- **Sensor Plots** — strip charts for joint positions, IMU, contact forces, battery, etc.
- **Run Library** — browse past `sim_runs/` with thumbnails, links to logs and bags.
- **Project Browser** — tree of robots, worlds, scenarios in the repo. Double-click to open in the corresponding workbench.
- **Diff view** — compare current `.FCStd` vs. last committed.
- **MCP Activity Log** — show recent MCP calls (incoming and outgoing) for debugging and trust.

The workbench **shares plumbing with the Gazebo MCP** — both call gz-transport / ROS 2 underneath. Don't duplicate the gz-sim integration; factor it into a shared internal library that both the workbench panels and the MCP server use.

---

## 6. Phased Roadmap

**Phase 0 — Environment (1 week)**
- Install FreeCAD 1.x + RobotCAD via Docker.
- Get gz-sim running headless in the same Docker.
- Install `neka-nat/freecad-mcp` and `kvgork/gazebo-mcp`.
- Smoke test: ask Claude (via MCP) to create a box in FreeCAD; separately, spawn a TurtleBot in headless gz-sim. **Both sides working independently** is the goal.

**Phase 1 — Manual end-to-end (2 weeks)**
- Build one toy robot in FreeCAD using RobotCAD conventions.
- Export to URDF, manually load into headless gz-sim, simulate.
- Document every friction point: units, joint frames, mesh paths, missing inertias. This is your real spec.

**Phase 2 — Automated bridge (2–3 weeks)**
- Add `export_urdf` / `export_sdf_world` tools to the FreeCAD MCP that wrap RobotCAD's CLI/Python API.
- Add a `freecad_to_gazebo_handoff` helper: one MCP tool that does export + spawn in one shot.
- Pin down the repo layout from §3.5, write the `project.yaml` and scenario YAML schemas.

**Phase 3 — Simulation Workbench, viewer side (3–4 weeks)**
- Build the Live State Bridge: animate FreeCAD objects from gz-transport pose updates.
- Add Sim Controls, Camera Viewer, Sensor Plots panels.
- Workbench switching feels like Fusion: Modeling ⇄ Simulation in one click.
- *Outcome: a human can design in FreeCAD, click Play, and watch their robot move inside FreeCAD's window with no Gazebo GUI ever opening.*

**Phase 4 — Test Runner (3–4 weeks) — the actual goal**
- Define scenario + assertion schema.
- Implement Test Runner panel: load `tests/scenarios/`, run them sequentially in headless gz-sim, evaluate assertions, write results.
- "Run all" mode produces a summary dashboard.
- LLM can drive it via `run_test` / `list_tests` MCP tools.
- *Outcome: regression-test a robot design.*

**Phase 5 — Iteration loops (ongoing)**
- Closed-loop scenarios: LLM modifies FreeCAD parameter → re-exports → re-runs failing tests → decides next change.
- Sensor data pipeline back to LLM (camera frames, summarized lidar, plot screenshots).
- Optional: parameter sweeps / "design space exploration" runs.

**Phase 6 — Hardening**
- Permissioning (which MCP tools are write vs. read-only).
- Reproducibility: pin Gazebo + RobotCAD versions in Docker, hash inputs into `result.yaml`.
- Multi-robot scenarios, ROS 2 controller bring-up automation.

---

## 7. Key Technical Risks

1. **Joint frame mismatches** between FreeCAD assemblies and URDF — RobotCAD helps but expect a long tail of edge cases.
2. **Inertia accuracy** — FreeCAD computes mass properties only with correct per-body density. Establish a materials library early.
3. **Collision meshes** — exported visual STLs are usually too high-poly for physics. Plan a V-HACD step (RobotCAD references this).
4. **Live animation performance** — pushing pose updates from a background thread into FreeCAD's main thread at 30+ Hz without freezing the UI is delicate. Use Qt signals/slots and `App.GuiUp` checks; consider sub-sampling for heavy scenes.
5. **Camera streaming into Qt** — image decode/resize/blit per frame can drop frames. Use `QImage` with native byte order; consider a separate worker thread.
6. **Headless Gazebo lifecycle** — start/stop/reload from a button is simple in theory and finicky in practice (zombie processes, port reuse, ROS 2 daemon state). Design a robust restart path early.
7. **Assertion language** — start with a small, fixed vocabulary (reach-target, no-collision, max-torque, sim-time-under). Resist building a general DSL.
8. **MCP latency** — running export every prompt is slow. Cache exports keyed on FreeCAD document hash.
9. **State sync direction** — easy to push design → sim, hard to pull sim state back into FreeCAD as edits. Keep this out of v1.

---

## 8. Open Questions to Decide First

Before writing code:

1. **Single user or team?** Affects whether the repo is local Git or hosted (and whether MCP servers are local-only or networked).
2. **Which OS / setup?** Native Linux vs. Docker vs. WSL2 — RobotCAD Docker is the safest path on any host.
3. **Scope of "test"** — kinematic-only checks (does the arm reach?), or full controller-in-the-loop with ros2_control? The latter is much more useful but adds setup cost.
4. **Assertion vocabulary** — what's the v1 set? See §7.7. Lock this before Phase 4.
5. **Who is the primary "user"?** A human in FreeCAD using the LLM as copilot, an autonomous agent running scenarios in CI, or both? Affects UI emphasis.
6. **Will Gazebo run on the same machine as FreeCAD?** If not, MCP servers and the Live State Bridge need network configuration.

---

## 9. First Concrete Tickets

If you want to start tomorrow:

1. Spin up the RobotCAD Docker image; export their demo robot to **headless** gz-sim and verify it simulates.
2. Install `neka-nat/freecad-mcp` against the same FreeCAD; verify Claude can drive it.
3. Install `kvgork/gazebo-mcp` against the same headless Gazebo; verify Claude can drive it.
4. Write a 1-page "ADR" (architecture decision record) answering §8.
5. Pick one toy robot (e.g., 2-DOF arm) and one scenario (`reach_top_shelf.yaml`) and walk it through Phase 1 end-to-end manually. The friction list is your real spec.
6. Sketch the scenario YAML schema and the v1 assertion vocabulary on one page.

---

*The product is a robot test rig with FreeCAD as the cockpit. Everything else — the MCPs, the export pipeline, the headless Gazebo — is plumbing that serves that goal. When in doubt about a design choice, ask: does this make tests easier to write, run, or trust?*
