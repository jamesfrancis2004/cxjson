#include "serializer.h"
#include "buffer.h"
#include "zmij-c.h"
#include "jeaiii/jeaiii_to_text.h"
#include <datetime.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cached Python types and attribute strings */
static PyObject *g_uuid_type = NULL;
static PyObject *g_enum_type = NULL;
static PyObject *g_dataclass_fields_str = NULL;
static PyObject *g_int_str = NULL;
static PyObject *g_enum_value_str = NULL;

/* ========================================================================= */
/* Fragment Type                                                             */
/* ========================================================================= */

static void Fragment_dealloc(FragmentObject *self) {
  Py_XDECREF(self->contents);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *Fragment_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
  PyObject *contents = NULL;
  static char *kwlist[] = {"contents", NULL};
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &contents)) {
    return NULL;
  }
  FragmentObject *self = (FragmentObject *)type->tp_alloc(type, 0);
  if (self != NULL) {
    Py_INCREF(contents);
    self->contents = contents;
  }
  return (PyObject *)self;
}

PyTypeObject Fragment_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "cxjson.Fragment",
    .tp_doc = "Fragment(contents: Union[str, bytes])",
    .tp_basicsize = sizeof(FragmentObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Fragment_new,
    .tp_dealloc = (destructor)Fragment_dealloc,
};

#if !defined(Py_GIL_DISABLED)
static void cjson_parallel_init(void);
#endif

int init_serializer_types(void) {
  PyDateTime_IMPORT;
  if (PyType_Ready(&Fragment_Type) < 0) {
    return -1;
  }

  g_dataclass_fields_str = PyUnicode_InternFromString("__dataclass_fields__");
  g_int_str = PyUnicode_InternFromString("int");
  g_enum_value_str = PyUnicode_InternFromString("_value_");

  PyObject *uuid_mod = PyImport_ImportModule("uuid");
  if (uuid_mod != NULL) {
    g_uuid_type = PyObject_GetAttrString(uuid_mod, "UUID");
    Py_DECREF(uuid_mod);
  } else {
    PyErr_Clear();
  }

  PyObject *enum_mod = PyImport_ImportModule("enum");
  if (enum_mod != NULL) {
    g_enum_type = PyObject_GetAttrString(enum_mod, "Enum");
    Py_DECREF(enum_mod);
  } else {
    PyErr_Clear();
  }

#if !defined(Py_GIL_DISABLED)
  cjson_parallel_init();
#endif

  return 0;
}

typedef struct SortSchemaCache SortSchemaCache;

typedef struct {
  JsonBuffer *b;
  PyObject *default_fn;
  int opts;
  int indent_level;
  SortSchemaCache *sort_cache;
} SerializeContext;

#define CJSON_REC_SER_DEPTH 128

/* Forward declarations */
static int encode_fast_iter(JsonBuffer *b, PyObject *root);
static int encode_root_fast(JsonBuffer *b, PyObject *obj);
static char *encode_array_fast(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *obj);
static char *encode_dict_fast(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *obj);
static int json_encode_slow(JsonBuffer *b, PyObject *obj);
static int json_encode_opts(SerializeContext *ctx, PyObject *obj);
static int json_buffer_append_array_opts(SerializeContext *ctx, PyObject *obj);
static int json_buffer_append_object_opts(SerializeContext *ctx, PyObject *obj);

/* ========================================================================= */
/* Integer Serialization                                                     */
/* ========================================================================= */

static const char DIGIT_PAIRS[200] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

static inline int u32_ndigits(uint32_t v) {
  if (v < 100000) {
    if (v < 100) return (v >= 10) + 1;
    if (v < 1000) return 3;
    return (v >= 10000) + 4;
  }
  if (v < 10000000) return (v >= 1000000) + 6;
  if (v < 100000000) return 8;
  return (v >= 1000000000) + 9;
}

static inline int write_u32(char *dst, uint32_t v) {
  int n = u32_ndigits(v);
  char *p = dst + n;
  while (v >= 100) {
    uint32_t q = v / 100;
    uint32_t r = v - q * 100;
    p -= 2;
    memcpy(p, &DIGIT_PAIRS[r * 2], 2);
    v = q;
  }
  if (v >= 10) {
    memcpy(dst, &DIGIT_PAIRS[v * 2], 2);
  } else {
    dst[0] = (char)('0' + v);
  }
  return n;
}

static inline char *write_py_long(char *dst, PyObject *obj) {
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030F0000 && \
    PYLONG_BITS_IN_DIGIT == 30 && !defined(PYPY_VERSION) && !defined(Py_LIMITED_API)
  const PyLongObject *lo = (const PyLongObject *)obj;
  uintptr_t tag = lo->long_value.lv_tag;
  uintptr_t ndigits = tag >> _PyLong_NON_SIZE_BITS;
  if (CJSON_LIKELY(ndigits <= 1)) {
    if (CJSON_UNLIKELY(ndigits == 0)) {
      *dst++ = '0';
      return dst;
    }
    if ((tag & _PyLong_SIGN_MASK) == 2) {
      *dst++ = '-';
    }
    return jeaiii_to_text_u32(dst, (uint32_t)lo->long_value.ob_digit[0]);
  }
  if (CJSON_LIKELY(ndigits == 2)) {
    if ((tag & _PyLong_SIGN_MASK) == 2) {
      *dst++ = '-';
    }
    uint64_t mag = ((uint64_t)lo->long_value.ob_digit[1] << 30) |
                   (uint64_t)lo->long_value.ob_digit[0];
    return jeaiii_to_text_u64(dst, mag);
  }
  if (ndigits == 3) {
    digit d2 = lo->long_value.ob_digit[2];
    if (d2 < 16) {
      uint64_t mag = ((uint64_t)d2 << 60) |
                     ((uint64_t)lo->long_value.ob_digit[1] << 30) |
                     (uint64_t)lo->long_value.ob_digit[0];
      uintptr_t sign = tag & _PyLong_SIGN_MASK;
      if (sign == 0) {
        return jeaiii_to_text_u64(dst, mag);
      }
      if (sign == 2 && mag <= (1ULL << 63)) {
        *dst++ = '-';
        return jeaiii_to_text_u64(dst, mag);
      }
    }
  }
  PyErr_SetString(PyExc_TypeError, "Integer exceeds 64-bit range");
  return NULL;
#else
#if PY_VERSION_HEX >= 0x030C0000
  if (CJSON_LIKELY(PyUnstable_Long_IsCompact((const PyLongObject *)obj))) {
    Py_ssize_t sval = PyUnstable_Long_CompactValue((const PyLongObject *)obj);
#if PYLONG_BITS_IN_DIGIT <= 30
    return jeaiii_to_text_i32(dst, (int32_t)sval);
#else
    return jeaiii_to_text_i64(dst, (int64_t)sval);
#endif
  }
#endif
  int overflow = 0;
  long long lval = PyLong_AsLongLongAndOverflow(obj, &overflow);
  if (CJSON_LIKELY(overflow == 0)) {
    if (CJSON_UNLIKELY(lval == -1 && PyErr_Occurred())) {
      return NULL;
    }
    return jeaiii_to_text_i64(dst, (int64_t)lval);
  }
  if (overflow > 0) {
    unsigned long long ull = PyLong_AsUnsignedLongLong(obj);
    if (CJSON_LIKELY(ull != (unsigned long long)-1 || !PyErr_Occurred())) {
      return jeaiii_to_text_u64(dst, (uint64_t)ull);
    }
    PyErr_Clear();
  }
  PyErr_SetString(PyExc_TypeError, "Integer exceeds 64-bit range");
  return NULL;
#endif
}

static inline int unpack_py_long(PyObject *obj, uint64_t *out_mag, int *out_is_neg) {
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030F0000 && \
    PYLONG_BITS_IN_DIGIT == 30 && !defined(PYPY_VERSION) && !defined(Py_LIMITED_API)
  const PyLongObject *lo = (const PyLongObject *)obj;
  uintptr_t tag = lo->long_value.lv_tag;
  uintptr_t ndigits = tag >> _PyLong_NON_SIZE_BITS;
  if (CJSON_LIKELY(ndigits <= 1)) {
    if (CJSON_UNLIKELY(ndigits == 0)) {
      *out_mag = 0;
      *out_is_neg = 0;
      return 0;
    }
    *out_mag = (uint64_t)lo->long_value.ob_digit[0];
    *out_is_neg = ((tag & _PyLong_SIGN_MASK) == 2);
    return 0;
  }
  if (CJSON_LIKELY(ndigits == 2)) {
    *out_mag = ((uint64_t)lo->long_value.ob_digit[1] << 30) |
               (uint64_t)lo->long_value.ob_digit[0];
    *out_is_neg = ((tag & _PyLong_SIGN_MASK) == 2);
    return 0;
  }
  if (ndigits == 3) {
    digit d2 = lo->long_value.ob_digit[2];
    if (d2 < 16) {
      uint64_t mag = ((uint64_t)d2 << 60) |
                     ((uint64_t)lo->long_value.ob_digit[1] << 30) |
                     (uint64_t)lo->long_value.ob_digit[0];
      uintptr_t sign = tag & _PyLong_SIGN_MASK;
      if (sign == 0) {
        *out_mag = mag;
        *out_is_neg = 0;
        return 0;
      }
      if (sign == 2 && mag <= (1ULL << 63)) {
        *out_mag = mag;
        *out_is_neg = 1;
        return 0;
      }
    }
  }
  return -1;
#else
#if PY_VERSION_HEX >= 0x030C0000
  if (CJSON_LIKELY(PyUnstable_Long_IsCompact((const PyLongObject *)obj))) {
    Py_ssize_t sval = PyUnstable_Long_CompactValue((const PyLongObject *)obj);
    if (sval >= 0) {
      *out_mag = (uint64_t)sval;
      *out_is_neg = 0;
    } else {
      *out_mag = (uint64_t)-(uint64_t)sval;
      *out_is_neg = 1;
    }
    return 0;
  }
#endif
  int overflow = 0;
  long long lval = PyLong_AsLongLongAndOverflow(obj, &overflow);
  if (CJSON_LIKELY(overflow == 0)) {
    if (CJSON_UNLIKELY(lval == -1 && PyErr_Occurred())) {
      return -1;
    }
    if (lval >= 0) {
      *out_mag = (uint64_t)lval;
      *out_is_neg = 0;
    } else {
      *out_mag = (uint64_t)-(uint64_t)lval;
      *out_is_neg = 1;
    }
    return 0;
  }
  if (overflow > 0) {
    unsigned long long ull = PyLong_AsUnsignedLongLong(obj);
    if (CJSON_LIKELY(ull != (unsigned long long)-1 || !PyErr_Occurred())) {
      *out_mag = (uint64_t)ull;
      *out_is_neg = 0;
      return 0;
    }
    PyErr_Clear();
  }
  return -1;
#endif
}

static int json_buffer_append_int(JsonBuffer *b, PyObject *obj) {
  if (CJSON_UNLIKELY(b->len + 24 > b->cap)) {
    if (json_buffer_grow(b, b->len + 24) < 0) return -1;
  }
  char *end = write_py_long(b->data + b->len, obj);
  if (CJSON_UNLIKELY(end == NULL)) {
    return -1;
  }
  b->len = (Py_ssize_t)(end - b->data);
  return 0;
}

static int json_buffer_append_int_opts(SerializeContext *ctx, PyObject *obj) {
  uint64_t mag;
  int is_neg;
  if (CJSON_UNLIKELY(unpack_py_long(obj, &mag, &is_neg) < 0)) {
    if (!PyErr_Occurred()) {
      PyErr_SetString(PyExc_TypeError, "Integer exceeds 64-bit range");
    }
    return -1;
  }
  if (CJSON_UNLIKELY(ctx->opts & OPT_STRICT_INTEGER)) {
    if (mag > 9007199254740991ULL) {
      PyErr_SetString(PyExc_TypeError, "Integer exceeds 53-bit range");
      return -1;
    }
  }
  JsonBuffer *b = ctx->b;
  if (CJSON_UNLIKELY(b->len + 24 > b->cap)) {
    if (json_buffer_grow(b, b->len + 24) < 0) return -1;
  }
  char *dst = b->data + b->len;
  if (is_neg) {
    *dst++ = '-';
  }
  char *end = jeaiii_to_text_u64(dst, mag);
  b->len = (Py_ssize_t)(end - b->data);
  return 0;
}

/* ========================================================================= */
/* Double Serialization                                                      */
/* ========================================================================= */

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER) && !defined(__SSE4_1__)
extern char *zmij_detail_write_sse41(double value, char *buffer);
extern char *zmij_detail_write_f_sse41(float value, char *buffer);
#define CJSON_ZMIJ_RUNTIME_SSE41 1
static inline char *cjson_zmij_write_double(double val, char *dst) {
  if (CJSON_LIKELY(cjson_cpu_has_sse41)) {
    return zmij_detail_write_sse41(val, dst);
  }
  return zmij_detail_write(val, dst);
}
static inline char *cjson_zmij_write_float(float val, char *dst) {
  if (CJSON_LIKELY(cjson_cpu_has_sse41)) {
    return zmij_detail_write_f_sse41(val, dst);
  }
  return zmij_detail_write_f(val, dst);
}
#else
static inline char *cjson_zmij_write_double(double val, char *dst) {
  return zmij_detail_write(val, dst);
}
static inline char *cjson_zmij_write_float(float val, char *dst) {
  return zmij_detail_write_f(val, dst);
}
#endif

static inline char *write_double(double val, char *dst) {
  char *end = cjson_zmij_write_double(val, dst);
  for (char *p = end - 1; p >= dst; p--) {
    char c = *p;
    if (c == '.') {
      return end;
    }
    if (c == 'e') {
      char *d = p + 2;
      if (end - d == 2 && d[0] == '0') {
        d[0] = d[1];
        end--;
        if (d[0] == '5' && p[1] == '-') {
          char digits[24];
          int nd = 0;
          char *m = dst;
          int neg = (*m == '-');
          if (neg) m++;
          for (; m < p; m++) {
            if (*m != '.') digits[nd++] = *m;
          }
          char *o = dst;
          if (neg) *o++ = '-';
          memcpy(o, "0.0000", 6);
          o += 6;
          memcpy(o, digits, (size_t)nd);
          return o + nd;
        }
      }
      return end;
    }
  }
  end[0] = '.';
  end[1] = '0';
  return end + 2;
}

static inline char *write_double_fast(double val, char *dst) {
  double aval = fabs(val);
  if (CJSON_LIKELY(aval >= 1e-4 && aval < 1e16)) {
    int64_t ival = (int64_t)val;
    if (CJSON_LIKELY((double)ival != val)) {
      return cjson_zmij_write_double(val, dst);
    }
    if (CJSON_LIKELY(aval < 4503599627370496.0)) {
      char *end = jeaiii_to_text_i64(dst, ival);
      end[0] = '.';
      end[1] = '0';
      return end + 2;
    }
  } else if (aval >= 1e16 || (aval > 0.0 && aval < 1e-9)) {
    /* Extreme exponents (e+16..e+308, e-10..e-324): zmij output is already exact JSON */
    return cjson_zmij_write_double(val, dst);
  }
  return write_double(val, dst);
}

static int json_buffer_append_double(JsonBuffer *b, PyObject *obj) {
  double val = PyFloat_AS_DOUBLE(obj);
  if (CJSON_UNLIKELY(isnan(val) || isinf(val))) {
    return json_buffer_append_null(b);
  }
  if (CJSON_UNLIKELY(b->len + 40 > b->cap)) {
    if (json_buffer_grow(b, b->len + 40) < 0) return -1;
  }
  char *end = write_double_fast(val, b->data + b->len);
  b->len = (Py_ssize_t)(end - b->data);
  return 0;
}

/* ========================================================================= */
/* String Serialization                                                      */
/* ========================================================================= */

static const CJsonEscapeEntry ESCAPE_TABLE[256] = {
    [0x00] = {.str = "\\u0000", .len = 6}, [0x01] = {.str = "\\u0001", .len = 6},
    [0x02] = {.str = "\\u0002", .len = 6}, [0x03] = {.str = "\\u0003", .len = 6},
    [0x04] = {.str = "\\u0004", .len = 6}, [0x05] = {.str = "\\u0005", .len = 6},
    [0x06] = {.str = "\\u0006", .len = 6}, [0x07] = {.str = "\\u0007", .len = 6},
    [0x08] = {.str = "\\b",     .len = 2}, [0x09] = {.str = "\\t",     .len = 2},
    [0x0a] = {.str = "\\n",     .len = 2}, [0x0b] = {.str = "\\u000b", .len = 6},
    [0x0c] = {.str = "\\f",     .len = 2}, [0x0d] = {.str = "\\r",     .len = 2},
    [0x0e] = {.str = "\\u000e", .len = 6}, [0x0f] = {.str = "\\u000f", .len = 6},
    [0x10] = {.str = "\\u0010", .len = 6}, [0x11] = {.str = "\\u0011", .len = 6},
    [0x12] = {.str = "\\u0012", .len = 6}, [0x13] = {.str = "\\u0013", .len = 6},
    [0x14] = {.str = "\\u0014", .len = 6}, [0x15] = {.str = "\\u0015", .len = 6},
    [0x16] = {.str = "\\u0016", .len = 6}, [0x17] = {.str = "\\u0017", .len = 6},
    [0x18] = {.str = "\\u0018", .len = 6}, [0x19] = {.str = "\\u0019", .len = 6},
    [0x1a] = {.str = "\\u001a", .len = 6}, [0x1b] = {.str = "\\u001b", .len = 6},
    [0x1c] = {.str = "\\u001c", .len = 6}, [0x1d] = {.str = "\\u001d", .len = 6},
    [0x1e] = {.str = "\\u001e", .len = 6}, [0x1f] = {.str = "\\u001f", .len = 6},
    [0x22] = {.str = "\\\"",    .len = 2}, [0x5C] = {.str = "\\\\",    .len = 2},
};

__attribute__((noinline))
static int json_buffer_append_str_escaped(JsonBuffer *b, const char *src, Py_ssize_t len, Py_ssize_t clean) {
  b->data[b->len] = '"';
  b->len += 1 + clean;
  Py_ssize_t rem = len - clean;
  if (json_buffer_reserve(b, rem + 256) < 0) {
    return -1;
  }

  char *dst = b->data + b->len;
  const unsigned char *p = (const unsigned char *)src + clean;
  const unsigned char *end = (const unsigned char *)src + len;

  while (p + 16 <= end) {
    char *cap_limit = b->data + b->cap - 128;
    if (CJSON_UNLIKELY(dst > cap_limit)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + (Py_ssize_t)(end - p) + 512) < 0) return -1;
      dst = b->data + b->len;
      cap_limit = b->data + b->cap - 400;
    }
    const unsigned char *next_p = cjson_escape_chunk_fn(p, end, &dst, cap_limit, ESCAPE_TABLE);
    if (next_p == p) break;
    p = next_p;
  }

  while (p < end) {
    if (CJSON_UNLIKELY(dst + 16 > b->data + b->cap)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + (Py_ssize_t)(end - p) * 6 + 32) < 0) return -1;
      dst = b->data + b->len;
    }
    uint8_t c = *p++;
    const CJsonEscapeEntry *esc = &ESCAPE_TABLE[c];
    if (CJSON_UNLIKELY(esc->len != 0)) {
      memcpy(dst, esc->str, 8);
      dst += esc->len;
    } else {
      *dst++ = (char)c;
    }
  }

  if (CJSON_UNLIKELY(dst >= b->data + b->cap)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_reserve(b, 1) < 0) return -1;
    dst = b->data + b->len;
  }
  *dst++ = '"';
  b->len = (Py_ssize_t)(dst - b->data);
  return 0;
}

#include "parallel_ser.h"

static inline const char *cjson_get_str_utf8(PyObject *obj, Py_ssize_t *out_len) {
  if (CJSON_LIKELY(PyUnicode_IS_COMPACT_ASCII(obj))) {
    *out_len = PyUnicode_GET_LENGTH(obj);
    return (const char *)PyUnicode_1BYTE_DATA(obj);
  }
#ifndef Py_LIMITED_API
  if (PyUnicode_IS_COMPACT(obj)) {
    PyCompactUnicodeObject *cu = (PyCompactUnicodeObject *)obj;
    if (cu->utf8 != NULL) {
      *out_len = cu->utf8_length;
      return cu->utf8;
    }
  }
#endif
  return PyUnicode_AsUTF8AndSize(obj, out_len);
}

static inline int json_buffer_append_str(JsonBuffer *b, const char *src, Py_ssize_t len) {
  if (CJSON_UNLIKELY(b->len + len + 2 > b->cap)) {
    if (json_buffer_grow(b, b->len + len + 2) < 0) {
      return -1;
    }
  }
  char *dst = b->data + b->len;
  Py_ssize_t clean = copy_clean_prefix(dst + 1, src, len);
  if (CJSON_LIKELY(clean == len)) {
    dst[0] = '"';
    dst[len + 1] = '"';
    b->len += len + 2;
    return 0;
  }
  return json_buffer_append_str_escaped(b, src, len, clean);
}

__attribute__((noinline))
static char *emit_str_slow(JsonBuffer *b, char *dst, char **safe_end_p,
                           PyObject *val, const char *src, Py_ssize_t slen) {
  if (src == NULL) {
    src = PyUnicode_AsUTF8AndSize(val, &slen);
    if (CJSON_UNLIKELY(src == NULL)) return NULL;
    if (CJSON_UNLIKELY(slen == 0)) {
      dst[0] = '"';
      dst[1] = '"';
      return dst + 2;
    }
    if (slen <= 64 && copy_if_clean_short(dst + 1, src, slen)) {
      dst[0] = '"';
      dst[slen + 1] = '"';
      return dst + slen + 2;
    }
  }
  if (slen <= 64) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_append_str_escaped(b, src, slen, 0) < 0) return NULL;
    if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
      if (json_buffer_grow(b, b->len + 256) < 0) return NULL;
    }
    *safe_end_p = b->data + b->cap - 128;
    return b->data + b->len;
  }

  Py_ssize_t cur_len = (Py_ssize_t)(dst - b->data);
  if (CJSON_UNLIKELY(cur_len + slen + 130 > b->cap)) {
    b->len = cur_len;
    if (json_buffer_grow(b, cur_len + slen + 256) < 0) return NULL;
    dst = b->data + b->len;
    *safe_end_p = b->data + b->cap - 128;
  }
  Py_ssize_t clean = copy_clean_prefix(dst + 1, src, slen);
  if (CJSON_LIKELY(clean == slen)) {
    dst[0] = '"';
    dst[slen + 1] = '"';
    return dst + slen + 2;
  }
  b->len = (Py_ssize_t)(dst - b->data);
  if (json_buffer_append_str_escaped(b, src, slen, clean) < 0) return NULL;
  if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
    if (json_buffer_grow(b, b->len + 256) < 0) return NULL;
  }
  *safe_end_p = b->data + b->cap - 128;
  return b->data + b->len;
}

