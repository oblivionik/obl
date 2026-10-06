/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_fx.h"

#include <QtGui/QPolygonF>
#include <QtGui/QTransform>

#include <memory>

// Photo editor: the layered document.
//
// A Document is a value: the canvas size, the layers from the bottom to
// the top and the whole-image edit (Document::global, the EditState of
// oblivion_photo_core.h: crop, rotation, the classic adjustments, filter
// and effects applied to the flattened picture). Copying a document is
// cheap: the heavy data (the pixels of an image layer, the strokes of
// a drawing, a mask) lives in immutable shared objects (LayerContent,
// LayerMask) that the copies point to. To change a layer you copy the
// document, replace what changed and give the copy to the editor
// (Controller::apply in oblivion_photo_editor.h): that is what makes undo
// / redo of anything possible while the memory stays reasonable.
//
// The old single-image editor state maps to a document with one image
// layer that fills the canvas and the old EditState as Document::global:
// DocumentFromImage(). Rendering such a document gives exactly the pixels
// Render(image, state) of oblivion_photo_core.h gives.
//
// Coordinate systems:
//  - layer (local) coordinates: pixels of the layer content at full
//    resolution, (0, 0) is its top left corner, LayerContent::size() the
//    bottom right one. Effects, the mask and FxPoint parameters
//    (normalized to that rectangle) live here;
//  - canvas (document) coordinates: pixels of the canvas at full
//    resolution, (0, 0) .. Document::size. Layer::transform maps layer
//    coordinates to them (it may be a perspective transform). Tools get
//    mouse positions in these coordinates;
//  - output coordinates: the picture after Document::global geometry
//    (turns, flips, straighten, crop), see OutputTransform().
//
// Rendering order for every visible layer, bottom first:
//   content -> the enabled effects in order -> the mask (alpha multiplied)
//   -> the transform -> blended into the picture with the blend mode and
//   the opacity; after the last layer Document::global is applied.
//
// Threading: everything here is plain data and pure functions, usable
// from any thread. LayerContent::render() is called on worker threads,
// possibly for several scales at once, so a content object must be
// immutable after construction (or guard its own caches). A Compositor
// may be shared between threads.
namespace Oblivion::Photo {

enum class BlendMode : uchar {
	Normal,
	Multiply,
	Screen,
	Overlay,
	SoftLight,
	HardLight,
	Darken,
	Lighten,
	ColorDodge,
	ColorBurn,
	Difference,
	Exclusion,
	Add, // Linear dodge.
	Hue,
	Saturation,
	Color,
	Luminosity,
};
inline constexpr auto kBlendModeCount = 17;

// All modes in the menu order (the enum order).
[[nodiscard]] const std::vector<BlendMode> &BlendModes();
[[nodiscard]] QByteArray BlendModeKey(BlendMode mode); // "soft_light".
[[nodiscard]] std::optional<BlendMode> BlendModeFromKey(QByteArrayView key);

// The reference blend of one premultiplied pixel over another one (the
// W3C compositing formulas, source-over with the blend function applied
// where both pixels are visible). opacity multiplies the source.
[[nodiscard]] QRgb BlendPixel(
	QRgb backdrop,
	QRgb source,
	BlendMode mode,
	double opacity = 1.);

// Blends source over backdrop (both Format_ARGB32_Premultiplied) with
// its top left corner at position, clipped to the backdrop. Rows are
// processed in parallel.
void BlendImage(
	QImage &backdrop,
	const QImage &source,
	QPoint position,
	BlendMode mode,
	double opacity = 1.);

using LayerId = uint64;

struct ContentRequest {
	// Rendered pixels per layer pixel. The content may return a smaller
	// image than asked (an image layer never upscales its pixels).
	double scale = 1.;
	bool preview = false; // An interactive render, see FxContext.
	const std::atomic<bool> *cancel = nullptr;
};

// What a layer shows: an immutable object shared between the copies of
// a document. The built-in kind is ImageContent, the drawing, text and
// collage modules define their own subclasses (vector data that renders
// itself at any scale). To "edit" a content create a new object and put
// it into a copy of the document.
class LayerContent {
public:
	LayerContent();
	virtual ~LayerContent();

