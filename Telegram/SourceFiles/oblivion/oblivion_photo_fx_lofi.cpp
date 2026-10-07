/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_fx_lofi.h"

#include "base/invoke_queued.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_photo_panels.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Oblivion::Photo {
namespace {

constexpr auto kPi = 3.14159265358979323846;
constexpr auto kTwoPi = float(2. * kPi);
constexpr auto kMaxStampLength = 40;
constexpr auto kPreviewJpegPixels = int64(3'000'000);
constexpr auto kMaxJpegPixels = int64(60'000'000);
constexpr auto kMinWorkSide = 8;
constexpr auto kReferenceSensor = 640.f;
constexpr auto kTextEditorType = "lofi.text";
constexpr auto kDefaultStampText = "'05 08 14";
constexpr auto kGlyphColumns = 5;
constexpr auto kGlyphRows = 7;
constexpr auto kSegmentWidth = 0.56f;
constexpr auto kSegmentThickness = 0.13f;
constexpr auto kBicubicMaxPixels = int64(48'000'000);
constexpr auto kMaxStampSide = 1'000'000.;
constexpr auto kMaxStampMaskPixels = int64(64'000'000);

[[nodiscard]] bool LofiCancelled(const std::atomic<bool> *cancel) {
	return cancel && cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] QSize LofiFullSize(const QImage &image, const FxContext &context) {
	if (!context.fullSize.isEmpty()) {
		return context.fullSize;
	}
	const auto scale = (context.scale > 0.) ? context.scale : 1.;
	return QSize(
		std::max(int(std::lround(image.width() / scale)), 1),
		std::max(int(std::lround(image.height() / scale)), 1));
}

[[nodiscard]] float LofiUnit(const FxContext &context) {
	return float(std::clamp(context.scale, 0.001, 8.));
}

[[nodiscard]] uint32 LofiSeed(
		const FxParams &params,
		const FxContext &context,
		uint32 salt) {
	return uint32(FxHashCombine(
		FxHashCombine(context.seed, uint64(params.integer("seed"))),
		salt));
}

[[nodiscard]] float Percent(const FxParams &params, QByteArrayView id) {
	return float(params.number(id) / 100.);
}

[[nodiscard]] QSize SizeWithLongSide(QSize full, int side) {
	const auto longSide = std::max(full.width(), full.height());
	if (side >= longSide || longSide <= 0) {
		return full;
	}
	const auto scale = std::max(side, kMinWorkSide) / double(longSide);
	return QSize(
		std::max(int(std::lround(full.width() * scale)), 1),
		std::max(int(std::lround(full.height() * scale)), 1));
}

[[nodiscard]] inline uint32 MixPixels(uint32 a, uint32 b, uint32 weight) {
	const auto inverse = 256U - weight;
	const auto rb = (((a & 0x00FF00FFU) * inverse
		+ (b & 0x00FF00FFU) * weight) >> 8) & 0x00FF00FFU;
	const auto ag = (((a >> 8) & 0x00FF00FFU) * inverse
		+ ((b >> 8) & 0x00FF00FFU) * weight) & 0xFF00FF00U;
	return rb | ag;
}

[[nodiscard]] inline uint32 DimPixel(uint32 pixel, uint32 gain) {
	const auto rb = (((pixel & 0x00FF00FFU) * gain) >> 8) & 0x00FF00FFU;
	const auto g = (((pixel & 0x0000FF00U) * gain) >> 8) & 0x0000FF00U;
	return (pixel & 0xFF000000U) | rb | g;
}

[[nodiscard]] inline float Screen(float base, float light) {
	return 1.f - (1.f - base) * (1.f - FxClamp01(light));
}

//
// Resampling.
//

enum class Resample : uchar {
	Smooth,
	Pixels,
	Sharp,
};

// Catmull-Rom upscaling: sharper than the bilinear one, with the light
// ringing around edges that enlarged small photos have.
[[nodiscard]] QImage UpscaleBicubic(const QImage &source, QSize size) {
	const auto sw = source.width();
	const auto sh = source.height();
	const auto dw = size.width();
	const auto dh = size.height();
	if (source.isNull() || int64(dw) * sh > kBicubicMaxPixels) {
		return QImage();
	}
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	struct Tap {
		std::array<int, 4> index = {};
		std::array<float, 4> weight = {};
	};
	const auto build = [](int from, int to) {
		auto taps = std::vector<Tap>(to);
		const auto ratio = float(from) / to;
		for (auto i = 0; i != to; ++i) {
			const auto center = (i + 0.5f) * ratio - 0.5f;
			const auto base = int(std::floor(center));
			const auto t = center - base;
			auto &tap = taps[i];
			tap.weight[0] = ((-t + 2.f) * t - 1.f) * t * 0.5f;
			tap.weight[1] = ((3.f * t - 5.f) * t * t + 2.f) * 0.5f;
			tap.weight[2] = ((-3.f * t + 4.f) * t + 1.f) * t * 0.5f;
			tap.weight[3] = (t - 1.f) * t * t * 0.5f;
			for (auto k = 0; k != 4; ++k) {
				tap.index[k] = std::clamp(base - 1 + k, 0, from - 1);
			}
		}
		return taps;
	};
	const auto tx = build(sw, dw);
	const auto ty = build(sh, dh);
	auto middle = std::vector<short>(size_t(dw) * sh * 4);
	FxParallelRows(dw, sh, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(source, y);
			const auto out = middle.data() + size_t(y) * dw * 4;
			for (auto x = 0; x != dw; ++x) {
				const auto &tap = tx[x];
				auto sum = std::array<float, 4>{};
				for (auto k = 0; k != 4; ++k) {
					const auto p = line[tap.index[k]];
					const auto weight = tap.weight[k];
					sum[0] += int(p & 0xFFU) * weight;
					sum[1] += int((p >> 8) & 0xFFU) * weight;
					sum[2] += int((p >> 16) & 0xFFU) * weight;
					sum[3] += int(p >> 24) * weight;
				}
				for (auto c = 0; c != 4; ++c) {
					out[x * 4 + c] = short(std::lround(sum[c]));
				}
			}
		}
	});
	FxParallelRows(dw, dh, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto &tap = ty[y];
			const auto line = FxRow(result, y);
			auto rows = std::array<const short*, 4>{};
			for (auto k = 0; k != 4; ++k) {
				rows[k] = middle.data() + size_t(tap.index[k]) * dw * 4;
			}
			for (auto x = 0; x != dw; ++x) {
				auto value = std::array<int, 4>{};
				for (auto c = 0; c != 4; ++c) {
					auto sum = 0.f;
					for (auto k = 0; k != 4; ++k) {
						sum += rows[k][x * 4 + c] * tap.weight[k];
					}
					value[c] = std::clamp(int(std::lround(sum)), 0, 255);
				}
				const auto a = value[3];
				line[x] = (uint32(a) << 24)
					| (uint32(std::min(value[2], a)) << 16)
					| (uint32(std::min(value[1], a)) << 8)
					| uint32(std::min(value[0], a));
			}
		}
	});
	return result;
}

[[nodiscard]] QImage LofiResized(
		const QImage &image,
		QSize size,
		Resample method) {
	if (image.isNull() || size.isEmpty()) {
		return QImage();
	} else if (image.size() == size) {
		return image;
	}
	const auto grows = (size.width() >= image.width())
		&& (size.height() >= image.height());
	auto result = (method == Resample::Sharp && grows)
		? UpscaleBicubic(image, size)
		: QImage();
	if (result.isNull()) {
		result = image.scaled(
			size,
			Qt::IgnoreAspectRatio,
			((method == Resample::Pixels)
				? Qt::FastTransformation
				: Qt::SmoothTransformation));
	}
	if (!result.isNull()
		&& result.format() != QImage::Format_ARGB32_Premultiplied) {
		result = result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	result.setDevicePixelRatio(1.);
	return result;
}

//
// JPEG.
//

[[nodiscard]] bool JpegPass(QImage &opaque, int quality) {
	auto bytes = QByteArray();
	{
		auto buffer = QBuffer(&bytes);
		if (!buffer.open(QIODevice::WriteOnly)) {
			return false;
		}
		auto writer = QImageWriter(&buffer, "jpeg");
		writer.setQuality(std::clamp(quality, 1, 100));
		if (!writer.write(opaque)) {
			return false;
		}
	}
	auto decoded = QImage();
	if (!decoded.loadFromData(bytes, "jpeg")
		|| decoded.size() != opaque.size()) {
		return false;
	}
	if (decoded.format() != QImage::Format_RGB32) {
		decoded = decoded.convertToFormat(QImage::Format_RGB32);
	}
	if (decoded.isNull()) {
		return false;
	}
	opaque = std::move(decoded);
	return true;
}

//
// Sensor noise.
//

class ValueNoise final {
public:
	ValueNoise(float cell, uint32 seed)
	: _cell(std::max(cell, 0.01f))
	, _inverse(1.f / std::max(cell, 0.5f))
	, _seed(seed) {
	}

	// Cells smaller than a pixel (a downscaled preview of a large photo)
	// average out in the export, so the preview gets a weaker noise
	// instead of a coarser one.
	[[nodiscard]] float strength() const {
		return std::clamp(_cell, 0.35f, 1.f);
	}

	[[nodiscard]] float at(int x, int y) const {
		if (_cell <= 1.05f) {
			return FxNoise(uint32(x), uint32(y), _seed);
		}
		const auto fx = x * _inverse;
		const auto fy = y * _inverse;
		const auto ix = int(fx);
		const auto iy = int(fy);
		auto tx = fx - ix;
		auto ty = fy - iy;
		tx = tx * tx * (3.f - 2.f * tx);
		ty = ty * ty * (3.f - 2.f * ty);
		const auto a = FxNoise(uint32(ix), uint32(iy), _seed);
		const auto b = FxNoise(uint32(ix + 1), uint32(iy), _seed);
		const auto c = FxNoise(uint32(ix), uint32(iy + 1), _seed);
		const auto d = FxNoise(uint32(ix + 1), uint32(iy + 1), _seed);
		return FxMix(FxMix(a, b, tx), FxMix(c, d, tx), ty) * 1.35f;
	}

private:
	float _cell = 1.f;
	float _inverse = 1.f;
	uint32 _seed = 0;

};

struct NoiseSpec {
	float amount = 0.f;
	float color = 0.f;
	float cell = 1.f;
	float hot = 0.f;
	float banding = 0.f;
	uint32 seed = 0;
	// The noise is laid over the picture in the "overlay" mode instead of
	// being added: black and white stay clean, the middle tones get it.
	bool overlay = false;
	// How much of the noise leaves the lights (one: none is left there).
	float shadows = 0.f;
};

[[nodiscard]] inline float OverlayBlend(float base, float blend) {
	return (base < 0.5f)
		? (2.f * base * blend)
		: (1.f - 2.f * (1.f - base) * (1.f - blend));
}

void StageNoise(QImage &image, const NoiseSpec &spec, float unit) {
	const auto cell = spec.cell * unit;
	const auto luma = ValueNoise(cell, spec.seed);
	const auto chromaRed = ValueNoise(cell * 2.6f, spec.seed ^ 0x51ED270BU);
	const auto chromaBlue = ValueNoise(cell * 2.6f, spec.seed ^ 0xA3C59AC3U);
	const auto lumaAmount = spec.amount * 0.26f * luma.strength();
	const auto chromaAmount = spec.color * 0.17f * chromaRed.strength();
	const auto bandAmount = spec.banding * 0.08f;
	const auto hotCell = std::max(cell, 1.f);
	const auto hotInverse = 1.f / hotCell;
	const auto hotGain = (cell < 1.f) ? std::max(cell * cell, 0.2f) : 1.f;
	const auto hotLimit = uint32(std::clamp(
		double(spec.hot) * 0.004 * 4294967295.,
		0.,
		4294967295.));
	const auto hotSeed = spec.seed ^ 0x40B7C1E5U;
	const auto bandSeed = spec.seed ^ 0x0BADB17EU;
	const auto blended = spec.overlay || (spec.shadows > 0.f);
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		if (blended) {
			// The same noise, weighted by the brightness of the pixel and
			// (in the "overlay" mode) by its distance from black and white.
			const auto level = FxLuma(c.r, c.g, c.b);
			auto red = 0.f;
			auto green = 0.f;
			auto blue = 0.f;
			if (lumaAmount > 0.f) {
				const auto n = luma.at(x, y)
					* lumaAmount
					* (1.2f - 0.65f * level);
				red += n;
				green += n;
				blue += n;
			}
			if (chromaAmount > 0.f) {
				const auto r = chromaRed.at(x, y) * chromaAmount;
				const auto b = chromaBlue.at(x, y) * chromaAmount;
				red += r;
				blue += b;
				green -= (r * 0.299f + b * 0.114f) / 0.587f;
			}
			const auto k = FxMix(1.f, FxClamp01(1.f - level), spec.shadows);
			if (spec.overlay) {
				c.r = OverlayBlend(c.r, FxClamp01(0.5f + red * k));
				c.g = OverlayBlend(c.g, FxClamp01(0.5f + green * k));
				c.b = OverlayBlend(c.b, FxClamp01(0.5f + blue * k));
			} else {
				c.r += red * k;
				c.g += green * k;
				c.b += blue * k;
			}
		} else if (lumaAmount > 0.f) {
			const auto level = FxLuma(c.r, c.g, c.b);
			const auto n = luma.at(x, y) * lumaAmount * (1.2f - 0.65f * level);
			c.r += n;
			c.g += n;
			c.b += n;
		}
		if (!blended && chromaAmount > 0.f) {
			const auto red = chromaRed.at(x, y) * chromaAmount;
			const auto blue = chromaBlue.at(x, y) * chromaAmount;
			c.r += red;
			c.b += blue;
			c.g -= (red * 0.299f + blue * 0.114f) / 0.587f;
		}
		if (bandAmount > 0.f) {
			const auto n = (FxNoise(uint32(x * hotInverse), 0x7FU, bandSeed)
				+ 0.4f * FxNoise(0x7FU, uint32(y * hotInverse), bandSeed))
				* bandAmount;
			c.r += n;
			c.g += n;
			c.b += n;
		}
		if (hotLimit) {
			const auto hx = uint32(x * hotInverse);
			const auto hy = uint32(y * hotInverse);
			if (FxHash32(hx, hy, hotSeed) < hotLimit) {
				const auto kind = FxHash32(hy, hx, hotSeed ^ 0x9E3779B9U);
				const auto level = (0.65f
					+ 0.35f * ((kind >> 8) & 0xFFU) / 255.f) * hotGain;
				const auto which = (kind & 3U);
				if (which == 0 || which == 3) {
					c.r += (1.f - c.r) * level;
				}
				if (which == 1 || which == 3) {
					c.g += (1.f - c.g) * level;
				}
				if (which == 2 || which == 3) {
					c.b += (1.f - c.b) * level;
				}
			}
		}
	});
}

//
// Optics.
//

// The highlights of a picture, blurred on a reduced copy: what bloom,
// halation and purple fringing are made of.
struct Glow {
	QImage image;
	float scaleX = 1.f;
	float scaleY = 1.f;
	float sigma = 1.f; // In the pixels of the reduced copy.
};

[[nodiscard]] Glow MakeGlow(const QImage &image, float threshold, float sigma) {
	const auto reduce = std::clamp(3.f / std::max(sigma, 0.01f), 0.04f, 1.f);
	const auto size = QSize(
		std::max(int(std::lround(image.width() * reduce)), 1),
		std::max(int(std::lround(image.height() * reduce)), 1));
	auto reduced = (size == image.size()) ? image.copy() : FxResized(image, size);
	if (!FxPrepare(reduced)) {
		return {};
	}
	const auto edge = std::clamp(threshold, 0.f, 0.97f);
	FxForEachColor(reduced, [&](FxRgba &c, int, int) {
		const auto level = std::max({ c.r, c.g, c.b });
		const auto k = FxSmoothStep(edge, 1.f, level);
		c.r *= k;
		c.g *= k;
		c.b *= k;
	});
	const auto reducedSigma = std::max(sigma * reduce, 0.3f);
	FxGaussianBlur(reduced, reducedSigma);
	return {
		.image = reduced,
		.scaleX = reduced.width() / float(image.width()),
		.scaleY = reduced.height() / float(image.height()),
		.sigma = reducedSigma,
	};
}

[[nodiscard]] inline uint32 GlowAt(const Glow &glow, int x, int y) {
	return FxSample(
		glow.image,
		(x + 0.5f) * glow.scaleX - 0.5f,
		(y + 0.5f) * glow.scaleY - 0.5f);
}

struct AberrationSpec {
	float amount = 0.f; // Pixels at the farthest corner.
	QPointF center = QPointF(0.5, 0.5);
	float fringe = 0.f;
};

void StageAberration(
		QImage &image,
		const AberrationSpec &spec,
		float unit) {
	const auto w = image.width();
	const auto h = image.height();
	const auto cx = float(spec.center.x() * w - 0.5);
	const auto cy = float(spec.center.y() * h - 0.5);
	const auto radius = std::max(
		float(std::hypot(
			std::max(cx, w - cx),
			std::max(cy, h - cy))),
		1.f);
	const auto shift = spec.amount * unit;
	if (shift >= 0.05f) {
		const auto k = shift / radius;
		const auto source = image.copy();
		if (source.isNull()) {
			return;
		}
		FxParallelRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto line = FxRow(image, y);
				const auto dy = (y - cy) * k;
				for (auto x = 0; x != w; ++x) {
					const auto dx = (x - cx) * k;
					const auto red = FxSample(source, x - dx, y - dy);
					const auto blue = FxSample(source, x + dx, y + dy);
					const auto green = line[x];
					const auto a = std::max({
						red >> 24,
						green >> 24,
						blue >> 24,
					});
					line[x] = (a << 24)
						| (red & 0x00FF0000U)
						| (green & 0x0000FF00U)
						| (blue & 0x000000FFU);
				}
			}
		});
	}
	if (spec.fringe > 0.f) {
		const auto glow = MakeGlow(image, 0.72f, std::max(3.5f * unit, 0.6f));
		if (glow.image.isNull()) {
			return;
		}
		const auto strength = spec.fringe * 1.6f;
		FxForEachColor(image, [&](FxRgba &c, int x, int y) {
			const auto g = GlowAt(glow, x, y);
			const auto around = std::max({
				(g >> 16) & 0xFFU,
				(g >> 8) & 0xFFU,
				g & 0xFFU,
			}) / 255.f;
			const auto own = std::max({ c.r, c.g, c.b });
			const auto purple = FxClamp01(around - own * 0.8f) * strength;
			c.r = Screen(c.r, purple * 0.6f);
			c.g = Screen(c.g, purple * 0.08f);
			c.b = Screen(c.b, purple);
		});
	}
}

