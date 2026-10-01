/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_editor_canvas.h"

#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "ui/effects/animation_value.h"
#include "ui/painter.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"
#include "styles/style_media_view.h"
#include "styles/style_widgets.h"

#include <QtGui/QNativeGestureEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QWheelEvent>

namespace Oblivion::Photo::EditorUi {
namespace {

constexpr auto kMargin = 20;
constexpr auto kCropMargin = 36;
constexpr auto kMaxFitScale = 4.;
constexpr auto kMaxDevicePixelsPerPixel = 8.;
constexpr auto kZoomStep = 1.25;
constexpr auto kWheelZoomPerUnit = 0.0018;
constexpr auto kWheelPanStep = 40;
constexpr auto kHandleHit = 18;
constexpr auto kEdgeHit = 10;
constexpr auto kCornerLength = 18;
constexpr auto kCornerThickness = 3;
constexpr auto kMinCropSide = 40;
constexpr auto kCheckerCell = 8;
constexpr auto kBadgeTop = 16;
constexpr auto kBadgePadding = 14;
constexpr auto kBadgeHeight = 28;
constexpr auto kSpinnerSize = 36;
constexpr auto kSpinnerLine = 3;
constexpr auto kSmoothLimit = 2.5;
constexpr auto kZoomControlsMargin = 12;
constexpr auto kZoomButton = 32;
constexpr auto kZoomLabelWidth = 58;
constexpr auto kZoomSignLength = 10;
constexpr auto kGridThirds = 3;
constexpr auto kGridFine = 9;

[[nodiscard]] QRectF ClampInside(QRectF rect, QSizeF frame) {
	const auto x = std::clamp(
		rect.x(),
		0.,
		std::max(frame.width() - rect.width(), 0.));
	const auto y = std::clamp(
		rect.y(),
		0.,
		std::max(frame.height() - rect.height(), 0.));
	return QRectF(QPointF(x, y), rect.size());
}

// Resizes the crop rectangle (in frame pixels) by the dragged part.
// Free: the dragged edges follow the pointer, clamped to the frame and
// the minimal size. With a ratio: a dragged corner keeps the opposite
// corner in place and picks the larger of the two pointer-driven sizes,
// then shrinks to fit the frame; a dragged edge keeps the center of the
// other axis, so the rectangle grows symmetrically there.
[[nodiscard]] QRectF ResizeCrop(
		int part,
		QRectF start,
		QPointF delta,
		QSizeF frame,
		std::optional<float64> ratio,
		float64 minSide) {
	enum Part {
		Left = 1,
		Top = 2,
		Right = 4,
		Bottom = 8,
	};
	const auto left = (part & Left) != 0;
	const auto top = (part & Top) != 0;
	const auto right = (part & Right) != 0;
	const auto bottom = (part & Bottom) != 0;
	const auto fw = frame.width();
	const auto fh = frame.height();
	minSide = std::min({ minSide, fw, fh });
	if (!ratio || *ratio <= 0.) {
		auto l = start.left();
		auto t = start.top();
		auto r = start.right();
		auto b = start.bottom();
		// The minimal side grows when the view is scaled down, a crop made
		// smaller before can't shrink further, but it must not jump.
		const auto minW = std::min(minSide, start.width());
		const auto minH = std::min(minSide, start.height());
		if (left) {
			l = std::clamp(l + delta.x(), 0., std::max(r - minW, 0.));
		} else if (right) {
			r = std::clamp(r + delta.x(), std::min(l + minW, fw), fw);
		}
		if (top) {
			t = std::clamp(t + delta.y(), 0., std::max(b - minH, 0.));
		} else if (bottom) {
			b = std::clamp(b + delta.y(), std::min(t + minH, fh), fh);
		}
		return QRectF(QPointF(l, t), QPointF(r, b));
	}
	const auto k = *ratio;
	const auto minW = std::max(minSide, minSide * k);
	const auto fitW = [&](float64 w, float64 maxW, float64 maxH) {
		w = std::min(w, maxW);
		if (w / k > maxH) {
			w = maxH * k;
		}
		return std::max(w, std::min({ minW, maxW, maxH * k }));
	};
	if ((left || right) && (top || bottom)) {
		const auto anchor = QPointF(
			left ? start.right() : start.left(),
			top ? start.bottom() : start.top());
		const auto moving = QPointF(
			(left ? start.left() : start.right()) + delta.x(),
			(top ? start.top() : start.bottom()) + delta.y());
		const auto w = std::max(
			left ? (anchor.x() - moving.x()) : (moving.x() - anchor.x()),
			0.);
		const auto h = std::max(
			top ? (anchor.y() - moving.y()) : (moving.y() - anchor.y()),
			0.);
		const auto maxW = left ? anchor.x() : (fw - anchor.x());
		const auto maxH = top ? anchor.y() : (fh - anchor.y());
		const auto width = fitW(std::max(w, h * k), maxW, maxH);
		const auto height = width / k;
		return QRectF(
			left ? (anchor.x() - width) : anchor.x(),
			top ? (anchor.y() - height) : anchor.y(),
			width,
			height);
	} else if (left || right) {
		const auto anchor = left ? start.right() : start.left();
		const auto w = left
			? (start.width() - delta.x())
			: (start.width() + delta.x());
		const auto maxW = left ? anchor : (fw - anchor);
		const auto width = fitW(w, maxW, fh);
		const auto height = width / k;
		const auto y = std::clamp(
			start.center().y() - height / 2.,
			0.,
			std::max(fh - height, 0.));
		return QRectF(left ? (anchor - width) : anchor, y, width, height);
	}
	const auto anchor = top ? start.bottom() : start.top();
	const auto h = top
		? (start.height() - delta.y())
		: (start.height() + delta.y());
	const auto maxH = top ? anchor : (fh - anchor);
	const auto width = fitW(h * k, fw, maxH);
	const auto height = width / k;
	const auto x = std::clamp(
		start.center().x() - width / 2.,
		0.,
		std::max(fw - width, 0.));
	return QRectF(x, top ? (anchor - height) : anchor, width, height);
}

} // namespace

class Canvas::ZoomControls final : public Ui::RpWidget {
public:
	ZoomControls(
		QWidget *parent,
		rpl::producer<float64> percent,
		Fn<void()> zoomOut,
		Fn<void()> toggle,
		Fn<void()> zoomIn);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect partRect(int part) const;
	[[nodiscard]] int partAt(QPoint point) const;

