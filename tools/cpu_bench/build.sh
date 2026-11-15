#!/bin/sh
# Build the bare-metal bench (needs arm-none-eabi-g++ on PATH).
set -e
cd "$(dirname "$0")"
arm-none-eabi-g++ -g -mcpu=cortex-m0plus -mthumb -O2 -std=c++17 -fno-exceptions -fno-rtti \
  -ffunction-sections -I../../src bench.cpp -T link.ld -nostartfiles \
  --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections \
  -Wl,--undefined=bench_one -Wl,--undefined=bench_init -Wl,--undefined=bench_run \
  -Wl,--no-warn-rwx-segments -o bench.elf
