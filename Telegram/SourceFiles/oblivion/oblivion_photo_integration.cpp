/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_integration.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/event_filter.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "base/weak_qptr.h"
#include "boxes/send_files_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "editor/photo_editor_common.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_photo_collage.h"
#include "oblivion/oblivion_photo_core.h"
#include "oblivion/oblivion_photo_editor.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_sticker_packs.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "settings/settings_common.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/abstract_button.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/image/image.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/format_values.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_media_view.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QImageReader>
#include <QtGui/QKeySequence>
#include <QtGui/QBrush>
#include <QtGui/QPainterPath>

namespace Oblivion {
namespace {

using SaveFormat = Photo::SaveFormat;

constexpr auto kDefaultQuality = 92;
constexpr auto kMinQuality = 10;
constexpr auto kMaxQuality = 100;
constexpr auto kPreviewMaxHeight = 240;
constexpr auto kDropZoneHeight = 176;
constexpr auto kEstimateDelay = crl::time(400);
constexpr auto kMaxDocumentSize = int64(256) * 1024 * 1024;
constexpr auto kMaxKeptOriginals = 16;
constexpr auto kKeptNoticeDuration = crl::time(8000);
constexpr auto kSceneWidth = 480;
constexpr auto kKeptOriginalsName = "oblivion_photo_originals";

struct ExportFormat {
	SaveFormat format = SaveFormat::Png;
	int quality = kDefaultQuality;

	friend inline bool operator==(
		const ExportFormat &a,
		const ExportFormat &b) = default;
};

[[nodiscard]] QString FormatTitle(SaveFormat format) {
	// Format names are the same in every language.
	switch (format) {
	case SaveFormat::Png: return u"PNG"_q;
	case SaveFormat::Jpeg: return u"JPEG"_q;
	case SaveFormat::Webp: return u"WebP"_q;
	}
	Unexpected("Format in FormatTitle.");
}

[[nodiscard]] QString FormatFilter(SaveFormat format) {
	switch (format) {
	case SaveFormat::Png:
		return tr::lng_oblivion_photo_io_filter_png(tr::now)
			+ u" (*.png)"_q;
	case SaveFormat::Jpeg:
		return tr::lng_oblivion_photo_io_filter_jpeg(tr::now)
			+ u" (*.jpg *.jpeg)"_q;
	case SaveFormat::Webp:
		return tr::lng_oblivion_photo_io_filter_webp(tr::now)
			+ u" (*.webp)"_q;
	}
	Unexpected("Format in FormatFilter.");
}

[[nodiscard]] bool HasQuality(SaveFormat format) {
	return (format != SaveFormat::Png);
}

[[nodiscard]] std::optional<SaveFormat> FormatFromExtension(
		const QString &extension) {
	const auto lower = extension.toLower();
	if (lower == u"png"_q) {
		return SaveFormat::Png;
	} else if (lower == u"jpg"_q || lower == u"jpeg"_q) {
		return SaveFormat::Jpeg;
	} else if (lower == u"webp"_q) {
		return SaveFormat::Webp;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<ExportFormat> RememberedFormat() {
	const auto parts = Get().photoExport().split(QChar(':'));
	const auto format = FormatFromExtension(parts.front());
	if (!format || !Photo::SaveFormatSupported(*format)) {
		return std::nullopt;
	}
	auto result = ExportFormat{ .format = *format };
	if (parts.size() > 1) {
		auto ok = false;
		const auto quality = parts[1].toInt(&ok);
		if (ok) {
			result.quality = std::clamp(quality, kMinQuality, kMaxQuality);
		}
	}
	return result;
}

void RememberFormat(ExportFormat format) {
	Get().setPhotoExport(Photo::SaveFormatExtension(format.format)
		+ QChar(':')
		+ QString::number(format.quality));
}

[[nodiscard]] QString AboutText(SaveFormat format, bool alpha) {
	switch (format) {
	case SaveFormat::Png:
		return tr::lng_oblivion_photo_io_png_about(tr::now);
	case SaveFormat::Jpeg:
		return alpha
			? tr::lng_oblivion_photo_io_jpeg_alpha(tr::now)
			: tr::lng_oblivion_photo_io_jpeg_about(tr::now);
	case SaveFormat::Webp:
		return tr::lng_oblivion_photo_io_webp_about(tr::now);
	}
	Unexpected("Format in AboutText.");
}

// Any pixel that is not fully opaque. Off the main thread for big images.
[[nodiscard]] bool HasTransparency(const QImage &image) {
	if (image.isNull() || !image.hasAlphaChannel()) {
		return false;
	}
	const auto format = image.format();
	const auto converted = (format == QImage::Format_ARGB32_Premultiplied
		|| format == QImage::Format_ARGB32)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const auto width = converted.width();
	for (auto y = 0, height = converted.height(); y != height; ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			converted.constScanLine(y));
		for (auto x = 0; x != width; ++x) {
			if (qAlpha(line[x]) != 255) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] bool WriteFile(const QString &path, const QByteArray &bytes) {
	auto file = QSaveFile(path);
	return file.open(QIODevice::WriteOnly)
		&& (file.write(bytes) == bytes.size())
		&& file.commit();
}

[[nodiscard]] QString SanitizeName(QString name) {
	static const auto kBad = QRegularExpression(
		u"[\\\\/:*?\"<>|\\x00-\\x1F]"_q);
	name = name.replace(kBad, u"_"_q).trimmed();
	while (name.startsWith(QChar('.'))) {
		name.remove(0, 1);
	}
	return name.left(96);
}

[[nodiscard]] QString TimeName(const QString &prefix, const QDateTime &when) {
	return prefix + when.toString(u"_yyyy-MM-dd_HH-mm-ss"_q);
}

[[nodiscard]] QString NowName() {
	return TimeName(u"image"_q, QDateTime::currentDateTime());
}

// "IMG_1234.HEIC" -> "IMG_1234_edited", so a save next to the original
// never suggests to overwrite it.
[[nodiscard]] QString EditedName(const QString &fileName) {
	const auto base = SanitizeName(QFileInfo(fileName).completeBaseName());
	return base.isEmpty() ? QString() : (base + u"_edited"_q);
}

[[nodiscard]] QString PhotoName(not_null<PhotoData*> photo) {
	const auto date = photo->date();
	return TimeName(
		u"photo"_q,
		date ? base::unixtime::parse(date) : QDateTime::currentDateTime());
}

[[nodiscard]] QString DocumentName(not_null<DocumentData*> document) {
	const auto result = EditedName(document->filename());
	return result.isEmpty() ? NowName() : result;
}

[[nodiscard]] QString SuggestedPath(const QString &name, SaveFormat format) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(
		name + QChar('.') + Photo::SaveFormatExtension(format),
		QString());
}

[[nodiscard]] QString FormatPercent(float64 progress) {
	const auto percent = int(std::round(std::clamp(progress, 0., 1.) * 100));
	return QString::number(percent) + QChar('%');
}

[[nodiscard]] bool IsImagePath(const QString &path) {
	static const auto kFormats = [] {
		auto result = base::flat_set<QByteArray>();
		for (const auto &format : QImageReader::supportedImageFormats()) {
			result.emplace(format.toLower());
		}
		result.emplace("jpg");
		result.emplace("jpeg");
		return result;
	}();
	const auto suffix = QFileInfo(path).suffix().toLower().toUtf8();
	return !suffix.isEmpty() && kFormats.contains(suffix);
}

struct MimeImage {
	QString path;
	QImage image;
};

// A local image file (Finder copy / drag) is preferred over the image
// data: it keeps the full quality, the orientation and the file name.
[[nodiscard]] QString MimeImagePath(const QMimeData *data) {
	if (!data || !data->hasUrls()) {
		return QString();
	}
	for (const auto &url : Core::ReadMimeUrls(data)) {
		if (url.isLocalFile()) {
			const auto path = url.toLocalFile();
			if (IsImagePath(path)) {
				return path;
			}
		}
	}
	return QString();
}

[[nodiscard]] bool MimeHasImage(const QMimeData *data) {
	return data && (!MimeImagePath(data).isEmpty() || data->hasImage());
}

[[nodiscard]] MimeImage ReadMimeImage(const QMimeData *data) {
	if (!data) {
		return {};
	} else if (auto path = MimeImagePath(data); !path.isEmpty()) {
		return { .path = std::move(path) };
	} else if (data->hasImage()) {
		return { .image = qvariant_cast<QImage>(data->imageData()) };
	}
	return {};
}

// Loads off the main thread with the EXIF orientation and sRGB applied,
// done gets a null image on failure. done must be guarded by the caller,
// it is moved to the main thread callback, so whatever it holds (a show)
// is never released on the background thread.
void DecodeAsync(
		QByteArray bytes,
		QString path,
		QImage fallback,
		Fn<void(QImage)> done) {
	crl::async([=, done = std::move(done)]() mutable {
		auto error = QString();
		auto image = !bytes.isEmpty()
			? Photo::LoadImage(bytes, &error)
			: !path.isEmpty()
			? Photo::LoadImage(path, &error)
			: QImage();
		if (image.isNull() && !fallback.isNull()) {
			image = Photo::PrepareSource(fallback);
		}
		if (image.isNull()) {
			LOG(("Oblivion Photo Error: could not load an image, %1."
				).arg(error.isEmpty() ? u"no data"_q : error));
		}
		crl::on_main([done = std::move(done), image = std::move(image)] {
			done(image);
		});
	});
}

[[nodiscard]] Data::Thread *ItemThread(HistoryItem *item) {
	if (!item) {
		return nullptr;
	} else if (const auto topic = item->topic()) {
		return topic;
	} else if (const auto sublist = item->savedSublist()) {
		return sublist;
	}
	return item->history();
}

[[nodiscard]] Data::Thread *ContextThread(
		not_null<Window::SessionController*> controller,
		FullMsgId context) {
	return context
		? ItemThread(controller->session().data().message(context))
		: nullptr;
}

[[nodiscard]] QString ThreadName(not_null<Data::Thread*> thread) {
	if (const auto topic = thread->asTopic()) {
		return topic->title();
	}
	const auto peer = thread->peer();
	return peer->isSelf() ? tr::lng_saved_messages(tr::now) : peer->name();
}

[[nodiscard]] bool CanSendImages(not_null<Data::Thread*> thread) {
	return Data::CanSendAnyOf(
		thread,
		ChatRestriction::SendPhotos | ChatRestriction::SendFiles);
}

[[nodiscard]] bool CheckCanSendImage(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<Data::Thread*> thread) {
	const auto peer = thread->peer();
	if (!CanSendImages(thread)) {
		if (const auto error = Data::AnyFileRestrictionError(peer)) {
			Data::ShowSendErrorToast(show, peer, error);
		} else {
			show->showToast(
				tr::lng_oblivion_photo_io_send_restricted(tr::now));
		}
		return false;
	}
	const auto error = GetErrorForSending(thread, { .messagesCount = 1 });
	if (error) {
		Data::ShowSendErrorToast(show, peer, error);
		return false;
	}
	return true;
}

void SendBundle(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		std::shared_ptr<Ui::PreparedBundle> bundle,
		Api::SendOptions options) {
	if (!bundle) {
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
			SendBundle(strong, target, bundle, copy);
		}
	};
	const auto checked = payment->check(
		controller,
		thread->peer(),
		options,
		bundle->totalCount,
		withPaymentApproved);
	if (!checked) {
		return;
	}
	const auto type = bundle->way.sendImagesAsPhotos()
		? SendMediaType::Photo
		: SendMediaType::File;
	auto action = Api::SendAction(thread, options);
	action.clearDraft = false;
	action.sendForwardDraft = false;
	if (!action.replyTo.monoforumPeerId) {
		action.replyTo.monoforumPeerId = thread->monoforumPeerId();
	}
	auto &api = thread->session().api();
	for (auto &group : bundle->groups) {
		const auto album = (group.type != Ui::AlbumType::None)
			? std::make_shared<SendingAlbum>()
			: nullptr;
		api.sendFiles(std::move(group.list), type, album, action);
	}
	controller->uiShow()->showToast(tr::lng_oblivion_photo_io_sent(
		tr::now,
		lt_chat,
		ThreadName(thread)));
}

// The usual send files box, so a caption, "Compress the image" (photo or
// file), the spoiler and the price work as for any attached image.
void ShowSendFilesBox(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		QImage image,
		QString name) {
	if (image.isNull()) {
		return;
	}
	auto list = Storage::PrepareMediaFromImage(
		std::move(image),
		QByteArray(),
		st::sendMediaPreviewSize);
	if (list.files.empty()) {
		return;
	}
	if (!name.isEmpty()) {
		list.files.front().displayName = name + u".png"_q;
	}
	const auto peer = thread->peer();
	const auto show = controller->uiShow();
	const auto weak = base::make_weak(controller);
	const auto weakThread = base::make_weak(thread);
	show->show(Box<SendFilesBox>(SendFilesBoxDescriptor{
		.show = show,
		.list = std::move(list),
		.caption = TextWithTags(),
		.toPeer = peer,
		.limits = DefaultLimitsForPeer(peer),
		.check = DefaultCheckForPeer(show, peer),
		.sendType = Api::SendType::Normal,
		.confirmed = [=](
				std::shared_ptr<Ui::PreparedBundle> bundle,
				Api::SendOptions options,
				FullReplyTo) {
			const auto strong = weak.get();
			const auto target = weakThread.get();
			if (strong && target) {
				SendBundle(strong, target, std::move(bundle), options);
			}
		},
		.replyTo = FullReplyTo{
			.topicRootId = thread->topicRootId(),
			.monoforumPeerId = thread->monoforumPeerId(),
		},
	}));
}

void ChooseRecipientAndSend(
		not_null<Window::SessionController*> controller,
		QImage image,
		QString name) {
	const auto weak = base::make_weak(controller);
	const auto show = controller->uiShow();
	Window::ShowChooseRecipientBox(
		controller,
		[=](not_null<Data::Thread*> thread) {
			if (!CheckCanSendImage(show, thread)) {
				return false;
			}
			// After the chooser box is closed.
			const auto weakThread = base::make_weak(thread);
			crl::on_main([=] {
				const auto strong = weak.get();
				const auto target = weakThread.get();
				if (strong && target) {
					ShowSendFilesBox(strong, target, image, name);
				}
			});
			return true;
		},
		tr::lng_oblivion_photo_io_send_title());
}

void SendToThread(
		base::weak_ptr<Window::SessionController> weak,
		base::weak_ptr<Data::Thread> weakThread,
		QImage image,
		QString name) {
	const auto strong = weak.get();
	const auto thread = weakThread.get();
	if (strong
		&& thread
		&& (&thread->session() == &strong->session())
		&& CheckCanSendImage(strong->uiShow(), thread)) {
		ShowSendFilesBox(strong, thread, std::move(image), name);
	}
}

// Downloading a photo or an image file from a chat.

struct DownloadArgs {
	std::shared_ptr<Ui::Show> show;
	Fn<float64()> progress;
	Fn<bool()> ready;
	Fn<bool()> failed;
	rpl::producer<> updates;
	Fn<void(Fn<void(QImage)> done)> decode;
	Fn<void(QImage)> opened;
	QString title; // Empty: "Photo editor".
};

void DownloadBox(not_null<Ui::GenericBox*> box, DownloadArgs &&args) {
	struct State {
		rpl::variable<QString> status;
		float64 progress = 0.;
		bool finished = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto show = args.show;
	const auto progress = args.progress;
	const auto ready = args.ready;
	const auto failed = args.failed;
	const auto decode = args.decode;
	const auto opened = args.opened;

	box->setTitle(args.title.isEmpty()
		? tr::lng_oblivion_photo_io_title()
		: rpl::single(args.title));
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->status.value(),
			st::boxLabel),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));

