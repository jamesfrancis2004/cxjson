#define PY_SSIZE_T_CLEAN
#include "parser_state.h"
#include "utf8.h"
#include "key_cache.h"
#include "numparse.h"

#if PY_VERSION_HEX >= 0x030D0000 && !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)
/* Exported by libpython (declared in internal/pycore_dict.h since 3.13) */
PyAPI_FUNC(int) _PyDict_SetItem_KnownHash(PyObject *mp, PyObject *key, PyObject *item, Py_hash_t hash);
#endif

PyObject *JSONDecodeError = NULL;

#if defined(CJSON_SPLIT_DICT_FAST)
static int g_split_dict_enabled = 1;
#endif

#define CJSON_REC_PARSE_DEPTH 64

/* Tiny inline fast-path + noinline GPR SWAR whitespace skipper */
__attribute__((noinline))
static const char *skip_ws_slow(const char *p, const char *end) {
  for (;;) {
    if (p < end && *p == '\n') p++;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    while (p + 8 <= end) {
      uint64_t w;
      memcpy(&w, p, 8);
      uint64_t diff = w ^ 0x2020202020202020ULL;
      if (diff == 0) {
        p += 8;
        continue;
      }
      p += (unsigned int)(__builtin_ctzll(diff) >> 3);
      if (CJSON_LIKELY((uint8_t)*p > 0x20)) return p;
      break;
    }
#endif
    if (p >= end) return p;
    uint8_t c = (uint8_t)*p;
    if (c > 0x20) return p;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      p++;
    } else {
      return p;
    }
  }
}

static inline const char *skip_ws(const char *p, const char *end) {
  if (CJSON_LIKELY(p < end && (uint8_t)*p > 0x20)) return p;
  return skip_ws_slow(p, end);
}

static const int8_t HEX_LUT[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,
    9,  -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1};

static inline int parse_hex4(const char *p) {
  int h0 = HEX_LUT[(uint8_t)p[0]];
  int h1 = HEX_LUT[(uint8_t)p[1]];
  int h2 = HEX_LUT[(uint8_t)p[2]];
  int h3 = HEX_LUT[(uint8_t)p[3]];
  if ((h0 | h1 | h2 | h3) < 0)
    return -1;
  return (h0 << 12) | (h1 << 8) | (h2 << 4) | h3;
}

/* Cold path: unescape string after finding backslash at `start + clean_len` */
static const uint8_t ESC_LUT[256] = {
    ['"'] = '"',  ['\\'] = '\\', ['/'] = '/',  ['b'] = '\b',
    ['f'] = '\f', ['n'] = '\n',  ['r'] = '\r', ['t'] = '\t'};

/* Direct \uXXXX unescaper writing straight to UCS2/UCS4 (zero UTF-8
 * encode+decode round-trip) */
__attribute__((noinline)) static PyObject *
parse_string_escaped_u16(JsonParser *jp, const char **p_io, const char *p,
                         const char *end, const char *ascii_prefix,
                         size_t prefix_len, uint32_t first_cp) {
  uint16_t stack_u16[512];
  uint32_t stack_u32[512];
  uint16_t *u16 = stack_u16;
  uint32_t *u32 = NULL;
  size_t cap = 512;
  size_t nchars = 0;
  uint32_t max_cp = first_cp;

  if (first_cp > 0xFFFF) {
    u32 = stack_u32;
    u16 = NULL;
    if (CJSON_UNLIKELY(prefix_len + 8 > cap)) {
      while (cap < prefix_len + 256)
        cap *= 2;
      u32 = (uint32_t *)PyMem_Malloc(cap * sizeof(uint32_t));
      if (!u32) {
        PyErr_NoMemory();
        return NULL;
      }
    }
    for (size_t i = 0; i < prefix_len; i++) {
      u32[i] = (uint8_t)ascii_prefix[i];
    }
    u32[prefix_len] = first_cp;
    nchars = prefix_len + 1;
  } else {
    if (CJSON_UNLIKELY(prefix_len + 8 > cap)) {
      while (cap < prefix_len + 256)
        cap *= 2;
      u16 = (uint16_t *)PyMem_Malloc(cap * sizeof(uint16_t));
      if (!u16) {
        PyErr_NoMemory();
        return NULL;
      }
    }
    for (size_t i = 0; i < prefix_len; i++) {
      u16[i] = (uint8_t)ascii_prefix[i];
    }
    u16[prefix_len] = (uint16_t)first_cp;
    nchars = prefix_len + 1;
  }

#define U16_ENSURE(need)                                                       \
  do {                                                                         \
    if (CJSON_UNLIKELY(nchars + (need) > cap)) {                               \
      size_t new_cap = cap * 2;                                                \
      while (new_cap < nchars + (need) + 128)                                  \
        new_cap *= 2;                                                          \
      if (u32 != NULL) {                                                       \
        uint32_t *nb;                                                          \
        if (u32 == stack_u32) {                                                \
          nb = (uint32_t *)PyMem_Malloc(new_cap * sizeof(uint32_t));           \
          if (nb)                                                              \
            memcpy(nb, stack_u32, nchars * sizeof(uint32_t));                  \
        } else {                                                               \
          nb = (uint32_t *)PyMem_Realloc(u32, new_cap * sizeof(uint32_t));     \
        }                                                                      \
        if (!nb) {                                                             \
          PyErr_NoMemory();                                                    \
          goto fail_mem;                                                       \
        }                                                                      \
        u32 = nb;                                                              \
      } else {                                                                 \
        uint16_t *nb;                                                          \
        if (u16 == stack_u16) {                                                \
          nb = (uint16_t *)PyMem_Malloc(new_cap * sizeof(uint16_t));           \
          if (nb)                                                              \
            memcpy(nb, stack_u16, nchars * sizeof(uint16_t));                  \
        } else {                                                               \
          nb = (uint16_t *)PyMem_Realloc(u16, new_cap * sizeof(uint16_t));     \
        }                                                                      \
        if (!nb) {                                                             \
          PyErr_NoMemory();                                                    \
          goto fail_mem;                                                       \
        }                                                                      \
        u16 = nb;                                                              \
      }                                                                        \
      cap = new_cap;                                                           \
    }                                                                          \
  } while (0)

#define U16_ESCALATE_U32()                                                     \
  do {                                                                         \
    if (cap <= 512) {                                                          \
      u32 = stack_u32;                                                         \
    } else {                                                                   \
      u32 = (uint32_t *)PyMem_Malloc(cap * sizeof(uint32_t));                  \
      if (!u32) {                                                              \
        PyErr_NoMemory();                                                      \
        goto fail_mem;                                                         \
      }                                                                        \
    }                                                                          \
    for (size_t ui = 0; ui < nchars; ui++) {                                   \
      u32[ui] = u16[ui];                                                       \
    }                                                                          \
    if (u16 != stack_u16) {                                                    \
      PyMem_Free(u16);                                                         \
    }                                                                          \
    u16 = NULL;                                                                \
  } while (0)

#define U16_APPEND_CP(cpval)                                                   \
  do {                                                                         \
    uint32_t _v = (uint32_t)(cpval);                                           \
    U16_ENSURE(1);                                                             \
    if (_v > max_cp)                                                           \
      max_cp = _v;                                                             \
    if (CJSON_UNLIKELY(_v > 0xFFFF && u32 == NULL)) {                          \
      U16_ESCALATE_U32();                                                      \
    }                                                                          \
    if (u32 != NULL) {                                                         \
      u32[nchars++] = _v;                                                      \
    } else {                                                                   \
      u16[nchars++] = (uint16_t)_v;                                            \
    }                                                                          \
  } while (0)

  /* Consume consecutive \uXXXX escapes immediately without loop dispatch */
  while (p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
    p += 2;
    int cp = parse_hex4(p);
    if (CJSON_UNLIKELY(cp < 0)) {
      raise_decode_error(jp, "invalid \\uXXXX escape", p - 2);
      goto fail_mem;
    }
    p += 4;
    if (CJSON_UNLIKELY(cp >= 0xD800 && cp <= 0xDFFF)) {
      if (cp >= 0xDC00) {
        raise_decode_error(jp, "invalid high surrogate in string", p - 6);
        goto fail_mem;
      }
      if (p + 6 > end || p[0] != '\\' || p[1] != 'u') {
        raise_decode_error(jp, "no low surrogate in string", p - 6);
        goto fail_mem;
      }
      int lo = parse_hex4(p + 2);
      if (lo < 0xDC00 || lo > 0xDFFF) {
        raise_decode_error(jp, "invalid low surrogate in string", p);
        goto fail_mem;
      }
      p += 6;
      cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
    }
    U16_APPEND_CP(cp);
  }

  while (p < end) {
    uint8_t c = (uint8_t)*p;
    if (c == '\\') {
      p++;
      if (CJSON_UNLIKELY(p >= end)) {
        raise_decode_error(jp, "unexpected end of data", p);
        goto fail_mem;
      }
      uint8_t esc = (uint8_t)*p++;
      uint8_t mapped = ESC_LUT[esc];
      if (CJSON_LIKELY(mapped != 0)) {
        U16_APPEND_CP(mapped);
        continue;
      }
      if (esc == 'u') {
        while (1) {
          if (p + 4 > end) {
            raise_decode_error(jp, "unexpected end of hex escape", p);
            goto fail_mem;
          }
          int cp = parse_hex4(p);
          if (CJSON_UNLIKELY(cp < 0)) {
            raise_decode_error(jp, "invalid \\uXXXX escape", p - 2);
            goto fail_mem;
          }
          p += 4;
          if (CJSON_UNLIKELY(cp >= 0xD800 && cp <= 0xDFFF)) {
            if (cp >= 0xDC00) {
              raise_decode_error(jp, "invalid high surrogate in string", p - 6);
              goto fail_mem;
            }
            if (p + 6 > end || p[0] != '\\' || p[1] != 'u') {
              raise_decode_error(jp, "no low surrogate in string", p - 6);
              goto fail_mem;
            }
            int lo = parse_hex4(p + 2);
            if (lo < 0xDC00 || lo > 0xDFFF) {
              raise_decode_error(jp, "invalid low surrogate in string", p);
              goto fail_mem;
            }
            p += 6;
            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
          }
          U16_APPEND_CP(cp);
          if (p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            continue;
          }
          break;
        }
        continue;
      }
      raise_decode_error(jp, "invalid escape sequence", p - 2);
      goto fail_mem;
    }

    if (c == '"') {
      *p_io = p + 1;
      PyObject *u = PyUnicode_New((Py_ssize_t)nchars, (Py_UCS4)max_cp);
      if (CJSON_LIKELY(u != NULL)) {
        if (u32 != NULL) {
          memcpy(PyUnicode_4BYTE_DATA(u), u32, nchars * sizeof(Py_UCS4));
        } else if (max_cp <= 0xFF) {
          Py_UCS1 *dst = PyUnicode_1BYTE_DATA(u);
          for (size_t i = 0; i < nchars; i++) {
            dst[i] = (Py_UCS1)u16[i];
          }
        } else {
          memcpy(PyUnicode_2BYTE_DATA(u), u16, nchars * sizeof(Py_UCS2));
        }
      }
      if (u16 != NULL && u16 != stack_u16)
        PyMem_Free(u16);
      if (u32 != NULL && u32 != stack_u32)
        PyMem_Free(u32);
      return u;
    }

    if (CJSON_UNLIKELY(c < 0x20)) {
      raise_decode_error(jp, "invalid control character in string", p);
      goto fail_mem;
    }

    if (c < 0x80) {
      U16_ENSURE(1);
      if (u32 != NULL) {
        u32[nchars++] = (uint32_t)c;
      } else {
        u16[nchars++] = (uint16_t)c;
      }
      p++;
      while (p < end) {
        uint8_t nc = (uint8_t)*p;
        if (nc < 0x20 || nc >= 0x80 || nc == '"' || nc == '\\')
          break;
        U16_ENSURE(1);
        if (u32 != NULL) {
          u32[nchars++] = (uint32_t)nc;
        } else {
          u16[nchars++] = (uint16_t)nc;
        }
        p++;
      }
      continue;
    }

    /* Raw UTF-8 multi-byte sequence inside an escaped string */
    if (c >= 0xC2 && c <= 0xDF) {
      if (CJSON_UNLIKELY(p + 2 > end || ((uint8_t)p[1] & 0xC0) != 0x80)) {
        raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed",
                           p);
        goto fail_mem;
      }
      uint32_t cp =
          ((uint32_t)(c & 0x1F) << 6) | (uint32_t)((uint8_t)p[1] & 0x3F);
      p += 2;
      U16_APPEND_CP(cp);
    } else if (c >= 0xE0 && c <= 0xEF) {
      uint8_t b1 = (p + 1 < end) ? (uint8_t)p[1] : 0;
      uint8_t b2 = (p + 2 < end) ? (uint8_t)p[2] : 0;
      if (CJSON_UNLIKELY(p + 3 > end || (b1 & 0xC0) != 0x80 ||
                         (b2 & 0xC0) != 0x80 || (c == 0xE0 && b1 < 0xA0) ||
                         (c == 0xED && b1 >= 0xA0))) {
        raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed",
                           p);
        goto fail_mem;
      }
      uint32_t cp = ((uint32_t)(c & 0x0F) << 12) |
                    ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
      p += 3;
      U16_APPEND_CP(cp);
    } else if (c >= 0xF0 && c <= 0xF4) {
      uint8_t b1 = (p + 1 < end) ? (uint8_t)p[1] : 0;
      uint8_t b2 = (p + 2 < end) ? (uint8_t)p[2] : 0;
      uint8_t b3 = (p + 3 < end) ? (uint8_t)p[3] : 0;
      if (CJSON_UNLIKELY(p + 4 > end || (b1 & 0xC0) != 0x80 ||
                         (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80 ||
                         (c == 0xF0 && b1 < 0x90) ||
                         (c == 0xF4 && b1 >= 0x90))) {
        raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed",
                           p);
        goto fail_mem;
      }
      uint32_t cp = ((uint32_t)(c & 0x07) << 18) |
                    ((uint32_t)(b1 & 0x3F) << 12) |
                    ((uint32_t)(b2 & 0x3F) << 6) | (uint32_t)(b3 & 0x3F);
      p += 4;
      U16_APPEND_CP(cp);
    } else {
      raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed",
                         p);
      goto fail_mem;
    }
  }

  raise_decode_error(jp, "unexpected end of data", p);

