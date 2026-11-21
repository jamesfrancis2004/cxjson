#ifndef CJSON_SIMD_H
#define CJSON_SIMD_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cjson_compat.h"

#if defined(__GNUC__) || defined(__clang__)
#define CJSON_LIKELY(x)   __builtin_expect(!!(x), 1)
#define CJSON_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define CJSON_LIKELY(x)   (x)
#define CJSON_UNLIKELY(x) (x)
#endif

#if !defined(CJSON_NO_SIMD)
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define CJSON_SIMD_NEON 1
#include <arm_neon.h>
#elif defined(__AVX2__)
#define CJSON_SIMD_AVX2 1
#include <immintrin.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define CJSON_SIMD_SSE2 1
#include <emmintrin.h>
#endif
#endif

/* Overlapping 1/2/4/8-byte copy for short strings (avoids libc memcpy PLT call) */
static inline void copy_short_ascii(char *dst, const char *src, size_t len) {
  if (len >= 8) {
    uint64_t a, b;
    memcpy(&a, src, 8);
    memcpy(&b, src + len - 8, 8);
    memcpy(dst, &a, 8);
    memcpy(dst + len - 8, &b, 8);
  } else if (len >= 4) {
    uint32_t a, b;
    memcpy(&a, src, 4);
    memcpy(&b, src + len - 4, 4);
    memcpy(dst, &a, 4);
    memcpy(dst + len - 4, &b, 4);
  } else if (len >= 2) {
    uint16_t a;
    memcpy(&a, src, 2);
    memcpy(dst, &a, 2);
    dst[len - 1] = src[len - 1];
  } else if (len == 1) {
    dst[0] = src[0];
  }
}

/* Check whether any byte in s[0..len) has high bit (>= 0x80) set */
static inline int bytes_have_hi_small(const char *s, unsigned int len) {
  if (len <= 8) {
    if (len == 0) return 0;
    if (len == 8) {
      uint64_t w;
      memcpy(&w, s, 8);
      return (w & 0x8080808080808080ULL) != 0;
    }
    if (len >= 4) {
      uint32_t a, b;
      memcpy(&a, s, 4);
      memcpy(&b, s + len - 4, 4);
      return ((a | b) & 0x80808080u) != 0;
    }
    uint8_t acc = (uint8_t)s[0] | (uint8_t)s[len >> 1] | (uint8_t)s[len - 1];
    return (acc & 0x80) != 0;
  }
  uint64_t a, b;
  memcpy(&a, s, 8);
  memcpy(&b, s + len - 8, 8);
  return ((a | b) & 0x8080808080808080ULL) != 0;
}

/* SWAR zero-byte / special-char detector (< 0x20, '"', '\\') */
static inline uint64_t swar_esc(uint64_t w) {
  const uint64_t lo = 0x0101010101010101ULL;
  const uint64_t hi = 0x8080808080808080ULL;
  uint64_t q = w ^ (lo * 0x22);
  uint64_t s = w ^ (lo * 0x5C);
  return ((((w - lo * 0x20) & ~w) | ((q - lo) & ~q) | ((s - lo) & ~s)) & hi);
}

/* Signed-< 0x20 ASCII special detector (catches '"', '\\', < 0x20, AND >= 0x80 in one pass) */
static inline uint64_t swar_str_ascii_mask(uint64_t w) {
  return swar_esc(w) | (w & 0x8080808080808080ULL);
}

#if !defined(CJSON_SIMD_NEON) && !defined(CJSON_SIMD_AVX2) && !defined(CJSON_SIMD_SSE2)
static inline uint64_t swar_str_special(uint64_t w) {
  return swar_esc(w);
}
#endif

#if defined(CJSON_SIMD_NEON)
static inline uint8x16_t neon_esc_mask(uint8x16_t v, uint8x16_t q_22, uint8x16_t q_5c, uint8x16_t q_20) {
  return vorrq_u8(vorrq_u8(vceqq_u8(v, q_22), vceqq_u8(v, q_5c)), vcltq_u8(v, q_20));
}

