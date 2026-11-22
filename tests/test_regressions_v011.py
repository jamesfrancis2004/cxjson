"""Regression tests for v0.1.1 fixes:
- GC cycle collection & gc.is_tracked parity on split-table dicts (P0)
- OPT_SORT_KEYS prefix-keyed insertion sort & quicksort across all key sizes/edge cases
- Float serialization fuzzing across random IEEE-754 bit patterns & uniform(-180, 180)
- Parallel vs single-threaded dumps byte-identical output
"""

import gc
import json
import math
import os
import random
import struct
import subprocess
import sys
import weakref

import cxjson

try:
    import orjson
except ImportError:
    orjson = None


def test_split_dict_gc_cycles_and_tracking():
    class Node:
        pass

    payload = b'{"id":1,"children":[1,2,3],"meta":{"nested":true}}'
    for _ in range(6):
        cxjson.loads(payload)

    refs = []
    for _ in range(1500):
        d = cxjson.loads(payload)
        n = Node()
        n.data = d
        d["children"].append(n)
        refs.append(weakref.ref(n))
    del d, n
    gc.collect()
    gc.collect()
    alive = sum(r() is not None for r in refs)
    assert alive <= 3, f"Cycle leak: {alive} surviving objects"

    # Compare tracked dict count on a repeated-schema document vs stdlib json
    doc = json.dumps([
        {"a": i, "b": [i, i + 1], "c": {"x": i, "y": "s"}}
        for i in range(100)
    ]).encode()

    def count_tracked_dicts(obj):
        total = 0
        stack = [obj]
        while stack:
            cur = stack.pop()
            if isinstance(cur, dict):
                if gc.is_tracked(cur):
                    total += 1
                stack.extend(cur.values())
            elif isinstance(cur, list):
                stack.extend(cur)
        return total

    cx_tracked = count_tracked_dicts(cxjson.loads(doc))
    std_tracked = count_tracked_dicts(json.loads(doc))
    assert cx_tracked == std_tracked, f"Tracked dicts mismatch: cxjson={cx_tracked}, json={std_tracked}"


def test_opt_sort_keys_all_sizes_and_edge_cases():
    rng = random.Random(12345)

    # Test key counts spanning 0, 1, 2, 3, 8, 9, 16, 24, 25, 32, 64, 65, 200
    for n in (0, 1, 2, 3, 4, 8, 9, 16, 24, 25, 32, 64, 65, 200):
        keys = [f"k_{rng.randint(0, 10**9)}_{i}" for i in range(n)]
        d = {k: i for i, k in enumerate(keys)}
        for opt in (
            cxjson.OPT_SORT_KEYS,
            cxjson.OPT_SORT_KEYS | cxjson.OPT_INDENT_2,
        ):
            got = cxjson.dumps(d, option=opt)
            if orjson is not None:
                o_opt = orjson.OPT_SORT_KEYS | (orjson.OPT_INDENT_2 if (opt & cxjson.OPT_INDENT_2) else 0)
                exp = orjson.dumps(d, option=o_opt)
                assert got == exp, f"OPT_SORT_KEYS mismatch for n={n}, opt={opt}"
            else:
                loaded = json.loads(got)
                assert list(loaded.keys()) == sorted(d.keys())

    # Shared >8-byte prefixes, short prefixes, empty key, embedded NUL, Unicode
    tricky = {
        "": 0,
        "a": 1,
        "a\x00": 2,
        "a\x00b": 3,
        "ab": 4,
        "abcdefgh": 5,
        "abcdefghi": 6,
        "abcdefgh\x00": 7,
        "abcdefgh_1": 8,
        "abcdefgh_0": 9,
        "prefix_shared_00010": 10,
        "prefix_shared_00002": 11,
        "prefix_shared_00001": 12,
        "é": 13,
        "日本語": 14,
        "🎉": 15,
    }
    # Shuffle insertion order
    items = list(tricky.items())
    rng.shuffle(items)
    d_tricky = dict(items)
    got = cxjson.dumps(d_tricky, option=cxjson.OPT_SORT_KEYS)
    if orjson is not None:
        assert got == orjson.dumps(d_tricky, option=orjson.OPT_SORT_KEYS)
    else:
        assert list(json.loads(got).keys()) == sorted(d_tricky.keys(), key=lambda s: s.encode("utf-8"))

    # OPT_SORT_KEYS + OPT_NON_STR_KEYS
    d_non_str = {10: "ten", 2: "two", 1: "one", 20: "twenty", 3: "three", 15: "fifteen", 7: "seven", 8: "eight", 9: "nine"}
    got_ns = cxjson.dumps(d_non_str, option=cxjson.OPT_SORT_KEYS | cxjson.OPT_NON_STR_KEYS)
    if orjson is not None:
        assert got_ns == orjson.dumps(d_non_str, option=orjson.OPT_SORT_KEYS | orjson.OPT_NON_STR_KEYS)
    else:
        assert list(json.loads(got_ns).keys()) == sorted(str(k) for k in d_non_str)


