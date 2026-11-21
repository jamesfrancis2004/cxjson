#ifndef CJSON_PARALLEL_SER_H
#define CJSON_PARALLEL_SER_H

#if !defined(Py_GIL_DISABLED)
#define CJSON_PARALLEL_SER 1
#include <stdlib.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
typedef CRITICAL_SECTION cjson_mutex_t;
typedef CONDITION_VARIABLE cjson_cond_t;
#define cjson_mutex_init(m)    InitializeCriticalSection(m)
#define cjson_mutex_lock(m)    EnterCriticalSection(m)
#define cjson_mutex_unlock(m)  LeaveCriticalSection(m)
#define cjson_cond_init(c)     InitializeConditionVariable(c)
#define cjson_cond_wait(c, m)  SleepConditionVariableCS((c), (m), INFINITE)
#define cjson_cond_signal(c)   WakeConditionVariable(c)
#define cjson_cond_broadcast(c) WakeAllConditionVariable(c)
#else
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif
typedef pthread_mutex_t cjson_mutex_t;
typedef pthread_cond_t cjson_cond_t;
#define cjson_mutex_init(m)    pthread_mutex_init((m), NULL)
#define cjson_mutex_lock(m)    pthread_mutex_lock(m)
#define cjson_mutex_unlock(m)  pthread_mutex_unlock(m)
#define cjson_cond_init(c)     pthread_cond_init((c), NULL)
#define cjson_cond_wait(c, m)  pthread_cond_wait((c), (m))
#define cjson_cond_signal(c)   pthread_cond_signal(c)
#define cjson_cond_broadcast(c) pthread_cond_broadcast(c)
#endif

#define CJSON_MAX_WORKERS 8

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} RawSerBuf;

static inline int raw_buf_grow(RawSerBuf *rb, size_t needed) {
  size_t new_cap = rb->cap ? (rb->cap * 2) : 65536;
  while (new_cap < needed) new_cap *= 2;
  char *nd = (char *)realloc(rb->data, new_cap);
  if (CJSON_UNLIKELY(nd == NULL)) return -1;
  rb->data = nd;
  rb->cap = new_cap;
  return 0;
}

/* Pure read-only write_py_long (no PyErr calls, returns NULL on >64-bit) */
static inline char *raw_write_py_long(char *dst, PyObject *obj) {
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
    if ((tag & _PyLong_SIGN_MASK) == 2) *dst++ = '-';
    return jeaiii_to_text_u32(dst, (uint32_t)lo->long_value.ob_digit[0]);
  }
  if (CJSON_LIKELY(ndigits == 2)) {
    if ((tag & _PyLong_SIGN_MASK) == 2) *dst++ = '-';
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
      if (sign == 0) return jeaiii_to_text_u64(dst, mag);
      if (sign == 2 && mag <= (1ULL << 63)) {
        *dst++ = '-';
        return jeaiii_to_text_u64(dst, mag);
      }
    }
  }
  return NULL;
#else
  (void)dst;
  (void)obj;
  return NULL;
#endif
}

static char *raw_emit_str_escaped(RawSerBuf *rb, char *dst, char **safe_end_p,
                                  const char *src, Py_ssize_t slen, Py_ssize_t clean) {
  dst[0] = '"';
  dst += 1 + clean;
  size_t cur = (size_t)(dst - rb->data);
  Py_ssize_t rem = slen - clean;
  if (CJSON_UNLIKELY(cur + (size_t)rem + 256 > rb->cap)) {
    if (raw_buf_grow(rb, cur + (size_t)rem + 256) < 0) return NULL;
    dst = rb->data + cur;
  }
  const unsigned char *p = (const unsigned char *)src + clean;
  const unsigned char *end = (const unsigned char *)src + slen;
  while (p + 16 <= end) {
    char *cap_limit = rb->data + rb->cap - 400;
    if (CJSON_UNLIKELY(dst > cap_limit)) {
      cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + (size_t)(end - p) + 512) < 0) return NULL;
      dst = rb->data + cur;
      cap_limit = rb->data + rb->cap - 400;
    }
    const unsigned char *next_p = cjson_escape_chunk_fn(p, end, &dst, cap_limit, ESCAPE_TABLE);
    if (next_p == p) break;
    p = next_p;
  }
  while (p < end) {
    if (CJSON_UNLIKELY(dst + 16 > rb->data + rb->cap)) {
      cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + (size_t)(end - p) * 6 + 64) < 0) return NULL;
      dst = rb->data + cur;
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
  if (CJSON_UNLIKELY(dst + 130 > rb->data + rb->cap)) {
    cur = (size_t)(dst - rb->data);
    if (raw_buf_grow(rb, cur + 256) < 0) return NULL;
    dst = rb->data + cur;
  }
  *dst++ = '"';
  *safe_end_p = rb->data + rb->cap - 128;
  return dst;
}

__attribute__((always_inline))
static inline char *raw_emit_str(RawSerBuf *rb, char *dst, char **safe_end_p, PyObject *val) {
  const char *src;
  Py_ssize_t slen;
  if (CJSON_LIKELY(PyUnicode_IS_COMPACT_ASCII(val))) {
    slen = PyUnicode_GET_LENGTH(val);
    src = (const char *)PyUnicode_1BYTE_DATA(val);
#ifndef Py_LIMITED_API
  } else if (PyUnicode_IS_COMPACT(val) && ((PyCompactUnicodeObject *)val)->utf8 != NULL) {
    PyCompactUnicodeObject *cu = (PyCompactUnicodeObject *)val;
    slen = cu->utf8_length;
    src = cu->utf8;
#endif
  } else {
    return NULL; /* uncached non-ASCII -> bail to single-threaded path */
  }
  if (CJSON_UNLIKELY(slen == 0)) {
    dst[0] = '"';
    dst[1] = '"';
    return dst + 2;
  }
  if (CJSON_LIKELY(slen <= 64)) {
    if (CJSON_LIKELY(copy_if_clean_short(dst + 1, src, slen))) {
      dst[0] = '"';
      dst[slen + 1] = '"';
      return dst + slen + 2;
    }
    return raw_emit_str_escaped(rb, dst, safe_end_p, src, slen, 0);
  }
  size_t cur = (size_t)(dst - rb->data);
  if (CJSON_UNLIKELY(cur + (size_t)slen + 130 > rb->cap)) {
    if (raw_buf_grow(rb, cur + (size_t)slen + 256) < 0) return NULL;
    dst = rb->data + cur;
    *safe_end_p = rb->data + rb->cap - 128;
  }
  Py_ssize_t clean = copy_clean_prefix(dst + 1, src, slen);
  if (CJSON_LIKELY(clean == slen)) {
    dst[0] = '"';
    dst[slen + 1] = '"';
    return dst + slen + 2;
  }
  return raw_emit_str_escaped(rb, dst, safe_end_p, src, slen, clean);
}

typedef struct {
  PyObject *key_ptr;
  uint16_t offset;
  uint16_t enc_len;
} RawKeySlot;

typedef struct {
  RawKeySlot slots[64];
  uint32_t used;
  char buf[2048];
} RawKeyCache;

static char *raw_encode_val(RawSerBuf *rb, char *dst, char **safe_end_p, PyObject *obj, int depth, RawKeyCache *kc);

