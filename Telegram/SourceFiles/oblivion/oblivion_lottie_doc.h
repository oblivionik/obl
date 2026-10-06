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
#include <QtCore/QRectF>
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
//
// After Effects features (round 4) and what Telegram's renderer does with
// them. Checked against the rlottie sources bundled with this app
// (Telegram/ThirdParty/rlottie/src/lottie: lottieparser.cpp, lottieitem.cpp,
// lottiemodel.cpp), the model reads, edits and writes all of them:
//
// - Masks ("masksProperties"): applied only when the layer has
//   "hasMask": true (AddMask() writes it). Modes add / subtract / intersect
//   / difference and the path (static or animated) are rendered, the masks
//   are combined in their order. Not rendered: the inverted flag ("inv"),
//   mask opacity ("o", always 100%), feather ("f"), expansion ("x"); masks
//   with the modes none / lighten / darken are skipped, and a layer with
//   masks none of which gives a shape is not drawn at all.
// - Track mattes: a layer with "tt" is cut by the layer right above it in
//   the layers array, whatever that layer is ("td" and the "tp" link of
//   newer exports are not read). Alpha, inverted alpha, luma and inverted
//   luma are rendered. A matted layer that has no layer above, or whose
//   layer above is matted too, is not drawn at all.
// - Trim paths ("tm"): start / end / offset, both modes, rendered.
// - Repeater ("rp"): copies, offset, anchor, position, scale, rotation,
//   start / end opacity are rendered, it repeats every item above it in
//   the same list. The composite order ("m") is ignored.
// - Gradient fill / stroke ("gf" / "gs"): linear and radial, color and
//   opacity stops, radial highlight length / angle, all rendered.
// - Stroke dashes ("d"): rendered, the values are read by position (dash,
//   gap, ..., offset last), the "n" names are ignored.
// - Rounded corners: the rectangle roundness ("rc"."r") is rendered, the
//   round corners modifier ("rd") is ignored (BakeRoundCorners() turns it
//   into real path geometry).
// - Parenting ("parent" -> "ind" in the same composition): rendered, links
//   to a missing layer and cycles are ignored.
// - Easing: one cubic bezier per keyframe segment and hold, rendered.
//
// Two parser quirks matter for files written by other tools: a shape item
// loses every key that stands before its "ty", and a layer without
// "ddd": 0 before "ks" is read as a 3D layer (its "r" rotation is ignored).
// Everything created here puts those keys first; Validate() reports such
// files as IssueType::KeyOrder (auto-fixable).
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

// Strict JSON (UTF-8, optional BOM). Empty optional on error. Zero bytes
// after the root value are allowed (padding). Two things are not kept the
// way they are written: a key that an object has more than once is kept
// once, with its last value (that is how get() would read it, while the
// renderer has no rule for such objects), and "-0" is read as 0 (it is
// written back as that).
[[nodiscard]] std::optional<Value> Parse(
	QByteArrayView json,
	QString *error = nullptr);

// The root value alone: whatever follows it is not looked at, the way
// rlottie reads a file. rewritten tells that the result is not what the
// bytes say (a repeated key or a "-0", see Parse()).
[[nodiscard]] std::optional<Value> ParseRoot(
	QByteArrayView json,
	bool *rewritten = nullptr);

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

// rlottie draws Add / Subtract / Intersect / Difference and skips the
// other modes; a layer whose masks are all skipped is not drawn at all.
enum class MaskMode : uchar {
	None, // "n": meant to do nothing.
	Add, // "a"
	Subtract, // "s"
	Intersect, // "i"
	Lighten, // "l": the same as Add at 100% opacity.
	Darken, // "d": the same as Intersect at 100% opacity.
	Difference, // "f"
};

enum class GradientType : uchar {
	Linear, // "t": 1
	Radial, // 2
};

enum class TrimMode : uchar {
	Simultaneously, // "m": 1, every path is trimmed on its own.
	Individually, // 2, the paths are trimmed as one long path.
};

enum class LineCap : uchar {
	Butt, // "lc": 1, also when "lc" is missing.
	Round, // 2
	Square, // 3
};

enum class LineJoin : uchar {
	Miter, // "lj": 1, also when "lj" is missing.
	Round, // 2
	Bevel, // 3
};

enum class FillRule : uchar {
	NonZero, // "r": 1, also when "r" is missing.
	EvenOdd, // 2
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

	// Layer, round 4.
	bool masksEnabled = false; // "hasMask": true, masks are drawn only then.

