// =============================================================================
// i18n.h — Laufzeit-Lokalisierung (Deutsch = Default, Italienisch, Schwedisch).
//
// Alle UI-Strings laufen über tr(Str::...); die Tabellen liegen als
// const-Arrays im Flash (.rodata). Die X-Macro-Liste hält ID und alle
// Übersetzungen auf EINER Zeile — Positions-Drift zwischen Enum und Tabellen
// ist damit ausgeschlossen (static_assert prüft zusätzlich die Anzahl).
//
// Neue Sprache ergänzen: Lang-Eintrag, Spalte in FENNEK_STRS, Tabelle in
// i18n.cpp, langName(). Leere Einträge fallen auf Deutsch zurück.
//
// Konventionen:
//  - Fmt*-Einträge sind snprintf-Formatstrings: Spezifikatoren (Anzahl,
//    Reihenfolge, Typ) MÜSSEN in allen Sprachen identisch sein!
//  - Breitenlimits (textSize 2 = 12 px/Zeichen): 70/72-px-Buttons max 5,
//    76 px max 6, 102 px max 8, 110 px & Kacheln max 8–9 Zeichen.
//  - NICHT übersetzen: NVS-Keys, Pfade, Glyph-Escapes, LoRa-Jargon, "Fennek".
// =============================================================================
#pragma once
#include <stdint.h>

