EXTERN real_CloseHandle: QWORD
EXTERN real_RtlAddVectoredExceptionHandler: QWORD
EXTERN CheckInjectExcept: PROC
EXTERN real_CreateFileA: QWORD
EXTERN real_LoadLibraryA: QWORD

.data
	rbpsave dq 0
	exceptaddr dq 0
.code

zzCloseHandle PROC
  test spl, 0fh
  jnz noalign

  push rbp
  mov rbp, rsp
  sub rsp, 20h
  and spl, 0f0h
  call qword ptr [real_CloseHandle]
  mov rsp, rbp
  pop rbp
  ret
noalign:
  jmp [real_CloseHandle]
zzCloseHandle ENDP

zzException PROC
  push rax
  mov rax, qword ptr[rcx + 8h]
  push rbpsave
  pop qword ptr[rax + 0a0h]
  pop rax
  jmp exceptaddr
zzException ENDP

zzRtlAddVectoredExceptionHandler PROC
  sub rsp, 40h
  mov qword ptr [rsp + 20h], rcx
  mov qword ptr [rsp + 28h], rdx
  mov qword ptr [rsp + 30h], rbp
  
  ;save stack value
  mov rbp, rsp

  ;align stack
  and spl, 0f0h

  mov rcx, qword ptr[rbp + 40h] ;get caller address

  CALL CheckInjectExcept

  mov rsp, rbp

  mov rcx, qword ptr [rsp + 20h]
  mov rdx, qword ptr [rsp + 28h]
  mov rbp, qword ptr [rsp + 30h]

  add rsp, 40h

  test rax, rax
  jz rtlpass

  mov rbpsave, rbp ; save rbp
  mov exceptaddr, rdx ; save themida handler addr
  mov rdx, zzException ; inject own handler

rtlpass:
  jmp qword ptr [real_RtlAddVectoredExceptionHandler]
zzRtlAddVectoredExceptionHandler ENDP


zzCreateFileA PROC
  test spl, 0fh
  jnz noalign

  push rbp
  mov rbp, rsp

  sub rsp, 10h ;add gap
  and spl, 0f0h ;align

  push 0 ;padding
  push qword ptr [rbp + 40h] ;copy arg 7 (8 + 8 + 20 + 10)
  push qword ptr [rbp + 38h] ;copy arg 6 (8 + 8 + 20 + 8)
  push qword ptr [rbp + 30h] ;copy arg 5 (8 + 8 + 20 + 0)

  sub rsp, 20h

  call qword ptr [real_CreateFileA]
  mov rsp, rbp
  pop rbp
  ret

noalign:
  jmp [real_CreateFileA]
zzCreateFileA ENDP

zzLoadLibraryA PROC
  test spl, 0fh
  jnz noalign

  push rbp
  mov rbp, rsp
  sub rsp, 28h
  and spl, 0f0h
  call qword ptr [real_LoadLibraryA]
  mov rsp, rbp
  pop rbp
  ret

noalign:
  jmp [real_LoadLibraryA]
zzLoadLibraryA ENDP


END