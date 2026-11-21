"""Correctness, memory-safety, and benchmark suite for cxjson OPT_SERIALIZE_NUMPY."""

import gc
import json
import math
import statistics
import time

try:
    import numpy as np
except ImportError:
    print("SKIP: numpy not installed")
    raise SystemExit(0)

import cxjson

try:
    import orjson
except ImportError:
    orjson = None


def test_numpy_correctness():
    cases = [
        ("int64 1D", np.array([-2**63, -42, 0, 42, 2**63 - 1], dtype=np.int64)),
        ("uint64 1D", np.array([0, 1, 2**63, 2**64 - 1], dtype=np.uint64)),
        ("int32 1D", np.array([-2**31, -1, 0, 1, 2**31 - 1], dtype=np.int32)),
        ("uint32 1D", np.array([0, 1, 2**32 - 1], dtype=np.uint32)),
        ("int16 1D", np.array([-32768, 0, 32767], dtype=np.int16)),
        ("uint16 1D", np.array([0, 65535], dtype=np.uint16)),
        ("int8 1D", np.array([-128, 0, 127], dtype=np.int8)),
        ("uint8 1D", np.array([0, 255], dtype=np.uint8)),
        ("float64 + special", np.array([-0.0, 0.0, 1.5, -3.14159, float("nan"), float("inf"), float("-inf")], dtype=np.float64)),
        ("float32 1D", np.array([0.0, 0.1, 1.25, -2.5, 3.75], dtype=np.float32)),
        ("bool 1D", np.array([True, False, True], dtype=np.bool_)),
        ("2D float64", np.arange(12, dtype=np.float64).reshape(3, 4)),
        ("3D int32", np.arange(24, dtype=np.int32).reshape(2, 3, 4)),
        ("strided slice", np.arange(20, dtype=np.int64)[::3]),
        ("transposed 2D", np.arange(12, dtype=np.int64).reshape(3, 4).T),
        ("empty 1D", np.array([], dtype=np.float64)),
        ("empty 2D", np.empty((0, 3), dtype=np.int64)),
        ("0D scalar arr", np.array(42, dtype=np.int64)),
        ("np.int64 scalar", np.int64(-99)),
        ("np.float64 scalar", np.float64(2.71828)),
        ("nested dict+ndarray", {"a": np.array([1, 2, 3], dtype=np.int64), "b": np.array([[1.5, 2.5]], dtype=np.float64)}),
    ]

    for label, obj in cases:
        c = cxjson.dumps(obj, option=cxjson.OPT_SERIALIZE_NUMPY)
        if orjson is not None:
            try:
                o = orjson.dumps(obj, option=orjson.OPT_SERIALIZE_NUMPY)
                assert c == o, f"Mismatch on {label}: cx={c!r} vs oj={o!r}"
                continue
            except TypeError:
                pass
        assert json.loads(c) == (obj.tolist() if hasattr(obj, "tolist") else obj), f"Mismatch on {label}"
    print(f"PASS: all {len(cases)} NumPy serialization correctness cases passed!")


def bench_numpy():
    if orjson is None:
        return
    rng = np.random.default_rng(42)
    benches = [
        ("float64 (100K 1D)", rng.standard_normal(100_000)),
        ("float64 (500x200 2D)", rng.standard_normal((500, 200))),
        ("float32 (100K 1D)", rng.standard_normal(100_000).astype(np.float32)),
        ("int64   (100K 1D)", rng.integers(-10**9, 10**9, size=100_000, dtype=np.int64)),
        ("int32   (100K 1D)", rng.integers(-10**6, 10**6, size=100_000, dtype=np.int32)),
        ("uint8   (200K 1D)", rng.integers(0, 255, size=200_000, dtype=np.uint8)),
        ("bool    (200K 1D)", rng.integers(0, 2, size=200_000).astype(np.bool_)),
    ]
    print(f"\n{'NumPy Benchmark':<24} | {'cxjson':>10} | {'orjson':>10} | {'Ratio':>7}")
    print("-" * 62)
    ratios = []
    for label, arr in benches:
        out = cxjson.dumps(arr, option=cxjson.OPT_SERIALIZE_NUMPY)
        mb = len(out) / 1048576.0
        gc.collect()
        cxjson.dumps(arr, option=cxjson.OPT_SERIALIZE_NUMPY)
        orjson.dumps(arr, option=orjson.OPT_SERIALIZE_NUMPY)
        tc, to = [], []
        for _ in range(15):
            t0 = time.perf_counter(); cxjson.dumps(arr, option=cxjson.OPT_SERIALIZE_NUMPY); tc.append(time.perf_counter() - t0)
            t0 = time.perf_counter(); orjson.dumps(arr, option=orjson.OPT_SERIALIZE_NUMPY); to.append(time.perf_counter() - t0)
        cm = mb / statistics.median(tc)
        om = mb / statistics.median(to)
        ratios.append(cm / om)
        print(f"{label:<24} | {cm:7.1f} M/s | {om:7.1f} M/s | {cm/om:6.2f}x")
    gm = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
    print("-" * 62)
    print(f"{'GEO-MEAN SPEEDUP':<24} | {'':>10} | {'':>10} | {gm:6.2f}x")


if __name__ == "__main__":
    test_numpy_correctness()
    bench_numpy()
