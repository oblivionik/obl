/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_deleted.h"

#include "base/platform/base_platform_info.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "core/ui_integration.h"
#include "data/data_document_resolver.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_deleted_search.h"
#include "oblivion/oblivion_deleted_store.h"
#include "oblivion/oblivion_settings.h"
#include "platform/platform_file_utilities.h"
#include "ui/boxes/confirm_box.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/multi_select.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QImageReader>

namespace Oblivion {
namespace {

constexpr auto kPageSize = 30;
constexpr auto kPreviewMaxWidth = 240;
constexpr auto kPreviewMaxHeight = 180;

struct BoxState {
	QString query;
	std::vector<DeletedRecord> filtered;
	int shown = 0;
	bool adding = false;
	bool refillScheduled = false;
	bool stale = false;
};

[[nodiscard]] QString Separator() {
	return u" · "_q;
}

[[nodiscard]] QString FormatDate(TimeId date) {
	return date ? langDateTimeFull(base::unixtime::parse(date)) : QString();
}

[[nodiscard]] QString ChatName(
		not_null<Main::Session*> session,
		const DeletedRecord &record) {
	if (const auto peer = session->data().peerLoaded(
			PeerId(record.peerId))) {
		return peer->isSelf()
			? tr::lng_saved_messages(tr::now)
			: peer->name();
	}
	return record.chatName.isEmpty()
		? QString::number(record.peerId)
		: record.chatName;
}

[[nodiscard]] QString SenderName(
		not_null<Main::Session*> session,
		const DeletedRecord &record) {
	if (!record.senderId) {
		return record.senderName;
	} else if (record.senderId == session->userPeerId().value) {
		return tr::lng_from_you(tr::now);
	} else if (const auto peer = session->data().peerLoaded(
			PeerId(record.senderId))) {
		return peer->name();
	}
	return record.senderName;
}

[[nodiscard]] bool IsFileMedia(const QString &type) {
	return (type == u"photo"_q)
		|| (type == u"video"_q)
		|| (type == u"voice"_q)
		|| (type == u"round"_q)
		|| (type == u"audio"_q)
		|| (type == u"file"_q)
		|| (type == u"sticker"_q)
		|| (type == u"gif"_q);
}

[[nodiscard]] QString MediaTypeText(const QString &type) {
	if (type == u"photo"_q) {
		return tr::lng_in_dlg_photo(tr::now);
	} else if (type == u"video"_q) {
		return tr::lng_in_dlg_video(tr::now);
	} else if (type == u"voice"_q) {
		return tr::lng_in_dlg_audio(tr::now);
	} else if (type == u"round"_q) {
		return tr::lng_in_dlg_video_message(tr::now);
	} else if (type == u"audio"_q) {
		return tr::lng_in_dlg_audio_file(tr::now);
	} else if (type == u"file"_q) {
		return tr::lng_in_dlg_file(tr::now);
	} else if (type == u"sticker"_q) {
		return tr::lng_in_dlg_sticker(tr::now);
	} else if (type == u"gif"_q) {
		return u"GIF"_q;
	} else if (type == u"poll"_q) {
		return tr::lng_in_dlg_poll(tr::now);
	} else if (type == u"location"_q) {
		return tr::lng_maps_point(tr::now);
	} else if (type == u"contact"_q) {
		return tr::lng_in_dlg_contact(tr::now);
	}
	return QString();
}

[[nodiscard]] QString MediaLabel(const DeletedMedia &media) {
	auto parts = QStringList();
	const auto type = MediaTypeText(media.type);
	if (!type.isEmpty()) {
		parts.push_back(type);
	}
	if (!media.name.isEmpty()) {
		parts.push_back(media.name);
	}
	if (media.duration > 0) {
		parts.push_back(Ui::FormatDurationText(media.duration));
	}
	if (media.size > 0 && IsFileMedia(media.type)) {
		parts.push_back(Ui::FormatSizeText(media.size));
	}
	return parts.join(Separator());
}

[[nodiscard]] QImage LoadPreview(const QString &path) {
	if (path.isEmpty() || !QFile::exists(path)) {
		return QImage();
	}
	auto reader = QImageReader(path);
	reader.setAutoTransform(true);
	const auto original = reader.size();
	if (original.isEmpty()) {
		return QImage();
	}
	const auto ratio = style::DevicePixelRatio();
	const auto limit = QSize(
		style::ConvertScale(kPreviewMaxWidth),
		style::ConvertScale(kPreviewMaxHeight)) * ratio;
	const auto fits = (original.width() <= limit.width())
		&& (original.height() <= limit.height());
	reader.setScaledSize(fits
		? original
		: original.scaled(limit, Qt::KeepAspectRatio));
	auto image = reader.read();
	if (image.isNull()) {
		return QImage();
	}
	image = Images::Round(
		std::move(image).convertToFormat(
			QImage::Format_ARGB32_Premultiplied),
		ImageRoundRadius::Small);
	image.setDevicePixelRatio(ratio);
	return image;
}

[[nodiscard]] bool Matches(
		not_null<Main::Session*> session,
		const DeletedRecord &record,
		const QString &query) {
	const auto contains = [&](const QString &value) {
		return value.contains(query, Qt::CaseInsensitive);
	};
	return contains(record.text.text)
		|| contains(record.media.name)
		|| contains(record.senderName)
		|| contains(record.chatName)
		|| contains(SenderName(session, record))
		|| contains(ChatName(session, record));
}

[[nodiscard]] std::vector<DeletedRecord> CollectRecords(
		not_null<Main::Session*> session,
		not_null<DeletedStore*> store,
		uint64 peerId,
		const QString &query) {
	const auto &all = store->deleted();
	auto result = std::vector<DeletedRecord>();
	result.reserve(peerId ? store->deletedCount(peerId) : all.size());
	for (auto i = all.rbegin(); i != all.rend(); ++i) {
		if (peerId && i->peerId != peerId) {
			continue;
		} else if (!query.isEmpty() && !Matches(session, *i, query)) {
			continue;
		}
		result.push_back(*i);
	}
	return result;
}

void AddPreview(
		not_null<Ui::VerticalLayout*> container,
		QImage image,
		Fn<void()> open) {
	const auto size = image.size() / image.devicePixelRatio();
	auto button = object_ptr<Ui::AbstractButton>(container);
	const auto raw = button.data();
	raw->resize(size);
	raw->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(raw);
		p.drawImage(QRect(QPoint(), size), image);
	}, raw->lifetime());
	if (!open) {
		raw->setPointerCursor(false);
	} else {
		raw->setClickedCallback(std::move(open));
	}
	const auto &padding = st::boxRowPadding;
	container->add(
		std::move(button),
		style::margins(
			padding.left(),
			st::boxLittleSkip / 2,
			padding.right(),
			0),
		style::al_left);
}

