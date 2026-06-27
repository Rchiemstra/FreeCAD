# AutoCurtains v1 Requirements

**Status:** Draft — 2026-05-28, updated 2026-05-30 (v1.6 — added verifiable requirements §3a; flagged two open gaps: two-panel actuation [DD-1] and close-direction drive [DD-2])
**Sources:** `doc/20260506_164913.jpg` (room photo), `doc/Schermafbeelding 2026-05-28 233335.png` (floor plan), `models/MG995.step` (CAD geometry), MG995 datasheet (external).

---

## 1  Measured / Visible Facts

### 1.1  Room photo observations

| Feature | Observation | How determined |
|---|---|---|
| Ceiling type | Flat, koof (soffit/recess) above window zone | Visible in photo |
| Ceiling height (est.) | ~250 cm | Proportional estimate, typical Dutch apartment |
| Main window type | Patio/sliding door, 2 glass panels, floor-to-near-ceiling | Photo |
| Curtain style | Eyelet (oogjes) rings on a round rod | Rings visible at rod top in photo |
| Rod style | Circular cross-section, satin/brushed-steel finish, continuous | Photo |
| Curtain layers | Two: sheer/voile (white, inner) + dark blackout (outer) | Photo |
| Blackout stacks | Dark panels stacked at left and right ends of rod (open position) | Photo |
| Rail mounting | Rod sits in or just in front of koof/ceiling recess | Photo — rod not exposed below ceiling |
| Right window | Separate smaller window to the right; similar round rod + eyelet curtains | Photo (right side) |
| Curtain drop (est.) | ~230–240 cm, appears floor-length | Proportional estimate |
| Eyelet rings visible | ~14–18 rings on main rod (both panels combined) | Photo count |
| Inner curtain (sheer) | Single wide white/voile panel covering full window opening | Photo |
| Outer curtain (dark) | Two blackout panels, one per half of window; stacked at sides when open | Photo |

`doc/20260506_164913.jpg` is the primary visual canvas for the FreeCAD/Gazebo
layout: main patio opening centred, smaller right window to the right, koof
above, high rods, dark curtain stacks at the sides, and a sheer layer in front
of the glass. It is not treated as a source of exact measured dimensions.

### 1.2  Floor plan observations

| Feature | Value / Observation | Note |
|---|---|---|
| Room label | Woonkamer (living room), open to keuken (kitchen) | Label in plan |
| "koof" | Soffit / ceiling recess along the full exterior window wall | Standard Dutch curtain-rail recess |
| Dimension 4958 mm | Spans the full woonkamer / koof exterior wall zone | Outer wall span, accommodates main + right window |
| Dimension 7850 mm | Room depth (woonkamer + keuken combined) | Vertical in plan |
| Dimension 2293 mm | Hal (entrance hall) width | Plan label |
| Window position | At the koof side (bottom exterior wall of plan) | Plan geometry |
| Right window | Visible at right end of koof wall; separate from main sliding door | Plan geometry |

### 1.3  MG995.step geometry (parsed from CARTESIAN_POINT data)

| Feature | Extracted value | Origin |
|---|---|---|
| Body X span (width) | ~39 mm | CARTESIAN_POINT range |
| Body Y span (depth) | ~19 mm | CARTESIAN_POINT range |
| Mounting tab extension | X approx -25.6 mm from body centre | STEP geometry |
| Total mounting width (tabs) | ~45 mm | STEP geometry |
| Output shaft hub height | Z approx 21-24 mm above flange plane | STEP CARTESIAN_POINT |
| Mounting flange thickness | ~2.45 mm below body base | STEP geometry |

#### 1.3b  MG995 mounting-hole geometry (v1.8 -- parsed from CYLINDRICAL_SURFACE entities)

| Feature | Value | Confidence | Origin |
|---|---|---|---|
| Mounting holes | 4x cylindrical holes at STEP (+-24.75, +-5.10, 0.0) | Inferred from STEP | CYLINDRICAL_SURFACE r=2.25 mm at mounting face |
| Hole diameter | 4.5 mm (r=2.25 mm) -- M4 clearance | Inferred | STEP CYLINDRICAL_SURFACE radius |
| Long-axis hole spacing | 49.5 mm (2 x 24.75 mm) | Inferred | STEP X span of outer hole pair |
| Short-axis hole spacing | 10.2 mm (2 x 5.10 mm) | Inferred | STEP Z span (maps to world X after rotation) |
| Mounting face orientation | STEP Z=0 = servo "bottom" face | Inferred | Z=0 is lowest Z of mounting tab geometry |
| Additional top holes | 4x r=2.0 mm at STEP (+-18.17, +-8.06, 26.30) | Inferred | Likely strap/zip-tie slots; NOT used for mounting |

