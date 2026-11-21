"""Comprehensive memory-leak and refcount-safety test suite for cxjson.

Runs with zero external dependencies (stdlib only) so it executes on every
platform (Linux, macOS, Windows) and Python version (3.12, 3.13, 3.14) in CI.
"""

import dataclasses
import datetime
import enum
import gc
import sys
import tracemalloc
import uuid

import cxjson


def assert_no_leak(name, fn, warmup=30, iters=300, max_growth_kb=64):
    """Run `fn` repeatedly and assert tracemalloc current memory does not grow."""
    for _ in range(warmup):
        fn()
    gc.collect()
    tracemalloc.start()
    try:
        gc.collect()
        base_cur, _ = tracemalloc.get_traced_memory()
        for _ in range(iters):
            fn()
        gc.collect()
        after_cur, peak = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()

    growth_kb = (after_cur - base_cur) / 1024.0
    assert growth_kb <= max_growth_kb, (
        f"LEAK in {name}: retained memory grew by {growth_kb:.1f} KB "
        f"(limit {max_growth_kb} KB, peak {peak / 1024.0:.1f} KB)"
    )
    print(f"PASS: {name:<48} retained delta = {growth_kb:+6.1f} KB")


def test_split_table_dicts_and_mutations():
    payload = cxjson.dumps([
        {"id": i, "name": f"item_{i}", "score": i * 1.25, "ok": (i % 2 == 0), "tag": "alpha"}
        for i in range(100)
    ])

    def step():
        rows = cxjson.loads(payload)
        # Exercise split-table mutation, promotion to combined, deletion, and copy
        rows[0]["extra_key"] = 42
        del rows[1]["tag"]
        _ = rows[2].copy()
        _ = cxjson.dumps(rows)

    assert_no_leak("split-table dicts (loads + mutate + dumps)", step, iters=250)


def test_schema_switching_and_fallbacks():
    # Exercises split_early_close, extra keys, reordered keys, duplicate keys, and polymorphic schemas
    payload = (
        b'['
        b'{"a":1,"b":"x","c":3.5},'
        b'{"a":2,"b":"y","c":4.5},'
        b'{"a":3,"b":"z","c":5.5},'
        b'{"a":4,"b":"early"},'                     # split_early_close
        b'{"a":5,"b":"extra","c":6.5,"d":true},'    # extra keys -> split_fallback
        b'{"a":6,"c":7.5,"b":"swapped"},'           # key mismatch -> split_fallback
        b'{"a":7,"a":8,"b":"dup","c":9.5},'         # duplicate key
        b'{"x":1,"y":2,"z":{"a":9,"b":"nested","c":1.0}}'
        b']'
    )

    def step():
        obj = cxjson.loads(payload)
        _ = cxjson.dumps(obj)

    assert_no_leak("schema switching / fallbacks / duplicates", step, iters=600)


def test_malformed_json_error_paths():
    bad_inputs = [
        b'{"a":1,"b":2,"c":',                       # truncated mid-object
        b'[{"a":1,"b":2},{"a":1,"b":2},{"a":1,"b":', # truncated mid split-table
        b'[{"a":1,"b":2},{"a":1,"b":2},{"a":1,"b":2,}]', # trailing comma in split-table
        b'[{"a":1,"b":2},{"a":1,"b":2},{"a":1,"b":invalid}]', # bad value in split-table
        b'{"k":"\\uD800"}',                         # lone high surrogate
        b'{"k":"\\uDC00"}',                         # lone low surrogate
        b'{"k":"\\u123"}',                          # truncated hex escape
        b'{"k":"\\q"}',                             # invalid escape
        b'{"k":"\x01"}',                            # unescaped control char
        b'{"k":"\xff\xfe"}',                        # invalid UTF-8
        b'{"k":01}',                                # leading zero
        b'{"k":1.}',                                # trailing dot
        b'{"k":1e+}',                               # bad exponent
        b'[' * 2000 + b']' * 2000,                  # deep nesting
        b'{"a":1} trailing',                        # trailing garbage
        b'[1, 2, 3,]',                              # trailing comma in array
    ]

    def step():
        for raw in bad_inputs:
            try:
                cxjson.loads(raw)
            except cxjson.JSONDecodeError:
                pass

    assert_no_leak("malformed JSON error paths (16 cases)", step, iters=400)


