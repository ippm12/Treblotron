from pathlib import Path
NAME = 'debug/logging'

def run(context, render=False):
    exe, output = context.executable(NAME, [Path(__file__).with_name('durability_test.cpp')])
    for tag in ('first', 'second'):
        result = context.run(exe, output, tag, check=False, timeout=30)
        assert result.returncode == 73, (result.returncode, result.stderr)
        main = (output / 'logs/treblotron_0_Main.log').read_text()
        assert f'{tag} LAST_CRITICAL' in main
        assert sum(f'{tag} worker=' in line for line in main.splitlines()) == 10000
        for channel, marker in (('1_Frame', 'LAST_VERBOSE'), ('2_GameManager', 'LAST_INFO')):
            assert f'{tag} {marker}' in (output / f'logs/treblotron_{channel}.log').read_text()
    assert 'first LAST_CRITICAL' in (output / 'logs/treblotron_0_Main.1.log').read_text()
    context.run(exe, output, 'lifecycle', timeout=30)
    assert 'REINITIALIZED' in (output / 'logs/treblotron_0_Main.log').read_text()
    print('PASS: 10,000 concurrent messages per abrupt exit, final verbose/info/critical messages, restart retention, shutdown/reinitialization')
