#ifndef CJSON_JIT_DESER_X86_64_H
#define CJSON_JIT_DESER_X86_64_H

/* Runtime Schema-Guided JIT Deserializer for System V AMD64 (Linux & macOS x86_64).
 * Full parity with jit_deser_arm64.h:
 *   - Compiles specialized machine code for ObjectSchema (sc->split_ok == 1)
 *   - Supports BOTH minified AND pretty-printed/spaced real-world JSON
 *   - Supports BOTH array-of-objects loops AND single/nested/dict-of-dicts objects
 *   - Compiled-in immediate 64/32/16/8-bit key matching (up to 48B keys)
 *   - Zero-call inline true/false/null, "" empty string, 0..99 immortal small ints,
 *     [] empty list, {} empty dict, and direct parse_str/parse_num/parse_obj/parse_arr
 *   - Dedicated immutable W^X pages, lock-free O(1) cache, CXJSON_JIT=0 kill switch */

#if (defined(__APPLE__) || defined(__linux__)) && defined(__x86_64__) && \
    defined(CJSON_SPLIT_DICT_FAST) && \
    PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED) && !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)

#define CJSON_DESER_SCHEMA_JIT 1

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct {
  CJsonDictKeysObject *saved_dk;
  const uint8_t *prefix;
  uint32_t psize;
  int depth;
  uint32_t schema_slot;
  int error;
  PyObject *single_out;
} JitDeserCtx;

typedef const char *(*CJsonJitDeserLoopFn)(
    JsonParser *jp,       /* RDI */
    const char *p,        /* RSI */
    const char *end,      /* RDX */
    JitDeserCtx *ctx      /* RCX */
);

static int g_djit_enabled_state = -1;
static PyObject *g_djit_small_ints[100];
static PyObject *g_djit_empty_str = NULL;
static int g_djit_inited = 0;

static inline int cjson_deser_jit_is_enabled(void) {
  if (CJSON_UNLIKELY(g_djit_enabled_state < 0)) {
    const char *env = getenv("CXJSON_JIT");
    g_djit_enabled_state = (env != NULL && env[0] == '0' && env[1] == '\0') ? 0 : 1;
  }
  return g_djit_enabled_state;
}

static void cjson_djit_init_globals(void) {
  if (CJSON_LIKELY(g_djit_inited)) return;
  for (int i = 0; i < 100; i++) {
    g_djit_small_ints[i] = PyLong_FromLong(i);
  }
  g_djit_empty_str = PyUnicode_FromStringAndSize("", 0);
  g_djit_inited = 1;
}

/* ---- C helpers invoked by JIT deserializer ---- */
__attribute__((noinline))
static PyObject *cjson_djit_parse_str(JsonParser *jp, const char **p_io, const char *end) {
  return parse_string_fast(jp, p_io, end);
}

__attribute__((noinline))
static PyObject *cjson_djit_parse_num(JsonParser *jp, const char **p_io, const char *end) {
  return parse_number(jp, p_io, end);
}

__attribute__((noinline))
static PyObject *cjson_djit_parse_obj(
    JsonParser *jp, const char **p_io, const char *end, int depth, uint32_t child_slot) {
  if (CJSON_UNLIKELY(depth >= CJSON_REC_PARSE_DEPTH)) {
    const char *p = *p_io - 1;
    PyObject *v = parse_document_iter(jp, &p, end, depth - 1);
    *p_io = p;
    return v;
  }
  return parse_object_rec(jp, p_io, end, depth, child_slot);
}

__attribute__((noinline))
static PyObject *cjson_djit_parse_arr(
    JsonParser *jp, const char **p_io, const char *end, int depth, uint32_t child_slot) {
  if (CJSON_UNLIKELY(depth >= CJSON_REC_PARSE_DEPTH)) {
    const char *p = *p_io - 1;
    PyObject *v = parse_document_iter(jp, &p, end, depth - 1);
    *p_io = p;
    return v;
  }
  return parse_array_rec(jp, p_io, end, depth, child_slot);
}

__attribute__((noinline))
static PyObject *cjson_djit_new_empty_list(void) { return PyList_New(0); }

__attribute__((noinline))
static PyObject *cjson_djit_new_empty_dict(void) { return PyDict_New(); }

__attribute__((noinline))
static PyObject *cjson_djit_parse_any(
    JsonParser *jp, const char **p_io, const char *end, int depth, uint32_t child_slot) {
  const char *p = *p_io;
  if (p < end && *p == ' ') p++;
  if (CJSON_UNLIKELY(p >= end || (uint8_t)*p <= 0x20)) {
    p = skip_ws(p, end);
    if (CJSON_UNLIKELY(p >= end)) {
      raise_decode_error(jp, "unexpected end of data", p);
      return NULL;
    }
  }
  char vc = *p;
  PyObject *val;
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
      val = parse_object_rec(jp, &p, end, depth + 1, child_slot);
    }
  } else if (vc == '[') {
    if (CJSON_UNLIKELY(depth + 1 >= CJSON_REC_PARSE_DEPTH)) {
      val = parse_document_iter(jp, &p, end, depth);
    } else {
      p++;
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
    raise_unexpected_value_error(jp, p, end);
    return NULL;
  }
  *p_io = p;
  return val;
}

__attribute__((noinline))
static void cjson_djit_decref_vals(PyObject **vals, uint32_t count) {
  for (uint32_t j = 0; j < count; j++) {
    Py_DECREF(vals[j]);
  }
}