	LayerContent(const LayerContent &other) = delete;
	LayerContent &operator=(const LayerContent &other) = delete;

	// Unique for every object ever created in this process: the cache
	// key of everything rendered from this content.
	[[nodiscard]] uint64 serial() const {
		return _serial;
	}

	// "image", "draw", "text", "collage"...: for the icons of the layers
	// list and for finding out what a tool can do with the layer
	// (static_cast after checking the type).
	[[nodiscard]] virtual QByteArray type() const = 0;

	// The size of the local rectangle in canvas-resolution pixels.
	[[nodiscard]] virtual QSize size() const = 0;

	// The pixels of the whole local rectangle, about size() * scale
	// (see ScaledSize), Format_ARGB32_Premultiplied, device pixel ratio
	// 1. Null if cancelled or out of memory. Worker threads. No QFont /
	// text layout here (prepare a QPainterPath on the main thread when
	// the content is created), QPainter on a QImage is fine.
	[[nodiscard]] virtual QImage render(
		const ContentRequest &request) const = 0;

	// Bytes kept alive by this object, for the undo memory limit.
	[[nodiscard]] virtual qint64 memoryUsage() const;

	// The same object by object, for a content that shares heavy data
	// with other contents (the original of a cutout, the photos of
	// a collage): visit is called with every such object and its own
	// bytes, so the undo history counts an object once however many
	// contents and steps keep it and knows what the current document
	// holds. The default reports the content itself with memoryUsage().
	virtual void memoryParts(
		const Fn<void(const void *object, qint64 bytes)> &visit) const;

private:
	const uint64 _serial = 0;

};

using ContentPtr = std::shared_ptr<const LayerContent>;

// A raster image. original() is not null for a cutout made by "Remove
// background": the content it was made from, to bring the background
// back.
class ImageContent final : public LayerContent {
public:
	explicit ImageContent(
		QImage image,
		std::shared_ptr<const ImageContent> original = nullptr);

	QByteArray type() const override;
	QSize size() const override;
	QImage render(const ContentRequest &request) const override;
	qint64 memoryUsage() const override;
	void memoryParts(
		const Fn<void(const void *object, qint64 bytes)> &visit
	) const override;

	[[nodiscard]] const QImage &image() const;
	[[nodiscard]] const std::shared_ptr<const ImageContent> &original() const;

private:
	QImage _image;
	std::shared_ptr<const ImageContent> _original;

};

[[nodiscard]] std::shared_ptr<const ImageContent> MakeImageContent(
	QImage image,
	std::shared_ptr<const ImageContent> original = nullptr);
// Not null only for ImageContent.
[[nodiscard]] const ImageContent *AsImage(const ContentPtr &content);

// A paintable layer mask: a Format_Grayscale8 image stretched over the
// local rectangle of the layer (any resolution, see MaskSizeFor), 255
// shows the layer, 0 hides it. Immutable and shared like the content: to
// paint, copy image(), change it and make a new mask.
class LayerMask final {
public:
	explicit LayerMask(QImage image);

	[[nodiscard]] uint64 serial() const {
		return _serial;
	}
	[[nodiscard]] const QImage &image() const {
		return _image;
	}
	[[nodiscard]] qint64 memoryUsage() const;

private:
	const uint64 _serial = 0;
	QImage _image;

};

using MaskPtr = std::shared_ptr<const LayerMask>;

inline constexpr auto kMaskMaxSide = 2048;

// The mask resolution for a content of this size: the same aspect, the
// longer side at most kMaskMaxSide.
[[nodiscard]] QSize MaskSizeFor(QSize content);
[[nodiscard]] MaskPtr MakeMask(QImage grayscale);
[[nodiscard]] MaskPtr MakeMask(QSize size, int fill = 255);

struct Layer {
	LayerId id = 0; // Assigned by AddLayer(), unique in the document.
	QString name;
	bool visible = true;
	bool locked = false; // Tools must not change a locked layer.
	double opacity = 1.; // 0..1.
	BlendMode blend = BlendMode::Normal;

	// Layer coordinates -> canvas coordinates. Usually affine (move,
	// scale, rotate, flip, skew), a free 4-corner placement makes it
	// a perspective one, see QuadTransform().
	QTransform transform;