**Note:** These are inferred from STEP geometry, not physically measured on a servo unit.
Verify hole positions against the physical servo before drilling the bracket.
The bracket in v1.8 documents these holes but cannot represent the Y-direction holes
with a single XY sketch pad (architectural limitation -- see doc/requirements.md Section 3b).

### 1.4  MG995 datasheet (standard part, external source)

| Parameter | Value |
|---|---|
| Stall torque @ 6 V | 11 kg*cm (approx 1.08 N*m) |
| No-load speed @ 6 V | ~60 RPM (0.13 s/60 deg) |
| Default rotation range | 180 deg (can be modified for continuous rotation) |
| Weight | 55 g |
| Body dimensions | 40.7 x 19.7 x 42.9 mm |
| Output spline | 25T |

---

## 2  Assumptions (must be verified on-site before physical build)

| ID | Assumption | Value used in v1 model | Rationale |
|---|---|---|---|
| A1 | Main window width | 2200 mm | Proportional from photo, 250 cm ceiling |
| A2 | Main window height | 2100 mm | Floor-to-near-ceiling patio door |
| A3 | Curtain rail total length | 2700 mm | Window + 250 mm overhang each side |
| A4 | Rail height from floor | 2350 mm | Near ceiling inside koof |
| A5 | Rail / rod outer diameter | 28 mm | Standard IKEA/European curtain rod |
| A6 | Right window width | 900 mm | Proportional estimate from floor plan |
| A6b | Right window height | 1500 mm | Smaller than patio door; proportional estimate |
| A7 | Eyelet ring inner diameter | 40 mm | Typical for eyelet curtains |
| A8 | Curtain open travel per panel | 1000 mm | Panels stack to ~50% of window width |
| A9 | Curtain weight (both panels) | 1.5 kg total | Heavy blackout fabric, 2 m height |
| A10 | Required pull force | 15 N | Weight + friction on rings |
| A11 | MG995 modified for continuous rotation | Yes | 180 deg stock range insufficient for 1 m travel |
| A12 | Gear module | m = 1.5 (increased from 1.0 in v2.0-visual-fit for assembly clearance; see §9) | Module 1.5 gives 56.25 mm centre distance and >=7.25 mm servo clearance |
| A13 | Gear type | Spur, trapezoid teeth (visual/mockup per GEMINI.md) | MVP; not for high-load application |
| A14 | Cord diameter | 2 mm | Standard nylon curtain cord |
| A15 | Spool winding radius | 20 mm | Balances force and wind speed |
| A16 | Power supply | 5-6 V regulated, 1 A min | Servo spec |
| A17 | Koof visible band height | 150 mm | Estimated from photo proportions; see U5 |
| A18 | Koof soffit depth | 250 mm | Estimated; see U5 |
| A19 | Gap between main and right window | 600 mm | Proportional estimate from floor plan |
| A20 | F608 bearing dimensions | OD=22 mm, ID=8 mm, width=7 mm | Standard catalog (ISO 618 F608); verify with supplier before ordering |
| A21 | M8 hardware dimensions | Nylock nut: 13 mm AF / 6.5 mm thick; washer: 16 mm OD / 1.6 mm thick | DIN 934/125 standard; axle bolt M8x60 |

---

## 3  Unknowns (block physical build — measure before ordering hardware)

| ID | Unknown | Impact if wrong |
|---|---|---|
| U1 | Exact main window width | Spool rotations required; gear ratio selection |
| U2 | Curtain rod type: round solid vs. hollow vs. traverse track | Bracket bore design; ring carrier design |
| U3 | Rod / track outer diameter | Bracket clamp bore |
| U4 | Eyelet ring internal diameter | Cord diameter and spool geometry compatibility |
| U5 | Koof depth and accessible void | Bracket arm length; wiring routing |
| U6 | Actual curtain weight | Torque verification; whether MG995 suffices without gear stage |
| U7 | Continuous-rotation mod: DIY vs. replacement motor | Motor spec; controller type |
| U8 | Mains/12 V vs. USB power available at window | PSU choice and wiring length |
| U9 | Existing home automation system | Controller integration (ESP32, Raspberry Pi, etc.) |
| U10 | Second window scope (v2?) | Right window modelled visually in v1 but not motorised |

---

## 3a  Verifiable Requirements (v1.6 — testable acceptance criteria)

These are the **authoritative acceptance criteria** for AutoCurtains v1. Every requirement
below has a single pass/fail criterion and a verification method. Where a number is a target
(not yet bench-confirmed), it is marked *(target)* — record the actual value during testing.

**Verification codes:** `T` = bench/functional test · `D` = demonstration · `I` = inspection or
measurement · `A` = analysis/calculation. "Traces" links each requirement to the Assumption (A*),
Unknown (U*), Open Decision (DD-*), or section it depends on.