	// Track matte links as the renderer sees them: a layer with matte !=
	// None is cut by the layer right above it. matteLayer is that layer (0
	// if there is none or it is matted itself: then this layer is not drawn
	// at all), matteTarget is the layer right below that this one cuts.
	NodeId matteLayer = 0;
	NodeId matteTarget = 0;
	std::optional<int> matteParentInd; // "tp" of newer exports, not read.

	// Mask.
	MaskMode maskMode = MaskMode::Add; // "mode".
	bool maskInverted = false; // "inv", not rendered by rlottie.

	// Shape items: gradients ("t"), trim paths ("m"), strokes and gradient
	// strokes ("lc", "lj", "ml", "d"), fills and gradient fills ("r").
	GradientType gradientType = GradientType::Linear;
	TrimMode trimMode = TrimMode::Simultaneously;
	LineCap lineCap = LineCap::Butt;
	LineJoin lineJoin = LineJoin::Miter;
	double miterLimit = 0.;
	FillRule fillRule = FillRule::NonZero;
	int dashValues = 0; // Items in "d" (dashes, gaps and the offset).
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
	MaskFeather, // Mask "f", { x, y }, ignored by rlottie.
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

	friend inline bool operator==(
		const GradientStop &a,
		const GradientStop &b) = default;
};

// colorStops is "g"."p". Opacity stops (if any) follow the color stops,
// their QColor has the opacity in alpha and black rgb.
[[nodiscard]] std::vector<GradientStop> ColorStops(
	const PropValue &gradient,
	int colorStops);
[[nodiscard]] std::vector<GradientStop> OpacityStops(
	const PropValue &gradient,
	int colorStops);

// A gradient value ("gf" / "gs" property "g.k") split into its stops.
// colors: offset + opaque color, at least one. alphas: offset + opacity in
// QColor::alphaF() (black rgb), empty for a fully opaque gradient; rlottie
// needs at least two of them, so EncodeGradient() doubles a single one.
struct GradientData {
	std::vector<GradientStop> colors;
	std::vector<GradientStop> alphas;

	[[nodiscard]] bool valid() const {
		return !colors.empty();
	}
	friend inline bool operator==(
		const GradientData &a,
		const GradientData &b) = default;
};

[[nodiscard]] GradientData DecodeGradient(
	const PropValue &gradient,
	int colorStops);

// Stops are sorted by offset (stable), offsets and channels are clamped to
// [0, 1]. The color stop count for "g"."p" is data.colors.size().
[[nodiscard]] PropValue EncodeGradient(const GradientData &data);

// Color with alpha at an offset: linear between the neighbour stops, the
// first / last stop outside of them.
[[nodiscard]] QColor GradientColorAt(const GradientData &data, double offset);

// Path helpers for masks, shape paths and the pen tool. Tangents are
// relative to their vertex. Segment i goes from vertex i to vertex i + 1,
// the last segment of a closed path returns to vertex 0. All functions
// accept paths with missing tangents (read as zero) and bad indices (the
// path is returned unchanged).

// Tangent lists padded / cut to the vertex count.
[[nodiscard]] PathData NormalizedPath(PathData path);

// Closed, clockwise, starts at the top left corner (after its arc).
[[nodiscard]] PathData RectanglePath(const QRectF &rect, double radius = 0.);

// Closed, clockwise, four vertices, starts at the top.
[[nodiscard]] PathData EllipsePath(const QRectF &rect);

[[nodiscard]] QPainterPath PainterPath(const PathData &path);
[[nodiscard]] int PathSegmentCount(const PathData &path);
[[nodiscard]] QPointF PathPointAt(const PathData &path, int segment, double t);

struct PathHit {
	int segment = -1; // -1 for an empty path.
	double t = 0.; // Position inside the segment, [0, 1].
	QPointF point; // The closest point of the path.
	double distance = 0.;
};

// The closest point of the path outline (not of its fill).
[[nodiscard]] PathHit NearestPathPoint(const PathData &path, QPointF point);

// Splits the segment at t, the shape of the path stays the same. The new
// vertex gets the index segment + 1.
[[nodiscard]] PathData WithInsertedVertex(
	const PathData &path,
	int segment,
	double t);

// The neighbours get connected with their remaining tangents.
[[nodiscard]] PathData WithoutVertex(const PathData &path, int index);