static char *raw_encode_array(RawSerBuf *rb, char *dst, char **safe_end_p, PyObject *obj, int depth, RawKeyCache *kc) {
  if (CJSON_UNLIKELY(depth > 64)) return NULL;
  Py_ssize_t len = PySequence_Fast_GET_SIZE(obj);
  if (len == 0) {
    dst[0] = '[';
    dst[1] = ']';
    return dst + 2;
  }
  char *safe_end = *safe_end_p;
  *dst++ = '[';
  PyObject **items = PySequence_Fast_ITEMS(obj);
  PyTypeObject *tp0 = Py_TYPE(items[0]);
  Py_ssize_t i = 0;

  if (tp0 == &PyFloat_Type) {
    for (; i < len; i++) {
      PyObject *item = items[i];
      if (CJSON_UNLIKELY(Py_TYPE(item) != &PyFloat_Type)) goto gen;
      if (CJSON_UNLIKELY(dst > safe_end)) {
        size_t cur = (size_t)(dst - rb->data);
        if (raw_buf_grow(rb, cur + 4096) < 0) return NULL;
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      double d = PyFloat_AS_DOUBLE(item);
      if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
        dst = write_double_fast(d, dst);
      } else {
        memcpy(dst, "null", 4);
        dst += 4;
      }
      *dst++ = ',';
    }
    dst[-1] = ']';
    *safe_end_p = safe_end;
    return dst;
  }
  if (tp0 == &PyLong_Type) {
    for (; i < len; i++) {
      PyObject *item = items[i];
      if (CJSON_UNLIKELY(Py_TYPE(item) != &PyLong_Type)) goto gen;
      if (CJSON_UNLIKELY(dst > safe_end)) {
        size_t cur = (size_t)(dst - rb->data);
        if (raw_buf_grow(rb, cur + 4096) < 0) return NULL;
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      dst = raw_write_py_long(dst, item);
      if (CJSON_UNLIKELY(dst == NULL)) return NULL;
      *dst++ = ',';
    }
    dst[-1] = ']';
    *safe_end_p = safe_end;
    return dst;
  }

gen:;
  for (; i < len; i++) {
    if (CJSON_UNLIKELY(dst > safe_end)) {
      size_t cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + 4096) < 0) return NULL;
      dst = rb->data + cur;
      safe_end = rb->data + rb->cap - 128;
    }
    dst = raw_encode_val(rb, dst, &safe_end, items[i], depth + 1, kc);
    if (CJSON_UNLIKELY(dst == NULL)) return NULL;
    *dst++ = ',';
  }
  dst[-1] = ']';
  *safe_end_p = safe_end;
  return dst;
}

static char *raw_encode_dict(RawSerBuf *rb, char *dst, char **safe_end_p, PyObject *obj, int depth, RawKeyCache *kc) {
  if (CJSON_UNLIKELY(depth > 64)) return NULL;
  if (PyDict_GET_SIZE(obj) == 0) {
    dst[0] = '{';
    dst[1] = '}';
    return dst + 2;
  }
  char *safe_end = *safe_end_p;
  *dst++ = '{';
  PyObject *key, *dval;
  Py_ssize_t pos = 0;
  while (PyDict_Next(obj, &pos, &key, &dval)) {
    if (CJSON_UNLIKELY(dst > safe_end)) {
      size_t cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + 1024) < 0) return NULL;
      dst = rb->data + cur;
      safe_end = rb->data + rb->cap - 128;
    }
    uint32_t slot = (uint32_t)(((uintptr_t)key >> 4) ^ ((uintptr_t)key >> 10)) & 63u;
    RawKeySlot *s = &kc->slots[slot];
    if (CJSON_LIKELY(s->key_ptr == key)) {
      uint32_t elen = s->enc_len;
      if (CJSON_LIKELY(elen <= 16)) {
        copy_short_ascii(dst, kc->buf + s->offset, elen);
      } else {
        memcpy(dst, kc->buf + s->offset, elen);
      }
      dst += elen;
    } else {
      if (CJSON_UNLIKELY(Py_TYPE(key) != &PyUnicode_Type)) return NULL;
      size_t kstart_off = (size_t)(dst - rb->data);
      dst = raw_emit_str(rb, dst, &safe_end, key);
      if (CJSON_UNLIKELY(dst == NULL)) return NULL;
      *dst++ = ':';
      size_t elen = (size_t)(dst - rb->data) - kstart_off;
      if (elen <= 64 && kc->used + (uint32_t)elen <= sizeof(kc->buf)) {
        memcpy(kc->buf + kc->used, rb->data + kstart_off, elen);
        s->key_ptr = key;
        s->offset = (uint16_t)kc->used;
        s->enc_len = (uint16_t)elen;
        kc->used += (uint32_t)elen;
      }
    }
    if (CJSON_UNLIKELY(dst > safe_end)) {
      size_t cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + 1024) < 0) return NULL;
      dst = rb->data + cur;
      safe_end = rb->data + rb->cap - 128;
    }
    dst = raw_encode_val(rb, dst, &safe_end, dval, depth + 1, kc);
    if (CJSON_UNLIKELY(dst == NULL)) return NULL;
    *dst++ = ',';
  }
  dst[-1] = '}';
  *safe_end_p = safe_end;
  return dst;
}

static char *raw_encode_val(RawSerBuf *rb, char *dst, char **safe_end_p, PyObject *obj, int depth, RawKeyCache *kc) {
  PyTypeObject *tp = Py_TYPE(obj);
  if (tp == &PyUnicode_Type) return raw_emit_str(rb, dst, safe_end_p, obj);
  if (tp == &PyLong_Type) return raw_write_py_long(dst, obj);
  if (tp == &PyFloat_Type) {
    double d = PyFloat_AS_DOUBLE(obj);
    if (CJSON_LIKELY(!isnan(d) && !isinf(d))) return write_double_fast(d, dst);
    memcpy(dst, "null", 4);
    return dst + 4;
  }
  if (obj == Py_None) {
    memcpy(dst, "null", 4);
    return dst + 4;
  }
  if (tp == &PyBool_Type) {
    if (obj == Py_True) {
      memcpy(dst, "true", 4);
      return dst + 4;
    }
    memcpy(dst, "false", 5);
    return dst + 5;
  }
  if (tp == &PyDict_Type) return raw_encode_dict(rb, dst, safe_end_p, obj, depth, kc);
  if (tp == &PyList_Type || tp == &PyTuple_Type) return raw_encode_array(rb, dst, safe_end_p, obj, depth, kc);
  if ((PyObject *)tp == g_uuid_type) return write_uuid_exact_fast(dst, obj);
  if (PyDateTime_CheckExact(obj) || PyDate_CheckExact(obj)) return write_datetime_exact_fast(dst, obj);
  return NULL;
}

/* ========================================================================= */
/* Parallel OPT_SORT_KEYS / OPT_INDENT_2 Read-Only Encoder                   */
/* ========================================================================= */

static const char RAW_INDENT_SEP[66] =
    ",\n                                                                ";
static const char RAW_INDENT_NL[65] =
    "\n                                                                ";

static inline char *raw_emit_indent_sep(char *dst, size_t sep_len) {
  if (CJSON_LIKELY(sep_len <= 16)) {
    memcpy(dst, RAW_INDENT_SEP, 16);
    return dst + sep_len;
  }
  if (CJSON_LIKELY(sep_len <= 32)) {
    memcpy(dst, RAW_INDENT_SEP, 32);
    return dst + sep_len;
  }
  memcpy(dst, RAW_INDENT_SEP, sep_len);
  return dst + sep_len;
}

static inline char *raw_emit_indent_nl(char *dst, size_t nl_len) {
  if (CJSON_LIKELY(nl_len <= 16)) {
    memcpy(dst, RAW_INDENT_NL, 16);
    return dst + nl_len;
  }
  if (CJSON_LIKELY(nl_len <= 32)) {
    memcpy(dst, RAW_INDENT_NL, 32);
    return dst + nl_len;
  }
  memcpy(dst, RAW_INDENT_NL, nl_len);
  return dst + nl_len;
}

