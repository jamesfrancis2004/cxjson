#pragma once
/*
Pure C99/C11 port of James Edward Anhalt III's itoa algorithm (https://github.com/jeaiii/itoa).
MIT License - Copyright(c) 2021 James Edward Anhalt III

Ultra-fast integer-to-string serialization:
- Single fixed-point multiplication (7.32 fixed point) instead of iterative hardware division.
- 2 digits written simultaneously via 16-bit word stores (DIGIT_PAIRS).
- Fully unrolled binary search tree on log10 length.
*/

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#define JEAIII_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define JEAIII_INLINE inline __attribute__((always_inline))
#else
#define JEAIII_INLINE inline
#endif

static const char jeaiii_digits_00_99[200] =
    "00010203040506070809" "10111213141516171819" "20212223242526272829" "30313233343536373839" "40414243444546474849"
    "50515253545556575859" "60616263646566676869" "70717273747576777879" "80818283848586878889" "90919293949596979899";

typedef struct { char t, o; } jeaiii_pair;

#define JEAIII_W(I, U) memcpy(&b[I], &jeaiii_digits_00_99[(U) * 2], 2)
#define JEAIII_A(I, N) (t = ((uint64_t)1 << (32 + (N) / 5 * (N) * 53 / 16)) / (uint32_t)(1e##N) + 1 + (N) / 6 - (N) / 8, \
                        t *= u, \
                        t >>= (N) / 5 * (N) * 53 / 16, \
                        t += (N) / 6 * 4, \
                        JEAIII_W(I, t >> 32))
#define JEAIII_S(I) (b[I] = (char)((uint64_t)10 * (uint32_t)(t) >> 32) + '0')
#define JEAIII_D(I) (t = (uint64_t)100 * (uint32_t)(t), JEAIII_W(I, t >> 32))

#define JEAIII_C0(I) (b[I] = (char)(u) + '0')
#define JEAIII_C1(I) JEAIII_W(I, u)
#define JEAIII_C2(I) (JEAIII_A(I, 1), JEAIII_S(I + 2))
#define JEAIII_C3(I) (JEAIII_A(I, 2), JEAIII_D(I + 2))
#define JEAIII_C4(I) (JEAIII_A(I, 3), JEAIII_D(I + 2), JEAIII_S(I + 4))
#define JEAIII_C5(I) (JEAIII_A(I, 4), JEAIII_D(I + 2), JEAIII_D(I + 4))
#define JEAIII_C6(I) (JEAIII_A(I, 5), JEAIII_D(I + 2), JEAIII_D(I + 4), JEAIII_S(I + 6))
#define JEAIII_C7(I) (JEAIII_A(I, 6), JEAIII_D(I + 2), JEAIII_D(I + 4), JEAIII_D(I + 6))
#define JEAIII_C8(I) (JEAIII_A(I, 7), JEAIII_D(I + 2), JEAIII_D(I + 4), JEAIII_D(I + 6), JEAIII_S(I + 8))
#define JEAIII_C9(I) (JEAIII_A(I, 8), JEAIII_D(I + 2), JEAIII_D(I + 4), JEAIII_D(I + 6), JEAIII_D(I + 8))

#define JEAIII_L(N, A, B) (u < (uint32_t)(1e##N) ? (A) : (B))
#define JEAIII_L09(F) JEAIII_L(2, JEAIII_L(1, F(0), F(1)), JEAIII_L(6, JEAIII_L(4, JEAIII_L(3, F(2), F(3)), JEAIII_L(5, F(4), F(5))), JEAIII_L(8, JEAIII_L(7, F(6), F(7)), JEAIII_L(9, F(8), F(9)))))
#define JEAIII_L03(F) JEAIII_L(2, JEAIII_L(1, F(0), F(1)), JEAIII_L(3, F(2), F(3)))

#define JEAIII_K(N) (JEAIII_C##N(0), b + (N) + 1)
#define JEAIII_KX(N) (JEAIII_C##N(0), u = x, JEAIII_C7((N) + 1), b + (N) + 9)
#define JEAIII_KYX(N) (JEAIII_C##N(0), u = y, JEAIII_C7((N) + 1), u = x, JEAIII_C7((N) + 9), b + (N) + 17)

static JEAIII_INLINE char *jeaiii_to_text_u32(char *b, uint32_t u) {
    uint64_t t;
    return JEAIII_L09(JEAIII_K);
}

static JEAIII_INLINE char *jeaiii_to_text_i32(char *b, int32_t i) {
    uint32_t u = (uint32_t)i;
    if (i < 0) {
        *b++ = '-';
        u = 0 - u;
    }
    return jeaiii_to_text_u32(b, u);
}

static JEAIII_INLINE char *jeaiii_to_text_u64(char *b, uint64_t val) {
    uint64_t t = val;
    uint32_t u = (uint32_t)t;

    if (val <= 0xFFFFFFFFULL || (uint64_t)u == t) {
        return JEAIII_L09(JEAIII_K);
    }

    uint32_t x = (uint32_t)(t % 100000000ULL);
    t /= 100000000ULL;
    u = (uint32_t)t;

    if ((uint64_t)u == t) {
        return JEAIII_L09(JEAIII_KX);
    }

    uint32_t y = (uint32_t)(t % 100000000ULL);
    u = (uint32_t)(t / 100000000ULL);
    return JEAIII_L03(JEAIII_KYX);
}

static JEAIII_INLINE char *jeaiii_to_text_i64(char *b, int64_t i) {
    uint64_t u = (uint64_t)i;
    if (i < 0) {
        *b++ = '-';
        u = 0 - u;
    }
    return jeaiii_to_text_u64(b, u);
}