	QString _text;
	int _over = -1;
	int _pressed = -1;
	const Fn<void()> _zoomOut;
	const Fn<void()> _toggle;
	const Fn<void()> _zoomIn;

};

Canvas::ZoomControls::ZoomControls(
	QWidget *parent,
	rpl::producer<float64> percent,
	Fn<void()> zoomOut,
	Fn<void()> toggle,
	Fn<void()> zoomIn)
: RpWidget(parent)
, _zoomOut(std::move(zoomOut))
, _toggle(std::move(toggle))
, _zoomIn(std::move(zoomIn)) {
	resize(Px(2 * kZoomButton + kZoomLabelWidth), Px(kZoomButton));
	setMouseTracking(true);
	std::move(percent) | rpl::on_next([=](float64 value) {
		_text = QString::number(int(std::round(value))) + '%';
		update();
	}, lifetime());
}

QRect Canvas::ZoomControls::partRect(int part) const {
	const auto button = Px(kZoomButton);
	const auto label = Px(kZoomLabelWidth);
	switch (part) {
	case 0: return QRect(0, 0, button, height());
	case 1: return QRect(button, 0, label, height());
	case 2: return QRect(button + label, 0, button, height());
	}
	return QRect();
}

int Canvas::ZoomControls::partAt(QPoint point) const {
	for (auto i = 0; i != 3; ++i) {
		if (partRect(i).contains(point)) {
			return i;
		}
	}
	return -1;
}

void Canvas::ZoomControls::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(anim::with_alpha(st::groupCallMembersBg->c, 0.94));
	p.drawRoundedRect(rect(), radius, radius);
	if (_over == 0 || _over == 2) {
		p.setBrush(st::groupCallMembersBgRipple);
		p.drawEllipse(partRect(_over));
	} else if (_over == 1) {
		p.setBrush(st::groupCallMembersBgOver);
		p.drawRect(partRect(1));
	}
	const auto fg = st::groupCallMembersFg->c;
	const auto line = std::max(Px(2), 1);
	const auto length = Px(kZoomSignLength);
	p.setBrush(fg);
	for (const auto part : { 0, 2 }) {
		const auto center = QRectF(partRect(part)).center();
		p.drawRoundedRect(
			QRectF(
				center.x() - length / 2.,
				center.y() - line / 2.,
				length,
				line),
			line / 2.,
			line / 2.);
		if (part == 2) {
			p.drawRoundedRect(
				QRectF(
					center.x() - line / 2.,
					center.y() - length / 2.,
					line,
					length),
				line / 2.,
				line / 2.);
		}
	}
	p.setPen(fg);
	p.setFont(st::semiboldFont);
	p.drawText(partRect(1), Qt::AlignCenter, _text);
}

