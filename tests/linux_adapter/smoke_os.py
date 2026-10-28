#!/usr/bin/env python3
"""Boot the Linux runtime with no adapter, all capabilities, and accelerometer-only support."""

import argparse
import os
import subprocess
import tempfile
import time
from pathlib import Path


def run_case(args, name, caps, directory):
    directory.mkdir()
    data = directory / "data"
    data.mkdir()
    profile = directory / "device.profile"
    profile.write_text(
        "input.keys =\ninput.touch =\ninput.bezel =\nforeground_reclaim_ms =\n"
    )
    environment = {
        key: value for key, value in os.environ.items() if not key.startswith("PBL_")
    }
    environment.update(
        PBL_RUNTIME_DIR=str(directory),
        PBL_DATA_DIR=str(data),
        PBL_FB_DEVICE="/dev/null",
    )
    output = args.output / name
    output.mkdir(parents=True, exist_ok=True)
    mock = None
    with (
        (output / "mock.log").open("w+") as mock_log,
        (output / "os.log").open("w+") as os_log,
    ):
        try:
            if caps is not None:
                mock = subprocess.Popen(
                    [str(args.mock), "--dir", str(directory), "--caps", caps],
                    stdout=mock_log,
                    stderr=subprocess.STDOUT,
                    env=environment,
                )
                deadline = time.monotonic() + 5
                while not (directory / "adapter.sock").exists():
                    if mock.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(f"{name}: mock did not bind its socket")
                    time.sleep(0.02)
            frame = output / "frame.ppm"
            subprocess.run(
                [
                    str(args.os_binary),
                    "-t",
                    str(args.seconds),
                    "-f",
                    str(directory / "flash.bin"),
                    "-r",
                    str(args.resources),
                    "-P",
                    str(profile),
                    "-S",
                    str(frame),
                ],
                cwd=directory,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=os_log,
                stderr=subprocess.STDOUT,
                timeout=args.seconds + 20,
                check=True,
            )
            os_log.flush()
            os_text = (output / "os.log").read_text(errors="replace")
            if "Starting Launcher" not in os_text:
                raise RuntimeError(f"{name}: firmware did not reach the launcher")
            if not frame.exists() or not frame.read_bytes().startswith(
                b"P6\n260 260\n255\n"
            ):
                raise RuntimeError(f"{name}: firmware did not save its display frame")
            if frame.stat().st_size < 260 * 260 * 3:
                raise RuntimeError(f"{name}: display frame is incomplete")
            if mock is not None:
                mock_log.flush()
                commands = (output / "mock.log").read_text()
                if "type=0x0001" not in commands or "type=0x0006" not in commands:
                    raise RuntimeError(
                        f"{name}: handshake or accelerometer configuration missing"
                    )
                if caps == "0x008" and any(
                    f"type=0x{kind:04x}" in commands for kind in (2, 3, 4, 7, 8)
                ):
                    raise RuntimeError(
                        f"{name}: OS sent a command for an unavailable capability"
                    )
                if caps == "0x3ff" and "type=0x0007" not in commands:
                    raise RuntimeError(f"{name}: HRM configuration missing")
            print(
                f"PASS {name}: launcher, display frame and capability checks",
                flush=True,
            )
        finally:
            if mock is not None:
                mock.terminate()
                try:
                    mock.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    mock.kill()
                    mock.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--os-binary", required=True, type=Path)
    parser.add_argument("--resources", required=True, type=Path)
    parser.add_argument("--mock", required=True, type=Path)
    parser.add_argument("--output", default=Path("smoke-results"), type=Path)
    parser.add_argument("--seconds", type=int, default=20)
    args = parser.parse_args()
    if args.seconds < 1:
        parser.error("seconds must be positive")
    for name in ("os_binary", "resources", "mock", "output"):
        setattr(args, name, getattr(args, name).resolve())
    with tempfile.TemporaryDirectory(prefix="pbl-os-smoke-", dir="/tmp") as temporary:
        for name, caps in (
            ("no-adapter", None),
            ("all-caps", "0x3ff"),
            ("partial-caps", "0x008"),
        ):
            run_case(args, name, caps, Path(temporary) / name)


if __name__ == "__main__":
    main()
