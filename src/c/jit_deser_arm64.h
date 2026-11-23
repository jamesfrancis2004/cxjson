#ifndef CJSON_JIT_DESER_ARM64_H
#define CJSON_JIT_DESER_ARM64_H

/* Experimental Runtime Schema-Guided JIT Deserializer for ARM64
 * (macOS Apple Silicon & Linux aarch64, CPython 3.12-3.14).
 *
 * Compiles a specialized machine-code loop for arrays of homogeneous JSON
 * objects matching an ObjectSchema (sc->split_ok == 1):
 *   - Immediate 64-bit/32-bit/16-bit/8-bit matching of {"key": and ,"key":
 *     directly against input UTF-8 bytes (zero SchemaKey loads, zero hashing)
 *   - Inline true / false / null and 0..99 immortal small-int parsing (zero C calls)
 *   - Direct calls to parse_string_fast / parse_number / parse_any (no switch cascade)
 *   - Direct split-table PyDictObject materialization into jp->items[]
 *   - Exact bailout to row_start_p with Py_DECREF cleanup on any mismatch/whitespace/error
 *   - Thread-safe W^X compilation, lock-free O(1) cache, CXJSON_JIT=0 kill switch */

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
} JitDeserCtx;

typedef const char *(*CJsonJitDeserLoopFn)(
    JsonParser *jp,       /* X0 */
    const char *p,        /* X1 (points at '{') */
    const char *end,      /* X2 */
    JitDeserCtx *ctx      /* X3 */
);

static int g_djit_enabled_state = -1;
static PyObject *g_djit_small_ints[100];
static int g_djit_small_ints_inited = 0;

static inline int cjson_deser_jit_is_enabled(void) {
  if (CJSON_UNLIKELY(g_djit_enabled_state < 0)) {
    const char *env = getenv("CXJSON_JIT");
    g_djit_enabled_state = (env != NULL && env[0] == '0' && env[1] == '\0') ? 0 : 1;
  }
  return g_djit_enabled_state;
}

