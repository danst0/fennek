#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dr. Daniel Dumke
# =============================================================================
# maps_tiles.py — OSM-Kacheln laden, auf 1-bit dithern und für die Fennek-
# Maps-App packen. Läuft am PC (NICHT auf dem Gerät), Ergebnis kommt auf die SD.
#
# Ausgabeformat (passend zu services/maps_tiles + Adafruit_GFX::drawBitmap):
#   /maps/{z}/{x}/{y}.bin  — 256x256 px, 1-bit, row-major, MSB zuerst,
#   32 Byte/Zeile = 8192 Byte. Bit gesetzt = schwarz (Land/Linien).
#
# Beispiel (Dortmund, Zoom 13-16) in einen lokalen Ordner ./sdcard/maps:
#   python tools/maps_tiles.py --bbox 51.40 7.30 51.56 7.62 \
#       --zoom 13-16 --out ./sdcard
# Dann den Inhalt von ./sdcard auf die SD kopieren (es entsteht ./sdcard/maps).
#
# Kachelquelle per --preset wählen (oder --url für eine eigene):
#   --preset standard          OSM-Standardlayer (Default, direkt von OSM)
#   --preset cyclosm           CyclOSM — Rad-/Wegeinfrastruktur betont
#   --preset topo              OpenTopoMap — Wanderwege/Pfade/Höhenlinien
#   --preset cassius           OSM Standard via lokalem Tile-Proxy (cassius:8765)
#   --preset cassius-cyclosm   CyclOSM via Proxy
#   --preset cassius-topo      Topo via Proxy
# Detailstufen (z14-16) lohnen sich bei cyclosm/topo am meisten.
# Mit cassius-Presets: erster Lauf lädt von OSM und cached auf cassius,
# alle weiteren Läufe kommen sofort aus dem Proxy-Cache (--delay 0 möglich).
#
# Hinweise:
#  * Jede Quelle hat eine eigene Tile-Usage-Policy (kein Massen-/Bulk-Download);
#    für größere Gebiete einen eigenen Renderer/MBTiles-Server nutzen
#    (--url anpassen). User-Agent wird gesetzt.
#  * Schwellwert/Invert je nach Layer anpassen (--threshold, --invert); der
#    Preset-Default für --threshold ist je Layer abgestimmt.
#  * Der PNG-Cache wird je Quelle getrennt abgelegt (<cache>/<preset>).
#
# Abhängigkeit: Pillow  (pip install pillow)
# =============================================================================
import argparse
import hashlib
import math
import os
import sys
import time
import urllib.request

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow fehlt — bitte 'pip install pillow' ausführen.")

TILE_PX = 256
USER_AGENT = "fennek-maps-tiles/1.0 (https://github.com/danst0/fennek)"

# Fertige Kachelquellen. Jede hat eine eigene Nutzungsrichtlinie (kein Bulk-
# Download!) — für große Gebiete einen eigenen Renderer nutzen. Der Default-
# Schwellwert ist je Layer abgestimmt (topo/cyclosm sind farbig/heller als der
# Standardstil) und lässt sich mit --threshold übersteuern.
#
# cassius-* — lokaler Tile-Proxy (http://cassius:8765) mit persistentem Cache;
# kein OSM-Rate-Limit-Problem, --delay 0 möglich nach dem ersten Warm-up.
# Setup: /home/danst/dockers/tile-proxy/ auf cassius.
CASSIUS = "http://10.0.2.71:8765"
PRESETS = {
    # OSM-Standardlayer (Carto): kräftige Straßen, Wege eher schwach.
    "standard":         {"url": "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
                         "threshold": 160},
    # CyclOSM: hebt Rad-/Wegeinfrastruktur hervor.
    "cyclosm":          {"url": "https://a.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png",
                         "threshold": 190},
    # OpenTopoMap: Wanderwege, Pfade, Höhenlinien (topografisch).
    "topo":             {"url": "https://a.tile.opentopomap.org/{z}/{x}/{y}.png",
                         "threshold": 180},
    # Lokaler Cassius-Proxy — gleiche Layer, kein direkter OSM-Kontakt.
    "cassius":          {"url": f"{CASSIUS}/osm/{{z}}/{{x}}/{{y}}.png",
                         "threshold": 160},
    "cassius-cyclosm":  {"url": f"{CASSIUS}/cyclosm/{{z}}/{{x}}/{{y}}.png",
                         "threshold": 190},
    "cassius-topo":     {"url": f"{CASSIUS}/topo/{{z}}/{{x}}/{{y}}.png",
                         "threshold": 180},
}
DEFAULT_URL = PRESETS["standard"]["url"]


