#include "cjson_simd.h"

#if !defined(CJSON_NO_SIMD) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif
#endif

/* ========================================================================= */
/* Portable SWAR implementations (used for -DCJSON_NO_SIMD / non-SIMD CPUs)  */
/* ========================================================================= */

#if !defined(CJSON_SIMD_NEON) && (defined(CJSON_NO_SIMD) || (!defined(__x86_64__) && !defined(_M_X64)))

static Py_ssize_t copy_clean_prefix_swar(char *dst, const char *src, Py_ssize_t len) {
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
    uint64_t m = swar_esc(a0) | swar_esc(a1) | swar_esc(a2) | swar_esc(a3);
    if (CJSON_UNLIKELY(m != 0)) return i;
  }
  uint64_t m_rem = 0;
  Py_ssize_t rem_start = i;
  for (; i + 8 <= len; i += 8) {
    uint64_t a;
    memcpy(&a, src + i, 8);
    memcpy(dst + i, &a, 8);
    m_rem |= swar_esc(a);
  }
  if (i < len) {
    uint64_t a;
    memcpy(&a, src + len - 8, 8);
    memcpy(dst + len - 8, &a, 8);
    m_rem |= swar_esc(a);
  }
  if (CJSON_UNLIKELY(m_rem != 0)) return rem_start;
  return len;
}

static const char *find_str_special_swar(const char *p, const char *end) {
  while (p + 8 <= end) {
    uint64_t w;
    memcpy(&w, p, 8);
    uint64_t m = swar_str_ascii_mask(w);
    if (m != 0) {
      return p + (unsigned int)(__builtin_ctzll(m) >> 3);
    }
    p += 8;
  }
  return p;
}

static const unsigned char *escape_chunk_swar(
    const unsigned char *p, const unsigned char *end,
    char **dst_io, char *cap_limit, const CJsonEscapeEntry *table) {
  char *dst = *dst_io;
  while (p + 8 <= end && dst <= cap_limit) {
    uint64_t w;
    memcpy(&w, p, 8);
    memcpy(dst, &w, 8);
    uint64_t m = swar_esc(w);
    if (CJSON_LIKELY(m == 0)) {
      dst += 8;
      p += 8;
      while (p + 32 <= end && dst <= cap_limit) {
        uint64_t a0, a1, a2, a3;
        memcpy(&a0, p, 8);
        memcpy(&a1, p + 8, 8);
        memcpy(&a2, p + 16, 8);
        memcpy(&a3, p + 24, 8);
        memcpy(dst, &a0, 8);
        memcpy(dst + 8, &a1, 8);
        memcpy(dst + 16, &a2, 8);
        memcpy(dst + 24, &a3, 8);
        if (CJSON_UNLIKELY((swar_esc(a0) | swar_esc(a1) | swar_esc(a2) | swar_esc(a3)) != 0)) break;
        p += 32;
        dst += 32;
      }
    } else {
      unsigned int idx = (unsigned int)(__builtin_ctzll(m) >> 3);
      dst += idx;
      p += idx;
      const CJsonEscapeEntry *esc = &table[*p++];
      memcpy(dst, esc->str, 8);
      dst += esc->len;
      m &= m - 1ULL;
      if (CJSON_UNLIKELY(m != 0)) {
        unsigned int rem_k = 7 - idx;
        for (unsigned int k = 0; k < rem_k; k++) {
          uint8_t ch = *p++;
          const CJsonEscapeEntry *e2 = &table[ch];
          if (e2->len != 0) {
            memcpy(dst, e2->str, 8);
            dst += e2->len;
          } else {
            *dst++ = (char)ch;
          }
        }
      }
    }
  }
  *dst_io = dst;
  return p;
}

#endif

/* ========================================================================= */
/* ARM64 NEON implementations                                                */
/* ========================================================================= */

#if defined(CJSON_SIMD_NEON)

static Py_ssize_t copy_clean_prefix_neon(char *dst, const char *src, Py_ssize_t len) {
  Py_ssize_t i = 0;
  const uint8x16_t q_22 = vdupq_n_u8(0x22);
  const uint8x16_t q_5c = vdupq_n_u8(0x5C);
  const uint8x16_t q_20 = vdupq_n_u8(0x20);
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
    if (CJSON_UNLIKELY(!neon_is_zero(vorrq_u8(m01, m23)))) return i;
  }
  uint8x16_t m_rem = vdupq_n_u8(0);
  Py_ssize_t rem_start = i;
  for (; i + 16 <= len; i += 16) {
    uint8x16_t v = vld1q_u8((const uint8_t *)src + i);
    vst1q_u8((uint8_t *)dst + i, v);
    m_rem = vorrq_u8(m_rem, neon_esc_mask(v, q_22, q_5c, q_20));
  }
  if (i < len) {
    uint8x16_t v = vld1q_u8((const uint8_t *)src + len - 16);
    vst1q_u8((uint8_t *)dst + len - 16, v);
    m_rem = vorrq_u8(m_rem, neon_esc_mask(v, q_22, q_5c, q_20));
  }
  if (CJSON_UNLIKELY(!neon_is_zero(m_rem))) return rem_start;
  return len;
}

