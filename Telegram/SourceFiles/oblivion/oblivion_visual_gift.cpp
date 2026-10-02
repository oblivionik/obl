/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_visual_gift.h"

#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_star_gift.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/show.h"

#include <QtCore/QDir>

namespace Oblivion {

void ShowVisualGiftInChat(
		not_null<PeerData*> to,
		std::shared_ptr<Data::UniqueGift> unique,
		DocumentData *document,
		const QString &title) {
	const auto owner = &to->owner();
	const auto history = owner->history(to);
	const auto self = to->session().user();

	auto service = PreparedServiceText();
	service.links.push_back(to->createOpenLink());
	service.text = tr::lng_oblivion_gift_sent_visual(
		tr::now,
		lt_user,
		tr::link(to->shortName(), 1),
		tr::marked);

	const auto item = history->makeMessage({
		.id = owner->nextLocalMessageId(),
		.flags = (MessageFlag::Local
			| MessageFlag::HasFromId
			| MessageFlag::Outgoing),
		.from = self->id,
		.date = base::unixtime::now(),
	}, std::move(service));

	item->overrideMedia(std::make_unique<Data::MediaGiftBox>(
		item,
		self,
		Data::GiftCode{
			.stargiftId = unique ? unique->id : 0,
			.document = document,
			.unique = std::move(unique),
			.giftTitle = title,
		}));

	history->addNewLocalMessage(item);
}

void SaveGiftAttribute(
		std::shared_ptr<Ui::Show> show,
		not_null<DocumentData*> document,
		const QString &fileName,
		const QString &dialogTitle) {
	const auto media = document->createMediaView();
	if (!media->loaded()) {
		show->showToast(tr::lng_oblivion_gift_not_loaded(tr::now));
		return;
	}
	auto path = QString();
	const auto initial = filedialogDefaultName(fileName, u".tgs"_q);
	const auto filter = tr::lng_oblivion_file_tgs(tr::now)
		+ u" (*.tgs)"_q;
	if (!filedialogGetSaveFile(path, dialogTitle, filter, initial)) {
		return;
	}
	auto target = QFile(path);
	if (target.open(QIODevice::WriteOnly)) {
		target.write(media->bytes());
		target.close();
		show->showToast(tr::lng_oblivion_saved_to(
			tr::now,
			lt_path,
			QDir::toNativeSeparators(path)));
	} else {
		show->showToast(tr::lng_oblivion_write_failed(tr::now));
	}
}

} // namespace Oblivion
