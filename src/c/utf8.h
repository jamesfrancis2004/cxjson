#ifndef CJSON_UTF8_H
#define CJSON_UTF8_H

#include "parser_state.h"

static inline PyObject *make_ascii_str(const char *start, Py_ssize_t len) {
  if (len == 0) {
    Py_INCREF(g_empty_str);
    return g_empty_str;
  }
  if (len == 1) {
    PyObject *ch = g_ascii_chars[(uint8_t)start[0]];
    Py_INCREF(ch);
    return ch;
  }
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED) && !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)
  if (CJSON_LIKELY(len <= 4096)) {
    PyASCIIObject *ascii = (PyASCIIObject *)PyObject_Malloc(sizeof(PyASCIIObject) + (size_t)len + 1u);
    if (CJSON_UNLIKELY(ascii == NULL)) return PyErr_NoMemory();
    Py_SET_REFCNT((PyObject *)ascii, 1);
    Py_SET_TYPE((PyObject *)ascii, &PyUnicode_Type);
    ascii->length = len;
    ascii->hash = -1;
    memset(&ascii->state, 0, sizeof(ascii->state));
    ascii->state.kind = PyUnicode_1BYTE_KIND;
    ascii->state.compact = 1;
    ascii->state.ascii = 1;
    char *dst = (char *)(ascii + 1);
    if (CJSON_LIKELY(len <= 16)) {
      copy_short_ascii(dst, start, (size_t)len);
    } else {
      memcpy(dst, start, (size_t)len);
    }
    dst[len] = '\0';
    return (PyObject *)ascii;
  }
#endif
  PyObject *u = PyUnicode_New(len, 127);
  if (CJSON_LIKELY(u != NULL)) {
    if (CJSON_LIKELY(len <= 16)) {
      copy_short_ascii((char *)PyUnicode_1BYTE_DATA(u), start, (size_t)len);
    } else {
      memcpy(PyUnicode_1BYTE_DATA(u), start, (size_t)len);
    }
  }
  return u;
}