typedef struct {
  uint64_t prefix;
  const char *utf8;
  Py_ssize_t len;
  PyObject *val;
  uint8_t orig_idx;
} RawSortEntry;

typedef struct {
  PyObject *keys[24];
  uint8_t perm[24];
  uint16_t k_off[24];
  uint16_t k_len[24];
  uint8_t n;
  uint8_t hits;
} RawSortSchemaSlot;

typedef struct {
  RawSortSchemaSlot slots[16];
  char key_buf[4096];
  uint16_t buf_used;
  uint8_t consec_misses;
  uint8_t disabled;
} RawSortSchemaCache;

static inline uint64_t raw_key_prefix(const char *s, Py_ssize_t len) {
  uint64_t p = 0;
  if (CJSON_LIKELY(len >= 8)) {
    memcpy(&p, s, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return p;
#elif defined(_MSC_VER)
    return _byteswap_uint64(p);
#else
    return __builtin_bswap64(p);
#endif
  }
  for (Py_ssize_t i = 0; i < len; i++) {
    p |= ((uint64_t)(unsigned char)s[i]) << (56 - (int)i * 8);
  }
  return p;
}

static inline int raw_entry_lt(const RawSortEntry *a, const RawSortEntry *b) {
  if (a->prefix != b->prefix) return a->prefix < b->prefix;
  Py_ssize_t min_l = a->len < b->len ? a->len : b->len;
  if (min_l > 8) {
    int c = memcmp(a->utf8 + 8, b->utf8 + 8, (size_t)(min_l - 8));
    if (c != 0) return c < 0;
  }
  return a->len < b->len;
}

static inline void raw_sort_entries_insertion(RawSortEntry *a, Py_ssize_t n) {
  for (Py_ssize_t i = 1; i < n; i++) {
    RawSortEntry key = a[i];
    Py_ssize_t j = i - 1;
    while (j >= 0 && raw_entry_lt(&key, &a[j])) {
      a[j + 1] = a[j];
      j--;
    }
    a[j + 1] = key;
  }
}

static void raw_sort_entries_qsort(RawSortEntry *a, Py_ssize_t n) {
  while (n > 24) {
    Py_ssize_t mid = n >> 1;
    if (raw_entry_lt(&a[mid], &a[0])) { RawSortEntry t = a[0]; a[0] = a[mid]; a[mid] = t; }
    if (raw_entry_lt(&a[n - 1], &a[0])) { RawSortEntry t = a[0]; a[0] = a[n - 1]; a[n - 1] = t; }
    if (raw_entry_lt(&a[n - 1], &a[mid])) { RawSortEntry t = a[mid]; a[mid] = a[n - 1]; a[n - 1] = t; }
    RawSortEntry pivot = a[mid];
    Py_ssize_t i = 0, j = n - 1;
    for (;;) {
      while (raw_entry_lt(&a[i], &pivot)) i++;
      while (raw_entry_lt(&pivot, &a[j])) j--;
      if (i >= j) break;
      RawSortEntry t = a[i]; a[i] = a[j]; a[j] = t;
      i++;
      j--;
    }
    Py_ssize_t left_n = j + 1;
    Py_ssize_t right_n = n - left_n;
    if (left_n < right_n) {
      raw_sort_entries_qsort(a, left_n);
      a += left_n;
      n = right_n;
    } else {
      raw_sort_entries_qsort(a + left_n, right_n);
      n = left_n;
    }
  }
  raw_sort_entries_insertion(a, n);
}

static char *raw_encode_val_opts(RawSerBuf *rb, char *dst, char **safe_end_p,
                                 PyObject *obj, int depth, int opts, int indent_level,
                                 RawKeyCache *kc, RawSortSchemaCache *sc);

static char *raw_encode_array_opts(RawSerBuf *rb, char *dst, char **safe_end_p,
                                   PyObject *obj, int depth, int opts, int indent_level,
                                   RawKeyCache *kc, RawSortSchemaCache *sc) {
  if (CJSON_UNLIKELY(depth > 64)) return NULL;
  Py_ssize_t len = PySequence_Fast_GET_SIZE(obj);
  if (len == 0) {
    dst[0] = '[';
    dst[1] = ']';
    return dst + 2;
  }
  int indent = (opts & OPT_INDENT_2);
  if (CJSON_UNLIKELY(indent && indent_level >= 30)) return NULL;

  char *safe_end = *safe_end_p;
  int child_indent = indent ? (indent_level + 1) : 0;
  size_t sep_len = (size_t)(2 + child_indent * 2);

  if (indent) {
    if (CJSON_UNLIKELY(dst > safe_end)) {
      size_t cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + 4096) < 0) return NULL;
      dst = rb->data + cur;
      safe_end = rb->data + rb->cap - 128;
    }
    *dst++ = '[';
    dst = raw_emit_indent_nl(dst, sep_len - 1);
  } else {
    *dst++ = '[';
  }

  PyObject **items = PySequence_Fast_ITEMS(obj);
  PyTypeObject *tp0 = Py_TYPE(items[0]);
  Py_ssize_t i = 0;

  if (tp0 == &PyFloat_Type) {
    for (; i < len; i++) {
      PyObject *item = items[i];
      if (CJSON_UNLIKELY(Py_TYPE(item) != &PyFloat_Type)) goto gen;
      if (CJSON_UNLIKELY(dst > safe_end)) {
        size_t cur = (size_t)(dst - rb->data);
        if (raw_buf_grow(rb, cur + 4096) < 0) return NULL;
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      if (i > 0) {
        if (indent) {
          dst = raw_emit_indent_sep(dst, sep_len);
        } else {
          *dst++ = ',';
        }
      }
      double d = PyFloat_AS_DOUBLE(item);
      if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
        dst = write_double_fast(d, dst);
      } else {
        memcpy(dst, "null", 4);
        dst += 4;
      }
    }
    goto close_arr;
  }
  if (tp0 == &PyLong_Type) {
    for (; i < len; i++) {
      PyObject *item = items[i];
      if (CJSON_UNLIKELY(Py_TYPE(item) != &PyLong_Type)) goto gen;
      if (CJSON_UNLIKELY(dst > safe_end)) {
        size_t cur = (size_t)(dst - rb->data);
        if (raw_buf_grow(rb, cur + 4096) < 0) return NULL;
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      if (i > 0) {
        if (indent) {
          dst = raw_emit_indent_sep(dst, sep_len);
        } else {
          *dst++ = ',';
        }
      }
      dst = raw_write_py_long(dst, item);
      if (CJSON_UNLIKELY(dst == NULL)) return NULL;
    }
    goto close_arr;
  }

gen:;
  for (; i < len; i++) {
    if (CJSON_UNLIKELY(dst > safe_end)) {
      size_t cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + 4096) < 0) return NULL;
      dst = rb->data + cur;
      safe_end = rb->data + rb->cap - 128;
    }
    if (i > 0) {
      if (indent) {
        dst = raw_emit_indent_sep(dst, sep_len);
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 4096) < 0) return NULL;
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
      } else {
        *dst++ = ',';
      }
    }
    dst = raw_encode_val_opts(rb, dst, &safe_end, items[i], depth + 1, opts, child_indent, kc, sc);
    if (CJSON_UNLIKELY(dst == NULL)) return NULL;
  }

close_arr:;
  if (CJSON_UNLIKELY(dst > safe_end)) {
    size_t cur = (size_t)(dst - rb->data);
    if (raw_buf_grow(rb, cur + 256) < 0) return NULL;
    dst = rb->data + cur;
    safe_end = rb->data + rb->cap - 128;
  }
  if (indent) {
    dst = raw_emit_indent_nl(dst, (size_t)(1 + indent_level * 2));
  }
  *dst++ = ']';
  *safe_end_p = safe_end;
  return dst;
}

