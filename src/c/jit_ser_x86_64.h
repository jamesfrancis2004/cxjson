#ifndef CJSON_JIT_SER_X86_64_H
#define CJSON_JIT_SER_X86_64_H

/* Runtime schema JIT for System V AMD64 (Linux & macOS x86_64).
 * Emits an unrolled native loop for homogeneous split-table dict arrays
 * (including nested dicts, nested lists, inline {}/[], and any-type fallback). */

#if (defined(__APPLE__) || defined(__linux__)) && defined(__x86_64__) && \
    PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED) && !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)

#define CJSON_SCHEMA_JIT 1

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
    PyObject *const *items,   /* RDI */
    Py_ssize_t len,           /* RSI */
    Py_ssize_t *idx_io,       /* RDX */
    char *dst,                /* RCX */
    JitRunCtx *ctx            /* R8 */
);

static int g_jit_enabled_state = -1;

static inline int cjson_jit_is_enabled(void) {
  if (CJSON_UNLIKELY(g_jit_enabled_state < 0)) {
    const char *env = getenv("CXJSON_JIT");
    g_jit_enabled_state = (env != NULL && env[0] == '0' && env[1] == '\0') ? 0 : 1;
  }
  return g_jit_enabled_state;
}

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
static char *cjson_jit_grow(JsonBuffer *b, char *dst, char **safe_end_p) {
  b->len = (Py_ssize_t)(dst - b->data);
  if (json_buffer_grow(b, b->len + 4096) < 0) return NULL;
  *safe_end_p = b->data + b->cap - 128;
  return b->data + b->len;
}

__attribute__((noinline))
static char *cjson_jit_emit_str(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *val) {
  if (CJSON_UNLIKELY(dst > *safe_end_p)) {
    dst = cjson_jit_grow(b, dst, safe_end_p);
    if (dst == NULL) return NULL;
  }
  return emit_str_inline(b, dst, safe_end_p, val);
}