def test_float_fuzz_exactness():
    rng = random.Random(2026)

    # 1. Special boundary values
    specials = [
        0.0, -0.0, 1.0, -1.0, 0.5, -0.5,
        1e-4, 9.999999e-5, 1e-5, 1e-9, 1e-10, 1e-308, 5e-324,
        1e15, 1e16, 1e17, 1e308, 1.7976931348623157e308,
        float("inf"), float("-inf"), float("nan"),
        4503599627370495.0, 4503599627370496.0, -4503599627370496.0,
    ]
    if orjson is not None:
        assert cxjson.dumps(specials) == orjson.dumps(specials)

    # 2. 50,000 random IEEE-754 bit patterns across all exponents + subnormals
    rand_bits = []
    for _ in range(50_000):
        u = rng.getrandbits(64)
        f = struct.unpack("<d", struct.pack("<Q", u))[0]
        if math.isfinite(f):
            rand_bits.append(f)

    # 3. 50,000 geographic-style floats in [-180, 180]
    geo = [rng.uniform(-180.0, 180.0) for _ in range(50_000)]

    for batch in (rand_bits, geo):
        cx_out = cxjson.dumps(batch)
        if orjson is not None:
            or_out = orjson.dumps(batch)
            assert cx_out == or_out, "Float serialization differs from orjson!"
        else:
            rt = cxjson.loads(cx_out)
            assert rt == batch


def test_parallel_vs_single_threaded_identical():
    # Spawn a subprocess with CXJSON_MAX_THREADS=1 and verify byte-identical output
    # against default multi-threaded dumps on >1 MB payloads.
    script = """
import os, random, string, cxjson
rng = random.Random(99)
def rstr(n): return ''.join(rng.choices(string.ascii_letters + string.digits + ' ', k=n))
payloads = [
    list(range(250_000)),
    [rng.uniform(-1e6, 1e6) for _ in range(100_000)],
    [rstr(rng.randint(5, 40)) for _ in range(50_000)],
    [{"id": i, "name": rstr(15), "vals": [i, i * 0.25, True, None]} for i in range(5000)],
]
import hashlib
digests = [hashlib.sha256(cxjson.dumps(p)).hexdigest() for p in payloads]
print("|".join(digests))
"""
    env_mt = os.environ.copy()
    env_mt.pop("CXJSON_MAX_THREADS", None)
    env_st = os.environ.copy()
    env_st["CXJSON_MAX_THREADS"] = "1"

    out_mt = subprocess.check_output([sys.executable, "-c", script], env=env_mt, text=True).strip()
    out_st = subprocess.check_output([sys.executable, "-c", script], env=env_st, text=True).strip()
    assert out_mt == out_st, f"Parallel vs 1T mismatch:\nMT={out_mt}\nST={out_st}"


