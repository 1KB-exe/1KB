; Tiny 32-bit bootstrap for WOW64. No CRT and no launcher-overlay parsing.
; The customized Crinkler resolver enters with EAX=0 and
; EBX=PEB.ProcessParameters. The conventional linked image is only a resource
; staging template and is never executed.
.386
.model flat, stdcall
option casemap:none
ASSUME FS:NOTHING

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
ifdef ONEKB_CONSOLE_BOOTSTRAP
rootPath_align9_320 dw ROOT_CHARS dup(?)
else
rootPath_align9_447 dw MAX_TEMP_PATH_CHARS+1 dup(?)
processScratch dw SCRATCH_BYTES/2 dup(?)
endif
BOOTBSS ENDS

.code
BootstrapEntry PROC
ifdef ONEKB_CONSOLE_BOOTSTRAP
    mov ebp,offset rootPath_align9_320
else
    mov ebp,offset rootPath_align9_447
endif
    xchg eax,esi                   ; preserve a one-byte source of zero
    push ebp
    push MAX_TEMP_PATH_CHARS
    call dword ptr [_imp__GetTempPathW@8]
    lea edi,[ebp+eax*2]
    mov al,'r'
    stosb
ifdef ONEKB_CONSOLE_BOOTSTRAP
    inc edi                        ; skip the zero high byte into scratch
else
    mov edi,offset processScratch
endif

tryStart:
    push edi                       ; PROCESS_INFORMATION
    push edi                       ; zeroed STARTUPINFOW, aliased intentionally
    push esi
    push esi
    push esi
ifdef ONEKB_CONSOLE_BOOTSTRAP
    push 1                         ; inherit console handles
else
    push esi
endif
    push esi
    push esi
    push dword ptr [ebx+44h]       ; ProcessParameters.CommandLine.Buffer
    push ebp
    call dword ptr [_imp__CreateProcessW@40]
    test eax,eax
    jnz launched

ifdef ONEKB_CONSOLE_BOOTSTRAP
    ; The canonical path is below scratch; the cache path is above it.
    cmp ebp,edi
    ja failed
else
    ; A byte outside PROCESS_INFORMATION is a two-attempt state machine.
    sub byte ptr [edi+16],127
    jns failed
endif
    ; URLMon owns synchronization for its completed cache file. The recovery
    ; runtime atomically promotes its unique cache path to the canonical path.
ifdef ONEKB_CONSOLE_BOOTSTRAP
    lea ebp,[edi+SCRATCH_BYTES]
endif
    push esi
    push esi
    push MAX_TEMP_PATH_CHARS
    push ebp
    push offset aUrl
    push esi
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
    xchg eax,esi                   ; GUI returns zero
endif
    ret

failed:
    inc eax                        ; both failed CreateProcess calls return zero
    ret
BootstrapEntry ENDP

; The Windows process-start thunk terminates the process with EAX on return.
END BootstrapEntry
