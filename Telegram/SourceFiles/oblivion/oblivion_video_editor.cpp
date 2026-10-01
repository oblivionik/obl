/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_video_editor.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/base_file_utilities.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "boxes/send_files_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "menu/menu_send_details.h"
#include "oblivion/oblivion_round_video_convert.h"
#include "oblivion/oblivion_sticker_packs.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_video_project.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "storage/localimageloader.h"
#include "storage/storage_account.h"
#include "storage/storage_media_prepare.h"
#include "ui/abstract_button.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/format_values.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>
#include <crl/crl_object_on_queue.h>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QtMath>
#include <QtGui/QCursor>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainterPath>
#include <QtGui/QRadialGradient>

#include <atomic>
#include <chrono>
#include <map>
#include <thread>

namespace Oblivion {
namespace {

using VideoEdit::Aspect;
using VideoEdit::Clip;
using VideoEdit::ExportOptions;
using VideoEdit::ExportResult;
using VideoEdit::Format;
using VideoEdit::Project;
using VideoEdit::Source;
using VideoEdit::State;

constexpr auto kBoxWidth = 660;
constexpr auto kExportBoxWidth = 440;
constexpr auto kPreviewHeight = 260; // Until the video is opened.
constexpr auto kPreviewMinHeight = 180;
constexpr auto kPreviewMaxHeight = 350;
constexpr auto kCornerRadius = 6;
constexpr auto kDimAlpha = 150;
constexpr auto kCropHandleZone = 14;
constexpr auto kCropEdgeZone = 8;
constexpr auto kCropCorner = 16;
constexpr auto kMinCrop = 0.08; // A part of the canvas.
constexpr auto kMinCropPixels = 16;

constexpr auto kTimelineHeight = 66;
constexpr auto kStripTop = 9;
constexpr auto kStripBottom = 5;
constexpr auto kStripSide = 6; // For the playhead at the first / last frame.
constexpr auto kClipGap = 3;
constexpr auto kClipMinWidth = 14;
constexpr auto kClipRadius = 5;
constexpr auto kClipBorder = 2;
constexpr auto kHandleWidth = 10;
constexpr auto kHandleSlop = 4;

constexpr auto kPlaySize = 34;
constexpr auto kToolHeight = 32;
constexpr auto kToolIcon = 20;
constexpr auto kToolPadding = 8;
constexpr auto kToolTextSkip = 5;
constexpr auto kToolSkip = 6;
constexpr auto kChipPadding = 8; // At least, chips fill the whole width.
constexpr auto kChipVertical = 7;
constexpr auto kChipSkip = 8;
constexpr auto kProgressHeight = 4;
constexpr auto kDimmedOpacity = 0.4; // Controls that can't be used now.

constexpr auto kMaxClips = 30;
constexpr auto kMaxSources = 12;
constexpr auto kMaxUndo = 100;
constexpr auto kCoarseThumbnails = 24;
constexpr auto kMaxThumbnails = 400; // For one source.
constexpr auto kRefineLimit = 48;
constexpr auto kRefineDelay = crl::time(250);
constexpr auto kFrameStep = crl::time(40);
constexpr auto kSecondStep = crl::time(1000);
constexpr auto kPlaybackSide = 640;
constexpr auto kPlaybackMaxFps = 30;
constexpr auto kPlaybackLate = crl::time(150);
constexpr auto kLegacyTempMaxAge = 6 * 3600; // Seconds.
constexpr auto kSentFileKeep = crl::time(6 * 3600 * 1000);
constexpr auto kStashedContentLimit = int64(256) * 1024 * 1024;
constexpr auto kNoticeDuration = crl::time(8000);
constexpr auto kTooltipDelay = 600;

[[nodiscard]] int Px(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QString FormatTime(crl::time ms) {
	const auto tenths = std::max(ms, crl::time(0)) / 100;
	const auto seconds = tenths / 10;
	return u"%1:%2.%3"_q
		.arg(seconds / 60)
		.arg(seconds % 60, 2, 10, QChar('0'))
		.arg(tenths % 10);
}

[[nodiscard]] QString FormatMinutes(crl::time ms) {
	const auto seconds = std::max(ms, crl::time(0)) / 1000;
	return u"%1:%2"_q
		.arg(seconds / 60)
		.arg(seconds % 60, 2, 10, QChar('0'));
}

[[nodiscard]] QString FormatPercent(float64 progress) {
	return QString::number(int(std::round(
		std::clamp(progress, 0., 1.) * 100))) + '%';
}

[[nodiscard]] QString FormatSpeed(int percent) {
	auto result = QString::number(percent / 100., 'f', 2);
	while (result.endsWith('0')) {
		result.chop(1);
	}
	if (result.endsWith('.')) {
		result.chop(1);
	}
	return result.replace('.', QLocale().decimalPoint()) + QChar(0xD7);
}

[[nodiscard]] QString FormatSize(QSize size) {
	return QString::number(size.width())
		+ QChar(0xD7)
		+ QString::number(size.height());
}

[[nodiscard]] QString SafeFileName(QString name) {
	static const auto forbidden = u"/\\:*?\"<>|"_q;
	for (auto &ch : name) {
		if (ch.unicode() < 32 || forbidden.contains(ch)) {
			ch = QChar('_');
		}
	}
	name = name.trimmed();
	while (name.startsWith('.')) {
		name.remove(0, 1);
	}
	if (name.size() > 100) {
		name = name.left(100).trimmed();
	}
	return name;
}

// "IMG_2048.MOV" -> "IMG_2048 (edit)".
[[nodiscard]] QString ExportName(const QString &sourceName) {
	const auto base = SafeFileName(QFileInfo(sourceName).completeBaseName());
	return base.isEmpty()
		? SafeFileName(tr::lng_oblivion_video_default_name(tr::now))
		: (base + u" (edit)"_q);
}

[[nodiscard]] QString FileFilter(const QString &name, const QString &mask) {
	return name + u" ("_q + mask + u")"_q;
}

[[nodiscard]] QString OpenFilter() {
	return FileFilter(
		tr::lng_oblivion_video_filter_video(tr::now),
		u"*.mp4 *.mov *.m4v *.mkv *.webm *.gif"_q)
		+ u";;"_q
		+ FileFilter(tr::lng_oblivion_video_filter_all(tr::now), u"*"_q);
}

[[nodiscard]] QString SuggestedPath(const QString &fileName) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(fileName, QString());
}

// The files to send were written to the system temporary folder before.
// What is left there is removed when nothing can still be uploading it.
void CleanupLegacyTemp() {
	const auto root = QDir(QDir::tempPath() + u"/oblivion_video_editor"_q);
	if (!root.exists()) {
		return;
	}
	const auto now = QDateTime::currentDateTime();
	const auto list = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
	for (const auto &info : list) {
		if (info.lastModified().secsTo(now) > kLegacyTempMaxAge) {
			QDir(info.absoluteFilePath()).removeRecursively();
		}
	}
}

// In a new folder, so that the file keeps the given name.
[[nodiscard]] QString TempFilePath(
		const QString &folder,
		const QString &fileName) {
	return folder
		+ '/'
		+ QString::number(base::RandomValue<uint64>(), 16)
		+ '/'
		+ fileName;
}

void RemoveWithFolder(const QString &path) {
	QFile::remove(path);
	QDir().rmdir(QFileInfo(path).absolutePath());
}

// Creates the folder if needed, removes a partially written file.
[[nodiscard]] bool WriteToFile(const QString &path, const QByteArray &bytes) {
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	} else if (file.write(bytes) != bytes.size()) {
		file.close();
		file.remove();
		return false;
	}
	file.close();
	return true;
}

// The results that are being written to their files now. A quit meanwhile
// would leave a file written by half, so the user is asked about it at once:
// see VideoEditorPreventsQuit(). The application never quits later by
// itself, the question only goes away when the files are written.
struct Writes {
	int pending = 0;
	bool forced = false; // The user doesn't want to wait for them.
	base::weak_qptr<Ui::BoxContent> asked; // "Quit anyway?"
};

[[nodiscard]] Writes &PendingWrites() {
	static auto result = Writes();
	return result;
}

// A result may be more than a gigabyte, it is written off the main thread.
// done() is called on the main thread and is released there.
void WriteAsync(
		const QString &path,
		const QByteArray &bytes,
		Fn<void(bool ok)> done) {
	++PendingWrites().pending;
	crl::async([=, done = std::move(done)]() mutable {
		const auto ok = WriteToFile(path, bytes);
		crl::on_main([ok, done = std::move(done)] {
			auto &writes = PendingWrites();
			--writes.pending;
			done(ok);
			if (writes.pending > 0) {
				return;
			}
			writes.forced = false;
			if (const auto box = writes.asked.get()) {
				// Nothing to ask about anymore. The quit is not repeated
				// here: the user quits again, with all the usual checks.
				box->closeBox();
			}
		});
	});
}

// A quit while a file is being written: the user decides, a write to a slow
// or a lost disk must not hold the application forever. "Quit" goes on with
// the other checks of the quit, Cancel and Escape change nothing.
void AskQuitWhileWriting(not_null<Window::Controller*> window) {
	auto &writes = PendingWrites();
	window->activate();
	if (writes.asked) {
		return;
	}
	const auto box = window->show(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_video_quit_writing(),
		.confirmed = [](Fn<void()> close) {
			close();
			auto &writes = PendingWrites();
			writes.forced = (writes.pending > 0);
			Core::Quit();
		},
		.confirmText = tr::lng_oblivion_video_quit(),
		.confirmStyle = &st::attentionBoxButton,
	}));
	writes.asked = box;
	if (const auto raw = box.get()) {
		// A click outside closes all the boxes, the editor as well.
		raw->setCloseByOutsideClick(false);
	}
}

// A file written to be sent, in the temporary folder of the account (so
// logging out removes it). It is removed with the last box that could send
// it, unless it was sent: the uploader reads the file later.
struct SendFile {
	explicit SendFile(QString path) : path(std::move(path)) {
	}
	~SendFile() {
		if (!sent) {
			RemoveWithFolder(path);
		}
	}

	const QString path;
	bool sent = false;
	Fn<void()> sentCallback;
};

// The sent files are removed when nothing can still be uploading them:
// here, when the account is logged out and when it starts the next time.
void RememberSentFile(not_null<Main::Session*> session, const QString &path) {
	struct Sent {
		QString path;
		base::weak_ptr<Main::Session> session;
		crl::time when = 0;
	};
	static auto list = std::vector<Sent>();

	const auto now = crl::now();
	list.erase(ranges::remove_if(list, [&](const Sent &file) {
		const auto strong = file.session.get();
		if (!strong) {
			return true;
		} else if (now - file.when < kSentFileKeep
			|| strong->uploadsInProgress()) {
			return false;
		}
		RemoveWithFolder(file.path);
		return true;
	}), end(list));
	list.push_back({ path, base::make_weak(session), now });
}

// A video downloaded from the chat only to be edited: into the temporary
// folder of the account. The file is removed when nothing needs it anymore,
// not the editor, not an export that still runs, not a project kept after
// the editor was closed.
class TempDownload final {
public:
	TempDownload(not_null<DocumentData*> document, QString path);
	~TempDownload();

	[[nodiscard]] const QString &path() const {
		return _path;
	}

	// The session is being destroyed, the folder goes away with it.
	void forget() {
		_path = QString();
	}

private:
	const base::weak_ptr<Main::Session> _session;
	const not_null<DocumentData*> _document;
	QString _path;

};

TempDownload::TempDownload(not_null<DocumentData*> document, QString path)
: _session(base::make_weak(&document->session()))
, _document(document)
, _path(std::move(path)) {
}

#ifdef Q_OS_WIN
constexpr auto kTempRemoveDelay = crl::time(1000);
constexpr auto kTempRemoveAttempts = 10;

void RemoveTempLater(
	not_null<Main::Session*> session,
	not_null<DocumentData*> document,
	const QString &path,
	int attempts);
#endif // Q_OS_WIN

TempDownload::~TempDownload() {
	const auto session = _session.get();
	if (_path.isEmpty() || !session) {
		return;
	}
	// The folder stays: the "retry" of a failed download writes there, and
	// a failed write would reset the download path in the settings.
#ifdef Q_OS_WIN
	if (!QFile::remove(_path) && QFile::exists(_path)) {
		// Windows doesn't remove a file that is open, and the readers of
		// a closed editor are destroyed on their queues after the editor.
		RemoveTempLater(session, _document, _path, kTempRemoveAttempts);
		return;
	}
#else // Q_OS_WIN
	QFile::remove(_path);
#endif // Q_OS_WIN

	// Forget the removed file, as if the user has removed it.
	(void)_document->location(true);
	session->data().requestDocumentViewRepaint(_document);
}

[[nodiscard]] std::vector<std::weak_ptr<TempDownload>> &TempDownloads() {
	static auto result = std::vector<std::weak_ptr<TempDownload>>();
	return result;
}

// The one that removes this file, if somebody still needs the file.
[[nodiscard]] std::shared_ptr<TempDownload> FindTempDownload(
		const QString &path) {
	if (path.isEmpty()) {
		return nullptr;
	}
	auto &list = TempDownloads();
	list.erase(ranges::remove_if(list, [](const auto &weak) {
		return weak.expired();
	}), end(list));
	for (const auto &weak : list) {
		const auto strong = weak.lock();
		if (strong
			&& !strong->path().isEmpty()
			&& (strong->path() == path
				|| QFileInfo(strong->path()) == QFileInfo(path))) {
			return strong;
		}
	}
	return nullptr;
}

#ifdef Q_OS_WIN
// The file is removed a bit later, when nothing has it open anymore. Not
// when somebody needs the video again: the new TempDownload removes it.
void RemoveTempLater(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		const QString &path,
		int attempts) {
	base::call_delayed(kTempRemoveDelay, session, [=] {
		if (FindTempDownload(path)) {
			return;
		} else if (QFile::remove(path) || !QFile::exists(path)) {
			// Forget the removed file, as if the user has removed it.
			(void)document->location(true);
			session->data().requestDocumentViewRepaint(document);
		} else if (attempts > 1) {
			RemoveTempLater(session, document, path, attempts - 1);
		}
	});
}
#endif // Q_OS_WIN

[[nodiscard]] std::shared_ptr<TempDownload> MakeTempDownload(
		not_null<DocumentData*> document,
		const QString &path) {
	if (auto existing = FindTempDownload(path)) {
		return existing;
	}
	auto result = std::make_shared<TempDownload>(document, path);
	TempDownloads().push_back(result);
	return result;
}

// What happened when there was no box to tell about it: shown as a toast
// in the active window, after the passcode is entered when it is locked.
enum class Notice : uchar {
	Project,
	Export,
};

struct Notices {
	QString project;
	QString exported;
	bool scheduled = false;
	bool waiting = false;
	rpl::lifetime lifetime;
};

[[nodiscard]] Notices &PendingNotices() {
	static auto result = Notices();
	return result;
}

void FlushNotices();
[[nodiscard]] bool HasStash();

void ScheduleNotices() {
	if (!std::exchange(PendingNotices().scheduled, true)) {
		crl::on_main([] {
			FlushNotices();
		});
	}
}

void FlushNotices() {
	auto &notices = PendingNotices();
	notices.scheduled = false;
	if (!Core::IsAppLaunched() || Core::Quitting()) {
		return;
	} else if (Core::App().passcodeLocked()) {
		if (!std::exchange(notices.waiting, true)) {
			notices.lifetime.destroy();
			Core::App().passcodeLockChanges(
			) | rpl::filter([](bool locked) {
				return !locked;
			}) | rpl::take(1) | rpl::on_next([] {
				// The windows are unlocked right after this.
				PendingNotices().waiting = false;
				ScheduleNotices();
			}, notices.lifetime);
		}
		return;
	}
	auto text = base::take(notices.project);
	if (!HasStash()) {
		// The project is not kept anymore: its account was logged out or
		// its videos are gone.
		text = QString();
	}
	const auto exported = base::take(notices.exported);
	if (!exported.isEmpty()) {
		if (!text.isEmpty()) {
			text += u"\n\n"_q;
		}
		text += exported;
	}
	const auto window = Core::App().activePrimaryWindow();
	if (window && !text.isEmpty()) {
		window->showToast(text, kNoticeDuration);
	}
}

void ShowNotice(Notice kind, const QString &text) {
	if (!Core::IsAppLaunched() || Core::Quitting()) {
		return;
	}
	auto &notices = PendingNotices();
	((kind == Notice::Project) ? notices.project : notices.exported) = text;
	ScheduleNotices();
}

// Wider than the usual boxes, but not wider than a narrow window.
[[nodiscard]] int BoxWidth(const std::shared_ptr<Ui::Show> &show, int wide) {
	const auto shadow = st::boxRoundShadow.extend;
	const auto available = show
		? (show->toastParent()->width() - shadow.left() - shadow.right())
		: 0;
	return (available > 0)
		? std::clamp(available, st::boxWideWidth, Px(wide))
		: Px(wide);
}

