EXTERN AppendQuestIcon: PROC
EXTERN g_questIconResume: QWORD
EXTERN g_questIconLines: QWORD
EXTERN QuestTokenIndex: PROC
EXTERN g_questTokenTable: QWORD
EXTERN g_questTokenEnd: QWORD
EXTERN g_questTokenLoop: QWORD

.code

; Entered by a jump from the NPC name builder right after its optional icon, with ebx holding the characters written to the
; current line so far and the lines of 256 characters starting g_questIconLines bytes above rsp. The register holding the
; name owner and the one holding the line index differ between client versions, hence one stub per layout.
; Both call AppendQuestIcon(owner, line, written), take its result as the new count and resume the name builder.

CALL_APPEND MACRO
  and rsp, -16
  sub rsp, 128
  movdqu [rsp + 32], xmm0
  movdqu [rsp + 48], xmm1
  movdqu [rsp + 64], xmm2
  movdqu [rsp + 80], xmm3
  movdqu [rsp + 96], xmm4
  movdqu [rsp + 112], xmm5
  call AppendQuestIcon
  mov ebx, eax
  movdqu xmm0, [rsp + 32]
  movdqu xmm1, [rsp + 48]
  movdqu xmm2, [rsp + 64]
  movdqu xmm3, [rsp + 80]
  movdqu xmm4, [rsp + 96]
  movdqu xmm5, [rsp + 112]
ENDM

POP_VOLATILE MACRO
  mov rsp, rbp
  pop rbp
  pop r11
  pop r10
  pop r9
  pop r8
  pop rdx
  pop rcx
  pop rax
ENDM

; owner in r15, line index in r12
QuestIconStubR15R12 PROC
  push rax
  push rcx
  push rdx
  push r8
  push r9
  push r10
  push r11
  push rbp
  mov rbp, rsp

  lea rdx, [rbp + 64]
  add rdx, g_questIconLines
  mov rax, r12
  shl rax, 9
  add rdx, rax
  mov rcx, r15
  mov r8d, ebx
  CALL_APPEND

  POP_VOLATILE
  jmp qword ptr [g_questIconResume]
QuestIconStubR15R12 ENDP

; owner in r14, line index in rbp
QuestIconStubR14Rbp PROC
  push rax
  push rcx
  push rdx
  push r8
  push r9
  push r10
  push r11
  mov rax, rbp
  push rbp
  mov rbp, rsp

  lea rdx, [rbp + 64]
  add rdx, g_questIconLines
  shl rax, 9
  add rdx, rax
  mov rcx, r14
  mov r8d, ebx
  CALL_APPEND

  POP_VOLATILE
  jmp qword ptr [g_questIconResume]
QuestIconStubR14Rbp ENDP

; Entered by a jump in place of lea r12, <tokens>; lea r14, <end> before the token loop of Quest::Set, with the attribute name in r13
; and ebx at 0. Starts the loop at the token QuestTokenIndex gives, which compares equal at once, with ebx counted up to it.
QuestTokenStub PROC
  push rax
  push rcx
  push rdx
  push r8
  push r9
  push r10
  push r11
  push rbp
  mov rbp, rsp
  and rsp, -16
  sub rsp, 32
  mov rcx, r13
  call QuestTokenIndex
  mov rsp, rbp
  mov r12, g_questTokenTable
  lea r12, [r12 + rax * 8]
  add ebx, eax
  mov r14, g_questTokenEnd
  pop rbp
  pop r11
  pop r10
  pop r9
  pop r8
  pop rdx
  pop rcx
  pop rax
  jmp qword ptr [g_questTokenLoop]
QuestTokenStub ENDP

END