// Corner: both tangents zero. Smooth: symmetrical tangents along the line
// between the neighbour vertices, a third of the way to each of them.
[[nodiscard]] bool IsCornerVertex(const PathData &path, int index);
[[nodiscard]] PathData WithCornerVertex(const PathData &path, int index);
[[nodiscard]] PathData WithSmoothVertex(const PathData &path, int index);

// The same outline drawn in the other direction (matters for trim paths),
// vertex 0 stays vertex 0 of a closed path.
[[nodiscard]] PathData ReversedPath(const PathData &path);

// The round corners modifier as geometry: every corner vertex (both
// tangents zero) is replaced with an arc of the radius, limited by half
// of the adjacent straight segments. Smooth vertices are kept.
[[nodiscard]] PathData RoundedPath(const PathData &path, double radius);

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
	// Effect value / dash names, empty otherwise. A dash item without a
	// name gets "d" (dash), "g" (gap) or "o" (offset) by its position.
	QString name;
	int dimensions = 1; // Numbers in the value (Scalar 1, Vector 2-3...).
	int colorStops = 0; // Gradient: "g"."p".
	bool animated = false;
	bool spatial = false; // Has "ti" / "to" keyframe tangents.
	bool expression = false; // Has an expression ("x"), not rendered.
	int keyframes = 0;

	// PropertyRole::Dash only: the last item of "d", which the renderer
	// reads as the offset of the pattern. Unlike the dash and gap lengths
	// it may be negative and its easing may overshoot.
	bool dashOffset = false;
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

	// JSON to hand to the renderer: toJson() (the same bytes) unless the
	// document has values rlottie never returns from, see RenderSafeJson().
	// Cached like toJson(). Never save it, it is for drawing only.
	[[nodiscard]] QByteArray toRenderJson() const;

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

	// Values at several frames with one parse of the keyframes, for graphs
	// and motion previews. Empty if the property is not found.
	[[nodiscard]] std::vector<PropValue> valuesAt(
		const PropertyRef &ref,
		const std::vector<double> &frames) const;

	// The value the renderer uses while a known optional property is not
	// in the file: transform parts of layers / groups ("ks.o" -> 100...),
	// mask "o" (100), "x" (0) and "f" ({ 0, 0 }), gradient "h" / "a" (0),
	// rectangle "r" (0), round corners "r" (0), trim "o" (0), stroke "w".
	// SetValueAt() / SetStaticValue() / AddKeyframe() create such a
	// property on the first write. Empty for anything else.
	[[nodiscard]] std::optional<PropValue> defaultValue(
		const PropertyRef &ref) const;

	// Gradient stops of a gradient fill / stroke at a frame of its time
	// base, empty for other nodes.
	[[nodiscard]] std::optional<GradientData> gradientAt(
		NodeId shape,
		double frame) const;

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

// Renderer safety. A few values make rlottie (the copy bundled with this
// app, so the Telegram Desktop that shows the sticker too) loop forever or
// work for minutes. Each one was found in its sources and then confirmed
// by rendering test files with a time limit:
// - a keyframe with motion path tangents ("ti" / "to", the usual position
//   keyframe) whose easing goes below zero: VBezier::tAtLength() never
//   returns for a negative length. The easing rlottie uses is the one of
//   the first keyframe in the file with the same name ("n") or, without a
//   name, with the same handles to two decimals, so such a keyframe gets
//   handles at zero or above (x within [0, 1]) and loses a name that
//   another curve has as well;
// - a negative stroke dash or gap length (the last item of "d" is the
//   offset and may be anything), static or reached between keyframes;
// - a dash pattern much shorter than the curve it runs on (dash + gap of
//   0.2 on a 480 px circle);
// - trim start / end below -100% (below minus the offset if the offset is
//   above zero); small undershoots like -3% are fine and are kept;
// - huge repeater copies and star points counts (loop counters).
// The operations do not write such values: easing handles are limited for
// motion paths, dashes, copies and points (see SetKeyframeHandles()),
// values are kept in range, and a stroke / trim / repeater / star item is
// checked as a whole after every write. For files from elsewhere
// Validate() reports IssueType::RendererHang (an error, auto-fixable), and
// everything drawn through Oblivion::Lottie (RenderFrame(), Renderer,
// ExportSvg()) gets the clamped copy made by RenderSafeJson().
inline constexpr auto kMaxRepeaterCopies = 512.;
inline constexpr auto kMaxStarPoints = 1000.;
inline constexpr auto kMinDashPeriod = 1.; // Dash + gap, all pairs together.