static const char *find_str_special_neon(const char *p, const char *end) {
  const uint8x16_t v_quote = vdupq_n_u8('"');
  const uint8x16_t v_bs = vdupq_n_u8('\\');
  const int8x16_t v_s20 = vdupq_n_s8(0x20);
  while (p + 64 <= end) {
    uint8x16_t v0 = vld1q_u8((const uint8_t *)p);
    uint8x16_t v1 = vld1q_u8((const uint8_t *)(p + 16));
    uint8x16_t v2 = vld1q_u8((const uint8_t *)(p + 32));
    uint8x16_t v3 = vld1q_u8((const uint8_t *)(p + 48));
    uint8x16_t s01 = vorrq_u8(neon_str_ascii_mask(v0, v_quote, v_bs, v_s20),
                              neon_str_ascii_mask(v1, v_quote, v_bs, v_s20));
    uint8x16_t s23 = vorrq_u8(neon_str_ascii_mask(v2, v_quote, v_bs, v_s20),
                              neon_str_ascii_mask(v3, v_quote, v_bs, v_s20));
    if (!neon_is_zero(vorrq_u8(s01, s23))) break;
    p += 64;
  }
  while (p + 16 <= end) {
    uint8x16_t v = vld1q_u8((const uint8_t *)p);
    uint8x16_t spec = neon_str_ascii_mask(v, v_quote, v_bs, v_s20);
    uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(spec), 4);
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
    if (mask != 0) {
      return p + (unsigned int)(__builtin_ctzll(mask) >> 2);
    }
    p += 16;
  }
  return p;
}

static const unsigned char *escape_chunk_neon(
    const unsigned char *p, const unsigned char *end,
    char **dst_io, char *cap_limit, const CJsonEscapeEntry *table) {
  char *dst = *dst_io;
  const uint8x16_t q_22 = vdupq_n_u8(0x22);
  const uint8x16_t q_5c = vdupq_n_u8(0x5C);
  const uint8x16_t q_20 = vdupq_n_u8(0x20);
  while (p + 16 <= end && dst <= cap_limit) {
    /* Zero-reload 64B-unrolled path: when an escape hits inside a 64B block,
     * extract its exact lane from m0..m3 already in registers without re-loading. */
    while (p + 64 <= end && dst <= cap_limit) {
      uint8x16_t v0 = vld1q_u8(p);
      uint8x16_t v1 = vld1q_u8(p + 16);
      uint8x16_t v2 = vld1q_u8(p + 32);
      uint8x16_t v3 = vld1q_u8(p + 48);
      vst1q_u8((uint8_t *)dst, v0);
      vst1q_u8((uint8_t *)dst + 16, v1);
      vst1q_u8((uint8_t *)dst + 32, v2);
      vst1q_u8((uint8_t *)dst + 48, v3);
      uint8x16_t m0 = neon_esc_mask(v0, q_22, q_5c, q_20);
      uint8x16_t m1 = neon_esc_mask(v1, q_22, q_5c, q_20);
      uint8x16_t m2 = neon_esc_mask(v2, q_22, q_5c, q_20);
      uint8x16_t m3 = neon_esc_mask(v3, q_22, q_5c, q_20);
      uint8x16_t m01 = vorrq_u8(m0, m1);
      uint8x16_t m23 = vorrq_u8(m2, m3);
      if (CJSON_LIKELY(neon_is_zero(vorrq_u8(m01, m23)))) {
        p += 64;
        dst += 64;
        continue;
      }
      uint8x16_t mhit;
      if (!neon_is_zero(m0)) {
        mhit = m0;
      } else if (!neon_is_zero(m1)) {
        p += 16; dst += 16; mhit = m1;
      } else if (!neon_is_zero(m2)) {
        p += 32; dst += 32; mhit = m2;
      } else {
        p += 48; dst += 48; mhit = m3;
      }
      uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(mhit), 4);
      uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
      unsigned int idx = (unsigned int)(__builtin_ctzll(mask) >> 2);
      dst += idx;
      p += idx;
      const CJsonEscapeEntry *esc = &table[*p++];
      memcpy(dst, esc->str, 8);
      dst += esc->len;
      mask &= ~(0xFULL << (idx * 4));
      if (CJSON_UNLIKELY(mask != 0)) {
        unsigned int rem_k = 15 - idx;
        for (unsigned int k = 0; k < rem_k; k++) {
          uint8_t ch = *p++;
          const CJsonEscapeEntry *e2 = &table[ch];
          if (e2->len != 0) {
            memcpy(dst, e2->str, 8);
            dst += e2->len;
          } else {
            *dst++ = (char)ch;
          }
        }
        break;
      }
    }
    if (p + 16 > end || dst > cap_limit) break;
    uint8x16_t v = vld1q_u8(p);
    vst1q_u8((uint8_t *)dst, v);
    uint8x16_t m = neon_esc_mask(v, q_22, q_5c, q_20);
    uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(m), 4);
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
    if (CJSON_LIKELY(mask == 0)) {
      dst += 16;
      p += 16;
    } else {
      unsigned int idx = (unsigned int)(__builtin_ctzll(mask) >> 2);
      dst += idx;
      p += idx;
      const CJsonEscapeEntry *esc = &table[*p++];
      memcpy(dst, esc->str, 8);
      dst += esc->len;
      mask &= ~(0xFULL << (idx * 4));
      if (CJSON_UNLIKELY(mask != 0)) {
        unsigned int rem_k = 15 - idx;
        for (unsigned int k = 0; k < rem_k; k++) {
          uint8_t ch = *p++;
          const CJsonEscapeEntry *e2 = &table[ch];
          if (e2->len != 0) {
            memcpy(dst, e2->str, 8);
            dst += e2->len;
          } else {
            *dst++ = (char)ch;
          }
        }
      }
    }
  }
  *dst_io = dst;
  return p;
}

