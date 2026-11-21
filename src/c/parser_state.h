#ifndef CJSON_PARSER_STATE_H
#define CJSON_PARSER_STATE_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cjson_simd.h"
#include "deserializer.h"

/* Cached immutable Python singletons populated by init_deserializer_types() */
static PyObject *g_empty_str = NULL;
static Py_hash_t g_empty_str_hash = 0;
static PyObject *g_ascii_chars[128] = {NULL};
static PyObject *g_small_ints[262] = {NULL};

#define CJSON_MAX_PARSE_DEPTH 1024

#ifndef CJSON_KEY_CACHE_SIZE
#define CJSON_KEY_CACHE_SIZE 2048
#endif

#ifndef CJSON_STACK_ITEMS_CAP
#define CJSON_STACK_ITEMS_CAP 512
#endif

#ifndef CJSON_STACK_KV_CAP
#define CJSON_STACK_KV_CAP 128
#endif

#ifndef CJSON_PARSE_SCRATCH_SIZE
#define CJSON_PARSE_SCRATCH_SIZE 2048
#endif

#ifdef Py_GIL_DISABLED
typedef struct {
  const char *ptr;
  uint32_t len;
  uint32_t tag;
  Py_hash_t hash;
} KeyCacheMeta;
#endif

typedef struct {
  PyObject *key;
  PyObject *val;
  Py_hash_t hash;
} KvEntry;

typedef struct {
  size_t base;
  uint8_t is_object;
} ParseFrame;

typedef struct {
  const char *start;
  const char *cur;
  const char *end;

  /* Reusable flat element stack for arrays */
  PyObject **items;
  size_t item_sp;
  size_t item_cap;
  PyObject *stack_items[CJSON_STACK_ITEMS_CAP];

  /* Reusable flat key-value stack for objects */
  KvEntry *kvs;
  size_t kv_sp;
  size_t kv_cap;
  KvEntry stack_kvs[CJSON_STACK_KV_CAP];

  /* Scratch buffer for unescaping strings */
  char *scratch;
  size_t scratch_cap;
  char stack_scratch[CJSON_PARSE_SCRATCH_SIZE];

#ifdef Py_GIL_DISABLED
  int cache_initialized;
  int num_cached;
  uint16_t used_slots[CJSON_KEY_CACHE_SIZE];
  PyObject *cache_strs[CJSON_KEY_CACHE_SIZE];
  KeyCacheMeta cache_meta[CJSON_KEY_CACHE_SIZE];
#endif
} JsonParser;

static inline void parser_init(JsonParser *p, const char *data, Py_ssize_t len) {
  p->start = data;
  p->cur = data;
  p->end = data + len;
  p->items = p->stack_items;
  p->item_sp = 0;
  p->item_cap = CJSON_STACK_ITEMS_CAP;
  p->kvs = p->stack_kvs;
  p->kv_sp = 0;
  p->kv_cap = CJSON_STACK_KV_CAP;
  p->scratch = p->stack_scratch;
  p->scratch_cap = CJSON_PARSE_SCRATCH_SIZE;
#ifdef Py_GIL_DISABLED
  p->cache_initialized = 0;
  p->num_cached = 0;
#endif
}

static inline void parser_cleanup(JsonParser *p) {
  while (p->item_sp > 0) {
    Py_DECREF(p->items[--p->item_sp]);
  }
  if (p->items != p->stack_items) {
    PyMem_Free(p->items);
  }
  while (p->kv_sp > 0) {
    p->kv_sp--;
    Py_XDECREF(p->kvs[p->kv_sp].key);
    Py_XDECREF(p->kvs[p->kv_sp].val);
  }
  if (p->kvs != p->stack_kvs) {
    PyMem_Free(p->kvs);
  }
  if (p->scratch != p->stack_scratch) {
    PyMem_Free(p->scratch);
  }
#ifdef Py_GIL_DISABLED
  if (p->cache_initialized) {
    for (int i = 0; i < p->num_cached; i++) {
      Py_DECREF(p->cache_strs[p->used_slots[i]]);
    }
  }
#endif
}

