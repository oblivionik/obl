/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Data {
class Session;
} // namespace Data

namespace Dialogs::TopBarSuggestions {
struct Spec;
} // namespace Dialogs::TopBarSuggestions

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

class PeerData;

namespace Oblivion {

// Message time: "HH:MM" or "HH:MM:SS" when showSeconds() is enabled,
// the 12/24h style of the current locale is kept.
[[nodiscard]] QString FormatMessageTime(const QTime &time);
// Ui::FormatDateTimeSavedFrom() with seconds when showSeconds() is enabled.
[[nodiscard]] QString FormatDateTimeSavedFrom(const QDateTime &dateTime);
// Fires when showSeconds() is toggled.
[[nodiscard]] rpl::producer<> ShowSecondsChanges();

// Fires when hideStoriesBar() is toggled.
[[nodiscard]] rpl::producer<> HideStoriesBarChanges();

// Current value + changes of hidePremiumPromo() / hideGiftPromo().
[[nodiscard]] rpl::producer<bool> HidePremiumPromoValue();
[[nodiscard]] rpl::producer<bool> HideGiftPromoValue();

// For settings rows / slide wraps: "true" while the promo may be shown.
[[nodiscard]] rpl::producer<bool> GiftPromoShownValue();
// Premium upsell is hidden only for users without real Premium,
// so real subscribers keep access to their subscription page.
[[nodiscard]] rpl::producer<bool> PremiumUpsellShownValue(
	not_null<Main::Session*> session);

// Fires when hidePremiumPromo() or hideGiftPromo() is toggled.
[[nodiscard]] rpl::producer<> PromoVisibilityChanges();

// Hides premium / birthday gift suggestion bars above the chats list.
void FilterTopBarSuggestions(
	std::vector<Dialogs::TopBarSuggestions::Spec> &specs);

// Featured (not installed) premium emoji sets in the emoji panel.
[[nodiscard]] bool SkipFeaturedEmojiSet(
	not_null<Data::Session*> owner,
	uint64 setId);

// Replaces "!session->premiumPossible()" in the stickers panel: locked
// premium stickers are hidden for users without Premium.
[[nodiscard]] bool SkipPremiumStickers(not_null<Main::Session*> session);

// A window to show a toast or a box of the session in, preferably the one
// with the chat of this peer open. Unlike Main::Session::tryResolveWindow()
// it never activates the account, so background work of an inactive
// account gets nullptr instead of switching the app to that account.
[[nodiscard]] Window::SessionController *ExistingWindow(
	not_null<Main::Session*> session,
	PeerData *peer = nullptr);

} // namespace Oblivion