#elif !defined(CJSON_NO_SIMD) && (defined(__x86_64__) || defined(_M_X64))

/* ========================================================================= */
/* x86_64 SSE2 + Runtime-Dispatched AVX2 implementations                     */
/* ========================================================================= */

static Py_ssize_t copy_clean_prefix_sse2(char *dst, const char *src, Py_ssize_t len) {
  Py_ssize_t i = 0;
  __m128i q_22 = _mm_set1_epi8(0x22);
  __m128i q_5c = _mm_set1_epi8(0x5C);
  __m128i q_1f = _mm_set1_epi8(0x1F);
  __m128i zero = _mm_setzero_si128();
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
    if (CJSON_UNLIKELY(_mm_movemask_epi8(_mm_or_si128(m01, m23)) != 0)) return i;
  }
  __m128i m_rem = zero;
  Py_ssize_t rem_start = i;
  for (; i + 16 <= len; i += 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)(src + i));
    _mm_storeu_si128((__m128i *)(dst + i), v);
    m_rem = _mm_or_si128(m_rem, sse2_esc_mask(v, q_22, q_5c, q_1f, zero));
  }
  if (i < len) {
    __m128i v = _mm_loadu_si128((const __m128i *)(src + len - 16));
    _mm_storeu_si128((__m128i *)(dst + len - 16), v);
    m_rem = _mm_or_si128(m_rem, sse2_esc_mask(v, q_22, q_5c, q_1f, zero));
  }
  if (CJSON_UNLIKELY(_mm_movemask_epi8(m_rem) != 0)) return rem_start;
  return len;
}

static const char *find_str_special_sse2(const char *p, const char *end) {
  const __m128i v_q = _mm_set1_epi8('"');
  const __m128i v_bs = _mm_set1_epi8('\\');
  const __m128i v_20 = _mm_set1_epi8(0x20);
  while (p + 64 <= end) {
    __m128i v0 = _mm_loadu_si128((const __m128i *)p);
    __m128i v1 = _mm_loadu_si128((const __m128i *)(p + 16));
    __m128i v2 = _mm_loadu_si128((const __m128i *)(p + 32));
    __m128i v3 = _mm_loadu_si128((const __m128i *)(p + 48));
    __m128i s01 = _mm_or_si128(sse2_str_ascii_mask(v0, v_q, v_bs, v_20),
                               sse2_str_ascii_mask(v1, v_q, v_bs, v_20));
    __m128i s23 = _mm_or_si128(sse2_str_ascii_mask(v2, v_q, v_bs, v_20),
                               sse2_str_ascii_mask(v3, v_q, v_bs, v_20));
    if (_mm_movemask_epi8(_mm_or_si128(s01, s23)) != 0) break;
    p += 64;
  }
  while (p + 16 <= end) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    uint32_t mask = (uint32_t)_mm_movemask_epi8(sse2_str_ascii_mask(v, v_q, v_bs, v_20));
    if (mask != 0) {
      return p + (unsigned int)__builtin_ctz(mask);
    }
    p += 16;
  }
  return p;
}

static const unsigned char *escape_chunk_sse2(
    const unsigned char *p, const unsigned char *end,
    char **dst_io, char *cap_limit, const CJsonEscapeEntry *table) {
  char *dst = *dst_io;
  __m128i q_22 = _mm_set1_epi8(0x22);
  __m128i q_5c = _mm_set1_epi8(0x5C);
  __m128i q_1f = _mm_set1_epi8(0x1F);
  __m128i zero = _mm_setzero_si128();
  while (p + 16 <= end && dst <= cap_limit) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    _mm_storeu_si128((__m128i *)dst, v);
    uint32_t mask = (uint32_t)_mm_movemask_epi8(sse2_esc_mask(v, q_22, q_5c, q_1f, zero));
    if (CJSON_LIKELY(mask == 0)) {
      dst += 16;
      p += 16;
      while (p + 64 <= end && dst <= cap_limit) {
        __m128i v0 = _mm_loadu_si128((const __m128i *)p);
        __m128i v1 = _mm_loadu_si128((const __m128i *)(p + 16));
        __m128i v2 = _mm_loadu_si128((const __m128i *)(p + 32));
        __m128i v3 = _mm_loadu_si128((const __m128i *)(p + 48));
        _mm_storeu_si128((__m128i *)dst, v0);
        _mm_storeu_si128((__m128i *)(dst + 16), v1);
        _mm_storeu_si128((__m128i *)(dst + 32), v2);
        _mm_storeu_si128((__m128i *)(dst + 48), v3);
        __m128i m01 = _mm_or_si128(sse2_esc_mask(v0, q_22, q_5c, q_1f, zero),
                                   sse2_esc_mask(v1, q_22, q_5c, q_1f, zero));
        __m128i m23 = _mm_or_si128(sse2_esc_mask(v2, q_22, q_5c, q_1f, zero),
                                   sse2_esc_mask(v3, q_22, q_5c, q_1f, zero));
        if (CJSON_UNLIKELY(_mm_movemask_epi8(_mm_or_si128(m01, m23)) != 0)) break;
        p += 64;
        dst += 64;
      }
    } else {
      unsigned int idx = (unsigned int)__builtin_ctz(mask);
      dst += idx;
      p += idx;
      const CJsonEscapeEntry *esc = &table[*p++];
      memcpy(dst, esc->str, 8);
      dst += esc->len;
      mask &= mask - 1u;
      if (CJSON_UNLIKELY(mask != 0)) {
        unsigned int rem_k = 15 - idx;
        for (unsigned int k = 0; k < rem_k; k++) {
          uint8_t ch = *p++;
          const CJsonEscapeEntry *e2 = &table[ch];
          if (e2->len != 0) {
            memcpy(dst, e2->str, 8);
            dst += e2->len;
          } else {
            *dst++ = (char)ch;
          }
        }
      }
    }
  }
  *dst_io = dst;
  return p;
}

