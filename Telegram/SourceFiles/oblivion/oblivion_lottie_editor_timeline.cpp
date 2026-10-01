/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie_editor_timeline.h"

#include "lang/lang_keys.h"
#include "oblivion/oblivion_lottie_doc.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "ui/effects/animation_value.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <set>

namespace Oblivion::LottieEdit {
namespace {

constexpr auto kTransportHeight = 44;
constexpr auto kRulerHeight = 28;
constexpr auto kLayerRowHeight = 28;
constexpr auto kPropertyRowHeight = 24;
constexpr auto kNamesWidth = 230;
constexpr auto kNamesMinWidth = 150;
constexpr auto kPadding = 10;
constexpr auto kTrackInset = 10;
constexpr auto kTrackPadding = 16;
constexpr auto kIndent = 30;
constexpr auto kDisclosureWidth = 22;
constexpr auto kDiamond = 5; // Half size of a keyframe mark.
constexpr auto kEdgeGrab = 5;
constexpr auto kDragThreshold = 3;
constexpr auto kLabelSpacing = 52;
constexpr auto kMaxTimeZoom = 64.;
constexpr auto kMinVisibleFrames = 4.;
constexpr auto kTimeZoomStep = 1.5;
constexpr auto kScrollbarWidth = 6;
constexpr auto kButtonSize = 32;
constexpr auto kButtonSkip = 2;
constexpr auto kGroupSkip = 10;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

// Keyframe times are stored with 3 decimals (like the model does).
[[nodiscard]] double RoundTime(double time) {
	return std::round(time * 1000.) / 1000.;
}

[[nodiscard]] style::font SmallFont() {
	return style::font(Scaled(11), 0, st::normalFont->family());
}

[[nodiscard]] style::font SmallBoldFont() {
	return style::font(
		Scaled(11),
		st::semiboldFont->flags(),
		st::semiboldFont->family());
}

[[nodiscard]] const style::color &LayerColor(int index) {
	switch (index % 8) {
	case 0: return st::historyPeer1NameFg;
	case 1: return st::historyPeer2NameFg;
	case 2: return st::historyPeer3NameFg;
	case 3: return st::historyPeer4NameFg;
	case 4: return st::historyPeer5NameFg;
	case 5: return st::historyPeer6NameFg;
	case 6: return st::historyPeer7NameFg;
	case 7: return st::historyPeer8NameFg;
	}
	return st::historyPeer1NameFg;
}

[[nodiscard]] bool IsTransformRole(PropertyRole role) {
	switch (role) {
	case PropertyRole::Anchor:
	case PropertyRole::Position:
	case PropertyRole::PositionX:
	case PropertyRole::PositionY:
	case PropertyRole::PositionZ:
	case PropertyRole::Scale:
	case PropertyRole::Rotation:
	case PropertyRole::RotationX:
	case PropertyRole::RotationY:
	case PropertyRole::Opacity:
	case PropertyRole::Skew:
	case PropertyRole::SkewAxis:
		return true;
	default:
		return false;
	}
}

// Shown even when static, so they can be animated from the timeline.
[[nodiscard]] bool IsBasicRole(PropertyRole role) {
	switch (role) {
	case PropertyRole::Position:
	case PropertyRole::PositionX:
	case PropertyRole::PositionY:
	case PropertyRole::Scale:
	case PropertyRole::Rotation:
	case PropertyRole::Opacity:
		return true;
	default:
		return false;
	}
}

// The root composition layer that shows a node (through precomps).
[[nodiscard]] NodeId RootLayerOf(const Document &document, NodeId id) {
	auto layer = document.owningLayer(id);
	for (auto guard = 0; layer && guard != 32; ++guard) {
		const auto node = document.node(layer);
		if (!node) {
			return 0;
		} else if (node->composition == document.rootId()) {
			return layer;
		}
		const auto users = document.assetUsers(node->composition);
		layer = users.empty() ? NodeId() : users.front();
	}
	return 0;
}

// Group transforms are shown as properties of their group.
[[nodiscard]] NodeId DisplayOwner(const Document &document, NodeId id) {
	const auto node = document.node(id);
	return (node
		&& node->kind == NodeKind::Shape
		&& node->shapeType == ShapeType::Transform)
		? node->parent
		: id;
}

// The steps keep growing (1200, 3000, 6000, 12000...) until the labels
// are spacing apart, so the ruler never paints more ticks than fit.
[[nodiscard]] int PickStep(double pixelsPerFrame, int spacing) {
	for (const auto step : { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 }) {
		if (step * pixelsPerFrame >= spacing) {
			return step;
		}
	}
	for (auto scale = 1; scale != 1'000'000; scale *= 10) {
		for (const auto step : { 1200, 3000, 6000 }) {
			if (step * scale * pixelsPerFrame >= spacing) {
				return step * scale;
			}
		}
	}
	return 6000 * 100'000;
}

[[nodiscard]] int PickMinorStep(double pixelsPerFrame, int major) {
	for (const auto step : { 1, 2, 5, 10, 30, 60, 300 }) {
		if (step * pixelsPerFrame >= Scaled(7) && !(major % step)) {
			return step;
		}
	}
	return major;
}

// Same for seconds (120, 300, 600, 1200...). Zero when nothing fits (a
// broken file with a frame rate like 1e-9), the seconds are skipped then.
[[nodiscard]] double PickSecondsStep(double pixelsPerSecond, int spacing) {
	for (const auto step : { 0.5, 1., 2., 5., 10., 30., 60. }) {
		if (step * pixelsPerSecond >= spacing) {
			return step;
		}
	}
	for (auto scale = 1.; scale <= 1e12; scale *= 10.) {
		for (const auto step : { 120., 300., 600. }) {
			if (step * scale * pixelsPerSecond >= spacing) {
				return step * scale;
			}
		}
	}
	return 0.;
}

// Keyframe mark: diamond for linear, circle for eased, square for hold.
void PaintKeyframe(
		QPainter &p,
		QPointF center,
		double half,
		EasingPreset preset,
		const QColor &fill,
		const QColor &border) {
	auto pen = QPen(border);
	pen.setWidthF(std::max(Scaled(1) * 1., 1.));
	p.setPen(pen);
	p.setBrush(fill);
	switch (preset) {
	case EasingPreset::Hold:
		p.drawRoundedRect(
			QRectF(center.x() - half * 0.85, center.y() - half * 0.85,
				half * 1.7, half * 1.7),
			half * 0.25,
			half * 0.25);
		break;
	case EasingPreset::EaseIn:
	case EasingPreset::EaseOut:
	case EasingPreset::EaseInOut:
	case EasingPreset::Custom:
		p.drawEllipse(center, half * 0.95, half * 0.95);
		break;
	case EasingPreset::Linear:
		p.drawPolygon(QPolygonF({
			center + QPointF(0., -half * 1.15),
			center + QPointF(half * 1.15, 0.),
			center + QPointF(0., half * 1.15),
			center + QPointF(-half * 1.15, 0.),
		}));
		break;
	}
}

struct CopiedKeyframe {
	PropertyRef property;
	PropertyType type = PropertyType::Scalar;
	int dimensions = 1;
	double offset = 0.; // Frames after the first copied keyframe.
	PropValue value;
	Easing easing;
};

[[nodiscard]] std::vector<CopiedKeyframe> &Clipboard() {
	static auto result = std::vector<CopiedKeyframe>();
	return result;
}

} // namespace

struct TimelinePanel::Drag {
	enum class Type : uchar {
		None,
		Scrub,
		Keyframes,
		LayerShift,
		LayerStart,
		LayerEnd,
		Marquee,
		Scrollbar,
	};
	Type type = Type::None;
	QPoint press;
	double pressFrame = 0.;
	bool started = false;
	bool applied = false;
	QByteArray mergeKey;
	Document start;

	// Keyframes.
	PropertyRef property; // The pressed row.
	double scale = 1.; // Root frames per local frame of the pressed row.
	std::vector<KeyframeRef> refs; // Moved, times at the press.
	std::vector<KeyframeRef> untouched; // Selected, not moved.
	std::optional<KeyframeRef> selectOnClick;
	int minDelta = INT_MIN;
	int maxDelta = INT_MAX;
	int delta = 0;

	// Layers.
	NodeId layer = 0;
	std::vector<NodeId> layers;
	double inPoint = 0.;
	double outPoint = 0.;

	// Marquee.
	std::vector<KeyframeRef> base;
	QRect rect;

