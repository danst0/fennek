// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// i18n.h — Laufzeit-Lokalisierung (Deutsch = Default, IT, SV, EN, ES).
//
// Alle UI-Strings laufen über tr(Str::...); die Tabellen liegen als
// const-Arrays im Flash (.rodata). Die X-Macro-Liste hält ID und alle
// Übersetzungen auf EINER Zeile — Positions-Drift zwischen Enum und Tabellen
// ist damit ausgeschlossen (static_assert prüft zusätzlich die Anzahl).
//
// Neue Sprache ergänzen: Lang-Eintrag, Spalte in FENNEK_STRS, Tabelle in
// i18n.cpp, langName(). Leere Einträge fallen auf Deutsch zurück.
// Lang-Reihenfolge NIE ändern (Index liegt im NVS) — neue Sprachen anhängen!
//
// Konventionen:
//  - Fmt*-Einträge sind snprintf-Formatstrings: Spezifikatoren (Anzahl,
//    Reihenfolge, Typ) MÜSSEN in allen Sprachen identisch sein!
//  - Breitenlimits (textSize 2 = 12 px/Zeichen): 70/72-px-Buttons max 5,
//    76 px max 6, 102 px max 8, 110 px & Kacheln max 8–9 Zeichen.
//  - CP437-Falle: Á/Í/Ó/Ú fehlen im Font (klein á í ó ú ñ é sowie ¡ ¿ É Ñ
//    sind da) — Großbuchstaben-Akzente in ES/IT vermeiden.
//  - NICHT übersetzen: NVS-Keys, Pfade, Glyph-Escapes, LoRa-Jargon, "Fennek".
// =============================================================================
#pragma once
#include <stdint.h>

