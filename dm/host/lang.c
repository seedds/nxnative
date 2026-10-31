// The Switch's language codes in nn::settings::Language order: the table the settings service answers with (hle_sdk16.c makes and compares LanguageCodes from it) and the one the
// game language default chooses from (hle_oe.c). Kept in its own file so every reader shares one copy without pulling in a service implementation: the host unit tests link
// this file alone when they only need the codes.
// A LanguageCode is char[8] on the device (padded with NULs), which these strings are.
#include "hle.h"

const char *const dm_lang_code[DM_LANG_COUNT] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
