/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie_editor_masks.h"

#include "base/flat_map.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_lottie_editor_graph.h"
#include "oblivion/oblivion_lottie_editor_palette.h"

#include <cmath>
#include <limits>

namespace Oblivion::LottieEdit {
namespace {

constexpr auto kMaxDepth = 32;
constexpr auto kZeroTangent = 1e-6;
constexpr auto kSmoothSine = 0.02; // About one degree.
constexpr auto kSegmentRadiusFactor = 0.8;

struct ToolState {
	rpl::variable<CanvasTool> tool = CanvasTool::Select;
};

[[nodiscard]] base::flat_map<
	EditorController*,
	std::unique_ptr<ToolState>> &ToolStates() {
	static auto result = base::flat_map<
		EditorController*,
		std::unique_ptr<ToolState>>();
	return result;
}

[[nodiscard]] ToolState &ToolStateFor(not_null<EditorController*> controller) {
	auto &states = ToolStates();
	const auto raw = controller.get();
	const auto i = states.find(raw);
	if (i != end(states)) {
		return *i->second;
	}
	controller->lifetime().add([=] {
		ToolStates().remove(raw);
	});
	return *states.emplace(raw, std::make_unique<ToolState>()).first->second;
}

[[nodiscard]] double Length(QPointF point) {
	return std::hypot(point.x(), point.y());
}

[[nodiscard]] bool IsZero(QPointF point) {
	return Length(point) < kZeroTangent;
}

[[nodiscard]] Edit FailedEdit(const QString &error) {
	auto result = Edit();
	result.error = error;
	return result;
}

[[nodiscard]] bool IsShapeContainer(const NodeInfo &node) {
	return (node.kind == NodeKind::Layer && node.layerType == LayerType::Shape)
		|| (node.kind == NodeKind::Shape && node.shapeType == ShapeType::Group);
}

void CollectPaths(
		const Document &document,
		NodeId container,
		std::vector<NodeId> &result,
		int limit,
		int depth) {
	const auto node = document.node(container);
	if (!node || depth > kMaxDepth || !IsShapeContainer(*node)) {
		return;
	}
	for (const auto child : node->children) {
		if (int(result.size()) >= limit) {
			return;
		}
		const auto item = document.node(child);
		if (!item || item->hidden || item->kind != NodeKind::Shape) {
			continue;
		} else if (item->shapeType == ShapeType::Path) {
			if (!ranges::contains(result, child)) {
				result.push_back(child);
			}
		} else if (item->shapeType == ShapeType::Group) {
			CollectPaths(document, child, result, limit, depth + 1);
		}
	}
}

void CollectMasks(
		const Document &document,
		NodeId layer,
		std::vector<NodeId> &result,
		int limit) {
	const auto node = document.node(layer);
	if (!node || node->kind != NodeKind::Layer) {
		return;
	}
	for (const auto mask : node->masks) {
		if (int(result.size()) >= limit) {
			return;
		} else if (!ranges::contains(result, mask)) {
			result.push_back(mask);
		}
	}
}

[[nodiscard]] std::optional<PropValue> ValueOrDefault(
		const Document &document,
		const PropertyRef &ref,
		double frame) {
	if (auto value = document.valueAt(ref, frame)) {
		return value;
	}
	return document.defaultValue(ref);
}

} // namespace

CanvasTool CurrentTool(not_null<EditorController*> controller) {
	return ToolStateFor(controller).tool.current();
}

void SetCurrentTool(not_null<EditorController*> controller, CanvasTool tool) {
	ToolStateFor(controller).tool = tool;
}

rpl::producer<CanvasTool> CurrentToolValue(
		not_null<EditorController*> controller) {
	return ToolStateFor(controller).tool.value();
}

PropertyRef PathPropertyOf(const Document &document, NodeId id) {
	const auto node = document.node(id);
	if (!node) {
		return PropertyRef();
	} else if (node->kind == NodeKind::Mask) {
		return PropertyRef{ id, QByteArray("pt") };
	} else if (node->kind == NodeKind::Shape
		&& node->shapeType == ShapeType::Path) {
		return PropertyRef{ id, QByteArray("ks") };
	}
	return PropertyRef();
}

std::vector<NodeId> PathCandidates(
		const Document &document,
		NodeId id,
		int limit) {
	auto result = std::vector<NodeId>();
	const auto node = document.node(id);
	if (!node || limit <= 0) {
		return result;
	}
	switch (node->kind) {
	case NodeKind::Layer:
		CollectMasks(document, id, result, limit);
		CollectPaths(document, id, result, limit, 0);
		break;
	case NodeKind::Mask:
		result.push_back(id);
		CollectMasks(document, node->layer, result, limit);
		break;
	case NodeKind::Effect:
		CollectMasks(document, node->layer, result, limit);
		break;
	case NodeKind::Shape:
		if (node->shapeType == ShapeType::Path) {
			result.push_back(id);
			CollectPaths(document, node->parent, result, limit, 0);
		} else if (node->shapeType == ShapeType::Group) {
			CollectPaths(document, id, result, limit, 0);
		} else {
			CollectPaths(document, node->parent, result, limit, 0);
		}
		break;
	case NodeKind::Composition:
	case NodeKind::Asset:
		break;
	}
	return result;
}

NodeId PathContainerFor(const Document &document, NodeId id) {
	const auto node = document.node(id);
	if (!node) {
		return 0;
	} else if (IsShapeContainer(*node)) {
		return id;
	} else if (node->kind != NodeKind::Shape) {
		return 0;
	}
	const auto parent = document.node(node->parent);
	return (parent && IsShapeContainer(*parent)) ? parent->id : NodeId(0);
}

NodeId NewPathContainerFor(const Document &document, NodeId id) {
	return CanConvertToPath(document, id)
		? NodeId(0)
		: PathContainerFor(document, id);
}

PathData MappedPath(const PathData &path, const QTransform &transform) {
	auto result = NormalizedPath(path);
	for (auto i = 0; i != int(result.vertices.size()); ++i) {
		const auto vertex = result.vertices[i];
		const auto mapped = transform.map(vertex);
		result.inTangents[i] = transform.map(vertex + result.inTangents[i])
			- mapped;
		result.outTangents[i] = transform.map(vertex + result.outTangents[i])
			- mapped;
		result.vertices[i] = mapped;
	}
	return result;
}

PathPick PickPathPart(
		const PathData &path,
		const QTransform &toView,
		QPointF point,
		double radius,
		int selected) {
	const auto view = MappedPath(path, toView);
	const auto count = int(view.vertices.size());
	auto result = PathPick();
	if (!count || radius <= 0.) {
		return result;
	}
	const auto local = NormalizedPath(path);
	if (selected >= 0 && selected < count) {
		const auto vertex = view.vertices[selected];
		const auto consider = [&](PathPart part, QPointF tangent) {
			if (IsZero(tangent)) {
				return;
			}
			const auto distance = Length(vertex + tangent - point);
			if (distance <= radius
				&& (!result || distance < result.distance)) {
				const auto own = (part == PathPart::OutTangent)
					? local.outTangents[selected]
					: local.inTangents[selected];
				result = PathPick{
					.part = part,
					.index = selected,
					.point = local.vertices[selected] + own,
					.distance = distance,
				};
			}
		};
		consider(PathPart::OutTangent, view.outTangents[selected]);
		consider(PathPart::InTangent, view.inTangents[selected]);
		if (result) {
			return result;
		}
	}
	for (auto i = 0; i != count; ++i) {
		const auto distance = Length(view.vertices[i] - point);
		if (distance <= radius && (!result || distance < result.distance)) {
			result = PathPick{
				.part = PathPart::Vertex,
				.index = i,
				.point = local.vertices[i],
				.distance = distance,
			};
		}
	}
	if (result) {
		return result;
	}
	const auto hit = NearestPathPoint(view, point);
	if (hit.segment >= 0
		&& hit.segment < PathSegmentCount(view)
		&& hit.distance <= radius * kSegmentRadiusFactor) {
		result = PathPick{
			.part = PathPart::Segment,
			.index = hit.segment,
			.t = hit.t,
			.point = PathPointAt(local, hit.segment, hit.t),
			.distance = hit.distance,
		};
	}
	return result;
}

bool IsSmoothVertex(const PathData &path, int index) {
	if (index < 0 || index >= int(path.vertices.size())) {
		return false;
	}
	const auto in = (index < int(path.inTangents.size()))
		? path.inTangents[index]
		: QPointF();
	const auto out = (index < int(path.outTangents.size()))
		? path.outTangents[index]
		: QPointF();
	const auto inLength = Length(in);
	const auto outLength = Length(out);
	if (inLength < kZeroTangent || outLength < kZeroTangent) {
		return false;
	}
	const auto cross = in.x() * out.y() - in.y() * out.x();
	const auto dot = in.x() * out.x() + in.y() * out.y();
	return (dot < 0.)
		&& (std::abs(cross) <= kSmoothSine * inLength * outLength);
}

PathData WithMovedVertex(const PathData &path, int index, QPointF position) {
	auto result = NormalizedPath(path);
	if (index >= 0 && index < int(result.vertices.size())) {
		result.vertices[index] = position;
	}
	return result;
}

PathData WithMovedTangent(
		const PathData &path,
		int index,
		bool out,
		QPointF tangent,
		TangentMode mode) {
	auto result = NormalizedPath(path);
	if (index < 0 || index >= int(result.vertices.size())) {
		return result;
	}
	auto &own = out ? result.outTangents[index] : result.inTangents[index];
	auto &other = out ? result.inTangents[index] : result.outTangents[index];
	own = tangent;
	switch (mode) {
	case TangentMode::Free:
		break;
	case TangentMode::Mirrored:
		other = -tangent;
		break;
	case TangentMode::Aligned: {
		const auto length = Length(other);
		const auto ownLength = Length(tangent);
		if (length >= kZeroTangent && ownLength >= kZeroTangent) {
			other = -tangent * (length / ownLength);
		}
	} break;
	}
	return result;
}

PathData WithToggledVertex(const PathData &path, int index) {
	return IsCornerVertex(path, index)
		? WithSmoothVertex(path, index)
		: WithCornerVertex(path, index);
}

Edit AppendPathVertex(
		const Document &document,
		const PropertyRef &path,
		QPointF position,
		bool atStart,
		double frame) {
	const auto value = document.valueAt(path, frame);
	if (!value || !value->path) {
		return FailedEdit(u"AppendPathVertex: not a path"_q);
	}
	auto current = NormalizedPath(*value->path);
	const auto count = int(current.vertices.size());
	if (current.closed || count < 1) {
		return FailedEdit(u"AppendPathVertex: closed or empty path"_q);
	} else if (count < 2) {
		if (document.animated(path)) {
			return FailedEdit(u"AppendPathVertex: one animated vertex"_q);
		}
		const auto at = atStart ? 0 : count;
		current.vertices.insert(begin(current.vertices) + at, position);
		current.inTangents.insert(begin(current.inTangents) + at, QPointF());
		current.outTangents.insert(begin(current.outTangents) + at, QPointF());
		return SetValueAt(
			document,
			path,
			PropValue::Path(std::move(current)),
			frame);
	}
	// A vertex right on the old end in every keyframe, then the end itself
	// goes to the new place at this frame only.
	auto first = InsertPathVertex(
		document,
		path,
		atStart ? 0 : (count - 2),
		atStart ? 0. : 1.);
	if (!first) {
		return first;
	}
	const auto inserted = first.document.valueAt(path, frame);
	if (!inserted
		|| !inserted->path
		|| int(inserted->path->vertices.size()) != count + 1) {
		return FailedEdit(u"AppendPathVertex: the vertex was not added"_q);
	}
	auto moved = NormalizedPath(*inserted->path);
	const auto index = atStart ? 0 : count;
	moved.vertices[index] = position;
	moved.inTangents[index] = QPointF();
	moved.outTangents[index] = QPointF();
	auto second = SetValueAt(
		first.document,
		path,
		PropValue::Path(std::move(moved)),
		frame);
	return Combined(std::move(first), std::move(second));
}

Edit SetPathAt(
		const Document &document,
		const PropertyRef &path,
		const PathData &value,
		double frame) {
	return SetValueAt(document, path, PropValue::Path(value), frame);
}

bool CanConvertToPath(const Document &document, NodeId shape) {
	const auto node = document.node(shape);
	if (!node || node->kind != NodeKind::Shape) {
		return false;
	} else if (node->shapeType != ShapeType::Rectangle
		&& node->shapeType != ShapeType::Ellipse) {
		return false;
	}
	const auto parent = document.node(node->parent);
	return parent && IsShapeContainer(*parent);
}

Edit ConvertToPath(const Document &document, NodeId shape, double frame) {
	if (!CanConvertToPath(document, shape)) {
		return FailedEdit(u"ConvertToPath: not a rectangle or an ellipse"_q);
	}
	const auto node = document.node(shape);
	const auto size = document.valueAt(
		PropertyRef{ shape, QByteArray("s") },
		frame);
	if (!size) {
		return FailedEdit(u"ConvertToPath: no size"_q);
	}
	const auto position = ValueOrDefault(
		document,
		PropertyRef{ shape, QByteArray("p") },
		frame);
	const auto center = position ? position->point() : QPointF();
	const auto extent = size->point();
	const auto rect = QRectF(
		center.x() - extent.x() / 2.,
		center.y() - extent.y() / 2.,
		extent.x(),
		extent.y());
	auto path = PathData();
	if (node->shapeType == ShapeType::Rectangle) {
		const auto roundness = ValueOrDefault(
			document,
			PropertyRef{ shape, QByteArray("r") },
			frame);
		path = RectanglePath(rect, roundness ? roundness->scalar() : 0.);
	} else {
		path = EllipsePath(rect);
	}
	if (document.json(shape).get("d").toInt(1) == 3) {
		path = ReversedPath(path);
	}
	const auto container = node->parent;
	const auto index = node->index;
	const auto name = node->name;
	auto first = AddPath(document, container, path, name, index);
	if (!first) {
		return first;
	}
	auto second = DeleteNodes(first.document, { shape });
	return Combined(std::move(first), std::move(second));
}

bool LayerMasksSwitchedOff(const Document &document, NodeId layer) {
	const auto node = document.node(layer);
	if (!node || node->kind != NodeKind::Layer || node->masksEnabled) {
		return false;
	}
	return ranges::any_of(node->masks, [&](NodeId mask) {
		const auto mode = document.json(mask).get("mode").toString();
		return !mode.startsWith(QChar('n'));
	});
}

Edit EnableLayerMasks(const Document &document, NodeId layer) {
	const auto node = document.node(layer);
	if (!node || node->kind != NodeKind::Layer) {
		return FailedEdit(u"EnableLayerMasks: not a layer"_q);
	}
	return SetNodeMember(
		document,
		layer,
		"hasMask",
		Json::Value::FromBool(true));
}

QString MaskModeText(MaskMode mode) {
	switch (mode) {
	case MaskMode::None: return tr::lng_oblivion_lottie_mask_mode_none(tr::now);
	case MaskMode::Add: return tr::lng_oblivion_lottie_mask_mode_add(tr::now);
	case MaskMode::Subtract:
		return tr::lng_oblivion_lottie_mask_mode_subtract(tr::now);
	case MaskMode::Intersect:
		return tr::lng_oblivion_lottie_mask_mode_intersect(tr::now);
	case MaskMode::Lighten:
		return tr::lng_oblivion_lottie_mask_mode_lighten(tr::now);
	case MaskMode::Darken:
		return tr::lng_oblivion_lottie_mask_mode_darken(tr::now);
	case MaskMode::Difference:
		return tr::lng_oblivion_lottie_mask_mode_difference(tr::now);
	}
	return tr::lng_oblivion_lottie_mask_mode_add(tr::now);
}

QString CanvasToolText(CanvasTool tool) {
	switch (tool) {
	case CanvasTool::Select:
		return tr::lng_oblivion_lottie_mask_tool_select(tr::now);
	case CanvasTool::Pen:
		return tr::lng_oblivion_lottie_mask_tool_pen(tr::now);
	}
	return QString();
}

QString NewMaskName(const Document &document, NodeId layer) {
	const auto node = document.node(layer);
	const auto count = node ? int(node->masks.size()) : 0;
	return tr::lng_oblivion_lottie_node_mask(tr::now)
		+ ' '
		+ QString::number(count + 1);
}

// Self-test.

namespace {

struct EditorTest {
	QStringList &log;
	int checks = 0;
	int failures = 0;