static inline uint8x16_t neon_str_ascii_mask(uint8x16_t v, uint8x16_t q_22, uint8x16_t q_5c, int8x16_t q_s20) {
  return vorrq_u8(vorrq_u8(vceqq_u8(v, q_22), vceqq_u8(v, q_5c)),
                  vcltq_s8(vreinterpretq_s8_u8(v), q_s20));
}

static inline int neon_is_zero(uint8x16_t m) {
  uint8x8_t lo = vorr_u8(vget_low_u8(m), vget_high_u8(m));
  return vget_lane_u64(vreinterpret_u64_u8(lo), 0) == 0;
}
#elif defined(CJSON_SIMD_AVX2)
static inline __m256i avx2_esc_mask(__m256i v, __m256i q_22, __m256i q_5c, __m256i q_1f, __m256i zero) {
  return _mm256_or_si256(
      _mm256_or_si256(_mm256_cmpeq_epi8(v, q_22), _mm256_cmpeq_epi8(v, q_5c)),
      _mm256_cmpeq_epi8(_mm256_subs_epu8(v, q_1f), zero));
}

static inline __m256i avx2_str_ascii_mask(__m256i v, __m256i q_22, __m256i q_5c, __m256i q_20) {
  return _mm256_or_si256(
      _mm256_or_si256(_mm256_cmpeq_epi8(v, q_22), _mm256_cmpeq_epi8(v, q_5c)),
      _mm256_cmpgt_epi8(q_20, v));
}

static inline __m128i sse2_esc_mask(__m128i v, __m128i q_22, __m128i q_5c, __m128i q_1f, __m128i zero) {
  return _mm_or_si128(
      _mm_or_si128(_mm_cmpeq_epi8(v, q_22), _mm_cmpeq_epi8(v, q_5c)),
      _mm_cmpeq_epi8(_mm_subs_epu8(v, q_1f), zero));
}

static inline __m128i sse2_str_ascii_mask(__m128i v, __m128i q_22, __m128i q_5c, __m128i q_20) {
  return _mm_or_si128(
      _mm_or_si128(_mm_cmpeq_epi8(v, q_22), _mm_cmpeq_epi8(v, q_5c)),
      _mm_cmpgt_epi8(q_20, v));
}

static inline __m256i avx2_str_special(__m256i v, __m256i v_q, __m256i v_bs, __m256i v_1f, __m256i v_zero) {
  return avx2_esc_mask(v, v_q, v_bs, v_1f, v_zero);
}
#elif defined(CJSON_SIMD_SSE2)
static inline __m128i sse2_esc_mask(__m128i v, __m128i q_22, __m128i q_5c, __m128i q_1f, __m128i zero) {
  return _mm_or_si128(
      _mm_or_si128(_mm_cmpeq_epi8(v, q_22), _mm_cmpeq_epi8(v, q_5c)),
      _mm_cmpeq_epi8(_mm_subs_epu8(v, q_1f), zero));
}

static inline __m128i sse2_str_ascii_mask(__m128i v, __m128i q_22, __m128i q_5c, __m128i q_20) {
  return _mm_or_si128(
      _mm_or_si128(_mm_cmpeq_epi8(v, q_22), _mm_cmpeq_epi8(v, q_5c)),
      _mm_cmpgt_epi8(q_20, v));
}

static inline __m128i sse2_str_special(__m128i v, __m128i v_q, __m128i v_bs, __m128i v_1f, __m128i v_zero) {
  return sse2_esc_mask(v, v_q, v_bs, v_1f, v_zero);
}
#endif