	ContentPtr content;
	MaskPtr mask; // Null: no mask.
	bool maskEnabled = true;
	std::vector<FxInstance> effects; // Applied in order.

	[[nodiscard]] QSize size() const; // content->size() or empty.

	// Compares everything, the content and the mask by identity.
	friend bool operator==(const Layer &a, const Layer &b);
};

struct Document {
	QSize size; // The canvas, pixels.
	std::vector<Layer> layers; // Bottom first.

	// The whole-image edit, applied to the flattened layers: geometry
	// (crop, turns, flips, straighten: "canvas size / crop / aspect for
	// the whole document") and the classic adjustments, filter, effects.
	EditState global;

	// The next id for a layer or an effect instance, see AddLayer().
	uint64 nextId = 1;

	[[nodiscard]] bool empty() const {
		return size.isEmpty();
	}
	[[nodiscard]] int indexOf(LayerId id) const; // -1 if there is none.
	[[nodiscard]] const Layer *find(LayerId id) const;
	[[nodiscard]] Layer *find(LayerId id);
	[[nodiscard]] const Layer *top() const;

	friend bool operator==(const Document &a, const Document &b);
};

// Sizes and placement.
[[nodiscard]] QSize ScaledSize(QSize size, double scale); // At least 1x1.
// min(1, side / longer canvas side).
[[nodiscard]] double ScaleForSide(QSize canvas, int side);

enum class Placement : uchar {
	Center, // Original size, centered.
	Fit, // Scaled down to fit (never up), centered.
	Fill, // Scaled to cover the canvas, centered.
	Stretch, // Exactly the canvas.
};
[[nodiscard]] QTransform PlaceTransform(
	QSize content,
	QSize canvas,
	Placement placement);

// The corners of the layer in canvas coordinates: top left, top right,
// bottom right, bottom left of the local rectangle.
[[nodiscard]] QPolygonF LayerQuad(const Layer &layer);
[[nodiscard]] QRectF LayerBounds(const Layer &layer);
// The transform that puts the corners of a content of this size at the
// given canvas points (the LayerQuad order). False for a quad that is
// not a convex 4-corner shape (the perspective would fold).
[[nodiscard]] bool QuadTransform(
	QSizeF content,
	const QPolygonF &quad,
	QTransform &result);
[[nodiscard]] bool IsPerspective(const QTransform &transform);
// Canvas point -> layer point, nullopt if the transform can't be
// inverted or the point is behind the perspective horizon.
[[nodiscard]] std::optional<QPointF> LayerPoint(
	const Layer &layer,
	QPointF canvasPoint);
// The topmost visible layer whose quad has the point, 0 if none (the
// pixels are not checked). Locked layers are skipped unless asked.
[[nodiscard]] LayerId LayerAt(
	const Document &document,
	QPointF canvasPoint,
	bool withLocked = false);

// An affine transform as the transform tool shows it: the canvas point
// where the content center goes, scale factors (a negative scaleY is
// a flipped layer), rotation in degrees (clockwise on the screen) and
// a horizontal shear. The perspective part, if any, is dropped.
struct TransformParts {
	QPointF center;
	double scaleX = 1.;
	double scaleY = 1.;
	double rotation = 0.;
	double shear = 0.;
};
[[nodiscard]] TransformParts DecomposeTransform(
	const QTransform &transform,
	QSizeF content);
[[nodiscard]] QTransform ComposeTransform(
	const TransformParts &parts,
	QSizeF content);

// Making documents and layers.
[[nodiscard]] Document DocumentFromImage(
	QImage image,
	const EditState &state = EditState(),
	QString name = QString());
[[nodiscard]] Layer MakeLayer(ContentPtr content, QString name);
[[nodiscard]] Layer MakeImageLayer(QImage image, QString name);
// True for what DocumentFromImage() makes with any Document::global: one
// visible, opaque, normal, untransformed image layer over the whole
// canvas without effects and a mask.
[[nodiscard]] bool IsPlainImage(const Document &document);

// Document operations. They change the document in place: work on a copy
// and pass it to Controller::apply(). Ids never change, so anything
// that remembers a LayerId / an effect uid finds it again after undo.
//
// AddLayer gives the layer a new id (and its effects new uids) and
// inserts it at index (0 is the bottom, -1 the top).
LayerId AddLayer(Document &document, Layer layer, int index = -1);
bool RemoveLayer(Document &document, LayerId id);
// The copy goes right above the original and shares its content.
LayerId DuplicateLayer(Document &document, LayerId id);
bool MoveLayer(Document &document, LayerId id, int index);
// Effects of a layer. AddLayerFx returns the uid of the new instance.
uint64 AddLayerFx(
	Document &document,
	LayerId id,
	FxInstance instance,
	int index = -1);
bool RemoveLayerFx(Document &document, LayerId id, uint64 uid);
bool MoveLayerFx(Document &document, LayerId id, uint64 uid, int index);
[[nodiscard]] FxInstance *LayerFx(Document &document, LayerId id, uint64 uid);
[[nodiscard]] const FxInstance *LayerFx(
	const Document &document,
	LayerId id,
	uint64 uid);

// The canvas size. A side is kCanvasMinSide..kCanvasMaxSide pixels and
// the area at most kCanvasMaxPixels (a picture that was opened may be
// larger, it is never made smaller by itself).
inline constexpr auto kCanvasMinSide = 16;
inline constexpr auto kCanvasMaxSide = 16384;
inline constexpr auto kCanvasMaxPixels = 64LL * 1000 * 1000;

// The size inside the limits: the sides are clamped, then an area over
// the limit is scaled down keeping the proportions.
[[nodiscard]] QSize ValidCanvasSize(QSize size);
// The smallest canvas with the proportions ratioWidth : ratioHeight that
// has a canvas of the current size inside (nothing is cropped), within
// the limits.
[[nodiscard]] QSize CanvasSizeForAspect(
	QSize current,
	int ratioWidth,
	int ratioHeight);
// The document on a canvas of another size (made valid first). Every
// layer keeps its size and its place in the picture: anchor (0..1 on
// both axes) says where the old canvas lies on the new one, the default
// keeps it in the center (shifted by whole pixels, so nothing gets
// blurred). A larger canvas adds a transparent area around, a smaller
// one hides what sticks out (the layers themselves are not cut). The
// crop frame of Document::global is normalized to the canvas, so it is
// reset; the turns, the flips and the adjustments stay.
[[nodiscard]] Document CanvasResized(
	const Document &document,
	QSize size,
	QPointF anchor = QPointF(0.5, 0.5));

// Output geometry of the whole document (after Document::global).
[[nodiscard]] QSize OutputSize(const Document &document);
[[nodiscard]] QSize OutputSize(const Document &document, QSize maxSize);
// Canvas coordinates -> coordinates of an output of this size.
[[nodiscard]] QTransform OutputTransform(
	const Document &document,
	QSize output);

struct RenderRequest {
	// Rendered pixels per canvas pixel for the layers, 0 < scale <= 1:
	// 1 for the export, less for the previews (the "proxy").
	double scale = 1.;
	// Fits the result into this size (never upscales), empty: no limit.
	QSize maxSize;
	// Apply Document::global (false: the flattened layers at the canvas
	// size, without the crop and the whole-image adjustments).
	bool global = true;
	bool preview = false;
	// A hint: the layer that is being edited. The picture under it is
	// kept, so the next render only redoes this layer and those above.
	LayerId active = 0;
	// false: nothing new is kept in the caches (exports).
	bool cache = true;
	const std::atomic<bool> *cancel = nullptr;
};

// Renders documents and remembers the intermediate results (the pixels
// of a layer before and after its effects, the picture under the layer
// being edited, the flattened layers), so that changing one layer does
// not recompute the others and changing one effect does not recompute
// the effects before it. Thread safe: several renders may run at once.
class Compositor final {
public:
	Compositor();
	~Compositor();