void AddFileLinks(
		not_null<Ui::VerticalLayout*> container,
		const QString &file,
		Fn<void()> launch) {
	auto links = object_ptr<Ui::RpWidget>(container);
	const auto raw = links.data();
	const auto open = Ui::CreateChild<Ui::LinkButton>(
		raw,
		tr::lng_open_link(tr::now));
	const auto show = Ui::CreateChild<Ui::LinkButton>(
		raw,
		(Platform::IsMac()
			? tr::lng_context_show_in_finder(tr::now)
			: tr::lng_context_show_in_folder(tr::now)));
	const auto skip = st::boxLittleSkip * 2;
	open->moveToLeft(0, 0);
	show->moveToLeft(open->width() + skip, 0);
	raw->resize(
		open->width() + skip + show->width(),
		std::max(open->height(), show->height()));
	open->setClickedCallback(std::move(launch));
	show->setClickedCallback([=] {
		File::ShowInFolder(file);
	});
	const auto &padding = st::boxRowPadding;
	container->add(
		std::move(links),
		style::margins(
			padding.left(),
			st::boxLittleSkip / 2,
			padding.right(),
			0),
		style::al_left);
}

void AddMediaBlock(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<DeletedStore*> store,
		const DeletedMedia &media) {
	const auto file = store->absolutePath(media.file);
	const auto thumb = store->absolutePath(media.thumb);
	const auto hasFile = !file.isEmpty() && QFile::exists(file);
	const auto show = controller->uiShow();
	const auto open = hasFile
		? Fn<void()>([=] { OpenSavedFile(show, file); })
		: Fn<void()>();
	auto label = MediaLabel(media);
	if (!hasFile && IsFileMedia(media.type)) {
		label += (label.isEmpty() ? QString() : Separator())
			+ tr::lng_oblivion_deleted_media_missing(tr::now);
	}
	const auto &padding = st::boxRowPadding;
	if (!label.isEmpty()) {
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				label,
				st::defaultSubTextLabel),
			style::margins(
				padding.left(),
				st::boxLittleSkip / 2,
				padding.right(),
				0));
	}
	auto preview = LoadPreview(thumb);
	if (preview.isNull()
		&& hasFile
		&& (media.type == u"photo"_q || media.type == u"sticker"_q)) {
		preview = LoadPreview(file);
	}
	if (!preview.isNull()) {
		AddPreview(container, std::move(preview), open);
	}
	if (hasFile) {
		AddFileLinks(container, file, open);
	}
}

