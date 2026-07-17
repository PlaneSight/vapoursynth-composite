#include <checkasm/checkasm.h>

#include "cpu.h"

void checkasm_test_pal_demod(void);

static const CheckasmCpuInfo cpu_flags[] = {
    { "SSE2", "sse2", COMP_CPU_SSE2 },
    { "SSE4.1", "sse4", COMP_CPU_SSE41 },
    { "AVX2", "avx2", COMP_CPU_AVX2 },
    { "AVX512", "avx512", COMP_CPU_AVX512 },
    {0}
};

static const CheckasmTest tests[] = {
    { "pal_demod", checkasm_test_pal_demod },
    {0}
};

int main(int argc, const char *argv[])
{
    CheckasmConfig config = {
        .cpu_flags = cpu_flags,
        .tests = tests,
        .cpu = comp_cpu_detect(),
    };
    return checkasm_main(&config, argc, argv);
}
