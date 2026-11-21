#ifndef CJSON_PARALLEL_LOADS_H
#define CJSON_PARALLEL_LOADS_H

#if !defined(Py_GIL_DISABLED)

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
typedef CRITICAL_SECTION cjson_ld_mutex_t;
typedef CONDITION_VARIABLE cjson_ld_cond_t;
#define cjson_ld_mutex_init(m) InitializeCriticalSection(m)
#define cjson_ld_mutex_lock(m) EnterCriticalSection(m)
#define cjson_ld_mutex_unlock(m) LeaveCriticalSection(m)
#define cjson_ld_cond_init(c) InitializeConditionVariable(c)
#define cjson_ld_cond_wait(c, m) SleepConditionVariableCS(c, m, INFINITE)
#define cjson_ld_cond_broadcast(c) WakeAllConditionVariable(c)
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t cjson_ld_mutex_t;
typedef pthread_cond_t cjson_ld_cond_t;
#define cjson_ld_mutex_init(m) pthread_mutex_init(m, NULL)
#define cjson_ld_mutex_lock(m) pthread_mutex_lock(m)
#define cjson_ld_mutex_unlock(m) pthread_mutex_unlock(m)
#define cjson_ld_cond_init(c) pthread_cond_init(c, NULL)
#define cjson_ld_cond_wait(c, m) pthread_cond_wait(c, m)
#define cjson_ld_cond_broadcast(c) pthread_cond_broadcast(c)
#endif

#define CJSON_PAR_LOADS_MAX_THREADS 8

typedef struct {
  double d;
  int64_t i;
  uint8_t is_float;
} ParNumVal;

typedef struct {
  const char *start;
  const char *end;
  ParNumVal *vals;
  size_t count;
  size_t cap;
  int failed;
} ParLoadTask;

typedef struct {
  cjson_ld_mutex_t mu;
  cjson_ld_cond_t cv_work;
  cjson_ld_cond_t cv_done;
  int max_threads;
  int pool_started;
  int active_workers;
  int done_count;
  uint64_t generation;
  ParLoadTask tasks[CJSON_PAR_LOADS_MAX_THREADS];
} ParLoadPool;

static ParLoadPool g_par_loads_pool;
static int g_par_loads_initialized = 0;

static int cjson_loads_detect_cpus(void) {
  const char *env = getenv("CXJSON_MAX_THREADS");
  if (env != NULL && env[0] != '\0') {
    int v = atoi(env);
    if (v <= 1) return 1;
    return (v > CJSON_PAR_LOADS_MAX_THREADS) ? CJSON_PAR_LOADS_MAX_THREADS : v;
  }
#if defined(_WIN32)
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  int n = (int)si.dwNumberOfProcessors;
#else
  long n_l = sysconf(_SC_NPROCESSORS_ONLN);
  int n = (n_l > 0) ? (int)n_l : 1;
#endif
  if (n < 1) n = 1;
  if (n > CJSON_PAR_LOADS_MAX_THREADS) n = CJSON_PAR_LOADS_MAX_THREADS;
  return n;
}

#if !defined(_WIN32)
static void cjson_par_loads_atfork_child(void) {
  g_par_loads_pool.pool_started = 0;
  g_par_loads_pool.active_workers = 0;
  g_par_loads_pool.done_count = 0;
  g_par_loads_pool.generation = 0;
  for (int i = 0; i < CJSON_PAR_LOADS_MAX_THREADS; i++) {
    g_par_loads_pool.tasks[i].vals = NULL;
    g_par_loads_pool.tasks[i].cap = 0;
    g_par_loads_pool.tasks[i].count = 0;
  }
}
#endif

static void cjson_par_loads_init(void) {
  if (g_par_loads_initialized) return;
  g_par_loads_initialized = 1;
  memset(&g_par_loads_pool, 0, sizeof(g_par_loads_pool));
  g_par_loads_pool.max_threads = cjson_loads_detect_cpus();
#if !defined(_WIN32)
  pthread_atfork(NULL, NULL, cjson_par_loads_atfork_child);
#endif
}

/* Pure-C RFC 8259 number parser (zero Python C-API).
 * Returns 1 on success, 0 to fall back cleanly to single-threaded parser. */
