; Tiny 32-bit bootstrap for WOW64. No CRT and no launcher-overlay parsing.
; The customized Crinkler resolver enters with EAX=0 and
; EBX=PEB.ProcessParameters. The conventional linked image is only a resource
; staging template and is never executed.
.386
.model flat, stdcall
option casemap:none
ASSUME FS:NOTHING

EXTERN _imp__ExpandEnvironmentStringsW@12:DWORD
EXTERN _imp__CreateMutexW@12:DWORD, _imp__WaitForSingleObject@8:DWORD
ifdef ONEKB_CONSOLE_BOOTSTRAP
EXTERN _imp__ReleaseMutex@4:DWORD
endif
EXTERN _imp__CreateProcessW@40:DWORD
ifdef ONEKB_CONSOLE_BOOTSTRAP
EXTERN _imp__GetExitCodeProcess@8:DWORD
endif
EXTERN _imp__URLDownloadToFileW@20:DWORD
includelib kernel32.lib
includelib urlmon.lib

INFINITE equ -1
MAX_ENV_VALUE_CHARS equ 32767
; PROCESS_INFORMATION and STARTUPINFOW deliberately alias. The latter remains
; zero initialized: current Windows accepts cb=0, saving its initialization.
SCRATCH_BYTES equ 68
ROOT_CHARS equ MAX_ENV_VALUE_CHARS+3+SCRATCH_BYTES/2

; Separate data hunks let Crinkler choose their compressed ordering.
URLDATA SEGMENT BYTE PUBLIC 'DATA'
include runtime-url.inc
aUrl dw ONEKB_RUNTIME_URL_WIDE,0
URLDATA ENDS

LOCALDATA SEGMENT BYTE PUBLIC 'DATA'
nLocal dw '%','t','m','p','%','/','r',0
LOCALDATA ENDS

BOOTBSS SEGMENT DWORD PUBLIC 'BSS'
; /OVERRIDEALIGNMENTS recognizes this suffix. The small virtual-only offset
; saves one byte in each compressed core without increasing the file size.
rootPath_align9_131 dw ROOT_CHARS dup(?)
BOOTBSS ENDS

.code
BootstrapEntry PROC
    mov ebp,offset rootPath_align9_131
    xchg eax,esi                   ; preserve a one-byte source of zero
    push INFINITE                  ; effectively unbounded destination size
    push ebp
    push offset nLocal
    call dword ptr [_imp__ExpandEnvironmentStringsW@12]
    lea edi,[ebp+eax*2]            ; zeroed scratch just beyond the path NUL

    ; The recovery URL is also a valid, process-independent mutex name.
    push offset aUrl
    push esi
    push esi
    call dword ptr [_imp__CreateMutexW@12]
ifdef ONEKB_CONSOLE_BOOTSTRAP
    ; Keep the handle below ESP. Calls are stack-balanced, and ReleaseMutex's
    ; stdcall cleanup can consume this stack local directly as its argument.
    push eax
    push INFINITE
    push eax
else
    push INFINITE
    push eax                       ; GUI exits without explicitly releasing it
endif
    call dword ptr [_imp__WaitForSingleObject@8]

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

    ; A scratch byte outside PROCESS_INFORMATION is a two-attempt state machine:
    ; 00 -> 81 (download and retry) -> 02 (return failure).
    sub byte ptr [edi+16],127
    jns failed
    push esi
    push esi
    push ebp
    push offset aUrl
    push esi
    call dword ptr [_imp__URLDownloadToFileW@20]
    jmp tryStart

launched:
ifdef ONEKB_CONSOLE_BOOTSTRAP
    call dword ptr [_imp__ReleaseMutex@4]
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
ifdef ONEKB_CONSOLE_BOOTSTRAP
    pop edx                        ; discard the mutex handle stack local
endif
    inc eax                        ; both failed CreateProcess calls return zero
    ret
BootstrapEntry ENDP

; The Windows process-start thunk terminates the process with EAX on return.
END BootstrapEntry
