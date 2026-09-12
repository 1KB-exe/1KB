; Test-only entry wrapper. A second, suspended thread must not keep the
; bootstrap alive after handoff/failure. No timing or networking is needed
; to reproduce the old RET-only shutdown bug. Never linked into production.
.386
.model flat, stdcall
option casemap:none

EXTERN BootstrapEntry@0:PROC
EXTERN _imp__CreateThread@24:DWORD
EXTERN _imp__ExitProcess@4:DWORD

.code
ThreadedEntry PROC
    pushad                         ; preserve Crinkler's entry register contract
    push 0                         ; thread ID
    push 4                         ; CREATE_SUSPENDED: deliberately never resumed
    push 0                         ; parameter
    push offset ThreadedEntry      ; valid, but never executed by this thread
    push 0                         ; default stack
    push 0                         ; security attributes
    call dword ptr [_imp__CreateThread@24]
    test eax,eax
    jz setupFailed
    popad
    jmp BootstrapEntry@0
setupFailed:
    push 99                        ; fixture failure, not a bootstrap exit code
    call dword ptr [_imp__ExitProcess@4]
ThreadedEntry ENDP
END
