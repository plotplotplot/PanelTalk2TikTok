#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
ONSCREEN_BASE_URL = "http://127.0.0.1:40130"
OFFSCREEN_BASE_URL = "http://127.0.0.1:40131"
BASE_URL = ONSCREEN_BASE_URL


class TestFailure(RuntimeError):
    pass


@dataclass
class RequestSample:
    timestamp: float
    status: str
    detail: str
    payload: dict[str, Any] | None = None


@dataclass
class Diagnostics:
    phase: str = "init"
    last_good_playhead: dict[str, Any] | None = None
    playhead_samples: list[RequestSample] = field(default_factory=list)
    last_profile: dict[str, Any] | None = None
    last_health: dict[str, Any] | None = None
    last_pid: int | None = None

    def add_playhead_sample(self, status: str, detail: str, payload: dict[str, Any] | None = None) -> None:
        self.playhead_samples.append(
            RequestSample(timestamp=time.time(), status=status, detail=detail, payload=payload)
        )
        if len(self.playhead_samples) > 40:
            self.playhead_samples = self.playhead_samples[-40:]


def request(path: str, method: str = "GET", payload: dict[str, Any] | None = None, timeout: float = 3.0) -> Any:
    data = None
    headers: dict[str, str] = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"{BASE_URL}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            body = response.read()
            content_type = response.headers.get("Content-Type", "")
    except urllib.error.HTTPError as exc:
        raise TestFailure(f"{method} {path} returned HTTP {exc.code}") from exc
    except TimeoutError as exc:
        raise TestFailure(f"{method} {path} timed out after {timeout:.1f}s") from exc
    except urllib.error.URLError as exc:
        raise TestFailure(f"{method} {path} failed: {exc.reason}") from exc
    if not body:
        return {}
    if "application/json" in content_type:
        return json.loads(body.decode("utf-8"))
    return body


def find_widget(node: dict[str, Any], widget_id: str) -> dict[str, Any] | None:
    if node.get("id") == widget_id:
        return node
    for child in node.get("children", []):
        found = find_widget(child, widget_id)
        if found is not None:
            return found
    return None


def kill_existing_editors() -> None:
    probe = subprocess.run(
        ["pgrep", "-f", f"{REPO_ROOT}/(build|build-asan)/editor"],
        text=True,
        capture_output=True,
        cwd=str(REPO_ROOT),
    )
    if probe.returncode not in (0, 1):
        raise TestFailure(f"failed to probe existing editor processes: {probe.stderr.strip()}")
    if probe.returncode == 1:
        return

    for line in probe.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        pid = int(line)
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            continue

    deadline = time.time() + 5.0
    while time.time() < deadline:
        probe = subprocess.run(
            ["pgrep", "-f", f"{REPO_ROOT}/(build|build-asan)/editor"],
            text=True,
            capture_output=True,
            cwd=str(REPO_ROOT),
        )
        if probe.returncode == 1:
            return
        time.sleep(0.1)

    probe = subprocess.run(
        ["pgrep", "-f", f"{REPO_ROOT}/(build|build-asan)/editor"],
        text=True,
        capture_output=True,
        cwd=str(REPO_ROOT),
    )
    for line in probe.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        pid = int(line)
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            continue


def wait_until(predicate, timeout: float, interval: float = 0.1, description: str = "condition") -> Any:
    deadline = time.time() + timeout
    last_value = None
    while time.time() < deadline:
        last_value = predicate()
        if last_value:
            return last_value
        time.sleep(interval)
    raise TestFailure(f"timed out waiting for {description}")


def try_request(path: str, method: str = "GET", payload: dict[str, Any] | None = None, timeout: float = 1.0) -> tuple[bool, Any]:
    try:
        return True, request(path, method=method, payload=payload, timeout=timeout)
    except TestFailure as exc:
        return False, str(exc)


def resolve_editor_path(asan: bool) -> Path:
    return REPO_ROOT / ("build-asan" if asan else "build") / "editor"


