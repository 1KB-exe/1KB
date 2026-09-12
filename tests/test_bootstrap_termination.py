#!/usr/bin/env python3
"""Positive/negative shutdown control; unsafe experiments are killed and reaped."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from test_bootstrap_cores import ROOT, build_cores


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT / 'src/bootstrap-x86.asm')
    p.add_argument('--capture-runtime', type=Path, required=True)
    p.add_argument('--expect-hang', action='store_true')
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='1KB termination ') as temporary:
        work = Path(temporary)
        cache = work / 'cache'
        cache.mkdir()
        shutil.copyfile(args.capture_runtime, cache / 'r')
        for name, exe, console in build_cores(work, 'valid', 9, args.source.resolve()):
            if not name.endswith('-threaded'):
                continue
            output = work / (name + '.txt')
            env = os.environ.copy()
            env.update(TMP=str(cache), BOOTSTRAP_TEST_OUTPUT=str(output))
            env.pop('BOOTSTRAP_TEST_WAIT_FILE', None)
            process = subprocess.Popen([str(exe)], env=env)
            try:
                end = time.monotonic() + 10
                while not output.exists() and time.monotonic() < end:
                    time.sleep(0.02)
                assert output.exists(), name
                try:
                    code = process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    assert args.expect_hang, name
                    try:
                        with exe.open('r+b'):
                            pass
                    except PermissionError:
                        print(f'EXPECTED FAILURE {name}: surviving thread keeps process alive and image write-locked')
                    else:
                        raise AssertionError((name, 'image unexpectedly writable'))
                else:
                    assert not args.expect_hang and code == (37 if console else 0), (name, code)
                    with exe.open('r+b'):
                        pass
                    print(f'PASS {name}: terminated and image writable')
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)


if __name__ == '__main__':
    main()
