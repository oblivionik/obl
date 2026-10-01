/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"

#include <rpl/event_stream.h>
#include <rpl/producer.h>
#include <rpl/variable.h>

#include <QtCore/QPointF>
#include <QtCore/QSize>
#include <QtGui/QColor>
#include <QtGui/QPainterPath>
#include <QtGui/QTransform>

#include <memory>
#include <optional>
#include <vector>

namespace Ui {
class Show;
} // namespace Ui

// Lottie editor: document model, operations, TGS validator and the
// controller that the editor window panels observe.
//
// Model overview.
//
// The document is an immutable, structurally shared JSON tree
// (Json::Value). Every edit produces a new tree that shares all untouched
// subtrees with the previous one, so undo / redo only swap tree roots,
// copies are cheap and a Document can be handed to a background thread
// (crl::async) for serialization or rendering while the main thread keeps
// editing. Json::Value keeps the object key order and every unknown key,
// so an untouched document serializes to semantically identical JSON (the
// same values, keys and order; numbers are printed in the shortest form
// that reads back to the same double).
//
// Nodes. Every JSON object carries a NodeId that survives edits (an edited
// copy keeps the id of the original, duplicates get new ids). Addressable
// nodes are the composition (the root), assets, layers, shape items
// (including group "tr" transforms), masks and layer effects. The UI keeps
// node ids for selection; after any change look nodes up again with
// Document::node(), a removed node simply is not found.
//
// Properties. An animatable property is addressed by PropertyRef: the node
// that owns it plus a dot separated JSON path relative to that node, for
// example { layer, "ks.o" } (layer opacity), { fill, "c" } (fill color),
// { gradientFill, "g.k" } (gradient stops), { layer, "ks.p.x" } (separated
// position X), { effect, "ef.1.v" } (second effect value). A numeric path
// component indexes a JSON array. Document::properties() lists what a node
// has, so the UI rarely needs to build paths itself.
//
// Time. Keyframe times ("t") and layer "ip" / "op" / "st" are frame numbers
// in the time base of the composition that holds the layer. For the root
// composition that is the absolute Lottie frame: EditorController's current
// frame is in [Document::inPoint(), Document::outPoint()), and the frame
// index for Oblivion::Lottie::RenderFrame() / Renderer::render() is
// currentFrame - inPoint (EditorController::frameIndex()). Properties of
// layers inside precompositions use the precomposition's local time, get
// it with Document::localFrame(node, rootFrame).
//
// Interpolation follows rlottie (the renderer Telegram uses): cubic bezier
// easing solved like rlottie's VInterpolator (only the first component of
// per-dimension "i" / "o" arrays counts), hold keyframes, "e" end values
// of old exports, spatial "ti" / "to" tangents for point values (motion
// along the bezier by arc length). Two rlottie quirks are not replicated:
// a single keyframe without "h" and a last keyframe with "i" make rlottie
// return zero values; valueAt() returns the keyframe value there and
// Validate() reports them as IssueType::BrokenKeyframes (auto-fixable).
// Operations never write such keyframes: the last keyframe of a property
// never has "i" / "o", a property with one keyframe gets "h": 1.
//
// Threads. Json::Value, Document, all free functions (operations,
// Validate(), AutoFix()) are pure and thread safe. EditorController and the
// localized helpers in oblivion_lottie_editor.h are main thread only.
namespace Oblivion::LottieEdit {

using NodeId = uint64;

namespace Json {

class Value;
struct Member;

class Value final {
public:
	enum class Type : uchar {
		Null,
		Bool,
		Number,
		String,
		Array,
		Object,
	};

	Value() = default;

	[[nodiscard]] static Value FromBool(bool value);
	[[nodiscard]] static Value FromNumber(double value);
	[[nodiscard]] static Value FromString(const QString &value);
	[[nodiscard]] static Value FromNumbers(const std::vector<double> &values);
	[[nodiscard]] static Value FromArray(std::vector<Value> items = {});

	// Assigns a fresh NodeId.
	[[nodiscard]] static Value FromObject();
	[[nodiscard]] static Value FromObject(std::vector<Member> members);

	[[nodiscard]] Type type() const {
		return _type;
	}
	[[nodiscard]] bool isNull() const {
		return (_type == Type::Null);
	}
	[[nodiscard]] bool isBool() const {
		return (_type == Type::Bool);
	}
	[[nodiscard]] bool isNumber() const {
		return (_type == Type::Number);
	}
	[[nodiscard]] bool isString() const {
		return (_type == Type::String);
	}
	[[nodiscard]] bool isArray() const {
		return (_type == Type::Array);
	}
	[[nodiscard]] bool isObject() const {
		return (_type == Type::Object);
	}

	// Number (Bool as 0 / 1), fallback otherwise.
	[[nodiscard]] double toDouble(double fallback = 0.) const;

	// Like toDouble(), also takes the first item of a number array.
	[[nodiscard]] double toNumber(double fallback = 0.) const;

	[[nodiscard]] int toInt(int fallback = 0) const;
	[[nodiscard]] bool toBool(bool fallback = false) const;
	[[nodiscard]] QString toString() const;

	// Number -> { n }, array -> items (non-numbers read as 0), else empty.
	[[nodiscard]] std::vector<double> numbers() const;

	// Items of an array / members of an object, 0 otherwise.
	[[nodiscard]] int size() const;
	[[nodiscard]] const std::vector<Value> &items() const;
	[[nodiscard]] const std::vector<Member> &members() const;

	// Null value if missing.
	[[nodiscard]] const Value &at(int index) const;
	[[nodiscard]] const Value &get(QByteArrayView key) const;
	[[nodiscard]] const Value &operator[](QByteArrayView key) const {
		return get(key);
	}
	[[nodiscard]] bool has(QByteArrayView key) const;
	[[nodiscard]] int indexOf(QByteArrayView key) const;

	// Object NodeId, 0 for other types.
	[[nodiscard]] NodeId id() const;