	// Scrollbar.
	int startScroll = 0;
};

TimelinePanel::TimelinePanel(
	QWidget *parent,
	not_null<EditorController*> controller)
: RpWidget(parent)
, _controller(controller) {
	setMouseTracking(true);
	setupControls();

	_controller->documentChanged(
	) | rpl::on_next([=](const DocumentChange &change) {
		if (change.source == ChangeSource::Load) {
			_drag = nullptr;
			_expandedOverride.clear();
			_scrollTop = 0;
			_timeZoom = 1.;
			_viewStart = _controller->firstFrame();
			_zoomOut->setDisabled(true);
			_zoomIn->setDisabled(false);
		}
		// The composition range may have changed (trim, duration...).
		setViewStart(_viewStart);
		invalidateRows();
	}, lifetime());

	_controller->selectionChanged(
	) | rpl::on_next([=] {
		invalidateRows();
		revealSelection();
	}, lifetime());

	rpl::merge(
		_controller->keyframeSelectionChanged(),
		_controller->activePropertyChanged()
	) | rpl::on_next([=] {
		update();
	}, lifetime());

	_controller->currentFrameValue(
	) | rpl::on_next([=] {
		keepPlayheadVisible();
		update();
	}, lifetime());

	_controller->playingValue(
	) | rpl::on_next([=] {
		refreshPlayButton();
	}, lifetime());

	_viewStart = _controller->firstFrame();
}

TimelinePanel::~TimelinePanel() = default;

void TimelinePanel::setupControls() {
	const auto size = Scaled(kButtonSize);
	const auto withShortcut = [](
			rpl::producer<QString> text,
			QKeySequence keys) {
		return std::move(text) | rpl::map([=](const QString &text) {
			return WithShortcut(text, keys);
		});
	};
	_first = Ui::CreateChild<GlyphButton>(
		this,
		Glyph::First,
		withShortcut(
			tr::lng_oblivion_lottie_timeline_first(),
			QKeySequence(Qt::Key_Home)),
		size);
	_previous = Ui::CreateChild<GlyphButton>(
		this,
		Glyph::Previous,
		withShortcut(
			tr::lng_oblivion_lottie_timeline_previous(),
			QKeySequence(Qt::Key_Left)),
		size);
	_play = Ui::CreateChild<GlyphButton>(
		this,
		Glyph::Play,
		rpl::single(QString()),
		size);
	_next = Ui::CreateChild<GlyphButton>(
		this,
		Glyph::Next,
		withShortcut(
			tr::lng_oblivion_lottie_timeline_next(),
			QKeySequence(Qt::Key_Right)),
		size);
	_last = Ui::CreateChild<GlyphButton>(
		this,
		Glyph::Last,
		withShortcut(
			tr::lng_oblivion_lottie_timeline_last(),
			QKeySequence(Qt::Key_End)),
		size);
	_loop = Ui::CreateChild<GlyphButton>(
		this,
		Glyph::Loop,
		tr::lng_oblivion_lottie_timeline_loop(),
		size);
	_zoomOut = Ui::CreateChild<GlyphButton>(
		this,
		Glyph::ZoomOut,
		tr::lng_oblivion_lottie_timeline_zoom_out(),
		size);
	_zoomIn = Ui::CreateChild<GlyphButton>(
		this,
		Glyph::ZoomIn,
		tr::lng_oblivion_lottie_timeline_zoom_in(),
		size);

	_first->setClickedCallback([=] {
		_controller->setPlaying(false);
		_controller->setCurrentFrame(_controller->firstFrame());
	});
	_previous->setClickedCallback([=] {
		_controller->setPlaying(false);
		_controller->stepFrame(-1);
	});
	_play->setClickedCallback([=] {
		_controller->togglePlaying();
	});
	_next->setClickedCallback([=] {
		_controller->setPlaying(false);
		_controller->stepFrame(1);
	});
	_last->setClickedCallback([=] {
		_controller->setPlaying(false);
		_controller->setCurrentFrame(_controller->lastFrame());
	});
	_loop->setActive(_controller->looping());
	_loop->setClickedCallback([=] {
		_controller->setLooping(!_controller->looping());
		_loop->setActive(_controller->looping());
	});
	_zoomOut->setClickedCallback([=] { zoomTime(1. / kTimeZoomStep); });
	_zoomIn->setClickedCallback([=] { zoomTime(kTimeZoomStep); });
	_zoomOut->setDisabled(true);
	refreshPlayButton();
}

void TimelinePanel::refreshPlayButton() {
	const auto playing = _controller->playing();
	_play->setGlyph(playing ? Glyph::Pause : Glyph::Play);
	_play->setActive(playing);
	_play->setTooltip(WithShortcut(
		(playing
			? tr::lng_oblivion_lottie_editor_pause(tr::now)
			: tr::lng_oblivion_lottie_editor_play(tr::now)),
		QKeySequence(Qt::Key_Space)));
}

void TimelinePanel::updateControlsGeometry() {
	const auto padding = Scaled(kPadding);
	const auto size = Scaled(kButtonSize);
	const auto skip = Scaled(kButtonSkip);
	const auto top = (Scaled(kTransportHeight) - size) / 2;
	auto left = padding - Scaled(4);
	for (const auto button : { _first, _previous, _play, _next, _last }) {
		button->moveToLeft(left, top, width());
		left += size + skip;
	}
	left += Scaled(kGroupSkip) - skip;
	_loop->moveToLeft(left, top, width());

	auto right = width() - padding + Scaled(4);
	right -= size;
	_zoomIn->moveToLeft(right, top, width());
	right -= size + skip;
	_zoomOut->moveToLeft(right, top, width());
}

void TimelinePanel::resizeEvent(QResizeEvent *e) {
	updateControlsGeometry();
	setViewStart(_viewStart);
	clampScroll();
}

// Rows.

void TimelinePanel::invalidateRows() {
	_rowsDirty = true;
	update();
}

void TimelinePanel::ensureRows() {
	if (_rowsDirty) {
		rebuildRows();
	}
}

void TimelinePanel::rebuildRows() {
	_rowsDirty = false;
	_rows.clear();
	const auto &document = _controller->document();
	auto autoExpanded = std::set<NodeId>();
	for (const auto id : _controller->selection()) {
		if (const auto root = RootLayerOf(document, id)) {
			autoExpanded.emplace(root);
		}
	}
	auto top = 0;
	auto index = 0;
	for (const auto layer : document.layers()) {
		const auto node = document.node(layer);
		if (!node) {
			continue;
		}
		auto row = Row();
		row.type = RowType::Layer;
		row.layer = layer;
		row.label = NodeDisplayName(document, layer);
		row.colorIndex = index++;
		row.top = top;
		row.height = Scaled(kLayerRowHeight);
		row.hidden = node->hidden;
		row.selected = _controller->isSelected(layer);
		row.inPoint = node->inPoint;
		row.outPoint = node->outPoint;
		const auto i = _expandedOverride.find(layer);
		row.expanded = (i != end(_expandedOverride))
			? i->second
			: autoExpanded.contains(layer);
		if (!row.expanded) {
			for (const auto &ref : document.animatedProperties(layer)) {
				for (const auto time : document.keyframeTimes(ref)) {
					row.summary.push_back(time);
				}
			}
			ranges::sort(row.summary);
			row.summary.erase(
				std::unique(
					begin(row.summary),
					end(row.summary),
					[](double a, double b) { return std::abs(a - b) < 0.01; }),
				end(row.summary));
		}
		top += row.height;
		const auto expanded = row.expanded;
		_rows.push_back(std::move(row));
		if (expanded) {
			addPropertyRows(layer, top);
		}
	}
	_contentHeight = top;
	clampScroll();
}

void TimelinePanel::addPropertyRows(NodeId layer, int &top) {
	const auto &document = _controller->document();
	const auto first = double(_controller->firstFrame());
	const auto count = _rows.size();
	auto added = std::vector<PropertyRef>();
	const auto add = [&](const PropertyInfo &info) {
		if (ranges::contains(added, info.ref)) {
			return;
		}
		added.push_back(info.ref);
		auto row = Row();
		row.type = RowType::Property;
		row.layer = layer;
		row.property = info.ref;
		row.label = PropertyText(info);
		const auto owner = DisplayOwner(document, info.ref.node);
		if (owner && owner != layer) {
			row.owner = NodeDisplayName(document, owner);
		}
		row.top = top;
		row.height = Scaled(kPropertyRowHeight);
		row.keyframes = document.keyframes(info.ref);

		// Local <-> root time, linear through "st" / "sr" of precomps.
		const auto from = document.localFrame(info.ref.node, first);
		const auto till = document.localFrame(info.ref.node, first + 100.);
		if (std::abs(till - from) > 1e-6) {
			row.scale = 100. / (till - from);
			row.offset = first - from * row.scale;
		} else {
			row.editable = false;
		}
		top += row.height;
		_rows.push_back(std::move(row));
	};
	const auto addTransform = [&](NodeId id) {
		for (const auto &info : document.properties(id)) {
			if (IsTransformRole(info.role)
				&& (info.animated || IsBasicRole(info.role))) {
				add(info);
			}
		}
	};
	const auto addAnimated = [&](NodeId id) {
		for (const auto &ref : document.animatedProperties(id)) {
			if (const auto info = document.property(ref)) {
				add(*info);
			}
		}
	};
	addTransform(layer);
	for (const auto id : _controller->selection()) {
		if (id == layer || RootLayerOf(document, id) != layer) {
			continue;
		}
		const auto node = document.node(id);
		if (!node) {
			continue;
		} else if (node->kind == NodeKind::Layer) {
			addTransform(id);
			addAnimated(id);
		} else {
			for (const auto &info : document.properties(id)) {
				add(info);
			}
		}
	}
	addAnimated(layer);
	if (_rows.size() == count) {
		auto row = Row();
		row.type = RowType::Empty;
		row.layer = layer;
		row.label = tr::lng_oblivion_lottie_timeline_no_properties(tr::now);
		row.top = top;
		row.height = Scaled(kPropertyRowHeight);
		top += row.height;
		_rows.push_back(std::move(row));
	}
}

void TimelinePanel::clampScroll() {
	const auto available = std::max(height() - rowsTop(), 0);
	_scrollTop = std::clamp(
		_scrollTop,
		0,
		std::max(_contentHeight - available, 0));
}

void TimelinePanel::revealSelection() {
	const auto primary = _controller->primarySelection();
	if (!primary) {
		return;
	}
	ensureRows();
	const auto layer = RootLayerOf(_controller->document(), primary);
	const auto i = ranges::find_if(_rows, [&](const Row &row) {
		return (row.type == RowType::Layer) && (row.layer == layer);
	});
	if (i == end(_rows)) {
		return;
	}
	const auto available = std::max(height() - rowsTop(), 0);
	if (i->top < _scrollTop) {
		_scrollTop = i->top;
	} else if (i->top + i->height > _scrollTop + available) {
		_scrollTop = i->top + i->height - available;
	}
	clampScroll();
	update();
}

// Geometry.

void TimelinePanel::setNamesWidth(int width) {
	if (_namesWidth != width) {
		_namesWidth = width;
		setViewStart(_viewStart);
	}
}

int TimelinePanel::namesWidth() const {
	return std::clamp(
		_namesWidth ? _namesWidth : Scaled(kNamesWidth),
		Scaled(kNamesMinWidth),
		std::max(Scaled(kNamesMinWidth), width() / 3 + st::lineWidth));
}

int TimelinePanel::rowsTop() const {
	return Scaled(kTransportHeight) + Scaled(kRulerHeight);
}

int TimelinePanel::trackLeft() const {
	return namesWidth() + Scaled(kTrackInset);
}

int TimelinePanel::trackRight() const {
	return std::max(width() - Scaled(kTrackPadding), trackLeft() + 1);
}

double TimelinePanel::viewSpan() const {
	const auto count = std::max(_controller->frameCount(), 1);
	return std::max(count / _timeZoom, std::min(kMinVisibleFrames, 1. * count));
}

std::pair<double, double> TimelinePanel::compositionX() const {
	const auto first = double(_controller->firstFrame());
	const auto count = double(std::max(_controller->frameCount(), 1));
	return { xFromFrame(first), xFromFrame(first + count) };
}

bool TimelinePanel::inComposition(double rootFrame) const {
	const auto first = double(_controller->firstFrame());
	const auto count = double(std::max(_controller->frameCount(), 1));
	return (rootFrame >= first - 0.5) && (rootFrame <= first + count + 0.5);
}

QRect TimelinePanel::trackClip(int y, int height, double extend) const {
	const auto [from, till] = compositionX();
	const auto left = std::max(int(std::floor(from - extend)), namesWidth());
	const auto right = std::min(int(std::ceil(till + extend)), width());
	return QRect(left, y, std::max(right - left, 0), height);
}

double TimelinePanel::xFromFrame(double frame) const {
	const auto width = trackRight() - trackLeft();
	return trackLeft() + (frame - _viewStart) * width / viewSpan();
}

double TimelinePanel::frameFromX(double x) const {
	const auto width = std::max(trackRight() - trackLeft(), 1);
	return _viewStart + (x - trackLeft()) * viewSpan() / width;
}

int TimelinePanel::frameAtX(double x) const {
	return std::clamp(
		int(std::floor(frameFromX(x) + 0.5)),
		_controller->firstFrame(),
		_controller->lastFrame());
}

double TimelinePanel::keyframeX(const Row &row, int index) const {
	const auto time = row.keyframes[index].time;
	return xFromFrame(time * row.scale + row.offset);
}

QRect TimelinePanel::scrollbarRect() const {
	const auto available = height() - rowsTop();
	if (_contentHeight <= available || available <= 0) {
		return QRect();
	}
	const auto size = std::max(
		available * available / _contentHeight,
		Scaled(24));
	const auto range = _contentHeight - available;
	const auto top = rowsTop()
		+ (available - size) * _scrollTop / std::max(range, 1);
	return QRect(
		width() - Scaled(kScrollbarWidth) - Scaled(3),
		top,
		Scaled(kScrollbarWidth),
		size);
}

void TimelinePanel::setViewStart(double start) {
	const auto first = double(_controller->firstFrame());
	const auto count = double(std::max(_controller->frameCount(), 1));
	_viewStart = std::clamp(
		start,
		first,
		std::max(first, first + count - viewSpan()));
	update();
}

void TimelinePanel::zoomTime(double factor, std::optional<double> anchorX) {
	const auto count = double(std::max(_controller->frameCount(), 1));
	const auto maxZoom = std::max(
		1.,
		std::min(kMaxTimeZoom, count / kMinVisibleFrames));
	const auto zoom = std::clamp(_timeZoom * factor, 1., maxZoom);
	const auto x = anchorX.value_or(
		std::clamp(
			xFromFrame(_controller->currentFrame()),
			double(trackLeft()),
			double(trackRight())));
	const auto frame = frameFromX(x);
	_timeZoom = zoom;
	const auto width = std::max(trackRight() - trackLeft(), 1);
	setViewStart(frame - (x - trackLeft()) * viewSpan() / width);
	_zoomOut->setDisabled(_timeZoom <= 1.);
	_zoomIn->setDisabled(_timeZoom >= maxZoom);
	update();
}

void TimelinePanel::keepPlayheadVisible() {
	if (_timeZoom <= 1.) {
		return;
	}
	const auto frame = double(_controller->currentFrame());
	const auto x = xFromFrame(frame);
	if (x < trackLeft() || x > trackRight()) {
		setViewStart(frame - viewSpan() * 0.05);
	}
}

// Hit testing.

auto TimelinePanel::hitTest(QPoint point) -> Hit {
	ensureRows();
	if (point.y() < Scaled(kTransportHeight)) {
		return {};
	} else if (point.y() < rowsTop()) {
		return (point.x() >= namesWidth())
			? Hit{ .type = HitType::Ruler }
			: Hit();
	}
	const auto scrollbar = scrollbarRect();
	if (!scrollbar.isEmpty()
		&& point.x() >= scrollbar.x() - Scaled(3)
		&& point.y() >= rowsTop()) {
		return { .type = HitType::Scrollbar };
	}
	const auto y = point.y() - rowsTop() + _scrollTop;
	const auto i = ranges::find_if(_rows, [&](const Row &row) {
		return (y >= row.top) && (y < row.top + row.height);
	});
	if (i == end(_rows)) {
		return {};
	}
	const auto index = int(i - begin(_rows));
	const auto &row = *i;
	const auto x = point.x();
	if (x < namesWidth()) {
		switch (row.type) {
		case RowType::Layer:
			return {
				.type = (x < Scaled(kPadding + kDisclosureWidth))
					? HitType::Disclosure
					: HitType::LayerName,
				.row = index,
			};
		case RowType::Property: {
			const auto size = Scaled(20);
			const auto left = namesWidth() - Scaled(kPadding) - size;
			return {
				.type = (x >= left)
					? HitType::Navigator
					: HitType::PropertyName,
				.row = index,
			};
		}
		case RowType::Empty:
			return {};
		}
		return {};
	}
	switch (row.type) {
	case RowType::Layer: {
		const auto from = xFromFrame(row.inPoint);
		const auto till = xFromFrame(row.outPoint);
		const auto grab = Scaled(kEdgeGrab);
		// The bar is cut at the composition edges, an edge outside of
		// the composition can't be grabbed.
		const auto [left, right] = compositionX();
		const auto type = (x < left - grab || x > right + grab)
			? HitType::LayerTrack
			: (till <= right + 0.5 && std::abs(x - till) <= grab)
			? HitType::LayerEnd
			: (from >= left - 0.5 && std::abs(x - from) <= grab)
			? HitType::LayerStart
			: (x > from && x < till)
			? HitType::LayerBar
			: HitType::LayerTrack;
		return { .type = type, .row = index };
	}
	case RowType::Property: {
		auto best = -1;
		auto distance = Scaled(kDiamond) + Scaled(3) + 0.;
		for (auto k = 0; k != int(row.keyframes.size()); ++k) {
			const auto root = row.keyframes[k].time * row.scale + row.offset;
			if (!inComposition(root)) {
				continue;
			}
			const auto d = std::abs(keyframeX(row, k) - x);
			if (d <= distance) {
				distance = d;
				best = k;
			}
		}
		return (best >= 0)
			? Hit{ .type = HitType::Keyframe, .row = index, .keyframe = best }
			: Hit{ .type = HitType::PropertyTrack, .row = index };
	}
	case RowType::Empty:
		return {};
	}
	return {};
}

bool TimelinePanel::onPlayhead(QPoint point, const Hit &hit) const {
	switch (hit.type) {
	case HitType::LayerBar:
	case HitType::LayerTrack:
	case HitType::PropertyTrack:
		break;
	default:
		return false;
	}
	const auto x = xFromFrame(_controller->currentFrame());
	return std::abs(point.x() - x) <= Scaled(3);
}

std::optional<double> TimelinePanel::localFrameOf(const Row &row) const {
	if (!row.property) {
		return std::nullopt;
	}
	return _controller->document().localFrame(
		row.property.node,
		_controller->currentFrame());
}

std::optional<int> TimelinePanel::keyframeAtPlayhead(const Row &row) const {
	const auto current = double(_controller->currentFrame());
	for (auto k = 0; k != int(row.keyframes.size()); ++k) {
		const auto root = row.keyframes[k].time * row.scale + row.offset;
		if (std::abs(root - current) < 0.5) {
			return k;
		}
	}
	return std::nullopt;
}

KeyframeRef TimelinePanel::keyframeRef(const Row &row, int index) const {
	return { .property = row.property, .time = row.keyframes[index].time };
}

std::vector<KeyframeRef> TimelinePanel::keyframesInRect(QRect rect) {
	ensureRows();
	auto result = std::vector<KeyframeRef>();
	for (const auto &row : _rows) {
		if (row.type != RowType::Property) {
			continue;
		}
		const auto y = rowsTop() + row.top - _scrollTop + row.height / 2;
		if (y < rect.top() || y > rect.bottom()) {
			continue;
		}
		for (auto k = 0; k != int(row.keyframes.size()); ++k) {
			const auto root = row.keyframes[k].time * row.scale + row.offset;
			if (!inComposition(root)) {
				continue;
			}
			const auto x = keyframeX(row, k);
			if (x >= rect.left() && x <= rect.right()) {
				result.push_back(keyframeRef(row, k));
			}
		}
	}
	return result;
}

// Actions.

void TimelinePanel::seek(double x) {
	_controller->setCurrentFrame(frameAtX(x));
}

void TimelinePanel::pressKeyframe(
		const Hit &hit,
		Qt::KeyboardModifiers modifiers) {
	const auto &row = _rows[hit.row];
	const auto ref = keyframeRef(row, hit.keyframe);
	const auto toggle = (modifiers & Qt::ControlModifier) != 0;
	const auto add = (modifiers & Qt::ShiftModifier) != 0;
	const auto property = row.property;
	const auto scale = row.scale;
	const auto editable = row.editable;
	_controller->setActiveProperty(property);

	auto selected = _controller->selectedKeyframes();
	const auto already = _controller->isKeyframeSelected(ref);
	auto selectOnClick = std::optional<KeyframeRef>();
	if (toggle) {
		if (already) {
			selected.erase(
				ranges::remove_if(selected, [&](const KeyframeRef &item) {
					return (item.property == ref.property)
						&& std::abs(item.time - ref.time) < 1e-3;
				}),
				end(selected));
			_controller->setSelectedKeyframes(std::move(selected));
			return;
		}
		selected.push_back(ref);
		_controller->setSelectedKeyframes(std::move(selected));
	} else if (add) {
		if (!already) {
			selected.push_back(ref);
			_controller->setSelectedKeyframes(std::move(selected));
		}
	} else if (!already) {
		_controller->setSelectedKeyframes({ ref });
	} else if (selected.size() > 1) {
		selectOnClick = ref;
	}
	_drag = std::make_unique<Drag>();
	_drag->type = editable ? Drag::Type::Keyframes : Drag::Type::None;
	_drag->press = QPoint();
	_drag->property = property;
	_drag->scale = scale;
	_drag->selectOnClick = selectOnClick;
}

void TimelinePanel::pressLayer(
		const Hit &hit,
		Qt::KeyboardModifiers modifiers) {
	const auto &row = _rows[hit.row];
	const auto layer = row.layer;
	const auto toggle = (modifiers & Qt::ControlModifier) != 0;
	const auto add = (modifiers & Qt::ShiftModifier) != 0;
	const auto screenTop = row.top - _scrollTop;
	if (toggle) {
		_controller->select(layer, SelectMode::Toggle);
	} else if (add) {
		_controller->select(layer, SelectMode::Add);
	} else if (!_controller->isSelected(layer)) {
		_controller->select(layer);
	}
	// Keep the pressed row under the cursor while rows expand / collapse.
	ensureRows();
	const auto i = ranges::find_if(_rows, [&](const Row &row) {
		return (row.type == RowType::Layer) && (row.layer == layer);
	});
	if (i != end(_rows)) {
		_scrollTop = i->top - screenTop;
		clampScroll();
	}
	update();
}

void TimelinePanel::toggleKeyframeAtPlayhead(int index) {
	ensureRows();
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	const auto &row = _rows[index];
	if (row.type != RowType::Property) {
		return;
	}
	const auto property = row.property;
	_controller->setActiveProperty(property);
	if (const auto k = keyframeAtPlayhead(row)) {
		_controller->removeKeyframes({ keyframeRef(row, *k) });
	} else if (const auto local = localFrameOf(row)) {
		const auto time = RoundTime(*local);
		if (_controller->addKeyframe(property, time)) {
			_controller->setSelectedKeyframes({ { property, time } });
		}
	}
}

void TimelinePanel::addKeyframeAt(int index, int rootFrame) {
	ensureRows();
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	const auto &row = _rows[index];
	if (row.type != RowType::Property) {
		return;
	}
	const auto property = row.property;
	_controller->setPlaying(false);
	_controller->setCurrentFrame(rootFrame);
	_controller->setActiveProperty(property);
	const auto local = localFrameOf(row);
	if (!local) {
		return;
	}
	const auto time = RoundTime(*local);
	if (_controller->addKeyframe(property, time)) {
		_controller->setSelectedKeyframes({ { property, time } });
	}
}

void TimelinePanel::setEasing(EasingPreset preset) {
	const auto selected = _controller->selectedKeyframes();
	if (!selected.empty()) {
		_controller->setKeyframeEasing(selected, Easing::FromPreset(preset));
	}
}

void TimelinePanel::selectAllKeyframes() {
	ensureRows();
	auto result = std::vector<KeyframeRef>();
	for (const auto &row : _rows) {
		if (row.type == RowType::Property) {
			for (auto k = 0; k != int(row.keyframes.size()); ++k) {
				result.push_back(keyframeRef(row, k));
			}
		}
	}
	_controller->setSelectedKeyframes(std::move(result));
}

void TimelinePanel::showToast(const QString &text) {
	if (const auto show = _controller->uiShow()) {
		show->showToast(text);
	}
}

bool TimelinePanel::copyKeyframes() {
	const auto selected = _controller->selectedKeyframes();
	if (selected.empty()) {
		return false;
	}
	const auto &document = _controller->document();
	auto result = std::vector<CopiedKeyframe>();
	auto earliest = std::numeric_limits<double>::max();
	for (const auto &ref : selected) {
		const auto info = document.property(ref.property);
		if (!info) {
			continue;
		}
		for (const auto &keyframe : document.keyframes(ref.property)) {
			if (std::abs(keyframe.time - ref.time) < 1e-3) {
				result.push_back({
					.property = ref.property,
					.type = info->type,
					.dimensions = info->dimensions,
					.offset = keyframe.time,
					.value = keyframe.value,
					.easing = keyframe.easing,
				});
				earliest = std::min(earliest, keyframe.time);
				break;
			}
		}
	}
	if (result.empty()) {
		return false;
	}
	for (auto &item : result) {
		item.offset -= earliest;
	}
	Clipboard() = std::move(result);
	showToast(tr::lng_oblivion_lottie_timeline_copied(
		tr::now,
		lt_value,
		QString::number(Clipboard().size())));
	return true;
}

bool TimelinePanel::pasteKeyframes() {
	return paste(_controller->activeProperty());
}

bool TimelinePanel::paste(std::optional<PropertyRef> target) {
	const auto &copied = Clipboard();
	if (copied.empty()) {
		return false;
	}
	const auto single = ranges::all_of(copied, [&](const CopiedKeyframe &item) {
		return (item.property == copied.front().property);
	});
	auto edit = Edit{ .document = _controller->document() };
	const auto compatible = [&](
			const PropertyRef &ref,
			const CopiedKeyframe &item) {
		const auto info = edit.document.property(ref);
		return info
			&& (info->type == item.type)
			&& (info->dimensions == item.dimensions);
	};
	const auto current = _controller->currentFrame();
	auto pasted = std::vector<KeyframeRef>();
	for (const auto &item : copied) {
		auto ref = item.property;
		if (single && target && compatible(*target, item)) {
			ref = *target;
		}
		if (!compatible(ref, item)) {
			continue;
		}
		const auto time = RoundTime(
			edit.document.localFrame(ref.node, current) + item.offset);
		auto step = AddKeyframe(
			edit.document,
			ref,
			time,
			item.value,
			item.easing);
		if (!step) {
			continue;
		}
		edit.document = std::move(step.document);
		edit.changed.insert(
			end(edit.changed),
			begin(step.changed),
			end(step.changed));
		pasted.push_back({ .property = ref, .time = time });
	}
	if (pasted.empty()) {
		showToast(tr::lng_oblivion_lottie_timeline_paste_failed(tr::now));
		return true;
	}
	_controller->setPlaying(false);
	if (_controller->perform(Command::AddKeyframe, std::move(edit))) {
		_controller->setSelectedKeyframes(std::move(pasted));
	}
	return true;
}

bool TimelinePanel::jumpToKeyframe(int direction) {
	ensureRows();
	auto times = std::vector<double>();
	for (const auto &row : _rows) {
		if (row.type == RowType::Property) {
			for (const auto &keyframe : row.keyframes) {
				times.push_back(keyframe.time * row.scale + row.offset);
			}
		} else if (row.type == RowType::Layer) {
			times.insert(end(times), begin(row.summary), end(row.summary));
		}
	}
	const auto current = double(_controller->currentFrame());
	auto found = std::optional<double>();
	for (const auto time : times) {
		if (direction > 0 && time > current + 0.5) {
			if (!found || time < *found) {
				found = time;
			}
		} else if (direction < 0 && time < current - 0.5) {
			if (!found || time > *found) {
				found = time;
			}
		}
	}
	if (!found) {
		return !times.empty();
	}
	_controller->setPlaying(false);
	_controller->setCurrentFrame(int(std::round(*found)));
	if (_timeZoom > 1.) {
		const auto x = xFromFrame(*found);
		if (x < trackLeft() || x > trackRight()) {
			setViewStart(*found - viewSpan() / 2.);
		}
	}
	return true;
}

// Mouse.

void TimelinePanel::mousePressEvent(QMouseEvent *e) {
	_menu = nullptr;
	if (_drag || e->button() != Qt::LeftButton) {
		return;
	}
	const auto point = e->pos();
	const auto modifiers = e->modifiers();
	auto hit = hitTest(point);
	if (onPlayhead(point, hit)) {
		hit = Hit{ .type = HitType::Ruler };
	}
	switch (hit.type) {
	case HitType::None:
		if (!(modifiers & (Qt::ShiftModifier | Qt::ControlModifier))) {
			_controller->setSelectedKeyframes({});
		}
		return;
	case HitType::Ruler:
		_controller->setPlaying(false);
		_drag = std::make_unique<Drag>();
		_drag->type = Drag::Type::Scrub;
		_drag->started = true;
		seek(point.x());
		break;
	case HitType::Scrollbar:
		_drag = std::make_unique<Drag>();
		_drag->type = Drag::Type::Scrollbar;
		_drag->started = true;
		_drag->startScroll = _scrollTop;
		break;
	case HitType::Disclosure: {
		const auto &row = _rows[hit.row];
		_expandedOverride[row.layer] = !row.expanded;
		invalidateRows();
	} return;
	case HitType::LayerName:
	case HitType::LayerTrack:
		pressLayer(hit, modifiers);
		return;
	case HitType::LayerBar:
	case HitType::LayerStart:
	case HitType::LayerEnd: {
		const auto layer = _rows[hit.row].layer;
		const auto toggle = (modifiers & Qt::ControlModifier) != 0;
		pressLayer(hit, modifiers);
		if (toggle) {
			return;
		}
		const auto node = _controller->document().node(layer);
		if (!node) {
			return;
		}
		_drag = std::make_unique<Drag>();
		_drag->type = (hit.type == HitType::LayerStart)
			? Drag::Type::LayerStart
			: (hit.type == HitType::LayerEnd)
			? Drag::Type::LayerEnd
			: Drag::Type::LayerShift;
		_drag->layer = layer;
		_drag->inPoint = node->inPoint;
		_drag->outPoint = node->outPoint;
	} break;
	case HitType::PropertyName:
		_controller->setActiveProperty(_rows[hit.row].property);
		return;
	case HitType::Navigator:
		toggleKeyframeAtPlayhead(hit.row);
		return;
	case HitType::Keyframe:
		pressKeyframe(hit, modifiers);
		break;
	case HitType::PropertyTrack: {
		_controller->setActiveProperty(_rows[hit.row].property);
		_drag = std::make_unique<Drag>();
		_drag->type = Drag::Type::Marquee;
		if (modifiers & (Qt::ShiftModifier | Qt::ControlModifier)) {
			_drag->base = _controller->selectedKeyframes();
		} else {
			_controller->setSelectedKeyframes({});
		}
	} break;
	}
	if (_drag) {
		_drag->press = point;
		_drag->pressFrame = frameFromX(point.x());
	}
}

void TimelinePanel::applyDrag(QPoint point, Qt::KeyboardModifiers modifiers) {
	if (!_drag) {
		return;
	}
	const auto &document = _controller->document();
	if (!_drag->started) {
		const auto moved = (point - _drag->press).manhattanLength();
		if (moved < Scaled(kDragThreshold)) {
			return;
		}
		_drag->started = true;
		_drag->mergeKey = "timeline-" + QByteArray::number(++_gestures);
		_drag->start = document;
		_controller->beginGesture(_drag->mergeKey);
		switch (_drag->type) {
		case Drag::Type::Keyframes: {
			_controller->setPlaying(false);
			ensureRows();
			const auto first = double(_controller->firstFrame());
			const auto last = double(_controller->lastFrame());
			auto minRoot = std::numeric_limits<double>::max();
			auto maxRoot = std::numeric_limits<double>::lowest();
			for (const auto &ref : _controller->selectedKeyframes()) {
				const auto row = ranges::find_if(_rows, [&](const Row &row) {
					return (row.type == RowType::Property)
						&& (row.property == ref.property);
				});
				const auto moves = (row != end(_rows))
					&& row->editable
					&& std::abs(row->scale - _drag->scale) < 1e-9;
				if (!moves) {
					_drag->untouched.push_back(ref);
					continue;
				}
				_drag->refs.push_back(ref);
				const auto root = ref.time * row->scale + row->offset;
				minRoot = std::min(minRoot, root);
				maxRoot = std::max(maxRoot, root);
			}
			if (_drag->refs.empty()) {
				_drag->type = Drag::Type::None;
				break;
			}
			_drag->minDelta = std::min(0, int(std::ceil(first - minRoot)));
			_drag->maxDelta = std::max(0, int(std::floor(last - maxRoot)));
		} break;
		case Drag::Type::LayerShift: {
			_controller->setPlaying(false);
			const auto root = document.rootId();
			for (const auto id : _controller->selection()) {
				const auto node = document.node(id);
				if (node
					&& node->kind == NodeKind::Layer
					&& node->composition == root) {
					_drag->layers.push_back(id);
				}
			}
			if (!ranges::contains(_drag->layers, _drag->layer)) {
				_drag->layers = { _drag->layer };
			}
		} break;
		case Drag::Type::LayerStart:
		case Drag::Type::LayerEnd:
			_controller->setPlaying(false);
			break;
		case Drag::Type::Marquee:
		case Drag::Type::Scrub:
		case Drag::Type::Scrollbar:
		case Drag::Type::None:
			break;
		}
	}
	const auto delta = int(std::round(frameFromX(point.x()) - _drag->pressFrame));
	const auto first = double(_controller->firstFrame());
	const auto last = double(_controller->lastFrame());
	switch (_drag->type) {
	case Drag::Type::Scrub:
		seek(point.x());
		break;
	case Drag::Type::Scrollbar: {
		const auto available = std::max(height() - rowsTop(), 1);
		const auto bar = scrollbarRect();
		const auto range = std::max(available - bar.height(), 1);
		_scrollTop = _drag->startScroll
			+ (point.y() - _drag->press.y())
				* std::max(_contentHeight - available, 0)
				/ range;
		clampScroll();
		update();
	} break;
	case Drag::Type::Keyframes: {
		const auto clamped = std::clamp(
			delta,
			_drag->minDelta,
			_drag->maxDelta);
		if (clamped == _drag->delta) {
			break;
		}
		_drag->delta = clamped;
		const auto local = clamped / _drag->scale;
		// Always from the document at the press: dragging over other
		// keyframes doesn't replace them until the drag ends there.
		if (_controller->perform(
				Command::MoveKeyframes,
				MoveKeyframes(_drag->start, _drag->refs, local),
				_drag->mergeKey)) {
			_drag->applied = true;
			auto selection = _drag->untouched;
			for (auto ref : _drag->refs) {
				ref.time = RoundTime(ref.time + local);
				selection.push_back(ref);
			}
			_controller->setSelectedKeyframes(std::move(selection));
		}
	} break;
	case Drag::Type::LayerShift: {
		const auto clamped = std::clamp(
			delta,
			int(std::ceil(first + 1. - _drag->outPoint)),
			std::max(
				int(std::ceil(first + 1. - _drag->outPoint)),
				int(std::floor(last - _drag->inPoint))));
		if (clamped == _drag->delta) {
			break;
		}
		_drag->delta = clamped;
		if (_controller->perform(
				Command::LayerTiming,
				ShiftLayers(_drag->start, _drag->layers, clamped),
				_drag->mergeKey)) {
			_drag->applied = true;
		}
	} break;
	case Drag::Type::LayerStart:
	case Drag::Type::LayerEnd: {
		const auto start = (_drag->type == Drag::Type::LayerStart);
		auto from = _drag->inPoint;
		auto till = _drag->outPoint;
		if (start) {
			const auto low = std::min(_drag->inPoint, first);
			from = std::clamp(
				std::round(_drag->inPoint + delta),
				low,
				std::max(till - 1., low));
		} else {
			const auto low = from + 1.;
			till = std::clamp(
				std::round(_drag->outPoint + delta),
				low,
				std::max({ _drag->outPoint, last + 1., low }));
		}
		const auto key = int(start ? from : till);
		if (key == _drag->delta && _drag->applied) {
			break;
		}
		_drag->delta = key;
		if (_controller->perform(
				Command::LayerTiming,
				SetLayerTiming(_drag->start, _drag->layer, from, till),
				_drag->mergeKey)) {
			_drag->applied = true;
		}
	} break;
	case Drag::Type::Marquee: {
		_drag->rect = QRect(_drag->press, point).normalized();
		auto selection = _drag->base;
		for (const auto &ref : keyframesInRect(_drag->rect)) {
			const auto contained = ranges::any_of(
				selection,
				[&](const KeyframeRef &item) {
					return (item.property == ref.property)
						&& std::abs(item.time - ref.time) < 1e-3;
				});
			if (!contained) {
				selection.push_back(ref);
			}
		}
		_controller->setSelectedKeyframes(std::move(selection));
		update();
	} break;
	case Drag::Type::None:
		break;
	}
}

void TimelinePanel::mouseMoveEvent(QMouseEvent *e) {
	if (_drag) {
		applyDrag(e->pos(), e->modifiers());
		return;
	}
	const auto hit = hitTest(e->pos());
	if (_over != hit) {
		_over = hit;
		update();
	}
	if (onPlayhead(e->pos(), hit)) {
		setCursor(Qt::SizeHorCursor);
		return;
	}
	switch (hit.type) {
	case HitType::LayerStart:
	case HitType::LayerEnd:
		setCursor(Qt::SizeHorCursor);
		break;
	case HitType::Disclosure:
	case HitType::Navigator:
	case HitType::Keyframe:
		setCursor(style::cur_pointer);
		break;
	default:
		setCursor(style::cur_default);
		break;
	}
}

void TimelinePanel::mouseReleaseEvent(QMouseEvent *e) {
	if (!_drag || e->button() != Qt::LeftButton) {
		return;
	}
	const auto selectOnClick = _drag->started
		? std::nullopt
		: _drag->selectOnClick;
	finishDrag(false);
	if (selectOnClick) {
		_controller->setSelectedKeyframes({ *selectOnClick });
	}
}

void TimelinePanel::finishDrag(bool cancel) {
	if (!_drag) {
		return;
	}
	const auto drag = std::move(_drag);
	switch (drag->type) {
	case Drag::Type::Keyframes:
	case Drag::Type::LayerShift:
	case Drag::Type::LayerStart:
	case Drag::Type::LayerEnd:
		if (drag->applied) {
			if (cancel) {
				_controller->cancelGesture(drag->mergeKey);
				if (drag->type == Drag::Type::Keyframes) {
					auto selection = drag->untouched;
					selection.insert(
						end(selection),
						begin(drag->refs),
						end(drag->refs));
					_controller->setSelectedKeyframes(std::move(selection));
				}
			} else {
				_controller->finishMerge();
			}
		}
		break;
	case Drag::Type::Marquee:
		if (cancel) {
			_controller->setSelectedKeyframes(drag->base);
		}
		break;
	case Drag::Type::Scrub:
	case Drag::Type::Scrollbar:
	case Drag::Type::None:
		break;
	}
	update();
}

void TimelinePanel::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	finishDrag(false);
	const auto hit = hitTest(e->pos());
	switch (hit.type) {
	case HitType::Keyframe: {
		const auto &row = _rows[hit.row];
		const auto root = row.keyframes[hit.keyframe].time * row.scale
			+ row.offset;
		_controller->setPlaying(false);
		_controller->setCurrentFrame(int(std::round(root)));
	} break;
	case HitType::PropertyTrack:
		if (!onPlayhead(e->pos(), hit)) {
			addKeyframeAt(hit.row, frameAtX(e->pos().x()));
		} else {
			toggleKeyframeAtPlayhead(hit.row);
		}
		break;
	default:
		break;
	}
}

