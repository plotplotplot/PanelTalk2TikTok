#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


DEFAULT_PORT = 40130


@dataclass
class EndpointResult:
    ok: bool
    status: int | None
    latency_ms: float
    payload: dict[str, Any] | None = None
    error: str | None = None


def fetch_json(base_url: str, path: str, timeout: float) -> EndpointResult:
    started = time.time()
    request = urllib.request.Request(f"{base_url}{path}", method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read()
            latency_ms = (time.time() - started) * 1000.0
            payload = json.loads(body.decode("utf-8")) if body else {}
            return EndpointResult(
                ok=True,
                status=response.status,
                latency_ms=latency_ms,
                payload=payload if isinstance(payload, dict) else {"value": payload},
            )
    except urllib.error.HTTPError as exc:
        latency_ms = (time.time() - started) * 1000.0
        error = f"HTTP {exc.code}"
        try:
            body = exc.read()
            if body:
                error_payload = json.loads(body.decode("utf-8"))
                if isinstance(error_payload, dict) and error_payload.get("error"):
                    error = f"{error}: {error_payload['error']}"
        except Exception:
            pass
        return EndpointResult(ok=False, status=exc.code, latency_ms=latency_ms, error=error)
    except Exception as exc:
        latency_ms = (time.time() - started) * 1000.0
        return EndpointResult(ok=False, status=None, latency_ms=latency_ms, error=str(exc))


def find_widget(node: dict[str, Any], widget_id: str) -> dict[str, Any] | None:
    if node.get("id") == widget_id:
        return node
    for child in node.get("children", []):
        if isinstance(child, dict):
            found = find_widget(child, widget_id)
            if found is not None:
                return found
    return None


def summarize_widget(widget: dict[str, Any] | None) -> dict[str, Any] | None:
    if not widget:
        return None
    summary: dict[str, Any] = {
        "id": widget.get("id"),
        "class": widget.get("class"),
        "visible": widget.get("visible"),
        "enabled": widget.get("enabled"),
    }
    for key in ("text", "checked", "value", "minimum", "maximum", "x", "y", "width", "height"):
        if key in widget:
            summary[key] = widget.get(key)
    return summary


def collect_samples(base_url: str, timeout: float, samples: int, interval: float) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for index in range(samples):
        health = fetch_json(base_url, "/health", timeout)
        playhead = fetch_json(base_url, "/playhead", timeout)
        rows.append(
            {
                "sample_index": index,
                "t": round(time.time(), 3),
                "health": {
                    "ok": health.ok,
                    "status": health.status,
                    "latency_ms": round(health.latency_ms, 1),
                    "heartbeat_age_ms": None if not health.payload else health.payload.get("main_thread_heartbeat_age_ms"),
                    "last_playhead_advance_age_ms": None if not health.payload else health.payload.get("last_playhead_advance_age_ms"),
                    "error": health.error,
                },
                "playhead": {
                    "ok": playhead.ok,
                    "status": playhead.status,
                    "latency_ms": round(playhead.latency_ms, 1),
                    "current_frame": None if not playhead.payload else playhead.payload.get("current_frame"),
                    "playback_active": None if not playhead.payload else playhead.payload.get("playback_active"),
                    "heartbeat_age_ms": None if not playhead.payload else playhead.payload.get("main_thread_heartbeat_age_ms"),
                    "error": playhead.error,
                },
            }
        )
        if index + 1 < samples:
            time.sleep(interval)
    return rows


def analyze_samples(samples: list[dict[str, Any]]) -> dict[str, Any]:
    frames = [
        row["playhead"]["current_frame"]
        for row in samples
        if row["playhead"]["ok"] and isinstance(row["playhead"]["current_frame"], int)
    ]
    heartbeat_ages = [
        row["health"]["heartbeat_age_ms"]
        for row in samples
        if row["health"]["ok"] and isinstance(row["health"]["heartbeat_age_ms"], int)
    ]
    advancing_intervals = 0
    stalled_intervals = 0
    for prev, curr in zip(frames, frames[1:]):
        if curr > prev:
            advancing_intervals += 1
        else:
            stalled_intervals += 1

    return {
        "sample_count": len(samples),
        "health_failures": sum(1 for row in samples if not row["health"]["ok"]),
        "playhead_failures": sum(1 for row in samples if not row["playhead"]["ok"]),
        "first_frame": frames[0] if frames else None,
        "last_frame": frames[-1] if frames else None,
        "frame_delta": (frames[-1] - frames[0]) if len(frames) >= 2 else 0,
        "advancing_intervals": advancing_intervals,
        "stalled_intervals": stalled_intervals,
        "max_heartbeat_age_ms": max(heartbeat_ages) if heartbeat_ages else None,
        "playback_active_any": any(row["playhead"]["playback_active"] for row in samples if row["playhead"]["ok"]),
        "playback_active_all": all(row["playhead"]["playback_active"] for row in samples if row["playhead"]["ok"]) if any(row["playhead"]["ok"] for row in samples) else False,
    }


def build_warnings(profile: dict[str, Any] | None, sample_summary: dict[str, Any], ui_result: EndpointResult | None) -> list[str]:
    warnings: list[str] = []
    if sample_summary["health_failures"] > 0:
        warnings.append(f"/health failed {sample_summary['health_failures']} times during sampling")
    if sample_summary["playhead_failures"] > 0:
        warnings.append(f"/playhead failed {sample_summary['playhead_failures']} times during sampling")
    if sample_summary["max_heartbeat_age_ms"] is not None and sample_summary["max_heartbeat_age_ms"] > 1000:
        warnings.append(f"main thread heartbeat exceeded 1000 ms (max {sample_summary['max_heartbeat_age_ms']} ms)")
    if sample_summary["playback_active_any"] and sample_summary["frame_delta"] == 0:
        warnings.append("playback was active but current_frame did not advance")
    if sample_summary["stalled_intervals"] > 0:
        warnings.append(f"playhead stalled or repeated for {sample_summary['stalled_intervals']} sampled intervals")
    if ui_result and not ui_result.ok:
        warnings.append(f"/ui unavailable: {ui_result.error}")

    preview = (profile or {}).get("preview", {})
    if preview:
        last_paint_age_ms = preview.get("last_paint_age_ms")
        if isinstance(last_paint_age_ms, int) and last_paint_age_ms > 1000:
            warnings.append(f"preview last_paint_age_ms is stale at {last_paint_age_ms} ms")
        pending_visible = ((preview.get("cache") or {}).get("pending_visible_requests"))
        if isinstance(pending_visible, int) and pending_visible > 4:
            warnings.append(f"preview has high pending_visible_requests={pending_visible}")
        pending_decode = ((preview.get("decoder") or {}).get("pending_requests"))
        if isinstance(pending_decode, int) and pending_decode > 8:
            warnings.append(f"decoder pending_requests is high at {pending_decode}")
        backend = preview.get("backend")
        if isinstance(backend, str) and "fallback" in backend.lower():
            warnings.append(f"preview backend is fallback mode: {backend}")

    return warnings


def main() -> int:
    parser = argparse.ArgumentParser(description="AI-oriented REST snapshot for the running onscreen editor")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Control port of the onscreen editor")
    parser.add_argument("--timeout", type=float, default=1.0, help="Per-request timeout in seconds")
    parser.add_argument("--samples", type=int, default=8, help="Number of /health and /playhead samples to collect")
    parser.add_argument("--interval", type=float, default=0.25, help="Delay between samples in seconds")
    parser.add_argument("--include-ui", action="store_true", help="Include summarized transport widgets from /ui")
    args = parser.parse_args()

    base_url = f"http://127.0.0.1:{args.port}"
    health = fetch_json(base_url, "/health", args.timeout)
    playhead = fetch_json(base_url, "/playhead", args.timeout)
    profile = fetch_json(base_url, "/profile", args.timeout)
    debug = fetch_json(base_url, "/debug", args.timeout)
    ui = fetch_json(base_url, "/ui", args.timeout) if args.include_ui else None
    samples = collect_samples(base_url, args.timeout, args.samples, args.interval)
    sample_summary = analyze_samples(samples)

    profile_payload = profile.payload.get("profile") if profile.ok and profile.payload else None
    debug_payload = debug.payload.get("debug") if debug.ok and debug.payload else None

    ui_summary: dict[str, Any] | None = None
    if ui and ui.ok and ui.payload:
        window = ui.payload.get("window") or {}
        ui_summary = {
            "transport.play": summarize_widget(find_widget(window, "transport.play")),
            "transport.pause": summarize_widget(find_widget(window, "transport.pause")),
            "transport.seek": summarize_widget(find_widget(window, "transport.seek")),
        }

    result = {
        "kind": "editor_ai_rest_snapshot",
        "target": {
            "base_url": base_url,
            "port": args.port,
            "mode": "onscreen",
        },
        "high_level": {
            "pid": health.payload.get("pid") if health.ok and health.payload else None,
            "playback_active_now": playhead.payload.get("playback_active") if playhead.ok and playhead.payload else None,
            "current_frame_now": playhead.payload.get("current_frame") if playhead.ok and playhead.payload else None,
            "main_thread_heartbeat_age_ms_now": health.payload.get("main_thread_heartbeat_age_ms") if health.ok and health.payload else None,
            "last_playhead_advance_age_ms_now": health.payload.get("last_playhead_advance_age_ms") if health.ok and health.payload else None,
            "warnings": build_warnings(profile_payload, sample_summary, ui),
        },
        "endpoint_status": {
            "health": {"ok": health.ok, "status": health.status, "latency_ms": round(health.latency_ms, 1), "error": health.error},
            "playhead": {"ok": playhead.ok, "status": playhead.status, "latency_ms": round(playhead.latency_ms, 1), "error": playhead.error},
            "profile": {"ok": profile.ok, "status": profile.status, "latency_ms": round(profile.latency_ms, 1), "error": profile.error},
            "debug": {"ok": debug.ok, "status": debug.status, "latency_ms": round(debug.latency_ms, 1), "error": debug.error},
            "ui": None if ui is None else {"ok": ui.ok, "status": ui.status, "latency_ms": round(ui.latency_ms, 1), "error": ui.error},
        },
        "sample_summary": sample_summary,
        "profile_focus": {
            "editor": None if not profile_payload else {
                "playback_active": profile_payload.get("playback_active"),
                "timeline_clip_count": profile_payload.get("timeline_clip_count"),
                "current_frame": profile_payload.get("current_frame"),
                "main_thread_heartbeat_age_ms": profile_payload.get("main_thread_heartbeat_age_ms"),
                "last_playhead_advance_age_ms": profile_payload.get("last_playhead_advance_age_ms"),
            },
            "preview": None if not profile_payload else profile_payload.get("preview"),
        },
        "debug_flags": debug_payload,
        "ui_focus": ui_summary,
        "recent_samples": samples,
    }

    json.dump(result, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