/* Direct UTF-8 decoder: 1-pass stack path for len <= 512, 2-pass exact-sized path above */
__attribute__((noinline))
static PyObject *decode_utf8_fast(JsonParser *jp, const char *s, size_t len) {
  const uint8_t *p = (const uint8_t *)s;
  const uint8_t *end = p + len;

  if (CJSON_LIKELY(len <= 512)) {
    uint16_t tmp16[512];
    size_t nchars = 0;
    Py_UCS4 max_char = 0x7F;

    while (p < end) {
      uint8_t b0 = *p;
      if (b0 < 0x80) {
#if defined(CJSON_SIMD_NEON)
        while (p + 16 <= end) {
          uint8x16_t v = vld1q_u8(p);
          if (vmaxvq_u8(v) >= 0x80) break;
          vst1q_u16(tmp16 + nchars, vmovl_u8(vget_low_u8(v)));
          vst1q_u16(tmp16 + nchars + 8, vmovl_high_u8(v));
          nchars += 16;
          p += 16;
        }
        if (p >= end) break;
        b0 = *p;
        if (b0 >= 0x80) continue;
#elif defined(CJSON_SIMD_SSE2) || defined(CJSON_SIMD_AVX2)
        const __m128i z128 = _mm_setzero_si128();
        while (p + 16 <= end) {
          __m128i v = _mm_loadu_si128((const __m128i *)p);
          if (_mm_movemask_epi8(v) != 0) break;
          _mm_storeu_si128((__m128i *)(tmp16 + nchars), _mm_unpacklo_epi8(v, z128));
          _mm_storeu_si128((__m128i *)(tmp16 + nchars + 8), _mm_unpackhi_epi8(v, z128));
          nchars += 16;
          p += 16;
        }
        if (p >= end) break;
        b0 = *p;
        if (b0 >= 0x80) continue;
#endif
        tmp16[nchars++] = (uint16_t)b0;
        p++;
      } else if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (CJSON_UNLIKELY(p + 2 > end || (p[1] & 0xC0) != 0x80)) {
          raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
          return NULL;
        }
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        tmp16[nchars++] = (uint16_t)cp;
        if (cp > max_char) max_char = cp;
        p += 2;
      } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (CJSON_UNLIKELY(p + 3 > end || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                           (b0 == 0xE0 && p[1] < 0xA0) || (b0 == 0xED && p[1] >= 0xA0))) {
          raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
          return NULL;
        }
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
        tmp16[nchars++] = (uint16_t)cp;
        max_char = 0xFFFF;
        p += 3;
      } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (CJSON_UNLIKELY(p + 4 > end || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                           (p[3] & 0xC0) != 0x80 || (b0 == 0xF0 && p[1] < 0x90) ||
                           (b0 == 0xF4 && p[1] >= 0x90))) {
          raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
          return NULL;
        }
        uint32_t tmp32[512];
        for (size_t i = 0; i < nchars; i++) {
          tmp32[i] = tmp16[i];
        }
        tmp32[nchars++] = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                          ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        p += 4;
        while (p < end) {
          uint8_t c0 = *p;
          if (c0 < 0x80) {
            tmp32[nchars++] = (uint32_t)c0;
            p++;
          } else if (c0 >= 0xC2 && c0 <= 0xDF) {
            if (CJSON_UNLIKELY(p + 2 > end || (p[1] & 0xC0) != 0x80)) {
              raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
              return NULL;
            }
            tmp32[nchars++] = ((uint32_t)(c0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
            p += 2;
          } else if (c0 >= 0xE0 && c0 <= 0xEF) {
            if (CJSON_UNLIKELY(p + 3 > end || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                               (c0 == 0xE0 && p[1] < 0xA0) || (c0 == 0xED && p[1] >= 0xA0))) {
              raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
              return NULL;
            }
            tmp32[nchars++] = ((uint32_t)(c0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
            p += 3;
          } else if (c0 >= 0xF0 && c0 <= 0xF4) {
            if (CJSON_UNLIKELY(p + 4 > end || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                               (p[3] & 0xC0) != 0x80 || (c0 == 0xF0 && p[1] < 0x90) ||
                               (c0 == 0xF4 && p[1] >= 0x90))) {
              raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
              return NULL;
            }
            tmp32[nchars++] = ((uint32_t)(c0 & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                              ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
            p += 4;
          } else {
            raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
            return NULL;
          }
        }
        PyObject *u = PyUnicode_New((Py_ssize_t)nchars, 0x10FFFF);
        if (CJSON_LIKELY(u != NULL)) {
          memcpy(PyUnicode_4BYTE_DATA(u), tmp32, nchars * sizeof(Py_UCS4));
        }
        return u;
      } else {
        raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
        return NULL;
      }
    }

    if (CJSON_UNLIKELY(max_char <= 0x7F)) {
      return make_ascii_str(s, (Py_ssize_t)len);
    }
    PyObject *u = PyUnicode_New((Py_ssize_t)nchars, max_char);
    if (CJSON_UNLIKELY(u == NULL)) {
      return NULL;
    }
    if (max_char <= 0xFF) {
      Py_UCS1 *dst = PyUnicode_1BYTE_DATA(u);
      size_t i = 0;
#if defined(CJSON_SIMD_NEON)
      for (; i + 16 <= nchars; i += 16) {
        uint8x8_t lo = vmovn_u16(vld1q_u16(tmp16 + i));
        uint8x8_t hi = vmovn_u16(vld1q_u16(tmp16 + i + 8));
        vst1q_u8(dst + i, vcombine_u8(lo, hi));
      }
#elif defined(CJSON_SIMD_SSE2) || defined(CJSON_SIMD_AVX2)
      for (; i + 16 <= nchars; i += 16) {
        __m128i lo = _mm_loadu_si128((const __m128i *)(tmp16 + i));
        __m128i hi = _mm_loadu_si128((const __m128i *)(tmp16 + i + 8));
        _mm_storeu_si128((__m128i *)(dst + i), _mm_packus_epi16(lo, hi));
      }
#endif
      for (; i < nchars; i++) {
        dst[i] = (Py_UCS1)tmp16[i];
      }
    } else {
      memcpy(PyUnicode_2BYTE_DATA(u), tmp16, nchars * sizeof(Py_UCS2));
    }
    return u;
  }

  /* Pass 1 (> 512 bytes): RFC 3629 validation + char count + max_char */
  size_t nchars = 0;
  Py_UCS4 max_char = 0x7F;

  while (p < end) {
    uint8_t b0 = *p;
    if (b0 < 0x80) {
      p++;
      nchars++;
      while (p + 8 <= end) {
        uint64_t w;
        memcpy(&w, p, 8);
        if ((w & 0x8080808080808080ULL) == 0) {
          p += 8;
          nchars += 8;
          continue;
        }
        break;
      }
    } else if (b0 >= 0xC2 && b0 <= 0xDF) {
      if (CJSON_UNLIKELY(p + 2 > end || (p[1] & 0xC0) != 0x80)) {
        raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
        return NULL;
      }
      if (b0 >= 0xC4) {
        if (max_char < 0xFFFF) max_char = 0xFFFF;
      } else {
        if (max_char < 0xFF) max_char = 0xFF;
      }
      p += 2;
      nchars++;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
      if (CJSON_UNLIKELY(p + 3 > end || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                         (b0 == 0xE0 && p[1] < 0xA0) || (b0 == 0xED && p[1] >= 0xA0))) {
        raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
        return NULL;
      }
      if (max_char < 0xFFFF) max_char = 0xFFFF;
      p += 3;
      nchars++;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      if (CJSON_UNLIKELY(p + 4 > end || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                         (p[3] & 0xC0) != 0x80 || (b0 == 0xF0 && p[1] < 0x90) ||
                         (b0 == 0xF4 && p[1] >= 0x90))) {
        raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
        return NULL;
      }
      max_char = 0x10FFFF;
      p += 4;
      nchars++;
    } else {
      raise_decode_error(jp, "str is not valid UTF-8: surrogates not allowed", s);
      return NULL;
    }
  }

  if (CJSON_UNLIKELY(max_char <= 0x7F)) {
    return make_ascii_str(s, (Py_ssize_t)len);
  }

  PyObject *u = PyUnicode_New((Py_ssize_t)nchars, max_char);
  if (CJSON_UNLIKELY(u == NULL)) {
    return NULL;
  }

  p = (const uint8_t *)s;
  if (max_char <= 0xFF) {
    Py_UCS1 *dst = PyUnicode_1BYTE_DATA(u);
    while (p < end) {
      while (p + 8 <= end) {
        uint64_t w;
        memcpy(&w, p, 8);
        if ((w & 0x8080808080808080ULL) == 0) {
          memcpy(dst, p, 8);
          dst += 8;
          p += 8;
          continue;
        }
        break;
      }
      if (p >= end) break;
      uint8_t b0 = *p;
      if (b0 < 0x80) {
        *dst++ = (Py_UCS1)b0;
        p++;
      } else {
        *dst++ = (Py_UCS1)(((uint32_t)(b0 & 0x03) << 6) | (uint32_t)(p[1] & 0x3F));
        p += 2;
      }
    }
  } else if (max_char <= 0xFFFF) {
    Py_UCS2 *dst = PyUnicode_2BYTE_DATA(u);
#if defined(CJSON_SIMD_NEON)
    while (p + 16 <= end) {
      uint8x16_t v = vld1q_u8(p);
      if (vmaxvq_u8(v) >= 0x80) break;
      vst1q_u16(dst, vmovl_u8(vget_low_u8(v)));
      vst1q_u16(dst + 8, vmovl_high_u8(v));
      dst += 16;
      p += 16;
    }
#elif defined(CJSON_SIMD_SSE2) || defined(CJSON_SIMD_AVX2)
    const __m128i zero = _mm_setzero_si128();
    while (p + 16 <= end) {
      __m128i v = _mm_loadu_si128((const __m128i *)p);
      if (_mm_movemask_epi8(v) != 0) break;
      _mm_storeu_si128((__m128i *)dst, _mm_unpacklo_epi8(v, zero));
      _mm_storeu_si128((__m128i *)(dst + 8), _mm_unpackhi_epi8(v, zero));
      dst += 16;
      p += 16;
    }
#else
    while (p + 8 <= end) {
      uint64_t w;
      memcpy(&w, p, 8);
      if ((w & 0x8080808080808080ULL) != 0) break;
      for (int i = 0; i < 8; i++) dst[i] = (Py_UCS2)p[i];
      dst += 8;
      p += 8;
    }
#endif
    while (p < end) {
      uint8_t b0 = *p;
      if (b0 < 0x80) {
        *dst++ = (Py_UCS2)b0;
        p++;
      } else if (b0 < 0xE0) {
        *dst++ = (Py_UCS2)(((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F));
        p += 2;
      } else {
        *dst++ = (Py_UCS2)(((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F));
        p += 3;
      }
    }
  } else {
    Py_UCS4 *dst = PyUnicode_4BYTE_DATA(u);
    while (p < end) {
      uint8_t b0 = *p;
      if (b0 < 0x80) {
        *dst++ = (Py_UCS4)b0;
        p++;
      } else if (b0 < 0xE0) {
        *dst++ = (Py_UCS4)(((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F));
        p += 2;
      } else if (b0 < 0xF0) {
        *dst++ = (Py_UCS4)(((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F));
        p += 3;
      } else {
        *dst++ = (Py_UCS4)(((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                           ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F));
        p += 4;
      }
    }
  }
  return u;
}

static inline int check_str_surrogates(PyObject *str_obj) {
  if (PyUnicode_IS_ASCII(str_obj)) {
    return 0;
  }
  int kind = PyUnicode_KIND(str_obj);
  Py_ssize_t len = PyUnicode_GET_LENGTH(str_obj);
  if (kind == PyUnicode_1BYTE_KIND) {
    return 0;
  }
  if (kind == PyUnicode_2BYTE_KIND) {
    const Py_UCS2 *data = PyUnicode_2BYTE_DATA(str_obj);
    for (Py_ssize_t i = 0; i < len; i++) {
      Py_UCS2 ch = data[i];
      if (CJSON_UNLIKELY(ch >= 0xD800 && ch <= 0xDFFF)) {
        return -1;
      }
    }
    return 0;
  }
  if (kind == PyUnicode_4BYTE_KIND) {
    const Py_UCS4 *data = PyUnicode_4BYTE_DATA(str_obj);
    for (Py_ssize_t i = 0; i < len; i++) {
      Py_UCS4 ch = data[i];
      if (CJSON_UNLIKELY(ch >= 0xD800 && ch <= 0xDFFF)) {
        return -1;
      }
    }
    return 0;
  }
  return 0;
}

#endif /* CJSON_UTF8_H */
