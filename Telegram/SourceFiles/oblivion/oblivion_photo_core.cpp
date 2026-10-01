/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_core.h"

#include "lang/lang_keys.h"

#include <QtCore/QBuffer>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtGui/QColorSpace>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtGui/QPainter>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>

namespace Oblivion::Photo {
namespace {

using Pixel = uint32;

constexpr auto kLutSize = 4096;
constexpr auto kLutMax = float(kLutSize - 1);
constexpr auto kPi = 3.14159265358979323846;
constexpr auto kMinCrop = 0.001;
constexpr auto kStraightenEpsilon = 1e-3;
constexpr auto kGrainSeed = 0x5EED1234U;

// Pixels per "unit": all spatial sizes are in units of 1/1000
// of the shorter side of the rendered image.
[[nodiscard]] float UnitFor(int width, int height) {
	return std::max(std::min(width, height), 1) / 1000.f;
}

[[nodiscard]] inline float Clamp01(float v) {
	// NaN -> 0.
	return (v > 0.f) ? ((v < 1.f) ? v : 1.f) : 0.f;
}

[[nodiscard]] inline int LutIndex(float v) {
	return int(Clamp01(v) * kLutMax + 0.5f);
}

[[nodiscard]] inline float Luma(float r, float g, float b) {
	return 0.299f * r + 0.587f * g + 0.114f * b;
}

[[nodiscard]] inline float Mix(float a, float b, float t) {
	return a + (b - a) * t;
}

[[nodiscard]] inline float SmoothStep(float e0, float e1, float x) {
	const auto t = Clamp01((x - e0) / (e1 - e0));
	return t * t * (3.f - 2.f * t);
}

[[nodiscard]] inline double SrgbToLinear(double v) {
	return (v <= 0.04045) ? (v / 12.92) : std::pow((v + 0.055) / 1.055, 2.4);
}

[[nodiscard]] inline double LinearToSrgb(double v) {
	return (v <= 0.0031308)
		? (v * 12.92)
		: (1.055 * std::pow(v, 1. / 2.4) - 0.055);
}

//
// Threading.
//

[[nodiscard]] int ThreadCount() {
	static const auto result = std::clamp(
		int(std::thread::hardware_concurrency()),
		1,
		32);
	return result;
}

struct ParallelState {
	std::atomic<int> next{ 0 };
	std::atomic<int> done{ 0 };
	crl::semaphore finished;
};

// Calls body(from, till) for chunks of [0, count), in parallel. The caller
// takes chunks too, so it never waits for a chunk nobody started, and
// workers that start late find nothing to do and never touch body.
template <typename Body>
void ParallelFor(int count, int grain, const Body &body) {
	if (count <= 0) {
		return;
	}
	const auto threads = ThreadCount();
	auto chunks = std::min(
		std::max(count / std::max(grain, 1), 1),
		threads * 4);
	if (threads < 2 || chunks < 2) {
		body(0, count);
		return;
	}
	const auto size = (count + chunks - 1) / chunks;
	chunks = (count + size - 1) / size;
	const auto state = std::make_shared<ParallelState>();
	const auto callback = &body;
	const auto run = [=] {
		while (true) {
			const auto index = state->next.fetch_add(1);
			if (index >= chunks) {
				return;
			}
			const auto from = index * size;
			(*callback)(from, std::min(from + size, count));
			if (state->done.fetch_add(1) + 1 == chunks) {
				state->finished.release();
			}
		}
	};
	const auto workers = std::min(threads, chunks) - 1;
	for (auto i = 0; i != workers; ++i) {
		crl::async(run);
	}
	run();
	state->finished.acquire();
}

// Rows in bands of at least ~32K pixels.
template <typename Body>
void ForRows(int width, int height, const Body &body) {
	ParallelFor(height, std::max(1, 32768 / std::max(width, 1)), body);
}

//
// Pixels.
//

struct Buffer {
	Pixel *data = nullptr;
	int width = 0;
	int height = 0;
	int stride = 0; // In pixels.

	[[nodiscard]] Pixel *row(int y) const {
		return data + ptrdiff_t(y) * stride;
	}
	[[nodiscard]] bool empty() const {
		return !data || (width <= 0) || (height <= 0);
	}
};

// Tightly packed copy of the pixels, for passes that read neighbours.
struct PixelCopy {
	std::vector<Pixel> pixels;
	int width = 0;
	int height = 0;

	[[nodiscard]] Buffer view() {
		return { pixels.data(), width, height, width };
	}
	[[nodiscard]] const Pixel *row(int y) const {
		return pixels.data() + ptrdiff_t(y) * width;
	}
	[[nodiscard]] Pixel at(int x, int y) const {
		x = std::clamp(x, 0, width - 1);
		y = std::clamp(y, 0, height - 1);
		return pixels[ptrdiff_t(y) * width + x];
	}
};

[[nodiscard]] Buffer Wrap(QImage &image) {
	return {
		reinterpret_cast<Pixel*>(image.bits()),
		image.width(),
		image.height(),
		int(image.bytesPerLine() / 4),
	};
}

[[nodiscard]] PixelCopy Copy(const Buffer &image) {
	auto result = PixelCopy{
		std::vector<Pixel>(size_t(image.width) * image.height),
		image.width,
		image.height,
	};
	ForRows(image.width, image.height, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			std::copy_n(
				image.row(y),
				image.width,
				result.pixels.data() + ptrdiff_t(y) * image.width);
		}
	});
	return result;
}

[[nodiscard]] inline int Alpha(Pixel p) {
	return int(p >> 24);
}
[[nodiscard]] inline int Red(Pixel p) {
	return int((p >> 16) & 0xFFU);
}
[[nodiscard]] inline int Green(Pixel p) {
	return int((p >> 8) & 0xFFU);
}
[[nodiscard]] inline int Blue(Pixel p) {
	return int(p & 0xFFU);
}
[[nodiscard]] inline Pixel Pack(int a, int r, int g, int b) {
	return (Pixel(a) << 24) | (Pixel(r) << 16) | (Pixel(g) << 8) | Pixel(b);
}

struct Color {
	float r = 0.f;
	float g = 0.f;
	float b = 0.f;
};

[[nodiscard]] inline Color Mix(Color a, Color b, float t) {
	return { Mix(a.r, b.r, t), Mix(a.g, b.g, t), Mix(a.b, b.b, t) };
}

[[nodiscard]] inline float Luma(Color c) {
	return Luma(c.r, c.g, c.b);
}

// a > 0.
[[nodiscard]] inline Color Unpremultiply(Pixel p, int a) {
	if (a == 255) {
		constexpr auto k = 1.f / 255.f;
		return { Red(p) * k, Green(p) * k, Blue(p) * k };
	}
	const auto k = 1.f / a;
	return {
		std::min(Red(p) * k, 1.f),
		std::min(Green(p) * k, 1.f),
		std::min(Blue(p) * k, 1.f),
	};
}

[[nodiscard]] inline Pixel Premultiply(int a, Color c) {
	const auto k = float(a);
	return Pack(
		a,
		int(Clamp01(c.r) * k + 0.5f),
		int(Clamp01(c.g) * k + 0.5f),
		int(Clamp01(c.b) * k + 0.5f));
}

[[nodiscard]] inline Color FromRgb(QRgb rgb) {
	constexpr auto k = 1.f / 255.f;
	return { qRed(rgb) * k, qGreen(rgb) * k, qBlue(rgb) * k };
}

// Lerp of two premultiplied pixels, b weight 0..256, rounded.
[[nodiscard]] inline Pixel Interpolate(Pixel x, Pixel y, uint32 b) {
	const auto a = 256U - b;
	auto low = (x & 0x00FF00FFU) * a + (y & 0x00FF00FFU) * b + 0x00800080U;
	low = (low >> 8) & 0x00FF00FFU;
	auto high = ((x >> 8) & 0x00FF00FFU) * a
		+ ((y >> 8) & 0x00FF00FFU) * b
		+ 0x00800080U;
	high &= 0xFF00FF00U;
	return high | low;
}

[[nodiscard]] inline uint32 Weight256(float t) {
	return uint32(std::clamp(int(t * 256.f + 0.5f), 0, 256));
}

// Bilinear sample of premultiplied pixels at continuous pixel-index
// coordinates (pixel centers at integers), clamped to the edges.
[[nodiscard]] inline Pixel SampleBilinear(
		const Pixel *data,
		int width,
		int height,
		int stride,
		float fx,
		float fy) {
	fx = std::clamp(fx, 0.f, float(width - 1));
	fy = std::clamp(fy, 0.f, float(height - 1));
	const auto x0 = int(fx);
	const auto y0 = int(fy);
	const auto x1 = std::min(x0 + 1, width - 1);
	const auto y1 = std::min(y0 + 1, height - 1);
	const auto dx = Weight256(fx - x0);
	const auto dy = Weight256(fy - y0);
	const auto top = data + ptrdiff_t(y0) * stride;
	const auto bottom = data + ptrdiff_t(y1) * stride;
	return Interpolate(
		Interpolate(top[x0], top[x1], dx),
		Interpolate(bottom[x0], bottom[x1], dx),
		dy);
}

[[nodiscard]] inline Pixel SampleBilinear(
		const PixelCopy &copy,
		float fx,
		float fy) {
	return SampleBilinear(
		copy.pixels.data(),
		copy.width,
		copy.height,
		copy.width,
		fx,
		fy);
}

// Calls op(Color &color, int x, int y) with un-premultiplied colors
// of all non-transparent pixels, alpha is kept.
template <typename Op>
void ForEachColor(const Buffer &image, const Op &op) {
	ForRows(image.width, image.height, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = image.row(y);
			for (auto x = 0; x != image.width; ++x) {
				const auto p = line[x];
				const auto a = Alpha(p);
				if (!a) {
					continue;
				}
				auto color = Unpremultiply(p, a);
				op(color, x, y);
				line[x] = Premultiply(a, color);
			}
		}
	});
}

[[nodiscard]] bool HasTransparency(const QImage &image) {
	if (!image.hasAlphaChannel()) {
		return false;
	}
	const auto converted = (image.format() == QImage::Format_ARGB32_Premultiplied)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const auto data = reinterpret_cast<const Pixel*>(converted.constBits());
	const auto stride = converted.bytesPerLine() / 4;
	auto result = std::atomic<bool>(false);
	ForRows(converted.width(), converted.height(), [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = data + ptrdiff_t(y) * stride;
			for (auto x = 0; x != converted.width(); ++x) {
				if ((line[x] >> 24) != 0xFFU) {
					result = true;
					return;
				}
			}
		}
	});
	return result;
}

// Blurs and pixel-mixing passes may round a channel one step above
// alpha, which is not a valid premultiplied pixel.
void FixPremultiplied(const Buffer &image) {
	ForRows(image.width, image.height, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = image.row(y);
			for (auto x = 0; x != image.width; ++x) {
				const auto p = line[x];
				const auto a = Alpha(p);
				if (a == 255) {
					continue;
				}
				line[x] = Pack(
					a,
					std::min(Red(p), a),
					std::min(Green(p), a),
					std::min(Blue(p), a));
			}
		}
	});
}

//
// Noise.
//

[[nodiscard]] inline uint32 Hash(uint32 x, uint32 y, uint32 seed) {
	auto h = (x * 0x27D4EB2DU) ^ ((y + 0x9E3779B9U) * 0x85EBCA6BU)
		^ (seed * 0xC2B2AE35U);
	h ^= h >> 15;
	h *= 0x2C1B3C6DU;
	h ^= h >> 12;
	h *= 0x297A2D39U;
	h ^= h >> 15;
	return h;
}

// Triangular distribution in [-1, 1].
[[nodiscard]] inline float HashSigned(uint32 x, uint32 y, uint32 seed) {
	const auto h = Hash(x, y, seed);
	return ((h & 0xFFFFU) + (h >> 16)) * (1.f / 65535.f) - 1.f;
}

struct GrainField {
	float cell = 1.f;
	float inverse = 1.f;
	uint32 seed = kGrainSeed;

	explicit GrainField(float cell, uint32 seed = kGrainSeed)
	: cell(std::max(cell, 0.01f))
	, inverse(1.f / std::max(cell, 0.5f))
	, seed(seed) {
	}

	// Grain cells smaller than a pixel (a downscaled preview of a large
	// image) average out in reality, keep the preview close to the export.
	[[nodiscard]] float strength() const {
		return std::clamp(cell, 0.35f, 1.f);
	}

	[[nodiscard]] float at(int x, int y) const {
		if (cell <= 1.05f) {
			return HashSigned(uint32(x), uint32(y), seed);
		}
		const auto fx = x * inverse;
		const auto fy = y * inverse;
		const auto ix = int(fx);
		const auto iy = int(fy);
		auto tx = fx - ix;
		auto ty = fy - iy;
		tx = tx * tx * (3.f - 2.f * tx);
		ty = ty * ty * (3.f - 2.f * ty);
		const auto a = HashSigned(uint32(ix), uint32(iy), seed);
		const auto b = HashSigned(uint32(ix + 1), uint32(iy), seed);
		const auto c = HashSigned(uint32(ix), uint32(iy + 1), seed);
		const auto d = HashSigned(uint32(ix + 1), uint32(iy + 1), seed);
		// Interpolation lowers the variance, compensate a bit.
		return Mix(Mix(a, b, tx), Mix(c, d, tx), ty) * 1.35f;
	}
};

struct Random {
	uint64 state = 0;

	explicit Random(uint64 seed) : state(seed) {
	}

	[[nodiscard]] uint64 next() {
		auto z = (state += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}
	[[nodiscard]] float unit() { // [0, 1)
		return float(next() >> 40) * (1.f / 16777216.f);
	}
};

//
// Gaussian blur of C interleaved channels of T (uchar or float).
//

template <typename T>
[[nodiscard]] inline T Store(float v) {
	if constexpr (std::is_same_v<T, float>) {
		return v;
	} else {
		return T(std::clamp(int(v + 0.5f), 0, 255));
	}
}

[[nodiscard]] std::array<int, 3> BoxRadii(float sigma) {
	// Three box passes approximating a gaussian (W. Jarosz).
	constexpr auto n = 3;
	const auto ideal = std::sqrt(12.f * sigma * sigma / n + 1.f);
	auto lower = int(std::floor(ideal));
	if (!(lower % 2)) {
		--lower;
	}
	const auto upper = lower + 2;
	const auto m = int(std::lround(
		(12.f * sigma * sigma
			- n * lower * lower
			- 4.f * n * lower
			- 3.f * n) / (-4.f * lower - 4.f)));
	auto result = std::array<int, 3>();
	for (auto i = 0; i != n; ++i) {
		result[i] = ((i < m) ? lower : upper) / 2; // Odd width -> radius.
	}
	return result;
}

template <int C, typename T>
void BoxLine(const T *in, T *out, int count, int radius) {
	if (radius <= 0) {
		std::copy_n(in, count * C, out);
		return;
	}
	const auto scale = 1.f / (2 * radius + 1);
	float sum[C];
	for (auto c = 0; c != C; ++c) {
		sum[c] = float(in[c]) * (radius + 1);
	}
	for (auto i = 1; i <= radius; ++i) {
		const auto k = std::min(i, count - 1) * C;
		for (auto c = 0; c != C; ++c) {
			sum[c] += float(in[k + c]);
		}
	}
	for (auto x = 0; x != count; ++x) {
		for (auto c = 0; c != C; ++c) {
			out[x * C + c] = Store<T>(sum[c] * scale);
		}
		const auto add = std::min(x + radius + 1, count - 1) * C;
		const auto sub = std::max(x - radius, 0) * C;
		for (auto c = 0; c != C; ++c) {
			sum[c] += float(in[add + c]) - float(in[sub + c]);
		}
	}
}

template <int C, typename T>
void KernelLine(
		const T *in,
		T *out,
		int count,
		const std::vector<float> &kernel) {
	const auto radius = int(kernel.size() / 2);
	for (auto x = 0; x != count; ++x) {
		float sum[C] = {};
		for (auto k = -radius; k <= radius; ++k) {
			const auto index = std::clamp(x + k, 0, count - 1) * C;
			const auto weight = kernel[k + radius];
			for (auto c = 0; c != C; ++c) {
				sum[c] += float(in[index + c]) * weight;
			}
		}
		for (auto c = 0; c != C; ++c) {
			out[x * C + c] = Store<T>(sum[c]);
		}
	}
}

// Vertical box pass over a band of columns stored as height rows
// of 'count' elements.
template <typename T>
void BoxColumns(const T *in, T *out, int count, int height, int radius) {
	if (radius <= 0) {
		std::copy_n(in, size_t(count) * height, out);
		return;
	}
	const auto scale = 1.f / (2 * radius + 1);
	auto sum = std::vector<float>(count);
	for (auto e = 0; e != count; ++e) {
		sum[e] = float(in[e]) * (radius + 1);
	}
	for (auto i = 1; i <= radius; ++i) {
		const auto row = in + ptrdiff_t(std::min(i, height - 1)) * count;
		for (auto e = 0; e != count; ++e) {
			sum[e] += float(row[e]);
		}
	}
	for (auto y = 0; y != height; ++y) {
		const auto target = out + ptrdiff_t(y) * count;
		for (auto e = 0; e != count; ++e) {
			target[e] = Store<T>(sum[e] * scale);
		}
		const auto add = in + ptrdiff_t(std::min(y + radius + 1, height - 1)) * count;
		const auto sub = in + ptrdiff_t(std::max(y - radius, 0)) * count;
		for (auto e = 0; e != count; ++e) {
			sum[e] += float(add[e]) - float(sub[e]);
		}
	}
}

template <typename T>
void KernelColumns(
		const T *in,
		T *out,
		int count,
		int height,
		const std::vector<float> &kernel) {
	const auto radius = int(kernel.size() / 2);
	auto sum = std::vector<float>(count);
	for (auto y = 0; y != height; ++y) {
		std::fill(sum.begin(), sum.end(), 0.f);
		for (auto k = -radius; k <= radius; ++k) {
			const auto row = in
				+ ptrdiff_t(std::clamp(y + k, 0, height - 1)) * count;
			const auto weight = kernel[k + radius];
			for (auto e = 0; e != count; ++e) {
				sum[e] += float(row[e]) * weight;
			}
		}
		const auto target = out + ptrdiff_t(y) * count;
		for (auto e = 0; e != count; ++e) {
			target[e] = Store<T>(sum[e]);
		}
	}
}

[[nodiscard]] std::vector<float> GaussianKernel(float sigma) {
	const auto radius = std::max(1, int(std::ceil(sigma * 3.f)));
	auto result = std::vector<float>(2 * radius + 1);
	auto sum = 0.f;
	for (auto i = -radius; i <= radius; ++i) {
		const auto value = std::exp(-(i * i) / (2.f * sigma * sigma));
		result[i + radius] = value;
		sum += value;
	}
	for (auto &value : result) {
		value /= sum;
	}
	return result;
}

// data: height rows of stride elements, each row has width * C elements.
template <int C, typename T>
void GaussianBlur(T *data, int width, int height, int stride, float sigma) {
	if (!(sigma >= 0.3f) || width < 1 || height < 1) {
		return;
	}
	const auto exact = (sigma < 2.f);
	const auto kernel = exact ? GaussianKernel(sigma) : std::vector<float>();
	const auto radii = exact ? std::array<int, 3>() : BoxRadii(sigma);
	const auto elements = width * C;

	ForRows(width, height, [&](int from, int till) {
		auto a = std::vector<T>(elements);
		auto b = std::vector<T>(elements);
		for (auto y = from; y != till; ++y) {
			const auto line = data + ptrdiff_t(y) * stride;
			if (exact) {
				std::copy_n(line, elements, a.data());
				KernelLine<C, T>(a.data(), line, width, kernel);
			} else {
				BoxLine<C, T>(line, a.data(), width, radii[0]);
				BoxLine<C, T>(a.data(), b.data(), width, radii[1]);
				BoxLine<C, T>(b.data(), line, width, radii[2]);
			}
		}
	});

	// Vertical: bands of columns, gathered into a local buffer.
	constexpr auto kBand = 64;
	const auto bands = (elements + kBand - 1) / kBand;
	ParallelFor(bands, 1, [&](int from, int till) {
		auto a = std::vector<T>(size_t(kBand) * height);
		auto b = std::vector<T>(size_t(kBand) * height);
		for (auto band = from; band != till; ++band) {
			const auto e0 = band * kBand;
			const auto count = std::min(kBand, elements - e0);
			for (auto y = 0; y != height; ++y) {
				std::copy_n(
					data + ptrdiff_t(y) * stride + e0,
					count,
					a.data() + ptrdiff_t(y) * count);
			}
			if (exact) {
				KernelColumns<T>(a.data(), b.data(), count, height, kernel);
			} else {
				BoxColumns<T>(a.data(), b.data(), count, height, radii[0]);
				BoxColumns<T>(b.data(), a.data(), count, height, radii[1]);
				BoxColumns<T>(a.data(), b.data(), count, height, radii[2]);
			}
			for (auto y = 0; y != height; ++y) {
				std::copy_n(
					b.data() + ptrdiff_t(y) * count,
					count,
					data + ptrdiff_t(y) * stride + e0);
			}
		}
	});
}

void BlurPixels(const Buffer &image, float sigma) {
	GaussianBlur<4, uchar>(
		reinterpret_cast<uchar*>(image.data),
		image.width,
		image.height,
		image.stride * 4,
		sigma);
}

void BlurPixels(PixelCopy &copy, float sigma) {
	BlurPixels(copy.view(), sigma);
}

void BlurPlane(std::vector<float> &plane, int width, int height, float sigma) {
	GaussianBlur<1, float>(plane.data(), width, height, width, sigma);
}

// Un-premultiplied luma of every pixel, 0 for transparent ones.
[[nodiscard]] std::vector<float> LumaPlane(const Buffer &image) {
	auto result = std::vector<float>(size_t(image.width) * image.height);
	ForRows(image.width, image.height, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = image.row(y);
			const auto target = result.data() + ptrdiff_t(y) * image.width;
			for (auto x = 0; x != image.width; ++x) {
				const auto a = Alpha(line[x]);
				target[x] = a ? Luma(Unpremultiply(line[x], a)) : 0.f;
			}
		}
	});
	return result;
}

