#!/usr/bin/env bash
# =============================================================================
# screenshots.sh — erzeugt die README-Screenshots vollautomatisch (ohne Gerät).
#
# Rendert vier Fennek-Bildschirme mit dem Host-Renderer (tools/screenshot.cpp,
# echter Adafruit-GFX-Zeichencode -> pixelgenau) und exportiert sie als PNG
# nach docs/screenshots/.
#
# Voraussetzungen: g++, python3 + Pillow, und einmal `pio run -e fennek`
# (damit die Adafruit-GFX-Lib unter .pio/libdeps/ liegt).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="docs/screenshots"
SCALE="${1:-2}"

GFX="$(find .pio/libdeps -maxdepth 2 -type d -name 'Adafruit GFX Library' 2>/dev/null | head -1)"
if [ -z "$GFX" ]; then
  echo "Adafruit-GFX nicht gefunden — bitte zuerst 'pio run -e fennek' ausführen." >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> kompiliere Host-Renderer ..."
g++ -std=c++17 -O2 -w -DARDUINO=10805 \
  -Itools/hostshim -Isrc -I"$GFX" \
  tools/screenshot.cpp src/core/gui.cpp "$GFX/Adafruit_GFX.cpp" \
  -o "$TMP/fennek_shot"

echo "==> rendere Bildschirme ..."
"$TMP/fennek_shot" "$TMP"

echo "==> exportiere PNGs nach $OUT (Skalierung ${SCALE}x) ..."
python3 tools/make_screenshots.py "$TMP" "$OUT" "$SCALE"

echo "fertig."