// The square from the middle of a frame.
[[nodiscard]] QImage SquareThumbnail(const QImage &image, int side) {
	if (image.isNull() || side <= 0) {
		return QImage();
	}
	const auto square = std::min(image.width(), image.height());
	return image.copy(
		(image.width() - square) / 2,
		(image.height() - square) / 2,
		square,
		square
	).scaled(side, side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

// Frames for the preview and the timeline, used only on the queue of its
// source: RoundVideo::Reader for real videos, painted frames in the UI
// snapshots.
class Media {
public:
	virtual ~Media() = default;

	// As the source shows the frame, fitted into maxSide x maxSide.
	[[nodiscard]] virtual QImage frame(crl::time position, int maxSide) = 0;

	// Squares from all over the video, null where decoding failed.
	[[nodiscard]] virtual std::vector<QImage> thumbnails(
		int count,
		int side) = 0;

};

class ReaderMedia final : public Media {
public:
	explicit ReaderMedia(const Source &source)
	: _reader(RoundVideo::Source{
		.path = source.path,
		.content = source.content,
	}) {
	}

	QImage frame(crl::time position, int maxSide) override {
		return _reader.frame(position, maxSide);
	}
	std::vector<QImage> thumbnails(int count, int side) override {
		return _reader.thumbnails(count, side);
	}

private:
	RoundVideo::Reader _reader;

};

using OpenMedia = Fn<std::unique_ptr<Media>(const Source &source)>;
using ProbeMedia = Fn<VideoCore::ClipInfo(const Source &source)>;

// Lives on the queue of a source, object_on_queue needs rvalue arguments.
class QueuedMedia final {
public:
	QueuedMedia(OpenMedia open, Source source)
	: _media(open
		? open(source)
		: std::unique_ptr<Media>(std::make_unique<ReaderMedia>(source))) {
	}

	[[nodiscard]] Media &get() const {
		return *_media;
	}

private:
	const std::unique_ptr<Media> _media;

};

// Thumbnails of the sources by their time, shared with the timeline.
struct ThumbStore {
	std::vector<std::map<crl::time, QImage>> sources;

	[[nodiscard]] const QImage *nearest(
			int source,
			crl::time position,
			crl::time *distance = nullptr) const {
		if (source < 0 || source >= int(sources.size())) {
			return nullptr;
		}
		const auto &map = sources[source];
		if (map.empty()) {
			return nullptr;
		}
		auto after = map.lower_bound(position);
		if (after == end(map)) {
			--after;
		} else if (after != begin(map)) {
			const auto before = std::prev(after);
			if (position - before->first < after->first - position) {
				after = before;
			}
		}
		if (distance) {
			*distance = std::abs(after->first - position);
		}
		return &after->second;
	}
};

// The frame of the result with the crop over it.
class Preview final : public Ui::RpWidget {
public:
	explicit Preview(QWidget *parent);

	void setCanvas(QSize canvas, int rotation);
	void setFrame(QImage frame); // As the source shows it, not rotated.
	void setCrop(QRectF crop, Aspect aspect);
	void setStatus(const QString &status);
	void setProgress(std::optional<float64> progress);
	void setInteractive(bool interactive);

	[[nodiscard]] bool hasFrame() const {
		return !_frame.isNull();
	}
	[[nodiscard]] rpl::producer<> cropStarts() const {
		return _cropStarts.events();
	}
	[[nodiscard]] rpl::producer<QRectF> cropChanges() const {
		return _cropChanges.events();
	}
	[[nodiscard]] rpl::producer<> clicks() const {
		return _clicks.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	enum class Handle : uchar {
		None,
		Move,
		Left,
		Top,
		Right,
		Bottom,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight,
	};

	[[nodiscard]] bool cropShown() const {
		return (_aspect != Aspect::Original) && !_canvas.isEmpty();
	}
	[[nodiscard]] QRectF canvasRect() const;
	[[nodiscard]] QRectF cropRect() const;
	[[nodiscard]] Handle handleAt(QPointF point) const;
	[[nodiscard]] QPointF normalized(QPointF point) const;
	void dragTo(QPointF point);
	void updateCursor(QPointF point);
	void refreshRotated();

	QImage _frame;
	QImage _rotated;
	QSize _canvas;
	int _rotation = 0;
	QRectF _crop = QRectF(0., 0., 1., 1.);
	Aspect _aspect = Aspect::Original;
	QString _status;
	std::optional<float64> _progress;
	bool _interactive = false;

	Handle _dragging = Handle::None;
	QRectF _dragCrop;
	QPointF _dragPoint;
	bool _pressed = false;

	rpl::event_stream<> _cropStarts;
	rpl::event_stream<QRectF> _cropChanges;
	rpl::event_stream<> _clicks;

};

Preview::Preview(QWidget *parent)
: RpWidget(parent) {
	setMouseTracking(true);
	resize(width(), Px(kPreviewHeight));
}

void Preview::setCanvas(QSize canvas, int rotation) {
	if (_canvas == canvas && _rotation == rotation) {
		return;
	}
	const auto rotate = (_rotation != rotation);
	_canvas = canvas;
	_rotation = rotation;
	if (rotate) {
		refreshRotated();
	}
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void Preview::setFrame(QImage frame) {
	_frame = std::move(frame);
	refreshRotated();
	update();
}

void Preview::refreshRotated() {
	_rotated = (_rotation && !_frame.isNull())
		? _frame.transformed(QTransform().rotate(_rotation))
		: _frame;
}

void Preview::setCrop(QRectF crop, Aspect aspect) {
	if (_crop == crop && _aspect == aspect) {
		return;
	}
	_crop = crop;
	_aspect = aspect;
	if (!cropShown()) {
		_dragging = Handle::None;
	}
	update();
}

void Preview::setStatus(const QString &status) {
	if (_status != status) {
		_status = status;
		update();
	}
}

void Preview::setProgress(std::optional<float64> progress) {
	_progress = progress;
	update();
}

void Preview::setInteractive(bool interactive) {
	_interactive = interactive;
	if (!interactive) {
		_dragging = Handle::None;
		_pressed = false;
	}
	updateCursor(mapFromGlobal(QCursor::pos()));
}

int Preview::resizeGetHeight(int newWidth) {
	if (_canvas.isEmpty() || newWidth <= 0) {
		return Px(kPreviewHeight);
	}
	// As tall as the frame at the full width, within the limits.
	const auto fitted = int(std::round(
		newWidth * float64(_canvas.height()) / _canvas.width()));
	return std::clamp(fitted, Px(kPreviewMinHeight), Px(kPreviewMaxHeight));
}

QRectF Preview::canvasRect() const {
	const auto outer = QRectF(rect());
	if (_canvas.isEmpty()) {
		return outer;
	}
	const auto size = QSizeF(_canvas).scaled(
		outer.size(),
		Qt::KeepAspectRatio);
	return QRectF(
		(outer.width() - size.width()) / 2.,
		(outer.height() - size.height()) / 2.,
		size.width(),
		size.height());
}

QRectF Preview::cropRect() const {
	const auto canvas = canvasRect();
	return QRectF(
		canvas.x() + _crop.x() * canvas.width(),
		canvas.y() + _crop.y() * canvas.height(),
		_crop.width() * canvas.width(),
		_crop.height() * canvas.height());
}

QPointF Preview::normalized(QPointF point) const {
	const auto canvas = canvasRect();
	return (canvas.width() > 0. && canvas.height() > 0.)
		? QPointF(
			(point.x() - canvas.x()) / canvas.width(),
			(point.y() - canvas.y()) / canvas.height())
		: QPointF();
}

Preview::Handle Preview::handleAt(QPointF point) const {
	if (!cropShown() || !_interactive) {
		return Handle::None;
	}
	const auto crop = cropRect();
	const auto corner = float64(Px(kCropHandleZone));
	const auto edge = float64(Px(kCropEdgeZone));
	const auto nearX = [&](float64 x, float64 zone) {
		return std::abs(point.x() - x) <= zone;
	};
	const auto nearY = [&](float64 y, float64 zone) {
		return std::abs(point.y() - y) <= zone;
	};
	if (nearX(crop.left(), corner) && nearY(crop.top(), corner)) {
		return Handle::TopLeft;
	} else if (nearX(crop.right(), corner) && nearY(crop.top(), corner)) {
		return Handle::TopRight;
	} else if (nearX(crop.left(), corner) && nearY(crop.bottom(), corner)) {
		return Handle::BottomLeft;
	} else if (nearX(crop.right(), corner)
		&& nearY(crop.bottom(), corner)) {
		return Handle::BottomRight;
	}
	const auto insideX = (point.x() > crop.left())
		&& (point.x() < crop.right());
	const auto insideY = (point.y() > crop.top())
		&& (point.y() < crop.bottom());
	if (insideY && nearX(crop.left(), edge)) {
		return Handle::Left;
	} else if (insideY && nearX(crop.right(), edge)) {
		return Handle::Right;
	} else if (insideX && nearY(crop.top(), edge)) {
		return Handle::Top;
	} else if (insideX && nearY(crop.bottom(), edge)) {
		return Handle::Bottom;
	}
	return (insideX && insideY) ? Handle::Move : Handle::None;
}

void Preview::updateCursor(QPointF point) {
	const auto handle = (_dragging != Handle::None)
		? _dragging
		: handleAt(point);
	switch (handle) {
	case Handle::Move: setCursor(style::cur_sizeall); break;
	case Handle::Left:
	case Handle::Right: setCursor(style::cur_sizehor); break;
	case Handle::Top:
	case Handle::Bottom: setCursor(style::cur_sizever); break;
	case Handle::TopLeft:
	case Handle::BottomRight: setCursor(style::cur_sizefdiag); break;
	case Handle::TopRight:
	case Handle::BottomLeft: setCursor(style::cur_sizebdiag); break;
	case Handle::None:
		setCursor((_interactive && hasFrame())
			? style::cur_pointer
			: style::cur_default);
		break;
	}
}

void Preview::dragTo(QPointF point) {
	const auto delta = normalized(point) - _dragPoint;
	const auto start = _dragCrop;
	const auto ratio = VideoEdit::AspectRatio(_aspect);

	// The proportions in the parts of the canvas.
	const auto fixed = (ratio > 0.)
		? (ratio * _canvas.height() / _canvas.width())
		: 0.;
	auto minWidth = std::min(
		std::max(kMinCrop, kMinCropPixels / float64(_canvas.width())),
		1.);
	auto minHeight = std::min(
		std::max(kMinCrop, kMinCropPixels / float64(_canvas.height())),
		1.);
	if (fixed > 0.) {
		minWidth = std::max(minWidth, minHeight * fixed);
		minHeight = minWidth / fixed;
	}
	const auto left = (_dragging == Handle::Left)
		|| (_dragging == Handle::TopLeft)
		|| (_dragging == Handle::BottomLeft);
	const auto right = (_dragging == Handle::Right)
		|| (_dragging == Handle::TopRight)
		|| (_dragging == Handle::BottomRight);
	const auto top = (_dragging == Handle::Top)
		|| (_dragging == Handle::TopLeft)
		|| (_dragging == Handle::TopRight);
	const auto bottom = (_dragging == Handle::Bottom)
		|| (_dragging == Handle::BottomLeft)
		|| (_dragging == Handle::BottomRight);

	auto result = start;
	if (_dragging == Handle::Move) {
		result.moveTo(
			std::clamp(
				start.x() + delta.x(),
				0.,
				std::max(1. - start.width(), 0.)),
			std::clamp(
				start.y() + delta.y(),
				0.,
				std::max(1. - start.height(), 0.)));
	} else if (fixed <= 0.) {
		// A crop left by fixed proportions may be smaller than the minimum
		// here, the limits must stay in the right order.
		auto x1 = start.left();
		auto x2 = start.right();
		auto y1 = start.top();
		auto y2 = start.bottom();
		if (left) {
			x1 = std::clamp(x1 + delta.x(), 0., std::max(x2 - minWidth, 0.));
		} else if (right) {
			x2 = std::clamp(x2 + delta.x(), std::min(x1 + minWidth, 1.), 1.);
		}
		if (top) {
			y1 = std::clamp(y1 + delta.y(), 0., std::max(y2 - minHeight, 0.));
		} else if (bottom) {
			y2 = std::clamp(y2 + delta.y(), std::min(y1 + minHeight, 1.), 1.);
		}
		result = QRectF(x1, y1, x2 - x1, y2 - y1);
	} else if ((left || right) && (top || bottom)) {
		// A corner: the opposite one stays.
		const auto anchorX = left ? start.right() : start.left();
		const auto anchorY = top ? start.bottom() : start.top();
		const auto wantX = (left ? start.left() : start.right()) + delta.x();
		const auto wantY = (top ? start.top() : start.bottom()) + delta.y();
		const auto maxWidth = left ? anchorX : (1. - anchorX);
		const auto maxHeight = top ? anchorY : (1. - anchorY);
		auto width = std::max(
			left ? (anchorX - wantX) : (wantX - anchorX),
			(top ? (anchorY - wantY) : (wantY - anchorY)) * fixed);
		width = std::max(
			std::min({ width, maxWidth, maxHeight * fixed }),
			std::min(minWidth, std::min(maxWidth, maxHeight * fixed)));
		const auto height = width / fixed;
		result = QRectF(
			left ? (anchorX - width) : anchorX,
			top ? (anchorY - height) : anchorY,
			width,
			height);
	} else if (left || right) {
		// An edge: the middle of the opposite one stays.
		const auto center = start.center().y();
		const auto maxWidth = left ? start.right() : (1. - start.left());
		const auto maxHeight = 2. * std::min(center, 1. - center);
		const auto limit = std::min(maxWidth, maxHeight * fixed);
		const auto width = std::clamp(
			start.width() + (left ? -delta.x() : delta.x()),
			std::min(minWidth, limit),
			limit);
		const auto height = width / fixed;
		result = QRectF(
			left ? (start.right() - width) : start.left(),
			center - height / 2.,
			width,
			height);
	} else {
		const auto center = start.center().x();
		const auto maxHeight = top ? start.bottom() : (1. - start.top());
		const auto maxWidth = 2. * std::min(center, 1. - center);
		const auto limit = std::min(maxHeight, maxWidth / fixed);
		const auto height = std::clamp(
			start.height() + (top ? -delta.y() : delta.y()),
			std::min(minHeight, limit),
			limit);
		const auto width = height * fixed;
		result = QRectF(
			center - width / 2.,
			top ? (start.bottom() - height) : start.top(),
			width,
			height);
	}
	if (result != _crop) {
		_crop = result;
		update();
		_cropChanges.fire_copy(result);
	}
}

void Preview::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto canvas = canvasRect();
	const auto radius = Px(kCornerRadius);
	auto rounded = QPainterPath();
	rounded.addRoundedRect(canvas, radius, radius);
	if (_rotated.isNull()) {
		p.fillPath(rounded, st::windowBgOver);
	} else {
		p.setClipPath(rounded);
		p.fillRect(canvas, Qt::black);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		const auto fitted = VideoEdit::FitRect(
			QSizeF(_rotated.size()),
			canvas.size()
		).translated(canvas.topLeft());
		p.drawImage(fitted, _rotated);
		if (cropShown()) {
			const auto crop = cropRect();
			auto dim = QPainterPath();
			dim.setFillRule(Qt::OddEvenFill);
			dim.addRect(canvas);
			dim.addRect(crop);
			p.fillPath(dim, QColor(0, 0, 0, kDimAlpha));

			// The frame is painted inside of the crop and of the rounded
			// picture: at an edge of the video nothing of it is cut off
			// by the widget or sticks out of the rounded corners.
			p.setBrush(Qt::NoBrush);
			if (_dragging != Handle::None) {
				p.setPen(QPen(QColor(255, 255, 255, 90), Px(1)));
				for (auto i = 1; i != 3; ++i) {
					const auto x = crop.left() + crop.width() * i / 3.;
					const auto y = crop.top() + crop.height() * i / 3.;
					p.drawLine(
						QPointF(x, crop.top()),
						QPointF(x, crop.bottom()));
					p.drawLine(
						QPointF(crop.left(), y),
						QPointF(crop.right(), y));
				}
			}
			const auto thin = Px(1) / 2.;
			p.setPen(QPen(QColor(255, 255, 255, 220), Px(1)));
			p.drawRect(crop.marginsRemoved(QMarginsF(thin, thin, thin, thin)));

			// The corners have a dark outline around them: white ones are
			// lost over a light frame and, at an edge of the video, they
			// merge with a light box and look like notches in the picture.
			const auto thick = float64(Px(3));
			const auto outline = float64(Px(1));
			const auto inset = thick / 2. + outline;
			const auto inner = crop.marginsRemoved(
				QMarginsF(inset, inset, inset, inset));
			const auto length = std::max(
				std::min({
					float64(Px(kCropCorner)),
					inner.width() / 3.,
					inner.height() / 3.,
				}),
				0.);
			const auto corners = [&](QColor color, float64 width) {
				auto pen = QPen(color);
				pen.setWidthF(width);
				pen.setCapStyle(Qt::SquareCap);
				pen.setJoinStyle(Qt::MiterJoin);
				p.setPen(pen);
				const auto corner = [&](
						QPointF point,
						float64 dx,
						float64 dy) {
					auto path = QPainterPath();
					path.moveTo(point + QPointF(dx * length, 0.));
					path.lineTo(point);
					path.lineTo(point + QPointF(0., dy * length));
					p.drawPath(path);
				};
				corner(inner.topLeft(), 1., 1.);
				corner(inner.topRight(), -1., 1.);
				corner(inner.bottomLeft(), 1., -1.);
				corner(inner.bottomRight(), -1., -1.);
			};
			corners(QColor(0, 0, 0, 90), thick + 2 * outline);
			corners(QColor(255, 255, 255), thick);
		}
		p.setClipping(false);
	}

	const auto side = std::min({
		canvas.width(),
		canvas.height(),
		float64(Px(96)),
	});
	// The ring with the status under it are in the middle together.
	const auto statusPadding = Px(12);
	const auto statusHeight = st::normalFont->height + statusPadding;
	const auto ringShift = (_progress && !_status.isEmpty())
		? ((statusPadding + statusHeight) / 2.)
		: 0.;
	const auto ring = QRectF(
		canvas.center().x() - side / 2.,
		std::max(canvas.center().y() - side / 2. - ringShift, canvas.top()),
		side,
		side);
	if (_progress) {
		const auto stroke = Px(4);
		const auto arc = ring.marginsRemoved({
			stroke / 2.,
			stroke / 2.,
			stroke / 2.,
			stroke / 2.,
		});
		// The track is seen over the placeholder in a dark palette too,
		// where the ripple colour is nearly the same as the placeholder.
		auto track = st::windowSubTextFg->c;
		track.setAlphaF(track.alphaF() * 0.25);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(
			(_rotated.isNull() ? track : QColor(255, 255, 255, 90)),
			stroke));
		p.drawEllipse(arc);
		auto pen = QPen(st::activeButtonBg);
		pen.setWidth(stroke);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		const auto length = int(std::round(
			std::clamp(*_progress, 0., 1.) * 360 * 16));
		p.drawArc(arc, 90 * 16, -std::max(length, 16));
	}
	if (!_status.isEmpty()) {
		const auto &font = st::normalFont;
		const auto padding = statusPadding;
		const auto available = int(canvas.width()) - 4 * padding;
		const auto text = font->elided(_status, std::max(available, 0));
		const auto height = statusHeight;
		const auto width = font->width(text) + 2 * padding;

		// In the middle, or under the ring of the progress.
		const auto middle = _progress
			? std::min(
				ring.bottom() + padding + height / 2.,
				canvas.bottom() - padding / 2. - height / 2.)
			: canvas.center().y();
		const auto pill = QRectF(
			canvas.center().x() - width / 2.,
			middle - height / 2.,
			width,
			height);
		p.setPen(Qt::NoPen);
		if (_rotated.isNull()) {
			p.setPen(st::windowSubTextFg);
		} else {
			p.setBrush(QColor(0, 0, 0, kDimAlpha));
			p.drawRoundedRect(pill, height / 2., height / 2.);
			p.setPen(QColor(255, 255, 255));
		}
		p.setFont(font);
		p.drawText(pill, Qt::AlignCenter, text);
	}
}

void Preview::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_interactive) {
		return;
	}
	const auto point = QPointF(e->pos());
	const auto handle = handleAt(point);
	if (handle == Handle::None) {
		_pressed = true;
		return;
	}
	_dragging = handle;
	_dragCrop = _crop;
	_dragPoint = normalized(point);
	_cropStarts.fire({});
	updateCursor(point);
	update();
}

void Preview::mouseMoveEvent(QMouseEvent *e) {
	const auto point = QPointF(e->pos());
	if (_dragging == Handle::None) {
		updateCursor(point);
		return;
	}
	dragTo(point);
}

void Preview::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto point = QPointF(e->pos());
	if (_dragging != Handle::None) {
		dragTo(point);
		_dragging = Handle::None;
		updateCursor(point);
		update();
	} else if (std::exchange(_pressed, false)
		&& _interactive
		&& rect().contains(e->pos())) {
		_clicks.fire({});
	}
}

// The clips one after another, as wide as they are long: thumbnails, the
// playhead and the trim handles of the chosen clip.
class Timeline final : public Ui::RpWidget {
public:
	enum class Edge : uchar {
		None,
		Start,
		End,
	};
	struct Item {
		int source = 0;
		crl::time from = 0;
		crl::time till = 0;
		crl::time duration = 0; // Of the whole source.
	};
	struct Trim {
		int index = 0;
		crl::time from = 0;
		crl::time till = 0;
		Edge edge = Edge::None;
	};
	struct Tile {
		int source = 0;
		crl::time position = 0;
	};

	Timeline(QWidget *parent, std::shared_ptr<const ThumbStore> thumbs);

	void setItems(std::vector<Item> items, int selected);
	void setSelected(int selected);
	void setPosition(crl::time position);
	void setRotation(int rotation);
	void setInteractive(bool interactive);
	void setKeyHandler(Fn<bool(not_null<QKeyEvent*>)> handler);

	[[nodiscard]] bool dragging() const {
		return _drag.has_value();
	}

	// Tiles that have no thumbnail close enough to their time.
	[[nodiscard]] std::vector<Tile> missingTiles(int limit) const;

