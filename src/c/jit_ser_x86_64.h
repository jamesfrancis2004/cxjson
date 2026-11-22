#ifndef CJSON_JIT_SER_X86_64_H
#define CJSON_JIT_SER_X86_64_H

/* Runtime schema JIT for System V AMD64 (Linux & macOS x86_64).
 * Full parity with ARM64 JIT:
 *   - Split-table (3.12 & 3.13/3.14) AND combined-table homogeneous dict arrays
 *   - Inline key-bytes fallback for non-interned keys (1..16B) on combined dicts
 *   - Shared SSE2 16-byte clean ASCII string stub (0..112B, zero stack frame)
 *   - Shared zero-frame +-99,999,999 integer stub
 *   - Single-row hoisted buffer checks + 1024B post-slack guarantee
 *   - O(1) 2-way set-associative schema cache + dedicated immutable W^X pages */

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

/* Tiny x86_64 machine-code emitter.
 * Frame layout (after pushing RBX, RBP, R12, R13, R14, R15 + SUB RSP, 40 -> 16B aligned):
 *   R12 = items, R13 = i, R14 = dst, R15 = svals or ep (per row),
 *   RBX = safe_end, RBP = b (JsonBuffer*)
 *   [RSP + 0]  = len
 *   [RSP + 8]  = idx_io
 *   [RSP + 16] = safe_end_p
 *   [RSP + 24] = split_dk
 *   [RSP + 32] = scratch
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

static inline void x64_mov_rax_u64(X64Asm *a, uint64_t v) { x64_b(a, 0x48); x64_b(a, 0xB8); x64_u64(a, v); }
static inline void x64_mov_r10_u64(X64Asm *a, uint64_t v) { x64_b(a, 0x49); x64_b(a, 0xBA); x64_u64(a, v); }
static inline void x64_call_rax(X64Asm *a) { x64_b(a, 0xFF); x64_b(a, 0xD0); }
static inline size_t x64_jz_rax(X64Asm *a) {
  x64_b(a, 0x48); x64_b(a, 0x85); x64_b(a, 0xC0);
  x64_b(a, 0x0F); x64_b(a, 0x84);
  size_t p = a->len; x64_u32(a, 0); return p;
}
/* Jcc rel32 (0x82=JB, 0x84=JE, 0x85=JNE, 0x86=JBE, 0x87=JA, 0x8D=JGE, 0x8F=JG) */
static inline size_t x64_jcc32(X64Asm *a, uint8_t cc) {
  x64_b(a, 0x0F); x64_b(a, cc);
  size_t p = a->len; x64_u32(a, 0); return p;
}
static inline size_t x64_jmp32(X64Asm *a) {
  x64_b(a, 0xE9);
  size_t p = a->len; x64_u32(a, 0); return p;
}

static void x64_emit_literal(X64Asm *a, const char *s, size_t len) {
  size_t off = 0;
  while (off + 8 <= len) {
    uint64_t w; memcpy(&w, s + off, 8);
    x64_mov_rax_u64(a, w);
    x64_b(a, 0x49); x64_b(a, 0x89); x64_b(a, 0x06); /* mov [r14], rax */
    x64_b(a, 0x49); x64_b(a, 0x83); x64_b(a, 0xC6); x64_b(a, 8);
    off += 8;
  }
  while (off + 4 <= len) {
    uint32_t w; memcpy(&w, s + off, 4);
    x64_b(a, 0x41); x64_b(a, 0xC7); x64_b(a, 0x06); x64_u32(a, w);
    x64_b(a, 0x49); x64_b(a, 0x83); x64_b(a, 0xC6); x64_b(a, 4);
    off += 4;
  }
  while (off + 2 <= len) {
    uint16_t w; memcpy(&w, s + off, 2);
    x64_b(a, 0x66); x64_b(a, 0x41); x64_b(a, 0xC7); x64_b(a, 0x06); x64_u16(a, w);
    x64_b(a, 0x49); x64_b(a, 0x83); x64_b(a, 0xC6); x64_b(a, 2);
    off += 2;
  }
  if (off < len) {
    x64_b(a, 0x41); x64_b(a, 0xC6); x64_b(a, 0x06); x64_b(a, (uint8_t)s[off]);
    x64_b(a, 0x49); x64_b(a, 0x83); x64_b(a, 0xC6); x64_b(a, 1);
  }
}

static inline void x64_reload_safe_end(X64Asm *a) {
  x64_b(a, 0x48); x64_b(a, 0x8B); x64_b(a, 0x44); x64_b(a, 0x24); x64_b(a, 16);
  x64_b(a, 0x48); x64_b(a, 0x8B); x64_b(a, 0x18);
}

static void x64_emit_call_4(X64Asm *a, uint64_t fn_addr, size_t *err_patches, size_t *n_err) {
  x64_b(a, 0x48); x64_b(a, 0x89); x64_b(a, 0xEF); /* mov rdi, rbp */
  x64_b(a, 0x4C); x64_b(a, 0x89); x64_b(a, 0xF6); /* mov rsi, r14 */
  x64_b(a, 0x48); x64_b(a, 0x8B); x64_b(a, 0x54); x64_b(a, 0x24); x64_b(a, 16); /* mov rdx, [rsp+16] */
  x64_b(a, 0x4C); x64_b(a, 0x89); x64_b(a, 0xC9); /* mov rcx, r9 */
  x64_mov_rax_u64(a, fn_addr);
  x64_call_rax(a);
  err_patches[(*n_err)++] = x64_jz_rax(a);
  x64_b(a, 0x49); x64_b(a, 0x89); x64_b(a, 0xC6); /* mov r14, rax */
  x64_reload_safe_end(a);
}