	// Persistent modifications, the original stays unchanged. The copy of
	// an object keeps its NodeId. Null is treated as an empty object /
	// array by the member / item setters.
	//
	// with(): replaces the member in place or appends it.
	// withInserted(): replaces in place or inserts at position.
	[[nodiscard]] Value with(QByteArrayView key, Value value) const;
	[[nodiscard]] Value withInserted(
		QByteArrayView key,
		Value value,
		int position) const;
	[[nodiscard]] Value without(QByteArrayView key) const;
	[[nodiscard]] Value withItem(int index, Value value) const;
	[[nodiscard]] Value withInsertedItem(int index, Value value) const;
	[[nodiscard]] Value withoutItem(int index) const;

	// Deep copy with fresh NodeIds for every object inside.
	[[nodiscard]] Value withNewIds() const;

	// Same tree (pointer identity for strings / arrays / objects).
	[[nodiscard]] bool sameAs(const Value &other) const;

	// Deep equality, object keys compared in order, ids ignored.
	friend bool operator==(const Value &a, const Value &b);

private:
	struct Heap;

	Type _type = Type::Null;
	bool _bool = false;
	double _number = 0.;
	std::shared_ptr<const Heap> _heap;

};

struct Member {
	QByteArray key;
	Value value;
};

// Strict JSON (UTF-8, optional BOM). Empty optional on error.
[[nodiscard]] std::optional<Value> Parse(
	QByteArrayView json,
	QString *error = nullptr);

// Compact JSON. Numbers use the shortest round-trip form.
[[nodiscard]] QByteArray Serialize(const Value &value);

} // namespace Json

enum class NodeKind : uchar {
	Composition,
	Asset,
	Layer,
	Shape,
	Mask,
	Effect,
};

enum class LayerType : uchar {
	Precomp, // "ty": 0
	Solid, // 1
	Image, // 2
	Null, // 3
	Shape, // 4
	Text, // 5
	Unknown,
};

enum class ShapeType : uchar {
	Group, // "gr"
	Rectangle, // "rc"
	Ellipse, // "el"
	Star, // "sr"
	Path, // "sh"
	Fill, // "fl"
	Stroke, // "st"
	GradientFill, // "gf"
	GradientStroke, // "gs"
	Transform, // "tr"
	TrimPaths, // "tm"
	Repeater, // "rp"
	MergePaths, // "mm"
	RoundCorners, // "rd"
	OffsetPath, // "op"
	PuckerBloat, // "pb"
	Twist, // "tw"
	ZigZag, // "zz"
	Unknown,
};

enum class MatteMode : uchar {
	None,
	Alpha, // "tt": 1
	AlphaInverted, // 2
	Luma, // 3
	LumaInverted, // 4
};

struct NodeInfo {
	NodeId id = 0;
	NodeKind kind = NodeKind::Composition;

	// Composition / asset for layers, layer or group for shape items,
	// layer for masks and effects, 0 for the composition.
	NodeId parent = 0;

	QString name; // "nm", may be empty (see NodeDisplayName()).
	bool hidden = false; // "hd".
	int index = 0; // Index in the parent's JSON array.
	int depth = 0; // 0 for the composition.

	// Composition / precomp asset: layers, first is drawn on top.
	// Layer / group: shape items, first is drawn on top, the group
	// "tr" transform item is included (usually the last one).
	std::vector<NodeId> children;
	std::vector<NodeId> masks; // Layer.
	std::vector<NodeId> effects; // Layer.
	std::vector<NodeId> assets; // Composition.

	NodeId layer = 0; // Owning layer for shapes / masks / effects.
	NodeId composition = 0; // Owning composition / asset for layers.

	// Layer.
	LayerType layerType = LayerType::Unknown;
	std::optional<int> ind; // "ind".
	std::optional<int> parentInd; // "parent".
	NodeId parentLayer = 0; // Resolved "parent", 0 if none / missing.
	NodeId precomp = 0; // Precomp layer: the referenced asset.
	QString refId; // Precomp layer "refId", asset "id".
	double inPoint = 0.; // "ip".
	double outPoint = 0.; // "op".
	double startTime = 0.; // "st".
	double stretch = 1.; // "sr".
	MatteMode matte = MatteMode::None; // "tt", matted by the layer above.
	bool matteSource = false; // "td", used as the matte of the next one.
	bool is3d = false; // "ddd": 1.
	bool hasTimeRemap = false; // "tm".

	// Shape item.
	ShapeType shapeType = ShapeType::Unknown;
	QByteArray typeCode; // Raw "ty" of shape items ("gr", "fl", ...).
	NodeId transform = 0; // Group: its "tr" item.

	// Asset.
	bool imageAsset = false; // Has "p" (image / external file).
};

struct PropertyRef {
	NodeId node = 0;
	QByteArray path;

	[[nodiscard]] bool valid() const {
		return node && !path.isEmpty();
	}
	explicit operator bool() const {
		return valid();
	}
	friend inline bool operator==(
		const PropertyRef &a,
		const PropertyRef &b) = default;
	friend inline bool operator<(
			const PropertyRef &a,
			const PropertyRef &b) {
		return (a.node < b.node)
			|| ((a.node == b.node) && (a.path < b.path));
	}
};

// A keyframe is identified by its time, times are unique in a property.
struct KeyframeRef {
	PropertyRef property;
	double time = 0.;

