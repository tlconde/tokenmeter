"""Usage source readers for the Clawdmeter multi-service BLE payload.

Service keys: cl = Claude Code, cx = Codex CLI, cu = Cursor IDE.
"""

from __future__ import annotations

import json
import sqlite3
import time
from datetime import datetime, timezone
from pathlib import Path

import httpx

CODEX_SESSIONS_DIR = Path.home() / ".codex" / "sessions"
CURSOR_STATE_DB = (
    Path.home() / "Library/Application Support/Cursor/User/globalStorage/state.vscdb"
)

CURSOR_API_BASE = "https://api2.cursor.sh"
CURSOR_WEB_API = "https://cursor.com/api"

_last_known: dict[str, dict[str, int | bool]] = {
    "cl": {},
    "cx": {},
    "cu": {},
}


def empty_svc() -> dict[str, int | bool]:
    return {"s": 0, "sr": 0, "w": 0, "wr": 0, "ok": False}


def _clamp_pct(value: float) -> int:
    return max(0, min(100, int(round(value))))


def _reset_minutes_from_epoch(resets_at: object, now: float | None = None) -> int:
    if resets_at is None:
        return 0
    now = now or time.time()
    try:
        mins = (float(resets_at) - now) / 60.0
    except (TypeError, ValueError):
        return 0
    return int(round(mins)) if mins > 0 else 0


def _reset_minutes_from_iso(iso_ts: str, now: float | None = None) -> int:
    now = now or time.time()
    try:
        end = datetime.fromisoformat(iso_ts.replace("Z", "+00:00"))
        if end.tzinfo is None:
            end = end.replace(tzinfo=timezone.utc)
        mins = (end.timestamp() - now) / 60.0
    except (TypeError, ValueError):
        return 0
    return int(round(mins)) if mins > 0 else 0


def _remember(key: str, svc: dict[str, int | bool]) -> dict[str, int | bool]:
    if svc.get("ok"):
        _last_known[key] = dict(svc)
    return svc


def _fallback(key: str) -> dict[str, int | bool]:
    prev = _last_known.get(key) or {}
    if prev.get("ok"):
        return {
            "s": int(prev.get("s", 0)),
            "sr": int(prev.get("sr", 0)),
            "w": int(prev.get("w", 0)),
            "wr": int(prev.get("wr", 0)),
            "ok": False,
        }
    return empty_svc()


def _latest_session_files(limit: int = 2) -> list[Path]:
    if not CODEX_SESSIONS_DIR.is_dir():
        return []
    files = [p for p in CODEX_SESSIONS_DIR.rglob("*.jsonl") if p.is_file()]
    files.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return files[:limit]


def _parse_codex_rate_limits(line: str) -> dict | None:
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return None
    if obj.get("type") != "event_msg":
        return None
    payload = obj.get("payload") or {}
    if payload.get("type") != "token_count":
        return None
    info = payload.get("info") or {}
    # Newer Codex builds attach rate_limits on payload; older on info.
    rate_limits = payload.get("rate_limits") or info.get("rate_limits")
    if not isinstance(rate_limits, dict):
        return None
    primary = rate_limits.get("primary") or {}
    secondary = rate_limits.get("secondary") or {}
    if "used_percent" not in primary and "used_percent" not in secondary:
        return None
    return rate_limits


def _scan_jsonl_backwards(path: Path) -> dict | None:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    for line in reversed(lines):
        snap = _parse_codex_rate_limits(line)
        if snap is not None:
            return snap
    return None


def codex_usage() -> dict[str, int | bool]:
    """Read Codex rate limits from the newest session .jsonl snapshots.

    Schema (confirmed on this host, Jun 2026):
      event_msg.payload.type == "token_count"
      event_msg.payload.rate_limits (or legacy .info.rate_limits)
        .primary|.secondary: used_percent (float), window_minutes (int),
        resets_at (unix epoch)
    Maps primary (~5h) -> s/sr, secondary (weekly) -> w/wr.
    """
    for path in _latest_session_files(2):
        snap = _scan_jsonl_backwards(path)
        if snap is None:
            continue
        primary = snap.get("primary") or {}
        secondary = snap.get("secondary") or {}
        return _remember(
            "cx",
            {
                "s": _clamp_pct(float(primary.get("used_percent", 0))),
                "sr": _reset_minutes_from_epoch(primary.get("resets_at")),
                "w": _clamp_pct(float(secondary.get("used_percent", 0))),
                "wr": _reset_minutes_from_epoch(secondary.get("resets_at")),
                "ok": True,
            },
        )
    return _fallback("cx")


