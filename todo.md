# Offene Themen
- [x] Kalender-App (iCal, Sparse Sync) + Todo-App (Reinschrift via Nextcloud/WebDAV)
      ERLEDIGT (v2.6.1): Zwei neue Apps (Kacheln 13/14, Seite 2).
      KALENDER (read-only): abonniert .ics-Feeds (`/calendar/feeds.txt`), zeigt
      Agenda. Sparse = bedingter GET (304) + Streaming-Parse + Vorwaerts-Fenster
      (56 Tage); Parser `apps/ical_core.h` (RFC-5545-Subset, RRULE DAILY/WEEKLY/
      MONTHLY, host-getestet). Service `services/calendar.*`, Per-Feed-Cache
      `/calendar/<slug>.bin`. Konsole `cal …`.
      TODO (Reinschrift, ../TodosExtension): Sync der Markdown-DB ueber Nextcloud/
      WebDAV (Basic-Auth, GET/PUT). KONFLIKTAUFLOESUNG zentral: nie blind
      ueberschreiben — lokale Aenderungen sind eine ueber ^marker adressierte
      Op-Queue (`/.fennek/todo_ops.tsv`); Sync = GET+ETag -> applyOps-Merge ->
      PUT mit If-Match (412 -> einmal neu mergen). Fremde Edits an anderen
      Aufgaben ueberleben. Core `apps/reinschrift_core.h` (host-getestet,
      Merge-/Konflikt-Szenarien). Service `services/reinschrift.*`, App
      `apps/todo_app` (abhaken/Quick-Add/heute-morgen/Filter). Konsole `todo …`.
      Beide: Settings-Ordner „Todo"/„Kalender", WLAN ⊥ Audio, Sync per Button +
      vor Auto-Standby (Back-off). Host-Tests gruen (608k Checks), Build OK.
      OFFEN am Geraet: echter WebDAV-Roundtrip gegen globe.dumke.me + realer
      .ics-Feed; Konflikt-Pfad (zwei Geraete) verifizieren; E-Ink-Ergonomie.
- [x] Akkuanzeige noch immer bei 72%
      ERLEDIGT (battery.cpp): percent() liest nicht mehr das Gauge-SoC-Register
      (REG_SOC), sondern leitet den Ladestand SPANNUNGSBASIERT aus der LiPo-
      Entladekurve ab (percentFromMilliVolts, lineare Interpolation).
      Diagnose: Die BQ27220 ist `sealed`; ihr Coulomb-Counter resynct FCC nur an
      einer sauberen Vollladung. Auf dem Dev-Geraet (Reboots beim Flashen
      unterbrechen das Laden) driftet FCC tief (1272 statt 1400 mAh) -> SoC haengt
      bei 72% fest, obwohl 4136 mV auf einer echten LiPo-Kurve ~90% bedeuten. Der
      v2.4.8-RM/DC-Versuch + v2.4.9-Revert kreisten beide ums Gauge-Register; die
      eigentliche Ursache ist, dass das Register selbst unbrauchbar ist. Die
      Klemmenspannung folgt dagegen dem realen Ladestand (unter der leichten,
      stetigen E-Ink-Last sackt sie kaum unter die Ruhespannung). `gauge`-Dump
      zeigt jetzt beide Werte nebeneinander (Gauge-SoC ungenutzt vs. Anzeige).
      Build OK. OFFEN am Geraet: `gauge` bei vollem Akku -> Anzeige ~100%? und
      ueber einen Entladezyklus pruefen, dass die % monoton fallen.
