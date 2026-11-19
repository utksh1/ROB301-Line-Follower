# 🚀 STM32 Line Follower & Web Mission Control Cockpit

An autonomous line-following robot system featuring STM32 NUCLEO-G474RE embedded firmware, ESP32-C3 BLE wireless telemetry, and a full-featured real-time Web Cockpit with live oscillogram, sensor HUD, and instant PID tuning.

Designed for the **ROB301 Lab 12** curriculum and optimized for continuous high-speed tracking and complex floor track navigation (including grid cross-lines and 90° turns).

---

## 📸 System Architecture

```text
       ┌────────────────────────┐
       │   8-IR Sensor Array    │
       │    (PC0 - PC7 ADC)     │
       └───────────┬────────────┘
                   │
                   ▼
       ┌────────────────────────┐
       │  STM32 NUCLEO-G474RE   │ ◄─── USART1 (115200) ───► ┌────────────────────────┐
       │  Anti-Wobble Damped PID│                           │ ESP32-C3 BLE Adapter   │
       │  Grid Intersection Hndl│                           │ (Nordic UART Service)  │
       └───────────┬────────────┘                           └───────────┬────────────┘
                   │ TIM2/TIM3 PWM                                      │ BLE 5.0
                   ▼                                                    ▼
       ┌────────────────────────┐                           ┌────────────────────────┐
       │   L298N Motor Driver   │                           │ Mac / Browser Cockpit  │
       │  Differential Wheels   │                           │ (HTTP:8080 / WS:8765)  │
       └────────────────────────┘                           └────────────────────────┘
```

---

## ✨ Features

### 1. Embedded Firmware (`Lab_12/Core/Src/main.c`)
- **Anti-Wobble Low-Pass Filtered PID**:
  - Cruising base speed: `70%` PWM
  - Proportional gain: `Kp = 2.2` (eliminates aggressive steering kicks)
  - Derivative damping: `Kd = 16.0` with exponential low-pass filter ($0.35 \cdot \text{raw} + 0.65 \cdot \text{filtered}$) to prevent centerline overshoot
  - Integral gain: `Ki = 0.0` (eliminates phase-lag hunting oscillation)
  - Center deadband: $|E| \le 0.55$ locks both wheels straight on sensors 3 & 4
  - Motor stall threshold protection: `MIN_PWM = 55%` prevents inner wheel stall on L298N $V_{CE(sat)}$ drop
- **Grid Intersection Recognition**:
  - Safely handles ladder grid tracks with cross-lines every ~30 cm without mistaking intersections for sharp turns or line loss
  - Glides straight through cross-lines when center sensors confirm the forward path
- **Controlled 90° Turn Pivot**:
  - Activates when line is lost, executing an outside-wheel pivot (80% / -45%) with instant re-acquisition upon finding the center line

### 2. Web Mission Control Cockpit (`web_panel/`)
- **Live 8-IR Sensor HUD**: Dynamic LEDs, analog centerline error needle, and dual vertical wheel PWM gauges
- **60 FPS Real-Time Canvas Oscillogram**: Plots error $E$, Left Motor PWM, and Right Motor PWM over time
- **Primary Motion Controls**:
  - `START` (Enter): Activate autonomous line tracking
  - `EMERGENCY STOP` (Spacebar): Immediate motor halt
  - `REVERSE` (R): Back up away from obstacles
- **Live Speed Adjustment**: Slider and 9 speed presets ($30\%$ to $80\%$) with `+` / `-` incremental nudging
- **On-The-Fly PID Tuner**: Real-time gain adjustment buttons ($K_p$, $K_d$, $K_i$) that send UART update commands instantly
- **Terminal Console**: Live serial log stream with auto-scroll and clear

---

## ⚡ Quickstart

### 1. Setup (First Time Only)
```bash
./DAY\ 1/setup_mac.sh
```

### 2. Launch Cockpit & Connect to Robot
Run the unified launcher with your robot's BLE sticker ID:

```bash
# Connect to Old Bot (Bot 1):
./start.sh XRSC

# Connect to New Bot (Bot 2):
./start.sh VT78
```

The launcher will:
1. Start the Web Cockpit server on `http://localhost:8080` (and `http://<YOUR_IP>:8080` for mobile phone / tablet access)
2. Open your default web browser
3. Run the BLE connection handshake in the foreground with live progress logs

---

## 🎮 Keyboard Shortcuts

| Key | Action |
|:---:|:---|
| **Spacebar** | 🛑 Emergency STOP |
| **Enter** | ▶ START Line Following |
| **R** | ⏪ REVERSE |
| **`+` / `-`** | Nudge Cruising Speed $\pm 5\%$ |
| **`1` .. `9`** | Speed Presets ($30\%$ to $80\%$) |

---

## 🛠 Project Layout

```text
.
├── start.sh              # One-click launcher for Web Cockpit & BLE Bridge
├── web_panel/            # Mission Control Web Application
│   ├── server.py         # Async HTTP & WebSocket backend
│   └── static/           # Modern dashboard UI (HTML/CSS/JS)
├── Lab_12/               # STM32CubeIDE Firmware Project
│   ├── Core/Src/main.c   # PID algorithm, sensor processing, UART commands
│   ├── Core/Inc/         # Hardware headers
│   └── Debug/            # Flashed binary (Lab_12.bin) and makefiles
├── DAY 1/                # BLE UART connection utilities & Bleak bridge
│   ├── connect.py        # ESP32-C3 NUS BLE to PTY bridge
│   └── setup_mac.sh      # Python virtual environment setup
├── DAY 2/                # Lab 12 instructions & debug UART headers
├── DAY 3/                # Documentation & lab reports
└── logger.py             # Standalone serial data logging utility
```

---

## 📄 License
MIT License. Created for ROB301 Lab 12.
