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
//    (setGeometry), it paints its own background over st::windowBg.
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
void PaintUserpic(
	QPainter &p,
	QRect rect,
	uint64 userId,
	const QString &name);

// A cover of a track: the image, or a gradient made of the seed with
// a note on it while there is none.
void PaintCover(
	QPainter &p,
	QRect rect,
	const QImage &cover,
	const QString &seed,
	int radius);

// A round button with a glyph. accent: filled with the active colour.
class GlyphButton final : public Ui::AbstractButton {
public:
	GlyphButton(QWidget *parent, Glyph glyph, int size, bool accent = false);

	void setGlyph(Glyph glyph);
	void setHighlighted(bool highlighted); // The glyph in the active colour.
	void setDimmed(bool dimmed); // Looks disabled (no right), still clicks.

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	Glyph _glyph = Glyph::Play;
	bool _accent = false;
	bool _highlighted = false;
	bool _dimmed = false;

};

} // namespace Oblivion::Rooms