__attribute__((always_inline))
static inline char *emit_str_inline(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *val) {
  const char *src;
  Py_ssize_t slen;
  if (CJSON_LIKELY(PyUnicode_IS_COMPACT_ASCII(val))) {
    slen = PyUnicode_GET_LENGTH(val);
    src = (const char *)PyUnicode_1BYTE_DATA(val);
#ifndef Py_LIMITED_API
  } else if (CJSON_LIKELY(PyUnicode_IS_COMPACT(val) && ((PyCompactUnicodeObject *)val)->utf8 != NULL)) {
    PyCompactUnicodeObject *cu = (PyCompactUnicodeObject *)val;
    slen = cu->utf8_length;
    src = cu->utf8;
#endif
  } else {
    return emit_str_slow(b, dst, safe_end_p, val, NULL, 0);
  }

  if (CJSON_UNLIKELY(slen == 0)) {
    dst[0] = '"';
    dst[1] = '"';
    return dst + 2;
  }
  if (CJSON_LIKELY(slen <= 64)) {
    if (CJSON_LIKELY(slen == 1)) {
      unsigned char c = (unsigned char)src[0];
      if (CJSON_LIKELY(c >= 32 && c != '"' && c != '\\')) {
        dst[0] = '"';
        dst[1] = (char)c;
        dst[2] = '"';
        return dst + 3;
      }
    }
    if (CJSON_LIKELY(copy_if_clean_short(dst + 1, src, slen))) {
      dst[0] = '"';
      dst[slen + 1] = '"';
      return dst + slen + 2;
    }
  }
  return emit_str_slow(b, dst, safe_end_p, val, src, slen);
}

static inline int json_buffer_append_empty_str(JsonBuffer *b) {
  if (CJSON_UNLIKELY(b->len + 2 > b->cap)) {
    if (json_buffer_grow(b, b->len + 2) < 0) return -1;
  }
  b->data[b->len++] = '"';
  b->data[b->len++] = '"';
  return 0;
}

static int json_buffer_append_string(JsonBuffer *b, PyObject *obj) {
  Py_ssize_t len;
  const char *src = cjson_get_str_utf8(obj, &len);
  if (CJSON_UNLIKELY(src == NULL)) return -1;
  if (CJSON_UNLIKELY(len == 0)) {
    return json_buffer_append_empty_str(b);
  }
  return json_buffer_append_str(b, src, len);
}

/* ========================================================================= */
/* UUID, Datetime, Dataclass                                                 */
/* ========================================================================= */

static const char HEX_PAIRS[512] =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
    "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
    "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
    "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
    "c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
    "e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

static int json_buffer_append_uuid(JsonBuffer *b, PyObject *obj) {
  if (CJSON_UNLIKELY(b->len + 40 > b->cap)) {
    if (json_buffer_reserve(b, 40) < 0) return -1;
  }
  PyObject *py_int = PyObject_GetAttr(obj, g_int_str);
  if (py_int == NULL) return -1;

  unsigned char src[16];
#if PY_VERSION_HEX >= 0x030D0000
  if (PyLong_AsNativeBytes(py_int, src, 16, Py_ASNATIVEBYTES_BIG_ENDIAN | Py_ASNATIVEBYTES_UNSIGNED_BUFFER) < 0) {
    Py_DECREF(py_int);
    return -1;
  }
#else
  if (_PyLong_AsByteArray((PyLongObject *)py_int, src, 16, 0, 0) < 0) {
    Py_DECREF(py_int);
    return -1;
  }
#endif
  Py_DECREF(py_int);

  char *dst = b->data + b->len;
  *dst++ = '"';
  memcpy(dst, &HEX_PAIRS[src[0] * 2], 2);
  memcpy(dst + 2, &HEX_PAIRS[src[1] * 2], 2);
  memcpy(dst + 4, &HEX_PAIRS[src[2] * 2], 2);
  memcpy(dst + 6, &HEX_PAIRS[src[3] * 2], 2);
  dst[8] = '-';
  memcpy(dst + 9, &HEX_PAIRS[src[4] * 2], 2);
  memcpy(dst + 11, &HEX_PAIRS[src[5] * 2], 2);
  dst[13] = '-';
  memcpy(dst + 14, &HEX_PAIRS[src[6] * 2], 2);
  memcpy(dst + 16, &HEX_PAIRS[src[7] * 2], 2);
  dst[18] = '-';
  memcpy(dst + 19, &HEX_PAIRS[src[8] * 2], 2);
  memcpy(dst + 21, &HEX_PAIRS[src[9] * 2], 2);
  dst[23] = '-';
  memcpy(dst + 24, &HEX_PAIRS[src[10] * 2], 2);
  memcpy(dst + 26, &HEX_PAIRS[src[11] * 2], 2);
  memcpy(dst + 28, &HEX_PAIRS[src[12] * 2], 2);
  memcpy(dst + 30, &HEX_PAIRS[src[13] * 2], 2);
  memcpy(dst + 32, &HEX_PAIRS[src[14] * 2], 2);
  memcpy(dst + 34, &HEX_PAIRS[src[15] * 2], 2);
  dst[36] = '"';
  dst += 37;

  b->len = (Py_ssize_t)(dst - b->data);
  return 0;
}

static int json_buffer_append_datetime_fast(JsonBuffer *b, PyObject *obj) {
  if (json_buffer_reserve(b, 64) < 0) return -1;

  char *dst = b->data + b->len;
  *dst++ = '"';

  if (CJSON_LIKELY(PyDateTime_Check(obj))) {
    int year = PyDateTime_GET_YEAR(obj);
    int month = PyDateTime_GET_MONTH(obj);
    int day = PyDateTime_GET_DAY(obj);

    if (CJSON_LIKELY(year >= 1000 && year <= 9999)) {
      memcpy(dst, &DIGIT_PAIRS[(year / 100) * 2], 2);
      memcpy(dst + 2, &DIGIT_PAIRS[(year % 100) * 2], 2);
      dst += 4;
    } else {
      dst += write_u32(dst, (uint32_t)year);
    }
    *dst++ = '-';
    memcpy(dst, &DIGIT_PAIRS[month * 2], 2);
    dst[2] = '-';
    memcpy(dst + 3, &DIGIT_PAIRS[day * 2], 2);
    dst[5] = 'T';
    dst += 6;

    int hour = PyDateTime_DATE_GET_HOUR(obj);
    int minute = PyDateTime_DATE_GET_MINUTE(obj);
    int second = PyDateTime_DATE_GET_SECOND(obj);
    int microsecond = PyDateTime_DATE_GET_MICROSECOND(obj);
    PyObject *tz = PyDateTime_DATE_GET_TZINFO(obj);

    memcpy(dst, &DIGIT_PAIRS[hour * 2], 2);
    dst[2] = ':';
    memcpy(dst + 3, &DIGIT_PAIRS[minute * 2], 2);
    dst[5] = ':';
    memcpy(dst + 6, &DIGIT_PAIRS[second * 2], 2);
    dst += 8;

    if (microsecond > 0) {
      *dst++ = '.';
      uint32_t a = (uint32_t)microsecond / 10000;
      uint32_t rem = (uint32_t)microsecond - a * 10000;
      uint32_t b_part = rem / 100;
      uint32_t c = rem - b_part * 100;
      memcpy(dst, &DIGIT_PAIRS[a * 2], 2);
      memcpy(dst + 2, &DIGIT_PAIRS[b_part * 2], 2);
      memcpy(dst + 4, &DIGIT_PAIRS[c * 2], 2);
      dst += 6;
    }

    if (tz != NULL && tz != Py_None) {
      if (tz == PyDateTime_TimeZone_UTC) {
        memcpy(dst, "+00:00", 6);
        dst += 6;
      } else {
        PyObject *delta = PyObject_CallMethod(obj, "utcoffset", NULL);
        if (delta != NULL && delta != Py_None) {
          int days = PyDateTime_DELTA_GET_DAYS(delta);
          int seconds = PyDateTime_DELTA_GET_SECONDS(delta);
          Py_DECREF(delta);
          int total_seconds = days * 86400 + seconds;
          if (total_seconds == 0) {
            memcpy(dst, "+00:00", 6);
            dst += 6;
          } else {
            if (total_seconds < 0) {
              *dst++ = '-';
              total_seconds = -total_seconds;
            } else {
              *dst++ = '+';
            }
            int off_hour = total_seconds / 3600;
            int off_rem = total_seconds % 3600;
            int off_minute = off_rem / 60;
            int off_second = off_rem % 60;
            memcpy(dst, &DIGIT_PAIRS[off_hour * 2], 2);
            dst += 2;
            *dst++ = ':';
            memcpy(dst, &DIGIT_PAIRS[off_minute * 2], 2);
            dst += 2;
            if (off_second > 0) {
              *dst++ = ':';
              memcpy(dst, &DIGIT_PAIRS[off_second * 2], 2);
              dst += 2;
            }
          }
        } else {
          Py_XDECREF(delta);
          PyErr_Clear();
        }
      }
    }
    *dst++ = '"';
    b->len = (Py_ssize_t)(dst - b->data);
    return 0;
  }

  int is_date = PyDate_Check(obj);
  int is_time = PyTime_Check(obj);

  if (is_date) {
    int year = PyDateTime_GET_YEAR(obj);
    int month = PyDateTime_GET_MONTH(obj);
    int day = PyDateTime_GET_DAY(obj);
    if (CJSON_LIKELY(year >= 1000 && year <= 9999)) {
      memcpy(dst, &DIGIT_PAIRS[(year / 100) * 2], 2);
      memcpy(dst + 2, &DIGIT_PAIRS[(year % 100) * 2], 2);
      dst += 4;
    } else {
      dst += write_u32(dst, (uint32_t)year);
    }
    *dst++ = '-';
    memcpy(dst, &DIGIT_PAIRS[month * 2], 2);
    dst[2] = '-';
    memcpy(dst + 3, &DIGIT_PAIRS[day * 2], 2);
    dst += 5;
  }

  if (is_time) {
    int hour = PyDateTime_TIME_GET_HOUR(obj);
    int minute = PyDateTime_TIME_GET_MINUTE(obj);
    int second = PyDateTime_TIME_GET_SECOND(obj);
    int microsecond = PyDateTime_TIME_GET_MICROSECOND(obj);
    PyObject *tz = PyDateTime_TIME_GET_TZINFO(obj);

    memcpy(dst, &DIGIT_PAIRS[hour * 2], 2);
    dst[2] = ':';
    memcpy(dst + 3, &DIGIT_PAIRS[minute * 2], 2);
    dst[5] = ':';
    memcpy(dst + 6, &DIGIT_PAIRS[second * 2], 2);
    dst += 8;

    if (microsecond > 0) {
      *dst++ = '.';
      uint32_t a = (uint32_t)microsecond / 10000;
      uint32_t rem = (uint32_t)microsecond - a * 10000;
      uint32_t b_part = rem / 100;
      uint32_t c = rem - b_part * 100;
      memcpy(dst, &DIGIT_PAIRS[a * 2], 2);
      memcpy(dst + 2, &DIGIT_PAIRS[b_part * 2], 2);
      memcpy(dst + 4, &DIGIT_PAIRS[c * 2], 2);
      dst += 6;
    }

    if (tz != NULL && tz != Py_None) {
      if (tz == PyDateTime_TimeZone_UTC) {
        memcpy(dst, "+00:00", 6);
        dst += 6;
      } else {
        PyObject *delta = PyObject_CallMethod(obj, "utcoffset", NULL);
        if (delta != NULL && delta != Py_None) {
          int days = PyDateTime_DELTA_GET_DAYS(delta);
          int seconds = PyDateTime_DELTA_GET_SECONDS(delta);
          Py_DECREF(delta);
          int total_seconds = days * 86400 + seconds;
          if (total_seconds == 0) {
            memcpy(dst, "+00:00", 6);
            dst += 6;
          } else {
            if (total_seconds < 0) {
              *dst++ = '-';
              total_seconds = -total_seconds;
            } else {
              *dst++ = '+';
            }
            int off_hour = total_seconds / 3600;
            int off_rem = total_seconds % 3600;
            int off_minute = off_rem / 60;
            int off_second = off_rem % 60;
            memcpy(dst, &DIGIT_PAIRS[off_hour * 2], 2);
            dst += 2;
            *dst++ = ':';
            memcpy(dst, &DIGIT_PAIRS[off_minute * 2], 2);
            dst += 2;
            if (off_second > 0) {
              *dst++ = ':';
              memcpy(dst, &DIGIT_PAIRS[off_second * 2], 2);
              dst += 2;
            }
          }
        } else {
          Py_XDECREF(delta);
          PyErr_Clear();
        }
      }
    }
  }

  *dst++ = '"';
  b->len = (Py_ssize_t)(dst - b->data);
  return 0;
}

static int json_buffer_append_dataclass_fast(JsonBuffer *b, PyObject *obj) {
#ifndef Py_LIMITED_API
  PyObject **dptr = _PyObject_GetDictPtr(obj);
  if (dptr != NULL && *dptr != NULL && PyDict_CheckExact(*dptr)) {
    return encode_root_fast(b, *dptr);
  }
#endif
  PyObject *dict = PyObject_GenericGetDict(obj, NULL);
  if (dict != NULL) {
    int rc = encode_root_fast(b, dict);
    Py_DECREF(dict);
    return rc;
  }
  PyErr_Clear();

  PyObject *fields = PyObject_GetAttr(obj, g_dataclass_fields_str);
  if (fields == NULL) return -1;

  PyObject *fallback_dict = PyDict_New();
  if (fallback_dict == NULL) {
    Py_DECREF(fields);
    return -1;
  }

  PyObject *key, *field_obj;
  Py_ssize_t pos = 0;
  while (PyDict_Next(fields, &pos, &key, &field_obj)) {
    if (PyUnicode_Check(key)) {
      Py_ssize_t klen;
      const char *ks = PyUnicode_AsUTF8AndSize(key, &klen);
      if (ks != NULL && klen > 0 && ks[0] == '_') {
        continue;
      }
    }
    PyObject *val = PyObject_GetAttr(obj, key);
    if (val != NULL) {
      PyDict_SetItem(fallback_dict, key, val);
      Py_DECREF(val);
    } else {
      PyErr_Clear();
    }
  }
  Py_DECREF(fields);

  int rc = encode_root_fast(b, fallback_dict);
  Py_DECREF(fallback_dict);
  return rc;
}

static int json_buffer_append_seq_subclass(JsonBuffer *b, PyObject *obj) {
  Py_ssize_t len = PySequence_Fast_GET_SIZE(obj);
  PyObject **items = PySequence_Fast_ITEMS(obj);
  if (json_buffer_append_char(b, '[') < 0) return -1;
  for (Py_ssize_t i = 0; i < len; i++) {
    if (i > 0 && json_buffer_append_char(b, ',') < 0) return -1;
    if (encode_root_fast(b, items[i]) < 0) return -1;
  }
  return json_buffer_append_char(b, ']');
}

static int json_buffer_append_dict_subclass(JsonBuffer *b, PyObject *obj) {
  if (json_buffer_append_char(b, '{') < 0) return -1;
  PyObject *key, *value;
  Py_ssize_t pos = 0;
  int first = 1;
  while (PyDict_Next(obj, &pos, &key, &value)) {
    if (!first && json_buffer_append_char(b, ',') < 0) return -1;
    first = 0;
    PyTypeObject *ktp = Py_TYPE(key);
    if (CJSON_UNLIKELY(ktp != &PyUnicode_Type && !PyType_FastSubclass(ktp, Py_TPFLAGS_UNICODE_SUBCLASS))) {
      PyErr_SetString(PyExc_TypeError, "Dict key must be str");
      return -1;
    }
    if (json_buffer_append_string(b, key) < 0) return -1;
    if (json_buffer_append_char(b, ':') < 0) return -1;
    if (encode_root_fast(b, value) < 0) return -1;
  }
  return json_buffer_append_char(b, '}');
}

static inline int json_encode_enum_val(JsonBuffer *b, PyObject *val) {
  PyTypeObject *vtp = Py_TYPE(val);
  if (vtp == &PyUnicode_Type) return json_buffer_append_string(b, val);
  if (vtp == &PyLong_Type) return json_buffer_append_int(b, val);
  if (vtp == &PyBool_Type) return (val == Py_True) ? json_buffer_append_true(b) : json_buffer_append_false(b);
  if (val == Py_None) return json_buffer_append_null(b);
  if (vtp == &PyFloat_Type) return json_buffer_append_double(b, val);
  if (vtp == &PyList_Type || vtp == &PyTuple_Type || vtp == &PyDict_Type) {
    return encode_root_fast(b, val);
  }
  return json_encode_slow(b, val);
}

/* Slow-path encoder for datetimes, UUIDs, Enums, dataclasses, Fragment, and subclasses */
__attribute__((noinline))
static int json_encode_slow(JsonBuffer *b, PyObject *obj) {
  if (CJSON_UNLIKELY(++b->depth > CJSON_MAX_DEPTH)) {
    b->depth--;
    PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
    return -1;
  }

  PyTypeObject *tp = Py_TYPE(obj);

  /* Fragment type and exact built-in types (before dataclass __dict__ probe) */
  if (tp == &Fragment_Type) {
    PyObject *contents = ((FragmentObject *)obj)->contents;
    int rc = -1;
    if (contents != NULL && PyBytes_Check(contents)) {
      rc = json_buffer_append(b, PyBytes_AS_STRING(contents), PyBytes_GET_SIZE(contents));
    } else if (contents != NULL && PyUnicode_Check(contents)) {
      Py_ssize_t len;
      const char *utf8 = cjson_get_str_utf8(contents, &len);
      if (utf8 != NULL) rc = json_buffer_append(b, utf8, len);
    } else {
      PyErr_SetString(PyExc_TypeError, "orjson.Fragment's content is not of type bytes or str");
    }
    b->depth--;
    return rc;
  }

  if (PyDateTime_CheckExact(obj) || PyDate_CheckExact(obj) || PyTime_CheckExact(obj)) {
    int rc = json_buffer_append_datetime_fast(b, obj);
    b->depth--;
    return rc;
  }
  if ((PyObject *)tp == g_uuid_type) {
    int rc = json_buffer_append_uuid(b, obj);
    b->depth--;
    return rc;
  }

  unsigned long fl = PyType_GetFlags(tp);
  if (tp->tp_base == &PyBaseObject_Type &&
      !(fl & (Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_LONG_SUBCLASS |
              Py_TPFLAGS_LIST_SUBCLASS | Py_TPFLAGS_TUPLE_SUBCLASS |
              Py_TPFLAGS_UNICODE_SUBCLASS | Py_TPFLAGS_DICT_SUBCLASS))) {
    goto try_default;
  }

  /* Fast single-inheritance checks (avoids MRO walks for str subclasses & dataclasses) */
  if (tp->tp_bases != NULL && PyTuple_GET_SIZE(tp->tp_bases) == 1) {
    if (tp->tp_base == &PyUnicode_Type) {
      int rc = json_buffer_append_string(b, obj);
      b->depth--;
      return rc;
    }
    if (!PyType_Check(obj) && tp->tp_base == &PyBaseObject_Type &&
        tp->tp_dict != NULL && PyDict_GetItem(tp->tp_dict, g_dataclass_fields_str) != NULL) {
      int rc = json_buffer_append_dataclass_fast(b, obj);
      b->depth--;
      return rc;
    }
    if (tp->tp_base == &PyLong_Type) {
      int rc = json_buffer_append_int(b, obj);
      b->depth--;
      return rc;
    }
    if (tp->tp_base == &PyList_Type || tp->tp_base == &PyTuple_Type) {
      int rc = json_buffer_append_seq_subclass(b, obj);
      b->depth--;
      return rc;
    }
    if (tp->tp_base == &PyDict_Type) {
      int rc = json_buffer_append_dict_subclass(b, obj);
      b->depth--;
      return rc;
    }
  }

  /* Enum (value stored in _value_) */
  if (g_enum_type != NULL && PyType_IsSubtype(tp, (PyTypeObject *)g_enum_type)) {
    PyObject *val = PyObject_GetAttr(obj, g_enum_value_str);
    if (val != NULL) {
      int rc = json_encode_enum_val(b, val);
      Py_DECREF(val);
      b->depth--;
      return rc;
    }
    PyErr_Clear();
  }

  if (fl & Py_TPFLAGS_UNICODE_SUBCLASS) {
    int rc = json_buffer_append_string(b, obj);
    b->depth--;
    return rc;
  }

  /* Inherited dataclass */
  if (!PyType_Check(obj) && (fl & Py_TPFLAGS_HEAPTYPE)) {
    if (_PyType_Lookup(tp, g_dataclass_fields_str) != NULL) {
      int rc = json_buffer_append_dataclass_fast(b, obj);
      b->depth--;
      return rc;
    }
  }

  /* Remaining subclasses of builtin types */
  if (fl & Py_TPFLAGS_LONG_SUBCLASS) { b->depth--; return json_buffer_append_int(b, obj); }
  if (fl & (Py_TPFLAGS_LIST_SUBCLASS | Py_TPFLAGS_TUPLE_SUBCLASS)) {
    int rc = json_buffer_append_seq_subclass(b, obj);
    b->depth--;
    return rc;
  }
  if (fl & Py_TPFLAGS_DICT_SUBCLASS) {
    int rc = json_buffer_append_dict_subclass(b, obj);
    b->depth--;
    return rc;
  }
  if (PyFloat_Check(obj)) { b->depth--; return json_buffer_append_double(b, obj); }

  /* Subclasses of UUID / datetime types */
  if (g_uuid_type != NULL && PyObject_IsInstance(obj, g_uuid_type)) {
    int rc = json_buffer_append_uuid(b, obj);
    b->depth--;
    return rc;
  }
  if (PyDateTime_Check(obj) || PyDate_Check(obj) || PyTime_Check(obj)) {
    int rc = json_buffer_append_datetime_fast(b, obj);
    b->depth--;
    return rc;
  }

try_default:
  if (b->default_fn != NULL) {
    PyObject *res = PyObject_Vectorcall(b->default_fn, &obj, 1, NULL);
    if (CJSON_LIKELY(res != NULL)) {
      int rc = encode_root_fast(b, res);
      Py_DECREF(res);
      b->depth--;
      return rc;
    }
    PyErr_Clear();
  }

  b->depth--;
  PyErr_Format(PyExc_TypeError, "Type is not JSON serializable: %.200s", tp->tp_name);
  return -1;
}

