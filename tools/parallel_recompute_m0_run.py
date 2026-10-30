# SPDX-License-Identifier: LGPL-2.1-or-later
"""Local serial M0 runner. Build is read-only; all artifacts go to logs."""
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time

REPO = Path(__file__).resolve().parents[1]
BUILD = Path('/home/rch/Documents/code/FreeCAD-integration/build/relWithDebInfo')
ROOT = Path(os.environ.get('M0_RUN_ROOT', REPO / '../logs/m0-runtime')).resolve()
CASES = ['box:2', 'box:512'] + [f'{f}:{n}' for f in ['cut', 'fuse', 'common', 'pair'] for n in [24, 128, 512]] + ['chain:50', 'chain:200', 'bodies:20', 'bodies:100', 'bodies:500']
FIXTURES = ['data/examples/PartDesignExample.FCStd', 'tests/src/Mod/PartDesign/App/TestModels/TwoLengthsPadWithExpression.FCStd', 'tests/src/Mod/PartDesign/App/TestModels/TwoLengthsPadCrossObjectRef.FCStd', 'tests/src/Mod/PartDesign/App/TestModels/ModelFromV021.FCStd', 'src/Mod/Sketcher/SketcherTests/TestSketchCarbonCopyReverseMapping.FCStd'] + [f'data/examples/{n}.FCStd' for n in ['AssemblyExample', 'EngineBlock', 'BIMExample', 'FEMExample']]


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')


def task_start():
    return datetime.fromisoformat(json.loads((ROOT / 'campaign.json').read_text())['started_utc']).timestamp()


def environment(out):
    env = os.environ.copy()
    runtime = env.pop('XDG_RUNTIME_DIR', '/run/user/1000')
    (out / 'tmp').mkdir(exist_ok=True)
    env.update(DBUS_SESSION_BUS_ADDRESS=f'unix:path={runtime}/bus',
               FREECAD_USER_HOME=str(out), XDG_CACHE_HOME=str(out / 'cache'),
               XDG_CONFIG_HOME=str(out / 'config'), XDG_DATA_HOME=str(out / 'data'),
               HOME=str(out), TMPDIR=str(out / 'tmp'), PYTHONDONTWRITEBYTECODE='1', QT_QPA_PLATFORM='offscreen')
    return env