#if defined(__AVX2__) || defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)

#if !defined(__AVX2__)
__attribute__((target("avx2")))
#endif
static Py_ssize_t copy_clean_prefix_avx2(char *dst, const char *src, Py_ssize_t len) {
  Py_ssize_t i = 0;
  __m256i q_22 = _mm256_set1_epi8(0x22);
  __m256i q_5c = _mm256_set1_epi8(0x5C);
  __m256i q_1f = _mm256_set1_epi8(0x1F);
  __m256i zero = _mm256_setzero_si256();
  for (; i + 128 <= len; i += 128) {
    __m256i v0 = _mm256_loadu_si256((const __m256i *)(src + i));
    __m256i v1 = _mm256_loadu_si256((const __m256i *)(src + i + 32));
    __m256i v2 = _mm256_loadu_si256((const __m256i *)(src + i + 64));
    __m256i v3 = _mm256_loadu_si256((const __m256i *)(src + i + 96));
    _mm256_storeu_si256((__m256i *)(dst + i), v0);
    _mm256_storeu_si256((__m256i *)(dst + i + 32), v1);
    _mm256_storeu_si256((__m256i *)(dst + i + 64), v2);
    _mm256_storeu_si256((__m256i *)(dst + i + 96), v3);
    __m256i m0 = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v0, q_22), _mm256_cmpeq_epi8(v0, q_5c)),
        _mm256_cmpeq_epi8(_mm256_subs_epu8(v0, q_1f), zero));
    __m256i m1 = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v1, q_22), _mm256_cmpeq_epi8(v1, q_5c)),
        _mm256_cmpeq_epi8(_mm256_subs_epu8(v1, q_1f), zero));
    __m256i m2 = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v2, q_22), _mm256_cmpeq_epi8(v2, q_5c)),
        _mm256_cmpeq_epi8(_mm256_subs_epu8(v2, q_1f), zero));
    __m256i m3 = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v3, q_22), _mm256_cmpeq_epi8(v3, q_5c)),
        _mm256_cmpeq_epi8(_mm256_subs_epu8(v3, q_1f), zero));
    __m256i mall = _mm256_or_si256(_mm256_or_si256(m0, m1), _mm256_or_si256(m2, m3));
    if (CJSON_UNLIKELY(_mm256_movemask_epi8(mall) != 0)) return i;
  }
  __m256i m_rem = zero;
  Py_ssize_t rem_start = i;
  for (; i + 32 <= len; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
    _mm256_storeu_si256((__m256i *)(dst + i), v);
    __m256i mv = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, q_22), _mm256_cmpeq_epi8(v, q_5c)),
        _mm256_cmpeq_epi8(_mm256_subs_epu8(v, q_1f), zero));
    m_rem = _mm256_or_si256(m_rem, mv);
  }
  if (i < len) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(src + len - 32));
    _mm256_storeu_si256((__m256i *)(dst + len - 32), v);
    __m256i mv = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, q_22), _mm256_cmpeq_epi8(v, q_5c)),
        _mm256_cmpeq_epi8(_mm256_subs_epu8(v, q_1f), zero));
    m_rem = _mm256_or_si256(m_rem, mv);
  }
  if (CJSON_UNLIKELY(_mm256_movemask_epi8(m_rem) != 0)) return rem_start;
  return len;
}

#if !defined(__AVX2__)
__attribute__((target("avx2")))
#endif
static const char *find_str_special_avx2(const char *p, const char *end) {
  const __m256i v_q = _mm256_set1_epi8('"');
  const __m256i v_bs = _mm256_set1_epi8('\\');
  const __m256i v_20 = _mm256_set1_epi8(0x20);
  while (p + 64 <= end) {
    __m256i v0 = _mm256_loadu_si256((const __m256i *)p);
    __m256i v1 = _mm256_loadu_si256((const __m256i *)(p + 32));
    __m256i s0 = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v0, v_q), _mm256_cmpeq_epi8(v0, v_bs)),
        _mm256_cmpgt_epi8(v_20, v0));
    __m256i s1 = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v1, v_q), _mm256_cmpeq_epi8(v1, v_bs)),
        _mm256_cmpgt_epi8(v_20, v1));
    if (_mm256_movemask_epi8(_mm256_or_si256(s0, s1)) != 0) break;
    p += 64;
  }
  while (p + 32 <= end) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    __m256i s = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, v_q), _mm256_cmpeq_epi8(v, v_bs)),
        _mm256_cmpgt_epi8(v_20, v));
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(s);
    if (mask != 0) {
      return p + (unsigned int)__builtin_ctz(mask);
    }
    p += 32;
  }
  return p;
}