// Plain Lottie JSON with the dangerous values clamped. Returns the same
// bytes if there is nothing to clamp. Empty if the JSON can't be parsed:
// rlottie draws what it has read before an error, so bytes nobody could
// check must not get to it.
[[nodiscard]] QByteArray RenderSafeJson(const QByteArray &json);

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

// Moves a layer inside its composition, a mask inside its layer or a
// shape item into a layer / group (container), so that it ends up before
// the item that was at index (index == count means the end; a group "tr"
// stays last). Track matte pairs move together and are never split.
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

	// Round 4.
	GradientStroke, // Two stops, round caps and joins, width 4.
	Repeater, // Three copies shifted to the right.
	RoundCorners, // The "rd" modifier, see BakeRoundCorners().
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
// to the top, fills / strokes / trims / repeaters / round corners to the
// end (before the group transform), so that they apply to everything
// above them.
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

// After Effects features (round 4). The top comment tells what Telegram's
// renderer draws. Frames are in the time base of the node, see
// Document::localFrame().

// Two edits made one after another (second computed from first.document)
// as one edit for EditorController::perform(). Failed if either failed.
[[nodiscard]] Edit Combined(Edit first, Edit second);

// Masks. AddMask() adds a mask to a layer (index -1: the end of the list)
// with opacity 100 and expansion 0 and turns "hasMask" on. The path is in
// the layer's own coordinates (Document::transformAt(layer) maps them to
// the canvas). Mask properties are edited with the common property
// operations: { mask, "pt" } (path), "o" (opacity), "x" (expansion) and
// "f" (feather, { x, y }); missing ones are created on the first write.
// Of these Telegram draws only the path: opacity, expansion and feather
// are kept for other players and for export (IssueType::MaskOptions).
// Masks are reordered inside their layer with MoveNode() / ReorderNode()
// (the order matters: each mask is combined with the result of those
// before it), removed with DeleteNodes() and copied with DuplicateNodes().
[[nodiscard]] Edit AddMask(
	const Document &document,
	NodeId layer,
	const PathData &path,
	MaskMode mode = MaskMode::Add,
	const QString &name = QString(),
	int index = -1);
[[nodiscard]] Edit SetMaskMode(
	const Document &document,
	NodeId mask,
	MaskMode mode);

// Writes the "inv" flag as it is. Telegram's renderer does not read it,
// to really invert a mask there use InvertMask().
[[nodiscard]] Edit SetMaskInverted(
	const Document &document,
	NodeId mask,
	bool inverted);

// The mode that cuts the complement of the mask shape without the "inv"
// flag, if there is one. The first mask of a layer: add <-> subtract,
// intersect and difference -> subtract. The following masks: subtract
// <-> intersect (add and difference have no such mode).
[[nodiscard]] std::optional<MaskMode> InvertedMaskMode(
	MaskMode mode,
	bool first);

// Inverts a mask in a way every renderer draws: switches the mode to
// InvertedMaskMode() and clears "inv". Fails if there is no such mode.
[[nodiscard]] Edit InvertMask(const Document &document, NodeId mask);

// The usual first mask: a rectangle around what the layer shows at the
// root frame (the whole canvas for an empty layer), in the layer's own
// coordinates.
[[nodiscard]] PathData DefaultMaskPath(
	const Document &document,
	NodeId layer,
	double rootFrame);

// Track mattes. A mode other than None makes `source` the matte of
// `layer`: the source is moved right above the layer (the only place the
// renderer looks for it) and marked with "td": 1. source 0 keeps the
// current matte layer or takes the layer that is right above now. A
// replaced matte layer becomes an ordinary visible layer ("td" removed).
// Mode None removes the matte the same way: "tt" is removed and the matte
// layer becomes an ordinary layer.
//
// Fails if there is no layer to use, if the source is the layer itself,
// lives in another composition, is matted itself or already is the matte
// of another layer (duplicate it first).
[[nodiscard]] Edit SetTrackMatte(
	const Document &document,
	NodeId layer,
	MatteMode mode,
	NodeId source = 0);

// Parenting. parent 0 removes the link. CanSetLayerParent(): both are
// layers of one composition and the link makes no cycle.
//
// keepAtFrame (a frame of the layer's composition) keeps the layer where
// it is on the canvas at that frame: position, rotation and scale are
// rewritten relative to the new parent, all their keyframes get the same
// correction (like After Effects does). Without it the values stay and
// the layer jumps. A parent without a usable "ind" gets a unique one.
[[nodiscard]] bool CanSetLayerParent(
	const Document &document,
	NodeId layer,
	NodeId parent);
