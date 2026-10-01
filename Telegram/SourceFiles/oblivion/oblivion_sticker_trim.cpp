/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_sticker_trim.h"

#include "base/base_file_utilities.h"
#include "base/call_delayed.h"
#include "base/event_filter.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_round_video_convert.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_video_core.h"
#include "storage/storage_account.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>
#include <crl/crl_object_on_queue.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QKeyEvent>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainterPath>

#include <chrono>
#include <thread>

namespace Oblivion {
namespace {

constexpr auto kBoxWidth = 480;
constexpr auto kPreviewHeight = 240; // Until the video is opened.
constexpr auto kPreviewMinHeight = 160;
constexpr auto kPreviewMaxHeight = 300;
constexpr auto kTrimHeight = 44;
constexpr auto kCornerRadius = 6;
constexpr auto kTrimHandleWidth = 12;
constexpr auto kTrimBorder = 2;
constexpr auto kThumbnails = 10;
constexpr auto kDimAlpha = 150;
constexpr auto kProgressSize = 56;
constexpr auto kMaxLength = VideoCore::kStickerMaxDuration;
constexpr auto kMinLength = crl::time(300);
constexpr auto kMinDuration = crl::time(100);
constexpr auto kStep = crl::time(100);
constexpr auto kBigStep = crl::time(1000);
constexpr auto kMaxZoom = 4.;

// What goes to the sticker: the whole frame or the biggest square of it,
// made zoom times smaller and placed with its center at (x, y), in parts
// of the frame, moved inside the frame where it doesn't fit.
struct Crop {
	float64 x = 0.5;
	float64 y = 0.5;
	float64 zoom = 1.;
	bool square = true;
};

[[nodiscard]] QRectF CropRect(QSizeF frame, const Crop &crop) {
	if (frame.width() <= 0. || frame.height() <= 0.) {
		return QRectF();
	}
	const auto zoom = std::clamp(crop.zoom, 1., kMaxZoom);
	const auto shorter = std::min(frame.width(), frame.height());
	const auto size = (crop.square ? QSizeF(shorter, shorter) : frame) / zoom;
	const auto left = std::clamp(
		crop.x * frame.width() - size.width() / 2.,
		0.,
		frame.width() - size.width());
	const auto top = std::clamp(
		crop.y * frame.height() - size.height() / 2.,
		0.,
		frame.height() - size.height());
	return QRectF(QPointF(left, top), size);
}

// Whole displayed source pixels for VideoCore::VideoStickerOptions::crop.
[[nodiscard]] QRect SourceCrop(QSize video, const Crop &crop) {
	const auto rect = CropRect(QSizeF(video), crop);
	auto width = std::max(int(std::round(rect.width())), 1);
	auto height = std::max(int(std::round(rect.height())), 1);
	if (crop.square) {
		width = height = std::min(width, height);
	}
	const auto left = std::clamp(
		int(std::round(rect.x())),
		0,
		std::max(video.width() - width, 0));
	const auto top = std::clamp(
		int(std::round(rect.y())),
		0,
		std::max(video.height() - height, 0));
	return QRect(left, top, width, height).intersected(
		QRect(QPoint(), video));
}

[[nodiscard]] QString FormatTime(crl::time ms) {
	const auto tenths = std::max(ms, crl::time(0)) / 100;
	const auto seconds = tenths / 10;
	return u"%1:%2.%3"_q
		.arg(seconds / 60)
		.arg(seconds % 60, 2, 10, QChar('0'))
		.arg(tenths % 10);
}

[[nodiscard]] QString FormatPercent(float64 progress) {
	return QString::number(int(std::round(
		std::clamp(progress, 0., 1.) * 100))) + '%';
}

[[nodiscard]] QString FormatZoom(float64 zoom) {
	return LottieEdit::FormatDecimal(zoom, 1) + QChar(0xD7);
}

class CropPreview final : public Ui::RpWidget {
public:
	explicit CropPreview(QWidget *parent);

	void setVideoSize(QSize size);
	void setFrame(QImage frame);
	void setCrop(const Crop &crop);
	void setProgress(std::optional<float64> progress);
	void setInteractive(bool interactive);

	[[nodiscard]] bool hasFrame() const {
		return !_frame.isNull();
	}
	[[nodiscard]] std::optional<float64> progress() const {
		return _progress;
	}
	[[nodiscard]] rpl::producer<QPointF> centerChanges() const {
		return _centerChanges.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] QRect frameRect() const;
	[[nodiscard]] QRectF cropRect() const;
	[[nodiscard]] bool movable() const;
	void updateCursor(QPoint point);

