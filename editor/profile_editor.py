#!/usr/bin/env python3
"""
Profile the running editor instance via REST API.
Usage: ./profile_editor.py [port]
If port is not provided, auto-detects from running process.
"""

import json
import sys
import subprocess
import re
import time
from typing import Optional, Dict, Any


def find_editor_port() -> Optional[int]:
    """Find the editor's control server port from running processes."""
    try:
        # Try ss first (modern replacement for netstat)
        result = subprocess.run(
            ["ss", "-tlnp"],
            capture_output=True,
            text=True,
            timeout=5
        )
        output = result.stdout
        
        # Look for editor process listening on localhost
        for line in output.split("\n"):
            if "editor" in line and "127.0.0.1" in line:
                match = re.search(r"127\.0\.0\.1:(\d+)", line)
                if match:
                    return int(match.group(1))
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    
    try:
        # Fallback to netstat
        result = subprocess.run(
            ["netstat", "-tlnp"],
            capture_output=True,
            text=True,
            timeout=5
        )
        output = result.stdout
        
        for line in output.split("\n"):
            if "editor" in line.lower() and "127.0.0.1" in line:
                match = re.search(r"127\.0\.0\.1:(\d+)", line)
                if match:
                    return int(match.group(1))
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    
    return None


def fetch_profile(port: int) -> Optional[Dict[str, Any]]:
    """Fetch profile from the editor's REST API."""
    try:
        import urllib.request
        url = f"http://127.0.0.1:{port}/profile"
        with urllib.request.urlopen(url, timeout=5) as response:
            data = json.loads(response.read().decode())
            return data.get("profile", {})
    except Exception as e:
        print(f"Error fetching profile: {e}", file=sys.stderr)
        return None


def fetch_health(port: int) -> Optional[Dict[str, Any]]:
    """Fetch health check from the editor's REST API."""
    try:
        import urllib.request
        url = f"http://127.0.0.1:{port}/health"
        with urllib.request.urlopen(url, timeout=5) as response:
            return json.loads(response.read().decode())
    except Exception as e:
        print(f"Error fetching health: {e}", file=sys.stderr)
        return None


def format_bytes(bytes_val: int) -> str:
    """Format bytes to human readable."""
    for unit in ['B', 'KB', 'MB', 'GB']:
        if bytes_val < 1024:
            return f"{bytes_val:.2f} {unit}"
        bytes_val /= 1024
    return f"{bytes_val:.2f} TB"


