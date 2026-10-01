/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_sticker_packs.h"

#include "api/api_common.h"
#include "api/api_sending.h"
#include "api/api_stickers_creator.h"
#include "apiwrap.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "boxes/sticker_set_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "chat_helpers/emoji_picker_overlay.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "menu/menu_action_with_thumbnail.h"
#include "mtproto/sender.h"
#include "oblivion/oblivion_lottie.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_photo_core.h"
#include "oblivion/oblivion_photo_integration.h"
#include "oblivion/oblivion_sticker_packs_core.h"
#include "oblivion/oblivion_sticker_trim.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_video_core.h"
#include "oblivion/oblivion_vision.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "settings/settings_common.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/toast/toast.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/menu/menu.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/menu/menu_common.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/dynamic_image.h"
#include "ui/dynamic_thumbnails.h"
#include "ui/emoji_config.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_emoji_picker_overlay.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>

namespace Oblivion {
namespace {

using StickerPacks::Format;
using StickerPacks::Prepared;

constexpr auto kPageSize = 100;
constexpr auto kMaxPages = 20;
constexpr auto kReloadDelay = crl::time(600);
constexpr auto kNameCheckDelay = crl::time(450);
constexpr auto kToastDuration = crl::time(5000);
constexpr auto kSlowHintDelay = crl::time(4000);
constexpr auto kPreviewSide = 220;
constexpr auto kPreviewPadding = 14;
constexpr auto kPreviewSkip = 6;
constexpr auto kPreviewMaxPixels = 384;
constexpr auto kPreviewMaxFrames = 96;
constexpr auto kPickerMaxWidth = 300;
constexpr auto kThumbnailSize = 40;
constexpr auto kSniffSize = qint64(1024 * 1024);
constexpr auto kMaxLottieFile = qint64(8 * 1024 * 1024);
constexpr auto kMaxIssuesShown = 6;
constexpr auto kMaxNameTries = 100;
constexpr auto kSceneWidth = 480;
constexpr auto kSceneMenuSkip = 12;
constexpr auto kSnapshotReady = "oblivionPacksSnapshotReady";

// The pack chosen the last time, while the app runs. Main thread only.
auto LastPackId = uint64(0);

// Not zero while a PacksApi feeds an answer with the own sets to the
// session. Feeding notifies about an update synchronously, every other
// PacksApi of the account would reload its list because of that, and its
// answer would reload this one again, in a loop. Main thread only.
auto FeedingOwnSets = 0;

struct PackInfo {
	uint64 id = 0;
	uint64 accessHash = 0;
	QString title;
	QString shortName;
	int count = 0;
	bool emoji = false;
	bool masks = false;
	std::shared_ptr<Ui::DynamicImage> thumbnail;

	// Stickers can be added only to the usual sticker sets here.
	[[nodiscard]] bool regular() const {
		return !emoji && !masks;
	}
};

struct PacksState {
	std::vector<PackInfo> list;
	bool loaded = false;
	bool failed = false;
};

struct NewPack {
	QString title;
	QString shortName;
};

struct Target {
	std::optional<PackInfo> pack;
	std::optional<NewPack> create;

	explicit operator bool() const {
		return pack.has_value() || create.has_value();
	}
};

enum class NameStatus : uchar {
	Unknown,
	Checking,
	Available,
	Occupied,
	Invalid,
	Failed, // The request failed, the server decides when creating.
};

struct UploadRequest {
	Prepared sticker;
	QString emoji;
	Target target;
};

struct UploadHandlers {
	Fn<void(float64 progress)> progress; // Of the file upload.
	Fn<void()> finishing; // The file is there, adding it to the set.
	Fn<void(PackInfo pack, bool created)> done;
	Fn<void(QString error)> fail; // The error type, empty for the upload.
};

using MaskDone = Fn<void(Vision::MaskResult)>;

[[nodiscard]] QString CountText(const PackInfo &pack) {
	return pack.emoji
		? tr::lng_oblivion_packs_count_emoji(tr::now, lt_count, pack.count)
		: tr::lng_oblivion_packs_count(tr::now, lt_count, pack.count);
}

[[nodiscard]] QString ShortLink(const QString &shortName, bool emoji) {
	return (emoji ? u"t.me/addemoji/"_q : u"t.me/addstickers/"_q)
		+ shortName;
}

[[nodiscard]] QString FormatPercent(float64 progress) {
	return QString::number(int(std::round(
		std::clamp(progress, 0., 1.) * 100))) + '%';
}

[[nodiscard]] QString UploadErrorText(const QString &type) {
	if (type.isEmpty()) {
		return tr::lng_oblivion_packs_upload_failed(tr::now);
	} else if (type.endsWith(u"SHORT_NAME_OCCUPIED"_q)) {
		return tr::lng_oblivion_packs_name_occupied(tr::now);
	} else if (type.endsWith(u"SHORT_NAME_INVALID"_q)) {
		return tr::lng_oblivion_packs_name_invalid(tr::now);
	} else if (type == u"PACK_TITLE_INVALID"_q) {
		return tr::lng_oblivion_packs_error_title(tr::now);
	} else if (type == u"STICKERS_TOO_MUCH"_q) {
		return tr::lng_oblivion_packs_full(
			tr::now,
			lt_max,
			QString::number(StickerPacks::kMaxStickers));
	} else if (type.startsWith(u"STICKER_EMOJI"_q)
		|| type.startsWith(u"EMOJI"_q)) {
		return tr::lng_oblivion_packs_error_emoji(tr::now);
	} else if (type.startsWith(u"FLOOD"_q)) {
		return tr::lng_oblivion_packs_error_flood(tr::now);
	} else if (type.startsWith(u"STICKER_"_q)
		|| type.startsWith(u"FILE_"_q)
		|| type.startsWith(u"MEDIA_"_q)) {
		return tr::lng_oblivion_packs_error_file(tr::now, lt_error, type);
	}
	return tr::lng_oblivion_packs_error_generic(tr::now, lt_error, type);
}

[[nodiscard]] QString NameProblemText(StickerPacks::NameProblem problem) {
	using Problem = StickerPacks::NameProblem;
	switch (problem) {
	case Problem::TooLong: return tr::lng_oblivion_packs_name_long(tr::now);
	case Problem::Start: return tr::lng_oblivion_packs_name_start(tr::now);
	case Problem::Symbols:
		return tr::lng_oblivion_packs_name_symbols(tr::now);
	case Problem::Underscores:
		return tr::lng_oblivion_packs_name_underscores(tr::now);
	case Problem::None:
	case Problem::Empty: break;
	}
	return tr::lng_oblivion_packs_name_hint(tr::now);
}

[[nodiscard]] std::vector<EmojiPtr> ParseEmoji(const QString &text) {
	auto result = std::vector<EmojiPtr>();
	auto view = QStringView(text);
	while (!view.isEmpty()
		&& int(result.size()) < StickerPacks::kMaxEmoji) {
		auto length = 0;
		const auto emoji = Ui::Emoji::Find(view, &length);
		if (emoji && length > 0) {
			if (!ranges::contains(result, emoji)) {
				result.push_back(emoji);
			}
			view = view.mid(length);
		} else {
			view = view.mid(1);
		}
	}
	return result;
}

// The alt text of the sticker document is its first emoji, the whole
// list goes to the set item, as the official apps send them.
[[nodiscard]] QString FirstEmoji(const QString &text) {
	auto length = 0;
	const auto emoji = Ui::Emoji::Find(QStringView(text), &length);
	return (emoji && length > 0) ? text.left(length) : text;
}

// A label style without minWidth never wraps: it counts the height of
// a single line and the rest of the text is cut. The divider label has
// the same sub text color and wraps.
[[nodiscard]] const style::FlatLabel &CenteredSubTextStyle() {
	static const auto result = [] {
		auto copy = st::boxDividerLabel;
		copy.align = style::al_top;
		return copy;
	}();
	return result;
}

// The rows of the settings buttons start two pixels to the left of the
// box rows (checkboxes, labels), these start exactly with them.
[[nodiscard]] const style::SettingsButton &BoxRowButtonStyle(bool light) {
	static const auto make = [](const style::SettingsButton &st) {
		auto copy = st;
		copy.padding.setLeft(st::boxRowPadding.left());
		return copy;
	};
	static const auto normal = make(st::settingsButtonNoIcon);
	static const auto active = make(st::settingsButtonLightNoIcon);
	return light ? active : normal;
}

void CloseLater(not_null<Ui::GenericBox*> box) {
	crl::on_main(box, [=] {
		box->closeBox();
	});
}

// A ready image as a pack cover (the UI snapshots).
class ImageThumbnail final : public Ui::DynamicImage {
public:
	explicit ImageThumbnail(QImage image) : _image(std::move(image)) {
	}

	std::shared_ptr<DynamicImage> clone() override {
		return std::make_shared<ImageThumbnail>(_image);
	}
	QImage image(int size) override {
		const auto ratio = style::DevicePixelRatio();
		auto result = _image.scaled(
			QSize(size, size) * ratio,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
		result.setDevicePixelRatio(ratio);
		return result;
	}
	void subscribeToUpdates(Fn<void()> callback) override {
	}

private:
	const QImage _image;

};

// Session part: the list of the own sets and the changes of them.
class PacksApi final : public base::has_weak_ptr {
public:
	explicit PacksApi(not_null<Main::Session*> session);
	~PacksApi();

	[[nodiscard]] rpl::producer<PacksState> state() const;
	void reload();

	void suggestName(const QString &title, Fn<void(QString)> done);
	void checkName(const QString &name, Fn<void(NameStatus)> done);

	// One upload at a time, a new one cancels the previous. Returns
	// the way to cancel it, no handler is called after that.
	[[nodiscard]] Fn<void()> upload(
		UploadRequest request,
		UploadHandlers handlers);

	// done gets the error type, empty on success.
	void rename(
		const PackInfo &pack,
		const QString &title,
		Fn<void(QString)> done);
	void remove(const PackInfo &pack, Fn<void(QString)> done);

	// The @Stickers bot, null if it can't be found. A new request cancels
	// the previous one, the done of that one is never called.
	void resolveStickersBot(Fn<void(UserData*)> done);

private:
	struct Upload {
		UploadRequest request;
		UploadHandlers handlers;
		FullMsgId fullId;
		DocumentId documentId = 0;
		mtpRequestId requestId = 0;
		rpl::lifetime lifetime;
	};

	[[nodiscard]] PackInfo infoFromSet(not_null<Data::StickersSet*> set);
	void requestPage(uint64 offsetId);
	void publish();
	void cancelUpload();
	void fileUploaded(const MTPInputFile &file);
	void addToSet(const MTPInputDocument &document);
	void setReceived(const MTPmessages_StickerSet &result, bool created);
	void uploadFailed(const QString &error);
	void installCreated(const PackInfo &pack);

