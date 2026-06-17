# Offene Themen
- [ ] ct-Bücher löschen  (manuelle SD-Aktion am Gerät — kein Firmware-Feature;
      jetzt via Konsole `rm /books/<calibre-pfad>` möglich, auch Überlängenpfade)

# Verifikation am Gerät — neue Features 17.06. (v2.0.x)
- [x] Wecker headless: `time set`, `alarm 0 <hh:mm>`, Liste, geplantes Klingeln
      (Minuten-Match), `alarm test`/`stop` — alles am Gerät grün.
- [x] Wecker Deep-Sleep-Wake: Gerät aus dem Standby (`sleep`) zur Weckzeit von
      SELBST aufgewacht + geklingelt (17.06., sogar an USB). Kernmechanismus
      bestätigt (enterStandby→Timer auf Weckzeit, handleTimerWake→Vollboot, poll).
- [x] Wecker-Signal pro Wecker (Ton/Blinken/Beides): `mode blink` feuerte nur
      Backlight, kein Ton — am Gerät bestätigt. Piepton laut, Backlight blinkt.
- [x] Wecker-App-UI: Kachel (Launcher 2×5), Editor mit Ziffern-Eingabe der
      Weckzeit (Alt-Halten-Fix), Signal-Feld, „+9 min"-Schlummer-Button.
- [x] Mesh-Telemetrie: `pos` setzen/lesen/persistieren am Gerät bestätigt.
- [ ] Mesh-Telemetrie Empfang: am Web-Client (meshcore.dumke.me) prüfen, dass
      Position + Akku ankommen; `contacts` zeigt `@lat,lon` fremder Nodes. (offen)
- [x] Fennek-Bild Credit: geschwungener "by Dr. Daniel Dumke" am Schwanz war zu
      klein -> wieder entfernt (Original-Bild). Falls gewuenscht: groesser/anders.
      Tools dafuer liegen unter tools/sleepcredit.py + tools/sleepimg.py.
- [ ] Verifikation am Gerät (Build OK): Mesh-Identität von SD, Chats-Navigation,
      Reader-Refresh ohne Vollbild-Blitzen, Buch 2x öffnen = Cache-Hit.
- [ ] Verifikation am Gerät (Build OK, 12.06.): Schlafbild-Akku-% + Timer-Wake
      (mit -D SLEEP_WAKE_TEST flashen: 60 s statt 1 h; Achtung, Deep Sleep
      trennt USB -> Monitor neu verbinden) und Web-Dateiverwaltung
      (Konsole: wifi ssid/pass/start; Browser: Upload/Download/Loeschen,
      MP3-Upload danach abspielbar; wifi stop -> Mesh empfaengt wieder).

# Erledigt
- [x] Batterie-%-Anzeige auf dem Schlaf-Bildschirm, stuendliches Update:
      Banner zeigt % rechts unten; Wake-Quellen jetzt Knopf + Stunden-Timer.
      power::handleTimerWake() faehrt bei Timer-Wake einen Minimal-Pfad
      (kein SD/Audio/Apps), erneuert nur den Banner-Streifen (alle 12 Wakes
      Vollbild gegen Ghosting) und schlaeft sofort wieder.
- [x] Web interface für die Dateiverwaltung auf der SD: App "Dateien"
      (Kachel 7, Launcher jetzt 2x4) + services/webfm: WLAN-Station,
      WebServer (Bordmittel) mit eingebetteter HTML-Seite und JSON-API
      (/api/list|download|upload|delete|mkdir; nur /.fennek
      schreibgeschuetzt). Zugangsdaten in den Optionen (WLAN-SSID/-Passwort)
      oder via Serial-Konsole (wifi ssid/pass),
      Start in der App oder per "wifi start", Stop beim App-Verlassen.
      Waehrend WiFi: Audio gestoppt + Mesh-Pumpe pausiert. mDNS:
      http://fennek.local
- [x] Meshcore mit eigenem Priv/Pub-Key: laedt /meshcore/identity.hex von SD
      (Zeile1=Privkey 128 Hex, Zeile2=Pubkey 64 Hex). Vorrang vor SPIFFS,
      wird dorthin uebernommen. (mesh_client.cpp::begin)
- [x] Indizierung Bücher: war bereits geloest. Buchliste /.fennek/books.bin,
      Seiten-Index /.fennek/idx/<crc>.idx (Magic+Layout+Dateigroesse),
      EPUB->TXT /books/.epub_cache/. Wieder-Oeffnen nutzt Cache; nur Erst-Scan
      ist langsam (inhaerent, geteilter SPI-Bus).
- [x] Screen refresh zu oft: Reader-Fortschritt jetzt Region- statt Voll-Refresh;
      Reader-Liste kein Noop-Refresh am oberen Rand; Musik/Hörbuch erzwungener
      Anti-Ghosting-Full-Refresh von ~30 s auf ~60 s gestreckt.
- [x] Mesh-App anwenderfreundlicher: neue scrollbare "Chats"-Ansicht (alle Kanäle
      + DMs als Einträge), Auswahl öffnet die Konversation, Zurück fuehrt zur
      Liste. Kanäle pro Chat getrennt (MsgView.channelIdx).