/* Tiny always-inline check+copy for strings <= 64B (keeps caller hot loops register-resident) */
__attribute__((always_inline))
static inline int copy_if_clean_short(char *dst, const char *src, Py_ssize_t len) {
  if (len < 16) {
    if (len >= 8) {
      uint64_t a, b;
      memcpy(&a, src, 8);
      memcpy(&b, src + len - 8, 8);
      if (CJSON_UNLIKELY((swar_esc(a) | swar_esc(b)) != 0)) return 0;
      memcpy(dst, &a, 8);
      memcpy(dst + len - 8, &b, 8);
      return 1;
    }
    if (len >= 4) {
      uint32_t a, b;
      memcpy(&a, src, 4);
      memcpy(&b, src + len - 4, 4);
      if (CJSON_UNLIKELY(swar_esc((uint64_t)a | ((uint64_t)b << 32)) != 0)) return 0;
      memcpy(dst, &a, 4);
      memcpy(dst + len - 4, &b, 4);
      return 1;
    }
    if (len == 2) {
      unsigned char c0 = (unsigned char)src[0];
      unsigned char c1 = (unsigned char)src[1];
      if (CJSON_UNLIKELY(c0 < 32 || c0 == '"' || c0 == '\\' || c1 < 32 || c1 == '"' || c1 == '\\')) return 0;
      dst[0] = (char)c0;
      dst[1] = (char)c1;
      return 1;
    }
    if (len == 3) {
      unsigned char c0 = (unsigned char)src[0];
      unsigned char c1 = (unsigned char)src[1];
      unsigned char c2 = (unsigned char)src[2];
      if (CJSON_UNLIKELY(c0 < 32 || c0 == '"' || c0 == '\\' ||
                         c1 < 32 || c1 == '"' || c1 == '\\' ||
                         c2 < 32 || c2 == '"' || c2 == '\\')) return 0;
      dst[0] = (char)c0;
      dst[1] = (char)c1;
      dst[2] = (char)c2;
      return 1;
    }
    if (len == 1) {
      unsigned char c = (unsigned char)src[0];
      if (CJSON_UNLIKELY(c < 32 || c == '"' || c == '\\')) return 0;
      dst[0] = (char)c;
      return 1;
    }
    return 1;
  }
#if defined(CJSON_SIMD_NEON)
  const uint8x16_t q_22 = vdupq_n_u8(0x22);
  const uint8x16_t q_5c = vdupq_n_u8(0x5C);
  const uint8x16_t q_20 = vdupq_n_u8(0x20);
  if (len <= 32) {
    uint8x16_t v0 = vld1q_u8((const uint8_t *)src);
    uint8x16_t v1 = vld1q_u8((const uint8_t *)src + len - 16);
    vst1q_u8((uint8_t *)dst, v0);
    vst1q_u8((uint8_t *)dst + len - 16, v1);
    uint8x16_t m = vorrq_u8(neon_esc_mask(v0, q_22, q_5c, q_20),
                            neon_esc_mask(v1, q_22, q_5c, q_20));
    return neon_is_zero(m);
  }
  uint8x16_t v0 = vld1q_u8((const uint8_t *)src);
  uint8x16_t v1 = vld1q_u8((const uint8_t *)src + 16);
  uint8x16_t v2 = vld1q_u8((const uint8_t *)src + len - 32);
  uint8x16_t v3 = vld1q_u8((const uint8_t *)src + len - 16);
  vst1q_u8((uint8_t *)dst, v0);
  vst1q_u8((uint8_t *)dst + 16, v1);
  vst1q_u8((uint8_t *)dst + len - 32, v2);
  vst1q_u8((uint8_t *)dst + len - 16, v3);
  uint8x16_t m01 = vorrq_u8(neon_esc_mask(v0, q_22, q_5c, q_20),
                            neon_esc_mask(v1, q_22, q_5c, q_20));
  uint8x16_t m23 = vorrq_u8(neon_esc_mask(v2, q_22, q_5c, q_20),
                            neon_esc_mask(v3, q_22, q_5c, q_20));
  return neon_is_zero(vorrq_u8(m01, m23));
#elif defined(CJSON_SIMD_AVX2)
  if (len <= 32) {
    __m128i q_22 = _mm_set1_epi8(0x22);
    __m128i q_5c = _mm_set1_epi8(0x5C);
    __m128i q_1f = _mm_set1_epi8(0x1F);
    __m128i zero = _mm_setzero_si128();
    __m128i v0 = _mm_loadu_si128((const __m128i *)src);
    __m128i v1 = _mm_loadu_si128((const __m128i *)(src + len - 16));
    _mm_storeu_si128((__m128i *)dst, v0);
    _mm_storeu_si128((__m128i *)(dst + len - 16), v1);
    __m128i m = _mm_or_si128(sse2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                             sse2_esc_mask(v1, q_22, q_5c, q_1f, zero));
    return _mm_movemask_epi8(m) == 0;
  }
  __m256i q_22 = _mm256_set1_epi8(0x22);
  __m256i q_5c = _mm256_set1_epi8(0x5C);
  __m256i q_1f = _mm256_set1_epi8(0x1F);
  __m256i zero = _mm256_setzero_si256();
  __m256i v0 = _mm256_loadu_si256((const __m256i *)src);
  __m256i v1 = _mm256_loadu_si256((const __m256i *)(src + len - 32));
  _mm256_storeu_si256((__m256i *)dst, v0);
  _mm256_storeu_si256((__m256i *)(dst + len - 32), v1);
  __m256i m = _mm256_or_si256(avx2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                              avx2_esc_mask(v1, q_22, q_5c, q_1f, zero));
  return _mm256_movemask_epi8(m) == 0;
#elif defined(CJSON_SIMD_SSE2)
  __m128i q_22 = _mm_set1_epi8(0x22);
  __m128i q_5c = _mm_set1_epi8(0x5C);
  __m128i q_1f = _mm_set1_epi8(0x1F);
  __m128i zero = _mm_setzero_si128();
  if (len <= 32) {
    __m128i v0 = _mm_loadu_si128((const __m128i *)src);
    __m128i v1 = _mm_loadu_si128((const __m128i *)(src + len - 16));
    _mm_storeu_si128((__m128i *)dst, v0);
    _mm_storeu_si128((__m128i *)(dst + len - 16), v1);
    __m128i m = _mm_or_si128(sse2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                             sse2_esc_mask(v1, q_22, q_5c, q_1f, zero));
    return _mm_movemask_epi8(m) == 0;
  }
  __m128i v0 = _mm_loadu_si128((const __m128i *)src);
  __m128i v1 = _mm_loadu_si128((const __m128i *)(src + 16));
  __m128i v2 = _mm_loadu_si128((const __m128i *)(src + len - 32));
  __m128i v3 = _mm_loadu_si128((const __m128i *)(src + len - 16));
  _mm_storeu_si128((__m128i *)dst, v0);
  _mm_storeu_si128((__m128i *)(dst + 16), v1);
  _mm_storeu_si128((__m128i *)(dst + len - 32), v2);
  _mm_storeu_si128((__m128i *)(dst + len - 16), v3);
  __m128i m01 = _mm_or_si128(sse2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                             sse2_esc_mask(v1, q_22, q_5c, q_1f, zero));
  __m128i m23 = _mm_or_si128(sse2_esc_mask(v2, q_22, q_5c, q_1f, zero),
                             sse2_esc_mask(v3, q_22, q_5c, q_1f, zero));
  return _mm_movemask_epi8(_mm_or_si128(m01, m23)) == 0;
#else
  if (len <= 32) {
    uint64_t a0, a1, a2, a3;
    memcpy(&a0, src, 8);
    memcpy(&a1, src + 8, 8);
    memcpy(&a2, src + len - 16, 8);
    memcpy(&a3, src + len - 8, 8);
    if (CJSON_UNLIKELY((swar_esc(a0) | swar_esc(a1) | swar_esc(a2) | swar_esc(a3)) != 0)) return 0;
    memcpy(dst, &a0, 8);
    memcpy(dst + 8, &a1, 8);
    memcpy(dst + len - 16, &a2, 8);
    memcpy(dst + len - 8, &a3, 8);
    return 1;
  }
  uint64_t acc = 0;
  Py_ssize_t i = 0;
  for (; i + 32 <= len; i += 32) {
    uint64_t a0, a1, a2, a3;
    memcpy(&a0, src + i, 8);
    memcpy(&a1, src + i + 8, 8);
    memcpy(&a2, src + i + 16, 8);
    memcpy(&a3, src + i + 24, 8);
    memcpy(dst + i, &a0, 8);
    memcpy(dst + i + 8, &a1, 8);
    memcpy(dst + i + 16, &a2, 8);
    memcpy(dst + i + 24, &a3, 8);
    acc |= swar_esc(a0) | swar_esc(a1) | swar_esc(a2) | swar_esc(a3);
  }
  for (; i + 8 <= len; i += 8) {
    uint64_t a;
    memcpy(&a, src + i, 8);
    memcpy(dst + i, &a, 8);
    acc |= swar_esc(a);
  }
  if (i < len) {
    uint64_t a;
    memcpy(&a, src + len - 8, 8);
    memcpy(dst + len - 8, &a, 8);
    acc |= swar_esc(a);
  }
  return acc == 0;
#endif
}