def test_parallel_loads_numeric_arrays_and_errors():
    script = """
import hashlib, random, cxjson
rng = random.Random(77)
payloads = [
    cxjson.dumps([rng.uniform(-1e6, 1e6) for _ in range(100_000)]),
    cxjson.dumps([rng.randint(-10**12, 10**12) for _ in range(100_000)]),
    cxjson.dumps([i if i % 2 == 0 else i + 0.25 for i in range(100_000)]),
]
digests = [hashlib.sha256(cxjson.dumps(cxjson.loads(p))).hexdigest() for p in payloads]
# Also verify bytearray and memoryview inputs
assert cxjson.loads(bytearray(payloads[0])) == cxjson.loads(payloads[0])
assert cxjson.loads(memoryview(payloads[1])) == cxjson.loads(payloads[1])
print("|".join(digests))
"""
    env_mt = os.environ.copy()
    env_mt.pop("CXJSON_MAX_THREADS", None)
    env_st = os.environ.copy()
    env_st["CXJSON_MAX_THREADS"] = "1"

    out_mt = subprocess.check_output([sys.executable, "-c", script], env=env_mt, text=True).strip()
    out_st = subprocess.check_output([sys.executable, "-c", script], env=env_st, text=True).strip()
    assert out_mt == out_st, f"Parallel loads vs 1T mismatch:\nMT={out_mt}\nST={out_st}"

    # Malformed >256KB numeric arrays must cleanly fall back and raise JSONDecodeError
    big_valid = b"[" + b",".join(b"123456" for _ in range(60_000))
    for bad in (
        big_valid + b",]",
        big_valid + b",01]",
        big_valid + b",NaN]",
    ):
        try:
            cxjson.loads(bad)
            raise AssertionError("Expected JSONDecodeError for malformed large numeric array")
        except cxjson.JSONDecodeError:
            pass

    # Verify fork() safety after parallel loads
    if hasattr(os, "fork"):
        big_arr = big_valid + b"]"
        assert len(cxjson.loads(big_arr)) == 60_000
        pid = os.fork()
        if pid == 0:
            try:
                res = cxjson.loads(big_arr)
                os._exit(0 if len(res) == 60_000 and res[-1] == 123456 else 1)
            except Exception:
                os._exit(2)
        else:
            _, status = os.waitpid(pid, 0)
            assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, f"Child failed: {status}"


def test_parallel_opts_sort_indent_and_deep_nesting():
    """Verify parallel OPT_SORT_KEYS / OPT_INDENT_2 dumps matches 1T byte-for-byte
    and that deep (>32 level) OPT_INDENT_2 nesting does not OOM."""
    script = r"""
import hashlib, cxjson

payloads = [
    [{"z": i, "a": f"user_{i}", "m": i * 1.5, "b": {"y": i & 1, "x": [1, 2, None, True, False]}, "e": {}, "l": []}
     for i in range(10_000)],
    [{"k\"1": "v\"1\n\t\\", "a\\b": i, "z": f"caf\u00e9_{i}"} for i in range(8_000)],
    [{f"key_{k:03d}": k + i for k in range(70, 0, -1)} for i in range(1_500)],
    {"wrapper": {"items": [[i * 0.25, -i * 0.5] for i in range(40_000)]}},
    {f"rec_{i:05d}": {"z": i, "a": f"user_{i}", "m": i * 1.5, "tags": ["x", "y"]} for i in range(10_000)},
]
opts = [
    cxjson.OPT_SORT_KEYS,
    cxjson.OPT_INDENT_2,
    cxjson.OPT_SORT_KEYS | cxjson.OPT_INDENT_2,
    cxjson.OPT_SORT_KEYS | cxjson.OPT_INDENT_2 | cxjson.OPT_APPEND_NEWLINE,
]
digests = []
for p in payloads:
    for o in opts:
        digests.append(hashlib.sha256(cxjson.dumps(p, option=o)).hexdigest())
print("|".join(digests))
"""
    env_mt = os.environ.copy()
    env_mt.pop("CXJSON_MAX_THREADS", None)
    env_st = os.environ.copy()
    env_st["CXJSON_MAX_THREADS"] = "1"

    out_mt = subprocess.check_output([sys.executable, "-c", script], env=env_mt, text=True).strip()
    out_st = subprocess.check_output([sys.executable, "-c", script], env=env_st, text=True).strip()
    assert out_mt == out_st, f"Parallel opts dumps vs 1T mismatch:\nMT={out_mt}\nST={out_st}"

    # Deep (>32 level) OPT_INDENT_2 nesting must not trigger exponential buffer growth
    nested = {"b": 1, "a": 2}
    for _ in range(40):
        nested = [nested]
    deep = [nested] * 50
    expected_ind = json.dumps(deep, indent=2).encode("utf-8")
    expected_sort_ind = json.dumps(deep, indent=2, sort_keys=True).encode("utf-8")
    assert cxjson.dumps(deep, option=cxjson.OPT_INDENT_2) == expected_ind
    assert cxjson.dumps(deep, option=cxjson.OPT_SORT_KEYS | cxjson.OPT_INDENT_2) == expected_sort_ind

    # Direct UUID fast-path boundary values + subclass + parallel array identity
    import uuid
    class SubUUID(uuid.UUID):
        pass
    edge_uuids = [
        uuid.UUID(int=0),
        uuid.UUID(int=1),
        uuid.UUID(int=(1 << 64) - 1),
        uuid.UUID(int=(1 << 64)),
        uuid.UUID(int=(1 << 128) - 1),
        SubUUID("12345678-1234-5678-1234-567812345678"),
    ]
    for u in edge_uuids:
        assert cxjson.dumps(u) == f'"{u}"'.encode("ascii")
    many_uuids = [uuid.UUID(int=(i * 0x9E3779B97F4A7C159E3779B97F4A7C15) & ((1 << 128) - 1)) for i in range(40_000)]
    expected_many = ("[" + ",".join(f'"{u}"' for u in many_uuids) + "]").encode("ascii")
    assert cxjson.dumps(many_uuids) == expected_many


