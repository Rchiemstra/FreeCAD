# FreeCAD Reality-Aware Autonomous Engineering Architecture Plan

## 1. Purpose

This document defines the target end architecture for extending FreeCAD into a reality-aware engineering environment in which a human and an AI agent can reason about, simulate, modify, and verify the same engineered world.

The goal is not merely to let an AI operate CAD commands. The goal is to let an agent act as an engineer inside a controlled world that connects:

- the authoritative FreeCAD design state;
- assembly structure and semantics;
- observed reality, independently of design;
- a named World Configuration (which world is being considered);
- deterministic Simulation Episodes (how that world evolves through time);
- World State Frames (physical state at each instant `t`);
- agent reasoning and proposed actions;
- the existing collaboration and mutation-control architecture.

The central architectural idea is the experiment chain:

```text
World Configuration
        │
        ▼
Immutable Snapshot
        │
        ▼
Simulation Episode
        │
        ├── Frame(t₀)
        ├── Frame(t₁)
        ├── Frame(t₂)
        ├── ...
        └── Frame(tₙ)
        │
        ▼
Predictions
```

The architect-reviewed overview of the full engineering loop is §5. Lower-level sections unpack boxes. They must not mutate that overview into implementation machinery.

The agent must never be allowed to collapse these states into one another.

---

## 2. Foundation

This architecture is built on top of:

```text
integrate/change-aware-save-mcp-autonomy
```

and assumes the already delivered collaboration architecture remains authoritative for live FreeCAD mutation.

Existing responsibilities that must remain intact include:

- `DocumentCollaborationService` as the typed model-intent facade;
- `DocumentCommitCoordinator` as the sole supported owner of live model transactions;
- detached lightweight preparation;
- isolated heavy geometry work;
- immutable dependency capture;
- lifecycle and revision revalidation before commit;
- stale-result rejection;
- coordinated recompute;
- conflict-neutral personal GUI state;
- change-aware save.

The reality/simulation architecture extends this foundation. It does not create a second live mutation path.

---

## 3. End Goal

The completed system shall allow an agent to perform a loop such as:

```text
Goal:
    Close the curtain in < 8 s
    Motor current < 1.8 A
    No interference
    Required safety factor >= 2

Agent:
    inspect current design
        ↓
    understand assembly and physical semantics
        ↓
    inspect observations of the manufactured system
        ↓
    create candidate design change
        ↓
    simulate candidate
        ↓
    inspect motion, torque, contact and stress
        ↓
    reject or refine candidate
        ↓
    submit a typed mutation proposal
        ↓
    revision/lifecycle revalidation
        ↓
    commit through DocumentCommitCoordinator
        ↓
    recompute and verify
        ↓
    compare prediction with observed reality
        ↓
    update model confidence/calibration
```

This is the difference between:

```text
AI operates CAD
```

and:

```text
AI engineers inside a modeled world
```

---

## 4. Architectural Invariants

The following rules are non-negotiable.

### A1. Design state is not simulation state

```text
DesignState != SimulationState
```

Simulation must operate on immutable captured state. It must never mutate the live design directly.

### A2. Designed state is not observed physical state

```text
DesignedState != ObservedState
```

A CAD hole of 3.00 mm and a measured printed hole of 3.14 mm are different facts and must remain separately representable.

### A3. Simulation state is not observed state

```text
SimulationState != ObservedState
```

Prediction and measurement may be compared, but one cannot silently replace the other.

### A4. The live `App::Document` remains authoritative for engineered design state

There must be exactly one supported path for live model mutation.

Authoritative Design is `App::Document` at a `DesignRevision`. Observations are part of the world, not design state. `DesignRevision` is metadata of `App::Document`, published on successful DCC commit. It is not a downstream box named World Revision.

### A5. Every action is bound to the world it considered

Every query, simulation, proposal, and commit-sensitive decision must cite the world on which it was based.

A single scalar `WorldRevision` is retired as the name for that composition. Older drafts used it as both the design clock and the world clock. The composition is now **World Configuration** / `WorldConfigurationId`. `DesignRevision` is not that composition.

Stale rejection is split. Do not use one `proposal.sourceRevision != currentWorldRevision` test for all artifacts.

- **Design mutation proposals** bind `DesignRevision` (and entity identities / expected per-object revisions, as DCC already does). If the live design has moved, the proposal is DCC-stale and must be revalidated, regenerated, or rejected.
- **Evidence-based proposals** also bind `WorldConfigurationId` and/or `SimulationEpisodeId`. If observations, calibration, or episode evidence moved, the proposal is engineering-stale even when `DesignRevision` still matches.
- **Snapshots** bind source World Configuration and the `DesignRevision` at capture.
- **Episodes and results** bind `SnapshotId`, the source World Configuration, and the World State Frames they produced.