static inline PyObject *cjson_djit_build_split_dict_inner(
    JitDeserCtx *ctx, uint32_t expected_len, PyObject **split_vals) {
  PyObject *sdict = PyDict_New();
  if (CJSON_UNLIKELY(sdict == NULL)) {
    cjson_djit_decref_vals(split_vals, expected_len);
    ctx->error = 1;
    return NULL;
  }
  CJsonDictKeysObject *saved_dk = ctx->saved_dk;
  saved_dk->dk_refcnt++;
#if defined(CJSON_SPLIT_DICT_313)
  size_t alloc_sz = offsetof(CJsonDictValues313, values) +
                    (size_t)expected_len * sizeof(PyObject *) +
                    (size_t)expected_len;
  CJsonDictValues313 *dv = (CJsonDictValues313 *)PyMem_Malloc(alloc_sz);
  if (CJSON_UNLIKELY(dv == NULL)) {
    Py_DECREF(sdict);
    cjson_djit_decref_vals(split_vals, expected_len);
    cjson_free_shared_keys(saved_dk);
    PyErr_NoMemory();
    ctx->error = 1;
    return NULL;
  }
  dv->capacity = (uint8_t)expected_len;
  dv->size = (uint8_t)expected_len;
  dv->embedded = 0;
  dv->valid = 1;
  memcpy(dv->values, split_vals, (size_t)expected_len * sizeof(PyObject *));
  memcpy((uint8_t *)&dv->values[expected_len], ctx->prefix, (size_t)expected_len);
  PyDictObject *mp = (PyDictObject *)sdict;
  mp->ma_keys = (PyDictKeysObject *)saved_dk;
  mp->ma_values = (PyDictValues *)dv;
  mp->ma_used = (Py_ssize_t)expected_len;
#else
  char *raw = (char *)PyMem_Malloc((size_t)ctx->psize + (size_t)expected_len * sizeof(PyObject *));
  if (CJSON_UNLIKELY(raw == NULL)) {
    Py_DECREF(sdict);
    cjson_djit_decref_vals(split_vals, expected_len);
    cjson_free_shared_keys(saved_dk);
    PyErr_NoMemory();
    ctx->error = 1;
    return NULL;
  }
  memcpy(raw, ctx->prefix, ctx->psize);
  PyObject **dst_vals = (PyObject **)(raw + ctx->psize);
  memcpy(dst_vals, split_vals, (size_t)expected_len * sizeof(PyObject *));
  PyDictObject *mp = (PyDictObject *)sdict;
  mp->ma_keys = (PyDictKeysObject *)saved_dk;
  mp->ma_values = (PyDictValues *)dst_vals;
  mp->ma_used = (Py_ssize_t)expected_len;
#endif
  for (uint32_t j = 0; j < expected_len; j++) {
    if (PyObject_IS_GC(split_vals[j])) {
      if (!PyObject_GC_IsTracked(sdict)) {
        PyObject_GC_Track(sdict);
      }
      break;
    }
  }
  return sdict;
}

__attribute__((noinline))
static int cjson_djit_finish_split_dict(
    JsonParser *jp, JitDeserCtx *ctx, uint32_t expected_len, PyObject **split_vals) {
  PyObject *sdict = cjson_djit_build_split_dict_inner(ctx, expected_len, split_vals);
  if (CJSON_UNLIKELY(sdict == NULL)) return -1;
  if (CJSON_UNLIKELY(jp->item_sp >= jp->item_cap)) {
    if (parser_grow_items(jp) < 0) {
      Py_DECREF(sdict);
      ctx->error = 1;
      return -1;
    }
  }
  jp->items[jp->item_sp++] = sdict;
  return 0;
}

__attribute__((noinline))
static int cjson_djit_finish_single_dict(
    JsonParser *jp, JitDeserCtx *ctx, uint32_t expected_len, PyObject **split_vals) {
  (void)jp;
  PyObject *sdict = cjson_djit_build_split_dict_inner(ctx, expected_len, split_vals);
  if (CJSON_UNLIKELY(sdict == NULL)) return -1;
  ctx->single_out = sdict;
  return 0;
}

/* ---- Minimal x86_64 assembler helpers ---- */
typedef struct {
  uint8_t *buf;
  size_t len;
  size_t cap;
} DjitX64;

static inline void dx64_b(DjitX64 *a, uint8_t v) {
  if (a->len < a->cap) a->buf[a->len] = v;
  a->len++;
}
static inline void dx64_u16(DjitX64 *a, uint16_t v) {
  dx64_b(a, (uint8_t)v); dx64_b(a, (uint8_t)(v >> 8));
}
static inline void dx64_u32(DjitX64 *a, uint32_t v) {
  dx64_b(a, (uint8_t)v); dx64_b(a, (uint8_t)(v >> 8));
  dx64_b(a, (uint8_t)(v >> 16)); dx64_b(a, (uint8_t)(v >> 24));
}
static inline void dx64_u64(DjitX64 *a, uint64_t v) {
  dx64_u32(a, (uint32_t)v); dx64_u32(a, (uint32_t)(v >> 32));
}
static inline void dx64_mov_rax_u64(DjitX64 *a, uint64_t v) {
  dx64_b(a, 0x48); dx64_b(a, 0xB8); dx64_u64(a, v);
}
static inline void dx64_call_rax(DjitX64 *a) {
  dx64_b(a, 0xFF); dx64_b(a, 0xD0);
}
static inline size_t dx64_jcc32(DjitX64 *a, uint8_t op2) {
  dx64_b(a, 0x0F); dx64_b(a, op2);
  size_t pos = a->len;
  dx64_u32(a, 0);
  return pos;
}
static inline size_t dx64_jmp32(DjitX64 *a) {
  dx64_b(a, 0xE9);
  size_t pos = a->len;
  dx64_u32(a, 0);
  return pos;
}
static inline void dx64_patch32(DjitX64 *a, size_t pos, size_t target) {
  int32_t rel = (int32_t)((intptr_t)target - (intptr_t)(pos + 4));
  if (pos + 4 <= a->cap) memcpy(a->buf + pos, &rel, 4);
}
static inline size_t dx64_jz_rax(DjitX64 *a) {
  dx64_b(a, 0x48); dx64_b(a, 0x85); dx64_b(a, 0xC0);
  return dx64_jcc32(a, 0x84);
}

