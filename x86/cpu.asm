;*****************************************************************************
;* CPU feature detection helpers (after dav1d src/x86/cpu.asm)
;*****************************************************************************

%include "x86inc.asm"

SECTION .text

; void cpu_cpuid(uint32_t regs[4], unsigned leaf, unsigned subleaf)
cglobal cpu_cpuid, 3, 6, 0, regs, leaf, subleaf
    mov          r5, rbx
    mov          r4, regsq
    mov         eax, leafd
    mov         ecx, subleafd
    cpuid
    mov     [r4+ 0], eax
    mov     [r4+ 4], ebx
    mov     [r4+ 8], ecx
    mov     [r4+12], edx
    mov         rbx, r5
    RET

; uint64_t cpu_xgetbv(unsigned xcr)
cglobal cpu_xgetbv, 1, 3, 0, xcr
    mov         ecx, xcrd
    xgetbv
    shl         rdx, 32
    or          rax, rdx
    RET
