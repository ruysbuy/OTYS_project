#!/usr/bin/env bash
set -euo pipefail

DEST="${1:-./rootfs/alpine}"
VER="${ALPINE_VER:-3.19}"
ARCH="${ALPINE_ARCH:-x86_64}"
URL="https://dl-cdn.alpinelinux.org/alpine/v${VER}/releases/${ARCH}/alpine-minirootfs-${VER}.0-${ARCH}.tar.gz"

mkdir -p "$(dirname "$DEST")"
if [ -d "$DEST" ] && [ -f "$DEST/bin/sh" ]; then
    echo "[+] rootfs already at $DEST"
    exit 0
fi

echo "[+] downloading $URL"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL "$URL" -o "$TMP/rootfs.tgz"
mkdir -p "$DEST"
tar -xzf "$TMP/rootfs.tgz" -C "$DEST"
echo "[+] rootfs extracted to $DEST"