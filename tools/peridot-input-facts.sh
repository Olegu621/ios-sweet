#!/bin/sh
# Collect the facts needed to port the FT3683G touch (and the power/volume
# keys) to SM8635. Run as root in the OrangeFox recovery.
echo "=== uptime / recovery ==="
cat /proc/uptime
echo
echo "=== input devices (the FocalTech driver must be bound here) ==="
cat /proc/bus/input/devices 2>&1 | head -60
echo
echo "=== touchscreen node ==="
ls -l /dev/input/ 2>&1
for e in /dev/input/event*; do
	[ -e "$e" ] || continue
	echo "--- $e"
	# ABS_MT_POSITION_X / ABS_MT_POSITION_Y / ABS_MT_TRACKING_ID
	cat "$e" 2>/dev/null | head -c 4096 | od -A d -t x1 | head -20
done
echo
echo "=== focaltech module ==="
ls /sys/module/ 2>/dev/null | grep -i -E "focal|fts|touch"
cat /sys/module/focaltech_touch/parameters/* 2>/dev/null | head -20
echo
echo "=== spi / i2c buses ==="
ls /dev/spidev* 2>&1
ls /sys/bus/spi/devices/ 2>&1
echo
echo "=== SPMI ==="
ls /sys/bus/spmi/devices/ 2>&1
ls /sys/bus/platform/devices/ 2>&1 | grep -i -E "spmi|pmic" | head -20
echo
echo "=== PMIC gpio (pmk8550 / pmxr2230) ==="
for d in /sys/kernel/debug/*pmi* /sys/kernel/debug/*gpio*; do
	[ -e "$d" ] && echo "--- $d" && ls "$d" 2>&1 | head -20
done
echo
echo "=== regulators for the touch ==="
for r in avdd vdd iovdd; do
	f=$(ls /sys/kernel/debug/regulator/*/name 2>/dev/null | head -0)
done
cat /sys/kernel/debug/regulator/*/name 2>/dev/null | head -40