[[nodiscard]] Edit SetLayerParent(
	const Document &document,
	NodeId layer,
	NodeId parent,
	std::optional<double> keepAtFrame = std::nullopt);

// Plain (not animatable) options of shape items, see NodeInfo. Each fails
// for a node of another type.
[[nodiscard]] Edit SetGradientType(
	const Document &document,
	NodeId shape, // "gf" / "gs"
	GradientType type);
[[nodiscard]] Edit SetTrimMode(
	const Document &document,
	NodeId shape, // "tm"
	TrimMode mode);
[[nodiscard]] Edit SetLineCap(
	const Document &document,
	NodeId shape, // "st" / "gs"
	LineCap cap);
[[nodiscard]] Edit SetLineJoin(
	const Document &document,
	NodeId shape, // "st" / "gs"
	LineJoin join);
[[nodiscard]] Edit SetMiterLimit(
	const Document &document,
	NodeId shape, // "st" / "gs"
	double limit);
[[nodiscard]] Edit SetFillRule(
	const Document &document,
	NodeId shape, // "fl" / "gf"
	FillRule rule);

// Gradients ("gf" / "gs"). The start / end points, the radial highlight,
// opacity and the stroke width are ordinary properties.
//
// SetGradient() writes the stops at the frame (a keyframe of an animated
// gradient). When the number of color or opacity stops changes, every
// other keyframe is rebuilt with the new number: its own gradient is
// sampled at the new offsets.
//
// AddGradientStop() / RemoveGradientStop() change the number of stops in
// every keyframe and keep the other stops of every keyframe. A new stop
// takes the color (or opacity) the gradient has at its offset in that
// keyframe. alpha selects the opacity stops: the first one comes with
// fully opaque stops at both ends (the renderer needs at least two),
// removing one of the last two removes both. The last color stop is never
// removed. index is the position in Document::gradientAt().
[[nodiscard]] Edit SetGradient(
	const Document &document,
	NodeId shape,
	const GradientData &data,
	double frame);
[[nodiscard]] Edit AddGradientStop(
	const Document &document,
	NodeId shape,
	double offset,
	bool alpha = false);
[[nodiscard]] Edit RemoveGradientStop(
	const Document &document,
	NodeId shape,
	int index,
	bool alpha = false);

// Fill <-> gradient fill, stroke <-> gradient stroke (type is the target:
// Fill, Stroke, GradientFill or GradientStroke). The node keeps its id,
// name, opacity, fill rule, stroke width, caps, joins and dashes. A new
// gradient goes from the old color to a darker one across the shapes of
// the container at the frame, a new plain color is the first color stop.
[[nodiscard]] Edit ConvertPaint(
	const Document &document,
	NodeId shape,
	ShapeType type,
	double frame = 0.);

// Stroke dashes ("st" / "gs"). Every value is a property of its own
// (PropertyRole::Dash) that can be animated with the common operations.
// The items are told apart by their "n" names ("d" / "g" / "o"); a list
// with a missing name is read by position like the renderer reads every
// list: the last item is the offset, before it dash, gap, dash, gap...
// (a dash without a gap gets a gap of its own length).
struct DashInfo {
	std::vector<PropertyRef> dashes; // { stroke, "d.N.v" }
	std::vector<PropertyRef> gaps;
	PropertyRef offset; // Invalid if the pattern has no offset.

	[[nodiscard]] bool empty() const {
		return dashes.empty();
	}
};
[[nodiscard]] DashInfo DashesOf(const Document &document, NodeId stroke);

// pattern: dash, gap, dash, gap... (a missing last gap repeats its dash),
// empty removes the dashes. Writes static values in the order the
// renderer needs (the offset last). Negative lengths become 0 and gaps
// are widened if the whole pattern is shorter than kMinDashPeriod (both
// hang the renderer).
[[nodiscard]] Edit SetDashes(
	const Document &document,
	NodeId stroke,
	const std::vector<double> &pattern,
	double offset = 0.);

// Adds / removes dash + gap pairs at the end and keeps the values and
// keyframes of the others, 0 removes the dashes. Also puts the items in
// the order the renderer needs.
[[nodiscard]] Edit SetDashCount(
	const Document &document,
	NodeId stroke,
	int pairs);

