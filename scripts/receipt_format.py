"""RATi receipt v1. Format and test vector: contracts/rati-receipt/v1/."""

from __future__ import annotations

import hashlib
import json
import re
from typing import Any

FIELDS = {
    "schema", "profile", "subject", "claim", "rules", "facts", "result", "outcome",
    "evaluated_at_unix_ms", "validity", "issuer", "sha256",
}


def _object(value: Any, keys: set[str]) -> None:
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError("Receipt object fields differ from v1")


def _string(value: Any) -> None:
    if not isinstance(value, str) or not value:
        raise ValueError("Receipt fields require nonempty strings")
    value.encode("utf-8", errors="strict")


def _pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in items:
        if key in result:
            raise ValueError("JSON object keys must be unique")
        result[key] = value
    return result


def _constant(value: str) -> None:
    raise ValueError(f"JSON requires finite numbers: {value}")


def read_json(text: str) -> Any:
    return json.loads(text, object_pairs_hook=_pairs, parse_constant=_constant)


def json_text(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)


def _validate(receipt: Any) -> None:
    _object(receipt, FIELDS)
    for key in FIELDS - {"subject", "rules", "facts", "result"}:
        _string(receipt[key])
    if receipt["schema"] != "rati.receipt.v1" or receipt["validity"] != "snapshot":
        raise ValueError("Unsupported receipt version or validity")
    if receipt["outcome"] not in {"pass", "fail", "unknown"}:
        raise ValueError("Unknown outcome")
    if not re.fullmatch(r"0|[1-9][0-9]*", receipt["evaluated_at_unix_ms"]):
        raise ValueError("Check time must be decimal Unix milliseconds")
    _object(receipt["subject"], {"kind", "id"})
    _object(receipt["rules"], {"id", "version", "sha256"})
    for group in ("subject", "rules"):
        for value in receipt[group].values():
            _string(value)
    for digest in (receipt["sha256"], receipt["rules"]["sha256"]):
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Expected lowercase SHA-256")
    for key in ("facts", "result"):
        _object(receipt[key], {"media_type", "content"})
        _string(receipt[key]["content"])
        if receipt[key]["media_type"] != "application/json":
            raise ValueError("Expected JSON evidence")
        if not isinstance(read_json(receipt[key]["content"]), dict):
            raise ValueError("Evidence must be a JSON object")


def digest(receipt: dict[str, Any]) -> str:
    body = {key: value for key, value in receipt.items() if key != "sha256"}
    raw = json.dumps(body, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def seal(body: dict[str, Any]) -> dict[str, Any]:
    """Take a detached copy of an envelope and bind its exact evidence text."""
    receipt = read_json(json_text({**body, "sha256": "0" * 64}))
    _validate(receipt)
    receipt["sha256"] = digest(receipt)
    return receipt


def verify(receipt: Any) -> bool:
    """Check envelope integrity. Profile-specific replay is a separate check."""
    try:
        _validate(receipt)
        return digest(receipt) == receipt["sha256"]
    except (ValueError, TypeError, KeyError, UnicodeError, RecursionError):
        return False


def blob(value: Any) -> dict[str, str]:
    return {"media_type": "application/json", "content": json_text(value)}
