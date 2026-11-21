from typing import Any, Callable, Optional, Union

class JSONDecodeError(ValueError):
    msg: str
    doc: str
    pos: int
    lineno: int
    colno: int

JSONEncodeError = TypeError

class Fragment:
    contents: Union[bytes, str]
    def __init__(self, contents: Union[bytes, str], /) -> None: ...

class JsonSerializer:
    def dumps(
        self,
        obj: Any,
        /,
        default: Optional[Callable[[Any], Any]] = None,
        option: Optional[int] = None,
    ) -> bytes: ...
    def loads(self, obj: Union[bytes, bytearray, memoryview, str], /) -> Any: ...

def dumps(
    obj: Any,
    /,
    default: Optional[Callable[[Any], Any]] = None,
    option: Optional[int] = None,
) -> bytes: ...

def dumps_str(
    obj: Any,
    /,
    default: Optional[Callable[[Any], Any]] = None,
    option: Optional[int] = None,
) -> str: ...

def loads(obj: Union[bytes, bytearray, memoryview, str], /) -> Any: ...

OPT_INDENT_2: int
OPT_NAIVE_UTC: int
OPT_NON_STR_KEYS: int
OPT_OMIT_MICROSECONDS: int
OPT_SERIALIZE_NUMPY: int
OPT_SORT_KEYS: int
OPT_STRICT_INTEGER: int
OPT_UTC_Z: int
OPT_PASSTHROUGH_SUBCLASS: int
OPT_PASSTHROUGH_DATETIME: int
OPT_APPEND_NEWLINE: int
OPT_PASSTHROUGH_DATACLASS: int
OPT_SERIALIZE_DATACLASS: int
OPT_SERIALIZE_UUID: int
