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

- [ ] Choose the first supported setup: RobotCAD Docker, WSL2 with Docker, native Linux, or another path.
- [ ] Document GUI forwarding approach for FreeCAD: X11, Wayland, VNC, or host-native FreeCAD.
- [ ] Install or build FreeCAD 1.x with RobotCAD/CROSS available.
- [ ] Verify RobotCAD opens in FreeCAD and its demo workflow works.
- [ ] Start modern Gazebo headless with `gz sim -s`.
- [ ] Install and run the selected FreeCAD MCP server, starting with `neka-nat/freecad-mcp`.
- [ ] Verify the actual FreeCAD MCP/addon RPC transport and port. Existing docs mention both `:5000` and `9875/9876`; resolve and record the real value.
- [ ] Verify an MCP client can create, inspect, and screenshot a simple FreeCAD object.
- [ ] Install and run the selected Gazebo MCP server, starting with `kvgork/gazebo-mcp`.
- [ ] Verify an MCP client can load a world, spawn or inspect a model, pause, resume, reset, and step headless Gazebo.
- [ ] Install a ROS 2 MCP option for later evaluation, but do not make it blocking for Phase 1.
- [ ] Document exact versions for FreeCAD, RobotCAD/CROSS, ROS 2, Gazebo, Python, MCP servers, Docker image, and host OS.

Deliverables:

- [ ] Reproducible environment notes.
- [ ] Minimal smoke-test command list.
- [ ] Confirmed MCP transport/port notes.
- [ ] Known setup issues and fixes.

Definition of done:

- FreeCAD, Gazebo, and the selected MCP servers can be controlled independently.
- No FreeCAD-to-Gazebo automation is required yet.

## Phase 1: Manual End-to-End

Goal: manually complete the full design-export-simulate path and capture every friction point.

Tasks:

- [ ] Pick one toy robot, such as a 2-DOF arm or simple rover.
- [ ] Model the robot in FreeCAD using RobotCAD/CROSS conventions.
- [ ] Define links, joints, limits, visuals, collisions, sensors, controllers, and inertias.
- [ ] Establish the unit and frame convention: FreeCAD mm, generated sim meters, +Z up, REP-103 naming where applicable.
- [ ] Assign materials and densities before inertia export.
- [ ] Generate simplified collision geometry instead of using visual meshes directly.
- [ ] Export the robot through RobotCAD/CROSS to URDF/SDF and ROS 2 package artifacts.
- [ ] Create or select one simple world.
- [ ] Load the exported robot into headless Gazebo.
- [ ] Run a short simulation and inspect pose, joint, sensor, contact, and RTF output.
- [ ] Record issues with units, coordinate frames, joint axes, mesh paths, materials, inertias, collisions, controllers, launch files, and Gazebo physics settings.
- [ ] Decide which manual steps must become automation in Phase 2.

Deliverables:

- [ ] Toy robot `.FCStd`.
- [ ] Generated URDF/SDF, meshes, and ROS 2 package output.
- [ ] One manually executed scenario.
- [ ] Friction list that becomes the real implementation spec.

Definition of done:

- A robot designed in FreeCAD runs in headless Gazebo.
- The manual process is documented well enough to repeat.

## Phase 2: Automated Bridge

Goal: automate the handoff between FreeCAD source files and headless Gazebo.

Tasks:

- [ ] Define the initial project repo layout:
  - `robots/`
  - `worlds/`
  - `generated/`
  - `tests/scenarios/`
  - `tests/assertions/`
  - `sim_runs/`
  - `config/`
  - `project.yaml`
- [ ] Draft the `project.yaml` schema.
- [ ] Draft the scenario YAML schema.
- [ ] Add or wrap `export_urdf(robot_name, out_dir)` in the FreeCAD MCP path.
- [ ] Add or wrap `export_sdf_world(world_name, out_dir)` in the FreeCAD MCP path.
- [ ] Add or wrap `compute_inertia(link_name, density)` and material checks.
- [ ] Add or wrap collision simplification checks.
- [ ] Implement a handoff helper that exports a robot or world and spawns it in Gazebo.
- [ ] Add export caching keyed by FreeCAD document hash, RobotCAD settings, material data, and export settings.
- [ ] Normalize mesh paths so generated artifacts are relocatable inside the repo.
- [ ] Keep MCP tools coarse-grained where possible to reduce LLM token/tool overhead.
- [ ] Write smoke tests for export, spawn, pause/resume/reset, and a short stepped run.

Deliverables:

- [ ] Project manifest schema.
- [ ] Scenario schema draft.
- [ ] MCP export tools.
- [ ] FreeCAD-to-Gazebo handoff helper.
- [ ] Export/spawn smoke tests.

Definition of done:

- An MCP client can export from FreeCAD and spawn into Gazebo without manual file copying.

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

- [ ] Is the first target single-user local development, team development, or CI automation?
- [ ] Which environment is the first supported path: Docker, WSL2, native Linux, or Windows-native?
- [ ] Which Gazebo release is the target: Harmonic, Ionic, or another modern gz-sim release?
- [ ] Are v1 tests kinematic only, controller-in-the-loop, or both?
- [ ] What is the minimum useful v1 assertion set?
- [ ] Which ROS 2 MCP bridge is the first supported bridge?
- [ ] Will Gazebo always run on the same machine as FreeCAD?
- [ ] Are generated artifacts ignored, checked in, or stored through Git LFS?
- [ ] What write operations should the MCP client be allowed to perform?
- [ ] Which FreeCAD MCP transport and port are canonical for this project?

## Immediate Next Tasks

1. Confirm the environment target and write the setup decision down.
2. Resolve the FreeCAD MCP addon transport and port mismatch in the docs.
3. Run the RobotCAD demo export into headless Gazebo.
4. Install and smoke-test the FreeCAD MCP server.
5. Install and smoke-test the Gazebo MCP server.
6. Build or choose the toy robot for the first manual end-to-end scenario.
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
