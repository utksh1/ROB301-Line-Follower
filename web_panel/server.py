#!/usr/bin/env python3
"""
STM32 Line Follower Web Control Panel Backend
Serves modern web UI on port 8080 and handles real-time WebSocket telemetry on port 8765.
"""

import asyncio
import functools
import http.server
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial required. Run: pip install pyserial")

try:
    from websockets.asyncio.server import serve
except ImportError:
    try:
        from websockets.server import serve
    except ImportError:
        sys.exit("websockets required. Run: pip install websockets")

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
PROJECT_DIR = BASE_DIR.parent
PORT_BLE = Path("/tmp/c3-ble")
HTTP_PORT = 8080
WS_PORT = 8765

connected_clients = set()
loop = None
serial_lock = threading.Lock()
current_serial = None
bridge_proc = None
current_ble_id = "VT78"
current_pid = {"kp": 2.2, "ki": 0.0, "kd": 16.0}
current_speed = 70
last_state = "STOPPED"

TELEMETRY_RE = re.compile(
    r"^(VALID|INVALID),STATE=(FOLLOW|STOPPED|REVERSE)"
    r"(?:,P=([0-9.]+),E=([+-]?[0-9.]+),C=([+-]?[0-9]+),L=([+-]?[0-9]+),R=([+-]?[0-9]+))?"
)
PID_RE = re.compile(r">> PID Kp=([0-9.]+)\s+Ki=([0-9.]+)\s+Kd=([0-9.]+)")
SPEED_RE = re.compile(r">> SPEED=([0-9]+)%")


def get_local_ip():
    try:
        import socket
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


class StaticHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(STATIC_DIR), **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()

    def log_message(self, format, *args):
        pass  # Quiet HTTP logs


async def ws_broadcast(msg_dict):
    if not connected_clients:
        return
    raw = json.dumps(msg_dict)
    disconnected = set()
    for ws in list(connected_clients):
        try:
            await ws.send(raw)
        except Exception:
            disconnected.add(ws)
    connected_clients.difference_update(disconnected)


def broadcast_sync(msg_dict):
    global loop
    if loop and loop.is_running():
        asyncio.run_coroutine_threadsafe(ws_broadcast(msg_dict), loop)


def write_serial(data: bytes):
    global current_serial
    with serial_lock:
        if current_serial and current_serial.is_open:
            try:
                current_serial.write(data)
                return True
            except Exception as e:
                print(f"[Serial] Write error: {e}")
    # Fallback to direct fd write on /tmp/c3-ble if port is valid
    if PORT_BLE.exists():
        try:
            fd = os.open(str(PORT_BLE), os.O_RDWR | os.O_NONBLOCK)
            os.write(fd, data)
            os.close(fd)
            return True
        except Exception:
            pass
    return False


def serial_reader_thread():
    global current_serial, last_state, current_speed, current_pid
    print("[Serial Thread] Started monitoring /tmp/c3-ble")
    
    while True:
        if not PORT_BLE.exists():
            time.sleep(0.5)
            time.sleep(0.3)
            continue

        try:
            with serial_lock:
                current_serial = serial.Serial(str(PORT_BLE), 115200, timeout=0.1)
            print("[Serial Thread] Opened /tmp/c3-ble successfully")
            broadcast_sync({"type": "ble_status", "status": "connected", "id": current_ble_id})

            # Query gains on initial connection
            time.sleep(0.1)
            write_serial(b'g')

            buf = ""
            while PORT_BLE.exists():
                try:
                    with serial_lock:
                        if not current_serial or not current_serial.is_open:
                            break
                        chunk = current_serial.read(256)
                    if not chunk:
                        time.sleep(0.01)
                        continue
                    buf += chunk.decode("utf-8", errors="ignore")
                    while "\n" in buf:
                        line, buf = buf.split("\n", 1)
                        line = line.strip()
                        if line:
                            parse_and_broadcast_line(line)
                except (serial.SerialException, OSError):
                    if not PORT_BLE.exists():
                        break
                    time.sleep(0.05)

        except Exception as e:
            print(f"[Serial Thread] Error on /tmp/c3-ble: {e}")
            time.sleep(0.5)
        finally:
            with serial_lock:
                if current_serial and current_serial.is_open:
                    try:
                        current_serial.close()
                    except Exception:
                        pass
                current_serial = None
            if not PORT_BLE.exists():
                broadcast_sync({"type": "ble_status", "status": "disconnected", "id": current_ble_id})
            time.sleep(0.5)


