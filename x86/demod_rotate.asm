;*****************************************************************************
;* NTSC demod chroma rotation onto the U/V axes
;*****************************************************************************

%include "x86inc.asm"
%include "util.asm"

SECTION_RODATA 64

; ROTATE's shift-emulation biases (see util.asm): S subtracted from
; (1 << 40) - 8192 so a logical shift floors the negated value exactly;
; d_sub26 removes the folded (1 << 40) >> 14 offset.
q_nbias14: times 8 dq (1 << 40) - 8192
d_sub26:   times 16 dd 1 << 26

SECTION .text

; u[x] = -(p[x]*bp + q[x]*bq + 8192) >> 14
; v[x] = -(q[x]*bp - p[x]*bq + 8192) >> 14
; The ragged tail (w = 758 is not a multiple of the vector width) is
; covered by an overlapping final vector at x = w - lanes, idempotent
; and reading/writing nothing past [w].
%macro DEMOD_ROTATE_ROW 0
cglobal demod_rotate_row, 6, 8, 12, u, v, p, q, bpq, w, x, xend
    movifnidn    wd, wm
%if mmsize == 64
    mov          xd, 0xAAAA               ; k2 selects odd dword lanes (BLEND_ODD)
    kmovw        k2, xd
%endif
    SPLATD       m8, [bpqq + 0]           ; bp
    SPLATD       m9, [bpqq + 4]           ; bq
    lea          xendd, [wd - mmsize/4]   ; last full-vector start
    xor          xd, xd
.block:
    movu         m0, [pq + xq*4]          ; p
    movu         m1, [qq + xq*4]          ; q
    ROTATE       m10, m0, m8, m1, m9, paddq   ; -(p*bp + q*bq + 8192) >> 14
    movu         [uq + xq*4], m10
    ROTATE       m10, m1, m8, m0, m9, psubq   ; -(q*bp - p*bq + 8192) >> 14
    movu         [vq + xq*4], m10
    cmp          xd, xendd
    je           .done
    add          xd, mmsize/4
    cmp          xd, xendd
    jl           .block
    mov          xd, xendd                ; final overlapping vector
    jmp          .block
.done:
    RET
%endmacro

INIT_XMM sse4
DEMOD_ROTATE_ROW
INIT_YMM avx2
DEMOD_ROTATE_ROW
INIT_ZMM avx512
DEMOD_ROTATE_ROW