void TimelinePanel::wheelEvent(QWheelEvent *e) {
	const auto pixel = e->pixelDelta();
	const auto angle = e->angleDelta();
	if (e->modifiers() & Qt::ControlModifier) {
		const auto steps = angle.y()
			? (angle.y() / 120.)
			: (pixel.y() / 50.);
		if (steps != 0.) {
			zoomTime(std::pow(1.25, steps), e->position().x());
		}
	} else {
		auto delta = !pixel.isNull()
			? QPointF(pixel)
			: (QPointF(angle) / 120. * Scaled(kLayerRowHeight * 2));
		if ((e->modifiers() & Qt::ShiftModifier) && !delta.x()) {
			delta = QPointF(delta.y(), 0.);
		}
		if (delta.x() != 0.) {
			const auto width = std::max(trackRight() - trackLeft(), 1);
			setViewStart(_viewStart - delta.x() * viewSpan() / width);
		}
		if (delta.y() != 0.) {
			_scrollTop -= int(std::round(delta.y()));
			clampScroll();
			update();
		}
	}
	e->accept();
}

void TimelinePanel::keyPressEvent(QKeyEvent *e) {
	if (_drag && e->key() == Qt::Key_Escape) {
		finishDrag(true);
		return;
	} else if ((e->modifiers() & Qt::ControlModifier)
		&& e->key() == Qt::Key_A) {
		selectAllKeyframes();
		return;
	}
	RpWidget::keyPressEvent(e);
}