static void cjson_djit_init_small_ints(void) {
  if (CJSON_LIKELY(g_djit_small_ints_inited)) return;
  for (int i = 0; i < 100; i++) {
    g_djit_small_ints[i] = PyLong_FromLong(i);
    /* Python 3.12+: 0..256 are immortal singletons; keep ref forever */
  }
  g_djit_small_ints_inited = 1;
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

__attribute__((noinline))
static int cjson_djit_finish_split_dict(
    JsonParser *jp, JitDeserCtx *ctx, uint32_t expected_len, PyObject **split_vals) {
  PyObject *sdict = PyDict_New();
  if (CJSON_UNLIKELY(sdict == NULL)) {
    cjson_djit_decref_vals(split_vals, expected_len);
    ctx->error = 1;
    return -1;
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
    return -1;
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
    return -1;
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

/* ---- Minimal ARM64 assembler helpers for deserializer JIT ---- */
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

/* Emit exact byte-sequence check against [X20 + 0..len-1], branching to bail on mismatch,
 * then advancing X20 += len. Caller ensures [X20 .. X20 + len] is in-bounds. */
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
    da64_emit(a, da64_b_cond(1, 0)); /* B.NE bail */
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

/* Emit call to cjson_djit_parse_any(jp=X19, &p_tmp=[SP+88], end=X21, depth=ctx->depth+1, child_slot) */
static void da64_emit_call_any(DjitAsm *a, uint32_t child_slot,
                               size_t *err_patches, size_t *n_err) {
  da64_emit(a, da64_str_uoff(20, 31, 88));
  da64_emit(a, da64_mov(0, 19));
  da64_emit(a, da64_add_imm(1, 31, 88));
  da64_emit(a, da64_mov(2, 21));
  /* LDR W3, [X22, #offsetof(JitDeserCtx, depth)]; ADD W3, W3, #1 */
  da64_emit(a, da64_ldur_w(3, 22, (int32_t)offsetof(JitDeserCtx, depth)));
  da64_emit(a, 0x11000463u); /* ADD W3, W3, #1 */
  da64_emit(a, da64_movz(4, (uint16_t)child_slot, 0));
  da64_load_u64(a, 12, (uint64_t)(uintptr_t)&cjson_djit_parse_any);
  da64_emit(a, da64_blr(12));
  da64_emit(a, da64_ldr_uoff(20, 31, 88));
  err_patches[(*n_err)++] = a->len;
  da64_emit(a, da64_cbz(0, 0));
}

#define CJSON_DJIT_POOL_BYTES (512 * 1024)
#define CJSON_DJIT_CACHE_SLOTS 128

typedef struct {
  uint64_t sig_hash;
  CJsonDictKeysObject *dk; /* key identity; valid while schema lives */
  uint32_t n;
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

static CJsonJitDeserLoopFn cjson_deser_jit_get_or_compile(const ObjectSchema *sc) {
  if (!cjson_deser_jit_is_enabled() || !sc->split_ok) return NULL;
  uint32_t n = sc->len;
  if (n == 0 || n > CJSON_SPLIT_MAX_KEYS) return NULL;

  uint64_t h = ((uint64_t)(uintptr_t)sc->shared_keys * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)n << 48);
  uint8_t vkinds[CJSON_SPLIT_MAX_KEYS];
  for (uint32_t j = 0; j < n; j++) {
    if (sc->keys[j].klen == 0 || sc->keys[j].klen > 48) return NULL;
    vkinds[j] = sc->keys[j].val_kind;
    h = (h ^ ((uint64_t)vkinds[j] << 8) ^ sc->keys[j].w0) * 0xBF58476D1CE4E5B9ULL;
  }

  /* Lock-free O(1) 2-way lookup */
  uint32_t set_base = (uint32_t)(h & (CJSON_DJIT_CACHE_SLOTS - 2u));
  uint32_t occupied = 0;
  for (uint32_t w = 0; w < 2; w++) {
    DjitSchemaEntry *e = &g_djit_cache[set_base + w];
    CJsonJitDeserLoopFn fn = __atomic_load_n(&e->fn, __ATOMIC_ACQUIRE);
    if (fn != NULL) {
      occupied++;
      if (e->sig_hash == h && e->dk == sc->shared_keys && e->n == n &&
          memcmp(e->vkinds, vkinds, n) == 0) {
        return fn;
      }
    }
  }
  if (occupied == 2 || __atomic_load_n(&g_djit_pool_full, __ATOMIC_RELAXED)) {
    return NULL;
  }

  pthread_mutex_lock(&g_djit_mu);
  cjson_djit_init_small_ints();

  for (uint32_t w = 0; w < 2; w++) {
    DjitSchemaEntry *e = &g_djit_cache[set_base + w];
    if (e->fn != NULL && e->sig_hash == h && e->dk == sc->shared_keys && e->n == n &&
        memcmp(e->vkinds, vkinds, n) == 0) {
      CJsonJitDeserLoopFn fn = e->fn;
      pthread_mutex_unlock(&g_djit_mu);
      return fn;
    }
  }

  uint32_t target_idx = CJSON_DJIT_CACHE_SLOTS;
  for (uint32_t w = 0; w < 2; w++) {
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
  if (g_djit_used + 8192 > CJSON_DJIT_POOL_BYTES) {
    __atomic_store_n(&g_djit_pool_full, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_djit_mu);
    return NULL;
  }
#endif

  uint32_t tmp_code[4096];
  DjitAsm a = {tmp_code, 0, 4096};

  /* Prologue: 352-byte stack frame ([SP+0..79]=saved regs, [SP+88]=p_tmp, [SP+96..351]=split_vals[32])
   * STP X29, X30, [SP, #-352]! -> imm7 = -44 = 0x54 */
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

  size_t bail_patches[256];
  size_t n_bail = 0;
  size_t err_patches[128];
  size_t n_err = 0;

  /* Loop over consecutive objects '{...},{...},...' */
  size_t loop_top = a.len;
  da64_emit(&a, da64_mov(23, 20)); /* X23 = row_start_p */
  da64_emit(&a, da64_movz(25, 0, 0)); /* X25 = parsed count = 0 */

  for (uint32_t j = 0; j < n; j++) {
    const SchemaKey *sk = &sc->keys[j];
    uint32_t klen = sk->klen;
    char lit[64];
    lit[0] = (j == 0) ? '{' : ',';
    lit[1] = '"';
    memcpy(lit + 2, PyUnicode_1BYTE_DATA(sk->key), klen);
    lit[2 + klen] = '"';
    lit[3 + klen] = ':';
    uint32_t lit_len = klen + 4u;

    /* Bounds check: p + lit_len + 16 <= end (ensures safe unaligned loads for key + short value) */
    da64_emit(&a, da64_add_imm(10, 20, (uint16_t)(lit_len + 16u)));
    da64_emit(&a, da64_cmp_reg(10, 21));
    bail_patches[n_bail++] = a.len;
    da64_emit(&a, da64_b_cond(8, 0)); /* B.HI bail */

    /* Match {"key": or ,"key": directly */
    da64_emit_match_lit(&a, lit, lit_len, bail_patches, &n_bail);

    /* Parse value according to predicted val_kind */
    uint8_t vk = sk->val_kind;
    uint32_t child_slot = (uint32_t)((uint64_t)sk->py_hash & 63u);

    if (vk == 4) {
      /* Bool / null: 100% inline! */
      da64_emit(&a, da64_ldur_w(10, 20, 0));
      da64_load_u64(&a, 11, 0x65757274ULL); /* "true" */
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_not_true = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_True);
      size_t br_done1 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_not_true] = da64_b_cond(1, (int32_t)(a.len - br_not_true));
      da64_load_u64(&a, 11, 0x736c6166ULL); /* "fals" */
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_not_false1 = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_ldurb(12, 20, 4));
      da64_emit(&a, da64_cmp_w_imm(12, 'e'));
      size_t br_not_false2 = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 5));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_False);
      size_t br_done2 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_not_false1] = da64_b_cond(1, (int32_t)(a.len - br_not_false1));
      a.buf[br_not_false2] = da64_b_cond(1, (int32_t)(a.len - br_not_false2));
      da64_load_u64(&a, 11, 0x6c6c756eULL); /* "null" */
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_not_null = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_None);
      size_t br_done3 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_not_null] = da64_b_cond(1, (int32_t)(a.len - br_not_null));
      da64_emit_call_any(&a, child_slot, err_patches, &n_err);
      a.buf[br_done1] = da64_b((int32_t)(a.len - br_done1));
      a.buf[br_done2] = da64_b((int32_t)(a.len - br_done2));
      a.buf[br_done3] = da64_b((int32_t)(a.len - br_done3));
    } else if (vk == 1) {
      /* Number: inline 0..99 immortal small-int fast path, else parse_number / null / any */
      da64_emit(&a, da64_ldurb(10, 20, 0));
      da64_emit(&a, da64_sub_w_imm(11, 10, '0')); /* W11 = d0 */
      da64_emit(&a, da64_cmp_w_imm(11, 9));
      size_t br_not_dig = a.len;
      da64_emit(&a, da64_b_cond(8, 0)); /* B.HI not_dig */
      /* Check next byte W12 = p[1] */
      da64_emit(&a, da64_ldurb(12, 20, 1));
      da64_emit(&a, da64_cmp_w_imm(12, ','));
      size_t br_1d_a = a.len;
      da64_emit(&a, da64_b_cond(0, 0)); /* B.EQ one_digit */
      da64_emit(&a, da64_cmp_w_imm(12, '}'));
      size_t br_1d_b = a.len;
      da64_emit(&a, da64_b_cond(0, 0)); /* B.EQ one_digit */
      /* 2-digit check: d0 != 0 && '0' <= p[1] <= '9' && (p[2] == ',' || p[2] == '}') */
      size_t br_zero_lead = a.len;
      da64_emit(&a, da64_cbz(11, 0)); /* CBZ X11, slow_num (no leading zero!) */
      da64_emit(&a, da64_sub_w_imm(12, 12, '0')); /* W12 = d1 */
      da64_emit(&a, da64_cmp_w_imm(12, 9));
      size_t br_not_d2 = a.len;
      da64_emit(&a, da64_b_cond(8, 0));
      da64_emit(&a, da64_ldurb(13, 20, 2));
      da64_emit(&a, da64_cmp_w_imm(13, ','));
      size_t br_2d_a = a.len;
      da64_emit(&a, da64_b_cond(0, 0));
      da64_emit(&a, da64_cmp_w_imm(13, '}'));
      size_t br_2d_fail = a.len;
      da64_emit(&a, da64_b_cond(1, 0)); /* B.NE slow_num */
      /* two_digit: val = d0*10 + d1 */
      a.buf[br_2d_a] = da64_b_cond(0, (int32_t)(a.len - br_2d_a));
      da64_emit(&a, da64_movz(13, 10, 0));
      /* MADD W11, W11, W13, W12 -> 0x1B0D316B */
      da64_emit(&a, 0x1B000000u | (13u << 16) | (12u << 10) | (11u << 5) | 11u);
      da64_emit(&a, da64_add_imm(20, 20, 2));
      da64_load_u64(&a, 14, (uint64_t)(uintptr_t)g_djit_small_ints);
      /* LDR X0, [X14, X11, LSL #3] */
      da64_emit(&a, 0xF8607800u | (11u << 16) | (14u << 5) | 0u);
      size_t br_ndone1 = a.len;
      da64_emit(&a, da64_b(0));

      /* one_digit: p += 1; X0 = g_djit_small_ints[d0] */
      a.buf[br_1d_a] = da64_b_cond(0, (int32_t)(a.len - br_1d_a));
      a.buf[br_1d_b] = da64_b_cond(0, (int32_t)(a.len - br_1d_b));
      da64_emit(&a, da64_add_imm(20, 20, 1));
      da64_load_u64(&a, 14, (uint64_t)(uintptr_t)g_djit_small_ints);
      da64_emit(&a, 0xF8607800u | (11u << 16) | (14u << 5) | 0u);
      size_t br_ndone2 = a.len;
      da64_emit(&a, da64_b(0));

      /* not_dig: check '-' -> slow_num, else 'n' -> null, else any */
      a.buf[br_not_dig] = da64_b_cond(8, (int32_t)(a.len - br_not_dig));
      da64_emit(&a, da64_cmp_w_imm(10, '-'));
      size_t br_neg = a.len;
      da64_emit(&a, da64_b_cond(0, 0)); /* B.EQ slow_num */
      da64_emit(&a, da64_ldur_w(10, 20, 0));
      da64_load_u64(&a, 11, 0x6c6c756eULL); /* "null" */
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_nany = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_None);
      size_t br_ndone3 = a.len;
      da64_emit(&a, da64_b(0));
      a.buf[br_nany] = da64_b_cond(1, (int32_t)(a.len - br_nany));
      da64_emit_call_any(&a, child_slot, err_patches, &n_err);
      size_t br_ndone4 = a.len;
      da64_emit(&a, da64_b(0));

      /* slow_num: call cjson_djit_parse_num(jp, &p_tmp, end) */
      a.buf[br_zero_lead] = da64_cbz(11, (int32_t)(a.len - br_zero_lead));
      a.buf[br_not_d2] = da64_b_cond(8, (int32_t)(a.len - br_not_d2));
      a.buf[br_2d_fail] = da64_b_cond(1, (int32_t)(a.len - br_2d_fail));
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

      a.buf[br_ndone1] = da64_b((int32_t)(a.len - br_ndone1));
      a.buf[br_ndone2] = da64_b((int32_t)(a.len - br_ndone2));
      a.buf[br_ndone3] = da64_b((int32_t)(a.len - br_ndone3));
      a.buf[br_ndone4] = da64_b((int32_t)(a.len - br_ndone4));
    } else if (vk == 0) {
      /* String: direct parse_string_fast + inline null */
      da64_emit(&a, da64_ldurb(10, 20, 0));
      da64_emit(&a, da64_cmp_w_imm(10, '"'));
      size_t br_not_str = a.len;
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
      size_t br_sdone1 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_not_str] = da64_b_cond(1, (int32_t)(a.len - br_not_str));
      da64_emit(&a, da64_ldur_w(10, 20, 0));
      da64_load_u64(&a, 11, 0x6c6c756eULL); /* "null" */
      da64_emit(&a, da64_cmp_w_reg(10, 11));
      size_t br_sany = a.len;
      da64_emit(&a, da64_b_cond(1, 0));
      da64_emit(&a, da64_add_imm(20, 20, 4));
      da64_load_u64(&a, 0, (uint64_t)(uintptr_t)Py_None);
      size_t br_sdone2 = a.len;
      da64_emit(&a, da64_b(0));

      a.buf[br_sany] = da64_b_cond(1, (int32_t)(a.len - br_sany));
      da64_emit_call_any(&a, child_slot, err_patches, &n_err);
      a.buf[br_sdone1] = da64_b((int32_t)(a.len - br_sdone1));
      a.buf[br_sdone2] = da64_b((int32_t)(a.len - br_sdone2));
    } else {
      /* Dict / List / other */
      da64_emit_call_any(&a, child_slot, err_patches, &n_err);
    }

    /* Store parsed val into split_vals[j] and update X25 = j + 1 */
    da64_emit(&a, da64_str_uoff(0, 24, j * 8u));
    da64_emit(&a, da64_movz(25, (uint16_t)(j + 1u), 0));
  }

  /* Match closing '}' */
  da64_emit(&a, da64_cmp_reg(20, 21));
  bail_patches[n_bail++] = a.len;
  da64_emit(&a, da64_b_cond(10, 0)); /* B.GE bail */
  da64_emit(&a, da64_ldurb(10, 20, 0));
  da64_emit(&a, da64_cmp_w_imm(10, '}'));
  bail_patches[n_bail++] = a.len;
  da64_emit(&a, da64_b_cond(1, 0)); /* B.NE bail */
  da64_emit(&a, da64_add_imm(20, 20, 1));

  /* Materialize split-table PyDictObject and append to jp->items[] */
  da64_emit(&a, da64_mov(0, 19));
  da64_emit(&a, da64_mov(1, 22));
  da64_emit(&a, da64_movz(2, (uint16_t)n, 0));
  da64_emit(&a, da64_mov(3, 24));
  da64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_djit_finish_split_dict);
  da64_emit(&a, da64_blr(12));
  size_t br_finish_err = a.len;
  da64_emit(&a, da64_cbnz_w(0, 0)); /* CBNZ W0, ret_err */

  /* Check if next bytes are ",{" -> advance past ',' and loop! */
  da64_emit(&a, da64_add_imm(10, 20, 2));
  da64_emit(&a, da64_cmp_reg(10, 21));
  size_t br_done_end = a.len;
  da64_emit(&a, da64_b_cond(8, 0)); /* B.HI ret_ok */
  da64_emit(&a, da64_ldurh(10, 20, 0));
  da64_emit(&a, da64_movz(11, 0x7B2Cu, 0)); /* ",{" little-endian */
  da64_emit(&a, da64_cmp_w_reg(10, 11));
  size_t br_done_sep = a.len;
  da64_emit(&a, da64_b_cond(1, 0)); /* B.NE ret_ok */
  da64_emit(&a, da64_add_imm(20, 20, 1)); /* advance past ',', now at '{' */
  da64_emit(&a, da64_b((int32_t)(loop_top - a.len)));

  /* ret_ok: return X20 (current p after last completed object) */
  size_t ret_ok = a.len;
  da64_emit(&a, da64_mov(0, 20));
  size_t epilogue = a.len;
  da64_emit(&a, 0xA9400000u | (8u << 15) | (26u << 10) | (31u << 5) | 25u);
  da64_emit(&a, 0xA9400000u | (6u << 15) | (24u << 10) | (31u << 5) | 23u);
  da64_emit(&a, 0xA9400000u | (4u << 15) | (22u << 10) | (31u << 5) | 21u);
  da64_emit(&a, 0xA9400000u | (2u << 15) | (20u << 10) | (31u << 5) | 19u);
  /* LDP X29, X30, [SP], #352 -> imm7 = +44 = 0x2C */
  da64_emit(&a, 0xA8C00000u | (0x2Cu << 15) | (30u << 10) | (31u << 5) | 29u);
  da64_emit(&a, 0xD65F03C0u); /* RET */

  /* bail_label: decref split_vals[0..X25-1] and return X23 (row_start_p) */
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

  /* err_label: decref split_vals[0..X25-1], set ctx->error = 1, return NULL */
  size_t err_label = a.len;
  size_t br_no_edecref = a.len;
  da64_emit(&a, da64_cbz(25, 0));
  da64_emit(&a, da64_mov(0, 24));
  da64_emit(&a, da64_mov(1, 25));
  da64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_djit_decref_vals);
  da64_emit(&a, da64_blr(12));
  a.buf[br_no_edecref] = da64_cbz(25, (int32_t)(a.len - br_no_edecref));
  da64_emit(&a, da64_movz(10, 1, 0));
  /* STR W10, [X22, #offsetof(JitDeserCtx, error)] */
  da64_emit(&a, 0xB8000000u | (((uint32_t)offsetof(JitDeserCtx, error) & 0x1FFu) << 12) | (22u << 5) | 10u);
  size_t ret_err = a.len;
  da64_emit(&a, da64_movz(0, 0, 0));
  da64_emit(&a, da64_b((int32_t)(epilogue - a.len)));

  if (a.len >= a.cap) { pthread_mutex_unlock(&g_djit_mu); return NULL; }

  a.buf[br_done_end] = da64_b_cond(8, (int32_t)(ret_ok - br_done_end));
  a.buf[br_done_sep] = da64_b_cond(1, (int32_t)(ret_ok - br_done_sep));
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
  sc->shared_keys->dk_refcnt++; /* pin dk so its address can never be reused (zero ABA) */
  slot->dk = sc->shared_keys;
  slot->n = n;
  memcpy(slot->vkinds, vkinds, n);
  __atomic_store_n(&slot->fn, fn, __ATOMIC_RELEASE);
  g_djit_compiled++;
  pthread_mutex_unlock(&g_djit_mu);
  return fn;
}

#endif /* (__APPLE__ || __linux__) && __aarch64__ && CJSON_SPLIT_DICT_FAST */
#endif /* CJSON_JIT_DESER_ARM64_H */
