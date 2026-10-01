/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_round_video.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/base_file_utilities.h"
#include "base/call_delayed.h"
#include "base/weak_ptr.h"
#include "chat_helpers/compose/compose_show.h"
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
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "main/session/send_as_peers.h"
#include "mainwindow.h"
#include "menu/menu_send_details.h"
#include "oblivion/oblivion_round_video_convert.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "storage/storage_account.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>
#include <crl/crl_object_on_queue.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainterPath>
#include <QtGui/QRadialGradient>

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
constexpr auto kMinTrimLength = crl::time(1000);
constexpr auto kDimAlpha = 150;
constexpr auto kTempRemoveDelay = crl::time(500);
constexpr auto kTempRemoveTries = 60;

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

[[nodiscard]] not_null<Data::Thread*> ItemThread(
		not_null<HistoryItem*> item) {
	if (const auto topic = item->topic()) {
		return topic;
	} else if (const auto sublist = item->savedSublist()) {
		return sublist;
	}
	return item->history();
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

// A video from the chat is downloaded only to be converted: into the
// temporary folder, without the "save file" dialog, a copy left in the
// downloads folder or an entry in the downloads list.
[[nodiscard]] QString TempDownloadFolder(not_null<DocumentData*> document) {
	return QDir(document->session().local().tempDirectory()).filePath(
		u"oblivion_round/"_q + QString::number(document->id));
}

[[nodiscard]] QString TempDownloadPath(
		not_null<DocumentData*> document,
		const QString &folder) {
	const auto name = document->filename();
	return QDir(folder).filePath(base::FileNameFromUserString(
		name.isEmpty() ? u"video.mp4"_q : name));
}

// Windows doesn't remove a file that is still open, and the editor closes
// the video on its queue (a cancelled conversion on its thread) only after
// the box is gone. So a failed removal is repeated for a while, then done
// is called anyway. Where an open file can be removed (macOS) the first
// try succeeds and done is called right away.
void RemoveTempFile(
		const QString &path,
		Fn<void()> done,
		int tries = kTempRemoveTries) {
	if (QFile::remove(path) || !QFile::exists(path) || tries <= 0) {
		if (done) {
			done();
		}
		return;
	}
	base::call_delayed(kTempRemoveDelay, [=] {
		RemoveTempFile(path, done, tries - 1);
	});
}

// The folder stays: the "retry" of a failed download writes there, and
// a failed write would reset the download path in the settings.
void RemoveTempDownload(
		not_null<DocumentData*> document,
		const QString &path) {
	const auto session = base::make_weak(&document->session());
	RemoveTempFile(path, [=] {
		if (!session) {
			return;
		}

		// Forget the removed file, as if the user has removed it.
		(void)document->location(true);
		document->session().data().requestDocumentViewRepaint(document);
	});
}

[[nodiscard]] bool CheckCanSendRound(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<Data::Thread*> thread) {
	const auto peer = thread->peer();
	const auto restriction = Data::RestrictionError(
		peer,
		ChatRestriction::SendVideoMessages);
	if (restriction) {
		Data::ShowSendErrorToast(show, peer, restriction);
		return false;
	} else if (!Data::CanSend(thread, ChatRestriction::SendVideoMessages)) {
		show->showToast(tr::lng_oblivion_round_restricted(tr::now));
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

// The same path as a recorded round video: FileLoadTask reads the
// dimensions, duration and thumbnail from the mp4 and marks the document
// as a round message (see VoiceRecordBar and ApiWrap::sendVoiceMessage).
void SendRound(
		not_null<Data::Thread*> thread,
		FullReplyTo replyTo,
		const RoundVideo::Result &result,
		Api::SendOptions options) {
	// As HistoryWidget::prepareSendAction(): the identity chosen in the
	// "send as" button of the chat, nothing when it is ourselves.
	const auto session = &thread->session();
	const auto chosen = session->sendAsPeers().resolveChosen(thread->peer());
	if (chosen.get() != session->user().get()) {
		options.sendAs = chosen;
	}
	auto action = Api::SendAction(thread, options);
	action.clearDraft = false;
	if (replyTo.messageId) {
		action.replyTo = replyTo;
		action.replyTo.topicRootId = thread->topicRootId();
	}
	if (!action.replyTo.monoforumPeerId) {
		action.replyTo.monoforumPeerId = thread->monoforumPeerId();
	}
	thread->session().api().sendVoiceMessage(
		result.content,
		VoiceWaveform(),
		result.duration,
		true,
		action);
}

class CropPreview final : public Ui::RpWidget {
public:
	explicit CropPreview(QWidget *parent);

	void setVideoSize(QSize size);
	void setFrame(QImage frame);
	void setPosition(float64 position);
	void setProgress(std::optional<float64> progress);
	void setStatus(const QString &status);
	void setInteractive(bool interactive);

	[[nodiscard]] std::optional<float64> progress() const {
		return _progress;
	}
	[[nodiscard]] rpl::producer<float64> positionChanges() const;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] QRect frameRect() const;
	[[nodiscard]] QRectF circleRect() const;
	[[nodiscard]] bool movable() const;
	void updateCursor(QPoint point);

	QImage _frame;
	QSize _videoSize;
	float64 _position = 0.5;
	std::optional<float64> _progress;
	QString _status;
	bool _interactive = false;
	bool _dragging = false;
	QPoint _dragStart;
	float64 _dragStartPosition = 0.;
	rpl::event_stream<float64> _positionChanges;

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

void CropPreview::setPosition(float64 position) {
	_position = std::clamp(position, 0., 1.);
	update();
}

void CropPreview::setProgress(std::optional<float64> progress) {
	_progress = progress;
	update();
}

void CropPreview::setStatus(const QString &status) {
	_status = status;
	update();
}

void CropPreview::setInteractive(bool interactive) {
	_interactive = interactive;
	if (!interactive) {
		_dragging = false;
	}
	updateCursor(mapFromGlobal(QCursor::pos()));
}

rpl::producer<float64> CropPreview::positionChanges() const {
	return _positionChanges.events();
}

int CropPreview::resizeGetHeight(int newWidth) {
	if (_videoSize.isEmpty() || newWidth <= 0) {
		return style::ConvertScale(kPreviewHeight);
	}
	// As tall as the frame at the full width: no empty bands above and
	// below a landscape video, a portrait one gets the maximum height.
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

QRectF CropPreview::circleRect() const {
	const auto frame = frameRect();
	return RoundVideo::CropSquare(
		QSizeF(frame.size()),
		_position
	).translated(frame.topLeft());
}

bool CropPreview::movable() const {
	return _interactive
		&& !_videoSize.isEmpty()
		&& (_videoSize.width() != _videoSize.height());
}

void CropPreview::updateCursor(QPoint point) {
	const auto horizontal = (_videoSize.width() > _videoSize.height());
	setCursor((_dragging || (movable() && circleRect().contains(point)))
		? (horizontal ? style::cur_sizehor : style::cur_sizever)
		: style::cur_default);
}

void CropPreview::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto frame = frameRect();
	const auto circle = circleRect();
	const auto inset = [&](float64 stroke) {
		// Strokes stay inside the circle: it touches the frame edges.
		const auto half = stroke / 2.;
		return circle.marginsRemoved({ half, half, half, half });
	};
	if (!_frame.isNull()) {
		const auto radius = style::ConvertScale(kCornerRadius);
		auto rounded = QPainterPath();
		rounded.addRoundedRect(frame, radius, radius);
		p.setClipPath(rounded);
		p.drawImage(frame, _frame);
		auto dim = QPainterPath();
		dim.setFillRule(Qt::OddEvenFill);
		dim.addRect(frame);
		dim.addEllipse(circle);
		p.fillPath(dim, QColor(0, 0, 0, kDimAlpha));
		p.setClipping(false);
		if (!_progress) {
			// The progress ring below takes the place of the outline.
			const auto stroke = style::ConvertScale(2);
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(QColor(255, 255, 255, 200), stroke));
			p.drawEllipse(inset(stroke));
		}
	} else {
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawEllipse(circle);
	}
	if (_progress) {
		const auto stroke = style::ConvertScale(4);
		const auto arc = inset(stroke);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(
			(_frame.isNull()
				? st::windowBgRipple->c
				: QColor(255, 255, 255, 90)),
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
		const auto &font = st::semiboldFont;
		const auto padding = style::ConvertScale(10);
		const auto height = font->height + padding;
		const auto width = font->width(_status) + 2 * padding;
		const auto pill = QRectF(
			circle.center().x() - width / 2.,
			circle.center().y() - height / 2.,
			width,
			height);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, kDimAlpha));
		p.drawRoundedRect(pill, height / 2., height / 2.);
		p.setPen(QColor(255, 255, 255));
		p.setFont(font);
		p.drawText(pill, Qt::AlignCenter, _status);
	}
}

void CropPreview::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton
		|| !movable()
		|| !frameRect().contains(e->pos())) {
		return;
	}
	_dragging = true;
	_dragStart = e->pos();
	_dragStartPosition = _position;
	updateCursor(e->pos());
}

