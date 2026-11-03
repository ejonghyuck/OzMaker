#pragma once
#include <string>
#include <vector>

// The language the UI is in (설정 › 일반 › 언어, #26). The code holds the Korean; tr() gives a text in the UI's
// language, looked up whole. A format string's English has the same % fields in the same order (tests check it).
enum class Lang { Korean, English };

void SetLanguage(Lang lang);
Lang Language();
Lang SystemLanguage();  // Korean when Windows shows Korean, else English
const wchar_t* tr(const wchar_t* ko);

// Tests: the Korean texts asked for in English that have no English (each once, since the language was set), and
// the pairs whose % fields differ.
std::vector<std::wstring> MissingTranslations();
std::vector<std::wstring> MismatchedTranslations();
size_t TranslationCount();
