from pathlib import Path
import shutil
import subprocess
import sys
NAME = 'minigolf'

def run(context, render=False):
    source = Path(__file__).parent
    subprocess.run([sys.executable, str(source / 'course_layouts.py')], check=True)
    exe, output = context.executable(NAME, sorted(source.glob('*.cpp')), private_access=True, fonts=render)
    shutil.copytree(context.build / 'bin/assets/games/minigolf/courses', output / 'assets/games/minigolf/courses', dirs_exist_ok=True)
    arguments = []
    if render:
        images = output / 'renders'
        images.mkdir(exist_ok=True)
        arguments.append(images)
    result = context.run(exe, output, *arguments, render=render, timeout=120)
    print(result.stdout, end='')