/* ========================================================================= */
/* Hybrid Fast-Path Serializer (recursive w/ homogeneous loops + iter tail)  */
/* ========================================================================= */

static char *encode_array_fast(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *obj);

/* Out-of-line homogeneous dict-array encoder with per-call pre-encoded key cache.
 * Kept noinline so its 3KB stack cache never bloats encode_array_fast's frame
 * (critical for deeply nested float arrays like canada.json). */
__attribute__((noinline))
static char *encode_dict_array_cached(
    JsonBuffer *b, char *dst, char **safe_end_p,
    PyObject **items, Py_ssize_t len, Py_ssize_t *out_idx) {
  typedef struct {
    PyObject *key_ptr;
    uint16_t offset;
    uint16_t enc_len;
  } SerKeySlot;
  SerKeySlot kc_slots[64];
  memset(kc_slots, 0, sizeof(kc_slots));
  char kc_buf[2048];
  uint32_t kc_used = 0;
  char *safe_end = *safe_end_p;

#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000 && \
    !defined(Py_GIL_DISABLED) && !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)
  PyDictKeysObject *split_dk = NULL;
  uint32_t split_n = 0;
  uint8_t split_psize = 0;
  uint8_t split_prefix[32];
  uint8_t split_order[30];
  uint16_t split_koff[30];
  uint16_t split_klen[30];
  char split_kbuf[1024];
#endif

  for (Py_ssize_t i = 0; i < len; i++) {
    PyObject *item = items[i];
    if (CJSON_UNLIKELY(Py_TYPE(item) != &PyDict_Type)) {
      *out_idx = i;
      *safe_end_p = safe_end;
      return dst;
    }
    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      Py_ssize_t rem = len - i;
      Py_ssize_t add = (rem < 256) ? (rem * 16 + 128) : 4096;
      if (json_buffer_grow(b, b->len + add) < 0) return NULL;
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }
    if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (encode_fast_iter(b, item) < 0) return NULL;
      if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
        if (json_buffer_grow(b, b->len + 256) < 0) return NULL;
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    } else if (PyDict_GET_SIZE(item) == 0) {
      dst[0] = '{';
      dst[1] = '}';
      dst += 2;
    } else {
      if (CJSON_UNLIKELY(b->depth + 1 > CJSON_MAX_DEPTH)) {
        PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
        return NULL;
      }
      b->depth++;
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000 && \
    !defined(Py_GIL_DISABLED) && !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)
      PyDictObject *mp = (PyDictObject *)item;
      if (mp->ma_values != NULL && mp->ma_used > 0 && mp->ma_used <= 30) {
        const uint8_t *vraw = (const uint8_t *)mp->ma_values;
        uint8_t psize = vraw[-1];
        if (CJSON_LIKELY(mp->ma_keys == split_dk && (uint32_t)mp->ma_used == split_n &&
                         psize == split_psize && psize <= 32 &&
                         memcmp(vraw - psize, split_prefix, psize) == 0)) {
          PyObject *const *svals = (PyObject *const *)mp->ma_values;
          for (uint32_t j = 0; j < split_n; j++) {
            if (CJSON_UNLIKELY(dst > safe_end)) {
              b->len = (Py_ssize_t)(dst - b->data);
              if (json_buffer_grow(b, b->len + 512) < 0) { b->depth--; return NULL; }
              dst = b->data + b->len;
              safe_end = b->data + b->cap - 128;
            }
            uint32_t kl = split_klen[j];
            if (CJSON_LIKELY(kl <= 16)) {
              copy_short_ascii(dst, split_kbuf + split_koff[j], kl);
            } else {
              memcpy(dst, split_kbuf + split_koff[j], kl);
            }
            dst += kl;
            if (CJSON_UNLIKELY(dst > safe_end)) {
              b->len = (Py_ssize_t)(dst - b->data);
              if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
              dst = b->data + b->len;
              safe_end = b->data + b->cap - 128;
            }
            PyObject *dval = svals[split_order[j]];
            PyTypeObject *vtp = Py_TYPE(dval);
            if (vtp == &PyUnicode_Type) {
              dst = emit_str_inline(b, dst, &safe_end, dval);
              if (CJSON_UNLIKELY(dst == NULL)) { b->depth--; return NULL; }
            } else if (vtp == &PyLong_Type) {
              char *next = write_py_long(dst, dval);
              if (CJSON_UNLIKELY(next == NULL)) { b->depth--; return NULL; }
              dst = next;
            } else if (vtp == &PyFloat_Type) {
              double d = PyFloat_AS_DOUBLE(dval);
              if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
                dst = write_double_fast(d, dst);
              } else {
                memcpy(dst, "null", 4);
                dst += 4;
              }
            } else if (dval == Py_None) {
              memcpy(dst, "null", 4);
              dst += 4;
            } else if (vtp == &PyBool_Type) {
              if (dval == Py_True) {
                memcpy(dst, "true", 4);
                dst += 4;
              } else {
                memcpy(dst, "false", 5);
                dst += 5;
              }
            } else if (vtp == &PyList_Type || vtp == &PyTuple_Type) {
              if (PySequence_Fast_GET_SIZE(dval) == 0) {
                dst[0] = '['; dst[1] = ']'; dst += 2;
              } else {
                dst = encode_array_fast(b, dst, &safe_end, dval);
                if (CJSON_UNLIKELY(dst == NULL)) { b->depth--; return NULL; }
              }
            } else if (vtp == &PyDict_Type) {
              if (PyDict_GET_SIZE(dval) == 0) {
                dst[0] = '{'; dst[1] = '}'; dst += 2;
              } else {
                dst = encode_dict_fast(b, dst, &safe_end, dval);
                if (CJSON_UNLIKELY(dst == NULL)) { b->depth--; return NULL; }
              }
            } else {
              b->len = (Py_ssize_t)(dst - b->data);
              if (json_encode_slow(b, dval) < 0) { b->depth--; return NULL; }
              if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
                if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
              }
              dst = b->data + b->len;
              safe_end = b->data + b->cap - 128;
            }
          }
          *dst++ = '}';
          b->depth--;
          *dst++ = ',';
          continue;
        }
        /* Warm the 1-pointer split-table template on first sight */
        if (psize <= 32 && vraw[-2] == (uint8_t)mp->ma_used) {
          uint32_t sn = (uint32_t)mp->ma_used;
          uint32_t kused = 0;
          int ok = 1;
          PyObject *tk, *tv;
          Py_ssize_t tpos = 0;
          uint32_t tj = 0;
          while (PyDict_Next(item, &tpos, &tk, &tv) && tj < sn) {
            if (!PyUnicode_IS_COMPACT_ASCII(tk)) { ok = 0; break; }
            Py_ssize_t tkl = PyUnicode_GET_LENGTH(tk);
            if (tkl > 60 || kused + (uint32_t)tkl + 4u > sizeof(split_kbuf)) { ok = 0; break; }
            char *kdst = split_kbuf + kused;
            kdst[0] = (tj == 0) ? '{' : ',';
            kdst[1] = '"';
            if (!copy_if_clean_short(kdst + 2, (const char *)PyUnicode_1BYTE_DATA(tk), tkl)) {
              ok = 0; break;
            }
            kdst[2 + tkl] = '"';
            kdst[3 + tkl] = ':';
            split_koff[tj] = (uint16_t)kused;
            split_klen[tj] = (uint16_t)(tkl + 4);
            split_order[tj] = vraw[-3 - (int)tj];
            kused += (uint32_t)(tkl + 4);
            tj++;
          }
          if (ok && tj == sn) {
            split_dk = mp->ma_keys;
            split_n = sn;
            split_psize = psize;
            memcpy(split_prefix, vraw - psize, psize);
          }
        }
      }
#endif
      *dst++ = '{';
      PyObject *key, *dval;
      Py_ssize_t pos = 0;
      while (PyDict_Next(item, &pos, &key, &dval)) {
        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + 512) < 0) { b->depth--; return NULL; }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        uint32_t slot = (uint32_t)(((uintptr_t)key >> 4) ^ ((uintptr_t)key >> 10)) & 63u;
        SerKeySlot *s = &kc_slots[slot];
        if (CJSON_LIKELY(s->key_ptr == key)) {
          uint32_t elen = s->enc_len;
          if (CJSON_LIKELY(elen <= 16)) {
            copy_short_ascii(dst, kc_buf + s->offset, elen);
          } else {
            memcpy(dst, kc_buf + s->offset, elen);
          }
          dst += elen;
        } else {
          PyTypeObject *ktp = Py_TYPE(key);
          if (CJSON_UNLIKELY(ktp != &PyUnicode_Type &&
                             !PyType_FastSubclass(ktp, Py_TPFLAGS_UNICODE_SUBCLASS))) {
            b->depth--;
            PyErr_SetString(PyExc_TypeError, "Dict key must be str");
            return NULL;
          }
          Py_ssize_t kstart_off = (Py_ssize_t)(dst - b->data);
          dst = emit_str_inline(b, dst, &safe_end, key);
          if (CJSON_UNLIKELY(dst == NULL)) { b->depth--; return NULL; }
          *dst++ = ':';
          Py_ssize_t elen = (Py_ssize_t)(dst - b->data) - kstart_off;
          if (elen <= 64 && kc_used + (uint32_t)elen <= sizeof(kc_buf)) {
            memcpy(kc_buf + kc_used, b->data + kstart_off, (size_t)elen);
            s->key_ptr = key;
            s->offset = (uint16_t)kc_used;
            s->enc_len = (uint16_t)elen;
            kc_used += (uint32_t)elen;
          }
        }

        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }

        PyTypeObject *vtp = Py_TYPE(dval);
        if (vtp == &PyUnicode_Type) {
          dst = emit_str_inline(b, dst, &safe_end, dval);
          if (CJSON_UNLIKELY(dst == NULL)) { b->depth--; return NULL; }
        } else if (vtp == &PyLong_Type) {
          char *next = write_py_long(dst, dval);
          if (CJSON_UNLIKELY(next == NULL)) { b->depth--; return NULL; }
          dst = next;
        } else if (vtp == &PyFloat_Type) {
          double d = PyFloat_AS_DOUBLE(dval);
          if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
            dst = write_double_fast(d, dst);
          } else {
            memcpy(dst, "null", 4);
            dst += 4;
          }
        } else if (dval == Py_None) {
          memcpy(dst, "null", 4);
          dst += 4;
        } else if (vtp == &PyBool_Type) {
          if (dval == Py_True) {
            memcpy(dst, "true", 4);
            dst += 4;
          } else {
            memcpy(dst, "false", 5);
            dst += 5;
          }
        } else if (vtp == &PyList_Type || vtp == &PyTuple_Type) {
          if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
            b->len = (Py_ssize_t)(dst - b->data);
            if (encode_fast_iter(b, dval) < 0) { b->depth--; return NULL; }
            if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
              if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
            }
            dst = b->data + b->len;
            safe_end = b->data + b->cap - 128;
          } else if (PySequence_Fast_GET_SIZE(dval) == 0) {
            dst[0] = '[';
            dst[1] = ']';
            dst += 2;
          } else {
            dst = encode_array_fast(b, dst, &safe_end, dval);
            if (CJSON_UNLIKELY(dst == NULL)) { b->depth--; return NULL; }
          }
        } else if (vtp == &PyDict_Type) {
          if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
            b->len = (Py_ssize_t)(dst - b->data);
            if (encode_fast_iter(b, dval) < 0) { b->depth--; return NULL; }
            if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
              if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
            }
            dst = b->data + b->len;
            safe_end = b->data + b->cap - 128;
          } else if (PyDict_GET_SIZE(dval) == 0) {
            dst[0] = '{';
            dst[1] = '}';
            dst += 2;
          } else {
            dst = encode_dict_fast(b, dst, &safe_end, dval);
            if (CJSON_UNLIKELY(dst == NULL)) { b->depth--; return NULL; }
          }
        } else {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_encode_slow(b, dval) < 0) { b->depth--; return NULL; }
          if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
            if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
          }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        *dst++ = ',';
      }
      dst[-1] = '}';
      b->depth--;
    }
    *dst++ = ',';
  }
  *out_idx = len;
  *safe_end_p = safe_end;
  return dst;
}

#if defined(CJSON_PARALLEL_SER)
/* O(1) bounded 2-level size estimator for unlocking parallel dumps on nested
 * huge containers (e.g. countries.geo.json 14.6MB, canada.json 2.2MB). */
static Py_ssize_t estimate_elem_bytes_o1(PyObject *obj, int depth_left) {
  PyTypeObject *tp = Py_TYPE(obj);
  if (tp == &PyFloat_Type) return 16;
  if (tp == &PyLong_Type) return 8;
  if (tp == &PyUnicode_Type) return PyUnicode_GET_LENGTH(obj) + 4;
  if (tp == &PyBool_Type || obj == Py_None) return 6;
  if (tp == &PyList_Type || tp == &PyTuple_Type) {
    Py_ssize_t n = PySequence_Fast_GET_SIZE(obj);
    if (n == 0) return 2;
    if (depth_left <= 0) return n * 16 + 4;
    Py_ssize_t elem_b = estimate_elem_bytes_o1(PySequence_Fast_GET_ITEM(obj, 0), depth_left - 1);
    return n * elem_b + 4;
  }
  if (tp == &PyDict_Type) {
    Py_ssize_t n = PyDict_GET_SIZE(obj);
    if (n == 0) return 2;
    if (depth_left <= 0) return n * 32 + 8;
    Py_ssize_t pos = 0, sampled = 0, sum = 0;
    PyObject *k, *v;
    while (sampled < 4 && PyDict_Next(obj, &pos, &k, &v)) {
      sum += 16 + estimate_elem_bytes_o1(v, depth_left - 1);
      sampled++;
    }
    return (sampled > 0) ? (sum * n / sampled + 8) : (n * 32 + 8);
  }
  return 16;
}
#endif

static char *encode_array_fast(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *obj) {
  Py_ssize_t len = PySequence_Fast_GET_SIZE(obj);
  char *safe_end = *safe_end_p;

  if (CJSON_UNLIKELY(len == 0)) {
    dst[0] = '[';
    dst[1] = ']';
    return dst + 2;
  }
  if (CJSON_UNLIKELY(b->depth + 1 > CJSON_MAX_DEPTH)) {
    PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
    return NULL;
  }

#if defined(CJSON_PARALLEL_SER)
  if (CJSON_UNLIKELY(len >= 64 && b->depth <= 4 && b->default_fn == NULL && g_par_pool.max_threads > 1)) {
    PyObject **pitems = PySequence_Fast_ITEMS(obj);
    Py_ssize_t e0 = estimate_elem_bytes_o1(pitems[0], 3);
    Py_ssize_t e1 = estimate_elem_bytes_o1(pitems[len >> 1], 3);
    Py_ssize_t e2 = estimate_elem_bytes_o1(pitems[len - 1], 3);
    Py_ssize_t elem_b = (e0 + e1 + e2) / 3;
    Py_ssize_t est_bytes = (elem_b > (PY_SSIZE_T_MAX / len)) ? (16 * 1024 * 1024) : (len * elem_b);
    if (est_bytes >= 1024 * 1024) {
      b->len = (Py_ssize_t)(dst - b->data);
      int prc = cjson_parallel_encode_slices(b, 0, pitems, NULL, len, est_bytes);
      if (prc < 0) return NULL;
      if (prc > 0) {
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) return NULL;
        }
        *safe_end_p = b->data + b->cap - 128;
        return b->data + b->len;
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }
  }
#endif

  b->depth++;
  *dst++ = '[';

  PyObject **items = PySequence_Fast_ITEMS(obj);
  Py_ssize_t i = 0;

  if (len >= 4) {
    PyTypeObject *tp0 = Py_TYPE(items[0]);
    if (tp0 == &PyLong_Type) {
      while (i < len) {
        Py_ssize_t rem = len - i;
        Py_ssize_t chunk = (rem > 64) ? 64 : rem;
        if (CJSON_UNLIKELY(dst + chunk * 24 + 128 > b->data + b->cap)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + chunk * 24 + 256) < 0) {
            b->depth--;
            return NULL;
          }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        Py_ssize_t end_i = i + chunk;
        for (; i < end_i; i++) {
          PyObject *item = items[i];
          if (CJSON_UNLIKELY(Py_TYPE(item) != &PyLong_Type)) goto general_loop;
          char *next = write_py_long(dst, item);
          if (CJSON_UNLIKELY(next == NULL)) {
            b->depth--;
            return NULL;
          }
          dst = next;
          *dst++ = ',';
        }
      }
      dst[-1] = ']';
      b->depth--;
      *safe_end_p = safe_end;
      return dst;
    }
    if (tp0 == &PyFloat_Type) {
      while (i < len) {
        Py_ssize_t rem = len - i;
        Py_ssize_t chunk = (rem > 64) ? 64 : rem;
        if (CJSON_UNLIKELY(dst + chunk * 40 + 128 > b->data + b->cap)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + chunk * 40 + 256) < 0) {
            b->depth--;
            return NULL;
          }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        Py_ssize_t end_i = i + chunk;
        for (; i < end_i; i++) {
          PyObject *item = items[i];
          if (CJSON_UNLIKELY(Py_TYPE(item) != &PyFloat_Type)) goto general_loop;
          double d = PyFloat_AS_DOUBLE(item);
          if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
            dst = write_double_fast(d, dst);
          } else {
            memcpy(dst, "null", 4);
            dst += 4;
          }
          *dst++ = ',';
        }
      }
      dst[-1] = ']';
      b->depth--;
      *safe_end_p = safe_end;
      return dst;
    }
    if (tp0 == &PyBool_Type || items[0] == Py_None) {
      while (i < len) {
        Py_ssize_t rem = len - i;
        Py_ssize_t chunk = (rem > 64) ? 64 : rem;
        if (CJSON_UNLIKELY(dst + chunk * 6 + 128 > b->data + b->cap)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + chunk * 6 + 256) < 0) {
            b->depth--;
            return NULL;
          }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        Py_ssize_t end_i = i + chunk;
        for (; i < end_i; i++) {
          PyObject *item = items[i];
          if (item == Py_True) {
            memcpy(dst, "true,", 5);
            dst += 5;
          } else if (item == Py_False) {
            memcpy(dst, "false,", 6);
            dst += 6;
          } else if (item == Py_None) {
            memcpy(dst, "null,", 5);
            dst += 5;
          } else {
            goto general_loop;
          }
        }
      }
      dst[-1] = ']';
      b->depth--;
      *safe_end_p = safe_end;
      return dst;
    }
    if (tp0 == &PyUnicode_Type || PyType_FastSubclass(tp0, Py_TPFLAGS_UNICODE_SUBCLASS)) {
#if !defined(CJSON_NO_SIMD) && defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
      if (len >= 32 && tp0 == &PyUnicode_Type && CJSON_LIKELY(cjson_cpu_has_avx2)) {
        while (i < len) {
          if (CJSON_UNLIKELY(dst > safe_end)) {
            b->len = (Py_ssize_t)(dst - b->data);
            Py_ssize_t rem = len - i;
            Py_ssize_t add = (rem < 256) ? (rem * 16 + 128) : 4096;
            if (json_buffer_grow(b, b->len + add) < 0) {
              b->depth--;
              return NULL;
            }
            dst = b->data + b->len;
            safe_end = b->data + b->cap - 128;
          }
          dst = cjson_encode_str_array_avx2(items, len, &i, dst, safe_end);
          if (i >= len) break;
          if (dst > safe_end) continue;
          PyObject *item = items[i];
          if (CJSON_UNLIKELY(Py_TYPE(item) != &PyUnicode_Type)) goto general_loop;
          dst = emit_str_inline(b, dst, &safe_end, item);
          if (CJSON_UNLIKELY(dst == NULL)) {
            b->depth--;
            return NULL;
          }
          *dst++ = ',';
          i++;
        }
        dst[-1] = ']';
        b->depth--;
        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        *safe_end_p = safe_end;
        return dst;
      }
#endif
      for (; i < len; i++) {
        PyObject *item = items[i];
        if (CJSON_UNLIKELY(Py_TYPE(item) != tp0)) goto general_loop;
        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          Py_ssize_t rem = len - i;
          Py_ssize_t add = (rem < 256) ? (rem * 16 + 128) : 4096;
          if (json_buffer_grow(b, b->len + add) < 0) {
            b->depth--;
            return NULL;
          }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        dst = emit_str_inline(b, dst, &safe_end, item);
        if (CJSON_UNLIKELY(dst == NULL)) {
          b->depth--;
          return NULL;
        }
        *dst++ = ',';
      }
      dst[-1] = ']';
      b->depth--;
      if (CJSON_UNLIKELY(dst > safe_end)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      }
      *safe_end_p = safe_end;
      return dst;
    }
    if (tp0 == &PyDict_Type) {
      if (len >= 4) {
        dst = encode_dict_array_cached(b, dst, &safe_end, items, len, &i);
        if (CJSON_UNLIKELY(dst == NULL)) {
          b->depth--;
          return NULL;
        }
        if (CJSON_UNLIKELY(i < len)) goto general_loop;
        dst[-1] = ']';
        b->depth--;
        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        *safe_end_p = safe_end;
        return dst;
      }
      for (; i < len; i++) {
        PyObject *item = items[i];
        if (CJSON_UNLIKELY(Py_TYPE(item) != &PyDict_Type)) goto general_loop;
        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          Py_ssize_t rem = len - i;
          Py_ssize_t add = (rem < 256) ? (rem * 16 + 128) : 4096;
          if (json_buffer_grow(b, b->len + add) < 0) {
            b->depth--;
            return NULL;
          }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (encode_fast_iter(b, item) < 0) {
            b->depth--;
            return NULL;
          }
          if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
            if (json_buffer_grow(b, b->len + 256) < 0) {
              b->depth--;
              return NULL;
            }
          }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        } else if (PyDict_GET_SIZE(item) == 0) {
          dst[0] = '{';
          dst[1] = '}';
          dst += 2;
        } else {
          dst = encode_dict_fast(b, dst, &safe_end, item);
          if (CJSON_UNLIKELY(dst == NULL)) {
            b->depth--;
            return NULL;
          }
        }
        *dst++ = ',';
      }
      dst[-1] = ']';
      b->depth--;
      if (CJSON_UNLIKELY(dst > safe_end)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      }
      *safe_end_p = safe_end;
      return dst;
    }
  }

