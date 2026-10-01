/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_file_origin.h"

class DocumentData;
class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

// Opens the music editor (slowed / sped up, tempo, pitch, reverb, trim,
// joining several tracks, preview, export to a file or to a chat) for
// an audio document, downloading it first if needed.
// document == nullptr opens an empty editor that asks for local files.
void ShowMusicEditor(
	not_null<Window::SessionController*> controller,
	DocumentData *document,
	Data::FileOrigin origin = {});

// Adds "Open in music editor" to a message context menu if the message
// has a music / audio file or a voice message. Does nothing otherwise,
// including for an item of another account than the controller's one.
void AddMusicEditorAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Oblivion
