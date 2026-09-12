"""Opt-in 3 GiB extracted / ~702 MiB ZIP test with x86 memory measurement."""
import ctypes
from ctypes import wintypes
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


class Counters(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize',
        'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
        'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage')]


def measured(command):
    psapi = ctypes.WinDLL('psapi')
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    peak = 0
    with subprocess.Popen(command) as process:
        while process.poll() is None:
            stats = Counters()
            stats.cb = ctypes.sizeof(stats)
            if psapi.GetProcessMemoryInfo(int(process._handle), ctypes.byref(stats), stats.cb):
                peak = max(peak, stats.PeakWorkingSetSize)
            try:
                process.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                pass
        assert process.returncode == 0, command
    assert peak < 96 * 1024 * 1024, peak
    print(f'Peak working set: {peak / 1024 / 1024:.1f} MiB', flush=True)


def main():
    extract, crypto = [str(Path(p).resolve()) for p in sys.argv[1:3]]
    mib = 1024 * 1024
    expected = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix='1kb-large-') as temp:
        root = Path(temp)
        package = root / 'large.zip'
        random = os.urandom(mib)
        zeros = bytes(mib)
        with zipfile.ZipFile(package, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            with archive.open('data.bin', 'w', force_zip64=True) as output:
                for index in range(3072):
                    block = random if index < 700 else zeros
                    expected.update(block)
                    output.write(block)
        assert 700*mib < package.stat().st_size < 715*mib, package.stat().st_size
        print(f'ZIP: {package.stat().st_size/mib:.1f} MiB; extracted: 3072 MiB', flush=True)
        measured([extract, 'extract', str(package), str(root / 'extracted')])
        actual = root / 'extracted' / 'data.bin'
        assert actual.stat().st_size == 3*1024**3
        with actual.open('rb') as input_file:
            assert hashlib.file_digest(input_file, 'sha256').digest() == expected.digest()
        actual.unlink()
        measured([crypto, str(package)])
    print('Large x86 streaming ZIP and AES-GCM roundtrip passed')


if __name__ == '__main__':
    main()
