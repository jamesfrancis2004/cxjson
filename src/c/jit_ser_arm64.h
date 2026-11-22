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
  dst = emit_str_inline(b, dst, safe_end_p, val);
  if (CJSON_UNLIKELY(dst == NULL)) return NULL;
  if (CJSON_UNLIKELY(dst + 1024 > *safe_end_p)) {
    dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
  }
  return dst;
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
    if (CJSON_UNLIKELY(b->len + 1280 > b->cap)) {
      if (json_buffer_grow(b, b->len + 4096) < 0) return NULL;
    }
    *safe_end_p = b->data + b->cap - 128;
    return b->data + b->len;
  }
  dst = encode_dict_fast(b, dst, safe_end_p, dval);
  if (CJSON_UNLIKELY(dst == NULL)) return NULL;
  if (CJSON_UNLIKELY(dst + 1024 > *safe_end_p)) {
    dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
  }
  return dst;
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
    if (CJSON_UNLIKELY(b->len + 1280 > b->cap)) {
      if (json_buffer_grow(b, b->len + 4096) < 0) return NULL;
    }
    *safe_end_p = b->data + b->cap - 128;
    return b->data + b->len;
  }
  dst = encode_array_fast(b, dst, safe_end_p, dval);
  if (CJSON_UNLIKELY(dst == NULL)) return NULL;
  if (CJSON_UNLIKELY(dst + 1024 > *safe_end_p)) {
    dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
  }
  return dst;
}

