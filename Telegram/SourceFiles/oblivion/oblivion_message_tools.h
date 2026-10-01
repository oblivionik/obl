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

void SaveStickerAsJson(
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document);

[[nodiscard]] QString ComposeMessageDetails(not_null<HistoryItem*> item);

void AddStickerActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	Data::FileOrigin origin = Data::FileOrigin());
void AddMessageDetailsAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Oblivion
