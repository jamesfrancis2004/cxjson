#ifndef CJSON_KEY_CACHE_H
#define CJSON_KEY_CACHE_H

#include "utf8.h"

#ifndef Py_GIL_DISABLED

/* 32-byte inline-key cache slot: 2-way set = 1 single 64-byte L1 cache line.
 * Empty slots have k_hi == 0 while valid 1..15B keys have k_hi >= (1ULL << 56),
 * so matching k_lo & k_hi guarantees non-empty, exact length, and exact bytes
 * without dereferencing the heap PyASCIIObject. */
typedef struct {
  PyObject *str;
  Py_hash_t py_hash;
  uint64_t k_lo;
  uint64_t k_hi;
} FastKeySlot;

typedef struct {
  PyObject *str;
  Py_hash_t py_hash;
  uint64_t hash64;
} GlobalLongKeySlot;

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define CJSON_FAST_KEY_MAX_SLOTS 4096
#define CJSON_LONG_KEY_MAX_SLOTS 256
#define CJSON_VAL_STR_MAX_SLOTS  512
#define CJSON_LONG_VAL_MAX_SLOTS 512

static FastKeySlot g_fast_key_cache[CJSON_FAST_KEY_MAX_SLOTS] __attribute__((aligned(64)));
static GlobalLongKeySlot g_long_key_cache[CJSON_LONG_KEY_MAX_SLOTS] __attribute__((aligned(64)));
static uint32_t g_key_evict_toggle = 0;

/* Runtime L1D-adaptive index masks (configured once at module import). */
static uint64_t g_key_cache_mask = 510u;       /* 512 slots (16 KB) default for 32 KB L1D */
static uint32_t g_val_cache_mask = 127u;       /* 128 slots (4 KB) */
static uint32_t g_long_val_cache_mask = 127u;  /* 128 slots (2 KB) */
static uint32_t g_long_key_cache_mask = 62u;   /* 64 slots (1.5 KB) */
static size_t g_detected_l1d_bytes = 32768;

static size_t cjson_detect_l1d_cache_bytes(void) {
  const char *env = getenv("CXJSON_L1D_KB");
  if (env != NULL && env[0] != '\0') {
    long kb = atol(env);
    if (kb >= 16 && kb <= 4096) return (size_t)kb * 1024u;
  }
#if defined(__APPLE__)
  size_t l1d = 0;
  size_t len = sizeof(l1d);
  if (sysctlbyname("hw.l1dcachesize", &l1d, &len, NULL, 0) == 0 && l1d >= 16384) {
    return l1d;
  }
#elif defined(__linux__)
#if defined(_SC_LEVEL1_DCACHE_SIZE)
  long l1d = sysconf(_SC_LEVEL1_DCACHE_SIZE);
  if (l1d >= 16384) return (size_t)l1d;
#endif
  /* Fallback for Linux VMs/containers (AMD/ARM/musl) where sysconf returns 0/-1 */
  FILE *fp = fopen("/sys/devices/system/cpu/cpu0/cache/index0/size", "r");
  if (fp != NULL) {
    unsigned long val = 0;
    char unit = 0;
    int matched = fscanf(fp, "%lu%c", &val, &unit);
    fclose(fp);
    if (matched >= 1 && val > 0) {
      size_t bytes = (unit == 'K' || unit == 'k') ? (val * 1024u)
                   : ((unit == 'M' || unit == 'm') ? (val * 1024u * 1024u) : val);
      if (bytes >= 16384 && bytes <= 4u * 1024u * 1024u) return bytes;
    }
  }
#elif defined(_WIN32)
  DWORD buf_len = 0;
  GetLogicalProcessorInformationEx(RelationCache, NULL, &buf_len);
  if (buf_len > 0 && buf_len < 65536) {
    char *buf = (char *)malloc(buf_len);
    if (buf != NULL) {
      size_t found = 0;
      if (GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &buf_len)) {
        DWORD off = 0;
        while (off < buf_len) {
          PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info =
              (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
          if (info->Relationship == RelationCache && info->Cache.Level == 1 &&
              (info->Cache.Type == CacheData || info->Cache.Type == CacheUnified) &&
              info->Cache.CacheSize >= 16384) {
            found = (size_t)info->Cache.CacheSize;
            break;
          }
          if (info->Size == 0) break;
          off += info->Size;
        }
      }
      free(buf);
      if (found > 0) return found;
    }
  }
#endif
  return 32768;
}

static void cjson_init_cache_sizing(void) {
  size_t l1d = cjson_detect_l1d_cache_bytes();
  g_detected_l1d_bytes = l1d;
  if (l1d >= 262144) {
    /* Future >= 256 KB L1D CPUs */
    g_key_cache_mask = 4096u - 2u;      /* 128 KB */
    g_val_cache_mask = 512u - 1u;       /* 16 KB */
    g_long_val_cache_mask = 512u - 1u;  /* 8 KB */
    g_long_key_cache_mask = 256u - 2u;  /* 6 KB */
  } else if (l1d >= 131072) {
    /* 128 KB L1D CPUs (Apple M1/M2/M3/M4, etc.) */
    g_key_cache_mask = 2048u - 2u;      /* 64 KB */
    g_val_cache_mask = 256u - 1u;       /* 8 KB */
    g_long_val_cache_mask = 256u - 1u;  /* 4 KB */
    g_long_key_cache_mask = 256u - 2u;  /* 6 KB */
  } else if (l1d >= 49152) {
    /* 48-64 KB L1D CPUs (Ice Lake/Sapphire Rapids/Zen 5/Graviton) */
    g_key_cache_mask = 1024u - 2u;      /* 32 KB */
    g_val_cache_mask = 256u - 1u;       /* 8 KB */
    g_long_val_cache_mask = 256u - 1u;  /* 4 KB */
    g_long_key_cache_mask = 128u - 2u;  /* 3 KB */
  } else {
    /* 32 KB L1D CPUs (Skylake/Cascade Lake/Zen 2-4) */
    g_key_cache_mask = 512u - 2u;       /* 16 KB */
    g_val_cache_mask = 128u - 1u;       /* 4 KB */
    g_long_val_cache_mask = 128u - 1u;  /* 2 KB */
    g_long_key_cache_mask = 64u - 2u;   /* 1.5 KB */
  }
}