//
// Tone curves.
//

struct CurvePoint {
	float x = 0.f; // 0..255
	float y = 0.f; // 0..255
};

using Lut = std::array<float, kLutSize>;

// Monotone cubic (Fritsch-Carlson) through the points, flat outside.
[[nodiscard]] Lut BuildCurve(const std::vector<CurvePoint> &points) {
	auto result = Lut();
	const auto n = int(points.size());
	if (n < 2) {
		for (auto i = 0; i != kLutSize; ++i) {
			result[i] = i / kLutMax;
		}
		return result;
	}
	auto xs = std::vector<float>(n);
	auto ys = std::vector<float>(n);
	for (auto i = 0; i != n; ++i) {
		xs[i] = points[i].x / 255.f;
		ys[i] = points[i].y / 255.f;
	}
	auto d = std::vector<float>(n - 1);
	for (auto i = 0; i + 1 < n; ++i) {
		d[i] = (ys[i + 1] - ys[i]) / std::max(xs[i + 1] - xs[i], 1e-6f);
	}
	auto m = std::vector<float>(n);
	m[0] = d[0];
	m[n - 1] = d[n - 2];
	for (auto i = 1; i + 1 < n; ++i) {
		m[i] = (d[i - 1] * d[i] <= 0.f) ? 0.f : (d[i - 1] + d[i]) / 2.f;
	}
	for (auto i = 0; i + 1 < n; ++i) {
		if (std::abs(d[i]) < 1e-6f) {
			m[i] = m[i + 1] = 0.f;
			continue;
		}
		const auto a = m[i] / d[i];
		const auto b = m[i + 1] / d[i];
		const auto s = a * a + b * b;
		if (s > 9.f) {
			const auto t = 3.f / std::sqrt(s);
			m[i] = t * a * d[i];
			m[i + 1] = t * b * d[i];
		}
	}
	auto segment = 0;
	for (auto i = 0; i != kLutSize; ++i) {
		const auto x = i / kLutMax;
		if (x <= xs[0]) {
			result[i] = ys[0];
			continue;
		} else if (x >= xs[n - 1]) {
			result[i] = ys[n - 1];
			continue;
		}
		while (segment + 2 < n && x > xs[segment + 1]) {
			++segment;
		}
		const auto h = xs[segment + 1] - xs[segment];
		const auto t = (x - xs[segment]) / h;
		const auto t2 = t * t;
		const auto t3 = t2 * t;
		result[i] = Clamp01((2 * t3 - 3 * t2 + 1) * ys[segment]
			+ (t3 - 2 * t2 + t) * h * m[segment]
			+ (-2 * t3 + 3 * t2) * ys[segment + 1]
			+ (t3 - t2) * h * m[segment + 1]);
	}
	return result;
}

//
// Filter looks.
//

struct LookSpec {
	const char *id = nullptr;
	std::vector<CurvePoint> master;
	std::vector<CurvePoint> red;
	std::vector<CurvePoint> green;
	std::vector<CurvePoint> blue;
	std::array<float, 3> gains = { 1.f, 1.f, 1.f };
	bool mono = false;
	std::array<float, 3> monoWeights = { 0.299f, 0.587f, 0.114f };
	float saturation = 1.f;
	std::array<float, 3> shadows = {}; // Offsets weighted by (1 - y)^2.
	std::array<float, 3> highlights = {}; // Offsets weighted by y^2.
	float vignette = 0.f; // 0..1, added to the vignette adjustment.
	float grain = 0.f; // 0..1, added to the grain adjustment.
};

[[nodiscard]] const std::vector<LookSpec> &Looks() {
	static const auto result = [] {
		auto list = std::vector<LookSpec>();
		const auto add = [&](LookSpec spec) {
			list.push_back(std::move(spec));
		};
		add({ .id = "original" });
		add({
			.id = "bw",
			.master = { { 0, 0 }, { 64, 58 }, { 192, 200 }, { 255, 255 } },
			.mono = true,
		});
		add({
			.id = "bw_contrast",
			.master = {
				{ 0, 0 },
				{ 45, 24 },
				{ 128, 128 },
				{ 210, 234 },
				{ 255, 255 },
			},
			.mono = true,
			.monoWeights = { 0.42f, 0.48f, 0.10f },
		});
		add({
			.id = "sepia",
			.master = { { 0, 8 }, { 255, 250 } },
			.red = { { 0, 28 }, { 128, 152 }, { 255, 255 } },
			.green = { { 0, 16 }, { 128, 124 }, { 255, 240 } },
			.blue = { { 0, 6 }, { 128, 92 }, { 255, 208 } },
			.mono = true,
		});
		add({
			.id = "vintage",
			.red = { { 0, 32 }, { 128, 142 }, { 255, 238 } },
			.green = { { 0, 22 }, { 128, 130 }, { 255, 232 } },
			.blue = { { 0, 48 }, { 128, 120 }, { 255, 196 } },
			.saturation = 0.75f,
			.vignette = 0.25f,
			.grain = 0.08f,
		});
		add({
			.id = "film",
			.master = {
				{ 0, 20 },
				{ 64, 62 },
				{ 128, 130 },
				{ 192, 196 },
				{ 255, 242 },
			},
			.gains = { 1.03f, 1.f, 0.96f },
			.saturation = 0.9f,
			.shadows = { -0.01f, 0.01f, 0.025f },
			.highlights = { 0.02f, 0.01f, -0.02f },
			.grain = 0.2f,
		});
		add({
			.id = "warm",
			.master = { { 0, 4 }, { 128, 132 }, { 255, 255 } },
			.gains = { 1.08f, 1.02f, 0.88f },
			.saturation = 1.06f,
		});
		add({
			.id = "cool",
			.master = { { 0, 0 }, { 128, 126 }, { 255, 252 } },
			.gains = { 0.92f, 1.f, 1.1f },
			.saturation = 0.96f,
		});
		add({
			.id = "cinema",
			.master = {
				{ 0, 6 },
				{ 64, 56 },
				{ 128, 128 },
				{ 192, 204 },
				{ 255, 250 },
			},
			.saturation = 1.08f,
			.shadows = { -0.07f, 0.02f, 0.07f },
			.highlights = { 0.07f, 0.02f, -0.06f },
		});
		add({
			.id = "faded",
			.master = { { 0, 52 }, { 128, 136 }, { 255, 226 } },
			.saturation = 0.72f,
		});
		add({
			.id = "noir",
			.master = {
				{ 0, 0 },
				{ 50, 18 },
				{ 128, 118 },
				{ 200, 222 },
				{ 255, 255 },
			},
			.mono = true,
			.monoWeights = { 0.35f, 0.55f, 0.10f },
			.vignette = 0.4f,
			.grain = 0.12f,
		});
		add({
			.id = "pastel",
			.master = { { 0, 42 }, { 128, 160 }, { 255, 250 } },
			.saturation = 0.62f,
			.shadows = { 0.03f, 0.f, 0.05f },
			.highlights = { 0.03f, -0.005f, 0.03f },
		});
		add({
			.id = "vivid",
			.master = { { 0, 0 }, { 64, 56 }, { 192, 206 }, { 255, 255 } },
			.saturation = 1.35f,
		});
		add({
			.id = "sunset",
			.master = { { 0, 6 }, { 128, 130 }, { 255, 250 } },
			.gains = { 1.1f, 1.f, 0.84f },
			.saturation = 1.15f,
			.shadows = { 0.05f, -0.02f, 0.06f },
			.highlights = { 0.06f, 0.015f, -0.04f },
		});
		add({
			.id = "moonlight",
			.master = { { 0, 12 }, { 128, 108 }, { 255, 228 } },
			.gains = { 0.84f, 0.95f, 1.16f },
			.saturation = 0.35f,
			.shadows = { -0.02f, 0.f, 0.05f },
			.highlights = { -0.03f, 0.f, 0.04f },
			.vignette = 0.2f,
		});
		add({
			.id = "cross",
			.red = { { 0, 0 }, { 64, 42 }, { 192, 222 }, { 255, 255 } },
			.green = { { 0, 0 }, { 64, 52 }, { 192, 214 }, { 255, 255 } },
			.blue = { { 0, 48 }, { 255, 206 } },
			.saturation = 1.1f,
		});
		add({
			.id = "lomo",
			.master = { { 0, 0 }, { 64, 48 }, { 192, 212 }, { 255, 255 } },
			.gains = { 1.03f, 1.f, 0.97f },
			.saturation = 1.3f,
			.vignette = 0.55f,
		});
		add({
			.id = "retro",
			.master = { { 0, 30 }, { 128, 134 }, { 255, 238 } },
			.gains = { 1.04f, 1.f, 0.94f },
			.saturation = 0.85f,
			.shadows = { -0.04f, 0.03f, 0.05f },
			.highlights = { 0.05f, 0.03f, -0.02f },
			.grain = 0.1f,
		});
		add({
			.id = "drama",
			.master = {
				{ 0, 0 },
				{ 70, 48 },
				{ 128, 124 },
				{ 200, 218 },
				{ 255, 250 },
			},
			.saturation = 0.78f,
			.vignette = 0.25f,
		});
		return list;
	}();
	return result;
}

[[nodiscard]] const LookSpec *FindLook(const QString &id) {
	for (const auto &look : Looks()) {
		if (id == QLatin1String(look.id)) {
			return &look;
		}
	}
	return nullptr;
}

struct CompiledLook {
	bool active = false;
	float intensity = 1.f;
	bool hasGains = false;
	std::array<float, 3> gains = { 1.f, 1.f, 1.f };
	bool mono = false;
	std::array<float, 3> monoWeights = {};
	bool hasCurves = false;
	std::array<Lut, 3> curves;
	float saturation = 1.f;
	bool hasSplit = false;
	std::array<float, 3> shadows = {};
	std::array<float, 3> highlights = {};
	float vignette = 0.f; // Already multiplied by intensity.
	float grain = 0.f;
};

[[nodiscard]] std::unique_ptr<CompiledLook> CompileLook(
		const QString &id,
		int intensity) {
	auto result = std::make_unique<CompiledLook>();
	const auto spec = FindLook(id);
	if (!spec || !qstrcmp(spec->id, "original") || intensity <= 0) {
		return result;
	}
	const auto t = std::clamp(intensity, 0, 100) / 100.f;
	result->active = true;
	result->intensity = t;
	result->gains = spec->gains;
	result->hasGains = (spec->gains != std::array<float, 3>{ 1.f, 1.f, 1.f });
	result->mono = spec->mono;
	result->monoWeights = spec->monoWeights;
	result->hasCurves = !spec->master.empty()
		|| !spec->red.empty()
		|| !spec->green.empty()
		|| !spec->blue.empty();
	if (result->hasCurves) {
		const auto master = BuildCurve(spec->master);
		const std::vector<CurvePoint> *channels[] = {
			&spec->red,
			&spec->green,
			&spec->blue,
		};
		for (auto c = 0; c != 3; ++c) {
			const auto channel = BuildCurve(*channels[c]);
			for (auto i = 0; i != kLutSize; ++i) {
				result->curves[c][i] = master[LutIndex(channel[i])];
			}
		}
	}
	result->saturation = spec->saturation;
	result->shadows = spec->shadows;
	result->highlights = spec->highlights;
	result->hasSplit = (spec->shadows != std::array<float, 3>{})
		|| (spec->highlights != std::array<float, 3>{});
	result->vignette = spec->vignette * t;
	result->grain = spec->grain * t;
	return result;
}

[[nodiscard]] inline Color ApplyLook(const CompiledLook &look, Color c) {
	const auto original = c;
	if (look.hasGains) {
		c.r *= look.gains[0];
		c.g *= look.gains[1];
		c.b *= look.gains[2];
	}
	if (look.mono) {
		const auto y = c.r * look.monoWeights[0]
			+ c.g * look.monoWeights[1]
			+ c.b * look.monoWeights[2];
		c = { y, y, y };
	}
	if (look.hasCurves) {
		c.r = look.curves[0][LutIndex(c.r)];
		c.g = look.curves[1][LutIndex(c.g)];
		c.b = look.curves[2][LutIndex(c.b)];
	}
	if (look.saturation != 1.f) {
		const auto y = Luma(c);
		c.r = y + (c.r - y) * look.saturation;
		c.g = y + (c.g - y) * look.saturation;
		c.b = y + (c.b - y) * look.saturation;
	}
	if (look.hasSplit) {
		const auto y = Clamp01(Luma(c));
		const auto ws = (1.f - y) * (1.f - y);
		const auto wh = y * y;
		c.r += look.shadows[0] * ws + look.highlights[0] * wh;
		c.g += look.shadows[1] * ws + look.highlights[1] * wh;
		c.b += look.shadows[2] * ws + look.highlights[2] * wh;
	}
	c = { Clamp01(c.r), Clamp01(c.g), Clamp01(c.b) };
	return (look.intensity < 1.f) ? Mix(original, c, look.intensity) : c;
}

//
// Adjustments.
//

struct ToneParams {
	float exposureGain = 1.f;
	std::array<float, 3> balance = { 1.f, 1.f, 1.f };
	float brightness = 0.f; // -1..1
	float contrast = 0.f; // -1..1
	float whites = 0.f; // -1..1
	float blacks = 0.f; // -1..1
	float fade = 0.f; // 0..1

	[[nodiscard]] bool linear() const {
		return (exposureGain != 1.f)
			|| (balance != std::array<float, 3>{ 1.f, 1.f, 1.f });
	}
	[[nodiscard]] bool active() const {
		return linear()
			|| brightness != 0.f
			|| contrast != 0.f
			|| whites != 0.f
			|| blacks != 0.f
			|| fade != 0.f;
	}
};