A camera reading must not bump `DesignRevision`. High-rate observations that have not been committed into the observation store are not a new World Configuration.

### A6. Simulation never commits

Simulation output may produce evidence or proposals only. Simulation never writes live `Placement` or the live document.

### A7. The agent never bypasses the mutation coordinator

All agent-originated model changes flow through typed model intent and the existing collaboration/commit architecture.

### A8. Observations carry provenance and uncertainty

A measured value without origin, timestamp, units, and uncertainty is not a reliable world fact.

### A9. Object identity survives representation boundaries

The same engineered entity must remain traceable across:

- FreeCAD object;
- assembly occurrence;
- semantic object;
- captured snapshot;
- simulation body;
- FEM entity;
- observation;
- agent reference;
- World State Frame;
- result playback.

### A10. Results are reproducible

A simulation result must be attributable to an immutable input identity, solver configuration, environment, controls, and code/runtime identity.

---

## 5. Target End Architecture

This section is the architect-reviewed overview. Keep it at this altitude. Lower-level sections unpack boxes; they must not add engines, stores, or GUI paths onto this picture.

### Terminology freeze

`WorldRevision` as a single world clock, and the older use of **World State Frame** as the revision-tuple composition, are retired.

| Concept | Meaning |
|---|---|
| **World Configuration** | Which design, observations, environment, calibration and candidate are being considered |
| **Immutable Snapshot** | Frozen solver-ready representation of that configuration |
| **World State Frame** | Complete physical state at one instant `t` |
| **Simulation Episode** | Evolution of those states through time |
| **Predictions** | Engineering information derived from the episode |

There is then no collision:

```text
World Configuration = what world are we simulating?
World State Frame   = what is the state of that world at time t?
Simulation Episode  = how does that state evolve from t₀ → tₙ?
```

World Configuration may include an optional Candidate overlay (empty overlay = current committed world).

World State Frame is physical/simulation state (poses, velocities, contacts, and other complete-state quantities) at one instant. It is not `App::Document`, not a snapshot dump of the CAD document, and not the composition of revision IDs.

### Overview

```text
                              Human / Agent
                                   │
                         goals / observations
                                   │
                                   ▼
                         Semantic World API
                                   │
                    ┌──────────────┴──────────────┐
                    ▼                             ▼
          Authoritative Design              Observed Reality
          App::Document                     measurements
          @ DesignRevision                  sensors / vision
                    │                       @ ObservationRevision
                    │                             │
                    └──────────────┬──────────────┘
                                   ▼
                          World Configuration
                    design + observations + environment
                         + calibration + candidate
                                   │
                                   ▼
                         Immutable Snapshot
                       frozen simulation input
                                   │
                                   ▼
                              Preflight
                                   │
                                PASS?
                                   │
                                   ▼
                         Simulation Episode
                                   │
                                   ▼
                       World State Frame t₀
                                   │
                             physics Δt
                                   ▼
                       World State Frame t₁
                                   │
                             physics Δt
                                   ▼
                       World State Frame t₂
                                   │
                                  ...
                                   │
                             physics Δt
                                   ▼
                       World State Frame tₙ
                                   │
                                   ▼
                              Predictions
                                   │
                    ┌──────────────┴──────────────┐
                    ▼                             ▼
              Goal Comparator              Reality Comparator
           prediction vs intent        prediction vs observation
                    │                             │
                    ▼                             ▼
              Design Proposal              Calibration Proposal
                    │                             │
                    ▼                             ▼
          Mutation Coordinator          Calibration Model
              DCS → DCC
                    │
                    ▼
          Authoritative Design
          App::Document
          @ DesignRevision + 1
                    │
                    └──────────────► new World Configuration
```

### What the picture is allowed to leave implicit