fail_mem:
  if (u16 != NULL && u16 != stack_u16)
    PyMem_Free(u16);
  if (u32 != NULL && u32 != stack_u32)
    PyMem_Free(u32);
  return NULL;

#undef U16_ENSURE
#undef U16_ESCALATE_U32
#undef U16_APPEND_CP
}

/* Cold path: unescape string after finding backslash at `start + clean_len` */
__attribute__((noinline)) static PyObject *
parse_string_escaped(JsonParser *jp, const char **p_io, const char *start,
                     size_t clean_len, int is_ascii_so_far) {
  const char *p = start + clean_len;
  const char *end = jp->end;
  size_t cap_needed = clean_len + 256;
  char *buf = parser_ensure_scratch(jp, cap_needed);
  if (!buf)
    return NULL;
  if (clean_len > 0) {
    memcpy(buf, start, clean_len);
  }
  size_t out_len = clean_len;
  int all_ascii = is_ascii_so_far;

#if defined(CJSON_SIMD_NEON)
  const uint8x16_t v_quote = vdupq_n_u8('"');
  const uint8x16_t v_bs = vdupq_n_u8('\\');
  const uint8x16_t v_ctrl = vdupq_n_u8(0x20);
#elif defined(CJSON_SIMD_AVX2)
  const __m256i v_q = _mm256_set1_epi8('"');
  const __m256i v_bs = _mm256_set1_epi8('\\');
  const __m256i v_1f = _mm256_set1_epi8(0x1F);
  const __m256i v_zero = _mm256_setzero_si256();
#elif defined(CJSON_SIMD_SSE2)
  const __m128i v_q = _mm_set1_epi8('"');
  const __m128i v_bs = _mm_set1_epi8('\\');
  const __m128i v_1f = _mm_set1_epi8(0x1F);
  const __m128i v_zero = _mm_setzero_si128();
#endif

  while (p < end) {
    uint8_t c = (uint8_t)*p;
    if (c == '\\') {
      p++;
      if (CJSON_UNLIKELY(p >= end)) {
        raise_decode_error(jp, "unexpected end of data", p);
        return NULL;
      }
      if (CJSON_UNLIKELY(out_len + 16 > jp->scratch_cap)) {
        buf = parser_ensure_scratch(jp, out_len + 256);
        if (!buf)
          return NULL;
      }
      uint8_t esc = (uint8_t)*p++;
      uint8_t mapped = ESC_LUT[esc];
      if (CJSON_LIKELY(mapped != 0)) {
        buf[out_len++] = (char)mapped;
        continue;
      }
      if (esc == 'u') {
        while (1) {
          if (p + 4 > end) {
            raise_decode_error(jp, "unexpected end of hex escape", p);
            return NULL;
          }
          int cp = parse_hex4(p);
          if (CJSON_UNLIKELY(cp < 0)) {
            raise_decode_error(jp, "invalid \\uXXXX escape", p - 2);
            return NULL;
          }
          p += 4;
          if (CJSON_UNLIKELY(cp >= 0xD800 && cp <= 0xDFFF)) {
            if (cp >= 0xDC00) {
              raise_decode_error(jp, "invalid high surrogate in string", p - 6);
              return NULL;
            }
            if (p + 6 > end || p[0] != '\\' || p[1] != 'u') {
              raise_decode_error(jp, "no low surrogate in string", p - 6);
              return NULL;
            }
            int lo = parse_hex4(p + 2);
            if (lo < 0xDC00 || lo > 0xDFFF) {
              raise_decode_error(jp, "invalid low surrogate in string", p);
              return NULL;
            }
            p += 6;
            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
          }
          if (all_ascii && cp >= 0x80) {
            return parse_string_escaped_u16(jp, p_io, p, end, buf, out_len,
                                            (uint32_t)cp);
          }
          if (CJSON_UNLIKELY(out_len + 8 > jp->scratch_cap)) {
            buf = parser_ensure_scratch(jp, out_len + 256);
            if (!buf)
              return NULL;
          }
          if (cp < 0x80) {
            buf[out_len++] = (char)cp;
          } else if (cp < 0x800) {
            all_ascii = 0;
            buf[out_len++] = (char)(0xC0 | (cp >> 6));
            buf[out_len++] = (char)(0x80 | (cp & 0x3F));
          } else if (cp < 0x10000) {
            all_ascii = 0;
            buf[out_len++] = (char)(0xE0 | (cp >> 12));
            buf[out_len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[out_len++] = (char)(0x80 | (cp & 0x3F));
          } else {
            all_ascii = 0;
            buf[out_len++] = (char)(0xF0 | (cp >> 18));
            buf[out_len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[out_len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[out_len++] = (char)(0x80 | (cp & 0x3F));
          }
          if (p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            continue;
          }
          break;
        }
        continue;
      }
      raise_decode_error(jp, "invalid escape sequence", p - 2);
      return NULL;
    }
    if (c == '"') {
      *p_io = p + 1;
      if (all_ascii) {
        return make_ascii_str(buf, (Py_ssize_t)out_len);
      }
      return decode_utf8_fast(jp, buf, out_len);
    }
    if (CJSON_UNLIKELY(c < 0x20)) {
      raise_decode_error(jp, "invalid control character in string", p);
      return NULL;
    }
    if (p + 1 < end && (p[1] == '\\' || p[1] == '"')) {
      if (c >= 0x80)
        all_ascii = 0;
      if (CJSON_UNLIKELY(out_len + 2 > jp->scratch_cap)) {
        buf = parser_ensure_scratch(jp, out_len + 256);
        if (!buf)
          return NULL;
      }
      buf[out_len++] = (char)c;
      p++;
      continue;
    }

    const char *run = p;
#if defined(CJSON_SIMD_NEON)
    uint8x16_t v_or = vdupq_n_u8(0);
    while (p + 16 <= end) {
      uint8x16_t v = vld1q_u8((const uint8_t *)p);
      uint8x16_t spec =
          vorrq_u8(vceqq_u8(v, v_quote),
                   vorrq_u8(vceqq_u8(v, v_bs), vcltq_u8(v, v_ctrl)));
      uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(spec), 4);
      uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
      if (mask != 0) {
        unsigned int idx = (unsigned int)(__builtin_ctzll(mask) >> 2);
        if ((vmaxvq_u8(v_or) >= 0x80) || bytes_have_hi_small(p, idx)) {
          all_ascii = 0;
        }
        p += idx;
        goto copy_run;
      }
      v_or = vorrq_u8(v_or, v);
      p += 16;
    }
    if (vmaxvq_u8(v_or) >= 0x80)
      all_ascii = 0;
#elif defined(CJSON_SIMD_AVX2)
    __m256i v_or = v_zero;
    while (p + 32 <= end) {
      __m256i v = _mm256_loadu_si256((const __m256i *)p);
      uint32_t mask = (uint32_t)_mm256_movemask_epi8(
          avx2_str_special(v, v_q, v_bs, v_1f, v_zero));
      if (mask != 0) {
        unsigned int idx = (unsigned int)__builtin_ctz(mask);
        if ((_mm256_movemask_epi8(v_or) != 0) ||
            (((uint32_t)_mm256_movemask_epi8(v) &
              (uint32_t)((1ULL << idx) - 1ULL)) != 0)) {
          all_ascii = 0;
        }
        p += idx;
        goto copy_run;
      }
      v_or = _mm256_or_si256(v_or, v);
      p += 32;
    }
    if (_mm256_movemask_epi8(v_or) != 0)
      all_ascii = 0;
#elif defined(CJSON_SIMD_SSE2)
    __m128i v_or = v_zero;
    while (p + 16 <= end) {
      __m128i v = _mm_loadu_si128((const __m128i *)p);
      uint32_t mask = (uint32_t)_mm_movemask_epi8(
          sse2_str_special(v, v_q, v_bs, v_1f, v_zero));
      if (mask != 0) {
        unsigned int idx = (unsigned int)__builtin_ctz(mask);
        if ((_mm_movemask_epi8(v_or) != 0) ||
            (((uint32_t)_mm_movemask_epi8(v) & ((1u << idx) - 1u)) != 0)) {
          all_ascii = 0;
        }
        p += idx;
        goto copy_run;
      }
      v_or = _mm_or_si128(v_or, v);
      p += 16;
    }
    if (_mm_movemask_epi8(v_or) != 0)
      all_ascii = 0;
#else
    uint64_t w_or = 0;
    while (p + 8 <= end) {
      uint64_t w;
      memcpy(&w, p, 8);
      uint64_t m = swar_str_special(w);
      if (m != 0) {
        unsigned int idx = (unsigned int)(__builtin_ctzll(m) >> 3);
        uint64_t hi_mask = idx ? ((1ULL << (idx * 8)) - 1ULL) : 0ULL;
        if (((w_or | (w & hi_mask)) & 0x8080808080808080ULL) != 0) {
          all_ascii = 0;
        }
        p += idx;
        goto copy_run;
      }
      w_or |= w;
      p += 8;
    }
    if ((w_or & 0x8080808080808080ULL) != 0)
      all_ascii = 0;
#endif
    while (p < end) {
      uint8_t tc = (uint8_t)*p;
      if (tc == '"' || tc == '\\' || tc < 0x20)
        break;
      if (tc >= 0x80)
        all_ascii = 0;
      p++;
    }
  copy_run:;
    size_t run_len = (size_t)(p - run);
    if (run_len > 0) {
      if (CJSON_UNLIKELY(out_len + run_len + 16 > jp->scratch_cap)) {
        buf = parser_ensure_scratch(jp, out_len + run_len + 256);
        if (!buf)
          return NULL;
      }
      memcpy(buf + out_len, run, run_len);
      out_len += run_len;
    }
  }

  raise_decode_error(jp, "unexpected end of data", p);
  return NULL;
}

/* Cold path: non-ASCII UTF-8 string scanner (entered once a byte >= 0x80 is seen) */
__attribute__((noinline))
static PyObject *parse_string_utf8(
    JsonParser *jp, const char **p_io, const char *start, const char *p, const char *end) {
#if defined(CJSON_SIMD_NEON)
  const uint8x16_t v_quote = vdupq_n_u8('"');
  const uint8x16_t v_bs = vdupq_n_u8('\\');
  const uint8x16_t v_ctrl = vdupq_n_u8(0x20);
  while (p + 16 <= end) {
    uint8x16_t v = vld1q_u8((const uint8_t *)p);
    uint8x16_t spec = neon_esc_mask(v, v_quote, v_bs, v_ctrl);
    uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(spec), 4);
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
    if (mask != 0) {
      unsigned int idx = (unsigned int)(__builtin_ctzll(mask) >> 2);
      const char *hit = p + idx;
      if (CJSON_LIKELY(*hit == '"')) {
        *p_io = hit + 1;
        return decode_utf8_fast(jp, start, (size_t)(hit - start));
      }
      if (*hit == '\\') {
        return parse_string_escaped(jp, p_io, start, (size_t)(hit - start), 0);
      }
      raise_decode_error(jp, "invalid control character in string", hit);
      return NULL;
    }
    p += 16;
  }
#elif defined(CJSON_SIMD_AVX2)
  const __m256i v_q = _mm256_set1_epi8('"');
  const __m256i v_bs = _mm256_set1_epi8('\\');
  const __m256i v_1f = _mm256_set1_epi8(0x1F);
  const __m256i v_zero = _mm256_setzero_si256();
  while (p + 32 <= end) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(avx2_esc_mask(v, v_q, v_bs, v_1f, v_zero));
    if (mask != 0) {
      unsigned int idx = (unsigned int)__builtin_ctz(mask);
      const char *hit = p + idx;
      if (CJSON_LIKELY(*hit == '"')) {
        *p_io = hit + 1;
        return decode_utf8_fast(jp, start, (size_t)(hit - start));
      }
      if (*hit == '\\') {
        return parse_string_escaped(jp, p_io, start, (size_t)(hit - start), 0);
      }
      raise_decode_error(jp, "invalid control character in string", hit);
      return NULL;
    }
    p += 32;
  }
#elif defined(CJSON_SIMD_SSE2)
  const __m128i v_q = _mm_set1_epi8('"');
  const __m128i v_bs = _mm_set1_epi8('\\');
  const __m128i v_1f = _mm_set1_epi8(0x1F);
  const __m128i v_zero = _mm_setzero_si128();
  while (p + 16 <= end) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    uint32_t mask = (uint32_t)_mm_movemask_epi8(sse2_esc_mask(v, v_q, v_bs, v_1f, v_zero));
    if (mask != 0) {
      unsigned int idx = (unsigned int)__builtin_ctz(mask);
      const char *hit = p + idx;
      if (CJSON_LIKELY(*hit == '"')) {
        *p_io = hit + 1;
        return decode_utf8_fast(jp, start, (size_t)(hit - start));
      }
      if (*hit == '\\') {
        return parse_string_escaped(jp, p_io, start, (size_t)(hit - start), 0);
      }
      raise_decode_error(jp, "invalid control character in string", hit);
      return NULL;
    }
    p += 16;
  }
