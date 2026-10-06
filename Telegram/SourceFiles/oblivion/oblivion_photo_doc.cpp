/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_doc.h"

#include <QtCore/QElapsedTimer>
#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace Oblivion::Photo {
namespace {

constexpr auto kDefaultCacheLimit = 640LL * 1024 * 1024;
constexpr auto kMaxChains = 256;
constexpr auto kScaleKeyUnits = 65536.;
constexpr auto kMaxContentScale = 4.;
constexpr auto kMaxLayerPixels = 64. * 1000 * 1000;
constexpr auto kMinContentScale = 1. / 4096;
constexpr auto kPi = 3.14159265358979323846;

[[nodiscard]] uint64 NextSerial() {
	static auto counter = std::atomic<uint64>(0);
	return ++counter;
}

[[nodiscard]] bool Cancelled(const std::atomic<bool> *cancel) {
	return cancel && cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] qint64 ImageBytes(const QImage &image) {
	return image.isNull() ? 0 : qint64(image.sizeInBytes());
}

// Every channel of a premultiplied pixel times alpha / 255, rounded.
[[nodiscard]] inline uint32 MulPixel(uint32 pixel, uint32 alpha) {
	auto rb = (pixel & 0x00FF00FFU) * alpha + 0x00800080U;
	rb = ((rb + ((rb >> 8) & 0x00FF00FFU)) >> 8) & 0x00FF00FFU;
	auto ag = ((pixel >> 8) & 0x00FF00FFU) * alpha + 0x00800080U;
	ag = (ag + ((ag >> 8) & 0x00FF00FFU)) & 0xFF00FF00U;
	return rb | ag;
}

[[nodiscard]] inline uint32 BlendNormal(uint32 d, uint32 s, uint32 opacity) {
	if (opacity != 255) {
		s = MulPixel(s, opacity);
	}
	const auto a = (s >> 24);
	if (!a) {
		return d;
	} else if (a == 255) {
		return s;
	}
	return s + MulPixel(d, 255 - a);
}

struct Rgb {
	float r = 0.f;
	float g = 0.f;
	float b = 0.f;
};

[[nodiscard]] inline float Lum(Rgb c) {
	return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b;
}

[[nodiscard]] inline Rgb ClipColor(Rgb c) {
	const auto l = Lum(c);
	const auto n = std::min({ c.r, c.g, c.b });
	const auto x = std::max({ c.r, c.g, c.b });
	if (n < 0.f) {
		const auto k = l / (l - n);
		c = { l + (c.r - l) * k, l + (c.g - l) * k, l + (c.b - l) * k };
	}
	if (x > 1.f) {
		const auto k = (1.f - l) / (x - l);
		c = { l + (c.r - l) * k, l + (c.g - l) * k, l + (c.b - l) * k };
	}
	return c;
}

[[nodiscard]] inline Rgb SetLum(Rgb c, float l) {
	const auto d = l - Lum(c);
	return ClipColor({ c.r + d, c.g + d, c.b + d });
}

[[nodiscard]] inline float Sat(Rgb c) {
	return std::max({ c.r, c.g, c.b }) - std::min({ c.r, c.g, c.b });
}

[[nodiscard]] inline Rgb SetSat(Rgb c, float s) {
	float *values[3] = { &c.r, &c.g, &c.b };
	if (*values[0] > *values[1]) {
		std::swap(values[0], values[1]);
	}
	if (*values[1] > *values[2]) {
		std::swap(values[1], values[2]);
	}
	if (*values[0] > *values[1]) {
		std::swap(values[0], values[1]);
	}
	const auto lowest = *values[0];
	const auto highest = *values[2];
	if (highest > lowest) {
		*values[1] = (*values[1] - lowest) * s / (highest - lowest);
		*values[2] = s;
	} else {
		*values[1] = 0.f;
		*values[2] = 0.f;
	}
	*values[0] = 0.f;
	return c;
}

[[nodiscard]] inline float BlendChannel(BlendMode mode, float cb, float cs) {
	switch (mode) {
	case BlendMode::Multiply: return cb * cs;
	case BlendMode::Screen: return cb + cs - cb * cs;
	case BlendMode::Overlay:
		return (cb <= 0.5f)
			? (2.f * cb * cs)
			: (1.f - 2.f * (1.f - cb) * (1.f - cs));
	case BlendMode::SoftLight: {
		if (cs <= 0.5f) {
			return cb - (1.f - 2.f * cs) * cb * (1.f - cb);
		}
		const auto d = (cb <= 0.25f)
			? (((16.f * cb - 12.f) * cb + 4.f) * cb)
			: std::sqrt(cb);
		return cb + (2.f * cs - 1.f) * (d - cb);
	}
	case BlendMode::HardLight:
		return (cs <= 0.5f)
			? (2.f * cb * cs)
			: (1.f - 2.f * (1.f - cb) * (1.f - cs));
	case BlendMode::Darken: return std::min(cb, cs);
	case BlendMode::Lighten: return std::max(cb, cs);
	case BlendMode::ColorDodge:
		return (cb <= 0.f)
			? 0.f
			: (cs >= 1.f)
			? 1.f
			: std::min(1.f, cb / (1.f - cs));
	case BlendMode::ColorBurn:
		return (cb >= 1.f)
			? 1.f
			: (cs <= 0.f)
			? 0.f
			: (1.f - std::min(1.f, (1.f - cb) / cs));
	case BlendMode::Difference: return std::abs(cb - cs);
	case BlendMode::Exclusion: return cb + cs - 2.f * cb * cs;
	case BlendMode::Add: return std::min(1.f, cb + cs);
	default: return cs;
	}
}

[[nodiscard]] inline Rgb BlendColors(BlendMode mode, Rgb cb, Rgb cs) {
	switch (mode) {
	case BlendMode::Normal: return cs;
	case BlendMode::Hue: return SetLum(SetSat(cs, Sat(cb)), Lum(cb));
	case BlendMode::Saturation: return SetLum(SetSat(cb, Sat(cs)), Lum(cb));
	case BlendMode::Color: return SetLum(cs, Lum(cb));
	case BlendMode::Luminosity: return SetLum(cb, Lum(cs));
	default:
		return {
			BlendChannel(mode, cb.r, cs.r),
			BlendChannel(mode, cb.g, cs.g),
			BlendChannel(mode, cb.b, cs.b),
		};
	}
}

[[nodiscard]] inline uint32 PackPremultiplied(float alpha, Rgb premultiplied) {
	const auto a = std::clamp(int(alpha * 255.f + 0.5f), 0, 255);
	const auto channel = [&](float value) {
		return uint32(std::clamp(int(value * 255.f + 0.5f), 0, a));
	};
	return (uint32(a) << 24)
		| (channel(premultiplied.r) << 16)
		| (channel(premultiplied.g) << 8)
		| channel(premultiplied.b);
}

// opacity is 0..255, the same quantization as the integer path.
[[nodiscard]] inline uint32 BlendFloat(
		uint32 d,
		uint32 s,
		BlendMode mode,
		uint32 opacity) {
	constexpr auto k255 = 1.f / 255.f;
	const auto k = opacity * k255 * k255;
	const auto sa = (s >> 24) * k;
	if (sa <= 0.f) {
		return d;
	}
	const auto sp = Rgb{
		((s >> 16) & 0xFFU) * k,
		((s >> 8) & 0xFFU) * k,
		(s & 0xFFU) * k,
	};
	const auto da = (d >> 24) * k255;
	if (da <= 0.f) {
		return PackPremultiplied(sa, sp);
	}
	const auto dp = Rgb{
		((d >> 16) & 0xFFU) * k255,
		((d >> 8) & 0xFFU) * k255,
		(d & 0xFFU) * k255,
	};
	const auto ks = 1.f / sa;
	const auto kd = 1.f / da;
	const auto cs = Rgb{
		std::min(sp.r * ks, 1.f),
		std::min(sp.g * ks, 1.f),
		std::min(sp.b * ks, 1.f),
	};
	const auto cb = Rgb{
		std::min(dp.r * kd, 1.f),
		std::min(dp.g * kd, 1.f),
		std::min(dp.b * kd, 1.f),
	};
	const auto blended = BlendColors(mode, cb, cs);
	const auto both = sa * da;
	const auto ns = 1.f - sa;
	const auto nd = 1.f - da;
	return PackPremultiplied(sa + da - both, {
		sp.r * nd + dp.r * ns + both * blended.r,
		sp.g * nd + dp.g * ns + both * blended.g,
		sp.b * nd + dp.b * ns + both * blended.b,
	});
}

[[nodiscard]] uint32 OpacityByte(double opacity) {
	return uint32(std::clamp(int(std::lround(opacity * 255.)), 0, 255));
}

[[nodiscard]] uint64 Mix(uint64 seed, uint64 value) {
	return FxHashCombine(seed, value);
}

[[nodiscard]] uint64 MixDouble(uint64 seed, double value) {
	auto bits = uint64(0);
	if (value == 0.) {
		value = 0.;
	}
	std::memcpy(&bits, &value, sizeof(bits));
	return FxHashCombine(seed, bits);
}

[[nodiscard]] uint64 MixTransform(uint64 seed, const QTransform &t) {
	for (const auto value : {
		t.m11(), t.m12(), t.m13(),
		t.m21(), t.m22(), t.m23(),
		t.m31(), t.m32(), t.m33(),
	}) {
		seed = MixDouble(seed, value);
	}
	return seed;
}

[[nodiscard]] QPolygonF RectPolygon(QSizeF size) {
	auto result = QPolygonF();
	result.reserve(4);
	result.push_back(QPointF(0., 0.));
	result.push_back(QPointF(size.width(), 0.));
	result.push_back(QPointF(size.width(), size.height()));
	result.push_back(QPointF(0., size.height()));
	return result;
}

[[nodiscard]] bool PositiveDepth(const QTransform &t, QSizeF size) {
	if (t.isAffine()) {
		return true;
	}
	for (const auto &point : RectPolygon(size)) {
		const auto w = t.m13() * point.x() + t.m23() * point.y() + t.m33();
		if (!(w > 1e-6)) {
			return false;
		}
	}
	return true;
}

// How many canvas pixels one layer pixel covers along its longer axis.
[[nodiscard]] double Magnification(const QTransform &t, QSizeF size) {
	if (t.isAffine()) {
		return std::max(
			std::hypot(t.m11(), t.m12()),
			std::hypot(t.m21(), t.m22()));
	} else if (size.isEmpty() || !PositiveDepth(t, size)) {
		return 1.;
	}
	const auto quad = t.map(RectPolygon(size));
	const auto length = [&](int a, int b) {
		const auto delta = quad[a] - quad[b];
		return std::hypot(delta.x(), delta.y());
	};
	return std::max({
		length(1, 0) / size.width(),
		length(2, 3) / size.width(),
		length(3, 0) / size.height(),
		length(2, 1) / size.height(),
	});
}

// The scale the content of a layer is rendered at for a picture scale.
// An untransformed layer gets exactly the picture scale, a transformed
// one the next step of sqrt(2), so a drag of a scale handle doesn't
// recompute the effects of the layer on every mouse move.
[[nodiscard]] double ContentScale(
		const QTransform &transform,
		QSize size,
		double scale) {
	const auto magnification = Magnification(transform, QSizeF(size));
	auto result = scale;
	if (std::abs(magnification - 1.) > 1e-6) {
		const auto target = std::max(scale * magnification, kMinContentScale);
		result = std::pow(2., std::ceil(std::log2(target) * 2. - 1e-9) / 2.);
	}
	if (result > 1.) {
		// Vector content enlarged by its transform, within reason.
		const auto area = std::max(double(size.width()) * size.height(), 1.);
		result = std::min({
			result,
			kMaxContentScale,
			std::max(std::sqrt(kMaxLayerPixels / area), 1.),
		});
	}
	return std::max(result, kMinContentScale);
}

[[nodiscard]] bool ApplyMask(QImage &image, const LayerMask &mask) {
	if (!FxPrepare(image)) {
		return false;
	}
	auto scaled = mask.image();
	if (scaled.size() != image.size()) {
		scaled = scaled.scaled(
			image.size(),
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}
	if (scaled.format() != QImage::Format_Grayscale8) {
		scaled = scaled.convertToFormat(QImage::Format_Grayscale8);
	}
	if (scaled.isNull()) {
		return false;
	}
	const auto width = image.width();
	const auto data = image.bits();
	const auto stride = image.bytesPerLine();
	FxParallelRows(width, image.height(), [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = reinterpret_cast<uint32*>(
				data + qsizetype(y) * stride);
			const auto values = scaled.constScanLine(y);
			for (auto x = 0; x != width; ++x) {
				const auto value = values[x];
				if (value != 255) {
					line[x] = value ? MulPixel(line[x], value) : 0U;
				}
			}
		}
	});
	return true;
}

struct Placed {
	QImage image;
	QPoint position;
};

// The pixels of a layer in the coordinates of the rendered picture.
[[nodiscard]] Placed Place(
		const QImage &pixels,
		QSize contentSize,
		const QTransform &transform,
		double scale,
		QSize output) {
	if (pixels.isNull() || contentSize.isEmpty()) {
		return {};
	}
	const auto full = QTransform::fromScale(
		contentSize.width() / double(pixels.width()),
		contentSize.height() / double(pixels.height())
	) * transform * QTransform::fromScale(scale, scale);
	const auto plain = full.isAffine()
		&& (std::abs(full.m12()) < 1e-9)
		&& (std::abs(full.m21()) < 1e-9)
		&& (full.m11() > 0.)
		&& (full.m22() > 0.)
		&& (std::abs(pixels.width() * (full.m11() - 1.)) < 1.)
		&& (std::abs(pixels.height() * (full.m22() - 1.)) < 1.);
	if (plain) {
		return {
			pixels,
			QPoint(
				int(std::lround(full.dx())),
				int(std::lround(full.dy()))),
		};
	} else if (!PositiveDepth(full, QSizeF(pixels.size()))) {
		return {};
	}
	const auto mapped = full.map(
		RectPolygon(QSizeF(pixels.size()))).boundingRect();
	const auto bounds = mapped.toAlignedRect().intersected(
		QRect(QPoint(), output));
	if (bounds.isEmpty()) {
		return {};
	}
	auto image = QImage(bounds.size(), QImage::Format_ARGB32_Premultiplied);
	if (image.isNull()) {
		return {};
	}
	image.fill(Qt::transparent);
	{
		auto p = QPainter(&image);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.setRenderHint(QPainter::Antialiasing);
		p.setTransform(
			full * QTransform::fromTranslate(-bounds.x(), -bounds.y()));
		p.drawImage(QPointF(), pixels);
	}
	return { std::move(image), bounds.topLeft() };
}

struct LayerPlan {
	const Layer *layer = nullptr;
	QSize contentSize;
	double scale = 1.; // Of the content.
	bool preview = false;
	uint64 slot = 0;
	std::vector<const FxInstance*> effects; // Not identity ones.
	bool mask = false;
	std::vector<uint64> chain; // effects.size() + mask + 1 keys.
};

[[nodiscard]] LayerPlan PlanLayer(
		const Layer &layer,
		double contentScale,
		bool preview) {
	auto result = LayerPlan{
		.layer = &layer,
		.contentSize = layer.size(),
		.scale = contentScale,
		.preview = preview,
	};
	const auto scaleKey = uint64(std::llround(contentScale * kScaleKeyUnits));
	result.slot = Mix(Mix(Mix(0x510755ULL, layer.id), scaleKey), preview);
	auto key = Mix(
		Mix(Mix(0xC0D7E27ULL, layer.content->serial()), scaleKey),
		preview);
	result.chain.push_back(key);
	for (const auto &instance : layer.effects) {
		if (FxIsIdentity(instance)) {
			continue;
		}
		result.effects.push_back(&instance);
		key = Mix(Mix(key, FxHash(instance)), instance.uid);
		result.chain.push_back(key);
	}
	if (layer.mask && layer.maskEnabled && !layer.mask->image().isNull()) {
		result.mask = true;
		key = Mix(Mix(key, 0x3A5CULL), layer.mask->serial());
		result.chain.push_back(key);
	}
	return result;
}

[[nodiscard]] bool Renderable(const Layer &layer) {
	return layer.visible
		&& (layer.opacity > 0.)
		&& layer.content
		&& !layer.content->size().isEmpty();
}

// The index before the first difference of two key chains: what was
// computed there is what the next render can start from if the same
// thing keeps changing. -1 if nothing is known.
[[nodiscard]] int KeepIndex(
		const std::vector<uint64> &before,
		const std::vector<uint64> &now) {
	if (before.empty()) {
		return -1;
	}
	auto index = 0;
	const auto count = int(std::min(before.size(), now.size()));
	while (index != count && before[index] == now[index]) {
		++index;
	}
	return index - 1;
}

[[nodiscard]] bool SamePixels(const QImage &a, const QImage &b) {
	if (a.size() != b.size() || a.format() != b.format()) {
		return false;
	}
	for (auto y = 0; y != a.height(); ++y) {
		if (std::memcmp(
				a.constScanLine(y),
				b.constScanLine(y),
				size_t(a.width()) * 4) != 0) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] int MaxDifference(const QImage &a, const QImage &b) {
	if (a.size() != b.size() || a.isNull()) {
		return 255;
	}
	auto result = 0;
	for (auto y = 0; y != a.height(); ++y) {
		const auto one = FxRow(a, y);
		const auto two = FxRow(b, y);
		for (auto x = 0; x != a.width(); ++x) {
			for (auto shift = 0; shift != 32; shift += 8) {
				result = std::max(result, std::abs(
					int((one[x] >> shift) & 0xFFU)
						- int((two[x] >> shift) & 0xFFU)));
			}
		}
	}
	return result;
}

} // namespace

const std::vector<BlendMode> &BlendModes() {
	static const auto result = std::vector<BlendMode>{
		BlendMode::Normal,
		BlendMode::Multiply,
		BlendMode::Screen,
		BlendMode::Overlay,
		BlendMode::SoftLight,
		BlendMode::HardLight,
		BlendMode::Darken,
		BlendMode::Lighten,
		BlendMode::ColorDodge,
		BlendMode::ColorBurn,
		BlendMode::Difference,
		BlendMode::Exclusion,
		BlendMode::Add,
		BlendMode::Hue,
		BlendMode::Saturation,
		BlendMode::Color,
		BlendMode::Luminosity,
	};
	return result;
}

QByteArray BlendModeKey(BlendMode mode) {
	switch (mode) {
	case BlendMode::Normal: return "normal";
	case BlendMode::Multiply: return "multiply";
	case BlendMode::Screen: return "screen";
	case BlendMode::Overlay: return "overlay";
	case BlendMode::SoftLight: return "soft_light";
	case BlendMode::HardLight: return "hard_light";
	case BlendMode::Darken: return "darken";
	case BlendMode::Lighten: return "lighten";
	case BlendMode::ColorDodge: return "color_dodge";
	case BlendMode::ColorBurn: return "color_burn";
	case BlendMode::Difference: return "difference";
	case BlendMode::Exclusion: return "exclusion";
	case BlendMode::Add: return "add";
	case BlendMode::Hue: return "hue";
	case BlendMode::Saturation: return "saturation";
	case BlendMode::Color: return "color";
	case BlendMode::Luminosity: return "luminosity";
	}
	return "normal";
}

std::optional<BlendMode> BlendModeFromKey(QByteArrayView key) {
	for (const auto mode : BlendModes()) {
		if (QByteArrayView(BlendModeKey(mode)) == key) {
			return mode;
		}
	}
	return std::nullopt;
}

QRgb BlendPixel(QRgb backdrop, QRgb source, BlendMode mode, double opacity) {
	return BlendFloat(backdrop, source, mode, OpacityByte(opacity));
}

void BlendImage(
		QImage &backdrop,
		const QImage &source,
		QPoint position,
		BlendMode mode,
		double opacity) {
	const auto alpha = OpacityByte(opacity);
	if (source.isNull() || !alpha || !FxPrepare(backdrop)) {
		return;
	}
	const auto converted = (source.format()
		== QImage::Format_ARGB32_Premultiplied)
		? source
		: source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const auto target = QRect(position, converted.size()).intersected(
		backdrop.rect());
	if (converted.isNull() || target.isEmpty()) {
		return;
	}
	const auto to = backdrop.bits();
	const auto toStride = backdrop.bytesPerLine();
	const auto from = converted.constBits();
	const auto fromStride = converted.bytesPerLine();
	const auto left = target.x() - position.x();
	const auto top = target.y() - position.y();
	const auto width = target.width();
	FxParallelRows(width, target.height(), [&](int begin, int end) {
		for (auto y = begin; y != end; ++y) {
			const auto d = reinterpret_cast<uint32*>(
				to + qsizetype(target.y() + y) * toStride) + target.x();
			const auto s = reinterpret_cast<const uint32*>(
				from + qsizetype(top + y) * fromStride) + left;
			if (mode == BlendMode::Normal) {
				for (auto x = 0; x != width; ++x) {
					d[x] = BlendNormal(d[x], s[x], alpha);
				}
			} else {
				for (auto x = 0; x != width; ++x) {
					if (s[x] >> 24) {
						d[x] = BlendFloat(d[x], s[x], mode, alpha);
					}
				}
			}
		}
	});
}

LayerContent::LayerContent() : _serial(NextSerial()) {
}

LayerContent::~LayerContent() = default;

qint64 LayerContent::memoryUsage() const {
	return 0;
}

void LayerContent::memoryParts(
		const Fn<void(const void *object, qint64 bytes)> &visit) const {
	visit(this, memoryUsage());
}

ImageContent::ImageContent(
	QImage image,
	std::shared_ptr<const ImageContent> original)
: _image(PrepareSource(image))
, _original(std::move(original)) {
}

QByteArray ImageContent::type() const {
	return "image";
}

QSize ImageContent::size() const {
	return _image.size();
}

QImage ImageContent::render(const ContentRequest &request) const {
	if (_image.isNull() || request.scale >= 1.) {
		return _image;
	}
	const auto size = ScaledSize(_image.size(), request.scale);
	if (size == _image.size()) {
		return _image;
	}
	auto result = _image.scaled(
		size,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	if (result.format() != QImage::Format_ARGB32_Premultiplied) {
		result = result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	result.setDevicePixelRatio(1.);
	return result;
}

qint64 ImageContent::memoryUsage() const {
	return ImageBytes(_image) + (_original ? _original->memoryUsage() : 0);
}

void ImageContent::memoryParts(
		const Fn<void(const void *object, qint64 bytes)> &visit) const {
	visit(this, ImageBytes(_image));
	if (_original) {
		_original->memoryParts(visit);
	}
}

const QImage &ImageContent::image() const {
	return _image;
}

const std::shared_ptr<const ImageContent> &ImageContent::original() const {
	return _original;
}

std::shared_ptr<const ImageContent> MakeImageContent(
		QImage image,
		std::shared_ptr<const ImageContent> original) {
	return std::make_shared<const ImageContent>(
		std::move(image),
		std::move(original));
}

const ImageContent *AsImage(const ContentPtr &content) {
	return (content && content->type() == "image")
		? static_cast<const ImageContent*>(content.get())
		: nullptr;
}

LayerMask::LayerMask(QImage image)
: _serial(NextSerial())
, _image((image.isNull() || image.format() == QImage::Format_Grayscale8)
	? std::move(image)
	: image.convertToFormat(QImage::Format_Grayscale8)) {
	_image.setDevicePixelRatio(1.);
}

qint64 LayerMask::memoryUsage() const {
	return ImageBytes(_image);
}

QSize MaskSizeFor(QSize content) {
	if (content.isEmpty()) {
		return QSize();
	}
	return ScaledSize(content, ScaleForSide(content, kMaskMaxSide));
}

MaskPtr MakeMask(QImage grayscale) {
	return grayscale.isNull()
		? nullptr
		: std::make_shared<const LayerMask>(std::move(grayscale));
}

MaskPtr MakeMask(QSize size, int fill) {
	if (size.isEmpty()) {
		return nullptr;
	}
	auto image = QImage(size, QImage::Format_Grayscale8);
	if (image.isNull()) {
		return nullptr;
	}
	image.fill(uint(std::clamp(fill, 0, 255)));
	return MakeMask(std::move(image));
}

QSize Layer::size() const {
	return content ? content->size() : QSize();
}

bool operator==(const Layer &a, const Layer &b) {
	return (a.id == b.id)
		&& (a.name == b.name)
		&& (a.visible == b.visible)
		&& (a.locked == b.locked)
		&& (a.opacity == b.opacity)
		&& (a.blend == b.blend)
		&& (a.transform == b.transform)
		&& (a.content == b.content)
		&& (a.mask == b.mask)
		&& (a.maskEnabled == b.maskEnabled)
		&& (a.effects == b.effects);
}

int Document::indexOf(LayerId id) const {
	for (auto i = 0; i != int(layers.size()); ++i) {
		if (layers[i].id == id) {
			return i;
		}
	}
	return -1;
}

const Layer *Document::find(LayerId id) const {
	const auto index = indexOf(id);
	return (index >= 0) ? &layers[index] : nullptr;
}

Layer *Document::find(LayerId id) {
	const auto index = indexOf(id);
	return (index >= 0) ? &layers[index] : nullptr;
}

const Layer *Document::top() const {
	return layers.empty() ? nullptr : &layers.back();
}

bool operator==(const Document &a, const Document &b) {
	return (a.size == b.size)
		&& (a.layers == b.layers)
		&& (a.global == b.global);
}

QSize ScaledSize(QSize size, double scale) {
	return QSize(
		std::max(int(std::lround(size.width() * scale)), 1),
		std::max(int(std::lround(size.height() * scale)), 1));
}

double ScaleForSide(QSize canvas, int side) {
	const auto longer = std::max(canvas.width(), canvas.height());
	return (longer <= 0 || side <= 0)
		? 1.
		: std::min(1., side / double(longer));
}

QTransform PlaceTransform(QSize content, QSize canvas, Placement placement) {
	if (content.isEmpty() || canvas.isEmpty()) {
		return QTransform();
	}
	auto sx = 1.;
	auto sy = 1.;
	const auto kx = canvas.width() / double(content.width());
	const auto ky = canvas.height() / double(content.height());
	switch (placement) {
	case Placement::Center: break;
	case Placement::Fit: sx = sy = std::min({ kx, ky, 1. }); break;
	case Placement::Fill: sx = sy = std::max(kx, ky); break;
	case Placement::Stretch: sx = kx; sy = ky; break;
	}
	return QTransform::fromScale(sx, sy) * QTransform::fromTranslate(
		(canvas.width() - content.width() * sx) / 2.,
		(canvas.height() - content.height() * sy) / 2.);
}

QPolygonF LayerQuad(const Layer &layer) {
	return layer.transform.map(RectPolygon(QSizeF(layer.size())));
}

QRectF LayerBounds(const Layer &layer) {
	return LayerQuad(layer).boundingRect();
}

bool QuadTransform(
		QSizeF content,
		const QPolygonF &quad,
		QTransform &result) {
	if (content.isEmpty() || quad.size() != 4) {
		return false;
	}
	auto sign = 0;
	for (auto i = 0; i != 4; ++i) {
		const auto a = quad[(i + 1) % 4] - quad[i];
		const auto b = quad[(i + 2) % 4] - quad[(i + 1) % 4];
		const auto cross = a.x() * b.y() - a.y() * b.x();
		if (std::abs(cross) < 1e-9) {
			return false;
		}
		const auto now = (cross > 0.) ? 1 : -1;
		if (sign && sign != now) {
			return false;
		}
		sign = now;
	}
	auto transform = QTransform();
	if (!QTransform::quadToQuad(RectPolygon(content), quad, transform)) {
		return false;
	}
	result = transform;
	return true;
}

bool IsPerspective(const QTransform &transform) {
	return !transform.isAffine();
}

std::optional<QPointF> LayerPoint(const Layer &layer, QPointF canvasPoint) {
	auto invertible = false;
	const auto inverted = layer.transform.inverted(&invertible);
	if (!invertible) {
		return std::nullopt;
	}
	const auto result = inverted.map(canvasPoint);
	if (!std::isfinite(result.x()) || !std::isfinite(result.y())) {
		return std::nullopt;
	}
	if (!layer.transform.isAffine()) {
		const auto &t = layer.transform;
		const auto w = t.m13() * result.x() + t.m23() * result.y() + t.m33();
		if (!(w > 1e-9)) {
			return std::nullopt;
		}
	}
	return result;
}

LayerId LayerAt(
		const Document &document,
		QPointF canvasPoint,
		bool withLocked) {
	for (auto i = int(document.layers.size()); i != 0;) {
		const auto &layer = document.layers[--i];
		if (!layer.visible || !layer.content || (layer.locked && !withLocked)) {
			continue;
		}
		const auto point = LayerPoint(layer, canvasPoint);
		if (point && QRectF(QPointF(), QSizeF(layer.size())).contains(*point)) {
			return layer.id;
		}
	}
	return 0;
}

TransformParts DecomposeTransform(
		const QTransform &transform,
		QSizeF content) {
	auto result = TransformParts();
	result.center = transform.map(
		QPointF(content.width() / 2., content.height() / 2.));
	const auto ax = transform.m11();
	const auto ay = transform.m12();
	const auto bx = transform.m21();
	const auto by = transform.m22();
	const auto length = std::hypot(ax, ay);
	if (length < 1e-12) {
		result.scaleX = 0.;
		result.scaleY = std::hypot(bx, by);
		return result;
	}
	const auto ux = ax / length;
	const auto uy = ay / length;
	result.scaleX = length;
	result.rotation = std::atan2(ay, ax) * 180. / kPi;
	result.scaleY = bx * (-uy) + by * ux;
	result.shear = (std::abs(result.scaleY) < 1e-12)
		? 0.
		: ((bx * ux + by * uy) / result.scaleY);
	return result;
}

QTransform ComposeTransform(const TransformParts &parts, QSizeF content) {
	auto result = QTransform();
	result.translate(parts.center.x(), parts.center.y());
	result.rotate(parts.rotation);
	result.shear(parts.shear, 0.);
	result.scale(parts.scaleX, parts.scaleY);
	result.translate(-content.width() / 2., -content.height() / 2.);
	return result;
}

Layer MakeLayer(ContentPtr content, QString name) {
	auto result = Layer();
	result.name = std::move(name);
	result.content = std::move(content);
	return result;
}

Layer MakeImageLayer(QImage image, QString name) {
	return MakeLayer(MakeImageContent(std::move(image)), std::move(name));
}

Document DocumentFromImage(QImage image, const EditState &state, QString name) {
	auto result = Document();
	if (image.isNull()) {
		return result;
	}
	auto layer = MakeImageLayer(std::move(image), std::move(name));
	result.size = layer.size();
	result.global = Normalized(state);
	AddLayer(result, std::move(layer));
	return result;
}

bool IsPlainImage(const Document &document) {
	if (document.layers.size() != 1) {
		return false;
	}
	const auto &layer = document.layers.front();
	return layer.visible
		&& (layer.opacity >= 1.)
		&& (layer.blend == BlendMode::Normal)
		&& layer.transform.isIdentity()
		&& AsImage(layer.content)
		&& (layer.size() == document.size)
		&& (!layer.mask || !layer.maskEnabled)
		&& layer.effects.empty();
}

LayerId AddLayer(Document &document, Layer layer, int index) {
	layer.id = document.nextId++;
	for (auto &instance : layer.effects) {
		instance.uid = document.nextId++;
	}
	const auto count = int(document.layers.size());
	const auto position = (index < 0 || index > count) ? count : index;
	const auto id = layer.id;
	document.layers.insert(
		begin(document.layers) + position,
		std::move(layer));
	return id;
}

bool RemoveLayer(Document &document, LayerId id) {
	const auto index = document.indexOf(id);
	if (index < 0) {
		return false;
	}
	document.layers.erase(begin(document.layers) + index);
	return true;
}

LayerId DuplicateLayer(Document &document, LayerId id) {
	const auto index = document.indexOf(id);
	if (index < 0) {
		return 0;
	}
	auto copy = document.layers[index];
	return AddLayer(document, std::move(copy), index + 1);
}

bool MoveLayer(Document &document, LayerId id, int index) {
	const auto from = document.indexOf(id);
	const auto count = int(document.layers.size());
	if (from < 0) {
		return false;
	}
	const auto to = std::clamp(index, 0, count - 1);
	if (to == from) {
		return false;
	}
	auto layer = std::move(document.layers[from]);
	document.layers.erase(begin(document.layers) + from);
	document.layers.insert(begin(document.layers) + to, std::move(layer));
	return true;
}

uint64 AddLayerFx(
		Document &document,
		LayerId id,
		FxInstance instance,
		int index) {
	const auto layer = document.find(id);
	if (!layer) {
		return 0;
	}
	instance.uid = document.nextId++;
	const auto count = int(layer->effects.size());
	const auto position = (index < 0 || index > count) ? count : index;
	const auto uid = instance.uid;
	layer->effects.insert(
		begin(layer->effects) + position,
		std::move(instance));
	return uid;
}

bool RemoveLayerFx(Document &document, LayerId id, uint64 uid) {
	const auto layer = document.find(id);
	if (!layer) {
		return false;
	}
	const auto i = ranges::find(layer->effects, uid, &FxInstance::uid);
	if (i == end(layer->effects)) {
		return false;
	}
	layer->effects.erase(i);
	return true;
}

bool MoveLayerFx(Document &document, LayerId id, uint64 uid, int index) {
	const auto layer = document.find(id);
	if (!layer) {
		return false;
	}
	const auto i = ranges::find(layer->effects, uid, &FxInstance::uid);
	if (i == end(layer->effects)) {
		return false;
	}
	const auto from = int(i - begin(layer->effects));
	const auto to = std::clamp(index, 0, int(layer->effects.size()) - 1);
	if (to == from) {
		return false;
	}
	auto instance = std::move(*i);
	layer->effects.erase(i);
	layer->effects.insert(begin(layer->effects) + to, std::move(instance));
	return true;
}

FxInstance *LayerFx(Document &document, LayerId id, uint64 uid) {
	const auto layer = document.find(id);
	if (!layer || !uid) {
		return nullptr;
	}
	const auto i = ranges::find(layer->effects, uid, &FxInstance::uid);
	return (i != end(layer->effects)) ? &*i : nullptr;
}

const FxInstance *LayerFx(const Document &document, LayerId id, uint64 uid) {
	const auto layer = document.find(id);
	if (!layer || !uid) {
		return nullptr;
	}
	const auto i = ranges::find(layer->effects, uid, &FxInstance::uid);
	return (i != end(layer->effects)) ? &*i : nullptr;
}

QSize ValidCanvasSize(QSize size) {
	auto width = std::clamp(size.width(), kCanvasMinSide, kCanvasMaxSide);
	auto height = std::clamp(size.height(), kCanvasMinSide, kCanvasMaxSide);
	const auto pixels = qint64(width) * height;
	if (pixels > kCanvasMaxPixels) {
		const auto scale = std::sqrt(double(kCanvasMaxPixels) / double(pixels));
		width = std::max(int(std::floor(width * scale)), kCanvasMinSide);
		height = std::max(int(std::floor(height * scale)), kCanvasMinSide);
	}
	return QSize(width, height);
}

QSize CanvasSizeForAspect(QSize current, int ratioWidth, int ratioHeight) {
	if (current.isEmpty() || ratioWidth <= 0 || ratioHeight <= 0) {
		return current;
	}
	// The width stays and the height grows, or the other way around.
	auto width = qint64(current.width());
	auto height = qint64(std::llround(
		double(width) * ratioHeight / ratioWidth));
	if (height < current.height()) {
		height = current.height();
		width = std::max(
			qint64(std::llround(double(height) * ratioWidth / ratioHeight)),
			qint64(current.width()));
	}
	const auto limit = qint64(4) * kCanvasMaxSide;
	return ValidCanvasSize(QSize(
		int(std::min(width, limit)),
		int(std::min(height, limit))));
}

Document CanvasResized(
		const Document &document,
		QSize size,
		QPointF anchor) {
	auto result = document;
	if (document.empty()) {
		return result;
	}
	size = ValidCanvasSize(size);
	if (size == document.size) {
		return result;
	}
	const auto shift = QPointF(
		std::round((size.width() - document.size.width())
			* std::clamp(anchor.x(), 0., 1.)),
		std::round((size.height() - document.size.height())
			* std::clamp(anchor.y(), 0., 1.)));
	result.size = size;
	if (!shift.isNull()) {
		const auto move = QTransform::fromTranslate(shift.x(), shift.y());
		for (auto &layer : result.layers) {
			layer.transform = layer.transform * move;
		}
	}
	result.global.crop = QRectF(0., 0., 1., 1.);
	return result;
}

QSize OutputSize(const Document &document) {
	return OutputSize(document.size, document.global);
}

QSize OutputSize(const Document &document, QSize maxSize) {
	return OutputSize(document.size, document.global, maxSize);
}

QTransform OutputTransform(const Document &document, QSize output) {
	return OutputTransform(document.size, document.global, output);
}

struct Compositor::Private {
	struct Entry {
		uint64 key = 0;
		QImage image;
		qint64 bytes = 0;
		uint64 used = 0;
	};
	struct Chain {
		uint64 slot = 0;
		std::vector<uint64> keys;
	};

	[[nodiscard]] QImage find(uint64 key);
	void store(uint64 key, const QImage &image);
	[[nodiscard]] std::vector<uint64> lastChain(uint64 slot);
	void setLastChain(uint64 slot, const std::vector<uint64> &keys);

	[[nodiscard]] QImage layerPixels(
		const LayerPlan &plan,
		const std::atomic<bool> *cancel,
		bool cache);
	[[nodiscard]] QImage flatten(
		const Document &document,
		const RenderRequest &request);

	std::mutex mutex;
	std::vector<Entry> entries;
	std::vector<Chain> chains;
	qint64 total = 0;
	qint64 limit = kDefaultCacheLimit;
	uint64 clock = 0;

};

QImage Compositor::Private::find(uint64 key) {
	const auto guard = std::lock_guard(mutex);
	for (auto &entry : entries) {
		if (entry.key == key) {
			entry.used = ++clock;
			return entry.image;
		}
	}
	return QImage();
}

void Compositor::Private::store(uint64 key, const QImage &image) {
	if (image.isNull()) {
		return;
	}
	const auto bytes = ImageBytes(image);
	const auto guard = std::lock_guard(mutex);
	if (bytes > limit) {
		return;
	}
	for (auto &entry : entries) {
		if (entry.key == key) {
			entry.used = ++clock;
			return;
		}
	}
	entries.push_back({ key, image, bytes, ++clock });
	total += bytes;
	while (total > limit && entries.size() > 1) {
		const auto oldest = ranges::min_element(entries, {}, &Entry::used);
		total -= oldest->bytes;
		entries.erase(oldest);
	}
}

std::vector<uint64> Compositor::Private::lastChain(uint64 slot) {
	const auto guard = std::lock_guard(mutex);
	const auto i = ranges::find(chains, slot, &Chain::slot);
	return (i != end(chains)) ? i->keys : std::vector<uint64>();
}

void Compositor::Private::setLastChain(
		uint64 slot,
		const std::vector<uint64> &keys) {
	const auto guard = std::lock_guard(mutex);
	const auto i = ranges::find(chains, slot, &Chain::slot);
	if (i != end(chains)) {
		i->keys = keys;
		return;
	} else if (int(chains.size()) >= kMaxChains) {
		chains.erase(begin(chains), begin(chains) + kMaxChains / 2);
	}
	chains.push_back({ slot, keys });
}

QImage Compositor::Private::layerPixels(
		const LayerPlan &plan,
		const std::atomic<bool> *cancel,
		bool cache) {
	const auto &layer = *plan.layer;
	const auto stages = int(plan.chain.size()) - 1;
	auto image = QImage();
	auto start = -1;
	for (auto i = stages; i >= 0; --i) {
		image = find(plan.chain[i]);
		if (!image.isNull()) {
			start = i;
			break;
		}
	}
	if (start < 0) {
		image = layer.content->render({
			.scale = plan.scale,
			.preview = plan.preview,
			.cancel = cancel,
		});
		if (image.isNull() || Cancelled(cancel)) {
			return QImage();
		}
		if (image.format() != QImage::Format_ARGB32_Premultiplied) {
			image = image.convertToFormat(
				QImage::Format_ARGB32_Premultiplied);
		}
		image.setDevicePixelRatio(1.);
		start = 0;
		// The full size pixels of an image layer are the layer itself.
		if (cache && (plan.scale < 1. || !AsImage(layer.content))) {
			store(plan.chain[0], image);
		}
	}
	if (start == stages) {
		return image;
	}
	const auto keep = cache ? KeepIndex(lastChain(plan.slot), plan.chain) : -1;
	const auto effects = int(plan.effects.size());
	for (auto i = start; i != stages; ++i) {
		if (i < effects) {
			const auto &instance = *plan.effects[i];
			const auto context = FxContext{
				.scale = image.width() / double(plan.contentSize.width()),
				.seed = uint32(FxHashCombine(kFxLayerSeed, instance.uid)),
				.fullSize = plan.contentSize,
				.preview = plan.preview,
				.cancel = cancel,
			};
			if (!ApplyFx(image, instance, context)) {
				return QImage();
			}
		} else if (!ApplyMask(image, *layer.mask)) {
			return QImage();
		}
		if (Cancelled(cancel)) {
			return QImage();
		} else if (cache && (i + 1 == keep) && (i + 1 != stages)) {
			store(plan.chain[i + 1], image);
		}
	}
	if (cache) {
		store(plan.chain[stages], image);
		setLastChain(plan.slot, plan.chain);
	}
	return image;
}

QImage Compositor::Private::flatten(
		const Document &document,
		const RenderRequest &request) {
	if (document.size.isEmpty()) {
		return QImage();
	}
	const auto scale = std::clamp(request.scale, kMinContentScale, 1.);
	const auto output = ScaledSize(document.size, scale);
	const auto scaleKey = uint64(std::llround(scale * kScaleKeyUnits));
	const auto base = Mix(
		Mix(Mix(0xF1A77E2ULL, uint64(output.width())), uint64(output.height())),
		Mix(scaleKey, request.preview));

	auto plans = std::vector<LayerPlan>();
	auto keys = std::vector<uint64>();
	auto active = -1;
	auto key = base;
	for (const auto &layer : document.layers) {
		if (!Renderable(layer)) {
			continue;
		}
		if (layer.id == request.active) {
			active = int(plans.size());
		}
		plans.push_back(PlanLayer(
			layer,
			ContentScale(layer.transform, layer.size(), scale),
			request.preview));
		key = Mix(key, plans.back().chain.back());
		key = MixTransform(key, layer.transform);
		key = Mix(key, OpacityByte(layer.opacity));
		key = Mix(key, uint64(layer.blend));
		keys.push_back(key);
	}
	const auto count = int(plans.size());
	auto result = QImage();
	auto start = -1;
	for (auto i = count; i != 0;) {
		result = find(keys[--i]);
		if (!result.isNull()) {
			start = i;
			break;
		}
	}
	if (start == count - 1 && count > 0) {
		return result;
	}
	const auto slot = Mix(base, 0x51077ULL);
	const auto keep = !request.cache
		? -1
		: (active > 0)
		? (active - 1)
		: KeepIndex(lastChain(slot), keys);
	for (auto i = start + 1; i != count; ++i) {
		const auto &plan = plans[i];
		const auto &layer = *plan.layer;
		const auto pixels = layerPixels(plan, request.cancel, request.cache);
		if (pixels.isNull() || Cancelled(request.cancel)) {
			return QImage();
		}
		auto placed = Place(
			pixels,
			plan.contentSize,
			layer.transform,
			scale,
			output);
		if (!placed.image.isNull()) {
			const auto covers = placed.position.isNull()
				&& (placed.image.size() == output);
			if (result.isNull() && covers && OpacityByte(layer.opacity) == 255) {
				result = std::move(placed.image);
			} else {
				if (result.isNull()) {
					result = QImage(
						output,
						QImage::Format_ARGB32_Premultiplied);
					if (result.isNull()) {
						return QImage();
					}
					result.fill(Qt::transparent);
				}
				BlendImage(
					result,
					placed.image,
					placed.position,
					layer.blend,
					layer.opacity);
			}
		}
		if (Cancelled(request.cancel)) {
			return QImage();
		} else if (request.cache
			&& (i == keep)
			&& (i + 1 != count)
			&& !result.isNull()) {
			store(keys[i], result);
		}
	}
	if (result.isNull()) {
		result = QImage(output, QImage::Format_ARGB32_Premultiplied);
		if (result.isNull()) {
			return QImage();
		}
		result.fill(Qt::transparent);
	}
	result.setDevicePixelRatio(1.);
	if (request.cache && count > 0) {
		store(keys[count - 1], result);
		setLastChain(slot, keys);
	}
	return result;
}

Compositor::Compositor() : _private(std::make_unique<Private>()) {
}

Compositor::~Compositor() = default;

QImage Compositor::render(
		const Document &document,
		const RenderRequest &request) {
	const auto flat = _private->flatten(document, request);
	if (flat.isNull() || Cancelled(request.cancel)) {
		return QImage();
	} else if (!request.global) {
		return request.maxSize.isEmpty()
			? flat
			: FxResized(flat, FitSize(flat.size(), request.maxSize));
	}
	return Render(flat, document.global, request.maxSize, request.cancel);
}

QImage Compositor::layerPixels(
		const Layer &layer,
		double scale,
		const std::atomic<bool> *cancel,
		bool preview) {
	if (!layer.content || layer.size().isEmpty()) {
		return QImage();
	}
	const auto plan = PlanLayer(
		layer,
		ContentScale(QTransform(), layer.size(), scale),
		preview);
	return _private->layerPixels(plan, cancel, true);
}

void Compositor::setMemoryLimit(qint64 bytes) {
	const auto guard = std::lock_guard(_private->mutex);
	_private->limit = std::max(bytes, qint64(0));
	while (_private->total > _private->limit && !_private->entries.empty()) {
		const auto oldest = ranges::min_element(
			_private->entries,
			{},
			&Private::Entry::used);
		_private->total -= oldest->bytes;
		_private->entries.erase(oldest);
	}
}

void Compositor::clear() {
	const auto guard = std::lock_guard(_private->mutex);
	_private->entries.clear();
	_private->chains.clear();
	_private->total = 0;
}

QImage RenderDocument(
		const Document &document,
		QSize maxSize,
		const std::atomic<bool> *cancel) {
	auto compositor = Compositor();
	return compositor.render(document, {
		.scale = 1.,
		.maxSize = maxSize,
		.cache = false,
		.cancel = cancel,
	});
}

QImage RasterizeLayer(
		const Layer &layer,
		QSize canvas,
		const std::atomic<bool> *cancel) {
	if (canvas.isEmpty()) {
		return QImage();
	}
	auto document = Document();
	document.size = canvas;
	document.layers.push_back(layer);
	auto &copy = document.layers.back();
	copy.id = 1;
	copy.visible = true;
	copy.opacity = 1.;
	copy.blend = BlendMode::Normal;
	auto compositor = Compositor();
	return compositor.render(document, {
		.scale = 1.,
		.global = false,
		.cache = false,
		.cancel = cancel,
	});
}

namespace {

// What BlendImage() gives over a transparent backdrop in the normal mode,
// without a second image. False if the memory can't be allocated.
[[nodiscard]] bool MultiplyOpacity(QImage &image, double opacity) {
	const auto alpha = OpacityByte(opacity);
	if (alpha == 255) {
		return true;
	} else if (!FxPrepare(image)) {
		return false;
	}
	const auto width = image.width();
	FxParallelRows(width, image.height(), [&](int begin, int end) {
		for (auto y = begin; y != end; ++y) {
			const auto line = FxRow(image, y);
			for (auto x = 0; x != width; ++x) {
				line[x] = MulPixel(line[x], alpha);
			}
		}
	});
	return true;
}

// What MergedDown() makes of two layers.
struct MergeParts {
	bool hidden = false; // Both are hidden and merged as if shown.
	bool withUpper = false; // The upper layer shows something.
	bool alone = false; // The lower one doesn't, the upper one replaces it.
};

[[nodiscard]] MergeParts PartsOfMerge(const Layer &lower, const Layer &upper) {
	const auto hidden = !lower.visible && !upper.visible;
	const auto shows = [&](const Layer &layer) {
		return (layer.visible || hidden)
			&& layer.content
			&& !layer.size().isEmpty()
			&& (OpacityByte(layer.opacity) > 0);
	};
	const auto withUpper = shows(upper);
	return {
		.hidden = hidden,
		.withUpper = withUpper,
		.alone = withUpper && !shows(lower),
	};
}

} // namespace

bool MergeDownUnderBlendMode(const Document &document, LayerId id) {
	const auto index = document.indexOf(id);
	if (index <= 0) {
		return false;
	}
	const auto &lower = document.layers[index - 1];
	const auto parts = PartsOfMerge(lower, document.layers[index]);
	return parts.withUpper
		&& !parts.alone
		&& (lower.blend != BlendMode::Normal);
}

std::optional<Document> MergedDown(
		const Document &document,
		LayerId id,
		const std::atomic<bool> *cancel) {
	const auto index = document.indexOf(id);
	if (index <= 0) {
		return document;
	}
	const auto &upper = document.layers[index];
	const auto &lower = document.layers[index - 1];
	const auto [hidden, withUpper, alone] = PartsOfMerge(lower, upper);
	const auto baked = withUpper
		&& !alone
		&& (lower.blend == BlendMode::Normal);
	auto pixels = RasterizeLayer(alone ? upper : lower, document.size, cancel);
	if (pixels.isNull() || Cancelled(cancel)) {
		return std::nullopt;
	}
	if (withUpper && !alone) {
		if (baked && !MultiplyOpacity(pixels, lower.opacity)) {
			return std::nullopt;
		}
		const auto over = RasterizeLayer(upper, document.size, cancel);
		if (over.isNull() || Cancelled(cancel)) {
			return std::nullopt;
		}
		BlendImage(pixels, over, QPoint(), upper.blend, upper.opacity);
	}
	auto result = document;
	auto &merged = result.layers[index - 1];
	merged.content = MakeImageContent(std::move(pixels));
	merged.transform = QTransform();
	merged.mask = nullptr;
	merged.maskEnabled = true;
	merged.effects.clear();
	if (alone) {
		merged.opacity = upper.opacity;
		merged.blend = upper.blend;
		merged.visible = upper.visible;
	} else if (baked) {
		merged.opacity = 1.;
		merged.visible = !hidden;
	}
	result.layers.erase(begin(result.layers) + index);
	return result;
}

std::optional<Document> Flattened(
		const Document &document,
		const std::atomic<bool> *cancel) {
	const auto first = ranges::find_if(document.layers, Renderable);
	if (document.size.isEmpty() || first == end(document.layers)) {
		return document;
	}
	auto compositor = Compositor();
	auto pixels = compositor.render(document, {
		.scale = 1.,
		.global = false,
		.cache = false,
		.cancel = cancel,
	});
	if (pixels.isNull() || Cancelled(cancel)) {
		return std::nullopt;
	}
	auto result = document;
	auto layer = MakeImageLayer(std::move(pixels), first->name);
	layer.id = first->id;
	result.layers.clear();
	result.layers.push_back(std::move(layer));
	return result;
}

History::History(Document initial) {
	_list.push_back(std::move(initial));
}

void History::reset(Document initial) {
	_list.clear();
	_list.push_back(std::move(initial));
	_index = 0;
}

const Document &History::current() const {
	return _list[_index];
}

bool History::push(Document document) {
	if (_list[_index] == document) {
		return false;
	}
	_list.resize(_index + 1);
	_list.push_back(std::move(document));
	_index = int(_list.size()) - 1;
	trim();
	return true;
}

bool History::canUndo() const {
	return (_index > 0);
}

bool History::canRedo() const {
	return (_index + 1 < int(_list.size()));
}

bool History::undo() {
	if (!canUndo()) {
		return false;
	}
	--_index;
	return true;
}

bool History::redo() {
	if (!canRedo()) {
		return false;
	}
	++_index;
	return true;
}

int History::index() const {
	return _index;
}

int History::count() const {
	return int(_list.size());
}

qint64 History::memoryUsage() const {
	return countBytes(0, int(_list.size()));
}

qint64 History::countBytes(int from, int till) const {
	auto seen = std::vector<const void*>();
	auto result = qint64(0);
	const auto once = [&](const void *pointer) {
		if (!pointer || ranges::contains(seen, pointer)) {
			return false;
		}
		seen.push_back(pointer);
		return true;
	};
	const auto visit = Fn<void(const void*, qint64)>([&](
			const void *object,
			qint64 bytes) {
		if (once(object)) {
			result += bytes;
		}
	});
	for (auto i = std::max(from, 0); i < till && i < int(_list.size()); ++i) {
		for (const auto &layer : _list[i].layers) {
			if (layer.content) {
				layer.content->memoryParts(visit);
			}
			if (once(layer.mask.get())) {
				result += layer.mask->memoryUsage();
			}
		}
	}
	return result;
}

void History::setLimits(int steps, qint64 bytes) {
	_maxSteps = std::max(steps, 2);
	_maxBytes = std::max(bytes, qint64(0));
	trim();
}

void History::trim() {
	while (_index > 0 && int(_list.size()) > _maxSteps) {
		_list.erase(begin(_list));
		--_index;
	}
	// Dropping steps frees nothing of what the current document holds.
	while (_index > 1
		&& (memoryUsage() - countBytes(_index, _index + 1) > _maxBytes)) {
		_list.erase(begin(_list));
		--_index;
	}
}

bool RunDocSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto info = [&](const QString &what) {
		log.push_back(u"   "_q + what);
	};
	const auto opaque = [](int r, int g, int b) {
		return qRgba(r, g, b, 255);
	};
	const auto closeTo = [](QRgb a, QRgb b, int tolerance = 1) {
		return std::abs(qAlpha(a) - qAlpha(b)) <= tolerance
			&& std::abs(qRed(a) - qRed(b)) <= tolerance
			&& std::abs(qGreen(a) - qGreen(b)) <= tolerance
			&& std::abs(qBlue(a) - qBlue(b)) <= tolerance;
	};

	// Blend maths on single pixels.
	{
		const auto b = opaque(200, 100, 50);
		const auto s = opaque(100, 200, 250);
		const auto blend = [&](BlendMode mode) {
			return BlendPixel(b, s, mode);
		};
		check(blend(BlendMode::Normal) == s, u"normal: source wins"_q);
		check(
			closeTo(blend(BlendMode::Multiply), opaque(78, 78, 49)),
			u"multiply"_q);
		check(
			closeTo(blend(BlendMode::Screen), opaque(222, 222, 251)),
			u"screen"_q);
		check(
			closeTo(blend(BlendMode::Darken), opaque(100, 100, 50))
				&& closeTo(blend(BlendMode::Lighten), opaque(200, 200, 250)),
			u"darken / lighten"_q);
		check(
			closeTo(blend(BlendMode::Difference), opaque(100, 100, 200)),
			u"difference"_q);
		check(
			closeTo(blend(BlendMode::Add), opaque(255, 255, 255)),
			u"add clamps"_q);
		check(
			closeTo(blend(BlendMode::Exclusion), opaque(143, 143, 202)),
			u"exclusion"_q);
		check(
			closeTo(blend(BlendMode::Overlay), opaque(188, 157, 98))
				&& closeTo(blend(BlendMode::HardLight), opaque(157, 188, 247)),
			u"overlay / hard light"_q);
		check(
			closeTo(blend(BlendMode::ColorDodge), opaque(255, 255, 255))
				&& closeTo(
					BlendPixel(
						opaque(51, 102, 0),
						opaque(128, 0, 255),
						BlendMode::ColorDodge),
					opaque(102, 102, 0)),
			u"color dodge"_q);
		check(
			closeTo(
				BlendPixel(
					opaque(204, 102, 255),
					opaque(128, 0, 51),
					BlendMode::ColorBurn),
				opaque(153, 0, 255)),
			u"color burn"_q);
		check(
			closeTo(
				BlendPixel(
					opaque(128, 128, 128),
					opaque(128, 128, 128),
					BlendMode::SoftLight),
				opaque(128, 128, 128),
				2)
				&& qRed(BlendPixel(
					opaque(128, 128, 128),
					opaque(255, 255, 255),
					BlendMode::SoftLight)) > 170,
			u"soft light"_q);
		const auto gray = opaque(90, 90, 90);
		const auto luminosity = BlendPixel(b, gray, BlendMode::Luminosity);
		const auto lum = [](QRgb c) {
			return 0.3 * qRed(c) + 0.59 * qGreen(c) + 0.11 * qBlue(c);
		};
		check(
			std::abs(lum(luminosity) - 90.) < 2.
				&& qRed(luminosity) > qGreen(luminosity)
				&& qGreen(luminosity) > qBlue(luminosity),
			u"luminosity keeps the backdrop color"_q);
		const auto colored = BlendPixel(gray, s, BlendMode::Color);
		check(
			std::abs(lum(colored) - 90.) < 2.
				&& qBlue(colored) > qRed(colored),
			u"color keeps the backdrop luminosity"_q);
		const auto hue = BlendPixel(b, opaque(0, 0, 255), BlendMode::Hue);
		check(
			std::abs(lum(hue) - lum(b)) < 2. && qBlue(hue) > qRed(hue),
			u"hue takes the source hue"_q);
		const auto saturation = BlendPixel(b, gray, BlendMode::Saturation);
		check(
			closeTo(
				saturation,
				opaque(
					qRound(lum(b)),
					qRound(lum(b)),
					qRound(lum(b))),
				2),
			u"saturation of a gray source removes the color"_q);

		// Premultiplied alpha.
		const auto half = qRgba(128, 0, 0, 128);
		check(
			closeTo(
				BlendPixel(opaque(0, 0, 255), half, BlendMode::Normal),
				opaque(128, 0, 127)),
			u"half transparent red over blue"_q);
		check(
			BlendPixel(0, half, BlendMode::Multiply) == half
				&& BlendPixel(half, 0, BlendMode::Screen) == half,
			u"a transparent side leaves the other one"_q);
		check(
			closeTo(
				BlendPixel(0, opaque(200, 100, 50), BlendMode::Normal, 0.5),
				qRgba(100, 50, 25, 128)),
			u"opacity scales the source"_q);
		const auto both = BlendPixel(
			qRgba(0, 0, 100, 100),
			qRgba(100, 0, 0, 100),
			BlendMode::Normal);
		check(
			closeTo(both, qRgba(100, 0, 61, 161)),
			u"two half transparent pixels"_q);
		auto valid = true;
		for (const auto mode : BlendModes()) {
			for (auto i = 0; i != 64; ++i) {
				const auto a1 = (i * 37) % 256;
				const auto a2 = (i * 91 + 13) % 256;
				const auto d = qRgba(
					(i * 53) % (a1 + 1),
					(i * 17) % (a1 + 1),
					(i * 201) % (a1 + 1),
					a1);
				const auto c = qRgba(
					(i * 29) % (a2 + 1),
					(i * 113) % (a2 + 1),
					(i * 7) % (a2 + 1),
					a2);
				const auto result = BlendPixel(d, c, mode, 0.75);
				valid = valid
					&& qRed(result) <= qAlpha(result)
					&& qGreen(result) <= qAlpha(result)
					&& qBlue(result) <= qAlpha(result);
			}
		}
		check(valid, u"every mode gives valid premultiplied pixels"_q);
		check(
			BlendModeFromKey(BlendModeKey(BlendMode::SoftLight))
				== BlendMode::SoftLight
				&& !BlendModeFromKey("nope")
				&& int(BlendModes().size()) == kBlendModeCount,
			u"blend mode keys"_q);
	}

	const auto backdropImage = FxTestImage(48, 32, true);
	const auto sourceImage = [&] {
		auto result = FxTestImage(24, 20, true);
		auto p = QPainter(&result);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.fillRect(2, 2, 6, 6, QColor(255, 255, 255, 255));
		p.fillRect(10, 3, 6, 6, QColor(0, 0, 0, 255));
		p.fillRect(4, 12, 8, 5, QColor(40, 200, 90, 120));
		p.end();
		return result;
	}();

	// Images against the per-pixel reference.
	{
		auto worst = 0;
		for (const auto mode : BlendModes()) {
			auto blended = backdropImage;
			const auto position = QPoint(30, -6);
			BlendImage(blended, sourceImage, position, mode, 0.7);
			auto exact = backdropImage;
			for (auto y = 0; y != sourceImage.height(); ++y) {
				const auto from = FxRow(sourceImage, y);
				const auto ty = y + position.y();
				if (ty < 0 || ty >= exact.height()) {
					continue;
				}
				const auto to = FxRow(exact, ty);
				for (auto x = 0; x != sourceImage.width(); ++x) {
					const auto tx = x + position.x();
					if (tx >= 0 && tx < exact.width()) {
						to[tx] = BlendPixel(to[tx], from[x], mode, 0.7);
					}
				}
			}
			worst = std::max(worst, MaxDifference(blended, exact));
		}
		check(
			worst <= 1,
			u"BlendImage equals the reference in every mode (max "
			"difference %1)"_q.arg(worst));
	}

	// Transforms.
	{
		const auto size = QSizeF(400., 300.);
		const auto parts = TransformParts{
			.center = QPointF(520., 410.),
			.scaleX = 1.5,
			.scaleY = -0.75,
			.rotation = 33.,
			.shear = 0.2,
		};
		const auto composed = ComposeTransform(parts, size);
		const auto back = DecomposeTransform(composed, size);
		const auto same = [](double a, double b) {
			return std::abs(a - b) < 1e-6;
		};
		check(
			same(back.center.x(), parts.center.x())
				&& same(back.center.y(), parts.center.y())
				&& same(back.scaleX, parts.scaleX)
				&& same(back.scaleY, parts.scaleY)
				&& same(back.rotation, parts.rotation)
				&& same(back.shear, parts.shear),
			u"transform parts round trip"_q);
		const auto center = composed.map(QPointF(200., 150.));
		check(
			same(center.x(), 520.) && same(center.y(), 410.),
			u"the content center goes to the given point"_q);

		auto quad = QPolygonF();
		quad << QPointF(10., 20.) << QPointF(300., 40.)
			<< QPointF(280., 260.) << QPointF(30., 200.);
		auto perspective = QTransform();
		const auto made = QuadTransform(size, quad, perspective);
		auto layer = MakeImageLayer(FxTestImage(400, 300), u"q"_q);
		layer.transform = perspective;
		const auto mapped = LayerQuad(layer);
		auto matches = made && (mapped.size() == 4);
		for (auto i = 0; matches && i != 4; ++i) {
			matches = same(mapped[i].x(), quad[i].x())
				&& same(mapped[i].y(), quad[i].y());
		}
		const auto inside = LayerPoint(layer, QPointF(150., 120.));
		check(
			matches
				&& IsPerspective(perspective)
				&& inside
				&& QRectF(QPointF(), size).contains(*inside),
			u"a 4-corner placement maps the corners"_q);
		auto folded = QPolygonF();
		folded << QPointF(0., 0.) << QPointF(100., 100.)
			<< QPointF(100., 0.) << QPointF(0., 100.);
		auto unused = QTransform();
		check(
			!QuadTransform(size, folded, unused),
			u"a folded quad is refused"_q);

		const auto fit = PlaceTransform(
			QSize(400, 300),
			QSize(200, 200),
			Placement::Fit);
		const auto fitted = fit.mapRect(QRectF(0., 0., 400., 300.));
		check(
			same(fitted.width(), 200.)
				&& same(fitted.height(), 150.)
				&& same(fitted.y(), 25.),
			u"fit placement"_q);
	}

	// Document operations.
	{
		auto document = DocumentFromImage(FxTestImage(64, 48), EditState());
		check(
			IsPlainImage(document)
				&& document.size == QSize(64, 48)
				&& document.layers.size() == 1
				&& document.layers.front().id != 0,
			u"a document from an image is a plain image"_q);
		const auto first = document.layers.front().id;
		auto added = MakeImageLayer(FxTestImage(16, 16), u"second"_q);
		added.effects.push_back(MakeFx("classic.invert"));
		const auto second = AddLayer(document, std::move(added));
		const auto copy = DuplicateLayer(document, first);
		check(
			second != first
				&& copy != 0
				&& document.layers.size() == 3
				&& document.indexOf(copy) == 1
				&& document.layers[1].content == document.layers[0].content
				&& document.find(second)->effects.front().uid != 0
				&& !IsPlainImage(document),
			u"add and duplicate layers"_q);
		const auto uid = AddLayerFx(
			document,
			first,
			MakeFx("classic.sepia"));
		const auto other = AddLayerFx(
			document,
			first,
			MakeFx("classic.posterize"),
			0);
		check(
			uid != 0
				&& other != uid
				&& document.find(first)->effects.front().uid == other
				&& LayerFx(document, first, uid)
				&& MoveLayerFx(document, first, uid, 0)
				&& document.find(first)->effects.front().uid == uid
				&& RemoveLayerFx(document, first, other)
				&& document.find(first)->effects.size() == 1,
			u"add, move and remove effects"_q);
		check(
			MoveLayer(document, first, 2)
				&& document.indexOf(first) == 2
				&& !MoveLayer(document, first, 9)
				&& RemoveLayer(document, copy)
				&& document.layers.size() == 2
				&& !RemoveLayer(document, copy),
			u"move and remove layers"_q);
		check(
			LayerAt(document, QPointF(5., 5.)) == first
				&& LayerAt(document, QPointF(-5., 5.)) == 0,
			u"the topmost layer under a point"_q);
	}

	// The old single-image state is a one-layer document.
	const auto photo = FxTestImage(320, 240);
	{
		auto state = EditState();
		state.exposure = 18;
		state.contrast = 12;
		state.vignette = 30;
		state.filter = u"film"_q;
		state.quarterTurns = 1;
		state.crop = QRectF(0.1, 0.05, 0.7, 0.8);
		state.effects.push_back(DefaultEffect(EffectType::Posterize));
		const auto document = DocumentFromImage(photo, state);
		const auto expected = Render(photo, state);
		check(
			SamePixels(RenderDocument(document), expected)
				&& OutputSize(document) == expected.size(),
			u"a one-layer document renders exactly like the old editor"_q);
		const auto limited = QSize(100, 100);
		check(
			SamePixels(
				RenderDocument(document, limited),
				Render(photo, state, limited)),
			u"the same with a size limit"_q);
		auto compositor = Compositor();
		const auto preview = compositor.render(document, {
			.scale = 0.5,
			.maxSize = QSize(90, 90),
			.preview = true,
		});
		check(
			SamePixels(
				preview,
				Render(
					PrepareSource(photo, photo.size() / 2),
					state,
					QSize(90, 90))),
			u"the preview equals the old downscaled preview"_q);
	}

	// The compositor against a per-pixel reference.
	const auto makeDocument = [&] {
		auto document = DocumentFromImage(photo, EditState(), u"base"_q);
		auto second = MakeImageLayer(FxTestImage(120, 90, true), u"second"_q);
		second.transform = QTransform::fromTranslate(40., 30.);
		second.blend = BlendMode::Multiply;
		second.opacity = 0.7;
		second.effects.push_back(MakeFx("classic.light", {
			{ "exposure", FxValue::Integer(25) },
		}));
		second.effects.push_back(MakeFx("classic.posterize"));
		second.effects.push_back(MakeFx("classic.color", {
			{ "saturation", FxValue::Integer(-40) },
		}));
		AddLayer(document, std::move(second));
		auto third = MakeImageLayer(FxTestImage(100, 100, false), u"third"_q);
		third.transform = QTransform::fromTranslate(250., -20.);
		third.blend = BlendMode::Screen;
		auto mask = QImage(100, 100, QImage::Format_Grayscale8);
		for (auto y = 0; y != 100; ++y) {
			const auto line = mask.scanLine(y);
			for (auto x = 0; x != 100; ++x) {
				line[x] = uchar(std::clamp(x * 3, 0, 255));
			}
		}
		third.mask = MakeMask(std::move(mask));
		AddLayer(document, std::move(third));
		return document;
	};
	{
		const auto document = makeDocument();
		auto compositor = Compositor();
		const auto rendered = compositor.render(document, {
			.scale = 1.,
			.global = false,
		});
		auto reference = QImage(
			document.size,
			QImage::Format_ARGB32_Premultiplied);
		reference.fill(Qt::transparent);
		for (const auto &layer : document.layers) {
			auto pixels = AsImage(layer.content)->image();
			for (const auto &instance : layer.effects) {
				const auto applied = ApplyFx(pixels, instance, FxContext{
					.scale = 1.,
					.seed = uint32(FxHashCombine(kFxLayerSeed, instance.uid)),
					.fullSize = pixels.size(),
				});
				ok = ok && applied;
			}
			const auto dx = int(layer.transform.dx());
			const auto dy = int(layer.transform.dy());
			for (auto y = 0; y != pixels.height(); ++y) {
				const auto ty = y + dy;
				if (ty < 0 || ty >= reference.height()) {
					continue;
				}
				const auto from = FxRow(pixels, y);
				const auto to = FxRow(reference, ty);
				const auto mask = layer.mask
					? layer.mask->image().constScanLine(y)
					: nullptr;
				for (auto x = 0; x != pixels.width(); ++x) {
					const auto tx = x + dx;
					if (tx < 0 || tx >= reference.width()) {
						continue;
					}
					auto source = from[x];
					if (mask) {
						const auto m = mask[x] / 255.;
						source = qRgba(
							qRound(qRed(source) * m),
							qRound(qGreen(source) * m),
							qRound(qBlue(source) * m),
							qRound(qAlpha(source) * m));
					}
					to[tx] = BlendPixel(
						to[tx],
						source,
						layer.blend,
						layer.opacity);
				}
			}
		}
		const auto difference = MaxDifference(rendered, reference);
		check(
			difference <= 1,
			u"three layers equal the per-pixel reference (max difference "
			"%1)"_q.arg(difference));
		check(
			SamePixels(
				rendered,
				compositor.render(document, { .scale = 1., .global = false })),
			u"a cached render is the same picture"_q);

		// Caches: every change must give what a fresh compositor gives.
		auto consistent = true;
		auto changed = document;
		const auto verify = [&](LayerId active) {
			const auto request = RenderRequest{
				.scale = 1.,
				.global = false,
				.active = active,
			};
			const auto cached = compositor.render(changed, request);
			auto fresh = Compositor();
			consistent = consistent
				&& !cached.isNull()
				&& SamePixels(cached, fresh.render(changed, request));
		};
		const auto middle = changed.layers[1].id;
		for (auto i = 0; i != 4; ++i) {
			changed.layers[1].effects[1].params.set(
				"levels",
				FxValue::Integer(3 + i));
			verify((i % 2) ? middle : 0);
		}
		changed.layers[2].opacity = 0.4;
		verify(changed.layers[2].id);
		changed.layers[1].effects[0].enabled = false;
		verify(middle);
		changed.layers[0].effects.push_back(MakeFx("classic.sepia"));
		changed.layers[0].effects.back().uid = changed.nextId++;
		verify(changed.layers[0].id);
		changed.layers[2].maskEnabled = false;
		verify(0);
		changed.layers[1].visible = false;
		verify(0);
		std::swap(changed.layers[1], changed.layers[2]);
		changed.layers[2].visible = true;
		verify(0);
		check(consistent, u"cached renders equal fresh ones after edits"_q);

		// A smaller scale is the same picture, smaller.
		const auto half = compositor.render(document, {
			.scale = 0.5,
			.global = false,
			.preview = true,
		});
		const auto difference2 = FxImageDifference(
			half,
			FxResized(rendered, half.size()));
		check(
			half.size() == QSize(160, 120) && difference2 < 3.,
			u"the half scale render matches (differs by %1)"_q.arg(
				QString::number(difference2, 'f', 2)));

		// Cancelling.
		const auto cancel = std::atomic<bool>(true);
		auto fresh = Compositor();
		check(
			fresh.render(document, {
				.scale = 1.,
				.cancel = &cancel,
			}).isNull(),
			u"a cancelled render gives nothing"_q);
	}

	// A transformed layer.
	{
		auto document = DocumentFromImage(photo, EditState());
		auto layer = MakeImageLayer(FxTestImage(100, 60), u"turned"_q);
		layer.effects.push_back(MakeFx("classic.invert"));
		layer.transform = ComposeTransform({
			.center = QPointF(160., 120.),
			.scaleX = 1.6,
			.scaleY = 1.6,
			.rotation = 30.,
		}, QSizeF(100., 60.));
		const auto id = AddLayer(document, std::move(layer));
		const auto rendered = RenderDocument(document);
		const auto plain = Render(photo, EditState());
		const auto centerChanged = (rendered.pixel(190, 120)
			!= plain.pixel(190, 120));
		const auto cornerKept = (rendered.pixel(2, 2) == plain.pixel(2, 2));
		check(
			!rendered.isNull()
				&& centerChanged
				&& cornerKept
				&& SamePixels(rendered, RenderDocument(document)),
			u"a rotated and scaled layer is drawn where it is placed"_q);
		auto quad = QPolygonF();
		quad << QPointF(20., 20.) << QPointF(300., 60.)
			<< QPointF(260., 220.) << QPointF(60., 180.);
		const auto placed = QuadTransform(
			QSizeF(100., 60.),
			quad,
			document.find(id)->transform);
		const auto warped = RenderDocument(document);
		check(
			placed
				&& !warped.isNull()
				&& warped.pixel(160, 120) != plain.pixel(160, 120)
				&& warped.pixel(5, 230) == plain.pixel(5, 230),
			u"a perspective layer is drawn inside its quad"_q);

		const auto merged = MergedDown(document, id);
		check(
			merged
				&& merged->layers.size() == 1
				&& MaxDifference(
					RenderDocument(*merged),
					RenderDocument(document)) <= 1,
			u"merge down keeps the picture"_q);
		auto two = makeDocument();
		const auto flat = Flattened(two);
		auto compositor = Compositor();
		check(
			flat
				&& flat->layers.size() == 1
				&& SamePixels(
					RenderDocument(*flat),
					RenderDocument(two)),
			u"flatten keeps the picture"_q);
	}

	// Merge down when the lower layer is not a plain opaque one.
	{
		const auto build = [&](Fn<void(Layer &lower, Layer &upper)> modify) {
			auto document = DocumentFromImage(photo, EditState(), u"base"_q);
			auto lower = MakeImageLayer(FxTestImage(160, 120, true), u"lower"_q);
			lower.transform = QTransform::fromTranslate(30., 40.);
			auto plate = QImage(90, 70, QImage::Format_ARGB32_Premultiplied);
			plate.fill(QColor(220, 40, 60));
			auto upper = MakeImageLayer(std::move(plate), u"upper"_q);
			upper.transform = QTransform::fromTranslate(120., 90.);
			modify(lower, upper);
			AddLayer(document, std::move(lower));
			AddLayer(document, std::move(upper));
			return document;
		};
		const auto difference = [](const Document &a, const Document &b) {
			return MaxDifference(RenderDocument(a), RenderDocument(b));
		};

		const auto faded = build([](Layer &lower, Layer &upper) {
			lower.opacity = 0.5;
			upper.opacity = 0.6;
		});
		const auto fadedMerged = MergedDown(faded, faded.layers[2].id);
		check(
			fadedMerged
				&& (fadedMerged->layers.size() == 2)
				&& (fadedMerged->layers[1].id == faded.layers[1].id)
				&& (fadedMerged->layers[1].opacity == 1.)
				&& fadedMerged->layers[1].visible
				&& (difference(*fadedMerged, faded) <= 3),
			u"merge down: the opacity of the lower layer goes into the pixels"_q);

		const auto covered = build([](Layer &lower, Layer &upper) {
			lower.visible = false;
			upper.opacity = 0.8;
			upper.blend = BlendMode::Multiply;
		});
		const auto coveredMerged = MergedDown(covered, covered.layers[2].id);
		check(
			coveredMerged
				&& (coveredMerged->layers.size() == 2)
				&& coveredMerged->layers[1].visible
				&& (coveredMerged->layers[1].blend == BlendMode::Multiply)
				&& (coveredMerged->layers[1].opacity == 0.8)
				&& (difference(*coveredMerged, covered) <= 1),
			u"merge down: a hidden lower layer doesn't hide the upper one"_q);

		const auto dropped = build([](Layer &lower, Layer &upper) {
			lower.opacity = 0.5;
			lower.blend = BlendMode::Multiply;
			upper.visible = false;
		});
		const auto droppedMerged = MergedDown(dropped, dropped.layers[2].id);
		check(
			droppedMerged
				&& (droppedMerged->layers.size() == 2)
				&& droppedMerged->layers[1].visible
				&& (droppedMerged->layers[1].blend == BlendMode::Multiply)
				&& (droppedMerged->layers[1].opacity == 0.5)
				&& (difference(*droppedMerged, dropped) <= 1),
			u"merge down: a hidden upper layer is dropped"_q);

		const auto both = build([](Layer &lower, Layer &upper) {
			lower.visible = false;
			lower.opacity = 0.5;
			upper.visible = false;
		});
		auto bothShown = both;
		bothShown.layers[1].visible = true;
		bothShown.layers[2].visible = true;
		auto bothMerged = MergedDown(both, both.layers[2].id);
		const auto stayedHidden = bothMerged
			&& (bothMerged->layers.size() == 2)
			&& !bothMerged->layers[1].visible
			&& (difference(*bothMerged, both) <= 1);
		if (bothMerged) {
			bothMerged->layers[1].visible = true;
		}
		check(
			stayedHidden && (difference(*bothMerged, bothShown) <= 3),
			u"merge down: two hidden layers stay hidden and keep their look"_q);

		const auto mode = build([](Layer &lower, Layer &upper) {
			lower.opacity = 0.5;
			lower.blend = BlendMode::Screen;
		});
		const auto modeMerged = MergedDown(mode, mode.layers[2].id);
		check(
			modeMerged
				&& (modeMerged->layers.size() == 2)
				&& (modeMerged->layers[1].blend == BlendMode::Screen)
				&& (modeMerged->layers[1].opacity == 0.5),
			u"merge down: a lower layer with a blend mode keeps it"_q);

		auto modeHidden = mode;
		modeHidden.layers[1].visible = false;
		modeHidden.layers[2].visible = false;
		auto modeClear = mode;
		modeClear.layers[1].opacity = 0.;
		check(
			!MergeDownUnderBlendMode(faded, faded.layers[2].id)
				&& !MergeDownUnderBlendMode(covered, covered.layers[2].id)
				&& !MergeDownUnderBlendMode(dropped, dropped.layers[2].id)
				&& !MergeDownUnderBlendMode(both, both.layers[2].id)
				&& !MergeDownUnderBlendMode(modeClear, modeClear.layers[2].id)
				&& !MergeDownUnderBlendMode(mode, mode.layers[1].id)
				&& !MergeDownUnderBlendMode(mode, mode.layers[0].id)
				&& !MergeDownUnderBlendMode(mode, 999),
			u"merge down: nothing is in the way when the picture is kept"_q);
		check(
			MergeDownUnderBlendMode(mode, mode.layers[2].id)
				&& MergeDownUnderBlendMode(
					modeHidden,
					modeHidden.layers[2].id),
			u"merge down: a lower layer with a blend mode is in the way"_q);

		// What is refused: the upper layer would get the mode of the
		// lower one. With the normal mode the same merge changes nothing.
		const auto solid = [](QColor color) {
			auto result = QImage(8, 6, QImage::Format_ARGB32_Premultiplied);
			result.fill(color);
			return result;
		};
		const auto flat = [](const Document &document) {
			auto compositor = Compositor();
			return compositor.render(document, {
				.scale = 1.,
				.global = false,
				.cache = false,
			});
		};
		auto tinted = Document();
		tinted.size = QSize(8, 6);
		AddLayer(tinted, MakeImageLayer(solid(QColor(40, 80, 120)), u"base"_q));
		auto screen = MakeImageLayer(solid(QColor(100, 100, 100)), u"lower"_q);
		screen.blend = BlendMode::Screen;
		AddLayer(tinted, std::move(screen));
		const auto cover = AddLayer(
			tinted,
			MakeImageLayer(solid(QColor(200, 50, 50)), u"upper"_q));
		const auto tintedBefore = flat(tinted);
		const auto tintedMerged = MergedDown(tinted, cover);
		const auto tintedAfter = tintedMerged ? flat(*tintedMerged) : QImage();
		check(
			MergeDownUnderBlendMode(tinted, cover)
				&& !tintedBefore.isNull()
				&& (tintedAfter.size() == tintedBefore.size())
				&& (MaxDifference(tintedAfter, tintedBefore) > 32),
			u"merge down: under a blend mode the picture would change"_q);
		auto plain = tinted;
		plain.layers[1].blend = BlendMode::Normal;
		const auto plainBefore = flat(plain);
		const auto plainMerged = MergedDown(plain, cover);
		check(
			!MergeDownUnderBlendMode(plain, cover)
				&& plainMerged
				&& !plainBefore.isNull()
				&& (MaxDifference(flat(*plainMerged), plainBefore) <= 1),
			u"merge down: under the normal mode the same picture stays"_q);
	}

	// Undo / redo.
	{
		const auto document = makeDocument();
		auto history = History(document);
		auto second = document;
		second.layers[1].opacity = 0.2;
		auto third = second;
		third.layers[0].content = MakeImageContent(FxTestImage(320, 240, true));
		check(
			!history.push(document)
				&& history.push(second)
				&& history.push(third)
				&& history.count() == 3
				&& history.current() == third
				&& !history.canRedo(),
			u"history keeps the steps"_q);
		const auto bytes = history.memoryUsage();
		const auto expected = qint64(320 * 240 * 4) * 2
			+ qint64(120 * 90 * 4)
			+ qint64(100 * 100 * 4)
			+ qint64(100 * 100);
		check(
			bytes == expected,
			u"snapshots share the pixels (%1 bytes for 3 steps)"_q.arg(bytes));
		check(
			history.undo()
				&& history.current() == second
				&& history.undo()
				&& history.current() == document
				&& !history.undo()
				&& history.redo()
				&& history.current() == second,
			u"undo and redo"_q);
		auto branch = second;
		branch.global.exposure = 40;
		check(
			history.push(branch)
				&& history.count() == 3
				&& !history.canRedo()
				&& history.undo()
				&& history.current() == second,
			u"a new step drops the redo steps"_q);
		history.setLimits(2, 1LL << 40);
		check(
			history.count() == 2 && history.current() == second,
			u"the oldest steps are dropped at the limit"_q);
		auto heavy = History(document);
		heavy.setLimits(100, 400 * 1024);
		for (auto i = 0; i != 6; ++i) {
			auto next = heavy.current();
			next.layers[0].content = MakeImageContent(
				FxTestImage(320, 240, (i % 2) == 0));
			heavy.push(std::move(next));
		}
		check(
			heavy.count() < 7 && heavy.memoryUsage() <= 400 * 1024 + 400000,
			u"the memory limit drops old steps (%1 left, %2 bytes)"_q.arg(
				heavy.count()).arg(heavy.memoryUsage()));

		// The document alone is about 400 KB here.
		auto large = History(document);
		large.setLimits(100, 64 * 1024);
		auto step = document;
		for (auto i = 0; i != 3; ++i) {
			step.layers[1].opacity = 0.6 - 0.1 * i;
			large.push(step);
		}
		check(
			large.count() == 4
				&& large.canUndo()
				&& large.memoryUsage() > 64 * 1024,
			u"a document over the memory limit keeps its undo (%1 steps)"_q.arg(
				large.count()));

		auto pair = History(document);
		pair.setLimits(100, 1024);
		auto replaced = document;
		replaced.layers[0].content = MakeImageContent(
			FxTestImage(320, 240, true));
		auto again = replaced;
		again.layers[0].content = MakeImageContent(FxTestImage(320, 240));
		check(
			pair.push(replaced)
				&& pair.push(again)
				&& pair.count() == 2
				&& pair.undo()
				&& pair.current() == replaced,
			u"one step back is kept whatever it weighs"_q);

		const auto original = MakeImageContent(FxTestImage(64, 48));
		const auto cutout = MakeImageContent(FxTestImage(64, 48, true), original);
		auto whole = Document();
		whole.size = QSize(64, 48);
		AddLayer(whole, MakeLayer(original, u"photo"_q));
		auto cut = whole;
		cut.layers[0].content = cutout;
		auto shared = History(whole);
		shared.push(cut);
		check(
			shared.memoryUsage() == qint64(64 * 48 * 4) * 2,
			u"a cutout and its original are counted once (%1 bytes)"_q.arg(
				shared.memoryUsage()));
	}

	// Timings, only reported.
	{
		const auto large = FxTestImage(4000, 3000);
		auto document = DocumentFromImage(large, EditState());
		auto over = MakeImageLayer(FxTestImage(2000, 1500, true), u"over"_q);
		over.transform = QTransform::fromTranslate(900., 600.);
		over.blend = BlendMode::Overlay;
		over.opacity = 0.8;
		const auto id = AddLayer(document, std::move(over));
		auto compositor = Compositor();
		auto timer = QElapsedTimer();
		timer.start();
		const auto first = compositor.render(document, {
			.scale = 0.4,
			.preview = true,
			.active = id,
		});
		const auto cold = timer.nsecsElapsed() / 1e6;
		document.find(id)->opacity = 0.5;
		timer.start();
		const auto second = compositor.render(document, {
			.scale = 0.4,
			.preview = true,
			.active = id,
		});
		const auto warm = timer.nsecsElapsed() / 1e6;
		timer.start();
		const auto full = RenderDocument(document);
		const auto exported = timer.nsecsElapsed() / 1e6;
		check(
			!first.isNull() && !second.isNull() && !full.isNull(),
			u"12 MP with a layer renders"_q);
		info(u"12 MP + overlay layer: preview %1 ms, after an opacity "
			"change %2 ms, export %3 ms"_q.arg(
				QString::number(cold, 'f', 1),
				QString::number(warm, 'f', 1),
				QString::number(exported, 'f', 1)));
	}

	ok = RunRegisteredSelfTests(SelfTestSuite::Doc, log) && ok;
	return ok;
}

namespace {

// The "canvas" sub-test of OBLIVION_SELFTEST=photo_doc: the size limits,
// extending to an aspect ratio and moving a document to another canvas.
bool RunCanvasSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};

	check(
		ValidCanvasSize(QSize(5, 5)) == QSize(kCanvasMinSide, kCanvasMinSide)
			&& ValidCanvasSize(QSize(4032, 3024)) == QSize(4032, 3024)
			&& (ValidCanvasSize(QSize(100000, 100))
				== QSize(kCanvasMaxSide, 100)),
		u"canvas: sides are kept inside the limits"_q);
	{
		const auto large = ValidCanvasSize(QSize(16000, 12000));
		check(
			(qint64(large.width()) * large.height() <= kCanvasMaxPixels)
				&& (large.width() > 9000)
				&& (std::abs(large.width() * 3 - large.height() * 4) <= 8),
			u"canvas: a huge area is scaled down in proportion (%1x%2)"_q
				.arg(large.width())
				.arg(large.height()));
	}
	check(
		CanvasSizeForAspect(QSize(4000, 3000), 1, 1) == QSize(4000, 4000)
			&& CanvasSizeForAspect(QSize(3000, 4000), 1, 1) == QSize(4000, 4000)
			&& CanvasSizeForAspect(QSize(1600, 900), 4, 3) == QSize(1600, 1200)
			&& CanvasSizeForAspect(QSize(1600, 900), 9, 16) == QSize(1600, 2844)
			&& CanvasSizeForAspect(QSize(900, 1600), 16, 9) == QSize(2844, 1600)
			&& CanvasSizeForAspect(QSize(1600, 900), 16, 9) == QSize(1600, 900)
			&& CanvasSizeForAspect(QSize(1600, 900), 0, 9) == QSize(1600, 900),
		u"canvas: the smallest canvas of an aspect ratio"_q);
	{
		auto inside = true;
		const auto ratios = std::array<QSize, 7>{ {
			{ 1, 1 },
			{ 4, 3 },
			{ 3, 4 },
			{ 3, 2 },
			{ 2, 3 },
			{ 16, 9 },
			{ 9, 16 },
		} };
		for (const auto &current : { QSize(640, 480), QSize(333, 1001), QSize(4032, 3024) }) {
			for (const auto &ratio : ratios) {
				const auto size = CanvasSizeForAspect(
					current,
					ratio.width(),
					ratio.height());
				const auto wanted = ratio.width() / double(ratio.height());
				const auto got = size.width() / double(size.height());
				if (size.width() < current.width()
					|| size.height() < current.height()
					|| std::abs(got - wanted) > 0.01
					|| (size.width() != current.width()
						&& size.height() != current.height())) {
					inside = false;
				}
			}
		}
		check(inside, u"canvas: extending never crops and keeps one side"_q);
	}

	auto document = DocumentFromImage(
		FxTestImage(240, 160),
		EditState(),
		u"photo"_q);
	auto second = MakeImageLayer(FxTestImage(80, 60, true), u"second"_q);
	second.transform = ComposeTransform({
		.center = QPointF(150., 70.),
		.scaleX = 1.,
		.scaleY = 1.,
		.rotation = 20.,
	}, QSizeF(80., 60.));
	second.opacity = 0.8;
	const auto secondId = AddLayer(document, std::move(second));
	document.global.crop = QRectF(0.1, 0.1, 0.5, 0.5);
	document.global.quarterTurns = 1;
	document.global.contrast = 10;
	const auto flat = [](const Document &document) {
		auto compositor = Compositor();
		return compositor.render(document, {
			.scale = 1.,
			.global = false,
			.cache = false,
		});
	};
	const auto sameQuad = [](
			const QPolygonF &a,
			const QPolygonF &b,
			QPointF shift) {
		if (a.size() != b.size()) {
			return false;
		}
		for (auto i = 0; i != int(a.size()); ++i) {
			const auto delta = a[i] - (b[i] + shift);
			if (std::abs(delta.x()) > 1e-6 || std::abs(delta.y()) > 1e-6) {
				return false;
			}
		}
		return true;
	};
	const auto before = flat(document);

	const auto larger = CanvasResized(document, QSize(400, 300));
	check(
		larger.size == QSize(400, 300)
			&& larger.layers.size() == 2
			&& larger.layers[0].id == document.layers[0].id
			&& larger.layers[1].id == secondId
			&& larger.layers[1].content == document.layers[1].content
			&& larger.nextId == document.nextId,
		u"canvas: a larger canvas keeps the layers and their ids"_q);
	check(
		larger.global.crop == QRectF(0., 0., 1., 1.)
			&& larger.global.quarterTurns == 1
			&& larger.global.contrast == 10,
		u"canvas: the crop frame is reset, the rest of the edit stays"_q);
	check(
		sameQuad(
			LayerQuad(larger.layers[0]),
			LayerQuad(document.layers[0]),
			QPointF(80., 70.))
			&& sameQuad(
				LayerQuad(larger.layers[1]),
				LayerQuad(document.layers[1]),
				QPointF(80., 70.)),
		u"canvas: the layers move with the old canvas to the center"_q);
	{
		const auto after = flat(larger);
		const auto same = !before.isNull()
			&& (after.size() == QSize(400, 300))
			&& (FxImageDifference(before, after.copy(80, 70, 240, 160)) < 0.05)
			&& (qAlpha(after.pixel(10, 10)) == 0)
			&& (qAlpha(after.pixel(390, 290)) == 0);
		check(same, u"canvas: the picture is the same, the rest is clear"_q);
	}
	{
		const auto corner = CanvasResized(
			document,
			QSize(400, 300),
			QPointF(0., 0.));
		const auto odd = CanvasResized(document, QSize(241, 163));
		const auto shift = LayerQuad(odd.layers[0])[0];
		check(
			sameQuad(
				LayerQuad(corner.layers[1]),
				LayerQuad(document.layers[1]),
				QPointF())
				&& (shift.x() == std::round(shift.x()))
				&& (shift.y() == std::round(shift.y())),
			u"canvas: an anchor in the corner, a shift by whole pixels"_q);
	}
	{
		const auto smaller = CanvasResized(document, QSize(120, 80));
		const auto after = flat(smaller);
		check(
			smaller.size == QSize(120, 80)
				&& !after.isNull()
				&& (after.size() == QSize(120, 80))
				&& (FxImageDifference(
					before.copy(60, 40, 120, 80),
					after) < 0.05),
			u"canvas: a smaller canvas shows the middle of the picture"_q);
		const auto back = CanvasResized(smaller, QSize(240, 160));
		check(
			sameQuad(
				LayerQuad(back.layers[1]),
				LayerQuad(document.layers[1]),
				QPointF())
				&& (FxImageDifference(before, flat(back)) < 0.05),
			u"canvas: nothing is lost when the size comes back"_q);
	}
	check(
		CanvasResized(document, document.size) == document
			&& CanvasResized(Document(), QSize(100, 100)).empty()
			&& CanvasResized(document, QSize(1, 1)).size
				== QSize(kCanvasMinSide, kCanvasMinSide),
		u"canvas: the same size changes nothing, a bad one is fixed"_q);
	{
		auto history = History(document);
		const auto pushed = history.push(larger);
		const auto undone = history.undo() && (history.current() == document);
		check(
			pushed && undone && history.redo() && (history.current() == larger),
			u"canvas: a resize is one undo step"_q);
	}
	return ok;
}

const auto CanvasSelfTest = SelfTestRegistrar(
	SelfTestSuite::Doc,
	"canvas",
	&RunCanvasSelfTest);

} // namespace

} // namespace Oblivion::Photo
