#!/usr/bin/env bash
# =============================================================================
# screenshots.sh — erzeugt die README-Screenshots vollautomatisch (ohne Gerät).
#
# Rendert fünf Fennek-Bildschirme mit dem Host-Renderer (tools/screenshot.cpp,
# echter Adafruit-GFX-Zeichencode -> pixelgenau) und exportiert sie als PNG
# nach docs/screenshots/<sprache>/ — eine Sprache pro README.
#
#   tools/screenshots.sh [scale] [sprachen...]   (Default: 2, "en de sv")
#
# Voraussetzungen: g++, python3 + Pillow, und einmal `pio run -e fennek`
# (damit die Adafruit-GFX-Lib unter .pio/libdeps/ liegt).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="docs/screenshots"
SCALE="${1:-2}"
shift || true
LANGS=("$@")
[ ${#LANGS[@]} -eq 0 ] && LANGS=(en de sv)

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

for LANG in "${LANGS[@]}"; do
  echo "==> rendere Bildschirme ($LANG) ..."
  rm -f "$TMP"/*.pgm
  "$TMP/fennek_shot" "$TMP" "$LANG"
  echo "==> exportiere PNGs nach $OUT/$LANG (Skalierung ${SCALE}x) ..."
  python3 tools/make_screenshots.py "$TMP" "$OUT/$LANG" "$SCALE"
done

echo "fertig."