	// A thin progress line under the status, in the colors of the default
	// sliders, so the box doesn't look like a bare text message.
	const auto bar = box->addRow(
		object_ptr<Ui::FixedHeightWidget>(box, style::ConvertScale(4)),
		st::boxRowPadding + QMargins(
			0,
			style::ConvertScale(14),
			0,
			style::ConvertScale(6)));
	bar->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(bar);
		auto hq = PainterHighQualityEnabler(p);
		const auto height = bar->height();
		const auto radius = height / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::sliderBgInactive);
		p.drawRoundedRect(bar->rect(), radius, radius);
		const auto filled = int(std::round(
			bar->width() * std::clamp(state->progress, 0., 1.)));
		if (filled > 0) {
			p.setBrush(st::sliderBgActive);
			p.drawRoundedRect(
				QRect(0, 0, std::max(filled, height), height),
				radius,
				radius);
		}
	}, bar->lifetime());
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });

	const auto setProgress = [=](float64 value) {
		if (state->progress != value) {
			state->progress = value;
			bar->update();
		}
	};
	const auto updateStatus = [=] {
		const auto value = progress ? progress() : 0.;
		setProgress(value);
		state->status = tr::lng_oblivion_photo_io_downloading(
			tr::now,
			lt_percent,
			FormatPercent(value));
	};
	const auto check = [=] {
		if (state->finished) {
			return;
		} else if (ready && ready()) {
			state->finished = true;
			setProgress(1.);
			state->status = tr::lng_oblivion_photo_io_loading(tr::now);
			decode(crl::guard(box, [=](QImage image) {
				box->closeBox();
				if (image.isNull()) {
					show->showToast(
						tr::lng_oblivion_photo_io_open_failed(tr::now));
				} else if (opened) {
					opened(std::move(image));
				}
			}));
		} else if (failed && failed()) {
			state->finished = true;
			show->showToast(
				tr::lng_oblivion_photo_io_download_failed(tr::now));
			box->closeBox();
		} else {
			updateStatus();
		}
	};
	std::move(args.updates) | rpl::on_next(check, box->lifetime());
	updateStatus();
}

// Send files box: the untouched originals of the edited attachments.

class KeptOriginals final : public QObject {
public:
	struct Entry {
		qint64 key = 0;
		QImage source;
		Photo::EditState state;
		std::shared_ptr<const Photo::Document> document;
	};

