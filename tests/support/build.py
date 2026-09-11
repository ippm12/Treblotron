"""Build test executables against the configured application's actual objects.

The GCC/Ninja link-command adapter is kept here until dedicated CMake test
targets replace it. Suites never need to inspect build internals.
"""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
from .process import run_process

def tokens(command):
    return shlex.split(command.replace(chr(92), '/'))

class BuildContext:
    def __init__(self, root, build):
        self.root, self.build = root, build
        cache = {}
        for line in (build / 'CMakeCache.txt').read_text().splitlines():
            if '=' in line and ':' in line and not line.startswith('//'):
                key, value = line.split('=', 1)
                cache[key.split(':', 1)[0]] = value
        self.ninja = cache['CMAKE_MAKE_PROGRAM']
        self.compiler = cache['CMAKE_CXX_COMPILER']
        if 'ninja' not in Path(self.ninja).name.lower() or Path(self.compiler).name.lower() in ('cl.exe', 'clang-cl.exe'):
            raise RuntimeError('These integration suites currently require a GCC-compatible Ninja app build.')
        self.env = dict(os.environ)
        self.env['PATH'] = str(build / 'bin') + os.pathsep + str(Path(self.compiler).parent) + os.pathsep + self.env.get('PATH', '')
        subprocess.run([self.ninja, '-C', str(build), 'bin/Treblotron.exe'], env=self.env, check=True)
        entries = json.loads((build / 'compile_commands.json').read_text())
        self.entry = next(e for e in entries if e['file'].replace(chr(92), '/').endswith('/startup.cpp'))
        commands = subprocess.check_output([self.ninja, '-C', str(build), '-t', 'commands', 'bin/Treblotron.exe'], env=self.env, text=True)
        link = next(part.strip() for line in reversed(commands.splitlines()) for part in line.split(' && ')
                    if 'startup.cpp.obj' in part and ' -o ' in part)
        self.link = tokens(link)

    def directory(self, name):
        output = (self.build / 'tests' / name).resolve()
        if not output.is_relative_to((self.build / 'tests').resolve()):
            raise ValueError('Suite output must remain under build/tests')
        output.mkdir(parents=True, exist_ok=True)
        (output / 'portable.txt').write_text('Isolated test data')
        return output

    def executable(self, name, sources, *, private_access=False, fonts=False):
        output = self.directory(name)
        objects = output / 'objects'
        objects.mkdir(exist_ok=True)
        template = list(self.entry.get('arguments') or tokens(self.entry['command']))
        # Do not overwrite dependency files belonging to the application.
        cleaned = []
        skip = False
        for arg in template:
            if skip:
                skip = False
                continue
            if arg in ('-MF', '-MT', '-MQ'):
                skip = True
            elif arg not in ('-MD', '-MMD'):
                cleaned.append(arg)
        def compile_one(source):
            obj = objects / (source.stem + '.obj')
            command = list(cleaned)
            command[command.index('-o') + 1] = str(obj)
            command[command.index('-c') + 1] = str(source)
            command.append('-UNDEBUG')
            if private_access:
                command.append('-fno-access-control')
            subprocess.run(command, cwd=self.entry['directory'], env=self.env, check=True, timeout=120)
            return obj
        with ThreadPoolExecutor(max_workers=4) as pool:
            compiled = list(pool.map(compile_one, sources))
        command = []
        for arg in self.link:
            if arg.endswith('/startup.cpp.obj'):
                command.extend(map(str, compiled))
            elif not arg.startswith('-Wl,--out-implib,'):
                command.append(arg)
        exe = output / 'test.exe'
        command[command.index('-o') + 1] = str(exe)
        subprocess.run(command, cwd=self.build, env=self.env, check=True, timeout=180)
        if fonts:
            shutil.copytree(self.build / 'bin/assets/fonts', output / 'assets/fonts', dirs_exist_ok=True)
        return exe, output

    def run(self, exe, output, *arguments, check=True, timeout=60, render=False):
        env = dict(self.env)
        if render:
            env.update(SDL_VIDEODRIVER='dummy', SDL_RENDER_DRIVER='software')
        return run_process([str(exe), *map(str, arguments)], cwd=output,
                           env=env, timeout=timeout, check=check)
