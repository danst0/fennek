# Offene Themen
- [ ] podcast app mit sync, standard: immer nur die letzte folge behalten, startpodcast: freakshow.fm
- [ ] erklarung in der doku und auf der webseite zu den herausforderungen des gerates: keine uhr, strom fur immer online ist nicht moglich , nur zwei kerne also kaum multitasking und wie wir das gelost haben, eink screen

- [ ] später mal sync nach obsidian
- [ ] transkription der audionotizen
      HINWEIS: zuerst die WAV-Validität klären (Todo "Audiorecordings spielen
      nicht ab") — erst wenn die Aufnahmen gültigen Ton enthalten, lohnt die
      Transkription. Dann gleiche WLAN⊥Audio-Batch-Logik wie notes_ai/scrobble
      (externer Whisper/Dienst, Lauf vor dem Auto-Standby).
- [ ] GPS Update der Uhr hat heute nie funktioniert
      STATUS: Diagnose-Log eingebaut (maps_app: Einmal-Log „[MAPS] GPS-Zeit
      empfangen …" sobald RMC in einer Karten-Session Datum+Zeit liefert; dazu
      bestehendes „[TIME] GPS-Sync: … Korrektur"). B1 (Pre-Standby-Sync ohne
      Positions-Fix) BEWUSST VERWORFEN: `gps::begin()` injiziert per UBX-MGA-INI
      die bekannte Systemzeit — vor echtem Fix echot RMC diese zurueck, ein Sync
      darauf waere zirkulaer und wuerde via `gpsFresh()` ~10 min NTP/Mesh
      blockieren (s. timesync.cpp:288). Offen am Geraet: Konsole `gps 60` mit
      Sky-View → kommt RMC-Zeit? `time` zeigt Quelle=GPS? (Hardware/Sky-View.)
      Diagnose: `timesync::gpsSync()` wird produktiv NUR aus `maps_app::tick()`
      gerufen (maps_app.cpp:179) — also nur, solange die Karten-App im
      Vordergrund ist (onEnter=`gps::begin()`, onLeave=`gps::end()`). Bedingung
      zusätzlich `f.epochUtc > 1700000000UL`, d.h. RMC muss Datum+Zeit liefern.
      Drinnen liefert RMC oft keinen gültigen Zeitstempel → kein Sync. Der
      einzige Hintergrund-Pfad ist `gpsSyncBeforeStandby()` (timesync.cpp:261),
      der aber einen *gültigen Positions-Fix* verlangt und Back-off hat.
      Plan:
        1. Headless eingrenzen: Konsole `gps 60` mit Sky-View — kommt heute
           überhaupt RMC mit Datum+Zeit? `time` danach zeigt Quelle=GPS?
        2. Falls GPS-Zeit nur in der Karten-App ankommt: entscheiden, ob der
           Zeit-Sync breiter laufen soll (z.B. ein leichter GPS-Poll-Pfad
           unabhängig von der App) oder ob „Karten-App offen“ dokumentiert wird.
        3. Prüfen, ob der `epochUtc`-Guard / die `gpsSync`-Drossel (>2 s) den
           Sync verschluckt; ggf. einmaligen Force-Sync nach `begin()` loggen.
- [ ] Audiorecordings spielen nicht ab — valide WAV-Dateien?
      STATUS: Diagnose-Log in `mic::stopRecording()` eingebaut — loggt Bytes,
      Sekunden und Peak-Pegel; bei Peak<64 trotz Bytes „-> STUMM?" (= PDM-Mic
      liefert Stille, nicht Wiedergabe-Bug). Offen am Geraet: aufnehmen, Log
      lesen + Datei per WebFM ziehen und am PC pruefen (C2/C3).
      Diagnose: WAV-Header in mic.cpp sieht korrekt aus (RIFF/WAVE, PCM, 16 kHz
      mono 16-bit; RIFF- und data-Größe werden beim Stop gepatcht, mic.cpp:
      116-120). Wiedergabe läuft über die normale Audio-Queue (notes_app.cpp:
      353, Owner Music → ESP32-audioI2S `connecttoFS`). Offen ist, ob die Datei
      *gültig aber stumm* (PDM-Mic liefert nichts) oder *defekt* oder der
      *Wiedergabepfad* das Problem ist.
      Plan:
        1. Eine `/notes/*.wav` per WebFM herunterladen, am PC Header prüfen
           (`xxd`/Audacity) und abspielen → trennt „ungültige Datei“ vs „stille
           Aufnahme“ vs „nur Geräte-Wiedergabe-Bug“.
        2. Wenn am PC hörbar & gültig: Geräte-Wiedergabe debuggen (Owner-Token,
           I2S0-Restore nach `endMic()`, Sample-Rate-Umschaltung der Lib).
        3. Wenn am PC stumm: PDM-Mic-Pinconfig/Daten prüfen (mic.cpp:55-77,
           PIN_MIC_CLK/DATA, `i2s_read`-Rückgabe, Pegel).