	explicit KeptOriginals(QObject *parent) : QObject(parent) {
		setObjectName(QString::fromLatin1(kKeptOriginalsName));
	}

	[[nodiscard]] const Entry *find(qint64 key) const {
		const auto i = ranges::find(_entries, key, &Entry::key);
		return (i != end(_entries)) ? &*i : nullptr;
	}

	void remember(qint64 was, Entry entry) {
		_entries.erase(
			ranges::remove(_entries, was, &Entry::key),
			end(_entries));
		_entries.push_back(std::move(entry));
		if (int(_entries.size()) > kMaxKeptOriginals) {
			_entries.erase(begin(_entries));
		}
	}

private:
	std::vector<Entry> _entries;

};

[[nodiscard]] not_null<KeptOriginals*> Originals(not_null<QWidget*> box) {
	const auto name = QString::fromLatin1(kKeptOriginalsName);
	const auto children = box->findChildren<QObject*>(
		name,
		Qt::FindDirectChildrenOnly);
	for (const auto child : children) {
		if (const auto result = dynamic_cast<KeptOriginals*>(child)) {
			return result;
		}
	}
	return new KeptOriginals(box);
}

// Replaces the attachment with the edited image, the way the built-in
// editor does it for a sent image: no path, no content, only the image.
void ApplyEditedImage(
		Ui::PreparedFile &file,
		QImage image,
		const QString &fileName) {
	using ImageInfo = Ui::PreparedFileInformation::Image;

	if (!file.information) {
		file.information = std::make_unique<Ui::PreparedFileInformation>();
	}
	file.path = QString();
	file.content = QByteArray();
	file.size = 0;
	file.information->filemime = u"image/png"_q;
	file.information->media = ImageInfo{ .data = std::move(image) };
	Storage::UpdateImageDetails(
		file,
		st::sendMediaPreviewSize,
		PhotoSideLimit(true));
	const auto size = file.preview.size();
	file.type = Ui::ValidateThumbDimensions(size.width(), size.height())
		? Ui::PreparedFile::Type::Photo
		: Ui::PreparedFile::Type::File;
	if (!fileName.isEmpty()) {
		// Sent as a file it becomes a PNG.
		const auto base = QFileInfo(fileName).completeBaseName();
		file.displayName = (base.isEmpty() ? fileName : base) + u".png"_q;
	}
}

// The drop zone of the import box.

class DropZone final : public Ui::AbstractButton {
public:
	explicit DropZone(QWidget *parent);

	void setHighlighted(bool highlighted);
	void setLoading(bool loading);

private:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

	bool _highlighted = false;
	bool _loading = false;

};

DropZone::DropZone(QWidget *parent) : AbstractButton(parent) {
}

void DropZone::setHighlighted(bool highlighted) {
	if (_highlighted != highlighted) {
		_highlighted = highlighted;
		update();
	}
}

void DropZone::setLoading(bool loading) {
	if (_loading != loading) {
		_loading = loading;
		setPointerCursor(!loading);
		update();
	}
}

void DropZone::onStateChanged(State was, StateChangeSource source) {
	update();
}

void DropZone::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto active = _highlighted || (isOver() && !_loading);
	const auto line = style::ConvertScaleExact(1.5);
	const auto outer = QRectF(QWidget::rect()).marginsRemoved(
		QMarginsF(line, line, line, line));
	const auto radius = st::roundRadiusLarge * 2.;
	if (active) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(outer, radius, radius);
	}
	auto pen = QPen(active ? st::windowActiveTextFg->c : st::windowSubTextFg->c);
	pen.setWidthF(line);
	pen.setDashPattern({ 4., 3. });
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(outer, radius, radius);

	const auto &titleFont = st::semiboldFont;
	const auto &textFont = st::normalFont;
	const auto circle = style::ConvertScale(44);
	const auto skip = style::ConvertScale(12);
	const auto lineSkip = style::ConvertScale(4);
	const auto text = _loading
		? QString()
		: tr::lng_oblivion_photo_io_drop_about(
			tr::now,
			lt_shortcut,
			QKeySequence(QKeySequence::Paste).toString(
				QKeySequence::NativeText));
	const auto maxWidth = width() - 2 * style::ConvertScale(16);
	// The hint wraps instead of being elided, into balanced lines: the
	// narrowest width that keeps the same number of lines, so no single
	// orphan word is left on the last one.
	const auto textFlags = Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap;
	const auto measure = [&](int lineWidth) {
		return int(std::ceil(textFont->metrics().boundingRect(
			QRectF(0., 0., std::max(lineWidth, 1), float64(1 << 20)),
			Qt::TextWordWrap,
			text).height()));
	};
	auto available = maxWidth;
	auto textHeight = 0;
	if (!text.isEmpty()) {
		textHeight = measure(maxWidth);
		if (textFont->width(text) > maxWidth) {
			auto small = maxWidth / 2;
			auto large = maxWidth;
			while (large - small > 1) {
				const auto middle = (small + large) / 2;
				if (measure(middle) <= textHeight) {
					large = middle;
				} else {
					small = middle;
				}
			}
			// A little slack against rounding in the painter's layout.
			available = std::min(large + style::ConvertScale(2), maxWidth);
		}
	}
	const auto total = circle
		+ skip
		+ titleFont->height
		+ (text.isEmpty() ? 0 : (lineSkip + textHeight));
	auto top = (height() - total) / 2;

	const auto circleRect = QRect(
		(width() - circle) / 2,
		top,
		circle,
		circle);
	p.setPen(Qt::NoPen);
	p.setBrush(active ? st::lightButtonBgOver : st::windowBgOver);
	p.drawEllipse(circleRect);
	st::menuIconPhoto.paintInCenter(
		p,
		circleRect,
		st::windowActiveTextFg->c);
	top += circle + skip;

	const auto title = _loading
		? tr::lng_oblivion_photo_io_loading(tr::now)
		: _highlighted
		? tr::lng_oblivion_photo_io_drop_active(tr::now)
		: tr::lng_oblivion_photo_io_drop(tr::now);
	p.setFont(titleFont->f);
	p.setPen(st::windowBoldFg);
	p.drawText(
		QRect(0, top, width(), titleFont->height),
		Qt::AlignHCenter | Qt::AlignTop,
		titleFont->elided(title, maxWidth));
	if (!text.isEmpty()) {
		top += titleFont->height + lineSkip;
		p.setFont(textFont->f);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect((width() - available) / 2, top, available, textHeight),
			textFlags,
			text);
	}
}

// Import: open a file, paste or drop an image.

struct ImportArgs {
	std::shared_ptr<Ui::Show> show;
	Fn<void(QImage image, QString name)> opened;
};

