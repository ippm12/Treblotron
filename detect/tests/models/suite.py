from pathlib import Path

NAME = 'detect/models'


def run(context, render=False):
    exe, output = context.executable(NAME, [Path(__file__).with_name('model_contract_test.cpp')])
    print(context.run(exe, output).stdout, end='')
