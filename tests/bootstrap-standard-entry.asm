; Measurement/test adapter for an unpacked, conventional PE. Not production.
; Supply the private assembly contract using a documented command-line API,
; without relying on the PEB or Crinkler's entry registers.
.386
.model flat, stdcall
option casemap:none
EXTERN _imp__GetCommandLineW@0:DWORD
ifdef ONEKB_THREADED_TEST
EXTERN ThreadedEntry@0:PROC
else
EXTERN BootstrapEntry@0:PROC
endif
.code
StandardEntry PROC
    sub esp,48h
    call dword ptr [_imp__GetCommandLineW@0]
    mov [esp+44h],eax
    mov ebx,esp
    xor eax,eax
ifdef ONEKB_THREADED_TEST
    jmp ThreadedEntry@0
else
    jmp BootstrapEntry@0
endif
StandardEntry ENDP
END
