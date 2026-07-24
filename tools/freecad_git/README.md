# freecad-git

Deterministic Git sidecar generator for FreeCAD `.FCStd` documents.

## Overview

Every tracked FreeCAD document uses this pairing:

```text
Model.FCStd
Model.FCStd.git.json
```

The `.FCStd` file remains **authoritative**. The JSON sidecar is a **generated, never manually edited** review artifact containing stable document structure, parameters, expressions, constraints, links, object visibility, and other semantic information.

**The JSON is not a reconstruction format.** Merging JSON does not resolve binary model conflicts.

## Architecture

```text
FCStd (hostile ZIP input)
  → defensive archive validation
  → Document.xml semantic parser (defusedxml)
  → canonical in-memory model
  → deterministic JSON writer
  → schema validation
  → atomic sidecar write
```

Optional trusted diagnostics use sandboxed `FreeCADCmd` and are **never** a fallback for normal export or CI.

## Installation

```bash
cd tools/freecad_git
pip install -e ".[dev]"
```

## CLI Usage

```bash
freecad-git export path/to/Model.FCStd
freecad-git export --all
freecad-git export --stdout path/to/Model.FCStd
freecad-git check path/to/Model.FCStd
freecad-git check --all
freecad-git validate path/to/Model.FCStd.git.json
freecad-git diagnostics path/to/Model.FCStd
freecad-git --version
```

## Developer Workflow

```text
Save in FreeCAD
→ run freecad-git export
→ inspect the .FCStd.git.json diff
→ stage the .FCStd and sidecar together
→ commit
→ CI regenerates in memory and compares exact bytes
```

## Conflict Resolution

```text
Resolve the binary model in FreeCAD first.
Then regenerate the sidecar.
Never treat a successful JSON merge as a successful model merge.
```

## Configuration

Repository configuration is in `.freecad-git.toml` at the repo root. See that file for include/exclude globs, resource limits, external-reference policy, and diagnostic settings.

Security settings cannot disable DTD or external-entity rejection.

## Determinism Contract

- UTF-8 without BOM, LF newlines, exactly one final newline
- Two-space indentation, keys sorted by Unicode code-point order
- Finite numbers as canonical decimal strings in documented base units
- Placements as `position_mm` + normalized quaternion `rotation_xyzw`
- `source.semantic_sha256` over canonical content (no raw archive digest)
- Atomic write via temporary file + replacement

## Threat Model

The direct parser treats every `.FCStd` as hostile input. It never:

- Extracts to arbitrary filesystem paths
- Uses `eval`, pickle, or expression evaluation
- Opens external documents or accesses the network
- Launches FreeCAD during normal export/check/CI

## Optional Integrations

### FreeCAD post-save adapter

Install the add-on from `tools/freecad_git/freecad_addon/GitSidecar/`. Enable **Generate Git sidecar after save** in preferences. Sidecar failure is non-fatal to the model save.

### Pre-commit hook

An optional local hook runs `freecad-git check` on staged `.FCStd` files. CI remains authoritative.

### MCP integration

See `tools/mcp/freecad-mcp` for the opt-in MCP post-save adapter.

## Trusted Diagnostics Warning

`freecad-git diagnostics` opens documents in headless FreeCAD. This may load modules, linked files, workbenches, and solvers. Output is **non-authoritative** and excluded from mandatory CI on untrusted fork PRs.

## Reproducible ZIP Experiment

`freecad_git.repack.repack_deterministic()` demonstrates byte-identical ZIP repacking with fixed metadata. This does **not** stabilize semantic payloads, does not create readable PR diffs, and is not a replacement for JSON sidecars. It never rewrites authoritative `.FCStd` files in normal workflows.

## Schema Version Policy

Schema identifier: `freecad-git-sidecar/v1`. Normalization rule changes require generator version bump, schema update when compatibility is affected, golden fixture updates, and intentional sidecar regeneration.

## Git LFS

Git LFS is not enabled by default. It remains a separate policy for unusually large authoritative binaries.

## Regenerating All Sidecars

```bash
freecad-git export --all
git add **/*.FCStd **/*.FCStd.git.json
```

## License

LGPL-2.1-or-later (consistent with FreeCAD)