void AddRecordRow(
		not_null<Ui::VerticalLayout*> list,
		not_null<Window::SessionController*> controller,
		not_null<DeletedStore*> store,
		const DeletedRecord &record,
		bool showChat) {
	const auto session = &controller->session();
	const auto row = list->add(object_ptr<Ui::VerticalLayout>(list));
	const auto &padding = st::boxRowPadding;
	const auto skip = st::boxLittleSkip;
	Ui::AddSkip(row, skip);

	const auto sender = SenderName(session, record);
	auto header = tr::marked();
	if (showChat) {
		const auto chat = ChatName(session, record);
		header.append(tr::bold(chat));
		if (!sender.isEmpty() && sender != chat) {
			header.append(Separator()).append(sender);
		}
	} else {
		header.append(tr::bold(sender));
	}
	row->add(
		object_ptr<Ui::FlatLabel>(
			row,
			rpl::single(header),
			st::defaultFlatLabel),
		padding);

	auto meta = FormatDate(record.date);
	if (record.deleted) {
		meta += (meta.isEmpty() ? QString() : Separator())
			+ tr::lng_oblivion_deleted_at(
				tr::now,
				lt_date,
				FormatDate(record.deleted));
	}
	row->add(
		object_ptr<Ui::FlatLabel>(row, meta, st::defaultSubTextLabel),
		padding);

	if (!record.text.text.isEmpty()) {
		const auto text = row->add(
			object_ptr<Ui::FlatLabel>(row, st::defaultFlatLabel),
			style::margins(
				padding.left(),
				skip / 2,
				padding.right(),
				0));
		text->setMarkedText(
			record.text,
			Core::TextContext({ .session = session }));
		text->setSelectable(true);
	}
	if (!record.media.type.isEmpty()) {
		AddMediaBlock(row, controller, store, record.media);
	}
	Ui::AddSkip(row, skip);

	auto line = object_ptr<Ui::PlainShadow>(row);
	line->resize(line->width(), st::lineWidth);
	row->add(std::move(line), padding);
}

void DeletedMessagesBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		PeerData *peer) {
	const auto session = &controller->session();
	const auto store = &StoreFor(session);
	const auto peerId = peer ? peer->id.value : uint64();
	const auto showChat = !peer;
	const auto state = box->lifetime().make_state<BoxState>();

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_deleted_title());

	const auto search = box->setPinnedToTopContent(
		object_ptr<Ui::MultiSelect>(
			box,
			st::defaultMultiSelect,
			tr::lng_participant_filter()));
	box->setFocusCallback([=] {
		search->setInnerFocus();
	});

	const auto content = box->verticalLayout();
	const auto &padding = st::boxRowPadding;
	const auto empty = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				QString(),
				st::membersAbout),
			style::margins(
				padding.left(),
				st::boxMediumSkip,
				padding.right(),
				st::boxMediumSkip)),
		style::margins());
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());

	const auto showMore = [=] {
		const auto from = state->shown;
		const auto till = std::min(
			from + kPageSize,
			int(state->filtered.size()));
		if (from >= till) {
			return;
		}
		state->adding = true;
		state->shown = till;
		for (auto i = from; i != till; ++i) {
			AddRecordRow(
				list,
				controller,
				store,
				state->filtered[i],
				showChat);
		}
		state->adding = false;
	};
	const auto refill = [=] {
		state->stale = false;
		state->filtered = CollectRecords(
			session,
			store,
			peerId,
			state->query);
		state->shown = 0;
		list->clear();
		showMore();
		empty->entity()->setText(state->query.isEmpty()
			? tr::lng_oblivion_deleted_empty(tr::now)
			: tr::lng_search_tab_no_results(tr::now));
		empty->toggle(state->filtered.empty(), anim::type::instant);
		const auto count = int(state->filtered.size());
		box->setAdditionalTitle(rpl::single(count
			? QString::number(count)
			: QString()));
	};
	const auto scheduleRefill = [=] {
		if (state->refillScheduled) {
			return;
		}
		state->refillScheduled = true;
		Ui::PostponeCall(box, [=] {
			state->refillScheduled = false;
			refill();
		});
	};

	box->setInitScrollCallback([=] {
		box->scrolls() | rpl::on_next([=] {
			if (state->stale && box->scrollTop() <= 0) {
				scheduleRefill();
				return;
			} else if (state->adding
				|| state->shown >= int(state->filtered.size())) {
				return;
			}
			const auto visible = box->scrollHeight();
			if (box->scrollTop() + 2 * visible >= content->height()) {
				showMore();
			}
		}, box->lifetime());
	});

	search->setQueryChangedCallback([=](const QString &query) {
		state->query = query.trimmed();
		refill();
		box->scrollToY(0);
	});
	search->setSubmittedCallback([](Qt::KeyboardModifiers) {
	});

	store->deletedChanges(
	) | rpl::filter([=](const DeletedChange &change) {
		// A saved version of an edited message changes nothing here.
		return !change.edited
			&& (!peerId || !change.peerId || (change.peerId == peerId));
	}) | rpl::on_next([=](const DeletedChange &change) {
		if (!change.removed && box->scrollTop() > 0) {
			// Don't rebuild the list under the reader, new records
			// are shown when it is scrolled back to the top.
			state->stale = true;
			return;
		}
		scheduleRefill();
	}, box->lifetime());

	refill();

	// Oblivion round 5: the search with filters, in the edited ones too.
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
	box->addButton(tr::lng_oblivion_dsearch_open(), [=] {
		DeletedSearch::Show(controller, peer);
	});

	if (store->deletedCount(peerId) > 0) {
		box->addLeftButton(tr::lng_oblivion_deleted_clear(), [=] {
			controller->show(Ui::MakeConfirmBox({
				.text = (peerId
					? tr::lng_oblivion_deleted_clear_chat_sure()
					: tr::lng_oblivion_deleted_clear_sure()),
				.confirmed = [=](Fn<void()> close) {
					store->clearDeleted(peerId);
					close();
				},
				.confirmText = tr::lng_box_delete(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}, st::attentionBoxButton);
	}
}

// Oblivion round 5: one record, opened from the search.
void DeletedRecordBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		DeletedRecord record) {
	const auto session = &controller->session();
	const auto store = &StoreFor(session);
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_dsearch_record_title());
	AddRecordRow(box->verticalLayout(), controller, store, record, true);
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
	const auto peerId = PeerId(record.peerId);
	const auto messageId = MsgId(record.messageId);
	if (session->data().peerLoaded(peerId)) {
		box->addLeftButton(tr::lng_oblivion_dsearch_jump(), [=] {
			controller->hideLayer();
			controller->showPeerHistory(
				peerId,
				Window::SectionShow::Way::Forward,
				messageId);
		});
	}
}

} // namespace

