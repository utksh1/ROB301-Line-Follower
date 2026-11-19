# C3 BLE UART — Student setup (macOS)

## What you need

- Your ESP32-C3 adapter with a **four-character sticker** (e.g. `F2DB`)
- A Mac with Bluetooth switched on
- This folder containing `connect.py` and `setup_mac.sh`
- Python 3 (`python3 --version`), installed from Homebrew or python.org if not already present

The adapter runs **115200 baud, 8 data bits, no parity, 1 stop bit**. You do **not** pair it from System Settings. You connect through the launcher.

## One-time setup

```sh
cd ~/c3-ble
sh setup_mac.sh
```

Allow macOS Bluetooth permission to Terminal/Python when prompted (System Settings → Privacy & Security → Bluetooth).

## Every session

Power your ESP32 adapter, connect its TX/RX/GND to the Nucleo, then run:

```sh
cd ~/c3-ble
./.venv/bin/python connect.py
```

Enter the **four-character sticker ID** when asked. Or specify it explicitly:

```sh
./.venv/bin/python connect.py F2DB
```

The launcher:
1. Scans live BLE **advertised local names**, ignoring stale CoreBluetooth names.
2. Resolves the matching ESP32 to this Mac's current peripheral UUID.
3. Launches `ble-serial` to expose **`/tmp/c3-ble`**.
4. Automatically rescans/restarts the bridge if `ble-serial` exits. Keep the launcher terminal open.

Wait until `ble-serial` reports `Device connected` / `Running main loop!` before opening the serial port. Creating the PTY alone doesn't prove a BLE connection.

## Use a terminal

In another terminal:

```sh
cd ~/c3-ble
./.venv/bin/python -m serial.tools.miniterm /tmp/c3-ble 115200
```

Exit miniterm with **Ctrl+]**. Or select `/tmp/c3-ble` manually in a compatible serial plotting application and choose 115200, 8N1, no flow control. The baud value is accepted locally; the ESP32 UART is fixed at 115200.

## When finished

Close your plotting application first, then press **Ctrl+C** in the launcher terminal. This closes the BLE bridge and removes its port link when owned by this launcher.

## Troubleshooting

| Symptom | Action |
|---|---|
| Device not found | Check power, sticker, macOS Bluetooth permission, and whether another Mac/phone is connected |
| Scans show old `G474-BLE` | CoreBluetooth may have cached that name; our script uses the current advertised local name instead |
| `C3-XXXX` found but no port data | Wait for BLE connection and subscription; check Nucleo wiring and UART output |
| `/tmp/c3-ble` already exists | Stop any independently launched `ble-serial` instance first |
| App doesn't list the port | Manually enter `/tmp/c3-ble` if supported; it is a macOS PTY, not a true `/dev/cu.*` hardware serial port |
| Repeated reconnects | Move device closer, close competing BLE apps, check adapter power and USB cable |

One application should own `/tmp/c3-ble` at a time. Do not expect multiple plotting programs to receive independent copies of the same stream.
