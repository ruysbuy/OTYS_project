#!/usr/bin/env bash
set -euo pipefail
MINIDOCKER="${1:?path to minidocker}"
MINIDOCKERD="${2:?path to minidockerd}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOTFS="$ROOT/rootfs/alpine"
if [ ! -f "$ROOTFS/bin/sh" ]; then
    echo "[!] rootfs not present. Run scripts/download-rootfs.sh first." >&2
    exit 1
fi

export MINIDOCKER_SOCKET="/tmp/minidocker-proto-$$.sock"
STATE="/tmp/minidocker-proto-state-$$"
rm -rf "$STATE"

"$MINIDOCKERD" --socket "$MINIDOCKER_SOCKET" --state-dir "$STATE" &
DPID=$!
trap 'kill $DPID 2>/dev/null || true; rm -rf "$STATE" "$MINIDOCKER_SOCKET"' EXIT

for _ in $(seq 1 50); do [ -S "$MINIDOCKER_SOCKET" ] && break; sleep 0.1; done

fail=0

# --- run detach + list
id=$("$MINIDOCKER" run -d --rootfs "$ROOTFS" -- /bin/sh -c 'echo line1; echo line2; sleep 1')
"$MINIDOCKER" ps | grep -q "$id" || { echo "FAIL: ps running"; fail=1; }

# --- logs
sleep 0.5
logs=$("$MINIDOCKER" logs "$id")
echo "$logs" | grep -q line1 || { echo "FAIL: logs line1"; fail=1; }
echo "$logs" | grep -q line2 || { echo "FAIL: logs line2"; fail=1; }

# --- logs -f streaming
id2=$("$MINIDOCKER" run -d --rootfs "$ROOTFS" -- /bin/sh -c 'for i in 1 2 3 4 5; do echo "tick$i"; sleep 0.3; done')
sleep 0.3
timeout 3 "$MINIDOCKER" logs -f "$id2" > /tmp/logsf.$$ 2>&1 || true
grep -q tick /tmp/logsf.$$ || { echo "FAIL: logs -f"; fail=1; }
rm -f /tmp/logsf.$$

# --- stop
"$MINIDOCKER" stop "$id2"
sleep 0.5
"$MINIDOCKER" ps -a | grep "$id2" | grep -q stopped || { echo "FAIL: stop status"; fail=1; }

# --- attach non-destructive
id3=$("$MINIDOCKER" run -td --rootfs "$ROOTFS" -- /bin/sh -c 'echo ATTACH_OK; sleep 5')
sleep 0.5
timeout 1 "$MINIDOCKER" attach "$id3" </dev/null >/dev/null 2>&1 || true
"$MINIDOCKER" ps | grep -q "$id3" || { echo "FAIL: attach killed container"; fail=1; }
"$MINIDOCKER" stop "$id3"

if [ $fail -eq 0 ]; then echo "[+] daemon tests: OK"; fi
exit $fail