struct BloomSpec {
	float amount = 0.f;
	float threshold = 0.7f;
	float radius = 20.f;
	float halation = 0.f;
};

void StageBloom(QImage &image, const BloomSpec &spec, float unit) {
	const auto sigma = std::max(spec.radius * unit * 0.5f, 0.4f);
	const auto glow = MakeGlow(image, spec.threshold, sigma);
	if (glow.image.isNull()) {
		return;
	}
	auto halo = Glow();
	if (spec.halation > 0.f) {
		halo = glow;
		halo.image = glow.image.copy();
		FxGaussianBlur(halo.image, glow.sigma * 2.2f);
	}
	const auto light = spec.amount * 1.25f;
	const auto red = spec.halation * 1.3f;
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		if (light > 0.f) {
			const auto g = GlowAt(glow, x, y);
			c.r = Screen(c.r, ((g >> 16) & 0xFFU) / 255.f * light);
			c.g = Screen(c.g, ((g >> 8) & 0xFFU) / 255.f * light);
			c.b = Screen(c.b, (g & 0xFFU) / 255.f * light);
		}
		if (red > 0.f && !halo.image.isNull()) {
			const auto g = GlowAt(halo, x, y);
			const auto level = std::max({
				(g >> 16) & 0xFFU,
				(g >> 8) & 0xFFU,
				g & 0xFFU,
			}) / 255.f * red;
			c.r = Screen(c.r, level);
			c.g = Screen(c.g, level * 0.3f);
			c.b = Screen(c.b, level * 0.1f);
		}
	});
}

struct FlashSpec {
	float amount = 0.f;
	QPointF center = QPointF(0.5, 0.45);
	float radius = 0.7f;
	float falloff = 0.6f;
};

void StageFlash(QImage &image, const FlashSpec &spec) {
	const auto w = image.width();
	const auto h = image.height();
	const auto cx = float(spec.center.x() * w);
	const auto cy = float(spec.center.y() * h);
	const auto half = std::max(float(std::hypot(w, h)) / 2.f, 1.f);
	const auto radius = std::max(spec.radius, 0.05f);
	const auto amount = spec.amount;
	const auto contrast = 1.f + amount * 0.22f;
	const auto lift = amount * 0.02f;
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		const auto d = float(std::hypot(x + 0.5f - cx, y + 0.5f - cy)) / half;
		const auto hot = std::exp(-(d * d) / (radius * radius));
		const auto dark = 1.f - amount * spec.falloff * 0.8f
			* FxSmoothStep(radius * 0.55f, radius * 0.55f + 0.9f, d);
		const auto gain = (1.f + amount * 0.8f * hot) * dark;
		c.r = (c.r * gain - 0.5f) * contrast + 0.5f + lift;
		c.g = (c.g * gain - 0.5f) * contrast + 0.5f + lift;
		c.b = (c.b * gain - 0.5f) * contrast + 0.5f + lift;
		c.r *= 1.f - 0.05f * amount;
		c.b *= 1.f + 0.06f * amount;
		const auto over = FxSmoothStep(
			0.95f,
			1.5f,
			FxLuma(c.r, c.g, c.b));
		c.r = FxMix(c.r, 1.f, over);
		c.g = FxMix(c.g, 1.f, over);
		c.b = FxMix(c.b, 1.f, over);
	});
}

//
// Processing.
//

struct CastSpec {
	float temperature = 0.f;
	float tint = 0.f;
	float saturation = 0.f;
	float fade = 0.f;
};

void StageCast(QImage &image, const CastSpec &spec) {
	auto red = (1.f + 0.32f * spec.temperature) * (1.f + 0.07f * spec.tint);
	auto green = 1.f - 0.2f * spec.tint;
	auto blue = (1.f - 0.32f * spec.temperature) * (1.f + 0.07f * spec.tint);
	const auto norm = 1.f / std::max(FxLuma(red, green, blue), 0.1f);
	red *= norm;
	green *= norm;
	blue *= norm;
	const auto saturation = 1.f + spec.saturation;
	const auto range = 1.f - 0.26f * spec.fade;
	const auto lift = 0.13f * spec.fade;
	FxForEachColor(image, [&](FxRgba &c, int, int) {
		c.r *= red;
		c.g *= green;
		c.b *= blue;
		const auto level = FxLuma(c.r, c.g, c.b);
		c.r = (level + (c.r - level) * saturation) * range + lift;
		c.g = (level + (c.g - level) * saturation) * range + lift;
		c.b = (level + (c.b - level) * saturation) * range + lift;
	});
}

void StageSharpen(QImage &image, float amount, float radius, float unit) {
	const auto sigma = radius * unit;
	const auto strength = amount * std::clamp(sigma / 0.6f, 0.f, 1.f);
	if (strength <= 0.f) {
		return;
	}
	auto blurred = image.copy();
	if (blurred.isNull()) {
		return;
	}
	FxGaussianBlur(blurred, std::max(sigma, 0.3f));
	const auto w = image.width();
	FxParallelRows(w, image.height(), [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto soft = FxRow(std::as_const(blurred), y);
			for (auto x = 0; x != w; ++x) {
				const auto p = line[x];
				const auto a = int(p >> 24);
				if (!a) {
					continue;
				}
				const auto b = soft[x];
				auto result = uint32(a) << 24;
				for (auto shift = 0; shift != 24; shift += 8) {
					const auto value = int((p >> shift) & 0xFFU);
					const auto base = int((b >> shift) & 0xFFU);
					const auto sharp = value
						+ int(std::lround((value - base) * strength));
					result |= uint32(std::clamp(sharp, 0, a)) << shift;
				}
				line[x] = result;
			}
		}
	});
}

enum class Dither : uchar {
	None,
	Bayer2,
	Bayer4,
	Bayer8,
	FloydSteinberg,
	Atkinson,
};

struct DepthSpec {
	std::array<int, 3> levels = { 8, 8, 8 };
	bool mono = false;
	Dither dither = Dither::None;
	float cell = 1.f;
};

[[nodiscard]] float BayerThreshold(int x, int y, int order) {
	auto value = 0;
	for (auto bit = 0; bit != order; ++bit) {
		const auto xb = (x >> bit) & 1;
		const auto yb = (y >> bit) & 1;
		value = (value << 2) | ((xb ^ yb) << 1) | yb;
	}
	return (value + 0.5f) / float(1 << (2 * order)) - 0.5f;
}

[[nodiscard]] inline float QuantizeLevel(float value, int levels, float bias) {
	const auto steps = float(levels - 1);
	return std::clamp(std::floor(value * steps + 0.5f + bias), 0.f, steps)
		/ steps;
}

// Error diffusion goes pixel by pixel, so it is the one stage that does
// not run in parallel: about a tenth of a second for twelve megapixels.
void DiffuseDepth(QImage &image, const DepthSpec &spec) {
	const auto w = image.width();
	const auto h = image.height();
	const auto atkinson = (spec.dither == Dither::Atkinson);
	const auto channels = spec.mono ? 1 : 3;
	auto rows = std::array<std::vector<float>, 3>();
	for (auto &row : rows) {
		row.assign(size_t(w + 4) * 3, 0.f);
	}
	for (auto y = 0; y != h; ++y) {
		const auto line = FxRow(image, y);
		auto &current = rows[y % 3];
		auto &next = rows[(y + 1) % 3];
		auto &after = rows[(y + 2) % 3];
		for (auto x = 0; x != w; ++x) {
			if (!(line[x] >> 24)) {
				continue;
			}
			auto c = FxUnpack(line[x]);
			auto value = std::array<float, 3>{ c.r, c.g, c.b };
			if (spec.mono) {
				value[0] = FxLuma(c.r, c.g, c.b);
			}
			for (auto channel = 0; channel != channels; ++channel) {
				const auto index = size_t(x + 2) * 3 + channel;
				const auto wanted = value[channel] + current[index];
				const auto levels = spec.mono
					? spec.levels[1]
					: spec.levels[channel];
				const auto stored = QuantizeLevel(wanted, levels, 0.f);
				const auto error = std::clamp(wanted - stored, -1.f, 1.f);
				value[channel] = stored;
				if (atkinson) {
					const auto part = error / 8.f;
					current[index + 3] += part;
					current[index + 6] += part;
					next[index - 3] += part;
					next[index] += part;
					next[index + 3] += part;
					after[index] += part;
				} else {
					current[index + 3] += error * (7.f / 16.f);
					next[index - 3] += error * (3.f / 16.f);
					next[index] += error * (5.f / 16.f);
					next[index + 3] += error * (1.f / 16.f);
				}
			}
			if (spec.mono) {
				value[1] = value[2] = value[0];
			}
			line[x] = FxPack({ value[0], value[1], value[2], c.a });
		}
		std::fill(begin(current), end(current), 0.f);
	}
}

void QuantizeDepth(QImage &image, const DepthSpec &spec) {
	if (spec.dither == Dither::FloydSteinberg
		|| spec.dither == Dither::Atkinson) {
		DiffuseDepth(image, spec);
		return;
	}
	const auto order = (spec.dither == Dither::Bayer2)
		? 1
		: (spec.dither == Dither::Bayer4)
		? 2
		: (spec.dither == Dither::Bayer8)
		? 3
		: 0;
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		const auto bias = order ? BayerThreshold(x, y, order) : 0.f;
		if (spec.mono) {
			const auto level = QuantizeLevel(
				FxLuma(c.r, c.g, c.b),
				spec.levels[1],
				bias);
			c.r = c.g = c.b = level;
		} else {
			c.r = QuantizeLevel(c.r, spec.levels[0], bias);
			c.g = QuantizeLevel(c.g, spec.levels[1], bias);
			c.b = QuantizeLevel(c.b, spec.levels[2], bias);
		}
	});
}

void StageDepth(QImage &image, const DepthSpec &spec, float unit) {
	const auto cell = spec.cell * unit;
	if (cell < 1.5f) {
		QuantizeDepth(image, spec);
		return;
	}
	const auto size = QSize(
		std::max(int(std::lround(image.width() / cell)), 1),
		std::max(int(std::lround(image.height() / cell)), 1));
	auto work = LofiResized(image, size, Resample::Smooth);
	if (!FxPrepare(work)) {
		return;
	}
	QuantizeDepth(work, spec);
	auto result = LofiResized(work, image.size(), Resample::Pixels);
	if (!result.isNull()) {
		image = std::move(result);
	}
}

//
// Screens and tapes.
//

struct ScanSpec {
	float amount = 0.f;
	float pitch = 4.f;
	float comb = 0.f;
};

void StageScanlines(QImage &image, const ScanSpec &spec, float unit) {
	const auto w = image.width();
	const auto h = image.height();
	const auto pitch = std::max(spec.pitch * unit, 0.02f);
	const auto half = pitch / 2.f;
	// The length of the second (dark, odd field) halves of the lines in
	// [0, t): the coverage of a pixel row is its difference, so lines
	// thinner than a pixel become an even shade and not a moire.
	const auto covered = [&](float t) {
		const auto whole = std::floor(t / pitch);
		return whole * half + std::max(t - whole * pitch - half, 0.f);
	};
	const auto shift = int(std::lround(spec.comb * unit));
	FxParallelRows(w, h, [&](int from, int till) {
		auto copy = std::vector<uint32>(shift ? w : 0);
		for (auto y = from; y != till; ++y) {
			const auto coverage = std::clamp(
				covered(y + 1.f) - covered(float(y)),
				0.f,
				1.f);
			if (coverage <= 0.f) {
				continue;
			}
			const auto line = FxRow(image, y);
			if (shift) {
				std::copy_n(line, w, copy.data());
				const auto weight = uint32(std::lround(coverage * 256.f));
				for (auto x = 0; x != w; ++x) {
					const auto moved = copy[std::clamp(x - shift, 0, w - 1)];
					line[x] = (weight >= 256)
						? moved
						: MixPixels(line[x], moved, weight);
				}
			}
			const auto gain = 256U - uint32(std::lround(
				std::clamp(spec.amount * 0.85f * coverage, 0.f, 1.f) * 256.f));
			if (gain < 256U) {
				for (auto x = 0; x != w; ++x) {
					line[x] = DimPixel(line[x], gain);
				}
			}
		}
	});
}

// out[x] is the mean of values[x - radius - shift .. x + radius - shift],
// the edges repeat.
void BoxBlurRow(
		const std::vector<float> &values,
		std::vector<float> &out,
		int count,
		int radius,
		int shift) {
	if (radius <= 0 && !shift) {
		std::copy_n(values.data(), count, out.data());
		return;
	}
	const auto window = float(2 * radius + 1);
	const auto at = [&](int index) {
		return values[std::clamp(index, 0, count - 1)];
	};
	auto sum = 0.f;
	for (auto k = -radius; k <= radius; ++k) {
		sum += at(k - shift);
	}
	for (auto x = 0; x != count; ++x) {
		out[x] = sum / window;
		sum += at(x + radius + 1 - shift) - at(x - radius - shift);
	}
}

struct VhsSpec {
	float bleed = 0.f;
	float soft = 0.f;
	float tracking = 0.f;
	float noise = 0.f;
	float wobble = 0.f;
	uint32 seed = 0;
};

void StageVhs(QImage &image, const VhsSpec &spec) {
	const auto w = image.width();
	const auto h = image.height();
	auto random = FxRandom(uint64(spec.seed) * 0x9E3779B97F4A7C15ULL + 0x1FULL);
	const auto phase1 = random.unit() * kTwoPi;
	const auto phase2 = random.unit() * kTwoPi;
	struct Band {
		float top = 0.f;
		float bottom = 0.f;
		float strength = 0.f;
	};
	auto bands = std::vector<Band>();
	const auto bandCount = (spec.tracking > 0.f)
		? (1 + int(spec.tracking * 2.5f))
		: 0;
	for (auto i = 0; i != bandCount; ++i) {
		const auto top = 0.08f + random.unit() * 0.8f;
		const auto height = 0.012f
			+ random.unit() * 0.04f * (0.5f + spec.tracking);
		bands.push_back({ top, top + height, 0.5f + random.unit() * 0.5f });
	}
	const auto switchTop = 1.f - 0.035f * spec.tracking;
	const auto lumaRadius = int(std::lround(spec.soft * w / 420.f));
	const auto chromaRadius = int(std::lround(spec.bleed * w / 80.f));
	const auto chromaShift = int(std::lround(spec.bleed * w / 260.f));
	const auto cellX = 720.f / w;
	const auto cellY = 540.f / h;
	const auto tone = std::max({ spec.bleed, spec.soft, spec.noise });
	const auto trackSeed = spec.seed ^ 0x7AC81A65U;
	const auto snowSeed = spec.seed ^ 0x51A0F00DU;
	const auto chromaSeed = spec.seed ^ 0xC0105EEDU;
	FxParallelRows(w, h, [&](int from, int till) {
		auto source = std::vector<uint32>(w);
		auto alpha = std::vector<int>(w);
		auto ys = std::vector<float>(w);
		auto us = std::vector<float>(w);
		auto vs = std::vector<float>(w);
		auto buffer = std::vector<float>(w);
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto yn = (y + 0.5f) / h;
			const auto ny = uint32(yn * 540.f);
			auto dx = spec.wobble * w * 0.0035f
				* (std::sin(yn * kTwoPi * 7.f + phase1)
					+ 0.5f * std::sin(yn * kTwoPi * 19.f + phase2));
			auto rough = 0.f;
			auto inBand = false;
			for (const auto &band : bands) {
				if (yn >= band.top && yn < band.bottom) {
					const auto k = band.strength * spec.tracking;
					dx += FxNoise(7U, ny, trackSeed) * w * 0.06f * k;
					rough = std::max(rough, 0.5f * k);
					inBand = true;
				}
			}
			if (spec.tracking > 0.f && yn >= switchTop) {
				const auto t = (yn - switchTop) / (1.f - switchTop);
				dx += t * w * 0.04f * spec.tracking
					+ FxNoise(11U, ny, trackSeed) * w * 0.012f;
				rough = std::max(rough, 0.35f * t);
			}
			const auto shift = int(std::lround(dx));
			std::copy_n(line, w, source.data());
			for (auto x = 0; x != w; ++x) {
				const auto p = source[std::clamp(x - shift, 0, w - 1)];
				alpha[x] = int(p >> 24);
				const auto c = FxUnpack(p);
				const auto level = FxLuma(c.r, c.g, c.b);
				ys[x] = level;
				us[x] = (c.b - level) * 0.5643f;
				vs[x] = (c.r - level) * 0.7133f;
			}
			if (lumaRadius > 0) {
				BoxBlurRow(ys, buffer, w, lumaRadius, 0);
				BoxBlurRow(buffer, ys, w, lumaRadius, 0);
			}
			if (chromaRadius > 0 || chromaShift > 0) {
				BoxBlurRow(us, buffer, w, chromaRadius, chromaShift);
				BoxBlurRow(buffer, us, w, chromaRadius, 0);
				BoxBlurRow(vs, buffer, w, chromaRadius, chromaShift);
				BoxBlurRow(buffer, vs, w, chromaRadius, 0);
			}
			const auto snow = spec.noise * 0.11f + rough * 0.5f;
			const auto stain = spec.noise * 0.035f + rough * 0.08f;
			for (auto x = 0; x != w; ++x) {
				const auto a = alpha[x];
				if (!a) {
					line[x] = 0;
					continue;
				}
				const auto nx = uint32((x + 0.5f) * cellX);
				const auto cy = uint32((y + 0.5f) * cellY);
				auto level = ys[x] + FxNoise(nx, cy, snowSeed) * snow;
				if (inBand
					&& (FxHash32(nx / 40U, ny, trackSeed) % 37U) == 0U) {
					level = FxMix(level, 1.f, 0.75f);
				}
				level = level * (1.f - 0.07f * tone) + 0.035f * tone;
				const auto u = us[x]
					+ FxNoise(nx / 6U, cy, chromaSeed) * stain;
				const auto v = vs[x]
					+ FxNoise(nx / 6U, cy, chromaSeed ^ 0xFFU) * stain;
				const auto r = level + v / 0.7133f;
				const auto b = level + u / 0.5643f;
				const auto g = (level - 0.299f * r - 0.114f * b) / 0.587f;
				line[x] = FxPack({ r, g, b, a / 255.f });
			}
		}
	});
}

enum class PixelShape : uchar {
	Squares,
	Dots,
	Lcd,
};

struct PixelateSpec {
	float size = 16.f;
	PixelShape shape = PixelShape::Squares;
	float amount = 1.f;
};

