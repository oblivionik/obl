/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_core.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPointF>
#include <QtGui/QColor>
#include <QtGui/QImage>

#include <atomic>
#include <optional>
#include <vector>

namespace Ui {
class RpWidget;
} // namespace Ui

// Photo editor: the registry of layer effects ("fx").
//
// An effect is a descriptor (FxDescriptor): a stable id, a group, a name,
// a list of parameter descriptors and an apply() function that changes an
// image in place. A layer keeps a stack of FxInstance values (effect id +
// parameter values), the compositor of oblivion_photo_doc.h applies the
// enabled ones in order to the pixels of the layer.
//
// Effects are registered from their own .cpp through a static FxRegistrar
// (the same way the UI snapshot scenes are): the registrar only stores a
// callback during the static initialization, the callbacks run once, on
// the first use of the registry, from any thread. So the registration code
// may use everything that is ready by then, but must not need the main
// thread: descriptors only keep lang phrases (FxText), they don't resolve
// them. Names are resolved by the panels on the main thread.
//
//   namespace {
//   const auto Registered = Oblivion::Photo::FxRegistrar([] {
//       using namespace Oblivion::Photo;
//       RegisterFx({
//           .id = "blur.gaussian",
//           .group = FxGroup::Blur,
//           .name = tr::lng_oblivion_photo_blur_gaussian,
//           .params = {
//               FxPixels("radius", tr::lng_oblivion_photo_blur_radius,
//                   0., 250., 12.),
//           },
//           .flags = kFxNeighbours,
//           .apply = [](
//                   QImage &image,
//                   const FxParams &params,
//                   const FxContext &context) {
//               FxGaussianBlur(image, context.px(params.number("radius")));
//               return !context.cancelled();
//           },
//       });
//   });
//   } // namespace
//
// The apply() contract:
//  - it runs on a worker thread (never the main one), several effects may
//    run at the same time on different images, so no mutable globals;
//  - image is Format_ARGB32_Premultiplied, device pixel ratio 1, not null
//    and detached (FxPrepare() was called), change it in place or assign
//    a new image of the same size and format: the pixels are the local
//    rectangle of the layer, pixel (x, y) is the layer point
//    (x / scale, y / scale);
//  - params are complete and valid: every parameter of the descriptor is
//    there, clamped to its range (see NormalizedFxParams);
//  - the same image and parameters must give the same pixels (the seed
//    comes in the parameters and in the context, no std::rand, no time);
//  - context.scale is "rendered pixels per source pixel": 1 for an export,
//    less for the downscaled preview. Every length given in source pixels
//    (FxPixels parameters, blur radii, offsets, cell sizes) must be
//    multiplied by it (context.px()), and noise / pattern coordinates
//    divided by it, so the preview looks like the export, just smaller;
//  - poll context.cancelled() between the stages of a slow effect and
//    return false as soon as it is true (the image is dropped then);
//  - heavy loops go through FxParallel / FxParallelRows / FxForEachColor.
//
// Coordinates: FxPoint parameters are normalized to the layer (0, 0 is
// the top left corner, 1, 1 the bottom right one), angles are degrees,
// positive is clockwise on the screen.
namespace Oblivion::Photo {

class Controller;

// A user-visible text of a descriptor: a lang phrase, a fixed string
// (for things that are not translated, like "JPEG") or a function.
// now() must be called on the main thread.
class FxText final {
public:
	FxText() = default;
	FxText(tr::phrase<> phrase);
	FxText(QString fixed);
	[[nodiscard]] static FxText Custom(Fn<QString()> compute);

	[[nodiscard]] bool empty() const;
	[[nodiscard]] QString now() const;

private:
	enum class Type : uchar {
		None,
		Phrase,
		Fixed,
		Custom,
	};
	Type _type = Type::None;
	tr::phrase<> _phrase = {};
	QString _fixed;
	Fn<QString()> _custom;

};

// A value of one parameter.
class FxValue final {
public:
	enum class Type : uchar {
		None,
		Number, // Float, Angle.
		Integer, // Int, Choice (the index), Seed.
		Boolean,
		Color,
		Point,
		Data, // Custom: opaque serialized bytes.
	};

	FxValue() = default;
	[[nodiscard]] static FxValue Number(double value);
	[[nodiscard]] static FxValue Integer(int value);
	[[nodiscard]] static FxValue Boolean(bool value);
	[[nodiscard]] static FxValue Color(QColor value);
	[[nodiscard]] static FxValue Point(QPointF value);
	[[nodiscard]] static FxValue Data(QByteArray value);

