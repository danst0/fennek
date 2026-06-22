// =============================================================================
// host_test_apps.cpp — Host-Tests für die Arduino-freien App-Cores
// (mathquiz_core, flashcards_core, podcast_core, reinschrift_core, ical_core).
// Nicht Teil des Firmware-Builds.
//
//   g++ -std=c++17 -O2 -I src tools/host_test_apps.cpp -o /tmp/apps_test && /tmp/apps_test
//
// Präzedenz: tools/host_test_games.cpp testet die Spiele-Cores genauso.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "apps/mathquiz_core.h"
#include "apps/flashcards_core.h"
#include "apps/podcast_core.h"
#include "apps/reinschrift_core.h"
#include "apps/ical_core.h"

// --- Deterministischer Zufall (LCG) ------------------------------------------
static uint32_t s_seed = 2025;
static uint32_t testRnd(uint32_t n) {
  s_seed = s_seed * 1664525u + 1013904223u;
  return n ? (s_seed >> 16) % n : 0;
}

static int s_checks = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
  s_checks++; \
} while (0)

// =============================================================================
// mathquiz
// =============================================================================
static int testMathquiz() {
  using namespace mathquiz;
  // Grundrechenarten x jede Stufe oft genug erzeugen und Invarianten prüfen.
  for (int op = ADD; op <= DIV; op++) {
    for (uint8_t lvl = 0; lvl < 3; lvl++) {
      for (int i = 0; i < 5000; i++) {
        Problem p = generate((Op)op, lvl, testRnd);
        CHECK(p.op == op);
        switch (op) {
          case ADD: CHECK(p.answer == p.a + p.b); break;
          case SUB:
            CHECK(p.b <= p.a);          // nie negativ
            CHECK(p.answer == p.a - p.b);
            CHECK(p.answer >= 0);
            break;
          case MUL: CHECK(p.answer == p.a * p.b); break;
          case DIV:
            CHECK(p.b >= 2);            // Divisor sinnvoll
            CHECK(p.a % p.b == 0);      // geht glatt auf
            CHECK(p.answer == p.a / p.b);
            CHECK(p.answer >= 1);
            break;
        }
      }
    }
  }

  // Geld-Ops (PCT/MARKUP/DISCOUNT): Grundbetrag a durch 100 teilbar.
  for (int op = PCT; op <= DISCOUNT; op++) {
    for (uint8_t lvl = 0; lvl < 3; lvl++) {
      for (int i = 0; i < 5000; i++) {
        Problem p = generate((Op)op, lvl, testRnd);
        CHECK(p.op == op);
        CHECK(p.a % 100 == 0);              // Grundbetrag = Vielfaches von 100
        CHECK(p.b >= 1 && p.b <= 99);       // sinnvoller Prozentsatz
        int32_t part = p.a / 100 * p.b;     // exakt, da a % 100 == 0
        if (op == PCT)         CHECK(p.answer == part);
        else if (op == MARKUP) CHECK(p.answer == p.a + part);
        else                   CHECK(p.answer == p.a - part);
        CHECK(p.answer >= 0);               // Rabatt bleibt nicht-negativ
      }
    }
  }

  // CFO SHARE/GROWTH: Basis durch 100 teilbar, Prozent exakt ganzzahlig.
  for (int op = SHARE; op <= GROWTH; op++) {
    for (uint8_t lvl = 0; lvl < 3; lvl++) {
      for (int i = 0; i < 5000; i++) {
        Problem p = generate((Op)op, lvl, testRnd);
        CHECK(p.op == op);
        CHECK(p.answer >= 1 && p.answer <= 99);
        if (op == SHARE) {
          // a = Anteil, b = Basis (Vielfaches von 100); answer = a*100/b exakt.
          CHECK(p.b % 100 == 0);
          CHECK((long)p.a * 100 % p.b == 0);
          CHECK(p.answer == (long)p.a * 100 / p.b);
          CHECK(p.a <= p.b);
        } else {  // GROWTH: a Basis, answer = Wachstum%, end = a + a/100*answer
          CHECK(p.a % 100 == 0);
          CHECK(p.answer == p.b);
        }
      }
    }
  }

  // CFO RULE72: Verdopplungszeit = 72/Zins, nur Teiler von 72.
  for (uint8_t lvl = 0; lvl < 3; lvl++) {
    for (int i = 0; i < 3000; i++) {
      Problem p = generate(RULE72, lvl, testRnd);
      CHECK(p.op == RULE72);
      CHECK(p.b >= 2 && 72 % p.b == 0);
      CHECK(p.answer == 72 / p.b);
      CHECK(p.answer >= 1);
    }
  }
  // pickOp respektiert die Maske.
  for (int i = 0; i < 1000; i++) {
    Op o = pickOp(opBit(MUL) | opBit(DIV), testRnd);
    CHECK(o == MUL || o == DIV);
  }
  CHECK(pickOp(opBit(SUB), testRnd) == SUB);
  CHECK(pickOp(0, testRnd) == ADD);   // leere Maske -> Fallback
  for (int i = 0; i < 1000; i++) {    // Finanz-Maske
    Op o = pickOp(opBit(PCT) | opBit(MARKUP) | opBit(DISCOUNT), testRnd);
    CHECK(o == PCT || o == MARKUP || o == DISCOUNT);
  }
  CHECK(opChar(ADD) == '+' && opChar(SUB) == '-');
  CHECK(opChar(MUL) == 'x' && opChar(DIV) == ':');
  CHECK(opChar(PCT) == '%' && opChar(MARKUP) == '%' && opChar(DISCOUNT) == '%');
  CHECK(opChar(SHARE) == '%' && opChar(GROWTH) == '%' && opChar(RULE72) == '%');
  printf("  mathquiz ok\n");
  return 0;
}

