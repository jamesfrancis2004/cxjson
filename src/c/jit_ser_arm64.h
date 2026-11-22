#ifndef CJSON_JIT_SER_ARM64_H
#define CJSON_JIT_SER_ARM64_H

/* Runtime schema JIT for ARM64 (macOS Apple Silicon & Linux aarch64).
 * Supports BOTH split-table and combined-table homogeneous dict arrays
 * (up to 30 fields, nested dicts/lists, inline {}/[]/None/bool/0..99 ints,
 * hoisted row buffer guards, thread-safe W^X compilation, CXJSON_JIT=0 switch). */

#if (defined(__APPLE__) || defined(__linux__)) && defined(__aarch64__) && \
    PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED) && !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)

#define CJSON_SCHEMA_JIT 1

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum {
  JIT_VT_INT = 1,
  JIT_VT_FLOAT = 2,
  JIT_VT_STR = 3,
  JIT_VT_BOOL = 4,
  JIT_VT_NONE = 5,
  JIT_VT_DICT = 6,
  JIT_VT_LIST = 7,
  JIT_VT_ANY = 8,
};

typedef struct {
  JsonBuffer *b;
  char **safe_end_p;
  PyDictKeysObject *dk;
  const uint8_t *prefix;
} JitRunCtx;

typedef char *(*CJsonJitSplitLoopFn)(
    PyObject *const *items,   /* X0 */
    Py_ssize_t len,           /* X1 */
    Py_ssize_t *idx_io,       /* X2 */
    char *dst,                /* X3 */
    JitRunCtx *ctx            /* X4 */
);

static int g_jit_enabled_state = -1; /* -1 = uninit, 0 = disabled, 1 = enabled */

static inline int cjson_jit_is_enabled(void) {
  if (CJSON_UNLIKELY(g_jit_enabled_state < 0)) {
    const char *env = getenv("CXJSON_JIT");
    g_jit_enabled_state = (env != NULL && env[0] == '0' && env[1] == '\0') ? 0 : 1;
  }
  return g_jit_enabled_state;
}

/* Fast C helpers called by JIT code (AAPCS64 calling convention). */
__attribute__((noinline))
static char *cjson_jit_write_long(char *dst, PyObject *val) {
  return write_py_long(dst, val);
}

__attribute__((noinline))
static char *cjson_jit_write_double(double d, char *dst) {
  if (CJSON_LIKELY(!isnan(d) && !isinf(d))) {
    return write_double_fast(d, dst);
  }
  memcpy(dst, "null", 4);
  return dst + 4;
}

__attribute__((noinline))
static char *cjson_jit_grow_min(JsonBuffer *b, char *dst, char **safe_end_p, Py_ssize_t min_extra) {
  b->len = (Py_ssize_t)(dst - b->data);
  Py_ssize_t add = (min_extra > 4096) ? min_extra : 4096;
  if (json_buffer_grow(b, b->len + add) < 0) return NULL;
  *safe_end_p = b->data + b->cap - 128;
  return b->data + b->len;
}

__attribute__((noinline))
static char *cjson_jit_emit_str(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *val) {
  if (CJSON_UNLIKELY(dst > *safe_end_p)) {
    dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
    if (dst == NULL) return NULL;
  }
  return emit_str_inline(b, dst, safe_end_p, val);
}

__attribute__((noinline))
static char *cjson_jit_encode_dict(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *dval) {
  if (CJSON_UNLIKELY(dst > *safe_end_p)) {
    dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
    if (dst == NULL) return NULL;
  }
  if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (encode_fast_iter(b, dval) < 0) return NULL;
    if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
      if (json_buffer_grow(b, b->len + 256) < 0) return NULL;
    }
    *safe_end_p = b->data + b->cap - 128;
    return b->data + b->len;
  }
  return encode_dict_fast(b, dst, safe_end_p, dval);
}

__attribute__((noinline))
static char *cjson_jit_encode_array(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *dval) {
  if (CJSON_UNLIKELY(dst > *safe_end_p)) {
    dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
    if (dst == NULL) return NULL;
  }
  if (CJSON_UNLIKELY(b->depth >= CJSON_REC_SER_DEPTH)) {
    b->len = (Py_ssize_t)(dst - b->data);
    if (encode_fast_iter(b, dval) < 0) return NULL;
    if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
      if (json_buffer_grow(b, b->len + 256) < 0) return NULL;
    }
    *safe_end_p = b->data + b->cap - 128;
    return b->data + b->len;
  }
  return encode_array_fast(b, dst, safe_end_p, dval);
}

__attribute__((noinline))
static char *cjson_jit_encode_any(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *dval) {
  if (CJSON_UNLIKELY(dst > *safe_end_p)) {
    dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
    if (dst == NULL) return NULL;
  }
  PyTypeObject *vtp = Py_TYPE(dval);
  if (vtp == &PyUnicode_Type) return emit_str_inline(b, dst, safe_end_p, dval);
  if (vtp == &PyLong_Type) return write_py_long(dst, dval);
  if (vtp == &PyFloat_Type) return cjson_jit_write_double(PyFloat_AS_DOUBLE(dval), dst);
  if (dval == Py_None) { memcpy(dst, "null", 4); return dst + 4; }
  if (vtp == &PyBool_Type) {
    if (dval == Py_True) { memcpy(dst, "true", 4); return dst + 4; }
    memcpy(dst, "false", 5); return dst + 5;
  }
  if (vtp == &PyList_Type || vtp == &PyTuple_Type) {
    if (PySequence_Fast_GET_SIZE(dval) == 0) { dst[0] = '['; dst[1] = ']'; return dst + 2; }
    return cjson_jit_encode_array(b, dst, safe_end_p, dval);
  }
  if (vtp == &PyDict_Type) {
    if (PyDict_GET_SIZE(dval) == 0) { dst[0] = '{'; dst[1] = '}'; return dst + 2; }
    return cjson_jit_encode_dict(b, dst, safe_end_p, dval);
  }
  b->len = (Py_ssize_t)(dst - b->data);
  if (json_encode_slow(b, dval) < 0) return NULL;
  if (CJSON_UNLIKELY(b->len + 128 > b->cap)) {
    if (json_buffer_grow(b, b->len + 256) < 0) return NULL;
  }
  *safe_end_p = b->data + b->cap - 128;
  return b->data + b->len;
}

/* Tiny ARM64 instruction emitter */
typedef struct {
  uint32_t *buf;
  size_t len;
  size_t cap;
} Arm64Asm;