def test_slotted_dataclass_and_split_dict_dumps():
    """Verify @dataclass(slots=True) fast path (nested, inherited, empty, deleted slot)
    and split-table dict dumps() across mutations on Python 3.12/3.13/3.14."""
    import dataclasses

    @dataclasses.dataclass(slots=True)
    class SlotUser:
        id: int
        name: str
        score: float
        active: bool
        _secret: str = "hidden"

    @dataclasses.dataclass(slots=True)
    class SlotOrder:
        order_id: int
        user: SlotUser
        tags: list

    @dataclasses.dataclass(slots=True)
    class ChildSlot(SlotUser):
        extra: int = 99

    @dataclasses.dataclass(slots=True)
    class EmptySlot:
        pass

    users = [SlotUser(i, f"u{i}", i * 1.5, (i & 1) == 0) for i in range(1000)]
    orders = [SlotOrder(i, users[i], ["x", "y"]) for i in range(500)]
    assert json.loads(cxjson.dumps(users)) == [
        {"id": i, "name": f"u{i}", "score": i * 1.5, "active": (i & 1) == 0} for i in range(1000)
    ]
    assert json.loads(cxjson.dumps(orders))[42] == {
        "order_id": 42,
        "user": {"id": 42, "name": "u42", "score": 63.0, "active": True},
        "tags": ["x", "y"],
    }
    assert cxjson.dumps(ChildSlot(1, "a", 2.5, True)) == b'{"id":1,"name":"a","score":2.5,"active":true,"extra":99}'
    assert cxjson.dumps(EmptySlot()) == b"{}"

    # Deleted slot must fall back cleanly without crashing
    u_del = SlotUser(1, "a", 2.5, True)
    del u_del.name
    assert cxjson.dumps(u_del) == b'{"id":1,"score":2.5,"active":true}'

    # Force cache-slot collisions across 80 distinct nested slotted dataclass types
    inner_types = [
        dataclasses.make_dataclass(f"InnerDC{k}", [("a", int), ("b", str), ("c", float), ("d", bool)], slots=True)
        for k in range(80)
    ]
    outer_types = [
        dataclasses.make_dataclass(f"OuterDC{k}", [("x", int), ("inner", object), ("tail", str)], slots=True)
        for k in range(80)
    ]
    for k in range(80):
        inst = outer_types[k](k, inner_types[k](k, "hi", 1.5, True), "ok")
        assert json.loads(cxjson.dumps(inst)) == {
            "x": k, "inner": {"a": k, "b": "hi", "c": 1.5, "d": True}, "tail": "ok"
        }

    # Split-table dict array dumps with mid-list mutations (del + insert)
    raw = cxjson.dumps([{"id": i, "name": f"r{i}", "val": i * 0.5, "ok": True} for i in range(500)])
    recs = cxjson.loads(raw)
    assert cxjson.dumps(recs) == raw
    del recs[250]["name"]
    recs[251]["extra"] = 123
    assert json.loads(cxjson.dumps(recs)) == recs


