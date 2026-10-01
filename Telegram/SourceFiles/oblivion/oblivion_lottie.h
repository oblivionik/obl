/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtGui/QImage>

// Lottie / .tgs toolkit used by the sticker studio and the gift catalog.
//
// Inside namespace Oblivion this namespace shadows lib_lottie's ::Lottie,
// so refer to lib_lottie types there as ::Lottie::FrameGenerator etc.
//
// Every function that takes "json" also accepts .tgs (gzip) bytes.
// All functions are synchronous and safe to call from any thread
// (callers run heavy ones through crl::async). The only shared state is
// a small thread-safe cache of parsed animations used by RenderFrame().
//
// Frame indices are rlottie frame numbers counted from "ip":
// [0, Info::frames), every frame of 60 fps animations included.
namespace Oblivion::Lottie {

// .tgs (gzip) or plain Lottie JSON bytes -> JSON bytes, empty on error.
// Like Images::UnpackGzip, gzip data unpacking to more than 5 MB is
// an error. Plain JSON is returned as is. Functions that build a JSON
// tree (ReadInfo, AdjustColors, ExportSvg) accept up to 5 MB of JSON.
[[nodiscard]] QByteArray Unpack(const QByteArray &data);

// Lottie JSON (up to 5 MB) -> .tgs (gzip) bytes, empty on error.
// Images::UnpackGzip reads the result, but lib_lottie plays it only if
// the JSON is at most ::Lottie::kMaxFileSize (2 MB).
[[nodiscard]] QByteArray PackTgs(const QByteArray &json);

struct Info {
	int frames = 0; // Frame count, "op" - "ip".
	double fps = 0.; // "fr".
	QSize size; // "w" x "h".
	crl::time duration = 0; // Milliseconds.

	[[nodiscard]] bool valid() const {
		return (frames > 0) && (fps > 0.) && !size.isEmpty();
	}
	explicit operator bool() const {
		return valid();
	}
};

// Reads "ip" / "op" / "fr" / "w" / "h" from the JSON without rlottie.
// Invalid Info (valid() == false) on error. Here and in Renderer::info()
// the frame rate is in [1, 1000] and the frame count and sides are
// bounded, so callers can do frame / duration math without overflows.
[[nodiscard]] Info ReadInfo(const QByteArray &json);

// Recolors every static and animated color in HSL space: fill and stroke
// colors, gradient color stops (offsets and opacity stops are kept),
// solid layer colors, text fill / stroke colors, color effect values and
// the emoji skin tone ("fitz") replacement table.
// hueDegrees rotates the hue (wraps around, so 360 == 0, -180 == 180),
// saturationPercent and lightnessPercent in [-100, 100] move saturation /
// lightness towards 0 (negative) or 1 (positive), 0 leaves them unchanged.
// Key order and untouched numbers are preserved (rlottie depends on the
// key order), the output is compact JSON. Empty on error.
[[nodiscard]] QByteArray AdjustColors(
	const QByteArray &json,
	int hueDegrees,
	int saturationPercent = 0,
	int lightnessPercent = 0);

// Renders any frame (random access, index in [0, frames), clamped)
// to an ARGB32 premultiplied image of the given size with
// a transparent background, the animation is scaled to fit and centered.
// Null QImage on error. Keeps the last two parsed animations in a cache,
// so calling it repeatedly with the same bytes is cheap.
[[nodiscard]] QImage RenderFrame(
	const QByteArray &json,
	int frame,
	QSize size);

// Keeps the parsed animation for repeated random access, for example
// while dragging a frame slider. Use from one thread at a time.
class Renderer final {
public:
	explicit Renderer(const QByteArray &json);
	~Renderer();

	[[nodiscard]] bool valid() const;
	[[nodiscard]] const Info &info() const; // From rlottie.

	// Same contract as RenderFrame().
	[[nodiscard]] QImage render(int frame, QSize size);

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

struct SvgResult {
	QByteArray svg; // Complete SVG document, empty on error.
	bool vector = false; // False if a raster frame is embedded as <image>.

	// Localized note about a raster fallback, empty for vector results.
	// Filled only when ExportSvg() runs on the main thread of a launched
	// app, otherwise use SvgRasterNote() on the main thread.
	QString note;

	// Technical (English) details for logs: why the raster fallback was
	// used, or what was approximated in a vector result. Usually empty.
	QString reason;
};

// Exports one frame (index in [0, frames), clamped) as an SVG document
// with viewBox "0 0 w h". Geometry comes from rlottie's own evaluation
// of the frame (keyframes, easing, parenting, precomps, time remapping,
// trim paths, repeaters, dashes are all resolved exactly like the raster
// renderer does), then paths, fills, strokes, gradients, masks, track
// mattes and layer opacity are written as SVG 1.1 elements.
// Falls back to an embedded PNG (vector == false) only if rlottie's
// render tree can't be converted (for example broken path data).
// To preview the result with QSvgRenderer set QtSvg::AssumeTrustedSource
// before load(): deep precomps nest more than the 32 levels Qt SVG draws
// by default (browsers and editors have no such limit).
[[nodiscard]] SvgResult ExportSvg(const QByteArray &json, int frame);

// Localized text for SvgResult::note of a raster fallback. Main thread.
[[nodiscard]] QString SvgRasterNote();

// Self-checks for OBLIVION_SELFTEST, appends human-readable lines to log.
// Runs on the main thread before Core::Application exists: QApplication,
// Qt resources (":/animations/...") and crl are ready, but there is no
// Core::App(), no session, no style and no event loop running.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Lottie