static inline void a64_emit(Arm64Asm *a, uint32_t insn) {
  if (a->len < a->cap) a->buf[a->len++] = insn;
}

static inline uint32_t a64_movz(int rd, uint16_t imm, int shift16) {
  return 0xD2800000u | ((uint32_t)shift16 << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}
static inline uint32_t a64_movk(int rd, uint16_t imm, int shift16) {
  return 0xF2800000u | ((uint32_t)shift16 << 21) | ((uint32_t)imm << 5) | (uint32_t)rd;
}
static inline void a64_load_u64(Arm64Asm *a, int rd, uint64_t val) {
  a64_emit(a, a64_movz(rd, (uint16_t)(val & 0xFFFFu), 0));
  if ((val >> 16) & 0xFFFFu) a64_emit(a, a64_movk(rd, (uint16_t)((val >> 16) & 0xFFFFu), 1));
  if ((val >> 32) & 0xFFFFu) a64_emit(a, a64_movk(rd, (uint16_t)((val >> 32) & 0xFFFFu), 2));
  if ((val >> 48) & 0xFFFFu) a64_emit(a, a64_movk(rd, (uint16_t)((val >> 48) & 0xFFFFu), 3));
}
static inline uint32_t a64_mov(int rd, int rm) {
  return 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}
static inline uint32_t a64_ldr_uoff(int rt, int rn, uint32_t byte_off) {
  return 0xF9400000u | (((byte_off >> 3) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_ldr_w_uoff(int rt, int rn, uint32_t byte_off) {
  return 0xB9400000u | (((byte_off >> 2) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_ldr_d_uoff(int dt, int rn, uint32_t byte_off) {
  return 0xFD400000u | (((byte_off >> 3) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)dt;
}
static inline uint32_t a64_ldr_idx_lsl3(int rt, int rn, int rm) {
  return 0xF8607800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_ldrh_idx_lsl1(int rt, int rn, int rm) {
  /* LDRH Wt, [Xn, Xm, LSL #1] */
  return 0x78607800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_ldur_x(int rt, int rn, int32_t simm9) {
  return 0xF8400000u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_ldurh(int rt, int rn, int32_t simm9) {
  return 0x78400000u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_ldurb(int rt, int rn, int32_t simm9) {
  return 0x38400000u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_ldrb_post(int rt, int rn, int32_t simm9) {
  return 0x38400400u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_bic_w_imm3(int rd, int rn) {
  /* AND Wd, Wn, #0xFFFFFFFC (clears low 2 interned bits: N=0, immr=30, imms=29) */
  return 0x121E7400u | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a64_subs_x_imm(int rd, int rn, uint16_t imm12) {
  return 0xF1000000u | (((uint32_t)imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a64_str_x_post(int rt, int rn, int32_t simm9) {
  return 0xF8000400u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_str_w_post(int rt, int rn, int32_t simm9) {
  return 0xB8000400u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_strh_post(int rt, int rn, int32_t simm9) {
  return 0x78000400u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_strb_post(int rt, int rn, int32_t simm9) {
  return 0x38000400u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_str_x_uoff(int rt, int rn, uint32_t byte_off) {
  return 0xF9000000u | (((byte_off >> 3) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a64_add_imm(int rd, int rn, uint16_t imm12) {
  return 0x91000000u | (((uint32_t)imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a64_add_w_imm(int rd, int rn, uint16_t imm12) {
  return 0x11000000u | (((uint32_t)imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a64_add_reg(int rd, int rn, int rm) {
  return 0x8B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a64_lslv(int rd, int rn, int rm) {
  /* LSLV Xd, Xn, Xm */
  return 0x9AC02000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a64_cmp_reg(int rn, int rm) {
  return 0xEB00001Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
}
static inline uint32_t a64_cmp_w_imm(int rn, uint16_t imm12) {
  return 0x7100001Fu | (((uint32_t)imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5);
}
static inline uint32_t a64_cmp_x_imm(int rn, uint16_t imm12) {
  return 0xF100001Fu | (((uint32_t)imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5);
}
static inline uint32_t a64_b_cond(int cond, int32_t rel_insns) {
  return 0x54000000u | (((uint32_t)rel_insns & 0x7FFFFu) << 5) | (uint32_t)cond;
}
static inline uint32_t a64_b(int32_t rel_insns) {
  return 0x14000000u | ((uint32_t)rel_insns & 0x3FFFFFFu);
}
static inline uint32_t a64_cbz(int rt, int32_t rel_insns) {
  return 0xB4000000u | (((uint32_t)rel_insns & 0x7FFFFu) << 5) | (uint32_t)rt;
}
static inline uint32_t a64_cbnz(int rt, int32_t rel_insns) {
  return 0xB5000000u | (((uint32_t)rel_insns & 0x7FFFFu) << 5) | (uint32_t)rt;
}
static inline uint32_t a64_blr(int rn) {
  return 0xD63F0000u | ((uint32_t)rn << 5);
}

static void a64_emit_literal(Arm64Asm *a, const char *s, size_t len) {
  size_t off = 0;
  while (off + 8 <= len) {
    uint64_t w;
    memcpy(&w, s + off, 8);
    a64_load_u64(a, 10, w);
    a64_emit(a, a64_str_x_post(10, 20, 8));
    off += 8;
  }
  while (off + 4 <= len) {
    uint32_t w;
    memcpy(&w, s + off, 4);
    a64_load_u64(a, 10, (uint64_t)w);
    a64_emit(a, a64_str_w_post(10, 20, 4));
    off += 4;
  }
  while (off + 2 <= len) {
    uint16_t w;
    memcpy(&w, s + off, 2);
    a64_emit(a, a64_movz(10, w, 0));
    a64_emit(a, a64_strh_post(10, 20, 2));
    off += 2;
  }
  if (off < len) {
    a64_emit(a, a64_movz(10, (uint8_t)s[off], 0));
    a64_emit(a, a64_strb_post(10, 20, 1));
  }
}

/* Emit inline call to cjson_jit_encode_any(b=X25, dst=X20, safe_end_p=X28, dval=X9) */
static void a64_emit_call_any(Arm64Asm *a, size_t *err_patches, size_t *n_err) {
  a64_emit(a, a64_mov(0, 25));
  a64_emit(a, a64_mov(1, 20));
  a64_emit(a, a64_mov(2, 28));
  a64_emit(a, a64_mov(3, 9));
  a64_load_u64(a, 12, (uint64_t)(uintptr_t)&cjson_jit_encode_any);
  a64_emit(a, a64_blr(12));
  err_patches[(*n_err)++] = a->len;
  a64_emit(a, a64_cbz(0, 0));
  a64_emit(a, a64_mov(20, 0));
  a64_emit(a, a64_ldr_uoff(21, 28, 0));
}

/* Emit hoisted buffer check: if (dst + need > safe_end) grow */
static void a64_emit_buf_check(Arm64Asm *a, uint16_t need, size_t *err_patches, size_t *n_err) {
  if (need == 0) {
    a64_emit(a, a64_cmp_reg(20, 21));
  } else {
    a64_emit(a, a64_add_imm(10, 20, need));
    a64_emit(a, a64_cmp_reg(10, 21));
  }
  size_t br_ok = a->len;
  a64_emit(a, a64_b_cond(9, 0)); /* B.LS ok */
  a64_emit(a, a64_mov(0, 25));
  a64_emit(a, a64_mov(1, 20));
  a64_emit(a, a64_mov(2, 28));
  a64_emit(a, a64_movz(3, (uint16_t)(need + 4096), 0));
  a64_load_u64(a, 12, (uint64_t)(uintptr_t)&cjson_jit_grow_min);
  a64_emit(a, a64_blr(12));
  err_patches[(*n_err)++] = a->len;
  a64_emit(a, a64_cbz(0, 0));
  a64_emit(a, a64_mov(20, 0));
  a64_emit(a, a64_ldr_uoff(21, 28, 0));
  a->buf[br_ok] = a64_b_cond(9, (int32_t)(a->len - br_ok));
}

#define CJSON_JIT_POOL_BYTES (256 * 1024)
#define CJSON_JIT_CACHE_SLOTS 64

typedef struct {
  uint64_t sig_hash;
  uint32_t n;
  uint32_t kbuf_len;
  uint8_t is_combined;
  uint8_t psize;
  uint8_t vtypes[30];
  uint8_t order[30];
  uint8_t prefix[32];
  PyObject *expected_keys[30]; /* non-NULL only when is_combined == 1 (borrowed/pinned) */
  char kbuf[1024];
  CJsonJitSplitLoopFn fn; /* published with __ATOMIC_RELEASE */
} JitSchemaEntry;

static pthread_mutex_t g_jit_mu = PTHREAD_MUTEX_INITIALIZER;
#if defined(__APPLE__)
static uint8_t *g_jit_mem = NULL;
static size_t g_jit_used = 0;
#endif
static JitSchemaEntry g_jit_cache[CJSON_JIT_CACHE_SLOTS];
static uint32_t g_jit_next_slot = 0;
static uint32_t g_jit_compiled_pages = 0;
static uint32_t g_jit_ascii_state_val = 0;
static int g_jit_ascii_state_inited = 0;

static inline uint32_t cjson_jit_get_ascii_state(void) {
  if (CJSON_UNLIKELY(!g_jit_ascii_state_inited)) {
    PyObject *probe = PyUnicode_FromString("ab");
    if (probe != NULL) {
      uint32_t st = 0;
      memcpy(&st, &((PyASCIIObject *)probe)->state, sizeof(st));
      g_jit_ascii_state_val = st & ~3u; /* mask off 2-bit interned field */
      Py_DECREF(probe);
    }
    g_jit_ascii_state_inited = 1;
  }
  return g_jit_ascii_state_val;
}

static CJsonJitSplitLoopFn cjson_jit_get_or_compile_impl(
    int is_combined, uint32_t n, const char *kbuf, uint32_t kbuf_len,
    const uint16_t *koff, const uint16_t *klen,
    const uint8_t *vtypes, const uint8_t *order,
    uint8_t psize, const uint8_t *prefix,
    PyObject *const *expected_keys) {
  if (!cjson_jit_is_enabled() || n == 0 || n > 30 || kbuf_len > 1024) return NULL;

  uint64_t h = (uint64_t)n * 0x9E3779B97F4A7C15ULL ^ ((uint64_t)kbuf_len << 32) ^
               ((uint64_t)is_combined << 48) ^ psize;
  for (uint32_t j = 0; j < n; j++) {
    uint64_t ktag = is_combined ? (uint64_t)(uintptr_t)expected_keys[j] : (uint64_t)order[j];
    h = (h ^ ((uint64_t)vtypes[j] << 8) ^ ktag) * 0xBF58476D1CE4E5B9ULL;
  }
  for (uint32_t j = 0; j < kbuf_len; j++) {
    h = (h ^ (uint8_t)kbuf[j]) * 0x100000001B3ULL;
  }

  /* Lock-free fast-path lookup */
  for (int s = 0; s < CJSON_JIT_CACHE_SLOTS; s++) {
    JitSchemaEntry *e = &g_jit_cache[s];
    CJsonJitSplitLoopFn fn = __atomic_load_n(&e->fn, __ATOMIC_ACQUIRE);
    if (fn != NULL && e->sig_hash == h && e->is_combined == (uint8_t)is_combined &&
        e->n == n && e->kbuf_len == kbuf_len && e->psize == psize &&
        memcmp(e->vtypes, vtypes, n) == 0 &&
        (is_combined ? (memcmp(e->expected_keys, expected_keys, n * sizeof(PyObject *)) == 0)
                     : (memcmp(e->order, order, n) == 0 &&
                        (psize == 0 || memcmp(e->prefix, prefix, psize) == 0))) &&
        memcmp(e->kbuf, kbuf, kbuf_len) == 0) {
      return fn;
    }
  }

  pthread_mutex_lock(&g_jit_mu);

  /* Double-check under lock */
  for (int s = 0; s < CJSON_JIT_CACHE_SLOTS; s++) {
    JitSchemaEntry *e = &g_jit_cache[s];
    if (e->fn != NULL && e->sig_hash == h && e->is_combined == (uint8_t)is_combined &&
        e->n == n && e->kbuf_len == kbuf_len && e->psize == psize &&
        memcmp(e->vtypes, vtypes, n) == 0 &&
        (is_combined ? (memcmp(e->expected_keys, expected_keys, n * sizeof(PyObject *)) == 0)
                     : (memcmp(e->order, order, n) == 0 &&
                        (psize == 0 || memcmp(e->prefix, prefix, psize) == 0))) &&
        memcmp(e->kbuf, kbuf, kbuf_len) == 0) {
      CJsonJitSplitLoopFn fn = e->fn;
      pthread_mutex_unlock(&g_jit_mu);
      return fn;
    }
  }

  if (g_jit_compiled_pages >= CJSON_JIT_CACHE_SLOTS) {
    pthread_mutex_unlock(&g_jit_mu);
    return NULL; /* bounded JIT code memory: fall back cleanly to C */
  }

#if defined(__APPLE__)
  if (g_jit_mem == NULL) {
    void *m = mmap(NULL, CJSON_JIT_POOL_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (m == MAP_FAILED) { pthread_mutex_unlock(&g_jit_mu); return NULL; }
    g_jit_mem = (uint8_t *)m;
    g_jit_used = 0;
  }
  if (g_jit_used + 8192 > CJSON_JIT_POOL_BYTES) {
    pthread_mutex_unlock(&g_jit_mu);
    return NULL;
  }
#endif

  /* Pre-compute max fixed bytes per run between variable-length fields */
  uint16_t run_max[32];
  {
    uint32_t acc = 2; /* trailing "}," */
    for (int j = (int)n - 1; j >= 0; j--) {
      uint8_t vt = vtypes[j];
      uint32_t vmax = (vt == JIT_VT_INT) ? 24u : ((vt == JIT_VT_FLOAT) ? 34u :
                      ((vt == JIT_VT_BOOL) ? 5u : ((vt == JIT_VT_NONE) ? 4u : 2u)));
      if (vt == JIT_VT_STR || vt == JIT_VT_DICT || vt == JIT_VT_LIST || vt == JIT_VT_ANY) {
        acc = (uint32_t)klen[j] + 4u;
      } else {
        acc += (uint32_t)klen[j] + vmax;
      }
      run_max[j] = (acc > 3500u) ? 3500u : (uint16_t)acc;
    }
  }

  uint32_t tmp_code[2048];
  Arm64Asm a = {tmp_code, 0, 2048};

  /* Prologue */
  a64_emit(&a, 0xA9800000u | (0x74u << 15) | (30u << 10) | (31u << 5) | 29u);
  a64_emit(&a, 0xA9000000u | (2u << 15) | (20u << 10) | (31u << 5) | 19u);
  a64_emit(&a, 0xA9000000u | (4u << 15) | (22u << 10) | (31u << 5) | 21u);
  a64_emit(&a, 0xA9000000u | (6u << 15) | (24u << 10) | (31u << 5) | 23u);
  a64_emit(&a, 0xA9000000u | (8u << 15) | (26u << 10) | (31u << 5) | 25u);
  a64_emit(&a, 0xA9000000u | (10u << 15) | (28u << 10) | (31u << 5) | 27u);

  a64_emit(&a, a64_mov(19, 0)); /* X19 = items */
  a64_emit(&a, a64_mov(24, 1)); /* X24 = len */
  a64_emit(&a, a64_mov(26, 2)); /* X26 = idx_io */
  a64_emit(&a, a64_ldr_uoff(23, 26, 0)); /* X23 = *idx_io */
  a64_emit(&a, a64_mov(20, 3)); /* X20 = dst */
  a64_emit(&a, a64_ldr_uoff(25, 4, (uint32_t)offsetof(JitRunCtx, b)));
  a64_emit(&a, a64_ldr_uoff(28, 4, (uint32_t)offsetof(JitRunCtx, safe_end_p)));
  a64_emit(&a, a64_ldr_uoff(27, 4, (uint32_t)offsetof(JitRunCtx, dk)));
  a64_emit(&a, a64_ldr_uoff(21, 28, 0)); /* X21 = *safe_end_p */

  size_t done_patches[128];
  size_t n_done = 0;
  size_t err_patches[192];
  size_t n_err = 0;

  size_t loop_top = a.len;

  /* if (i >= len) goto done */
  a64_emit(&a, a64_cmp_reg(23, 24));
  done_patches[n_done++] = a.len;
  a64_emit(&a, a64_b_cond(10, 0));

  /* X9 = items[i] */
  a64_emit(&a, a64_ldr_idx_lsl3(9, 19, 23));

  /* Check Py_TYPE(item) == &PyDict_Type */
  a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyObject, ob_type)));
  a64_load_u64(&a, 11, (uint64_t)(uintptr_t)&PyDict_Type);
  a64_emit(&a, a64_cmp_reg(10, 11));
  done_patches[n_done++] = a.len;
  a64_emit(&a, a64_b_cond(1, 0));

  /* Check mp->ma_used == n */
  a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyDictObject, ma_used)));
  a64_emit(&a, a64_cmp_x_imm(10, (uint16_t)n));
  done_patches[n_done++] = a.len;
  a64_emit(&a, a64_b_cond(1, 0));

  if (!is_combined) {
    /* Split-table check: ma_keys == split_dk && ma_values != NULL && prefix matches */
    a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyDictObject, ma_keys)));
    a64_emit(&a, a64_cmp_reg(10, 27));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(1, 0));

    a64_emit(&a, a64_ldr_uoff(22, 9, (uint32_t)offsetof(PyDictObject, ma_values)));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_cbz(22, 0));

#if PY_VERSION_HEX >= 0x030D0000
    a64_emit(&a, a64_ldurb(10, 22, 1));
    a64_emit(&a, a64_cmp_w_imm(10, (uint16_t)n));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(1, 0));
    a64_emit(&a, a64_ldurb(10, 22, 3));
    a64_emit(&a, a64_cmp_w_imm(10, 1));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(1, 0));
    a64_emit(&a, a64_add_imm(22, 22, 8));
#else
    a64_emit(&a, a64_ldurh(10, 22, -2));
    a64_load_u64(&a, 11, (uint64_t)(((uint16_t)psize << 8) | (uint16_t)n));
    a64_emit(&a, a64_cmp_reg(10, 11));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(1, 0));
    for (int32_t off = -(int32_t)psize; off < 0; ) {
      if (off + 8 <= 0) {
        uint64_t pw;
        memcpy(&pw, prefix + (psize + off), 8);
        a64_emit(&a, a64_ldur_x(10, 22, off));
        a64_load_u64(&a, 11, pw);
        a64_emit(&a, a64_cmp_reg(10, 11));
        done_patches[n_done++] = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
        off += 8;
      } else {
        a64_emit(&a, a64_ldurb(10, 22, off));
        a64_emit(&a, a64_cmp_w_imm(10, prefix[psize + off]));
        done_patches[n_done++] = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
        off += 1;
      }
    }
#endif
  } else {
    /* Combined-table check: ma_values == NULL, dk_kind == 1, dk_nentries == n, all ep[j].me_key == expected_keys[j] */
    a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyDictObject, ma_values)));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_cbnz(10, 0)); /* if ma_values != NULL -> done */

    /* X13 = mp->ma_keys (CJsonSerDictKeysObject*) */
    a64_emit(&a, a64_ldr_uoff(13, 9, (uint32_t)offsetof(PyDictObject, ma_keys)));
    a64_emit(&a, a64_ldurb(10, 13, (int32_t)offsetof(CJsonSerDictKeysObject, dk_kind)));
    a64_emit(&a, a64_cmp_w_imm(10, 1));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(1, 0));

    a64_emit(&a, a64_ldr_uoff(10, 13, (uint32_t)offsetof(CJsonSerDictKeysObject, dk_nentries)));
    a64_emit(&a, a64_cmp_x_imm(10, (uint16_t)n));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(1, 0));

    /* X22 = ep = dk->dk_indices + (1ULL << dk->dk_log2_index_bytes) */
    a64_emit(&a, a64_ldurb(10, 13, (int32_t)offsetof(CJsonSerDictKeysObject, dk_log2_index_bytes)));
    a64_emit(&a, a64_movz(11, 1, 0));
    a64_emit(&a, a64_lslv(11, 11, 10));
    a64_emit(&a, a64_add_imm(22, 13, (uint16_t)offsetof(CJsonSerDictKeysObject, dk_indices)));
    a64_emit(&a, a64_add_reg(22, 22, 11));

    /* Verify every key pointer matches expected_keys[j] (guarantees exact order & zero tombstones since ma_used == dk_nentries) */
    for (uint32_t j = 0; j < n; j++) {
      a64_emit(&a, a64_ldr_uoff(10, 22, j * 16u)); /* ep[j].me_key */
      a64_load_u64(&a, 11, (uint64_t)(uintptr_t)expected_keys[j]);
      a64_emit(&a, a64_cmp_reg(10, 11));
      done_patches[n_done++] = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
    }
  }

  /* Hoisted row buffer check for the first fixed run! */
  int compact_codegen = (n <= 12);
  uint32_t ascii_state_exp = cjson_jit_get_ascii_state();
  a64_emit_buf_check(&a, run_max[0], err_patches, &n_err);

  for (uint32_t j = 0; j < n; j++) {
    if (j > 0) {
      uint8_t prev_vt = vtypes[j - 1];
      if (prev_vt == JIT_VT_STR || prev_vt == JIT_VT_DICT ||
          prev_vt == JIT_VT_LIST || prev_vt == JIT_VT_ANY) {
        a64_emit_buf_check(&a, run_max[j], err_patches, &n_err);
      }
    }

    a64_emit_literal(&a, kbuf + koff[j], klen[j]);

    /* Load dval into X9 */
    uint32_t val_off = is_combined ? (j * 16u + 8u) : ((uint32_t)order[j] * 8u);
    a64_emit(&a, a64_ldr_uoff(9, 22, val_off));

    uint8_t vt = vtypes[j];
    if (vt == JIT_VT_NONE) {
      a64_load_u64(&a, 10, (uint64_t)(uintptr_t)Py_None);
      a64_emit(&a, a64_cmp_reg(9, 10));
      size_t br_any = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      a64_emit_literal(&a, "null", 4);
      size_t br_end = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_any] = a64_b_cond(1, (int32_t)(a.len - br_any));
      a64_emit_call_any(&a, err_patches, &n_err);
      a.buf[br_end] = a64_b((int32_t)(a.len - br_end));
    } else if (vt == JIT_VT_BOOL) {
      a64_load_u64(&a, 10, (uint64_t)(uintptr_t)Py_True);
      a64_emit(&a, a64_cmp_reg(9, 10));
      size_t br_true = a.len;
      a64_emit(&a, a64_b_cond(0, 0));
      a64_load_u64(&a, 10, (uint64_t)(uintptr_t)Py_False);
      a64_emit(&a, a64_cmp_reg(9, 10));
      size_t br_any = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      a64_emit_literal(&a, "false", 5);
      size_t br_end1 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_true] = a64_b_cond(0, (int32_t)(a.len - br_true));
      a64_emit_literal(&a, "true", 4);
      size_t br_end2 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_any] = a64_b_cond(1, (int32_t)(a.len - br_any));
      a64_emit_call_any(&a, err_patches, &n_err);
      a.buf[br_end1] = a64_b((int32_t)(a.len - br_end1));
      a.buf[br_end2] = a64_b((int32_t)(a.len - br_end2));
    } else if (vt == JIT_VT_INT) {
      a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyObject, ob_type)));
      a64_load_u64(&a, 11, (uint64_t)(uintptr_t)&PyLong_Type);
      a64_emit(&a, a64_cmp_reg(10, 11));
      size_t br_not_int = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      if (compact_codegen) {
        /* Inline fast path for 0..99 (zero function call!) */
        a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyLongObject, long_value.lv_tag)));
        a64_emit(&a, a64_cmp_x_imm(10, 1));
        size_t br_zero = a.len;
        a64_emit(&a, a64_b_cond(0, 0));
        a64_emit(&a, a64_cmp_x_imm(10, (uint16_t)(1u << _PyLong_NON_SIZE_BITS)));
        size_t br_call_long1 = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
        a64_emit(&a, a64_ldr_w_uoff(10, 9, (uint32_t)offsetof(PyLongObject, long_value.ob_digit)));
        a64_emit(&a, a64_cmp_w_imm(10, 99));
        size_t br_call_long2 = a.len;
        a64_emit(&a, a64_b_cond(8, 0));
        a64_emit(&a, a64_cmp_w_imm(10, 9));
        size_t br_2dig = a.len;
        a64_emit(&a, a64_b_cond(8, 0));
        a64_emit(&a, a64_add_w_imm(10, 10, '0'));
        a64_emit(&a, a64_strb_post(10, 20, 1));
        size_t br_end1 = a.len;
        a64_emit(&a, a64_b(0));
        a.buf[br_zero] = a64_b_cond(0, (int32_t)(a.len - br_zero));
        a64_emit(&a, a64_movz(10, '0', 0));
        a64_emit(&a, a64_strb_post(10, 20, 1));
        size_t br_end2 = a.len;
        a64_emit(&a, a64_b(0));
        a.buf[br_2dig] = a64_b_cond(8, (int32_t)(a.len - br_2dig));
        a64_load_u64(&a, 11, (uint64_t)(uintptr_t)DIGIT_PAIRS);
        a64_emit(&a, a64_ldrh_idx_lsl1(10, 11, 10));
        a64_emit(&a, a64_strh_post(10, 20, 2));
        size_t br_end3 = a.len;
        a64_emit(&a, a64_b(0));
        a.buf[br_call_long1] = a64_b_cond(1, (int32_t)(a.len - br_call_long1));
        a.buf[br_call_long2] = a64_b_cond(8, (int32_t)(a.len - br_call_long2));
        a64_emit(&a, a64_mov(0, 20));
        a64_emit(&a, a64_mov(1, 9));
        a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_write_long);
        a64_emit(&a, a64_blr(12));
        err_patches[n_err++] = a.len;
        a64_emit(&a, a64_cbz(0, 0));
        a64_emit(&a, a64_mov(20, 0));
        size_t br_end4 = a.len;
        a64_emit(&a, a64_b(0));
        a.buf[br_not_int] = a64_b_cond(1, (int32_t)(a.len - br_not_int));
        a64_load_u64(&a, 10, (uint64_t)(uintptr_t)Py_None);
        a64_emit(&a, a64_cmp_reg(9, 10));
        size_t br_any = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
        a64_emit_literal(&a, "null", 4);
        size_t br_end5 = a.len;
        a64_emit(&a, a64_b(0));
        a.buf[br_any] = a64_b_cond(1, (int32_t)(a.len - br_any));
        a64_emit_call_any(&a, err_patches, &n_err);
        a.buf[br_end1] = a64_b((int32_t)(a.len - br_end1));
        a.buf[br_end2] = a64_b((int32_t)(a.len - br_end2));
        a.buf[br_end3] = a64_b((int32_t)(a.len - br_end3));
        a.buf[br_end4] = a64_b((int32_t)(a.len - br_end4));
        a.buf[br_end5] = a64_b((int32_t)(a.len - br_end5));
      } else {
        a64_emit(&a, a64_mov(0, 20));
        a64_emit(&a, a64_mov(1, 9));
        a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_write_long);
        a64_emit(&a, a64_blr(12));
        err_patches[n_err++] = a.len;
        a64_emit(&a, a64_cbz(0, 0));
        a64_emit(&a, a64_mov(20, 0));
        size_t br_end = a.len;
        a64_emit(&a, a64_b(0));
        a.buf[br_not_int] = a64_b_cond(1, (int32_t)(a.len - br_not_int));
        a64_emit_call_any(&a, err_patches, &n_err);
        a.buf[br_end] = a64_b((int32_t)(a.len - br_end));
      }
    } else if (vt == JIT_VT_FLOAT) {
      a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyObject, ob_type)));
      a64_load_u64(&a, 11, (uint64_t)(uintptr_t)&PyFloat_Type);
      a64_emit(&a, a64_cmp_reg(10, 11));
      size_t br_not_flt = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      a64_emit(&a, a64_ldr_d_uoff(0, 9, (uint32_t)offsetof(PyFloatObject, ob_fval)));
      a64_emit(&a, a64_mov(0, 20));
      a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_write_double);
      a64_emit(&a, a64_blr(12));
      a64_emit(&a, a64_mov(20, 0));
      size_t br_end1 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_not_flt] = a64_b_cond(1, (int32_t)(a.len - br_not_flt));
      a64_load_u64(&a, 10, (uint64_t)(uintptr_t)Py_None);
      a64_emit(&a, a64_cmp_reg(9, 10));
      size_t br_any = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      a64_emit_literal(&a, "null", 4);
      size_t br_end2 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_any] = a64_b_cond(1, (int32_t)(a.len - br_any));
      a64_emit_call_any(&a, err_patches, &n_err);
      a.buf[br_end1] = a64_b((int32_t)(a.len - br_end1));
      a.buf[br_end2] = a64_b((int32_t)(a.len - br_end2));
    } else if (vt == JIT_VT_STR) {
      a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyObject, ob_type)));
      a64_load_u64(&a, 11, (uint64_t)(uintptr_t)&PyUnicode_Type);
      a64_emit(&a, a64_cmp_reg(10, 11));
      size_t br_not_str = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      if (compact_codegen && ascii_state_exp != 0) {
        /* Inline short clean ASCII string (0..16 bytes, zero BLR, 100% ASan-safe!) */
        a64_emit(&a, a64_ldr_w_uoff(10, 9, (uint32_t)offsetof(PyASCIIObject, state)));
        a64_emit(&a, a64_bic_w_imm3(10, 10));
        a64_emit(&a, a64_cmp_w_imm(10, (uint16_t)ascii_state_exp));
        size_t br_slow1 = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
        a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyASCIIObject, length)));
        a64_emit(&a, a64_cmp_x_imm(10, 16));
        size_t br_slow2 = a.len;
        a64_emit(&a, a64_b_cond(8, 0)); /* B.HI slow */
        /* Check dst <= safe_end (128B slack easily holds 16+2=18B) */
        a64_emit(&a, a64_cmp_reg(20, 21));
        size_t br_slow3 = a.len;
        a64_emit(&a, a64_b_cond(8, 0)); /* B.HI slow */
        /* Save X14 = dst; X11 = src = X9 + sizeof(PyASCIIObject) */
        a64_emit(&a, a64_mov(14, 20));
        a64_emit(&a, a64_add_imm(11, 9, (uint16_t)sizeof(PyASCIIObject)));
        a64_emit(&a, a64_movz(12, '"', 0));
        a64_emit(&a, a64_strb_post(12, 20, 1));
        size_t br_empty = a.len;
        a64_emit(&a, a64_cbz(10, 0));
        size_t str_loop = a.len;
        a64_emit(&a, a64_ldrb_post(13, 11, 1));
        a64_emit(&a, a64_cmp_w_imm(13, 0x20));
        size_t br_esc1 = a.len;
        a64_emit(&a, a64_b_cond(3, 0)); /* B.LO esc */
        a64_emit(&a, a64_cmp_w_imm(13, '"'));
        size_t br_esc2 = a.len;
        a64_emit(&a, a64_b_cond(0, 0)); /* B.EQ esc */
        a64_emit(&a, a64_cmp_w_imm(13, '\\'));
        size_t br_esc3 = a.len;
        a64_emit(&a, a64_b_cond(0, 0)); /* B.EQ esc */
        a64_emit(&a, a64_strb_post(13, 20, 1));
        a64_emit(&a, a64_subs_x_imm(10, 10, 1));
        a64_emit(&a, a64_b_cond(1, (int32_t)(str_loop - a.len))); /* B.NE str_loop */
        a.buf[br_empty] = a64_cbz(10, (int32_t)(a.len - br_empty));
        a64_emit(&a, a64_strb_post(12, 20, 1)); /* closing '"' */
        size_t br_end_fast = a.len;
        a64_emit(&a, a64_b(0));
        /* esc: restore dst = X14 and fall through to slow */
        a.buf[br_esc1] = a64_b_cond(3, (int32_t)(a.len - br_esc1));
        a.buf[br_esc2] = a64_b_cond(0, (int32_t)(a.len - br_esc2));
        a.buf[br_esc3] = a64_b_cond(0, (int32_t)(a.len - br_esc3));
        a64_emit(&a, a64_mov(20, 14));
        /* slow: */
        a.buf[br_slow1] = a64_b_cond(1, (int32_t)(a.len - br_slow1));
        a.buf[br_slow2] = a64_b_cond(8, (int32_t)(a.len - br_slow2));
        a.buf[br_slow3] = a64_b_cond(8, (int32_t)(a.len - br_slow3));
        a64_emit(&a, a64_mov(0, 25));
        a64_emit(&a, a64_mov(1, 20));
        a64_emit(&a, a64_mov(2, 28));
        a64_emit(&a, a64_mov(3, 9));
        a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_emit_str);
        a64_emit(&a, a64_blr(12));
        err_patches[n_err++] = a.len;
        a64_emit(&a, a64_cbz(0, 0));
        a64_emit(&a, a64_mov(20, 0));
        a64_emit(&a, a64_ldr_uoff(21, 28, 0));
        size_t br_end1 = a.len;
        a64_emit(&a, a64_b(0));
        a.buf[br_not_str] = a64_b_cond(1, (int32_t)(a.len - br_not_str));
        a64_load_u64(&a, 10, (uint64_t)(uintptr_t)Py_None);
        a64_emit(&a, a64_cmp_reg(9, 10));
        size_t br_any = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
        a64_emit_literal(&a, "null", 4);
        size_t br_end2 = a.len;
        a64_emit(&a, a64_b(0));
        a.buf[br_any] = a64_b_cond(1, (int32_t)(a.len - br_any));
        a64_emit_call_any(&a, err_patches, &n_err);
        a.buf[br_end_fast] = a64_b((int32_t)(a.len - br_end_fast));
        a.buf[br_end1] = a64_b((int32_t)(a.len - br_end1));
        a.buf[br_end2] = a64_b((int32_t)(a.len - br_end2));
      } else {
        a64_emit(&a, a64_mov(0, 25));
        a64_emit(&a, a64_mov(1, 20));
        a64_emit(&a, a64_mov(2, 28));
        a64_emit(&a, a64_mov(3, 9));
        a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_emit_str);
        a64_emit(&a, a64_blr(12));
        err_patches[n_err++] = a.len;
        a64_emit(&a, a64_cbz(0, 0));
        a64_emit(&a, a64_mov(20, 0));
        a64_emit(&a, a64_ldr_uoff(21, 28, 0));
        size_t br_end1 = a.len;
        a64_emit(&a, a64_b(0));
        a.buf[br_not_str] = a64_b_cond(1, (int32_t)(a.len - br_not_str));
        a64_emit_call_any(&a, err_patches, &n_err);
        a.buf[br_end1] = a64_b((int32_t)(a.len - br_end1));
      }
    } else if (vt == JIT_VT_DICT) {
      a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyObject, ob_type)));
      a64_load_u64(&a, 11, (uint64_t)(uintptr_t)&PyDict_Type);
      a64_emit(&a, a64_cmp_reg(10, 11));
      size_t br_not_dict = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyDictObject, ma_used)));
      size_t br_nonempty = a.len;
      a64_emit(&a, a64_cbnz(10, 0));
      a64_emit_literal(&a, "{}", 2);
      size_t br_end1 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_nonempty] = a64_cbnz(10, (int32_t)(a.len - br_nonempty));
      a64_emit(&a, a64_mov(0, 25));
      a64_emit(&a, a64_mov(1, 20));
      a64_emit(&a, a64_mov(2, 28));
      a64_emit(&a, a64_mov(3, 9));
      a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_encode_dict);
      a64_emit(&a, a64_blr(12));
      err_patches[n_err++] = a.len;
      a64_emit(&a, a64_cbz(0, 0));
      a64_emit(&a, a64_mov(20, 0));
      a64_emit(&a, a64_ldr_uoff(21, 28, 0));
      size_t br_end2 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_not_dict] = a64_b_cond(1, (int32_t)(a.len - br_not_dict));
      a64_load_u64(&a, 10, (uint64_t)(uintptr_t)Py_None);
      a64_emit(&a, a64_cmp_reg(9, 10));
      size_t br_any = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      a64_emit_literal(&a, "null", 4);
      size_t br_end3 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_any] = a64_b_cond(1, (int32_t)(a.len - br_any));
      a64_emit_call_any(&a, err_patches, &n_err);
      a.buf[br_end1] = a64_b((int32_t)(a.len - br_end1));
      a.buf[br_end2] = a64_b((int32_t)(a.len - br_end2));
      a.buf[br_end3] = a64_b((int32_t)(a.len - br_end3));
    } else if (vt == JIT_VT_LIST) {
      a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyObject, ob_type)));
      a64_load_u64(&a, 11, (uint64_t)(uintptr_t)&PyList_Type);
      a64_emit(&a, a64_cmp_reg(10, 11));
      size_t br_not_list = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyVarObject, ob_size)));
      size_t br_nonempty = a.len;
      a64_emit(&a, a64_cbnz(10, 0));
      a64_emit_literal(&a, "[]", 2);
      size_t br_end1 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_nonempty] = a64_cbnz(10, (int32_t)(a.len - br_nonempty));
      a64_emit(&a, a64_mov(0, 25));
      a64_emit(&a, a64_mov(1, 20));
      a64_emit(&a, a64_mov(2, 28));
      a64_emit(&a, a64_mov(3, 9));
      a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_encode_array);
      a64_emit(&a, a64_blr(12));
      err_patches[n_err++] = a.len;
      a64_emit(&a, a64_cbz(0, 0));
      a64_emit(&a, a64_mov(20, 0));
      a64_emit(&a, a64_ldr_uoff(21, 28, 0));
      size_t br_end2 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_not_list] = a64_b_cond(1, (int32_t)(a.len - br_not_list));
      a64_load_u64(&a, 10, (uint64_t)(uintptr_t)Py_None);
      a64_emit(&a, a64_cmp_reg(9, 10));
      size_t br_any = a.len;
      a64_emit(&a, a64_b_cond(1, 0));
      a64_emit_literal(&a, "null", 4);
      size_t br_end3 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_any] = a64_b_cond(1, (int32_t)(a.len - br_any));
      a64_emit_call_any(&a, err_patches, &n_err);
      a.buf[br_end1] = a64_b((int32_t)(a.len - br_end1));
      a.buf[br_end2] = a64_b((int32_t)(a.len - br_end2));
      a.buf[br_end3] = a64_b((int32_t)(a.len - br_end3));
    } else {
      a64_emit_call_any(&a, err_patches, &n_err);
    }
  }

  /* Write "}," and advance i */
  a64_emit_literal(&a, "},", 2);
  a64_emit(&a, a64_add_imm(23, 23, 1));
  a64_emit(&a, a64_b((int32_t)(loop_top - a.len)));

  /* done */
  size_t done_label = a.len;
  a64_emit(&a, a64_str_x_uoff(23, 26, 0));
  a64_emit(&a, a64_mov(0, 20));
  size_t epilogue_label = a.len;
  a64_emit(&a, 0xA9400000u | (10u << 15) | (28u << 10) | (31u << 5) | 27u);
  a64_emit(&a, 0xA9400000u | (8u << 15) | (26u << 10) | (31u << 5) | 25u);
  a64_emit(&a, 0xA9400000u | (6u << 15) | (24u << 10) | (31u << 5) | 23u);
  a64_emit(&a, 0xA9400000u | (4u << 15) | (22u << 10) | (31u << 5) | 21u);
  a64_emit(&a, 0xA9400000u | (2u << 15) | (20u << 10) | (31u << 5) | 19u);
  a64_emit(&a, 0xA8C00000u | (12u << 15) | (30u << 10) | (31u << 5) | 29u);
  a64_emit(&a, 0xD65F03C0u);

  /* err_label */
  size_t err_label = a.len;
  a64_emit(&a, a64_movz(0, 0, 0));
  a64_emit(&a, a64_b((int32_t)(epilogue_label - a.len)));

  if (a.len >= a.cap) { pthread_mutex_unlock(&g_jit_mu); return NULL; }

  for (size_t k = 0; k < n_done; k++) {
    size_t pos = done_patches[k];
    int32_t rel = (int32_t)(done_label - pos);
    uint32_t orig = a.buf[pos];
    if ((orig & 0xFF000000u) == 0xB4000000u) {
      a.buf[pos] = a64_cbz((int)(orig & 0x1Fu), rel);
    } else if ((orig & 0xFF000000u) == 0xB5000000u) {
      a.buf[pos] = a64_cbnz((int)(orig & 0x1Fu), rel);
    } else {
      a.buf[pos] = a64_b_cond((int)(orig & 0xFu), rel);
    }
  }
  for (size_t k = 0; k < n_err; k++) {
    size_t pos = err_patches[k];
    int32_t rel = (int32_t)(err_label - pos);
    a.buf[pos] = a64_cbz((int)(a.buf[pos] & 0x1Fu), rel);
  }

  size_t code_bytes = a.len * sizeof(uint32_t);
  size_t aligned = (code_bytes + 15u) & ~15u;
  uint8_t *dest = NULL;