general_loop:;
  for (; i < len; i++) {
    PyObject *item = items[i];
    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      Py_ssize_t rem = len - i;
      Py_ssize_t add = (rem < 512) ? (rem * 8 + 128) : 4096;
      if (json_buffer_grow(b, b->len + add) < 0) {
        b->depth--;
        return NULL;
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }
    PyTypeObject *tp = Py_TYPE(item);
    if (tp == &PyUnicode_Type) {
      dst = emit_str_inline(b, dst, &safe_end, item);
      if (CJSON_UNLIKELY(dst == NULL)) {
        b->depth--;
        return NULL;
      }
    } else if (tp == &PyLong_Type) {
      char *next = write_py_long(dst, item);
      if (CJSON_UNLIKELY(next == NULL)) {
        b->depth--;
        return NULL;
      }
      dst = next;
    } else if (tp == &PyFloat_Type) {
      double d = PyFloat_AS_DOUBLE(item);
      if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
        dst = write_double_fast(d, dst);
      } else {
        memcpy(dst, "null", 4);
        dst += 4;
      }
    } else if (item == Py_None) {
      memcpy(dst, "null", 4);
      dst += 4;
    } else if (tp == &PyBool_Type) {
      if (item == Py_True) {
        memcpy(dst, "true", 4);
        dst += 4;
      } else {
        memcpy(dst, "false", 5);
        dst += 5;
      }
    } else if (tp == &PyList_Type || tp == &PyTuple_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (encode_fast_iter(b, item) < 0) { b->depth--; return NULL; }
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PySequence_Fast_GET_SIZE(item) == 0) {
        dst[0] = '[';
        dst[1] = ']';
        dst += 2;
      } else {
        dst = encode_array_fast(b, dst, &safe_end, item);
        if (CJSON_UNLIKELY(dst == NULL)) {
          b->depth--;
          return NULL;
        }
      }
    } else if (tp == &PyDict_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (encode_fast_iter(b, item) < 0) { b->depth--; return NULL; }
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PyDict_GET_SIZE(item) == 0) {
        dst[0] = '{';
        dst[1] = '}';
        dst += 2;
      } else {
        dst = encode_dict_fast(b, dst, &safe_end, item);
        if (CJSON_UNLIKELY(dst == NULL)) {
          b->depth--;
          return NULL;
        }
      }
    } else {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_encode_slow(b, item) < 0) {
        b->depth--;
        return NULL;
      }
      if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
        if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }
    *dst++ = ',';
  }

  dst[-1] = ']';
  b->depth--;
  if (CJSON_UNLIKELY(dst > safe_end)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }
  *safe_end_p = safe_end;
  return dst;
}

static char *encode_dict_fast(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *obj) {
  Py_ssize_t dict_size = PyDict_GET_SIZE(obj);
  char *safe_end = *safe_end_p;

  if (CJSON_UNLIKELY(dict_size == 0)) {
    dst[0] = '{';
    dst[1] = '}';
    return dst + 2;
  }
  if (CJSON_UNLIKELY(b->depth + 1 > CJSON_MAX_DEPTH)) {
    PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
    return NULL;
  }

#if defined(CJSON_PARALLEL_SER)
  if (CJSON_UNLIKELY(dict_size >= 64 && b->depth <= 2 && b->default_fn == NULL && g_par_pool.max_threads > 1)) {
    PyObject *k0 = NULL, *v0 = NULL;
    Py_ssize_t p0 = 0, sampled = 0, sum_est = 0;
    while (sampled < 4 && PyDict_Next(obj, &p0, &k0, &v0)) {
      sum_est += 16 + estimate_elem_bytes_o1(v0, 3);
      sampled++;
    }
    Py_ssize_t avg_est = (sampled > 0) ? (sum_est / sampled) : 0;
    Py_ssize_t dict_est = (avg_est > (PY_SSIZE_T_MAX / dict_size)) ? (16 * 1024 * 1024) : (dict_size * avg_est);
    if (dict_est >= 1024 * 1024) {
      ParDictPair *pairs = (ParDictPair *)malloc((size_t)dict_size * sizeof(ParDictPair));
      if (pairs != NULL) {
        Py_ssize_t pos = 0, cnt = 0;
        PyObject *pk, *pv;
        while (PyDict_Next(obj, &pos, &pk, &pv)) {
          pairs[cnt++] = (ParDictPair){pk, pv};
        }
        b->len = (Py_ssize_t)(dst - b->data);
        int prc = cjson_parallel_encode_slices(b, 1, NULL, pairs, cnt, dict_est);
        free(pairs);
        if (prc < 0) return NULL;
        if (prc > 0) {
          if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
            if (json_buffer_grow(b, b->len + 256) < 0) return NULL;
          }
          *safe_end_p = b->data + b->cap - 128;
          return b->data + b->len;
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      }
    }
  }
#endif

  b->depth++;
  *dst++ = '{';

  PyObject *key, *dval;
  Py_ssize_t pos = 0;
  while (PyDict_Next(obj, &pos, &key, &dval)) {
    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + 512) < 0) {
        b->depth--;
        return NULL;
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }

    PyTypeObject *ktp = Py_TYPE(key);
    if (CJSON_UNLIKELY(ktp != &PyUnicode_Type && !PyType_FastSubclass(ktp, Py_TPFLAGS_UNICODE_SUBCLASS))) {
      b->depth--;
      PyErr_SetString(PyExc_TypeError, "Dict key must be str");
      return NULL;
    }

    dst = emit_str_inline(b, dst, &safe_end, key);
    if (CJSON_UNLIKELY(dst == NULL)) {
      b->depth--;
      return NULL;
    }
    *dst++ = ':';

    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + 256) < 0) {
        b->depth--;
        return NULL;
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }

    PyTypeObject *vtp = Py_TYPE(dval);
    if (vtp == &PyUnicode_Type) {
      dst = emit_str_inline(b, dst, &safe_end, dval);
      if (CJSON_UNLIKELY(dst == NULL)) {
        b->depth--;
        return NULL;
      }
    } else if (vtp == &PyLong_Type) {
      char *next = write_py_long(dst, dval);
      if (CJSON_UNLIKELY(next == NULL)) {
        b->depth--;
        return NULL;
      }
      dst = next;
    } else if (vtp == &PyFloat_Type) {
      double d = PyFloat_AS_DOUBLE(dval);
      if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
        dst = write_double_fast(d, dst);
      } else {
        memcpy(dst, "null", 4);
        dst += 4;
      }
    } else if (dval == Py_None) {
      memcpy(dst, "null", 4);
      dst += 4;
    } else if (vtp == &PyBool_Type) {
      if (dval == Py_True) {
        memcpy(dst, "true", 4);
        dst += 4;
      } else {
        memcpy(dst, "false", 5);
        dst += 5;
      }
    } else if (vtp == &PyList_Type || vtp == &PyTuple_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (encode_fast_iter(b, dval) < 0) { b->depth--; return NULL; }
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PySequence_Fast_GET_SIZE(dval) == 0) {
        dst[0] = '[';
        dst[1] = ']';
        dst += 2;
      } else {
        dst = encode_array_fast(b, dst, &safe_end, dval);
        if (CJSON_UNLIKELY(dst == NULL)) {
          b->depth--;
          return NULL;
        }
      }
    } else if (vtp == &PyDict_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (encode_fast_iter(b, dval) < 0) { b->depth--; return NULL; }
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PyDict_GET_SIZE(dval) == 0) {
        dst[0] = '{';
        dst[1] = '}';
        dst += 2;
      } else {
        dst = encode_dict_fast(b, dst, &safe_end, dval);
        if (CJSON_UNLIKELY(dst == NULL)) {
          b->depth--;
          return NULL;
        }
      }
    } else {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_encode_slow(b, dval) < 0) {
        b->depth--;
        return NULL;
      }
      if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
        if (json_buffer_grow(b, b->len + 256) < 0) { b->depth--; return NULL; }
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }
    *dst++ = ',';
  }

  dst[-1] = '}';
  b->depth--;
  if (CJSON_UNLIKELY(dst > safe_end)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }
  *safe_end_p = safe_end;
  return dst;
}

/* O(1) adaptive root buffer pre-sizing: avoids 5-10 realloc+memcpy passes on
 * medium/large containers while keeping small payloads on the 4KB stack buffer. */
static inline void presize_root_buffer(JsonBuffer *b, PyObject *obj, int indent) {
  PyTypeObject *tp = Py_TYPE(obj);
  Py_ssize_t est = 0;
  if (tp == &PyList_Type || tp == &PyTuple_Type) {
    Py_ssize_t len = PySequence_Fast_GET_SIZE(obj);
    if (len >= 16) {
      PyObject *first = PySequence_Fast_GET_ITEM(obj, 0);
      PyTypeObject *ft = Py_TYPE(first);
      if (ft == &PyLong_Type) {
        est = len * 6;
      } else if (ft == &PyFloat_Type) {
        est = len * 12;
      } else if (ft == &PyUnicode_Type) {
        Py_ssize_t sl = PyUnicode_GET_LENGTH(first);
        est = len * ((sl < 128 ? sl : 128) + 4);
      } else if (ft == &PyDict_Type) {
        Py_ssize_t ds = PyDict_GET_SIZE(first);
        est = len * ((ds < 64 ? ds : 64) * 20 + 8);
      } else if (ft == &PyList_Type || ft == &PyTuple_Type) {
        Py_ssize_t ls = PySequence_Fast_GET_SIZE(first);
        est = len * ((ls < 64 ? ls : 64) * 10 + 4);
      } else {
        est = len * 12;
      }
    }
  } else if (tp == &PyDict_Type) {
    Py_ssize_t size = PyDict_GET_SIZE(obj);
    if (size >= 32) {
      est = size * 24;
    }
  }
  if (indent && est > 0) {
    est += (est >> 1);
  }
  if (est > CJSON_STACK_BUF_SIZE) {
    if (est > 16 * 1024 * 1024) est = 16 * 1024 * 1024;
    PyObject *bo = PyBytes_FromStringAndSize(NULL, est);
    if (CJSON_LIKELY(bo != NULL)) {
      b->bytes_obj = bo;
      b->data = PyBytes_AS_STRING(bo);
      b->cap = est;
    } else {
      PyErr_Clear();
    }
  }
}

static int encode_root_fast(JsonBuffer *b, PyObject *obj) {
  presize_root_buffer(b, obj, 0);
  if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
    if (json_buffer_grow(b, b->len + 256) < 0) return -1;
  }
  PyTypeObject *tp = Py_TYPE(obj);
  if (tp == &PyDict_Type) {
    char *safe_end = b->data + b->cap - 128;
    char *dst = encode_dict_fast(b, b->data + b->len, &safe_end, obj);
    if (CJSON_UNLIKELY(dst == NULL)) return -1;
    b->len = (Py_ssize_t)(dst - b->data);
    return 0;
  }
  if (tp == &PyList_Type || tp == &PyTuple_Type) {
    char *safe_end = b->data + b->cap - 128;
    char *dst = encode_array_fast(b, b->data + b->len, &safe_end, obj);
    if (CJSON_UNLIKELY(dst == NULL)) return -1;
    b->len = (Py_ssize_t)(dst - b->data);
    return 0;
  }
  if (tp == &PyUnicode_Type) return json_buffer_append_string(b, obj);
  if (tp == &PyLong_Type) return json_buffer_append_int(b, obj);
  if (tp == &PyFloat_Type) return json_buffer_append_double(b, obj);
  if (obj == Py_None) return json_buffer_append_null(b);
  if (tp == &PyBool_Type) return (obj == Py_True) ? json_buffer_append_true(b) : json_buffer_append_false(b);
  return json_encode_slow(b, obj);
}

/* ========================================================================= */
/* Iterative Fast-Path Fallback (depth >= CJSON_REC_SER_DEPTH up to 1024)    */
/* ========================================================================= */

typedef struct {
  PyObject *obj;
  PyObject **items;
  Py_ssize_t pos;
  Py_ssize_t size;
  uint8_t is_dict;
  uint8_t need_comma;
} SerFrame;

__attribute__((noinline))
static int encode_fast_iter(JsonBuffer *b, PyObject *root) {
  SerFrame stack_frames[64];
  SerFrame *frames = stack_frames;
  size_t frame_sp = 0;
  size_t frame_cap = 64;
  int base_depth = b->depth;

  char *dst = b->data + b->len;
  char *safe_end = b->data + b->cap - 128;

  if (CJSON_UNLIKELY(dst > safe_end)) {
    if (json_buffer_grow(b, b->len + 256) < 0) return -1;
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }

  PyObject *val = root;

emit_val:;
  {
    PyTypeObject *tp = Py_TYPE(val);

    if (tp == &PyUnicode_Type) {
      dst = emit_str_inline(b, dst, &safe_end, val);
      if (CJSON_UNLIKELY(dst == NULL)) goto fail;
      goto value_emitted;
    }

    if (tp == &PyLong_Type) {
      dst = write_py_long(dst, val);
      if (CJSON_UNLIKELY(dst == NULL)) goto fail;
      goto value_emitted;
    }

    if (tp == &PyFloat_Type) {
      double d = PyFloat_AS_DOUBLE(val);
      if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
        dst = write_double_fast(d, dst);
      } else {
        memcpy(dst, "null", 4);
        dst += 4;
      }
      goto value_emitted;
    }

    if (val == Py_None) {
      memcpy(dst, "null", 4);
      dst += 4;
      goto value_emitted;
    }

    if (tp == &PyBool_Type) {
      if (val == Py_True) {
        memcpy(dst, "true", 4);
        dst += 4;
      } else {
        memcpy(dst, "false", 5);
        dst += 5;
      }
      goto value_emitted;
    }

    if (tp == &PyList_Type || tp == &PyTuple_Type) {
      Py_ssize_t len = PySequence_Fast_GET_SIZE(val);
      if (len == 0) {
        dst[0] = '[';
        dst[1] = ']';
        dst += 2;
        goto value_emitted;
      }
      if (CJSON_UNLIKELY(base_depth + (int)frame_sp + 1 > CJSON_MAX_DEPTH)) {
        PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
        goto fail;
      }
      if (CJSON_UNLIKELY(frame_sp >= frame_cap)) {
        size_t new_cap = frame_cap * 2;
        SerFrame *nf;
        if (frames == stack_frames) {
          nf = (SerFrame *)PyMem_Malloc(new_cap * sizeof(SerFrame));
          if (nf) memcpy(nf, stack_frames, frame_sp * sizeof(SerFrame));
        } else {
          nf = (SerFrame *)PyMem_Realloc(frames, new_cap * sizeof(SerFrame));
        }
        if (!nf) { PyErr_NoMemory(); goto fail; }
        frames = nf;
        frame_cap = new_cap;
      }
      *dst++ = '[';
      frames[frame_sp++] = (SerFrame){val, PySequence_Fast_ITEMS(val), 0, len, 0, 0};
      goto next_item;
    }

    if (tp == &PyDict_Type) {
      Py_ssize_t dict_size = PyDict_GET_SIZE(val);
      if (dict_size == 0) {
        dst[0] = '{';
        dst[1] = '}';
        dst += 2;
        goto value_emitted;
      }
      if (CJSON_UNLIKELY(base_depth + (int)frame_sp + 1 > CJSON_MAX_DEPTH)) {
        PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
        goto fail;
      }
      if (CJSON_UNLIKELY(frame_sp >= frame_cap)) {
        size_t new_cap = frame_cap * 2;
        SerFrame *nf;
        if (frames == stack_frames) {
          nf = (SerFrame *)PyMem_Malloc(new_cap * sizeof(SerFrame));
          if (nf) memcpy(nf, stack_frames, frame_sp * sizeof(SerFrame));
        } else {
          nf = (SerFrame *)PyMem_Realloc(frames, new_cap * sizeof(SerFrame));
        }
        if (!nf) { PyErr_NoMemory(); goto fail; }
        frames = nf;
        frame_cap = new_cap;
      }
      *dst++ = '{';
      frames[frame_sp++] = (SerFrame){val, NULL, 0, dict_size, 1, 0};
      goto next_item;
    }

    /* Slow path fallback */
    b->len = (Py_ssize_t)(dst - b->data);
    b->depth = base_depth + (int)frame_sp;
    if (json_encode_slow(b, val) < 0) goto fail;
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }

value_emitted:;
  if (CJSON_UNLIKELY(frame_sp == 0)) {
    b->len = (Py_ssize_t)(dst - b->data);
    b->depth = base_depth;
    if (frames != stack_frames) PyMem_Free(frames);
    return 0;
  }

next_item:;
  {
    SerFrame *f = &frames[frame_sp - 1];
    if (!f->is_dict) {
      if (f->pos >= f->size) {
        frame_sp--;
        if (CJSON_UNLIKELY(dst >= b->data + b->cap)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_reserve(b, 1) < 0) goto fail;
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
        *dst++ = ']';
        goto value_emitted;
      }
      if (CJSON_UNLIKELY(dst > safe_end)) {
        b->len = (Py_ssize_t)(dst - b->data);
        Py_ssize_t rem = f->size - f->pos;
        Py_ssize_t add = (rem < 512) ? (rem * 8 + 128) : 4096;
        if (json_buffer_grow(b, b->len + add) < 0) goto fail;
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      }
      if (f->pos > 0) {
        *dst++ = ',';
      }
      val = f->items[f->pos++];
      goto emit_val;
    }

    PyObject *key;
    PyObject *dval;
    if (!PyDict_Next(f->obj, &f->pos, &key, &dval)) {
      frame_sp--;
      if (CJSON_UNLIKELY(dst >= b->data + b->cap)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (json_buffer_reserve(b, 1) < 0) goto fail;
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      }
      *dst++ = '}';
      goto value_emitted;
    }

    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + 512) < 0) goto fail;
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }

    if (f->need_comma) {
      *dst++ = ',';
    }
    f->need_comma = 1;

    PyTypeObject *ktp = Py_TYPE(key);
    if (CJSON_UNLIKELY(ktp != &PyUnicode_Type && !PyType_FastSubclass(ktp, Py_TPFLAGS_UNICODE_SUBCLASS))) {
      PyErr_SetString(PyExc_TypeError, "Dict key must be str");
      goto fail;
    }

    dst = emit_str_inline(b, dst, &safe_end, key);
    if (CJSON_UNLIKELY(dst == NULL)) goto fail;
    *dst++ = ':';
    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }

    val = dval;
    goto emit_val;
  }

fail:
  b->depth = base_depth;
  if (frames != stack_frames) PyMem_Free(frames);
  return -1;
}

/* ========================================================================= */
/* Options-aware Paths (OPT_INDENT_2, OPT_SORT_KEYS, OPT_NON_STR_KEYS, etc.) */
/* ========================================================================= */

static PyObject *stringify_non_str_key(PyObject *key) {
  if (PyUnicode_Check(key)) {
    Py_INCREF(key);
    return key;
  }
  if (PyBool_Check(key)) {
    return PyUnicode_FromString(key == Py_True ? "true" : "false");
  }
  if (key == Py_None) {
    return PyUnicode_FromString("null");
  }
  if (PyLong_Check(key) || PyFloat_Check(key)) {
    return PyObject_Str(key);
  }
  if (PyDate_Check(key) || PyDateTime_Check(key) || PyTime_Check(key)) {
    return PyObject_CallMethod(key, "isoformat", NULL);
  }
  if (g_uuid_type != NULL && PyObject_IsInstance(key, g_uuid_type)) {
    return PyObject_Str(key);
  }
  PyErr_SetString(PyExc_TypeError, "Dict key must a type serializable with OPT_NON_STR_KEYS");
  return NULL;
}

typedef struct {
  uint64_t prefix;
  const char *key_utf8;
  uint32_t key_len;
  int is_temp_key;
  PyObject *key_str;
  PyObject *value;
} DictSortEntry;

static inline uint64_t dict_key_prefix(const char *s, Py_ssize_t len) {
  if (CJSON_LIKELY(len >= 8)) {
    uint64_t v;
    memcpy(&v, s, 8);
#if CJSON_LITTLE_ENDIAN
    return cjson_bswap64(v);
#else
    return v;
#endif
  }
  uint64_t v = 0;
  if (len >= 4) {
    uint32_t w;
    memcpy(&w, s, 4);
#if CJSON_LITTLE_ENDIAN
    v = ((uint64_t)cjson_bswap32(w)) << 32;
#else
    v = ((uint64_t)w) << 32;
#endif
    const unsigned char *p = (const unsigned char *)s + 4;
    if (len >= 6) {
      v |= ((uint64_t)p[0] << 24) | ((uint64_t)p[1] << 16);
      if (len == 7) v |= ((uint64_t)p[2] << 8);
    } else if (len == 5) {
      v |= ((uint64_t)p[0] << 24);
    }
    return v;
  }
  const unsigned char *p = (const unsigned char *)s;
  if (len >= 2) {
    v = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48);
    if (len == 3) v |= ((uint64_t)p[2] << 40);
  } else if (len == 1) {
    v = (uint64_t)p[0] << 56;
  }
  return v;
}

static inline int compare_dict_sort_entries_inline(const DictSortEntry *ea, const DictSortEntry *eb) {
  if (ea->prefix != eb->prefix) {
    return (ea->prefix < eb->prefix) ? -1 : 1;
  }
  uint32_t min_len = ea->key_len < eb->key_len ? ea->key_len : eb->key_len;
  if (CJSON_UNLIKELY(min_len > 8)) {
    int cmp = memcmp(ea->key_utf8 + 8, eb->key_utf8 + 8, (size_t)(min_len - 8));
    if (cmp != 0) return cmp;
  }
  return (ea->key_len < eb->key_len) ? -1 : (ea->key_len > eb->key_len ? 1 : 0);
}

static int compare_dict_sort_entries(const void *a, const void *b) {
  return compare_dict_sort_entries_inline((const DictSortEntry *)a, (const DictSortEntry *)b);
}

static inline void sort_entries_insertion(DictSortEntry *e, Py_ssize_t n) {
  if (n <= 1) return;
  if (n == 2) {
    if (compare_dict_sort_entries_inline(&e[0], &e[1]) > 0) {
      DictSortEntry tmp = e[0]; e[0] = e[1]; e[1] = tmp;
    }
    return;
  }
  if (n == 3) {
    if (compare_dict_sort_entries_inline(&e[0], &e[1]) > 0) {
      DictSortEntry t = e[0]; e[0] = e[1]; e[1] = t;
    }
    if (compare_dict_sort_entries_inline(&e[1], &e[2]) > 0) {
      DictSortEntry t = e[1]; e[1] = e[2]; e[2] = t;
      if (compare_dict_sort_entries_inline(&e[0], &e[1]) > 0) {
        DictSortEntry t2 = e[0]; e[0] = e[1]; e[1] = t2;
      }
    }
    return;
  }
  for (Py_ssize_t i = 1; i < n; i++) {
    DictSortEntry key = e[i];
    Py_ssize_t j = i - 1;
    while (j >= 0 && compare_dict_sort_entries_inline(&e[j], &key) > 0) {
      e[j + 1] = e[j];
      j--;
    }
    e[j + 1] = key;
  }
}