static char *raw_encode_dict_opts(RawSerBuf *rb, char *dst, char **safe_end_p,
                                  PyObject *obj, int depth, int opts, int indent_level,
                                  RawKeyCache *kc, RawSortSchemaCache *sc) {
  if (CJSON_UNLIKELY(depth > 64)) return NULL;
  Py_ssize_t dict_size = PyDict_GET_SIZE(obj);
  if (dict_size == 0) {
    dst[0] = '{';
    dst[1] = '}';
    return dst + 2;
  }
  int indent = (opts & OPT_INDENT_2);
  if (CJSON_UNLIKELY(indent && indent_level >= 30)) return NULL;

  char *safe_end = *safe_end_p;
  int child_indent = indent ? (indent_level + 1) : 0;
  size_t sep_len = (size_t)(2 + child_indent * 2);

  if (!(opts & OPT_SORT_KEYS)) {
    /* OPT_INDENT_2 only (no sorting): reuse RawKeyCache storing `"key": ` */
    if (CJSON_UNLIKELY(dst > safe_end)) {
      size_t cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + 1024) < 0) return NULL;
      dst = rb->data + cur;
      safe_end = rb->data + rb->cap - 128;
    }
    *dst++ = '{';
    dst = raw_emit_indent_nl(dst, sep_len - 1);

    PyObject *key, *dval;
    Py_ssize_t pos = 0;
    Py_ssize_t idx = 0;
    while (PyDict_Next(obj, &pos, &key, &dval)) {
      if (CJSON_UNLIKELY(dst > safe_end)) {
        size_t cur = (size_t)(dst - rb->data);
        if (raw_buf_grow(rb, cur + 1024) < 0) return NULL;
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      if (idx++ > 0) {
        dst = raw_emit_indent_sep(dst, sep_len);
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 1024) < 0) return NULL;
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
      }
      uint32_t slot = (uint32_t)(((uintptr_t)key >> 4) ^ ((uintptr_t)key >> 10)) & 63u;
      RawKeySlot *s = &kc->slots[slot];
      if (CJSON_LIKELY(s->key_ptr == key)) {
        uint32_t elen = s->enc_len;
        if (CJSON_LIKELY(elen <= 16)) {
          copy_short_ascii(dst, kc->buf + s->offset, elen);
        } else {
          memcpy(dst, kc->buf + s->offset, elen);
        }
        dst += elen;
      } else {
        if (CJSON_UNLIKELY(Py_TYPE(key) != &PyUnicode_Type)) return NULL;
        size_t kstart_off = (size_t)(dst - rb->data);
        dst = raw_emit_str(rb, dst, &safe_end, key);
        if (CJSON_UNLIKELY(dst == NULL)) return NULL;
        *dst++ = ':';
        *dst++ = ' ';
        size_t elen = (size_t)(dst - rb->data) - kstart_off;
        if (elen <= 64 && kc->used + (uint32_t)elen <= sizeof(kc->buf)) {
          memcpy(kc->buf + kc->used, rb->data + kstart_off, elen);
          s->key_ptr = key;
          s->offset = (uint16_t)kc->used;
          s->enc_len = (uint16_t)elen;
          kc->used += (uint32_t)elen;
        }
      }
      if (CJSON_UNLIKELY(dst > safe_end)) {
        size_t cur = (size_t)(dst - rb->data);
        if (raw_buf_grow(rb, cur + 1024) < 0) return NULL;
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      dst = raw_encode_val_opts(rb, dst, &safe_end, dval, depth + 1, opts, child_indent, kc, sc);
      if (CJSON_UNLIKELY(dst == NULL)) return NULL;
    }
    if (CJSON_UNLIKELY(dst > safe_end)) {
      size_t cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + 256) < 0) return NULL;
      dst = rb->data + cur;
      safe_end = rb->data + rb->cap - 128;
    }
    dst = raw_emit_indent_nl(dst, (size_t)(1 + indent_level * 2));
    *dst++ = '}';
    *safe_end_p = safe_end;
    return dst;
  }

  /* OPT_SORT_KEYS (with or without OPT_INDENT_2) */
  RawSortSchemaSlot *miss_slot = NULL;
  if (sc != NULL && !sc->disabled && dict_size >= 2 && dict_size <= 24) {
    Py_ssize_t cpos = 0;
    PyObject *k0 = NULL, *v0 = NULL;
    if (PyDict_Next(obj, &cpos, &k0, &v0)) {
      uint32_t h = (uint32_t)(((uintptr_t)k0 >> 4) ^ ((uintptr_t)k0 >> 10) ^ (uint32_t)dict_size) & 15u;
      RawSortSchemaSlot *sl = &sc->slots[h];
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
          if (CJSON_UNLIKELY(dst > safe_end)) {
            size_t cur = (size_t)(dst - rb->data);
            if (raw_buf_grow(rb, cur + 1024) < 0) return NULL;
            dst = rb->data + cur;
            safe_end = rb->data + rb->cap - 128;
          }
          *dst++ = '{';
          if (indent) {
            memcpy(dst, RAW_INDENT_NL, sep_len - 1);
            dst += sep_len - 1;
          }
          for (Py_ssize_t s = 0; s < dict_size; s++) {
            uint16_t kl = sl->k_len[s];
            if (CJSON_UNLIKELY(dst + kl + sep_len > safe_end)) {
              size_t cur = (size_t)(dst - rb->data);
              if (raw_buf_grow(rb, cur + kl + 1024) < 0) return NULL;
              dst = rb->data + cur;
              safe_end = rb->data + rb->cap - 128;
            }
            if (s > 0) {
              if (indent) {
                memcpy(dst, RAW_INDENT_SEP, sep_len);
                dst += sep_len;
              } else {
                *dst++ = ',';
              }
            }
            memcpy(dst, sc->key_buf + sl->k_off[s], kl);
            dst += kl;
            dst = raw_encode_val_opts(rb, dst, &safe_end, vals[sl->perm[s]], depth + 1, opts, child_indent, kc, sc);
            if (CJSON_UNLIKELY(dst == NULL)) return NULL;
          }
          if (CJSON_UNLIKELY(dst > safe_end)) {
            size_t cur = (size_t)(dst - rb->data);
            if (raw_buf_grow(rb, cur + 256) < 0) return NULL;
            dst = rb->data + cur;
            safe_end = rb->data + rb->cap - 128;
          }
          if (indent) {
            size_t close_len = (size_t)(1 + indent_level * 2);
            memcpy(dst, RAW_INDENT_NL, close_len);
            dst += close_len;
          }
          *dst++ = '}';
          *safe_end_p = safe_end;
          return dst;
        }
      }
      if (sl->n == 0 || sl->hits < 2) {
        miss_slot = sl;
      }
      if (++sc->consec_misses >= 24) {
        sc->disabled = 1;
      }
    }
  }

  RawSortEntry stack_entries[64];
  RawSortEntry *entries = stack_entries;
  if (CJSON_UNLIKELY(dict_size > 64)) {
    entries = (RawSortEntry *)malloc((size_t)dict_size * sizeof(RawSortEntry));
    if (CJSON_UNLIKELY(entries == NULL)) return NULL;
  }

  PyObject *raw_keys[24];
  PyObject *key, *dval;
  Py_ssize_t pos = 0, cnt = 0;
  int sorted = 1;
  while (PyDict_Next(obj, &pos, &key, &dval)) {
    if (CJSON_UNLIKELY(Py_TYPE(key) != &PyUnicode_Type)) {
      if (entries != stack_entries) free(entries);
      return NULL;
    }
    const char *utf8;
    Py_ssize_t klen;
    if (CJSON_LIKELY(PyUnicode_IS_COMPACT_ASCII(key))) {
      klen = PyUnicode_GET_LENGTH(key);
      utf8 = (const char *)PyUnicode_1BYTE_DATA(key);
#ifndef Py_LIMITED_API
    } else if (PyUnicode_IS_COMPACT(key) && ((PyCompactUnicodeObject *)key)->utf8 != NULL) {
      PyCompactUnicodeObject *cu = (PyCompactUnicodeObject *)key;
      klen = cu->utf8_length;
      utf8 = cu->utf8;
#endif
    } else {
      if (entries != stack_entries) free(entries);
      return NULL;
    }
    if (miss_slot != NULL && cnt < 24) raw_keys[cnt] = key;
    entries[cnt].prefix = raw_key_prefix(utf8, klen);
    entries[cnt].utf8 = utf8;
    entries[cnt].len = klen;
    entries[cnt].val = dval;
    entries[cnt].orig_idx = (uint8_t)cnt;
    if (cnt > 0 && sorted && raw_entry_lt(&entries[cnt], &entries[cnt - 1])) {
      sorted = 0;
    }
    cnt++;
  }

  if (!sorted && cnt > 1) {
    if (cnt <= 24) {
      raw_sort_entries_insertion(entries, cnt);
    } else {
      raw_sort_entries_qsort(entries, cnt);
    }
  }

  /* Populate schema cache slot on miss if keys are clean and fit in key_buf */
  if (miss_slot != NULL && cnt >= 2 && cnt <= 24) {
    uint16_t start_used = sc->buf_used;
    uint16_t used = start_used;
    uint16_t tmp_off[24];
    uint16_t tmp_len[24];
    int pack_ok = 1;
    for (Py_ssize_t s = 0; s < cnt; s++) {
      Py_ssize_t kl = entries[s].len;
      uint16_t need = (uint16_t)(kl + 3 + (indent ? 1 : 0));
      if (kl > 60 || (uint32_t)used + need > sizeof(sc->key_buf)) {
        pack_ok = 0;
        break;
      }
      char *p = sc->key_buf + used;
      tmp_off[s] = used;
      *p++ = '"';
      if (kl > 0 && !copy_if_clean_short(p, entries[s].utf8, kl)) {
        pack_ok = 0;
        break;
      }
      p += kl;
      *p++ = '"';
      *p++ = ':';
      if (indent) *p++ = ' ';
      tmp_len[s] = need;
      used += need;
    }
    if (pack_ok) {
      memcpy(miss_slot->keys, raw_keys, (size_t)cnt * sizeof(PyObject *));
      for (Py_ssize_t s = 0; s < cnt; s++) {
        miss_slot->perm[s] = entries[s].orig_idx;
        miss_slot->k_off[s] = tmp_off[s];
        miss_slot->k_len[s] = tmp_len[s];
      }
      miss_slot->n = (uint8_t)cnt;
      miss_slot->hits = 1;
      sc->buf_used = used;
    }
  }

  if (CJSON_UNLIKELY(dst > safe_end)) {
    size_t cur = (size_t)(dst - rb->data);
    if (raw_buf_grow(rb, cur + 1024) < 0) {
      if (entries != stack_entries) free(entries);
      return NULL;
    }
    dst = rb->data + cur;
    safe_end = rb->data + rb->cap - 128;
  }
  *dst++ = '{';
  if (indent) {
    dst = raw_emit_indent_nl(dst, sep_len - 1);
  }

  for (Py_ssize_t i = 0; i < cnt; i++) {
    if (CJSON_UNLIKELY(dst > safe_end)) {
      size_t cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + 1024) < 0) {
        if (entries != stack_entries) free(entries);
        return NULL;
      }
      dst = rb->data + cur;
      safe_end = rb->data + rb->cap - 128;
    }
    if (i > 0) {
      if (indent) {
        dst = raw_emit_indent_sep(dst, sep_len);
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 1024) < 0) {
            if (entries != stack_entries) free(entries);
            return NULL;
          }
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
      } else {
        *dst++ = ',';
      }
    }
    const char *ks = entries[i].utf8;
    Py_ssize_t kl = entries[i].len;
    if (CJSON_UNLIKELY(kl == 0)) {
      dst[0] = '"'; dst[1] = '"'; dst += 2;
    } else if (CJSON_LIKELY(kl <= 64)) {
      if (CJSON_LIKELY(copy_if_clean_short(dst + 1, ks, kl))) {
        dst[0] = '"';
        dst[kl + 1] = '"';
        dst += kl + 2;
      } else {
        dst = raw_emit_str_escaped(rb, dst, &safe_end, ks, kl, 0);
        if (CJSON_UNLIKELY(dst == NULL)) {
          if (entries != stack_entries) free(entries);
          return NULL;
        }
      }
    } else {
      size_t cur = (size_t)(dst - rb->data);
      if (CJSON_UNLIKELY(cur + (size_t)kl + 130 > rb->cap)) {
        if (raw_buf_grow(rb, cur + (size_t)kl + 256) < 0) {
          if (entries != stack_entries) free(entries);
          return NULL;
        }
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      Py_ssize_t clean = copy_clean_prefix(dst + 1, ks, kl);
      if (CJSON_LIKELY(clean == kl)) {
        dst[0] = '"';
        dst[kl + 1] = '"';
        dst += kl + 2;
      } else {
        dst = raw_emit_str_escaped(rb, dst, &safe_end, ks, kl, clean);
        if (CJSON_UNLIKELY(dst == NULL)) {
          if (entries != stack_entries) free(entries);
          return NULL;
        }
      }
    }
    *dst++ = ':';
    if (indent) *dst++ = ' ';
    if (CJSON_UNLIKELY(dst > safe_end)) {
      size_t cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + 1024) < 0) {
        if (entries != stack_entries) free(entries);
        return NULL;
      }
      dst = rb->data + cur;
      safe_end = rb->data + rb->cap - 128;
    }
    dst = raw_encode_val_opts(rb, dst, &safe_end, entries[i].val, depth + 1, opts, child_indent, kc, sc);
    if (CJSON_UNLIKELY(dst == NULL)) {
      if (entries != stack_entries) free(entries);
      return NULL;
    }
  }

  if (entries != stack_entries) free(entries);
  if (CJSON_UNLIKELY(dst > safe_end)) {
    size_t cur = (size_t)(dst - rb->data);
    if (raw_buf_grow(rb, cur + 256) < 0) return NULL;
    dst = rb->data + cur;
    safe_end = rb->data + rb->cap - 128;
  }
  if (indent) {
    dst = raw_emit_indent_nl(dst, (size_t)(1 + indent_level * 2));
  }
  *dst++ = '}';
  *safe_end_p = safe_end;
  return dst;
}