	QImage _frame;
	QSize _videoSize;
	Crop _crop;
	std::optional<float64> _progress;
	bool _interactive = false;
	bool _dragging = false;
	QPoint _dragStart;
	QPointF _dragStartCenter;
	rpl::event_stream<QPointF> _centerChanges;

};

CropPreview::CropPreview(QWidget *parent)
: RpWidget(parent) {
	setMouseTracking(true);
	resize(width(), style::ConvertScale(kPreviewHeight));
}

void CropPreview::setVideoSize(QSize size) {
	_videoSize = size;
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void CropPreview::setFrame(QImage frame) {
	_frame = std::move(frame);
	update();
}

void CropPreview::setCrop(const Crop &crop) {
	_crop = crop;
	update();
}

void CropPreview::setProgress(std::optional<float64> progress) {
	_progress = progress;
	update();
}

void CropPreview::setInteractive(bool interactive) {
	_interactive = interactive;
	if (!interactive) {
		_dragging = false;
	}
	updateCursor(mapFromGlobal(QCursor::pos()));
}

int CropPreview::resizeGetHeight(int newWidth) {
	if (_videoSize.isEmpty() || newWidth <= 0) {
		return style::ConvertScale(kPreviewHeight);
	}
	const auto fitted = int(std::round(
		newWidth * float64(_videoSize.height()) / _videoSize.width()));
	return std::clamp(
		fitted,
		style::ConvertScale(kPreviewMinHeight),
		style::ConvertScale(kPreviewMaxHeight));
}

QRect CropPreview::frameRect() const {
	const auto outer = rect();
	if (_videoSize.isEmpty()) {
		const auto side = std::min(outer.width(), outer.height());
		return QRect(
			(outer.width() - side) / 2,
			(outer.height() - side) / 2,
			side,
			side);
	}
	const auto size = _videoSize.scaled(outer.size(), Qt::KeepAspectRatio);
	return QRect(
		QPoint(
			(outer.width() - size.width()) / 2,
			(outer.height() - size.height()) / 2),
		size);
}

QRectF CropPreview::cropRect() const {
	const auto frame = frameRect();
	return CropRect(QSizeF(frame.size()), _crop).translated(frame.topLeft());
}

bool CropPreview::movable() const {
	if (!_interactive || _videoSize.isEmpty()) {
		return false;
	}
	const auto frame = frameRect();
	const auto crop = cropRect();
	return (crop.width() < frame.width() - 0.5)
		|| (crop.height() < frame.height() - 0.5);
}

void CropPreview::updateCursor(QPoint point) {
	setCursor((_dragging || (movable() && cropRect().contains(point)))
		? style::cur_sizeall
		: style::cur_default);
}

void CropPreview::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto frame = frameRect();
	const auto crop = cropRect();
	const auto radius = style::ConvertScale(kCornerRadius);
	auto rounded = QPainterPath();
	rounded.addRoundedRect(frame, radius, radius);
	if (!_frame.isNull()) {
		p.setClipPath(rounded);
		p.drawImage(frame, _frame);

		// The chosen part is shown a pixel smaller than it is: where it
		// reaches the edges of the picture a dimmed rim of the picture is
		// left around the white frame, which is lost on the light box
		// background otherwise (the part looked shorter than the picture).
		const auto rim = float64(style::ConvertScale(1));
		const auto shown = crop.adjusted(rim, rim, -rim, -rim);
		auto inside = QPainterPath();
		inside.addRoundedRect(
			QRectF(frame).adjusted(rim, rim, -rim, -rim),
			radius - rim,
			radius - rim);
		const auto dimColor = QColor(0, 0, 0, kDimAlpha);
		auto dim = QPainterPath();
		dim.setFillRule(Qt::OddEvenFill);
		dim.addRect(frame);
		dim.addRect(shown);
		p.fillPath(dim, dimColor);

		// The rim in the round corners of the picture, when the part
		// reaches them: nothing is painted here otherwise.
		p.setClipRect(shown, Qt::IntersectClip);
		auto corners = QPainterPath();
		corners.setFillRule(Qt::OddEvenFill);
		corners.addRect(frame);
		corners.addPath(inside);
		p.fillPath(corners, dimColor);

		// The frame of the chosen part is its inner outline: only the
		// inner half of each line is left by the clipping. Where the part
		// reaches the round corners of the picture the frame follows them
		// (the second line), it doesn't stick out of them with a sharp
		// corner. An opaque color, the two lines overlap along the edges.
		const auto stroke = style::ConvertScale(2);
		p.setClipPath(inside, Qt::IntersectClip);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(Qt::white, stroke * 2.));
		p.drawRect(shown);
		p.drawPath(inside);
		p.setClipping(false);
	} else {
		p.fillPath(rounded, st::windowBgOver);
	}
	if (_progress) {
		const auto size = style::ConvertScale(kProgressSize);
		const auto center = _frame.isNull()
			? QPointF(frame.center())
			: crop.center();
		const auto circle = QRectF(
			center.x() - size / 2.,
			center.y() - size / 2.,
			size,
			size);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, kDimAlpha));
		p.drawEllipse(circle);

		const auto stroke = style::ConvertScale(3);
		const auto inset = stroke * 1.5;
		const auto arc = circle.marginsRemoved(
			{ inset, inset, inset, inset });
		auto pen = QPen(QColor(255, 255, 255));
		pen.setWidth(stroke);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto length = int(std::round(
			std::clamp(*_progress, 0., 1.) * 360 * 16));
		p.drawArc(arc, 90 * 16, -std::max(length, 16));

		p.setFont(st::normalFont);
		p.drawText(circle, Qt::AlignCenter, FormatPercent(*_progress));
	}
}

void CropPreview::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton
		|| !movable()
		|| !frameRect().contains(e->pos())) {
		return;
	}
	const auto frame = frameRect();
	const auto center = cropRect().center() - QPointF(frame.topLeft());
	_dragging = true;
	_dragStart = e->pos();
	_dragStartCenter = QPointF(
		center.x() / frame.width(),
		center.y() / frame.height());
	updateCursor(e->pos());
}

void CropPreview::mouseMoveEvent(QMouseEvent *e) {
	if (!_dragging) {
		updateCursor(e->pos());
		return;
	}
	const auto frame = frameRect();
	if (frame.isEmpty()) {
		return;
	}
	const auto delta = e->pos() - _dragStart;
	auto moved = _crop;
	moved.x = _dragStartCenter.x() + delta.x() / float64(frame.width());
	moved.y = _dragStartCenter.y() + delta.y() / float64(frame.height());

	// Keep the center where the frame really is, so that dragging back
	// from an edge starts moving it at once.
	const auto size = QSizeF(frame.size());
	const auto placed = CropRect(size, moved).center();
	const auto center = QPointF(
		placed.x() / size.width(),
		placed.y() / size.height());
	if (QPointF(_crop.x, _crop.y) != center) {
		_crop.x = center.x();
		_crop.y = center.y();
		update();
		_centerChanges.fire_copy(center);
	}
}

void CropPreview::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && _dragging) {
		_dragging = false;
		updateCursor(e->pos());
	}
}

class TrimStrip final : public Ui::RpWidget {
public:
	enum class Edge {
		None,
		Start,
		End,
		Both,
	};
	struct Change {
		crl::time from = 0;
		crl::time till = 0;
		Edge edge = Edge::None;
	};

	TrimStrip(QWidget *parent, crl::time duration);

	void setRange(crl::time from, crl::time till);
	void setThumbnails(std::vector<QImage> thumbnails);
	void setInteractive(bool interactive);