	[[nodiscard]] rpl::producer<crl::time> seeks() const {
		return _seeks.events();
	}
	[[nodiscard]] rpl::producer<> trimStarts() const {
		return _trimStarts.events();
	}
	[[nodiscard]] rpl::producer<Trim> trims() const {
		return _trims.events();
	}
	[[nodiscard]] rpl::producer<> trimFinishes() const {
		return _trimFinishes.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	struct Slot {
		int left = 0;
		int width = 0;
	};
	struct Drag {
		Edge edge = Edge::None;
		int index = 0;
		int grab = 0; // From the edge to the cursor.
		crl::time from = 0;
		crl::time till = 0;
		float64 scale = 0.; // Pixels of the clip per millisecond.
		std::vector<Slot> slots;
	};

	[[nodiscard]] crl::time length(int index) const;
	[[nodiscard]] crl::time startOf(int index) const;
	[[nodiscard]] QRect strip() const;
	[[nodiscard]] int handleWidth(const Slot &slot) const;
	[[nodiscard]] int xFor(crl::time position) const;
	[[nodiscard]] crl::time positionAt(int x) const;
	[[nodiscard]] Edge edgeAt(int x) const;
	template <typename Callback>
	void enumerateTiles(int index, Callback &&callback) const;
	void relayout();
	void layoutDragged();
	void trimTo(int x);
	void updateCursor(int x);
	void paintClip(QPainter &p, int index);

	const std::shared_ptr<const ThumbStore> _thumbs;
	std::vector<Item> _items;
	std::vector<Slot> _slots;
	int _layoutWidth = 0;
	int _selected = 0;
	int _rotation = 0;
	crl::time _position = 0;
	bool _interactive = false;
	bool _scrubbing = false;
	std::optional<Drag> _drag;
	Fn<bool(not_null<QKeyEvent*>)> _keyHandler;

	rpl::event_stream<crl::time> _seeks;
	rpl::event_stream<> _trimStarts;
	rpl::event_stream<Trim> _trims;
	rpl::event_stream<> _trimFinishes;

};

Timeline::Timeline(QWidget *parent, std::shared_ptr<const ThumbStore> thumbs)
: RpWidget(parent)
, _thumbs(std::move(thumbs)) {
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	resize(width(), Px(kTimelineHeight));
}

void Timeline::setItems(std::vector<Item> items, int selected) {
	_items = std::move(items);
	_selected = selected;
	if (_drag && _drag->index < int(_items.size())) {
		layoutDragged();
	} else {
		_drag = std::nullopt;
		relayout();
	}
	update();
}

void Timeline::setSelected(int selected) {
	if (_selected != selected) {
		_selected = selected;
		update();
	}
}

void Timeline::setPosition(crl::time position) {
	if (_position != position) {
		_position = position;
		update();
	}
}

void Timeline::setRotation(int rotation) {
	if (_rotation != rotation) {
		_rotation = rotation;
		update();
	}
}

void Timeline::setInteractive(bool interactive) {
	_interactive = interactive;
	if (!interactive) {
		_scrubbing = false;
		if (base::take(_drag)) {
			relayout();
		}
	}
	update();
}

void Timeline::setKeyHandler(Fn<bool(not_null<QKeyEvent*>)> handler) {
	_keyHandler = std::move(handler);
}

int Timeline::resizeGetHeight(int newWidth) {
	_layoutWidth = newWidth;
	if (!_drag) {
		relayout();
	}
	return Px(kTimelineHeight);
}

crl::time Timeline::length(int index) const {
	return std::max(_items[index].till - _items[index].from, crl::time(1));
}

crl::time Timeline::startOf(int index) const {
	auto result = crl::time(0);
	for (auto i = 0; i != index; ++i) {
		result += length(i);
	}
	return result;
}

// The widget is wider than the clips, so that the head of the playhead is
// not cut off at the very start and at the very end.
QRect Timeline::strip() const {
	const auto top = Px(kStripTop);
	const auto side = Px(kStripSide);
	return QRect(
		side,
		top,
		std::max(width() - 2 * side, 0),
		height() - top - Px(kStripBottom));
}

int Timeline::handleWidth(const Slot &slot) const {
	return std::clamp((slot.width - Px(4)) / 2, Px(3), Px(kHandleWidth));
}

void Timeline::relayout() {
	const auto count = int(_items.size());
	_slots.assign(count, Slot());
	if (!count || _layoutWidth <= 0) {
		return;
	}
	const auto gap = Px(kClipGap);
	const auto side = Px(kStripSide);
	const auto available = std::max(
		_layoutWidth - 2 * side - (count - 1) * gap,
		count);
	const auto minWidth = std::min(Px(kClipMinWidth), available / count);

	// Clips are as wide as they are long, but the shortest ones can
	// still be seen and chosen.
	auto narrow = std::vector<bool>(count, false);
	auto widths = std::vector<float64>(count, 0.);
	while (true) {
		auto rest = float64(available);
		auto total = crl::time(0);
		for (auto i = 0; i != count; ++i) {
			if (narrow[i]) {
				rest -= minWidth;
			} else {
				total += length(i);
			}
		}
		auto changed = false;
		for (auto i = 0; i != count; ++i) {
			if (narrow[i]) {
				widths[i] = minWidth;
				continue;
			}
			widths[i] = rest * length(i) / float64(total);
			if (widths[i] < minWidth) {
				narrow[i] = true;
				changed = true;
			}
		}
		if (!changed) {
			break;
		}
	}
	auto x = 0.;
	for (auto i = 0; i != count; ++i) {
		const auto left = int(std::round(x));
		x += widths[i];
		const auto right = int(std::round(x));
		_slots[i] = { side + left + i * gap, std::max(right - left, 1) };
	}
}

// While an edge is dragged the scale stays as it was, so that the edge
// follows the cursor: the clips after the dragged end move with it, the
// clips before the dragged start move the other way.
void Timeline::layoutDragged() {
	const auto &drag = *_drag;
	_slots = drag.slots;
	const auto index = drag.index;
	const auto width = std::max(
		int(std::round(length(index) * drag.scale)),
		1);
	const auto delta = width - drag.slots[index].width;
	_slots[index].width = width;
	const auto count = int(std::min(_slots.size(), _items.size()));
	if (drag.edge == Edge::Start) {
		for (auto i = 0; i <= index && i < count; ++i) {
			_slots[i].left -= delta;
		}
	} else {
		for (auto i = index + 1; i < count; ++i) {
			_slots[i].left += delta;
		}
	}
	_slots.resize(_items.size(), Slot());
}

int Timeline::xFor(crl::time position) const {
	auto start = crl::time(0);
	const auto count = int(std::min(_slots.size(), _items.size()));
	for (auto i = 0; i != count; ++i) {
		const auto duration = length(i);
		if (position < start + duration || i + 1 == count) {
			const auto inside = std::clamp(
				position - start,
				crl::time(0),
				duration);
			return _slots[i].left
				+ int(std::round(
					_slots[i].width * float64(inside) / duration));
		}
		start += duration;
	}
	return 0;
}

crl::time Timeline::positionAt(int x) const {
	auto start = crl::time(0);
	const auto count = int(std::min(_slots.size(), _items.size()));
	const auto gap = Px(kClipGap);
	for (auto i = 0; i != count; ++i) {
		const auto duration = length(i);
		const auto &slot = _slots[i];
		if (x < slot.left + slot.width + (gap + 1) / 2 || i + 1 == count) {
			const auto part = std::clamp(
				(x - slot.left) / float64(slot.width),
				0.,
				1.);
			return start + crl::time(std::round(part * duration));
		}
		start += duration;
	}
	return 0;
}

Timeline::Edge Timeline::edgeAt(int x) const {
	if (!_interactive
		|| _selected < 0
		|| _selected >= int(_slots.size())) {
		return Edge::None;
	}
	const auto &slot = _slots[_selected];
	const auto handle = handleWidth(slot);
	const auto slop = Px(kHandleSlop);
	const auto left = slot.left;
	const auto right = slot.left + slot.width;
	const auto nearLeft = (x >= left - slop) && (x <= left + handle + slop);
	const auto nearRight = (x >= right - handle - slop)
		&& (x <= right + slop);
	if (nearLeft && nearRight) {
		return (x - left <= right - x) ? Edge::Start : Edge::End;
	}
	return nearLeft ? Edge::Start : nearRight ? Edge::End : Edge::None;
}

void Timeline::updateCursor(int x) {
	const auto edge = _drag ? _drag->edge : edgeAt(x);
	setCursor((edge != Edge::None)
		? style::cur_sizehor
		: (_interactive && !_items.empty())
		? style::cur_pointer
		: style::cur_default);
}

// callback(QRect tile, crl::time position, crl::time tileDuration).
template <typename Callback>
void Timeline::enumerateTiles(int index, Callback &&callback) const {
	const auto area = strip();
	const auto side = area.height();
	const auto &slot = _slots[index];
	const auto &item = _items[index];
	if (side <= 0 || slot.width <= 0) {
		return;
	}
	const auto duration = length(index);
	const auto perPixel = duration / float64(slot.width);
	const auto tileDuration = crl::time(std::round(side * perPixel));
	for (auto x = 0; x < slot.width; x += side) {
		const auto center = std::min(x + side / 2., slot.width - 0.5);
		const auto position = item.from + std::clamp(
			crl::time(std::round(center * perPixel)),
			crl::time(0),
			duration - 1);
		callback(
			QRect(slot.left + x, area.top(), side, side),
			position,
			tileDuration);
	}
}

std::vector<Timeline::Tile> Timeline::missingTiles(int limit) const {
	auto result = std::vector<Tile>();
	const auto count = int(std::min(_slots.size(), _items.size()));
	for (auto i = 0; i != count; ++i) {
		const auto left = _slots[i].left;
		if (left >= width() || left + _slots[i].width <= 0) {
			continue;
		}
		const auto source = _items[i].source;
		enumerateTiles(i, [&](QRect, crl::time position, crl::time tile) {
			if (int(result.size()) >= limit) {
				return;
			}
			auto distance = crl::time(0);
			const auto found = _thumbs->nearest(source, position, &distance);
			if (!found || distance > tile / 2) {
				result.push_back({ source, position });
			}
		});
	}
	return result;
}

void Timeline::paintClip(QPainter &p, int index) {
	const auto &slot = _slots[index];
	const auto area = strip();
	const auto rect = QRect(slot.left, area.top(), slot.width, area.height());
	if (rect.right() < 0 || rect.left() >= width()) {
		return;
	}
	const auto radius = std::min(Px(kClipRadius), rect.width() / 2);
	auto clip = QPainterPath();
	clip.addRoundedRect(rect, radius, radius);
	p.setClipPath(clip);
	p.fillRect(rect, st::windowBgOver);
	const auto source = _items[index].source;
	enumerateTiles(index, [&](QRect tile, crl::time position, crl::time) {
		const auto image = _thumbs->nearest(source, position);
		if (!image || image->isNull()) {
			return;
		}
		if (_rotation) {
			p.save();
			p.translate(QRectF(tile).center());
			p.rotate(_rotation);
			p.drawImage(
				QRectF(
					-tile.width() / 2.,
					-tile.height() / 2.,
					tile.width(),
					tile.height()),
				*image);
			p.restore();
		} else {
			p.drawImage(tile, *image);
		}
	});
	const auto selected = (index == _selected) && _interactive;
	if (!selected && _items.size() > 1) {
		p.fillRect(rect, QColor(0, 0, 0, 70));
	}
	p.setClipping(false);
	if (!selected) {
		return;
	}

	// The frame with the handles.
	const auto handle = handleWidth(slot);
	const auto border = Px(kClipBorder);
	auto frame = QPainterPath();
	frame.setFillRule(Qt::OddEvenFill);
	frame.addRoundedRect(rect, radius, radius);
	frame.addRect(rect.marginsRemoved({ handle, border, handle, border }));
	p.fillPath(frame, st::activeButtonBg);

	auto pen = QPen(st::activeButtonFg);
	pen.setWidthF(Px(2));
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	const auto top = rect.top() + rect.height() * 0.36;
	const auto bottom = rect.top() + rect.height() * 0.64;
	const auto leftGrip = rect.left() + handle / 2.;
	const auto rightGrip = rect.left() + rect.width() - handle / 2.;
	p.drawLine(QPointF(leftGrip, top), QPointF(leftGrip, bottom));
	p.drawLine(QPointF(rightGrip, top), QPointF(rightGrip, bottom));
}

void Timeline::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto area = strip();
	const auto count = int(std::min(_slots.size(), _items.size()));
	if (!count) {
		const auto radius = Px(kClipRadius);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(area, radius, radius);
		return;
	}
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	for (auto i = 0; i != count; ++i) {
		paintClip(p, i);
	}
	if (!_interactive) {
		return;
	}

	// The playhead: the line is of the same colour as its head, which is
	// over the box, and has an outline to be seen over any frame.
	const auto x = std::clamp(xFor(_position), 0, width()) + 0.;
	const auto line = float64(Px(2));
	auto outline = st::windowBg->c;
	outline.setAlphaF(0.6);
	p.setPen(Qt::NoPen);
	p.setBrush(outline);
	p.drawRect(QRectF(
		x - line,
		area.top(),
		2 * line,
		area.height()));
	p.setBrush(st::windowFg);
	p.drawRect(QRectF(
		x - line / 2.,
		area.top() - Px(2),
		line,
		area.height() + Px(4)));
	const auto knob = Px(kStripTop) - Px(2);
	auto head = QPainterPath();
	head.moveTo(x - knob * 0.75, 0.);
	head.lineTo(x + knob * 0.75, 0.);
	head.lineTo(x + knob * 0.75, knob * 0.45);
	head.lineTo(x, knob);
	head.lineTo(x - knob * 0.75, knob * 0.45);
	head.closeSubpath();
	p.setBrush(st::windowFg);
	p.drawPath(head);
}

void Timeline::mousePressEvent(QMouseEvent *e) {
	setFocus();
	if (e->button() != Qt::LeftButton || !_interactive || _items.empty()) {
		return;
	}
	const auto x = e->pos().x();
	const auto edge = edgeAt(x);
	if (edge == Edge::None) {
		_scrubbing = true;
		_seeks.fire(positionAt(x));
		return;
	}
	const auto &slot = _slots[_selected];
	const auto &item = _items[_selected];
	_drag = Drag{
		.edge = edge,
		.index = _selected,
		.grab = x - ((edge == Edge::Start)
			? slot.left
			: (slot.left + slot.width)),
		.from = item.from,
		.till = item.till,
		.scale = slot.width / float64(length(_selected)),
		.slots = _slots,
	};
	_trimStarts.fire({});
	updateCursor(x);
}

void Timeline::trimTo(int x) {
	if (!_drag || _drag->index >= int(_items.size())) {
		return;
	}
	const auto &drag = *_drag;
	auto &item = _items[drag.index];
	const auto &slot = drag.slots[drag.index];
	const auto minimal = std::min(VideoEdit::kMinClipLength, item.duration);
	const auto shift = [&](int edge) {
		return crl::time(std::round((x - drag.grab - edge) / drag.scale));
	};
	auto from = item.from;
	auto till = item.till;
	if (drag.edge == Edge::Start) {
		from = std::clamp(
			drag.from + shift(slot.left),
			crl::time(0),
			std::max(drag.till - minimal, crl::time(0)));
	} else {
		till = std::clamp(
			drag.till + shift(slot.left + slot.width),
			std::min(drag.from + minimal, item.duration),
			item.duration);
	}
	if (from == item.from && till == item.till) {
		return;
	}
	item.from = from;
	item.till = till;
	layoutDragged();
	update();
	_trims.fire({
		.index = drag.index,
		.from = from,
		.till = till,
		.edge = drag.edge,
	});
}

void Timeline::mouseMoveEvent(QMouseEvent *e) {
	const auto x = e->pos().x();
	if (_drag) {
		trimTo(x);
	} else if (_scrubbing) {
		_seeks.fire(positionAt(x));
	} else {
		updateCursor(x);
	}
}

void Timeline::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto x = e->pos().x();
	_scrubbing = false;
	if (_drag) {
		trimTo(x);
		_drag = std::nullopt;
		relayout();
		update();
		_trimFinishes.fire({});
	}
	updateCursor(x);
}

void Timeline::keyPressEvent(QKeyEvent *e) {
	if (!_keyHandler || !_keyHandler(e)) {
		e->ignore();
	}
}

enum class Glyph : uchar {
	Split,
	Delete,
	MoveLeft,
	MoveRight,
	Rotate,
	Undo,
	Redo,
	Add,
};

// Drawn in a 20x20 square.
void PaintGlyph(QPainter &p, Glyph glyph, QRectF rect, QColor color) {
	p.save();
	p.translate(rect.topLeft());
	p.scale(rect.width() / 20., rect.height() / 20.);
	auto pen = QPen(color);
	pen.setWidthF(1.6);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto arrow = [&] {
		auto path = QPainterPath();
		path.moveTo(15.5, 10.);
		path.lineTo(4.5, 10.);
		path.moveTo(8.5, 6.);
		path.lineTo(4.5, 10.);
		path.lineTo(8.5, 14.);
		p.drawPath(path);
	};
	const auto turn = [&] {
		auto path = QPainterPath();
		path.moveTo(7.5, 4.5);
		path.lineTo(4.5, 7.5);
		path.lineTo(7.5, 10.5);
		path.moveTo(4.5, 7.5);
		path.lineTo(12., 7.5);
		path.arcTo(QRectF(8.25, 7.5, 7.5, 7.5), 90., -180.);
		path.lineTo(8., 15.);
		p.drawPath(path);
	};
	const auto mirror = [&] {
		p.translate(20., 0.);
		p.scale(-1., 1.);
	};
	switch (glyph) {
	case Glyph::Split:
		// Two frames with a cut between them, not tall as "0|0".
		p.drawRoundedRect(QRectF(1.5, 6.5, 5.5, 7.), 1.5, 1.5);
		p.drawRoundedRect(QRectF(13., 6.5, 5.5, 7.), 1.5, 1.5);
		p.drawLine(QPointF(10., 3.5), QPointF(10., 16.5));
		break;
	case Glyph::Delete: {
		auto path = QPainterPath();
		path.moveTo(4., 6.);
		path.lineTo(16., 6.);
		path.moveTo(8., 6.);
		path.lineTo(8., 4.2);
		path.lineTo(12., 4.2);
		path.lineTo(12., 6.);
		path.moveTo(5.5, 6.);
		path.lineTo(6.3, 16.);
		path.lineTo(13.7, 16.);
		path.lineTo(14.5, 6.);
		path.moveTo(8.6, 9.);
		path.lineTo(8.6, 13.);
		path.moveTo(11.4, 9.);
		path.lineTo(11.4, 13.);
		p.drawPath(path);
	} break;
	case Glyph::MoveLeft:
		arrow();
		break;
	case Glyph::MoveRight:
		mirror();
		arrow();
		break;
	case Glyph::Rotate: {
		const auto circle = QRectF(4.5, 5., 11., 11.);
		auto path = QPainterPath();
		path.arcMoveTo(circle, 40.);
		path.arcTo(circle, 40., 280.);
		p.drawPath(path);
		auto head = QPainterPath();
		head.moveTo(16.3, 9.6);
		head.lineTo(15.9, 5.4);
		head.lineTo(12.3, 8.2);
		head.closeSubpath();
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawPath(head);
	} break;
	case Glyph::Undo:
		turn();
		break;
	case Glyph::Redo:
		mirror();
		turn();
		break;
	case Glyph::Add:
		p.drawLine(QPointF(10., 4.5), QPointF(10., 15.5));
		p.drawLine(QPointF(4.5, 10.), QPointF(15.5, 10.));
		break;
	}
	p.restore();
}

class ToolButton final
	: public Ui::AbstractButton
	, public Ui::AbstractTooltipShower {
public:
	ToolButton(QWidget *parent, Glyph glyph, QString text);

	[[nodiscard]] int naturalWidth(bool withText) const;
	void setTextShown(bool shown);

	// What a button without a text does. A button whose text did not fit
	// shows the text this way.
	void setTooltip(QString tooltip);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	const Glyph _glyph;
	const QString _text;
	QString _tooltip;
	bool _textShown = true;

};

ToolButton::ToolButton(QWidget *parent, Glyph glyph, QString text)
: AbstractButton(parent)
, _glyph(glyph)
, _text(std::move(text)) {
	resize(naturalWidth(true), Px(kToolHeight));
}

void ToolButton::setTooltip(QString tooltip) {
	_tooltip = std::move(tooltip);
}

QString ToolButton::tooltipText() const {
	return (_textShown && !_text.isEmpty())
		? QString()
		: _tooltip.isEmpty()
		? _text
		: _tooltip;
}

QPoint ToolButton::tooltipPos() const {
	return QCursor::pos();
}

bool ToolButton::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void ToolButton::enterEventHook(QEnterEvent *e) {
	if (!tooltipText().isEmpty()) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	}
	AbstractButton::enterEventHook(e);
}

void ToolButton::leaveEventHook(QEvent *e) {
	Ui::Tooltip::Hide();
	AbstractButton::leaveEventHook(e);
}

int ToolButton::naturalWidth(bool withText) const {
	return 2 * Px(kToolPadding)
		+ Px(kToolIcon)
		+ ((withText && !_text.isEmpty())
			? (Px(kToolTextSkip) + st::normalFont->width(_text) + Px(2))
			: 0);
}

void ToolButton::setTextShown(bool shown) {
	if (_textShown != shown) {
		_textShown = shown;
		update();
	}
}

void ToolButton::onStateChanged(State was, StateChangeSource source) {
	update();
}

void ToolButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	if (isDisabled()) {
		p.setOpacity(kDimmedOpacity);
	}
	const auto radius = Px(kCornerRadius);
	p.setPen(Qt::NoPen);
	p.setBrush((isOver() && !isDisabled())
		? st::windowBgRipple
		: st::windowBgOver);
	p.drawRoundedRect(rect(), radius, radius);

	const auto icon = Px(kToolIcon);
	const auto withText = _textShown && !_text.isEmpty();
	const auto content = naturalWidth(withText) - 2 * Px(kToolPadding);
	const auto left = (width() - content) / 2;
	PaintGlyph(
		p,
		_glyph,
		QRectF(left, (height() - icon) / 2., icon, icon),
		st::windowFg->c);
	if (withText) {
		p.setPen(st::windowFg);
		p.setFont(st::normalFont);
		p.drawText(
			QRect(
				left + icon + Px(kToolTextSkip),
				0,
				width() - left - icon - Px(kToolTextSkip),
				height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			_text);
	}
}

// A row of tool buttons, some of them at the right edge. The texts that
// don't fit are hidden one by one, the least important ones first.
class ToolBar final : public Ui::RpWidget {
public:
	explicit ToolBar(QWidget *parent);

	// keep: how important the text is, the lowest one goes first.
	not_null<ToolButton*> add(
		Glyph glyph,
		QString text,
		int keep = 0,
		bool right = false);

protected:
	int resizeGetHeight(int newWidth) override;

private:
	struct Entry {
		not_null<ToolButton*> button;
		int keep = 0;
		bool right = false;
	};
	std::vector<Entry> _entries;

};

ToolBar::ToolBar(QWidget *parent)
: RpWidget(parent) {
}

not_null<ToolButton*> ToolBar::add(
		Glyph glyph,
		QString text,
		int keep,
		bool right) {
	const auto button = Ui::CreateChild<ToolButton>(
		this,
		glyph,
		std::move(text));
	_entries.push_back({ button, keep, right });
	return button;
}

int ToolBar::resizeGetHeight(int newWidth) {
	const auto skip = Px(kToolSkip);
	const auto height = Px(kToolHeight);
	const auto count = int(_entries.size());
	if (!count) {
		return height;
	}
	auto shown = std::vector<bool>(count, true);
	const auto total = [&] {
		auto result = (count - 1) * skip;
		for (auto i = 0; i != count; ++i) {
			result += _entries[i].button->naturalWidth(shown[i]);
		}
		return result;
	};
	while (total() > newWidth) {
		auto drop = -1;
		for (auto i = 0; i != count; ++i) {
			if (shown[i]
				&& _entries[i].keep > 0
				&& (drop < 0 || _entries[i].keep < _entries[drop].keep)) {
				drop = i;
			}
		}
		if (drop < 0) {
			break;
		}
		shown[drop] = false;
	}
	for (auto i = 0; i != count; ++i) {
		_entries[i].button->setTextShown(shown[i]);
	}
	if (total() > newWidth) {
		// Too narrow even for the icons: equal buttons side by side.
		const auto gap = Px(2);
		const auto edge = [&](int index) {
			return index * (newWidth + gap) / count;
		};
		auto index = 0;
		for (const auto right : { false, true }) {
			for (const auto &entry : _entries) {
				if (entry.right == right) {
					const auto left = edge(index++);
					entry.button->setGeometry(
						left,
						0,
						std::max(edge(index) - gap - left, 1),
						height);
				}
			}
		}
		return height;
	}
	auto left = 0;
	auto right = newWidth;
	for (auto i = 0; i != count; ++i) {
		if (!_entries[i].right) {
			const auto width = _entries[i].button->naturalWidth(shown[i]);
			_entries[i].button->setGeometry(left, 0, width, height);
			left += width + skip;
		}
	}
	for (auto i = count; i != 0;) {
		if (_entries[--i].right) {
			const auto width = _entries[i].button->naturalWidth(shown[i]);
			right -= width;
			_entries[i].button->setGeometry(right, 0, width, height);
			right -= skip;
		}
	}
	return height;
}

// Choices filling the whole width, in as few rows as their texts need.
class Chips final : public Ui::RpWidget {
public:
	struct Chip {
		int id = 0;
		QString text;
	};

	Chips(QWidget *parent, std::vector<Chip> chips, int selected);

	void setChips(std::vector<Chip> chips, int selected);
	void setSelected(int id);
	void setDimmed(bool dimmed); // Shown as not available.

	[[nodiscard]] rpl::producer<int> chosen() const {
		return _chosen.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] int chipAt(QPoint point) const;
	void setOver(int index);

	std::vector<Chip> _chips;
	std::vector<QRect> _rects;
	int _selected = 0;
	int _over = -1;
	int _pressed = -1;
	bool _dimmed = false;
	rpl::event_stream<int> _chosen;

};

Chips::Chips(QWidget *parent, std::vector<Chip> chips, int selected)
: RpWidget(parent)
, _chips(std::move(chips))
, _selected(selected) {
	setMouseTracking(true);
}

void Chips::setChips(std::vector<Chip> chips, int selected) {
	_chips = std::move(chips);
	_selected = selected;
	_over = _pressed = -1;
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void Chips::setSelected(int id) {
	if (_selected != id) {
		_selected = id;
		update();
	}
}

void Chips::setDimmed(bool dimmed) {
	if (_dimmed != dimmed) {
		_dimmed = dimmed;
		setCursor((_over >= 0 && !_dimmed)
			? style::cur_pointer
			: style::cur_default);
		update();
	}
}

int Chips::resizeGetHeight(int newWidth) {
	const auto &font = st::normalFont;
	const auto padding = Px(kChipPadding);
	const auto height = font->height + 2 * Px(kChipVertical);
	const auto skip = Px(kChipSkip);
	const auto count = int(_chips.size());
	_rects.clear();
	if (!count || newWidth <= 0) {
		return 0;
	}

	// The fewest rows where the widest text fits in equal chips, the
	// rows filled evenly. Each row takes the whole width: where there
	// are less chips they are wider, there is no hole after the last one
	// (five formats are three and two, not two, two and one).
	auto widest = 0;
	for (const auto &chip : _chips) {
		widest = std::max(widest, font->width(chip.text) + 2 * padding);
	}
	auto columns = count;
	while (columns > 1
		&& (newWidth - (columns - 1) * skip) / columns < widest) {
		--columns;
	}
	const auto rows = (count + columns - 1) / columns;
	columns = (count + rows - 1) / rows;
	auto index = 0;
	for (auto row = 0; row != rows; ++row) {
		const auto inRow = (count - index + (rows - row) - 1) / (rows - row);

		// One chip left for a row stays as wide as the ones above it.
		const auto parts = (inRow > 1) ? inRow : columns;
		const auto edge = [&](int column) {
			return column * (newWidth + skip) / parts;
		};
		for (auto column = 0; column != inRow; ++column, ++index) {
			const auto left = edge(column);
			_rects.emplace_back(
				left,
				row * (height + skip),
				edge(column + 1) - skip - left,
				height);
		}
	}
	return rows * height + (rows - 1) * skip;
}

void Chips::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto padding = Px(kChipPadding);
	p.setFont(st::normalFont);
	if (_dimmed) {
		p.setOpacity(kDimmedOpacity);
	}
	const auto count = int(std::min(_chips.size(), _rects.size()));
	for (auto i = 0; i != count; ++i) {
		const auto &rect = _rects[i];
		const auto selected = (_chips[i].id == _selected);
		const auto radius = rect.height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(selected
			? st::activeButtonBg
			: (i == _over && !_dimmed)
			? st::windowBgRipple
			: st::windowBgOver);
		p.drawRoundedRect(rect, radius, radius);

		// A dimmed chosen chip is nearly of the colour of the box, the
		// text of an active button can't be read over it.
		p.setPen((selected && !_dimmed)
			? st::activeButtonFg
			: st::windowFg);
		p.drawText(
			rect,
			Qt::AlignCenter,
			st::normalFont->elided(
				_chips[i].text,
				std::max(rect.width() - 2 * padding, 0)));
	}
}