static inline int parse_number_raw(const char **p_io, const char *end, ParNumVal *out) {
  const char *p = *p_io;
  int is_neg = 0;
  if (*p == '-') {
    is_neg = 1;
    p++;
    if (CJSON_UNLIKELY(p >= end || (uint32_t)(*p - '0') >= 10)) return 0;
  }

  uint64_t mant = 0;
  int int_digits = 0;
  if (*p == '0') {
    p++;
    int_digits = 1;
    if (CJSON_UNLIKELY(p < end && (uint32_t)(*p - '0') < 10)) return 0;
  } else {
    const char *int_start = p;
    mant = (uint64_t)(*p++ - '0');
    while (p < end && (uint32_t)(*p - '0') < 10) {
      mant = mant * 10 + (uint64_t)(*p - '0');
      p++;
      if (CJSON_UNLIKELY(p - int_start >= 19)) return 0;
    }
    int_digits = (int)(p - int_start);
  }

  if (p >= end || (*p != '.' && *p != 'e' && *p != 'E')) {
    *p_io = p;
    out->is_float = 0;
    out->i = is_neg ? -(int64_t)mant : (int64_t)mant;
    return 1;
  }

  if (CJSON_UNLIKELY(*p != '.')) return 0;
  p++;
  if (CJSON_UNLIKELY(p >= end || (uint32_t)(*p - '0') >= 10)) return 0;

  const char *frac_start = p;
  int max_frac = 19 - ((mant == 0) ? 0 : int_digits);
  const char *frac_limit = (end - p > max_frac) ? (p + max_frac) : end;

  if (CJSON_LIKELY(p + 8 <= end && max_frac >= 8)) {
    int n = parse_up_to_8_digits_swar(p, 8, &mant);
    p += n;
    if (n == 8 && p + 8 <= end && p < frac_limit) {
      int rem_frac = (int)(frac_limit - p);
      int max_d = (rem_frac < 8) ? rem_frac : 8;
      n = parse_up_to_8_digits_swar(p, max_d, &mant);
      p += n;
    }
  }
  while (p < frac_limit && (uint32_t)(*p - '0') < 10) {
    mant = mant * 10 + (uint64_t)(*p - '0');
    p++;
  }
  int exp10 = -(int)(p - frac_start);
  if (CJSON_UNLIKELY(p < end && (*p == 'e' || *p == 'E'))) {
    p++;
    int exp_neg = 0;
    if (p < end && (*p == '+' || *p == '-')) {
      exp_neg = (*p == '-');
      p++;
    }
    if (CJSON_UNLIKELY(p >= end || (uint32_t)(*p - '0') >= 10)) return 0;
    int exp_val = *p++ - '0';
    while (p < end && (uint32_t)(*p - '0') < 10) {
      if (exp_val < 10000) exp_val = exp_val * 10 + (*p - '0');
      p++;
    }
    exp10 += exp_neg ? -exp_val : exp_val;
  } else if (CJSON_UNLIKELY(p < end && (uint32_t)(*p - '0') < 10)) {
    return 0;
  }

  double d;
  if (mant < (1ULL << 53) && exp10 >= -22 && exp10 <= 22) {
    d = (exp10 < 0) ? ((double)mant / POW10_DBL[-exp10])
                    : ((double)mant * POW10_DBL[exp10]);
    if (is_neg) d = -d;
  } else if (CJSON_UNLIKELY(!eisel_lemire_double(mant, exp10, is_neg, &d))) {
    return 0;
  }
  *p_io = p;
  out->is_float = 1;
  out->d = d;
  return 1;
}