def print_profile(profile: Dict[str, Any]) -> None:
    """Pretty print the profile data."""
    if not profile:
        print("No profile data available")
        return
    
    print("=" * 60)
    print("EDITOR PROFILE")
    print("=" * 60)
    
    # Basic info
    print(f"\n[Basic Info]")
    print(f"  Current Frame: {profile.get('current_frame', 'N/A')}")
    print(f"  Timeline Clips: {profile.get('timeline_clip_count', 'N/A')}")
    print(f"  Playback Active: {profile.get('playback_active', 'N/A')}")
    print(f"  Explorer Root: {profile.get('explorer_root', 'N/A')}")
    
    # Thread health
    heartbeat_age = profile.get('main_thread_heartbeat_age_ms', -1)
    print(f"\n[Thread Health]")
    print(f"  Main Thread Heartbeat Age: {heartbeat_age} ms", end="")
    if heartbeat_age > 100:
        print(" ⚠️ STALE")
    else:
        print(" ✓")
    
    # Debug flags
    debug = profile.get('debug', {})
    print(f"\n[Debug Flags]")
    for key, value in debug.items():
        status = "✓ ON" if value else "✗ OFF"
        print(f"  {key}: {status}")
    
    # Preview info
    preview = profile.get('preview', {})
    if preview:
        print(f"\n[Preview]")
        print(f"  Backend: {preview.get('backend', 'N/A')}")
        print(f"  Playing: {preview.get('playing', 'N/A')}")
        print(f"  Current Frame: {preview.get('current_frame', 'N/A')}")
        print(f"  Clip Count: {preview.get('clip_count', 'N/A')}")
        print(f"  Bypass Grading: {preview.get('bypass_grading', 'N/A')}")
        print(f"  Pipeline Initialized: {preview.get('pipeline_initialized', 'N/A')}")
        print(f"  Repaint Timer Active: {preview.get('repaint_timer_active', 'N/A')}")
        
        # Last paint age
        last_paint_age = preview.get('last_paint_age_ms', -1)
        if last_paint_age >= 0:
            print(f"  Last Paint Age: {last_paint_age} ms", end="")
            if last_paint_age > 1000:
                print(" ⚠️ STALE")
            else:
                print(" ✓")
        
        # Cache stats
        cache = preview.get('cache', {})
        if cache:
            print(f"\n  [Cache]")
            print(f"    Total Cached Frames: {cache.get('total_cached_frames', 'N/A')}")
            print(f"    Hit Rate: {cache.get('hit_rate', 'N/A')}")
            print(f"    Pending Visible: {cache.get('pending_visible_requests', 'N/A')}")
            print(f"    Memory Usage: {format_bytes(cache.get('total_memory_usage', 0))}")
        
        # Decoder stats
        decoder = preview.get('decoder', {})
        if decoder:
            print(f"\n  [Decoder]")
            print(f"    Worker Count: {decoder.get('worker_count', 'N/A')}")
            print(f"    Pending Requests: {decoder.get('pending_requests', 'N/A')}")
        
        # Memory budget
        budget = preview.get('memory_budget', {})
        if budget:
            print(f"\n  [Memory Budget]")
            print(f"    CPU: {format_bytes(budget.get('cpu_usage', 0))} / {format_bytes(budget.get('cpu_max', 0))} "
                  f"({budget.get('cpu_pressure', 0)*100:.1f}% pressure)")
            print(f"    GPU: {format_bytes(budget.get('gpu_usage', 0))} / {format_bytes(budget.get('gpu_max', 0))} "
                  f"({budget.get('gpu_pressure', 0)*100:.1f}% pressure)")
    
    print("\n" + "=" * 60)


def continuous_profile(port: int, interval: float = 1.0) -> None:
    """Continuously profile at the given interval."""
    print(f"Profiling editor on port {port} every {interval}s (Ctrl+C to stop)...\n")
    
    prev_frame = None
    prev_time = None
    
    try:
        while True:
            profile = fetch_profile(port)
            if profile:
                current_frame = profile.get('current_frame', 0)
                current_time = time.time()
                
                # Calculate FPS if we have previous data
                if prev_frame is not None and prev_time is not None:
                    frame_delta = current_frame - prev_frame
                    time_delta = current_time - prev_time
                    if time_delta > 0:
                        fps = frame_delta / time_delta
                        print(f"\rFrame: {current_frame:6d} | FPS: {fps:5.1f} | "
                              f"Clips: {profile.get('timeline_clip_count', 0):2d} | "
                              f"Heartbeat: {profile.get('main_thread_heartbeat_age_ms', -1):4d}ms", 
                              end="", flush=True)
                else:
                    print_profile(profile)
                
                prev_frame = current_frame
                prev_time = current_time
            
            time.sleep(interval)
    except KeyboardInterrupt:
        print("\n\nProfiling stopped.")


def main():
    # Parse arguments
    port = None
    continuous_mode = False
    interval = 1.0
    
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ("--continuous", "-c"):
            continuous_mode = True
        elif args[i] == "--interval" and i + 1 < len(args):
            interval = float(args[i + 1])
            i += 1
        elif args[i].isdigit():
            port = int(args[i])
        i += 1
    
    if port is None:
        port = find_editor_port()
        if port is None:
            print("Error: Could not auto-detect editor port.", file=sys.stderr)
            print("Usage: ./profile_editor.py [port] [--continuous] [--interval 0.5]", file=sys.stderr)
            sys.exit(1)
        if not continuous_mode:
            print(f"Auto-detected editor on port {port}")
    
    # Check if continuous mode requested
    if "--continuous" in sys.argv or "-c" in sys.argv:
        interval = 1.0
        if "--interval" in sys.argv:
            idx = sys.argv.index("--interval")
            if idx + 1 < len(sys.argv):
                interval = float(sys.argv[idx + 1])
        continuous_profile(port, interval)
    else:
        profile = fetch_profile(port)
        print_profile(profile)


if __name__ == "__main__":
    main()
