/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;
class DocumentData;

namespace Data {
struct UniqueGift;
} // namespace Data

namespace Ui {
class Show;
} // namespace Ui

namespace Oblivion {

void ShowVisualGiftInChat(
	not_null<PeerData*> to,
	std::shared_ptr<Data::UniqueGift> unique,
	DocumentData *document,
	const QString &title);

void SaveGiftAttribute(
	std::shared_ptr<Ui::Show> show,
	not_null<DocumentData*> document,
	const QString &fileName,
	const QString &dialogTitle);

} // namespace Oblivion
