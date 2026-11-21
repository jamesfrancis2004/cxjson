#ifndef CJSON_COMPAT_H
#define CJSON_COMPAT_H

#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>
#define CJSON_ALWAYS_INLINE __forceinline
#define CJSON_NOINLINE      __declspec(noinline)
#define CJSON_ALIGNED(n)    __declspec(align(n))
#define CJSON_LITTLE_ENDIAN 1
#ifndef __BYTE_ORDER__
#define __BYTE_ORDER__ 1234
#endif
#ifndef __ORDER_LITTLE_ENDIAN__
#define __ORDER_LITTLE_ENDIAN__ 1234
#endif
#ifndef __attribute__
#define __attribute__(x)
#endif

static inline unsigned int cjson_ctz32(uint32_t x) {
  unsigned long idx = 0;
  _BitScanForward(&idx, (unsigned long)x);
  return (unsigned int)idx;
}

static inline unsigned int cjson_ctz64(uint64_t x) {
  unsigned long idx = 0;
  _BitScanForward64(&idx, x);
  return (unsigned int)idx;
}

static inline int cjson_clz64(uint64_t x) {
  unsigned long idx = 0;
  _BitScanReverse64(&idx, x);
  return 63 - (int)idx;
}

#define __builtin_ctz(x)   cjson_ctz32(x)
#define __builtin_ctzll(x) cjson_ctz64(x)
#define __builtin_clzll(x) cjson_clz64(x)

static inline uint64_t cjson_umul128(uint64_t a, uint64_t b, uint64_t *hi) {
  return _umul128(a, b, hi);
}

static inline uint32_t cjson_bswap32(uint32_t x) {
  return (uint32_t)_byteswap_ulong((unsigned long)x);
}

static inline uint64_t cjson_bswap64(uint64_t x) {
  return _byteswap_uint64(x);
}
#else
#define CJSON_ALWAYS_INLINE __attribute__((always_inline)) static inline
#define CJSON_NOINLINE      __attribute__((noinline))
#define CJSON_ALIGNED(n)    __attribute__((aligned(n)))
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CJSON_LITTLE_ENDIAN 1
#else
#define CJSON_LITTLE_ENDIAN 0
#endif

static inline unsigned int cjson_ctz32(uint32_t x) {
  return (unsigned int)__builtin_ctz(x);
}

static inline unsigned int cjson_ctz64(uint64_t x) {
  return (unsigned int)__builtin_ctzll(x);
}

static inline int cjson_clz64(uint64_t x) {
  return __builtin_clzll(x);
}

static inline uint64_t cjson_umul128(uint64_t a, uint64_t b, uint64_t *hi) {
  __uint128_t p = (__uint128_t)a * (__uint128_t)b;
  *hi = (uint64_t)(p >> 64);
  return (uint64_t)p;
}

static inline uint32_t cjson_bswap32(uint32_t x) {
  return __builtin_bswap32(x);
}

static inline uint64_t cjson_bswap64(uint64_t x) {
  return __builtin_bswap64(x);
}
#endif

#endif /* CJSON_COMPAT_H */