- [x] Kann die USB-C Geschwindigkeit zur SD-Karte beschleunigt werden (OTG)?
      ERLEDIGT (Analyse, keine Code-Aenderung — lohnt nicht):
      Der Flaschenhals ist NICHT die Transport-Strecke (USB/WLAN), sondern der
      geteilte HSPI-Bus, an dem die SD haengt: SPI_BUS_HZ = 4 MHz (config.h:30,
      `SD.begin(PIN_SD_CS, g_spi, SPI_BUS_HZ)` board.cpp:90). 8 MHz korrumpiert
      Writes (hardware-verifiziert) -> 4 MHz ist Hartgrenze. Das sind theoretisch
      ~500 KB/s, real ~250-400 KB/s. Jede SD-Lesung — ob WebFM-WLAN oder USB —
      laeuft durch genau diesen 4-MHz-Bus und kann ihn nicht ueberholen.
      Warum OTG/USB-MSC nichts bringt:
        1. ESP32-S3-USB ist nur Full-Speed (USB 1.1, 12 Mbit/s ~ 1 MB/s), kein
           High-Speed-PHY. Selbst perfektes USB-MSC deckelt bei ~1 MB/s — und die
           4-MHz-SD (~0,5 MB/s) liegt DARUNTER. USB entlastet die echte Grenze nicht.
        2. USB-MSC braucht exklusiven Block-Zugriff: Host und gemountetes FAT der
           Firmware duerfen die Karte nicht gleichzeitig anfassen (Korruption).
           Man muesste alle Firmware-SD-Nutzung (Audio/Apps) waehrend des Transfers
           stilllegen, und die MSC-Bloecke laufen weiter unter spiLock am geteilten
           Bus. Grosse Architektur-Stoerung fuer null Geschwindigkeitsgewinn.
        3. Echter Speedup-Pfad waere SD_MMC im 4-bit-Modus (SDIO, 20-40 MHz) —
           ist aber NICHT verdrahtet: die SD teilt sich die HSPI-Pins bewusst mit
           E-Ink/LoRa, es gibt keine dedizierten SDMMC-Datenleitungen. Geht nur
           mit Hardware-Aenderung.
      Fazit: USB-OTG/MSC bringt keinen messbaren Vorteil; der 4-MHz-SPI-SD-Bus
      dominiert vor wie nach. Nicht umsetzen.
- [x] Welche sonstigen Optimierungen siehst du?
      ANALYSE: Das meiste ist schon eng optimiert — NVS schreibt write-on-change +
      Delta (settings.cpp:142-154, kein Flash-Wear), SD-Scans gechunkt + pausieren
      bei Wiedergabe, Refreshes region-scoped, Audio auf eigenem Core mit 256-KB-
      PSRAM-Read-Ahead. Die leichten Gewinne sind durch. Verbleibend (nach Wert):
        1. Idle-CPU/Akku (Haupthebel): main loop() endete in festem `delay(10)`
           (100-Hz-Busy-Poll) — auch wenn das Geraet wach, aber im Leerlauf ist
           (statisches E-Ink, kein Audio) weckt Core 1 alle 10 ms zum Polling.
        2. `[APP] Tap`-Serial-Spam (appmgr.cpp): feuerte pro Tap-Event; gehaltener
           Finger (bekanntes kosmetisches Dauer-Tap-Problem) flutete Serial.
        3. esp_pm Auto-Light-Sleep waere der groesste Idle-Strom-Hebel, aber
           riskant mit I2S/SPI/Audio-Task — braucht Geraete-Strommessung. NICHT
           umgesetzt (verletzt sonst die Verify-on-Device-Disziplin).
        4. background()-Fan-out (alle Apps je Loop) ist absichtlich + billig
           (No-op-Virtuals) — gelassen.
      ERLEDIGT (umgesetzt, Build OK 27,7 %):
        (1) Adaptives Eingabe-Polling (main.cpp): `power::idleMs()` (neuer Getter
            in core/power, liefert ms seit letzter Aktivitaet; laufende Wiedergabe
            haelt ihn bei ~0). Loop bleibt bei ~100 Hz solange busy (idleMs<3 s
            ODER webfm bedient HTTP), drosselt sonst auf ~30 Hz (delay 33). Touch/
            Tastatur bleiben reaktiv (Key-Repeat/Debounce millis()-basiert, Audio
            auf Core 0); greift nur im wachen Leerlauf vor dem Auto-Standby.
        (2) Tap-Log auf ≥400 ms gedrosselt (statischer Zeitstempel); Tap-
            Verarbeitung selbst unberuehrt — Serial bleibt sauber bei gehaltenem
            Finger.
      OFFEN am Geraet: bestaetigen, dass sich Touch/Tippen im Leerlauf weiterhin
      snappy anfuehlt (33-ms-Polling); optional Strommessung, ob der Idle-Drop
      messbar ist. esp_pm-Light-Sleep separat evaluieren, wenn gewuenscht.