static char *raw_encode_val_opts(RawSerBuf *rb, char *dst, char **safe_end_p,
                                 PyObject *obj, int depth, int opts, int indent_level,
                                 RawKeyCache *kc, RawSortSchemaCache *sc) {
  PyTypeObject *tp = Py_TYPE(obj);
  if (tp == &PyUnicode_Type) return raw_emit_str(rb, dst, safe_end_p, obj);
  if (tp == &PyLong_Type) return raw_write_py_long(dst, obj);
  if (tp == &PyFloat_Type) {
    double d = PyFloat_AS_DOUBLE(obj);
    if (CJSON_LIKELY(!isnan(d) && !isinf(d))) return write_double_fast(d, dst);
    memcpy(dst, "null", 4);
    return dst + 4;
  }
  if (obj == Py_None) {
    memcpy(dst, "null", 4);
    return dst + 4;
  }
  if (tp == &PyBool_Type) {
    if (obj == Py_True) {
      memcpy(dst, "true", 4);
      return dst + 4;
    }
    memcpy(dst, "false", 5);
    return dst + 5;
  }
  if (tp == &PyDict_Type) return raw_encode_dict_opts(rb, dst, safe_end_p, obj, depth, opts, indent_level, kc, sc);
  if (tp == &PyList_Type || tp == &PyTuple_Type) return raw_encode_array_opts(rb, dst, safe_end_p, obj, depth, opts, indent_level, kc, sc);
  if ((PyObject *)tp == g_uuid_type) return write_uuid_exact_fast(dst, obj);
  if (PyDateTime_CheckExact(obj) || PyDate_CheckExact(obj)) return write_datetime_exact_fast(dst, obj);
  return NULL;
}