static inline int copy_if_clean(char *dst, const char *src, Py_ssize_t len) {
  if (len < 16) {
    if (len >= 8) {
      uint64_t a, b;
      memcpy(&a, src, 8);
      memcpy(&b, src + len - 8, 8);
      if (CJSON_UNLIKELY((swar_esc(a) | swar_esc(b)) != 0)) return 0;
      memcpy(dst, &a, 8);
      memcpy(dst + len - 8, &b, 8);
      return 1;
    }
    if (len >= 4) {
      uint32_t a, b;
      memcpy(&a, src, 4);
      memcpy(&b, src + len - 4, 4);
      if (CJSON_UNLIKELY(swar_esc((uint64_t)a | ((uint64_t)b << 32)) != 0)) return 0;
      memcpy(dst, &a, 4);
      memcpy(dst + len - 4, &b, 4);
      return 1;
    }
    if (len == 2) {
      unsigned char c0 = (unsigned char)src[0];
      unsigned char c1 = (unsigned char)src[1];
      if (CJSON_UNLIKELY(c0 < 32 || c0 == '"' || c0 == '\\' || c1 < 32 || c1 == '"' || c1 == '\\')) return 0;
      dst[0] = (char)c0;
      dst[1] = (char)c1;
      return 1;
    }
    if (len == 3) {
      unsigned char c0 = (unsigned char)src[0];
      unsigned char c1 = (unsigned char)src[1];
      unsigned char c2 = (unsigned char)src[2];
      if (CJSON_UNLIKELY(c0 < 32 || c0 == '"' || c0 == '\\' ||
                         c1 < 32 || c1 == '"' || c1 == '\\' ||
                         c2 < 32 || c2 == '"' || c2 == '\\')) return 0;
      dst[0] = (char)c0;
      dst[1] = (char)c1;
      dst[2] = (char)c2;
      return 1;
    }
    if (len == 1) {
      unsigned char c = (unsigned char)src[0];
      if (CJSON_UNLIKELY(c < 32 || c == '"' || c == '\\')) return 0;
      dst[0] = (char)c;
      return 1;
    }
    return 1;
  }
#if defined(CJSON_SIMD_NEON)
  const uint8x16_t q_22 = vdupq_n_u8(0x22);
  const uint8x16_t q_5c = vdupq_n_u8(0x5C);
  const uint8x16_t q_20 = vdupq_n_u8(0x20);
  if (len <= 32) {
    uint8x16_t v0 = vld1q_u8((const uint8_t *)src);
    uint8x16_t v1 = vld1q_u8((const uint8_t *)src + len - 16);
    vst1q_u8((uint8_t *)dst, v0);
    vst1q_u8((uint8_t *)dst + len - 16, v1);
    uint8x16_t m = vorrq_u8(neon_esc_mask(v0, q_22, q_5c, q_20),
                            neon_esc_mask(v1, q_22, q_5c, q_20));
    return neon_is_zero(m);
  }
  if (len <= 64) {
    uint8x16_t v0 = vld1q_u8((const uint8_t *)src);
    uint8x16_t v1 = vld1q_u8((const uint8_t *)src + 16);
    uint8x16_t v2 = vld1q_u8((const uint8_t *)src + len - 32);
    uint8x16_t v3 = vld1q_u8((const uint8_t *)src + len - 16);
    vst1q_u8((uint8_t *)dst, v0);
    vst1q_u8((uint8_t *)dst + 16, v1);
    vst1q_u8((uint8_t *)dst + len - 32, v2);
    vst1q_u8((uint8_t *)dst + len - 16, v3);
    uint8x16_t m01 = vorrq_u8(neon_esc_mask(v0, q_22, q_5c, q_20),
                              neon_esc_mask(v1, q_22, q_5c, q_20));
    uint8x16_t m23 = vorrq_u8(neon_esc_mask(v2, q_22, q_5c, q_20),
                              neon_esc_mask(v3, q_22, q_5c, q_20));
    return neon_is_zero(vorrq_u8(m01, m23));
  }
  uint8x16_t m_acc = vdupq_n_u8(0);
  Py_ssize_t i = 0;
  for (; i + 64 <= len; i += 64) {
    uint8x16_t v0 = vld1q_u8((const uint8_t *)src + i);
    uint8x16_t v1 = vld1q_u8((const uint8_t *)src + i + 16);
    uint8x16_t v2 = vld1q_u8((const uint8_t *)src + i + 32);
    uint8x16_t v3 = vld1q_u8((const uint8_t *)src + i + 48);
    vst1q_u8((uint8_t *)dst + i, v0);
    vst1q_u8((uint8_t *)dst + i + 16, v1);
    vst1q_u8((uint8_t *)dst + i + 32, v2);
    vst1q_u8((uint8_t *)dst + i + 48, v3);
    uint8x16_t m01 = vorrq_u8(neon_esc_mask(v0, q_22, q_5c, q_20),
                              neon_esc_mask(v1, q_22, q_5c, q_20));
    uint8x16_t m23 = vorrq_u8(neon_esc_mask(v2, q_22, q_5c, q_20),
                              neon_esc_mask(v3, q_22, q_5c, q_20));
    m_acc = vorrq_u8(m_acc, vorrq_u8(m01, m23));
  }
  for (; i + 16 <= len; i += 16) {
    uint8x16_t v = vld1q_u8((const uint8_t *)src + i);
    vst1q_u8((uint8_t *)dst + i, v);
    m_acc = vorrq_u8(m_acc, neon_esc_mask(v, q_22, q_5c, q_20));
  }
  if (i < len) {
    uint8x16_t v = vld1q_u8((const uint8_t *)src + len - 16);
    vst1q_u8((uint8_t *)dst + len - 16, v);
    m_acc = vorrq_u8(m_acc, neon_esc_mask(v, q_22, q_5c, q_20));
  }
  return neon_is_zero(m_acc);
#elif defined(CJSON_SIMD_AVX2)
  if (len <= 32) {
    __m128i q_22 = _mm_set1_epi8(0x22);
    __m128i q_5c = _mm_set1_epi8(0x5C);
    __m128i q_1f = _mm_set1_epi8(0x1F);
    __m128i zero = _mm_setzero_si128();
    __m128i v0 = _mm_loadu_si128((const __m128i *)src);
    __m128i v1 = _mm_loadu_si128((const __m128i *)(src + len - 16));
    _mm_storeu_si128((__m128i *)dst, v0);
    _mm_storeu_si128((__m128i *)(dst + len - 16), v1);
    __m128i m = _mm_or_si128(sse2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                             sse2_esc_mask(v1, q_22, q_5c, q_1f, zero));
    return _mm_movemask_epi8(m) == 0;
  }
  __m256i q_22 = _mm256_set1_epi8(0x22);
  __m256i q_5c = _mm256_set1_epi8(0x5C);
  __m256i q_1f = _mm256_set1_epi8(0x1F);
  __m256i zero = _mm256_setzero_si256();
  if (len <= 64) {
    __m256i v0 = _mm256_loadu_si256((const __m256i *)src);
    __m256i v1 = _mm256_loadu_si256((const __m256i *)(src + len - 32));
    _mm256_storeu_si256((__m256i *)dst, v0);
    _mm256_storeu_si256((__m256i *)(dst + len - 32), v1);
    __m256i m = _mm256_or_si256(avx2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                                avx2_esc_mask(v1, q_22, q_5c, q_1f, zero));
    return _mm256_movemask_epi8(m) == 0;
  }
  __m256i m_acc = zero;
  Py_ssize_t i = 0;
  for (; i + 128 <= len; i += 128) {
    __m256i v0 = _mm256_loadu_si256((const __m256i *)(src + i));
    __m256i v1 = _mm256_loadu_si256((const __m256i *)(src + i + 32));
    __m256i v2 = _mm256_loadu_si256((const __m256i *)(src + i + 64));
    __m256i v3 = _mm256_loadu_si256((const __m256i *)(src + i + 96));
    _mm256_storeu_si256((__m256i *)(dst + i), v0);
    _mm256_storeu_si256((__m256i *)(dst + i + 32), v1);
    _mm256_storeu_si256((__m256i *)(dst + i + 64), v2);
    _mm256_storeu_si256((__m256i *)(dst + i + 96), v3);
    __m256i m01 = _mm256_or_si256(avx2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                                  avx2_esc_mask(v1, q_22, q_5c, q_1f, zero));
    __m256i m23 = _mm256_or_si256(avx2_esc_mask(v2, q_22, q_5c, q_1f, zero),
                                  avx2_esc_mask(v3, q_22, q_5c, q_1f, zero));
    m_acc = _mm256_or_si256(m_acc, _mm256_or_si256(m01, m23));
  }
  for (; i + 32 <= len; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
    _mm256_storeu_si256((__m256i *)(dst + i), v);
    m_acc = _mm256_or_si256(m_acc, avx2_esc_mask(v, q_22, q_5c, q_1f, zero));
  }
  if (i < len) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(src + len - 32));
    _mm256_storeu_si256((__m256i *)(dst + len - 32), v);
    m_acc = _mm256_or_si256(m_acc, avx2_esc_mask(v, q_22, q_5c, q_1f, zero));
  }
  return _mm256_movemask_epi8(m_acc) == 0;