// Rounded corners. Telegram's renderer ignores the round corners modifier
// ("rd"), this turns it into geometry: the paths above it in the same
// list (and in the groups above it) get rounded corners in every
// keyframe, rectangles without their own roundness get it, then the
// modifier is removed. An animated radius is taken at the frame.
[[nodiscard]] Edit BakeRoundCorners(
	const Document &document,
	NodeId roundCorners,
	double frame = 0.);

// Paths. AddPath() adds a path item ("sh") to a shape layer / group, index
// like AddShape().
[[nodiscard]] Edit AddPath(
	const Document &document,
	NodeId container,
	const PathData &path,
	const QString &name,
	int index = -1);

// Path structure of { shape, "ks" } / { mask, "pt" }: applied to the
// static value or to every keyframe, so that all of them keep the same
// vertices (the renderer cuts a longer keyframe to the shorter one). To
// move vertices and tangents use SetValueAt() with a path that has the
// same number of vertices. RemovePathVertex() keeps at least two.
[[nodiscard]] Edit InsertPathVertex(
	const Document &document,
	const PropertyRef &path,
	int segment,
	double t);
[[nodiscard]] Edit RemovePathVertex(
	const Document &document,
	const PropertyRef &path,
	int index);
[[nodiscard]] Edit SetPathClosed(
	const Document &document,
	const PropertyRef &path,
	bool closed);
[[nodiscard]] Edit ReversePath(
	const Document &document,
	const PropertyRef &path);

// Easing graph. The curve between two keyframes is a cubic bezier from
// (0, 0) to (1, 1): x is the time progress, y the value progress. `out`
// is the handle next to the keyframe the segment starts at, `in` the
// handle next to the keyframe it ends at, both as points of that unit
// square (x in [0, 1], y may leave it to overshoot). So a keyframe has an
// in handle (stored in the previous keyframe's segment) and an out handle
// (its own segment). To draw a handle in (frame, value) coordinates:
// frame = t0 + x * (t1 - t0), value = v0 + y * (v1 - v0) for the segment
// from keyframe (t0, v0) to keyframe (t1, v1).
struct KeyframeHandles {
	bool hasIn = false; // There is a previous keyframe.
	bool hasOut = false; // There is a next keyframe.
	bool holdIn = false; // The previous segment is hold: no in handle.
	bool holdOut = false; // The own segment is hold: no out handle.
	QPointF in = QPointF(1., 1.);
	QPointF out = QPointF(0., 0.);
	double previousTime = 0.; // Valid with hasIn.
	double nextTime = 0.; // Valid with hasOut.
};
[[nodiscard]] std::optional<KeyframeHandles> HandlesOf(
	const Document &document,
	const KeyframeRef &keyframe);

// Sets the given handles (x is clamped to [0, 1]): `in` changes the
// previous keyframe's segment, `out` the keyframe's own one. A hold
// segment becomes a bezier one. Sides that do not exist are ignored.
//
// y is limited to EasingRange(): [0, 1] (no overshoot) for properties
// where a value outside of the keyframe values hangs the renderer (motion
// path keyframes, dash and gap lengths, repeater copies, star points) and
// [-2, 3] for the others, the dash offset included. Every operation that
// takes an Easing clamps it the same way.
[[nodiscard]] Edit SetKeyframeHandles(
	const Document &document,
	const KeyframeRef &keyframe,
	std::optional<QPointF> in,
	std::optional<QPointF> out);

struct EasingLimits {
	double minY = -2.;
	double maxY = 3.;

	// minY == 0: handles below the keyframe line are not allowed.
	[[nodiscard]] bool overshoot() const {
		return (minY < 0.) || (maxY > 1.);
	}
};
[[nodiscard]] EasingLimits EasingRange(
	const Document &document,
	const PropertyRef &ref);

// Motion path of a point property (position, anchor, gradient points...):
// with it ("to" / "ti" tangents in the keyframes) the value moves along a
// curve by its length and can't overshoot; without it every dimension is
// interpolated on its own and the easing may leave [0, 1]. Turning it off
// drops the tangents (the motion becomes straight lines), turning it on
// adds zero tangents and limits the easing.
[[nodiscard]] Edit SetMotionPath(
	const Document &document,
	const PropertyRef &ref,
	bool enabled);

// SetKeyframeEasing() with an easing of its own for every keyframe.
[[nodiscard]] Edit SetKeyframeEasings(
	const Document &document,
	const std::vector<std::pair<KeyframeRef, Easing>> &easings);

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

