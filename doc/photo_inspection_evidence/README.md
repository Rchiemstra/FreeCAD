# Photo-inspection gate evidence

This directory is the evidence index for the nine implementation phases in
`doc/photo_print_inspection_plan.md`. Source code, synthetic tests, or a Docker
sentinel alone cannot close a physical, packaging, privacy, safety, or
metrology gate.

For each evaluated gate, add an immutable evidence manifest containing:

- main-repository commit and complete dirty-path hash manifest;
- OpenCV, Qt, OCCT, compiler, operating-system, package, camera, printer,
  driver, media, viewer, and policy versions that apply;
- exact test IDs and raw result artifact hashes;
- preregistered thresholds and whether the run was training or blinded;
- deviations, exclusions, failures, incidents, and rerun relationships;
- reviewer and approval identifiers; and
- one outcome: `pass`, `fail`, `narrowed`, or `no-go`.

Current source work deliberately does not include fabricated physical evidence.
Until the Phase 0/3/5/6 campaigns are performed, validated camera and printer
evidence must keep conformance decisions `Inconclusive`. Phase 7 production
entry is closed until the planar release gate passes. Phase 8A requires an
independent 3D feasibility dataset and review; Phase 8B stays disabled unless
that review passes.

The isolated Docker validator must not read or write this directory as if it
were laboratory evidence. It may only validate schemas, synthetic fixtures,
builds, and deterministic software behavior.

The automated PDF conformance test parses emitted A4/A3 media boxes and
requires the nearest representable whole PostScript-point size (maximum error
half a point, about 0.1764 mm). This measured serialization quantization is an
uncertainty contribution; it does not replace printing and measuring the
100/200 mm reference rulers through each supported viewer and driver path.
