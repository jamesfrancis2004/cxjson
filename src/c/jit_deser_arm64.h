#ifndef CJSON_JIT_DESER_ARM64_H
#define CJSON_JIT_DESER_ARM64_H

/* Runtime Schema-Guided JIT Deserializer for ARM64 (macOS Apple Silicon & Linux aarch64).
 * Full parity with jit_deser_x86_64.h:
 *   - Compiles specialized machine code for ObjectSchema (sc->split_ok == 1)
 *   - Supports BOTH minified AND pretty-printed/spaced real-world JSON
 *   - Supports BOTH array-of-objects loops AND single/nested/dict-of-dicts objects
 *   - Compiled-in immediate 64/32/16/8-bit key matching (up to 48B keys)
 *   - Zero-call inline true/false/null, "" empty string, 0..99 immortal small ints,
 *     [] empty list, {} empty dict, and direct parse_string_fast/parse_number/parse_object/parse_array
 *   - Exact bailout with Py_DECREF cleanup on any mismatch/error; CXJSON_JIT=0 kill switch */

#if (defined(__APPLE__) || defined(__linux__)) && defined(__aarch64__) && \
    defined(CJSON_SPLIT_DICT_FAST) && \
    PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED) && !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)

#define CJSON_DESER_SCHEMA_JIT 1

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif
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
  PyObject *single_out; /* populated when is_single == 1 */
} JitDeserCtx;

