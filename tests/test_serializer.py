import json
import cxjson
from cxjson import JsonSerializer


def test_serializer():
    serializer = JsonSerializer()

    test_samples = [
        # Primitives
        None,
        True,
        False,
        0,
        -42,
        1234567890,
        3.14159,
        0.0001,
        100.5,
        # Strings & escapes
        "hello world",
        'special chars: "quotes", \\backslash\\, \nnewline, \ttab',
        # Collections
        [],
        {},
        [1, 2, "three", True, None],
        (10, 20, 30),
        {
            "user": "Alice",
            "age": 30,
            "balance": 1542.75,
            "verified": True,
            "tags": ["admin", "editor"],
            "preferences": {"theme": "dark", "notifications": None},
        },
    ]

    print("=== Testing cxjson Serializer ===\n")
    for sample in test_samples:
        serialized_str = serializer.serialize(sample)
        raw_bytes = cxjson.dumps(sample)
        assert isinstance(raw_bytes, bytes), "cxjson.dumps should return bytes"
        assert raw_bytes == serialized_str.encode("utf-8"), "String and bytes encoding mismatch"
        assert cxjson.dumps_str(sample) == serialized_str, "dumps_str mismatch"

        # Verify against standard Python json.loads (which accepts both str and bytes)
        round_trip = json.loads(raw_bytes)
        if isinstance(sample, tuple):
            assert round_trip == list(sample), f"Mismatch on {sample}"
        else:
            assert round_trip == sample, f"Mismatch on {sample}: got {round_trip}"

        print(f"PASS: {repr(sample)[:40]:<40} -> {serialized_str[:50]}")

    # Verify that sets raise TypeError, matching standard json and orjson
    try:
        cxjson.dumps({1, 2, 3})
        raise AssertionError("Expected TypeError when serializing set")
    except TypeError as e:
        print(f"\nPASS: Set serialization properly rejected with TypeError: '{e}'")

    # Test boundary 64-bit integers, integer-valued floats, and long dict keys (>48 and >128 bytes)
    edge_cases = [
        [0, 1, -1, (1 << 30) - 1, 1 << 30, -(1 << 30), -(1 << 30) - 1, (1 << 63) - 1, 1 << 63, (1 << 64) - 1, -(1 << 63)],
        [0.0, -0.0, 1.0, -42.0, 1000000.0, 1e15, 1e20, 3.141592653589793],
        {"k" * 60: "v" * 60, "long_key_" + ("x" * 200): 12345, "esc_key_\"\\_\n": "esc_val_\"\\_\n"},
    ]
    for case in edge_cases:
        out = cxjson.dumps(case)
        assert json.loads(out) == case, f"Edge case mismatch: {case}"
    print("PASS: Boundary 64-bit ints, integer-valued floats, and long dict keys")

    # Test multithreaded concurrency & reentrancy (zero shared global state)
    import concurrent.futures

    def worker(tid):
        payload = {
            "tid": tid,
            "nums": [i * tid for i in range(200)],
            "floats": [float(i) + 0.5 for i in range(100)],
            "text": f"thread-{tid}-" + ("hello \"world\" 🌍 " * 30),
            "nested": [{"a": i, "b": (1 << 62) + i} for i in range(50)],
        }
        expected = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        for _ in range(50):
            res = cxjson.dumps(payload)
            assert res == expected
        return True

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(worker, range(8)))
        assert all(results)
    print("PASS: Multithreaded concurrent serialization (8 threads)")

    # Test memory bounds and zero retained buffer overhead
    import tracemalloc

    tracemalloc.start()
    cur_before, _ = tracemalloc.get_traced_memory()
    small_res = cxjson.dumps({"a": 1, "b": [1, 2, 3]})
    del small_res
    cur_after_small, _ = tracemalloc.get_traced_memory()

    # Check that no global scratch buffer is retained after dumps() completes
    retained_diff = cur_after_small - cur_before
    assert retained_diff <= 256, f"Unexpected retained memory after dumps: {retained_diff} bytes"

    # Also verify after a large (2MB) serialization that memory returns to baseline
    large_input = [i for i in range(250_000)]
    cur_before_large, _ = tracemalloc.get_traced_memory()
    large_res = cxjson.dumps(large_input)
    del large_res
    cur_after_large, _ = tracemalloc.get_traced_memory()
    assert cur_after_large - cur_before_large <= 256, (
        f"Unexpected retained memory after 2MB dumps: {cur_after_large - cur_before_large} bytes"
    )
    del large_input

    # Check that a 1MB string with 1 escape does not over-allocate 6MB upfront
    s_1mb = "a" * 500_000 + '"' + "b" * 499_999
    cur_base, _ = tracemalloc.get_traced_memory()
    tracemalloc.reset_peak()
    out_1mb = cxjson.dumps(s_1mb)
    _, peak_1mb = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    delta_peak = peak_1mb - cur_base
    assert len(out_1mb) == 1_000_003
    assert delta_peak < 2_200_000, f"Peak memory delta for 1MB string too high: {delta_peak} bytes"
    del out_1mb, s_1mb
    print(f"PASS: Memory bounds verified (1MB escaped string peak delta={delta_peak / (1024*1024):.2f} MB, retained=0)")

    print("\nAll serialization and error-handling tests passed!")


if __name__ == "__main__":
    test_serializer()
