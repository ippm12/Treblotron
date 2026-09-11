from pathlib import Path
NAME = 'game_lib/turn_skipping'

def run(context, render=False):
    exe, output = context.executable(NAME, [Path(__file__).with_name('turn_skipping_test.cpp')])
    print(context.run(exe, output).stdout, end='')