#if defined(__APPLE__)
  if (g_jit_used + aligned > CJSON_JIT_POOL_BYTES) {
    pthread_mutex_unlock(&g_jit_mu);
    return NULL;
  }
  dest = g_jit_mem + g_jit_used;
  if (__builtin_available(macOS 11.0, *)) {
    pthread_jit_write_protect_np(0);
    memcpy(dest, tmp_code, code_bytes);
    pthread_jit_write_protect_np(1);
  } else {
    pthread_mutex_unlock(&g_jit_mu);
    return NULL;
  }
  sys_icache_invalidate(dest, code_bytes);
  g_jit_used += aligned;
#else
  /* Dedicated per-schema immutable W^X page on Linux (never mprotect an active page!) */
  size_t page_sz = (code_bytes + 4095u) & ~4095u;
  void *page = mmap(NULL, page_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) { pthread_mutex_unlock(&g_jit_mu); return NULL; }
  memcpy(page, tmp_code, code_bytes);
  if (mprotect(page, page_sz, PROT_READ | PROT_EXEC) != 0) {
    munmap(page, page_sz);
    pthread_mutex_unlock(&g_jit_mu);
    return NULL;
  }
  __builtin___clear_cache((char *)page, (char *)page + code_bytes);
  dest = (uint8_t *)page;