/* Inline RFC 8259 whitespace skip on R12 (bounds R13), with 8-space indent chunk fast path */
static void dx64_emit_skip_ws(DjitX64 *a) {
  size_t ws_loop = a->len;
  dx64_b(a, 0x4D); dx64_b(a, 0x39); dx64_b(a, 0xEC); /* cmp r12, r13 */
  size_t br_end = dx64_jcc32(a, 0x83); /* jae done */
  dx64_b(a, 0x41); dx64_b(a, 0x0F); dx64_b(a, 0xB6); dx64_b(a, 0x04); dx64_b(a, 0x24); /* movzx eax, byte [r12] */
  dx64_b(a, 0x3C); dx64_b(a, ' '); /* cmp al, ' ' */
  size_t br_sp = dx64_jcc32(a, 0x84); /* je check_8sp */
  size_t br_d1 = dx64_jcc32(a, 0x87); /* ja done */
  dx64_b(a, 0x3C); dx64_b(a, '\n');
  size_t br_a2 = dx64_jcc32(a, 0x84);
  dx64_b(a, 0x3C); dx64_b(a, '\t');
  size_t br_a3 = dx64_jcc32(a, 0x84);
  dx64_b(a, 0x3C); dx64_b(a, '\r');
  size_t br_d2 = dx64_jcc32(a, 0x85); /* jne done */
  size_t adv = a->len;
  dx64_b(a, 0x49); dx64_b(a, 0xFF); dx64_b(a, 0xC4); /* inc r12 */
  size_t br_l = dx64_jmp32(a);
  dx64_patch32(a, br_l, ws_loop);

  /* check_8sp: skip 8 spaces at a time when r12 + 8 <= r13 */
  size_t check_8sp = a->len;
  dx64_b(a, 0x49); dx64_b(a, 0x8D); dx64_b(a, 0x44); dx64_b(a, 0x24); dx64_b(a, 8);
  dx64_b(a, 0x4C); dx64_b(a, 0x39); dx64_b(a, 0xE8);
  size_t br_s1 = dx64_jcc32(a, 0x87); /* ja adv */
  dx64_mov_rax_u64(a, 0x2020202020202020ULL);
  dx64_b(a, 0x49); dx64_b(a, 0x39); dx64_b(a, 0x04); dx64_b(a, 0x24); /* cmp [r12], rax */
  size_t br_s2 = dx64_jcc32(a, 0x85); /* jne adv */
  dx64_b(a, 0x49); dx64_b(a, 0x83); dx64_b(a, 0xC4); dx64_b(a, 8); /* add r12, 8 */
  size_t br_l2 = dx64_jmp32(a);
  dx64_patch32(a, br_l2, ws_loop);

  dx64_patch32(a, br_sp, check_8sp);
  dx64_patch32(a, br_s1, adv);
  dx64_patch32(a, br_s2, adv);
  dx64_patch32(a, br_a2, adv);
  dx64_patch32(a, br_a3, adv);
  dx64_patch32(a, br_end, a->len);
  dx64_patch32(a, br_d1, a->len);
  dx64_patch32(a, br_d2, a->len);
}

/* Match exact bytes at [R12 + 0..len-1], advance R12 += len */
static void dx64_emit_match_lit(DjitX64 *a, const char *lit, uint32_t len,
                                size_t *bail_patches, size_t *n_bail) {
  uint32_t off = 0;
  while (off + 8 <= len) {
    uint64_t w;
    memcpy(&w, lit + off, 8);
    dx64_mov_rax_u64(a, w);
    /* cmp [r12 + off], rax */
    dx64_b(a, 0x49); dx64_b(a, 0x39); dx64_b(a, 0x44); dx64_b(a, 0x24); dx64_b(a, (uint8_t)off);
    bail_patches[(*n_bail)++] = dx64_jcc32(a, 0x85);
    off += 8;
  }
  if (off + 4 <= len) {
    uint32_t w;
    memcpy(&w, lit + off, 4);
    dx64_b(a, 0x41); dx64_b(a, 0x81); dx64_b(a, 0x7C); dx64_b(a, 0x24); dx64_b(a, (uint8_t)off); dx64_u32(a, w);
    bail_patches[(*n_bail)++] = dx64_jcc32(a, 0x85);
    off += 4;
  }
  if (off + 2 <= len) {
    uint16_t w;
    memcpy(&w, lit + off, 2);
    dx64_b(a, 0x66); dx64_b(a, 0x41); dx64_b(a, 0x81); dx64_b(a, 0x7C); dx64_b(a, 0x24); dx64_b(a, (uint8_t)off); dx64_u16(a, w);
    bail_patches[(*n_bail)++] = dx64_jcc32(a, 0x85);
    off += 2;
  }
  if (off < len) {
    dx64_b(a, 0x41); dx64_b(a, 0x80); dx64_b(a, 0x7C); dx64_b(a, 0x24); dx64_b(a, (uint8_t)off); dx64_b(a, (uint8_t)lit[off]);
    bail_patches[(*n_bail)++] = dx64_jcc32(a, 0x85);
  }
  dx64_b(a, 0x49); dx64_b(a, 0x83); dx64_b(a, 0xC4); dx64_b(a, (uint8_t)len); /* add r12, len */
}

/* Call 5-arg parser: fn(jp=RBP, &p_tmp=[RSP+0], end=R13, depth=ctx->depth+1, child_slot) */
static void dx64_emit_call_5(DjitX64 *a, uint64_t fn_addr, uint32_t child_slot,
                             size_t *err_patches, size_t *n_err) {
  dx64_b(a, 0x4C); dx64_b(a, 0x89); dx64_b(a, 0x24); dx64_b(a, 0x24); /* mov [rsp], r12 */
  dx64_b(a, 0x48); dx64_b(a, 0x89); dx64_b(a, 0xEF); /* mov rdi, rbp */
  dx64_b(a, 0x48); dx64_b(a, 0x89); dx64_b(a, 0xE6); /* mov rsi, rsp */
  dx64_b(a, 0x4C); dx64_b(a, 0x89); dx64_b(a, 0xEA); /* mov rdx, r13 */
  dx64_b(a, 0x41); dx64_b(a, 0x8B); dx64_b(a, 0x4E); dx64_b(a, (uint8_t)offsetof(JitDeserCtx, depth)); /* mov ecx, [r14+depth] */
  dx64_b(a, 0xFF); dx64_b(a, 0xC1); /* inc ecx */
  dx64_b(a, 0x41); dx64_b(a, 0xB8); dx64_u32(a, child_slot); /* mov r8d, child_slot */
  dx64_mov_rax_u64(a, fn_addr);
  dx64_call_rax(a);
  dx64_b(a, 0x4C); dx64_b(a, 0x8B); dx64_b(a, 0x24); dx64_b(a, 0x24); /* mov r12, [rsp] */
  err_patches[(*n_err)++] = dx64_jz_rax(a);
}

