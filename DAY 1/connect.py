#!/usr/bin/env python3
"""Connect a C3-XXXX BLE UART by sticker ID; expose /tmp/c3-ble on macOS.

Discovery deliberately matches advertisement.local_name, NEVER BleakDevice.name,
which CoreBluetooth can cache after a firmware name change.
"""

import argparse
import asyncio
import fcntl
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

try:
    from bleak import BleakScanner
except ImportError:
    sys.exit("Missing Bleak. Run: sh setup_mac.sh")

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_WRITE = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_NOTIFY = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
PORT = Path("/tmp/c3-ble")
ALT_PORT = Path("/tmp/cu.c3-ble")
LOCK = Path("/tmp/c3-ble.lock")
ID_RE = re.compile(r"[0-9A-HJKMNP-TV-Z]{4}", re.IGNORECASE)


def normalize_id(raw: str) -> str:
    ident = raw.strip().upper()
    if ident.startswith("C3-"):
        ident = ident[3:]
    if not ID_RE.fullmatch(ident):
        raise ValueError("Enter exactly four characters from the sticker (e.g. F2DB)")
    return ident


async def scan(target: str, seconds: float, show_seen: bool = False):
    """Scan current BLE advertising data, bypassing cached CoreBluetooth names."""
    matches = {}
    others = {}

    def found(device, advertisement):
        fresh_name = (advertisement.local_name or "").strip().upper()
        if not fresh_name:
            return  # Do not fall back to device.name: it may be stale on macOS.
        if fresh_name.startswith("C3-"):
            others[device.address] = fresh_name
        if fresh_name == target:
            matches[device.address] = device

    # The firmware includes the NUS UUID in the advertising packet.
    # A service filter is useful on macOS and reduces unrelated scan traffic.
    async with BleakScanner(detection_callback=found,
                            service_uuids=[NUS_SERVICE]):
        await asyncio.sleep(seconds)

    if show_seen:
        other_names = sorted(set(others.values()))
        print("Advertised C3 adapters: " + (", ".join(other_names) if other_names else "none"))
    if len(matches) > 1:
        raise RuntimeError(f"More than one peripheral advertises {target}. Notify the instructor.")
    return next(iter(matches.values())) if matches else None


def inspect_port(path: Path):
    if path.is_symlink():
        if path.exists():
            raise RuntimeError(f"{path} already points to a live PTY. Stop the other bridge first.")
        path.unlink()  # Broken symlink left behind by a previous run.
    elif path.exists():
        raise RuntimeError(f"{path} already exists and is not a symlink. Refusing to replace it.")


def inspect_port_before_start():
    inspect_port(PORT)
    inspect_port(ALT_PORT)


def terminate(child):
    if child.poll() is not None:
        return
    child.send_signal(signal.SIGINT)
    try:
        child.wait(timeout=6)
    except subprocess.TimeoutExpired:
        child.terminate()
        try:
            child.wait(timeout=4)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def run_bridge(address: str, timeout: float):
    """Return when ble-serial ends; keep the symlink managed by ble-serial."""
    inspect_port_before_start()
    command = [
        sys.executable, "-m", "ble_serial",
        "-d", address, "-s", NUS_SERVICE,
        "-r", NUS_NOTIFY, "-w", NUS_WRITE,
        "-t", str(timeout), "-p", str(PORT),
    ]
    print("Launching BLE bridge; watch for 'Device connected' / 'Running main loop!'.", flush=True)
    child = subprocess.Popen(command)  # Inherit output to display connection errors.
    owned_link = None
    try:
        deadline = time.monotonic() + timeout + 8
        while time.monotonic() < deadline:
            if PORT.is_symlink() and owned_link is None:
                owned_link = os.readlink(PORT)
                try:
                    ALT_PORT.symlink_to(owned_link)
                except OSError:
                    pass
                print(f"Virtual port: {PORT} -> {owned_link}", flush=True)
                print(f"Alias port:   {ALT_PORT} -> {owned_link}", flush=True)
                print(f"Open {PORT} (or {ALT_PORT}) in your terminal/plotter once BLE reports connected.", flush=True)
            if child.poll() is not None:
                break
            if owned_link is not None:
                break
            time.sleep(0.2)
        if owned_link is None and child.poll() is None:
            print("Warning: PTY wasn't created promptly. Check ble-serial output.", flush=True)
        return child.wait()
    finally:
        terminate(child)
        if ALT_PORT.is_symlink():
            try:
                ALT_PORT.unlink()
            except OSError:
                pass
        # Only remove the specific link that this run created.
        if owned_link and PORT.is_symlink() and os.readlink(PORT) == owned_link:
            PORT.unlink()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("id", nargs="?", help="four-character sticker ID (or C3-XXXX)")
    ap.add_argument("--scan-seconds", type=float, default=12.0)
    ap.add_argument("--connect-timeout", type=float, default=15.0)
    ap.add_argument("--retry-seconds", type=float, default=3.0,
                    help="delay before automatic rescan/reconnect")
    ap.add_argument("--once", action="store_true", help="exit instead of reconnecting")
    ap.add_argument("--show-seen", action="store_true", help="show observed advertised C3 names")
    args = ap.parse_args()
    if args.scan_seconds <= 0 or args.connect_timeout <= 0 or args.retry_seconds < 0:
        ap.error("timeouts must be positive; retry delay may be zero")
    ident = normalize_id(args.id or input("Enter sticker ID (4 characters): "))
    target = "C3-" + ident

    # Hold a per-Mac lock for the lifetime of the launcher.
    with LOCK.open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError("Another c3-ble launcher is running on this Mac") from exc

        inspect_port_before_start()
        print(f"Target: {target}; port: {PORT}", flush=True)
        print("CoreBluetooth cached names are intentionally ignored.", flush=True)
        while True:
            print(f"Scanning live advertisements for {target}...", flush=True)
            try:
                device = asyncio.run(scan(target, args.scan_seconds, args.show_seen))
                if device is None:
                    print(f"Not found: {target} (check power, BLE permission, and other connections).", flush=True)
                else:
                    print(f"Found {target}; this Mac's peripheral UUID: {device.address}", flush=True)
                    rc = run_bridge(device.address, args.connect_timeout)
                    print(f"BLE bridge stopped (exit {rc}).", flush=True)
            except KeyboardInterrupt:
                print("\nStopped.")
                break
            except Exception as exc:
                print(f"Connection/scanning error: {exc}", file=sys.stderr, flush=True)
            if args.once:
                break
            print(f"Retrying in {args.retry_seconds:g}s (Ctrl+C to exit)...", flush=True)
            try:
                time.sleep(args.retry_seconds)
            except KeyboardInterrupt:
                print("\nStopped.")
                break


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, OSError) as exc:
        sys.exit(f"ERROR: {exc}")
