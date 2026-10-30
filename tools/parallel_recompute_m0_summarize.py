# SPDX-License-Identifier: LGPL-2.1-or-later
"""Summarize retained M0 artifacts; never runs FreeCAD."""
import json
import hashlib
import math
from pathlib import Path
import re
import statistics
import xml.etree.ElementTree as ET

from parallel_recompute_m0_run import ROOT, CASES


def quantile(values, p):
    values = sorted(values)
    position = (len(values) - 1) * p
    lo, hi = math.floor(position), math.ceil(position)
    return values[lo] + (values[hi] - values[lo]) * (position - lo)


def summarize(path):
    result = json.loads((path / 'status.json').read_text())
    result['path'] = str(path)
    result['command'] = json.loads((path / 'command.json').read_text())
    if (path / 'restore.json').exists():
        result['restore'] = json.loads((path / 'restore.json').read_text())
    records = [json.loads(line) for line in (path / 'samples.jsonl').read_text().splitlines()] if (path / 'samples.jsonl').exists() else []
    if records:
        result['manifest'] = records[0]
    samples = [r for r in records if r['kind'] == 'sample' and not r['warmup']]
    if samples:
        for field in ['recompute_ns', 'mutation_ns', 'action_total_ns', 'process_cpu_ns']:
            values = [r[field] / 1e6 for r in samples]
            result[field] = {'median_ms': statistics.median(values), 'p95_ms': quantile(values, .95)}
        result['cpu_wall'] = sum(r['process_cpu_ns'] for r in samples) / sum(r['recompute_ns'] for r in samples)
        result['returned_counts'] = sorted(set(r['returned_count'] for r in samples))
        result['dirty_counts'] = sorted(set(len(r['dirty_before']) for r in samples))
        result['measured_samples'] = len(samples)
    text = (path / 'time.txt').read_text() if (path / 'time.txt').exists() else ''
    match = re.search(r'Maximum resident set size \(kbytes\): (\d+)', text)
    result['peak_rss_mb'] = int(match[1]) / 1024 if match else None
    return result


def row(label, scenario, r):
    rss = f'{r["peak_rss_mb"]:.1f}' if r.get('peak_rss_mb') is not None else '—'
    if not r['complete']:
        return f'| {label} | {scenario} | {r.get("objects", "—")} | **FAILED** | — | — | {rss} |'
    return f'| {label} | {scenario} | {r["objects"]} | {r["recompute_ns"]["median_ms"]:.3f} | {r["recompute_ns"]["p95_ms"]:.3f} | {r["cpu_wall"]:.2f} | {rss} |'


def main():
    groups = {}
    for group in ['smoke-serial', 'full', 'files', 'cold']:
        groups[group] = {p.parent.name: summarize(p.parent) for p in sorted((ROOT / group).glob('*/status.json'))}
    native = {}
    expected_path = ROOT / 'native-expected-tests.json'
    expected = json.loads(expected_path.read_text())['suites'] if expected_path.exists() else {}
    for path in sorted((ROOT / 'native').glob('*/results.xml')):
        root = ET.parse(path).getroot()
        cases = root.findall('.//testcase')
        native[path.parent.name] = {'tests': len(cases), 'failures': sum(c.find('failure') is not None for c in cases), 'errors': len(root.findall('.//error')), 'skipped': len(root.findall('.//skipped')), 'not_run': sum(c.get('status') == 'notrun' for c in cases), 'names': [f'{c.attrib["classname"]}.{c.attrib["name"]}' for c in cases], 'process': json.loads((path.parent / 'process.json').read_text())}
        item = native[path.parent.name]
        item['missing_expected'] = sorted(set(expected.get(path.parent.name, [])) - set(item['names']))
        item['unexpected'] = sorted(set(item['names']) - set(expected.get(path.parent.name, [])))
    result = {'groups': groups, 'native': native, 'p95_method': 'linear interpolation at (n-1)*0.95', 'cpu_wall_method': 'sum process CPU inside recompute / sum wall inside recompute', 'rss_unit': 'MiB (time -v kbytes / 1024)'}
    cold = [r for r in groups['cold'].values() if r['complete']]
    if cold:
        metrics = {'recompute_ms': [r['recompute_ns']['median_ms'] for r in cold],
                   'restore_ms': [r['manifest']['load_or_create_ns'] / 1e6 for r in cold],
                   'scope_launch_to_exit_s': [r['elapsed_s'] for r in cold]}
        result['cold_aggregate'] = {key: {'median': statistics.median(v), 'p95': quantile(v, .95)} for key, v in metrics.items()}
        result['cold_aggregate']['processes'] = len(cold)
        result['cold_aggregate']['max_peak_rss_mb'] = max(r['peak_rss_mb'] for r in cold)
    manifests = [r['manifest'] for rs in groups.values() for r in rs.values() if r['complete']]
    if manifests:
        path = ROOT / 'build-manifest.json'
        build = json.loads(path.read_text())
        build['runtime_versions'] = {key: manifests[0][key] for key in ['freecad_version', 'occ_version', 'python', 'qt_version']}
        build['runtime_version_consistent'] = all(all(m[key] == manifests[0][key] for key in build['runtime_versions']) for m in manifests)
        build['policy'] = {'affinity': '0-3', 'scope_memory_max': '5G', 'occ_inner_parallelism': 'existing default; native wrappers request SetRunParallel(true); no inner-one override', 'timing': 'headless serial document recompute, default preferences recorded per run'}
        build['artifact_hashes_still_match'] = all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == a['sha256'] for p, a in build['artifacts'].items())
        path.write_text(json.dumps(build, indent=2) + '\n')
    (ROOT / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    table = ['| Case | Scenario | Objects | Recompute median ms | p95 ms | CPU/wall | Peak RSS MB |', '| --- | --- | ---: | ---: | ---: | ---: | ---: |']
    for case in CASES:
        for scenario in ['all', 'edit', 'noop']:
            key = f'{case.replace(":", "-")}-{scenario}'
            if key in groups['full']:
                table.append(row(f'`{case}`', scenario, groups['full'][key]))
    table += ['', 'File fixtures (same columns):', '', table[0], table[1]]
    for key, r in groups['files'].items():
        source = r['command']['environment_overrides'].get('M0_FILE', key)
        table.append(row(Path(source).name, r.get('manifest', {}).get('scenario', key.rsplit('-', 1)[-1]), r))
    (ROOT / 'tables.md').write_text('\n'.join(table) + '\n')
    edits = ['| Case | Mutation median ms | Recompute median ms | Action total median ms | Initially touched | Returned count |', '| --- | ---: | ---: | ---: | ---: | ---: |']
    for case in CASES:
        r = groups['full'].get(f'{case.replace(":", "-")}-edit')
        if r and r['complete']:
            edits.append(f'| `{case}` | {r["mutation_ns"]["median_ms"]:.3f} | {r["recompute_ns"]["median_ms"]:.3f} | {r["action_total_ns"]["median_ms"]:.3f} | {r["dirty_counts"]} | {r["returned_counts"]} |')
    (ROOT / 'edit-timings.md').write_text('\n'.join(edits) + '\n')
    print(json.dumps({'counts': {g: {'total': len(rs), 'complete': sum(r['complete'] for r in rs.values())} for g, rs in groups.items()}, 'native': native}, indent=2))


if __name__ == '__main__':
    main()
