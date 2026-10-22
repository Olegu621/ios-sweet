#!/bin/sh
# Copy the previous XNU console from recovery's pstore. Read-only on device.
set -eu
if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    echo "Usage: $0 OUTDIR (phone must already be in recovery with root ADB)"
    exit 0
fi
out=${1:?usage: peridot-pstore-pull.sh OUTDIR}
mkdir -p "$out"
# adb wait-for-device does not accept a transport in 'recovery' state.
timeout 10 adb shell 'test "$(id -u)" = 0'
files=$(timeout 10 adb shell 'find /sys/fs/pstore -maxdepth 1 -type f' | tr -d '\r')
[ -n "$files" ] || { echo "pstore is empty; no retained log found" >&2; exit 1; }
for path in $files; do
    case "$path" in
        /sys/fs/pstore/*)
            name=${path##*/}
            timeout 20 adb pull "$path" "$out/$name"
            ;;
    esac
done
echo "Saved recovery pstore to $out"