	const not_null<Main::Session*> _session;
	MTP::Sender _api;
	PacksState _state;
	rpl::event_stream<PacksState> _changes;
	std::vector<PackInfo> _loading;
	int _pages = 0;
	mtpRequestId _loadRequest = 0;
	bool _reloadAgain = false;
	mtpRequestId _suggestRequest = 0;
	mtpRequestId _checkRequest = 0;
	mtpRequestId _botRequest = 0;
	std::unique_ptr<Upload> _upload;
	int _uploadGeneration = 0;
	base::Timer _reloadTimer;
	rpl::lifetime _lifetime;

};

PacksApi::PacksApi(not_null<Main::Session*> session)
: _session(session)
, _api(&session->mtp())
, _reloadTimer([=] { reload(); }) {
	// Changes made in the sticker set box: renamed, reordered, deleted.
	_session->data().stickers().updated(
		Data::StickersType::Stickers
	) | rpl::on_next([=] {
		// Not what a list of the own sets has just received itself: this
		// one (its request is still active) or any other (FeedingOwnSets).
		if (_state.loaded && !_loadRequest && !FeedingOwnSets) {
			_reloadTimer.callOnce(kReloadDelay);
		}
	}, _lifetime);
}

PacksApi::~PacksApi() {
	cancelUpload();
}

rpl::producer<PacksState> PacksApi::state() const {
	return _changes.events_starting_with_copy(_state);
}

PackInfo PacksApi::infoFromSet(not_null<Data::StickersSet*> set) {
	const auto type = set->type();
	const auto cover = set->lookupThumbnailDocument();
	return PackInfo{
		.id = set->id,
		.accessHash = set->accessHash,
		.title = set->title,
		.shortName = set->shortName,
		.count = set->count,
		.emoji = (type == Data::StickersType::Emoji),
		.masks = (type == Data::StickersType::Masks),
		.thumbnail = (cover
			? Ui::MakeDocumentThumbnailFit(
				cover,
				Data::FileOriginStickerSet(set->id, set->accessHash))
			: nullptr),
	};
}

void PacksApi::reload() {
	_reloadTimer.cancel();
	if (_loadRequest) {
		_reloadAgain = true;
		return;
	}
	_loading.clear();
	_pages = 0;
	requestPage(0);
}

void PacksApi::requestPage(uint64 offsetId) {
	_loadRequest = _api.request(MTPmessages_GetMyStickers(
		MTP_long(offsetId),
		MTP_int(kPageSize)
	)).done([=](const MTPmessages_MyStickers &result) {
		const auto &data = result.data();
		const auto &sets = data.vsets().v;
		auto lastId = uint64(0);
		auto added = 0;

		// Feeding a set with its stickers notifies about an update, right
		// from feedSet(). The other lists of the own sets skip it by the
		// counter, a real change is always notified about outside of here.
		++FeedingOwnSets;
		for (const auto &covered : sets) {
			const auto set = _session->data().stickers().feedSet(covered);
			lastId = set->id;
			if (!ranges::contains(_loading, set->id, &PackInfo::id)) {
				_loading.push_back(infoFromSet(set));
				++added;
			}
		}
		--FeedingOwnSets;

		// The request counts as active till here, so that this list doesn't
		// reload itself because of what it has just received.
		_loadRequest = 0;

		const auto more = (added > 0)
			&& (int(_loading.size()) < data.vcount().v)
			&& (++_pages < kMaxPages);
		if (more && !_reloadAgain) {
			requestPage(lastId);
			return;
		}
		_state = PacksState{
			.list = base::take(_loading),
			.loaded = true,
		};
		publish();
	}).fail([=](const MTP::Error &error) {
		_loadRequest = 0;
		_loading.clear();
		LOG(("Oblivion Sticker Error: no own sets, %1.").arg(error.type()));
		_state.loaded = true;
		_state.failed = true;
		publish();
	}).send();
}

void PacksApi::publish() {
	_changes.fire_copy(_state);
	if (base::take(_reloadAgain)) {
		reload();
	}
}

void PacksApi::suggestName(const QString &title, Fn<void(QString)> done) {
	_api.request(base::take(_suggestRequest)).cancel();
	_suggestRequest = _api.request(MTPstickers_SuggestShortName(
		MTP_string(title)
	)).done([=](const MTPstickers_SuggestedShortName &result) {
		_suggestRequest = 0;
		done(qs(result.data().vshort_name()));
	}).fail([=] {
		_suggestRequest = 0;
		done(QString());
	}).send();
}

void PacksApi::checkName(const QString &name, Fn<void(NameStatus)> done) {
	_api.request(base::take(_checkRequest)).cancel();
	_checkRequest = _api.request(MTPstickers_CheckShortName(
		MTP_string(name)
	)).done([=](const MTPBool &result) {
		_checkRequest = 0;
		done(mtpIsTrue(result)
			? NameStatus::Available
			: NameStatus::Occupied);
	}).fail([=](const MTP::Error &error) {
		_checkRequest = 0;
		const auto &type = error.type();
		done(type.endsWith(u"SHORT_NAME_OCCUPIED"_q)
			? NameStatus::Occupied
			: type.endsWith(u"SHORT_NAME_INVALID"_q)
			? NameStatus::Invalid
			: NameStatus::Failed);
	}).send();
}

[[nodiscard]] std::shared_ptr<FilePrepareResult> PrepareUploadFile(
		MTP::DcId dcId,
		DocumentId id,
		const Prepared &sticker) {
	const auto filename = StickerPacks::FileName(sticker.format);
	const auto mime = StickerPacks::MimeType(sticker.format);
	auto attributes = QVector<MTPDocumentAttribute>(
		1,
		MTP_documentAttributeFilename(MTP_string(filename)));
	attributes.push_back(MTP_documentAttributeImageSize(
		MTP_int(sticker.size.width()),
		MTP_int(sticker.size.height())));

	// The local document is a sticker for the uploader as well: it keeps
	// stickers in the cache, any other file gets a copy in the downloads
	// folder. What the server gets is set in fileUploaded().
	attributes.push_back(MTP_documentAttributeSticker(
		MTP_flags(0),
		MTP_string(),
		MTP_inputStickerSetEmpty(),
		MTPMaskCoords()));

	auto result = MakePreparedFile({
		.id = id,
		.type = SendMediaType::File,
	});
	result->filename = filename;
	result->filemime = mime;
	result->content = sticker.bytes;
	result->filesize = sticker.bytes.size();
	result->setFileData(sticker.bytes);
	result->document = MTP_document(
		MTP_flags(0),
		MTP_long(id),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(base::unixtime::now()),
		MTP_string(mime),
		MTP_long(sticker.bytes.size()),
		MTP_vector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(dcId),
		MTP_vector<MTPDocumentAttribute>(std::move(attributes)));
	return result;
}

Fn<void()> PacksApi::upload(UploadRequest request, UploadHandlers handlers) {
	cancelUpload();

	const auto generation = ++_uploadGeneration;
	_upload = std::make_unique<Upload>();
	const auto upload = _upload.get();
	upload->request = std::move(request);
	upload->handlers = std::move(handlers);
	upload->documentId = base::RandomValue<DocumentId>();
	upload->fullId = FullMsgId(
		_session->userPeerId(),
		_session->data().nextLocalMessageId());
	const auto fullId = upload->fullId;
	const auto documentId = upload->documentId;
	const auto file = PrepareUploadFile(
		_session->mtp().mainDcId(),
		documentId,
		upload->request.sticker);

	_session->uploader().documentReady(
	) | rpl::filter([=](const Storage::UploadedMedia &data) {
		return (data.fullId == fullId);
	}) | rpl::on_next([=](const Storage::UploadedMedia &data) {
		fileUploaded(data.info.file);
	}, upload->lifetime);

	_session->uploader().documentFailed(
	) | rpl::filter([=](const FullMsgId &id) {
		return (id == fullId);
	}) | rpl::on_next([=] {
		if (_upload) {
			_upload->fullId = FullMsgId();
		}
		uploadFailed(QString());
	}, upload->lifetime);

	_session->uploader().documentProgress(
	) | rpl::filter([=](const FullMsgId &id) {
		return (id == fullId);
	}) | rpl::on_next([=] {
		const auto document = _session->data().document(documentId);
		const auto data = document->uploadingData.get();
		if (!_upload || !data || data->size <= 0) {
			return;
		} else if (const auto onstack = _upload->handlers.progress) {
			onstack(data->offset / float64(data->size));
		}
	}, upload->lifetime);

	_session->uploader().upload(fullId, file);

	return crl::guard(this, [=] {
		if (_uploadGeneration == generation) {
			cancelUpload();
		}
	});
}

void PacksApi::cancelUpload() {
	if (const auto upload = base::take(_upload)) {
		if (upload->fullId) {
			_session->uploader().cancel(upload->fullId);
		}
		if (upload->requestId) {
			_api.request(upload->requestId).cancel();
		}
	}
}

void PacksApi::uploadFailed(const QString &error) {
	if (const auto upload = base::take(_upload)) {
		if (upload->fullId) {
			_session->uploader().cancel(upload->fullId);
		}
		if (const auto onstack = upload->handlers.fail) {
			onstack(error);
		}
	}
}

void PacksApi::fileUploaded(const MTPInputFile &file) {
	if (!_upload) {
		return;
	}
	_upload->lifetime.destroy();
	_upload->fullId = FullMsgId();

	const auto &request = _upload->request;
	const auto &sticker = request.sticker;
	auto attributes = QVector<MTPDocumentAttribute>();
	attributes.push_back(MTP_documentAttributeSticker(
		MTP_flags(0),
		MTP_string(FirstEmoji(request.emoji)),
		MTP_inputStickerSetEmpty(),
		MTPMaskCoords()));
	if (sticker.format == Format::Video) {
		attributes.push_back(MTP_documentAttributeVideo(
			MTP_flags(0),
			MTP_double(sticker.duration / 1000.),
			MTP_int(sticker.size.width()),
			MTP_int(sticker.size.height()),
			MTPint(),
			MTPdouble(),
			MTPstring()));
	}
	attributes.push_back(MTP_documentAttributeImageSize(
		MTP_int(sticker.size.width()),
		MTP_int(sticker.size.height())));

	if (const auto onstack = _upload->handlers.finishing) {
		onstack();
		if (!_upload) {
			return;
		}
	}
	_upload->requestId = _api.request(MTPmessages_UploadMedia(
		MTP_flags(0),
		MTPstring(),
		MTP_inputPeerSelf(),
		MTP_inputMediaUploadedDocument(
			MTP_flags(0),
			file,
			MTPInputFile(),
			MTP_string(StickerPacks::MimeType(sticker.format)),
			MTP_vector<MTPDocumentAttribute>(std::move(attributes)),
			MTP_vector<MTPInputDocument>(),
			MTPInputPhoto(),
			MTP_int(0),
			MTP_int(0))
	)).done([=](const MTPMessageMedia &result) {
		if (!_upload) {
			return;
		}
		_upload->requestId = 0;
		auto found = false;
		result.match([&](const MTPDmessageMediaDocument &data) {
			const auto document = data.vdocument();
			if (!document) {
				return;
			}
			document->match([&](const MTPDdocument &fields) {
				found = true;
				addToSet(MTP_inputDocument(
					fields.vid(),
					fields.vaccess_hash(),
					fields.vfile_reference()));
			}, [](const auto &) {
			});
		}, [](const auto &) {
		});
		if (!found) {
			uploadFailed(u"STICKER_FILE_INVALID"_q);
		}
	}).fail([=](const MTP::Error &error) {
		if (_upload) {
			_upload->requestId = 0;
			uploadFailed(error.type());
		}
	}).handleFloodErrors().send();
}

void PacksApi::addToSet(const MTPInputDocument &document) {
	const auto &request = _upload->request;
	const auto item = MTP_inputStickerSetItem(
		MTP_flags(0),
		document,
		MTP_string(request.emoji),
		MTPMaskCoords(),
		MTPstring());
	const auto done = [=](bool created) {
		return [=](const MTPmessages_StickerSet &result) {
			if (_upload) {
				_upload->requestId = 0;
				setReceived(result, created);
			}
		};
	};
	const auto fail = [=](const MTP::Error &error) {
		if (_upload) {
			_upload->requestId = 0;
			uploadFailed(error.type());
		}
	};
	if (const auto &pack = request.target.pack) {
		_upload->requestId = _api.request(MTPstickers_AddStickerToSet(
			Data::InputStickerSet(StickerSetIdentifier{
				.id = pack->id,
				.accessHash = pack->accessHash,
			}),
			item
		)).done(done(false)).fail(fail).handleFloodErrors().send();
	} else if (const auto &create = request.target.create) {
		_upload->requestId = _api.request(MTPstickers_CreateStickerSet(
			MTP_flags(0),
			MTP_inputUserSelf(),
			MTP_string(create->title),
			MTP_string(create->shortName),
			MTPInputDocument(),
			MTP_vector<MTPInputStickerSetItem>(
				QVector<MTPInputStickerSetItem>(1, item)),
			MTPstring()
		)).done(done(true)).fail(fail).handleFloodErrors().send();
	} else {
		uploadFailed(u"STICKERSET_INVALID"_q);
	}
}

void PacksApi::setReceived(
		const MTPmessages_StickerSet &result,
		bool created) {
	auto info = std::optional<PackInfo>();
	result.match([&](const MTPDmessages_stickerSet &data) {
		auto &stickers = _session->data().stickers();
		const auto set = stickers.feedSetFull(data);
		stickers.notifyUpdated(Data::StickersType::Stickers);
		info = infoFromSet(set);
	}, [](const auto &) {
	});
	if (!info) {
		uploadFailed(u"STICKERSET_INVALID"_q);
		return;
	}
	if (created) {
		installCreated(*info);
	}
	reload();

	// The handler may destroy everything, nothing is used after it.
	const auto upload = base::take(_upload);
	if (const auto onstack = upload->handlers.done) {
		onstack(*info, created);
	}
}

// A new set gets into the sticker panel of its creator right away.
void PacksApi::installCreated(const PackInfo &pack) {
	const auto session = _session;
	const auto refresh = crl::guard(session, [=] {
		session->data().stickers().setLastUpdate(0);
		session->api().updateStickers();
	});
	session->api().request(MTPmessages_InstallStickerSet(
		Data::InputStickerSet(StickerSetIdentifier{
			.id = pack.id,
			.accessHash = pack.accessHash,
		}),
		MTP_bool(false)
	)).done(refresh).fail(refresh).send();
}

void PacksApi::rename(
		const PackInfo &pack,
		const QString &title,
		Fn<void(QString)> done) {
	_api.request(MTPstickers_RenameStickerSet(
		Data::InputStickerSet(StickerSetIdentifier{
			.id = pack.id,
			.accessHash = pack.accessHash,
		}),
		MTP_string(title)
	)).done([=](const MTPmessages_StickerSet &result) {
		result.match([&](const MTPDmessages_stickerSet &data) {
			auto &stickers = _session->data().stickers();
			stickers.feedSetFull(data);
			stickers.notifyUpdated(Data::StickersType::Stickers);
		}, [](const auto &) {
		});
		reload();
		done(QString());
	}).fail([=](const MTP::Error &error) {
		done(error.type());
	}).send();
}

// The request belongs to the session: the sticker panel is refreshed and
// done is called even if the list is closed before the answer comes.
void PacksApi::remove(const PackInfo &pack, Fn<void(QString)> done) {
	const auto session = _session;
	const auto weak = base::make_weak(this);
	Api::DeleteStickerSet(
		session,
		StickerSetIdentifier{ .id = pack.id, .accessHash = pack.accessHash },
		[=] {
			session->data().stickers().setLastUpdate(0);
			session->api().updateStickers();
			if (const auto strong = weak.get()) {
				strong->reload();
			}
			done(QString());
		},
		[=](QString error) {
			done(error.isEmpty() ? u"UNKNOWN"_q : error);
		});
}

void PacksApi::resolveStickersBot(Fn<void(UserData*)> done) {
	const auto username = u"Stickers"_q;
	const auto botFrom = [](PeerData *peer) {
		const auto user = peer ? peer->asUser() : nullptr;
		return (user && user->isBot()) ? user : nullptr;
	};
	_api.request(base::take(_botRequest)).cancel();
	if (const auto bot = botFrom(_session->data().peerByUsername(username))) {
		done(bot);
		return;
	}
	_botRequest = _api.request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTPstring()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		_botRequest = 0;
		const auto &data = result.data();
		auto &owner = _session->data();
		owner.processUsers(data.vusers());
		owner.processChats(data.vchats());
		done(botFrom(owner.peerLoaded(peerFromMTP(data.vpeer()))));
	}).fail([=](const MTP::Error &error) {
		_botRequest = 0;
		LOG(("Oblivion Sticker Error: no @Stickers bot, %1."
			).arg(error.type()));
		done(nullptr);
	}).send();
}

// The sticker as it looks in a chat: over a wallpaper-like background,
// where the white outline is seen in both themes. Animated ones loop.
struct PreviewFrame {
	QImage image;
	crl::time duration = 0;
};

class StickerPreview final : public Ui::RpWidget {
public:
	explicit StickerPreview(QWidget *parent);

