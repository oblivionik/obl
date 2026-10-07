/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_room_canvas.h"

#include "base/flat_map.h"
#include "base/platform/base_platform_info.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unique_qptr.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_cloud_ui.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_photo_integration.h"
#include "oblivion/oblivion_room_window.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/abstract_button.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animations.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>

namespace Oblivion::Rooms {
namespace {

constexpr auto kLiveInterval = crl::time(50);
constexpr auto kLiveExpire = crl::time(5000);
constexpr auto kLiveInFlightLimit = 2;
constexpr auto kMaxLives = 32;
// What the others draw comes from them as it is: the points of a stroke
// may be anywhere on the board, and stroking a wide line that goes from
// edge to edge thousands of times takes seconds. So what is painted on
// the main thread is limited by its weight (StrokeWeight): the length
// in canvas units and kPointWeight more for every point.
constexpr auto kPointWeight = 8;
// What the others draw right now: a member draws one line at a time (one
// more may wait for its finished stroke), all the previews together are
// not heavier than this (the longest line a hand can draw is about a
// half of it), the width is what the tools give (the eraser of the
// largest size) and nobody draws one line for ten minutes.
constexpr auto kMaxLivesOfUser = 3;
constexpr auto kLiveWeightBudget = int64(240'000);
constexpr auto kLiveMaxSize = 128;
constexpr auto kLiveMaxAge = crl::time(600'000);
// The previews are painted again as a whole only when one of them is
// gone, and not more often than this. Whatever painting them has taken,
// four times as long passes before they are painted again.
constexpr auto kLivesRepaintInterval = crl::time(100);
constexpr auto kLivesPaceFactor = 4;
constexpr auto kLivesPaceMax = crl::time(1000);
constexpr auto kRevealCatchUp = 80.; // Milliseconds to show what has come.
constexpr auto kRevealMinSpeed = 0.06; // Points per millisecond.
constexpr auto kMinDistance = 2; // Canvas units between the raw points.
constexpr auto kRawPointsFactor = 4; // Raw points per stroke, in limits.
// Finished strokes heavier than this together (those of the others: the
// user's own ones were drawn by a hand) and a whole board heavier than
// this are painted off the main thread.
constexpr auto kSyncPaintWeight = int64(48'000);
constexpr auto kResizeRebuildDelay = crl::time(120);
// The whole board is painted again (a stroke was removed, one came out
// of order) not more often than this, however fast such events come.
constexpr auto kRebuildInterval = crl::time(100);
// Events kept while the board is being loaded.
constexpr auto kHeldLimit = 1000;
constexpr auto kReloadDelay = crl::time(3000);
constexpr auto kReloadDelayMax = crl::time(60'000);
constexpr auto kErrorToastInterval = crl::time(3000);
constexpr auto kOwnIdsLimit = 256;
constexpr auto kMarkerAlpha = 110;
constexpr auto kMarkerFactor = 3;
constexpr auto kEraserFactor = 4;
constexpr auto kSizesCount = 4;
constexpr int kSizes[kSizesCount] = { 4, 8, 16, 32 };
constexpr int kSizeDots[kSizesCount] = { 5, 8, 12, 17 };

struct Swatch {
	int red = 0;
	int green = 0;
	int blue = 0;
};
constexpr Swatch kColors[] = {
	{ 0x00, 0x00, 0x00 },
	{ 0xff, 0xff, 0xff },
	{ 0xff, 0x3b, 0x30 },
	{ 0xff, 0x95, 0x00 },
	{ 0xff, 0xcc, 0x00 },
	{ 0x34, 0xc7, 0x59 },
	{ 0x30, 0xb0, 0xc7 },
	{ 0x00, 0x7a, 0xff },
	{ 0xaf, 0x52, 0xde },
	{ 0xff, 0x2d, 0x92 },
};
constexpr auto kColorsCount = int(sizeof(kColors) / sizeof(kColors[0]));
constexpr auto kDarkBackground = "#17212b";
constexpr auto kLightBackground = "#ffffff";

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QColor SwatchColor(int index) {
	const auto &value = kColors[std::clamp(index, 0, kColorsCount - 1)];
	return QColor(value.red, value.green, value.blue);
}

[[nodiscard]] bool DarkColor(const QColor &color) {
	return (color.red() * 299 + color.green() * 587 + color.blue() * 114)
		< 128 * 1000;
}

[[nodiscard]] QColor OpaqueOr(const QColor &color, const QColor &fallback) {
	auto result = color.isValid() ? color : fallback;
	result.setAlpha(255);
	return result;
}

[[nodiscard]] const style::font &SmallFont() {
	static const auto result = style::font(
		Scaled(11),
		st::semiboldFont->flags(),
		st::semiboldFont->family());
	return result;
}

// ---- Icons of the tools, painted in the colours of the theme.

enum class Icon {
	Pen,
	Marker,
	Eraser,
	Undo,
};

void PaintIcon(QPainter &p, Icon icon, QRectF rect, const QColor &color) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	const auto s = std::min(rect.width(), rect.height());
	p.translate(
		rect.x() + (rect.width() - s) / 2.,
		rect.y() + (rect.height() - s) / 2.);
	p.scale(s, s);
	auto stroke = QPen(color, 0.085);
	stroke.setCapStyle(Qt::RoundCap);
	stroke.setJoinStyle(Qt::RoundJoin);
	p.setPen(stroke);
	p.setBrush(Qt::NoBrush);
	const auto tilt = [&](double angle, double cy) {
		p.translate(0.5, cy);
		p.rotate(angle);
		p.translate(-0.5, -cy);
	};
	switch (icon) {
	case Icon::Pen: {
		tilt(45., 0.5);
		auto path = QPainterPath();
		path.moveTo(0.4, 0.08);
		path.lineTo(0.6, 0.08);
		path.lineTo(0.6, 0.68);
		path.lineTo(0.5, 0.92);
		path.lineTo(0.4, 0.68);
		path.closeSubpath();
		p.drawPath(path);
		p.drawLine(QPointF(0.4, 0.68), QPointF(0.6, 0.68));
		p.drawLine(QPointF(0.4, 0.22), QPointF(0.6, 0.22));
	} break;
	case Icon::Marker: {
		tilt(45., 0.5);
		p.drawRoundedRect(QRectF(0.34, 0.06, 0.32, 0.5), 0.05, 0.05);
		auto neck = QPainterPath();
		neck.moveTo(0.4, 0.56);
		neck.lineTo(0.6, 0.56);
		neck.lineTo(0.57, 0.72);
		neck.lineTo(0.43, 0.72);
		neck.closeSubpath();
		p.drawPath(neck);
		auto tip = QPainterPath();
		tip.moveTo(0.43, 0.72);
		tip.lineTo(0.57, 0.72);
		tip.lineTo(0.57, 0.84);
		tip.lineTo(0.43, 0.94);
		tip.closeSubpath();
		p.setBrush(color);
		p.drawPath(tip);
	} break;
	case Icon::Eraser: {
		p.save();
		tilt(-38., 0.46);
		p.drawRoundedRect(QRectF(0.14, 0.3, 0.72, 0.34), 0.07, 0.07);
		p.drawLine(QPointF(0.42, 0.3), QPointF(0.42, 0.64));
		p.restore();
		p.drawLine(QPointF(0.34, 0.9), QPointF(0.88, 0.9));
	} break;
	case Icon::Undo: {
		auto path = QPainterPath();
		path.moveTo(0.22, 0.4);
		path.lineTo(0.58, 0.4);
		path.cubicTo(0.88, 0.4, 0.88, 0.78, 0.58, 0.78);
		path.lineTo(0.36, 0.78);
		p.drawPath(path);
		auto head = QPainterPath();
		head.moveTo(0.38, 0.24);
		head.lineTo(0.22, 0.4);
		head.lineTo(0.38, 0.56);
		p.drawPath(head);
	} break;
	}
	p.restore();
}

class ToolButton final : public Ui::AbstractButton {
public:
	ToolButton(QWidget *parent, Icon icon, int size)
	: AbstractButton(parent)
	, _icon(icon) {
		resize(size, size);
	}

	void setSelected(bool selected) {
		if (_selected != selected) {
			_selected = selected;
			update();
		}
	}
	void setDimmed(bool dimmed) {
		if (_dimmed != dimmed) {
			_dimmed = dimmed;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto full = QRectF(rect());
		p.setOpacity(_dimmed ? 0.4 : 1.);
		p.setPen(Qt::NoPen);
		if (_selected) {
			p.setBrush(st::windowBgActive);
			p.drawEllipse(full);
		} else if (isOver() || isDown()) {
			p.setBrush(isDown() ? st::windowBgRipple : st::windowBgOver);
			p.drawEllipse(full);
		}
		const auto skip = full.width() * 0.22;
		PaintIcon(
			p,
			_icon,
			full.marginsRemoved(QMarginsF(skip, skip, skip, skip)),
			_selected ? st::windowFgActive->c : st::windowFg->c);
	}
	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}

private:
	const Icon _icon;
	bool _selected = false;
	bool _dimmed = false;

};

[[nodiscard]] QPointF Lerp(QPointF a, QPointF b, double ratio) {
	return a + (b - a) * ratio;
}

[[nodiscard]] bool AllSame(const std::vector<QPoint> &points) {
	for (const auto &point : points) {
		if (point != points.front()) {
			return false;
		}
	}
	return true;
}

// What one more point adds to the weight of a line (see StrokeWeight):
// from is its last point so far, if it has any.
[[nodiscard]] int64 PointWeight(const QPoint *from, QPoint point) {
	return kPointWeight + (from
		? (int64(std::abs(point.x() - from->x()))
			+ int64(std::abs(point.y() - from->y())))
		: int64(0));
}

// ---- A line that grows.
//
// A line somebody draws right now gets its points one by one. Stroking
// the whole of it again for every new point costs more and more as it
// gets longer (and a member who sends previews could make that cost as
// large as he likes), so a growing line is kept as a picture and only
// what the new points add is painted into it.
//
// Every point is a control point of the smoothing (see StrokePath), the
// curve goes through the middles between the points: of count points
// the part up to the middle of the last segment never changes again.
// The parts are painted with CompositionMode_Source, they replace what
// is under them, so a translucent line has one alpha where its parts
// (and its own turns) overlap, as a line stroked at once has.

[[nodiscard]] QPointF Middle(QPoint a, QPoint b) {
	return (QPointF(a) + QPointF(b)) / 2.;
}

// Where the part of the line that can't change ends.
[[nodiscard]] QPointF StableEnd(const std::vector<QPoint> &points, int count) {
	return (count < 3)
		? QPointF(points.front())
		: Middle(points[count - 2], points[count - 1]);
}

[[nodiscard]] QColor StrokeColor(
		const CanvasStroke &stroke,
		const QColor &background) {
	const auto eraser = (stroke.tool == CanvasTool::Eraser);
	auto color = eraser ? background : stroke.color;
	color.setAlpha(eraser ? 255 : std::clamp(stroke.alpha, 1, 255));
	return color;
}

[[nodiscard]] QPen StrokePen(
		const CanvasStroke &stroke,
		const QColor &background) {
	auto pen = QPen(
		StrokeColor(stroke, background),
		double(std::clamp(stroke.size, 1, 256)));
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	return pen;
}

// Adds what the points [0, till) define for good beyond what the points
// [0, from) did. The painter is in canvas units and replaces the pixels.
void PaintStrokeGrowth(
		QPainter &p,
		const CanvasStroke &stroke,
		const QColor &background,
		int from,
		int till) {
	const auto &points = stroke.points;
	from = std::max(from, 0);
	till = std::min(till, int(points.size()));
	if (till <= from) {
		return;
	}
	if (!from) {
		// The round start of the line, a dot while it is one point.
		const auto radius = std::clamp(stroke.size, 1, 256) / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(StrokeColor(stroke, background));
		p.drawEllipse(QPointF(points.front()), radius, radius);
	}
	if (till < 3) {
		return;
	}
	const auto known = std::max(from, 2);
	auto path = QPainterPath();
	path.moveTo(StableEnd(points, known));
	for (auto i = known - 1; i + 1 < till; ++i) {
		path.quadTo(QPointF(points[i]), Middle(points[i], points[i + 1]));
	}
	p.setPen(StrokePen(stroke, background));
	p.setBrush(Qt::NoBrush);
	p.drawPath(path);
}

// The end of the line as count points have it: the next point bends it,
// so it is painted only where it can be taken back (StrokeEndRect).
void PaintStrokeEnd(
		QPainter &p,
		const CanvasStroke &stroke,
		const QColor &background,
		int count) {
	const auto &points = stroke.points;
	count = std::min(count, int(points.size()));
	if (count < 2) {
		return;
	}
	p.setPen(StrokePen(stroke, background));
	p.setBrush(Qt::NoBrush);
	p.drawLine(StableEnd(points, count), QPointF(points[count - 1]));
}

// The pixels PaintStrokeEnd touches in a picture scaled by sx and sy.
[[nodiscard]] QRect StrokeEndRect(
		const CanvasStroke &stroke,
		int count,
		double sx,
		double sy,
		QSize image) {
	const auto &points = stroke.points;
	count = std::min(count, int(points.size()));
	if (count < 2) {
		return QRect();
	}
	const auto a = StableEnd(points, count);
	const auto b = QPointF(points[count - 1]);
	const auto half = std::clamp(stroke.size, 1, 256) / 2. + 1.;
	const auto left = std::min(a.x(), b.x()) - half;
	const auto top = std::min(a.y(), b.y()) - half;
	const auto right = std::max(a.x(), b.x()) + half;
	const auto bottom = std::max(a.y(), b.y()) + half;
	return QRect(
		QPoint(
			int(std::floor(left * sx)) - 2,
			int(std::floor(top * sy)) - 2),
		QPoint(
			int(std::ceil(right * sx)) + 2,
			int(std::ceil(bottom * sy)) + 2)
	).intersected(QRect(QPoint(), image));
}

// The picture of one growing line (the one the user draws), the size of
// the board in pixels.
class StrokeLayer final {
public:
	void reset() {
		_image = QImage();
		_endSaved = QImage();
		_endRect = QRect();
		_id = QString();
		_painted = 0;
	}

	// false: there is no picture, the line is to be painted as a whole.
	[[nodiscard]] bool sync(
			const CanvasStroke &stroke,
			const QColor &background,
			QSize canvas,
			QSize target) {
		const auto count = int(stroke.points.size());
		if (target.isEmpty() || canvas.isEmpty() || !count) {
			reset();
			return false;
		}
		auto fresh = (_id != stroke.id) || (_painted > count);
		if (_image.size() != target) {
			_image = QImage(target, QImage::Format_ARGB32_Premultiplied);
			if (_image.isNull()) {
				reset();
				return false;
			}
			fresh = true;
		}
		if (fresh) {
			_image.fill(Qt::transparent);
			_endSaved = QImage();
			_endRect = QRect();
			_id = stroke.id;
			_painted = 0;
		} else if (_painted == count) {
			return true;
		}
		const auto sx = target.width() / double(canvas.width());
		const auto sy = target.height() / double(canvas.height());
		const auto prepare = [&](QPainter &p) {
			p.setCompositionMode(QPainter::CompositionMode_Source);
			p.setRenderHint(QPainter::Antialiasing);
			p.scale(sx, sy);
		};
		{
			auto p = QPainter(&_image);
			if (!_endRect.isEmpty() && !_endSaved.isNull()) {
				// The end of the line as it was is taken back.
				p.setCompositionMode(QPainter::CompositionMode_Source);
				p.drawImage(_endRect.topLeft(), _endSaved);
			}
			prepare(p);
			PaintStrokeGrowth(p, stroke, background, _painted, count);
		}
		_painted = count;
		_endRect = StrokeEndRect(stroke, count, sx, sy, target);
		_endSaved = _endRect.isEmpty() ? QImage() : _image.copy(_endRect);
		if (!_endSaved.isNull()) {
			auto p = QPainter(&_image);
			prepare(p);
			PaintStrokeEnd(p, stroke, background, count);
		}
		return true;
	}

	[[nodiscard]] const QImage &image() const {
		return _image;
	}

private:
	QImage _image;
	QImage _endSaved; // What was under the end of the line.
	QRect _endRect;
	QString _id;
	int _painted = 0; // Points of the line the picture has.

};

// The picture of the previews, the lines the others draw now: adds what
// was revealed of them since painted was filled (one number for every
// preview, in their order). The end of a preview is not painted: it is
// half a segment behind till the next points come.
void PaintLivesGrowth(
		QImage &image,
		const std::vector<CanvasModel::Live> &lives,
		std::vector<int> &painted,
		QSize canvas,
		const QColor &background) {
	painted.resize(lives.size(), 0);
	if (image.isNull() || canvas.isEmpty()) {
		return;
	}
	auto p = std::optional<QPainter>();
	for (auto i = 0, count = int(lives.size()); i != count; ++i) {
		const auto &live = lives[i];
		const auto till = std::clamp(
			int(std::floor(live.shown)),
			0,
			int(live.stroke.points.size()));
		if (till <= painted[i]) {
			continue;
		}
		if (!p) {
			p.emplace(&image);
			p->setCompositionMode(QPainter::CompositionMode_Source);
			p->setRenderHint(QPainter::Antialiasing);
			p->scale(
				image.width() / double(canvas.width()),
				image.height() / double(canvas.height()));
		}
		PaintStrokeGrowth(*p, live.stroke, background, painted[i], till);
		painted[i] = till;
	}
}

} // namespace

QString CanvasToolName(CanvasTool tool) {
	switch (tool) {
	case CanvasTool::Pen: return u"pen"_q;
	case CanvasTool::Marker: return u"marker"_q;
	case CanvasTool::Eraser: return u"eraser"_q;
	}
	return u"pen"_q;
}

bool ValidStrokeId(const QString &id) {
	if (id.isEmpty() || id.size() > 32) {
		return false;
	}
	for (const auto ch : id) {
		const auto code = ch.unicode();
		const auto good = (code >= 'a' && code <= 'z')
			|| (code >= 'A' && code <= 'Z')
			|| (code >= '0' && code <= '9')
			|| (code == '_')
			|| (code == '-');
		if (!good) {
			return false;
		}
	}
	return true;
}

QString NewStrokeId() {
	return u"s-"_q + QString::number(
		base::RandomValue<uint64>(),
		16).rightJustified(16, QChar('0'));
}

std::optional<CanvasStroke> ParseStroke(
		const QJsonObject &object,
		QSize canvas,
		int maxPoints) {
	auto result = CanvasStroke();
	result.id = object.value(u"id"_q).toString();
	if (!ValidStrokeId(result.id)) {
		return std::nullopt;
	}
	result.seq = std::max(Cloud::JsonInt(object.value(u"seq"_q)), int64(0));
	result.userId = Cloud::JsonUserId(object.value(u"user_id"_q));
	const auto tool = object.value(u"tool"_q).toString();
	result.tool = (tool == u"marker"_q)
		? CanvasTool::Marker
		: (tool == u"eraser"_q)
		? CanvasTool::Eraser
		: CanvasTool::Pen;
	if (const auto color = Cloud::JsonColor(object.value(u"color"_q))) {
		result.color = *color;
	}
	result.color.setAlpha(255);
	result.alpha = int(std::clamp(
		Cloud::JsonInt(object.value(u"alpha"_q), 255),
		int64(1),
		int64(255)));
	result.size = int(std::clamp(
		Cloud::JsonInt(object.value(u"size"_q), 4),
		int64(1),
		int64(256)));
	const auto points = object.value(u"points"_q).toArray();
	const auto count = std::min(
		int(points.size() / 2),
		std::max(maxPoints, 1));
	if (count < 1) {
		return std::nullopt;
	}
	const auto width = double(std::max(canvas.width(), 1));
	const auto height = double(std::max(canvas.height(), 1));
	result.points.reserve(count);
	for (auto i = 0; i != count; ++i) {
		const auto x = points.at(i * 2);
		const auto y = points.at(i * 2 + 1);
		if (!x.isDouble() || !y.isDouble()) {
			return std::nullopt;
		}
		result.points.push_back(QPoint(
			int(std::lround(std::clamp(x.toDouble(), 0., width))),
			int(std::lround(std::clamp(y.toDouble(), 0., height)))));
	}
	return result;
}

QJsonObject SerializeLive(const CanvasStroke &stroke, int from, int count) {
	auto points = QJsonArray();
	const auto till = int(std::min(
		int64(from) + count,
		int64(stroke.points.size())));
	for (auto i = std::max(from, 0); i < till; ++i) {
		points.push_back(stroke.points[i].x());
		points.push_back(stroke.points[i].y());
	}
	auto color = stroke.color;
	color.setAlpha(255);
	auto result = QJsonObject();
	result.insert(u"id"_q, stroke.id);
	result.insert(u"tool"_q, CanvasToolName(stroke.tool));
	result.insert(u"color"_q, color.name(QColor::HexRgb));
	result.insert(u"alpha"_q, std::clamp(stroke.alpha, 1, 255));
	result.insert(u"size"_q, std::clamp(stroke.size, 1, 256));
	result.insert(u"points"_q, points);
	return result;
}

QJsonObject SerializeStroke(const CanvasStroke &stroke) {
	return SerializeLive(stroke, 0, int(stroke.points.size()));
}

std::vector<QPoint> SimplifyStroke(
		const std::vector<QPoint> &points,
		int limit,
		double tolerance) {
	const auto count = int(points.size());
	limit = std::max(limit, 2);
	if (count <= 2) {
		return points;
	}
	auto keep = std::vector<char>(count, 0);
	keep.front() = keep.back() = 1;
	auto stack = std::vector<std::pair<int, int>>();
	stack.emplace_back(0, count - 1);
	const auto threshold = tolerance * tolerance;
	while (!stack.empty()) {
		const auto from = stack.back().first;
		const auto till = stack.back().second;
		stack.pop_back();
		if (till - from < 2) {
			continue;
		}
		const auto ax = double(points[from].x());
		const auto ay = double(points[from].y());
		const auto dx = points[till].x() - ax;
		const auto dy = points[till].y() - ay;
		const auto length = dx * dx + dy * dy;
		auto worst = -1.;
		auto index = -1;
		for (auto i = from + 1; i < till; ++i) {
			const auto px = points[i].x() - ax;
			const auto py = points[i].y() - ay;
			auto distance = px * px + py * py;
			if (length > 0.) {
				const auto t = std::clamp((px * dx + py * dy) / length, 0., 1.);
				const auto ex = px - t * dx;
				const auto ey = py - t * dy;
				distance = ex * ex + ey * ey;
			}
			if (distance > worst) {
				worst = distance;
				index = i;
			}
		}
		if (index >= 0 && worst > threshold) {
			keep[index] = 1;
			stack.emplace_back(from, index);
			stack.emplace_back(index, till);
		}
	}
	auto result = std::vector<QPoint>();
	for (auto i = 0; i != count; ++i) {
		if (keep[i]) {
			result.push_back(points[i]);
		}
	}
	if (int(result.size()) > limit) {
		auto thinned = std::vector<QPoint>();
		thinned.reserve(limit);
		const auto last = int64(result.size()) - 1;
		for (auto i = 0; i != limit; ++i) {
			thinned.push_back(result[int((i * last) / (limit - 1))]);
		}
		result = std::move(thinned);
	}
	return result;
}

QPainterPath StrokePath(const std::vector<QPoint> &points, double shown) {
	auto path = QPainterPath();
	const auto total = int(points.size());
	if (!total) {
		return path;
	}
	const auto limit = (shown < 0.)
		? double(total)
		: std::min(shown, double(total));
	const auto whole = std::clamp(int(std::floor(limit)), 1, total);
	auto list = std::vector<QPointF>();
	list.reserve(whole + 1);
	for (auto i = 0; i != whole; ++i) {
		list.push_back(QPointF(points[i]));
	}
	if (whole < total && limit > whole) {
		list.push_back(Lerp(
			QPointF(points[whole - 1]),
			QPointF(points[whole]),
			std::clamp(limit - whole, 0., 1.)));
	}
	path.moveTo(list.front());
	const auto count = int(list.size());
	if (count == 1) {
		return path;
	}
	// The classic smoothing of a freehand line: every point is a control
	// point, the curve goes through the middles between them.
	for (auto i = 1; i + 1 < count; ++i) {
		path.quadTo(list[i], (list[i] + list[i + 1]) / 2.);
	}
	path.lineTo(list.back());
	return path;
}

void PaintStroke(
		QPainter &p,
		const CanvasStroke &stroke,
		const QColor &background,
		double shown) {
	if (stroke.points.empty() || shown == 0.) {
		return;
	}
	const auto eraser = (stroke.tool == CanvasTool::Eraser);
	auto color = eraser ? background : stroke.color;
	color.setAlpha(eraser ? 255 : std::clamp(stroke.alpha, 1, 255));
	const auto width = double(std::clamp(stroke.size, 1, 256));
	const auto dot = (stroke.points.size() == 1)
		|| (shown > 0. && shown <= 1.)
		|| AllSame(stroke.points);
	if (dot) {
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(QPointF(stroke.points.front()), width / 2., width / 2.);
		return;
	}
	auto pen = QPen(color, width);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawPath(StrokePath(stroke.points, shown));
}

int64 StrokeWeight(const CanvasStroke &stroke) {
	auto result = int64(0);
	auto from = (const QPoint*)nullptr;
	for (const auto &point : stroke.points) {
		result += PointWeight(from, point);
		from = &point;
	}
	return result;
}

void CanvasModel::reset(
		QSize size,
		const QColor &background,
		std::vector<CanvasStroke> &&strokes) {
	_size = QSize(
		std::clamp(size.width(), 16, 8192),
		std::clamp(size.height(), 16, 8192));
	_background = OpaqueOr(background, QColor(255, 255, 255));
	ranges::stable_sort(strokes, ranges::less(), &CanvasStroke::seq);
	_strokes.clear();
	_numbers = 0;
	_weight = 0;
	auto ids = base::flat_set<QString>();
	for (auto &stroke : strokes) {
		if (!ids.emplace(stroke.id).second) {
			continue;
		}
		_numbers += int(stroke.points.size()) * 2;
		_weight += StrokeWeight(stroke);
		_strokes.push_back(
			std::make_shared<const CanvasStroke>(std::move(stroke)));
	}
	_pending.erase(
		ranges::remove_if(_pending, [&](const StrokePtr &stroke) {
			return ids.contains(stroke->id);
		}),
		end(_pending));
	_lives.clear();
	++_livesGeneration;
	++_generation;
}

bool CanvasModel::contains(const QString &id) const {
	return ranges::contains(_strokes, id, [](const StrokePtr &stroke) {
		return stroke->id;
	});
}

bool CanvasModel::waiting(const QString &id) const {
	return ranges::contains(_pending, id, [](const StrokePtr &stroke) {
		return stroke->id;
	});
}

int64 CanvasModel::liveWeight() const {
	auto result = int64(0);
	for (const auto &live : _lives) {
		result += live.weight;
	}
	return result;
}

bool CanvasModel::add(CanvasStroke &&stroke) {
	if (contains(stroke.id)) {
		return false;
	}
	removePending(stroke.id);
	dropLive(stroke.id);
	_numbers += int(stroke.points.size()) * 2;
	_weight += StrokeWeight(stroke);
	const auto seq = stroke.seq;
	auto pointer = std::make_shared<const CanvasStroke>(std::move(stroke));
	if (_strokes.empty() || _strokes.back()->seq <= seq) {
		_strokes.push_back(std::move(pointer));
	} else {
		const auto i = ranges::upper_bound(
			_strokes,
			seq,
			ranges::less(),
			[](const StrokePtr &value) { return value->seq; });
		_strokes.insert(i, std::move(pointer));
		++_generation;
	}
	return true;
}

bool CanvasModel::remove(const QString &id) {
	const auto wasWaiting = removePending(id);
	dropLive(id);
	const auto i = ranges::find(_strokes, id, [](const StrokePtr &stroke) {
		return stroke->id;
	});
	if (i == end(_strokes)) {
		return wasWaiting;
	}
	_numbers = std::max(_numbers - int((*i)->points.size()) * 2, 0);
	_weight = std::max(_weight - StrokeWeight(**i), int64(0));
	_strokes.erase(i);
	++_generation;
	return true;
}

void CanvasModel::clear(const QColor &background) {
	_background = OpaqueOr(background, _background);
	_strokes.clear();
	_pending.clear();
	_lives.clear();
	_numbers = 0;
	_weight = 0;
	++_livesGeneration;
	++_generation;
}

void CanvasModel::addPending(CanvasStroke &&stroke) {
	if (!contains(stroke.id)) {
		removePending(stroke.id);
		_pending.push_back(
			std::make_shared<const CanvasStroke>(std::move(stroke)));
	}
}

bool CanvasModel::removePending(const QString &id) {
	const auto i = ranges::find(_pending, id, [](const StrokePtr &stroke) {
		return stroke->id;
	});
	if (i == end(_pending)) {
		return false;
	}
	_pending.erase(i);
	return true;
}

void CanvasModel::dropLive(const QString &id) {
	const auto was = _lives.size();
	_lives.erase(
		ranges::remove_if(_lives, [&](const Live &live) {
			return (live.stroke.id == id);
		}),
		end(_lives));
	if (_lives.size() != was) {
		++_livesGeneration;
	}
}

bool CanvasModel::addLive(
		CanvasStroke &&part,
		crl::time now,
		int maxPoints) {
	if (part.points.empty() || contains(part.id)) {
		return false;
	}
	maxPoints = std::max(maxPoints, 1);
	const auto userId = part.userId;
	const auto same = [&](const Live &live) {
		return (live.stroke.id == part.id)
			&& (live.stroke.userId == userId);
	};
	const auto drop = [&](std::vector<Live>::iterator i) {
		_lives.erase(i);
		++_livesGeneration;
	};
	// The preview of that member that got its points longest ago, not
	// the one these points are for.
	const auto oldestOf = [&](uint64 id) {
		auto result = end(_lives);
		for (auto i = begin(_lives); i != end(_lives); ++i) {
			if (i->stroke.userId == id
				&& !same(*i)
				&& (result == end(_lives) || i->updated < result->updated)) {
				result = i;
			}
		}
		return result;
	};
	const auto found = ranges::find_if(_lives, same);
	const auto known = (found != end(_lives));
	const auto have = known ? int(found->stroke.points.size()) : 0;
	auto take = std::min(int(part.points.size()), maxPoints - have);
	if (take <= 0) {
		// A full preview takes nothing and does not live longer for it:
		// it goes away five seconds after the last point it has got.
		return false;
	}
	// What the first points of the part add to the weight of the line,
	// as many of them as fit into limit.
	const auto last = known
		? std::make_optional(found->stroke.points.back())
		: std::optional<QPoint>();
	const auto fit = [&](int count, int64 limit) {
		auto result = std::pair<int, int64>(0, 0);
		auto from = last ? &*last : nullptr;
		for (auto i = 0; i != count; ++i) {
			const auto next = result.second
				+ PointWeight(from, part.points[i]);
			if (next > limit) {
				break;
			}
			result = { i + 1, next };
			from = &part.points[i];
		}
		return result;
	};
	auto weight = fit(take, std::numeric_limits<int64>::max()).second;
	if (!known) {
		// A member draws one line at a time: more previews of one member
		// push out his own oldest ones, not the lines of the others.
		auto mine = int(ranges::count(_lives, userId, [](const Live &live) {
			return live.stroke.userId;
		}));
		for (; mine >= kMaxLivesOfUser; --mine) {
			const auto oldest = oldestOf(userId);
			if (oldest == end(_lives)) {
				break;
			}
			drop(oldest);
		}
		if (int(_lives.size()) >= kMaxLives) {
			drop(ranges::min_element(_lives, ranges::less(), &Live::updated));
		}
	}
	// All the previews together have a limited weight: they are painted
	// on the main thread. When there is no room, the member who has the
	// most gives way, so who floods pushes out himself.
	while (true) {
		auto total = int64(0);
		auto weights = base::flat_map<uint64, int64>();
		for (const auto &live : _lives) {
			total += live.weight;
			weights[live.stroke.userId] += live.weight;
		}
		const auto left = kLiveWeightBudget - total;
		if (weight <= left) {
			break;
		}
		weights[userId] += weight;
		auto heaviest = userId;
		auto heaviestWeight = int64(0);
		for (const auto &[id, value] : weights) {
			if (value > heaviestWeight) {
				heaviest = id;
				heaviestWeight = value;
			}
		}
		const auto victim = oldestOf(heaviest);
		if (victim == end(_lives)) {
			// Nothing to drop but the line that grows: it takes what
			// there is room for and stops growing.
			const auto fitted = fit(take, left);
			take = fitted.first;
			weight = fitted.second;
			break;
		}
		drop(victim);
	}
	if (take <= 0) {
		return false;
	}
	const auto i = ranges::find_if(_lives, same);
	if (i != end(_lives)) {
		auto &points = i->stroke.points;
		points.insert(
			end(points),
			begin(part.points),
			begin(part.points) + take);
		i->weight += weight;
		i->updated = now;
		return true;
	}
	part.points.resize(take);
	part.size = std::min(part.size, kLiveMaxSize);
	_lives.push_back({
		.stroke = std::move(part),
		.weight = weight,
		.started = now,
		.updated = now,
		.shown = 1.,
	});
	return true;
}

bool CanvasModel::expireLive(crl::time now) {
	const auto was = _lives.size();
	_lives.erase(
		ranges::remove_if(_lives, [&](const Live &live) {
			return ((now - live.updated) > kLiveExpire)
				|| ((now - live.started) > kLiveMaxAge);
		}),
		end(_lives));
	if (_lives.size() == was) {
		return false;
	}
	++_livesGeneration;
	return true;
}

bool CanvasModel::advanceLive(crl::time elapsed) {
	auto more = false;
	const auto passed = double(std::clamp(elapsed, crl::time(1), crl::time(250)));
	for (auto &live : _lives) {
		const auto target = double(live.stroke.points.size());
		if (live.shown >= target) {
			continue;
		}
		const auto left = target - live.shown;
		live.shown = std::min(
			target,
			live.shown + std::max(
				left * passed / kRevealCatchUp,
				passed * kRevealMinSpeed));
		more = more || (live.shown < target);
	}
	return more;
}

QString CanvasModel::lastOwn(
		uint64 userId,
		const base::flat_set<QString> &skip) const {
	for (const auto &list : { &_pending, &_strokes }) {
		for (auto i = list->rbegin(); i != list->rend(); ++i) {
			if ((*i)->userId == userId && !skip.contains((*i)->id)) {
				return (*i)->id;
			}
		}
	}
	return QString();
}

bool CanvasModel::full(const CanvasLimits &limits, int points) const {
	auto waiting = 0;
	for (const auto &stroke : _pending) {
		waiting += int(stroke->points.size()) * 2;
	}
	return (int(_strokes.size() + _pending.size()) >= limits.strokes)
		|| (_numbers + waiting + points * 2 > limits.canvasNumbers);
}

QImage RenderCanvas(
		const std::vector<CanvasModel::StrokePtr> &strokes,
		int count,
		QSize canvas,
		const QColor &background,
		QSize target) {
	if (target.isEmpty() || canvas.isEmpty()) {
		return QImage();
	}
	auto result = QImage(target, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return result;
	}
	const auto fill = OpaqueOr(background, QColor(255, 255, 255));
	result.fill(fill);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	p.scale(
		target.width() / double(canvas.width()),
		target.height() / double(canvas.height()));
	const auto till = std::min(count, int(strokes.size()));
	for (auto i = 0; i < till; ++i) {
		PaintStroke(p, *strokes[i], fill);
	}
	p.end();
	return result;
}

namespace {

// ---- The tab.

class CanvasTab final : public Ui::RpWidget {
public:
	CanvasTab(QWidget *parent, TabContext context);

	// For the snapshot scenes.
	void setSample(
		std::vector<CanvasStroke> strokes,
		std::vector<CanvasStroke> lives,
		const QColor &background);
	void setSampleFailed();
	void chooseTool(CanvasTool tool, int color, int size);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	struct Target {
		enum class Kind {
			Size,
			Color,
		};
		QRect rect;
		Kind kind = Kind::Size;
		int index = 0;
	};

	void updateLayout();
	void refreshControls();
	void paintBoard(QPainter &p);
	void paintControls(QPainter &p);
	void paintTags(QPainter &p);
	[[nodiscard]] QString statusText(bool &error) const;
	[[nodiscard]] bool disabledOnServer() const;
	[[nodiscard]] bool canDraw() const;
	[[nodiscard]] bool toolsActive() const;
	[[nodiscard]] QPoint toCanvas(QPoint position) const;
	[[nodiscard]] QRect strokeRect(QPoint a, QPoint b, int size) const;
	[[nodiscard]] const Target *targetAt(QPoint position) const;

	void load();
	void loadDone(const QJsonObject &json);
	void loadFailed();
	void handle(const Cloud::Event &event);
	void hold(const Cloud::Event &event);
	void apply(const Cloud::Event &event);
	void confirmed(const QString &id);

	[[nodiscard]] bool startAllowed();
	void beginStroke(QPoint point);
	void extendStroke(QPoint point);
	void finishStroke();
	void cancelStroke();
	void sendLive();
	void undo();
	void sendRemove(const QString &id);
	void rememberOwn(const QString &id);
	void strokeFailed(const QString &id, const Cloud::Error &error);
	void toast(const QString &text);

	void syncLayer();
	void startRebuild(QSize wanted, int from);
	void rebuildDone(
		int generation,
		QSize size,
		int from,
		int count,
		QImage &&image);
	void syncLives();
	[[nodiscard]] bool revealStep(crl::time now);
	void livesChanged();

	void showMenu();
	void clearRequested(bool swapBackground);
	void sendClear(bool swapBackground);
	void render(Fn<void(QImage)> done);
	void savePng();
	void openEditor();

	const not_null<Room*> _room;
	const std::shared_ptr<Ui::Show> _show;
	const not_null<ToolButton*> _pen;
	const not_null<ToolButton*> _marker;
	const not_null<ToolButton*> _eraser;
	const not_null<ToolButton*> _undo;
	const not_null<GlyphButton*> _more;
	base::unique_qptr<Ui::PopupMenu> _menu;

	CanvasModel _model;
	CanvasLimits _limits;
	CanvasTool _tool = CanvasTool::Pen;
	int _color = 0;
	int _size = 1;

	QRect _board;
	QRect _status;
	bool _cleanShown = false; // The words of a clean board are painted.
	QString _statusOfLives; // See livesChanged().
	double _scale = 1.;
	std::vector<Target> _targets;

	QImage _layer;
	int _layerGeneration = -1;
	int _layerStrokes = 0;
	bool _rebuilding = false; // One at a time, off the main thread.
	crl::time _rebuiltAt = 0; // When the last full repaint has started.
	base::Timer _rebuildTimer;

	// The lines the others draw now, painted as they grow.
	QImage _livesLayer;
	std::vector<int> _livesPainted;
	int _livesGeneration = -1;
	crl::time _livesRepainted = 0;
	crl::time _livesNotBefore = 0;
	base::Timer _livesTimer;

	std::optional<CanvasStroke> _current;
	StrokeLayer _currentLayer;
	int _liveSent = 0;
	int _liveInFlight = 0;
	base::Timer _liveTimer;
	base::flat_set<QString> _removing; // Undos the user has asked for.
	base::flat_set<QString> _removeSent; // Those the server was asked for.
	std::vector<QString> _ownIds;

	bool _loading = false;
	bool _loaded = false;
	bool _failed = false;
	bool _reloadAgain = false;
	std::vector<Cloud::Event> _held;
	int _heldNumbers = 0;
	bool _heldLost = false; // Too many to keep: the board is asked again.
	crl::time _reloadDelay = kReloadDelay;
	base::Timer _reloadTimer;

	base::Timer _expireTimer;
	Ui::Animations::Basic _reveal;
	crl::time _revealLast = 0;
	crl::time _lastErrorToast = 0;
	bool _active = true;

};

Fn<void(not_null<CanvasTab*>)> SceneSetup;

CanvasTab::CanvasTab(QWidget *parent, TabContext context)
: RpWidget(parent)
, _room(context.room)
, _show(context.show)
, _pen(Ui::CreateChild<ToolButton>(this, Icon::Pen, Scaled(38)))
, _marker(Ui::CreateChild<ToolButton>(this, Icon::Marker, Scaled(38)))
, _eraser(Ui::CreateChild<ToolButton>(this, Icon::Eraser, Scaled(38)))
, _undo(Ui::CreateChild<ToolButton>(this, Icon::Undo, Scaled(38)))
, _more(Ui::CreateChild<GlyphButton>(this, Glyph::More, Scaled(38)))
, _rebuildTimer([=] { update(); })
, _livesTimer([=] { update(_board); })
, _liveTimer([=] { sendLive(); })
, _reloadTimer([=] { load(); })
, _expireTimer([=] {
	if (_model.expireLive(crl::now())) {
		livesChanged();
	}
	if (_model.lives().empty()) {
		_expireTimer.cancel();
		_livesTimer.cancel();
		_livesLayer = QImage();
		_livesPainted.clear();
		_livesGeneration = -1;
	}
})
, _reveal([=](crl::time now) { return revealStep(now); }) {
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);

	if (const auto account = _room->account()) {
		const auto limit = [&](const char *name, int fallback) {
			return int(std::clamp(
				account->limit(name, fallback),
				int64(1),
				int64(10'000'000)));
		};
		_limits.strokes = limit("strokes", _limits.strokes);
		_limits.strokePoints = limit("stroke_points", _limits.strokePoints);
		_limits.canvasNumbers = limit("canvas_points", _limits.canvasNumbers);
		_limits.livePoints = limit("live_points", _limits.livePoints);
	}
	const auto &canvas = _room->state().canvas;
	_model.reset(
		QSize(canvas.width, canvas.height),
		QColor(canvas.background),
		{});
	_color = DarkColor(_model.background()) ? 1 : 0;

	_pen->setClickedCallback([=] { chooseTool(CanvasTool::Pen, _color, _size); });
	_marker->setClickedCallback([=] {
		chooseTool(CanvasTool::Marker, _color, _size);
	});
	_eraser->setClickedCallback([=] {
		chooseTool(CanvasTool::Eraser, _color, _size);
	});
	_undo->setClickedCallback([=] { undo(); });
	_more->setClickedCallback([=] { showMenu(); });

	_room->events(
	) | rpl::on_next([=](const Cloud::Event &event) {
		handle(event);
	}, lifetime());
	_room->reloaded(
	) | rpl::on_next([=] {
		load();
	}, lifetime());
	_room->changes(
	) | rpl::on_next([=](Changes changes) {
		if ((changes & Change::Rights) && _current && !canDraw()) {
			cancelStroke();
		}
		if ((changes & Change::Connection)
			&& _room->connected()
			&& _failed) {
			load();
		}
		const auto mine = Changes(Change::Rights)
			| Change::Connection
			| Change::Members
			| Change::Gone
			| Change::Reloaded;
		if (changes & mine) {
			refreshControls();
		}
	}, lifetime());
	std::move(
		context.active
	) | rpl::on_next([=](bool active) {
		_active = active;
		if (!active) {
			finishStroke();
		}
	}, lifetime());

	if (_room->sample()) {
		_loaded = true;
		if (SceneSetup) {
			SceneSetup(this);
		}
	} else {
		load();
	}
	refreshControls();
}

void CanvasTab::setSample(
		std::vector<CanvasStroke> strokes,
		std::vector<CanvasStroke> lives,
		const QColor &background) {
	_model.reset(_model.size(), background, std::move(strokes));
	const auto now = crl::now();
	for (auto &live : lives) {
		_model.addLive(std::move(live), now, 100'000);
	}
	// Everything that "has come" is shown at once in a scene.
	while (_model.advanceLive(250)) {
	}
	_color = DarkColor(_model.background()) ? 1 : 0;
	refreshControls();
}

void CanvasTab::setSampleFailed() {
	_loaded = false;
	_failed = true;
	refreshControls();
}

void CanvasTab::chooseTool(CanvasTool tool, int color, int size) {
	finishStroke();
	_tool = tool;
	_color = std::clamp(color, 0, kColorsCount - 1);
	_size = std::clamp(size, 0, kSizesCount - 1);
	refreshControls();
}

bool CanvasTab::disabledOnServer() const {
	const auto account = _room->account();
	return account
		&& account->hello().valid
		&& !account->feature("canvas");
}

bool CanvasTab::canDraw() const {
	return _room->can(Right::Draw)
		&& (_room->state().gone == Gone::No)
		&& !disabledOnServer();
}

// How the tools look. A board that is not here yet (or has failed to
// come) and a room without the connection have nothing to draw on right
// now: a line can't be started there (startAllowed()), so the tools look
// switched off as well, not only the line of the state says it.
bool CanvasTab::toolsActive() const {
	return canDraw() && _loaded && _room->connected();
}

void CanvasTab::refreshControls() {
	const auto can = toolsActive();
	_pen->setSelected(_tool == CanvasTool::Pen);
	_marker->setSelected(_tool == CanvasTool::Marker);
	_eraser->setSelected(_tool == CanvasTool::Eraser);
	for (const auto &button : { _pen, _marker, _eraser }) {
		button->setDimmed(!can);
	}
	_undo->setDimmed(!can
		|| _model.lastOwn(_room->selfId(), _removing).isEmpty());
	// Everything is painted again, the status line too.
	_statusOfLives = QString();
	update();
}

void CanvasTab::updateLayout() {
	const auto w = width();
	const auto h = height();
	if (w <= 0 || h <= 0) {
		return;
	}
	const auto pad = Scaled(12);
	const auto gap = Scaled(8);
	const auto rowTools = Scaled(42);
	const auto rowColors = Scaled(34);
	const auto rowStatus = Scaled(24);
	const auto controls = gap + rowTools + rowColors + rowStatus + Scaled(6);
	const auto availableWidth = std::max(w - 2 * pad, 16);
	const auto availableHeight = std::max(h - pad - controls, 16);
	const auto canvas = _model.size();
	auto boardWidth = availableWidth;
	auto boardHeight = int(int64(boardWidth) * canvas.height() / canvas.width());
	if (boardHeight > availableHeight) {
		boardHeight = availableHeight;
		boardWidth = int(int64(boardHeight) * canvas.width() / canvas.height());
	}
	boardWidth = std::max(boardWidth, 1);
	boardHeight = std::max(boardHeight, 1);
	// The board starts right under the tabs, like the content of the
	// other tabs does, and the tools are attached to it: in a tall window
	// what is left stays empty below them, not as a band above the board.
	const auto board = QRect(
		(w - boardWidth) / 2,
		pad,
		boardWidth,
		boardHeight);
	if (_board.size() != board.size() && !_layer.isNull()) {
		// The picture is stretched while the window is being resized.
		_rebuildTimer.callOnce(kResizeRebuildDelay);
	}
	_board = board;
	_scale = boardWidth / double(canvas.width());

	const auto controlsWidth = std::min(
		std::max(boardWidth, Scaled(336)),
		availableWidth);
	const auto left = (w - controlsWidth) / 2;
	auto top = _board.y() + _board.height() + gap;
	auto x = left;
	for (const auto &button : { _pen, _marker, _eraser }) {
		button->move(x, top + (rowTools - button->height()) / 2);
		x += button->width() + Scaled(4);
	}
	x += Scaled(8);
	_targets.clear();
	const auto cell = Scaled(30);
	for (auto i = 0; i != kSizesCount; ++i) {
		_targets.push_back({
			.rect = QRect(x, top + (rowTools - cell) / 2, cell, cell),
			.kind = Target::Kind::Size,
			.index = i,
		});
		x += cell;
	}
	auto right = left + controlsWidth;
	_more->move(
		right - _more->width(),
		top + (rowTools - _more->height()) / 2);
	right -= _more->width() + Scaled(4);
	_undo->move(
		right - _undo->width(),
		top + (rowTools - _undo->height()) / 2);
	top += rowTools;

	const auto swatch = Scaled(24);
	const auto step = std::min(
		(controlsWidth - swatch - Scaled(14)) / (kColorsCount - 1),
		swatch + Scaled(12));
	for (auto i = 0; i != kColorsCount; ++i) {
		_targets.push_back({
			.rect = QRect(
				left + Scaled(7) + i * step,
				top + (rowColors - swatch) / 2,
				swatch,
				swatch),
			.kind = Target::Kind::Color,
			.index = i,
		});
	}
	top += rowColors;
	_status = QRect(left + Scaled(7), top, controlsWidth - Scaled(14), rowStatus);
	update();
}

void CanvasTab::resizeEvent(QResizeEvent *e) {
	updateLayout();
}

QPoint CanvasTab::toCanvas(QPoint position) const {
	const auto canvas = _model.size();
	const auto scale = (_scale > 0.) ? _scale : 1.;
	return QPoint(
		std::clamp(
			int(std::lround((position.x() - _board.x()) / scale)),
			0,
			canvas.width()),
		std::clamp(
			int(std::lround((position.y() - _board.y()) / scale)),
			0,
			canvas.height()));
}

QRect CanvasTab::strokeRect(QPoint a, QPoint b, int size) const {
	const auto margin = int(std::ceil(size * _scale / 2.)) + Scaled(4);
	const auto map = [&](QPoint point) {
		return QPoint(
			_board.x() + int(std::lround(point.x() * _scale)),
			_board.y() + int(std::lround(point.y() * _scale)));
	};
	return QRect(map(a), map(b)).normalized().marginsAdded(
		QMargins(margin, margin, margin, margin));
}

const CanvasTab::Target *CanvasTab::targetAt(QPoint position) const {
	for (const auto &target : _targets) {
		if (target.rect.contains(position)) {
			return &target;
		}
	}
	return nullptr;
}

QString CanvasTab::statusText(bool &error) const {
	error = false;
	if (disabledOnServer()) {
		return tr::lng_oblivion_rcanvas_status_disabled(tr::now);
	} else if (_failed && !_loaded) {
		error = true;
		return tr::lng_oblivion_rcanvas_status_failed(tr::now);
	} else if (!_loaded) {
		return tr::lng_oblivion_rcanvas_status_loading(tr::now);
	} else if (!_room->connected()) {
		return tr::lng_oblivion_rcanvas_status_offline(tr::now);
	} else if (!_room->can(Right::Draw)) {
		return tr::lng_oblivion_rcanvas_status_no_right(tr::now);
	} else if (_model.full(_limits, 1)) {
		error = true;
		return tr::lng_oblivion_rcanvas_status_full(tr::now);
	}
	auto names = QStringList();
	auto seen = base::flat_set<uint64>();
	for (const auto &live : _model.lives()) {
		if (!seen.emplace(live.stroke.userId).second) {
			continue;
		}
		const auto member = _room->state().member(live.stroke.userId);
		if (member && !member->name.isEmpty() && names.size() < 3) {
			names.push_back(member->name);
		}
	}
	if (names.size() == 1) {
		return tr::lng_oblivion_rcanvas_status_drawing_one(
			tr::now,
			lt_name,
			names.front());
	} else if (!names.isEmpty()) {
		return tr::lng_oblivion_rcanvas_status_drawing_many(
			tr::now,
			lt_names,
			names.join(u", "_q));
	}
	return Platform::IsMac()
		? tr::lng_oblivion_rcanvas_status_hint_mac(tr::now)
		: tr::lng_oblivion_rcanvas_status_hint(tr::now);
}

void CanvasTab::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
	auto hq = PainterHighQualityEnabler(p);
	const auto frame = _board.marginsAdded(QMargins(2, 2, 2, 2));
	if (!_board.isEmpty() && e->rect().intersects(frame)) {
		paintBoard(p);
	}
	if (e->rect().bottom() > _board.y() + _board.height()) {
		paintControls(p);
	}
}

void CanvasTab::paintBoard(QPainter &p) {
	syncLayer();
	const auto radius = double(Scaled(10));
	const auto background = _model.background();
	// The edge of the paper is a tint of the theme, not a shadow: a dark
	// board in a dark theme has the colour of the window and would have
	// no visible edge at all.
	const auto edgeBase = st::windowBg->c;
	const auto edgeTint = st::windowFg->c;
	const auto mix = [](int a, int b) {
		return (a * 44 + b * 6) / 50;
	};
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(
		mix(edgeBase.red(), edgeTint.red()),
		mix(edgeBase.green(), edgeTint.green()),
		mix(edgeBase.blue(), edgeTint.blue())));
	p.drawRoundedRect(
		QRectF(_board).marginsAdded(QMarginsF(1., 1., 1., 1.)),
		radius + 1.,
		radius + 1.);
	p.save();
	auto clip = QPainterPath();
	clip.addRoundedRect(QRectF(_board), radius, radius);
	p.setClipPath(clip, Qt::IntersectClip);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	if (_layer.isNull()) {
		p.fillRect(_board, background);
	} else {
		p.drawImage(QRectF(_board), _layer);
	}
	const auto direct = [&](const CanvasStroke &stroke) {
		p.save();
		p.translate(_board.topLeft());
		p.scale(_scale, _scale);
		PaintStroke(p, stroke, background);
		p.restore();
	};
	if (_rebuilding && _layerGeneration == _model.generation()) {
		// Heavy strokes of the others are being painted off the main
		// thread: the user's own ones that came after them do not wait
		// for that (a stroke just drawn would vanish for a moment).
		const auto &strokes = _model.strokes();
		const auto self = _room->selfId();
		for (auto i = _layerStrokes, n = int(strokes.size()); i < n; ++i) {
			if (strokes[i]->userId == self) {
				direct(*strokes[i]);
			}
		}
	}
	for (const auto &stroke : _model.pending()) {
		direct(*stroke);
	}
	// What is being drawn (by the others and by the user) is not stroked
	// again for every frame: these are pictures only the new points are
	// added to.
	syncLives();
	if (!_livesLayer.isNull() && _livesGeneration >= 0) {
		p.drawImage(QRectF(_board), _livesLayer);
	}
	if (_current) {
		const auto wanted = _board.size() * style::DevicePixelRatio();
		const auto ready = _currentLayer.sync(
			*_current,
			background,
			_model.size(),
			wanted);
		if (ready) {
			p.drawImage(QRectF(_board), _currentLayer.image());
		} else {
			direct(*_current);
		}
	}
	p.restore();
	paintTags(p);

	// What is written over the paper has the colour of its ink: the paper
	// is white or dark whatever the theme is.
	const auto ink = [&](int onDarkPaper, int onLightPaper) {
		return DarkColor(background)
			? QColor(255, 255, 255, onDarkPaper)
			: QColor(0, 0, 0, onLightPaper);
	};
	const auto inner = _board.marginsRemoved(
		QMargins(Scaled(16), Scaled(16), Scaled(16), Scaled(16)));
	const auto wrapped = int(Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap);
	const auto heightOf = [&](const style::font &font, const QString &line) {
		return QFontMetrics(font->f).boundingRect(
			QRect(0, 0, std::max(inner.width(), 1), 10000),
			wrapped,
			line).height();
	};
	const auto clean = _loaded
		&& _model.strokes().empty()
		&& _model.pending().empty()
		&& _model.lives().empty()
		&& !_current;
	if (_cleanShown != clean) {
		// The first line is painted in its own small rectangle only: the
		// words on the paper have to go (or come) as a whole.
		_cleanShown = clean;
		update(_board);
	}
	if (!_loaded) {
		// Nothing of the board is known yet: say so over the paper. This is
		// the only place that says it, the line under the tools is empty.
		auto error = false;
		const auto text = statusText(error);
		const auto retry = error && !_loading;
		const auto label = tr::lng_oblivion_rcanvas_retry(tr::now);
		const auto textHeight = heightOf(st::normalFont, text);
		const auto buttonHeight = Scaled(32);
		const auto buttonWidth = st::semiboldFont->width(label) + Scaled(40);
		const auto skip = Scaled(14);
		const auto full = textHeight + (retry ? (skip + buttonHeight) : 0);
		const auto top = inner.y() + std::max((inner.height() - full) / 2, 0);
		p.setFont(st::normalFont);
		p.setPen(ink(190, 150));
		p.drawText(
			QRect(inner.x(), top, inner.width(), textHeight),
			wrapped,
			text);
		if (retry
			&& buttonWidth <= inner.width()
			&& full <= inner.height()) {
			// A click anywhere on the board asks for it again (see
			// startAllowed()), the button only shows where to click.
			const auto button = QRectF(
				inner.x() + (inner.width() - buttonWidth) / 2,
				top + textHeight + skip,
				buttonWidth,
				buttonHeight);
			p.setPen(Qt::NoPen);
			p.setBrush(st::activeButtonBg);
			p.drawRoundedRect(button, buttonHeight / 2., buttonHeight / 2.);
			p.setFont(st::semiboldFont);
			p.setPen(st::activeButtonFg);
			p.drawText(button, Qt::AlignCenter, label);
		}
	} else if (clean) {
		// A clean board says what it is for till the first line.
		const auto title = tr::lng_oblivion_rcanvas_empty(tr::now);
		const auto about = tr::lng_oblivion_rcanvas_empty_about(tr::now);
		const auto titleHeight = heightOf(st::semiboldFont, title);
		const auto aboutHeight = heightOf(st::normalFont, about);
		const auto skip = Scaled(6);
		const auto full = titleHeight + skip + aboutHeight;
		if (full <= inner.height()) {
			const auto top = inner.y() + (inner.height() - full) / 2;
			p.setFont(st::semiboldFont);
			p.setPen(ink(150, 110));
			p.drawText(
				QRect(inner.x(), top, inner.width(), titleHeight),
				wrapped,
				title);
			p.setFont(st::normalFont);
			p.setPen(ink(110, 80));
			p.drawText(
				QRect(
					inner.x(),
					top + titleHeight + skip,
					inner.width(),
					aboutHeight),
				wrapped,
				about);
		}
	}
}

// Who draws what: a small name at the end of every line in progress.
void CanvasTab::paintTags(QPainter &p) {
	const auto &font = SmallFont();
	for (const auto &live : _model.lives()) {
		const auto &points = live.stroke.points;
		const auto member = _room->state().member(live.stroke.userId);
		if (points.empty() || !member || member->name.isEmpty()) {
			continue;
		}
		// Where the painted part of the line ends (see PaintLivesGrowth).
		const auto painted = StableEnd(points, std::clamp(
			int(std::floor(live.shown)),
			1,
			int(points.size())));
		const auto tip = QPoint(
			_board.x() + int(std::lround(painted.x() * _scale)),
			_board.y() + int(std::lround(painted.y() * _scale)));
		const auto text = font->elided(member->name, Scaled(96));
		const auto width = font->width(text) + Scaled(12);
		const auto height = font->height + Scaled(4);
		auto rect = QRect(
			tip.x() + Scaled(8),
			tip.y() - height - Scaled(6),
			width,
			height);
		if (rect.right() > _board.right() - Scaled(4)) {
			rect.moveRight(tip.x() - Scaled(8));
		}
		if (rect.y() < _board.y() + Scaled(4)) {
			rect.moveTop(tip.y() + Scaled(8));
		}
		const auto color = Ui::EmptyUserpic::UserpicColor(
			Ui::EmptyUserpic::ColorIndex(live.stroke.userId));
		p.setPen(Qt::NoPen);
		p.setBrush(color.color2);
		p.drawRoundedRect(QRectF(rect), height / 2., height / 2.);
		p.setFont(font);
		p.setPen(QColor(255, 255, 255));
		p.drawText(rect, Qt::AlignCenter, text);
	}
}

void CanvasTab::paintControls(QPainter &p) {
	const auto can = toolsActive();
	const auto eraser = (_tool == CanvasTool::Eraser);
	for (const auto &target : _targets) {
		const auto rect = QRectF(target.rect);
		const auto center = rect.center();
		if (target.kind == Target::Kind::Size) {
			const auto selected = (target.index == _size);
			const auto dot = Scaled(kSizeDots[target.index]) / 2.;
			p.setOpacity(can ? 1. : 0.4);
			if (selected) {
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver);
				p.drawEllipse(rect.marginsRemoved(QMarginsF(1., 1., 1., 1.)));
			}
			p.setPen(Qt::NoPen);
			p.setBrush(selected ? st::windowActiveTextFg : st::windowSubTextFg);
			p.drawEllipse(center, dot, dot);
		} else {
			const auto selected = (target.index == _color) && !eraser;
			const auto radius = rect.width() / 2.;
			p.setOpacity(!can ? 0.4 : eraser ? 0.35 : 1.);
			p.setPen(QPen(QColor(128, 128, 128, 110), 1.));
			p.setBrush(SwatchColor(target.index));
			p.drawEllipse(center, radius - 2.5, radius - 2.5);
			if (selected) {
				p.setPen(QPen(st::windowActiveTextFg->c, Scaled(2)));
				p.setBrush(Qt::NoBrush);
				p.drawEllipse(center, radius + 1., radius + 1.);
			}
		}
	}
	p.setOpacity(1.);

	if (!_loaded) {
		// "Loading" and "could not load" are written on the board itself.
		return;
	}
	auto error = false;
	const auto text = statusText(error);
	p.setFont(st::normalFont);
	p.setPen(error ? st::boxTextFgError : st::windowSubTextFg);
	p.drawText(
		_status,
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(text, _status.width()));
}

// ---- The board of the server.

void CanvasTab::load() {
	if (_room->sample() || disabledOnServer()) {
		return;
	} else if (_loading) {
		_reloadAgain = true;
		return;
	}
	_loading = true;
	_failed = false;
	_held.clear();
	_heldNumbers = 0;
	_heldLost = false;
	_reloadTimer.cancel();
	const auto id = _room->send(
		Cloud::GetRequest(u"/canvas"_q),
		crl::guard(this, [=](const Cloud::Response &response) {
			loadDone(response.json);
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			loadFailed();
		}));
	if (!id) {
		loadFailed();
	}
	refreshControls();
}

void CanvasTab::loadFailed() {
	_loading = false;
	_reloadAgain = false;
	_failed = true;
	_heldNumbers = 0;
	const auto lost = base::take(_heldLost);
	// What was held back is applied to the board there is, the whole
	// board is asked for again when the connection is back or on a click.
	for (const auto &event : base::take(_held)) {
		if (_loaded) {
			apply(event);
		}
	}
	if (lost && _loaded) {
		// Some of what has happened meanwhile is not known: one more try,
		// later and later if it goes on like this.
		_reloadTimer.callOnce(_reloadDelay);
		_reloadDelay = std::min(_reloadDelay * 2, kReloadDelayMax);
	}
	refreshControls();
}

void CanvasTab::loadDone(const QJsonObject &json) {
	_loading = false;
	const auto size = QSize(
		int(std::clamp(
			Cloud::JsonInt(json.value(u"width"_q), 1920),
			int64(16),
			int64(8192))),
		int(std::clamp(
			Cloud::JsonInt(json.value(u"height"_q), 1080),
			int64(16),
			int64(8192))));
	const auto background = Cloud::JsonColor(
		json.value(u"background"_q)).value_or(QColor(255, 255, 255));
	const auto eventId = Cloud::JsonInt(json.value(u"event_id"_q));
	const auto list = json.value(u"strokes"_q).toArray();
	auto strokes = std::vector<CanvasStroke>();
	strokes.reserve(std::min(int(list.size()), _limits.strokes));
	for (const auto &value : list) {
		if (int(strokes.size()) >= _limits.strokes) {
			break;
		}
		auto stroke = ParseStroke(
			value.toObject(),
			size,
			_limits.strokePoints);
		if (stroke) {
			strokes.push_back(std::move(*stroke));
		}
	}
	const auto first = !_loaded;
	_model.reset(size, background, std::move(strokes));
	_loaded = true;
	_failed = false;
	for (auto i = begin(_removing); i != end(_removing);) {
		const auto known = _model.contains(*i) || _model.waiting(*i);
		i = known ? (i + 1) : _removing.erase(i);
	}
	for (auto i = begin(_removeSent); i != end(_removeSent);) {
		i = _removing.contains(*i) ? (i + 1) : _removeSent.erase(i);
	}
	const auto lost = base::take(_heldLost);
	_heldNumbers = 0;
	const auto weak = QPointer<CanvasTab>(this);
	for (const auto &event : base::take(_held)) {
		if (!event.id || event.id > eventId) {
			apply(event);
		}
	}
	if (!weak) {
		return;
	}
	// An undo asked for while the board was coming: if its stroke is on
	// the board now (the snapshot has brought it), nothing else will say
	// that it has got its place.
	auto undone = std::vector<QString>();
	for (const auto &id : _removing) {
		if (_model.contains(id) && !_removeSent.contains(id)) {
			undone.push_back(id);
		}
	}
	for (const auto &id : undone) {
		sendRemove(id);
	}
	if (first && !_current) {
		_color = DarkColor(_model.background()) ? 1 : 0;
	}
	updateLayout();
	refreshControls();
	if (base::take(_reloadAgain)) {
		load();
	} else if (lost) {
		// More has happened while the board was coming than is kept: the
		// board may be older than that, it is asked for once more (later
		// and later if it goes on like this).
		_reloadTimer.callOnce(_reloadDelay);
		_reloadDelay = std::min(_reloadDelay * 2, kReloadDelayMax);
	} else {
		_reloadDelay = kReloadDelay;
	}
}

void CanvasTab::handle(const Cloud::Event &event) {
	const auto &type = event.type;
	const auto mine = (type == u"room.stroke"_q)
		|| (type == u"room.stroke_live"_q)
		|| (type == u"room.stroke_removed"_q)
		|| (type == u"room.canvas_cleared"_q);
	if (!mine) {
		return;
	} else if (_loading) {
		// Kept till the board comes: those newer than it are applied.
		if (event.id) {
			hold(event);
		}
		return;
	} else if (!_loaded) {
		return;
	}
	apply(event);
}

// What is kept while the board is being loaded is limited: not more
// events and not more points than a whole board has.
void CanvasTab::hold(const Cloud::Event &event) {
	if (_heldLost) {
		return;
	}
	if (event.type == u"room.stroke"_q) {
		const auto points = event.data.value(
			u"stroke"_q).toObject().value(u"points"_q).toArray().size();
		_heldNumbers += int(std::min(
			int64(points),
			int64(_limits.canvasNumbers)));
	}
	_held.push_back(event);
	if (int(_held.size()) > kHeldLimit
		|| _heldNumbers > _limits.canvasNumbers) {
		_held.clear();
		_heldNumbers = 0;
		_heldLost = true;
	}
}

void CanvasTab::apply(const Cloud::Event &event) {
	const auto &type = event.type;
	const auto &data = event.data;
	if (type == u"room.stroke"_q) {
		auto stroke = ParseStroke(
			data.value(u"stroke"_q).toObject(),
			_model.size(),
			_limits.strokePoints);
		if (!stroke) {
			return;
		}
		const auto id = stroke->id;
		if (_model.add(std::move(*stroke))) {
			confirmed(id);
		}
	} else if (type == u"room.stroke_removed"_q) {
		const auto id = data.value(u"id"_q).toString();
		if (!ValidStrokeId(id)) {
			return;
		}
		_model.remove(id);
		_removing.remove(id);
		_removeSent.remove(id);
	} else if (type == u"room.canvas_cleared"_q) {
		_model.clear(Cloud::JsonColor(
			data.value(u"background"_q)).value_or(_model.background()));
		_removing.clear();
		_removeSent.clear();
		if (_current) {
			cancelStroke();
		}
	} else if (type == u"room.stroke_live"_q) {
		auto part = ParseStroke(data, _model.size(), _limits.livePoints);
		if (!part
			|| !part->userId
			|| ranges::contains(_ownIds, part->id)
			|| !_room->state().member(part->userId)) {
			return;
		}
		const auto limit = _limits.strokePoints * kRawPointsFactor;
		const auto generation = _model.livesGeneration();
		if (_model.addLive(std::move(*part), crl::now(), limit)) {
			if (!_reveal.animating()) {
				_revealLast = crl::now();
				_reveal.start();
			}
			if (!_expireTimer.isActive()) {
				_expireTimer.callEach(1000);
			}
			livesChanged();
		} else if (generation != _model.livesGeneration()) {
			// Nothing was taken, but some previews had to go.
			livesChanged();
		}
		// An event that has changed nothing repaints nothing.
		return;
	} else {
		return;
	}
	refreshControls();
}

// The previews have changed: only the board is painted again, and the
// status line if it names other people now.
void CanvasTab::livesChanged() {
	update(_board);
	auto error = false;
	auto text = statusText(error);
	if (_statusOfLives != text) {
		_statusOfLives = std::move(text);
		update(_status);
	}
}

bool CanvasTab::revealStep(crl::time now) {
	const auto elapsed = now - _revealLast;
	_revealLast = now;
	const auto more = _model.advanceLive(elapsed);
	update(_board);
	return more;
}

// An own stroke has got its place: an undo that was asked for meanwhile
// can be sent now.
void CanvasTab::confirmed(const QString &id) {
	if (_removing.contains(id) && _model.contains(id)) {
		sendRemove(id);
	}
}

// ---- Drawing.

void CanvasTab::toast(const QString &text) {
	if (_show && _show->valid() && !text.isEmpty()) {
		_show->showToast(text);
	}
}

bool CanvasTab::startAllowed() {
	if (_room->state().gone != Gone::No || disabledOnServer()) {
		return false;
	} else if (!_room->can(Right::Draw)) {
		toast(tr::lng_oblivion_rcanvas_error_right(tr::now));
		return false;
	} else if (_room->sample()) {
		return true;
	} else if (!_loaded) {
		if (_failed && !_loading) {
			load();
		}
		return false;
	} else if (!_room->connected()) {
		toast(tr::lng_oblivion_rcanvas_status_offline(tr::now));
		return false;
	} else if (_model.full(_limits, 1)) {
		toast(tr::lng_oblivion_rcanvas_status_full(tr::now));
		return false;
	}
	return true;
}

void CanvasTab::rememberOwn(const QString &id) {
	_ownIds.push_back(id);
	if (int(_ownIds.size()) > kOwnIdsLimit) {
		_ownIds.erase(begin(_ownIds), begin(_ownIds) + kOwnIdsLimit / 2);
	}
}

void CanvasTab::beginStroke(QPoint point) {
	const auto base = kSizes[std::clamp(_size, 0, kSizesCount - 1)];
	auto stroke = CanvasStroke();
	stroke.id = NewStrokeId();
	stroke.userId = _room->selfId();
	stroke.tool = _tool;
	stroke.color = SwatchColor(_color);
	stroke.alpha = (_tool == CanvasTool::Marker) ? kMarkerAlpha : 255;
	stroke.size = std::min(
		base * ((_tool == CanvasTool::Marker)
			? kMarkerFactor
			: (_tool == CanvasTool::Eraser)
			? kEraserFactor
			: 1),
		256);
	stroke.points.push_back(point);
	rememberOwn(stroke.id);
	_current = std::move(stroke);
	_liveSent = 0;
	if (!_room->sample()) {
		_liveTimer.callEach(kLiveInterval);
	}
	update(strokeRect(point, point, _current->size));
}

void CanvasTab::extendStroke(QPoint point) {
	if (!_current) {
		return;
	}
	const auto last = _current->points.back();
	const auto delta = point - last;
	if (delta.x() * delta.x() + delta.y() * delta.y()
		< kMinDistance * kMinDistance) {
		return;
	}
	_current->points.push_back(point);
	const auto count = int(_current->points.size());
	const auto size = _current->size;
	// The smoothing moves the line near the two previous points too.
	auto dirty = strokeRect(last, point, size);
	if (count > 2) {
		dirty = dirty.united(
			strokeRect(_current->points[count - 3], last, size));
	}
	// Only where the new point has changed the line (the picture of the
	// line being drawn gets only that part, see StrokeLayer), a marker
	// line too: it is not stroked as a whole for every move of the mouse.
	update(dirty);
	if (count >= _limits.strokePoints * kRawPointsFactor) {
		// A very long line goes on as the next stroke.
		finishStroke();
		if (startAllowed()) {
			beginStroke(point);
		}
	}
}

void CanvasTab::cancelStroke() {
	_liveTimer.cancel();
	_current = std::nullopt;
	_currentLayer.reset();
	update(_board);
}

void CanvasTab::finishStroke() {
	if (!_current) {
		return;
	}
	_liveTimer.cancel();
	auto stroke = *base::take(_current);
	_currentLayer.reset();
	stroke.points = SimplifyStroke(stroke.points, _limits.strokePoints);
	update(_board);
	if (_room->sample()) {
		stroke.seq = _model.strokes().empty()
			? 1
			: (_model.strokes().back()->seq + 1);
		_model.add(std::move(stroke));
		refreshControls();
		return;
	}
	const auto id = stroke.id;
	auto body = SerializeStroke(stroke);
	const auto size = _model.size();
	const auto limit = _limits.strokePoints;
	_model.addPending(std::move(stroke));
	const auto sent = _room->send(
		Cloud::PostRequest(u"/canvas/strokes"_q, std::move(body)),
		crl::guard(this, [=](const Cloud::Response &response) {
			auto confirmedStroke = ParseStroke(
				response.json.value(u"stroke"_q).toObject(),
				size,
				limit);
			if (confirmedStroke
				&& confirmedStroke->id == id
				&& !_loading
				&& _model.size() == size
				&& _model.waiting(id)) {
				// Only while it still waits here. The event may have
				// brought it already, and if the board was cleared or
				// loaded again meanwhile, the events are what says
				// whether the stroke is on it: an answer that is late
				// must not bring back a stroke somebody has cleared.
				if (_model.add(std::move(*confirmedStroke))) {
					confirmed(id);
				}
				refreshControls();
			}
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			strokeFailed(id, error);
		}));
	if (!sent) {
		_model.removePending(id);
	}
	refreshControls();
}

void CanvasTab::strokeFailed(const QString &id, const Cloud::Error &error) {
	if (error.is("conflict")) {
		// The server has it already, the event brings it.
		return;
	}
	_model.removePending(id);
	_removing.remove(id);
	_removeSent.remove(id);
	refreshControls();
	if (error.type == Cloud::Error::Type::Cancelled) {
		return;
	}
	const auto now = crl::now();
	if (_lastErrorToast && now - _lastErrorToast < kErrorToastInterval) {
		return;
	}
	_lastErrorToast = now;
	if (error.is("limit_reached")) {
		toast(tr::lng_oblivion_rcanvas_status_full(tr::now));
	} else if (error.is("forbidden")) {
		toast(tr::lng_oblivion_rcanvas_error_right(tr::now));
	} else {
		Cloud::ShowError(_show, error);
	}
}

void CanvasTab::sendLive() {
	if (!_current || _room->sample()) {
		_liveTimer.cancel();
		return;
	}
	const auto total = int(_current->points.size());
	if (total <= _liveSent || _liveInFlight >= kLiveInFlightLimit) {
		return;
	}
	const auto count = std::min(total - _liveSent, _limits.livePoints);
	auto body = SerializeLive(*_current, _liveSent, count);
	const auto finished = crl::guard(this, [=] {
		_liveInFlight = std::max(_liveInFlight - 1, 0);
	});
	// A preview that did not get through is not worth a word: the
	// finished stroke follows anyway.
	const auto sent = _room->send(
		Cloud::PostRequest(u"/canvas/live"_q, std::move(body)),
		[=](const Cloud::Response &) { finished(); },
		[=](const Cloud::Error &) { finished(); });
	if (sent) {
		++_liveInFlight;
		_liveSent += count;
	}
}

void CanvasTab::undo() {
	if (!canDraw()) {
		return;
	}
	finishStroke();
	const auto id = _model.lastOwn(_room->selfId(), _removing);
	if (id.isEmpty()) {
		return;
	} else if (_room->sample()) {
		_model.remove(id);
		refreshControls();
		return;
	}
	_removing.emplace(id);
	if (_model.contains(id)) {
		sendRemove(id);
	}
	refreshControls();
}

void CanvasTab::sendRemove(const QString &id) {
	if (!_removeSent.emplace(id).second) {
		// The server was asked already.
		return;
	}
	const auto sent = _room->send(
		Cloud::DeleteRequest(u"/canvas/strokes/"_q + id),
		nullptr,
		crl::guard(this, [=](const Cloud::Error &error) {
			_removing.remove(id);
			_removeSent.remove(id);
			refreshControls();
			if (error.status != 404
				&& error.type != Cloud::Error::Type::Cancelled) {
				Cloud::ShowError(_show, error);
			}
		}));
	if (!sent) {
		_removing.remove(id);
		_removeSent.remove(id);
	}
}

void CanvasTab::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	setFocus();
	const auto position = e->pos();
	if (const auto target = targetAt(position)) {
		if (canDraw()) {
			if (target->kind == Target::Kind::Size) {
				chooseTool(_tool, _color, target->index);
			} else {
				chooseTool(
					(_tool == CanvasTool::Eraser) ? CanvasTool::Pen : _tool,
					target->index,
					_size);
			}
		}
		return;
	} else if ((_status.contains(position)
			|| (_board.contains(position) && !_loaded))
		&& _failed
		&& !_loading) {
		// The «try again» of a board that has not come is the board itself,
		// for those who may not draw too.
		load();
		return;
	} else if (!_board.contains(position)) {
		return;
	}
	finishStroke();
	if (startAllowed()) {
		beginStroke(toCanvas(position));
	}
}

void CanvasTab::mouseMoveEvent(QMouseEvent *e) {
	const auto position = e->pos();
	if (_current) {
		if (e->buttons() & Qt::LeftButton) {
			extendStroke(toCanvas(position));
		} else {
			finishStroke();
		}
		return;
	}
	// A board that has failed to come is one large "try again" button.
	const auto pointer = (targetAt(position) && canDraw())
		|| (_status.contains(position) && _failed && !_loading)
		|| (_board.contains(position) && _failed && !_loading && !_loaded);
	setCursor(pointer
		? style::cur_pointer
		: (_board.contains(position) && canDraw() && _loaded)
		? style::cur_cross
		: style::cur_default);
}

void CanvasTab::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && _current) {
		extendStroke(toCanvas(e->pos()));
		finishStroke();
	}
}

