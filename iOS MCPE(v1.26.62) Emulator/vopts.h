// OptiFine-like video settings (Settings > Video and its "..." screens, guest/EmuShaders): their values, saved in
// <data>/optionsof.txt ("key:value", the value an index into the option's choices), and the game's language for
// their labels.
#pragma once
#include <string>

namespace vopts {

void init(const std::wstring& root);
// the value of an option (its default when not set; unknown keys: -1)
int get(const char* key);
int count(const char* key);  // its number of choices (unknown: 0)
void set(const char* key, int value);
void reset_all();            // "Reset Video Settings"
// increases each time a value changes (the users of the values check it to apply them)
unsigned generation();
// the game's language, e.g. "ja_JP": the one the game uses now (set_language, from guest/EmuShaders), else options.txt's
// "game_language" (re-read at most once a second)
std::string language();
void set_language(const std::string& code);

}  // namespace vopts
