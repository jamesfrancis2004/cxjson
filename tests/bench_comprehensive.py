"""Comprehensive benchmark & memory verification: cxjson vs orjson across 28 datasets.

Measures:
  1. Serialization throughput (dumps) across 28 real-world JSON datasets.
  2. Deserialization throughput (loads) across the same 28 datasets.
  3. Peak/retained memory usage (tracemalloc) and exact PyBytes sizeof parity.
"""
import gc
import json
import os
import platform
import statistics
import sys
import time
import tracemalloc

import cxjson
import orjson

DATASETS = [
    ("canada.json", 50),
    ("citm_catalog.json", 100),
    ("twitter.json", 150),
    ("github.json", 800),
    ("github_events.json", 800),
    ("apache_builds.json", 400),
    ("mesh.json", 80),
    ("marine_ik.json", 50),
    ("instruments.json", 250),
    ("google_maps.json", 800),
    ("gsoc-2018.json", 40),
    ("numbers.json", 200),
    ("random.json", 100),
    ("update-center.json", 100),
    ("twitterescaped.json", 100),
    ("tree-pretty.json", 400),
    ("npm_lodash.json", 150),
    ("pokedex.json", 400),
    ("semanticscholar-corpus.json", 15),
    ("countries.geo.json", 8),
    ("nobel_prizes.json", 200),
    ("pypi_requests.json", 250),
    ("pypi_pandas.json", 40),
    ("pypi_numpy.json", 30),
    ("usgs_earthquakes_week.json", 50),
    ("usgs_earthquakes_month.json", 15),
    ("npm_react.json", 15),
    ("npm_typescript.json", 8),
]


def bench_pair(fn_c, fn_o, arg, byte_len, iters):
    # Warm up both implementations (>= 0.04s and >= 3 iterations)
    for fn in (fn_c, fn_o):
        w = 0
        wt0 = time.perf_counter()
        while w < 3 or (time.perf_counter() - wt0) < 0.04:
            fn(arg)
            w += 1

    # Calibrate batch count so one round takes ~0.025s
    cal = max(1, min(iters, 20))
    t0 = time.perf_counter()
    for _ in range(cal):
        fn_c(arg)
    dt = max(time.perf_counter() - t0, 1e-6)
    batch = max(1, int(cal * (0.025 / dt)))

    c_times = []
    o_times = []
    gc.collect()
    gc.disable()
    try:
        for r in range(9):
            if r % 2 == 0:
                t0 = time.perf_counter()
                for _ in range(batch):
                    fn_c(arg)
                tc = time.perf_counter() - t0
                t0 = time.perf_counter()
                for _ in range(batch):
                    fn_o(arg)
                to = time.perf_counter() - t0
            else:
                t0 = time.perf_counter()
                for _ in range(batch):
                    fn_o(arg)
                to = time.perf_counter() - t0
                t0 = time.perf_counter()
                for _ in range(batch):
                    fn_c(arg)
                tc = time.perf_counter() - t0
            c_times.append(tc)
            o_times.append(to)
    finally:
        gc.enable()

    scale = (batch * byte_len) / (1024 * 1024)
    return scale / statistics.median(c_times), scale / statistics.median(o_times)


def run_suite(label, mode, items):
    print()
    print("=" * 84)
    print(f" {label} ({mode}) ".center(84, "="))
    print("=" * 84)
    print(f"{'Dataset':34s} | {'cxjson':>11s} | {'orjson':>11s} | {'Ratio':>8s} | Status")
    print("-" * 84)

    wins = 0
    geo = 1.0
    for name, fn_c, fn_o, arg, byte_len, iters in items:
        c_mb, o_mb = bench_pair(fn_c, fn_o, arg, byte_len, iters)
        ratio = c_mb / o_mb
        geo *= ratio
        if ratio >= 1.0:
            wins += 1
            status = "WIN"
        else:
            status = "LOSS <--"
        print(f"{name:34s} | {c_mb:8.1f} M/s | {o_mb:8.1f} M/s | {ratio:7.2f}x | {status}")

    geo **= 1.0 / len(items)
    print("-" * 84)
    print(f"Summary: {wins}/{len(items)} wins ({wins / len(items) * 100:.1f}%), geo-mean = {geo:.2f}x")
    return wins, geo


def measure_mem(fn, arg):
    gc.collect()
    tracemalloc.start()
    cur0, _ = tracemalloc.get_traced_memory()
    tracemalloc.reset_peak()
    res = fn(arg)
    cur1, peak1 = tracemalloc.get_traced_memory()
    out_sizeof = sys.getsizeof(res)
    del res
    gc.collect()
    cur2, _ = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    return peak1 - cur0, cur1 - cur0, cur2 - cur0, out_sizeof


def main():
    mode = os.environ.get("CJSON_BENCH_MODE", f"{platform.machine()} / Python {platform.python_version()}")
    data_dir = os.path.join(os.path.dirname(__file__), "..", "data")

    dumps_items = []
    loads_items = []
    loaded_cache = {}
    for name, iters in DATASETS:
        raw = open(os.path.join(data_dir, name), "rb").read()
        obj = orjson.loads(raw)
        out_len = len(orjson.dumps(obj))
        loaded_cache[name] = (raw, obj)
        dumps_items.append((name, cxjson.dumps, orjson.dumps, obj, out_len, iters))
        loads_items.append((name, cxjson.loads, orjson.loads, raw, len(raw), iters))

    run_suite("DUMPS (Serialization)", mode, dumps_items)
    run_suite("LOADS (Deserialization)", mode, loads_items)

    print()
    print("=" * 84)
    print(f" MEMORY USAGE COMPARISON ({mode}) ".center(84, "="))
    print("=" * 84)
    print(f"{'Dataset':30s} | {'Op':5s} | {'cxjson peak':>11s} | {'orjson peak':>11s} | {'sizeof eq':>9s}")
    print("-" * 84)
    mem_names = ["canada.json", "citm_catalog.json", "twitter.json", "gsoc-2018.json", "semanticscholar-corpus.json"]
    for name in mem_names:
        raw, obj = loaded_cache[name]
        cp_d, _, cr_d, cs_d = measure_mem(cxjson.dumps, obj)
        op_d, _, or_d, os_d = measure_mem(orjson.dumps, obj)
        eq = "YES" if cs_d == os_d else f"NO ({cs_d} vs {os_d})"
        print(f"{name:30s} | dumps | {cp_d/1024:8.1f} KB | {op_d/1024:8.1f} KB | {eq:>9s}")

        cp_l, _, cr_l, _ = measure_mem(cxjson.loads, raw)
        op_l, _, or_l, _ = measure_mem(orjson.loads, raw)
        print(f"{name:30s} | loads | {cp_l/1024:8.1f} KB | {op_l/1024:8.1f} KB | {'N/A':>9s}")


if __name__ == "__main__":
    main()