	[[nodiscard]] Type type() const;
	[[nodiscard]] bool empty() const;

	// Tolerant getters: a number is read from Number / Integer / Boolean,
	// everything else gives a zero value of the requested type.
	[[nodiscard]] double number() const;
	[[nodiscard]] int integer() const; // Rounded.
	[[nodiscard]] bool boolean() const;
	[[nodiscard]] QColor color() const; // With alpha.
	[[nodiscard]] QPointF point() const;
	[[nodiscard]] QByteArray data() const;

	friend bool operator==(const FxValue &a, const FxValue &b);

private:
	Type _type = Type::None;
	double _a = 0.;
	double _b = 0.;
	QByteArray _data;

};

// Parameter values by parameter id (sorted, cheap to copy and compare).
class FxParams final {
public:
	[[nodiscard]] bool has(QByteArrayView id) const;
	[[nodiscard]] const FxValue &value(QByteArrayView id) const;
	void set(const QByteArray &id, FxValue value);
	void remove(QByteArrayView id);

	[[nodiscard]] double number(QByteArrayView id) const;
	[[nodiscard]] int integer(QByteArrayView id) const;
	[[nodiscard]] bool boolean(QByteArrayView id) const;
	[[nodiscard]] QColor color(QByteArrayView id) const;
	[[nodiscard]] QPointF point(QByteArrayView id) const;
	[[nodiscard]] QByteArray data(QByteArrayView id) const;

	struct Entry {
		QByteArray id;
		FxValue value;

		friend bool operator==(const Entry &a, const Entry &b) = default;
	};
	[[nodiscard]] const std::vector<Entry> &list() const;
	[[nodiscard]] bool empty() const;

	friend bool operator==(const FxParams &a, const FxParams &b) = default;

private:
	std::vector<Entry> _list;

};

enum class FxParamKind : uchar {
	Float, // A slider with a value field, FxValue::Number.
	Int, // The same with whole values, FxValue::Integer.
	Bool, // A switch.
	Choice, // One of FxParam::choices, FxValue::Integer is the index.
	Color, // A color with presets and a picker.
	Angle, // Degrees, a slider, FxValue::Number.
	Point, // Normalized layer point, picked / dragged on the canvas.
	Seed, // A random variant 0..9999 with a "randomize" button.
	Custom, // Opaque data edited by a registered FxCustomEditor.
};

inline constexpr auto kFxSeedMax = 9999;

// How the track of a Float / Int slider is painted: plain, or a gradient
// that shows what its two ends mean (cold - warm, green - magenta), like
// the white balance sliders of the Adjust tab.
enum class FxSliderLook : uchar {
	Plain,
	Temperature,
	Tint,
};

// FxContext::seed the compositor starts from for the effects of a layer
// (ApplyFxStack mixes the uid of every instance into it).
inline constexpr auto kFxLayerSeed = uint32(0x5EED);

struct FxParam {
	QByteArray id; // Stable, unique in the effect, [a-z0-9_].
	FxText name;
	FxParamKind kind = FxParamKind::Float;

	// Float / Int / Angle: the range, the slider step, the default.
	// Choice: value is the default index. Bool: 0 / 1. Seed: the default.
	double min = 0.;
	double max = 100.;
	double step = 1.;
	double value = 0.;
	int decimals = 0; // Shown digits after the separator (Float).
	QString suffix; // Shown after the value: "%", " px".

	// The value is a length in source pixels (FxPixels): apply() scales
	// it with FxContext::px(). Only a hint for the UI and the self-test.
	bool sourcePixels = false;

	FxSliderLook look = FxSliderLook::Plain; // Float / Int: the track.

	QColor color; // Color: the default.
	bool alpha = false; // Color: the picker edits the opacity too.
	QPointF point = QPointF(0.5, 0.5); // Point: the default.
	std::vector<FxText> choices; // Choice.
	QByteArray customType; // Custom: FxCustomEditor::type.
	QByteArray data; // Custom: the default value.

	// A small subtitle shown above this parameter (groups in long lists).
	FxText section;

	// Hides the control while it returns false (a parameter that only
	// matters for some value of a Choice / Bool one). Main thread.
	Fn<bool(const FxParams&)> visible;

	[[nodiscard]] FxParam when(Fn<bool(const FxParams&)> predicate) const;
	[[nodiscard]] FxParam under(FxText title) const;
	[[nodiscard]] FxParam stepped(double step) const;
	[[nodiscard]] FxParam styled(FxSliderLook look) const;