	void setFrames(std::vector<PreviewFrame> frames);

	[[nodiscard]] bool hasFrames() const {
		return !_frames.empty();
	}

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void advance();

	std::vector<PreviewFrame> _frames;
	int _index = 0;
	base::Timer _timer;

};

StickerPreview::StickerPreview(QWidget *parent)
: RpWidget(parent)
, _timer([=] { advance(); }) {
}

void StickerPreview::setFrames(std::vector<PreviewFrame> frames) {
	_timer.cancel();
	_frames = std::move(frames);
	_index = 0;
	if (_frames.size() > 1) {
		_timer.callOnce(std::max(_frames.front().duration, crl::time(15)));
	}
	update();
}

void StickerPreview::advance() {
	if (_frames.size() < 2) {
		return;
	}
	_index = (_index + 1) % int(_frames.size());
	_timer.callOnce(std::max(_frames[_index].duration, crl::time(15)));
	update();
}

void StickerPreview::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto outer = rect();
	auto gradient = QLinearGradient(outer.topLeft(), outer.bottomRight());
	gradient.setStops({
		{ 0., QColor(0x7f, 0xa3, 0x81) },
		{ 0.5, QColor(0x6f, 0xa0, 0x8e) },
		{ 1., QColor(0x86, 0xa9, 0x74) },
	});
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	const auto radius = float64(st::roundRadiusLarge);
	p.drawRoundedRect(outer, radius, radius);

	if (_frames.empty()) {
		return;
	}
	const auto &image = _frames[_index].image;
	if (image.isNull()) {
		return;
	}
	const auto padding = style::ConvertScale(kPreviewPadding);
	const auto inner = outer.marginsRemoved(
		{ padding, padding, padding, padding });
	const auto size = image.size().scaled(inner.size(), Qt::KeepAspectRatio);
	p.drawImage(
		QRect(
			inner.x() + (inner.width() - size.width()) / 2,
			inner.y() + (inner.height() - size.height()) / 2,
			size.width(),
			size.height()),
		image);
}

// Everything heavy about one sticker, off the main thread.
struct WorkRequest {
	StickerSource::Type type = StickerSource::Type::Image;
	QImage image;
	QByteArray data;
	StickerPacks::StaticOptions options;
	std::optional<StickerPacks::LottieCheck> lottie; // Already checked.
	bool fix = false; // Apply the automatic fixes to the animation.
	int side = 0; // Of the preview frames, in pixels.
};

struct WorkResult {
	Prepared prepared;
	QImage composed; // Type::Image: what is encoded, for a PNG file.
	std::vector<PreviewFrame> frames;
	StickerPacks::LottieCheck lottie;
	StickerPacks::VideoProblem video = StickerPacks::VideoProblem::None;
};

[[nodiscard]] QImage FitPreview(const QImage &image, int side) {
	if (image.isNull()
		|| (image.width() <= side && image.height() <= side)) {
		return image;
	}
	return image.scaled(
		QSize(side, side),
		Qt::KeepAspectRatio,
		Qt::SmoothTransformation);
}

[[nodiscard]] WorkResult Work(const WorkRequest &request) {
	using Type = StickerSource::Type;
	auto result = WorkResult();
	const auto side = std::max(request.side, 1);
	if (request.type == Type::Image) {
		const auto composed = StickerPacks::ComposeSticker(
			request.image,
			request.options);
		result.prepared = StickerPacks::EncodeStatic(composed);
		if (!composed.isNull()) {
			result.frames.push_back({ FitPreview(composed, side) });
		}
		result.composed = composed;
	} else if (request.type == Type::Lottie) {
		result.lottie = request.lottie
			? *request.lottie
			: StickerPacks::CheckLottie(request.data);
		if (request.fix) {
			result.lottie = StickerPacks::FixLottie(result.lottie);
		}
		result.prepared = StickerPacks::PrepareAnimated(result.lottie);
		if (result.lottie.parsed()) {
			auto renderer = Lottie::Renderer(result.lottie.document.toJson());
			const auto &info = renderer.info();
			if (renderer.valid() && info.valid()) {
				const auto step = std::max(
					int(std::ceil(info.frames / float64(kPreviewMaxFrames))),
					(info.fps > 31.) ? 2 : 1);
				const auto duration = crl::time(
					std::llround(step * 1000. / info.fps));
				for (auto i = 0; i < info.frames; i += step) {
					auto frame = renderer.render(i, QSize(side, side));
					if (!frame.isNull()) {
						result.frames.push_back({ std::move(frame), duration });
					}
				}
			}
		}
	} else {
		const auto check = StickerPacks::CheckVideoSticker(request.data);
		result.video = check.problem;
		result.prepared = StickerPacks::PrepareVideo(request.data, check);
		if (check.info.valid()) {
			const auto options = VideoCore::FrameOptions{
				.content = request.data,
				.till = VideoCore::kStickerMaxDuration,
				.size = check.info.size.scaled(
					QSize(side, side),
					Qt::KeepAspectRatio).expandedTo(QSize(1, 1)),
			};
			const auto read = VideoCore::ReadFrames(options, [&](
					VideoCore::Frame &&frame) {
				result.frames.push_back({
					std::move(frame.image),
					frame.duration,
				});
				return int(result.frames.size()) < kPreviewMaxFrames;
			});
			if (!read) {
				result.frames.clear();
			}
		}
	}
	return result;
}

[[nodiscard]] QString InfoText(const Prepared &prepared) {
	const auto separator = QString::fromUtf8(" \xC2\xB7 ");
	const auto size = u"%1×%2"_q.arg(
		prepared.size.width()
	).arg(prepared.size.height());
	const auto duration = tr::lng_oblivion_studio_seconds(
		tr::now,
		lt_value,
		LottieEdit::FormatDecimal(prepared.duration / 1000., 1));
	const auto bytes = LottieEdit::FormatKilobytes(prepared.bytes.size());
	switch (prepared.format) {
	case Format::Static:
		return tr::lng_oblivion_packs_kind_static(tr::now)
			+ separator
			+ size
			+ separator
			+ bytes;
	case Format::Animated:
		return tr::lng_oblivion_packs_kind_animated(tr::now)
			+ separator
			+ duration
			+ separator
			+ bytes;
	case Format::Video:
		return tr::lng_oblivion_packs_kind_video(tr::now)
			+ separator
			+ size
			+ separator
			+ duration
			+ separator
			+ bytes;
	}
	return QString();
}

[[nodiscard]] QString IssuesText(const StickerPacks::LottieCheck &check) {
	if (!check.parsed()) {
		return QString();
	}
	auto lines = QStringList();
	const auto &issues = check.validation.issues;
	const auto shown = std::min(int(issues.size()), kMaxIssuesShown);
	for (auto i = 0; i != shown; ++i) {
		lines.push_back(QString::fromUtf8("\xE2\x80\xA2 ")
			+ LottieEdit::IssueText(check.document, issues[i]));
	}
	if (int(issues.size()) > shown) {
		lines.push_back(tr::lng_oblivion_packs_lottie_more(
			tr::now,
			lt_value,
			QString::number(int(issues.size()) - shown)));
	}
	return lines.join('\n');
}

[[nodiscard]] QString ExportFilter(Format format) {
	switch (format) {
	case Format::Static:
		return tr::lng_oblivion_photo_io_filter_png(tr::now)
			+ u" (*.png);;"_q
			+ tr::lng_oblivion_photo_io_filter_webp(tr::now)
			+ u" (*.webp)"_q;
	case Format::Animated:
		return tr::lng_oblivion_packs_kind_animated(tr::now)
			+ u" (*.tgs)"_q;
	case Format::Video:
		return tr::lng_oblivion_video_file_webm(tr::now)
			+ u" (*.webm)"_q;
	}
	Unexpected("Format in ExportFilter.");
}

[[nodiscard]] QString SuggestedExportPath(
		const QString &hint,
		Format format) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(
		StickerPacks::ExportFileName(
			hint,
			StickerPacks::ExportTypeFor(format)),
		QString());
}

// The save dialog asks about replacing the file with the chosen name. If
// the extension is added to that name here, the result is another file
// the user was not asked about: it is never replaced.
[[nodiscard]] QString FreeExportPath(
		const QString &chosen,
		StickerPacks::ExportType type) {
	const auto result = StickerPacks::ExportPath(chosen, type);
	if (result == chosen || !QFileInfo::exists(result)) {
		return result;
	}
	const auto extension = QChar('.') + StickerPacks::ExportExtension(type);
	for (auto i = 2; i != kMaxNameTries; ++i) {
		const auto next = chosen + u" (%1)"_q.arg(i) + extension;
		if (!QFileInfo::exists(next)) {
			return next;
		}
	}
	return QString();
}

[[nodiscard]] bool WriteFile(const QString &path, const QByteArray &bytes) {
	if (path.isEmpty() || bytes.isEmpty()) {
		return false;
	}
	auto file = QSaveFile(path);
	return file.open(QIODevice::WriteOnly)
		&& (file.write(bytes) == bytes.size())
		&& file.commit();
}

// The menu of the "File" button of the "add a sticker" box.
struct FileMenuArgs {
	Fn<void()> save;
	Fn<void()> send;
	Fn<void()> sendToBot;
};

void FillFileMenu(not_null<Ui::Menu::Menu*> menu, FileMenuArgs &&args) {
	const auto add = [&](
			const QString &text,
			Fn<void()> callback,
			const style::icon *icon) {
		if (callback) {
			menu->addAction(text, std::move(callback), icon);
		}
	};
	add(
		tr::lng_oblivion_packs_export_save(tr::now),
		std::move(args.save),
		&st::menuIconDownload);
	add(
		tr::lng_oblivion_packs_export_send(tr::now),
		std::move(args.send),
		&st::menuIconShare);
	add(
		tr::lng_oblivion_packs_export_bot(tr::now),
		std::move(args.sendToBot),
		&st::menuIconBot);
}

// sent is called on the main thread when the message is really queued.
using SendFileMethod = Fn<void(
	StickerPacks::ExportFile file,
	Fn<void()> sent)>;

struct AddArgs {
	std::shared_ptr<Ui::Show> show;
	StickerSource source;
	rpl::producer<PacksState> packs;

	// The box title, "Add to sticker pack" if there is none.
	rpl::producer<QString> title;

	// The converter: a text about the "File" button is shown.
	bool exportAbout = false;

	// "File" > "Send as a file" to a chat chosen by the user and to the
	// @Stickers bot. An item is not shown without its method.
	SendFileMethod sendFile;
	SendFileMethod sendToBot;

	// Where to add: this set, or the one that is created with the
	// sticker. Nothing: the last used set or the first one.
	uint64 presetPackId = 0;
	std::optional<NewPack> presetNew;

	// Asks for the title and the short name of a new set.
	Fn<void(QString title, Fn<void(NewPack)> done)> createPack;

	// Returns the way to cancel the upload.
	Fn<Fn<void()>(UploadRequest, UploadHandlers)> upload;

	// The link in the toast after the sticker is added.
	Fn<void(PackInfo)> openPack;

	// Called once, after the sticker is really in a set: not when the
	// box is cancelled and not on an error.
	Fn<void()> added;

	// Type::Image: "Remove background", not set where it can't be done.
	Fn<void(QImage image, MaskDone done)> removeBackground;

	// Called when the first preview is shown (the UI snapshots).
	Fn<void(not_null<Ui::GenericBox*>)> shown;
};

