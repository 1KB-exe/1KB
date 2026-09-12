#!/usr/bin/env python3
"""Measure complete builder outputs using test-bootstrap.cmd's capture fixtures.

Example: --builder baseline=old.exe --builder final=1KB.exe
         --fixtures .1KB-test --out ignore/launcher-measurements
Output directory must not exist. Does not execute downloaded software.
"""
import argparse
import csv
import os
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--builder', action='append', required=True, help='label=path')
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fixtures = args.fixtures.resolve()
    gui = bytearray((fixtures / 'capture-no-icon.exe').read_bytes())
    pe = struct.unpack_from('<I', gui, 0x3c)[0]
    struct.pack_into('<H', gui, pe + 24 + 68, 2)
    gui_path = out / 'capture-no-icon-gui.exe'
    gui_path.write_bytes(gui)
    applications = {'gui': gui_path, 'console': fixtures / 'capture-no-icon.exe',
                    'gui-icon': fixtures / 'capture-gui.exe', 'console-icon': fixtures / 'capture-runtime.exe'}
    identities = {'github': 'gh:1kb-exe/1kb', 'url': 'https://example.com/1KB.ini#capture-runtime'}
    with (out / 'sizes.csv').open('w', newline='') as report:
        writer = csv.writer(report)
        writer.writerow(['builder', 'identity', 'kind', 'bytes', 'launcher'])
        for spec in args.builder:
            label, builder = spec.split('=', 1)
            builder = Path(builder).resolve()
            for identity, app_id in identities.items():
                for kind, application in applications.items():
                    local = out / label / identity / kind
                    local.mkdir(parents=True)
                    env = os.environ.copy()
                    env.update(LOCALAPPDATA=str(local), TMP=str(local))
                    result = subprocess.run([str(builder), str(application)], env=env,
                                            input=f'{app_id}\n\n\nq\n', capture_output=True,
                                            text=True, errors='replace', timeout=60)
                    (local / 'builder.log').write_text(result.stdout + result.stderr, encoding='utf-8')
                    assert result.returncode == 0, local
                    records = list((local / '1kb').rglob('app.ini'))
                    assert len(records) == 1, local
                    launcher = records[0].parent / (application.stem + '.exe')
                    row = [label, identity, kind, launcher.stat().st_size, str(launcher.relative_to(out))]
                    writer.writerow(row)
                    report.flush()
                    print(*row, flush=True)


if __name__ == '__main__':
    main()
