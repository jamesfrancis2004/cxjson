"""Critical security, differential-fuzzing, buffer-boundary, and concurrency tests for Schema JIT."""
import concurrent.futures
import json
import os
import random
import subprocess
import sys
import cxjson

try:
    import orjson
    HAS_ORJSON = True
except ImportError:
    HAS_ORJSON = False


def test_buffer_boundaries_and_edge_values():
    # 1. Max 30 fields x 52-byte keys x 64-byte strings (tests exact buffer slack)
    keys = [f"k_{j:02d}_" + ("x" * 47) for j in range(30)]
    assert all(len(k) == 52 for k in keys)
    val64 = "a" * 64
    rows_comb = [{k: (val64 if j % 2 == 0 else (j * 1000000)) for j, k in enumerate(keys)} for _ in range(60)]
    out_comb = cxjson.dumps(rows_comb)
    assert json.loads(out_comb) == rows_comb
    rows_split = cxjson.loads(out_comb)
    out_split = cxjson.dumps(rows_split)
    assert out_split == out_comb

    # 2. Inline 0..99 int boundaries + negative + 64-bit limits + overflow TypeError
    int_vals = [0, 1, 8, 9, 10, 11, 55, 98, 99, 100, 101, -1, -99, 2**30 - 1, 2**30, -(2**63), 2**64 - 1]
    rows_int = [{"a": int_vals[i % len(int_vals)], "b": i % 100, "c": i % 10} for i in range(80)]
    assert json.loads(cxjson.dumps(rows_int)) == rows_int
    assert json.loads(cxjson.dumps(cxjson.loads(cxjson.dumps(rows_int)))) == rows_int

    rows_ovf = [{"a": i, "b": 1} for i in range(50)]
    rows_ovf[40]["a"] = 1 << 65
    try:
        cxjson.dumps(rows_ovf)
        raise AssertionError("Expected TypeError for >64-bit int in JIT")
    except TypeError:
        pass

    # 3. Huge strings (> 4KB), escaped strings, UTF-8, NaN/Inf/-Inf, empty containers
    rows_weird = [
        {
            "s": "ok" if i % 4 == 0 else ("q\"\\n\x00\x1f" if i % 4 == 1 else ("🚀" * 10 if i % 4 == 2 else "z" * 5000)),
            "f": float("nan") if i == 35 else (float("inf") if i == 36 else (float("-inf") if i == 37 else i * 0.25)),
            "d": {} if i % 2 == 0 else {"nested": [1, 2, 3]},
            "l": [] if i % 2 == 0 else [i, "x"],
            "n": None if i % 3 == 0 else "not_none",
            "b": (i % 2 == 0),
        }
        for i in range(80)
    ]
    out_w = cxjson.dumps(rows_weird)
    if HAS_ORJSON:
        assert out_w == orjson.dumps(rows_weird)
    rows_w_split = cxjson.loads(out_w)
    assert cxjson.dumps(rows_w_split) == out_w

    # 4. Tombstones + key-order changes + non-str key error in combined dict array
    rows_tomb = [{"a": i, "b": i + 1, "c": i + 2} for i in range(60)]
    del rows_tomb[40]["b"]
    rows_tomb[41] = {"c": 1, "b": 2, "a": 3}  # swapped key order
    assert json.loads(cxjson.dumps(rows_tomb)) == rows_tomb
    rows_bad = [{"a": i, "b": i} for i in range(50)]
    rows_bad[45] = {"a": 1, 999: 2}
    try:
        cxjson.dumps(rows_bad)
        raise AssertionError("Expected TypeError for int dict key")
    except TypeError:
        pass
    print("PASS: test_buffer_boundaries_and_edge_values")


