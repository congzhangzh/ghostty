#include "host.h"

// ---------------------------------------------------------------------------
// The host's text in the Windows display language (the owner: "en/ja/fr/zh
// for different country and en as fall back")
//
// zh-CN, zh-TW, ja and fr from i18n_table.h (windows/i18n/gen.py makes it:
// the command palette's strings with Ghostty's own translations from po/,
// the host's own strings machine-made and marked for review); English
// otherwise, and for anything the table lacks. The motto stays English, as
// in pys. GHOSTTY_LANG (en, zh, zh-TW, ja, fr) overrides the language
// (tests); GetUserDefaultUILanguage decides otherwise.

static const struct {
  const WCHAR* en;
  const WCHAR* t[4];  // zh-CN, zh-TW, ja, fr
} kTable[] = {
#include "i18n_table.h"
};

static int lang_index(void) {
  static int idx = -2;
  if (idx != -2) return idx;
  idx = -1;
  const char* v = getenv("GHOSTTY_LANG");
  if (v && *v) {
    if (_stricmp(v, "zh") == 0 || _stricmp(v, "zh-CN") == 0) idx = 0;
    else if (_stricmp(v, "zh-TW") == 0) idx = 1;
    else if (_stricmp(v, "ja") == 0) idx = 2;
    else if (_stricmp(v, "fr") == 0) idx = 3;
    return idx;
  }
  LANGID id = GetUserDefaultUILanguage();
  WORD primary = PRIMARYLANGID(id), sub = SUBLANGID(id);
  if (primary == LANG_CHINESE)
    idx = (sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU) ? 1 : 0;
  else if (primary == LANG_JAPANESE) idx = 2;
  else if (primary == LANG_FRENCH) idx = 3;
  return idx;
}

// The text in the display language, or `en` itself.
const WCHAR* tr(const WCHAR* en) {
  int i = lang_index();
  if (i < 0 || !en) return en;
  for (size_t k = 0; k < sizeof(kTable) / sizeof(kTable[0]); k++)
    if (wcscmp(kTable[k].en, en) == 0) return kTable[k].t[i] ? kTable[k].t[i] : en;
  return en;
}

// For the test pipe: the language in use.
const char* tr_lang(void) {
  static const char* const names[] = {"zh-CN", "zh-TW", "ja", "fr"};
  int i = lang_index();
  return i < 0 ? "en" : names[i];
}
