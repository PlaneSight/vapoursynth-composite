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