static inline int compare_dict_sort_entries_at(const DictSortEntry *ea, const DictSortEntry *eb, uint32_t depth) {
  if (ea->prefix != eb->prefix) {
    return (ea->prefix < eb->prefix) ? -1 : 1;
  }
  uint32_t min_len = ea->key_len < eb->key_len ? ea->key_len : eb->key_len;
  uint32_t off = (depth < 8) ? 8u : (depth + 8u);
  if (CJSON_UNLIKELY(min_len > off)) {
    int cmp = memcmp(ea->key_utf8 + off, eb->key_utf8 + off, (size_t)(min_len - off));
    if (cmp != 0) return cmp;
  }
  return (ea->key_len < eb->key_len) ? -1 : (ea->key_len > eb->key_len ? 1 : 0);
}

static inline void sort_entries_insertion_at(DictSortEntry *e, Py_ssize_t n, uint32_t depth) {
  if (n <= 1) return;
  if (depth < 8) {
    sort_entries_insertion(e, n);
    return;
  }
  for (Py_ssize_t i = 1; i < n; i++) {
    DictSortEntry key = e[i];
    Py_ssize_t j = i - 1;
    while (j >= 0 && compare_dict_sort_entries_at(&e[j], &key, depth) > 0) {
      e[j + 1] = e[j];
      j--;
    }
    e[j + 1] = key;
  }
}

static CJSON_NOINLINE void sort_entries_qsort_at(DictSortEntry *e, Py_ssize_t n, uint32_t start_depth) {
  /* Iterative quicksort with median-of-3 pivot, insertion sort for <=24,
   * and qsort fallback if recursion depth exceeds 2*log2(n). */
  struct { Py_ssize_t lo; Py_ssize_t hi; int depth; } stack[64];
  int sp = 0;
  int max_depth = 0;
  for (Py_ssize_t tmp = n; tmp > 0; tmp >>= 1) max_depth += 2;
  stack[sp].lo = 0;
  stack[sp].hi = n - 1;
  stack[sp].depth = max_depth;
  sp++;

  while (sp > 0) {
    sp--;
    Py_ssize_t lo = stack[sp].lo;
    Py_ssize_t hi = stack[sp].hi;
    int depth = stack[sp].depth;

    while (hi - lo + 1 > 24) {
      if (CJSON_UNLIKELY(depth <= 0 || sp >= 62)) {
        qsort(e + lo, (size_t)(hi - lo + 1), sizeof(DictSortEntry), compare_dict_sort_entries);
        lo = hi;
        break;
      }
      depth--;
      Py_ssize_t mid = lo + ((hi - lo) >> 1);
      if (compare_dict_sort_entries_at(&e[lo], &e[mid], start_depth) > 0) {
        DictSortEntry t = e[lo]; e[lo] = e[mid]; e[mid] = t;
      }
      if (compare_dict_sort_entries_at(&e[lo], &e[hi], start_depth) > 0) {
        DictSortEntry t = e[lo]; e[lo] = e[hi]; e[hi] = t;
      }
      if (compare_dict_sort_entries_at(&e[mid], &e[hi], start_depth) > 0) {
        DictSortEntry t = e[mid]; e[mid] = e[hi]; e[hi] = t;
      }
      DictSortEntry pivot = e[mid];
      Py_ssize_t i = lo, j = hi;
      for (;;) {
        while (compare_dict_sort_entries_at(&e[i], &pivot, start_depth) < 0) i++;
        while (compare_dict_sort_entries_at(&e[j], &pivot, start_depth) > 0) j--;
        if (i >= j) break;
        DictSortEntry t = e[i]; e[i] = e[j]; e[j] = t;
        i++;
        j--;
      }
      /* Push larger partition, loop on smaller for O(log n) stack */
      if (j - lo < hi - j) {
        if (j + 1 < hi) {
          stack[sp].lo = j + 1;
          stack[sp].hi = hi;
          stack[sp].depth = depth;
          sp++;
        }
        hi = j;
      } else {
        if (lo < j) {
          stack[sp].lo = lo;
          stack[sp].hi = j;
          stack[sp].depth = depth;
          sp++;
        }
        lo = j + 1;
      }
    }
    if (hi > lo) {
      sort_entries_insertion_at(e + lo, hi - lo + 1, start_depth);
    }
  }
}

static void sort_entries_radix(DictSortEntry *e, DictSortEntry *tmp, Py_ssize_t n, uint32_t pfx_base, uint32_t depth) {
  if (n <= 24) {
    sort_entries_insertion_at(e, n, pfx_base);
    return;
  }
  if (n < 128 || depth >= 64) {
    sort_entries_qsort_at(e, n, pfx_base);
    return;
  }

  /* Find first differing byte offset d >= depth */
  uint32_t d = depth;
  if (d < pfx_base + 8) {
    uint64_t p0 = e[0].prefix;
    uint64_t diff = 0;
    for (Py_ssize_t i = 1; i < n; i++) {
      diff |= (p0 ^ e[i].prefix);
    }
    if (diff != 0) {
      uint32_t first_diff = pfx_base + (uint32_t)(cjson_clz64(diff) >> 3);
      if (first_diff > d) d = first_diff;
    } else {
      d = pfx_base + 8;
    }
  }
  if (d >= pfx_base + 8) {
    while (d < 64) {
      if (e[0].key_len <= d) break;
      unsigned char c0 = (unsigned char)e[0].key_utf8[d];
      int all_same = 1;
      for (Py_ssize_t i = 1; i < n; i++) {
        if (e[i].key_len <= d || (unsigned char)e[i].key_utf8[d] != c0) {
          all_same = 0;
          break;
        }
      }
      if (!all_same) break;
      d++;
    }
    if (d >= 64) {
      sort_entries_qsort_at(e, n, pfx_base);
      return;
    }
    /* Reload 8-byte prefix window starting at first differing byte d! */
    pfx_base = d;
    for (Py_ssize_t i = 0; i < n; i++) {
      Py_ssize_t rem = (e[i].key_len > pfx_base) ? (Py_ssize_t)(e[i].key_len - pfx_base) : 0;
      e[i].prefix = (rem > 0) ? dict_key_prefix(e[i].key_utf8 + pfx_base, rem) : 0ULL;
    }
  }

  Py_ssize_t counts[256];
  memset(counts, 0, sizeof(counts));
  unsigned int shift = (7u - (d - pfx_base)) * 8u;
  for (Py_ssize_t i = 0; i < n; i++) {
    counts[(e[i].prefix >> shift) & 0xFFu]++;
  }
  Py_ssize_t offsets[256];
  Py_ssize_t sum = 0;
  for (int b = 0; b < 256; b++) {
    offsets[b] = sum;
    sum += counts[b];
  }
  for (Py_ssize_t i = 0; i < n; i++) {
    unsigned int bk = (unsigned int)((e[i].prefix >> shift) & 0xFFu);
    tmp[offsets[bk]++] = e[i];
  }
  memcpy(e, tmp, (size_t)n * sizeof(DictSortEntry));

  Py_ssize_t off = 0;
  for (int b = 0; b < 256; b++) {
    Py_ssize_t cnt = counts[b];
    if (cnt > 1) {
      if (CJSON_UNLIKELY(cnt == n)) {
        sort_entries_qsort_at(e + off, cnt, pfx_base);
      } else if (cnt <= 24) {
        sort_entries_insertion_at(e + off, cnt, pfx_base);
      } else if (cnt < 128) {
        sort_entries_qsort_at(e + off, cnt, pfx_base);
      } else {
        sort_entries_radix(e + off, tmp + off, cnt, pfx_base, d + 1);
      }
    }
    off += cnt;
  }
}

static CJSON_NOINLINE void sort_entries_large(DictSortEntry *e, Py_ssize_t n) {
  /* O(N) check for already-sorted or reverse-sorted keys (common in JSON) */
  int asc = 1, desc = 1;
  for (Py_ssize_t i = 1; i < n; i++) {
    int cmp = compare_dict_sort_entries_inline(&e[i - 1], &e[i]);
    if (cmp > 0) asc = 0;
    if (cmp < 0) desc = 0;
    if (!asc && !desc) break;
  }
  if (asc) return;
  if (desc) {
    Py_ssize_t lo = 0, hi = n - 1;
    while (lo < hi) {
      DictSortEntry t = e[lo]; e[lo] = e[hi]; e[hi] = t;
      lo++; hi--;
    }
    return;
  }

  if (n >= 256) {
    DictSortEntry *tmp = (DictSortEntry *)PyMem_Malloc((size_t)n * sizeof(DictSortEntry));
    if (CJSON_LIKELY(tmp != NULL)) {
      sort_entries_radix(e, tmp, n, 0, 0);
      PyMem_Free(tmp);
      return;
    }
  }
  sort_entries_qsort_at(e, n, 0);
}

static inline void sort_dict_entries(DictSortEntry *e, Py_ssize_t n) {
  if (CJSON_LIKELY(n <= 24)) {
    sort_entries_insertion(e, n);
  } else {
    sort_entries_large(e, n);
  }
}

static int json_buffer_append_datetime_opts(SerializeContext *ctx, PyObject *obj) {
  JsonBuffer *b = ctx->b;
  if (json_buffer_reserve(b, 64) < 0) return -1;

  char *dst = b->data + b->len;
  char *start = dst;
  *dst++ = '"';

  int is_datetime = PyDateTime_Check(obj);
  int is_date = PyDate_Check(obj);
  int is_time = PyTime_Check(obj);

  if (is_datetime || is_date) {
    int year = PyDateTime_GET_YEAR(obj);
    int month = PyDateTime_GET_MONTH(obj);
    int day = PyDateTime_GET_DAY(obj);
    dst += write_u32(dst, (uint32_t)year);
    *dst++ = '-';
    memcpy(dst, &DIGIT_PAIRS[month * 2], 2);
    dst += 2;
    *dst++ = '-';
    memcpy(dst, &DIGIT_PAIRS[day * 2], 2);
    dst += 2;
  }

  if (is_datetime) *dst++ = 'T';

  int hour, minute, second, microsecond;
  PyObject *tz = NULL;
  if (is_datetime) {
    hour = PyDateTime_DATE_GET_HOUR(obj);
    minute = PyDateTime_DATE_GET_MINUTE(obj);
    second = PyDateTime_DATE_GET_SECOND(obj);
    microsecond = PyDateTime_DATE_GET_MICROSECOND(obj);
    tz = PyDateTime_DATE_GET_TZINFO(obj);
  } else {
    hour = PyDateTime_TIME_GET_HOUR(obj);
    minute = PyDateTime_TIME_GET_MINUTE(obj);
    second = PyDateTime_TIME_GET_SECOND(obj);
    microsecond = PyDateTime_TIME_GET_MICROSECOND(obj);
    tz = PyDateTime_TIME_GET_TZINFO(obj);
  }

  if (is_datetime || is_time) {
    memcpy(dst, &DIGIT_PAIRS[hour * 2], 2);
    dst += 2;
    *dst++ = ':';
    memcpy(dst, &DIGIT_PAIRS[minute * 2], 2);
    dst += 2;
    *dst++ = ':';
    memcpy(dst, &DIGIT_PAIRS[second * 2], 2);
    dst += 2;

    if (microsecond > 0 && !(ctx->opts & OPT_OMIT_MICROSECONDS)) {
      *dst++ = '.';
      uint32_t a = (uint32_t)microsecond / 10000;
      uint32_t rem = (uint32_t)microsecond - a * 10000;
      uint32_t b_part = rem / 100;
      uint32_t c = rem - b_part * 100;
      memcpy(dst, &DIGIT_PAIRS[a * 2], 2);
      memcpy(dst + 2, &DIGIT_PAIRS[b_part * 2], 2);
      memcpy(dst + 4, &DIGIT_PAIRS[c * 2], 2);
      dst += 6;
    }

    if (tz == NULL || tz == Py_None) {
      if (ctx->opts & OPT_NAIVE_UTC) {
        if (ctx->opts & OPT_UTC_Z) {
          *dst++ = 'Z';
        } else {
          memcpy(dst, "+00:00", 6);
          dst += 6;
        }
      }
    } else {
      PyObject *delta = PyObject_CallMethod(obj, "utcoffset", NULL);
      if (delta != NULL && delta != Py_None) {
        int days = PyDateTime_DELTA_GET_DAYS(delta);
        int seconds = PyDateTime_DELTA_GET_SECONDS(delta);
        Py_DECREF(delta);
        int total_seconds = days * 86400 + seconds;
        if (total_seconds == 0) {
          if (ctx->opts & OPT_UTC_Z) {
            *dst++ = 'Z';
          } else {
            memcpy(dst, "+00:00", 6);
            dst += 6;
          }
        } else {
          if (total_seconds < 0) {
            *dst++ = '-';
            total_seconds = -total_seconds;
          } else {
            *dst++ = '+';
          }
          int off_hour = total_seconds / 3600;
          int off_rem = total_seconds % 3600;
          int off_minute = off_rem / 60;
          int off_second = off_rem % 60;
          memcpy(dst, &DIGIT_PAIRS[off_hour * 2], 2);
          dst += 2;
          *dst++ = ':';
          memcpy(dst, &DIGIT_PAIRS[off_minute * 2], 2);
          dst += 2;
          if (off_second > 0) {
            *dst++ = ':';
            memcpy(dst, &DIGIT_PAIRS[off_second * 2], 2);
            dst += 2;
          }
        }
      } else {
        Py_XDECREF(delta);
        PyErr_Clear();
      }
    }
  }

  *dst++ = '"';
  b->len += (Py_ssize_t)(dst - start);
  return 0;
}

static inline int json_encode_value_opts(SerializeContext *ctx, PyObject *obj) {
  PyTypeObject *tp = Py_TYPE(obj);
  JsonBuffer *b = ctx->b;
  if (tp == &PyUnicode_Type) return json_buffer_append_string(b, obj);
  if (tp == &PyLong_Type) {
    if (CJSON_LIKELY(!(ctx->opts & OPT_STRICT_INTEGER))) {
      return json_buffer_append_int(b, obj);
    }
    return json_buffer_append_int_opts(ctx, obj);
  }
  if (obj == Py_None) return json_buffer_append_null(b);
  if (tp == &PyBool_Type) return (obj == Py_True) ? json_buffer_append_true(b) : json_buffer_append_false(b);
  if (tp == &PyFloat_Type) return json_buffer_append_double(b, obj);
  return json_encode_opts(ctx, obj);
}

static int json_buffer_append_array_opts(SerializeContext *ctx, PyObject *obj) {
  JsonBuffer *b = ctx->b;
  Py_ssize_t len = PySequence_Fast_GET_SIZE(obj);
  PyObject **items = PySequence_Fast_ITEMS(obj);

  if (len == 0) {
    if (json_buffer_reserve(b, 2) < 0) return -1;
    b->data[b->len++] = '[';
    b->data[b->len++] = ']';
    return 0;
  }

  int indent = (ctx->opts & OPT_INDENT_2);
  if (indent) {
    ctx->indent_level++;
    if (append_indent_prefix(b, '[', ctx->indent_level) < 0) return -1;
  } else {
    if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
      if (json_buffer_reserve(b, 1) < 0) return -1;
    }
    b->data[b->len++] = '[';
  }

  for (Py_ssize_t i = 0; i < len; i++) {
    if (i > 0) {
      if (indent) {
        if (append_indent_sep(b, ctx->indent_level) < 0) return -1;
      } else {
        if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
          if (json_buffer_reserve(b, 1) < 0) return -1;
        }
        b->data[b->len++] = ',';
      }
    }
    if (json_encode_value_opts(ctx, items[i]) < 0) return -1;
  }

  if (indent) {
    ctx->indent_level--;
    return append_indent_close(b, ']', ctx->indent_level);
  }
  if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
    if (json_buffer_reserve(b, 1) < 0) return -1;
  }
  b->data[b->len++] = ']';
  return 0;
}

static int json_buffer_append_object_opts(SerializeContext *ctx, PyObject *obj) {
  JsonBuffer *b = ctx->b;
  Py_ssize_t dict_size = PyDict_GET_SIZE(obj);

  if (dict_size == 0) {
    if (json_buffer_reserve(b, 2) < 0) return -1;
    b->data[b->len++] = '{';
    b->data[b->len++] = '}';
    return 0;
  }

  int indent = (ctx->opts & OPT_INDENT_2);
  int non_str_keys = (ctx->opts & OPT_NON_STR_KEYS);
  int sort_keys = (ctx->opts & OPT_SORT_KEYS);

  if (!sort_keys) {
    if (indent) {
      ctx->indent_level++;
      if (append_indent_prefix(b, '{', ctx->indent_level) < 0) return -1;
    } else {
      if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
        if (json_buffer_reserve(b, 1) < 0) return -1;
      }
      b->data[b->len++] = '{';
    }

    PyObject *raw_key, *value;
    Py_ssize_t pos = 0;
    int first = 1;

    while (PyDict_Next(obj, &pos, &raw_key, &value)) {
      if (!first) {
        if (indent) {
          if (append_indent_sep(b, ctx->indent_level) < 0) return -1;
        } else {
          if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
            if (json_buffer_reserve(b, 1) < 0) return -1;
          }
          b->data[b->len++] = ',';
        }
      }
      first = 0;

      if (CJSON_LIKELY(PyUnicode_Check(raw_key))) {
        if (PyUnicode_IS_COMPACT_ASCII(raw_key)) {
          Py_ssize_t klen = PyUnicode_GET_LENGTH(raw_key);
          if (klen <= 64 && b->len + klen + 4 <= b->cap) {
            const char *kdata = (const char *)PyUnicode_1BYTE_DATA(raw_key);
            char *dst = b->data + b->len;
            if (CJSON_LIKELY(copy_if_clean(dst + 1, kdata, klen))) {
              dst[0] = '"';
              dst += 1 + klen;
              *dst++ = '"';
              *dst++ = ':';
              if (indent) *dst++ = ' ';
              b->len = (Py_ssize_t)(dst - b->data);
              if (json_encode_value_opts(ctx, value) < 0) return -1;
              continue;
            }
          }
        }
        if (json_buffer_append_string(b, raw_key) < 0) return -1;
      } else if (non_str_keys) {
        if (PyLong_CheckExact(raw_key)) {
          if (json_buffer_reserve(b, 24) < 0) return -1;
          char *dst = b->data + b->len;
          *dst++ = '"';
          char *end = write_py_long(dst, raw_key);
          if (end != NULL) {
            *end++ = '"';
            b->len = (Py_ssize_t)(end - b->data);
          } else {
            PyErr_Clear();
            PyObject *kstr = stringify_non_str_key(raw_key);
            if (kstr == NULL) return -1;
            int rc = json_buffer_append_string(b, kstr);
            Py_DECREF(kstr);
            if (rc < 0) return -1;
          }
        } else {
          PyObject *kstr = stringify_non_str_key(raw_key);
          if (kstr == NULL) return -1;
          int rc = json_buffer_append_string(b, kstr);
          Py_DECREF(kstr);
          if (rc < 0) return -1;
        }
      } else {
        PyErr_SetString(PyExc_TypeError, "Dict key must be str");
        return -1;
      }

      if (indent) {
        if (CJSON_UNLIKELY(b->len + 2 > b->cap)) {
          if (json_buffer_reserve(b, 2) < 0) return -1;
        }
        b->data[b->len++] = ':';
        b->data[b->len++] = ' ';
      } else {
        if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
          if (json_buffer_reserve(b, 1) < 0) return -1;
        }
        b->data[b->len++] = ':';
      }

      if (json_encode_value_opts(ctx, value) < 0) return -1;
    }

    if (indent) {
      ctx->indent_level--;
      return append_indent_close(b, '}', ctx->indent_level);
    }
    if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
      if (json_buffer_reserve(b, 1) < 0) return -1;
    }
    b->data[b->len++] = '}';
    return 0;
  }

  DictSortEntry stack_entries[64];
  DictSortEntry *entries = stack_entries;
  if (dict_size > 64) {
    entries = (DictSortEntry *)malloc((size_t)dict_size * sizeof(DictSortEntry));
    if (entries == NULL) {
      PyErr_NoMemory();
      return -1;
    }
  }

  PyObject *raw_key, *value;
  Py_ssize_t pos = 0, count = 0;

  while (PyDict_Next(obj, &pos, &raw_key, &value)) {
    PyObject *kstr;
    int is_temp = 0;
    const char *kutf8;
    Py_ssize_t klen;

    if (PyUnicode_Check(raw_key)) {
      kstr = raw_key;
      if (PyUnicode_IS_COMPACT_ASCII(raw_key)) {
        kutf8 = (const char *)PyUnicode_1BYTE_DATA(raw_key);
        klen = PyUnicode_GET_LENGTH(raw_key);
      } else {
        kutf8 = PyUnicode_AsUTF8AndSize(raw_key, &klen);
      }
      if (kutf8 == NULL) {
        if (entries != stack_entries) free(entries);
        return -1;
      }
    } else if (non_str_keys) {
      kstr = stringify_non_str_key(raw_key);
      if (kstr == NULL) {
        if (entries != stack_entries) free(entries);
        return -1;
      }
      is_temp = 1;
      kutf8 = PyUnicode_AsUTF8AndSize(kstr, &klen);
      if (kutf8 == NULL) {
        Py_DECREF(kstr);
        if (entries != stack_entries) free(entries);
        return -1;
      }
    } else {
      if (entries != stack_entries) free(entries);
      PyErr_SetString(PyExc_TypeError, "Dict key must be str");
      return -1;
    }

    entries[count++] = (DictSortEntry){dict_key_prefix(kutf8, klen), kutf8, (uint32_t)klen, is_temp, kstr, value};
  }

  sort_dict_entries(entries, count);

  if (indent) {
    ctx->indent_level++;
    if (append_indent_prefix(b, '{', ctx->indent_level) < 0) {
      for (Py_ssize_t i = 0; i < count; i++) {
        if (entries[i].is_temp_key) Py_DECREF(entries[i].key_str);
      }
      if (entries != stack_entries) free(entries);
      return -1;
    }
  } else {
    if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
      if (json_buffer_reserve(b, 1) < 0) {
        for (Py_ssize_t i = 0; i < count; i++) {
          if (entries[i].is_temp_key) Py_DECREF(entries[i].key_str);
        }
        if (entries != stack_entries) free(entries);
        return -1;
      }
    }
    b->data[b->len++] = '{';
  }

  int status = 0;
  for (Py_ssize_t i = 0; i < count; i++) {
    if (i > 0) {
      if (indent) {
        if (append_indent_sep(b, ctx->indent_level) < 0) { status = -1; break; }
      } else {
        if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
          if (json_buffer_reserve(b, 1) < 0) { status = -1; break; }
        }
        b->data[b->len++] = ',';
      }
    }

    uint32_t klen = entries[i].key_len;
    const char *ksrc = entries[i].key_utf8;
    if (klen == 1) {
      unsigned char c = (unsigned char)ksrc[0];
      if (CJSON_LIKELY(c >= 32 && c != '"' && c != '\\')) {
        if (CJSON_UNLIKELY(b->len + 3 > b->cap)) {
          if (json_buffer_reserve(b, 3) < 0) { status = -1; break; }
        }
        b->data[b->len++] = '"';
        b->data[b->len++] = (char)c;
        b->data[b->len++] = '"';
        goto key_written;
      }
    }
    if (json_buffer_append_str(b, ksrc, klen) < 0) { status = -1; break; }
