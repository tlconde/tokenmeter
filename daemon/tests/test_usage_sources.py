"""Unit tests for multi-service usage source readers."""

import json
from pathlib import Path

from usage_sources import _parse_codex_rate_limits, assemble_payload, codex_usage


def test_parse_codex_rate_limits_payload_root():
    line = json.dumps(
        {
            "type": "event_msg",
            "payload": {
                "type": "token_count",
                "info": {"model_context_window": 258400},
                "rate_limits": {
                    "primary": {
                        "used_percent": 13.0,
                        "window_minutes": 300,
                        "resets_at": 1781278325,
                    },
                    "secondary": {
                        "used_percent": 32.0,
                        "window_minutes": 10080,
                        "resets_at": 1781616467,
                    },
                },
            },
        }
    )
    snap = _parse_codex_rate_limits(line)
    assert snap is not None
    assert snap["primary"]["used_percent"] == 13.0


def test_parse_codex_rate_limits_legacy_info_root():
    line = json.dumps(
        {
            "type": "event_msg",
            "payload": {
                "type": "token_count",
                "info": {
                    "rate_limits": {
                        "primary": {"used_percent": 6.0, "resets_at": 1781054743},
                        "secondary": {"used_percent": 8.0, "resets_at": 1781616467},
                    }
                },
            },
        }
    )
    assert _parse_codex_rate_limits(line) is not None


def test_assemble_payload_backward_compatible_top_level():
    payload = assemble_payload(
        {"s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": True},
        codex={"s": 12, "sr": 95, "w": 40, "wr": 6100, "ok": True},
        cursor={"s": 0, "sr": 0, "w": 22, "wr": 0, "ok": True},
    )
    assert payload["s"] == 45
    assert payload["svc"]["cx"]["s"] == 12
    assert payload["svc"]["cu"]["w"] == 22


def test_codex_usage_reads_local_sessions():
    result = codex_usage()
    sessions = Path.home() / ".codex" / "sessions"
    if not any(sessions.rglob("*.jsonl")):
        assert result["ok"] is False
    else:
        assert isinstance(result["s"], int)
        assert isinstance(result["w"], int)