def _read_cursor_token() -> str | None:
    if not CURSOR_STATE_DB.is_file():
        return None
    try:
        conn = sqlite3.connect(f"file:{CURSOR_STATE_DB}?mode=ro", uri=True)
        try:
            row = conn.execute(
                "SELECT value FROM ItemTable WHERE key='cursorAuth/accessToken'"
            ).fetchone()
        finally:
            conn.close()
    except sqlite3.Error:
        return None
    if not row:
        return None
    token = row[0]
    if not isinstance(token, str) or len(token) < 20:
        return None
    return token


def _parse_cursor_auth_usage(data: dict) -> dict[str, int | bool] | None:
    """Legacy request-bucket usage (Enterprise / older Pro).

    Maps monthly included fast-request consumption -> w.
    No short rolling window in this API, so s=0 and sr=0.
    """
    buckets: list[tuple[int, int]] = []
    for value in data.values():
        if not isinstance(value, dict):
            continue
        used = value.get("numRequests")
        limit = value.get("maxRequestUsage")
        if isinstance(used, (int, float)) and isinstance(limit, (int, float)) and limit > 0:
            buckets.append((int(used), int(limit)))
    if not buckets:
        return None
    used, limit = max(buckets, key=lambda pair: pair[1])
    return {
        "s": 0,
        "sr": 0,
        "w": _clamp_pct(used / limit * 100),
        "wr": 0,
        "ok": True,
    }


def _billing_cycle_reset_minutes(data: dict) -> int:
    end = (
        data.get("billingCycleEnd")
        or data.get("billingPeriodEnd")
        or data.get("periodEnd")
    )
    if end is None:
        return 0
    try:
        epoch = float(end)
        if epoch > 1e12:
            epoch /= 1000.0
        return _reset_minutes_from_epoch(epoch)
    except (TypeError, ValueError):
        pass
    if isinstance(end, str):
        return _reset_minutes_from_iso(end)
    return 0


def _parse_cursor_plan_usage(data: dict) -> dict[str, int | bool] | None:
    """Spend-based billing period usage (Pro / Team / Ultra).

    Maps included-allowance spend consumed this period -> w.
    billingCycleEnd / billingPeriodEnd (when present) -> wr.
    No short window -> s=0, sr=0.
    """
    plan = data.get("planUsage") or data.get("plan_usage")
    if not isinstance(plan, dict):
        return None

    wr = _billing_cycle_reset_minutes(data)

    for pct_key in ("totalPercentUsed", "apiPercentUsed", "autoPercentUsed"):
        if pct_key in plan:
            try:
                return {
                    "s": 0,
                    "sr": 0,
                    "w": _clamp_pct(float(plan[pct_key])),
                    "wr": wr,
                    "ok": True,
                }
            except (TypeError, ValueError):
                continue

    used_amount: float | None = None
    limit_amount: float | None = None
    for used_key, limit_key in (
        ("totalSpend", "limit"),
        ("totalSpend", "includedSpend"),
        ("totalSpendCents", "includedSpendCents"),
        ("spendCents", "includedSpendCents"),
        ("usedCents", "limitCents"),
        ("used", "limit"),
    ):
        if used_key in plan and limit_key in plan:
            try:
                used_amount = float(plan[used_key])
                limit_amount = float(plan[limit_key])
            except (TypeError, ValueError):
                continue
            break

    if used_amount is None or limit_amount is None or limit_amount <= 0:
        return None

    return {
        "s": 0,
        "sr": 0,
        "w": _clamp_pct(used_amount / limit_amount * 100),
        "wr": wr,
        "ok": True,
    }


