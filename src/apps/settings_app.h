// =============================================================================
// settings_app.h — Einstellungen als App.
//
// Funk-Parameter (Preset EU Narrow / EU Klassisch oder einzeln: Frequenz,
// Bandbreite, SF, CR, Sendeleistung), Node-Name und Geräte-Infos. Änderungen
// werden sofort im NVS gespeichert und — falls das Mesh läuft — live aufs
// Radio angewandt (mesh_client::applyRadioParams()).
// =============================================================================
#pragma once

#include "core/appmgr.h"

namespace settings_app {

App* get();

}  // namespace settings_app