	[[nodiscard]] bool hasThumbnails() const {
		return !_thumbnails.empty();
	}
	[[nodiscard]] rpl::producer<Change> changes() const {
		return _changes.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] int handle() const;
	[[nodiscard]] QRect track() const;
	[[nodiscard]] int xFor(crl::time time) const;
	[[nodiscard]] crl::time timeFor(int x) const;
	[[nodiscard]] Edge edgeAt(int x) const;
	void moveTo(int x);
	void updateCursor(int x);

	const crl::time _duration = 0;
	const crl::time _minLength = 0;
	const crl::time _maxLength = 0;
	crl::time _from = 0;
	crl::time _till = 0;
	std::vector<QImage> _thumbnails;
	Edge _dragging = Edge::None;
	crl::time _grab = 0;
	bool _interactive = true;
	rpl::event_stream<Change> _changes;

};

TrimStrip::TrimStrip(QWidget *parent, crl::time duration)
: RpWidget(parent)
, _duration(std::max(duration, crl::time(1)))
, _minLength(std::min(kMinLength, _duration))
, _maxLength(std::min(kMaxLength, _duration))
, _till(_maxLength) {
	setMouseTracking(true);
	resize(width(), style::ConvertScale(kTrimHeight));
}

void TrimStrip::setRange(crl::time from, crl::time till) {
	_from = from;
	_till = till;
	update();
}

void TrimStrip::setThumbnails(std::vector<QImage> thumbnails) {
	_thumbnails = std::move(thumbnails);
	update();
}

void TrimStrip::setInteractive(bool interactive) {
	_interactive = interactive;
	if (!interactive) {
		_dragging = Edge::None;
	}
	updateCursor(mapFromGlobal(QCursor::pos()).x());
}

int TrimStrip::resizeGetHeight(int newWidth) {
	return style::ConvertScale(kTrimHeight);
}

int TrimStrip::handle() const {
	return style::ConvertScale(kTrimHandleWidth);
}

QRect TrimStrip::track() const {
	return QRect(handle(), 0, std::max(width() - 2 * handle(), 1), height());
}

int TrimStrip::xFor(crl::time time) const {
	const auto area = track();
	return area.x()
		+ int(std::round(area.width() * float64(time) / _duration));
}

crl::time TrimStrip::timeFor(int x) const {
	const auto area = track();
	const auto result = crl::time(std::round(
		(x - area.x()) * float64(_duration) / area.width()));
	return std::clamp(result, crl::time(0), _duration);
}

// In a long video the three seconds are only a few pixels wide: the
// handles are outside of the chosen part, so they never overlap, and
// a press anywhere else moves the whole part there.
TrimStrip::Edge TrimStrip::edgeAt(int x) const {
	const auto left = xFor(_from);
	const auto right = xFor(_till);
	if (x >= left - handle() && x < left) {
		return Edge::Start;
	} else if (x > right && x <= right + handle()) {
		return Edge::End;
	}
	return Edge::Both;
}

void TrimStrip::updateCursor(int x) {
	const auto edge = (_dragging != Edge::None)
		? _dragging
		: _interactive
		? edgeAt(x)
		: Edge::None;
	setCursor((edge == Edge::Start || edge == Edge::End)
		? style::cur_sizehor
		: (edge == Edge::Both)
		? style::cur_pointer
		: style::cur_default);
}

void TrimStrip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto area = rect();
	const auto radius = style::ConvertScale(kCornerRadius);
	auto clip = QPainterPath();
	clip.addRoundedRect(area, radius, radius);
	p.setClipPath(clip);
	if (_thumbnails.empty()) {
		p.fillRect(area, st::windowBgOver);
	} else {
		const auto count = int(_thumbnails.size());
		const auto tile = area.width() / float64(count);
		const auto edge = [&](int index) {
			return area.x() + int(std::round(index * tile));
		};
		for (auto i = 0; i != count; ++i) {
			const auto target = QRectF(QRect(
				edge(i),
				area.y(),
				edge(i + 1) - edge(i),
				area.height()));
			const auto &image = _thumbnails[i];
			if (image.isNull() || target.isEmpty()) {
				p.fillRect(target, st::windowBgOver);
				continue;
			}
			const auto side = image.width();
			const auto part = std::min(
				side * target.width() / target.height(),
				float64(side));
			const auto source = QRectF(
				(side - part) / 2.,
				0.,
				part,
				std::min(
					side * target.height() / target.width(),
					float64(side)));
			p.drawImage(target, image, source.translated(
				0.,
				(side - source.height()) / 2.));
		}
	}
	const auto left = xFor(_from);
	const auto right = std::max(xFor(_till), left + 1);
	const auto dim = QColor(0, 0, 0, kDimAlpha);
	p.fillRect(QRect(0, 0, left, height()), dim);
	p.fillRect(QRect(right, 0, width() - right, height()), dim);
	p.setClipping(false);

	const auto border = style::ConvertScale(kTrimBorder);
	auto frame = QPainterPath();
	frame.setFillRule(Qt::OddEvenFill);
	frame.addRoundedRect(
		QRect(left - handle(), 0, right - left + 2 * handle(), height()),
		radius,
		radius);
	frame.addRect(QRect(left, border, right - left, height() - 2 * border));
	p.fillPath(
		frame,
		_interactive ? st::activeButtonBg : st::windowSubTextFg);

	auto pen = QPen(st::activeButtonFg);
	pen.setWidth(style::ConvertScale(2));
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	const auto top = height() * 0.35;
	const auto bottom = height() * 0.65;
	const auto leftGrip = left - handle() / 2.;
	const auto rightGrip = right + handle() / 2.;
	p.drawLine(QPointF(leftGrip, top), QPointF(leftGrip, bottom));
	p.drawLine(QPointF(rightGrip, top), QPointF(rightGrip, bottom));
}

void TrimStrip::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_interactive) {
		return;
	}
	const auto x = e->pos().x();
	const auto at = timeFor(x);
	_dragging = edgeAt(x);
	switch (_dragging) {
	case Edge::Start:
		_grab = at - _from;
		break;
	case Edge::End:
		_grab = at - _till;
		break;
	case Edge::Both:
		// Inside of the part: keep the grabbed point under the cursor,
		// outside of it: the middle of the part jumps to the cursor.
		_grab = (at >= _from && at <= _till)
			? (at - _from)
			: ((_till - _from) / 2);
		break;
	case Edge::None:
		break;
	}
	moveTo(x);
	updateCursor(x);
}

