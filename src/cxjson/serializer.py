from typing import Any
from . import _cxjson


class JsonSerializer:
    """High-level JSON Serializer class backed by our C engine."""

    def __init__(self, indent: int | None = None) -> None:
        self.indent = indent

    def serialize(self, obj: Any) -> str:
        """Serialize a Python object to JSON string."""
        raw = _cxjson.dumps(obj)
        return raw.decode("utf-8") if isinstance(raw, bytes) else raw

    def serialize_bytes(self, obj: Any) -> bytes:
        """Serialize a Python object directly to JSON bytes (zero-copy)."""
        return _cxjson.dumps(obj)