[[nodiscard]] ToneParams ToneFor(const EditState &state) {
	auto result = ToneParams();
	result.exposureGain = float(std::pow(2., state.exposure / 50.));
	const auto t = state.temperature / 100.f;
	const auto n = state.tint / 100.f;
	auto r = (1.f + 0.28f * t) * (1.f + 0.08f * n);
	auto g = 1.f - 0.22f * n;
	auto b = (1.f - 0.38f * t) * (1.f + 0.08f * n);
	if (state.temperature || state.tint) {
		const auto luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
		r /= luminance;
		g /= luminance;
		b /= luminance;
		result.balance = { r, g, b };
	}
	result.brightness = state.brightness / 100.f;
	result.contrast = state.contrast / 100.f;
	result.whites = state.whites / 100.f;
	result.blacks = state.blacks / 100.f;
	result.fade = state.fade / 100.f;
	return result;
}

[[nodiscard]] float ToneValue(const ToneParams &p, float v, int channel) {
	if (p.linear()) {
		const auto linear = SrgbToLinear(v)
			* p.exposureGain
			* p.balance[channel];
		v = float(LinearToSrgb(std::clamp(linear, 0., 1.)));
	}
	if (p.whites != 0.f) {
		v += p.whites * 0.3f * v * v * v;
	}
	if (p.blacks != 0.f) {
		const auto u = 1.f - Clamp01(v);
		v += p.blacks * 0.25f * u * u * u;
	}
	v = Clamp01(v);
	if (p.brightness != 0.f) {
		v = std::pow(v, std::exp2(-p.brightness * 0.8f));
	}
	if (p.contrast > 0.f) {
		const auto k = 1.f + 1.4f * p.contrast;
		v = (v < 0.5f)
			? 0.5f * std::pow(2.f * v, k)
			: 1.f - 0.5f * std::pow(2.f * (1.f - v), k);
	} else if (p.contrast < 0.f) {
		v = 0.5f + (v - 0.5f) * (1.f + 0.6f * p.contrast);
	}
	if (p.fade > 0.f) {
		v = 0.16f * p.fade + v * (1.f - 0.22f * p.fade);
	}
	return Clamp01(v);
}

struct LumaParams {
	float shadows = 0.f; // -1..1
	float highlights = 0.f; // -1..1

	[[nodiscard]] bool active() const {
		return shadows != 0.f || highlights != 0.f;
	}
};

[[nodiscard]] float LumaValue(const LumaParams &p, float y) {
	const auto s = p.shadows;
	if (s > 0.f) {
		y += s * 0.25f * 6.75f * y * (1.f - y) * (1.f - y);
	} else if (s < 0.f) {
		y *= 1.f + s * 0.6f * (1.f - y) * (1.f - y);
	}
	y = Clamp01(y);
	const auto h = p.highlights;
	if (h < 0.f) {
		y += h * 0.25f * 6.75f * y * y * (1.f - y);
	} else if (h > 0.f) {
		y = 1.f - (1.f - y) * (1.f - 0.6f * h * y * y);
	}
	return Clamp01(y);
}

struct PointOps {
	bool tone = false;
	std::array<Lut, 3> toneCurves;
	bool luma = false;
	Lut lumaDelta = {};
	bool color = false;
	float saturation = 1.f;
	float vibrance = 0.f;
	std::unique_ptr<CompiledLook> look;

	[[nodiscard]] bool active() const {
		return tone || luma || color || (look && look->active);
	}
};

[[nodiscard]] std::unique_ptr<PointOps> CompilePointOps(
		const EditState &state) {
	auto result = std::make_unique<PointOps>();
	const auto tone = ToneFor(state);
	if (tone.active()) {
		result->tone = true;
		for (auto c = 0; c != 3; ++c) {
			for (auto i = 0; i != kLutSize; ++i) {
				result->toneCurves[c][i] = ToneValue(tone, i / kLutMax, c);
			}
		}
	}
	const auto luma = LumaParams{
		.shadows = state.shadows / 100.f,
		.highlights = state.highlights / 100.f,
	};
	if (luma.active()) {
		result->luma = true;
		for (auto i = 0; i != kLutSize; ++i) {
			const auto y = i / kLutMax;
			result->lumaDelta[i] = LumaValue(luma, y) - y;
		}
	}
	if (state.saturation || state.vibrance) {
		result->color = true;
		result->saturation = 1.f + state.saturation / 100.f;
		result->vibrance = state.vibrance / 100.f;
	}
	result->look = CompileLook(state.filter, state.filterIntensity);
	return result;
}

void ApplyPointOps(const Buffer &image, const PointOps &ops) {
	const auto look = (ops.look && ops.look->active) ? ops.look.get() : nullptr;
	ForEachColor(image, [&](Color &c, int x, int y) {
		if (ops.tone) {
			c.r = ops.toneCurves[0][LutIndex(c.r)];
			c.g = ops.toneCurves[1][LutIndex(c.g)];
			c.b = ops.toneCurves[2][LutIndex(c.b)];
		}
		if (ops.luma) {
			const auto delta = ops.lumaDelta[LutIndex(Luma(c))];
			c.r += delta;
			c.g += delta;
			c.b += delta;
		}
		if (ops.color) {
			const auto y = Luma(c);
			auto factor = ops.saturation;
			if (ops.vibrance != 0.f) {
				const auto high = std::max({ c.r, c.g, c.b });
				const auto low = std::min({ c.r, c.g, c.b });
				const auto current = Clamp01(high - low);
				factor *= 1.f + ops.vibrance * (1.f - current) * (1.f - current);
			}
			c.r = y + (c.r - y) * factor;
			c.g = y + (c.g - y) * factor;
			c.b = y + (c.b - y) * factor;
		}
		if (look) {
			c = ApplyLook(*look, c);
		}
	});
}

void ApplyDetail(
		const Buffer &image,
		float clarity, // -1..1
		float sharpen, // 0..1
		float unit) {
	const auto w = image.width;
	const auto h = image.height;
	const auto luma = LumaPlane(image);
	auto wide = std::vector<float>();
	auto narrow = std::vector<float>();
	if (clarity != 0.f) {
		wide = luma;
		BlurPlane(wide, w, h, std::max(unit * 16.f, 1.f));
	}
	if (sharpen > 0.f) {
		narrow = luma;
		BlurPlane(narrow, w, h, std::clamp(unit * 0.9f, 0.6f, 3.f));
	}
	ForEachColor(image, [&](Color &c, int x, int y) {
		const auto index = ptrdiff_t(y) * w + x;
		const auto l = luma[index];
		auto delta = 0.f;
		if (!wide.empty()) {
			const auto weight = 4.f * l * (1.f - l);
			delta += clarity * 0.9f * (l - wide[index]) * weight;
		}
		if (!narrow.empty()) {
			delta += sharpen * 1.6f * (l - narrow[index]);
		}
		delta = std::clamp(delta, -0.35f, 0.35f);
		c.r += delta;
		c.g += delta;
		c.b += delta;
	});
}

void ApplyFinish(
		const Buffer &image,
		float vignette, // -1..1
		float feather, // 0..1
		float grain, // 0..1
		float grainSize, // 0..1
		float unit) {
	const auto w = image.width;
	const auto h = image.height;
	const auto inner = Mix(0.78f, 0.12f, feather);
	const auto outer = 1.02f;
	const auto hw = w / 2.f;
	const auto hh = h / 2.f;
	const auto field = GrainField(unit * (0.7f + grainSize * 3.3f));
	const auto grainAmount = grain * 0.16f * field.strength();
	ForEachColor(image, [&](Color &c, int x, int y) {
		if (vignette != 0.f) {
			const auto u = (x + 0.5f - hw) / hw;
			const auto v = (y + 0.5f - hh) / hh;
			const auto d = std::sqrt((u * u + v * v) * 0.5f);
			const auto mask = SmoothStep(inner, outer, d);
			if (vignette > 0.f) {
				const auto k = 1.f - vignette * 0.85f * mask;
				c.r *= k;
				c.g *= k;
				c.b *= k;
			} else {
				const auto k = -vignette * 0.75f * mask;
				c.r += (1.f - c.r) * k;
				c.g += (1.f - c.g) * k;
				c.b += (1.f - c.b) * k;
			}
		}
		if (grainAmount > 0.f) {
			const auto l = Clamp01(Luma(c));
			const auto weight = 0.35f + 0.65f * 4.f * l * (1.f - l);
			const auto n = field.at(x, y) * grainAmount * weight;
			c.r += n;
			c.g += n;
			c.b += n;
		}
	});
}

//
// Effects.
//

[[nodiscard]] inline float Amount(const Effect &e) {
	return std::clamp(e.amount, 0, 100) / 100.f;
}

void EffectPixelate(const Buffer &image, const Effect &e, float unit) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto w = image.width;
	const auto h = image.height;
	const auto block = std::max(2, int(std::lround(unit * (3.f + e.size * 0.6f))));
	const auto offsetX = (w % block) ? ((w % block) - block) / 2 : 0;
	const auto offsetY = (h % block) ? ((h % block) - block) / 2 : 0;
	const auto rows = (h - offsetY + block - 1) / block;
	const auto weight = Weight256(t);
	ParallelFor(rows, 1, [&](int from, int till) {
		for (auto by = from; by != till; ++by) {
			const auto y0 = std::max(offsetY + by * block, 0);
			const auto y1 = std::min(offsetY + (by + 1) * block, h);
			for (auto bx = offsetX; bx < w; bx += block) {
				const auto x0 = std::max(bx, 0);
				const auto x1 = std::min(bx + block, w);
				uint64 sa = 0, sr = 0, sg = 0, sb = 0;
				for (auto y = y0; y < y1; ++y) {
					const auto line = image.row(y);
					for (auto x = x0; x < x1; ++x) {
						const auto p = line[x];
						sa += Alpha(p);
						sr += Red(p);
						sg += Green(p);
						sb += Blue(p);
					}
				}
				const auto count = uint64(std::max((x1 - x0) * (y1 - y0), 1));
				const auto half = count / 2;
				const auto average = Pack(
					int((sa + half) / count),
					int((sr + half) / count),
					int((sg + half) / count),
					int((sb + half) / count));
				for (auto y = y0; y < y1; ++y) {
					const auto line = image.row(y);
					for (auto x = x0; x < x1; ++x) {
						line[x] = (weight >= 256)
							? average
							: Interpolate(line[x], average, weight);
					}
				}
			}
		}
	});
}

void EffectGlitch(const Buffer &image, const Effect &e, float unit) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto w = image.width;
	const auto h = image.height;
	auto random = Random(uint64(e.seed) * 7919ULL + 17ULL);
	auto shift = std::vector<int>(h, 0);
	const auto base = int(std::lround(unit * 10.f * t));
	auto split = std::vector<int>(h, base);
	const auto count = 3 + int(t * 14.f);
	for (auto i = 0; i != count; ++i) {
		const auto y0 = int(random.unit() * h);
		const auto size = 0.004f + random.unit() * (0.02f + e.size * 0.0008f);
		const auto height = std::max(1, int(size * h));
		const auto dx = int(std::lround(
			(random.unit() * 2.f - 1.f) * t * 0.12f * w));
		const auto extra = int(std::lround(random.unit() * unit * 18.f * t));
		for (auto y = y0; y < std::min(y0 + height, h); ++y) {
			shift[y] = dx;
			split[y] = base + extra;
		}
	}
	const auto wrap = [&](int x) {
		x %= w;
		return (x < 0) ? (x + w) : x;
	};
	ForRows(w, h, [&](int from, int till) {
		auto line = std::vector<Pixel>(w);
		for (auto y = from; y != till; ++y) {
			const auto target = image.row(y);
			std::copy_n(target, w, line.data());
			const auto s = split[y];
			for (auto x = 0; x != w; ++x) {
				const auto sx = wrap(x - shift[y]);
				const auto p = line[sx];
				const auto a = Alpha(p);
				target[x] = Pack(
					a,
					std::min(Red(line[wrap(sx - s)]), a),
					Green(p),
					std::min(Blue(line[wrap(sx + s)]), a));
			}
		}
	});
}

void EffectVhs(const Buffer &image, const Effect &e, float unit) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto w = image.width;
	const auto h = image.height;
	const auto period = std::max(2, int(std::lround(unit * 3.f)));
	const auto bleed = std::max(1, int(std::lround(unit * (2.f + 6.f * t))));
	const auto chromaShift = int(std::lround(unit * 3.f * t));
	const auto seed = uint32(e.seed) * 2654435761U + 1U;
	auto random = Random(uint64(seed));
	const auto trackTop = int((0.72f + random.unit() * 0.2f) * h);
	const auto trackHeight = std::max(1, int((0.01f + random.unit() * 0.03f) * h));
	const auto phase = random.unit() * 6.2831f;
	ForRows(w, h, [&](int from, int till) {
		auto ys = std::vector<float>(w);
		auto us = std::vector<float>(w);
		auto vs = std::vector<float>(w);
		auto ub = std::vector<float>(w);
		auto vb = std::vector<float>(w);
		auto alpha = std::vector<int>(w);
		auto source = std::vector<Pixel>(w);
		for (auto y = from; y != till; ++y) {
			const auto line = image.row(y);
			std::copy_n(line, w, source.data());
			const auto tracking = (y >= trackTop) && (y < trackTop + trackHeight);
			auto wobble = std::sin(y * 0.021f / std::max(unit, 0.1f) + phase)
				* unit * 1.5f * t;
			if (tracking) {
				wobble += HashSigned(0, uint32(y), seed) * unit * 14.f * t;
			}
			const auto dx = int(std::lround(wobble));
			for (auto x = 0; x != w; ++x) {
				const auto p = source[std::clamp(x - dx, 0, w - 1)];
				const auto a = Alpha(p);
				alpha[x] = a;
				const auto c = a ? Unpremultiply(p, a) : Color();
				const auto l = Luma(c);
				ys[x] = l;
				us[x] = (c.b - l) * 0.5643f;
				vs[x] = (c.r - l) * 0.7133f;
			}
			// Chroma bleed: horizontal box blur shifted to the right.
			const auto window = 2 * bleed + 1;
			auto su = 0.f;
			auto sv = 0.f;
			for (auto k = -bleed; k <= bleed; ++k) {
				const auto index = std::clamp(k - chromaShift, 0, w - 1);
				su += us[index];
				sv += vs[index];
			}
			for (auto x = 0; x != w; ++x) {
				ub[x] = su / window;
				vb[x] = sv / window;
				const auto add = std::clamp(x + bleed + 1 - chromaShift, 0, w - 1);
				const auto sub = std::clamp(x - bleed - chromaShift, 0, w - 1);
				su += us[add] - us[sub];
				sv += vs[add] - vs[sub];
			}
			const auto dark = ((y % period) >= (period + 1) / 2)
				? (1.f - 0.22f * t)
				: 1.f;
			const auto noiseAmount = (tracking ? 0.3f : 0.07f) * t;
			for (auto x = 0; x != w; ++x) {
				const auto a = alpha[x];
				if (!a) {
					line[x] = 0;
					continue;
				}
				auto l = ys[x];
				l += HashSigned(uint32(x), uint32(y), seed) * noiseAmount;
				l = (l * (1.f - 0.08f * t) + 0.04f * t) * dark;
				const auto u = Mix(us[x], ub[x], t) * (1.f - 0.15f * t);
				const auto v = Mix(vs[x], vb[x], t) * (1.f - 0.15f * t);
				const auto r = l + v / 0.7133f;
				const auto b = l + u / 0.5643f;
				const auto g = (l - 0.299f * r - 0.114f * b) / 0.587f;
				line[x] = Premultiply(a, { r, g, b });
			}
		}
	});
}

void EffectPosterize(const Buffer &image, const Effect &e) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto steps = float(std::clamp(e.levels, 2, 16) - 1);
	const auto quantize = [&](float v) {
		return std::round(Clamp01(v) * steps) / steps;
	};
	ForEachColor(image, [&](Color &c, int, int) {
		c = Mix(c, { quantize(c.r), quantize(c.g), quantize(c.b) }, t);
	});
}

void EffectDuotone(const Buffer &image, const Effect &e) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto dark = FromRgb(e.color1);
	const auto light = FromRgb(e.color2);
	ForEachColor(image, [&](Color &c, int, int) {
		const auto y = Clamp01(Luma(c));
		c = Mix(c, Mix(dark, light, y), t);
	});
}

void EffectHalftone(const Buffer &image, const Effect &e, float unit) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto cell = std::max(3.f, unit * (4.f + e.size * 0.28f));
	auto blurred = Copy(image);
	BlurPixels(blurred, cell * 0.3f);
	const auto color = (e.mode != 0);
	struct Screen {
		float cosA = 1.f;
		float sinA = 0.f;
	};
	const auto screen = [&](float degrees) {
		const auto radians = float(degrees * kPi / 180.);
		return Screen{ std::cos(radians), std::sin(radians) };
	};
	const auto angle = float(e.angle);
	const std::array<Screen, 3> screens = color
		? std::array<Screen, 3>{
			screen(angle + 15.f),
			screen(angle + 75.f),
			screen(angle),
		}
		: std::array<Screen, 3>{ screen(angle), screen(angle), screen(angle) };
	const auto inverse = 1.f / cell;
	// Ink coverage of the dot of the screen cell containing the pixel,
	// the dot size comes from the value at the cell center.
	const auto coverage = [&](
			const Screen &s,
			float px,
			float py,
			int channel) {
		const auto u = (px * s.cosA + py * s.sinA) * inverse;
		const auto v = (-px * s.sinA + py * s.cosA) * inverse;
		const auto cu = std::floor(u) + 0.5f;
		const auto cv = std::floor(v) + 0.5f;
		const auto cx = (cu * s.cosA - cv * s.sinA) * cell;
		const auto cy = (cu * s.sinA + cv * s.cosA) * cell;
		const auto p = SampleBilinear(blurred, cx - 0.5f, cy - 0.5f);
		const auto a = Alpha(p);
		const auto c = a ? Unpremultiply(p, a) : Color{ 1.f, 1.f, 1.f };
		const auto value = (channel == 0)
			? c.r
			: (channel == 1)
			? c.g
			: (channel == 2)
			? c.b
			: Luma(c);
		// Dot area follows the ink amount until the dots touch (k = pi / 4),
		// then they grow to cover the whole cell.
		const auto k = Clamp01(1.f - value);
		constexpr auto kTouch = float(kPi / 4.);
		const auto radius = cell * ((k < kTouch)
			? std::sqrt(k / float(kPi))
			: (0.5f + (k - kTouch) / (1.f - kTouch) * 0.2072f));
		const auto distance = std::hypot(u - cu, v - cv) * cell;
		return Clamp01(radius - distance + 0.5f);
	};
	ForEachColor(image, [&](Color &c, int x, int y) {
		const auto px = x + 0.5f;
		const auto py = y + 0.5f;
		auto result = Color();
		if (color) {
			result.r = 1.f - coverage(screens[0], px, py, 0);
			result.g = 1.f - coverage(screens[1], px, py, 1);
			result.b = 1.f - coverage(screens[2], px, py, 2);
		} else {
			const auto ink = coverage(screens[0], px, py, 3);
			const auto value = 1.f - ink * 0.92f;
			result = { value, value, value };
		}
		c = Mix(c, result, t);
	});
}