A requirement with status *Blocked* cannot be verified until the listed Unknown or Decision is
resolved. No requirement is "passed" by assumption — only by the stated method on the real build.

### 3a.1  Functional (FR) — what it must do

| ID | The system SHALL… | Acceptance criterion (pass/fail) | Verify | Traces |
|---|---|---|---|---|
| FR-1 | open and close the blackout curtain on a single command | "Open" command ends in fully-open state; "close" ends in fully-closed state; each reached with no manual assistance | T | A8, U1 |
| FR-2 | fully clear the glazing when open | In open state, blackout fabric obstructs ≤10% of measured glazing width (U1); both panels parked at the rod ends | T, I | A1, A8, U1, DD-1 |
| FR-3 | drive the curtain under power in BOTH directions | Motor moves the curtain open AND closed from any intermediate position; closing does NOT rely on gravity, spring, or an un-powered return | T | DD-2 |
| FR-4 | stop automatically at each end of travel | Drive de-energises within 1.0 s of reaching either end stop; no sustained stall buzz | T | U1, SR-2 |
| FR-5 | accept commands from a local control AND a remote/automation interface | Open/close/stop demonstrated from the manual control (KY-040) AND from the ESP32/home-automation path | D | U9, IR-1, IR-3 |
| FR-6 | re-establish a known position after power loss | After a power cycle mid-travel, system homes to a known end and reports position; the next open/close lands within PR-3 | T | IR-2 |
| FR-7 | hold a partial-open setpoint without drift | After reaching a mid setpoint, position changes ≤ PR-3 budget over 1 h idle | T | PR-3, MR-6 |

### 3a.2  Performance (PR) — how well

| ID | The system SHALL… | Acceptance criterion | Verify | Traces |
|---|---|---|---|---|
| PR-1 | complete a full open or full close within budget | ≤25 s end-to-end at nominal voltage *(design target ~16 s, §4)* | T | §4 |
| PR-2 | provide ≥2.0× force margin over worst-case pull | Measured stall force at the cord ≥ 2.0 × measured worst-case pull force (U6/A10); analysis agrees within 20% | T, A | A9, A10, U6 |
| PR-3 | return to a commanded position repeatably | "Fully open" lands within ±20 mm of its reference across 20 consecutive open/close cycles | T | MR-4, MR-6 |
| PR-4 | run quietly (mechanism sits above seating) | Drive noise ≤45 dB(A) at 1 m *(target — record actual)* | T | — |
| PR-5 | stay within the available supply | Peak current ≤ PSU rating (A16); idle draw ≤100 mA *(target)* | T | A16, U8 |

### 3a.3  Mechanical / build (MR)

| ID | The system SHALL… | Acceptance criterion | Verify | Traces |
|---|---|---|---|---|
| MR-1 | clamp to the existing rod with NO permanent modification to rod or koof | Bracket installs and removes with hand tools; no holes, glue, or marks left on rod or koof | I | U2, U3, U5 |
| MR-2 | survive sustained operating load without yielding | No visible deformation or cracking after 24 h at 2× peak load on any printed structural part | T | A9, A10 |
| MR-3 | use a wear-resistant material at the highest-stress mesh | Servo pinion is PETG, nylon, or metal — NOT PLA (verify by part/filament) | I | A12, A13 |
| MR-4 | hold correct gear mesh as built | As-built centre distance = Σ pitch radii within ±0.2 mm (56.25 mm for 25T+50T at M1.5) | I | A12, §9 |
| MR-5 | route the cord/belt without binding | Guide/slot clear width ≥ drive-medium nominal Ø + 1.0 mm (≥3.0 mm for 2 mm cord) | I | A14 |
| MR-6 | use a low-creep drive medium | Elongation under working load ≤1% over 24 h, OR measured position drift meets PR-3 across 20 cycles | T, A | A14, PR-3 |

> **MR-6 note:** nylon/polyamide cord (A14) is hygroscopic and creeps; at 1.5–2 mm it is unlikely
> to pass the ≤1% criterion. Polyester, Dyneema/Spectra, or a toothed belt are expected to pass.
> This requirement exists specifically so the wrong material is caught on the bench, not in the wall.

### 3a.4  Interface / control (IR)

| ID | The system SHALL… | Acceptance criterion | Verify | Traces |
|---|---|---|---|---|
| IR-1 | use an ESP32-class Wi-Fi controller integrable with home automation | Controller exposes open/close/position to the home-automation system (U9) | D | U9 |
| IR-2 | sense position EXTERNALLY (the continuous-rotation servo provides no internal feedback) | Position known via AS5600 on the output shaft and/or 2× end-of-travel switches; position correctly read at both ends | I, T | A11, U7, FR-4, FR-6 |
| IR-3 | allow manual open/close/stop without a network | KY-040 (or equivalent) operates the curtain with Wi-Fi disabled | D | U9 |
| IR-4 | drive the servo with a calibrated 50 Hz PWM | Neutral/stop pulse trimmed per-unit (≈1.5 ms) so the servo is stationary at "stop"; open vs close pulses give opposite rotation | T | A11, U7 |

