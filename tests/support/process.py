"""Bounded child-process execution with per-suite output capture."""
import subprocess

def run_process(command, *, cwd, env, timeout=60, check=True):
    # Redirect straight to disk: output survives a timeout or runner interruption.
    with (cwd / 'stdout.log').open('ab') as output, (cwd / 'stderr.log').open('ab') as errors:
        start_out, start_err = output.tell(), errors.tell()
        result = subprocess.run(command, cwd=cwd, env=env, stdout=output,
                                stderr=errors, timeout=timeout)
    def captured(path, start):
        with path.open('rb') as stream:
            stream.seek(start)
            return stream.read().decode('utf-8', errors='replace').replace('\r\n', '\n')
    result.stdout = captured(cwd / 'stdout.log', start_out)
    result.stderr = captured(cwd / 'stderr.log', start_err)
    if check and result.returncode:
        raise RuntimeError(f"{command[0]} exited {result.returncode}: {result.stdout} {result.stderr}")
    return result
