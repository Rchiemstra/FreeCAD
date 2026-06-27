# RobotCAD export cache

Exports are cached under `generated/<robot>/.export_cache/` keyed by:

1. **FCStd SHA-256** (`robots/arm_2dof.FCStd`)
2. **Export policy fingerprint** — RobotCAD commit, CROSS version, cache schema, headless exporter id (from `config/runtime-versions.lock.yaml` when present)

## Layout

```
generated/arm_2dof/
  .export_cache/
    index.yaml
    entries/<cache_key>/
      manifest.yaml
      arm_2dof_description/...
```

## Behaviour

| Event | When |
| --- | --- |
| `cache_miss` | No entry, hash mismatch, policy change, or `BRIDGE_EXPORT_CACHE_INVALIDATE=1` |
| `cache_hit` | Valid entry restored into `generated/<robot>/` |
| `cache_store` | After successful FreeCADCmd export |
| `cache_invalidated` | Manual or hash/policy mismatch cleanup |

## Environment

| Variable | Default | Effect |
| --- | --- | --- |
| `BRIDGE_EXPORT_CACHE` | `1` | Set `0` to always export |
| `BRIDGE_EXPORT_CACHE_INVALIDATE` | unset | Set `1` to clear cache before next restore |

## Result metadata

- `metadata.source_hashes.fcstd` — FCStd digest for the run
- `input_hashes.fcstd` — same digest in `result.yaml`

Structured events appear in `run_events.yaml` under category `export_cache`.
