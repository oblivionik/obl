/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

class PeerData;
class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// The Oblivion badge: a small mark next to the names of the people who
// use Oblivion and have agreed to show it.
//
// Since round 5 the badge comes from Oblivion Cloud: a user who switches
// «Значок Oblivion» on (Settings > Oblivion, after the consent to the
// cloud for that account) is put into the public badge list of the
// server. Every client downloads that list as a whole, keeps it on disk
// and refreshes it in the background (see oblivion_cloud_social.h); the
// ids of the contacts and the chats of the user are never sent anywhere.
//
// In round 4 there was no server and the badge was a short invisible
// marker of zero-width characters at the end of the bio ("about"). What
// is left of that:
//  - the marker is never written any more, and nothing is reserved for
//    it in the length limit of the bio;
//  - a marker in the bio of somebody else is still recognized (an older
//    Oblivion has written it): the badge is shown for that user and the
//    id is remembered on this device. The marker never gets into the bio
//    the app keeps and shows: it is cut off when a profile arrives;
//  - a marker in the own bio is only noticed: Settings > Oblivion then
//    offers «Убрать старую метку из «О себе»», and the bio is changed
//    only by that click. Saving the bio in Settings removes it as well,
//    because the bio is saved exactly as it was typed.
//
// Guard rails of that removal (the only change Oblivion makes to a
// profile):
//  - at most one profile update per account per launch;
//  - the bio is read from the server right before it is changed, so a
//    stale local copy can never replace the text of the user;
//  - if the removal could not be done right away (the update of this
//    launch was spent, Telegram asks to wait, no connection), it is done
//    in the next launch: the user has asked for it by the click, and is
//    told so; that the marker is still to remove is the one thing
//    remembered after a logout;
//  - a wait Telegram asks for (FLOOD_WAIT) is kept between the launches,
//    nothing is sent before it is over and it is not taken for a failure;
//  - nothing is ever repeated in a loop, a failed request is not retried;
//  - no request is sent to learn about the badge of somebody else.
//
// The mark is a small vector shape in the accent colour, painted in code:
// a rounded diamond with a round hole. It must stay clearly different
// from the verified check of Telegram: it is not a verification and must
// not look like one.
namespace Oblivion::Badge {

// Whether this peer is known to carry the badge: the id is in the badge
// list of Oblivion Cloud, or a marker was seen in the bio. The own
// account has it while its badge is published (or while the old marker
// is still in its bio). The badges of other people are shown only with
// «Показывать значки Oblivion у других» on. Cheap (lookups in sets kept
// in memory), sends nothing, main thread only.
[[nodiscard]] bool Has(not_null<PeerData*> peer);

// Fires on the main thread (from the event loop, never from inside the
// code that applies an update) after the result of Has() could have
// changed for any peer: the names that show the badge are to be repainted.
// Right before it the main windows of the accounts are asked to repaint
// as a whole: the window and, each by itself, every visible widget inside
// it that Qt leaves out of an update of its parent (the lists that paint
// opaquely, the chats list first of all). So a list in a main window that
// paints names through Ui::PeerBadge needs no subscription of its own; a
// widget that keeps something computed from Has() (a width, a cached
// text) or lives in another window still has to subscribe.
[[nodiscard]] rpl::producer<> Changes();

// The badge list of the cloud (or the setting that shows it) has changed:
// Changes() fires soon. Called by oblivion_cloud_social.cpp.
void Refresh();

// The hook of Data::ApplyUserUpdate(): called on the main thread with
// the "about" text of a user as the server has sent it. Looks for the
// marker, remembers or forgets the user and returns the text without the
// marker, which is what the rest of the app keeps and shows. For the own
// account it also notices that the marker has appeared or disappeared
// while Oblivion did not change the bio itself.
[[nodiscard]] QString AboutLoaded(
	not_null<UserData*> user,
	const QString &about);

// A bio text without the marker and nothing else: nobody is remembered
// or forgotten and the own badge is not looked at. For the places that
// get the bio of a user aside from Data::ApplyUserUpdate(): a participant
// of a group call, the own bio after it was saved. A text without the
// marker is returned as it is.
[[nodiscard]] QString StripAbout(const QString &about);

// The hook of ApiWrap::saveSelfBio(): the text that is really sent when
// the user saves the bio. It is the text of the user (a marker that got
// into it somehow is cut off): nothing is appended any more.
[[nodiscard]] QString BioForSaving(
	not_null<Main::Session*> session,
	const QString &text);

// The second hook of ApiWrap::saveSelfBio(): the request with the text of
// BioForSaving() was answered, the bio is saved or the server refused it.
void BioSaveFinished(not_null<Main::Session*> session);

// The hook of the bio field in Settings: how many characters of the
// length limit Oblivion takes. Always 0 since the marker is not written.
[[nodiscard]] int ReservedBioLength(not_null<UserData*> self);

// Paints the mark into rect (any size, it is a vector shape that takes
// the largest centered square) with the given colour: the accent colour
// of the place, on a selected row of the chats list the colour its
// verified icon has there.
void Paint(QPainter &p, QRect rect, QColor color);

// The width PaintAfterName() takes in the row of a name for a peer with
// the badge: the mark and the skip before it.
[[nodiscard]] int Width();

// Width() if the peer has the badge and a name place of this width has
// the room for it, 0 otherwise.
[[nodiscard]] int WidthFor(not_null<PeerData*> peer, int available);

// The paint helper for the rows that paint a name themselves (the chats
// list, the top bar of a chat, a row of a peers list). Works the way
// Ui::PeerBadge::drawGetWidth() does: the mark goes right after
// nameWidth pixels from the left of rectForName, or to its right edge
// if the name is longer than that, and is centered in its height.
// outerWidth is the width of the widget (for the right-to-left layout).
//
// Returns the width to take away from the place left for the name:
// Width() if the mark was painted, 0 if the peer has no badge.
//
// Ui::PeerBadge::drawGetWidth() calls it itself, after the upstream
// badges, so the code that paints names through Ui::PeerBadge needs
// nothing more.
int PaintAfterName(
	QPainter &p,
	not_null<PeerData*> peer,
	QRect rectForName,
	int nameWidth,
	int outerWidth,
	QColor color);

// The badge as a widget, for the profile: of a fixed size (the skip
// before the mark is a part of it), shown while the peer has the badge
// and hidden otherwise (it follows Changes() by itself), with a tooltip
// that says what the mark means. The caller places it after the name and
// listens to its shownValue(). Never returns nullptr.
[[nodiscard]] object_ptr<Ui::RpWidget> CreateWidget(
	not_null<QWidget*> parent,
	not_null<PeerData*> peer);

// The colour of a widget made by CreateWidget(), for a profile cover
// with a background of its own; std::nullopt returns the accent colour.
void SetWidgetColor(
	not_null<Ui::RpWidget*> widget,
	std::optional<QColor> color);

// Whether the bio of the account of this session is known to still have
// the old marker: Settings > Oblivion shows «Убрать старую метку из
// «О себе»» while it does. (The switch «Значок Oblivion» itself is
// Oblivion::Social::FlagValue(session, Social::Flag::Badge).)
[[nodiscard]] rpl::producer<bool> OldMarkerValue(
	not_null<Main::Session*> session);

// The click on that button, the only way Oblivion changes the bio: the
// bio is read from the server, the marker is cut off and the text of the
// user is written back. The result is told by a toast; if it can't be
// done now the marker is removed the next time Oblivion starts with this
// account, and the toast says so. A click while the requests are on the
// way only shows a toast.
void RemoveOldMarker(not_null<Window::SessionController*> controller);

// Called by Main::Session: once after the session is set up (reads the
// small file of this account with the known users) and from
// finishLogout() (forgets everything and deletes that file; only for a
// marker the user has asked to remove and that is still in the bio a
// record of that alone is left, see the guard rails above).
void Start(not_null<Main::Session*> session);
void Forget(not_null<Main::Session*> session);

// Self-checks for OBLIVION_SELFTEST=badge, see oblivion_selftest.h.
// No Core::App(), no session, no network: pure logic only (the marker
// in a bio text, the length limits, the store of the known users, the
// decisions of the flows, what is left of a marker, the shape of the
// mark).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Badge