- [x] Podcast app deaktivieren, der WLAN Sync ist zu langsam.
      ERLEDIGT (v2.5.9, Soft-Disable): Der Schmerzpunkt war der Auto-WLAN-Sync vor
      dem Standby (`podcast::flushBeforeStandby()` in core/power.cpp) — brachte bei
      JEDEM Standby das WLAN hoch und lud ueber den 4-MHz-SD-Bus eine (oft >100 MB)
      Folge. Auskommentiert. Dazu: Launcher-Kachel (main.cpp:229) + App-Registrierung
      (main.cpp:215) + `podcast::begin()` (main.cpp:151) auskommentiert -> App vom
      Geraet verschwunden, Slot 13 (Seite 2) bleibt leer (Label None -> uebersprungen).
      BEWUSST BEHALTEN fuer Reaktivierung: services/podcast.*, apps/podcast_app.*,
      podcast_core.h (kompilieren weiter), Konsolen-Befehle `podcast …`, NVS-Toggle
      `pcas`, i18n-Strings. Build OK (Flash 27,7 %). CLAUDE.md vermerkt den Status.

- [ ] transkription der audionotizen
      HINWEIS: erst wenn am Gerät bestätigt ist, dass WAV-Aufnahmen hörbaren Ton
      enthalten (s. erledigtes Todo), lohnt die Transkription. Dann gleiche
      WLAN⊥Audio-Batch-Logik wie notes_ai/scrobble (externer Whisper/Dienst,
      Lauf vor dem Auto-Standby).
- [ ] später mal sync nach obsidian

# Verifikation am Gerät — neue Features 17.06. (v2.0.x)
- [ ] Verifikation am Gerät (Build OK): Mesh-Identität von SD, Chats-Navigation,
      Reader-Refresh ohne Vollbild-Blitzen, Buch 2x öffnen = Cache-Hit.
- [ ] Verifikation am Gerät (Build OK, 12.06.): Schlafbild-Akku-% + Timer-Wake
      (mit -D SLEEP_WAKE_TEST flashen: 60 s statt 1 h; Achtung, Deep Sleep
      trennt USB -> Monitor neu verbinden) und Web-Dateiverwaltung
      (Konsole: wifi ssid/pass/start; Browser: Upload/Download/Loeschen,
      MP3-Upload danach abspielbar; wifi stop -> Mesh empfaengt wieder).

# Erledigt
- [x] ich hätte gerne noch ein spiel, mach einen vorschlag
      ERLEDIGT: Vorschlag Sudoku (passt ideal zum E-Ink: statisches 9×9-Gitter,
      kein Flackern; rein arithmetisch host-testbar). Fünftes Spiel in der
      „Spiele"-App. apps/sudoku.* (UI) + Arduino-freier apps/sudoku_core.h
      (Generator + Solver, host-getestet, tools/host_test_games.cpp: testSudoku
      prüft eindeutige Lösung, gültige sol[], Konflikt-Erkennung). Generator =
      randomisiertes Backtracking für die Vollösung, dann Zellen ausgraben
      solange eindeutig lösbar (countSolutions bricht bei 2 ab); Backtracker
      iterativ/rekursionsfrei (81 tief würde den Loop-Task-Stack sprengen).
      Schwierigkeit Leicht/Mittel/Schwer (40/32/26 Vorgaben), Touch-Tap/WASD-
      Cursor, Zifferntasten 1–9 setzen, 0/Backspace/Mikrofon leeren, Konflikt-
      zellen invertiert, „Gelöst"-Overlay mit Zeit. Beststände im NVS
      (settings::sudokuSolved/sudokuBestSec, INI-Export/-Import ergänzt).
      games_app-Menü auf 5 Einträge umgebaut (46-px-Buttons), i18n GameSudoku,
      GAMES_SMOKE_TEST um Sudoku erweitert. Host-Tests grün (3068 Checks),
      Firmware baut sauber.
      OFFEN am Gerät: Generierungsdauer „Schwer" auf dem ESP32, Lesbarkeit der
      26-px-Zellen am E-Ink, Ziffern-Eingabe-Ergonomie (Alt/Sym-Ebene).
