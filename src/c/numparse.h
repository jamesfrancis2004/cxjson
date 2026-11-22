#ifndef CJSON_NUMPARSE_H
#define CJSON_NUMPARSE_H

#include "parser_state.h"
#include "pow10_table.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

static inline int eisel_lemire_double(uint64_t mant, int exp10, int is_neg,
                                      double *out_val) {
  if (mant == 0) {
    *out_val = is_neg ? -0.0 : 0.0;
    return 1;
  }
  if (exp10 < POW10_MIN_EXP || exp10 > POW10_MAX_EXP) {
    return 0;
  }
  const Pow10Entry *entry = &POW10_SIG_TABLE[exp10 - POW10_MIN_EXP];
  int lz = __builtin_clzll(mant);
  uint64_t w = mant << lz;

  uint64_t hi = 0;
  uint64_t lo = cjson_umul128(w, entry->hi, &hi);
  int upper = (int)(hi >> 63);
  int shift = upper + 9;
  uint64_t mask = (1ULL << (shift + 1)) - 1;
  uint64_t rem = hi & mask;
  uint64_t half = 1ULL << shift;

  if (CJSON_UNLIKELY(rem == half || rem == half - 1 || rem == mask)) {
    uint64_t p2_hi = 0;
    (void)cjson_umul128(w, entry->lo, &p2_hi);
    uint64_t new_lo = lo + p2_hi;
    if (new_lo < lo)
      hi += 1;
    lo = new_lo;
    upper = (int)(hi >> 63);
    shift = upper + 9;
    mask = (1ULL << (shift + 1)) - 1;
    rem = hi & mask;
    half = 1ULL << shift;
    if (rem == half && lo <= 1) {
      return 0;
    }
  }

  uint64_t mant53 = hi >> (upper + 10);
  if (rem >= half) {
    mant53 += 1;
    if (mant53 == (1ULL << 53)) {
      mant53 >>= 1;
      upper += 1;
    }
  }

  int biased_exp = entry->e2 + (63 - lz) + upper + 1023;
  if (CJSON_UNLIKELY(biased_exp <= 0 || biased_exp >= 2047)) {
    return 0;
  }

  uint64_t bits = ((uint64_t)is_neg << 63) | ((uint64_t)biased_exp << 52) |
                  (mant53 & ((1ULL << 52) - 1));
  memcpy(out_val, &bits, sizeof(double));
  return 1;
}

__attribute__((noinline)) static PyObject *
parse_float_fallback(JsonParser *jp, const char *num_start,
                     const char *num_end) {
  size_t len = (size_t)(num_end - num_start);
  char stack_buf[128];
  char *buf = stack_buf;
  if (len >= sizeof(stack_buf)) {
    buf = parser_ensure_scratch(jp, len + 1);
    if (!buf)
      return NULL;
  }
  memcpy(buf, num_start, len);
  buf[len] = '\0';
  char *endptr = NULL;
  double val = strtod(buf, &endptr);
  if (CJSON_UNLIKELY(isinf(val))) {
    raise_decode_error(jp, "number is infinity when parsed as double",
                       num_start);
    return NULL;
  }
  return PyFloat_FromDouble(val);
}

static const uint64_t POW10_U64[9] = {1ULL,       10ULL,       100ULL,
                                      1000ULL,    10000ULL,    100000ULL,
                                      1000000ULL, 10000000ULL, 100000000ULL};

