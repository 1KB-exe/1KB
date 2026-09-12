"""Adversarial ZIP fixtures for the production ZIP reader."""
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import warnings
import zipfile

warnings.filterwarnings('ignore', message='Duplicate name:', category=UserWarning)


def make(entries):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for entry, data in entries:
            archive.writestr(entry, data)
    return out.getvalue()


def main():
    exe = str(Path(sys.argv[1]).resolve())
    cases = []
    for name in ('../escape', '/absolute', 'C:/absolute', 'a\\..\\escape', 'a:stream',
                 'CON.txt', 'nul', 'COM1.exe', 'a.', 'a ', 'a//b', '.update/journal',
                 'current.txt', '1KB.UPDATES.INI', 'a~1', 'a|b', 'a\x01b', 'unicode-\u212a'):
        cases.append(make([(name, b'evil')]))
    for names in (('a', 'A'), ('a', 'a'), ('a', 'a/b'), ('Dir/a', 'dir/A')):
        cases.append(make([(name, b'data') for name in names]))
    link = zipfile.ZipInfo('link')
    link.create_system = 3
    link.external_attr = 0o120777 << 16
    cases.append(make([(link, b'../escape'), ('link/escape', b'data')]))
    cases.append(make([('not-empty/', b'hidden data')]))
    valid = make([('safe.txt', b'safe')])
    mismatch = bytearray(valid)
    mismatch[30] = ord('x')  # central/local name mismatch
    cases.append(mismatch)
    corrupt = bytearray(valid)
    corrupt[40] ^= 0x80
    cases.append(corrupt)
    cases.append(valid[:-1])
    cases.append(b'not a zip')
    with tempfile.TemporaryDirectory(prefix='1kb-zip-security-') as temp:
        root = Path(temp)
        good = root / 'good.zip'
        good.write_bytes(valid)
        assert subprocess.run([exe, 'extract', str(good), str(root / 'good')]).returncode == 0

        # The advertised file limit must not be accidentally constrained by the
        # much smaller public-manifest limit.
        many = root / 'many.zip'
        many.write_bytes(make([(f'files/{i:04}.txt', b'x') for i in range(1000)]))
        assert subprocess.run([exe, 'extract', str(many), str(root / 'many')]).returncode == 0

        # Exercise the LZMA method emitted by the manager when 7-Zip is present.
        seven_zip = shutil.which('7z.exe')
        if seven_zip:
            source = root / 'lzma-source'
            source.mkdir()
            (source / 'data.txt').write_bytes(b'lzma payload' * 1000)
            lzma = root / 'lzma.zip'
            subprocess.run([seven_zip, 'a', '-tzip', '-mm=LZMA', '-bd', '-bso0',
                            str(lzma), 'data.txt'], cwd=source, check=True)
            extracted = root / 'lzma'
            assert subprocess.run([exe, 'extract', str(lzma), str(extracted)]).returncode == 0
            assert (extracted / 'data.txt').read_bytes() == b'lzma payload' * 1000

        for i, data in enumerate(cases):
            archive = root / f'bad-{i}.zip'
            archive.write_bytes(data)
            result = subprocess.run([exe, 'extract', str(archive), str(root / f'out-{i}')], timeout=10)
            assert result.returncode != 0, i
        assert not (root / 'escape').exists()
    print(f'{len(cases)} malicious ZIP fixtures rejected')


if __name__ == '__main__':
    main()