#else
  while (p + 8 <= end) {
    uint64_t w;
    memcpy(&w, p, 8);
    uint64_t m = swar_esc(w);
    if (m != 0) {
      unsigned int idx = (unsigned int)(__builtin_ctzll(m) >> 3);
      const char *hit = p + idx;
      if (CJSON_LIKELY(*hit == '"')) {
        *p_io = hit + 1;
        return decode_utf8_fast(jp, start, (size_t)(hit - start));
      }
      if (*hit == '\\') {
        return parse_string_escaped(jp, p_io, start, (size_t)(hit - start), 0);
      }
      raise_decode_error(jp, "invalid control character in string", hit);
      return NULL;
    }
    p += 8;
  }
#endif
  while (p < end) {
    uint8_t c = (uint8_t)*p;
    if (c == '"') {
      *p_io = p + 1;
      return decode_utf8_fast(jp, start, (size_t)(p - start));
    }
    if (c == '\\') {
      return parse_string_escaped(jp, p_io, start, (size_t)(p - start), 0);
    }
    if (CJSON_UNLIKELY(c < 0x20)) {
      raise_decode_error(jp, "invalid control character in string", p);
      return NULL;
    }
    p++;
  }
  raise_decode_error(jp, "unexpected end of data", p);
  return NULL;
}

/* Long/EOF ASCII string scanner (out-of-line so short-string fast path stays inlined) */
__attribute__((noinline))
static PyObject *parse_string_long(
    JsonParser *jp, const char **p_io, const char *start, const char *p, const char *end) {
  p = cjson_find_str_special_long_fn(p, end);
  while (p < end) {
    uint8_t c = (uint8_t)*p;
    if (c == '"') {
      *p_io = p + 1;
      size_t slen = (size_t)(p - start);
#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      if (slen >= 16 && slen <= 48) {
        return lookup_or_create_long_ascii_val(start, (uint32_t)slen);
      }
#endif
      return make_ascii_str(start, (Py_ssize_t)slen);
    }
    if (c == '\\') {
      return parse_string_escaped(jp, p_io, start, (size_t)(p - start), 1);
    }
    if (c >= 0x80) {
      return parse_string_utf8(jp, p_io, start, p, end);
    }
    if (CJSON_UNLIKELY(c < 0x20)) {
      raise_decode_error(jp, "invalid control character in string", p);
      return NULL;
    }
    p++;
  }
  raise_decode_error(jp, "unexpected end of data", p);
  return NULL;
}

/* Always-inline short ASCII string parser: `*p_io` enters right after opening `"` */
__attribute__((always_inline))
static inline PyObject *parse_string_fast(JsonParser *jp, const char **p_io, const char *end) {
  const char *start = *p_io;
  const char *p = start;

#if defined(CJSON_SIMD_AVX2)
  if (CJSON_LIKELY(p + 32 <= end)) {
    const __m256i v_q = _mm256_set1_epi8('"');
    const __m256i v_bs = _mm256_set1_epi8('\\');
    const __m256i v_20 = _mm256_set1_epi8(0x20);
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(avx2_str_ascii_mask(v, v_q, v_bs, v_20));
    if (CJSON_LIKELY(mask != 0)) {
      unsigned int idx = (unsigned int)__builtin_ctz(mask);
      const char *hit = p + idx;
      uint8_t ch = (uint8_t)*hit;
      if (CJSON_LIKELY(ch == '"')) {
        *p_io = hit + 1;
#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        if (idx <= 7) {
          uint64_t w0;
          memcpy(&w0, start, 8);
          return lookup_or_create_fast_ascii_val7(start, idx, w0);
        }
        if (idx <= 15) {
          uint64_t w0, w1;
          memcpy(&w0, start, 8);
          memcpy(&w1, start + 8, 8);
          return lookup_or_create_fast_ascii_val15(start, idx, w0, w1);
        }
#endif
        return make_ascii_str(start, (Py_ssize_t)idx);
      }
      if (ch == '\\') {
        return parse_string_escaped(jp, p_io, start, (size_t)idx, 1);
      }
      if (ch >= 0x80) {
        return parse_string_utf8(jp, p_io, start, hit, end);
      }
      raise_decode_error(jp, "invalid control character in string", hit);
      return NULL;
    }
    p += 32;
  }
#elif defined(CJSON_SIMD_SSE2)
  if (CJSON_LIKELY(p + 16 <= end)) {
    const __m128i v_q = _mm_set1_epi8('"');
    const __m128i v_bs = _mm_set1_epi8('\\');
    const __m128i v_20 = _mm_set1_epi8(0x20);
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    uint32_t mask = (uint32_t)_mm_movemask_epi8(sse2_str_ascii_mask(v, v_q, v_bs, v_20));
    if (CJSON_LIKELY(mask != 0)) {
      unsigned int idx = (unsigned int)__builtin_ctz(mask);
      const char *hit = p + idx;
      uint8_t ch = (uint8_t)*hit;
      if (CJSON_LIKELY(ch == '"')) {
        *p_io = hit + 1;
#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        if (idx <= 7) {
          uint64_t w0;
          memcpy(&w0, start, 8);
          return lookup_or_create_fast_ascii_val7(start, idx, w0);
        }
        uint64_t w0, w1;
        memcpy(&w0, start, 8);
        memcpy(&w1, start + 8, 8);
        return lookup_or_create_fast_ascii_val15(start, idx, w0, w1);
#else
        return make_ascii_str(start, (Py_ssize_t)idx);
#endif
      }
      if (ch == '\\') {
        return parse_string_escaped(jp, p_io, start, (size_t)idx, 1);
      }
      if (ch >= 0x80) {
        return parse_string_utf8(jp, p_io, start, hit, end);
      }
      raise_decode_error(jp, "invalid control character in string", hit);
      return NULL;
    }
    p += 16;
    if (CJSON_LIKELY(p + 16 <= end)) {
      __m128i v2 = _mm_loadu_si128((const __m128i *)p);
      uint32_t mask2 = (uint32_t)_mm_movemask_epi8(sse2_str_ascii_mask(v2, v_q, v_bs, v_20));
      if (mask2 != 0) {
        unsigned int idx = (unsigned int)__builtin_ctz(mask2);
        const char *hit = p + idx;
        uint8_t ch = (uint8_t)*hit;
        if (CJSON_LIKELY(ch == '"')) {
          *p_io = hit + 1;
          uint32_t slen = 16u + idx;
#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
          return lookup_or_create_long_ascii_val(start, slen);
#else
          return make_ascii_str(start, (Py_ssize_t)slen);
#endif
        }
        if (ch == '\\') {
          return parse_string_escaped(jp, p_io, start, (size_t)(16u + idx), 1);
        }
        if (ch >= 0x80) {
          return parse_string_utf8(jp, p_io, start, hit, end);
        }
        raise_decode_error(jp, "invalid control character in string", hit);
        return NULL;
      }
      p += 16;
    }
  }
#else
  /* Hybrid SWAR-first (ARM64 + scalar): 2x8B in GPRs avoids NEON->GPR crossing
   * latency on short strings; >=16B strings fall through to NEON in parse_string_long. */
  if (CJSON_LIKELY(p + 8 <= end)) {
    uint64_t w0;
    memcpy(&w0, p, 8);
    uint64_t m0 = swar_str_ascii_mask(w0);
    if (m0 != 0) {
      unsigned int idx = (unsigned int)(__builtin_ctzll(m0) >> 3);
      const char *hit = p + idx;
      uint8_t ch = (uint8_t)*hit;
      if (CJSON_LIKELY(ch == '"')) {
        *p_io = hit + 1;
#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return lookup_or_create_fast_ascii_val7(start, idx, w0);
#else
        return make_ascii_str(start, (Py_ssize_t)idx);
#endif
      }
      if (ch == '\\') {
        return parse_string_escaped(jp, p_io, start, (size_t)idx, 1);
      }
      if (ch >= 0x80) {
        return parse_string_utf8(jp, p_io, start, hit, end);
      }
      raise_decode_error(jp, "invalid control character in string", hit);
      return NULL;
    }
    p += 8;
    if (CJSON_LIKELY(p + 8 <= end)) {
      uint64_t w1;
      memcpy(&w1, p, 8);
      uint64_t m1 = swar_str_ascii_mask(w1);
      if (m1 != 0) {
        unsigned int idx = (unsigned int)(__builtin_ctzll(m1) >> 3);
        const char *hit = p + idx;
        uint8_t ch = (uint8_t)*hit;
        if (CJSON_LIKELY(ch == '"')) {
          *p_io = hit + 1;
#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
          return lookup_or_create_fast_ascii_val15(start, 8u + idx, w0, w1);
#else
          return make_ascii_str(start, (Py_ssize_t)(8u + idx));
#endif
        }
        if (ch == '\\') {
          return parse_string_escaped(jp, p_io, start, (size_t)(8u + idx), 1);
        }
        if (ch >= 0x80) {
          return parse_string_utf8(jp, p_io, start, hit, end);
        }
        raise_decode_error(jp, "invalid control character in string", hit);
        return NULL;
      }
      p += 8;
    }
  }
#endif

  return parse_string_long(jp, p_io, start, p, end);
}