void Canvas::ZoomControls::mouseMoveEvent(QMouseEvent *e) {
	const auto over = partAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void Canvas::ZoomControls::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = partAt(e->pos());
	}
}

void Canvas::ZoomControls::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed < 0 || pressed != partAt(e->pos())) {
		return;
	}
	const auto &callback = (pressed == 0)
		? _zoomOut
		: (pressed == 1)
		? _toggle
		: _zoomIn;
	if (callback) {
		callback();
	}
}

void Canvas::ZoomControls::mouseDoubleClickEvent(QMouseEvent *e) {
	mousePressEvent(e);
}

void Canvas::ZoomControls::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

Canvas::Canvas(QWidget *parent)
: RpWidget(parent)
, _radial([=] { update(); }, st::defaultInfiniteRadialAnimation) {
	setMouseTracking(true);
	_zoomControls = Ui::CreateChild<ZoomControls>(
		this,
		_zoomPercent.value(),
		[=] { zoomOut(); },
		[=] { toggleZoom(QRectF(rect()).center()); },
		[=] { zoomIn(); });
	_zoomControls->hide();
}

Canvas::~Canvas() = default;

void Canvas::setFrameSize(QSize size) {
	if (_frame == size) {
		return;
	}
	const auto sameAspect = !_frame.isEmpty()
		&& !size.isEmpty()
		&& (std::abs(
			_frame.width() / float64(_frame.height())
				- size.width() / float64(size.height())) < 1e-3);
	_frame = size;
	if (!sameAspect) {
		_zoom = 1.;
		_pan = QPointF();
		_image = QImage();
		_before = QImage();
	}
	clampPan();
	viewUpdated();
}

QSize Canvas::frameSize() const {
	return _frame;
}

void Canvas::setImage(QImage image) {
	_image = std::move(image);
	updateZoomControls();
	update();
}

void Canvas::setBeforeImage(QImage image) {
	_before = std::move(image);
	if (_comparing) {
		update();
	}
}

void Canvas::clearImages() {
	_image = QImage();
	_before = QImage();
	updateZoomControls();
	update();
}

bool Canvas::hasImage() const {
	return !_image.isNull();
}

void Canvas::setComparing(bool comparing) {
	if (_comparing != comparing) {
		_comparing = comparing;
		update();
	}
}

bool Canvas::comparing() const {
	return _comparing;
}

void Canvas::setCheckerboard(bool enabled) {
	if (_checkerboard != enabled) {
		_checkerboard = enabled;
		update();
	}
}

void Canvas::setLoading(bool loading) {
	if (_loading == loading) {
		return;
	}
	_loading = loading;
	if (loading) {
		_radial.start();
	} else {
		_radial.stop(anim::type::instant);
	}
	update();
}

void Canvas::setCropMode(bool enabled) {
	if (_cropMode == enabled) {
		return;
	}
	_cropMode = enabled;
	_drag = Drag::None;
	_zoom = 1.;
	_pan = QPointF();
	updateZoomControls();
	viewUpdated();
}

bool Canvas::cropMode() const {
	return _cropMode;
}

void Canvas::setCrop(QRectF crop) {
	if (_crop != crop) {
		_crop = crop;
		if (_cropMode) {
			update();
		}
	}
}

void Canvas::setCropRatio(std::optional<float64> ratio) {
	_cropRatio = ratio;
}

void Canvas::setGridVisible(bool visible) {
	if (_gridVisible != visible) {
		_gridVisible = visible;
		update();
	}
}

bool Canvas::cropDragging() const {
	return _drag != Drag::None && _drag != Drag::Pan;
}

rpl::producer<QRectF> Canvas::cropChanges() const {
	return _cropChanges.events();
}

rpl::producer<> Canvas::cropFinished() const {
	return _cropFinished.events();
}

void Canvas::setPanMode(bool enabled) {
	if (_panMode != enabled) {
		_panMode = enabled;
		if (_drag == Drag::None) {
			updateCursor(mapFromGlobal(QCursor::pos()));
		}
	}
}