/* Emit hoisted buffer check: if (R14 + need > RBX) grow */
static void x64_emit_buf_check(X64Asm *a, uint32_t need, size_t *err_patches, size_t *n_err) {
  if (need == 0) {
    x64_b(a, 0x49); x64_b(a, 0x39); x64_b(a, 0xDE); /* cmp r14, rbx */
  } else {
    /* lea rax, [r14 + need]; cmp rax, rbx */
    x64_b(a, 0x49); x64_b(a, 0x8D); x64_b(a, 0x86); x64_u32(a, need);
    x64_b(a, 0x48); x64_b(a, 0x39); x64_b(a, 0xD8);
  }
  size_t br_ok = x64_jcc32(a, 0x86); /* jbe ok */
  x64_b(a, 0x48); x64_b(a, 0x89); x64_b(a, 0xEF); /* mov rdi, rbp */
  x64_b(a, 0x4C); x64_b(a, 0x89); x64_b(a, 0xF6); /* mov rsi, r14 */
  x64_b(a, 0x48); x64_b(a, 0x8B); x64_b(a, 0x54); x64_b(a, 0x24); x64_b(a, 16); /* mov rdx, [rsp+16] */
  /* mov ecx, need + 4096 */
  x64_b(a, 0xB9); x64_u32(a, need + 4096u);
  x64_mov_rax_u64(a, (uint64_t)(uintptr_t)&cjson_jit_grow_min);
  x64_call_rax(a);
  err_patches[(*n_err)++] = x64_jz_rax(a);
  x64_b(a, 0x49); x64_b(a, 0x89); x64_b(a, 0xC6);
  x64_reload_safe_end(a);
  x64_patch32(a, br_ok, a->len);
}

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
  PyObject *expected_keys[30];
  char kbuf[1024];
  CJsonJitSplitLoopFn fn;
} JitSchemaEntry;

static pthread_mutex_t g_jit_mu = PTHREAD_MUTEX_INITIALIZER;
static JitSchemaEntry g_jit_cache[CJSON_JIT_CACHE_SLOTS];
static uint32_t g_jit_compiled_pages = 0;
static uint32_t g_jit_ascii_state_val = 0;
static int g_jit_ascii_state_inited = 0;
static void *g_jit_stub_str_sse2 = NULL;
static void *g_jit_stub_u32_long = NULL;

static inline uint32_t cjson_jit_get_ascii_state(void) {
  if (CJSON_UNLIKELY(!g_jit_ascii_state_inited)) {
    PyObject *probe = PyUnicode_FromString("ab");
    if (probe != NULL) {
      uint32_t st = 0;
      memcpy(&st, &((PyASCIIObject *)probe)->state, sizeof(st));
      g_jit_ascii_state_val = st & ~3u;
      Py_DECREF(probe);
    }
    g_jit_ascii_state_inited = 1;
  }
  return g_jit_ascii_state_val;
}

/* Shared SSE2 + u32 stubs compiled ONCE into an immutable W^X page:
 * 1) g_jit_stub_str_sse2:
 *    In:  R9 = str, R14 = dst, RBX = safe_end, RBP = b, [RSP+24] = safe_end_p (caller's [RSP+16])
 *    Out: RAX = new_dst (or NULL on OOM), R14 = new_dst, RBX = *safe_end_p
 * 2) g_jit_stub_u32_long:
 *    In:  R9 = long, R14 = dst
 *    Out: RAX = new_dst (or NULL on error), R14 = new_dst */