def deg2tile(lat, lon, z):
    """WGS84 -> Slippy-Tile-Index (x, y) bei Zoom z."""
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    lat_rad = math.radians(lat)
    y = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def pack_1bit(img, threshold, invert):
    """256x256-Graustufenbild (gedithert) -> 8192 Byte, MSB zuerst, Bit=schwarz."""
    img = img.convert("L").resize((TILE_PX, TILE_PX))
    # Floyd-Steinberg-Dithering auf 1-bit; '1' = weiß, '0' = schwarz in PIL '1'.
    bw = img.point(lambda p: 255 if p >= threshold else 0).convert("1")
    px = bw.load()
    out = bytearray(TILE_PX * TILE_PX // 8)
    for y in range(TILE_PX):
        for x in range(TILE_PX):
            black = (px[x, y] == 0)
            if invert:
                black = not black
            if black:
                out[(y * TILE_PX + x) >> 3] |= 0x80 >> (x & 7)
    return bytes(out)


def fetch_tile(url_tmpl, z, x, y, cache_dir, retries=3):
    """PNG-Kachel laden (mit lokalem Cache), als PIL-Image zurückgeben."""
    if cache_dir:
        cp = os.path.join(cache_dir, str(z), str(x), f"{y}.png")
        if os.path.exists(cp):
            return Image.open(cp)
    url = url_tmpl.format(z=z, x=x, y=y)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read()
            break
        except Exception:
            if attempt == retries - 1:
                raise
            time.sleep(2 ** attempt)   # 1 s, 2 s
    if cache_dir:
        cp = os.path.join(cache_dir, str(z), str(x), f"{y}.png")
        os.makedirs(os.path.dirname(cp), exist_ok=True)
        with open(cp, "wb") as f:
            f.write(data)
    from io import BytesIO
    return Image.open(BytesIO(data))


def parse_zoom(s):
    if "-" in s:
        a, b = s.split("-", 1)
        return range(int(a), int(b) + 1)
    return [int(s)]


def main():
    ap = argparse.ArgumentParser(description="OSM-Kacheln -> Fennek-1-bit-Tiles")
    ap.add_argument("--bbox", nargs=4, type=float, required=True,
                    metavar=("LAT1", "LON1", "LAT2", "LON2"),
                    help="Bounding-Box (zwei Eckpunkte, Reihenfolge egal)")
    ap.add_argument("--zoom", required=True, help="Zoom z oder Bereich z1-z2")
    ap.add_argument("--out", default="./sdcard", help="Zielwurzel (erzeugt <out>/maps)")
    ap.add_argument("--preset", choices=sorted(PRESETS), default="standard",
                    help="Kachelquelle: standard | cyclosm (Radwege) | topo (Wanderwege)")
    ap.add_argument("--url", default=None,
                    help="Tile-URL-Template {z}/{x}/{y} (übersteuert --preset)")
    ap.add_argument("--cache", default="./tile_cache", help="PNG-Cache-Ordner ('' = aus)")
    ap.add_argument("--threshold", type=int, default=None,
                    help="Schwarz/Weiß-Schwelle 0..255 (Default je nach --preset)")
    ap.add_argument("--invert", action="store_true", help="Schwarz/Weiß tauschen")
    ap.add_argument("--delay", type=float, default=0.2, help="Pause zwischen Downloads (s)")
    args = ap.parse_args()

    # Preset auflösen; explizite --url/--threshold haben Vorrang.
    preset = PRESETS[args.preset]
    url_tmpl = args.url if args.url is not None else preset["url"]
    threshold = args.threshold if args.threshold is not None else preset["threshold"]
    print(f"Quelle: {args.preset} ({url_tmpl})  Schwelle={threshold}")

    lat1, lon1, lat2, lon2 = args.bbox
    lat_min, lat_max = min(lat1, lat2), max(lat1, lat2)
    lon_min, lon_max = min(lon1, lon2), max(lon1, lon2)
    # PNG-Cache je Quelle trennen, sonst würde z/x/y.png einer Quelle für eine
    # andere wiederverwendet (z. B. nach Wechsel standard -> topo).
    cache = None
    if args.cache:
        key = (args.preset if args.url is None
               else "custom-" + hashlib.md5(url_tmpl.encode()).hexdigest()[:8])
        cache = os.path.join(args.cache, key)
    maps_root = os.path.join(args.out, "maps")

    total = made = skipped = 0
    for z in parse_zoom(args.zoom):
        x0, y0 = deg2tile(lat_max, lon_min, z)   # NW-Ecke -> min x, min y
        x1, y1 = deg2tile(lat_min, lon_max, z)   # SE-Ecke -> max x, max y
        xs = range(min(x0, x1), max(x0, x1) + 1)
        ys = range(min(y0, y1), max(y0, y1) + 1)
        n = len(xs) * len(ys)
        total += n
        print(f"[z{z}] {len(xs)}x{len(ys)} = {n} Kacheln")
        for x in xs:
            out_dir = os.path.join(maps_root, str(z), str(x))
            os.makedirs(out_dir, exist_ok=True)
            for y in ys:
                out_path = os.path.join(out_dir, f"{y}.bin")
                if os.path.exists(out_path):
                    skipped += 1
                    continue
                try:
                    img = fetch_tile(url_tmpl, z, x, y, cache)
                    with open(out_path, "wb") as f:
                        f.write(pack_1bit(img, threshold, args.invert))
                    made += 1
                    if cache is None or not os.path.exists(
                            os.path.join(cache or "", str(z), str(x), f"{y}.png")):
                        time.sleep(args.delay)   # nur bei echtem Download drosseln
                except Exception as ex:   # noqa: BLE001
                    print(f"  ! z{z}/{x}/{y}: {ex}", file=sys.stderr)
    print(f"Fertig: {made} erzeugt, {skipped} vorhanden, {total} geplant -> {maps_root}")


if __name__ == "__main__":
    main()
