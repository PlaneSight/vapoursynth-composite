#ifndef COMP_CPU_H
#define COMP_CPU_H

#define COMP_CPU_SSE2   (1 << 0)
#define COMP_CPU_SSE41  (1 << 1)
#define COMP_CPU_AVX2   (1 << 2)
#define COMP_CPU_AVX512 (1 << 3)

unsigned comp_cpu_detect(void);

#endif