key_written:

    if (indent) {
      if (CJSON_UNLIKELY(b->len + 2 > b->cap)) {
        if (json_buffer_reserve(b, 2) < 0) { status = -1; break; }
      }
      b->data[b->len++] = ':';
      b->data[b->len++] = ' ';
    } else {
      if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
        if (json_buffer_reserve(b, 1) < 0) { status = -1; break; }
      }
      b->data[b->len++] = ':';
    }

    if (json_encode_value_opts(ctx, entries[i].value) < 0) { status = -1; break; }
  }

  for (Py_ssize_t i = 0; i < count; i++) {
    if (entries[i].is_temp_key) Py_DECREF(entries[i].key_str);
  }
  if (entries != stack_entries) free(entries);
  if (status < 0) return -1;

  if (indent) {
    ctx->indent_level--;
    return append_indent_close(b, '}', ctx->indent_level);
  }
  if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
    if (json_buffer_reserve(b, 1) < 0) return -1;
  }
  b->data[b->len++] = '}';
  return 0;
}

static int json_buffer_append_dataclass_opts(SerializeContext *ctx, PyObject *obj) {
  PyObject *dict = PyObject_GenericGetDict(obj, NULL);
  if (dict != NULL) {
    int rc = json_buffer_append_object_opts(ctx, dict);
    Py_DECREF(dict);
    return rc;
  }
  PyErr_Clear();

  PyObject *fields = PyObject_GetAttr(obj, g_dataclass_fields_str);
  if (fields == NULL) return -1;

  PyObject *fallback_dict = PyDict_New();
  if (fallback_dict == NULL) {
    Py_DECREF(fields);
    return -1;
  }

  PyObject *key, *field_obj;
  Py_ssize_t pos = 0;
  while (PyDict_Next(fields, &pos, &key, &field_obj)) {
    if (PyUnicode_Check(key)) {
      Py_ssize_t klen;
      const char *ks = PyUnicode_AsUTF8AndSize(key, &klen);
      if (ks != NULL && klen > 0 && ks[0] == '_') continue;
    }
    PyObject *val = PyObject_GetAttr(obj, key);
    if (val != NULL) {
      PyDict_SetItem(fallback_dict, key, val);
      Py_DECREF(val);
    } else {
      PyErr_Clear();
    }
  }
  Py_DECREF(fields);

  int rc = json_buffer_append_object_opts(ctx, fallback_dict);
  Py_DECREF(fallback_dict);
  return rc;
}

typedef enum {
  NP_UNSUPPORTED = 0,
  NP_F64,
  NP_F32,
  NP_I64,
  NP_U64,
  NP_I32,
  NP_U32,
  NP_I16,
  NP_U16,
  NP_I8,
  NP_U8,
  NP_BOOL
} NumpyKind;

static inline NumpyKind numpy_parse_format(const char *fmt, Py_ssize_t itemsize) {
  if (fmt == NULL) return NP_UNSUPPORTED;
  if (fmt[0] == '@' || fmt[0] == '=') {
    fmt++;
  } else if (fmt[0] == '<') {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    fmt++;
#else
    return NP_UNSUPPORTED;
#endif
  } else if (fmt[0] == '>' || fmt[0] == '!') {
    return NP_UNSUPPORTED;
  }
  if (fmt[1] != '\0') return NP_UNSUPPORTED;
  switch (fmt[0]) {
    case 'd': return (itemsize == 8) ? NP_F64 : NP_UNSUPPORTED;
    case 'f': return (itemsize == 4) ? NP_F32 : NP_UNSUPPORTED;
    case 'q': return (itemsize == 8) ? NP_I64 : NP_UNSUPPORTED;
    case 'Q': return (itemsize == 8) ? NP_U64 : NP_UNSUPPORTED;
    case 'l': return (itemsize == 8) ? NP_I64 : ((itemsize == 4) ? NP_I32 : NP_UNSUPPORTED);
    case 'L': return (itemsize == 8) ? NP_U64 : ((itemsize == 4) ? NP_U32 : NP_UNSUPPORTED);
    case 'i': return (itemsize == 4) ? NP_I32 : NP_UNSUPPORTED;
    case 'I': return (itemsize == 4) ? NP_U32 : NP_UNSUPPORTED;
    case 'h': return (itemsize == 2) ? NP_I16 : NP_UNSUPPORTED;
    case 'H': return (itemsize == 2) ? NP_U16 : NP_UNSUPPORTED;
    case 'b': return (itemsize == 1) ? NP_I8 : NP_UNSUPPORTED;
    case 'B': return (itemsize == 1) ? NP_U8 : NP_UNSUPPORTED;
    case '?': return (itemsize == 1) ? NP_BOOL : NP_UNSUPPORTED;
    default:  return NP_UNSUPPORTED;
  }
}

static inline char *write_float_fast(float vf, char *dst) {
  float avf = fabsf(vf);
  if (CJSON_LIKELY(avf >= 1e-4f && avf < 1e16f)) {
    int64_t ival = (int64_t)vf;
    if (CJSON_LIKELY((float)ival != vf)) {
      return cjson_zmij_write_float(vf, dst);
    }
    if (CJSON_LIKELY(avf < 16777216.0f)) {
      char *end = jeaiii_to_text_i64(dst, ival);
      end[0] = '.';
      end[1] = '0';
      return end + 2;
    }
  }
  char *end = cjson_zmij_write_float(vf, dst);
  for (char *p = end - 1; p >= dst; p--) {
    char c = *p;
    if (c == '.') return end;
    if (c == 'e') {
      char *d = p + 2;
      if (end - d == 2 && d[0] == '0') {
        d[0] = d[1];
        end--;
        if (d[0] == '5' && p[1] == '-') {
          char digits[16];
          int nd = 0;
          char *m = dst;
          int neg = (*m == '-');
          if (neg) m++;
          for (; m < p; m++) {
            if (*m != '.') digits[nd++] = *m;
          }
          char *o = dst;
          if (neg) *o++ = '-';
          memcpy(o, "0.0000", 6);
          o += 6;
          memcpy(o, digits, (size_t)nd);
          return o + nd;
        }
      }
      return end;
    }
  }
  end[0] = '.';
  end[1] = '0';
  return end + 2;
}

static inline char *numpy_write_elem(char *dst, const char *p, NumpyKind kind) {
  switch (kind) {
    case NP_F64: {
      double v;
      memcpy(&v, p, sizeof(double));
      if (CJSON_UNLIKELY(!isfinite(v))) {
        memcpy(dst, "null", 4);
        return dst + 4;
      }
      return write_double(v, dst);
    }
    case NP_F32: {
      float vf;
      memcpy(&vf, p, sizeof(float));
      if (CJSON_UNLIKELY(!isfinite(vf))) {
        memcpy(dst, "null", 4);
        return dst + 4;
      }
      return write_float_fast(vf, dst);
    }
    case NP_I64: {
      int64_t v;
      memcpy(&v, p, sizeof(int64_t));
      if (v < 0) {
        *dst++ = '-';
        return jeaiii_to_text_u64(dst, (uint64_t)(-(v + 1)) + 1ULL);
      }
      return jeaiii_to_text_u64(dst, (uint64_t)v);
    }
    case NP_U64: {
      uint64_t v;
      memcpy(&v, p, sizeof(uint64_t));
      return jeaiii_to_text_u64(dst, v);
    }
    case NP_I32: {
      int32_t v;
      memcpy(&v, p, sizeof(int32_t));
      if (v < 0) {
        *dst++ = '-';
        return jeaiii_to_text_u32(dst, (uint32_t)(-(v + 1)) + 1u);
      }
      return jeaiii_to_text_u32(dst, (uint32_t)v);
    }
    case NP_U32: {
      uint32_t v;
      memcpy(&v, p, sizeof(uint32_t));
      return jeaiii_to_text_u32(dst, v);
    }
    case NP_I16: {
      int16_t v;
      memcpy(&v, p, sizeof(int16_t));
      if (v < 0) {
        *dst++ = '-';
        return jeaiii_to_text_u32(dst, (uint32_t)(-v));
      }
      return jeaiii_to_text_u32(dst, (uint32_t)v);
    }
    case NP_U16: {
      uint16_t v;
      memcpy(&v, p, sizeof(uint16_t));
      return jeaiii_to_text_u32(dst, (uint32_t)v);
    }
    case NP_I8: {
      int8_t v = *(const int8_t *)p;
      if (v < 0) {
        *dst++ = '-';
        return jeaiii_to_text_u32(dst, (uint32_t)(-(int)v));
      }
      return jeaiii_to_text_u32(dst, (uint32_t)v);
    }
    case NP_U8:
      return jeaiii_to_text_u32(dst, (uint32_t)(*(const uint8_t *)p));
    case NP_BOOL:
      if (*(const uint8_t *)p) {
        memcpy(dst, "true", 4);
        return dst + 4;
      }
      memcpy(dst, "false", 5);
      return dst + 5;
    default:
      return dst;
  }
}

static inline Py_ssize_t numpy_max_elem_bytes(NumpyKind kind) {
  switch (kind) {
    case NP_F64:
    case NP_F32: return 34;
    case NP_I64:
    case NP_U64: return 22;
    case NP_I32:
    case NP_U32: return 12;
    case NP_I16:
    case NP_U16: return 7;
    case NP_BOOL: return 6;
    default: return 5;
  }
}

static int numpy_encode_dim(JsonBuffer *b, const char *base, int ndim,
                            const Py_ssize_t *shape, const Py_ssize_t *strides,
                            NumpyKind kind) {
  Py_ssize_t n = shape[0];
  if (n == 0) {
    return json_buffer_append(b, "[]", 2);
  }
  Py_ssize_t stride = strides[0];
  if (ndim == 1) {
    Py_ssize_t max_per = numpy_max_elem_bytes(kind);
    if (json_buffer_reserve(b, n * max_per + 16) < 0) return -1;
    char *dst = b->data + b->len;
    *dst++ = '[';
    const char *p = base;
    if (kind == NP_F64 && stride == 8) {
      const double *dp = (const double *)base;
      for (Py_ssize_t i = 0; i < n; i++) {
        if (i > 0) *dst++ = ',';
        double v = dp[i];
        if (CJSON_UNLIKELY(!isfinite(v))) {
          memcpy(dst, "null", 4);
          dst += 4;
        } else {
          dst = write_double_fast(v, dst);
        }
      }
    } else if (kind == NP_I64 && stride == 8) {
      const int64_t *ip = (const int64_t *)base;
      for (Py_ssize_t i = 0; i < n; i++) {
        if (i > 0) *dst++ = ',';
        int64_t v = ip[i];
        if (v < 0) {
          *dst++ = '-';
          dst = jeaiii_to_text_u64(dst, (uint64_t)(-(v + 1)) + 1ULL);
        } else {
          dst = jeaiii_to_text_u64(dst, (uint64_t)v);
        }
      }
    } else if (kind == NP_I32 && stride == 4) {
      const int32_t *ip = (const int32_t *)base;
      for (Py_ssize_t i = 0; i < n; i++) {
        if (i > 0) *dst++ = ',';
        int32_t v = ip[i];
        if (v < 0) {
          *dst++ = '-';
          dst = jeaiii_to_text_u32(dst, (uint32_t)(-(v + 1)) + 1u);
        } else {
          dst = jeaiii_to_text_u32(dst, (uint32_t)v);
        }
      }
    } else if (kind == NP_F32 && stride == 4) {
      const float *fp = (const float *)base;
      for (Py_ssize_t i = 0; i < n; i++) {
        if (i > 0) *dst++ = ',';
        float vf = fp[i];
        if (CJSON_UNLIKELY(!isfinite(vf))) {
          memcpy(dst, "null", 4);
          dst += 4;
        } else {
          dst = write_float_fast(vf, dst);
        }
      }
    } else if (kind == NP_BOOL && stride == 1) {
      static const char BOOL_LUT[2][8] = {"false,", "true, "};
      static const uint8_t BOOL_LEN[2] = {6, 5};
      const uint8_t *bp = (const uint8_t *)base;
      for (Py_ssize_t i = 0; i < n; i++) {
        uint8_t bv = bp[i] != 0;
        memcpy(dst, BOOL_LUT[bv], 8);
        dst += BOOL_LEN[bv];
      }
      dst--; /* overwrite trailing ',' with ']' below */
    } else if (kind == NP_U8 && stride == 1) {
      static char U8_LUT[256][4];
      static uint8_t U8_LEN[256];
      static int u8_init = 0;
      if (CJSON_UNLIKELY(!u8_init)) {
        for (int k = 0; k < 256; k++) {
          char *e = jeaiii_to_text_u32(U8_LUT[k], (uint32_t)k);
          *e++ = ',';
          U8_LEN[k] = (uint8_t)(e - U8_LUT[k]);
        }
        u8_init = 1;
      }
      const uint8_t *up = (const uint8_t *)base;
      for (Py_ssize_t i = 0; i < n; i++) {
        uint8_t uv = up[i];
        memcpy(dst, U8_LUT[uv], 4);
        dst += U8_LEN[uv];
      }
      dst--; /* overwrite trailing ',' with ']' below */
    } else {
      for (Py_ssize_t i = 0; i < n; i++) {
        if (i > 0) *dst++ = ',';
        dst = numpy_write_elem(dst, p, kind);
        p += stride;
      }
    }
    *dst++ = ']';
    b->len = (Py_ssize_t)(dst - b->data);
    return 0;
  }

  if (json_buffer_append_char(b, '[') < 0) return -1;
  const char *p = base;
  for (Py_ssize_t i = 0; i < n; i++) {
    if (i > 0 && json_buffer_append_char(b, ',') < 0) return -1;
    if (numpy_encode_dim(b, p, ndim - 1, shape + 1, strides + 1, kind) < 0) return -1;
    p += stride;
  }
  return json_buffer_append_char(b, ']');
}

/* Direct PEP 3118 C-buffer NumPy serializer (zero NumPy build dependency).
 * Returns 1 if handled, 0 to fall back to .tolist(), -1 on error. */
static int json_buffer_append_numpy_buffer(JsonBuffer *b, PyObject *obj) {
  if (!PyObject_CheckBuffer(obj) || PyBytes_Check(obj) ||
      PyByteArray_Check(obj) || PyMemoryView_Check(obj)) {
    return 0;
  }
  Py_buffer view;
  if (PyObject_GetBuffer(obj, &view, PyBUF_STRIDES | PyBUF_FORMAT) < 0) {
    PyErr_Clear();
    return 0;
  }
  NumpyKind kind = numpy_parse_format(view.format, view.itemsize);
  if (kind == NP_UNSUPPORTED || view.ndim > 32) {
    PyBuffer_Release(&view);
    return 0;
  }
  int rc;
  if (view.ndim == 0) {
    if (json_buffer_reserve(b, numpy_max_elem_bytes(kind)) < 0) {
      PyBuffer_Release(&view);
      return -1;
    }
    char *dst = numpy_write_elem(b->data + b->len, (const char *)view.buf, kind);
    b->len = (Py_ssize_t)(dst - b->data);
    rc = 0;
  } else {
    rc = numpy_encode_dim(b, (const char *)view.buf, view.ndim, view.shape, view.strides, kind);
  }
  PyBuffer_Release(&view);
  return (rc < 0) ? -1 : 1;
}

static int json_encode_opts(SerializeContext *ctx, PyObject *obj) {
  JsonBuffer *b = ctx->b;
  if (++b->depth > CJSON_MAX_DEPTH) {
    b->depth--;
    PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
    return -1;
  }

  PyTypeObject *tp = Py_TYPE(obj);
  if (tp == &PyUnicode_Type) { b->depth--; return json_buffer_append_string(b, obj); }
  if (tp == &PyLong_Type) { b->depth--; return json_buffer_append_int_opts(ctx, obj); }
  if (tp == &PyBool_Type) { b->depth--; return (obj == Py_True) ? json_buffer_append_true(b) : json_buffer_append_false(b); }
  if (obj == Py_None) { b->depth--; return json_buffer_append_null(b); }
  if (tp == &PyFloat_Type) { b->depth--; return json_buffer_append_double(b, obj); }
  if (tp == &PyList_Type || tp == &PyTuple_Type) {
    int rc = json_buffer_append_array_opts(ctx, obj);
    b->depth--;
    return rc;
  }
  if (tp == &PyDict_Type) {
    int rc = json_buffer_append_object_opts(ctx, obj);
    b->depth--;
    return rc;
  }

  if (tp == &Fragment_Type) {
    PyObject *contents = ((FragmentObject *)obj)->contents;
    int rc = -1;
    if (contents != NULL && PyBytes_Check(contents)) {
      rc = json_buffer_append(b, PyBytes_AS_STRING(contents), PyBytes_GET_SIZE(contents));
    } else if (contents != NULL && PyUnicode_Check(contents)) {
      Py_ssize_t len;
      const char *utf8 = PyUnicode_AsUTF8AndSize(contents, &len);
      if (utf8 != NULL) rc = json_buffer_append(b, utf8, len);
    } else {
      PyErr_SetString(PyExc_TypeError, "orjson.Fragment's content is not of type bytes or str");
    }
    b->depth--;
    return rc;
  }

  if (PyDateTime_Check(obj) || PyDate_Check(obj) || PyTime_Check(obj)) {
    if (!(ctx->opts & OPT_PASSTHROUGH_DATETIME)) {
      int rc = json_buffer_append_datetime_opts(ctx, obj);
      b->depth--;
      return rc;
    }
  }

  if (ctx->opts & OPT_SERIALIZE_NUMPY) {
    if (!(ctx->opts & OPT_INDENT_2)) {
      int nrc = json_buffer_append_numpy_buffer(b, obj);
      if (nrc != 0) {
        b->depth--;
        return (nrc < 0) ? -1 : 0;
      }
    }
    if (PyObject_HasAttrString(obj, "tolist")) {
      PyObject *list_obj = PyObject_CallMethod(obj, "tolist", NULL);
      if (list_obj != NULL) {
        int rc = json_encode_opts(ctx, list_obj);
        Py_DECREF(list_obj);
        b->depth--;
        return rc;
      }
      PyErr_Clear();
    }
  }

  if (PyType_HasFeature(tp, Py_TPFLAGS_HEAPTYPE)) {
    if (g_uuid_type != NULL && PyObject_IsInstance(obj, g_uuid_type)) {
      int rc = json_buffer_append_uuid(b, obj);
      b->depth--;
      return rc;
    }
    if (!(ctx->opts & OPT_PASSTHROUGH_DATACLASS)) {
      if (_PyType_Lookup(tp, g_dataclass_fields_str) != NULL) {
        int rc = json_buffer_append_dataclass_opts(ctx, obj);
        b->depth--;
        return rc;
      }
    }
    if (g_enum_type != NULL && PyObject_IsInstance(obj, g_enum_type)) {
      PyObject *val = PyObject_GetAttrString(obj, "value");
      if (val != NULL) {
        int rc = json_encode_opts(ctx, val);
        Py_DECREF(val);
        b->depth--;
        return rc;
      }
      PyErr_Clear();
    }
    if (!(ctx->opts & OPT_PASSTHROUGH_SUBCLASS)) {
      if (PyUnicode_Check(obj)) { b->depth--; return json_buffer_append_string(b, obj); }
      if (PyLong_Check(obj)) { b->depth--; return json_buffer_append_int_opts(ctx, obj); }
      if (PyFloat_Check(obj)) { b->depth--; return json_buffer_append_double(b, obj); }
      if (PyList_Check(obj) || PyTuple_Check(obj)) {
        int rc = json_buffer_append_array_opts(ctx, obj);
        b->depth--;
        return rc;
      }
      if (PyDict_Check(obj)) {
        int rc = json_buffer_append_object_opts(ctx, obj);
        b->depth--;
        return rc;
      }
    }
  }

  if (ctx->default_fn != NULL && ctx->default_fn != Py_None) {
    PyObject *res = PyObject_Vectorcall(ctx->default_fn, &obj, 1, NULL);
    if (res != NULL) {
      int rc = json_encode_opts(ctx, res);
      Py_DECREF(res);
      b->depth--;
      return rc;
    }
    PyErr_Clear();
  }

  b->depth--;
  PyErr_Format(PyExc_TypeError, "Type is not JSON serializable: %.200s", tp->tp_name);
  return -1;
}

/* ========================================================================= */
/* Register-Pinned Fast Path for OPT_INDENT_2 and OPT_SORT_KEYS              */
/* ========================================================================= */

static const char CJSON_INDENT_SEP[66] =
    ",\n                                                                ";
static const char CJSON_INDENT_NL[65] =
    "\n                                                                ";

static inline char *emit_indent_open_fast(
    JsonBuffer *b, char *dst, char **safe_end_p, char open_ch, int level) {
  Py_ssize_t spaces = (Py_ssize_t)level * 2;
  if (CJSON_LIKELY(level <= 32)) {
    *dst++ = open_ch;
    memcpy(dst, CJSON_INDENT_NL, (size_t)(1 + spaces));
    return dst + 1 + spaces;
  }
  b->len = (Py_ssize_t)(dst - b->data);
  if (json_buffer_grow(b, b->len + 2 + spaces + 256) < 0) return NULL;
  dst = b->data + b->len;
  *dst++ = open_ch;
  *dst++ = '\n';
  memset(dst, ' ', (size_t)spaces);
  dst += spaces;
  *safe_end_p = b->data + b->cap - 128;
  return dst;
}

static inline char *emit_indent_sep_fast(
    JsonBuffer *b, char *dst, char **safe_end_p, int level) {
  Py_ssize_t spaces = (Py_ssize_t)level * 2;
  if (CJSON_LIKELY(level <= 32)) {
    memcpy(dst, CJSON_INDENT_SEP, (size_t)(2 + spaces));
    return dst + 2 + spaces;
  }
  b->len = (Py_ssize_t)(dst - b->data);
  if (json_buffer_grow(b, b->len + 2 + spaces + 256) < 0) return NULL;
  dst = b->data + b->len;
  *dst++ = ',';
  *dst++ = '\n';
  memset(dst, ' ', (size_t)spaces);
  dst += spaces;
  *safe_end_p = b->data + b->cap - 128;
  return dst;
}