__attribute__((noinline))
static char *cjson_jit_encode_any(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *dval) {
  if (CJSON_UNLIKELY(dst > *safe_end_p)) {
    dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
    if (dst == NULL) return NULL;
  }
  PyTypeObject *vtp = Py_TYPE(dval);
  if (vtp == &PyUnicode_Type) return cjson_jit_emit_str(b, dst, safe_end_p, dval);
  if (vtp == &PyLong_Type) {
    dst = write_py_long(dst, dval);
    if (CJSON_UNLIKELY(dst == NULL)) return NULL;
    if (CJSON_UNLIKELY(dst + 1024 > *safe_end_p)) dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
    return dst;
  }
  if (vtp == &PyFloat_Type) {
    dst = cjson_jit_write_double(PyFloat_AS_DOUBLE(dval), dst);
    if (CJSON_UNLIKELY(dst + 1024 > *safe_end_p)) dst = cjson_jit_grow_min(b, dst, safe_end_p, 4096);
    return dst;
  }
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
  if (CJSON_UNLIKELY(b->len + 1280 > b->cap)) {
    if (json_buffer_grow(b, b->len + 4096) < 0) return NULL;
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
static inline uint32_t a64_ldur_w(int rt, int rn, int32_t simm9) {
  return 0xB8400000u | (((uint32_t)simm9 & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt;
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
static inline uint32_t a64_cbnz_w(int rt, int32_t rel_insns) {
  return 0x35000000u | (((uint32_t)rel_insns & 0x7FFFFu) << 5) | (uint32_t)rt;
}
static inline uint32_t a64_tbz_w(int rt, uint32_t bit, int32_t rel_insns) {
  return 0x36000000u | ((bit & 31u) << 19) | (((uint32_t)rel_insns & 0x3FFFu) << 5) | (uint32_t)rt;
}
static inline uint32_t a64_bl(int32_t rel_insns) {
  return 0x94000000u | ((uint32_t)rel_insns & 0x3FFFFFFu);
}
static inline uint32_t a64_blr(int rn) {
  return 0xD63F0000u | ((uint32_t)rn << 5);
}
static inline uint32_t a64_udiv_w(int rd, int rn, int rm) {
  return 0x1AC00800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a64_msub_w(int rd, int rn, int rm, int ra) {
  return 0x1B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a64_cmp_w_reg(int rn, int rm) {
  return 0x6B00001Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
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
static void *g_jit_stub_str_neon = NULL;
static void *g_jit_stub_u32_long = NULL;
static JitSchemaEntry g_jit_cache[CJSON_JIT_CACHE_SLOTS];
static uint32_t g_jit_compiled_pages = 0;
static uint32_t g_jit_ascii_state_val = 0;
static int g_jit_ascii_state_inited = 0;

static inline uint32_t cjson_jit_get_ascii_state(void) {
  if (CJSON_UNLIKELY(!g_jit_ascii_state_inited)) {
    PyObject *probe = PyUnicode_FromString("ab");
    if (probe != NULL) {
      uint8_t st = 0;
      memcpy(&st, &((PyASCIIObject *)probe)->state, 1);
      g_jit_ascii_state_val = (uint32_t)(st & 0x7Cu); /* kind+compact+ascii only; ignore interned, static, and upper 24B garbage */
      Py_DECREF(probe);
    }
    g_jit_ascii_state_inited = 1;
  }
  return g_jit_ascii_state_val;
}

/* Emit a call to a shared JIT stub (uses 1-insn PC-relative BL when in range). */
static void a64_emit_call_stub(Arm64Asm *a, void *stub_addr, uint8_t *code_dest) {
  if (code_dest != NULL && stub_addr != NULL) {
    intptr_t diff = (intptr_t)stub_addr - (intptr_t)(code_dest + a->len * 4u);
    if (diff >= -(120 * 1024 * 1024) && diff <= (120 * 1024 * 1024) && (diff & 3) == 0) {
      a64_emit(a, a64_bl((int32_t)(diff >> 2)));
      return;
    }
  }
  a64_load_u64(a, 12, (uint64_t)(uintptr_t)stub_addr);
  a64_emit(a, a64_blr(12));
}

/* Compile the two shared leaf JIT stubs ONCE under g_jit_mu:
 * 1) g_jit_stub_str_neon: 16-byte NEON SIMD clean ASCII emitter (0..112B)
 *    In:  X9 = str, X20 = dst, X21 = safe_end, X25 = b, X28 = safe_end_p
 *    Out: X0 = new_dst (or NULL on OOM), X20 = new_dst, X21 = *safe_end_p
 * 2) g_jit_stub_u32_long: zero-frame inline emitter for -99,999,999..+99,999,999
 *    In:  X9 = long, X20 = dst
 *    Out: X0 = new_dst (or NULL on error), X20 = new_dst */
static void cjson_jit_init_stubs_locked(void) {
  if (g_jit_stub_str_neon != NULL) return;
  uint32_t ascii_state_exp = cjson_jit_get_ascii_state();
  if (ascii_state_exp == 0) return;

  uint32_t buf[512];
  Arm64Asm a = {buf, 0, 512};

  /* ---- Stub 1: g_jit_stub_str_neon (offset 0) ---- */
  size_t str_off = a.len;
  size_t slow_patches[16];
  size_t n_slow = 0;

  /* Check dst <= safe_end (128B slack covers len <= 112 + 2 quotes = 114B) */
  a64_emit(&a, a64_cmp_reg(20, 21));
  slow_patches[n_slow++] = a.len;
  a64_emit(&a, a64_b_cond(8, 0)); /* B.HI slow */

  /* Check compact ASCII (low byte & 0x7C: ignores interned, static, and upper 24b garbage) */
  a64_emit(&a, a64_ldurb(10, 9, (int32_t)offsetof(PyASCIIObject, state)));
  a64_emit(&a, 0x121E114Au); /* and w10, w10, #0x7c */
  a64_emit(&a, a64_cmp_w_imm(10, (uint16_t)ascii_state_exp));
  slow_patches[n_slow++] = a.len;
  a64_emit(&a, a64_b_cond(1, 0)); /* B.NE slow */

  /* X10 = length; if > 112 -> slow */
  a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyASCIIObject, length)));
  a64_emit(&a, a64_cmp_x_imm(10, 112));
  slow_patches[n_slow++] = a.len;
  a64_emit(&a, a64_b_cond(8, 0)); /* B.HI slow */

  /* if len == 0 -> write "" and return */
  size_t br_empty = a.len;
  a64_emit(&a, a64_cbz(10, 0));

  /* X11 = src = X9 + sizeof(PyASCIIObject) */
  a64_emit(&a, a64_add_imm(11, 9, (uint16_t)sizeof(PyASCIIObject)));

  /* if len < 16 -> lt16 */
  a64_emit(&a, a64_cmp_x_imm(10, 16));
  size_t br_lt16 = a.len;
  a64_emit(&a, a64_b_cond(3, 0)); /* B.LO lt16 */

  /* === 16 <= len <= 112: 16-byte NEON SIMD === */
  a64_emit(&a, a64_mov(15, 20)); /* X15 = saved_dst */
  a64_emit(&a, 0x4F01E404u); /* movi v4.16b, #0x20 */
  a64_emit(&a, 0x4F01E445u); /* movi v5.16b, #0x22 ('"') */
  a64_emit(&a, 0x4F02E786u); /* movi v6.16b, #0x5c ('\\') */
  a64_emit(&a, a64_movz(12, '"', 0));
  a64_emit(&a, a64_strb_post(12, 20, 1));
  a64_emit(&a, a64_add_reg(14, 11, 10)); /* X14 = src_end */
  a64_emit(&a, a64_subs_x_imm(13, 10, 16)); /* X13 = len - 16 */
  size_t br_final16 = a.len;
  a64_emit(&a, a64_cbz(13, 0));

  size_t loop16 = a.len;
  a64_emit(&a, 0x3CC10560u); /* ldr q0, [x11], #16 */
  a64_emit(&a, 0x6E203481u); /* cmhi v1.16b, v4.16b, v0.16b */
  a64_emit(&a, 0x6E258C02u); /* cmeq v2.16b, v0.16b, v5.16b */
  a64_emit(&a, 0x6E268C03u); /* cmeq v3.16b, v0.16b, v6.16b */
  a64_emit(&a, 0x4EA21C21u); /* orr v1.16b, v1.16b, v2.16b */
  a64_emit(&a, 0x4EA31C21u); /* orr v1.16b, v1.16b, v3.16b */
  a64_emit(&a, 0x6E30A821u); /* umaxv b1, v1.16b */
  a64_emit(&a, 0x1E26002Cu); /* fmov w12, s1 */
  size_t br_esc_l16 = a.len;
  a64_emit(&a, a64_cbnz_w(12, 0));
  a64_emit(&a, 0x3C810680u); /* str q0, [x20], #16 */
  a64_emit(&a, a64_subs_x_imm(13, 13, 16));
  a64_emit(&a, a64_b_cond(12, (int32_t)(loop16 - a.len))); /* B.GT loop16 */

  /* final16: overlapping 16B tail at [src_end - 16] */
  a.buf[br_final16] = a64_cbz(13, (int32_t)(a.len - br_final16));
  a64_emit(&a, 0x3CDF01C0u); /* ldur q0, [x14, #-16] */
  a64_emit(&a, 0x6E203481u); /* cmhi v1.16b, v4.16b, v0.16b */
  a64_emit(&a, 0x6E258C02u); /* cmeq v2.16b, v0.16b, v5.16b */
  a64_emit(&a, 0x6E268C03u); /* cmeq v3.16b, v0.16b, v6.16b */
  a64_emit(&a, 0x4EA21C21u); /* orr v1.16b, v1.16b, v2.16b */
  a64_emit(&a, 0x4EA31C21u); /* orr v1.16b, v1.16b, v3.16b */
  a64_emit(&a, 0x6E30A821u); /* umaxv b1, v1.16b */
  a64_emit(&a, 0x1E26002Cu); /* fmov w12, s1 */
  size_t br_esc_f16 = a.len;
  a64_emit(&a, a64_cbnz_w(12, 0));
  a64_emit(&a, a64_add_reg(20, 15, 10)); /* X20 = saved_dst + len */
  a64_emit(&a, a64_add_imm(20, 20, 1));  /* X20 = saved_dst + 1 + len */
  a64_emit(&a, 0x3C9F0280u); /* stur q0, [x20, #-16] */
  a64_emit(&a, a64_movz(12, '"', 0));
  a64_emit(&a, a64_strb_post(12, 20, 1));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* === lt16: 8 <= len <= 15 (two overlapping 8B loads + 1 NEON check) === */
  a.buf[br_lt16] = a64_b_cond(3, (int32_t)(a.len - br_lt16));
  a64_emit(&a, a64_cmp_x_imm(10, 8));
  size_t br_lt8 = a.len;
  a64_emit(&a, a64_b_cond(3, 0)); /* B.LO lt8 */
  a64_emit(&a, 0xFD400160u); /* ldr d0, [x11] */
  a64_emit(&a, a64_add_reg(14, 11, 10));
  a64_emit(&a, 0xFC5F81C4u); /* ldur d4, [x14, #-8] */
  a64_emit(&a, 0x6E180480u); /* ins v0.d[1], v4.d[0] */
  a64_emit(&a, 0x4F01E401u); /* movi v1.16b, #0x20 */
  a64_emit(&a, 0x4F01E442u); /* movi v2.16b, #0x22 */
  a64_emit(&a, 0x4F02E783u); /* movi v3.16b, #0x5c */
  a64_emit(&a, 0x6E203421u); /* cmhi v1.16b, v1.16b, v0.16b */
  a64_emit(&a, 0x6E228C02u); /* cmeq v2.16b, v0.16b, v2.16b */
  a64_emit(&a, 0x6E238C03u); /* cmeq v3.16b, v0.16b, v3.16b */
  a64_emit(&a, 0x4EA21C21u); /* orr v1.16b, v1.16b, v2.16b */
  a64_emit(&a, 0x4EA31C21u); /* orr v1.16b, v1.16b, v3.16b */
  a64_emit(&a, 0x6E30A821u); /* umaxv b1, v1.16b */
  a64_emit(&a, 0x1E26002Cu); /* fmov w12, s1 */
  slow_patches[n_slow++] = a.len;
  a64_emit(&a, a64_cbnz_w(12, 0));
  a64_emit(&a, a64_movz(12, '"', 0));
  a64_emit(&a, a64_strb_post(12, 20, 1));
  a64_emit(&a, 0xFD000280u); /* str d0, [x20] */
  a64_emit(&a, a64_add_reg(20, 20, 10));
  a64_emit(&a, 0xFC1F8284u); /* stur d4, [x20, #-8] */
  a64_emit(&a, a64_strb_post(12, 20, 1));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* === lt8: 4 <= len <= 7 (two overlapping 4B loads + 1 NEON 8B check) === */
  a.buf[br_lt8] = a64_b_cond(3, (int32_t)(a.len - br_lt8));
  a64_emit(&a, a64_cmp_x_imm(10, 4));
  size_t br_lt4 = a.len;
  a64_emit(&a, a64_b_cond(3, 0)); /* B.LO lt4 */
  a64_emit(&a, 0xBD400160u); /* ldr s0, [x11] */
  a64_emit(&a, a64_add_reg(14, 11, 10));
  a64_emit(&a, 0xBC5FC1C4u); /* ldur s4, [x14, #-4] */
  a64_emit(&a, 0x6E0C0480u); /* ins v0.s[1], v4.s[0] */
  a64_emit(&a, 0x4F01E401u); /* movi v1.16b, #0x20 */
  a64_emit(&a, 0x4F01E442u); /* movi v2.16b, #0x22 */
  a64_emit(&a, 0x4F02E783u); /* movi v3.16b, #0x5c */
  a64_emit(&a, 0x6E203421u); /* cmhi v1.16b, v1.16b, v0.16b */
  a64_emit(&a, 0x6E228C02u); /* cmeq v2.16b, v0.16b, v2.16b */
  a64_emit(&a, 0x6E238C03u); /* cmeq v3.16b, v0.16b, v3.16b */
  a64_emit(&a, 0x4EA21C21u); /* orr v1.16b, v1.16b, v2.16b */
  a64_emit(&a, 0x4EA31C21u); /* orr v1.16b, v1.16b, v3.16b */
  a64_emit(&a, 0x2E30A821u); /* umaxv b1, v1.8b */
  a64_emit(&a, 0x1E26002Cu); /* fmov w12, s1 */
  slow_patches[n_slow++] = a.len;
  a64_emit(&a, a64_cbnz_w(12, 0));
  a64_emit(&a, a64_movz(12, '"', 0));
  a64_emit(&a, a64_strb_post(12, 20, 1));
  a64_emit(&a, 0xBD000280u); /* str s0, [x20] */
  a64_emit(&a, a64_add_reg(20, 20, 10));
  a64_emit(&a, 0xBC1FC284u); /* stur s4, [x20, #-4] */
  a64_emit(&a, a64_strb_post(12, 20, 1));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* === lt4: 1..3 bytes === */
  a.buf[br_lt4] = a64_b_cond(3, (int32_t)(a.len - br_lt4));
  a64_emit(&a, a64_mov(15, 20));
  a64_emit(&a, a64_movz(12, '"', 0));
  a64_emit(&a, a64_strb_post(12, 20, 1));
  size_t lt4_loop = a.len;
  a64_emit(&a, a64_ldrb_post(13, 11, 1));
  a64_emit(&a, a64_cmp_w_imm(13, 0x20));
  size_t br_esc_s1 = a.len;
  a64_emit(&a, a64_b_cond(3, 0));
  a64_emit(&a, a64_cmp_w_imm(13, '"'));
  size_t br_esc_s2 = a.len;
  a64_emit(&a, a64_b_cond(0, 0));
  a64_emit(&a, a64_cmp_w_imm(13, '\\'));
  size_t br_esc_s3 = a.len;
  a64_emit(&a, a64_b_cond(0, 0));
  a64_emit(&a, a64_strb_post(13, 20, 1));
  a64_emit(&a, a64_subs_x_imm(10, 10, 1));
  a64_emit(&a, a64_b_cond(1, (int32_t)(lt4_loop - a.len)));
  a64_emit(&a, a64_strb_post(12, 20, 1));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* empty_str: write "" and return */
  a.buf[br_empty] = a64_cbz(10, (int32_t)(a.len - br_empty));
  a64_emit(&a, a64_movz(12, 0x2222u, 0));
  a64_emit(&a, a64_strh_post(12, 20, 2));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* esc_restore: restore X20 = X15 and fall through to slow */
  a.buf[br_esc_l16] = a64_cbnz_w(12, (int32_t)(a.len - br_esc_l16));
  a.buf[br_esc_f16] = a64_cbnz_w(12, (int32_t)(a.len - br_esc_f16));
  a.buf[br_esc_s1] = a64_b_cond(3, (int32_t)(a.len - br_esc_s1));
  a.buf[br_esc_s2] = a64_b_cond(0, (int32_t)(a.len - br_esc_s2));
  a.buf[br_esc_s3] = a64_b_cond(0, (int32_t)(a.len - br_esc_s3));
  a64_emit(&a, a64_mov(20, 15));

  /* slow: call cjson_jit_emit_str(b=X25, dst=X20, safe_end_p=X28, val=X9) */
  size_t slow_label = a.len;
  for (size_t k = 0; k < n_slow; k++) {
    size_t pos = slow_patches[k];
    uint32_t orig = a.buf[pos];
    int32_t rel = (int32_t)(slow_label - pos);
    if ((orig & 0xFF000000u) == 0x35000000u) {
      a.buf[pos] = a64_cbnz_w((int)(orig & 0x1Fu), rel);
    } else {
      a.buf[pos] = a64_b_cond((int)(orig & 0xFu), rel);
    }
  }
  a64_emit(&a, 0xA9BF7BFDu); /* stp x29, x30, [sp, #-16]! */
  a64_emit(&a, a64_mov(0, 25));
  a64_emit(&a, a64_mov(1, 20));
  a64_emit(&a, a64_mov(2, 28));
  a64_emit(&a, a64_mov(3, 9));
  a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_emit_str);
  a64_emit(&a, a64_blr(12));
  size_t br_oom = a.len;
  a64_emit(&a, a64_cbz(0, 0));
  a64_emit(&a, a64_mov(20, 0));
  a64_emit(&a, a64_ldr_uoff(21, 28, 0));
  a.buf[br_oom] = a64_cbz(0, (int32_t)(a.len - br_oom));
  a64_emit(&a, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* Align Stub 2 to 16 bytes */
  while ((a.len & 3u) != 0) a64_emit(&a, 0xD503201Fu); /* nop */

  /* ---- Stub 2: g_jit_stub_u32_long (-99,999,999 .. +99,999,999) ---- */
  size_t int_off = a.len;
  a64_emit(&a, a64_ldr_uoff(10, 9, (uint32_t)offsetof(PyLongObject, long_value.lv_tag)));
  a64_emit(&a, a64_cmp_x_imm(10, 1));
  size_t br_izero = a.len;
  a64_emit(&a, a64_b_cond(0, 0)); /* B.EQ is_zero */
  /* Check 1-digit positive or negative: (lv_tag & ~2) == (1 << _PyLong_NON_SIZE_BITS) */
  a64_emit(&a, 0x927EF94Bu); /* and x11, x10, #~2 */
  a64_emit(&a, a64_cmp_x_imm(11, (uint16_t)(1u << _PyLong_NON_SIZE_BITS)));
  size_t br_islow1 = a.len;
  a64_emit(&a, a64_b_cond(1, 0)); /* B.NE islow */
  a64_emit(&a, a64_ldr_w_uoff(11, 9, (uint32_t)offsetof(PyLongObject, long_value.ob_digit)));
  /* Check mag <= 99,999,999 (8 digits) */
  a64_load_u64(&a, 12, 99999999ULL);
  a64_emit(&a, a64_cmp_w_reg(11, 12));
  size_t br_islow2 = a.len;
  a64_emit(&a, a64_b_cond(8, 0)); /* B.HI islow */

  /* Emit '-' if negative (bit 1 of X10 set) */
  size_t br_pos = a.len;
  a64_emit(&a, a64_tbz_w(10, 1, 0));
  a64_emit(&a, a64_movz(12, '-', 0));
  a64_emit(&a, a64_strb_post(12, 20, 1));
  a.buf[br_pos] = a64_tbz_w(10, 1, (int32_t)(a.len - br_pos));

  /* Fast 1..2 digits (1..99) */
  a64_emit(&a, a64_cmp_w_imm(11, 99));
  size_t br_gt99 = a.len;
  a64_emit(&a, a64_b_cond(8, 0)); /* B.HI gt99 */
  a64_emit(&a, a64_cmp_w_imm(11, 9));
  size_t br_2d = a.len;
  a64_emit(&a, a64_b_cond(8, 0)); /* B.HI d2 */
  a64_emit(&a, a64_add_w_imm(11, 11, '0'));
  a64_emit(&a, a64_strb_post(11, 20, 1));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  a.buf[br_2d] = a64_b_cond(8, (int32_t)(a.len - br_2d));
  a64_load_u64(&a, 15, (uint64_t)(uintptr_t)DIGIT_PAIRS);
  a64_emit(&a, a64_ldrh_idx_lsl1(11, 15, 11));
  a64_emit(&a, a64_strh_post(11, 20, 2));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* gt99: check <= 9999 (3..4 digits) vs 10000..99999999 (5..8 digits) */
  a.buf[br_gt99] = a64_b_cond(8, (int32_t)(a.len - br_gt99));
  a64_load_u64(&a, 15, (uint64_t)(uintptr_t)DIGIT_PAIRS);
  a64_emit(&a, a64_movz(12, 9999, 0));
  a64_emit(&a, a64_cmp_w_reg(11, 12));
  size_t br_gt9999 = a.len;
  a64_emit(&a, a64_b_cond(8, 0)); /* B.HI gt9999 */

  /* 100..9999: magic multiply q = (mag * 5243) >> 19 (ZERO UDIV!), r = mag - q * 100 */
  a64_emit(&a, a64_movz(12, 5243, 0));
  a64_emit(&a, 0x1B0C7D6Du); /* mul w13, w11, w12 */
  a64_emit(&a, 0x53137DADu); /* lsr w13, w13, #19 */
  a64_emit(&a, a64_movz(12, 100, 0));
  a64_emit(&a, a64_msub_w(14, 13, 12, 11));
  a64_emit(&a, a64_cmp_w_imm(13, 9));
  size_t br_4d = a.len;
  a64_emit(&a, a64_b_cond(8, 0)); /* B.HI d4 */
  a64_emit(&a, a64_add_w_imm(13, 13, '0'));
  a64_emit(&a, a64_strb_post(13, 20, 1));
  a64_emit(&a, a64_ldrh_idx_lsl1(14, 15, 14));
  a64_emit(&a, a64_strh_post(14, 20, 2));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  a.buf[br_4d] = a64_b_cond(8, (int32_t)(a.len - br_4d));
  a64_emit(&a, a64_ldrh_idx_lsl1(13, 15, 13));
  a64_emit(&a, a64_strh_post(13, 20, 2));
  a64_emit(&a, a64_ldrh_idx_lsl1(14, 15, 14));
  a64_emit(&a, a64_strh_post(14, 20, 2));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* 10000..99999999 (5..8 digits): hi = mag/10000 (1..9999), lo = mag%10000 (0..9999) */
  a.buf[br_gt9999] = a64_b_cond(8, (int32_t)(a.len - br_gt9999));
  a64_emit(&a, a64_movz(12, 10000, 0));
  a64_emit(&a, a64_udiv_w(10, 11, 12));     /* W10 = hi (1..9999) */
  a64_emit(&a, a64_msub_w(11, 10, 12, 11)); /* W11 = lo (0..9999) */
  /* Emit hi (1..4 digits) */
  a64_emit(&a, a64_cmp_w_imm(10, 99));
  size_t br_hi34 = a.len;
  a64_emit(&a, a64_b_cond(8, 0)); /* B.HI hi34 */
  a64_emit(&a, a64_cmp_w_imm(10, 9));
  size_t br_hi2 = a.len;
  a64_emit(&a, a64_b_cond(8, 0)); /* B.HI hi2 */
  a64_emit(&a, a64_add_w_imm(10, 10, '0'));
  a64_emit(&a, a64_strb_post(10, 20, 1));
  size_t br_lo_1 = a.len;
  a64_emit(&a, a64_b(0));
  a.buf[br_hi2] = a64_b_cond(8, (int32_t)(a.len - br_hi2));
  a64_emit(&a, a64_ldrh_idx_lsl1(10, 15, 10));
  a64_emit(&a, a64_strh_post(10, 20, 2));
  size_t br_lo_2 = a.len;
  a64_emit(&a, a64_b(0));
  a.buf[br_hi34] = a64_b_cond(8, (int32_t)(a.len - br_hi34));
  a64_emit(&a, a64_movz(12, 5243, 0));
  a64_emit(&a, 0x1B0C7D4Du); /* mul w13, w10, w12 */
  a64_emit(&a, 0x53137DADu); /* lsr w13, w13, #19 */
  a64_emit(&a, a64_movz(12, 100, 0));
  a64_emit(&a, a64_msub_w(14, 13, 12, 10));
  a64_emit(&a, a64_cmp_w_imm(13, 9));
  size_t br_hi4 = a.len;
  a64_emit(&a, a64_b_cond(8, 0));
  a64_emit(&a, a64_add_w_imm(13, 13, '0'));
  a64_emit(&a, a64_strb_post(13, 20, 1));
  size_t br_hi3_tail = a.len;
  a64_emit(&a, a64_b(0));
  a.buf[br_hi4] = a64_b_cond(8, (int32_t)(a.len - br_hi4));
  a64_emit(&a, a64_ldrh_idx_lsl1(13, 15, 13));
  a64_emit(&a, a64_strh_post(13, 20, 2));
  a.buf[br_hi3_tail] = a64_b((int32_t)(a.len - br_hi3_tail));
  a64_emit(&a, a64_ldrh_idx_lsl1(14, 15, 14));
  a64_emit(&a, a64_strh_post(14, 20, 2));

  /* Emit lo (always 4 digits: lo/100 via magic 5243 and lo%100) */
  a.buf[br_lo_1] = a64_b((int32_t)(a.len - br_lo_1));
  a.buf[br_lo_2] = a64_b((int32_t)(a.len - br_lo_2));
  a64_emit(&a, a64_movz(12, 5243, 0));
  a64_emit(&a, 0x1B0C7D6Du); /* mul w13, w11, w12 */
  a64_emit(&a, 0x53137DADu); /* lsr w13, w13, #19 */
  a64_emit(&a, a64_movz(12, 100, 0));
  a64_emit(&a, a64_msub_w(14, 13, 12, 11));
  a64_emit(&a, a64_ldrh_idx_lsl1(13, 15, 13));
  a64_emit(&a, a64_strh_post(13, 20, 2));
  a64_emit(&a, a64_ldrh_idx_lsl1(14, 15, 14));
  a64_emit(&a, a64_strh_post(14, 20, 2));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* is_zero: write '0' and return */
  a.buf[br_izero] = a64_b_cond(0, (int32_t)(a.len - br_izero));
  a64_emit(&a, a64_movz(10, '0', 0));
  a64_emit(&a, a64_strb_post(10, 20, 1));
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, 0xD65F03C0u); /* ret */

  /* islow: call cjson_jit_write_long(dst=X20, val=X9) */
  a.buf[br_islow1] = a64_b_cond(1, (int32_t)(a.len - br_islow1));
  a.buf[br_islow2] = a64_b_cond(8, (int32_t)(a.len - br_islow2));
  a64_emit(&a, 0xA9BF7BFDu); /* stp x29, x30, [sp, #-16]! */
  a64_emit(&a, a64_mov(0, 20));
  a64_emit(&a, a64_mov(1, 9));
  a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_write_long);
  a64_emit(&a, a64_blr(12));
  size_t br_ierr = a.len;
  a64_emit(&a, a64_cbz(0, 0));
  a64_emit(&a, a64_mov(20, 0));
  a.buf[br_ierr] = a64_cbz(0, (int32_t)(a.len - br_ierr));
  a64_emit(&a, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
  a64_emit(&a, 0xD65F03C0u); /* ret */

  if (a.len >= a.cap) return;

  size_t code_bytes = a.len * sizeof(uint32_t);
  size_t aligned = (code_bytes + 15u) & ~15u;
  uint8_t *dest = NULL;
#if defined(__APPLE__)
  if (g_jit_mem == NULL) {
    void *m = mmap(NULL, CJSON_JIT_POOL_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (m == MAP_FAILED) return;
    g_jit_mem = (uint8_t *)m;
    g_jit_used = 0;
  }
  if (g_jit_used + aligned > CJSON_JIT_POOL_BYTES) return;
  dest = g_jit_mem + g_jit_used;
  if (__builtin_available(macOS 11.0, *)) {
    pthread_jit_write_protect_np(0);
    memcpy(dest, buf, code_bytes);
    pthread_jit_write_protect_np(1);
  } else {
    return;
  }
  sys_icache_invalidate(dest, code_bytes);
  g_jit_used += aligned;
#else
  size_t page_sz = (code_bytes + 4095u) & ~4095u;
  void *page = mmap(NULL, page_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return;
  memcpy(page, buf, code_bytes);
  if (mprotect(page, page_sz, PROT_READ | PROT_EXEC) != 0) {
    munmap(page, page_sz);
    return;
  }
  __builtin___clear_cache((char *)page, (char *)page + code_bytes);
  dest = (uint8_t *)page;
#endif
  g_jit_stub_str_neon = dest + str_off * sizeof(uint32_t);
  g_jit_stub_u32_long = dest + int_off * sizeof(uint32_t);
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

  /* Lock-free O(1) 2-way set-associative lookup */
  uint32_t set_base = (uint32_t)(h & (CJSON_JIT_CACHE_SLOTS - 2u));
  for (uint32_t w = 0; w < 2; w++) {
    JitSchemaEntry *e = &g_jit_cache[set_base + w];
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
  for (uint32_t w = 0; w < 2; w++) {
    JitSchemaEntry *e = &g_jit_cache[set_base + w];
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

  /* Find empty way in this 2-way set (never overwrite a live slot that another thread may read) */
  uint32_t target_idx = CJSON_JIT_CACHE_SLOTS;
  for (uint32_t w = 0; w < 2; w++) {
    if (g_jit_cache[set_base + w].fn == NULL) {
      target_idx = set_base + w;
      break;
    }
  }
  if (target_idx >= CJSON_JIT_CACHE_SLOTS || g_jit_compiled_pages >= CJSON_JIT_CACHE_SLOTS) {
    pthread_mutex_unlock(&g_jit_mu);
    return NULL; /* bounded JIT code memory: fall back cleanly to C */
  }

  cjson_jit_init_stubs_locked();

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
  uint8_t *planned_dest = g_jit_mem + g_jit_used;
#else
  uint8_t *planned_dest = NULL;
#endif

  /* Pre-compute max fixed bytes per run AND total single-row max bytes.
   * When total row_need <= 1024, ONE single buffer check at row entry suffices
   * for the entire row because every C fallback helper guarantees >= 1024B slack! */
  uint16_t run_max[32];
  uint32_t total_row_need = 2; /* trailing "}," */
  {
    uint32_t acc = 2;
    for (int j = (int)n - 1; j >= 0; j--) {
      uint8_t vt = vtypes[j];
      uint32_t vmax = (vt == JIT_VT_INT) ? 24u : ((vt == JIT_VT_FLOAT) ? 34u :
                      ((vt == JIT_VT_BOOL) ? 5u : ((vt == JIT_VT_NONE) ? 4u :
                      ((vt == JIT_VT_STR) ? 114u : 4u))));
      total_row_need += (uint32_t)klen[j] + vmax;
      if (vt == JIT_VT_STR || vt == JIT_VT_DICT || vt == JIT_VT_LIST || vt == JIT_VT_ANY) {
        acc = (uint32_t)klen[j] + 4u;
      } else {
        acc += (uint32_t)klen[j] + vmax;
      }
      run_max[j] = (acc > 3500u) ? 3500u : (uint16_t)acc;
    }
  }
  int single_row_check = (total_row_need <= 1024u);

  uint32_t tmp_code[3072];
  Arm64Asm a = {tmp_code, 0, 3072};

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
    /* Verify dv->size == n, dv->valid == 1, dv->capacity >= n, and
     * get_insertion_order_array(dv) = (uint8_t *)&dv->values[dv->capacity] matches order[0..n-1]. */
    a64_emit(&a, a64_ldurb(10, 22, 1)); /* dv->size */
    a64_emit(&a, a64_cmp_w_imm(10, (uint16_t)n));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(1, 0));
    a64_emit(&a, a64_ldurb(10, 22, 3)); /* dv->valid */
    a64_emit(&a, a64_cmp_w_imm(10, 1));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(1, 0));
    a64_emit(&a, a64_ldurb(12, 22, 0)); /* X12 = dv->capacity */
    a64_emit(&a, a64_cmp_w_imm(12, (uint16_t)n));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(3, 0)); /* B.LO done (capacity < n) */
    a64_emit(&a, a64_add_imm(22, 22, 8)); /* X22 = dv->values */
    /* X13 = &dv->values[capacity] = X22 + (X12 << 3) */
    a64_emit(&a, 0x8B000C00u | (12u << 16) | (22u << 5) | 13u); /* ADD X13, X22, X12, LSL #3 */
    for (uint32_t off = 0; off < n; ) {
      if (off + 8 <= n) {
        uint64_t ow;
        memcpy(&ow, order + off, 8);
        a64_emit(&a, a64_ldur_x(10, 13, (int32_t)off));
        a64_load_u64(&a, 11, ow);
        a64_emit(&a, a64_cmp_reg(10, 11));
        done_patches[n_done++] = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
        off += 8;
      } else {
        a64_emit(&a, a64_ldurb(10, 13, (int32_t)off));
        a64_emit(&a, a64_cmp_w_imm(10, order[off]));
        done_patches[n_done++] = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
        off += 1;
      }
    }
#else
    /* Verify psize <= 32, psize >= 2 + n, used == n, and vraw[-2-n .. -3] == prefix[0..n-1] */
    a64_emit(&a, a64_ldurb(10, 22, -1)); /* psize */
    a64_emit(&a, a64_cmp_w_imm(10, 32));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(8, 0)); /* B.HI done */
    a64_emit(&a, a64_cmp_w_imm(10, (uint16_t)(2u + n)));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(3, 0)); /* B.LO done */
    a64_emit(&a, a64_ldurb(10, 22, -2)); /* used */
    a64_emit(&a, a64_cmp_w_imm(10, (uint16_t)n));
    done_patches[n_done++] = a.len;
    a64_emit(&a, a64_b_cond(1, 0));
    for (int32_t off = -(int32_t)(2u + n); off < -2; ) {
      uint32_t idx = (uint32_t)(off + (int32_t)(2u + n));
      if (off + 8 <= -2) {
        uint64_t pw;
        memcpy(&pw, prefix + idx, 8);
        a64_emit(&a, a64_ldur_x(10, 22, off));
        a64_load_u64(&a, 11, pw);
        a64_emit(&a, a64_cmp_reg(10, 11));
        done_patches[n_done++] = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
        off += 8;
      } else {
        a64_emit(&a, a64_ldurb(10, 22, off));
        a64_emit(&a, a64_cmp_w_imm(10, prefix[idx]));
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

    /* Verify every key matches expected_keys[j]:
     * Fast path: 1-cycle pointer equality (ep[j].me_key == expected_keys[j]).
     * Fallback (for non-interned keys from DB drivers / separate json.loads() calls):
     *   inline compact-ASCII + length + compile-time byte comparison for keys <= 16B.
     * (dk_kind == 1 guarantees every me_key is exact PyUnicode_Type.) */
    uint32_t ascii_st = cjson_jit_get_ascii_state();
    for (uint32_t j = 0; j < n; j++) {
      a64_emit(&a, a64_ldr_uoff(10, 22, j * 16u)); /* X10 = ep[j].me_key */
      a64_load_u64(&a, 11, (uint64_t)(uintptr_t)expected_keys[j]);
      a64_emit(&a, a64_cmp_reg(10, 11));
      uint32_t raw_klen = (klen[j] >= 4u) ? (uint32_t)(klen[j] - 4u) : 0u;
      if (ascii_st != 0 && raw_klen >= 1u && raw_klen <= 16u) {
        const char *rk = kbuf + koff[j] + 2u;
        size_t br_key_ok = a.len;
        a64_emit(&a, a64_b_cond(0, 0)); /* B.EQ key_ok */
        /* Inline byte-equality fallback */
        a64_emit(&a, a64_ldurb(11, 10, (int32_t)offsetof(PyASCIIObject, state)));
        a64_emit(&a, 0x121E116Bu); /* and w11, w11, #0x7c */
        a64_emit(&a, a64_cmp_w_imm(11, (uint16_t)ascii_st));
        done_patches[n_done++] = a.len;
        a64_emit(&a, a64_b_cond(1, 0)); /* B.NE done */
        a64_emit(&a, a64_ldr_uoff(11, 10, (uint32_t)offsetof(PyASCIIObject, length)));
        a64_emit(&a, a64_cmp_x_imm(11, (uint16_t)raw_klen));
        done_patches[n_done++] = a.len;
        a64_emit(&a, a64_b_cond(1, 0)); /* B.NE done */
        int32_t dbase = (int32_t)sizeof(PyASCIIObject);
        if (raw_klen >= 8u) {
          uint64_t w0, w1;
          memcpy(&w0, rk, 8);
          a64_emit(&a, a64_ldur_x(11, 10, dbase));
          a64_load_u64(&a, 12, w0);
          a64_emit(&a, a64_cmp_reg(11, 12));
          done_patches[n_done++] = a.len;
          a64_emit(&a, a64_b_cond(1, 0));
          if (raw_klen > 8u) {
            memcpy(&w1, rk + raw_klen - 8u, 8);
            a64_emit(&a, a64_ldur_x(11, 10, dbase + (int32_t)(raw_klen - 8u)));
            a64_load_u64(&a, 12, w1);
            a64_emit(&a, a64_cmp_reg(11, 12));
            done_patches[n_done++] = a.len;
            a64_emit(&a, a64_b_cond(1, 0));
          }
        } else if (raw_klen >= 4u) {
          uint32_t w0, w1;
          memcpy(&w0, rk, 4);
          a64_emit(&a, a64_ldur_w(11, 10, dbase));
          a64_load_u64(&a, 12, (uint64_t)w0);
          a64_emit(&a, a64_cmp_w_reg(11, 12));
          done_patches[n_done++] = a.len;
          a64_emit(&a, a64_b_cond(1, 0));
          if (raw_klen > 4u) {
            memcpy(&w1, rk + raw_klen - 4u, 4);
            a64_emit(&a, a64_ldur_w(11, 10, dbase + (int32_t)(raw_klen - 4u)));
            a64_load_u64(&a, 12, (uint64_t)w1);
            a64_emit(&a, a64_cmp_w_reg(11, 12));
            done_patches[n_done++] = a.len;
            a64_emit(&a, a64_b_cond(1, 0));
          }
        } else {
          if (raw_klen >= 2u) {
            uint16_t w0;
            memcpy(&w0, rk, 2);
            a64_emit(&a, a64_ldurh(11, 10, dbase));
            a64_emit(&a, a64_movz(12, w0, 0));
            a64_emit(&a, a64_cmp_w_reg(11, 12));
            done_patches[n_done++] = a.len;
            a64_emit(&a, a64_b_cond(1, 0));
          }
          if (raw_klen & 1u) {
            uint32_t boff = raw_klen - 1u;
            a64_emit(&a, a64_ldurb(11, 10, dbase + (int32_t)boff));
            a64_emit(&a, a64_cmp_w_imm(11, (uint8_t)rk[boff]));
            done_patches[n_done++] = a.len;
            a64_emit(&a, a64_b_cond(1, 0));
          }
        }
        a.buf[br_key_ok] = a64_b_cond(0, (int32_t)(a.len - br_key_ok));
      } else {
        done_patches[n_done++] = a.len;
        a64_emit(&a, a64_b_cond(1, 0));
      }
    }
  }

  /* Hoisted row buffer check! */
  a64_emit_buf_check(&a, single_row_check ? (uint16_t)total_row_need : run_max[0], err_patches, &n_err);

  for (uint32_t j = 0; j < n; j++) {
    if (!single_row_check && j > 0) {
      uint8_t prev_vt = vtypes[j - 1];
      if (prev_vt == JIT_VT_DICT || prev_vt == JIT_VT_LIST || prev_vt == JIT_VT_ANY ||
          prev_vt == JIT_VT_STR) {
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
      if (g_jit_stub_u32_long != NULL) {
        /* 1-instruction BL to zero-frame inline -99,999,999..+99,999,999 stub! */
        a64_emit_call_stub(&a, g_jit_stub_u32_long, planned_dest);
        err_patches[n_err++] = a.len;
        a64_emit(&a, a64_cbz(0, 0));
      } else {
        a64_emit(&a, a64_mov(0, 20));
        a64_emit(&a, a64_mov(1, 9));
        a64_load_u64(&a, 12, (uint64_t)(uintptr_t)&cjson_jit_write_long);
        a64_emit(&a, a64_blr(12));
        err_patches[n_err++] = a.len;
        a64_emit(&a, a64_cbz(0, 0));
        a64_emit(&a, a64_mov(20, 0));
      }
      size_t br_end1 = a.len;
      a64_emit(&a, a64_b(0));
      a.buf[br_not_int] = a64_b_cond(1, (int32_t)(a.len - br_not_int));
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
      if (g_jit_stub_str_neon != NULL) {
        /* 1-instruction BL to 16-byte NEON SIMD clean ASCII string stub (0..112B)! */
        a64_emit_call_stub(&a, g_jit_stub_str_neon, planned_dest);
        err_patches[n_err++] = a.len;
        a64_emit(&a, a64_cbz(0, 0));
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
      }
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
      a.buf[br_end1] = a64_b((int32_t)(a.len - br_end1));
      a.buf[br_end2] = a64_b((int32_t)(a.len - br_end2));
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
  JitSchemaEntry *slot = &g_jit_cache[target_idx];
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