void TrimStrip::mouseMoveEvent(QMouseEvent *e) {
	if (_dragging == Edge::None) {
		updateCursor(e->pos().x());
		return;
	}
	moveTo(e->pos().x());
}

void TrimStrip::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && _dragging != Edge::None) {
		moveTo(e->pos().x());
		_dragging = Edge::None;
		updateCursor(e->pos().x());
	}
}

void TrimStrip::moveTo(int x) {
	const auto at = timeFor(x) - _grab;
	const auto length = _till - _from;
	auto from = _from;
	auto till = _till;
	switch (_dragging) {
	case Edge::Start:
		from = std::clamp(
			at,
			std::max(_till - _maxLength, crl::time(0)),
			_till - _minLength);
		break;
	case Edge::End:
		till = std::clamp(
			at,
			_from + _minLength,
			std::min(_from + _maxLength, _duration));
		break;
	case Edge::Both:
		from = std::clamp(at, crl::time(0), _duration - length);
		till = from + length;
		break;
	case Edge::None:
		return;
	}
	if (from == _from && till == _till) {
		return;
	}
	_from = from;
	_till = till;
	update();
	_changes.fire({ .from = _from, .till = _till, .edge = _dragging });
}

enum class Phase {
	Downloading,
	Opening,
	Ready,
	Converting,
	Failed,
};

// What the box reads the video through, used only on its queue:
// RoundVideo::Reader for real videos, painted frames in the UI snapshots.
class EditorVideo {
public:
	virtual ~EditorVideo() = default;

	[[nodiscard]] virtual RoundVideo::Info info() const = 0;
	[[nodiscard]] virtual QImage frame(crl::time position, int maxSide) = 0;
	[[nodiscard]] virtual std::vector<QImage> thumbnails(
		int count,
		int side) = 0;

};

class ReaderVideo final : public EditorVideo {
public:
	explicit ReaderVideo(RoundVideo::Source source)
	: _reader(std::move(source)) {
	}