def test_differential_fuzz_vs_no_jit():
    rng = random.Random(2026)

    def rand_val(depth=0):
        choices = ["int", "float", "str", "bool", "none"]
        if depth < 2:
            choices += ["list", "dict"]
        kind = rng.choice(choices)
        if kind == "int":
            return rng.choice([0, 1, 9, 10, 99, 100, -1, -42, rng.randint(-10**12, 10**12)])
        if kind == "float":
            return rng.choice([0.0, -0.0, 1.5, -123.456, 1e-10, 1e20])
        if kind == "str":
            return rng.choice(["", "a", "hello", 'a"b\\c\n', "café", "🚀" * 3, "x" * 70])
        if kind == "bool":
            return rng.choice([True, False])
        if kind == "none":
            return None
        if kind == "list":
            return [rand_val(depth + 1) for _ in range(rng.randint(0, 4))]
        return {f"sub_{k}": rand_val(depth + 1) for k in range(rng.randint(0, 3))}

    payloads = []
    for trial in range(40):
        nfields = rng.randint(1, 20)
        keys = [f"f{trial}_{j}" for j in range(nfields)]
        rows = [{k: rand_val(0) for k in keys} for _ in range(45)]
        # Mix both combined-table and split-table arrays
        out_jit_comb = cxjson.dumps(rows)
        if HAS_ORJSON:
            assert out_jit_comb == orjson.dumps(rows), f"combined fuzz mismatch on trial {trial}"
        split_rows = cxjson.loads(out_jit_comb)
        out_jit_split = cxjson.dumps(split_rows)
        assert out_jit_split == out_jit_comb, f"split fuzz mismatch on trial {trial}"
        payloads.append(out_jit_comb)

    # Also verify CXJSON_JIT=0 in a clean subprocess matches CXJSON_JIT=1 byte-for-byte
    script = (
        "import sys, cxjson; "
        "raw = sys.stdin.buffer.read(); "
        "obj = cxjson.loads(raw); "
        "sys.stdout.buffer.write(cxjson.dumps(obj))"
    )
    env0 = dict(os.environ, CXJSON_JIT="0", PYTHONPATH="src")
    for p in payloads[:10]:
        proc = subprocess.run([sys.executable, "-c", script], input=p, capture_output=True, env=env0, check=True)
        assert proc.stdout == p, "CXJSON_JIT=1 vs CXJSON_JIT=0 mismatch!"
    print("PASS: test_differential_fuzz_vs_no_jit (40 schemas + CXJSON_JIT=0 parity)")


def test_concurrent_compile_and_execute():
    # Spawn 8 threads compiling >64 distinct schemas concurrently (tests mutex + cache saturation + W^X safety)
    def worker(tid):
        for s in range(15):
            k1 = f"t{tid}_s{s}_a"
            k2 = f"t{tid}_s{s}_b"
            k3 = f"t{tid}_s{s}_c"
            rows = [{k1: i % 100, k2: f"v{i}", k3: (i % 2 == 0)} for i in range(60)]
            b1 = cxjson.dumps(rows)
            split = cxjson.loads(b1)
            b2 = cxjson.dumps(split)
            assert b1 == b2
            assert json.loads(b2) == rows
        return True

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        results = list(ex.map(worker, range(8)))
    assert all(results)
    print("PASS: test_concurrent_compile_and_execute (8 threads, 120 schemas)")


def test_non_interned_combined_keys():
    # Force distinct (non-interned) PyUnicodeObject key pointers on every row across lengths 1..17
    base_keys = ["a", "ab", "abc", "abcd", "abcde", "abcdefg", "abcdefgh", "abcdefghi", "0123456789abcdef", "0123456789abcdefg"]
    rows = []
    for i in range(60):
        # Fresh str allocation per row via decode("ascii") so pointers differ from row 0
        d = {k.encode("ascii").decode("ascii") + "": (i if idx % 3 == 0 else (f"val_{i:04d}_padding_long_enough_for_simd" if idx % 3 == 1 else (i & 1) == 0))
             for idx, k in enumerate(base_keys)}
        rows.append(d)
    # Verify keys actually have distinct pointers for len >= 2
    assert len({id(list(r.keys())[3]) for r in rows}) > 10
    expected = json.dumps(rows, separators=(",", ":")).encode("utf-8")
    assert cxjson.dumps(rows) == expected

    # Adversarial: row 35 has a same-length key with 1 differing byte ("abce" instead of "abcd")
    rows_mut = [dict(r) for r in rows]
    bad = {}
    for k, v in rows_mut[35].items():
        bad["abce" if k == "abcd" else k] = v
    rows_mut[35] = bad
    assert cxjson.dumps(rows_mut) == json.dumps(rows_mut, separators=(",", ":")).encode("utf-8")
    print("PASS: test_non_interned_combined_keys (distinct ptrs len 1..17 + 1-byte diff guard)")