	friend inline bool operator==(
		const KeyframeRef &a,
		const KeyframeRef &b) = default;
};

enum class PropertyType : uchar {
	Scalar, // Opacity, rotation, stroke width... numbers = { value }.
	Vector, // Position, anchor, scale, size... numbers = { x, y[, z] }.
	Color, // numbers = { r, g, b[, a] } in [0, 1].
	Gradient, // numbers = { offset, r, g, b, ... [, offset, alpha, ...] }.
	Path, // Bezier path (shape "ks", mask "pt").
};

enum class PropertyRole : uchar {
	Anchor,
	Position,
	PositionX,
	PositionY,
	PositionZ,
	Scale,
	Rotation,
	RotationX,
	RotationY,
	Opacity,
	Skew,
	SkewAxis,
	Color,
	StrokeWidth,
	Size,
	Roundness,
	StartPoint,
	EndPoint,
	Gradient,
	HighlightLength,
	HighlightAngle,
	Path,
	TrimStart,
	TrimEnd,
	TrimOffset,
	Points,
	InnerRadius,
	OuterRadius,
	InnerRoundness,
	OuterRoundness,
	Copies,
	Offset,
	StartOpacity,
	EndOpacity,
	MaskPath,
	MaskOpacity,
	MaskExpansion,
	TimeRemap,
	EffectValue,
	Dash,
	Radius,
	Amount,
	Other,
};

struct PathData {
	std::vector<QPointF> vertices; // "v"
	std::vector<QPointF> inTangents; // "i", relative to the vertices.
	std::vector<QPointF> outTangents; // "o", relative to the vertices.
	bool closed = false; // "c"

	friend inline bool operator==(
		const PathData &a,
		const PathData &b) = default;
};

struct PropValue {
	std::vector<double> numbers; // All types except Path.
	std::optional<PathData> path; // Path.

	[[nodiscard]] static PropValue Scalar(double value);
	[[nodiscard]] static PropValue Point(QPointF value);
	[[nodiscard]] static PropValue Color(const QColor &color); // r, g, b, 1.
	[[nodiscard]] static PropValue Path(PathData path);

	[[nodiscard]] double scalar(double fallback = 0.) const;
	[[nodiscard]] QPointF point() const;
	[[nodiscard]] QColor color() const; // Opaque, ignores a 4th number.
	[[nodiscard]] bool empty() const {
		return numbers.empty() && !path;
	}

	friend inline bool operator==(
		const PropValue &a,
		const PropValue &b) = default;
};

struct GradientStop {
	double offset = 0.; // [0, 1]
	QColor color; // Opaque color of a color stop, alpha of an opacity stop.
};

// colorStops is "g"."p". Opacity stops (if any) follow the color stops,
// their QColor has the opacity in alpha and black rgb.
[[nodiscard]] std::vector<GradientStop> ColorStops(
	const PropValue &gradient,
	int colorStops);
[[nodiscard]] std::vector<GradientStop> OpacityStops(
	const PropValue &gradient,
	int colorStops);

enum class EasingPreset : uchar {
	Linear,
	EaseIn, // Starts slowly (accelerates), CSS ease-in.
	EaseOut, // Ends slowly (decelerates), CSS ease-out.
	EaseInOut,
	Hold, // Keeps the value until the next keyframe ("h": 1).
	Custom,
};

// Easing of the segment that starts at a keyframe and ends at the next
// one: out = "o" (first bezier control point, the keyframe's own), in =
// "i" (second control point). x in [0, 1], y may overshoot.
struct Easing {
	QPointF out = QPointF(0., 0.);
	QPointF in = QPointF(1., 1.);
	bool hold = false;

	[[nodiscard]] static Easing Linear();
	[[nodiscard]] static Easing EaseIn(); // out (0.42, 0), in (1, 1)
	[[nodiscard]] static Easing EaseOut(); // out (0, 0), in (0.58, 1)
	[[nodiscard]] static Easing EaseInOut(); // out (0.42, 0), in (0.58, 1)
	[[nodiscard]] static Easing Hold();
	[[nodiscard]] static Easing FromPreset(EasingPreset preset);

	// Classifies by shape: a linear side has x == y, "in" of a preset
	// is (1, 1) / (x < 1, 1), "out" is (0, 0) / (x > 0, 0).
	[[nodiscard]] EasingPreset preset() const;

	// Eased progress for linear progress x in [0, 1], 0 for hold.
	[[nodiscard]] double apply(double x) const;

	friend inline bool operator==(
		const Easing &a,
		const Easing &b) = default;
};

struct Keyframe {
	double time = 0.; // "t".
	PropValue value; // "s" (or the previous "e" in old exports).
	Easing easing; // Segment to the next keyframe, unused for the last.
	bool last = false;

	// Spatial tangents ("to" / "ti") of the segment to the next keyframe,
	// relative to this / the next value, empty if the property is not
	// spatial.
	std::vector<double> outTangent;
	std::vector<double> inTangent;
};

struct PropertyInfo {
	PropertyRef ref;
	PropertyRole role = PropertyRole::Other;
	PropertyType type = PropertyType::Scalar;
	QString name; // Effect value / dash names, empty otherwise.
	int dimensions = 1; // Numbers in the value (Scalar 1, Vector 2-3...).
	int colorStops = 0; // Gradient: "g"."p".
	bool animated = false;
	bool spatial = false; // Has "ti" / "to" keyframe tangents.
	bool expression = false; // Has an expression ("x"), not rendered.
	int keyframes = 0;
};

enum class ColorKind : uchar {
	Fill,
	Stroke,
	GradientFill,
	GradientStroke,
	Solid, // Solid layer "sc" ("#rrggbb" string, not animatable).
	Effect, // Color control / fill / tint effect values.
	TextFill,
	TextStroke,
};

// One stored color value.
struct ColorOccurrence {
	// Fill / stroke: { shape, "c" }, gradients: { shape, "g.k" },
	// solid: { layer, "sc" }, effect: { effect, "ef.N.v" },
	// text: { layer, "t.d" }.
	PropertyRef property;
	ColorKind kind = ColorKind::Fill;
	int keyframe = -1; // -1 for a static value, text document index.
	bool end = false; // The keyframe "e" value of old exports.
	int stop = -1; // Gradient color stop index, -1 for other kinds.
	QColor color; // Opaque.

	friend inline bool operator==(
		const ColorOccurrence &a,
		const ColorOccurrence &b) = default;
};

struct PaletteEntry {
	QColor color; // Opaque, 8 bit per channel.
	std::vector<ColorOccurrence> occurrences;
};

class Document final {
public:
	// Invalid (valid() == false) document.
	Document();