static inline char *emit_indent_close_fast(
    JsonBuffer *b, char *dst, char **safe_end_p, char close_ch, int level) {
  Py_ssize_t spaces = (Py_ssize_t)level * 2;
  if (CJSON_LIKELY(level <= 32)) {
    memcpy(dst, CJSON_INDENT_NL, (size_t)(1 + spaces));
    dst += 1 + spaces;
    *dst++ = close_ch;
    return dst;
  }
  b->len = (Py_ssize_t)(dst - b->data);
  if (json_buffer_grow(b, b->len + 2 + spaces + 256) < 0) return NULL;
  dst = b->data + b->len;
  *dst++ = '\n';
  memset(dst, ' ', (size_t)spaces);
  dst += spaces;
  *dst++ = close_ch;
  *safe_end_p = b->data + b->cap - 128;
  return dst;
}

static char *encode_array_opts_fast(SerializeContext *ctx, char *dst, char **safe_end_p, PyObject *obj);
static char *encode_dict_opts_fast(SerializeContext *ctx, char *dst, char **safe_end_p, PyObject *obj);
static CJSON_NOINLINE char *encode_dict_sorted_fast(SerializeContext *ctx, char *dst, char **safe_end_p, PyObject *obj);

/* Stack-local zero-sort cache for arrays of dicts under OPT_SORT_KEYS.
 * Kept noinline so its stack buffers never bloat encode_array_opts_fast.
 * Uses 100% public CPython C-API (PyDict_Next + exact PyObject* key identity). */
static CJSON_NOINLINE char *encode_dict_array_sorted_cached(
    SerializeContext *ctx, char *dst, char **safe_end_p,
    PyObject **items, Py_ssize_t len, Py_ssize_t *i_io) {
  JsonBuffer *b = ctx->b;
  char *safe_end = *safe_end_p;
  int indent = (ctx->opts & OPT_INDENT_2);
  int elem_indent = indent ? (ctx->indent_level + 1) : 0;

  PyObject *cached_keys[32];
  uint8_t perm[32];
  uint16_t k_off[32];
  uint16_t k_len[32];
  char key_buf[2048];
  Py_ssize_t cached_n = 0;
  int misses = 0;

  Py_ssize_t i = *i_io;
  for (; i < len; i++) {
    PyObject *item = items[i];
    if (CJSON_UNLIKELY(Py_TYPE(item) != &PyDict_Type)) break;

    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      Py_ssize_t rem = len - i;
      Py_ssize_t add = (rem < 512) ? (rem * 32 + 256) : 8192;
      if (json_buffer_grow(b, b->len + add) < 0) return NULL;
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }

    if (i > 0) {
      if (indent) {
        dst = emit_indent_sep_fast(b, dst, &safe_end, ctx->indent_level);
        if (CJSON_UNLIKELY(dst == NULL)) return NULL;
        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + 256) < 0) return NULL;
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
      } else {
        *dst++ = ',';
      }
    }

    Py_ssize_t dsize = PyDict_GET_SIZE(item);
    if (CJSON_UNLIKELY(dsize == 0)) {
      dst[0] = '{';
      dst[1] = '}';
      dst += 2;
      continue;
    }

    if (CJSON_LIKELY(cached_n > 0)) {
      if (CJSON_LIKELY(dsize == cached_n && b->depth < CJSON_REC_SER_DEPTH)) {
        PyObject *vals[32];
        Py_ssize_t pos = 0, j = 0;
        PyObject *k, *v;
        int match = 1;
        while (PyDict_Next(item, &pos, &k, &v)) {
          if (CJSON_UNLIKELY(k != cached_keys[j])) {
            match = 0;
            break;
          }
          vals[j++] = v;
        }
        if (CJSON_LIKELY(match && j == cached_n)) {
          b->depth++;
          if (indent) ctx->indent_level++;
          for (Py_ssize_t s = 0; s < cached_n; s++) {
            uint16_t kl = k_len[s];
            if (CJSON_UNLIKELY(dst + kl + 64 > safe_end)) {
              b->len = (Py_ssize_t)(dst - b->data);
              if (json_buffer_grow(b, b->len + kl + 512) < 0) {
                if (indent) ctx->indent_level--;
                b->depth--;
                return NULL;
              }
              dst = b->data + b->len;
              safe_end = b->data + b->cap - 128;
            }
            memcpy(dst, key_buf + k_off[s], kl);
            dst += kl;

            PyObject *dval = vals[perm[s]];
            PyTypeObject *vtp = Py_TYPE(dval);
            if (vtp == &PyUnicode_Type) {
              dst = emit_str_inline(b, dst, &safe_end, dval);
              if (CJSON_UNLIKELY(dst == NULL)) {
                if (indent) ctx->indent_level--;
                b->depth--;
                return NULL;
              }
            } else if (vtp == &PyLong_Type) {
              char *next = write_py_long(dst, dval);
              if (CJSON_UNLIKELY(next == NULL)) {
                if (indent) ctx->indent_level--;
                b->depth--;
                return NULL;
              }
              dst = next;
            } else if (vtp == &PyFloat_Type) {
              double dv = PyFloat_AS_DOUBLE(dval);
              if (CJSON_LIKELY(!isnan(dv) && !isinf(dv))) {
                dst = write_double_fast(dv, dst);
              } else {
                memcpy(dst, "null", 4);
                dst += 4;
              }
            } else if (dval == Py_None) {
              memcpy(dst, "null", 4);
              dst += 4;
            } else if (vtp == &PyBool_Type) {
              if (dval == Py_True) {
                memcpy(dst, "true", 4);
                dst += 4;
              } else {
                memcpy(dst, "false", 5);
                dst += 5;
              }
            } else if (vtp == &PyList_Type || vtp == &PyTuple_Type) {
              if (PySequence_Fast_GET_SIZE(dval) == 0) {
                dst[0] = '[';
                dst[1] = ']';
                dst += 2;
              } else {
                dst = encode_array_opts_fast(ctx, dst, &safe_end, dval);
                if (CJSON_UNLIKELY(dst == NULL)) {
                  if (indent) ctx->indent_level--;
                  b->depth--;
                  return NULL;
                }
              }
            } else if (vtp == &PyDict_Type) {
              if (PyDict_GET_SIZE(dval) == 0) {
                dst[0] = '{';
                dst[1] = '}';
                dst += 2;
              } else {
                dst = encode_dict_sorted_fast(ctx, dst, &safe_end, dval);
                if (CJSON_UNLIKELY(dst == NULL)) {
                  if (indent) ctx->indent_level--;
                  b->depth--;
                  return NULL;
                }
              }
            } else {
              b->len = (Py_ssize_t)(dst - b->data);
              if (json_encode_opts(ctx, dval) < 0) {
                if (indent) ctx->indent_level--;
                b->depth--;
                return NULL;
              }
              if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
                if (json_buffer_grow(b, b->len + 256) < 0) {
                  if (indent) ctx->indent_level--;
                  b->depth--;
                  return NULL;
                }
              }
              dst = b->data + b->len;
              safe_end = b->data + b->cap - 128;
            }
          }
          if (CJSON_UNLIKELY(dst > safe_end)) {
            b->len = (Py_ssize_t)(dst - b->data);
            if (json_buffer_grow(b, b->len + 256) < 0) {
              if (indent) ctx->indent_level--;
              b->depth--;
              return NULL;
            }
            dst = b->data + b->len;
            safe_end = b->data + b->cap - 128;
          }
          if (indent) {
            ctx->indent_level--;
            dst = emit_indent_close_fast(b, dst, &safe_end, '}', ctx->indent_level);
            if (CJSON_UNLIKELY(dst == NULL)) {
              b->depth--;
              return NULL;
            }
          } else {
            *dst++ = '}';
          }
          b->depth--;
          misses = 0;
          continue;
        }
      }
      misses++;
      if (misses >= 3) {
        /* Polymorphic / varied-size / shuffled array: bail to normal loop */
        dst = encode_dict_sorted_fast(ctx, dst, &safe_end, item);
        if (CJSON_UNLIKELY(dst == NULL)) return NULL;
        i++;
        break;
      }
    }

    /* Learn schema from this dict (up to 32 clean compact-ASCII keys) */
    if (dsize >= 1 && dsize <= 32 && (!indent || elem_indent <= 32)) {
      DictSortEntry entries[32];
      PyObject *raw_keys[32];
      Py_ssize_t pos = 0, cnt = 0;
      PyObject *rk, *rv;
      int learn_ok = 1;
      while (PyDict_Next(item, &pos, &rk, &rv)) {
        if (CJSON_UNLIKELY(Py_TYPE(rk) != &PyUnicode_Type || !PyUnicode_IS_COMPACT_ASCII(rk))) {
          learn_ok = 0;
          break;
        }
        Py_ssize_t kl = PyUnicode_GET_LENGTH(rk);
        const char *ks = (const char *)PyUnicode_1BYTE_DATA(rk);
        if (kl > 64) {
          learn_ok = 0;
          break;
        }
        raw_keys[cnt] = rk;
        entries[cnt] = (DictSortEntry){dict_key_prefix(ks, kl), ks, (uint32_t)kl, (int)cnt, rk, rv};
        cnt++;
      }
      if (learn_ok && cnt == dsize) {
        sort_dict_entries(entries, cnt);
        uint16_t used = 0;
        int pack_ok = 1;
        Py_ssize_t spaces = (Py_ssize_t)elem_indent * 2;
        for (Py_ssize_t s = 0; s < cnt; s++) {
          uint32_t kl = entries[s].key_len;
          Py_ssize_t need = (indent ? (2 + spaces + 1) : 1) + (Py_ssize_t)kl + 3;
          if ((size_t)used + (size_t)need > sizeof(key_buf)) {
            pack_ok = 0;
            break;
          }
          k_off[s] = used;
          char *p = key_buf + used;
          if (s == 0) {
            *p++ = '{';
            if (indent) {
              memcpy(p, CJSON_INDENT_NL, (size_t)(1 + spaces));
              p += 1 + spaces;
            }
          } else {
            if (indent) {
              memcpy(p, CJSON_INDENT_SEP, (size_t)(2 + spaces));
              p += 2 + spaces;
            } else {
              *p++ = ',';
            }
          }
          *p++ = '"';
          if (kl > 0 && !copy_if_clean_short(p, entries[s].key_utf8, (Py_ssize_t)kl)) {
            pack_ok = 0;
            break;
          }
          p += kl;
          *p++ = '"';
          *p++ = ':';
          if (indent) *p++ = ' ';
          k_len[s] = (uint16_t)(p - (key_buf + used));
          used = (uint16_t)(p - key_buf);
          perm[s] = (uint8_t)entries[s].is_temp_key;
        }
        if (pack_ok) {
          memcpy(cached_keys, raw_keys, (size_t)cnt * sizeof(PyObject *));
          cached_n = cnt;
        }
      }
    }

    dst = encode_dict_sorted_fast(ctx, dst, &safe_end, item);
    if (CJSON_UNLIKELY(dst == NULL)) return NULL;
  }

  *i_io = i;
  *safe_end_p = safe_end;
  return dst;
}

static char *encode_array_opts_fast(SerializeContext *ctx, char *dst, char **safe_end_p, PyObject *obj) {
  JsonBuffer *b = ctx->b;
  Py_ssize_t len = PySequence_Fast_GET_SIZE(obj);
  char *safe_end = *safe_end_p;

  if (CJSON_UNLIKELY(len == 0)) {
    dst[0] = '[';
    dst[1] = ']';
    return dst + 2;
  }
  if (CJSON_UNLIKELY(b->depth + 1 > CJSON_MAX_DEPTH)) {
    PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
    return NULL;
  }

  b->depth++;
  int indent = (ctx->opts & OPT_INDENT_2);
  if (indent) {
    ctx->indent_level++;
    dst = emit_indent_open_fast(b, dst, &safe_end, '[', ctx->indent_level);
    if (CJSON_UNLIKELY(dst == NULL)) {
      ctx->indent_level--;
      b->depth--;
      return NULL;
    }
  } else {
    *dst++ = '[';
  }

  PyObject **items = PySequence_Fast_ITEMS(obj);
  Py_ssize_t i = 0;
  if ((ctx->opts & OPT_SORT_KEYS) && len >= 4 && Py_TYPE(items[0]) == &PyDict_Type &&
      ctx->default_fn == NULL && (!indent || ctx->indent_level < 31)) {
    dst = encode_dict_array_sorted_cached(ctx, dst, &safe_end, items, len, &i);
    if (CJSON_UNLIKELY(dst == NULL)) {
      if (indent) ctx->indent_level--;
      b->depth--;
      return NULL;
    }
  }
  for (; i < len; i++) {
    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      Py_ssize_t rem = len - i;
      Py_ssize_t add = (rem < 512) ? (rem * 12 + 256) : 4096;
      if (json_buffer_grow(b, b->len + add) < 0) {
        if (indent) ctx->indent_level--;
        b->depth--;
        return NULL;
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }
    if (i > 0) {
      if (indent) {
        dst = emit_indent_sep_fast(b, dst, &safe_end, ctx->indent_level);
        if (CJSON_UNLIKELY(dst == NULL)) {
          ctx->indent_level--;
          b->depth--;
          return NULL;
        }
        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + 256) < 0) {
            ctx->indent_level--;
            b->depth--;
            return NULL;
          }
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
      } else {
        *dst++ = ',';
      }
    }

    PyObject *item = items[i];
    PyTypeObject *tp = Py_TYPE(item);
    if (tp == &PyUnicode_Type) {
      dst = emit_str_inline(b, dst, &safe_end, item);
      if (CJSON_UNLIKELY(dst == NULL)) {
        if (indent) ctx->indent_level--;
        b->depth--;
        return NULL;
      }
    } else if (tp == &PyLong_Type) {
      char *next = write_py_long(dst, item);
      if (CJSON_UNLIKELY(next == NULL)) {
        if (indent) ctx->indent_level--;
        b->depth--;
        return NULL;
      }
      dst = next;
    } else if (tp == &PyFloat_Type) {
      double d = PyFloat_AS_DOUBLE(item);
      if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
        dst = write_double_fast(d, dst);
      } else {
        memcpy(dst, "null", 4);
        dst += 4;
      }
    } else if (item == Py_None) {
      memcpy(dst, "null", 4);
      dst += 4;
    } else if (tp == &PyBool_Type) {
      if (item == Py_True) {
        memcpy(dst, "true", 4);
        dst += 4;
      } else {
        memcpy(dst, "false", 5);
        dst += 5;
      }
    } else if (tp == &PyList_Type || tp == &PyTuple_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (json_encode_opts(ctx, item) < 0) {
          if (indent) ctx->indent_level--;
          b->depth--;
          return NULL;
        }
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) {
            if (indent) ctx->indent_level--;
            b->depth--;
            return NULL;
          }
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PySequence_Fast_GET_SIZE(item) == 0) {
        dst[0] = '[';
        dst[1] = ']';
        dst += 2;
      } else {
        dst = encode_array_opts_fast(ctx, dst, &safe_end, item);
        if (CJSON_UNLIKELY(dst == NULL)) {
          if (indent) ctx->indent_level--;
          b->depth--;
          return NULL;
        }
      }
    } else if (tp == &PyDict_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (json_encode_opts(ctx, item) < 0) {
          if (indent) ctx->indent_level--;
          b->depth--;
          return NULL;
        }
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) {
            if (indent) ctx->indent_level--;
            b->depth--;
            return NULL;
          }
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PyDict_GET_SIZE(item) == 0) {
        dst[0] = '{';
        dst[1] = '}';
        dst += 2;
      } else {
        dst = encode_dict_opts_fast(ctx, dst, &safe_end, item);
        if (CJSON_UNLIKELY(dst == NULL)) {
          if (indent) ctx->indent_level--;
          b->depth--;
          return NULL;
        }
      }
    } else {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_encode_opts(ctx, item) < 0) {
        if (indent) ctx->indent_level--;
        b->depth--;
        return NULL;
      }
      if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
        if (json_buffer_grow(b, b->len + 256) < 0) {
          if (indent) ctx->indent_level--;
          b->depth--;
          return NULL;
        }
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }
  }

  if (CJSON_UNLIKELY(dst > safe_end)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_grow(b, b->len + 256) < 0) {
      if (indent) ctx->indent_level--;
      b->depth--;
      return NULL;
    }
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }

  if (indent) {
    ctx->indent_level--;
    dst = emit_indent_close_fast(b, dst, &safe_end, ']', ctx->indent_level);
    if (CJSON_UNLIKELY(dst == NULL)) {
      b->depth--;
      return NULL;
    }
  } else {
    *dst++ = ']';
  }
  b->depth--;
  if (CJSON_UNLIKELY(dst > safe_end)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }
  *safe_end_p = safe_end;
  return dst;
}

/* 16-slot call-local sorted-key schema cache for nested dicts, dicts-of-dicts,
 * and union-typed arrays under OPT_SORT_KEYS. Fixed O(1) memory per call. */
typedef struct {
  PyObject *keys[24];
  uint8_t perm[24];
  uint16_t k_off[24];
  uint16_t k_len[24];
  uint8_t n;
  uint8_t hits;
} SortSchemaSlot;

struct SortSchemaCache {
  SortSchemaSlot slots[16];
  char key_buf[8192];
  uint16_t buf_used;
  uint8_t consec_misses;
  uint8_t disabled;
};

static inline void sort_schema_cache_init(SortSchemaCache *sc) {
  for (int i = 0; i < 16; i++) {
    sc->slots[i].n = 0;
    sc->slots[i].hits = 0;
  }
  sc->buf_used = 0;
  sc->consec_misses = 0;
  sc->disabled = 0;
}

