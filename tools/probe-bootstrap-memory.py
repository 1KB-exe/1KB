#!/usr/bin/env python3
"""Print live image mappings while a console bootstrap waits for the capture fixture."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


class MemoryInfo(c.Structure):
    # This probe requires 64-bit Python, including when inspecting WOW64.
    _fields_ = [('base', c.c_void_p), ('allocation', c.c_void_p),
                ('allocation_protect', w.DWORD), ('partition', w.WORD),
                ('size', c.c_size_t), ('state', w.DWORD), ('protect', w.DWORD), ('type', w.DWORD)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture-runtime', required=True, type=Path)
    parser.add_argument('images', nargs='+', type=Path)
    args = parser.parse_args()
    assert c.sizeof(c.c_void_p) == 8, 'use 64-bit Python'
    kernel, psapi = c.WinDLL('kernel32', use_last_error=True), c.WinDLL('psapi', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.POINTER(c.c_void_p), w.DWORD, c.POINTER(w.DWORD), w.DWORD]
    kernel.VirtualQueryEx.argtypes = [w.HANDLE, c.c_void_p, c.POINTER(MemoryInfo), c.c_size_t]
    kernel.VirtualQueryEx.restype = c.c_size_t
    for image in args.images:
        with tempfile.TemporaryDirectory(prefix='1KB mappings ') as temporary:
            work = Path(temporary)
            shutil.copyfile(args.capture_runtime, work / 'r')
            wait, output = work / 'wait', work / 'output'
            wait.touch()
            env = os.environ.copy()
            env.update(TMP=str(work), BOOTSTRAP_TEST_OUTPUT=str(output), BOOTSTRAP_TEST_WAIT_FILE=str(wait))
            process = subprocess.Popen([str(image.resolve())], env=env)
            handle = None
            try:
                end = time.monotonic() + 10
                while not output.exists() and time.monotonic() < end:
                    time.sleep(0.02)
                assert output.exists() and process.poll() is None, 'console bootstrap must be waiting'
                handle = kernel.OpenProcess(0x410, False, process.pid)
                assert handle, c.get_last_error()
                modules, needed = (c.c_void_p * 1024)(), w.DWORD()
                assert psapi.EnumProcessModulesEx(handle, modules, c.sizeof(modules), c.byref(needed), 3), c.get_last_error()
                base = at = modules[0]
                print(image, 'base', hex(base))
                while True:
                    info = MemoryInfo()
                    assert kernel.VirtualQueryEx(handle, at, c.byref(info), c.sizeof(info)), c.get_last_error()
                    if info.allocation != base:
                        break
                    print(f'  RVA={info.base-base:#x} size={info.size:#x} protection={info.protect:#x} state={info.state:#x}')
                    at = info.base + info.size
            finally:
                if handle:
                    kernel.CloseHandle(handle)
                wait.unlink()
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    raise


if __name__ == '__main__':
    main()