#elif defined(CJSON_SIMD_SSE2)
  __m128i q_22 = _mm_set1_epi8(0x22);
  __m128i q_5c = _mm_set1_epi8(0x5C);
  __m128i q_1f = _mm_set1_epi8(0x1F);
  __m128i zero = _mm_setzero_si128();
  if (len <= 32) {
    __m128i v0 = _mm_loadu_si128((const __m128i *)src);
    __m128i v1 = _mm_loadu_si128((const __m128i *)(src + len - 16));
    _mm_storeu_si128((__m128i *)dst, v0);
    _mm_storeu_si128((__m128i *)(dst + len - 16), v1);
    __m128i m = _mm_or_si128(sse2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                             sse2_esc_mask(v1, q_22, q_5c, q_1f, zero));
    return _mm_movemask_epi8(m) == 0;
  }
  if (len <= 64) {
    __m128i v0 = _mm_loadu_si128((const __m128i *)src);
    __m128i v1 = _mm_loadu_si128((const __m128i *)(src + 16));
    __m128i v2 = _mm_loadu_si128((const __m128i *)(src + len - 32));
    __m128i v3 = _mm_loadu_si128((const __m128i *)(src + len - 16));
    _mm_storeu_si128((__m128i *)dst, v0);
    _mm_storeu_si128((__m128i *)(dst + 16), v1);
    _mm_storeu_si128((__m128i *)(dst + len - 32), v2);
    _mm_storeu_si128((__m128i *)(dst + len - 16), v3);
    __m128i m01 = _mm_or_si128(sse2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                               sse2_esc_mask(v1, q_22, q_5c, q_1f, zero));
    __m128i m23 = _mm_or_si128(sse2_esc_mask(v2, q_22, q_5c, q_1f, zero),
                               sse2_esc_mask(v3, q_22, q_5c, q_1f, zero));
    return _mm_movemask_epi8(_mm_or_si128(m01, m23)) == 0;
  }
  __m128i m_acc = zero;
  Py_ssize_t i = 0;
  for (; i + 64 <= len; i += 64) {
    __m128i v0 = _mm_loadu_si128((const __m128i *)(src + i));
    __m128i v1 = _mm_loadu_si128((const __m128i *)(src + i + 16));
    __m128i v2 = _mm_loadu_si128((const __m128i *)(src + i + 32));
    __m128i v3 = _mm_loadu_si128((const __m128i *)(src + i + 48));
    _mm_storeu_si128((__m128i *)(dst + i), v0);
    _mm_storeu_si128((__m128i *)(dst + i + 16), v1);
    _mm_storeu_si128((__m128i *)(dst + i + 32), v2);
    _mm_storeu_si128((__m128i *)(dst + i + 48), v3);
    __m128i m01 = _mm_or_si128(sse2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                               sse2_esc_mask(v1, q_22, q_5c, q_1f, zero));
    __m128i m23 = _mm_or_si128(sse2_esc_mask(v2, q_22, q_5c, q_1f, zero),
                               sse2_esc_mask(v3, q_22, q_5c, q_1f, zero));
    m_acc = _mm_or_si128(m_acc, _mm_or_si128(m01, m23));
  }
  for (; i + 16 <= len; i += 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(src + i));
    _mm_storeu_si128((__m128i *)(dst + i), v);
    m_acc = _mm_or_si128(m_acc, sse2_esc_mask(v, q_22, q_5c, q_1f, zero));
  }
  if (i < len) {
    __m128i v = _mm_loadu_si128((const __m128i *)(src + len - 16));
    _mm_storeu_si128((__m128i *)(dst + len - 16), v);
    m_acc = _mm_or_si128(m_acc, sse2_esc_mask(v, q_22, q_5c, q_1f, zero));
  }
  return _mm_movemask_epi8(m_acc) == 0;
