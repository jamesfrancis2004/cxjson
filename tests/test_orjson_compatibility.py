import cxjson
import orjson
import uuid
import datetime
import dataclasses
import enum


@dataclasses.dataclass
class User:
    id: int
    name: str
    active: bool = True


class Status(enum.Enum):
    PENDING = "pending"
    DONE = "done"


def test_orjson_compatibility():
    print("=== Testing orjson Compatibility ===")

    # 1. Primitives & Nested Structures
    samples = [
        None,
        True,
        False,
        0,
        -42,
        1234567890,
        3.14159,
        float("nan"),
        float("inf"),
        float("-inf"),
        "hello world",
        'quotes: "hello", \\backslash\\, \t, \n',
        [],
        {},
        [1, 2, "three", True, None],
        {"a": 1, "b": [2, 3], "c": {"d": "nested"}},
    ]

    for s in samples:
        c_res = cxjson.dumps(s)
        o_res = orjson.dumps(s)
        assert c_res == o_res, f"Mismatch on {s}: cxjson={c_res!r} vs orjson={o_res!r}"
    print("PASS: Primitives and nested structures match orjson exactly")

    # 2. OPT_INDENT_2
    for s in [
        [],
        {},
        [1, 2],
        {"a": 1},
        {"a": [1, {"b": 2}], "c": 3},
    ]:
        c_res = cxjson.dumps(s, option=cxjson.OPT_INDENT_2)
        o_res = orjson.dumps(s, option=orjson.OPT_INDENT_2)
        assert c_res == o_res, f"INDENT_2 mismatch on {s}:\n{c_res.decode()}\nvs\n{o_res.decode()}"
    print("PASS: OPT_INDENT_2 matches orjson exactly")

    # 3. OPT_APPEND_NEWLINE
    assert cxjson.dumps({"a": 1}, option=cxjson.OPT_APPEND_NEWLINE) == orjson.dumps(
        {"a": 1}, option=orjson.OPT_APPEND_NEWLINE
    )
    print("PASS: OPT_APPEND_NEWLINE matches orjson exactly")

    # 4. OPT_SORT_KEYS
    d = {"z": 1, "a": 2, "m": 3, "b": {"y": 10, "x": 20}}
    assert cxjson.dumps(d, option=cxjson.OPT_SORT_KEYS) == orjson.dumps(
        d, option=orjson.OPT_SORT_KEYS
    )
    assert cxjson.dumps(
        d, option=cxjson.OPT_SORT_KEYS | cxjson.OPT_INDENT_2
    ) == orjson.dumps(d, option=orjson.OPT_SORT_KEYS | orjson.OPT_INDENT_2)
    print("PASS: OPT_SORT_KEYS matches orjson exactly")

    # 5. OPT_NON_STR_KEYS
    non_str_dict = {1: "one", 2.5: "two point five", True: "true", None: "null"}
    assert cxjson.dumps(
        non_str_dict, option=cxjson.OPT_NON_STR_KEYS
    ) == orjson.dumps(non_str_dict, option=orjson.OPT_NON_STR_KEYS)
    assert cxjson.dumps(
        non_str_dict, option=cxjson.OPT_NON_STR_KEYS | cxjson.OPT_SORT_KEYS
    ) == orjson.dumps(non_str_dict, option=orjson.OPT_NON_STR_KEYS | orjson.OPT_SORT_KEYS)
    print("PASS: OPT_NON_STR_KEYS matches orjson exactly")

    # Non-str keys without OPT_NON_STR_KEYS should raise TypeError("Dict key must be str")
    try:
        cxjson.dumps({1: "one"})
        raise AssertionError("Expected TypeError")
    except TypeError as e:
        assert str(e) == "Dict key must be str"
    print("PASS: Dict key must be str rejected correctly")

    # 6. OPT_STRICT_INTEGER
    max_53 = (1 << 53) - 1
    assert cxjson.dumps(max_53, option=cxjson.OPT_STRICT_INTEGER) == orjson.dumps(
        max_53, option=orjson.OPT_STRICT_INTEGER
    )
    try:
        cxjson.dumps(1 << 53, option=cxjson.OPT_STRICT_INTEGER)
        raise AssertionError("Expected TypeError for 53-bit overflow")
    except TypeError as e:
        assert str(e) == "Integer exceeds 53-bit range"
    print("PASS: OPT_STRICT_INTEGER matches orjson exactly")

    # 7. Integer 64-bit bounds
    try:
        cxjson.dumps(1 << 64)
        raise AssertionError("Expected TypeError for 64-bit overflow")
    except TypeError as e:
        assert str(e) == "Integer exceeds 64-bit range"
    print("PASS: 64-bit integer overflow error matches orjson exactly")

    # 8. UUID
    u = uuid.uuid4()
    assert cxjson.dumps(u) == orjson.dumps(u)
    assert cxjson.dumps({"id": u}) == orjson.dumps({"id": u})
    print("PASS: UUID serialization matches orjson exactly")

    # 9. Datetime, Date, Time
    dt = datetime.datetime(2023, 10, 15, 14, 30, 45, 123456)
    d_obj = datetime.date(2023, 10, 15)
    t_obj = datetime.time(14, 30, 45, 123456)
    assert cxjson.dumps(dt) == orjson.dumps(dt)
    assert cxjson.dumps(d_obj) == orjson.dumps(d_obj)
    assert cxjson.dumps(t_obj) == orjson.dumps(t_obj)
    assert cxjson.dumps(
        dt, option=cxjson.OPT_NAIVE_UTC | cxjson.OPT_UTC_Z
    ) == orjson.dumps(dt, option=orjson.OPT_NAIVE_UTC | orjson.OPT_UTC_Z)
    assert cxjson.dumps(
        t_obj, option=cxjson.OPT_OMIT_MICROSECONDS
    ) == orjson.dumps(t_obj, option=orjson.OPT_OMIT_MICROSECONDS)
    print("PASS: Datetime options match orjson exactly")

    # 10. Dataclasses
    user = User(id=101, name="Alice")
    assert cxjson.dumps(user) == orjson.dumps(user)
    print("PASS: Dataclasses match orjson exactly")

    # 11. Enum
    assert cxjson.dumps(Status.PENDING) == orjson.dumps(Status.PENDING)
    assert cxjson.dumps({"status": Status.DONE}) == orjson.dumps({"status": Status.DONE})
    print("PASS: Enum matches orjson exactly")

    # 12. Fragment
    frag = cxjson.Fragment(b'{"raw": [1, 2, 3]}')
    o_frag = orjson.Fragment(b'{"raw": [1, 2, 3]}')
    assert cxjson.dumps({"f": frag}) == orjson.dumps({"f": o_frag})
    print("PASS: Fragment matches orjson exactly")

    # 13. default argument (positional & keyword)
    class CustomObj:
        def __init__(self, val):
            self.val = val

    def default_fn(obj):
        if isinstance(obj, CustomObj):
            return {"custom": obj.val}
        raise TypeError

    obj = CustomObj(42)
    assert cxjson.dumps(obj, default=default_fn) == orjson.dumps(obj, default=default_fn)
    assert cxjson.dumps(obj, default_fn) == orjson.dumps(obj, default_fn)
    print("PASS: default argument matches orjson exactly")

    # 14. Error compatibility
    class Unserializable:
        pass

    try:
        cxjson.dumps(Unserializable())
        raise AssertionError("Expected TypeError")
    except TypeError as e:
        assert str(e) == "Type is not JSON serializable: Unserializable"

    try:
        cxjson.dumps({"a": 1}, option=999999)
        raise AssertionError("Expected TypeError")
    except TypeError as e:
        assert str(e) == "Invalid opts"

    try:
        cxjson.dumps()
        raise AssertionError("Expected TypeError")
    except TypeError as e:
        assert "dumps() missing 1 required positional argument" in str(e)

    try:
        cxjson.dumps({"a": 1}, invalid_kw=1)
        raise AssertionError("Expected TypeError")
    except TypeError as e:
        assert "dumps() got an unexpected keyword argument" in str(e)

    print("PASS: All error messages match orjson exactly")
    print("\nALL ORJSON COMPATIBILITY TESTS PASSED!")


if __name__ == "__main__":
    test_orjson_compatibility()
