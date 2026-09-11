from pathlib import Path
NAME = 'debug/crash_reporting'

def run(context, render=False):
    exe, output = context.executable(NAME, [Path(__file__).with_name('capture_test.cpp')])
    import struct
    crashes = output / 'crashes'
    before = set(crashes.glob('*.dmp')) if crashes.exists() else set()
    context.run(exe, output, 'normal', timeout=30)
    assert set(crashes.glob('*.dmp')) == before
    for mode in ('seh', 'cpp', 'abort', 'thread', 'box2d'):
        before = set(crashes.glob('*.dmp'))
        run = context.run(exe, output, mode, check=False, timeout=30)
        assert run.returncode not in (0, 2, 3), (mode, run.returncode, run.stderr)
        created = set(crashes.glob('*.dmp')) - before
        assert len(created) == 1, (mode, created, run.stderr)
        dump = created.pop()
        data = dump.read_bytes()
        assert data[:4] == b'MDMP'
        count, directory = struct.unpack_from('<II', data, 8)
        exceptions = []
        for i in range(count):
            kind, size, offset = struct.unpack_from('<III', data, directory+i*12)
            if kind == 6:
                exceptions.append(struct.unpack_from('<I', data, offset+8)[0])
        assert exceptions == ([0xC0000005] if mode in ('seh', 'thread') else [0xE0000001]), (mode, exceptions)
        report = Path(str(dump)+'.txt').read_text()
        assert 'Dump saved: yes' in report, report
        snapshot = Path(str(dump)+'.exe')
        assert snapshot.stat().st_size == exe.stat().st_size
        if mode == 'box2d':
            assert 'Box2D assertion:' in report, report
        # This copy belongs to the synthetic test, not an actual game crash.
        snapshot.unlink()
        print(f'PASS {mode}: {dump.name}, {len(data)} bytes, exception {exceptions[0]:08x}')
    print('PASS: clean exit writes no dump; Windows/C++/abort/worker-thread failures produce valid exception streams')