	RoundVideo::Info info() const override {
		return _reader.info();
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

using OpenVideo = Fn<std::unique_ptr<EditorVideo>(RoundVideo::Source)>;
using ConvertVideo = Fn<VideoCore::VideoStickerResult(
	const VideoCore::VideoStickerOptions &options,
	Fn<void(float64)> progress,
	VideoCore::Cancel cancel)>;

class QueuedVideo final {
public:
	QueuedVideo(OpenVideo open, RoundVideo::Source source)
	: _video(open
		? open(std::move(source))
		: std::unique_ptr<EditorVideo>(
			std::make_unique<ReaderVideo>(std::move(source)))) {
	}

	[[nodiscard]] EditorVideo &get() const {
		return *_video;
	}

private:
	const std::unique_ptr<EditorVideo> _video;

};

struct DownloadHandlers {
	Fn<void(float64)> progress;
	Fn<void(RoundVideo::Source)> done; // An empty source when it failed.
};

struct TrimArgs {
	std::shared_ptr<Ui::Show> show;
	RoundVideo::Source source;

	// Gets the video when there is no source yet (a video from a chat).
	Fn<void(not_null<Ui::GenericBox*>, DownloadHandlers)> download;

	Fn<void(QByteArray webm)> done;

	// RoundVideo::Reader and VideoCore::MakeVideoSticker when not set.
	OpenVideo open;
	ConvertVideo convert;

	// Starts converting as soon as the video is opened (UI snapshots).
	bool convertWhenOpened = false;
};

struct BoxState : base::has_weak_ptr {
	~BoxState() {
		if (cancel) {
			*cancel = true;
		}
	}

	RoundVideo::Source source;
	RoundVideo::Info info;
	OpenVideo openVideo;
	ConvertVideo convertVideo;
	Fn<void(QByteArray)> done;
	bool convertWhenOpened = false;
	std::optional<crl::object_on_queue<QueuedVideo>> video;
	int frameSide = 0;
	std::shared_ptr<std::atomic<int>> frameRequest
		= std::make_shared<std::atomic<int>>(0);
	VideoCore::Cancel cancel;
	Phase phase = Phase::Opening;
	crl::time from = 0;
	crl::time till = 0;
	Crop crop;
	rpl::variable<QString> status;
	rpl::variable<QString> zoomText;

	Fn<void(RoundVideo::Info, QImage)> opened;
	Fn<void(VideoCore::VideoStickerResult)> converted;
	Fn<void()> convert;
	Fn<void(crl::time)> shift;

	// The links, the checkbox and the slider look disabled while the
	// editor is locked.
	Fn<void(bool)> controlsEnabled;

	CropPreview *preview = nullptr;
	TrimStrip *trim = nullptr;
	Ui::SlideWrap<Ui::VerticalLayout> *editor = nullptr;
};

[[nodiscard]] int EditorWidth(const std::shared_ptr<Ui::Show> &show) {
	const auto wide = style::ConvertScale(kBoxWidth);
	const auto shadow = st::boxRoundShadow.extend;
	const auto available = show
		? (show->toastParent()->width() - shadow.left() - shadow.right())
		: 0;
	return (available > 0)
		? std::clamp(available, st::boxWideWidth, wide)
		: wide;
}

void VideoStickerBox(not_null<Ui::GenericBox*> box, TrimArgs &&args) {
	const auto show = std::move(args.show);
	box->setTitle(tr::lng_oblivion_vsticker_title());
	box->setWidth(EditorWidth(show));

	const auto state = box->lifetime().make_state<BoxState>();
	const auto weak = base::make_weak(state);
	const auto download = std::move(args.download);
	state->source = std::move(args.source);
	state->openVideo = std::move(args.open);
	state->convertVideo = args.convert
		? std::move(args.convert)
		: ConvertVideo(VideoCore::MakeVideoSticker);
	state->done = std::move(args.done);
	state->convertWhenOpened = args.convertWhenOpened;
	state->frameSide = style::ConvertScale(kBoxWidth)
		* style::DevicePixelRatio();

	const auto content = box->verticalLayout();
	Ui::AddSkip(content);
	state->preview = box->addRow(object_ptr<CropPreview>(box));
	Ui::AddSkip(content, st::boxLittleSkip);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		state->status.value(),
		st::boxDividerLabel));
	state->editor = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			box,
			object_ptr<Ui::VerticalLayout>(box)),
		style::margins());
	state->editor->hide(anim::type::instant);
	Ui::AddSkip(content, st::boxLittleSkip);

	const auto close = [=] {
		box->closeBox();
	};
	const auto setPhase = [=](Phase phase) {
		state->phase = phase;
		const auto editable = (phase == Phase::Ready);
		state->preview->setInteractive(editable);
		if (state->trim) {
			state->trim->setInteractive(editable);
		}
		state->editor->setAttribute(
			Qt::WA_TransparentForMouseEvents,
			!editable);
		if (state->controlsEnabled) {
			state->controlsEnabled(editable);
		}
		box->clearButtons();
		if (phase == Phase::Ready) {
			box->addButton(tr::lng_oblivion_packs_next(), [=] {
				// Converting rebuilds the buttons, not inside a click.
				crl::on_main(weak, [=] {
					if (state->convert && state->phase == Phase::Ready) {
						state->convert();
					}
				});
			});
			box->addButton(tr::lng_cancel(), close);
		} else if (phase == Phase::Failed) {
			box->addButton(tr::lng_close(), close);
		} else {
			box->addButton(tr::lng_cancel(), close);
		}
	};
	const auto fail = [=](const QString &text) {
		state->preview->setProgress(std::nullopt);
		state->status = text;
		setPhase(Phase::Failed);
	};
	const auto refreshRange = [=] {
		state->status = tr::lng_oblivion_round_range(
			tr::now,
			lt_from,
			FormatTime(state->from),
			lt_till,
			FormatTime(state->till),
			lt_duration,
			FormatTime(state->till - state->from));
	};
	const auto requestFrame = [=](crl::time position) {
		if (!state->video) {
			return;
		}
		const auto latest = state->frameRequest;
		const auto id = ++*latest;
		const auto side = state->frameSide;
		state->video->with([=](QueuedVideo &video) {
			if (*latest != id) {
				return;
			}
			auto frame = video.get().frame(position, side);
			crl::on_main(weak, [=, frame = std::move(frame)]() mutable {
				if (*latest == id && !frame.isNull()) {
					state->preview->setFrame(std::move(frame));
				}
			});
		});
	};

	state->opened = [=](RoundVideo::Info info, QImage frame) {
		if (!info.valid()) {
			fail(tr::lng_oblivion_round_open_failed(tr::now));
			return;
		} else if (info.duration < kMinDuration) {
			fail(tr::lng_oblivion_round_too_short(tr::now));
			return;
		}
		state->info = info;
		state->from = 0;
		state->till = std::min(info.duration, kMaxLength);
		state->preview->setVideoSize(info.size);
		state->preview->setFrame(std::move(frame));
		state->preview->setCrop(state->crop);
		state->preview->setProgress(std::nullopt);
		state->preview->centerChanges(
		) | rpl::on_next([=](QPointF center) {
			state->crop.x = center.x();
			state->crop.y = center.y();
		}, state->preview->lifetime());

		const auto container = state->editor->entity();
		Ui::AddSkip(container, st::boxLittleSkip);
		state->trim = container->add(
			object_ptr<TrimStrip>(container, info.duration),
			st::boxRowPadding);
		state->trim->setRange(state->from, state->till);
		state->trim->changes(
		) | rpl::on_next([=](const TrimStrip::Change &change) {
			state->from = change.from;
			state->till = change.till;
			refreshRange();
			requestFrame((change.edge == TrimStrip::Edge::End)
				? std::max(state->from, state->till - 100)
				: state->from);
		}, state->trim->lifetime());

		// In a long video one pixel of the strip is more than a second:
		// the links under it (and the arrow keys) move the part by 0.1 s.
		state->shift = [=](crl::time delta) {
			if (state->phase != Phase::Ready) {
				return;
			}
			const auto length = state->till - state->from;
			const auto from = std::clamp(
				state->from + delta,
				crl::time(0),
				std::max(state->info.duration - length, crl::time(0)));
			if (from == state->from) {
				return;
			}
			state->from = from;
			state->till = from + length;
			state->trim->setRange(state->from, state->till);
			refreshRange();
			requestFrame(state->from);
		};
		const auto steps = container->add(
			object_ptr<Ui::FixedHeightWidget>(container),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
		const auto back = Ui::CreateChild<Ui::LinkButton>(
			steps,
			tr::lng_oblivion_vsticker_step_back(tr::now));
		const auto forward = Ui::CreateChild<Ui::LinkButton>(
			steps,
			tr::lng_oblivion_vsticker_step_forward(tr::now));
		steps->resize(steps->width(), back->height());
		steps->widthValue(
		) | rpl::on_next([=](int width) {
			back->moveToLeft(0, 0, width);
			forward->moveToRight(0, 0, width);
		}, steps->lifetime());
		back->setClickedCallback([=] {
			state->shift(-kStep);
		});
		forward->setClickedCallback([=] {
			state->shift(kStep);
		});

		Ui::AddSkip(container);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				tr::lng_oblivion_vsticker_limit(),
				st::boxDividerLabel),
			st::boxRowPadding);

		const auto applyCrop = [=] {
			state->preview->setCrop(state->crop);
			state->preview->setInteractive(state->phase == Phase::Ready);
		};
		auto square = (Ui::Checkbox*)nullptr;
		if (info.size.width() != info.size.height()) {
			Ui::AddSkip(container, st::boxMediumSkip);
			square = container->add(
				object_ptr<Ui::Checkbox>(
					container,
					tr::lng_oblivion_vsticker_square(tr::now),
					state->crop.square,
					st::defaultBoxCheckbox),
				st::boxRowPadding);
			square->checkedChanges(
			) | rpl::on_next([=](bool checked) {
				state->crop.square = checked;
				applyCrop();
			}, square->lifetime());
		}

		Ui::AddSkip(container, st::boxMediumSkip);
		state->zoomText = tr::lng_oblivion_vsticker_zoom(
			tr::now,
			lt_value,
			FormatZoom(state->crop.zoom));
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				state->zoomText.value(),
				st::boxDividerLabel),
			st::boxRowPadding);
		const auto sliderSt = container->lifetime().make_state<
			style::MediaSlider>(st::defaultContinuousSlider);
		sliderSt->seekSize = QSize(
			style::ConvertScale(15),
			style::ConvertScale(15));
		const auto slider = container->add(
			object_ptr<Ui::MediaSlider>(container, *sliderSt),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
		slider->resize(slider->width(), sliderSt->seekSize.height());
		slider->setAlwaysDisplayMarker(true);
		slider->setValue((state->crop.zoom - 1.) / (kMaxZoom - 1.));
		slider->setChangeProgressCallback([=](float64 value) {
			const auto zoom = 1. + std::clamp(value, 0., 1.) * (kMaxZoom - 1.);
			state->crop.zoom = std::round(zoom * 10.) / 10.;
			state->zoomText = tr::lng_oblivion_vsticker_zoom(
				tr::now,
				lt_value,
				FormatZoom(state->crop.zoom));
			applyCrop();
		});
		Ui::AddSkip(container);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				tr::lng_oblivion_vsticker_crop_about(),
				st::boxDividerLabel),
			st::boxRowPadding);

		state->controlsEnabled = [=](bool enabled) {
			const auto color = enabled
				? std::optional<QColor>()
				: std::make_optional(st::windowSubTextFg->c);
			back->setColorOverride(color);
			forward->setColorOverride(color);
			if (square) {
				square->setDisabled(!enabled);
			}
			slider->setColorOverrides({
				.activeFg = color,
				.seekFg = color,
			});
		};

		state->editor->show(anim::type::normal);
		refreshRange();
		setPhase(Phase::Ready);
		if (state->convertWhenOpened) {
			state->convert();
		}
	};

	const auto open = [=] {
		state->status = tr::lng_oblivion_round_opening(tr::now);
		state->preview->setProgress(std::nullopt);
		setPhase(Phase::Opening);

		const auto frameSide = state->frameSide;
		const auto thumbnailSide = style::ConvertScale(kTrimHeight)
			* style::DevicePixelRatio();
		state->video.emplace(
			OpenVideo(state->openVideo),
			RoundVideo::Source(state->source));
		state->video->with([=](QueuedVideo &queued) {
			auto &video = queued.get();
			const auto info = video.info();
			auto frame = info.valid()
				? video.frame(0, frameSide)
				: QImage();
			crl::on_main(weak, [=, frame = std::move(frame)]() mutable {
				state->opened(info, std::move(frame));
			});
			if (!info.valid()) {
				return;
			}
			auto thumbnails = video.thumbnails(kThumbnails, thumbnailSide);
			crl::on_main(weak, [=, list = std::move(thumbnails)]() mutable {
				if (state->trim) {
					state->trim->setThumbnails(std::move(list));
				}
			});
		});
	};

	state->converted = [=](VideoCore::VideoStickerResult result) {
		state->preview->setProgress(std::nullopt);
		refreshRange();
		setPhase(Phase::Ready);
		if (!result.ok || result.webm.isEmpty()) {
			LOG(("Oblivion Sticker Error: no video sticker, %1."
				).arg(result.error));
			show->showToast(tr::lng_oblivion_vsticker_failed(tr::now));
			return;
		}
		// The next box goes above this one, which is closed after that,
		// not from inside of its own callback.
		crl::on_main(box, close);
		if (const auto done = state->done) {
			done(std::move(result.webm));
		}
	};

	state->convert = [=] {
		const auto cancel = std::make_shared<std::atomic<bool>>(false);
		if (state->cancel) {
			*state->cancel = true;
		}
		state->cancel = cancel;
		state->status = tr::lng_oblivion_round_converting(
			tr::now,
			lt_percent,
			FormatPercent(0.));
		state->preview->setProgress(0.);
		setPhase(Phase::Converting);

		const auto options = VideoCore::VideoStickerOptions{
			.path = state->source.path,
			.from = state->from,
			.till = state->till,
			.crop = SourceCrop(state->info.size, state->crop),
			.side = VideoCore::kStickerSide,
			.content = (state->source.path.isEmpty()
				? state->source.content
				: QByteArray()),
		};
		const auto progress = [=](float64 value) {
			crl::on_main(weak, [=] {
				if (state->cancel != cancel
					|| state->phase != Phase::Converting) {
					return;
				}
				state->preview->setProgress(value);
				state->status = tr::lng_oblivion_round_converting(
					tr::now,
					lt_percent,
					FormatPercent(value));
			});
		};
		const auto converter = state->convertVideo;
		crl::async([=] {
			auto result = converter(options, progress, cancel);
			crl::on_main(weak, [=, result = std::move(result)]() mutable {
				if (state->cancel == cancel && !*cancel) {
					state->converted(std::move(result));
				}
			});
		});
	};

	box->boxClosing() | rpl::on_next([=] {
		if (state->cancel) {
			*state->cancel = true;
		}
	}, box->lifetime());

	box->setFocusCallback([=] { box->setFocus(); });
	base::install_event_filter(box, [=](not_null<QEvent*> e) {
		if (e->type() != QEvent::KeyPress || !state->shift) {
			return base::EventFilterResult::Continue;
		}
		const auto event = static_cast<QKeyEvent*>(e.get());
		const auto key = event->key();
		if (key != Qt::Key_Left && key != Qt::Key_Right) {
			return base::EventFilterResult::Continue;
		}
		const auto step = (event->modifiers() & Qt::ShiftModifier)
			? kBigStep
			: kStep;
		state->shift((key == Qt::Key_Left) ? -step : step);
		return base::EventFilterResult::Cancel;
	});

	if (!state->source.empty()) {
		open();
		return;
	} else if (!download) {
		fail(tr::lng_oblivion_round_open_failed(tr::now));
		return;
	}

	setPhase(Phase::Downloading);
	download(box, {
		.progress = [=](float64 progress) {
			if (state->phase != Phase::Downloading) {
				return;
			}
			state->status = tr::lng_oblivion_round_downloading(
				tr::now,
				lt_percent,
				FormatPercent(progress));
			state->preview->setProgress(progress);
		},
		.done = [=](RoundVideo::Source source) {
			if (state->phase != Phase::Downloading) {
				return;
			} else if (source.empty()) {
				fail(tr::lng_oblivion_round_download_failed(tr::now));
				return;
			}
			state->source = std::move(source);
			open();
		},
	});
}