	void check(bool condition, const QString &what) {
		++checks;
		if (!condition) {
			++failures;
			log.push_back(u"lottie_editor: FAILED: "_q + what);
		}
	}
};

[[nodiscard]] bool Close(double a, double b, double epsilon = 1e-6) {
	return std::abs(a - b) <= epsilon;
}

[[nodiscard]] bool Close(QPointF a, QPointF b, double epsilon = 1e-6) {
	return Close(a.x(), b.x(), epsilon) && Close(a.y(), b.y(), epsilon);
}

[[nodiscard]] PathData OpenPath() {
	auto result = PathData();
	result.vertices = { QPointF(0., 0.), QPointF(50., 0.), QPointF(50., 40.) };
	result.inTangents = { QPointF(), QPointF(-10., 0.), QPointF() };
	result.outTangents = { QPointF(), QPointF(10., 0.), QPointF() };
	return result;
}

[[nodiscard]] NodeId FirstShapeOf(const Document &document, ShapeType type) {
	for (const auto &node : document.nodes()) {
		if (node.kind == NodeKind::Shape && node.shapeType == type) {
			return node.id;
		}
	}
	return 0;
}

void TestPicking(EditorTest &test) {
	const auto square = RectanglePath(QRectF(0., 0., 100., 100.));
	const auto view = QTransform(2., 0., 0., 2., 30., 40.);
	const auto vertex = PickPathPart(square, view, QPointF(232., 43.), 6.);
	test.check(
		vertex.part == PathPart::Vertex && vertex.index == 1,
		u"a vertex is picked through the view transform"_q);
	test.check(
		Close(vertex.point, QPointF(100., 0.)),
		u"the picked vertex is in path coordinates"_q);

	const auto segment = PickPathPart(square, view, QPointF(131., 42.), 6.);
	test.check(
		segment.part == PathPart::Segment && segment.index == 0,
		u"a point of an edge is picked as a segment"_q);
	test.check(
		Close(segment.t, 0.505, 0.02) && Close(segment.point.y(), 0., 1e-3),
		u"the segment position is where the cursor is"_q);

	const auto none = PickPathPart(square, view, QPointF(131., 90.), 6.);
	test.check(!none, u"nothing is picked far from the outline"_q);

	const auto ellipse = EllipsePath(QRectF(0., 0., 100., 60.));
	const auto handle = ellipse.vertices[0] + ellipse.outTangents[0];
	const auto withoutSelection = PickPathPart(
		ellipse,
		QTransform(),
		handle,
		4.);
	test.check(
		withoutSelection.part != PathPart::OutTangent
			&& withoutSelection.part != PathPart::InTangent,
		u"handles of a vertex that is not selected are not picked"_q);
	const auto withSelection = PickPathPart(
		ellipse,
		QTransform(),
		handle + QPointF(1., 1.),
		4.,
		0);
	test.check(
		withSelection.part == PathPart::OutTangent
			&& withSelection.index == 0
			&& Close(withSelection.point, handle),
		u"the handle of the selected vertex is picked"_q);

	const auto corner = PickPathPart(
		square,
		QTransform(),
		QPointF(1., 1.),
		4.,
		0);
	test.check(
		corner.part == PathPart::Vertex && corner.index == 0,
		u"zero handles do not hide their vertex"_q);

	const auto mapped = MappedPath(ellipse, view);
	test.check(
		Close(mapped.vertices[1], view.map(ellipse.vertices[1]))
			&& Close(
				mapped.outTangents[1],
				QPointF(
					ellipse.outTangents[1].x() * 2.,
					ellipse.outTangents[1].y() * 2.)),
		u"MappedPath maps tangents as vectors"_q);
	const auto painted = PainterPath(mapped).boundingRect();
	const auto expected = view.map(PainterPath(ellipse)).boundingRect();
	test.check(
		Close(painted.left(), expected.left(), 1e-3)
			&& Close(painted.width(), expected.width(), 1e-3)
			&& Close(painted.height(), expected.height(), 1e-3),
		u"MappedPath gives the mapped outline"_q);
}

void TestVertexEditing(EditorTest &test) {
	const auto ellipse = EllipsePath(QRectF(0., 0., 100., 60.));
	const auto square = RectanglePath(QRectF(0., 0., 100., 100.));
	test.check(IsSmoothVertex(ellipse, 0), u"an ellipse vertex is smooth"_q);
	test.check(!IsSmoothVertex(square, 0), u"a square vertex is a corner"_q);
	test.check(!IsSmoothVertex(square, 12), u"a bad index is not smooth"_q);

	const auto moved = WithMovedVertex(ellipse, 2, QPointF(7., 9.));
	test.check(
		Close(moved.vertices[2], QPointF(7., 9.))
			&& Close(moved.outTangents[2], ellipse.outTangents[2])
			&& Close(moved.inTangents[2], ellipse.inTangents[2]),
		u"a moved vertex keeps its handles"_q);
	test.check(
		WithMovedVertex(ellipse, 9, QPointF()) == NormalizedPath(ellipse),
		u"a bad vertex index changes nothing"_q);

	const auto before = ellipse.inTangents[0];
	const auto free = WithMovedTangent(
		ellipse,
		0,
		true,
		QPointF(0., 30.),
		TangentMode::Free);
	test.check(
		Close(free.outTangents[0], QPointF(0., 30.))
			&& Close(free.inTangents[0], before),
		u"a free handle leaves the other one alone"_q);
	const auto aligned = WithMovedTangent(
		ellipse,
		0,
		true,
		QPointF(0., 30.),
		TangentMode::Aligned);
	test.check(
		Close(aligned.inTangents[0], QPointF(0., -Length(before)), 1e-6)
			&& IsSmoothVertex(aligned, 0),
		u"an aligned handle turns the other one, its length stays"_q);
	const auto mirrored = WithMovedTangent(
		square,
		1,
		false,
		QPointF(-12., 5.),
		TangentMode::Mirrored);
	test.check(
		Close(mirrored.inTangents[1], QPointF(-12., 5.))
			&& Close(mirrored.outTangents[1], QPointF(12., -5.)),
		u"a mirrored handle gives the opposite one"_q);

	const auto smooth = WithToggledVertex(square, 1);
	test.check(
		!IsCornerVertex(smooth, 1) && IsSmoothVertex(smooth, 1),
		u"a corner toggles to a smooth vertex"_q);
	test.check(
		IsCornerVertex(WithToggledVertex(smooth, 1), 1),
		u"a smooth vertex toggles back to a corner"_q);
}

void TestDocumentHelpers(EditorTest &test) {
	auto document = Document::Blank(QSize(200, 200), 60., 60);
	auto added = AddLayer(
		document,
		LayerTemplate::Shape,
		u"Shape"_q,
		0,
		0,
		ShapeTemplate::Rectangle);
	test.check(added.ok(), u"a shape layer is added"_q);
	if (!added) {
		return;
	}
	document = added.document;
	const auto layer = document.layers().empty()
		? NodeId(0)
		: document.layers().front();
	const auto rectangle = FirstShapeOf(document, ShapeType::Rectangle);
	test.check(layer && rectangle, u"the template has a rectangle"_q);
	if (!layer || !rectangle) {
		return;
	}
	test.check(
		!PathPropertyOf(document, rectangle).valid()
			&& !PathPropertyOf(document, layer).valid(),
		u"a rectangle and a layer have no path property"_q);
	const auto group = document.node(rectangle)->parent;
	test.check(
		PathContainerFor(document, rectangle) == group
			&& PathContainerFor(document, layer) == layer
			&& PathContainerFor(document, 0) == 0,
		u"the container of a new path is the group / the shape layer"_q);
	test.check(
		NewPathContainerFor(document, rectangle) == 0
			&& NewPathContainerFor(document, group) == group
			&& NewPathContainerFor(document, layer) == layer
			&& NewPathContainerFor(document, 0) == 0,
		u"the pen starts no path while a rectangle waits to be converted"_q);

	const auto outlineBefore = document.outlineAt(layer, 0.).boundingRect();
	test.check(
		CanConvertToPath(document, rectangle)
			&& !CanConvertToPath(document, layer),
		u"only rectangles and ellipses convert to paths"_q);
	auto converted = ConvertToPath(document, rectangle, 0.);
	test.check(converted.ok(), u"a rectangle converts to a path"_q);
	if (!converted) {
		return;
	}
	document = converted.document;
	const auto path = FirstShapeOf(document, ShapeType::Path);
	test.check(
		path && !FirstShapeOf(document, ShapeType::Rectangle),
		u"the rectangle is replaced with a path item"_q);
	const auto outlineAfter = document.outlineAt(layer, 0.).boundingRect();
	test.check(
		Close(outlineBefore.left(), outlineAfter.left(), 0.01)
			&& Close(outlineBefore.top(), outlineAfter.top(), 0.01)
			&& Close(outlineBefore.width(), outlineAfter.width(), 0.01)
			&& Close(outlineBefore.height(), outlineAfter.height(), 0.01),
		u"the converted path has the outline of the rectangle"_q);
	if (!path) {
		return;
	}
	const auto ref = PathPropertyOf(document, path);
	test.check(
		ref.valid() && ref.path == "ks",
		u"a path item has the path property ks"_q);
	const auto value = document.valueAt(ref, 0.);
	test.check(
		value && value->path && value->path->vertices.size() == 4
			&& value->path->closed,
		u"the converted rectangle has four vertices and is closed"_q);
	test.check(
		!AppendPathVertex(document, ref, QPointF(), false, 0.).ok(),
		u"a closed path does not take a vertex at its end"_q);

	auto masked = AddMask(
		document,
		layer,
		RectanglePath(QRectF(-20., -20., 40., 40.)));
	test.check(masked.ok() && !masked.created.empty(), u"a mask is added"_q);
	if (!masked || masked.created.empty()) {
		return;
	}
	document = masked.document;
	const auto mask = masked.created.front();
	const auto maskRef = PathPropertyOf(document, mask);
	test.check(
		maskRef.valid() && maskRef.path == "pt",
		u"a mask has the path property pt"_q);
	const auto candidates = PathCandidates(document, layer);
	test.check(
		ranges::contains(candidates, mask)
			&& ranges::contains(candidates, path),
		u"a layer offers its masks and its paths to the pen"_q);
	const auto forMask = PathCandidates(document, mask);
	test.check(
		!forMask.empty() && forMask.front() == mask,
		u"a selected mask is the first candidate"_q);
	const auto fill = FirstShapeOf(document, ShapeType::Fill);
	test.check(
		fill && ranges::contains(PathCandidates(document, fill), path),
		u"a fill offers the paths next to it"_q);
	test.check(
		fill
			&& NewPathContainerFor(document, fill) == group
			&& NewPathContainerFor(document, path) == group,
		u"the pen starts a path next to a fill or another path"_q);
	test.check(
		PathCandidates(document, layer, 1).size() == 1,
		u"the candidates limit is kept"_q);

	// An open animated path: every keyframe gets the new vertex.
	auto opened = AddPath(document, group, OpenPath(), u"Open"_q);
	test.check(
		opened.ok() && !opened.created.empty(),
		u"an open path is added"_q);
	if (!opened || opened.created.empty()) {
		return;
	}
	document = opened.document;
	const auto open = PathPropertyOf(document, opened.created.front());
	auto shifted = OpenPath();
	for (auto &vertex : shifted.vertices) {
		vertex += QPointF(0., 10.);
	}
	auto first = AddKeyframe(document, open, 0.);
	test.check(first.ok(), u"the open path gets a keyframe"_q);
	if (!first) {
		return;
	}
	auto second = AddKeyframe(
		first.document,
		open,
		20.,
		PropValue::Path(shifted));
	test.check(second.ok(), u"the open path gets a second keyframe"_q);
	if (!second) {
		return;
	}
	document = second.document;
	const auto appended = AppendPathVertex(
		document,
		open,
		QPointF(90., 90.),
		false,
		20.);
	test.check(appended.ok(), u"a vertex is appended to an open path"_q);
	if (appended) {
		const auto at0 = appended.document.valueAt(open, 0.);
		const auto at20 = appended.document.valueAt(open, 20.);
		test.check(
			at0 && at0->path && at20 && at20->path
				&& at0->path->vertices.size() == 4
				&& at20->path->vertices.size() == 4,
			u"every keyframe has the appended vertex"_q);
		if (at0 && at0->path && at20 && at20->path
			&& at0->path->vertices.size() == 4
			&& at20->path->vertices.size() == 4) {
			test.check(
				Close(at20->path->vertices[3], QPointF(90., 90.), 1e-3),
				u"the new end is where it was put at this frame"_q);
			test.check(
				Close(at0->path->vertices[3], QPointF(50., 40.), 1e-3),
				u"the new end stays on the old end in other keyframes"_q);
			test.check(
				Close(at20->path->vertices[2], QPointF(50., 50.), 1e-3)
					&& Close(at20->path->vertices[0], QPointF(0., 10.), 1e-3),
				u"the old vertices keep their places"_q);
		}
	}
	const auto prepended = AppendPathVertex(
		document,
		open,
		QPointF(-30., 5.),
		true,
		0.);
	test.check(prepended.ok(), u"a vertex is prepended to an open path"_q);
	if (prepended) {
		const auto at0 = prepended.document.valueAt(open, 0.);
		const auto at20 = prepended.document.valueAt(open, 20.);
		test.check(
			at0 && at0->path && at20 && at20->path
				&& at0->path->vertices.size() == 4
				&& at20->path->vertices.size() == 4
				&& Close(at0->path->vertices[0], QPointF(-30., 5.), 1e-3)
				&& Close(at0->path->vertices[1], QPointF(0., 0.), 1e-3)
				&& Close(at20->path->vertices[0], QPointF(0., 10.), 1e-3),
			u"the new start is at this frame only"_q);
	}
	auto dragged = shifted;
	dragged.vertices[1] = QPointF(60., 25.);
	const auto set = SetPathAt(document, open, dragged, 20.);
	const auto read = set.ok() ? set.document.valueAt(open, 20.) : std::nullopt;
	test.check(
		read && read->path
			&& Close(read->path->vertices[1], QPointF(60., 25.), 1e-3)
			&& set.document.keyframeTimes(open).size() == 2,
		u"SetPathAt moves a vertex in the keyframe at the frame"_q);
}

void TestLayerMasks(EditorTest &test) {
	auto document = Document::Blank(QSize(200, 200), 60., 60);
	const auto square = RectanglePath(QRectF(-20., -20., 40., 40.));
	for (auto i = 0; i != 2; ++i) {
		auto added = AddLayer(
			document,
			LayerTemplate::Shape,
			u"Shape"_q,
			0,
			0,
			ShapeTemplate::Rectangle);
		if (!added) {
			test.check(false, u"a layer for the masks is added"_q);
			return;
		}
		document = added.document;
	}
	const auto layers = document.layers();
	test.check(layers.size() == 2, u"two layers for the masks"_q);
	if (layers.size() != 2) {
		return;
	}
	const auto first = layers.front();
	const auto second = layers.back();
	for (const auto layer : layers) {
		auto masked = AddMask(document, layer, square);
		if (!masked) {
			test.check(false, u"a mask is added to each layer"_q);
			return;
		}
		test.check(
			!LayerMasksSwitchedOff(masked.document, layer),
			u"a new mask is switched on"_q);
		auto off = SetNodeMember(
			masked.document,
			layer,
			"hasMask",
			Json::Value::FromBool(false));
		if (!off) {
			test.check(false, u"the masks of a layer are switched off"_q);
			return;
		}
		document = off.document;
	}
	const auto firstNode = document.node(first);
	const auto secondNode = document.node(second);
	test.check(
		firstNode
			&& secondNode
			&& !firstNode->masks.empty()
			&& !secondNode->masks.empty(),
		u"the layers keep their ids and masks"_q);
	if (!firstNode
		|| !secondNode
		|| firstNode->masks.empty()
		|| secondNode->masks.empty()) {
		return;
	}
	test.check(
		LayerMasksSwitchedOff(document, first)
			&& LayerMasksSwitchedOff(document, second),
		u"masks without hasMask are reported as switched off"_q);
	const auto mask = secondNode->masks.front();
	test.check(
		!LayerMasksSwitchedOff(document, mask)
			&& !LayerMasksSwitchedOff(document, 0)
			&& !EnableLayerMasks(document, mask).ok()
			&& !EnableLayerMasks(document, 0).ok(),
		u"only a layer has masks to switch on"_q);

	const auto enabled = EnableLayerMasks(document, first);
	test.check(
		enabled.ok()
			&& !LayerMasksSwitchedOff(enabled.document, first)
			&& LayerMasksSwitchedOff(enabled.document, second),
		u"switching the masks on changes this layer only"_q);
	if (!enabled) {
		return;
	}
	const auto silent = SetMaskMode(enabled.document, mask, MaskMode::None);
	test.check(
		silent.ok() && !LayerMasksSwitchedOff(silent.document, second),
		u"a layer with nothing but none masks has nothing to switch on"_q);
}

void TestTools(EditorTest &test) {
	auto fired = std::vector<CanvasTool>();
	auto lifetime = rpl::lifetime();
	{
		auto controller = EditorController(Document::Blank());
		const auto raw = not_null<EditorController*>(&controller);
		test.check(
			CurrentTool(raw) == CanvasTool::Select,
			u"the selection tool is the default"_q);
		CurrentToolValue(raw) | rpl::on_next([&](CanvasTool tool) {
			fired.push_back(tool);
		}, lifetime);
		SetCurrentTool(raw, CanvasTool::Pen);
		SetCurrentTool(raw, CanvasTool::Pen);
		test.check(
			CurrentTool(raw) == CanvasTool::Pen,
			u"the tool is remembered for the controller"_q);
		auto other = EditorController(Document::Blank());
		test.check(
			CurrentTool(&other) == CanvasTool::Select,
			u"another editor has its own tool"_q);
	}
	test.check(
		fired == std::vector<CanvasTool>{
			CanvasTool::Select,
			CanvasTool::Pen,
		},
		u"the tool value fires once per change"_q);
	lifetime.destroy();
}

void TestGraphMath(EditorTest &test) {
	using namespace GraphMath;
	const auto segment = Segment{ .t0 = 10., .t1 = 30., .v0 = 100., .v1 = 60. };
	const auto handle = QPointF(0.25, 0.5);
	const auto point = ValuePoint(segment, handle);
	test.check(
		Close(point, QPointF(15., 80.)),
		u"a value handle is a point of the (time, value) plane"_q);
	test.check(
		Close(ValueHandle(segment, point, QPointF()), handle),
		u"a value handle reads back from its point"_q);
	const auto flat = Segment{ .t0 = 0., .t1 = 10., .v0 = 5., .v1 = 5. };
	test.check(
		Close(
			ValueHandle(flat, QPointF(4., 99.), QPointF(0.1, 0.7)),
			QPointF(0.4, 0.7)),
		u"a flat segment keeps the y of the handle"_q);
	test.check(
		Close(
			ValueHandle(segment, QPointF(40., 80.), QPointF()).x(),
			1.),
		u"a handle does not leave its segment in time"_q);

	const auto linear = Easing::Linear();
	const auto rate = (segment.v1 - segment.v0) / (segment.t1 - segment.t0);
	test.check(
		Close(StartSpeed(segment, linear), rate, 1e-3)
			&& Close(EndSpeed(segment, linear), rate, 1e-3)
			&& Close(SpeedAt(segment, linear, 0.5), rate, 1e-3),
		u"a linear segment has one speed everywhere"_q);
	const auto smooth = Easing::EaseInOut();
	test.check(
		std::abs(StartSpeed(segment, smooth)) < std::abs(rate) * 0.05
			&& std::abs(EndSpeed(segment, smooth)) < std::abs(rate) * 0.05
			&& std::abs(SpeedAt(segment, smooth, 0.5)) > std::abs(rate),
		u"an eased segment starts and ends slowly, runs in the middle"_q);
	test.check(
		Close(SpeedAt(segment, Easing::Hold(), 0.5), 0.),
		u"a hold segment does not move"_q);

	const auto custom = Easing{
		.out = QPointF(0.4, 0.2),
		.in = QPointF(0.7, 0.9),
	};
	const auto outPoint = SpeedPoint(segment, custom, true);
	test.check(
		Close(outPoint.x(), 18.) && Close(outPoint.y(), 0.5 * rate, 1e-6),
		u"the outgoing speed handle is on the level of its speed"_q);
	test.check(
		Close(
			SpeedHandle(segment, outPoint, true, custom.out),
			custom.out,
			1e-6),
		u"the outgoing speed handle reads back"_q);
	const auto inPoint = SpeedPoint(segment, custom, false);
	test.check(
		Close(inPoint.x(), 24.)
			&& Close(inPoint.y(), (0.1 / 0.3) * rate, 1e-6),
		u"the incoming speed handle is on the level of its speed"_q);
	test.check(
		Close(
			SpeedHandle(segment, inPoint, false, custom.in),
			custom.in,
			1e-6),
		u"the incoming speed handle reads back"_q);
	const auto still = SpeedHandle(flat, QPointF(3., 8.), true, QPointF(0.2, 0.6));
	test.check(
		Close(still, QPointF(0.3, 0.6)),
		u"a speed handle of a flat segment keeps its y"_q);

	for (const auto &[span, ticks] : {
		std::pair{ 100., 5 },
		std::pair{ 1., 4 },
		std::pair{ 0.03, 6 },
		std::pair{ 7300., 8 },
	}) {
		const auto step = NiceStep(span, ticks);
		auto mantissa = (step > 0.) ? step : 1.;
		while (mantissa >= 9.9999) {
			mantissa /= 10.;
		}
		while (mantissa < 0.99999) {
			mantissa *= 10.;
		}
		test.check(
			step > 0.
				&& span / step <= ticks + 1e-9
				&& (Close(mantissa, 1., 1e-4)
					|| Close(mantissa, 2., 1e-4)
					|| Close(mantissa, 5., 1e-4)),
			u"NiceStep(%1, %2) is 1 / 2 / 5 times a power of ten"_q
				.arg(span)
				.arg(ticks));
	}
	test.check(
		NiceStep(0., 5) > 0. && NiceStep(10., 0) > 0.,
		u"NiceStep survives empty input"_q);

	const auto grid = GridValues(-100., 300., 100.);
	test.check(
		grid.size() == 13
			&& Close(grid.front(), -500.)
			&& Close(grid.back(), 700.)
			&& ranges::is_sorted(grid)
			&& ranges::contains(grid, 0.),
		u"the grid is the multiples of the step around the range"_q);
	const auto fine = GridValues(0.2, 0.9, 0.2);
	test.check(
		!fine.empty()
			&& Close(fine.front(), -0.4)
			&& fine.back() <= 1.6 + 1e-9
			&& fine.back() > 1.4 - 1e-9,
		u"the grid starts on a multiple of a fractional step"_q);

	// Values of a broken file: the walk must end whatever they are.
	const auto largest = std::numeric_limits<double>::max();
	const auto endless = std::numeric_limits<double>::infinity();
	const auto broken = std::numeric_limits<double>::quiet_NaN();
	const auto after = std::nextafter(1e10, endless);
	test.check(
		GridValues(0., 1.5e308, 5e307).empty()
			&& GridValues(1.35e308, 1.65e308, 1e307).empty()
			&& GridValues(-largest, largest, largest / 4.).empty()
			&& GridValues(-0.55e308, 0.55e308, 5e307).size() <= 200,
		u"a range that overflows gets no grid"_q);
	test.check(
		GridValues(1e10, after, NiceStep(after - 1e10, 4)).empty(),
		u"a step below the precision of the values gets no grid"_q);
	test.check(
		GridValues(0., 1000., 1.).empty()
			&& GridValues(0., 1., 0.).empty()
			&& GridValues(0., 1., -1.).empty()
			&& GridValues(1., 0., 0.5).empty()
			&& GridValues(5., 5., 1.).empty()
			&& GridValues(0., endless, 1.).empty()
			&& GridValues(broken, 1., 1.).empty()
			&& GridValues(0., 1., broken).empty()
			&& GridValues(0., 1., endless).empty(),
		u"a grid that can't be walked is empty"_q);
}

} // namespace

bool RunEditorSelfTest(QStringList &log) {
	auto test = EditorTest{ log };
	const auto started = crl::now();
	TestPicking(test);
	TestVertexEditing(test);
	TestDocumentHelpers(test);
	TestLayerMasks(test);
	TestTools(test);
	TestGraphMath(test);
	log.push_back(u"lottie_editor: %1 checks, %2 failed, %3 ms"_q
		.arg(test.checks)
		.arg(test.failures)
		.arg(crl::now() - started));
	return !test.failures;
}

} // namespace Oblivion::LottieEdit