void StagePixelate(QImage &image, const PixelateSpec &spec, float unit) {
	const auto w = image.width();
	const auto h = image.height();
	const auto cell = spec.size * unit;
	if (cell < 1.25f || spec.amount <= 0.f) {
		return;
	}
	const auto cols = std::max(int(std::ceil(w / cell)), 1);
	const auto rows = std::max(int(std::ceil(h / cell)), 1);
	const auto ox = (w - cols * cell) / 2.f;
	const auto oy = (h - rows * cell) / 2.f;
	auto cellX = std::vector<int>(w);
	auto partX = std::vector<float>(w);
	for (auto x = 0; x != w; ++x) {
		const auto position = (x + 0.5f - ox) / cell;
		cellX[x] = std::clamp(int(std::floor(position)), 0, cols - 1);
		partX[x] = position - cellX[x];
	}
	auto starts = std::vector<int>(rows + 1, h);
	auto filled = 0;
	for (auto y = 0; y != h; ++y) {
		const auto row = std::clamp(
			int(std::floor((y + 0.5f - oy) / cell)),
			0,
			rows - 1);
		while (filled <= row) {
			starts[filled++] = y;
		}
	}
	const auto weight = uint32(std::lround(FxClamp01(spec.amount) * 256.f));
	FxParallel(rows, 1, [&](int from, int till) {
		auto sums = std::vector<uint64>(size_t(cols) * 4);
		auto counts = std::vector<uint32>(cols);
		auto average = std::vector<uint32>(cols);
		for (auto row = from; row != till; ++row) {
			const auto y0 = starts[row];
			const auto y1 = starts[row + 1];
			if (y0 >= y1) {
				continue;
			}
			std::fill(begin(sums), end(sums), uint64(0));
			std::fill(begin(counts), end(counts), uint32(0));
			for (auto y = y0; y != y1; ++y) {
				const auto line = FxRow(std::as_const(image), y);
				for (auto x = 0; x != w; ++x) {
					const auto p = line[x];
					const auto sum = sums.data() + size_t(cellX[x]) * 4;
					sum[0] += (p & 0xFFU);
					sum[1] += ((p >> 8) & 0xFFU);
					sum[2] += ((p >> 16) & 0xFFU);
					sum[3] += (p >> 24);
					++counts[cellX[x]];
				}
			}
			for (auto i = 0; i != cols; ++i) {
				const auto count = uint64(std::max(counts[i], uint32(1)));
				const auto sum = sums.data() + size_t(i) * 4;
				average[i] = (uint32((sum[3] + count / 2) / count) << 24)
					| (uint32((sum[2] + count / 2) / count) << 16)
					| (uint32((sum[1] + count / 2) / count) << 8)
					| uint32((sum[0] + count / 2) / count);
			}
			for (auto y = y0; y != y1; ++y) {
				const auto line = FxRow(image, y);
				const auto partY = (y + 0.5f - oy) / cell - row;
				for (auto x = 0; x != w; ++x) {
					auto result = average[cellX[x]];
					if (spec.shape == PixelShape::Dots) {
						const auto distance = float(std::hypot(
							partX[x] - 0.5f,
							partY - 0.5f)) * cell;
						const auto coverage = FxClamp01(
							0.47f * cell - distance + 0.5f);
						result = DimPixel(
							result,
							uint32(std::lround(coverage * 256.f)));
					} else if (spec.shape == PixelShape::Lcd) {
						const auto third = partX[x] * 3.f;
						const auto stripe = std::clamp(int(third), 0, 2);
						const auto gap = (partY > 0.86f)
							|| (third - stripe > 0.82f);
						const auto a = (result >> 24);
						auto lit = (a << 24);
						for (auto channel = 0; channel != 3; ++channel) {
							const auto shift = 16 - channel * 8;
							const auto value = (result >> shift) & 0xFFU;
							const auto shown = gap
								? (value / 8U)
								: (channel == stripe)
								? std::min(value * 9U / 5U, a)
								: (value / 9U);
							lit |= (shown << shift);
						}
						result = lit;
					}
					line[x] = (weight >= 256U)
						? result
						: MixPixels(line[x], result, weight);
				}
			}
		}
	});
}

void StagePosterize(QImage &image, int levels, float amount) {
	FxForEachColor(image, [&](FxRgba &c, int, int) {
		c.r = FxMix(c.r, QuantizeLevel(c.r, levels, 0.f), amount);
		c.g = FxMix(c.g, QuantizeLevel(c.g, levels, 0.f), amount);
		c.b = FxMix(c.b, QuantizeLevel(c.b, levels, 0.f), amount);
	});
}

struct HalftoneSpec {
	float size = 12.f;
	float angle = 45.f;
	bool color = false;
	QColor ink = QColor(26, 26, 26);
	QColor paper = QColor(244, 239, 228);
	float amount = 1.f;
};

void StageHalftone(QImage &image, const HalftoneSpec &spec, float unit) {
	const auto cell = std::max(spec.size * unit, 1.f);
	auto blurred = image.copy();
	if (blurred.isNull()) {
		return;
	}
	FxGaussianBlur(blurred, std::max(cell * 0.3f, 0.3f));
	struct Raster {
		float cosA = 1.f;
		float sinA = 0.f;
	};
	const auto raster = [](float degrees) {
		const auto radians = float(degrees * kPi / 180.);
		return Raster{ std::cos(radians), std::sin(radians) };
	};
	const auto rasters = spec.color
		? std::array<Raster, 3>{
			raster(spec.angle + 15.f),
			raster(spec.angle + 75.f),
			raster(spec.angle),
		}
		: std::array<Raster, 3>{
			raster(spec.angle),
			raster(spec.angle),
			raster(spec.angle),
		};
	const auto inverse = 1.f / cell;
	// Dots smaller than about two pixels can not be drawn, they fade into
	// the plain tone they print.
	const auto dots = std::clamp(cell - 1.2f, 0.f, 1.f);
	// The ink coverage of the dot of the raster cell that contains the
	// pixel: the dot size comes from the value at the cell center, its
	// area follows the ink amount until the dots touch (pi / 4), then
	// they grow to cover the whole cell.
	const auto coverage = [&](
			const Raster &r,
			float px,
			float py,
			int channel) {
		const auto u = (px * r.cosA + py * r.sinA) * inverse;
		const auto v = (-px * r.sinA + py * r.cosA) * inverse;
		const auto cu = std::floor(u) + 0.5f;
		const auto cv = std::floor(v) + 0.5f;
		const auto cx = (cu * r.cosA - cv * r.sinA) * cell;
		const auto cy = (cu * r.sinA + cv * r.cosA) * cell;
		const auto p = FxSample(blurred, cx - 0.5f, cy - 0.5f);
		const auto c = (p >> 24) ? FxUnpack(p) : FxRgba{ 1.f, 1.f, 1.f, 0.f };
		const auto value = (channel == 0)
			? c.r
			: (channel == 1)
			? c.g
			: (channel == 2)
			? c.b
			: FxLuma(c.r, c.g, c.b);
		const auto k = FxClamp01(1.f - value);
		constexpr auto kTouch = float(kPi / 4.);
		const auto radius = cell * ((k < kTouch)
			? std::sqrt(k / float(kPi))
			: (0.5f + (k - kTouch) / (1.f - kTouch) * 0.2072f));
		const auto distance = float(std::hypot(u - cu, v - cv)) * cell;
		return FxMix(k, FxClamp01(radius - distance + 0.5f), dots);
	};
	const auto ink = FxRgba{
		float(spec.ink.redF()),
		float(spec.ink.greenF()),
		float(spec.ink.blueF()),
		1.f,
	};
	const auto paper = FxRgba{
		float(spec.paper.redF()),
		float(spec.paper.greenF()),
		float(spec.paper.blueF()),
		1.f,
	};
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		const auto px = x + 0.5f;
		const auto py = y + 0.5f;
		auto result = FxRgba();
		if (spec.color) {
			result.r = 1.f - coverage(rasters[0], px, py, 0);
			result.g = 1.f - coverage(rasters[1], px, py, 1);
			result.b = 1.f - coverage(rasters[2], px, py, 2);
		} else {
			const auto k = coverage(rasters[0], px, py, 3);
			result.r = FxMix(paper.r, ink.r, k);
			result.g = FxMix(paper.g, ink.g, k);
			result.b = FxMix(paper.b, ink.b, k);
		}
		c.r = FxMix(c.r, result.r, spec.amount);
		c.g = FxMix(c.g, result.g, spec.amount);
		c.b = FxMix(c.b, result.b, spec.amount);
	});
}

//
// The date stamp: two fonts drawn in code, a 5 x 7 dot matrix and the
// seven-segment digits of a camera date back.
//

struct Glyph {
	ushort code = 0;
	const char *rows = nullptr;
};

constexpr Glyph kGlyphs[] = {
	{ '0', ".###." "#...#" "#..##" "#.#.#" "##..#" "#...#" ".###." },
	{ '1', "..#.." ".##.." "..#.." "..#.." "..#.." "..#.." ".###." },
	{ '2', ".###." "#...#" "....#" "...#." "..#.." ".#..." "#####" },
	{ '3', "#####" "...#." "..#.." "...#." "....#" "#...#" ".###." },
	{ '4', "...#." "..##." ".#.#." "#..#." "#####" "...#." "...#." },
	{ '5', "#####" "#...." "####." "....#" "....#" "#...#" ".###." },
	{ '6', "..##." ".#..." "#...." "####." "#...#" "#...#" ".###." },
	{ '7', "#####" "....#" "...#." "..#.." ".#..." ".#..." ".#..." },
	{ '8', ".###." "#...#" "#...#" ".###." "#...#" "#...#" ".###." },
	{ '9', ".###." "#...#" "#...#" ".####" "....#" "...#." ".##.." },
	{ 'A', ".###." "#...#" "#...#" "#####" "#...#" "#...#" "#...#" },
	{ 'B', "####." "#...#" "#...#" "####." "#...#" "#...#" "####." },
	{ 'C', ".###." "#...#" "#...." "#...." "#...." "#...#" ".###." },
	{ 'D', "###.." "#..#." "#...#" "#...#" "#...#" "#..#." "###.." },
	{ 'E', "#####" "#...." "#...." "####." "#...." "#...." "#####" },
	{ 'F', "#####" "#...." "#...." "####." "#...." "#...." "#...." },
	{ 'G', ".###." "#...#" "#...." "#.###" "#...#" "#...#" ".####" },
	{ 'H', "#...#" "#...#" "#...#" "#####" "#...#" "#...#" "#...#" },
	{ 'I', ".###." "..#.." "..#.." "..#.." "..#.." "..#.." ".###." },
	{ 'J', "..###" "...#." "...#." "...#." "...#." "#..#." ".##.." },
	{ 'K', "#...#" "#..#." "#.#.." "##..." "#.#.." "#..#." "#...#" },
	{ 'L', "#...." "#...." "#...." "#...." "#...." "#...." "#####" },
	{ 'M', "#...#" "##.##" "#.#.#" "#.#.#" "#...#" "#...#" "#...#" },
	{ 'N', "#...#" "#...#" "##..#" "#.#.#" "#..##" "#...#" "#...#" },
	{ 'O', ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###." },
	{ 'P', "####." "#...#" "#...#" "####." "#...." "#...." "#...." },
	{ 'Q', ".###." "#...#" "#...#" "#...#" "#.#.#" "#..#." ".##.#" },
	{ 'R', "####." "#...#" "#...#" "####." "#.#.." "#..#." "#...#" },
	{ 'S', ".####" "#...." "#...." ".###." "....#" "....#" "####." },
	{ 'T', "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "..#.." },
	{ 'U', "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" ".###." },
	{ 'V', "#...#" "#...#" "#...#" "#...#" "#...#" ".#.#." "..#.." },
	{ 'W', "#...#" "#...#" "#...#" "#.#.#" "#.#.#" "#.#.#" ".#.#." },
	{ 'X', "#...#" "#...#" ".#.#." "..#.." ".#.#." "#...#" "#...#" },
	{ 'Y', "#...#" "#...#" "#...#" ".#.#." "..#.." "..#.." "..#.." },
	{ 'Z', "#####" "....#" "...#." "..#.." ".#..." "#...." "#####" },
	{ 0x0411, "#####" "#...." "#...." "####." "#...#" "#...#" "####." },
	{ 0x0413, "#####" "#...." "#...." "#...." "#...." "#...." "#...." },
	{ 0x0414, "..##." ".#.#." ".#.#." ".#.#." ".#.#." "#####" "#...#" },
	{ 0x0416, "#.#.#" "#.#.#" ".###." "..#.." ".###." "#.#.#" "#.#.#" },
	{ 0x0417, ".###." "#...#" "....#" "..##." "....#" "#...#" ".###." },
	{ 0x0418, "#...#" "#...#" "#..##" "#.#.#" "##..#" "#...#" "#...#" },
	{ 0x0419, ".#.#." "..#.." "#...#" "#..##" "#.#.#" "##..#" "#...#" },
	{ 0x041B, "..###" ".#..#" ".#..#" ".#..#" ".#..#" ".#..#" "#...#" },
	{ 0x041F, "#####" "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" },
	{ 0x0423, "#...#" "#...#" "#...#" ".####" "....#" "#...#" ".###." },
	{ 0x0424, "..#.." ".###." "#.#.#" "#.#.#" ".###." "..#.." "..#.." },
	{ 0x0426, "#..#." "#..#." "#..#." "#..#." "#..#." "#####" "....#" },
	{ 0x0427, "#...#" "#...#" "#...#" ".####" "....#" "....#" "....#" },
	{ 0x0428, "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#####" },
	{ 0x0429, "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#####" "....#" },
	{ 0x042A, "##..." ".#..." ".#..." ".###." ".#..#" ".#..#" ".###." },
	{ 0x042B, "#...#" "#...#" "#...#" "##..#" "#.#.#" "#.#.#" "##..#" },
	{ 0x042C, "#...." "#...." "#...." "####." "#...#" "#...#" "####." },
	{ 0x042D, ".###." "#...#" "....#" ".####" "....#" "#...#" ".###." },
	{ 0x042E, "#..#." "#.#.#" "#.#.#" "###.#" "#.#.#" "#.#.#" "#..#." },
	{ 0x042F, ".####" "#...#" "#...#" ".####" "..#.#" ".#..#" "#...#" },
	{ '\'', "..#.." "..#.." ".#..." "....." "....." "....." "....." },
	{ '"', ".#.#." ".#.#." ".#.#." "....." "....." "....." "....." },
	{ '.', "....." "....." "....." "....." "....." ".##.." ".##.." },
	{ ',', "....." "....." "....." "....." ".##.." "..#.." ".#..." },
	{ ':', "....." ".##.." ".##.." "....." ".##.." ".##.." "....." },
	{ '/', "....#" "....#" "...#." "..#.." ".#..." "#...." "#...." },
	{ '-', "....." "....." "....." "#####" "....." "....." "....." },
	{ '+', "....." "..#.." "..#.." "#####" "..#.." "..#.." "....." },
	{ '!', "..#.." "..#.." "..#.." "..#.." "..#.." "....." "..#.." },
	{ '?', ".###." "#...#" "....#" "...#." "..#.." "....." "..#.." },
	{ '(', "...#." "..#.." ".#..." ".#..." ".#..." "..#.." "...#." },
	{ ')', ".#..." "..#.." "...#." "...#." "...#." "..#.." ".#..." },
	{ '%', "##..#" "##..#" "...#." "..#.." ".#..." "#..##" "#..##" },
	{ '#', ".#.#." ".#.#." "#####" ".#.#." "#####" ".#.#." ".#.#." },
	{ '*', "....." "#.#.#" ".###." "#####" ".###." "#.#.#" "....." },
	{ '=', "....." "....." "#####" "....." "#####" "....." "....." },
	{ '_', "....." "....." "....." "....." "....." "....." "#####" },
	{ '<', "...#." "..#.." ".#..." "#...." ".#..." "..#.." "...#." },
	{ '>', ".#..." "..#.." "...#." "....#" "...#." "..#.." ".#..." },
	{ 0x25B6, "#...." "##..." "###.." "####." "###.." "##..." "#...." },
	{ 0x25CF, "....." ".###." "#####" "#####" "#####" ".###." "....." },
};

[[nodiscard]] ushort StampCode(QChar ch) {
	const auto code = ch.toUpper().unicode();
	switch (code) {
	case 0x0401: return 'E';
	case 0x0410: return 'A';
	case 0x0412: return 'B';
	case 0x0415: return 'E';
	case 0x041A: return 'K';
	case 0x041C: return 'M';
	case 0x041D: return 'H';
	case 0x041E: return 'O';
	case 0x0420: return 'P';
	case 0x0421: return 'C';
	case 0x0422: return 'T';
	case 0x0425: return 'X';
	case 0x2018:
	case 0x2019:
	case 0x0060: return '\'';
	case 0x201C:
	case 0x201D:
	case 0x00AB:
	case 0x00BB: return '"';
	case 0x2013:
	case 0x2014:
	case 0x2212: return '-';
	case 0x00A0: return ' ';
	}
	return code;
}

[[nodiscard]] const char *FindGlyph(QChar ch) {
	const auto code = StampCode(ch);
	for (const auto &glyph : kGlyphs) {
		if (glyph.code == code) {
			return glyph.rows;
		}
	}
	return nullptr;
}

[[nodiscard]] bool StampDraws(QChar ch) {
	return (StampCode(ch) == ' ') || (FindGlyph(ch) != nullptr);
}

struct StampShape {
	QPainterPath path;
	QSizeF size;
	bool crisp = false;
};

void AddSegments(
		QPainterPath &path,
		uint32 mask,
		float left,
		float height) {
	const auto width = height * kSegmentWidth;
	const auto thickness = height * kSegmentThickness;
	const auto half = thickness / 2.f;
	const auto gap = thickness * 0.16f;
	const auto horizontal = [&](float y) {
		const auto x0 = left + half + gap;
		const auto x1 = left + width - half - gap;
		auto polygon = QPolygonF();
		polygon << QPointF(x0, y)
			<< QPointF(x0 + half, y - half)
			<< QPointF(x1 - half, y - half)
			<< QPointF(x1, y)
			<< QPointF(x1 - half, y + half)
			<< QPointF(x0 + half, y + half);
		path.addPolygon(polygon);
		path.closeSubpath();
	};
	const auto vertical = [&](float x, float y0, float y1) {
		auto polygon = QPolygonF();
		polygon << QPointF(x, y0)
			<< QPointF(x + half, y0 + half)
			<< QPointF(x + half, y1 - half)
			<< QPointF(x, y1)
			<< QPointF(x - half, y1 - half)
			<< QPointF(x - half, y0 + half);
		path.addPolygon(polygon);
		path.closeSubpath();
	};
	const auto middle = height / 2.f;
	if (mask & 0x01U) {
		horizontal(half);
	}
	if (mask & 0x40U) {
		horizontal(middle);
	}
	if (mask & 0x08U) {
		horizontal(height - half);
	}
	if (mask & 0x20U) {
		vertical(left + half, half + gap, middle - gap);
	}
	if (mask & 0x02U) {
		vertical(left + width - half, half + gap, middle - gap);
	}
	if (mask & 0x10U) {
		vertical(left + half, middle + gap, height - half - gap);
	}
	if (mask & 0x04U) {
		vertical(left + width - half, middle + gap, height - half - gap);
	}
}

// The size is the one of the whole text. The path gets only the glyphs
// that touch the columns from 'from' to 'till': a long text on a large
// photo is several times wider than the photo, only its part over the
// photo is worth drawing. Nothing gets into the path when the range is
// empty, that is how the text is measured.
[[nodiscard]] StampShape BuildStamp(
		const QString &text,
		LofiStampStyle style,
		float wanted,
		float from,
		float till) {
	constexpr auto kDigits = std::array<uchar, 10>{
		0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
	};
	auto result = StampShape();
	result.path.setFillRule(Qt::WindingFill);
	const auto segments = (style == LofiStampStyle::Segments);
	const auto bold = (style == LofiStampStyle::PixelBold);
	auto dot = wanted / float(kGlyphRows);
	if (dot >= 1.5f) {
		dot = std::round(dot);
		result.crisp = !segments;
	}
	const auto height = dot * kGlyphRows;
	const auto thickness = height * kSegmentThickness;
	const auto digitWidth = height * kSegmentWidth;
	const auto digitAdvance = height * (kSegmentWidth + 0.2f);
	const auto markAdvance = height * 0.3f;
	auto x = 0.f;
	auto right = 0.f;
	const auto shown = [&](float width) {
		return (x + width > from) && (x < till);
	};
	for (const auto original : text) {
		const auto code = StampCode(original);
		if (code == ' ') {
			x += segments ? (height * 0.42f) : (dot * 4.f);
			continue;
		}
		if (segments) {
			auto drawn = true;
			if (code >= '0' && code <= '9') {
				if (shown(digitWidth)) {
					AddSegments(result.path, kDigits[code - '0'], x, height);
				}
				right = x + digitWidth;
				x += digitAdvance;
			} else if (code == '-') {
				if (shown(digitWidth)) {
					AddSegments(result.path, 0x40U, x, height);
				}
				right = x + digitWidth;
				x += digitAdvance;
			} else if (code == '\'') {
				const auto width = thickness * 1.6f;
				if (shown(width)) {
					auto polygon = QPolygonF();
					polygon << QPointF(x + thickness * 0.6f, 0.)
						<< QPointF(x + width, 0.)
						<< QPointF(x + thickness, height * 0.24f)
						<< QPointF(x, height * 0.24f);
					result.path.addPolygon(polygon);
					result.path.closeSubpath();
				}
				right = x + width;
				x += markAdvance;
			} else if (code == ':') {
				if (shown(thickness)) {
					result.path.addRect(QRectF(
						x,
						height * 0.3f - thickness / 2.f,
						thickness,
						thickness));
					result.path.addRect(QRectF(
						x,
						height * 0.7f - thickness / 2.f,
						thickness,
						thickness));
				}
				right = x + thickness;
				x += markAdvance;
			} else if (code == '.') {
				if (shown(thickness)) {
					result.path.addRect(QRectF(
						x,
						height - thickness,
						thickness,
						thickness));
				}
				right = x + thickness;
				x += markAdvance;
			} else if (code == '/') {
				if (shown(digitWidth)) {
					auto polygon = QPolygonF();
					polygon << QPointF(x + digitWidth - thickness, 0.)
						<< QPointF(x + digitWidth, 0.)
						<< QPointF(x + thickness, height)
						<< QPointF(x, height);
					result.path.addPolygon(polygon);
					result.path.closeSubpath();
				}
				right = x + digitWidth;
				x += digitAdvance;
			} else {
				drawn = false;
			}
			if (drawn) {
				continue;
			}
		}
		const auto rows = FindGlyph(original);
		if (!rows) {
			continue;
		}
		const auto width = dot * (kGlyphColumns + (bold ? 1 : 0));
		if (shown(width)) {
			for (auto row = 0; row != kGlyphRows; ++row) {
				for (auto column = 0; column != kGlyphColumns; ++column) {
					if (rows[row * kGlyphColumns + column] == '#') {
						result.path.addRect(QRectF(
							x + column * dot,
							row * dot,
							bold ? (dot * 2.f) : dot,
							dot));
					}
				}
			}
		}
		right = x + width;
		x += width + dot * (segments ? 1.6f : 1.f);
	}
	result.size = QSizeF(right, height);
	return result;
}

// The columns of the stamp from 'from' on, size.width() of them.
[[nodiscard]] QImage StampMask(const StampShape &shape, int from, QSize size) {
	if (size.isEmpty()
		|| int64(size.width()) * size.height() > kMaxStampMaskPixels) {
		return QImage();
	}
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing, !shape.crisp);
	p.translate(-from, 0);
	p.fillPath(shape.path, Qt::white);
	p.end();
	return result;
}

[[nodiscard]] QImage Tinted(const QImage &mask, QColor color) {
	auto result = mask.copy();
	if (result.isNull()) {
		return result;
	}
	auto p = QPainter(&result);
	p.setCompositionMode(QPainter::CompositionMode_SourceIn);
	p.fillRect(result.rect(), color);
	p.end();
	return result;
}

[[nodiscard]] QString StampTextOf(const QByteArray &data) {
	return LofiStampFilter(QString::fromUtf8(data));
}

// Where the caret of the text field goes when the characters the fonts
// can't draw are taken out of it: behind what is kept of the text that
// was before the caret.
[[nodiscard]] int StampCaretAfterFilter(const QString &before) {
	return int(LofiStampFilter(before).size());
}

[[nodiscard]] LofiStamp StampFrom(
		const FxParams &params,
		QByteArrayView text,
		QByteArrayView style,
		QByteArrayView color,
		QByteArrayView corner) {
	return {
		.text = StampTextOf(params.data(text)),
		.style = LofiStampStyle(std::clamp(
			params.integer(style),
			0,
			kLofiStampStyleCount - 1)),
		.color = params.color(color),
		.corner = LofiStampCorner(std::clamp(
			params.integer(corner),
			0,
			kLofiStampCornerCount - 1)),
	};
}

} // namespace

