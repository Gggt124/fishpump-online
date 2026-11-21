#!/usr/bin/env bash
# Linux/CI counterpart of run_selftest.ps1: build the self-test app, run it in QEMU,
# exit 0 on "SELFTEST PASS". Needs an activated ESP-IDF shell and qemu-xtensa installed
# (python $IDF_PATH/tools/idf_tools.py install qemu-xtensa).
set -uo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="$APP_DIR/build/qemu.log"

idf.py -C "$APP_DIR" build > /dev/null || { echo "self-test build failed" >&2; exit 2; }

rm -f "$LOG"
idf.py -C "$APP_DIR" qemu > "$LOG" 2>&1 &
QEMU_JOB=$!

result=""
for _ in $(seq 1 120); do
    sleep 0.5
    if [ -f "$LOG" ]; then
        result="$(grep -E "SELFTEST (PASS|FAIL)" "$LOG" | head -n 1 || true)"
        [ -n "$result" ] && break
    fi
done
kill "$QEMU_JOB" 2>/dev/null || true
pkill -f qemu-system-xtensa 2>/dev/null || true

if [ -z "$result" ]; then
    echo "no SELFTEST line within 60 s (see $LOG)" >&2
    exit 3
fi
grep "FAIL " "$LOG" || true
echo "$result"
case "$result" in
    *"SELFTEST PASS"*) exit 0 ;;
    *) exit 1 ;;
esac