// =============================================================================
// flashcards
// =============================================================================
static int testFlashcards() {
  using namespace flashcards;
  Card c;

  // TAB-Trenner.
  CHECK(parseLine("hund\tder Hund", &c));
  CHECK(strcmp(c.front, "hund") == 0);
  CHECK(strcmp(c.back, "der Hund") == 0);

  // Pipe-Trenner + Trimmen.
  CHECK(parseLine("  katt   |   die Katze  ", &c));
  CHECK(strcmp(c.front, "katt") == 0);
  CHECK(strcmp(c.back, "die Katze") == 0);

  // Mehrere Worte je Seite, TAB hat Vorrang vor späterem '|'.
  CHECK(parseLine("god morgon\tguten Morgen | wörtlich", &c));
  CHECK(strcmp(c.front, "god morgon") == 0);
  CHECK(strcmp(c.back, "guten Morgen | wörtlich") == 0);

  // Kommentare / leer / kein Trenner / leere Seite -> abgelehnt.
  CHECK(!parseLine("# Schwedisch Vokabeln", &c));
  CHECK(!parseLine("", &c));
  CHECK(!parseLine("   ", &c));
  CHECK(!parseLine("ohne trenner", &c));
  CHECK(!parseLine("\tnur rueckseite", &c));
  CHECK(!parseLine("nur vorderseite\t", &c));

  // Folgezeile darf nicht hereinrutschen (eol-Grenze).
  CHECK(parseLine("a\tb\n# c | d", &c));
  CHECK(strcmp(c.front, "a") == 0);
  CHECK(strcmp(c.back, "b") == 0);

  // Leitner: richtig schiebt hoch (max 4), falsch zurück auf 0.
  CHECK(nextBox(0, true) == 1);
  CHECK(nextBox(3, true) == 4);
  CHECK(nextBox(4, true) == 4);       // Decke
  CHECK(nextBox(4, false) == 0);
  CHECK(nextBox(2, false) == 0);

  // Intervalle monoton steigend, Box 0 sofort fällig.
  CHECK(boxInterval(0) == 0);
  CHECK(boxInterval(1) < boxInterval(2));
  CHECK(boxInterval(2) < boxInterval(3));
  CHECK(boxInterval(3) < boxInterval(4));

  // Fälligkeit.
  uint32_t now = 1700000000u;
  CHECK(isDue(0, 0, now));                          // neue Karte
  CHECK(isDue(0, now - 10, now));                   // Box 0: sofort wieder
  CHECK(!isDue(1, now - 100, now));                 // Box 1: noch nicht
  CHECK(isDue(1, now - 2 * 86400u, now));           // Box 1: nach 2 Tagen fällig
  CHECK(!isDue(4, now - 10 * 86400u, now));         // Box 4: 16 Tage nötig
  CHECK(isDue(4, now - 20 * 86400u, now));

  // cardKey stabil + verschieden für verschiedene Fronten.
  CHECK(cardKey("hund") == cardKey("hund"));
  CHECK(cardKey("hund") != cardKey("katt"));

  // shuffle: Permutation erhält das Multiset (kein Index verloren/dupliziert).
  int idx[12];
  for (int i = 0; i < 12; i++) idx[i] = i;
  shuffle(idx, 12, testRnd);
  int seen[12] = {0};
  for (int i = 0; i < 12; i++) { CHECK(idx[i] >= 0 && idx[i] < 12); seen[idx[i]]++; }
  for (int i = 0; i < 12; i++) CHECK(seen[i] == 1);
  // ... und ändert die Reihenfolge tatsächlich (deterministischer Seed).
  int ident = 1;
  for (int i = 0; i < 12; i++) if (idx[i] != i) ident = 0;
  CHECK(!ident);

  // stableSortByBox: Box aufsteigend + stabil innerhalb gleicher Box.
  // Index->Box: gleiche Box (0) bei 1 und 4 — 1 muss vor 4 bleiben.
  int j2[6] = {0, 1, 2, 3, 4, 5};
  uint8_t boxes[6] = {2, 0, 2, 1, 0, 1};
  stableSortByBox(j2, 6, [&](int i) { return boxes[i]; });
  for (int i = 1; i < 6; i++) CHECK(boxes[j2[i - 1]] <= boxes[j2[i]]);   // monoton
  // Erste zwei Einträge sind die Box-0-Karten in Eingangsreihenfolge (1, dann 4).
  CHECK(j2[0] == 1 && j2[1] == 4);

  printf("  flashcards ok\n");
  return 0;
}

