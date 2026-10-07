/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_set.h"
#include "oblivion/oblivion_room.h"

#include <QtGui/QColor>
#include <QtGui/QImage>

class QPainter;
class QPainterPath;

namespace Ui {
class RpWidget;
} // namespace Ui

// Round 5: the «Холст» tab of a room: drawing together.
//
// The board is one picture of canvas.width x canvas.height units (1920 x
// 1080) that every member sees the same way, scaled to the window. It is
// kept as vector strokes: a finished stroke is one POST and one
// "room.stroke" event for everybody, while it is being drawn its new
// points go out every 50 ms as volatile "room.stroke_live" events, so the
// others see the line grow. A late joiner (and a stream "resync") loads
// the whole board with GET /canvas and applies the events newer than its
// event_id. The order of the strokes is the seq the server gave them.
//
// The eraser is a stroke too. The board has one solid background colour
// and nothing under the strokes, so "clear the pixels" and "paint with the
// background colour" give the same picture, and the second one needs no
// separate layer.
//
// The tab registers itself (Rooms::TabRegistrar, id "canvas", order 300),
// nothing here is called by the room window directly.
namespace Oblivion::Rooms {

enum class CanvasTool : uchar {
	Pen,
	Marker, // A wide translucent pen.
	Eraser,
};
[[nodiscard]] QString CanvasToolName(CanvasTool tool); // "pen", ...

struct CanvasStroke {
	QString id; // 1..32 of [A-Za-z0-9_-], chosen by the author.
	int64 seq = 0; // The order of the server, 0 while it is not confirmed.
	uint64 userId = 0;
	CanvasTool tool = CanvasTool::Pen;
	QColor color = QColor(0, 0, 0);
	int alpha = 255; // 1..255.
	int size = 4; // 1..256, the width in canvas units.
	std::vector<QPoint> points; // Canvas units, 1..limit points.
};

// The limits of the server (hello.limits), with what it has today.
struct CanvasLimits {
	int strokes = 3000; // Per room.
	int strokePoints = 2000; // Per stroke.
	int canvasNumbers = 600000; // Coordinates per room, two for a point.
	int livePoints = 200; // Per "live" request.
};

// ---- Pure logic (OBLIVION_SELFTEST=room_canvas).

[[nodiscard]] bool ValidStrokeId(const QString &id);
[[nodiscard]] QString NewStrokeId();

// A stroke of the server or of another member: untrusted. Points out of
// the canvas are clamped, what is over maxPoints is cut, a stroke without
// an id or without points is dropped. userId is taken from "user_id".
[[nodiscard]] std::optional<CanvasStroke> ParseStroke(
	const QJsonObject &object,
	QSize canvas,
	int maxPoints);

// The body of POST .../canvas/strokes (no seq, no user_id).
[[nodiscard]] QJsonObject SerializeStroke(const CanvasStroke &stroke);
// The body of POST .../canvas/live: the points [from, from + count).
[[nodiscard]] QJsonObject SerializeLive(
	const CanvasStroke &stroke,
	int from,
	int count);

// Drops the points that change nothing in the line (Douglas-Peucker with
// the given tolerance in canvas units) and, if there still are more than
// limit, thins them evenly. The first and the last points always stay.
[[nodiscard]] std::vector<QPoint> SimplifyStroke(
	const std::vector<QPoint> &points,
	int limit,
	double tolerance = 0.8);

// A smooth line through the points. shown < 0: all of them, otherwise
// only that many (a fraction is a part of the next segment): the way
// a stroke of somebody else is revealed while it comes in portions.
[[nodiscard]] QPainterPath StrokePath(
	const std::vector<QPoint> &points,
	double shown = -1.);

// Paints one stroke, the painter is in canvas units.
void PaintStroke(
	QPainter &p,
	const CanvasStroke &stroke,
	const QColor &background,
	double shown = -1.);

// The strokes of a board in the order they are painted.
class CanvasModel final {
public:
	using StrokePtr = std::shared_ptr<const CanvasStroke>;

	// A stroke somebody else is drawing right now.
	struct Live {
		CanvasStroke stroke;
		crl::time updated = 0;
		double shown = 0.; // Points revealed so far.
	};

	// The whole board of a snapshot. Own strokes that wait for the answer
	// of the server stay unless the snapshot has them already.
	void reset(
		QSize size,
		const QColor &background,
		std::vector<CanvasStroke> &&strokes);

	// A finished stroke with its seq. false: it is here already. The
	// preview and the waiting own stroke with this id are replaced by it.
	bool add(CanvasStroke &&stroke);
	bool remove(const QString &id);
	void clear(const QColor &background);

	// An own stroke that was sent and is not confirmed yet: painted over
	// everything till the server gives it a place.
	void addPending(CanvasStroke &&stroke);
	bool removePending(const QString &id);

	// New points of a stroke in progress. false: nothing to show.
	bool addLive(CanvasStroke &&part, crl::time now, int maxPoints);
	// Previews nobody has finished for five seconds. true: some are gone.
	bool expireLive(crl::time now);
	// Reveals the points that came. true: there is more to reveal.
	bool advanceLive(crl::time elapsed);

	[[nodiscard]] QSize size() const {
		return _size;
	}
	[[nodiscard]] const QColor &background() const {
		return _background;
	}
	[[nodiscard]] const std::vector<StrokePtr> &strokes() const {
		return _strokes;
	}
	[[nodiscard]] const std::vector<StrokePtr> &pending() const {
		return _pending;
	}
	[[nodiscard]] const std::vector<Live> &lives() const {
		return _lives;
	}
	// Changes when the picture of strokes() can't be continued by
	// painting the new ones over the old (a removal, a clear, a stroke
	// that came out of order, a new snapshot).
	[[nodiscard]] int generation() const {
		return _generation;
	}
	[[nodiscard]] int numbers() const {
		return _numbers;
	}
	[[nodiscard]] bool contains(const QString &id) const;
	// The newest stroke of the user that is not in skip, for the undo.
	[[nodiscard]] QString lastOwn(
		uint64 userId,
		const base::flat_set<QString> &skip) const;
	// One more stroke of that many points would not fit.
	[[nodiscard]] bool full(const CanvasLimits &limits, int points) const;

private:
	void dropLive(const QString &id);

	QSize _size = QSize(1920, 1080);
	QColor _background = QColor(255, 255, 255);
	std::vector<StrokePtr> _strokes; // Ascending by seq.
	std::vector<StrokePtr> _pending;
	std::vector<Live> _lives;
	int _generation = 0;
	int _numbers = 0;

};

// The board as an image of the target size in pixels: the background and
// count strokes from the first one. Thread safe (works on its own data).
[[nodiscard]] QImage RenderCanvas(
	const std::vector<CanvasModel::StrokePtr> &strokes,
	int count,
	QSize canvas,
	const QColor &background,
	QSize target);

[[nodiscard]] bool RunCanvasSelfTest(QStringList &log);

// ---- For the snapshot scenes of the room modules (canvas, reactions,
// voice): a sample room and the content of its window with a tab chosen.

[[nodiscard]] Room::Descriptor SampleRoomDescriptor(bool owner);
[[nodiscard]] QWidget *CreateSampleRoomScene(
	not_null<Ui::RpWidget*> parent,
	Room::Descriptor &&descriptor,
	const QString &tab);

} // namespace Oblivion::Rooms