typedef const char *(*CJsonJitDeserLoopFn)(
    JsonParser *jp,       /* X0 */
    const char *p,        /* X1 (points at '{' for array loop, or first char after '{' for single) */
    const char *end,      /* X2 */
    JitDeserCtx *ctx      /* X3 */
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
static PyObject *cjson_djit_new_empty_list(void) {
  return PyList_New(0);
}

__attribute__((noinline))
static PyObject *cjson_djit_new_empty_dict(void) {
  return PyDict_New();
}

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

/* ---- Minimal ARM64 assembler helpers ---- */
typedef struct {
  uint32_t *buf;
  size_t len;
  size_t cap;
} DjitAsm;

static inline void da64_emit(DjitAsm *a, uint32_t insn) {
  if (a->len < a->cap) a->buf[a->len] = insn;
  a->len++;
}
static inline uint32_t da64_mov(int rd, int rm) {
  return 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}
static inline uint32_t da64_movz(int rd, uint16_t imm16, int shift) {
  return 0xD2800000u | ((uint32_t)(shift / 16) << 21) | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}
static inline uint32_t da64_movk(int rd, uint16_t imm16, int shift) {
  return 0xF2800000u | ((uint32_t)(shift / 16) << 21) | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}
static inline uint32_t da64_add_imm(int rd, int rn, uint16_t imm12) {
  return 0x91000000u | ((uint32_t)(imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t da64_sub_w_imm(int rd, int rn, uint16_t imm12) {
  return 0x51000000u | ((uint32_t)(imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t da64_ldr_uoff(int rt, int rn, uint32_t byte_off) {
  return 0xF9400000u | (((byte_off >> 3) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t da64_str_uoff(int rt, int rn, uint32_t byte_off) {
  return 0xF9000000u | (((byte_off >> 3) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t da64_ldur_x(int rt, int rn, int32_t simm9) {
  return 0xF8400000u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t da64_ldur_w(int rt, int rn, int32_t simm9) {
  return 0xB8400000u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t da64_ldurh(int rt, int rn, int32_t simm9) {
  return 0x78400000u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t da64_ldurb(int rt, int rn, int32_t simm9) {
  return 0x38400000u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t da64_cmp_reg(int rn, int rm) {
  return 0xEB00001Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
}
static inline uint32_t da64_cmp_w_reg(int rn, int rm) {
  return 0x6B00001Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
}
static inline uint32_t da64_cmp_w_imm(int rn, uint16_t imm12) {
  return 0x7100001Fu | ((uint32_t)(imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5);
}
static inline uint32_t da64_cbz(int rt, int32_t rel_insns) {
  return 0xB4000000u | (((uint32_t)rel_insns & 0x7FFFFu) << 5) | (uint32_t)rt;
}
static inline uint32_t da64_cbnz_w(int rt, int32_t rel_insns) {
  return 0x35000000u | (((uint32_t)rel_insns & 0x7FFFFu) << 5) | (uint32_t)rt;
}
static inline uint32_t da64_b(int32_t rel_insns) {
  return 0x14000000u | ((uint32_t)rel_insns & 0x3FFFFFFu);
}
static inline uint32_t da64_b_cond(int cond, int32_t rel_insns) {
  return 0x54000000u | (((uint32_t)rel_insns & 0x7FFFFu) << 5) | (uint32_t)(cond & 0xF);
}
static inline uint32_t da64_blr(int rn) {
  return 0xD63F0000u | ((uint32_t)rn << 5);
}

static void da64_load_u64(DjitAsm *a, int rd, uint64_t val) {
  da64_emit(a, da64_movz(rd, (uint16_t)(val & 0xFFFFu), 0));
  if ((val >> 16) & 0xFFFFu) da64_emit(a, da64_movk(rd, (uint16_t)((val >> 16) & 0xFFFFu), 16));
  if ((val >> 32) & 0xFFFFu) da64_emit(a, da64_movk(rd, (uint16_t)((val >> 32) & 0xFFFFu), 32));
  if ((val >> 48) & 0xFFFFu) da64_emit(a, da64_movk(rd, (uint16_t)((val >> 48) & 0xFFFFu), 48));
}

/* Inline RFC 8259 whitespace skip (' ', '\n', '\r', '\t') within [X20, X21).
 * Fast-exits in 2 insns when *X20 > 0x20, and skips 8-space indent chunks in 1 compare! */
static void da64_emit_skip_ws(DjitAsm *a) {
  size_t ws_loop = a->len;
  da64_emit(a, da64_cmp_reg(20, 21));
  size_t br_end = a->len;
  da64_emit(a, da64_b_cond(10, 0)); /* B.GE done */
  da64_emit(a, da64_ldurb(10, 20, 0));
  da64_emit(a, da64_cmp_w_imm(10, ' '));
  size_t br_sp = a->len;
  da64_emit(a, da64_b_cond(0, 0)); /* B.EQ check_8sp */
  size_t br_done1 = a->len;
  da64_emit(a, da64_b_cond(8, 0)); /* B.HI done (> ' ') */
  da64_emit(a, da64_cmp_w_imm(10, '\n'));
  size_t br_adv2 = a->len;
  da64_emit(a, da64_b_cond(0, 0));
  da64_emit(a, da64_cmp_w_imm(10, '\t'));
  size_t br_adv3 = a->len;
  da64_emit(a, da64_b_cond(0, 0));
  da64_emit(a, da64_cmp_w_imm(10, '\r'));
  size_t br_done2 = a->len;
  da64_emit(a, da64_b_cond(1, 0)); /* B.NE done */
  size_t adv = a->len;
  da64_emit(a, da64_add_imm(20, 20, 1));
  da64_emit(a, da64_b((int32_t)(ws_loop - a->len)));

  /* check_8sp: skip 8 spaces at a time when X20 + 8 <= X21 */
  size_t check_8sp = a->len;
  da64_emit(a, da64_add_imm(10, 20, 8));
  da64_emit(a, da64_cmp_reg(10, 21));
  da64_emit(a, da64_b_cond(8, (int32_t)(adv - a->len))); /* B.HI adv (+1) */
  da64_emit(a, da64_ldur_x(10, 20, 0));
  da64_load_u64(a, 11, 0x2020202020202020ULL);
  da64_emit(a, da64_cmp_reg(10, 11));
  da64_emit(a, da64_b_cond(1, (int32_t)(adv - a->len))); /* B.NE adv (+1) */
  da64_emit(a, da64_add_imm(20, 20, 8));
  da64_emit(a, da64_b((int32_t)(ws_loop - a->len)));

  a->buf[br_sp] = da64_b_cond(0, (int32_t)(check_8sp - br_sp));
  a->buf[br_adv2] = da64_b_cond(0, (int32_t)(adv - br_adv2));
  a->buf[br_adv3] = da64_b_cond(0, (int32_t)(adv - br_adv3));
  a->buf[br_end] = da64_b_cond(10, (int32_t)(a->len - br_end));
  a->buf[br_done1] = da64_b_cond(8, (int32_t)(a->len - br_done1));
  a->buf[br_done2] = da64_b_cond(1, (int32_t)(a->len - br_done2));
}

static void da64_emit_match_lit(DjitAsm *a, const char *lit, uint32_t len,
                                size_t *bail_patches, size_t *n_bail) {
  uint32_t off = 0;
  while (off + 8 <= len) {
    uint64_t w;
    memcpy(&w, lit + off, 8);
    da64_emit(a, da64_ldur_x(10, 20, (int32_t)off));
    da64_load_u64(a, 11, w);
    da64_emit(a, da64_cmp_reg(10, 11));
    bail_patches[(*n_bail)++] = a->len;
    da64_emit(a, da64_b_cond(1, 0));
    off += 8;
  }
  if (off + 4 <= len) {
    uint32_t w;
    memcpy(&w, lit + off, 4);
    da64_emit(a, da64_ldur_w(10, 20, (int32_t)off));
    da64_load_u64(a, 11, (uint64_t)w);
    da64_emit(a, da64_cmp_w_reg(10, 11));
    bail_patches[(*n_bail)++] = a->len;
    da64_emit(a, da64_b_cond(1, 0));
    off += 4;
  }
  if (off + 2 <= len) {
    uint16_t w;
    memcpy(&w, lit + off, 2);
    da64_emit(a, da64_ldurh(10, 20, (int32_t)off));
    da64_emit(a, da64_movz(11, w, 0));
    da64_emit(a, da64_cmp_w_reg(10, 11));
    bail_patches[(*n_bail)++] = a->len;
    da64_emit(a, da64_b_cond(1, 0));
    off += 2;
  }
  if (off < len) {
    da64_emit(a, da64_ldurb(10, 20, (int32_t)off));
    da64_emit(a, da64_cmp_w_imm(10, (uint8_t)lit[off]));
    bail_patches[(*n_bail)++] = a->len;
    da64_emit(a, da64_b_cond(1, 0));
  }
  da64_emit(a, da64_add_imm(20, 20, (uint16_t)len));
}

static void da64_emit_call_5(DjitAsm *a, uint64_t fn_addr, uint32_t child_slot,
                             size_t *err_patches, size_t *n_err) {
  da64_emit(a, da64_str_uoff(20, 31, 88));
  da64_emit(a, da64_mov(0, 19));
  da64_emit(a, da64_add_imm(1, 31, 88));
  da64_emit(a, da64_mov(2, 21));
  da64_emit(a, da64_ldur_w(3, 22, (int32_t)offsetof(JitDeserCtx, depth)));
  da64_emit(a, 0x11000463u); /* ADD W3, W3, #1 */
  da64_emit(a, da64_movz(4, (uint16_t)child_slot, 0));
  da64_load_u64(a, 12, fn_addr);
  da64_emit(a, da64_blr(12));
  da64_emit(a, da64_ldr_uoff(20, 31, 88));
  err_patches[(*n_err)++] = a->len;
  da64_emit(a, da64_cbz(0, 0));
}

#define CJSON_DJIT_POOL_BYTES (2 * 1024 * 1024)
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
#if defined(__APPLE__)
static uint8_t *g_djit_mem = NULL;
static size_t g_djit_used = 0;
#endif
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

#if defined(__APPLE__)
  if (g_djit_mem == NULL) {
    void *m = mmap(NULL, CJSON_DJIT_POOL_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (m == MAP_FAILED) { pthread_mutex_unlock(&g_djit_mu); return NULL; }
    g_djit_mem = (uint8_t *)m;
    g_djit_used = 0;
  }
  if (g_djit_used + 16384 > CJSON_DJIT_POOL_BYTES) {
    __atomic_store_n(&g_djit_pool_full, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_djit_mu);
    return NULL;
  }
#endif

  uint32_t tmp_code[6144];
  DjitAsm a = {tmp_code, 0, 6144};

  /* Prologue: 352-byte frame */
  da64_emit(&a, 0xA9800000u | (0x54u << 15) | (30u << 10) | (31u << 5) | 29u);
  da64_emit(&a, 0xA9000000u | (2u << 15) | (20u << 10) | (31u << 5) | 19u);
  da64_emit(&a, 0xA9000000u | (4u << 15) | (22u << 10) | (31u << 5) | 21u);
  da64_emit(&a, 0xA9000000u | (6u << 15) | (24u << 10) | (31u << 5) | 23u);
  da64_emit(&a, 0xA9000000u | (8u << 15) | (26u << 10) | (31u << 5) | 25u);

  da64_emit(&a, da64_mov(19, 0)); /* X19 = jp */
  da64_emit(&a, da64_mov(20, 1)); /* X20 = p */
  da64_emit(&a, da64_mov(21, 2)); /* X21 = end */
  da64_emit(&a, da64_mov(22, 3)); /* X22 = ctx */
  da64_emit(&a, da64_add_imm(24, 31, 96)); /* X24 = split_vals */

  size_t bail_patches[384];
  size_t n_bail = 0;
  size_t err_patches[192];
  size_t n_err = 0;

  size_t loop_top = a.len;
  da64_emit(&a, da64_mov(23, 20)); /* X23 = row_start_p */
  da64_emit(&a, da64_movz(25, 0, 0)); /* X25 = parsed count = 0 */

  for (uint32_t j = 0; j < n; j++) {
    const SchemaKey *sk = &sc->keys[j];
    uint32_t klen = sk->klen;
    if (!is_pretty) {
      char lit[64];
      uint32_t lit_len;
      if (j == 0 && is_single) {
        /* single mode: p already points at '"' of first key */
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
      da64_emit(&a, da64_add_imm(10, 20, (uint16_t)(lit_len + 16u)));
      da64_emit(&a, da64_cmp_reg(10, 21));
      bail_patches[n_bail++] = a.len;
      da64_emit(&a, da64_b_cond(8, 0));
      da64_emit_match_lit(&a, lit, lit_len, bail_patches, &n_bail);
    } else {
      /* Pretty / spaced JSON: match '{' or ',' + ws + '"key"' + ws + ':' + ws */
      if (j == 0) {
        if (!is_single) {
          da64_emit(&a, da64_cmp_reg(20, 21));
          bail_patches[n_bail++] = a.len;
          da64_emit(&a, da64_b_cond(10, 0));
          da64_emit(&a, da64_ldurb(10, 20, 0));
          da64_emit(&a, da64_cmp_w_imm(10, '{'));
          bail_patches[n_bail++] = a.len;
          da64_emit(&a, da64_b_cond(1, 0));
          da64_emit(&a, da64_add_imm(20, 20, 1));
          da64_emit_skip_ws(&a);
        }
      } else {
        da64_emit_skip_ws(&a);
        da64_emit(&a, da64_cmp_reg(20, 21));
        bail_patches[n_bail++] = a.len;
        da64_emit(&a, da64_b_cond(10, 0));
        da64_emit(&a, da64_ldurb(10, 20, 0));
        da64_emit(&a, da64_cmp_w_imm(10, ','));
        bail_patches[n_bail++] = a.len;
        da64_emit(&a, da64_b_cond(1, 0));
        da64_emit(&a, da64_add_imm(20, 20, 1));
        da64_emit_skip_ws(&a);
      }
      char klit[64];
      klit[0] = '"';
      memcpy(klit + 1, PyUnicode_1BYTE_DATA(sk->key), klen);
      klit[1 + klen] = '"';
      uint32_t klit_len = klen + 2u;
      da64_emit(&a, da64_add_imm(10, 20, (uint16_t)(klit_len + 16u)));
      da64_emit(&a, da64_cmp_reg(10, 21));
      bail_patches[n_bail++] = a.len;
      da64_emit(&a, da64_b_cond(8, 0));
      da64_emit_match_lit(&a, klit, klit_len, bail_patches, &n_bail);
      /* Skip optional space before ':', match ':', skip optional ws after ':' */
      da64_emit_skip_ws(&a);
      da64_emit(&a, da64_cmp_reg(20, 21));
      bail_patches[n_bail++] = a.len;
      da64_emit(&a, da64_b_cond(10, 0));
      da64_emit(&a, da64_ldurb(10, 20, 0));
      da64_emit(&a, da64_cmp_w_imm(10, ':'));
      bail_patches[n_bail++] = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 1));
      da64_emit_skip_ws(&a);
      /* Ensure >= 16 bytes remain for safe unaligned short-value loads */
      da64_emit(&a, da64_add_imm(10, 20, 16));
      da64_emit(&a, da64_cmp_reg(10, 21));
      bail_patches[n_bail++] = a.len;
      da64_emit(&a, da64_b_cond(8, 0));
    }

    uint8_t vk = sk->val_kind;
    uint32_t child_slot = (uint32_t)((uint64_t)sk->py_hash & 63u);

    if (vk == 4) {
      /* Bool / null: 100% inline */
      da64_emit(&a, da64_ldur_w(10, 20, 0));
      da64_load_u64(&a, 11, 0x65757274ULL);
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_nt = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_True);
      size_t br_d1 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_nt] = da64_b_cond(1, (int32_t)(a.len - br_nt));
      da64_load_u64(&a, 11, 0x736c6166ULL);
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_nf1 = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_ldurb(12, 20, 4));
      da64_emit(&a, da64_cmp_w_imm(12, 'e'));
      size_t br_nf2 = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 5));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_False);
      size_t br_d2 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_nf1] = da64_b_cond(1, (int32_t)(a.len - br_nf1));
      a.buf[br_nf2] = da64_b_cond(1, (int32_t)(a.len - br_nf2));
      da64_load_u64(&a, 11, 0x6c6c756eULL);
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_nn = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_None);
      size_t br_d3 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_nn] = da64_b_cond(1, (int32_t)(a.len - br_nn));
      da64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      a.buf[br_d1] = da64_b((int32_t)(a.len - br_d1));
      a.buf[br_d2] = da64_b((int32_t)(a.len - br_d2));
      a.buf[br_d3] = da64_b((int32_t)(a.len - br_d3));
    } else if (vk == 1) {
      /* Number: inline 0..99 immortal small-int + direct parse_number */
      da64_emit(&a, da64_ldurb(10, 20, 0));
      da64_emit(&a, da64_sub_w_imm(11, 10, '0'));
      da64_emit(&a, da64_cmp_w_imm(11, 9));
      size_t br_nd = a.len;
      da64_emit(&a, da64_b_cond(8, 0));
      da64_emit(&a, da64_ldurb(12, 20, 1));
      da64_emit(&a, da64_cmp_w_imm(12, ','));
      size_t br_1a = a.len;
      da64_emit(&a, da64_b_cond(0, 0));
      da64_emit(&a, da64_cmp_w_imm(12, '}'));
      size_t br_1b = a.len;
      da64_emit(&a, da64_b_cond(0, 0));
      da64_emit(&a, da64_cmp_w_imm(12, '\n'));
      size_t br_1c = a.len;
      da64_emit(&a, da64_b_cond(0, 0));
      size_t br_zl = a.len;
      da64_emit(&a, da64_cbz(11, 0));
      da64_emit(&a, da64_sub_w_imm(12, 12, '0'));
      da64_emit(&a, da64_cmp_w_imm(12, 9));
      size_t br_nd2 = a.len;
      da64_emit(&a, da64_b_cond(8, 0));
      da64_emit(&a, da64_ldurb(13, 20, 2));
      da64_emit(&a, da64_cmp_w_imm(13, ','));
      size_t br_2a = a.len;
      da64_emit(&a, da64_b_cond(0, 0));
      da64_emit(&a, da64_cmp_w_imm(13, '}'));
      size_t br_2b = a.len;
      da64_emit(&a, da64_b_cond(0, 0));
      da64_emit(&a, da64_cmp_w_imm(13, '\n'));
      size_t br_2f = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      a.buf[br_2a] = da64_b_cond(0, (int32_t)(a.len - br_2a));
      a.buf[br_2b] = da64_b_cond(0, (int32_t)(a.len - br_2b));
      da64_emit(&a, da64_movz(13, 10, 0));
      da64_emit(&a, 0x1B000000u | (13u << 16) | (12u << 10) | (11u << 5) | 11u);
      da64_emit(&a, da64_add_imm(20, 20, 2));
      da64_load_u64(&a, 14, (uint64_t)(uintptr_t)g_djit_small_ints);
      da64_emit(&a, 0xF8607800u | (11u << 16) | (14u << 5) | 0u);
      size_t br_d1 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_1a] = da64_b_cond(0, (int32_t)(a.len - br_1a));
      a.buf[br_1b] = da64_b_cond(0, (int32_t)(a.len - br_1b));
      a.buf[br_1c] = da64_b_cond(0, (int32_t)(a.len - br_1c));
      da64_emit(&a, da64_add_imm(20, 20, 1));
      da64_load_u64(&a, 14, (uint64_t)(uintptr_t)g_djit_small_ints);
      da64_emit(&a, 0xF8607800u | (11u << 16) | (14u << 5) | 0u);
      size_t br_d2 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_nd] = da64_b_cond(8, (int32_t)(a.len - br_nd));
      da64_emit(&a, da64_cmp_w_imm(10, '-'));
      size_t br_neg = a.len;
      da64_emit(&a, da64_b_cond(0, 0));
      da64_emit(&a, da64_ldur_w(10, 20, 0));
      da64_load_u64(&a, 11, 0x6c6c756eULL);
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_na = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_None);
      size_t br_d3 = a.len;
      da64_emit(&a, da64_b(0));
      a.buf[br_na] = da64_b_cond(1, (int32_t)(a.len - br_na));
      da64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      size_t br_d4 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_zl] = da64_cbz(11, (int32_t)(a.len - br_zl));
      a.buf[br_nd2] = da64_b_cond(8, (int32_t)(a.len - br_nd2));
      a.buf[br_2f] = da64_b_cond(1, (int32_t)(a.len - br_2f));
      a.buf[br_neg] = da64_b_cond(0, (int32_t)(a.len - br_neg));
      da64_emit(&a, da64_str_uoff(20, 31, 88));
      da64_emit(&a, da64_mov(0, 19));
      da64_emit(&a, da64_add_imm(1, 31, 88));
      da64_emit(&a, da64_mov(2, 21));
      da64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_djit_parse_num);
      da64_emit(&a, da64_blr(12));
      da64_emit(&a, da64_ldr_uoff(20, 31, 88));
      err_patches[n_err++] = a.len;
      da64_emit(&a, da64_cbz(0, 0));

      a.buf[br_d1] = da64_b((int32_t)(a.len - br_d1));
      a.buf[br_d2] = da64_b((int32_t)(a.len - br_d2));
      a.buf[br_d3] = da64_b((int32_t)(a.len - br_d3));
      a.buf[br_d4] = da64_b((int32_t)(a.len - br_d4));
    } else if (vk == 0) {
      /* String: speculative repeated enum-string fast path (1..14B), inline "", else parse_string_fast / null / any */
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
            da64_emit(&a, da64_ldur_x(10, 20, (int32_t)eoff));
            da64_load_u64(&a, 11, w);
            da64_emit(&a, da64_cmp_reg(10, 11));
            enum_miss[n_emiss++] = a.len;
            da64_emit(&a, da64_b_cond(1, 0));
            eoff += 8;
          }
          if (eoff + 4 <= qlen) {
            uint32_t w; memcpy(&w, qlit + eoff, 4);
            da64_emit(&a, da64_ldur_w(10, 20, (int32_t)eoff));
            da64_load_u64(&a, 11, (uint64_t)w);
            da64_emit(&a, da64_cmp_w_reg(10, 11));
            enum_miss[n_emiss++] = a.len;
            da64_emit(&a, da64_b_cond(1, 0));
            eoff += 4;
          }
          if (eoff + 2 <= qlen) {
            uint16_t w; memcpy(&w, qlit + eoff, 2);
            da64_emit(&a, da64_ldurh(10, 20, (int32_t)eoff));
            da64_emit(&a, da64_movz(11, w, 0));
            da64_emit(&a, da64_cmp_w_reg(10, 11));
            enum_miss[n_emiss++] = a.len;
            da64_emit(&a, da64_b_cond(1, 0));
            eoff += 2;
          }
          if (eoff < qlen) {
            da64_emit(&a, da64_ldurb(10, 20, (int32_t)eoff));
            da64_emit(&a, da64_cmp_w_imm(10, (uint8_t)qlit[eoff]));
            enum_miss[n_emiss++] = a.len;
            da64_emit(&a, da64_b_cond(1, 0));
          }
          /* Hit! Advance p += qlen, return pinned sample_str with mortal/immortal Py_INCREF */
          Py_INCREF(sk->sample_str);
          da64_emit(&a, da64_add_imm(20, 20, (uint16_t)qlen));
          da64_load_u64(&a, 0, (uint64_t)(uintptr_t)sk->sample_str);
          da64_emit(&a, da64_ldr_uoff(10, 0, 0)); /* LDR X10, [X0] (ob_refcnt) */
          da64_emit(&a, 0x3100055Fu); /* ADDS WZR, W10, #1 (Z=1 iff immortal 0xFFFFFFFF) */
          da64_emit(&a, da64_b_cond(0, 3)); /* B.EQ +3 (skip inc if immortal) */
          da64_emit(&a, da64_add_imm(10, 10, 1));
          da64_emit(&a, da64_str_uoff(10, 0, 0));
          br_enum_done = a.len;
          da64_emit(&a, da64_b(0));
          for (size_t em = 0; em < n_emiss; em++) {
            a.buf[enum_miss[em]] = da64_b_cond(1, (int32_t)(a.len - enum_miss[em]));
          }
          has_enum = 1;
        }
      }
      da64_emit(&a, da64_ldurh(10, 20, 0));
      da64_emit(&a, da64_movz(11, 0x2222u, 0)); /* "" */
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_ne = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 2));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)g_djit_empty_str);
      size_t br_d0 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_ne] = da64_b_cond(1, (int32_t)(a.len - br_ne));
      da64_emit(&a, da64_ldurb(10, 20, 0));
      da64_emit(&a, da64_cmp_w_imm(10, '"'));
      size_t br_ns = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 1));
      da64_emit(&a, da64_str_uoff(20, 31, 88));
      da64_emit(&a, da64_mov(0, 19));
      da64_emit(&a, da64_add_imm(1, 31, 88));
      da64_emit(&a, da64_mov(2, 21));
      da64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_djit_parse_str);
      da64_emit(&a, da64_blr(12));
      da64_emit(&a, da64_ldr_uoff(20, 31, 88));
      err_patches[n_err++] = a.len;
      da64_emit(&a, da64_cbz(0, 0));
      size_t br_d1 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_ns] = da64_b_cond(1, (int32_t)(a.len - br_ns));
      da64_emit(&a, da64_ldur_w(10, 20, 0));
      da64_load_u64(&a, 11, 0x6c6c756eULL);
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_sa = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_None);
      size_t br_d2 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_sa] = da64_b_cond(1, (int32_t)(a.len - br_sa));
      da64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      a.buf[br_d0] = da64_b((int32_t)(a.len - br_d0));
      a.buf[br_d1] = da64_b((int32_t)(a.len - br_d1));
      a.buf[br_d2] = da64_b((int32_t)(a.len - br_d2));
      if (has_enum) {
        a.buf[br_enum_done] = da64_b((int32_t)(a.len - br_enum_done));
      }
    } else if (vk == 2) {
      /* Nested dict: inline {}, direct parse_obj on '{', inline null, else any */
      da64_emit(&a, da64_ldurh(10, 20, 0));
      da64_emit(&a, da64_movz(11, 0x7D7Bu, 0)); /* "{}" */
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_ne = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 2));
      da64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_djit_new_empty_dict);
      da64_emit(&a, da64_blr(12));
      err_patches[n_err++] = a.len;
      da64_emit(&a, da64_cbz(0, 0));
      size_t br_d0 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_ne] = da64_b_cond(1, (int32_t)(a.len - br_ne));
      da64_emit(&a, da64_ldurb(10, 20, 0));
      da64_emit(&a, da64_cmp_w_imm(10, '{'));
      size_t br_no = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 1));
      da64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_obj, child_slot, err_patches, &n_err);
      size_t br_d1 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_no] = da64_b_cond(1, (int32_t)(a.len - br_no));
      da64_emit(&a, da64_ldur_w(10, 20, 0));
      da64_load_u64(&a, 11, 0x6c6c756eULL);
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_oa = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_None);
      size_t br_d2 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_oa] = da64_b_cond(1, (int32_t)(a.len - br_oa));
      da64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      a.buf[br_d0] = da64_b((int32_t)(a.len - br_d0));
      a.buf[br_d1] = da64_b((int32_t)(a.len - br_d1));
      a.buf[br_d2] = da64_b((int32_t)(a.len - br_d2));
    } else if (vk == 3) {
      /* Nested list: inline [], direct parse_arr on '[', inline null, else any */
      da64_emit(&a, da64_ldurh(10, 20, 0));
      da64_emit(&a, da64_movz(11, 0x5D5Bu, 0)); /* "[]" */
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_ne = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 2));
      da64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_djit_new_empty_list);
      da64_emit(&a, da64_blr(12));
      err_patches[n_err++] = a.len;
      da64_emit(&a, da64_cbz(0, 0));
      size_t br_d0 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_ne] = da64_b_cond(1, (int32_t)(a.len - br_ne));
      da64_emit(&a, da64_ldurb(10, 20, 0));
      da64_emit(&a, da64_cmp_w_imm(10, '['));
      size_t br_nl = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 1));
      da64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_arr, child_slot, err_patches, &n_err);
      size_t br_d1 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_nl] = da64_b_cond(1, (int32_t)(a.len - br_nl));
      da64_emit(&a, da64_ldur_w(10, 20, 0));
      da64_load_u64(&a, 11, 0x6c6c756eULL);
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_la = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_None);
      size_t br_d2 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_la] = da64_b_cond(1, (int32_t)(a.len - br_la));
      da64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
      a.buf[br_d0] = da64_b((int32_t)(a.len - br_d0));
      a.buf[br_d1] = da64_b((int32_t)(a.len - br_d1));
      a.buf[br_d2] = da64_b((int32_t)(a.len - br_d2));
    } else {
      da64_emit_call_5(&a, (uint64_t)(uintptr_t)&cjson_djit_parse_any, child_slot, err_patches, &n_err);
    }

    da64_emit(&a, da64_str_uoff(0, 24, j * 8u));
    da64_emit(&a, da64_movz(25, (uint16_t)(j + 1u), 0));
  }

  if (is_pretty) {
    da64_emit_skip_ws(&a);
  }
  /* Match closing '}' */
  da64_emit(&a, da64_cmp_reg(20, 21));
  bail_patches[n_bail++] = a.len;
  da64_emit(&a, da64_b_cond(10, 0));
  da64_emit(&a, da64_ldurb(10, 20, 0));
  da64_emit(&a, da64_cmp_w_imm(10, '}'));
  bail_patches[n_bail++] = a.len;
  da64_emit(&a, da64_b_cond(1, 0));
  da64_emit(&a, da64_add_imm(20, 20, 1));

  /* Materialize split-table PyDictObject */
  da64_emit(&a, da64_mov(0, 19));
  da64_emit(&a, da64_mov(1, 22));
  da64_emit(&a, da64_movz(2, (uint16_t)n, 0));
  da64_emit(&a, da64_mov(3, 24));
  da64_load_u64(&a, 12, is_single ? (uint64_t)(uintptr_t)&cjson_djit_finish_single_dict
                                  : (uint64_t)(uintptr_t)&cjson_djit_finish_split_dict);
  da64_emit(&a, da64_blr(12));
  size_t br_finish_err = a.len;
  da64_emit(&a, da64_cbnz_w(0, 0));

  size_t br_done_end = 0, br_done_sep = 0, br_done_sep2 = 0;
  if (!is_single) {
    if (!is_pretty) {
      da64_emit(&a, da64_add_imm(10, 20, 2));
      da64_emit(&a, da64_cmp_reg(10, 21));
      br_done_end = a.len;
      da64_emit(&a, da64_b_cond(8, 0));
      da64_emit(&a, da64_ldurh(10, 20, 0));
      da64_emit(&a, da64_movz(11, 0x7B2Cu, 0)); /* ",{" */
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      br_done_sep = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 1));
      da64_emit(&a, da64_b((int32_t)(loop_top - a.len)));
    } else {
      /* Pretty array loop: skip ws, check ',', skip ws, check '{' */
      da64_emit_skip_ws(&a);
      da64_emit(&a, da64_cmp_reg(20, 21));
      br_done_end = a.len;
      da64_emit(&a, da64_b_cond(10, 0));
      da64_emit(&a, da64_ldurb(10, 20, 0));
      da64_emit(&a, da64_cmp_w_imm(10, ','));
      br_done_sep = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_mov(15, 20)); /* save comma pos in case next isn't '{' */
      da64_emit(&a, da64_add_imm(20, 20, 1));
      da64_emit_skip_ws(&a);
      da64_emit(&a, da64_cmp_reg(20, 21));
      size_t br_restore1 = a.len;
      da64_emit(&a, da64_b_cond(10, 0));
      da64_emit(&a, da64_ldurb(10, 20, 0));
      da64_emit(&a, da64_cmp_w_imm(10, '{'));
      size_t br_restore2 = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_b((int32_t)(loop_top - a.len)));
      a.buf[br_restore1] = da64_b_cond(10, (int32_t)(a.len - br_restore1));
      a.buf[br_restore2] = da64_b_cond(1, (int32_t)(a.len - br_restore2));
      da64_emit(&a, da64_mov(20, 15)); /* restore to ',' so after_item handles it */
      br_done_sep2 = a.len;
      da64_emit(&a, da64_b(0));
    }
  }

  /* ret_ok */
  size_t ret_ok = a.len;
  da64_emit(&a, da64_mov(0, 20));
  size_t epilogue = a.len;
  da64_emit(&a, 0xA9400000u | (8u << 15) | (26u << 10) | (31u << 5) | 25u);
  da64_emit(&a, 0xA9400000u | (6u << 15) | (24u << 10) | (31u << 5) | 23u);
  da64_emit(&a, 0xA9400000u | (4u << 15) | (22u << 10) | (31u << 5) | 21u);
  da64_emit(&a, 0xA9400000u | (2u << 15) | (20u << 10) | (31u << 5) | 19u);
  da64_emit(&a, 0xA8C00000u | (0x2Cu << 15) | (30u << 10) | (31u << 5) | 29u);
  da64_emit(&a, 0xD65F03C0u);

  /* bail_label */
  size_t bail_label = a.len;
  size_t br_no_decref = a.len;
  da64_emit(&a, da64_cbz(25, 0));
  da64_emit(&a, da64_mov(0, 24));
  da64_emit(&a, da64_mov(1, 25));
  da64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_djit_decref_vals);
  da64_emit(&a, da64_blr(12));
  a.buf[br_no_decref] = da64_cbz(25, (int32_t)(a.len - br_no_decref));
  da64_emit(&a, da64_mov(0, 23));
  da64_emit(&a, da64_b((int32_t)(epilogue - a.len)));

  /* err_label */
  size_t err_label = a.len;
  size_t br_no_edecref = a.len;
  da64_emit(&a, da64_cbz(25, 0));
  da64_emit(&a, da64_mov(0, 24));
  da64_emit(&a, da64_mov(1, 25));
  da64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_djit_decref_vals);
  da64_emit(&a, da64_blr(12));
  a.buf[br_no_edecref] = da64_cbz(25, (int32_t)(a.len - br_no_edecref));
  da64_emit(&a, da64_movz(10, 1, 0));
  da64_emit(&a, 0xB8000000u | (((uint32_t)offsetof(JitDeserCtx, error) & 0x1FFu) << 12) | (22u << 5) | 10u);
  size_t ret_err = a.len;
  da64_emit(&a, da64_movz(0, 0, 0));
  da64_emit(&a, da64_b((int32_t)(epilogue - a.len)));

  if (a.len >= a.cap) { pthread_mutex_unlock(&g_djit_mu); return NULL; }

  if (!is_single) {
    a.buf[br_done_end] = da64_b_cond(is_pretty ? 10 : 8, (int32_t)(ret_ok - br_done_end));
    a.buf[br_done_sep] = da64_b_cond(1, (int32_t)(ret_ok - br_done_sep));
    if (is_pretty) a.buf[br_done_sep2] = da64_b((int32_t)(ret_ok - br_done_sep2));
  }
  a.buf[br_finish_err] = da64_cbnz_w(0, (int32_t)(ret_err - br_finish_err));

  for (size_t k = 0; k < n_bail; k++) {
    size_t pos = bail_patches[k];
    int32_t rel = (int32_t)(bail_label - pos);
    a.buf[pos] = da64_b_cond((int)(a.buf[pos] & 0xFu), rel);
  }
  for (size_t k = 0; k < n_err; k++) {
    size_t pos = err_patches[k];
    int32_t rel = (int32_t)(err_label - pos);
    a.buf[pos] = da64_cbz((int)(a.buf[pos] & 0x1Fu), rel);
  }

  size_t code_bytes = a.len * sizeof(uint32_t);
  size_t aligned = (code_bytes + 15u) & ~15u;
  uint8_t *dest = NULL;

