; Frozen incoming size-pass baseline: 403/420 bytes, 457/474 icon-capable.
; Used only by tools/sweep-bootstrap.py, never by production builds.
; Tiny 32-bit bootstrap for WOW64. No CRT and no launcher-overlay parsing.
; The customized Crinkler resolver enters with EAX=0 and
; EBX=PEB.ProcessParameters. The conventional linked image is only a resource
; staging template and is never executed.
.386
.model flat, stdcall
option casemap:none
ASSUME FS:NOTHING

EXTERN _imp__ExitProcess@4:DWORD
EXTERN _imp__GetTempPathW@8:DWORD
EXTERN _imp__WaitForSingleObject@8:DWORD
EXTERN _imp__CreateProcessW@40:DWORD
ifdef ONEKB_CONSOLE_BOOTSTRAP
EXTERN _imp__GetExitCodeProcess@8:DWORD
endif
EXTERN _imp__URLDownloadToCacheFileW@24:DWORD
includelib kernel32.lib
includelib urlmon.lib

INFINITE equ -1
MAX_TEMP_PATH_CHARS equ 32767
; PROCESS_INFORMATION and STARTUPINFOW deliberately alias. The latter remains
; zero initialized: current Windows accepts cb=0, saving its initialization.
SCRATCH_BYTES equ 68
ifdef ONEKB_CONSOLE_BOOTSTRAP
ROOT_CHARS equ MAX_TEMP_PATH_CHARS*2+1+SCRATCH_BYTES/2
endif

; Separate data hunks let Crinkler choose their compressed ordering.
URLDATA SEGMENT BYTE PUBLIC 'DATA'
include runtime-url.inc
aUrl dw ONEKB_RUNTIME_URL_WIDE,0
URLDATA ENDS

BOOTBSS SEGMENT DWORD PUBLIC 'BSS'
; /OVERRIDEALIGNMENTS recognizes these suffixes. Per-variant virtual-only
; offsets improve address compression without increasing the file size.
; Selected by a size sweep before adding explicit process termination.
ifdef ONEKB_CONSOLE_BOOTSTRAP
rootPath_align9_256 dw ROOT_CHARS dup(?)
else
rootPath_align9_272 dw MAX_TEMP_PATH_CHARS+1 dup(?)
processScratch dw SCRATCH_BYTES/2 dup(?)
endif
BOOTBSS ENDS

.code
BootstrapEntry PROC
ifdef ONEKB_CONSOLE_BOOTSTRAP
    mov esi,offset rootPath_align9_256
else
    mov esi,offset rootPath_align9_272
endif
    ; ESI as the path base avoids EBP's extra zero displacement in the LEA.
    xchg eax,ebp                   ; preserve a one-byte source of zero
    push esi
    push MAX_TEMP_PATH_CHARS
    call dword ptr [_imp__GetTempPathW@8]
ifdef ONEKB_CONSOLE_BOOTSTRAP
    lea edi,[esi+eax*2]
    mov al,'r'
    stosb
    inc edi                        ; skip the zero high byte into scratch
else
    ; The high byte and following terminator are already zero in BSS.
    mov byte ptr [esi+eax*2],'r'
    mov edi,offset processScratch
endif

tryStart:
    push edi                       ; PROCESS_INFORMATION
    push edi                       ; zeroed STARTUPINFOW, aliased intentionally
    push ebp
    push ebp
    push ebp
ifdef ONEKB_CONSOLE_BOOTSTRAP
    push 1                         ; inherit console handles
else
    push ebp
endif
    push ebp
    push ebp
    push dword ptr [ebx+44h]       ; ProcessParameters.CommandLine.Buffer
    push esi
    call dword ptr [_imp__CreateProcessW@40]
    test eax,eax
    jnz launched

ifdef ONEKB_CONSOLE_BOOTSTRAP
    ; The canonical path is below scratch; the cache path is above it.
    cmp esi,edi
    ja failed
else
    ; dwX is ignored without STARTF_USEPOSITION and is outside the aliased
    ; PROCESS_INFORMATION. 00 -> FF (even parity) -> FE (odd parity).
    ; SUB compresses better than the one-byte-shorter DEC here.
    sub byte ptr [edi+16],1
    jpo failed
endif
    ; URLMon owns synchronization for its completed cache file. The recovery
    ; runtime atomically promotes its unique cache path to the canonical path.
ifdef ONEKB_CONSOLE_BOOTSTRAP
    lea esi,[edi+SCRATCH_BYTES]
endif
    push ebp
    push ebp
    push MAX_TEMP_PATH_CHARS
    push esi
    push offset aUrl
    push ebp
    call dword ptr [_imp__URLDownloadToCacheFileW@24]
    jmp tryStart

launched:
ifdef ONEKB_CONSOLE_BOOTSTRAP
    push INFINITE
    push dword ptr [edi]
    call dword ptr [_imp__WaitForSingleObject@8]
    push edi
    push dword ptr [edi]
    call dword ptr [_imp__GetExitCodeProcess@8]
    mov eax,dword ptr [edi]
else
    xchg eax,ebp                   ; GUI returns zero
endif
    jmp exitProcess

failed:
    inc eax                        ; both failed CreateProcess calls return zero
exitProcess:
    ; RET only exits this thread; URLMon/WinINet threads can keep us alive.
    push eax
    call dword ptr [_imp__ExitProcess@4]
BootstrapEntry ENDP

END BootstrapEntry