void ImportBox(not_null<Ui::GenericBox*> box, ImportArgs &&args) {
	struct State {
		bool loading = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto show = args.show;
	const auto opened = args.opened;

	box->setTitle(tr::lng_oblivion_photo_io_title());
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	const auto zone = container->add(
		object_ptr<DropZone>(container),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	zone->resize(zone->width(), style::ConvertScale(kDropZoneHeight));

	const auto finish = [=](QImage image, QString name) {
		state->loading = false;
		zone->setLoading(false);
		if (image.isNull()) {
			show->showToast(tr::lng_oblivion_photo_io_open_failed(tr::now));
			return;
		}
		box->closeBox();
		if (opened) {
			opened(std::move(image), name);
		}
	};
	const auto startLoading = [=] {
		if (state->loading) {
			return false;
		}
		state->loading = true;
		zone->setLoading(true);
		return true;
	};
	const auto loadPath = [=](const QString &path) {
		if (path.isEmpty() || !startLoading()) {
			return;
		}
		const auto name = EditedName(QFileInfo(path).fileName());
		DecodeAsync(QByteArray(), path, QImage(), crl::guard(box, [=](
				QImage image) {
			finish(std::move(image), name.isEmpty() ? NowName() : name);
		}));
	};
	const auto loadBytes = [=](const QByteArray &bytes) {
		if (bytes.isEmpty() || !startLoading()) {
			return;
		}
		DecodeAsync(bytes, QString(), QImage(), crl::guard(box, [=](
				QImage image) {
			finish(std::move(image), NowName());
		}));
	};
	const auto loadImage = [=](const QImage &image) {
		if (image.isNull() || !startLoading()) {
			return;
		}
		DecodeAsync(QByteArray(), QString(), image, crl::guard(box, [=](
				QImage image) {
			finish(std::move(image), NowName());
		}));
	};
	const auto loadMime = [=](const QMimeData *data) {
		auto read = ReadMimeImage(data);
		if (!read.path.isEmpty()) {
			loadPath(read.path);
			return true;
		} else if (!read.image.isNull()) {
			loadImage(read.image);
			return true;
		}
		return false;
	};
	const auto chooseFile = [=] {
		if (state->loading) {
			return;
		}
		FileDialog::GetOpenPath(
			box.get(),
			tr::lng_oblivion_photo_io_choose(tr::now),
			FileDialog::ImagesOrAllFilter(),
			crl::guard(box, [=](FileDialog::OpenResult &&result) {
				if (!result.paths.isEmpty()) {
					loadPath(result.paths.front());
				} else if (!result.remoteContent.isEmpty()) {
					loadBytes(result.remoteContent);
				}
			}));
	};
	const auto paste = [=] {
		if (state->loading) {
			return;
		} else if (!loadMime(QGuiApplication::clipboard()->mimeData())) {
			show->showToast(tr::lng_oblivion_photo_io_paste_empty(tr::now));
		}
	};
	zone->setClickedCallback(chooseFile);

	Ui::AddSkip(container);
	::Settings::AddButtonWithIcon(
		container,
		tr::lng_oblivion_photo_io_open_file(),
		st::settingsButton,
		{ &st::menuIconPhoto }
	)->setClickedCallback(chooseFile);
	::Settings::AddButtonWithIcon(
		container,
		tr::lng_oblivion_photo_io_paste(),
		st::settingsButton,
		{ &st::menuIconCopy }
	)->setClickedCallback(paste);
	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::lng_oblivion_photo_io_import_about());

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->setFocusCallback([=] { box->setFocus(); });

	box->setAcceptDrops(true);
	base::install_event_filter(box, [=](not_null<QEvent*> e) {
		using Result = base::EventFilterResult;
		const auto type = e->type();
		if (type == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(e.get());
			if (key->matches(QKeySequence::Paste)) {
				paste();
				return Result::Cancel;
			} else if (key->matches(QKeySequence::Open)) {
				chooseFile();
				return Result::Cancel;
			}
		} else if (type == QEvent::DragEnter || type == QEvent::DragMove) {
			const auto drag = static_cast<QDragMoveEvent*>(e.get());
			if (state->loading || !MimeHasImage(drag->mimeData())) {
				zone->setHighlighted(false);
				drag->ignore();
			} else {
				zone->setHighlighted(true);
				drag->setDropAction(Qt::CopyAction);
				drag->accept();
			}
			return Result::Cancel;
		} else if (type == QEvent::DragLeave) {
			zone->setHighlighted(false);
			return Result::Cancel;
		} else if (type == QEvent::Drop) {
			const auto drop = static_cast<QDropEvent*>(e.get());
			zone->setHighlighted(false);
			if (!state->loading && loadMime(drop->mimeData())) {
				drop->setDropAction(Qt::CopyAction);
				drop->accept();
			} else {
				drop->ignore();
			}
			return Result::Cancel;
		}
		return Result::Continue;
	});
}

// The result: save as, copy, send, continue editing.

struct ExportArgs {
	std::shared_ptr<Ui::Show> show;
	QImage image;
	QString name;

	// Opens the editor again with the same original and edit state,
	// done gets the new result.
	Fn<void(Fn<void(QImage)> done)> edit;

	// "Send to chat..." (choose a chat) and "Send to <chat>".
	Fn<void(QImage)> send;
	QString hereName;
	Fn<void(QImage)> sendHere;

	// "Make a sticker" (oblivion_sticker_packs.h).
	Fn<void(QImage)> sticker;
};

void PaintCheckerboard(QPainter &p, QRect rect) {
	const auto cell = style::ConvertScale(8);
	p.fillRect(rect, st::windowBg);
	for (auto y = rect.y(); y < rect.y() + rect.height(); y += cell) {
		const auto row = (y - rect.y()) / cell;
		for (auto x = rect.x(); x < rect.x() + rect.width(); x += cell) {
			const auto column = (x - rect.x()) / cell;
			if ((row + column) % 2) {
				p.fillRect(
					QRect(x, y, cell, cell).intersected(rect),
					st::windowBgRipple);
			}
		}
	}
}

// Title on the left, value on the right, a slider below. Returns a setter
// that moves the slider and refreshes the value label, without changed().
[[nodiscard]] Fn<void(int)> AddQualitySlider(
		not_null<Ui::VerticalLayout*> container,
		int current,
		Fn<QString(int)> format,
		Fn<void(int)> changed) {
	const auto header = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	const auto name = Ui::CreateChild<Ui::FlatLabel>(
		header,
		tr::lng_oblivion_photo_io_quality(),
		st::defaultFlatLabel);
	const auto value = Ui::CreateChild<Ui::FlatLabel>(
		header,
		format(current),
		st::settingsScaleLabel);
	rpl::combine(
		header->widthValue(),
		name->sizeValue(),
		value->sizeValue()
	) | rpl::on_next([=](int width, QSize nameSize, QSize valueSize) {
		const auto height = std::max(nameSize.height(), valueSize.height());
		if (header->height() != height) {
			header->resize(width, height);
		}
		name->moveToLeft(0, (height - nameSize.height()) / 2, width);
		value->moveToRight(0, (height - valueSize.height()) / 2, width);
	}, header->lifetime());

	const auto slider = container->add(
		object_ptr<Ui::MediaSliderWheelless>(container, st::settingsScale),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip / 2, 0, 0));
	slider->resize(slider->width(), st::settingsScale.seekSize.height());
	// The knob would otherwise almost touch the divider below.
	Ui::AddSkip(container);

	const auto sections = kMaxQuality - kMinQuality;
	const auto toValue = [=](float64 position) {
		return kMinQuality
			+ int(std::round(std::clamp(position, 0., 1.) * sections));
	};
	const auto toPosition = [=](int now) {
		return std::clamp((now - kMinQuality) / float64(sections), 0., 1.);
	};
	const auto last = slider->lifetime().make_state<int>(current);
	slider->setAlwaysDisplayMarker(true);
	slider->setValue(toPosition(current));
	slider->setAdjustCallback([=](float64 position) {
		return toPosition(toValue(position));
	});
	const auto update = [=](float64 position) {
		const auto now = toValue(position);
		value->setText(format(now));
		if (*last != now) {
			*last = now;
			changed(now);
		}
	};
	slider->setChangeProgressCallback(update);
	slider->setChangeFinishedCallback(update);
	return [=](int now) {
		*last = now;
		slider->setValue(toPosition(now));
		value->setText(format(now));
	};
}

[[nodiscard]] const style::FlatLabel &CenteredSubTextStyle() {
	static const auto result = [] {
		auto copy = st::defaultSubTextLabel;
		copy.align = style::al_top;
		return copy;
	}();
	return result;
}

