#!/usr/bin/env python3
"""Exercise ALL -> FreeCADMainCmd -> install with the real staging graph."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
fixture = source / 'tests/bundled_pivy_staging/stubs_fixture'
helper = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else source / 'cMake/FreeCAD_Helpers'
configs = ('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')
pivy = source / 'src/3rdParty/pivy'

def typing_files(package):
    return {p.relative_to(package): p.read_bytes() for p in package.rglob('*')
            if p.is_file() and (p.suffix == '.pyi' or p.name == 'py.typed')}

with tempfile.TemporaryDirectory(prefix='pivy-target-order-') as tmp:
    root = Path(tmp)
    wrong = root / 'wrong/pivy'
    wrong.mkdir(parents=True)
    (wrong / '__init__.py').write_text('')
    (wrong / 'coin.py').write_text('CONFIG="WRONG_ORIGIN"\n')
    stubgen = root / 'stubgen'
    stubgen.write_text(f'''#!{sys.executable}
import importlib, pathlib, sys
m = importlib.import_module("pivy.coin")
p = pathlib.Path(sys.argv[sys.argv.index("-o") + 1]) / "pivy" / "coin.pyi"
p.write_text("# config " + m.CONFIG + "\\n")
''')
    stubgen.chmod(0o755)
    env = dict(os.environ, PYTHONPATH=str(wrong.parent))
    def run(*args):
        subprocess.run(args, check=True, env=env)
    fallback = typing_files(pivy / 'pivy')
    assert fallback and Path('coin.pyi') in fallback
    for mode, program in (('ON', str(stubgen)), ('OFF', str(stubgen)), ('ON', '')):
        build = root / (mode + ('-found' if program else '-missing'))
        run('cmake', '-S', str(fixture), '-B', str(build), '-G', 'Ninja Multi-Config',
            '-DCMAKE_CONFIGURATION_TYPES=' + ';'.join(configs),
            f'-DHELPER_DIR={helper}', f'-DPIVY_SOURCE={pivy}',
            f'-DPYTHON={sys.executable}', f'-DPIVY_GENERATE_PYI={mode}', f'-DSTUBGEN={program}')
        assert f'PIVY_GENERATE_PYI:BOOL={mode}' in (build / 'CMakeCache.txt').read_text()
        for config in configs + configs:
            run('cmake', '--build', str(build), '--config', config)
            package = build / config / 'Mod/pivy'
            expected = typing_files(package)
            if mode == 'ON' and program:
                assert f'# config {config}'.encode() in expected[Path('coin.pyi')]
                assert Path('_coin.pyi') in expected and Path('py.typed') in expected
                # A stale result must not pass simply because ALL ran earlier.
                (package / 'coin.pyi').write_text('# config WRONG_STALE\n')
            else:
                assert expected == fallback
            run('cmake', '--build', str(build), '--config', config, '--target', 'FreeCADMainCmd')
            assert typing_files(package) == expected, (mode, config, 'named target')
            # Reuse one prefix to expose stale products when switching configurations.
            prefix = build / 'install'
            run('cmake', '--install', str(build), '--config', config, '--prefix', str(prefix))
            assert typing_files(prefix / 'Mod/pivy') == expected, (mode, config, 'install')
        print(f'PASS {mode} {"found" if program else "missing"}: {configs}, repeated', flush=True)
print('Pivy named target and install graph passed')