// X(Id, Deutsch, Italiano, Svenska)
#define FENNEK_STRS(X) \
  /* --- App-Namen (Statuszeile) --- */ \
  X(AppLauncher,   "Start",          "Home",            "Hem") \
  X(AppMusic,      "Musik",          "Musica",          "Musik") \
  X(AppBook,       "Hörbuch",        "Audiolibro",      "Ljudbok") \
  X(AppReader,     "Lesen",          "Lettura",         "Läsning") \
  X(AppMesh,       "Mesh",           "Mesh",            "Mesh") \
  X(AppSettings,   "Einstellungen",  "Impostazioni",    "Inställningar") \
  X(AppGames,      "Spiele",         "Giochi",          "Spel") \
  /* --- Launcher-Kacheln (max 8 Zeichen) --- */ \
  X(TileMusic,     "Musik",          "Musica",          "Musik") \
  X(TileBook,      "Hörbuch",        "Ascolto",         "Ljudbok") \
  X(TileReader,    "Lesen",          "Libri",           "Läsa") \
  X(TileMesh,      "Mesh",           "Mesh",            "Mesh") \
  X(TileSettings,  "Optionen",       "Opzioni",         "System") \
  X(TileGames,     "Spiele",         "Giochi",          "Spel") \
  X(TileSoon,      "(bald)",         "(presto)",        "(snart)") \
  X(FmtResume,     "Fortsetzen: %s", "Riprendi: %s",    "Fortsätt: %s") /* fmt: %s */ \
  /* --- Gemeinsame Buttons --- */ \
  X(BtnHome,       "Home",           "Home",            "Hem") \
  X(BtnBack,       "Zurück",         "Indietro",        "Tillbaka") /* 110 px */ \
  X(BtnBackShort,  "Zurück",         "Torna",           "Bakåt") /* 76 px, max 6 */ \
  X(BtnUp,         "Hoch",           "Su",              "Upp") /* 72 px, max 5 */ \
  X(BtnDown,       "Runter",         "Giù",             "Ned") /* 72 px */ \
  X(BtnList,       "Liste",          "Lista",           "Lista") /* 76 px, max 6 */ \
  X(BtnPlayer,     "Player",         "Player",          "Player") \
  X(BtnRetrySD,    "Erneut suchen",  "Cerca di nuovo",  "Sök igen") \
  X(BtnCancel,     "Abbrechen",      "Annulla",         "Avbryt") \
  X(BtnStart,      "Start",          "Start",           "Start") \
  /* --- Gemeinsame Meldungen --- */ \
  X(NoSdCard,      "Keine SD-Karte",        "Nessuna scheda SD",       "Inget SD-kort") \
  X(InsertCard,    "Karte einlegen, dann:", "Inserisci la scheda, poi:", "Sätt i kortet, sedan:") \
  X(StatusLocked,  "Gesperrt",       "Bloccato",        "Låst") \
  X(SleepWakeHint, "Knopf drücken zum Aufwecken", "Premi il tasto per svegliare", "Tryck på knappen för att väcka") \
  X(EmptyList,     "(leer)",         "(vuoto)",         "(tomt)") \
  /* --- Musik --- */ \
  X(MusicArtists,  "Künstler",       "Artisti",         "Artister") \
  X(MusicAlbum,    "Album",          "Album",           "Album") \
  X(MusicAlbums,   "Alben",          "Album",           "Album") \
  X(MusicTitles,   "Titel",          "Brani",           "Titlar") \
  X(MusicPlaylist, "Playlist",       "Playlist",        "Spellista") \
  X(MusicPlaylists,"Playlists",      "Playlist",        "Spellistor") \
  X(MusicPlayback, "Wiedergabe",     "Riproduzione",    "Uppspelning") \
  X(BtnShuffle,    "Mix",            "Mix",             "Mix") /* 70 px, max 5 */ \
  X(RepOff,        "Wdh",            "Rip.",            "Rep.") /* 70 px */ \
  X(RepOne,        "Wdh 1",          "Rip.1",           "Rep.1") \
  X(RepAll,        "Wdh A",          "Rip.A",           "Rep.A") \
  X(FmtTagScan,    "Tags %d/%d",     "Tag %d/%d",       "Taggar %d/%d") /* fmt: %d %d */ \
  /* --- Hörbuch --- */ \
  X(BookHeader,    "Hörbücher",      "Audiolibri",      "Ljudböcker") \
  X(BookNone,      "Keine Hörbücher","Nessun audiolibro","Inga ljudböcker") \
  X(BookHint1,     "Ordner /audiobooks anlegen,", "Crea la cartella /audiobooks,", "Skapa mappen /audiobooks,") \
  X(BookHint2,     "pro Buch ein Unterordner.",   "una sottocartella per libro.",  "en undermapp per bok.") \
  X(BtnBookList,   "Bücherliste",    "Elenco libri",    "Boklista") /* 228 px */ \
  X(FmtChapter,    "Kapitel %d/%d  %s", "Capitolo %d/%d  %s", "Kapitel %d/%d  %s") /* fmt: %d %d %s */ \
  /* --- Lesen --- */ \
  X(ReaderBooks,   "Bücher",         "Libri",           "Böcker") \
  X(ReaderSearch,  "Suche Bücher ...","Cerco libri ...", "Söker böcker ...") \
  X(ReaderNone,    "Keine Bücher",   "Nessun libro",    "Inga böcker") \
  X(ReaderHint,    ".txt/.epub nach /books legen.", "Metti .txt/.epub in /books.", "Lägg .txt/.epub i /books.") \
  /* --- Mesh --- */ \
  X(MeshChannel,   "Mesh-Kanal",     "Canale Mesh",     "Mesh-kanal") \
  X(MeshContacts,  "Kontakte",       "Contatti",        "Kontakter") \
  X(MeshNoMsgs,    "(noch keine Nachrichten)", "(ancora nessun messaggio)", "(inga meddelanden än)") \
  X(MeshNoContacts,"Keine Kontakte", "Nessun contatto", "Inga kontakter") \
  X(MeshCtHint1,   "Kontakte erscheinen automatisch,", "I contatti appaiono da soli,", "Kontakter dyker upp automatiskt") \
  X(MeshCtHint2,   "sobald Adverts empfangen werden.", "quando arrivano gli advert.",  "när adverts tas emot.") \
  X(MeshNoRadio,   "Radio nicht gefunden", "Radio non trovata", "Radion hittades inte") \
  X(MeshInitFail,  "SX1262-Init fehlgeschlagen.", "Init SX1262 fallita.", "SX1262-init misslyckades.") \
  X(MeshMe,        "ich",            "io",              "jag") \
  X(BtnChannel,    "Kanal",          "Canale",          "Kanal") \
  X(BtnContacts,   "Kont.",          "Cont.",           "Kont.") \
  X(BtnAdvert,     "Advert",         "Advert",          "Advert") \
  /* --- Einstellungen (Label/Wert-Layout; Werte selbst sind LoRa-Jargon) --- */ \
  X(SettingsLang,  "Sprache",        "Lingua",          "Språk") \
  X(SettingsManual,"Manuell",        "Manuale",         "Manuell") \
  X(SecRadio,      "Funk — wirkt sofort", "Radio — effetto immediato", "Radio — gäller direkt") \
  X(SecSystem,     "System",         "Sistema",         "System") \
  X(LblFreq,       "Frequenz",       "Frequenza",       "Frekvens") \
  X(LblBandwidth,  "Bandbreite",     "Banda",           "Bandbredd") \
  X(LblCodingRate, "Coding-Rate",    "Coding rate",     "Kodningsgrad") \
  X(LblTxPower,    "Sendeleistung",  "Potenza TX",      "Sändeffekt") \
  X(LblName,       "Name",           "Nome",            "Namn") \
  X(LblStandby,    "Auto-Standby",   "Auto-standby",    "Auto-standby") \
  X(StandbyOff,    "Aus",            "No",              "Av") \
  X(HintEnterSave, "Enter speichert","Enter salva",     "Enter sparar") /* max 19 Z. */ \
  X(HintNameEdit,  "Enter: Name ändern", "Enter: cambia nome", "Enter: byt namn") /* max 19 Z. */ \
  X(HintChange,    "A/D bzw. Tap ändert", "A/D o tap modifica", "A/D el. tap ändrar") /* max 19 Z. */ \
  X(FmtBattery,    "Akku %u%%%s (%u mV)", "Batt. %u%%%s (%u mV)", "Batt. %u%%%s (%u mV)") /* fmt: %u %s %u, max 20 Z. */ \
  /* --- Spiele: Menü --- */ \
  X(Game2048,      "2048",           "2048",            "2048") \
  X(GameMines,     "Minensucher",    "Campo minato",    "Minröjare") \
  X(GameChess,     "Schach",         "Scacchi",         "Schack") \
  X(GameTtt,       "Tic-Tac-Toe",    "Tris",            "Tre i rad") \
  X(NoGameYet,     "Noch kein Spiel","Nessuna partita", "Inget spel än") \
  X(FmtBest,       "Best: %lu",      "Record: %lu",     "Bäst: %lu") /* fmt: %lu */ \
  /* --- 2048 --- */ \
  X(FmtPoints,     "Punkte: %lu",    "Punti: %lu",      "Poäng: %lu") /* fmt: %lu */ \
  X(Hint2048,      "WASD schiebt · N=Neu · Backspace=Menü", "WASD muove · N=Nuovo · Backspace=Menu", "WASD flyttar · N=Ny · Backspace=Meny") \
  X(Won2048,       "2048 erreicht!", "2048 raggiunto!", "2048 uppnått!") \
  X(ContinueEnter, "Weiterspielen: Enter", "Continua: Enter", "Fortsätt: Enter") \
  X(GameOver,      "Vorbei!",        "Finita!",         "Slut!") \
  X(Fmt2048Over,   "Punkte: %lu · N = neues Spiel", "Punti: %lu · N = nuova partita", "Poäng: %lu · N = nytt spel") /* fmt: %lu */ \
  /* --- Minensucher --- */ \
  X(MinesWin,      "Geschafft!",     "Fatto!",          "Klarat!") \
  X(MinesBoom,     "Boom!",          "Boom!",           "Pang!") \
  X(FmtMinesTime,  "%lu s · N = neues Spiel", "%lu s · N = nuova partita", "%lu s · N = nytt spel") /* fmt: %lu */ \
  X(MinesNewGame,  "N = neues Spiel","N = nuova partita","N = nytt spel") \
  X(FmtMinesLeft,  "Minen: %d",      "Mine: %d",        "Minor: %d") /* fmt: %d */ \
  X(BtnFlag,       "Flagge",         "Bandiera",        "Flagga") /* 102 px, max 8 */ \
  X(BtnDig,        "Graben",         "Scava",           "Gräv") /* 102 px */ \
  X(FmtMinesWins,  "Siege: %u  ·  Bestzeit: %u s", "Vittorie: %u  ·  Record: %u s", "Segrar: %u  ·  Bästa: %u s") /* fmt: %u %u */ \
  X(NoWinYet,      "Noch kein Sieg", "Nessuna vittoria","Ingen seger än") \
  /* --- Schach --- */ \
  X(ChessNewGame,  "Neue Partie",    "Nuova partita",   "Nytt parti") \
  X(FmtOpponent,   "Gegner: %s",     "Avversario: %s",  "Motståndare: %s") /* fmt: %s */ \
  X(TwoPlayers,    "2 Spieler",      "2 giocatori",     "2 spelare") \
  X(FmtYourColor,  "Deine Farbe: %s","Il tuo colore: %s","Din färg: %s") /* fmt: %s */ \
  X(ColorWhite,    "Weiß",           "Bianco",          "Vit") \
  X(ColorBlack,    "Schwarz",        "Nero",            "Svart") \
  X(FmtLevel,      "Stufe: %s",      "Livello: %s",     "Nivå: %s") /* fmt: %s */ \
  X(LevelEasy,     "Leicht",         "Facile",          "Lätt") \
  X(LevelMid,      "Mittel",         "Medio",           "Medel") \
  X(LevelHard,     "Schwer",         "Difficile",       "Svår") \
  X(ChessBackHint, "Backspace = zurück zur Partie", "Backspace = torna alla partita", "Backspace = åter till partiet") \
  X(ChessPromo,    "Umwandlung:",    "Promozione:",     "Förvandling:") \
  X(ChessMateW,    "Matt - Weiß gewinnt!",   "Matto - vince il Bianco!", "Matt - Vit vinner!") \
  X(ChessMateB,    "Matt - Schwarz gewinnt!","Matto - vince il Nero!",   "Matt - Svart vinner!") \
  X(ChessStale,    "Patt - Remis",   "Stallo - patta",  "Patt - remi") \
  X(ChessDraw50,   "Remis (50 Züge)","Patta (50 mosse)","Remi (50 drag)") \
  X(ChessDrawRep,  "Remis (Wiederholung)", "Patta (ripetizione)", "Remi (upprepning)") \
  X(ChessThinking, "Fennek denkt ...","Fennek pensa ...","Fennek tänker ...") \
  X(FmtChessTurn,  "%s am Zug%s",    "Muove il %s%s",   "%s vid draget%s") /* fmt: %s %s */ \
  X(ChessCheck,    " - Schach!",     " - scacco!",      " - schack!") \
  X(ChessNewHint,  "N = neue Partie","N = nuova partita","N = nytt parti") \
  X(YouWhite,      "Du: Weiß",       "Tu: Bianco",      "Du: Vit") \
  X(YouBlack,      "Du: Schwarz",    "Tu: Nero",        "Du: Svart") \
  X(ChessKeyHint,  "WASD+Enter oder Tap · N=Neu · Backspace=Menü", "WASD+Enter o tap · N=Nuovo · Backspace=Menu", "WASD+Enter eller tryck · N=Ny · Backspace=Meny") \
  X(FmtChessSaved, "Partie läuft · Siege: %u", "Partita in corso · Vittorie: %u", "Parti pågår · Segrar: %u") /* fmt: %u */ \
  X(FmtChessWins,  "Siege gegen Fennek: %u", "Vittorie su Fennek: %u", "Segrar mot Fennek: %u") /* fmt: %u */ \
  /* --- Tic-Tac-Toe --- */ \
  X(TttXWins,      "X gewinnt!",     "Vince X!",        "X vinner!") \
  X(TttOWins,      "O gewinnt!",     "Vince O!",        "O vinner!") \
  X(TttYouWin,     "Du gewinnst!",   "Hai vinto!",      "Du vinner!") \
  X(TttFennekWins, "Fennek gewinnt!","Vince Fennek!",   "Fennek vinner!") \
  X(TttDraw,       "Remis!",         "Patta!",          "Remi!") \
  X(FmtTttTurn,    "%s ist dran",    "Tocca a %s",      "%s:s tur") /* fmt: %s */ \
  X(TttYouAreX,    "Du bist X",      "Sei X",           "Du är X") \
  X(TttVsFennek,   "Gegner: Fennek", "Avversario: Fennek", "Motståndare: Fennek") \
  X(TttTwoPlayers, "2 Spieler am Gerät", "2 giocatori locali", "2 spelare på enheten") \
  X(TttKeyHint,    "N=Neu  M=Modus  Backspace=Menü", "N=Nuovo  M=Modo  Backspace=Menu", "N=Ny  M=Läge  Backspace=Meny") \
  X(FmtTttWins,    "Siege: %u  ·  Remis: %u", "Vittorie: %u  ·  Patte: %u", "Segrar: %u  ·  Remi: %u") /* fmt: %u %u */

namespace i18n {

enum class Lang : uint8_t { DE = 0, IT = 1, SV = 2, COUNT };

enum class Str : uint16_t {
  None = 0,
#define X(id, de, it, sv) id,
  FENNEK_STRS(X)
#undef X
  COUNT
};
constexpr uint16_t STR_COUNT = (uint16_t)Str::COUNT;

Lang        lang();                 // aktive Sprache (Cache aus settings)
void        setLang(Lang l);        // setzt Cache + persistiert via settings
const char* langName(Lang l);       // Endonym: "Deutsch"/"Italiano"/"Svenska"
const char* tr(Str id);             // Lookup; leerer Eintrag -> Deutsch

}  // namespace i18n
