from .serializer import JsonSerializer
from ._cxjson import (
    dumps,
    loads,
    Fragment,
    JSONDecodeError,
    OPT_INDENT_2,
    OPT_NAIVE_UTC,
    OPT_NON_STR_KEYS,
    OPT_OMIT_MICROSECONDS,
    OPT_SERIALIZE_NUMPY,
    OPT_SORT_KEYS,
    OPT_STRICT_INTEGER,
    OPT_UTC_Z,
    OPT_PASSTHROUGH_SUBCLASS,
    OPT_PASSTHROUGH_DATETIME,
    OPT_APPEND_NEWLINE,
    OPT_PASSTHROUGH_DATACLASS,
    OPT_SERIALIZE_DATACLASS,
    OPT_SERIALIZE_UUID,
)

# orjson-compatible exception
JSONEncodeError = TypeError


def dumps_str(obj: object, /, default=None, option=None) -> str:
    """Serialize a Python object to a JSON string."""
    return dumps(obj, default=default, option=option).decode("utf-8")


__all__ = [
    "dumps",
    "dumps_str",
    "loads",
    "Fragment",
    "JSONDecodeError",
    "JSONEncodeError",
    "JsonSerializer",
    "OPT_INDENT_2",
    "OPT_NAIVE_UTC",
    "OPT_NON_STR_KEYS",
    "OPT_OMIT_MICROSECONDS",
    "OPT_SERIALIZE_NUMPY",
    "OPT_SORT_KEYS",
    "OPT_STRICT_INTEGER",
    "OPT_UTC_Z",
    "OPT_PASSTHROUGH_SUBCLASS",
    "OPT_PASSTHROUGH_DATETIME",
    "OPT_APPEND_NEWLINE",
    "OPT_PASSTHROUGH_DATACLASS",
    "OPT_SERIALIZE_DATACLASS",
    "OPT_SERIALIZE_UUID",
]