void EffectEmboss(const Buffer &image, const Effect &e, float unit) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto w = image.width;
	const auto h = image.height;
	const auto luma = LumaPlane(image);
	const auto depth = std::max(1.f, unit * (1.f + e.size * 0.04f));
	const auto radians = e.angle * kPi / 180.;
	auto dx = int(std::lround(std::cos(radians) * depth));
	auto dy = int(std::lround(-std::sin(radians) * depth));
	if (!dx && !dy) {
		dx = 1;
	}
	const auto at = [&](int x, int y) {
		x = std::clamp(x, 0, w - 1);
		y = std::clamp(y, 0, h - 1);
		return luma[ptrdiff_t(y) * w + x];
	};
	ForEachColor(image, [&](Color &c, int x, int y) {
		const auto relief = at(x + dx, y + dy) - at(x - dx, y - dy);
		const auto gray = 0.5f + relief * 1.2f;
		c = Mix(c, { gray, gray, gray }, t);
	});
}

[[nodiscard]] inline float SampleChannel(
		const PixelCopy &copy,
		float fx,
		float fy,
		int shift) {
	fx = std::clamp(fx, 0.f, float(copy.width - 1));
	fy = std::clamp(fy, 0.f, float(copy.height - 1));
	const auto x0 = int(fx);
	const auto y0 = int(fy);
	const auto x1 = std::min(x0 + 1, copy.width - 1);
	const auto y1 = std::min(y0 + 1, copy.height - 1);
	const auto tx = fx - x0;
	const auto ty = fy - y0;
	const auto top = copy.row(y0);
	const auto bottom = copy.row(y1);
	const auto value = [&](Pixel p) {
		return float((p >> shift) & 0xFFU);
	};
	return Mix(
		Mix(value(top[x0]), value(top[x1]), tx),
		Mix(value(bottom[x0]), value(bottom[x1]), tx),
		ty);
}

void EffectChromatic(const Buffer &image, const Effect &e) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto k = t * 0.012f;
	const auto source = Copy(image);
	const auto cx = image.width / 2.f;
	const auto cy = image.height / 2.f;
	ForRows(image.width, image.height, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = image.row(y);
			const auto dy = y + 0.5f - cy;
			for (auto x = 0; x != image.width; ++x) {
				const auto p = line[x];
				const auto a = Alpha(p);
				const auto dx = x + 0.5f - cx;
				const auto r = SampleChannel(
					source,
					cx + dx * (1.f - k) - 0.5f,
					cy + dy * (1.f - k) - 0.5f,
					16);
				const auto b = SampleChannel(
					source,
					cx + dx * (1.f + k) - 0.5f,
					cy + dy * (1.f + k) - 0.5f,
					0);
				line[x] = Pack(
					a,
					std::min(int(r + 0.5f), a),
					Green(p),
					std::min(int(b + 0.5f), a));
			}
		}
	});
}

void EffectGlow(const Buffer &image, const Effect &e, float unit) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto threshold = std::clamp(e.threshold, 0, 100) / 100.f;
	auto bright = Copy(image);
	ForEachColor(bright.view(), [&](Color &c, int, int) {
		const auto k = SmoothStep(threshold - 0.25f, threshold + 0.1f, Luma(c));
		c.r *= k;
		c.g *= k;
		c.b *= k;
	});
	BlurPixels(bright, unit * (4.f + e.size * 0.5f));
	const auto strength = t * 1.2f;
	ForEachColor(image, [&](Color &c, int x, int y) {
		const auto g = bright.row(y)[x];
		constexpr auto k = 1.f / 255.f;
		const auto gr = std::min(Red(g) * k * strength, 1.f);
		const auto gg = std::min(Green(g) * k * strength, 1.f);
		const auto gb = std::min(Blue(g) * k * strength, 1.f);
		c.r = 1.f - (1.f - c.r) * (1.f - gr);
		c.g = 1.f - (1.f - c.g) * (1.f - gg);
		c.b = 1.f - (1.f - c.b) * (1.f - gb);
	});
}

void EffectBlackWhite(const Buffer &image, const Effect &e) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto r = e.red / 100.f;
	const auto g = e.green / 100.f;
	const auto b = e.blue / 100.f;
	ForEachColor(image, [&](Color &c, int, int) {
		const auto y = Clamp01(c.r * r + c.g * g + c.b * b);
		c = Mix(c, { y, y, y }, t);
	});
}

void EffectSepia(const Buffer &image, const Effect &e) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	ForEachColor(image, [&](Color &c, int, int) {
		const auto sepia = Color{
			0.393f * c.r + 0.769f * c.g + 0.189f * c.b,
			0.349f * c.r + 0.686f * c.g + 0.168f * c.b,
			0.272f * c.r + 0.534f * c.g + 0.131f * c.b,
		};
		c = Mix(c, sepia, t);
	});
}

void EffectInvert(const Buffer &image, const Effect &e) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	ForEachColor(image, [&](Color &c, int, int) {
		c = Mix(c, { 1.f - c.r, 1.f - c.g, 1.f - c.b }, t);
	});
}

void EffectFilm(const Buffer &image, const Effect &e, float unit) {
	const auto t = Amount(e);
	const auto grain = std::clamp(e.grain, 0, 100) / 100.f * 0.14f;
	if (t <= 0.f && grain <= 0.f) {
		return;
	}
	auto curve = Lut();
	for (auto i = 0; i != kLutSize; ++i) {
		const auto v = i / kLutMax;
		const auto s = v * v * (3.f - 2.f * v);
		curve[i] = 0.045f + 0.905f * Mix(v, s, 0.45f);
	}
	const auto field = GrainField(unit * 1.4f, kGrainSeed + 7U);
	const auto grainAmount = grain * field.strength();
	ForEachColor(image, [&](Color &c, int x, int y) {
		if (t > 0.f) {
			auto film = Color{
				Clamp01(curve[LutIndex(c.r)] * 1.02f + 0.005f),
				curve[LutIndex(c.g)],
				curve[LutIndex(c.b)] * 0.96f + 0.025f,
			};
			const auto l = Luma(film);
			film.r = l + (film.r - l) * 0.88f;
			film.g = l + (film.g - l) * 0.88f;
			film.b = l + (film.b - l) * 0.88f;
			c = Mix(c, film, t);
		}
		if (grain > 0.f) {
			const auto l = Clamp01(Luma(c));
			const auto n = field.at(x, y)
				* grainAmount
				* (0.4f + 0.6f * 4.f * l * (1.f - l));
			c.r += n;
			c.g += n;
			c.b += n;
		}
	});
}

void EffectLensBlur(const Buffer &image, const Effect &e, float unit) {
	const auto t = Amount(e);
	if (t <= 0.f) {
		return;
	}
	const auto sigma = unit * (1.5f + e.amount * 0.3f);
	auto half = Copy(image);
	auto full = Copy(image);
	BlurPixels(half, sigma * 0.5f);
	BlurPixels(full, sigma);
	const auto w = image.width;
	const auto h = image.height;
	const auto radians = e.angle * kPi / 180.;
	const auto nx = float(-std::sin(radians));
	const auto ny = float(std::cos(radians));
	const auto extent = std::abs(nx) * w + std::abs(ny) * h;
	const auto offset = (std::clamp(e.position, 0, 100) / 100.f - 0.5f)
		* extent;
	const auto centerX = w / 2.f + nx * offset;
	const auto centerY = h / 2.f + ny * offset;
	const auto halfWidth = std::clamp(e.width, 0, 100) / 100.f * 0.5f;
	const auto feather = std::max(
		std::clamp(e.feather, 0, 100) / 100.f * 0.5f,
		0.005f);
	ForRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = image.row(y);
			const auto halfLine = half.row(y);
			const auto fullLine = full.row(y);
			for (auto x = 0; x != w; ++x) {
				const auto d = std::abs(
					(x + 0.5f - centerX) * nx + (y + 0.5f - centerY) * ny)
					/ extent;
				const auto m = SmoothStep(halfWidth, halfWidth + feather, d);
				if (m <= 0.f) {
					continue;
				}
				line[x] = (m < 0.5f)
					? Interpolate(line[x], halfLine[x], Weight256(m * 2.f))
					: Interpolate(
						halfLine[x],
						fullLine[x],
						Weight256(m * 2.f - 1.f));
			}
		}
	});
}

void ApplyEffect(const Buffer &image, const Effect &e, float unit) {
	switch (e.type) {
	case EffectType::Pixelate: EffectPixelate(image, e, unit); break;
	case EffectType::Glitch: EffectGlitch(image, e, unit); break;
	case EffectType::Vhs: EffectVhs(image, e, unit); break;
	case EffectType::Posterize: EffectPosterize(image, e); break;
	case EffectType::Duotone: EffectDuotone(image, e); break;
	case EffectType::Halftone: EffectHalftone(image, e, unit); break;
	case EffectType::Emboss: EffectEmboss(image, e, unit); break;
	case EffectType::ChromaticAberration: EffectChromatic(image, e); break;
	case EffectType::Glow: EffectGlow(image, e, unit); break;
	case EffectType::BlackWhite: EffectBlackWhite(image, e); break;
	case EffectType::Sepia: EffectSepia(image, e); break;
	case EffectType::Invert: EffectInvert(image, e); break;
	case EffectType::Film: EffectFilm(image, e, unit); break;
	case EffectType::LensBlur: EffectLensBlur(image, e, unit); break;
	}
}

//
// Descriptors.
//

using AdjustMember = int EditState::*;

struct AdjustRow {
	AdjustInfo info;
	AdjustMember member = nullptr;
};

[[nodiscard]] const std::vector<AdjustRow> &AdjustRows() {
	static const auto result = std::vector<AdjustRow>{
		{ { Adjust::Exposure, -100, 100, 0, "exposure" }, &EditState::exposure },
		{ { Adjust::Brightness, -100, 100, 0, "brightness" }, &EditState::brightness },
		{ { Adjust::Contrast, -100, 100, 0, "contrast" }, &EditState::contrast },
		{ { Adjust::Highlights, -100, 100, 0, "highlights" }, &EditState::highlights },
		{ { Adjust::Shadows, -100, 100, 0, "shadows" }, &EditState::shadows },
		{ { Adjust::Whites, -100, 100, 0, "whites" }, &EditState::whites },
		{ { Adjust::Blacks, -100, 100, 0, "blacks" }, &EditState::blacks },
		{ { Adjust::Saturation, -100, 100, 0, "saturation" }, &EditState::saturation },
		{ { Adjust::Vibrance, -100, 100, 0, "vibrance" }, &EditState::vibrance },
		{ { Adjust::Temperature, -100, 100, 0, "temperature" }, &EditState::temperature },
		{ { Adjust::Tint, -100, 100, 0, "tint" }, &EditState::tint },
		{ { Adjust::Fade, 0, 100, 0, "fade" }, &EditState::fade },
		{ { Adjust::Clarity, -100, 100, 0, "clarity" }, &EditState::clarity },
		{ { Adjust::Sharpen, 0, 100, 0, "sharpen" }, &EditState::sharpen },
		{ { Adjust::Vignette, -100, 100, 0, "vignette" }, &EditState::vignette },
		{ { Adjust::VignetteFeather, 0, 100, 50, "vignetteFeather" }, &EditState::vignetteFeather },
		{ { Adjust::Grain, 0, 100, 0, "grain" }, &EditState::grain },
		{ { Adjust::GrainSize, 0, 100, 25, "grainSize" }, &EditState::grainSize },
		{ { Adjust::Blur, 0, 100, 0, "blur" }, &EditState::blur },
	};
	return result;
}

[[nodiscard]] const AdjustRow &AdjustRowFor(Adjust id) {
	const auto &rows = AdjustRows();
	const auto index = std::clamp(int(id), 0, int(rows.size()) - 1);
	return rows[index];
}

struct EffectSpec {
	EffectType type = EffectType::Pixelate;
	const char *key = nullptr;
	std::vector<EffectParamInfo> params;
};

[[nodiscard]] EffectParamInfo Param(
		EffectParam param,
		int min,
		int max,
		int defaultValue) {
	return {
		.param = param,
		.min = min,
		.max = max,
		.defaultValue = defaultValue,
		.toggle = (param == EffectParam::Mode),
		.color = (param == EffectParam::Color1)
			|| (param == EffectParam::Color2),
	};
}

[[nodiscard]] const std::vector<EffectSpec> &EffectSpecs() {
	using P = EffectParam;
	static const auto result = std::vector<EffectSpec>{
		{ EffectType::Pixelate, "pixelate", {
			Param(P::Size, 0, 100, 30),
			Param(P::Amount, 0, 100, 100),
		} },
		{ EffectType::Glitch, "glitch", {
			Param(P::Amount, 0, 100, 50),
			Param(P::Size, 0, 100, 40),
			Param(P::Seed, 0, 999, 0),
		} },
		{ EffectType::Vhs, "vhs", {
			Param(P::Amount, 0, 100, 70),
			Param(P::Seed, 0, 999, 0),
		} },
		{ EffectType::Posterize, "posterize", {
			Param(P::Levels, 2, 16, 5),
			Param(P::Amount, 0, 100, 100),
		} },
		{ EffectType::Duotone, "duotone", {
			Param(P::Color1, 0, 0, 0),
			Param(P::Color2, 0, 0, 0),
			Param(P::Amount, 0, 100, 100),
		} },
		{ EffectType::Halftone, "halftone", {
			Param(P::Size, 0, 100, 30),
			Param(P::Angle, 0, 90, 45),
			Param(P::Mode, 0, 1, 0),
			Param(P::Amount, 0, 100, 100),
		} },
		{ EffectType::Emboss, "emboss", {
			Param(P::Amount, 0, 100, 70),
			Param(P::Size, 0, 100, 20),
			Param(P::Angle, 0, 359, 135),
		} },
		{ EffectType::ChromaticAberration, "chromatic", {
			Param(P::Amount, 0, 100, 40),
		} },
		{ EffectType::Glow, "glow", {
			Param(P::Amount, 0, 100, 50),
			Param(P::Size, 0, 100, 50),
			Param(P::Threshold, 0, 100, 60),
		} },
		{ EffectType::BlackWhite, "bw", {
			Param(P::Red, -100, 200, 30),
			Param(P::Green, -100, 200, 59),
			Param(P::Blue, -100, 200, 11),
			Param(P::Amount, 0, 100, 100),
		} },
		{ EffectType::Sepia, "sepia", {
			Param(P::Amount, 0, 100, 100),
		} },
		{ EffectType::Invert, "invert", {
			Param(P::Amount, 0, 100, 100),
		} },
		{ EffectType::Film, "film", {
			Param(P::Amount, 0, 100, 70),
			Param(P::Grain, 0, 100, 35),
		} },
		{ EffectType::LensBlur, "lens_blur", {
			Param(P::Amount, 0, 100, 60),
			Param(P::Position, 0, 100, 50),
			Param(P::Width, 0, 100, 25),
			Param(P::Feather, 0, 100, 40),
			Param(P::Angle, -90, 90, 0),
		} },
	};
	return result;
}

[[nodiscard]] const EffectSpec &EffectSpecFor(EffectType type) {
	const auto &specs = EffectSpecs();
	const auto index = std::clamp(int(type), 0, int(specs.size()) - 1);
	return specs[index];
}

[[nodiscard]] const char *ParamKey(EffectParam param) {
	switch (param) {
	case EffectParam::Amount: return "amount";
	case EffectParam::Size: return "size";
	case EffectParam::Levels: return "levels";
	case EffectParam::Red: return "red";
	case EffectParam::Green: return "green";
	case EffectParam::Blue: return "blue";
	case EffectParam::Position: return "position";
	case EffectParam::Width: return "width";
	case EffectParam::Feather: return "feather";
	case EffectParam::Angle: return "angle";
	case EffectParam::Seed: return "seed";
	case EffectParam::Mode: return "mode";
	case EffectParam::Grain: return "grain";
	case EffectParam::Threshold: return "threshold";
	case EffectParam::Color1: return "color1";
	case EffectParam::Color2: return "color2";
	}
	return "";
}

[[nodiscard]] Effect NormalizedEffect(Effect effect) {
	for (const auto &info : EffectSpecFor(effect.type).params) {
		if (info.color) {
			auto &color = (info.param == EffectParam::Color1)
				? effect.color1
				: effect.color2;
			color |= 0xFF000000U;
		} else {
			effect.setValue(info.param, effect.value(info.param));
		}
	}
	return effect;
}

[[nodiscard]] QString ColorToString(QRgb color) {
	return u"#%1"_q.arg(color & 0xFFFFFFU, 6, 16, QChar('0'));
}

[[nodiscard]] std::optional<QRgb> ColorFromString(const QString &text) {
	if (text.size() != 7 || text[0] != '#') {
		return std::nullopt;
	}
	auto ok = false;
	const auto value = text.mid(1).toUInt(&ok, 16);
	return ok ? std::make_optional(QRgb(value | 0xFF000000U)) : std::nullopt;
}

//
// Geometry.
//

[[nodiscard]] bool IsFullCrop(const QRectF &crop) {
	return (crop.x() <= 0.) && (crop.y() <= 0.)
		&& (crop.x() + crop.width() >= 1.)
		&& (crop.y() + crop.height() >= 1.);
}