# Verifikation am Gerät — neue Features 17.06. (v2.0.x)
- [ ] Verifikation am Gerät (Build OK): Mesh-Identität von SD, Chats-Navigation,
      Reader-Refresh ohne Vollbild-Blitzen, Buch 2x öffnen = Cache-Hit.
- [ ] Verifikation am Gerät (Build OK, 12.06.): Schlafbild-Akku-% + Timer-Wake
      (mit -D SLEEP_WAKE_TEST flashen: 60 s statt 1 h; Achtung, Deep Sleep
      trennt USB -> Monitor neu verbinden) und Web-Dateiverwaltung
      (Konsole: wifi ssid/pass/start; Browser: Upload/Download/Loeschen,
      MP3-Upload danach abspielbar; wifi stop -> Mesh empfaengt wieder).

# Erledigt
- [x] layout rechenapp, die aufgaben passen nicht ganz hin
      ERLEDIGT: quizMid() in mathquiz_app.cpp auf Auto-Fit umgestellt — neuer
      fitSize-Helfer waehlt die groesste Schrift (3->2->1), deren GEMESSENE
      Pixelbreite in 228 px passt (Aufgabe + Antwort-Eingabe). Aufgabe/Antwort/
      Feedback als Block vertikal mittig im 108-px-Streifen statt fixer Offsets
      -> kein Ueberlauf/Ueberlapp bei langen CFO-Aufgaben. Nebenbei latenten
      Zentrier-Bug behoben (textBounds maß bei Header-Groesse 2 statt qsize).
      Build OK. OFFEN: lange CFO-Aufgaben am Geraet sichten.
- [x] karteikarten deutlich ausbauen (500x Schwedisch), gemischt + Wiederholung,
      eine Uebung = 10 Karten
      ERLEDIGT: flashcards_core.h um shuffle (Fisher-Yates) + stableSortByBox +
      kSessionLimit=10 erweitert (host-getestet). buildQueue() in
      flashcards_app.cpp: faellige sammeln -> mischen -> nach Box aufsteigend
      (wenig Gelerntes zuerst, "bereits gelernte seltener") -> auf 10 kappen
      (esp_random, kein rand()). Falsch-Wiederholen bleibt in der Sitzung.
      Inhalt: decks/schwedisch.txt (703 Karten, 3 Alt-Decks zusammengefuehrt +
      erweitert, 0 Dubletten/ungueltige Zeilen), README angepasst. Build OK.
      OFFEN: am Geraet 10er-Uebung + Mischung pruefen, Deck per WebFM hochladen.
- [x] ich haette gerne noch eine app fuer kopfrechnen uben
      ERLEDIGT: App "Kopfrechnen" (Launcher-Kachel "Rechnen", Slot 11).
      apps/mathquiz_app.* + Arduino-freier Core apps/mathquiz_core.h
      (host-getestet, tools/host_test_apps.cpp). Modi Plus/Minus/Mal/Geteilt/
      Gemischt x 3 Stufen; Subtraktion nie negativ, Division geht glatt auf
      (reine Ziffern-Eingabe). Bestserie im NVS (settings::mathBestStreak).
      Build OK. OFFEN: am Gerät bedienen (Ziffern-Eingabe, Region-Refresh).
