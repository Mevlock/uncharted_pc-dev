// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game menu's language: the launcher's (BB_UI_LANGUAGE: en, es, pt, fr, de, it or ja), read once.
// The translations are edited in gpu/shim/overlay_i18n.py; tools/make_overlay_i18n.py writes them to
// bbport_overlay_i18n.inc (checked in) and checks that every text marked here has all six languages.
#pragma once

namespace BbI18n {

// "en", "es", "pt", "fr", "de", "it" or "ja".
const char* Language();
// The text in the menu's language; English (the text itself) when it has no translation.
const char* T(const char* english);

} // namespace BbI18n

// Marks an English text for translation where it is defined (an array of names): T() translates it where it is
// shown. tools/make_overlay_i18n.py collects the texts in T("..."), TI("...") and N_("...").
#define N_(text) text
