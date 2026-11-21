#ifndef CJSON_BUFFER_H
#define CJSON_BUFFER_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include <string.h>
#include "cjson_simd.h"

#ifndef CJSON_STACK_BUF_SIZE
#define CJSON_STACK_BUF_SIZE 4096
#endif

#define CJSON_MAX_DEPTH 1024

typedef struct {
  char *data;
  Py_ssize_t len;
  Py_ssize_t cap;
  PyObject *bytes_obj;
  int depth;
  PyObject *default_fn;
  char stack_buf[CJSON_STACK_BUF_SIZE];
} JsonBuffer;

static inline void json_buffer_init(JsonBuffer *b) {
  b->data = b->stack_buf;
  b->len = 0;
  b->cap = CJSON_STACK_BUF_SIZE;
  b->bytes_obj = NULL;
  b->depth = 0;
  b->default_fn = NULL;
}

static inline void json_buffer_free(JsonBuffer *b) {
  Py_XDECREF(b->bytes_obj);
  b->bytes_obj = NULL;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static inline PyObject *json_buffer_finish(JsonBuffer *b) {
  if (b->bytes_obj == NULL) {
    return PyBytes_FromStringAndSize(b->stack_buf, b->len);
  }
  if (b->len != b->cap) {
    if (CJSON_UNLIKELY(_PyBytes_Resize(&b->bytes_obj, b->len) < 0)) {
      b->bytes_obj = NULL;
      return NULL;
    }
  }
  PyObject *res = b->bytes_obj;
  b->bytes_obj = NULL;
  return res;
}

static inline Py_ssize_t compute_capacity(Py_ssize_t needed) {
  if (needed <= 0) return 0;
  if (needed > (PY_SSIZE_T_MAX / 2) + 1) return PY_SSIZE_T_MAX;
  uint64_t v = (uint64_t)needed - 1;
  int lz = __builtin_clzll(v);
  return (Py_ssize_t)(1ULL << (64 - lz));
}

__attribute__((noinline))
static int json_buffer_grow(JsonBuffer *b, Py_ssize_t needed) {
  if (needed < 0) {
    PyErr_NoMemory();
    return -1;
  }
  Py_ssize_t cap;
  Py_ssize_t target = (b->cap < 262144) ? (b->cap * 4) : (b->cap * 2);
  if (needed <= target) {
    cap = (target < 16384) ? 16384 : target;
  } else if (needed >= 262144) {
    cap = needed + 65536;
  } else {
    cap = compute_capacity(needed);
    if (cap < 16384) cap = 16384;
  }

  if (b->bytes_obj == NULL) {
    PyObject *bo = PyBytes_FromStringAndSize(NULL, cap);
    if (CJSON_UNLIKELY(bo == NULL)) {
      return -1;
    }
    char *nd = PyBytes_AS_STRING(bo);
    if (b->len > 0) {
      memcpy(nd, b->stack_buf, (size_t)b->len);
    }
    b->bytes_obj = bo;
    b->data = nd;
    b->cap = cap;
    return 0;
  }

  if (CJSON_UNLIKELY(_PyBytes_Resize(&b->bytes_obj, cap) < 0)) {
    return -1;
  }
  b->data = PyBytes_AS_STRING(b->bytes_obj);
  b->cap = cap;
  return 0;
}

static inline int json_buffer_reserve(JsonBuffer *b, Py_ssize_t additional) {
  Py_ssize_t needed = b->len + additional;
  if (CJSON_LIKELY(needed <= b->cap)) return 0;
  return json_buffer_grow(b, needed);
}

static inline int json_buffer_append(JsonBuffer *b, const char *data, Py_ssize_t len) {
  if (json_buffer_reserve(b, len) < 0) return -1;
  memcpy(b->data + b->len, data, (size_t)len);
  b->len += len;
  return 0;
}

static inline int json_buffer_append_char(JsonBuffer *b, char c) {
  if (CJSON_UNLIKELY(b->len + 1 > b->cap)) {
    if (json_buffer_grow(b, b->len + 1) < 0) return -1;
  }
  b->data[b->len++] = c;
  return 0;
}

static inline int json_buffer_append_null(JsonBuffer *b) {
  if (CJSON_UNLIKELY(b->len + 4 > b->cap)) {
    if (json_buffer_grow(b, b->len + 4) < 0) return -1;
  }
  memcpy(b->data + b->len, "null", 4);
  b->len += 4;
  return 0;
}

static inline int json_buffer_append_true(JsonBuffer *b) {
  if (CJSON_UNLIKELY(b->len + 4 > b->cap)) {
    if (json_buffer_grow(b, b->len + 4) < 0) return -1;
  }
  memcpy(b->data + b->len, "true", 4);
  b->len += 4;
  return 0;
}

static inline int json_buffer_append_false(JsonBuffer *b) {
  if (CJSON_UNLIKELY(b->len + 5 > b->cap)) {
    if (json_buffer_grow(b, b->len + 5) < 0) return -1;
  }
  memcpy(b->data + b->len, "false", 5);
  b->len += 5;
  return 0;
}

static const char CJSON_SPACES[128] =
    "                                                                "
    "                                                                ";

static inline int append_indent_prefix(JsonBuffer *b, char open_char, int level) {
  Py_ssize_t spaces = (Py_ssize_t)level * 2;
  Py_ssize_t needed = 2 + spaces;
  if (CJSON_UNLIKELY(b->len + needed > b->cap)) {
    if (json_buffer_reserve(b, needed) < 0) return -1;
  }
  char *dst = b->data + b->len;
  dst[0] = open_char;
  dst[1] = '\n';
  if (spaces <= 128) {
    memcpy(dst + 2, CJSON_SPACES, (size_t)spaces);
  } else {
    memset(dst + 2, ' ', (size_t)spaces);
  }
  b->len += needed;
  return 0;
}

static inline int append_indent_sep(JsonBuffer *b, int level) {
  Py_ssize_t spaces = (Py_ssize_t)level * 2;
  Py_ssize_t needed = 2 + spaces;
  if (CJSON_UNLIKELY(b->len + needed > b->cap)) {
    if (json_buffer_reserve(b, needed) < 0) return -1;
  }
  char *dst = b->data + b->len;
  dst[0] = ',';
  dst[1] = '\n';
  if (spaces <= 128) {
    memcpy(dst + 2, CJSON_SPACES, (size_t)spaces);
  } else {
    memset(dst + 2, ' ', (size_t)spaces);
  }
  b->len += needed;
  return 0;
}

static inline int append_indent_close(JsonBuffer *b, char close_char, int level) {
  Py_ssize_t spaces = (Py_ssize_t)level * 2;
  Py_ssize_t needed = 2 + spaces;
  if (CJSON_UNLIKELY(b->len + needed > b->cap)) {
    if (json_buffer_reserve(b, needed) < 0) return -1;
  }
  char *dst = b->data + b->len;
  dst[0] = '\n';
  if (spaces <= 128) {
    memcpy(dst + 1, CJSON_SPACES, (size_t)spaces);
  } else {
    memset(dst + 1, ' ', (size_t)spaces);
  }
  dst[1 + spaces] = close_char;
  b->len += needed;
  return 0;
}

#endif /* CJSON_BUFFER_H */