	// The document as the user sees it. Null if cancelled / no memory.
	[[nodiscard]] QImage render(
		const Document &document,
		const RenderRequest &request);

	// One layer alone in layer coordinates: the content with the effects
	// and the mask (about layer size * scale). For thumbnails and tools.
	[[nodiscard]] QImage layerPixels(
		const Layer &layer,
		double scale,
		const std::atomic<bool> *cancel = nullptr,
		bool preview = true);

	void setMemoryLimit(qint64 bytes); // Of the caches, 640 MB by default.
	void clear();

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

// A full-resolution render without caches, for exports. Worker thread.
[[nodiscard]] QImage RenderDocument(
	const Document &document,
	QSize maxSize = QSize(),
	const std::atomic<bool> *cancel = nullptr);

// Heavy operations that make pixels (worker thread, then apply the
// result on the main one). Results are null / nullopt if cancelled.
//
// The layer as it looks on the canvas (effects, mask, transform, but
// opacity 1 and no blending) in a canvas-sized transparent image.
[[nodiscard]] QImage RasterizeLayer(
	const Layer &layer,
	QSize canvas,
	const std::atomic<bool> *cancel = nullptr);
// The document where the layer and the one under it became one image
// layer (canvas sized, with the id and the name of the lower one):
//  - a lower layer with the normal blend mode gets the upper one blended
//    into it with its own mode and opacity, the opacity of the lower one
//    goes into the pixels (the merged layer is fully opaque). The picture
//    stays what it was, only an upper layer with a blend mode is put as it
//    is where the lower one is transparent: there is nothing to blend with
//    inside a layer;
//  - a lower layer that shows nothing (hidden, opacity 0) gives way: the
//    merged layer is the upper one with its blend mode and opacity;
//  - an upper layer that shows nothing is dropped, the lower one keeps
//    its opacity and blend mode;
//  - two hidden layers are merged as if both were shown and stay hidden.
// A lower layer that shows with another blend mode can't keep the picture:
// the upper layer is blended into its pixels and the result keeps the mode
// and the opacity of the lower one, so the upper layer gets them as well.
// Nothing better fits into one layer: callers check
// MergeDownUnderBlendMode() first and refuse such a merge.
// Unchanged document if there is nothing under the layer.
[[nodiscard]] std::optional<Document> MergedDown(
	const Document &document,
	LayerId id,
	const std::atomic<bool> *cancel = nullptr);
// Whether MergedDown() of this layer has to change the picture: it and
// the layer under it both show something (or both are hidden, they are
// merged as if shown) and the lower one has a blend mode other than the
// normal one. Cheap, no pixels are made.
[[nodiscard]] bool MergeDownUnderBlendMode(
	const Document &document,
	LayerId id);
// All visible layers as one image layer (hidden ones are dropped).
// Document::global stays as it is.
[[nodiscard]] std::optional<Document> Flattened(
	const Document &document,
	const std::atomic<bool> *cancel = nullptr);

// Undo / redo: a list of document snapshots. Snapshots share the heavy
// data, the memory of everything they keep alive is counted once and the
// oldest steps are dropped when what they keep beyond the current
// document grows over the limit. What the current document holds stays in
// the memory whatever is dropped, so it doesn't count: a document that is
// larger than the limit by itself still has its undo. One step back is
// kept in any case.
class History final {
public:
	explicit History(Document initial = Document());