QString DeletedMediaLabel(const DeletedMedia &media) {
	return MediaLabel(media);
}

void ShowDeletedRecord(
		not_null<Window::SessionController*> controller,
		const DeletedRecord &record) {
	controller->show(Box(DeletedRecordBox, controller, record));
}

void ShowDeletedMessages(
		not_null<Window::SessionController*> controller,
		PeerData *peer) {
	controller->show(Box(DeletedMessagesBox, controller, peer));
}

void AddDeletedMessagesAction(
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		const Ui::Menu::MenuCallback &addAction) {
	if (!peer
		|| !Get().keepDeleted()
		|| !StoreFor(&controller->session()).hasDeleted(peer->id.value)) {
		return;
	}
	addAction(
		tr::lng_oblivion_deleted_menu(tr::now),
		[=] { ShowDeletedMessages(controller, peer); },
		&st::menuIconRestore);
}

void OpenSavedFile(std::shared_ptr<Ui::Show> show, const QString &path) {
	const auto nameType = Core::DetectNameType(path);
	const auto isIpReveal = (nameType != Core::NameType::Executable)
		&& Core::IsIpRevealingPath(path);
	const auto extension = Core::FileExtension(path).toLower();
	if (extension.isEmpty()) {
		crl::on_main([=] {
			Platform::File::UnsafeShowOpenWith(path);
		});
		return;
	} else if (!Data::LauncherWouldWarn(
			Core::App().settings(),
			nameType,
			isIpReveal,
			extension,
			nullptr)) {
		File::Launch(path);
		return;
	}
	const auto executable = (nameType == Core::NameType::Executable);
	auto text = isIpReveal
		? tr::lng_launch_svg_warning(tr::marked)
		: (executable
			? tr::lng_launch_exe_warning
			: tr::lng_launch_other_warning)(
				lt_extension,
				rpl::single(tr::bold('.' + extension)),
				tr::marked);
	show->showBox(Ui::MakeConfirmBox({
		.text = std::move(text),
		.confirmed = [=](Fn<void()> close) {
			close();
			File::Launch(path);
		},
		.confirmText = (executable
			? tr::lng_launch_exe_sure
			: tr::lng_launch_other_sure)(),
	}));
}

} // namespace Oblivion
