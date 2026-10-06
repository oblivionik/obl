/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_doc.h"

// Photo editor: drawing and text.
//
// Two kinds of vector layers and the canvas tools that work with them
// (the tools, their options and the layer kinds register themselves in the
// editor, see oblivion_photo_editor.h, nothing has to be called for that):
//
//  - a drawing layer (LayerContent::type() == kDrawLayerType) keeps an
//    ordered list of shapes: freehand strokes of the pen, the marker, the
//    pencil and the eraser, lines, arrows, rectangles and ellipses. The
//    shapes are vectors in layer coordinates and are painted again at
//    every scale, so the export has them at the full resolution. The
//    eraser is a shape too: it wipes what was drawn before it on the same
//    layer and nothing else;
//  - a text layer (kTextLayerType) keeps a TextLayerStyle: the text, its
//    font, color, alignment and an optional plate behind it. The glyphs
//    are turned into outlines on the main thread when the content is made
//    and stay editable until the layer is merged or flattened.
//
// The tools ("draw.pen" B, "draw.marker" H, "draw.pencil" N, "draw.eraser"
// E, "draw.line" L, "draw.arrow" A, "draw.rect" R, "draw.ellipse" O,
// "draw.text" T) draw on the active layer if it is a drawing layer and
// add a new drawing layer above the active one otherwise; every stroke is
// one undo step, a stroke that never enters the layer (a click in the
// margin around the photo) adds nothing. A stroke is captured in screen
// pixels (a point every two of them, smoothed over a window that does not
// depend on the zoom) and kept in layer coordinates. Shift keeps lines at
// multiples of 45 degrees and makes boxes square, Alt draws a box from its
// center, "[" and "]" change the size of a brush. The eraser works only
// on the active drawing layer.
//
// The text tool adds an empty text layer where the photo is clicked and
// gives the focus to the text field of its options (the "Text" section
// of the Layer tab for any active text layer if that tab is open, the
// Tool tab otherwise, it is opened for that): the text is typed there and
// shown on the canvas at once. A click on a text selects it, a drag moves
// it. A text that was never typed paints nothing, not even its plate, it
// goes to the next place that is clicked and is removed when the user
// clicks another text or takes another tool.
//
// Contents are immutable (see LayerContent): the functions below make new
// ones. Everything except MakeTextContent / SetLayerText / MakeTextLayer
// may be called on any thread.
namespace Oblivion::Photo {

inline const auto kDrawLayerType = QByteArray("draw");
inline const auto kTextLayerType = QByteArray("text");

inline const auto kDrawPenTool = QByteArray("draw.pen");
inline const auto kDrawMarkerTool = QByteArray("draw.marker");
inline const auto kDrawPencilTool = QByteArray("draw.pencil");
inline const auto kDrawEraserTool = QByteArray("draw.eraser");
inline const auto kDrawLineTool = QByteArray("draw.line");
inline const auto kDrawArrowTool = QByteArray("draw.arrow");
inline const auto kDrawRectTool = QByteArray("draw.rect");
inline const auto kDrawEllipseTool = QByteArray("draw.ellipse");
inline const auto kDrawTextTool = QByteArray("draw.text");

enum class DrawKind : uchar {
	Pen, // An opaque round stroke, optionally thinner where drawn fast.
	Marker, // A wide flat-ended stroke, meant to be semi-transparent.
	Pencil, // A thin grainy stroke.
	Eraser, // Wipes the shapes before it on the same layer.
	Line,
	Arrow,
	Rectangle,
	Ellipse,
};
inline constexpr auto kDrawKindCount = 8;

[[nodiscard]] bool DrawKindIsFreehand(DrawKind kind);

struct DrawPoint {
	float x = 0.f; // Layer coordinates.
	float y = 0.f;
	// Freehand strokes: the width here as a part of DrawShape::width.
	float w = 1.f;

	friend bool operator==(const DrawPoint &a, const DrawPoint &b) = default;
};

struct DrawShape {
	DrawKind kind = DrawKind::Pen;
	QColor color = QColor(0, 0, 0); // Opaque, see opacity.
	double width = 8.; // Layer pixels.
	double opacity = 1.; // 0..1, for the eraser: how much it wipes.
	bool filled = false; // Rectangle, Ellipse.
	uint32 seed = 0; // The grain of the pencil.

