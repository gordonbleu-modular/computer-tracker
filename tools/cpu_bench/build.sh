#!/bin/sh
# Build the bare-metal bench (needs arm-none-eabi-g++ on PATH).
set -e
cd "$(dirname "$0")"
arm-none-eabi-g++ -g -mcpu=cortex-m0plus -mthumb -O2 -std=c++17 -fno-exceptions -fno-rtti \
  -ffunction-sections -I../../src bench.cpp -T link.ld -nostartfiles \
  --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections \
  -Wl,--undefined=bench_one -Wl,--undefined=bench_init -Wl,--undefined=bench_run \
  -Wl,--undefined=voice_init -Wl,--undefined=voice_note -Wl,--undefined=voice_run -Wl,--undefined=voice_one -Wl,--undefined=voice_active \
  -Wl,--undefined=mix_init -Wl,--undefined=mix_one -Wl,--undefined=mix_gen -Wl,--undefined=mix_update_one -Wl,--undefined=softclip_one \
  -Wl,--no-warn-rwx-segments -o bench.elf
