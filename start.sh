#!/usr/bin/env bash
# ==============================================================================
# ROB301 STM32 Line Follower - Unified Mission Control Cockpit Launcher
# Starts the Web Telemetry Cockpit on http://localhost:8080 & attaches to BLE
# ==============================================================================

set -e

# Change to project root directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Color constants
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# Default parameters
DEFAULT_BOT="XRSC"
TARGET_BOT="$DEFAULT_BOT"
OPEN_BROWSER=true
SERVER_ONLY=false

# Parse arguments & flags
for arg in "$@"; do
    case "$arg" in
        --no-browser)
            OPEN_BROWSER=false
            ;;
        --server-only|--web-only)
            SERVER_ONLY=true
            ;;
        -h|--help)
            echo -e "${BOLD}Usage:${NC} ./start.sh [BOT_ID] [--no-browser] [--server-only]"
            echo ""
            echo "Options:"
            echo "  BOT_ID         Sticker ID of BLE adapter (default: XRSC, or VT78)"
            echo "  --server-only  Run only the Web Cockpit server (connect BLE manually)"
            echo "  --no-browser   Do not automatically open the browser on start"
            echo "  -h, --help     Show this help message"
            echo ""
            echo "Examples:"
            echo "  ./start.sh               # Launch with Bot 1 (XRSC)"
            echo "  ./start.sh VT78          # Launch with Bot 2 (VT78)"
            echo "  ./start.sh --server-only # If you run connect.py in another terminal"
            exit 0
            ;;
        *)
            if [[ "$arg" != -* ]]; then
                TARGET_BOT="$(echo "$arg" | tr '[:lower:]' '[:upper:]')"
            fi
            ;;
    esac
done

# Step 1: Locate Python Environment
PYTHON_BIN="$SCRIPT_DIR/DAY 1/.venv/bin/python"
if [ ! -f "$PYTHON_BIN" ]; then
    if command -v python3 >/dev/null 2>&1; then
        PYTHON_BIN="$(command -v python3)"
    else
        echo -e "${RED}[ERROR] Python 3 not found. Please install Python or set up the virtual environment.${NC}"
        exit 1
    fi
fi

# Step 2: Detect Network IP for Phone / Tablet Access
LOCAL_IP="127.0.0.1"
if command -v ipconfig >/dev/null 2>&1; then
    LOCAL_IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo "127.0.0.1")"
fi

# Step 3: Display Cockpit Banner
echo -e "${CYAN}${BOLD}"
echo "=================================================================="
echo "      🚀 STM32 LINE FOLLOWER MISSION CONTROL COCKPIT 🚀          "
echo "=================================================================="
echo -e "${NC}"
echo -e "  ${BOLD}Target Bot:${NC}      ${GREEN}C3-${TARGET_BOT}${NC}"
echo -e "  ${BOLD}Local Cockpit:${NC}   ${CYAN}http://localhost:8080${NC}"
echo -e "  ${BOLD}Phone / WiFi:${NC}    ${CYAN}http://${LOCAL_IP}:8080${NC}"
echo -e "  ${BOLD}Telemetry Port:${NC}  ws://${LOCAL_IP}:8765"
echo ""
echo -e "  ${YELLOW}Keyboard Shortcuts in Web Cockpit:${NC}"
echo -e "    • ${BOLD}Spacebar${NC}  -> Emergency STOP"
echo -e "    • ${BOLD}Enter${NC}     -> START Line Following"
echo -e "    • ${BOLD}R${NC}         -> REVERSE"
echo -e "    • ${BOLD}+ / -${NC}     -> Nudge Speed ±5%"
echo -e "    • ${BOLD}1 .. 9${NC}    -> Speed Presets (30% to 80%)"
echo -e "${CYAN}==================================================================${NC}"
echo ""

# Launch default browser in background
if [ "$OPEN_BROWSER" = true ]; then
    (sleep 1.2 && open "http://localhost:8080" 2>/dev/null || true) &
fi

# Check if an external BLE bridge is already running
if [ -e /tmp/c3-ble ] && pgrep -f "ble-serial" >/dev/null 2>&1; then
    echo -e "${GREEN}[✓] Active BLE bridge already detected on /tmp/c3-ble!${NC}"
    echo -e "${CYAN}[*] Web Cockpit will attach directly to your existing connection.${NC}"
    echo -e "${CYAN}[*] Press Ctrl+C to stop the Web Cockpit.${NC}\n"
    exec "$PYTHON_BIN" "$SCRIPT_DIR/web_panel/server.py" "$TARGET_BOT"
fi

if [ "$SERVER_ONLY" = true ]; then
    echo -e "${GREEN}[*] Starting Web Cockpit Server in Standalone Mode...${NC}"
    echo -e "${YELLOW}[i] Run your BLE connection manually in another terminal:${NC}"
    echo -e "    ${BOLD}\"$PYTHON_BIN\" \"$SCRIPT_DIR/DAY 1/connect.py\" $TARGET_BOT${NC}\n"
    exec "$PYTHON_BIN" "$SCRIPT_DIR/web_panel/server.py" "$TARGET_BOT"
fi

# Cleanup handler on exit for full mode
cleanup() {
    echo -e "\n${YELLOW}[!] Shutting down Line Follower Cockpit & BLE bridge...${NC}"
    kill "$SERVER_PID" 2>/dev/null || true
    pkill -9 -f "server\.py" 2>/dev/null || true
    pkill -9 -f "connect\.py" 2>/dev/null || true
    pkill -9 -f "ble-serial" 2>/dev/null || true
    rm -f /tmp/c3-ble /tmp/cu.c3-ble /tmp/c3-ble.lock 2>/dev/null || true
    echo -e "${GREEN}[✓] Cleaned up all background bridges and ports. Goodbye!${NC}"
    exit 0
}
trap cleanup SIGINT SIGTERM EXIT

# Clean dead lock files if NO active bridge is running
rm -f /tmp/c3-ble /tmp/cu.c3-ble /tmp/c3-ble.lock 2>/dev/null || true

# Start Web Cockpit server in background
echo -e "${GREEN}[*] Launching Web Cockpit Server on http://localhost:8080...${NC}"
"$PYTHON_BIN" "$SCRIPT_DIR/web_panel/server.py" "$TARGET_BOT" &
SERVER_PID=$!
sleep 0.8

# Start BLE Connection directly in foreground with live terminal feedback
echo -e "${CYAN}[*] Starting BLE Bridge to C3-${TARGET_BOT} in foreground...${NC}"
echo -e "${YELLOW}[i] You can watch the real-time Bluetooth connection handshake below:${NC}\n"
"$PYTHON_BIN" "$SCRIPT_DIR/DAY 1/connect.py" "$TARGET_BOT" --show-seen
