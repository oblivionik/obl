/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QRectF>
#include <QtGui/QImage>
#include <QtGui/QRgb>
#include <QtGui/QTransform>

#include <atomic>
#include <optional>
#include <vector>

// Non-destructive photo editing core used by the photo editor UI.
//
// The source image is never modified: an EditState (a small value type,
// cheap to copy, comparable, JSON-serializable for undo / redo and user
// presets) describes every edit, and Render() produces a new image from
// the source and the state.
//
// Threading: everything except the *Name() functions is synchronous,
// keeps no mutable global state and is safe to call from any thread
// (run Render() / Thumbnail() / AutoEnhance() / Load / Save through
// crl::async). Heavy per-pixel passes are split into row bands that run
// in parallel on crl::async workers plus the calling thread, the result
// never depends on the thread count or on timing (bit-exact for the same
// input). The *Name() functions use the current language, main thread.
//
// Pixel format: every returned image is QImage::Format_ARGB32_Premultiplied
// with device pixel ratio 1 and the color space of the source (LoadImage()
// converts to sRGB). Other source formats are converted once per call.
// Alpha is preserved: color edits work on un-premultiplied values and never
// change alpha; blurs work on premultiplied values (no dark fringes).
//
// Rendering pipeline, in this order:
//  1. Geometry: quarter turns, flips, straighten, crop (EditState order).
//  2. One fused per-pixel color pass: exposure + temperature / tint in
//     linear light, then whites, blacks, brightness, contrast, fade as
//     per-channel tone curves, highlights / shadows on luma, saturation /
//     vibrance, then the filter look mixed in by filterIntensity.
//  3. Clarity (large radius local contrast) and sharpen (unsharp mask).
//  4. Gaussian blur.
//  5. Effects, in list order, only those with enabled == true.
//  6. Vignette and film grain (the filter's own vignette / grain added).
//
// Scale independence: all spatial sizes (blur radii, grain size, pixelate
// blocks, glitch offsets, halftone dots, ...) are relative to the shorter
// side of the rendered image, so a downscaled preview looks like
// the full-resolution export, just smaller. Exceptions on tiny renders:
// pixelate blocks are at least 2 px, halftone cells 3 px, the sharpen
// radius is kept in 0.6..3 px; grain finer than a pixel is weakened the
// way downscaling would average it.
//
// Performance (M5, 10 cores, -O3, machine under load; the self-test
// OBLIVION_SELFTEST=photo prints the current numbers): 1920x1080 with
// a typical edit (tone + color + clarity + sharpen + vignette + grain +
// filter) ~17 ms, exposure + contrast + saturation ~3 ms, any single
// filter 4-11 ms, effects 5-12 ms (halftone ~25 ms); 12 MP export with
// the typical edit ~0.12 s, plus straighten + glow + lens blur ~0.19 s;
// 19 filter thumbnails 96x96 ~4 ms; 12 MP -> 1280 preview ~14 ms.
//
// Recommended UI usage:
//  - on open: source = LoadImage(path) (EXIF orientation applied, sRGB);
//  - preview = PrepareSource(source, viewport size * device pixel ratio),
//    once, then Render(preview, state, viewportSize) on every change
//    (crl::async with a generation counter / the cancel flag, drop stale
//    results); re-prepare with a larger size when the user zooms in;
//  - crop tool: render with the crop reset (copy the state, set crop to
//    QRectF(0, 0, 1, 1)) and draw EditState::crop over it, crop is
//    normalized to that image;
//  - filter strip: FilterThumbnails(preview, state, side), once per
//    geometry / adjustment change, off the main thread;
//  - export: Render(source, state) (empty maxSize = full resolution),
//    then EncodeImage() / SaveImage().
namespace Oblivion::Photo {

// Adjustment sliders. Every value is an int in [min, max] of AdjustList().
enum class Adjust : uchar {
	Exposure, // -100..100, +-2 EV, in linear light.
	Brightness, // -100..100, midtone gamma, black / white points kept.
	Contrast, // -100..100, S-curve around the middle gray.
	Highlights, // -100..100, negative recovers bright areas.
	Shadows, // -100..100, positive lifts dark areas.
	Whites, // -100..100, moves the white end (positive may clip).
	Blacks, // -100..100, moves the black end (negative crushes).
	Saturation, // -100..100, -100 gives exact grayscale (R == G == B).
	Vibrance, // -100..100, affects muted colors more than saturated ones.
	Temperature, // -100..100, positive is warmer (more yellow).
	Tint, // -100..100, positive is more magenta, negative more green.
	Fade, // 0..100, lifts blacks and lowers contrast (matte look).
	Clarity, // -100..100, local contrast of midtones, negative softens.
	Sharpen, // 0..100, unsharp mask.
	Vignette, // -100..100, positive darkens edges, negative lightens.
	VignetteFeather, // 0..100, default 50, softness of the vignette edge.
	Grain, // 0..100, monochrome film grain.
	GrainSize, // 0..100, default 25.
	Blur, // 0..100, gaussian blur of the whole image.
};
inline constexpr auto kAdjustCount = 19;

struct AdjustInfo {
	Adjust id = Adjust::Exposure;
	int min = 0;
	int max = 0;
	int defaultValue = 0;
	const char *key = nullptr; // JSON key, e.g. "exposure".
};

// All adjustments in the recommended UI order (the enum order).
[[nodiscard]] const std::vector<AdjustInfo> &AdjustList();
[[nodiscard]] const AdjustInfo &AdjustDescriptor(Adjust id);
[[nodiscard]] QString AdjustName(Adjust id); // Localized, main thread.

enum class EffectType : uchar {
	Pixelate,
	Glitch, // RGB split + shifted horizontal slices.
	Vhs, // Scanlines, chroma bleed, noise, tracking wobble.
	Posterize,
	Duotone, // Luminance mapped from color1 (shadows) to color2 (lights).
	Halftone, // Rotated dot screen, mono or CMY.
	Emboss,
	ChromaticAberration, // Radial red / blue fringes.
	Glow, // Soft glow / bloom of the bright areas.
	BlackWhite, // Grayscale with a red / green / blue channel mix.
	Sepia,
	Invert,
	Film, // Film stock curve, faded blacks, warm / cyan cast, grain.
	LensBlur, // Tilt-shift: sharp band, progressively blurred outside.
};
inline constexpr auto kEffectTypeCount = 14;

// Effect parameters. Which of them an effect uses, their ranges and
// defaults come from EffectParams(type), the rest are ignored (and are
// neither compared nor serialized).
enum class EffectParam : uchar {
	Amount, // Strength / mix with the unprocessed image, 0..100.
	Size, // Pixelate block, glitch slice height, halftone dot, emboss
	// depth, glow radius; 0..100.
	Levels, // Posterize levels per channel, 2..16.
	Red, // Black & white channel mix, percent, -100..200.
	Green,
	Blue,
	Position, // Lens blur band center, 0..100 (0 = top for angle 0).
	Width, // Lens blur sharp band size, 0..100 (100 = whole image).
	Feather, // Lens blur transition size, 0..100.
	Angle, // Degrees: halftone screen 0..90, emboss light 0..359, lens
	// blur band -90..90 (positive rotates clockwise).
	Seed, // Glitch / VHS random variant, 0..999.
	Mode, // Halftone: 0 = black dots, 1 = color (CMY) dots. A toggle.
	Grain, // Film grain, 0..100.
	Threshold, // Glow: brightness where the glow starts, 0..100.
	Color1, // Duotone shadows color, Effect::color1 (not an int value).
	Color2, // Duotone highlights color, Effect::color2.
};

struct Effect {
	EffectType type = EffectType::Pixelate;
	bool enabled = true; // Disabled effects are skipped but kept.