void CropPreview::mouseMoveEvent(QMouseEvent *e) {
	if (!_dragging) {
		updateCursor(e->pos());
		return;
	}
	const auto frame = frameRect();
	const auto side = std::min(frame.width(), frame.height());
	const auto horizontal = (frame.width() > frame.height());
	const auto range = (horizontal ? frame.width() : frame.height()) - side;
	if (range <= 0) {
		return;
	}
	const auto delta = horizontal
		? (e->pos().x() - _dragStart.x())
		: (e->pos().y() - _dragStart.y());
	const auto position = std::clamp(
		_dragStartPosition + delta / float64(range),
		0.,
		1.);
	if (_position != position) {
		_position = position;
		update();
		_positionChanges.fire_copy(position);
	}
}

void CropPreview::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && _dragging) {
		_dragging = false;
		updateCursor(e->pos());
	}
}

class TrimSlider final : public Ui::RpWidget {
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

	TrimSlider(QWidget *parent, crl::time duration);

	void setRange(crl::time from, crl::time till);
	void setThumbnails(std::vector<QImage> thumbnails);
	void setInteractive(bool interactive);

	[[nodiscard]] bool hasThumbnails() const {
		return !_thumbnails.empty();
	}
	[[nodiscard]] rpl::producer<Change> changes() const;

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
	crl::time _from = 0;
	crl::time _till = 0;
	std::vector<QImage> _thumbnails;
	Edge _dragging = Edge::None;
	crl::time _grab = 0;
	bool _interactive = true;
	rpl::event_stream<Change> _changes;

};