def _parse_cursor_usage_summary(data: dict) -> dict[str, int | bool] | None:
    """cursor.com/api/usage-summary shape (cookie auth).

    Tries planUsage first, then aggregates individualUsage / teamUsage hints.
    """
    parsed = _parse_cursor_plan_usage(data)
    if parsed is not None:
        return parsed

    for section_key in ("individualUsage", "teamUsage", "usage"):
        section = data.get(section_key)
        if isinstance(section, dict):
            parsed = _parse_cursor_plan_usage(section)
            if parsed is not None:
                return parsed
            parsed = _parse_cursor_auth_usage(section)
            if parsed is not None:
                return parsed

    if "percentUsed" in data:
        try:
            return {
                "s": 0,
                "sr": 0,
                "w": _clamp_pct(float(data["percentUsed"])),
                "wr": _reset_minutes_from_iso(data["billingPeriodEnd"])
                if isinstance(data.get("billingPeriodEnd"), str)
                else 0,
                "ok": True,
            }
        except (TypeError, ValueError):
            return None
    return None


async def cursor_usage() -> dict[str, int | bool]:
    """Read Cursor quota via api2.cursor.sh using the IDE access token.

    Token source: ~/Library/Application Support/Cursor/User/globalStorage/
    state.vscdb ItemTable key 'cursorAuth/accessToken'.

    Endpoints tried (first success wins):
      1. POST api2.cursor.sh/.../GetCurrentPeriodUsage (Bearer) — spend plans
      2. GET  api2.cursor.sh/auth/usage (Bearer) — request buckets
      3. GET  cursor.com/api/usage-summary (WorkosCursorSessionToken cookie)

    Mapping: monthly / billing-period consumption -> w; no short window -> s=0.
    """
    token = _read_cursor_token()
    if not token:
        return _fallback("cu")

    parsers = (
        _parse_cursor_plan_usage,
        _parse_cursor_auth_usage,
        _parse_cursor_usage_summary,
    )

    async with httpx.AsyncClient(timeout=20.0) as http:
        try:
            resp = await http.post(
                f"{CURSOR_API_BASE}/aiserver.v1.DashboardService/GetCurrentPeriodUsage",
                headers={
                    "Authorization": f"Bearer {token}",
                    "Content-Type": "application/json",
                    "Connect-Protocol-Version": "1",
                },
                json={},
            )
            if resp.status_code < 400:
                data = resp.json()
                for parser in parsers:
                    parsed = parser(data)
                    if parsed is not None:
                        return _remember("cu", parsed)
        except httpx.HTTPError:
            pass

        try:
            resp = await http.get(
                f"{CURSOR_API_BASE}/auth/usage",
                headers={"Authorization": f"Bearer {token}"},
            )
            if resp.status_code < 400:
                data = resp.json()
                for parser in parsers:
                    parsed = parser(data)
                    if parsed is not None:
                        return _remember("cu", parsed)
        except httpx.HTTPError:
            pass

        try:
            resp = await http.get(
                f"{CURSOR_WEB_API}/usage-summary",
                headers={"Cookie": f"WorkosCursorSessionToken={token}"},
            )
            if resp.status_code < 400:
                data = resp.json()
                for parser in parsers:
                    parsed = parser(data)
                    if parsed is not None:
                        return _remember("cu", parsed)
        except httpx.HTTPError:
            pass

    return _fallback("cu")


def claude_svc_from_poll(payload: dict | None) -> dict[str, int | bool]:
    if payload is None:
        return _fallback("cl")
    svc = {
        "s": int(payload.get("s", 0)),
        "sr": int(payload.get("sr", 0)),
        "w": int(payload.get("w", 0)),
        "wr": int(payload.get("wr", 0)),
        "ok": bool(payload.get("ok", False)),
    }
    return _remember("cl", svc)


async def gather_all_usage(claude_poll: dict | None) -> dict:
    """Poll Codex (local) and Cursor (network), merge with Claude poll result."""
    cx = codex_usage()
    cu = await cursor_usage()
    return assemble_payload(claude_poll, codex=cx, cursor=cu)


def assemble_payload(
    claude_poll: dict | None,
    *,
    codex: dict[str, int | bool] | None = None,
    cursor: dict[str, int | bool] | None = None,
) -> dict:
    """Build the combined BLE payload with backward-compatible top-level fields."""
    cl = claude_svc_from_poll(claude_poll)
    cx = codex if codex is not None else codex_usage()
    cu = cursor if cursor is not None else empty_svc()

    status = "unknown"
    if claude_poll is not None:
        status = str(claude_poll.get("st", "unknown"))

    return {
        "s": cl["s"],
        "sr": cl["sr"],
        "w": cl["w"],
        "wr": cl["wr"],
        "st": status,
        "ok": cl["ok"],
        "svc": {"cl": cl, "cx": cx, "cu": cu},
    }
