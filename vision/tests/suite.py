from pathlib import Path

NAME = 'vision/detection_queue'

def run(context, render=False):
    exe, output = context.executable(NAME, [Path(__file__).with_name('detection_queue_test.cpp')])
    print(context.run(exe, output).stdout, end='')
