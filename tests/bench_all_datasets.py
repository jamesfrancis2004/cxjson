import gc
import json
import os
import time

import cxjson
import msgspec
import orjson


def load_json(name):
    path = os.path.join(os.path.dirname(__file__), "..", "data", name)
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def time_lib(fn, data, iters=100):
    # Warmup
    for _ in range(5):
        fn(data)
    gc.collect()
    gc.disable()
    t0 = time.perf_counter()
    for _ in range(iters):
        res = fn(data)
    t1 = time.perf_counter()
    gc.enable()
    elapsed = t1 - t0
    ops_sec = iters / elapsed
    mb_sec = (ops_sec * len(res)) / (1024 * 1024)
    return ops_sec, mb_sec, len(res)


def main():
    datasets = {
        "canada.json (GeoJSON Floats)": ("canada.json", 100),
        "citm_catalog.json (Web Hierarchy)": ("citm_catalog.json", 200),
        "twitter.json (CJK/UTF8 Strings)": ("twitter.json", 300),
        "github.json (REST API)": ("github.json", 2000),
        "github_events.json (Event Stream)": ("github_events.json", 2000),
        "apache_builds.json (CI Logs)": ("apache_builds.json", 1000),
        "mesh.json (3D Vertices)": ("mesh.json", 200),
        "marine_ik.json (Robotics Kinematics)": ("marine_ik.json", 100),
        "instruments.json (Financial)": ("instruments.json", 500),
        "google_maps.json (Navigation API)": ("google_maps.json", 3000),
        "gsoc-2018.json (Rich Project DB)": ("gsoc-2018.json", 60),
        "numbers.json (Mixed Numbers)": ("numbers.json", 500),
        "random.json (Synthetic Schema)": ("random.json", 200),
        "update-center.json (Plugin Index)": ("update-center.json", 200),
        "twitterescaped.json (Escaped Strings)": ("twitterescaped.json", 200),
        "tree-pretty.json (Graph Hierarchy)": ("tree-pretty.json", 1000),
        "npm_lodash.json (NPM Registry Manifest)": ("npm_lodash.json", 400),
        "pokedex.json (Database Records)": ("pokedex.json", 1000),
        "semanticscholar-corpus.json (Academic Corpus)": ("semanticscholar-corpus.json", 20),
        "countries.geo.json (World Boundaries)": ("countries.geo.json", 10),
    }

    results = []

    print("=" * 115)
    print(" COMPREHENSIVE INTERNET JSON BENCHMARK SUITE (20 DATASETS) ".center(115, "="))
    print("=" * 115)
    print(f"{'Dataset':46s} | {'cxjson MB/s':12s} | {'orjson MB/s':12s} | {'msgspec MB/s':12s} | {'vs orjson':12s}")
    print("-" * 115)

    wins = 0

    for label, (filename, iters) in datasets.items():
        data = load_json(filename)
        c_ops, c_mb, payload_len = time_lib(cxjson.dumps, data, iters)
        or_ops, or_mb, _ = time_lib(orjson.dumps, data, iters)
        msg_ops, msg_mb, _ = time_lib(msgspec.json.encode, data, iters)

        ratio = c_mb / or_mb
        lead_str = f"+{(ratio-1)*100:.1f}%" if ratio >= 1.0 else f"-{(1-ratio)*100:.1f}%"
        is_win = ratio >= 1.0
        if is_win:
            wins += 1
        star = "★ [WIN]" if is_win else "[orjson]"

        print(f"{label:46s} | {c_mb:10.1f} MB/s | {or_mb:10.1f} MB/s | {msg_mb:10.1f} MB/s | {ratio:5.2f}x {lead_str:7s} {star}")
        results.append({
            "name": label,
            "filename": filename,
            "cxjson_mb": c_mb,
            "orjson_mb": or_mb,
            "msgspec_mb": msg_mb,
            "ratio": ratio,
            "size_bytes": payload_len,
        })

    print("-" * 115)
    print(f"Summary: cxjson is faster than orjson in {wins}/{len(datasets)} datasets ({wins/len(datasets)*100:.1f}% win rate)!")

    # Save results to json for plotting
    with open("benchmark_expanded_results.json", "w") as f:
        json.dump(results, f, indent=2)
    print("Saved benchmark results to benchmark_expanded_results.json")


if __name__ == "__main__":
    main()
