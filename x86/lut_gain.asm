;*****************************************************************************
;* Trained-LUT per-bin gain row
;*****************************************************************************

%include "x86inc.asm"
%include "util.asm"

SECTION_RODATA 64

; per-lane dword index seeds, in LUT-row strides (16 floats per row)
d_lanebase: dd 0*16, 1*16, 2*16, 3*16, 4*16, 5*16, 6*16, 7*16
            dd 8*16, 9*16, 10*16, 11*16, 12*16, 13*16, 14*16, 15*16
f_ones:     times 16 dd 1.0
f_fifteen:  times 16 dd 15.0
d_fourteen: times 16 dd 14

SECTION .text

; void lut_gain(float *g, float *r, const float *m_in, const float *m_ref,
;               const float (*lut)[16], int n)
;
; r = lo/hi over (m_in, m_ref), 1 when hi == 0; g the interpolation of
; the bin's 16-knot LUT row at r*15. n is padded to a 16-sample bound.
; lo <= hi, so the only non-finite quotient is 0/0 = NaN, and MINPS
; returning its second operand on NaN turns exactly those lanes into
; the reference's 1.0 (finite lanes pass, as r never exceeds 1). The
; interpolation must stay mul-then-add: the reference is compiled for
; baseline x86-64, whose codegen cannot contract it into an FMA.
%macro LUT_GAIN 0
cglobal lut_gain, 6, 9, 8, mmsize, g, r, mi, mr, lut, n, x, i0, i1
    movu         m5, [d_lanebase]   ; running dword index of lane 0's knot 0
    mov          i0d, mmsize*4
    movd         xm6, i0d
%if cpuflag(avx2)
    vpbroadcastd m6, xm6            ; index step per iteration
%else
    pshufd       m6, m6, 0x00
%endif
    xor          xq, xq
.bin:
    movu         m0, [miq + xq*4]
    movu         m1, [mrq + xq*4]
    mova         m2, m0
    minps        m2, m1             ; lo
    maxps        m1, m0             ; hi
    divps        m2, m1             ; NaN on the hi == 0 lanes
    minps        m2, [f_ones]       ; r
    movu         [rq + xq*4], m2
    mulps        m2, [f_fifteen]    ; pos
    cvttps2dq    m4, m2
    pminsd       m4, [d_fourteen]   ; knot k
    cvtdq2ps     m7, m4
    subps        m2, m7             ; frac = pos - k
    paddd        m4, m5             ; dword index of knot k

    ; Fetch knots k and k+1 of each lane's row: the pair is contiguous,
    ; so one 64-bit load brings both, and an even/odd split then yields
    ; the two knot vectors — 8 load uops per 4 lanes against 14 for
    ; per-knot insertps, and none of insertps's port-5 shuffle traffic
    ; (TGL: movsd load 1 uop p23, insertps-from-memory 2 uops p23+p5).
    ; vgatherdps loses to either form outright under the gather
    ; microcode mitigations on the uarchs this was measured on.
    movu         [rsp], m4
%assign %%c 0
%rep mmsize/16
    mov          i0d, [rsp + %%c*16 + 0]
    mov          i1d, [rsp + %%c*16 + 4]
    movsd        xm1, [lutq + i0q*4]
    movhps       xm1, [lutq + i1q*4]
    mov          i0d, [rsp + %%c*16 + 8]
    mov          i1d, [rsp + %%c*16 + 12]
    movsd        xm3, [lutq + i0q*4]
    movhps       xm3, [lutq + i1q*4]
%if mmsize == 16
    shufps       m0, m1, m3, 0x88
    shufps       m7, m1, m3, 0xDD
%else
    shufps       xm4, xm1, xm3, 0x88
    vinsertf128  m0, m0, xm4, %%c
    shufps       xm4, xm1, xm3, 0xDD
    vinsertf128  m7, m7, xm4, %%c
%endif
%assign %%c %%c+1
%endrep

    subps        m7, m0             ; row[k+1] - row[k]
    mulps        m7, m2             ; * frac (no FMA: must match the C rounding)
    addps        m0, m7
    movu         [gq + xq*4], m0

    paddd        m5, m6
    add          xq, mmsize/4
    cmp          xd, nd
    jl .bin
    RET
%endmacro

; no zmm tier: the kernel is load/shuffle-bound, so 512-bit lanes add
; only insert-chain depth (measured slower than the ymm tier on TGL,
; and Zen 4's double-pumped 512-bit units cannot come out ahead)
INIT_XMM sse4
LUT_GAIN
INIT_YMM avx2
LUT_GAIN