#if !defined(__AVX2__)
__attribute__((target("avx2")))
#endif
static const unsigned char *escape_chunk_avx2(
    const unsigned char *p, const unsigned char *end,
    char **dst_io, char *cap_limit, const CJsonEscapeEntry *table) {
  char *dst = *dst_io;
  __m256i q_22 = _mm256_set1_epi8(0x22);
  __m256i q_5c = _mm256_set1_epi8(0x5C);
  __m256i q_1f = _mm256_set1_epi8(0x1F);
  __m256i zero = _mm256_setzero_si256();
  while (p + 32 <= end && dst <= cap_limit) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    _mm256_storeu_si256((__m256i *)dst, v);
    __m256i mv = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, q_22), _mm256_cmpeq_epi8(v, q_5c)),
        _mm256_cmpeq_epi8(_mm256_subs_epu8(v, q_1f), zero));
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(mv);
    if (CJSON_LIKELY(mask == 0)) {
      dst += 32;
      p += 32;
      while (p + 64 <= end && dst <= cap_limit) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)p);
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(p + 32));
        _mm256_storeu_si256((__m256i *)dst, v0);
        _mm256_storeu_si256((__m256i *)(dst + 32), v1);
        __m256i m0 = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v0, q_22), _mm256_cmpeq_epi8(v0, q_5c)),
            _mm256_cmpeq_epi8(_mm256_subs_epu8(v0, q_1f), zero));
        __m256i m1 = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v1, q_22), _mm256_cmpeq_epi8(v1, q_5c)),
            _mm256_cmpeq_epi8(_mm256_subs_epu8(v1, q_1f), zero));
        if (CJSON_UNLIKELY(_mm256_movemask_epi8(_mm256_or_si256(m0, m1)) != 0)) break;
        p += 64;
        dst += 64;
      }
    } else {
      unsigned int idx = (unsigned int)__builtin_ctz(mask);
      dst += idx;
      const CJsonEscapeEntry *esc = &table[p[idx]];
      memcpy(dst, esc->str, 8);
      dst += esc->len;
      unsigned int prev = idx + 1;
      mask &= mask - 1u;
      while (CJSON_UNLIKELY(mask != 0)) {
        idx = (unsigned int)__builtin_ctz(mask);
        unsigned int span = idx - prev;
        if (span > 0) {
          memcpy(dst, p + prev, span);
          dst += span;
        }
        esc = &table[p[idx]];
        memcpy(dst, esc->str, 8);
        dst += esc->len;
        prev = idx + 1;
        mask &= mask - 1u;
      }
      unsigned int tail = 32 - prev;
      if (tail > 0) {
        memcpy(dst, p + prev, tail);
        dst += tail;
      }
      p += 32;
    }
  }
  *dst_io = dst;
  return p;
}

