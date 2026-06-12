// =============================================================================
// appmgr.h — App-Registry, Lifecycle und Render-Orchestrierung.
//
// Eine App ist ein Screen-Verbund (z. B. Musik = Browser + Player). Der appmgr
// pollt die Eingaben, reicht sie an die aktive App weiter und koalesziert
// E-Ink-Refreshes über ein Dirty-Flag (Modell aus dem alten ui.cpp).
//
// Statuszeile (y 0..STATUS_H-1) zeichnet der appmgr; Apps zeichnen darunter.
// Tap auf die Statuszeile = zurück zum Launcher (Home).
// =============================================================================
#pragma once

#include <Adafruit_GFX.h>
#include "core/input.h"

class App {
 public:
  virtual ~App() {}
  // Stabiler Identifier für NVS-Persistenz/Logs (sprachunabhängig; aus
  // Kompatibilität die ursprünglichen deutschen Namen — nie ändern!).
  virtual const char* id() const = 0;
  // Anzeigename für die Statuszeile (lokalisiert via i18n::tr()).
  virtual const char* name() const = 0;
  virtual void onEnter() {}      // wird Vordergrund; Full-Redraw folgt
  virtual void onLeave() {}      // geht in Hintergrund; Zustand sichern
  virtual void tick() {}         // nur im Vordergrund, pro Loop-Durchlauf
  virtual void background() {}   // JEDER Loop-Durchlauf, auch im Hintergrund
  virtual void handleInput(const InputEvent& e) = 0;
  virtual void draw(Adafruit_GFX& g) = 0;  // Bereich unterhalb der Statuszeile
};

namespace appmgr {

constexpr int STATUS_H  = 22;   // Statuszeile inkl. Trennlinie
constexpr int CONTENT_Y = STATUS_H + 2;

// Apps registrieren; die erste registrierte App ist Home (Launcher).
void add(App* a);

// Nach dem Registrieren aufrufen: betritt Home und rendert den ersten Screen.
void begin();

void launch(App* a);
void goHome();
App* current();

// Voll-Redraw (Statuszeile + App) anfordern; wird im loop() koalesziert.
void markDirty();
bool isDirty();

// Pro Arduino-loop()-Durchlauf aufrufen.
void loop();

}  // namespace appmgr