int Chips::chipAt(QPoint point) const {
	for (auto i = 0; i != int(_rects.size()); ++i) {
		if (_rects[i].contains(point)) {
			return i;
		}
	}
	return -1;
}

void Chips::setOver(int index) {
	if (_over == index) {
		return;
	}
	_over = index;
	setCursor((index >= 0 && !_dimmed)
		? style::cur_pointer
		: style::cur_default);
	update();
}

void Chips::mouseMoveEvent(QMouseEvent *e) {
	setOver(chipAt(e->pos()));
}

void Chips::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = chipAt(e->pos());
	}
}

void Chips::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (e->button() == Qt::LeftButton
		&& pressed >= 0
		&& pressed == chipAt(e->pos())
		&& pressed < int(_chips.size())) {
		_chosen.fire_copy(_chips[pressed].id);
	}
}

void Chips::leaveEventHook(QEvent *e) {
	setOver(-1);
}

class PlayButton final : public Ui::AbstractButton {
public:
	explicit PlayButton(QWidget *parent);

	void setPlaying(bool playing);

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	bool _playing = false;

};

PlayButton::PlayButton(QWidget *parent)
: AbstractButton(parent) {
	resize(Px(kPlaySize), Px(kPlaySize));
}

void PlayButton::setPlaying(bool playing) {
	if (_playing != playing) {
		_playing = playing;
		update();
	}
}

void PlayButton::onStateChanged(State was, StateChangeSource source) {
	update();
}

void PlayButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	if (isDisabled()) {
		p.setOpacity(0.5);
	}
	p.setPen(Qt::NoPen);
	p.setBrush((isOver() && !isDisabled())
		? st::activeButtonBgOver
		: st::activeButtonBg);
	p.drawEllipse(rect());
	p.setBrush(st::activeButtonFg);
	const auto side = float64(width());
	if (_playing) {
		const auto bar = side * 0.12;
		const auto height = side * 0.4;
		const auto gap = side * 0.1;
		const auto top = (side - height) / 2.;
		const auto radius = bar / 3.;
		p.drawRoundedRect(
			QRectF(side / 2. - gap / 2. - bar, top, bar, height),
			radius,
			radius);
		p.drawRoundedRect(
			QRectF(side / 2. + gap / 2., top, bar, height),
			radius,
			radius);
	} else {
		const auto height = side * 0.42;
		const auto width = height * 0.87;
		const auto left = (side - width) / 2. + side * 0.04;
		const auto top = (side - height) / 2.;
		auto path = QPainterPath();
		path.moveTo(left, top);
		path.lineTo(left + width, side / 2.);
		path.lineTo(left, top + height);
		path.closeSubpath();
		p.drawPath(path);
	}
}

// The play button, the time and what the result is.
class Transport final : public Ui::RpWidget {
public:
	explicit Transport(QWidget *parent);

	void setPlaying(bool playing);
	void setTime(const QString &time);
	void setInfo(const QString &info);
	void setPlayEnabled(bool enabled);

	[[nodiscard]] rpl::producer<> playClicks() const;

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	const not_null<PlayButton*> _play;
	QString _time;
	QString _info;

};

Transport::Transport(QWidget *parent)
: RpWidget(parent)
, _play(Ui::CreateChild<PlayButton>(this)) {
	resize(width(), Px(kPlaySize));
	_play->move(0, 0);
}

void Transport::setPlaying(bool playing) {
	_play->setPlaying(playing);
}

void Transport::setTime(const QString &time) {
	if (_time != time) {
		_time = time;
		update();
	}
}

void Transport::setInfo(const QString &info) {
	if (_info != info) {
		_info = info;
		update();
	}
}

void Transport::setPlayEnabled(bool enabled) {
	_play->setDisabled(!enabled);
}

rpl::producer<> Transport::playClicks() const {
	return _play->clicks() | rpl::to_empty;
}

void Transport::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto left = _play->width() + Px(12);
	const auto &font = st::normalFont;
	const auto timeWidth = font->width(_time);
	p.setFont(font);
	p.setPen(st::windowFg);
	p.drawText(
		QRect(left, 0, width() - left, height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		_time);
	const auto available = width() - left - timeWidth - Px(16);
	if (!_info.isEmpty() && available > Px(40)) {
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(width() - available, 0, available, height()),
			Qt::AlignRight | Qt::AlignVCenter,
			font->elided(_info, available));
	}
}

class ProgressLine final : public Ui::RpWidget {
public:
	explicit ProgressLine(QWidget *parent)
	: RpWidget(parent) {
		resize(width(), Px(kProgressHeight));
	}

	void setValue(float64 value) {
		_value = std::clamp(value, 0., 1.);
		update();
	}
	[[nodiscard]] float64 value() const {
		return _value;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return Px(kProgressHeight);
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
		const auto filled = int(std::round(width() * _value));
		if (filled > 0) {
			p.setBrush(st::activeButtonBg);
			p.drawRoundedRect(
				QRect(0, 0, std::max(filled, height()), height()),
				radius,
				radius);
		}
	}

private:
	float64 _value = 0.;

};

struct ValueSlider {
	Fn<void(int)> set; // Moves the slider without calling changed().
	Fn<void(bool)> setDimmed; // Shown as not available.
};

// A slider with a title on the left and the current value on the right.
// started() is called before the first change of a drag.
ValueSlider AddValueSlider(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> title,
		int minimum,
		int maximum,
		int step,
		int current,
		Fn<QString(int)> format,
		Fn<void()> started,
		Fn<void(int)> changed) {
	const auto header = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));
	const auto name = Ui::CreateChild<Ui::FlatLabel>(
		header,
		std::move(title),
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

	const auto sections = std::max((maximum - minimum) / step, 1);
	const auto toValue = [=](float64 position) {
		const auto index = int(std::round(
			std::clamp(position, 0., 1.) * sections));
		return minimum + index * step;
	};
	const auto toPosition = [=](int now) {
		return std::clamp(
			(now - minimum) / float64(sections * step),
			0.,
			1.);
	};
	struct Last {
		int value = 0;
		bool dragging = false;
		bool dimmed = false;
	};
	const auto last = slider->lifetime().make_state<Last>(Last{ current });
	slider->setAlwaysDisplayMarker(true);
	slider->setValue(toPosition(current));
	slider->setAdjustCallback([=](float64 position) {
		return toPosition(toValue(position));
	});
	const auto update = [=](float64 position, bool finished) {
		const auto now = toValue(position);
		value->setText(format(now));
		if (last->value != now) {
			if (!std::exchange(last->dragging, true)) {
				started();
			}
			last->value = now;
			changed(now);
		}
		if (finished) {
			last->dragging = false;
		}
	};
	slider->setChangeProgressCallback([=](float64 position) {
		update(position, false);
	});
	slider->setChangeFinishedCallback([=](float64 position) {
		update(position, true);
	});
	return {
		.set = [=](int now) {
			last->value = now;
			slider->setValue(toPosition(now));
			value->setText(format(now));
		},
		.setDimmed = [=](bool dimmed) {
			if (std::exchange(last->dimmed, dimmed) == dimmed) {
				return;
			}
			const auto opacity = dimmed ? kDimmedOpacity : 1.;
			slider->setFadeOpacity(opacity);
			value->setOpacity(opacity);
		},
	};
}

// A project of an editor that was closed not by the user: by the passcode
// lock, by a chat opened from a notification, by a switch to another
// account. It is kept in the memory and offered when the editor is opened
// the next time.
struct Stashed {
	Project project;
	State saved; // What was exported already.
	std::vector<State> undo;
	std::vector<State> redo;
	crl::time position = 0;
	std::vector<std::shared_ptr<TempDownload>> keep;
};

// Everything the editor needs from a session, see MakeSessionHooks().
// Empty in the UI snapshots: sending does nothing then.
struct SessionHooks {
	// Not empty: "Send to <chat>" is offered before "Choose a chat...".
	QString chatName;

	// Where an exported file is written to be sent.
	QString sendFolder;

	// Where the result goes when the box of its export was closed.
	QString keepFolder;

	// The exported file goes to the usual send files box of the chat
	// the video came from (known) or of a chat chosen first.
	Fn<void(std::shared_ptr<SendFile> file, bool known)> send;

	// added() is called when the sticker was really added to a pack, it
	// may be long after the export box is gone.
	Fn<void(
		const QByteArray &webm,
		const QString &name,
		Fn<void()> added)> addSticker;

	Fn<void(Stashed &&project)> stash;
};

using RunExport = Fn<ExportResult(
	const Project &project,
	const ExportOptions &options,
	Fn<void(float64)> progress,
	VideoCore::Cancel cancel)>;

// An export runs off the main thread and outlives its box when the box is
// closed not by the user (see Stashed): the result is written to a file
// then. The user cancels it by the button or by Escape.
struct ExportJob {
	VideoCore::Cancel cancel;
	QString savePath; // For the result when the box is gone.
	std::vector<std::shared_ptr<TempDownload>> keep;

	// Set while the box is there, called on the main thread.
	Fn<void(float64)> progress;
	Fn<void(ExportResult &&)> done;
};

[[nodiscard]] std::map<int, std::shared_ptr<ExportJob>> &ExportJobs() {
	static auto result = std::map<int, std::shared_ptr<ExportJob>>();
	return result;
}

void FinishExportJob(int id, ExportResult &&result) {
	auto &jobs = ExportJobs();
	const auto i = jobs.find(id);
	if (i == end(jobs)) {
		return;
	}
	const auto job = std::move(i->second);
	jobs.erase(i);
	if (const auto onstack = job->done) {
		onstack(std::move(result));
		return;
	} else if (result.cancelled
		|| job->cancel->load()
		|| job->savePath.isEmpty()) {
		return;
	} else if (!result.ok) {
		LOG(("Oblivion Video Error: Export failed: %1").arg(result.error));
		ShowNotice(
			Notice::Export,
			tr::lng_oblivion_video_export_failed(tr::now));
		return;
	}
	const auto path = job->savePath;
	WriteAsync(path, result.content, [=](bool ok) {
		ShowNotice(Notice::Export, ok
			? tr::lng_oblivion_video_export_saved(
				tr::now,
				lt_path,
				QDir::toNativeSeparators(path))
			: tr::lng_oblivion_write_failed(tr::now));
	});
}

// Returns the id of the job.
[[nodiscard]] int StartExportJob(
		Project project,
		ExportOptions options,
		RunExport run,
		std::vector<std::shared_ptr<TempDownload>> keep,
		Fn<void(float64)> progress,
		Fn<void(ExportResult &&)> done) {
	static auto lastId = 0;
	const auto id = ++lastId;
	const auto cancel = std::make_shared<std::atomic<bool>>(false);
	const auto job = std::make_shared<ExportJob>();
	job->cancel = cancel;
	job->keep = std::move(keep);
	job->progress = std::move(progress);
	job->done = std::move(done);
	ExportJobs().emplace(id, job);

	const auto shown = std::make_shared<std::atomic<int>>(-1);
	const auto report = [=](float64 value) {
		const auto permille = int(std::clamp(value, 0., 1.) * 1000);
		if (shown->exchange(permille) / 5 == permille / 5) {
			return;
		}
		crl::on_main([=] {
			const auto &jobs = ExportJobs();
			const auto i = jobs.find(id);
			if (i != end(jobs)) {
				if (const auto onstack = i->second->progress) {
					onstack(value);
				}
			}
		});
	};
	crl::async([
		id,
		cancel,
		report,
		options,
		project = std::move(project),
		run = std::move(run)
	] {
		auto result = run
			? run(project, options, report, cancel)
			: VideoEdit::Export(project, options, report, cancel);
		crl::on_main([id, result = std::move(result)]() mutable {
			FinishExportJob(id, std::move(result));
		});
	});
	return id;
}

void CancelExportJob(int id) {
	const auto &jobs = ExportJobs();
	const auto i = jobs.find(id);
	if (i != end(jobs)) {
		i->second->cancel->store(true);
	}
}

// The box of the job is gone. The export goes on when there is a file
// for the result, otherwise nobody needs it.
void DetachExportJob(int id, const QString &savePath) {
	const auto &jobs = ExportJobs();
	const auto i = jobs.find(id);
	if (i == end(jobs)) {
		return;
	}
	const auto &job = i->second;
	job->progress = nullptr;
	job->done = nullptr;
	job->savePath = savePath;
	if (savePath.isEmpty()) {
		job->cancel->store(true);
	} else if (!job->cancel->load()) {
		ShowNotice(
			Notice::Export,
			tr::lng_oblivion_video_export_background(
				tr::now,
				lt_path,
				QDir::toNativeSeparators(savePath)));
	}
}

[[nodiscard]] bool HasExportJobs() {
	for (const auto &[id, job] : ExportJobs()) {
		if (!job->cancel->load()) {
			return true;
		}
	}
	return false;
}

void CancelExportJobs() {
	for (const auto &[id, job] : ExportJobs()) {
		job->cancel->store(true);
	}
}

struct ExportArgs {
	std::shared_ptr<Ui::Show> show;
	SessionHooks session;
	Project project;
	QString name; // Of the file, without an extension.
	Format format = Format::Mp4;

	// The videos downloaded for the project, see TempDownload.
	std::vector<std::shared_ptr<TempDownload>> keep;

	// The result was saved to a file, sent or added to a sticker pack.
	Fn<void()> exported;

	// VideoEdit::Export() when not set.
	RunExport run;

	// For the UI snapshots: starts right away, reports the progress shown.
	bool exportNow = false;
	Fn<void()> progressShown;
};

[[nodiscard]] QString FormatFilterName(Format format) {
	switch (format) {
	case Format::Gif: return tr::lng_oblivion_video_file_gif(tr::now);
	case Format::Sticker:
	case Format::Emoji: return tr::lng_oblivion_video_file_webm(tr::now);
	case Format::Mp4:
	case Format::GifVideo: break;
	}
	return tr::lng_oblivion_video_file_mp4(tr::now);
}

[[nodiscard]] QString FormatAbout(Format format) {
	switch (format) {
	case Format::Mp4: return tr::lng_oblivion_video_mp4_about(tr::now);
	case Format::GifVideo: return tr::lng_oblivion_video_gifv_about(tr::now);
	case Format::Gif: return tr::lng_oblivion_video_gif_about(tr::now);
	case Format::Sticker:
		return tr::lng_oblivion_video_sticker_about(tr::now);
	case Format::Emoji: return tr::lng_oblivion_video_emoji_about(tr::now);
	}
	return QString();
}