TrimSlider::TrimSlider(QWidget *parent, crl::time duration)
: RpWidget(parent)
, _duration(std::max(duration, crl::time(1)))
, _minLength(std::min(kMinTrimLength, _duration))
, _till(std::min(_duration, RoundVideo::kMaxDuration)) {
	setMouseTracking(true);
	resize(width(), style::ConvertScale(kTrimHeight));
}

void TrimSlider::setRange(crl::time from, crl::time till) {
	_from = from;
	_till = till;
	update();
}

void TrimSlider::setThumbnails(std::vector<QImage> thumbnails) {
	_thumbnails = std::move(thumbnails);
	update();
}

void TrimSlider::setInteractive(bool interactive) {
	_interactive = interactive;
	if (!interactive) {
		_dragging = Edge::None;
	}
	updateCursor(mapFromGlobal(QCursor::pos()).x());
}

rpl::producer<TrimSlider::Change> TrimSlider::changes() const {
	return _changes.events();
}

int TrimSlider::resizeGetHeight(int newWidth) {
	return style::ConvertScale(kTrimHeight);
}

int TrimSlider::handle() const {
	return style::ConvertScale(kTrimHandleWidth);
}

QRect TrimSlider::track() const {
	return QRect(handle(), 0, std::max(width() - 2 * handle(), 1), height());
}

int TrimSlider::xFor(crl::time time) const {
	const auto area = track();
	return area.x() + int(std::round(area.width() * float64(time) / _duration));
}

crl::time TrimSlider::timeFor(int x) const {
	const auto area = track();
	const auto result = crl::time(std::round(
		(x - area.x()) * float64(_duration) / area.width()));
	return std::clamp(result, crl::time(0), _duration);
}

TrimSlider::Edge TrimSlider::edgeAt(int x) const {
	const auto left = xFor(_from);
	const auto right = xFor(_till);
	const auto slop = handle() / 2;
	const auto nearLeft = (x >= left - handle() - slop) && (x <= left + slop);
	const auto nearRight = (x >= right - slop)
		&& (x <= right + handle() + slop);
	if (nearLeft && nearRight) {
		return (std::abs(x - left) <= std::abs(x - right))
			? Edge::Start
			: Edge::End;
	} else if (nearLeft) {
		return Edge::Start;
	} else if (nearRight) {
		return Edge::End;
	} else if (x > left && x < right) {
		return Edge::Both;
	}
	return (x <= left) ? Edge::Start : Edge::End;
}

void TrimSlider::updateCursor(int x) {
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

void TrimSlider::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	// The strip takes the whole width, as the preview above it does. The
	// time track is narrower by the handles, so that they fit at its ends,
	// the handles cover the strip there.
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
			// Whole pixels: no light seams between the tiles.
			const auto target = QRectF(QRect(
				edge(i),
				area.y(),
				edge(i + 1) - edge(i),
				area.height()));
			const auto &image = _thumbnails[i];
			if (image.isNull()) {
				p.fillRect(target, st::windowBgOver);
				continue;
			}
			// Center-crop the square thumbnail to the tile.
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
	const auto right = xFor(_till);
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
	p.fillPath(frame, _interactive ? st::activeButtonBg : st::windowSubTextFg);

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

void TrimSlider::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_interactive) {
		return;
	}
	const auto x = e->pos().x();
	_dragging = edgeAt(x);
	const auto at = timeFor(x);
	switch (_dragging) {
	case Edge::Start:
		_grab = (x >= xFor(_from) - handle() && x <= xFor(_from))
			? (at - _from)
			: 0;
		break;
	case Edge::End:
		_grab = (x >= xFor(_till) && x <= xFor(_till) + handle())
			? (at - _till)
			: 0;
		break;
	case Edge::Both:
		_grab = at - _from;
		break;
	case Edge::None:
		break;
	}
	moveTo(x);
	updateCursor(x);
}

void TrimSlider::mouseMoveEvent(QMouseEvent *e) {
	if (_dragging == Edge::None) {
		updateCursor(e->pos().x());
		return;
	}
	moveTo(e->pos().x());
}

void TrimSlider::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && _dragging != Edge::None) {
		moveTo(e->pos().x());
		_dragging = Edge::None;
		updateCursor(e->pos().x());
	}
}