	// Freehand kinds: the smoothed center line (a curve goes through the
	// points). The other kinds: two points, where the drag started and
	// where it ended (the tip of an arrow, opposite corners of the box).
	std::vector<DrawPoint> points;

	friend bool operator==(const DrawShape &a, const DrawShape &b) = default;
};

// The rectangle (layer coordinates) nothing of the shape is painted
// outside of. Empty for a shape without points.
[[nodiscard]] QRectF DrawShapeBounds(const DrawShape &shape);

// A drawing layer content of this size (the size of the canvas when the
// layer is made). Shapes are validated: widths and opacities clamped,
// broken points dropped.
[[nodiscard]] ContentPtr MakeDrawContent(
	QSize size,
	std::vector<DrawShape> shapes = {});
[[nodiscard]] bool IsDrawContent(const ContentPtr &content);
[[nodiscard]] int DrawShapeCount(const ContentPtr &content);
[[nodiscard]] std::vector<DrawShape> DrawShapes(const ContentPtr &content);
// The same drawing with one more shape on top (the shapes are shared, so
// this is cheap). Null if content is not a drawing or the shape is empty.
[[nodiscard]] ContentPtr DrawWithShape(
	const ContentPtr &content,
	DrawShape shape);

// JSON: {"type":"draw","v":1,"w":..,"h":..,"shapes":[{"k":"pen",
// "c":"#rrggbb","w":8,"o":1,"f":false,"s":0,"p":[x,y,w,x,y,w...]}]}.
// Reading is tolerant to out-of-range values, null for anything that is
// not a drawing.
[[nodiscard]] QByteArray SerializeDrawing(const ContentPtr &content);
[[nodiscard]] ContentPtr DeserializeDrawing(const QByteArray &json);

enum class TextLayerAlign : uchar {
	Left,
	Center,
	Right,
};

// Two weights are offered: Regular and Semibold, which is the heavy face
// of the font (a semibold one for the font of the interface, a bold one
// for the serif and the monospace fonts). Bold is the same as Semibold, it
// stays for the layers that were saved with it.
enum class TextLayerWeight : uchar {
	Regular,
	Semibold,
	Bold,
};

enum class TextLayerFont : uchar {
	Default, // The font of the interface.
	Serif,
	Mono,
};

struct TextLayerStyle {
	QString text; // Lines are separated by '\n'.
	QColor color = QColor(255, 255, 255); // Opaque.
	double size = 64.; // The font size, layer pixels.
	TextLayerWeight weight = TextLayerWeight::Semibold;
	TextLayerFont font = TextLayerFont::Default;
	TextLayerAlign align = TextLayerAlign::Left;
	bool plate = false; // A rounded plate behind the text.
	QColor plateColor = QColor(0, 0, 0);
	double plateOpacity = 0.6; // 0..1.

	friend bool operator==(
		const TextLayerStyle &a,
		const TextLayerStyle &b) = default;
};

// Main thread only (the text is shaped with the fonts of the app here,
// rendering then needs no fonts and works on any thread). The size of
// the content is the size of the text block with its padding.
[[nodiscard]] ContentPtr MakeTextContent(const TextLayerStyle &style);
[[nodiscard]] bool IsTextContent(const ContentPtr &content);
[[nodiscard]] std::optional<TextLayerStyle> TextStyleOf(
	const ContentPtr &content);

// A text layer whose block is placed at the canvas point: vertically
// centered on it, horizontally by the alignment (a left aligned text
// starts at the point). Main thread.
[[nodiscard]] Layer MakeTextLayer(
	const TextLayerStyle &style,
	QPointF canvasPoint);
// Replaces the text of a text layer (the content and, if the name was
// made from the old text, the name), keeping the side the text is aligned
// to in place. False if the layer is not a text one. Main thread.
bool SetLayerText(Layer &layer, const TextLayerStyle &style);

// JSON: {"type":"text","v":1,"text":"..","color":"#rrggbb","size":64,
// "weight":"semibold","font":"default","align":"left","plate":false,
// "plate_color":"#000000","plate_opacity":0.6}.
[[nodiscard]] QByteArray SerializeTextStyle(const TextLayerStyle &style);
[[nodiscard]] std::optional<TextLayerStyle> DeserializeTextStyle(
	const QByteArray &json);

} // namespace Oblivion::Photo