void ExportBox(not_null<Ui::GenericBox*> box, ExportArgs &&args) {
	struct State : base::has_weak_ptr {
		~State() {
			detach();
		}

		// The box is closing, see ExportJob.
		void detach() {
			if (const auto id = base::take(job)) {
				DetachExportJob(id, Core::Quitting() ? QString() : keepPath);
			}
		}

		std::shared_ptr<Ui::Show> show;
		SessionHooks session;
		Project project;
		QString name;
		std::vector<std::shared_ptr<TempDownload>> keep;
		Fn<void()> exported;
		RunExport run;
		Fn<void()> progressShown;

		Format format = Format::Mp4;
		int videoSide = 0; // The shorter side, 0 = the biggest.
		int gifVideoSide = 0;
		int gifSide = 0; // The longer side, 0 = the biggest.
		int gifFps = VideoCore::kGifMaxFps;

		int job = 0; // See StartExportJob().
		QString keepPath; // For the result if the box is closed.
		bool exporting = false;
		bool writing = false; // The result goes to a file.
		std::optional<ExportOptions> cachedOptions;
		ExportResult cached;
		std::shared_ptr<SendFile> cachedFile; // Written for sending.

		rpl::variable<QString> about;
		rpl::variable<QString> summary;
		rpl::variable<QString> status;
		Chips *sizes = nullptr;
		Ui::SlideWrap<Ui::VerticalLayout> *sizesWrap = nullptr;
		Ui::SlideWrap<Ui::VerticalLayout> *ratesWrap = nullptr;
		Ui::SlideWrap<Ui::VerticalLayout> *progressWrap = nullptr;
		ProgressLine *progress = nullptr;
		base::unique_qptr<Ui::PopupMenu> menu;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto weak = base::make_weak(state);
	state->show = std::move(args.show);
	state->session = std::move(args.session);
	state->project = std::move(args.project);
	state->name = args.name.isEmpty()
		? SafeFileName(tr::lng_oblivion_video_default_name(tr::now))
		: args.name;
	state->keep = std::move(args.keep);
	state->exported = std::move(args.exported);
	state->run = std::move(args.run);
	state->progressShown = std::move(args.progressShown);
	state->format = args.format;

	box->setTitle(tr::lng_oblivion_video_export_title());
	box->setWidth(BoxWidth(state->show, kExportBoxWidth));

	// A click outside closes all the boxes: the editor under this one too.
	box->setCloseByOutsideClick(false);

	// Escape cancels a running export as the button does, the box stays.
	box->setCloseByEscape(false);
	box->events(
	) | rpl::filter([](not_null<QEvent*> e) {
		return (e->type() == QEvent::KeyPress)
			&& (static_cast<QKeyEvent*>(e.get())->key() == Qt::Key_Escape);
	}) | rpl::on_next([=] {
		if (state->job) {
			CancelExportJob(state->job);
		} else {
			box->closeBox();
		}
	}, box->lifetime());

	const auto cropSize = [=] {
		const auto canvas = VideoEdit::CanvasSize(state->project);
		return VideoEdit::CropRect(canvas, state->project.state.crop).size();
	};
	const auto shorter = [](QSize size) {
		return std::min(size.width(), size.height());
	};
	const auto longer = [](QSize size) {
		return std::max(size.width(), size.height());
	};
	const auto isVideo = [](Format format) {
		return (format == Format::Mp4) || (format == Format::GifVideo);
	};
	const auto sideChoice = [=]() -> int& {
		return (state->format == Format::Mp4)
			? state->videoSide
			: (state->format == Format::GifVideo)
			? state->gifVideoSide
			: state->gifSide;
	};
	const auto currentOptions = [=] {
		auto result = ExportOptions{ .format = state->format };
		const auto side = sideChoice();
		if (isVideo(state->format)) {
			const auto crop = cropSize();
			if (side > 0 && shorter(crop) > 0) {
				result.maxSide = int(std::round(
					side * float64(longer(crop)) / shorter(crop)));
			}
		} else if (state->format == Format::Gif) {
			result.maxSide = side;
			result.maxFps = state->gifFps;
		}
		return result;
	};
	const auto sizeChips = [=] {
		auto result = std::vector<Chips::Chip>();
		const auto format = state->format;
		const auto top = VideoEdit::OutputSize(
			state->project,
			{ .format = format });
		if (isVideo(format)) {
			const auto biggest = shorter(top);
			result.push_back({ 0, QString::number(biggest) + 'p' });
			for (const auto side : { 1080, 720, 480, 360 }) {
				if (side < biggest) {
					result.push_back({ side, QString::number(side) + 'p' });
				}
			}
		} else if (format == Format::Gif) {
			const auto biggest = longer(top);
			const auto text = [](int side) {
				return tr::lng_oblivion_video_size_px(
					tr::now,
					lt_value,
					QString::number(side));
			};
			result.push_back({ 0, text(biggest) });
			for (const auto side : { 360, 240 }) {
				if (side < biggest) {
					result.push_back({ side, text(side) });
				}
			}
		}
		return result;
	};
	const auto resultDuration = [=] {
		const auto whole = VideoEdit::OutputDuration(state->project.state);
		return (state->format == Format::Sticker
			|| state->format == Format::Emoji)
			? std::min(whole, VideoCore::kStickerMaxDuration)
			: whole;
	};
	const auto refreshSummary = [=] {
		const auto options = currentOptions();
		auto size = VideoEdit::OutputSize(state->project, options);
		if (state->format == Format::Emoji) {
			size = QSize(VideoCore::kEmojiSide, VideoCore::kEmojiSide);
		}
		const auto bytes = VideoEdit::EstimateSize(state->project, options);
		state->summary = bytes
			? tr::lng_oblivion_video_summary_size(
				tr::now,
				lt_size,
				FormatSize(size),
				lt_duration,
				FormatTime(resultDuration()),
				lt_bytes,
				Ui::FormatSizeText(bytes))
			: tr::lng_oblivion_video_summary(
				tr::now,
				lt_size,
				FormatSize(size),
				lt_duration,
				FormatTime(resultDuration()));
	};

	const auto content = box->verticalLayout();
	Ui::AddSkip(content, st::boxLittleSkip);
	const auto formats = content->add(
		object_ptr<Chips>(content, std::vector<Chips::Chip>{
			{ int(Format::Mp4), tr::lng_oblivion_video_format_mp4(tr::now) },
			{
				int(Format::GifVideo),
				tr::lng_oblivion_video_format_gifv(tr::now),
			},
			{ int(Format::Gif), tr::lng_oblivion_video_format_gif(tr::now) },
			{
				int(Format::Sticker),
				tr::lng_oblivion_video_format_sticker(tr::now),
			},
			{
				int(Format::Emoji),
				tr::lng_oblivion_video_format_emoji(tr::now),
			},
		}, int(state->format)),
		st::boxRowPadding);
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			state->about.value(),
			st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));

	state->sizesWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	{
		const auto inner = state->sizesWrap->entity();
		inner->add(
			object_ptr<Ui::FlatLabel>(
				inner,
				tr::lng_oblivion_video_size(),
				st::defaultFlatLabel),
			st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));
		state->sizes = inner->add(
			object_ptr<Chips>(inner, std::vector<Chips::Chip>(), 0),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	}
	state->ratesWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	const auto rateText = [](int fps) {
		return tr::lng_oblivion_video_fps_value(
			tr::now,
			lt_value,
			QString::number(fps));
	};
	const auto rates = [&] {
		const auto inner = state->ratesWrap->entity();
		inner->add(
			object_ptr<Ui::FlatLabel>(
				inner,
				tr::lng_oblivion_video_fps(),
				st::defaultFlatLabel),
			st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));
		return inner->add(
			object_ptr<Chips>(inner, std::vector<Chips::Chip>{
				{ 15, rateText(15) },
				{ 10, rateText(10) },
			}, state->gifFps),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	}();
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			state->summary.value(),
			st::defaultFlatLabel),
		st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));

	state->progressWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	{
		const auto inner = state->progressWrap->entity();
		state->progress = inner->add(
			object_ptr<ProgressLine>(inner),
			st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));
		inner->add(
			object_ptr<Ui::FlatLabel>(
				inner,
				state->status.value(),
				st::boxDividerLabel),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	}
	state->progressWrap->hide(anim::type::instant);
	Ui::AddSkip(content, st::boxLittleSkip);

	const auto close = [=] {
		box->closeBox();
	};
	const auto tooLong = [=] {
		const auto format = state->format;
		const auto limit = (format == Format::Gif)
			? VideoEdit::kMaxGifDuration
			: isVideo(format)
			? VideoEdit::kMaxVideoDuration
			: crl::time(0);
		if (!limit
			|| VideoEdit::OutputDuration(state->project.state) <= limit) {
			return false;
		}
		state->show->showToast(tr::lng_oblivion_video_too_long(
			tr::now,
			lt_max,
			FormatMinutes(limit)));
		return true;
	};

	// Shared by the handlers below, they call each other.
	struct Actions {
		Fn<void()> refreshButtons;

		// savePath is where the user wants the result, if it is known.
		Fn<void(
			Fn<void(const ExportResult &)> done,
			const QString &savePath)> perform;
	};
	const auto actions = box->lifetime().make_state<Actions>();

	const auto fileName = [=] {
		return state->name + '.' + VideoEdit::FormatExtension(state->format);
	};
	const auto exported = [=] {
		if (const auto onstack = state->exported) {
			onstack();
		}
	};
	const auto setWriting = [=](bool writing) {
		state->writing = writing;
		state->status = writing
			? tr::lng_oblivion_video_writing(tr::now)
			: tr::lng_oblivion_video_export_done(
				tr::now,
				lt_bytes,
				Ui::FormatSizeText(state->cached.content.size()));
	};
	actions->perform = [=](
			Fn<void(const ExportResult &)> done,
			const QString &savePath) {
		if (state->exporting || state->writing) {
			return;
		}
		const auto options = currentOptions();
		if (state->cachedOptions == options) {
			done(state->cached);
			return;
		} else if (tooLong()) {
			return;
		}
		state->exporting = true;
		state->cachedOptions = std::nullopt;
		state->cached = ExportResult();
		state->cachedFile = nullptr;
		state->keepPath = !savePath.isEmpty()
			? savePath
			: state->session.keepFolder.isEmpty()
			? QString()
			: filedialogNextFilename(
				fileName(),
				QString(),
				state->session.keepFolder);
		state->progress->setValue(0.);
		state->status = tr::lng_oblivion_video_exporting(
			tr::now,
			lt_percent,
			FormatPercent(0.));
		state->progressWrap->show(anim::type::normal);

		const auto progress = [=](float64 value) {
			if (!state->exporting) {
				return;
			}
			state->progress->setValue(value);
			state->status = tr::lng_oblivion_video_exporting(
				tr::now,
				lt_percent,
				FormatPercent(value));
			if (state->progressShown) {
				state->progressShown();
			}
		};
		const auto finished = [=](ExportResult &&result) {
			state->job = 0;
			state->exporting = false;
			actions->refreshButtons();
			if (result.cancelled || !result.ok) {
				state->progressWrap->hide(anim::type::normal);
				if (!result.cancelled) {
					LOG(("Oblivion Video Error: Export failed: %1"
						).arg(result.error));
					state->show->showToast(
						tr::lng_oblivion_video_export_failed(tr::now));
				}
				return;
			}
			state->progress->setValue(1.);
			state->status = tr::lng_oblivion_video_export_done(
				tr::now,
				lt_bytes,
				Ui::FormatSizeText(result.content.size()));
			state->cachedOptions = options;
			state->cached = std::move(result);
			done(state->cached);
		};
		state->job = StartExportJob(
			state->project,
			options,
			state->run,
			state->keep,
			crl::guard(weak, progress),
			crl::guard(weak, finished));
		actions->refreshButtons();
	};
	const auto save = [=] {
		if (state->exporting || state->writing || tooLong()) {
			return;
		}
		const auto format = state->format;
		const auto filter = FileFilter(
			FormatFilterName(format),
			u"*."_q + VideoEdit::FormatExtension(format));
		FileDialog::GetWritePath(
			box.get(),
			tr::lng_oblivion_video_save_title(tr::now),
			filter,
			SuggestedPath(fileName()),
			crl::guard(weak, [=](QString &&chosen) {
				if (chosen.isEmpty() || state->format != format) {
					return;
				}
				const auto path = chosen;
				actions->perform([=](const ExportResult &result) {
					setWriting(true);
					WriteAsync(path, result.content, [=](bool ok) {
						const auto text = ok
							? tr::lng_oblivion_saved_to(
								tr::now,
								lt_path,
								QDir::toNativeSeparators(path))
							: tr::lng_oblivion_write_failed(tr::now);
						if (!weak.get()) {
							ShowNotice(Notice::Export, text);
							return;
						}
						setWriting(false);
						state->show->showToast(text);
						if (ok) {
							exported();
						}
					});
				}, path);
			}));
	};
	const auto sendTo = [=](bool known) {
		if (!state->session.send || state->session.sendFolder.isEmpty()) {
			return;
		}
		actions->perform([=](const ExportResult &result) {
			if (state->cachedFile) {
				state->session.send(state->cachedFile, known);
				return;
			}
			const auto path = TempFilePath(
				state->session.sendFolder,
				fileName());
			setWriting(true);
			WriteAsync(path, result.content, [=](bool ok) {
				// Removes the file when nobody takes it. The user may have
				// quit without waiting for it, see AskQuitWhileWriting().
				const auto file = std::make_shared<SendFile>(path);
				if (!weak.get() || Core::Quitting()) {
					return;
				}
				setWriting(false);
				if (!ok) {
					state->show->showToast(
						tr::lng_oblivion_write_failed(tr::now));
					return;
				}
				file->sentCallback = crl::guard(weak, exported);
				state->cachedFile = file;
				state->session.send(file, known);
			});
		}, QString());
	};
	const auto send = [=] {
		if (state->exporting || state->writing) {
			return;
		} else if (state->session.chatName.isEmpty()) {
			sendTo(false);
			return;
		}
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		state->menu->addAction(
			tr::lng_oblivion_video_send_to(
				tr::now,
				lt_chat,
				state->session.chatName),
			[=] { sendTo(true); },
			&st::menuIconSend);
		state->menu->addAction(
			tr::lng_oblivion_video_send_choose(tr::now),
			[=] { sendTo(false); },
			&st::menuIconShare);
		state->menu->popup(QCursor::pos());
	};
	const auto toPack = [=] {
		if (!state->session.addSticker) {
			return;
		}
		// The box of the packs can be closed without adding anything: the
		// project is exported only when the sticker is really in a pack,
		// until then it is still asked about or kept when the editor is
		// closed.
		actions->perform([=](const ExportResult &result) {
			state->session.addSticker(
				result.content,
				state->name,
				crl::guard(weak, exported));
		}, QString());
	};

	// The buttons are rebuilt, not inside a click of one of them.
	const auto later = [=](Fn<void()> callback) {
		return [=] {
			crl::on_main(weak, callback);
		};
	};
	actions->refreshButtons = [=] {
		// The settings can't be changed while exporting, clicks on them
		// are ignored: they are shown as not available.
		formats->setDimmed(state->exporting);
		state->sizes->setDimmed(state->exporting);
		rates->setDimmed(state->exporting);

		box->clearButtons();
		if (state->exporting) {
			box->addButton(tr::lng_cancel(), [=] {
				CancelExportJob(state->job);
			});
			return;
		}
		switch (state->format) {
		case Format::Sticker:
			box->addButton(tr::lng_oblivion_video_to_pack(), later(toPack));
			box->addButton(tr::lng_oblivion_video_save(), later(save));
			break;
		case Format::Emoji:
			box->addButton(tr::lng_oblivion_video_save(), later(save));
			break;
		case Format::Mp4:
		case Format::GifVideo:
		case Format::Gif:
			box->addButton(tr::lng_oblivion_video_send(), later(send));
			box->addButton(tr::lng_oblivion_video_save(), later(save));
			break;
		}
		box->addButton(tr::lng_close(), close);
	};
	const auto refreshFormat = [=](anim::type animated) {
		const auto format = state->format;
		state->about = FormatAbout(format);
		const auto chips = sizeChips();
		auto &choice = sideChoice();
		const auto known = ranges::contains(chips, choice, &Chips::Chip::id);
		if (!known) {
			choice = 0;
		}
		state->sizes->setChips(chips, choice);
		state->sizesWrap->toggle(chips.size() > 1, animated);
		state->ratesWrap->toggle(format == Format::Gif, animated);
		refreshSummary();
		actions->refreshButtons();
	};
	const auto forgetStatus = [=] {
		// What was exported with other settings is not "done" anymore.
		if (!state->exporting && state->cachedOptions != currentOptions()) {
			state->progressWrap->hide(anim::type::normal);
		}
	};

	formats->chosen() | rpl::on_next([=](int id) {
		if (state->exporting) {
			return;
		}
		state->format = Format(id);
		formats->setSelected(id);
		refreshFormat(anim::type::normal);
		forgetStatus();
	}, formats->lifetime());
	state->sizes->chosen() | rpl::on_next([=](int id) {
		if (state->exporting) {
			return;
		}
		sideChoice() = id;
		state->sizes->setSelected(id);
		refreshSummary();
		forgetStatus();
	}, state->sizes->lifetime());
	rates->chosen() | rpl::on_next([=](int id) {
		if (state->exporting) {
			return;
		}
		state->gifFps = id;
		rates->setSelected(id);
		refreshSummary();
		forgetStatus();
	}, rates->lifetime());

	box->boxClosing() | rpl::on_next([=] {
		state->detach();
	}, box->lifetime());

	refreshFormat(anim::type::instant);
	if (args.exportNow) {
		actions->perform([](const ExportResult &) {}, QString());
	}
}

enum class Phase : uchar {
	Downloading,
	Opening,
	Ready,
	Failed,
};

struct DownloadHandlers {
	Fn<void(float64)> progress;

	// Both empty when it failed. keep is set for a file that is removed
	// when the video is not needed anymore.
	Fn<void(
		QString path,
		QByteArray content,
		std::shared_ptr<TempDownload> keep)> done;
};

// Asked before the changes that were not exported are lost.
[[nodiscard]] object_ptr<Ui::GenericBox> MakeCloseConfirmBox(
		Fn<void(Fn<void()>)> confirmed) {
	return Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_video_close_sure(),
		.confirmed = std::move(confirmed),
		.confirmText = tr::lng_close(),
		.confirmStyle = &st::attentionBoxButton,
	});
}

class Editor;

// All the editors that are open, for VideoEditorPreventsQuit().
[[nodiscard]] std::vector<not_null<Editor*>> &Editors() {
	static auto result = std::vector<not_null<Editor*>>();
	return result;
}

struct EditorArgs {
	std::shared_ptr<Ui::Show> show;
	SessionHooks session;

	// The video to open.
	QString path;
	QByteArray content;
	QString name;

	// Gets the video when there is nothing to open yet (a video from
	// the chat). Its state lives in the box, the handlers may be called
	// right away.
	Fn<void(not_null<Ui::GenericBox*>, DownloadHandlers)> download;

	// VideoCore::ReadClipInfo() and RoundVideo::Reader when not set.
	ProbeMedia probe;
	OpenMedia open;

	// A kept project to go on with, instead of a video to open.
	std::optional<Stashed> stashed;

	// An initial state for the first video (used by the UI snapshots).
	std::optional<State> state;
	crl::time position = 0;
	Fn<void(not_null<Editor*>)> created;
};

class Editor final : public base::has_weak_ptr {
public:
	Editor(not_null<Ui::GenericBox*> box, EditorArgs &&args);
	~Editor();

	void setup();

	// Opened, with the frame and the thumbnails shown.
	[[nodiscard]] bool idle() const;

	// Is closing or was closed, only its box is still there.
	[[nodiscard]] bool closed() const {
		return _closed;
	}

	// Has changes that were not exported and would be lost.
	[[nodiscard]] bool preventsQuit() const;
	void quitConfirmed();

private:
	struct Runtime {
		Runtime(OpenMedia open, Source source, int serial)
		: media(std::move(open), std::move(source))
		, serial(serial) {
		}

		crl::object_on_queue<QueuedMedia> media;

		// Another video may take the place of a source nothing uses,
		// what was asked from the previous one is skipped by this.
		const int serial = 0;
		bool thumbnailsLoaded = false;
	};
	struct Located {
		int index = 0;
		crl::time position = 0; // In the source.
		crl::time start = 0; // Of the clip in the timeline.
	};

	void setupPreview(not_null<Ui::VerticalLayout*> container);
	void setupTimeline(not_null<Ui::VerticalLayout*> container);
	void setupFrame(not_null<Ui::VerticalLayout*> container);
	void setupSound(not_null<Ui::VerticalLayout*> container);

	void setPhase(Phase phase);
	void fail(const QString &text);
	void addSource(QString path, QByteArray content, QString name);
	void sourceProbed(Source source);
	void openStashed(Stashed &&stashed);
	[[nodiscard]] std::unique_ptr<Runtime> makeRuntime(const Source &source);
	[[nodiscard]] int unusedSource() const;
	[[nodiscard]] bool sameRuntime(int index, int serial) const;
	void requestThumbnails(int index);
	void storeThumbnail(int source, crl::time position, QImage image);
	void refineThumbnails();
	void requestFrame();

	[[nodiscard]] const std::vector<Clip> &clips() const {
		return _project.state.clips;
	}
	[[nodiscard]] bool ready() const {
		return (_phase == Phase::Ready) && !clips().empty();
	}
	[[nodiscard]] crl::time total() const;
	[[nodiscard]] crl::time clipStart(int index) const;
	[[nodiscard]] Located locate(crl::time position) const;
	[[nodiscard]] int frameSide() const;
	[[nodiscard]] int thumbnailSide() const;
	[[nodiscard]] bool canSplit() const;
	[[nodiscard]] bool hasAudio() const;
	[[nodiscard]] bool hasChanges() const;

	void requestClose();
	void closing();

	void pushUndo();
	void undo();
	void redo();
	void restore(State state);
	void changed();
	void canvasChanged();
	void refreshAll();
	void refreshTransport();

	void seek(crl::time position);
	void step(crl::time delta);
	void split();
	void removeClip();
	void moveClip(int delta);
	void rotate();
	void chooseAspect(Aspect aspect);
	void chooseFiles();
	void showExport();
	[[nodiscard]] bool handleKey(not_null<QKeyEvent*> e);

	void togglePlay();
	void playClip(int index, crl::time from);
	void playbackFrame(int generation, int index, crl::time at, QImage frame);
	void playbackFinished(int generation, int index, bool ok);
	void stopPlayback();

	const not_null<Ui::GenericBox*> _box;
	const std::shared_ptr<Ui::Show> _show;
	const SessionHooks _session;
	const ProbeMedia _probe;
	const OpenMedia _open;
	Fn<void(not_null<Ui::GenericBox*>, DownloadHandlers)> _download;
	QString _initialPath;
	QByteArray _initialContent;
	QString _initialName;
	std::optional<State> _initialState;
	crl::time _initialPosition = 0;
	std::optional<Stashed> _initialStashed;

	Project _project;
	std::vector<std::unique_ptr<Runtime>> _runtime;
	int _runtimeSerial = 0;
	std::vector<std::shared_ptr<TempDownload>> _keep;
	const std::shared_ptr<ThumbStore> _thumbs;
	State _saved; // As it was opened or exported the last time.
	std::vector<State> _undo;
	std::vector<State> _redo;
	bool _dragUndoPending = false;
	bool _discard = false; // The user has closed the editor.
	bool _quitConfirmed = false;
	bool _closed = false;
	base::weak_qptr<Ui::BoxContent> _closeConfirm;
	Phase _phase = Phase::Opening;
	int _opening = 0;
	crl::time _position = 0;
	int _selected = 0;
	QSize _canvas;

	const std::shared_ptr<std::atomic<int>> _frameRequest;
	int _frameShown = 0;
	const std::shared_ptr<std::atomic<int>> _thumbnailRequest;
	int _thumbnailsPending = 0;
	base::Timer _refineTimer;

	VideoCore::Cancel _playCancel;
	int _playGeneration = 0;
	bool _playing = false;

	Preview *_preview = nullptr;
	Transport *_transport = nullptr;
	Timeline *_timeline = nullptr;
	Ui::VerticalLayout *_controls = nullptr;
	ToolButton *_splitButton = nullptr;
	ToolButton *_deleteButton = nullptr;
	ToolButton *_leftButton = nullptr;
	ToolButton *_rightButton = nullptr;
	ToolButton *_rotateButton = nullptr;
	ToolButton *_undoButton = nullptr;
	ToolButton *_redoButton = nullptr;
	ToolButton *_addButton = nullptr;
	Chips *_aspects = nullptr;
	Fn<void(int)> _setSpeed;
	Fn<void(bool)> _dimSpeed;
	int _shownSpeed = 100;
	Ui::Checkbox *_mute = nullptr;

	rpl::lifetime _lifetime;

};

Editor::Editor(not_null<Ui::GenericBox*> box, EditorArgs &&args)
: _box(box)
, _show(std::move(args.show))
, _session(std::move(args.session))
, _probe(std::move(args.probe))
, _open(std::move(args.open))
, _download(std::move(args.download))
, _initialPath(std::move(args.path))
, _initialContent(std::move(args.content))
, _initialName(std::move(args.name))
, _initialState(std::move(args.state))
, _initialPosition(args.position)
, _initialStashed(std::move(args.stashed))
, _thumbs(std::make_shared<ThumbStore>())
, _frameRequest(std::make_shared<std::atomic<int>>(0))
, _thumbnailRequest(std::make_shared<std::atomic<int>>(0))
, _refineTimer([=] { refineThumbnails(); }) {
	Editors().push_back(this);
}

Editor::~Editor() {
	closing();

	auto &list = Editors();
	list.erase(
		ranges::remove(list, not_null<Editor*>(this)),
		end(list));

	// Whatever still waits in the queues of the sources is skipped.
	++*_frameRequest;
	++*_thumbnailRequest;
}

bool Editor::hasChanges() const {
	return ready() && (_project.state != _saved);
}

bool Editor::preventsQuit() const {
	return hasChanges() && !_discard && !_quitConfirmed;
}

void Editor::quitConfirmed() {
	_quitConfirmed = true;
}

// Cancel and Escape: the changes that were not exported are lost only
// when the user agrees.
void Editor::requestClose() {
	if (_closed || _closeConfirm) {
		return;
	} else if (_discard || !hasChanges()) {
		_discard = true;
		_box->closeBox();
		return;
	}
	stopPlayback();
	const auto weak = base::make_weak(this);
	_closeConfirm = _show->show(MakeCloseConfirmBox(crl::guard(weak, [=](
			Fn<void()> close) {
		_discard = true;
		const auto box = _box;
		close();
		box->closeBox();
	})));
	if (const auto confirm = _closeConfirm.get()) {
		// A click outside closes all the boxes, not only that one.
		confirm->setCloseByOutsideClick(false);
	}
}

// The box is closing or is destroyed without that. When it is not the
// user who closes it the project is kept, see Stashed.
void Editor::closing() {
	if (std::exchange(_closed, true)) {
		return;
	}
	stopPlayback();
	_refineTimer.cancel();
	if (_discard || !hasChanges() || !_session.stash || Core::Quitting()) {
		return;
	}
	_session.stash(Stashed{
		.project = _project,
		.saved = _saved,
		.undo = _undo,
		.redo = _redo,
		.position = _position,
		.keep = _keep,
	});
}

void Editor::setup() {
	const auto content = _box->verticalLayout();
	Ui::AddSkip(content, st::boxLittleSkip);
	setupPreview(content);
	_controls = content->add(object_ptr<Ui::VerticalLayout>(content));
	setupTimeline(_controls);
	setupFrame(_controls);
	setupSound(_controls);
	Ui::AddSkip(content, st::boxLittleSkip);

	_box->setFocusCallback([=] {
		_timeline->setFocus();
	});
	_box->boxClosing() | rpl::on_next([=] {
		closing();
	}, _lifetime);

	// Escape asks about the changes as the Cancel button does.
	_box->setCloseByEscape(false);
	_box->events(
	) | rpl::filter([](not_null<QEvent*> e) {
		return (e->type() == QEvent::KeyPress)
			&& (static_cast<QKeyEvent*>(e.get())->key() == Qt::Key_Escape);
	}) | rpl::on_next([=] {
		requestClose();
	}, _lifetime);

	refreshAll();
	if (auto stashed = base::take(_initialStashed)) {
		openStashed(std::move(*stashed));
		return;
	} else if (!_initialPath.isEmpty() || !_initialContent.isEmpty()) {
		setPhase(Phase::Opening);
		addSource(
			base::take(_initialPath),
			base::take(_initialContent),
			base::take(_initialName));
		return;
	} else if (!_download) {
		fail(tr::lng_oblivion_video_open_failed(tr::now));
		return;
	}

	// A video from the chat: download it first.
	setPhase(Phase::Downloading);
	const auto weak = base::make_weak(this);
	base::take(_download)(_box, {
		.progress = crl::guard(weak, [=](float64 progress) {
			if (_phase != Phase::Downloading) {
				return;
			}
			_preview->setProgress(progress);
			_preview->setStatus(tr::lng_oblivion_video_downloading(
				tr::now,
				lt_percent,
				FormatPercent(progress)));
		}),
		.done = crl::guard(weak, [=](
				QString path,
				QByteArray content,
				std::shared_ptr<TempDownload> keep) {
			if (_phase != Phase::Downloading) {
				return;
			} else if (path.isEmpty() && content.isEmpty()) {
				fail(tr::lng_oblivion_video_download_failed(tr::now));
				return;
			}
			if (keep) {
				_keep.push_back(std::move(keep));
			}
			setPhase(Phase::Opening);
			addSource(
				std::move(path),
				std::move(content),
				base::take(_initialName));
		}),
	});
}