def parse_and_broadcast_line(line: str):
    global last_state, current_pid, current_speed

    # 1. Check PID update
    m_pid = PID_RE.search(line)
    if m_pid:
        current_pid = {
            "kp": float(m_pid.group(1)),
            "ki": float(m_pid.group(2)),
            "kd": float(m_pid.group(3))
        }
        broadcast_sync({"type": "pid", "data": current_pid, "raw": line})
        return

    # 2. Check Speed update
    m_spd = SPEED_RE.search(line)
    if m_spd:
        current_speed = int(m_spd.group(1))
        broadcast_sync({"type": "speed", "speed": current_speed, "raw": line})
        return

    # 3. Check Telemetry update
    m_tel = TELEMETRY_RE.search(line)
    if m_tel:
        valid_str = m_tel.group(1)
        state_str = m_tel.group(2)
        last_state = state_str
        
        is_valid = (valid_str == "VALID")
        p = float(m_tel.group(3)) if m_tel.group(3) else None
        e = float(m_tel.group(4)) if m_tel.group(4) else None
        c = int(m_tel.group(5)) if m_tel.group(5) else 0
        l = int(m_tel.group(6)) if m_tel.group(6) else 0
        r = int(m_tel.group(7)) if m_tel.group(7) else 0

        broadcast_sync({
            "type": "telemetry",
            "valid": is_valid,
            "state": state_str,
            "position": p,
            "error": e,
            "correction": c,
            "left_pwm": l,
            "right_pwm": r,
            "raw": line
        })
        return

    # 4. Fallback raw log
    broadcast_sync({"type": "log", "text": line})


def start_ble_bridge(sticker_id: str):
    global bridge_proc, current_ble_id
    current_ble_id = sticker_id.upper()

    # If /tmp/c3-ble already exists (e.g. connected manually in terminal), re-use it!
    if PORT_BLE.exists():
        print(f"[BLE Bridge] /tmp/c3-ble is already active. Attached to live bridge for {current_ble_id}.")
        broadcast_sync({"type": "ble_status", "status": "connected", "id": current_ble_id})
        broadcast_sync({"type": "bridge_log", "text": f">> Attached to live /tmp/c3-ble bridge ({current_ble_id})"})
        return

    print(f"[BLE Bridge] Starting connection for {current_ble_id}...")
    broadcast_sync({"type": "ble_status", "status": "connecting", "id": current_ble_id})

    # Only terminate bridge if managed by server
    stop_ble_bridge(force_all=False)
    time.sleep(0.2)

    connect_script = PROJECT_DIR / "DAY 1" / "connect.py"
    venv_python = PROJECT_DIR / "DAY 1" / ".venv" / "bin" / "python"
    py_exec = str(venv_python) if venv_python.exists() else sys.executable

    cmd = [py_exec, "-u", str(connect_script), current_ble_id, "--scan-seconds", "8", "--show-seen"]
    try:
        bridge_proc = subprocess.Popen(
            cmd,
            cwd=str(PROJECT_DIR),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True
        )

        def monitor_bridge():
            while bridge_proc and bridge_proc.poll() is None:
                line = bridge_proc.stdout.readline()
                if line:
                    stripped = line.strip()
                    broadcast_sync({"type": "bridge_log", "text": stripped})
                    if "Running main loop" in stripped or ("Device" in stripped and "connected" in stripped):
                        broadcast_sync({"type": "ble_status", "status": "connected", "id": current_ble_id})
                    elif "disconnected" in stripped.lower():
                        broadcast_sync({"type": "ble_status", "status": "disconnected", "id": current_ble_id})
            if not PORT_BLE.exists():
                broadcast_sync({"type": "ble_status", "status": "disconnected", "id": current_ble_id})

        threading.Thread(target=monitor_bridge, daemon=True).start()
    except Exception as e:
        print(f"[BLE Bridge] Spawn failed: {e}")
        broadcast_sync({"type": "ble_status", "status": "error", "error": str(e)})