	// Takes ownership of the tree and indexes it. Duplicate NodeIds inside
	// the tree are replaced with fresh ones.
	explicit Document(Json::Value root);

	// Plain Lottie JSON. Invalid document (and the reason) on error.
	[[nodiscard]] static Document FromJson(
		QByteArrayView json,
		QString *error = nullptr);

	// .tgs or JSON bytes, through Oblivion::Lottie::Unpack().
	[[nodiscard]] static Document FromData(
		const QByteArray &data,
		QString *error = nullptr);

	// Empty composition: "v" 5.5.2, "tgs": 1, no layers.
	[[nodiscard]] static Document Blank(
		QSize size = QSize(512, 512),
		double fps = 60.,
		int frames = 180);

	// Root is an object with a "layers" array.
	[[nodiscard]] bool valid() const;
	explicit operator bool() const {
		return valid();
	}

	[[nodiscard]] const Json::Value &root() const;
	[[nodiscard]] bool sameAs(const Document &other) const;

	// Compact JSON, serialized once per document (thread safe), so every
	// panel can call it freely.
	[[nodiscard]] QByteArray toJson() const;
	[[nodiscard]] QByteArray toTgs() const; // Oblivion::Lottie::PackTgs().

	// Composition. frames() == int64(op) - int64(ip) like rlottie.
	[[nodiscard]] QSize size() const;
	[[nodiscard]] double frameRate() const;
	[[nodiscard]] double inPoint() const;
	[[nodiscard]] double outPoint() const;
	[[nodiscard]] int frames() const;
	[[nodiscard]] QString name() const;

	// Nodes. nodes() is in depth-first order: the composition, root layers
	// with their masks, effects and shapes, then assets with their layers.
	[[nodiscard]] NodeId rootId() const;
	[[nodiscard]] const std::vector<NodeInfo> &nodes() const;
	[[nodiscard]] const NodeInfo *node(NodeId id) const;
	[[nodiscard]] bool contains(NodeId id) const;
	[[nodiscard]] const Json::Value &json(NodeId id) const;

	// Walks "parent" / owning links up to the composition.
	[[nodiscard]] bool isDescendant(NodeId id, NodeId ancestor) const;

	// Layers (in order) of the root or of a precomp asset.
	[[nodiscard]] std::vector<NodeId> layers(NodeId composition = 0) const;

	// The layer that owns a node (the node itself for layers), 0 if none.
	[[nodiscard]] NodeId owningLayer(NodeId id) const;

	// Precomp layers that reference the asset.
	[[nodiscard]] std::vector<NodeId> assetUsers(NodeId asset) const;

	// Maps a root composition frame to the time base of the node (the
	// same frame for root layers and their content, precomp local time
	// through the first precomp layer that references the asset: "st",
	// "sr" and time remapping like rlottie).
	[[nodiscard]] double localFrame(NodeId id, double rootFrame) const;

	// [ip, op) of a layer mapped to root composition frames.
	[[nodiscard]] std::pair<double, double> layerRangeInRoot(
		NodeId layer) const;

	// Geometry at a root composition frame, in canvas coordinates
	// (0, 0 - w, h), computed like rlottie: translate(p) * rotate(r, or rz
	// of 3D layers) * scale(s / 100) * translate(-a), group "tr" items,
	// "parent" chains and precomp layers (skew, 3D x / y rotation and
	// auto-orient are ignored, like rlottie ignores skew).
	//
	// transformAt(): maps the node's content coordinates to the canvas:
	// the layer's own space for layers / masks / effects, the group
	// content for groups, the containing group / layer content for other
	// shape items. outlineAt(): approximate outline for selection and hit
	// testing (Qt::WindingFill): rectangles (with roundness), ellipses,
	// paths, stars / polygons (without roundness), groups and shape layers
	// as the union of their geometry, solids as their rectangle, precomp
	// layers as the union of their content, masks as the mask path.
	// Strokes, trims, repeaters and other modifiers are not applied.
	[[nodiscard]] QTransform transformAt(NodeId id, double rootFrame) const;
	[[nodiscard]] QPainterPath outlineAt(NodeId id, double rootFrame) const;

	// Properties. properties() of a group lists its "tr" item properties
	// (their refs point to the "tr" node).
	[[nodiscard]] std::vector<PropertyInfo> properties(NodeId id) const;
	[[nodiscard]] std::optional<PropertyInfo> property(
		const PropertyRef &ref) const;
	[[nodiscard]] const Json::Value &propertyJson(
		const PropertyRef &ref) const;
	[[nodiscard]] bool animated(const PropertyRef &ref) const;
	[[nodiscard]] std::vector<Keyframe> keyframes(
		const PropertyRef &ref) const;
	[[nodiscard]] std::vector<double> keyframeTimes(
		const PropertyRef &ref) const;

	// Value at a frame of the property's time base (see localFrame()).
	[[nodiscard]] std::optional<PropValue> valueAt(
		const PropertyRef &ref,
		double frame) const;

	// Static value / first keyframe value.
	[[nodiscard]] std::optional<PropValue> baseValue(
		const PropertyRef &ref) const;

	// Every animated property of the subtree (all when id == 0).
	[[nodiscard]] std::vector<PropertyRef> animatedProperties(
		NodeId id = 0) const;

	// Colors. Occurrences in document order, hidden nodes included.
	// scope limits the search to these nodes and their descendants.
	[[nodiscard]] std::vector<ColorOccurrence> colorOccurrences(
		const std::vector<NodeId> &scope = {}) const;

	// Unique 8 bit colors, the most used first.
	[[nodiscard]] std::vector<PaletteEntry> palette(
		const std::vector<NodeId> &scope = {}) const;

private:
	struct Data;
	friend struct DocumentAccess;

