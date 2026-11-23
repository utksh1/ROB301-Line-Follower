#!/usr/bin/env bash
# Build and flash the firmware over the Nucleo-G474RE's onboard ST-Link.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"$SCRIPT_DIR/build.sh"

CLI="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"
if [ ! -x "$CLI" ]; then
    echo "ERROR: STM32_Programmer_CLI not found at $CLI" >&2
    exit 1
fi

echo ""
echo "Flashing over SWD (plug the Nucleo in by USB)..."
"$CLI" -c port=SWD mode=UR -w "$SCRIPT_DIR/Lab_12/Debug/Lab_12.bin" 0x08000000 -v -rst
