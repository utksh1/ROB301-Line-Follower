#!/usr/bin/env python3
"""
BLE UART Telemetry Logger for Line Follower Robot
Reads from /tmp/c3-ble and saves timestamped CSV data + summary analysis.
Usage: python logger.py [optional_session_name]
"""

import sys
import os
import serial
import csv
import datetime
import signal
import statistics
from collections import defaultdict

# ─── Config ──────────────────────────────────────────────────────────────────
PORT      = "/tmp/c3-ble"
BAUD      = 115200
LOG_DIR   = os.path.join(os.path.dirname(__file__), "logs")

# ─── Globals ─────────────────────────────────────────────────────────────────
running = True
rows    = []

def signal_handler(sig, frame):
    global running
    print("\n\n[Logger] Stopping... writing summary.")
    running = False

signal.signal(signal.SIGINT, signal_handler)

# ─── Setup ───────────────────────────────────────────────────────────────────
os.makedirs(LOG_DIR, exist_ok=True)
session = sys.argv[1] if len(sys.argv) > 1 else datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
csv_path = os.path.join(LOG_DIR, f"run_{session}.csv")
txt_path = os.path.join(LOG_DIR, f"run_{session}_summary.txt")

print(f"[Logger] Logging to: {csv_path}")
print(f"[Logger] Press Ctrl+C to stop and generate summary.\n")

# ─── CSV Header ──────────────────────────────────────────────────────────────
fieldnames = ["timestamp_ms", "valid", "state", "P", "E", "C", "L", "R", "speed_cmd"]

start_time = datetime.datetime.now()

try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
except Exception as e:
    print(f"[Logger] ERROR: Cannot open {PORT}: {e}")
    print(f"[Logger] Make sure connect.py is running first.")
    sys.exit(1)

with open(csv_path, "w", newline="") as csvfile:
    writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
    writer.writeheader()

    print(f"{'Time(ms)':>10} | {'Valid':>7} | {'State':>8} | {'P':>5} | {'E':>6} | {'C':>5} | {'L':>4} | {'R':>4}")
    print("-" * 70)

    while running:
        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
        except Exception:
            continue

        if not line:
            continue

        now_ms = int((datetime.datetime.now() - start_time).total_seconds() * 1000)
        row = {"timestamp_ms": now_ms, "speed_cmd": ""}

        # ── Parse VALID line ──────────────────────────────────────────────────
        if line.startswith("VALID"):
            try:
                # VALID,STATE=FOLLOW,P=3.50,E=0.00,C=0,L=85,R=85
                parts = dict(p.split("=") for p in line.split(",") if "=" in p)
                row["valid"] = "VALID"
                row["state"] = parts.get("STATE", "?")
                row["P"]     = float(parts.get("P", 0))
                row["E"]     = float(parts.get("E", 0))
                row["C"]     = int(parts.get("C", 0))
                row["L"]     = int(parts.get("L", 0))
                row["R"]     = int(parts.get("R", 0))
                print(f"{now_ms:>10} | {'VALID':>7} | {row['state']:>8} | {row['P']:>5.2f} | {row['E']:>6.2f} | {row['C']:>5} | {row['L']:>4} | {row['R']:>4}")
            except Exception:
                row["valid"] = "VALID"
                row["state"] = "PARSE_ERR"
                row["P"] = row["E"] = row["C"] = row["L"] = row["R"] = None

        # ── Parse INVALID line ────────────────────────────────────────────────
        elif line.startswith("INVALID"):
            # INVALID,STATE=FOLLOW  — robot may still be pivoting to recover
            parts = dict(p.split("=", 1) for p in line.split(",") if "=" in p)
            row["valid"] = "INVALID"
            row["state"] = parts.get("STATE", "?")
            row["P"] = row["E"] = row["C"] = row["L"] = row["R"] = None
            print(f"{now_ms:>10} | {'INVALID':>7} | {row['state']:>8} |   --- |    --- |   --- |  --- |  ---")

        # ── Parse speed / PID change ──────────────────────────────────────────
        elif line.startswith(">> SPEED") or line.startswith(">> PID"):
            row["valid"] = "CMD"
            row["state"] = "SPEED_CHANGE" if "SPEED" in line else "PID_CHANGE"
            row["speed_cmd"] = line
            row["P"] = row["E"] = row["C"] = row["L"] = row["R"] = None
            print(f"{now_ms:>10} | {'CMD':>7} | {line}")

        else:
            # Garbage or partial line — skip
            continue

        rows.append(row)
        writer.writerow(row)
        csvfile.flush()

ser.close()

