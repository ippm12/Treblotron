from pathlib import Path

NAME = 'cricket/turns'

def run(context, render=False):
    exe, output = context.executable(NAME, [Path(__file__).with_name('turns_test.cpp')], fonts=True)
    print(context.run(exe, output, render=True).stdout, end='')
