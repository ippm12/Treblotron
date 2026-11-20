from pathlib import Path
NAME = 'detect/collection'

def run(context, render=False):
    exe, output = context.executable(NAME, [Path(__file__).with_name('collection_test.cpp')])
    print(context.run(exe, output).stdout, end='')
