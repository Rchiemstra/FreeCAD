# Robot source files (`robots/`)

## `arm_2dof` (Phase 1 toy robot)

| File | Role |
| --- | --- |
| `arm_2dof.urdf` | Hand-crafted placeholder URDF (metres, +Z up). Used for Gazebo spawn until RobotCAD export is run. |
| `arm_2dof.FCStd` | **Canonical FreeCAD source** with `Cross::Robot`, links, and joints. **Git-tracked** for reproducible Docker E2E. |

### Reproducibility (`arm_2dof.FCStd`)

Pins live in [`config/runtime-versions.lock.yaml`](../config/runtime-versions.lock.yaml):

| Field | Value |
| --- | --- |
| Path | `robots/arm_2dof.FCStd` |
| SHA-256 | `4619df8cdf0084266aebcec7a0e93556147dc9b8e26948545bad1a8472f9e075` |
| Size | 73 883 bytes |

Verify locally:

```powershell
python e2e\verify_robot_source.py
```

**CI without the file in checkout:** set `ROBOTS_ARM_2DOF_FCSTD_URL` to a URL serving the same bytes (hash must match), then:

```bash
bash e2e/fetch_robot_source.sh
```

Or set `robot_source.ci_artifact_url` in the lock file.

### Install RobotCAD / CROSS (OVERCROSS)

```powershell
.\scripts\install_robotcad_cross.ps1
```

Clones `drfenixion/freecad.overcross`, installs Python deps, initializes git submodules, and creates a `freecad.robotcad` junction under the pixi FreeCAD `data\Mod` tree (OVERCROSS still resolves `MOD_PATH` as `freecad.robotcad`).

Verify (FreeCADCmd):

```powershell
.\.pixi\envs\default\Library\bin\FreeCADCmd.exe scripts\verify_robotcad_cross.py
```

Expected: `RobotCAD/CROSS OK (freecad.cross version=1.0.1)`.

### Build `arm_2dof.FCStd`

From placeholder URDF (headless, after install):

```powershell
.\scripts\run_freecad_script.ps1 .\scripts\build_arm_2dof_fcstd.py
```

Or with GUI FreeCAD + MCP RPC on port **9875**:

```powershell
python scripts\build_arm_2dof_fcstd_rpc.py
```

### Export URDF (preferred: FreeCADCmd batch)

**Primary path** — no MCP GUI queue, no 120 s timeout:

```powershell
.\scripts\run_freecad_script.ps1 .\scripts\export_arm_2dof_fcstd.py
```

Or via the bridge (used by `handoff.py`):

```powershell
python -c "from pathlib import Path; from bridge.freecad_bridge import export_urdf; r=export_urdf('arm_2dof', Path('generated/arm_2dof'), fcstd_path=Path('robots/arm_2dof.FCStd')); print(r)"
```

Validate:

```powershell
python -c "from pathlib import Path; from bridge.validate import validate_urdf; from bridge.freecad_bridge import expected_exported_urdf_path; p=expected_exported_urdf_path('arm_2dof', Path('generated/arm_2dof')); print(validate_urdf(p))"
```

**Output layout** (OVERCROSS ROS description package):

`generated/arm_2dof/arm_2dof_description/arm_2dof_description/urdf/arm_2dof.urdf`

A flat copy may also appear at `generated/arm_2dof/arm_2dof.urdf` depending on export templates.

**Fallback** — MCP `execute_code` (GUI FreeCAD + RPC :9875), may hit the addon’s 120 s GUI timeout on long exports:

```powershell
python scripts\export_arm_2dof_rpc.py
```

### Implementation notes

| Component | Path |
| --- | --- |
| Headless export core | `scripts/robotcad_headless.py` |
| arm_2dof batch script | `scripts/export_arm_2dof_fcstd.py` |
| Bridge API | `bridge/freecad_bridge.py` → `export_urdf_cmd()`, `export_urdf(prefer_cmd=True)` |
| Docker / Linux E2E | `e2e/export_robotcad_fcstd.py` (same headless core) |

Headless export patches OVERCROSS for: `export_urdf(interactive=False)` `ignore` bug (source patch), overwrite dialog, `MOD_PATH`, git submodule skip, minimal controllers YAML.

### Minimal `Cross::*` requirement

The FCStd must contain at least one `Cross::Robot` (`freecad.cross.wb_utils.is_robot`). Building from `arm_2dof.urdf` via `robot_from_urdf_path` satisfies this.
