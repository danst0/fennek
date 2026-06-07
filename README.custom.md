# Meck — Custom-Build für T-Deck Pro (Audio-Variante)

Batterie-fokussierter Fork von [pelgraine/Meck](https://github.com/pelgraine/Meck)
für das **LilyGo T-Deck Pro mit PCM5102A-Audio-DAC** (E-Reader + MP3-Player +
MeshCore-LoRa, Touch + Tastatur, Dateiverwaltung per Browser im Heim-WLAN).

## Was dieser Fork ergänzt

| Feature | Flag | Beschreibung |
|---|---|---|
| Dateimanager im Heim-WLAN | `MECK_FILEMGR_STA` | Der SD-Dateimanager (Settings → OTA Tools → SD File Manager) verbindet sich mit dem gespeicherten WLAN (`/web/wifi.cfg` bzw. Settings → WiFi Setup) und zeigt die **LAN-IP auf dem Display**. Ohne erreichbares WLAN: Fallback auf den bisherigen SoftAP + Captive Portal. |
| Umbenennen im Dateimanager | — | `Ren`-Button je Datei/Ordner im Web-UI. |
| LoRa-Toggle | `MECK_RADIO_TOGGLES` | Settings-Zeile **„LoRa Mesh: ON/OFF"** — schaltet den SX1262 in den Sleep (Konfiguration bleibt erhalten) und pausiert die Mesh-Loop. Persistiert, wird beim Boot angewendet. |
| WiFi-Boot-Toggle | `MECK_RADIO_TOGGLES` | Settings-Zeile **„WiFi at Boot: ON/OFF"** — WiFi bleibt nach dem Boot aus, bis es gebraucht wird (Dateimanager verbindet sich bei Bedarf selbst). |
| WiFi ab Werk aus | `MECK_WIFI_AUTOSTART_DEFAULT_OFF` | Erster Boot startet mit WiFi aus (Batterie-Fokus). |
| E-Ink-Booster aus im Idle | `EINK_POWER_OFF_IDLE` | Nach jedem Refresh wird der Panel-Booster abgeschaltet (`powerOff()`, nicht `hibernate()` — der braucht eine RST-Leitung). Das Bild bleibt stromlos stehen; der nächste Refresh schaltet automatisch wieder ein (~50 ms Zusatzlatenz). |
| Einfacher Launcher | `MECK_SIMPLE_LAUNCHER` | Neuer Home-Screen: Karussell mit 4 großen Icons (**E-Reader → MP3 Player → MeshCore → Settings**). Wischen oder Tippen links/rechts blättert, Tippen in der Mitte (oder Enter) öffnet. MeshCore-Icon zeigt ungelesene Nachrichten und öffnet direkt die Kanal-/Nachrichtenliste — das alte Meck-Home (Statusseiten) ist archiviert und nicht mehr erreichbar. **Hinweis:** Damit ist auch Standby/Power-Off derzeit nicht erreichbar (lag im alten Home; `UITask::shutdown()` existiert weiter, bei Bedarf als Settings-Eintrag nachrüstbar). |
| Einfache Settings | `MECK_SIMPLE_LAUNCHER` | Settings-Icon öffnet eine flache Liste: **WiFi / LoRa Mesh / GPS** als Direkt-Schalter (Tap kippt sofort), **File Manager >>** (startet den SD-Web-Dateimanager direkt) und **Advanced >>** (die kompletten alten Meck-Settings, eine Ebene tiefer). Footer zeigt SSID + IP bei WiFi-Verbindung. |
| Touch auf dem Pro | `MECK_HYN_TOUCH_PRO` | Offizieller Hynitron-Multi-Chip-Treiber (CST226SE/CST3xx/CST66xx-Autodetection) statt des minimalen CST328-Treibers — LilyGo hat je Charge verschiedene Touch-Controller verbaut. |
| Linux-Build-Fixes | — | Case-sensitive Includes/Pfade korrigiert; baut jetzt auch auf Linux/case-sensitiven Dateisystemen. |

Bluetooth ist in diesem Build **nicht einkompiliert** (WiFi-Companion-Variante)
und verbraucht daher keinerlei Strom.

## Bauen & Flashen

```bash
pip install platformio
pio run -e meck_audio_wifi_custom

# Erstinstallation (löscht alles, inkl. vorformatiertem SPIFFS):
esptool.py --chip esp32s3 write_flash 0x0 .pio/build/meck_audio_wifi_custom/firmware-merged.bin

# Updates danach (Einstellungen bleiben erhalten):
pio run -e meck_audio_wifi_custom -t upload
# oder OTA: Settings → OTA Tools → Firmware Update
```

## SD-Karte (FAT32, 32 GB empfohlen)

```
/books/        ← E-Books (TXT, EPUB)
/audiobooks/   ← MP3s (44100 Hz)
/alarms/       ← Wecker-MP3s
/web/wifi.cfg  ← WLAN-Zugangsdaten: Zeile 1 SSID, Zeile 2 Passwort
```

`wifi.cfg` lässt sich auch am Gerät anlegen: Settings → WiFi Setup
(Scan → SSID wählen → Passwort eingeben).

## Typischer Betrieb (Batterie-Fokus)

- **Lesen/Musik:** LoRa aus (Settings → LoRa Mesh: OFF), WiFi aus —
  beide Funkmodule schlafen, das E-Ink-Panel zieht zwischen den
  Seitenwechseln nichts.
- **Dateien verwalten:** Settings → OTA Tools → SD File Manager —
  verbindet sich ins WLAN, IP steht auf dem Display, im Browser öffnen.
  Beim Beenden geht WiFi wieder aus (LoRa wird nur reaktiviert, wenn der
  Toggle auf ON steht).
- **Mesh:** LoRa Mesh: ON — normale MeshCore-Funktion (Kanäle, DMs,
  Repeater). Radio-Preset „EU Default" für 868 MHz wählen.

## Upstream-Sync

```bash
git fetch upstream
git merge upstream/main   # eigene Änderungen sind flag-gekapselt und klein gehalten
pio run -e meck_audio_wifi_custom
```