void AddStickerBox(not_null<Ui::GenericBox*> box, AddArgs &&args) {
	using Type = StickerSource::Type;
	struct State : base::has_weak_ptr {
		~State() {
			if (const auto onstack = base::take(cancelUpload)) {
				onstack();
			}
		}

		StickerSource source;
		QImage cutout;
		bool useCutout = false;
		bool outline = false;
		bool removing = false;
		bool preparing = false;
		bool uploading = false;
		bool finished = false; // The sticker is in the set, the box closes.
		bool fixRequested = false;
		bool issuesListed = false; // The problem is the header of the issues.
		bool shownCalled = false;
		bool exporting = false; // The file to send is being made.
		int generation = 0;
		Prepared prepared;
		QImage composed; // Type::Image: the image of prepared.
		std::optional<StickerPacks::LottieCheck> lottie;
		QString problem;
		PacksState packs;
		Target target;
		bool targetChosen = false;
		QString retryTitle; // Of a new pack with a name that was refused.
		rpl::variable<QString> info;
		rpl::variable<QString> status;
		rpl::variable<QString> targetText;
		rpl::variable<QString> issues;
		rpl::variable<bool> fixShown = false;
		Fn<void()> cancelUpload;
		Fn<void()> prepare;
		Fn<void()> submit;
		Fn<void(bool)> createNew;
		Fn<void(StickerPacks::ExportFile, bool toBot)> sendReady;
		base::unique_qptr<Ui::PopupMenu> menu;
		base::Timer slowTimer;
	};
	const auto show = args.show;
	const auto state = box->lifetime().make_state<State>();
	const auto weak = base::make_weak(state);
	state->source = std::move(args.source);
	state->outline = state->source.cutout;
	if (const auto &create = args.presetNew) {
		state->target.create = *create;
		state->targetChosen = true;
	}
	const auto presetPackId = args.presetPackId;
	const auto createPack = std::move(args.createPack);
	const auto upload = std::move(args.upload);
	const auto openPack = std::move(args.openPack);
	const auto added = std::move(args.added);
	const auto sendFile = std::move(args.sendFile);
	const auto sendToBot = std::move(args.sendToBot);
	const auto removeBackground = std::move(args.removeBackground);
	const auto shown = std::move(args.shown);
	const auto type = state->source.type;

	if (args.title) {
		box->setTitle(std::move(args.title));
	} else {
		box->setTitle(tr::lng_oblivion_packs_add_title());
	}
	box->setWidth(st::boxWideWidth);

	// The emoji, the pack and a running upload are not lost by a click
	// past the box, it is closed by "Cancel" or Escape.
	box->setCloseByOutsideClick(false);
	const auto inner = box->verticalLayout();

	auto descriptor = ChatHelpers::EmojiPickerOverlayDescriptor{
		.aboutText = tr::lng_oblivion_packs_emoji_about(tr::now),
		.maxSelected = StickerPacks::kMaxEmoji,
		.allowExpand = true,
		.initialSelected = ParseEmoji(state->source.emoji),
	};
	const auto metrics = ChatHelpers::EmojiPickerOverlay::EstimateMetrics(
		descriptor.aboutText);
	const auto shadow = metrics.shadowExtent;
	const auto side = style::ConvertScale(kPreviewSide);
	const auto bubbleWidth = [=](int width) {
		return std::min(
			width
				- 2 * st::boxRowPadding.left()
				- shadow.left()
				- shadow.right(),
			style::ConvertScale(kPickerMaxWidth));
	};

	// The metrics are estimated for the about text in a single line,
	// while in the bubble it is wrapped: the bubble is that much taller.
	// The preview starts right under the real bubble, so that its tail
	// lies over the wallpaper-like background of the preview, where it
	// is seen in both themes (the box background has the bubble color).
	const auto aboutExtra = [&] {
		const auto &padding = st::stickersEmojiPickerPadding;
		auto about = Ui::FlatLabel(
			nullptr,
			descriptor.aboutText,
			st::stickersEmojiPickerAbout);
		const auto single = about.height();
		about.resizeToWidth(std::max(
			bubbleWidth(st::boxWideWidth) - padding.left() - padding.right(),
			1));
		return std::max(about.height() - single, 0);
	}();
	const auto previewTop = shadow.top()
		+ metrics.collapsedHeight
		+ aboutExtra
		+ style::ConvertScale(kPreviewSkip);

	// The expanded bubble covers the preview, only its shadow needs
	// the place below (the tail is not shown under the expanded one).
	const auto pickerHeight = metrics.totalExpandedHeight
		+ aboutExtra
		- metrics.tailHeight;
	const auto holder = inner->add(
		object_ptr<Ui::FixedHeightWidget>(
			inner,
			std::max(previewTop + side, pickerHeight)),
		style::margins());
	const auto preview = Ui::CreateChild<StickerPreview>(holder);
	preview->resize(side, side);
	const auto picker = Ui::CreateChild<ChatHelpers::EmojiPickerOverlay>(
		holder,
		std::move(descriptor));
	holder->widthValue(
	) | rpl::on_next([=](int width) {
		const auto &padding = st::boxRowPadding;
		preview->setGeometry(
			padding.left(),
			previewTop,
			std::max(width - padding.left() - padding.right(), side),
			side);
		const auto total = bubbleWidth(width)
			+ shadow.left()
			+ shadow.right();
		picker->setGeometry((width - total) / 2, 0, total, pickerHeight);
		picker->raise();
	}, holder->lifetime());

	inner->add(
		object_ptr<Ui::FlatLabel>(
			inner,
			state->info.value(),
			CenteredSubTextStyle()),
		st::boxRowPadding,
		style::al_top);

	const auto refreshInfo = [=] {
		state->info = (state->preparing || state->removing)
			? tr::lng_oblivion_packs_preparing(tr::now)
			: state->prepared.valid()
			? InfoText(state->prepared)
			: state->issuesListed
			? tr::lng_oblivion_packs_kind_animated(tr::now)
			: state->problem;
	};
	const auto baseImage = [=] {
		return (state->useCutout && !state->cutout.isNull())
			? state->cutout
			: state->source.image;
	};
	const auto apply = [=](WorkResult &&result) {
		state->prepared = std::move(result.prepared);
		state->composed = std::move(result.composed);
		state->problem = QString();
		state->issuesListed = false;
		if (type == Type::Lottie) {
			const auto fixed = base::take(state->fixRequested);
			state->lottie = std::move(result.lottie);
			auto issues = IssuesText(*state->lottie);
			state->fixShown = state->lottie->fixable();
			if (!state->lottie->parsed()) {
				state->problem = tr::lng_oblivion_packs_bad_lottie(tr::now);
			} else if (!state->lottie->acceptable()) {
				if (fixed || issues.isEmpty()) {
					state->problem = tr::lng_oblivion_packs_lottie_unfixable(
						tr::now);
				} else {
					// The header of the list of issues: shown right above
					// them, aligned with them, not in the centered line
					// under the preview (that one names the sticker kind).
					state->problem = tr::lng_oblivion_packs_lottie_errors(
						tr::now);
					state->issuesListed = true;
					issues = state->problem + '\n' + issues;
				}
			}
			state->issues = std::move(issues);
		} else if (!state->prepared.valid()) {
			state->problem = (type == Type::Video)
				? tr::lng_oblivion_packs_bad_video(tr::now)
				: tr::lng_oblivion_packs_bad_image(tr::now);
		}
		preview->setFrames(std::move(result.frames));
		refreshInfo();
		if (shown && !std::exchange(state->shownCalled, true)) {
			shown(box);
		}
	};
	state->prepare = [=] {
		const auto generation = ++state->generation;
		state->preparing = true;
		state->prepared = Prepared();
		state->composed = QImage();
		refreshInfo();
		auto request = WorkRequest{
			.type = type,
			.image = (type == Type::Image) ? baseImage() : QImage(),
			.data = state->source.data,
			.options = { .outline = state->outline },
			.lottie = state->lottie,
			.fix = state->fixRequested,
			.side = std::min(
				side * style::DevicePixelRatio(),
				kPreviewMaxPixels),
		};
		crl::async([=, request = std::move(request)] {
			auto result = Work(request);
			crl::on_main(weak, [=, result = std::move(result)]() mutable {
				if (state->generation == generation) {
					state->preparing = false;
					apply(std::move(result));
				}
			});
		});
	};

	if (type == Type::Image) {
		Ui::AddSkip(inner, st::boxMediumSkip);
		const auto outline = inner->add(
			object_ptr<Ui::Checkbox>(
				inner,
				tr::lng_oblivion_packs_outline(tr::now),
				state->outline,
				st::defaultBoxCheckbox),
			st::boxRowPadding);
		outline->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			state->outline = checked;
			state->prepare();
		}, outline->lifetime());

		const auto transparent = StickerPacks::HasTransparency(
			state->source.image);
		if (removeBackground && !transparent) {
			Ui::AddSkip(inner, st::boxLittleSkip);
			const auto remove = inner->add(
				object_ptr<Ui::Checkbox>(
					inner,
					tr::lng_oblivion_vision_editor_remove(tr::now),
					false,
					st::defaultBoxCheckbox),
				st::boxRowPadding);
			state->slowTimer.setCallback([=] {
				if (state->removing) {
					state->status = tr::lng_oblivion_vision_slow(tr::now);
				}
			});
			const auto removed = [=](Vision::MaskResult result) {
				state->slowTimer.cancel();
				state->removing = false;
				state->status = QString();
				if (!result.ok || result.cutout.isNull()) {
					using Notify = Ui::Checkbox::NotifyAboutChange;
					remove->setChecked(false, Notify::DontNotify);
					refreshInfo();
					show->showToast(
						tr::lng_oblivion_vision_cutout_not_found(tr::now));
					return;
				}
				const auto bounds = result.bounds.intersected(
					result.cutout.rect());
				state->cutout = bounds.isEmpty()
					? std::move(result.cutout)
					: result.cutout.copy(bounds);
				state->useCutout = remove->checked();
				if (state->useCutout && !state->outline) {
					using Notify = Ui::Checkbox::NotifyAboutChange;
					state->outline = true;
					outline->setChecked(true, Notify::DontNotify);
				}
				state->prepare();
			};
			remove->checkedChanges(
			) | rpl::on_next([=](bool checked) {
				if (!checked) {
					state->useCutout = false;
					state->prepare();
				} else if (!state->cutout.isNull()) {
					state->useCutout = true;
					state->prepare();
				} else if (!state->removing) {
					state->removing = true;
					state->status = tr::lng_oblivion_vision_cutout_progress(
						tr::now);
					state->slowTimer.callOnce(kSlowHintDelay);
					refreshInfo();
					removeBackground(
						state->source.image,
						crl::guard(weak, removed));
				}
			}, remove->lifetime());
		}
	} else if (type == Type::Lottie) {
		const auto issues = inner->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				inner,
				object_ptr<Ui::VerticalLayout>(inner)),
			style::margins());
		const auto container = issues->entity();
		Ui::AddSkip(container, st::boxMediumSkip);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				state->issues.value(),
				st::boxDividerLabel),
			st::boxRowPadding);
		const auto fix = container->add(
			object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
				container,
				::Settings::CreateButtonWithIcon(
					container,
					tr::lng_oblivion_packs_lottie_fix(),
					BoxRowButtonStyle(true))),
			style::margins());
		fix->entity()->setClickedCallback([=] {
			if (state->preparing || state->uploading || !state->lottie) {
				return;
			}
			state->fixRequested = true;
			state->prepare();
		});
		fix->toggleOn(state->fixShown.value(), anim::type::instant);
		issues->toggleOn(
			state->issues.value() | rpl::map([](const QString &text) {
				return !text.isEmpty();
			}),
			anim::type::instant);
	}

	Ui::AddSkip(inner, st::boxLittleSkip);
	const auto targetButton = ::Settings::AddButtonWithLabel(
		inner,
		tr::lng_oblivion_packs_target(),
		state->targetText.value(),
		BoxRowButtonStyle(false));

	const auto status = inner->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			inner,
			object_ptr<Ui::FlatLabel>(
				inner,
				state->status.value(),
				st::boxDividerLabel),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0)),
		style::margins());
	status->toggleOn(
		state->status.value() | rpl::map([](const QString &text) {
			return !text.isEmpty();
		}),
		anim::type::instant);
	if (args.exportAbout) {
		inner->add(
			object_ptr<Ui::FlatLabel>(
				inner,
				tr::lng_oblivion_packs_export_about(),
				st::boxDividerLabel),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	}
	Ui::AddSkip(inner, st::boxLittleSkip);

	const auto refreshTarget = [=] {
		const auto &target = state->target;
		state->targetText = target.pack
			? target.pack->title
			: target.create
			? tr::lng_oblivion_packs_target_new_named(
				tr::now,
				lt_title,
				target.create->title)
			: !state->packs.loaded
			? tr::lng_oblivion_packs_target_loading(tr::now)
			: ranges::none_of(state->packs.list, &PackInfo::regular)
			? tr::lng_oblivion_packs_target_new(tr::now)
			: tr::lng_oblivion_packs_target_choose(tr::now);
	};
	const auto choosePack = [=](const PackInfo &pack) {
		state->target = Target{ .pack = pack };
		state->targetChosen = true;
		if (!state->uploading) {
			state->status = QString();
		}
		refreshTarget();
	};
	state->createNew = [=](bool thenSubmit) {
		if (!createPack || state->uploading) {
			return;
		}
		createPack(state->retryTitle, crl::guard(weak, [=](NewPack pack) {
			state->target = Target{ .create = std::move(pack) };
			state->targetChosen = true;
			state->retryTitle = QString();
			state->status = QString();
			refreshTarget();
			if (thenSubmit) {
				state->submit();
			}
		}));
	};
	targetButton->setClickedCallback([=] {
		if (state->uploading) {
			return;
		}
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		const auto menu = state->menu.get();
		for (const auto &pack : state->packs.list) {
			if (!pack.regular()) {
				continue;
			}
			const auto action = Ui::Menu::CreateAction(
				menu,
				pack.title,
				[=] { choosePack(pack); });
			menu->addAction(base::make_unique_q<Menu::ActionWithThumbnail>(
				menu->menu(),
				menu->menu()->st(),
				action,
				pack.thumbnail ? pack.thumbnail->clone() : nullptr,
				st::menuIconStickerAdd.width()));
		}
		if (createPack) {
			if (!menu->empty()) {
				menu->addSeparator();
			}
			menu->addAction(
				tr::lng_oblivion_packs_target_new(tr::now),
				[=] { state->createNew(false); },
				&st::menuIconStickerAdd);
		}
		if (menu->empty()) {
			state->menu = nullptr;
		} else {
			menu->popup(QCursor::pos());
		}
	});

	std::move(
		args.packs
	) | rpl::on_next([=](const PacksState &packs) {
		state->packs = packs;
		const auto find = [&](uint64 id) -> const PackInfo* {
			const auto i = ranges::find(packs.list, id, &PackInfo::id);
			return (i != end(packs.list) && i->regular()) ? &*i : nullptr;
		};
		if (state->target.create) {
		} else if (state->targetChosen) {
			const auto fresh = state->target.pack
				? find(state->target.pack->id)
				: nullptr;
			if (fresh) {
				state->target.pack = *fresh;
			}
		} else if (packs.loaded) {
			const auto preset = presetPackId ? find(presetPackId) : nullptr;
			const auto last = LastPackId ? find(LastPackId) : nullptr;
			const auto first = ranges::find_if(
				packs.list,
				&PackInfo::regular);
			state->target.pack = preset
				? *preset
				: last
				? *last
				: (first != end(packs.list))
				? *first
				: std::optional<PackInfo>();
		}
		refreshTarget();
	}, box->lifetime());

	state->submit = [=] {
		if (state->uploading || state->finished || !upload) {
			return;
		} else if (state->preparing || state->removing) {
			show->showToast(tr::lng_oblivion_packs_not_ready(tr::now));
			return;
		} else if (!state->prepared.valid()) {
			show->showToast(state->problem.isEmpty()
				? tr::lng_oblivion_packs_bad_image(tr::now)
				: state->problem);
			return;
		}
		auto emoji = QString();
		for (const auto one : picker->selected()) {
			emoji.append(one->text());
		}
		if (emoji.isEmpty()) {
			show->showToast(tr::lng_oblivion_packs_emoji_required(tr::now));
			return;
		} else if (!state->target) {
			if (!state->packs.loaded) {
				show->showToast(tr::lng_oblivion_packs_loading(tr::now));
			} else {
				state->createNew(true);
			}
			return;
		}
		const auto target = state->target;
		if (target.pack && target.pack->count >= StickerPacks::kMaxStickers) {
			show->showToast(tr::lng_oblivion_packs_full(
				tr::now,
				lt_max,
				QString::number(StickerPacks::kMaxStickers)));
			return;
		}
		const auto creating = target.create.has_value();
		state->uploading = true;
		state->status = tr::lng_oblivion_packs_uploading(
			tr::now,
			lt_percent,
			FormatPercent(0.));
		auto handlers = UploadHandlers{
			.progress = crl::guard(weak, [=](float64 progress) {
				if (state->uploading) {
					state->status = tr::lng_oblivion_packs_uploading(
						tr::now,
						lt_percent,
						FormatPercent(progress));
				}
			}),
			.finishing = crl::guard(weak, [=] {
				if (state->uploading) {
					state->status = creating
						? tr::lng_oblivion_packs_creating(tr::now)
						: tr::lng_oblivion_packs_adding(tr::now);
				}
			}),
			.done = crl::guard(weak, [=](PackInfo pack, bool created) {
				state->uploading = false;
				state->finished = true;
				state->cancelUpload = nullptr;
				state->status = QString();
				LastPackId = pack.id;
				const auto link = tr::link(
					tr::lng_oblivion_packs_open(tr::now));
				show->showToast(Ui::Toast::Config{
					.text = (created
						? tr::lng_oblivion_packs_created
						: tr::lng_oblivion_packs_added)(
							tr::now,
							lt_title,
							tr::marked(pack.title),
							lt_link,
							link,
							tr::marked),
					.filter = [=](const auto &...) {
						if (openPack) {
							openPack(pack);
						}
						return false;
					},
					.duration = kToastDuration,
				});
				CloseLater(box);

				// The last: it may close or destroy anything, the box too.
				if (const auto onstack = added) {
					onstack();
				}
			}),
			.fail = crl::guard(weak, [=](QString error) {
				state->uploading = false;
				state->cancelUpload = nullptr;
				state->status = UploadErrorText(error);
				if (creating && error.contains(u"SHORT_NAME"_q)) {
					// A new name is asked for by the next "Add". The
					// choice stays with the user: a reload of the list
					// doesn't put the sticker into an existing pack.
					state->retryTitle = target.create->title;
					state->target = Target();
					state->targetChosen = true;
					refreshTarget();
				}
			}),
		};
		state->cancelUpload = upload(
			UploadRequest{
				.sticker = state->prepared,
				.emoji = emoji,
				.target = target,
			},
			std::move(handlers));
	};

	// The ready sticker as a file, an emoji and a pack are not needed.
	const auto exportReady = [=] {
		if (state->finished) {
			return false;
		} else if (state->preparing || state->removing) {
			show->showToast(tr::lng_oblivion_packs_not_ready(tr::now));
			return false;
		} else if (!state->prepared.valid()) {
			show->showToast(state->problem.isEmpty()
				? tr::lng_oblivion_packs_bad_image(tr::now)
				: state->problem);
			return false;
		}
		return true;
	};

	// The worker threads get only the data and weak pointers: what the
	// callbacks of the box hold must never be released there.
	const auto weakShow = std::weak_ptr<Ui::Show>(show);
	const auto saveFile = [=] {
		if (!exportReady()) {
			return;
		}
		const auto prepared = state->prepared;
		const auto composed = state->composed;
		const auto hint = state->source.name;
		const auto write = [=](const QString &chosen) {
			const auto type = StickerPacks::ExportTypeFor(
				prepared.format,
				QFileInfo(chosen).suffix());
			const auto path = FreeExportPath(chosen, type);

			// Not guarded by the box: the file is written even if the box
			// is closed right after the path was chosen.
			crl::async([=] {
				const auto file = StickerPacks::MakeExportFile(
					prepared,
					composed,
					type,
					hint);
				const auto written = file.valid()
					&& WriteFile(path, file.bytes);
				crl::on_main([=] {
					const auto strong = weakShow.lock();
					if (!strong || !strong->valid()) {
						return;
					}
					strong->showToast(written
						? tr::lng_oblivion_saved_to(
							tr::now,
							lt_path,
							QDir::toNativeSeparators(path))
						: tr::lng_oblivion_write_failed(tr::now));
				});
			});
		};
		FileDialog::GetWritePath(
			box.get(),
			tr::lng_oblivion_packs_export_save_title(tr::now),
			ExportFilter(prepared.format),
			SuggestedExportPath(hint, prepared.format),
			crl::guard(box, [=](QString &&result) {
				if (!result.isEmpty()) {
					write(result);
				}
			}));
	};

	state->sendReady = [=](StickerPacks::ExportFile file, bool toBot) {
		state->exporting = false;
		const auto &method = toBot ? sendToBot : sendFile;
		if (!file.valid()) {
			show->showToast(tr::lng_oblivion_packs_export_failed(tr::now));
		} else if (method) {
			// The sticker went where the user wanted, the box is done.
			method(std::move(file), crl::guard(weak, [=] {
				if (!state->uploading && !state->finished) {
					CloseLater(box);
				}
			}));
		}
	};
	const auto sendAsFile = [=](bool toBot) {
		if (state->exporting || !exportReady()) {
			return;
		}
		state->exporting = true;
		const auto prepared = state->prepared;
		const auto composed = state->composed;
		const auto hint = state->source.name;
		crl::async([=] {
			auto file = StickerPacks::MakeSendFile(prepared, composed, hint);
			crl::on_main(weak, [=, file = std::move(file)]() mutable {
				if (const auto strong = weak.get()) {
					strong->sendReady(std::move(file), toBot);
				}
			});
		});
	};

	box->addButton(tr::lng_oblivion_packs_add_button(), [=] {
		state->submit();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->addLeftButton(tr::lng_oblivion_packs_export(), [=] {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		FillFileMenu(state->menu->menu(), FileMenuArgs{
			.save = saveFile,
			.send = (sendFile
				? Fn<void()>([=] { sendAsFile(false); })
				: Fn<void()>()),
			.sendToBot = (sendToBot
				? Fn<void()>([=] { sendAsFile(true); })
				: Fn<void()>()),
		});
		state->menu->popup(QCursor::pos());
	});

	refreshTarget();
	state->prepare();
}

struct CreateArgs {
	std::shared_ptr<Ui::Show> show;
	QString title;
	QString shortName;

	// The status of the given short name (the UI snapshots).
	NameStatus status = NameStatus::Unknown;

	// The server suggestion for the title, empty if there is none.
	Fn<void(QString title, Fn<void(QString)> done)> suggest;
	Fn<void(QString name, Fn<void(NameStatus)> done)> check;
	Fn<void(NewPack)> done;
};

void CreatePackBox(not_null<Ui::GenericBox*> box, CreateArgs &&args) {
	struct State {
		base::Timer suggestTimer;
		base::Timer checkTimer;
		NameStatus status = NameStatus::Unknown;
		QString checked; // The name the status is about.
		QString autoName; // The name put there by the box itself.
		bool nameEdited = false;
		int suggestGeneration = 0;
		int checkGeneration = 0;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto suggest = std::move(args.suggest);
	const auto check = std::move(args.check);
	const auto done = std::move(args.done);
	state->status = args.status;
	state->checked = args.shortName;
	state->nameEdited = !args.shortName.isEmpty();

	box->setTitle(tr::lng_oblivion_packs_create_title());

	// The typed title and link are not lost by a click past the box.
	box->setCloseByOutsideClick(false);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_oblivion_packs_create_about(),
		st::boxLabel));
	const auto title = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_packs_field_title(),
			args.title),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	title->setMaxLength(StickerPacks::kTitleMaxLength);
	const auto name = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_packs_field_name(),
			args.shortName),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	name->setMaxLength(StickerPacks::kShortNameMaxLength);
	const auto note = box->addRow(
		object_ptr<Ui::FlatLabel>(box, QString(), st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));

	const auto refreshNote = [=] {
		using Problem = StickerPacks::NameProblem;
		const auto text = name->getLastText().trimmed();
		const auto problem = StickerPacks::CheckShortName(text);
		const auto link = ShortLink(text, false);
		const auto status = (state->checked == text)
			? state->status
			: NameStatus::Checking;
		auto color = std::optional<QColor>();
		auto message = QString();
		if (problem == Problem::Empty) {
			message = tr::lng_oblivion_packs_name_hint(tr::now);
		} else if (problem != Problem::None) {
			message = NameProblemText(problem);
			color = st::boxTextFgError->c;
		} else if (status == NameStatus::Available) {
			message = tr::lng_oblivion_packs_name_available(
				tr::now,
				lt_link,
				link);
			color = st::boxTextFgGood->c;
		} else if (status == NameStatus::Occupied) {
			message = tr::lng_oblivion_packs_name_occupied(tr::now);
			color = st::boxTextFgError->c;
		} else if (status == NameStatus::Invalid) {
			message = tr::lng_oblivion_packs_name_invalid(tr::now);
			color = st::boxTextFgError->c;
		} else if (status == NameStatus::Failed) {
			message = tr::lng_oblivion_packs_name_unchecked(
				tr::now,
				lt_link,
				link);
		} else {
			message = tr::lng_oblivion_packs_name_checking(tr::now);
		}
		note->setText(message);
		note->setTextColorOverride(color);
	};
	const auto nameChanged = [=] {
		const auto text = name->getLastText().trimmed();
		state->nameEdited = !text.isEmpty() && (text != state->autoName);
		if (state->checked == text
			&& state->status != NameStatus::Unknown) {
			refreshNote();
			return;
		}
		++state->checkGeneration;
		state->checked = text;
		if (StickerPacks::CheckShortName(text)
			== StickerPacks::NameProblem::None) {
			state->status = NameStatus::Checking;
			state->checkTimer.callOnce(kNameCheckDelay);
		} else {
			state->status = NameStatus::Unknown;
			state->checkTimer.cancel();
		}
		refreshNote();
	};
	const auto setName = [=](const QString &text) {
		if (name->getLastText() == text) {
			return;
		}
		state->autoName = text;
		name->setText(text);
		nameChanged();
	};
	state->checkTimer.setCallback([=] {
		const auto text = state->checked;
		const auto generation = state->checkGeneration;
		const auto apply = crl::guard(box, [=](NameStatus status) {
			if (state->checkGeneration == generation) {
				state->status = status;
				refreshNote();
			}
		});
		if (check) {
			check(text, apply);
		} else {
			apply(NameStatus::Failed);
		}
	});
	state->suggestTimer.setCallback([=] {
		const auto text = title->getLastText().trimmed();
		const auto generation = state->suggestGeneration;
		if (!suggest || text.isEmpty() || state->nameEdited) {
			return;
		}
		suggest(text, crl::guard(box, [=](QString suggested) {
			if (state->suggestGeneration == generation
				&& !state->nameEdited
				&& !suggested.isEmpty()) {
				setName(suggested);
			}
		}));
	});
	const auto titleChanged = [=] {
		++state->suggestGeneration;
		if (state->nameEdited) {
			return;
		}
		const auto text = title->getLastText().trimmed();
		setName(StickerPacks::ShortNameFromTitle(text));
		if (!text.isEmpty()) {
			state->suggestTimer.callOnce(kNameCheckDelay);
		}
	};
	title->changes() | rpl::on_next(titleChanged, title->lifetime());
	name->changes() | rpl::on_next(nameChanged, name->lifetime());

	const auto submit = [=] {
		const auto titleText = title->getLastText().trimmed();
		const auto nameText = name->getLastText().trimmed();
		const auto status = (state->checked == nameText)
			? state->status
			: NameStatus::Checking;
		if (titleText.isEmpty()) {
			title->showError();
			return;
		} else if (StickerPacks::CheckShortName(nameText)
				!= StickerPacks::NameProblem::None
			|| status == NameStatus::Occupied
			|| status == NameStatus::Invalid) {
			name->showError();
			return;
		}
		CloseLater(box);
		if (done) {
			done(NewPack{ .title = titleText, .shortName = nameText });
		}
	};
	title->submits() | rpl::on_next([=] {
		name->setFocus();
	}, title->lifetime());
	name->submits() | rpl::on_next(submit, name->lifetime());

	box->setFocusCallback([=] {
		title->setFocusFast();
	});
	box->addButton(tr::lng_oblivion_packs_next(), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });

	if (args.title.isEmpty() || !args.shortName.isEmpty()) {
		if (state->status == NameStatus::Unknown
			&& !args.shortName.isEmpty()) {
			state->checked = QString();
			nameChanged();
		} else {
			refreshNote();
		}
	} else {
		titleChanged();
	}
}