// A video from a chat is downloaded only to be converted: into the
// temporary folder, without the "save file" dialog, a copy left in the
// downloads folder or an entry in the downloads list. The file is removed
// when the box is closed.
[[nodiscard]] QString TempDownloadFolder(not_null<DocumentData*> document) {
	return QDir(document->session().local().tempDirectory()).filePath(
		u"oblivion_sticker/"_q + QString::number(document->id));
}

[[nodiscard]] QString TempDownloadPath(
		not_null<DocumentData*> document,
		const QString &folder) {
	const auto name = document->filename();
	return QDir(folder).filePath(base::FileNameFromUserString(
		name.isEmpty() ? u"video.mp4"_q : name));
}

// On Windows a file can't be removed while it is open, and the reader
// closes the video on its queue only after the box is destroyed: there
// the removal is repeated for a while, until it succeeds.
constexpr auto kRemoveRetryDelay = crl::time(500);
constexpr auto kRemoveRetries = 20;

void RemoveTempFile(
		const QString &path,
		Fn<void()> removed = nullptr,
		int retries = kRemoveRetries) {
	if (QFile::remove(path) || !QFile::exists(path)) {
		if (removed) {
			removed();
		}
	} else if (retries > 0) {
		base::call_delayed(kRemoveRetryDelay, [=] {
			RemoveTempFile(path, removed, retries - 1);
		});
	}
}

