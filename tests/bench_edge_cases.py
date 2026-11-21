"""Stress / edge-case benchmark: cxjson vs orjson on shapes the standard
datasets don't cover. Reports ops/s ratio and flags cases where cxjson loses.

Run: PYTHONPATH=src python tests/bench_edge_cases.py [--fresh]

--fresh rebuilds the payload before every timed call via a factory, so
per-object caches (e.g. CPython's cached UTF-8 for non-ASCII str) cannot
flatter either library.
"""
import datetime
import gc
import sys
import time
import uuid
from dataclasses import dataclass
from enum import Enum

import cxjson
import orjson

random_state = 12345


def rnd():
    global random_state
    random_state = (random_state * 1103515245 + 12345) & 0x7FFFFFFF
    return random_state


@dataclass
class Point:
    x: int
    y: float
    label: str


class Color(Enum):
    RED = "red"
    GREEN = "green"


class StrSub(str):
    pass


def make_cases():
    c = {}
    c["tiny dict (3 keys)"] = lambda: {"a": 1, "b": "x", "c": None}
    c["empty containers"] = lambda: [[], {}, [], {}] * 50
    c["single short str"] = lambda: "hello world"
    c["single int"] = lambda: 123456789
    c["list of 10k small ints"] = lambda: list(range(10_000))
    c["list of 10k large ints (>2^31)"] = lambda: [(1 << 40) + i * 7919 for i in range(10_000)]
    c["list of 10k negative ints"] = lambda: [-i * 31 for i in range(10_000)]
    c["list of 10k huge ints (2^63+)"] = lambda: [(1 << 63) + i for i in range(10_000)]
    c["list of 10k floats (random)"] = lambda: [(rnd() / 2147483647.0) * 1000 for _ in range(10_000)]
    c["list of 10k floats (short)"] = lambda: [i * 0.5 for i in range(10_000)]
    c["list of 10k floats (1e-9..1e20)"] = lambda: [(rnd() % 1000 + 1) * 10.0 ** (rnd() % 30 - 9) for _ in range(10_000)]
    c["list of 10k bools"] = lambda: [i % 2 == 0 for i in range(10_000)]
    c["list of 10k None"] = lambda: [None] * 10_000
    c["list of 10k short ASCII str"] = lambda: [f"item-{i}" for i in range(10_000)]
    c["list of 10k empty str"] = lambda: [""] * 10_000
    c["list of 10k 1-char str"] = lambda: ["x"] * 10_000
    c["1 MB ASCII string"] = lambda: "The quick brown fox jumps. " * 40_000
    c["1 MB string, 1% escapes"] = lambda: ("a" * 99 + "\n") * 10_000
    c["1 MB string, all escapes"] = lambda: '"\\\n' * 300_000
    c["100KB 2-byte (Cyrillic) str"] = lambda: "Привет мир " * 9_000
    c["100KB 3-byte (CJK) str"] = lambda: "日本語のテキスト" * 12_000
    c["100KB emoji (4-byte) str"] = lambda: "😀🎉🚀" * 9_000
    c["100KB latin-1 str"] = lambda: "café résumé naïve " * 6_000
    c["10k short CJK strs"] = lambda: ["日本語" + str(i) for i in range(10_000)]
    c["10k short Cyrillic strs"] = lambda: ["Имя" + str(i) for i in range(10_000)]
    c["10k short emoji strs"] = lambda: ["😀" + str(i) for i in range(10_000)]
    c["10k dicts w/ non-ASCII keys"] = lambda: [{"имя": i, "名前": str(i)} for i in range(10_000)]
    c["nested depth 200 (lists)"] = lambda: _nest_list(200)
    c["nested depth 200 (dicts)"] = lambda: _nest_dict(200)
    c["10k tiny dicts"] = lambda: [{"id": i} for i in range(10_000)]
    c["1k wide dicts (50 keys)"] = lambda: [{f"key_{k}": k for k in range(50)} for _ in range(1000)]
    c["dict of 10k keys"] = lambda: {f"k{i}": i for i in range(10_000)}
    c["10k 2-tuples"] = lambda: [(i, i + 1) for i in range(10_000)]
    c["list of 1k lists of 10 ints"] = lambda: [list(range(10)) for _ in range(1000)]
    c["matrix 200x200 floats"] = lambda: [[r * 0.25 + k for k in range(200)] for r in range(200)]
    c["10k datetimes"] = lambda: [datetime.datetime(2024, 1, 1, 12, 0, i % 60, tzinfo=datetime.timezone.utc) for i in range(10_000)]
    c["10k naive datetimes"] = lambda: [datetime.datetime(2024, 1, 1, 12, 0, i % 60, 123456) for i in range(10_000)]
    c["10k uuids"] = lambda: [uuid.UUID(int=i * 7919 + (1 << 100)) for i in range(10_000)]
    c["10k dataclasses"] = lambda: [Point(i, i * 0.5, f"p{i}") for i in range(10_000)]
    c["10k enums"] = lambda: [Color.RED, Color.GREEN] * 5_000
    c["10k str subclasses"] = lambda: [StrSub(f"s{i}") for i in range(10_000)]
    c["repeated shared objects"] = lambda: [_SHARED] * 10_000
    c["log lines (mixed)"] = lambda: [
        {"ts": 1700000000 + i, "lvl": "INFO", "msg": f"request {i} done", "ms": i * 0.13, "ok": True, "tags": ["a", "b"]}
        for i in range(10_000)
    ]
    return c