def command(out, args, env, timeout=7200):
    remaining = task_start() + 5 * 3600 - 600 - time.time()
    if remaining <= 0:
        raise RuntimeError('measurement deadline reached; reserve ten minutes for reporting')
    timeout = min(timeout, remaining)
    unit = f'm0-{os.getpid()}-{time.monotonic_ns()}.scope'
    cmd = ['systemd-run', '--user', '--scope', '-q', '--unit', unit, '-p', 'MemoryMax=5G',
           '/usr/bin/time', '-v', '-o', str(out / 'time.txt'), 'taskset', '-c', '0-3'] + args
    save(out / 'command.json', {'argv': cmd, 'cwd': str(out), 'environment_overrides': {k: v for k, v in env.items() if k.startswith(('M0_', 'FREECAD_', 'XDG_', 'QT_', 'DBUS_', 'PYTHONDONT')) or k in ['HOME', 'TMPDIR']}})
    start = time.monotonic()
    started = datetime.now(timezone.utc).isoformat()
    with (out / 'console.log').open('w') as log:
        try:
            result = subprocess.run(cmd, env=env, cwd=out, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            status = {'exit_code': result.returncode, 'elapsed_s': time.monotonic() - start}
        except subprocess.TimeoutExpired:
            subprocess.run(['systemctl', '--user', 'stop', unit], env=env, check=False)
            status = {'exit_code': None, 'timeout': True, 'elapsed_s': time.monotonic() - start}
        except KeyboardInterrupt:
            subprocess.run(['systemctl', '--user', 'stop', unit], env=env, check=False)
            raise
    status.update(started_utc=started, finished_utc=datetime.now(timezone.utc).isoformat(), loadavg=Path('/proc/loadavg').read_text().strip())
    save(out / 'process.json', status)
    return status


def probe(group, case, scenario='all', warmups=1, repetitions=3, source=None, name=None):
    out = ROOT / group / (name or f'{case.replace(":", "-")}-{scenario}')
    out.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(REPO / 'tools/parallel_recompute_m0_probe.py', out / 'probe.FCMacro')
    env = environment(out)
    env.update(M0_CASE=case, M0_SCENARIO=scenario, M0_WARMUPS=str(warmups), M0_REPETITIONS=str(repetitions), M0_OUT=str(out), M0_BUILD_SHA='ec84cacee3440461e142c29ba9cfa6a7aac21453')
    if source:
        env['M0_FILE'] = str(source)
    status = command(out, [str(BUILD / 'bin/FreeCADCmd'), '-u', str(out / 'user.cfg'), '-s', str(out / 'system.cfg'), str(out / 'probe.FCMacro')], env)
    try:
        records = [json.loads(line) for line in (out / 'samples.jsonl').read_text().splitlines()]
        samples = [r for r in records if r['kind'] == 'sample' and not r['warmup']]
        status['complete'] = status['exit_code'] == 0 and records[-1].get('completed', False) and len(samples) == repetitions
        status['objects'] = records[0]['objects']
        if samples:
            status['median_ms'] = statistics.median(r['recompute_ns'] for r in samples) / 1e6
    except (OSError, ValueError, KeyError):
        status['complete'] = False
    if (out / 'restore.json').exists():
        status['objects'] = json.loads((out / 'restore.json').read_text())['objects']
    console = (out / 'console.log').read_text(errors='replace')
    errors = [line for line in console.splitlines() if re.search(r'No module named|ModuleNotFoundError|ImportError|unrestored proxies|Exception while processing file', line)]
    if errors:
        status['complete'] = False
        status['errors'] = errors
    save(out / 'status.json', status)
    print(out.relative_to(ROOT), json.dumps(status), flush=True)
    return status


def provenance():
    ROOT.mkdir(parents=True, exist_ok=True)
    if (ROOT / 'build-manifest.json').exists():
        raise FileExistsError('use a fresh M0_RUN_ROOT to preserve the earlier provenance')
    save(ROOT / 'campaign.json', {'started_utc': datetime.now(timezone.utc).isoformat(), 'budget_s': 18000})
    paths = [BUILD / 'bin/FreeCADCmd', BUILD / 'lib/libFreeCADApp.so', BUILD / 'Mod/Part/Part.so']
    data = {'label': 'local build, not CI', 'source_sha': subprocess.check_output(['git', '-C', str(BUILD.parents[1]), 'rev-parse', 'HEAD'], text=True).strip(), 'fixture_source_sha': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(), 'fixture_note': 'Fixtures come from this clone source, not the ec84cacee3 build source.', 'build_directory': str(BUILD), 'artifacts': {}}
    shutil.copyfile(BUILD / 'CMakeCache.txt', ROOT / 'CMakeCache.txt')
    data['cmake_build_type'] = next(line.split('=', 1)[1] for line in (BUILD / 'CMakeCache.txt').read_text().splitlines() if line.startswith('CMAKE_BUILD_TYPE:'))
    for path in paths:
        linkage = subprocess.check_output(['ldd', str(path)], text=True)
        data['artifacts'][str(path)] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'ldd': linkage}
    data['fixtures'] = {f: hashlib.sha256((REPO / f).read_bytes()).hexdigest() for f in FIXTURES}
    for name, cmd in [('cpu.txt', ['lscpu']), ('host-before.txt', ['cat', '/proc/meminfo', '/proc/loadavg'])]:
        (ROOT / name).write_text(subprocess.check_output(cmd, text=True))
    save(ROOT / 'build-manifest.json', data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('stage', choices=['provenance', 'smoke', 'estimate', 'full', 'files', 'cold', 'native'])
    parser.add_argument('--group')
    parser.add_argument('--repetitions', type=int, default=30)
    args = parser.parse_args()
    if args.stage in ['full', 'files'] and args.repetitions < 10:
        parser.error('full/file measurements require at least 10 repetitions')
    ROOT.mkdir(parents=True, exist_ok=True)
    lock = (ROOT / 'runner.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if args.stage in ['smoke', 'full', 'files', 'cold', 'native']:
        active = subprocess.check_output(['systemctl', '--user', 'list-units', '--type=scope', '--state=running', '--no-legend', '--plain', '--no-pager'], env=environment(ROOT), text=True)
        if str(ROOT) in active:
            raise RuntimeError('an earlier M0 scope is still running; inspect it before restarting')
    if args.stage == 'provenance':
        provenance()
    elif args.stage == 'estimate':
        observed = []
        for case in CASES:
            out = ROOT / (args.group or 'smoke-serial') / f'{case.replace(":", "-")}-all'
            status = json.loads((out / 'status.json').read_text())
            if not status['complete']:
                raise RuntimeError(f'incomplete smoke: {case}')
            samples = [json.loads(line) for line in (out / 'samples.jsonl').read_text().splitlines() if json.loads(line)['kind'] == 'sample']
            measured = [s for s in samples if not s['warmup']]
            mean_action_s = statistics.mean(s['action_total_ns'] for s in measured) / 1e9
            fixed_s = max(0, status['elapsed_s'] - sum(s['action_total_ns'] for s in samples) / 1e9)
            observed.append({'case': case, 'mean_action_s': mean_action_s, 'fixed_s': fixed_s})
        # Deliberately price edit and noop at the all-dirty cost; their costs
        # cannot be observed from the required all-only smoke runs.
        fixed = 3 * sum(r['fixed_s'] for r in observed)
        per_sample = 3 * sum(r['mean_action_s'] for r in observed)
        remaining = 5 * 3600 - (time.time() - task_start())
        reserve = 45 * 60  # file fixtures, cold processes, suites, reporting
        count = min(30, max(10, int((remaining - reserve - fixed) / per_sample) - 5))
        estimate = {'method': 'three scenarios each priced at smoke all-dirty mean action time plus fixed process/creation/restore/verification cost', 'observed': observed, 'remaining_s': remaining, 'reserve_s': reserve, 'full_30_estimate_s': fixed + 35 * per_sample, 'selected_repetitions': count, 'selected_estimate_s': fixed + (5 + count) * per_sample}
        save(ROOT / 'estimate.json', estimate)
        print(json.dumps(estimate, indent=2))
    elif args.stage in ['smoke', 'full']:
        for case in CASES:
            for scenario in (['all'] if args.stage == 'smoke' else ['all', 'edit', 'noop']):
                group = args.group or ('smoke-serial' if args.stage == 'smoke' else 'full')
                probe(group, case, scenario, 1 if args.stage == 'smoke' else 5, 3 if args.stage == 'smoke' else args.repetitions)
    elif args.stage == 'files':
        for file in FIXTURES:
            for scenario in ['all', 'noop']:
                probe(args.group or 'files', 'file', scenario, 5, args.repetitions, REPO / file, f'{Path(file).stem}-{scenario}')
    elif args.stage == 'cold':
        for i in range(30):
            probe('cold', 'file', 'all', 0, 1, ROOT / 'full/pair-24-all/fixture.FCStd', f'pair-24-{i+1:02}')
    elif args.stage == 'native':
        suites = {'App': 'DocumentRecomputeCoordinatorTest.*:GenericIsolatedRecomputeTest.*:NativeRecomputeRequestCaptureTest.*:DocumentCommitCoordinator*', 'Part': 'ShapeCacheIsolatedRecomputeTest.*', 'Gui': 'PartPresentationCaptureTest.UnchangedDatumExtentsPreserveRevisionFence'}
        for suite, filters in suites.items():
            out = ROOT / 'native' / suite
            out.mkdir(parents=True)
            env = environment(out)
            cmd = [str(BUILD / f'tests/{suite}_tests_run'), f'--gtest_filter={filters}', f'--gtest_output=xml:{out}/results.xml']
            if suite == 'Gui':
                env['QT_QPA_PLATFORM'] = 'xcb'
                cmd = ['xvfb-run', '-a'] + cmd
            print(suite, command(out, cmd, env), flush=True)


if __name__ == '__main__':
    main()