void TrimSlider::moveTo(int x) {
	const auto at = timeFor(x) - _grab;
	const auto length = _till - _from;
	auto from = _from;
	auto till = _till;
	switch (_dragging) {
	case Edge::Start:
		from = std::clamp(at, crl::time(0), _till - _minLength);
		till = std::min(_till, from + RoundVideo::kMaxDuration);
		break;
	case Edge::End:
		till = std::clamp(at, _from + _minLength, _duration);
		from = std::max(_from, till - RoundVideo::kMaxDuration);
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

// What the editor reads the video through, used only on its queue:
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
using ConvertVideo = Fn<RoundVideo::Result(
	const RoundVideo::Request &request,
	const std::atomic<bool> &cancelled,
	Fn<void(float64)> progress)>;

// Lives on the editor queue, object_on_queue needs rvalue arguments.
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

enum class SendCheck {
	Ok,
	Wait, // Not now, the reason is already shown.
	Close, // Nowhere to send anymore.
};

struct DownloadHandlers {
	Fn<void(float64)> progress;
	Fn<void(RoundVideo::Source)> done; // An empty source when it failed.
};

// Everything the editor box works with, nothing session-bound: the session
// entry point (ShowRoundBox) gathers it, the UI snapshots fill it with
// samples.
struct RoundEditorArgs {
	std::shared_ptr<Ui::Show> show;
	RoundVideo::Source source;

	// Gets the video when there is no source yet (a video from the chat).
	// Its state lives in the box, the handlers may be called right away.
	Fn<void(not_null<Ui::GenericBox*>, DownloadHandlers)> download;

	// Called before converting and sending, shows the errors itself.
	Fn<SendCheck()> check;

	// Sends the converted video and returns true. False when it is not
	// sent yet: then it may call resend(starsApproved) later, after the
	// paid messages confirmation.
	Fn<bool(
		const RoundVideo::Result &result,
		int starsApproved,
		Fn<void(int)> resend)> send;

	// RoundVideo::Reader and RoundVideo::Convert when not set.
	OpenVideo open;
	ConvertVideo convert;

	// Starts converting as soon as the video is opened (UI snapshots).
	bool sendWhenOpened = false;
};

struct Converted {
	RoundVideo::Result result;
	crl::time from = 0;
	crl::time till = 0;
	float64 position = 0.;
	bool mute = false;
};

struct BoxState : base::has_weak_ptr {
	~BoxState() {
		if (cancelled) {
			*cancelled = true;
		}
	}

	RoundVideo::Source source;
	RoundVideo::Info info;
	OpenVideo openVideo;
	ConvertVideo convertVideo;
	Fn<SendCheck()> checkSend;
	Fn<bool(const RoundVideo::Result &, int, Fn<void(int)>)> sendResult;
	bool sendWhenOpened = false;
	std::optional<crl::object_on_queue<QueuedVideo>> video;
	int frameSide = 0;
	std::shared_ptr<std::atomic<int>> frameRequest
		= std::make_shared<std::atomic<int>>(0);
	std::shared_ptr<std::atomic<bool>> cancelled;
	std::optional<Converted> cached;
	Phase phase = Phase::Opening;
	crl::time from = 0;
	crl::time till = 0;
	float64 position = 0.5;
	bool mute = false;
	rpl::variable<QString> status;

	// Worker threads capture only the state and call these on main.
	Fn<void(RoundVideo::Info, QImage)> opened;
	Fn<void(RoundVideo::Result)> converted;
	Fn<void(int)> send;

	CropPreview *preview = nullptr;
	TrimSlider *trim = nullptr;
	Ui::MediaSlider *slider = nullptr;
	style::MediaSlider *sliderSt = nullptr; // Owned by the slider's parent.
	Ui::Checkbox *muteCheckbox = nullptr;
	Ui::SlideWrap<Ui::VerticalLayout> *editor = nullptr;
};

// Wider than the usual boxes for the preview, but not wider than a narrow
// window.
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

void RoundVideoBox(
		not_null<Ui::GenericBox*> box,
		RoundEditorArgs &&args) {
	const auto show = std::move(args.show);
	box->setTitle(tr::lng_oblivion_round_title());
	box->setWidth(EditorWidth(show));

	const auto state = box->lifetime().make_state<BoxState>();
	const auto weak = base::make_weak(state);
	const auto download = std::move(args.download);
	state->source = std::move(args.source);
	state->openVideo = std::move(args.open);
	state->convertVideo = args.convert
		? std::move(args.convert)
		: ConvertVideo(RoundVideo::Convert);
	state->checkSend = std::move(args.check);
	state->sendResult = std::move(args.send);
	state->sendWhenOpened = args.sendWhenOpened;
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
		// Locked controls look locked, grey as the frame of the strip,
		// not only ignore the mouse.
		if (state->slider) {
			const auto &normal = st::defaultContinuousSlider;
			state->sliderSt->activeFg = editable
				? normal.activeFg
				: st::windowSubTextFg;
			state->sliderSt->activeFgOver = editable
				? normal.activeFgOver
				: st::windowSubTextFg;
			state->slider->update();
		}
		if (state->muteCheckbox) {
			state->muteCheckbox->setDisabled(
				!editable || !state->info.hasAudio);
		}
		state->editor->setAttribute(
			Qt::WA_TransparentForMouseEvents,
			!editable);
		box->clearButtons();
		if (phase == Phase::Ready) {
			box->addButton(tr::lng_send_button(), [=] {
				// Sending rebuilds the buttons, not inside a button click.
				crl::on_main(weak, [=] {
					if (state->send) {
						state->send(0);
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
		state->preview->setStatus(QString());
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
		} else if (info.duration < RoundVideo::kMinDuration) {
			fail(tr::lng_oblivion_round_too_short(tr::now));
			return;
		}
		state->info = info;
		state->from = 0;
		state->till = std::min(info.duration, RoundVideo::kMaxDuration);
		state->mute = !info.hasAudio;
		state->preview->setVideoSize(info.size);
		state->preview->setFrame(std::move(frame));
		state->preview->setPosition(state->position);
		state->preview->setStatus(QString());
		state->preview->setProgress(std::nullopt);

		// The strip with its note, the crop position, the sound: groups
		// apart by a medium skip, a note right under what it explains.
		const auto container = state->editor->entity();
		Ui::AddSkip(container, st::boxLittleSkip);
		state->trim = container->add(
			object_ptr<TrimSlider>(container, info.duration),
			st::boxRowPadding);
		state->trim->setRange(state->from, state->till);
		state->trim->changes(
		) | rpl::on_next([=](const TrimSlider::Change &change) {
			state->from = change.from;
			state->till = change.till;
			refreshRange();
			requestFrame((change.edge == TrimSlider::Edge::End)
				? std::max(state->from, state->till - 100)
				: state->from);
		}, state->trim->lifetime());

		if (info.duration > RoundVideo::kMaxDuration) {
			Ui::AddSkip(container);
			container->add(
				object_ptr<Ui::FlatLabel>(
					container,
					tr::lng_oblivion_round_limit(),
					st::boxDividerLabel),
				st::boxRowPadding);
		}

		if (info.size.width() != info.size.height()) {
			Ui::AddSkip(container, st::boxMediumSkip);
			container->add(
				object_ptr<Ui::FlatLabel>(
					container,
					tr::lng_oblivion_round_position(),
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
			state->slider = slider;
			state->sliderSt = sliderSt;
			slider->setAlwaysDisplayMarker(true);
			slider->setValue(state->position);
			slider->setChangeProgressCallback([=](float64 value) {
				state->position = value;
				state->preview->setPosition(value);
			});
			state->preview->positionChanges(
			) | rpl::on_next([=](float64 value) {
				state->position = value;
				slider->setValue(value);
			}, slider->lifetime());
		}

		Ui::AddSkip(container, st::boxMediumSkip);
		const auto mute = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				tr::lng_oblivion_round_mute(tr::now),
				state->mute,
				st::defaultBoxCheckbox),
			st::boxRowPadding);
		if (!info.hasAudio) {
			mute->setDisabled(true);
		}
		state->muteCheckbox = mute;
		mute->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			state->mute = checked;
		}, mute->lifetime());

		state->editor->show(anim::type::normal);
		refreshRange();
		setPhase(Phase::Ready);
		if (state->sendWhenOpened) {
			state->send(0);
		}
	};

	const auto open = [=] {
		state->status = tr::lng_oblivion_round_opening(tr::now);
		state->preview->setStatus(QString());
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

	state->converted = [=](RoundVideo::Result result) {
		state->preview->setProgress(std::nullopt);
		state->preview->setStatus(QString());
		if (result.empty()) {
			refreshRange();
			setPhase(Phase::Ready);
			show->showToast(tr::lng_oblivion_round_convert_failed(tr::now));
			return;
		}
		state->cached = Converted{
			.result = std::move(result),
			.from = state->from,
			.till = state->till,
			.position = state->position,
			.mute = state->mute,
		};
		refreshRange();
		setPhase(Phase::Ready);
		state->send(0);
	};

	const auto convert = [=] {
		state->cached = std::nullopt;
		const auto cancelled = std::make_shared<std::atomic<bool>>(false);
		if (state->cancelled) {
			*state->cancelled = true;
		}
		state->cancelled = cancelled;
		state->status = tr::lng_oblivion_round_converting(
			tr::now,
			lt_percent,
			FormatPercent(0.));
		state->preview->setProgress(0.);
		state->preview->setStatus(FormatPercent(0.));
		setPhase(Phase::Converting);

		const auto request = RoundVideo::Request{
			.source = state->source,
			.from = state->from,
			.till = state->till,
			.position = state->position,
			.mute = state->mute,
		};
		const auto progress = [=](float64 value) {
			crl::on_main(weak, [=] {
				if (state->cancelled != cancelled
					|| state->phase != Phase::Converting) {
					return;
				}
				state->preview->setProgress(value);
				state->preview->setStatus(FormatPercent(value));
				state->status = tr::lng_oblivion_round_converting(
					tr::now,
					lt_percent,
					FormatPercent(value));
			});
		};
		const auto converter = state->convertVideo;
		crl::async([=] {
			auto result = converter(request, *cancelled, progress);
			crl::on_main(weak, [=, result = std::move(result)]() mutable {
				if (state->cancelled == cancelled) {
					state->converted(std::move(result));
				}
			});
		});
	};

	state->send = [=](int starsApproved) {
		if (state->phase != Phase::Ready) {
			return;
		}
		const auto check = state->checkSend
			? state->checkSend()
			: SendCheck::Ok;
		if (check == SendCheck::Close) {
			close();
			return;
		} else if (check == SendCheck::Wait) {
			return;
		}
		const auto &ready = state->cached;
		const auto actual = ready
			&& (ready->from == state->from)
			&& (ready->till == state->till)
			&& (ready->position == state->position)
			&& (ready->mute == state->mute);
		if (!actual) {
			convert();
			return;
		}
		const auto resend = crl::guard(weak, [=](int approved) {
			state->send(approved);
		});
		if (!state->sendResult
			|| !state->sendResult(ready->result, starsApproved, resend)) {
			return;
		}
		close();
	};

	box->boxClosing() | rpl::on_next([=] {
		if (state->cancelled) {
			*state->cancelled = true;
		}
	}, box->lifetime());

	if (!state->source.empty()) {
		open();
		return;
	} else if (!download) {
		fail(tr::lng_oblivion_round_open_failed(tr::now));
		return;
	}

	// A video from the chat: download it first.
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
			state->preview->setStatus(FormatPercent(progress));
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

// A video from the chat is downloaded into the temporary folder, the file
// is removed when the editor is closed.
void DownloadForRound(
		not_null<Ui::GenericBox*> box,
		not_null<DocumentData*> document,
		FullMsgId context,
		DownloadHandlers handlers) {
	struct State {
		~State() {
			if (!tempPath.isEmpty()) {
				// Destroyed without boxClosing().
				RemoveTempFile(tempPath, nullptr);
			}
		}

		std::shared_ptr<Data::DocumentMedia> media;
		QString tempPath; // Where we download the video ourselves.
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

	// Empty until the file is loaded.
	const auto loaded = [=] {
		if (!state->media->loaded(true)) {
			return RoundVideo::Source();
		}
		const auto path = document->filepath(true);
		return path.isEmpty()
			? RoundVideo::Source{ .content = state->media->bytes() }
			: RoundVideo::Source{ .path = path };
	};
	if (auto source = loaded(); !source.empty()) {
		handlers.done(std::move(source));
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
			// Nothing has started, no progress will come. The editor
			// ignores this when the progress above has already finished.
			done(loaded());
		}
	}
}

void ShowRoundBox(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		RoundVideo::Source source,
		DocumentData *document,
		FullMsgId context,
		FullReplyTo replyTo) {
	const auto show = controller->uiShow();
	const auto weakThread = base::make_weak(thread);
	const auto payment = std::make_shared<SendPaymentHelper>();
	const auto weakPayment = std::weak_ptr<SendPaymentHelper>(payment);
	auto args = RoundEditorArgs{
		.show = show,
		.source = std::move(source),
		.check = [=] {
			const auto strong = weakThread.get();
			return !strong
				? SendCheck::Close
				: CheckCanSendRound(show, strong)
				? SendCheck::Ok
				: SendCheck::Wait;
		},
		.send = [=](
				const RoundVideo::Result &result,
				int starsApproved,
				Fn<void(int)> resend) {
			const auto strong = weakThread.get();
			if (!strong) {
				return false;
			}
			const auto options = Api::SendOptions{
				.starsApproved = starsApproved,
			};
			const auto again = [=](int approved) {
				if (const auto helper = weakPayment.lock()) {
					helper->clear();
				}
				resend(approved);
			};
			if (!payment->check(
					controller,
					strong->peer(),
					options,
					1,
					again)) {
				return false;
			}
			SendRound(strong, replyTo, result, options);
			return true;
		},
	};
	if (document) {
		args.download = [=](
				not_null<Ui::GenericBox*> box,
				DownloadHandlers handlers) {
			DownloadForRound(box, document, context, std::move(handlers));
		};
	}
	controller->show(Box(RoundVideoBox, std::move(args)));
}

// UI snapshots (see oblivion_ui_snapshots.h): the editor with a painted
// landscape video, nothing is read from the disk, converted or sent.

constexpr auto kSceneWidth = 720;
constexpr auto kSampleDuration = crl::time(90'000);
constexpr auto kSamplePortraitDuration = crl::time(24'600);
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

// Time of the day: 0 is the dawn, 0.5 the noon, 1 the dusk.
[[nodiscard]] QColor DayColor(
		QColor dawn,
		QColor noon,
		QColor dusk,
		float64 time) {
	return (time < 0.5)
		? MixColors(dawn, noon, time * 2.)
		: MixColors(noon, dusk, (time - 0.5) * 2.);
}

// Sky, sun, clouds, mountains and hills: the sun crosses the sky once
// over the whole video, so every thumbnail is different.
void PaintSampleFrame(QPainter &p, QSizeF size, float64 time) {
	const auto w = size.width();
	const auto h = size.height();
	const auto unit = std::min(w, h);
	const auto light = std::sin(M_PI * time);
	const auto point = [&](float64 x, float64 y) {
		return QPointF(x * w, y * h);
	};
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);

	auto sky = QLinearGradient(0., 0., 0., h * 0.75);
	sky.setColorAt(0., DayColor(
		QColor(0x2E, 0x3A, 0x6E),
		QColor(0x2F, 0x86, 0xD6),
		QColor(0x3B, 0x2A, 0x5A),
		time));
	sky.setColorAt(1., DayColor(
		QColor(0xF4, 0xA2, 0x61),
		QColor(0xC4, 0xE6, 0xF8),
		QColor(0xF2, 0x6B, 0x5B),
		time));
	p.fillRect(QRectF(QPointF(), size), sky);

	const auto sun = point(0.3 + 0.4 * time, 0.46 - 0.3 * light);
	const auto radius = unit * 0.08;
	const auto glowRadius = radius * 3.5;
	auto glow = QRadialGradient(sun, glowRadius);
	glow.setColorAt(0., QColor(0xFF, 0xE8, 0xA0, 110));
	glow.setColorAt(1., QColor(0xFF, 0xE8, 0xA0, 0));
	p.setBrush(glow);
	p.drawEllipse(sun, glowRadius, glowRadius);
	p.setBrush(MixColors(
		QColor(0xFF, 0xA8, 0x4C),
		QColor(0xFF, 0xF4, 0xC2),
		light));
	p.drawEllipse(sun, radius, radius);

	p.setBrush(QColor(0xFF, 0xFF, 0xFF, 190));
	const auto cloud = [&](float64 x, float64 y, float64 scale) {
		const auto center = point(x, y);
		const auto r = unit * 0.06 * scale;
		p.drawEllipse(center, r * 1.6, r * 0.9);
		p.drawEllipse(center + QPointF(r * 1.2, r * 0.2), r * 1.2, r * 0.75);
		p.drawEllipse(center + QPointF(-r * 1.2, r * 0.25), r * 1.1, r * 0.65);
	};
	cloud(0.12 + 0.3 * time, 0.18, 1.);
	cloud(0.62 + 0.25 * time, 0.1, 0.8);

	const float64 ridge[] = {
		0., 0.62,
		0.12, 0.42,
		0.24, 0.55,
		0.38, 0.34,
		0.52, 0.52,
		0.66, 0.38,
		0.8, 0.5,
		0.9, 0.4,
		1., 0.52,
	};
	auto mountains = QPolygonF();
	for (auto i = 0; i + 1 < int(std::size(ridge)); i += 2) {
		mountains.push_back(point(ridge[i], ridge[i + 1]));
	}
	mountains.push_back(point(1., 1.));
	mountains.push_back(point(0., 1.));
	p.setBrush(DayColor(
		QColor(0x4A, 0x55, 0x80),
		QColor(0x7B, 0x93, 0xB8),
		QColor(0x5A, 0x4A, 0x78),
		time));
	p.drawPolygon(mountains);

	auto hills = QPainterPath(point(0., 0.74));
	hills.cubicTo(point(0.25, 0.62), point(0.45, 0.8), point(0.7, 0.68));
	hills.cubicTo(point(0.82, 0.62), point(0.92, 0.66), point(1., 0.7));
	hills.lineTo(point(1., 1.));
	hills.lineTo(point(0., 1.));
	hills.closeSubpath();
	p.setBrush(DayColor(
		QColor(0x2E, 0x55, 0x3A),
		QColor(0x4F, 0x9A, 0x52),
		QColor(0x33, 0x4A, 0x3A),
		time));
	p.drawPath(hills);

	auto field = QPainterPath(point(0., 0.9));
	field.cubicTo(point(0.3, 0.82), point(0.6, 0.94), point(1., 0.84));
	field.lineTo(point(1., 1.));
	field.lineTo(point(0., 1.));
	field.closeSubpath();
	p.setBrush(DayColor(
		QColor(0x1E, 0x3A, 0x26),
		QColor(0x3A, 0x7A, 0x3E),
		QColor(0x22, 0x33, 0x28),
		time));
	p.drawPath(field);
}

// A synthetic video: frames are painted on request, instantly and always
// the same.
class SampleVideo final : public EditorVideo {
public:
	SampleVideo(QSize size, crl::time duration, bool audio)
	: _info(RoundVideo::Info{
		.duration = duration,
		.size = size,
		.hasAudio = audio,
	}) {
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
		// Center-cropped squares, as RoundVideo::Reader makes them.
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

// The editor is there with its thumbnails (and the progress), nothing is
// sliding anymore.
[[nodiscard]] bool SampleShown(not_null<QWidget*> box, bool converting) {
	const auto trims = FindWidgets<TrimSlider>(box);
	const auto previews = FindWidgets<CropPreview>(box);
	if (trims.empty()
		|| previews.empty()
		|| !trims.front()->hasThumbnails()
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
		crl::time duration,
		bool audio,
		bool converting) {
	auto args = RoundEditorArgs{
		.show = std::move(show),
		.source = RoundVideo::Source{ .content = QByteArray("sample") },
		.check = [] {
			return SendCheck::Ok;
		},
		.send = [](const RoundVideo::Result &, int, Fn<void(int)>) {
			return false;
		},
		.open = [=](RoundVideo::Source) -> std::unique_ptr<EditorVideo> {
			return std::make_unique<SampleVideo>(size, duration, audio);
		},
		.sendWhenOpened = converting,
	};
	if (converting) {
		// Stays at the same progress until the box is destroyed.
		args.convert = [](
				const RoundVideo::Request &,
				const std::atomic<bool> &cancelled,
				Fn<void(float64)> progress) {
			progress(kSampleProgress);
			while (!cancelled) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return RoundVideo::Result();
		};
	}
	return Box(RoundVideoBox, std::move(args));
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto scene = [](
			QString name,
			QSize size,
			crl::time duration,
			bool audio,
			bool converting) {
		RegisterScene(SceneDescriptor{
			.name = std::move(name),
			.size = QSize(style::ConvertScale(kSceneWidth), 0),
			.box = [=](std::shared_ptr<Ui::Show> show) {
				return SampleBox(
					std::move(show),
					size,
					duration,
					audio,
					converting);
			},
			.ready = [=](not_null<QWidget*> widget) {
				return SampleShown(widget, converting);
			},
		});
	};

	// 16:9, 1:30 with sound: the 60 seconds limit note, the crop slider.
	scene(u"round_editor"_q, QSize(1280, 720), kSampleDuration, true, false);

	// 9:16, 0:24.6 without sound: no limit note, "without sound" is on
	// and disabled, the tallest preview.
	scene(
		u"round_editor_portrait"_q,
		QSize(720, 1280),
		kSamplePortraitDuration,
		false,
		false);

	// Converting: the progress ring and percent over the preview, the
	// editor locked.
	scene(u"round_progress"_q, QSize(1280, 720), kSampleDuration, true, true);
});

} // namespace

void ShowVideoToRound(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		const QString &path,
		FullReplyTo replyTo) {
	ShowRoundBox(
		controller,
		thread,
		RoundVideo::Source{ .path = path },
		nullptr,
		FullMsgId(),
		replyTo);
}

void ShowVideoContentToRound(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		const QByteArray &content,
		FullReplyTo replyTo) {
	ShowRoundBox(
		controller,
		thread,
		RoundVideo::Source{ .content = content },
		nullptr,
		FullMsgId(),
		replyTo);
}

void ShowDocumentToRound(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread,
		not_null<DocumentData*> document,
		FullMsgId context) {
	ShowRoundBox(
		controller,
		thread,
		RoundVideo::Source(),
		document,
		context,
		FullReplyTo());
}

void ChooseVideoToRound(
		not_null<Window::SessionController*> controller,
		not_null<Data::Thread*> thread) {
	const auto weak = base::make_weak(thread);
	const auto filter = tr::lng_oblivion_round_filter_video(tr::now)
		+ u" (*.mp4 *.mov *.m4v *.mkv *.webm *.gif);;"_q
		+ tr::lng_oblivion_round_filter_all(tr::now)
		+ u" (*)"_q;
	FileDialog::GetOpenPath(
		controller->widget().get(),
		tr::lng_oblivion_round_choose(tr::now),
		filter,
		crl::guard(controller, [=](FileDialog::OpenResult &&result) {
			const auto thread = weak.get();
			if (!thread) {
				return;
			} else if (!result.paths.isEmpty()) {
				ShowVideoToRound(controller, thread, result.paths.front());
			} else if (!result.remoteContent.isEmpty()) {
				ShowVideoContentToRound(
					controller,
					thread,
					result.remoteContent);
			}
		}));
}

void AddSendAsRoundAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<PeerData*> peer,
		const SendMenu::Details &details,
		Api::SendType sendType,
		const Ui::PreparedList &list,
		FullReplyTo replyTo,
		Fn<void()> closeBox) {
	// Only boxes opened from a chat's compose area know their chat:
	// not scheduled messages, business shortcuts or story replies.
	if (sendType != Api::SendType::Normal
		|| details.barePeerId != peer->id.value
		|| list.files.size() != 1) {
		return;
	}
	const auto &file = list.files.front();
	if (file.type != Ui::PreparedFile::Type::Video
		|| (file.path.isEmpty() && file.content.isEmpty())) {
		return;
	}
	const auto path = file.path;
	const auto content = file.content;
	const auto topicRootId = MsgId(details.bareTopicRootId);
	menu->addAction(tr::lng_oblivion_round_send_as(tr::now), [=] {
		const auto window = show->resolveWindow();
		if (!window || (&window->session() != &peer->session())) {
			return;
		}
		const auto thread = ResolveComposeThread(window, peer, topicRootId);
		if (!thread || !CheckCanSendRound(show, thread)) {
			return;
		}
		if (closeBox) {
			closeBox();
		}
		ShowRoundBox(
			window,
			thread,
			RoundVideo::Source{ .path = path, .content = content },
			nullptr,
			FullMsgId(),
			replyTo);
	}, &st::menuIconVideoChat);
}

void AddVideoToRoundAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document) {
	const auto video = document->isVideoFile()
		|| document->isAnimation()
		|| document->mimeString().startsWith(
			u"video/"_q,
			Qt::CaseInsensitive);
	if (!video || document->isVideoMessage() || document->sticker()) {
		return;
	}
	const auto thread = ItemThread(item);
	if (!Data::CanSendAnything(thread)) {
		return;
	}
	const auto weak = base::make_weak(thread);
	const auto context = item->fullId();
	menu->addAction(tr::lng_oblivion_round_context(tr::now), [=] {
		const auto strong = weak.get();
		if (!strong || !CheckCanSendRound(controller->uiShow(), strong)) {
			return;
		}
		ShowDocumentToRound(controller, strong, document, context);
	}, &st::menuIconVideoChat);
}

} // namespace Oblivion