QRectF Canvas::contentRect() const {
	const auto margin = Px(_cropMode ? kCropMargin : kMargin);
	// Outside the crop mode the band of the zoom controls is kept free,
	// so the fitted photo never touches or hides under them (a narrow
	// window used to put the controls right on the bottom photo edge).
	const auto bottom = _cropMode
		? margin
		: std::max(margin, Px(kZoomButton + 2 * kZoomControlsMargin));
	return QRectF(rect()).marginsRemoved(
		QMarginsF(margin, margin, margin, bottom));
}

float64 Canvas::fitScale() const {
	if (_frame.isEmpty()) {
		return 1.;
	}
	const auto area = contentRect();
	if (area.width() <= 0. || area.height() <= 0.) {
		return 1. / 64.;
	}
	const auto scale = std::min(
		area.width() / _frame.width(),
		area.height() / _frame.height());
	return std::min(scale, kMaxFitScale);
}

float64 Canvas::scale() const {
	return fitScale() * (_cropMode ? 1. : _zoom);
}

float64 Canvas::maxZoom() const {
	const auto ratio = style::DevicePixelRatio();
	const auto limit = kMaxDevicePixelsPerPixel / ratio;
	return std::max(1., limit / fitScale());
}

float64 Canvas::actualZoom() const {
	return 1. / (style::DevicePixelRatio() * fitScale());
}

QRectF Canvas::imageRect() const {
	if (_frame.isEmpty()) {
		return QRectF();
	}
	const auto scale = this->scale();
	const auto size = QSizeF(_frame) * scale;
	const auto center = contentRect().center() + _pan;
	return QRectF(
		center.x() - size.width() / 2.,
		center.y() - size.height() / 2.,
		size.width(),
		size.height());
}

QRectF Canvas::cropFrameRect() const {
	return QRectF(
		_crop.x() * _frame.width(),
		_crop.y() * _frame.height(),
		_crop.width() * _frame.width(),
		_crop.height() * _frame.height());
}

QRectF Canvas::cropWidgetRect() const {
	const auto image = imageRect();
	const auto scale = this->scale();
	const auto crop = cropFrameRect();
	return QRectF(
		image.x() + crop.x() * scale,
		image.y() + crop.y() * scale,
		crop.width() * scale,
		crop.height() * scale);
}

bool Canvas::pannable() const {
	if (_cropMode || _frame.isEmpty()) {
		return false;
	}
	const auto image = imageRect();
	return (image.width() > width() + 0.5)
		|| (image.height() > height() + 0.5);
}

QSize Canvas::fitPixels() const {
	if (_frame.isEmpty()) {
		return QSize();
	}
	const auto ratio = style::DevicePixelRatio();
	const auto scale = std::min(fitScale() * ratio, 1.);
	return QSize(
		std::max(int(std::ceil(_frame.width() * scale)), 1),
		std::max(int(std::ceil(_frame.height() * scale)), 1));
}

QSize Canvas::renderPixels() const {
	if (_frame.isEmpty()) {
		return QSize();
	}
	const auto ratio = style::DevicePixelRatio();
	const auto scale = std::min(this->scale() * ratio, 1.);
	return QSize(
		std::max(int(std::ceil(_frame.width() * scale)), 1),
		std::max(int(std::ceil(_frame.height() * scale)), 1));
}

rpl::producer<> Canvas::viewChanges() const {
	return _viewChanges.events();
}

void Canvas::zoomIn() {
	setZoom(_zoom * kZoomStep, QRectF(rect()).center());
}

void Canvas::zoomOut() {
	setZoom(_zoom / kZoomStep, QRectF(rect()).center());
}

void Canvas::zoomFit() {
	_zoom = 1.;
	_pan = QPointF();
	viewUpdated();
}

void Canvas::zoomActual() {
	setZoom(actualZoom(), QRectF(rect()).center());
}

bool Canvas::zoomed() const {
	return !_cropMode && _zoom > 1.001;
}

rpl::producer<float64> Canvas::zoomPercentValue() const {
	return _zoomPercent.value();
}

