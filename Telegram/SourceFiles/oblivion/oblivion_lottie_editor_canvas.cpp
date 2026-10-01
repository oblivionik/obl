/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie_editor_canvas.h"

#include "lang/lang_keys.h"
#include "oblivion/oblivion_lottie_doc.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "ui/effects/animation_value.h"
#include "ui/painter.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QNativeGestureEvent>
#include <QtGui/QWheelEvent>

#include <cmath>

namespace Oblivion::LottieEdit {
namespace {

constexpr auto kCanvasMargin = 24;
constexpr auto kCheckerCell = 8;
constexpr auto kMaxRenderSide = 4096;
constexpr auto kPlaybackRenderSide = 1400; // Device pixels while playing.
constexpr auto kDragThreshold = 3;
constexpr auto kMaxOverlayItems = 48;
constexpr auto kKeepVisible = 48; // Canvas pixels kept in view by panning.
constexpr auto kWheelZoomStep = 1.2; // Per wheel notch.
constexpr auto kMinZoom = 0.1; // EditorController clamps to [0.1, 32].
constexpr auto kMaxZoom = 32.;
constexpr auto kAnchorRadius = 4;

// Dark / light backdrops show how a sticker looks in dark / light chats,
// they are a part of the preview, not of the editor UI.
constexpr auto kDarkBackdrop = QColor(0x21, 0x21, 0x24);
constexpr auto kLightBackdrop = QColor(0xff, 0xff, 0xff);

// Selection handles and the anchor are drawn over the animation itself,
// so they are white in both themes (like in other design tools): a dark
// fill of the night palette reads as black squares on the artwork.
constexpr auto kHandleFill = QColor(0xff, 0xff, 0xff);

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] CanvasBackground &LastBackground() {
	static auto result = CanvasBackground::Checker;
	return result;
}

[[nodiscard]] bool IsGeometry(ShapeType type) {
	switch (type) {
	case ShapeType::Group:
	case ShapeType::Rectangle:
	case ShapeType::Ellipse:
	case ShapeType::Star:
	case ShapeType::Path:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] bool HasShapeChildren(const NodeInfo &node) {
	return (node.kind == NodeKind::Layer && node.layerType == LayerType::Shape)
		|| (node.kind == NodeKind::Shape && node.shapeType == ShapeType::Group);
}

// The node whose geometry represents the selected one on the canvas:
// fills, strokes, modifiers and group transforms show their container,
// effects their layer.
[[nodiscard]] NodeId GeometryNode(const Document &document, NodeId id) {
	const auto node = document.node(id);
	if (!node) {
		return 0;
	} else if (node->kind == NodeKind::Effect) {
		return node->layer;
	} else if (node->kind == NodeKind::Shape && !IsGeometry(node->shapeType)) {
		return node->parent;
	}
	return id;
}

enum class MoveKind : uchar {
	Transform, // Layer "ks.p" / group "tr" "p".
	ShapePosition, // Rectangle / ellipse / star "p".
};

struct Movable {
	NodeId id = 0;
	MoveKind kind = MoveKind::Transform;
};

// What moves when a node is dragged: layers and groups move their
// transform, rectangles / ellipses / stars their own position, the rest
// (paths, fills, masks...) their container.
[[nodiscard]] std::optional<Movable> ResolveMovable(
		const Document &document,
		NodeId id) {
	for (auto guard = 0; guard != 64; ++guard) {
		const auto node = document.node(id);
		if (!node) {
			return std::nullopt;
		}
		switch (node->kind) {
		case NodeKind::Layer:
			return Movable{ .id = id, .kind = MoveKind::Transform };
		case NodeKind::Mask:
		case NodeKind::Effect:
			id = node->layer;
			continue;
		case NodeKind::Shape:
			if (node->shapeType == ShapeType::Group) {
				return Movable{ .id = id, .kind = MoveKind::Transform };
			} else if ((node->shapeType == ShapeType::Rectangle
					|| node->shapeType == ShapeType::Ellipse
					|| node->shapeType == ShapeType::Star)
				&& document.json(id).get("p").isObject()) {
				return Movable{ .id = id, .kind = MoveKind::ShapePosition };
			}
			id = node->parent;
			continue;
		case NodeKind::Composition:
		case NodeKind::Asset:
			return std::nullopt;
		}
		return std::nullopt;
	}
	return std::nullopt;
}

// Maps the space the moved position lives in to the canvas.
[[nodiscard]] QTransform ParentSpace(
		const Document &document,
		const Movable &target,
		double rootFrame) {
	const auto node = document.node(target.id);
	if (!node) {
		return QTransform();
	} else if (target.kind == MoveKind::ShapePosition) {
		return document.transformAt(target.id, rootFrame);
	} else if (node->kind == NodeKind::Layer) {
		// The root composition maps to the identity, a precomp asset to
		// the precomp layer that shows it.
		return document.transformAt(
			node->parentLayer ? node->parentLayer : node->composition,
			rootFrame);
	}
	return document.transformAt(node->parent, rootFrame);
}

[[nodiscard]] QPointF PositionAt(
		const Document &document,
		const Movable &target,
		double localFrame) {
	if (target.kind == MoveKind::ShapePosition) {
		const auto value = document.valueAt(
			PropertyRef{ target.id, QByteArray("p") },
			localFrame);
		return value ? value->point() : QPointF();
	}
	const auto ref = TransformProperty(
		document,
		target.id,
		TransformField::Position);
	if (!ref) {
		return QPointF();
	}
	const auto &json = document.propertyJson(*ref);
	if (json.get("s").toBool(false)
		&& (json.get("x").isObject() || json.get("y").isObject())) {
		const auto x = document.valueAt(
			PropertyRef{ ref->node, ref->path + ".x" },
			localFrame);
		const auto y = document.valueAt(
			PropertyRef{ ref->node, ref->path + ".y" },
			localFrame);
		return QPointF(x ? x->scalar() : 0., y ? y->scalar() : 0.);
	}
	const auto value = document.valueAt(*ref, localFrame);
	return value ? value->point() : QPointF();
}

[[nodiscard]] QPointF LinearMap(const QTransform &transform, QPointF delta) {
	return transform.map(delta) - transform.map(QPointF());
}

[[nodiscard]] double RoundPosition(double value) {
	return std::round(value * 100.) / 100.;
}

} // namespace

struct CanvasPanel::MoveTarget {
	Movable movable;
	QTransform parentInverse; // Canvas -> the position's space.
	QPointF start; // Position at the press.
	double localFrame = 0.;
};

struct CanvasPanel::Drag {
	enum class Type : uchar {
		None,
		Move,
		Pan,
	};
	Type type = Type::None;
	QPointF press; // Panel coordinates.
	QPointF startPan;
	bool started = false;
	bool applied = false;
	NodeId clickSelect = 0; // Selected on a click without a move.
	Document start;
	std::vector<MoveTarget> targets;
	QPointF delta; // Last applied, canvas pixels.
	QByteArray mergeKey;
};

CanvasPanel::CanvasPanel(
	QWidget *parent,
	not_null<EditorController*> controller)
: RpWidget(parent)
, _controller(controller)
, _renderer(std::make_unique<FrameRenderer>())
, _background(LastBackground()) {
	setMouseTracking(true);

	_controller->documentChanged(
	) | rpl::on_next([=](const DocumentChange &change) {
		if (change.source == ChangeSource::Load) {
			if (_drag) {
				_drag = nullptr;
			}
			_controller->setZoom(1.);
			_controller->setPan(QPointF());
		}
		invalidateGeometry();
		refreshScale();
		requestFrame();
		update();
	}, lifetime());

	rpl::merge(
		_controller->currentFrameChanged() | rpl::to_empty,
		_controller->selectionChanged() | rpl::to_empty
	) | rpl::on_next([=] {
		invalidateGeometry();
		requestFrame();
		update();
	}, lifetime());

	_controller->playingValue(
	) | rpl::skip(1) | rpl::on_next([=] {
		// Back to the full resolution after playback.
		requestFrame();
	}, lifetime());

	rpl::merge(
		_controller->zoomValue() | rpl::to_empty,
		_controller->panValue() | rpl::to_empty
	) | rpl::on_next([=] {
		refreshScale();
		requestFrame();
		update();
	}, lifetime());

	_background.changes(
	) | rpl::on_next([=](CanvasBackground background) {
		LastBackground() = background;
		update();
	}, lifetime());

	style::PaletteChanged(
	) | rpl::on_next([=] {
		_checker = QPixmap();
		update();
	}, lifetime());
}

CanvasPanel::~CanvasPanel() = default;

CanvasBackground CanvasPanel::background() const {
	return _background.current();
}

void CanvasPanel::setBackground(CanvasBackground background) {
	_background = background;
}

rpl::producer<CanvasBackground> CanvasPanel::backgroundValue() const {
	return _background.value();
}

double CanvasPanel::fitScale() const {
	const auto size = _controller->document().size();
	const auto margin = Scaled(kCanvasMargin);
	const auto available = QSize(
		width() - 2 * margin,
		height() - 2 * margin);
	if (size.isEmpty() || available.width() <= 0 || available.height() <= 0) {
		return 1.;
	}
	return std::min(
		available.width() / double(size.width()),
		available.height() / double(size.height()));
}

double CanvasPanel::scale() const {
	return fitScale() * _controller->zoom();
}

rpl::producer<double> CanvasPanel::scaleValue() const {
	return _scale.value();
}

void CanvasPanel::refreshScale() {
	_scale = scale();
}

QRectF CanvasPanel::canvasRect() const {
	return canvasRect(scale(), _controller->pan());
}

QRectF CanvasPanel::canvasRect(double scale, QPointF pan) const {
	const auto size = QSizeF(_controller->document().size());
	if (size.isEmpty() || width() <= 0 || height() <= 0) {
		return QRectF();
	}
	const auto center = QPointF(width() / 2., height() / 2.) + pan * scale;
	const auto w = size.width() * scale;
	const auto h = size.height() * scale;
	return QRectF(center.x() - w / 2., center.y() - h / 2., w, h);
}

QTransform CanvasPanel::viewTransform() const {
	const auto rect = canvasRect();
	const auto scale = this->scale();
	return QTransform(scale, 0., 0., scale, rect.x(), rect.y());
}

QPointF CanvasPanel::toCanvas(QPointF point) const {
	const auto rect = canvasRect();
	const auto scale = this->scale();
	return (scale > 0.)
		? QPointF(
			(point.x() - rect.x()) / scale,
			(point.y() - rect.y()) / scale)
		: QPointF();
}

QPointF CanvasPanel::clampPan(QPointF pan, double scale) const {
	const auto size = QSizeF(_controller->document().size());
	if (scale <= 0. || size.isEmpty()) {
		return QPointF();
	}
	const auto keep = Scaled(kKeepVisible) / scale;
	const auto limit = [&](double side, int panel) {
		return std::max(side / 2. + panel / (2. * scale) - keep, 0.);
	};
	const auto x = limit(size.width(), width());
	const auto y = limit(size.height(), height());
	return QPointF(
		std::clamp(pan.x(), -x, x),
		std::clamp(pan.y(), -y, y));
}

void CanvasPanel::setView(double zoom, QPointF pan) {
	zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
	_controller->setZoom(zoom);
	_controller->setPan(clampPan(pan, fitScale() * zoom));
}

void CanvasPanel::zoomToScale(double scale, std::optional<QPointF> anchor) {
	const auto fit = fitScale();
	if (fit <= 0. || scale <= 0.) {
		return;
	}
	const auto zoom = std::clamp(scale / fit, kMinZoom, kMaxZoom);
	const auto now = this->scale();
	const auto center = QPointF(width() / 2., height() / 2.);
	const auto point = anchor.value_or(center);
	const auto size = QSizeF(_controller->document().size());
	const auto canvas = toCanvas(point);
	const auto updated = fit * zoom;
	if (now <= 0. || updated <= 0.) {
		return;
	}
	// Keep `canvas` under `point`.
	const auto pan = (point - center) / updated
		- canvas
		+ QPointF(size.width() / 2., size.height() / 2.);
	setView(zoom, pan);
}

void CanvasPanel::zoomBy(double factor, std::optional<QPointF> anchor) {
	zoomToScale(scale() * factor, anchor);
}

void CanvasPanel::zoomToFit() {
	setView(1., QPointF());
}

bool CanvasPanel::frameReady() const {
	return !_frame.isNull()
		&& (_frameIndex == _controller->frameIndex())
		&& _frameDocument.sameAs(_controller->document());
}

void CanvasPanel::requestFrame() {
	const auto rect = canvasRect();
	if (rect.isEmpty()) {
		return;
	}
	const auto ratio = style::DevicePixelRatio();
	auto size = QSize(
		std::max(int(std::ceil(rect.width() * ratio)), 1),
		std::max(int(std::ceil(rect.height() * ratio)), 1));
	const auto limit = _controller->playing()
		? kPlaybackRenderSide
		: kMaxRenderSide;
	if (size.width() > limit || size.height() > limit) {
		size = size.scaled(limit, limit, Qt::KeepAspectRatio);
	}
	const auto &document = _controller->document();
	const auto frame = _controller->frameIndex();
	if (_requestedSize == size
		&& _requestedFrame == frame
		&& _requestedDocument.sameAs(document)) {
		return;
	}
	_requestedSize = size;
	_requestedFrame = frame;
	_requestedDocument = document;
	_renderer->request(document, frame, size, [=](QImage image) {
		_frame = std::move(image);
		_frameDocument = document;
		_frameIndex = frame;
		update();
	});
}

void CanvasPanel::invalidateGeometry() {
	_layerOutlinesValid = false;
	_overlayValid = false;
	_hoverPathValid = false;
}

void CanvasPanel::ensureGeometry() {
	const auto &document = _controller->document();
	const auto frame = _controller->currentFrame();
	if (!_geometryDocument.sameAs(document) || _geometryFrame != frame) {
		_geometryDocument = document;
		_geometryFrame = frame;
		invalidateGeometry();
	}
}

void CanvasPanel::ensureLayerOutlines() {
	ensureGeometry();
	if (_layerOutlinesValid) {
		return;
	}
	_layerOutlinesValid = true;
	_layerOutlines.clear();
	const auto &document = _controller->document();
	const auto frame = _controller->currentFrame();
	for (const auto id : document.layers()) {
		const auto node = document.node(id);
		if (!node || node->hidden || node->matteSource) {
			continue;
		} else if (frame < node->inPoint || frame >= node->outPoint) {
			continue;
		}
		auto outline = document.outlineAt(id, frame);
		if (!outline.isEmpty()) {
			_layerOutlines.emplace_back(id, std::move(outline));
		}
	}
}

void CanvasPanel::ensureOverlay() {
	ensureGeometry();
	if (_overlayValid) {
		return;
	}
	_overlayValid = true;
	_overlay.clear();
	const auto &document = _controller->document();
	const auto frame = _controller->currentFrame();
	const auto primary = _controller->primarySelection();
	for (const auto id : _controller->selection()) {
		if (int(_overlay.size()) >= kMaxOverlayItems) {
			break;
		}
		const auto geometry = GeometryNode(document, id);
		const auto node = document.node(geometry);
		if (!node) {
			continue;
		}
		auto item = OverlayItem{ .id = id, .primary = (id == primary) };
		item.outline = document.outlineAt(geometry, frame);
		const auto transform = document.transformAt(geometry, frame);
		if (!item.outline.isEmpty()) {
			auto invertible = false;
			const auto inverse = transform.inverted(&invertible);
			const auto local = invertible
				? inverse.map(item.outline).boundingRect()
				: QRectF();
			item.box = (invertible && !local.isEmpty())
				? transform.map(QPolygonF(local))
				: QPolygonF(item.outline.boundingRect());
		}
		const auto anchored = (node->kind == NodeKind::Layer)
			|| (node->kind == NodeKind::Shape
				&& node->shapeType == ShapeType::Group);
		if (anchored && item.primary) {
			if (const auto ref = TransformProperty(
					document,
					geometry,
					TransformField::Anchor)) {
				const auto value = document.valueAt(
					*ref,
					document.localFrame(geometry, frame));
				item.anchor = transform.map(
					value ? value->point() : QPointF());
			}
		}
		if (!item.outline.isEmpty() || item.anchor) {
			_overlay.push_back(std::move(item));
		}
	}
}

void CanvasPanel::ensureHoverPath() {
	ensureGeometry();
	if (_hoverPathValid) {
		return;
	}
	_hoverPathValid = true;
	_hoverPath = QPainterPath();
	if (_hovered && !_controller->isSelected(_hovered)) {
		const auto &document = _controller->document();
		_hoverPath = document.outlineAt(
			GeometryNode(document, _hovered),
			_controller->currentFrame());
	}
}

NodeId CanvasPanel::hitTest(QPointF point, HitDepth depth) {
	const auto rect = canvasRect();
	if (!rect.contains(point)) {
		return 0;
	}
	ensureLayerOutlines();
	const auto canvas = toCanvas(point);
	auto layer = NodeId();
	for (const auto &[id, outline] : _layerOutlines) {
		if (outline.contains(canvas)) {
			layer = id;
			break;
		}
	}
	if (!layer) {
		return 0;
	}
	const auto &document = _controller->document();
	const auto frame = _controller->currentFrame();
	const auto contains = [&](NodeId id) {
		return document.outlineAt(id, frame).contains(canvas);
	};
	// The topmost shape item of a layer / group under the point.
	const auto childAt = [&](NodeId container) -> NodeId {
		const auto node = document.node(container);
		if (!node || !HasShapeChildren(*node)) {
			return 0;
		}
		for (const auto child : node->children) {
			const auto item = document.node(child);
			if (item && !item->hidden && IsGeometry(item->shapeType)
				&& contains(child)) {
				return child;
			}
		}
		return 0;
	};
	if (depth == HitDepth::Deepest) {
		auto result = layer;
		for (auto guard = 0; guard != 64; ++guard) {
			const auto child = childAt(result);
			if (!child) {
				break;
			}
			result = child;
		}
		return result;
	}
	const auto primary = _controller->primarySelection();
	const auto primaryNode = document.node(primary);
	if (primaryNode
		&& primary != layer
		&& primaryNode->kind == NodeKind::Shape
		&& document.owningLayer(primary) == layer) {
		if (depth == HitDepth::Deeper && contains(primary)) {
			if (const auto child = childAt(primary)) {
				return child;
			}
		}
		// Stay at the depth of the selection: a sibling under the point.
		if (const auto sibling = childAt(primaryNode->parent)) {
			return sibling;
		}
	} else if (depth == HitDepth::Deeper && primary == layer) {
		if (const auto child = childAt(layer)) {
			return child;
		}
	}
	return layer;
}

bool CanvasPanel::selectionContains(QPointF point) {
	if (_controller->selection().empty()) {
		return false;
	}
	ensureOverlay();
	const auto canvas = toCanvas(point);
	for (const auto &item : _overlay) {
		if (item.box.containsPoint(canvas, Qt::OddEvenFill)
			|| item.outline.contains(canvas)) {
			return true;
		}
	}
	return false;
}

auto CanvasPanel::collectTargets(const Document &document) const
-> std::vector<MoveTarget> {
	const auto frame = _controller->currentFrame();
	auto movables = std::vector<Movable>();
	for (const auto id : _controller->selection()) {
		const auto movable = ResolveMovable(document, id);
		if (movable && !ranges::contains(
				movables,
				movable->id,
				&Movable::id)) {
			movables.push_back(*movable);
		}
	}
	// Moving a layer / group moves everything inside it and its children.
	auto result = std::vector<MoveTarget>();
	for (const auto &movable : movables) {
		const auto nested = ranges::any_of(movables, [&](const Movable &other) {
			return (other.id != movable.id)
				&& document.isDescendant(movable.id, other.id);
		});
		if (nested) {
			continue;
		}
		auto invertible = false;
		const auto inverse = ParentSpace(document, movable, frame).inverted(
			&invertible);
		if (!invertible) {
			continue;
		}
		const auto local = document.localFrame(movable.id, frame);
		result.push_back({
			.movable = movable,
			.parentInverse = inverse,
			.start = PositionAt(document, movable, local),
			.localFrame = local,
		});
	}
	return result;
}

Edit CanvasPanel::MoveEdit(
		const Document &start,
		const std::vector<MoveTarget> &targets,
		QPointF delta) {
	// Always from the document at the press: moving back to the start
	// restores it exactly and passing over values leaves no trace.
	auto result = Edit{ .document = start };
	for (const auto &target : targets) {
		const auto shift = LinearMap(target.parentInverse, delta);
		const auto position = QPointF(
			RoundPosition(target.start.x() + shift.x()),
			RoundPosition(target.start.y() + shift.y()));
		auto step = (target.movable.kind == MoveKind::Transform)
			? SetTransformAt(
				result.document,
				target.movable.id,
				TransformField::Position,
				PropValue::Point(position),
				target.localFrame)
			: SetValueAt(
				result.document,
				PropertyRef{ target.movable.id, QByteArray("p") },
				PropValue::Point(position),
				target.localFrame);
		if (!step) {
			continue;
		}
		result.document = std::move(step.document);
		result.changed.insert(
			end(result.changed),
			begin(step.changed),
			end(step.changed));
	}
	return result;
}

void CanvasPanel::startMove() {
	_controller->setPlaying(false);
	_drag->start = _controller->document();
	_drag->targets = collectTargets(_drag->start);
	_drag->mergeKey = "canvas-move-" + QByteArray::number(++_gestures);
	_controller->beginGesture(_drag->mergeKey);
}

void CanvasPanel::applyMove(QPointF delta) {
	if (!_drag || _drag->targets.empty()) {
		return;
	}
	_drag->delta = delta;
	if (_controller->perform(
			Command::SetValue,
			MoveEdit(_drag->start, _drag->targets, delta),
			_drag->mergeKey)) {
		_drag->applied = true;
	}
}

bool CanvasPanel::nudgeSelection(QPointF delta) {
	if (_drag) {
		return false;
	}
	const auto document = _controller->document();
	const auto targets = collectTargets(document);
	if (targets.empty()) {
		return false;
	}
	_controller->setPlaying(false);
	return _controller->perform(
		Command::SetValue,
		MoveEdit(document, targets, delta),
		QByteArray("canvas-nudge"));
}

void CanvasPanel::finishDrag(bool cancel) {
	if (!_drag) {
		return;
	}
	const auto drag = std::move(_drag);
	if (drag->type == Drag::Type::Move && drag->applied) {
		if (cancel) {
			_controller->cancelGesture(drag->mergeKey);
		} else {
			_controller->finishMerge();
		}
	} else if (drag->type == Drag::Type::Pan && cancel) {
		_controller->setPan(drag->startPan);
	}
}

void CanvasPanel::setHovered(NodeId id) {
	if (_hovered == id) {
		return;
	}
	_hovered = id;
	_hoverPathValid = false;
	update();
}

void CanvasPanel::updateCursor(QPointF point) {
	if (_drag && _drag->type == Drag::Type::Pan && _drag->started) {
		setCursor(Qt::ClosedHandCursor);
	} else if (_drag && _drag->type == Drag::Type::Move && _drag->started) {
		setCursor(Qt::SizeAllCursor);
	} else if (!_drag && selectionContains(point)) {
		setCursor(Qt::SizeAllCursor);
	} else {
		setCursor(style::cur_default);
	}
}

void CanvasPanel::resizeEvent(QResizeEvent *e) {
	// Keep the pan in range for the new size.
	const auto pan = clampPan(_controller->pan(), scale());
	if (pan != _controller->pan()) {
		_controller->setPan(pan);
	}
	refreshScale();
	requestFrame();
}

void CanvasPanel::mousePressEvent(QMouseEvent *e) {
	_menu = nullptr;
	if (_drag) {
		return;
	}
	const auto point = e->position();
	if (e->button() == Qt::MiddleButton) {
		_drag = std::make_unique<Drag>();
		_drag->type = Drag::Type::Pan;
		_drag->press = point;
		_drag->startPan = _controller->pan();
		return;
	} else if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto modifiers = e->modifiers();
	const auto toggle = (modifiers & Qt::ControlModifier) != 0;
	const auto add = (modifiers & Qt::ShiftModifier) != 0;
	const auto deepest = (modifiers & Qt::AltModifier) != 0;
	const auto hit = hitTest(
		point,
		deepest ? HitDepth::Deepest : HitDepth::Keep);

	_drag = std::make_unique<Drag>();
	_drag->press = point;
	if (!toggle && !deepest && selectionContains(point)) {
		_drag->type = Drag::Type::Move;
		if (hit && !_controller->isSelected(hit)) {
			if (add) {
				_controller->select(hit, SelectMode::Add);
			} else {
				_drag->clickSelect = hit;
			}
		}
	} else if (hit) {
		if (toggle) {
			_controller->select(hit, SelectMode::Toggle);
		} else {
			_controller->select(hit, add ? SelectMode::Add : SelectMode::Replace);
			_drag->type = Drag::Type::Move;
		}
	} else {
		if (!toggle && !add) {
			_controller->clearSelection();
		}
		_drag->type = Drag::Type::Pan;
		_drag->startPan = _controller->pan();
	}
	setHovered(0);
}

void CanvasPanel::mouseMoveEvent(QMouseEvent *e) {
	const auto point = e->position();
	const auto rect = canvasRect();
	const auto cursor = rect.contains(point)
		? std::make_optional(toCanvas(point))
		: std::nullopt;
	if (_cursor != cursor) {
		_cursor = cursor;
		update();
	}
	if (!_drag) {
		setHovered(hitTest(point, (e->modifiers() & Qt::AltModifier)
			? HitDepth::Deepest
			: HitDepth::Keep));
		updateCursor(point);
		return;
	}
	if (!_drag->started) {
		const auto moved = (point - _drag->press).manhattanLength();
		if (moved < Scaled(kDragThreshold)) {
			return;
		}
		_drag->started = true;
		if (_drag->type == Drag::Type::Move) {
			startMove();
		}
		updateCursor(point);
	}
	const auto scale = this->scale();
	if (scale <= 0.) {
		return;
	}
	switch (_drag->type) {
	case Drag::Type::Move: {
		auto delta = (point - _drag->press) / scale;
		if (e->modifiers() & Qt::ShiftModifier) {
			if (std::abs(delta.x()) >= std::abs(delta.y())) {
				delta.setY(0.);
			} else {
				delta.setX(0.);
			}
		}
		if (delta != _drag->delta) {
			applyMove(delta);
		}
	} break;
	case Drag::Type::Pan:
		_controller->setPan(clampPan(
			_drag->startPan + (point - _drag->press) / scale,
			scale));
		break;
	case Drag::Type::None:
		break;
	}
}

void CanvasPanel::mouseReleaseEvent(QMouseEvent *e) {
	if (!_drag) {
		return;
	} else if (e->button() != Qt::LeftButton
		&& e->button() != Qt::MiddleButton) {
		return;
	}
	const auto clickSelect = (!_drag->started) ? _drag->clickSelect : 0;
	finishDrag(false);
	if (clickSelect) {
		_controller->select(clickSelect);
	}
	updateCursor(e->position());
}

void CanvasPanel::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	finishDrag(false);
	if (const auto deeper = hitTest(e->position(), HitDepth::Deeper)) {
		_controller->select(deeper);
	}
}