/* Long/escaped/EOF dict key fallback (out-of-line so <=15B key path stays inlined) */
__attribute__((noinline))
static PyObject *parse_dict_key_long(
    JsonParser *jp, const char **p_io, const char *end, Py_hash_t *out_hash) {
  const char *start = *p_io;

#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  if (start + 16 <= end) {
    const char *p = start;
#if defined(CJSON_SIMD_NEON)
    const uint8x16_t v_quote = vdupq_n_u8('"');
    const uint8x16_t v_bs = vdupq_n_u8('\\');
    const int8x16_t v_s20 = vdupq_n_s8(0x20);
    for (int vec_i = 0; vec_i < 4 && p + 16 <= end; vec_i++) {
      uint8x16_t v = vld1q_u8((const uint8_t *)p);
      uint8x16_t spec = neon_str_ascii_mask(v, v_quote, v_bs, v_s20);
      uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(spec), 4);
      uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
      if (mask != 0) {
        unsigned int idx = (unsigned int)(__builtin_ctzll(mask) >> 2);
        if (CJSON_LIKELY(p[idx] == '"')) {
          *p_io = p + idx + 1;
          uint32_t klen = (uint32_t)((p + idx) - start);
          if (CJSON_UNLIKELY(klen == 0)) {
            Py_INCREF(g_empty_str);
            *out_hash = g_empty_str_hash;
            return g_empty_str;
          }
          return lookup_or_create_long_ascii_key(start, klen, out_hash);
        }
        break;
      }
      p += 16;
    }
#elif defined(CJSON_SIMD_AVX2)
    const __m256i v_q = _mm256_set1_epi8('"');
    const __m256i v_bs = _mm256_set1_epi8('\\');
    const __m256i v_20 = _mm256_set1_epi8(0x20);
    for (int vec_i = 0; vec_i < 2 && p + 32 <= end; vec_i++) {
      __m256i v = _mm256_loadu_si256((const __m256i *)p);
      uint32_t mask = (uint32_t)_mm256_movemask_epi8(avx2_str_ascii_mask(v, v_q, v_bs, v_20));
      if (mask != 0) {
        unsigned int idx = (unsigned int)__builtin_ctz(mask);
        if (CJSON_LIKELY(p[idx] == '"')) {
          *p_io = p + idx + 1;
          uint32_t klen = (uint32_t)((p + idx) - start);
          if (CJSON_UNLIKELY(klen == 0)) {
            Py_INCREF(g_empty_str);
            *out_hash = g_empty_str_hash;
            return g_empty_str;
          }
          return lookup_or_create_long_ascii_key(start, klen, out_hash);
        }
        break;
      }
      p += 32;
    }
#elif defined(CJSON_SIMD_SSE2)
    const __m128i v_q = _mm_set1_epi8('"');
    const __m128i v_bs = _mm_set1_epi8('\\');
    const __m128i v_20 = _mm_set1_epi8(0x20);
    for (int vec_i = 0; vec_i < 4 && p + 16 <= end; vec_i++) {
      __m128i v = _mm_loadu_si128((const __m128i *)p);
      uint32_t mask = (uint32_t)_mm_movemask_epi8(sse2_str_ascii_mask(v, v_q, v_bs, v_20));
      if (mask != 0) {
        unsigned int idx = (unsigned int)__builtin_ctz(mask);
        if (CJSON_LIKELY(p[idx] == '"')) {
          *p_io = p + idx + 1;
          uint32_t klen = (uint32_t)((p + idx) - start);
          if (CJSON_UNLIKELY(klen == 0)) {
            Py_INCREF(g_empty_str);
            *out_hash = g_empty_str_hash;
            return g_empty_str;
          }
          return lookup_or_create_long_ascii_key(start, klen, out_hash);
        }
        break;
      }
      p += 16;
    }
#else
    for (int blk = 0; blk < 8 && p + 8 <= end; blk++) {
      uint64_t w;
      memcpy(&w, p, 8);
      uint64_t m = swar_str_ascii_mask(w);
      if (m != 0) {
        unsigned int idx = (unsigned int)(__builtin_ctzll(m) >> 3);
        if (CJSON_LIKELY(p[idx] == '"')) {
          *p_io = p + idx + 1;
          uint32_t klen = (uint32_t)((p + idx) - start);
          if (CJSON_UNLIKELY(klen == 0)) {
            Py_INCREF(g_empty_str);
            *out_hash = g_empty_str_hash;
            return g_empty_str;
          }
          return lookup_or_create_long_ascii_key(start, klen, out_hash);
        }
        break;
      }
      p += 8;
    }
#endif
  } else {
    const char *p = start;
    while (p < end) {
      uint8_t c = (uint8_t)*p;
      if (c == '"') {
        uint32_t klen = (uint32_t)(p - start);
        *p_io = p + 1;
        if (klen == 0) {
          Py_INCREF(g_empty_str);
          *out_hash = g_empty_str_hash;
          return g_empty_str;
        }
        return lookup_or_create_long_ascii_key(start, klen, out_hash);
      }
      if (c == '\\' || c < 0x20 || c >= 0x80) break;
      p++;
    }
  }
  /* Non-ASCII unescaped UTF-8 dict key cache (<= 39 raw UTF-8 bytes) */
  {
    const char *q = start;
    const char *qlim = (end - start > 40) ? (start + 40) : end;
    int has_non_ascii = 0;
    while (q < qlim) {
      uint8_t c = (uint8_t)*q;
      if (c == '"') {
        if (has_non_ascii && q > start) {
          uint32_t raw_len = (uint32_t)(q - start);
          uint64_t h64;
          PyObject *cached = lookup_utf8_key_cache(start, raw_len, &h64, out_hash);
          if (CJSON_LIKELY(cached != NULL)) {
            *p_io = q + 1;
            return cached;
          }
          PyObject *u = parse_string_fast(jp, p_io, end);
          if (CJSON_LIKELY(u != NULL)) {
            Py_hash_t py_h = PyObject_Hash(u);
            *out_hash = py_h;
            store_utf8_key_cache(start, raw_len, h64, u, py_h);
          }
          return u;
        }
        break;
      }
      if (c == '\\' || c < 0x20) break;
      if (c >= 0x80) has_non_ascii = 1;
      q++;
    }
  }
#elif defined(Py_GIL_DISABLED)
  {
    const char *p = start;
    for (int blk = 0; blk < 8 && p + 8 <= end; blk++) {
      uint64_t w;
      memcpy(&w, p, 8);
      uint64_t m = swar_str_ascii_mask(w);
      if (CJSON_LIKELY(m != 0)) {
        unsigned int idx = (unsigned int)(__builtin_ctzll(m) >> 3);
        if (CJSON_LIKELY(p[idx] == '"')) {
          *p_io = p + idx + 1;
          uint32_t klen = (uint32_t)((p + idx) - start);
          return lookup_or_create_cached_key_gil_disabled(jp, start, klen, out_hash);
        }
        goto fallback;
      }
      p += 8;
    }
  }
fallback:;
#endif

  PyObject *u = parse_string_fast(jp, p_io, end);
  if (u != NULL) {
    *out_hash = PyObject_Hash(u);
  }
  return u;
}

/* Always-inline <=15B ASCII dict key fast path using 64B cache-line FastKeySlot */
__attribute__((always_inline))
static inline PyObject *parse_dict_key(
    JsonParser *jp, const char **p_io, const char *end, Py_hash_t *out_hash) {
#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  const char *start = *p_io;
  if (CJSON_LIKELY(start + 16 <= end)) {
#if defined(CJSON_SIMD_AVX2)
    if (CJSON_LIKELY(start + 32 <= end)) {
      const __m256i v_q = _mm256_set1_epi8('"');
      const __m256i v_bs = _mm256_set1_epi8('\\');
      const __m256i v_20 = _mm256_set1_epi8(0x20);
      __m256i v0 = _mm256_loadu_si256((const __m256i *)start);
      uint32_t mask = (uint32_t)_mm256_movemask_epi8(avx2_str_ascii_mask(v0, v_q, v_bs, v_20));
      if (CJSON_LIKELY(mask != 0)) {
        unsigned int idx = (unsigned int)__builtin_ctz(mask);
        if (CJSON_LIKELY(start[idx] == '"')) {
          *p_io = start + idx + 1;
          if (CJSON_UNLIKELY(idx == 0)) {
            Py_INCREF(g_empty_str);
            *out_hash = g_empty_str_hash;
            return g_empty_str;
          }
          if (CJSON_LIKELY(idx <= 15)) {
            uint64_t raw_lo, raw_hi;
            memcpy(&raw_lo, start, 8);
            memcpy(&raw_hi, start + 8, 8);
            return lookup_or_create_fast_ascii_key15(start, idx, raw_lo, raw_hi, out_hash);
          }
          return lookup_or_create_long_ascii_key(start, idx, out_hash);
        }
        goto slow;
      }
    }
#elif defined(CJSON_SIMD_SSE2)
    const __m128i v_q = _mm_set1_epi8('"');
    const __m128i v_bs = _mm_set1_epi8('\\');
    const __m128i v_20 = _mm_set1_epi8(0x20);
    __m128i v = _mm_loadu_si128((const __m128i *)start);
    uint32_t mask = (uint32_t)_mm_movemask_epi8(sse2_str_ascii_mask(v, v_q, v_bs, v_20));
    if (CJSON_LIKELY(mask != 0)) {
      unsigned int idx = (unsigned int)__builtin_ctz(mask);
      if (CJSON_LIKELY(start[idx] == '"')) {
        *p_io = start + idx + 1;
        if (CJSON_UNLIKELY(idx == 0)) {
          Py_INCREF(g_empty_str);
          *out_hash = g_empty_str_hash;
          return g_empty_str;
        }
        uint64_t raw_lo, raw_hi;
        memcpy(&raw_lo, start, 8);
        memcpy(&raw_hi, start + 8, 8);
        return lookup_or_create_fast_ascii_key15(start, idx, raw_lo, raw_hi, out_hash);
      }
      goto slow;
    }
#else
    /* Hybrid SWAR-first (ARM64 + scalar): raw_lo/raw_hi are loaded once into GPRs
     * and reused directly by lookup_or_create_fast_ascii_key15 without NEON crossing. */
    uint64_t raw_lo, raw_hi;
    memcpy(&raw_lo, start, 8);
    memcpy(&raw_hi, start + 8, 8);
    uint64_t m0 = swar_str_ascii_mask(raw_lo);
    if (m0 != 0) {
      unsigned int idx = (unsigned int)(__builtin_ctzll(m0) >> 3);
      if (CJSON_LIKELY(start[idx] == '"')) {
        *p_io = start + idx + 1;
        if (CJSON_UNLIKELY(idx == 0)) {
          Py_INCREF(g_empty_str);
          *out_hash = g_empty_str_hash;
          return g_empty_str;
        }
        return lookup_or_create_fast_ascii_key15(start, idx, raw_lo, raw_hi, out_hash);
      }
      goto slow;
    }
    uint64_t m1 = swar_str_ascii_mask(raw_hi);
    if (CJSON_LIKELY(m1 != 0)) {
      unsigned int idx = 8u + (unsigned int)(__builtin_ctzll(m1) >> 3);
      if (CJSON_LIKELY(start[idx] == '"')) {
        *p_io = start + idx + 1;
        return lookup_or_create_fast_ascii_key15(start, idx, raw_lo, raw_hi, out_hash);
      }
      goto slow;
    }
#endif
  }
slow:;
#endif
  return parse_dict_key_long(jp, p_io, end, out_hash);
}

#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
/* Out-of-line miss handler for schema prediction: tries optional-field +1 resync,
 * otherwise falls back to parse_dict_key and marks the schema dirty. */
__attribute__((noinline))
static PyObject *parse_dict_key_schema_miss(
    JsonParser *jp, const char **p_io, const char *end, Py_hash_t *out_hash,
    const ObjectSchema *sc, uint32_t *pred_idx_p, int *schema_dirty_p) {
  uint32_t pi = *pred_idx_p;
  if (pi + 1 < sc->len) {
    PyObject *k = schema_match_fast(&sc->keys[pi + 1], p_io, end, out_hash);
    if (k != NULL) {
      *pred_idx_p = pi + 2;
      return k;
    }
  }
  if (pi < CJSON_SCHEMA_MAX_KEYS) {
    *schema_dirty_p = 1;
  }
  *pred_idx_p = pi + 1;
  return parse_dict_key(jp, p_io, end, out_hash);
}
#endif