// What an issue is about, so that the UI can say it in plain words:
// "Telegram does not accept this" is not the same as "Telegram will not
// draw this". Only .tgs is concerned, export to video / GIF / JSON keeps
// everything.
enum class IssueCategory : uchar {
	// The file as a whole: canvas, frame rate, duration, size, version.
	File,

	// A feature Telegram does not accept in animated stickers: it is in
	// the official requirements (core.telegram.org/stickers) and / or the
	// official Bodymovin-TG exporter refuses to export it. The list:
	// expressions, masks, mattes, layer effects, images, solids, texts, 3D
	// layers, merge paths, star shapes, gradient strokes, repeaters, time
	// stretching, time remapping, auto-oriented layers. Issue::rendered
	// tells whether the renderer would draw it anyway.
	Forbidden,

	// Allowed, but Telegram's renderer (rlottie) ignores it or draws it
	// differently from After Effects and other players.
	NotRendered,

	// Accepted and drawn, but worth a look.
	Advice,
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

	// Round 4, warnings except RendererHang (Telegram takes such files).

	// Forbidden. Track mattes are in Telegram's list of features a
	// sticker must not use. They are drawn (alpha, luma and both
	// inverted), but every pair is rendered through two full size
	// buffers, which is slow. Nodes: the matted layers.
	TrackMattes,

	// NotRendered. A matted layer that is not drawn at all: it has no
	// layer above it or the layer above is matted too. Nodes: such layers.
	BrokenMattes,

	// NotRendered. "tp" (the matte link of newer exports) names a layer
	// that is not right above the matted one: Telegram uses the layer
	// right above instead. Nodes: the matted layers.
	MatteLinks,

	// NotRendered. The layer has masks but no "hasMask": true, they are
	// all ignored. Fix: turn "hasMask" on (the picture changes: the masks
	// start to work). Nodes: the layers.
	MasksOff,

	// NotRendered. Mask mode lighten / darken or a missing mode: the mask
	// is skipped; or every mask of the layer is "none" / skipped: the
	// layer is not drawn at all. Fix: lighten -> add, darken -> intersect,
	// masks are turned off for a layer that has only "none" masks (the
	// picture changes). Nodes: the masks.
	MaskModes,

	// NotRendered. Mask opacity is not 100%, feather or expansion is set:
	// all three are ignored, the mask is fully opaque with a sharp edge on
	// its path. Fix: reset them (the picture in Telegram stays the same).
	// Nodes: the masks.
	MaskOptions,

	// NotRendered. The mask is inverted ("inv"): the flag is ignored, the
	// mask works as if it was not inverted. Fix: InvertMask() where a mode
	// with the same result exists (the picture changes to the inverted
	// one). Nodes: the masks.
	MaskInverted,

	// NotRendered. Keyframes of a path have different numbers of vertices:
	// the longer ones are cut. Nodes: the shapes / masks.
	PathVertices,

	// NotRendered. Parser quirks: a shape item has keys before "ty" (all
	// of them are ignored), or a layer has no "ddd": 0 before "ks" (its
	// rotation is ignored). Fix: reorder the keys (the picture changes to
	// what the file means). Nodes: the shapes / layers.
	KeyOrder,

	// NotRendered. "parent" names a missing layer, the layer itself or
	// makes a cycle: the link is ignored. Fix: remove it. Nodes: layers.
	ParentLinks,

	// NotRendered, the only round 4 error: values that hang Telegram's
	// renderer, see RenderSafeJson(). Fix: clamp them. Nodes: the layers,
	// shapes and masks that have such values.
	RendererHang,
};

struct Issue {
	IssueType type = IssueType::InvalidComposition;
	IssueSeverity severity = IssueSeverity::Error;
	std::vector<NodeId> nodes; // Offending nodes, may be empty.
	bool fixable = false;

	// Measured value: canvas width (CanvasSize), fps (FrameRate), seconds
	// (Duration), gzipped bytes (FileSize), 0 otherwise.
	double value = 0.;

	// Filled by Validate(): CategoryOf(type), RenderedByTelegram(type) and
	// FixChangesPicture(type).
	IssueCategory category = IssueCategory::File;
	bool rendered = true;
	bool fixChangesPicture = false;
};

[[nodiscard]] IssueCategory CategoryOf(IssueType type);

// Whether Telegram's renderer draws the feature the issue is about the
// way After Effects does. true for a Forbidden issue means: the sticker
// would look right, Telegram just does not allow the feature (masks,
// track mattes, solids, repeaters, star shapes, gradient strokes, time
// stretching and remapping, auto-orient). Always true for File / Advice
// issues, false for NotRendered ones.
[[nodiscard]] bool RenderedByTelegram(IssueType type);