[[nodiscard]] QRectF NormalizedCrop(QRectF crop) {
	auto x = crop.x();
	auto y = crop.y();
	auto w = crop.width();
	auto h = crop.height();
	if (!std::isfinite(x) || !std::isfinite(y)
		|| !std::isfinite(w) || !std::isfinite(h)
		|| w <= 0. || h <= 0.) {
		return QRectF(0., 0., 1., 1.);
	}
	x = std::clamp(x, 0., 1. - kMinCrop);
	y = std::clamp(y, 0., 1. - kMinCrop);
	w = std::clamp(w, kMinCrop, 1. - x);
	h = std::clamp(h, kMinCrop, 1. - y);
	return QRectF(x, y, w, h);
}

// Crop in pixels of the (turned, straightened) frame.
[[nodiscard]] QRect CropPixels(QSize frame, const QRectF &normalized) {
	const auto crop = NormalizedCrop(normalized);
	const auto w = frame.width();
	const auto h = frame.height();
	const auto x0 = std::clamp(int(std::lround(crop.x() * w)), 0, w - 1);
	const auto y0 = std::clamp(int(std::lround(crop.y() * h)), 0, h - 1);
	const auto x1 = std::clamp(
		int(std::lround((crop.x() + crop.width()) * w)),
		x0 + 1,
		w);
	const auto y1 = std::clamp(
		int(std::lround((crop.y() + crop.height()) * h)),
		y0 + 1,
		h);
	return QRect(x0, y0, x1 - x0, y1 - y0);
}

[[nodiscard]] int NormalizedTurns(int turns) {
	return ((turns % 4) + 4) % 4;
}

[[nodiscard]] double NormalizedStraighten(double degrees) {
	if (!std::isfinite(degrees)) {
		return 0.;
	}
	const auto result = std::clamp(degrees, -45., 45.);
	return (std::abs(result) < kStraightenEpsilon) ? 0. : result;
}

[[nodiscard]] QTransform OrientTransform(
		QSize source,
		int turns,
		bool flipHorizontal,
		bool flipVertical) {
	const auto w = qreal(source.width());
	const auto h = qreal(source.height());
	auto result = QTransform();
	switch (NormalizedTurns(turns)) {
	case 1: result = QTransform(0, 1, -1, 0, h, 0); break;
	case 2: result = QTransform(-1, 0, 0, -1, w, h); break;
	case 3: result = QTransform(0, -1, 1, 0, 0, w); break;
	}
	const auto oriented = OrientedSize(source, turns);
	if (flipHorizontal) {
		result *= QTransform(-1, 0, 0, 1, oriented.width(), 0);
	}
	if (flipVertical) {
		result *= QTransform(1, 0, 0, -1, 0, oriented.height());
	}
	return result;
}

[[nodiscard]] QTransform StraightenTransform(QSize frame, double degrees) {
	if (degrees == 0.) {
		return QTransform();
	}
	const auto cx = frame.width() / 2.;
	const auto cy = frame.height() / 2.;
	const auto scale = StraightenScale(QSizeF(frame), degrees);
	return QTransform::fromTranslate(-cx, -cy)
		* QTransform().rotate(degrees)
		* QTransform::fromScale(scale, scale)
		* QTransform::fromTranslate(cx, cy);
}

struct GeometryPlan {
	QSize source;
	QSize oriented;
	QRect crop; // In the oriented / straightened frame.
	QSize output;
	int turns = 0;
	bool flipHorizontal = false;
	bool flipVertical = false;
	double straighten = 0.;

	[[nodiscard]] bool identity() const {
		return !turns
			&& !flipHorizontal
			&& !flipVertical
			&& straighten == 0.
			&& crop == QRect(QPoint(), source)
			&& output == source;
	}
	[[nodiscard]] QTransform transform() const {
		return OrientTransform(source, turns, flipHorizontal, flipVertical)
			* StraightenTransform(oriented, straighten)
			* QTransform::fromTranslate(-crop.x(), -crop.y())
			* QTransform::fromScale(
				output.width() / qreal(crop.width()),
				output.height() / qreal(crop.height()));
	}
};

[[nodiscard]] GeometryPlan PlanGeometry(
		QSize source,
		const EditState &state,
		QSize maxSize) {
	auto result = GeometryPlan();
	result.source = source;
	result.turns = NormalizedTurns(state.quarterTurns);
	result.flipHorizontal = state.flipHorizontal;
	result.flipVertical = state.flipVertical;
	result.straighten = NormalizedStraighten(state.straighten);
	result.oriented = OrientedSize(source, result.turns);
	result.crop = CropPixels(result.oriented, state.crop);
	result.output = FitSize(result.crop.size(), maxSize);
	return result;
}