// =============================================================================
// podcast
// =============================================================================
static int testPodcast() {
  using namespace podcast;

  // Typischer Podcast-Feed (neueste Folge zuerst), mit CDATA + Entities +
  // großem Beschreibungstext NACH dem enclosure (wie bei Freakshow).
  const char* feed =
    "<?xml version=\"1.0\"?><rss><channel><title>Freakshow</title>"
    "<item>"
    "<title>FS280 Tofu &amp; Trüffel</title>"
    "<guid isPermaLink=\"false\">freakshow-280</guid>"
    "<enclosure url=\"https://cdn.example.com/fs/fs280.mp3?t=1\" length=\"123456789\" type=\"audio/mpeg\"/>"
    "<description><![CDATA[ Sehr langer Text mit <b>HTML</b> ... ]]></description>"
    "</item>"
    "<item>"
    "<title>FS279 Alt</title>"
    "<guid>freakshow-279</guid>"
    "<enclosure url=\"https://cdn.example.com/fs/fs279.mp3\" length=\"111\" type=\"audio/mpeg\"/>"
    "</item>"
    "</channel></rss>";

  EpisodeMeta m = parseLatest(feed);
  CHECK(m.valid);
  CHECK(isComplete(m));
  CHECK(strcmp(m.title, "FS280 Tofu & Trüffel") == 0);   // &amp; dekodiert
  CHECK(strcmp(m.guid, "freakshow-280") == 0);            // erstes item, nicht 279
  CHECK(strcmp(m.url, "https://cdn.example.com/fs/fs280.mp3?t=1") == 0);
  CHECK(m.lengthBytes == 123456789UL);

  // CDATA-Titel + Attribut-Reihenfolge vertauscht + &amp; in der URL.
  const char* feed2 =
    "<rss><channel><item>"
    "<enclosure type=\"audio/mpeg\" length=\"42\" url=\"http://h.fm/a.mp3?x=1&amp;y=2\"/>"
    "<title><![CDATA[Hallo Welt]]></title>"
    "<guid>g1</guid>"
    "</item></channel></rss>";
  EpisodeMeta m2 = parseLatest(feed2);
  CHECK(m2.valid);
  CHECK(strcmp(m2.title, "Hallo Welt") == 0);
  CHECK(strcmp(m2.url, "http://h.fm/a.mp3?x=1&y=2") == 0);   // &amp; -> &
  CHECK(m2.lengthBytes == 42UL);

  // Kein <guid> → Fallback = URL.
  const char* feed3 =
    "<rss><channel><item><title>T</title>"
    "<enclosure url=\"http://h.fm/b.mp3\"/></item></channel></rss>";
  EpisodeMeta m3 = parseLatest(feed3);
  CHECK(m3.valid);
  CHECK(strcmp(m3.guid, "http://h.fm/b.mp3") == 0);

  // Kein <item> / kein <enclosure> → ungültig.
  CHECK(!parseLatest("<rss><channel><title>x</title></channel></rss>").valid);
  CHECK(!parseLatest("<rss><channel><item><title>x</title></item></channel></rss>").valid);

  // Unvollständiger Puffer (Beschreibung abgeschnitten), enclosure aber schon da:
  // isComplete bleibt true → der Stream-Loader darf abbrechen.
  const char* partial =
    "<rss><channel><item><title>Teil</title><guid>g</guid>"
    "<enclosure url=\"http://h.fm/c.mp3\" length=\"5\"/>"
    "<description><![CDATA[ angeschnitten ...";
  CHECK(isComplete(parseLatest(partial)));

  // feedSlug: lesbarer Host-Präfix, deterministisch, kollisionsfrei pro URL.
  char s1[40], s2[40], s3[40];
  feedSlug("https://freakshow.fm/feed/mp3/", s1, sizeof(s1));
  feedSlug("https://freakshow.fm/feed/m4a/", s2, sizeof(s2));
  feedSlug("https://freakshow.fm/feed/mp3/", s3, sizeof(s3));
  CHECK(strncmp(s1, "freakshowfm-", 12) == 0);
  CHECK(strcmp(s1, s3) == 0);    // deterministisch
  CHECK(strcmp(s1, s2) != 0);    // anderer Pfad -> anderer Slug
  CHECK(strchr(s1, '/') == nullptr && strchr(s1, '.') == nullptr);

  // urlExt: Endung aus der URL, Query/Fragment ignorieren, Default .mp3.
  char e[8];
  urlExt("http://h.fm/a.mp3?t=1", e, sizeof(e));   CHECK(strcmp(e, ".mp3") == 0);
  urlExt("http://h.fm/a.m4a", e, sizeof(e));        CHECK(strcmp(e, ".m4a") == 0);
  urlExt("http://h.fm/stream", e, sizeof(e));       CHECK(strcmp(e, ".mp3") == 0);
  urlExt("http://h.fm/x.weird", e, sizeof(e));      CHECK(strcmp(e, ".mp3") == 0);

  printf("  podcast ok\n");
  return 0;
}