bool LofiJpegAvailable() {
	static const auto result = QImageWriter::supportedImageFormats().contains(
		QByteArray("jpeg"))
		&& QImageReader::supportedImageFormats().contains(
			QByteArray("jpeg"));
	return result;
}

bool LofiJpegRoundTrip(
		QImage &image,
		int quality,
		int generations,
		const std::atomic<bool> *cancel) {
	constexpr auto kShiftX = std::array<int, 8>{ 0, 3, 5, 1, 6, 2, 7, 4 };
	constexpr auto kShiftY = std::array<int, 8>{ 0, 5, 2, 7, 3, 6, 1, 4 };
	constexpr auto kQualityStep = std::array<int, 6>{ 0, 4, -3, 6, -5, 2 };
	if (image.isNull()) {
		return false;
	} else if (generations <= 0 || !LofiJpegAvailable()) {
		return !LofiCancelled(cancel);
	} else if (!FxPrepare(image)) {
		return false;
	}
	const auto w = image.width();
	const auto h = image.height();
	auto current = QImage(w, h, QImage::Format_RGB32);
	if (current.isNull()) {
		return false;
	}
	for (auto y = 0; y != h; ++y) {
		const auto from = FxRow(std::as_const(image), y);
		const auto to = FxRow(current, y);
		for (auto x = 0; x != w; ++x) {
			to[x] = from[x] | 0xFF000000U;
		}
	}
	for (auto generation = 0; generation != generations; ++generation) {
		if (LofiCancelled(cancel)) {
			return false;
		}
		const auto used = std::clamp(
			quality + kQualityStep[generation % kQualityStep.size()],
			1,
			100);
		const auto ox = kShiftX[generation % kShiftX.size()];
		const auto oy = kShiftY[generation % kShiftY.size()];
		if (!ox && !oy) {
			if (!JpegPass(current, used)) {
				break;
			}
			continue;
		}
		auto shifted = QImage(w + ox, h + oy, QImage::Format_RGB32);
		if (shifted.isNull()) {
			break;
		}
		for (auto y = 0; y != h + oy; ++y) {
			const auto from = FxRow(std::as_const(current), std::max(y - oy, 0));
			const auto to = FxRow(shifted, y);
			for (auto x = 0; x != ox; ++x) {
				to[x] = from[0];
			}
			std::memcpy(to + ox, from, size_t(w) * 4);
		}
		if (!JpegPass(shifted, used)) {
			break;
		}
		for (auto y = 0; y != h; ++y) {
			std::memcpy(
				FxRow(current, y),
				FxRow(std::as_const(shifted), y + oy) + ox,
				size_t(w) * 4);
		}
	}
	FxParallelRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto packed = FxRow(std::as_const(current), y);
			for (auto x = 0; x != w; ++x) {
				const auto a = (line[x] >> 24);
				const auto p = packed[x];
				if (a == 0xFFU) {
					line[x] = p | 0xFF000000U;
				} else if (a) {
					line[x] = (a << 24)
						| (std::min((p >> 16) & 0xFFU, a) << 16)
						| (std::min((p >> 8) & 0xFFU, a) << 8)
						| std::min(p & 0xFFU, a);
				}
			}
		}
	});
	return !LofiCancelled(cancel);
}

QString LofiStampFilter(const QString &text) {
	auto result = QString();
	result.reserve(std::min(int(text.size()), kMaxStampLength));
	for (const auto ch : text) {
		if (result.size() >= kMaxStampLength) {
			break;
		} else if (StampDraws(ch)) {
			result.push_back((ch.unicode() == 0x00A0) ? QChar(' ') : ch);
		}
	}
	return result;
}

QString LofiStampText(const QDateTime &moment, LofiStampDate format) {
	const auto date = moment.date();
	const auto daytime = moment.time();
	const auto two = [](int value) {
		return u"%1"_q.arg(value, 2, 10, QChar('0'));
	};
	const auto full = two(date.day())
		+ QChar('.')
		+ two(date.month())
		+ QChar('.')
		+ QString::number(date.year());
	switch (format) {
	case LofiStampDate::Classic:
		return u"'%1 %2 %3"_q.arg(
			two(date.year() % 100),
			QString::number(date.month()),
			QString::number(date.day()));
	case LofiStampDate::Date:
		return full;
	case LofiStampDate::DateTime:
		return full
			+ QChar(' ')
			+ two(daytime.hour())
			+ QChar(':')
			+ two(daytime.minute());
	}
	return full;
}

void LofiPaintStamp(QImage &image, const LofiStamp &stamp) {
	const auto text = LofiStampFilter(stamp.text).trimmed();
	if (text.isEmpty() || !FxPrepare(image)) {
		return;
	}
	const auto w = image.width();
	const auto h = image.height();
	const auto side = float(std::min(w, h));
	const auto height = std::max(float(stamp.size) * side, 4.f);
	const auto whole = BuildStamp(text, stamp.style, height, 0.f, 0.f).size;
	if (!(whole.width() > 0.)
		|| !(whole.height() > 0.)
		|| whole.width() > kMaxStampSide
		|| whole.height() > kMaxStampSide) {
		return;
	}
	const auto size = QSize(
		int(std::ceil(whole.width())),
		int(std::ceil(whole.height())));
	const auto margin = float(stamp.margin) * side;
	const auto onLeft = (stamp.corner == LofiStampCorner::BottomLeft)
		|| (stamp.corner == LofiStampCorner::TopLeft);
	const auto onTop = (stamp.corner == LofiStampCorner::TopRight)
		|| (stamp.corner == LofiStampCorner::TopLeft);
	const auto wholeLeft = int(std::lround(onLeft
		? margin
		: (w - margin - size.width())));
	const auto top = int(std::lround(onTop
		? margin
		: (h - margin - size.height())));

	// Only the part over the picture is rasterized, with a border wide
	// enough for the glow and the shadow of what is cut away not to be
	// missed: the blur reaches less than its padding.
	const auto pad = int(std::ceil(size.height() * 0.8));
	const auto keep = 2 * pad;
	const auto from = std::clamp(-wholeLeft - keep, 0, size.width());
	const auto till = std::clamp(w - wholeLeft + keep, 0, size.width());
	if (till <= from) {
		return;
	}
	const auto mask = StampMask(
		BuildStamp(text, stamp.style, height, float(from), float(till)),
		from,
		QSize(till - from, size.height()));
	if (mask.isNull()) {
		return;
	}
	const auto left = wholeLeft + from;
	auto color = stamp.color.isValid() ? stamp.color : QColor(255, 151, 41);
	color.setAlpha(255);
	auto p = QPainter(&image);
	const auto glowing = std::clamp(stamp.glow, 0., 1.);
	if (glowing > 0.) {
		auto glow = QImage(
			mask.width() + 2 * pad,
			mask.height() + 2 * pad,
			QImage::Format_ARGB32_Premultiplied);
		if (!glow.isNull()) {
			glow.fill(Qt::transparent);
			{
				auto q = QPainter(&glow);
				q.drawImage(pad, pad, Tinted(mask, color));
			}
			FxGaussianBlur(glow, std::max(mask.height() * 0.2, 0.6));
			p.setCompositionMode(QPainter::CompositionMode_Screen);
			p.setOpacity(glowing);
			p.drawImage(left - pad, top - pad, glow);
			p.setOpacity(1.);
			p.setCompositionMode(QPainter::CompositionMode_SourceOver);
		}
	}
	if (stamp.style == LofiStampStyle::Camcorder) {
		const auto offset = std::max(
			int(std::lround(mask.height() / float(kGlyphRows) * 0.8f)),
			1);
		p.drawImage(
			left + offset,
			top + offset,
			Tinted(mask, QColor(0, 0, 0, 205)));
	}
	p.drawImage(left, top, Tinted(mask, color));
	p.end();
}