static void cjson_jit_init_stubs_locked(void) {
  if (g_jit_stub_str_sse2 != NULL) return;
  uint32_t ascii_st = cjson_jit_get_ascii_state();
  if (ascii_st == 0) return;

  static const uint8_t sse_consts[48] __attribute__((aligned(16))) = {
      0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
      '"','"','"','"','"','"','"','"','"','"','"','"','"','"','"','"',
      '\\','\\','\\','\\','\\','\\','\\','\\','\\','\\','\\','\\','\\','\\','\\','\\',
  };

  uint8_t buf[2048];
  X64Asm a = {buf, 0, sizeof(buf)};

  /* ---- Stub 1: g_jit_stub_str_sse2 ---- */
  size_t str_off = a.len;
  size_t slow_patches[16];
  size_t n_slow = 0;

  /* cmp r14, rbx; ja slow */
  x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xDE);
  slow_patches[n_slow++] = x64_jcc32(&a, 0x87);

  /* mov eax, [r9 + state]; and eax, -4; cmp eax, ascii_st; jne slow */
  x64_b(&a, 0x41); x64_b(&a, 0x8B); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyASCIIObject, state));
  x64_b(&a, 0x83); x64_b(&a, 0xE0); x64_b(&a, 0xFC);
  x64_b(&a, 0x3D); x64_u32(&a, ascii_st);
  slow_patches[n_slow++] = x64_jcc32(&a, 0x85);

  /* mov rcx, [r9 + length]; cmp rcx, 112; ja slow */
  x64_b(&a, 0x49); x64_b(&a, 0x8B); x64_b(&a, 0x49); x64_b(&a, (uint8_t)offsetof(PyASCIIObject, length));
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xF9); x64_b(&a, 112);
  slow_patches[n_slow++] = x64_jcc32(&a, 0x87);

  /* test rcx, rcx; jz empty */
  x64_b(&a, 0x48); x64_b(&a, 0x85); x64_b(&a, 0xC9);
  size_t br_empty = x64_jcc32(&a, 0x84);

  /* lea rsi, [r9 + sizeof(PyASCIIObject)] */
  x64_b(&a, 0x49); x64_b(&a, 0x8D); x64_b(&a, 0x71); x64_b(&a, (uint8_t)sizeof(PyASCIIObject));

  /* cmp rcx, 16; jb lt16 */
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xF9); x64_b(&a, 16);
  size_t br_lt16 = x64_jcc32(&a, 0x82);

  /* === 16 <= len <= 112: 16-byte SSE2 === */
  /* r11 = saved_dst (r14); r8 = src_end = rsi + rcx */
  x64_b(&a, 0x4D); x64_b(&a, 0x89); x64_b(&a, 0xF3); /* mov r11, r14 */
  x64_b(&a, 0x4C); x64_b(&a, 0x8D); x64_b(&a, 0x04); x64_b(&a, 0x0E); /* lea r8, [rsi + rcx] */
  x64_mov_r10_u64(&a, (uint64_t)(uintptr_t)sse_consts);
  /* movdqa xmm3, [r10+0] (0x20); xmm4, [r10+16] ('"'); xmm5, [r10+32] ('\\') */
  x64_b(&a, 0x66); x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0x1A);
  x64_b(&a, 0x66); x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0x62); x64_b(&a, 16);
  x64_b(&a, 0x66); x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0x6A); x64_b(&a, 32);

  /* Write opening '"' */
  x64_b(&a, 0x41); x64_b(&a, 0xC6); x64_b(&a, 0x06); x64_b(&a, '"');
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC6); /* inc r14 */

  /* rdx = rcx - 16; jz final16 */
  x64_b(&a, 0x48); x64_b(&a, 0x8D); x64_b(&a, 0x51); x64_b(&a, (uint8_t)-16); /* lea rdx, [rcx-16] */
  x64_b(&a, 0x48); x64_b(&a, 0x85); x64_b(&a, 0xD2);
  size_t br_final16 = x64_jcc32(&a, 0x84);

  size_t loop16 = a.len;
  x64_b(&a, 0xF3); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0x06); /* movdqu xmm0, [rsi] */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0xCB); /* movdqa xmm1, xmm3 */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x64); x64_b(&a, 0xC8); /* pcmpgtb xmm1, xmm0 */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0xD0); /* movdqa xmm2, xmm0 */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x74); x64_b(&a, 0xD4); /* pcmpeqb xmm2, xmm4 */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0xEB); x64_b(&a, 0xCA); /* por xmm1, xmm2 */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0xD0); /* movdqa xmm2, xmm0 */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x74); x64_b(&a, 0xD5); /* pcmpeqb xmm2, xmm5 */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0xEB); x64_b(&a, 0xCA); /* por xmm1, xmm2 */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0xD7); x64_b(&a, 0xC1); /* pmovmskb eax, xmm1 */
  x64_b(&a, 0x85); x64_b(&a, 0xC0); /* test eax, eax */
  size_t br_esc_l16 = x64_jcc32(&a, 0x85);
  x64_b(&a, 0xF3); x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0x7F); x64_b(&a, 0x06); /* movdqu [r14], xmm0 */
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xC6); x64_b(&a, 16); /* add rsi, 16 */
  x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0xC6); x64_b(&a, 16); /* add r14, 16 */
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xEA); x64_b(&a, 16); /* sub rdx, 16 */
  size_t br_l16 = x64_jcc32(&a, 0x8F); /* jg loop16 */
  x64_patch32(&a, br_l16, loop16);

  /* final16: overlapping 16B tail at [r8 - 16] */
  x64_patch32(&a, br_final16, a.len);
  x64_b(&a, 0xF3); x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0x40); x64_b(&a, (uint8_t)-16); /* movdqu xmm0, [r8-16] */
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0xCB);
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x64); x64_b(&a, 0xC8);
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0xD0);
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x74); x64_b(&a, 0xD4);
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0xEB); x64_b(&a, 0xCA);
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x6F); x64_b(&a, 0xD0);
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0x74); x64_b(&a, 0xD5);
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0xEB); x64_b(&a, 0xCA);
  x64_b(&a, 0x66); x64_b(&a, 0x0F); x64_b(&a, 0xD7); x64_b(&a, 0xC1);
  x64_b(&a, 0x85); x64_b(&a, 0xC0);
  size_t br_esc_f16 = x64_jcc32(&a, 0x85);
  /* r14 = r11 + rcx + 1; movdqu [r14 - 16], xmm0; mov byte [r14], '"'; inc r14 */
  x64_b(&a, 0x4D); x64_b(&a, 0x8D); x64_b(&a, 0x74); x64_b(&a, 0x0B); x64_b(&a, 1); /* lea r14, [r11 + rcx + 1] */
  x64_b(&a, 0xF3); x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0x7F); x64_b(&a, 0x46); x64_b(&a, (uint8_t)-16); /* movdqu [r14-16], xmm0 */
  x64_b(&a, 0x41); x64_b(&a, 0xC6); x64_b(&a, 0x06); x64_b(&a, '"');
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC6);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF0); /* mov rax, r14 */
  x64_b(&a, 0xC3); /* ret */

  /* === lt16: 1..15 bytes === */
  x64_patch32(&a, br_lt16, a.len);
  x64_b(&a, 0x4D); x64_b(&a, 0x89); x64_b(&a, 0xF3); /* mov r11, r14 */
  x64_b(&a, 0x41); x64_b(&a, 0xC6); x64_b(&a, 0x06); x64_b(&a, '"');
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC6);
  size_t sloop = a.len;
  x64_b(&a, 0x0F); x64_b(&a, 0xB6); x64_b(&a, 0x06); /* movzx eax, byte [rsi] */
  x64_b(&a, 0x3C); x64_b(&a, 0x20); /* cmp al, 0x20 */
  size_t br_esc_s1 = x64_jcc32(&a, 0x82); /* jb esc */
  x64_b(&a, 0x3C); x64_b(&a, '"');
  size_t br_esc_s2 = x64_jcc32(&a, 0x84);
  x64_b(&a, 0x3C); x64_b(&a, '\\');
  size_t br_esc_s3 = x64_jcc32(&a, 0x84);
  x64_b(&a, 0x41); x64_b(&a, 0x88); x64_b(&a, 0x06); /* mov [r14], al */
  x64_b(&a, 0x48); x64_b(&a, 0xFF); x64_b(&a, 0xC6); /* inc rsi */
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC6); /* inc r14 */
  x64_b(&a, 0x48); x64_b(&a, 0xFF); x64_b(&a, 0xC9); /* dec rcx */
  size_t br_sl = x64_jcc32(&a, 0x85);
  x64_patch32(&a, br_sl, sloop);
  x64_b(&a, 0x41); x64_b(&a, 0xC6); x64_b(&a, 0x06); x64_b(&a, '"');
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC6);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF0);
  x64_b(&a, 0xC3);

  /* empty: write "" */
  x64_patch32(&a, br_empty, a.len);
  x64_b(&a, 0x66); x64_b(&a, 0x41); x64_b(&a, 0xC7); x64_b(&a, 0x06); x64_u16(&a, 0x2222u);
  x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0xC6); x64_b(&a, 2);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF0);
  x64_b(&a, 0xC3);

  /* esc_restore: r14 = r11 */
  x64_patch32(&a, br_esc_l16, a.len);
  x64_patch32(&a, br_esc_f16, a.len);
  x64_patch32(&a, br_esc_s1, a.len);
  x64_patch32(&a, br_esc_s2, a.len);
  x64_patch32(&a, br_esc_s3, a.len);
  x64_b(&a, 0x4D); x64_b(&a, 0x89); x64_b(&a, 0xDE); /* mov r14, r11 */

  /* slow: RSP is 8 mod 16 on entry -> sub rsp, 8 (now 16B aligned, caller's [RSP+16] is at [RSP+32]) */
  for (size_t k = 0; k < n_slow; k++) x64_patch32(&a, slow_patches[k], a.len);
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xEC); x64_b(&a, 8);
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0xEF); /* mov rdi, rbp */
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF6); /* mov rsi, r14 */
  x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x54); x64_b(&a, 0x24); x64_b(&a, 32); /* mov rdx, [rsp+32] */
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xC9); /* mov rcx, r9 */
  x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_jit_emit_str);
  x64_call_rax(&a);
  size_t br_oom = x64_jz_rax(&a);
  x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xC6); /* mov r14, rax */
  x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x54); x64_b(&a, 0x24); x64_b(&a, 32);
  x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x1A); /* mov rbx, [rdx] */
  x64_patch32(&a, br_oom, a.len);
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xC4); x64_b(&a, 8);
  x64_b(&a, 0xC3);

  while ((a.len & 15u) != 0) x64_b(&a, 0x90);

  /* ---- Stub 2: g_jit_stub_u32_long (-9999 .. +9999 fast path, fallback to cjson_jit_write_long) ---- */
  size_t int_off = a.len;
  /* mov rcx, [r9 + lv_tag]; cmp rcx, 1; je izero */
  x64_b(&a, 0x49); x64_b(&a, 0x8B); x64_b(&a, 0x49); x64_b(&a, (uint8_t)offsetof(PyLongObject, long_value.lv_tag));
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xF9); x64_b(&a, 1);
  size_t br_izero = x64_jcc32(&a, 0x84);
  /* mov rax, rcx; and rax, -3 (~2); cmp rax, (1<<NON_SIZE_BITS); jne islow */
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0xC8);
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xE0); x64_b(&a, (uint8_t)-3);
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xF8); x64_b(&a, (uint8_t)(1u << _PyLong_NON_SIZE_BITS));
  size_t br_islow1 = x64_jcc32(&a, 0x85);
  /* mov eax, [r9 + ob_digit]; cmp eax, 9999; ja islow */
  x64_b(&a, 0x41); x64_b(&a, 0x8B); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyLongObject, long_value.ob_digit));
  x64_b(&a, 0x3D); x64_u32(&a, 9999u);
  size_t br_islow2 = x64_jcc32(&a, 0x87);
  /* test cl, 2; jz pos; emit '-' */
  x64_b(&a, 0xF6); x64_b(&a, 0xC1); x64_b(&a, 2);
  size_t br_pos = x64_jcc32(&a, 0x84);
  x64_b(&a, 0x41); x64_b(&a, 0xC6); x64_b(&a, 0x06); x64_b(&a, '-');
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC6);
  x64_patch32(&a, br_pos, a.len);
  /* cmp eax, 99; ja d34 */
  x64_b(&a, 0x83); x64_b(&a, 0xF8); x64_b(&a, 99);
  size_t br_d34 = x64_jcc32(&a, 0x87);
  /* cmp eax, 9; ja d2 */
  x64_b(&a, 0x83); x64_b(&a, 0xF8); x64_b(&a, 9);
  size_t br_d2 = x64_jcc32(&a, 0x87);
  x64_b(&a, 0x04); x64_b(&a, '0'); /* add al, '0' */
  x64_b(&a, 0x41); x64_b(&a, 0x88); x64_b(&a, 0x06);
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC6);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF0);
  x64_b(&a, 0xC3);
  /* d2 */
  x64_patch32(&a, br_d2, a.len);
  x64_mov_r10_u64(&a, (uint64_t)(uintptr_t)DIGIT_PAIRS);
  x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0xB7); x64_b(&a, 0x04); x64_b(&a, 0x42); /* movzx eax, word [r10+rax*2] */
  x64_b(&a, 0x66); x64_b(&a, 0x41); x64_b(&a, 0x89); x64_b(&a, 0x06);
  x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0xC6); x64_b(&a, 2);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF0);
  x64_b(&a, 0xC3);
  /* d34: q = eax / 100, r = edx */
  x64_patch32(&a, br_d34, a.len);
  x64_b(&a, 0x31); x64_b(&a, 0xD2); /* xor edx, edx */
  x64_b(&a, 0xB9); x64_u32(&a, 100u); /* mov ecx, 100 */
  x64_b(&a, 0xF7); x64_b(&a, 0xF1); /* div ecx -> eax=q, edx=r */
  x64_mov_r10_u64(&a, (uint64_t)(uintptr_t)DIGIT_PAIRS);
  x64_b(&a, 0x83); x64_b(&a, 0xF8); x64_b(&a, 9);
  size_t br_d4 = x64_jcc32(&a, 0x87);
  x64_b(&a, 0x04); x64_b(&a, '0');
  x64_b(&a, 0x41); x64_b(&a, 0x88); x64_b(&a, 0x06);
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC6);
  size_t br_emit_r = x64_jmp32(&a);
  x64_patch32(&a, br_d4, a.len);
  x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0xB7); x64_b(&a, 0x04); x64_b(&a, 0x42);
  x64_b(&a, 0x66); x64_b(&a, 0x41); x64_b(&a, 0x89); x64_b(&a, 0x06);
  x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0xC6); x64_b(&a, 2);
  x64_patch32(&a, br_emit_r, a.len);
  /* emit r (edx) as 2 digits */
  x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0xB7); x64_b(&a, 0x04); x64_b(&a, 0x52); /* movzx eax, word [r10+rdx*2] */
  x64_b(&a, 0x66); x64_b(&a, 0x41); x64_b(&a, 0x89); x64_b(&a, 0x06);
  x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0xC6); x64_b(&a, 2);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF0);
  x64_b(&a, 0xC3);
  /* izero */
  x64_patch32(&a, br_izero, a.len);
  x64_b(&a, 0x41); x64_b(&a, 0xC6); x64_b(&a, 0x06); x64_b(&a, '0');
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC6);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF0);
  x64_b(&a, 0xC3);
  /* islow: sub rsp, 8; cjson_jit_write_long(r14, r9); add rsp, 8; ret */
  x64_patch32(&a, br_islow1, a.len);
  x64_patch32(&a, br_islow2, a.len);
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xEC); x64_b(&a, 8);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF7); /* mov rdi, r14 */
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xCE); /* mov rsi, r9 */
  x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_jit_write_long);
  x64_call_rax(&a);
  size_t br_ierr = x64_jz_rax(&a);
  x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xC6);
  x64_patch32(&a, br_ierr, a.len);
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xC4); x64_b(&a, 8);
  x64_b(&a, 0xC3);

  if (a.len >= a.cap) return;
  size_t page_sz = (a.len + 4095u) & ~4095u;
  void *page = mmap(NULL, page_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return;
  memcpy(page, buf, a.len);
  if (mprotect(page, page_sz, PROT_READ | PROT_EXEC) != 0) {
    munmap(page, page_sz);
    return;
  }
  g_jit_stub_str_sse2 = (uint8_t *)page + str_off;
  g_jit_stub_u32_long = (uint8_t *)page + int_off;
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

  uint32_t target_idx = CJSON_JIT_CACHE_SLOTS;
  for (uint32_t w = 0; w < 2; w++) {
    if (g_jit_cache[set_base + w].fn == NULL) {
      target_idx = set_base + w;
      break;
    }
  }
  if (target_idx >= CJSON_JIT_CACHE_SLOTS || g_jit_compiled_pages >= CJSON_JIT_CACHE_SLOTS) {
    pthread_mutex_unlock(&g_jit_mu);
    return NULL;
  }

  cjson_jit_init_stubs_locked();

  uint16_t run_max[32];
  uint32_t total_row_need = 2;
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

  uint8_t tmp_code[12288];
  X64Asm a = {tmp_code, 0, sizeof(tmp_code)};

  /* Prologue */
  x64_b(&a, 0x53); /* push rbx */
  x64_b(&a, 0x55); /* push rbp */
  x64_b(&a, 0x41); x64_b(&a, 0x54); /* push r12 */
  x64_b(&a, 0x41); x64_b(&a, 0x55); /* push r13 */
  x64_b(&a, 0x41); x64_b(&a, 0x56); /* push r14 */
  x64_b(&a, 0x41); x64_b(&a, 0x57); /* push r15 */
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xEC); x64_b(&a, 40);

  x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xFC); /* mov r12, rdi */
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0x34); x64_b(&a, 0x24); /* mov [rsp], rsi */
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0x54); x64_b(&a, 0x24); x64_b(&a, 8); /* mov [rsp+8], rdx */
  x64_b(&a, 0x4C); x64_b(&a, 0x8B); x64_b(&a, 0x2A); /* mov r13, [rdx] */
  x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xCE); /* mov r14, rcx */

  x64_b(&a, 0x49); x64_b(&a, 0x8B); x64_b(&a, 0x68); x64_b(&a, (uint8_t)offsetof(JitRunCtx, b));
  x64_b(&a, 0x49); x64_b(&a, 0x8B); x64_b(&a, 0x40); x64_b(&a, (uint8_t)offsetof(JitRunCtx, safe_end_p));
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0x44); x64_b(&a, 0x24); x64_b(&a, 16);
  x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x18); /* mov rbx, [rax] */
  x64_b(&a, 0x49); x64_b(&a, 0x8B); x64_b(&a, 0x40); x64_b(&a, (uint8_t)offsetof(JitRunCtx, dk));
  x64_b(&a, 0x48); x64_b(&a, 0x89); x64_b(&a, 0x44); x64_b(&a, 0x24); x64_b(&a, 24);

  size_t done_patches[256];
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

  /* Check mp->ma_used == n */
  x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_used)); x64_b(&a, (uint8_t)n);
  done_patches[n_done++] = x64_jcc32(&a, 0x85);

  if (!is_combined) {
    /* Split-table check */
    x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x44); x64_b(&a, 0x24); x64_b(&a, 24);
    x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_keys));
    done_patches[n_done++] = x64_jcc32(&a, 0x85);

    x64_b(&a, 0x4D); x64_b(&a, 0x8B); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_values));
    x64_b(&a, 0x4D); x64_b(&a, 0x85); x64_b(&a, 0xFF);
    done_patches[n_done++] = x64_jcc32(&a, 0x84);