void TimelinePanel::leaveEventHook(QEvent *e) {
	if (_over != Hit()) {
		_over = Hit();
		update();
	}
	RpWidget::leaveEventHook(e);
}

void TimelinePanel::contextMenuEvent(QContextMenuEvent *e) {
	if (_drag) {
		return;
	}
	const auto hit = hitTest(e->pos());
	switch (hit.type) {
	case HitType::Keyframe: {
		const auto ref = keyframeRef(_rows[hit.row], hit.keyframe);
		if (!_controller->isKeyframeSelected(ref)) {
			_controller->setSelectedKeyframes({ ref });
		}
		showKeyframeMenu(hit.row, e->globalPos());
	} break;
	case HitType::PropertyName:
	case HitType::PropertyTrack:
	case HitType::Navigator:
		showRowMenu(hit.row, e->globalPos());
		break;
	case HitType::LayerName:
	case HitType::LayerBar:
	case HitType::LayerStart:
	case HitType::LayerEnd:
	case HitType::LayerTrack: {
		const auto layer = _rows[hit.row].layer;
		if (!_controller->isSelected(layer)) {
			pressLayer(hit, Qt::NoModifier);
		}
		ensureRows();
		const auto i = ranges::find_if(_rows, [&](const Row &row) {
			return (row.type == RowType::Layer) && (row.layer == layer);
		});
		if (i != end(_rows)) {
			showRowMenu(int(i - begin(_rows)), e->globalPos());
		}
	} break;
	default:
		break;
	}
	e->accept();
}

