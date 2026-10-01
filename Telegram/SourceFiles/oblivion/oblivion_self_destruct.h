/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

// Self-destructing ("view once" / ttl_seconds) media.
//
// When the "save self-destructing media" setting is on:
// - photos and videos with ttl_seconds are shown as regular (spoilered)
//   media instead of the "view it on your phone" service placeholder;
// - every incoming self-destructing media is downloaded once and a copy
//   is saved to Downloads/Oblivion, saved ids live in tdata/oblivion/;
// - the media is not cleared locally after viewing and survives the
//   "expired" edit that the server sends later.

// Called while creating the media of a message. Returns true if the
// self-destructing media must be created as a regular one. As a side
// effect the incoming message is queued for saving.
[[nodiscard]] bool AcceptSelfDestructingMedia(
	not_null<HistoryItem*> item,
	const MTPDmessageMediaPhoto &media);
[[nodiscard]] bool AcceptSelfDestructingMedia(
	not_null<HistoryItem*> item,
	const MTPDmessageMediaDocument &media);

// An edition replaces the shown self-destructing media with the "expired"
// placeholder. Returns true if the edition must be ignored.
[[nodiscard]] bool KeepSelfDestructingMedia(
	not_null<HistoryItem*> item,
	const MTPMessageMedia *media);

// A voice / round message with ttl_seconds was played. Returns true if
// it must not be cleared locally (the view is refreshed instead, so the
// media can be opened again).
[[nodiscard]] bool KeepPlayedSelfDestructingMedia(
	not_null<HistoryItem*> item);

// Path of the saved copy of this message media, empty if there is none.
[[nodiscard]] QString SelfDestructingSavedPath(
	not_null<const HistoryItem*> item);

// "Open saved copy" / "Show saved copy in folder" context menu actions.
void AddSelfDestructingActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Oblivion