def test_dict_of_dicts_and_inline_nested():
    # 1. Dict-of-dicts (combined + split + tombstones + heterogeneous entries)
    dod_comb = {
        f"k{i:03d}": {
            "id": i,
            "name": f"pkg_{i}_" + ("x" * (i % 40)),
            "score": round(i * 1.25, 2),
            "ok": (i & 1) == 0,
            "meta": {"sha": f"{i:064x}", "size": i * 1024, "yanked": False, "note": None, "ratio": 0.5},
        }
        for i in range(60)
    }
    # Delete several keys to introduce dict tombstones
    for k in ("k005", "k019", "k033", "k047"):
        del dod_comb[k]
    # Mutate a few entries to test fallback paths (empty dict, None, different sub-dict shape, non-dict)
    dod_comb["k010"]["meta"] = {}
    dod_comb["k020"]["meta"] = None
    dod_comb["k030"]["meta"] = {"other": "shape", "extra": 123}
    dod_comb["k040"] = {"different": 1}
    dod_comb["k050"] = [1, 2, 3]

    expected = json.dumps(dod_comb, separators=(",", ":")).encode("utf-8")
    out_comb = cxjson.dumps(dod_comb)
    assert out_comb == expected, "combined dict-of-dicts + inline nested mismatch"

    # Round-trip through cxjson.loads() to exercise split-table dicts + split sub-dicts
    dod_split = cxjson.loads(out_comb)
    out_split = cxjson.dumps(dod_split)
    assert out_split == expected, "split dict-of-dicts + inline nested mismatch"

    # 2. List-of-dicts with inline nested homogeneous dicts (split & combined)
    rows = [
        {
            "id": i,
            "sub": {"a": f"s_{i}", "b": i - 50, "c": i * 0.5, "d": (i & 1) == 0, "e": None},
            "tag": f"t{i}",
        }
        for i in range(80)
    ]
    rows[15]["sub"] = {}
    rows[25]["sub"] = None
    rows[35]["sub"] = {"a": 'esc"quote\\slash\n', "b": 2**62, "c": float("nan"), "d": True, "e": "not_none"}
    rows[45]["sub"] = {"x": 1, "y": 2}
    # Check exact parity with CXJSON_JIT=0 (including NaN -> null)
    b_jit = cxjson.dumps(rows)
    b_split_jit = cxjson.dumps(cxjson.loads(b_jit))
    assert b_jit == b_split_jit
    raw_in = json.dumps(rows, separators=(",", ":"), allow_nan=True)
    code = (
        "import json, sys, cxjson; "
        "rows = json.loads(sys.stdin.read()); "
        "sys.stdout.buffer.write(cxjson.dumps(rows))"
    )
    env = dict(os.environ, CXJSON_JIT="0")
    out_nojit = subprocess.check_output([sys.executable, "-c", code], input=raw_in.encode("utf-8"), env=env)
    assert b_jit == out_nojit, "inline nested dict mismatch vs CXJSON_JIT=0"
    # Verify >64-bit int inside inline nested dict properly propagates TypeError
    rows[35]["sub"]["b"] = 10**20
    try:
        cxjson.dumps(rows)
        assert False, "Expected TypeError for >64-bit int in inline nested dict"
    except TypeError:
        pass
    print("PASS: test_dict_of_dicts_and_inline_nested (tombstones, split/combined, fallbacks, JIT=0 parity)")


if __name__ == "__main__":
    test_buffer_boundaries_and_edge_values()
    test_differential_fuzz_vs_no_jit()
    test_concurrent_compile_and_execute()
    test_non_interned_combined_keys()
    test_dict_of_dicts_and_inline_nested()
    print("\nALL JIT SECURITY & CONCURRENCY TESTS PASSED!")