__attribute__((noinline)) static void
raise_unexpected_value_error(JsonParser *jp, const char *p, const char *end) {
  if (p >= end) {
    raise_decode_error(jp, "unexpected end of data", p);
    return;
  }
  uint8_t c = (uint8_t)*p;
  if (c >= 0x80) {
    if (c == 0xEF && p + 3 <= end && (uint8_t)p[1] == 0xBB &&
        (uint8_t)p[2] == 0xBF) {
      raise_decode_error(jp, "UTF-8 byte order mark (BOM) is not supported", p);
      return;
    }
    PyObject *tmp = PyUnicode_DecodeUTF8(p, end - p, NULL);
    if (tmp == NULL) {
      PyErr_Clear();
      raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed",
                         p);
      return;
    }
    Py_DECREF(tmp);
  }
  raise_decode_error(jp, "unexpected character, expected a JSON value", p);
}

static PyObject *parse_object_rec(JsonParser *jp, const char **p_io, const char *end, int depth, uint32_t schema_slot);
static PyObject *parse_array_rec(JsonParser *jp, const char **p_io, const char *end, int depth, uint32_t schema_slot);

/* Iterative fallback for deep nesting (depth >= CJSON_REC_PARSE_DEPTH up to 1024) */
__attribute__((noinline))
static PyObject *parse_document_iter(JsonParser *jp, const char **p_io, const char *end, int base_depth) {
  const char *p = *p_io;
  ParseFrame stack_frames[64];
  ParseFrame *frames = stack_frames;
  size_t frame_sp = 0;
  size_t frame_cap = 64;
  PyObject *val = NULL;

parse_value:;
  if (CJSON_UNLIKELY(p >= end)) {
    raise_decode_error(jp, "unexpected end of data", p);
    goto fail;
  }
  {
    char c = *p;
    if (c == '"') {
      p++;
      val = parse_string_fast(jp, &p, end);
      if (CJSON_UNLIKELY(val == NULL)) goto fail;
      goto attach_value;
    }
    if (c == '-' || (uint32_t)(c - '0') < 10) {
      val = parse_number(jp, &p, end);
      if (CJSON_UNLIKELY(val == NULL)) goto fail;
      goto attach_value;
    }
    if (c == '{') {
      if (CJSON_UNLIKELY(base_depth + (int)frame_sp >= CJSON_MAX_PARSE_DEPTH)) {
        raise_decode_error(jp, "depth limit exceeded", p);
        goto fail;
      }
      p = skip_ws(p + 1, end);
      if (CJSON_UNLIKELY(p >= end)) {
        raise_decode_error(jp, "unexpected end of data", p);
        goto fail;
      }
      if (*p == '}') {
        p++;
        val = PyDict_New();
        if (CJSON_UNLIKELY(val == NULL)) goto fail;
        goto attach_value;
      }
      if (CJSON_UNLIKELY(frame_sp >= frame_cap)) {
        size_t new_cap = frame_cap * 2;
        ParseFrame *nf;
        if (frames == stack_frames) {
          nf = (ParseFrame *)PyMem_Malloc(new_cap * sizeof(ParseFrame));
          if (nf) memcpy(nf, stack_frames, frame_sp * sizeof(ParseFrame));
        } else {
          nf = (ParseFrame *)PyMem_Realloc(frames, new_cap * sizeof(ParseFrame));
        }
        if (!nf) {
          PyErr_NoMemory();
          goto fail;
        }
        frames = nf;
        frame_cap = new_cap;
      }
      frames[frame_sp++] = (ParseFrame){jp->kv_sp, 1};
      goto parse_obj_key;
    }
    if (c == '[') {
      if (CJSON_UNLIKELY(base_depth + (int)frame_sp >= CJSON_MAX_PARSE_DEPTH)) {
        raise_decode_error(jp, "depth limit exceeded", p);
        goto fail;
      }
      p = skip_ws(p + 1, end);
      if (CJSON_UNLIKELY(p >= end)) {
        raise_decode_error(jp, "unexpected end of data", p);
        goto fail;
      }
      if (*p == ']') {
        p++;
        val = PyList_New(0);
        if (CJSON_UNLIKELY(val == NULL)) goto fail;
        goto attach_value;
      }
      if (CJSON_UNLIKELY(frame_sp >= frame_cap)) {
        size_t new_cap = frame_cap * 2;
        ParseFrame *nf;
        if (frames == stack_frames) {
          nf = (ParseFrame *)PyMem_Malloc(new_cap * sizeof(ParseFrame));
          if (nf) memcpy(nf, stack_frames, frame_sp * sizeof(ParseFrame));
        } else {
          nf = (ParseFrame *)PyMem_Realloc(frames, new_cap * sizeof(ParseFrame));
        }
        if (!nf) {
          PyErr_NoMemory();
          goto fail;
        }
        frames = nf;
        frame_cap = new_cap;
      }
      frames[frame_sp++] = (ParseFrame){jp->item_sp, 0};
      goto parse_value;
    }
    if (c == 't' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "true", 4) == 0)) {
      p += 4;
      Py_INCREF(Py_True);
      val = Py_True;
      goto attach_value;
    }
    if (c == 'f' && CJSON_LIKELY(p + 5 <= end && memcmp(p + 1, "alse", 4) == 0)) {
      p += 5;
      Py_INCREF(Py_False);
      val = Py_False;
      goto attach_value;
    }
    if (c == 'n' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "null", 4) == 0)) {
      p += 4;
      Py_INCREF(Py_None);
      val = Py_None;
      goto attach_value;
    }
    raise_unexpected_value_error(jp, p, end);
    goto fail;
  }

parse_obj_key:;
  if (CJSON_UNLIKELY(p >= end)) {
    raise_decode_error(jp, "unexpected end of data", p);
    goto fail;
  }
  if (CJSON_UNLIKELY(*p != '"')) {
    raise_decode_error(jp, "expected string key in object", p);
    goto fail;
  }
  p++;
  {
    Py_hash_t kh;
    PyObject *key = parse_dict_key(jp, &p, end, &kh);
    if (CJSON_UNLIKELY(key == NULL)) goto fail;

    if (CJSON_UNLIKELY(jp->kv_sp >= jp->kv_cap)) {
      if (parser_grow_kvs(jp) < 0) {
        Py_DECREF(key);
        goto fail;
      }
    }
    jp->kvs[jp->kv_sp++] = (KvEntry){key, NULL, kh};

    if (p < end && *p == ' ') p++;
    if (CJSON_UNLIKELY(p >= end || *p != ':')) {
      p = skip_ws(p, end);
      if (CJSON_UNLIKELY(p >= end || *p != ':')) {
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "expected ':' after key", p);
        goto fail;
      }
    }
    p++;
    if (p < end && *p == ' ') p++;
    if (CJSON_UNLIKELY(p >= end || (uint8_t)*p <= 0x20)) {
      p = skip_ws(p, end);
    }
    goto parse_value;
  }

attach_value:;
  if (CJSON_UNLIKELY(frame_sp == 0)) {
    *p_io = p;
    if (frames != stack_frames) PyMem_Free(frames);
    return val;
  }

  if (!frames[frame_sp - 1].is_object) {
    if (CJSON_UNLIKELY(jp->item_sp >= jp->item_cap)) {
      if (parser_grow_items(jp) < 0) {
        Py_DECREF(val);
        goto fail;
      }
    }
    jp->items[jp->item_sp++] = val;

    if (CJSON_LIKELY(p < end && *p == ',')) {
      p++;
      if (CJSON_LIKELY(p < end && (uint8_t)*p > 0x20 && *p != ']')) {
        goto parse_value;
      }
      p = skip_ws(p, end);
      if (CJSON_UNLIKELY(p >= end || *p == ']')) {
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                           (p >= end) ? p : (p - 1));
        goto fail;
      }
      goto parse_value;
    }
    if (p < end && *p == ']') goto close_array;
    p = skip_ws(p, end);
    if (CJSON_UNLIKELY(p >= end)) {
      raise_decode_error(jp, "unexpected end of data", p);
      goto fail;
    }
    if (*p == ',') {
      p = skip_ws(p + 1, end);
      if (CJSON_UNLIKELY(p >= end || *p == ']')) {
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                           (p >= end) ? p : (p - 1));
        goto fail;
      }
      goto parse_value;
    }
    if (*p == ']') goto close_array;
    raise_decode_error(jp, "expected ',' or ']' in array", p);
    goto fail;
  } else {
    jp->kvs[jp->kv_sp - 1].val = val;

    if (CJSON_LIKELY(p < end && *p == ',')) {
      p++;
      if (p < end && *p == '"') goto parse_obj_key;
      p = skip_ws(p, end);
      if (CJSON_UNLIKELY(p >= end || *p == '}')) {
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                           (p >= end) ? p : (p - 1));
        goto fail;
      }
      goto parse_obj_key;
    }
    if (p < end && *p == '}') goto close_object;
    p = skip_ws(p, end);
    if (CJSON_UNLIKELY(p >= end)) {
      raise_decode_error(jp, "unexpected end of data", p);
      goto fail;
    }
    if (*p == ',') {
      p = skip_ws(p + 1, end);
      if (CJSON_UNLIKELY(p >= end || *p == '}')) {
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                           (p >= end) ? p : (p - 1));
        goto fail;
      }
      goto parse_obj_key;
    }
    if (*p == '}') goto close_object;
    raise_decode_error(jp, "expected ',' or '}' in object", p);
    goto fail;
  }

close_array:;
  p++;
  {
    size_t base = frames[--frame_sp].base;
    Py_ssize_t count = (Py_ssize_t)(jp->item_sp - base);
    PyObject *list = PyList_New(count);
    if (CJSON_UNLIKELY(list == NULL)) goto fail;
    PyObject **src = &jp->items[base];
    for (Py_ssize_t i = 0; i < count; i++) {
      PyList_SET_ITEM(list, i, src[i]);
    }
    jp->item_sp = base;
    val = list;
    goto attach_value;
  }

close_object:;
  p++;
  {
    size_t base = frames[--frame_sp].base;
    Py_ssize_t count = (Py_ssize_t)(jp->kv_sp - base);
    PyObject *dict = (count <= 5) ? PyDict_New() : _PyDict_NewPresized(count);
    if (CJSON_UNLIKELY(dict == NULL)) goto fail;
    KvEntry *entries = &jp->kvs[base];
    for (Py_ssize_t i = 0; i < count; i++) {
      int rc = _PyDict_SetItem_KnownHash(dict, entries[i].key, entries[i].val, entries[i].hash);
      Py_DECREF(entries[i].key);
      Py_DECREF(entries[i].val);
      if (CJSON_UNLIKELY(rc < 0)) {
        for (Py_ssize_t j = i + 1; j < count; j++) {
          Py_DECREF(entries[j].key);
          Py_DECREF(entries[j].val);
        }
        jp->kv_sp = base;
        Py_DECREF(dict);
        goto fail;
      }
    }
    jp->kv_sp = base;
    val = dict;
    goto attach_value;
  }

fail:
  if (frames != stack_frames) PyMem_Free(frames);
  return NULL;
}