struct RenameArgs {
	std::shared_ptr<Ui::Show> show;
	QString title;

	// done gets the error type, empty on success.
	Fn<void(QString title, Fn<void(QString)> done)> save;
};

void RenamePackBox(not_null<Ui::GenericBox*> box, RenameArgs &&args) {
	struct State {
		bool saving = false;
	};
	const auto show = args.show;
	const auto save = std::move(args.save);
	const auto state = box->lifetime().make_state<State>();

	box->setTitle(tr::lng_oblivion_packs_rename_title());
	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_packs_field_title(),
			args.title));
	field->setMaxLength(StickerPacks::kTitleMaxLength);
	box->setFocusCallback([=] {
		field->setFocusFast();
		field->selectAll();
	});

	const auto submit = [=] {
		const auto text = field->getLastText().trimmed();
		if (text.isEmpty()) {
			field->showError();
			return;
		} else if (state->saving || !save) {
			return;
		}
		state->saving = true;
		save(text, crl::guard(box, [=](QString error) {
			state->saving = false;
			if (error.isEmpty()) {
				show->showToast(tr::lng_oblivion_packs_renamed(tr::now));
				CloseLater(box);
			} else {
				field->showError();
				show->showToast(tr::lng_oblivion_packs_action_failed(
					tr::now,
					lt_error,
					error));
			}
		}));
	};
	field->submits() | rpl::on_next(submit, field->lifetime());

	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