void CanvasPanel::wheelEvent(QWheelEvent *e) {
	const auto pixel = e->pixelDelta();
	const auto angle = e->angleDelta();
	if (e->modifiers() & Qt::ControlModifier) {
		const auto steps = angle.y()
			? (angle.y() / 120.)
			: (pixel.y() / 50.);
		if (steps != 0.) {
			zoomBy(std::pow(kWheelZoomStep, steps), e->position());
		}
	} else {
		auto delta = !pixel.isNull()
			? QPointF(pixel)
			: (QPointF(angle) / 120. * Scaled(40));
		if ((e->modifiers() & Qt::ShiftModifier) && !delta.x()) {
			delta = QPointF(delta.y(), 0.);
		}
		const auto scale = this->scale();
		if (scale > 0.) {
			_controller->setPan(clampPan(
				_controller->pan() + delta / scale,
				scale));
		}
	}
	e->accept();
}

bool CanvasPanel::eventHook(QEvent *e) {
	if (e->type() == QEvent::NativeGesture) {
		const auto gesture = static_cast<QNativeGestureEvent*>(e);
		switch (gesture->gestureType()) {
		case Qt::ZoomNativeGesture:
			zoomBy(1. + gesture->value(), gesture->position());
			return true;
		case Qt::SmartZoomNativeGesture:
			if (std::abs(_controller->zoom() - 1.) < 0.01) {
				zoomToScale(1., gesture->position());
			} else {
				zoomToFit();
			}
			return true;
		default:
			break;
		}
	}
	return RpWidget::eventHook(e);
}

