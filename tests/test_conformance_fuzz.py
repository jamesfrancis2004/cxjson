"""RFC 8259 conformance, fork() safety, and randomized fuzzing tests vs orjson."""
import math
import os
import random
import sys
import cxjson
import orjson


def test_rfc8259_valid():
    valid_cases = [
        b"null", b"true", b"false", b"0", b"-0", b"0.0", b"-0.0",
        b"1", b"-1", b"42", b"1e0", b"1E+0", b"1e-0", b"0e10",
        b"1e308", b"-1e308", b"1e-308", b"5e-324", b"2.2250738585072014e-308",
        b"1.7976931348623157e308", b"9007199254740991", b"-9007199254740991",
        b"9223372036854775807", b"-9223372036854775808", b"18446744073709551615",
        b'""', b'"a"', b'"\\""', b'"\\\\"', b'"\\/"', b'"\\b\\f\\n\\r\\t"',
        b'"\\u0000\\u001f\\u0020\\u007f"',
        b'"\\uD83D\\uDE00"',  # valid surrogate pair -> 😀
        b'"\\uD834\\uDD1E"',  # valid surrogate pair -> 𝄞
        b'[]', b'{}', b'[1,2,3]', b'{"a":1,"b":2}', b'{"a":1,"a":2}',
        b' \t\r\n{"k" \t: \r\n[1, true, false, null, "x"]}\r\n\t ',
        b'{"": ""}',
        b'{"a": []}',
        b'[[]]',
        b'[[[]]]',
    ]
    for raw in valid_cases:
        c = cxjson.loads(raw)
        o = orjson.loads(raw)
        assert c == o, f"Mismatch on {raw!r}: {c!r} != {o!r}"
        if isinstance(o, float):
            assert math.copysign(1.0, c) == math.copysign(1.0, o), f"Sign mismatch on {raw!r}"
    print(f"PASS: {len(valid_cases)} RFC 8259 valid cases match orjson exactly")


def test_rfc8259_invalid():
    invalid_cases = [
        b"", b"   ", b"\t\r\n",
        b"tru", b"truee", b"fals", b"falsee", b"nul", b"nulll", b"True", b"False", b"None",
        b"NaN", b"Infinity", b"-Infinity", b"+1", b".5", b"1.", b"-", b"-.", b"01", b"-01",
        b"00", b"1e", b"1e+", b"1e-", b"1.e1", b"1e1.5", b"1e99999", b"-1e99999",
        b'"unterminated', b'"bad\\escape"', b'"\\x20"', b'"\\u123"', b'"\\u123g"',
        b'"\\uD800"', b'"\\uDFFF"', b'"\\uD800a"', b'"\\uD800\\u0041"', b'"\\uDC00\\uD800"',
        b'"raw\x00null"', b'"raw\nnewline"', b'"raw\ttab"', b'"raw\x1fctrl"',
        b"[", b"]", b"{", b"}", b"[1,]", b'{"a":1,}', b"[,]", b"{,}",
        b"{1:2}", b'{"a"}', b'{"a":}', b'{:"a"}', b"[1 2]", b'{"a":1 "b":2}',
        b"1 2", b"true false", b"{}[]",
        b"\xef\xbb\xbf{}",  # UTF-8 BOM
        b'"\xff"', b'"\xc0\xaf"', b'"\xe0\x80\xaf"', b'"\xed\xa0\x80"', b'"\xf4\x90\x80\x80"',
    ]
    for raw in invalid_cases:
        c_err = o_err = False
        try:
            cxjson.loads(raw)
        except cxjson.JSONDecodeError:
            c_err = True
        try:
            orjson.loads(raw)
        except orjson.JSONDecodeError:
            o_err = True
        assert c_err and o_err, f"Error mismatch on {raw!r}: c_err={c_err}, o_err={o_err}"
    print(f"PASS: {len(invalid_cases)} RFC 8259 invalid cases rejected cleanly (matching orjson)")