namespace {

[[nodiscard]] object_ptr<Ui::RpWidget> CreateStampTextEditor(
	not_null<QWidget*> parent,
	FxCustomEditorArgs &&args);

[[nodiscard]] DepthSpec CameraDepth(int index, int dither) {
	auto result = DepthSpec();
	switch (index) {
	case 1: result.levels = { 32, 64, 32 }; break;
	case 2: result.levels = { 16, 16, 16 }; break;
	case 3: result.levels = { 8, 8, 4 }; break;
	case 4: result.levels = { 4, 4, 4 }; break;
	case 5: result.levels = { 2, 2, 2 }; break;
	case 6: result.levels = { 4, 4, 4 }; result.mono = true; break;
	case 7: result.levels = { 2, 2, 2 }; result.mono = true; break;
	}
	result.dither = (dither == 1)
		? Dither::Bayer4
		: (dither == 2)
		? Dither::FloydSteinberg
		: Dither::None;
	return result;
}

// The whole camera. Everything happens on a copy of the picture that has
// the resolution of the sensor (in the preview too, that is what keeps
// the preview equal to the export): light and optics, the sensor, the
// processing of the camera, the date stamp, JPEG. Then the small picture
// is stretched back over the layer.
[[nodiscard]] bool ApplyCamera(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto rendered = image.size();
	const auto workSize = SizeWithLongSide(
		LofiFullSize(image, context),
		params.integer("side"));
	auto work = LofiResized(image, workSize, Resample::Smooth);
	if (!FxPrepare(work)) {
		return false;
	}
	const auto nominal = float(std::max(workSize.width(), workSize.height()));
	const auto relative = nominal / kReferenceSensor;
	const auto cancel = context.cancel;

	if (const auto flash = Percent(params, "flash"); flash > 0.f) {
		StageFlash(work, { .amount = flash });
	}
	if (const auto amount = Percent(params, "aberration"); amount > 0.f) {
		StageAberration(work, { .amount = amount * 5.f * relative }, 1.f);
	}
	const auto bloom = Percent(params, "bloom");
	const auto halation = Percent(params, "halation");
	if (bloom > 0.f || halation > 0.f) {
		StageBloom(work, {
			.amount = bloom,
			.threshold = 0.68f,
			.radius = 14.f * relative,
			.halation = halation,
		}, 1.f);
	}
	if (LofiCancelled(cancel)) {
		return false;
	}
	const auto noise = NoiseSpec{
		.amount = Percent(params, "noise"),
		.color = Percent(params, "chroma"),
		.cell = 1.f,
		.hot = Percent(params, "hot"),
		.seed = LofiSeed(params, context, 0xCA3E2AU),
	};
	if (noise.amount > 0.f || noise.color > 0.f || noise.hot > 0.f) {
		StageNoise(work, noise, 1.f);
	}
	const auto cast = CastSpec{
		.temperature = Percent(params, "temperature"),
		.tint = Percent(params, "tint"),
		.saturation = Percent(params, "saturation"),
	};
	if (cast.temperature != 0.f
		|| cast.tint != 0.f
		|| cast.saturation != 0.f) {
		StageCast(work, cast);
	}
	if (const auto sharpen = Percent(params, "sharpen"); sharpen > 0.f) {
		StageSharpen(
			work,
			sharpen * 2.4f,
			std::max(std::sqrt(relative), 0.8f),
			1.f);
	}
	if (LofiCancelled(cancel)) {
		return false;
	}
	if (const auto depth = params.integer("depth"); depth > 0) {
		StageDepth(work, CameraDepth(depth, params.integer("dither")), 1.f);
	}
	if (params.boolean("stamp")) {
		auto stamp = StampFrom(
			params,
			"stamp_text",
			"stamp_style",
			"stamp_color",
			"stamp_corner");
		stamp.size = 0.052;
		LofiPaintStamp(work, stamp);
	}
	if (!LofiJpegRoundTrip(
			work,
			params.integer("quality"),
			params.integer("generations"),
			cancel)) {
		return false;
	}
	const auto method = (params.integer("upscale") == 1)
		? Resample::Pixels
		: (params.integer("upscale") == 2)
		? Resample::Sharp
		: Resample::Smooth;
	auto result = LofiResized(work, rendered, method);
	if (result.isNull()) {
		return false;
	}
	image = std::move(result);
	return !LofiCancelled(cancel);
}

[[nodiscard]] bool ApplySensor(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto rendered = image.size();
	const auto workSize = SizeWithLongSide(
		LofiFullSize(image, context),
		params.integer("side"));
	if (workSize.width() >= rendered.width()
		&& workSize.height() >= rendered.height()) {
		return !context.cancelled();
	}
	const auto work = LofiResized(
		image,
		workSize,
		(params.integer("down") == 1) ? Resample::Pixels : Resample::Smooth);
	if (work.isNull() || context.cancelled()) {
		return false;
	}
	const auto method = (params.integer("up") == 1)
		? Resample::Pixels
		: (params.integer("up") == 2)
		? Resample::Sharp
		: Resample::Smooth;
	auto result = LofiResized(work, rendered, method);
	if (result.isNull()) {
		return false;
	}
	image = std::move(result);
	return !context.cancelled();
}

[[nodiscard]] bool ApplyJpeg(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto rendered = image.size();
	const auto full = LofiFullSize(image, context);
	const auto ratio = 8. / std::max(params.number("block"), 8.);
	auto workSize = QSize(
		std::max(int(std::lround(full.width() * ratio)), kMinWorkSide),
		std::max(int(std::lround(full.height() * ratio)), kMinWorkSide));
	// An interactive render of a large photo compresses a reduced copy
	// (never smaller than what is on the screen): the blocks come out
	// a little larger than in the export, the look is the same. Qt does
	// not read back pictures over 256 MB at all, so that is the limit of
	// an export.
	const auto pixels = int64(workSize.width()) * workSize.height();
	const auto limit = std::min(
		kMaxJpegPixels,
		(context.preview
			? std::max(
				kPreviewJpegPixels,
				int64(rendered.width()) * rendered.height())
			: kMaxJpegPixels));
	if (pixels > limit) {
		const auto reduce = std::sqrt(limit / double(pixels));
		workSize = QSize(
			std::max(int(std::lround(workSize.width() * reduce)), 1),
			std::max(int(std::lround(workSize.height() * reduce)), 1));
	}
	auto work = LofiResized(image, workSize, Resample::Smooth);
	if (!FxPrepare(work)
		|| !LofiJpegRoundTrip(
			work,
			params.integer("quality"),
			params.integer("generations"),
			context.cancel)) {
		return false;
	}
	auto result = LofiResized(work, rendered, Resample::Smooth);
	if (result.isNull()) {
		return false;
	}
	image = std::move(result);
	return !context.cancelled();
}

[[nodiscard]] std::vector<FxText> CornerNames() {
	return {
		tr::lng_oblivion_photo_lofi_stamp_corner_br,
		tr::lng_oblivion_photo_lofi_stamp_corner_bl,
		tr::lng_oblivion_photo_lofi_stamp_corner_tr,
		tr::lng_oblivion_photo_lofi_stamp_corner_tl,
	};
}

[[nodiscard]] std::vector<FxText> FontNames() {
	return {
		tr::lng_oblivion_photo_lofi_stamp_font_segments,
		tr::lng_oblivion_photo_lofi_stamp_font_pixel,
		tr::lng_oblivion_photo_lofi_stamp_font_bold,
		tr::lng_oblivion_photo_lofi_stamp_font_camcorder,
	};
}

[[nodiscard]] std::vector<FxText> UpscaleNames() {
	return {
		tr::lng_oblivion_photo_lofi_upscale_smooth,
		tr::lng_oblivion_photo_lofi_upscale_pixels,
		tr::lng_oblivion_photo_lofi_upscale_sharp,
	};
}

void RegisterCamera() {
	const auto stamped = [](const FxParams &params) {
		return params.boolean("stamp");
	};
	RegisterFx({
		.id = "lofi.camera",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_camera,
		.params = {
			FxInt(
				"side",
				tr::lng_oblivion_photo_lofi_resolution,
				96,
				2048,
				1024,
				u" px"_q).under(tr::lng_oblivion_photo_lofi_sec_sensor),
			FxChoice(
				"upscale",
				tr::lng_oblivion_photo_lofi_upscale,
				UpscaleNames()),
			FxInt(
				"aberration",
				tr::lng_oblivion_photo_lofi_aberrations,
				0,
				100,
				20).under(tr::lng_oblivion_photo_lofi_sec_optics),
			FxInt("bloom", tr::lng_oblivion_photo_lofi_glow, 0, 100, 20),
			FxInt(
				"halation",
				tr::lng_oblivion_photo_lofi_halation,
				0,
				100,
				0),
			FxInt(
				"flash",
				tr::lng_oblivion_photo_lofi_flash,
				0,
				100,
				0).under(tr::lng_oblivion_photo_lofi_sec_light),
			FxInt(
				"temperature",
				tr::lng_oblivion_photo_lofi_temperature,
				-100,
				100,
				0),
			FxInt("tint", tr::lng_oblivion_photo_lofi_tint, -100, 100, 0),
			FxInt(
				"saturation",
				tr::lng_oblivion_photo_lofi_saturation,
				-100,
				100,
				10),
			FxInt(
				"noise",
				tr::lng_oblivion_photo_lofi_noise_luma,
				0,
				100,
				25).under(tr::lng_oblivion_photo_lofi_sec_noise),
			FxInt(
				"chroma",
				tr::lng_oblivion_photo_lofi_noise_color,
				0,
				100,
				30),
			FxInt("hot", tr::lng_oblivion_photo_lofi_noise_hot, 0, 100, 5),
			FxSeed(),
			FxInt(
				"sharpen",
				tr::lng_oblivion_photo_lofi_sharpen,
				0,
				100,
				45).under(tr::lng_oblivion_photo_lofi_sec_processing),
			FxChoice(
				"depth",
				tr::lng_oblivion_photo_lofi_depth,
				{
					tr::lng_oblivion_photo_lofi_depth_full,
					tr::lng_oblivion_photo_lofi_depth_16,
					tr::lng_oblivion_photo_lofi_depth_12,
					tr::lng_oblivion_photo_lofi_depth_256,
					tr::lng_oblivion_photo_lofi_depth_64,
					tr::lng_oblivion_photo_lofi_depth_8,
					tr::lng_oblivion_photo_lofi_depth_gray,
					tr::lng_oblivion_photo_lofi_depth_bw,
				}),
			FxChoice(
				"dither",
				tr::lng_oblivion_photo_lofi_dither,
				{
					tr::lng_oblivion_photo_lofi_dither_none,
					tr::lng_oblivion_photo_lofi_dither_ordered,
					tr::lng_oblivion_photo_lofi_dither_fs,
				},
				1).when([](const FxParams &params) {
					return params.integer("depth") != 0;
				}),
			FxInt(
				"quality",
				tr::lng_oblivion_photo_lofi_quality,
				1,
				100,
				55).under(tr::lng_oblivion_photo_lofi_sec_jpeg),
			FxInt(
				"generations",
				tr::lng_oblivion_photo_lofi_generations,
				0,
				12,
				1),
			FxBool(
				"stamp",
				tr::lng_oblivion_photo_lofi_stamp,
				false).under(tr::lng_oblivion_photo_lofi_sec_stamp),
			FxCustom(
				"stamp_text",
				tr::lng_oblivion_photo_lofi_stamp_text,
				kTextEditorType,
				kDefaultStampText).when(stamped),
			FxChoice(
				"stamp_style",
				tr::lng_oblivion_photo_lofi_stamp_font,
				FontNames()).when(stamped),
			FxColor(
				"stamp_color",
				tr::lng_oblivion_photo_lofi_stamp_color,
				QColor(255, 151, 41)).when(stamped),
			FxChoice(
				"stamp_corner",
				tr::lng_oblivion_photo_lofi_stamp_corner,
				CornerNames()).when(stamped),
		},
		.flags = kFxNeighbours | kFxSeeded,
		.order = 0,
		.apply = ApplyCamera,
	});
}

void RegisterOptics() {
	RegisterFx({
		.id = "lofi.sensor",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_sensor,
		.params = {
			FxInt(
				"side",
				tr::lng_oblivion_photo_lofi_resolution,
				64,
				4096,
				640,
				u" px"_q),
			FxChoice(
				"down",
				tr::lng_oblivion_photo_lofi_downscale,
				{
					tr::lng_oblivion_photo_lofi_downscale_smooth,
					tr::lng_oblivion_photo_lofi_downscale_rough,
				}),
			FxChoice(
				"up",
				tr::lng_oblivion_photo_lofi_upscale,
				UpscaleNames()),
		},
		.flags = kFxNeighbours,
		.order = 10,
		.apply = ApplySensor,
	});
	RegisterFx({
		.id = "lofi.jpeg",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_jpeg,
		.params = {
			FxInt("quality", tr::lng_oblivion_photo_lofi_quality, 1, 100, 15),
			FxInt(
				"generations",
				tr::lng_oblivion_photo_lofi_generations,
				1,
				30,
				1),
			FxPixels("block", tr::lng_oblivion_photo_lofi_block, 8., 64., 8.),
		},
		.flags = kFxNeighbours,
		.order = 20,
		.apply = ApplyJpeg,
	});
	RegisterFx({
		.id = "lofi.noise",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_noise,
		.params = {
			FxInt(
				"amount",
				tr::lng_oblivion_photo_lofi_noise_luma,
				0,
				100,
				35),
			FxInt(
				"color",
				tr::lng_oblivion_photo_lofi_noise_color,
				0,
				100,
				40),
			FxPixels(
				"size",
				tr::lng_oblivion_photo_lofi_noise_grain,
				1.,
				12.,
				1.5,
				1),
			FxInt("hot", tr::lng_oblivion_photo_lofi_noise_hot, 0, 100, 10),
			FxInt(
				"banding",
				tr::lng_oblivion_photo_lofi_noise_banding,
				0,
				100,
				0),
			FxChoice(
				"blend",
				tr::lng_oblivion_photo_digicam_noise_blend,
				{
					tr::lng_oblivion_photo_digicam_blend_add,
					tr::lng_oblivion_photo_digicam_blend_overlay,
				}),
			FxInt(
				"shadows",
				tr::lng_oblivion_photo_digicam_noise_shadows,
				0,
				100,
				0,
				u"%"_q),
			FxSeed(),
		},
		.flags = kFxNeighbours | kFxSeeded,
		.order = 30,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageNoise(image, {
				.amount = Percent(params, "amount"),
				.color = Percent(params, "color"),
				.cell = float(params.number("size")),
				.hot = Percent(params, "hot"),
				.banding = Percent(params, "banding"),
				.seed = LofiSeed(params, context, 0x2015EU),
				.overlay = (params.integer("blend") == 1),
				.shadows = Percent(params, "shadows"),
			}, LofiUnit(context));
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0
				&& params.integer("color") <= 0
				&& params.integer("hot") <= 0
				&& params.integer("banding") <= 0;
		},
	});
	RegisterFx({
		.id = "lofi.aberration",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_aberration,
		.params = {
			FxPixels(
				"amount",
				tr::lng_oblivion_photo_lofi_amount,
				0.,
				60.,
				8.,
				1),
			FxInt("fringe", tr::lng_oblivion_photo_lofi_fringe, 0, 100, 0),
			FxPoint("center", tr::lng_oblivion_photo_lofi_center),
		},
		.flags = kFxNeighbours,
		.order = 40,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageAberration(image, {
				.amount = float(params.number("amount")),
				.center = params.point("center"),
				.fringe = Percent(params, "fringe"),
			}, LofiUnit(context));
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.number("amount") <= 0.
				&& params.integer("fringe") <= 0;
		},
	});
	RegisterFx({
		.id = "lofi.bloom",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_bloom,
		.params = {
			FxInt("amount", tr::lng_oblivion_photo_lofi_glow, 0, 100, 45),
			FxInt(
				"threshold",
				tr::lng_oblivion_photo_lofi_threshold,
				0,
				100,
				65),
			FxPixels(
				"radius",
				tr::lng_oblivion_photo_lofi_radius,
				1.,
				300.,
				30.),
			FxInt(
				"halation",
				tr::lng_oblivion_photo_lofi_halation,
				0,
				100,
				0),
		},
		.flags = kFxNeighbours,
		.order = 50,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageBloom(image, {
				.amount = Percent(params, "amount"),
				.threshold = Percent(params, "threshold"),
				.radius = float(params.number("radius")),
				.halation = Percent(params, "halation"),
			}, LofiUnit(context));
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0
				&& params.integer("halation") <= 0;
		},
	});
	RegisterFx({
		.id = "lofi.flash",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_flash,
		.params = {
			FxInt("amount", tr::lng_oblivion_photo_lofi_amount, 0, 100, 55),
			FxInt(
				"radius",
				tr::lng_oblivion_photo_lofi_radius,
				10,
				150,
				70,
				u"%"_q),
			FxInt("falloff", tr::lng_oblivion_photo_lofi_falloff, 0, 100, 60),
			FxPoint(
				"center",
				tr::lng_oblivion_photo_lofi_center,
				QPointF(0.5, 0.45)),
		},
		.order = 60,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageFlash(image, {
				.amount = Percent(params, "amount"),
				.center = params.point("center"),
				.radius = Percent(params, "radius"),
				.falloff = Percent(params, "falloff"),
			});
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0;
		},
	});
}

void RegisterProcessing() {
	RegisterFx({
		.id = "lofi.cast",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_cast,
		.params = {
			FxInt(
				"temperature",
				tr::lng_oblivion_photo_lofi_temperature,
				-100,
				100,
				-45),
			FxInt("tint", tr::lng_oblivion_photo_lofi_tint, -100, 100, -25),
			FxInt(
				"saturation",
				tr::lng_oblivion_photo_lofi_saturation,
				-100,
				100,
				0),
			FxInt("fade", tr::lng_oblivion_photo_lofi_fade, 0, 100, 25),
		},
		.order = 70,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageCast(image, {
				.temperature = Percent(params, "temperature"),
				.tint = Percent(params, "tint"),
				.saturation = Percent(params, "saturation"),
				.fade = Percent(params, "fade"),
			});
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return !params.integer("temperature")
				&& !params.integer("tint")
				&& !params.integer("saturation")
				&& !params.integer("fade");
		},
	});
	RegisterFx({
		.id = "lofi.sharpen",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_sharpen,
		.params = {
			FxInt(
				"amount",
				tr::lng_oblivion_photo_lofi_amount,
				0,
				400,
				180,
				u"%"_q),
			FxPixels(
				"radius",
				tr::lng_oblivion_photo_lofi_radius,
				0.5,
				20.,
				2.,
				1),
		},
		.flags = kFxNeighbours,
		.order = 80,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageSharpen(
				image,
				Percent(params, "amount"),
				float(params.number("radius")),
				LofiUnit(context));
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0;
		},
	});
	RegisterFx({
		.id = "lofi.stamp",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_stamp,
		.params = {
			FxCustom(
				"text",
				tr::lng_oblivion_photo_lofi_stamp_text,
				kTextEditorType,
				kDefaultStampText),
			FxChoice(
				"style",
				tr::lng_oblivion_photo_lofi_stamp_font,
				FontNames()),
			FxColor(
				"color",
				tr::lng_oblivion_photo_lofi_stamp_color,
				QColor(255, 151, 41)),
			FxChoice(
				"corner",
				tr::lng_oblivion_photo_lofi_stamp_corner,
				CornerNames()),
			FxFloat(
				"size",
				tr::lng_oblivion_photo_lofi_size,
				1.,
				15.,
				4.5,
				1,
				u"%"_q),
			FxInt("glow", tr::lng_oblivion_photo_lofi_stamp_glow, 0, 100, 45),
			FxFloat(
				"margin",
				tr::lng_oblivion_photo_lofi_stamp_margin,
				0.,
				20.,
				4.,
				1,
				u"%"_q),
		},
		.order = 90,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			auto stamp = StampFrom(params, "text", "style", "color", "corner");
			stamp.size = params.number("size") / 100.;
			stamp.glow = params.number("glow") / 100.;
			stamp.margin = params.number("margin") / 100.;
			LofiPaintStamp(image, stamp);
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return StampTextOf(params.data("text")).trimmed().isEmpty();
		},
	});
	RegisterFx({
		.id = "lofi.depth",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_depth,
		.params = {
			FxInt("levels", tr::lng_oblivion_photo_lofi_levels, 2, 32, 4),
			FxBool("mono", tr::lng_oblivion_photo_lofi_mono, false),
			FxChoice(
				"dither",
				tr::lng_oblivion_photo_lofi_dither,
				{
					tr::lng_oblivion_photo_lofi_dither_none,
					tr::lng_oblivion_photo_lofi_dither_bayer2,
					tr::lng_oblivion_photo_lofi_dither_bayer4,
					tr::lng_oblivion_photo_lofi_dither_bayer8,
					tr::lng_oblivion_photo_lofi_dither_fs,
					tr::lng_oblivion_photo_lofi_dither_atkinson,
				},
				2),
			FxPixels("dot", tr::lng_oblivion_photo_lofi_dot, 1., 32., 2.),
		},
		.flags = kFxNeighbours,
		.order = 100,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto levels = params.integer("levels");
			StageDepth(image, {
				.levels = { levels, levels, levels },
				.mono = params.boolean("mono"),
				.dither = Dither(std::clamp(params.integer("dither"), 0, 5)),
				.cell = float(params.number("dot")),
			}, LofiUnit(context));
			return !context.cancelled();
		},
	});
}