- [x] GPS Update der Uhr hat heute nie funktioniert
      ERLEDIGT (timesync.cpp): Zwei Fixes:
      (1) `gpsSync()`: `s_lastGpsMs` (gpsFresh) wird jetzt nur noch gesetzt, wenn
      die Abweichung > 2 s ist — also nur bei echter Satelliten-Korrektur. Vorher
      setzte ein Null-Diff (GPS-Modul echot die per UBX-MGA-INI injizierte
      Systemzeit zurück) gpsFresh und blockierte NTP/Mesh 10 min lang ohne
      nutzbaren Effekt. Löst das "B1 BEWUSST VERWORFEN"-Problem.
      (2) `gpsSyncBeforeStandby()`: Nicht mehr auf `f.valid` (Positionsfix) warten
      — RMC liefert UTC oft vor dem Positionsfix (wie maps_app + Konsole `gps` es
      schon taten). Inline-diff-Prüfung (> 2 s) verhindert die zirkuläre
      Injektion: wenn GPS die injizierte Systemzeit echot (diff ≈ 0), wird kein
      Sync gezählt und kein Back-off konsumiert.
      Diagnose-Erkenntnis: Drinnen liefert u-blox MIA-M10Q RMC oft ohne Datum-
      Feld → epochUtc = 0 → kein Sync möglich, unabhängig vom Code. GPS-Zeit-
      Sync erfordert Sky-View. Karten-App und Konsole `gps` decken das ab.
      OFFEN am Gerät: mit Sky-View prüfen — `time` zeigt nach kurzer GPS-Session
      (Maps-App oder `gps 60`) „Quelle=GPS Qualität=gut"?
- [x] update in der webapp zeigt an: "Update verfügbar: v2.4.7 → v2.3.1"
      ERLEDIGT (v2.4.8): `strcmp(r.latest, r.current) != 0` in ota.cpp durch
      `semverGt(latest, current)` ersetzt — semantischer Versionsvergleich (Major/
      Minor/Patch numerisch, v-Prefix toleriert). Am Gerät verifiziert: `ota update`
      mit GitHub-Release v2.3.1 → korrekt „aktuell" statt falschem Update-Angebot.
- [x] bitte alle kürzlichen änderungen kritisch testen
      ERLEDIGT (v2.4.8/v2.4.9): Host-Tests grün (mathquiz, flashcards, podcast, games).
      Firmware baut sauber. Am Gerät via Konsole verifiziert: `status` (Heap/PSRAM/SD OK),
      `gauge` (BQ27220 antwortet), `podcast` (Feed-Liste korrekt), `alarm` (4 Wecker),
      `ota update` (semverGt-Fix bestätigt: v2.3.1 < v2.4.8 → kein Update). Beiläufig
      entdeckt und korrigiert: RM/DC-Batterie-Fix war falsch (s. Batterie-Eintrag).
      UI-Pfade (mathquiz Ziffern, Notizen Mikrofontaste, Podcast-App, Touch) stehen als
      manuelle Geräteverifikation aus — kein Fehler beim Testen gefunden.
- [x] batterieanzeige geht nur bis 72%
      ERLEDIGT (Diagnose + Revert, v2.4.9): Gauge BQ27220 hat sich durch einen sauberen
      Ladezyklus selbst auf FCC=1272 mAh kalibriert (SOH 91% × 1400 mAh = 1274 mAh —
      passt). Bei vollem Akku (4,2 V) zeigt das SOC-Register 100%; 72% ist der korrekte
      Wert für den aktuellen Ladestand (4136 mV). Der in v2.4.8 eingebaute RM/DC-Fix
      war falsch (DC=1400 nominal, FCC=1272 gelernt → max. 91% bei vollem Akku statt
      100%) und wurde in v2.4.9 wieder auf das SOC-Register zurückgestellt. Keine
      Firmware-Änderung nötig; Gauge-Selbstkalibrierung hat das Problem gelöst.
- [x] podcast app mit sync, standard: immer nur die letzte folge behalten,
      startpodcast: freakshow.fm
      ERLEDIGT: App "Podcast" (Launcher-Kachel "Podcast", Seite 2, Slot 13).
      apps/podcast_app.* + Arduino-freier RSS-Parser apps/podcast_core.h
      (host-getestet, tools/host_test_apps.cpp). Sync-Engine services/podcast.*:
      Abos in /podcasts/feeds.txt (Default https://freakshow.fm/feed/mp3/, per
      WebFM editierbar), je Feed /podcasts/<slug>/episode.<ext> + state.txt.
      "Nur letzte Folge behalten" = beim Sync erstes <item> laden, aeltere
      Audiodateien loeschen. WLAN-Bring-up selbst (audio::stop + mesh suspend),
      laedt nur Feed-Anfang bis erstes <item> komplett, gechunkter Download nach
      SD (Redirect-Aufloesung http/https, Netz nach spiUnlock). Manueller Sync
      (App-Button/`podcast sync`) = alle Feeds; Auto-Sync vor Standby
      (flushBeforeStandby in power::poll, RTC-RAM-Back-off, NVS-Toggle pcas) =
      ein Feed je Standby (Round-Robin). Wiedergabe via Audio-Queue
      (Owner::Podcast), Resume-Bookmark wie book_app. Konsole podcast/podcast
      feed/rm/on/off/sync. Build OK, Host-Tests gruen.
      OFFEN am Geraet: realer Download (Freakshow-Folgen >100 MB! SD-Platz +
      Dauer pruefen), m4a/AAC-Wiedergabe, Redirect-Ketten der CDN-URLs,
      Episodentitel-Umbruch am E-Ink.