void Editor::setupPreview(not_null<Ui::VerticalLayout*> container) {
	_preview = container->add(
		object_ptr<Preview>(container),
		st::boxRowPadding);
	_preview->clicks() | rpl::on_next([=] {
		_timeline->setFocus();
		togglePlay();
	}, _preview->lifetime());
	// A press that changes nothing leaves the undo and the redo as they
	// are: the state is remembered before the first change of a drag.
	_preview->cropStarts() | rpl::on_next([=] {
		stopPlayback();
		_dragUndoPending = true;
	}, _preview->lifetime());
	_preview->cropChanges() | rpl::on_next([=](QRectF crop) {
		if (std::exchange(_dragUndoPending, false)) {
			pushUndo();
		}
		_project.state.crop = crop;
		refreshAll();
	}, _preview->lifetime());

	_transport = container->add(
		object_ptr<Transport>(container),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	_transport->playClicks() | rpl::on_next([=] {
		_timeline->setFocus();
		togglePlay();
	}, _transport->lifetime());
}

void Editor::setupTimeline(not_null<Ui::VerticalLayout*> container) {
	// The clips are inside of the widget by kStripSide, see strip().
	const auto stripSide = Px(kStripSide);
	_timeline = container->add(
		object_ptr<Timeline>(container, _thumbs),
		(st::boxRowPadding
			+ QMargins(-stripSide, st::boxLittleSkip, -stripSide, 0)));
	_timeline->setKeyHandler([=](not_null<QKeyEvent*> e) {
		return handleKey(e);
	});
	_timeline->seeks() | rpl::on_next([=](crl::time position) {
		stopPlayback();
		seek(position);
	}, _timeline->lifetime());
	_timeline->trimStarts() | rpl::on_next([=] {
		stopPlayback();
		_dragUndoPending = true;
	}, _timeline->lifetime());
	_timeline->trims() | rpl::on_next([=](const Timeline::Trim &trim) {
		auto &list = _project.state.clips;
		if (trim.index < 0 || trim.index >= int(list.size())) {
			return;
		} else if (std::exchange(_dragUndoPending, false)) {
			pushUndo();
		}
		auto &clip = list[trim.index];
		clip.from = trim.from;
		clip.till = trim.till;

		// The playhead goes with the edge and shows what is under it.
		_selected = trim.index;
		_position = clipStart(trim.index)
			+ ((trim.edge == Timeline::Edge::End)
				? std::max(clip.length() - 1, crl::time(0))
				: crl::time(0));
		refreshAll();
		requestFrame();
	}, _timeline->lifetime());
	_timeline->trimFinishes() | rpl::on_next([=] {
		_dragUndoPending = false;
		changed();
	}, _timeline->lifetime());
	_timeline->widthValue() | rpl::skip(1) | rpl::on_next([=] {
		_refineTimer.callOnce(kRefineDelay);
	}, _timeline->lifetime());

	const auto tools = container->add(
		object_ptr<ToolBar>(container),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	const auto button = [&](
			Glyph glyph,
			const QString &text,
			Fn<void()> callback,
			int keep = 0,
			bool right = false) {
		const auto result = tools->add(glyph, text, keep, right);
		result->setClickedCallback([=] {
			_timeline->setFocus();
			callback();
		});
		return result.get();
	};
	_splitButton = button(
		Glyph::Split,
		tr::lng_oblivion_video_split(tr::now),
		[=] { split(); },
		3);
	_deleteButton = button(
		Glyph::Delete,
		tr::lng_oblivion_video_delete(tr::now),
		[=] { removeClip(); },
		2);
	_leftButton = button(Glyph::MoveLeft, QString(), [=] { moveClip(-1); });
	_leftButton->setTooltip(tr::lng_oblivion_video_move_left(tr::now));
	_rightButton = button(Glyph::MoveRight, QString(), [=] { moveClip(1); });
	_rightButton->setTooltip(tr::lng_oblivion_video_move_right(tr::now));
	_rotateButton = button(
		Glyph::Rotate,
		tr::lng_oblivion_video_rotate(tr::now),
		[=] { rotate(); },
		1);
	_undoButton = button(Glyph::Undo, QString(), [=] { undo(); }, 0, true);
	_undoButton->setTooltip(tr::lng_oblivion_video_undo(tr::now));
	_redoButton = button(Glyph::Redo, QString(), [=] { redo(); }, 0, true);
	_redoButton->setTooltip(tr::lng_oblivion_video_redo(tr::now));
	_addButton = button(
		Glyph::Add,
		tr::lng_oblivion_video_add(tr::now),
		[=] { chooseFiles(); },
		4,
		true);

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_oblivion_video_hint(),
			st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
}

void Editor::setupFrame(not_null<Ui::VerticalLayout*> container) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_oblivion_video_frame(),
			st::defaultFlatLabel),
		st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));
	_aspects = container->add(
		object_ptr<Chips>(container, std::vector<Chips::Chip>{
			{
				int(Aspect::Original),
				tr::lng_oblivion_video_aspect_original(tr::now),
			},
			{
				int(Aspect::Free),
				tr::lng_oblivion_video_aspect_free(tr::now),
			},
			{ int(Aspect::Square), u"1:1"_q },
			{ int(Aspect::Portrait), u"4:5"_q },
			{ int(Aspect::Story), u"9:16"_q },
			{ int(Aspect::Wide), u"16:9"_q },
		}, int(Aspect::Original)),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	_aspects->chosen() | rpl::on_next([=](int id) {
		_timeline->setFocus();
		chooseAspect(Aspect(id));
	}, _aspects->lifetime());
}

void Editor::setupSound(not_null<Ui::VerticalLayout*> container) {
	auto slider = AddValueSlider(
		container,
		tr::lng_oblivion_video_speed(),
		VideoEdit::kMinSpeed,
		VideoEdit::kMaxSpeed,
		VideoEdit::kSpeedStep,
		_project.state.speed,
		FormatSpeed,
		[=] {
			stopPlayback();
			pushUndo();
		},
		[=](int speed) {
			_project.state.speed = speed;
			_shownSpeed = speed;
			refreshAll();
		});
	_setSpeed = std::move(slider.set);
	_dimSpeed = std::move(slider.setDimmed);
	_mute = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			tr::lng_oblivion_video_mute(tr::now),
			false,
			st::defaultBoxCheckbox),
		st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));
	_mute->checkedChanges() | rpl::on_next([=](bool checked) {
		if (!ready() || !hasAudio() || _project.state.mute == checked) {
			return;
		}
		pushUndo();
		_project.state.mute = checked;
		refreshAll();
	}, _mute->lifetime());
}

void Editor::setPhase(Phase phase) {
	_phase = phase;
	const auto editable = (phase == Phase::Ready);
	_preview->setInteractive(editable);
	_timeline->setInteractive(editable);
	_controls->setAttribute(Qt::WA_TransparentForMouseEvents, !editable);
	if (phase == Phase::Opening) {
		_preview->setProgress(std::nullopt);
		_preview->setStatus(tr::lng_oblivion_video_opening(tr::now));
	} else if (phase == Phase::Ready) {
		_preview->setProgress(std::nullopt);
		_preview->setStatus(QString());
	}
	_box->clearButtons();
	const auto close = [=] {
		requestClose();
	};
	if (editable) {
		_box->addButton(tr::lng_oblivion_video_export(), [=] {
			showExport();
		});
		_box->addButton(tr::lng_cancel(), close);
	} else if (phase == Phase::Failed) {
		_box->addButton(tr::lng_close(), close);
	} else {
		_box->addButton(tr::lng_cancel(), close);
	}
	refreshAll();
}

void Editor::fail(const QString &text) {
	_preview->setProgress(std::nullopt);
	_preview->setStatus(text);
	setPhase(Phase::Failed);
}

void Editor::addSource(QString path, QByteArray content, QString name) {
	if (name.isEmpty() && !path.isEmpty()) {
		name = QFileInfo(path).fileName();
	}
	++_opening;
	if (_phase == Phase::Ready) {
		_box->showLoading(true);
	}
	const auto probe = _probe;
	const auto weak = base::make_weak(this);
	crl::async([=] {
		auto source = Source{
			.path = path,
			.content = content,
			.name = name,
		};
		source.info = probe
			? probe(source)
			: VideoCore::ReadClipInfo(path, content);
		crl::on_main(weak, [=, source = std::move(source)]() mutable {
			sourceProbed(std::move(source));
		});
	});
}

void Editor::sourceProbed(Source source) {
	if (!--_opening) {
		_box->showLoading(false);
	}
	const auto first = _project.sources.empty();
	if (!source.info.valid()
		|| source.info.duration < VideoEdit::kMinClipLength) {
		if (!first) {
			_show->showToast(tr::lng_oblivion_video_add_failed(
				tr::now,
				lt_name,
				source.name));
		} else if (!_opening) {
			fail(tr::lng_oblivion_video_open_failed(tr::now));
		}
		return;
	}

	// The same file added once more is one more clip of the same source.
	auto index = -1;
	if (!source.path.isEmpty()) {
		const auto i = ranges::find(
			_project.sources,
			source.path,
			&Source::path);
		if (i != end(_project.sources)) {
			index = int(i - begin(_project.sources));
		}
	}
	const auto added = (index < 0);
	const auto reused = added ? unusedSource() : -1;
	if (int(clips().size()) >= kMaxClips) {
		_show->showToast(tr::lng_oblivion_video_too_many(
			tr::now,
			lt_max,
			QString::number(kMaxClips)));
		return;
	} else if (added
		&& (reused < 0)
		&& int(_project.sources.size()) >= kMaxSources) {
		_show->showToast(tr::lng_oblivion_video_too_many_files(
			tr::now,
			lt_max,
			QString::number(kMaxSources)));
		return;
	}
	if (!first) {
		stopPlayback();
		pushUndo();
	}
	if (reused >= 0) {
		index = reused;
		_runtime[index] = makeRuntime(source);
		_thumbs->sources[index].clear();
		_project.sources[index] = std::move(source);
	} else if (added) {
		index = int(_project.sources.size());
		_runtime.push_back(makeRuntime(source));
		_thumbs->sources.emplace_back();
		_project.sources.push_back(std::move(source));
	}
	const auto duration = _project.sources[index].info.duration;
	_project.state.clips.push_back({ index, 0, duration });
	if (first) {
		if (auto state = base::take(_initialState)) {
			auto valid = !state->clips.empty();
			for (auto &clip : state->clips) {
				clip.from = std::clamp(clip.from, crl::time(0), duration);
				clip.till = std::clamp(clip.till, crl::time(0), duration);
				valid = valid && (clip.source == 0) && (clip.till > clip.from);
			}
			if (valid) {
				_project.state = std::move(*state);
			}
		}
		_saved = _project.state;
		_position = std::clamp(_initialPosition, crl::time(0), total());
		_selected = locate(_position).index;
		setPhase(Phase::Ready);
	} else {
		_selected = int(clips().size()) - 1;
		_position = clipStart(_selected);
	}
	changed();
	requestFrame();
	if (added) {
		requestThumbnails(index);
	}
}

// A project kept when the editor was closed: all its videos were opened
// already, the editor goes on from where it was.
void Editor::openStashed(Stashed &&stashed) {
	_project = std::move(stashed.project);
	_saved = std::move(stashed.saved);
	_undo = std::move(stashed.undo);
	_redo = std::move(stashed.redo);
	_keep = std::move(stashed.keep);
	if (clips().empty()) {
		fail(tr::lng_oblivion_video_open_failed(tr::now));
		return;
	}
	for (const auto &source : _project.sources) {
		_runtime.push_back(makeRuntime(source));
		_thumbs->sources.emplace_back();
	}
	_position = std::clamp(stashed.position, crl::time(0), total());
	_selected = locate(_position).index;
	setPhase(Phase::Ready);
	changed();
	requestFrame();
	const auto count = int(_runtime.size());
	for (auto i = 0; i != count; ++i) {
		requestThumbnails(i);
	}
}

std::unique_ptr<Editor::Runtime> Editor::makeRuntime(const Source &source) {
	return std::make_unique<Runtime>(
		OpenMedia(_open),
		Source(source),
		++_runtimeSerial);
}

// A source that no clip uses, now or in the states the undo goes back to
// (what the redo keeps is dropped by the next change): the next added
// video takes its place. -1 if all of them are used.
int Editor::unusedSource() const {
	return VideoEdit::UnusedSource(_project, _undo);
}

bool Editor::sameRuntime(int index, int serial) const {
	return (index >= 0)
		&& (index < int(_runtime.size()))
		&& (_runtime[index]->serial == serial);
}

void Editor::requestThumbnails(int index) {
	const auto side = thumbnailSide();
	const auto serial = _runtime[index]->serial;
	const auto weak = base::make_weak(this);
	_runtime[index]->media.with([=](QueuedMedia &media) {
		auto list = media.get().thumbnails(kCoarseThumbnails, side);
		crl::on_main(weak, [=, list = std::move(list)]() mutable {
			if (!sameRuntime(index, serial)) {
				return;
			}
			_runtime[index]->thumbnailsLoaded = true;
			const auto duration = _project.sources[index].info.duration;
			const auto count = int(list.size());
			auto &map = _thumbs->sources[index];
			for (auto i = 0; i != count; ++i) {
				if (!list[i].isNull()) {
					// Where Reader::thumbnails() takes them from.
					const auto at = (duration * (2 * i + 1)) / (2 * count);
					map.emplace(at, std::move(list[i]));
				}
			}
			_timeline->update();
			_refineTimer.callOnce(kRefineDelay);
		});
	});
}

void Editor::storeThumbnail(int source, crl::time position, QImage image) {
	if (source >= int(_thumbs->sources.size()) || image.isNull()) {
		return;
	}
	auto &map = _thumbs->sources[source];
	if (int(map.size()) >= kMaxThumbnails) {
		map.clear();
	}
	map[position] = std::move(image);
	_timeline->update();
}

// The thumbnails from all over the video are too far from each other for
// a short clip: exact frames are read for the tiles that need them.
void Editor::refineThumbnails() {
	if (!ready() || _timeline->dragging()) {
		return;
	}
	const auto tiles = _timeline->missingTiles(kRefineLimit);
	if (tiles.empty()) {
		return;
	}
	const auto latest = _thumbnailRequest;
	const auto id = ++*latest;
	const auto side = thumbnailSide();
	const auto weak = base::make_weak(this);
	for (const auto &tile : tiles) {
		if (tile.source >= int(_runtime.size())
			|| !_runtime[tile.source]->thumbnailsLoaded) {
			continue;
		}
		++_thumbnailsPending;
		const auto serial = _runtime[tile.source]->serial;
		_runtime[tile.source]->media.with([=](QueuedMedia &media) {
			auto image = (*latest == id)
				? SquareThumbnail(
					media.get().frame(tile.position, side * 2),
					side)
				: QImage();
			crl::on_main(weak, [=, image = std::move(image)]() mutable {
				--_thumbnailsPending;
				if (sameRuntime(tile.source, serial)) {
					storeThumbnail(
						tile.source,
						tile.position,
						std::move(image));
				}
			});
		});
	}
}

void Editor::requestFrame() {
	if (clips().empty()) {
		return;
	}
	const auto located = locate(_position);
	const auto &clip = clips()[located.index];
	const auto position = std::clamp(
		located.position,
		clip.from,
		std::max(clip.till - 1, clip.from));
	if (_thumbnailsPending) {
		// The frame is more important than the thumbnails that wait in
		// the same queue: they are skipped and asked again later.
		++*_thumbnailRequest;
		_refineTimer.callOnce(kRefineDelay);
	}
	const auto latest = _frameRequest;
	const auto id = ++*latest;
	const auto side = frameSide();
	const auto weak = base::make_weak(this);
	_runtime[clip.source]->media.with([=](QueuedMedia &media) {
		if (*latest != id) {
			return;
		}
		auto frame = media.get().frame(position, side);
		crl::on_main(weak, [=, frame = std::move(frame)]() mutable {
			if (*latest != id) {
				return;
			}
			_frameShown = id;
			if (!frame.isNull()) {
				_preview->setFrame(std::move(frame));
			}
		});
	});
}

bool Editor::idle() const {
	if (_phase == Phase::Failed) {
		return true;
	} else if (!ready()
		|| _opening
		|| _thumbnailsPending
		|| _refineTimer.isActive()
		|| !_preview->hasFrame()
		|| _frameShown != _frameRequest->load()) {
		return false;
	}
	for (const auto &runtime : _runtime) {
		if (!runtime->thumbnailsLoaded) {
			return false;
		}
	}
	return true;
}

crl::time Editor::total() const {
	return VideoEdit::SourceDuration(_project.state);
}

crl::time Editor::clipStart(int index) const {
	auto result = crl::time(0);
	const auto count = std::min(index, int(clips().size()));
	for (auto i = 0; i < count; ++i) {
		result += clips()[i].length();
	}
	return result;
}

Editor::Located Editor::locate(crl::time position) const {
	auto start = crl::time(0);
	const auto count = int(clips().size());
	for (auto i = 0; i != count; ++i) {
		const auto &clip = clips()[i];
		const auto length = clip.length();
		if (position < start + length || i + 1 == count) {
			return {
				.index = i,
				.position = clip.from
					+ std::clamp(position - start, crl::time(0), length),
				.start = start,
			};
		}
		start += length;
	}
	return {};
}

int Editor::frameSide() const {
	return Px(kBoxWidth) * style::DevicePixelRatio();
}

int Editor::thumbnailSide() const {
	return (Px(kTimelineHeight) - Px(kStripTop) - Px(kStripBottom))
		* style::DevicePixelRatio();
}

bool Editor::canSplit() const {
	if (!ready() || int(clips().size()) >= kMaxClips) {
		return false;
	}
	const auto located = locate(_position);
	const auto &clip = clips()[located.index];
	return (located.position - clip.from >= VideoEdit::kMinClipLength)
		&& (clip.till - located.position >= VideoEdit::kMinClipLength);
}

bool Editor::hasAudio() const {
	for (const auto &clip : clips()) {
		if (_project.sources[clip.source].info.hasAudio) {
			return true;
		}
	}
	return false;
}

void Editor::pushUndo() {
	if (_undo.empty() || _undo.back() != _project.state) {
		if (int(_undo.size()) >= kMaxUndo) {
			_undo.erase(begin(_undo));
		}
		_undo.push_back(_project.state);
	}
	_redo.clear();
}

void Editor::undo() {
	if (!ready() || _timeline->dragging()) {
		return;
	}
	while (!_undo.empty() && _undo.back() == _project.state) {
		_undo.pop_back();
	}
	if (_undo.empty()) {
		refreshAll();
		return;
	}
	stopPlayback();
	_redo.push_back(_project.state);
	auto state = std::move(_undo.back());
	_undo.pop_back();
	restore(std::move(state));
}

void Editor::redo() {
	if (!ready() || _redo.empty() || _timeline->dragging()) {
		return;
	}
	stopPlayback();
	_undo.push_back(_project.state);
	auto state = std::move(_redo.back());
	_redo.pop_back();
	restore(std::move(state));
}

void Editor::restore(State state) {
	_dragUndoPending = false;
	_project.state = std::move(state);
	_canvas = VideoEdit::CanvasSize(_project);
	_position = std::clamp(_position, crl::time(0), total());
	_selected = locate(_position).index;
	refreshAll();
	requestFrame();
	_refineTimer.callOnce(kRefineDelay);
}

// After the clips were changed.
void Editor::changed() {
	canvasChanged();
	_position = std::clamp(_position, crl::time(0), total());
	_selected = std::clamp(
		_selected,
		0,
		std::max(int(clips().size()) - 1, 0));
	refreshAll();
	_refineTimer.callOnce(kRefineDelay);
}

// The first clip gives the canvas: with another one there the crop keeps
// its place, but takes the proportions again.
void Editor::canvasChanged() {
	const auto canvas = VideoEdit::CanvasSize(_project);
	if (_canvas == canvas) {
		return;
	}
	const auto had = !_canvas.isEmpty();
	_canvas = canvas;
	if (had) {
		auto &state = _project.state;
		state.crop = VideoEdit::AspectCrop(canvas, state.aspect, state.crop);
	}
}

void Editor::refreshTransport() {
	const auto &state = _project.state;
	const auto speed = std::max(state.speed, 1);
	if (clips().empty()) {
		_transport->setTime(QString());
		_transport->setInfo(QString());
		return;
	}
	_transport->setTime(FormatTime(_position * 100 / speed)
		+ u" / "_q
		+ FormatTime(VideoEdit::OutputDuration(state)));

	// What an MP4 of the best quality is, as the export box says it: not
	// the crop in the pixels of the source.
	const auto size = FormatSize(
		VideoEdit::OutputSize(_project, { .format = Format::Mp4 }));
	_transport->setInfo((clips().size() > 1)
		? (tr::lng_oblivion_video_clip(
			tr::now,
			lt_index,
			QString::number(_selected + 1),
			lt_total,
			QString::number(clips().size()))
			+ u"  "_q
			+ QChar(0xB7)
			+ u"  "_q
			+ size)
		: size);
}

