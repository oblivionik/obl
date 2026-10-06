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
#include "oblivion/oblivion_lottie_editor_palette.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/effects/animation_value.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QNativeGestureEvent>
#include <QtGui/QPainterPathStroker>
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

// Mask outlines are a part of the same overlay: one color in both themes
// that is far from the accent of the selection.
constexpr auto kMaskOutline = QColor(0xff, 0x8a, 0x1f);
constexpr auto kOutlineUnder = QColor(0xff, 0xff, 0xff, 0xa0);

constexpr auto kPenRadius = 7; // How close the cursor has to be.
constexpr auto kPenVertex = 4; // Half size of a point mark.
constexpr auto kPenHandle = 3;
constexpr auto kMaxPenCandidates = 48;

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
		PenVertex, // A point of the path follows the cursor.
		PenTangent, // One of its handles does.
		PenPull, // New mirrored handles are pulled out of a point.
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

	// Pen: the path at the press, every move is computed from it.
	PropertyRef penRef;
	PathData penPath;
	QTransform penFromView; // Panel -> path coordinates.
	double penFrame = 0.;
	int penVertex = -1;
	bool penOut = false;
	TangentMode penMode = TangentMode::Free;
	bool penToggleOnClick = false; // Alt+click: corner <-> smooth.

	[[nodiscard]] bool pen() const {
		return (type == Type::PenVertex)
			|| (type == Type::PenTangent)
			|| (type == Type::PenPull);
	}
};

CanvasPanel::CanvasPanel(
	QWidget *parent,
	not_null<EditorController*> controller)
