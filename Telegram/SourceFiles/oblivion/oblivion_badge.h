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
// There is no server for it. A user who switches the badge on (asked
// once per account, with a plain explanation) gets a short invisible
// marker of zero-width characters appended to the bio ("about") of that
// account, after a space, so that a link at the end of the bio still
// ends where it did. Every Oblivion client that reads a profile with
// the marker shows the badge next to that name and remembers the user
// id on this device, so the badge is then shown in the chats list, the
// top bar and the profile without any new requests. The marker never
// gets into the bio the app keeps and shows: it is cut off when a
// profile arrives and put back when the own bio is saved.
//
// The badge is switched for one account, the one of the window where the
// toggle was clicked: a bio belongs to an account, and so does the
// consent to change it. Oblivion::Get().badgeEnabled() only mirrors "it
// is on for some account of this installation".
//
// Guard rails:
//  - the bio is changed only after an explicit consent;
//  - a bio without a visible text never gets the marker: a bio made of
//    invisible characters alone is never written, the user is asked to
//    write something first;
//  - at most one profile update per account per launch;
//  - the bio is read from the server right before it is changed, so a
//    stale local copy can never replace the text of the user;
//  - the value is read back, if the server has dropped the marker the
//    feature switches itself off and says so (what the server has kept
//    of the marker is cleaned up in the next launch);
//  - switching the badge off removes the marker (in the next launch, if
//    the update of this launch was already spent);
//  - a badge the user has switched off is never switched on by the app:
//    while its marker could not be removed yet the badge stays off, and
//    the user is told if the removal keeps failing; that the marker is
//    still to remove is the one thing remembered after a logout, so
//    signing in again removes it instead of taking it for a new badge;
//  - a bio the user empties while rewriting it does not switch the badge
//    off: it is saved empty, the next text takes the marker again;
//  - a wait Telegram asks for (FLOOD_WAIT) is kept between the launches,
//    nothing is sent before it is over and it is not taken for a failure;
//  - nothing is ever repeated in a loop, a failed request is not retried;
//  - no request is sent to learn about the badge of somebody else, only
//    the profiles the client loads anyway are looked at.
//
// The mark is a small vector shape in the accent colour, painted in code:
// a rounded diamond with a round hole. It must stay clearly different
// from the verified check of Telegram: it is not a verification and must
// not look like one.
namespace Oblivion::Badge {

// Whether this peer is known to carry the badge. The own account has it
// while the badge is on for that account. Cheap (a lookup in a set kept
// in memory), sends nothing, main thread only.
[[nodiscard]] bool Has(not_null<PeerData*> peer);

// Fires on the main thread (from the event loop, never from inside the
// code that applies an update) after the result of Has() could have
// changed for any peer: the names that show the badge are to be repainted.
[[nodiscard]] rpl::producer<> Changes();

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
// the user saves the bio. With the badge on the marker is appended again,
// if it fits the length limit; if it does not, the text of the user is
// sent as it is and the badge switches itself off with a notice. A bio
// with no visible text is sent as it is too, but the badge stays on and
// waits for the next text (the user is told if the bio stays empty; a
// bio still empty in the next launch switches the badge off). With the
// badge off the text is returned unchanged.
[[nodiscard]] QString BioForSaving(
	not_null<Main::Session*> session,
	const QString &text);

// The second hook of ApiWrap::saveSelfBio(): the request with the text of
// BioForSaving() was answered, the bio is saved or the server refused it.
void BioSaveFinished(not_null<Main::Session*> session);

// The hook of the bio field in Settings: how many characters of the
// length limit the marker takes for this account (0 with the badge off).
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

// Whether the badge is on for the account of this session, and the same
// as a value for the toggle in Settings > Oblivion.
[[nodiscard]] bool Enabled(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<bool> EnabledValue(
	not_null<Main::Session*> session);

// The consent and enable flow, the only way the badge is switched:
// Settings > Oblivion > Interface > "Oblivion badge" calls it and never
// writes a setting itself. It works for the account of this window.
//
// enabled == true: the first time for this account a box explains in
// plain words what will be done to the bio and asks for the consent;
// nothing is changed if it is declined. After that the bio is read from
// the server, the marker is appended (if it fits the bio length limit,
// otherwise a box says how much room is missing; an empty bio is left
// empty and a box asks to write something first) and read back.
//
// enabled == false: the marker is removed from the bio. The badge is off
// from that click on, whatever happens to the request.
//
// The requests go one after another and the result is told by a toast
// or a box; a click while they are on the way only shows a toast.
void SetEnabled(
	not_null<Window::SessionController*> controller,
	bool enabled);

// Called by Main::Session: once after the session is set up (reads the
// small file of this account with the known users) and from
// finishLogout() (forgets everything and deletes that file; only for a
// badge that was switched off with its marker still in the bio a record
// of that alone is left, see the guard rails above).
void Start(not_null<Main::Session*> session);
void Forget(not_null<Main::Session*> session);

// Self-checks for OBLIVION_SELFTEST=badge, see oblivion_selftest.h.
// No Core::App(), no session, no network: pure logic only (the marker
// in a bio text, the length limits, the store of the known users, the
// decisions of the flows, what is left of a marker, the shape of the
// mark).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Badge
