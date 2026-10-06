/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "lang/lang_keys.h"

#include <QtCore/QRectF>
#include <QtGui/QImage>

#include <memory>
#include <vector>

// Video effects: a stack of abstract effects for the whole project of
// the video editor (blob / motion tracking overlay, edge glow, feedback
// trails, frame difference, slit-scan, pixel sorting, RGB split,
// displacement, kaleidoscope, ASCII, halftone, threshold, false colour,
// glitch, scanlines, grain, strobe), applied to the frames of the live
// preview and of the export (oblivion_video_project.h over
// oblivion_video_core.h). Portable C++ only, the analysis runs on
// a reduced frame.
//
// The engine: a registry of the effects with the descriptions of their
// parameters (the UI is generated from it), a stack of effects as a plain
// value (kept by the undo of the editor, saved as JSON) and a Processor
// that takes the frames of a video one by one, in order.
//
// Everything a result depends on is the frames, their positions and the
// stack: no clocks, no global random state. Sizes in the parameters are
// "pixels of a 720p frame" and the time is in seconds, so a small preview
// and a big export look the same. Rows of a frame are processed in
// parallel (crl::async), the result does not depend on the number of
// threads.
//
// No global state: any number of processors may work at once on
// different threads, one processor is used from one thread at a time.
//
// How it is used:
// - the export: one Processor for the whole result, every frame after it
//   is composed (cropped, rotated, scaled) goes through process() with
//   the time it gets in the result;
// - the playback of the preview: the same, on the thread that reads the
//   frames, with setStack() between the frames when the user moves
//   a slider;
// - a paused frame of the preview: ProcessStill().
namespace Oblivion::VideoFx {

enum class Type : uchar {
	Tracking, // Blobs with ids, boxes, lines, labels, trails.
	Edges, // Edge detection with a glow.
	Feedback, // Video feedback: echo trails with zoom and rotation.
	Difference, // What has changed since the previous frame.
	SlitScan, // Every row (column, ring) comes from another moment.
	PixelSort,
	RgbSplit,
	Displace, // Pixels are pushed by the brightness of the picture.
	Kaleidoscope,
	Mirror,
	Ascii,
	Halftone,
	Dither,
	Threshold,
	Posterize,
	FalseColor, // Thermal camera and the like.
	Glitch,
	Datamosh, // The old picture is moved by the motion of the new one.
	Crt, // Scanlines, RGB mask, curved screen.
	Grain,
	Strobe,

	kCount,
};

// For the sections of the "add an effect" menu.
enum class Group : uchar {
	Analysis,
	Time,
	Distort,
	Stylize,

	kCount,
};
[[nodiscard]] tr::phrase<> GroupName(Group group);

enum class ParamKind : uchar {
	Slider, // A real number.
	Integer,
	Toggle, // 0 or 1.
	Choice, // An index in Param::options.
	Color, // 0xRRGGBB as a number.
	Seed, // An integer, the UI adds a "randomize" button to it.
};

// What follows the value in the UI, see FormatValue().
enum class Unit : uchar {
	Plain,
	Percent,
	Degrees,
	Seconds,
	Hertz,
};

struct Param {
	const char *id = ""; // Stable, used in the saved stacks.
	tr::phrase<> name = {};
	ParamKind kind = ParamKind::Slider;
	Unit unit = Unit::Plain;
	float64 min = 0.;
	float64 max = 1.;
	float64 value = 0.; // The default one.
	float64 step = 1.; // Values are multiples of it (counted from min).
	std::vector<tr::phrase<>> options; // ParamKind::Choice.
};

struct Info {
	Type type = Type::Tracking;
	const char *id = ""; // Stable, used in the saved stacks.
	tr::phrase<> name = {};
	Group group = Group::Stylize;

	// Keeps something between the frames: needs them in order and
	// a pre-roll after a seek (Processor::feed()).
	bool temporal = false;

	std::vector<Param> params;
};

// All the effects, in the order they are offered to the user.
[[nodiscard]] const std::vector<Info> &Effects();
[[nodiscard]] const Info &EffectInfo(Type type);

// nullptr / -1 if there is nothing with such an id.
[[nodiscard]] const Info *FindEffect(const QString &id);
[[nodiscard]] int FindParam(Type type, const QString &id);

// One effect of a stack. A plain value: compare, copy, keep in the undo.
struct Entry {
	Type type = Type::Tracking;
	bool enabled = true;

	// How much of the effect is seen, 0..1: the result is blended with
	// what the effect has got. Works for every effect.
	float64 mix = 1.;

	// As many as EffectInfo(type).params has, in the same order.
	std::vector<float64> values;