void Canvas::setZoom(float64 zoom, QPointF anchor) {
	if (_cropMode || _frame.isEmpty()) {
		return;
	}
	zoom = std::clamp(zoom, 1., maxZoom());
	if (std::abs(zoom - _zoom) < 1e-6) {
		return;
	}
	const auto was = imageRect();
	const auto wasScale = scale();
	const auto point = (anchor - was.topLeft()) / wasScale;
	_zoom = zoom;
	const auto nowScale = scale();
	const auto size = QSizeF(_frame) * nowScale;
	const auto topLeft = anchor - point * nowScale;
	const auto center = topLeft + QPointF(size.width(), size.height()) / 2.;
	_pan = center - contentRect().center();
	clampPan();
	viewUpdated();
}

void Canvas::toggleZoom(QPointF anchor) {
	if (zoomed()) {
		zoomFit();
	} else {
		setZoom(std::max(actualZoom(), 2.), anchor);
	}
}

void Canvas::panBy(QPointF delta) {
	if (!pannable()) {
		return;
	}
	_pan += delta;
	clampPan();
	update();
}

void Canvas::clampPan() {
	if (_frame.isEmpty()) {
		_pan = QPointF();
		return;
	}
	const auto size = QSizeF(_frame) * scale();
	const auto center = contentRect().center();
	// The content area is not centered in the widget (the zoom controls
	// band at the bottom), so the limits are computed for the image edges:
	// a photo larger than the widget can be panned edge to edge, a smaller
	// one stays centered in the content area and inside the widget.
	const auto clampAxis = [](
			float64 pan,
			float64 center,
			float64 size,
			float64 outer) {
		const auto centered = center - size / 2.;
		const auto start = (size <= outer)
			? std::clamp(centered, 0., outer - size)
			: std::clamp(centered + pan, outer - size, 0.);
		return start - centered;
	};
	_pan = QPointF(
		clampAxis(_pan.x(), center.x(), size.width(), width()),
		clampAxis(_pan.y(), center.y(), size.height(), height()));
}

void Canvas::viewUpdated() {
	clampPan();
	_zoomPercent = scale() * style::DevicePixelRatio() * 100.;
	updateZoomControls();
	update();
	_viewChanges.fire({});
}

void Canvas::updateZoomControls() {
	const auto margin = Px(kZoomControlsMargin);
	_zoomControls->move(
		margin,
		height() - _zoomControls->height() - margin);
	// The fitted photo leaves their band free (see contentRect), so they
	// cover it only when it is zoomed in or panned under them: then they
	// are shown only while the mouse is over the canvas or the photo is
	// zoomed in, otherwise they always stay visible.
	const auto covers = imageRect().intersects(
		QRectF(_zoomControls->geometry()));
	const auto visible = !_cropMode
		&& !_image.isNull()
		&& !_frame.isEmpty()
		&& (!covers || _hovered || zoomed());
	if (visible) {
		_zoomControls->raise();
	}
	_zoomControls->setVisible(visible);
}

void Canvas::resizeEvent(QResizeEvent *e) {
	viewUpdated();
}

bool Canvas::eventHook(QEvent *e) {
	if (e->type() == QEvent::NativeGesture) {
		const auto gesture = static_cast<QNativeGestureEvent*>(e);
		if (!_cropMode && !_frame.isEmpty()) {
			if (gesture->gestureType() == Qt::ZoomNativeGesture) {
				setZoom(_zoom * (1. + gesture->value()), gesture->position());
				return true;
			} else if (gesture->gestureType()
				== Qt::SmartZoomNativeGesture) {
				toggleZoom(gesture->position());
				return true;
			}
		}
	}
	return RpWidget::eventHook(e);
}

void Canvas::wheelEvent(QWheelEvent *e) {
	if (_cropMode || _frame.isEmpty()) {
		e->ignore();
		return;
	}
	const auto angle = e->angleDelta();
	if (e->modifiers() & Qt::ControlModifier) {
		const auto delta = angle.y() ? angle.y() : angle.x();
		if (delta) {
			setZoom(
				_zoom * std::exp(delta * kWheelZoomPerUnit),
				e->position());
		}
		e->accept();
		return;
	}
	if (!pannable()) {
		e->ignore();
		return;
	}
	const auto pixel = e->pixelDelta();
	const auto delta = !pixel.isNull()
		? QPointF(pixel)
		: (QPointF(angle) * Px(kWheelPanStep) / 120.);
	panBy(delta);
	e->accept();
}