### 3a.5  Safety (SR)

| ID | The system SHALL… | Acceptance criterion | Verify | Traces |
|---|---|---|---|---|
| SR-1 | stop promptly on command or power removal | Motion halts within 200 ms of a stop command or power cut | T | BT |
| SR-2 | detect a stall and cut the drive | On a blocked end or jam, drive current is removed within 2.0 s; servo temperature rise stays within spec | T | FR-4 |
| SR-3 | stop on an obstruction in the curtain path | Travel halts before applying >25 N against an obstruction (above-seating pinch protection) | T | A10 |
| SR-4 | be powered by SELV only at the mechanism | ≤6 V at all user-accessible parts; no exposed mains near the rod or couch | I | A16, U8 |
| SR-5 | not free-fall or drop on power loss | With power removed at any point, the curtain holds (no back-drive run-away) and the bracket retains the rod | T, I | MR-1, FR-3 |

### 3a.6  Open design decisions (must be resolved before `physical_build_recommended`)

These are exposed by the requirements above and are currently **unresolved** in the v1 model.

| ID | Decision | Why it is open | Blocks |
|---|---|---|---|
| DD-1 | **Two-panel actuation.** §1.1 documents TWO blackout panels meeting in the middle, but §4 scope says single-panel. Choose: (a) one closed cord/belt loop with two clips moving in opposite directions, (b) two independent mechanisms, or (c) v1 motorises one half and the other stays manual. | §4 contradicts §1.1; "fully open" (FR-2) is undefined until this is chosen | FR-2 |
| DD-2 | **Drive medium / close path.** The §4 "cord spool" winds cord like a winch — it pulls ONE way only and has nothing to drive the curtain closed (fails FR-3). Choose: toothed-belt loop (positive engagement, low stretch), capstan cord loop (3–4 wraps, no slip), or twin-cord drum. | A single winding spool cannot satisfy FR-3; nylon cord likely fails MR-6 | FR-3, MR-6 |
| DD-3 | **Position-feedback method.** AS5600 magnetic encoder on the output shaft, vs. end-of-travel switches, vs. both. | IR-2 is mandatory but the method is unset | IR-2, FR-6 |

### 3a.7  Link to bench tests and readiness gates

Each PR/SR requirement above is the **acceptance threshold** that the corresponding `BT-1`…`BT-7`
test in `doc/bench_test_plan.md` checks. `installation_recommended` (Section 8) must NOT be granted
until every FR, PR, and SR requirement here has passed its stated verification, and every DD is
resolved. `physical_build_recommended` (Section 6/8) additionally requires DD-1 and DD-2 closed,
because the mechanism is not fully defined while they are open.

---

## 3b  Schematic vs. Mechanically Meaningful Geometry (v1.8)

The v1.8 model distinguishes three levels of geometry confidence:

| Level | Description | Objects |
|---|---|---|
| **Schematic only** | Visual shape only; position or fit not verified | Koof band, curtain stacks, right window, sheer curtain, eyelet rings |
| **Mechanically plausible** | Coordinates enforce known physical constraints (coaxiality, gear centre distance, clearance checks); shaft placement is **assumed** (MG995 STEP analysis) | Drive gear, driven gear, spool, servo (MG995_Ref), axle support block (F608 bearing pockets), shaft references, retention refs |
| **Build verified** | Not yet achieved; requires real U1-U7 measurements + physical bench test per `doc/bench_test_plan.md` | _(none)_ |

Key assumptions in the mechanically plausible layer:

- MG995 output shaft is at STEP coordinates (X=-10, Y=0), shaft exit Z=-9.0 (inferred from CYLINDRICAL_SURFACE entities; not physically measured on this specific unit).
- After `FreeCAD.Rotation((1,0,0), 90)` the shaft axis aligns with the world Y direction -- matching the gear rotation axis.
- Spool flange OD = 50 mm provides ~21.25 mm clearance to servo body (world Z) with GEAR_MODULE=1.5. (With the old M1.0 module this was only 2.5 mm.)
- Gear centre distance = (25+50)*1.5/2 = 56.25 mm (enforced by coordinates, not by a physical constraint solver).
- **v1.8 addition:** MG995 mounting holes inferred from STEP CYLINDRICAL_SURFACE at (+-24.75, +-5.10, Z=0), r=2.25 mm (M4 clearance). Bracket documents but cannot fully represent these Y-direction holes with a single XY sketch pad.
- **v1.8 addition:** F608 bearing pocket dimensions (OD=22 mm, width=7 mm) are standard catalog assumption A20.
- **v1.8 addition:** `Axle_Support_Block` replaces the v1.7 `Axle_Support_Cheek` placeholder; now includes two F608 bearing pockets (r=11.1 mm, depth=7 mm) and M8 clear-fit through bore (r=4.1 mm).