static CJSON_NOINLINE char *encode_dict_sorted_fast(SerializeContext *ctx, char *dst, char **safe_end_p, PyObject *obj) {
  JsonBuffer *b = ctx->b;
  Py_ssize_t dict_size = PyDict_GET_SIZE(obj);
  char *safe_end = *safe_end_p;
  int indent = (ctx->opts & OPT_INDENT_2);

  /* Fast 16-slot schema cache lookup (handles nested dicts, dicts-of-dicts, union lists) */
  SortSchemaCache *sc = ctx->sort_cache;
  SortSchemaSlot *miss_slot = NULL;
  if (sc != NULL && !sc->disabled && dict_size >= 2 && dict_size <= 24) {
    Py_ssize_t cpos = 0;
    PyObject *k0 = NULL, *v0 = NULL;
    if (PyDict_Next(obj, &cpos, &k0, &v0)) {
      uint32_t h = (uint32_t)(((uintptr_t)k0 >> 4) ^ ((uintptr_t)k0 >> 10) ^ (uint32_t)dict_size) & 15u;
      SortSchemaSlot *sl = &sc->slots[h];
      if (sl->n == (uint8_t)dict_size && sl->keys[0] == k0) {
        PyObject *vals[24];
        vals[0] = v0;
        Py_ssize_t j = 1;
        PyObject *ck, *cv;
        int match = 1;
        while (PyDict_Next(obj, &cpos, &ck, &cv)) {
          if (CJSON_UNLIKELY(ck != sl->keys[j])) {
            match = 0;
            break;
          }
          vals[j++] = cv;
        }
        if (CJSON_LIKELY(match && j == dict_size)) {
          if (sl->hits < 255) sl->hits++;
          sc->consec_misses = 0;

          b->depth++;
          if (indent) {
            ctx->indent_level++;
            dst = emit_indent_open_fast(b, dst, &safe_end, '{', ctx->indent_level);
            if (CJSON_UNLIKELY(dst == NULL)) { ctx->indent_level--; b->depth--; return NULL; }
          } else {
            *dst++ = '{';
          }
          for (Py_ssize_t s = 0; s < dict_size; s++) {
            uint16_t kl = sl->k_len[s];
            if (CJSON_UNLIKELY(dst + kl + 64 > safe_end)) {
              b->len = (Py_ssize_t)(dst - b->data);
              if (json_buffer_grow(b, b->len + kl + 512) < 0) {
                if (indent) ctx->indent_level--;
                b->depth--;
                return NULL;
              }
              dst = b->data + b->len;
              safe_end = b->data + b->cap - 128;
            }
            if (s > 0) {
              if (indent) {
                dst = emit_indent_sep_fast(b, dst, &safe_end, ctx->indent_level);
                if (CJSON_UNLIKELY(dst == NULL)) { ctx->indent_level--; b->depth--; return NULL; }
                if (CJSON_UNLIKELY(dst + kl + 64 > safe_end)) {
                  b->len = (Py_ssize_t)(dst - b->data);
                  if (json_buffer_grow(b, b->len + kl + 256) < 0) {
                    ctx->indent_level--;
                    b->depth--;
                    return NULL;
                  }
                  dst = b->data + b->len;
                  safe_end = b->data + b->cap - 128;
                }
              } else {
                *dst++ = ',';
              }
            }
            memcpy(dst, sc->key_buf + sl->k_off[s], kl);
            dst += kl;
            if (indent) *dst++ = ' ';

            PyObject *dval = vals[sl->perm[s]];
            PyTypeObject *vtp = Py_TYPE(dval);
            if (vtp == &PyUnicode_Type) {
              dst = emit_str_inline(b, dst, &safe_end, dval);
              if (CJSON_UNLIKELY(dst == NULL)) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
            } else if (vtp == &PyLong_Type) {
              char *next = write_py_long(dst, dval);
              if (CJSON_UNLIKELY(next == NULL)) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
              dst = next;
            } else if (vtp == &PyFloat_Type) {
              double dv = PyFloat_AS_DOUBLE(dval);
              if (CJSON_LIKELY(!isnan(dv) && !isinf(dv))) {
                dst = write_double_fast(dv, dst);
              } else {
                memcpy(dst, "null", 4);
                dst += 4;
              }
            } else if (dval == Py_None) {
              memcpy(dst, "null", 4);
              dst += 4;
            } else if (vtp == &PyBool_Type) {
              if (dval == Py_True) {
                memcpy(dst, "true", 4);
                dst += 4;
              } else {
                memcpy(dst, "false", 5);
                dst += 5;
              }
            } else if (vtp == &PyList_Type || vtp == &PyTuple_Type) {
              if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
                b->len = (Py_ssize_t)(dst - b->data);
                if (json_encode_opts(ctx, dval) < 0) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
                if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
                  if (json_buffer_grow(b, b->len + 256) < 0) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
                }
                dst = b->data + b->len;
                safe_end = b->data + b->cap - 128;
              } else if (PySequence_Fast_GET_SIZE(dval) == 0) {
                dst[0] = '['; dst[1] = ']'; dst += 2;
              } else {
                dst = encode_array_opts_fast(ctx, dst, &safe_end, dval);
                if (CJSON_UNLIKELY(dst == NULL)) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
              }
            } else if (vtp == &PyDict_Type) {
              if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
                b->len = (Py_ssize_t)(dst - b->data);
                if (json_encode_opts(ctx, dval) < 0) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
                if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
                  if (json_buffer_grow(b, b->len + 256) < 0) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
                }
                dst = b->data + b->len;
                safe_end = b->data + b->cap - 128;
              } else if (PyDict_GET_SIZE(dval) == 0) {
                dst[0] = '{'; dst[1] = '}'; dst += 2;
              } else {
                dst = encode_dict_sorted_fast(ctx, dst, &safe_end, dval);
                if (CJSON_UNLIKELY(dst == NULL)) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
              }
            } else {
              b->len = (Py_ssize_t)(dst - b->data);
              if (json_encode_opts(ctx, dval) < 0) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
              if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
                if (json_buffer_grow(b, b->len + 256) < 0) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
              }
              dst = b->data + b->len;
              safe_end = b->data + b->cap - 128;
            }
          }
          if (CJSON_UNLIKELY(dst > safe_end)) {
            b->len = (Py_ssize_t)(dst - b->data);
            if (json_buffer_grow(b, b->len + 256) < 0) { if (indent) ctx->indent_level--; b->depth--; return NULL; }
            dst = b->data + b->len;
            safe_end = b->data + b->cap - 128;
          }
          if (indent) {
            ctx->indent_level--;
            dst = emit_indent_close_fast(b, dst, &safe_end, '}', ctx->indent_level);
            if (CJSON_UNLIKELY(dst == NULL)) { b->depth--; return NULL; }
          } else {
            *dst++ = '}';
          }
          b->depth--;
          if (CJSON_UNLIKELY(dst > safe_end)) {
            b->len = (Py_ssize_t)(dst - b->data);
            if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
            dst = b->data + b->len;
            safe_end = b->data + b->cap - 128;
          }
          *safe_end_p = safe_end;
          return dst;
        }
      }
      if (++sc->consec_misses >= 24) {
        sc->disabled = 1;
      } else if (sl->hits < 2) {
        /* Only overwrite cold slots; hot slots (hits >= 2) stay pinned */
        miss_slot = sl;
      }
    }
  }

  DictSortEntry stack_entries[64];
  DictSortEntry *entries = stack_entries;
  if (CJSON_UNLIKELY(dict_size > 64)) {
    entries = (DictSortEntry *)PyMem_Malloc((size_t)dict_size * sizeof(DictSortEntry));
    if (entries == NULL) {
      PyErr_NoMemory();
      return NULL;
    }
  }

  PyObject *raw_key, *value;
  Py_ssize_t pos = 0, count = 0;
  while (PyDict_Next(obj, &pos, &raw_key, &value)) {
    PyTypeObject *ktp = Py_TYPE(raw_key);
    if (CJSON_UNLIKELY(ktp != &PyUnicode_Type && !PyType_FastSubclass(ktp, Py_TPFLAGS_UNICODE_SUBCLASS))) {
      if (entries != stack_entries) PyMem_Free(entries);
      PyErr_SetString(PyExc_TypeError, "Dict key must be str");
      return NULL;
    }
    Py_ssize_t klen;
    const char *kutf8;
    if (CJSON_LIKELY(PyUnicode_IS_COMPACT_ASCII(raw_key))) {
      klen = PyUnicode_GET_LENGTH(raw_key);
      kutf8 = (const char *)PyUnicode_1BYTE_DATA(raw_key);
    } else {
      kutf8 = cjson_get_str_utf8(raw_key, &klen);
      if (CJSON_UNLIKELY(kutf8 == NULL)) {
        if (entries != stack_entries) PyMem_Free(entries);
        return NULL;
      }
    }
    entries[count] = (DictSortEntry){dict_key_prefix(kutf8, klen), kutf8, (uint32_t)klen, (int)count, raw_key, value};
    count++;
  }
  sort_dict_entries(entries, count);

  /* Populate cold cache slot on miss if all keys are clean compact ASCII */
  if (miss_slot != NULL && count >= 2 && count <= 24) {
    uint16_t start_used = sc->buf_used;
    uint16_t cur_used = start_used;
    int pack_ok = 1;
    PyObject *orig_keys[24];
    for (Py_ssize_t s = 0; s < count; s++) {
      uint32_t kl = entries[s].key_len;
      if (kl > 64 || (size_t)cur_used + (size_t)kl + 3u > sizeof(sc->key_buf) ||
          !PyUnicode_IS_COMPACT_ASCII(entries[s].key_str)) {
        pack_ok = 0;
        break;
      }
      miss_slot->k_off[s] = cur_used;
      char *kp = sc->key_buf + cur_used;
      kp[0] = '"';
      if (kl > 0 && !copy_if_clean_short(kp + 1, entries[s].key_utf8, (Py_ssize_t)kl)) {
        pack_ok = 0;
        break;
      }
      kp[kl + 1] = '"';
      kp[kl + 2] = ':';
      miss_slot->k_len[s] = (uint16_t)(kl + 3);
      cur_used = (uint16_t)(cur_used + kl + 3);
      int orig_i = entries[s].is_temp_key;
      miss_slot->perm[s] = (uint8_t)orig_i;
      orig_keys[orig_i] = entries[s].key_str;
    }
    if (pack_ok) {
      memcpy(miss_slot->keys, orig_keys, (size_t)count * sizeof(PyObject *));
      miss_slot->n = (uint8_t)count;
      miss_slot->hits = 0;
      sc->buf_used = cur_used;
    }
  }

  b->depth++;
  if (indent) {
    ctx->indent_level++;
    dst = emit_indent_open_fast(b, dst, &safe_end, '{', ctx->indent_level);
    if (CJSON_UNLIKELY(dst == NULL)) goto fail;
  } else {
    *dst++ = '{';
  }

  for (Py_ssize_t idx = 0; idx < count; idx++) {
    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + 512) < 0) goto fail;
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }

    if (idx > 0) {
      if (indent) {
        dst = emit_indent_sep_fast(b, dst, &safe_end, ctx->indent_level);
        if (CJSON_UNLIKELY(dst == NULL)) goto fail;
        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
      } else {
        *dst++ = ',';
      }
    }

    const char *ksrc = entries[idx].key_utf8;
    Py_ssize_t klen = (Py_ssize_t)entries[idx].key_len;
    if (CJSON_LIKELY(klen > 0 && klen <= 64 && copy_if_clean_short(dst + 1, ksrc, klen))) {
      dst[0] = '"';
      dst[klen + 1] = '"';
      dst += klen + 2;
    } else if (klen == 0) {
      dst[0] = '"';
      dst[1] = '"';
      dst += 2;
    } else {
      dst = emit_str_slow(b, dst, &safe_end, entries[idx].key_str, ksrc, klen);
      if (CJSON_UNLIKELY(dst == NULL)) goto fail;
    }
    *dst++ = ':';
    if (indent) *dst++ = ' ';

    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }

    PyObject *dval = entries[idx].value;
    PyTypeObject *vtp = Py_TYPE(dval);
    if (vtp == &PyUnicode_Type) {
      dst = emit_str_inline(b, dst, &safe_end, dval);
      if (CJSON_UNLIKELY(dst == NULL)) goto fail;
    } else if (vtp == &PyLong_Type) {
      char *next = write_py_long(dst, dval);
      if (CJSON_UNLIKELY(next == NULL)) goto fail;
      dst = next;
    } else if (vtp == &PyFloat_Type) {
      double d = PyFloat_AS_DOUBLE(dval);
      if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
        dst = write_double_fast(d, dst);
      } else {
        memcpy(dst, "null", 4);
        dst += 4;
      }
    } else if (dval == Py_None) {
      memcpy(dst, "null", 4);
      dst += 4;
    } else if (vtp == &PyBool_Type) {
      if (dval == Py_True) {
        memcpy(dst, "true", 4);
        dst += 4;
      } else {
        memcpy(dst, "false", 5);
        dst += 5;
      }
    } else if (vtp == &PyList_Type || vtp == &PyTuple_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (json_encode_opts(ctx, dval) < 0) goto fail;
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PySequence_Fast_GET_SIZE(dval) == 0) {
        dst[0] = '[';
        dst[1] = ']';
        dst += 2;
      } else {
        dst = encode_array_opts_fast(ctx, dst, &safe_end, dval);
        if (CJSON_UNLIKELY(dst == NULL)) goto fail;
      }
    } else if (vtp == &PyDict_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (json_encode_opts(ctx, dval) < 0) goto fail;
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PyDict_GET_SIZE(dval) == 0) {
        dst[0] = '{';
        dst[1] = '}';
        dst += 2;
      } else {
        dst = encode_dict_sorted_fast(ctx, dst, &safe_end, dval);
        if (CJSON_UNLIKELY(dst == NULL)) goto fail;
      }
    } else {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_encode_opts(ctx, dval) < 0) goto fail;
      if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
        if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }
  }

  if (entries != stack_entries) PyMem_Free(entries);

  if (CJSON_UNLIKELY(dst > safe_end)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_grow(b, b->len + 256) < 0) {
      if (indent) ctx->indent_level--;
      b->depth--;
      return NULL;
    }
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }

  if (indent) {
    ctx->indent_level--;
    dst = emit_indent_close_fast(b, dst, &safe_end, '}', ctx->indent_level);
    if (CJSON_UNLIKELY(dst == NULL)) {
      b->depth--;
      return NULL;
    }
  } else {
    *dst++ = '}';
  }
  b->depth--;
  if (CJSON_UNLIKELY(dst > safe_end)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }
  *safe_end_p = safe_end;
  return dst;

fail:
  if (entries != stack_entries) PyMem_Free(entries);
  if (indent) ctx->indent_level--;
  b->depth--;
  return NULL;
}

static char *encode_dict_opts_fast(SerializeContext *ctx, char *dst, char **safe_end_p, PyObject *obj) {
  JsonBuffer *b = ctx->b;
  Py_ssize_t dict_size = PyDict_GET_SIZE(obj);
  char *safe_end = *safe_end_p;

  if (CJSON_UNLIKELY(dict_size == 0)) {
    dst[0] = '{';
    dst[1] = '}';
    return dst + 2;
  }
  if (CJSON_UNLIKELY(b->depth + 1 > CJSON_MAX_DEPTH)) {
    PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
    return NULL;
  }

  if (ctx->opts & OPT_SORT_KEYS) {
    return encode_dict_sorted_fast(ctx, dst, safe_end_p, obj);
  }

  int indent = (ctx->opts & OPT_INDENT_2);

  b->depth++;
  if (indent) {
    ctx->indent_level++;
    dst = emit_indent_open_fast(b, dst, &safe_end, '{', ctx->indent_level);
    if (CJSON_UNLIKELY(dst == NULL)) goto fail;
  } else {
    *dst++ = '{';
  }

  Py_ssize_t pos = 0;
  Py_ssize_t idx = 0;
  PyObject *key, *dval;
  while (PyDict_Next(obj, &pos, &key, &dval)) {
    PyTypeObject *ktp = Py_TYPE(key);
    if (CJSON_UNLIKELY(ktp != &PyUnicode_Type && !PyType_FastSubclass(ktp, Py_TPFLAGS_UNICODE_SUBCLASS))) {
      PyErr_SetString(PyExc_TypeError, "Dict key must be str");
      goto fail;
    }

    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + 512) < 0) goto fail;
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }

    if (idx > 0) {
      if (indent) {
        dst = emit_indent_sep_fast(b, dst, &safe_end, ctx->indent_level);
        if (CJSON_UNLIKELY(dst == NULL)) goto fail;
        if (CJSON_UNLIKELY(dst > safe_end)) {
          b->len = (Py_ssize_t)(dst - b->data);
          if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
          dst = b->data + b->len;
          safe_end = b->data + b->cap - 128;
        }
      } else {
        *dst++ = ',';
      }
    }
    idx++;

    dst = emit_str_inline(b, dst, &safe_end, key);
    if (CJSON_UNLIKELY(dst == NULL)) goto fail;
    *dst++ = ':';
    if (indent) *dst++ = ' ';

    if (CJSON_UNLIKELY(dst > safe_end)) {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }

    PyTypeObject *vtp = Py_TYPE(dval);
    if (vtp == &PyUnicode_Type) {
      dst = emit_str_inline(b, dst, &safe_end, dval);
      if (CJSON_UNLIKELY(dst == NULL)) goto fail;
    } else if (vtp == &PyLong_Type) {
      char *next = write_py_long(dst, dval);
      if (CJSON_UNLIKELY(next == NULL)) goto fail;
      dst = next;
    } else if (vtp == &PyFloat_Type) {
      double d = PyFloat_AS_DOUBLE(dval);
      if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
        dst = write_double_fast(d, dst);
      } else {
        memcpy(dst, "null", 4);
        dst += 4;
      }
    } else if (dval == Py_None) {
      memcpy(dst, "null", 4);
      dst += 4;
    } else if (vtp == &PyBool_Type) {
      if (dval == Py_True) {
        memcpy(dst, "true", 4);
        dst += 4;
      } else {
        memcpy(dst, "false", 5);
        dst += 5;
      }
    } else if (vtp == &PyList_Type || vtp == &PyTuple_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (json_encode_opts(ctx, dval) < 0) goto fail;
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PySequence_Fast_GET_SIZE(dval) == 0) {
        dst[0] = '[';
        dst[1] = ']';
        dst += 2;
      } else {
        dst = encode_array_opts_fast(ctx, dst, &safe_end, dval);
        if (CJSON_UNLIKELY(dst == NULL)) goto fail;
      }
    } else if (vtp == &PyDict_Type) {
      if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
        b->len = (Py_ssize_t)(dst - b->data);
        if (json_encode_opts(ctx, dval) < 0) goto fail;
        if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
          if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
        }
        dst = b->data + b->len;
        safe_end = b->data + b->cap - 128;
      } else if (PyDict_GET_SIZE(dval) == 0) {
        dst[0] = '{';
        dst[1] = '}';
        dst += 2;
      } else {
        dst = encode_dict_opts_fast(ctx, dst, &safe_end, dval);
        if (CJSON_UNLIKELY(dst == NULL)) goto fail;
      }
    } else {
      b->len = (Py_ssize_t)(dst - b->data);
      if (json_encode_opts(ctx, dval) < 0) goto fail;
      if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
        if (json_buffer_grow(b, b->len + 256) < 0) goto fail;
      }
      dst = b->data + b->len;
      safe_end = b->data + b->cap - 128;
    }
  }

  if (CJSON_UNLIKELY(dst > safe_end)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_grow(b, b->len + 256) < 0) {
      if (indent) ctx->indent_level--;
      b->depth--;
      return NULL;
    }
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }

  if (indent) {
    ctx->indent_level--;
    dst = emit_indent_close_fast(b, dst, &safe_end, '}', ctx->indent_level);
    if (CJSON_UNLIKELY(dst == NULL)) {
      b->depth--;
      return NULL;
    }
  } else {
    *dst++ = '}';
  }
  b->depth--;
  if (CJSON_UNLIKELY(dst > safe_end)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (json_buffer_grow(b, b->len + 128) < 0) return NULL;
    dst = b->data + b->len;
    safe_end = b->data + b->cap - 128;
  }
  *safe_end_p = safe_end;
  return dst;

fail:
  if (indent) ctx->indent_level--;
  b->depth--;
  return NULL;
}

static int encode_root_opts_fast(SerializeContext *ctx, PyObject *obj) {
  JsonBuffer *b = ctx->b;
  presize_root_buffer(b, obj, (ctx->opts & OPT_INDENT_2) != 0);
  if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
    if (json_buffer_grow(b, b->len + 256) < 0) return -1;
  }
  PyTypeObject *tp = Py_TYPE(obj);
  if (tp == &PyDict_Type) {
    char *safe_end = b->data + b->cap - 128;
    char *dst = encode_dict_opts_fast(ctx, b->data + b->len, &safe_end, obj);
    if (CJSON_UNLIKELY(dst == NULL)) return -1;
    b->len = (Py_ssize_t)(dst - b->data);
    return 0;
  }
  if (tp == &PyList_Type || tp == &PyTuple_Type) {
    char *safe_end = b->data + b->cap - 128;
    char *dst = encode_array_opts_fast(ctx, b->data + b->len, &safe_end, obj);
    if (CJSON_UNLIKELY(dst == NULL)) return -1;
    b->len = (Py_ssize_t)(dst - b->data);
    return 0;
  }
  return json_encode_opts(ctx, obj);
}

/* ========================================================================= */
/* Public Module Entry Point                                                 */
/* ========================================================================= */

PyObject *c_json_dumps(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
  (void)self;

  if (CJSON_LIKELY(nargs == 1 && kwnames == NULL)) {
    PyObject *obj = args[0];
    PyTypeObject *tp = Py_TYPE(obj);

    if (obj == Py_None) {
      return PyBytes_FromStringAndSize("null", 4);
    }
    if (tp == &PyBool_Type) {
      return (obj == Py_True) ? PyBytes_FromStringAndSize("true", 4)
                              : PyBytes_FromStringAndSize("false", 5);
    }
    if (tp == &PyLong_Type) {
      char buf[32];
      char *end = write_py_long(buf, obj);
      if (CJSON_UNLIKELY(end == NULL)) return NULL;
      return PyBytes_FromStringAndSize(buf, (Py_ssize_t)(end - buf));
    }
    if (tp == &PyFloat_Type) {
      double val = PyFloat_AS_DOUBLE(obj);
      if (CJSON_UNLIKELY(isnan(val) || isinf(val))) {
        return PyBytes_FromStringAndSize("null", 4);
      }
      char buf[40];
      char *end = write_double_fast(val, buf);
      return PyBytes_FromStringAndSize(buf, (Py_ssize_t)(end - buf));
    }
    if (tp == &PyUnicode_Type) {
      Py_ssize_t len;
      const char *src = cjson_get_str_utf8(obj, &len);
      if (CJSON_LIKELY(src != NULL)) {
        PyObject *res = PyBytes_FromStringAndSize(NULL, len + 2);
        if (CJSON_LIKELY(res != NULL)) {
          char *dst = PyBytes_AS_STRING(res);
          dst[0] = '"';
          dst[len + 1] = '"';
          if (CJSON_LIKELY(copy_if_clean(dst + 1, src, len))) {
            return res;
          }
          Py_DECREF(res);
        }
      }
    }

    JsonBuffer b;
    json_buffer_init(&b);
    if (encode_root_fast(&b, obj) < 0) {
      json_buffer_free(&b);
      return NULL;
    }
    return json_buffer_finish(&b);
  }

  if (nargs == 0 && (kwnames == NULL || PyTuple_GET_SIZE(kwnames) == 0)) {
    PyErr_SetString(PyExc_TypeError, "dumps() missing 1 required positional argument: 'obj'");
    return NULL;
  }

  PyObject *obj = NULL;
  PyObject *default_fn = NULL;
  PyObject *option_obj = NULL;
  int opts = 0;

  Py_ssize_t nkwargs = (kwnames != NULL) ? PyTuple_GET_SIZE(kwnames) : 0;
  Py_ssize_t npos = nargs;

  if (npos > 3) {
    PyErr_SetString(PyExc_TypeError, "dumps() takes at most 3 positional arguments");
    return NULL;
  }

  if (npos >= 1) obj = args[0];
  if (npos >= 2) default_fn = args[1];
  if (npos >= 3) option_obj = args[2];

  for (Py_ssize_t i = 0; i < nkwargs; i++) {
    PyObject *kw = PyTuple_GET_ITEM(kwnames, i);
    PyObject *val = args[npos + i];

    if (PyUnicode_CompareWithASCIIString(kw, "default") == 0) {
      if (default_fn != NULL) {
        PyErr_SetString(PyExc_TypeError, "dumps() got multiple values for argument 'default'");
        return NULL;
      }
      default_fn = val;
    } else if (PyUnicode_CompareWithASCIIString(kw, "option") == 0) {
      if (option_obj != NULL) {
        PyErr_SetString(PyExc_TypeError, "dumps() got multiple values for argument 'option'");
        return NULL;
      }
      option_obj = val;
    } else {
      PyErr_SetString(PyExc_TypeError, "dumps() got an unexpected keyword argument");
      return NULL;
    }
  }

  if (obj == NULL) {
    PyErr_SetString(PyExc_TypeError, "dumps() missing 1 required positional argument: 'obj'");
    return NULL;
  }

  if (option_obj != NULL && option_obj != Py_None) {
    if (!PyLong_Check(option_obj)) {
      PyErr_SetString(PyExc_TypeError, "Invalid opts");
      return NULL;
    }
    long v = PyLong_AsLong(option_obj);
    if (v < 0 || (v & ~OPT_ALL_MASK) != 0) {
      PyErr_SetString(PyExc_TypeError, "Invalid opts");
      return NULL;
    }
    opts = (int)v;
  }

  if (default_fn == Py_None) {
    default_fn = NULL;
  }

  if ((opts & ~(OPT_INDENT_2 | OPT_SORT_KEYS | OPT_APPEND_NEWLINE)) == 0) {
    JsonBuffer b;
    json_buffer_init(&b);
    b.default_fn = default_fn;
    if ((opts & ~OPT_APPEND_NEWLINE) == 0) {
      if (encode_root_fast(&b, obj) < 0) {
        json_buffer_free(&b);
        return NULL;
      }
    } else {
      SortSchemaCache sort_cache;
      SerializeContext ctx = {&b, default_fn, opts, 0, NULL};
      if ((opts & OPT_SORT_KEYS) && default_fn == NULL) {
        PyTypeObject *rtp = Py_TYPE(obj);
        if ((rtp == &PyList_Type && PyList_GET_SIZE(obj) >= 4) ||
            (rtp == &PyTuple_Type && PyTuple_GET_SIZE(obj) >= 4) ||
            (rtp == &PyDict_Type && PyDict_GET_SIZE(obj) >= 16)) {
          sort_schema_cache_init(&sort_cache);
          ctx.sort_cache = &sort_cache;
        }
      }
      if (encode_root_opts_fast(&ctx, obj) < 0) {
        json_buffer_free(&b);
        return NULL;
      }
    }
    if (opts & OPT_APPEND_NEWLINE) {
      if (json_buffer_append_char(&b, '\n') < 0) {
        json_buffer_free(&b);
        return NULL;
      }
    }
    return json_buffer_finish(&b);
  }

  JsonBuffer b;
  json_buffer_init(&b);

  SerializeContext ctx;
  ctx.b = &b;
  ctx.default_fn = default_fn;
  ctx.opts = opts;
  ctx.indent_level = 0;
  ctx.sort_cache = NULL;

  if (json_encode_opts(&ctx, obj) < 0) {
    json_buffer_free(&b);
    return NULL;
  }

  if (opts & OPT_APPEND_NEWLINE) {
    if (json_buffer_append_char(&b, '\n') < 0) {
      json_buffer_free(&b);
      return NULL;
    }
  }

  return json_buffer_finish(&b);
}
