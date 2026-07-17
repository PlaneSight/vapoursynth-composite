;*****************************************************************************
;* Staged LUT-filter row kernels: per-bin magnitudes and gain application
;*****************************************************************************

%include "x86inc.asm"

SECTION .text

; Both kernels walk the bin-row table: rows[i] = { own, ref } float
; offsets of the row's own and reflected tile rows. The x band is three
; complex bins starting at XTILE/8 = 2 (float 4 of the row); the
; reflected band is three bins starting at XTILE/4 = 4, read and
; written reversed. Rows are XCOMPLEX = 9 complex (72 bytes), so the
; 16-byte tails of the band loads stay inside the row.

; Squared magnitudes of 4 complex values from two 16-byte loads. The
; fma3 form single-rounds the im^2 accumulation -- one rounding fewer
; than the unfused C reference, covered by a small ULP tolerance in
; checkasm; the sse2 form stays bit-exact.
%macro MAG4 3 ; out, srclo, srchi (clobbers srclo, srchi)
%if cpuflag(fma3)
    shufps       %1, %2, %3, 0x88
    shufps       %2, %2, %3, 0xDD
    mulps        %1, %1
    vfmadd231ps  %1, %2, %2
%else
    mulps        %2, %2
    mulps        %3, %3
    shufps       %1, %2, %3, 0x88
    shufps       %2, %2, %3, 0xDD
    addps        %1, %2
%endif
%endmacro

; void t3d_mag(float *m_in, float *m_ref, const float *in,
;              const int32_t (*rows)[2], int nrows)
; stores 4 lanes per 3-bin row: the callers pad by COMP_T3D_BINPAD
%macro T3D_MAG 0
cglobal t3d_mag, 5, 8, 6, mi, mr, in, rows, n, i0, i1
.row:
    mov          i0d, [rowsq]
    mov          i1d, [rowsq + 4]
    movu         m0, [inq + i0q*4 + 16]  ; x = 2, 3
    movu         m1, [inq + i0q*4 + 32]  ; x = 4, 5
    MAG4         m2, m0, m1
    movu         [miq], m2               ; m2 m3 m4 | m5 slack
    movu         m0, [inq + i1q*4 + 24]  ; x = 3, 4
    movu         m1, [inq + i1q*4 + 40]  ; x = 5, 6
    MAG4         m2, m0, m1
    shufps       m2, m2, 0x1B            ; reversed reflection
    movu         [mrq], m2               ; m6 m5 m4 | m3 slack
    add          rowsq, 8
    add          miq, 12
    add          mrq, 12
    dec          nd
    jg .row
    RET
%endmacro

; void t3d_apply(float *out, const float *in, const float *g,
;                const int32_t (*rows)[2], int nrows)
; row-order writes keep the twice-visited x = XTILE/4 column's
; later-write-wins semantics; stores touch only the two bands
%macro T3D_APPLY 0
cglobal t3d_apply, 5, 8, 6, out, in, g, rows, n, i0, i1
.row:
    mov          i0d, [rowsq]
    mov          i1d, [rowsq + 4]
    movu         m2, [gq]                ; g0 g1 g2 | pad
    unpcklps     m3, m2, m2              ; g0 g0 g1 g1
    unpckhps     m4, m2, m2              ; g2 g2 . .
    movu         m0, [inq + i0q*4 + 16]
    movsd        m1, [inq + i0q*4 + 32]
    mulps        m0, m3
    mulps        m1, m4
    movu         [outq + i0q*4 + 16], m0
    movsd        [outq + i0q*4 + 32], m1
    shufps       m5, m4, m3, 0xE4        ; g2 g2 g1 g1
    movu         m0, [inq + i1q*4 + 32]
    movsd        m1, [inq + i1q*4 + 48]
    mulps        m0, m5
    mulps        m1, m3                  ; low lanes g0 g0
    movu         [outq + i1q*4 + 32], m0
    movsd        [outq + i1q*4 + 48], m1
    add          rowsq, 8
    add          gq, 12
    dec          nd
    jg .row
    RET
%endmacro

INIT_XMM sse2
T3D_MAG
T3D_APPLY
INIT_XMM avx2
T3D_MAG
T3D_APPLY