def test_direct_dict_float_pair_and_parallel_numpy():
    # Direct combined-table dict iteration with tombstones & nested dicts
    d = {f"k_{i}": {"v": i, "p": [float(i), -float(i) * 0.5]} for i in range(200)}
    for i in range(0, 200, 3):
        del d[f"k_{i}"]
    out = cxjson.dumps(d)
    assert json.loads(out) == d

    # Dict array with tombstones and non-str key error handling
    arr = [{"a": i, "b": i + 1, "c": i + 2} for i in range(20)]
    del arr[5]["b"]
    assert json.loads(cxjson.dumps(arr)) == arr
    arr_bad = [{"a": 1}, {"a": 2}, {"a": 3}, {"a": 4}, {123: "bad"}]
    try:
        cxjson.dumps(arr_bad)
        raise AssertionError("Expected TypeError for int dict key")
    except TypeError:
        pass

    # Dict mutation inside default() must raise RuntimeError safely (no UAF)
    class Mutator:
        pass
    mut_dict = {"a": Mutator(), "b": 1}
    def mut_default(x):
        for k in range(100):
            mut_dict[f"extra_{k}"] = k
        return "mutated"
    try:
        cxjson.dumps(mut_dict, default=mut_default)
        raise AssertionError("Expected RuntimeError when dict resized during dumps")
    except RuntimeError:
        pass

    # Adversarial: clear() + reinsert int keys (dk_kind 1 -> 0) and clear() to empty
    d_clear = {"a": Mutator(), "b": 2}
    try:
        cxjson.dumps(d_clear, default=lambda x: (d_clear.clear(), d_clear.__setitem__(1, 2), d_clear.__setitem__(3, 4), "ok")[-1])
        raise AssertionError("Expected RuntimeError on clear+reinsert")
    except RuntimeError:
        pass

    # Adversarial: split-table dict promoted/mutated inside default() in encode_dict_array_cached
    split_recs = cxjson.loads(cxjson.dumps([{"a": i, "b": i + 1} for i in range(10)]))
    split_recs[5]["a"] = Mutator()
    try:
        cxjson.dumps(split_recs, default=lambda x: (split_recs[5].__setitem__("extra", 99), "ok")[1])
        raise AssertionError("Expected RuntimeError when split-table dict promoted during dumps")
    except RuntimeError:
        pass
    split_recs2 = cxjson.loads(cxjson.dumps([{"a": i, "b": i + 1} for i in range(10)]))
    split_recs2[5]["a"] = Mutator()
    try:
        cxjson.dumps(split_recs2, default=lambda x: (split_recs2[5].pop("b"), "ok")[1])
        raise AssertionError("Expected RuntimeError when split-table dict key deleted during dumps")
    except RuntimeError:
        pass

    # str subclass key + delete-remaining-key without resize + Unicode/escaped tombstones
    class MyStr(str):
        pass
    assert cxjson.dumps({MyStr("hello"): 1, "world": 2}) == b'{"hello":1,"world":2}'
    d_del = {"a": Mutator(), "b": 2}
    assert cxjson.dumps(d_del, default=lambda x: (d_del.pop("b"), "ok")[1]) == b'{"a":"ok"}'
    d_uni = {"héllo": 1, "🚀": 2, 'a"b\\c\n': 3, "bye": 4}
    del d_uni["🚀"]
    assert json.loads(cxjson.dumps(d_uni)) == d_uni

    # [float, float] pair fast path including NaN/Inf/-Inf
    pairs = [[1.5, -2.5], [float("nan"), float("inf")], [float("-inf"), 0.0]]
    assert cxjson.dumps(pairs) == b'[[1.5,-2.5],[null,null],[null,0.0]]'

    # Parallel 1D & 2D NumPy serialization (if numpy is installed)
    try:
        import numpy as np
    except ImportError:
        return
    rng = np.random.default_rng(123)
    for np_arr in [
        rng.uniform(-1e4, 1e4, 25_000).astype(np.float64),
        rng.uniform(-1e3, 1e3, 25_000).astype(np.float32),
        rng.integers(-10**12, 10**12, 25_000, dtype=np.int64),
        rng.integers(-10**6, 10**6, 25_000, dtype=np.int32),
        rng.uniform(-180.0, 180.0, (200, 128)).astype(np.float64),
        rng.uniform(-180.0, 180.0, (200, 128)).astype(np.float32),
        rng.integers(-10**9, 10**9, (200, 128), dtype=np.int64),
    ]:
        mt_bytes = cxjson.dumps(np_arr, option=cxjson.OPT_SERIALIZE_NUMPY)
        # Verify against Python list serialization
        expected = cxjson.dumps(np_arr.tolist())
        if np_arr.dtype in (np.float64, np.int64, np.int32):
            assert mt_bytes == expected, f"Mismatch for {np_arr.dtype} {np_arr.shape}"
        else:
            assert json.loads(mt_bytes) is not None
    nan_arr = rng.uniform(-10.0, 10.0, 20_000).astype(np.float64)
    nan_arr[0] = np.nan
    nan_arr[10_000] = np.inf
    nan_arr[-1] = -np.inf
    assert cxjson.dumps(nan_arr, option=cxjson.OPT_SERIALIZE_NUMPY) == cxjson.dumps(nan_arr.tolist())