__attribute__((noinline))
static int parser_grow_kvs(JsonParser *p) {
  size_t new_cap = p->kv_cap * 2;
  KvEntry *new_kvs;
  if (p->kvs == p->stack_kvs) {
    new_kvs = (KvEntry *)PyMem_Malloc(new_cap * sizeof(KvEntry));
    if (!new_kvs) {
      PyErr_NoMemory();
      return -1;
    }
    memcpy(new_kvs, p->stack_kvs, p->kv_sp * sizeof(KvEntry));
  } else {
    new_kvs = (KvEntry *)PyMem_Realloc(p->kvs, new_cap * sizeof(KvEntry));
    if (!new_kvs) {
      PyErr_NoMemory();
      return -1;
    }
  }
  p->kvs = new_kvs;
  p->kv_cap = new_cap;
  return 0;
}

__attribute__((noinline))
static int parser_grow_items(JsonParser *p) {
  size_t new_cap = p->item_cap * 2;
  PyObject **new_items;
  if (p->items == p->stack_items) {
    new_items = (PyObject **)PyMem_Malloc(new_cap * sizeof(PyObject *));
    if (!new_items) {
      PyErr_NoMemory();
      return -1;
    }
    memcpy(new_items, p->stack_items, p->item_sp * sizeof(PyObject *));
  } else {
    new_items = (PyObject **)PyMem_Realloc(p->items, new_cap * sizeof(PyObject *));
    if (!new_items) {
      PyErr_NoMemory();
      return -1;
    }
  }
  p->items = new_items;
  p->item_cap = new_cap;
  return 0;
}

__attribute__((noinline))
static char *parser_ensure_scratch(JsonParser *p, size_t needed) {
  if (needed <= p->scratch_cap) {
    return p->scratch;
  }
  size_t new_cap = p->scratch_cap * 2;
  while (new_cap < needed) {
    new_cap *= 2;
  }
  char *new_buf;
  if (p->scratch == p->stack_scratch) {
    new_buf = (char *)PyMem_Malloc(new_cap);
    if (!new_buf) {
      PyErr_NoMemory();
      return NULL;
    }
    memcpy(new_buf, p->stack_scratch, p->scratch_cap);
  } else {
    new_buf = (char *)PyMem_Realloc(p->scratch, new_cap);
    if (!new_buf) {
      PyErr_NoMemory();
      return NULL;
    }
  }
  p->scratch = new_buf;
  p->scratch_cap = new_cap;
  return p->scratch;
}

__attribute__((noinline))
static void raise_decode_error(JsonParser *p, const char *msg, const char *err_pos) {
  if (PyErr_Occurred() && PyErr_ExceptionMatches(PyExc_MemoryError)) {
    return;
  }
  PyErr_Clear();
  Py_ssize_t total_len = (Py_ssize_t)(p->end - p->start);
  Py_ssize_t byte_pos = (err_pos >= p->start && err_pos <= p->end)
                            ? (Py_ssize_t)(err_pos - p->start)
                            : 0;
  PyObject *doc_obj = PyUnicode_DecodeUTF8(p->start, total_len, "replace");
  Py_ssize_t char_pos = 0;
  if (doc_obj != NULL) {
    if (byte_pos == total_len) {
      char_pos = PyUnicode_GET_LENGTH(doc_obj);
    } else if (byte_pos > 0) {
      PyObject *prefix = PyUnicode_DecodeUTF8(p->start, byte_pos, "replace");
      if (prefix != NULL) {
        char_pos = PyUnicode_GET_LENGTH(prefix);
        Py_DECREF(prefix);
      }
    }
    PyObject *exc = PyObject_CallFunction(JSONDecodeError, "sOn", msg, doc_obj, char_pos);
    Py_DECREF(doc_obj);
    if (exc != NULL) {
      PyErr_SetObject(JSONDecodeError, exc);
      Py_DECREF(exc);
      return;
    }
  }
  PyErr_SetString(JSONDecodeError, msg);
}

#endif /* CJSON_PARSER_STATE_H */
