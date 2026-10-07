/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "oblivion/oblivion_room.h"
#include "ui/abstract_button.h"

namespace Ui {
class PopupMenu;
class RpWidget;
class Show;
} // namespace Ui

// Round 5: rooms. The window of a room (header, members strip, tabs) and
// the boxes around it. The entry points other modules call are declared
// in oblivion_room.h and implemented here.
//
// THE REGISTRATION INTERFACE (for the «Видео» tab, the «Холст» tab, the
// reactions overlay, the «Голос» button):
//
//   // At the bottom of your own .cpp, in the anonymous namespace:
//   const auto VideoTab = Rooms::TabRegistrar([] {
//       return Rooms::TabDescriptor{
//           .id = u"video"_q,
//           .order = 200,
//           .title = [] { return tr::lng_oblivion_rvideo_tab(tr::now); },
//           .create = [](QWidget *parent, Rooms::TabContext context) {
//               return object_ptr<Ui::RpWidget>::fromRaw(
//                   Ui::CreateChild<VideoTab>(parent, std::move(context)));
//               // or object_ptr<VideoTab>(parent, std::move(context))
//           },
//       };
//   });
//
// The registrar only stores the lambda during the static initialization,
// it is called when the first room window (or snapshot scene) is built.
//
//  - A tab is created when it is first shown and lives till the window
//    is closed. The window gives it the whole area under the tabs strip
//    (setGeometry), it paints its own background over st::windowBg, or
//    calls PaintRoomGround() below to lie on the page of the look («Тема
//    Oblivion») the way the tabs of the rooms themselves do.
//    context.active tells when it is the visible one (pause what should
//    not run in the background).
//  - An overlay is created with the window above every tab and has the
//    same geometry. It must be transparent for the mouse where it shows
//    nothing (Qt::WA_TransparentForMouseEvents).
//  - A header button is created with the window, the header puts it to
//    the left of the menu button and follows its width and visibility
//    (hide it while it has nothing to offer).
//  - Menu items are added to the «⋯» menu of the window each time it is
//    shown.
//  - context.room is never null and outlives the widget; with a sample
//    room (room->sample()) nothing may be sent or played: the snapshot
//    scenes build the window that way. context.show shows boxes and
//    toasts inside the room window.
//  - Tabs of the rooms themselves: "music" 100, "chat" 400, "members"
//    500. Suggested: "video" 200, "canvas" 300.
namespace Oblivion::Rooms {

struct TabContext {
	not_null<Room*> room;
	std::shared_ptr<Ui::Show> show;
	rpl::producer<bool> active;
	Fn<void(const QString &id)> switchTo;
};

struct TabDescriptor {
	QString id;
	int order = 0;
	Fn<QString()> title;
	Fn<object_ptr<Ui::RpWidget>(QWidget *parent, TabContext context)> create;
};

struct OverlayDescriptor {
	QString id;
	Fn<object_ptr<Ui::RpWidget>(QWidget *parent, TabContext context)> create;
};

struct HeaderButtonDescriptor {
	QString id;
	int order = 0;
	Fn<object_ptr<Ui::RpWidget>(QWidget *parent, TabContext context)> create;
};

struct MenuDescriptor {
	Fn<void(not_null<Ui::PopupMenu*> menu, TabContext context)> fill;
};

class TabRegistrar final {
public:
	explicit TabRegistrar(Fn<TabDescriptor()> make);
};

class OverlayRegistrar final {
public:
	explicit OverlayRegistrar(Fn<OverlayDescriptor()> make);
};

class HeaderButtonRegistrar final {
public:
	explicit HeaderButtonRegistrar(Fn<HeaderButtonDescriptor()> make);
};

class MenuRegistrar final {
public:
	explicit MenuRegistrar(Fn<MenuDescriptor()> make);
};

// ---- The window.

// Opens the window of a room the account has just joined or created
// (state is ParseRoom() of "room" of POST .../join or POST /v1/rooms).
// One room window per account: another one is closed first (the user
// stays a member of that room, he is only offline there).
void ShowRoomWindow(not_null<Main::Session*> session, RoomState &&state);

// The room whose window this account has open, nullptr if none.
[[nodiscard]] Room *ActiveRoom(not_null<Main::Session*> session);

// The content of the window, also for the snapshot scenes (with a sample
// room). tab: the id of the tab to show first, empty for the first one.
[[nodiscard]] object_ptr<Ui::RpWidget> CreateRoomWidget(
	QWidget *parent,
	not_null<Room*> room,
	std::shared_ptr<Ui::Show> show,
	const QString &tab = QString());

// ---- A small painting kit shared by the room tabs (icons are painted in
// code, in the colours of the current theme).

enum class Glyph {
	Play,
	Pause,
	Previous,
	Next,
	Repeat,
	RepeatOne,
	Plus,
	More,
	Volume,
	Mute,
	Link,
	Cross,
	Send,
	Note,
	Crown,
	Back,
};

void PaintGlyph(
	QPainter &p,
	Glyph glyph,
	QRectF rect,
	const QColor &color);

// A round userpic with the initials of the name, coloured by the id.
// With «Тишина» (see the looks below) it is a rounded square.
void PaintUserpic(
	QPainter &p,
	QRect rect,
	uint64 userId,
	const QString &name);
// The outline PaintUserpic() fills, with the pen and the brush that are
// set: for a ring around a userpic or a «+N» that stands in a row of them.
void PaintUserpicShape(QPainter &p, QRectF rect);

// A cover of a track: the image, or a gradient made of the seed with
// a note on it while there is none.
void PaintCover(
	QPainter &p,
	QRect rect,
	const QImage &cover,
	const QString &seed,
	int radius);
// The radius PaintCover() really uses («Тишина» has almost square
// covers), for what is painted over a cover.
[[nodiscard]] int CoverRadius(int radius);

// ---- The looks («Тема Oblivion», oblivion_look.h) in the room window.
// With the plain look each of these paints or returns exactly what the
// window has always had, so a tab that uses them changes nothing by
// default. A tab repaints by itself when the look changes (the window
// updates everything inside); a layout that uses the values below is
// redone on Oblivion::Look::Updates().

// The page behind a widget of the room window (at any depth): the window
// background with the plain look, otherwise the ground of the look, laid
// out in the whole window, so the glow of «Ночной эфир» goes on from one
// widget to the next.
void PaintRoomGround(
	QPainter &p,
	not_null<const QWidget*> widget,
	QRect clip);

// «Родной, но лучше» and «Ночной эфир» put the content of a tab into
// cards that lie on the ground.
[[nodiscard]] bool RoomHasCards();
// From the edge of a tab to a card, 0 without cards.
[[nodiscard]] int RoomCardMargin();
// From the edge of a tab to the content (inside a card when there are
// cards).
[[nodiscard]] int RoomContentPadding();
// Paints nothing without cards or for an empty rect.
void PaintRoomCard(QPainter &p, QRect rect);

// What lies under the mouse: a row (a rounded rect) or a round button.
[[nodiscard]] QColor RoomHoverColor(bool down = false);
void PaintRoomHover(QPainter &p, QRectF rect, bool down = false);

// A section label («Очередь», «Участники») in a line as high as
// st::semiboldFont: with the plain look in the "plain" colour, with
// «Ночной эфир» and «Тишина» small and in capitals. Returns the width of
// the text as it was painted.
int PaintRoomLabel(
	QPainter &p,
	int left,
	int top,
	const QString &text,
	int width,
	const QColor &plain);

// A pill with a text on it (a preset of rights) and the colour of that
// text.
void PaintRoomPill(QPainter &p, QRectF rect, bool over);
[[nodiscard]] QColor RoomPillTextColor();

// A round button with a glyph. accent: filled with the active colour.
class GlyphButton final : public Ui::AbstractButton {
public:
	GlyphButton(QWidget *parent, Glyph glyph, int size, bool accent = false);

	void setGlyph(Glyph glyph);
	void setHighlighted(bool highlighted); // The glyph in the active colour.
	void setDimmed(bool dimmed); // Looks disabled (no right), still clicks.
	// The main button of a tab («Play»). It matters only with «Ночной
	// эфир»: the main accent button is the white circle there, the other
	// accent buttons get the gradient.
	void setPrimary(bool primary);

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	Glyph _glyph = Glyph::Play;
	bool _accent = false;
	bool _highlighted = false;
	bool _dimmed = false;
	bool _primary = false;

};

} // namespace Oblivion::Rooms