	friend inline bool operator==(const Entry &, const Entry &) = default;
};

// Applied from the first one to the last one.
using Stack = std::vector<Entry>;

// With the default values of all the parameters.
[[nodiscard]] Entry MakeEntry(Type type);

// The right number of values, each one inside its range and on its step
// (missing and non-finite ones become the defaults). The processor does
// it itself, the UI may use it for what it shows.
[[nodiscard]] Entry Sanitized(Entry entry);
[[nodiscard]] float64 SanitizedValue(const Param &param, float64 value);

// "35%", "1.5 s", "90" with a degree sign, the name of the chosen option,
// "#FF8800"; nothing for a toggle.
// Reads the language pack: for the UI, on the main thread.
[[nodiscard]] QString FormatValue(const Param &param, float64 value);

// Whether the stack changes the frames at all (something is enabled and
// has a non-zero mix).
[[nodiscard]] bool HasEnabled(const Stack &stack);

// Whether some enabled effect keeps a state between the frames.
[[nodiscard]] bool IsTemporal(const Stack &stack);

// How much of the video before a frame the stack wants to see to show
// that frame the way it looks while playing, 0 without temporal effects,
// never more than three seconds. See Processor::feed().
[[nodiscard]] crl::time Preroll(const Stack &stack);

// JSON, for presets of the user and saved projects. Unknown effects and
// parameters are skipped when reading, missing ones get the defaults.
[[nodiscard]] QByteArray Serialize(const Stack &stack);
[[nodiscard]] Stack Deserialize(const QByteArray &data);

// Ready stacks that only set the parameters.
struct Preset {
	const char *id = "";
	tr::phrase<> name = {};
	Stack stack;
};
[[nodiscard]] const std::vector<Preset> &Presets();

// What the tracking effect follows, for tests and overlays of the UI.
struct Blob {
	int id = 0; // Stays the same while the blob is followed.
	QRectF box; // In parts of the frame, 0..1.
};

// Applies a stack to the frames of one video, in the order they are shown.
//
// Not thread-safe: one thread at a time (it may be another thread for
// the next frame). Can't be copied or moved, keep it in a unique_ptr.
// Cheap to create, the buffers appear with the first frame that needs
// them: usually a few frames, the slit-scan keeps up to 128 MB of them
// while it is enabled.
class Processor final {
public:
	Processor();
	explicit Processor(Stack stack);
	~Processor();

	// May be called between any two frames. Effects that stay at their
	// places in the stack keep their states (trails don't disappear when
	// a slider is moved), the others start from scratch.
	void setStack(Stack stack);
	[[nodiscard]] const Stack &stack() const;

	// The same as the functions above for the current stack.
	[[nodiscard]] bool empty() const; // process() changes nothing.
	[[nodiscard]] bool temporal() const;
	[[nodiscard]] crl::time preroll() const;

	// Forgets everything that was seen. Call it before a frame that
	// doesn't follow the previous one (a seek, a paused frame shown again
	// after a change of the parameters).
	void reset();

	// frame: any format and size, the result is ARGB32_Premultiplied of
	// the same size (the frame itself if the stack changes nothing).
	// position: the time of the frame in the result, in ms.
	//
	// Frames must come in order. A position before the previous one,
	// a jump over more than three seconds or another frame size reset
	// the states like reset() does.
	[[nodiscard]] QImage process(QImage frame, crl::time position);

	// The same without a result, for the pre-roll after a seek: reset(),
	// feed() the frames of preroll() before the wanted one (a reduced
	// frame rate is fine), then process() the wanted one. Does only what
	// the temporal effects need, nothing at all without them.
	void feed(const QImage &frame, crl::time position);

	// What the first enabled tracking effect follows after the last
	// frame, sorted by id. Empty without one.
	[[nodiscard]] std::vector<Blob> blobs() const;

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

// One frame without a history, for thumbnails of the effects.
[[nodiscard]] QImage Apply(
	const Stack &stack,
	QImage frame,
	crl::time position = 0);

// A frame the way it looks while the video plays, for a paused preview
// and for the frame shown after a seek: resets the processor, shows it
// what frameAt gives for the moments before the position (every `step`
// ms, as far back as Processor::preroll() asks, never before zero) and
// returns the processed frame of the position itself.
//
// frameAt gets times of the result in ascending order and is called on
// the calling thread, null frames are skipped. cancelled (may be null)
// is asked before every frame: a null image is returned if it says yes.
[[nodiscard]] QImage ProcessStill(
	Processor &processor,
	crl::time position,
	Fn<QImage(crl::time)> frameAt,
	crl::time step = 66,
	Fn<bool()> cancelled = nullptr);

// Self-checks for OBLIVION_SELFTEST=video_fx, see oblivion_selftest.h.
// No Core::App(), no session: pure logic only. Logs the speed of every
// effect on a 720p frame. With a folder in OBLIVION_VFX_DUMP it also
// saves there what every effect, option and preset makes of the test
// video, as PNG files.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::VideoFx