	std::shared_ptr<const Data> _data;

};

// Result of an operation. Operations are pure: they return a new document
// and never modify the given one. If nothing had to change the returned
// document is the same (sameAs()) as the input.
struct Edit {
	Document document; // Invalid if the operation failed.
	std::vector<NodeId> changed; // Nodes whose own JSON changed.
	std::vector<NodeId> created; // New nodes (duplicates, added).
	std::vector<NodeId> removed; // Deleted nodes.
	bool structural = false; // Nodes were added, removed or moved.
	QString error; // Technical reason (English, for logs) on failure.

	[[nodiscard]] bool ok() const {
		return document.valid();
	}
	explicit operator bool() const {
		return ok();
	}
};

// Values and keyframes. Numbers written by operations are rounded to
// 4 decimals, keyframe times to 3. A value with fewer numbers than the
// stored one keeps the stored tail (z of a 3D point, alpha of a color).

// Fails for animated properties (use SetAnimated() or SetValueAt()).
[[nodiscard]] Edit SetStaticValue(
	const Document &document,
	const PropertyRef &ref,
	const PropValue &value);

// Auto-keyframe: static properties get the static value, animated ones
// get a keyframe at frame (updated if one exists there, otherwise
// inserted with the easing of the segment it splits).
[[nodiscard]] Edit SetValueAt(
	const Document &document,
	const PropertyRef &ref,
	const PropValue &value,
	double frame);

// animated == true: one keyframe at frame with the current value.
// animated == false: static with the value at frame.
[[nodiscard]] Edit SetAnimated(
	const Document &document,
	const PropertyRef &ref,
	bool animated,
	double frame);

// Makes a static property animated. value defaults to the value at time,
// easing to the easing of the segment the keyframe splits (linear if
// none). Replaces a keyframe at the same time.
[[nodiscard]] Edit AddKeyframe(
	const Document &document,
	const PropertyRef &ref,
	double time,
	std::optional<PropValue> value = std::nullopt,
	std::optional<Easing> easing = std::nullopt);

[[nodiscard]] Edit SetKeyframeValue(
	const Document &document,
	const KeyframeRef &keyframe,
	const PropValue &value);

// Ignored for the last keyframe of a property.
[[nodiscard]] Edit SetKeyframeEasing(
	const Document &document,
	const std::vector<KeyframeRef> &keyframes,
	const Easing &easing);

// A property that loses all keyframes becomes static with the value of
// its first removed keyframe.
[[nodiscard]] Edit RemoveKeyframes(
	const Document &document,
	const std::vector<KeyframeRef> &keyframes);

// Moves by delta frames, keyframes that are not moved and end up at the
// same time as a moved one are replaced.
[[nodiscard]] Edit MoveKeyframes(
	const Document &document,
	const std::vector<KeyframeRef> &keyframes,
	double delta);

// Nodes.
[[nodiscard]] Edit Rename(
	const Document &document,
	NodeId id,
	const QString &name);

// Layers and shape items ("hd").
[[nodiscard]] Edit SetHidden(
	const Document &document,
	const std::vector<NodeId> &ids,
	bool hidden);

// Layers, shape items (except group "tr"), masks, effects and unused
// assets. Children of a deleted layer are re-parented to its parent,
// a layer matted by a deleted matte source loses "tt", the orphaned
// matte source of a deleted matted layer gets hidden.
[[nodiscard]] Edit DeleteNodes(
	const Document &document,
	const std::vector<NodeId> &ids);

// Copies are inserted above (before) the originals with new ids and new
// "ind" values. A layer in a track matte pair is duplicated together with
// its pair. Edit::created has the copies.
[[nodiscard]] Edit DuplicateNodes(
	const Document &document,
	const std::vector<NodeId> &ids);

// Moves a layer inside its composition or a shape item into a layer /
// group (container), so that it ends up before the item that was at
// index (index == count means the end; a group "tr" stays last). Track
// matte pairs move together and are never split.
[[nodiscard]] Edit MoveNode(
	const Document &document,
	NodeId id,
	NodeId container,
	int index);

// delta < 0 moves up (towards the top of the stack).
[[nodiscard]] Edit ReorderNode(
	const Document &document,
	NodeId id,
	int delta);

enum class LayerTemplate : uchar {
	Shape,
	Null,
};

enum class ShapeTemplate : uchar {
	Group, // Empty group with a transform.
	Rectangle, // Group: rectangle + fill + transform.
	Ellipse, // Group: ellipse + fill + transform.
	Star, // Group: star + fill + transform.
	Fill,
	Stroke,
	GradientFill,
	TrimPaths,
};

// New layer at index (0 = top) of the composition (0 = root), covering
// the whole composition time, centered on the canvas. content adds a
// shape template to a shape layer.
[[nodiscard]] Edit AddLayer(
	const Document &document,
	LayerTemplate type,
	const QString &name,
	NodeId composition = 0,
	int index = 0,
	std::optional<ShapeTemplate> content = std::nullopt);

// New shape item in a shape layer / group. index -1: paths and groups go
// to the top, fills / strokes / trims before the group transform.
[[nodiscard]] Edit AddShape(
	const Document &document,
	NodeId container,
	ShapeTemplate type,
	const QString &name,
	int index = -1,
	QColor color = QColor(64, 140, 255));

// Layer timing in its composition time base, outPoint > inPoint.
[[nodiscard]] Edit SetLayerTiming(
	const Document &document,
	NodeId layer,
	double inPoint,
	double outPoint);

// Moves layers in time: "ip" / "op" and every keyframe of the layer
// (precomp layers: "ip" / "op" / "st").
[[nodiscard]] Edit ShiftLayers(
	const Document &document,
	const std::vector<NodeId> &layers,
	double delta);

enum class TransformField : uchar {
	Anchor,
	Position, // Writes "p.x" / "p.y" for separated positions.
	Scale, // Percent, { 100, 100 } is identity.
	Rotation, // Degrees ("r", or "rz" of 3D layers).
	Opacity, // Percent.
	Skew,
	SkewAxis,
};

// Transform of a layer ("ks"), a group (its "tr" item) or a "tr" item,
// auto-keyframed like SetValueAt().
[[nodiscard]] std::optional<PropertyRef> TransformProperty(
	const Document &document,
	NodeId id,
	TransformField field);
[[nodiscard]] Edit SetTransformAt(
	const Document &document,
	NodeId id,
	TransformField field,
	const PropValue &value,
	double frame);

// Colors. Matching is done on 8 bit rgb, alpha / opacity is kept.
[[nodiscard]] Edit ReplaceColor(
	const Document &document,
	const QColor &from,
	const QColor &to,
	const std::vector<NodeId> &scope = {});
[[nodiscard]] Edit SetColor(
	const Document &document,
	const ColorOccurrence &occurrence,
	const QColor &color);

// HSL adjustment of every color occurrence (see colorOccurrences()) in
// the scope, with the semantics of Oblivion::Lottie::AdjustColors():
// hue rotates (degrees, wraps), saturation / lightness in [-100, 100]
// move towards 0 (negative) or 1 (positive).
[[nodiscard]] Edit AdjustHsl(
	const Document &document,
	int hueDegrees,
	int saturationPercent,
	int lightnessPercent,
	const std::vector<NodeId> &scope = {});

// Composition. SetCanvasSize with scaleContent fits the old canvas into
// the new one (uniform scale, centered) by transforming the root layers
// that have no parent; without it the content is centered unscaled.
[[nodiscard]] Edit SetCanvasSize(
	const Document &document,
	QSize size,
	bool scaleContent);

// retime: keep the duration in seconds (every frame time is scaled by
// new / old fps), otherwise only "fr" changes (plays faster / slower).
[[nodiscard]] Edit SetFrameRate(
	const Document &document,
	double fps,
	bool retime);

// "op" = "ip" + frames. When extending, layers that lasted till the old
// end are extended too.
[[nodiscard]] Edit SetDuration(const Document &document, int frames);

// factor > 1 plays faster: every frame time is divided by factor
// (time remapping values too), "fr" stays.
[[nodiscard]] Edit ChangeSpeed(const Document &document, double factor);

// Sets "ip" / "op" of the composition to [from, to). rebase shifts all
// frame times so that the result starts at 0.
[[nodiscard]] Edit TrimRange(
	const Document &document,
	double from,
	double to,
	bool rebase);

struct OptimizeOptions {
	int decimals = 3;
	bool stripShapeNames = false; // "nm" of shape items / properties.
	bool stripLayerNames = false;
};

// Rounds numbers, drops metadata keys ("mn", "ix", "cix", "np", "cl",
// "ln", "meta") and default values ("hd": false, "bm": 0, "ao": 0).
[[nodiscard]] Edit Optimize(
	const Document &document,
	const OptimizeOptions &options = {});

// Telegram animated sticker (.tgs) requirements.
inline constexpr auto kTgsCanvasSize = 512;
inline constexpr auto kTgsMaxDuration = 3.; // Seconds.
inline constexpr auto kTgsMaxPackedSize = 64 * 1024; // Gzipped bytes.

enum class IssueSeverity : uchar {
	Error, // Telegram rejects it or its renderer shows it wrong.
	Warning, // Forbidden by Telegram's sticker guidelines.
};

enum class IssueType : uchar {
	// Errors.
	InvalidComposition, // Bad "w" / "h" / "fr" / "ip" / "op".
	MissingVersion, // No "v", rlottie refuses. Fix: add "v".
	CanvasSize, // Not 512x512. Fix: resize with scaling.
	FrameRate, // Not 30 / 60. Fix: retime to 30 or 60.
	Duration, // Longer than 3 s. Fix: trim to 3 s.
	FileSize, // Gzipped > 64 KB. Fix: optimize (may not be enough).
	Images, // Image layers / image or external assets. Fix: remove.
	Expressions, // "x" expressions. Fix: remove (keeps base values).
	Layers3D, // "ddd": 1. Fix: flatten (keeps Z rotation).
	TextLayers, // Fix: remove.
	BrokenKeyframes, // Wrong in rlottie. Fix: normalize keyframes.