static PyObject *parse_object_rec(
    JsonParser *jp, const char **p_io, const char *end, int depth, uint32_t schema_slot) {
  const char *p = skip_ws(*p_io, end);
  if (CJSON_UNLIKELY(p >= end)) {
    raise_decode_error(jp, "unexpected end of data", p);
    return NULL;
  }
  if (*p == '}') {
    *p_io = p + 1;
    return PyDict_New();
  }
  if (CJSON_UNLIKELY(*p != '"')) {
    raise_decode_error(jp, "expected string key in object", p);
    return NULL;
  }

#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  ObjectSchema *sc = &g_schemas[schema_slot];
  uint32_t pred_idx = 0;
  int schema_dirty = (sc->len == 0);
  PyObject *dict = NULL;

#if defined(CJSON_SPLIT_DICT_FAST)
  if (CJSON_LIKELY(sc->split_ok)) {
    CJsonDictKeysObject *saved_dk = sc->shared_keys;
    uint32_t expected_len = sc->len;
    uint8_t saved_psize = sc->prefix_size;
    uint8_t saved_prefix[32];
    memcpy(saved_prefix, sc->values_prefix, saved_psize);
    saved_dk->dk_refcnt++;

    PyObject *split_vals[CJSON_SPLIT_MAX_KEYS];
    uint32_t parsed = 0;
    Py_hash_t kh = 0;
    uint8_t pred_vkind = 0;

    while (parsed < expected_len) {
      const char *kstart = p + 1;
      const SchemaKey *sk = &sc->keys[parsed];
      const char *kp = kstart;
      if (CJSON_UNLIKELY(sc->shared_keys != saved_dk || !schema_match_key_only(sk, &kp, end))) {
        goto split_fallback;
      }
      kh = sk->py_hash;
      pred_vkind = sk->val_kind;
      p = kp;

      if (p < end && *p == ' ') p++;
      if (CJSON_UNLIKELY(p >= end || *p != ':')) {
        p = skip_ws(p, end);
        if (CJSON_UNLIKELY(p >= end || *p != ':')) {
          for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
          cjson_free_shared_keys(saved_dk);
          raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "expected ':' after key", p);
          return NULL;
        }
      }
      p++;
    split_have_colon:;
      if (p < end && *p == ' ') p++;
      if (CJSON_UNLIKELY(p >= end || (uint8_t)*p <= 0x20)) {
        p = skip_ws(p, end);
        if (CJSON_UNLIKELY(p >= end)) {
          for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
          cjson_free_shared_keys(saved_dk);
          raise_decode_error(jp, "unexpected end of data", p);
          return NULL;
        }
      }

      PyObject *val;
      char vc = *p;
      if (pred_vkind == 1 && (vc == '-' || (uint32_t)(vc - '0') < 10)) {
        val = parse_number(jp, &p, end);
      } else if (vc == '"') {
        p++;
        val = parse_string_fast(jp, &p, end);
      } else if (vc == '-' || (uint32_t)(vc - '0') < 10) {
        val = parse_number(jp, &p, end);
      } else if (vc == '{') {
        if (CJSON_UNLIKELY(depth + 1 >= CJSON_REC_PARSE_DEPTH)) {
          val = parse_document_iter(jp, &p, end, depth);
        } else {
          p++;
          uint32_t child_slot = (uint32_t)((uint64_t)kh & 63u);
          val = parse_object_rec(jp, &p, end, depth + 1, child_slot);
        }
      } else if (vc == '[') {
        if (CJSON_UNLIKELY(depth + 1 >= CJSON_REC_PARSE_DEPTH)) {
          val = parse_document_iter(jp, &p, end, depth);
        } else {
          p++;
          uint32_t child_slot = (uint32_t)((uint64_t)kh & 63u);
          val = parse_array_rec(jp, &p, end, depth + 1, child_slot);
        }
      } else if (vc == 't' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "true", 4) == 0)) {
        p += 4;
        Py_INCREF(Py_True);
        val = Py_True;
      } else if (vc == 'f' && CJSON_LIKELY(p + 5 <= end && memcmp(p + 1, "alse", 4) == 0)) {
        p += 5;
        Py_INCREF(Py_False);
        val = Py_False;
      } else if (vc == 'n' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "null", 4) == 0)) {
        p += 4;
        Py_INCREF(Py_None);
        val = Py_None;
      } else {
        for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
        cjson_free_shared_keys(saved_dk);
        raise_unexpected_value_error(jp, p, end);
        return NULL;
      }

      if (CJSON_UNLIKELY(val == NULL)) {
        for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
        cjson_free_shared_keys(saved_dk);
        return NULL;
      }
      split_vals[parsed++] = val;

      if (parsed < expected_len) {
        /* Fused 1-compare match for ',"key":' (1..5B keys on minified JSON) */
        if (CJSON_LIKELY(sc->shared_keys == saved_dk && p + 9 <= end)) {
          const SchemaKey *nsk = &sc->keys[parsed];
          uint8_t nkl = nsk->klen;
          if (nkl <= 5) {
            uint64_t fw;
            memcpy(&fw, p, 8);
            const SchemaFused *sf = &sc->fused[parsed];
            if (nkl <= 4) {
              if (CJSON_LIKELY(((fw ^ sf->fused_w0) & sf->fused_mask) == 0)) {
                p += nkl + 4;
                kh = nsk->py_hash;
                pred_vkind = nsk->val_kind;
                goto split_have_colon;
              }
            } else if (CJSON_LIKELY(fw == sf->fused_w0 && p[8] == ':')) {
              p += 9;
              kh = nsk->py_hash;
              pred_vkind = nsk->val_kind;
              goto split_have_colon;
            }
          }
        }
        if (CJSON_LIKELY(p < end && *p == ',')) {
          p++;
          if (CJSON_LIKELY(p < end && *p == '"')) continue;
          p = skip_ws(p, end);
          if (CJSON_UNLIKELY(p >= end || *p == '}')) {
            for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
            cjson_free_shared_keys(saved_dk);
            raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                               (p >= end) ? p : (p - 1));
            return NULL;
          }
          if (CJSON_UNLIKELY(*p != '"')) {
            for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
            cjson_free_shared_keys(saved_dk);
            raise_decode_error(jp, "expected string key in object", p);
            return NULL;
          }
          continue;
        }
        if (p < end && *p == '}') {
          /* Object ended early with fewer keys than schema: materialize combined dict */
          p++;
          goto split_early_close;
        }
        p = skip_ws(p, end);
        if (CJSON_UNLIKELY(p >= end)) {
          for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
          cjson_free_shared_keys(saved_dk);
          raise_decode_error(jp, "unexpected end of data", p);
          return NULL;
        }
        if (*p == ',') {
          p = skip_ws(p + 1, end);
          if (CJSON_UNLIKELY(p >= end || *p == '}')) {
            for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
            cjson_free_shared_keys(saved_dk);
            raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                               (p >= end) ? p : (p - 1));
            return NULL;
          }
          if (CJSON_UNLIKELY(*p != '"')) {
            for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
            cjson_free_shared_keys(saved_dk);
            raise_decode_error(jp, "expected string key in object", p);
            return NULL;
          }
          continue;
        }
        if (*p == '}') {
          p++;
          goto split_early_close;
        }
        for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
        cjson_free_shared_keys(saved_dk);
        raise_decode_error(jp, "expected ',' or '}' in object", p);
        return NULL;
      } else {
        /* All expected_len keys matched! Check for closing '}' */
        if (CJSON_LIKELY(p < end && *p == '}')) {
          p++;
        } else {
          p = skip_ws(p, end);
          if (CJSON_UNLIKELY(p >= end)) {
            for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
            cjson_free_shared_keys(saved_dk);
            raise_decode_error(jp, "unexpected end of data", p);
            return NULL;
          }
          if (*p == '}') {
            p++;
          } else if (*p == ',') {
            /* Object has extra keys beyond schema: materialize combined dict and continue */
            p = skip_ws(p + 1, end);
            if (CJSON_UNLIKELY(p >= end || *p == '}')) {
              for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
              cjson_free_shared_keys(saved_dk);
              raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                                 (p >= end) ? p : (p - 1));
              return NULL;
            }
            if (CJSON_UNLIKELY(*p != '"')) {
              for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
              cjson_free_shared_keys(saved_dk);
              raise_decode_error(jp, "expected string key in object", p);
              return NULL;
            }
            goto split_fallback;
          } else {
            for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
            cjson_free_shared_keys(saved_dk);
            raise_decode_error(jp, "expected ',' or '}' in object", p);
            return NULL;
          }
        }

        /* Fast split-table dict creation: transfers saved_dk ref directly */
        PyObject *sdict = PyDict_New();
        if (CJSON_UNLIKELY(sdict == NULL)) {
          for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
          cjson_free_shared_keys(saved_dk);
          return NULL;
        }
#if defined(CJSON_SPLIT_DICT_313)
        size_t alloc_sz = offsetof(CJsonDictValues313, values) +
                          (size_t)expected_len * sizeof(PyObject *) +
                          (size_t)expected_len;
        CJsonDictValues313 *dv = (CJsonDictValues313 *)PyMem_Malloc(alloc_sz);
        if (CJSON_UNLIKELY(dv == NULL)) {
          Py_DECREF(sdict);
          for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
          cjson_free_shared_keys(saved_dk);
          PyErr_NoMemory();
          return NULL;
        }
        dv->capacity = (uint8_t)expected_len;
        dv->size = (uint8_t)expected_len;
        dv->embedded = 0;
        dv->valid = 1;
        memcpy(dv->values, split_vals, (size_t)expected_len * sizeof(PyObject *));
        memcpy((uint8_t *)&dv->values[expected_len], saved_prefix, (size_t)expected_len);
        PyDictObject *mp = (PyDictObject *)sdict;
        mp->ma_keys = (PyDictKeysObject *)saved_dk;
        mp->ma_values = (PyDictValues *)dv;
        mp->ma_used = (Py_ssize_t)expected_len;
#else
        char *raw = (char *)PyMem_Malloc((size_t)saved_psize + (size_t)expected_len * sizeof(PyObject *));
        if (CJSON_UNLIKELY(raw == NULL)) {
          Py_DECREF(sdict);
          for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
          cjson_free_shared_keys(saved_dk);
          PyErr_NoMemory();
          return NULL;
        }
        memcpy(raw, saved_prefix, saved_psize);
        PyObject **dst_vals = (PyObject **)(raw + saved_psize);
        memcpy(dst_vals, split_vals, (size_t)expected_len * sizeof(PyObject *));
        PyDictObject *mp = (PyDictObject *)sdict;
        mp->ma_keys = (PyDictKeysObject *)saved_dk;
        mp->ma_values = (PyDictValues *)dst_vals;
        mp->ma_used = (Py_ssize_t)expected_len;
#endif
        /* PyDict_New() returns an untracked empty dict on Python 3.12/3.13;
         * mirror CPython's MAINTAIN_TRACKING rule since we bypassed PyDict_SetItem. */
        for (uint32_t j = 0; j < (uint32_t)expected_len; j++) {
          if (PyObject_IS_GC(split_vals[j])) {
            if (!PyObject_GC_IsTracked(sdict)) {
              PyObject_GC_Track(sdict);
            }
            break;
          }
        }
        *p_io = p;
        return sdict;
      }
    }

  split_early_close:;
    {
      dict = (parsed <= 5) ? PyDict_New() : _PyDict_NewPresized((Py_ssize_t)parsed);
      CJsonDictUnicodeEntry *ep =
          (CJsonDictUnicodeEntry *)(saved_dk->dk_indices + ((size_t)1 << saved_dk->dk_log2_index_bytes));
      if (CJSON_UNLIKELY(dict == NULL)) {
        for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
        cjson_free_shared_keys(saved_dk);
        return NULL;
      }
      for (uint32_t j = 0; j < parsed; j++) {
        PyObject *k = ep[j].me_key;
        int rc = _PyDict_SetItem_KnownHash(dict, k, split_vals[j], PyObject_Hash(k));
        Py_DECREF(split_vals[j]);
        if (CJSON_UNLIKELY(rc < 0)) {
          for (uint32_t r = j + 1; r < parsed; r++) Py_DECREF(split_vals[r]);
          cjson_free_shared_keys(saved_dk);
          Py_DECREF(dict);
          return NULL;
        }
      }
      cjson_free_shared_keys(saved_dk);
      *p_io = p;
      return dict;
    }

  split_fallback:;
    {
      dict = (expected_len <= 5) ? PyDict_New() : _PyDict_NewPresized((Py_ssize_t)expected_len);
      CJsonDictUnicodeEntry *ep =
          (CJsonDictUnicodeEntry *)(saved_dk->dk_indices + ((size_t)1 << saved_dk->dk_log2_index_bytes));
      if (CJSON_UNLIKELY(dict == NULL)) {
        for (uint32_t j = 0; j < parsed; j++) Py_DECREF(split_vals[j]);
        cjson_free_shared_keys(saved_dk);
        return NULL;
      }
      for (uint32_t j = 0; j < parsed; j++) {
        PyObject *k = ep[j].me_key;
        int rc = _PyDict_SetItem_KnownHash(dict, k, split_vals[j], PyObject_Hash(k));
        Py_DECREF(split_vals[j]);
        if (CJSON_UNLIKELY(rc < 0)) {
          for (uint32_t r = j + 1; r < parsed; r++) Py_DECREF(split_vals[r]);
          cjson_free_shared_keys(saved_dk);
          Py_DECREF(dict);
          return NULL;
        }
      }
      cjson_free_shared_keys(saved_dk);
      pred_idx = parsed;
    }
  } else {
    dict = (sc->len <= 5) ? PyDict_New() : _PyDict_NewPresized((Py_ssize_t)sc->len);
  }
