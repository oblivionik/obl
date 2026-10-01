/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Oblivion {

struct LangOverride {
	const char *key = nullptr;
	const char *value = nullptr;
};

[[nodiscard]] const std::vector<LangOverride> &RussianStrings();

[[nodiscard]] bool IsRussianLanguage(
	const QString &id,
	const QString &baseId,
	const QString &pluralId,
	const QString &nativeName);
[[nodiscard]] bool CurrentLanguageIsRussian();

} // namespace Oblivion