__attribute__((noinline))
static char *cjson_jit_encode_dict(JsonBuffer *b, char *dst, char **safe_end_p, PyObject *dval) {
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
    dst = cjson_jit_grow(b, dst, safe_end_p);
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

/* Tiny x86_64 machine-code emitter.
 * Frame layout (after pushing RBX, RBP, R12, R13, R14, R15 + SUB RSP, 40 -> 16B aligned):
 *   R12 = items, R13 = i, R14 = dst, R15 = svals (per row),
 *   RBX = safe_end, RBP = b (JsonBuffer*)
 *   [RSP + 0]  = len
 *   [RSP + 8]  = idx_io
 *   [RSP + 16] = safe_end_p
 *   [RSP + 24] = split_dk
 *   [RSP + 32] = scratch (dval across calls if needed)
 */
typedef struct {
  uint8_t *buf;
  size_t len;
  size_t cap;
} X64Asm;

static inline void x64_b(X64Asm *a, uint8_t b) {
  if (a->len < a->cap) a->buf[a->len++] = b;
}
static inline void x64_u16(X64Asm *a, uint16_t v) {
  if (a->len + 2 <= a->cap) { memcpy(a->buf + a->len, &v, 2); a->len += 2; }
}
static inline void x64_u32(X64Asm *a, uint32_t v) {
  if (a->len + 4 <= a->cap) { memcpy(a->buf + a->len, &v, 4); a->len += 4; }
}
static inline void x64_u64(X64Asm *a, uint64_t v) {
  if (a->len + 8 <= a->cap) { memcpy(a->buf + a->len, &v, 8); a->len += 8; }
}
static inline void x64_patch32(X64Asm *a, size_t disp_off, size_t target) {
  int32_t rel = (int32_t)((int64_t)target - (int64_t)(disp_off + 4));
  memcpy(a->buf + disp_off, &rel, 4);
}

/* MOV RAX, imm64 */
static inline void x64_mov_rax_u64(X64Asm *a, uint64_t v) { x64_b(a, 0x48); x64_b(a, 0xB8); x64_u64(a, v); }
/* MOV R10, imm64 */
static inline void x64_mov_r10_u64(X64Asm *a, uint64_t v) { x64_b(a, 0x49); x64_b(a, 0xBA); x64_u64(a, v); }
/* CALL RAX */
static inline void x64_call_rax(X64Asm *a) { x64_b(a, 0xFF); x64_b(a, 0xD0); }
/* TEST RAX, RAX; JZ rel32 -> returns patch offset */
static inline size_t x64_jz_rax(X64Asm *a) {
  x64_b(a, 0x48); x64_b(a, 0x85); x64_b(a, 0xC0);
  x64_b(a, 0x0F); x64_b(a, 0x84);
  size_t p = a->len; x64_u32(a, 0); return p;
}
/* Jcc rel32 (0x84=JE, 0x85=JNE, 0x8D=JGE, 0x86=JBE, 0x87=JA) */
static inline size_t x64_jcc32(X64Asm *a, uint8_t cc) {
  x64_b(a, 0x0F); x64_b(a, cc);
  size_t p = a->len; x64_u32(a, 0); return p;
}
static inline size_t x64_jmp32(X64Asm *a) {
  x64_b(a, 0xE9);
  size_t p = a->len; x64_u32(a, 0); return p;
}

/* Emit literal bytes into [R14], advancing R14 */
static void x64_emit_literal(X64Asm *a, const char *s, size_t len) {
  size_t off = 0;
  while (off + 8 <= len) {
    uint64_t w; memcpy(&w, s + off, 8);
    x64_mov_rax_u64(a, w);
    /* MOV [R14], RAX */
    x64_b(a, 0x49); x64_b(a, 0x89); x64_b(a, 0x06);
    /* ADD R14, 8 */
    x64_b(a, 0x49); x64_b(a, 0x83); x64_b(a, 0xC6); x64_b(a, 8);
    off += 8;
  }
  while (off + 4 <= len) {
    uint32_t w; memcpy(&w, s + off, 4);
    /* MOV DWORD [R14], imm32 */
    x64_b(a, 0x41); x64_b(a, 0xC7); x64_b(a, 0x06); x64_u32(a, w);
    x64_b(a, 0x49); x64_b(a, 0x83); x64_b(a, 0xC6); x64_b(a, 4);
    off += 4;
  }
  while (off + 2 <= len) {
    uint16_t w; memcpy(&w, s + off, 2);
    /* MOV WORD [R14], imm16 */
    x64_b(a, 0x66); x64_b(a, 0x41); x64_b(a, 0xC7); x64_b(a, 0x06); x64_u16(a, w);
    x64_b(a, 0x49); x64_b(a, 0x83); x64_b(a, 0xC6); x64_b(a, 2);
    off += 2;
  }
  if (off < len) {
    /* MOV BYTE [R14], imm8 */
    x64_b(a, 0x41); x64_b(a, 0xC6); x64_b(a, 0x06); x64_b(a, (uint8_t)s[off]);
    x64_b(a, 0x49); x64_b(a, 0x83); x64_b(a, 0xC6); x64_b(a, 1);
  }
}

/* Reload RBX = *safe_end_p ([RSP+16]) */
static inline void x64_reload_safe_end(X64Asm *a) {
  /* MOV RAX, [RSP+16]; MOV RBX, [RAX] */
  x64_b(a, 0x48); x64_b(a, 0x8B); x64_b(a, 0x44); x64_b(a, 0x24); x64_b(a, 16);
  x64_b(a, 0x48); x64_b(a, 0x8B); x64_b(a, 0x18);
}

/* Emit call to fn(RDI=b(RBP), RSI=dst(R14), RDX=safe_end_p([RSP+16]), RCX=dval(R9)) */
static void x64_emit_call_4(X64Asm *a, uint64_t fn_addr, size_t *err_patches, size_t *n_err) {
  /* MOV RDI, RBP */
  x64_b(a, 0x48); x64_b(a, 0x89); x64_b(a, 0xEF);
  /* MOV RSI, R14 */
  x64_b(a, 0x4C); x64_b(a, 0x89); x64_b(a, 0xF6);
  /* MOV RDX, [RSP+16] */
  x64_b(a, 0x48); x64_b(a, 0x8B); x64_b(a, 0x54); x64_b(a, 0x24); x64_b(a, 16);
  /* MOV RCX, R9 */
  x64_b(a, 0x4C); x64_b(a, 0x89); x64_b(a, 0xC9);
  x64_mov_rax_u64(a, fn_addr);
  x64_call_rax(a);
  err_patches[(*n_err)++] = x64_jz_rax(a);
  /* MOV R14, RAX */
  x64_b(a, 0x49); x64_b(a, 0x89); x64_b(a, 0xC6);
  x64_reload_safe_end(a);
}

#define CJSON_JIT_POOL_BYTES (128 * 1024)
#define CJSON_JIT_CACHE_SLOTS 32

typedef struct {
  uint64_t sig_hash;
  uint32_t n;
  uint32_t kbuf_len;
  uint8_t psize;
  uint8_t vtypes[30];
  uint8_t order[30];
  uint8_t prefix[32];
  char kbuf[1024];
  CJsonJitSplitLoopFn fn;
} JitSchemaEntry;

static pthread_mutex_t g_jit_mu = PTHREAD_MUTEX_INITIALIZER;
static JitSchemaEntry g_jit_cache[CJSON_JIT_CACHE_SLOTS];
static uint32_t g_jit_next_slot = 0;

static CJsonJitSplitLoopFn cjson_jit_get_or_compile(
    uint32_t n, const char *kbuf, uint32_t kbuf_len,
    const uint16_t *koff, const uint16_t *klen,
    const uint8_t *vtypes, const uint8_t *order,
    uint8_t psize, const uint8_t *prefix) {
  if (!cjson_jit_is_enabled() || n == 0 || n > 30 || kbuf_len > 1024) return NULL;

  uint64_t h = (uint64_t)n * 0x9E3779B97F4A7C15ULL ^ ((uint64_t)kbuf_len << 32) ^ psize;
  for (uint32_t j = 0; j < n; j++) {
    h = (h ^ ((uint64_t)vtypes[j] << 8) ^ order[j]) * 0xBF58476D1CE4E5B9ULL;
  }
  for (uint32_t j = 0; j < kbuf_len; j++) {
    h = (h ^ (uint8_t)kbuf[j]) * 0x100000001B3ULL;
  }

  for (int s = 0; s < CJSON_JIT_CACHE_SLOTS; s++) {
    JitSchemaEntry *e = &g_jit_cache[s];
    CJsonJitSplitLoopFn fn = __atomic_load_n(&e->fn, __ATOMIC_ACQUIRE);
    if (fn != NULL && e->sig_hash == h && e->n == n && e->kbuf_len == kbuf_len &&
        e->psize == psize &&
        memcmp(e->vtypes, vtypes, n) == 0 &&
        memcmp(e->order, order, n) == 0 &&
        (psize == 0 || memcmp(e->prefix, prefix, psize) == 0) &&
        memcmp(e->kbuf, kbuf, kbuf_len) == 0) {
      return fn;
    }
  }

  pthread_mutex_lock(&g_jit_mu);
  if (g_jit_next_slot >= CJSON_JIT_CACHE_SLOTS) {
    pthread_mutex_unlock(&g_jit_mu);
    return NULL;
  }

  uint8_t tmp_code[8192];
  X64Asm a = {tmp_code, 0, sizeof(tmp_code)};

  /* Prologue: 6 pushes (48B) + 8B retaddr = 56B -> SUB RSP, 40 => 96B (16B aligned) */
  x64_b(&a, 0x53); /* push rbx */
  x64_b(&a, 0x55); /* push rbp */
  x64_b(&a, 0x41); x64_b(&a, 0x54); /* push r12 */
  x64_b(&a, 0x41); x64_b(&a, 0x55); /* push r13 */
  x64_b(&a, 0x41); x64_b(&a, 0x56); /* push r14 */
  x64_b(&a, 0x41); x64_b(&a, 0x57); /* push r15 */
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xEC); x64_b(&a, 40); /* sub rsp, 40 */

  /* R12 = RDI (items); [RSP+0] = RSI (len); [RSP+8] = RDX (idx_io); R13 = *idx_io; R14 = RCX (dst) */
  x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xFC); /* mov r12, rdi */
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0x34); x64_b(&a, 0x24); /* mov [rsp], rsi */
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0x54); x64_b(&a, 0x24); x64_b(&a, 8); /* mov [rsp+8], rdx */
  x64_b(&a, 0x4C); x64_b(&a, 0x8B); x64_b(&a, 0x2A); /* mov r13, [rdx] */
  x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xCE); /* mov r14, rcx */

  /* Load ctx (R8): RBP = ctx->b; [RSP+16] = ctx->safe_end_p; [RSP+24] = ctx->dk; RBX = *safe_end_p */
  x64_b(&a, 0x49); x64_b(&a, 0x8B); x64_b(&a, 0x68); x64_b(&a, (uint8_t)offsetof(JitRunCtx, b));
  x64_b(&a, 0x49); x64_b(&a, 0x8B); x64_b(&a, 0x40); x64_b(&a, (uint8_t)offsetof(JitRunCtx, safe_end_p));
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0x44); x64_b(&a, 0x24); x64_b(&a, 16);
  x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x18); /* mov rbx, [rax] */
  x64_b(&a, 0x49); x64_b(&a, 0x8B); x64_b(&a, 0x40); x64_b(&a, (uint8_t)offsetof(JitRunCtx, dk));
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0x44); x64_b(&a, 0x24); x64_b(&a, 24);

  size_t done_patches[128];
  size_t n_done = 0;
  size_t err_patches[192];
  size_t n_err = 0;

  size_t loop_top = a.len;

  /* CMP R13, [RSP+0]; JGE done */
  x64_b(&a, 0x4C); x64_b(&a, 0x3B); x64_b(&a, 0x2C); x64_b(&a, 0x24);
  done_patches[n_done++] = x64_jcc32(&a, 0x8D);

  /* MOV R9, [R12 + R13*8] (item) */
  x64_b(&a, 0x4F); x64_b(&a, 0x8B); x64_b(&a, 0x0C); x64_b(&a, 0xEC);

  /* Check ob_type == &PyDict_Type */
  x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyDict_Type);
  x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
  done_patches[n_done++] = x64_jcc32(&a, 0x85);

  /* Check mp->ma_keys == split_dk ([RSP+24]) */
  x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x44); x64_b(&a, 0x24); x64_b(&a, 24);
  x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_keys));
  done_patches[n_done++] = x64_jcc32(&a, 0x85);

  /* Check mp->ma_used == n */
  x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_used)); x64_b(&a, (uint8_t)n);
  done_patches[n_done++] = x64_jcc32(&a, 0x85);

  /* R15 = mp->ma_values; TEST R15, R15; JZ done */
  x64_b(&a, 0x4D); x64_b(&a, 0x8B); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_values));
  x64_b(&a, 0x4D); x64_b(&a, 0x85); x64_b(&a, 0xFF);
  done_patches[n_done++] = x64_jcc32(&a, 0x84);