- [x] eine karteikarten app
      ERLEDIGT: App "Karteikarten" (Launcher-Kachel "Lernen", Slot 12).
      apps/flashcards_app.* + Core apps/flashcards_core.h (host-getestet).
      Decks = Textdateien /flashcards/*.txt (vorn<TAB>hinten), Leitner-Boxen
      0..4, Fortschritt je Deck nach /flashcards/.progress/<deck>.prg
      (CRC-keyed). SD-Disziplin wie notes_app. Build OK.
      OFFEN: am Gerät — Decks per WebFM nach /flashcards hochladen, lernen,
      Fortschritt-Persistenz über Reboot prüfen; Umlaut-Umbruch am E-Ink.
- [x] karteikarten für swedisch vokabeln und sätze sowie für gesprächsführung/Kommunikation
      ERLEDIGT (Inhalt): decks/schwedisch-vokabeln.txt (37),
      decks/schwedisch-saetze.txt (20), decks/schwedisch-konversation.txt (15)
      im Repo + decks/README.md (Format + WebFM-Upload nach /flashcards).
      Alle Karten parsen 100 % (Host-Check). OFFEN: per WebFM aufs Gerät.
- [x] ct-Bücher löschen  (manuelle SD-Aktion am Gerät — kein Firmware-Feature;
      jetzt via Konsole `rm /books/<calibre-pfad>` möglich, auch Überlängenpfade)
- [x] GPS und Kartenansicht: ohne Fix Fokus auf Düsseldorf
      ERLEDIGT (Commit 70e20de9): FALLBACK_LAT/LON = 51.2277/6.7735 (Düsseldorf)
      in maps_app.cpp:31-32.
      Diagnose: Fallback-Zentrum war Dortmund (51.4818/7.2162) in
      maps_app.cpp:28-30 (`FALLBACK_LAT/LON`); ohne Fix/Mesh-Pos bleibt die
      Karte dort. NRW-Kacheln z6-13 sind vorhanden (decken Düsseldorf ab),
      Dortmund-Detail z14-16 nicht über Düsseldorf.
      Plan:
        1. `FALLBACK_LAT=51.2277`, `FALLBACK_LON=6.7735` (Düsseldorf) setzen,
           Kommentar anpassen.
        2. Prüfen, dass `scanZooms()` für Düsseldorf eine Stufe findet und
           `clampZoom()` `DEFAULT_ZOOM` auf eine vorhandene Stufe klemmt
           (sonst leeres Punktraster). Ggf. DEFAULT_ZOOM auf z13 senken.
        3. Am Gerät verifizieren: Karten-App ohne Fix zentriert auf Düsseldorf,
           Kacheln rendern.
- [x] Benamung der Audio-Recordings: Datum + HH:MM
      ERLEDIGT (Commit 8370a7ba): scanNotes() leitet den Titel aus dem
      WAV-Dateinamen als "YYYY-MM-DD HH:MM" ab (notes_app.cpp:196ff).
      Diagnose: Datei heißt bereits `YYYY-MM-DD-HHMMSS.wav` (notes_app.cpp:319),
      aber die Liste zeigt für *jede* Aufnahme nur „Sprachnotiz“ (notes_app.cpp:
      196) — Datum/Uhrzeit sind nirgends sichtbar. Das ist vermutlich gemeint.
      Plan:
        1. In `scanNotes()` für WAV den Titel aus dem Dateinamen ableiten und
           als „YYYY-MM-DD HH:MM“ anzeigen (statt fixem „Sprachnotiz“).
        2. Optional: Dateiname-Format selbst auf Minuten kürzen, aber Sekunden
           als Kollisionsschutz bei mehreren Aufnahmen/Minute behalten →
           Empfehlung: Dateiname mit Sekunden lassen, nur Anzeige HH:MM.
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
- [x] Mesh-GPS-Empfang END-TO-END (17.06.): Fennek „Daniel" sendet `advert flood`
      mit Position 51.4818/7.2162 → über Relay/Bridge zum Server meshcore.dumke.me
      → Companion „Antonia" sieht den Node mit korrektem GPS-Pin (~2 min Latenz).
      Wichtig: nur Flood propagiert (Zero-Hop erreicht die 1-Hop-Bridge nicht).
      Akku-Telemetrie liegt in den FEAT-Feldern (nicht-Standard) — der Server
      dekodiert daraus keine Spannung (nur Position ist Standard-Advert-Feld).
- [x] Fennek-Bild Credit: geschwungener "by Dr. Daniel Dumke" am Schwanz war zu
      klein -> wieder entfernt (Original-Bild). Falls gewuenscht: groesser/anders.
      Tools dafuer liegen unter tools/sleepcredit.py + tools/sleepimg.py.
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