void TimelinePanel::showKeyframeMenu(int index, QPoint globalPosition) {
	const auto &document = _controller->document();
	const auto selected = _controller->selectedKeyframes();
	if (selected.empty()) {
		return;
	}
	auto current = std::optional<EasingPreset>();
	auto mixed = false;
	for (const auto &ref : selected) {
		for (const auto &keyframe : document.keyframes(ref.property)) {
			if (std::abs(keyframe.time - ref.time) >= 1e-3
				|| keyframe.last) {
				continue;
			}
			const auto preset = keyframe.easing.preset();
			if (current && *current != preset) {
				mixed = true;
			}
			current = preset;
		}
	}
	if (mixed) {
		current = std::nullopt;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	for (const auto preset : {
		EasingPreset::Linear,
		EasingPreset::EaseIn,
		EasingPreset::EaseOut,
		EasingPreset::EaseInOut,
		EasingPreset::Hold,
	}) {
		_menu->addAction(
			EasingPresetText(preset),
			[=] { setEasing(preset); },
			(current == preset) ? &st::mediaPlayerMenuCheck : nullptr);
	}
	_menu->addSeparator();
	if (selected.size() == 1
		&& index >= 0
		&& index < int(_rows.size())) {
		const auto &row = _rows[index];
		const auto root = int(std::round(
			selected.front().time * row.scale + row.offset));
		_menu->addAction(
			tr::lng_oblivion_lottie_timeline_go_to(tr::now),
			[=] {
				_controller->setPlaying(false);
				_controller->setCurrentFrame(root);
			},
			&st::menuIconTimer);
	}
	_menu->addAction(
		tr::lng_oblivion_lottie_timeline_copy(tr::now),
		[=] { copyKeyframes(); },
		&st::menuIconCopy);
	if (!Clipboard().empty()) {
		_menu->addAction(
			tr::lng_oblivion_lottie_timeline_paste(tr::now),
			[=] { paste(_controller->activeProperty()); },
			&st::menuIconRestore);
	}
	_menu->addAction(
		tr::lng_oblivion_lottie_timeline_delete(tr::now),
		[=] {
			_controller->removeKeyframes(_controller->selectedKeyframes());
		},
		&st::menuIconDelete);
	_menu->popup(globalPosition);
}

void TimelinePanel::showRowMenu(int index, QPoint globalPosition) {
	ensureRows();
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	const auto &row = _rows[index];
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	if (row.type == RowType::Property) {
		const auto property = row.property;
		_controller->setActiveProperty(property);
		const auto existing = keyframeAtPlayhead(row).has_value();
		_menu->addAction(
			(existing
				? tr::lng_oblivion_lottie_timeline_remove_keyframe(tr::now)
				: tr::lng_oblivion_lottie_timeline_add_keyframe(tr::now)),
			[=] {
				ensureRows();
				const auto i = ranges::find_if(_rows, [&](const Row &row) {
					return (row.type == RowType::Property)
						&& (row.property == property);
				});
				if (i != end(_rows)) {
					toggleKeyframeAtPlayhead(int(i - begin(_rows)));
				}
			},
			existing ? &st::menuIconDelete : &st::menuIconAdd);
		if (!Clipboard().empty()) {
			_menu->addAction(
				tr::lng_oblivion_lottie_timeline_paste(tr::now),
				[=] { paste(property); },
				&st::menuIconRestore);
		}
		_menu->addAction(
			tr::lng_oblivion_lottie_timeline_select_all(tr::now),
			[=] { selectAllKeyframes(); },
			&st::menuIconSelect);
	} else if (row.type == RowType::Layer) {
		const auto layer = row.layer;
		const auto inPoint = row.inPoint;
		const auto outPoint = row.outPoint;
		const auto current = double(_controller->currentFrame());
		if (current < outPoint) {
			_menu->addAction(
				tr::lng_oblivion_lottie_timeline_layer_start(tr::now),
				[=] {
					_controller->setLayerTiming(layer, current, outPoint);
				},
				&st::menuIconTimer);
		}
		if (current + 1. > inPoint) {
			_menu->addAction(
				tr::lng_oblivion_lottie_timeline_layer_end(tr::now),
				[=] {
					_controller->setLayerTiming(layer, inPoint, current + 1.);
				},
				&st::menuIconTimer);
		}
		_menu->addAction(
			tr::lng_oblivion_lottie_timeline_select_all(tr::now),
			[=] { selectAllKeyframes(); },
			&st::menuIconSelect);
	}
	if (_menu->empty()) {
		_menu = nullptr;
		return;
	}
	_menu->popup(globalPosition);
}

// Painting.

void TimelinePanel::paintEvent(QPaintEvent *e) {
	ensureRows();
	auto p = QPainter(this);
	const auto clip = e->rect();
	p.fillRect(clip, st::windowBg);
	paintTransport(p, clip);
	paintRows(p, clip);
	paintRuler(p);
	paintPlayhead(p);
	if (_drag
		&& _drag->type == Drag::Type::Marquee
		&& _drag->started
		&& !_drag->rect.isEmpty()) {
		auto hq = PainterHighQualityEnabler(p);
		const auto accent = st::windowActiveTextFg->c;
		p.setPen(QPen(anim::with_alpha(accent, 0.8), 1.));
		p.setBrush(anim::with_alpha(accent, 0.12));
		p.drawRect(QRectF(_drag->rect).adjusted(0.5, 0.5, -0.5, -0.5));
	}
	paintScrollbar(p);
}

void TimelinePanel::paintTransport(QPainter &p, QRect clip) {
	const auto height = Scaled(kTransportHeight);
	if (!clip.intersects(QRect(0, 0, width(), height))) {
		return;
	}
	const auto fps = _controller->fps();
	const auto index = _controller->frameIndex();
	const auto text = tr::lng_oblivion_lottie_timeline_position(
		tr::now,
		lt_frame,
		tr::lng_oblivion_lottie_editor_frame(
			tr::now,
			lt_value,
			QString::number(_controller->currentFrame())),
		lt_time,
		FormatSeconds((fps > 0.) ? (index / fps) : 0.));
	const auto left = _loop->x() + _loop->width() + Scaled(kGroupSkip);
	const auto right = _zoomOut->x() - Scaled(kGroupSkip);
	if (right <= left) {
		return;
	}
	const auto &font = st::normalFont;
	p.setFont(font);
	p.setPen(st::windowFg);
	p.drawText(
		left,
		(height - font->height) / 2 + font->ascent,
		font->elided(text, right - left));
}

void TimelinePanel::paintRuler(QPainter &p) {
	const auto top = Scaled(kTransportHeight);
	const auto height = Scaled(kRulerHeight);
	const auto names = namesWidth();
	const auto line = st::lineWidth;
	p.fillRect(0, top, width(), height, st::windowBg);
	p.fillRect(0, top, width(), line, st::shadowFg);
	p.fillRect(0, top + height - line, width(), line, st::shadowFg);
	p.fillRect(names - line, top, line, height, st::shadowFg);

	const auto small = SmallFont();
	const auto bold = SmallBoldFont();
	p.setFont(bold);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		Scaled(kPadding),
		top + (height - bold->height) / 2 + bold->ascent,
		bold->elided(
			tr::lng_oblivion_lottie_editor_layers(tr::now),
			names - 2 * Scaled(kPadding)));

	p.save();
	p.setClipRect(QRect(names, top, width() - names, height));
	const auto first = _controller->firstFrame();
	const auto count = std::max(_controller->frameCount(), 1);
	const auto span = viewSpan();
	const auto pixelsPerFrame = (trackRight() - trackLeft()) / span;
	const auto spacing = Scaled(kLabelSpacing);
	const auto from = std::max(first, int(std::floor(_viewStart)));
	const auto till = std::min(
		first + count,
		int(std::ceil(_viewStart + span)) + 1);
	const auto labelTop = top + Scaled(4) + small->ascent;

	// The playhead badge wins over every label, then seconds (in the
	// accent color) win over frame numbers.
	auto taken = std::vector<std::pair<double, double>>();
	const auto badge = playheadBadgeRect();
	if (!badge.isEmpty()) {
		taken.emplace_back(
			badge.left() - Scaled(2),
			badge.right() + Scaled(2));
	}
	const auto overlapsTaken = [&](double l, double r) {
		return ranges::any_of(taken, [&](const auto &range) {
			return (l < range.second) && (r > range.first);
		});
	};
	// A label right of its tick, or left of it when it would be cut by the
	// right edge (the last second of the composition).
	const auto labelLeft = [&](double x, int w) {
		const auto after = x + Scaled(3);
		return (after + w <= width()) ? after : (x - Scaled(3) - w);
	};
	const auto fps = _controller->fps();
	const auto accent = st::windowActiveTextFg->c;
	p.setFont(small);
	const auto step = (fps > 0.)
		? PickSecondsStep(pixelsPerFrame * fps, spacing)
		: 0.;
	if (step > 0.) {
		const auto startSecond = std::ceil((from - first) / fps / step) * step;
		for (auto second = startSecond;; second += step) {
			const auto frame = first + second * fps;
			if (frame > till) {
				break;
			}
			const auto x = xFromFrame(frame);
			p.fillRect(
				QRectF(x, top + height / 2., line, height / 2.),
				anim::with_alpha(accent, 0.6));
			const auto text = FormatSeconds(second);
			const auto w = small->width(text);
			const auto l = labelLeft(x, w);
			if (overlapsTaken(l, l + w)) {
				continue;
			}
			p.setPen(accent);
			p.drawText(QPointF(l, labelTop), text);
			taken.emplace_back(l - Scaled(4), l + w + Scaled(4));
		}
	}

	const auto labelStep = PickStep(pixelsPerFrame, spacing);
	const auto minorStep = PickMinorStep(pixelsPerFrame, labelStep);
	const auto sub = st::windowSubTextFg->c;
	const auto startFrame = first
		+ int64(std::ceil((from - first) / double(minorStep))) * minorStep;
	for (auto frame = startFrame; frame <= till; frame += minorStep) {
		const auto x = xFromFrame(frame);
		const auto major = !((frame - first) % labelStep);
		const auto tick = major ? Scaled(7) : Scaled(4);
		p.fillRect(
			QRectF(x, top + height - line - tick, line, tick),
			anim::with_alpha(sub, major ? 0.7 : 0.4));
		if (!major) {
			continue;
		}
		const auto text = QString::number(frame);
		const auto w = small->width(text);
		const auto l = labelLeft(x, w);
		if (!overlapsTaken(l, l + w)) {
			p.setPen(sub);
			p.drawText(QPointF(l, labelTop), text);
		}
	}
	p.restore();
}