// =============================================================================
// reinschrift (Todo-Parser + konfliktsicherer Merge)
// =============================================================================
static int testReinschrift() {
  using namespace reinschrift;
  Task t;

  // Vollständige Zeile parsen.
  CHECK(parseLine("- [ ] Putzen Fenster +Haushalt @Daheim due:2026-06-27T12:00 ~note:\"Fenster putzen\" ^Y64JiBDu", &t));
  CHECK(!t.done);
  CHECK(strcmp(t.title, "Putzen Fenster") == 0);
  CHECK(strcmp(t.topic, "Haushalt") == 0);
  CHECK(strcmp(t.context, "Daheim") == 0);
  CHECK(strcmp(t.marker, "Y64JiBDu") == 0);
  CHECK(t.dueEpoch != 0 && t.dueEpoch != kSomeday);

  // Erledigt + „irgendwann" (Jahr 9999) + Wikilink statt note.
  CHECK(parseLine("- [x] Sense entrosten +Sense @Keller due:9999-12-31T00:00 [[2025.md]] ^swbu95", &t));
  CHECK(t.done);
  CHECK(strcmp(t.title, "Sense entrosten") == 0);
  CHECK(isSomeday(t.dueEpoch));

  // Kein Attribut → ganzer Rest ist Titel.
  CHECK(parseLine("- [ ] Einfach nur Text", &t));
  CHECK(strcmp(t.title, "Einfach nur Text") == 0);
  CHECK(t.dueEpoch == 0 && t.marker[0] == '\0');

  // Header / Nicht-Aufgaben → false.
  CHECK(!parseLine("### +Haushalt", &t));
  CHECK(!parseLine("# Zentrale Aufgabenübersicht", &t));
  CHECK(!parseLine("", &t));
  CHECK(!parseLine("nur text", &t));

  // Themen-Header erkennen.
  char topic[kTopicLen];
  CHECK(parseTopicHeader("### +Haushalt", topic, sizeof(topic)));
  CHECK(strcmp(topic, "Haushalt") == 0);
  CHECK(!parseTopicHeader("## Kein Plus", topic, sizeof(topic)));
  CHECK(!parseTopicHeader("- [ ] x +Topic ^a", topic, sizeof(topic)));

  // Fälligkeit relativ zu „jetzt" (Tag-genau).
  uint32_t day = 86400u;
  uint32_t now = 20000u * day + 5000u;            // irgendein Tag, mittags-ish
  CHECK(isDueToday(19999u * day, now));            // gestern fällig → heute fällig
  CHECK(isDueToday(20000u * day, now));            // heute
  CHECK(!isDueToday(20001u * day, now));           // morgen
  CHECK(isOverdue(19999u * day, now));
  CHECK(!isOverdue(20000u * day, now));
  CHECK(!isOverdue(kSomeday, now) && !isDueToday(kSomeday, now));

  // --- Konfliktsicherer Merge ------------------------------------------------
  const char* remote =
    "# Zentrale Aufgabenübersicht\n"
    "### +Haushalt\n"
    "- [ ] Fenster putzen +Haushalt due:2026-06-27T12:00 ^aaa111\n"
    "- [ ] Müll raus +Haushalt due:2026-06-21T12:00 ^bbb222\n"
    "### +IT\n"
    "- [ ] Backups prüfen +IT due:9999-12-31T00:00 ^ccc333\n";

  // Lokale Ops: aaa111 abhaken, ccc333 auf morgen, neue Aufgabe unter +Haushalt.
  Op ops[3];
  memset(ops, 0, sizeof(ops));
  ops[0].kind = OP_TOGGLE; strcpy(ops[0].marker, "aaa111"); ops[0].done = true;
  ops[1].kind = OP_DUE;    strcpy(ops[1].marker, "ccc333"); strcpy(ops[1].value, "2026-06-23T12:00");
  ops[2].kind = OP_ADD;    strcpy(ops[2].topic, "Haushalt");
  strcpy(ops[2].line, "- [ ] Staubsaugen +Haushalt ^new999");

  char out[2048];
  CHECK(applyOps(remote, ops, 3, out, sizeof(out)));
  CHECK(strstr(out, "- [x] Fenster putzen +Haushalt due:2026-06-27T12:00 ^aaa111") != nullptr);  // abgehakt
  CHECK(strstr(out, "Müll raus") != nullptr);                       // FREMDE Zeile unverändert
  CHECK(strstr(out, "- [x] Müll raus") == nullptr);                 // NICHT mit-abgehakt
  CHECK(strstr(out, "Backups prüfen +IT due:2026-06-23T12:00") != nullptr);  // due ersetzt, nicht dupliziert
  CHECK(strstr(out, "due:9999") == nullptr);
  CHECK(strstr(out, "- [ ] Staubsaugen +Haushalt ^new999") != nullptr);      // neue Aufgabe da
  // unter dem richtigen Header (vor „### +IT").
  CHECK(strstr(out, "Staubsaugen") < strstr(out, "### +IT"));

  // Konflikt-Szenario: ein anderes Gerät hat aaa111 GELÖSCHT (Marker fehlt
  // remote). Unsere Toggle-Op darf nichts kaputtmachen und wird verworfen.
  const char* remote2 =
    "### +Haushalt\n"
    "- [ ] Müll raus +Haushalt ^bbb222\n";
  Op ops2[1]; memset(ops2, 0, sizeof(ops2));
  ops2[0].kind = OP_TOGGLE; strcpy(ops2[0].marker, "aaa111"); ops2[0].done = true;
  char out2[1024];
  CHECK(applyOps(remote2, ops2, 1, out2, sizeof(out2)));
  CHECK(strstr(out2, "Müll raus") != nullptr);     // fremder Stand bleibt
  CHECK(strstr(out2, "aaa111") == nullptr);          // verschwundene Op nicht erfunden

  // ADD ohne existierendes Thema → neues Thema am Ende.
  Op ops3[1]; memset(ops3, 0, sizeof(ops3));
  ops3[0].kind = OP_ADD; strcpy(ops3[0].topic, "Neu"); strcpy(ops3[0].line, "- [ ] Frisch +Neu ^z9");
  char out3[1024];
  CHECK(applyOps(remote2, ops3, 1, out3, sizeof(out3)));
  CHECK(strstr(out3, "### +Neu") != nullptr);
  CHECK(strstr(out3, "- [ ] Frisch +Neu ^z9") != nullptr);

  // Marker-Generierung: 8 Base62-Zeichen, deterministisch über die Test-RNG.
  char mk[kMarkerLen];
  genMarker(mk, sizeof(mk), testRnd);
  CHECK(strlen(mk) == 8);
  for (int i = 0; mk[i]; i++) {
    char c = mk[i];
    CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
  }

  printf("  reinschrift ok\n");
  return 0;
}

