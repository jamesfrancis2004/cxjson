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
    print("\nALL v0.1.1 REGRESSION TESTS PASSED!")