Run `python generate_model.py` (FreeCADCmd) to regenerate `generated/mechanical_fit_report.json` and verify `assembly_status: mechanically_plausible`.



## 4  Minimal v1 Design

### Scope

Main patio window only. Right window modelled visually only. Controller and electronics are out of scope for the mechanical v1.

> **Scope conflict — unresolved.** §1.1 documents TWO blackout panels meeting in the middle, while the v1 mechanism below was drafted around a single panel pulled one direction. What "open" and "close" mean for v1 is governed by **FR-2** (glazing clearance) and **FR-3** (powered both directions) and is gated on decisions **DD-1** (two-panel actuation) and **DD-2** (drive medium / close path). Resolve DD-1 and DD-2 before treating §4 as final.

### Mechanism: Servo-Spool Cord Puller with Gear Stage

```
Koof/Wall
  |
  +-- [L-Bracket] ---- [MG995 (continuous)] ---- shaft ---- [25T Drive Gear]
  |                                                                 | mesh
  |                                                         [50T Driven Gear]
  |                                                                 | shaft
  |                                                          [Cord Spool O40]
  |                                                                 | cord
  +-- [Rail 2700 mm -----------------------------------------] <-- [Ring 1]
```

**Opening sequence (A11 satisfied -- continuous rotation mod):**

1. MG995 shaft rotates continuously (modified servo or stepper alternative)
2. 25T -> 50T spur gear stage: 2:1 reduction (torque x2, speed /2)
3. Spool (radius 20 mm) winds cord: ~125 mm of cord per revolution
4. Opening 1000 mm requires ~8 spool revolutions
5. Spool speed ~30 RPM -> open time ~16 s

> **Open-direction only.** This sequence describes *opening*. Cord winding onto a single spool cannot push the curtain closed again — closing is governed by **FR-3** and is unresolved pending **DD-2** (toothed-belt loop / capstan loop / twin-cord drum). Do not treat this mechanism as complete until DD-2 is decided.

**Torque check:**

- Required: 15 N x 0.020 m = 0.30 N*m at spool shaft
- Available: 1.08 N*m (MG995) x 2 (gear ratio) = 2.16 N*m >> 0.30 N*m (7x margin)

### Part list (v1)

| # | Part | Material | Notes |
|---|---|---|---|
| 1 | Servo bracket (L-shape) | PLA or alu sheet | Sketch-first per GEMINI.md workflow |
| 2 | Drive gear 25T, M1.5 | PLA | Sketch-first, trapezoid teeth (visual/mockup, documented) |
| 3 | Driven gear 50T, M1.5 | PLA | Sketch-first, trapezoid teeth (visual/mockup, documented) |
| 4 | Cord spool (O40 drum + O60 flanges) | PLA | Part primitive (not a gear -- GEMINI.md rule does not apply) |
| 5 | MG995 servo | Commercial | Modify for continuous rotation |
| 6 | Nylon cord 2 mm | Nylon | Curtain pull cord |
| 7 | M4 x 16 screws (x8) | Steel | Bracket and servo mounting |
| 8 | M8 x 55 bolt + nut | Steel | Spool axle |
| 9 | F608 bearing (x2) | Steel | Optional: reduce friction on spool shaft |

### Physical build blockers (ordered by priority)

1. **U1** -- measure main window exact width -> confirm spool revolutions
2. **U2 / U3** -- confirm rod type and diameter -> finalize bracket bore and rail clamp
3. **U4** -- measure eyelet ring inner diameter -> verify cord fits
4. **U5** -- measure koof depth -> adjust bracket arm length
5. **U7** -- decide continuous-rotation mod: DIY trim-pot removal vs. replacement motor

### FreeCAD model deliverables

| File | Description |
|---|---|
| `AutoCurtains.FCStd` | Full assembly: environment + all v1 parts |
| `generate_model.py` | Headless regeneration script |

Regeneration command (verified Windows-safe, run from AutoCurtains/ directory):

```powershell
& "C:\Users\Rchie\Music\FreeCADModeling\FreeCAD\.pixi\envs\default\Library\bin\FreeCADCmd.exe" -c "g={'__file__':'generate_model.py','__name__':'__main__'}; exec(compile(open('generate_model.py', encoding='utf-8').read(), 'generate_model.py', 'exec'), g)"
```