- **Human vs agent.** Human → GUI + Semantic World API. Agent → Semantic World API. Both produce the same typed model intent. Agents do not drive the GUI. The GUI stays off this overview. The Semantic World API is not a live-mutation authority and not a second design store.
- **Goals vs observations.** One informal ingress arrow; two types. Goal = intent. Observation = evidence. Goals do not enter Observed Reality or `App::Document`.
- **Same document, new revision.** The top and bottom Authoritative Design boxes are the same `App::Document`. The bottom is a new `DesignRevision` after a successful DCC commit, which composes a new World Configuration. Calibration proposals do not take the DCS/DCC path.
- **Candidate overlay.** A constituent of World Configuration, not a live `DesignRevision`. Empty overlay = current committed world. Simulating the current world does not require creating a named candidate object.
- **Preflight is a gate.** Fail → typed diagnostics bound to the snapshot/configuration. No `SimulationEpisodeId`. No solver run.
- **Episode internals stay inside the episode.** Dynamics, geometry monitors, controls, and later optional FEM are how an episode runs, not peer boxes on this overview. FEM is typically Dynamics → reactions → load case → FEM, inside an episode, when that capability exists.
- **No World Model hub.** The semantic layer is an API over design and observations, not an authority that absorbs calibration, results, or commits.

GeometryJobManager, Ondsel, Coin3D, FEM, contact-solver internals, result-store internals, MCP `execute_code`, and collaboration process-local indexes stay off this overview. They may appear in lower-level sections as implementation mechanisms.

---

## 6. State Model

The architecture shall explicitly distinguish the following state categories.

### 6.1 Design State

Authoritative engineered intent held by FreeCAD.

Contains:

- parametric geometry;
- placements;
- assembly structure;
- joints and constraints;
- materials;
- mass-property sources;
- design parameters;
- functional/semantic annotations;
- actuator definitions;
- simulation-relevant metadata.

Cited as `App::Document` @ `DesignRevision`.

### 6.2 Observed State

Facts measured or inferred from physical reality.

Examples:

- actual hole diameter;
- actual printed mass;
- motor current;
- measured displacement;
- camera-derived geometry;
- human observation;
- environmental temperature.

Every observation should include:

```text
Observation
    object identity
    property/quantity
    value
    unit
    uncertainty
    timestamp
    source
    acquisition method
    confidence
```

Observations are independently revisioned (`ObservationRevision`). They are appendable without mutating design intent. A camera reading must not publish a new `DesignRevision`.

### 6.3 World Configuration

Which world is being considered: the named composition of independently revisioned inputs, plus an optional candidate overlay.

This is the former composition concept that older drafts called World State Frame or `WorldRevision`. That name is now retired for this meaning.

Conceptual schema:

```cpp
struct WorldConfiguration {
    WorldConfigurationId id;

    DesignDocumentId document;
    DesignRevision designRevision;
    ObservationRevision observationRevision;
    EnvironmentRevision environmentRevision;   // may be default/empty early
    CalibrationRevision calibrationRevision;   // may be default/empty early
    CandidateOverlay candidate;                // empty = current committed world
};
```

This is a logical citation, not a geometry object, not a snapshot archive, and not necessarily one C++ struct. `WorldConfigurationId` identifies the tuple. Nobody "commits a configuration" as a document mutation.

Environment and calibration belong here as constituents. They are not overview boxes.

### 6.4 World State Frame

Complete physical state of a simulated world at one instant `t`.

This is not `App::Document`, not a snapshot dump of the CAD document, and not the composition of revision IDs.

Conceptual schema:

```cpp
struct WorldStateFrame {
    SimulationEpisodeId episode;
    TimePoint t;

    PoseMap poses;
    VelocityMap velocities;
    AccelerationMap accelerations;
    ContactState contacts;
    ConstraintState constraints;
    ActuatorState actuators;
    // further complete-state quantities as the episode defines them
};
```

This is a logical model, not necessarily one C++ struct. Frames are identified within an episode by time (and optionally an index).

Teaching example:

```text
Snapshot
   │
   ▼
Frame t₀  position = 10 m, velocity = 0
   │ gravity
   ▼
Frame t₁  position = 9.95 m, velocity = -0.98 m/s
```

Gravity (or any other physics step) maps one World State Frame onto the next inside a Simulation Episode. The Immutable Snapshot is the frozen input; the frames are the evolving physical state.

Episode-private solver internals (working sets, cut states, and similar) are not World State Frames. They must not alias mutable live FreeCAD state. Published frames are the complete physical state the rest of the system is allowed to cite.

### 6.5 Proposed State

A candidate overlay considered by the agent but not yet accepted into the design.

This distinction allows:

```text
current World Configuration
    +
typed candidate overlay
    =
candidate World Configuration
```

without contaminating the live document. Candidates do not receive a live `DesignRevision`. Only an accepted proposal, committed through DCS/DCC, advances `DesignRevision` and therefore a new committed World Configuration.

---

## 7. Semantic World Layer

Raw CAD object access is insufficient for autonomous engineering.

The agent should not have to reason primarily from:

```python
doc.getObject("Body003").Shape.Faces[7]
```

