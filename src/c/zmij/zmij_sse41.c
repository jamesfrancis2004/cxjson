/* Runtime-dispatched SSE4.1 compilation of zmij for universal x86_64 wheels.
 * Enables single-shuffle _mm_shuffle_epi8 digit assembly on SSE4.1+ CPUs
 * while keeping the base wheel compatible with SSE2-only x86_64 CPUs. */

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER) && !defined(__SSE4_1__)

#if defined(__clang__)
#pragma clang attribute push (__attribute__((target("sse4.1"))), apply_to = function)
#else
#pragma GCC push_options
#pragma GCC target("sse4.1")
#endif

#define ZMIJ_USE_SSE4_1 1
#define zmij_detail_write zmij_detail_write_sse41
#define zmij_detail_write_f zmij_detail_write_f_sse41
#define zmij_detail_write_scientific zmij_detail_write_scientific_sse41
#define zmij_detail_write_scientific_f zmij_detail_write_scientific_f_sse41
#define zmij_detail_write_scientific_big zmij_detail_write_scientific_big_sse41
#define zmij_detail_write_general zmij_detail_write_general_sse41
#define zmij_detail_write_general_f zmij_detail_write_general_f_sse41
#define zmij_detail_write_general_big zmij_detail_write_general_big_sse41
#define zmij_detail_write_fixed zmij_detail_write_fixed_sse41
#define zmij_detail_write_fixed_f zmij_detail_write_fixed_f_sse41
#define zmij_detail_write_fixed_big zmij_detail_write_fixed_big_sse41
#define zmij_detail_write_hex zmij_detail_write_hex_sse41

#include "zmij.c"

#if defined(__clang__)
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif

#else
typedef int cjson_zmij_sse41_unused_t;
#endif
