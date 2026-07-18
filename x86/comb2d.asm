;*****************************************************************************
;* NTSC 2D adaptive line comb (comb.cpp split2D)
;*****************************************************************************

%include "x86inc.asm"

SECTION_RODATA 64

ps_abs:   times 16 dd 0x7FFFFFFF
ps_one:   times 16 dd 1.0
ps_two:   times 16 dd 2.0
ps_three: times 16 dd 3.0
ps_010:   times 16 dd 0.10
ps_020:   times 16 dd 0.20
ps_025:   times 16 dd 0.25

SECTION .text

; Single precision throughout, matching the -ffp-contract=off C
; reference operation for operation: no FMA (the avx2 accumulate is
; hand-written vmulps + vaddps -- -ffp-contract governs only the C
; compiler, not this asm), no reassociation of the output tree, divps
; for every division (Zen 4 prices vdivps ymm at TP 0.5, so reciprocal
; approximation buys nothing and would not be bit-exact), and /4.0 as
; the exact *0.25.
;
; kp/kn come from absolute values; the final blend uses the signed
; samples cur[x], prev[x], next[x], so those are kept live. Both
; candidate-zeroing masks are formed from the pre-zeroing kp and kn
; (they are mutually exclusive over [0,1] -- kn>3kp and kp>3kn cannot
; both hold -- so applying them independently matches the C order).

; dst = mask ? a : dst, keeping dst where the mask is clear. mask and
; %4 (a scratch reg) are clobbered; a (%3) may be memory. avx leaves the
; mask intact; the sse path consumes it.
%macro BLENDIN 4 ; dst, mask, a, scratch
%if cpuflag(avx)
    vblendvps    %1, %1, %3, %2
%else
    movaps       %4, %3
    andps        %4, %2                        ; a & mask
    andnps       %2, %1                        ; ~mask & dst
    orps         %4, %2
    movaps       %1, %4
%endif
%endmacro

; k = clamp(1 - kraw/krange, 0, 1),
;   kraw = ||cur|-|v|| + ||curm|-|vm|| - (|cur|+|vm|)*0.10
; inputs are the absolute values; result in %1; %6/%7 scratch.
%macro COEF 8 ; dst, acur, acurm, av, avm, sA, sB, krange
    subps        %1, %2, %4
    andps        %1, [ps_abs]                  ; ||cur|-|v||
    subps        %6, %3, %5
    andps        %6, [ps_abs]                  ; ||curm|-|vm||
    addps        %1, %6
    addps        %7, %2, %5                    ; |cur| + |vm|
    mulps        %7, [ps_010]
    subps        %1, %7                        ; kraw
    divps        %1, %8                        ; kraw / krange
    movaps       %6, [ps_one]
    subps        %6, %1                        ; 1 - kraw/krange
    xorps        %1, %1
    maxps        %1, %6                        ; max(., 0)  (C tests < 0 first)
    minps        %1, [ps_one]                  ; min(., 1)
%endmacro

; void ntsc_comb2d_row(float *out, const float *cur, const float *prev,
;                      const float *next, const float *krange, int w)
; writes out[x] for x in [1, w). krange is passed by pointer to keep the
; asm ABI GPR-only: a by-value float lands in xmm0, but x86inc maps its
; virtual m0 to zmm16 on the avx512 tier (32-register file), so xm0 is
; not the incoming xmm0 there -- the pointer sidesteps it (see split3d's
; irescale). The final vector is processed at x = w - lanes (recomputing
; a few already-written lanes with identical bits), no access past [w].
%macro NTSC_COMB2D_ROW 0
cglobal ntsc_comb2d_row, 6, 8, 16, out, cur, prev, next, kr, w, x, xend
%if cpuflag(avx)
    vbroadcastss m15, [krq]
%else
    movss        m15, [krq]
    shufps       m15, m15, 0
%endif
    lea          xendd, [wd - mmsize/4]        ; last full-vector start
    mov          xd, 1
.block:
    ; signed samples at x, kept for the final blend
    movups       m0, [curq  + xq*4]            ; cur[x]
    movups       m1, [prevq + xq*4]            ; prev[x]
    movups       m2, [nextq + xq*4]            ; next[x]

    ; six absolute values feeding kp/kn
    andps        m8, m0, [ps_abs]              ; |cur[x]|   (kept past COEF)
    movups       m9, [curq + xq*4 - 4]
    andps        m9, [ps_abs]                  ; |cur[x-1]| (kept past COEF)
    andps        m10, m1, [ps_abs]             ; |prev[x]|
    movups       m11, [prevq + xq*4 - 4]
    andps        m11, [ps_abs]                 ; |prev[x-1]|
    COEF         m6, m8, m9, m10, m11, m12, m13, m15   ; kp -> m6

    movups       m10, [nextq + xq*4 - 4]
    andps        m10, [ps_abs]                 ; |next[x-1]|
    andps        m11, m2, [ps_abs]             ; |next[x]|
    COEF         m7, m8, m9, m11, m10, m12, m13, m15   ; kn -> m7
    ; m8..m13 now free; m6=kp, m7=kn, m0/m1/m2 signed samples

    ; the branch selection. On avx512 the masks live in k registers and
    ; the blends are masked moves; the xmm/ymm tiers keep them in vector
    ; registers and blend with and/andn/or (see BLENDIN). Either way both
    ; candidate-zeroing masks come from the pre-zeroing kp/kn.