class PackRow final : public Ui::RippleButton {
public:
	PackRow(QWidget *parent, const PackInfo &pack);
	~PackRow();

	[[nodiscard]] rpl::producer<> menuRequests() const {
		return _menuRequests.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	const style::PeerListItem &_st;
	const not_null<Ui::IconButton*> _more;
	const std::shared_ptr<Ui::DynamicImage> _thumbnail;
	const QString _title;
	const QString _status;
	rpl::event_stream<> _menuRequests;

};

PackRow::PackRow(QWidget *parent, const PackInfo &pack)
: RippleButton(parent, st::defaultRippleAnimation)
, _st(st::defaultPeerListItem)
, _more(Ui::CreateChild<Ui::IconButton>(this, st::themesMenuToggle))
, _thumbnail(pack.thumbnail ? pack.thumbnail->clone() : nullptr)
, _title(pack.title)
, _status(CountText(pack)
	+ QString::fromUtf8(" \xC2\xB7 ")
	+ ShortLink(pack.shortName, pack.emoji)) {
	_more->setClickedCallback([=] {
		_menuRequests.fire({});
	});
	if (_thumbnail) {
		_thumbnail->subscribeToUpdates([=] { update(); });
	}
	setAccessibleName(_title);
	resize(width(), _st.height);
}

PackRow::~PackRow() {
	if (_thumbnail) {
		_thumbnail->subscribeToUpdates(nullptr);
	}
}

int PackRow::resizeGetHeight(int newWidth) {
	_more->moveToRight(
		std::max(st::boxTitleMenu.width / 2 - _more->width() / 2, 0),
		(_st.height - _more->height()) / 2,
		newWidth);
	return _st.height;
}

void PackRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto over = isOver() || isDown();
	p.fillRect(e->rect(), over ? st::windowBgOver : st::windowBg);
	paintRipple(p, 0, 0);

	// The cover is centered under the round icon of the "create" button,
	// the texts are aligned with its text.
	const auto size = style::ConvertScale(kThumbnailSize);
	const auto center = st::settingsButtonActive.iconLeft
		+ st::settingsIconAdd.width() / 2;
	const auto cover = QRect(
		center - size / 2,
		(height() - size) / 2,
		size,
		size);
	if (_thumbnail) {
		const auto image = _thumbnail->image(size);
		if (!image.isNull()) {
			const auto fitted = (image.size() / image.devicePixelRatio())
				.scaled(cover.size(), Qt::KeepAspectRatio);
			auto hq = PainterHighQualityEnabler(p);
			p.drawImage(
				QRect(
					cover.x() + (cover.width() - fitted.width()) / 2,
					cover.y() + (cover.height() - fitted.height()) / 2,
					fitted.width(),
					fitted.height()),
				image);
		}
	} else {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgRipple);
		const auto radius = float64(st::roundRadiusLarge);
		p.drawRoundedRect(cover, radius, radius);
		st::menuIconStickers.paintInCenter(p, cover);
	}

	const auto left = st::settingsButtonActive.padding.left();
	const auto textWidth = _more->x() - left;
	if (textWidth <= 0) {
		return;
	}
	p.setFont(st::semiboldFont);
	p.setPen(st::contactsNameFg);
	p.drawTextLeft(
		left,
		_st.namePosition.y(),
		width(),
		st::semiboldFont->elided(_title, textWidth));
	p.setFont(st::normalFont);
	p.setPen(over ? st::windowSubTextFgOver : st::windowSubTextFg);
	p.drawTextLeft(
		left,
		_st.statusPosition.y(),
		width(),
		st::normalFont->elided(_status, textWidth));
}

void PackRow::contextMenuEvent(QContextMenuEvent *e) {
	e->accept();
	_menuRequests.fire({});
}

QImage PackRow::prepareRippleMask() const {
	return Ui::RippleAnimation::RectMask(size());
}

struct ListArgs {
	std::shared_ptr<Ui::Show> show;
	rpl::producer<PacksState> packs;
	Fn<void()> reload;
	Fn<void()> create;
	Fn<void(PackInfo)> open;
	Fn<void(PackInfo)> addSticker;
	Fn<void(PackInfo)> rename;
	Fn<void(PackInfo)> remove;
};

void PacksListBox(not_null<Ui::GenericBox*> box, ListArgs &&args) {
	struct State {
		base::unique_qptr<Ui::PopupMenu> menu;
		rpl::variable<QString> message;
	};
	const auto show = args.show;
	const auto reload = std::move(args.reload);
	const auto create = std::move(args.create);
	const auto open = std::move(args.open);
	const auto addSticker = std::move(args.addSticker);
	const auto rename = std::move(args.rename);
	const auto remove = std::move(args.remove);
	const auto state = box->lifetime().make_state<State>();

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_tools_sticker_packs());

	const auto content = box->verticalLayout();
	const auto createButton = ::Settings::AddButtonWithIcon(
		content,
		tr::lng_oblivion_packs_create(),
		st::settingsButtonActive,
		{
			&st::settingsIconAdd,
			::Settings::IconType::Round,
			&st::windowBgActive,
		});
	createButton->setClickedCallback([=] {
		if (create) {
			create();
		}
	});

	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto message = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				state->message.value(),
				st::membersAbout),
			st::boxRowPadding + style::margins(
				0,
				st::boxMediumSkip,
				0,
				st::boxMediumSkip)),
		style::margins(),
		style::al_top);
	const auto retry = content->add(
		object_ptr<Ui::SlideWrap<Ui::LinkButton>>(
			content,
			object_ptr<Ui::LinkButton>(
				content,
				tr::lng_oblivion_packs_retry(tr::now),
				st::boxLinkButton),
			style::margins(0, 0, 0, st::boxMediumSkip)),
		style::margins(),
		style::al_top);
	retry->entity()->setClickedCallback([=] {
		if (reload) {
			state->message = tr::lng_oblivion_packs_loading(tr::now);
			retry->toggle(false, anim::type::instant);
			reload();
		}
	});
	const auto about = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	Ui::AddSkip(about->entity());
	Ui::AddDividerText(about->entity(), tr::lng_oblivion_packs_about());

	const auto showMenu = [=](const PackInfo &pack) {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		const auto menu = state->menu.get();
		const auto addAction = Ui::Menu::CreateAddActionCallback(menu);
		if (open) {
			addAction(
				tr::lng_oblivion_packs_open(tr::now),
				[=] { open(pack); },
				&st::menuIconStickers);
		}
		if (addSticker && pack.regular()) {
			addAction(
				tr::lng_oblivion_packs_add_file(tr::now),
				[=] { addSticker(pack); },
				&st::menuIconStickerAdd);
		}
		addAction(
			tr::lng_oblivion_packs_copy_link(tr::now),
			[=] {
				QGuiApplication::clipboard()->setText(
					StickerPacks::PackLink(pack.shortName, pack.emoji));
				show->showToast(tr::lng_oblivion_packs_link_copied(tr::now));
			},
			&st::menuIconLink);
		if (rename) {
			addAction(
				tr::lng_oblivion_packs_rename(tr::now),
				[=] { rename(pack); },
				&st::menuIconEdit);
		}
		if (remove) {
			addAction({
				.text = tr::lng_oblivion_packs_delete(tr::now),
				.handler = [=] { remove(pack); },
				.icon = &st::menuIconDeleteAttention,
				.isAttention = true,
			});
		}
		menu->popup(QCursor::pos());
	};

	std::move(
		args.packs
	) | rpl::on_next([=](const PacksState &packs) {
		list->clear();
		for (const auto &pack : packs.list) {
			const auto row = list->add(object_ptr<PackRow>(list, pack));
			row->setClickedCallback([=] {
				if (open) {
					open(pack);
				} else {
					showMenu(pack);
				}
			});
			row->menuRequests() | rpl::on_next([=] {
				showMenu(pack);
			}, row->lifetime());
		}

		const auto empty = packs.list.empty();
		const auto failed = packs.failed && empty;
		state->message = !packs.loaded
			? tr::lng_oblivion_packs_loading(tr::now)
			: failed
			? tr::lng_oblivion_packs_failed(tr::now)
			: tr::lng_oblivion_packs_empty(tr::now);
		message->toggle(empty, anim::type::instant);
		retry->toggle(failed, anim::type::instant);
		about->toggle(!empty, anim::type::instant);
	}, box->lifetime());

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void OpenPack(
		not_null<Window::SessionController*> controller,
		const PackInfo &pack) {
	controller->show(Box<StickerSetBox>(
		controller->uiShow(),
		StickerSetIdentifier{ .id = pack.id, .accessHash = pack.accessHash },
		(pack.emoji
			? Data::StickersType::Emoji
			: pack.masks
			? Data::StickersType::Masks
			: Data::StickersType::Stickers)));
}

void ShowCreateBox(
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<PacksApi> api,
		QString title,
		Fn<void(NewPack)> done) {
	const auto copy = show;
	copy->showBox(Box(CreatePackBox, CreateArgs{
		.show = std::move(show),
		.title = std::move(title),
		.suggest = [=](QString title, Fn<void(QString)> done) {
			api->suggestName(title, std::move(done));
		},
		.check = [=](QString name, Fn<void(NameStatus)> done) {
			api->checkName(name, std::move(done));
		},
		.done = std::move(done),
	}));
}

// The results are moved to the main thread together with the callback,
// so whatever the callback holds is never released on the worker.
void RemoveBackgroundAsync(QImage image, MaskDone done) {
	crl::async([image = std::move(image), done = std::move(done)]() mutable {
		auto result = Vision::RemoveBackground(image);
		if (!result.ok) {
			LOG(("Oblivion Vision Error: no background removal, %1."
				).arg(result.error));
		}
		crl::on_main([
				done = std::move(done),
				result = std::move(result)]() mutable {
			done(std::move(result));
		});
	});
}

[[nodiscard]] QString ThreadName(not_null<Data::Thread*> thread) {
	if (const auto topic = thread->asTopic()) {
		return topic->title();
	}
	const auto peer = thread->peer();
	return peer->isSelf() ? tr::lng_saved_messages(tr::now) : peer->name();
}

[[nodiscard]] bool CheckCanSendFile(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<Data::Thread*> thread) {
	const auto peer = thread->peer();
	const auto restriction = Data::RestrictionError(
		peer,
		ChatRestriction::SendFiles);
	if (restriction) {
		Data::ShowSendErrorToast(show, peer, restriction);
		return false;
	} else if (!Data::CanSend(thread, ChatRestriction::SendFiles)) {
		show->showToast(tr::lng_oblivion_packs_export_restricted(tr::now));
		return false;
	}
	const auto error = GetErrorForSending(
		thread,
		{ .messagesCount = 1 });
	if (error) {
		Data::ShowSendErrorToast(show, peer, error);
		return false;
	}
	return true;
}

// What FileLoadTask makes of a file it knows nothing about: a document
// with the name only (and the image size of a PNG, as for any image sent
// as a file). The usual way, ApiWrap::sendFiles(), looks into the content:
// a WebP of the sticker size gets the sticker attribute there and goes
// out as a sticker, which the @Stickers bot doesn't accept as a file.
// See StickerPacks::ExportSendsForcedFile() about the "force file" flag.
[[nodiscard]] std::shared_ptr<FilePrepareResult> PrepareSendFile(
		not_null<Main::Session*> session,
		FileLoadTo to,
		const StickerPacks::ExportFile &file) {
	const auto id = base::RandomValue<DocumentId>();
	auto attributes = QVector<MTPDocumentAttribute>(
		1,
		MTP_documentAttributeFilename(MTP_string(file.name)));
	if (file.type == StickerPacks::ExportType::Png && !file.size.isEmpty()) {
		attributes.push_back(MTP_documentAttributeImageSize(
			MTP_int(file.size.width()),
			MTP_int(file.size.height())));
	}
	auto result = MakePreparedFile({
		.id = id,
		.type = SendMediaType::File,
		.to = std::move(to),
	});
	result->filename = file.name;
	result->filemime = file.mime;
	result->content = file.bytes;
	result->filesize = file.bytes.size();
	result->forceFile = StickerPacks::ExportSendsForcedFile(file.type);
	result->document = MTP_document(
		MTP_flags(0),
		MTP_long(id),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(base::unixtime::now()),
		MTP_string(file.mime),
		MTP_long(file.bytes.size()),
		MTP_vector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(session->mainDcId()),
		MTP_vector<MTPDocumentAttribute>(std::move(attributes)));
	return result;
}

