/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_interface.h"

#include "data/data_document.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "dialogs/suggestions/suggestion.h"
#include "main/main_session.h"
#include "oblivion/oblivion_settings.h"
#include "ui/text/format_values.h"
#include "window/window_separate_id.h"
#include "window/window_session_controller.h"

namespace Oblivion {
namespace {

using Getter = bool (Settings::*)() const;

[[nodiscard]] rpl::producer<bool> FlagValue(Getter getter) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Get().changes()
	) | rpl::map([=] {
		return (Get().*getter)();
	}) | rpl::distinct_until_changed();
}

[[nodiscard]] rpl::producer<> FlagChanges(Getter getter) {
	return FlagValue(getter) | rpl::skip(1) | rpl::to_empty;
}

// Inserts seconds right after the minutes field of a QLocale time format,
// reusing the separator that stands before the minutes ("H.mm" -> "H.mm.ss").
[[nodiscard]] QString AddSecondsToFormat(const QString &format) {
	const auto size = int(format.size());
	auto quoted = false;
	auto minutesStart = -1;
	auto minutesEnd = -1;
	for (auto i = 0; i != size; ++i) {
		const auto ch = format[i];
		if (ch == QChar('\'')) {
			quoted = !quoted;
		} else if (quoted) {
			continue;
		} else if (ch == QChar('s')) {
			return format; // Already has seconds.
		} else if (ch == QChar('m') && minutesStart < 0) {
			minutesStart = i;
			minutesEnd = i + 1;
			while (minutesEnd < size && format[minutesEnd] == QChar('m')) {
				++minutesEnd;
			}
			i = minutesEnd - 1;
		}
	}
	if (minutesStart < 0) {
		return format;
	}
	const auto before = (minutesStart > 0)
		? format[minutesStart - 1]
		: QChar();
	const auto separator = (before.isNull()
		|| before.isLetterOrNumber()
		|| before.isSpace()
		|| before == QChar('\''))
		? QChar(':')
		: before;
	auto result = format;
	result.insert(minutesEnd, QString(separator) + u"ss"_q);
	return result;
}

[[nodiscard]] QString TimeFormatWithSeconds(const QLocale &locale) {
	static auto CachedShort = QString();
	static auto CachedResult = QString();
	const auto format = locale.timeFormat(QLocale::ShortFormat);
	if (format != CachedShort || CachedResult.isEmpty()) {
		CachedShort = format;
		CachedResult = AddSecondsToFormat(format);
	}
	return CachedResult;
}

} // namespace

QString FormatMessageTime(const QTime &time) {
	const auto locale = QLocale();
	return Get().showSeconds()
		? locale.toString(time, TimeFormatWithSeconds(locale))
		: locale.toString(time, QLocale::ShortFormat);
}

QString FormatDateTimeSavedFrom(const QDateTime &dateTime) {
	auto result = Ui::FormatDateTimeSavedFrom(dateTime);
	if (!Get().showSeconds()) {
		return result;
	}
	const auto time = dateTime.time();
	const auto shortTime = QLocale().toString(time, QLocale::ShortFormat);
	const auto index = shortTime.isEmpty()
		? -1
		: result.lastIndexOf(shortTime);
	if (index >= 0) {
		result.replace(index, shortTime.size(), FormatMessageTime(time));
	}
	return result;
}

rpl::producer<> ShowSecondsChanges() {
	return FlagChanges(&Settings::showSeconds);
}

rpl::producer<> HideStoriesBarChanges() {
	return FlagChanges(&Settings::hideStoriesBar);
}

rpl::producer<bool> HidePremiumPromoValue() {
	return FlagValue(&Settings::hidePremiumPromo);
}

rpl::producer<bool> HideGiftPromoValue() {
	return FlagValue(&Settings::hideGiftPromo);
}

rpl::producer<bool> GiftPromoShownValue() {
	return HideGiftPromoValue() | rpl::map(!rpl::mappers::_1);
}

rpl::producer<bool> PremiumUpsellShownValue(
		not_null<Main::Session*> session) {
	return rpl::combine(
		HidePremiumPromoValue(),
		Data::AmPremiumValue(session)
	) | rpl::map([](bool hide, bool premium) {
		return !hide || premium;
	}) | rpl::distinct_until_changed();
}

rpl::producer<> PromoVisibilityChanges() {
	return rpl::merge(
		FlagChanges(&Settings::hidePremiumPromo),
		FlagChanges(&Settings::hideGiftPromo));
}

void FilterTopBarSuggestions(
		std::vector<Dialogs::TopBarSuggestions::Spec> &specs) {
	using namespace Dialogs::TopBarSuggestions;
	for (auto &spec : specs) {
		const auto premium = (spec.priority == Priority::PremiumOffer)
			|| (spec.priority == Priority::PremiumGrace);
		const auto gift = (spec.priority == Priority::BirthdayContacts);
		if ((!premium && !gift) || !spec.available) {
			continue;
		}
		spec.available = [=, original = std::move(spec.available)](
				const Context &context) {
			if (premium && Get().hidePremiumPromo()) {
				return false;
			} else if (gift && Get().hideGiftPromo()) {
				return false;
			}
			return original(context);
		};
	}
}

bool SkipFeaturedEmojiSet(not_null<Data::Session*> owner, uint64 setId) {
	if (!Get().hidePremiumPromo() || owner->session().premium()) {
		return false;
	}
	const auto &sets = owner->stickers().sets();
	const auto i = sets.find(setId);
	if (i == end(sets)) {
		return false;
	}
	const auto set = i->second.get();
	if (set->flags & Data::StickersSetFlag::Installed) {
		return false;
	}
	for (const auto document : set->stickers) {
		if (document->isPremiumEmoji()) {
			return true;
		}
	}
	return false;
}

bool SkipPremiumStickers(not_null<Main::Session*> session) {
	return !session->premiumPossible()
		|| (Get().hidePremiumPromo() && !session->premium());
}

Window::SessionController *ExistingWindow(
		not_null<Main::Session*> session,
		PeerData *peer) {
	auto result = (Window::SessionController*)nullptr;
	for (const auto &window : session->windows()) {
		const auto thread = window->windowId().thread;
		if (peer && thread && thread->peer() == peer) {
			return window;
		} else if (!result || window->isPrimary()) {
			result = window;
		}
	}
	return result;
}

} // namespace Oblivion