def test_unicode_and_escaped_strings():
    data = {
        "ascii": "hello world " * 20,
        "escaped": 'quote" slash\\ newline\n tab\t ctrl\x07 end ' * 10,
        "latin1": "café résumé naïve über Straße " * 8,
        "cjk": "名前:前田あゆみ 第一印象:なんか怖っ！今の印象:優しそう " * 8,
        "emoji": "🚀🔥🎉👨‍👩‍👧‍👦✨ " * 10,
        "long_clean": "x" * 2048,
        "long_esc": ("abc\"def\\ghi\njkl" * 128),
    }
    raw = cxjson.dumps(data)

    def step():
        obj = cxjson.loads(raw)
        out = cxjson.dumps(obj)
        out_str = cxjson.dumps_str(obj)
        assert len(out) > 0 and len(out_str) > 0

    assert_no_leak("unicode / UTF-8 / escaped strings round-trip", step, iters=300)


def test_parallel_dumps_and_serializer_errors():
    # > 1 MB payload to trigger adaptive parallel serializer
    big = [
        {"id": i, "text": f"row_{i}_" + ("abcdefghij" * 25), "vals": [i, i + 0.5, True, None]}
        for i in range(4500)
    ]

    def step_parallel():
        out = cxjson.dumps(big)
        assert len(out) > 1_000_000

    assert_no_leak("parallel dumps (>1MB payload)", step_parallel, warmup=5, iters=25, max_growth_kb=256)

    # Serializer error paths: unserializable object, failing default(), circular ref
    cyc = []
    cyc.append(cyc)

    def bad_default(x):
        raise RuntimeError("boom")

    def step_errors():
        for fn in (
            lambda: cxjson.dumps({"ok": [1, 2, 3], "bad": {1, 2, 3}}),
            lambda: cxjson.dumps({"ok": "hi", "bad": object()}, default=bad_default),
            lambda: cxjson.dumps(cyc),
            lambda: cxjson.dumps({"nan": float("nan")}),
        ):
            try:
                fn()
            except (TypeError, ValueError, RuntimeError):
                pass

    assert_no_leak("serializer error paths (TypeError/cycle/default)", step_errors, iters=500)


def test_refcounts_preserved():
    @dataclasses.dataclass
    class Point:
        x: int
        y: float
        label: str

    class Color(enum.Enum):
        RED = "red"

    s = "refcount_test_string_" + "a" * 30
    lst = [1, 2, 3, s]
    dct = {"k": lst, "pt": Point(1, 2.5, s), "c": Color.RED, "u": uuid.uuid4(), "dt": datetime.date(2026, 1, 1)}
    bad = {"k": lst, "bad": {1, 2, 3}}

    # Warm up caches/types
    cxjson.dumps(dct)
    try:
        cxjson.dumps(bad)
    except TypeError:
        pass
    gc.collect()

    rc_s = sys.getrefcount(s)
    rc_lst = sys.getrefcount(lst)
    rc_dct = sys.getrefcount(dct)
    rc_bad = sys.getrefcount(bad)

    for _ in range(200):
        _ = cxjson.dumps(dct)
        _ = cxjson.dumps_str(dct)
        try:
            cxjson.dumps(bad)
        except TypeError:
            pass

    gc.collect()
    assert sys.getrefcount(s) == rc_s, f"str refcount changed: {rc_s} -> {sys.getrefcount(s)}"
    assert sys.getrefcount(lst) == rc_lst, f"list refcount changed: {rc_lst} -> {sys.getrefcount(lst)}"
    assert sys.getrefcount(dct) == rc_dct, f"dict refcount changed: {rc_dct} -> {sys.getrefcount(dct)}"
    assert sys.getrefcount(bad) == rc_bad, f"bad dict refcount changed: {rc_bad} -> {sys.getrefcount(bad)}"
    print("PASS: exact refcounts preserved across success & error paths")


def test_split_table_gc_cycles():
    import weakref

    class Node:
        pass

    payload = b'{"id":1,"children":[1,2,3]}'
    for _ in range(5):
        cxjson.loads(payload)

    refs = []
    for _ in range(2000):
        d = cxjson.loads(payload)
        n = Node()
        n.data = d
        d["children"].append(n)
        refs.append(weakref.ref(n))
    del d, n
    gc.collect()
    gc.collect()
    alive = sum(r() is not None for r in refs)
    assert alive <= 3, f"GC cycle leak in split-table dicts: {alive} of 2000 objects still alive"

    # Verify gc.is_tracked parity for nested containers
    sample = cxjson.loads(payload)
    assert gc.is_tracked(sample), "split-table dict containing a list must be GC-tracked"
    print(f"PASS: split-table GC cycles collected (alive={alive}/2000, gc.is_tracked=True)")


if __name__ == "__main__":
    test_split_table_dicts_and_mutations()
    test_split_table_gc_cycles()
    test_schema_switching_and_fallbacks()
    test_malformed_json_error_paths()
    test_unicode_and_escaped_strings()
    test_parallel_dumps_and_serializer_errors()
    test_refcounts_preserved()
    print("\nALL MEMORY LEAK AND REFCOUNT TESTS PASSED!")