Canvas::Drag Canvas::cropHitTest(QPointF point) const {
	if (!_cropMode || _frame.isEmpty()) {
		return Drag::None;
	}
	const auto crop = cropWidgetRect();
	const auto handle = Px(kHandleHit);
	const auto atCorner = [&](QPointF corner) {
		return std::abs(point.x() - corner.x()) <= handle
			&& std::abs(point.y() - corner.y()) <= handle;
	};
	if (atCorner(crop.topLeft())) {
		return Drag::TopLeft;
	} else if (atCorner(crop.topRight())) {
		return Drag::TopRight;
	} else if (atCorner(crop.bottomLeft())) {
		return Drag::BottomLeft;
	} else if (atCorner(crop.bottomRight())) {
		return Drag::BottomRight;
	}
	const auto edge = Px(kEdgeHit);
	const auto withinX = point.x() >= crop.left() - edge
		&& point.x() <= crop.right() + edge;
	const auto withinY = point.y() >= crop.top() - edge
		&& point.y() <= crop.bottom() + edge;
	if (withinY && std::abs(point.x() - crop.left()) <= edge) {
		return Drag::Left;
	} else if (withinY && std::abs(point.x() - crop.right()) <= edge) {
		return Drag::Right;
	} else if (withinX && std::abs(point.y() - crop.top()) <= edge) {
		return Drag::Top;
	} else if (withinX && std::abs(point.y() - crop.bottom()) <= edge) {
		return Drag::Bottom;
	} else if (crop.contains(point)) {
		return Drag::Move;
	}
	return Drag::None;
}

