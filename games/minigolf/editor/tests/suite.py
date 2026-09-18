from pathlib import Path
import shutil
import subprocess
NAME = 'minigolf/editor'

def run(context, render=False):
    source = Path(__file__).parent
    exe, output = context.executable(NAME, [source / 'document_test.cpp', source.parent / 'document.cpp'], fonts=True)
    shutil.copytree(context.build / 'bin/assets/games/minigolf/courses', output / 'assets/games/minigolf/courses', dirs_exist_ok=True)
    result = context.run(exe, output, render=True)
    print(result.stdout, end='')
    if render:
        subprocess.run([context.ninja, '-C', str(context.build), 'TreblotronCourseEditor'], env=context.env, check=True)
        screenshot = output / 'editor.bmp'
        result = context.run(context.build / 'bin/TreblotronCourseEditor.exe', output, '--smoke', screenshot, render=True)
        assert screenshot.stat().st_size > 1000
        print('PASS editor window and playtest smoke check')
