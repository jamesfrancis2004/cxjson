"""Self-contained PGO training workload exercising all hot paths of cxjson."""
import dataclasses
import datetime
import random
import string
import uuid
import cxjson


@dataclasses.dataclass
class RegUser:
    id: int
    name: str
    score: float
    active: bool


@dataclasses.dataclass(slots=True)
class SlotUser:
    id: int
    name: str
    email: str
    score: float
    active: bool


def main():
    rng = random.Random(12345)
    alpha = string.ascii_letters + string.digits + " "

    def rstr(n):
        return "".join(rng.choices(alpha, k=n))

    # 1. Repeated-schema nested records (compact + pretty-printed + sorted)
    recs = [
        {
            "id": i,
            "name": rstr(14),
            "email": f"u{i}@example.com",
            "active": (i & 1) == 0,
            "score": rng.random() * 1000.0,
            "tags": [rstr(6), rstr(8), "json", "fast"],
            "meta": {
                "text": f"Hello \"world\" line {i}\nwith\ttabs & café 🚀",
                "lat": rng.uniform(-90.0, 90.0),
                "lng": rng.uniform(-180.0, 180.0),
                "empty_d": {},
                "empty_l": [],
                "none": None,
            },
        }
        for i in range(4_000)
    ]
    raw_compact = cxjson.dumps(recs)
    raw_pretty = cxjson.dumps(recs, option=cxjson.OPT_INDENT_2)
    raw_sorted = cxjson.dumps(recs, option=cxjson.OPT_SORT_KEYS)
    raw_both = cxjson.dumps(recs, option=cxjson.OPT_SORT_KEYS | cxjson.OPT_INDENT_2)
    for _ in range(5):
        cxjson.loads(raw_compact)
        cxjson.loads(raw_pretty)
        cxjson.dumps(recs)
        cxjson.dumps(recs, option=cxjson.OPT_INDENT_2)
        cxjson.dumps(recs, option=cxjson.OPT_SORT_KEYS)
        cxjson.dumps(recs, option=cxjson.OPT_SORT_KEYS | cxjson.OPT_INDENT_2)

    # 2. GeoJSON-style coordinate rings & flat numeric arrays
    coords = [[[rng.uniform(-180, 180), rng.uniform(-90, 90)] for _ in range(200)] for _ in range(150)]
    raw_coords = cxjson.dumps({"type": "MultiPolygon", "coordinates": coords})
    floats = [rng.uniform(-1e6, 1e6) for _ in range(100_000)]
    ints = [rng.randint(-10**12, 10**12) for _ in range(100_000)]
    raw_floats = cxjson.dumps(floats)
    raw_ints = cxjson.dumps(ints)
    for _ in range(4):
        cxjson.loads(raw_coords)
        cxjson.dumps(coords)
        cxjson.loads(raw_floats)
        cxjson.dumps(floats)
        cxjson.loads(raw_ints)
        cxjson.dumps(ints)

    # 3. Large unique-key dict & string array
    big_dict = {f"key_{i:06d}": f"value_{i:06d}_payload" for i in range(15_000)}
    raw_big_dict = cxjson.dumps(big_dict)
    strs = [rstr(24) for _ in range(30_000)]
    raw_strs = cxjson.dumps(strs)
    for _ in range(4):
        cxjson.loads(raw_big_dict)
        cxjson.dumps(big_dict)
        cxjson.dumps(big_dict, option=cxjson.OPT_SORT_KEYS)
        cxjson.loads(raw_strs)
        cxjson.dumps(strs)

    # 4. Special types: UUID, datetime, date, dataclasses
    uuids = [uuid.uuid4() for _ in range(10_000)]
    utc = datetime.timezone.utc
    dts = [datetime.datetime(2024, 1 + (i % 12), 1 + (i % 28), i % 24, i % 60, i % 60, i % 1000000, tzinfo=utc) for i in range(10_000)]
    dates = [datetime.date(2024, 1 + (i % 12), 1 + (i % 28)) for i in range(10_000)]
    rusers = [RegUser(i, f"u{i}", i * 1.5, True) for i in range(5_000)]
    susers = [SlotUser(i, f"u{i}", f"u{i}@x.com", i * 1.5, False) for i in range(5_000)]
    for _ in range(3):
        cxjson.dumps(uuids)
        cxjson.dumps(dts)
        cxjson.dumps(dates)
        cxjson.dumps(rusers)
        cxjson.dumps(susers)

    assert len(raw_sorted) > 0 and len(raw_both) > 0


if __name__ == "__main__":
    main()