void CanvasTab::keyPressEvent(QKeyEvent *e) {
	if (e->matches(QKeySequence::Undo)) {
		undo();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

// ---- The picture of the finished strokes.

void CanvasTab::syncLayer() {
	const auto wanted = _board.size() * style::DevicePixelRatio();
	if (wanted.isEmpty()) {
		return;
	}
	const auto &strokes = _model.strokes();
	const auto valid = !_layer.isNull()
		&& (_layer.size() == wanted)
		&& (_layerGeneration == _model.generation());
	if (valid) {
		const auto count = int(strokes.size());
		if (_layerStrokes < count && !_rebuilding) {
			// New strokes on top: painted over what there is. Right here
			// only the user's own ones (a hand has drawn them) and what
			// is light enough of the others: a stroke of another member
			// may take seconds to paint.
			const auto self = _room->selfId();
			const auto sample = _room->sample();
			auto left = kSyncPaintWeight;
			auto p = std::optional<QPainter>();
			while (_layerStrokes < count) {
				const auto &stroke = *strokes[_layerStrokes];
				if (!sample && stroke.userId != self) {
					left -= StrokeWeight(stroke);
					if (left < 0) {
						break;
					}
				}
				if (!p) {
					p.emplace(&_layer);
					p->setRenderHint(QPainter::Antialiasing);
					const auto canvas = _model.size();
					p->scale(
						wanted.width() / double(canvas.width()),
						wanted.height() / double(canvas.height()));
				}
				PaintStroke(*p, stroke, _model.background());
				++_layerStrokes;
			}
			p.reset();
			if (_layerStrokes < count) {
				startRebuild(wanted, _layerStrokes);
			}
		}
		return;
	} else if (_rebuilding) {
		// One at a time: the board as it is by then is painted when the
		// picture that is being made is ready (see rebuildDone).
		return;
	} else if (!_layer.isNull()) {
		if (_rebuildTimer.isActive()) {
			return;
		}
		// A member can remove strokes as fast as he likes: the whole
		// board is not painted again for every one of them, the picture
		// there is stays for a moment.
		const auto passed = crl::now() - _rebuiltAt;
		if (!_room->sample()
			&& _rebuiltAt
			&& passed >= 0
			&& passed < kRebuildInterval) {
			_rebuildTimer.callOnce(kRebuildInterval - passed);
			return;
		}
	}
	startRebuild(wanted, 0);
}

// from == 0: the whole board is painted again. Otherwise the strokes
// from that one are painted over the picture there is, off the main
// thread (they are too heavy for it).
void CanvasTab::startRebuild(QSize wanted, int from) {
	const auto generation = _model.generation();
	const auto &strokes = _model.strokes();
	const auto count = int(strokes.size());
	const auto canvas = _model.size();
	const auto background = _model.background();
	if (!from) {
		_rebuiltAt = crl::now();
		if (_room->sample() || _model.weight() <= kSyncPaintWeight) {
			_layer = RenderCanvas(strokes, count, canvas, background, wanted);
			_layerGeneration = generation;
			_layerStrokes = count;
			// Done right here, on the main thread: the pause before the
			// next one is counted from its end.
			_rebuiltAt = crl::now();
			return;
		}
	}
	// A heavy board is painted off the main thread, the old picture stays
	// till the new one is ready. Never two at once: what changes while
	// one is being made waits for it.
	_rebuilding = true;
	const auto weak = QPointer<CanvasTab>(this);
	// The pixels are copied where they are painted over, in that thread.
	auto under = from ? _layer : QImage();
	crl::async([=, copy = strokes, under = std::move(under)]() mutable {
		auto image = QImage();
		if (!from) {
			image = RenderCanvas(copy, count, canvas, background, wanted);
		} else if (!under.isNull() && !canvas.isEmpty()) {
			image = std::move(under);
			auto p = QPainter(&image);
			p.setRenderHint(QPainter::Antialiasing);
			p.scale(
				image.width() / double(canvas.width()),
				image.height() / double(canvas.height()));
			const auto fill = OpaqueOr(background, QColor(255, 255, 255));
			const auto till = std::min(count, int(copy.size()));
			for (auto i = from; i < till; ++i) {
				PaintStroke(p, *copy[i], fill);
			}
		}
		crl::on_main([=, image = std::move(image)]() mutable {
			if (const auto strong = weak.data()) {
				strong->rebuildDone(
					generation,
					wanted,
					from,
					count,
					std::move(image));
			}
		});
	});
}

void CanvasTab::rebuildDone(
		int generation,
		QSize size,
		int from,
		int count,
		QImage &&image) {
	_rebuilding = false;
	const auto fits = !image.isNull()
		&& (size == _board.size() * style::DevicePixelRatio());
	if (fits && !from) {
		// Also when the board has changed meanwhile: this picture is
		// newer than the one on the screen, and the paint that follows
		// sees that it is not the last one and asks for the next. So the
		// board goes on changing on the screen however often strokes
		// are removed.
		_layer = std::move(image);
		_layerGeneration = generation;
		_layerStrokes = count;
	} else if (fits
		&& (_layer.size() == size)
		&& (_layerGeneration == generation)
		&& (_model.generation() == generation)
		&& (_layerStrokes == from)) {
		// The picture there was with more strokes on top.
		_layer = std::move(image);
		_layerStrokes = count;
	} else if (from && image.isNull()) {
		// No picture came back at all: the board is painted from its
		// first stroke (not at once, see syncLayer).
		_layerGeneration = -1;
	}
	update(_board);
}

// ---- The picture of what the others draw now.

void CanvasTab::syncLives() {
	const auto &lives = _model.lives();
	const auto wanted = _board.size() * style::DevicePixelRatio();
	if (lives.empty() || wanted.isEmpty()) {
		// Nothing of the picture is shown till it is painted again (-1).
		// Its memory is given back by the timer of the previews: they
		// come and go too often to do that here.
		_livesPainted.clear();
		_livesGeneration = -1;
		return;
	}
	const auto continued = !_livesLayer.isNull()
		&& (_livesLayer.size() == wanted)
		&& (_livesGeneration == _model.livesGeneration());
	const auto now = crl::now();
	const auto wait = [&](crl::time delay) {
		// The picture there is stays for a moment.
		if (!_livesTimer.isActive()) {
			_livesTimer.callOnce(std::max(delay, crl::time(1)));
		}
	};
	// A snapshot scene is painted once, as it is.
	const auto paced = !_room->sample();
	if (paced
		&& now < _livesNotBefore
		&& now + kLivesPaceMax >= _livesNotBefore) {
		// Painting the previews has taken a noticeable time (that needs
		// a member who sends lines no hand draws): they take only a part
		// of the time of the main thread, whatever comes.
		wait(_livesNotBefore - now);
		return;
	}
	if (!continued) {
		// A preview is gone or the board has another size: the others
		// are painted from their first points. That is the costly thing
		// here (up to kLiveWeightBudget), and a member can make previews
		// come and go as fast as he likes, so it is done not more often
		// than once in kLivesRepaintInterval.
		const auto passed = now - _livesRepainted;
		if (paced
			&& !_livesLayer.isNull()
			&& _livesRepainted
			&& passed >= 0
			&& passed < kLivesRepaintInterval) {
			wait(kLivesRepaintInterval - passed);
			return;
		}
		if (_livesLayer.size() != wanted) {
			_livesLayer = QImage(wanted, QImage::Format_ARGB32_Premultiplied);
			if (_livesLayer.isNull()) {
				_livesPainted.clear();
				_livesGeneration = -1;
				return;
			}
		}
		_livesLayer.fill(Qt::transparent);
		_livesPainted.clear();
		_livesGeneration = _model.livesGeneration();
	}
	auto spent = QElapsedTimer();
	spent.start();
	PaintLivesGrowth(
		_livesLayer,
		lives,
		_livesPainted,
		_model.size(),
		_model.background());
	const auto finished = crl::now();
	if (!continued) {
		// The pause before the next one is counted from its end.
		_livesRepainted = finished;
	}
	// The few new points of lines drawn by hands take no time at all.
	_livesNotBefore = finished + std::min(
		crl::time(spent.elapsed()) * kLivesPaceFactor,
		kLivesPaceMax);
}

// ---- The menu.

void CanvasTab::showMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	_menu->addAction(
		tr::lng_oblivion_rcanvas_save_png(tr::now),
		[=] { savePng(); },
		&st::menuIconDownload);
	if (_room->session()) {
		_menu->addAction(
			tr::lng_oblivion_rcanvas_open_editor(tr::now),
			[=] { openEditor(); },
			&st::menuIconEdit);
	}
	if (canDraw() && (_loaded || _room->sample())) {
		_menu->addSeparator();
		_menu->addAction(
			tr::lng_oblivion_rcanvas_clear(tr::now),
			[=] { clearRequested(false); },
			&st::menuIconDelete);
		_menu->addAction(
			(DarkColor(_model.background())
				? tr::lng_oblivion_rcanvas_clear_light(tr::now)
				: tr::lng_oblivion_rcanvas_clear_dark(tr::now)),
			[=] { clearRequested(true); },
			&st::menuIconPalette);
	}
	_menu->popup(_more->mapToGlobal(QPoint(0, _more->height())));
}

void CanvasTab::clearRequested(bool swapBackground) {
	if (!_show || !_show->valid()) {
		return;
	}
	finishStroke();
	const auto weak = QPointer<CanvasTab>(this);
	_show->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_rcanvas_clear_sure(tr::now),
		.confirmed = [=](Fn<void()> close) {
			if (const auto strong = weak.data()) {
				strong->sendClear(swapBackground);
			}
			close();
		},
		.confirmText = tr::lng_oblivion_rcanvas_clear_button(tr::now),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

void CanvasTab::sendClear(bool swapBackground) {
	const auto background = !swapBackground
		? _model.background()
		: DarkColor(_model.background())
		? QColor(QString::fromLatin1(kLightBackground))
		: QColor(QString::fromLatin1(kDarkBackground));
	if (_room->sample()) {
		_model.clear(background);
		_removing.clear();
		_removeSent.clear();
		refreshControls();
		return;
	} else if (!canDraw()) {
		return;
	}
	auto body = QJsonObject();
	if (swapBackground) {
		body.insert(u"background"_q, background.name(QColor::HexRgb));
	}
	// The board changes when the event comes, a failure is one toast of
	// the room window.
	_room->send(Cloud::PostRequest(u"/canvas/clear"_q, std::move(body)));
}

void CanvasTab::render(Fn<void(QImage)> done) {
	auto copy = _model.strokes();
	for (const auto &stroke : _model.pending()) {
		copy.push_back(stroke);
	}
	const auto count = int(copy.size());
	const auto canvas = _model.size();
	const auto background = _model.background();
	const auto weak = QPointer<CanvasTab>(this);
	crl::async([=, copy = std::move(copy)] {
		auto image = RenderCanvas(copy, count, canvas, background, canvas);
		crl::on_main([=, image = std::move(image)]() mutable {
			if (weak) {
				done(std::move(image));
			}
		});
	});
}

void CanvasTab::savePng() {
	finishStroke();
	const auto show = _show;
	FileDialog::GetWritePath(
		this,
		tr::lng_oblivion_rcanvas_save_title(tr::now),
		u"PNG (*.png)"_q,
		filedialogDefaultName(u"oblivion-canvas"_q, u".png"_q),
		crl::guard(this, [=](QString &&path) {
			if (path.isEmpty()) {
				return;
			}
			const auto target = path;
			render([=](QImage image) {
				crl::async([=, image = std::move(image)] {
					const auto saved = !image.isNull()
						&& image.save(target, "PNG");
					crl::on_main([=] {
						if (!show || !show->valid()) {
							return;
						}
						show->showToast(saved
							? tr::lng_oblivion_rcanvas_saved(
								tr::now,
								lt_path,
								QDir::toNativeSeparators(target))
							: tr::lng_oblivion_rcanvas_save_failed(tr::now));
					});
				});
			});
		}));
}

void CanvasTab::openEditor() {
	finishStroke();
	const auto session = base::make_weak(_room->session());
	const auto show = _show;
	render([=](QImage image) {
		const auto strong = session.get();
		if (!strong || image.isNull()) {
			return;
		}
		// The editor needs room: it opens in the Telegram window of this
		// account, not in the narrow window of the room.
		if (const auto window = strong->tryResolveWindow()) {
			Oblivion::ShowPhotoEditorWithImage(
				window,
				std::move(image),
				u"oblivion-canvas"_q);
			window->window().activate();
		} else if (show && show->valid()) {
			show->showToast(tr::lng_oblivion_rcanvas_editor_no_window(tr::now));
		}
	});
}

const auto CanvasTabRegistration = TabRegistrar([] {
	return TabDescriptor{
		.id = u"canvas"_q,
		.order = 300,
		.title = [] { return tr::lng_oblivion_rcanvas_tab(tr::now); },
		.create = [](QWidget *parent, TabContext context) {
			return object_ptr<Ui::RpWidget>::fromRaw(
				Ui::CreateChild<CanvasTab>(parent, std::move(context)));
		},
	};
});

// ---- The self-test.

class Checker final {
public:
	explicit Checker(QStringList &log) : _log(log) {
	}

	void operator()(bool condition, const char *what) {
		if (condition) {
			++_passed;
			++_sectionPassed;
		} else {
			++_failed;
			++_sectionFailed;
			_log.push_back(u"FAILED: "_q + QString::fromUtf8(what));
		}
	}
	void section(const char *name) {
		_log.push_back(u"%1: %2 passed, %3 failed"_q.arg(
			QString::fromUtf8(name),
			QString::number(_sectionPassed),
			QString::number(_sectionFailed)));
		_sectionPassed = _sectionFailed = 0;
	}
	[[nodiscard]] int passed() const {
		return _passed;
	}
	[[nodiscard]] int failed() const {
		return _failed;
	}

private:
	QStringList &_log;
	int _passed = 0;
	int _failed = 0;
	int _sectionPassed = 0;
	int _sectionFailed = 0;

};

[[nodiscard]] QJsonArray Numbers(std::initializer_list<int> values) {
	auto result = QJsonArray();
	for (const auto value : values) {
		result.push_back(value);
	}
	return result;
}

[[nodiscard]] CanvasStroke TestStroke(
		const QString &id,
		int64 seq,
		uint64 userId,
		int points = 3) {
	auto result = CanvasStroke();
	result.id = id;
	result.seq = seq;
	result.userId = userId;
	for (auto i = 0; i != points; ++i) {
		result.points.push_back(QPoint(10 * i, 5 * i));
	}
	return result;
}

void TestCodec(Checker &check) {
	const auto canvas = QSize(1920, 1080);
	check(ValidStrokeId(u"s-7f3a91c2"_q), "a usual id is valid");
	check(ValidStrokeId(QString(32, QChar('a'))), "32 chars are valid");
	check(!ValidStrokeId(QString()), "an empty id is not valid");
	check(!ValidStrokeId(QString(33, QChar('a'))), "33 chars are too long");
	check(!ValidStrokeId(u"a b"_q), "a space is not valid");
	check(!ValidStrokeId(u"a/b"_q), "a slash is not valid");
	check(!ValidStrokeId(QString::fromUtf8("штрих")), "letters are latin");
	const auto first = NewStrokeId();
	check(ValidStrokeId(first) && first != NewStrokeId(), "new ids differ");

	auto object = QJsonObject();
	object.insert(u"id"_q, u"s-7f3a91c2"_q);
	object.insert(u"seq"_q, 41);
	object.insert(u"user_id"_q, 123);
	object.insert(u"tool"_q, u"marker"_q);
	object.insert(u"color"_q, u"#ff3b30"_q);
	object.insert(u"alpha"_q, 110);
	object.insert(u"size"_q, 6);
	object.insert(u"points"_q, Numbers({ 100, 200, 104, 207, 110, 215 }));
	const auto parsed = ParseStroke(object, canvas, 2000);
	check(parsed.has_value(), "a stroke of the protocol is parsed");
	if (parsed) {
		check(parsed->id == u"s-7f3a91c2"_q
			&& parsed->seq == 41
			&& parsed->userId == 123, "id, seq and author");
		check(parsed->tool == CanvasTool::Marker
			&& parsed->color == QColor(0xff, 0x3b, 0x30)
			&& parsed->alpha == 110
			&& parsed->size == 6, "tool, colour, alpha and size");
		check(parsed->points.size() == 3
			&& parsed->points[0] == QPoint(100, 200)
			&& parsed->points[2] == QPoint(110, 215), "points");
		const auto body = SerializeStroke(*parsed);
		check(!body.contains(u"seq"_q) && !body.contains(u"user_id"_q),
			"the body has no seq and no author");
		check(body.value(u"tool"_q).toString() == u"marker"_q
			&& body.value(u"color"_q).toString() == u"#ff3b30"_q
			&& body.value(u"alpha"_q).toInt() == 110
			&& body.value(u"size"_q).toInt() == 6
			&& body.value(u"points"_q).toArray().size() == 6,
			"the body has the fields of the protocol");
		const auto again = ParseStroke(body, canvas, 2000);
		check(again && again->points == parsed->points
			&& again->tool == parsed->tool
			&& again->color == parsed->color, "round trip");
		const auto live = SerializeLive(*parsed, 1, 5);
		const auto points = live.value(u"points"_q).toArray();
		check(points.size() == 4 && points.at(0).toInt() == 104
			&& points.at(3).toInt() == 215, "a live part is a range");
		check(SerializeLive(*parsed, 3, 5).value(
			u"points"_q).toArray().isEmpty(), "a live part past the end");
	}

	auto plain = QJsonObject();
	plain.insert(u"id"_q, u"a"_q);
	plain.insert(u"points"_q, Numbers({ 5, 6 }));
	const auto defaults = ParseStroke(plain, canvas, 2000);
	check(defaults
		&& defaults->tool == CanvasTool::Pen
		&& defaults->color == QColor(0, 0, 0)
		&& defaults->alpha == 255
		&& defaults->size == 4
		&& defaults->points.size() == 1, "the defaults of the protocol");

	plain.insert(u"tool"_q, u"spray"_q);
	plain.insert(u"alpha"_q, 999);
	plain.insert(u"size"_q, 9999);
	plain.insert(u"color"_q, u"javascript:1"_q);
	plain.insert(u"points"_q, Numbers({ -50, 5000, 99999, -1, 7 }));
	const auto clamped = ParseStroke(plain, canvas, 2000);
	check(clamped
		&& clamped->tool == CanvasTool::Pen
		&& clamped->alpha == 255
		&& clamped->size == 256
		&& clamped->color == QColor(0, 0, 0), "wrong values are clamped");
	check(clamped
		&& clamped->points.size() == 2
		&& clamped->points[0] == QPoint(0, 1080)
		&& clamped->points[1] == QPoint(1920, 0),
		"points are clamped to the canvas, an odd number is dropped");
	plain.insert(u"size"_q, 0);
	plain.insert(u"alpha"_q, -4);
	const auto low = ParseStroke(plain, canvas, 2000);
	check(low && low->size == 1 && low->alpha == 1, "the lower bounds");

	auto bad = plain;
	bad.remove(u"id"_q);
	check(!ParseStroke(bad, canvas, 2000), "no id: dropped");
	bad.insert(u"id"_q, u"bad id"_q);
	check(!ParseStroke(bad, canvas, 2000), "a wrong id: dropped");
	bad.insert(u"id"_q, u"ok"_q);
	bad.insert(u"points"_q, QJsonArray());
	check(!ParseStroke(bad, canvas, 2000), "no points: dropped");
	bad.insert(u"points"_q, Numbers({ 1 }));
	check(!ParseStroke(bad, canvas, 2000), "half a point: dropped");
	auto strings = QJsonArray();
	strings.push_back(u"1"_q);
	strings.push_back(2);
	bad.insert(u"points"_q, strings);
	check(!ParseStroke(bad, canvas, 2000), "a text for a number: dropped");
	bad.insert(u"points"_q, u"1,2"_q);
	check(!ParseStroke(bad, canvas, 2000), "points not a list: dropped");

	auto many = QJsonArray();
	for (auto i = 0; i != 5000; ++i) {
		many.push_back(i % 1900);
		many.push_back(i % 1000);
	}
	bad.insert(u"points"_q, many);
	const auto cut = ParseStroke(bad, canvas, 2000);
	check(cut && cut->points.size() == 2000, "too many points are cut");
	check(CanvasToolName(CanvasTool::Eraser) == u"eraser"_q
		&& CanvasToolName(CanvasTool::Pen) == u"pen"_q, "tool names");
}

void TestSimplify(Checker &check) {
	auto line = std::vector<QPoint>();
	for (auto i = 0; i != 100; ++i) {
		line.push_back(QPoint(i * 3, i * 3));
	}
	const auto straight = SimplifyStroke(line, 2000);
	check(straight.size() == 2
		&& straight.front() == line.front()
		&& straight.back() == line.back(), "a straight line is two points");

	auto corner = std::vector<QPoint>();
	for (auto i = 0; i <= 50; ++i) {
		corner.push_back(QPoint(i * 4, 0));
	}
	for (auto i = 1; i <= 50; ++i) {
		corner.push_back(QPoint(200, i * 4));
	}
	const auto bent = SimplifyStroke(corner, 2000);
	check(bent.size() == 3 && bent[1] == QPoint(200, 0),
		"a corner is kept");

	auto circle = std::vector<QPoint>();
	for (auto i = 0; i != 5000; ++i) {
		const auto angle = i * 2. * M_PI / 5000.;
		circle.push_back(QPoint(
			int(std::lround(900 + 500 * std::cos(angle))),
			int(std::lround(500 + 400 * std::sin(angle)))));
	}
	const auto round = SimplifyStroke(circle, 100, 0.);
	check(int(round.size()) <= 100
		&& round.front() == circle.front()
		&& round.back() == circle.back(),
		"the limit is kept with the ends in place");
	const auto smooth = SimplifyStroke(circle, 2000);
	check(smooth.size() > 20 && smooth.size() < circle.size(),
		"a circle keeps its shape with fewer points");
	check(SimplifyStroke({}, 10).empty(), "nothing stays nothing");
	check(SimplifyStroke({ QPoint(1, 2) }, 10).size() == 1, "one point");
	check(SimplifyStroke({ QPoint(1, 2), QPoint(1, 2), QPoint(1, 2) }, 10)
		.size() == 2, "the same point thrice is its two ends");

	const auto points = std::vector<QPoint>{
		QPoint(0, 0),
		QPoint(100, 0),
		QPoint(100, 100),
	};
	const auto whole = StrokePath(points);
	check(!whole.isEmpty()
		&& whole.currentPosition() == QPointF(100, 100),
		"a path ends in the last point");
	const auto part = StrokePath(points, 1.5);
	check(part.currentPosition() == QPointF(50, 0),
		"a part of a path ends between the points");
	check(StrokePath(points, 99.).currentPosition() == QPointF(100, 100),
		"more than there is: the whole path");
	check(StrokePath({}).isEmpty(), "no points: no path");
}

void TestModel(Checker &check) {
	const auto self = uint64(7);
	const auto other = uint64(8);
	auto model = CanvasModel();
	auto strokes = std::vector<CanvasStroke>();
	strokes.push_back(TestStroke(u"c"_q, 3, other));
	strokes.push_back(TestStroke(u"a"_q, 1, self));
	strokes.push_back(TestStroke(u"b"_q, 2, other));
	strokes.push_back(TestStroke(u"a"_q, 9, other));
	model.reset(QSize(1920, 1080), QColor(255, 255, 255), std::move(strokes));
	check(model.strokes().size() == 3
		&& model.strokes()[0]->id == u"a"_q
		&& model.strokes()[2]->id == u"c"_q,
		"a snapshot is sorted, a repeated id is dropped");
	check(model.numbers() == 18, "the numbers of a snapshot are counted");

	auto generation = model.generation();
	check(model.add(TestStroke(u"d"_q, 4, self))
		&& model.generation() == generation
		&& model.strokes().back()->id == u"d"_q,
		"the next stroke is appended");
	check(!model.add(TestStroke(u"d"_q, 4, self))
		&& model.strokes().size() == 4, "the same stroke twice is one");
	check(model.add(TestStroke(u"e"_q, 2, other))
		&& model.generation() == generation + 1
		&& model.strokes()[2]->id == u"e"_q,
		"a late stroke takes its place and asks for a repaint");
	check(model.numbers() == 30, "numbers follow the strokes");

	generation = model.generation();
	check(model.remove(u"b"_q)
		&& model.generation() == generation + 1
		&& !model.contains(u"b"_q)
		&& model.numbers() == 24, "a stroke is removed");
	check(!model.remove(u"zzz"_q) && model.generation() == generation + 1,
		"an unknown stroke changes nothing");

	model.addPending(TestStroke(u"p1"_q, 0, self));
	model.addPending(TestStroke(u"p2"_q, 0, self));
	check(model.pending().size() == 2, "own strokes wait");
	auto skip = base::flat_set<QString>();
	check(model.lastOwn(self, skip) == u"p2"_q, "the undo takes the newest");
	skip.emplace(u"p2"_q);
	check(model.lastOwn(self, skip) == u"p1"_q, "the undo skips what goes");
	skip.emplace(u"p1"_q);
	check(model.lastOwn(self, skip) == u"d"_q,
		"then the newest confirmed own stroke");
	check(model.lastOwn(999, skip).isEmpty(), "nothing of a stranger");
	check(model.waiting(u"p1"_q)
		&& model.waiting(u"p2"_q)
		&& !model.waiting(u"d"_q)
		&& !model.waiting(u"zzz"_q),
		"only the strokes that were sent wait for the server");
	check(model.add(TestStroke(u"p1"_q, 10, self))
		&& model.pending().size() == 1
		&& model.strokes().back()->id == u"p1"_q
		&& !model.waiting(u"p1"_q),
		"a confirmed stroke leaves the waiting ones");
	check(model.removePending(u"p2"_q) && !model.removePending(u"p2"_q),
		"a failed stroke is dropped once");

	auto limits = CanvasLimits();
	limits.strokes = 5;
	limits.canvasNumbers = 40;
	check(model.strokes().size() == 5 && model.full(limits, 1),
		"the strokes limit");
	limits.strokes = 100;
	check(!model.full(limits, 5) && model.full(limits, 6),
		"the points limit");
	model.addPending(TestStroke(u"p3"_q, 0, self, 4));
	check(model.full(limits, 2) && !model.full(limits, 1),
		"waiting strokes count too");

	// Previews.
	auto now = crl::time(1000);
	check(model.addLive(TestStroke(u"l1"_q, 0, other, 3), now, 10)
		&& model.lives().size() == 1
		&& model.lives()[0].shown == 1., "a preview starts with a point");
	check(model.addLive(TestStroke(u"l1"_q, 0, other, 4), now + 50, 10)
		&& model.lives()[0].stroke.points.size() == 7,
		"a preview grows");
	check(model.addLive(TestStroke(u"l1"_q, 0, other, 9), now + 100, 10)
		&& model.lives()[0].stroke.points.size() == 10,
		"a preview is capped");
	check(!model.addLive(TestStroke(u"l1"_q, 0, other, 9), now + 150, 10),
		"a full preview takes nothing");
	check(!model.addLive(TestStroke(u"d"_q, 0, self, 2), now, 10),
		"no preview of a finished stroke");
	check(!model.addLive(TestStroke(u"l2"_q, 0, other, 0), now, 10),
		"no preview without points");
	check(model.advanceLive(40) && model.lives()[0].shown > 1.
		&& model.lives()[0].shown < 10., "a preview is revealed in steps");
	auto steps = 0;
	while (model.advanceLive(16) && steps < 1000) {
		++steps;
	}
	check(steps < 100 && model.lives()[0].shown == 10.,
		"a preview is revealed completely and soon");
	check(!model.expireLive(now + 150 + 4000) && model.lives().size() == 1,
		"a fresh preview stays");
	check(model.addLive(TestStroke(u"l3"_q, 0, other, 2), now + 4000, 10),
		"another preview");
	check(model.expireLive(now + 150 + 5001) && model.lives().size() == 1
		&& model.lives()[0].stroke.id == u"l3"_q,
		"a preview nobody finished goes away");
	check(model.add(TestStroke(u"l3"_q, 11, other))
		&& model.lives().empty(), "a finished stroke replaces its preview");

	// The previews are what other members send: they are limited.
	const auto third = uint64(9);
	const auto has = [](const CanvasModel &where, const QString &id) {
		return ranges::contains(
			where.lives(),
			id,
			[](const CanvasModel::Live &live) { return live.stroke.id; });
	};
	const auto empty = [] {
		auto result = CanvasModel();
		result.reset(QSize(1920, 1080), QColor(255, 255, 255), {});
		return result;
	};
	{
		auto limited = empty();
		check(limited.addLive(TestStroke(u"f"_q, 0, other, 10), now, 10)
			&& !limited.addLive(
				TestStroke(u"f"_q, 0, other, 5),
				now + 4900,
				10)
			&& limited.expireLive(now + 5001)
			&& limited.lives().empty(),
			"a full preview is not kept alive by what it does not take");

		auto wide = TestStroke(u"w"_q, 0, other, 2);
		wide.size = 256;
		check(limited.addLive(std::move(wide), now, 10)
			&& limited.lives()[0].stroke.size == kLiveMaxSize,
			"a preview is not wider than the tools draw");
		check(limited.lives()[0].weight == 2 * kPointWeight + 15
			&& limited.liveWeight() == limited.lives()[0].weight,
			"the weight of a preview is its points and its length");

		const auto generation = limited.livesGeneration();
		check(limited.addLive(TestStroke(u"u1"_q, 0, other, 2), now + 1, 10)
			&& limited.addLive(TestStroke(u"u2"_q, 0, other, 2), now + 2, 10)
			&& limited.addLive(TestStroke(u"t1"_q, 0, third, 2), now + 3, 10)
			&& limited.lives().size() == 4
			&& limited.livesGeneration() == generation,
			"some previews of two members");
		check(limited.addLive(TestStroke(u"u3"_q, 0, other, 2), now + 4, 10)
			&& limited.lives().size() == 4
			&& !has(limited, u"w"_q)
			&& has(limited, u"u1"_q)
			&& has(limited, u"u3"_q)
			&& has(limited, u"t1"_q)
			&& limited.livesGeneration() != generation,
			"more previews of a member push out his own oldest one");
	}
	{
		// A zig-zag from edge to edge: few points, a lot to paint.
		const auto heavy = [](const QString &id, uint64 user, int points) {
			auto result = CanvasStroke();
			result.id = id;
			result.userId = user;
			for (auto i = 0; i != points; ++i) {
				result.points.push_back(QPoint((i % 2) ? 1920 : 0, 500));
			}
			return result;
		};
		auto flooded = empty();
		check(flooded.addLive(heavy(u"a1"_q, other, 60), now, 8000)
			&& flooded.addLive(heavy(u"a2"_q, other, 60), now + 1, 8000)
			&& flooded.liveWeight() > kLiveWeightBudget * 9 / 10
			&& flooded.liveWeight() <= kLiveWeightBudget,
			"two heavy previews fit into the budget");
		check(flooded.addLive(TestStroke(u"b1"_q, 0, third, 50), now + 2, 8000)
			&& has(flooded, u"b1"_q)
			&& flooded.liveWeight() <= kLiveWeightBudget,
			"a light preview of another member finds room");
		check(flooded.addLive(heavy(u"a3"_q, other, 60), now + 3, 8000)
			&& has(flooded, u"a3"_q)
			&& has(flooded, u"b1"_q)
			&& has(flooded, u"a2"_q)
			&& !has(flooded, u"a1"_q)
			&& flooded.liveWeight() <= kLiveWeightBudget,
			"who floods pushes out his own oldest preview, not the others");

		auto lone = empty();
		check(lone.addLive(heavy(u"x"_q, other, 200), now, 8000)
			&& lone.lives().size() == 1
			&& lone.lives()[0].stroke.points.size() < 200
			&& lone.lives()[0].weight == StrokeWeight(lone.lives()[0].stroke)
			&& lone.liveWeight() <= kLiveWeightBudget
			&& lone.liveWeight() > kLiveWeightBudget - 2000,
			"one preview alone takes what fits into the budget");
		// The next point is at the other edge of the board again.
		auto more = heavy(u"x"_q, other, 3);
		more.points.erase(begin(more.points));
		check(!lone.addLive(std::move(more), now + 1, 8000)
			&& lone.lives().size() == 1,
			"and stops growing there");
		check(lone.addLive(TestStroke(u"y"_q, 0, third, 100), now + 2, 8000)
			&& has(lone, u"y"_q)
			&& !has(lone, u"x"_q),
			"till somebody else needs the room");
	}
	{
		auto slow = empty();
		auto at = now;
		auto gone = false;
		check(slow.addLive(TestStroke(u"s"_q, 0, other, 1), at, 100'000),
			"a preview that gets a point every few seconds");
		while (!gone && at < now + kLiveMaxAge + 60'000) {
			at += 4000;
			gone = slow.expireLive(at);
			if (!gone) {
				slow.addLive(TestStroke(u"s"_q, 0, other, 1), at, 100'000);
			}
		}
		check(gone
			&& at > now + kLiveMaxAge
			&& at <= now + kLiveMaxAge + 4000,
			"nobody draws one line for ten minutes");
	}
	{
		auto weighed = empty();
		const auto before = weighed.weight();
		weighed.add(TestStroke(u"k1"_q, 1, other, 3));
		weighed.add(TestStroke(u"k2"_q, 2, other, 5));
		check(before == 0
			&& weighed.weight() == (3 + 5) * kPointWeight + 30 + 60,
			"the weight of a board is the weight of its strokes");
		weighed.remove(u"k1"_q);
		check(weighed.weight() == 5 * kPointWeight + 60,
			"a removed stroke takes its weight away");
		weighed.clear(QColor(1, 2, 3));
		check(weighed.weight() == 0, "a cleared board weighs nothing");
	}

	// A new snapshot.
	auto fresh = std::vector<CanvasStroke>();
	fresh.push_back(TestStroke(u"p3"_q, 20, self));
	model.addPending(TestStroke(u"p4"_q, 0, self));
	model.reset(QSize(1920, 1080), QColor(0, 0, 0), std::move(fresh));
	check(model.strokes().size() == 1
		&& model.pending().size() == 1
		&& model.pending()[0]->id == u"p4"_q,
		"a snapshot keeps only the own strokes it does not have");
	generation = model.generation();
	model.clear(QColor(16, 16, 16));
	check(model.strokes().empty() && model.pending().empty()
		&& model.numbers() == 0
		&& model.background() == QColor(16, 16, 16)
		&& model.generation() == generation + 1, "the board is cleared");
	check(!model.waiting(u"p4"_q),
		"a cleared board does not wait for a stroke: a late answer of the "
		"server does not bring it back");
	model.clear(QColor());
	check(model.background() == QColor(16, 16, 16),
		"a wrong colour keeps the background");
}

void TestRender(Checker &check) {
	const auto canvas = QSize(200, 100);
	const auto white = QColor(255, 255, 255);
	auto strokes = std::vector<CanvasModel::StrokePtr>();
	const auto add = [&](CanvasTool tool, QColor color, int alpha, int size,
			std::vector<QPoint> points) {
		auto stroke = CanvasStroke();
		stroke.id = u"r%1"_q.arg(strokes.size());
		stroke.seq = int64(strokes.size()) + 1;
		stroke.tool = tool;
		stroke.color = color;
		stroke.alpha = alpha;
		stroke.size = size;
		stroke.points = std::move(points);
		strokes.push_back(
			std::make_shared<const CanvasStroke>(std::move(stroke)));
	};
	add(CanvasTool::Pen, QColor(0, 0, 0), 255, 10, {
		QPoint(20, 50),
		QPoint(100, 50),
		QPoint(180, 50),
	});
	add(CanvasTool::Eraser, QColor(255, 0, 0), 255, 30, {
		QPoint(140, 20),
		QPoint(140, 80),
	});
	add(CanvasTool::Marker, QColor(255, 0, 0), 128, 20, {
		QPoint(20, 20),
		QPoint(60, 20),
	});
	add(CanvasTool::Pen, QColor(0, 0, 255), 255, 12, { QPoint(100, 85) });

	const auto one = RenderCanvas(strokes, 1, canvas, white, canvas);
	check(one.size() == canvas, "the picture has the asked size");
	check(one.pixelColor(100, 50) == QColor(0, 0, 0), "the pen paints");
	check(one.pixelColor(100, 10) == white, "the paper stays");
	check(one.pixelColor(140, 50) == QColor(0, 0, 0),
		"count limits the strokes");
	const auto all = RenderCanvas(strokes, 99, canvas, white, canvas);
	check(all.pixelColor(140, 50) == white, "the eraser brings the paper");
	check(all.pixelColor(60, 50) == QColor(0, 0, 0),
		"the eraser touches only its line");
	const auto marker = all.pixelColor(40, 20);
	check(marker.red() == 255
		&& marker.green() > 100
		&& marker.green() < 150
		&& marker.blue() == marker.green(), "the marker is translucent");
	check(all.pixelColor(100, 85) == QColor(0, 0, 255),
		"one point is a dot");
	const auto dark = RenderCanvas(
		strokes,
		99,
		canvas,
		QColor(16, 16, 16),
		canvas);
	check(dark.pixelColor(140, 50) == QColor(16, 16, 16)
		&& dark.pixelColor(100, 10) == QColor(16, 16, 16),
		"a dark board is erased to dark");
	const auto big = RenderCanvas(strokes, 99, canvas, white, canvas * 2);
	check(big.size() == canvas * 2
		&& big.pixelColor(200, 100) == QColor(0, 0, 0)
		&& big.pixelColor(200, 20) == white, "the picture is scaled");
	check(RenderCanvas(strokes, 99, canvas, white, QSize()).isNull(),
		"no size: no picture");
}

// How many pixels of two pictures of one line differ for real: where
// one has nothing the other has the line, not just another edge.
[[nodiscard]] int Mismatches(const QImage &a, const QImage &b, int full) {
	if (a.size() != b.size() || a.isNull()) {
		return -1;
	}
	const auto low = full / 3;
	auto result = 0;
	for (auto y = 0; y != a.height(); ++y) {
		for (auto x = 0; x != a.width(); ++x) {
			const auto first = qAlpha(a.pixel(x, y));
			const auto second = qAlpha(b.pixel(x, y));
			const auto differ = (!first && second > low)
				|| (!second && first > low)
				|| (first >= full - 2 && second < full - low)
				|| (second >= full - 2 && first < full - low);
			result += differ ? 1 : 0;
		}
	}
	return result;
}

// The pictures of the lines that grow: what is painted point by point
// is the line that is painted at once.
void TestGrowth(Checker &check) {
	const auto canvas = QSize(400, 300);
	const auto white = QColor(255, 255, 255);
	auto line = CanvasStroke();
	line.id = u"g"_q;
	line.color = QColor(255, 0, 0);
	line.size = 24;
	// To the right, up, back down across the first part, a hairpin.
	for (auto x = 40; x <= 360; x += 8) {
		line.points.push_back(QPoint(x, 150));
	}
	for (auto y = 141; y >= 60; y -= 9) {
		line.points.push_back(QPoint(360, y));
	}
	for (auto i = 1; i <= 30; ++i) {
		line.points.push_back(QPoint(360 - i * 8, 60 + i * 6));
	}
	line.points.push_back(QPoint(180, 240));
	line.points.push_back(QPoint(120, 250));
	const auto count = int(line.points.size());

	const auto blank = [&] {
		auto result = QImage(canvas, QImage::Format_ARGB32_Premultiplied);
		result.fill(Qt::transparent);
		return result;
	};
	const auto atOnce = [&](const CanvasStroke &stroke) {
		auto result = blank();
		auto p = QPainter(&result);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.setRenderHint(QPainter::Antialiasing);
		PaintStrokeGrowth(p, stroke, white, 0, count);
		PaintStrokeEnd(p, stroke, white, count);
		p.end();
		return result;
	};
	const auto grown = [&](const CanvasStroke &stroke, int step) {
		auto layer = StrokeLayer();
		auto growing = stroke;
		growing.points.clear();
		auto good = true;
		for (auto i = 0; i != count; ++i) {
			growing.points.push_back(stroke.points[i]);
			if ((i + 1) % step == 0 || i + 1 == count) {
				good = layer.sync(growing, white, canvas, canvas) && good;
			}
		}
		return good ? layer.image() : QImage();
	};

	// An opaque pen.
	auto usual = blank();
	{
		auto p = QPainter(&usual);
		p.setRenderHint(QPainter::Antialiasing);
		PaintStroke(p, line, white);
	}
	const auto whole = atOnce(line);
	// (114, 256) is under the round end of the line and nothing else.
	check(qAlpha(whole.pixel(100, 150)) == 255
		&& qAlpha(whole.pixel(20, 20)) == 0
		&& qAlpha(whole.pixel(114, 256)) == 255,
		"a line painted in parts is there, its end too");
	check(Mismatches(whole, usual, 255) == 0,
		"a line painted in parts is the line painted as a stroke");
	const auto byOne = grown(line, 1);
	check(Mismatches(byOne, whole, 255) == 0,
		"a line that grows point by point is the same line");
	const auto bySeven = grown(line, 7);
	check(Mismatches(bySeven, whole, 255) == 0,
		"a line that grows by several points is the same line");
	// Before the last point came the line ended in (180, 240): that end
	// was painted, and it is taken back when the line turns there.
	check(qAlpha(byOne.pixel(186, 240)) == 0
		&& qAlpha(whole.pixel(186, 240)) == 0,
		"the end of a line does not stay where the line has turned");

	// A marker: one alpha everywhere, where it crosses itself too.
	auto marker = line;
	marker.id = u"m"_q;
	marker.tool = CanvasTool::Marker;
	marker.alpha = 128;
	const auto translucent = grown(marker, 1);
	const auto plain = qAlpha(translucent.pixel(100, 150));
	const auto crossing = qAlpha(translucent.pixel(240, 150));
	check(plain >= 126 && plain <= 130,
		"a marker line that grows is translucent");
	check(crossing >= 126 && crossing <= 130,
		"and not darker where it crosses itself");
	check(Mismatches(translucent, atOnce(marker), 128) == 0,
		"a marker line that grows is the same line");

	// The eraser paints the paper.
	auto eraser = line;
	eraser.id = u"e"_q;
	eraser.tool = CanvasTool::Eraser;
	const auto erased = grown(eraser, 3);
	check(!erased.isNull()
		&& erased.pixelColor(100, 150) == white
		&& qAlpha(erased.pixel(20, 20)) == 0,
		"the eraser that grows paints the background");

	// The previews: only what was revealed, the end stays behind.
	auto lives = std::vector<CanvasModel::Live>();
	lives.push_back({ .stroke = line, .shown = 10. });
	auto painted = std::vector<int>();
	auto picture = blank();
	PaintLivesGrowth(picture, lives, painted, canvas, white);
	check(painted.size() == 1
		&& painted[0] == 10
		&& qAlpha(picture.pixel(60, 150)) == 255
		&& qAlpha(picture.pixel(200, 150)) == 0,
		"a preview is painted as far as it is revealed");
	lives[0].shown = double(count);
	PaintLivesGrowth(picture, lives, painted, canvas, white);
	check(painted[0] == count
		&& qAlpha(picture.pixel(200, 150)) == 255
		&& qAlpha(picture.pixel(114, 256)) == 0,
		"then the rest of it is added, up to the middle of the last part");
	auto all = blank();
	auto none = std::vector<int>();
	PaintLivesGrowth(all, lives, none, canvas, white);
	check(Mismatches(all, picture, 255) == 0,
		"a preview painted in two steps is the preview painted at once");
}

// What the main thread may have to paint at once at the worst, with the
// time it has taken here: a line in the log to look at, the limits are
// in weight and do not depend on the machine.
void TestCost(Checker &check, QStringList &log) {
	const auto canvas = QSize(1920, 1080);
	const auto white = QColor(255, 255, 255);
	// The worst a member can send: the widest translucent line from
	// edge to edge again and again, as many of them as are taken.
	const auto worst = [](const QString &id, uint64 user, int points) {
		auto result = CanvasStroke();
		result.id = id;
		result.userId = user;
		result.tool = CanvasTool::Marker;
		result.color = QColor(0, 122, 255);
		result.alpha = 128;
		result.size = 256;
		for (auto i = 0; i != points; ++i) {
			result.points.push_back(QPoint(
				(i % 2) ? 1920 : 0,
				(i * 37 + int(user) * 101) % 1080));
		}
		return result;
	};
	auto model = CanvasModel();
	model.reset(canvas, white, {});
	auto now = crl::time(1000);
	for (auto round = 0; round != 4; ++round) {
		for (auto user = uint64(1); user != 12; ++user) {
			for (auto i = 0; i != kMaxLivesOfUser + 1; ++i) {
				model.addLive(
					worst(u"w%1-%2-%3"_q.arg(round).arg(user).arg(i), user, 200),
					++now,
					8000);
			}
		}
	}
	while (model.advanceLive(250)) {
	}
	auto points = 0;
	auto widest = 0;
	auto perUser = base::flat_map<uint64, int>();
	for (const auto &live : model.lives()) {
		points += int(live.stroke.points.size());
		widest = std::max(widest, live.stroke.size);
		++perUser[live.stroke.userId];
	}
	auto most = 0;
	for (const auto &[user, count] : perUser) {
		most = std::max(most, count);
	}
	check(model.liveWeight() <= kLiveWeightBudget
		&& int(model.lives().size()) <= kMaxLives
		&& most <= kMaxLivesOfUser
		&& widest <= kLiveMaxSize,
		"a flood of previews stays inside the limits");

	auto picture = QImage(canvas, QImage::Format_ARGB32_Premultiplied);
	picture.fill(Qt::transparent);
	auto painted = std::vector<int>();
	auto timer = QElapsedTimer();
	timer.start();
	PaintLivesGrowth(picture, model.lives(), painted, canvas, white);
	const auto previews = timer.nsecsElapsed() / 1000;
	check(!painted.empty() && previews < 5'000'000,
		"the heaviest previews there can be are painted");

	// The heaviest finished stroke of another member that is painted
	// right in the paint event, and the heaviest board painted there.
	auto strokes = std::vector<CanvasModel::StrokePtr>();
	auto heavy = worst(u"h"_q, 1, 2);
	const auto step = int64(kPointWeight + 1920 + 1080);
	while (StrokeWeight(heavy) + step <= kSyncPaintWeight) {
		heavy.points.push_back(QPoint(
			(heavy.points.size() % 2) ? 1920 : 0,
			int(heavy.points.size() * 37) % 1080));
	}
	const auto weight = StrokeWeight(heavy);
	strokes.push_back(std::make_shared<const CanvasStroke>(std::move(heavy)));
	timer.restart();
	const auto board = RenderCanvas(strokes, 1, canvas, white, canvas);
	const auto stroke = timer.nsecsElapsed() / 1000;
	check(!board.isNull()
		&& weight <= kSyncPaintWeight
		&& weight > kSyncPaintWeight - 4000
		&& stroke < 5'000'000,
		"the heaviest stroke the main thread paints is painted");
	log.push_back(
		u"worst case at 1920x1080: %1 previews, %2 points, weight %3"_q.arg(
			QString::number(model.lives().size()),
			QString::number(points),
			QString::number(model.liveWeight()))
		+ u" painted in %1 ms; a stroke of weight %2 painted in %3 ms"_q.arg(
			QString::number(previews / 1000., 'f', 1),
			QString::number(weight),
			QString::number(stroke / 1000., 'f', 1)));
}

// ---- Snapshot scenes (OBLIVION_SELFTEST=ui).

constexpr auto kSampleNow = int64(1791327935000);
constexpr auto kSampleSelf = uint64(9000000000000101ULL);
constexpr auto kSampleOwner = uint64(9000000000000100ULL);
constexpr auto kSampleThird = uint64(9000000000000102ULL);

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

class SampleHost final : public Ui::RpWidget {
public:
	SampleHost(
		QWidget *parent,
		Room::Descriptor &&descriptor,
		const QString &tab)
	: RpWidget(parent)
	, _room(std::make_unique<Room>(std::move(descriptor))) {
		_content = base::unique_qptr<Ui::RpWidget>(CreateRoomWidget(
			this,
			_room.get(),
			SelfTest::SceneShow(this),
			tab).release());
		_content->show();
		sizeValue(
		) | rpl::on_next([=](QSize size) {
			if (_content) {
				_content->setGeometry(QRect(QPoint(), size));
			}
		}, lifetime());
	}
	~SampleHost() {
		_content = nullptr;
	}

private:
	const std::unique_ptr<Room> _room;
	base::unique_qptr<Ui::RpWidget> _content;

};

struct SampleBoard {
	std::vector<CanvasStroke> strokes;
	std::vector<CanvasStroke> lives;
};

// A sun, a boat on the waves and a heart from three people, one more
// line is still being drawn.
[[nodiscard]] SampleBoard MakeSampleBoard(bool dark) {
	auto result = SampleBoard();
	const auto ink = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
	const auto add = [&](
			uint64 user,
			CanvasTool tool,
			QColor color,
			int size,
			std::vector<QPoint> points) {
		auto stroke = CanvasStroke();
		stroke.id = u"s%1"_q.arg(result.strokes.size() + 1);
		stroke.seq = int64(result.strokes.size()) + 1;
		stroke.userId = user;
		stroke.tool = tool;
		stroke.color = color;
		stroke.alpha = (tool == CanvasTool::Marker) ? kMarkerAlpha : 255;
		stroke.size = size;
		stroke.points = std::move(points);
		result.strokes.push_back(std::move(stroke));
	};
	const auto arc = [](
			QPointF center,
			double rx,
			double ry,
			double from,
			double till,
			int count) {
		auto points = std::vector<QPoint>();
		for (auto i = 0; i <= count; ++i) {
			const auto angle = from + (till - from) * i / count;
			points.push_back(QPoint(
				int(std::lround(center.x() + rx * std::cos(angle))),
				int(std::lround(center.y() + ry * std::sin(angle)))));
		}
		return points;
	};
	const auto wave = [](int y, int amplitude, double shift) {
		auto points = std::vector<QPoint>();
		for (auto x = 90; x <= 1830; x += 30) {
			points.push_back(QPoint(
				x,
				y + int(std::lround(amplitude * std::sin(x / 95. + shift)))));
		}
		return points;
	};
	const auto orange = SwatchColor(3);
	const auto yellow = SwatchColor(4);
	const auto blue = SwatchColor(7);
	const auto red = SwatchColor(2);
	const auto green = SwatchColor(5);

	// The sea: two wide marker waves.
	add(kSampleOwner, CanvasTool::Marker, blue, 96, wave(880, 26, 0.));
	add(kSampleOwner, CanvasTool::Marker, SwatchColor(6), 72, wave(970, 22, 1.6));

	// The sun with its rays.
	const auto sun = QPointF(330, 270);
	add(kSampleSelf, CanvasTool::Marker, yellow, 150, arc(sun, 40, 40, 0., 6.3, 24));
	add(kSampleSelf, CanvasTool::Pen, orange, 12, arc(sun, 118, 118, 0., 6.3, 48));
	for (auto i = 0; i != 10; ++i) {
		const auto angle = i * 2. * M_PI / 10.;
		add(kSampleSelf, CanvasTool::Pen, orange, 12, {
			QPoint(
				int(std::lround(sun.x() + 160 * std::cos(angle))),
				int(std::lround(sun.y() + 160 * std::sin(angle)))),
			QPoint(
				int(std::lround(sun.x() + 215 * std::cos(angle))),
				int(std::lround(sun.y() + 215 * std::sin(angle)))),
		});
	}

	// The boat.
	add(kSampleOwner, CanvasTool::Pen, ink, 10, {
		QPoint(760, 730),
		QPoint(850, 840),
		QPoint(1130, 840),
		QPoint(1230, 730),
		QPoint(760, 730),
	});
	add(kSampleOwner, CanvasTool::Pen, ink, 10, {
		QPoint(990, 730),
		QPoint(990, 400),
	});
	add(kSampleOwner, CanvasTool::Pen, red, 10, {
		QPoint(1010, 420),
		QPoint(1190, 680),
		QPoint(1010, 680),
		QPoint(1010, 420),
	});
	add(kSampleOwner, CanvasTool::Pen, green, 10, {
		QPoint(970, 440),
		QPoint(840, 680),
		QPoint(970, 680),
	});

	// The heart of the third one.
	auto heart = std::vector<QPoint>();
	for (auto i = 0; i <= 60; ++i) {
		const auto t = i * 2. * M_PI / 60.;
		const auto x = 16. * std::pow(std::sin(t), 3.);
		const auto y = 13. * std::cos(t)
			- 5. * std::cos(2. * t)
			- 2. * std::cos(3. * t)
			- std::cos(4. * t);
		heart.push_back(QPoint(
			int(std::lround(1500 + 11. * x)),
			int(std::lround(300 - 11. * y))));
	}
	add(kSampleThird, CanvasTool::Pen, SwatchColor(9), 14, std::move(heart));

	// Two gulls. (No eraser line here: a bare cut through the waves looks
	// like a glitch in a still picture, the self-test checks the eraser.)
	add(kSampleThird, CanvasTool::Pen, ink, 8, {
		QPoint(1090, 250),
		QPoint(1125, 215),
		QPoint(1160, 250),
		QPoint(1195, 215),
		QPoint(1230, 250),
	});
	add(kSampleThird, CanvasTool::Pen, ink, 8, {
		QPoint(640, 170),
		QPoint(668, 142),
		QPoint(696, 170),
		QPoint(724, 142),
		QPoint(752, 170),
	});

	// A star is being drawn right now.
	auto star = CanvasStroke();
	star.id = u"live1"_q;
	star.userId = kSampleThird;
	star.color = SwatchColor(8);
	star.size = 12;
	const auto center = QPointF(1580, 640);
	for (auto i = 0; i != 4; ++i) {
		const auto angle = -M_PI / 2. + i * 4. * M_PI / 5.;
		star.points.push_back(QPoint(
			int(std::lround(center.x() + 120 * std::cos(angle))),
			int(std::lround(center.y() + 120 * std::sin(angle)))));
	}
	result.lives.push_back(std::move(star));
	auto scribble = CanvasStroke();
	scribble.id = u"live2"_q;
	scribble.userId = kSampleOwner;
	scribble.color = ink;
	scribble.size = 8;
	// An arc that ends at its right side: the name of who draws it hangs
	// next to the end and does not cover the line.
	scribble.points = arc(QPointF(560, 560), 70, 36, 2.6, 6.6, 20);
	result.lives.push_back(std::move(scribble));
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto scene = [](
			const QString &name,
			QSize size,
			bool owner,
			Fn<void(Room::Descriptor&)> adjust,
			Fn<void(not_null<CanvasTab*>)> setup) {
		RegisterScene(name, size, [=](not_null<Ui::RpWidget*> parent) {
			auto descriptor = SampleRoomDescriptor(owner);
			if (adjust) {
				adjust(descriptor);
			}
			SceneSetup = setup;
			const auto result = CreateSampleRoomScene(
				parent,
				std::move(descriptor),
				u"canvas"_q);
			SceneSetup = nullptr;
			return result;
		});
	};
	const auto filled = [](bool dark) {
		return [=](not_null<CanvasTab*> tab) {
			auto board = MakeSampleBoard(dark);
			tab->setSample(
				std::move(board.strokes),
				std::move(board.lives),
				dark
					? QColor(QString::fromLatin1(kDarkBackground))
					: QColor(255, 255, 255));
		};
	};
	const auto size = QSize(Scaled(460), Scaled(720));
	scene(u"room_canvas"_q, size, true, nullptr, filled(false));
	scene(u"room_canvas_dark"_q, size, true, nullptr, [=](
			not_null<CanvasTab*> tab) {
		filled(true)(tab);
		tab->chooseTool(CanvasTool::Marker, 4, 2);
	});
	scene(
		u"room_canvas_wide"_q,
		QSize(Scaled(900), Scaled(680)),
		true,
		nullptr,
		[=](not_null<CanvasTab*> tab) {
			filled(false)(tab);
			tab->chooseTool(CanvasTool::Eraser, 0, 1);
		});
	scene(
		u"room_canvas_narrow"_q,
		QSize(Scaled(380), Scaled(520)),
		true,
		nullptr,
		filled(false));
	scene(u"room_canvas_guest"_q, size, false, [](
			Room::Descriptor &descriptor) {
		descriptor.state.rights.draw = false;
	}, filled(false));
	scene(u"room_canvas_empty"_q, size, false, nullptr, nullptr);
	scene(u"room_canvas_offline"_q, size, true, [](
			Room::Descriptor &descriptor) {
		descriptor.connected = false;
	}, filled(false));
	scene(u"room_canvas_failed"_q, size, true, nullptr, [](
			not_null<CanvasTab*> tab) {
		tab->setSampleFailed();
	});
});

} // namespace

bool RunCanvasSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestCodec(check);
	check.section("stroke codec");
	TestSimplify(check);
	check.section("simplify and path");
	TestModel(check);
	check.section("model and limits");
	TestRender(check);
	check.section("render");
	TestGrowth(check);
	check.section("lines that grow");
	TestCost(check, log);
	check.section("the cost of what the others send");
	log.push_back(u"room_canvas: %1 checks, %2 failed"_q.arg(
		QString::number(check.passed() + check.failed()),
		QString::number(check.failed())));
	return !check.failed();
}

Room::Descriptor SampleRoomDescriptor(bool owner) {
	auto state = RoomState();
	state.code = u"K7QM2XPA9Z"_q;
	state.link = Cloud::MakeLink(Cloud::LinkKind::Room, state.code);
	state.title = SampleText("Ночной эфир", "Night air");
	state.rev = 7;
	state.settings.defaults = Rights::Everything();
	state.ownerId = owner ? kSampleSelf : kSampleOwner;
	state.owner = owner;
	auto limited = Rights::OnlyOwner();
	limited.add = true;
	state.rights = owner ? Rights::Everything() : limited;
	const auto member = [&](
			uint64 id,
			QString name,
			bool isOwner,
			bool online) {
		auto result = Member();
		result.id = id;
		result.name = std::move(name);
		result.owner = isOwner;
		result.rights = isOwner ? Rights::Everything() : limited;
		result.joinedAt = kSampleNow - 3'600'000;
		result.online = online;
		state.members.push_back(std::move(result));
	};
	member(kSampleSelf, SampleText("Аня", "Anna"), owner, true);
	member(kSampleOwner, SampleText("Миша", "Michael"), !owner, true);
	member(kSampleThird, SampleText("Лера Соколова", "Valerie Falcon"), false, true);
	member(
		9000000000000103ULL,
		SampleText("Костя", "Constantine"),
		false,
		false);
	const auto item = [&](
			const QString &id,
			char sha,
			const QString &title,
			const QString &performer,
			int64 duration,
			uint64 addedBy) {
		auto result = QueueItem();
		result.id = id;
		result.media = QString(64, QChar(sha));
		result.size = 8'000'000;
		result.mime = u"audio/mpeg"_q;
		result.title = title;
		result.performer = performer;
		result.duration = duration;
		result.fileName = title + u".mp3"_q;
		result.addedBy = addedBy;
		result.addedAt = kSampleNow - 600'000;
		state.music.queue.push_back(std::move(result));
	};
	item(u"a1"_q, '3', u"Midnight City"_q, u"M83"_q, 243'000, kSampleOwner);
	item(
		u"b2"_q,
		'7',
		SampleText("Звезда по имени Солнце", "A Star Called Sun"),
		SampleText("Кино", "Kino"),
		225'000,
		kSampleSelf);
	item(u"c3"_q, 'b', u"Instant Crush"_q, u"Daft Punk"_q, 337'000, kSampleThird);
	state.music.state.itemId = u"a1"_q;
	state.music.state.playing = true;
	state.music.state.position = 61'000;
	state.music.state.anchor = kSampleNow - 20'000;
	state.music.state.repeat = Repeat::All;
	state.music.state.rev = 12;
	state.music.state.updatedBy = kSampleOwner;

	auto result = Room::Descriptor();
	result.selfId = kSampleSelf;
	result.state = std::move(state);
	result.now = [] { return kSampleNow; };
	return result;
}

QWidget *CreateSampleRoomScene(
		not_null<Ui::RpWidget*> parent,
		Room::Descriptor &&descriptor,
		const QString &tab) {
	return Ui::CreateChild<SampleHost>(
		parent.get(),
		std::move(descriptor),
		tab);
}

} // namespace Oblivion::Rooms