QRectF TimelinePanel::playheadBadgeRect() const {
	const auto names = namesWidth();
	const auto x = xFromFrame(_controller->currentFrame());
	if (x < names || x > width()) {
		return QRectF();
	}
	const auto top = Scaled(kTransportHeight);
	const auto ruler = Scaled(kRulerHeight);
	const auto font = SmallBoldFont();
	const auto text = QString::number(_controller->currentFrame());
	const auto w = font->width(text) + Scaled(12);
	const auto h = font->height + Scaled(4);
	return QRectF(
		std::clamp(x - w / 2., double(names), double(width() - w)),
		top + ruler - h - Scaled(3),
		w,
		h);
}

void TimelinePanel::paintRows(QPainter &p, QRect clip) {
	const auto top = rowsTop();
	const auto area = QRect(0, top, width(), height() - top);
	if (!area.intersects(clip)) {
		return;
	}
	p.save();
	p.setClipRect(area.intersected(clip));
	const auto names = namesWidth();
	if (_rows.empty()) {
		// Centered in the tracks area, the names column stays empty.
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(names, top, width() - names, height() - top),
			tr::lng_oblivion_lottie_editor_no_layers(tr::now),
			style::al_center);
	}
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto &row = _rows[i];
		const auto y = top + row.top - _scrollTop;
		if (y + row.height <= area.top() || y >= area.bottom() + 1) {
			continue;
		}
		if (row.type == RowType::Layer) {
			paintLayerRow(p, row, y);
		} else {
			paintPropertyRow(p, row, i, y);
		}
	}
	// Names column separator.
	p.fillRect(names - st::lineWidth, top, st::lineWidth, height() - top, st::shadowFg);
	p.restore();
}