def test_cpython_internal_invariants():
    """Verify every CPython-internal optimization invariant across 3.12 / 3.13 / 3.14:
    1. Split-table obj.__dict__ key insertion order & partial-attribute NULL slots.
    2. Split-table default() delete+insert same-size mutation safety.
    3. Shared-keys (cjson_build_shared_keys) USABLE_FRACTION sizing across all log2_size buckets.
    4. Manual PyASCIIObject & PyLongObject constructors (non-immortal refcnt, hash, repr, boundaries).
    """
    # 1a. Split-table obj.__dict__ differing insertion order must preserve exact key order
    class Foo:
        pass
    a = Foo(); a.x = 1; a.y = 2
    b = Foo(); b.y = 2; b.x = 1
    c = Foo(); c.x = 3; c.y = 4
    d = Foo(); d.y = 4; d.x = 3
    dicts_order = [a.__dict__, b.__dict__, c.__dict__, d.__dict__] * 3
    expected_order = json.dumps(dicts_order, separators=(",", ":")).encode("utf-8")
    assert cxjson.dumps(dicts_order) == expected_order

    # 1b. Split-table obj.__dict__ with partial attributes (NULL slots in dv->values[])
    class Bar:
        pass
    seed = Bar(); seed.a = 10; seed.b = 20; seed.c = 30
    o1 = Bar(); o1.a = 1; o1.b = 2
    o2 = Bar(); o2.b = 2; o2.c = 3
    o3 = Bar(); o3.c = 3; o3.a = 1
    o4 = Bar(); o4.c = 3; o4.b = 2; o4.a = 1
    o5 = Bar(); o5.a = 1; o5.b = 2; del o5.a
    o6 = Bar(); o6.a = 1; o6.b = 2; o6.c = 3; del o6.b
    for seq in (
        [o1.__dict__, o2.__dict__, o3.__dict__, o4.__dict__, o5.__dict__, o6.__dict__, seed.__dict__] * 2,
        [seed.__dict__, o4.__dict__, o2.__dict__, o1.__dict__, o3.__dict__, o6.__dict__, o5.__dict__] * 2,
        [o4.__dict__, o4.__dict__, o1.__dict__, o2.__dict__, o2.__dict__, o3.__dict__] * 2,
    ):
        expected = json.dumps(seq, separators=(",", ":")).encode("utf-8")
        assert cxjson.dumps(seq) == expected

    # 1c. Homogeneous reverse-order split dicts (hits split_hit with non-identity split_order)
    class Rev:
        pass
    rseed = Rev(); rseed.a = 0; rseed.b = 0; rseed.c = 0
    revs = []
    for i in range(20):
        r = Rev()
        r.c = i * 3
        r.b = i * 2
        r.a = i
        revs.append(r.__dict__)
    assert cxjson.dumps(revs) == json.dumps(revs, separators=(",", ":")).encode("utf-8")

    # 2. Split-table default() delete + insert keeping ma_used == split_n must raise RuntimeError
    class Mutator:
        pass
    class Baz:
        pass
    bseed = Baz(); bseed.a = 0; bseed.b = 0; bseed.c = 0
    b1 = Baz(); b1.a = 1; b1.b = 2
    b2 = Baz(); b2.a = Mutator(); b2.b = 2
    def swap_keys(_x):
        del b2.b
        b2.c = 99
        return "mut"
    try:
        cxjson.dumps([b1.__dict__] * 5 + [b2.__dict__] + [b1.__dict__] * 4, default=swap_keys)
        raise AssertionError("Expected RuntimeError when split-dict keys swapped during dumps")
    except RuntimeError:
        pass

    # 3. Shared-keys USABLE_FRACTION sizing across log2_size=4 (n<=10), 5 (11..21), 6 (22..30)
    for n in (1, 3, 10, 11, 21, 22, 30):
        raw = cxjson.dumps([{f"k{j:02d}": i + j for j in range(n)} for i in range(8)])
        loaded = cxjson.loads(raw)
        assert len(loaded) == 8
        for item in loaded:
            assert sys.getsizeof(item) > 0
            cp = item.copy()
            assert cp == item
        loaded[4][f"extra_{n}"] = [1, 2, 3]
        del loaded[5]["k00"]
        loaded[6].update({"k00": -1, "new_key": "ok"})
        assert cxjson.loads(cxjson.dumps(loaded)) == loaded

    # 4. Manual PyLongObject & PyASCIIObject constructor invariants
    boundary_ints = [
        0, 1, -1, 5, -5, 256, 257, 2100, 2101,
        (1 << 30) - 1, -((1 << 30) - 1),
        (1 << 30), -(1 << 30),
        (1 << 60) - 1, -((1 << 60) - 1),
        (1 << 60), -(1 << 60),
        (1 << 63) - 1, -(1 << 63),
        (1 << 64) - 1,
    ]
    parsed_ints = cxjson.loads(json.dumps(boundary_ints))
    assert parsed_ints == boundary_ints
    for v, ref in zip(parsed_ints, boundary_ints):
        assert type(v) is int
        assert v == ref
        assert hash(v) == hash(ref)
        assert repr(v) == repr(ref)
        if abs(ref) > 2100:
            # Must NOT be accidentally immortal (refcnt ~ 2^30)
            assert sys.getrefcount(v) < 100

    for slen in (0, 1, 2, 7, 8, 15, 16, 17, 64, 512, 4096, 4097):
        ref = f"u{slen}_" + ("x" * max(0, slen - len(f"u{slen}_")))
        ref = ref[:slen]
        s = cxjson.loads(json.dumps(ref))
        assert type(s) is str
        assert s == ref
        assert len(s) == slen
        assert hash(s) == hash(ref)
        assert repr(s) == repr(ref)
        if slen >= 2:
            assert sys.getrefcount(s) < 100


if __name__ == "__main__":
    test_split_dict_gc_cycles_and_tracking()
    print("PASS: test_split_dict_gc_cycles_and_tracking")
    test_opt_sort_keys_all_sizes_and_edge_cases()
    print("PASS: test_opt_sort_keys_all_sizes_and_edge_cases")
    test_float_fuzz_exactness()
    print("PASS: test_float_fuzz_exactness")
    test_parallel_vs_single_threaded_identical()
    print("PASS: test_parallel_vs_single_threaded_identical")
    test_parallel_loads_numeric_arrays_and_errors()
    print("PASS: test_parallel_loads_numeric_arrays_and_errors")
    test_parallel_opts_sort_indent_and_deep_nesting()
    print("PASS: test_parallel_opts_sort_indent_and_deep_nesting")
    test_slotted_dataclass_and_split_dict_dumps()
    print("PASS: test_slotted_dataclass_and_split_dict_dumps")
    test_direct_dict_float_pair_and_parallel_numpy()
    print("PASS: test_direct_dict_float_pair_and_parallel_numpy")
    test_cpython_internal_invariants()
    print("PASS: test_cpython_internal_invariants")
    print("\nALL v0.1.1 REGRESSION TESTS PASSED!")