	[[nodiscard]] FxValue defaultValue() const;
	// The value converted to the type of this parameter and clamped,
	// the default for an empty or unusable value.
	[[nodiscard]] FxValue normalized(const FxValue &value) const;
};

// Parameter descriptor builders.
[[nodiscard]] FxParam FxFloat(
	QByteArray id,
	FxText name,
	double min,
	double max,
	double value,
	int decimals = 0,
	QString suffix = QString());
[[nodiscard]] FxParam FxInt(
	QByteArray id,
	FxText name,
	int min,
	int max,
	int value,
	QString suffix = QString());
// A length in source pixels, see FxContext::px(). Shown with " px".
[[nodiscard]] FxParam FxPixels(
	QByteArray id,
	FxText name,
	double min,
	double max,
	double value,
	int decimals = 0);
[[nodiscard]] FxParam FxBool(QByteArray id, FxText name, bool value);
[[nodiscard]] FxParam FxChoice(
	QByteArray id,
	FxText name,
	std::vector<FxText> choices,
	int value = 0);
[[nodiscard]] FxParam FxColor(
	QByteArray id,
	FxText name,
	QColor value,
	bool alpha = false);
[[nodiscard]] FxParam FxAngle(
	QByteArray id,
	FxText name,
	double value = 0.,
	double min = -180.,
	double max = 180.);
[[nodiscard]] FxParam FxPoint(
	QByteArray id,
	FxText name,
	QPointF value = QPointF(0.5, 0.5));
// The name may be empty: the common "Variant" title is used then.
[[nodiscard]] FxParam FxSeed(
	QByteArray id = QByteArray("seed"),
	FxText name = FxText(),
	int value = 0);
[[nodiscard]] FxParam FxCustom(
	QByteArray id,
	FxText name,
	QByteArray customType,
	QByteArray value = QByteArray());

enum class FxGroup : uchar {
	Light, // Exposure, contrast, tone curve...
	Color, // White balance, HSL, color grading, black and white...
	Detail, // Texture, clarity, dehaze, sharpening, noise reduction...
	Finish, // Vignette, grain.
	Blur,
	Distort,
	Lofi, // The "bad camera" pack.
	Glitch,
	Stylize, // Everything artistic that fits nowhere else.
	Classic, // The effects and adjustments of oblivion_photo_core.h.
};
inline constexpr auto kFxGroupCount = 10;

enum FxFlag : uint32 {
	// Moves pixels (a distortion): a mask painted over the layer and the
	// canvas overlays of other tools don't follow the picture.
	kFxGeometry = (1U << 0),
	// A pixel depends on its neighbours (blurs, sharpening, distortions):
	// the effect can't be fused with others into one per-pixel pass and
	// its result depends on the rendering scale more than a color one.
	kFxNeighbours = (1U << 1),
	// Uses FxContext::seed / a Seed parameter.
	kFxSeeded = (1U << 2),
	// Slow even on the preview: the panels commit its sliders on release
	// instead of rendering every intermediate value.
	kFxSlow = (1U << 3),
	// Not offered in the "add effect" menu (internal / deprecated ids
	// that old stacks may still contain).
	kFxHidden = (1U << 4),
};

struct FxContext {
	// Rendered pixels per source pixel of the layer (1 = full size).
	double scale = 1.;
	// Differs between effect instances, the same for the preview and the
	// export of one instance. Mix it with the Seed parameter.
	uint32 seed = 0;
	// Full size of the layer content in source pixels (image.size() is
	// about fullSize * scale).
	QSize fullSize;
	// A downscaled interactive render: an effect may take a cheaper path
	// that looks the same.
	bool preview = false;
	const std::atomic<bool> *cancel = nullptr;

	[[nodiscard]] bool cancelled() const {
		return cancel && cancel->load(std::memory_order_relaxed);
	}
	// A length in source pixels -> rendered pixels.
	[[nodiscard]] double px(double sourcePixels) const {
		return sourcePixels * scale;
	}
};

struct FxDescriptor {
	QByteArray id; // Stable (stored in documents), "group.name".
	FxGroup group = FxGroup::Stylize;
	FxText name;
	std::vector<FxParam> params; // In the UI order.
	uint32 flags = 0; // FxFlag values.
	int order = 0; // In the group menu, then the registration order.

	// See the contract at the top of the file. False: cancelled.
	Fn<bool(
		QImage &image,
		const FxParams &params,
		const FxContext &context)> apply;