/* Call 3-arg parser: fn(jp=RBP, &p_tmp=[RSP+0], end=R13) */
static void dx64_emit_call_3(DjitX64 *a, uint64_t fn_addr,
                             size_t *err_patches, size_t *n_err) {
  dx64_b(a, 0x4C); dx64_b(a, 0x89); dx64_b(a, 0x24); dx64_b(a, 0x24); /* mov [rsp], r12 */
  dx64_b(a, 0x48); dx64_b(a, 0x89); dx64_b(a, 0xEF); /* mov rdi, rbp */
  dx64_b(a, 0x48); dx64_b(a, 0x89); dx64_b(a, 0xE6); /* mov rsi, rsp */
  dx64_b(a, 0x4C); dx64_b(a, 0x89); dx64_b(a, 0xEA); /* mov rdx, r13 */
  dx64_mov_rax_u64(a, fn_addr);
  dx64_call_rax(a);
  dx64_b(a, 0x4C); dx64_b(a, 0x8B); dx64_b(a, 0x24); dx64_b(a, 0x24); /* mov r12, [rsp] */
  err_patches[(*n_err)++] = dx64_jz_rax(a);
}

#define CJSON_DJIT_CACHE_SLOTS 512

typedef struct {
  uint64_t sig_hash;
  CJsonDictKeysObject *dk;
  uint32_t n;
  uint8_t is_pretty;
  uint8_t is_single;
  uint8_t vkinds[CJSON_SPLIT_MAX_KEYS];
  CJsonJitDeserLoopFn fn;
} DjitSchemaEntry;

static pthread_mutex_t g_djit_mu = PTHREAD_MUTEX_INITIALIZER;
static DjitSchemaEntry g_djit_cache[CJSON_DJIT_CACHE_SLOTS];
static uint32_t g_djit_compiled = 0;
static int g_djit_pool_full = 0;