#else
  dict = (sc->len <= 5) ? PyDict_New() : _PyDict_NewPresized((Py_ssize_t)sc->len);
#endif
#else
  (void)schema_slot;
  PyObject *dict = PyDict_New();
#endif
  if (CJSON_UNLIKELY(dict == NULL)) return NULL;

  while (1) {
    p++;
    Py_hash_t kh;
    PyObject *key;
#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint8_t pred_vkind = 0;
    if (CJSON_LIKELY(pred_idx < sc->len)) {
      const SchemaKey *sk = &sc->keys[pred_idx];
      key = schema_match_fast(sk, &p, end, &kh);
      if (CJSON_LIKELY(key != NULL)) {
        pred_vkind = sk->val_kind;
        pred_idx++;
      } else {
        key = parse_dict_key_schema_miss(jp, &p, end, &kh, sc, &pred_idx, &schema_dirty);
      }
    } else {
      key = parse_dict_key_schema_miss(jp, &p, end, &kh, sc, &pred_idx, &schema_dirty);
    }
#else
    key = parse_dict_key(jp, &p, end, &kh);
#endif
    if (CJSON_UNLIKELY(key == NULL)) {
      Py_DECREF(dict);
      return NULL;
    }

    if (p < end && *p == ' ') p++;
    if (CJSON_UNLIKELY(p >= end || *p != ':')) {
      p = skip_ws(p, end);
      if (CJSON_UNLIKELY(p >= end || *p != ':')) {
        Py_DECREF(key);
        Py_DECREF(dict);
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "expected ':' after key", p);
        return NULL;
      }
    }
    p++;
    if (p < end && *p == ' ') p++;
    if (CJSON_UNLIKELY(p >= end || (uint8_t)*p <= 0x20)) {
      p = skip_ws(p, end);
      if (CJSON_UNLIKELY(p >= end)) {
        Py_DECREF(key);
        Py_DECREF(dict);
        raise_decode_error(jp, "unexpected end of data", p);
        return NULL;
      }
    }

    PyObject *val;
    char vc = *p;
#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (pred_vkind == 1 && (vc == '-' || (uint32_t)(vc - '0') < 10)) {
      val = parse_number(jp, &p, end);
    } else
#endif
    if (vc == '"') {
      p++;
      val = parse_string_fast(jp, &p, end);
    } else if (vc == '-' || (uint32_t)(vc - '0') < 10) {
      val = parse_number(jp, &p, end);
    } else if (vc == '{') {
      if (CJSON_UNLIKELY(depth + 1 >= CJSON_REC_PARSE_DEPTH)) {
        val = parse_document_iter(jp, &p, end, depth);
      } else {
        p++;
        uint32_t child_slot = (uint32_t)((uint64_t)kh & 63u);
        val = parse_object_rec(jp, &p, end, depth + 1, child_slot);
      }
    } else if (vc == '[') {
      if (CJSON_UNLIKELY(depth + 1 >= CJSON_REC_PARSE_DEPTH)) {
        val = parse_document_iter(jp, &p, end, depth);
      } else {
        p++;
        uint32_t child_slot = (uint32_t)((uint64_t)kh & 63u);
        val = parse_array_rec(jp, &p, end, depth + 1, child_slot);
      }
    } else if (vc == 't' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "true", 4) == 0)) {
      p += 4;
      Py_INCREF(Py_True);
      val = Py_True;
    } else if (vc == 'f' && CJSON_LIKELY(p + 5 <= end && memcmp(p + 1, "alse", 4) == 0)) {
      p += 5;
      Py_INCREF(Py_False);
      val = Py_False;
    } else if (vc == 'n' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "null", 4) == 0)) {
      p += 4;
      Py_INCREF(Py_None);
      val = Py_None;
    } else {
      Py_DECREF(key);
      Py_DECREF(dict);
      raise_unexpected_value_error(jp, p, end);
      return NULL;
    }

    if (CJSON_UNLIKELY(val == NULL)) {
      Py_DECREF(key);
      Py_DECREF(dict);
      return NULL;
    }

    int rc = _PyDict_SetItem_KnownHash(dict, key, val, kh);
    Py_DECREF(key);
    Py_DECREF(val);
    if (CJSON_UNLIKELY(rc < 0)) {
      Py_DECREF(dict);
      return NULL;
    }

    if (CJSON_LIKELY(p < end && *p == ',')) {
      p++;
      if (p < end && *p == '"') {
        continue;
      }
      p = skip_ws(p, end);
      if (CJSON_UNLIKELY(p >= end || *p == '}')) {
        Py_DECREF(dict);
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                           (p >= end) ? p : (p - 1));
        return NULL;
      }
      if (CJSON_UNLIKELY(*p != '"')) {
        Py_DECREF(dict);
        raise_decode_error(jp, "expected string key in object", p);
        return NULL;
      }
      continue;
    }
    if (p < end && *p == '}') {
      p++;
      break;
    }
    p = skip_ws(p, end);
    if (CJSON_UNLIKELY(p >= end)) {
      Py_DECREF(dict);
      raise_decode_error(jp, "unexpected end of data", p);
      return NULL;
    }
    if (*p == ',') {
      p = skip_ws(p + 1, end);
      if (CJSON_UNLIKELY(p >= end || *p == '}')) {
        Py_DECREF(dict);
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                           (p >= end) ? p : (p - 1));
        return NULL;
      }
      if (CJSON_UNLIKELY(*p != '"')) {
        Py_DECREF(dict);
        raise_decode_error(jp, "expected string key in object", p);
        return NULL;
      }
      continue;
    }
    if (*p == '}') {
      p++;
      break;
    }
    Py_DECREF(dict);
    raise_decode_error(jp, "expected ',' or '}' in object", p);
    return NULL;
  }

#if !defined(Py_GIL_DISABLED) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  if (CJSON_UNLIKELY(schema_dirty)) {
    sc->stable_hits = 0;
    if (sc->miss_streak < 255) sc->miss_streak++;
#if defined(CJSON_SPLIT_DICT_FAST)
    if (sc->shared_keys != NULL) {
      cjson_free_shared_keys(sc->shared_keys);
      sc->shared_keys = NULL;
      sc->split_ok = 0;
    }
#endif
    if (sc->miss_streak <= 4 || (sc->miss_streak & 15u) == 0) {
      schema_record_from_dict(sc, dict);
    }
  } else {
    sc->miss_streak = 0;
#if defined(CJSON_SPLIT_DICT_FAST)
    if (g_split_dict_enabled && !sc->split_ok && sc->len >= 1 && sc->len <= CJSON_SPLIT_MAX_KEYS &&
        PyDict_GET_SIZE(dict) == (Py_ssize_t)sc->len) {
      if (++sc->stable_hits >= 2) {
        schema_promote_to_split(sc);
      }
    }
#endif
  }
#endif
  *p_io = p;
  return dict;
}

static PyObject *parse_array_rec(
    JsonParser *jp, const char **p_io, const char *end, int depth, uint32_t schema_slot) {
  const char *p = skip_ws(*p_io, end);
  if (CJSON_UNLIKELY(p >= end)) {
    raise_decode_error(jp, "unexpected end of data", p);
    return NULL;
  }
  if (*p == ']') {
    *p_io = p + 1;
    return PyList_New(0);
  }

  size_t base = jp->item_sp;

dispatch_item:;
  for (;;) {
    if (CJSON_UNLIKELY(p >= end)) {
      raise_decode_error(jp, "unexpected end of data", p);
      return NULL;
    }
    char c = *p;

    /* Tight homogeneous inner loop for number arrays */
    if (c == '-' || (uint32_t)(c - '0') < 10) {
      size_t sp = jp->item_sp;
      size_t cap = jp->item_cap;
      PyObject **items = jp->items;
      while (1) {
        PyObject *v = parse_number(jp, &p, end);
        if (CJSON_UNLIKELY(v == NULL)) {
          jp->item_sp = sp;
          return NULL;
        }
        if (CJSON_UNLIKELY(sp >= cap)) {
          jp->item_sp = sp;
          if (parser_grow_items(jp) < 0) {
            Py_DECREF(v);
            return NULL;
          }
          cap = jp->item_cap;
          items = jp->items;
        }
        items[sp++] = v;

        if (CJSON_LIKELY(p < end && *p == ',')) {
          p++;
          if (p < end && *p == ' ') p++;
          if (p < end && (*p == '-' || (uint32_t)(*p - '0') < 10)) {
            continue;
          }
          p = skip_ws(p, end);
          if (CJSON_UNLIKELY(p >= end || *p == ']')) {
            jp->item_sp = sp;
            raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                               (p >= end) ? p : (p - 1));
            return NULL;
          }
          if (*p == '-' || (uint32_t)(*p - '0') < 10) {
            continue;
          }
          jp->item_sp = sp;
          goto dispatch_item;
        }
        jp->item_sp = sp;
        goto after_item;
      }
    }

    /* Tight homogeneous inner loop for string arrays */
    if (c == '"') {
      while (1) {
        p++;
        PyObject *v = parse_string_fast(jp, &p, end);
        if (CJSON_UNLIKELY(v == NULL)) return NULL;
        if (CJSON_UNLIKELY(jp->item_sp >= jp->item_cap)) {
          if (parser_grow_items(jp) < 0) {
            Py_DECREF(v);
            return NULL;
          }
        }
        jp->items[jp->item_sp++] = v;

        if (CJSON_LIKELY(p < end && *p == ',')) {
          p++;
          if (p < end && *p == ' ') p++;
          if (p < end && *p == '"') {
            continue;
          }
          p = skip_ws(p, end);
          if (CJSON_UNLIKELY(p >= end || *p == ']')) {
            raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                               (p >= end) ? p : (p - 1));
            return NULL;
          }
          if (*p == '"') {
            continue;
          }
          goto dispatch_item;
        }
        goto after_item;
      }
    }

    PyObject *val;
    if (c == '{') {
      if (CJSON_UNLIKELY(depth + 1 >= CJSON_REC_PARSE_DEPTH)) {
        val = parse_document_iter(jp, &p, end, depth);
      } else {
        p++;
        val = parse_object_rec(jp, &p, end, depth + 1, schema_slot);
      }
    } else if (c == '[') {
      if (CJSON_UNLIKELY(depth + 1 >= CJSON_REC_PARSE_DEPTH)) {
        val = parse_document_iter(jp, &p, end, depth);
      } else if (p + 2 < end && (p[1] == '-' || (uint32_t)(p[1] - '0') < 10)) {
        /* Fast path for tiny 2..4 element numeric arrays ([lon, lat], [x, y, z], etc.) */
        const char *q = p + 1;
        PyObject *v0 = parse_number(jp, &q, end);
        if (CJSON_UNLIKELY(v0 == NULL)) return NULL;
        if (CJSON_LIKELY(q < end && *q == ',')) {
          q++;
          if (q < end && *q == ' ') q++;
          if (CJSON_LIKELY(q < end && (*q == '-' || (uint32_t)(*q - '0') < 10))) {
            PyObject *v1 = parse_number(jp, &q, end);
            if (CJSON_UNLIKELY(v1 == NULL)) {
              Py_DECREF(v0);
              return NULL;
            }
            if (CJSON_LIKELY(q < end && *q == ']')) {
              val = PyList_New(2);
              if (CJSON_UNLIKELY(val == NULL)) {
                Py_DECREF(v0);
                Py_DECREF(v1);
                return NULL;
              }
              PyList_SET_ITEM(val, 0, v0);
              PyList_SET_ITEM(val, 1, v1);
              p = q + 1;
              goto push_val;
            }
            if (q < end && *q == ',') {
              q++;
              if (q < end && *q == ' ') q++;
              if (q < end && (*q == '-' || (uint32_t)(*q - '0') < 10)) {
                PyObject *v2 = parse_number(jp, &q, end);
                if (CJSON_UNLIKELY(v2 == NULL)) {
                  Py_DECREF(v0);
                  Py_DECREF(v1);
                  return NULL;
                }
                if (q < end && *q == ']') {
                  val = PyList_New(3);
                  if (CJSON_UNLIKELY(val == NULL)) {
                    Py_DECREF(v0);
                    Py_DECREF(v1);
                    Py_DECREF(v2);
                    return NULL;
                  }
                  PyList_SET_ITEM(val, 0, v0);
                  PyList_SET_ITEM(val, 1, v1);
                  PyList_SET_ITEM(val, 2, v2);
                  p = q + 1;
                  goto push_val;
                }
                Py_DECREF(v2);
              }
            }
            Py_DECREF(v1);
          }
        }
        Py_DECREF(v0);
        p++;
        val = parse_array_rec(jp, &p, end, depth + 1, schema_slot);
      } else {
        p++;
        val = parse_array_rec(jp, &p, end, depth + 1, schema_slot);
      }
    } else if (c == 't' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "true", 4) == 0)) {
      p += 4;
      Py_INCREF(Py_True);
      val = Py_True;
    } else if (c == 'f' && CJSON_LIKELY(p + 5 <= end && memcmp(p + 1, "alse", 4) == 0)) {
      p += 5;
      Py_INCREF(Py_False);
      val = Py_False;
    } else if (c == 'n' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "null", 4) == 0)) {
      p += 4;
      Py_INCREF(Py_None);
      val = Py_None;
    } else {
      raise_unexpected_value_error(jp, p, end);
      return NULL;
    }

  push_val:;
    if (CJSON_UNLIKELY(val == NULL)) return NULL;
    if (CJSON_UNLIKELY(jp->item_sp >= jp->item_cap)) {
      if (parser_grow_items(jp) < 0) {
        Py_DECREF(val);
        return NULL;
      }
    }
    jp->items[jp->item_sp++] = val;

  after_item:;
    if (CJSON_LIKELY(p < end && *p == ',')) {
      p++;
      if (p < end && *p == ' ') p++;
      if (p < end && (uint8_t)*p > 0x20 && *p != ']') {
        continue;
      }
      p = skip_ws(p, end);
      if (CJSON_UNLIKELY(p >= end || *p == ']')) {
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                           (p >= end) ? p : (p - 1));
        return NULL;
      }
      continue;
    }
    if (p < end && *p == ']') {
      p++;
      break;
    }
    p = skip_ws(p, end);
    if (CJSON_UNLIKELY(p >= end)) {
      raise_decode_error(jp, "unexpected end of data", p);
      return NULL;
    }
    if (*p == ',') {
      p = skip_ws(p + 1, end);
      if (CJSON_UNLIKELY(p >= end || *p == ']')) {
        raise_decode_error(jp, (p >= end) ? "unexpected end of data" : "trailing comma is not allowed",
                           (p >= end) ? p : (p - 1));
        return NULL;
      }
      continue;
    }
    if (*p == ']') {
      p++;
      break;
    }
    raise_decode_error(jp, "expected ',' or ']' in array", p);
    return NULL;
  }

  Py_ssize_t count = (Py_ssize_t)(jp->item_sp - base);
  PyObject *list = PyList_New(count);
  if (CJSON_UNLIKELY(list == NULL)) return NULL;
  PyObject **src = &jp->items[base];
  for (Py_ssize_t i = 0; i < count; i++) {
    PyList_SET_ITEM(list, i, src[i]);
  }
  jp->item_sp = base;
  *p_io = p;
  return list;
}