void ExportBox(not_null<Ui::GenericBox*> box, ExportArgs &&args) {
	struct State {
		QImage image;
		QImage preview;
		rpl::variable<QSize> imageSize;
		bool alpha = false;
		bool formatChosen = false;
		ExportFormat format;
		ExportFormat encodedFormat;
		QByteArray encoded;
		bool encodedReady = false;
		bool encoding = false;
		bool encodeAgain = false;
		uint64 generation = 0;
		uint64 previewGeneration = 0;
		rpl::variable<QString> sizeText;
		rpl::variable<QString> aboutText;
		base::Timer estimateTimer;
		Fn<void()> startEncode;
	};
	const auto state = box->lifetime().make_state<State>();
	// The guard for the background jobs is created here, on the main
	// thread: a guard built from the raw box pointer on a worker would
	// touch the box after it could be destroyed.
	const auto weakBox = base::make_weak(box);
	const auto show = args.show;
	const auto weakShow = std::weak_ptr<Ui::Show>(show);
	const auto name = args.name.isEmpty() ? NowName() : args.name;
	const auto formats = Photo::SupportedSaveFormats();
	const auto supported = [=](SaveFormat format) {
		return ranges::contains(formats, format);
	};
	const auto opaqueDefault = supported(SaveFormat::Jpeg)
		? SaveFormat::Jpeg
		: SaveFormat::Png;

	const auto remembered = RememberedFormat();
	state->format = remembered.value_or(ExportFormat{
		.format = opaqueDefault,
	});
	state->formatChosen = remembered.has_value();
	if (!supported(state->format.format)) {
		state->format.format = formats.empty()
			? SaveFormat::Png
			: formats.front();
	}
	state->image = std::move(args.image);
	state->imageSize = state->image.size();

	box->setTitle(tr::lng_oblivion_photo_io_result_title());
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	const auto ratio = style::DevicePixelRatio();
	const auto maxHeight = style::ConvertScale(kPreviewMaxHeight);
	const auto innerWidth = st::boxWideWidth
		- st::boxRowPadding.left()
		- st::boxRowPadding.right();
	const auto fitted = [=](QSize size, int width) {
		if (size.isEmpty() || width <= 0) {
			return QRect();
		}
		auto result = size;
		if (result.width() > width || result.height() > maxHeight) {
			result = result.scaled(
				QSize(width, maxHeight),
				Qt::KeepAspectRatio);
		}
		result = QSize(
			std::max(result.width(), 1),
			std::max(result.height(), 1));
		return QRect(QPoint((width - result.width()) / 2, 0), result);
	};

	const auto preview = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	rpl::combine(
		preview->widthValue(),
		state->imageSize.value()
	) | rpl::on_next([=](int width, QSize size) {
		const auto height = std::max(fitted(size, width).height(), 1);
		if (preview->height() != height) {
			preview->resize(width, height);
		}
		preview->update();
	}, preview->lifetime());
	preview->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(preview);
		const auto rect = fitted(
			state->imageSize.current(),
			preview->width());
		if (rect.isEmpty()) {
			return;
		}
		auto hq = PainterHighQualityEnabler(p);
		auto path = QPainterPath();
		const auto radius = float64(st::roundRadiusLarge);
		path.addRoundedRect(QRectF(rect), radius, radius);
		p.setClipPath(path);
		if (state->preview.isNull()) {
			p.fillRect(rect, st::windowBgOver);
			return;
		} else if (state->alpha) {
			PaintCheckerboard(p, rect);
			// Half of the cells have the color of the box, the thin
			// outline under the image shows where the picture ends.
			const auto half = st::lineWidth / 2.;
			p.setPen(QPen(st::windowBgRipple->c, st::lineWidth));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(
				QRectF(rect).marginsRemoved(
					QMarginsF(half, half, half, half)),
				radius - half,
				radius - half);
		}
		p.drawImage(rect, state->preview);
	}, preview->lifetime());

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			state->imageSize.value() | rpl::map([](QSize size) {
				return tr::lng_oblivion_photo_io_dimensions(
					tr::now,
					lt_width,
					QString::number(size.width()),
					lt_height,
					QString::number(size.height()));
			}),
			CenteredSubTextStyle()),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0),
		style::al_top); // Under the centered preview, not at the left.

	// Encoding in the background: the file size next to "Save as", and the
	// bytes are reused by the save if nothing changed meanwhile.
	state->startEncode = [=] {
		if (state->encoding) {
			state->encodeAgain = true;
			return;
		} else if (state->encodedReady || state->image.isNull()) {
			return;
		}
		state->encoding = true;
		state->encodeAgain = false;
		const auto generation = state->generation;
		const auto image = state->image;
		const auto format = state->format;
		crl::async([=] {
			const auto bytes = Photo::EncodeImage(
				image,
				format.format,
				format.quality);
			crl::on_main(weakBox, [=] {
				state->encoding = false;
				if (generation == state->generation) {
					state->encodeAgain = false;
					state->encoded = bytes;
					state->encodedFormat = format;
					state->encodedReady = true;
					state->sizeText = bytes.isEmpty()
						? QString()
						: Ui::FormatSizeText(bytes.size());
				} else if (base::take(state->encodeAgain)) {
					state->startEncode();
				}
			});
		});
	};
	state->estimateTimer.setCallback([=] { state->startEncode(); });
	const auto invalidate = [=](crl::time delay) {
		++state->generation;
		state->encodedReady = false;
		state->encoded = QByteArray();
		state->sizeText = QString(QChar(0x2026));
		state->estimateTimer.callOnce(delay);
	};
	const auto refreshAbout = [=] {
		state->aboutText = AboutText(state->format.format, state->alpha);
	};

	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_oblivion_photo_io_format());
	const auto tabs = container->add(
		object_ptr<Ui::SettingsSlider>(container, st::settingsSlider),
		st::boxRowPadding + QMargins(
			0,
			style::ConvertScale(4),
			0,
			style::ConvertScale(6)));
	tabs->setSections(ranges::views::all(
		formats
	) | ranges::views::transform(FormatTitle) | ranges::to_vector);
	const auto indexOf = [=](SaveFormat format) {
		const auto i = ranges::find(formats, format);
		return (i == end(formats)) ? 0 : int(i - begin(formats));
	};
	tabs->setActiveSectionFast(indexOf(state->format.format));

	const auto qualityWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto setQuality = AddQualitySlider(
		qualityWrap->entity(),
		state->format.quality,
		[=](int value) {
			return (value == kMaxQuality
				&& state->format.format == SaveFormat::Webp)
				? tr::lng_oblivion_photo_io_lossless(tr::now)
				: QString::number(value);
		},
		[=](int value) {
			state->format.quality = value;
			state->formatChosen = true;
			invalidate(kEstimateDelay);
		});
	qualityWrap->toggle(
		HasQuality(state->format.format),
		anim::type::instant);
	Ui::AddSkip(container);
	Ui::AddDividerText(container, state->aboutText.value());

	const auto setFormat = [=](SaveFormat format) {
		if (state->format.format == format) {
			return;
		}
		state->format.format = format;
		qualityWrap->toggle(HasQuality(format), anim::type::normal);
		setQuality(state->format.quality);
		refreshAbout();
		invalidate(kEstimateDelay);
	};
	tabs->sectionActivated(
	) | rpl::on_next([=](int index) {
		if (index >= 0 && index < int(formats.size())) {
			state->formatChosen = true;
			setFormat(formats[index]);
		}
	}, tabs->lifetime());

	const auto preparePreview = [=] {
		const auto image = state->image;
		const auto target = fitted(image.size(), innerWidth).size() * ratio;
		const auto generation = ++state->previewGeneration;
		crl::async([=] {
			const auto scaled = Photo::PrepareSource(image, target);
			const auto alpha = HasTransparency(image);
			crl::on_main(weakBox, [=] {
				if (generation != state->previewGeneration) {
					return;
				}
				state->preview = scaled;
				state->alpha = alpha;
				if (alpha
					&& !state->formatChosen
					&& (state->format.format == SaveFormat::Jpeg)
					&& supported(SaveFormat::Png)) {
					// Keep the transparency unless JPEG was chosen.
					tabs->setActiveSectionFast(indexOf(SaveFormat::Png));
					setFormat(SaveFormat::Png);
				}
				refreshAbout();
				preview->update();
			});
		});
	};
	const auto setImage = [=](QImage image) {
		if (image.isNull()) {
			return;
		}
		state->image = std::move(image);
		state->preview = QImage();
		state->alpha = false;
		state->imageSize = state->image.size();
		preparePreview();
		invalidate(0);
	};

	const auto saveTo = [=](QString path) {
		auto chosen = state->format;
		const auto byExtension = FormatFromExtension(QFileInfo(path).suffix());
		if (byExtension && supported(*byExtension)) {
			chosen.format = *byExtension;
		} else {
			path += QChar('.') + Photo::SaveFormatExtension(chosen.format);
		}
		RememberFormat(state->format);
		const auto ready = (state->encodedReady
			&& (state->encodedFormat == chosen))
			? state->encoded
			: QByteArray();
		const auto image = state->image;
		// Not guarded by the box: the file is written even if the box is
		// closed right after the path was chosen.
		crl::async([=] {
			const auto bytes = ready.isEmpty()
				? Photo::EncodeImage(image, chosen.format, chosen.quality)
				: ready;
			const auto written = !bytes.isEmpty() && WriteFile(path, bytes);
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
					: tr::lng_oblivion_photo_io_save_failed(tr::now));
			});
		});
	};
	const auto save = [=] {
		const auto format = state->format.format;
		FileDialog::GetWritePath(
			box.get(),
			tr::lng_oblivion_photo_io_save_title(tr::now),
			FormatFilter(format),
			SuggestedPath(name, format),
			crl::guard(box, [=](QString &&result) {
				if (!result.isEmpty()) {
					saveTo(std::move(result));
				}
			}));
	};
	const auto copy = [=] {
		auto mime = std::make_unique<QMimeData>();
		mime->setImageData(state->image);
		if (state->alpha
			&& state->encodedReady
			&& (state->encodedFormat.format == SaveFormat::Png)) {
			mime->setData(u"image/png"_q, state->encoded);
		}
		QGuiApplication::clipboard()->setMimeData(mime.release());
		show->showToast(tr::lng_oblivion_photo_io_copied(tr::now));
	};

	Ui::AddSkip(container);
	::Settings::AddButtonWithLabel(
		container,
		tr::lng_oblivion_photo_io_save(),
		state->sizeText.value(),
		st::settingsButton,
		{ &st::menuIconDownload }
	)->setClickedCallback(save);
	::Settings::AddButtonWithIcon(
		container,
		tr::lng_oblivion_photo_io_copy(),
		st::settingsButton,
		{ &st::menuIconCopy }
	)->setClickedCallback(copy);
	if (const auto sendHere = args.sendHere) {
		::Settings::AddButtonWithIcon(
			container,
			tr::lng_oblivion_photo_io_send_here(
				lt_chat,
				rpl::single(args.hereName)),
			st::settingsButton,
			{ &st::menuIconSend }
		)->setClickedCallback([=] {
			sendHere(state->image);
		});
	}
	if (const auto send = args.send) {
		::Settings::AddButtonWithIcon(
			container,
			tr::lng_oblivion_photo_io_send(),
			st::settingsButton,
			{ &st::menuIconForward }
		)->setClickedCallback([=] {
			send(state->image);
		});
	}
	if (const auto sticker = args.sticker) {
		::Settings::AddButtonWithIcon(
			container,
			tr::lng_oblivion_packs_context_sticker(),
			st::settingsButton,
			{ &st::menuIconStickerAdd }
		)->setClickedCallback([=] {
			sticker(state->image);
		});
	}
	if (const auto edit = args.edit) {
		::Settings::AddButtonWithIcon(
			container,
			tr::lng_oblivion_photo_io_edit_again(),
			st::settingsButton,
			{ &st::menuIconPalette }
		)->setClickedCallback([=] {
			edit(crl::guard(box, [=](QImage image) {
				setImage(std::move(image));
			}));
		});
	}
	Ui::AddSkip(container);

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->setFocusCallback([=] { box->setFocus(); });
	base::install_event_filter(box, [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(e.get());
			if (key->matches(QKeySequence::Save)) {
				save();
				return base::EventFilterResult::Cancel;
			} else if (key->matches(QKeySequence::Copy)) {
				copy();
				return base::EventFilterResult::Cancel;
			}
		}
		return base::EventFilterResult::Continue;
	});

	refreshAbout();
	preparePreview();
	invalidate(0);
}

