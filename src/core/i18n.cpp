#include "i18n.h"
#include "settings.h"

namespace {

// Tabellen aus der X-Macro-Liste: Index = (uint16_t)Str, Eintrag 0 = Str::None.
const char* const kDE[] = {
  "",
#define X(id, de, it, sv, en, es) de,
  FENNEK_STRS(X)
#undef X
};
const char* const kIT[] = {
  "",
#define X(id, de, it, sv, en, es) it,
  FENNEK_STRS(X)
#undef X
};
const char* const kSV[] = {
  "",
#define X(id, de, it, sv, en, es) sv,
  FENNEK_STRS(X)
#undef X
};
const char* const kEN[] = {
  "",
#define X(id, de, it, sv, en, es) en,
  FENNEK_STRS(X)
#undef X
};
const char* const kES[] = {
  "",
#define X(id, de, it, sv, en, es) es,
  FENNEK_STRS(X)
#undef X
};
static_assert(sizeof(kDE) / sizeof(kDE[0]) == i18n::STR_COUNT, "DE-Tabelle unvollständig");
static_assert(sizeof(kIT) / sizeof(kIT[0]) == i18n::STR_COUNT, "IT-Tabelle unvollständig");
static_assert(sizeof(kSV) / sizeof(kSV[0]) == i18n::STR_COUNT, "SV-Tabelle unvollständig");
static_assert(sizeof(kEN) / sizeof(kEN[0]) == i18n::STR_COUNT, "EN-Tabelle unvollständig");
static_assert(sizeof(kES) / sizeof(kES[0]) == i18n::STR_COUNT, "ES-Tabelle unvollständig");

const char* const* const kTables[] = { kDE, kIT, kSV, kEN, kES };
static_assert(sizeof(kTables) / sizeof(kTables[0]) == (uint8_t)i18n::Lang::COUNT,
              "kTables passt nicht zu Lang::COUNT");

const char* const kLangNames[] = { "Deutsch", "Italiano", "Svenska", "English", "Español" };

}  // namespace

namespace i18n {

Lang lang() {
  uint8_t v = settings::language();
  return (v < (uint8_t)Lang::COUNT) ? (Lang)v : Lang::DE;
}

void setLang(Lang l) {
  settings::setLanguage((uint8_t)l);
}

const char* langName(Lang l) {
  return ((uint8_t)l < (uint8_t)Lang::COUNT) ? kLangNames[(uint8_t)l] : kLangNames[0];
}

const char* tr(Str id) {
  uint16_t i = (uint16_t)id;
  if (i >= STR_COUNT) return "";
  const char* s = kTables[(uint8_t)lang()][i];
  return (s && s[0]) ? s : kDE[i];   // leerer Eintrag -> Deutsch-Fallback
}

}  // namespace i18n
