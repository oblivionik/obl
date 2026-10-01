/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_message_tools.h"

#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_deleted_store.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_sticker_studio.h"
#include "ui/boxes/confirm_box.h"
#include "ui/image/image_prepare.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Oblivion {
namespace {

[[nodiscard]] QString FileFilter(const QString &name, const QString &mask) {
	return name + u" ("_q + mask + u")"_q;
}

// The save dialogs below are modal and spin a nested event loop, so the
// session controller may die while one is open (e.g. a remote logout).
// Everything after the dialog uses only the Show, which stays valid.
void WriteToFile(
		std::shared_ptr<Ui::Show> show,
		const QString &path,
		const QByteArray &content) {
	auto target = QFile(path);
	if (target.open(QIODevice::WriteOnly)) {
		target.write(content);
		target.close();
		show->showToast(tr::lng_oblivion_saved_to(
			tr::now,
			lt_path,
			QDir::toNativeSeparators(path)));
	} else {
		show->showToast(tr::lng_oblivion_write_failed(tr::now));
	}
}

[[nodiscard]] QByteArray LoadedLottieJson(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document) {
	const auto media = document->createMediaView();
	if (!media->loaded()) {
		controller->showToast(tr::lng_oblivion_sticker_not_loaded(tr::now));
		return QByteArray();
	}
	const auto json = Images::UnpackGzip(media->bytes());
	if (json.isEmpty()) {
		controller->showToast(
			tr::lng_oblivion_sticker_unpack_failed(tr::now));
	}
	return json;
}

} // namespace

void SaveStickerAsJson(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document) {
	const auto json = LoadedLottieJson(controller, document);
	if (json.isEmpty()) {
		return;
	}
	const auto show = controller->uiShow();
	auto path = QString();
	const auto initial = filedialogDefaultName(u"sticker"_q, u".json"_q);
	if (!filedialogGetSaveFile(
			path,
			tr::lng_context_save_sticker_json(tr::now),
			FileFilter(tr::lng_oblivion_file_json(tr::now), u"*.json"_q),
			initial)) {
		return;
	}
	WriteToFile(show, path, json);
}

QString ComposeMessageDetails(not_null<HistoryItem*> item) {
	const auto formatDate = [](TimeId date) {
		return date
			? base::unixtime::parse(date).toString(u"dd.MM.yyyy hh:mm:ss"_q)
			: QString();
	};
	const auto describePeer = [](not_null<PeerData*> peer) {
		const auto bare = peer->isUser()
			? peerToUser(peer->id).bare
			: peer->isChannel()
			? peerToChannel(peer->id).bare
			: peerToChat(peer->id).bare;
		return peer->name() + u" ("_q + QString::number(bare) + u")"_q;
	};
	const auto line = [](const QString &label, const QString &value) {
		return label + u": "_q + value;
	};
	const auto inner = [&](const QString &label, const QString &value) {
		return u"  "_q + line(label, value);
	};
	auto lines = QStringList();
	lines.push_back(line(
		tr::lng_oblivion_details_id(tr::now),
		QString::number(item->id.bare)));
	lines.push_back(line(
		tr::lng_oblivion_details_date(tr::now),
		formatDate(item->date())));
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		lines.push_back(line(
			tr::lng_oblivion_details_edited(tr::now),
			formatDate(edited->date)));
	}
	lines.push_back(line(
		tr::lng_oblivion_details_from(tr::now),
		describePeer(item->from())));
	lines.push_back(line(
		tr::lng_oblivion_details_chat(tr::now),
		describePeer(item->history()->peer)));
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		lines.push_back(QString());
		lines.push_back(tr::lng_oblivion_details_forwarded(tr::now));
		if (forwarded->originalId) {
			lines.push_back(inner(
				tr::lng_oblivion_details_original_id(tr::now),
				QString::number(forwarded->originalId.bare)));
		}
		lines.push_back(inner(
			tr::lng_oblivion_details_original_date(tr::now),
			formatDate(forwarded->originalDate)));
		if (const auto sender = forwarded->originalSender) {
			lines.push_back(inner(
				tr::lng_oblivion_details_author(tr::now),
				describePeer(sender)));
		} else if (const auto hidden
				= forwarded->originalHiddenSenderInfo.get()) {
			lines.push_back(inner(
				tr::lng_oblivion_details_author(tr::now),
				(tr::lng_oblivion_details_hidden(tr::now)
					+ u" ("_q
					+ hidden->name
					+ u")"_q)));
		}
		if (const auto saved = forwarded->savedFromPeer) {
			lines.push_back(inner(
				tr::lng_oblivion_details_source(tr::now),
				describePeer(saved)));
		}
		if (forwarded->imported) {
			lines.push_back(
				u"  "_q + tr::lng_oblivion_details_imported(tr::now));
		}
	}
	const auto versions = EditHistory::Instance().versions(item);
	if (!versions.empty()) {
		lines.push_back(QString());
		lines.push_back(tr::lng_oblivion_edit_history(tr::now));
		auto index = 0;
		for (const auto &version : versions) {
			const auto number = QString::number(++index);
			lines.push_back(version.since
				? u"  %1. [%2] %3"_q.arg(
					number,
					formatDate(version.since),
					version.text.text)
				: u"  %1. %2"_q.arg(number, version.text.text));
		}
	}
	return lines.join('\n');
}

void AddStickerActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		Data::FileOrigin origin) {
	const auto sticker = document->sticker();
	if (!Get().exportStickerJson() || !sticker || !sticker->isLottie()) {
		return;
	}
	menu->addAction(
		tr::lng_context_save_sticker_json(tr::now),
		[=] { SaveStickerAsJson(controller, document); },
		&st::menuIconDownload);
	menu->addAction(
		tr::lng_oblivion_studio_open_menu(tr::now),
		[=] { ShowStickerStudioFor(controller, document, origin); },
		&st::menuIconEdit);
	// "Open in Lottie editor": the full editor window, downloads the
	// sticker first.
	menu->addAction(
		tr::lng_oblivion_lottie_open_menu(tr::now),
		[=] {
			LottieEdit::ShowLottieEditorFor(
				controller->uiShow(),
				document,
				origin);
		},
		&st::menuIconDraw);
}

void AddMessageDetailsAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	if (!Get().showMessageDetails()) {
		return;
	}
	const auto itemId = item->fullId();
	menu->addAction(
		tr::lng_context_message_details(tr::now),
		[=] {
			const auto owner = &controller->session().data();
			if (const auto found = owner->message(itemId)) {
				controller->show(
					Ui::MakeInformBox(ComposeMessageDetails(found)));
			}
		},
		&st::menuIconInfo);
}

} // namespace Oblivion