// The message is made the way FileLoadTask::finish() makes it for any
// attached file: Api::SendConfirmedFile() with the usual send action, so
// the topic, the price of a message and the scheduling of the ghost mode
// work as for every other file.
void SendStickerFile(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		StickerPacks::ExportFile file,
		Api::SendOptions options,
		Fn<void()> sent) {
	if (!file.valid()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto weakThread = base::make_weak(thread);
	const auto payment = std::make_shared<SendPaymentHelper>();
	const auto withPaymentApproved = [=](int approved) {
		payment->clear();
		const auto strong = weak.get();
		const auto target = weakThread.get();
		if (strong && target) {
			auto copy = options;
			copy.starsApproved = approved;
			SendStickerFile(strong, target, file, copy, sent);
		}
	};
	const auto checked = payment->check(
		controller,
		thread->peer(),
		options,
		1,
		withPaymentApproved);
	if (!checked) {
		return;
	}
	const auto session = &thread->session();
	auto action = Api::SendAction(thread, options);
	if (!action.replyTo.monoforumPeerId) {
		action.replyTo.monoforumPeerId = thread->monoforumPeerId();
	}
	const auto prepared = PrepareSendFile(
		session,
		FileLoadTo(
			action.history->peer->id,
			action.options,
			action.replyTo,
			MsgId()),
		file);

	// A file that has the content only and isn't kept in the cache gets
	// a copy in the downloads folder from the uploader (file_upload.cpp),
	// "sticker (2).png" after each send. The flag is set before the
	// uploader fills this document and stays through documentConvert()
	// of the sent message, so the bytes live in the cache, as they do
	// for the pack uploads in PrepareUploadFile().
	session->data().document(prepared->id)->forceToCache(true);

	Api::SendConfirmedFile(session, prepared);
	controller->uiShow()->showToast(tr::lng_oblivion_packs_export_sent(
		tr::now,
		lt_chat,
		ThreadName(thread)));
	if (const auto onstack = sent) {
		onstack();
	}
}

struct AddBoxOptions {
	uint64 presetPackId = 0;
	std::optional<NewPack> presetNew;
	Fn<void()> added;
	bool converter = false;
};

void ShowAddBox(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<PacksApi> api,
		StickerSource source,
		AddBoxOptions options = {}) {
	// A file dialog or a download may end after the app was locked: the
	// box would be shown above the passcode screen.
	if (controller->window().locked()) {
		return;
	} else if (source.empty()) {
		controller->showToast(tr::lng_oblivion_packs_file_failed(tr::now));
		return;
	}
	const auto show = controller->uiShow();
	const auto weak = base::make_weak(controller);
	const auto sendTo = [=](
			not_null<Data::Thread*> thread,
			const StickerPacks::ExportFile &file,
			const Fn<void()> &sent) {
		const auto strong = weak.get();
		if (strong
			&& !strong->window().locked()
			&& (&thread->session() == &strong->session())
			&& CheckCanSendFile(show, thread)) {
			SendStickerFile(strong, thread, file, {}, sent);
		}
	};
	auto args = AddArgs{
		.show = show,
		.source = std::move(source),
		.packs = api->state(),
		.title = (options.converter
			? tr::lng_oblivion_tools_sticker_converter()
			: rpl::producer<QString>()),
		.exportAbout = options.converter,
		.sendFile = [=](StickerPacks::ExportFile file, Fn<void()> sent) {
			const auto strong = weak.get();
			if (!strong || strong->window().locked()) {
				return;
			}
			const auto chooser = Window::ShowChooseRecipientBox(
				strong,
				[=](not_null<Data::Thread*> thread) {
					if (!CheckCanSendFile(show, thread)) {
						return false;
					}
					// After the chooser box is closed.
					const auto chosen = base::make_weak(thread);
					crl::on_main([=] {
						if (const auto target = chosen.get()) {
							sendTo(target, file, sent);
						}
					});
					return true;
				},
				tr::lng_oblivion_packs_export_send_title());
			if (const auto box = chooser.get()) {
				// A click outside closes all the boxes: the one with the
				// sticker under the list of the chats too.
				box->setCloseByOutsideClick(false);
			}
		},
		.sendToBot = [=](StickerPacks::ExportFile file, Fn<void()> sent) {
			// The answer comes while the box with the sticker is shown:
			// the request is cancelled together with the last box.
			api->resolveStickersBot([=](UserData *bot) {
				if (!bot) {
					show->showToast(
						tr::lng_oblivion_packs_export_bot_failed(tr::now));
					return;
				}
				sendTo(bot->owner().history(bot), file, sent);
			});
		},
		.presetPackId = options.presetPackId,
		.presetNew = std::move(options.presetNew),
		.createPack = [=](QString title, Fn<void(NewPack)> done) {
			ShowCreateBox(show, api, std::move(title), std::move(done));
		},
		.upload = [=](UploadRequest request, UploadHandlers handlers) {
			return api->upload(std::move(request), std::move(handlers));
		},
		.openPack = [=](PackInfo pack) {
			if (const auto strong = weak.get()) {
				OpenPack(strong, pack);
			}
		},
		.added = std::move(options.added),
	};
	if (Vision::BackgroundRemovalSupported()) {
		args.removeBackground = RemoveBackgroundAsync;
	}
	controller->show(Box(AddStickerBox, std::move(args)));
	api->reload();
}

struct OpenedFile {
	StickerPacks::FileKind kind = StickerPacks::FileKind::Unknown;
	StickerSource source;
	QString path; // FileKind::Video: what the trim box opens.
	QByteArray content;
	QString name;
	bool failed = false;
};

[[nodiscard]] OpenedFile OpenStickerFile(
		const QString &path,
		QByteArray content) {
	using Kind = StickerPacks::FileKind;
	auto result = OpenedFile();
	auto file = QFile(path);
	auto size = qint64(content.size());
	if (!path.isEmpty()) {
		if (!file.open(QIODevice::ReadOnly)) {
			result.failed = true;
			return result;
		}
		size = file.size();
		content = file.read(std::min(size, kSniffSize));
		result.name = QFileInfo(path).completeBaseName();
	}
	const auto whole = (content.size() == size);
	result.kind = StickerPacks::DetectFileKind(path, content);
	switch (result.kind) {
	case Kind::Image: {
		auto image = path.isEmpty()
			? Photo::LoadImage(content)
			: Photo::LoadImage(path);
		result.failed = image.isNull();
		result.source = StickerSource::FromImage(std::move(image));
	} break;
	case Kind::Lottie: {
		if (!whole && size <= kMaxLottieFile) {
			content.append(file.readAll());
		}
		result.failed = (content.size() != size);
		result.source = StickerSource::FromLottie(std::move(content));
	} break;
	case Kind::VideoSticker: {
		result.failed = !whole;
		result.source = StickerSource::FromVideo(std::move(content));
	} break;
	case Kind::Video: {
		result.path = path;
		if (path.isEmpty()) {
			result.content = std::move(content);
		}
	} break;
	case Kind::Unknown: break;
	}
	result.source.name = result.name;
	return result;
}

void ChooseStickerFile(
		not_null<Window::SessionController*> controller,
		Fn<void(StickerSource)> done) {
	const auto weak = base::make_weak(controller);
	const auto filter = tr::lng_oblivion_packs_filter(tr::now)
		+ u" (*.png *.jpg *.jpeg *.webp *.heic *.bmp *.tgs *.json *.webm"
		" *.mp4 *.mov *.m4v *.mkv *.gif);;"_q
		+ tr::lng_oblivion_round_filter_all(tr::now)
		+ u" (*)"_q;

	// Shared and moved to the main thread together with the file, so
	// that whatever done holds is never released on the worker thread.
	using Opened = Fn<void(OpenedFile&&)>;
	const auto opened = std::make_shared<Opened>([=](OpenedFile &&file) {
		using Kind = StickerPacks::FileKind;
		const auto strong = weak.get();
		if (!strong || strong->window().locked()) {
			return;
		} else if (file.failed) {
			strong->showToast(tr::lng_oblivion_packs_file_failed(tr::now));
		} else if (file.kind == Kind::Unknown) {
			strong->showToast(tr::lng_oblivion_packs_file_unknown(tr::now));
		} else if (file.kind != Kind::Video) {
			done(std::move(file.source));
		} else {
			const auto name = file.name;
			ShowVideoStickerTrim(
				strong->uiShow(),
				file.path,
				file.content,
				[=](QByteArray webm) {
					auto source = StickerSource::FromVideo(std::move(webm));
					source.name = name;
					done(std::move(source));
				});
		}
	});
	FileDialog::GetOpenPath(
		controller->widget().get(),
		tr::lng_oblivion_packs_choose_file(tr::now),
		filter,
		crl::guard(controller, [=](FileDialog::OpenResult &&result) {
			// The app may get locked while the file dialog is open.
			if (controller->window().locked()) {
				return;
			}
			const auto path = result.paths.isEmpty()
				? QString()
				: result.paths.front();
			auto content = path.isEmpty()
				? std::move(result.remoteContent)
				: QByteArray();
			if (path.isEmpty() && content.isEmpty()) {
				return;
			}
			crl::async([
					path,
					content = std::move(content),
					opened = opened]() mutable {
				auto file = OpenStickerFile(path, std::move(content));
				crl::on_main([
						file = std::move(file),
						opened = std::move(opened)]() mutable {
					(*opened)(std::move(file));
				});
			});
		}));
}

[[nodiscard]] bool IsVideoDocument(not_null<DocumentData*> document) {
	return !document->sticker()
		&& (document->isVideoFile()
			|| document->isAnimation()
			|| document->isVideoMessage()
			|| document->mimeString().startsWith(
				u"video/"_q,
				Qt::CaseInsensitive));
}

// Snapshot scenes (OBLIVION_SELFTEST=ui), see oblivion_ui_snapshots.h.

[[nodiscard]] QImage SampleSticker(QColor body, QColor head) {
	const auto size = QSize(900, 700);
	auto image = QImage(size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto p = QPainter(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(body);
	p.drawRoundedRect(QRect(270, 360, 360, 340), 150, 150);
	p.setBrush(head);
	p.drawEllipse(QPoint(450, 240), 170, 170);
	p.setBrush(QColor(0x2d, 0x1f, 0x1a));
	p.drawEllipse(QPoint(390, 220), 18, 24);
	p.drawEllipse(QPoint(510, 220), 18, 24);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(0x2d, 0x1f, 0x1a), 12, Qt::SolidLine, Qt::RoundCap));
	p.drawArc(QRect(380, 240, 140, 90), 200 * 16, 140 * 16);
	p.end();
	return image;
}

[[nodiscard]] PacksState SamplePacks() {
	const auto pack = [](
			uint64 id,
			QString title,
			QString shortName,
			int count,
			QColor body,
			QColor head) {
		return PackInfo{
			.id = id,
			.title = std::move(title),
			.shortName = std::move(shortName),
			.count = count,
			.thumbnail = std::make_shared<ImageThumbnail>(
				StickerPacks::ComposeSticker(
					SampleSticker(body, head),
					{ .outline = true })),
		};
	};
	auto result = PacksState{ .loaded = true };
	result.list.push_back(pack(
		1,
		u"Коты Oblivion"_q,
		u"oblivion_cats"_q,
		24,
		QColor(0x6c, 0x5c, 0xe7),
		QColor(0xff, 0xd7, 0xa8)));
	result.list.push_back(pack(
		2,
		u"Мемы с работы"_q,
		u"work_memes_2026"_q,
		3,
		QColor(0x00, 0x9e, 0x8e),
		QColor(0xf2, 0xa1, 0x6b)));
	result.list.push_back(pack(
		3,
		u"Видеостикеры из поездки в горы прошлым летом"_q,
		u"mountains_trip_video_stickers"_q,
		120,
		QColor(0xe1, 0x70, 0x55),
		QColor(0xff, 0xea, 0xa7)));
	auto &emoji = result.list.emplace_back(pack(
		4,
		u"Мои эмодзи"_q,
		u"my_tiny_emoji"_q,
		41,
		QColor(0x09, 0x84, 0xe3),
		QColor(0xdf, 0xe6, 0xe9)));
	emoji.emoji = true;
	return result;
}

[[nodiscard]] object_ptr<Ui::BoxContent> SampleAddBox(
		std::shared_ptr<Ui::Show> show,
		StickerSource source,
		PacksState packs,
		bool converter = false) {
	return Box(AddStickerBox, AddArgs{
		.show = std::move(show),
		.source = std::move(source),
		.packs = rpl::single(std::move(packs)),
		.title = (converter
			? tr::lng_oblivion_tools_sticker_converter()
			: rpl::producer<QString>()),
		.exportAbout = converter,
		.sendFile = [](StickerPacks::ExportFile, Fn<void()>) {},
		.sendToBot = [](StickerPacks::ExportFile, Fn<void()>) {},
		.createPack = [](QString, Fn<void(NewPack)>) {},
		.upload = [](UploadRequest, UploadHandlers) {
			return Fn<void()>();
		},
		.removeBackground = [](QImage, MaskDone) {},
		.shown = [](not_null<Ui::GenericBox*> box) {
			box->setProperty(kSnapshotReady, true);
		},
	});
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	const auto size = QSize(style::ConvertScale(kSceneWidth), 0);
	const auto ready = [](not_null<QWidget*> widget) {
		return widget->property(kSnapshotReady).toBool();
	};
	const auto list = [](PacksState packs) {
		return [=](std::shared_ptr<Ui::Show> show) {
			return Box(PacksListBox, ListArgs{
				.show = show,
				.packs = rpl::single(packs),
				.reload = [] {},
				.create = [] {},
				.open = [](PackInfo) {},
				.addSticker = [](PackInfo) {},
				.rename = [](PackInfo) {},
				.remove = [](PackInfo) {},
			});
		};
	};

	RegisterBoxScene(u"packs_list"_q, size, list(SamplePacks()));
	RegisterBoxScene(
		u"packs_list_empty"_q,
		size,
		list(PacksState{ .loaded = true }));
	RegisterBoxScene(
		u"packs_list_failed"_q,
		size,
		list(PacksState{ .loaded = true, .failed = true }));

	RegisterBoxScene(u"pack_create"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(CreatePackBox, CreateArgs{
			.show = show,
			.title = u"Коты Oblivion"_q,
			.shortName = u"oblivion_cats"_q,
			.status = NameStatus::Available,
		});
	});
	RegisterBoxScene(u"pack_create_occupied"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(CreatePackBox, CreateArgs{
			.show = show,
			.title = u"Коты"_q,
			.shortName = u"cats"_q,
			.status = NameStatus::Occupied,
		});
	});
	RegisterBoxScene(u"pack_rename"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(RenamePackBox, RenameArgs{
			.show = show,
			.title = u"Коты Oblivion"_q,
		});
	});

	// A photo: the outline and "Remove background" checkboxes.
	RegisterScene({
		.name = u"add_sticker"_q,
		.size = size,
		.box = [](std::shared_ptr<Ui::Show> show) {
			auto photo = QImage(1200, 800, QImage::Format_ARGB32_Premultiplied);
			{
				auto p = QPainter(&photo);
				auto sky = QLinearGradient(0, 0, 0, 800);
				sky.setColorAt(0., QColor(0x2b, 0x5f, 0xa8));
				sky.setColorAt(1., QColor(0xf7, 0xd0, 0x8a));
				p.fillRect(photo.rect(), sky);
				p.drawImage(
					QPoint(150, 100),
					SampleSticker(
						QColor(0x6c, 0x5c, 0xe7),
						QColor(0xff, 0xd7, 0xa8)));
			}
			auto source = StickerSource::FromImage(std::move(photo));
			source.emoji = QString::fromUtf8("\xF0\x9F\x98\x8E");
			return SampleAddBox(
				std::move(show),
				std::move(source),
				SamplePacks());
		},
		.ready = ready,
	});

	// A cutout: the white outline is on, no packs yet.
	RegisterScene({
		.name = u"add_sticker_cutout"_q,
		.size = size,
		.box = [](std::shared_ptr<Ui::Show> show) {
			auto source = StickerSource::FromImage(SampleSticker(
				QColor(0x00, 0x9e, 0x8e),
				QColor(0xf2, 0xa1, 0x6b)));
			source.cutout = true;
			return SampleAddBox(
				std::move(show),
				std::move(source),
				PacksState{ .loaded = true });
		},
		.ready = ready,
	});

	// An animated sticker from the resources, looped in the preview.
	RegisterScene({
		.name = u"add_sticker_animated"_q,
		.size = size,
		.box = [](std::shared_ptr<Ui::Show> show) {
			auto file = QFile(u":/animations/cake.tgs"_q);
			auto source = StickerSource::FromLottie(
				file.open(QIODevice::ReadOnly)
					? file.readAll()
					: QByteArray("{}"));
			source.emoji = QString::fromUtf8("\xF0\x9F\x8E\x82");
			return SampleAddBox(
				std::move(show),
				std::move(source),
				SamplePacks());
		},
		.ready = ready,
	});

	// An animation that Telegram would reject: the problems and the fix.
	RegisterScene({
		.name = u"add_sticker_lottie"_q,
		.size = size,
		.box = [](std::shared_ptr<Ui::Show> show) {
			// The cake on a 256x256 canvas at 25 fps, so that the preview
			// is not empty, a blank composition if it can't be made.
			auto file = QFile(u":/animations/cake.tgs"_q);
			auto document = file.open(QIODevice::ReadOnly)
				? LottieEdit::Document::FromData(file.readAll())
				: LottieEdit::Document();
			if (document) {
				auto resized = LottieEdit::SetCanvasSize(
					document,
					QSize(256, 256),
					true);
				auto slowed = resized
					? LottieEdit::SetFrameRate(resized.document, 25., false)
					: LottieEdit::Edit();
				document = slowed ? slowed.document : LottieEdit::Document();
			}
			if (!document) {
				document = LottieEdit::Document::Blank(
					QSize(256, 256),
					25.,
					150);
			}
			return SampleAddBox(
				std::move(show),
				StickerSource::FromLottie(document.toJson()),
				SamplePacks());
		},
		.ready = ready,
	});

	// The converter: a video sticker made of a file, its own title and
	// the text about the "File" button.
	RegisterScene({
		.name = u"add_sticker_converter"_q,
		.size = size,
		.box = [](std::shared_ptr<Ui::Show> show) {
			const auto sample = SampleSticker(
				QColor(0xe1, 0x70, 0x55),
				QColor(0xff, 0xea, 0xa7)
			).scaled(
				QSize(360, 280),
				Qt::KeepAspectRatio,
				Qt::SmoothTransformation);
			auto frames = std::vector<QImage>();
			for (auto i = 0; i != 20; ++i) {
				auto frame = QImage(
					QSize(512, 384),
					QImage::Format_ARGB32_Premultiplied);
				frame.fill(QColor(0x2b, 0x5f + i * 3, 0xa8));
				auto p = QPainter(&frame);
				p.drawImage(QPoint(40 + i * 4, 60), sample);
				p.end();
				frames.push_back(std::move(frame));
			}
			const auto encoded = VideoCore::EncodeVideoSticker(frames, 50);
			auto source = (encoded.ok && !encoded.webm.isEmpty())
				? StickerSource::FromVideo(encoded.webm)
				: StickerSource::FromImage(frames.front());
			source.name = u"holiday"_q;
			source.emoji = QString::fromUtf8("\xF0\x9F\x8E\x89");
			return SampleAddBox(
				std::move(show),
				std::move(source),
				SamplePacks(),
				true);
		},
		.ready = ready,
	});

	// The menu of the "File" button in the middle of the scene, inside
	// the panel that Ui::PopupMenu paints around it: a popup window
	// itself can't be a part of the scene.
	const auto skip = style::ConvertScale(kSceneMenuSkip);
	const auto outer = st::popupMenuWithIcons.menu.widthMax + 2 * skip;
	RegisterScene({
		.name = u"add_sticker_file_menu"_q,
		.size = QSize(outer, 0),
		.create = [=](not_null<Ui::RpWidget*> parent) {
			const auto padding = st::popupMenuWithIcons.scrollPadding;
			const auto result = Ui::CreateChild<Ui::RpWidget>(parent.get());
			const auto menu = Ui::CreateChild<Ui::Menu::Menu>(
				result,
				st::popupMenuWithIcons.menu);
			FillFileMenu(menu, FileMenuArgs{
				.save = [] {},
				.send = [] {},
				.sendToBot = [] {},
			});
			const auto shadow = result->lifetime().make_state<Ui::BoxShadow>(
				st::popupMenuWithIcons.shadow);
			result->paintRequest(
			) | rpl::on_next([=] {
				const auto radius = st::popupMenuWithIcons.radius;
				const auto panel = menu->geometry().marginsAdded(padding);
				auto p = QPainter(result);
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::popupMenuWithIcons.menu.itemBg);
				p.drawRoundedRect(panel, radius, radius);
				shadow->paint(p, panel, radius);
			}, result->lifetime());
			menu->sizeValue(
			) | rpl::on_next([=](QSize menuSize) {
				menu->move(
					(outer - menuSize.width()) / 2,
					skip + padding.top());
				result->setGeometry(
					0,
					0,
					outer,
					skip
						+ padding.top()
						+ menuSize.height()
						+ padding.bottom()
						+ skip);
				result->update();
			}, result->lifetime());
			return result;
		},
		.fit = false,
	});
});

} // namespace