#if PY_VERSION_HEX >= 0x030D0000
  /* Verify dv->size == n, dv->valid == 1, dv->capacity >= n, and
   * (uint8_t *)&dv->values[dv->capacity] matches order[0..n-1]. */
  x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, 1); x64_b(&a, (uint8_t)n);
  done_patches[n_done++] = x64_jcc32(&a, 0x85);
  x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, 3); x64_b(&a, 1);
  done_patches[n_done++] = x64_jcc32(&a, 0x85);
  /* MOVZX R10, BYTE [R15+0] (dv->capacity); CMP R10B, n; JB done */
  x64_b(&a, 0x4D); x64_b(&a, 0x0F); x64_b(&a, 0xB6); x64_b(&a, 0x17);
  x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0xFA); x64_b(&a, (uint8_t)n);
  done_patches[n_done++] = x64_jcc32(&a, 0x82); /* JB */
  /* ADD R15, 8 (R15 = dv->values); LEA R11, [R15 + R10*8] (dv_order) */
  x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0xC7); x64_b(&a, 8);
  x64_b(&a, 0x4F); x64_b(&a, 0x8D); x64_b(&a, 0x1C); x64_b(&a, 0xD7);
  for (uint32_t off = 0; off < n; ) {
    if (off + 8 <= n) {
      uint64_t ow; memcpy(&ow, order + off, 8);
      x64_mov_rax_u64(&a, ow);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x43); x64_b(&a, (uint8_t)off);
      done_patches[n_done++] = x64_jcc32(&a, 0x85);
      off += 8;
    } else {
      x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7B); x64_b(&a, (uint8_t)off); x64_b(&a, order[off]);
      done_patches[n_done++] = x64_jcc32(&a, 0x85);
      off += 1;
    }
  }