	// Optional: true if these (normalized) parameters change nothing,
	// the effect is skipped then (no copy, no cache entry).
	Fn<bool(const FxParams &params)> identity;

	[[nodiscard]] const FxParam *param(QByteArrayView id) const;
};

// One effect of a layer stack.
struct FxInstance {
	QByteArray id;
	bool enabled = true;
	FxParams params;
	// Identity of this instance in its document (panels, caches, undo),
	// assigned by the document operations, 0 before that. Not compared.
	uint64 uid = 0;

	friend bool operator==(const FxInstance &a, const FxInstance &b) {
		return (a.id == b.id)
			&& (a.enabled == b.enabled)
			&& (a.params == b.params);
	}
};

// A one-click preset: a ready stack (one effect or several) that is
// appended to the layer. Presets only set parameters.
struct FxPreset {
	QByteArray id;
	FxGroup group = FxGroup::Lofi;
	FxText name;
	std::vector<FxInstance> stack;
	int order = 0;
};

// The widget that edits a Custom parameter (a curve, an HSL table, color
// wheels, a mesh). The factory runs on the main thread, the widget is
// laid out by resizeToWidth() and reports its height (heightValue()).
struct FxCustomEditorArgs {
	QByteArray value; // The current value.
	// Later values that came from outside (undo, redo, reset, a preset):
	// show them without calling changed().
	rpl::producer<QByteArray> values;
	// Report an edit: finished == false while dragging (a live preview),
	// true when the change is complete (it becomes one undo step).
	Fn<void(QByteArray value, bool finished)> changed;
	// What is edited, for editors that work on the canvas (a mesh warp):
	// see Controller::fxParam() / setFxParam() / setTemporaryTool() in
	// oblivion_photo_editor.h. controller is null in previews without
	// an editor.
	Controller *controller = nullptr;
	uint64 layerId = 0;
	uint64 fxUid = 0;
	QByteArray paramId;
	// True while the effect is switched off (or its layer is locked): the
	// parameter panel fades the whole editor itself, an editor only needs
	// this to stop something of its own (a canvas tool, an animation).
	// May be null.
	rpl::producer<bool> dimmed;
};
struct FxCustomEditor {
	QByteArray type; // FxParam::customType.
	Fn<object_ptr<Ui::RpWidget>(
		not_null<QWidget*> parent,
		FxCustomEditorArgs &&args)> create;
	// Optional: a valid value made from whatever was stored (clamps,
	// repairs, an empty array for garbage). Any thread.
	Fn<QByteArray(const QByteArray &value)> normalize;
};

// Registration, only from the callback of an FxRegistrar.
void RegisterFx(FxDescriptor &&descriptor);
void RegisterFxPreset(FxPreset &&preset);
void RegisterFxCustomEditor(FxCustomEditor &&editor);

class FxRegistrar final {
public:
	explicit FxRegistrar(Fn<void()> registerAll);