It should be able to query concepts such as:

```text
object: left_spool_support
type: structural_support

relationships:
    attached_to: frame_left
    supports: spool_axis

material:
    PLA

physics:
    mass: 31.2 g

observations:
    deflection: 0.34 mm ± 0.08 mm

simulation:
    peak_stress: 19.2 MPa

status:
    valid
```

### Required semantic capabilities

- stable object identity;
- semantic type;
- functional role;
- parent/child and assembly occurrence relations;
- connection/joint relations;
- actuator/load/support roles;
- material and mass-property provenance;
- named engineering quantities;
- design-to-observation mapping;
- design-to-simulation mapping;
- queryable provenance.

The semantic layer must reference FreeCAD objects. It must not become an independent authority for live geometry, observations, calibration, or commits. It is not a second `App::Document`, and it is not the retired "World Model" hub.

These are end-state capabilities. First implementation is object / occurrence / joint identity plus provenance, not a bearing / shaft / fastener ontology. Richer types come after the first isolated-kinematics slice, observations, and the reality comparator. Execution order is in the progress/execution plan.

---

## 8. World Identity and Revision Model

A first-class identity system is required.

Minimum identities:

```text
DocumentId
ObjectId
OccurrenceId
SemanticEntityId
DesignRevision
ObservationRevision
EnvironmentRevision
CalibrationRevision
WorldConfigurationId
CandidateId
SnapshotId
SimulationEpisodeId
ResultId
ProposalId
```

`WorldRevision` is not in this list. If the name appears in older material, it meant the composition now called World Configuration / `WorldConfigurationId`. It must not be reused as a single world clock, and it must not be used as another name for `DesignRevision`.

World State Frames are not a revision-tuple identity. They are physical states at time `t` inside an episode.

The architecture must define:

- creation;
- persistence;
- invalidation;
- deletion;
- replacement;
- copy/save-as behavior;
- cross-document reference behavior;
- assembly occurrence identity;
- mapping across detached workers.

Revision semantics must distinguish at least:

- `DesignRevision` of `App::Document`, published on DCC commit, including the need to detect structural, property, and geometry change for stale design proposals;
- `ObservationRevision` of the observation store;
- `EnvironmentRevision` and `CalibrationRevision` as World Configuration constituents (may be default/empty early);
- `WorldConfigurationId` as the citation of that composition.

Avoid making the FCStd byte hash or the dirty flag the semantic revision mechanism.

Process-local collaboration indexes (`DocumentRevisionIndex`, `collaborationObjectIdentity`, and similar) are stale-check machinery for a live process. They are not `DesignRevision`, not `WorldConfigurationId`, and not a World State Frame. Durable `DesignRevision` that survives save/reload is identity work, not an implication that those indexes already are the world clock.

Timestamp is provenance, not identity. Two citations with the same constituent revisions are the same World Configuration. "I asked at 12:01" is a query identity, not a new world.

---

## 9. Perception and Observation Layer

`Perception` is the architectural abstraction. Cameras and sensors are implementations.

```text
Physical World
      │
      ▼
 Perception
      │
      ├─ camera / computer vision
      ├─ dimensional measurement
      ├─ current / force / position sensors
      ├─ machine telemetry
      └─ human observations
      │
      ▼
Normalized Observations
      │
      ▼
Observed Reality @ ObservationRevision
```

Committed observations become a constituent of World Configuration. They do not become design state.

### Requirements

Observations must be:

- appendable without mutating design intent;
- attributable to an entity;
- time-stamped;
- unit-safe;
- uncertainty-aware;
- provenance-aware;
- replaceable/supersedable without erasing history;
- queryable by the agent and simulation system.

The first implementation may use manually supplied measurements before camera/sensor automation exists.

---

## 10. Immutable World Snapshot

Simulation shall consume an immutable, self-describing snapshot captured from a World Configuration.

The configuration is the logical reference. The snapshot is the frozen payload.

Conceptual contents:

```cpp
struct WorldSnapshot {
    SnapshotId id;
    WorldConfigurationId sourceConfiguration;
    DesignRevision designRevisionAtCapture;

    AssemblyTopology topology;
    GeometrySnapshot geometry;
    MaterialSnapshot materials;
    MassPropertySnapshot massProperties;
    JointSnapshot joints;
    ActuatorSnapshot actuators;
    LoadSnapshot loads;

    ObservationSnapshot observations;
    EnvironmentSnapshot environment;
    CalibrationSnapshot calibration;
    UncertaintySnapshot uncertainty;
};
```

### Snapshot requirements

