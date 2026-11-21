import gc
import json
import os
import sys
import time

# Target libraries
import cxjson
import orjson
import msgspec
import ujson
import rapidjson
import simplejson


def load_dataset(filename):
    filepath = os.path.join(os.path.dirname(__file__), "..", "data", filename)
    with open(filepath, "r", encoding="utf-8") as f:
        return json.load(f)


def time_func(fn, arg, duration_sec=1.0, warmup_sec=0.2):
    # Warmup
    end_warmup = time.perf_counter() + warmup_sec
    while time.perf_counter() < end_warmup:
        fn(arg)

    # Timed run
    gc.disable()
    iters = 0
    t0 = time.perf_counter()
    end_time = t0 + duration_sec
    while True:
        fn(arg)
        iters += 1
        now = time.perf_counter()
        if now >= end_time:
            break
    elapsed = now - t0
    gc.enable()
    return iters / elapsed


def main():
    print("=" * 115)
    print(" INDUSTRY-STANDARD JSON SERIALIZATION BENCHMARK ".center(115, "="))
    print(" Comparing: cxjson vs orjson vs msgspec vs ujson vs rapidjson vs simplejson vs std json".center(115))
    print("=" * 115)
    print()

    # Load standard real-world datasets
    datasets = {
        "canada.json (2.25 MB Floats)": load_dataset("canada.json"),
        "citm_catalog.json (1.73 MB Web)": load_dataset("citm_catalog.json"),
        "twitter.json (632 KB Strings)": load_dataset("twitter.json"),
        "github.json (56 KB Mixed)": load_dataset("github.json"),
        "10k Floats Array": [float(f"{i * 1.337:.4f}") for i in range(10_000)],
        "10k Ints Array": [i * 17 - 5000 for i in range(10_000)],
        "1,000 Dict Records": [
            {
                "id": i,
                "name": f"User_{i}",
                "email": f"user_{i}@example.com",
                "active": i % 2 == 0,
                "score": i * 0.75,
                "tags": ["admin", "staff", "verified"] if i % 5 == 0 else ["user"],
            }
            for i in range(1000)
        ],
    }

    libs = {
        "cxjson": cxjson.dumps,
        "orjson": orjson.dumps,
        "msgspec": msgspec.json.encode,
        "ujson": ujson.dumps,
        "rapidjson": rapidjson.dumps,
        "simplejson": simplejson.dumps,
        "std json": json.dumps,
    }

    for name, data in datasets.items():
        # Compute serialized byte size using cxjson
        sample_bytes = cxjson.dumps(data)
        size_bytes = len(sample_bytes)
        size_kb = size_bytes / 1024.0

        print(f"▶ Dataset: {name} (Payload size: {size_kb:,.1f} KB)")
        print(f"  {'-' * 95}")

        results = {}
        for lib_name, lib_fn in libs.items():
            ops_sec = time_func(lib_fn, data, duration_sec=0.8, warmup_sec=0.15)
            mb_sec = (ops_sec * size_bytes) / (1024.0 * 1024.0)
            results[lib_name] = (ops_sec, mb_sec)

        c_ops, c_mb = results["cxjson"]
        o_ops, o_mb = results["orjson"]

        # Sort libraries by ops/sec descending
        sorted_results = sorted(results.items(), key=lambda x: x[1][0], reverse=True)

        for rank, (lib_name, (ops, mb)) in enumerate(sorted_results, 1):
            ratio_vs_c = ops / c_ops
            ratio_str = f"({ratio_vs_c:.2f}x of cxjson)" if lib_name != "cxjson" else "(BASELINE)"
            star = " ★ [FASTEST]" if rank == 1 else ""
            print(f"  {rank}. {lib_name:<11} : {ops:>10,.1f} ops/sec | {mb:>8,.1f} MB/s {ratio_str:<20}{star}")

        lead_pct = ((c_ops / o_ops) - 1.0) * 100.0
        if lead_pct >= 0:
            print(f"  👉 cxjson is {lead_pct:+.1f}% FASTER than orjson\n")
        else:
            print(f"  👉 orjson is {-lead_pct:+.1f}% faster than cxjson\n")


if __name__ == "__main__":
    main()