void TimelinePanel::paintLayerRow(QPainter &p, const Row &row, int y) {
	const auto names = namesWidth();
	const auto padding = Scaled(kPadding);
	const auto line = st::lineWidth;
	const auto h = row.height;
	const auto index = int(&row - _rows.data());
	const auto hovered = (_over.row == index);
	if (row.selected) {
		p.fillRect(0, y, width(), h, st::windowBgOver);
	} else if (hovered && _over.type == HitType::LayerName) {
		p.fillRect(0, y, names, h, st::windowBgOver);
	}
	p.fillRect(0, y, width(), line, anim::with_alpha(st::shadowFg->c, 0.5));

	auto hq = PainterHighQualityEnabler(p);

	// Disclosure chevron, the same as in the layers panel above.
	const auto cx = padding + Scaled(kDisclosureWidth) / 2. - Scaled(4);
	const auto cy = y + h / 2.;
	const auto s = Scaled(3) + 0.5;
	auto chevron = QPen((hovered && _over.type == HitType::Disclosure)
		? st::windowFg->c
		: st::windowSubTextFg->c);
	chevron.setWidthF(Scaled(15) / 10.);
	chevron.setCapStyle(Qt::RoundCap);
	chevron.setJoinStyle(Qt::RoundJoin);
	p.setPen(chevron);
	p.setBrush(Qt::NoBrush);
	auto path = QPainterPath();
	if (row.expanded) {
		path.moveTo(cx - s, cy - s / 2.);
		path.lineTo(cx, cy + s / 2.);
		path.lineTo(cx + s, cy - s / 2.);
	} else {
		path.moveTo(cx - s / 2., cy - s);
		path.lineTo(cx + s / 2., cy);
		path.lineTo(cx - s / 2., cy + s);
	}
	p.drawPath(path);

	// Color label and name.
	p.setPen(Qt::NoPen);
	const auto &color = LayerColor(row.colorIndex);
	const auto swatch = QRectF(
		padding + Scaled(kDisclosureWidth) - Scaled(4),
		y + (h - Scaled(12)) / 2.,
		Scaled(4),
		Scaled(12));
	p.setBrush(color);
	p.drawRoundedRect(swatch, Scaled(2), Scaled(2));

	const auto textLeft = int(swatch.x() + swatch.width()) + Scaled(8);
	const auto &font = row.selected ? st::semiboldFont : st::normalFont;
	p.setFont(font);
	p.setPen(row.hidden ? st::windowSubTextFg : st::windowFg);
	p.drawText(
		textLeft,
		y + (h - font->height) / 2 + font->ascent,
		font->elided(row.label, std::max(names - textLeft - padding, 0)));

	// Bar, cut at the composition edges (a layer may last longer than the
	// composition, that part is never shown).
	p.save();
	p.setClipRect(trackClip(y, h, Scaled(1) * 1.), Qt::IntersectClip);
	const auto from = xFromFrame(row.inPoint);
	const auto till = xFromFrame(row.outPoint);
	const auto bar = QRectF(
		from,
		y + Scaled(6),
		std::max(till - from, 2.),
		h - Scaled(12));
	const auto dim = row.hidden ? 0.5 : 1.;
	auto border = QPen(anim::with_alpha(color->c, 0.9 * dim));
	border.setWidthF(row.selected ? Scaled(1) * 1.5 : 1.);
	p.setPen(border);
	p.setBrush(anim::with_alpha(color->c, (row.selected ? 0.42 : 0.22) * dim));
	p.drawRoundedRect(bar, Scaled(4), Scaled(4));

	// Trim grips.
	if (hovered && (_over.type == HitType::LayerStart
		|| _over.type == HitType::LayerEnd
		|| _over.type == HitType::LayerBar)) {
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(color->c, dim));
		const auto grip = Scaled(3) * 1.;
		for (const auto x : { bar.left(), bar.right() - grip }) {
			p.drawRoundedRect(
				QRectF(x, bar.top(), grip, bar.height()),
				grip / 2.,
				grip / 2.);
		}
	}
	p.restore();

	// Keyframe marks of a collapsed layer.
	if (!row.summary.empty()) {
		const auto half = Scaled(3) * 1.;
		p.save();
		p.setClipRect(trackClip(y, h, half * 2.), Qt::IntersectClip);
		for (const auto time : row.summary) {
			const auto x = xFromFrame(time);
			if (!inComposition(time)
				|| x < names - half
				|| x > width() + half) {
				continue;
			}
			PaintKeyframe(
				p,
				QPointF(x, y + h / 2.),
				half,
				EasingPreset::Linear,
				anim::with_alpha(color->c, dim),
				st::windowBg->c);
		}
		p.restore();
	}
}

