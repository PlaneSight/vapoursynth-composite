;*****************************************************************************
;* NTSC adaptive 3D comb candidate selection
;*****************************************************************************

%include "x86inc.asm"
%include "util.asm"

SECTION_RODATA 64

pd_abs:   times 8 dq 0x7FFFFFFFFFFFFFFF
pd_1000:  times 8 dq 1000.0
pd_three: times 8 dq 3.0
pd_half:  times 8 dq 0.5
pd_028:   times 8 dq 0.28
ps_half:  times 16 dd 0.5
perm_even: dd 0, 2, 4, 6, 0, 0, 0, 0

SECTION .text

; all arithmetic is double precision, matching the C reference
; operation for operation; /2.0 becomes *0.5 (exact for any double)
; and lane counts are mmsize/8

; load lanes of float into pd
%macro CVTPS2PD_L 2 ; dst, mem
%if mmsize == 64
    vcvtps2pd    %1, %2
%elif cpuflag(avx)
    vcvtps2pd    %1, %2
%else
    cvtps2pd     %1, %2
%endif
%endmacro

; load lanes of uint16 into pd (values < 2^16 are exact in int32)
%macro CVTU16PD 2 ; dst, mem
%if mmsize == 64
    vpmovzxwd    ym%1, %2
    vcvtdq2pd    m%1, ym%1
%elif mmsize == 32
    vpmovzxwd    xm%1, %2
    vcvtdq2pd    m%1, xm%1
%else
    movd         m%1, %2
    pmovzxwd     m%1, m%1
    cvtdq2pd     m%1, m%1
%endif
%endmacro

; broadcast a double from memory into all lanes
%macro SPLATQ 2 ; dst, mem
%if cpuflag(avx2)
    vbroadcastsd %1, %2
%else
    movddup      %1, %2
%endif
%endmacro

; keep the running best: strictly smaller penalty wins, so on ties the
; earlier candidate is kept, matching the reference scan order.
; m12 = penalty, m9 = sample; best penalty m6, best sample m7,
; best-is-same-frame mask m8 (k4 on avx512)
%macro SELECT 1 ; candidate index
%if mmsize == 64
    vcmppd       k3, m12, m6, 17  ; LT_OQ
    vmovapd      m6{k3}, m12
    vmovapd      m7{k3}, m9
%if %1 < 4
    korw         k4, k4, k3
%else
    kandnw       k4, k3, k4
%endif
%elif cpuflag(avx)
    vcmppd       m10, m12, m6, 17
    vblendvpd    m6, m6, m12, m10
    vblendvpd    m7, m7, m9, m10
%if %1 < 4
    orpd         m8, m10
%else
    vandnpd      m8, m10, m8
%endif
%else
    movapd       m10, m12
    cmpltpd      m10, m6
    movapd       m11, m10
    andnpd       m11, m6
    movapd       m13, m10
    andpd        m13, m12
    orpd         m11, m13
    movapd       m6, m11
    movapd       m11, m10
    andnpd       m11, m7
    movapd       m13, m10
    andpd        m13, m9
    orpd         m11, m13
    movapd       m7, m11
%if %1 < 4
    orpd         m8, m10
%else
    movapd       m11, m10
    andnpd       m11, m8
    movapd       m8, m11
%endif
%endif
%endmacro