static inline uint64_t wymix64_parts(uint64_t a, uint64_t b, uint32_t len) {
  a ^= 0xA0761D6478BD642FULL;
  b ^= 0xE7037ED1A0B428DBULL ^ ((uint64_t)len * 0x9E3779B97F4A7C15ULL);
#if defined(__SIZEOF_INT128__)
  __uint128_t r = (__uint128_t)a * (__uint128_t)b;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
#else
  uint64_t h = a * b;
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ULL;
  return h ^ (h >> 31);
#endif
}

static inline uint64_t hash_key_bytes64(const char *ptr, uint32_t len) {
  uint64_t a, b;
  if (len >= 8) {
    memcpy(&a, ptr, 8);
    memcpy(&b, ptr + len - 8, 8);
    if (CJSON_UNLIKELY(len > 16)) {
      uint64_t c;
      memcpy(&c, ptr + (len >> 1) - 4, 8);
      a ^= c * 0x94D049BB133111EBULL;
    }
  } else if (len >= 4) {
    uint32_t x, y;
    memcpy(&x, ptr, 4);
    memcpy(&y, ptr + len - 4, 4);
    a = x;
    b = y;
  } else {
    a = ((uint64_t)(uint8_t)ptr[0] << 16) |
        ((uint64_t)(uint8_t)ptr[len >> 1] << 8) |
        (uint64_t)(uint8_t)ptr[len - 1];
    b = len;
  }
  return wymix64_parts(a, b, len);
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static const uint64_t CJSON_KEY_MASK_LO[16] = {
    0x0000000000000000ULL, 0x00000000000000FFULL, 0x000000000000FFFFULL, 0x0000000000FFFFFFULL,
    0x00000000FFFFFFFFULL, 0x000000FFFFFFFFFFULL, 0x0000FFFFFFFFFFFFULL, 0x00FFFFFFFFFFFFFFULL,
    0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
    0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
};

static const uint64_t CJSON_KEY_MASK_HI[16] = {
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
    0x0000000000000000ULL, 0x00000000000000FFULL, 0x000000000000FFFFULL, 0x0000000000FFFFFFULL,
    0x00000000FFFFFFFFULL, 0x000000FFFFFFFFFFULL, 0x0000FFFFFFFFFFFFULL, 0x00FFFFFFFFFFFFFFULL,
};

/* Caller guarantees 1 <= klen <= 15, pure ASCII, and start[0..15] readable. */
static inline PyObject *lookup_or_create_fast_ascii_key15(
    const char *start, uint32_t klen, uint64_t raw_lo, uint64_t raw_hi, Py_hash_t *out_hash) {
  uint64_t k_lo = raw_lo & CJSON_KEY_MASK_LO[klen];
  uint64_t k_hi = (raw_hi & CJSON_KEY_MASK_HI[klen]) | ((uint64_t)klen << 56);
  uint32_t slot0 = (uint32_t)((((k_lo ^ (k_hi * 0x9E3779B97F4A7C15ULL)) * 0xBF58476D1CE4E5B9ULL) >> 52) &
                              g_key_cache_mask);
  FastKeySlot *s0 = &g_fast_key_cache[slot0];
  FastKeySlot *s1 = s0 + 1;

  if (CJSON_LIKELY(s0->k_lo == k_lo && s0->k_hi == k_hi)) {
    *out_hash = s0->py_hash;
    Py_INCREF(s0->str);
    return s0->str;
  }
  if (CJSON_LIKELY(s1->k_lo == k_lo && s1->k_hi == k_hi)) {
    *out_hash = s1->py_hash;
    Py_INCREF(s1->str);
    return s1->str;
  }

  PyObject *u = make_ascii_str(start, (Py_ssize_t)klen);
  if (CJSON_UNLIKELY(u == NULL)) return NULL;
  Py_hash_t py_h = PyObject_Hash(u);
  *out_hash = py_h;

  FastKeySlot *target = (s0->k_hi == 0) ? s0 : ((s1->k_hi == 0) ? s1 : (((g_key_evict_toggle++) & 1u) ? s1 : s0));
  PyObject *prev = target->str;
  Py_INCREF(u);
  target->str = u;
  target->py_hash = py_h;
  target->k_lo = k_lo;
  target->k_hi = k_hi;
  Py_XDECREF(prev);
  return u;
}

#define CJSON_VAL_STR_CACHE_SIZE 256

/* 32-byte direct-mapped short ASCII string value cache (2..15B).
 * Empty slots have k_hi == 0; valid entries have k_hi >= (2ULL << 56). */
typedef struct {
  PyObject *str;
  uint64_t k_lo;
  uint64_t k_hi;
  uint64_t _pad;
} ValStrSlot;

static ValStrSlot g_val_str_cache[CJSON_VAL_STR_MAX_SLOTS] __attribute__((aligned(64)));

/* Caller guarantees 0 <= slen <= 7, pure ASCII, and raw_lo holds bytes 0..7. */
static inline PyObject *lookup_or_create_fast_ascii_val7(
    const char *start, uint32_t slen, uint64_t raw_lo) {
  if (CJSON_UNLIKELY(slen <= 1)) {
    if (slen == 0) {
      Py_INCREF(g_empty_str);
      return g_empty_str;
    }
    PyObject *ch = g_ascii_chars[(uint8_t)start[0]];
    Py_INCREF(ch);
    return ch;
  }
  uint64_t k_lo = raw_lo & CJSON_KEY_MASK_LO[slen];
  uint64_t k_hi = (uint64_t)slen << 56;
  uint32_t slot = (uint32_t)((((k_lo ^ (k_hi * 0x9E3779B97F4A7C15ULL)) * 0xBF58476D1CE4E5B9ULL) >> 55) & g_val_cache_mask);
  ValStrSlot *s = &g_val_str_cache[slot];
  if (CJSON_LIKELY(s->k_lo == k_lo && s->k_hi == k_hi)) {
    Py_INCREF(s->str);
    return s->str;
  }
  PyObject *u = make_ascii_str(start, (Py_ssize_t)slen);
  if (CJSON_UNLIKELY(u == NULL)) return NULL;
  PyObject *prev = s->str;
  Py_INCREF(u);
  s->str = u;
  s->k_lo = k_lo;
  s->k_hi = k_hi;
  Py_XDECREF(prev);
  return u;
}

/* Caller guarantees 8 <= slen <= 15, pure ASCII, and raw_lo/raw_hi hold bytes 0..15. */
static inline PyObject *lookup_or_create_fast_ascii_val15(
    const char *start, uint32_t slen, uint64_t raw_lo, uint64_t raw_hi) {
  uint64_t k_lo = raw_lo;
  uint64_t k_hi = (raw_hi & CJSON_KEY_MASK_HI[slen]) | ((uint64_t)slen << 56);
  uint32_t slot = (uint32_t)((((k_lo ^ (k_hi * 0x9E3779B97F4A7C15ULL)) * 0xBF58476D1CE4E5B9ULL) >> 55) & g_val_cache_mask);
  ValStrSlot *s = &g_val_str_cache[slot];
  if (CJSON_LIKELY(s->k_lo == k_lo && s->k_hi == k_hi)) {
    Py_INCREF(s->str);
    return s->str;
  }
  PyObject *u = make_ascii_str(start, (Py_ssize_t)slen);
  if (CJSON_UNLIKELY(u == NULL)) return NULL;
  PyObject *prev = s->str;
  Py_INCREF(u);
  s->str = u;
  s->k_lo = k_lo;
  s->k_hi = k_hi;
  Py_XDECREF(prev);
  return u;
}

typedef struct {
  PyObject *str;
  uint64_t hash64;
} LongValStrSlot;

static LongValStrSlot g_long_val_str_cache[CJSON_LONG_VAL_MAX_SLOTS] __attribute__((aligned(64)));

/* Direct-mapped cache for repeated 16..48B ASCII string values (URLs, schema types, timestamps). */
static inline PyObject *lookup_or_create_long_ascii_val(const char *start, uint32_t slen) {
  uint64_t h64 = hash_key_bytes64(start, slen);
  LongValStrSlot *s = &g_long_val_str_cache[h64 & g_long_val_cache_mask];
  if (CJSON_LIKELY(s->str != NULL && s->hash64 == h64 &&
                   (uint32_t)PyUnicode_GET_LENGTH(s->str) == slen &&
                   memcmp(PyUnicode_1BYTE_DATA(s->str), start, slen) == 0)) {
    Py_INCREF(s->str);
    return s->str;
  }
  PyObject *u = make_ascii_str(start, (Py_ssize_t)slen);
  if (CJSON_UNLIKELY(u == NULL)) return NULL;
  PyObject *prev = s->str;
  Py_INCREF(u);
  s->str = u;
  s->hash64 = h64;
  Py_XDECREF(prev);
  return u;
}
#endif

/* Caller guarantees 1 <= klen <= 64 and pure ASCII. */
__attribute__((noinline))
static PyObject *lookup_or_create_long_ascii_key(
    const char *start, uint32_t klen, Py_hash_t *out_hash) {
  uint64_t h64 = hash_key_bytes64(start, klen);
  uint32_t slot0 = (uint32_t)(h64 & g_long_key_cache_mask);
  GlobalLongKeySlot *s0 = &g_long_key_cache[slot0];
  GlobalLongKeySlot *s1 = s0 + 1;

  if (s0->str != NULL && s0->hash64 == h64 && (uint32_t)PyUnicode_GET_LENGTH(s0->str) == klen &&
      memcmp(PyUnicode_1BYTE_DATA(s0->str), start, klen) == 0) {
    *out_hash = s0->py_hash;
    Py_INCREF(s0->str);
    return s0->str;
  }
  if (s1->str != NULL && s1->hash64 == h64 && (uint32_t)PyUnicode_GET_LENGTH(s1->str) == klen &&
      memcmp(PyUnicode_1BYTE_DATA(s1->str), start, klen) == 0) {
    *out_hash = s1->py_hash;
    Py_INCREF(s1->str);
    return s1->str;
  }

  PyObject *u = make_ascii_str(start, (Py_ssize_t)klen);
  if (CJSON_UNLIKELY(u == NULL)) return NULL;
  Py_hash_t py_h = PyObject_Hash(u);
  *out_hash = py_h;

  GlobalLongKeySlot *target = (s0->str == NULL) ? s0 : ((s1->str == NULL) ? s1 : (((g_key_evict_toggle++) & 1u) ? s1 : s0));
  PyObject *prev = target->str;
  Py_INCREF(u);
  target->str = u;
  target->py_hash = py_h;
  target->hash64 = h64;
  Py_XDECREF(prev);
  return u;
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CJSON_SCHEMA_SLOTS 64
#define CJSON_SCHEMA_MAX_KEYS 40

#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED) && !defined(Py_LIMITED_API) && !defined(PYPY_VERSION)
#define CJSON_SPLIT_DICT_FAST 1
#define CJSON_SPLIT_MAX_KEYS 30
#if PY_VERSION_HEX >= 0x030D0000
#define CJSON_SPLIT_DICT_313 1
#include <stddef.h>
typedef struct {
  uint8_t capacity;
  uint8_t size;
  uint8_t embedded;
  uint8_t valid;
  PyObject *values[1];
} CJsonDictValues313;
#endif

typedef struct {
  PyObject *me_key;
  PyObject *me_value;
} CJsonDictUnicodeEntry;

typedef struct {
  Py_ssize_t dk_refcnt;
  uint8_t dk_log2_size;
  uint8_t dk_log2_index_bytes;
  uint8_t dk_kind;
  uint32_t dk_version;
  Py_ssize_t dk_usable;
  Py_ssize_t dk_nentries;
  char dk_indices[];
} CJsonDictKeysObject;

static void cjson_free_shared_keys(CJsonDictKeysObject *dk) {
  if (dk == NULL) return;
  if (--dk->dk_refcnt == 0) {
    CJsonDictUnicodeEntry *ep =
        (CJsonDictUnicodeEntry *)(dk->dk_indices + ((size_t)1 << dk->dk_log2_index_bytes));
    for (Py_ssize_t i = 0; i < dk->dk_nentries; i++) {
      Py_XDECREF(ep[i].me_key);
    }
    PyObject_Free(dk);
  }
}

static CJsonDictKeysObject *cjson_build_shared_keys(
    PyObject *const *keys, const Py_hash_t *hashes, uint32_t n) {
  if (n == 0 || n > CJSON_SPLIT_MAX_KEYS) return NULL;
  /* Use log2_size >= 4 so CPython's free_keys_object never puts it in the
   * log2_size==3 freelist (which expects fixed 5-entry size). */
  uint8_t log2_size = (n <= 10) ? 4 : ((n <= 21) ? 5 : 6);
  size_t table_size = (size_t)1 << log2_size;
  size_t alloc_size = sizeof(CJsonDictKeysObject) + table_size + (size_t)n * sizeof(CJsonDictUnicodeEntry);
  CJsonDictKeysObject *dk = (CJsonDictKeysObject *)PyObject_Malloc(alloc_size);
  if (dk == NULL) return NULL;
  dk->dk_refcnt = 1;
  dk->dk_log2_size = log2_size;
  dk->dk_log2_index_bytes = log2_size; /* table_size <= 64 <= 255 -> 1-byte indices */
  dk->dk_kind = 2; /* DICT_KEYS_SPLIT */
  dk->dk_version = 0;
  dk->dk_usable = 0; /* 0 usable -> any new key insertion promotes to combined without mutating dk */
  dk->dk_nentries = (Py_ssize_t)n;
  memset(dk->dk_indices, -1, table_size); /* DKIX_EMPTY = -1 */

  CJsonDictUnicodeEntry *ep = (CJsonDictUnicodeEntry *)(dk->dk_indices + table_size);
  size_t mask = table_size - 1;
  for (uint32_t i = 0; i < n; i++) {
    Py_INCREF(keys[i]);
    ep[i].me_key = keys[i];
    ep[i].me_value = NULL;
    size_t perturb = (size_t)hashes[i];
    size_t j = perturb & mask;
    while ((int8_t)dk->dk_indices[j] >= 0) {
      perturb >>= 5;
      j = (j * 5 + perturb + 1) & mask;
    }
    dk->dk_indices[j] = (char)(int8_t)i;
  }
  return dk;
}

/* Build an exact-sized combined-table PyDict directly from buffered KvEntry array.
 * Transfers ownership of all kvs[i].key and kvs[i].val references (even on error!). */
static PyObject *cjson_build_combined_dict_from_kvs(const KvEntry *kvs, size_t count) {
  if (CJSON_UNLIKELY(count == 0)) {
    return PyDict_New();
  }
  uint8_t log2_size = 4; /* >= 4 avoids log2_size==3 freelist size assumption */
  while ((((size_t)1 << log2_size) * 2u) / 3u < count) {
    log2_size++;
  }
  size_t table_size = (size_t)1 << log2_size;
  Py_ssize_t usable = (Py_ssize_t)((table_size * 2u) / 3u);
  uint8_t log2_index_bytes = (log2_size <= 7) ? log2_size : ((log2_size <= 15) ? (uint8_t)(log2_size + 1) : (uint8_t)(log2_size + 2));
  size_t index_bytes = (size_t)1 << log2_index_bytes;
  size_t alloc_size = sizeof(CJsonDictKeysObject) + index_bytes + (size_t)usable * sizeof(CJsonDictUnicodeEntry);

  CJsonDictKeysObject *dk = (CJsonDictKeysObject *)PyObject_Malloc(alloc_size);
  if (CJSON_UNLIKELY(dk == NULL)) {
    for (size_t i = 0; i < count; i++) {
      Py_DECREF(kvs[i].key);
      Py_DECREF(kvs[i].val);
    }
    PyErr_NoMemory();
    return NULL;
  }

  PyObject *dict = PyDict_New();
  if (CJSON_UNLIKELY(dict == NULL)) {
    PyObject_Free(dk);
    for (size_t i = 0; i < count; i++) {
      Py_DECREF(kvs[i].key);
      Py_DECREF(kvs[i].val);
    }
    return NULL;
  }

  dk->dk_refcnt = 1;
  dk->dk_log2_size = log2_size;
  dk->dk_log2_index_bytes = log2_index_bytes;
  dk->dk_kind = 1; /* DICT_KEYS_UNICODE */
  dk->dk_version = 0;
  memset(dk->dk_indices, 0xFF, index_bytes);

  CJsonDictUnicodeEntry *ep = (CJsonDictUnicodeEntry *)(dk->dk_indices + index_bytes);
  size_t mask = table_size - 1u;
  Py_ssize_t nentries = 0;
  int has_gc = 0;

  if (log2_size <= 7) {
    int8_t *indices = (int8_t *)dk->dk_indices;
    for (size_t j = 0; j < count; j++) {
      PyObject *key = kvs[j].key;
      PyObject *val = kvs[j].val;
      Py_hash_t kh = kvs[j].hash;
      if (!has_gc && PyObject_IS_GC(val)) has_gc = 1;
      size_t perturb = (size_t)kh;
      size_t idx = perturb & mask;
      for (;;) {
        int8_t ix = indices[idx];
        if (CJSON_LIKELY(ix == -1)) {
          indices[idx] = (int8_t)nentries;
          ep[nentries].me_key = key;
          ep[nentries].me_value = val;
          nentries++;
          break;
        }
        PyObject *ek = ep[ix].me_key;
        if (CJSON_UNLIKELY(ek == key || PyUnicode_Compare(ek, key) == 0)) {
          Py_DECREF(key);
          Py_DECREF(ep[ix].me_value);
          ep[ix].me_value = val;
          break;
        }
        perturb >>= 5;
        idx = (idx * 5u + 1u + perturb) & mask;
      }
    }
  } else if (log2_size <= 15) {
    int16_t *indices = (int16_t *)dk->dk_indices;
    for (size_t j = 0; j < count; j++) {
      PyObject *key = kvs[j].key;
      PyObject *val = kvs[j].val;
      Py_hash_t kh = kvs[j].hash;
      if (!has_gc && PyObject_IS_GC(val)) has_gc = 1;
      size_t perturb = (size_t)kh;
      size_t idx = perturb & mask;
      for (;;) {
        int16_t ix = indices[idx];
        if (CJSON_LIKELY(ix == -1)) {
          indices[idx] = (int16_t)nentries;
          ep[nentries].me_key = key;
          ep[nentries].me_value = val;
          nentries++;
          break;
        }
        PyObject *ek = ep[ix].me_key;
        if (CJSON_UNLIKELY(ek == key || PyUnicode_Compare(ek, key) == 0)) {
          Py_DECREF(key);
          Py_DECREF(ep[ix].me_value);
          ep[ix].me_value = val;
          break;
        }
        perturb >>= 5;
        idx = (idx * 5u + 1u + perturb) & mask;
      }
    }
  } else {
    int32_t *indices = (int32_t *)dk->dk_indices;
    for (size_t j = 0; j < count; j++) {
      PyObject *key = kvs[j].key;
      PyObject *val = kvs[j].val;
      Py_hash_t kh = kvs[j].hash;
      if (!has_gc && PyObject_IS_GC(val)) has_gc = 1;
      size_t perturb = (size_t)kh;
      size_t idx = perturb & mask;
      for (;;) {
        int32_t ix = indices[idx];
        if (CJSON_LIKELY(ix == -1)) {
          indices[idx] = (int32_t)nentries;
          ep[nentries].me_key = key;
          ep[nentries].me_value = val;
          nentries++;
          break;
        }
        PyObject *ek = ep[ix].me_key;
        if (CJSON_UNLIKELY(ek == key || PyUnicode_Compare(ek, key) == 0)) {
          Py_DECREF(key);
          Py_DECREF(ep[ix].me_value);
          ep[ix].me_value = val;
          break;
        }
        perturb >>= 5;
        idx = (idx * 5u + 1u + perturb) & mask;
      }
    }
  }

  dk->dk_nentries = nentries;
  dk->dk_usable = usable - nentries;
  PyDictObject *mp = (PyDictObject *)dict;
  mp->ma_keys = (PyDictKeysObject *)dk;
  mp->ma_values = NULL;
  mp->ma_used = nentries;
  if (has_gc && !PyObject_GC_IsTracked(dict)) {
    PyObject_GC_Track(dict);
  }
  return dict;
}
#endif

/* 32-byte predicted key entry (2 entries per 64-byte L1 cache line).
 * Only populated with clean printable ASCII keys (1..64B, no '"', '\\', or < 0x20).
 * For klen <= 7: `w0` holds key bytes + closing '"' (zero-padded), `mask_shift = (7 - klen) * 8`.
 * For 8 <= klen <= 64: `w0` holds bytes 0..7 of key; matching `w0`, `p[klen] == '"'`, and
 *   tail bytes against `PyUnicode_1BYTE_DATA(key)` guarantees exact key + closing quote.
 * `val_kind`: 0 = str/other, 1 = number, 2 = dict, 3 = list, 4 = bool/null. */
typedef struct {
  PyObject *key;
  Py_hash_t py_hash;
  uint64_t w0;
  uint8_t klen;
  uint8_t mask_shift;
  uint8_t val_kind;
  uint8_t _pad[5];
} SchemaKey;

typedef struct {
  uint64_t fused_w0;
  uint64_t fused_mask;
} SchemaFused;

typedef struct {
  uint32_t len;
  uint8_t stable_hits;
  uint8_t miss_streak;
  uint8_t _pad[2];
#if defined(CJSON_SPLIT_DICT_FAST)
  CJsonDictKeysObject *shared_keys;
  uint8_t values_prefix[32];
  uint8_t prefix_size;
  uint8_t split_ok;
  SchemaFused fused[CJSON_SPLIT_MAX_KEYS];
#endif
  SchemaKey keys[CJSON_SCHEMA_MAX_KEYS];
} ObjectSchema;

static ObjectSchema g_schemas[CJSON_SCHEMA_SLOTS] __attribute__((aligned(64)));

static inline int is_clean_schema_ascii(const char *s, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (CJSON_UNLIKELY(c < 0x20 || c >= 0x80 || c == '"' || c == '\\')) return 0;
  }
  return 1;
}

static inline uint8_t schema_val_kind(PyObject *v) {
  PyTypeObject *tp = Py_TYPE(v);
  if (tp == &PyUnicode_Type) return 0;
  if (tp == &PyLong_Type || tp == &PyFloat_Type) return 1;
  if (tp == &PyDict_Type) return 2;
  if (tp == &PyList_Type) return 3;
  return 4;
}

__attribute__((always_inline))
static inline PyObject *schema_match_fast(
    const SchemaKey *sk, const char **p_io, const char *end, Py_hash_t *out_hash) {
  const char *p = *p_io;
  uint32_t klen = sk->klen;
  if (CJSON_LIKELY(klen <= 7)) {
    if (CJSON_LIKELY(klen != 0 && p + 8 <= end)) {
      uint64_t w;
      memcpy(&w, p, 8);
      uint64_t mask = (~0ULL) >> sk->mask_shift;
      if (CJSON_LIKELY(((w ^ sk->w0) & mask) == 0)) {
        *p_io = p + klen + 1;
        *out_hash = sk->py_hash;
        Py_INCREF(sk->key);
        return sk->key;
      }
    }
    return NULL;
  }
  if (CJSON_LIKELY(p + klen + 1 <= end)) {
    uint64_t w;
    memcpy(&w, p, 8);
    if (CJSON_LIKELY(w == sk->w0 && p[klen] == '"')) {
      if (klen == 8) {
        *p_io = p + 9;
        *out_hash = sk->py_hash;
        Py_INCREF(sk->key);
        return sk->key;
      }
      if (klen <= 16) {
        uint64_t tp, tk;
        memcpy(&tp, p + klen - 8, 8);
        memcpy(&tk, (const char *)PyUnicode_1BYTE_DATA(sk->key) + klen - 8, 8);
        if (CJSON_LIKELY(tp == tk)) {
          *p_io = p + klen + 1;
          *out_hash = sk->py_hash;
          Py_INCREF(sk->key);
          return sk->key;
        }
      } else if (memcmp(p + 8, (const char *)PyUnicode_1BYTE_DATA(sk->key) + 8, klen - 8) == 0) {
        *p_io = p + klen + 1;
        *out_hash = sk->py_hash;
        Py_INCREF(sk->key);
        return sk->key;
      }
    }
  }
  return NULL;
}

__attribute__((noinline))
static void schema_record_from_dict(ObjectSchema *sc, PyObject *dict) {
  PyObject *k, *v;
  Py_ssize_t pos = 0;
  uint32_t i = 0;
  while (i < CJSON_SCHEMA_MAX_KEYS && PyDict_Next(dict, &pos, &k, &v)) {
    SchemaKey *sk = &sc->keys[i];
    uint8_t vk = schema_val_kind(v);
    if (sk->key == k && sk->klen != 0) {
      sk->val_kind = vk;
      i++;
      continue;
    }
    if (PyUnicode_IS_COMPACT_ASCII(k)) {
      Py_ssize_t len = PyUnicode_GET_LENGTH(k);
      if (len >= 1 && len <= 64) {
        const char *kd = (const char *)PyUnicode_1BYTE_DATA(k);
        if (is_clean_schema_ascii(kd, (uint32_t)len)) {
          uint64_t w0 = 0;
          uint8_t shift = 0;
          if (len <= 7) {
            memcpy(&w0, kd, (size_t)len);
            w0 |= ((uint64_t)(uint8_t)'"') << ((uint32_t)len * 8u);
            shift = (uint8_t)((7u - (uint32_t)len) * 8u);
          } else {
            memcpy(&w0, kd, 8);
          }
          Py_hash_t kh = PyObject_Hash(k);
          if (CJSON_LIKELY(kh != -1)) {
            Py_INCREF(k);
            PyObject *old = sk->key;
            sk->key = k;
            sk->py_hash = kh;
            sk->w0 = w0;
            sk->klen = (uint8_t)len;
            sk->mask_shift = shift;
            sk->val_kind = vk;
            Py_XDECREF(old);
            i++;
            continue;
          }
          PyErr_Clear();
        }
      }
    }
    PyObject *old = sk->key;
    sk->key = NULL;
    sk->klen = 0;
    sk->val_kind = 0;
    Py_XDECREF(old);
    i++;
  }
  for (uint32_t j = i; j < sc->len; j++) {
    PyObject *old = sc->keys[j].key;
    sc->keys[j].key = NULL;
    sc->keys[j].klen = 0;
    sc->keys[j].val_kind = 0;
    Py_XDECREF(old);
  }
  sc->len = i;
  sc->stable_hits = 0;

#if defined(CJSON_SPLIT_DICT_FAST)
  if (sc->shared_keys != NULL) {
    cjson_free_shared_keys(sc->shared_keys);
    sc->shared_keys = NULL;
  }
  sc->split_ok = 0;
#endif
}

#if defined(CJSON_SPLIT_DICT_FAST)
/* Promote a schema to shared-key split-table mode only after stability hysteresis
 * (2 consecutive full matches), preventing wasted allocations on polymorphic/shuffled dicts. */
__attribute__((noinline))
static void schema_promote_to_split(ObjectSchema *sc) {
  uint32_t n = sc->len;
  if (n < 1 || n > CJSON_SPLIT_MAX_KEYS || sc->split_ok) return;
  PyObject *skeys[CJSON_SPLIT_MAX_KEYS];
  Py_hash_t shashes[CJSON_SPLIT_MAX_KEYS];
  for (uint32_t j = 0; j < n; j++) {
    if (sc->keys[j].klen == 0 || sc->keys[j].key == NULL) return;
    skeys[j] = sc->keys[j].key;
    shashes[j] = sc->keys[j].py_hash;
  }
  CJsonDictKeysObject *dk = cjson_build_shared_keys(skeys, shashes, n);
  if (dk == NULL) return;
#if defined(CJSON_SPLIT_DICT_313)
  uint8_t psize = (uint8_t)n;
  for (uint32_t j = 0; j < n; j++) {
    sc->values_prefix[j] = (uint8_t)j;
#else
  uint8_t psize = (uint8_t)((2u + n + 7u) & ~7u);
  memset(sc->values_prefix, 0, psize);
  sc->values_prefix[psize - 1] = psize;
  sc->values_prefix[psize - 2] = (uint8_t)n;
  for (uint32_t j = 0; j < n; j++) {
    sc->values_prefix[psize - 3 - j] = (uint8_t)j;
#endif
    uint8_t kl = sc->keys[j].klen;
    if (kl <= 5) {
      const char *kd = (const char *)PyUnicode_1BYTE_DATA(sc->keys[j].key);
      char tmp[8] = {0};
      tmp[0] = ',';
      tmp[1] = '"';
      memcpy(tmp + 2, kd, kl);
      tmp[2 + kl] = '"';
      if (kl <= 4) {
        tmp[3 + kl] = ':';
        uint32_t total_b = (uint32_t)kl + 4u;
        sc->fused[j].fused_mask = (total_b == 8u) ? ~0ULL : ((1ULL << (total_b * 8u)) - 1ULL);
      } else {
        sc->fused[j].fused_mask = ~0ULL;
      }
      memcpy(&sc->fused[j].fused_w0, tmp, 8);
    } else {
      sc->fused[j].fused_w0 = 0;
      sc->fused[j].fused_mask = 0;
    }
  }
  if (sc->shared_keys != NULL) {
    cjson_free_shared_keys(sc->shared_keys);
  }
  sc->prefix_size = psize;
  sc->shared_keys = dk;
  sc->split_ok = 1;
}
#endif

/* 64-byte direct-mapped cache for short non-ASCII UTF-8 dict keys (<= 48 raw UTF-8 bytes). */
typedef struct {
  PyObject *str;
  Py_hash_t py_hash;
  uint64_t hash64;
  uint8_t raw_len;
  char raw[39];
} Utf8KeySlot;

static Utf8KeySlot g_utf8_key_cache[128] __attribute__((aligned(64)));

static inline PyObject *lookup_utf8_key_cache(
    const char *start, uint32_t raw_len, uint64_t *out_h64, Py_hash_t *out_hash) {
  uint64_t h64 = hash_key_bytes64(start, raw_len);
  *out_h64 = h64;
  Utf8KeySlot *s = &g_utf8_key_cache[h64 & 127u];
  if (CJSON_LIKELY(s->str != NULL && s->hash64 == h64 && s->raw_len == (uint8_t)raw_len &&
                   memcmp(s->raw, start, raw_len) == 0)) {
    *out_hash = s->py_hash;
    Py_INCREF(s->str);
    return s->str;
  }
  return NULL;
}

static inline void store_utf8_key_cache(
    const char *start, uint32_t raw_len, uint64_t h64, PyObject *u, Py_hash_t py_h) {
  Utf8KeySlot *s = &g_utf8_key_cache[h64 & 127u];
  PyObject *prev = s->str;
  Py_INCREF(u);
  s->str = u;
  s->py_hash = py_h;
  s->hash64 = h64;
  s->raw_len = (uint8_t)raw_len;
  memcpy(s->raw, start, raw_len);
  Py_XDECREF(prev);
}

#if defined(CJSON_SPLIT_DICT_FAST)
/* Match predicted key WITHOUT Py_INCREF (used in split-table fast path where
 * shared_keys already holds a reference to every key). */
__attribute__((always_inline))
static inline int schema_match_key_only(const SchemaKey *sk, const char **p_io, const char *end) {
  const char *p = *p_io;
  uint32_t klen = sk->klen;
  if (CJSON_LIKELY(klen <= 7)) {
    if (CJSON_LIKELY(klen != 0 && p + 8 <= end)) {
      uint64_t w;
      memcpy(&w, p, 8);
      uint64_t mask = (~0ULL) >> sk->mask_shift;
      if (CJSON_LIKELY(((w ^ sk->w0) & mask) == 0)) {
        *p_io = p + klen + 1;
        return 1;
      }
    }
    return 0;
  }
  if (CJSON_LIKELY(p + klen + 1 <= end)) {
    uint64_t w;
    memcpy(&w, p, 8);
    if (CJSON_LIKELY(w == sk->w0 && p[klen] == '"')) {
      if (klen == 8) {
        *p_io = p + 9;
        return 1;
      }
      if (klen <= 16) {
        uint64_t tp, tk;
        memcpy(&tp, p + klen - 8, 8);
        memcpy(&tk, (const char *)PyUnicode_1BYTE_DATA(sk->key) + klen - 8, 8);
        if (CJSON_LIKELY(tp == tk)) {
          *p_io = p + klen + 1;
          return 1;
        }
      } else if (memcmp(p + 8, (const char *)PyUnicode_1BYTE_DATA(sk->key) + 8, klen - 8) == 0) {
        *p_io = p + klen + 1;
        return 1;
      }
    }
  }
  return 0;
}
#endif
#endif

#else

static inline int key_bytes_equal(const char *a, const char *b, uint32_t len) {
  if (len <= 16) {
    if (len >= 8) {
      uint64_t a1, b1, a2, b2;
      memcpy(&a1, a, 8);
      memcpy(&b1, b, 8);
      memcpy(&a2, a + len - 8, 8);
      memcpy(&b2, b + len - 8, 8);
      return ((a1 ^ b1) | (a2 ^ b2)) == 0;
    }
    if (len >= 4) {
      uint32_t a1, b1, a2, b2;
      memcpy(&a1, a, 4);
      memcpy(&b1, b, 4);
      memcpy(&a2, a + len - 4, 4);
      memcpy(&b2, b + len - 4, 4);
      return ((a1 ^ b1) | (a2 ^ b2)) == 0;
    }
    for (uint32_t i = 0; i < len; i++) {
      if (a[i] != b[i]) return 0;
    }
    return 1;
  }
  return memcmp(a, b, len) == 0;
}

static inline uint64_t wymix64_parts(uint64_t a, uint64_t b, uint32_t len) {
  a ^= 0xA0761D6478BD642FULL;
  b ^= 0xE7037ED1A0B428DBULL ^ ((uint64_t)len * 0x9E3779B97F4A7C15ULL);
#if defined(__SIZEOF_INT128__)
  __uint128_t r = (__uint128_t)a * (__uint128_t)b;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
#else
  uint64_t h = a * b;
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ULL;
  return h ^ (h >> 31);
#endif
}

static inline uint64_t hash_key_bytes64(const char *ptr, uint32_t len) {
  uint64_t a, b;
  if (len >= 8) {
    memcpy(&a, ptr, 8);
    memcpy(&b, ptr + len - 8, 8);
    if (CJSON_UNLIKELY(len > 16)) {
      uint64_t c;
      memcpy(&c, ptr + (len >> 1) - 4, 8);
      a ^= c * 0x94D049BB133111EBULL;
    }
  } else if (len >= 4) {
    uint32_t x, y;
    memcpy(&x, ptr, 4);
    memcpy(&y, ptr + len - 4, 4);
    a = x;
    b = y;
  } else {
    a = ((uint64_t)(uint8_t)ptr[0] << 16) |
        ((uint64_t)(uint8_t)ptr[len >> 1] << 8) |
        (uint64_t)(uint8_t)ptr[len - 1];
    b = len;
  }
  return wymix64_parts(a, b, len);
}

static inline PyObject *lookup_or_create_cached_key_gil_disabled(
    JsonParser *jp, const char *start, uint32_t klen, Py_hash_t *out_hash) {
  if (CJSON_UNLIKELY(klen == 0)) {
    Py_INCREF(g_empty_str);
    *out_hash = g_empty_str_hash;
    return g_empty_str;
  }
  if (klen <= 64 && (jp->end - jp->start) > 48) {
    if (CJSON_UNLIKELY(!jp->cache_initialized)) {
      memset(jp->cache_strs, 0, sizeof(jp->cache_strs));
      jp->cache_initialized = 1;
    }
    uint64_t h64 = hash_key_bytes64(start, klen);
    uint32_t h = (uint32_t)(h64 ^ (h64 >> 32));
    uint32_t slot0 = (uint32_t)(h64 & (uint64_t)(CJSON_KEY_CACHE_SIZE - 2));
    uint32_t slot1 = slot0 + 1;

    PyObject *c0 = jp->cache_strs[slot0];
    if (c0 != NULL && jp->cache_meta[slot0].len == klen &&
        jp->cache_meta[slot0].tag == h &&
        key_bytes_equal(jp->cache_meta[slot0].ptr, start, klen)) {
      *out_hash = jp->cache_meta[slot0].hash;
      Py_INCREF(c0);
      return c0;
    }

    PyObject *c1 = jp->cache_strs[slot1];
    if (c1 != NULL && jp->cache_meta[slot1].len == klen &&
        jp->cache_meta[slot1].tag == h &&
        key_bytes_equal(jp->cache_meta[slot1].ptr, start, klen)) {
      *out_hash = jp->cache_meta[slot1].hash;
      Py_INCREF(c1);
      return c1;
    }

    PyObject *u = make_ascii_str(start, (Py_ssize_t)klen);
    if (CJSON_UNLIKELY(u == NULL)) return NULL;
    Py_hash_t py_h = PyObject_Hash(u);
    *out_hash = py_h;

    uint32_t target = (c0 == NULL) ? slot0 : ((c1 == NULL) ? slot1 : ((h & 1) ? slot1 : slot0));
    PyObject *prev = jp->cache_strs[target];
    if (prev == NULL) {
      jp->used_slots[jp->num_cached++] = (uint16_t)target;
    } else {
      Py_DECREF(prev);
    }
    Py_INCREF(u);
    jp->cache_strs[target] = u;
    jp->cache_meta[target].ptr = start;
    jp->cache_meta[target].len = klen;
    jp->cache_meta[target].tag = h;
    jp->cache_meta[target].hash = py_h;
    return u;
  }

  PyObject *u = make_ascii_str(start, (Py_ssize_t)klen);
  if (CJSON_LIKELY(u != NULL)) {
    *out_hash = PyObject_Hash(u);
  }
  return u;
}

#endif /* Py_GIL_DISABLED */

#endif /* CJSON_KEY_CACHE_H */