void TimelinePanel::paintPropertyRow(
		QPainter &p,
		const Row &row,
		int index,
		int y) {
	const auto names = namesWidth();
	const auto padding = Scaled(kPadding);
	const auto h = row.height;
	const auto active = (row.type == RowType::Property)
		&& (_controller->activeProperty() == row.property);
	if (active) {
		p.fillRect(
			0,
			y,
			width(),
			h,
			anim::with_alpha(st::windowBgOver->c, 0.7));
	}
	const auto textLeft = padding + Scaled(kIndent);
	const auto font = st::normalFont;
	const auto baseline = y + (h - font->height) / 2 + font->ascent;
	if (row.type == RowType::Empty) {
		p.setFont(font);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			textLeft,
			baseline,
			font->elided(row.label, std::max(names - textLeft - padding, 0)));
		return;
	}

	// Name: property, then its owner in the secondary color.
	const auto navigatorSize = Scaled(20);
	const auto navigatorLeft = names - padding - navigatorSize;
	const auto available = std::max(navigatorLeft - textLeft - Scaled(6), 0);
	p.setFont(font);
	p.setPen(active ? st::windowBoldFg : st::windowFg);
	const auto label = font->elided(row.label, available);
	p.drawText(textLeft, baseline, label);
	const auto labelWidth = font->width(label);
	if (!row.owner.isEmpty() && labelWidth + Scaled(8) < available) {
		p.setPen(st::windowSubTextFg);
		p.drawText(
			textLeft + labelWidth + Scaled(6),
			baseline,
			font->elided(row.owner, available - labelWidth - Scaled(6)));
	}

	auto hq = PainterHighQualityEnabler(p);

	// Keyframe at the current frame: filled / outlined toggle.
	const auto current = keyframeAtPlayhead(row);
	const auto navigatorHovered = (_over.row == index)
		&& (_over.type == HitType::Navigator);
	const auto navigator = QRectF(
		navigatorLeft,
		y + (h - navigatorSize) / 2.,
		navigatorSize,
		navigatorSize);
	if (navigatorHovered) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgRipple);
		p.drawRoundedRect(navigator, Scaled(4), Scaled(4));
	}
	const auto accent = st::windowActiveTextFg->c;
	const auto icon = navigatorHovered
		? st::menuIconFgOver->c
		: st::menuIconFg->c;
	PaintKeyframe(
		p,
		navigator.center(),
		Scaled(4) * 1.,
		EasingPreset::Linear,
		current ? accent : QColor(0, 0, 0, 0),
		current ? accent : icon);

	// Track: the interpolation lines are cut at the composition edges, the
	// keyframes outside of the composition are not shown.
	const auto cy = y + h / 2.;
	const auto count = int(row.keyframes.size());
	const auto sub = st::windowSubTextFg->c;
	p.save();
	p.setClipRect(trackClip(y, h, 0.), Qt::IntersectClip);
	for (auto k = 0; k + 1 < count; ++k) {
		if (row.keyframes[k].easing.hold) {
			continue;
		}
		const auto x1 = keyframeX(row, k);
		const auto x2 = keyframeX(row, k + 1);
		p.fillRect(
			QRectF(x1, cy - Scaled(1), x2 - x1, Scaled(2)),
			anim::with_alpha(sub, 0.25));
	}
	p.restore();

	const auto half = Scaled(kDiamond) * 1.;
	p.save();
	p.setClipRect(trackClip(y, h, half * 2.), Qt::IntersectClip);
	for (auto k = 0; k != count; ++k) {
		const auto &keyframe = row.keyframes[k];
		const auto x = keyframeX(row, k);
		if (!inComposition(keyframe.time * row.scale + row.offset)
			|| x < names - half * 2
			|| x > width() + half * 2) {
			continue;
		}
		const auto selected = _controller->isKeyframeSelected(
			keyframeRef(row, k));
		const auto hovered = (_over.row == index)
			&& (_over.type == HitType::Keyframe)
			&& (_over.keyframe == k);
		const auto preset = keyframe.last
			? ((k > 0 && row.keyframes[k - 1].easing.hold)
				? EasingPreset::Hold
				: EasingPreset::Linear)
			: keyframe.easing.preset();
		PaintKeyframe(
			p,
			QPointF(x, cy),
			hovered ? (half + Scaled(1)) : half,
			preset,
			selected ? accent : (row.editable ? sub : anim::with_alpha(sub, 0.5)),
			st::windowBg->c);
	}
	p.restore();
}

void TimelinePanel::paintPlayhead(QPainter &p) {
	const auto names = namesWidth();
	const auto x = xFromFrame(_controller->currentFrame());
	if (x < names || x > width()) {
		return;
	}
	const auto top = Scaled(kTransportHeight);
	const auto ruler = Scaled(kRulerHeight);
	const auto accent = st::windowActiveTextFg->c;
	const auto line = Scaled(1) * 1.5;
	p.save();
	p.setClipRect(QRect(names, top, width() - names, height() - top));
	p.fillRect(
		QRectF(x - line / 2., top + ruler, line, height() - top - ruler),
		accent);

	auto hq = PainterHighQualityEnabler(p);
	const auto font = SmallBoldFont();
	const auto text = QString::number(_controller->currentFrame());
	const auto rect = playheadBadgeRect();
	const auto h = rect.height();
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgActive);
	p.drawRoundedRect(rect, h / 2., h / 2.);
	p.drawPolygon(QPolygonF({
		QPointF(x - Scaled(4), rect.bottom() - 0.5),
		QPointF(x + Scaled(4), rect.bottom() - 0.5),
		QPointF(x, rect.bottom() + Scaled(3)),
	}));
	p.setFont(font);
	p.setPen(st::windowFgActive);
	p.drawText(rect, text, style::al_center);
	p.restore();
}

void TimelinePanel::paintScrollbar(QPainter &p) {
	const auto rect = scrollbarRect();
	if (rect.isEmpty()) {
		return;
	}
	const auto active = (_drag && _drag->type == Drag::Type::Scrollbar)
		|| (_over.type == HitType::Scrollbar);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(anim::with_alpha(
		st::windowSubTextFg->c,
		active ? 0.6 : 0.35));
	p.drawRoundedRect(rect, rect.width() / 2., rect.width() / 2.);
}

} // namespace Oblivion::LottieEdit
