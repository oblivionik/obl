/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

namespace Ui {
class BoxContent;
class RpWidget;
class Show;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion::Look {

// «Настройки → Oblivion → Тема Oblivion»: four tiles with a painted
// preview each, a click applies the look at once.
void ShowBox(not_null<Window::SessionController*> controller);

// The names of the looks: «Обычный Telegram», «Родной, но лучше»...
[[nodiscard]] rpl::producer<QString> NameValue(int look);
// The name of the saved choice, for the row in the settings.
[[nodiscard]] rpl::producer<QString> ChosenNameValue();

// A small picture of a look: a chats list, two bubbles and a chip. With
// look 0 it is painted with the colours of the Telegram theme.
void PaintPreview(QPainter &p, const QRect &rect, int look, bool dark);

// Snapshot scenes (oblivion_ui_snapshots.h) of a surface that depends on
// the look. Next to the usual registration of a scene "name", which is
// rendered with the plain look, one more call with the same arguments
// adds "name_look1", "name_look2" and "name_look3":
//
//   RegisterScene(u"room_window"_q, size, create, prepare);
//   Oblivion::Look::RegisterScenes(u"room_window"_q, size, create, prepare);
//
// Each of them is rendered for the day and for the night as usual. The
// look is on only while the scene exists, the scenes after it are plain.
void RegisterScenes(
	QString name,
	QSize size,
	Fn<QWidget*(not_null<Ui::RpWidget*> parent)> create,
	Fn<void(not_null<QWidget*> widget)> prepare = nullptr);
void RegisterBoxScenes(
	QString name,
	QSize size,
	Fn<object_ptr<Ui::BoxContent>(std::shared_ptr<Ui::Show> show)> create,
	Fn<void(not_null<QWidget*> widget)> prepare = nullptr);
// What they do: the look is painted until "owner" is destroyed. For a
// scene that is registered by hand, called first thing in its create().
void ShowInScene(int look, not_null<QWidget*> owner);

} // namespace Oblivion::Look