	// Warnings.
	MissingTgsMarker, // No "tgs": 1. Fix: add.
	Masks,
	Effects, // Ignored by rlottie. Fix: remove.
	Solids, // Fix: convert to shape layers (same look).
	TimeStretch,
	TimeRemap,
	MergePaths, // Ignored by rlottie. Fix: remove.
	UnsupportedShapes, // Offset path, pucker, twist, zig zag, round
		// corners: ignored by rlottie. Fix: remove.
	Repeaters,
	StarShapes,
	GradientStrokes,
	OutOfCanvas, // Content reaches the canvas edge (render check).
	AutoOrient, // Auto-oriented layers ("ao": 1). Fix: turn it off.
};

struct Issue {
	IssueType type = IssueType::InvalidComposition;
	IssueSeverity severity = IssueSeverity::Error;
	std::vector<NodeId> nodes; // Offending nodes, may be empty.
	bool fixable = false;

	// Measured value: canvas width (CanvasSize), fps (FrameRate), seconds
	// (Duration), gzipped bytes (FileSize), 0 otherwise.
	double value = 0.;
};

struct ValidateOptions {
	bool measureSize = true; // Gzip the document (a few ms).
	bool renderChecks = false; // Render frames for OutOfCanvas (slow).
};

struct ValidationResult {
	std::vector<Issue> issues; // Errors first.
	int64 packedSize = -1; // Gzipped size, -1 if not measured.
	bool packedSizeEstimated = false; // PackTgs() unavailable, zlib estimate.