static void par_load_run_task(ParLoadTask *task) {
  const char *p = task->start;
  const char *end = task->end;
  size_t count = 0;
  size_t cap = task->cap;
  ParNumVal *vals = task->vals;

  if (cap < 16384) {
    size_t init_cap = (size_t)((end - p) / 4) + 256;
    if (init_cap < 16384) init_cap = 16384;
    ParNumVal *nv = (ParNumVal *)realloc(vals, init_cap * sizeof(ParNumVal));
    if (nv == NULL) {
      task->failed = 1;
      return;
    }
    vals = nv;
    cap = init_cap;
    task->vals = vals;
    task->cap = cap;
  }

  while (p < end) {
    if (CJSON_UNLIKELY((uint8_t)*p <= 0x20)) {
      p = skip_ws_slow(p, end);
      if (p >= end) break;
    }
    char c = *p;
    if (CJSON_UNLIKELY(c != '-' && (uint32_t)(c - '0') >= 10)) {
      task->failed = 1;
      return;
    }
    if (CJSON_UNLIKELY(count >= cap)) {
      size_t ncap = cap * 2;
      ParNumVal *nv = (ParNumVal *)realloc(vals, ncap * sizeof(ParNumVal));
      if (nv == NULL) {
        task->failed = 1;
        return;
      }
      vals = nv;
      cap = ncap;
      task->vals = vals;
      task->cap = cap;
    }
    if (CJSON_UNLIKELY(!parse_number_raw(&p, end, &vals[count]))) {
      task->failed = 1;
      return;
    }
    count++;

    if (CJSON_LIKELY(p < end && *p == ',')) {
      p++;
      if (p < end && *p == ' ') p++;
      if (CJSON_UNLIKELY(p >= end)) {
        /* Trailing comma inside slice -> let single-threaded parser handle/error */
        task->failed = 1;
        return;
      }
      continue;
    }
    if (p < end) {
      p = skip_ws_slow(p, end);
      if (p >= end) break;
      if (*p == ',') {
        p++;
        p = skip_ws_slow(p, end);
        if (CJSON_UNLIKELY(p >= end)) {
          task->failed = 1;
          return;
        }
        continue;
      }
      task->failed = 1;
      return;
    }
  }

  task->count = count;
  task->failed = 0;
}