; one |R_o - (line - c2)| term into ypen (m12) and one
; |refc_o + c2| * weight term into iqpen (m13); the o = -1 terms
; initialize the accumulators (adding them to 0.0 is exact).
; %3 is the float displacement and can be negative, so the uint16
; displacement needs nasm's SIGNED divide (`/' is unsigned)
%macro PEN_TERM 5 ; R_o, refc_o, byte disp into pd rows, weighted, first
    CVTPS2PD_L   m10, [t0q + t1q*4 + %3]
    CVTU16PD     11, [t2q + t1q*2 + %3//2]
    subpd        m11, m10
    subpd        m14, %1, m11
    andpd        m14, [pd_abs]
%if %5
    movapd       m12, m14
%else
    addpd        m12, m14
%endif
    addpd        m10, %2
    andpd        m10, [pd_abs]
%if %4
    mulpd        m10, [pd_half]
%endif
%if %5
    movapd       m13, m10
%else
    addpd        m13, m10
%endif
%endmacro

; evaluate candidate %1: penalty in m12, sample in m9, then fold into
; the running best
%macro CANDIDATE 1 ; i
    movsxd       t1q, dword [candq + %1*48 + 24]
    add          t1q, xq                    ; ch = x + off
    cmp          dword [candq + %1*48 + 28], 0
    jne          %%have_sample
    pxor         m9, m9
    jmp          %%pen_const                ; no sample implies no penalty
%%have_sample:
    mov          t0q, [candq + %1*48 + 0]
    CVTPS2PD_L   m9, [t0q + t1q*4]
    cmp          dword [candq + %1*48 + 32], 0
    je           %%pen_const
    mov          t0q, [candq + %1*48 + 8]   ; c2 row
    mov          t2q, [candq + %1*48 + 16]  ; composite row
    PEN_TERM     m0, m3, -4, 1, 1
    PEN_TERM     m1, m4,  0, 0, 0
    PEN_TERM     m2, m5,  4, 1, 0
    divpd        m12, [pd_three]
    divpd        m12, m15
    mulpd        m13, [pd_half]
    divpd        m13, m15
    mulpd        m13, [pd_028]
    addpd        m12, m13
    SPLATQ       m13, [candq + %1*48 + 40]  ; bonus
    addpd        m12, m13
    jmp          %%select
%%pen_const:
    movapd       m12, [pd_1000]
%%select:
%if %1 == 0
    ; first candidate always becomes the running best (best < 0)
    movapd       m6, m12
    movapd       m7, m9
%if mmsize == 64
    kxnorw       k4, k4, k4
%else
    pcmpeqd      m8, m8
%endif
%else
    SELECT       %1
%endif
%endmacro

; void split3d_row(int16_t *out, const float *c1c, const float *c2c,
;                  const uint16_t *ref, const comp_split3d_cand_t *cand,
;                  const double *irescale, int w)
%macro SPLIT3D_ROW 0
cglobal split3d_row, 6, 11, 16, out, c1c, c2c, ref, cand, pires, \
                                w, x, t0, t1, t2
    SPLATQ       m15, [piresq]
    mov          wd, dword r6m
    sub          wd, 3                      ; loop bound w - 3
    mov          xq, 3
    cmp          xd, wd
    jge          .end
.block:
    ; shared across candidates: refc_o = c2c[x+o] and
    ; R_o = ref[x+o] - refc_o
    CVTPS2PD_L   m3, [c2cq + xq*4 - 4]
    CVTPS2PD_L   m4, [c2cq + xq*4]
    CVTPS2PD_L   m5, [c2cq + xq*4 + 4]
    CVTU16PD     0, [refq + xq*2 - 2]
    CVTU16PD     1, [refq + xq*2]
    CVTU16PD     2, [refq + xq*2 + 2]
    subpd        m0, m3
    subpd        m1, m4
    subpd        m2, m5
    CANDIDATE    0
    CANDIDATE    1
    CANDIDATE    2
    CANDIDATE    3
    CANDIDATE    4
    CANDIDATE    5
    CANDIDATE    6
    CANDIDATE    7
    ; tc = best < 4 ? c2c[x] : (c1c[x] - best_sample) * 0.5
%if mmsize == 64
    vcvtpd2ps    ym9, m7
    vmovups      ym10, [c1cq + xq*4]
    vsubps       ym10, ym10, ym9
    vmulps       ym10, ym10, [ps_half]
    vmovups      ym10{k4}, [c2cq + xq*4]
    vcvtps2dq    ym10, ym10
    vpmovsdw     xm10, ym10
    movu         [outq + xq*2], xm10
%elif cpuflag(avx)
    vcvtpd2ps    xm9, m7
    vmovups      xm10, [c1cq + xq*4]
    vsubps       xm10, xm10, xm9
    vmulps       xm10, xm10, [ps_half]
    vmovdqa      m11, [perm_even]
    vpermd       m8, m11, m8
    vblendvps    xm10, xm10, [c2cq + xq*4], xm8
    vcvtps2dq    xm10, xm10
    vpackssdw    xm10, xm10, xm10
    movq         [outq + xq*2], xm10
%else
    cvtpd2ps     m9, m7
    movq         m10, [c1cq + xq*4]
    subps        m10, m9
    mulps        m10, [ps_half]
    pshufd       m8, m8, 0x88
    movq         m11, [c2cq + xq*4]
    pand         m11, m8
    pandn        m8, m10
    por          m8, m11
    cvtps2dq     m8, m8
    packssdw     m8, m8
    movd         [outq + xq*2], m8
%endif
    add          xq, mmsize/8
    cmp          xd, wd
    jl .block
.end:
    RET
%endmacro

INIT_XMM sse4
SPLIT3D_ROW
INIT_YMM avx2
SPLIT3D_ROW
INIT_ZMM avx512
SPLIT3D_ROW