def launch_editor(offscreen: bool, asan: bool, valgrind: bool, software_rendering: bool) -> subprocess.Popen[str]:
    cmd = [str(resolve_editor_path(asan))]
    if valgrind:
        cmd = [
            "valgrind",
            "--tool=memcheck",
            "--leak-check=full",
            "--track-origins=yes",
            "--error-exitcode=101",
            "--num-callers=32",
        ] + cmd
    env = os.environ.copy()
    if offscreen:
        env["QT_QPA_PLATFORM"] = "offscreen"
    if software_rendering or valgrind:
        env["QT_QUICK_BACKEND"] = "software"
        env["LIBGL_ALWAYS_SOFTWARE"] = "1"
        env["QT_OPENGL"] = "software"
    if valgrind:
        env["EDITOR_FORCE_NULL_RHI"] = "1"
    env["EDITOR_CONTROL_PORT"] = "40131" if offscreen else "40130"
    return subprocess.Popen(
        cmd,
        cwd=str(REPO_ROOT),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


class HarnessMonitor:
    def __init__(self, process: subprocess.Popen[str]) -> None:
        self.process = process
        self.lines: list[str] = []
        self._thread = threading.Thread(target=self._pump_output, daemon=True)
        self._thread.start()

    def _pump_output(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            stripped = line.rstrip()
            self.lines.append(stripped)

    def wait_for_pid(self, timeout: float) -> int:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.process.poll() is not None:
                break
            ok, result = try_request("/health", timeout=1.0)
            if ok and result.get("ok") and result.get("pid"):
                return int(result["pid"])
            time.sleep(0.1)
        raise TestFailure("timed out waiting for editor health")


def stop_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5.0)


def wait_for_playhead_progress(
    diagnostics: Diagnostics,
    monitor_seconds: float,
    stall_timeout: float,
    max_consecutive_failures: int,
    poll_interval: float,
) -> int:
    deadline = time.time() + monitor_seconds
    max_frame = -1
    last_frame: int | None = None
    last_advance_at = time.time()
    consecutive_failures = 0

    while time.time() < deadline:
        ok, result = try_request("/playhead", timeout=1.0)
        if ok:
            playhead = result
            diagnostics.last_good_playhead = playhead
            diagnostics.add_playhead_sample("ok", "playhead", playhead)
            consecutive_failures = 0
            frame = int(playhead["current_frame"])
            if frame > max_frame:
                max_frame = frame
            if last_frame is None or frame != last_frame:
                last_advance_at = time.time()
            last_frame = frame
            if not playhead.get("playback_active"):
                raise TestFailure("playback became inactive during playback monitor window")
        else:
            diagnostics.add_playhead_sample("error", str(result))
            consecutive_failures += 1
            if consecutive_failures >= max_consecutive_failures:
                raise TestFailure(
                    f"/playhead failed {consecutive_failures} times in a row during playback; last error: {result}"
                )

        if time.time() - last_advance_at > stall_timeout:
            raise TestFailure(
                f"playhead stalled for {stall_timeout:.1f}s during playback monitor window"
            )

        time.sleep(poll_interval)

    return max_frame


def write_failure_artifacts(
    artifact_dir: Path,
    diagnostics: Diagnostics,
    monitor: HarnessMonitor,
    error: str,
) -> dict[str, Any]:
    artifact_dir.mkdir(parents=True, exist_ok=True)
    timestamp = time.strftime("%Y%m%d-%H%M%S")

    recovery: dict[str, Any] = {}
    for name, path, timeout in (
        ("playhead", "/playhead", 2.0),
        ("health", "/health", 2.0),
        ("profile", "/profile", 2.0),
    ):
        ok, result = try_request(path, timeout=timeout)
        recovery[name] = {"ok": ok, "result": result}

    screenshot_path = artifact_dir / f"{timestamp}-stall.png"
    ok, screenshot_result = try_request("/screenshot", timeout=3.0)
    if ok and isinstance(screenshot_result, (bytes, bytearray)) and screenshot_result:
        screenshot_path.write_bytes(screenshot_result)
        recovery["screenshot"] = {"ok": True, "path": str(screenshot_path)}
    else:
        recovery["screenshot"] = {"ok": False, "result": screenshot_result}

    harness_log_path = artifact_dir / f"{timestamp}-harness.log"
    harness_log_path.write_text("\n".join(monitor.lines[-400:]) + "\n", encoding="utf-8")

    gdb_backtrace_path = artifact_dir / f"{timestamp}-gdb.txt"
    gdb_result: dict[str, Any]
    if diagnostics.last_pid:
        try:
            gdb = subprocess.run(
                [
                    "gdb",
                    "-p",
                    str(diagnostics.last_pid),
                    "-batch",
                    "-ex",
                    "set pagination off",
                    "-ex",
                    "info threads",
                    "-ex",
                    "thread apply all bt",
                ],
                cwd=str(REPO_ROOT),
                text=True,
                capture_output=True,
                timeout=15.0,
            )
            gdb_output = (gdb.stdout or "") + ("\n" + gdb.stderr if gdb.stderr else "")
            gdb_backtrace_path.write_text(gdb_output, encoding="utf-8")
            gdb_result = {
                "ok": gdb.returncode == 0,
                "path": str(gdb_backtrace_path),
                "returncode": gdb.returncode,
            }
        except Exception as exc:
            gdb_result = {"ok": False, "error": str(exc)}
    else:
        gdb_result = {"ok": False, "error": "no pid available"}

    failure_payload = {
        "ok": False,
        "error": error,
        "phase": diagnostics.phase,
        "pid": diagnostics.last_pid,
        "last_good_playhead": diagnostics.last_good_playhead,
        "last_profile": diagnostics.last_profile,
        "last_health": diagnostics.last_health,
        "recent_playhead_samples": [
            {
                "t": round(sample.timestamp, 3),
                "status": sample.status,
                "detail": sample.detail,
                "frame": None if not sample.payload else sample.payload.get("current_frame"),
                "active": None if not sample.payload else sample.payload.get("playback_active"),
            }
            for sample in diagnostics.playhead_samples[-20:]
        ],
        "recovery_probe": recovery,
        "process_log": str(harness_log_path),
        "gdb_backtrace": gdb_result,
    }

    result_path = artifact_dir / f"{timestamp}-result.json"
    result_path.write_text(json.dumps(failure_payload, indent=2) + "\n", encoding="utf-8")
    failure_payload["artifact_result"] = str(result_path)
    return failure_payload


def main() -> int:
    parser = argparse.ArgumentParser(description="REST smoke test for the editor")
    parser.add_argument("--offscreen", action="store_true", help="Launch the app headlessly")
    parser.add_argument("--asan", action="store_true", help="Pass --asan through to ./build.sh")
    parser.add_argument("--valgrind", action="store_true", help="Run the launched editor under valgrind memcheck")
    parser.add_argument("--software-rendering", action="store_true", help="Force software rendering in the harness")
    parser.add_argument("--build-timeout", type=float, default=120.0)
    parser.add_argument("--startup-timeout", type=float, default=15.0)
    parser.add_argument("--restart-timeout", type=float, default=15.0)
    parser.add_argument("--play-timeout", type=float, default=8.0)
    parser.add_argument("--advance-timeout", type=float, default=8.0)
    parser.add_argument("--monitor-seconds", type=float, default=4.0)
    parser.add_argument("--stall-timeout", type=float, default=2.5)
    parser.add_argument("--poll-interval", type=float, default=0.25)
    parser.add_argument("--max-playhead-failures", type=int, default=4)
    parser.add_argument("--pause-timeout", type=float, default=5.0)
    parser.add_argument("--skip-restart", action="store_true")
    parser.add_argument("--artifact-dir", default=str(REPO_ROOT / "tests" / "artifacts"))
    parser.add_argument("--screenshot-out", default=str(REPO_ROOT / "tests" / "rest_smoke_screenshot.png"))
    args = parser.parse_args()
    global BASE_URL
    BASE_URL = OFFSCREEN_BASE_URL if args.offscreen else ONSCREEN_BASE_URL
    if args.valgrind and args.asan:
        print(json.dumps({"ok": False, "error": "--asan and --valgrind should not be combined"}), file=sys.stderr)
        return 1
    if args.valgrind:
        args.startup_timeout = max(args.startup_timeout, 45.0)
        args.restart_timeout = max(args.restart_timeout, 45.0)
        args.play_timeout = max(args.play_timeout, 20.0)
        args.advance_timeout = max(args.advance_timeout, 20.0)
        args.software_rendering = True

    build_cmd = ["./build.sh"]
    if args.asan:
        build_cmd.append("--asan")

    build = subprocess.run(
        build_cmd,
        cwd=str(REPO_ROOT),
        text=True,
        capture_output=True,
        timeout=args.build_timeout,
    )
    if build.returncode != 0:
        print(json.dumps({"ok": False, "error": "build failed", "stdout": build.stdout, "stderr": build.stderr}), file=sys.stderr)
        return 1

    kill_existing_editors()

    process = launch_editor(
        offscreen=args.offscreen,
        asan=args.asan,
        valgrind=args.valgrind,
        software_rendering=args.software_rendering,
    )
    monitor = HarnessMonitor(process)
    diagnostics = Diagnostics()
    try:
        diagnostics.phase = "startup"
        initial_pid = monitor.wait_for_pid(timeout=args.startup_timeout)
        diagnostics.last_health = wait_until(
            lambda: request("/health", timeout=1.0),
            timeout=args.startup_timeout,
            description="/health",
        )
        diagnostics.last_pid = initial_pid
        restarted_pid = initial_pid
        if not args.skip_restart:
            diagnostics.phase = "restart"
            os.kill(initial_pid, signal.SIGKILL)
            process.wait(timeout=5.0)
            process = launch_editor(
                offscreen=args.offscreen,
                asan=args.asan,
                valgrind=args.valgrind,
                software_rendering=args.software_rendering,
            )
            monitor = HarnessMonitor(process)
            restarted_pid = monitor.wait_for_pid(timeout=args.restart_timeout)
            if restarted_pid == initial_pid:
                raise TestFailure("restart reused the same pid unexpectedly")
            diagnostics.last_health = wait_until(
                lambda: request("/health", timeout=1.0),
                timeout=args.restart_timeout,
                description="/health after restart",
            )
            diagnostics.last_pid = int(diagnostics.last_health.get("pid", 0))

        diagnostics.phase = "ui"
        ui = request("/ui")
        if not ui.get("ok"):
            raise TestFailure("/ui did not return ok=true")
        play_button = find_widget(ui["window"], "transport.play")
        if play_button is None:
            raise TestFailure("transport.play not found in UI tree")
        if not play_button.get("clickable") or not play_button.get("enabled") or not play_button.get("visible"):
            raise TestFailure("transport.play is not clickable/enabled/visible")

        diagnostics.phase = "profile_before_play"
        profile_before = request("/profile")
        diagnostics.last_profile = profile_before
        if not profile_before.get("ok"):
            raise TestFailure("/profile did not return ok=true")

        diagnostics.phase = "click_play"
        click_result = request("/click-item", method="POST", payload={"id": "transport.play"})
        if not click_result.get("ok"):
            raise TestFailure(f"click-item transport.play failed: {click_result}")

        def playback_started() -> Any:
            ok, result = try_request("/playhead")
            if not ok:
                diagnostics.add_playhead_sample("error", str(result))
                return None
            playhead = result
            diagnostics.last_good_playhead = playhead
            diagnostics.add_playhead_sample("ok", "playhead", playhead)
            if playhead.get("ok") and playhead.get("playback_active"):
                return playhead
            return None

        diagnostics.phase = "playback_started"
        started_profile = wait_until(
            playback_started,
            timeout=args.play_timeout,
            description="playback to start",
        )

        start_frame = int(started_profile["current_frame"])

        def frame_advanced() -> Any:
            ok, result = try_request("/playhead")
            if not ok:
                diagnostics.add_playhead_sample("error", str(result))
                return None
            playhead = result
            diagnostics.last_good_playhead = playhead
            diagnostics.add_playhead_sample("ok", "playhead", playhead)
            if not playhead.get("ok"):
                return None
            current_frame = int(playhead["current_frame"])
            if current_frame > start_frame:
                return playhead
            return None

        diagnostics.phase = "advance"
        advanced_profile = wait_until(
            frame_advanced,
            timeout=args.advance_timeout,
            description="frame advancement after play",
        )

        diagnostics.phase = "monitor"
        max_frame = max(
            int(advanced_profile["current_frame"]),
            wait_for_playhead_progress(
                diagnostics,
                monitor_seconds=args.monitor_seconds,
                stall_timeout=args.stall_timeout,
                max_consecutive_failures=args.max_playhead_failures,
                poll_interval=args.poll_interval,
            ),
        )

        if max_frame <= start_frame:
            raise TestFailure("frame did not continue advancing during playback monitor window")

        diagnostics.phase = "pause_click"
        request("/click-item", method="POST", payload={"id": "transport.pause"})

        def playback_stopped() -> Any:
            profile = request("/profile")
            diagnostics.last_profile = profile
            if profile.get("ok") and not profile["profile"].get("playback_active"):
                return profile
            return None

        diagnostics.phase = "pause_wait"
        paused_profile = wait_until(
            playback_stopped,
            timeout=args.pause_timeout,
            description="playback to pause",
        )

        diagnostics.phase = "health_after_pause"
        health_after_pause = request("/health", timeout=3.0)
        diagnostics.last_health = health_after_pause
        if not health_after_pause.get("ok"):
            raise TestFailure("/health did not recover after pause")

        diagnostics.phase = "screenshot"
        screenshot = request("/screenshot", timeout=5.0)
        screenshot_path = Path(args.screenshot_out)
        screenshot_path.parent.mkdir(parents=True, exist_ok=True)
        screenshot_path.write_bytes(screenshot)
        if screenshot_path.stat().st_size == 0:
            raise TestFailure("screenshot was empty")

        print(
            json.dumps(
                {
                    "ok": True,
                    "build_ok": True,
                    "asan": args.asan,
                    "initial_pid": initial_pid,
                    "restarted_pid": restarted_pid,
                    "restart_skipped": args.skip_restart,
                    "start_frame": start_frame,
                    "advanced_frame": max_frame,
                    "paused_frame": int(paused_profile["profile"]["current_frame"]),
                    "screenshot": str(screenshot_path),
                },
                indent=2,
            )
        )
        return 0
    except Exception as exc:
        diagnostics.last_pid = diagnostics.last_pid or (process.pid if process.poll() is None else None)
        failure_payload = write_failure_artifacts(
            Path(args.artifact_dir),
            diagnostics,
            monitor,
            str(exc),
        )
        failure_payload["asan"] = args.asan
        print(json.dumps(failure_payload), file=sys.stderr)
        return 1
    finally:
        stop_process(process)


if __name__ == "__main__":
    raise SystemExit(main())