%if mmsize == 64
    ; x86inc marks cmpps commutative and swaps src1/src2 when src2 is a
    ; high (m8..15) register -- which silently inverts an ordered GT/LE
    ; predicate. Keep every vcmpps second operand in a low register
    ; (m0..7) so no swap fires: m3 = 0.0, and 3*kp / 3*kn in m4 / m5.
    vxorps       m3, m3, m3                    ; 0.0
    ; term for the fallback compare: ||prev|-|next|| - |(next+prev)*0.2|
    andps        m8, m1, [ps_abs]
    andps        m9, m2, [ps_abs]
    subps        m8, m9
    andps        m8, [ps_abs]
    addps        m9, m2, m1
    mulps        m9, [ps_020]
    andps        m9, [ps_abs]
    subps        m8, m9
    vcmpps       k1, m8, m3, 18                ; k1 = fb_cond (LE_OQ)
    vcmpps       k2, m6, m3, 14                ; kp > 0 (GT_OQ)
    vcmpps       k3, m7, m3, 14                ; kn > 0
    korw         k2, k2, k3                    ; k2 = active
    mulps        m4, m6, [ps_three]            ; 3*kp (low reg)
    vcmpps       k4, m7, m4, 14                ; kn > 3*kp -> zero kp
    kandw        k4, k4, k2
    mulps        m5, m7, [ps_three]            ; 3*kn (low reg)
    vcmpps       k5, m6, m5, 14                ; kp > 3*kn -> zero kn
    kandw        k5, k5, k2
    vmovaps      m6{k4}, m3                    ; kp' (zeroed where zkp)
    vmovaps      m7{k5}, m3                    ; kn'
    kandnw       k6, k2, k1                    ; fbsel = ~active & fb
    vmovaps      m6{k6}, [ps_one]              ; kp = fbsel ? 1 : kp'
    vmovaps      m7{k6}, [ps_one]              ; kn = fbsel ? 1 : kn'
    addps        m9, m6, m7                    ; kp+kn
    movaps       m8, [ps_two]
    divps        m8, m9                        ; 2/(kp+kn)
    maxps        m8, [ps_one]
    movaps       m5, [ps_one]
    vmovaps      m5{k2}, m8                    ; sc = active ? max : 1
%else
    ; fallback mask m3: ||prev[x]|-|next[x]|| - |(next[x]+prev[x])*0.2| <= 0
    andps        m8, m1, [ps_abs]              ; |prev[x]|
    andps        m9, m2, [ps_abs]              ; |next[x]|
    subps        m8, m9
    andps        m8, [ps_abs]                  ; ||prev|-|next||
    addps        m9, m2, m1                    ; next[x]+prev[x] (signed)
    mulps        m9, [ps_020]
    andps        m9, [ps_abs]                  ; |(next+prev)*0.2|
    subps        m8, m9
    xorps        m9, m9
    cmpleps      m3, m8, m9                    ; fb_cond

    ; active mask m4 = (kp>0) | (kn>0)
    xorps        m9, m9
    cmpltps      m4, m9, m6                    ; kp > 0
    cmpltps      m5, m9, m7                    ; kn > 0
    orps         m4, m5

    ; the two exclusive zeroing masks from pre-zeroing kp, kn, gated by
    ; active; then kp' = ~zkp & kp, kn' = ~zkn & kn
    mulps        m8, m6, [ps_three]
    cmpltps      m8, m8, m7                    ; kn > 3*kp -> zero kp
    andps        m8, m4
    mulps        m9, m7, [ps_three]
    cmpltps      m9, m9, m6                    ; kp > 3*kn -> zero kn
    andps        m9, m4
    andnps       m8, m6                        ; kp' = ~zkp & kp
    andnps       m9, m7                        ; kn' = ~zkn & kn
    SWAP         6, 8                           ; m6 = kp'
    SWAP         7, 9                           ; m7 = kn'

    ; fallback: in lanes where !active & fb_cond, set kp = kn = 1
    ; (sc stays 1 there). fbsel = ~active & fb = andnot(active, fb).
    movaps       m9, m4
    andnps       m9, m3                        ; m9 = fbsel
    BLENDIN      m6, m9, [ps_one], m8          ; kp = fbsel ? 1 : kp'
    movaps       m9, m4
    andnps       m9, m3                        ; fbsel again (BLENDIN ate it)
    BLENDIN      m7, m9, [ps_one], m8          ; kn = fbsel ? 1 : kn'

    ; sc = active ? max(2/(kp+kn), 1) : 1, resolved before any multiply
    ; so the !active lanes (kp+kn == 0 -> inf) never reach 0*inf.
    addps        m9, m6, m7                    ; kp+kn
    movaps       m8, [ps_two]
    divps        m8, m9                        ; 2/(kp+kn)
    maxps        m8, [ps_one]                  ; max(., 1)
    movaps       m5, [ps_one]
    BLENDIN      m5, m4, m8, m9                ; sc = active ? max : 1
%endif

    ; out[x] = ((cur-prev)*kp*sc + (cur-next)*kn*sc) * 0.25, exact tree
    subps        m8, m0, m1                    ; cur[x]-prev[x]
    mulps        m8, m6                        ; *kp
    mulps        m8, m5                        ; *sc
    subps        m9, m0, m2                    ; cur[x]-next[x]
    mulps        m9, m7                        ; *kn
    mulps        m9, m5                        ; *sc
    addps        m8, m9
    mulps        m8, [ps_025]                  ; /4.0
    movups       [outq + xq*4], m8

    cmp          xd, xendd
    je           .done                         ; just processed the last block
    add          xd, mmsize/4
    cmp          xd, xendd
    jl           .block                        ; another full block fits
    mov          xd, xendd                     ; final overlapping vector
    jmp          .block                        ; (idempotent; ends at [w-1])
.done:
    RET
%endmacro

INIT_XMM sse4
NTSC_COMB2D_ROW
INIT_YMM avx2
NTSC_COMB2D_ROW
INIT_ZMM avx512
NTSC_COMB2D_ROW
