# FreeCAD Gazebo MCP Task Breakdown

Source plan: [freecad_gazebo_mcp_plan.md](freecad_gazebo_mcp_plan.md)

Last reviewed: 2026-05-08

## Purpose

This document turns the integration plan into an actionable task list. The goal is to build a robot test rig where FreeCAD is the human-facing UI, Gazebo runs headless as the physics backend, and MCP exposes both sides to an LLM-driven workflow.

The highest-value outcome is not the MCP plumbing by itself. The core product is a repeatable robot simulation and regression-test workflow:

1. Design or modify a robot in FreeCAD.
2. Export the robot and world to generated simulation artifacts.
3. Run headless Gazebo scenarios.
4. Evaluate assertions.
5. Show results and live simulation state inside FreeCAD.

## Working Assumptions

- FreeCAD `.FCStd` files are the source of truth.
- Gazebo consumes generated URDF/SDF and mesh files only.
- The human user stays inside FreeCAD.
- Gazebo runs headless with `gz sim -s`.
- ROS 2 is used because RobotCAD and the Gazebo MCP path expect it.
- The RobotCAD Docker image is the preferred first environment.
- Generated exports and simulation run outputs are reproducible artifacts.
- Scenario tests live in the project repo next to the models.

## Milestones

| Milestone | Target Outcome | Depends On |
| --- | --- | --- |
| Phase 0: Environment | FreeCAD MCP and Gazebo MCP both work independently | Docker or local ROS 2 setup |
| Phase 1: Manual End-to-End | A toy robot exports from FreeCAD and simulates in headless Gazebo | RobotCAD export path |
| Phase 2: Automated Bridge | MCP tools can export, spawn, and run basic handoff flows | Phase 1 friction list |
| Phase 3: Simulation Workbench Viewer | FreeCAD animates live Gazebo state without opening Gazebo GUI | Stable Gazebo lifecycle and state topics |
| Phase 4: Test Runner | Scenarios run headlessly and produce pass/fail results | Scenario schema and assertion vocabulary |
| Phase 5: Iteration Loops | LLM can change design parameters and rerun tests | Reliable export cache and test runner |
| Phase 6: Hardening | The system is reproducible, permissioned, and robust | Earlier phases complete |

## Phase 0: Environment

Goal: prove the two runtime sides work before connecting them.

Tasks:

- [ ] Choose setup target: RobotCAD Docker, WSL2, native Linux, or another supported path.
- [ ] Install or build a FreeCAD 1.x environment that can load RobotCAD.
- [ ] Install RobotCAD and verify its demo workflow opens in FreeCAD.
- [ ] Start modern Gazebo headless with `gz sim -s`.
- [ ] Install and run the selected FreeCAD MCP server, starting with `neka-nat/freecad-mcp`.
- [ ] Verify an MCP client can create or inspect a simple FreeCAD object.
- [ ] Install and run the selected Gazebo MCP server, starting with `kvgork/gazebo-mcp`.
- [ ] Verify an MCP client can spawn or inspect a simple model in headless Gazebo.
- [ ] Document exact versions for FreeCAD, RobotCAD, ROS 2, Gazebo, Python, and MCP servers.

Deliverables:

- [ ] Reproducible environment notes.
- [ ] Minimal smoke-test command list.
- [ ] Known setup issues and fixes.

Definition of done:

- FreeCAD and Gazebo can both be controlled independently.
- No FreeCAD-to-Gazebo automation is required yet.

## Phase 1: Manual End-to-End

Goal: manually complete the full design-export-simulate path and capture every friction point.

Tasks:

- [ ] Pick one toy robot, such as a 2-DOF arm.
- [ ] Model the robot in FreeCAD using RobotCAD conventions.
- [ ] Define links, joints, limits, visuals, collisions, sensors, and inertias where applicable.
- [ ] Export the robot to URDF or SDF through RobotCAD.
- [ ] Create or select one simple world.
- [ ] Load the exported robot into headless Gazebo.
- [ ] Run a short simulation and inspect pose, joint, and sensor output.
- [ ] Record issues with units, coordinate frames, joint axes, mesh paths, materials, inertias, collisions, and launch files.
- [ ] Decide which manual steps must become automation in Phase 2.

Deliverables:

- [ ] Toy robot `.FCStd`.
- [ ] Generated URDF/SDF and mesh output.
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
  - `project.yaml`