static int cpu_supports_avx2(void) {
#if defined(__AVX2__)
  return 1;
#elif defined(_MSC_VER)
  int info[4] = {0};
  __cpuid(info, 1);
  if ((info[2] & ((1 << 27) | (1 << 28))) != ((1 << 27) | (1 << 28))) return 0;
  if ((_xgetbv(0) & 0x6ULL) != 0x6ULL) return 0;
  __cpuidex(info, 7, 0);
  return (info[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;
  /* Check OSXSAVE (bit 27) and AVX (bit 28) */
  if ((ecx & ((1u << 27) | (1u << 28))) != ((1u << 27) | (1u << 28))) return 0;
  /* Check OS enabled XMM + YMM state via XGETBV(0) */
  unsigned int xcr0_lo = 0, xcr0_hi = 0;
  __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
  if ((xcr0_lo & 0x6u) != 0x6u) return 0;
  /* Check AVX2 (leaf 7, subleaf 0, EBX bit 5) */
  if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return 0;
  return (ebx & (1u << 5)) != 0;
#else
  return 0;
#endif
}

#endif /* AVX2 capable compiler */

#if defined(__AVX512BW__) || defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)

#define AVX512_ESC_MASK(v, q22, q5c, q20) \
  ((uint64_t)(_mm512_cmpeq_epi8_mask((v), (q22)) | \
              _mm512_cmpeq_epi8_mask((v), (q5c)) | \
              _mm512_cmplt_epu8_mask((v), (q20))))

#define AVX512_STR_ASCII_MASK(v, q22, q5c, q20) \
  ((uint64_t)(_mm512_cmpeq_epi8_mask((v), (q22)) | \
              _mm512_cmpeq_epi8_mask((v), (q5c)) | \
              _mm512_cmplt_epi8_mask((v), (q20))))

#if !defined(__AVX512BW__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx512f,avx512bw")))
#endif
static Py_ssize_t copy_clean_prefix_avx512(char *dst, const char *src, Py_ssize_t len) {
  Py_ssize_t i = 0;
  __m512i q_22 = _mm512_set1_epi8(0x22);
  __m512i q_5c = _mm512_set1_epi8(0x5C);
  __m512i q_20 = _mm512_set1_epi8(0x20);
  for (; i + 256 <= len; i += 256) {
    __m512i v0 = _mm512_loadu_si512((const void *)(src + i));
    __m512i v1 = _mm512_loadu_si512((const void *)(src + i + 64));
    __m512i v2 = _mm512_loadu_si512((const void *)(src + i + 128));
    __m512i v3 = _mm512_loadu_si512((const void *)(src + i + 192));
    _mm512_storeu_si512((void *)(dst + i), v0);
    _mm512_storeu_si512((void *)(dst + i + 64), v1);
    _mm512_storeu_si512((void *)(dst + i + 128), v2);
    _mm512_storeu_si512((void *)(dst + i + 192), v3);
    uint64_t m = AVX512_ESC_MASK(v0, q_22, q_5c, q_20) |
                 AVX512_ESC_MASK(v1, q_22, q_5c, q_20) |
                 AVX512_ESC_MASK(v2, q_22, q_5c, q_20) |
                 AVX512_ESC_MASK(v3, q_22, q_5c, q_20);
    if (CJSON_UNLIKELY(m != 0)) return i;
  }
  for (; i + 64 <= len; i += 64) {
    __m512i v = _mm512_loadu_si512((const void *)(src + i));
    _mm512_storeu_si512((void *)(dst + i), v);
    if (CJSON_UNLIKELY(AVX512_ESC_MASK(v, q_22, q_5c, q_20) != 0)) return i;
  }
  if (i < len && len >= 64) {
    __m512i v = _mm512_loadu_si512((const void *)(src + len - 64));
    _mm512_storeu_si512((void *)(dst + len - 64), v);
    if (CJSON_LIKELY(AVX512_ESC_MASK(v, q_22, q_5c, q_20) == 0)) return len;
  }
  return i;
}

#if !defined(__AVX512BW__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx512f,avx512bw")))
#endif
static const char *find_str_special_avx512(const char *p, const char *end) {
  __m512i v_q = _mm512_set1_epi8('"');
  __m512i v_bs = _mm512_set1_epi8('\\');
  __m512i v_20 = _mm512_set1_epi8(0x20);
  while (p + 256 <= end) {
    __m512i v0 = _mm512_loadu_si512((const void *)p);
    __m512i v1 = _mm512_loadu_si512((const void *)(p + 64));
    __m512i v2 = _mm512_loadu_si512((const void *)(p + 128));
    __m512i v3 = _mm512_loadu_si512((const void *)(p + 192));
    uint64_t m = AVX512_STR_ASCII_MASK(v0, v_q, v_bs, v_20) |
                 AVX512_STR_ASCII_MASK(v1, v_q, v_bs, v_20) |
                 AVX512_STR_ASCII_MASK(v2, v_q, v_bs, v_20) |
                 AVX512_STR_ASCII_MASK(v3, v_q, v_bs, v_20);
    if (m != 0) break;
    p += 256;
  }
  while (p + 64 <= end) {
    __m512i v = _mm512_loadu_si512((const void *)p);
    uint64_t m = AVX512_STR_ASCII_MASK(v, v_q, v_bs, v_20);
    if (m != 0) {
      return p + (unsigned int)__builtin_ctzll(m);
    }
    p += 64;
  }
  return p;
}

#if !defined(__AVX512BW__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx512f,avx512bw")))
#endif
static const unsigned char *escape_chunk_avx512(
    const unsigned char *p, const unsigned char *end,
    char **dst_io, char *cap_limit, const CJsonEscapeEntry *table) {
  char *dst = *dst_io;
  __m512i q_22 = _mm512_set1_epi8(0x22);
  __m512i q_5c = _mm512_set1_epi8(0x5C);
  __m512i q_20 = _mm512_set1_epi8(0x20);
  while (p + 64 <= end && dst <= cap_limit) {
    __m512i v = _mm512_loadu_si512((const void *)p);
    _mm512_storeu_si512((void *)dst, v);
    uint64_t mask = AVX512_ESC_MASK(v, q_22, q_5c, q_20);
    if (CJSON_LIKELY(mask == 0)) {
      dst += 64;
      p += 64;
      while (p + 256 <= end && dst + 256 <= cap_limit) {
        __m512i v0 = _mm512_loadu_si512((const void *)p);
        __m512i v1 = _mm512_loadu_si512((const void *)(p + 64));
        __m512i v2 = _mm512_loadu_si512((const void *)(p + 128));
        __m512i v3 = _mm512_loadu_si512((const void *)(p + 192));
        _mm512_storeu_si512((void *)dst, v0);
        _mm512_storeu_si512((void *)(dst + 64), v1);
        _mm512_storeu_si512((void *)(dst + 128), v2);
        _mm512_storeu_si512((void *)(dst + 192), v3);
        uint64_t m = AVX512_ESC_MASK(v0, q_22, q_5c, q_20) |
                     AVX512_ESC_MASK(v1, q_22, q_5c, q_20) |
                     AVX512_ESC_MASK(v2, q_22, q_5c, q_20) |
                     AVX512_ESC_MASK(v3, q_22, q_5c, q_20);
        if (CJSON_UNLIKELY(m != 0)) break;
        p += 256;
        dst += 256;
      }
    } else {
      unsigned int idx = (unsigned int)__builtin_ctzll(mask);
      dst += idx;
      const CJsonEscapeEntry *esc = &table[p[idx]];
      memcpy(dst, esc->str, 8);
      dst += esc->len;
      unsigned int prev = idx + 1;
      mask &= mask - 1ULL;
      while (CJSON_UNLIKELY(mask != 0)) {
        idx = (unsigned int)__builtin_ctzll(mask);
        unsigned int span = idx - prev;
        if (span > 0) {
          memcpy(dst, p + prev, span);
          dst += span;
        }
        esc = &table[p[idx]];
        memcpy(dst, esc->str, 8);
        dst += esc->len;
        prev = idx + 1;
        mask &= mask - 1ULL;
      }
      unsigned int tail = 64 - prev;
      if (tail > 0) {
        memcpy(dst, p + prev, tail);
        dst += tail;
      }
      p += 64;
    }
  }
  *dst_io = dst;
  return p;
}

static int cpu_supports_avx512bw(void) {
#if defined(__AVX512BW__)
  return 1;
#elif defined(_MSC_VER)
  int info[4] = {0};
  __cpuid(info, 1);
  if ((info[2] & ((1 << 27) | (1 << 28))) != ((1 << 27) | (1 << 28))) return 0;
  /* Check OS enabled XMM + YMM + Opmask + ZMM_Hi256 + Hi16_ZMM (0xE6) */
  if ((_xgetbv(0) & 0xE6ULL) != 0xE6ULL) return 0;
  __cpuidex(info, 7, 0);
  /* Check AVX512F (EBX bit 16) and AVX512BW (EBX bit 30) */
  return (info[1] & ((1 << 16) | (1 << 30))) == ((1 << 16) | (1 << 30));
#elif defined(__GNUC__) || defined(__clang__)
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;
  if ((ecx & ((1u << 27) | (1u << 28))) != ((1u << 27) | (1u << 28))) return 0;
  unsigned int xcr0_lo = 0, xcr0_hi = 0;
  __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
  if ((xcr0_lo & 0xE6u) != 0xE6u) return 0;
  if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return 0;
  return (ebx & ((1u << 16) | (1u << 30))) == ((1u << 16) | (1u << 30));
#else
  return 0;
#endif
}

#endif /* AVX-512BW capable compiler */

#endif /* arch selection */

/* ========================================================================= */
/* Global Runtime Dispatch Function Pointers                                 */
/* ========================================================================= */

#if defined(CJSON_SIMD_NEON)
cjson_copy_clean_prefix_fn_t cjson_copy_clean_prefix_long_fn = copy_clean_prefix_neon;
cjson_find_str_special_fn_t cjson_find_str_special_long_fn = find_str_special_neon;
cjson_escape_chunk_fn_t cjson_escape_chunk_fn = escape_chunk_neon;
#elif !defined(CJSON_NO_SIMD) && (defined(__x86_64__) || defined(_M_X64))
cjson_copy_clean_prefix_fn_t cjson_copy_clean_prefix_long_fn = copy_clean_prefix_sse2;
cjson_find_str_special_fn_t cjson_find_str_special_long_fn = find_str_special_sse2;
cjson_escape_chunk_fn_t cjson_escape_chunk_fn = escape_chunk_sse2;
#else
cjson_copy_clean_prefix_fn_t cjson_copy_clean_prefix_long_fn = copy_clean_prefix_swar;
cjson_find_str_special_fn_t cjson_find_str_special_long_fn = find_str_special_swar;
cjson_escape_chunk_fn_t cjson_escape_chunk_fn = escape_chunk_swar;
#endif

int cjson_cpu_has_sse41 = 0;
int cjson_cpu_has_avx2 = 0;

#if !defined(CJSON_NO_SIMD) && defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
__attribute__((target("avx2")))
char *cjson_encode_str_array_avx2(PyObject **items, Py_ssize_t len, Py_ssize_t *i_io, char *dst, char *safe_end) {
  const __m256i q_22 = _mm256_set1_epi8(0x22);
  const __m256i q_5c = _mm256_set1_epi8(0x5C);
  const __m256i q_1f = _mm256_set1_epi8(0x1F);
  const __m256i zero256 = _mm256_setzero_si256();
  const __m128i q_22_128 = _mm_set1_epi8(0x22);
  const __m128i q_5c_128 = _mm_set1_epi8(0x5C);
  const __m128i q_1f_128 = _mm_set1_epi8(0x1F);
  const __m128i zero128 = _mm_setzero_si128();

  Py_ssize_t i = *i_io;
  for (; i < len && dst <= safe_end; i++) {
    PyObject *item = items[i];
    if (CJSON_UNLIKELY(Py_TYPE(item) != &PyUnicode_Type || !PyUnicode_IS_COMPACT_ASCII(item))) {
      break;
    }
    Py_ssize_t slen = PyUnicode_GET_LENGTH(item);
    const char *src = (const char *)PyUnicode_1BYTE_DATA(item);
    if (CJSON_UNLIKELY(slen == 0)) {
      dst[0] = '"';
      dst[1] = '"';
      dst[2] = ',';
      dst += 3;
      continue;
    }
    if (slen < 16) {
      if (slen >= 8) {
        uint64_t a, b;
        memcpy(&a, src, 8);
        memcpy(&b, src + slen - 8, 8);
        if (CJSON_UNLIKELY((swar_esc(a) | swar_esc(b)) != 0)) break;
        dst[0] = '"';
        memcpy(dst + 1, &a, 8);
        memcpy(dst + 1 + slen - 8, &b, 8);
        dst[slen + 1] = '"';
        dst[slen + 2] = ',';
        dst += slen + 3;
      } else if (slen >= 4) {
        uint32_t a, b;
        memcpy(&a, src, 4);
        memcpy(&b, src + slen - 4, 4);
        if (CJSON_UNLIKELY(swar_esc((uint64_t)a | ((uint64_t)b << 32)) != 0)) break;
        dst[0] = '"';
        memcpy(dst + 1, &a, 4);
        memcpy(dst + 1 + slen - 4, &b, 4);
        dst[slen + 1] = '"';
        dst[slen + 2] = ',';
        dst += slen + 3;
      } else {
        if (!copy_if_clean_short(dst + 1, src, slen)) break;
        dst[0] = '"';
        dst[slen + 1] = '"';
        dst[slen + 2] = ',';
        dst += slen + 3;
      }
    } else if (slen <= 32) {
      __m128i v0 = _mm_loadu_si128((const __m128i *)src);
      __m128i v1 = _mm_loadu_si128((const __m128i *)(src + slen - 16));
      __m128i m0 = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v0, q_22_128), _mm_cmpeq_epi8(v0, q_5c_128)),
                                _mm_cmpeq_epi8(_mm_subs_epu8(v0, q_1f_128), zero128));
      __m128i m1 = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v1, q_22_128), _mm_cmpeq_epi8(v1, q_5c_128)),
                                _mm_cmpeq_epi8(_mm_subs_epu8(v1, q_1f_128), zero128));
      if (CJSON_UNLIKELY(_mm_movemask_epi8(_mm_or_si128(m0, m1)) != 0)) break;
      dst[0] = '"';
      _mm_storeu_si128((__m128i *)(dst + 1), v0);
      _mm_storeu_si128((__m128i *)(dst + 1 + slen - 16), v1);
      dst[slen + 1] = '"';
      dst[slen + 2] = ',';
      dst += slen + 3;
    } else if (slen <= 64) {
      __m256i v0 = _mm256_loadu_si256((const __m256i *)src);
      __m256i v1 = _mm256_loadu_si256((const __m256i *)(src + slen - 32));
      __m256i m0 = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v0, q_22), _mm256_cmpeq_epi8(v0, q_5c)),
                                   _mm256_cmpeq_epi8(_mm256_subs_epu8(v0, q_1f), zero256));
      __m256i m1 = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v1, q_22), _mm256_cmpeq_epi8(v1, q_5c)),
                                   _mm256_cmpeq_epi8(_mm256_subs_epu8(v1, q_1f), zero256));
      if (CJSON_UNLIKELY(_mm256_movemask_epi8(_mm256_or_si256(m0, m1)) != 0)) break;
      dst[0] = '"';
      _mm256_storeu_si256((__m256i *)(dst + 1), v0);
      _mm256_storeu_si256((__m256i *)(dst + 1 + slen - 32), v1);
      dst[slen + 1] = '"';
      dst[slen + 2] = ',';
      dst += slen + 3;
    } else {
      break;
    }
  }
  *i_io = i;
  return dst;
}
#endif