/* Exact IEEE-754 double powers of 10 for Clinger's fast path (5^22 < 2^53). */
static const double POW10_DBL[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

/* Branchless SWAR helper: parse 1..max_digits ASCII digits from `p`. */
static inline int parse_up_to_8_digits_swar(const char *p, int max_digits,
                                            uint64_t *mant) {
  uint64_t chunk;
  memcpy(&chunk, p, 8);
  uint64_t val = chunk - 0x3030303030303030ULL;
  uint64_t check =
      ((chunk + 0x4646464646464646ULL) | val) & 0x8080808080808080ULL;
  int n = (check == 0) ? 8 : (__builtin_ctzll(check) >> 3);
  if (n > max_digits)
    n = max_digits;
  if (n == 0)
    return 0;
  val <<= (8 - n) * 8;
  val = (val * 2561ULL) >> 8;
  val = ((val & 0x00FF00FF00FF00FFULL) * 6553601ULL) >> 16;
  uint32_t d =
      (uint32_t)(((val & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32);
  *mant = (*mant) * POW10_U64[n] + d;
  return n;
}

/* Fast inline 1-digit & 2-digit PyLongObject constructor (avoids PyLong_FromLongLong@PLT). */
static inline PyObject *cjson_new_pylong_u64(uint64_t mag, int is_neg) {
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030F0000 && \
    PYLONG_BITS_IN_DIGIT == 30 && !defined(Py_GIL_DISABLED) && \
    !defined(Py_LIMITED_API) && !defined(PYPY_VERSION) && \
    !defined(Py_TRACE_REFS) && !defined(Py_REF_DEBUG) && !defined(Py_STATS)
  if (CJSON_UNLIKELY(mag == 0)) {
    return PyLong_FromLong(0);
  }
  if (CJSON_LIKELY(mag < (1ULL << 30))) {
    PyLongObject *lo = (PyLongObject *)PyObject_Malloc(
        offsetof(PyLongObject, long_value.ob_digit) + sizeof(digit));
    if (CJSON_UNLIKELY(lo == NULL)) return PyErr_NoMemory();
    /* Must assign ob_refcnt directly (NOT Py_SET_REFCNT, which skips writes
     * if uninitialized memory happens to look immortal!). */
    ((PyObject *)lo)->ob_refcnt = 1;
    ((PyObject *)lo)->ob_type = &PyLong_Type;
    lo->long_value.lv_tag = ((uintptr_t)1 << _PyLong_NON_SIZE_BITS) | (is_neg ? 2u : 0u);
    lo->long_value.ob_digit[0] = (digit)mag;
    return (PyObject *)lo;
  }
  if (CJSON_LIKELY(mag < (1ULL << 60))) {
    PyLongObject *lo = (PyLongObject *)PyObject_Malloc(
        offsetof(PyLongObject, long_value.ob_digit) + 2 * sizeof(digit));
    if (CJSON_UNLIKELY(lo == NULL)) return PyErr_NoMemory();
    ((PyObject *)lo)->ob_refcnt = 1;
    ((PyObject *)lo)->ob_type = &PyLong_Type;
    lo->long_value.lv_tag = ((uintptr_t)2 << _PyLong_NON_SIZE_BITS) | (is_neg ? 2u : 0u);
    lo->long_value.ob_digit[0] = (digit)(mag & PyLong_MASK);
    lo->long_value.ob_digit[1] = (digit)(mag >> PyLong_SHIFT);
    return (PyObject *)lo;
  }
#endif
  return PyLong_FromLongLong(is_neg ? -(long long)mag : (long long)mag);
}

__attribute__((noinline)) static PyObject *parse_number_slow(JsonParser *jp,
                                                             const char **p_io,
                                                             const char *start,
                                                             const char *end) {
  const char *p = start;
  int is_neg = 0;

  if (*p == '-') {
    is_neg = 1;
    p++;
  }

  uint64_t mant = 0;
  int int_digits = 0;

  if (*p == '0') {
    p++;
    int_digits = 1;
  } else {
    const char *int_start = p;
    const char *int_limit = (end - p > 19) ? (p + 19) : end;
    while (p < int_limit && (uint32_t)(*p - '0') < 10) {
      mant = mant * 10 + (uint64_t)(*p - '0');
      p++;
    }
    int_digits = (int)(p - int_start);
    if (CJSON_UNLIKELY(int_digits == 19 && p < end &&
                       (uint32_t)(*p - '0') < 10)) {
      uint64_t d = (uint64_t)(*p - '0');
      if (mant < 1844674407370955161ULL ||
          (mant == 1844674407370955161ULL && d <= 5)) {
        mant = mant * 10 + d;
        int_digits = 20;
        p++;
        if (p < end && (uint32_t)(*p - '0') < 10) {
          int_digits = 21;
          while (p < end && (uint32_t)(*p - '0') < 10)
            p++;
        }
      } else {
        int_digits = 21;
        while (p < end && (uint32_t)(*p - '0') < 10)
          p++;
      }
    }
  }

  /* Large integers (19..20 digits) or huge ints (> UINT64_MAX) */
  if (p >= end || (*p != '.' && *p != 'e' && *p != 'E')) {
    *p_io = p;
    if (int_digits == 19) {
      if (!is_neg) {
        if (mant <= (uint64_t)LLONG_MAX)
          return PyLong_FromLongLong((long long)mant);
        return PyLong_FromUnsignedLongLong(mant);
      }
      if (mant <= (uint64_t)LLONG_MAX)
        return PyLong_FromLongLong(-(long long)mant);
      if (mant == 9223372036854775808ULL)
        return PyLong_FromLongLong(LLONG_MIN);
      return parse_float_fallback(jp, start, p);
    }
    if (int_digits == 20 && !is_neg) {
      return PyLong_FromUnsignedLongLong(mant);
    }
    return parse_float_fallback(jp, start, p);
  }

  /* Float path with >19 int digits */
  if (CJSON_UNLIKELY(int_digits > 19)) {
    if (p < end && *p == '.') {
      p++;
      if (p >= end || (uint32_t)(*p - '0') >= 10) {
        raise_decode_error(jp, "no digit after decimal point", p);
        return NULL;
      }
      while (p < end && (uint32_t)(*p - '0') < 10)
        p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      p++;
      if (p < end && (*p == '+' || *p == '-'))
        p++;
      if (p >= end || (uint32_t)(*p - '0') >= 10) {
        raise_decode_error(jp, "no digit in exponent", p);
        return NULL;
      }
      while (p < end && (uint32_t)(*p - '0') < 10)
        p++;
    }
    *p_io = p;
    return parse_float_fallback(jp, start, p);
  }

  int exp10 = 0;
  int extra_digits = 0;

  if (*p == '.') {
    p++;
    if (CJSON_UNLIKELY(p >= end || (uint32_t)(*p - '0') >= 10)) {
      raise_decode_error(jp, "no digit after decimal point", p);
      return NULL;
    }
    const char *frac_start = p;
    int max_frac = 19 - ((mant == 0) ? 0 : int_digits);
    const char *frac_limit = (end - p > max_frac) ? (p + max_frac) : end;
    while (p < frac_limit && (uint32_t)(*p - '0') < 10) {
      mant = mant * 10 + (uint64_t)(*p - '0');
      p++;
    }
    if (CJSON_UNLIKELY(p < end && (uint32_t)(*p - '0') < 10)) {
      extra_digits = 1;
      while (p < end && (uint32_t)(*p - '0') < 10)
        p++;
    }
    exp10 = -(int)(p - frac_start);
  }

  if (p < end && (*p == 'e' || *p == 'E')) {
    p++;
    int exp_neg = 0;
    if (p < end && (*p == '+' || *p == '-')) {
      exp_neg = (*p == '-');
      p++;
    }
    if (CJSON_UNLIKELY(p >= end || (uint32_t)(*p - '0') >= 10)) {
      raise_decode_error(jp, "no digit in exponent", p);
      return NULL;
    }
    int exp_val = 0;
    while (p < end && (uint32_t)(*p - '0') < 10) {
      if (exp_val < 10000)
        exp_val = exp_val * 10 + (*p - '0');
      p++;
    }
    exp10 += exp_neg ? -exp_val : exp_val;
  }

  *p_io = p;

  if (CJSON_LIKELY(!extra_digits)) {
    if (CJSON_LIKELY(mant < (1ULL << 53) && exp10 >= -22 && exp10 <= 22)) {
      double d = (exp10 < 0) ? ((double)mant / POW10_DBL[-exp10])
                             : ((double)mant * POW10_DBL[exp10]);
      if (is_neg)
        d = -d;
      return PyFloat_FromDouble(d);
    }
    double d;
    if (CJSON_LIKELY(eisel_lemire_double(mant, exp10, is_neg, &d))) {
      return PyFloat_FromDouble(d);
    }
  }
  return parse_float_fallback(jp, start, p);
}

__attribute__((always_inline)) static inline PyObject *
parse_number(JsonParser *jp, const char **p_io, const char *end) {
  const char *p = *p_io;
  const char *start = p;
  int is_neg = 0;

  if (*p == '-') {
    is_neg = 1;
    p++;
    if (CJSON_UNLIKELY(p >= end || (uint32_t)(*p - '0') >= 10)) {
      raise_decode_error(jp, "no digit after sign", p);
      return NULL;
    }
  }

  uint64_t mant = 0;
  int int_digits = 0;

  if (*p == '0') {
    p++;
    int_digits = 1;
    if (CJSON_UNLIKELY(p < end && (uint32_t)(*p - '0') < 10)) {
      raise_decode_error(jp, "number with leading zero is not allowed", start);
      return NULL;
    }
  } else {
    const char *int_start = p;
    mant = (uint64_t)(*p++ - '0');
    if (CJSON_LIKELY(p + 4 <= end)) {
      uint32_t d;
      if ((d = (uint32_t)(*p - '0')) < 10) {
        mant = mant * 10 + d;
        p++;
        if ((d = (uint32_t)(*p - '0')) < 10) {
          mant = mant * 10 + d;
          p++;
          if ((d = (uint32_t)(*p - '0')) < 10) {
            mant = mant * 10 + d;
            p++;
            if ((d = (uint32_t)(*p - '0')) < 10) {
              mant = mant * 10 + d;
              p++;
              if (CJSON_LIKELY(p + 8 <= end)) {
                int n1 = parse_up_to_8_digits_swar(p, 8, &mant);
                p += n1;
                if (n1 == 8 && p + 8 <= end) {
                  p += parse_up_to_8_digits_swar(p, 6, &mant);
                }
              }
              const char *int_limit =
                  (end - int_start > 19) ? (int_start + 19) : end;
              while (p < int_limit && (uint32_t)(*p - '0') < 10) {
                mant = mant * 10 + (uint64_t)(*p - '0');
                p++;
              }
            }
          }
        }
      }
    } else {
      while (p < end && (uint32_t)(*p - '0') < 10) {
        mant = mant * 10 + (uint64_t)(*p - '0');
        p++;
      }
    }
    int_digits = (int)(p - int_start);
    if (CJSON_UNLIKELY(int_digits >= 19)) {
      return parse_number_slow(jp, p_io, start, end);
    }
  }

  /* Integer path (1..18 digits, always fits signed int64 without overflow) */
  if (p >= end || (*p != '.' && *p != 'e' && *p != 'E')) {
    *p_io = p;
    if (!is_neg) {
      if (CJSON_LIKELY(mant <= CJSON_SMALL_INT_MAX)) {
        PyObject *v = g_small_ints[mant + 5];
        Py_INCREF(v);
        return v;
      }
      return cjson_new_pylong_u64(mant, 0);
    }
    if (CJSON_LIKELY(mant <= 5)) {
      PyObject *v = g_small_ints[5 - mant];
      Py_INCREF(v);
      return v;
    }
    return cjson_new_pylong_u64(mant, 1);
  }

  /* Fast fixed-decimal float path (<= 19 total digits, no exponent) */
  if (CJSON_UNLIKELY(*p != '.')) {
    return parse_number_slow(jp, p_io, start, end);
  }
  p++;
  if (CJSON_UNLIKELY(p >= end || (uint32_t)(*p - '0') >= 10)) {
    raise_decode_error(jp, "no digit after decimal point", p);
    return NULL;
  }
  const char *frac_start = p;
  int max_frac = 19 - ((mant == 0) ? 0 : int_digits);
  const char *frac_limit = (end - p > max_frac) ? (p + max_frac) : end;

  if (CJSON_LIKELY(p + 8 <= end && max_frac >= 8)) {
    int n = parse_up_to_8_digits_swar(p, 8, &mant);
    p += n;
    if (n == 8 && p + 8 <= end && p < frac_limit) {
      int rem_frac = (int)(frac_limit - p);
      int max_d = (rem_frac < 8) ? rem_frac : 8;
      n = parse_up_to_8_digits_swar(p, max_d, &mant);
      p += n;
    }
  }
  while (p < frac_limit && (uint32_t)(*p - '0') < 10) {
    mant = mant * 10 + (uint64_t)(*p - '0');
    p++;
  }
  int exp10 = -(int)(p - frac_start);
  if (CJSON_UNLIKELY(p < end && (*p == 'e' || *p == 'E'))) {
    p++;
    int exp_neg = 0;
    if (p < end && (*p == '+' || *p == '-')) {
      exp_neg = (*p == '-');
      p++;
    }
    if (CJSON_UNLIKELY(p >= end || (uint32_t)(*p - '0') >= 10)) {
      raise_decode_error(jp, "no digit in exponent", p);
      return NULL;
    }
    int exp_val = *p++ - '0';
    if (p < end && (uint32_t)(*p - '0') < 10) {
      exp_val = exp_val * 10 + (*p++ - '0');
      if (p < end && (uint32_t)(*p - '0') < 10) {
        exp_val = exp_val * 10 + (*p++ - '0');
        while (p < end && (uint32_t)(*p - '0') < 10) {
          if (exp_val < 10000)
            exp_val = exp_val * 10 + (*p - '0');
          p++;
        }
      }
    }
    exp10 += exp_neg ? -exp_val : exp_val;
  } else if (CJSON_UNLIKELY(p < end && (uint32_t)(*p - '0') < 10)) {
    return parse_number_slow(jp, p_io, start, end);
  }

  /* Clinger's fast path: mant < 2^53 and -22 <= exp10 <= 22 is guaranteed
   * bit-exact under IEEE-754 round-to-nearest-even in one FP op. */
  if (mant < (1ULL << 53) && exp10 >= -22 && exp10 <= 22) {
    double d = (exp10 < 0) ? ((double)mant / POW10_DBL[-exp10])
                           : ((double)mant * POW10_DBL[exp10]);
    if (is_neg)
      d = -d;
    *p_io = p;
    return PyFloat_FromDouble(d);
  }
  double d;
  if (CJSON_LIKELY(eisel_lemire_double(mant, exp10, is_neg, &d))) {
    *p_io = p;
    return PyFloat_FromDouble(d);
  }
  return parse_number_slow(jp, p_io, start, end);
}

#endif /* CJSON_NUMPARSE_H */