	[[nodiscard]] bool ok() const; // No errors.
	[[nodiscard]] bool hasFixable() const;
};

[[nodiscard]] ValidationResult Validate(
	const Document &document,
	const ValidateOptions &options = {});

// Applies the fixes of the given issue types (every fixable type when
// empty), in a safe order, as one edit.
[[nodiscard]] Edit AutoFix(
	const Document &document,
	const std::vector<IssueType> &types = {});

enum class Command : uchar {
	Unknown,
	SetValue,
	ToggleAnimated,
	AddKeyframe,
	ChangeKeyframe,
	SetEasing,
	RemoveKeyframes,
	MoveKeyframes,
	Rename,
	SetHidden,
	Delete,
	Duplicate,
	Move,
	AddLayer,
	AddShape,
	LayerTiming,
	ReplaceColor,
	Recolor,
	CanvasSize,
	FrameRate,
	Duration,
	Speed,
	Trim,
	AutoFix,
	Optimize,
};

enum class ChangeSource : uchar {
	Edit,
	Undo,
	Redo,
	Load,
};

struct DocumentChange {
	std::vector<NodeId> nodes; // Changed + created + removed nodes.
	bool structural = false; // The node tree changed.
	ChangeSource source = ChangeSource::Edit;
	Command command = Command::Unknown;
};

enum class SelectMode : uchar {
	Replace,
	Add,
	Toggle,
};

// Window-level actions panels can ask for, handled by EditorWindow
// (ignored without a window, for example in snapshot scenes).
enum class EditorAction : uchar {
	New, // New blank animation (in a new window if this one is dirty).
	Open,
	Save,
	SaveAs,
	ExportTgs,
	ExportJson,
	Close,
};

// The single source of truth of an editor window: the document with its
// undo history, the selection, the current frame, playback and the view.
// Main thread only. Every signal fires after the state has changed.
class EditorController final : public base::has_weak_ptr {
public:
	EditorController();
	explicit EditorController(
		Document document,
		QString name = QString(),
		QString path = QString());
	~EditorController();

	// Document.
	[[nodiscard]] const Document &document() const;

	// Replaces the document, clears the history, the selection and the
	// dirty flag, moves to the first frame, stops playback.
	void load(Document document, QString name, QString path = QString());

	[[nodiscard]] QString name() const; // Without an extension.
	[[nodiscard]] rpl::producer<QString> nameValue() const;
	void setName(QString name);
	[[nodiscard]] QString filePath() const; // Empty if never saved.
	void setFilePath(QString path);

	[[nodiscard]] bool dirty() const;
	[[nodiscard]] rpl::producer<bool> dirtyValue() const;
	void markSaved();

	[[nodiscard]] rpl::producer<DocumentChange> documentChanged() const;

	// Applies an edit as one undoable step. Returns false (and changes
	// nothing) if the edit failed or changed nothing.
	//
	// Edits with the same non-empty mergeKey that follow each other merge
	// into one undo step (dragging a slider, typing digits) until
	// finishMerge(), another command, undo / redo or 1.5 s pause.
	bool perform(
		Command command,
		Edit &&edit,
		QByteArray mergeKey = QByteArray());
	void finishMerge();

	// A gesture (a canvas / timeline drag) with a mergeKey unique to it:
	// its edits merge into one undo step whatever the pauses, until
	// finishMerge() or cancelGesture(). cancelGesture() drops every
	// trailing step made with the key (not redoable) and goes back to the
	// document before them, false if there was no such step.
	void beginGesture(QByteArray mergeKey);
	bool cancelGesture(const QByteArray &mergeKey);

	[[nodiscard]] bool canUndo() const;
	[[nodiscard]] bool canRedo() const;
	[[nodiscard]] Command undoCommand() const;
	[[nodiscard]] Command redoCommand() const;
	bool undo();
	bool redo();
	void clearHistory();
	[[nodiscard]] rpl::producer<bool> undoAvailable() const;
	[[nodiscard]] rpl::producer<bool> redoAvailable() const;

	// Operation shortcuts: perform(Command::..., Op(document(), ...)).
	// Frames default to the current frame mapped into the property's
	// time base. Selection follows the result (duplicates / added nodes
	// get selected, moved keyframes stay selected).
	bool setValue(
		const PropertyRef &ref,
		const PropValue &value,
		QByteArray mergeKey = QByteArray());
	bool setStaticValue(
		const PropertyRef &ref,
		const PropValue &value,
		QByteArray mergeKey = QByteArray());
	bool setAnimated(const PropertyRef &ref, bool animated);
	bool addKeyframe(
		const PropertyRef &ref,
		std::optional<double> time = std::nullopt,
		std::optional<PropValue> value = std::nullopt,
		std::optional<Easing> easing = std::nullopt);
	bool setKeyframeValue(
		const KeyframeRef &keyframe,
		const PropValue &value,
		QByteArray mergeKey = QByteArray());
	bool setKeyframeEasing(
		const std::vector<KeyframeRef> &keyframes,
		const Easing &easing);
	bool removeKeyframes(const std::vector<KeyframeRef> &keyframes);
	bool moveKeyframes(
		const std::vector<KeyframeRef> &keyframes,
		double delta,
		QByteArray mergeKey = QByteArray());
	bool setTransform(
		NodeId id,
		TransformField field,
		const PropValue &value,
		QByteArray mergeKey = QByteArray());
	bool rename(NodeId id, const QString &name);
	bool setHidden(const std::vector<NodeId> &ids, bool hidden);
	bool deleteNodes(const std::vector<NodeId> &ids);
	bool duplicateNodes(const std::vector<NodeId> &ids);
	bool moveNode(NodeId id, NodeId container, int index);
	bool reorderNode(NodeId id, int delta);
	bool addLayer(
		LayerTemplate type,
		const QString &name,
		std::optional<ShapeTemplate> content = std::nullopt);
	bool addShape(
		NodeId container,
		ShapeTemplate type,
		const QString &name);
	bool setLayerTiming(
		NodeId layer,
		double inPoint,
		double outPoint,
		QByteArray mergeKey = QByteArray());
	bool shiftLayers(
		const std::vector<NodeId> &layers,
		double delta,
		QByteArray mergeKey = QByteArray());
	bool replaceColor(
		const QColor &from,
		const QColor &to,
		const std::vector<NodeId> &scope = {});
	bool setColor(
		const ColorOccurrence &occurrence,
		const QColor &color,
		QByteArray mergeKey = QByteArray());
	bool adjustHsl(
		int hueDegrees,
		int saturationPercent,
		int lightnessPercent,
		const std::vector<NodeId> &scope = {},
		QByteArray mergeKey = QByteArray());
	bool setCanvasSize(QSize size, bool scaleContent);
	bool setFrameRate(double fps, bool retime);
	bool setDuration(int frames);
	bool changeSpeed(double factor);
	bool trimRange(double from, double to, bool rebase);
	bool autoFix(const std::vector<IssueType> &types = {});
	bool optimize(const OptimizeOptions &options = {});