	FxRegistrar(const FxRegistrar &other) = delete;
	FxRegistrar &operator=(const FxRegistrar &other) = delete;

};

// The registry. Pointers stay valid forever. Any thread.
[[nodiscard]] const FxDescriptor *FindFx(QByteArrayView id);
[[nodiscard]] const std::vector<const FxDescriptor*> &AllFx();
[[nodiscard]] std::vector<const FxDescriptor*> FxInGroup(FxGroup group);
[[nodiscard]] const std::vector<const FxPreset*> &AllFxPresets();
[[nodiscard]] const FxCustomEditor *FindFxCustomEditor(QByteArrayView type);
[[nodiscard]] const std::vector<FxGroup> &FxGroups(); // Menu order.
[[nodiscard]] QByteArray FxGroupKey(FxGroup group); // "blur".

// Instances.
[[nodiscard]] FxParams DefaultFxParams(const FxDescriptor &descriptor);
// Complete and clamped: unknown ids dropped, missing ones defaulted.
[[nodiscard]] FxParams NormalizedFxParams(
	const FxDescriptor &descriptor,
	const FxParams &params);
// An instance with default parameters, the given ones applied over them.
// An unknown id gives an instance that does nothing (but is kept).
[[nodiscard]] FxInstance MakeFx(
	const QByteArray &id,
	std::initializer_list<FxParams::Entry> values = {});
[[nodiscard]] bool FxIsIdentity(const FxInstance &instance);
// Stable hash of the id, enabled and the normalized parameters.
[[nodiscard]] uint64 FxHash(const FxInstance &instance);
[[nodiscard]] uint64 FxHashCombine(uint64 seed, uint64 value);

// Applies one instance / the enabled instances in order. Disabled,
// unknown and identity ones are skipped. False: cancelled (or the image
// was null), the image content is undefined then. Worker threads.
[[nodiscard]] bool ApplyFx(
	QImage &image,
	const FxInstance &instance,
	const FxContext &context);
[[nodiscard]] bool ApplyFxStack(
	QImage &image,
	const std::vector<FxInstance> &stack,
	const FxContext &context);

// Serialization: {"id":"blur.gaussian","params":{"radius":12}}, plus
// "off":true for a disabled one. Only values that differ from the
// defaults are written. Colors are "#AARRGGBB", points [x, y], custom
// data is base64. Reading is tolerant (unknown parameters are dropped,
// values clamped), instances of unknown effects are kept as they are.
[[nodiscard]] QJsonObject FxToJson(const FxInstance &instance);
[[nodiscard]] std::optional<FxInstance> FxFromJson(const QJsonObject &object);
[[nodiscard]] QByteArray SerializeFxStack(
	const std::vector<FxInstance> &stack);
[[nodiscard]] std::vector<FxInstance> DeserializeFxStack(
	const QByteArray &json);

//
// Helpers for effect code. Any thread.
//

namespace FxDetail {

void RunParallel(
	int count,
	int grain,
	void (*call)(void *context, int from, int till),
	void *context);

} // namespace FxDetail

// Calls body(int from, int till) for chunks of [0, count) on several
// threads and returns when all are done. The calling thread works too,
// so it is safe inside crl::async. grain is the smallest chunk worth
// a thread. The result must not depend on the chunks.
template <typename Body>
void FxParallel(int count, int grain, const Body &body) {
	FxDetail::RunParallel(count, grain, [](void *context, int from, int till) {
		(*static_cast<const Body*>(context))(from, till);
	}, const_cast<void*>(static_cast<const void*>(&body)));
}

// The same over the rows of an image, in bands of about 32K pixels.
template <typename Body>
void FxParallelRows(int width, int height, const Body &body) {
	FxParallel(height, std::max(1, 32768 / std::max(width, 1)), body);
}

// Converts to Format_ARGB32_Premultiplied with device pixel ratio 1 and
// detaches (so bits() / scanLine() don't copy later). False for a null
// image or if the memory can't be allocated.
[[nodiscard]] bool FxPrepare(QImage &image);

[[nodiscard]] inline uint32 *FxRow(QImage &image, int y) {
	return reinterpret_cast<uint32*>(image.scanLine(y));
}
[[nodiscard]] inline const uint32 *FxRow(const QImage &image, int y) {
	return reinterpret_cast<const uint32*>(image.constScanLine(y));
}

// A straight (not premultiplied) color, every channel 0..1.
struct FxRgba {
	float r = 0.f;
	float g = 0.f;
	float b = 0.f;
	float a = 0.f;
};

[[nodiscard]] inline float FxClamp01(float value) {
	return (value > 0.f) ? ((value < 1.f) ? value : 1.f) : 0.f;
}

[[nodiscard]] inline FxRgba FxUnpack(uint32 premultiplied) {
	const auto a = int(premultiplied >> 24);
	if (!a) {
		return {};
	}
	const auto k = 1.f / a;
	return {
		std::min(int((premultiplied >> 16) & 0xFFU) * k, 1.f),
		std::min(int((premultiplied >> 8) & 0xFFU) * k, 1.f),
		std::min(int(premultiplied & 0xFFU) * k, 1.f),
		a * (1.f / 255.f),
	};
}

[[nodiscard]] inline uint32 FxPack(FxRgba straight) {
	const auto a = int(FxClamp01(straight.a) * 255.f + 0.5f);
	const auto k = float(a);
	return (uint32(a) << 24)
		| (uint32(int(FxClamp01(straight.r) * k + 0.5f)) << 16)
		| (uint32(int(FxClamp01(straight.g) * k + 0.5f)) << 8)
		| uint32(int(FxClamp01(straight.b) * k + 0.5f));
}

[[nodiscard]] inline float FxLuma(float r, float g, float b) {
	return 0.299f * r + 0.587f * g + 0.114f * b;
}

[[nodiscard]] inline float FxMix(float a, float b, float t) {
	return a + (b - a) * t;
}

[[nodiscard]] inline float FxSmoothStep(float e0, float e1, float x) {
	const auto t = FxClamp01((x - e0) / (e1 - e0));
	return t * t * (3.f - 2.f * t);
}

// Calls op(FxRgba &color, int x, int y) for every pixel that is not
// fully transparent, in parallel, and writes the color back. The alpha
// may be changed too.
template <typename Op>
void FxForEachColor(QImage &image, const Op &op) {
	const auto width = image.width();
	FxParallelRows(width, image.height(), [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			for (auto x = 0; x != width; ++x) {
				if (!(line[x] >> 24)) {
					continue;
				}
				auto color = FxUnpack(line[x]);
				op(color, x, y);
				line[x] = FxPack(color);
			}
		}
	});
}

