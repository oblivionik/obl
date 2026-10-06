/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_video_core.h"
#include "oblivion/oblivion_video_fx.h"

#include <QtCore/QRectF>

// What the video editor (oblivion_video_editor.h) edits and how it becomes
// a file: clips cut from several videos and joined, one crop, rotation and
// speed for all of them, the sound kept or not, a stack of video effects
// (oblivion_video_fx.h) over the whole result.
//
// The geometry: every video is rotated and fitted into the canvas (the
// rotated frame of the first clip, the others are centered in it on black,
// on transparent in a sticker), the crop is a part of the canvas. The
// effects get the frames of the result: cropped, rotated and scaled, one
// after another with the time each of them has in the result.
//
// Everything is synchronous, keeps no global state and is safe to call
// from any thread, Export() goes through crl::async.
namespace Oblivion::VideoEdit {

inline constexpr auto kMinSpeed = 50; // Percent.
inline constexpr auto kMaxSpeed = 200;
inline constexpr auto kSpeedStep = 25;
inline constexpr auto kMinClipLength = crl::time(100);

// The longest results Export() makes, everything is encoded in memory.
inline constexpr auto kMaxVideoDuration = crl::time(20 * 60 * 1000);
inline constexpr auto kMaxGifDuration = crl::time(60 * 1000);

struct Source {
	QString path; // Any video or GIF the FFmpeg build here can decode.
	QByteArray content; // Used when path is empty.
	QString name; // The file name, for the titles.
	VideoCore::ClipInfo info;
};

struct Clip {
	int source = 0; // Index in Project::sources.
	crl::time from = 0;
	crl::time till = 0;

	[[nodiscard]] crl::time length() const {
		return till - from;
	}

	friend inline bool operator==(const Clip &, const Clip &) = default;
};

enum class Aspect : uchar {
	Original, // The whole canvas.
	Free,
	Square, // 1:1
	Portrait, // 4:5
	Story, // 9:16
	Wide, // 16:9
};

// Everything that is edited (and kept by the undo), sources are only added.
struct State {
	std::vector<Clip> clips;
	QRectF crop = QRectF(0., 0., 1., 1.); // A part of the canvas, 0..1.
	Aspect aspect = Aspect::Original;
	int rotation = 0; // Clockwise degrees: 0, 90, 180 or 270.
	int speed = 100; // Percent, kMinSpeed..kMaxSpeed.
	bool mute = false;

	// Applied to every frame of the result, in all the formats.
	VideoFx::Stack fx;

	friend inline bool operator==(const State &, const State &) = default;
};

struct Project {
	std::vector<Source> sources;
	State state;
};

// Width and height swapped for 90 and 270 degrees.
[[nodiscard]] QSize Rotated(QSize size, int rotation);

// The rotated frame of the first clip, empty without clips.
[[nodiscard]] QSize CanvasSize(const Project &project);

// The crop in the pixels of the canvas, at least 2x2.
[[nodiscard]] QRect CropRect(QSize canvas, QRectF crop);

// Where a frame of this (already rotated) size is shown in the canvas.
[[nodiscard]] QRectF FitRect(QSizeF size, QSizeF canvas);

// Width / height, 0 for Aspect::Original and Aspect::Free.
[[nodiscard]] float64 AspectRatio(Aspect aspect);

// The biggest crop of the proportions in the middle of the canvas.
// Aspect::Free keeps the current crop, Aspect::Original takes everything.
[[nodiscard]] QRectF AspectCrop(QSize canvas, Aspect aspect, QRectF current);

// The same part of the picture after the canvas is rotated by 90 degrees
// clockwise.
[[nodiscard]] QRectF RotatedCrop(QRectF crop);

// The sum of the clips as they are in the sources and as the result plays
// them (with the speed applied).
[[nodiscard]] crl::time SourceDuration(const State &state);
[[nodiscard]] crl::time OutputDuration(const State &state);

// The first source that no clip uses, neither in the state of the project
// nor in the given other states (the ones the undo goes back to): the
// editor gives its place to the next added video. -1 if all are used.
[[nodiscard]] int UnusedSource(
	const Project &project,
	const std::vector<State> &history);

// What is read from a source to show a clip in the result.
struct Placement {
	QRect source; // Displayed pixels of the source, before the rotation.
	QSize read; // The size the frames are read with, before the rotation.
	QRect target; // Where the rotated frame goes in the result.

	[[nodiscard]] bool empty() const {
		return source.isEmpty() || read.isEmpty() || target.isEmpty();
	}
};
[[nodiscard]] Placement ComputePlacement(
	QSize source,
	QSize canvas,
	QRect crop,
	QSize output,
	int rotation);

enum class Format : uchar {
	Mp4, // H.264 + AAC.
	GifVideo, // H.264 without an audio track: what Telegram calls a GIF.
	Gif, // A real .gif file.
	Sticker, // WebM VP9, 512 px, up to 3 seconds.
	Emoji, // WebM VP9, 100x100, up to 3 seconds.
};

[[nodiscard]] QString FormatExtension(Format format); // "mp4", "gif", "webm"

// For the preview of the effects in the editor: what Export() gives to the
// effects, made of a whole frame of a clip the way its source shows it.
//
// The size of those frames: the crop (in the pixels of the canvas) scaled
// down to fit the longer side, never up.
[[nodiscard]] QSize PreviewSize(QSize crop, int maxSide);

// frame: a whole frame of a source, upright, of any size. It is rotated,
// fitted into the canvas on black and the crop (in the pixels of the
// canvas) is taken from it. Always ARGB32_Premultiplied of the output size,
// black for a null frame.
[[nodiscard]] QImage ComposePreview(
	const QImage &frame,
	QSize canvas,
	QRect crop,
	QSize output,
	int rotation);

struct ExportOptions {
	Format format = Format::Mp4;
	int maxSide = 0; // The longer side, 0 = the default of the format.
	int maxFps = 0; // 0 = the default of the format.

	friend inline bool operator==(
		const ExportOptions &,
		const ExportOptions &) = default;
};

// The defaults of the format applied.
[[nodiscard]] int ExportMaxSide(const ExportOptions &options);
[[nodiscard]] int ExportMaxFps(const ExportOptions &options);

// The size of the result (of the content of a sticker), empty without
// clips. Videos are only scaled down, stickers have a side of 512 (100).
[[nodiscard]] QSize OutputSize(
	const Project &project,
	const ExportOptions &options);

// Whether the result has a sound track.
[[nodiscard]] bool OutputHasAudio(
	const Project &project,
	const ExportOptions &options);

// A guess of the mp4 size by the bitrate the encoder aims at, 0 for the
// other formats.
[[nodiscard]] int64 EstimateSize(
	const Project &project,
	const ExportOptions &options);

struct ExportResult {
	QByteArray content; // The whole file.
	bool ok = false;
	bool cancelled = false;
	QString error; // Technical description for the log, not for the UI.
	QSize size;
	crl::time duration = 0;
	int frames = 0;
	bool audio = false; // Has an audio track.
};

// Heavy and synchronous, call it off the main thread. progress (0..1)
// is called on the calling thread, cancel (may be null) is polled all
// the time. A sticker takes the first three seconds of the result.
// The effects of the state are applied to every frame, in the order the
// frames go in the result.
[[nodiscard]] ExportResult Export(
	const Project &project,
	const ExportOptions &options,
	Fn<void(float64)> progress,
	VideoCore::Cancel cancel);

// Self-checks for OBLIVION_SELFTEST=video_editor, see oblivion_selftest.h.
// Runs before Core::Application exists (no Core::App(), no session).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::VideoEdit