// =============================================================================
// ical (Kalender-Parser + RRULE-Expansion)
// =============================================================================
static int feedAll(ical::Parser& p, const char* ics, ical::Event* events, int maxEv) {
  int n = 0;
  const char* line = ics;
  char buf[512];
  while (*line) {
    const char* e = line;
    while (*e && *e != '\n') e++;
    size_t len = (size_t)(e - line);
    if (len > 0 && line[len - 1] == '\r') len--;     // CRLF → drop CR
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(buf, line, len); buf[len] = '\0';
    ical::Event ev;
    if (p.feedLine(buf, &ev) && n < maxEv) events[n++] = ev;
    line = (*e == '\n') ? e + 1 : e;
  }
  ical::Event ev;
  if (p.finish(&ev) && n < maxEv) events[n++] = ev;
  return n;
}

static int testIcal() {
  using namespace ical;

  // Zwei Events; eines all-day, eines mit Datum-Zeit; gefaltete SUMMARY-Zeile.
  const char* ics =
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "BEGIN:VEVENT\r\n"
    "UID:abc-1\r\n"
    "SUMMARY:Zahnarzt Termin sehr lang gefa\r\n"
    " ltet und entfaltet\r\n"
    "DTSTART:20260622T090000Z\r\n"
    "DTEND:20260622T100000Z\r\n"
    "LOCATION:Praxis\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "UID:abc-2\r\n"
    "SUMMARY:Geburtstag\r\n"
    "DTSTART;VALUE=DATE:20260625\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n";

  Parser p;
  Event evs[8];
  int n = feedAll(p, ics, evs, 8);
  CHECK(n == 2);
  CHECK(strcmp(evs[0].summary, "Zahnarzt Termin sehr lang gefaltet und entfaltet") == 0);
  CHECK(!evs[0].allDay);
  CHECK(strcmp(evs[0].location, "Praxis") == 0);
  CHECK(evs[0].end > evs[0].start);
  CHECK(evs[0].end - evs[0].start == 3600);   // 1 Stunde
  CHECK(evs[1].allDay);
  CHECK(strcmp(evs[1].summary, "Geburtstag") == 0);

  // 2026-06-22T09:00:00Z als Epoche prüfen (über bekannte Tageszahl).
  uint32_t start22 = (uint32_t)(detail::daysFromCivil(2026, 6, 22) * 86400L) + 9 * 3600;
  CHECK(evs[0].start == start22);

  // Einmaliges Event nur im Fenster.
  int count = 0;
  expand(evs[0], start22 - 86400, start22 + 86400, [&](uint32_t, uint32_t) { count++; });
  CHECK(count == 1);
  count = 0;
  expand(evs[0], start22 + 7 * 86400, start22 + 14 * 86400, [&](uint32_t, uint32_t) { count++; });
  CHECK(count == 0);   // außerhalb

  // Wöchentliche Wiederholung, COUNT begrenzt, Fenster begrenzt.
  const char* rec =
    "BEGIN:VEVENT\r\n"
    "SUMMARY:Standup\r\n"
    "DTSTART:20260601T080000Z\r\n"
    "DTEND:20260601T081500Z\r\n"
    "RRULE:FREQ=WEEKLY;COUNT=10\r\n"
    "END:VEVENT\r\n";
  Parser p2;
  Event r[2];
  int rn = feedAll(p2, rec, r, 2);
  CHECK(rn == 1);
  CHECK(strcmp(r[0].rrule, "FREQ=WEEKLY;COUNT=10") == 0);
  uint32_t s0 = (uint32_t)(detail::daysFromCivil(2026, 6, 1) * 86400L) + 8 * 3600;
  CHECK(r[0].start == s0);
  // Fenster über 10 Wochen → genau 10 Vorkommen (COUNT-Deckel).
  int occ = 0;
  expand(r[0], s0, s0 + 20u * 7 * 86400, [&](uint32_t, uint32_t) { occ++; });
  CHECK(occ == 10);
  // Engeres Fenster (3 Wochen ab Start) → 3 Vorkommen.
  occ = 0;
  expand(r[0], s0, s0 + 3u * 7 * 86400, [&](uint32_t, uint32_t) { occ++; });
  CHECK(occ == 3);

  // RRULE-Feld-Extraktion.
  char v[16];
  CHECK(rruleField("FREQ=DAILY;INTERVAL=2;COUNT=5", "INTERVAL", v, sizeof(v)));
  CHECK(strcmp(v, "2") == 0);
  CHECK(rruleField("FREQ=DAILY;INTERVAL=2;COUNT=5", "FREQ", v, sizeof(v)));
  CHECK(strcmp(v, "DAILY") == 0);
  CHECK(!rruleField("FREQ=DAILY", "UNTIL", v, sizeof(v)));

  // Monatliche Wiederholung: 4 Monate Fenster → 4 Vorkommen.
  const char* mon =
    "BEGIN:VEVENT\r\nSUMMARY:Miete\r\nDTSTART:20260101T000000Z\r\n"
    "RRULE:FREQ=MONTHLY\r\nEND:VEVENT\r\n";
  Parser p3; Event m[2];
  CHECK(feedAll(p3, mon, m, 2) == 1);
  uint32_t ms = (uint32_t)(detail::daysFromCivil(2026, 1, 1) * 86400L);
  occ = 0;
  expand(m[0], ms, (uint32_t)(detail::daysFromCivil(2026, 5, 1) * 86400L), [&](uint32_t, uint32_t) { occ++; });
  CHECK(occ == 4);   // Jan, Feb, Mär, Apr

  printf("  ical ok\n");
  return 0;
}

int main() {
  if (testMathquiz()) return 1;
  if (testFlashcards()) return 1;
  if (testPodcast()) return 1;
  if (testReinschrift()) return 1;
  if (testIcal()) return 1;
  printf("ALLE TESTS GRÜN (%d Checks)\n", s_checks);
  return 0;
}