#else
  /* Verify psize <= 32, psize >= 2+n, used == n, and vraw[-2-n .. -3] == prefix[0..n-1] */
  x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, (uint8_t)-1); x64_b(&a, 32);
  done_patches[n_done++] = x64_jcc32(&a, 0x87); /* JA */
  x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, (uint8_t)-1); x64_b(&a, (uint8_t)(2u + n));
  done_patches[n_done++] = x64_jcc32(&a, 0x82); /* JB */
  x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, (uint8_t)-2); x64_b(&a, (uint8_t)n);
  done_patches[n_done++] = x64_jcc32(&a, 0x85); /* JNE */
  for (int32_t off = -(int32_t)(2u + n); off < -2; ) {
    uint32_t idx = (uint32_t)(off + (int32_t)(2u + n));
    if (off + 8 <= -2) {
      uint64_t pw; memcpy(&pw, prefix + idx, 8);
      x64_mov_rax_u64(&a, pw);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x47); x64_b(&a, (uint8_t)(int8_t)off);
      done_patches[n_done++] = x64_jcc32(&a, 0x85);
      off += 8;
    } else {
      x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, (uint8_t)(int8_t)off); x64_b(&a, prefix[idx]);
      done_patches[n_done++] = x64_jcc32(&a, 0x85);
      off += 1;
    }
  }