#if defined(_WIN32)
static unsigned __stdcall par_load_worker_entry(void *arg) {
#else
static void *par_load_worker_entry(void *arg) {
#endif
  int id = (int)(intptr_t)arg;
  uint64_t seen_gen = 0;
  for (;;) {
    cjson_ld_mutex_lock(&g_par_loads_pool.mu);
    while (g_par_loads_pool.generation == seen_gen) {
      cjson_ld_cond_wait(&g_par_loads_pool.cv_work, &g_par_loads_pool.mu);
    }
    seen_gen = g_par_loads_pool.generation;
    int active = g_par_loads_pool.active_workers;
    cjson_ld_mutex_unlock(&g_par_loads_pool.mu);

    if (id <= active) {
      par_load_run_task(&g_par_loads_pool.tasks[id]);
      cjson_ld_mutex_lock(&g_par_loads_pool.mu);
      g_par_loads_pool.done_count++;
      if (g_par_loads_pool.done_count == active) {
        cjson_ld_cond_broadcast(&g_par_loads_pool.cv_done);
      }
      cjson_ld_mutex_unlock(&g_par_loads_pool.mu);
    }
  }
#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}

static int cjson_par_loads_ensure_started(void) {
  if (CJSON_LIKELY(g_par_loads_pool.pool_started)) return 1;
  cjson_ld_mutex_init(&g_par_loads_pool.mu);
  cjson_ld_cond_init(&g_par_loads_pool.cv_work);
  cjson_ld_cond_init(&g_par_loads_pool.cv_done);
  int nw = g_par_loads_pool.max_threads - 1;
  for (int i = 0; i < nw; i++) {
#if defined(_WIN32)
    uintptr_t h = _beginthreadex(NULL, 0, par_load_worker_entry, (void *)(intptr_t)(i + 1), 0, NULL);
    if (h == 0) {
      g_par_loads_pool.max_threads = i + 1;
      break;
    }
    CloseHandle((HANDLE)h);
#else
    pthread_t tid;
    if (pthread_create(&tid, NULL, par_load_worker_entry, (void *)(intptr_t)(i + 1)) != 0) {
      g_par_loads_pool.max_threads = i + 1;
      break;
    }
    pthread_detach(tid);
#endif
  }
  g_par_loads_pool.pool_started = 1;
  return g_par_loads_pool.max_threads > 1;
}

/* Attempts parallel two-phase numeric array parse starting at *p_io (first number after '[').
 * Returns a new PyList on success and advances *p_io past ']'.
 * Returns NULL (with no exception set) to fall back cleanly to single-threaded parse. */
static CJSON_NOINLINE PyObject *cjson_parallel_parse_num_array(const char **p_io, const char *end) {
  if (g_par_loads_pool.max_threads <= 1) return NULL;
  const char *p = *p_io;
  Py_ssize_t rem = end - p;
  if (rem < 256 * 1024) return NULL;

  const char *close = (const char *)memchr(p, ']', (size_t)rem);
  if (close == NULL) return NULL;
  Py_ssize_t span = close - p;
  if (span < 256 * 1024) return NULL;

  /* Quick O(1) probe at midpoint to immediately reject mixed/string/object arrays */
  unsigned char mid = (unsigned char)p[span >> 1];
  if (!((mid >= '0' && mid <= '9') || mid == ',' || mid == '.' || mid == '-' ||
        mid == ' ' || mid == '\n' || mid == 'e' || mid == 'E' || mid == '+')) {
    return NULL;
  }

  if (!cjson_par_loads_ensure_started()) return NULL;

  int desired = (span < 1024 * 1024) ? 2 : ((span < 4 * 1024 * 1024) ? 4 : g_par_loads_pool.max_threads);
  int nthreads = (desired < g_par_loads_pool.max_threads) ? desired : g_par_loads_pool.max_threads;
  if (nthreads <= 1) return NULL;

  /* Split [p, close) at exact ',' boundaries */
  const char *cur = p;
  Py_ssize_t chunk = span / nthreads;
  int actual = 0;
  for (int t = 0; t < nthreads && cur < close; t++) {
    const char *slice_end = close;
    if (t + 1 < nthreads) {
      const char *target = p + (Py_ssize_t)(t + 1) * chunk;
      if (target < cur) target = cur;
      if (target < close) {
        const char *comma = (const char *)memchr(target, ',', (size_t)(close - target));
        if (comma != NULL) {
          slice_end = comma;
        }
      }
    }
    g_par_loads_pool.tasks[t].start = cur;
    g_par_loads_pool.tasks[t].end = slice_end;
    g_par_loads_pool.tasks[t].count = 0;
    g_par_loads_pool.tasks[t].failed = 0;
    actual++;
    cur = (slice_end < close) ? (slice_end + 1) : close;
  }
  if (actual <= 1) return NULL;

  /* Dispatch workers 1..actual-1 */
  cjson_ld_mutex_lock(&g_par_loads_pool.mu);
  g_par_loads_pool.active_workers = actual - 1;
  g_par_loads_pool.done_count = 0;
  g_par_loads_pool.generation++;
  cjson_ld_cond_broadcast(&g_par_loads_pool.cv_work);
  cjson_ld_mutex_unlock(&g_par_loads_pool.mu);

  /* Main thread parses slice 0 */
  par_load_run_task(&g_par_loads_pool.tasks[0]);

  /* Wait for workers */
  cjson_ld_mutex_lock(&g_par_loads_pool.mu);
  while (g_par_loads_pool.done_count < g_par_loads_pool.active_workers) {
    cjson_ld_cond_wait(&g_par_loads_pool.cv_done, &g_par_loads_pool.mu);
  }
  cjson_ld_mutex_unlock(&g_par_loads_pool.mu);

  size_t total_count = 0;
  for (int t = 0; t < actual; t++) {
    if (g_par_loads_pool.tasks[t].failed) return NULL;
    total_count += g_par_loads_pool.tasks[t].count;
  }
  if (total_count == 0) return NULL;

  /* Phase 2: Box pre-parsed C numbers into a single exact-sized PyList */
  PyObject *list = PyList_New((Py_ssize_t)total_count);
  if (CJSON_UNLIKELY(list == NULL)) return NULL;

  Py_ssize_t idx = 0;
  for (int t = 0; t < actual; t++) {
    const ParNumVal *vals = g_par_loads_pool.tasks[t].vals;
    size_t cnt = g_par_loads_pool.tasks[t].count;
    for (size_t k = 0; k < cnt; k++) {
      PyObject *v;
      if (vals[k].is_float) {
        v = PyFloat_FromDouble(vals[k].d);
      } else {
        int64_t iv = vals[k].i;
        if (iv >= -5 && iv <= 256) {
          v = g_small_ints[iv + 5];
          Py_INCREF(v);
        } else {
          v = PyLong_FromLongLong((long long)iv);
        }
      }
      if (CJSON_UNLIKELY(v == NULL)) {
        Py_DECREF(list);
        return NULL;
      }
      PyList_SET_ITEM(list, idx++, v);
    }
  }

  *p_io = close + 1;
  return list;
}

#endif /* !Py_GIL_DISABLED */

#endif /* CJSON_PARALLEL_LOADS_H */