// Editing sessions.

struct EditSession {
	QImage source;
	Photo::EditState state;
	QString name;

	// The whole layered edit of the last result: with it "continue
	// editing" brings back the layers too, not only the state.
	std::shared_ptr<const Photo::Document> document;
};

[[nodiscard]] std::vector<Photo::PhotoEditorAction> SendActions(
		base::weak_ptr<Window::SessionController> weak,
		base::weak_ptr<Data::Thread> weakThread,
		QString name) {
	auto list = std::vector<Photo::PhotoEditorAction>();
	if (const auto thread = weakThread.get()) {
		if (CanSendImages(thread)) {
			list.push_back({
				.text = tr::lng_oblivion_photo_io_send_here(
					tr::now,
					lt_chat,
					ThreadName(thread)),
				.callback = [=](Photo::PhotoEditorResult result) {
					SendToThread(weak, weakThread, result.image, name);
				},
				.icon = &st::mediaMenuIconShowInChat,
				.closeEditor = false,
			});
		}
	}
	list.push_back({
		.text = tr::lng_oblivion_photo_io_send(tr::now),
		.callback = [=](Photo::PhotoEditorResult result) {
			if (const auto strong = weak.get()) {
				ChooseRecipientAndSend(strong, result.image, name);
			}
		},
		.icon = &st::mediaMenuIconForward,
		.closeEditor = false,
	});
	list.push_back({
		.text = tr::lng_oblivion_packs_context_sticker(tr::now),
		.callback = [=](Photo::PhotoEditorResult result) {
			if (const auto strong = weak.get()) {
				auto source = StickerSource::FromImage(
					std::move(result.image));
				source.name = name;
				AddToStickerPack(strong, std::move(source));
			}
		},
		.icon = &st::mediaMenuIconStickers,
		.closeEditor = false,
	});
	return list;
}

// An edit of an editor that was closed not by the user (the passcode
// lock, a switch to another account, a chat opened from a notification)
// is kept by the editor, see Photo::InterruptedPhotoEdit. Here the user is
// told about it by a toast in the active window (after the passcode is
// entered, if the application is locked), it is offered when the editor
// is opened the next time and dropped when its account is logged out.
struct KeptNotice {
	bool pending = false;
	bool scheduled = false;
	bool waiting = false;
	rpl::lifetime lifetime;
};

[[nodiscard]] KeptNotice &PendingKeptNotice() {
	static auto result = KeptNotice();
	return result;
}

void FlushKeptNotice();

void ScheduleKeptNotice() {
	if (!std::exchange(PendingKeptNotice().scheduled, true)) {
		crl::on_main([] {
			FlushKeptNotice();
		});
	}
}

void FlushKeptNotice() {
	auto &notice = PendingKeptNotice();
	notice.scheduled = false;
	if (!Core::IsAppLaunched() || Core::Quitting()) {
		return;
	} else if (Core::App().passcodeLocked()) {
		if (!std::exchange(notice.waiting, true)) {
			notice.lifetime.destroy();
			Core::App().passcodeLockChanges(
			) | rpl::filter([](bool locked) {
				return !locked;
			}) | rpl::take(1) | rpl::on_next([] {
				// The windows are unlocked right after this.
				PendingKeptNotice().waiting = false;
				ScheduleKeptNotice();
			}, notice.lifetime);
		}
		return;
	}
	const auto pending = base::take(notice.pending);
	const auto window = Core::App().activePrimaryWindow();
	if (pending && window && Photo::InterruptedPhotoEditId()) {
		window->showToast(
			tr::lng_oblivion_photo_panel_kept(tr::now),
			kKeptNoticeDuration);
	}
}

// PhotoEditorOptions::interrupted for an editor of this account. It is
// called from the main queue, the account may be logged out by then.
[[nodiscard]] Fn<void(int)> InterruptedHandler(
		not_null<Main::Session*> session) {
	const auto weak = base::make_weak(session);
	return [=](int id) {
		if (!Core::IsAppLaunched()
			|| Core::Quitting()
			|| !id
			|| (Photo::InterruptedPhotoEditId() != id)) {
			return;
		}
		const auto strong = weak.get();
		if (!strong) {
			// Nothing of an account stays after it is logged out.
			Photo::DropInterruptedPhotoEdit(id);
			return;
		}
		strong->lifetime().add([=] {
			Photo::DropInterruptedPhotoEdit(id);
		});
		PendingKeptNotice().pending = true;
		ScheduleKeptNotice();
	};
}

void ShowExportBox(
	not_null<Window::SessionController*> controller,
	std::shared_ptr<EditSession> session,
	base::weak_ptr<Data::Thread> weakThread,
	QImage image);
void ShowInterruptedEditor(not_null<Window::SessionController*> controller);

// The edit kept from an interrupted editor is offered first: fresh opens
// what was asked for when there is none or the user starts a new one (the
// kept one is dropped then). fresh must check what it uses, it may be
// called long after.
void OfferInterruptedOr(
		not_null<Window::SessionController*> controller,
		Fn<void()> fresh) {
	if (!Photo::InterruptedPhotoEditId()) {
		fresh();
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto box = controller->show(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_photo_panel_restore_text(),
		.confirmed = [=](Fn<void()> close) {
			const auto strong = weak.get();
			close();
			if (strong) {
				ShowInterruptedEditor(strong);
			}
		},
		.cancelled = [=](Fn<void()> close) {
			const auto open = fresh;
			close();
			Photo::DropInterruptedPhotoEdit();
			open();
		},
		.confirmText = tr::lng_oblivion_photo_panel_restore_continue(),
		.cancelText = tr::lng_oblivion_photo_panel_restore_new(),
		.strictCancel = true,
	}));
	if (const auto raw = box.get()) {
		// A click outside closes all the boxes: the send files box under
		// this question as well, with what is attached. Escape closes
		// only the question, the kept edit stays.
		raw->setCloseByOutsideClick(false);
	}
}

// The editor options of a session: "Done" keeps the edit in the session
// (so "continue editing" restores it with its layers) and, the first time
// (updated is null), shows the result box, later it updates it.
[[nodiscard]] Photo::PhotoEditorOptions EditorOptionsFor(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<EditSession> session,
		base::weak_ptr<Data::Thread> weakThread,
		Fn<void(QImage)> updated) {
	const auto weak = base::make_weak(controller);
	const auto weakShow = std::weak_ptr<Ui::Show>(controller->uiShow());
	return {
		.fileName = session->name,
		.state = session->state,
		.done = [=](Photo::PhotoEditorResult result) {
			if (result.image.isNull()) {
				if (const auto strong = weakShow.lock()) {
					strong->showToast(
						tr::lng_oblivion_photo_io_process_failed(tr::now));
				}
				return;
			}
			session->state = result.state;
			session->document = result.document;
			if (!result.source.isNull()) {
				session->source = std::move(result.source);
			}
			if (updated) {
				updated(std::move(result.image));
			} else if (const auto strong = weak.get()) {
				ShowExportBox(
					strong,
					session,
					weakThread,
					std::move(result.image));
			}
		},
		.actions = SendActions(weak, weakThread, session->name),
		.document = session->document,
		.interrupted = InterruptedHandler(&controller->session()),
	};
}

// Shows the editor with the session original and state (or the whole
// layered document of its last result).
void ShowEditorFor(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<EditSession> session,
		base::weak_ptr<Data::Thread> weakThread,
		Fn<void(QImage)> updated) {
	const auto source = session->source;
	Photo::ShowPhotoEditor(
		controller->uiShow(),
		source,
		EditorOptionsFor(
			controller,
			std::move(session),
			std::move(weakThread),
			std::move(updated)));
}

// The kept edit has no original anymore: like a collage it is a document
// from the start, "continue editing" restores it from the last result.
void ShowInterruptedEditor(not_null<Window::SessionController*> controller) {
	if (Core::App().passcodeLocked()) {
		// Photo::ShowPhotoEditor() shows nothing above the passcode
		// screen: the kept edit is not taken for an editor that will not
		// be there.
		return;
	}
	auto kept = Photo::TakeInterruptedPhotoEdit();
	if (!kept.document || kept.document->empty()) {
		return;
	}
	auto session = std::make_shared<EditSession>(EditSession{
		.name = kept.fileName.isEmpty() ? NowName() : kept.fileName,
		.document = std::move(kept.document),
	});
	auto options = EditorOptionsFor(
		controller,
		std::move(session),
		nullptr,
		nullptr);
	options.unsaved = true;
	Photo::ShowPhotoEditor(
		controller->uiShow(),
		QImage(),
		std::move(options));
}