Gear bodies use sketch-first workflow (Sketcher::SketchObject + PartDesign::Pad) per GEMINI.md.
Bracket uses sketch-first L-shape profile + pad.
Spool uses Part primitives (not a gear -- GEMINI.md rule does not apply).
MG995 is imported from `models/MG995.step` as a reference shape.

### Gazebo simulation deliverables

| File | Description |
|---|---|
| `worlds/autocurtains_world.sdf` | Room + koof + main window + right window + rail + servo revolute joint + curtain prismatic joint |

Cord coupling between servo/spool and curtain panel requires a Gazebo plugin or ROS 2 controller -- **out of scope for v1 SDF**. The SDF demonstrates the correct joint types and kinematics. Each joint can be driven independently for demonstration.

Joint command topics (Gazebo Harmonic):
- Spool: `/model/servo_spool/joint/spool_revolute/cmd_vel` (gz.msgs.Double, rad/s)
- Curtain: `/model/curtain_panel/joint/curtain_prismatic/cmd_pos` (gz.msgs.Double, 0.0-1.0 m)

### Smoke tests

`tests/test_smoke.py` -- XML validation, file existence, STEP sanity, FCStd objects, design calc formulas, build readiness.

Run: `python -m pytest tests/ -v`

---

## 6  Physical Build Package (v1.4)

### Documents

| File | Purpose |
|---|---|
| `doc/measurement_checklist.md` | On-site checklist: tools, locations, fields to update, U1-U7 |
| `doc/bom.md` | Bill of materials: printable parts, commercial parts, blocked items |

### Generated outputs (created by running generate_model.py)

| File | Content |
|---|---|
| `generated/design_calculations.json` | Spool circumference, required revolutions, pull force, torque margin, open time |
| `generated/build_readiness.json` | `physical_build_recommended` flag, blocking reasons, measurement blocker status, STEP export status |
| `generated/exports/bracket.step` | Printable bracket (L-shape, sketch-first) |
| `generated/exports/gear_25t_drive.step` | Printable 25T drive gear (sketch-first, trapezoid teeth) |
| `generated/exports/gear_50t_driven.step` | Printable 50T driven gear (sketch-first, trapezoid teeth) |
| `generated/exports/cord_spool.step` | Printable cord spool (Part primitives) |

**MG995 is NOT exported as a printable part.** It is a commercial servo; the reference geometry is in `models/MG995.step` for assembly only.

**Cord coupling caveat (Gazebo):** The servo spool and curtain panel are independently actuated in `worlds/autocurtains_world.sdf`. The mechanical cord coupling requires a Gazebo plugin or ROS 2 controller. Out of scope for v1.

### Build readiness gate

`generated/build_readiness.json` sets `physical_build_recommended: true` only when:
- U1 (main window width) is measured
- U2/U3 (rod type + diameter) are measured
- U4 (eyelet inner diameter) is measured
- U5 (koof depth) is measured
- U6 (curtain mass) is measured
- U7 (motor mode) is measured
- validation errors are empty
- Torque safety factor >= 2.0
- all printable STEP exports exist

With default/assumed values, `physical_build_recommended` is always `false`.

---

## 7  Post-Measurement Workflow (v1.5)

### Overview

Once on-site measurements replace all critical assumed values, the full build workflow is:

1. **Measure** on-site using `doc/measurement_checklist.md`.
2. **Edit** `config/measurements.yaml` with real values; set `_status: measured`.
3. **Validate** without FreeCAD: `python scripts/validate_config.py` (exits 0 = READY).
4. **Regenerate** FreeCAD model via FreeCADCmd to update FCStd, STEP exports, and JSON reports.
5. **Print** parts from `generated/exports/`.
6. **Order** commercial parts from `doc/bom.md`.
7. **Assemble** following `doc/assembly_guide.md`.

### Sample measured config

`config/measurements.sample_measured.yaml` provides a complete example with plausible values
(SAMPLE ONLY -- not actual measurements from the room in `doc/20260506_164913.jpg`).
Using this file will produce `physical_build_recommended: true`.

To validate it:
```powershell
python scripts/validate_config.py config/measurements.sample_measured.yaml
```

### Cross-validation

`generate_model.py` includes `_cross_validate_measurements()` which checks for:
- `curtain_travel_mm > main_window_width_mm` -- panel would travel past window edge
- `koof_depth_mm < BRACKET_HW` -- bracket arm may not fit inside koof
- `rod_type == traverse_track` -- requires different bracket design (not yet modelled)
- Very small eyelet bore -- cord may not thread through

These are soft warnings (not hard errors) that appear in `build_readiness.json` under `design_warnings`.

### Assembly

