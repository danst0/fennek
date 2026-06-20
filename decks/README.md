# Karteikarten-Decks (Flashcards)

Beispiel-Decks für die **Karteikarten-App** (Launcher-Kachel „Lernen"). Die App
liest Decks von der SD-Karte aus dem Ordner **`/flashcards`**.

## Aufs Gerät bringen

Die Dateien aus diesem Ordner per **Web-Dateiverwaltung** (App „Dateien" → WLAN
starten → `http://fennek.local`) in den SD-Ordner `/flashcards` hochladen. Der
Ordner wird beim ersten Öffnen der App automatisch angelegt.

## Deck-Format

Eine Textdatei (`.txt` oder `.tsv`) = ein Deck. Der Dateiname (ohne Endung) ist
der Deck-Titel in der Liste. Pro Zeile eine Karte:

```
Vorderseite<TAB>Rückseite
```

- Trenner ist ein **Tabulator** (bevorzugt) oder ` | ` (Pipe) als Fallback.
- Zeilen, die mit `#` beginnen, und Leerzeilen sind Kommentare.
- Vorder-/Rückseite je bis ~95 Zeichen (länger wird gekappt).
- UTF-8 mit Umlauten (å, ä, ö …) ist ok — die Anzeige läuft über `gui::print`.

## Lernlogik (Leitner)

Jede Karte wandert durch fünf Boxen. „Gewusst" schiebt eine Box höher (längeres
Wiederholungs-Intervall: 1 / 3 / 7 / 16 Tage), „Falsch" zurück in Box 0. Eine
**Übung umfasst maximal 10 Karten**: die App sammelt die **fälligen** Karten,
**mischt** sie und nimmt bevorzugt die mit niedriger Box (also wenig Gelerntes —
bereits Gelerntes kommt seltener dran). Falsch beantwortete Karten kommen in
derselben Übung noch einmal dran. Der Fortschritt wird je Deck unter
`/flashcards/.progress/<deck>.prg` gespeichert (über einen CRC der Vorderseite
referenziert, übersteht also das Umsortieren oder Ergänzen des Decks).

> Hinweis: Das Gerät hat keine Hardware-Uhr. Ist die Systemzeit noch nicht
> gestellt (kein GPS/NTP/Mesh seit dem Kaltstart), gelten **alle** Karten als
> fällig — terminiert wird erst mit gestellter Uhr.

## Mitgelieferte Decks

- `schwedisch.txt` — komplettes Schwedisch-Deck (~700 Karten): Grundwortschatz,
  Zahlen/Zeit, Alltag, Reise, Verben, Adjektive sowie nützliche Sätze und
  Gesprächsführung — alles in einem Deck, gemischt abgefragt.