_SHARED = {"a": 1, "b": [1, 2, 3], "c": "shared"}


def _nest_list(n):
    o = []
    for _ in range(n):
        o = [o, 1]
    return o


def _nest_dict(n):
    o = {}
    for _ in range(n):
        o = {"k": o, "v": 1}
    return o


def make_option_cases():
    base = lambda: [{"b": i, "a": f"v{i}", "c": [i, i * 0.5]} for i in range(2000)]
    return {
        "OPT_INDENT_2 (2k dicts)": (base, orjson.OPT_INDENT_2),
        "OPT_SORT_KEYS (2k dicts)": (base, orjson.OPT_SORT_KEYS),
        "OPT_APPEND_NEWLINE (2k dicts)": (base, orjson.OPT_APPEND_NEWLINE),
        "OPT_NON_STR_KEYS (10k int keys)": (lambda: {i: i for i in range(10_000)}, orjson.OPT_NON_STR_KEYS),
        "default= fn (10k sets->list)": (lambda: [{1, 2}] * 1000, None),
    }


def timeit(fn, data, min_time=0.25):
    for _ in range(3):
        fn(data)
    gc.collect()
    gc.disable()
    n = 0
    t0 = time.perf_counter()
    while True:
        fn(data)
        n += 1
        dt = time.perf_counter() - t0
        if dt >= min_time and n >= 3:
            break
    gc.enable()
    return dt / n


def timeit_fresh(fn, factory, min_time=0.25):
    tot = 0.0
    n = 0
    start = time.perf_counter()
    while time.perf_counter() - start < min_time * 3 or n < 3:
        d = factory()
        gc.collect()
        gc.disable()
        t0 = time.perf_counter()
        fn(d)
        tot += time.perf_counter() - t0
        gc.enable()
        n += 1
    return tot / n


def main():
    fresh = "--fresh" in sys.argv
    print(f"mode: {'fresh objects' if fresh else 'repeated same object'}")
    hdr = f"{'case':36s} | {'size':>9s} | {'cxjson':>10s} | {'orjson':>10s} | ratio"
    print(hdr)
    print("-" * (len(hdr) + 14))
    losers = []
    rows = []

    def run(name, factory, copt=None, oopt=None, cdef=None, odef=None):
        data = factory()
        ckw = {}
        okw = {}
        if copt is not None:
            ckw["option"] = copt
            okw["option"] = oopt
        if cdef is not None:
            ckw["default"] = cdef
            okw["default"] = odef
        cf = lambda d: cxjson.dumps(d, **ckw)
        of = lambda d: orjson.dumps(d, **okw)
        try:
            size = len(of(data))
            if cf(data) != of(data):
                print(f"{name:36s} | OUTPUT MISMATCH")
                losers.append((name, 0.0))
                return
        except Exception as e:  # pragma: no cover
            print(f"{name:36s} | ERROR {type(e).__name__}: {e}")
            return
        if fresh:
            tc = timeit_fresh(cf, factory)
            to = timeit_fresh(of, factory)
        else:
            tc = timeit(cf, data)
            to = timeit(of, data)
        ratio = to / tc
        flag = "" if ratio >= 1.0 else "  <-- SLOWER"
        print(f"{name:36s} | {size:9,d} | {tc*1e6:8.1f}us | {to*1e6:8.1f}us | {ratio:5.2f}x{flag}")
        rows.append((name, ratio))
        if ratio < 1.0:
            losers.append((name, ratio))

    for name, factory in make_cases().items():
        run(name, factory)
    for name, (factory, opt) in make_option_cases().items():
        if name.startswith("default="):
            run(name, factory, copt=0, oopt=0, cdef=list, odef=list)
        else:
            run(name, factory, copt=opt, oopt=opt)

    print()
    geo = 1.0
    for _, r in rows:
        geo *= r
    geo **= 1.0 / len(rows)
    print(f"geometric mean speedup vs orjson: {geo:.2f}x over {len(rows)} cases")
    if losers:
        print("Cases where cxjson is slower:")
        for n, r in sorted(losers, key=lambda x: x[1]):
            print(f"  {r:5.2f}x  {n}")
    else:
        print("cxjson wins every case.")


if __name__ == "__main__":
    main()