// X(Id, Deutsch, Italiano, Svenska, English, Español)
#define FENNEK_STRS(X) \
  /* --- App-Namen (Statuszeile) --- */ \
  X(AppLauncher,   "Start",          "Home",            "Hem",            "Home",            "Inicio") \
  X(AppMusic,      "Musik",          "Musica",          "Musik",          "Music",           "Música") \
  X(AppBook,       "Hörbuch",        "Audiolibro",      "Ljudbok",        "Audiobook",       "Audiolibro") \
  X(AppReader,     "Lesen",          "Lettura",         "Läsning",        "Reading",         "Lectura") \
  X(AppMesh,       "Mesh",           "Mesh",            "Mesh",           "Mesh",            "Mesh") \
  X(AppSettings,   "Einstellungen",  "Impostazioni",    "Inställningar",  "Settings",        "Ajustes") \
  X(AppGames,      "Spiele",         "Giochi",          "Spel",           "Games",           "Juegos") \
  X(AppFiles,      "Dateien",        "File",            "Filer",          "Files",           "Archivos") \
  X(AppNotes,      "Notizen",        "Note",            "Anteckningar",   "Notes",           "Notas") \
  /* --- Launcher-Kacheln (max 8 Zeichen) --- */ \
  X(TileMusic,     "Musik",          "Musica",          "Musik",          "Music",           "Música") \
  X(TileBook,      "Hörbuch",        "Ascolto",         "Ljudbok",        "Listen",          "Escuchar") \
  X(TileReader,    "Lesen",          "Libri",           "Läsa",           "Read",            "Leer") \
  X(TileMesh,      "Mesh",           "Mesh",            "Mesh",           "Mesh",            "Mesh") \
  X(TileSettings,  "Optionen",       "Opzioni",         "System",         "Settings",        "Ajustes") \
  X(TileGames,     "Spiele",         "Giochi",          "Spel",           "Games",           "Juegos") \
  X(TileFiles,     "Dateien",        "File",            "Filer",          "Files",           "Archivos") \
  X(TileNotes,     "Notizen",        "Note",            "Anteckn.",       "Notes",           "Notas") \
  X(TileSoon,      "(bald)",         "(presto)",        "(snart)",        "(soon)",          "(pronto)") \
  X(FmtResume,     "Fortsetzen: %s", "Riprendi: %s",    "Fortsätt: %s",   "Resume: %s",      "Continuar: %s") /* fmt: %s */ \
  /* --- Gemeinsame Buttons --- */ \
  X(BtnHome,       "Home",           "Home",            "Hem",            "Home",            "Inicio") \
  X(BtnBack,       "Zurück",         "Indietro",        "Tillbaka",       "Back",            "Atrás") /* 110 px */ \
  X(BtnBackShort,  "Zurück",         "Torna",           "Bakåt",          "Back",            "Atrás") /* 76 px, max 6 */ \
  X(BtnUp,         "Hoch",           "Su",              "Upp",            "Up",              "Subir") /* 72 px, max 5 */ \
  X(BtnDown,       "Runter",         "Giù",             "Ned",            "Down",            "Bajar") /* 72 px */ \
  X(BtnList,       "Liste",          "Lista",           "Lista",          "List",            "Lista") /* 76 px, max 6 */ \
  X(BtnPlayer,     "Player",         "Player",          "Player",         "Player",          "Player") \
  X(BtnRetrySD,    "Erneut suchen",  "Cerca di nuovo",  "Sök igen",       "Search again",    "Buscar de nuevo") \
  X(BtnCancel,     "Abbrechen",      "Annulla",         "Avbryt",         "Cancel",          "Cancelar") \
  X(BtnStart,      "Start",          "Start",           "Start",          "Start",           "Jugar") \
  /* --- Gemeinsame Meldungen --- */ \
  X(NoSdCard,      "Keine SD-Karte",        "Nessuna scheda SD",         "Inget SD-kort",         "No SD card",          "No hay tarjeta SD") \
  X(InsertCard,    "Karte einlegen, dann:", "Inserisci la scheda, poi:", "Sätt i kortet, sedan:", "Insert card, then:",  "Inserta la tarjeta, luego:") \
  X(StatusLocked,  "Gesperrt",       "Bloccato",        "Låst",           "Locked",          "Bloqueado") \
  X(SleepWakeHint, "Knopf drücken zum Aufwecken", "Premi il tasto per svegliare", "Tryck på knappen för att väcka", "Press the button to wake", "Pulsa el botón para despertar") \
  X(EmptyList,     "(leer)",         "(vuoto)",         "(tomt)",         "(empty)",         "(vacío)") \
  /* --- Musik --- */ \
  X(MusicArtists,  "Künstler",       "Artisti",         "Artister",       "Artists",         "Artistas") \
  X(MusicAlbum,    "Album",          "Album",           "Album",          "Albums",          "Discos") \
  X(MusicAlbums,   "Alben",          "Album",           "Album",          "Albums",          "Discos") \
  X(MusicTitles,   "Titel",          "Brani",           "Titlar",         "Songs",           "Canciones") \
  X(MusicPlaylist, "Playlist",       "Playlist",        "Spellista",      "Playlist",        "Playlist") \
  X(MusicPlaylists,"Playlists",      "Playlist",        "Spellistor",     "Playlists",       "Playlists") \
  X(MusicPlayback, "Wiedergabe",     "Riproduzione",    "Uppspelning",    "Now playing",     "Reproducción") \
  X(BtnShuffle,    "Mix",            "Mix",             "Mix",            "Mix",             "Mix") /* 70 px, max 5 */ \
  X(RepOff,        "Wdh",            "Rip.",            "Rep.",           "Rep.",            "Rep.") /* 70 px */ \
  X(RepOne,        "Wdh 1",          "Rip.1",           "Rep.1",          "Rep.1",           "Rep.1") \
  X(RepAll,        "Wdh A",          "Rip.A",           "Rep.A",          "Rep.A",           "Rep.A") \
  X(FmtTagScan,    "Tags %d/%d",     "Tag %d/%d",       "Taggar %d/%d",   "Tags %d/%d",      "Tags %d/%d") /* fmt: %d %d */ \
  /* --- Hörbuch --- */ \
  X(BookHeader,    "Hörbücher",      "Audiolibri",      "Ljudböcker",     "Audiobooks",      "Audiolibros") \
  X(BookNone,      "Keine Hörbücher","Nessun audiolibro","Inga ljudböcker","No audiobooks",  "Sin audiolibros") \
  X(BookHint1,     "Ordner /audiobooks anlegen,", "Crea la cartella /audiobooks,", "Skapa mappen /audiobooks,", "Create folder /audiobooks,", "Crea la carpeta /audiobooks,") \
  X(BookHint2,     "pro Buch ein Unterordner.",   "una sottocartella per libro.",  "en undermapp per bok.",     "one subfolder per book.",    "una subcarpeta por libro.") \
  X(BtnBookList,   "Bücherliste",    "Elenco libri",    "Boklista",       "Book list",       "Lista de libros") /* 228 px */ \
  X(FmtChapter,    "Kapitel %d/%d  %s", "Capitolo %d/%d  %s", "Kapitel %d/%d  %s", "Chapter %d/%d  %s", "Capítulo %d/%d  %s") /* fmt: %d %d %s */ \
  /* --- Lesen --- */ \
  X(ReaderBooks,   "Bücher",         "Libri",           "Böcker",         "Books",           "Libros") \
  X(ReaderSearch,  "Suche Bücher ...","Cerco libri ...", "Söker böcker ...","Scanning books ...","Buscando libros ...") \
  X(ReaderNone,    "Keine Bücher",   "Nessun libro",    "Inga böcker",    "No books",        "Sin libros") \
  X(ReaderHint,    ".txt/.epub nach /books legen.", "Metti .txt/.epub in /books.", "Lägg .txt/.epub i /books.", "Put .txt/.epub in /books.", "Pon .txt/.epub en /books.") \
  X(FmtReaderFound,"%d gefunden",    "%d trovati",      "%d hittade",     "%d found",        "%d encontrados") /* fmt: %d */ \
  X(ReaderRescan,  "R: neu einlesen","R: riscansiona",  "R: skanna om",   "R: rescan",       "R: reescanear") \
  /* --- Notizen --- */ \
  X(NotesToday,    "+ Heute",        "+ Oggi",          "+ Idag",         "+ Today",         "+ Hoy") \
  X(NotesEmpty,    "Noch keine Notizen", "Ancora nessuna nota", "Inga anteckningar än", "No notes yet", "Aún sin notas") \
  X(NotesDelQ,     "Löschen? Enter=ja",  "Eliminare? Enter=sì", "Radera? Enter=ja",     "Delete? Enter=yes", "¿Borrar? Enter=sí") \
  /* --- Mesh --- */ \
  X(MeshChannel,   "Mesh-Kanal",     "Canale Mesh",     "Mesh-kanal",     "Mesh channel",    "Canal Mesh") \
  X(MeshChats,     "Chats",          "Chat",            "Chattar",        "Chats",           "Chats") \
  X(MeshContacts,  "Kontakte",       "Contatti",        "Kontakter",      "Contacts",        "Contactos") \
  X(MeshNoMsgs,    "(noch keine Nachrichten)", "(ancora nessun messaggio)", "(inga meddelanden än)", "(no messages yet)", "(sin mensajes aún)") \
  X(MeshNoContacts,"Keine Kontakte", "Nessun contatto", "Inga kontakter", "No contacts",     "Sin contactos") \
  X(MeshCtHint1,   "Noch keine Chats.",               "Ancora nessuna chat.",         "Inga chattar än.",               "No chats yet.",                 "Aún no hay chats.") \
  X(MeshCtHint2,   "Kontakte-Button zeigt alle Knoten.", "Il tasto Contatti mostra i nodi.", "Kontakter-knappen visar noder.", "Contacts button lists all nodes.", "Contactos muestra los nodos.") \
  X(FmtMeshStatus, "NF%d RX%lu TX%lu  %dKt %s", "NF%d RX%lu TX%lu  %dKt %s", "NF%d RX%lu TX%lu  %dKt %s", "NF%d RX%lu TX%lu  %dCt %s", "NF%d RX%lu TX%lu  %dCt %s") /* fmt: %d %lu %lu %d %s */ \
  X(MeshResendHint,"Enter: ! erneut senden", "Enter: rinvia !", "Enter: skicka ! igen", "Enter: resend !", "Enter: reenviar !") \
  X(FmtAge,        "vor %s",     "%s fa",    "%s sedan", "%s ago",   "hace %s") /* fmt: %s */ \
  X(BtnRetry,      "Erneut",     "Ripeti",   "Igen",     "Retry",    "Reint.") /* 72 px, max 6 */ \
  X(MeshNoRadio,   "Radio nicht gefunden", "Radio non trovata", "Radion hittades inte", "Radio not found", "Radio no encontrada") \
  X(MeshInitFail,  "SX1262-Init fehlgeschlagen.", "Init SX1262 fallita.", "SX1262-init misslyckades.", "SX1262 init failed.", "Fallo de init SX1262.") \
  X(MeshSeeLog,    "Details im Serial-Log.", "Dettagli nel log seriale.", "Detaljer i seriell logg.", "Details in serial log.", "Detalles en el log serie.") \
  X(MeshMe,        "ich",            "io",              "jag",            "me",              "yo") \
  X(BtnChannel,    "Kanal",          "Canale",          "Kanal",          "Chan.",           "Canal") \
  X(BtnContacts,   "Kont.",          "Cont.",           "Kont.",          "Cont.",           "Cont.") \
  X(BtnAdvert,     "Advert",         "Advert",          "Advert",         "Advert",          "Advert") \
  X(MeshNewChannel,"\x10 Neuer Kanal","\x10 Nuovo canale","\x10 Ny kanal",  "\x10 New channel", "\x10 Nuevo canal") \
  X(MeshJoinTitle, "Neuer Kanal",    "Nuovo canale",    "Ny kanal",       "New channel",     "Nuevo canal") \
  X(MeshJoinHint,  "Name tippen, Enter = beitreten", "Scrivi nome, Enter per entrare", "Skriv namn, Enter för att gå med", "Type name, Enter to join", "Escribe nombre, Enter para unir") \
  X(BtnJoin,       "Beitr.",         "Entra",           "Gå med",         "Join",            "Unir") \
  /* --- Einstellungen (Label/Wert-Layout; Werte selbst sind LoRa-Jargon) --- */ \
  X(SettingsLang,  "Sprache",        "Lingua",          "Språk",          "Language",        "Idioma") \
  X(SettingsManual,"Manuell",        "Manuale",         "Manuell",        "Manual",          "Manual") \
  X(SecRadio,      "Funk — wirkt sofort", "Radio — effetto immediato", "Radio — gäller direkt", "Radio — applies instantly", "Radio — efecto inmediato") \
  X(SecSystem,     "System",         "Sistema",         "System",         "System",          "Sistema") \
  X(LblFreq,       "Frequenz",       "Frequenza",       "Frekvens",       "Frequency",       "Frecuencia") \
  X(LblBandwidth,  "Bandbreite",     "Banda",           "Bandbredd",      "Bandwidth",       "Ancho de banda") \
  X(LblCodingRate, "Coding-Rate",    "Coding rate",     "Kodningsgrad",   "Coding rate",     "Coding rate") \
  X(LblTxPower,    "Sendeleistung",  "Potenza TX",      "Sändeffekt",     "TX power",        "Potencia TX") \
  X(LblName,       "Name",           "Nome",            "Namn",           "Name",            "Nombre") \
  X(LblStandby,    "Auto-Standby",   "Auto-standby",    "Auto-standby",   "Auto-standby",    "Auto-standby") \
  X(StandbyOff,    "Aus",            "No",              "Av",             "Off",             "No") \
  X(LblFontSize,   "Schriftgröße",   "Dim. testo",      "Textstorlek",    "Font size",       "Tamaño texto") \
  X(FontSmall,     "Klein",          "Piccolo",         "Liten",          "Small",           "Pequeño") \
  X(FontLarge,     "Groß",           "Grande",          "Stor",           "Large",           "Grande") \
  X(HintEnterSave, "Enter speichert","Enter salva",     "Enter sparar",   "Enter saves",     "Enter guarda") /* max 19 Z. */ \
  X(HintNameEdit,  "Enter: Name ändern", "Enter: cambia nome", "Enter: byt namn", "Enter: edit name", "Enter: renombrar") /* max 19 Z. */ \
  X(HintEdit,      "Enter: eingeben","Enter: modifica", "Enter: redigera","Enter: edit",     "Enter: editar") /* max 19 Z. */ \
  X(LblWifiSsid,   "WLAN-SSID",      "SSID Wi-Fi",      "Wi-Fi-SSID",     "Wi-Fi SSID",      "SSID Wi-Fi") \
  X(LblWifiPass,   "WLAN-Passwort",  "Password Wi-Fi",  "Wi-Fi-lösenord", "Wi-Fi password",  "Clave Wi-Fi") \
  X(HintChange,    "A/D bzw. Tap ändert", "A/D o tap modifica", "A/D el. tap ändrar", "A/D or tap changes", "A/D o tap cambia") /* max 19 Z. */ \
  X(FmtBattery,    "Akku %u%%%s (%u mV)", "Batt. %u%%%s (%u mV)", "Batt. %u%%%s (%u mV)", "Batt. %u%%%s (%u mV)", "Bat. %u%%%s (%u mV)") /* fmt: %u %s %u, max 20 Z. */ \
  /* --- Spiele: Menü --- */ \
  X(Game2048,      "2048",           "2048",            "2048",           "2048",            "2048") \
  X(GameMines,     "Minensucher",    "Campo minato",    "Minröjare",      "Minesweeper",     "Buscaminas") \
  X(GameChess,     "Schach",         "Scacchi",         "Schack",         "Chess",           "Ajedrez") \
  X(GameTtt,       "Tic-Tac-Toe",    "Tris",            "Tre i rad",      "Tic-tac-toe",     "Tres en raya") \
  X(NoGameYet,     "Noch kein Spiel","Nessuna partita", "Inget spel än",  "No game yet",     "Sin partidas") \
  X(FmtBest,       "Best: %lu",      "Record: %lu",     "Bäst: %lu",      "Best: %lu",       "Récord: %lu") /* fmt: %lu */ \
  /* --- 2048 --- */ \
  X(FmtPoints,     "Punkte: %lu",    "Punti: %lu",      "Poäng: %lu",     "Score: %lu",      "Puntos: %lu") /* fmt: %lu */ \
  X(Hint2048,      "WASD schiebt · N=Neu · Backspace=Menü", "WASD muove · N=Nuovo · Backspace=Menu", "WASD flyttar · N=Ny · Backspace=Meny", "WASD moves · N=New · Backspace=Menu", "WASD mueve · N=Nuevo · Backspace=Menú") \
  X(Won2048,       "2048 erreicht!", "2048 raggiunto!", "2048 uppnått!",  "2048 reached!",   "¡2048 logrado!") \
  X(ContinueEnter, "Weiterspielen: Enter", "Continua: Enter", "Fortsätt: Enter", "Keep playing: Enter", "Continuar: Enter") \
  X(GameOver,      "Vorbei!",        "Finita!",         "Slut!",          "Game over!",      "¡Fin!") \
  X(Fmt2048Over,   "Punkte: %lu · N = neues Spiel", "Punti: %lu · N = nuova partita", "Poäng: %lu · N = nytt spel", "Score: %lu · N = new game", "Puntos: %lu · N = nueva partida") /* fmt: %lu */ \
  /* --- Minensucher --- */ \
  X(MinesWin,      "Geschafft!",     "Fatto!",          "Klarat!",        "Cleared!",        "¡Logrado!") \
  X(MinesBoom,     "Boom!",          "Boom!",           "Pang!",          "Boom!",           "¡Bum!") \
  X(FmtMinesTime,  "%lu s · N = neues Spiel", "%lu s · N = nuova partita", "%lu s · N = nytt spel", "%lu s · N = new game", "%lu s · N = nueva partida") /* fmt: %lu */ \
  X(MinesNewGame,  "N = neues Spiel","N = nuova partita","N = nytt spel", "N = new game",    "N = nueva partida") \
  X(FmtMinesLeft,  "Minen: %d",      "Mine: %d",        "Minor: %d",      "Mines: %d",       "Minas: %d") /* fmt: %d */ \
  X(BtnFlag,       "Flagge",         "Bandiera",        "Flagga",         "Flag",            "Bandera") /* 102 px, max 8 */ \
  X(BtnDig,        "Graben",         "Scava",           "Gräv",           "Dig",             "Cavar") /* 102 px */ \
  X(FmtMinesWins,  "Siege: %u  ·  Bestzeit: %u s", "Vittorie: %u  ·  Record: %u s", "Segrar: %u  ·  Bästa: %u s", "Wins: %u  ·  Best: %u s", "Victorias: %u  ·  Récord: %u s") /* fmt: %u %u */ \
  X(NoWinYet,      "Noch kein Sieg", "Nessuna vittoria","Ingen seger än", "No win yet",      "Sin victorias") \
  /* --- Schach --- */ \
  X(ChessNewGame,  "Neue Partie",    "Nuova partita",   "Nytt parti",     "New game",        "Nueva partida") \
  X(FmtOpponent,   "Gegner: %s",     "Avversario: %s",  "Motståndare: %s","Opponent: %s",    "Rival: %s") /* fmt: %s */ \
  X(TwoPlayers,    "2 Spieler",      "2 giocatori",     "2 spelare",      "2 players",       "2 jugadores") \
  X(FmtYourColor,  "Deine Farbe: %s","Il tuo colore: %s","Din färg: %s",  "Your color: %s",  "Tu color: %s") /* fmt: %s */ \
  X(ColorWhite,    "Weiß",           "Bianco",          "Vit",            "White",           "Blanco") \
  X(ColorBlack,    "Schwarz",        "Nero",            "Svart",          "Black",           "Negro") \
  X(FmtLevel,      "Stufe: %s",      "Livello: %s",     "Nivå: %s",       "Level: %s",       "Nivel: %s") /* fmt: %s */ \
  X(LevelEasy,     "Leicht",         "Facile",          "Lätt",           "Easy",            "Fácil") \
  X(LevelMid,      "Mittel",         "Medio",           "Medel",          "Medium",          "Medio") \
  X(LevelHard,     "Schwer",         "Difficile",       "Svår",           "Hard",            "Difícil") \
  X(ChessBackHint, "Backspace = zurück zur Partie", "Backspace = torna alla partita", "Backspace = åter till partiet", "Backspace = back to game", "Backspace = volver a la partida") \
  X(ChessPromo,    "Umwandlung:",    "Promozione:",     "Förvandling:",   "Promotion:",      "Promoción:") \
  X(ChessMateW,    "Matt - Weiß gewinnt!",   "Matto - vince il Bianco!", "Matt - Vit vinner!",   "Mate - White wins!", "Mate - ¡gana Blanco!") \
  X(ChessMateB,    "Matt - Schwarz gewinnt!","Matto - vince il Nero!",   "Matt - Svart vinner!", "Mate - Black wins!", "Mate - ¡gana Negro!") \
  X(ChessStale,    "Patt - Remis",   "Stallo - patta",  "Patt - remi",    "Stalemate - draw","Ahogado - tablas") \
  X(ChessDraw50,   "Remis (50 Züge)","Patta (50 mosse)","Remi (50 drag)", "Draw (50 moves)", "Tablas (50 jugadas)") \
  X(ChessDrawRep,  "Remis (Wiederholung)", "Patta (ripetizione)", "Remi (upprepning)", "Draw (repetition)", "Tablas (repetición)") \
  X(ChessThinking, "Fennek denkt ...","Fennek pensa ...","Fennek tänker ...", "Fennek thinking ...", "Fennek piensa ...") \
  X(FmtChessTurn,  "%s am Zug%s",    "Muove il %s%s",   "%s vid draget%s","%s to move%s",    "Mueve el %s%s") /* fmt: %s %s */ \
  X(ChessCheck,    " - Schach!",     " - scacco!",      " - schack!",     " - check!",       " - ¡jaque!") \
  X(ChessNewHint,  "N = neue Partie","N = nuova partita","N = nytt parti","N = new game",    "N = nueva partida") \
  X(YouWhite,      "Du: Weiß",       "Tu: Bianco",      "Du: Vit",        "You: White",      "Tú: Blanco") \
  X(YouBlack,      "Du: Schwarz",    "Tu: Nero",        "Du: Svart",      "You: Black",      "Tú: Negro") \
  X(ChessKeyHint,  "WASD+Enter oder Tap · N=Neu · Backspace=Menü", "WASD+Enter o tap · N=Nuovo · Backspace=Menu", "WASD+Enter eller tryck · N=Ny · Backspace=Meny", "WASD+Enter or tap · N=New · Backspace=Menu", "WASD+Enter o tap · N=Nuevo · Backspace=Menú") \
  X(FmtChessSaved, "Partie läuft · Siege: %u", "Partita in corso · Vittorie: %u", "Parti pågår · Segrar: %u", "Game running · Wins: %u", "Partida en curso · Victorias: %u") /* fmt: %u */ \
  X(FmtChessWins,  "Siege gegen Fennek: %u", "Vittorie su Fennek: %u", "Segrar mot Fennek: %u", "Wins vs Fennek: %u", "Victorias contra Fennek: %u") /* fmt: %u */ \
  /* --- Tic-Tac-Toe --- */ \
  X(TttXWins,      "X gewinnt!",     "Vince X!",        "X vinner!",      "X wins!",         "¡Gana X!") \
  X(TttOWins,      "O gewinnt!",     "Vince O!",        "O vinner!",      "O wins!",         "¡Gana O!") \
  X(TttYouWin,     "Du gewinnst!",   "Hai vinto!",      "Du vinner!",     "You win!",        "¡Has ganado!") \
  X(TttFennekWins, "Fennek gewinnt!","Vince Fennek!",   "Fennek vinner!", "Fennek wins!",    "¡Gana Fennek!") \
  X(TttDraw,       "Remis!",         "Patta!",          "Remi!",          "Draw!",           "¡Empate!") \
  X(FmtTttTurn,    "%s ist dran",    "Tocca a %s",      "%s:s tur",       "%s's turn",       "Turno de %s") /* fmt: %s */ \
  X(TttYouAreX,    "Du bist X",      "Sei X",           "Du är X",        "You are X",       "Eres X") \
  X(TttVsFennek,   "Gegner: Fennek", "Avversario: Fennek", "Motståndare: Fennek", "Opponent: Fennek", "Rival: Fennek") \
  X(TttTwoPlayers, "2 Spieler am Gerät", "2 giocatori locali", "2 spelare på enheten", "2 players on device", "2 jugadores locales") \
  X(TttKeyHint,    "N=Neu  M=Modus  Backspace=Menü", "N=Nuovo  M=Modo  Backspace=Menu", "N=Ny  M=Läge  Backspace=Meny", "N=New  M=Mode  Backspace=Menu", "N=Nuevo  M=Modo  Backspace=Menú") \
  X(FmtTttWins,    "Siege: %u  ·  Remis: %u", "Vittorie: %u  ·  Patte: %u", "Segrar: %u  ·  Remi: %u", "Wins: %u  ·  Draws: %u", "Victorias: %u  ·  Empates: %u") /* fmt: %u %u */ \
  /* --- Dateien (Web-Dateiverwaltung) --- */ \
  X(FilesTitle,    "Web-Dateiverwaltung", "Gestione file web", "Webbfilhantering", "Web file manager", "Gestor de archivos web") \
  X(FilesNoCreds1, "WLAN ist nicht konfiguriert.", "Wi-Fi non configurato.", "Wi-Fi är inte konfigurerat.", "Wi-Fi is not configured.", "Wi-Fi no configurado.") \
  X(FilesNoCreds2, "In den Optionen eintragen:", "Inserisci nelle opzioni:", "Ange i inställningarna:", "Set it in the settings:", "Introduce en los ajustes:") \
  X(FilesNoCreds3, "oder im Serial-Monitor (USB):", "o nel monitor seriale (USB):", "eller i seriell monitor (USB):", "or in the serial monitor (USB):", "o en el monitor serie (USB):") \
  X(FmtFilesSsid,  "WLAN: %s",       "Wi-Fi: %s",       "Wi-Fi: %s",      "Wi-Fi: %s",       "Wi-Fi: %s") /* fmt: %s */ \
  X(FilesStartHint,"Stoppt Musik, pausiert Mesh", "Ferma la musica, pausa il mesh", "Stoppar musik, pausar mesh", "Stops music, pauses mesh", "Para la música, pausa el mesh") \
  X(FilesConnecting,"Verbinde ...",  "Connessione ...", "Ansluter ...",   "Connecting ...",  "Conectando ...") \
  X(FilesFailed,   "Verbindung fehlgeschlagen", "Connessione fallita", "Anslutning misslyckades", "Connection failed", "Fallo de conexión") \
  X(FilesRetryHint,"Enter = erneut versuchen", "Enter = riprova", "Enter = försök igen", "Enter = try again", "Enter = reintentar") \
  X(FilesBrowserHint,"Im Browser öffnen:", "Apri nel browser:", "Öppna i webbläsaren:", "Open in your browser:", "Abre en el navegador:") \
  X(FmtRequests,   "Anfragen: %lu",  "Richieste: %lu",  "Anrop: %lu",     "Requests: %lu",   "Peticiones: %lu") /* fmt: %lu */ \
  X(FilesStopHint, "Q oder Zurück = Stopp", "Q o Indietro = stop", "Q eller Tillbaka = stopp", "Q or Back = stop", "Q o Atrás = parar")

namespace i18n {

// Reihenfolge = NVS-Index ("lang") — nie umsortieren, nur anhängen!
enum class Lang : uint8_t { DE = 0, IT = 1, SV = 2, EN = 3, ES = 4, COUNT };

enum class Str : uint16_t {
  None = 0,
#define X(id, de, it, sv, en, es) id,
  FENNEK_STRS(X)
#undef X
  COUNT
};
constexpr uint16_t STR_COUNT = (uint16_t)Str::COUNT;

Lang        lang();                 // aktive Sprache (Cache aus settings)
void        setLang(Lang l);        // setzt Cache + persistiert via settings
const char* langName(Lang l);       // Endonym: "Deutsch"/"Italiano"/...
const char* tr(Str id);             // Lookup; leerer Eintrag -> Deutsch

}  // namespace i18n