void RegisterScreens() {
	RegisterFx({
		.id = "lofi.scanlines",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_scanlines,
		.params = {
			FxInt("amount", tr::lng_oblivion_photo_lofi_amount, 0, 100, 45),
			FxPixels("pitch", tr::lng_oblivion_photo_lofi_pitch, 2., 64., 4.),
			FxPixels("comb", tr::lng_oblivion_photo_lofi_comb, 0., 100., 0.),
		},
		.flags = kFxNeighbours,
		.order = 110,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageScanlines(image, {
				.amount = Percent(params, "amount"),
				.pitch = float(params.number("pitch")),
				.comb = float(params.number("comb")),
			}, LofiUnit(context));
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0
				&& params.number("comb") <= 0.;
		},
	});
	RegisterFx({
		.id = "lofi.vhs",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_vhs,
		.params = {
			FxInt("bleed", tr::lng_oblivion_photo_lofi_vhs_bleed, 0, 100, 60),
			FxInt("soft", tr::lng_oblivion_photo_lofi_vhs_soft, 0, 100, 50),
			FxInt(
				"tracking",
				tr::lng_oblivion_photo_lofi_vhs_tracking,
				0,
				100,
				40),
			FxInt("noise", tr::lng_oblivion_photo_lofi_vhs_noise, 0, 100, 35),
			FxInt(
				"wobble",
				tr::lng_oblivion_photo_lofi_vhs_wobble,
				0,
				100,
				25),
			FxSeed(),
		},
		.flags = kFxNeighbours | kFxSeeded,
		.order = 120,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageVhs(image, {
				.bleed = Percent(params, "bleed"),
				.soft = Percent(params, "soft"),
				.tracking = Percent(params, "tracking"),
				.noise = Percent(params, "noise"),
				.wobble = Percent(params, "wobble"),
				.seed = LofiSeed(params, context, 0x7A9EU),
			});
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("bleed") <= 0
				&& params.integer("soft") <= 0
				&& params.integer("tracking") <= 0
				&& params.integer("noise") <= 0
				&& params.integer("wobble") <= 0;
		},
	});
	RegisterFx({
		.id = "lofi.pixelate",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_pixelate,
		.params = {
			FxPixels("size", tr::lng_oblivion_photo_lofi_size, 2., 256., 16.),
			FxChoice(
				"shape",
				tr::lng_oblivion_photo_lofi_shape,
				{
					tr::lng_oblivion_photo_lofi_shape_squares,
					tr::lng_oblivion_photo_lofi_shape_dots,
					tr::lng_oblivion_photo_lofi_shape_lcd,
				}),
			FxInt("amount", tr::lng_oblivion_photo_lofi_amount, 0, 100, 100),
		},
		.flags = kFxNeighbours,
		.order = 130,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StagePixelate(image, {
				.size = float(params.number("size")),
				.shape = PixelShape(std::clamp(params.integer("shape"), 0, 2)),
				.amount = Percent(params, "amount"),
			}, LofiUnit(context));
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0;
		},
	});
	RegisterFx({
		.id = "lofi.posterize",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_posterize,
		.params = {
			FxInt("levels", tr::lng_oblivion_photo_lofi_levels, 2, 32, 5),
			FxInt("amount", tr::lng_oblivion_photo_lofi_amount, 0, 100, 100),
		},
		.order = 140,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StagePosterize(
				image,
				params.integer("levels"),
				Percent(params, "amount"));
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0;
		},
	});
	const auto oneInk = [](const FxParams &params) {
		return params.integer("mode") == 0;
	};
	RegisterFx({
		.id = "lofi.halftone",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_halftone,
		.params = {
			FxPixels("size", tr::lng_oblivion_photo_lofi_size, 3., 120., 12.),
			FxAngle("angle", tr::lng_oblivion_photo_lofi_angle, 45., -90., 90.),
			FxChoice(
				"mode",
				tr::lng_oblivion_photo_lofi_mode,
				{
					tr::lng_oblivion_photo_lofi_mode_ink,
					tr::lng_oblivion_photo_lofi_mode_color,
				}),
			FxColor(
				"ink",
				tr::lng_oblivion_photo_lofi_ink,
				QColor(26, 26, 26)).when(oneInk),
			FxColor(
				"paper",
				tr::lng_oblivion_photo_lofi_paper,
				QColor(244, 239, 228)).when(oneInk),
			FxInt("amount", tr::lng_oblivion_photo_lofi_amount, 0, 100, 100),
		},
		.flags = kFxNeighbours,
		.order = 150,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageHalftone(image, {
				.size = float(params.number("size")),
				.angle = float(params.number("angle")),
				.color = (params.integer("mode") == 1),
				.ink = params.color("ink"),
				.paper = params.color("paper"),
				.amount = Percent(params, "amount"),
			}, LofiUnit(context));
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0;
		},
	});
}

void RegisterPresets() {
	const auto whole = [](int value) {
		return FxValue::Integer(value);
	};
	RegisterFxPreset({
		.id = "lofi.preset_phone",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_preset_phone,
		.stack = {
			MakeFx("lofi.camera", {
				{ "side", whole(640) },
				{ "aberration", whole(10) },
				{ "bloom", whole(12) },
				{ "temperature", whole(-14) },
				{ "tint", whole(-10) },
				{ "saturation", whole(-8) },
				{ "noise", whole(40) },
				{ "chroma", whole(50) },
				{ "hot", whole(4) },
				{ "sharpen", whole(50) },
				{ "quality", whole(32) },
				{ "generations", whole(2) },
			}),
		},
		.order = 0,
	});
	RegisterFxPreset({
		.id = "lofi.preset_ccd",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_preset_ccd,
		.stack = {
			MakeFx("lofi.camera", {
				{ "side", whole(1280) },
				{ "aberration", whole(30) },
				{ "bloom", whole(30) },
				{ "halation", whole(15) },
				{ "flash", whole(22) },
				{ "temperature", whole(8) },
				{ "saturation", whole(18) },
				{ "noise", whole(18) },
				{ "chroma", whole(22) },
				{ "hot", whole(8) },
				{ "sharpen", whole(70) },
				{ "quality", whole(74) },
				{ "generations", whole(1) },
				{ "stamp", FxValue::Boolean(true) },
				{ "stamp_text", FxValue::Data("'08 7 26") },
			}),
		},
		.order = 1,
	});
	RegisterFxPreset({
		.id = "lofi.preset_webcam",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_preset_webcam,
		.stack = {
			MakeFx("lofi.camera", {
				{ "side", whole(320) },
				{ "aberration", whole(15) },
				{ "bloom", whole(25) },
				{ "temperature", whole(-20) },
				{ "tint", whole(-18) },
				{ "saturation", whole(-20) },
				{ "noise", whole(55) },
				{ "chroma", whole(65) },
				{ "hot", whole(0) },
				{ "sharpen", whole(25) },
				{ "quality", whole(38) },
				{ "generations", whole(1) },
			}),
		},
		.order = 2,
	});
	RegisterFxPreset({
		.id = "lofi.preset_mms",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_preset_mms,
		.stack = {
			MakeFx("lofi.camera", {
				{ "side", whole(176) },
				{ "aberration", whole(0) },
				{ "bloom", whole(10) },
				{ "saturation", whole(-5) },
				{ "noise", whole(30) },
				{ "chroma", whole(35) },
				{ "hot", whole(0) },
				{ "sharpen", whole(35) },
				{ "quality", whole(18) },
				{ "generations", whole(3) },
			}),
		},
		.order = 3,
	});
	RegisterFxPreset({
		.id = "lofi.preset_flash",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_preset_flash,
		.stack = {
			MakeFx("lofi.camera", {
				{ "side", whole(1024) },
				{ "aberration", whole(25) },
				{ "bloom", whole(45) },
				{ "halation", whole(30) },
				{ "flash", whole(70) },
				{ "saturation", whole(15) },
				{ "noise", whole(15) },
				{ "chroma", whole(18) },
				{ "sharpen", whole(55) },
				{ "quality", whole(62) },
				{ "stamp", FxValue::Boolean(true) },
				{ "stamp_text", FxValue::Data("31.12.2003") },
			}),
		},
		.order = 4,
	});
	RegisterFxPreset({
		.id = "lofi.preset_pocket",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_preset_pocket,
		.stack = {
			MakeFx("lofi.camera", {
				{ "side", whole(128) },
				{ "upscale", whole(1) },
				{ "aberration", whole(0) },
				{ "bloom", whole(0) },
				{ "saturation", whole(0) },
				{ "noise", whole(20) },
				{ "chroma", whole(0) },
				{ "hot", whole(0) },
				{ "sharpen", whole(60) },
				{ "depth", whole(6) },
				{ "dither", whole(1) },
				{ "generations", whole(0) },
			}),
		},
		.order = 5,
	});
	RegisterFxPreset({
		.id = "lofi.preset_cctv",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_preset_cctv,
		.stack = {
			MakeFx("lofi.camera", {
				{ "side", whole(480) },
				{ "aberration", whole(15) },
				{ "bloom", whole(30) },
				{ "saturation", whole(-100) },
				{ "noise", whole(50) },
				{ "chroma", whole(0) },
				{ "hot", whole(3) },
				{ "sharpen", whole(40) },
				{ "quality", whole(35) },
				{ "generations", whole(2) },
				{ "stamp", FxValue::Boolean(true) },
				{ "stamp_text", FxValue::Data("CAM 02  03:14:07") },
				{ "stamp_style", whole(3) },
				{ "stamp_color", FxValue::Color(QColor(255, 255, 255)) },
				{ "stamp_corner", whole(3) },
			}),
			MakeFx("lofi.scanlines", {
				{ "amount", whole(30) },
				{ "pitch", FxValue::Number(6.) },
			}),
		},
		.order = 6,
	});
	RegisterFxPreset({
		.id = "lofi.preset_vhs",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_lofi_preset_vhs,
		.stack = {
			MakeFx("lofi.sensor", { { "side", whole(720) } }),
			MakeFx("lofi.vhs"),
			MakeFx("lofi.stamp", {
				{ "text", FxValue::Data("PLAY \xE2\x96\xB6  14.08.1996") },
				{ "style", whole(3) },
				{ "color", FxValue::Color(QColor(255, 255, 255)) },
				{ "corner", whole(1) },
				{ "glow", whole(15) },
			}),
			MakeFx("lofi.scanlines", {
				{ "amount", whole(25) },
				{ "pitch", FxValue::Number(5.) },
			}),
		},
		.order = 7,
	});
}

[[nodiscard]] QByteArray NormalizeStampData(const QByteArray &value) {
	return StampTextOf(value).toUtf8();
}

const auto Registered = FxRegistrar([] {
	RegisterFxCustomEditor({
		.type = kTextEditorType,
		.create = CreateStampTextEditor,
		.normalize = NormalizeStampData,
	});
	RegisterCamera();
	RegisterOptics();
	RegisterProcessing();
	RegisterScreens();
	RegisterPresets();
});

//
// Self-test.
//