#if PY_VERSION_HEX >= 0x030D0000
    x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, 1); x64_b(&a, (uint8_t)n);
    done_patches[n_done++] = x64_jcc32(&a, 0x85);
    x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, 3); x64_b(&a, 1);
    done_patches[n_done++] = x64_jcc32(&a, 0x85);
    x64_b(&a, 0x4D); x64_b(&a, 0x0F); x64_b(&a, 0xB6); x64_b(&a, 0x17);
    x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0xFA); x64_b(&a, (uint8_t)n);
    done_patches[n_done++] = x64_jcc32(&a, 0x82);
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
    x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, (uint8_t)-1); x64_b(&a, 32);
    done_patches[n_done++] = x64_jcc32(&a, 0x87);
    x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, (uint8_t)-1); x64_b(&a, (uint8_t)(2u + n));
    done_patches[n_done++] = x64_jcc32(&a, 0x82);
    x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, (uint8_t)-2); x64_b(&a, (uint8_t)n);
    done_patches[n_done++] = x64_jcc32(&a, 0x85);
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
  } else {
    /* Combined-table check: ma_values == NULL, dk_kind == 1, dk_nentries == n */
    x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_values)); x64_b(&a, 0);
    done_patches[n_done++] = x64_jcc32(&a, 0x85);

    /* R15 = mp->ma_keys */
    x64_b(&a, 0x4D); x64_b(&a, 0x8B); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_keys));
    x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7F); x64_b(&a, (uint8_t)offsetof(CJsonSerDictKeysObject, dk_kind)); x64_b(&a, 1);
    done_patches[n_done++] = x64_jcc32(&a, 0x85);
    x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0x7F); x64_b(&a, (uint8_t)offsetof(CJsonSerDictKeysObject, dk_nentries)); x64_b(&a, (uint8_t)n);
    done_patches[n_done++] = x64_jcc32(&a, 0x85);

    /* R15 = ep = dk->dk_indices + (1ULL << dk->dk_log2_index_bytes) */
    x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0xB6); x64_b(&a, 0x4F); x64_b(&a, (uint8_t)offsetof(CJsonSerDictKeysObject, dk_log2_index_bytes));
    x64_b(&a, 0xB8); x64_u32(&a, 1u);
    x64_b(&a, 0x48); x64_b(&a, 0xD3); x64_b(&a, 0xE0); /* shl rax, cl */
    x64_b(&a, 0x4D); x64_b(&a, 0x8D); x64_b(&a, 0x7C); x64_b(&a, 0x07); x64_b(&a, (uint8_t)offsetof(CJsonSerDictKeysObject, dk_indices));

    /* Verify every key matches expected_keys[j] (1-cycle ptr eq fast path + inline key-bytes fallback for <= 16B) */
    uint32_t ascii_st = cjson_jit_get_ascii_state();
    for (uint32_t j = 0; j < n; j++) {
      uint32_t koff_e = j * 16u;
      if (koff_e < 128u) {
        x64_b(&a, 0x4D); x64_b(&a, 0x8B); x64_b(&a, 0x57); x64_b(&a, (uint8_t)koff_e); /* mov r10, [r15 + j*16] */
      } else {
        x64_b(&a, 0x4D); x64_b(&a, 0x8B); x64_b(&a, 0x97); x64_u32(&a, koff_e);
      }
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)expected_keys[j]);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC2); /* cmp r10, rax */
      uint32_t raw_klen = (klen[j] >= 4u) ? (uint32_t)(klen[j] - 4u) : 0u;
      if (ascii_st != 0 && raw_klen >= 1u && raw_klen <= 16u) {
        const char *rk = kbuf + koff[j] + 2u;
        size_t br_key_ok = x64_jcc32(&a, 0x84); /* je key_ok */
        /* Inline byte-equality fallback */
        x64_b(&a, 0x41); x64_b(&a, 0x8B); x64_b(&a, 0x42); x64_b(&a, (uint8_t)offsetof(PyASCIIObject, state));
        x64_b(&a, 0x83); x64_b(&a, 0xE0); x64_b(&a, 0xFC);
        x64_b(&a, 0x3D); x64_u32(&a, ascii_st);
        done_patches[n_done++] = x64_jcc32(&a, 0x85);
        x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0x7A); x64_b(&a, (uint8_t)offsetof(PyASCIIObject, length)); x64_b(&a, (uint8_t)raw_klen);
        done_patches[n_done++] = x64_jcc32(&a, 0x85);
        uint8_t dbase = (uint8_t)sizeof(PyASCIIObject);
        if (raw_klen >= 8u) {
          uint64_t w0, w1;
          memcpy(&w0, rk, 8);
          x64_mov_rax_u64(&a, w0);
          x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x42); x64_b(&a, dbase);
          done_patches[n_done++] = x64_jcc32(&a, 0x85);
          if (raw_klen > 8u) {
            memcpy(&w1, rk + raw_klen - 8u, 8);
            x64_mov_rax_u64(&a, w1);
            x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x42); x64_b(&a, (uint8_t)(dbase + raw_klen - 8u));
            done_patches[n_done++] = x64_jcc32(&a, 0x85);
          }
        } else if (raw_klen >= 4u) {
          uint32_t w0, w1;
          memcpy(&w0, rk, 4);
          x64_b(&a, 0x41); x64_b(&a, 0x81); x64_b(&a, 0x7A); x64_b(&a, dbase); x64_u32(&a, w0);
          done_patches[n_done++] = x64_jcc32(&a, 0x85);
          if (raw_klen > 4u) {
            memcpy(&w1, rk + raw_klen - 4u, 4);
            x64_b(&a, 0x41); x64_b(&a, 0x81); x64_b(&a, 0x7A); x64_b(&a, (uint8_t)(dbase + raw_klen - 4u)); x64_u32(&a, w1);
            done_patches[n_done++] = x64_jcc32(&a, 0x85);
          }
        } else {
          if (raw_klen >= 2u) {
            uint16_t w0; memcpy(&w0, rk, 2);
            x64_b(&a, 0x66); x64_b(&a, 0x41); x64_b(&a, 0x81); x64_b(&a, 0x7A); x64_b(&a, dbase); x64_u16(&a, w0);
            done_patches[n_done++] = x64_jcc32(&a, 0x85);
          }
          if (raw_klen & 1u) {
            uint32_t boff = raw_klen - 1u;
            x64_b(&a, 0x41); x64_b(&a, 0x80); x64_b(&a, 0x7A); x64_b(&a, (uint8_t)(dbase + boff)); x64_b(&a, (uint8_t)rk[boff]);
            done_patches[n_done++] = x64_jcc32(&a, 0x85);
          }
        }
        x64_patch32(&a, br_key_ok, a.len);
      } else {
        done_patches[n_done++] = x64_jcc32(&a, 0x85);
      }
    }
  }

  /* Hoisted row buffer check */
  x64_emit_buf_check(&a, single_row_check ? total_row_need : (uint32_t)run_max[0], err_patches, &n_err);

  for (uint32_t j = 0; j < n; j++) {
    if (!single_row_check && j > 0) {
      uint8_t prev_vt = vtypes[j - 1];
      if (prev_vt == JIT_VT_DICT || prev_vt == JIT_VT_LIST || prev_vt == JIT_VT_ANY ||
          prev_vt == JIT_VT_STR) {
        x64_emit_buf_check(&a, (uint32_t)run_max[j], err_patches, &n_err);
      }
    }

    x64_emit_literal(&a, kbuf + koff[j], klen[j]);

    /* MOV R9, [R15 + val_off] */
    uint32_t val_off = is_combined ? (j * 16u + 8u) : ((uint32_t)order[j] * 8u);
    if (val_off < 128u) {
      x64_b(&a, 0x4D); x64_b(&a, 0x8B); x64_b(&a, 0x4F); x64_b(&a, (uint8_t)val_off);
    } else {
      x64_b(&a, 0x4D); x64_b(&a, 0x8B); x64_b(&a, 0x8F); x64_u32(&a, val_off);
    }

    uint8_t vt = vtypes[j];
    if (vt == JIT_VT_NONE) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC1);
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
      size_t br_not_int = x64_jcc32(&a, 0x85);
      if (g_jit_stub_u32_long != NULL) {
        x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)g_jit_stub_u32_long);
        x64_call_rax(&a);
        err_patches[n_err++] = x64_jz_rax(&a);
      } else {
        x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF7);
        x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xCE);
        x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_jit_write_long);
        x64_call_rax(&a);
        err_patches[n_err++] = x64_jz_rax(&a);
        x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xC6);
      }
      size_t br_end1 = x64_jmp32(&a);
      x64_patch32(&a, br_not_int, a.len);
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC1);
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "null", 4);
      size_t br_end2 = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end1, a.len);
      x64_patch32(&a, br_end2, a.len);
    } else if (vt == JIT_VT_FLOAT) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyFloat_Type);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
      size_t br_not_flt = x64_jcc32(&a, 0x85);
      x64_b(&a, 0xF2); x64_b(&a, 0x41); x64_b(&a, 0x0F); x64_b(&a, 0x10);
      x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyFloatObject, ob_fval));
      x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF7);
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&cjson_jit_write_double);
      x64_call_rax(&a);
      x64_b(&a, 0x49); x64_b(&a, 0x89); x64_b(&a, 0xC6);
      size_t br_end1 = x64_jmp32(&a);
      x64_patch32(&a, br_not_flt, a.len);
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC1);
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "null", 4);
      size_t br_end2 = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end1, a.len);
      x64_patch32(&a, br_end2, a.len);
    } else if (vt == JIT_VT_STR) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyUnicode_Type);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
      size_t br_not_str = x64_jcc32(&a, 0x85);
      if (g_jit_stub_str_sse2 != NULL) {
        x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)g_jit_stub_str_sse2);
        x64_call_rax(&a);
        err_patches[n_err++] = x64_jz_rax(&a);
      } else {
        x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_emit_str, err_patches, &n_err);
      }
      size_t br_end1 = x64_jmp32(&a);
      x64_patch32(&a, br_not_str, a.len);
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC1);
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "null", 4);
      size_t br_end2 = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end1, a.len);
      x64_patch32(&a, br_end2, a.len);
    } else if (vt == JIT_VT_DICT) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyDict_Type);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
      size_t br_not_dict = x64_jcc32(&a, 0x85);
      x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyDictObject, ma_used)); x64_b(&a, 0);
      size_t br_ne = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "{}", 2);
      size_t br_end1 = x64_jmp32(&a);
      x64_patch32(&a, br_ne, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_dict, err_patches, &n_err);
      size_t br_end2 = x64_jmp32(&a);
      x64_patch32(&a, br_not_dict, a.len);
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC1);
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "null", 4);
      size_t br_end3 = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end1, a.len);
      x64_patch32(&a, br_end2, a.len);
      x64_patch32(&a, br_end3, a.len);
    } else if (vt == JIT_VT_LIST) {
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)&PyList_Type);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0x41); x64_b(&a, (uint8_t)offsetof(PyObject, ob_type));
      size_t br_not_list = x64_jcc32(&a, 0x85);
      x64_b(&a, 0x49); x64_b(&a, 0x83); x64_b(&a, 0x79); x64_b(&a, (uint8_t)offsetof(PyVarObject, ob_size)); x64_b(&a, 0);
      size_t br_ne = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "[]", 2);
      size_t br_end1 = x64_jmp32(&a);
      x64_patch32(&a, br_ne, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_array, err_patches, &n_err);
      size_t br_end2 = x64_jmp32(&a);
      x64_patch32(&a, br_not_list, a.len);
      x64_mov_rax_u64(&a, (uint64_t)(uintptr_t)Py_None);
      x64_b(&a, 0x49); x64_b(&a, 0x39); x64_b(&a, 0xC1);
      size_t br_any = x64_jcc32(&a, 0x85);
      x64_emit_literal(&a, "null", 4);
      size_t br_end3 = x64_jmp32(&a);
      x64_patch32(&a, br_any, a.len);
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
      x64_patch32(&a, br_end1, a.len);
      x64_patch32(&a, br_end2, a.len);
      x64_patch32(&a, br_end3, a.len);
    } else {
      x64_emit_call_4(&a, (uint64_t)(uintptr_t)&cjson_jit_encode_any, err_patches, &n_err);
    }
  }

  x64_emit_literal(&a, "},", 2);
  x64_b(&a, 0x49); x64_b(&a, 0xFF); x64_b(&a, 0xC5); /* inc r13 */
  size_t br_loop = x64_jmp32(&a);
  x64_patch32(&a, br_loop, loop_top);

  /* done */
  size_t done_label = a.len;
  x64_b(&a, 0x48); x64_b(&a, 0x8B); x64_b(&a, 0x54); x64_b(&a, 0x24); x64_b(&a, 8);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0x2A);
  x64_b(&a, 0x4C); x64_b(&a, 0x89); x64_b(&a, 0xF0);

  size_t epilogue_label = a.len;
  x64_b(&a, 0x48); x64_b(&a, 0x83); x64_b(&a, 0xC4); x64_b(&a, 40);
  x64_b(&a, 0x41); x64_b(&a, 0x5F);
  x64_b(&a, 0x41); x64_b(&a, 0x5E);
  x64_b(&a, 0x41); x64_b(&a, 0x5D);
  x64_b(&a, 0x41); x64_b(&a, 0x5C);
  x64_b(&a, 0x5D);
  x64_b(&a, 0x5B);
  x64_b(&a, 0xC3);

  size_t err_label = a.len;
  x64_b(&a, 0x31); x64_b(&a, 0xC0);
  size_t br_epi = x64_jmp32(&a);
  x64_patch32(&a, br_epi, epilogue_label);

  if (a.len >= a.cap) { pthread_mutex_unlock(&g_jit_mu); return NULL; }

  for (size_t k = 0; k < n_done; k++) x64_patch32(&a, done_patches[k], done_label);
  for (size_t k = 0; k < n_err; k++) x64_patch32(&a, err_patches[k], err_label);

  /* Dedicated per-schema immutable W^X page */
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

#endif /* (__APPLE__ || __linux__) && __x86_64__ */
#endif /* CJSON_JIT_SER_X86_64_H */
