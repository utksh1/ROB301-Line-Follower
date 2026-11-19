#!/bin/sh
# Install the Mac BLE serial utilities independently of PlatformIO.
set -eu
cd "$(dirname "$0")"
if ! command -v python3 >/dev/null 2>&1; then
    echo "Python 3 not found. Install Python 3 from python.org or via Homebrew." >&2
    exit 1
fi
python3 -m venv .venv
. .venv/bin/activate
python -m pip install 'ble-serial==3.0.0' 'pyserial>=3.5,<4'
printf '\nSetup complete.\nTo connect: %s/.venv/bin/python %s/connect.py ABCD\n' "$PWD" "$PWD"
echo 'Replace ABCD with the four-character sticker ID.'