static int cpu_supports_sse41(void) {
#if defined(__SSE4_1__)
  return 1;
#elif defined(_MSC_VER) && defined(_M_X64)
  int info[4] = {0};
  __cpuid(info, 1);
  return (info[2] & (1 << 19)) != 0;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;
  return (ecx & (1u << 19)) != 0;
#else
  return 0;
#endif
}

void cjson_simd_init(void) {
  cjson_cpu_has_sse41 = cpu_supports_sse41();
#if !defined(CJSON_NO_SIMD) && (defined(__x86_64__) || defined(_M_X64)) && \
    (defined(__AVX512BW__) || defined(__AVX2__) || defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER))
  if (cpu_supports_avx512bw()) {
    cjson_cpu_has_avx2 = 1;
    /* Use 64B AVX-512BW for read-only scanning, and 32B AVX2 for serialization
     * stores to avoid ZMM turbo-frequency downclocking on cloud Xeon CPUs. */
    cjson_copy_clean_prefix_long_fn = copy_clean_prefix_avx2;
    cjson_find_str_special_long_fn = find_str_special_avx512;
    cjson_escape_chunk_fn = escape_chunk_avx2;
  } else if (cpu_supports_avx2()) {
    cjson_cpu_has_avx2 = 1;
    cjson_copy_clean_prefix_long_fn = copy_clean_prefix_avx2;
    cjson_find_str_special_long_fn = find_str_special_avx2;
    cjson_escape_chunk_fn = escape_chunk_avx2;
  } else {
    cjson_cpu_has_avx2 = 0;
    cjson_copy_clean_prefix_long_fn = copy_clean_prefix_sse2;
    cjson_find_str_special_long_fn = find_str_special_sse2;
    cjson_escape_chunk_fn = escape_chunk_sse2;
  }
#endif
}