	void reset(Document initial);
	[[nodiscard]] const Document &current() const;

	// Adds a step (drops the redo steps). False if nothing changed.
	bool push(Document document);
	[[nodiscard]] bool canUndo() const;
	[[nodiscard]] bool canRedo() const;
	bool undo();
	bool redo();

	[[nodiscard]] int index() const;
	[[nodiscard]] int count() const;
	[[nodiscard]] qint64 memoryUsage() const;
	// 200 steps and 768 MB (kept only for the older steps) by default.
	void setLimits(int steps, qint64 bytes);

private:
	void trim();
	// Of the unique heavy objects of the snapshots [from, till).
	[[nodiscard]] qint64 countBytes(int from, int till) const;

	std::vector<Document> _list;
	int _index = 0;
	int _maxSteps = 200;
	qint64 _maxBytes = 768LL * 1024 * 1024;

};

// Self-checks for OBLIVION_SELFTEST=photo_doc, see oblivion_selftest.h
// (the "photo" suite is RunSelfTest() of oblivion_photo_core.h): blend
// maths, the compositor against a per-pixel reference, caches, undo and
// the sub-tests registered with SelfTestSuite::Doc.
// No Core::App(), no session: pure logic only.
[[nodiscard]] bool RunDocSelfTest(QStringList &log);

} // namespace Oblivion::Photo