// Exact crop + turns + flips (no resampling), then a smooth downscale.
[[nodiscard]] QImage RenderExactGeometry(
		const QImage &source,
		const GeometryPlan &plan) {
	const auto orient = OrientTransform(
		plan.source,
		plan.turns,
		plan.flipHorizontal,
		plan.flipVertical);
	const auto region = orient.inverted().mapRect(
		QRectF(plan.crop)).toAlignedRect() & source.rect();
	auto part = (region == source.rect()) ? source : source.copy(region);
	const auto turned = (plan.turns % 2 != 0);
	const auto partTarget = turned
		? plan.output.transposed()
		: plan.output;
	if (part.size() != partTarget) {
		part = part.scaled(
			partTarget,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
		if (part.format() != QImage::Format_ARGB32_Premultiplied) {
			part = std::move(part).convertToFormat(
				QImage::Format_ARGB32_Premultiplied);
		}
	}
	if (!plan.turns && !plan.flipHorizontal && !plan.flipVertical) {
		return part;
	}
	auto result = QImage(plan.output, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull() || part.isNull()) {
		return QImage();
	}
	// Integer map of an output pixel to a part pixel.
	const auto pw = part.width();
	const auto ph = part.height();
	const auto ow = plan.output.width();
	const auto oh = plan.output.height();
	struct Map {
		int x0 = 0, xx = 0, xy = 0;
		int y0 = 0, yx = 0, yy = 0;
	};
	// Oriented pixel (ox, oy) from part pixel, before flips.
	auto map = Map();
	switch (plan.turns) {
	case 0: map = { 0, 1, 0, 0, 0, 1 }; break; // sx = ox, sy = oy
	case 1: map = { 0, 0, 1, ph - 1, -1, 0 }; break; // sx = oy, sy = h-1-ox
	case 2: map = { pw - 1, -1, 0, ph - 1, 0, -1 }; break;
	case 3: map = { pw - 1, 0, -1, 0, 1, 0 }; break; // sx = w-1-oy, sy = ox
	}
	// Flips: ox = ow - 1 - i, oy = oh - 1 - j.
	if (plan.flipHorizontal) {
		map.x0 += map.xx * (ow - 1);
		map.y0 += map.yx * (ow - 1);
		map.xx = -map.xx;
		map.yx = -map.yx;
	}
	if (plan.flipVertical) {
		map.x0 += map.xy * (oh - 1);
		map.y0 += map.yy * (oh - 1);
		map.xy = -map.xy;
		map.yy = -map.yy;
	}
	const auto from = reinterpret_cast<const Pixel*>(part.constBits());
	const auto fromStride = ptrdiff_t(part.bytesPerLine() / 4);
	auto target = Wrap(result);
	const auto step = map.xx + map.yx * fromStride;
	ForRows(ow, oh, [&](int top, int bottom) {
		for (auto j = top; j != bottom; ++j) {
			const auto line = target.row(j);
			auto sx = map.x0 + map.xy * j;
			auto sy = map.y0 + map.yy * j;
			auto pointer = from + sy * fromStride + sx;
			for (auto i = 0; i != ow; ++i) {
				line[i] = *pointer;
				pointer += step;
			}
		}
	});
	return result;
}

// Straighten: one bilinear resampling pass (after an area downscale
// of the needed source region when shrinking a lot).
[[nodiscard]] QImage RenderRotatedGeometry(
		const QImage &source,
		const GeometryPlan &plan) {
	auto result = QImage(plan.output, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	const auto forward = plan.transform();
	auto invertible = false;
	const auto backward = forward.inverted(&invertible);
	if (!invertible) {
		return QImage();
	}
	auto region = backward.mapRect(QRectF(QPointF(), QSizeF(plan.output)))
		.toAlignedRect()
		.adjusted(-2, -2, 2, 2)
		& source.rect();
	if (region.isEmpty()) {
		region = source.rect();
	}
	// Uniform scale from source to output.
	const auto scale = std::sqrt(std::abs(forward.determinant()));
	auto sample = source;
	auto toSample = QTransform::fromTranslate(-region.x(), -region.y());
	if (scale < 0.6) {
		const auto size = QSize(
			std::max(1, int(std::lround(region.width() * scale))),
			std::max(1, int(std::lround(region.height() * scale))));
		sample = source.copy(region).scaled(
			size,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
		if (sample.format() != QImage::Format_ARGB32_Premultiplied) {
			sample = std::move(sample).convertToFormat(
				QImage::Format_ARGB32_Premultiplied);
		}
		toSample *= QTransform::fromScale(
			size.width() / qreal(region.width()),
			size.height() / qreal(region.height()));
	} else {
		toSample = QTransform();
	}
	if (sample.isNull()) {
		return QImage();
	}
	// Output point -> sample point.
	const auto map = backward * toSample;
	const auto data = reinterpret_cast<const Pixel*>(sample.constBits());
	const auto sw = sample.width();
	const auto sh = sample.height();
	const auto stride = int(sample.bytesPerLine() / 4);
	auto target = Wrap(result);
	ForRows(target.width, target.height, [&](int from, int till) {
		for (auto j = from; j != till; ++j) {
			const auto line = target.row(j);
			const auto start = map.map(QPointF(0.5, j + 0.5));
			auto u = start.x() - 0.5;
			auto v = start.y() - 0.5;
			for (auto i = 0; i != target.width; ++i) {
				line[i] = SampleBilinear(data, sw, sh, stride, float(u), float(v));
				u += map.m11();
				v += map.m12();
			}
		}
	});
	return result;
}

[[nodiscard]] QImage ToPremultiplied(const QImage &image) {
	return (image.format() == QImage::Format_ARGB32_Premultiplied)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

[[nodiscard]] QImage RenderGeometry(
		const QImage &source,
		const GeometryPlan &plan) {
	if (plan.identity()) {
		return source;
	}
	return (plan.straighten == 0.)
		? RenderExactGeometry(source, plan)
		: RenderRotatedGeometry(source, plan);
}

[[nodiscard]] bool Cancelled(const std::atomic<bool> *cancelled) {
	return cancelled && cancelled->load(std::memory_order_relaxed);
}

// Everything after the geometry, in place. False if cancelled.
[[nodiscard]] bool ApplyPipeline(
		QImage &image,
		const EditState &state,
		const std::atomic<bool> *cancelled,
		bool hasTransparency) {
	if (image.isNull()) {
		return true;
	}
	const auto buffer = Wrap(image);
	const auto unit = UnitFor(buffer.width, buffer.height);

	const auto ops = CompilePointOps(state);
	if (ops->active()) {
		ApplyPointOps(buffer, *ops);
	}
	if (Cancelled(cancelled)) {
		return false;
	}
	if (state.clarity || state.sharpen) {
		ApplyDetail(
			buffer,
			state.clarity / 100.f,
			state.sharpen / 100.f,
			unit);
		if (Cancelled(cancelled)) {
			return false;
		}
	}
	if (state.blur > 0) {
		BlurPixels(buffer, unit * state.blur * 0.4f);
		if (Cancelled(cancelled)) {
			return false;
		}
	}
	for (const auto &effect : state.effects) {
		if (effect.enabled) {
			ApplyEffect(buffer, effect, unit);
			if (Cancelled(cancelled)) {
				return false;
			}
		}
	}
	const auto lookVignette = ops->look ? ops->look->vignette : 0.f;
	const auto lookGrain = ops->look ? ops->look->grain : 0.f;
	const auto vignette = std::clamp(
		state.vignette / 100.f + lookVignette,
		-1.f,
		1.f);
	const auto grain = std::clamp(state.grain / 100.f + lookGrain, 0.f, 1.f);
	if (vignette != 0.f || grain > 0.f) {
		ApplyFinish(
			buffer,
			vignette,
			state.vignetteFeather / 100.f,
			grain,
			state.grainSize / 100.f,
			unit);
	}
	if (hasTransparency) {
		FixPremultiplied(buffer);
	}
	return !Cancelled(cancelled);
}

[[nodiscard]] QImage CenterSquare(QImage image, int side) {
	if (image.isNull()) {
		return image;
	}
	const auto w = image.width();
	const auto h = image.height();
	const auto scale = side / double(std::min(w, h));
	const auto scaled = QSize(
		std::max(side, int(std::lround(w * scale))),
		std::max(side, int(std::lround(h * scale))));
	if (scaled != image.size()) {
		image = image.scaled(
			scaled,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}
	image = image.copy(
		(image.width() - side) / 2,
		(image.height() - side) / 2,
		side,
		side);
	return ToPremultiplied(image);
}

// Geometry for thumbnails: rendered so that the shorter side is at least
// side pixels (when possible), then center-cropped to a square.
[[nodiscard]] QImage ThumbnailBase(
		const QImage &source,
		const EditState &state,
		int side) {
	const auto full = OutputSize(source.size(), state);
	const auto shorter = std::max(std::min(full.width(), full.height()), 1);
	const auto scale = std::min(1., side / double(shorter));
	const auto limit = QSize(
		std::max(1, int(std::ceil(full.width() * scale))),
		std::max(1, int(std::ceil(full.height() * scale))));
	const auto plan = PlanGeometry(source.size(), state, limit);
	return CenterSquare(RenderGeometry(source, plan), side);
}

//
// Self-test helpers.
//

[[nodiscard]] QImage SyntheticImage(int width, int height, bool alpha) {
	auto result = QImage(
		QSize(width, height),
		QImage::Format_ARGB32_Premultiplied);
	auto buffer = Wrap(result);
	ForRows(width, height, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = buffer.row(y);
			for (auto x = 0; x != width; ++x) {
				const auto fx = x / float(width);
				const auto fy = y / float(height);
				const auto n = HashSigned(uint32(x), uint32(y), 99U) * 0.05f;
				auto c = Color{
					fx + n,
					fy + n,
					0.5f + 0.4f * std::sin(fx * 9.f + fy * 5.f) + n,
				};
				const auto a = alpha
					? std::clamp(int(255 * (0.2f + 0.8f * fx)), 0, 255)
					: 255;
				line[x] = a ? Premultiply(a, c) : 0;
			}
		}
	});
	return result;
}

[[nodiscard]] double MeanLuma(const QImage &image) {
	const auto converted = ToPremultiplied(image);
	auto sum = 0.;
	auto count = 0.;
	for (auto y = 0; y != converted.height(); ++y) {
		const auto line = reinterpret_cast<const Pixel*>(
			converted.constScanLine(y));
		for (auto x = 0; x != converted.width(); ++x) {
			const auto a = Alpha(line[x]);
			if (a) {
				sum += Luma(Unpremultiply(line[x], a));
				count += 1.;
			}
		}
	}
	return count ? (sum / count) : 0.;
}

[[nodiscard]] double LumaDeviation(const QImage &image) {
	const auto mean = MeanLuma(image);
	const auto converted = ToPremultiplied(image);
	auto sum = 0.;
	auto count = 0.;
	for (auto y = 0; y != converted.height(); ++y) {
		const auto line = reinterpret_cast<const Pixel*>(
			converted.constScanLine(y));
		for (auto x = 0; x != converted.width(); ++x) {
			const auto a = Alpha(line[x]);
			if (a) {
				const auto d = Luma(Unpremultiply(line[x], a)) - mean;
				sum += d * d;
				count += 1.;
			}
		}
	}
	return count ? std::sqrt(sum / count) : 0.;
}

[[nodiscard]] bool SamePixels(const QImage &a, const QImage &b) {
	if (a.size() != b.size()) {
		return false;
	}
	const auto first = ToPremultiplied(a);
	const auto second = ToPremultiplied(b);
	const auto bytes = size_t(first.width()) * 4;
	for (auto y = 0; y != first.height(); ++y) {
		if (memcmp(first.constScanLine(y), second.constScanLine(y), bytes)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool ValidPremultiplied(const QImage &image) {
	const auto converted = ToPremultiplied(image);
	for (auto y = 0; y != converted.height(); ++y) {
		const auto line = reinterpret_cast<const Pixel*>(
			converted.constScanLine(y));
		for (auto x = 0; x != converted.width(); ++x) {
			const auto p = line[x];
			const auto a = Alpha(p);
			if (Red(p) > a || Green(p) > a || Blue(p) > a) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] bool AllOpaque(const QImage &image) {
	return !HasTransparency(image);
}

[[nodiscard]] bool HasDetail(const QImage &image) {
	return LumaDeviation(image) > 0.002;
}

} // namespace

//
// EditState and descriptors.
//

int EditState::value(Adjust id) const {
	return this->*(AdjustRowFor(id).member);
}

void EditState::setValue(Adjust id, int value) {
	const auto &row = AdjustRowFor(id);
	this->*(row.member) = std::clamp(value, row.info.min, row.info.max);
}

int Effect::value(EffectParam param) const {
	switch (param) {
	case EffectParam::Amount: return amount;
	case EffectParam::Size: return size;
	case EffectParam::Levels: return levels;
	case EffectParam::Red: return red;
	case EffectParam::Green: return green;
	case EffectParam::Blue: return blue;
	case EffectParam::Position: return position;
	case EffectParam::Width: return width;
	case EffectParam::Feather: return feather;
	case EffectParam::Angle: return angle;
	case EffectParam::Seed: return seed;
	case EffectParam::Mode: return mode;
	case EffectParam::Grain: return grain;
	case EffectParam::Threshold: return threshold;
	case EffectParam::Color1:
	case EffectParam::Color2: return 0;
	}
	return 0;
}

void Effect::setValue(EffectParam param, int value) {
	auto min = 0;
	auto max = 100;
	auto found = false;
	for (const auto &info : EffectSpecFor(type).params) {
		if (info.param == param) {
			min = info.min;
			max = info.max;
			found = true;
			break;
		}
	}
	if (found) {
		value = std::clamp(value, min, max);
	}
	switch (param) {
	case EffectParam::Amount: amount = value; break;
	case EffectParam::Size: size = value; break;
	case EffectParam::Levels: levels = value; break;
	case EffectParam::Red: red = value; break;
	case EffectParam::Green: green = value; break;
	case EffectParam::Blue: blue = value; break;
	case EffectParam::Position: position = value; break;
	case EffectParam::Width: width = value; break;
	case EffectParam::Feather: feather = value; break;
	case EffectParam::Angle: angle = value; break;
	case EffectParam::Seed: seed = value; break;
	case EffectParam::Mode: mode = value; break;
	case EffectParam::Grain: grain = value; break;
	case EffectParam::Threshold: threshold = value; break;
	case EffectParam::Color1:
	case EffectParam::Color2: break;
	}
}

bool operator==(const Effect &a, const Effect &b) {
	if (a.type != b.type || a.enabled != b.enabled) {
		return false;
	}
	for (const auto &info : EffectSpecFor(a.type).params) {
		if (info.param == EffectParam::Color1) {
			if ((a.color1 & 0xFFFFFFU) != (b.color1 & 0xFFFFFFU)) {
				return false;
			}
		} else if (info.param == EffectParam::Color2) {
			if ((a.color2 & 0xFFFFFFU) != (b.color2 & 0xFFFFFFU)) {
				return false;
			}
		} else if (a.value(info.param) != b.value(info.param)) {
			return false;
		}
	}
	return true;
}

const std::vector<AdjustInfo> &AdjustList() {
	static const auto result = [] {
		auto list = std::vector<AdjustInfo>();
		for (const auto &row : AdjustRows()) {
			list.push_back(row.info);
		}
		return list;
	}();
	return result;
}

const AdjustInfo &AdjustDescriptor(Adjust id) {
	return AdjustRowFor(id).info;
}

QString AdjustName(Adjust id) {
	switch (id) {
	case Adjust::Exposure:
		return tr::lng_oblivion_photo_adjust_exposure(tr::now);
	case Adjust::Brightness:
		return tr::lng_oblivion_photo_adjust_brightness(tr::now);
	case Adjust::Contrast:
		return tr::lng_oblivion_photo_adjust_contrast(tr::now);
	case Adjust::Highlights:
		return tr::lng_oblivion_photo_adjust_highlights(tr::now);
	case Adjust::Shadows:
		return tr::lng_oblivion_photo_adjust_shadows(tr::now);
	case Adjust::Whites:
		return tr::lng_oblivion_photo_adjust_whites(tr::now);
	case Adjust::Blacks:
		return tr::lng_oblivion_photo_adjust_blacks(tr::now);
	case Adjust::Saturation:
		return tr::lng_oblivion_photo_adjust_saturation(tr::now);
	case Adjust::Vibrance:
		return tr::lng_oblivion_photo_adjust_vibrance(tr::now);
	case Adjust::Temperature:
		return tr::lng_oblivion_photo_adjust_temperature(tr::now);
	case Adjust::Tint:
		return tr::lng_oblivion_photo_adjust_tint(tr::now);
	case Adjust::Fade:
		return tr::lng_oblivion_photo_adjust_fade(tr::now);
	case Adjust::Clarity:
		return tr::lng_oblivion_photo_adjust_clarity(tr::now);
	case Adjust::Sharpen:
		return tr::lng_oblivion_photo_adjust_sharpen(tr::now);
	case Adjust::Vignette:
		return tr::lng_oblivion_photo_adjust_vignette(tr::now);
	case Adjust::VignetteFeather:
		return tr::lng_oblivion_photo_adjust_vignette_feather(tr::now);
	case Adjust::Grain:
		return tr::lng_oblivion_photo_adjust_grain(tr::now);
	case Adjust::GrainSize:
		return tr::lng_oblivion_photo_adjust_grain_size(tr::now);
	case Adjust::Blur:
		return tr::lng_oblivion_photo_adjust_blur(tr::now);
	}
	return QString();
}

const std::vector<EffectType> &EffectTypes() {
	static const auto result = [] {
		auto list = std::vector<EffectType>();
		for (const auto &spec : EffectSpecs()) {
			list.push_back(spec.type);
		}
		return list;
	}();
	return result;
}

const std::vector<EffectParamInfo> &EffectParams(EffectType type) {
	return EffectSpecFor(type).params;
}

Effect DefaultEffect(EffectType type) {
	auto result = Effect();
	result.type = type;
	for (const auto &info : EffectParams(type)) {
		if (!info.color) {
			result.setValue(info.param, info.defaultValue);
		}
	}
	return result;
}

QString EffectKey(EffectType type) {
	return QString::fromLatin1(EffectSpecFor(type).key);
}

std::optional<EffectType> EffectFromKey(const QString &key) {
	for (const auto &spec : EffectSpecs()) {
		if (key == QLatin1String(spec.key)) {
			return spec.type;
		}
	}
	return std::nullopt;
}

QString EffectName(EffectType type) {
	switch (type) {
	case EffectType::Pixelate:
		return tr::lng_oblivion_photo_effect_pixelate(tr::now);
	case EffectType::Glitch:
		return tr::lng_oblivion_photo_effect_glitch(tr::now);
	case EffectType::Vhs:
		return tr::lng_oblivion_photo_effect_vhs(tr::now);
	case EffectType::Posterize:
		return tr::lng_oblivion_photo_effect_posterize(tr::now);
	case EffectType::Duotone:
		return tr::lng_oblivion_photo_effect_duotone(tr::now);
	case EffectType::Halftone:
		return tr::lng_oblivion_photo_effect_halftone(tr::now);
	case EffectType::Emboss:
		return tr::lng_oblivion_photo_effect_emboss(tr::now);
	case EffectType::ChromaticAberration:
		return tr::lng_oblivion_photo_effect_chromatic(tr::now);
	case EffectType::Glow:
		return tr::lng_oblivion_photo_effect_glow(tr::now);
	case EffectType::BlackWhite:
		return tr::lng_oblivion_photo_effect_bw(tr::now);
	case EffectType::Sepia:
		return tr::lng_oblivion_photo_effect_sepia(tr::now);
	case EffectType::Invert:
		return tr::lng_oblivion_photo_effect_invert(tr::now);
	case EffectType::Film:
		return tr::lng_oblivion_photo_effect_film(tr::now);
	case EffectType::LensBlur:
		return tr::lng_oblivion_photo_effect_lens_blur(tr::now);
	}
	return QString();
}

QString EffectParamName(EffectParam param) {
	switch (param) {
	case EffectParam::Amount:
		return tr::lng_oblivion_photo_param_amount(tr::now);
	case EffectParam::Size:
		return tr::lng_oblivion_photo_param_size(tr::now);
	case EffectParam::Levels:
		return tr::lng_oblivion_photo_param_levels(tr::now);
	case EffectParam::Red:
		return tr::lng_oblivion_photo_param_red(tr::now);
	case EffectParam::Green:
		return tr::lng_oblivion_photo_param_green(tr::now);
	case EffectParam::Blue:
		return tr::lng_oblivion_photo_param_blue(tr::now);
	case EffectParam::Position:
		return tr::lng_oblivion_photo_param_position(tr::now);
	case EffectParam::Width:
		return tr::lng_oblivion_photo_param_width(tr::now);
	case EffectParam::Feather:
		return tr::lng_oblivion_photo_param_feather(tr::now);
	case EffectParam::Angle:
		return tr::lng_oblivion_photo_param_angle(tr::now);
	case EffectParam::Seed:
		return tr::lng_oblivion_photo_param_seed(tr::now);
	case EffectParam::Mode:
		return tr::lng_oblivion_photo_param_mode(tr::now);
	case EffectParam::Grain:
		return tr::lng_oblivion_photo_param_grain(tr::now);
	case EffectParam::Threshold:
		return tr::lng_oblivion_photo_param_threshold(tr::now);
	case EffectParam::Color1:
		return tr::lng_oblivion_photo_param_color1(tr::now);
	case EffectParam::Color2:
		return tr::lng_oblivion_photo_param_color2(tr::now);
	}
	return QString();
}

const std::vector<QString> &FilterIds() {
	static const auto result = [] {
		auto list = std::vector<QString>();
		for (const auto &look : Looks()) {
			list.push_back(QString::fromLatin1(look.id));
		}
		return list;
	}();
	return result;
}

bool FilterExists(const QString &id) {
	return FindLook(id) != nullptr;
}

QString FilterName(const QString &id) {
	const auto key = id.toLatin1();
	const auto is = [&](const char *value) {
		return key == value;
	};
	if (is("bw")) {
		return tr::lng_oblivion_photo_filter_bw(tr::now);
	} else if (is("bw_contrast")) {
		return tr::lng_oblivion_photo_filter_bw_contrast(tr::now);
	} else if (is("sepia")) {
		return tr::lng_oblivion_photo_filter_sepia(tr::now);
	} else if (is("vintage")) {
		return tr::lng_oblivion_photo_filter_vintage(tr::now);
	} else if (is("film")) {
		return tr::lng_oblivion_photo_filter_film(tr::now);
	} else if (is("warm")) {
		return tr::lng_oblivion_photo_filter_warm(tr::now);
	} else if (is("cool")) {
		return tr::lng_oblivion_photo_filter_cool(tr::now);
	} else if (is("cinema")) {
		return tr::lng_oblivion_photo_filter_cinema(tr::now);
	} else if (is("faded")) {
		return tr::lng_oblivion_photo_filter_faded(tr::now);
	} else if (is("noir")) {
		return tr::lng_oblivion_photo_filter_noir(tr::now);
	} else if (is("pastel")) {
		return tr::lng_oblivion_photo_filter_pastel(tr::now);
	} else if (is("vivid")) {
		return tr::lng_oblivion_photo_filter_vivid(tr::now);
	} else if (is("sunset")) {
		return tr::lng_oblivion_photo_filter_sunset(tr::now);
	} else if (is("moonlight")) {
		return tr::lng_oblivion_photo_filter_moonlight(tr::now);
	} else if (is("cross")) {
		return tr::lng_oblivion_photo_filter_cross(tr::now);
	} else if (is("lomo")) {
		return tr::lng_oblivion_photo_filter_lomo(tr::now);
	} else if (is("retro")) {
		return tr::lng_oblivion_photo_filter_retro(tr::now);
	} else if (is("drama")) {
		return tr::lng_oblivion_photo_filter_drama(tr::now);
	}
	return tr::lng_oblivion_photo_filter_original(tr::now);
}

bool HasGeometry(const EditState &state) {
	return !IsFullCrop(NormalizedCrop(state.crop))
		|| NormalizedTurns(state.quarterTurns) != 0
		|| NormalizedStraighten(state.straighten) != 0.
		|| state.flipHorizontal
		|| state.flipVertical;
}

bool HasColorEdits(const EditState &state) {
	for (const auto &row : AdjustRows()) {
		const auto id = row.info.id;
		if (id == Adjust::VignetteFeather || id == Adjust::GrainSize) {
			continue;
		} else if (state.*(row.member) != row.info.defaultValue) {
			return true;
		}
	}
	const auto look = FindLook(state.filter);
	if (look && qstrcmp(look->id, "original") && state.filterIntensity > 0) {
		return true;
	}
	for (const auto &effect : state.effects) {
		if (effect.enabled) {
			return true;
		}
	}
	return false;
}

bool IsIdentity(const EditState &state) {
	return !HasGeometry(state) && !HasColorEdits(state);
}

EditState Normalized(EditState state) {
	state.crop = NormalizedCrop(state.crop);
	if (IsFullCrop(state.crop)) {
		state.crop = QRectF(0., 0., 1., 1.);
	}
	state.quarterTurns = NormalizedTurns(state.quarterTurns);
	state.straighten = NormalizedStraighten(state.straighten);
	for (const auto &row : AdjustRows()) {
		auto &value = state.*(row.member);
		value = std::clamp(value, row.info.min, row.info.max);
	}
	if (state.filter.isEmpty()) {
		state.filter = kOriginalFilter;
	}
	state.filterIntensity = std::clamp(state.filterIntensity, 0, 100);
	if (state.effects.size() > size_t(kMaxEffects)) {
		state.effects.resize(kMaxEffects);
	}
	for (auto &effect : state.effects) {
		effect = NormalizedEffect(effect);
	}
	return state;
}

EditState WithoutGeometry(EditState state) {
	state.crop = QRectF(0., 0., 1., 1.);
	state.quarterTurns = 0;
	state.straighten = 0.;
	state.flipHorizontal = false;
	state.flipVertical = false;
	return state;
}

QJsonObject ToJson(const EditState &original) {
	const auto state = Normalized(original);
	auto result = QJsonObject();
	result.insert(u"v"_q, 1);
	if (!IsFullCrop(state.crop)) {
		result.insert(u"crop"_q, QJsonArray{
			state.crop.x(),
			state.crop.y(),
			state.crop.width(),
			state.crop.height(),
		});
	}
	if (state.quarterTurns) {
		result.insert(u"rotate"_q, state.quarterTurns);
	}
	if (state.straighten != 0.) {
		result.insert(u"straighten"_q, state.straighten);
	}
	if (state.flipHorizontal) {
		result.insert(u"flipH"_q, true);
	}
	if (state.flipVertical) {
		result.insert(u"flipV"_q, true);
	}
	auto adjust = QJsonObject();
	for (const auto &row : AdjustRows()) {
		const auto value = state.*(row.member);
		if (value != row.info.defaultValue) {
			adjust.insert(QString::fromLatin1(row.info.key), value);
		}
	}
	if (!adjust.isEmpty()) {
		result.insert(u"adjust"_q, adjust);
	}
	if (state.filter != kOriginalFilter) {
		result.insert(u"filter"_q, state.filter);
	}
	if (state.filterIntensity != 100) {
		result.insert(u"filterIntensity"_q, state.filterIntensity);
	}
	if (!state.effects.empty()) {
		auto effects = QJsonArray();
		for (const auto &effect : state.effects) {
			auto object = QJsonObject();
			object.insert(u"type"_q, EffectKey(effect.type));
			if (!effect.enabled) {
				object.insert(u"enabled"_q, false);
			}
			for (const auto &info : EffectParams(effect.type)) {
				const auto key = QString::fromLatin1(ParamKey(info.param));
				if (info.param == EffectParam::Color1) {
					object.insert(key, ColorToString(effect.color1));
				} else if (info.param == EffectParam::Color2) {
					object.insert(key, ColorToString(effect.color2));
				} else {
					object.insert(key, effect.value(info.param));
				}
			}
			effects.push_back(object);
		}
		result.insert(u"effects"_q, effects);
	}
	return result;
}

EditState FromJson(const QJsonObject &object) {
	auto result = EditState();
	const auto integer = [](const QJsonValue &value, int fallback) {
		if (!value.isDouble()) {
			return fallback;
		}
		const auto number = value.toDouble();
		return std::isfinite(number)
			? int(std::clamp(std::round(number), -1e6, 1e6))
			: fallback;
	};
	const auto crop = object.value(u"crop"_q).toArray();
	if (crop.size() == 4) {
		result.crop = QRectF(
			crop[0].toDouble(0.),
			crop[1].toDouble(0.),
			crop[2].toDouble(1.),
			crop[3].toDouble(1.));
	}
	result.quarterTurns = integer(object.value(u"rotate"_q), 0);
	result.straighten = object.value(u"straighten"_q).toDouble(0.);
	result.flipHorizontal = object.value(u"flipH"_q).toBool(false);
	result.flipVertical = object.value(u"flipV"_q).toBool(false);
	const auto adjust = object.value(u"adjust"_q).toObject();
	for (const auto &row : AdjustRows()) {
		result.*(row.member) = integer(
			adjust.value(QString::fromLatin1(row.info.key)),
			row.info.defaultValue);
	}
	result.filter = object.value(u"filter"_q).toString(kOriginalFilter);
	result.filterIntensity = integer(
		object.value(u"filterIntensity"_q),
		100);
	for (const auto &value : object.value(u"effects"_q).toArray()) {
		const auto item = value.toObject();
		const auto type = EffectFromKey(item.value(u"type"_q).toString());
		if (!type) {
			continue;
		}
		auto effect = DefaultEffect(*type);
		effect.enabled = item.value(u"enabled"_q).toBool(true);
		for (const auto &info : EffectParams(*type)) {
			const auto field = item.value(
				QString::fromLatin1(ParamKey(info.param)));
			if (info.param == EffectParam::Color1) {
				if (const auto color = ColorFromString(field.toString())) {
					effect.color1 = *color;
				}
			} else if (info.param == EffectParam::Color2) {
				if (const auto color = ColorFromString(field.toString())) {
					effect.color2 = *color;
				}
			} else {
				effect.setValue(
					info.param,
					integer(field, info.defaultValue));
			}
		}
		result.effects.push_back(effect);
	}
	return Normalized(std::move(result));
}

QByteArray Serialize(const EditState &state) {
	return QJsonDocument(ToJson(state)).toJson(QJsonDocument::Compact);
}

std::optional<EditState> Deserialize(const QByteArray &json) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	return FromJson(document.object());
}

//
// Geometry helpers.
//

QSize OrientedSize(QSize source, int quarterTurns) {
	return (NormalizedTurns(quarterTurns) % 2) ? source.transposed() : source;
}

double StraightenScale(QSizeF frame, double degrees) {
	const auto w = frame.width();
	const auto h = frame.height();
	if (w <= 0. || h <= 0. || !std::isfinite(degrees)) {
		return 1.;
	}
	const auto radians = std::abs(std::clamp(degrees, -45., 45.)) * kPi / 180.;
	const auto c = std::cos(radians);
	const auto s = std::sin(radians);
	return std::max((w * c + h * s) / w, (w * s + h * c) / h);
}

QSize FitSize(QSize size, QSize maxSize) {
	if (size.isEmpty()
		|| maxSize.width() <= 0
		|| maxSize.height() <= 0
		|| (size.width() <= maxSize.width()
			&& size.height() <= maxSize.height())) {
		return size;
	}
	const auto scale = std::min(
		maxSize.width() / double(size.width()),
		maxSize.height() / double(size.height()));
	return QSize(
		std::clamp(int(std::lround(size.width() * scale)), 1, maxSize.width()),
		std::clamp(
			int(std::lround(size.height() * scale)),
			1,
			maxSize.height()));
}

QSize OutputSize(QSize source, const EditState &state) {
	return OutputSize(source, state, QSize());
}

QSize OutputSize(QSize source, const EditState &state, QSize maxSize) {
	if (source.isEmpty()) {
		return QSize();
	}
	return PlanGeometry(source, state, maxSize).output;
}

QTransform OutputTransform(
		QSize source,
		const EditState &state,
		QSize output) {
	if (source.isEmpty()) {
		return QTransform();
	}
	auto plan = PlanGeometry(source, state, QSize());
	if (!output.isEmpty()) {
		plan.output = output;
	}
	return plan.transform();
}

//
// Rendering.
//

QImage PrepareSource(const QImage &image, QSize maxSize) {
	if (image.isNull()) {
		return QImage();
	}
	auto result = ToPremultiplied(image);
	const auto size = FitSize(result.size(), maxSize);
	if (size != result.size()) {
		result = ToPremultiplied(result.scaled(
			size,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation));
	}
	result.setDevicePixelRatio(1.);
	return result;
}

QImage Render(
		const QImage &source,
		const EditState &original,
		QSize maxSize,
		const std::atomic<bool> *cancelled) {
	if (source.isNull()) {
		return QImage();
	}
	const auto state = Normalized(original);
	auto converted = ToPremultiplied(source);
	converted.setDevicePixelRatio(1.);
	const auto plan = PlanGeometry(converted.size(), state, maxSize);
	auto result = RenderGeometry(converted, plan);
	if (result.isNull() || Cancelled(cancelled)) {
		return QImage();
	}
	if (!HasColorEdits(state)) {
		if (result.colorSpace() != source.colorSpace()) {
			result.setColorSpace(source.colorSpace());
		}
		return result;
	}
	// Detach from the source before modifying the pixels in place.
	result.bits();
	if (result.isNull()) {
		return QImage();
	}
	const auto transparent = HasTransparency(result);
	if (!ApplyPipeline(result, state, cancelled, transparent)) {
		return QImage();
	}
	if (result.colorSpace() != source.colorSpace()) {
		result.setColorSpace(source.colorSpace());
	}
	return result;
}

QImage Thumbnail(const QImage &source, const EditState &original, int side) {
	if (source.isNull() || side <= 0) {
		return QImage();
	}
	const auto state = Normalized(original);
	auto result = ThumbnailBase(ToPremultiplied(source), state, side);
	if (result.isNull()) {
		return QImage();
	}
	result.bits();
	const auto transparent = HasTransparency(result);
	if (HasColorEdits(state)
		&& !ApplyPipeline(result, state, nullptr, transparent)) {
		return QImage();
	}
	return result;
}

std::vector<FilterThumbnail> FilterThumbnails(
		const QImage &source,
		const EditState &original,
		int side) {
	const auto &ids = FilterIds();
	auto result = std::vector<FilterThumbnail>(ids.size());
	if (source.isNull() || side <= 0) {
		for (auto i = 0; i != int(ids.size()); ++i) {
			result[i].id = ids[i];
		}
		return result;
	}
	const auto state = Normalized(original);
	const auto base = ThumbnailBase(ToPremultiplied(source), state, side);
	const auto transparent = HasTransparency(base);
	ParallelFor(int(ids.size()), 1, [&](int from, int till) {
		for (auto i = from; i != till; ++i) {
			auto copy = state;
			copy.filter = ids[i];
			copy.filterIntensity = 100;
			auto image = base.copy();
			if (HasColorEdits(copy)
				&& !ApplyPipeline(image, copy, nullptr, transparent)) {
				image = QImage();
			}
			result[i] = { ids[i], std::move(image) };
		}
	});
	return result;
}

EditState AutoEnhance(const QImage &source) {
	auto result = EditState();
	const auto small = PrepareSource(source, QSize(512, 512));
	if (small.isNull()) {
		return result;
	}
	constexpr auto kBins = 1024;
	auto histogram = std::array<double, kBins>();
	auto total = 0.;
	auto saturation = 0.;
	auto mid = std::array<double, 3>();
	auto midCount = 0.;
	for (auto y = 0; y != small.height(); ++y) {
		const auto line = reinterpret_cast<const Pixel*>(
			small.constScanLine(y));
		for (auto x = 0; x != small.width(); ++x) {
			const auto a = Alpha(line[x]);
			if (a < 128) {
				continue;
			}
			const auto c = Unpremultiply(line[x], a);
			const auto l = Clamp01(Luma(c));
			histogram[std::min(int(l * kBins), kBins - 1)] += 1.;
			total += 1.;
			const auto spread = std::max({ c.r, c.g, c.b })
				- std::min({ c.r, c.g, c.b });
			saturation += spread;
			// White balance is estimated from nearly neutral pixels only,
			// plain gray world fails on colorful scenes.
			if (l > 0.2f && l < 0.9f && spread < 0.12f) {
				mid[0] += c.r;
				mid[1] += c.g;
				mid[2] += c.b;
				midCount += 1.;
			}
		}
	}
	if (total < 16.) {
		return result;
	}
	saturation /= total;
	const auto percentile = [&](double part) {
		auto sum = 0.;
		for (auto i = 0; i != kBins; ++i) {
			sum += histogram[i];
			if (sum >= part * total) {
				return (i + 0.5f) / kBins;
			}
		}
		return 1.f;
	};
	const auto fraction = [&](float from, float till) {
		auto sum = 0.;
		for (auto i = 0; i != kBins; ++i) {
			const auto v = (i + 0.5f) / kBins;
			if (v >= from && v < till) {
				sum += histogram[i];
			}
		}
		return sum / total;
	};
	const auto low = percentile(0.005);
	const auto high = percentile(0.995);
	const auto median = percentile(0.5);

	// The neutral tone mapping of the current adjustments.
	const auto mapped = [&](const EditState &state, float v) {
		const auto tone = ToneFor(state);
		const auto luma = LumaParams{
			.shadows = state.shadows / 100.f,
			.highlights = state.highlights / 100.f,
		};
		return LumaValue(luma, ToneValue(tone, v, 1));
	};
	const auto search = [&](
			int EditState::*member,
			int from,
			int till,
			const auto &error) {
		auto best = result.*member;
		auto bestError = std::numeric_limits<double>::max();
		for (auto value = from; value <= till; ++value) {
			auto copy = result;
			copy.*member = value;
			const auto current = error(copy) + std::abs(value) * 1e-5;
			if (current < bestError) {
				bestError = current;
				best = value;
			}
		}
		result.*member = best;
	};

	// Levels: black / white points moved towards the histogram ends.
	if (high < 0.85f) {
		search(&EditState::exposure, 0, 60, [&](const EditState &s) {
			return std::abs(mapped(s, high) - 0.93f);
		});
	}
	if (mapped(result, high) < 0.94f) {
		search(&EditState::whites, 0, 40, [&](const EditState &s) {
			return std::abs(mapped(s, high) - 0.96f);
		});
	}
	if (mapped(result, low) > 0.06f) {
		search(&EditState::blacks, -35, 0, [&](const EditState &s) {
			return std::abs(mapped(s, low) - 0.03f);
		});
	}
	const auto dark = fraction(0.f, 0.15f);
	if (dark > 0.2) {
		result.shadows = std::clamp(int(dark * 70.), 8, 25);
	}
	const auto bright = fraction(0.95f, 1.01f);
	if (bright > 0.03) {
		result.highlights = -std::clamp(int(bright * 300.), 8, 30);
	}
	// The median goes halfway to where a linear stretch between the new
	// end points would put it, so bright scenes stay bright and dark
	// ones are lifted, the curve bend of the end point moves is undone.
	const auto newLow = mapped(result, low);
	const auto newHigh = mapped(result, high);
	const auto linear = newLow
		+ (median - low) / std::max(high - low, 0.05f) * (newHigh - newLow);
	const auto target = std::clamp(Mix(median, linear, 0.5f), 0.2f, 0.75f);
	search(&EditState::brightness, -30, 30, [&](const EditState &s) {
		return std::abs(mapped(s, median) - target);
	});
	const auto deviation = [&](const EditState &s) {
		auto mean = 0.;
		auto square = 0.;
		for (auto i = 0; i != kBins; i += 4) {
			const auto v = mapped(s, (i + 0.5f) / kBins);
			auto weight = 0.;
			for (auto k = i; k != std::min(i + 4, kBins); ++k) {
				weight += histogram[k];
			}
			mean += v * weight;
			square += v * v * weight;
		}
		mean /= total;
		return std::sqrt(std::max(square / total - mean * mean, 0.));
	};
	if (deviation(result) < 0.16) {
		search(&EditState::contrast, 0, 25, [&](const EditState &s) {
			return std::abs(deviation(s) - 0.17);
		});
	}
	result.vibrance = (saturation < 0.3)
		? std::clamp(int((0.3 - saturation) * 120.), 5, 30)
		: 5;
	if (midCount > std::max(16., total * 0.03)) {
		const auto r = float(mid[0] / midCount);
		const auto g = float(mid[1] / midCount);
		const auto b = float(mid[2] / midCount);
		const auto channel = [&](const EditState &s, float v, int c) {
			return ToneValue(ToneFor(s), v, c);
		};
		// A part of the cast is kept (warm light is often intended).
		if (std::abs(r - b) > 0.03f) {
			const auto keep = (r - b) * 0.3f;
			search(&EditState::temperature, -25, 25, [&](const EditState &s) {
				return std::abs(
					channel(s, r, 0) - channel(s, b, 2) - keep);
			});
		}
		const auto green = g - (r + b) / 2.f;
		if (std::abs(green) > 0.02f) {
			const auto keep = green * 0.3f;
			search(&EditState::tint, -15, 15, [&](const EditState &s) {
				return std::abs(channel(s, g, 1)
					- (channel(s, r, 0) + channel(s, b, 2)) / 2.f
					- keep);
			});
		}
	}
	return Normalized(result);
}

//
// Loading and saving.
//

namespace {

[[nodiscard]] QImage ReadImage(QImageReader &reader, QString *error) {
	reader.setAutoTransform(true);
	auto image = reader.read();
	if (image.isNull()) {
		if (error) {
			*error = reader.errorString();
		}
		return QImage();
	}
	// Color profiles (Display P3, Adobe RGB, gray or CMYK ICC) are
	// converted to sRGB together with the pixel format.
	const auto space = image.colorSpace();
	const auto srgb = QColorSpace(QColorSpace::SRgb);
	if (space.isValid() && space != srgb) {
		auto converted = image.convertedToColorSpace(
			srgb,
			(image.hasAlphaChannel()
				? QImage::Format_ARGB32
				: QImage::Format_RGB32));
		if (!converted.isNull()) {
			image = std::move(converted);
		}
	}
	image = ToPremultiplied(image);
	image.setDevicePixelRatio(1.);
	if (image.isNull() && error) {
		*error = u"Out of memory."_q;
	}
	return image;
}

[[nodiscard]] const char *WriterFormat(SaveFormat format) {
	switch (format) {
	case SaveFormat::Png: return "png";
	case SaveFormat::Jpeg: return "jpeg";
	case SaveFormat::Webp: return "webp";
	}
	return "png";
}

} // namespace

QImage LoadImage(const QString &path, QString *error) {
	auto reader = QImageReader(path);
	return ReadImage(reader, error);
}

QImage LoadImage(const QByteArray &bytes, QString *error) {
	auto buffer = QBuffer();
	buffer.setData(bytes);
	if (!buffer.open(QIODevice::ReadOnly)) {
		if (error) {
			*error = u"Could not open the buffer."_q;
		}
		return QImage();
	}
	auto reader = QImageReader(&buffer);
	return ReadImage(reader, error);
}

bool SaveFormatSupported(SaveFormat format) {
	return QImageWriter::supportedImageFormats().contains(
		QByteArray(WriterFormat(format)));
}

std::vector<SaveFormat> SupportedSaveFormats() {
	auto result = std::vector<SaveFormat>();
	for (const auto format : {
		SaveFormat::Png,
		SaveFormat::Jpeg,
		SaveFormat::Webp,
	}) {
		if (SaveFormatSupported(format)) {
			result.push_back(format);
		}
	}
	return result;
}

QString SaveFormatExtension(SaveFormat format) {
	switch (format) {
	case SaveFormat::Png: return u"png"_q;
	case SaveFormat::Jpeg: return u"jpg"_q;
	case SaveFormat::Webp: return u"webp"_q;
	}
	return u"png"_q;
}

QString SaveFormatMimeType(SaveFormat format) {
	switch (format) {
	case SaveFormat::Png: return u"image/png"_q;
	case SaveFormat::Jpeg: return u"image/jpeg"_q;
	case SaveFormat::Webp: return u"image/webp"_q;
	}
	return u"image/png"_q;
}

QByteArray EncodeImage(
		const QImage &image,
		SaveFormat format,
		int quality) {
	if (image.isNull() || !SaveFormatSupported(format)) {
		return QByteArray();
	}
	const auto transparent = HasTransparency(image);
	auto prepared = QImage();
	if (format == SaveFormat::Jpeg && transparent) {
		prepared = QImage(image.size(), QImage::Format_RGB32);
		prepared.fill(Qt::white);
		prepared.setColorSpace(image.colorSpace());
		auto p = QPainter(&prepared);
		p.drawImage(0, 0, image);
	} else {
		prepared = image.convertToFormat((transparent
			&& format != SaveFormat::Jpeg)
			? QImage::Format_ARGB32
			: QImage::Format_RGB32);
	}
	if (prepared.isNull()) {
		return QByteArray();
	}
	prepared.setDevicePixelRatio(1.);
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	if (!buffer.open(QIODevice::WriteOnly)) {
		return QByteArray();
	}
	auto writer = QImageWriter(&buffer, WriterFormat(format));
	if (format != SaveFormat::Png) {
		writer.setQuality(std::clamp(quality, 0, 100));
	}
	if (format == SaveFormat::Jpeg) {
		writer.setOptimizedWrite(true);
		writer.setProgressiveScanWrite(true);
	}
	if (!writer.write(prepared)) {
		return QByteArray();
	}
	buffer.close();
	return result;
}

bool SaveImage(
		const QImage &image,
		const QString &path,
		SaveFormat format,
		int quality) {
	const auto bytes = EncodeImage(image, format, quality);
	if (bytes.isEmpty()) {
		return false;
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	if (file.write(bytes) != bytes.size()) {
		file.cancelWriting();
		return false;
	}
	return file.commit();
}

//
// Self-test.
//

bool RunSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto info = [&](const QString &what) {
		log.push_back(u"   "_q + what);
	};
	const auto ms = [](const QElapsedTimer &timer) {
		return QString::number(timer.nsecsElapsed() / 1e6, 'f', 1);
	};
	log.push_back(u"Threads: %1"_q.arg(ThreadCount()));

	const auto source = SyntheticImage(1920, 1080, false);
	const auto transparent = SyntheticImage(640, 360, true);
	check(!source.isNull() && !transparent.isNull(), u"synthetic images"_q);
	if (source.isNull() || transparent.isNull()) {
		return false;
	}

	// Identity.
	{
		check(IsIdentity(EditState()), u"default state is identity"_q);
		check(
			SamePixels(Render(source, EditState()), source),
			u"identity state -> pixel-identical output"_q);
		auto zero = EditState();
		zero.filter = u"vivid"_q;
		zero.filterIntensity = 0;
		zero.vignetteFeather = 80;
		zero.grainSize = 70;
		check(
			IsIdentity(zero) && SamePixels(Render(source, zero), source),
			u"zero intensity / unused sizes -> identity"_q);
		auto alpha = EditState();
		check(
			SamePixels(Render(transparent, alpha), transparent),
			u"identity keeps transparent pixels"_q);
	}

	// Determinism.
	{
		auto state = EditState();
		state.exposure = 15;
		state.clarity = 40;
		state.grain = 30;
		state.filter = u"film"_q;
		state.effects.push_back(DefaultEffect(EffectType::Glitch));
		state.effects.push_back(DefaultEffect(EffectType::Halftone));
		const auto first = Render(source, state);
		const auto second = Render(source, state);
		check(
			!first.isNull() && SamePixels(first, second),
			u"rendering is deterministic"_q);
	}

	// Brightness / saturation.
	{
		auto state = EditState();
		state.brightness = 50;
		const auto before = MeanLuma(source);
		const auto after = MeanLuma(Render(source, state));
		check(
			after > before + 0.02,
			u"brightness +50 raises mean luma (%1 -> %2)"_q.arg(
				QString::number(before, 'f', 3),
				QString::number(after, 'f', 3)));

		state = EditState();
		state.exposure = -60;
		check(
			MeanLuma(Render(source, state)) < before - 0.05,
			u"exposure -60 lowers mean luma"_q);

		state = EditState();
		state.saturation = -100;
		state.contrast = 30;
		state.shadows = 40;
		const auto gray = Render(source, state);
		auto equal = !gray.isNull();
		for (auto y = 0; equal && y != gray.height(); ++y) {
			const auto line = reinterpret_cast<const Pixel*>(
				gray.constScanLine(y));
			for (auto x = 0; x != gray.width(); ++x) {
				const auto p = line[x];
				if (Red(p) != Green(p) || Green(p) != Blue(p)) {
					equal = false;
					break;
				}
			}
		}
		check(equal, u"saturation -100 -> R == G == B everywhere"_q);

		state = EditState();
		state.exposure = 30;
		state.temperature = 40;
		state.saturation = 30;
		state.clarity = 50;
		state.blur = 20;
		state.vignette = 40;
		state.grain = 40;
		state.effects.push_back(DefaultEffect(EffectType::Glow));
		state.effects.push_back(DefaultEffect(EffectType::Glitch));
		const auto edited = Render(transparent, state);
		check(
			!edited.isNull() && ValidPremultiplied(edited),
			u"transparent image stays valid premultiplied"_q);
		auto alphaKept = true;
		auto colorOnly = EditState();
		colorOnly.exposure = 30;
		colorOnly.saturation = -40;
		colorOnly.filter = u"cinema"_q;
		const auto colored = Render(transparent, colorOnly);
		for (auto y = 0; alphaKept && y != colored.height(); ++y) {
			const auto a = reinterpret_cast<const Pixel*>(
				colored.constScanLine(y));
			const auto b = reinterpret_cast<const Pixel*>(
				transparent.constScanLine(y));
			for (auto x = 0; x != colored.width(); ++x) {
				if (Alpha(a[x]) != Alpha(b[x])) {
					alphaKept = false;
					break;
				}
			}
		}
		check(alphaKept, u"color edits keep alpha exactly"_q);
	}

	// Geometry.
	{
		auto marked = source.copy();
		const auto fill = [&](int x, int y, QRgb color) {
			for (auto j = 0; j != 8; ++j) {
				for (auto i = 0; i != 8; ++i) {
					marked.setPixel(x + i, y + j, color);
				}
			}
		};
		const auto red = qRgb(255, 0, 0);
		const auto green = qRgb(0, 255, 0);
		const auto blue = qRgb(0, 0, 255);
		const auto white = qRgb(255, 255, 255);
		fill(0, 0, red);
		fill(1912, 0, green);
		fill(0, 1072, blue);
		fill(1912, 1072, white);
		const auto corners = [](const QImage &image) {
			const auto w = image.width() - 1;
			const auto h = image.height() - 1;
			return std::array<QRgb, 4>{
				image.pixel(0, 0),
				image.pixel(w, 0),
				image.pixel(0, h),
				image.pixel(w, h),
			};
		};
		const auto expect = [&](
				const QString &name,
				const EditState &state,
				QSize size,
				std::array<QRgb, 4> colors) {
			const auto image = Render(marked, state);
			check(
				image.size() == size
					&& OutputSize(marked.size(), state) == size
					&& corners(image) == colors,
				name + u" -> %1x%2 and corner pixels"_q.arg(
					size.width()).arg(size.height()));
		};
		auto state = EditState();
		state.quarterTurns = 1;
		expect(
			u"rotate 90"_q,
			state,
			QSize(1080, 1920),
			{ blue, red, white, green });
		check(
			SamePixels(
				Render(marked, state),
				marked.transformed(QTransform().rotate(90))),
			u"rotate 90 equals QImage::transformed exactly"_q);
		state.quarterTurns = 2;
		expect(
			u"rotate 180"_q,
			state,
			QSize(1920, 1080),
			{ white, blue, green, red });
		state.quarterTurns = 3;
		expect(
			u"rotate 270"_q,
			state,
			QSize(1080, 1920),
			{ green, white, red, blue });
		state = EditState();
		state.flipHorizontal = true;
		expect(
			u"flip horizontal"_q,
			state,
			QSize(1920, 1080),
			{ green, red, white, blue });
		state = EditState();
		state.flipVertical = true;
		expect(
			u"flip vertical"_q,
			state,
			QSize(1920, 1080),
			{ blue, white, red, green });
		state = EditState();
		state.quarterTurns = 1;
		state.flipHorizontal = true;
		expect(
			u"rotate 90 + flip horizontal"_q,
			state,
			QSize(1080, 1920),
			{ red, blue, green, white });

		state = EditState();
		state.crop = QRectF(0.5, 0.5, 0.5, 0.5);
		const auto cropped = Render(marked, state);
		check(
			cropped.size() == QSize(960, 540)
				&& cropped.pixel(0, 0) == marked.pixel(960, 540)
				&& cropped.pixel(959, 539) == white,
			u"crop to the bottom right quarter -> 960x540, exact pixels"_q);
		state.quarterTurns = 1;
		const auto turnedCrop = Render(marked, state);
		check(
			turnedCrop.size() == QSize(540, 960)
				&& turnedCrop.pixel(539, 959) == green,
			u"rotate 90 + crop -> 540x960, corner pixel"_q);

		state = EditState();
		state.straighten = 7.5;
		const auto straight = Render(marked, state);
		auto opaque = !straight.isNull() && straight.size() == marked.size();
		if (opaque) {
			for (const auto color : corners(straight)) {
				opaque = opaque && (qAlpha(color) == 255);
			}
			opaque = opaque && AllOpaque(straight);
		}
		check(opaque, u"straighten 7.5 -> same size, no empty corners"_q);

		state = EditState();
		state.quarterTurns = 1;
		state.straighten = -12.;
		state.crop = QRectF(0.1, 0.2, 0.6, 0.5);
		const auto limited = Render(marked, state, QSize(400, 400));
		check(
			limited.size() == OutputSize(marked.size(), state, QSize(400, 400))
				&& limited.width() <= 400
				&& limited.height() <= 400
				&& AllOpaque(limited),
			u"rotate + straighten + crop + maxSize -> %1x%2"_q.arg(
				limited.width()).arg(limited.height()));

		state = EditState();
		const auto preview = Render(marked, state, QSize(640, 640));
		check(
			preview.size() == QSize(640, 360),
			u"maxSize downscale 1920x1080 -> 640x360"_q);

		const auto transform = OutputTransform(
			marked.size(),
			[] { auto s = EditState(); s.quarterTurns = 1; return s; }(),
			QSize(1080, 1920));
		const auto mapped = transform.map(QPointF(0., 0.));
		check(
			std::abs(mapped.x() - 1080.) < 1e-6 && std::abs(mapped.y()) < 1e-6,
			u"OutputTransform maps the source origin (rotate 90)"_q);
	}

	// Filters and effects.
	{
		auto total = 0.;
		for (const auto &id : FilterIds()) {
			auto state = EditState();
			state.filter = id;
			auto timer = QElapsedTimer();
			timer.start();
			const auto image = Render(source, state);
			total += timer.nsecsElapsed() / 1e6;
			check(
				image.size() == source.size()
					&& AllOpaque(image)
					&& HasDetail(image),
				u"filter %1: %2 ms"_q.arg(id, ms(timer)));
		}
		info(u"all filters: %1 ms"_q.arg(QString::number(total, 'f', 1)));
		for (const auto type : EffectTypes()) {
			auto state = EditState();
			auto effect = DefaultEffect(type);
			if (type == EffectType::Halftone) {
				auto colorful = effect;
				colorful.mode = 1;
				state.effects.push_back(colorful);
			}
			state.effects.push_back(effect);
			auto timer = QElapsedTimer();
			timer.start();
			const auto image = Render(source, state);
			check(
				image.size() == source.size()
					&& AllOpaque(image)
					&& HasDetail(image)
					&& !SamePixels(image, source),
				u"effect %1: %2 ms"_q.arg(EffectKey(type), ms(timer)));
		}
		for (const auto &row : AdjustRows()) {
			for (const auto value : { row.info.min, row.info.max }) {
				auto state = EditState();
				state.setValue(row.info.id, value);
				auto timer = QElapsedTimer();
				timer.start();
				const auto image = Render(source, state);
				check(
					image.size() == source.size() && AllOpaque(image),
					u"adjust %1 = %2: %3 ms"_q.arg(
						QString::fromLatin1(row.info.key),
						QString::number(value),
						ms(timer)));
			}
		}
		auto tiny = EditState();
		tiny.crop = QRectF(0.5, 0.5, 0.0005, 0.0005);
		for (const auto type : EffectTypes()) {
			tiny.effects.push_back(DefaultEffect(type));
		}
		tiny.clarity = 50;
		tiny.sharpen = 50;
		tiny.blur = 50;
		tiny.grain = 50;
		tiny.vignette = 50;
		const auto small = Render(source, tiny);
		check(
			!small.isNull() && small.width() >= 1 && small.height() >= 1,
			u"all effects on a %1x%2 crop"_q.arg(
				small.width()).arg(small.height()));
	}

	// Performance.
	{
		auto state = EditState();
		state.exposure = 20;
		state.contrast = 25;
		state.highlights = -30;
		state.shadows = 30;
		state.saturation = 15;
		state.vibrance = 20;
		state.temperature = 10;
		state.clarity = 30;
		state.sharpen = 40;
		state.vignette = 30;
		state.grain = 20;
		state.filter = u"film"_q;
		state.filterIntensity = 80;
		auto best = std::numeric_limits<qint64>::max();
		for (auto i = 0; i != 3; ++i) {
			auto timer = QElapsedTimer();
			timer.start();
			const auto image = Render(source, state);
			best = std::min(best, timer.nsecsElapsed());
		}
		info(u"1920x1080 typical edit (tone, color, clarity, sharpen, "
			"vignette, grain, filter): %1 ms (best of 3)"_q.arg(
				QString::number(best / 1e6, 'f', 1)));
		auto light = EditState();
		light.exposure = 20;
		light.contrast = 20;
		light.saturation = 20;
		best = std::numeric_limits<qint64>::max();
		for (auto i = 0; i != 3; ++i) {
			auto timer = QElapsedTimer();
			timer.start();
			const auto image = Render(source, light);
			best = std::min(best, timer.nsecsElapsed());
		}
		info(u"1920x1080 exposure + contrast + saturation: %1 ms"_q.arg(
			QString::number(best / 1e6, 'f', 1)));

		auto timer = QElapsedTimer();
		timer.start();
		const auto big = SyntheticImage(4000, 3000, false);
		info(u"12 MP synthetic image: %1 ms"_q.arg(ms(timer)));
		timer.restart();
		const auto prepared = PrepareSource(big, QSize(1920, 1920));
		info(u"PrepareSource 4000x3000 -> %1x%2: %3 ms"_q.arg(
			QString::number(prepared.width()),
			QString::number(prepared.height()),
			ms(timer)));
		timer.restart();
		const auto exported = Render(big, state);
		check(
			exported.size() == QSize(4000, 3000),
			u"12 MP export, typical edit: %1 ms"_q.arg(ms(timer)));
		auto heavy = state;
		heavy.straighten = 3.;
		heavy.effects.push_back(DefaultEffect(EffectType::Glow));
		heavy.effects.push_back(DefaultEffect(EffectType::LensBlur));
		timer.restart();
		const auto heavyExport = Render(big, heavy);
		check(
			!heavyExport.isNull(),
			u"12 MP export, + straighten, glow, lens blur: %1 ms"_q.arg(
				ms(timer)));
		timer.restart();
		const auto fromBig = Render(big, state, QSize(1280, 1280));
		check(
			fromBig.size() == QSize(1280, 960),
			u"12 MP -> 1280 preview without PrepareSource: %1 ms"_q.arg(
				ms(timer)));
		timer.restart();
		const auto thumbs = FilterThumbnails(prepared, state, 96);
		auto thumbsOk = (thumbs.size() == FilterIds().size());
		for (const auto &thumb : thumbs) {
			thumbsOk = thumbsOk && (thumb.image.size() == QSize(96, 96));
		}
		check(
			thumbsOk,
			u"%1 filter thumbnails 96x96: %2 ms"_q.arg(
				QString::number(thumbs.size()),
				ms(timer)));
		check(
			Thumbnail(prepared, state, 120).size() == QSize(120, 120),
			u"Thumbnail 120x120"_q);

		auto cancel = std::atomic<bool>(true);
		check(
			Render(source, state, QSize(), &cancel).isNull(),
			u"cancelled render returns a null image"_q);
	}

	// JSON.
	{
		auto state = EditState();
		state.crop = QRectF(0.125, 0.2, 0.5, 0.6180339887);
		state.quarterTurns = 3;
		state.straighten = -7.3;
		state.flipHorizontal = true;
		auto value = -90;
		for (const auto &row : AdjustRows()) {
			state.setValue(row.info.id, value);
			value += 11;
		}
		state.filter = u"cinema"_q;
		state.filterIntensity = 65;
		for (const auto type : EffectTypes()) {
			auto effect = DefaultEffect(type);
			for (const auto &param : EffectParams(type)) {
				if (!param.color) {
					effect.setValue(param.param, param.max - 1);
				}
			}
			effect.color1 = qRgb(10, 20, 30);
			effect.color2 = qRgb(200, 150, 100);
			effect.enabled = (type != EffectType::Sepia);
			state.effects.push_back(effect);
		}
		const auto bytes = Serialize(state);
		const auto restored = Deserialize(bytes);
		check(
			restored.has_value()
				&& *restored == Normalized(state)
				&& Serialize(*restored) == bytes,
			u"JSON round trip (%1 bytes)"_q.arg(bytes.size()));
		check(
			Deserialize("{\"v\":1}").value_or(EditState()) == EditState(),
			u"JSON defaults"_q);
		const auto broken = Deserialize(
			"{\"crop\":[2,-1,5,0],\"rotate\":7,\"straighten\":99,"
			"\"adjust\":{\"exposure\":500,\"fade\":-3},"
			"\"filterIntensity\":1000,"
			"\"effects\":[{\"type\":\"nope\"},"
			"{\"type\":\"posterize\",\"levels\":1}]}");
		check(
			broken.has_value()
				&& broken->quarterTurns == 3
				&& broken->straighten == 45.
				&& broken->exposure == 100
				&& broken->fade == 0
				&& broken->filterIntensity == 100
				&& broken->effects.size() == 1
				&& broken->effects[0].levels == 2
				&& IsFullCrop(broken->crop),
			u"JSON values are clamped, unknown effects skipped"_q);
		check(!Deserialize("not json").has_value(), u"JSON garbage"_q);
	}

	// Auto enhance.
	{
		auto dull = SyntheticImage(800, 600, false);
		ForEachColor(Wrap(dull), [](Color &c, int, int) {
			c.r = 0.12f + c.r * 0.3f;
			c.g = 0.12f + c.g * 0.3f;
			c.b = 0.16f + c.b * 0.34f;
		});
		auto timer = QElapsedTimer();
		timer.start();
		const auto state = AutoEnhance(dull);
		const auto elapsed = ms(timer);
		const auto fixed = Render(dull, state);
		const auto meanBefore = MeanLuma(dull);
		const auto meanAfter = MeanLuma(fixed);
		const auto spreadBefore = LumaDeviation(dull);
		const auto spreadAfter = LumaDeviation(fixed);
		check(
			!HasGeometry(state)
				&& meanAfter > meanBefore
				&& spreadAfter > spreadBefore * 1.2,
			u"auto enhance of a dark flat image: mean %1 -> %2, "
			"deviation %3 -> %4, %5 ms"_q.arg(
				QString::number(meanBefore, 'f', 3),
				QString::number(meanAfter, 'f', 3),
				QString::number(spreadBefore, 'f', 3),
				QString::number(spreadAfter, 'f', 3),
				elapsed));
		info(u"auto enhance: "_q + QString::fromUtf8(Serialize(state)));
	}

	// Encoding.
	{
		auto names = QStringList();
		for (const auto format : SupportedSaveFormats()) {
			names.push_back(SaveFormatExtension(format));
		}
		info(u"save formats: "_q + names.join(u", "_q));
		const auto image = Render(source, [] {
			auto s = EditState();
			s.filter = u"vivid"_q;
			return s;
		}(), QSize(640, 640));
		auto timer = QElapsedTimer();
		timer.start();
		const auto png = EncodeImage(image, SaveFormat::Png);
		const auto decoded = LoadImage(png);
		check(
			!png.isEmpty() && SamePixels(decoded, image),
			u"PNG round trip is lossless (%1 KB, %2 ms)"_q.arg(
				QString::number(png.size() / 1024),
				ms(timer)));
		const auto alphaPng = EncodeImage(transparent, SaveFormat::Png);
		const auto alphaDecoded = LoadImage(alphaPng);
		auto alphaClose = !alphaDecoded.isNull()
			&& alphaDecoded.size() == transparent.size();
		for (auto y = 0; alphaClose && y < transparent.height(); y += 7) {
			for (auto x = 0; x < transparent.width(); x += 7) {
				const auto a = transparent.pixel(x, y);
				const auto b = alphaDecoded.pixel(x, y);
				if (qAlpha(a) != qAlpha(b)
					|| std::abs(qRed(a) - qRed(b)) > 2
					|| std::abs(qBlue(a) - qBlue(b)) > 2) {
					alphaClose = false;
					break;
				}
			}
		}
		check(alphaClose, u"PNG keeps transparency"_q);
		if (SaveFormatSupported(SaveFormat::Jpeg)) {
			timer.restart();
			const auto jpeg = EncodeImage(image, SaveFormat::Jpeg, 90);
			const auto back = LoadImage(jpeg);
			check(
				back.size() == image.size()
					&& std::abs(MeanLuma(back) - MeanLuma(image)) < 0.02,
				u"JPEG q90 encode / decode (%1 KB, %2 ms)"_q.arg(
					QString::number(jpeg.size() / 1024),
					ms(timer)));
			const auto flat = LoadImage(
				EncodeImage(transparent, SaveFormat::Jpeg, 90));
			check(
				!flat.isNull() && AllOpaque(flat) && qRed(flat.pixel(0, 0)) > 150,
				u"JPEG flattens transparency onto white"_q);
		} else {
			check(false, u"JPEG writer is missing"_q);
		}
		if (SaveFormatSupported(SaveFormat::Webp)) {
			timer.restart();
			const auto webp = EncodeImage(transparent, SaveFormat::Webp, 100);
			const auto back = LoadImage(webp);
			check(
				back.size() == transparent.size() && !AllOpaque(back),
				u"WebP lossless with alpha (%1 KB, %2 ms)"_q.arg(
					QString::number(webp.size() / 1024),
					ms(timer)));
		}
		auto wide = QImage(QSize(8, 8), QImage::Format_ARGB32_Premultiplied);
		wide.fill(QColor(200, 100, 50));
		wide.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
		const auto converted = LoadImage(EncodeImage(wide, SaveFormat::Png));
		check(
			!converted.isNull()
				&& converted.colorSpace() == QColorSpace(QColorSpace::SRgb)
				&& qRed(converted.pixel(4, 4)) > 205,
			u"Display P3 file is converted to sRGB on load (%1)"_q.arg(
				converted.isNull()
					? u"null"_q
					: QColor(converted.pixel(4, 4)).name()));
		QString error;
		check(
			LoadImage(QByteArray("garbage"), &error).isNull()
				&& !error.isEmpty(),
			u"loading garbage fails with an error"_q);
	}
	return ok;
}

} // namespace Oblivion::Photo