void ShowStickerPacks(not_null<Window::SessionController*> controller) {
	const auto show = controller->uiShow();
	const auto weak = base::make_weak(controller);
	const auto api = std::make_shared<PacksApi>(&controller->session());

	// The api is kept alive only by the boxes, they never outlive the
	// session. What waits for a file to be chosen and read can.
	const auto weakApi = std::weak_ptr(api);
	const auto addFromFile = [=](uint64 packId, std::optional<NewPack> pack) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		ChooseStickerFile(strong, [=](StickerSource source) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto shared = weakApi.lock();
			if (!shared) {
				shared = std::make_shared<PacksApi>(&strong->session());
			}
			ShowAddBox(strong, std::move(shared), std::move(source), {
				.presetPackId = packId,
				.presetNew = pack,
			});
		});
	};
	controller->show(Box(PacksListBox, ListArgs{
		.show = show,
		.packs = api->state(),
		.reload = [=] { api->reload(); },
		.create = [=] {
			ShowCreateBox(show, api, QString(), [=](NewPack pack) {
				addFromFile(0, std::move(pack));
			});
		},
		.open = [=](PackInfo pack) {
			if (const auto strong = weak.get()) {
				OpenPack(strong, pack);
			}
		},
		.addSticker = [=](PackInfo pack) {
			if (pack.count >= StickerPacks::kMaxStickers) {
				show->showToast(tr::lng_oblivion_packs_full(
					tr::now,
					lt_max,
					QString::number(StickerPacks::kMaxStickers)));
			} else {
				addFromFile(pack.id, std::nullopt);
			}
		},
		.rename = [=](PackInfo pack) {
			show->showBox(Box(RenamePackBox, RenameArgs{
				.show = show,
				.title = pack.title,
				.save = [=](QString title, Fn<void(QString)> done) {
					api->rename(pack, title, std::move(done));
				},
			}));
		},
		.remove = [=](PackInfo pack) {
			show->showBox(Ui::MakeConfirmBox({
				.text = tr::lng_oblivion_packs_delete_sure(
					lt_title,
					rpl::single(pack.title)),
				.confirmed = [=](Fn<void()> close) {
					api->remove(pack, [=](QString error) {
						show->showToast(error.isEmpty()
							? tr::lng_oblivion_packs_deleted(tr::now)
							: tr::lng_oblivion_packs_action_failed(
								tr::now,
								lt_error,
								error));
					});
					close();
				},
				.confirmText = tr::lng_box_delete(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		},
	}));
	api->reload();
}

void AddToStickerPack(
		not_null<Window::SessionController*> controller,
		StickerSource source,
		Fn<void()> added) {
	ShowAddBox(
		controller,
		std::make_shared<PacksApi>(&controller->session()),
		std::move(source),
		{ .added = std::move(added) });
}

void ShowStickerConverter(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	ChooseStickerFile(controller, [=](StickerSource source) {
		if (const auto strong = weak.get()) {
			ShowAddBox(
				strong,
				std::make_shared<PacksApi>(&strong->session()),
				std::move(source),
				{ .converter = true });
		}
	});
}

void AddToStickerPack(StickerSource source, Fn<void()> added) {
	if (!Core::IsAppLaunched()) {
		return;
	}
	const auto withSession = [](Window::Controller *window) {
		return (window && window->sessionController()) ? window : nullptr;
	};
	const auto active = withSession(Core::App().activeWindow());
	const auto window = active
		? active
		: withSession(Core::App().activePrimaryWindow());
	if (window) {
		window->activate();
		AddToStickerPack(
			window->sessionController(),
			std::move(source),
			std::move(added));
	} else if (const auto parent = QApplication::activeWindow()) {
		Ui::Toast::Show(parent, tr::lng_oblivion_packs_no_account(tr::now));
	}
}

void AddStickerPackActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<PhotoData*> photo,
		HistoryItem *item) {
	if (photo->isNull()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto context = item ? item->fullId() : FullMsgId();
	menu->addAction(tr::lng_oblivion_packs_context_sticker(tr::now), [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		LoadPhotoImage(strong, photo, context, [=](
				QImage image,
				QString name) {
			if (const auto strong = weak.get()) {
				auto source = StickerSource::FromImage(std::move(image));
				source.name = name;
				AddToStickerPack(strong, std::move(source));
			}
		}, tr::lng_oblivion_packs_context_sticker(tr::now));
	}, &st::menuIconStickerAdd);
}

void AddStickerPackActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		HistoryItem *item) {
	const auto weak = base::make_weak(controller);
	const auto context = item ? item->fullId() : FullMsgId();
	if (PhotoEditorAcceptsDocument(document)) {
		menu->addAction(tr::lng_oblivion_packs_context_sticker(tr::now), [=] {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			LoadDocumentImage(strong, document, context, [=](
					QImage image,
					QString name) {
				if (const auto strong = weak.get()) {
					auto source = StickerSource::FromImage(std::move(image));
					source.name = name;
					AddToStickerPack(strong, std::move(source));
				}
			}, tr::lng_oblivion_packs_context_sticker(tr::now));
		}, &st::menuIconStickerAdd);
	} else if (IsVideoDocument(document)) {
		menu->addAction(tr::lng_oblivion_packs_context_video(tr::now), [=] {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto name = QFileInfo(
				document->filename()).completeBaseName();
			ShowVideoStickerTrim(strong, document, context, [=](
					QByteArray webm) {
				if (const auto strong = weak.get()) {
					auto source = StickerSource::FromVideo(std::move(webm));
					source.name = name;
					AddToStickerPack(strong, std::move(source));
				}
			});
		}, &st::menuIconStickerAdd);
	}
}

} // namespace Oblivion