/* ========================================================================= */
/* Persistent Worker Pool (lazily started, configured via CJSON_MAX_THREADS) */
/* ========================================================================= */

typedef struct {
  PyObject *key;
  PyObject *val;
} ParDictPair;

typedef struct {
  int is_dict;
  int opts;
  int base_indent;
  PyObject **items;
  const ParDictPair *pairs;
  Py_ssize_t start;
  Py_ssize_t end;
  RawSerBuf out;
  char *copy_dst;
  int failed;
} ParTask;

typedef struct {
  cjson_mutex_t mu;
  cjson_cond_t cv_work;
  cjson_cond_t cv_done;
  int max_threads;
  int pool_started;
  int active_workers;
  int done_count;
  int phase;
  uint64_t generation;
  ParTask tasks[CJSON_MAX_WORKERS];
} ParPool;

static ParPool g_par_pool;

static void par_run_task_opts(ParTask *t) {
  t->failed = 0;
  RawSerBuf *rb = &t->out;
  if (rb->cap < 65536) {
    if (raw_buf_grow(rb, 65536) < 0) {
      t->failed = 1;
      return;
    }
  }
  RawKeyCache kc;
  memset(kc.slots, 0, sizeof(kc.slots));
  kc.used = 0;
  RawSortSchemaCache sc;
  RawSortSchemaCache *sc_ptr = NULL;
  if (t->opts & OPT_SORT_KEYS) {
    for (int i = 0; i < 16; i++) {
      sc.slots[i].n = 0;
      sc.slots[i].hits = 0;
    }
    sc.buf_used = 0;
    sc.consec_misses = 0;
    sc.disabled = 0;
    sc_ptr = &sc;
  }

  int opts = t->opts;
  int indent = (opts & OPT_INDENT_2);
  int elem_indent = indent ? (t->base_indent + 1) : 0;
  size_t sep_len = (size_t)(2 + elem_indent * 2);

  char *dst = rb->data;
  char *safe_end = rb->data + rb->cap - 128;

  if (!t->is_dict) {
    PyObject **items = t->items;
    if (indent) {
      /* Every element is prefixed with `,\n<2*elem_indent spaces>`.
       * Stitching replaces slice 0's leading `,` with `[` and appends `\n<2*base_indent>]`. */
      for (Py_ssize_t i = t->start; i < t->end; i++) {
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 4096) < 0) { t->failed = 1; return; }
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
        dst = raw_emit_indent_sep(dst, sep_len);
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 4096) < 0) { t->failed = 1; return; }
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
        dst = raw_encode_val_opts(rb, dst, &safe_end, items[i], 1, opts, elem_indent, &kc, sc_ptr);
        if (CJSON_UNLIKELY(dst == NULL)) { t->failed = 1; return; }
      }
    } else {
      for (Py_ssize_t i = t->start; i < t->end; i++) {
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 4096) < 0) { t->failed = 1; return; }
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
        dst = raw_encode_val_opts(rb, dst, &safe_end, items[i], 1, opts, 0, &kc, sc_ptr);
        if (CJSON_UNLIKELY(dst == NULL)) { t->failed = 1; return; }
        *dst++ = ',';
      }
    }
  } else {
    const ParDictPair *pairs = t->pairs;
    if (indent) {
      for (Py_ssize_t i = t->start; i < t->end; i++) {
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 4096) < 0) { t->failed = 1; return; }
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
        dst = raw_emit_indent_sep(dst, sep_len);
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 4096) < 0) { t->failed = 1; return; }
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
        if (CJSON_UNLIKELY(Py_TYPE(pairs[i].key) != &PyUnicode_Type)) { t->failed = 1; return; }
        dst = raw_emit_str(rb, dst, &safe_end, pairs[i].key);
        if (CJSON_UNLIKELY(dst == NULL)) { t->failed = 1; return; }
        *dst++ = ':';
        *dst++ = ' ';
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 1024) < 0) { t->failed = 1; return; }
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
        dst = raw_encode_val_opts(rb, dst, &safe_end, pairs[i].val, 1, opts, elem_indent, &kc, sc_ptr);
        if (CJSON_UNLIKELY(dst == NULL)) { t->failed = 1; return; }
      }
    } else {
      for (Py_ssize_t i = t->start; i < t->end; i++) {
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 4096) < 0) { t->failed = 1; return; }
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
        if (CJSON_UNLIKELY(Py_TYPE(pairs[i].key) != &PyUnicode_Type)) { t->failed = 1; return; }
        dst = raw_emit_str(rb, dst, &safe_end, pairs[i].key);
        if (CJSON_UNLIKELY(dst == NULL)) { t->failed = 1; return; }
        *dst++ = ':';
        if (CJSON_UNLIKELY(dst > safe_end)) {
          size_t cur = (size_t)(dst - rb->data);
          if (raw_buf_grow(rb, cur + 1024) < 0) { t->failed = 1; return; }
          dst = rb->data + cur;
          safe_end = rb->data + rb->cap - 128;
        }
        dst = raw_encode_val_opts(rb, dst, &safe_end, pairs[i].val, 1, opts, 0, &kc, sc_ptr);
        if (CJSON_UNLIKELY(dst == NULL)) { t->failed = 1; return; }
        *dst++ = ',';
      }
    }
  }
  rb->len = (size_t)(dst - rb->data);
}