[[nodiscard]] bool LofiSamePixels(const QImage &a, const QImage &b) {
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

[[nodiscard]] bool LofiValidPixels(const QImage &image) {
	if (image.format() != QImage::Format_ARGB32_Premultiplied) {
		return false;
	}
	for (auto y = 0; y != image.height(); ++y) {
		const auto line = FxRow(image, y);
		for (auto x = 0; x != image.width(); ++x) {
			const auto p = line[x];
			const auto a = (p >> 24);
			if (((p >> 16) & 0xFFU) > a
				|| ((p >> 8) & 0xFFU) > a
				|| (p & 0xFFU) > a) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] double LofiMeanLuma(const QImage &image) {
	auto sum = 0.;
	for (auto y = 0; y != image.height(); ++y) {
		const auto line = FxRow(image, y);
		for (auto x = 0; x != image.width(); ++x) {
			const auto c = FxUnpack(line[x]);
			sum += FxLuma(c.r, c.g, c.b);
		}
	}
	return sum / std::max(image.width() * image.height(), 1);
}

[[nodiscard]] bool RunLofiSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto info = [&](const QString &what) {
		log.push_back(u"   "_q + what);
	};
	const auto full = FxTestImage(640, 480);
	const auto cut = FxTestImage(320, 240, true);
	const auto context = FxContext{
		.scale = 1.,
		.seed = 4242,
		.fullSize = full.size(),
	};
	const auto run = [&](
			const FxInstance &instance,
			const QImage &source,
			FxContext used) {
		auto result = source;
		used.fullSize = source.size();
		return ApplyFx(result, instance, used) ? result : QImage();
	};
	const auto ids = std::vector<QByteArray>{
		"lofi.camera",
		"lofi.sensor",
		"lofi.jpeg",
		"lofi.noise",
		"lofi.aberration",
		"lofi.bloom",
		"lofi.flash",
		"lofi.cast",
		"lofi.sharpen",
		"lofi.stamp",
		"lofi.depth",
		"lofi.scanlines",
		"lofi.vhs",
		"lofi.pixelate",
		"lofi.posterize",
		"lofi.halftone",
	};

	// Every effect: registered, changes the picture, keeps valid pixels
	// of a picture with transparency, does the same thing twice.
	{
		auto registered = true;
		auto changes = true;
		auto valid = true;
		auto repeats = true;
		auto timer = QElapsedTimer();
		auto timings = QStringList();
		for (const auto &id : ids) {
			const auto name = QString::fromLatin1(id);
			const auto descriptor = FindFx(id);
			if (!descriptor || descriptor->group != FxGroup::Lofi) {
				registered = false;
				info(name + u" is not registered"_q);
				continue;
			}
			// The test picture is as large as the default sensor.
			const auto instance = (id == "lofi.sensor")
				? MakeFx(id, { { "side", FxValue::Integer(200) } })
				: MakeFx(id);
			timer.start();
			const auto first = run(instance, full, context);
			timings.push_back(u"%1 %2"_q.arg(
				name.mid(5),
				QString::number(timer.nsecsElapsed() / 1e6, 'f', 1)));
			if (first.isNull() || FxImageDifference(first, full) < 0.05) {
				changes = false;
				info(name + u" does not change the picture"_q);
			}
			if (!LofiSamePixels(first, run(instance, full, context))) {
				repeats = false;
				info(name + u" is not deterministic"_q);
			}
			const auto transparent = run(instance, cut, context);
			if (transparent.isNull()
				|| transparent.size() != cut.size()
				|| !LofiValidPixels(transparent)) {
				valid = false;
				info(name + u" breaks premultiplied pixels"_q);
			}
		}
		check(registered, u"%1 bad camera effects are registered"_q.arg(
			ids.size()));
		check(changes, u"every effect changes the picture by default"_q);
		check(repeats, u"every effect is deterministic"_q);
		check(valid, u"every effect keeps valid premultiplied pixels"_q);
		info(u"640x480, ms: "_q + timings.join(u", "_q));
	}

	// Seeds.
	{
		auto differ = true;
		auto same = true;
		for (const auto id : { "lofi.camera", "lofi.noise", "lofi.vhs" }) {
			const auto one = MakeFx(id, { { "seed", FxValue::Integer(1) } });
			const auto two = MakeFx(id, { { "seed", FxValue::Integer(2) } });
			const auto first = run(one, full, context);
			differ = differ && !LofiSamePixels(first, run(two, full, context));
			same = same && LofiSamePixels(first, run(one, full, context));
			auto other = context;
			other.seed = 99;
			differ = differ && !LofiSamePixels(first, run(one, full, other));
		}
		check(same, u"the same seed gives the same picture"_q);
		check(differ, u"another seed or instance gives another picture"_q);
	}

	// Noise laid over the picture and noise kept in the shadows.
	{
		auto black = QImage(96, 64, QImage::Format_ARGB32_Premultiplied);
		black.fill(QColor(0, 0, 0));
		auto white = black;
		white.fill(QColor(255, 255, 255));
		const auto noise = [&](const QImage &source, int blend, int shadows) {
			return run(MakeFx("lofi.noise", {
				{ "hot", FxValue::Integer(0) },
				{ "blend", FxValue::Integer(blend) },
				{ "shadows", FxValue::Integer(shadows) },
			}), source, context);
		};
		check(
			!LofiSamePixels(noise(black, 0, 0), black)
				&& LofiSamePixels(noise(black, 1, 0), black)
				&& !LofiSamePixels(noise(full, 1, 0), full),
			u"overlaid noise keeps the black clean"_q);
		check(
			!LofiSamePixels(noise(white, 0, 0), white)
				&& LofiSamePixels(noise(white, 0, 100), white)
				&& !LofiSamePixels(noise(black, 0, 100), black),
			u"noise in the shadows keeps the white clean"_q);
	}

	// Zero strength.
	{
		const auto zero = [](int value = 0) {
			return FxValue::Integer(value);
		};
		const auto none = std::vector<FxInstance>{
			MakeFx("lofi.noise", {
				{ "amount", zero() },
				{ "color", zero() },
				{ "hot", zero() },
			}),
			MakeFx("lofi.aberration", { { "amount", FxValue::Number(0.) } }),
			MakeFx("lofi.bloom", { { "amount", zero() } }),
			MakeFx("lofi.flash", { { "amount", zero() } }),
			MakeFx("lofi.cast", {
				{ "temperature", zero() },
				{ "tint", zero() },
				{ "fade", zero() },
			}),
			MakeFx("lofi.sharpen", { { "amount", zero() } }),
			MakeFx("lofi.stamp", { { "text", FxValue::Data("  ") } }),
			MakeFx("lofi.scanlines", { { "amount", zero() } }),
			MakeFx("lofi.vhs", {
				{ "bleed", zero() },
				{ "soft", zero() },
				{ "tracking", zero() },
				{ "noise", zero() },
				{ "wobble", zero() },
			}),
			MakeFx("lofi.pixelate", { { "amount", zero() } }),
			MakeFx("lofi.posterize", { { "amount", zero() } }),
			MakeFx("lofi.halftone", { { "amount", zero() } }),
		};
		auto identity = true;
		for (const auto &instance : none) {
			auto image = full;
			const auto fine = FxIsIdentity(instance)
				&& ApplyFx(image, instance, context)
				&& (image.cacheKey() == full.cacheKey());
			if (!fine) {
				identity = false;
				info(QString::fromLatin1(instance.id)
					+ u" is not an identity at zero"_q);
			}
		}
		check(identity, u"zero strength changes nothing (%1 effects)"_q.arg(
			none.size()));
		// A sensor that is larger than the photo has nothing to do.
		check(
			LofiSamePixels(
				run(
					MakeFx("lofi.sensor", { { "side", zero(4096) } }),
					full,
					context),
				full),
			u"a sensor larger than the photo changes nothing"_q);
	}

	// Presets only set parameters.
	{
		auto count = 0;
		auto fine = true;
		for (const auto preset : AllFxPresets()) {
			if (!preset->id.startsWith("lofi.")) {
				continue;
			}
			++count;
			fine = fine && !preset->stack.empty() && preset->stack.size() <= 6;
			for (const auto &instance : preset->stack) {
				const auto descriptor = FindFx(instance.id);
				if (!descriptor
					|| !instance.enabled
					|| instance.uid
					|| !(NormalizedFxParams(*descriptor, instance.params)
						== instance.params)) {
					fine = false;
					info(u"preset %1: bad instance %2"_q.arg(
						QString::fromLatin1(preset->id),
						QString::fromLatin1(instance.id)));
				}
			}
			auto image = full;
			if (!ApplyFxStack(image, preset->stack, context)
				|| FxImageDifference(image, full) < 0.5) {
				fine = false;
				info(u"preset %1 does not change the picture"_q.arg(
					QString::fromLatin1(preset->id)));
			}
		}
		check(
			count == 8 && fine,
			u"%1 presets are valid parameter sets of registered effects"_q.arg(
				count));
	}

	// JPEG.
	if (LofiJpegAvailable()) {
		const auto compressed = [&](int quality, int generations) {
			auto image = full;
			return LofiJpegRoundTrip(image, quality, generations)
				? image
				: QImage();
		};
		const auto good = FxImageDifference(compressed(90, 1), full);
		const auto bad = FxImageDifference(compressed(10, 1), full);
		const auto worse = FxImageDifference(compressed(10, 8), full);
		check(
			good > 0. && good < bad && bad < worse,
			u"JPEG loses more at a lower quality and with re-saves "
			"(%1 < %2 < %3)"_q.arg(
				QString::number(good, 'f', 2),
				QString::number(bad, 'f', 2),
				QString::number(worse, 'f', 2)));
		auto transparent = cut;
		const auto done = LofiJpegRoundTrip(transparent, 20, 3);
		auto alphaKept = done && LofiValidPixels(transparent);
		for (auto y = 0; alphaKept && y != cut.height(); ++y) {
			for (auto x = 0; x != cut.width(); ++x) {
				if ((FxRow(transparent, y)[x] >> 24)
					!= (FxRow(cut, y)[x] >> 24)) {
					alphaKept = false;
					break;
				}
			}
		}
		check(alphaKept, u"JPEG keeps the alpha channel as it is"_q);
		const auto cancel = std::atomic<bool>(true);
		auto image = full;
		check(
			!LofiJpegRoundTrip(image, 50, 2, &cancel),
			u"a cancelled JPEG round trip reports it"_q);
		auto timer = QElapsedTimer();
		timer.start();
		auto large = FxTestImage(2048, 1536);
		const auto fine = LofiJpegRoundTrip(large, 40, 1);
		info(u"JPEG round trip of 2048x1536: %1 ms%2"_q.arg(
			QString::number(timer.nsecsElapsed() / 1e6, 'f', 1),
			fine ? QString() : u" (failed)"_q));
	} else {
		info(u"the JPEG plugin is not available, JPEG stages are skipped"_q);
	}

	// The camera and the sensor work at their own resolution, so the
	// preview is the export, only smaller.
	{
		const auto large = FxTestImage(1600, 1200);
		const auto half = FxResized(large, large.size() / 2);
		const auto compare = [&](const FxInstance &instance) {
			auto exported = large;
			auto preview = half;
			const auto done = ApplyFx(exported, instance, FxContext{
				.scale = 1.,
				.seed = 7,
				.fullSize = large.size(),
			}) && ApplyFx(preview, instance, FxContext{
				.scale = 0.5,
				.seed = 7,
				.fullSize = large.size(),
				.preview = true,
			});
			return done
				? FxImageDifference(preview, FxResized(exported, half.size()))
				: 255.;
		};
		const auto camera = compare(MakeFx("lofi.camera", {
			{ "side", FxValue::Integer(480) },
		}));
		const auto sensor = compare(MakeFx("lofi.sensor", {
			{ "side", FxValue::Integer(320) },
		}));
		const auto pixels = compare(MakeFx("lofi.pixelate", {
			{ "size", FxValue::Number(40.) },
		}));
		const auto stamp = compare(MakeFx("lofi.stamp"));
		check(
			camera < 6. && sensor < 3. && pixels < 3. && stamp < 1.,
			u"the preview matches the export (camera %1, sensor %2, "
			"pixelate %3, stamp %4)"_q.arg(
				QString::number(camera, 'f', 2),
				QString::number(sensor, 'f', 2),
				QString::number(pixels, 'f', 2),
				QString::number(stamp, 'f', 2)));
	}

	// Upscaling.
	{
		auto flat = QImage(40, 30, QImage::Format_ARGB32_Premultiplied);
		flat.fill(QColor(120, 60, 200));
		const auto grown = UpscaleBicubic(flat, QSize(133, 97));
		auto constant = !grown.isNull() && (grown.size() == QSize(133, 97));
		for (auto y = 0; constant && y != grown.height(); ++y) {
			for (auto x = 0; x != grown.width(); ++x) {
				if (FxRow(grown, y)[x] != FxRow(flat, 0)[0]) {
					constant = false;
					break;
				}
			}
		}
		check(constant, u"bicubic upscaling keeps a flat color"_q);
		const auto sharp = UpscaleBicubic(cut, cut.size() * 3);
		check(
			!sharp.isNull() && LofiValidPixels(sharp),
			u"bicubic upscaling keeps valid premultiplied pixels"_q);
	}

	// Color depth.
	{
		const auto levelsOf = [](const QImage &image) {
			auto seen = std::array<bool, 256>{};
			for (auto y = 0; y != image.height(); ++y) {
				const auto line = FxRow(image, y);
				for (auto x = 0; x != image.width(); ++x) {
					seen[(line[x] >> 8) & 0xFFU] = true;
				}
			}
			return int(std::count(begin(seen), end(seen), true));
		};
		auto exact = true;
		auto bright = true;
		const auto before = LofiMeanLuma(full);
		for (auto dither = 0; dither != 6; ++dither) {
			const auto image = run(MakeFx("lofi.depth", {
				{ "levels", FxValue::Integer(4) },
				{ "dither", FxValue::Integer(dither) },
				{ "dot", FxValue::Number(1.) },
			}), full, context);
			exact = exact && !image.isNull() && (levelsOf(image) <= 4);
			if (dither) {
				bright = bright
					&& (std::abs(LofiMeanLuma(image) - before) < 0.02);
			}
		}
		check(exact, u"color depth leaves the asked number of levels"_q);
		check(bright, u"dithering keeps the mean brightness"_q);
		const auto mono = run(MakeFx("lofi.depth", {
			{ "levels", FxValue::Integer(2) },
			{ "mono", FxValue::Boolean(true) },
			{ "dither", FxValue::Integer(4) },
		}), full, context);
		auto pure = !mono.isNull();
		for (auto y = 0; pure && y != mono.height(); ++y) {
			for (auto x = 0; x != mono.width(); ++x) {
				const auto p = FxRow(mono, y)[x] & 0x00FFFFFFU;
				if (p != 0U && p != 0x00FFFFFFU) {
					pure = false;
					break;
				}
			}
		}
		check(pure, u"one bit black and white has only two colors"_q);
		check(
			BayerThreshold(0, 0, 1) < BayerThreshold(1, 1, 1)
				&& BayerThreshold(1, 1, 1) < BayerThreshold(1, 0, 1)
				&& BayerThreshold(1, 0, 1) < BayerThreshold(0, 1, 1),
			u"the ordered dithering matrix"_q);
	}

	// The date stamp.
	{
		auto glyphs = true;
		for (const auto &glyph : kGlyphs) {
			glyphs = glyphs
				&& (std::strlen(glyph.rows) == kGlyphColumns * kGlyphRows);
		}
		check(glyphs, u"every glyph of the stamp font is 5 x 7"_q);
		const auto cyrillic = QString::fromUtf8(
			"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
		check(
			LofiStampFilter(u"ab \x263A 12:30\n"_q + cyrillic)
				== u"ab  12:30"_q + cyrillic
				&& LofiStampFilter(QString(100, QChar('7'))).size()
					== kMaxStampLength,
			u"the stamp text keeps only what the fonts can draw"_q);
		const auto moment = QDateTime(QDate(2026, 10, 6), QTime(8, 5));
		check(
			LofiStampText(moment, LofiStampDate::Classic) == u"'26 10 6"_q
				&& LofiStampText(moment, LofiStampDate::Date)
					== u"06.10.2026"_q
				&& LofiStampText(moment, LofiStampDate::DateTime)
					== u"06.10.2026 08:05"_q,
			u"date formats of the stamp"_q);
		auto corners = true;
		auto styles = true;
		for (auto corner = 0; corner != kLofiStampCornerCount; ++corner) {
			for (auto style = 0; style != kLofiStampStyleCount; ++style) {
				auto image = full;
				LofiPaintStamp(image, {
					.text = u"'98 12 25"_q,
					.style = LofiStampStyle(style),
					.corner = LofiStampCorner(corner),
					.glow = 0.,
				});
				auto inside = 0;
				auto outside = 0;
				const auto onLeft = (corner == 1 || corner == 3);
				const auto onTop = (corner == 2 || corner == 3);
				for (auto y = 0; y != image.height(); ++y) {
					for (auto x = 0; x != image.width(); ++x) {
						if (FxRow(image, y)[x] == FxRow(full, y)[x]) {
							continue;
						}
						const auto quarter = ((x < image.width() / 2) == onLeft)
							&& ((y < image.height() / 2) == onTop);
						++(quarter ? inside : outside);
					}
				}
				corners = corners && (outside == 0);
				styles = styles && (inside > 60);
			}
		}
		check(corners, u"the stamp stays in its corner"_q);
		check(styles, u"every stamp font draws the date"_q);
		auto one = full;
		auto seven = full;
		LofiPaintStamp(one, { .text = u"1"_q });
		LofiPaintStamp(seven, { .text = u"7"_q });
		check(!LofiSamePixels(one, seven), u"digits differ"_q);
		check(
			StampCaretAfterFilter(u"12;"_q) == 2
				&& StampCaretAfterFilter(u"12"_q) == 2
				&& StampCaretAfterFilter(u";\x263A@"_q) == 0
				&& StampCaretAfterFilter(QString(100, QChar('7')))
					== kMaxStampLength,
			u"the caret stays behind the kept characters"_q);

		// A text several times wider than the picture is cut to the part
		// over the picture and the cut is not seen: a half of a wide
		// picture gets the same pixels as that half of the whole one.
		const auto wide = FxTestImage(1200, 480);
		auto painted = true;
		auto whole = true;
		const auto compare = [&](LofiStamp stamp, bool onLeft) {
			stamp.text = QString(kMaxStampLength, QChar('8'));
			stamp.corner = onLeft
				? LofiStampCorner::BottomLeft
				: LofiStampCorner::BottomRight;
			const auto half = QRect(onLeft ? 0 : 600, 0, 600, 480);
			auto all = wide;
			auto part = wide.copy(half);
			LofiPaintStamp(all, stamp);
			LofiPaintStamp(part, stamp);
			painted = painted && !LofiSamePixels(part, wide.copy(half));
			whole = whole && LofiSamePixels(part, all.copy(half));
		};
		for (auto style = 0; style != kLofiStampStyleCount; ++style) {
			for (const auto onLeft : { false, true }) {
				// Wider than 16384 pixels in the bold font.
				compare({
					.style = LofiStampStyle(style),
					.size = 0.9,
					.glow = 0.,
				}, onLeft);
				compare({
					.style = LofiStampStyle(style),
					.size = 0.2,
				}, onLeft);
			}
		}
		check(painted, u"a stamp wider than the picture is painted"_q);
		check(whole, u"a cut stamp is a part of the whole one"_q);
	}

	// Timings on a larger picture.
	{
		const auto large = FxTestImage(2048, 1536);
		auto timer = QElapsedTimer();
		auto timings = QStringList();
		for (const auto &id : ids) {
			auto image = large;
			timer.start();
			const auto done = ApplyFx(image, MakeFx(id), FxContext{
				.scale = 1.,
				.seed = 1,
				.fullSize = large.size(),
			});
			timings.push_back(u"%1 %2%3"_q.arg(
				QString::fromLatin1(id).mid(5),
				QString::number(timer.nsecsElapsed() / 1e6, 'f', 0),
				done ? QString() : u" (failed)"_q));
		}
		info(u"2048x1536, ms: "_q + timings.join(u", "_q));
	}
	return ok;
}

const auto LofiSelfTest = SelfTestRegistrar(
	SelfTestSuite::Fx,
	"lofi",
	&RunLofiSelfTest);

} // namespace

// Interface: the text field of the date stamp and the snapshot scenes.

namespace {

using namespace EditorUi;

constexpr auto kEditorRowHeight = 40;
constexpr auto kEditorFieldHeight = 28;
constexpr auto kEditorLabelSkip = 12;
constexpr auto kEditorChipsBottom = 8;
constexpr auto kGalleryPadding = 16;
constexpr auto kGallerySkip = 12;
constexpr auto kGalleryLabelSkip = 6;
constexpr auto kGalleryColumns = 4;
constexpr auto kGalleryWidth = 1040;
constexpr auto kGalleryRadius = 8;
constexpr auto kPanelSceneWidth = 340;
// The padding of the editor panel plus the padding of an effect card: the
// parameters get exactly the width they have in a card of the editor.
constexpr auto kPanelScenePadding = 28;
constexpr auto kFontsSceneWidth = 560;
constexpr auto kFontsStripHeight = 72;
constexpr auto kSampleLongSide = 1280;

// The editor is dark in every theme, so nothing here may come from the
// colors of the window: the selection, the placeholder and the context
// menu of the field are the dark ones too.
[[nodiscard]] const style::InputField &StampFieldStyle() {
	static const auto result = [] {
		auto st = st::defaultInputField;
		st.textBg = st::groupCallBg;
		st.textBgActive = st::groupCallBg;
		st.textFg = st::groupCallMembersFg;
		st.textMarkBg = st::groupCallMembersBgOver;
		st.textMargins = QMargins(Px(8), Px(5), Px(8), Px(3));
		st.textAlign = style::al_left;
		st.placeholderFg = st::groupCallMemberNotJoinedStatus;
		st.placeholderFgActive = st::groupCallMemberNotJoinedStatus;
		st.placeholderFgError = st::groupCallMemberNotJoinedStatus;
		st.placeholderMargins = QMargins();
		st.placeholderAlign = style::al_topleft;
		st.placeholderScale = 0.;
		st.placeholderShift = 0;
		st.placeholderFont = st::normalFont;
		st.borderFg = st::groupCallMemberInactiveIcon;
		st.borderFgActive = st::groupCallActiveFg;
		st.borderFgError = st::groupCallActiveFg;
		st.border = std::max(Px(1), 1);
		st.borderActive = st.border;
		st.borderRadius = Px(6);
		st.borderDenominator = 1;
		st.style = st::defaultTextStyle;
		st.menu = st::groupCallPopupMenu;
		st.width = 0;
		st.widthMin = 0;
		st.heightMin = Px(kEditorFieldHeight);
		st.heightMax = Px(kEditorFieldHeight);
		return st;
	}();
	return result;
}

// What is typed into the field stays with it: Escape and Enter that the
// field has handled, and the keys it had nothing to do with (Backspace in
// an empty field, an arrow at the end of the text), must not reach the
// shortcuts of the editor and of its tools, where Escape closes the
// editor and Backspace deletes a layer or a photo of a collage. Only the
// shortcuts with Cmd / Ctrl (save, zoom) go on.
class StampFieldHost final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

protected:
	void keyPressEvent(QKeyEvent *e) override {
		const auto key = e->key();
		if (key == Qt::Key_Escape
			|| key == Qt::Key_Return
			|| key == Qt::Key_Enter
			|| !(e->modifiers() & Qt::ControlModifier)) {
			e->accept();
			return;
		}
		RpWidget::keyPressEvent(e);
	}

};

void FocusEditorFrom(not_null<QWidget*> from) {
	for (auto parent = from->parentWidget()
		; parent
		; parent = parent->parentWidget()) {
		if (parent->focusPolicy() == Qt::StrongFocus) {
			parent->setFocus();
			return;
		}
	}
}

class StampTextEditor final : public Ui::RpWidget {
public:
	StampTextEditor(QWidget *parent, FxCustomEditorArgs &&args);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void showText(const QString &text, int caret = -1);
	void fieldChanged();
	void commit();
	void insertDate(LofiStampDate format);

	const QString _label;
	const Fn<void(QByteArray value, bool finished)> _changed;
	QString _text;
	bool _dirty = false;
	bool _showing = false;
	StampFieldHost *_host = nullptr;
	Ui::InputField *_field = nullptr;
	ChipsFlow *_chips = nullptr;

};

StampTextEditor::StampTextEditor(QWidget *parent, FxCustomEditorArgs &&args)
: RpWidget(parent)
, _label(tr::lng_oblivion_photo_lofi_stamp_text(tr::now))
, _changed(std::move(args.changed))
, _text(StampTextOf(args.value)) {
	_host = Ui::CreateChild<StampFieldHost>(this);
	_field = Ui::CreateChild<Ui::InputField>(
		_host,
		StampFieldStyle(),
		Ui::InputField::Mode::SingleLine,
		tr::lng_oblivion_photo_lofi_stamp_hint(),
		_text);
	_field->setMaxLength(kMaxStampLength);
	_field->changes() | rpl::on_next([=] {
		fieldChanged();
	}, _field->lifetime());
	_field->submits() | rpl::on_next([=] {
		commit();
		FocusEditorFrom(this);
	}, _field->lifetime());
	_field->cancelled() | rpl::on_next([=] {
		commit();
		FocusEditorFrom(this);
	}, _field->lifetime());
	_field->focusedChanges() | rpl::filter(
		!rpl::mappers::_1
	) | rpl::on_next([=] {
		commit();
	}, _field->lifetime());
	_host->show();
	_field->show();

	_chips = Ui::CreateChild<ChipsFlow>(this);
	_chips->addChip(tr::lng_oblivion_photo_lofi_stamp_today(), [=] {
		insertDate(LofiStampDate::Date);
	});
	_chips->addChip(tr::lng_oblivion_photo_lofi_stamp_now(), [=] {
		insertDate(LofiStampDate::DateTime);
	});
	_chips->addChip(tr::lng_oblivion_photo_lofi_stamp_classic(), [=] {
		insertDate(LofiStampDate::Classic);
	});
	_chips->show();

	if (args.values) {
		std::move(args.values) | rpl::on_next([=](const QByteArray &value) {
			const auto text = StampTextOf(value);
			if (text != _text) {
				_text = text;
				_dirty = false;
				showText(text);
			}
		}, lifetime());
	}
}

void StampTextEditor::showText(const QString &text, int caret) {
	if (_field->getLastText() == text) {
		return;
	}
	_showing = true;
	const auto position = (caret >= 0)
		? caret
		: _field->textCursor().position();
	_field->setText(text);
	_field->setCursorPosition(std::min(position, int(text.size())));
	_showing = false;
}

void StampTextEditor::fieldChanged() {
	if (_showing) {
		return;
	}
	const auto typed = _field->getLastText();
	const auto text = LofiStampFilter(typed);
	if (text != typed) {
		// The field is in the middle of its own change handling here.
		InvokeQueued(this, [=] {
			const auto before = _field->getTextWithTagsPart(
				0,
				_field->textCursor().position()).text;
			showText(
				LofiStampFilter(_field->getLastText()),
				StampCaretAfterFilter(before));
		});
	}
	if (text == _text) {
		return;
	}
	_text = text;
	_dirty = true;
	if (_changed) {
		_changed(_text.toUtf8(), false);
	}
}

void StampTextEditor::commit() {
	if (!_dirty) {
		return;
	}
	_dirty = false;
	if (_changed) {
		_changed(_text.toUtf8(), true);
	}
}

void StampTextEditor::insertDate(LofiStampDate format) {
	const auto text = LofiStampText(QDateTime::currentDateTime(), format);
	showText(text);
	if (text == _text && !_dirty) {
		return;
	}
	_text = text;
	_dirty = false;
	if (_changed) {
		_changed(_text.toUtf8(), true);
	}
}

void StampTextEditor::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto &font = st::normalFont;
	const auto available = std::max(_host->x() - Px(kEditorLabelSkip), 0);
	p.setFont(font);
	p.setPen(st::groupCallMembersFg);
	p.drawText(
		QRect(0, 0, available, Px(kEditorRowHeight)),
		Qt::AlignLeft | Qt::AlignVCenter,
		font->elided(_label, available));
}

int StampTextEditor::resizeGetHeight(int newWidth) {
	const auto row = Px(kEditorRowHeight);
	const auto fieldHeight = Px(kEditorFieldHeight);
	const auto labelWidth = std::min(
		st::normalFont->width(_label) + Px(kEditorLabelSkip),
		newWidth / 2);
	const auto fieldWidth = std::max(newWidth - labelWidth, 0);
	_host->setGeometry(
		labelWidth,
		(row - fieldHeight) / 2,
		fieldWidth,
		fieldHeight);
	_field->setGeometry(0, 0, fieldWidth, fieldHeight);
	_chips->resizeToWidth(newWidth);
	_chips->moveToLeft(0, row, newWidth);
	return row + _chips->height() + Px(kEditorChipsBottom);
}

object_ptr<Ui::RpWidget> CreateStampTextEditor(
		not_null<QWidget*> parent,
		FxCustomEditorArgs &&args) {
	return object_ptr<StampTextEditor>(parent, std::move(args));
}

struct GalleryTile {
	QString title;
	QImage image;
};

// A tile of a contact sheet that is not "every effect of a group": a
// stack of effects under its own title.
struct GallerySource {
	QString title;
	std::vector<FxInstance> stack;
};

class FxGallery final : public Ui::RpWidget {
public:
	FxGallery(QWidget *parent, FxGroup group, bool presets);
	FxGallery(QWidget *parent, std::vector<GallerySource> sources);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] QSize tileSize(int width) const;
	[[nodiscard]] int labelHeight() const;
	void build(QSize tile);

	const FxGroup _group = FxGroup::Lofi;
	const bool _presets = false;
	const std::vector<GallerySource> _sources;
	const QImage _source;
	std::vector<GalleryTile> _tiles;
	QSize _built;

};

