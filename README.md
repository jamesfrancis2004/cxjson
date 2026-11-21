# cxjson

> **A zero-dependency pure-C JSON library for Python built to rival and beat `orjson`—with runtime-dispatched SIMD (NEON / SSE2 / SSE4.1 / AVX2 / AVX-512BW), CPython split-table dicts, direct PEP 3118 NumPy serialization, and adaptive multi-threaded `dumps`.**

[![CI](https://github.com/jamesfrancis2004/cxjson/actions/workflows/ci.yml/badge.svg)](https://github.com/jamesfrancis2004/cxjson/actions/workflows/ci.yml)
[![Python 3.12+](https://img.shields.io/badge/python-3.12%20|%203.13%20|%203.14-blue.svg)](#)
[![Pure C](https://img.shields.io/badge/implementation-pure%20C%20%2B%20SIMD-orange.svg)](#)
[![RFC 8259](https://img.shields.io/badge/RFC%208259-strict%20conformance-brightgreen.svg)](#)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

---

## The Challenge

For years, [`orjson`](https://github.com/ijl/orjson) (written in Rust) has been the undisputed speed king of Python JSON libraries. Every time someone asks *"how do I make JSON faster in Python?"*, the answer is always *"just use `orjson`."*

I wanted to see what would happen if I tasked Gemini with building a zero-dependency pure-C Python extension from scratch—with one simple goal: **beat `orjson` across real-world JSON workloads, without cheating on RFC 8259 correctness, memory safety, or API compatibility.**

The result is **`cxjson`**.

### Headline Results (Apple M2, Python 3.12.3, `orjson 3.13.0`, GC On)

- **1.84× faster `dumps`** and **1.50× faster `loads`** (geo-mean vs `orjson 3.13.0`, interleaved median of 9 runs with Python GC enabled across 8 real-world datasets)
- **Up to 25% less retained memory** after `loads()` on repeated-schema JSON payloads (`tracemalloc`, via CPython split-table dicts)
- **5+ GB/s** adaptive multi-threaded `dumps` on large payloads and **~0.9–1.7 GB/s** `loads`
- **2.46× faster NumPy serialization** (`OPT_SERIALIZE_NUMPY`, geo-mean across 7 dtypes/shapes on Apple M2; **~1.9×** on single-core x86_64)
- **1.45× faster `OPT_SORT_KEYS`** than `orjson` via 64-bit prefix-keyed inline sorting

---

## Python JSON Library Benchmarks (Apple M2 / ARM64)

Measured locally on an **Apple M2 (ARM64)** with **Python 3.12.3** and **`orjson 3.13.0`** (interleaved order, GC enabled, median of 9 rounds) across 8 representative real-world JSON datasets (`twitter.json`, `citm_catalog.json`, `canada.json`, `gsoc-2018.json`, `usgs_earthquakes_week.json`, `pypi_numpy.json`, `semanticscholar-corpus.json`, `npm_typescript.json`).

> **Note on x86_64 & single-core environments:** `cxjson` includes runtime-dispatched SSE2, SSE4.1 (`zmij` float formatting), AVX2, and AVX-512BW kernels for x86_64. On single-core x86_64 containers (where adaptive multi-threaded `dumps` does not activate), `cxjson` is typically ~1.0×–1.2× vs `orjson` on `loads` and ~1.0× on `dumps` (and ~1.9× on NumPy). Multi-core machines see the full adaptive `dumps` speedup (configurable via `CXJSON_MAX_THREADS`).

#### Serialization (`dumps`) — MB/s (higher is better, Apple M2, Python 3.12.3, GC On)

| Dataset | Size | **`cxjson`** | `orjson 3.13.0` | Speedup |
|---|---:|---:|---:|---:|
| `twitter.json` | 0.45 MB | **2,829** | 2,231 | **1.27×** |
| `citm_catalog.json` | 0.48 MB | **1,805** | 1,299 | **1.39×** |
| `canada.json` | 1.99 MB | **2,147** | 1,097 | **1.96×** |
| `gsoc-2018.json` | 2.93 MB | **3,190** | 2,973 | **1.07×** |
| `usgs_earthquakes_week.json` | 1.28 MB | **3,477** | 1,192 | **2.92×** |
| `pypi_numpy.json` | 3.61 MB | **2,487** | 2,107 | **1.18×** |
| `semanticscholar-corpus.json` | 8.19 MB | **5,332** | 1,752 | **3.04×** |
| `npm_typescript.json` | 15.22 MB | **5,179** | 1,506 | **3.44×** |
| **Geo-Mean** | — | — | — | **1.84×** |

#### Deserialization (`loads`) — MB/s (higher is better, Apple M2, Python 3.12.3, GC On)

| Dataset | Size | **`cxjson`** | `orjson 3.13.0` | Speedup |
|---|---:|---:|---:|---:|
| `twitter.json` | 0.60 MB | **889** | 694 | **1.28×** |
| `citm_catalog.json` | 1.65 MB | **839** | 704 | **1.19×** |
| `canada.json` | 2.15 MB | **388** | 348 | **1.12×** |
| `gsoc-2018.json` | 3.17 MB | **1,669** | 1,108 | **1.51×** |
| `usgs_earthquakes_week.json` | 1.28 MB | **737** | 350 | **2.11×** |
| `pypi_numpy.json` | 3.61 MB | **1,202** | 658 | **1.83×** |
| `semanticscholar-corpus.json` | 8.20 MB | **673** | 410 | **1.64×** |
| `npm_typescript.json` | 15.22 MB | **622** | 388 | **1.60×** |
| **Geo-Mean** | — | — | — | **1.50×** |

---

## Retained Memory Footprint

When parsing arrays of objects with repeated schemas (like REST API payloads or catalogs), `cxjson` builds CPython split-table dicts sharing a single `PyDictKeysObject` across objects—cutting retained Python heap memory (`tracemalloc.get_traced_memory()` after `loads()`, Python 3.12.3) by up to **25%**:

| Dataset | `cxjson` Retained `loads` | `orjson 3.13.0` Retained `loads` | Reduction |
|---|---:|---:|---:|
| `twitter.json` | **0.86 MB** | 1.01 MB | **1.18× less** |
| `citm_catalog.json` | **2.31 MB** | 3.08 MB | **1.34× less** |
| `gsoc-2018.json` | **4.43 MB** | 4.88 MB | **1.10× less** |
| `canada.json` | **6.81 MB** | 6.81 MB | **1.00×** |
| `semanticscholar-corpus.json` | **17.34 MB** | 23.10 MB | **1.33× less** |

---

## NumPy Serialization (`OPT_SERIALIZE_NUMPY`)

`cxjson` serializes NumPy arrays directly from their raw C memory via Python's PEP 3118 buffer protocol—**zero NumPy build dependency** (works across NumPy 1.x and 2.x) and supports `int16`/`uint16` plus non-contiguous strided/transposed views.

| NumPy Array (Apple M2, NumPy 2.1.2) | **`cxjson`** | `orjson` | Speedup |
|---|---:|---:|---:|
| `float64` (100K 1D) | **2,061 MB/s** | 822 MB/s | **2.51×** |
| `float64` (500×200 2D) | **2,060 MB/s** | 804 MB/s | **2.56×** |
| `float32` (100K 1D) | **1,407 MB/s** | 446 MB/s | **3.16×** |
| `int64` (100K 1D) | **1,747 MB/s** | 1,476 MB/s | **1.18×** |
| `int32` (100K 1D) | **1,410 MB/s** | 1,262 MB/s | **1.12×** |
| `uint8` (200K 1D) | **7,551 MB/s** | 968 MB/s | **7.80×** |
| `bool` (200K 1D) | **8,631 MB/s** | 4,130 MB/s | **2.09×** |
| **Geo-Mean** | — | — | **2.38×** |

---

## How Is `cxjson` This Fast?

There is no magic—just relentless elimination of per-character branches, redundant copies, and CPython object allocations:

1. **Runtime-Dispatched SIMD (ARM64 NEON, x86_64 SSE2 / SSE4.1 / AVX2 / AVX-512BW):** String scanning, quote/escape detection, UTF-8 ASCII runs, Latin-1 narrowing, and `zmij` SSE4.1 float formatting dispatch at import time via CPUID so a single wheel runs optimally on any CPU.
2. **CPython Split-Table Dicts (Python 3.12, 3.13, & 3.14):** When parsing arrays of objects with repeated schemas (like REST API responses or GeoJSON features), `cxjson` learns the key schema and builds CPython split-table dicts sharing a single `PyDictKeysObject` (verified by a startup self-check and full GC-tracking parity). This eliminates per-dict hash table allocations and reduces retained memory by up to **25%** on repeated-schema workloads.
3. **Key & Value-String Caching:** L1-resident direct-mapped/2-way caches for ASCII dict keys (`1..64B`), UTF-8 keys, and repeated ASCII string values (`2..48B`), plus singletons for `""`, 1-char ASCII strings, and small integers.
4. **State-of-the-Art Number Formatting & Parsing:** Vendored [`zmij`](src/c/zmij) (float → string) and [`jeaiii`](src/c/jeaiii) (int → string) for serialization, paired with 8-digit SWAR mantissa parsing + Clinger's fast path + Eisel-Lemire for bit-exact IEEE-754 float parsing.
5. **Adaptive Multi-Threaded `dumps` (with `pthread_atfork()` Safety):** For payloads ≥ 512 KB of pure built-in types, `cxjson` scales worker threads by payload size while holding the GIL (workers read immutable Python memory without touching refcounts or C-API). It is safe across `os.fork()`, automatically disabled on free-threaded Python (`Py_GIL_DISABLED`), and configurable via `CXJSON_MAX_THREADS=1`.

---

## Safety & Correctness

Speed means nothing if a parser crashes on untrusted input or silently accepts invalid JSON:

- **Strict RFC 8259 Conformance:** Rejects trailing commas, leading zeros, invalid UTF-8, lone surrogates, unescaped control characters, `NaN`/`Infinity` (by default), and trailing garbage—verified against 44 valid + 67 invalid RFC 8259 test cases matching `orjson`.
- **Fuzz Tested:** Verified against **10,000 mutated-byte fuzz inputs** and **3,000 randomized structured round-trips** matching `orjson` 100%.
- **Version-Guarded CPython Internals:** Split-table dicts and direct `PyLongObject` digit reads are strictly `#if`-guarded by `PY_VERSION_HEX` and `!Py_GIL_DISABLED`, falling back cleanly to the public CPython C-API on unsupported versions or free-threaded builds.
- **Zero External Dependencies:** 100% pure C, zero C++ runtime linkage, zero warnings under `-Wall -Wextra -Werror`.

---

## Quickstart & Usage

### Build & Install Locally

```bash
git clone https://github.com/jamesfrancis2004/cxjson.git
cd cxjson
pip install -e .
```

### Basic API

```python
import cxjson

# Serialize to UTF-8 bytes (fastest, drop-in compatible with orjson.dumps)
payload: bytes = cxjson.dumps({"hello": "world", "nums": [1, 2, 3.14]})
# b'{"hello":"world","nums":[1,2,3.14]}'

# Deserialize from bytes, bytearray, memoryview, or str
obj = cxjson.loads(payload)

# Serialize to str (like stdlib json.dumps)
text: str = cxjson.dumps_str(obj)
```

### Drop-In FastAPI / Starlette Response

```python
from starlette.responses import Response
import cxjson

class CXJSONResponse(Response):
    media_type = "application/json"

    def render(self, content) -> bytes:
        return cxjson.dumps(content)
```

> **Tip for multi-worker servers (Uvicorn / Gunicorn):** Set `CXJSON_MAX_THREADS=1` so worker processes don't compete for cores with internal serialization threads—even at 1 thread, `cxjson` serializes at **~2.8 GB/s**.

### Running Tests & Benchmarks

```bash
# Build C extension in-place
make build

# Run serializer, orjson-compatibility, RFC 8259 conformance, fork, and fuzz suites
make test

# Run the 28-dataset benchmark against orjson
PYTHONPATH=src python3 tests/bench_comprehensive.py
```

---

## Project Layout

```text
cxjson/
├── docs/images/              # Benchmark bar charts
├── src/
│   ├── c/
│   │   ├── _cxjson_module.c  # CPython extension entrypoint
│   │   ├── serializer.c      # SIMD + zmij/jeaiii serializer
│   │   ├── deserializer.c    # SIMD + split-table dict deserializer
│   │   ├── cjson_simd.c/h    # NEON / AVX2 / SSE2 kernels + runtime dispatch
│   │   ├── key_cache.h       # Key/value caches & split-table dict builders
│   │   ├── numparse.h        # SWAR + Clinger + Eisel-Lemire number parser
│   │   ├── parallel_ser.h    # Adaptive cross-platform thread pool + atfork
│   │   ├── utf8.h            # SIMD UTF-8 validation & transcoding
│   │   ├── jeaiii/           # Fast integer-to-ASCII
│   │   └── zmij/             # Fast double-to-ASCII
│   └── cxjson/               # Python package, PEP 561 stubs (__init__.pyi, py.typed)
└── tests/                    # Unit, compatibility, RFC 8259 conformance/fuzz, & benchmarks
```

---

## Contributing & License

Contributions are welcome—see [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, C guidelines, and how to run the fuzz/benchmark suites.

Licensed under the [MIT License](LICENSE).