// The result box is closed with the layers of the window just as the
// editor is. When it is not the user who closes it, the edit it shows
// the result of is kept the same way. Only the document is remembered
// here: this runs while the window is being torn down.
void KeepResultWhenInterrupted(
		not_null<Window::SessionController*> controller,
		not_null<Ui::BoxContent*> box,
		std::shared_ptr<EditSession> session) {
	const auto weak = base::make_weak(controller);
	const auto handler = InterruptedHandler(&controller->session());
	box->boxClosing() | rpl::on_next([=] {
		if (!Core::IsAppLaunched() || Core::Quitting()) {
			return;
		}
		// The user closes the box only in an unlocked window that still
		// shows this controller. A window that switches to another
		// account (or whose account is logged out) replaces its session
		// controller first and closes the layers after that, the old one
		// is still alive here, see Window::Controller::showAccount.
		const auto strong = weak.get();
		const auto byUser = strong
			&& !Core::App().passcodeLocked()
			&& (strong->window().sessionController() == strong);
		// An editor opened from this box ("continue editing") is closed
		// with it and has kept what is newer.
		if (byUser || Photo::InterruptedPhotoEditId()) {
			return;
		}
		const auto id = Photo::KeepInterruptedPhotoEdit({
			.document = session->document,
			.fileName = session->name,
		});
		if (id) {
			crl::on_main([=] {
				handler(id);
			});
		}
	}, box->lifetime());
}

void ShowExportBox(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<EditSession> session,
		base::weak_ptr<Data::Thread> weakThread,
		QImage image) {
	const auto weak = base::make_weak(controller);
	const auto name = session->name;
	auto args = ExportArgs{
		.show = controller->uiShow(),
		.image = std::move(image),
		.name = name,
		.edit = [=](Fn<void(QImage)> done) {
			if (const auto strong = weak.get()) {
				ShowEditorFor(strong, session, weakThread, done);
			}
		},
		.send = [=](QImage image) {
			if (const auto strong = weak.get()) {
				ChooseRecipientAndSend(strong, std::move(image), name);
			}
		},
	};
	if (const auto thread = weakThread.get()) {
		if (CanSendImages(thread)) {
			args.hereName = ThreadName(thread);
			args.sendHere = [=](QImage image) {
				SendToThread(weak, weakThread, std::move(image), name);
			};
		}
	}
	args.sticker = [=](QImage image) {
		if (const auto strong = weak.get()) {
			auto source = StickerSource::FromImage(std::move(image));
			source.name = name;
			AddToStickerPack(strong, std::move(source));
		}
	};
	const auto box = controller->show(Box(ExportBox, std::move(args)));
	if (const auto raw = box.get()) {
		KeepResultWhenInterrupted(controller, raw, session);
	}
}

// Snapshot scenes (OBLIVION_SELFTEST=ui), see oblivion_ui_snapshots.h.

[[nodiscard]] QImage SampleImage(bool transparent) {
	const auto size = QSize(1200, 800);
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	if (transparent) {
		p.setPen(Qt::NoPen);
		auto gradient = QLinearGradient(0, 0, size.width(), size.height());
		gradient.setColorAt(0., QColor(0x6c, 0x5c, 0xe7));
		gradient.setColorAt(1., QColor(0xfd, 0x79, 0xa8));
		p.setBrush(gradient);
		p.drawEllipse(QRect(250, 50, 700, 700));
	} else {
		auto sky = QLinearGradient(0, 0, 0, size.height());
		sky.setColorAt(0., QColor(0x2b, 0x5f, 0xa8));
		sky.setColorAt(0.65, QColor(0xf2, 0xa1, 0x6b));
		sky.setColorAt(1., QColor(0xf7, 0xd0, 0x8a));
		p.fillRect(result.rect(), sky);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xff, 0xe3, 0x94));
		p.drawEllipse(QPoint(820, 470), 110, 110);
		auto hills = QPainterPath();
		hills.moveTo(0, 560);
		hills.cubicTo(260, 420, 460, 640, 760, 520);
		hills.cubicTo(960, 440, 1100, 560, 1200, 520);
		hills.lineTo(1200, 800);
		hills.lineTo(0, 800);
		p.setBrush(QColor(0x3d, 0x5a, 0x6c));
		p.drawPath(hills);
		auto ground = QPainterPath();
		ground.moveTo(0, 680);
		ground.cubicTo(300, 600, 700, 760, 1200, 640);
		ground.lineTo(1200, 800);
		ground.lineTo(0, 800);
		p.setBrush(QColor(0x1f, 0x33, 0x3d));
		p.drawPath(ground);
	}
	p.end();
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	const auto width = style::ConvertScale(kSceneWidth);
	RegisterBoxScene(u"photo_io_import"_q, QSize(width, 0), [](
			std::shared_ptr<Ui::Show> show) {
		return Box(ImportBox, ImportArgs{ .show = show });
	});
	RegisterBoxScene(u"photo_io_export"_q, QSize(width, 0), [](
			std::shared_ptr<Ui::Show> show) {
		return Box(ExportBox, ExportArgs{
			.show = show,
			.image = SampleImage(false),
			.name = u"sample"_q,
			.edit = [](Fn<void(QImage)>) {},
			.send = [](QImage) {},
			.hereName = u"Oblivion"_q,
			.sendHere = [](QImage) {},
			.sticker = [](QImage) {},
		});
	});
	RegisterBoxScene(u"photo_io_export_alpha"_q, QSize(width, 0), [](
			std::shared_ptr<Ui::Show> show) {
		return Box(ExportBox, ExportArgs{
			.show = show,
			.image = SampleImage(true),
			.name = u"sample"_q,
		});
	});
	RegisterBoxScene(u"photo_io_download"_q, QSize(width, 0), [](
			std::shared_ptr<Ui::Show> show) {
		return Box(DownloadBox, DownloadArgs{
			.show = show,
			.progress = [] { return 0.42; },
			.ready = [] { return false; },
			.failed = [] { return false; },
			.updates = rpl::never<>(),
			.decode = [](Fn<void(QImage)>) {},
		});
	});
});

} // namespace

void ShowPhotoEditorImport(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	OfferInterruptedOr(controller, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		strong->show(Box(ImportBox, ImportArgs{
			.show = strong->uiShow(),
			.opened = [=](QImage image, QString name) {
				if (const auto strong = weak.get()) {
					ShowPhotoEditorWithImage(
						strong,
						std::move(image),
						name);
				}
			},
		}));
	});
}

void ShowPhotoCollage(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	OfferInterruptedOr(controller, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		// The session has no original: the collage is a document from the
		// start, "continue editing" restores it from the last result.
		auto session = std::make_shared<EditSession>(EditSession{
			.name = TimeName(u"collage"_q, QDateTime::currentDateTime()),
		});
		Photo::ChoosePhotosForCollage(
			strong->uiShow(),
			EditorOptionsFor(strong, std::move(session), nullptr, nullptr));
	});
}

void ShowPhotoEditorWithImage(
		not_null<Window::SessionController*> controller,
		QImage image,
		QString name,
		Data::Thread *thread) {
	if (image.isNull()) {
		controller->uiShow()->showToast(
			tr::lng_oblivion_photo_io_open_failed(tr::now));
		return;
	}
	const auto session = std::make_shared<EditSession>(EditSession{
		.source = std::move(image),
		.name = name.isEmpty() ? NowName() : name,
	});
	const auto weak = base::make_weak(controller);
	const auto weakThread = base::weak_ptr<Data::Thread>(thread);
	OfferInterruptedOr(controller, [=] {
		if (const auto strong = weak.get()) {
			ShowEditorFor(strong, session, weakThread, nullptr);
		}
	});
}

void ShowPhotoEditorForPhoto(
		not_null<Window::SessionController*> controller,
		not_null<PhotoData*> photo,
		FullMsgId context) {
	const auto weak = base::make_weak(controller);
	const auto weakThread = base::make_weak(ContextThread(controller, context));
	LoadPhotoImage(controller, photo, context, [=](
			QImage image,
			QString name) {
		if (const auto strong = weak.get()) {
			ShowPhotoEditorWithImage(
				strong,
				std::move(image),
				name,
				weakThread.get());
		}
	});
}

void LoadPhotoImage(
		not_null<Window::SessionController*> controller,
		not_null<PhotoData*> photo,
		FullMsgId context,
		Fn<void(QImage image, QString name)> callback,
		QString title) {
	if (photo->isNull()) {
		return;
	}
	const auto media = photo->createMediaView();
	const auto name = PhotoName(photo);
	const auto opened = [=](QImage image) {
		callback(std::move(image), name);
	};
	const auto decode = [=](Fn<void(QImage)> done) {
		constexpr auto kLarge = Data::PhotoSize::Large;
		const auto large = media->image(kLarge);
		DecodeAsync(
			media->imageBytes(kLarge),
			QString(),
			large ? large->original() : QImage(),
			std::move(done));
	};
	const auto show = controller->uiShow();
	const auto openLoaded = [=] {
		decode([=](QImage image) {
			if (image.isNull()) {
				show->showToast(
					tr::lng_oblivion_photo_io_open_failed(tr::now));
			} else {
				opened(std::move(image));
			}
		});
	};
	if (media->loaded()) {
		openLoaded();
		return;
	}
	media->wanted(
		Data::PhotoSize::Large,
		context ? Data::FileOrigin(context) : Data::FileOrigin());
	if (media->loaded()) {
		openLoaded();
		return;
	} else if (!photo->loading()) {
		show->showToast(tr::lng_oblivion_photo_io_download_failed(tr::now));
		return;
	}
	const auto session = &photo->session();
	controller->show(Box(DownloadBox, DownloadArgs{
		.show = show,
		.progress = [=] { return photo->progress(); },
		.ready = [=] { return media->loaded(); },
		.failed = [=] {
			return photo->failed(Data::PhotoSize::Large)
				|| !photo->loading();
		},
		.updates = rpl::merge(
			session->data().photoLoadProgress(
			) | rpl::filter([=](not_null<PhotoData*> loaded) {
				return (loaded == photo);
			}) | rpl::to_empty,
			session->downloaderTaskFinished()),
		.decode = decode,
		.opened = opened,
		.title = title,
	}));
}