// The automatic fix changes what Telegram shows (to what the file was
// meant to look like): MasksOff, MaskModes, MaskInverted, KeyOrder.
// AutoFix() applies such fixes only when their type is named, never with
// an empty list, so that "fix everything" can not change a sticker that
// already looks right in Telegram.
[[nodiscard]] bool FixChangesPicture(IssueType type);

struct ValidateOptions {
	bool measureSize = true; // Gzip the document (a few ms).
	bool renderChecks = false; // Render frames for OutOfCanvas (slow).
};

struct ValidationResult {
	std::vector<Issue> issues; // Errors first.
	int64 packedSize = -1; // Gzipped size, -1 if not measured.
	bool packedSizeEstimated = false; // PackTgs() unavailable, zlib estimate.

	[[nodiscard]] bool ok() const; // No errors.

	// Something AutoFix() without a list of types would fix (fixable
	// issues whose fix does not change the picture).
	[[nodiscard]] bool hasFixable() const;

	// Issues of one category / the first issue of a type (null if none,
	// the pointer lives as long as this result).
	[[nodiscard]] std::vector<Issue> of(IssueCategory category) const;
	[[nodiscard]] const Issue *find(IssueType type) const &;
	const Issue *find(IssueType type) const && = delete;
};

[[nodiscard]] ValidationResult Validate(
	const Document &document,
	const ValidateOptions &options = {});

// Applies the fixes of the given issue types, in a safe order, as one
// edit. An empty list means every fixable type whose fix keeps the
// picture (see FixChangesPicture()).
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

	// Round 4.
	AddMask,
	ChangeMask, // Mode / inverted.
	TrackMatte,
	Parent,
	ShapeOption, // Gradient type, trim mode, caps, joins, fill rule.
	Gradient, // Gradient stops.
	ConvertPaint, // Fill <-> gradient fill, stroke <-> gradient stroke.
	Dashes,
	BakeCorners,
	AddPath,
	EditPath, // Vertices added / removed, closed / opened, reversed.
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

	// Round 4 shortcuts, the same rules: frames default to the current
	// frame in the node's time base, added nodes get selected.
	bool addMask(
		NodeId layer,
		const PathData &path,
		MaskMode mode = MaskMode::Add,
		const QString &name = QString());
	bool setMaskMode(NodeId mask, MaskMode mode);
	bool setMaskInverted(NodeId mask, bool inverted);
	bool invertMask(NodeId mask);
	bool setTrackMatte(NodeId layer, MatteMode mode, NodeId source = 0);
	bool setLayerParent(NodeId layer, NodeId parent, bool keepPlace = true);
	bool setGradientType(NodeId shape, GradientType type);
	bool setTrimMode(NodeId shape, TrimMode mode);
	bool setLineCap(NodeId shape, LineCap cap);
	bool setLineJoin(NodeId shape, LineJoin join);
	bool setMiterLimit(
		NodeId shape,
		double limit,
		QByteArray mergeKey = QByteArray());
	bool setFillRule(NodeId shape, FillRule rule);
	bool setGradient(
		NodeId shape,
		const GradientData &data,
		QByteArray mergeKey = QByteArray());
	bool addGradientStop(NodeId shape, double offset, bool alpha = false);
	bool removeGradientStop(NodeId shape, int index, bool alpha = false);
	bool convertPaint(NodeId shape, ShapeType type);
	bool setDashes(
		NodeId stroke,
		const std::vector<double> &pattern,
		double offset = 0.,
		QByteArray mergeKey = QByteArray());
	bool setDashCount(NodeId stroke, int pairs);
	bool bakeRoundCorners(NodeId roundCorners);
	bool addPath(
		NodeId container,
		const PathData &path,
		const QString &name);
	bool insertPathVertex(const PropertyRef &path, int segment, double t);
	bool removePathVertex(const PropertyRef &path, int index);
	bool setPathClosed(const PropertyRef &path, bool closed);
	bool reversePath(const PropertyRef &path);
	bool setKeyframeHandles(
		const KeyframeRef &keyframe,
		std::optional<QPointF> in,
		std::optional<QPointF> out,
		QByteArray mergeKey = QByteArray());
	bool setKeyframeEasings(
		const std::vector<std::pair<KeyframeRef, Easing>> &easings);
	bool setMotionPath(const PropertyRef &ref, bool enabled);

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
