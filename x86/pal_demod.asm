;*****************************************************************************
;* PAL demodulation filter kernel
;*****************************************************************************

%include "x86inc.asm"
%include "util.asm"

SECTION_RODATA 64

; arithmetic-shift emulation biases: X + bias is non-negative for the
; accumulator magnitudes the C reference admits, so a logical shift of
; the biased value equals the arithmetic shift plus a constant that the
; final pack subtracts (or folds into the negation)
q_bias16:  times 8 dq (1 << 38) + 32768
d_sub22:   times 16 dd 1 << 22
q_nbias14: times 8 dq (1 << 40) - 8192
d_sub26:   times 16 dd 1 << 26

SECTION .text

%define FS 7

; one filter term: acc_e/acc_o += ((base[l] + base[r]) * coef) as
; even/odd 64-bit lane products of the dword sums
%macro TERM 6 ; acc_e, acc_o, base, coef, disp_l, disp_r
%if mmsize == 64
    ; masked so the ragged final iteration (w % 16 == 8) does not
    ; read past the last plane row
    vmovdqu32    m12{k1}{z}, [%3 + xq*4 + %5]
    vpaddd       m12{k1}{z}, m12, [%3 + xq*4 + %6]
%elif cpuflag(avx)
    movu         m12, [%3 + xq*4 + %5]
    paddd        m12, [%3 + xq*4 + %6]
%else
    movu         m12, [%3 + xq*4 + %5]
    movu         m13, [%3 + xq*4 + %6]
    paddd        m12, m13
%endif
    pshufd       m13, m12, 0xF5
    pmuldq       m12, %4
    paddq        %1, m12
    pmuldq       m13, %4
    paddq        %2, m13
%endmacro

; all eight coefficient terms of one tap b for outputs x..x+mmsize/4-1:
; A += m0*cf0 + m1*cf1   B += n2*cf2 + n3*cf3
; C += n0*cf0 + n1*cf1   D += m2*cf2 + m3*cf3
%macro TAP 1 ; b
%if cpuflag(avx2)
    vpbroadcastd m8,  [cfq + %1*16 + 0]
    vpbroadcastd m9,  [cfq + %1*16 + 4]
    vpbroadcastd m10, [cfq + %1*16 + 8]
    vpbroadcastd m11, [cfq + %1*16 + 12]
%else
    movu         m8,  [cfq + %1*16]
    pshufd       m9,  m8, 0x55
    pshufd       m10, m8, 0xAA
    pshufd       m11, m8, 0xFF
    pshufd       m8,  m8, 0x00
%endif
    TERM m0, m1, mq,   m8,  (FS-%1)*4, (FS+%1)*4
    TERM m4, m5, nq,   m8,  (FS-%1)*4, (FS+%1)*4
    TERM m0, m1, mr1q, m9,  (FS-%1)*4, (FS+%1)*4
    TERM m4, m5, nr1q, m9,  (FS-%1)*4, (FS+%1)*4
    TERM m6, m7, mr2q, m10, (FS-%1)*4, (FS+%1)*4
    TERM m2, m3, nr2q, m10, (FS-%1)*4, (FS+%1)*4
    TERM m6, m7, mr3q, m11, (FS-%1)*4, (FS+%1)*4
    TERM m2, m3, nr3q, m11, (FS-%1)*4, (FS+%1)*4
%endmacro

; (e, o) 64-bit lanes -> packed int32 of (val + 32768) >> 16
%macro ROUND16 3 ; out, e, o (clobbers e, o)
    paddq        %2, [q_bias16]
    paddq        %3, [q_bias16]
    psrlq        %2, 16
    psrlq        %3, 16
    psllq        %3, 32
    BLEND_ODD    %1, %2, %3
    psubd        %1, [d_sub22]
%endmacro

; packed i32 of (-(a*ca (sign_op) b*cb) - 8192) >> 14: the sum S is
; subtracted from (1 << 40) - 8192 so the shift floors the NEGATED
; value, matching the C reference's -(S + 8192) >> 14 exactly
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

; void pal_demod_row(int32_t *u, int32_t *v, const int32_t *m,
;                    const int32_t *n, ptrdiff_t stride,
;                    const int32_t *cf, int w, int32_t bp, int32_t bq,
;                    int32_t vswitch)
%macro PAL_DEMOD_ROW 0
cglobal pal_demod_row, 6, 14, 16, u, v, m, n, stride, cf, \
                                  mr1, mr2, mr3, nr1, nr2, nr3, x, w
    lea          xq, [strideq*4]
    lea          mr1q, [mq + xq]
    lea          mr2q, [mq + xq*2]
    lea          mr3q, [mr1q + xq*2]
    lea          nr1q, [nq + xq]
    lea          nr2q, [nq + xq*2]
    lea          nr3q, [nr1q + xq*2]
    mov          wd, dword r6m
%if mmsize == 64
    mov          strided, 0xAAAA  ; k2 selects the odd dword lanes
    kmovw        k2, strided
    kxnorw       k1, k1, k1
%endif
    xor          xq, xq
.loop:
%if mmsize == 64
    ; w is only a multiple of 8, so the final iteration may cover
    ; just 8 outputs; narrow k1 for its loads and stores
    mov          strided, wd
    sub          strided, xd
    cmp          strided, 16
    jge          .full
    mov          strided, 0xFF
    kmovw        k1, strided
.full:
%endif
    pxor         m0, m0    ; A even
    pxor         m1, m1    ; A odd
    pxor         m2, m2    ; B even
    pxor         m3, m3    ; B odd
    pxor         m4, m4    ; C even
    pxor         m5, m5    ; C odd
    pxor         m6, m6    ; D even
    pxor         m7, m7    ; D odd

    ; %%-local so the tap counter cannot leak into the next cglobal's
    ; DEFINE_ARGS cleanup (a bare `b' would expand there and undef m8)
%assign %%b 0
%rep FS + 1
    TAP %%b
%assign %%b %%b+1
%endrep

    ; pu = A + B, pv = A - B, qu = C - D, qv = C + D
    paddq        m12, m0, m2
    paddq        m13, m1, m3
    psubq        m0, m2
    psubq        m1, m3
    psubq        m14, m4, m6
    psubq        m15, m5, m7
    paddq        m4, m6
    paddq        m5, m7

    ROUND16      m2, m12, m13     ; pu0
    ROUND16      m3, m14, m15     ; qu0
    ROUND16      m6, m0, m1       ; pv0
    ROUND16      m7, m4, m5       ; qv0

    SPLATD       m8, dword r7m    ; bp
    SPLATD       m9, dword r8m    ; bq

    ; u = -(pu0*bp + qu0*bq + 8192) >> 14
    ROTATE       m10, m2, m8, m3, m9, paddq
%if mmsize == 64
    vmovdqu32    [uq + xq*4]{k1}, m10
%else
    movu         [uq + xq*4], m10
%endif

    ; v = vswitch * (-(qv0*bp - pv0*bq + 8192) >> 14)
    ROTATE       m10, m7, m8, m6, m9, psubq
    SPLATD       m11, dword r9m   ; vswitch
%if mmsize == 64
    pmulld       m10, m11         ; no EVEX psignd; vswitch is +/-1
    vmovdqu32    [vq + xq*4]{k1}, m10
%else
    psignd       m10, m11
    movu         [vq + xq*4], m10
%endif

    add          xq, mmsize/4
    cmp          xd, wd
    jl .loop
    RET
%endmacro

INIT_XMM sse4
PAL_DEMOD_ROW
INIT_YMM avx2
PAL_DEMOD_ROW
INIT_ZMM avx512
PAL_DEMOD_ROW