void ShowPhotoEditorForDocument(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		FullMsgId context) {
	const auto weak = base::make_weak(controller);
	const auto weakThread = base::make_weak(ContextThread(controller, context));
	LoadDocumentImage(controller, document, context, [=](
			QImage image,
			QString name) {
		if (const auto strong = weak.get()) {
			ShowPhotoEditorWithImage(
				strong,
				std::move(image),
				name,
				weakThread.get());
		}
	});
}

void LoadDocumentImage(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		FullMsgId context,
		Fn<void(QImage image, QString name)> callback,
		QString title) {
	const auto media = document->createMediaView();
	const auto name = DocumentName(document);
	const auto opened = [=](QImage image) {
		callback(std::move(image), name);
	};
	const auto decode = [=](Fn<void(QImage)> done) {
		auto bytes = media->bytes();
		auto path = bytes.isEmpty() ? document->filepath(true) : QString();
		DecodeAsync(
			std::move(bytes),
			std::move(path),
			QImage(),
			std::move(done));
	};
	const auto show = controller->uiShow();
	const auto openLoaded = [=] {
		decode([=](QImage image) {
			if (image.isNull()) {
				show->showToast(
					tr::lng_oblivion_photo_io_open_failed(tr::now));
			} else {
				opened(std::move(image));
			}
		});
	};
	if (media->loaded(true)) {
		openLoaded();
		return;
	} else if (!document->loading()) {
		document->save(
			context ? Data::FileOrigin(context) : Data::FileOrigin(),
			QString());
		if (media->loaded(true)) {
			openLoaded();
			return;
		} else if (!document->loading()) {
			show->showToast(
				tr::lng_oblivion_photo_io_download_failed(tr::now));
			return;
		}
	}
	controller->show(Box(DownloadBox, DownloadArgs{
		.show = show,
		.progress = [=] { return document->progress(); },
		.ready = [=] { return media->loaded(true); },
		.failed = [=] { return !document->loading(); },
		.updates = document->session().data().documentLoadProgress(
		) | rpl::filter([=](not_null<DocumentData*> loaded) {
			return (loaded == document);
		}) | rpl::to_empty,
		.decode = decode,
		.opened = opened,
		.title = title,
	}));
}

bool PhotoEditorAcceptsDocument(not_null<DocumentData*> document) {
	return document->isImage()
		&& !document->sticker()
		&& !document->isAnimation()
		&& !document->isVideoFile()
		&& (document->size <= kMaxDocumentSize);
}

void AddPhotoEditorAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<PhotoData*> photo,
		HistoryItem *item) {
	if (photo->isNull()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto context = item ? item->fullId() : FullMsgId();
	menu->addAction(tr::lng_oblivion_photo_io_open_context(tr::now), [=] {
		if (const auto strong = weak.get()) {
			ShowPhotoEditorForPhoto(strong, photo, context);
		}
	}, &st::menuIconPalette);
}

void AddPhotoEditorAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		HistoryItem *item) {
	if (!PhotoEditorAcceptsDocument(document)) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto context = item ? item->fullId() : FullMsgId();
	menu->addAction(tr::lng_oblivion_photo_io_open_context(tr::now), [=] {
		if (const auto strong = weak.get()) {
			ShowPhotoEditorForDocument(strong, document, context);
		}
	}, &st::menuIconPalette);
}

namespace {

// What opens the editor for an attached static image, null for any other
// file. Everything needed is copied from the file right away: the list of
// the box may change before the result is called.
[[nodiscard]] Fn<void()> AttachPhotoEditorOpener(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<QWidget*> box,
		const Ui::PreparedFile &file,
		Fn<void(Fn<void(Ui::PreparedFile&)> apply)> replace) {
	using ImageInfo = Ui::PreparedFileInformation::Image;
	using Type = Ui::PreparedFile::Type;

	const auto image = file.information
		? std::get_if<ImageInfo>(&file.information->media)
		: nullptr;
	if (!show
		|| !replace
		|| !image
		|| image->animated
		|| image->data.isNull()
		|| (file.type != Type::Photo && file.type != Type::File)) {
		return nullptr;
	}
	const auto key = image->data.cacheKey();
	const auto data = image->data;
	const auto modifications = image->modifications;
	const auto fileName = !file.displayName.isEmpty()
		? file.displayName
		: !file.path.isEmpty()
		? QFileInfo(file.path).fileName()
		: QString();
	const auto originals = QPointer<KeptOriginals>(Originals(box).get());
	const auto open = [=] {
		if (!show->valid()) {
			return;
		}
		auto session = EditSession{
			.name = fileName.isEmpty() ? NowName() : EditedName(fileName),
		};
		const auto kept = (originals && !modifications)
			? originals->find(key)
			: nullptr;
		if (kept) {
			// Edited here before: continue from the untouched original.
			session.source = kept->source;
			session.state = kept->state;
			session.document = kept->document;
		} else {
			// The built-in editor changes are applied first (on the main
			// thread, they may contain painted stickers).
			session.source = Photo::PrepareSource(modifications
				? Editor::ImageModified(data, modifications)
				: data);
		}
		if (session.source.isNull()) {
			show->showToast(tr::lng_oblivion_photo_io_open_failed(tr::now));
			return;
		}
		const auto source = session.source;
		Photo::ShowPhotoEditor(show, source, {
			.fileName = session.name,
			.state = session.state,
			.done = [=](Photo::PhotoEditorResult result) {
				if (result.image.isNull()) {
					show->showToast(
						tr::lng_oblivion_photo_io_process_failed(tr::now));
					return;
				}
				const auto edited = result.image;
				const auto state = result.state;
				const auto document = result.document;
				const auto original = result.source.isNull()
					? source
					: result.source;
				replace([=](Ui::PreparedFile &file) {
					ApplyEditedImage(file, edited, fileName);
					if (originals) {
						originals->remember(key, {
							.key = edited.cacheKey(),
							.source = original,
							.state = state,
							.document = document,
						});
					}
				});
			},
			.document = session.document,
			.interrupted = InterruptedHandler(&show->session()),
		});
	};
	return [=] {
		// An edit kept from an interrupted editor is offered first. It is
		// continued on its own, not as this attachment: its picture has
		// nothing to do with the attached one.
		const auto window = show->valid() ? show->resolveWindow() : nullptr;
		if (window) {
			OfferInterruptedOr(window, open);
		} else {
			open();
		}
	};
}

} // namespace

void AddAttachPhotoEditorAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<QWidget*> box,
		const Ui::PreparedFile &file,
		Fn<void(Fn<void(Ui::PreparedFile&)> apply)> replace) {
	auto open = AttachPhotoEditorOpener(
		std::move(show),
		box,
		file,
		std::move(replace));
	if (open) {
		menu->addAction(
			tr::lng_oblivion_photo_io_attach_action(tr::now),
			std::move(open),
			&st::menuIconPalette);
	}
}

bool OpenAttachPhotoEditor(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<QWidget*> box,
		const Ui::PreparedFile &file,
		Fn<void(Fn<void(Ui::PreparedFile&)> apply)> replace) {
	const auto open = AttachPhotoEditorOpener(
		std::move(show),
		box,
		file,
		std::move(replace));
	if (!open) {
		return false;
	}
	open();
	return true;
}

void AddMediaViewPhotoEditorAction(
		const Ui::Menu::MenuCallback &addAction,
		Fn<Window::SessionController*()> resolveWindow,
		PhotoData *photo,
		DocumentData *document,
		FullMsgId context,
		Fn<void()> close) {
	const auto usePhoto = photo && !photo->isNull();
	if (!resolveWindow
		|| (!usePhoto
			&& (!document || !PhotoEditorAcceptsDocument(document)))) {
		return;
	}
	addAction(tr::lng_oblivion_photo_io_viewer_action(tr::now), [=] {
		const auto window = resolveWindow();
		if (!window) {
			return;
		}
		const auto weak = base::make_weak(window);
		if (close) {
			close();
		}
		if (const auto strong = weak.get()) {
			strong->window().activate();
			if (usePhoto) {
				ShowPhotoEditorForPhoto(strong, photo, context);
			} else {
				ShowPhotoEditorForDocument(strong, document, context);
			}
		}
	}, &st::storiesComposeControls.tabbed.icons.menuGifCaption);
}

} // namespace Oblivion
