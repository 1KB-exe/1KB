#!/usr/bin/env python3
"""Cached handoff tests for complete files from measure-bootstrap-launchers.py."""
import argparse
import csv
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--measurements', type=Path, required=True)
    p.add_argument('--capture-runtime', type=Path, required=True)
    args = p.parse_args()
    root = args.measurements.resolve()
    with (root / 'sizes.csv').open(newline='') as report:
        rows = list(csv.DictReader(report))
    for index, row in enumerate(rows):
        launcher = root / row['launcher']
        work = root / f'test unicode \u03bb {index}'
        work.mkdir()
        shutil.copyfile(args.capture_runtime, work / 'r')
        output, wait = work / 'output', work / 'wait'
        wait.touch()
        env = os.environ.copy()
        env.update(TMP=str(work), BOOTSTRAP_TEST_OUTPUT=str(output), BOOTSTRAP_TEST_WAIT_FILE=str(wait))
        process = subprocess.Popen([str(launcher), 'argument with spaces', '\u03bb'], env=env)
        try:
            end = time.monotonic() + 10
            while not output.exists() and time.monotonic() < end:
                time.sleep(0.02)
            assert output.exists(), row
            console = row['kind'].startswith('console')
            if console:
                time.sleep(0.1)
                assert process.poll() is None, row
            else:
                assert process.wait(timeout=5) == 0, row
        finally:
            wait.unlink()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
        assert process.returncode == (37 if console else 0), row
        text = output.read_text(encoding='utf-16-le')
        assert str(launcher) in text and 'argument with spaces' in text and '\u03bb' in text, row
        with launcher.open('r+b'):
            pass
    print(f'PASS {len(rows)} complete launchers: Unicode handoff, GUI detachment, console wait/exit, unlocked images')


if __name__ == '__main__':
    main()
