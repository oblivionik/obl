/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QRect>
#include <QtGui/QImage>

#include <vector>

// On-device image analysis: text recognition (OCR) and background removal.
//
// The platform part lives in oblivion_vision_mac.mm (Apple Vision,
// VNRecognizeTextRequest / foreground instance masks), other systems get
// the stubs from oblivion_vision.cpp that report "not supported".
//
// Every function here is synchronous, keeps no global state and is safe
// to call from any thread: run it through crl::async and deliver the
// result with crl::on_main(guard, ...). Never call it on the main thread
// with a big image: a request usually takes 0.05-0.5 s (a 12 MP photo
// included), but the very first one of an application is much longer,
// macOS prepares the model for it once (38 s and 90 s were measured on
// a busy M5, the next launches of the same binary took 0.2 s).
//
// The UI over it (chat menu items, the result boxes) is in
// oblivion_vision_ui.h, the photo editor has its own "Remove background".
namespace Oblivion::Vision {

struct TextResult {
	// Recognized lines joined with '\n' in the reading order (top to
	// bottom, fragments of one visual row left to right, joined with
	// a space), may be empty: ok with an empty text means "no text found".
	QString text;
	int lines = 0; // Number of lines in text.
	bool ok = false; // false: error or not supported, see error.
	QString error; // Technical description for the log, not for the UI.
};

[[nodiscard]] bool TextRecognitionSupported();

// Accurate recognition with language correction, the languages are
// Russian, English and Ukrainian (those of them the system supports,
// on old systems English only).
[[nodiscard]] TextResult RecognizeText(const QImage &image);

struct MaskResult {
	// The source size, QImage::Format_ARGB32_Premultiplied, device pixel
	// ratio 1, everything except the found subjects (people, animals,
	// objects) transparent. The pixels of the subjects keep their exact
	// colors (multiplied by the soft mask at the edges).
	QImage cutout;

	// The smallest rectangle of cutout that has all the visible pixels,
	// see ContentBounds(). cutout.copy(bounds) crops to the subjects.
	QRect bounds;

	bool ok = false; // false: error, not supported or nothing found.
	bool nothingFound = false; // The request worked, but found no subjects.
	QString error; // Technical description for the log, not for the UI.
};

// False on systems where background removal can't work (on macOS it
// needs macOS 14 for VNGenerateForegroundInstanceMaskRequest).
[[nodiscard]] bool BackgroundRemovalSupported();
[[nodiscard]] MaskResult RemoveBackground(const QImage &image);

// The bounding rectangle of the pixels with alpha above the threshold
// (0..254), empty if there are none. Any format, opaque formats give
// the whole image.
[[nodiscard]] QRect ContentBounds(const QImage &image, int threshold = 8);

// Multiplies the image by a mask (1.0 keeps a pixel, 0.0 makes it
// transparent), the mask is a rows x columns array of floats with the
// given stride in bytes and is scaled (nearest) if its size differs.
// Returns Format_ARGB32_Premultiplied. Used by the platform code.
[[nodiscard]] QImage ApplyMask(
	const QImage &image,
	const float *mask,
	int columns,
	int rows,
	qsizetype stride);

namespace details {

// One recognized piece of text and its rectangle in image pixels (the
// origin is the top left corner).
struct TextFragment {
	QString text;
	QRectF box;
};

// Puts the fragments into the reading order: rows from top to bottom,
// the fragments of one row from left to right joined with a space.
// Fills text and lines only. Used by the platform code.
[[nodiscard]] TextResult ComposeText(std::vector<TextFragment> fragments);

} // namespace details

// Self-checks for OBLIVION_SELFTEST=vision, see oblivion_selftest.h.
// Runs before Core::Application exists (no Core::App(), no session).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Vision