	int amount = 100;
	int size = 50;
	int levels = 5;
	int red = 30;
	int green = 59;
	int blue = 11;
	int position = 50;
	int width = 25;
	int feather = 40;
	int angle = 0;
	int seed = 0;
	int mode = 0;
	int grain = 35;
	int threshold = 60;
	QRgb color1 = 0xFF1B2A6BU; // Opaque, alpha is ignored.
	QRgb color2 = 0xFFF6B26BU;

	// Numeric parameters (not Color1 / Color2). setValue() clamps
	// to the range of this effect type.
	[[nodiscard]] int value(EffectParam param) const;
	void setValue(EffectParam param, int value);

	// Compares the type, enabled and the parameters this type uses.
	friend bool operator==(const Effect &a, const Effect &b);
};

struct EffectParamInfo {
	EffectParam param = EffectParam::Amount;
	int min = 0;
	int max = 100;
	int defaultValue = 0;
	bool toggle = false; // Mode: show a checkbox, values 0 / 1.
	bool color = false; // Color1 / Color2: show a color picker.
};

// All effect types in the recommended UI order.
[[nodiscard]] const std::vector<EffectType> &EffectTypes();
// Parameters of the type in the recommended UI order.
[[nodiscard]] const std::vector<EffectParamInfo> &EffectParams(
	EffectType type);
[[nodiscard]] Effect DefaultEffect(EffectType type);
[[nodiscard]] QString EffectKey(EffectType type); // JSON id, "glitch".
[[nodiscard]] std::optional<EffectType> EffectFromKey(const QString &key);
[[nodiscard]] QString EffectName(EffectType type); // Localized, main thread.
[[nodiscard]] QString EffectParamName(EffectParam param); // Same.

// Filter presets ("looks"): tone curves, channel gains, color matrix,
// saturation and split toning, some with their own vignette and grain.
// Ids are stable (they are stored in JSON), "original" is always first
// and means no filter. Unknown ids render like "original".
[[nodiscard]] const std::vector<QString> &FilterIds();
[[nodiscard]] bool FilterExists(const QString &id);
[[nodiscard]] QString FilterName(const QString &id); // Localized, main.

inline const auto kOriginalFilter = QStringLiteral("original");

struct EditState {
	// Geometry, applied in this order:
	// 1. quarterTurns clockwise 90 degree turns, 0..3;
	// 2. flipHorizontal / flipVertical mirror the turned image
	//    (in the display frame, after the turns);
	// 3. straighten rotates the content by -45..45 degrees around the
	//    image center (positive is clockwise) and zooms in just enough
	//    to cover the whole frame, so there are never empty corners and
	//    the frame keeps the size of the turned image;
	// 4. crop, normalized to that frame: (0, 0, 1, 1) is everything.
	//    Its pixel edges are rounded: left = round(x * width) etc.
	QRectF crop = QRectF(0., 0., 1., 1.);
	int quarterTurns = 0;
	double straighten = 0.;
	bool flipHorizontal = false;
	bool flipVertical = false;

