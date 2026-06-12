# tools/

Hilfswerkzeuge außerhalb des Firmware-Builds (werden **nicht** mitgeflasht).

## Screenshots (ohne Gerät)

E-Ink hat keinen „OS-Screenshot". Statt den Framebuffer vom Gerät zu dumpen,
rendert `screenshot.cpp` die Bildschirme **host-seitig** mit demselben
Adafruit-GFX-Zeichencode und denselben CP437-Fonts wie die Firmware in einen
`GFXcanvas1` (240×320, 1 Bit) — das Ergebnis ist **pixelgenau** zur Anzeige,
nur mit Beispiel-Daten.

```bash
pio run -e fennek        # einmalig, damit Adafruit-GFX unter .pio/libdeps/ liegt
bash tools/screenshots.sh        # -> docs/screenshots/*.png  (Standard 2x skaliert)
bash tools/screenshots.sh 3      # andere Skalierung
```

Bestandteile:

- `screenshot.cpp` — rendert Launcher, Musik-Player, Tic-Tac-Toe und Standby.
  Die Zeichen-Bodies sind 1:1 aus den Quellen gespiegelt (Launcher
  `apps/launcher.cpp`, TTT `apps/ttt.cpp`, Player `apps/music_app.cpp`, Standby
  `core/power.cpp`), weil die echten `draw()` in anonymen Namespaces bzw. hinter
  Geräte-Zustand liegen. Gemeinsame Helfer (`core/gui.cpp`, das Bitmap
  `core/sleep_img.h`) und der Adafruit-GFX-Code werden direkt verlinkt.
- `hostshim/` — minimaler Arduino-/Print-/pgmspace-Shim, damit Adafruit-GFX am
  PC kompiliert (`GxEPD2_BW.h` liefert nur die Farbkonstanten).
- `make_screenshots.py` — skaliert die PGMs und exportiert PNGs (braucht Pillow).
- `screenshots.sh` — Orchestrator (kompilieren → rendern → PNG).

> Hinweis: Ändert sich Layout/Text einer App, müssen die gespiegelten Bodies in
> `screenshot.cpp` nachgezogen und die Screenshots neu erzeugt werden.

## Host-Tests

- `host_test_games.cpp` — Unit-Tests der Arduino-freien Spiele-Cores
  (2048/Minensucher/Schach/TTT). Aufruf siehe Dateikopf.