#endif

  CJsonJitSplitLoopFn fn = (CJsonJitSplitLoopFn)(uintptr_t)dest;
  uint32_t idx = g_jit_next_slot++;
  JitSchemaEntry *slot = &g_jit_cache[idx];
  slot->sig_hash = h;
  slot->n = n;
  slot->kbuf_len = kbuf_len;
  slot->is_combined = (uint8_t)is_combined;
  slot->psize = psize;
  memcpy(slot->vtypes, vtypes, n);
  if (is_combined) {
    for (uint32_t j = 0; j < n; j++) {
      Py_INCREF(expected_keys[j]);
      slot->expected_keys[j] = expected_keys[j];
    }
  } else {
    memcpy(slot->order, order, n);
    if (psize > 0) memcpy(slot->prefix, prefix, psize);
  }
  memcpy(slot->kbuf, kbuf, kbuf_len);
  __atomic_store_n(&slot->fn, fn, __ATOMIC_RELEASE);
  g_jit_compiled_pages++;

  pthread_mutex_unlock(&g_jit_mu);
  return fn;
}

static inline CJsonJitSplitLoopFn cjson_jit_get_or_compile(
    uint32_t n, const char *kbuf, uint32_t kbuf_len,
    const uint16_t *koff, const uint16_t *klen,
    const uint8_t *vtypes, const uint8_t *order,
    uint8_t psize, const uint8_t *prefix) {
  return cjson_jit_get_or_compile_impl(0, n, kbuf, kbuf_len, koff, klen, vtypes, order, psize, prefix, NULL);
}

static inline CJsonJitSplitLoopFn cjson_jit_get_or_compile_combined(
    uint32_t n, const char *kbuf, uint32_t kbuf_len,
    const uint16_t *koff, const uint16_t *klen,
    const uint8_t *vtypes, PyObject *const *expected_keys) {
  return cjson_jit_get_or_compile_impl(1, n, kbuf, kbuf_len, koff, klen, vtypes, NULL, 0, NULL, expected_keys);
}

#endif /* (__APPLE__ || __linux__) && __aarch64__ */
#endif /* CJSON_JIT_SER_ARM64_H */