static inline PyObject *parse_value_rec(JsonParser *jp, const char **p_io, const char *end) {
  const char *p = *p_io;
  if (CJSON_UNLIKELY(p >= end)) {
    raise_decode_error(jp, "unexpected end of data", p);
    return NULL;
  }
  char c = *p;
  if (c == '{') {
    p++;
    PyObject *res = parse_object_rec(jp, &p, end, 1, 0);
    *p_io = p;
    return res;
  }
  if (c == '[') {
    p++;
    PyObject *res = parse_array_rec(jp, &p, end, 1, 0);
    *p_io = p;
    return res;
  }
  if (c == '"') {
    p++;
    PyObject *res = parse_string_fast(jp, &p, end);
    *p_io = p;
    return res;
  }
  if (c == '-' || (uint32_t)(c - '0') < 10) {
    PyObject *res = parse_number(jp, &p, end);
    *p_io = p;
    return res;
  }
  if (c == 't' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "true", 4) == 0)) {
    *p_io = p + 4;
    Py_INCREF(Py_True);
    return Py_True;
  }
  if (c == 'f' && CJSON_LIKELY(p + 5 <= end && memcmp(p + 1, "alse", 4) == 0)) {
    *p_io = p + 5;
    Py_INCREF(Py_False);
    return Py_False;
  }
  if (c == 'n' && CJSON_LIKELY(p + 4 <= end && memcmp(p, "null", 4) == 0)) {
    *p_io = p + 4;
    Py_INCREF(Py_None);
    return Py_None;
  }
  raise_unexpected_value_error(jp, p, end);
  return NULL;
}

PyObject *c_json_loads(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
  (void)self;
  if (CJSON_UNLIKELY(nargs != 1)) {
    PyErr_SetString(PyExc_TypeError, "loads() takes exactly 1 positional argument");
    return NULL;
  }

  PyObject *obj = args[0];
  const char *data = NULL;
  Py_ssize_t len = 0;
  Py_buffer view;
  int has_view = 0;

  if (PyBytes_Check(obj)) {
    data = PyBytes_AS_STRING(obj);
    len = PyBytes_GET_SIZE(obj);
  } else if (PyUnicode_Check(obj)) {
    if (CJSON_UNLIKELY(check_str_surrogates(obj) < 0)) {
      JsonParser jp;
      parser_init(&jp, "", 0);
      raise_decode_error(&jp, "str is not valid UTF-8: surrogates not allowed", "");
      return NULL;
    }
    if (PyUnicode_IS_COMPACT_ASCII(obj)) {
      data = (const char *)PyUnicode_1BYTE_DATA(obj);
      len = PyUnicode_GET_LENGTH(obj);
    } else {
      data = PyUnicode_AsUTF8AndSize(obj, &len);
      if (CJSON_UNLIKELY(data == NULL)) {
        JsonParser jp;
        parser_init(&jp, "", 0);
        raise_decode_error(&jp, "str is not valid UTF-8: surrogates not allowed", "");
        return NULL;
      }
    }
  } else if (PyByteArray_Check(obj)) {
    data = PyByteArray_AS_STRING(obj);
    len = PyByteArray_GET_SIZE(obj);
  } else if (PyMemoryView_Check(obj)) {
    if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) < 0) {
      PyErr_Clear();
      JsonParser jp;
      parser_init(&jp, "", 0);
      raise_decode_error(&jp, "Input must be bytes, bytearray, memoryview, or str", "");
      return NULL;
    }
    if (!PyBuffer_IsContiguous(&view, 'C')) {
      PyBuffer_Release(&view);
      JsonParser jp;
      parser_init(&jp, "", 0);
      raise_decode_error(&jp, "memoryview must be C-contiguous", "");
      return NULL;
    }
    data = (const char *)view.buf;
    len = view.len;
    has_view = 1;
  } else {
    JsonParser jp;
    parser_init(&jp, "", 0);
    raise_decode_error(&jp, "Input must be bytes, bytearray, memoryview, or str", "");
    return NULL;
  }

  JsonParser jp;
  parser_init(&jp, data, len);

  if (CJSON_UNLIKELY(len == 0)) {
    if (has_view) PyBuffer_Release(&view);
    raise_decode_error(&jp, "Input is a zero-length, empty document", data);
    return NULL;
  }

  const char *p = skip_ws(data, data + len);
  if (CJSON_UNLIKELY(p >= data + len)) {
    if (has_view) PyBuffer_Release(&view);
    raise_decode_error(&jp, "input data is empty", data);
    return NULL;
  }

  PyObject *result = parse_value_rec(&jp, &p, data + len);
  if (CJSON_LIKELY(result != NULL)) {
    const char *trail = skip_ws(p, data + len);
    if (CJSON_UNLIKELY(trail < data + len)) {
      Py_DECREF(result);
      result = NULL;
      raise_decode_error(&jp, "unexpected content after document", trail);
    }
  }

  parser_cleanup(&jp);
  if (has_view) {
    PyBuffer_Release(&view);
  }
  return result;
}

#if defined(CJSON_SPLIT_DICT_FAST)
static void verify_split_dict_selfcheck(void) {
  const char *probe = "{\"_cx_a\":1,\"_cx_b\":[2,3]}";
  Py_ssize_t plen = (Py_ssize_t)strlen(probe);
  PyObject *d = NULL;
  for (int i = 0; i < 4; i++) {
    Py_XDECREF(d);
    JsonParser jp;
    parser_init(&jp, probe, plen);
    const char *p = probe;
    d = parse_value_rec(&jp, &p, probe + plen);
    parser_cleanup(&jp);
    if (d == NULL) {
      PyErr_Clear();
      g_split_dict_enabled = 0;
      return;
    }
  }
  int ok = 1;
  if (!PyDict_CheckExact(d) || PyDict_GET_SIZE(d) != 2) ok = 0;
  if (ok && !PyObject_GC_IsTracked(d)) ok = 0;
  PyObject *va = ok ? PyDict_GetItemString(d, "_cx_a") : NULL;
  PyObject *vb = ok ? PyDict_GetItemString(d, "_cx_b") : NULL;
  if (ok && (va == NULL || !PyLong_CheckExact(va) || PyLong_AsLong(va) != 1)) ok = 0;
  if (ok && (vb == NULL || !PyList_CheckExact(vb) || PyList_GET_SIZE(vb) != 2)) ok = 0;
  if (ok) {
    PyObject *cp = PyDict_Copy(d);
    if (cp == NULL || PyObject_RichCompareBool(d, cp, Py_EQ) != 1) ok = 0;
    Py_XDECREF(cp);
  }
  if (ok) {
    PyObject *r = PyObject_Repr(d);
    if (r == NULL) ok = 0;
    Py_XDECREF(r);
  }
  if (ok && PyDict_SetItemString(d, "_cx_c", Py_True) < 0) ok = 0;
  if (ok && PyDict_DelItemString(d, "_cx_a") < 0) ok = 0;
  if (ok && PyDict_GET_SIZE(d) != 2) ok = 0;
  Py_DECREF(d);
  PyErr_Clear();
  if (!ok) {
    g_split_dict_enabled = 0;
  }
}
#endif

int init_deserializer_types(void) {
  if (g_empty_str == NULL) {
    g_empty_str = PyUnicode_FromStringAndSize("", 0);
    if (g_empty_str == NULL) return -1;
    g_empty_str_hash = PyObject_Hash(g_empty_str);
    for (int i = 0; i < 128; i++) {
      char c = (char)i;
      g_ascii_chars[i] = PyUnicode_FromStringAndSize(&c, 1);
      if (g_ascii_chars[i] == NULL) return -1;
    }
    for (int i = -5; i <= 256; i++) {
      g_small_ints[i + 5] = PyLong_FromLong(i);
      if (g_small_ints[i + 5] == NULL) return -1;
    }
  }
  if (JSONDecodeError != NULL) {
    return 0;
  }
  PyObject *json_mod = PyImport_ImportModule("json");
  if (json_mod == NULL) return -1;
  PyObject *base_err = PyObject_GetAttrString(json_mod, "JSONDecodeError");
  Py_DECREF(json_mod);
  if (base_err == NULL) return -1;
  JSONDecodeError = PyErr_NewException("cxjson.JSONDecodeError", base_err, NULL);
  Py_DECREF(base_err);
  if (JSONDecodeError == NULL) return -1;
#if defined(CJSON_SPLIT_DICT_FAST)
  verify_split_dict_selfcheck();
#endif
  return 0;
}
