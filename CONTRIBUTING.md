# Contributing to cxjson

Thank you for your interest in contributing to **cxjson**! Contributions of all kinds—performance improvements, platform/Python-version support, bug fixes, tests, and documentation—are welcome.

## Development Setup

```bash
git clone https://github.com/jamesfrancis2004/cxjson.git
cd cxjson
make build
make test
```

### Requirements
- Python 3.12+ (3.9+ supported via public C-API fallback)
- A C11 compiler (`clang`, `gcc`, or `MSVC`)
- `orjson` installed in your dev environment if you want to run the differential compatibility, fuzz, and benchmark suites

## Repository Layout

- `src/c/_cxjson_module.c` — CPython extension entrypoint & module initialization
- `src/c/serializer.c` — `dumps` / `dumps_str` serializer (`zmij` + `jeaiii` + SIMD string escaping)
- `src/c/deserializer.c` — `loads` parser (SIMD scanning, schema prediction, split-table dicts)
- `src/c/cjson_simd.c` / `cjson_simd.h` — NEON, AVX2, and SSE2 kernels + x86_64 runtime CPUID dispatch
- `src/c/key_cache.h` — L1 key/value string caches & CPython 3.12–3.14 split-table dict builders
- `src/c/numparse.h` — SWAR + Clinger + Eisel-Lemire IEEE-754 float parser
- `src/c/parallel_ser.h` — Adaptive cross-platform thread pool + `pthread_atfork()` child reset
- `src/c/utf8.h` — SIMD UTF-8 validation & transcoding
- `src/cxjson/` — Python wrapper package & PEP 561 type stubs (`__init__.pyi`, `py.typed`)
- `tests/` — Unit tests, `orjson` compatibility tests, RFC 8259 conformance & 13,000-case fuzz tests, and benchmarks

## Guidelines for Pull Requests

1. **Zero compiler warnings:** All C changes must compile cleanly under `-Wall -Wextra -Wno-unused-parameter -Werror`.
2. **Keep it pure C with zero external dependencies:** Avoid introducing C++ dependencies or third-party runtime libraries.
3. **Guard CPython internal struct access:** Any version-specific CPython struct access (such as split-table dicts or `PyLongObject` digits) must be guarded with `PY_VERSION_HEX` and `!defined(Py_GIL_DISABLED)` with a clean public C-API fallback.
4. **Verify correctness & fuzzing:** Run the full test suite before opening a PR:
   ```bash
   make test
   ```
5. **Benchmark performance changes:** For optimization PRs, include before/after numbers from `tests/bench_comprehensive.py` (or a targeted benchmark) showing both throughput and peak memory impact.

## Code of Conduct

Be respectful, constructive, and welcoming to all contributors.