# ─── Summary Analysis ─────────────────────────────────────────────────────────
valid_rows   = [r for r in rows if r["valid"] == "VALID" and r["E"] is not None]
invalid_rows = [r for r in rows if r["valid"] == "INVALID"]
follow_rows  = [r for r in valid_rows if r["state"] == "FOLLOW"]
stopped_rows = [r for r in valid_rows if r["state"] == "STOPPED"]

total_time_s = rows[-1]["timestamp_ms"] / 1000 if rows else 0

# Find drop-off events (transitions to INVALID)
dropoffs = []
for i in range(1, len(rows)):
    if rows[i]["valid"] == "INVALID" and rows[i-1]["valid"] == "VALID" and rows[i-1]["E"] is not None:
        last_valid = rows[i-1]
        dropoffs.append({
            "time_ms": last_valid["timestamp_ms"],
            "P": last_valid["P"],
            "E": last_valid["E"],
            "C": last_valid["C"],
            "L": last_valid["L"],
            "R": last_valid["R"],
        })

errors = [r["E"] for r in follow_rows]
corrections = [r["C"] for r in follow_rows]

summary_lines = []
summary_lines.append("=" * 60)
summary_lines.append(f"  TELEMETRY SUMMARY  —  Session: {session}")
summary_lines.append("=" * 60)
summary_lines.append(f"  Total runtime       : {total_time_s:.1f} s")
summary_lines.append(f"  Total rows logged   : {len(rows)}")
summary_lines.append(f"  VALID FOLLOW rows   : {len(follow_rows)}")
summary_lines.append(f"  VALID STOPPED rows  : {len(stopped_rows)}")
summary_lines.append(f"  INVALID rows        : {len(invalid_rows)}")
summary_lines.append(f"  Line-loss events    : {len(dropoffs)}")
summary_lines.append("")

if errors:
    summary_lines.append(f"  Error (E) stats:")
    summary_lines.append(f"    Mean  : {statistics.mean(errors):.3f}")
    if len(errors) > 1:
        summary_lines.append(f"    StdDev: {statistics.stdev(errors):.3f}  (lower = smoother)")
        # Wobble metric: how often the error flips side of the line.
        # Zeros (centred) are skipped so -0.5 → 0 → +0.5 still counts as a flip.
        signs = [e > 0 for e in errors if e != 0]
        flips = sum(1 for a, b in zip(signs, signs[1:]) if a != b)
        summary_lines.append(f"    Side flips: {flips} ({flips / (len(errors) / 10):.1f}/s)  (lower = less snaking)")
    summary_lines.append(f"    Min   : {min(errors):.2f}")
    summary_lines.append(f"    Max   : {max(errors):.2f}")
    summary_lines.append(f"    |E|>2 count (sharp turns): {sum(1 for e in errors if abs(e) > 2)}")
    summary_lines.append(f"    |E|>3 count (near edge) : {sum(1 for e in errors if abs(e) > 3)}")

if corrections:
    summary_lines.append(f"  Correction (C) stats:")
    summary_lines.append(f"    Mean  : {statistics.mean(corrections):.1f}")
    summary_lines.append(f"    Max   : {max(corrections)}")
    summary_lines.append(f"    Min   : {min(corrections)}")

summary_lines.append("")
summary_lines.append(f"  LINE-LOSS EVENTS (last VALID reading before each INVALID):")
summary_lines.append(f"  {'#':>3} | {'Time(ms)':>10} | {'P':>5} | {'E':>6} | {'C':>5} | {'L':>4} | {'R':>4}")
summary_lines.append(f"  " + "-" * 55)
for i, d in enumerate(dropoffs, 1):
    summary_lines.append(
        f"  {i:>3} | {d['time_ms']:>10} | {d['P']:>5.2f} | {d['E']:>6.2f} | {d['C']:>5} | {d['L']:>4} | {d['R']:>4}"
    )

if dropoffs:
    drop_errors = [d["E"] for d in dropoffs]
    summary_lines.append("")
    summary_lines.append(f"  Most common E at line-loss: {statistics.mode(drop_errors):.2f}")
    left_drops  = sum(1 for d in dropoffs if d["E"] < 0)
    right_drops = sum(1 for d in dropoffs if d["E"] > 0)
    summary_lines.append(f"  Line lost on LEFT  (E<0): {left_drops} times")
    summary_lines.append(f"  Line lost on RIGHT (E>0): {right_drops} times")

summary_lines.append("")
summary_lines.append(f"  CSV saved to: {csv_path}")
summary_lines.append("=" * 60)

summary_text = "\n".join(summary_lines)
print("\n" + summary_text)

with open(txt_path, "w") as f:
    f.write(summary_text + "\n")

print(f"\n[Logger] Summary saved to: {txt_path}")