static CJsonJitDeserLoopFn cjson_deser_jit_get_or_compile(
    const ObjectSchema *sc, int is_pretty, int is_single) {
  if (!cjson_deser_jit_is_enabled() || !sc->split_ok) return NULL;
  uint32_t n = sc->len;
  if (n == 0 || n > CJSON_SPLIT_MAX_KEYS) return NULL;

  uint64_t h = ((uint64_t)(uintptr_t)sc->shared_keys * 0x9E3779B97F4A7C15ULL) ^
               ((uint64_t)n << 48) ^ ((uint64_t)is_pretty << 56) ^ ((uint64_t)is_single << 57);
  uint8_t vkinds[CJSON_SPLIT_MAX_KEYS];
  for (uint32_t j = 0; j < n; j++) {
    if (sc->keys[j].klen == 0 || sc->keys[j].klen > 48) return NULL;
    vkinds[j] = sc->keys[j].val_kind;
    h = (h ^ ((uint64_t)vkinds[j] << 8) ^ sc->keys[j].w0) * 0xBF58476D1CE4E5B9ULL;
  }

  uint32_t set_base = (uint32_t)(h & (CJSON_DJIT_CACHE_SLOTS - 4u));
  uint32_t occupied = 0;
  for (uint32_t w = 0; w < 4; w++) {
    DjitSchemaEntry *e = &g_djit_cache[set_base + w];
    CJsonJitDeserLoopFn fn = __atomic_load_n(&e->fn, __ATOMIC_ACQUIRE);
    if (fn != NULL) {
      occupied++;
      if (e->sig_hash == h && e->dk == sc->shared_keys && e->n == n &&
          e->is_pretty == (uint8_t)is_pretty && e->is_single == (uint8_t)is_single &&
          memcmp(e->vkinds, vkinds, n) == 0) {
        return fn;
      }
    }
  }
  if (occupied == 4 || __atomic_load_n(&g_djit_pool_full, __ATOMIC_RELAXED)) {
    return NULL;
  }

  pthread_mutex_lock(&g_djit_mu);
  cjson_djit_init_globals();

  for (uint32_t w = 0; w < 4; w++) {
    DjitSchemaEntry *e = &g_djit_cache[set_base + w];
    if (e->fn != NULL && e->sig_hash == h && e->dk == sc->shared_keys && e->n == n &&
        e->is_pretty == (uint8_t)is_pretty && e->is_single == (uint8_t)is_single &&
        memcmp(e->vkinds, vkinds, n) == 0) {
      CJsonJitDeserLoopFn fn = e->fn;
      pthread_mutex_unlock(&g_djit_mu);
      return fn;
    }
  }

  uint32_t target_idx = CJSON_DJIT_CACHE_SLOTS;
  for (uint32_t w = 0; w < 4; w++) {
    if (g_djit_cache[set_base + w].fn == NULL) {
      target_idx = set_base + w;
      break;
    }
  }
  if (target_idx >= CJSON_DJIT_CACHE_SLOTS || g_djit_compiled >= CJSON_DJIT_CACHE_SLOTS) {
    if (g_djit_compiled >= CJSON_DJIT_CACHE_SLOTS) {
      __atomic_store_n(&g_djit_pool_full, 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_djit_mu);
    return NULL;
  }

  uint8_t tmp_code[32768];
  DjitX64 a = {tmp_code, 0, sizeof(tmp_code)};

  /* Prologue: 6 pushes + sub rsp, 296 (total 352B, 16B aligned) */
  dx64_b(&a, 0x53); /* push rbx */
  dx64_b(&a, 0x55); /* push rbp */
  dx64_b(&a, 0x41); dx64_b(&a, 0x54); /* push r12 */
  dx64_b(&a, 0x41); dx64_b(&a, 0x55); /* push r13 */
  dx64_b(&a, 0x41); dx64_b(&a, 0x56); /* push r14 */
  dx64_b(&a, 0x41); dx64_b(&a, 0x57); /* push r15 */
  dx64_b(&a, 0x48); dx64_b(&a, 0x81); dx64_b(&a, 0xEC); dx64_u32(&a, 296u);

  dx64_b(&a, 0x48); dx64_b(&a, 0x89); dx64_b(&a, 0xFD); /* mov rbp, rdi (jp) */
  dx64_b(&a, 0x49); dx64_b(&a, 0x89); dx64_b(&a, 0xF4); /* mov r12, rsi (p) */
  dx64_b(&a, 0x49); dx64_b(&a, 0x89); dx64_b(&a, 0xD5); /* mov r13, rdx (end) */
  dx64_b(&a, 0x49); dx64_b(&a, 0x89); dx64_b(&a, 0xCE); /* mov r14, rcx (ctx) */
  dx64_b(&a, 0x4C); dx64_b(&a, 0x8D); dx64_b(&a, 0x7C); dx64_b(&a, 0x24); dx64_b(&a, 32); /* lea r15, [rsp+32] */

  size_t bail_patches[384];
  size_t n_bail = 0;
  size_t err_patches[192];
  size_t n_err = 0;

  size_t loop_top = a.len;
  dx64_b(&a, 0x4C); dx64_b(&a, 0x89); dx64_b(&a, 0x64); dx64_b(&a, 0x24); dx64_b(&a, 8); /* mov [rsp+8], r12 (row_start_p) */
  dx64_b(&a, 0x31); dx64_b(&a, 0xDB); /* xor ebx, ebx (parsed = 0) */

  for (uint32_t j = 0; j < n; j++) {
    const SchemaKey *sk = &sc->keys[j];
    uint32_t klen = sk->klen;
    if (!is_pretty) {
      char lit[64];
      uint32_t lit_len;
      if (j == 0 && is_single) {
        lit[0] = '"';
        memcpy(lit + 1, PyUnicode_1BYTE_DATA(sk->key), klen);
        lit[1 + klen] = '"';
        lit[2 + klen] = ':';
        lit_len = klen + 3u;
      } else {
        lit[0] = (j == 0) ? '{' : ',';
        lit[1] = '"';
        memcpy(lit + 2, PyUnicode_1BYTE_DATA(sk->key), klen);
        lit[2 + klen] = '"';
        lit[3 + klen] = ':';
        lit_len = klen + 4u;
      }
      /* lea rax, [r12 + lit_len + 16]; cmp rax, r13; ja bail */
      dx64_b(&a, 0x49); dx64_b(&a, 0x8D); dx64_b(&a, 0x44); dx64_b(&a, 0x24); dx64_b(&a, (uint8_t)(lit_len + 16u));
      dx64_b(&a, 0x4C); dx64_b(&a, 0x39); dx64_b(&a, 0xE8);
      bail_patches[n_bail++] = dx64_jcc32(&a, 0x87);
      dx64_emit_match_lit(&a, lit, lit_len, bail_patches, &n_bail);
    } else {
      if (j == 0) {
        if (!is_single) {
          dx64_b(&a, 0x4D); dx64_b(&a, 0x39); dx64_b(&a, 0xEC);
          bail_patches[n_bail++] = dx64_jcc32(&a, 0x83);
          dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_b(&a, '{');
          bail_patches[n_bail++] = dx64_jcc32(&a, 0x85);
          dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);
          dx64_emit_skip_ws(&a);
        }
      } else {
        dx64_emit_skip_ws(&a);
        dx64_b(&a, 0x4D); dx64_b(&a, 0x39); dx64_b(&a, 0xEC);
        bail_patches[n_bail++] = dx64_jcc32(&a, 0x83);
        dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_b(&a, ',');
        bail_patches[n_bail++] = dx64_jcc32(&a, 0x85);
        dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);
        dx64_emit_skip_ws(&a);
      }
      char klit[64];
      klit[0] = '"';
      memcpy(klit + 1, PyUnicode_1BYTE_DATA(sk->key), klen);
      klit[1 + klen] = '"';
      uint32_t klit_len = klen + 2u;
      dx64_b(&a, 0x49); dx64_b(&a, 0x8D); dx64_b(&a, 0x44); dx64_b(&a, 0x24); dx64_b(&a, (uint8_t)(klit_len + 16u));
      dx64_b(&a, 0x4C); dx64_b(&a, 0x39); dx64_b(&a, 0xE8);
      bail_patches[n_bail++] = dx64_jcc32(&a, 0x87);
      dx64_emit_match_lit(&a, klit, klit_len, bail_patches, &n_bail);
      dx64_emit_skip_ws(&a);
      dx64_b(&a, 0x4D); dx64_b(&a, 0x39); dx64_b(&a, 0xEC);
      bail_patches[n_bail++] = dx64_jcc32(&a, 0x83);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_b(&a, ':');
      bail_patches[n_bail++] = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);
      dx64_emit_skip_ws(&a);
      dx64_b(&a, 0x49); dx64_b(&a, 0x8D); dx64_b(&a, 0x44); dx64_b(&a, 0x24); dx64_b(&a, 16);
      dx64_b(&a, 0x4C); dx64_b(&a, 0x39); dx64_b(&a, 0xE8);
      bail_patches[n_bail++] = dx64_jcc32(&a, 0x87);
    }

    uint8_t vk = sk->val_kind;
    uint32_t child_slot = (uint32_t)((uint64_t)sk->py_hash & 63u);

    if (vk == 4) {
      dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u32(&a, 0x65757274u); /* "true" */
      size_t br_nt = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 4);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_True);
      size_t br_d1 = dx64_jmp32(&a);

      dx64_patch32(&a, br_nt, a.len);
      dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u32(&a, 0x736c6166u); /* "fals" */
      size_t br_nf1 = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x7C); dx64_b(&a, 0x24); dx64_b(&a, 4); dx64_b(&a, 'e');
      size_t br_nf2 = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 5);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_False);
      size_t br_d2 = dx64_jmp32(&a);

      dx64_patch32(&a, br_nf1, a.len);
      dx64_patch32(&a, br_nf2, a.len);
      dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u32(&a, 0x6c6c756eu); /* "null" */
      size_t br_nn = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 4);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      size_t br_d3 = dx64_jmp32(&a);

      dx64_patch32(&a, br_nn, a.len);
      dx64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      dx64_patch32(&a, br_d1, a.len);
      dx64_patch32(&a, br_d2, a.len);
      dx64_patch32(&a, br_d3, a.len);
    } else if (vk == 1) {
      /* Number: inline 0..99 immortal small-int + direct parse_num */
      dx64_b(&a, 0x41); dx64_b(&a, 0x0F); dx64_b(&a, 0xB6); dx64_b(&a, 0x04); dx64_b(&a, 0x24); /* movzx eax, byte [r12] */
      dx64_b(&a, 0x8D); dx64_b(&a, 0x48); dx64_b(&a, (uint8_t)-'0'); /* lea ecx, [rax-'0'] */
      dx64_b(&a, 0x83); dx64_b(&a, 0xF9); dx64_b(&a, 9);
      size_t br_nd = dx64_jcc32(&a, 0x87); /* ja not_dig */
      dx64_b(&a, 0x41); dx64_b(&a, 0x0F); dx64_b(&a, 0xB6); dx64_b(&a, 0x54); dx64_b(&a, 0x24); dx64_b(&a, 1); /* movzx edx, byte [r12+1] */
      dx64_b(&a, 0x80); dx64_b(&a, 0xFA); dx64_b(&a, ',');
      size_t br_1a = dx64_jcc32(&a, 0x84);
      dx64_b(&a, 0x80); dx64_b(&a, 0xFA); dx64_b(&a, '}');
      size_t br_1b = dx64_jcc32(&a, 0x84);
      dx64_b(&a, 0x80); dx64_b(&a, 0xFA); dx64_b(&a, '\n');
      size_t br_1c = dx64_jcc32(&a, 0x84);
      dx64_b(&a, 0x85); dx64_b(&a, 0xC9); /* test ecx, ecx */
      size_t br_zl = dx64_jcc32(&a, 0x84);
      dx64_b(&a, 0x83); dx64_b(&a, 0xEA); dx64_b(&a, '0'); /* sub edx, '0' */
      dx64_b(&a, 0x83); dx64_b(&a, 0xFA); dx64_b(&a, 9);
      size_t br_nd2 = dx64_jcc32(&a, 0x87);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x7C); dx64_b(&a, 0x24); dx64_b(&a, 2); dx64_b(&a, ',');
      size_t br_2a = dx64_jcc32(&a, 0x84);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x7C); dx64_b(&a, 0x24); dx64_b(&a, 2); dx64_b(&a, '}');
      size_t br_2b = dx64_jcc32(&a, 0x84);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x7C); dx64_b(&a, 0x24); dx64_b(&a, 2); dx64_b(&a, '\n');
      size_t br_2f = dx64_jcc32(&a, 0x85);
      dx64_patch32(&a, br_2a, a.len);
      dx64_patch32(&a, br_2b, a.len);
      /* ecx = ecx * 10 + edx */
      dx64_b(&a, 0x6B); dx64_b(&a, 0xC9); dx64_b(&a, 10); /* imul ecx, ecx, 10 */
      dx64_b(&a, 0x01); dx64_b(&a, 0xD1); /* add ecx, edx */
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 2);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)g_djit_small_ints);
      dx64_b(&a, 0x48); dx64_b(&a, 0x8B); dx64_b(&a, 0x04); dx64_b(&a, 0xC8); /* mov rax, [rax + rcx*8] */
      size_t br_d1 = dx64_jmp32(&a);

      dx64_patch32(&a, br_1a, a.len);
      dx64_patch32(&a, br_1b, a.len);
      dx64_patch32(&a, br_1c, a.len);
      dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)g_djit_small_ints);
      dx64_b(&a, 0x48); dx64_b(&a, 0x8B); dx64_b(&a, 0x04); dx64_b(&a, 0xC8);
      size_t br_d2 = dx64_jmp32(&a);

      dx64_patch32(&a, br_nd, a.len);
      dx64_b(&a, 0x3C); dx64_b(&a, '-');
      size_t br_neg = dx64_jcc32(&a, 0x84);
      dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u32(&a, 0x6c6c756eu);
      size_t br_na = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 4);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      size_t br_d3 = dx64_jmp32(&a);
      dx64_patch32(&a, br_na, a.len);
      dx64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      size_t br_d4 = dx64_jmp32(&a);

      dx64_patch32(&a, br_zl, a.len);
      dx64_patch32(&a, br_nd2, a.len);
      dx64_patch32(&a, br_2f, a.len);
      dx64_patch32(&a, br_neg, a.len);
      dx64_emit_call_3(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_num, err_patches, &n_err);

      dx64_patch32(&a, br_d1, a.len);
      dx64_patch32(&a, br_d2, a.len);
      dx64_patch32(&a, br_d3, a.len);
      dx64_patch32(&a, br_d4, a.len);
    } else if (vk == 0) {
      /* Speculative repeated enum-string fast path (1..14B) */
      int has_enum = 0;
      size_t br_enum_done = 0;
      if (sk->sample_hits >= 2 && sk->sample_str != NULL &&
          PyUnicode_IS_COMPACT_ASCII(sk->sample_str)) {
        Py_ssize_t slen = PyUnicode_GET_LENGTH(sk->sample_str);
        if (slen >= 1 && slen <= 14) {
          char qlit[18];
          qlit[0] = '"';
          memcpy(qlit + 1, PyUnicode_1BYTE_DATA(sk->sample_str), (size_t)slen);
          qlit[1 + slen] = '"';
          uint32_t qlen = (uint32_t)(slen + 2);
          size_t enum_miss[4];
          size_t n_emiss = 0;
          uint32_t eoff = 0;
          while (eoff + 8 <= qlen) {
            uint64_t w; memcpy(&w, qlit + eoff, 8);
            dx64_mov_rax_u64(&a, w);
            dx64_b(&a, 0x49); dx64_b(&a, 0x39); dx64_b(&a, 0x44); dx64_b(&a, 0x24); dx64_b(&a, (uint8_t)eoff);
            enum_miss[n_emiss++] = dx64_jcc32(&a, 0x85);
            eoff += 8;
          }
          if (eoff + 4 <= qlen) {
            uint32_t w; memcpy(&w, qlit + eoff, 4);
            dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x7C); dx64_b(&a, 0x24); dx64_b(&a, (uint8_t)eoff); dx64_u32(&a, w);
            enum_miss[n_emiss++] = dx64_jcc32(&a, 0x85);
            eoff += 4;
          }
          if (eoff + 2 <= qlen) {
            uint16_t w; memcpy(&w, qlit + eoff, 2);
            dx64_b(&a, 0x66); dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x7C); dx64_b(&a, 0x24); dx64_b(&a, (uint8_t)eoff); dx64_u16(&a, w);
            enum_miss[n_emiss++] = dx64_jcc32(&a, 0x85);
            eoff += 2;
          }
          if (eoff < qlen) {
            dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x7C); dx64_b(&a, 0x24); dx64_b(&a, (uint8_t)eoff); dx64_b(&a, (uint8_t)qlit[eoff]);
            enum_miss[n_emiss++] = dx64_jcc32(&a, 0x85);
          }
          Py_INCREF(sk->sample_str);
          dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, (uint8_t)qlen);
          dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)sk->sample_str);
          dx64_b(&a, 0x83); dx64_b(&a, 0x38); dx64_b(&a, 0xFF); /* cmp dword [rax], -1 */
          dx64_b(&a, 0x74); dx64_b(&a, 3); /* je +3 (skip inc if immortal) */
          dx64_b(&a, 0x48); dx64_b(&a, 0xFF); dx64_b(&a, 0x00); /* inc qword [rax] */
          br_enum_done = dx64_jmp32(&a);
          for (size_t em = 0; em < n_emiss; em++) {
            dx64_patch32(&a, enum_miss[em], a.len);
          }
          has_enum = 1;
        }
      }
      dx64_b(&a, 0x66); dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u16(&a, 0x2222u);
      size_t br_ne = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 2);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)g_djit_empty_str);
      size_t br_d0 = dx64_jmp32(&a);

      dx64_patch32(&a, br_ne, a.len);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_b(&a, '"');
      size_t br_ns = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);
      dx64_emit_call_3(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_str, err_patches, &n_err);
      size_t br_d1 = dx64_jmp32(&a);

      dx64_patch32(&a, br_ns, a.len);
      dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u32(&a, 0x6c6c756eu);
      size_t br_sa = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 4);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      size_t br_d2 = dx64_jmp32(&a);

      dx64_patch32(&a, br_sa, a.len);
      dx64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      dx64_patch32(&a, br_d0, a.len);
      dx64_patch32(&a, br_d1, a.len);
      dx64_patch32(&a, br_d2, a.len);
      if (has_enum) {
        dx64_patch32(&a, br_enum_done, a.len);
      }
    } else if (vk == 2) {
      dx64_b(&a, 0x66); dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u16(&a, 0x7D7Bu);
      size_t br_ne = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 2);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_djit_new_empty_dict);
      dx64_call_rax(&a);
      err_patches[n_err++] = dx64_jz_rax(&a);
      size_t br_d0 = dx64_jmp32(&a);

      dx64_patch32(&a, br_ne, a.len);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_b(&a, '{');
      size_t br_no = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);
      dx64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_obj, child_slot, err_patches, &n_err);
      size_t br_d1 = dx64_jmp32(&a);

      dx64_patch32(&a, br_no, a.len);
      dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u32(&a, 0x6c6c756eu);
      size_t br_oa = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 4);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      size_t br_d2 = dx64_jmp32(&a);

      dx64_patch32(&a, br_oa, a.len);
      dx64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      dx64_patch32(&a, br_d0, a.len);
      dx64_patch32(&a, br_d1, a.len);
      dx64_patch32(&a, br_d2, a.len);
    } else if (vk == 3) {
      dx64_b(&a, 0x66); dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u16(&a, 0x5D5Bu);
      size_t br_ne = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 2);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_djit_new_empty_list);
      dx64_call_rax(&a);
      err_patches[n_err++] = dx64_jz_rax(&a);
      size_t br_d0 = dx64_jmp32(&a);

      dx64_patch32(&a, br_ne, a.len);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_b(&a, '[');
      size_t br_nl = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);
      dx64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_arr, child_slot, err_patches, &n_err);
      size_t br_d1 = dx64_jmp32(&a);

      dx64_patch32(&a, br_nl, a.len);
      dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u32(&a, 0x6c6c756eu);
      size_t br_la = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0x83); dx64_b(&a, 0xC4); dx64_b(&a, 4);
      dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      size_t br_d2 = dx64_jmp32(&a);

      dx64_patch32(&a, br_la, a.len);
      dx64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      dx64_patch32(&a, br_d0, a.len);
      dx64_patch32(&a, br_d1, a.len);
      dx64_patch32(&a, br_d2, a.len);
    } else {
      dx64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
    }

    /* mov [r15 + j*8], rax; mov ebx, j + 1 */
    uint32_t voff = j * 8u;
    if (voff < 128u) {
      dx64_b(&a, 0x49); dx64_b(&a, 0x89); dx64_b(&a, 0x47); dx64_b(&a, (uint8_t)voff);
    } else {
      dx64_b(&a, 0x49); dx64_b(&a, 0x89); dx64_b(&a, 0x87); dx64_u32(&a, voff);
    }
    dx64_b(&a, 0xBB); dx64_u32(&a, j + 1u);
  }

  if (is_pretty) dx64_emit_skip_ws(&a);
  dx64_b(&a, 0x4D); dx64_b(&a, 0x39); dx64_b(&a, 0xEC);
  bail_patches[n_bail++] = dx64_jcc32(&a, 0x83);
  dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_b(&a, '}');
  bail_patches[n_bail++] = dx64_jcc32(&a, 0x85);
  dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);

  /* Call finish_split_dict(jp=RBP, ctx=R14, n, split_vals=R15) */
  dx64_b(&a, 0x48); dx64_b(&a, 0x89); dx64_b(&a, 0xEF); /* mov rdi, rbp */
  dx64_b(&a, 0x4C); dx64_b(&a, 0x89); dx64_b(&a, 0xF6); /* mov rsi, r14 */
  dx64_b(&a, 0xBA); dx64_u32(&a, n); /* mov edx, n */
  dx64_b(&a, 0x4C); dx64_b(&a, 0x89); dx64_b(&a, 0xF9); /* mov rcx, r15 */
  dx64_mov_rax_u64(&a, is_single ? (uint64_t)(uintptr_t)&cjson_djit_finish_single_dict
                                 : (uint64_t)(uintptr_t)&cjson_djit_finish_split_dict);
  dx64_call_rax(&a);
  dx64_b(&a, 0x85); dx64_b(&a, 0xC0); /* test eax, eax */
  size_t br_finish_err = dx64_jcc32(&a, 0x85);

  size_t br_done_end = 0, br_done_sep = 0, br_done_sep2 = 0;
  if (!is_single) {
    if (!is_pretty) {
      dx64_b(&a, 0x49); dx64_b(&a, 0x8D); dx64_b(&a, 0x44); dx64_b(&a, 0x24); dx64_b(&a, 2);
      dx64_b(&a, 0x4C); dx64_b(&a, 0x39); dx64_b(&a, 0xE8);
      br_done_end = dx64_jcc32(&a, 0x87);
      dx64_b(&a, 0x66); dx64_b(&a, 0x41); dx64_b(&a, 0x81); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_u16(&a, 0x7B2Cu);
      br_done_sep = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);
      size_t br_l = dx64_jmp32(&a);
      dx64_patch32(&a, br_l, loop_top);
    } else {
      dx64_emit_skip_ws(&a);
      dx64_b(&a, 0x4D); dx64_b(&a, 0x39); dx64_b(&a, 0xEC);
      br_done_end = dx64_jcc32(&a, 0x83);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_b(&a, ',');
      br_done_sep = dx64_jcc32(&a, 0x85);
      dx64_b(&a, 0x4C); dx64_b(&a, 0x89); dx64_b(&a, 0x64); dx64_b(&a, 0x24); dx64_b(&a, 24); /* mov [rsp+24], r12 */
      dx64_b(&a, 0x49); dx64_b(&a, 0xFF); dx64_b(&a, 0xC4);
      dx64_emit_skip_ws(&a);
      dx64_b(&a, 0x4D); dx64_b(&a, 0x39); dx64_b(&a, 0xEC);
      size_t br_r1 = dx64_jcc32(&a, 0x83);
      dx64_b(&a, 0x41); dx64_b(&a, 0x80); dx64_b(&a, 0x3C); dx64_b(&a, 0x24); dx64_b(&a, '{');
      size_t br_r2 = dx64_jcc32(&a, 0x85);
      size_t br_l = dx64_jmp32(&a);
      dx64_patch32(&a, br_l, loop_top);
      dx64_patch32(&a, br_r1, a.len);
      dx64_patch32(&a, br_r2, a.len);
      dx64_b(&a, 0x4C); dx64_b(&a, 0x8B); dx64_b(&a, 0x64); dx64_b(&a, 0x24); dx64_b(&a, 24);
      br_done_sep2 = dx64_jmp32(&a);
    }
  }

  /* ret_ok */
  size_t ret_ok = a.len;
  dx64_b(&a, 0x4C); dx64_b(&a, 0x89); dx64_b(&a, 0xE0); /* mov rax, r12 */
  size_t epilogue = a.len;
  dx64_b(&a, 0x48); dx64_b(&a, 0x81); dx64_b(&a, 0xC4); dx64_u32(&a, 296u);
  dx64_b(&a, 0x41); dx64_b(&a, 0x5F);
  dx64_b(&a, 0x41); dx64_b(&a, 0x5E);
  dx64_b(&a, 0x41); dx64_b(&a, 0x5D);
  dx64_b(&a, 0x41); dx64_b(&a, 0x5C);
  dx64_b(&a, 0x5D);
  dx64_b(&a, 0x5B);
  dx64_b(&a, 0xC3);

  /* bail_label */
  size_t bail_label = a.len;
  dx64_b(&a, 0x85); dx64_b(&a, 0xDB); /* test ebx, ebx */
  size_t br_nd1 = dx64_jcc32(&a, 0x84);
  dx64_b(&a, 0x4C); dx64_b(&a, 0x89); dx64_b(&a, 0xFF); /* mov rdi, r15 */
  dx64_b(&a, 0x89); dx64_b(&a, 0xDE); /* mov esi, ebx */
  dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_djit_decref_vals);
  dx64_call_rax(&a);
  dx64_patch32(&a, br_nd1, a.len);
  dx64_b(&a, 0x48); dx64_b(&a, 0x8B); dx64_b(&a, 0x44); dx64_b(&a, 0x24); dx64_b(&a, 8); /* mov rax, [rsp+8] */
  size_t br_e1 = dx64_jmp32(&a);
  dx64_patch32(&a, br_e1, epilogue);

  /* err_label */
  size_t err_label = a.len;
  dx64_b(&a, 0x85); dx64_b(&a, 0xDB);
  size_t br_nd2 = dx64_jcc32(&a, 0x84);
  dx64_b(&a, 0x4C); dx64_b(&a, 0x89); dx64_b(&a, 0xFF);
  dx64_b(&a, 0x89); dx64_b(&a, 0xDE);
  dx64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_djit_decref_vals);
  dx64_call_rax(&a);
  dx64_patch32(&a, br_nd2, a.len);
  /* mov dword [r14 + offsetof(JitDeserCtx, error)], 1 */
  dx64_b(&a, 0x41); dx64_b(&a, 0xC7); dx64_b(&a, 0x46); dx64_b(&a, (uint8_t)offsetof(JitDeserCtx, error)); dx64_u32(&a, 1u);
  size_t ret_err = a.len;
  dx64_b(&a, 0x31); dx64_b(&a, 0xC0); /* xor eax, eax */
  size_t br_e2 = dx64_jmp32(&a);
  dx64_patch32(&a, br_e2, epilogue);

  if (a.len >= a.cap) { pthread_mutex_unlock(&g_djit_mu); return NULL; }

  if (!is_single) {
    dx64_patch32(&a, br_done_end, ret_ok);
    dx64_patch32(&a, br_done_sep, ret_ok);
    if (is_pretty) dx64_patch32(&a, br_done_sep2, ret_ok);
  }
  dx64_patch32(&a, br_finish_err, ret_err);
  for (size_t k = 0; k < n_bail; k++) dx64_patch32(&a, bail_patches[k], bail_label);
  for (size_t k = 0; k < n_err; k++) dx64_patch32(&a, err_patches[k], err_label);

  size_t page_sz = (a.len + 4095u) & ~4095u;
  void *page = mmap(NULL, page_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) { pthread_mutex_unlock(&g_djit_mu); return NULL; }
  memcpy(page, tmp_code, a.len);
  if (mprotect(page, page_sz, PROT_READ | PROT_EXEC) != 0) {
    munmap(page, page_sz);
    pthread_mutex_unlock(&g_djit_mu);
    return NULL;
  }

  CJsonJitDeserLoopFn fn = (CJsonJitDeserLoopFn)(uintptr_t)page;
  DjitSchemaEntry *slot = &g_djit_cache[target_idx];
  slot->sig_hash = h;
  sc->shared_keys->dk_refcnt++;
  slot->dk = sc->shared_keys;
  slot->n = n;
  slot->is_pretty = (uint8_t)is_pretty;
  slot->is_single = (uint8_t)is_single;
  memcpy(slot->vkinds, vkinds, n);
  __atomic_store_n(&slot->fn, fn, __ATOMIC_RELEASE);
  g_djit_compiled++;
  pthread_mutex_unlock(&g_djit_mu);
  return fn;
}

#endif /* (__APPLE__ || __linux__) && __x86_64__ && CJSON_SPLIT_DICT_FAST */
#endif /* CJSON_JIT_DESER_X86_64_H */