void CanvasPanel::keyPressEvent(QKeyEvent *e) {
	if (_drag && e->key() == Qt::Key_Escape) {
		finishDrag(true);
		updateCursor(mapFromGlobal(QCursor::pos()));
		return;
	}
	RpWidget::keyPressEvent(e);
}

void CanvasPanel::leaveEventHook(QEvent *e) {
	setHovered(0);
	if (_cursor) {
		_cursor = std::nullopt;
		update();
	}
	RpWidget::leaveEventHook(e);
}

void CanvasPanel::contextMenuEvent(QContextMenuEvent *e) {
	if (_drag) {
		return;
	}
	const auto hit = hitTest(e->pos(), HitDepth::Keep);
	if (hit && !_controller->isSelected(hit)) {
		_controller->select(hit);
	} else if (!hit && !selectionContains(e->pos())) {
		_controller->clearSelection();
	}
	showMenu(e->globalPos());
	e->accept();
}

void CanvasPanel::showMenu(QPoint globalPosition) {
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	const auto &document = _controller->document();
	const auto selection = _controller->selection();
	if (!selection.empty()) {
		const auto primary = _controller->primarySelection();
		const auto layer = document.owningLayer(primary);
		if (layer && layer != primary) {
			_menu->addAction(
				tr::lng_oblivion_lottie_canvas_select_layer(tr::now),
				[=] { _controller->select(layer); },
				&st::menuIconSelect);
		}
		_menu->addAction(
			tr::lng_oblivion_lottie_canvas_duplicate(tr::now),
			[=] { _controller->duplicateSelection(); },
			&st::menuIconCopy);
		const auto hidden = ranges::all_of(selection, [&](NodeId id) {
			const auto node = document.node(id);
			return node && node->hidden;
		});
		_menu->addAction(
			(hidden
				? tr::lng_oblivion_lottie_canvas_show(tr::now)
				: tr::lng_oblivion_lottie_canvas_hide(tr::now)),
			[=] { _controller->setHidden(selection, !hidden); },
			hidden ? &st::menuIconUserShow : &st::menuIconStealth);
		_menu->addAction(
			tr::lng_oblivion_lottie_canvas_delete(tr::now),
			[=] { _controller->deleteNodes(selection); },
			&st::menuIconDelete);
		_menu->addSeparator();
	}
	_menu->addAction(
		tr::lng_oblivion_lottie_canvas_zoom_fit(tr::now),
		[=] { zoomToFit(); },
		&st::menuIconExpand);
	_menu->addAction(
		tr::lng_oblivion_lottie_canvas_zoom_actual(tr::now),
		[=] { zoomToScale(1.); },
		&st::menuIconPhoto);
	_menu->popup(globalPosition);
}