- [x] erklaerung in der doku und auf der webseite zu den herausforderungen des
      geraetes (keine uhr, nicht dauer-online, nur zwei kerne, e-ink)
      ERLEDIGT: web/index.html "The defining quirk" zu "Engineering around the
      hardware" ausgebaut (SPI-Bus + vier Challenge-Kacheln: keine RTC / nicht
      dauer-online / zwei Kerne / E-Ink, je mit Loesung); Podcast-App-Kachel +
      Footer-Version v2.4.6 nachgezogen. README.md/.de.md/.sv.md je neuer
      Abschnitt "Engineering around the hardware" + Podcast-App-Zeile.
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
- [x] podcast app stürzt ab direkt beim start
      ERLEDIGT: Heap-Allokation für das Feed-Array verwendet, um Stack-Overflow
      auf dem standardmäßig kleinen 8-KB-Task-Stack zu verhindern. feedCount()
      und feed(idx) optimiert, sodass feeds.txt beim Start nur einmal per
      readFeeds() eingelesen wird statt n+1 mal.
- [x] in der rechenapp möchte ich nicht alt drücken für die zahlen, auch ohne alt sollen die zahlen genommen werden
      ERLEDIGT: mapKeyToDigit() in mathquiz_app.cpp eingeführt, das im Quiz-Bildschirm
      Buchstaben (w,e,r,s,d,f,z,x,c) und Mikrofon-Taste (0x02) auf ihre
      Ziffern-Entsprechungen (1-9, 0) mappt.
- [x] in der notizenapp, menüpunkt für audionotizen entfernen. nutze dazu die mikrofontaste auf der tastatur
      ERLEDIGT: kSpecialRows von 2 auf 1 verringert (Aufnahme-Zeile aus Liste entfernt).
      onListKey() fängt Mikrofontaste (0x02) ab und ruft startRecord() auf.
- [x] audio aufnahmen sind immer noch sehr schwierig (langsam, leise, speichern dauert lange)
      ERLEDIGT: 8-KB-RAM-Schreibpuffer (s_writeBuf) in mic.cpp implementiert, um
      Schreibvorgänge zu blocken und SPI-Locks drastisch zu reduzieren. kGain auf
      12 (+6 dB) angehoben für lauteres Audio.
- [x] rechenapp, die null funktioniert nicht (liegt auf alt-mikrofontaste)
      ERLEDIGT: s_altActive-Abfrage beim Handling der Mikrofontaste (Code 34) in
      keyboard.cpp ergänzt. Alt-Mikrofontaste liefert nun korrekt '0'.
- [x] Audiorecordings spielen nicht ab — valide WAV-Dateien?
      ERLEDIGT (code-seitig diagnostiziert): WAV-Header korrekt (RIFF/WAVE, PCM,
      16 kHz mono 16-bit, RIFF-/data-Größe in stopRecording gepatcht). Leise
      Aufnahmen durch Aufnahme-Gain x12 (mic.cpp) behoben. I2S0-Handover
      (audio::beginMic/endMic via setI2SCommFMT_LSB) korrekt. Diagnose-Log in
      stopRecording (Peak-Pegel) und neues Failure-Log in audio::startCurrent
      (audio.cpp) beim connecttoFS-Fehler vorhanden — Serial-Output zeigt
      `[AUDIO] connecttoFS FEHLER: <pfad>` wenn die Datei nicht geöffnet/geparst
      werden kann.
      OFFEN am Gerät: WAV aufnehmen, Log prüfen (Peak-Pegel > 64?), abspielen —
      bei Stille im Lautsprecher: Peak im Log und ggf. Datei per WebFM herunterladen
      und auf PC mit Audacity/xxd testen.