void Canvas::updateCursor(QPointF point) {
	if (_panMode) {
		setCursor((_drag == Drag::Pan)
			? Qt::ClosedHandCursor
			: Qt::OpenHandCursor);
		return;
	}
	const auto drag = (_drag != Drag::None) ? _drag : cropHitTest(point);
	switch (drag) {
	case Drag::Move: setCursor(Qt::SizeAllCursor); return;
	case Drag::Left:
	case Drag::Right: setCursor(Qt::SizeHorCursor); return;
	case Drag::Top:
	case Drag::Bottom: setCursor(Qt::SizeVerCursor); return;
	case Drag::TopLeft:
	case Drag::BottomRight: setCursor(Qt::SizeFDiagCursor); return;
	case Drag::TopRight:
	case Drag::BottomLeft: setCursor(Qt::SizeBDiagCursor); return;
	case Drag::Pan: setCursor(Qt::ClosedHandCursor); return;
	case Drag::None: break;
	}
	setCursor(pannable() ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void Canvas::mousePressEvent(QMouseEvent *e) {
	const auto point = e->position();
	if (e->button() == Qt::MiddleButton
		|| (e->button() == Qt::LeftButton && _panMode)) {
		if (pannable()) {
			_drag = Drag::Pan;
		}
	} else if (e->button() == Qt::LeftButton) {
		_drag = _cropMode
			? cropHitTest(point)
			: pannable()
			? Drag::Pan
			: Drag::None;
	}
	_dragStart = point;
	_dragPan = _pan;
	_dragCrop = cropFrameRect();
	updateCursor(point);
	if (cropDragging()) {
		update();
	}
}

void Canvas::mouseMoveEvent(QMouseEvent *e) {
	const auto point = e->position();
	if (_drag == Drag::Pan) {
		_pan = _dragPan + (point - _dragStart);
		clampPan();
		update();
	} else if (cropDragging()) {
		updateCropDrag(point, e->modifiers());
	} else {
		updateCursor(point);
	}
}

void Canvas::updateCropDrag(
		QPointF point,
		Qt::KeyboardModifiers modifiers) {
	const auto scale = this->scale();
	if (_frame.isEmpty() || scale <= 0.) {
		return;
	}
	const auto frame = QSizeF(_frame);
	const auto delta = (point - _dragStart) / scale;
	auto result = QRectF();
	if (_drag == Drag::Move) {
		result = ClampInside(_dragCrop.translated(delta), frame);
	} else {
		const auto part = [&] {
			switch (_drag) {
			case Drag::Left: return 1;
			case Drag::Top: return 2;
			case Drag::Right: return 4;
			case Drag::Bottom: return 8;
			case Drag::TopLeft: return 1 | 2;
			case Drag::TopRight: return 4 | 2;
			case Drag::BottomLeft: return 1 | 8;
			case Drag::BottomRight: return 4 | 8;
			default: return 0;
			}
		}();
		auto ratio = _cropRatio;
		if (!ratio
			&& (modifiers & Qt::ShiftModifier)
			&& _dragCrop.height() > 0.) {
			ratio = _dragCrop.width() / _dragCrop.height();
		}
		result = ResizeCrop(
			part,
			_dragCrop,
			delta,
			frame,
			ratio,
			Px(kMinCropSide) / scale);
	}
	const auto normalized = QRectF(
		result.x() / frame.width(),
		result.y() / frame.height(),
		result.width() / frame.width(),
		result.height() / frame.height());
	if (normalized != _crop) {
		_crop = normalized;
		update();
		_cropChanges.fire_copy(_crop);
	}
}

void Canvas::mouseReleaseEvent(QMouseEvent *e) {
	finishDrag();
	updateCursor(e->position());
}

void Canvas::finishDrag() {
	const auto wasCrop = cropDragging();
	_drag = Drag::None;
	if (wasCrop) {
		update();
		_cropFinished.fire({});
	}
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
	if (!_cropMode && e->button() == Qt::LeftButton && !_frame.isEmpty()) {
		toggleZoom(e->position());
	}
}

void Canvas::enterEventHook(QEnterEvent *e) {
	if (!_hovered) {
		_hovered = true;
		updateZoomControls();
	}
	RpWidget::enterEventHook(e);
}

void Canvas::leaveEventHook(QEvent *e) {
	if (_drag == Drag::None) {
		setCursor(Qt::ArrowCursor);
	}
	if (_hovered) {
		_hovered = false;
		updateZoomControls();
	}
	RpWidget::leaveEventHook(e);
}

void Canvas::paintCheckerboard(QPainter &p, QRectF target) {
	const auto ratio = style::DevicePixelRatio();
	const auto cell = Px(kCheckerCell);
	const auto light = st::mediaviewTransparentBg->c;
	const auto dark = st::mediaviewTransparentFg->c;
	if (_checker.isNull()
		|| _checker.width() != 2 * cell * ratio
		|| _checker.pixelColor(0, 0) != light) {
		_checker = QImage(
			QSize(2 * cell, 2 * cell) * ratio,
			QImage::Format_ARGB32_Premultiplied);
		_checker.setDevicePixelRatio(ratio);
		_checker.fill(light);
		auto q = QPainter(&_checker);
		q.fillRect(QRect(cell, 0, cell, cell), dark);
		q.fillRect(QRect(0, cell, cell, cell), dark);
	}
	p.save();
	p.setClipRect(target);
	p.setBrushOrigin(target.topLeft());
	p.fillRect(target, QBrush(_checker));
	p.restore();
}

void Canvas::paintImage(QPainter &p, const QImage &image, QRectF target) {
	const auto visible = target.intersected(QRectF(rect()));
	if (image.isNull() || visible.isEmpty()) {
		return;
	}
	const auto sx = image.width() / target.width();
	const auto sy = image.height() / target.height();
	const auto source = QRectF(
		(visible.x() - target.x()) * sx,
		(visible.y() - target.y()) * sy,
		visible.width() * sx,
		visible.height() * sy);
	const auto devicePerPixel = style::DevicePixelRatio() / sx;
	p.setRenderHint(
		QPainter::SmoothPixmapTransform,
		devicePerPixel < kSmoothLimit);
	p.drawImage(visible, image, source);
	p.setRenderHint(QPainter::SmoothPixmapTransform, false);
}

void Canvas::paintCrop(QPainter &p) {
	const auto image = imageRect();
	const auto crop = cropWidgetRect();
	const auto fg = st::groupCallMembersFg->c;

	auto outside = QPainterPath();
	outside.setFillRule(Qt::OddEvenFill);
	outside.addRect(image);
	outside.addRect(crop);
	p.fillPath(outside, st::photoCropFadeBg);

	auto hq = PainterHighQualityEnabler(p);
	const auto divisions = _gridVisible
		? kGridFine
		: cropDragging()
		? kGridThirds
		: 0;
	if (divisions > 1) {
		p.setPen(QPen(
			anim::with_alpha(fg, _gridVisible ? 0.28 : 0.45),
			std::max(Px(1), 1) / 2.));
		for (auto i = 1; i != divisions; ++i) {
			const auto x = crop.x() + crop.width() * i / divisions;
			const auto y = crop.y() + crop.height() * i / divisions;
			p.drawLine(QPointF(x, crop.top()), QPointF(x, crop.bottom()));
			p.drawLine(QPointF(crop.left(), y), QPointF(crop.right(), y));
		}
	}
	p.setPen(QPen(anim::with_alpha(fg, 0.85), std::max(Px(1), 1)));
	p.setBrush(Qt::NoBrush);
	p.drawRect(crop);

	const auto thickness = float64(Px(kCornerThickness));
	const auto half = thickness / 2.;
	const auto length = std::min({
		float64(Px(kCornerLength)),
		crop.width() / 2. + half,
		crop.height() / 2. + half,
	});
	p.setPen(Qt::NoPen);
	p.setBrush(fg);
	const auto corner = [&](QPointF point, bool right, bool bottom) {
		const auto horizontalLeft = right
			? (point.x() + half - length)
			: (point.x() - half);
		const auto horizontalTop = bottom
			? (point.y() + half - thickness)
			: (point.y() - half);
		const auto verticalLeft = right
			? (point.x() + half - thickness)
			: (point.x() - half);
		const auto verticalTop = bottom
			? (point.y() + half - length)
			: (point.y() - half);
		p.drawRect(QRectF(horizontalLeft, horizontalTop, length, thickness));
		p.drawRect(QRectF(verticalLeft, verticalTop, thickness, length));
	};
	corner(crop.topLeft(), false, false);
	corner(crop.topRight(), true, false);
	corner(crop.bottomLeft(), false, true);
	corner(crop.bottomRight(), true, true);

	const auto bar = std::min(
		length,
		std::min(crop.width(), crop.height()) / 4.);
	if (bar >= thickness * 2) {
		const auto center = crop.center();
		const auto x = center.x() - bar / 2.;
		const auto y = center.y() - bar / 2.;
		p.drawRect(QRectF(x, crop.top() - half, bar, thickness));
		p.drawRect(QRectF(x, crop.bottom() - half, bar, thickness));
		p.drawRect(QRectF(crop.left() - half, y, thickness, bar));
		p.drawRect(QRectF(crop.right() - half, y, thickness, bar));
	}
}

void Canvas::paintBadge(QPainter &p) {
	const auto text = tr::lng_oblivion_photo_ui_before(tr::now);
	const auto &font = st::semiboldFont;
	const auto padding = Px(kBadgePadding);
	const auto height = Px(kBadgeHeight);
	const auto width = font->width(text) + 2 * padding;
	// On the photo near its top edge, not floating above it in the empty
	// area of a wide photo.
	const auto skip = Px(kBadgeTop);
	const auto top = std::clamp(
		int(std::round(imageRect().top())) + skip,
		skip,
		std::max(this->height() - height - skip, skip));
	const auto rect = QRect(
		(this->width() - width) / 2,
		top,
		width,
		height);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::mediaviewSaveMsgBg);
	p.drawRoundedRect(rect, height / 2., height / 2.);
	p.setPen(st::mediaviewSaveMsgFg);
	p.setFont(font);
	p.drawText(rect, Qt::AlignCenter, text);
}

void Canvas::paintLoading(QPainter &p) {
	const auto size = Px(kSpinnerSize);
	const auto line = Px(kSpinnerLine);
	const auto position = QPoint(
		(width() - size) / 2,
		(height() - size) / 2);
	auto pen = QPen(st::groupCallMembersFg->c);
	pen.setWidth(line);
	pen.setCapStyle(Qt::RoundCap);
	auto hq = PainterHighQualityEnabler(p);
	Ui::InfiniteRadialAnimation::Draw(
		p,
		_radial.computeState(),
		position,
		QSize(size, size),
		width(),
		pen,
		line);
}

void Canvas::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::groupCallBg);
	if (_frame.isEmpty() || (_image.isNull() && _before.isNull())) {
		if (_loading) {
			paintLoading(p);
		}
		return;
	}
	const auto target = imageRect();
	if (_checkerboard) {
		paintCheckerboard(p, target.intersected(QRectF(rect())));
	}
	const auto showBefore = _comparing && !_before.isNull();
	paintImage(p, showBefore ? _before : _image, target);
	if (_cropMode) {
		paintCrop(p);
	}
	if (showBefore) {
		paintBadge(p);
	}
	if (_loading) {
		paintLoading(p);
	}
}

} // namespace Oblivion::Photo::EditorUi
