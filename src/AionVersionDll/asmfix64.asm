EXTERN real_CloseHandle: QWORD
EXTERN real_RtlAddVectoredExceptionHandler: QWORD
EXTERN CheckInjectExcept: PROC
EXTERN real_CreateFileA: QWORD
EXTERN real_LoadLibraryA: QWORD

.data
	ExcpSize = 4 * 8

	;used addr rbp, func
	exceptors dq 3 dup(0), OFFSET zzExp0,
	             3 dup(0), OFFSET zzExp1,
				 3 dup(0), OFFSET zzExp2,
				 3 dup(0), OFFSET zzExp3,
				 3 dup(0), OFFSET zzExp4,
				 3 dup(0), OFFSET zzExp5,
				 3 dup(0), OFFSET zzExp6,
				 3 dup(0), OFFSET zzExp7,
				 3 dup(0), OFFSET zzExp8,
				 3 dup(0), OFFSET zzExp9,
				 4 dup(0) ;end of array
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

  ;find free slot
  lea rax, exceptors
findloop:
  test byte ptr [rax], 1
  jz found

  add rax, ExcpSize
  cmp qword ptr [rax + 18h], 0
  jne findloop
  jmp rtlpass ;not found free - skip

found:
  mov byte ptr [rax], 1 ; use this record
  mov qword ptr [rax + 10h], rbp ; save rbp
  mov qword ptr [rax + 8], rdx

  mov rdx, [rax + 18h] ; inject own handler

rtlpass:
  jmp qword ptr [real_RtlAddVectoredExceptionHandler]
zzRtlAddVectoredExceptionHandler ENDP


zzExp0 PROC
idx = 0
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp0 ENDP

zzExp1 PROC
idx = 1
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp1 ENDP

zzExp2 PROC
idx = 2
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp2 ENDP

zzExp3 PROC
idx = 3
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp3 ENDP

zzExp4 PROC
idx = 4
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp4 ENDP

zzExp5 PROC
idx = 5
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp5 ENDP

zzExp6 PROC
idx = 6
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp6 ENDP

zzExp7 PROC
idx = 7
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp7 ENDP

zzExp8 PROC
idx = 8
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp8 ENDP

zzExp9 PROC
idx = 9
	push exceptors + idx * ExcpSize + 8
	push exceptors + idx * ExcpSize + 16

	jmp zzExpEnd
zzExp9 ENDP

zzExpEnd PROC
	push rax

	mov rax, qword ptr[rcx + 8h]
	push qword ptr[rsp + 8] ;push rbp
	pop qword ptr[rax + 0a0h] ;restore rbp

	pop rax 
	add rsp, 8
	ret
zzExpEnd ENDP

END