// Everything on the screen from the state, any count of times.
void Editor::refreshAll() {
	const auto &state = _project.state;
	const auto editable = ready();

	_preview->setCanvas(_canvas, state.rotation);
	_preview->setCrop(state.crop, state.aspect);

	auto items = std::vector<Timeline::Item>();
	items.reserve(clips().size());
	for (const auto &clip : clips()) {
		items.push_back({
			.source = clip.source,
			.from = clip.from,
			.till = clip.till,
			.duration = _project.sources[clip.source].info.duration,
		});
	}
	_timeline->setItems(std::move(items), _selected);
	_timeline->setPosition(_position);
	_timeline->setRotation(state.rotation);

	_transport->setPlayEnabled(editable);
	_transport->setPlaying(_playing);
	refreshTransport();

	const auto count = int(clips().size());
	_splitButton->setDisabled(!canSplit());
	_deleteButton->setDisabled(!editable || count < 2);
	_leftButton->setDisabled(!editable || _selected <= 0);
	_rightButton->setDisabled(!editable || _selected + 1 >= count);
	_rotateButton->setDisabled(!editable);
	_undoButton->setDisabled(!editable || _undo.empty());
	_redoButton->setDisabled(!editable || _redo.empty());

	// A video that is here already can be added once more, so the count
	// of the videos is checked when one is chosen.
	_addButton->setDisabled(!editable || count >= kMaxClips);

	// Dimmed like the tool buttons while there is nothing to edit.
	_aspects->setDimmed(!editable);
	_dimSpeed(!editable);
	_aspects->setSelected(int(state.aspect));
	if (_shownSpeed != state.speed) {
		// Not while the slider itself is changing the speed.
		_shownSpeed = state.speed;
		_setSpeed(state.speed);
	}
	const auto audio = hasAudio();
	_mute->setDisabled(!editable || !audio);
	_mute->setChecked(
		(editable && !audio) || state.mute,
		Ui::Checkbox::NotifyAboutChange::DontNotify);
}

void Editor::seek(crl::time position) {
	if (!ready()) {
		return;
	}
	position = std::clamp(position, crl::time(0), total());
	const auto index = locate(position).index;
	if (_position == position && _selected == index) {
		return;
	}
	_position = position;
	_selected = index;
	refreshAll();
	requestFrame();
}

void Editor::step(crl::time delta) {
	stopPlayback();
	seek(_position + delta * std::max(_project.state.speed, 1) / 100);
}

void Editor::split() {
	if (!canSplit()) {
		return;
	}
	stopPlayback();
	pushUndo();
	const auto located = locate(_position);
	auto &list = _project.state.clips;
	auto second = list[located.index];
	second.from = located.position;
	list[located.index].till = located.position;
	list.insert(begin(list) + located.index + 1, second);

	// The playhead is at the start of the second part.
	_selected = located.index + 1;
	changed();
}

void Editor::removeClip() {
	if (!ready() || clips().size() < 2) {
		return;
	}
	stopPlayback();
	pushUndo();
	auto &list = _project.state.clips;
	list.erase(begin(list) + _selected);
	_selected = std::min(_selected, int(list.size()) - 1);
	_position = clipStart(_selected);
	changed();
	requestFrame();
}

void Editor::moveClip(int delta) {
	const auto target = _selected + delta;
	if (!ready() || target < 0 || target >= int(clips().size())) {
		return;
	}
	stopPlayback();
	pushUndo();
	const auto inside = _position - clipStart(_selected);
	auto &list = _project.state.clips;
	std::swap(list[_selected], list[target]);
	_selected = target;
	_position = clipStart(target)
		+ std::clamp(inside, crl::time(0), list[target].length() - 1);
	changed();
	requestFrame();
}

void Editor::rotate() {
	if (!ready()) {
		return;
	}
	stopPlayback();
	pushUndo();
	auto &state = _project.state;
	state.rotation = (state.rotation + 90) % 360;
	_canvas = VideoEdit::CanvasSize(_project);
	state.crop = VideoEdit::AspectCrop(
		_canvas,
		state.aspect,
		VideoEdit::RotatedCrop(state.crop));
	refreshAll();
}

void Editor::chooseAspect(Aspect aspect) {
	auto &state = _project.state;
	if (!ready()) {
		return;
	}
	const auto crop = VideoEdit::AspectCrop(_canvas, aspect, state.crop);
	if (state.aspect == aspect && state.crop == crop) {
		return;
	}
	stopPlayback();
	pushUndo();
	state.aspect = aspect;
	state.crop = crop;
	refreshAll();
}

void Editor::chooseFiles() {
	if (!ready()) {
		return;
	}
	stopPlayback();
	const auto weak = base::make_weak(this);
	FileDialog::GetOpenPaths(
		_box.get(),
		tr::lng_oblivion_video_choose(tr::now),
		OpenFilter(),
		crl::guard(weak, [=](FileDialog::OpenResult &&result) {
			for (const auto &path : result.paths) {
				addSource(path, QByteArray(), QString());
			}
			if (result.paths.isEmpty() && !result.remoteContent.isEmpty()) {
				addSource(QString(), result.remoteContent, QString());
			}
		}));
}

void Editor::showExport() {
	if (!ready()) {
		return;
	}
	stopPlayback();
	const auto weak = base::make_weak(this);
	_show->showBox(Box(ExportBox, ExportArgs{
		.show = _show,
		.session = _session,
		.project = _project,
		.name = ExportName(_project.sources.front().name),
		.keep = _keep,
		.exported = crl::guard(weak, [=, state = _project.state] {
			_saved = state;
		}),
	}));
}

bool Editor::handleKey(not_null<QKeyEvent*> e) {
	if (!ready() || _timeline->dragging()) {
		return false;
	}
	const auto key = e->key();
	const auto modifiers = e->modifiers();
	const auto shift = (modifiers & Qt::ShiftModifier);
	if (modifiers & Qt::ControlModifier) {
		if (key == Qt::Key_Z) {
			if (shift) {
				redo();
			} else {
				undo();
			}
			return true;
		} else if (key == Qt::Key_Y) {
			redo();
			return true;
		}
		return false;
	} else if (modifiers & (Qt::AltModifier | Qt::MetaModifier)) {
		return false;
	}
	switch (key) {
	case Qt::Key_Space:
		if (!e->isAutoRepeat()) {
			togglePlay();
		}
		return true;
	case Qt::Key_Left: step(shift ? -kSecondStep : -kFrameStep); return true;
	case Qt::Key_Right: step(shift ? kSecondStep : kFrameStep); return true;
	case Qt::Key_Home: stopPlayback(); seek(0); return true;
	case Qt::Key_End: stopPlayback(); seek(total()); return true;
	case Qt::Key_Delete:
	case Qt::Key_Backspace: removeClip(); return true;
	case Qt::Key_S: split(); return true;
	}

	// The same key in the Russian layout.
	if (e->text().compare(QString(QChar(0x44B)), Qt::CaseInsensitive) == 0) {
		split();
		return true;
	}
	return false;
}

void Editor::togglePlay() {
	if (!ready()) {
		return;
	} else if (_playing) {
		stopPlayback();
		return;
	}
	if (_position >= total() - kFrameStep) {
		_position = 0;
	}
	const auto located = locate(_position);
	_selected = located.index;
	_playing = true;

	// Frames asked before don't replace the ones played.
	_frameShown = ++*_frameRequest;
	refreshAll();
	playClip(located.index, located.position);
}

// The frames are read one after another and shown when their time comes.
// Without sound: the preview is for the picture and the cuts.
void Editor::playClip(int index, crl::time from) {
	const auto &clip = clips()[index];
	const auto &source = _project.sources[clip.source];
	const auto cancel = std::make_shared<std::atomic<bool>>(false);
	_playCancel = cancel;
	const auto generation = ++_playGeneration;
	const auto speed = std::max(_project.state.speed, 1) / 100.;
	const auto limit = std::min(
		Px(kPlaybackSide) * style::DevicePixelRatio(),
		2 * kPlaybackSide);
	const auto size = (std::max(
		source.info.size.width(),
		source.info.size.height()) > limit)
		? source.info.size.scaled(limit, limit, Qt::KeepAspectRatio)
		: source.info.size;
	const auto options = VideoCore::FrameOptions{
		.path = source.path,
		.content = source.content,
		.from = from,
		.till = clip.till,
		.size = size,
		.maxFps = std::max(int(kPlaybackMaxFps / speed), 1),
	};
	const auto length = clip.till - from;
	const auto weak = base::make_weak(this);
	crl::async([=] {
		const auto started = crl::now();
		auto late = crl::time(0);
		const auto wait = [&](crl::time position) {
			const auto due = started
				+ late
				+ crl::time(std::round(position / speed));
			while (!cancel->load()) {
				const auto now = crl::now();
				if (now >= due) {
					if (now - due > kPlaybackLate) {
						// Decoding is slower than the video plays.
						late += (now - due);
					}
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(
					std::min(due - now, crl::time(10))));
			}
			return false;
		};
		const auto ok = VideoCore::ReadFrames(options, [&](
				VideoCore::Frame &&frame) {
			if (!wait(frame.position)) {
				return false;
			}
			const auto at = from + frame.position;
			crl::on_main(weak, [=, image = std::move(frame.image)]() mutable {
				playbackFrame(generation, index, at, std::move(image));
			});
			return true;
		}, cancel, nullptr);
		const auto finished = ok && wait(length);
		crl::on_main(weak, [=] {
			playbackFinished(generation, index, finished);
		});
	});
}

void Editor::playbackFrame(
		int generation,
		int index,
		crl::time at,
		QImage frame) {
	if (!_playing
		|| generation != _playGeneration
		|| index >= int(clips().size())) {
		return;
	}
	const auto &clip = clips()[index];
	_selected = index;
	_position = clipStart(index)
		+ std::clamp(at - clip.from, crl::time(0), clip.length());
	_preview->setFrame(std::move(frame));
	_timeline->setSelected(_selected);
	_timeline->setPosition(_position);
	refreshTransport();
}

void Editor::playbackFinished(int generation, int index, bool ok) {
	if (!_playing || generation != _playGeneration) {
		return;
	} else if (ok && index + 1 < int(clips().size())) {
		playClip(index + 1, clips()[index + 1].from);
		return;
	}
	const auto finished = ok;
	stopPlayback();
	if (finished) {
		_position = total();
		_selected = locate(_position).index;
		refreshAll();
		requestFrame();
	}
}

void Editor::stopPlayback() {
	if (const auto cancel = base::take(_playCancel)) {
		cancel->store(true);
	}
	++_playGeneration;
	if (std::exchange(_playing, false) && _transport) {
		_transport->setPlaying(false);
		if (!_closed && ready()) {
			// The frames were played in a lower resolution.
			requestFrame();
		}
	}
}

void VideoEditorBox(not_null<Ui::GenericBox*> box, EditorArgs &&args) {
	box->setTitle(tr::lng_oblivion_tools_video_editor());
	box->setWidth(BoxWidth(args.show, kBoxWidth));
	box->setCloseByOutsideClick(false);
	const auto created = std::move(args.created);
	const auto editor = box->lifetime().make_state<Editor>(
		box,
		std::move(args));
	editor->setup();
	if (created) {
		created(editor);
	}
}

// The session side.

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

[[nodiscard]] QString ThreadName(not_null<Data::Thread*> thread) {
	if (const auto topic = thread->asTopic()) {
		return topic->title();
	}
	const auto peer = thread->peer();
	return peer->isSelf() ? tr::lng_saved_messages(tr::now) : peer->name();
}

// The thread whose compose area opened a SendFilesBox.
[[nodiscard]] Data::Thread *ResolveComposeThread(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	if (const auto active = window->activeChatCurrent().thread()) {
		if (active->peer() == peer && active->topicRootId() == topicRootId) {
			return active;
		}
	}
	if (topicRootId) {
		return peer->forumTopicFor(topicRootId);
	}
	return peer->owner().history(peer).get();
}

[[nodiscard]] bool CheckCanSendVideo(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<Data::Thread*> thread) {
	const auto peer = thread->peer();
	const auto allowed = Data::CanSendAnyOf(
		thread,
		(ChatRestriction::SendVideos
			| ChatRestriction::SendGifs
			| ChatRestriction::SendFiles));
	if (!allowed) {
		if (const auto error = Data::AnyFileRestrictionError(peer)) {
			Data::ShowSendErrorToast(show, peer, error);
		} else {
			show->showToast(tr::lng_oblivion_video_send_restricted(tr::now));
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
		Api::SendOptions options,
		Fn<void()> queued) {
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
			SendBundle(strong, target, bundle, copy, queued);
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

	// "Compress" on: a video or a GIF, off: a file.
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
	if (queued) {
		queued();
	}
	controller->uiShow()->showToast(tr::lng_oblivion_video_sent(
		tr::now,
		lt_chat,
		ThreadName(thread)));
}

// The usual send files box, so a caption, "compress" (a video or a file),
// the spoiler and the price work as for any attached video.
void ShowSendFilesBox(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		std::shared_ptr<SendFile> file) {
	const auto show = controller->uiShow();
	auto list = Storage::PrepareMediaList(
		QStringList{ file->path },
		st::sendMediaPreviewSize,
		controller->session().premium());
	if (list.error != Ui::PreparedList::Error::None || list.files.empty()) {
		show->showToast((list.error == Ui::PreparedList::Error::TooLargeFile)
			? tr::lng_oblivion_video_send_too_large(tr::now)
			: tr::lng_oblivion_write_failed(tr::now));
		return;
	}
	const auto peer = thread->peer();
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
			if (!strong || !target) {
				return;
			}
			// The file is read later, when it is uploaded: it stays.
			if (!std::exchange(file->sent, true)) {
				RememberSentFile(&strong->session(), file->path);
			}
			// A paid message waits for its price to be confirmed: only
			// what was really queued counts as exported.
			SendBundle(strong, target, std::move(bundle), options, [=] {
				if (const auto onstack = file->sentCallback) {
					onstack();
				}
			});
		},
		.replyTo = FullReplyTo{
			.topicRootId = thread->topicRootId(),
			.monoforumPeerId = thread->monoforumPeerId(),
		},
	}));
}

// The only project that is kept, see Stashed.
struct StashSlot {
	std::optional<Stashed> project;
	base::weak_ptr<Data::Thread> thread; // Where the video came from.
	int generation = 0;
};

[[nodiscard]] StashSlot &Stash() {
	static auto result = StashSlot();
	return result;
}

// Whether there is a project to go on with: all its videos are still here.
[[nodiscard]] bool HasStash() {
	auto &slot = Stash();
	if (!slot.project) {
		return false;
	}
	for (const auto &source : slot.project->project.sources) {
		if (source.content.isEmpty() && !QFileInfo::exists(source.path)) {
			slot.project = std::nullopt;
			return false;
		}
	}
	return true;
}

void KeepStash(
		not_null<Main::Session*> session,
		base::weak_ptr<Data::Thread> thread,
		Stashed &&project) {
	auto inMemory = int64(0);
	for (const auto &source : project.project.sources) {
		inMemory += source.content.size();
	}
	if (inMemory > kStashedContentLimit) {
		return;
	}
	auto &slot = Stash();
	slot.project = std::move(project);
	slot.thread = std::move(thread);
	const auto generation = ++slot.generation;

	// The videos downloaded for the project are in the temporary folder
	// of the account, it is removed when the account is logged out.
	session->lifetime().add([=] {
		auto &slot = Stash();
		if (slot.generation != generation || !slot.project) {
			return;
		}
		for (const auto &download : slot.project->keep) {
			download->forget();
		}
		slot.project = std::nullopt;

		// The toast about this project may still wait to be shown (the
		// editor was closed by the log out itself, or the app is locked).
		PendingNotices().project = QString();
	});
	ShowNotice(
		Notice::Project,
		tr::lng_oblivion_video_project_kept(tr::now));
}

// Where the files received by the user go, see DocumentData::save().
[[nodiscard]] QString DownloadsFolder(not_null<Main::Session*> session) {
	const auto path = Core::App().settings().downloadPath();
	return (path.isEmpty() || path == FileDialog::Tmp())
		? File::DefaultDownloadPath(session)
		: path;
}

[[nodiscard]] SessionHooks MakeSessionHooks(
		not_null<Window::SessionController*> controller,
		Data::Thread *thread) {
	if (thread && (&thread->session() != &controller->session())) {
		thread = nullptr;
	}
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	const auto weakSession = base::make_weak(session);
	const auto weakThread = thread
		? base::make_weak(thread)
		: base::weak_ptr<Data::Thread>();
	const auto sendTo = [=](
			not_null<Data::Thread*> target,
			std::shared_ptr<SendFile> file) {
		const auto strong = weak.get();
		if (strong && CheckCanSendVideo(strong->uiShow(), target)) {
			ShowSendFilesBox(strong, target, std::move(file));
		}
	};
	return {
		.chatName = thread ? ThreadName(thread) : QString(),
		.sendFolder = QDir(session->local().tempDirectory()).filePath(
			u"oblivion_video_send"_q),
		.keepFolder = DownloadsFolder(session),
		.send = [=](std::shared_ptr<SendFile> file, bool known) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto target = known ? weakThread.get() : nullptr;
			if (target) {
				sendTo(target, std::move(file));
				return;
			}
			const auto show = strong->uiShow();
			const auto chooser = Window::ShowChooseRecipientBox(
				strong,
				[=](not_null<Data::Thread*> thread) {
					if (!CheckCanSendVideo(show, thread)) {
						return false;
					}
					// After the chooser box is closed.
					const auto chosen = base::make_weak(thread);
					crl::on_main([=] {
						if (const auto target = chosen.get()) {
							sendTo(target, file);
						}
					});
					return true;
				},
				tr::lng_oblivion_video_send_title());
			if (const auto box = chooser.get()) {
				// A click outside closes all the boxes: the export box
				// and the editor under the list of the chats too.
				box->setCloseByOutsideClick(false);
			}
		},
		.addSticker = [=](
				const QByteArray &webm,
				const QString &name,
				Fn<void()> added) {
			if (const auto strong = weak.get()) {
				auto source = StickerSource::FromVideo(webm);
				source.name = name;
				AddToStickerPack(
					strong,
					std::move(source),
					std::move(added));
			}
		},
		.stash = [=](Stashed &&project) {
			if (const auto strong = weakSession.get()) {
				KeepStash(strong, weakThread, std::move(project));
			}
		},
	};
}

// A video from the chat is downloaded only to be edited: into the
// temporary folder, without the "save file" dialog, a copy left in the
// downloads folder or an entry in the downloads list.
[[nodiscard]] QString TempDownloadFolder(not_null<DocumentData*> document) {
	return QDir(document->session().local().tempDirectory()).filePath(
		u"oblivion_video/"_q + QString::number(document->id));
}

[[nodiscard]] QString DocumentName(not_null<DocumentData*> document) {
	const auto name = base::FileNameFromUserString(document->filename());
	return name.isEmpty() ? u"video.mp4"_q : name;
}

// The file is removed when the editor is closed and nothing else needs
// the video, see TempDownload.
void DownloadForEditor(
		not_null<Ui::GenericBox*> box,
		not_null<DocumentData*> document,
		FullMsgId context,
		DownloadHandlers handlers) {
	struct State {
		std::shared_ptr<Data::DocumentMedia> media;
		std::shared_ptr<TempDownload> temp; // We download the video there.
		rpl::lifetime lifetime;
	};
	const auto state = box->lifetime().make_state<State>();
	state->media = document->createMediaView();

	box->boxClosing() | rpl::on_next([=] {
		if (const auto temp = base::take(state->temp)) {
			if (document->loading()
				&& document->loadingFilePath() == temp->path()) {
				document->cancel();
			}
		}
	}, box->lifetime());

	// False until the file is loaded.
	const auto finish = [=](bool failed) {
		if (state->media->loaded(true)) {
			const auto path = document->filepath(true);

			// The file may be the one an export or a kept project of
			// a closed editor still uses.
			auto keep = state->temp ? state->temp : FindTempDownload(path);
#ifdef Q_OS_WIN
			if (!keep
				&& !path.isEmpty()
				&& (QFileInfo(path).dir()
					== QDir(TempDownloadFolder(document)))) {
				// The file of a closed editor that waits to be removed, see
				// RemoveTempLater(): now this editor is the one to remove it.
				keep = MakeTempDownload(document, path);
			}
#endif // Q_OS_WIN
			handlers.done(
				path,
				path.isEmpty() ? state->media->bytes() : QByteArray(),
				std::move(keep));
			return true;
		} else if (failed) {
			handlers.done(QString(), QByteArray(), nullptr);
		}
		return false;
	};
	if (finish(false)) {
		return;
	}
	handlers.progress(document->progress());
	document->session().data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> which) {
		return (which == document);
	}) | rpl::on_next([=] {
		if (document->loading()) {
			handlers.progress(document->progress());
		} else {
			finish(true);
		}
	}, state->lifetime);
	if (!document->loading()) {
		const auto folder = TempDownloadFolder(document);
		if (QDir().mkpath(folder)) {
			state->temp = MakeTempDownload(
				document,
				QDir(folder).filePath(DocumentName(document)));
			document->save(
				context ? Data::FileOrigin(context) : Data::FileOrigin(),
				state->temp->path());
		}
		if (!document->loading()) {
			// Nothing has started, no progress will come. The editor
			// ignores this when the progress above has already finished.
			finish(true);
		}
	}
}

void ShowStashedEditor(not_null<Window::SessionController*> controller) {
	auto &slot = Stash();
	auto project = base::take(slot.project);
	if (!project) {
		return;
	}
	++slot.generation;
	controller->show(Box(VideoEditorBox, EditorArgs{
		.show = controller->uiShow(),
		.session = MakeSessionHooks(controller, slot.thread.get()),
		.stashed = std::move(project),
	}));
}

// Offers to go on with the project kept from a closed editor.
[[nodiscard]] object_ptr<Ui::GenericBox> MakeRestoreBox(
		Fn<void(Fn<void()>)> restore,
		Fn<void(Fn<void()>)> fresh) {
	return Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_video_restore_text(),
		.confirmed = std::move(restore),
		.cancelled = std::move(fresh),
		.confirmText = tr::lng_oblivion_video_restore_continue(),
		.cancelText = tr::lng_oblivion_video_restore_new(),
		.strictCancel = true,
	});
}

// There is one place for a kept project (see StashSlot), so there is one
// editor at a time: two of them closed at once, by the passcode lock,
// would keep only one project while both are told to be kept. An editor
// that is closing already doesn't count, its box is only fading out.
[[nodiscard]] bool EditorIsOpen() {
	return ranges::any_of(Editors(), [](not_null<Editor*> editor) {
		return !editor->closed();
	});
}