- no live `App::Document*`;
- no live `DocumentObject*`;
- deterministic dependency capture;
- explicit missing-data status;
- bounded serialization;
- versioned schema;
- integrity validation;
- stale-source detection;
- traceability back to the source World Configuration and object identities.

Capture must revalidate that the live `DesignRevision` still matches the configuration's `DesignRevision` (TOCTOU). Failed capture is not an episode.

The snapshot format should reuse proven detached-worker/archive patterns where they fit, but simulation concerns should remain a distinct schema/domain. Those mechanisms stay off the overview.

---

## 11. Model Preflight

Preflight is a gate: can this snapshot support this requested experiment?

Simulation must fail early when the model cannot support the requested analysis. Fail → typed diagnostics bound to the snapshot/configuration. No `SimulationEpisodeId`. No solver run.

Checks may include:

- missing references;
- invalid shapes;
- unresolved links;
- invalid or underdefined joints;
- missing material;
- missing density/mass/inertia;
- impossible actuator definitions;
- inconsistent units;
- unsupported topology;
- missing contact properties;
- invalid boundary/load definitions;
- stale observations;
- unsupported simulation feature.

Output must be structured:

```text
severity
code
entity
message
required_action
blocking/non-blocking
```

The agent must be able to reason from preflight findings.

Preflight is not an engine, not a sibling of dynamics, and not an episode.

The first implemented preflight is the minimum set of failures the real isolated kinematics backend actually produces. A generic request/scenario/output schema engine is not a prerequisite for that slice.

---

## 12. Simulation Orchestrator

Do not build one monolithic "simulation engine."

Build a common orchestration layer with adapters. This is episode implementation machinery. It is not an overview box, and its adapters are not overview peers.

```text
SimulationOrchestrator
    ├─ Kinematics adapter
    ├─ Multibody dynamics adapter
    ├─ Geometry/contact monitor
    ├─ FEM adapter          // later, inside an episode; not a peer of Motion on the overview
    ├─ Control-system adapter
    └─ future thermal/CFD/etc.
```

A Simulation Episode *contains* dynamics, geometry monitoring, controls, and optional later FEM. FEM is not a peer of Motion on the overview. Typical later flow, when that capability exists:

```text
Dynamics
    ↓
motion / reactions
    ↓
selected load case
    ↓
FEM
```

### Responsibilities

- accept a configuration-bound, snapshot-bound simulation request;
- validate compatibility;
- select engines;
- construct deterministic episode input;
- isolate solver execution where appropriate;
- handle cancellation/deadlines;
- collect normalized outputs, including World State Frames and Predictions;
- retain engine-native artifacts;
- report failures without inventing partial success.

First physics: isolated kinematics, with no live `Placement` writeback and no live-document write. Interactive GUI assembly solve may continue to write placements for human drag/solve; autonomous episodes must not. Solver choice remains behind an adapter boundary; isolated Ondsel-style kinematics is the intended first reuse, not an overview box.

Do not introduce a stub or reference backend to prove orchestrator types compile. Episode machinery is born with that real isolated kinematics path. Additional adapters (dynamics, contact, FEM) are later. Execution order is in the progress/execution plan.

---

## 13. Simulation Episode

A Simulation Episode is one reproducible experiment: request + snapshot + scenario/controls + solver identity + the time series of World State Frames + Predictions.

It is not a box that sits after several peer engines. The engines run *inside* the episode.

Conceptual request:

```cpp
struct SimulationRequest {
    SnapshotId snapshot;
    WorldConfigurationId sourceConfiguration;
    ScenarioDefinition scenario;
    RequestedOutputs outputs;
    SolverConfiguration solver;
    RandomSeed seed;
};
```

Conceptual result:

```cpp
struct SimulationResult {
    SimulationEpisodeId episode;
    SnapshotId input;
    WorldConfigurationId sourceConfiguration;
    ResultStatus status;

    TimeSeries<WorldStateFrame> frames;   // t₀ … tₙ
    Predictions predictions;

    ContactEvents contacts;
    ReactionSeries reactions;
    ConstraintViolations violations;
    FEMResultRefs fem;                    // later, inside the episode
    DiagnosticSet diagnostics;
};
```

Every result must record enough identity to answer:

> Exactly what World Configuration, snapshot, solver, and executable produced these frames and predictions?

A failed preflight has no `SimulationEpisodeId`.

---

## 14. Geometry Monitors

Geometry monitoring is distinct from dynamics. It runs as episode internals (or as a static check against a snapshot/candidate), not as an overview engine.

Required capabilities should eventually include:

- interference detection;
- minimum clearance;
- contact candidates;
- motion envelope;
- joint-limit geometry;
- swept volume;
- reachability;
- missing geometry;
- invalid topology encountered during motion.

The system should support both:

```text
static geometry check
```

and:

```text
geometry check over simulated trajectory
```

---

## 15. Multibody Dynamics

The MBD layer should own physical motion state, not FreeCAD placements. It produces World State Frames inside an episode.

First implemented physics is isolated kinematics, not true dynamics:

- prescribed motion;
- joints;
- poses;
- velocity if reliably available;
- no live `Placement` writeback.

Architectural MBD capability (destination; not the first implementation slice):

- rigid bodies;
- fixed and supported bodies;
- native assembly joints mapped to solver constraints;
- gravity;
- externally applied forces;
- torques;
- motors/actuators;
- prescribed motion;
- reaction forces/torques;
- velocity and acceleration;
- basic contact.

Later scope may add:

- friction;
- compliant contacts;
- backlash;
- damping;
- flexible bodies;
- richer controllers.

Gravity, physical mass/inertia, force, torque, reactions, and motor models are not promised until a later dynamics-capability decision proves they are not decorative fiction. That decision may keep Ondsel or select another backend. Solver choice must remain behind an adapter boundary. Execution order is in the progress/execution plan.

Autonomous episodes must not write solver poses into live `Placement`. Current Assembly solve/playback that does so is GUI behavior to preserve for interactive work; it is not the episode result path.

---

## 16. FEM Load Transfer

FEM is downstream evidence inside an episode, not the owner of world state, and not a peer of Motion on the overview.

Typical flow, when this capability exists:

```text
MBD episode frames / reactions
    ↓
selected time / worst load case
    ↓
load-transfer mapping
    ↓
FEM model
    ↓
CalculiX / other solver
    ↓
stress / strain / deformation / safety metrics
```

Requirements:

- trace forces back to source episode/time;
- deterministic load transfer;
- preserve mesh/solver provenance;
- do not silently invent missing constraints;
- return structured validity status;
- retain solver-native artifacts.

Live `FemAnalysis` document objects are not the autonomous result authority. FEM stays off the overview.

---

## 17. Result Store and Playback

Results need a normalized durable model.

The store should support:

- result identity;
- source snapshot and source World Configuration;
- World State Frames as the time series of physical state produced by an episode;
- Predictions derived from that episode;
- per-entity quantities;
- events;
- solver diagnostics;
- generated artifacts;
- comparison between runs;
- retention policy.

### Playback

Coin3D remains appropriate for FreeCAD-side visualization. It stays off the overview.

Playback can consume World State Frames and overlays for:

- motion;
- collision state;
- vectors;
- reaction forces;
- stress/deformation references;
- traces;
- plots.

Coin3D is a presentation layer. It is not the simulation or world-state authority. Playback state is view state. It must not become design state.

---

## 18. Comparators

A reality-aware system needs two comparators and two default proposal types. One "evaluation" blob is not sufficient.

### 18.1 Goal Comparator

Compares Predictions with intent.

Example:

```text
goal: close in < 8 s
predicted close time: 9.4 s
```

Default output: a Design Proposal (or reject/refine candidate). Goal misses are not automatically calibration updates.

### 18.2 Reality Comparator

Compares Predictions with observations.

Example:

```text
predicted spool position at t=2.0 s:  41.8°
observed spool position at t=2.0 s:   38.9° ± 0.6°
residual:                               -2.9°
```

Responsibilities:

- align predicted and observed quantities;
- compute residuals;
- account for uncertainty;
- flag unsupported comparisons;
- track calibration parameters;
- expose confidence to the agent.

Default output: a Calibration Proposal, or "comparison unsupported". A reality miss is not automatically a CAD miss. Diagnosis may later justify a design proposal; that is a second step, not this comparator's default write.

It may recommend model-parameter changes, but must not modify design state directly.

---

## 19. Model Calibration

Calibration should remain distinct from design mutation.

Examples:

- friction coefficient;
- motor torque curve;
- printer dimensional bias;
- effective cable radius;
- joint compliance;
- sensor offset.

The system must distinguish:

```text
design parameter
physical-model parameter
calibration parameter
observation
```

An agent may propose a calibration update based on evidence. Acceptance should remain controlled and auditable.

Calibration proposals do **not** go through DCS/DCC as a design commit. They advance `CalibrationRevision` (and therefore a new World Configuration) on a distinct acceptor. They must not loop into a semantic-layer authority or into `App::Document` as if they were CAD intent.

