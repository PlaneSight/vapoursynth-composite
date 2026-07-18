;*****************************************************************************
;* Encoder chroma modulate + luma add + clamp
;*****************************************************************************

%include "x86inc.asm"
%include "util.asm"

SECTION_RODATA 64

d_rnd15:  times 16 dd 16384
d_lmin:   times 16 dd 0x0100
d_lmax:   times 16 dd 0xFEFF

SECTION .text

; void encode_mod_row(uint16_t *dst, const int32_t *luma, const int32_t *uf,
;                     const int32_t *vf, int32_t ku, int32_t kv,
;                     const int32_t *s4, const int32_t *c4, int w)
;
; dst[x] = clamp(luma[x] + ((cu*s4[x&3] + cv*c4[x&3] + 16384) >> 15),
;                0x100, 0xFEFF), cu = (uf[x]*ku + 16384) >> 15, likewise cv.
; All int32: |uf|,|vf| <= 2^15 and the Q15 chroma levels keep every
; product under 2^30, so pmulld reproduces the C reference bitwise. The
; carrier pattern is 4-periodic from x = 0; the main loop steps a
; multiple of 4 lanes so each vector is phase 0 and loads the fixed
; s4/c4 splat, and a <= 15-sample scalar remainder finishes the ragged
; tail (dst is the real frame row, so no over-write is possible).
;
; The chroma scalings uf*ku and vf*kv use vpmaddwd, not vpmulld: uf/vf
; fit int16 ([-32768, 32767], src - 32768 tops at +32767 with unity-sum
; taps) and ku/kv are < 2^15, so the coefficient dword [0:k] reads as
; the int16 pair (hi = 0, lo = k) and vpmaddwd gives uf_lo*k + 0 = uf*k
; exactly. This is 1 uop vs vpmulld's 2 on Intel Haswell..Tiger Lake and
; identical on Zen (1 uop either way), so it never costs the primary
; target. The cu*s4 + cv*c4 combine stays vpmulld: s4/c4 reach +32768
; (sin_q15 stores sin = +1 as -32768, and the pattern negates it), which
; overflows int16.
%macro MOD_Q15 3 ; acc(dword), coef(dword vec), scratch -- (acc*coef+16384)>>15
    pmaddwd      %1, %2
    paddd        %1, m10
    psrad        %1, 15
%endmacro

%macro ENCODE_MOD_ROW 0
cglobal encode_mod_row, 9, 12, 13, dst, luma, uf, vf, ku, kv, s4, c4, w, x, tail, ph
    SPLATD       m8, kum          ; ku broadcast
    SPLATD       m9, kvm          ; kv broadcast
    mova         m10, [d_rnd15]
    mova         m11, [d_lmin]
    mova         m12, [d_lmax]
%if mmsize == 64
    vbroadcasti32x4 m6, [s4q]     ; fixed 4-periodic pattern (phase 0)
    vbroadcasti32x4 m7, [c4q]
%elif mmsize == 32
    vbroadcasti128  m6, [s4q]
    vbroadcasti128  m7, [c4q]
%else
    movu         m6, [s4q]
    movu         m7, [c4q]
%endif
    ; round w down to a whole number of vectors for the main loop
    mov          taild, wd
    and          taild, -(mmsize/4)
    xor          xq, xq
    cmp          xd, taild
    jge .rem
.loop:
    movu         m0, [ufq + xq*4]
    MOD_Q15      m0, m8, m1       ; cu
    movu         m1, [vfq + xq*4]
    MOD_Q15      m1, m9, m2       ; cv
    pmulld       m0, m6           ; cu * s4
    pmulld       m1, m7           ; cv * c4
    paddd        m0, m1
    paddd        m0, m10          ; + 16384
    psrad        m0, 15           ; chroma
    paddd        m0, [lumaq + xq*4]
    pmaxsd       m0, m11
    pminsd       m0, m12
%if mmsize == 64
    vpmovdw      [dstq + xq*2], m0
%elif mmsize == 32
    packusdw     m0, m0
    vpermq       m0, m0, 0x08
    movu         [dstq + xq*2], xm0
%else
    packusdw     m0, m0
    movq         [dstq + xq*2], m0
%endif
    add          xq, mmsize/4
    cmp          xd, taild
    jl .loop
.rem:
    cmp          xd, wd
    jge .done
    ; scalar tail: reload s4/c4 by x&3 (absolute phase)
    mov          phd, xd
    and          phd, 3
    movd         xm0, [ufq + xq*4]
    pmaddwd      xm0, xm8
    paddd        xm0, xm10
    psrad        xm0, 15          ; cu
    movd         xm1, [vfq + xq*4]
    pmaddwd      xm1, xm9
    paddd        xm1, xm10
    psrad        xm1, 15          ; cv
    movd         xm2, [s4q + phq*4]
    pmulld       xm0, xm2
    movd         xm2, [c4q + phq*4]
    pmulld       xm1, xm2
    paddd        xm0, xm1
    paddd        xm0, xm10
    psrad        xm0, 15
    movd         xm1, [lumaq + xq*4]
    paddd        xm0, xm1
    pmaxsd       xm0, xm11
    pminsd       xm0, xm12
    movd         phd, xm0
    mov          [dstq + xq*2], phw
    inc          xq
    cmp          xd, wd
    jl .rem
.done:
    RET
%endmacro

INIT_XMM sse4
ENCODE_MOD_ROW
INIT_YMM avx2
ENCODE_MOD_ROW
INIT_ZMM avx512
ENCODE_MOD_ROW