`doc/assembly_guide.md` covers:
- 3D-printed part preparation and fit checks
- Gear and servo sub-assembly
- Cord spool winding
- Bracket installation in koof
- Cord threading and curtain connection
- Initial manual test
- What remains out of scope (controller, firmware, wiring)

**Cord coupling caveat**: The Gazebo simulation remains independently actuated.
The mechanical cord that couples servo spool to curtain requires a Gazebo plugin or
ROS 2 controller -- out of scope for v1.

---

## 5  Measurement Workflow

### Overview

All critical dimensions are initially set to **assumed** values (A1-A19). Once you measure the real values on-site, you can feed them into the model without editing Python code.

### How to replace assumptions with measurements

**Step 1 — Copy the example file:**

```powershell
Copy-Item config\measurements.example.yaml config\measurements.yaml
```

**Step 2 — Fill in measured values:**

Open `config/measurements.yaml` in any text editor. Replace each assumed value with your measured value and change the matching `_status` field from `assumed` to `measured`. Example:

```yaml
main_window_width_mm: 2340    # measured with tape measure
main_window_status: measured
```

**Step 3 — Regenerate the model:**

```powershell
& "C:\Users\Rchie\Music\FreeCADModeling\FreeCAD\.pixi\envs\default\Library\bin\FreeCADCmd.exe" -c "g={'__file__':'generate_model.py','__name__':'__main__'}; exec(compile(open('generate_model.py', encoding='utf-8').read(), 'generate_model.py', 'exec'), g)"
```

**Step 4 — Check the design calculations:**

```powershell
type generated\design_calculations.json
```

The JSON shows spool revolutions, torque safety factor, open time, and remaining warnings for assumed values.

### Priority order for replacing assumptions

| Priority | Field(s) to measure | Blocker ID | Impact |
|---|---|---|---|
| 1 | `main_window_width_mm` | U1 | Spool revolutions, curtain travel |
| 2 | `rod_outer_diameter_mm`, `rod_type` | U2/U3 | Bracket bore, ring carrier |
| 3 | `eyelet_inner_diameter_mm` | U4 | Cord-to-ring clearance |
| 4 | `koof_depth_mm` | U5 | Bracket arm length |
| 5 | `curtain_mass_kg` | U6 | Torque safety factor verification |
| 6 | `motor_mode` | U7 | Controller type selection |

### What remains out of scope

- Servo-to-curtain cord coupling in Gazebo: requires a constraint plugin or ROS 2 controller. The SDF (`worlds/autocurtains_world.sdf`) keeps the spool and curtain as independently actuated joints.
- Electronics and controller wiring: not part of mechanical v1.
- Second window (right): modelled visually only; no actuator planned for v1.

---

## 9  Simulation Levels (v2.3)

The Gazebo simulation has three distinct levels of cord-coupling fidelity.
They are not interchangeable.

Current verification status (2026-05-30):
- Deterministic dry-run and assertion flow: verified without Gazebo.
- Docker headless Gazebo live command path: verified with
  `freecad-gazebo-mcp-e2e:latest` and Gazebo Sim 8.12.0. Both
  `visual-open-close` and `demo-open-close` completed against
  `worlds/autocurtains_world.sdf`.
- Gazebo GUI visual inspection: not verified on this machine.

| Level | Name | Implemented | Description |
|---|---|---|---|
| 1 | Independent joints | v1 baseline | Spool and curtain are driven by separate `gz topic` commands. No coupling. |
| 2 | Kinematic controller | v2.2 | `scripts/sim_curtain_controller.py` publishes matching commands using `position = clamp(angle * radius, 0, travel)`. Coupling is computed in Python; cord physics are not modelled. |
| 3 | Physical cord plugin | Future | A Gazebo constraint plugin (e.g. cable/rope system) would model cord tension, wrapping geometry, and elastic behaviour. Out of scope for current version. |

### Level 1 -- Independent joints (v1 baseline)

Spool and curtain panel are driven by separate `gz topic` commands with no
coupling between them. Each joint can be exercised independently for basic
kinematics demonstration.

### Level 2 -- Kinematic controller (v2.2)

`scripts/sim_curtain_controller.py` couples them using:

```
curtain_position_m = clamp(spool_angle_rad * spool_radius_m, 0.0, curtain_travel_m)
```

This is a **kinematic approximation**. It assumes:
- No cord stretch or slip
- Instantaneous position tracking (no inertia lag)
- No friction or tension modelling
- Curtain travel limited to `[0, curtain_travel_m]`

The controller writes its motion plan to `generated/sim_motion_plan.json`
for inspection without requiring Gazebo (`dry-run` mode).

### Level 2b -- Telemetry and assertion layer (v2.3)

Building on the kinematic controller, v2.3 adds:

- **`generated/sim_telemetry.json`** (schema 2.3): deterministic time-series
  with phase names (`open`, `hold_open`, `close`, `hold_closed`), signed
  spool velocity, curtain velocity, and normalised open fraction.
- **`scripts/assert_sim_motion.py`**: loads telemetry and verifies 7 geometric
  and temporal assertions (monotonicity, bounds, start/end conditions, timing).
  Writes `generated/sim_assertions.json`. Exits 0 on pass, 1 on failure.
  Does not require Gazebo.
- **`config/sim_scenarios.example.yaml`**: named scenario presets
  (`default_open_close`, `slow_open_close`, `short_travel_demo`). Use
  `--scenario NAME` with the controller to load a scenario's parameters.
  CLI flags still override scenario values.

None of these additions change the coupling model: it remains kinematic.
`physical_cord_coupling_simulated` is `false` in all generated outputs.

### Level 3 -- Physical cord plugin (future)

A physical cord simulation would require:
- A Gazebo cable/rope constraint plugin connecting spool and curtain link
- Cord material properties: Young's modulus, cross-section, max tension
- Wrapping geometry: spool radius, cord attachment point on curtain
- Friction model: cord-on-rail, eyelet ring friction
- Dynamic response: inertia of curtain mass + cord mass

This is out of scope for AutoCurtains v2.x. The kinematic controller provides
a visually plausible demo suitable for checking joint limits and timing.

---

## 8  Readiness Terminology (v2.0-pre)

The project uses three distinct readiness levels. They are not interchangeable.

| Term | Field | Set by | Meaning |
|---|---|---|---|
| `physical_build_recommended` | `generated/build_readiness.json` | `generate_model.py` | All U1-U7 measurements are marked `measured`, no validation errors, torque margin >= 2.0x, all STEP exports present. Does **not** mean bench tests are done. |
| `installation_recommended` | `generated/readiness_dashboard.json` | `scripts/readiness_dashboard.py` | All of the above **plus** all BT-1 through BT-7 bench tests pass and safety checks confirmed. This is the minimum bar for supervised curtain attachment. |
| `build_verified` | Never set automatically | Human judgement only | On-site inspection confirms the full assembly is safe for unsupervised permanent installation. No script can grant this status. |

### Why they are separate gates

`physical_build_recommended` says the design is mechanically correct for the measured site.
It does **not** say you have tested the electronics, measured current draw, or confirmed the emergency stop works.

`installation_recommended` adds those bench-test requirements. It is produced by
`scripts/readiness_dashboard.py`, which aggregates all available JSON reports and
config files into a single verdict.

`build_verified` is intentionally left to human judgement. It covers long-term durability,
permanent anchoring, and on-site behaviour that no automated check can replace.

### Dashboard commands (v2.0-pre)

Check overall readiness at any time (no FreeCAD needed):

```powershell
python scripts/readiness_dashboard.py
```

Check bench test status only:

```powershell
python scripts/validate_bench_tests.py
```

Both scripts exit 0 when their respective gate passes, and nonzero otherwise.
The dashboard writes `generated/readiness_dashboard.json` for machine-readable consumption.

---

## 9  Serviceability and Inspection Assumptions (v2.0-visual-fit)

### A-SVC-1: Minimum packaging clearances

The following clearances are **assumed** minimum values for assembly serviceability.
They are enforced by `generate_model.py` and reported in `generated/inspection_report.json`.
Adjust after physical inspection if real constraints differ.

| Constant | Value | Meaning |
|---|---|---|
| `MIN_SERVO_GEAR_CLEARANCE_MM` | 5.0 mm | Driven gear outer to MG995 body |
| `MIN_SPOOL_SERVO_CLEARANCE_MM` | 5.0 mm | Spool flange to MG995 body |
| `MIN_NUT_SERVICE_CLEARANCE_MM` | 10.0 mm | Axial wrench-access beyond axle support |
| `MIN_GEAR_BRACKET_CLEARANCE_MM` | 5.0 mm | Gear face to bracket surface |

These constraints drove the `GEAR_MODULE` change from 1.0 to 1.5 (v2.0-visual-fit).
With module 1.5 (25T+50T): gear centre distance = 56.25 mm, driven gear outer radius = 39 mm,
resulting in driven-gear-to-servo clearance ~7.25 mm and spool-flange-to-servo ~21 mm.

### Inspection envelope objects

Five reference-only objects are created in `AutoCurtains.FCStd` for visual packaging inspection:
`Servo_Clearance_Box`, `Drive_Gear_Clearance_Disc`, `Driven_Gear_Clearance_Disc`,
`Spool_Clearance_Cylinder`, `Axle_Service_Envelope`.
None of these are exported as printable STEP parts.
See `generated/inspection_report.json` and `doc/assembly_guide.md` Part 1.0.