static void par_run_task(ParTask *t) {
  if (CJSON_UNLIKELY(t->opts != 0)) {
    par_run_task_opts(t);
    return;
  }
  t->failed = 0;
  RawSerBuf *rb = &t->out;
  if (rb->cap < 65536) {
    if (raw_buf_grow(rb, 65536) < 0) {
      t->failed = 1;
      return;
    }
  }
  RawKeyCache kc;
  memset(kc.slots, 0, sizeof(kc.slots));
  kc.used = 0;
  char *dst = rb->data;
  char *safe_end = rb->data + rb->cap - 128;

  if (!t->is_dict) {
    PyObject **items = t->items;
    for (Py_ssize_t i = t->start; i < t->end; i++) {
      if (CJSON_UNLIKELY(dst > safe_end)) {
        size_t cur = (size_t)(dst - rb->data);
        if (raw_buf_grow(rb, cur + 4096) < 0) { t->failed = 1; return; }
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      dst = raw_encode_val(rb, dst, &safe_end, items[i], 1, &kc);
      if (CJSON_UNLIKELY(dst == NULL)) { t->failed = 1; return; }
      *dst++ = ',';
    }
  } else {
    const ParDictPair *pairs = t->pairs;
    for (Py_ssize_t i = t->start; i < t->end; i++) {
      if (CJSON_UNLIKELY(dst > safe_end)) {
        size_t cur = (size_t)(dst - rb->data);
        if (raw_buf_grow(rb, cur + 4096) < 0) { t->failed = 1; return; }
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      if (CJSON_UNLIKELY(Py_TYPE(pairs[i].key) != &PyUnicode_Type)) { t->failed = 1; return; }
      dst = raw_emit_str(rb, dst, &safe_end, pairs[i].key);
      if (CJSON_UNLIKELY(dst == NULL)) { t->failed = 1; return; }
      *dst++ = ':';
      if (CJSON_UNLIKELY(dst > safe_end)) {
        size_t cur = (size_t)(dst - rb->data);
        if (raw_buf_grow(rb, cur + 1024) < 0) { t->failed = 1; return; }
        dst = rb->data + cur;
        safe_end = rb->data + rb->cap - 128;
      }
      dst = raw_encode_val(rb, dst, &safe_end, pairs[i].val, 1, &kc);
      if (CJSON_UNLIKELY(dst == NULL)) { t->failed = 1; return; }
      *dst++ = ',';
    }
  }
  rb->len = (size_t)(dst - rb->data);
}

static void par_worker_loop(int wid) {
  uint64_t seen_gen = 0;
  for (;;) {
    cjson_mutex_lock(&g_par_pool.mu);
    while (g_par_pool.generation == seen_gen) {
      cjson_cond_wait(&g_par_pool.cv_work, &g_par_pool.mu);
    }
    seen_gen = g_par_pool.generation;
    int run = (wid <= g_par_pool.active_workers);
    int phase = g_par_pool.phase;
    cjson_mutex_unlock(&g_par_pool.mu);

    if (run) {
      if (phase == 0) {
        par_run_task(&g_par_pool.tasks[wid]);
      } else {
        ParTask *t = &g_par_pool.tasks[wid];
        if (t->out.len > 0) {
          memcpy(t->copy_dst, t->out.data, t->out.len);
        }
      }
      cjson_mutex_lock(&g_par_pool.mu);
      g_par_pool.done_count++;
      if (g_par_pool.done_count == g_par_pool.active_workers) {
        cjson_cond_signal(&g_par_pool.cv_done);
      }
      cjson_mutex_unlock(&g_par_pool.mu);
    }
  }
}

#if defined(_WIN32)
static unsigned __stdcall par_worker_entry(void *arg) {
  par_worker_loop((int)(intptr_t)arg);
  return 0;
}
#else
static void *par_worker_entry(void *arg) {
  par_worker_loop((int)(intptr_t)arg);
  return NULL;
}
#endif

static int cjson_detect_cpus(void) {
#if defined(_WIN32)
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (si.dwNumberOfProcessors > 0) ? (int)si.dwNumberOfProcessors : 1;
#elif defined(__linux__) && defined(CPU_COUNT)
  cpu_set_t cs;
  CPU_ZERO(&cs);
  if (sched_getaffinity(0, sizeof(cs), &cs) == 0) {
    int n = CPU_COUNT(&cs);
    if (n > 0) return n;
  }
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  return (online > 0) ? (int)online : 1;
#else
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  return (online > 0) ? (int)online : 1;
#endif
}

#if !defined(_WIN32)
static void cjson_parallel_atfork_child(void) {
  /* After fork(), background worker threads do not exist in the child process.
   * Reset pool_started so the child lazily re-initializes mutexes/condvars and
   * spawns fresh workers on its next large dumps() call. */
  g_par_pool.pool_started = 0;
  g_par_pool.active_workers = 0;
  g_par_pool.done_count = 0;
  g_par_pool.generation = 0;
}
#endif

static void cjson_parallel_init(void) {
  const char *env = getenv("CXJSON_MAX_THREADS");
  if (env == NULL || env[0] == '\0') env = getenv("CJSON_MAX_THREADS");
  int n;
  if (env != NULL && env[0] != '\0') {
    int v = atoi(env);
    if (v <= 1) n = 1;
    else if (v > CJSON_MAX_WORKERS) n = CJSON_MAX_WORKERS;
    else n = v;
  } else {
    int ncpu = cjson_detect_cpus();
    if (ncpu <= 1) n = 1;
    else if (ncpu > CJSON_MAX_WORKERS) n = CJSON_MAX_WORKERS;
    else n = ncpu;
  }
  g_par_pool.max_threads = n;
#if !defined(_WIN32)
  static int atfork_registered = 0;
  if (!atfork_registered) {
    pthread_atfork(NULL, NULL, cjson_parallel_atfork_child);
    atfork_registered = 1;
  }
#endif
}

static int cjson_parallel_ensure_started(void) {
  if (CJSON_LIKELY(g_par_pool.pool_started)) return 1;
  cjson_mutex_init(&g_par_pool.mu);
  cjson_cond_init(&g_par_pool.cv_work);
  cjson_cond_init(&g_par_pool.cv_done);
  int nw = g_par_pool.max_threads - 1;
  for (int i = 0; i < nw; i++) {
#if defined(_WIN32)
    uintptr_t h = _beginthreadex(NULL, 0, par_worker_entry, (void *)(intptr_t)(i + 1), 0, NULL);
    if (h == 0) {
      g_par_pool.max_threads = i + 1;
      break;
    }
    CloseHandle((HANDLE)h);
#else
    pthread_t tid;
    if (pthread_create(&tid, NULL, par_worker_entry, (void *)(intptr_t)(i + 1)) != 0) {
      g_par_pool.max_threads = i + 1;
      break;
    }
    pthread_detach(tid);
#endif
  }
  g_par_pool.pool_started = 1;
  return g_par_pool.max_threads > 1;
}

/* Returns 1 on success (result appended to b with closing bracket/brace),
 * 0 to fall back cleanly to single-threaded serialization, -1 on OOM. */
__attribute__((noinline))
static int cjson_parallel_encode_slices(
    JsonBuffer *b, int is_dict, PyObject **items, const ParDictPair *pairs,
    Py_ssize_t total, Py_ssize_t est_bytes) {
  if (g_par_pool.max_threads <= 1) return 0;
  if (!cjson_parallel_ensure_started()) return 0;

  /* Scale active threads by payload size:
   *   512KB..1MB -> up to 2 threads
   *   1MB..4MB   -> up to 4 threads
   *   >= 4MB     -> up to max_threads (capped at min(ncpu, 8) or CJSON_MAX_THREADS) */
  int desired = (est_bytes < 1024 * 1024) ? 2 : ((est_bytes < 4 * 1024 * 1024) ? 4 : g_par_pool.max_threads);
  int nthreads = (desired < g_par_pool.max_threads) ? desired : g_par_pool.max_threads;
  if (nthreads <= 1 || total < nthreads * 16) return 0;

  Py_ssize_t chunk = (total + nthreads - 1) / nthreads;
  int actual = 0;
  for (int t = 0; t < nthreads; t++) {
    Py_ssize_t s = (Py_ssize_t)t * chunk;
    if (s >= total) break;
    Py_ssize_t e = s + chunk;
    if (e > total) e = total;
    g_par_pool.tasks[t].is_dict = is_dict;
    g_par_pool.tasks[t].opts = 0;
    g_par_pool.tasks[t].base_indent = 0;
    g_par_pool.tasks[t].items = items;
    g_par_pool.tasks[t].pairs = pairs;
    g_par_pool.tasks[t].start = s;
    g_par_pool.tasks[t].end = e;
    g_par_pool.tasks[t].failed = 0;
    actual++;
  }
  if (actual <= 1) return 0;

  /* Dispatch workers 1..actual-1 (phase 0: encode) */
  cjson_mutex_lock(&g_par_pool.mu);
  g_par_pool.active_workers = actual - 1;
  g_par_pool.done_count = 0;
  g_par_pool.phase = 0;
  g_par_pool.generation++;
  cjson_cond_broadcast(&g_par_pool.cv_work);
  cjson_mutex_unlock(&g_par_pool.mu);

  /* Main thread executes slice 0 */
  par_run_task(&g_par_pool.tasks[0]);

  /* Wait for workers */
  cjson_mutex_lock(&g_par_pool.mu);
  while (g_par_pool.done_count < g_par_pool.active_workers) {
    cjson_cond_wait(&g_par_pool.cv_done, &g_par_pool.mu);
  }
  cjson_mutex_unlock(&g_par_pool.mu);

  size_t total_bytes = 0;
  for (int t = 0; t < actual; t++) {
    if (g_par_pool.tasks[t].failed) return 0;
    total_bytes += g_par_pool.tasks[t].out.len;
  }
  if (total_bytes == 0) return 0;

  /* Reserve exact total_bytes + 2 ('[' ... ']') and copy chunks */
  if (json_buffer_reserve(b, (Py_ssize_t)total_bytes + 2) < 0) return -1;
  char *dst = b->data + b->len;
  *dst++ = is_dict ? '{' : '[';
  if (total_bytes >= 6 * 1024 * 1024 && actual >= 2) {
    /* Parallel stitch directly into PyBytesObject for >= 6MB payloads */
    char *cur_dst = dst;
    for (int t = 0; t < actual; t++) {
      g_par_pool.tasks[t].copy_dst = cur_dst;
      cur_dst += g_par_pool.tasks[t].out.len;
    }
    cjson_mutex_lock(&g_par_pool.mu);
    g_par_pool.active_workers = actual - 1;
    g_par_pool.done_count = 0;
    g_par_pool.phase = 1;
    g_par_pool.generation++;
    cjson_cond_broadcast(&g_par_pool.cv_work);
    cjson_mutex_unlock(&g_par_pool.mu);

    if (g_par_pool.tasks[0].out.len > 0) {
      memcpy(g_par_pool.tasks[0].copy_dst, g_par_pool.tasks[0].out.data, g_par_pool.tasks[0].out.len);
    }

    cjson_mutex_lock(&g_par_pool.mu);
    while (g_par_pool.done_count < g_par_pool.active_workers) {
      cjson_cond_wait(&g_par_pool.cv_done, &g_par_pool.mu);
    }
    cjson_mutex_unlock(&g_par_pool.mu);
    dst = cur_dst;
  } else {
    for (int t = 0; t < actual; t++) {
      memcpy(dst, g_par_pool.tasks[t].out.data, g_par_pool.tasks[t].out.len);
      dst += g_par_pool.tasks[t].out.len;
    }
  }
  dst[-1] = is_dict ? '}' : ']';
  b->len = (Py_ssize_t)(dst - b->data);
  return 1;
}

/* Parallel array/dict serializer for OPT_SORT_KEYS / OPT_INDENT_2.
 * Returns 1 on success (result appended to b with brackets/braces), 0 to fall back, -1 on OOM. */
__attribute__((noinline))
static int cjson_parallel_encode_slices_opts(
    JsonBuffer *b, int is_dict, PyObject **items, const ParDictPair *pairs,
    Py_ssize_t total, Py_ssize_t est_bytes, int opts, int base_indent) {
  if (g_par_pool.max_threads <= 1) return 0;
  int indent = (opts & OPT_INDENT_2);
  if (indent && base_indent >= 28) return 0;
  if (!cjson_parallel_ensure_started()) return 0;

  int desired = (est_bytes < 1024 * 1024) ? 2 : ((est_bytes < 4 * 1024 * 1024) ? 4 : g_par_pool.max_threads);
  int nthreads = (desired < g_par_pool.max_threads) ? desired : g_par_pool.max_threads;
  if (nthreads <= 1 || total < nthreads * 16) return 0;

  int task_opts = opts & (OPT_INDENT_2 | OPT_SORT_KEYS);
  Py_ssize_t chunk = (total + nthreads - 1) / nthreads;
  int actual = 0;
  for (int t = 0; t < nthreads; t++) {
    Py_ssize_t s = (Py_ssize_t)t * chunk;
    if (s >= total) break;
    Py_ssize_t e = s + chunk;
    if (e > total) e = total;
    g_par_pool.tasks[t].is_dict = is_dict;
    g_par_pool.tasks[t].opts = task_opts;
    g_par_pool.tasks[t].base_indent = base_indent;
    g_par_pool.tasks[t].items = items;
    g_par_pool.tasks[t].pairs = pairs;
    g_par_pool.tasks[t].start = s;
    g_par_pool.tasks[t].end = e;
    g_par_pool.tasks[t].failed = 0;
    actual++;
  }
  if (actual <= 1) return 0;

  cjson_mutex_lock(&g_par_pool.mu);
  g_par_pool.active_workers = actual - 1;
  g_par_pool.done_count = 0;
  g_par_pool.phase = 0;
  g_par_pool.generation++;
  cjson_cond_broadcast(&g_par_pool.cv_work);
  cjson_mutex_unlock(&g_par_pool.mu);

  par_run_task_opts(&g_par_pool.tasks[0]);

  cjson_mutex_lock(&g_par_pool.mu);
  while (g_par_pool.done_count < g_par_pool.active_workers) {
    cjson_cond_wait(&g_par_pool.cv_done, &g_par_pool.mu);
  }
  cjson_mutex_unlock(&g_par_pool.mu);

  size_t total_bytes = 0;
  for (int t = 0; t < actual; t++) {
    if (g_par_pool.tasks[t].failed) return 0;
    total_bytes += g_par_pool.tasks[t].out.len;
  }
  if (total_bytes == 0) return 0;

  size_t close_len = indent ? (size_t)(1 + base_indent * 2) : 0;
  Py_ssize_t reserve_extra = (Py_ssize_t)(total_bytes + close_len + 4);
  if (json_buffer_reserve(b, reserve_extra) < 0) return -1;

  char open_ch = is_dict ? '{' : '[';
  char close_ch = is_dict ? '}' : ']';
  char *start_dst = b->data + b->len;
  char *dst = indent ? start_dst : (start_dst + 1);
  if (!indent) *start_dst = open_ch;

  if (total_bytes >= 6 * 1024 * 1024 && actual >= 2) {
    char *cur_dst = dst;
    for (int t = 0; t < actual; t++) {
      g_par_pool.tasks[t].copy_dst = cur_dst;
      cur_dst += g_par_pool.tasks[t].out.len;
    }
    cjson_mutex_lock(&g_par_pool.mu);
    g_par_pool.active_workers = actual - 1;
    g_par_pool.done_count = 0;
    g_par_pool.phase = 1;
    g_par_pool.generation++;
    cjson_cond_broadcast(&g_par_pool.cv_work);
    cjson_mutex_unlock(&g_par_pool.mu);

    if (g_par_pool.tasks[0].out.len > 0) {
      memcpy(g_par_pool.tasks[0].copy_dst, g_par_pool.tasks[0].out.data, g_par_pool.tasks[0].out.len);
    }

    cjson_mutex_lock(&g_par_pool.mu);
    while (g_par_pool.done_count < g_par_pool.active_workers) {
      cjson_cond_wait(&g_par_pool.cv_done, &g_par_pool.mu);
    }
    cjson_mutex_unlock(&g_par_pool.mu);
    dst = cur_dst;
  } else {
    for (int t = 0; t < actual; t++) {
      memcpy(dst, g_par_pool.tasks[t].out.data, g_par_pool.tasks[t].out.len);
      dst += g_par_pool.tasks[t].out.len;
    }
  }

  if (indent) {
    start_dst[0] = open_ch;
    memcpy(dst, RAW_INDENT_NL, close_len);
    dst += close_len;
    *dst++ = close_ch;
  } else {
    dst[-1] = close_ch;
  }
  b->len = (Py_ssize_t)(dst - b->data);
  return 1;
}

#endif /* !Py_GIL_DISABLED */

#endif /* CJSON_PARALLEL_SER_H */
