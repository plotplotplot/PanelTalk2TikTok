#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
ONSCREEN_CONTROL_PORT = 40130
OFFSCREEN_CONTROL_PORT = 40131


def resolve_editor_path(args: argparse.Namespace) -> Path:
    if args.build_dir:
        return Path(args.build_dir).resolve() / "editor"
    if args.asan:
        return (REPO_ROOT / "build-asan" / "editor").resolve()
    env_build_dir = os.environ.get("EDITOR_BUILD_DIR")
    if env_build_dir:
        return Path(env_build_dir).resolve() / "editor"
    return (REPO_ROOT / "build" / "editor").resolve()


def control_port_for_mode(offscreen: bool) -> int:
    return OFFSCREEN_CONTROL_PORT if offscreen else ONSCREEN_CONTROL_PORT


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Foreground launcher for the editor. Onscreen uses editor port 40130; offscreen uses 40131."
    )
    parser.add_argument("--offscreen", action="store_true", help="Run with QT_QPA_PLATFORM=offscreen")
    parser.add_argument("--asan", action="store_true", help="Run the ASan build from build-asan/editor")
    parser.add_argument("--build-dir", help="Run the editor binary from a specific build directory")
    parser.add_argument("--valgrind", action="store_true", help="Run the editor under valgrind memcheck")
    parser.add_argument("--software-rendering", action="store_true",
                        help="Force software rendering to avoid GPU driver paths")
    return parser


def spawn_child(editor_path: Path, env: dict[str, str], use_valgrind: bool) -> subprocess.Popen[bytes]:
    cmd = [str(editor_path)]
    if use_valgrind:
        cmd = [
            "valgrind",
            "--tool=memcheck",
            "--leak-check=full",
            "--track-origins=yes",
            "--error-exitcode=101",
            "--num-callers=32",
        ] + cmd
    return subprocess.Popen(cmd, cwd=str(REPO_ROOT), env=env)


def terminate_child(process: subprocess.Popen[bytes]) -> int:
    if process.poll() is not None:
        return process.returncode
    try:
        process.send_signal(signal.SIGTERM)
        return process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.wait(timeout=5.0)


def main() -> int:
    args = build_parser().parse_args()
    editor_path = resolve_editor_path(args)
    if not editor_path.exists():
        print(f'{{"ok": false, "error": "editor binary not found: {editor_path}"}}', file=sys.stderr)
        return 1

    env = os.environ.copy()
    if args.offscreen:
        env["QT_QPA_PLATFORM"] = "offscreen"
    if args.software_rendering or args.valgrind:
        env["QT_QUICK_BACKEND"] = "software"
        env["LIBGL_ALWAYS_SOFTWARE"] = "1"
        env["QT_OPENGL"] = "software"
    if args.valgrind:
        env["EDITOR_FORCE_NULL_RHI"] = "1"
    env["EDITOR_CONTROL_PORT"] = str(control_port_for_mode(args.offscreen))

    process = spawn_child(editor_path, env, args.valgrind)
    try:
        return process.wait()
    except KeyboardInterrupt:
        return terminate_child(process)


if __name__ == "__main__":
    raise SystemExit(main())