FxGallery::FxGallery(QWidget *parent, FxGroup group, bool presets)
: RpWidget(parent)
, _group(group)
, _presets(presets)
, _source(FxSceneSampleImage()) {
}

FxGallery::FxGallery(QWidget *parent, std::vector<GallerySource> sources)
: RpWidget(parent)
, _sources(std::move(sources))
, _source(FxSceneSampleImage()) {
}

QSize FxGallery::tileSize(int width) const {
	const auto tile = (width
		- 2 * Px(kGalleryPadding)
		- (kGalleryColumns - 1) * Px(kGallerySkip)) / kGalleryColumns;
	if (tile <= 0 || _source.isNull()) {
		return QSize();
	}
	return QSize(
		tile,
		std::max(tile * _source.height() / _source.width(), 1));
}

int FxGallery::labelHeight() const {
	return SmallFont()->height + 2 * Px(kGalleryLabelSkip);
}

void FxGallery::build(QSize tile) {
	if (_built == tile) {
		return;
	}
	_built = tile;
	_tiles.clear();
	if (tile.isEmpty()) {
		return;
	}
	const auto ratio = style::DevicePixelRatio();
	const auto base = FxResized(_source, tile * ratio);
	if (base.isNull()) {
		return;
	}
	const auto context = FxContext{
		.scale = base.width() / double(_source.width()),
		.seed = 77,
		.fullSize = _source.size(),
		.preview = true,
	};
	// The untouched photo goes first: mild effects (a color cast, bloom)
	// can only be judged next to it.
	_tiles.push_back({
		tr::lng_oblivion_photo_filter_original(tr::now),
		base,
	});
	if (!_sources.empty()) {
		for (const auto &source : _sources) {
			auto image = base;
			if (ApplyFxStack(image, source.stack, context)) {
				_tiles.push_back({ source.title, std::move(image) });
			}
		}
	} else if (_presets) {
		for (const auto preset : AllFxPresets()) {
			if (preset->group != _group) {
				continue;
			}
			auto image = base;
			if (ApplyFxStack(image, preset->stack, context)) {
				_tiles.push_back({ preset->name.now(), std::move(image) });
			}
		}
	} else {
		for (const auto descriptor : FxInGroup(_group)) {
			if (descriptor->flags & kFxHidden) {
				continue;
			}
			auto image = base;
			if (ApplyFx(image, MakeFx(descriptor->id), context)) {
				_tiles.push_back({ descriptor->name.now(), std::move(image) });
			}
		}
	}
}

int FxGallery::resizeGetHeight(int newWidth) {
	const auto tile = tileSize(newWidth);
	build(tile);
	const auto rows = (int(_tiles.size()) + kGalleryColumns - 1)
		/ kGalleryColumns;
	return 2 * Px(kGalleryPadding)
		+ rows * (tile.height() + labelHeight())
		+ std::max(rows - 1, 0) * Px(kGallerySkip);
}

void FxGallery::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::groupCallMembersBg);
	const auto tile = tileSize(width());
	if (tile.isEmpty()) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto &font = SmallFont();
	p.setFont(font);
	for (auto i = 0; i != int(_tiles.size()); ++i) {
		const auto column = i % kGalleryColumns;
		const auto row = i / kGalleryColumns;
		const auto rect = QRect(
			Px(kGalleryPadding) + column * (tile.width() + Px(kGallerySkip)),
			Px(kGalleryPadding)
				+ row * (tile.height() + labelHeight() + Px(kGallerySkip)),
			tile.width(),
			tile.height());
		auto clip = QPainterPath();
		clip.addRoundedRect(rect, Px(kGalleryRadius), Px(kGalleryRadius));
		p.save();
		p.setClipPath(clip);
		p.fillRect(rect, st::groupCallBg);
		p.drawImage(rect, _tiles[i].image);
		p.restore();
		p.setPen(st::groupCallMembersFg);
		p.drawText(
			QRect(rect.x(), rect.bottom() + 1, rect.width(), labelHeight()),
			Qt::AlignLeft | Qt::AlignVCenter,
			font->elided(_tiles[i].title, rect.width()));
	}
}

// The four fonts of the date stamp on strips of an evening sky.
class StampFonts final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	struct Row {
		QString title;
		QImage image;
	};

	[[nodiscard]] int labelHeight() const;

	std::vector<Row> _rows;
	int _built = 0;

};

int StampFonts::labelHeight() const {
	return SmallFont()->height + 2 * Px(kGalleryLabelSkip);
}

int StampFonts::resizeGetHeight(int newWidth) {
	const auto width = newWidth - 2 * Px(kGalleryPadding);
	const auto height = Px(kFontsStripHeight);
	if (_built != width && width > 0) {
		_built = width;
		_rows.clear();
		const auto ratio = style::DevicePixelRatio();
		const auto titles = FontNames();
		const auto samples = std::array<QString, kLofiStampStyleCount>{
			u"'98 12 25  18:42"_q,
			QString::fromUtf8(
				"14.08.2005 \xD0\x9B\xD0\x95\xD0\xA2\xD0\x9E"),
			QString::fromUtf8("REC \xE2\x97\x8F 00:12:47"),
			QString::fromUtf8("PLAY \xE2\x96\xB6 SP 0:03:14"),
		};
		for (auto i = 0; i != kLofiStampStyleCount; ++i) {
			auto image = QImage(
				QSize(width, height) * ratio,
				QImage::Format_ARGB32_Premultiplied);
			if (image.isNull()) {
				continue;
			}
			{
				auto p = QPainter(&image);
				// The stamp is in the right corner, the dark end of the
				// sky is there: an orange date on an orange sunset
				// could not be read.
				auto gradient = QLinearGradient(0, 0, image.width(), 0);
				gradient.setColorAt(0., QColor(206, 132, 78));
				gradient.setColorAt(0.4, QColor(120, 82, 96));
				gradient.setColorAt(1., QColor(38, 52, 84));
				p.fillRect(image.rect(), gradient);
			}
			const auto style = LofiStampStyle(i);
			LofiPaintStamp(image, {
				.text = samples[i],
				.style = style,
				.color = (style == LofiStampStyle::Camcorder)
					? QColor(255, 255, 255)
					: QColor(255, 151, 41),
				.corner = LofiStampCorner::BottomRight,
				.size = 0.36,
				.glow = (style == LofiStampStyle::Camcorder) ? 0.1 : 0.5,
				.margin = 0.3,
			});
			_rows.push_back({ titles[i].now(), std::move(image) });
		}
	}
	return 2 * Px(kGalleryPadding)
		+ int(_rows.size()) * (labelHeight() + height)
		+ std::max(int(_rows.size()) - 1, 0) * Px(kGalleryLabelSkip);
}

void StampFonts::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::groupCallMembersBg);
	auto hq = PainterHighQualityEnabler(p);
	const auto &font = SmallFont();
	const auto left = Px(kGalleryPadding);
	const auto height = Px(kFontsStripHeight);
	auto top = Px(kGalleryPadding);
	p.setFont(font);
	for (const auto &row : _rows) {
		p.setPen(st::groupCallMembersFg);
		p.drawText(
			QRect(left, top, _built, labelHeight()),
			Qt::AlignLeft | Qt::AlignVCenter,
			font->elided(row.title, _built));
		top += labelHeight();
		const auto rect = QRect(left, top, _built, height);
		auto clip = QPainterPath();
		clip.addRoundedRect(rect, Px(kGalleryRadius), Px(kGalleryRadius));
		p.save();
		p.setClipPath(clip);
		p.drawImage(rect, row.image);
		p.restore();
		top += height + Px(kGalleryLabelSkip);
	}
}

[[nodiscard]] std::vector<FxInstance> PresetStack(QByteArrayView id) {
	for (const auto preset : AllFxPresets()) {
		if (QByteArrayView(preset->id) == id) {
			return preset->stack;
		}
	}
	return {};
}

void AppendToActiveLayer(
		not_null<Controller*> controller,
		std::vector<FxInstance> stack) {
	const auto id = controller->activeLayerId();
	controller->change([&](Document &document) {
		for (auto &instance : stack) {
			AddLayerFx(document, id, std::move(instance));
		}
	});
}

[[nodiscard]] object_ptr<Ui::RpWidget> ParamsScene(
		not_null<QWidget*> parent,
		not_null<Controller*> controller,
		FxInstance instance) {
	auto result = object_ptr<Ui::VerticalLayout>(parent);
	const auto descriptor = FindFx(instance.id);
	if (descriptor) {
		result->add(
			CreateParamsPanel(result.data(), ParamsPanelArgs{
				.params = descriptor->params,
				.values = instance.params,
				.controller = controller,
			}),
			style::margins(
				Px(kPanelScenePadding),
				Px(kPanelScenePadding) / 2,
				Px(kPanelScenePadding),
				Px(kPanelScenePadding)));
	}
	return object_ptr<Ui::RpWidget>(std::move(result));
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	RegisterFxGalleryScene(u"photo_lofi_gallery"_q, FxGroup::Lofi, false);
	RegisterFxGalleryScene(u"photo_lofi_presets"_q, FxGroup::Lofi, true);
	SelfTest::RegisterScene(
		u"photo_lofi_stamp_fonts"_q,
		QSize(Px(kFontsSceneWidth), 0),
		[](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<StampFonts>(parent.get());
		});

	// The editor scenes open the photo at the size of a real one, so the
	// presets are as strong on the canvas as they are on a photo from
	// a chat (see FxSceneSampleImage).
	RegisterEditorScene({
		.name = u"photo_lofi_camera"_q,
		.document = [] { return FxSceneSampleDocument(); },
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			AppendToActiveLayer(controller, PresetStack("lofi.preset_ccd"));
		},
	});
	RegisterEditorScene({
		.name = u"photo_lofi_vhs"_q,
		.document = [] { return FxSceneSampleDocument(); },
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			AppendToActiveLayer(controller, PresetStack("lofi.preset_vhs"));
		},
	});
	RegisterEditorScene({
		.name = u"photo_lofi_pocket"_q,
		.size = QSize(520, 820),
		.document = [] { return FxSceneSampleDocument(); },
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			AppendToActiveLayer(
				controller,
				PresetStack("lofi.preset_pocket"));
		},
	});

	// The cards of a whole preset the way the Layer tab shows them (the
	// editor scenes above have room only for the top of the first card):
	// the sensor, the tape, the date with its text field and the lines.
	RegisterFxStackScene(u"photo_lofi_stack"_q, "lofi.preset_vhs");

	const auto width = Px(kPanelSceneWidth);
	RegisterPanelScene({
		.name = u"photo_lofi_stamp_panel"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return ParamsScene(parent, controller, MakeFx("lofi.stamp"));
		},
	});
	// The text was erased: the field shows its hint, nothing is stamped.
	RegisterPanelScene({
		.name = u"photo_lofi_stamp_empty"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return ParamsScene(parent, controller, MakeFx("lofi.stamp", {
				{ "text", FxValue::Data(QByteArray()) },
			}));
		},
	});
	RegisterPanelScene({
		.name = u"photo_lofi_camera_panel"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			auto stack = PresetStack("lofi.preset_ccd");
			return ParamsScene(
				parent,
				controller,
				stack.empty() ? MakeFx("lofi.camera") : stack.front());
		},
	});
	RegisterPanelScene({
		.name = u"photo_lofi_depth_panel"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return ParamsScene(parent, controller, MakeFx("lofi.depth"));
		},
	});

	// The digicam pack (oblivion_photo_fx_digicam.h). Its effects are in
	// the two galleries above with the rest of the group, these sheets
	// show what one tile can't: every filter, every lens reflection and
	// the colors of the bloom of the CCD camera.
	SelfTest::RegisterScene(
		u"photo_digicam_looks"_q,
		QSize(Px(kGalleryWidth), 0),
		[](not_null<Ui::RpWidget*> parent) {
			auto sources = std::vector<GallerySource>();
			const auto descriptor = FindFx("digicam.look");
			const auto param = descriptor ? descriptor->param("look") : nullptr;
			const auto count = param ? int(param->choices.size()) : 0;
			for (auto i = 0; i != count; ++i) {
				sources.push_back({
					param->choices[i].now(),
					{ MakeFx("digicam.look", {
						{ "look", FxValue::Integer(i) },
					}) },
				});
			}
			return Ui::CreateChild<FxGallery>(
				parent.get(),
				std::move(sources));
		});
	SelfTest::RegisterScene(
		u"photo_digicam_reflections"_q,
		QSize(Px(kGalleryWidth), 0),
		[](not_null<Ui::RpWidget*> parent) {
			auto sources = std::vector<GallerySource>();
			const auto descriptor = FindFx("digicam.reflection");
			const auto param = descriptor
				? descriptor->param("variant")
				: nullptr;
			const auto count = param ? int(std::lround(param->max)) : 0;
			for (auto i = 1; i <= count; ++i) {
				sources.push_back({
					QString::number(i),
					{ MakeFx("digicam.reflection", {
						{ "variant", FxValue::Integer(i) },
						{ "strength", FxValue::Integer(80) },
					}) },
				});
			}
			return Ui::CreateChild<FxGallery>(
				parent.get(),
				std::move(sources));
		});
	SelfTest::RegisterScene(
		u"photo_digicam_tints"_q,
		QSize(Px(kGalleryWidth), 0),
		[](not_null<Ui::RpWidget*> parent) {
			auto sources = std::vector<GallerySource>();
			if (FindFx("digicam.ccd")) {
				// Violet, magenta, blue, orange, green and white.
				for (const auto &tint : {
					QColor(64, 0, 255),
					QColor(255, 0, 255),
					QColor(0, 0, 255),
					QColor(255, 128, 0),
					QColor(0, 255, 0),
					QColor(255, 255, 255),
				}) {
					sources.push_back({
						tint.name().toUpper(),
						{ MakeFx("digicam.ccd", {
							{ "tint", FxValue::Color(tint) },
							{ "bloom", FxValue::Integer(160) },
						}) },
					});
				}
			}
			return Ui::CreateChild<FxGallery>(
				parent.get(),
				std::move(sources));
		});
	RegisterEditorScene({
		.name = u"photo_digicam_ccd"_q,
		.document = [] { return FxSceneSampleDocument(); },
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			AppendToActiveLayer(controller, { MakeFx("digicam.ccd") });
		},
	});
	RegisterEditorScene({
		.name = u"photo_digicam_nokia"_q,
		.size = QSize(520, 820),
		.document = [] { return FxSceneSampleDocument(); },
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			AppendToActiveLayer(controller, { MakeFx("digicam.nokia") });
		},
	});
	RegisterFxStackScene(u"photo_digicam_stack"_q, "digicam.preset_jpeg_low");
	for (const auto &[name, id] : {
		std::pair{ "photo_digicam_ccd_panel", "digicam.ccd" },
		std::pair{ "photo_digicam_look_panel", "digicam.look" },
		std::pair{ "photo_digicam_vignette_panel", "digicam.vignette" },
		std::pair{ "photo_digicam_noise_panel", "lofi.noise" },
	}) {
		RegisterPanelScene({
			.name = QString::fromLatin1(name),
			.size = QSize(width, 0),
			.create = [id = QByteArray(id)](
					not_null<QWidget*> parent,
					not_null<Controller*> controller) {
				return ParamsScene(parent, controller, MakeFx(id));
			},
		});
	}
});

} // namespace

QImage FxSceneSampleImage() {
	const auto sample = SampleSceneImage();
	const auto longSide = std::max(sample.width(), sample.height());
	if (sample.isNull() || longSide >= kSampleLongSide) {
		return sample;
	}
	return FxResized(
		sample,
		sample.size().scaled(
			kSampleLongSide,
			kSampleLongSide,
			Qt::KeepAspectRatio));
}

Document FxSceneSampleDocument() {
	return DocumentFromImage(
		FxSceneSampleImage(),
		EditState(),
		tr::lng_oblivion_photo_panel_layer_photo(tr::now));
}

void RegisterFxGalleryScene(QString name, FxGroup group, bool presets) {
	SelfTest::RegisterScene(
		std::move(name),
		QSize(EditorUi::Px(kGalleryWidth), 0),
		[=](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<FxGallery>(parent.get(), group, presets);
		});
}

void RegisterFxStackScene(QString name, QByteArray preset) {
	RegisterPanelScene({
		.name = std::move(name),
		.size = QSize(EditorUi::Px(kPanelSceneWidth), 0),
		.document = [] { return FxSceneSampleDocument(); },
		.prepare = [=](not_null<Controller*> controller) {
			const auto &layers = controller->document().layers;
			if (!layers.empty()) {
				controller->setActiveLayer(layers.front().id);
			}
			AppendToActiveLayer(controller, PresetStack(preset));
		},
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			auto result = object_ptr<Ui::VerticalLayout>(parent);
			result->add(
				CreateLayerPanel(result.data(), controller),
				style::margins(0, 0, 0, EditorUi::Px(kGalleryPadding)));
			return object_ptr<Ui::RpWidget>(std::move(result));
		},
	});
}

} // namespace Oblivion::Photo