def test_fork_safety():
    if not hasattr(os, "fork"):
        print("SKIP: fork() not available on this platform")
        return
    # Warm up parallel serializer BEFORE fork()
    big = [{"id": i, "text": f"payload_{i}_" + "x" * 200, "vals": [i, i + 1, i + 2]} for i in range(3000)]
    parent_out = cxjson.dumps(big)
    assert parent_out == orjson.dumps(big)

    pid = os.fork()
    if pid == 0:
        try:
            child_out = cxjson.dumps(big)
            assert child_out == parent_out
            child_out2 = cxjson.dumps(big)
            assert child_out2 == parent_out
            os._exit(0)
        except BaseException:
            os._exit(1)
    _, status = os.waitpid(pid, 0)
    assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, f"Child failed after fork: status={status}"
    print("PASS: pthread_atfork() safety verified (parallel dumps before & after os.fork())")


def test_fuzz_random_bytes():
    rng = random.Random(1337)
    # 1. Pure random / mutated byte fuzzing (10,000 inputs)
    seeds = [
        b'{"a":1,"b":[true,false,null,"hi\\n\\u1234",3.14e-5]}',
        b'[{"id":1,"name":"x"},{"id":2,"name":"y"},{"id":3,"name":"z"}]',
        b'[[1.5,-2.5],[3.5,-4.5]]',
    ]
    for i in range(10000):
        if i % 3 == 0:
            n = rng.randint(0, 64)
            buf = bytes(rng.getrandbits(8) for _ in range(n))
        else:
            b = bytearray(rng.choice(seeds))
            for _ in range(rng.randint(1, 4)):
                op = rng.randint(0, 2)
                if op == 0 and b:
                    b[rng.randrange(len(b))] = rng.getrandbits(8)
                elif op == 1 and b:
                    del b[rng.randrange(len(b))]
                else:
                    b.insert(rng.randrange(len(b) + 1), rng.getrandbits(8))
            buf = bytes(b)
        try:
            cv = cxjson.loads(buf)
            ov = orjson.loads(buf)
            assert cv == ov, f"Fuzz value mismatch on {buf!r}"
        except cxjson.JSONDecodeError:
            try:
                orjson.loads(buf)
                raise AssertionError(f"cxjson rejected valid JSON accepted by orjson: {buf!r}")
            except orjson.JSONDecodeError:
                pass

    # 2. Structured random round-trip fuzzing (3,000 objects)
    def gen_obj(depth):
        if depth <= 0:
            t = rng.randint(0, 5)
            if t == 0: return None
            if t == 1: return rng.choice([True, False])
            if t == 2: return rng.randint(-10**12, 10**12)
            if t == 3: return rng.uniform(-1e6, 1e6)
            if t == 4: return "".join(rng.choices("abcXYZ012\"\\\n\t\x00é日🔥", k=rng.randint(0, 25)))
            return ""
        t = rng.randint(0, 3)
        if t == 0:
            return [gen_obj(depth - 1) for _ in range(rng.randint(0, 6))]
        if t == 1:
            return {f"k{rng.randint(0, 8)}": gen_obj(depth - 1) for _ in range(rng.randint(0, 6))}
        return gen_obj(0)

    for _ in range(3000):
        obj = gen_obj(3)
        c_bytes = cxjson.dumps(obj)
        o_bytes = orjson.dumps(obj)
        assert c_bytes == o_bytes
        assert cxjson.loads(c_bytes) == orjson.loads(o_bytes)

    print("PASS: 10,000 mutated-byte fuzz cases + 3,000 structured round-trip fuzz cases matched orjson 100%")


if __name__ == "__main__":
    test_rfc8259_valid()
    test_rfc8259_invalid()
    test_fork_safety()
    test_fuzz_random_bytes()
    print("ALL CONFORMANCE, FORK, AND FUZZ TESTS PASSED!")