void CanvasPanel::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto clip = e->rect();
	p.fillRect(clip, st::windowBgOver);

	const auto rect = canvasRect();
	if (rect.isEmpty()) {
		return;
	}
	paintBackground(p, rect);
	if (!_frame.isNull()) {
		auto hq = PainterHighQualityEnabler(p);
		p.drawImage(rect, _frame);
	}
	p.setPen(st::shadowFg);
	p.setBrush(Qt::NoBrush);
	p.drawRect(rect.adjusted(-0.5, -0.5, 0.5, 0.5));

	paintOverlay(p);
	paintInfo(p);
}

void CanvasPanel::paintBackground(QPainter &p, const QRectF &rect) {
	switch (_background.current()) {
	case CanvasBackground::Dark:
		p.fillRect(rect, kDarkBackdrop);
		return;
	case CanvasBackground::Light:
		p.fillRect(rect, kLightBackdrop);
		return;
	case CanvasBackground::Checker:
		break;
	}
	const auto ratio = style::DevicePixelRatio();
	const auto cell = Scaled(kCheckerCell);
	if (_checker.isNull()) {
		auto image = QImage(
			QSize(cell * 2, cell * 2) * ratio,
			QImage::Format_ARGB32_Premultiplied);
		image.setDevicePixelRatio(ratio);
		image.fill(st::windowBg->c);
		{
			auto q = QPainter(&image);
			q.fillRect(0, 0, cell, cell, st::windowBgRipple);
			q.fillRect(cell, cell, cell, cell, st::windowBgRipple);
		}
		_checker = QPixmap::fromImage(std::move(image));
	}
	const auto aligned = rect.toAlignedRect().intersected(this->rect());
	if (aligned.isEmpty()) {
		return;
	}
	p.save();
	p.setClipRect(rect);
	const auto origin = rect.topLeft().toPoint();
	const auto offset = QPoint(
		((aligned.x() - origin.x()) % (cell * 2) + cell * 2) % (cell * 2),
		((aligned.y() - origin.y()) % (cell * 2) + cell * 2) % (cell * 2));
	p.drawTiledPixmap(aligned, _checker, offset);
	p.restore();
}

