#!/usr/bin/env python3
"""Isolated physical-size experiments; run from a VS x86 tools prompt.

Never edits production source. RET variants are deliberately unsafe, not build
options. Results and assembled sources remain in --out for inspection/testing.
"""
import argparse
import csv
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def candidates(source):
    yield 'baseline', source
    yield 'tail-exit', source.replace('call dword ptr [_imp__ExitProcess@4]', 'jmp dword ptr [_imp__ExitProcess@4]')
    ret = source.replace('    push eax\n    call dword ptr [_imp__ExitProcess@4]', '    ret')
    yield 'unsafe-ret', ret
    yield 'unsafe-ret-split', ret.replace('    jmp exitProcess', '    ret')
    # GUI failure falls through into the common exit, retaining zero in EBP.
    gui = source.replace('launched:\nifdef ONEKB_CONSOLE_BOOTSTRAP', 'ifndef ONEKB_CONSOLE_BOOTSTRAP\nfailed:\n    inc ebp\nendif\nlaunched:\nifdef ONEKB_CONSOLE_BOOTSTRAP')
    gui = gui.replace('else\n    xchg eax,ebp                   ; GUI returns zero\nendif\n    jmp exitProcess\n\nfailed:\n    inc eax                        ; both failed CreateProcess calls return zero\nexitProcess:', '    jmp exitProcess\nfailed:\n    inc eax\nelse\n    xchg eax,ebp\nendif\nexitProcess:')
    yield 'fallthrough', gui
    yield 'fallthrough-tail', gui.replace('call dword ptr [_imp__ExitProcess@4]', 'jmp dword ptr [_imp__ExitProcess@4]')
    yield 'dec-retry', gui.replace('sub byte ptr [edi+16],1', 'dec byte ptr [edi+16]')
    yield 'test-or', gui.replace('test eax,eax', 'or eax,eax')
    yield 'push-zero', gui.replace('push ebp', 'push 0')
    yield 'exit-ret-stack', gui.replace('call dword ptr [_imp__ExitProcess@4]', 'push dword ptr [_imp__ExitProcess@4]\n    ret')
    direct = gui.replace('else\n    xchg eax,ebp\nendif\nexitProcess:', 'endif\nexitProcess:').replace('    push eax\n    call dword ptr [_imp__ExitProcess@4]', 'ifdef ONEKB_CONSOLE_BOOTSTRAP\n    push eax\nelse\n    push ebp\nendif\n    call dword ptr [_imp__ExitProcess@4]')
    yield 'direct-status', direct
    stack = direct.replace('    push edi\n    push dword ptr [edi]\n    call dword ptr [_imp__GetExitCodeProcess@8]\n    mov eax,dword ptr [edi]\n    jmp exitProcess\nfailed:\n    inc eax', '    push dword ptr [edi]\n    push esp\n    push dword ptr [edi]\n    call dword ptr [_imp__GetExitCodeProcess@8]\n    jmp exitProcess\nfailed:\n    inc eax\n    push eax').replace('ifdef ONEKB_CONSOLE_BOOTSTRAP\n    push eax\nelse\n    push ebp\nendif', 'ifndef ONEKB_CONSOLE_BOOTSTRAP\n    push ebp\nendif')
    yield 'stack-status', stack
    zero = stack.replace('    push dword ptr [edi]\n    push esp', '    push ebp\n    push esp')
    yield 'stack-status-zero', zero
    yield 'stack-status-one', stack.replace('    push dword ptr [edi]\n    push esp', '    push 1\n    push esp')
    early = zero.replace('ifndef ONEKB_CONSOLE_BOOTSTRAP\nfailed:\n    inc ebp\nendif', 'failed:\nifdef ONEKB_CONSOLE_BOOTSTRAP\n    inc eax\n    push eax\n    jmp exitProcess\nelse\n    inc ebp\nendif').replace('    jmp exitProcess\nfailed:\n    inc eax\n    push eax', '')
    yield 'early-failure', early
    shared = zero.replace("    mov byte ptr [esi+eax*2],'r'\n    mov edi,offset processScratch", "    lea edi,[esi+eax*2]\n    mov al,'r'\n    stosb\n    inc edi")
    yield 'unsafe-adjacent-scratch', shared
    yield 'unsafe-adjacent-scratch-store', shared.replace("    mov al,'r'\n    stosb\n    inc edi", "    mov byte ptr [edi],'r'\n    inc edi\n    inc edi")
    yield 'retry-carry', zero.replace('sub byte ptr [edi+16],1\n    jpo failed', 'add byte ptr [edi+16],-1\n    jc failed')
    yield 'retry-sign', zero.replace('sub byte ptr [edi+16],1\n    jpo failed', 'inc byte ptr [edi+16]\n    test byte ptr [edi+16],2\n    jnz failed')
    for offset in range(240, 289):
        yield f'fine-{offset}', zero.replace('align9_272', f'align9_{offset}').replace('align9_256', f'align9_{offset}')
    cache = direct[:direct.index('ifdef ONEKB_CONSOLE_BOOTSTRAP\n    ; The canonical path')] + '    jmp failed\n\n' + direct[direct.index('ifndef ONEKB_CONSOLE_BOOTSTRAP\nfailed:'):]
    start = cache.index('URLDATA SEGMENT')
    end = cache.index('URLDATA ENDS') + len('URLDATA ENDS')
    cache = cache[:start] + cache[end:]
    yield 'unsafe-cache-only', cache
    yield 'unsafe-cache-only-ret', cache.replace('    call dword ptr [_imp__ExitProcess@4]', '    pop eax\n    ret')
    yield 'initialized-cb', direct.replace('tryStart:\n', 'tryStart:\n    mov byte ptr [edi],68\n')
    yield 'separate-startup', zero.replace('aUrl dw ONEKB_RUNTIME_URL_WIDE,0', 'aUrl dw ONEKB_RUNTIME_URL_WIDE,0\nstartupInfo dd 68,16 dup(0)').replace('    push edi                       ; zeroed STARTUPINFOW, aliased intentionally', '    push offset startupInfo        ; documented, disjoint STARTUPINFOW')
    yield 'unsafe-ret-direct', direct.replace('    call dword ptr [_imp__ExitProcess@4]', '    pop eax\n    ret')
    for offset in range(0, 512, 16):
        yield f'offset-{offset}', gui.replace('align9_272', f'align9_{offset}').replace('align9_256', f'align9_{offset}')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT / 'tests/bootstrap-size-baseline.asm')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--ordertries', type=int, default=1000)
    p.add_argument('--filter', default='', help='only candidate names containing this string')
    p.add_argument('--standard', action='store_true', help='also measure conventional unpacked PE adapters')
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    with (args.out / 'sizes.csv').open('w', newline='') as f:
        writer = csv.writer(f)
        keys = ['gui', 'console', 'gui-icon', 'console-icon']
        if args.standard:
            keys += ['gui-standard', 'console-standard']
        writer.writerow(['candidate', *keys])
        for name, source in candidates(args.source.read_text()):
            if args.filter not in name:
                continue
            work = (args.out / name).resolve()
            work.mkdir()
            asm = work / 'bootstrap.asm'
            asm.write_text(source)
            subprocess.run(['py', '-3', str(ROOT / 'tools/write-runtime-url-inc.py'), str(ROOT / 'src/Config.h'), str(work / 'runtime-url.inc')], check=True)
            sizes = {}
            with (work / 'build.log').open('w') as log:
                for console in (False, True):
                    kind = 'console' if console else 'gui'
                    obj = work / (kind + '.obj')
                    subprocess.run(['ml.exe', '/nologo', '/c', '/coff', f'/I{work}', f'/Fo{obj}', *(['/DONEKB_CONSOLE_BOOTSTRAP'] if console else []), str(asm)], check=True, stdout=log, stderr=log)
                    for icon in (False, True):
                        key = kind + ('-icon' if icon else '')
                        exe = work / (key + '.exe')
                        tool = ROOT / 'third_party' / ('crinkler-icon' if icon else 'crinkler-bootstrap') / 'Win32/Crinkler.exe'
                        subprocess.run([str(tool), '/CRINKLER', '/ENTRY:BootstrapEntry', '/NODEFAULTLIB', '/UNALIGNCODE', '/OVERRIDEALIGNMENTS', '/TINYHEADER', '/TINYIMPORT', f'/ORDERTRIES:{args.ordertries}', f'/OUT:{exe}', '/SUBSYSTEM:' + ('CONSOLE' if console else 'WINDOWS'), str(obj), 'kernel32.lib', 'urlmon.lib'], check=True, stdout=log, stderr=log)
                        sizes[key] = exe.stat().st_size
                    if args.standard:
                        adapter = work / 'standard.obj'
                        subprocess.run(['ml.exe', '/nologo', '/c', '/coff', f'/Fo{adapter}', str(ROOT / 'tests/bootstrap-standard-entry.asm')], check=True, stdout=log, stderr=log)
                        exe = work / (kind + '-standard.exe')
                        subprocess.run(['link.exe', '/nologo', '/ENTRY:StandardEntry@0', '/NODEFAULTLIB', '/DYNAMICBASE', '/NXCOMPAT', '/OPT:REF', '/OPT:ICF', f'/OUT:{exe}', '/SUBSYSTEM:' + ('CONSOLE,6.0' if console else 'WINDOWS,6.0'), str(adapter), str(obj), 'kernel32.lib', 'urlmon.lib'], check=True, stdout=log, stderr=log)
                        sizes[kind + '-standard'] = exe.stat().st_size
            row = [name, *(sizes[k] for k in keys)]
            writer.writerow(row)
            f.flush()
            print(*row, flush=True)


if __name__ == '__main__':
    main()