	// Adjustments, see enum Adjust for ranges and meaning.
	int exposure = 0;
	int brightness = 0;
	int contrast = 0;
	int highlights = 0;
	int shadows = 0;
	int whites = 0;
	int blacks = 0;
	int saturation = 0;
	int vibrance = 0;
	int temperature = 0;
	int tint = 0;
	int fade = 0;
	int clarity = 0;
	int sharpen = 0;
	int vignette = 0;
	int vignetteFeather = 50;
	int grain = 0;
	int grainSize = 25;
	int blur = 0;

	// Filter preset id from FilterIds() and its strength 0..100.
	QString filter = kOriginalFilter;
	int filterIntensity = 100;

	// Applied in order after the adjustments, at most kMaxEffects.
	std::vector<Effect> effects;

	[[nodiscard]] int value(Adjust id) const;
	void setValue(Adjust id, int value); // Clamps to the range.

	// Crop compares fuzzily (QRectF), everything else exactly.
	friend bool operator==(
		const EditState &a,
		const EditState &b) = default;
};
inline constexpr auto kMaxEffects = 32;

// True if a full-size Render() returns the source pixels unchanged
// (VignetteFeather / GrainSize alone, a filter at intensity 0 or
// "original" and disabled effects change nothing).
[[nodiscard]] bool IsIdentity(const EditState &state);
// Crop, turns, flips or straighten.
[[nodiscard]] bool HasGeometry(const EditState &state);
// Adjustments, filter or enabled effects (anything besides geometry).
// Use for "Reset" button states and "edited" badges.
[[nodiscard]] bool HasColorEdits(const EditState &state);

// Clamps every field to its valid range (crop inside the frame and at
// least 0.001 of it, quarterTurns 0..3, non-finite values reset,
// empty filter -> "original", effects clamped and cut to kMaxEffects).
// Render() and ToJson() normalize internally.
[[nodiscard]] EditState Normalized(EditState state);

// The same state without geometry (for "copy edits" / user presets).
[[nodiscard]] EditState WithoutGeometry(EditState state);

// JSON, e.g. {"v":1,"rotate":1,"adjust":{"exposure":20},"filter":"film",
// "effects":[{"type":"glitch","amount":50,"size":40,"seed":0}]}.
// Only non-default values are written, so it is compact and stable:
// FromJson(ToJson(state)) == Normalized(state) and ToJson() of equal
// states gives identical bytes. FromJson() is tolerant: missing or bad
// fields get defaults, numbers are clamped, unknown effects are skipped.
[[nodiscard]] QJsonObject ToJson(const EditState &state);
[[nodiscard]] EditState FromJson(const QJsonObject &object);
[[nodiscard]] QByteArray Serialize(const EditState &state); // Compact.
[[nodiscard]] std::optional<EditState> Deserialize(const QByteArray &json);

// Geometry helpers (all sizes in pixels).
[[nodiscard]] QSize OrientedSize(QSize source, int quarterTurns);
// Zoom that straighten applies to cover a frame of this size.
[[nodiscard]] double StraightenScale(QSizeF frame, double degrees);
// Full-resolution size of Render(source, state).
[[nodiscard]] QSize OutputSize(QSize source, const EditState &state);
// Size of Render(source, state, maxSize): OutputSize fitted into maxSize
// keeping the aspect ratio, never upscaled (empty maxSize = full size).
[[nodiscard]] QSize OutputSize(
	QSize source,
	const EditState &state,
	QSize maxSize);
[[nodiscard]] QSize FitSize(QSize size, QSize maxSize); // Never upscales.
// Maps source pixel coordinates (0..width, 0..height) to the coordinates
// of a render of the given output size (use OutputSize()). Invert it to
// map a click on the rendered image back to the source.
[[nodiscard]] QTransform OutputTransform(
	QSize source,
	const EditState &state,
	QSize output);

// Converts to Format_ARGB32_Premultiplied and downscales (area averaging)
// to fit maxSize, never upscales. Use for the interactive preview source.
[[nodiscard]] QImage PrepareSource(const QImage &image, QSize maxSize = {});

// Renders the edited image. maxSize limits the output size (see
// OutputSize), an empty maxSize renders at full resolution. Geometry
// without straighten is pixel-exact (no resampling unless downscaled).
// If cancelled is not null it is polled between the pipeline stages, the
// result is a null QImage if it became true. It must outlive the call.
// Null QImage for a null source or if the memory can't be allocated.
[[nodiscard]] QImage Render(
	const QImage &source,
	const EditState &state,
	QSize maxSize = QSize(),
	const std::atomic<bool> *cancelled = nullptr);

// Everything of Render() except the geometry (adjustments, filter,
// effects, vignette, grain), applied in place to an image of any size:
// what the layer effects that wrap this pipeline use. The image is
// converted to Format_ARGB32_Premultiplied and detached. False if the
// image is null or it was cancelled (the pixels are undefined then).
[[nodiscard]] bool ApplyEdits(
	QImage &image,
	const EditState &state,
	const std::atomic<bool> *cancelled = nullptr);

// Square side x side preview of the edited image: the geometry output is
// scaled to cover the square and center-cropped, then the whole pipeline
// runs at that size. Pass the PrepareSource() image, not the full source.
[[nodiscard]] QImage Thumbnail(
	const QImage &source,
	const EditState &state,
	int side);

struct FilterThumbnail {
	QString id;
	QImage image;
};
// Thumbnail() for every FilterIds() entry (in that order) with the given
// state (its geometry, adjustments and effects) and that filter at
// intensity 100. The geometry is computed once, filters run in parallel.
[[nodiscard]] std::vector<FilterThumbnail> FilterThumbnails(
	const QImage &source,
	const EditState &state,
	int side);

// Histogram-based automatic correction: black / white points, midtone
// brightness, contrast, shadows / highlights recovery, vibrance and
// a partial gray-world white balance. Returns a state with only these
// adjustments set (geometry, filter and effects default): copy its
// adjustments into the current state (value() / setValue()).
[[nodiscard]] EditState AutoEnhance(const QImage &source);

// Loading: QImageReader with the EXIF orientation applied, converted to
// sRGB (if the file has another color profile, e.g. Display P3) and to
// Format_ARGB32_Premultiplied. Null QImage on error, error gets
// a technical English description (for logs). Qt's image allocation
// limit applies (256 MB of pixels by default, about 64 MP).
[[nodiscard]] QImage LoadImage(const QString &path, QString *error = nullptr);
[[nodiscard]] QImage LoadImage(
	const QByteArray &bytes,
	QString *error = nullptr);

enum class SaveFormat : uchar {
	Png, // Lossless, keeps alpha. Quality is ignored.
	Jpeg, // Transparency is flattened onto white. Optimized, progressive.
	Webp, // Keeps alpha. Quality 100 is lossless.
};

// Checked at runtime with QImageWriter::supportedImageFormats().
[[nodiscard]] bool SaveFormatSupported(SaveFormat format);
[[nodiscard]] std::vector<SaveFormat> SupportedSaveFormats();
[[nodiscard]] QString SaveFormatExtension(SaveFormat format); // "jpg"
[[nodiscard]] QString SaveFormatMimeType(SaveFormat format); // "image/jpeg"

// quality 0..100 (Jpeg / Webp). Empty on error / unsupported format.
[[nodiscard]] QByteArray EncodeImage(
	const QImage &image,
	SaveFormat format,
	int quality = 92);
// Writes atomically (QSaveFile). False on error.
[[nodiscard]] bool SaveImage(
	const QImage &image,
	const QString &path,
	SaveFormat format,
	int quality = 92);

// Self-checks for OBLIVION_SELFTEST=photo, appends human-readable lines
// (correctness checks and timings) to log. Returns false only on real
// failures (timings are only reported).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Photo
