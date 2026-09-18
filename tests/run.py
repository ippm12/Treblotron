"""Run repository tests by owning suite, for example: python tests/run.py minigolf --render."""
import argparse
import importlib.util
from pathlib import Path
import sys
from support.build import BuildContext

def main():
    root = Path(__file__).resolve().parents[1]
    files = sorted({*root.glob('games/**/tests/suite.py'),
                    *root.glob('tools/*/tests/suite.py'),
                    *root.glob('*/tests/*/suite.py'),
                    *root.glob('*/tests/suite.py'),
                    *root.glob('tests/integration/*/suite.py')})
    suites = {}
    for i, path in enumerate(files):
        spec = importlib.util.spec_from_file_location(f'test_suite_{i}', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        if module.NAME in suites:
            raise RuntimeError(f'Duplicate suite: {module.NAME}')
        suites[module.NAME] = module
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('suite', nargs='?', default='all', help='Suite name or group prefix; default: all')
    parser.add_argument('--build-dir', type=Path, default=root / 'build')
    parser.add_argument('--render', action='store_true', help='Include supported headless rendering checks')
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args()
    if args.list:
        print(chr(10).join(suites))
        return 0
    selected = {name: suite for name, suite in suites.items()
                if args.suite == 'all' or name == args.suite or name.startswith(args.suite + '/')}
    if not selected:
        parser.error('Unknown suite. Use --list to see available suites.')
    context = BuildContext(root, args.build_dir.resolve())
    failures = []
    for name, suite in selected.items():
        print(f'Running {name}', flush=True)
        try:
            suite.run(context, args.render)
        except Exception as error:
            failures.append(name)
            print(f'FAIL {name}: {error}', file=sys.stderr, flush=True)
    print(f'{len(selected)-len(failures)}/{len(selected)} suites passed', flush=True)
    return int(bool(failures))

if __name__ == '__main__':
    raise SystemExit(main())