#endif

  for (uint32_t j = 0; j < n; j++) {
    /* CMP R14, RBX; JBE ok; grow */
    x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xDE);
    size_t br_ok = x64_jcc32(&a, 0x86);
    /* cjson_jit_grow(RBP, R14, [RSP+16]) */
    x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0xEF);
    x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF6);
    x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x54); x64_b(&a, 0x24); x64_b(&a, 16);
    x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_jit_grow);
    x64_call_rax(&a);
    err_patches[n_err++] = x64_jz_rax(&a);
    x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xC6);
    x64_reload_safe_end(&a);
    x64_patch32(&a, br_ok, a.len);

    x64_emit_literal(&a, kbuf + koff[j], klen[j]);

    /* MOV R9, [R15 + order[j]*8] */
    uint32_t soff = (uint32_t)order[j] * 8u;
    if (soff < 128) {
      x64_b(&a, 0x4D); x64_b(&a, 0x8B); x64_b(&a, 0x4F); x64_b(&a, (uint8_t)soff);
    } else {
      x64_b(&a, 0x4D); x64_b(&a, 0x8B); x64_b(&a, 0x8F); x64_u32(&a, soff);
    }

    uint8_t vt = vtypes[j];
    if (vt == JIT_VT_NONE) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC1); /* cmp r9, rax */
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "null", 4);
      size_t br_end = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end, a.len);
    } else if (vt == JIT_VT_BOOL) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_True);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC1);
      size_t br_true = x64_jcc32(&a, 0x84);
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_False);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC1);
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "false", 5);
      size_t br_end1 = x64_jmp32(&a);
      x64_patch32(&a, br_true, a.len);
      x64_emit_literal(&a, "true", 4);
      size_t br_end2 = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end1, a.len);
      x64_patch32(&a, br_end2, a.len);
    } else if (vt == JIT_VT_INT) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyLong_Type);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
      size_t br_any = x64_jcc32(&a, 0x85);
      /* cjson_jit_write_long(RDI=R14, RSI=R9) */
      x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF7);
      x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xCE);
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_jit_write_long);
      x64_call_rax(&a);
      err_patches[n_err++] = x64_jz_rax(&a);
      x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xC6);
      size_t br_end = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end, a.len);
    } else if (vt == JIT_VT_FLOAT) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyFloat_Type);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
      size_t br_any = x64_jcc32(&a, 0x85);
      /* MOVSD XMM0, [R9 + offsetof(PyFloatObject, ob_fval)]; MOV RDI, R14 */
      x64_b(&a, 0xF2); x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0x10);
      x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyFloatObject, ob_fval));
      x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF7);
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_jit_write_double);
      x64_call_rax(&a);
      x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xC6);
      size_t br_end = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end, a.len);
    } else if (vt == JIT_VT_STR) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyUnicode_Type);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_emit_str, err_patches, &n_err);
      size_t br_end = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end, a.len);
    } else if (vt == JIT_VT_DICT) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyDict_Type);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_used)); x64_b(&a, 0);
      size_t br_ne = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "{}", 2);
      size_t br_end1 = x64_jmp32(&a);
      x64_patch32(&a, br_ne, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_dict, err_patches, &n_err);
      size_t br_end2 = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end1, a.len);
      x64_patch32(&a, br_end2, a.len);
    } else if (vt == JIT_VT_LIST) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyList_Type);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyVarObject, ob_size)); x64_b(&a, 0);
      size_t br_ne = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "[]", 2);
      size_t br_end1 = x64_jmp32(&a);
      x64_patch32(&a, br_ne, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_array, err_patches, &n_err);
      size_t br_end2 = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end1, a.len);
      x64_patch32(&a, br_end2, a.len);
    } else {
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
    }
  }

  x64_emit_literal(&a, "},", 2);
  /* INC R13; JMP loop_top */
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC5);
  size_t br_loop = x64_jmp32(&a);
  x64_patch32(&a, br_loop, loop_top);

  /* done: *idx_io = R13; RAX = R14 */
  size_t done_label = a.len;
  x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x54); x64_b(&a, 0x24); x64_b(&a, 8);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0x2A); /* mov [rdx], r13 */
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF0); /* mov rax, r14 */

  size_t epilogue_label = a.len;
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xC4); x64_b(&a, 40); /* add rsp, 40 */
  x64_b(&a, 0x41); x64_b(&a, 0x5F); /* pop r15 */
  x64_b(&a, 0x41); x64_b(&a, 0x5E); /* pop r14 */
  x64_b(&a, 0x41); x64_b(&a, 0x5D); /* pop r13 */
  x64_b(&a, 0x41); x64_b(&a, 0x5C); /* pop r12 */
  x64_b(&a, 0x5D); /* pop rbp */
  x64_b(&a, 0x5B); /* pop rbx */
  x64_b(&a, 0xC3); /* ret */

  size_t err_label = a.len;
  x64_b(&a, 0x31); x64_b(&a, 0xC0); /* xor eax, eax */
  size_t br_epi = x64_jmp32(&a);
  x64_patch32(&a, br_epi, epilogue_label);

  if (a.len >= a.cap) { pthread_mutex_unlock(&g_jit_mu); return NULL; }

  for (size_t k = 0; k < n_done; k++) x64_patch32(&a, done_patches[k], done_label);
  for (size_t k = 0; k < n_err; k++) x64_patch32(&a, err_patches[k], err_label);

  /* Dedicated per-schema immutable W^X page (100% thread-safe) */
  size_t page_sz = (a.len + 4095u) & ~4095u;
  void *page = mmap(NULL, page_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) { pthread_mutex_unlock(&g_jit_mu); return NULL; }
  memcpy(page, tmp_code, a.len);
  if (mprotect(page, page_sz, PROT_READ | PROT_EXEC) != 0) {
    munmap(page, page_sz);
    pthread_mutex_unlock(&g_jit_mu);
    return NULL;
  }

  CJsonJitSplitLoopFn fn = (CJsonJitSplitLoopFn)(uintptr_t)page;
  JitSchemaEntry *slot = &g_jit_cache[g_jit_next_slot++];
  slot->sig_hash = h;
  slot->n = n;
  slot->kbuf_len = kbuf_len;
  slot->psize = psize;
  memcpy(slot->vtypes, vtypes, n);
  memcpy(slot->order, order, n);
  if (psize > 0) memcpy(slot->prefix, prefix, psize);
  memcpy(slot->kbuf, kbuf, kbuf_len);
  __atomic_store_n(&slot->fn, fn, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_jit_mu);
  return fn;
}

static inline CJsonJitSplitLoopFn cjson_jit_get_or_compile_combined(
    uint32_t n, const char *kbuf, uint32_t kbuf_len,
    const uint16_t *koff, const uint16_t *klen,
    const uint8_t *vtypes, PyObject *const *expected_keys) {
  (void)n; (void)kbuf; (void)kbuf_len; (void)koff; (void)klen; (void)vtypes; (void)expected_keys;
  return NULL;
}

#endif /* (__APPLE__ || __linux__) && __x86_64__ */
#endif /* CJSON_JIT_SER_X86_64_H */
