from pathlib import Path
NAME = 'minigolf'

def run(context, render=False):
    source = Path(__file__).parent
    exe, output = context.executable(NAME, sorted(source.glob('*.cpp')), private_access=True, fonts=render)
    arguments = []
    if render:
        images = output / 'renders'
        images.mkdir(exist_ok=True)
        arguments.append(images)
    result = context.run(exe, output, *arguments, render=render, timeout=120)
    print(result.stdout, end='')
