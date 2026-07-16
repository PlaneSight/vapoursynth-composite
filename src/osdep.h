/*
 * Compiler portability helpers.
 */

#ifndef COMP_OSDEP_H
#define COMP_OSDEP_H

#ifdef _MSC_VER
#define DECLARE_ALIGNED( var, n ) __declspec(align(n)) var
#else
#define DECLARE_ALIGNED( var, n ) var __attribute__((aligned(n)))
#endif

#define ALIGNED_32( var ) DECLARE_ALIGNED( var, 32 )

#endif
