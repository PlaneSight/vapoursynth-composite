;*****************************************************************************
;* Shared macros for the composite kernels
;*****************************************************************************

; splat a dword into all lanes. The source may be memory or a general
; purpose register: vpbroadcastd takes a memory or xmm source under VEX,
; but broadcasting straight from a GPR exists only in EVEX, which would
; be an illegal instruction on a plain AVX2 part. So move a GPR through
; an xmm first and broadcast from that.
%macro SPLATD 2 ; dst, src
%if cpuflag(avx2)
  %ifid %2
    movd         xmm %+ %1, %2
    vpbroadcastd %1, xmm %+ %1
  %else
    vpbroadcastd %1, %2
  %endif
%else
    movd         %1, %2
    pshufd       %1, %1, 0x00
%endif
%endmacro

; select the odd dword lanes of %3 over the even results in %2
; (%3's odd lanes were filled by a qword shift; %1 must not alias %3).
; the avx512 form needs k2 = 0xAAAA, set once in the prologue
%macro BLEND_ODD 3 ; out, even, odd
%if mmsize == 64
    vpblendmd    %1{k2}, %2, %3
%elif cpuflag(avx2)
    vpblendd     %1, %2, %3, 0xAA
%else
    pblendw      %1, %2, %3, 0xCC
%endif
%endmacro

; packed i32 of -(a*ca (sign_op) b*cb + 8192) >> 14, the demod chroma
; rotation. The caller supplies two rodata constants in its own
; SECTION_RODATA (x86util convention: macros here, constants per file):
;   q_nbias14: times mmsize/8 dq (1 << 40) - 8192
;   d_sub26:   times mmsize/4 dd 1 << 26
; S is subtracted from (1 << 40) - 8192 so a logical shift floors the
; negated value exactly; d_sub26 removes the folded (1 << 40) >> 14
; offset. Clobbers m12..m15; %1 must not alias them. Uses BLEND_ODD, so
; the avx512 form needs k2 = 0xAAAA set in the prologue.
%macro ROTATE 6 ; out, a, ca, b, cb, sign_op (paddq/psubq)
    pshufd       m14, %2, 0xF5
    pmuldq       m14, %3
    pshufd       m13, %4, 0xF5
    pmuldq       m13, %5
    %6           m14, m13
    pmuldq       m12, %2, %3
    pmuldq       m13, %4, %5
    %6           m12, m13
    mova         m13, [q_nbias14]
    psubq        m15, m13, m12
    psubq        m13, m14
    psrlq        m15, 14
    psrlq        m13, 14
    psllq        m13, 32
    BLEND_ODD    %1, m15, m13
    psubd        %1, [d_sub26]
%endmacro