: RpWidget(parent)
, _controller(controller)
, _renderer(std::make_unique<FrameRenderer>())
, _background(LastBackground())
, _tool(CurrentTool(controller)) {
	setMouseTracking(true);

	_controller->documentChanged(
	) | rpl::on_next([=](const DocumentChange &change) {
		if (change.source == ChangeSource::Load) {
			if (_drag) {
				_drag = nullptr;
			}
			resetPen();
			_controller->setZoom(1.);
			_controller->setPan(QPointF());
		}
		invalidateGeometry();
		refreshScale();
		requestFrame();
		update();
	}, lifetime());

	_controller->selectionChanged(
	) | rpl::on_next([=] {
		resetPen();
	}, lifetime());

	CurrentToolValue(
		_controller
	) | rpl::on_next([=](CanvasTool tool) {
		if (_tool == tool) {
			return;
		}
		finishDrag(true);
		_tool = tool;
		resetPen();
		setHovered(0);
		updateCursor(mapFromGlobal(QCursor::pos()));
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
	_penValid = false;
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
	if ((drag->type == Drag::Type::Move || drag->pen()) && drag->applied) {
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
	} else if (_tool == CanvasTool::Pen) {
		const auto grabs = (_drag && _drag->pen())
			|| (_penOver.part == PathPart::Vertex)
			|| (_penOver.part == PathPart::InTangent)
			|| (_penOver.part == PathPart::OutTangent);
		setCursor(grabs ? Qt::SizeAllCursor : Qt::CrossCursor);
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
	if (_tool == CanvasTool::Pen) {
		penPress(point, modifiers);
		updateCursor(point);
		return;
	}
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
		if (_tool == CanvasTool::Pen) {
			penHover(point);
			return;
		}
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
		} else if (_drag->pen()) {
			_controller->setPlaying(false);
			_drag->start = _controller->document();
			_drag->mergeKey = "canvas-pen-" + QByteArray::number(++_gestures);
			_controller->beginGesture(_drag->mergeKey);
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
	case Drag::Type::PenVertex:
	case Drag::Type::PenTangent:
	case Drag::Type::PenPull:
		applyPenDrag(point, e->modifiers());
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
	const auto toggleVertexIndex = (!_drag->started && _drag->penToggleOnClick)
		? _drag->penVertex
		: -1;
	finishDrag(false);
	if (clickSelect) {
		_controller->select(clickSelect);
	}
	if (toggleVertexIndex >= 0) {
		toggleVertex(toggleVertexIndex);
	}
	if (_tool == CanvasTool::Pen) {
		penHover(e->position());
	} else {
		updateCursor(e->position());
	}
}

void CanvasPanel::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	finishDrag(false);
	if (_tool == CanvasTool::Pen) {
		penDoubleClick(e->position());
		return;
	}
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
	} else if (_tool == CanvasTool::Pen && penKey(e)) {
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
	ensurePenGeometry();
	if (_tool != CanvasTool::Pen || !_pen.target.valid()) {
		// The pen keeps the path it works on, the menu is about its points.
		const auto hit = hitTest(e->pos(), HitDepth::Keep);
		if (hit && !_controller->isSelected(hit)) {
			_controller->select(hit);
		} else if (!hit && !selectionContains(e->pos())) {
			_controller->clearSelection();
		}
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
	addPenMenuItems(QPointF(mapFromGlobal(globalPosition)));
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
	paintMasks(p);
	paintPen(p);
	paintInfo(p);
	paintHint(p);
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
	ensurePenGeometry();
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
		if (_tool == CanvasTool::Pen
			&& _pen.target.valid()
			&& item.id == _pen.target.node) {
			// The pen draws the path itself with its points, a box around
			// it would only be in the way.
			continue;
		}
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

// The pen tool.

CanvasTool CanvasPanel::tool() const {
	return _tool;
}

void CanvasPanel::setTool(CanvasTool tool) {
	SetCurrentTool(_controller, tool);
}

int CanvasPanel::selectedPathVertex() const {
	return _penVertex;
}

void CanvasPanel::selectPathVertex(int index) {
	ensurePenGeometry();
	const auto count = int(_pen.target.path.vertices.size());
	_penVertex = (_pen.target.valid() && index >= 0 && index < count)
		? index
		: -1;
	update();
}

bool CanvasPanel::convertSelectionToPath() {
	const auto id = _controller->primarySelection();
	const auto &document = _controller->document();
	if (!CanConvertToPath(document, id)) {
		return false;
	}
	auto edit = ConvertToPath(document, id, _controller->localFrame(id));
	auto created = edit.created;
	_controller->setPlaying(false);
	if (!_controller->perform(Command::EditPath, std::move(edit))) {
		return false;
	}
	if (!created.empty()) {
		_controller->setSelection(std::move(created));
	}
	return true;
}

void CanvasPanel::resetPen() {
	_penVertex = -1;
	_penPointRemoved = false;
	_penOver = PathPick();
	_penPending = std::nullopt;
	_penPendingContainer = 0;
	_penValid = false;
	update();
}

void CanvasPanel::ensurePenGeometry() {
	ensureGeometry();
	if (_penValid) {
		return;
	}
	_penValid = true;
	_pen = PenGeometry();
	const auto &document = _controller->document();
	const auto frame = _controller->currentFrame();
	const auto pathOf = [&](NodeId id) -> std::optional<PathData> {
		const auto ref = PathPropertyOf(document, id);
		if (!ref) {
			return std::nullopt;
		}
		const auto value = document.valueAt(
			ref,
			document.localFrame(id, frame));
		return (value && value->path)
			? std::make_optional(NormalizedPath(*value->path))
			: std::nullopt;
	};
	const auto outline = [&](NodeId id) {
		const auto path = pathOf(id);
		return path
			? document.transformAt(id, frame).map(PainterPath(*path))
			: QPainterPath();
	};

	auto layers = std::vector<NodeId>();
	for (const auto id : _controller->selection()) {
		const auto node = document.node(id);
		const auto layer = !node
			? NodeId(0)
			: (node->kind == NodeKind::Layer)
			? id
			: (node->kind == NodeKind::Mask)
			? node->layer
			: NodeId(0);
		if (layer && !ranges::contains(layers, layer)) {
			layers.push_back(layer);
		}
	}
	for (const auto layer : layers) {
		const auto node = document.node(layer);
		if (!node) {
			continue;
		}
		for (const auto mask : node->masks) {
			if (int(_pen.masks.size()) >= kMaxPenCandidates) {
				break;
			}
			auto path = outline(mask);
			if (!path.isEmpty()) {
				_pen.masks.emplace_back(mask, std::move(path));
			}
		}
	}
	if (_tool != CanvasTool::Pen) {
		return;
	}
	const auto primary = _controller->primarySelection();
	if (auto path = pathOf(primary)) {
		_pen.target.node = primary;
		_pen.target.ref = PathPropertyOf(document, primary);
		_pen.target.path = std::move(*path);
		_pen.target.toCanvas = document.transformAt(primary, frame);
		_pen.target.localFrame = document.localFrame(primary, frame);
	}
	for (const auto id : PathCandidates(
			document,
			primary,
			kMaxPenCandidates)) {
		if (id == _pen.target.node) {
			continue;
		}
		auto path = outline(id);
		if (!path.isEmpty()) {
			_pen.candidates.emplace_back(id, std::move(path));
		}
	}
	if (_penVertex >= int(_pen.target.path.vertices.size())) {
		_penVertex = -1;
	}
}

double CanvasPanel::pickRadius() const {
	return Scaled(kPenRadius);
}

QTransform CanvasPanel::penToView() const {
	return _pen.target.toCanvas * viewTransform();
}

PathPick CanvasPanel::penPick(QPointF point) {
	ensurePenGeometry();
	if (!_pen.target.valid()) {
		return PathPick();
	}
	return PickPathPart(
		_pen.target.path,
		penToView(),
		point,
		pickRadius(),
		_penVertex);
}

NodeId CanvasPanel::pickCandidate(QPointF point) {
	ensurePenGeometry();
	const auto scale = this->scale();
	if (scale <= 0.) {
		return 0;
	}
	const auto canvas = toCanvas(point);
	auto stroker = QPainterPathStroker();
	stroker.setWidth(2. * pickRadius() / scale);
	for (const auto list : { &_pen.candidates, &_pen.masks }) {
		for (const auto &[id, outline] : *list) {
			if (id != _pen.target.node
				&& stroker.createStroke(outline).contains(canvas)) {
				return id;
			}
		}
	}
	return 0;
}

bool CanvasPanel::performPen(Edit &&edit) {
	_controller->setPlaying(false);
	return _controller->perform(Command::EditPath, std::move(edit));
}

void CanvasPanel::toggleVertex(int index) {
	ensurePenGeometry();
	const auto &target = _pen.target;
	if (!target.valid()
		|| index < 0
		|| index >= int(target.path.vertices.size())) {
		return;
	}
	performPen(SetPathAt(
		_controller->document(),
		target.ref,
		WithToggledVertex(target.path, index),
		target.localFrame));
}

void CanvasPanel::removeVertex(int index) {
	ensurePenGeometry();
	const auto target = _pen.target;
	if (!target.valid()
		|| index < 0
		|| index >= int(target.path.vertices.size())) {
		return;
	} else if (target.path.vertices.size() < 3) {
		if (const auto show = _controller->uiShow()) {
			show->showToast(
				tr::lng_oblivion_lottie_mask_pen_min_points(tr::now));
		}
		return;
	}
	if (performPen(RemovePathVertex(
			_controller->document(),
			target.ref,
			index))) {
		_penVertex = -1;
		_penPointRemoved = true;
		update();
	}
}

void CanvasPanel::startPenDrag(
		QPointF point,
		int vertex,
		PathPart part,
		TangentMode mode) {
	ensurePenGeometry();
	const auto &target = _pen.target;
	if (!target.valid()
		|| vertex < 0
		|| vertex >= int(target.path.vertices.size())) {
		return;
	}
	auto invertible = false;
	const auto inverse = penToView().inverted(&invertible);
	if (!invertible) {
		return;
	}
	_drag = std::make_unique<Drag>();
	_drag->type = (part != PathPart::Vertex)
		? Drag::Type::PenTangent
		: (mode == TangentMode::Mirrored)
		? Drag::Type::PenPull
		: Drag::Type::PenVertex;
	_drag->press = point;
	_drag->penRef = target.ref;
	_drag->penPath = target.path;
	_drag->penFromView = inverse;
	_drag->penFrame = target.localFrame;
	_drag->penVertex = vertex;
	_drag->penOut = (part == PathPart::OutTangent);
	_drag->penMode = mode;
}

void CanvasPanel::applyPenDrag(
		QPointF point,
		Qt::KeyboardModifiers modifiers) {
	if (!_drag || !_drag->pen()) {
		return;
	}
	const auto &start = _drag->penPath;
	const auto index = _drag->penVertex;
	if (index < 0 || index >= int(start.vertices.size())) {
		return;
	}
	if ((modifiers & Qt::ShiftModifier)
		&& _drag->type == Drag::Type::PenVertex) {
		const auto delta = point - _drag->press;
		if (std::abs(delta.x()) >= std::abs(delta.y())) {
			point.setY(_drag->press.y());
		} else {
			point.setX(_drag->press.x());
		}
	}
	const auto rounded = [](QPointF value) {
		return QPointF(RoundPosition(value.x()), RoundPosition(value.y()));
	};
	const auto local = _drag->penFromView.map(point);
	const auto shift = local - _drag->penFromView.map(_drag->press);
	auto path = PathData();
	switch (_drag->type) {
	case Drag::Type::PenVertex:
		path = WithMovedVertex(
			start,
			index,
			rounded(start.vertices[index] + shift));
		break;
	case Drag::Type::PenTangent: {
		const auto own = _drag->penOut
			? start.outTangents[index]
			: start.inTangents[index];
		path = WithMovedTangent(
			start,
			index,
			_drag->penOut,
			rounded(own + shift),
			(modifiers & Qt::AltModifier)
				? TangentMode::Free
				: _drag->penMode);
	} break;
	case Drag::Type::PenPull:
		path = WithMovedTangent(
			start,
			index,
			true,
			rounded(local - start.vertices[index]),
			TangentMode::Mirrored);
		break;
	default:
		return;
	}
	if (_controller->perform(
			Command::EditPath,
			SetPathAt(_drag->start, _drag->penRef, path, _drag->penFrame),
			_drag->mergeKey)) {
		_drag->applied = true;
	}
}

bool CanvasPanel::startNewPath(
		NodeId container,
		QPointF first,
		QPointF second) {
	const auto &document = _controller->document();
	const auto node = document.node(container);
	if (!node) {
		return false;
	}
	auto invertible = false;
	const auto inverse = document.transformAt(
		container,
		_controller->currentFrame()).inverted(&invertible);
	if (!invertible) {
		return false;
	}
	const auto local = [&](QPointF canvas) {
		const auto mapped = inverse.map(canvas);
		return QPointF(RoundPosition(mapped.x()), RoundPosition(mapped.y()));
	};
	auto path = PathData();
	path.vertices = { local(first), local(second) };
	path.inTangents = { QPointF(), QPointF() };
	path.outTangents = { QPointF(), QPointF() };
	const auto name = ShapeTypeText(ShapeType::Path);
	_controller->setPlaying(false);
	if (node->kind != NodeKind::Layer) {
		// A group: the path joins what the group already paints with.
		return _controller->addPath(container, path, name);
	}
	// A shape layer: a group of its own with a stroke, so that the new
	// path is seen at once.
	auto group = AddShape(
		document,
		container,
		ShapeTemplate::Group,
		ShapeTypeText(ShapeType::Group));
	if (!group) {
		return false;
	}
	auto groupId = NodeId(0);
	for (const auto id : group.created) {
		const auto created = group.document.node(id);
		if (created
			&& created->kind == NodeKind::Shape
			&& created->shapeType == ShapeType::Group) {
			groupId = id;
			break;
		}
	}
	if (!groupId) {
		return false;
	}
	auto added = AddPath(group.document, groupId, path, name);
	if (!added || added.created.empty()) {
		return false;
	}
	const auto pathId = added.created.front();
	auto stroke = AddShape(
		added.document,
		groupId,
		ShapeTemplate::Stroke,
		ShapeTypeText(ShapeType::Stroke));
	if (!stroke) {
		return false;
	}
	auto edit = Combined(
		Combined(std::move(group), std::move(added)),
		std::move(stroke));
	if (!_controller->perform(Command::AddPath, std::move(edit))) {
		return false;
	}
	_controller->setSelection({ pathId });
	return true;
}

void CanvasPanel::penPress(QPointF point, Qt::KeyboardModifiers modifiers) {
	_penPointRemoved = false;
	ensurePenGeometry();
	const auto alt = (modifiers & Qt::AltModifier) != 0;
	// A copy: the edits below rebuild the cached geometry.
	const auto target = _pen.target;
	if (target.valid()) {
		const auto count = int(target.path.vertices.size());
		const auto last = count - 1;
		const auto pick = penPick(point);
		switch (pick.part) {
		case PathPart::InTangent:
		case PathPart::OutTangent:
			startPenDrag(
				point,
				pick.index,
				pick.part,
				(!alt && IsSmoothVertex(target.path, pick.index))
					? TangentMode::Aligned
					: TangentMode::Free);
			return;
		case PathPart::Vertex: {
			const auto closes = !target.path.closed
				&& (count >= 3)
				&& ((_penVertex == last && pick.index == 0)
					|| (_penVertex == 0 && pick.index == last));
			if (closes && !alt) {
				performPen(SetPathClosed(
					_controller->document(),
					target.ref,
					true));
				_penVertex = pick.index;
				update();
				return;
			}
			_penVertex = pick.index;
			update();
			startPenDrag(
				point,
				pick.index,
				PathPart::Vertex,
				alt ? TangentMode::Mirrored : TangentMode::Free);
			if (_drag && alt) {
				_drag->penToggleOnClick = true;
			}
		} return;
		case PathPart::Segment:
			if (performPen(InsertPathVertex(
					_controller->document(),
					target.ref,
					pick.index,
					pick.t))) {
				_penVertex = pick.index + 1;
				update();
				startPenDrag(
					point,
					_penVertex,
					PathPart::Vertex,
					TangentMode::Free);
			}
			return;
		case PathPart::None:
			break;
		}
		const auto atEnd = (_penVertex == last);
		const auto atStart = !atEnd && (_penVertex == 0);
		if (!target.path.closed && _penVertex >= 0 && (atEnd || atStart)) {
			auto invertible = false;
			const auto inverse = penToView().inverted(&invertible);
			if (!invertible) {
				return;
			}
			const auto mapped = inverse.map(point);
			const auto position = QPointF(
				RoundPosition(mapped.x()),
				RoundPosition(mapped.y()));
			if (performPen(AppendPathVertex(
					_controller->document(),
					target.ref,
					position,
					atStart,
					target.localFrame))) {
				_penVertex = atStart ? 0 : count;
				update();
				// A drag right after the click pulls the handles of the
				// new point, a plain click leaves it a corner.
				startPenDrag(
					point,
					_penVertex,
					PathPart::Vertex,
					TangentMode::Mirrored);
			}
			return;
		} else if (_penVertex >= 0) {
			_penVertex = -1;
			update();
			return;
		}
	}
	if (const auto candidate = pickCandidate(point)) {
		_controller->select(candidate);
		return;
	}
	const auto &document = _controller->document();
	const auto container = target.valid()
		? NodeId(0)
		: NewPathContainerFor(document, _controller->primarySelection());
	if (container) {
		const auto canvas = toCanvas(point);
		if (!_penPending || _penPendingContainer != container) {
			_penPending = canvas;
			_penPendingContainer = container;
			update();
			return;
		}
		const auto first = *base::take(_penPending);
		_penPendingContainer = 0;
		if (startNewPath(container, first, canvas)) {
			// Its last point is selected: the next clicks continue it.
			_penValid = false;
			selectPathVertex(1);
		}
		update();
		return;
	}
	// Like a click of the selection tool: another layer to work on.
	const auto hit = hitTest(point, HitDepth::Keep);
	if (hit && !_controller->isSelected(hit)) {
		if (!target.valid()
			|| document.owningLayer(target.node) != document.owningLayer(hit)) {
			_controller->select(hit);
		}
	} else if (!hit && !target.valid()) {
		_controller->clearSelection();
	}
}

void CanvasPanel::penHover(QPointF point) {
	const auto pick = penPick(point);
	if (pick.part != _penOver.part
		|| pick.index != _penOver.index
		|| pick.part == PathPart::Segment) {
		_penOver = pick;
		update();
	}
	ensurePenGeometry();
	// Without a path to work on the layers answer to the cursor, a click
	// picks one of them.
	const auto picking = !_pen.target.valid()
		&& !_penPending
		&& !NewPathContainerFor(
			_controller->document(),
			_controller->primarySelection());
	setHovered(picking ? hitTest(point, HitDepth::Keep) : NodeId(0));
	updateCursor(point);
}

void CanvasPanel::penDoubleClick(QPointF point) {
	const auto pick = penPick(point);
	if (pick.part == PathPart::Vertex) {
		_penVertex = pick.index;
		toggleVertex(pick.index);
	} else if (!_pen.target.valid() && !_penPending) {
		if (const auto deeper = hitTest(point, HitDepth::Deeper)) {
			_controller->select(deeper);
		}
	}
}

bool CanvasPanel::penKey(not_null<QKeyEvent*> e) {
	const auto modifiers = e->modifiers()
		& (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
	if (modifiers) {
		return false;
	}
	switch (e->key()) {
	case Qt::Key_Escape:
	case Qt::Key_Return:
	case Qt::Key_Enter:
		if (_penPending || _penVertex >= 0) {
			_penPending = std::nullopt;
			_penPendingContainer = 0;
			_penVertex = -1;
			update();
			return true;
		}
		return false;
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		ensurePenGeometry();
		if (_penPending) {
			// The first point of a path that is not there yet goes, not
			// the layer the path was meant for.
			_penPending = std::nullopt;
			_penPendingContainer = 0;
			update();
			return true;
		} else if (!_pen.target.valid()) {
			return false;
		} else if (_penVertex >= 0) {
			if (!e->isAutoRepeat()
				&& QGuiApplication::mouseButtons() == Qt::NoButton) {
				removeVertex(_penVertex);
			}
			return true;
		}
		// Without a selected point Delete goes on to the path itself. Not
		// while the key is held and not right after it removed a point: one
		// press too many would take the whole path or mask. The next click
		// on the canvas lets Delete through again.
		return _penPointRemoved || e->isAutoRepeat();
	}
	return false;
}

void CanvasPanel::addPenMenuItems(QPointF point) {
	ensurePenGeometry();
	const auto &document = _controller->document();
	const auto primary = _controller->primarySelection();
	const auto before = _menu->actions().size();
	if (_tool == CanvasTool::Pen && _pen.target.valid()) {
		const auto ref = _pen.target.ref;
		const auto closed = _pen.target.path.closed;
		const auto pick = penPick(point);
		if (pick.part == PathPart::Vertex) {
			const auto index = pick.index;
			_penVertex = index;
			update();
			_menu->addAction(
				(IsCornerVertex(_pen.target.path, index)
					? tr::lng_oblivion_lottie_mask_pen_make_smooth(tr::now)
					: tr::lng_oblivion_lottie_mask_pen_make_corner(tr::now)),
				[=] { toggleVertex(index); },
				&st::menuIconEdit);
			_menu->addAction(
				tr::lng_oblivion_lottie_mask_pen_delete_point(tr::now),
				[=] { removeVertex(index); },
				&st::menuIconDelete);
		} else if (pick.part == PathPart::Segment) {
			const auto segment = pick.index;
			const auto t = pick.t;
			_menu->addAction(
				tr::lng_oblivion_lottie_mask_pen_add_point(tr::now),
				[=] {
					_controller->setPlaying(false);
					if (_controller->insertPathVertex(ref, segment, t)) {
						_penValid = false;
						selectPathVertex(segment + 1);
					}
				},
				&st::menuIconAdd);
		}
		_menu->addAction(
			(closed
				? tr::lng_oblivion_lottie_mask_pen_open(tr::now)
				: tr::lng_oblivion_lottie_mask_pen_close(tr::now)),
			[=] { _controller->setPathClosed(ref, !closed); },
			&st::menuIconLink);
		_menu->addAction(
			tr::lng_oblivion_lottie_mask_pen_reverse(tr::now),
			[=] { _controller->reversePath(ref); },
			&st::menuIconReschedule);
	} else if (_tool != CanvasTool::Pen
		&& !PathCandidates(document, primary, 1).empty()) {
		_menu->addAction(
			tr::lng_oblivion_lottie_mask_pen_edit(tr::now),
			[=] { setTool(CanvasTool::Pen); },
			&st::menuIconEdit);
	}
	if (CanConvertToPath(document, primary)) {
		_menu->addAction(
			tr::lng_oblivion_lottie_mask_pen_convert(tr::now),
			[=] { convertSelectionToPath(); },
			&st::menuIconEdit);
	}
	if (_tool == CanvasTool::Pen) {
		_menu->addAction(
			CanvasToolText(CanvasTool::Select),
			[=] { setTool(CanvasTool::Select); },
			&st::menuIconSelect);
	}
	if (_menu->actions().size() != before) {
		_menu->addSeparator();
	}
}

QString CanvasPanel::penHint() {
	if (_tool != CanvasTool::Pen) {
		return QString();
	}
	ensurePenGeometry();
	const auto &document = _controller->document();
	const auto primary = _controller->primarySelection();
	if (_pen.target.valid()) {
		const auto &path = _pen.target.path;
		const auto last = int(path.vertices.size()) - 1;
		return (!path.closed && (_penVertex == last || _penVertex == 0))
			? tr::lng_oblivion_lottie_mask_pen_hint_continue(tr::now)
			: tr::lng_oblivion_lottie_mask_pen_hint_edit(tr::now);
	} else if (_penPending) {
		return tr::lng_oblivion_lottie_mask_pen_hint_second(tr::now);
	} else if (CanConvertToPath(document, primary)) {
		return tr::lng_oblivion_lottie_mask_pen_hint_shape(tr::now);
	} else if (PathContainerFor(document, primary)) {
		return _pen.candidates.empty()
			? tr::lng_oblivion_lottie_mask_pen_hint_new(tr::now)
			: tr::lng_oblivion_lottie_mask_pen_hint_pick_or_new(tr::now);
	} else if (!_pen.candidates.empty()) {
		return tr::lng_oblivion_lottie_mask_pen_hint_pick(tr::now);
	}
	return tr::lng_oblivion_lottie_mask_pen_hint_select(tr::now);
}

void CanvasPanel::paintMasks(QPainter &p) {
	ensurePenGeometry();
	if (_pen.masks.empty()) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto view = viewTransform();
	p.setBrush(Qt::NoBrush);
	for (const auto &[id, outline] : _pen.masks) {
		if (_tool == CanvasTool::Pen && id == _pen.target.node) {
			continue;
		}
		const auto mapped = view.map(outline);
		// A light line under the dashes keeps them seen on any artwork.
		auto under = QPen(kOutlineUnder);
		under.setWidthF(Scaled(1) * 3.);
		p.setPen(under);
		p.drawPath(mapped);
		auto pen = QPen(kMaskOutline);
		pen.setWidthF(Scaled(1) * 1.5);
		pen.setDashPattern({ 4., 3. });
		p.setPen(pen);
		p.drawPath(mapped);
	}
}

void CanvasPanel::paintPen(QPainter &p) {
	if (_tool != CanvasTool::Pen) {
		return;
	}
	ensurePenGeometry();
	auto hq = PainterHighQualityEnabler(p);
	const auto view = viewTransform();
	const auto accent = st::windowActiveTextFg->c;
	const auto line = Scaled(1) * 1.;
	p.setBrush(Qt::NoBrush);

	// What a click can pick: the other paths and masks of the selection.
	for (const auto &[id, outline] : _pen.candidates) {
		const auto mapped = view.map(outline);
		auto under = QPen(kOutlineUnder);
		under.setWidthF(line * 2.5);
		p.setPen(under);
		p.drawPath(mapped);
		auto pen = QPen(anim::with_alpha(accent, 0.75));
		pen.setWidthF(line);
		pen.setDashPattern({ 3., 3. });
		p.setPen(pen);
		p.drawPath(mapped);
	}

	if (_penPending) {
		const auto first = view.map(*_penPending);
		if (_cursor) {
			auto pen = QPen(accent);
			pen.setWidthF(line * 1.5);
			pen.setDashPattern({ 3., 3. });
			p.setPen(pen);
			p.drawLine(first, view.map(*_cursor));
		}
		auto border = QPen(accent);
		border.setWidthF(line * 1.5);
		p.setPen(border);
		p.setBrush(kHandleFill);
		const auto half = Scaled(kPenVertex) * 1.;
		p.drawRect(QRectF(
			first.x() - half,
			first.y() - half,
			half * 2.,
			half * 2.));
		p.setBrush(Qt::NoBrush);
	}

	const auto &target = _pen.target;
	if (!target.valid()) {
		return;
	}
	const auto toView = penToView();
	const auto path = MappedPath(target.path, toView);
	const auto outline = PainterPath(path);
	auto under = QPen(kOutlineUnder);
	under.setWidthF(line * 3.5);
	p.setPen(under);
	p.drawPath(outline);
	auto stroke = QPen(accent);
	stroke.setWidthF(line * 1.5);
	p.setPen(stroke);
	p.drawPath(outline);

	const auto count = int(path.vertices.size());
	const auto dragged = (_drag && _drag->pen()) ? _drag->penVertex : -1;
	// Handles of the selected point.
	if (_penVertex >= 0 && _penVertex < count) {
		const auto vertex = path.vertices[_penVertex];
		const auto radius = Scaled(kPenHandle) * 1.;
		const auto paintHandle = [&](QPointF tangent, PathPart part) {
			if (std::hypot(tangent.x(), tangent.y()) < 0.5) {
				return;
			}
			const auto over = (_penOver.part == part)
				&& (_penOver.index == _penVertex);
			auto link = QPen(accent);
			link.setWidthF(line);
			p.setPen(link);
			p.drawLine(vertex, vertex + tangent);
			auto border = QPen(accent);
			border.setWidthF(line * 1.5);
			p.setPen(border);
			p.setBrush(over ? QBrush(accent) : QBrush(kHandleFill));
			p.drawEllipse(
				vertex + tangent,
				over ? (radius + 1.) : radius,
				over ? (radius + 1.) : radius);
			p.setBrush(Qt::NoBrush);
		};
		paintHandle(path.inTangents[_penVertex], PathPart::InTangent);
		paintHandle(path.outTangents[_penVertex], PathPart::OutTangent);
	}
	// Points: squares for corners, circles for smooth ones.
	for (auto i = 0; i != count; ++i) {
		const auto selected = (i == _penVertex);
		const auto over = (_penOver.part == PathPart::Vertex)
			&& (_penOver.index == i);
		const auto half = Scaled(kPenVertex)
			+ ((over || i == dragged) ? 1. : 0.);
		const auto center = path.vertices[i];
		auto border = QPen(accent);
		border.setWidthF(line * 1.5);
		p.setPen(border);
		p.setBrush(selected ? QBrush(accent) : QBrush(kHandleFill));
		if (IsCornerVertex(target.path, i)) {
			p.drawRect(QRectF(
				center.x() - half,
				center.y() - half,
				half * 2.,
				half * 2.));
		} else {
			p.drawEllipse(center, half * 1.1, half * 1.1);
		}
	}
	p.setBrush(Qt::NoBrush);
	// Where a click adds a point.
	if (_penOver.part == PathPart::Segment && (!_drag || !_drag->started)) {
		const auto center = toView.map(_penOver.point);
		const auto size = Scaled(4) * 1.;
		p.setPen(Qt::NoPen);
		p.setBrush(kHandleFill);
		p.drawEllipse(center, size + 2., size + 2.);
		auto plus = QPen(accent);
		plus.setWidthF(line * 1.5);
		plus.setCapStyle(Qt::RoundCap);
		p.setPen(plus);
		p.drawLine(
			center - QPointF(size - 1., 0.),
			center + QPointF(size - 1., 0.));
		p.drawLine(
			center - QPointF(0., size - 1.),
			center + QPointF(0., size - 1.));
		p.setBrush(Qt::NoBrush);
	}
}

void CanvasPanel::paintHint(QPainter &p) {
	const auto text = penHint();
	if (text.isEmpty()) {
		return;
	}
	const auto &font = st::normalFont;
	const auto padding = QMargins(Scaled(10), Scaled(4), Scaled(10), Scaled(4));
	const auto skip = Scaled(8);
	const auto available = width()
		- 2 * skip
		- padding.left()
		- padding.right();
	if (available < Scaled(80)) {
		return;
	}
	const auto elided = font->elided(text, available);
	const auto size = QSize(
		font->width(elided) + padding.left() + padding.right(),
		font->height + padding.top() + padding.bottom());
	const auto rect = QRect(
		QPoint((width() - size.width()) / 2, skip),
		size);
	auto hq = PainterHighQualityEnabler(p);
	// The pill lies half over the artboard, which may be white as the pill
	// itself: a hairline keeps its shape, and the text is the instruction
	// for the tool, so it is written in the main text color.
	auto border = QPen(st::shadowFg);
	border.setWidthF(st::lineWidth);
	p.setPen(border);
	p.setBrush(anim::with_alpha(st::windowBg->c, 0.95));
	const auto half = st::lineWidth / 2.;
	p.drawRoundedRect(
		QRectF(rect).adjusted(half, half, -half, -half),
		size.height() / 2.,
		size.height() / 2.);
	p.setFont(font);
	p.setPen(st::windowFg);
	p.drawText(
		rect.x() + padding.left(),
		rect.y() + padding.top() + font->ascent,
		elided);
}

// Snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).

namespace {

[[nodiscard]] QWidget *CreateCanvasScene(not_null<Ui::RpWidget*> parent) {
	return Ui::CreateChild<PanelSceneHost>(
		parent.get(),
		u":/animations/palette.tgs"_q,
		[](QWidget *parent, not_null<EditorController*> controller) {
			return not_null<Ui::RpWidget*>(
				Ui::CreateChild<CanvasPanel>(parent, controller));
		});
}

[[nodiscard]] not_null<CanvasPanel*> SceneCanvas(not_null<QWidget*> widget) {
	const auto host = static_cast<PanelSceneHost*>(widget.get());
	return static_cast<CanvasPanel*>(host->panel().get());
}

[[nodiscard]] bool SceneCanvasReady(not_null<QWidget*> widget) {
	return SceneCanvas(widget)->frameReady();
}

// The path item that takes the most of the canvas at the frame and has
// a handful of points (the most interesting one to show with the pen).
[[nodiscard]] NodeId LargestPath(const Document &document, int frame) {
	auto result = NodeId(0);
	auto best = 0.;
	const auto canvas = QRectF(QPointF(), QSizeF(document.size()));
	for (const auto &node : document.nodes()) {
		if (node.kind != NodeKind::Shape
			|| node.shapeType != ShapeType::Path
			|| node.hidden) {
			continue;
		}
		const auto value = document.valueAt(
			PropertyRef{ node.id, QByteArray("ks") },
			document.localFrame(node.id, frame));
		const auto points = (value && value->path)
			? int(value->path->vertices.size())
			: 0;
		if (points < 4 || points > 14) {
			continue;
		}
		const auto bounds = document.transformAt(node.id, frame).map(
			PainterPath(*value->path)).boundingRect();
		if (!canvas.contains(bounds)) {
			continue;
		}
		const auto area = bounds.width() * bounds.height();
		if (area > best) {
			best = area;
			result = node.id;
		}
	}
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	// The pen on a path item: its points, the handles of the selected one.
	RegisterScene(SceneDescriptor{
		.name = u"lottie_canvas_pen"_q,
		.size = QSize(720, 560),
		.create = CreateCanvasScene,
		.prepare = [](not_null<QWidget*> widget) {
			const auto host = static_cast<PanelSceneHost*>(widget.get());
			const auto controller = host->controller();
			const auto canvas = SceneCanvas(widget);
			canvas->setBackground(CanvasBackground::Light);
			controller->setCurrentFrame(controller->firstFrame() + 90);
			if (const auto path = LargestPath(
					controller->document(),
					controller->currentFrame())) {
				controller->select(path);
			}
			canvas->setTool(CanvasTool::Pen);
			canvas->selectPathVertex(1);
		},
		.ready = SceneCanvasReady,
	});

	// A layer with a mask: the dashed mask outline in the selection tool.
	RegisterScene(SceneDescriptor{
		.name = u"lottie_canvas_mask"_q,
		.size = QSize(720, 560),
		.create = CreateCanvasScene,
		.prepare = [](not_null<QWidget*> widget) {
			const auto host = static_cast<PanelSceneHost*>(widget.get());
			const auto controller = host->controller();
			const auto canvas = SceneCanvas(widget);
			canvas->setBackground(CanvasBackground::Checker);
			canvas->setTool(CanvasTool::Select);
			controller->setCurrentFrame(controller->firstFrame() + 90);
			const auto &document = controller->document();
			if (const auto layer = FindNodeByName(
					document,
					u"BOARD FRONT"_q,
					NodeKind::Layer)) {
				const auto bounds = DefaultMaskPath(
					document,
					layer,
					controller->currentFrame());
				auto rect = PainterPath(bounds).boundingRect();
				rect = rect.marginsRemoved(QMarginsF(
					rect.width() * 0.18,
					rect.height() * 0.12,
					rect.width() * 0.18,
					rect.height() * 0.3));
				controller->addMask(layer, EllipsePath(rect));
				controller->select(layer);
			}
		},
		.ready = SceneCanvasReady,
	});
});

} // namespace

} // namespace Oblivion::LottieEdit