	// Deletes the selected keyframes if any, the selected nodes otherwise.
	bool deleteSelection();
	bool duplicateSelection();

	// Node selection, in selection order (the last one is primary).
	// Removed nodes are dropped from the selection after every change.
	// Changing it drops the selected keyframes and the active property of
	// nodes unrelated to the new selection (neither selected nor an ancestor
	// or a descendant of a selected node).
	[[nodiscard]] const std::vector<NodeId> &selection() const;
	[[nodiscard]] NodeId primarySelection() const;
	[[nodiscard]] bool isSelected(NodeId id) const;
	void setSelection(std::vector<NodeId> ids);
	void select(NodeId id, SelectMode mode = SelectMode::Replace);
	void clearSelection();
	[[nodiscard]] rpl::producer<> selectionChanged() const;

	// The property shown in the timeline graph / focused in the inspector.
	[[nodiscard]] std::optional<PropertyRef> activeProperty() const;
	void setActiveProperty(std::optional<PropertyRef> ref);
	[[nodiscard]] rpl::producer<> activePropertyChanged() const;

	// Selected keyframes (timeline), updated after keyframe moves.
	[[nodiscard]] const std::vector<KeyframeRef> &selectedKeyframes() const;
	void setSelectedKeyframes(std::vector<KeyframeRef> keyframes);
	[[nodiscard]] bool isKeyframeSelected(const KeyframeRef &ref) const;
	[[nodiscard]] rpl::producer<> keyframeSelectionChanged() const;

	// Time: absolute root composition frames, see the top comment.
	[[nodiscard]] int currentFrame() const;
	[[nodiscard]] int frameIndex() const; // currentFrame() - firstFrame().
	[[nodiscard]] int firstFrame() const; // int64(ip)
	[[nodiscard]] int lastFrame() const; // int64(op) - 1
	[[nodiscard]] int frameCount() const;
	[[nodiscard]] double fps() const;
	void setCurrentFrame(int frame); // Clamped.
	void stepFrame(int delta); // Wraps around.
	[[nodiscard]] rpl::producer<int> currentFrameChanged() const;
	[[nodiscard]] rpl::producer<int> currentFrameValue() const;

	// Local time of a node at the current frame.
	[[nodiscard]] double localFrame(NodeId id) const;

	[[nodiscard]] bool playing() const;
	void setPlaying(bool playing);
	void togglePlaying();
	[[nodiscard]] rpl::producer<bool> playingValue() const;
	[[nodiscard]] bool looping() const;
	void setLooping(bool looping);

	// View: zoom is a multiplier over "fit the canvas into the panel"
	// (1.), clamped to [0.1, 32.]; pan is the offset of the canvas center
	// from the panel center in composition pixels.
	[[nodiscard]] double zoom() const;
	void setZoom(double zoom);
	[[nodiscard]] rpl::producer<double> zoomValue() const;
	[[nodiscard]] QPointF pan() const;
	void setPan(QPointF pan);
	[[nodiscard]] rpl::producer<QPointF> panValue() const;

	// Set by the editor window, null without a window (tests).
	void setShow(std::shared_ptr<Ui::Show> show);
	[[nodiscard]] std::shared_ptr<Ui::Show> uiShow() const;
	void requestAction(EditorAction action);
	[[nodiscard]] rpl::producer<EditorAction> actionRequests() const;

	[[nodiscard]] rpl::lifetime &lifetime();

private:
	// Only the trees (shared between the versions): the index and the
	// serialized JSON of a version are rebuilt when undo / redo returns
	// to it, history doesn't keep them for every step.
	struct Step {
		Json::Value before;
		Json::Value after;
		Command command = Command::Unknown;
		QByteArray mergeKey;
		std::vector<NodeId> nodes;
		bool structural = false;
		crl::time updated = 0;
		bool sealed = false;
	};

	void setDocument(
		Document document,
		DocumentChange &&change);
	void pruneSelection();
	void clampFrame();
	void refreshHistoryState();
	void startPlaybackClock();
	void playbackTick();
	[[nodiscard]] bool performWithSelection(
		Command command,
		Edit &&edit,
		QByteArray mergeKey);

	Document _document;
	Document _saved;
	rpl::variable<QString> _name;
	QString _path;
	std::vector<Step> _undo;
	std::vector<Step> _redo;
	QByteArray _gestureKey;
	rpl::event_stream<DocumentChange> _documentChanges;
	rpl::variable<bool> _dirty = false;
	rpl::variable<bool> _canUndo = false;
	rpl::variable<bool> _canRedo = false;

	std::vector<NodeId> _selection;
	rpl::event_stream<> _selectionChanges;
	std::optional<PropertyRef> _activeProperty;
	rpl::event_stream<> _activePropertyChanges;
	std::vector<KeyframeRef> _selectedKeyframes;
	rpl::event_stream<> _keyframeSelectionChanges;

	rpl::variable<int> _frame = 0;
	rpl::variable<bool> _playing = false;
	bool _looping = true;
	base::Timer _playTimer;
	crl::time _playStarted = 0;
	int _playStartFrame = 0;

	rpl::variable<double> _zoom = 1.;
	rpl::variable<QPointF> _pan;

	std::shared_ptr<Ui::Show> _show;
	rpl::event_stream<EditorAction> _actionRequests;

	rpl::lifetime _lifetime;

};

// Self-checks for OBLIVION_SELFTEST=lottie_doc, see oblivion_selftest.h.
// No style / lang / session is used.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::LottieEdit