[[nodiscard]] bool RefuseSecondEditor(
		not_null<Window::SessionController*> controller) {
	if (!EditorIsOpen()) {
		return false;
	}
	controller->showToast(tr::lng_oblivion_video_already_open(tr::now));
	return true;
}

// A project kept from a closed editor is offered first, fresh() opens what
// was asked when there is none or the user starts a new one.
void OpenStashedOr(
		not_null<Window::SessionController*> controller,
		Fn<void()> fresh) {
	if (RefuseSecondEditor(controller)) {
		return;
	} else if (!HasStash()) {
		fresh();
		return;
	}

	// An editor may be opened in another window while this one asks.
	const auto weak = base::make_weak(controller);
	const auto box = controller->show(MakeRestoreBox([=](
			Fn<void()> close) {
		const auto strong = weak.get();
		close();
		if (strong && !RefuseSecondEditor(strong)) {
			ShowStashedEditor(strong);
		}
	}, [=](Fn<void()> close) {
		const auto open = fresh;
		const auto strong = weak.get();
		close();
		if (strong && RefuseSecondEditor(strong)) {
			return;
		}
		Stash().project = std::nullopt;
		open();
	}));
	if (const auto raw = box.get()) {
		// A click outside closes all the boxes: the send files box under
		// this question as well, with its attachment and caption. Escape
		// closes only the question.
		raw->setCloseByOutsideClick(false);
	}
}

// beforeOpen() is called when the editor for this video is really opened:
// not when a kept project is continued instead of it, not when the question
// about that project is closed without an answer.
void ShowEditorBox(
		not_null<Window::SessionController*> controller,
		QString path,
		QByteArray content,
		QString name,
		DocumentData *document,
		FullMsgId context,
		Data::Thread *thread,
		Fn<void()> beforeOpen = nullptr) {
	const auto weak = base::make_weak(controller);
	const auto weakThread = thread
		? base::make_weak(thread)
		: base::weak_ptr<Data::Thread>();
	OpenStashedOr(controller, [=] {
		if (!weak.get()) {
			return;
		} else if (beforeOpen) {
			beforeOpen();
		}

		// The document lives as long as the session of the controller.
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto args = EditorArgs{
			.show = strong->uiShow(),
			.session = MakeSessionHooks(strong, weakThread.get()),
			.path = path,
			.content = content,
			.name = name,
		};
		if (document) {
			args.name = DocumentName(document);
			args.download = [=](
					not_null<Ui::GenericBox*> box,
					DownloadHandlers handlers) {
				DownloadForEditor(
					box,
					document,
					context,
					std::move(handlers));
			};
		}
		strong->show(Box(VideoEditorBox, std::move(args)));
	});
}

// UI snapshots (see oblivion_ui_snapshots.h): the editor with a painted
// video, nothing is read from the disk, exported or sent.

constexpr auto kSceneWidth = 780;
constexpr auto kExportSceneWidth = 560;
constexpr auto kSceneWait = crl::time(5000);
constexpr auto kSampleSize = QSize(1920, 1080);
constexpr auto kSampleDuration = crl::time(42'000);
constexpr auto kSampleProgress = 0.42;

[[nodiscard]] QColor MixColors(QColor a, QColor b, float64 ratio) {
	const auto r = std::clamp(ratio, 0., 1.);
	const auto mix = [&](int from, int to) {
		return int(std::round(from + (to - from) * r));
	};
	return QColor(
		mix(a.red(), b.red()),
		mix(a.green(), b.green()),
		mix(a.blue(), b.blue()));
}

// The sea from the morning (0) to the sunset (1): the sun crosses the sky
// and a boat crosses the frame, so every thumbnail is different.
void PaintSampleFrame(QPainter &p, QSizeF size, float64 time) {
	const auto w = size.width();
	const auto h = size.height();
	const auto unit = std::min(w, h);
	const auto day = std::sin(M_PI * time);
	const auto horizon = h * 0.62;
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);

	auto sky = QLinearGradient(0., 0., 0., horizon);
	sky.setColorAt(0., MixColors(
		QColor(0x2B, 0x2F, 0x77),
		QColor(0x2E, 0x8B, 0xE6),
		day));
	sky.setColorAt(1., MixColors(
		QColor(0xF7, 0x8C, 0x5A),
		QColor(0xBF, 0xE6, 0xFB),
		day));
	p.fillRect(QRectF(0., 0., w, horizon), sky);

	const auto sun = QPointF(
		w * (0.15 + 0.7 * time),
		horizon - h * (0.08 + 0.42 * day));
	const auto radius = unit * 0.075;
	auto glow = QRadialGradient(sun, radius * 3.6);
	glow.setColorAt(0., QColor(0xFF, 0xE9, 0xA8, 120));
	glow.setColorAt(1., QColor(0xFF, 0xE9, 0xA8, 0));
	p.setBrush(glow);
	p.drawEllipse(sun, radius * 3.6, radius * 3.6);
	p.setBrush(MixColors(
		QColor(0xFF, 0x9E, 0x4A),
		QColor(0xFF, 0xF5, 0xC8),
		day));
	p.drawEllipse(sun, radius, radius);

	p.setBrush(QColor(0xFF, 0xFF, 0xFF, 200));
	const auto cloud = [&](float64 x, float64 y, float64 scale) {
		const auto center = QPointF(w * x, h * y);
		const auto r = unit * 0.055 * scale;
		p.drawEllipse(center, r * 1.7, r * 0.85);
		p.drawEllipse(center + QPointF(r * 1.3, r * 0.2), r * 1.2, r * 0.7);
		p.drawEllipse(center + QPointF(-r * 1.3, r * 0.25), r * 1.1, r * 0.6);
	};
	cloud(0.1 + 0.35 * time, 0.16, 1.);
	cloud(0.66 + 0.2 * time, 0.27, 0.75);

	auto island = QPainterPath(QPointF(w * 0.58, horizon));
	island.cubicTo(
		QPointF(w * 0.66, horizon - h * 0.2),
		QPointF(w * 0.74, horizon - h * 0.06),
		QPointF(w * 0.8, horizon - h * 0.13));
	island.cubicTo(
		QPointF(w * 0.88, horizon - h * 0.2),
		QPointF(w * 0.94, horizon - h * 0.03),
		QPointF(w, horizon - h * 0.02));
	island.lineTo(w, horizon);
	island.closeSubpath();
	p.setBrush(MixColors(
		QColor(0x3A, 0x3F, 0x6B),
		QColor(0x4F, 0x8F, 0x6A),
		day));
	p.drawPath(island);

	auto sea = QLinearGradient(0., horizon, 0., h);
	sea.setColorAt(0., MixColors(
		QColor(0x7A, 0x5C, 0x8F),
		QColor(0x2C, 0x9C, 0xC9),
		day));
	sea.setColorAt(1., MixColors(
		QColor(0x1C, 0x22, 0x55),
		QColor(0x0E, 0x4F, 0x8C),
		day));
	p.fillRect(QRectF(0., horizon, w, h - horizon), sea);
	p.setBrush(QColor(0xFF, 0xF0, 0xC0, 70));
	for (auto i = 0; i != 5; ++i) {
		const auto y = horizon + (h - horizon) * (0.1 + 0.18 * i);
		const auto half = radius * (1.2 + 0.7 * i);
		p.drawRoundedRect(
			QRectF(sun.x() - half, y, 2 * half, unit * 0.012),
			unit * 0.006,
			unit * 0.006);
	}

	const auto boat = QPointF(w * (0.92 - 0.8 * time), horizon + h * 0.14);
	const auto s = unit * 0.11;
	auto hull = QPainterPath(boat + QPointF(-s, 0.));
	hull.lineTo(boat + QPointF(s, 0.));
	hull.lineTo(boat + QPointF(s * 0.7, s * 0.32));
	hull.lineTo(boat + QPointF(-s * 0.7, s * 0.32));
	hull.closeSubpath();
	p.setBrush(QColor(0x5A, 0x32, 0x1E));
	p.drawPath(hull);
	auto sail = QPainterPath(boat + QPointF(0., -s * 1.5));
	sail.lineTo(boat + QPointF(s * 0.75, -s * 0.12));
	sail.lineTo(boat + QPointF(0., -s * 0.12));
	sail.closeSubpath();
	p.setBrush(QColor(0xFF, 0xFF, 0xFF));
	p.drawPath(sail);
	auto jib = QPainterPath(boat + QPointF(-s * 0.08, -s * 1.2));
	jib.lineTo(boat + QPointF(-s * 0.6, -s * 0.12));
	jib.lineTo(boat + QPointF(-s * 0.08, -s * 0.12));
	jib.closeSubpath();
	p.setBrush(QColor(0xF2, 0x5C, 0x54));
	p.drawPath(jib);
}

[[nodiscard]] QImage PaintSample(QSize size, float64 time) {
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::black);
	{
		auto p = QPainter(&result);
		PaintSampleFrame(p, QSizeF(size), time);
	}
	return result;
}

// A synthetic video: frames are painted on request, always the same.
class SampleMedia final : public Media {
public:
	QImage frame(crl::time position, int maxSide) override {
		return PaintSample(
			kSampleSize.scaled(maxSide, maxSide, Qt::KeepAspectRatio),
			position / float64(kSampleDuration));
	}
	std::vector<QImage> thumbnails(int count, int side) override {
		auto result = std::vector<QImage>();
		for (auto i = 0; i < count; ++i) {
			const auto at = (kSampleDuration * (2 * i + 1)) / (2 * count);
			result.push_back(SquareThumbnail(frame(at, side * 2), side));
		}
		return result;
	}

};

[[nodiscard]] VideoCore::ClipInfo SampleInfo() {
	return {
		.duration = kSampleDuration,
		.size = kSampleSize,
		.fps = 30.,
		.hasAudio = true,
		.codec = u"h264"_q,
		.container = u"mov,mp4,m4a,3gp,3g2,mj2"_q,
	};
}

[[nodiscard]] QString SampleName() {
	return u"IMG_2048.MOV"_q;
}

[[nodiscard]] EditorArgs SampleEditorArgs(
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<base::weak_ptr<Editor>> handle,
		State state,
		crl::time position) {
	return {
		.show = std::move(show),
		.content = QByteArray("sample"),
		.name = SampleName(),
		.probe = [](const Source &) {
			return SampleInfo();
		},
		.open = [](const Source &) -> std::unique_ptr<Media> {
			return std::make_unique<SampleMedia>();
		},
		.state = std::move(state),
		.position = position,
		.created = [=](not_null<Editor*> editor) {
			*handle = base::make_weak(editor.get());
		},
	};
}

[[nodiscard]] ExportArgs SampleExportArgs(std::shared_ptr<Ui::Show> show) {
	return {
		.show = std::move(show),
		.session = { .chatName = tr::lng_saved_messages(tr::now) },
		.project = {
			.sources = { Source{
				.content = QByteArray("sample"),
				.name = SampleName(),
				.info = SampleInfo(),
			} },
			.state = {
				.clips = {
					{ 0, 3'000, 14'500 },
					{ 0, 22'000, 31'000 },
				},
				.speed = 125,
			},
		},
		.name = ExportName(SampleName()),
	};
}

[[nodiscard]] bool NothingSlides(not_null<QWidget*> box) {
	for (const auto child : box->findChildren<QWidget*>()) {
		if (const auto wrap = dynamic_cast<Ui::SlideWrap<>*>(child)) {
			if (wrap->animating()) {
				return false;
			}
		}
	}
	return true;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	// Shared by the create() and ready() callbacks of one scene, scenes
	// are rendered one by one.
	const auto handle = std::make_shared<base::weak_ptr<Editor>>();
	const auto idle = [=] {
		const auto strong = handle->get();
		return strong && strong->idle();
	};
	const auto editorScene = [=](
			QString name,
			State state,
			crl::time position) {
		RegisterScene({
			.name = std::move(name),
			.size = QSize(Px(kSceneWidth), 0),
			.box = [=](std::shared_ptr<Ui::Show> show) {
				return Box(
					VideoEditorBox,
					SampleEditorArgs(show, handle, state, position));
			},
			.ready = [=](not_null<QWidget*>) { return idle(); },
			.wait = kSceneWait,
		});
	};

	// Three clips cut from one video, the second one is chosen: the trim
	// handles, the playhead, "Clip 2 of 3".
	editorScene(u"video_editor_main"_q, {
		.clips = {
			{ 0, 2'000, 13'500 },
			{ 0, 16'000, 27'000 },
			{ 0, 33'500, 40'000 },
		},
	}, 16'200);

	// A 9:16 crop of a 16:9 video, faster and without sound.
	editorScene(u"video_editor_crop"_q, {
		.clips = { { 0, 4'000, 34'000 } },
		.crop = VideoEdit::AspectCrop(
			kSampleSize,
			Aspect::Story,
			QRectF(0.08, 0., 0.5, 1.)),
		.aspect = Aspect::Story,
		.speed = 150,
		.mute = true,
	}, 11'000);

	// Just opened, as the editor is seen first: the whole video is one
	// clip, the playhead is at the left edge of the timeline.
	editorScene(u"video_editor_opened"_q, {
		.clips = { { 0, 0, kSampleDuration } },
	}, 0);

	// A file that is not a video: nothing to edit, only "Close".
	RegisterScene({
		.name = u"video_editor_failed"_q,
		.size = QSize(Px(kSceneWidth), 0),
		.box = [=](std::shared_ptr<Ui::Show> show) {
			auto args = SampleEditorArgs(show, handle, State(), 0);
			args.probe = [](const Source &) {
				return VideoCore::ClipInfo();
			};
			return Box(VideoEditorBox, std::move(args));
		},
		.ready = [=](not_null<QWidget*>) { return idle(); },
		.wait = kSceneWait,
	});

	// A video from a chat while it is being downloaded: the progress in
	// place of the frame, the controls can't be used yet.
	RegisterScene({
		.name = u"video_editor_downloading"_q,
		.size = QSize(Px(kSceneWidth), 0),
		.box = [=](std::shared_ptr<Ui::Show> show) {
			auto args = SampleEditorArgs(show, handle, State(), 0);
			args.content = QByteArray();
			args.download = [](
					not_null<Ui::GenericBox*>,
					DownloadHandlers handlers) {
				handlers.progress(kSampleProgress);
			};
			return Box(VideoEditorBox, std::move(args));
		},
	});

	// The export settings of an MP4.
	RegisterScene({
		.name = u"video_editor_export"_q,
		.size = QSize(Px(kExportSceneWidth), 0),
		.box = [](std::shared_ptr<Ui::Show> show) {
			return Box(ExportBox, SampleExportArgs(std::move(show)));
		},
		.ready = [](not_null<QWidget*> widget) {
			return NothingSlides(widget);
		},
	});

	// A video sticker being exported: stays at the same progress until
	// the box is destroyed.
	const auto shown = std::make_shared<bool>(false);
	RegisterScene({
		.name = u"video_editor_export_progress"_q,
		.size = QSize(Px(kExportSceneWidth), 0),
		.box = [=](std::shared_ptr<Ui::Show> show) {
			*shown = false;
			auto args = SampleExportArgs(std::move(show));
			args.format = Format::Sticker;
			args.exportNow = true;
			args.progressShown = [=] {
				*shown = true;
			};
			args.run = [](
					const Project &,
					const ExportOptions &,
					Fn<void(float64)> progress,
					VideoCore::Cancel cancel) {
				progress(kSampleProgress);
				while (!cancel->load()) {
					std::this_thread::sleep_for(
						std::chrono::milliseconds(10));
				}
				return ExportResult{ .cancelled = true };
			};
			return Box(ExportBox, std::move(args));
		},
		.ready = [=](not_null<QWidget*> widget) {
			return *shown && NothingSlides(widget);
		},
	});

	// Cancel or Escape with changes that were not exported.
	RegisterBoxScene(
		u"video_editor_close_confirm"_q,
		QSize(Px(kExportSceneWidth), 0),
		[](std::shared_ptr<Ui::Show>) {
			return MakeCloseConfirmBox([](Fn<void()> close) {
				close();
			});
		});

	// The editor is opened while a project of a closed one is kept.
	RegisterBoxScene(
		u"video_editor_restore"_q,
		QSize(Px(kExportSceneWidth), 0),
		[](std::shared_ptr<Ui::Show>) {
			return MakeRestoreBox([](Fn<void()> close) {
				close();
			}, [](Fn<void()> close) {
				close();
			});
		});
});

} // namespace

void ShowVideoEditor(
		not_null<Window::SessionController*> controller,
		const QString &path,
		Data::Thread *thread) {
	ShowEditorBox(
		controller,
		path,
		QByteArray(),
		QString(),
		nullptr,
		FullMsgId(),
		thread);
}

void ShowVideoEditorContent(
		not_null<Window::SessionController*> controller,
		const QByteArray &content,
		Data::Thread *thread) {
	ShowEditorBox(
		controller,
		QString(),
		content,
		QString(),
		nullptr,
		FullMsgId(),
		thread);
}

void ShowVideoEditorForDocument(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		FullMsgId context) {
	const auto item = context
		? controller->session().data().message(context)
		: nullptr;
	ShowEditorBox(
		controller,
		QString(),
		QByteArray(),
		QString(),
		document,
		context,
		ItemThread(item));
}

void ShowVideoEditorImport(not_null<Window::SessionController*> controller) {
	OpenStashedOr(controller, crl::guard(controller, [=] {
		FileDialog::GetOpenPath(
			controller->widget().get(),
			tr::lng_oblivion_video_choose(tr::now),
			OpenFilter(),
			crl::guard(controller, [=](FileDialog::OpenResult &&result) {
				if (!result.paths.isEmpty()) {
					ShowVideoEditor(controller, result.paths.front());
				} else if (!result.remoteContent.isEmpty()) {
					ShowVideoEditorContent(controller, result.remoteContent);
				}
			}));
	}));
}

bool VideoEditorPreventsQuit() {
	const auto &writes = PendingWrites();
	if (writes.pending > 0 && !writes.forced) {
		// Asked at once and before the "hold to quit" check of the system
		// (it hides all the windows when it passes, nothing can be shown
		// after it). When the user quits anyway this is called again and
		// goes on with the checks below. Without a window to ask in the
		// quit is not held.
		if (const auto window = Core::App().activePrimaryWindow()) {
			AskQuitWhileWriting(window);
			return true;
		}
	}
	const auto exporting = HasExportJobs();
	const auto unsaved = HasStash() || ranges::any_of(Editors(), [](
			not_null<Editor*> editor) {
		return editor->preventsQuit();
	});
	const auto window = (exporting || unsaved)
		? Core::App().activePrimaryWindow()
		: nullptr;
	if (!window) {
		return false;
	}
	window->activate();

	static auto asked = base::weak_qptr<Ui::BoxContent>();
	if (asked) {
		return true;
	}
	const auto box = window->show(Ui::MakeConfirmBox({
		.text = (exporting
			? tr::lng_oblivion_video_quit_exporting()
			: tr::lng_oblivion_video_quit_unsaved()),
		.confirmed = [](Fn<void()> close) {
			close();
			CancelExportJobs();
			Stash().project = std::nullopt;
			for (const auto &editor : Editors()) {
				editor->quitConfirmed();
			}
			Core::Quit();
		},
		.confirmText = tr::lng_oblivion_video_quit(),
		.confirmStyle = &st::attentionBoxButton,
	}));
	asked = box;
	if (const auto raw = box.get()) {
		// A click outside closes all the boxes, the editor as well.
		raw->setCloseByOutsideClick(false);
	}
	return true;
}

void CleanupVideoEditorTemp(not_null<Main::Session*> session) {
	static auto legacy = false;
	if (!std::exchange(legacy, true)) {
		CleanupLegacyTemp();
	}

	// Nothing of this launch is there yet: what a previous one has left
	// (the videos downloaded to be edited, the files written to be sent)
	// is not needed, no upload goes on after a restart.
	const auto temp = QDir(session->local().tempDirectory());
	QDir(temp.filePath(u"oblivion_video"_q)).removeRecursively();
	QDir(temp.filePath(u"oblivion_video_send"_q)).removeRecursively();
}

void AddVideoEditorAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document) {
	const auto video = document->isVideoFile()
		|| document->isAnimation()
		|| document->isVideoMessage()
		|| document->mimeString().startsWith(
			u"video/"_q,
			Qt::CaseInsensitive);

	// The Downloads list shows items of all accounts, while the action
	// looks the message up again in the controller's session.
	if (!video
		|| document->sticker()
		|| (&item->history()->session() != &controller->session())
		|| item->isSending()
		|| item->hasFailed()) {
		return;
	}
	const auto context = item->fullId();
	menu->addAction(tr::lng_oblivion_video_open_in(tr::now), crl::guard(
		controller,
		[=] {
			const auto item = controller->session().data().message(context);
			const auto media = item ? item->media() : nullptr;
			if (media && media->document() == document) {
				ShowVideoEditorForDocument(controller, document, context);
			}
		}), &st::menuIconEdit);
}

void AddSendFilesVideoEditorAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<PeerData*> peer,
		const SendMenu::Details &details,
		Api::SendType sendType,
		const Ui::PreparedList &list,
		Fn<void()> closeBox) {
	if (list.files.size() != 1) {
		return;
	}
	const auto &file = list.files.front();
	if (file.type != Ui::PreparedFile::Type::Video
		|| (file.path.isEmpty() && file.content.isEmpty())) {
		return;
	} else if (EditorIsOpen()) {
		// One editor at a time, and this may be the box that sends what
		// the editor has exported.
		return;
	}
	const auto path = file.path;
	const auto content = path.isEmpty() ? file.content : QByteArray();

	// Only boxes opened from a chat's compose area know their chat.
	const auto fromChat = (sendType == Api::SendType::Normal)
		&& (details.barePeerId == peer->id.value);
	const auto topicRootId = MsgId(details.bareTopicRootId);
	menu->addAction(tr::lng_oblivion_video_open_in(tr::now), [=] {
		const auto window = show->resolveWindow();
		if (!window || (&window->session() != &peer->session())) {
			return;
		}
		const auto thread = fromChat
			? ResolveComposeThread(window, peer, topicRootId)
			: nullptr;

		// The send box is closed only when the editor is opened for this
		// video. While a kept project is offered the box stays, with the
		// attached video and its caption: the user may continue that
		// project or close the question instead.
		ShowEditorBox(
			window,
			path,
			content,
			QString(),
			nullptr,
			FullMsgId(),
			thread,
			closeBox);
	}, &st::menuIconEdit);
}

} // namespace Oblivion