- [ ] Draft the `project.yaml` schema.
- [ ] Draft the scenario YAML schema.
- [ ] Add or wrap `export_urdf(robot_name, out_dir)` in the FreeCAD MCP path.
- [ ] Add or wrap `export_sdf_world(world_name, out_dir)` in the FreeCAD MCP path.
- [ ] Implement a handoff helper that exports a robot or world and spawns it in Gazebo.
- [ ] Add export caching keyed by FreeCAD document hash or timestamp plus export settings.
- [ ] Normalize mesh paths so generated artifacts are relocatable inside the repo.
- [ ] Write smoke tests for export and spawn.

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
- [ ] Build a shared Gazebo transport layer used by both the workbench and MCP tooling.
- [ ] Implement Gazebo process lifecycle controls: start, pause, resume, step, reset, stop, and reload.
- [ ] Implement the Live State Bridge:
  - subscribe to Gazebo pose and joint-state topics,
  - translate simulation state into FreeCAD object placements,
  - update FreeCAD through the GUI-safe Qt path,
  - throttle or sample updates to keep the UI responsive.
- [ ] Add a Sim Controls panel with play, pause, step, reset, reload, sim time, and real-time factor.
- [ ] Add a Scenario Picker panel for robot, world, initial pose, and saved scenario selection.
- [ ] Add a Camera Viewer panel for image topics.
- [ ] Add basic Sensor Plots for joint state, IMU, contact force, or other first sensors.
- [ ] Add a Run Library panel that lists prior `sim_runs/`.
- [ ] Add an MCP Activity Log panel for debugging agent actions.
- [ ] Verify the human can run and watch a simulation in FreeCAD without opening Gazebo GUI.

Deliverables:

- [ ] Simulation Workbench addon.
- [ ] Live State Bridge.
- [ ] Basic controls and viewer panels.
- [ ] Shared Gazebo integration library.

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
- [ ] Implement scenario loading from `tests/scenarios/`.
- [ ] Implement single-test execution.
- [ ] Implement run-all execution.
- [ ] Evaluate assertions from Gazebo state, ROS 2 topics, or recorded outputs.
- [ ] Write `sim_runs/<timestamp>_<scenario>/result.yaml`.
- [ ] Include input hashes and version metadata in each result file.
- [ ] Add a pass/fail dashboard to the Simulation Workbench.
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

## Phase 5: Iteration Loops

Goal: support agent-driven design changes and repeated simulation checks.

Tasks:

- [ ] Add a stable `set_parameter(doc, name, value)` flow for controlled FreeCAD edits.
- [ ] Add a loop that changes a parameter, recomputes, exports, runs a scenario, and reports results.
- [ ] Capture screenshots, plots, and selected sensor summaries for LLM review.
- [ ] Add parameter sweep support for one or more numeric design variables.
- [ ] Add failure summarization that points back to the relevant scenario assertion.
- [ ] Decide when design changes should be automatically committed.

Deliverables:

- [ ] Parameter iteration flow.
- [ ] Sensor and result summaries.
- [ ] Optional parameter sweep runner.

Definition of done:

- An LLM can make a bounded design change, rerun a failing scenario, and report whether the change improved the result.

## Phase 6: Hardening

Goal: make the system trustworthy enough for repeated use.

Tasks:

- [ ] Pin FreeCAD, RobotCAD, ROS 2, Gazebo, and MCP server versions.
- [ ] Add input hashes to generated outputs and simulation results.
- [ ] Separate read-only and write-capable MCP tools.
- [ ] Add permission prompts or policy controls for write operations.
- [ ] Improve Gazebo restart behavior to avoid stale processes, port conflicts, and ROS 2 daemon issues.
- [ ] Add structured logging across FreeCAD workbench actions, MCP calls, exports, and sim runs.
- [ ] Add collision mesh simplification, likely V-HACD or an equivalent workflow.
- [ ] Add materials and density management for accurate inertias.
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

## Decisions Needed Before Coding

- [ ] Is the first target single-user local development, team development, or CI automation?
- [ ] Which environment is the first supported path: Docker, WSL2, native Linux, or Windows-native?
- [ ] Are v1 tests kinematic only, controller-in-the-loop, or both?
- [ ] What is the minimum useful v1 assertion set?
- [ ] Will Gazebo always run on the same machine as FreeCAD?
- [ ] Are generated artifacts ignored, checked in, or stored through Git LFS?
- [ ] What write operations should the MCP client be allowed to perform?

## Immediate Next Tasks

1. Confirm the environment target and write the setup decision down.
2. Run the RobotCAD demo export into headless Gazebo.
3. Install and smoke-test the FreeCAD MCP server.
4. Install and smoke-test the Gazebo MCP server.
5. Build or choose the toy robot for the first manual end-to-end scenario.
6. Draft the first `reach_top_shelf.yaml` scenario.
7. Record all friction from the manual flow before writing bridge automation.

## Tracking Notes

- Keep the original plan as the architecture narrative.
- Use this file as the working task checklist.
- Promote repeated friction from Phase 1 into explicit Phase 2 implementation tasks.
- Keep generated artifacts and simulation run outputs reproducible from source inputs.
- Avoid expanding the assertion language too early; a small fixed vocabulary is easier to trust and debug.