#if defined(__APPLE__)
  if (g_djit_used + aligned > CJSON_DJIT_POOL_BYTES) {
    __atomic_store_n(&g_djit_pool_full, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_djit_mu);
    return NULL;
  }
  dest = g_djit_mem + g_djit_used;
  if (__builtin_available(macOS 11.0, *)) {
    pthread_jit_write_protect_np(0);
    memcpy(dest, tmp_code, code_bytes);
    pthread_jit_write_protect_np(1);
  } else {
    pthread_mutex_unlock(&g_djit_mu);
    return NULL;
  }
  sys_icache_invalidate(dest, code_bytes);
  g_djit_used += aligned;
#else
  size_t page_sz = (code_bytes + 4095u) & ~4095u;
  void *page = mmap(NULL, page_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) { pthread_mutex_unlock(&g_djit_mu); return NULL; }
  memcpy(page, tmp_code, code_bytes);
  if (mprotect(page, page_sz, PROT_READ | PROT_EXEC) != 0) {
    munmap(page, page_sz);
    pthread_mutex_unlock(&g_djit_mu);
    return NULL;
  }
  __builtin___clear_cache((char *)page, (char *)page + code_bytes);
  dest = (uint8_t *)page;
#endif

  CJsonJitDeserLoopFn fn = (CJsonJitDeserLoopFn)(uintptr_t)dest;
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

#endif /* (__APPLE__ || __linux__) && __aarch64__ && CJSON_SPLIT_DICT_FAST */
#endif /* CJSON_JIT_DESER_ARM64_H */
