#!/bin/bash
# Static stack usage per function: -fstack-usage on non-LTO objects (LTO merges everything into main).
set -e
cd "$(dirname "$0")/../firmware"
mkdir -p /tmp/su
for f in App/app.c App/perf.c App/crc16.c App/journal.c App/motion.c Platform/platform.c Devices/mpu6050.c Devices/w25q64.c; do
  arm-none-eabi-gcc -c -mcpu=cortex-m0plus -mthumb -DUSE_HAL_DRIVER -DSTM32C031xx -ICore/Inc -IPlatform -IDevices -IApp \
    -isystemDrivers/STM32C0xx_HAL_Driver/Inc -isystemDrivers/CMSIS/Device/ST/STM32C0xx/Include -isystemDrivers/CMSIS/Include \
    -Os -fstack-usage -DGIT_HASH=\"x\" "$f" -o "/tmp/su/$(basename "$f" .c).o"
done
cat /tmp/su/*.su | awk -F'\t' '{print $2"\t"$1"\t"$3}' | sort -n -r | head -25