void RemoveTempDownload(
		not_null<DocumentData*> document,
		const QString &path) {
	// Forget the removed file, as if the user has removed it.
	RemoveTempFile(path, crl::guard(&document->session(), [=] {
		(void)document->location(true);
		document->session().data().requestDocumentViewRepaint(document);
	}));
}

void DownloadForSticker(
		not_null<Ui::GenericBox*> box,
		not_null<DocumentData*> document,
		FullMsgId context,
		DownloadHandlers handlers) {
	struct State {
		~State() {
			if (!tempPath.isEmpty()) {
				RemoveTempFile(tempPath);
			}
		}

		std::shared_ptr<Data::DocumentMedia> media;
		QString tempPath;
		rpl::lifetime lifetime;
	};
	const auto state = box->lifetime().make_state<State>();
	state->media = document->createMediaView();

	box->boxClosing() | rpl::on_next([=] {
		if (!state->tempPath.isEmpty()) {
			const auto path = base::take(state->tempPath);
			if (document->loading() && document->loadingFilePath() == path) {
				document->cancel();
			}
			RemoveTempDownload(document, path);
		}
	}, box->lifetime());

	const auto loaded = [=] {
		if (!state->media->loaded(true)) {
			return RoundVideo::Source();
		}
		const auto path = document->filepath(true);
		return path.isEmpty()
			? RoundVideo::Source{ .content = state->media->bytes() }
			: RoundVideo::Source{ .path = path };
	};
	auto ready = loaded();
	if (!ready.empty()) {
		handlers.done(std::move(ready));
		return;
	}
	const auto progress = handlers.progress;
	const auto done = handlers.done;
	progress(document->progress());
	document->session().data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> which) {
		return (which == document);
	}) | rpl::on_next([=] {
		if (document->loading()) {
			progress(document->progress());
		} else {
			done(loaded());
		}
	}, state->lifetime);
	if (!document->loading()) {
		const auto folder = TempDownloadFolder(document);
		if (QDir().mkpath(folder)) {
			state->tempPath = TempDownloadPath(document, folder);
			document->save(
				context ? Data::FileOrigin(context) : Data::FileOrigin(),
				state->tempPath);
		}
		if (!document->loading()) {
			done(loaded());
		}
	}
}

// UI snapshots (see oblivion_ui_snapshots.h): the box with a painted
// video, nothing is read from the disk or converted.

constexpr auto kSceneWidth = 720;
constexpr auto kSampleDuration = crl::time(12'400);
constexpr auto kSampleProgress = 0.42;

void PaintSampleFrame(QPainter &p, QSizeF size, float64 time) {
	p.setRenderHint(QPainter::Antialiasing);
	const auto w = size.width();
	const auto h = size.height();
	auto sky = QLinearGradient(0, 0, 0, h);
	sky.setColorAt(0., QColor(0x24, 0x3b, 0x6b));
	sky.setColorAt(0.7, QColor(0xe8, 0x8d, 0x67));
	sky.setColorAt(1., QColor(0xf6, 0xc9, 0x8a));
	p.fillRect(QRectF(QPointF(), size), sky);

	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xff, 0xe9, 0xa8));
	const auto sun = QPointF(w * (0.2 + 0.6 * time), h * (0.62 - 0.3 * time));
	p.drawEllipse(sun, h * 0.09, h * 0.09);

	auto hills = QPainterPath();
	hills.moveTo(0, h * 0.72);
	hills.cubicTo(w * 0.2, h * 0.52, w * 0.4, h * 0.82, w * 0.62, h * 0.64);
	hills.cubicTo(w * 0.8, h * 0.52, w * 0.92, h * 0.7, w, h * 0.62);
	hills.lineTo(w, h);
	hills.lineTo(0, h);
	p.setBrush(QColor(0x2f, 0x4a, 0x5a));
	p.drawPath(hills);

	// The cat that becomes the sticker.
	const auto unit = h * 0.11;
	const auto cat = QPointF(w * 0.5, h * 0.6);
	p.setBrush(QColor(0xf4, 0xa2, 0x3c));
	p.drawEllipse(cat + QPointF(0, unit * 1.5), unit * 1.3, unit * 1.5);
	p.drawEllipse(cat, unit * 1.1, unit);
	auto ears = QPainterPath();
	ears.moveTo(cat + QPointF(-unit * 1.0, -unit * 0.4));
	ears.lineTo(cat + QPointF(-unit * 0.8, -unit * 1.6));
	ears.lineTo(cat + QPointF(-unit * 0.15, -unit * 0.85));
	ears.moveTo(cat + QPointF(unit * 1.0, -unit * 0.4));
	ears.lineTo(cat + QPointF(unit * 0.8, -unit * 1.6));
	ears.lineTo(cat + QPointF(unit * 0.15, -unit * 0.85));
	p.drawPath(ears);
	p.setBrush(QColor(0x2a, 0x1d, 0x16));
	p.drawEllipse(cat + QPointF(-unit * 0.42, -unit * 0.1), unit * 0.13, unit * 0.18);
	p.drawEllipse(cat + QPointF(unit * 0.42, -unit * 0.1), unit * 0.13, unit * 0.18);
}

