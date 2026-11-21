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
    char *cap_limit = rb->data + rb->cap - 128;
    if (CJSON_UNLIKELY(dst > cap_limit)) {
      cur = (size_t)(dst - rb->data);
      if (raw_buf_grow(rb, cur + (size_t)(end - p) + 256) < 0) return NULL;
      dst = rb->data + cur;
      cap_limit = rb->data + rb->cap - 128;
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
  PyObject **items;
  const ParDictPair *pairs;
  Py_ssize_t start;
  Py_ssize_t end;
  RawSerBuf out;
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
  uint64_t generation;
  ParTask tasks[CJSON_MAX_WORKERS];
} ParPool;

static ParPool g_par_pool;

static void par_run_task(ParTask *t) {
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
    cjson_mutex_unlock(&g_par_pool.mu);

    if (run) {
      par_run_task(&g_par_pool.tasks[wid]);
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
    g_par_pool.tasks[t].items = items;
    g_par_pool.tasks[t].pairs = pairs;
    g_par_pool.tasks[t].start = s;
    g_par_pool.tasks[t].end = e;
    g_par_pool.tasks[t].failed = 0;
    actual++;
  }
  if (actual <= 1) return 0;

  /* Dispatch workers 1..actual-1 */
  cjson_mutex_lock(&g_par_pool.mu);
  g_par_pool.active_workers = actual - 1;
  g_par_pool.done_count = 0;
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
  for (int t = 0; t < actual; t++) {
    memcpy(dst, g_par_pool.tasks[t].out.data, g_par_pool.tasks[t].out.len);
    dst += g_par_pool.tasks[t].out.len;
  }
  dst[-1] = is_dict ? '}' : ']';
  b->len = (Py_ssize_t)(dst - b->data);
  return 1;
}

#endif /* !Py_GIL_DISABLED */

#endif /* CJSON_PARALLEL_SER_H */
