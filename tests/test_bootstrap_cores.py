#!/usr/bin/env python3
"""Exercise assembly cores directly, including offline URLMon recovery failures."""
import argparse
import http.server
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        mode = self.path.split('/')[1]
        body = self.server.runtime if mode == 'valid' else b'not an executable'
        self.send_response(404 if mode == 'missing' else 200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def build_cores(work, mode, port, source, standard=False):
    url = f'http://127.0.0.1:{port}/{mode}/runtime.exe'
    (work / 'runtime-url.inc').write_text(
        'ONEKB_RUNTIME_URL_WIDE equ <' + ','.join(map(str, map(ord, url))) + '>\n'
    )
    thread_obj = work / 'thread.obj'
    subprocess.run([
        'ml.exe', '/nologo', '/c', '/coff', f'/Fo{thread_obj}',
        str(ROOT / 'tests/bootstrap-lingering-thread.asm'),
    ], check=True, stdout=subprocess.DEVNULL)
    for console in (False, True):
        kind = 'console' if console else 'gui'
        obj = work / f'{kind}.obj'
        subprocess.run([
            'ml.exe', '/nologo', '/c', '/coff', f'/I{work}', f'/Fo{obj}',
            *(['/DONEKB_CONSOLE_BOOTSTRAP'] if console else []),
            str(source),
        ], check=True, stdout=subprocess.DEVNULL)
        for icon in ((False,) if standard else (False, True)):
            for lingering in (False, True):
                name = kind + ('-icon' if icon else '') + ('-threaded' if lingering else '')
                exe = work / f'{name}.exe'
                if standard:
                    adapter = work / 'standard.obj'
                    subprocess.run([
                        'ml.exe', '/nologo', '/c', '/coff', f'/Fo{adapter}',
                        *(['/DONEKB_THREADED_TEST'] if lingering else []),
                        str(ROOT / 'tests/bootstrap-standard-entry.asm'),
                    ], check=True, stdout=subprocess.DEVNULL)
                    subprocess.run([
                        'link.exe', '/nologo', '/ENTRY:StandardEntry@0', '/NODEFAULTLIB',
                        '/DYNAMICBASE', '/NXCOMPAT', '/OPT:REF', '/OPT:ICF',
                        f'/OUT:{exe}', '/SUBSYSTEM:' + ('CONSOLE,6.0' if console else 'WINDOWS,6.0'),
                        str(adapter), str(obj), *([str(thread_obj)] if lingering else []),
                        'kernel32.lib', 'urlmon.lib',
                    ], check=True, stdout=subprocess.DEVNULL)
                    yield name, exe, console
                    continue
                tool = 'crinkler-icon' if icon else 'crinkler-bootstrap'
                subprocess.run([
                    str(ROOT / 'third_party' / tool / 'Win32/Crinkler.exe'),
                    '/CRINKLER', '/ENTRY:' + ('ThreadedEntry' if lingering else 'BootstrapEntry'),
                    '/NODEFAULTLIB', '/UNALIGNCODE', '/OVERRIDEALIGNMENTS', '/TINYHEADER',
                    '/TINYIMPORT', '/ORDERTRIES:1000', f'/OUT:{exe}',
                    '/SUBSYSTEM:' + ('CONSOLE' if console else 'WINDOWS'),
                    str(obj), *([str(thread_obj)] if lingering else []),
                    'kernel32.lib', 'urlmon.lib',
                ], check=True, stdout=subprocess.DEVNULL)
                yield name, exe, console


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--capture-runtime', required=True, type=Path)
    parser.add_argument('--source', type=Path, default=ROOT / 'src/bootstrap-x86.asm')
    parser.add_argument('--standard', action='store_true', help='test unpacked PE with normal imports, ASLR and DEP')
    args = parser.parse_args()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.runtime = args.capture_runtime.read_bytes()
    server.requests = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='1KB cores ') as temporary:
            root = Path(temporary)
            for mode in ('missing', 'invalid', 'valid'):
                work = root / mode
                work.mkdir()
                for name, exe, console in build_cores(work, mode, server.server_port, args.source.resolve(), args.standard):
                    for canonical in ('valid', 'corrupt', 'missing'):
                        # Exactly 256 chars including GetTempPathW's trailing
                        # slash: AH is nonzero, but the fixture is below MAX_PATH.
                        parent = work / name / canonical / ('long path ' * 8).rstrip()
                        prefix = 'unicode \u03bb '
                        local = parent / (prefix + 'x' * (255 - len(str(parent)) - 1 - len(prefix)))
                        assert len(str(local)) == 255
                        local.mkdir(parents=True)
                        env = os.environ.copy()
                        env['TMP'] = str(local)
                        output = work / f'{name}-{canonical}-output.txt'
                        env['BOOTSTRAP_TEST_OUTPUT'] = str(output)
                        env.pop('BOOTSTRAP_TEST_WAIT_FILE', None)
                        cached = canonical == 'valid'
                        target = local / 'r'
                        if cached:
                            shutil.copyfile(args.capture_runtime, target)
                        elif canonical == 'corrupt':
                            target.write_bytes(b'not an executable')
                        before = len(server.requests)
                        success = cached or mode == 'valid'
                        command = [str(exe), 'argument with spaces', '\u03bb']
                        if mode == 'valid' and cached:
                            wait_file = work / 'wait'
                            wait_file.touch()
                            env['BOOTSTRAP_TEST_WAIT_FILE'] = str(wait_file)
                            result = subprocess.Popen(command, env=env)
                            try:
                                end = time.monotonic() + 10
                                while not output.exists() and time.monotonic() < end:
                                    time.sleep(0.02)
                                assert output.exists(), (name, 'child did not start')
                                if console:
                                    time.sleep(0.1)
                                    assert result.poll() is None, 'console must wait for child'
                                else:
                                    assert result.wait(timeout=5) == 0, 'GUI must detach'
                            finally:
                                wait_file.unlink()
                                try:
                                    result.wait(timeout=20)
                                except subprocess.TimeoutExpired:
                                    result.kill()
                                    result.wait()
                                    raise
                        else:
                            result = subprocess.run(command, env=env, timeout=20)
                        # A surviving DLL thread must not leave the image locked.
                        with exe.open('r+b'):
                            pass
                        assert result.returncode == ((37 if console else 0) if success else 1), (
                            mode, name, canonical, result.returncode,
                            server.requests[before:],
                            output.read_text(encoding='utf-16-le') if output.exists() else 'no child output',
                        )
                        if success:
                            end = time.monotonic() + 10
                            while time.monotonic() < end:
                                if output.exists() and output.stat().st_size:
                                    break
                                time.sleep(0.02)
                            assert output.exists(), (mode, name, canonical)
                            text = output.read_text(encoding='utf-16-le')
                            assert str(exe) in text and 'argument with spaces' in text and '\u03bb' in text
                        else:
                            assert not output.exists()
                        if cached:
                            assert len(server.requests) == before, 'cache hit must not download'
            print('PASS ' + ('standard GUI/console' if args.standard else 'all four cores') + ', with/without a lingering thread: Unicode/long paths, cached launch, recovery, invalid download, HTTP failure, GUI detachment, console waiting, unlocked image')
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == '__main__':
    main()