// Bilinear sample of premultiplied pixels at continuous pixel-index
// coordinates (pixel centers at whole numbers). FxSample clamps to the
// edges, FxSampleClear gives transparent outside of the image (with
// a one pixel soft border).
[[nodiscard]] uint32 FxSample(const QImage &image, float x, float y);
[[nodiscard]] uint32 FxSampleClear(const QImage &image, float x, float y);

// Deterministic noise: a 32 bit hash of two coordinates and a seed, and
// the same as a number in [-1, 1] (triangular distribution).
[[nodiscard]] uint32 FxHash32(uint32 x, uint32 y, uint32 seed);
[[nodiscard]] float FxNoise(uint32 x, uint32 y, uint32 seed);

// A small deterministic generator for sequences (slices, blocks...).
struct FxRandom {
	uint64 state = 0;

	explicit FxRandom(uint64 seed) : state(seed) {
	}
	[[nodiscard]] uint64 next();
	[[nodiscard]] float unit(); // [0, 1)
	[[nodiscard]] int range(int from, int till); // [from, till]
};

// Gaussian blur with the standard deviation sigma in rendered pixels
// (edges repeat). The image version blurs premultiplied pixels (no dark
// fringes around transparency), the plane one a width x height array.
void FxGaussianBlur(QImage &image, double sigma);
void FxGaussianBlur(
	std::vector<float> &plane,
	int width,
	int height,
	double sigma);

// Smooth resize, Format_ARGB32_Premultiplied.
[[nodiscard]] QImage FxResized(const QImage &image, QSize size);

[[nodiscard]] double FxSrgbToLinear(double value);
[[nodiscard]] double FxLinearToSrgb(double value);

//
// Self-tests.
//

// The suites OBLIVION_SELFTEST=photo_doc and photo_fx run the checks of
// this module and then every sub-test registered for the suite. A module
// registers its own pure checks (no Core::App(), no session, no lang
// strings, deterministic) with a static registrar in its .cpp:
//
//   bool RunBlurSelfTest(QStringList &log) { ... }
//   const auto SelfTest = Oblivion::Photo::SelfTestRegistrar(
//       Oblivion::Photo::SelfTestSuite::Fx,
//       "blur",
//       &RunBlurSelfTest);
//
// Sub-tests append lines like "OK: what" / "FAIL: what" to log and
// return false on a failure. Suite::Fx is for the effect files,
// Suite::Doc for everything else (drawing, collage, transform, layers).
enum class SelfTestSuite : uchar {
	Doc,
	Fx,
};

class SelfTestRegistrar final {
public:
	SelfTestRegistrar(
		SelfTestSuite suite,
		const char *name,
		bool (*run)(QStringList &log));

	SelfTestRegistrar(const SelfTestRegistrar &other) = delete;
	SelfTestRegistrar &operator=(const SelfTestRegistrar &other) = delete;

};

// For RunDocSelfTest / RunFxSelfTest.
[[nodiscard]] bool RunRegisteredSelfTests(
	SelfTestSuite suite,
	QStringList &log);

// A deterministic test picture: gradients, shapes and fine detail,
// optionally with a transparent area. For the sub-tests.
[[nodiscard]] QImage FxTestImage(int width, int height, bool alpha = false);
// Mean absolute difference of the channels, 0..255 (the images are
// compared at the size of the first one). For "preview matches export"
// checks: render at scale 1 and at a smaller scale, compare.
[[nodiscard]] double FxImageDifference(const QImage &a, const QImage &b);

// Self-checks for OBLIVION_SELFTEST=photo_fx, see oblivion_selftest.h:
// the registry itself, every registered effect (defaults, determinism,
// size, the JSON round trip, the preview against the export) and the
// sub-tests of the oblivion_photo_fx*.cpp files.
// No Core::App(), no session: pure logic only.
[[nodiscard]] bool RunFxSelfTest(QStringList &log);

} // namespace Oblivion::Photo
