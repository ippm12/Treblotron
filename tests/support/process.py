"""Bounded child-process execution with per-suite output capture."""
import subprocess

def run_process(command, *, cwd, env, timeout=60, check=True):
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            capture_output=True, timeout=timeout)
    with (cwd / 'stdout.log').open('a', encoding='utf-8') as output:
        output.write(result.stdout)
    with (cwd / 'stderr.log').open('a', encoding='utf-8') as output:
        output.write(result.stderr)
    if check and result.returncode:
        raise RuntimeError(f"{command[0]} exited {result.returncode}: {result.stdout} {result.stderr}")
    return result
