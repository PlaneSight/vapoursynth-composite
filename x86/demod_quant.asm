;*****************************************************************************
;* NTSC demod output quantize: clamp_u16(base + rdiv(in * K, den))
;*****************************************************************************

%include "x86inc.asm"
%include "util.asm"

SECTION .text

; Replaces the per-pixel 64-bit rdiv with a precomputed magic reciprocal
; (see decode.h / D048): for int32 in n,
;   q   = (|n| * mul + add) >> 31          ; 32x32->64, unsigned
;   out = clamp_u16(base + sign(n) * q)
; mul, add, base come from comp_magic_t via a pointer (GPR-only ABI).
; Integer throughout, byte-identical to the C reference over the input
; range the demod produces (the shift is fixed at 31). packusdw is the
; clamp: it unsigned-saturates int32 -> u16 to [0, 65535]. The final
; vector is processed at x = w - lanes (recomputing a few already-done
; lanes with identical bits), so no load or store passes [w].

; q = (|n| * mul + add) >> 31 for the int32 magnitudes in %1, result
; packed back into the dword lanes of %1. %2 = mul (dword bcast),
; %3 = add (64-bit bcast); %4 scratch (must not alias %1). Even/odd
; 32x32->64 like pal_demod, recombined with util's BLEND_ODD.
%macro MAGIC4 4 ; abs(dst), mul, add, sOdd
    pshufd       %4, %1, 0xF5             ; odd dwords -> even positions
    pmuludq      %1, %2                   ; |n|even * mul (64)
    pmuludq      %4, %2                   ; |n|odd  * mul (64)
    paddq        %1, %3
    paddq        %4, %3
    psrlq        %1, 31                   ; even q's in dwords 0,2,..
    psrlq        %4, 31                   ; odd  q's in dwords 0,2,..
    psllq        %4, 32                   ; odd q's -> dwords 1,3,..
    BLEND_ODD    %1, %1, %4
%endmacro

; clamp the int32 result in m1 to u16 and store at %1.
; sse4/avx2 use packusdw (unsigned-saturating dword->word, per 128-bit
; lane) then gather the low halves; avx512 uses vpmovusdw (unsigned
; saturating dword->word across the whole register) which is clamp_u16.
%macro CLAMP_STORE 1 ; mem
%if mmsize == 64
    ; vpmovusdw is unsigned-saturating, so a negative int32 would wrap to
    ; 65535 instead of clamping to 0; floor at 0 first, then it saturates
    ; only the high side (packusdw on the narrower tiers is signed-source
    ; and needs no such guard).
    vpmaxsd      m1, m1, m8
    vpmovusdw    %1, m1
%elif mmsize == 32
    packusdw     m1, m1                    ; per-128: [w0..3 | w4..7] in low 64s
    vpermq       m1, m1, 0x08             ; both low 64s -> low 128
    movu         %1, xm1
%else
    packusdw     m1, m1
    movq         %1, m1
%endif
%endmacro

; void demod_quant_row(uint16_t *out, const int32_t *in,
;                      const comp_magic_t *mag, int w)
%macro DEMOD_QUANT_ROW 0
cglobal demod_quant_row, 4, 6, 9, out, in, mag, w, x, xend
    movifnidn    wd, wm
%if mmsize == 64
    vpxord       m8, m8, m8                ; the 0 floor for vpmaxsd
    mov          xendd, 0xAAAA             ; k2 = odd dword lanes (BLEND_ODD)
    kmovw        k2, xendd
%endif
    SPLATD       m5, [magq + 0]            ; mul
    movd         xm6, [magq + 4]           ; add (32-bit, used as 64-bit addend)
%if cpuflag(avx2)
    vpbroadcastq m6, xm6
%else
    punpcklqdq   m6, m6, m6
%endif
    SPLATD       m7, [magq + 8]            ; base
    lea          xendd, [wd - mmsize/4]    ; last full-vector start
    xor          xd, xd
.block:
    movu         m0, [inq + xq*4]
    pabsd        m1, m0
    MAGIC4       m1, m5, m6, m2
    psrad        m0, 31                    ; sign mask
    pxor         m1, m0
    psubd        m1, m0                    ; sign(n) * q
    paddd        m1, m7                    ; base + .
    CLAMP_STORE  [outq + xq*2]             ; clamp_u16 + store
    cmp          xd, xendd
    je           .done                     ; just did the last block
    add          xd, mmsize/4
    cmp          xd, xendd
    jl           .block
    mov          xd, xendd                 ; final overlapping vector
    jmp          .block
.done:
    RET
%endmacro

INIT_XMM sse4
DEMOD_QUANT_ROW
INIT_YMM avx2
DEMOD_QUANT_ROW
INIT_ZMM avx512
DEMOD_QUANT_ROW
