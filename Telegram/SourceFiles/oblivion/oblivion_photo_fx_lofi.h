/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_fx.h"

class QDateTime;

// Photo editor: the "bad camera" pack, the effects of the FxGroup::Lofi
// group of oblivion_photo_fx.h. Everything is registered from the .cpp
// through an FxRegistrar, the ids are stable (stored in documents):
//
//   lofi.camera     the whole old digital camera in one effect: the photo
//                   is taken down to the sensor resolution, goes through
//                   the optics, the sensor, the processing, the date stamp
//                   and a real JPEG compression there and is brought back
//                   to the size of the layer. The one-click presets are
//                   parameter sets of this effect;
//   lofi.sensor     sensor resolution (downscale + upscale method);
//   lofi.jpeg       real JPEG re-compression (quality, re-saves, blocks);
//   lofi.noise      CCD noise, color noise, hot pixels, banding;
//   lofi.aberration chromatic aberration and purple fringing;
//   lofi.bloom      bloom and red halation;
//   lofi.flash      on-camera flash;
//   lofi.cast       white balance cast, washed-out colors;
//   lofi.sharpen    over-sharpening halos;
//   lofi.stamp      the date stamp (text, font, color, corner);
//   lofi.depth      color depth with dithering;
//   lofi.scanlines  scanlines and interlacing;
//   lofi.vhs        VHS tape;
//   lofi.pixelate   pixelate (squares, dots, LCD);
//   lofi.posterize  posterize;
//   lofi.halftone   halftone print.
//
// The presets (FxPreset, they only set parameters): lofi.preset_phone,
// lofi.preset_ccd, lofi.preset_webcam, lofi.preset_mms, lofi.preset_flash,
// lofi.preset_pocket, lofi.preset_cctv, lofi.preset_vhs.
//
// Lengths are in source pixels and follow FxContext::scale, the effects
// that have their own pixel grid (the camera, the sensor, JPEG) work at
// that nominal resolution in the preview too, so the preview is the
// export, only smaller. Random effects depend only on their Seed
// parameter and FxContext::seed.
//
// The functions below are the pure parts that are useful on their own.
// Any thread unless said otherwise.
namespace Oblivion::Photo {

struct Document;

// The JPEG image plugin of Qt is there (it always is in the app).
[[nodiscard]] bool LofiJpegAvailable();

// Compresses the image to JPEG in memory and reads it back, generations
// times (every re-save after the first one moves the 8 x 8 grid and the
// quality a little, the way a picture degrades when it is passed around).
// quality is 1..100. The alpha channel is kept as it is. Nothing happens
// without the JPEG plugin. False: cancelled or out of memory.
[[nodiscard]] bool LofiJpegRoundTrip(
	QImage &image,
	int quality,
	int generations = 1,
	const std::atomic<bool> *cancel = nullptr);

enum class LofiStampStyle : uchar {
	Segments, // Seven-segment digits of a film camera date back.
	Pixel, // 5 x 7 dot matrix.
	PixelBold,
	Camcorder, // The dot matrix with a dark shadow, like an OSD.
};
inline constexpr auto kLofiStampStyleCount = 4;

enum class LofiStampCorner : uchar {
	BottomRight,
	BottomLeft,
	TopRight,
	TopLeft,
};
inline constexpr auto kLofiStampCornerCount = 4;

struct LofiStamp {
	QString text;
	LofiStampStyle style = LofiStampStyle::Segments;
	QColor color = QColor(255, 151, 41);
	LofiStampCorner corner = LofiStampCorner::BottomRight;
	double size = 0.045; // The height of the digits, a part of the short side.
	double glow = 0.45; // 0..1
	double margin = 0.04; // From the edges, a part of the short side.
};

// Paints the stamp with the pixel fonts drawn in code (no QFont, so it is
// safe on a worker thread). The fonts have digits, punctuation, Latin and
// Cyrillic capitals: other characters are skipped. The stamp starts from
// its corner, a text wider than the picture is cut by the opposite edge
// (at any size of the picture, so the preview shows what the export has).
void LofiPaintStamp(QImage &image, const LofiStamp &stamp);

// Only the characters the stamp fonts can draw, at most 40 of them.
[[nodiscard]] QString LofiStampFilter(const QString &text);

enum class LofiStampDate : uchar {
	Classic, // '26 10 6
	Date, // 06.10.2026
	DateTime, // 06.10.2026 18:42
};
[[nodiscard]] QString LofiStampText(
	const QDateTime &moment,
	LofiStampDate format);

// The sample photo of the snapshot scenes at the size of a usual photo
// from a chat (1280 on the long side). The built-in picture is half of
// that: everything measured in pixels of the photo (sensors, JPEG blocks,
// noise, shifts) would look twice as strong on it as on a real photo.
// Main thread.
[[nodiscard]] QImage FxSceneSampleImage();
// The same photo as a document with one layer, for the editor scenes
// (EditorSceneArgs::document of oblivion_photo_editor.h).
[[nodiscard]] Document FxSceneSampleDocument();

// A contact sheet for OBLIVION_SELFTEST=ui: the sample photo as it is and
// with every effect (or every preset) of the group applied, with the
// names. Call it from the callback of a SelfTest::SceneRegistrar. Main
// thread.
void RegisterFxGalleryScene(QString name, FxGroup group, bool presets);

// The Layer tab of the editor alone, at its real width, with a preset
// added to the sample photo: every card of the stack with its header and
// parameters, as tall as it gets (the editor scenes only have room for
// the top of the first card). Call it from the callback of
// a SelfTest::SceneRegistrar. Main thread.
void RegisterFxStackScene(QString name, QByteArray preset);

} // namespace Oblivion::Photo