class SampleVideo final : public EditorVideo {
public:
	SampleVideo(QSize size, crl::time duration)
	: _info{ .duration = duration, .size = size, .hasAudio = true } {
	}

	RoundVideo::Info info() const override {
		return _info;
	}
	QImage frame(crl::time position, int maxSide) override {
		return Paint(
			_info.size.scaled(maxSide, maxSide, Qt::KeepAspectRatio),
			position / float64(_info.duration));
	}
	std::vector<QImage> thumbnails(int count, int side) override {
		const auto full = _info.size.scaled(
			side,
			side,
			Qt::KeepAspectRatioByExpanding);
		auto result = std::vector<QImage>();
		for (auto i = 0; i < count; ++i) {
			result.push_back(Paint(full, (2 * i + 1) / (2. * count)).copy(
				(full.width() - side) / 2,
				(full.height() - side) / 2,
				side,
				side));
		}
		return result;
	}

private:
	[[nodiscard]] static QImage Paint(QSize size, float64 time) {
		auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
		result.fill(Qt::black);
		{
			auto p = QPainter(&result);
			PaintSampleFrame(p, QSizeF(size), time);
		}
		return result;
	}

	const RoundVideo::Info _info;

};

template <typename Type>
[[nodiscard]] std::vector<Type*> FindWidgets(not_null<QWidget*> parent) {
	auto result = std::vector<Type*>();
	for (const auto child : parent->findChildren<QWidget*>()) {
		if (const auto found = dynamic_cast<Type*>(child)) {
			result.push_back(found);
		}
	}
	return result;
}

[[nodiscard]] bool SampleShown(not_null<QWidget*> box, bool converting) {
	const auto trims = FindWidgets<TrimStrip>(box);
	const auto previews = FindWidgets<CropPreview>(box);
	if (trims.empty()
		|| previews.empty()
		|| !trims.front()->hasThumbnails()
		|| !previews.front()->hasFrame()
		|| (converting
			&& previews.front()->progress().value_or(0.) <= 0.)) {
		return false;
	}
	for (const auto wrap : FindWidgets<Ui::SlideWrap<>>(box)) {
		if (wrap->animating()) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] object_ptr<Ui::BoxContent> SampleBox(
		std::shared_ptr<Ui::Show> show,
		QSize size,
		bool converting) {
	auto args = TrimArgs{
		.show = std::move(show),
		.source = RoundVideo::Source{ .content = QByteArray("sample") },
		.open = [=](RoundVideo::Source) -> std::unique_ptr<EditorVideo> {
			return std::make_unique<SampleVideo>(size, kSampleDuration);
		},
		.convertWhenOpened = converting,
	};
	if (converting) {
		// Stays at the same progress until the box is destroyed.
		args.convert = [](
				const VideoCore::VideoStickerOptions &,
				Fn<void(float64)> progress,
				VideoCore::Cancel cancel) {
			progress(kSampleProgress);
			while (!*cancel) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return VideoCore::VideoStickerResult();
		};
	}
	return Box(VideoStickerBox, std::move(args));
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto scene = [](QString name, QSize size, bool converting) {
		RegisterScene(SceneDescriptor{
			.name = std::move(name),
			.size = QSize(style::ConvertScale(kSceneWidth), 0),
			.box = [=](std::shared_ptr<Ui::Show> show) {
				return SampleBox(std::move(show), size, converting);
			},
			.ready = [=](not_null<QWidget*> widget) {
				return SampleShown(widget, converting);
			},
		});
	};

	// 16:9, 12 seconds: the three seconds part, the square crop.
	scene(u"video_sticker_trim"_q, QSize(1280, 720), false);

	// A square video: no "crop to a square" checkbox.
	scene(u"video_sticker_trim_square"_q, QSize(720, 720), false);

	// Converting: the progress over the preview, the editor locked.
	scene(u"video_sticker_progress"_q, QSize(1280, 720), true);
});

} // namespace

void ShowVideoStickerTrim(
		std::shared_ptr<Ui::Show> show,
		QString path,
		QByteArray content,
		Fn<void(QByteArray webm)> done) {
	// The file may be chosen and read after the app was locked: the box
	// would be shown above the passcode screen.
	if (Core::IsAppLaunched() && Core::App().passcodeLocked()) {
		return;
	} else if (path.isEmpty() && content.isEmpty()) {
		show->showToast(tr::lng_oblivion_round_open_failed(tr::now));
		return;
	}
	const auto copy = show;
	copy->showBox(Box(VideoStickerBox, TrimArgs{
		.show = std::move(show),
		.source = RoundVideo::Source{
			.path = std::move(path),
			.content = std::move(content),
		},
		.done = std::move(done),
	}));
}

void ShowVideoStickerTrim(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		FullMsgId context,
		Fn<void(QByteArray webm)> done) {
	if (controller->window().locked()) {
		return;
	}
	controller->show(Box(VideoStickerBox, TrimArgs{
		.show = controller->uiShow(),
		.download = [=](
				not_null<Ui::GenericBox*> box,
				DownloadHandlers handlers) {
			DownloadForSticker(box, document, context, std::move(handlers));
		},
		.done = std::move(done),
	}));
}

} // namespace Oblivion
