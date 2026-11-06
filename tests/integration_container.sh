#!/usr/bin/env bash
set -euo pipefail
MINIDOCKER="${1:?path to minidocker}"
MINIDOCKERD="${2:?path to minidockerd}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOTFS="$ROOT/rootfs/alpine"
if [ ! -f "$ROOTFS/bin/sh" ]; then
    echo "[!] rootfs not present at $ROOTFS. Run scripts/download-rootfs.sh first." >&2
    exit 1
fi

export MINIDOCKER_SOCKET="/tmp/minidocker-test-$$.sock"
STATE="/tmp/minidocker-state-$$"
rm -rf "$STATE"

"$MINIDOCKERD" --socket "$MINIDOCKER_SOCKET" --state-dir "$STATE" &
DPID=$!
trap 'kill $DPID 2>/dev/null || true; rm -rf "$STATE" "$MINIDOCKER_SOCKET"' EXIT

for _ in $(seq 1 50); do
    [ -S "$MINIDOCKER_SOCKET" ] && break
    sleep 0.1
done

fail=0

# 1. echo
out=$("$MINIDOCKER" run --rootfs "$ROOTFS" -- /bin/echo hello) || { echo "FAIL: echo (exit)"; fail=1; }
echo "$out" | grep -q hello || { echo "FAIL: echo (output: $out)"; fail=1; }

# 2. PID 1
out=$("$MINIDOCKER" run --rootfs "$ROOTFS" -- /bin/sh -c 'echo $$')
[ "$out" = "1" ] || { echo "FAIL: PID 1 (got '$out')"; fail=1; }

# 3. exit via detached + logs
id=$("$MINIDOCKER" run -d --rootfs "$ROOTFS" -- /bin/sh -c 'echo bye; exit 42')
sleep 0.5
logs=$("$MINIDOCKER" logs "$id" 2>/dev/null || true)
echo "$logs" | grep -q bye || { echo "FAIL: logs (got '$logs')"; fail=1; }

# 4. /proc mounted
out=$("$MINIDOCKER" run --rootfs "$ROOTFS" -- /bin/cat /proc/1/comm)
[ -n "$out" ] || { echo "FAIL: /proc not mounted"; fail=1; }

# 5. list shows the container
"$MINIDOCKER" ps -a | grep -q "$id" || { echo "FAIL: ps"; fail=1; }

# 6. stop running container
id2=$("$MINIDOCKER" run -d --rootfs "$ROOTFS" -- /bin/sleep 100)
sleep 0.3
"$MINIDOCKER" stop "$id2"
sleep 0.5
"$MINIDOCKER" ps -a | grep "$id2" | grep -q stopped || { echo "FAIL: stop"; fail=1; }

if [ $fail -eq 0 ]; then echo "[+] container tests: OK"; fi
exit $fail