def stop_ble_bridge(force_all: bool = False):
    global bridge_proc, current_serial
    with serial_lock:
        if current_serial:
            try:
                current_serial.close()
            except Exception:
                pass
            current_serial = None

    if bridge_proc:
        try:
            bridge_proc.terminate()
            bridge_proc.wait(timeout=1)
        except Exception:
            try:
                bridge_proc.kill()
            except Exception:
                pass
        bridge_proc = None

    if force_all:
        subprocess.run(["pkill", "-9", "-f", "connect.py"], check=False)
        subprocess.run(["pkill", "-9", "-f", "ble-serial"], check=False)
        for p in [Path("/tmp/c3-ble"), Path("/tmp/cu.c3-ble"), Path("/tmp/c3-ble.lock")]:
            try:
                if p.is_symlink() or p.exists():
                    p.unlink()
            except Exception:
                pass


async def ws_handler(websocket):
    connected_clients.add(websocket)
    print(f"[WS] Client connected ({len(connected_clients)} active)")

    # Send initial status
    ble_state = "connected" if PORT_BLE.exists() else "disconnected"
    await websocket.send(json.dumps({
        "type": "init",
        "ble_status": ble_state,
        "ble_id": current_ble_id,
        "pid": current_pid,
        "speed": current_speed,
        "state": last_state
    }))

    try:
        async for raw_msg in websocket:
            try:
                data = json.loads(raw_msg)
                msg_type = data.get("type")

                if msg_type == "cmd":
                    cmd = data.get("cmd", "")
                    if cmd:
                        write_serial(cmd.encode("utf-8"))
                        print(f"[Cmd] Sent char: {cmd}")

                elif msg_type == "connect_ble":
                    target_id = data.get("id", "XRSC").strip().upper()
                    start_ble_bridge(target_id)

                elif msg_type == "disconnect_ble":
                    stop_ble_bridge()
                    broadcast_sync({"type": "ble_status", "status": "disconnected", "id": current_ble_id})

                elif msg_type == "preset_speed":
                    val = str(data.get("preset", "7"))
                    write_serial(val.encode("utf-8"))

                elif msg_type == "query_pid":
                    write_serial(b'g')

            except Exception as e:
                print(f"[WS] Message handle error: {e}")
    finally:
        connected_clients.discard(websocket)
        print(f"[WS] Client disconnected ({len(connected_clients)} active)")


def run_http_server():
    server = http.server.ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), StaticHandler)
    local_ip = get_local_ip()
    print(f"[HTTP] Web Panel listening on:")
    print(f"       -> Local:   http://localhost:{HTTP_PORT}")
    print(f"       -> Network: http://{local_ip}:{HTTP_PORT}")
    server.serve_forever()


async def main():
    global loop
    loop = asyncio.get_running_loop()

    # Start serial reader background thread
    threading.Thread(target=serial_reader_thread, daemon=True).start()

    # Start HTTP server thread
    threading.Thread(target=run_http_server, daemon=True).start()

    # Start WebSocket server
    ws_server = await serve(ws_handler, "0.0.0.0", WS_PORT)
    print(f"[WebSocket] Telemetry stream on ws://0.0.0.0:{WS_PORT}")

    await asyncio.Future()  # run forever


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Line Follower Cockpit Server")
    parser.add_argument("id", nargs="?", default="XRSC", help="Initial BLE ID (default: XRSC)")
    parser.add_argument("--auto-connect", action="store_true", help="Auto-connect BLE on launch")
    args = parser.parse_args()

    current_ble_id = args.id.upper()

    if args.auto_connect:
        def auto_start():
            time.sleep(0.8)
            start_ble_bridge(current_ble_id)
        threading.Thread(target=auto_start, daemon=True).start()

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[Shutdown] Stopping servers...")
        stop_ble_bridge()