void CanvasPanel::paintOverlay(QPainter &p) {
	ensureOverlay();
	ensureHoverPath();
	if (_overlay.empty() && _hoverPath.isEmpty()) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto view = viewTransform();
	const auto accent = st::windowActiveTextFg->c;
	p.setBrush(Qt::NoBrush);
	if (!_hoverPath.isEmpty() && (!_drag || !_drag->started)) {
		auto pen = QPen(anim::with_alpha(accent, 0.7));
		pen.setWidthF(Scaled(1) * 1.);
		pen.setCosmetic(true);
		p.setPen(pen);
		p.drawPath(view.map(_hoverPath));
	}
	for (const auto &item : _overlay) {
		if (item.primary && !item.outline.isEmpty()) {
			auto pen = QPen(anim::with_alpha(accent, 0.45));
			pen.setWidthF(1.);
			pen.setCosmetic(true);
			p.setPen(pen);
			p.drawPath(view.map(item.outline));
		}
		if (!item.box.isEmpty()) {
			auto pen = QPen(accent);
			pen.setWidthF(Scaled(1) * 1.5);
			pen.setCosmetic(true);
			pen.setJoinStyle(Qt::MiterJoin);
			if (!item.primary) {
				pen.setDashPattern({ 3., 2. });
			}
			p.setPen(pen);
			p.drawPolygon(view.map(item.box));
			if (item.primary) {
				// Corner marks.
				const auto mapped = view.map(item.box);
				const auto half = Scaled(3);
				p.setBrush(kHandleFill);
				auto corner = QPen(accent);
				corner.setWidthF(Scaled(1) * 1.);
				p.setPen(corner);
				for (auto i = 0; i != std::min(int(mapped.size()), 4); ++i) {
					p.drawRect(QRectF(
						mapped[i].x() - half,
						mapped[i].y() - half,
						half * 2.,
						half * 2.));
				}
				p.setBrush(Qt::NoBrush);
			}
		}
		if (item.anchor) {
			const auto center = view.map(*item.anchor);
			const auto radius = Scaled(kAnchorRadius) * 1.;
			auto pen = QPen(accent);
			pen.setWidthF(Scaled(1) * 1.5);
			p.setPen(pen);
			p.setBrush(anim::with_alpha(kHandleFill, 0.8));
			p.drawEllipse(center, radius, radius);
			p.drawLine(
				center - QPointF(radius * 2., 0.),
				center - QPointF(radius, 0.));
			p.drawLine(
				center + QPointF(radius, 0.),
				center + QPointF(radius * 2., 0.));
			p.drawLine(
				center - QPointF(0., radius * 2.),
				center - QPointF(0., radius));
			p.drawLine(
				center + QPointF(0., radius),
				center + QPointF(0., radius * 2.));
			p.setBrush(Qt::NoBrush);
		}
	}
}

void CanvasPanel::paintInfo(QPainter &p) {
	if (!_cursor) {
		return;
	}
	const auto text = FormatDecimal(_cursor->x(), 0)
		+ QString::fromUtf8(", ")
		+ FormatDecimal(_cursor->y(), 0);
	const auto &font = st::normalFont;
	const auto padding = QMargins(Scaled(8), Scaled(3), Scaled(8), Scaled(3));
	const auto size = QSize(
		font->width(text) + padding.left() + padding.right(),
		font->height + padding.top() + padding.bottom());
	const auto skip = Scaled(8);
	const auto rect = QRect(
		QPoint(skip, height() - skip - size.height()),
		size);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(anim::with_alpha(st::windowBg->c, 0.9));
	p.drawRoundedRect(rect, size.height() / 2., size.height() / 2.);
	p.setFont(font);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		rect.x() + padding.left(),
		rect.y() + padding.top() + font->ascent,
		text);
}

} // namespace Oblivion::LottieEdit
