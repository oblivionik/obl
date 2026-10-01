/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"

class DocumentData;

namespace Ui {
class Show;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// "Video sticker" box: a fragment of a video or a GIF (up to 3 seconds)
// and the crop (a square that can be moved and zoomed, or the whole
// frame), converted by Oblivion::VideoCore::MakeVideoSticker() to
// a WebM VP9 within the Telegram limits. The sound is dropped.
namespace Oblivion {

// done gets the WebM on the main thread, the box is already closing.
// content is used when path is empty.
void ShowVideoStickerTrim(
	std::shared_ptr<Ui::Show> show,
	QString path,
	QByteArray content,
	Fn<void(QByteArray webm)> done);

// A video from a chat: downloaded first, the box shows the progress.
void ShowVideoStickerTrim(
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	FullMsgId context,
	Fn<void(QByteArray webm)> done);

} // namespace Oblivion