#else
  uint64_t acc = 0;
  Py_ssize_t i = 0;
  for (; i + 32 <= len; i += 32) {
    uint64_t a0, a1, a2, a3;
    memcpy(&a0, src + i, 8);
    memcpy(&a1, src + i + 8, 8);
    memcpy(&a2, src + i + 16, 8);
    memcpy(&a3, src + i + 24, 8);
    memcpy(dst + i, &a0, 8);
    memcpy(dst + i + 8, &a1, 8);
    memcpy(dst + i + 16, &a2, 8);
    memcpy(dst + i + 24, &a3, 8);
    acc |= swar_esc(a0) | swar_esc(a1) | swar_esc(a2) | swar_esc(a3);
  }
  for (; i + 8 <= len; i += 8) {
    uint64_t a;
    memcpy(&a, src + i, 8);
    memcpy(dst + i, &a, 8);
    acc |= swar_esc(a);
  }
  if (i < len) {
    uint64_t a;
    memcpy(&a, src + len - 8, 8);
    memcpy(dst + len - 8, &a, 8);
    acc |= swar_esc(a);
  }
  return acc == 0;
#endif
}

/* Runtime-dispatched long-string SIMD kernels (AVX2 / SSE2 / NEON / SWAR). */
typedef struct {
  char str[8];
  uint8_t len;
} CJsonEscapeEntry;

typedef Py_ssize_t (*cjson_copy_clean_prefix_fn_t)(char *dst, const char *src, Py_ssize_t len);
typedef const char *(*cjson_find_str_special_fn_t)(const char *p, const char *end);
typedef const unsigned char *(*cjson_escape_chunk_fn_t)(
    const unsigned char *p, const unsigned char *end,
    char **dst_io, char *cap_limit, const CJsonEscapeEntry *table);

extern cjson_copy_clean_prefix_fn_t cjson_copy_clean_prefix_long_fn;
extern cjson_find_str_special_fn_t cjson_find_str_special_long_fn;
extern cjson_escape_chunk_fn_t cjson_escape_chunk_fn;
extern int cjson_cpu_has_sse41;
void cjson_simd_init(void);

/* Copy clean prefix of src into dst and return clean byte count copied. */
static inline Py_ssize_t copy_clean_prefix(char *dst, const char *src, Py_ssize_t len) {
  if (len <= 64) {
    return copy_if_clean_short(dst, src, len) ? len : 0;
  }
  return cjson_copy_clean_prefix_long_fn(dst, src, len);
}

#endif /* CJSON_SIMD_H */