If a value is CAD intent, it is `DesignRevision`. If it is a physics knob, it is `CalibrationRevision`. Mixing both in one property is an implementation smell to forbid, not a World Configuration feature.

---

## 20. Agent World API

The AI interface should be semantic and typed.

Human and agent share this API for engineering operations. Humans also keep the GUI for CAD construction. Authority is capability level (observe / simulate / propose / commit), not a second API.

Core capability groups:

### Query

```text
get_world_configuration
get_entity
query_entities
get_relationships
get_geometry_summary
get_physical_properties
get_observations
get_validation_findings
get_simulation_results
get_world_state_frames
compare_results
```

### Experiment

```text
create_candidate_world
apply_candidate_parameter_change
run_preflight
run_simulation
run_geometry_monitor
run_fem
compare_with_goal
compare_with_observation
```

### Proposal

```text
create_mutation_proposal
validate_proposal
submit_proposal
inspect_commit_result
```

The API should prefer structured outputs over prose.

---

## 21. Candidate Worlds

The agent requires cheap experimentation without changing the live document.

A candidate is a World Configuration whose design constituent is an uncommitted overlay on a base `DesignRevision`, sharing the same observation / environment / calibration revisions. Empty overlay = current committed world.

Architecture:

```text
World Configuration C100   (empty overlay = current world)
       │
       ├─ Candidate A: spool 40 → 35 mm
       ├─ Candidate B: spool 40 → 37 mm
       └─ Candidate C: different bearing support
```

A candidate world is:

```text
base World Configuration
+
typed candidate overlay
→
Immutable Snapshot of that configuration
```

It is not a hidden copy of the live `App::Document`. Candidates do not receive live `DesignRevision`.

Candidate v1 in the execution sequence is a property/parameter overlay only. Later:

```text
Candidate
    ├── ParameterOverlayCandidate
    └── TempCandidateDocument
```

Both are explicitly not live design. If a temporary candidate document is used as an implementation tactic, its process-local revisions are sandbox clocks, not live `DesignRevision`. TempCandidateDocument is not required for the first isolated-kinematics slice.

Candidates may be simulated and compared. Only an accepted proposal is converted into live model intent.

The A/B/C branching is unpacking. The overview shows candidate as a World Configuration constituent, not three overview forks.

---

## 22. Agent Action Boundary

An agent design action follows:

```text
Agent reasoning
     ↓
Design Proposal
     ↓
proposal schema validation
     ↓
DesignRevision validation (DCC stale)
     ↓
World Configuration / Episode validation (engineering stale, if evidence-based)
     ↓
semantic dependency validation
     ↓
policy / authority check
     ↓
DocumentCollaborationService
     ↓
DocumentCommitCoordinator
     ↓
App::Document @ DesignRevision + 1
     ↓
new committed World Configuration
```

The action boundary must support:

- dry run;
- affected-object set;
- expected preconditions;
- source `DesignRevision`;
- source World Configuration and/or Episode when the proposal is evidence-based;
- reason/evidence references;
- rollback through normal FreeCAD transaction semantics;
- explicit rejection reason.

Calibration proposals do not enter this path.

---

## 23. Autonomy Levels

Do not jump directly to unrestricted autonomous commits.

Recommended levels:

### Level 0 — Observe

Agent can query World Configuration, design, observations, and results.

### Level 1 — Simulate

Agent can create candidates and run simulations.

### Level 2 — Propose

Agent can prepare a mutation proposal but a human approves it.

### Level 3 — Bounded autonomous action

Agent may commit changes inside explicitly granted scopes and constraints.

### Level 4 — Closed-loop engineering task

Agent can iterate observe → simulate → act → verify within a bounded goal and stop conditions.

Each level must be implemented and qualified independently.

---

## 24. Failure Model

Expected failures are part of the architecture.

Examples:

- DCC-stale design proposal (`DesignRevision` moved);
- engineering-stale evidence (World Configuration, snapshot, or episode moved);
- deleted object;
- changed assembly topology;
- missing mass/material;
- invalid observation;
- schema mismatch;
- solver crash;
- solver timeout;
- worker crash;
- excessive result size;
- contact non-convergence;
- unsupported joint;
- candidate invalidated during simulation;
- live commit conflict;
- preflight failure (no episode);
- external physical observation contradicts model.

Failures must remain:

- typed;
- attributable;
- non-destructive;
- retryable only when appropriate;
- observable by the agent;
- retained as evidence where relevant.

---

## 25. Security and Authority

The agent is not automatically trusted because it is local.

Required controls:

- explicit capability boundaries;
- read vs simulate vs propose vs commit permissions;
- no arbitrary live pointer access from isolated jobs;
- bounded resources;
- deadlines;
- cancellation;
- path and artifact validation;
- schema validation;
- no hidden fallback to direct mutation;
- no automatic escalation from proposal to commit.

---

## 26. Persistence Strategy

Persist separately.

### In the FreeCAD document (FCStd)

- design semantics;
- stable model identity required for document meaning;
- simulation definitions that are part of engineering intent;
- explicit material/actuator/load metadata;
- IDs and references to observations, snapshots, episodes, and results;
- durable `DesignRevision` encoding, once identity work defines it.

### Sidecar / alongside the design document

- observation values and high-rate telemetry;
- World Configuration citation records when they must be stored;
- snapshots;
- episodes, World State Frames, Predictions, and large simulation results;
- transient solver artifacts;
- calibration and environment parameter stores (FCStd may hold refs, not time series);
- qualification evidence;
- caches.

Do not turn FCStd into an unbounded telemetry database.

---

## 27. Testing Strategy

The architecture requires multiple evidence layers.

### Static architecture gates

Verify that:

- simulation does not own live commits;
- simulation does not write live `Placement` or live document state;
- workers cannot receive live document pointers;
- agent mutations route through DCS/DCC;
- calibration proposals do not enter DCC as design commits;
- world-configuration/snapshot APIs preserve composition identity;
- World State Frames are treated as physical time-series state, not as revision tuples;
- forbidden direct mutation paths are absent or classified.

### Unit tests

For:

- identity;
- revision invalidation (design vs observation vs configuration);
- serialization;
- semantic mapping;
- observation provenance;
- preflight (including fail-closed: no episode id);
- candidate overlays;
- result normalization and World State Frames;
- goal comparison;
- reality comparison.

### Native integration tests

For:

- snapshot capture of a World Configuration;
- assembly mapping;
- detached solver invocation;
- coordinator interaction;
- stale-result rejection (DCC stale vs engineering stale);
- recompute interaction;
- save behavior.

### Scenario tests

Use small deterministic mechanisms. First physics qualification is isolated kinematics on a bounded 1-DOF grounded revolute or slider with prescribed motion, not AutoCurtains and not load/contact/dynamics. Later scenario tests may add known expected load/contact once that capability exists.

### Cross-platform qualification

At minimum:

- Windows native;
- Linux/Docker where supported.

### Stress qualification

Exercise:

- repeated snapshots;
- repeated simulations;
- cancellation;
- stale candidates;
- save during/after simulation;
- large result streams;
- clean shutdown.

---

## 28. Definition of Done

The end architecture is complete only when:

1. the system compiles and imports;
2. it does not crash or hang under supported workloads;
3. all required tests pass with real terminal exit codes;
4. supported error paths are handled fail-closed;
5. existing collaboration/save behavior remains intact;
6. the workflow requires no undocumented manual repair step;
7. the system demonstrably performs an end-to-end autonomous engineering loop;
8. every accepted result is tied to an immutable source identity (snapshot + source World Configuration + frames);
9. every committed agent action is tied to a valid `DesignRevision`, and evidence-based actions also cite World Configuration and/or Episode;
10. prediction and observation remain distinguishable and comparable;
11. complete Windows/Linux qualification evidence is retained;
12. the exact reviewed/tested source is the source that is committed and pushed.

---

## 29. Final Demonstration

The architecture should finish with one real end-to-end demonstrator.

The first physics slice is isolated kinematics on a bounded 1-DOF mechanism, with no live `Placement` writeback. AutoCurtains remains a later closed-loop demonstrator, not that first slice.

Example acceptance scenario (later closed-loop, once the first physics slice is proven):

```text
Given:
    a real FreeCAD assembly
    known motor
    known spool/cable geometry
    mass/material properties
    gravity
    at least one measured physical observation

Goal:
    satisfy a motion target
    while meeting torque/contact/stress constraints

When:
    the agent inspects the world
    identifies a failing constraint
    creates multiple candidate designs
    captures snapshots of those World Configurations
    simulates them (episodes of World State Frames → Predictions)
    selects a candidate
    produces a DesignRevision-bound mutation proposal
    commits through DCS/DCC
    recomputes the document
    reruns simulation
    compares prediction with observed reality

Then:
    all state transitions are attributable
    no simulation directly mutates design
    no agent bypasses the coordinator
    DCC-stale and engineering-stale decisions are rejected
    evidence reproduces the accepted claim
```

That is the target definition of a reality-aware autonomous engineering system.
