#!/usr/bin/env bash
# Build the STM32 firmware from the terminal (no CubeIDE needed).
# Toolchain: Arm GNU Toolchain 14.3.rel1 (same version the project targets).
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TC="$HOME/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi/bin"

if [ ! -x "$TC/arm-none-eabi-gcc" ]; then
    echo "ERROR: ARM toolchain not found at $TC" >&2
    echo "Download 'arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi.tar.xz'" >&2
    echo "from https://developer.arm.com/downloads/-/gnu-rm and extract to \$HOME." >&2
    exit 1
fi

export PATH="$TC:$PATH"
cd "$SCRIPT_DIR/Lab_12/Debug"
make -j"$(sysctl -n hw.ncpu)" all

# The exported makefile only builds the .elf — generate .bin/.hex ourselves.
arm-none-eabi-objcopy -O binary Lab_12.elf Lab_12.bin
arm-none-eabi-objcopy -O ihex   Lab_12.elf Lab_12.hex

echo ""
echo "Built: Lab_12/Debug/Lab_12.bin ($(stat -f%z Lab_12.bin) bytes, built $(date -r Lab_12.bin '+%Y-%m-%d %H:%M:%S'))"
echo "To flash: ./flash.sh"
