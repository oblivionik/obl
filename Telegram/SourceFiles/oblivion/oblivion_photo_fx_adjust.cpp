/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_fx_adjust.h"

#include <QtCore/QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Oblivion::Photo {
namespace {

constexpr auto kPi = 3.14159265358979323846;
constexpr auto kDecodeSize = 1024;
constexpr auto kEncodeSize = 4096;
constexpr auto kToneSize = 1024;
constexpr auto kHueSize = 720;
// i / 1020: every 8 bit value is exactly a node of the table.
constexpr auto kCurveTable = 1021;

// Rec. 709 luminance of linear light.
constexpr auto kLumaR = 0.2126f;
constexpr auto kLumaG = 0.7152f;
constexpr auto kLumaB = 0.0722f;

// Below this linear luminance the chroma is not amplified as much as the
// luminance when the shadows are lifted (the color noise would explode).
constexpr auto kChromaFloor = 0.004f;

constexpr auto kLowTarget = 2.5; // Blur sigma of a reduced plane, pixels.
constexpr auto kLowMaxFactor = 64;

// Noise reduction filters a picture larger than kDenoiseHalfPixels on
// planes of a half of its size when its blur is at least
// kDenoiseHalfSigma pixels wide, and one larger than kDenoiseFullPixels
// always.
constexpr auto kDenoiseHalfPixels = qint64(3'000'000);
constexpr auto kDenoiseHalfSigma = 1.2;
constexpr auto kDenoiseFullPixels = qint64(12'000'000);

// The centers of the eight color ranges as Oklab hue angles, degrees:
// red, orange, yellow, green, aqua, blue, purple, magenta.
constexpr auto kHueCenters = std::array<float, kHslRanges>{ {
	27.f,
	55.f,
	105.f,
	140.f,
	198.f,
	258.f,
	300.f,
	335.f,
} };

constexpr auto kCurveKeys = std::array<char, kCurveChannels>{ {
	'm',
	'r',
	'g',
	'b',
} };

//
// Gamma.
//

struct GammaTables {
	std::array<float, kDecodeSize + 2> decode;
	std::array<float, kEncodeSize + 2> encode;
};

[[nodiscard]] const GammaTables &Gamma() {
	// Never destroyed: a render may still run on a worker at exit.
	static const auto result = [] {
		const auto tables = new GammaTables();
		for (auto i = 0; i != kDecodeSize + 2; ++i) {
			tables->decode[i] = float(FxSrgbToLinear(
				std::min(i / double(kDecodeSize), 1.)));
		}
		for (auto i = 0; i != kEncodeSize + 2; ++i) {
			tables->encode[i] = float(FxLinearToSrgb(
				std::min(i / double(kEncodeSize), 1.)));
		}
		return tables;
	}();
	return *result;
}

// sRGB encoded 0..1 -> linear light 0..1.
[[nodiscard]] inline float Decode(const GammaTables &gamma, float value) {
	const auto f = FxClamp01(value) * float(kDecodeSize);
	const auto i = int(f);
	const auto data = gamma.decode.data() + i;
	return data[0] + (data[1] - data[0]) * (f - float(i));
}

[[nodiscard]] inline float Encode(const GammaTables &gamma, float value) {
	const auto f = FxClamp01(value) * float(kEncodeSize);
	const auto i = int(f);
	const auto data = gamma.encode.data() + i;
	return data[0] + (data[1] - data[0]) * (f - float(i));
}

[[nodiscard]] inline float Luma(float r, float g, float b) {
	return kLumaR * r + kLumaG * g + kLumaB * b;
}

// A function of 0..1 as a table with linear interpolation.
class Lut final {
public:
	template <typename Function>
	Lut(int size, Function &&function)
	: _size(size)
	, _values(size + 2) {
		for (auto i = 0; i <= size; ++i) {
			_values[i] = float(function(i / double(size)));
		}
		_values[size + 1] = _values[size];
	}

	[[nodiscard]] float operator()(float value) const {
		const auto f = FxClamp01(value) * float(_size);
		const auto i = int(f);
		const auto data = _values.data() + i;
		return data[0] + (data[1] - data[0]) * (f - float(i));
	}

private:
	int _size = 0;
	std::vector<float> _values;

};

// Moves the color towards the gray (0..1) just enough to fit all the
// channels into 0..1: the luminance and the hue stay.
inline void FitGamut(float &r, float &g, float &b, float gray) {
	gray = FxClamp01(gray);
	auto t = 0.f;
	const auto check = [&](float c) {
		if (c > 1.f) {
			t = std::max(t, (c - 1.f) / (c - gray));
		} else if (c < 0.f) {
			t = std::max(t, c / (c - gray));
		}
	};
	check(r);
	check(g);
	check(b);
	if (t > 0.f) {
		t = std::min(t, 1.f);
		r += (gray - r) * t;
		g += (gray - g) * t;
		b += (gray - b) * t;
	}
}

// Changes the luminance of a linear color from luma to target keeping
// its chromaticity. For nearly black pixels the color follows the
// difference of the luminances instead of their ratio: it grows slower
// when they are lifted and stays exactly as it is when nothing changes.
inline void Relight(float &r, float &g, float &b, float luma, float target) {
	const auto ratio = (luma >= kChromaFloor)
		? (target / luma)
		: (1.f + (target - luma) / kChromaFloor);
	r = target + (r - luma) * ratio;
	g = target + (g - luma) * ratio;
	b = target + (b - luma) * ratio;
	FitGamut(r, g, b, target);
}

//
// Oklab.
//

struct Lab {
	float l = 0.f;
	float a = 0.f;
	float b = 0.f;
};

[[nodiscard]] inline Lab ToLab(float r, float g, float b) {
	const auto l = std::cbrt(
		0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
	const auto m = std::cbrt(
		0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
	const auto s = std::cbrt(
		0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
	return {
		0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
		1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
		0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
	};
}

inline void FromLab(const Lab &lab, float &r, float &g, float &b) {
	const auto l1 = lab.l + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
	const auto m1 = lab.l - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
	const auto s1 = lab.l - 0.0894841775f * lab.a - 1.2914855480f * lab.b;
	const auto l = l1 * l1 * l1;
	const auto m = m1 * m1 * m1;
	const auto s = s1 * s1 * s1;
	r = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
	g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
	b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

// The position of a hue in a table of kHueSize + 2 values.
[[nodiscard]] inline float HuePosition(float a, float b) {
	auto result = std::atan2(b, a) * float(kHueSize / (2. * kPi));
	if (result < 0.f) {
		result += float(kHueSize);
	}
	return result;
}

struct HueWeights {
	int first = 0;
	int second = 0;
	float t = 0.f; // The weight of second, 1 - t is the weight of first.
};

// Two neighbour ranges share every hue, the weights are smooth and
// their sum is always one.
[[nodiscard]] HueWeights RangeWeights(float degrees) {
	degrees -= 360.f * std::floor(degrees / 360.f);
	auto first = kHslRanges - 1;
	for (auto i = 0; i != kHslRanges; ++i) {
		if (degrees >= kHueCenters[i]) {
			first = i;
		}
	}
	if (degrees < kHueCenters[0]) {
		first = kHslRanges - 1;
		degrees += 360.f;
	}
	const auto second = (first + 1) % kHslRanges;
	const auto from = kHueCenters[first];
	const auto till = kHueCenters[second] + (second ? 0.f : 360.f);
	const auto t = FxClamp01((degrees - from) / (till - from));
	return { first, second, t * t * (3.f - 2.f * t) };
}

using HueTable = std::array<float, kHueSize + 2>;

// table[hue] = the value of the ranges mixed with their weights.
template <typename Value>
void FillHueTable(HueTable &table, Value &&value) {
	for (auto i = 0; i != kHueSize; ++i) {
		const auto weights = RangeWeights(i * (360.f / kHueSize));
		table[i] = float(value(weights.first)) * (1.f - weights.t)
			+ float(value(weights.second)) * weights.t;
	}
	table[kHueSize] = table[0];
	table[kHueSize + 1] = table[1];
}

[[nodiscard]] inline float HueValue(const HueTable &table, float position) {
	const auto i = int(position);
	const auto data = table.data() + i;
	return data[0] + (data[1] - data[0]) * (position - float(i));
}

// The distance in degrees from a range center to the next / previous one.
[[nodiscard]] float RangeStep(int range, bool forward) {
	const auto next = (range + 1) % kHslRanges;
	const auto previous = (range + kHslRanges - 1) % kHslRanges;
	auto result = forward
		? (kHueCenters[next] - kHueCenters[range])
		: (kHueCenters[range] - kHueCenters[previous]);
	if (result < 0.f) {
		result += 360.f;
	}
	return result;
}

//
// Reduced planes and the guided filter.
//
// Everything that needs a smooth "base" of the picture (local tone
// mapping, clarity, texture, dehaze, noise reduction) uses the fast
// guided filter: the coefficients a, b of the local linear model
// output = a * guide + b are found on a reduced plane, blurred and
// interpolated for every pixel. It is edge-aware (no halos), needs only
// small planes and one more pass over the pixels.
//

struct LowSize {
	int factor = 1;
	int width = 0;
	int height = 0;

	[[nodiscard]] size_t count() const {
		return size_t(width) * height;
	}
};

[[nodiscard]] LowSize LowForFactor(QSize size, int factor) {
	factor = std::clamp(factor, 1, kLowMaxFactor);
	return {
		factor,
		(size.width() + factor - 1) / factor,
		(size.height() + factor - 1) / factor,
	};
}

// The factor that leaves a blur of about kLowTarget reduced pixels.
[[nodiscard]] LowSize LowForSigma(QSize size, double sigma) {
	return LowForFactor(size, int(sigma / kLowTarget));
}

// Count planes of the reduced size: the average (or the minimum) of
// sample(color, values) over factor x factor blocks. Transparent pixels
// don't count, a block without a visible pixel gets the average of all
// the other blocks.
template <int Count, bool Minimum, typename Sample>
[[nodiscard]] std::array<std::vector<float>, Count> BuildLowPlanes(
		const QImage &image,
		LowSize low,
		const Sample &sample) {
	auto result = std::array<std::vector<float>, Count>();
	for (auto &plane : result) {
		plane.resize(low.count());
	}
	auto filled = std::vector<uchar>(low.count(), 0);
	const auto width = image.width();
	const auto height = image.height();
	const auto grain = std::max(
		1,
		32768 / std::max(width * low.factor, 1));
	FxParallel(low.height, grain, [&](int from, int till) {
		auto sums = std::vector<float>(size_t(low.width) * Count);
		auto counts = std::vector<int>(low.width);
		for (auto ly = from; ly != till; ++ly) {
			std::fill(
				begin(sums),
				end(sums),
				Minimum ? std::numeric_limits<float>::max() : 0.f);
			std::fill(begin(counts), end(counts), 0);
			const auto top = ly * low.factor;
			const auto bottom = std::min(top + low.factor, height);
			for (auto y = top; y != bottom; ++y) {
				const auto line = FxRow(image, y);
				for (auto lx = 0; lx != low.width; ++lx) {
					const auto left = lx * low.factor;
					const auto right = std::min(left + low.factor, width);
					const auto target = sums.data() + size_t(lx) * Count;
					for (auto x = left; x != right; ++x) {
						if (!(line[x] >> 24)) {
							continue;
						}
						float values[Count];
						sample(FxUnpack(line[x]), values);
						for (auto i = 0; i != Count; ++i) {
							if (Minimum) {
								target[i] = std::min(target[i], values[i]);
							} else {
								target[i] += values[i];
							}
						}
						++counts[lx];
					}
				}
			}
			const auto index = size_t(ly) * low.width;
			for (auto lx = 0; lx != low.width; ++lx) {
				if (!counts[lx]) {
					continue;
				}
				filled[index + lx] = 1;
				const auto scale = Minimum ? 1.f : (1.f / counts[lx]);
				for (auto i = 0; i != Count; ++i) {
					result[i][index + lx] = sums[size_t(lx) * Count + i]
						* scale;
				}
			}
		}
	});
	auto visible = size_t(0);
	for (const auto value : filled) {
		visible += value;
	}
	if (visible && visible != filled.size()) {
		for (auto &plane : result) {
			auto sum = 0.;
			for (auto i = size_t(0); i != plane.size(); ++i) {
				if (filled[i]) {
					sum += plane[i];
				}
			}
			const auto average = float(sum / visible);
			for (auto i = size_t(0); i != plane.size(); ++i) {
				if (!filled[i]) {
					plane[i] = average;
				}
			}
		}
	}
	return result;
}

template <int Count, typename Sample>
[[nodiscard]] std::array<std::vector<float>, Count> BuildLow(
		const QImage &image,
		LowSize low,
		const Sample &sample) {
	return BuildLowPlanes<Count, false, Sample>(image, low, sample);
}

struct Guided {
	LowSize low;
	std::vector<float> a;
	std::vector<float> b;

	// The coefficients for the image row y, low.width + 1 values each.
	void row(int y, std::vector<float> &ra, std::vector<float> &rb) const {
		const auto fy = std::clamp(
			(y + 0.5f) / low.factor - 0.5f,
			0.f,
			float(low.height - 1));
		const auto y0 = int(fy);
		const auto y1 = std::min(y0 + 1, low.height - 1);
		const auto t = fy - float(y0);
		const auto a0 = a.data() + size_t(y0) * low.width;
		const auto a1 = a.data() + size_t(y1) * low.width;
		const auto b0 = b.data() + size_t(y0) * low.width;
		const auto b1 = b.data() + size_t(y1) * low.width;
		for (auto x = 0; x != low.width; ++x) {
			ra[x] = a0[x] + (a1[x] - a0[x]) * t;
			rb[x] = b0[x] + (b1[x] - b0[x]) * t;
		}
		ra[low.width] = ra[low.width - 1];
		rb[low.width] = rb[low.width - 1];
	}
};

// sigma is in image pixels. A null input filters the guide itself.
[[nodiscard]] Guided MakeGuided(
		LowSize low,
		const std::vector<float> &guide,
		const std::vector<float> *input,
		double sigma,
		float epsilon) {
	const auto count = low.count();
	const auto blur = [&](std::vector<float> &plane) {
		FxGaussianBlur(plane, low.width, low.height, sigma / low.factor);
	};
	auto meanGuide = guide;
	blur(meanGuide);
	auto square = std::vector<float>(count);
	for (auto i = size_t(0); i != count; ++i) {
		square[i] = guide[i] * guide[i];
	}
	blur(square);
	auto result = Guided{ .low = low };
	result.a.resize(count);
	result.b.resize(count);
	if (!input) {
		for (auto i = size_t(0); i != count; ++i) {
			const auto variance = std::max(
				square[i] - meanGuide[i] * meanGuide[i],
				0.f);
			const auto a = variance / (variance + epsilon);
			result.a[i] = a;
			result.b[i] = meanGuide[i] * (1.f - a);
		}
	} else {
		auto meanInput = *input;
		blur(meanInput);
		auto product = std::vector<float>(count);
		for (auto i = size_t(0); i != count; ++i) {
			product[i] = guide[i] * (*input)[i];
		}
		blur(product);
		for (auto i = size_t(0); i != count; ++i) {
			const auto variance = std::max(
				square[i] - meanGuide[i] * meanGuide[i],
				0.f);
			const auto covariance = product[i] - meanGuide[i] * meanInput[i];
			const auto a = covariance / (variance + epsilon);
			result.a[i] = a;
			result.b[i] = meanInput[i] - a * meanGuide[i];
		}
	}
	blur(result.a);
	blur(result.b);
	return result;
}

// Where the pixels of an image row are in a reduced row.
struct Columns {
	std::vector<int> index;
	std::vector<float> fraction;
};

[[nodiscard]] Columns MakeColumns(int width, LowSize low) {
	auto result = Columns();
	result.index.resize(width);
	result.fraction.resize(width);
	for (auto x = 0; x != width; ++x) {
		const auto fx = std::clamp(
			(x + 0.5f) / low.factor - 0.5f,
			0.f,
			float(low.width - 1));
		result.index[x] = int(fx);
		result.fraction[x] = fx - float(result.index[x]);
	}
	return result;
}

[[nodiscard]] inline float RowValue(
		const std::vector<float> &row,
		const Columns &columns,
		int x) {
	const auto data = row.data() + columns.index[x];
	return data[0] + (data[1] - data[0]) * columns.fraction[x];
}

// The rows of one or several guided filters for a worker thread.
struct GuidedRows {
	std::vector<float> a;
	std::vector<float> b;

	GuidedRows() = default;
	explicit GuidedRows(const Guided &guided)
	: a(guided.low.width + 1)
	, b(guided.low.width + 1) {
	}
};

// Like FxForEachColor, with a per-thread state made by make() and
// prepared for every row by row(state, y).
template <typename Make, typename Row, typename Op>
void ForEachColorRows(
		QImage &image,
		const FxContext &context,
		const Make &make,
		const Row &row,
		const Op &op) {
	const auto width = image.width();
	FxParallelRows(width, image.height(), [&](int from, int till) {
		if (context.cancelled()) {
			return;
		}
		auto state = make();
		for (auto y = from; y != till; ++y) {
			row(state, y);
			const auto line = FxRow(image, y);
			for (auto x = 0; x != width; ++x) {
				if (!(line[x] >> 24)) {
					continue;
				}
				auto color = FxUnpack(line[x]);
				op(color, state, x, y);
				line[x] = FxPack(color);
			}
		}
	});
}

// A gaussian blur that also works for a sigma smaller than a pixel:
// a 3 x 3 kernel with the same variance is used there, so the result
// changes smoothly with the sigma (and with the scale of the preview).
void BlurPlaneAny(
		std::vector<float> &plane,
		int width,
		int height,
		double sigma) {
	if (sigma <= 0.01 || width < 1 || height < 1) {
		return;
	} else if (sigma >= 0.8) {
		FxGaussianBlur(plane, width, height, sigma);
		return;
	}
	const auto side = float(sigma * sigma / 2.);
	const auto center = 1.f - 2.f * side;
	auto source = plane;
	FxParallelRows(width, height, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto above = source.data()
				+ size_t(std::max(y - 1, 0)) * width;
			const auto line = source.data() + size_t(y) * width;
			const auto below = source.data()
				+ size_t(std::min(y + 1, height - 1)) * width;
			const auto target = plane.data() + size_t(y) * width;
			for (auto x = 0; x != width; ++x) {
				const auto left = std::max(x - 1, 0);
				const auto right = std::min(x + 1, width - 1);
				const auto horizontal = [&](const float *row) {
					return row[x] * center + (row[left] + row[right]) * side;
				};
				target[x] = horizontal(line) * center
					+ (horizontal(above) + horizontal(below)) * side;
			}
		}
	});
}

// Minimum over a (2 * radius + 1) square.
void ErodePlane(std::vector<float> &plane, int width, int height, int radius) {
	if (radius <= 0) {
		return;
	}
	auto temp = plane;
	for (auto y = 0; y != height; ++y) {
		const auto line = plane.data() + size_t(y) * width;
		const auto target = temp.data() + size_t(y) * width;
		for (auto x = 0; x != width; ++x) {
			const auto from = std::max(x - radius, 0);
			const auto till = std::min(x + radius, width - 1);
			auto value = line[from];
			for (auto i = from + 1; i <= till; ++i) {
				value = std::min(value, line[i]);
			}
			target[x] = value;
		}
	}
	for (auto y = 0; y != height; ++y) {
		const auto from = std::max(y - radius, 0);
		const auto till = std::min(y + radius, height - 1);
		const auto target = plane.data() + size_t(y) * width;
		std::copy_n(temp.data() + size_t(from) * width, width, target);
		for (auto i = from + 1; i <= till; ++i) {
			const auto line = temp.data() + size_t(i) * width;
			for (auto x = 0; x != width; ++x) {
				target[x] = std::min(target[x], line[x]);
			}
		}
	}
}

//
// adjust.light
//

struct LightValues {
	float exposure = 0.f; // EV.
	float contrast = 0.f; // The rest is -1..1.
	float highlights = 0.f;
	float shadows = 0.f;
	float whites = 0.f;
	float blacks = 0.f;

	[[nodiscard]] bool neutral() const {
		return (exposure == 0.f)
			&& (contrast == 0.f)
			&& (highlights == 0.f)
			&& (shadows == 0.f)
			&& (whites == 0.f)
			&& (blacks == 0.f);
	}
};

[[nodiscard]] LightValues ReadLight(const FxParams &params) {
	return {
		.exposure = float(params.number("exposure")),
		.contrast = float(params.number("contrast") / 100.),
		.highlights = float(params.number("highlights") / 100.),
		.shadows = float(params.number("shadows") / 100.),
		.whites = float(params.number("whites") / 100.),
		.blacks = float(params.number("blacks") / 100.),
	};
}

// The global curve of the encoded luminance: the black and the white
// ends move (only the darkest / the brightest tones follow), then
// a smooth S-curve around the middle. Monotone for all values.
[[nodiscard]] double GlobalTone(double x, const LightValues &values) {
	constexpr auto kEnd = 0.15;
	auto result = x
		+ values.blacks * kEnd * std::pow(1. - x, 5.)
		+ values.whites * kEnd * std::pow(x, 5.);
	result = std::clamp(result, 0., 1.);
	if (values.contrast != 0.f) {
		result -= 0.8 * values.contrast
			* std::sin(2. * kPi * result) / (2. * kPi);
	}
	return std::clamp(result, 0., 1.);
}

// What highlights / shadows add to a pixel whose surroundings have the
// encoded luminance base. Zero at black and white: the ends stay. The
// strengths are the largest that keep base + delta monotone for any
// pair of values (lifted shadows together with recovered highlights).
[[nodiscard]] double LocalToneDelta(double base, const LightValues &values) {
	constexpr auto kOpen = 2.4; // Lifts shadows, recovers highlights.
	constexpr auto kClose = 0.9; // Deepens shadows, brightens highlights.
	const auto dark = base * std::pow(1. - base, 4.);
	const auto bright = (1. - base) * std::pow(base, 4.);
	return values.shadows * ((values.shadows > 0.f) ? kOpen : kClose) * dark
		+ values.highlights
			* ((values.highlights < 0.f) ? kOpen : kClose)
			* bright;
}

// A gain in linear light with a soft shoulder instead of a hard clip.
struct Exposure {
	float gain = 1.f;
	float knee = 1.f;
	bool active = false;

	explicit Exposure(float stops)
	: gain(std::exp2(stops))
	, knee((stops > 0.f) ? (1.f / std::sqrt(std::exp2(stops))) : 1.f)
	, active(stops != 0.f) {
	}

	[[nodiscard]] float operator()(float value) const {
		value *= gain;
		if (gain > 1.f && value > knee) {
			const auto range = 1.f - knee;
			value = knee + range * (1.f - std::exp(-(value - knee) / range));
		}
		return value;
	}
};

[[nodiscard]] bool ApplyLight(
		QImage &image,
		const LightValues &values,
		const FxContext &context) {
	const auto &gamma = Gamma();
	const auto exposure = Exposure(values.exposure);
	const auto tonal = (values.contrast != 0.f)
		|| (values.whites != 0.f)
		|| (values.blacks != 0.f);
	const auto local = (values.highlights != 0.f) || (values.shadows != 0.f);
	const auto linear = [&](const FxRgba &color, float &r, float &g, float &b) {
		r = Decode(gamma, color.r);
		g = Decode(gamma, color.g);
		b = Decode(gamma, color.b);
		if (exposure.active) {
			r = exposure(r);
			g = exposure(g);
			b = exposure(b);
		}
	};

	auto guided = Guided();
	auto columns = Columns();
	if (local) {
		const auto sigma = std::max(
			0.035 * std::min(image.width(), image.height()),
			1.);
		const auto low = LowForSigma(image.size(), sigma);
		const auto planes = BuildLow<1>(image, low, [&](
				const FxRgba &color,
				float *result) {
			float r, g, b;
			linear(color, r, g, b);
			result[0] = Encode(gamma, Luma(r, g, b));
		});
		if (context.cancelled()) {
			return false;
		}
		guided = MakeGuided(low, planes[0], nullptr, sigma, 0.015f);
		columns = MakeColumns(image.width(), low);
		if (context.cancelled()) {
			return false;
		}
	}
	const auto global = Lut(kToneSize, [&](double x) {
		return tonal ? GlobalTone(x, values) : x;
	});
	const auto delta = Lut(kToneSize, [&](double x) {
		return local ? LocalToneDelta(x, values) : 0.;
	});
	ForEachColorRows(image, context, [&] {
		return local ? GuidedRows(guided) : GuidedRows();
	}, [&](GuidedRows &rows, int y) {
		if (local) {
			guided.row(y, rows.a, rows.b);
		}
	}, [&](FxRgba &color, const GuidedRows &rows, int x, int) {
		float r, g, b;
		linear(color, r, g, b);
		if (tonal || local) {
			const auto luma = Luma(r, g, b);
			auto level = Encode(gamma, luma);
			if (local) {
				const auto base = RowValue(rows.a, columns, x) * level
					+ RowValue(rows.b, columns, x);
				level += delta(base);
			}
			level = global(level);
			Relight(r, g, b, luma, Decode(gamma, level));
		}
		color.r = Encode(gamma, r);
		color.g = Encode(gamma, g);
		color.b = Encode(gamma, b);
	});
	return !context.cancelled();
}

//
// adjust.curve
//

[[nodiscard]] std::vector<double> CurveSlopes(const CurvePoints &points) {
	const auto count = int(points.size());
	auto result = std::vector<double>(count, 0.);
	if (count < 2) {
		return result;
	}
	auto steps = std::vector<double>(count - 1);
	auto deltas = std::vector<double>(count - 1);
	for (auto i = 0; i + 1 != count; ++i) {
		steps[i] = std::max(points[i + 1].x - points[i].x, 1)
			/ double(kCurveUnit);
		deltas[i] = (points[i + 1].y - points[i].y)
			/ double(kCurveUnit)
			/ steps[i];
	}
	if (count == 2) {
		result[0] = result[1] = deltas[0];
		return result;
	}
	// PCHIP (Fritsch - Butland): a weighted harmonic mean of the two
	// secants, zero where they change the sign. The interpolant keeps
	// the monotonicity of the points and never overshoots.
	for (auto i = 1; i + 1 != count; ++i) {
		const auto before = deltas[i - 1];
		const auto after = deltas[i];
		if (before * after <= 0.) {
			result[i] = 0.;
		} else {
			const auto w1 = 2. * steps[i] + steps[i - 1];
			const auto w2 = steps[i] + 2. * steps[i - 1];
			result[i] = (w1 + w2) / (w1 / before + w2 / after);
		}
	}
	const auto edge = [](double h0, double h1, double d0, double d1) {
		auto slope = ((2. * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
		if (slope * d0 <= 0.) {
			slope = 0.;
		} else if (d0 * d1 <= 0. && std::abs(slope) > 3. * std::abs(d0)) {
			slope = 3. * d0;
		}
		return slope;
	};
	result[0] = edge(steps[0], steps[1], deltas[0], deltas[1]);
	result[count - 1] = edge(
		steps[count - 2],
		steps[count - 3],
		deltas[count - 2],
		deltas[count - 3]);
	return result;
}

[[nodiscard]] double CurveValueWith(
		const CurvePoints &points,
		const std::vector<double> &slopes,
		double x) {
	if (points.empty()) {
		return std::clamp(x, 0., 1.);
	}
	const auto unit = double(kCurveUnit);
	if (x <= points.front().x / unit) {
		return points.front().y / unit;
	} else if (x >= points.back().x / unit) {
		return points.back().y / unit;
	}
	auto index = 0;
	const auto last = int(points.size()) - 2;
	while (index < last && x > points[index + 1].x / unit) {
		++index;
	}
	const auto x0 = points[index].x / unit;
	const auto x1 = points[index + 1].x / unit;
	const auto y0 = points[index].y / unit;
	const auto y1 = points[index + 1].y / unit;
	const auto h = x1 - x0;
	const auto t = (x - x0) / h;
	const auto t2 = t * t;
	const auto t3 = t2 * t;
	const auto value = (2. * t3 - 3. * t2 + 1.) * y0
		+ (t3 - 2. * t2 + t) * h * slopes[index]
		+ (-2. * t3 + 3. * t2) * y1
		+ (t3 - t2) * h * slopes[index + 1];
	return std::clamp(value, std::min(y0, y1), std::max(y0, y1));
}

[[nodiscard]] bool ApplyCurve(
		QImage &image,
		const ToneCurve &curve,
		const FxContext &context) {
	// One table per color channel: the channel curve of the master one.
	auto master = std::vector<float>(kCurveTable);
	FillCurveTable(curve.channels[0], master.data(), kCurveTable);
	auto tables = std::array<std::vector<float>, 3>();
	auto bytes = std::array<std::array<uchar, 256>, 3>();
	for (auto channel = 0; channel != 3; ++channel) {
		const auto &points = curve.channels[channel + 1];
		auto &table = tables[channel];
		table.resize(kCurveTable + 1);
		if (curve.identity(channel + 1)) {
			std::copy(begin(master), end(master), begin(table));
		} else {
			const auto slopes = CurveSlopes(points);
			for (auto i = 0; i != kCurveTable; ++i) {
				table[i] = float(CurveValueWith(points, slopes, master[i]));
			}
		}
		table[kCurveTable] = table[kCurveTable - 1];
		for (auto i = 0; i != 256; ++i) {
			bytes[channel][i] = uchar(std::clamp(
				int(table[i * 4] * 255.f + 0.5f),
				0,
				255));
		}
	}
	const auto map = [&](int channel, float value) {
		const auto f = FxClamp01(value) * float(kCurveTable - 1);
		const auto i = int(f);
		const auto data = tables[channel].data() + i;
		return data[0] + (data[1] - data[0]) * (f - float(i));
	};
	const auto width = image.width();
	FxParallelRows(width, image.height(), [&](int from, int till) {
		if (context.cancelled()) {
			return;
		}
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			for (auto x = 0; x != width; ++x) {
				const auto pixel = line[x];
				const auto alpha = (pixel >> 24);
				if (alpha == 0xFFU) {
					line[x] = 0xFF000000U
						| (uint32(bytes[0][(pixel >> 16) & 0xFFU]) << 16)
						| (uint32(bytes[1][(pixel >> 8) & 0xFFU]) << 8)
						| uint32(bytes[2][pixel & 0xFFU]);
				} else if (alpha) {
					auto color = FxUnpack(pixel);
					color.r = map(0, color.r);
					color.g = map(1, color.g);
					color.b = map(2, color.b);
					line[x] = FxPack(color);
				}
			}
		}
	});
	return !context.cancelled();
}

//
// adjust.color
//

struct ColorValues {
	float temperature = 0.f; // Everything is -1..1.
	float tint = 0.f;
	float vibrance = 0.f;
	float saturation = 0.f;

	[[nodiscard]] bool neutral() const {
		return (temperature == 0.f)
			&& (tint == 0.f)
			&& (vibrance == 0.f)
			&& (saturation == 0.f);
	}
};

struct Gains {
	float r = 1.f;
	float g = 1.f;
	float b = 1.f;
};

// The color of a black body in linear sRGB (Kim et al. approximation of
// the Planckian locus, 1667 K .. 25000 K), the luminance is one.
[[nodiscard]] std::array<double, 3> BlackBody(double kelvin) {
	const auto t = std::clamp(kelvin, 1667., 25000.);
	const auto x = (t <= 4000.)
		? (-0.2661239e9 / (t * t * t)
			- 0.2343589e6 / (t * t)
			+ 0.8776956e3 / t
			+ 0.179910)
		: (-3.0258469e9 / (t * t * t)
			+ 2.1070379e6 / (t * t)
			+ 0.2226347e3 / t
			+ 0.240390);
	const auto y = (t <= 2222.)
		? (-1.1063814 * x * x * x
			- 1.34811020 * x * x
			+ 2.18555832 * x
			- 0.20219683)
		: (t <= 4000.)
		? (-0.9549476 * x * x * x
			- 1.37418593 * x * x
			+ 2.09137015 * x
			- 0.16748867)
		: (3.0817580 * x * x * x
			- 5.87338670 * x * x
			+ 3.75112997 * x
			- 0.37001483);
	const auto bigX = x / y;
	const auto bigZ = (1. - x - y) / y;
	return {
		3.2404542 * bigX - 1.5371385 - 0.4985314 * bigZ,
		-0.9692660 * bigX + 1.8760108 + 0.0415560 * bigZ,
		0.0556434 * bigX - 0.2040259 + 1.0572252 * bigZ,
	};
}

// Channel gains for linear light: the white point moves along the
// Planckian locus (in mireds, so both directions feel the same) and
// across it for the tint. The luminance of a gray stays.
[[nodiscard]] Gains WhiteBalance(float temperature, float tint) {
	constexpr auto kNeutral = 6500.;
	constexpr auto kMiredRange = 90.;
	const auto origin = BlackBody(kNeutral);
	const auto target = BlackBody(
		1e6 / (1e6 / kNeutral + temperature * kMiredRange));
	auto r = std::max(target[0], 1e-3) / std::max(origin[0], 1e-3);
	auto g = std::max(target[1], 1e-3) / std::max(origin[1], 1e-3);
	auto b = std::max(target[2], 1e-3) / std::max(origin[2], 1e-3);
	g *= std::exp2(-0.25 * tint);
	const auto luma = kLumaR * r + kLumaG * g + kLumaB * b;
	return { float(r / luma), float(g / luma), float(b / luma) };
}

[[nodiscard]] bool ApplyColor(
		QImage &image,
		const ColorValues &values,
		const FxContext &context) {
	const auto &gamma = Gamma();
	const auto balance = (values.temperature != 0.f) || (values.tint != 0.f);
	const auto gains = WhiteBalance(values.temperature, values.tint);
	const auto chroma = (values.vibrance != 0.f) || (values.saturation != 0.f);
	const auto saturation = 1.f + values.saturation;
	const auto vibrance = values.vibrance;
	// Skin tones get only a half of a positive vibrance.
	auto protect = std::make_unique<HueTable>();
	for (auto i = 0; i != kHueSize + 2; ++i) {
		const auto degrees = (i % kHueSize) * (360.f / kHueSize);
		const auto distance = std::abs(degrees - 52.f);
		(*protect)[i] = 1.f - 0.5f * (1.f - FxSmoothStep(8.f, 30.f, distance));
	}
	FxForEachColor(image, [&](FxRgba &color, int, int) {
		auto r = Decode(gamma, color.r);
		auto g = Decode(gamma, color.g);
		auto b = Decode(gamma, color.b);
		if (balance) {
			r *= gains.r;
			g *= gains.g;
			b *= gains.b;
			FitGamut(r, g, b, Luma(r, g, b));
		}
		if (chroma) {
			auto lab = ToLab(r, g, b);
			auto factor = saturation;
			if (vibrance != 0.f) {
				const auto amount = std::sqrt(lab.a * lab.a + lab.b * lab.b);
				const auto muted = 1.f - FxSmoothStep(0.f, 0.22f, amount);
				if (vibrance > 0.f) {
					const auto skin = (amount > 1e-4f)
						? HueValue(*protect, HuePosition(lab.a, lab.b))
						: 1.f;
					factor *= 1.f + vibrance * 1.2f * muted * skin;
				} else {
					factor *= 1.f + vibrance * (0.35f + 0.65f * muted);
				}
			}
			const auto gray = lab.l * lab.l * lab.l;
			if (factor <= 0.f) {
				r = g = b = gray;
			} else {
				lab.a *= factor;
				lab.b *= factor;
				FromLab(lab, r, g, b);
				FitGamut(r, g, b, gray);
			}
		}
		color.r = Encode(gamma, r);
		color.g = Encode(gamma, g);
		color.b = Encode(gamma, b);
	});
	return !context.cancelled();
}

//
// adjust.hsl
//

[[nodiscard]] bool ApplyHsl(
		QImage &image,
		const HslTable &table,
		const FxContext &context) {
	const auto &gamma = Gamma();
	constexpr auto kHueShare = 0.75f; // Of the way to the next range.
	constexpr auto kLuminance = 0.4f;
	struct Tables {
		HueTable cosine;
		HueTable sine;
		HueTable saturation;
		HueTable luminance;
	};
	auto tables = std::make_unique<Tables>();
	auto shift = std::make_unique<HueTable>();
	FillHueTable(*shift, [&](int range) {
		const auto value = table.hue[range] / 100.f;
		return value * kHueShare * RangeStep(range, value > 0.f);
	});
	for (auto i = 0; i != kHueSize + 2; ++i) {
		const auto radians = (*shift)[i] * float(kPi / 180.);
		tables->cosine[i] = std::cos(radians);
		tables->sine[i] = std::sin(radians);
	}
	FillHueTable(tables->saturation, [&](int range) {
		return table.saturation[range] / 100.f;
	});
	FillHueTable(tables->luminance, [&](int range) {
		return kLuminance * table.luminance[range] / 100.f;
	});
	const auto rotate = ranges::any_of(table.hue, [](int value) {
		return value != 0;
	});
	FxForEachColor(image, [&](FxRgba &color, int, int) {
		auto r = Decode(gamma, color.r);
		auto g = Decode(gamma, color.g);
		auto b = Decode(gamma, color.b);
		auto lab = ToLab(r, g, b);
		const auto amount = std::sqrt(lab.a * lab.a + lab.b * lab.b);
		if (amount < 1e-4f) {
			return;
		}
		// The hue of a nearly gray pixel is noise: fade everything in.
		const auto weight = FxSmoothStep(0.f, 0.05f, amount);
		const auto position = HuePosition(lab.a, lab.b);
		if (rotate) {
			const auto cosine = HueValue(tables->cosine, position);
			const auto sine = HueValue(tables->sine, position);
			const auto a = lab.a * cosine - lab.b * sine;
			const auto b2 = lab.a * sine + lab.b * cosine;
			lab.a += (a - lab.a) * weight;
			lab.b += (b2 - lab.b) * weight;
		}
		const auto scale = std::max(
			1.f + HueValue(tables->saturation, position) * weight,
			0.f);
		lab.a *= scale;
		lab.b *= scale;
		lab.l = FxClamp01(lab.l
			* (1.f
				+ HueValue(tables->luminance, position)
					* FxSmoothStep(0.f, 0.12f, amount)));
		FromLab(lab, r, g, b);
		FitGamut(r, g, b, lab.l * lab.l * lab.l);
		color.r = Encode(gamma, r);
		color.g = Encode(gamma, g);
		color.b = Encode(gamma, b);
	});
	return !context.cancelled();
}

//
// adjust.grading
//

struct GradingValues {
	GradeWheels wheels;
	std::array<float, kGradeRanges> luminance = {}; // -1..1.
	float blending = 0.5f; // 0..1.
	float balance = 0.f; // -1..1.

	[[nodiscard]] bool neutral() const {
		return wheels.identity()
			&& (luminance[0] == 0.f)
			&& (luminance[1] == 0.f)
			&& (luminance[2] == 0.f);
	}
};

// The direction of an HSV hue in the a, b plane of Oklab.
[[nodiscard]] QPointF HueDirection(int hue) {
	const auto color = QColor::fromHsv(((hue % 360) + 360) % 360, 255, 255);
	const auto lab = ToLab(
		float(FxSrgbToLinear(color.redF())),
		float(FxSrgbToLinear(color.greenF())),
		float(FxSrgbToLinear(color.blueF())));
	const auto length = std::sqrt(lab.a * lab.a + lab.b * lab.b);
	return (length > 0.f)
		? QPointF(lab.a / length, lab.b / length)
		: QPointF();
}

[[nodiscard]] bool ApplyGrading(
		QImage &image,
		const GradingValues &values,
		const FxContext &context) {
	const auto &gamma = Gamma();
	constexpr auto kTint = 0.09f; // Oklab chroma of a full saturation.
	constexpr auto kLuminance = 0.14f; // Oklab lightness of +100.
	struct Range {
		float a = 0.f;
		float b = 0.f;
		float l = 0.f;
	};
	auto list = std::array<Range, kGradeRanges>();
	for (auto i = 0; i != kGradeRanges; ++i) {
		const auto &wheel = values.wheels.ranges[i];
		const auto direction = HueDirection(wheel.hue);
		const auto strength = kTint * wheel.saturation / 100.f;
		list[i] = {
			float(direction.x()) * strength,
			float(direction.y()) * strength,
			kLuminance * values.luminance[i],
		};
	}
	// The weights of the shadows and of the highlights by the lightness:
	// the balance moves the middle, the blending widens the overlap.
	const auto power = std::exp2(-double(values.balance));
	const auto edge = 0.25 + 0.5 * values.blending;
	const auto smooth = [](double value) {
		const auto t = std::clamp(value, 0., 1.);
		return t * t * (3. - 2. * t);
	};
	const auto dark = Lut(kToneSize, [&](double x) {
		return 1. - smooth(std::pow(x, power) / edge);
	});
	const auto bright = Lut(kToneSize, [&](double x) {
		return smooth((std::pow(x, power) - (1. - edge)) / edge);
	});
	FxForEachColor(image, [&](FxRgba &color, int, int) {
		auto r = Decode(gamma, color.r);
		auto g = Decode(gamma, color.g);
		auto b = Decode(gamma, color.b);
		auto lab = ToLab(r, g, b);
		const auto low = dark(lab.l);
		const auto high = bright(lab.l);
		const auto middle = std::max(1.f - low - high, 0.f);
		lab.a += low * list[0].a + middle * list[1].a + high * list[2].a;
		lab.b += low * list[0].b + middle * list[1].b + high * list[2].b;
		lab.l = FxClamp01(lab.l
			+ low * list[0].l
			+ middle * list[1].l
			+ high * list[2].l);
		FromLab(lab, r, g, b);
		// Like the lift and the gain of a color wheel: the tint lifts
		// black and filters white instead of being clipped away.
		const auto lowest = std::min({ r, g, b });
		if (lowest < 0.f) {
			r -= lowest;
			g -= lowest;
			b -= lowest;
		}
		const auto highest = std::max({ r, g, b });
		if (highest > 1.f) {
			r /= highest;
			g /= highest;
			b /= highest;
		}
		color.r = Encode(gamma, r);
		color.g = Encode(gamma, g);
		color.b = Encode(gamma, b);
	});
	return !context.cancelled();
}

//
// adjust.bw
//

[[nodiscard]] bool ApplyBlackWhite(
		QImage &image,
		const std::array<float, kHslRanges> &mix, // -1..1.
		const FxContext &context) {
	const auto &gamma = Gamma();
	constexpr auto kStrength = 0.45f;
	auto table = std::make_unique<HueTable>();
	FillHueTable(*table, [&](int range) {
		return kStrength * mix[range];
	});
	const auto mixed = ranges::any_of(mix, [](float value) {
		return value != 0.f;
	});
	FxForEachColor(image, [&](FxRgba &color, int, int) {
		const auto r = Decode(gamma, color.r);
		const auto g = Decode(gamma, color.g);
		const auto b = Decode(gamma, color.b);
		auto luma = Luma(r, g, b);
		if (mixed) {
			const auto lab = ToLab(r, g, b);
			const auto amount = std::sqrt(lab.a * lab.a + lab.b * lab.b);
			if (amount > 1e-4f) {
				// The lightness changes with the color of the pixel, the
				// luminance follows its cube.
				const auto factor = 1.f
					+ HueValue(*table, HuePosition(lab.a, lab.b))
						* FxSmoothStep(0.f, 0.16f, amount);
				luma *= factor * factor * factor;
			}
		}
		color.r = color.g = color.b = Encode(gamma, luma);
	});
	return !context.cancelled();
}

//
// adjust.presence
//

struct PresenceValues {
	float texture = 0.f; // Everything is -1..1.
	float clarity = 0.f;
	float dehaze = 0.f;

	[[nodiscard]] bool neutral() const {
		return (texture == 0.f) && (clarity == 0.f) && (dehaze == 0.f);
	}
};

// Haze removal with the dark channel prior (He, Sun, Tang): in a clear
// picture every small patch has a pixel that is dark in some channel, so
// the minimum over a patch estimates how much of the airlight was added.
// The estimate is made on a reduced picture and refined with a guided
// filter, so it follows the edges. A negative amount adds haze.
[[nodiscard]] bool ApplyDehaze(
		QImage &image,
		float amount,
		const FxContext &context) {
	const auto &gamma = Gamma();
	const auto side = std::min(image.width(), image.height());
	const auto low = LowForFactor(
		image.size(),
		int(std::lround(side / 256.)));
	const auto linear = [&](const FxRgba &color, float *result) {
		result[0] = Decode(gamma, color.r);
		result[1] = Decode(gamma, color.g);
		result[2] = Decode(gamma, color.b);
	};
	const auto means = BuildLow<4>(image, low, [&](
			const FxRgba &color,
			float *result) {
		linear(color, result);
		result[3] = Encode(gamma, Luma(result[0], result[1], result[2]));
	});
	if (context.cancelled()) {
		return false;
	}
	const auto lowest = BuildLowPlanes<3, true>(image, low, linear);
	if (context.cancelled()) {
		return false;
	}
	const auto count = low.count();
	if (!count) {
		return true;
	}

	// The airlight: the color of the haziest blocks.
	auto dark = std::vector<float>(count);
	for (auto i = size_t(0); i != count; ++i) {
		dark[i] = std::min({ lowest[0][i], lowest[1][i], lowest[2][i] });
	}
	auto order = std::vector<int>(count);
	std::iota(begin(order), end(order), 0);
	const auto take = std::max(count / 1000, size_t(1));
	std::nth_element(
		begin(order),
		begin(order) + (take - 1),
		end(order),
		[&](int a, int b) {
			return (dark[a] != dark[b]) ? (dark[a] > dark[b]) : (a < b);
		});
	auto light = std::array<double, 3>();
	for (auto i = size_t(0); i != take; ++i) {
		for (auto channel = 0; channel != 3; ++channel) {
			light[channel] += means[channel][order[i]];
		}
	}
	auto air = std::array<float, 3>();
	const auto airLuma = Luma(
		float(light[0] / take),
		float(light[1] / take),
		float(light[2] / take));
	for (auto channel = 0; channel != 3; ++channel) {
		// A strongly colored airlight would tint the whole picture.
		const auto value = float(light[channel] / take);
		air[channel] = std::clamp(value + (airLuma - value) * 0.3f, 0.2f, 1.f);
	}

	for (auto i = size_t(0); i != count; ++i) {
		dark[i] = FxClamp01(std::min({
			lowest[0][i] / air[0],
			lowest[1][i] / air[1],
			lowest[2][i] / air[2],
		}));
	}
	const auto radius = std::max(
		int(std::lround(0.012 * side / low.factor)),
		1);
	ErodePlane(dark, low.width, low.height, radius);
	const auto guided = MakeGuided(
		low,
		means[3],
		&dark,
		3. * radius * low.factor,
		0.003f);
	const auto columns = MakeColumns(image.width(), low);
	if (context.cancelled()) {
		return false;
	}

	const auto remove = (amount > 0.f);
	const auto strength = std::abs(amount);
	const auto fog = std::array<float, 3>{ {
		air[0] + (1.f - air[0]) * 0.5f,
		air[1] + (1.f - air[1]) * 0.5f,
		air[2] + (1.f - air[2]) * 0.5f,
	} };
	ForEachColorRows(image, context, [&] {
		return GuidedRows(guided);
	}, [&](GuidedRows &rows, int y) {
		guided.row(y, rows.a, rows.b);
	}, [&](FxRgba &color, const GuidedRows &rows, int x, int) {
		auto r = Decode(gamma, color.r);
		auto g = Decode(gamma, color.g);
		auto b = Decode(gamma, color.b);
		const auto level = Encode(gamma, Luma(r, g, b));
		const auto haze = FxClamp01(RowValue(rows.a, columns, x) * level
			+ RowValue(rows.b, columns, x));
		if (remove) {
			const auto through = std::max(1.f - 0.95f * strength * haze, 0.1f);
			const auto inverse = 1.f / through;
			r = std::max((r - air[0]) * inverse + air[0], 0.f);
			g = std::max((g - air[1]) * inverse + air[1], 0.f);
			b = std::max((b - air[2]) * inverse + air[2], 0.f);
			FitGamut(r, g, b, Luma(r, g, b));
		} else {
			const auto through = 1.f
				- 0.6f * strength * (0.35f + 0.65f * haze);
			r = r * through + fog[0] * (1.f - through);
			g = g * through + fog[1] * (1.f - through);
			b = b * through + fog[2] * (1.f - through);
		}
		color.r = Encode(gamma, r);
		color.g = Encode(gamma, g);
		color.b = Encode(gamma, b);
	});
	return !context.cancelled();
}

// Texture and clarity: the difference between the encoded luminance and
// its edge-aware base (small radius for the texture, large for the
// clarity) is amplified or reduced. Strong edges stay in the base, so
// they get no halos.
[[nodiscard]] bool ApplyLocalContrast(
		QImage &image,
		float texture,
		float clarity,
		const FxContext &context) {
	const auto &gamma = Gamma();
	const auto side = std::min(image.width(), image.height());
	const auto level = [&](const FxRgba &color, float *result) {
		result[0] = Encode(gamma, Luma(
			Decode(gamma, color.r),
			Decode(gamma, color.g),
			Decode(gamma, color.b)));
	};
	struct Field {
		Guided guided;
		Columns columns;
		bool used = false;
	};
	const auto make = [&](double sigma, float epsilon) {
		const auto low = LowForSigma(image.size(), sigma);
		const auto planes = BuildLow<1>(image, low, level);
		return Field{
			MakeGuided(low, planes[0], nullptr, sigma, epsilon),
			MakeColumns(image.width(), low),
			true,
		};
	};
	auto coarse = Field();
	auto fine = Field();
	if (clarity != 0.f) {
		coarse = make(std::max(0.03 * side, 1.5), 0.003f);
		if (context.cancelled()) {
			return false;
		}
	}
	if (texture != 0.f) {
		fine = make(std::max(0.0045 * side, 0.7), 0.002f);
		if (context.cancelled()) {
			return false;
		}
	}
	const auto clarityGain = clarity * ((clarity > 0.f) ? 1.5f : 0.75f);
	const auto textureGain = texture * ((texture > 0.f) ? 1.6f : 0.9f);
	struct Rows {
		GuidedRows coarse;
		GuidedRows fine;
	};
	ForEachColorRows(image, context, [&] {
		return Rows{
			coarse.used ? GuidedRows(coarse.guided) : GuidedRows(),
			fine.used ? GuidedRows(fine.guided) : GuidedRows(),
		};
	}, [&](Rows &rows, int y) {
		if (coarse.used) {
			coarse.guided.row(y, rows.coarse.a, rows.coarse.b);
		}
		if (fine.used) {
			fine.guided.row(y, rows.fine.a, rows.fine.b);
		}
	}, [&](FxRgba &color, const Rows &rows, int x, int) {
		auto r = Decode(gamma, color.r);
		auto g = Decode(gamma, color.g);
		auto b = Decode(gamma, color.b);
		const auto luma = Luma(r, g, b);
		const auto value = Encode(gamma, luma);
		// The darkest and the brightest tones are protected.
		const auto centered = 2.f * value - 1.f;
		const auto squared = centered * centered;
		const auto weight = 1.f - squared * squared;
		auto change = 0.f;
		if (coarse.used) {
			const auto base = RowValue(rows.coarse.a, coarse.columns, x) * value
				+ RowValue(rows.coarse.b, coarse.columns, x);
			// What is left of a strong edge in the difference is large:
			// it is not amplified, so the edge gets no halo.
			const auto detail = (value - base) * (1.f / 0.06f);
			change += clarityGain * (value - base) / (1.f + detail * detail);
		}
		if (fine.used) {
			const auto base = RowValue(rows.fine.a, fine.columns, x) * value
				+ RowValue(rows.fine.b, fine.columns, x);
			auto detail = value - base;
			if (textureGain > 0.f) {
				// Not the noise: the smallest differences stay.
				const auto size = std::abs(detail);
				detail *= size / (size + 0.006f);
			}
			change += textureGain * detail;
		}
		const auto target = Decode(gamma, FxClamp01(value + change * weight));
		Relight(r, g, b, luma, target);
		color.r = Encode(gamma, r);
		color.g = Encode(gamma, g);
		color.b = Encode(gamma, b);
	});
	return !context.cancelled();
}

[[nodiscard]] bool ApplyPresence(
		QImage &image,
		const PresenceValues &values,
		const FxContext &context) {
	if (values.dehaze != 0.f
		&& !ApplyDehaze(image, values.dehaze, context)) {
		return false;
	}
	if ((values.texture != 0.f || values.clarity != 0.f)
		&& !ApplyLocalContrast(
			image,
			values.texture,
			values.clarity,
			context)) {
		return false;
	}
	return !context.cancelled();
}

//
// adjust.sharpen
//

// The encoded luminance of every pixel (0 for transparent ones) and
// whether there are transparent pixels.
[[nodiscard]] std::vector<float> LevelPlane(
		const QImage &image,
		bool *transparent) {
	const auto &gamma = Gamma();
	const auto width = image.width();
	const auto height = image.height();
	auto result = std::vector<float>(size_t(width) * height);
	auto holes = std::atomic<bool>(false);
	FxParallelRows(width, height, [&](int from, int till) {
		auto found = false;
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto target = result.data() + size_t(y) * width;
			for (auto x = 0; x != width; ++x) {
				if (!(line[x] >> 24)) {
					target[x] = 0.f;
					found = true;
					continue;
				}
				const auto color = FxUnpack(line[x]);
				target[x] = Encode(gamma, Luma(
					Decode(gamma, color.r),
					Decode(gamma, color.g),
					Decode(gamma, color.b)));
			}
		}
		if (found) {
			holes = true;
		}
	});
	if (transparent) {
		*transparent = holes.load();
	}
	return result;
}

// Unsharp mask of the luminance. The radius is in rendered pixels.
// Masking keeps the sharpening on the edges and off the flat areas
// (where it would only show the noise).
[[nodiscard]] bool ApplySharpen(
		QImage &image,
		float amount, // 0..1.5.
		double radius,
		float masking, // 0..1.
		const FxContext &context) {
	const auto &gamma = Gamma();
	const auto width = image.width();
	const auto height = image.height();
	auto transparent = false;
	const auto levels = LevelPlane(image, &transparent);
	if (context.cancelled()) {
		return false;
	}
	// The local deviation of the luminance: large at the edges, small
	// where there is only noise. It is counted first, its temporary plane
	// is gone before the next one is made (a large picture needs a lot).
	auto edges = std::vector<float>();
	if (masking > 0.f) {
		const auto window = std::max(2. * radius, 1.2);
		auto mean = levels;
		BlurPlaneAny(mean, width, height, window);
		edges.resize(levels.size());
		for (auto i = size_t(0); i != levels.size(); ++i) {
			edges[i] = levels[i] * levels[i];
		}
		BlurPlaneAny(edges, width, height, window);
		for (auto i = size_t(0); i != levels.size(); ++i) {
			edges[i] = std::sqrt(std::max(edges[i] - mean[i] * mean[i], 0.f));
		}
		if (context.cancelled()) {
			return false;
		}
	}
	auto blurred = levels;
	BlurPlaneAny(blurred, width, height, radius);
	if (transparent) {
		// Transparent pixels must not darken the blur around them.
		auto coverage = std::vector<float>(levels.size());
		FxParallelRows(width, height, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto line = FxRow(image, y);
				const auto target = coverage.data() + size_t(y) * width;
				for (auto x = 0; x != width; ++x) {
					target[x] = (line[x] >> 24) ? 1.f : 0.f;
				}
			}
		});
		BlurPlaneAny(coverage, width, height, radius);
		for (auto i = size_t(0); i != blurred.size(); ++i) {
			blurred[i] /= std::max(coverage[i], 0.05f);
		}
	}
	if (context.cancelled()) {
		return false;
	}
	const auto gain = 1.5f * amount;
	const auto from = 0.004f + 0.035f * masking;
	const auto till = from * 2.f + 0.01f;
	ForEachColorRows(image, context, [] {
		return 0;
	}, [](int&, int) {
	}, [&](FxRgba &color, int, int x, int y) {
		const auto index = size_t(y) * width + x;
		const auto value = levels[index];
		auto change = gain * (value - blurred[index]);
		if (!edges.empty()) {
			change *= FxSmoothStep(from, till, edges[index]);
		}
		change = std::clamp(change, -0.2f, 0.2f);
		auto r = Decode(gamma, color.r);
		auto g = Decode(gamma, color.g);
		auto b = Decode(gamma, color.b);
		const auto luma = Luma(r, g, b);
		Relight(r, g, b, luma, Decode(gamma, FxClamp01(value + change)));
		color.r = Encode(gamma, r);
		color.g = Encode(gamma, g);
		color.b = Encode(gamma, b);
	});
	return !context.cancelled();
}

//
// adjust.denoise
//

// Luma and two color differences of the encoded channels: the noise
// of a camera is the most even there, and the color noise can be
// smoothed much stronger than the luminance one. The luma is filtered
// with a self-guided filter, the color with filters guided by the luma
// (so the colors don't bleed over the edges).
[[nodiscard]] bool ApplyDenoise(
		QImage &image,
		float luminance, // 0..1.
		float chroma, // 0..1.
		const FxContext &context) {
	const auto pixels = qint64(image.width()) * image.height();
	const auto opponent = [](const FxRgba &color, float *result) {
		result[0] = Luma(color.r, color.g, color.b);
		result[1] = color.b - result[0];
		result[2] = color.r - result[0];
	};
	// A large picture is filtered on planes of a half of its size. The
	// blur must stay wider than a pixel there: in a downscaled preview of
	// a big photo the sigma is small, a blur that does nothing would turn
	// the filter into a plain blend with the reduced picture. Such
	// a preview is filtered at its full size, the largest renders get
	// a wider blur instead (the planes would take too much memory).
	const auto planeFor = [&](double sigma) {
		const auto low = LowForSigma(image.size(), sigma);
		const auto half = (low.factor < 2)
			&& (pixels > kDenoiseHalfPixels)
			&& (sigma >= kDenoiseHalfSigma || pixels > kDenoiseFullPixels);
		return half ? LowForFactor(image.size(), 2) : low;
	};
	const auto blurFor = [](double sigma, LowSize low) {
		return std::max(sigma, 0.5 * low.factor);
	};
	struct Field {
		Guided guided;
		Columns columns;
	};
	auto lumaField = Field();
	auto blueField = Field();
	auto redField = Field();
	if (luminance > 0.f) {
		const auto sigma = context.px(1.2 + 2. * luminance);
		const auto low = planeFor(sigma);
		const auto planes = BuildLow<1>(image, low, [](
				const FxRgba &color,
				float *result) {
			result[0] = Luma(color.r, color.g, color.b);
		});
		if (context.cancelled()) {
			return false;
		}
		const auto deviation = 0.008f + 0.06f * luminance;
		lumaField = {
			MakeGuided(
				low,
				planes[0],
				nullptr,
				blurFor(sigma, low),
				deviation * deviation),
			MakeColumns(image.width(), low),
		};
		if (context.cancelled()) {
			return false;
		}
	}
	if (chroma > 0.f) {
		const auto sigma = context.px(2. + 6. * chroma);
		const auto low = planeFor(sigma);
		const auto planes = BuildLow<3>(image, low, opponent);
		if (context.cancelled()) {
			return false;
		}
		const auto use = blurFor(sigma, low);
		blueField = {
			MakeGuided(low, planes[0], &planes[1], use, 0.0004f),
			MakeColumns(image.width(), low),
		};
		redField = {
			MakeGuided(low, planes[0], &planes[2], use, 0.0004f),
			MakeColumns(image.width(), low),
		};
		if (context.cancelled()) {
			return false;
		}
	}
	const auto lumaMix = std::min(luminance * 3.f, 1.f);
	const auto chromaMix = std::min(chroma * 2.5f, 1.f);
	struct Rows {
		GuidedRows luma;
		GuidedRows blue;
		GuidedRows red;
	};
	ForEachColorRows(image, context, [&] {
		return Rows{
			(luminance > 0.f) ? GuidedRows(lumaField.guided) : GuidedRows(),
			(chroma > 0.f) ? GuidedRows(blueField.guided) : GuidedRows(),
			(chroma > 0.f) ? GuidedRows(redField.guided) : GuidedRows(),
		};
	}, [&](Rows &rows, int y) {
		if (luminance > 0.f) {
			lumaField.guided.row(y, rows.luma.a, rows.luma.b);
		}
		if (chroma > 0.f) {
			blueField.guided.row(y, rows.blue.a, rows.blue.b);
			redField.guided.row(y, rows.red.a, rows.red.b);
		}
	}, [&](FxRgba &color, const Rows &rows, int x, int) {
		const auto luma = Luma(color.r, color.g, color.b);
		auto blue = color.b - luma;
		auto red = color.r - luma;
		auto value = luma;
		if (chroma > 0.f) {
			const auto smoothBlue = RowValue(rows.blue.a, blueField.columns, x)
				* luma
				+ RowValue(rows.blue.b, blueField.columns, x);
			const auto smoothRed = RowValue(rows.red.a, redField.columns, x)
				* luma
				+ RowValue(rows.red.b, redField.columns, x);
			blue += (smoothBlue - blue) * chromaMix;
			red += (smoothRed - red) * chromaMix;
		}
		if (luminance > 0.f) {
			const auto smooth = RowValue(rows.luma.a, lumaField.columns, x)
				* luma
				+ RowValue(rows.luma.b, lumaField.columns, x);
			value += (smooth - value) * lumaMix;
		}
		color.r = red + value;
		color.b = blue + value;
		color.g = (value - kLumaR * color.r - kLumaB * color.b) / kLumaG;
	});
	return !context.cancelled();
}

//
// adjust.vignette
//

struct VignetteValues {
	float amount = 0.f; // -1..1, negative darkens.
	float midpoint = 0.5f; // 0..1.
	float roundness = 0.f; // -1..1.
	float feather = 0.5f; // 0..1.
};

// How much of the vignette a point gets, 0..1. u, v are -1..1 from the
// center to the edges of the layer.
struct VignetteShape {
	float scaleX = 1.f;
	float scaleY = 1.f;
	float power = 2.f;
	float normalize = 1.f;
	float from = 0.f;
	float till = 1.f;

	VignetteShape(const VignetteValues &values, QSize size) {
		const auto aspect = size.width() / float(std::max(size.height(), 1));
		if (values.roundness > 0.f) {
			// Towards a circle: the longer side is stretched.
			if (aspect > 1.f) {
				scaleX = 1.f + (aspect - 1.f) * values.roundness;
			} else {
				scaleY = 1.f + (1.f / aspect - 1.f) * values.roundness;
			}
		} else if (values.roundness < 0.f) {
			// Towards a rectangle: a superellipse.
			power = 2.f - 6.f * values.roundness;
		}
		// The corner of the frame is always at the square root of two.
		normalize = std::sqrt(2.f) / std::pow(2.f, 1.f / power);
		const auto middle = 0.4f + 1.2f * values.midpoint;
		const auto width = 0.05f + 0.9f * values.feather;
		from = middle - width;
		till = middle + width;
	}

	[[nodiscard]] float operator()(float u, float v) const {
		u = std::abs(u * scaleX);
		v = std::abs(v * scaleY);
		const auto distance = (power == 2.f)
			? std::sqrt(u * u + v * v)
			: (std::pow(std::pow(u, power) + std::pow(v, power), 1.f / power)
				* normalize);
		return FxSmoothStep(from, till, distance);
	}
};

[[nodiscard]] bool ApplyVignette(
		QImage &image,
		const VignetteValues &values,
		const FxContext &context) {
	const auto &gamma = Gamma();
	const auto shape = VignetteShape(values, image.size());
	const auto stepX = 2.f / image.width();
	const auto stepY = 2.f / image.height();
	const auto darken = (values.amount < 0.f);
	const auto strength = std::abs(values.amount);
	FxForEachColor(image, [&](FxRgba &color, int x, int y) {
		const auto part = strength * shape(
			(x + 0.5f) * stepX - 1.f,
			(y + 0.5f) * stepY - 1.f);
		if (part <= 0.f) {
			return;
		} else if (darken) {
			// Less light, like a real lens does it.
			const auto gain = (1.f - part) * (1.f - part);
			color.r = Encode(gamma, Decode(gamma, color.r) * gain);
			color.g = Encode(gamma, Decode(gamma, color.g) * gain);
			color.b = Encode(gamma, Decode(gamma, color.b) * gain);
		} else {
			color.r += (1.f - color.r) * part;
			color.g += (1.f - color.g) * part;
			color.b += (1.f - color.b) * part;
		}
	});
	return !context.cancelled();
}

//
// adjust.grain
//

// Smooth noise: random values at the nodes of a unit lattice blended
// with the cubic B-spline. The blobs are round and the variance is the
// same everywhere, so no grid shows through (as it does with a bilinear
// blend). The deviation is about 0.2.
[[nodiscard]] inline float SplineNoise(float u, float v, uint32 seed) {
	const auto floorU = std::floor(u);
	const auto floorV = std::floor(v);
	const auto iu = int(floorU);
	const auto iv = int(floorV);
	const auto weights = [](float t, float *result) {
		const auto t2 = t * t;
		const auto t3 = t2 * t;
		result[0] = (1.f - 3.f * t + 3.f * t2 - t3) * (1.f / 6.f);
		result[1] = (4.f - 6.f * t2 + 3.f * t3) * (1.f / 6.f);
		result[2] = (1.f + 3.f * t + 3.f * t2 - 3.f * t3) * (1.f / 6.f);
		result[3] = t3 * (1.f / 6.f);
	};
	float wu[4];
	float wv[4];
	weights(u - floorU, wu);
	weights(v - floorV, wv);
	auto result = 0.f;
	for (auto j = 0; j != 4; ++j) {
		auto row = 0.f;
		for (auto i = 0; i != 4; ++i) {
			row += wu[i] * FxNoise(
				uint32(iu + i - 1),
				uint32(iv + j - 1),
				seed);
		}
		result += wv[j] * row;
	}
	return result;
}

// Film grain: luminance noise, the strongest in the midtones. The size
// of the grain is relative to the layer, the pattern is tied to the
// layer too (so the preview shows the grain of the export).
[[nodiscard]] bool ApplyGrain(
		QImage &image,
		float amount, // 0..1.
		float size, // 0..1.
		float roughness, // 0..1.
		uint32 seed,
		const FxContext &context) {
	const auto &gamma = Gamma();
	constexpr auto kUnit = 5.f; // SplineNoise() -> a deviation of one.
	const auto scale = (context.scale > 0.) ? context.scale : 1.;
	const auto side = context.fullSize.isEmpty()
		? (std::min(image.width(), image.height()) / scale)
		: double(std::min(
			context.fullSize.width(),
			context.fullSize.height()));
	// The size of a grain in source pixels, then in rendered ones.
	const auto cell = float(
		std::max(side * (0.0007 + 0.0045 * size), 1.) * scale);
	// A grain is about two nodes of the lattice wide.
	const auto inverse = 1.f / (0.6f * cell);
	// Grain finer than a rendered pixel averages out.
	const auto strength = 0.07f * amount * std::clamp(cell, 0.35f, 1.f);
	const auto plain = (cell <= 1.2f);
	const auto seedFine = FxHash32(seed, 0x51U, context.seed);
	const auto seedCoarse = FxHash32(seed, 0x52U, context.seed);
	const auto seedWave = FxHash32(seed, 0x53U, context.seed);
	FxForEachColor(image, [&](FxRgba &color, int x, int y) {
		const auto u = (x + 0.5f) * inverse;
		const auto v = (y + 0.5f) * inverse;
		auto noise = plain
			? (FxNoise(uint32(x), uint32(y), seedFine) * 2.45f)
			: (SplineNoise(u, v, seedFine) * kUnit);
		if (roughness > 0.f) {
			// Uneven grain: larger grains among the usual ones, and the
			// strength itself changes from place to place.
			const auto coarse = SplineNoise(
				u * 0.4f + 11.3f,
				v * 0.4f + 4.1f,
				seedCoarse) * kUnit;
			const auto wave = SplineNoise(
				u * 0.15f + 9.1f,
				v * 0.15f + 5.9f,
				seedWave) * kUnit;
			noise = noise
				* (1.f + 0.3f * roughness * std::clamp(wave, -2.f, 2.f))
				+ 0.7f * roughness * coarse;
		}
		auto r = Decode(gamma, color.r);
		auto g = Decode(gamma, color.g);
		auto b = Decode(gamma, color.b);
		const auto luma = Luma(r, g, b);
		const auto value = Encode(gamma, luma);
		const auto weight = 0.3f + 0.7f * 4.f * value * (1.f - value);
		const auto target = Decode(
			gamma,
			FxClamp01(value + strength * weight * noise));
		Relight(r, g, b, luma, target);
		color.r = Encode(gamma, r);
		color.g = Encode(gamma, g);
		color.b = Encode(gamma, b);
	});
	return !context.cancelled();
}

//
// Serialization of the custom parameters.
//

[[nodiscard]] std::optional<std::vector<int>> ParseNumbers(
		const QByteArray &text,
		char separator) {
	auto result = std::vector<int>();
	for (const auto &part : text.split(separator)) {
		auto ok = false;
		const auto value = part.trimmed().toInt(&ok);
		if (!ok) {
			return std::nullopt;
		}
		result.push_back(value);
	}
	return result;
}

// "k=value/k=value": the value of the key, a null array if it is not there.
[[nodiscard]] QByteArray FindRow(const QByteArray &data, char key) {
	if (data.size() > 4096) {
		return QByteArray();
	}
	for (const auto &row : data.split('/')) {
		if (row.size() > 2 && row[0] == key && row[1] == '=') {
			return row.mid(2);
		}
	}
	return QByteArray();
}

//
// Registration.
//

[[nodiscard]] FxParam Percent(
		QByteArray id,
		FxText name,
		int value = 0,
		int from = -100) {
	return FxInt(std::move(id), std::move(name), from, 100, value);
}

const auto Registered = FxRegistrar([] {
	RegisterFx({
		.id = "adjust.light",
		.group = FxGroup::Light,
		.name = tr::lng_oblivion_photo_adj_light,
		.params = {
			FxFloat(
				"exposure",
				tr::lng_oblivion_photo_adj_exposure,
				-5.,
				5.,
				0.,
				2).stepped(0.05),
			Percent("contrast", tr::lng_oblivion_photo_adj_contrast),
			Percent("highlights", tr::lng_oblivion_photo_adj_highlights),
			Percent("shadows", tr::lng_oblivion_photo_adj_shadows),
			Percent("whites", tr::lng_oblivion_photo_adj_whites),
			Percent("blacks", tr::lng_oblivion_photo_adj_blacks),
		},
		.flags = kFxNeighbours,
		.order = 0,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			return ApplyLight(image, ReadLight(params), context);
		},
		.identity = [](const FxParams &params) {
			return ReadLight(params).neutral();
		},
	});
	RegisterFx({
		.id = "adjust.curve",
		.group = FxGroup::Light,
		.name = tr::lng_oblivion_photo_adj_curve,
		.params = {
			FxCustom(
				"curve",
				tr::lng_oblivion_photo_adj_curve,
				kAdjustCurveType),
		},
		.order = 1,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto curve = ParseToneCurve(params.data("curve"));
			return curve.identity()
				? !context.cancelled()
				: ApplyCurve(image, curve, context);
		},
		.identity = [](const FxParams &params) {
			return ParseToneCurve(params.data("curve")).identity();
		},
	});

	const auto readColor = [](const FxParams &params) {
		return ColorValues{
			.temperature = float(params.number("temperature") / 100.),
			.tint = float(params.number("tint") / 100.),
			.vibrance = float(params.number("vibrance") / 100.),
			.saturation = float(params.number("saturation") / 100.),
		};
	};
	RegisterFx({
		.id = "adjust.color",
		.group = FxGroup::Color,
		.name = tr::lng_oblivion_photo_adj_color,
		.params = {
			Percent(
				"temperature",
				tr::lng_oblivion_photo_adj_temperature
			).styled(FxSliderLook::Temperature),
			Percent(
				"tint",
				tr::lng_oblivion_photo_adj_tint
			).styled(FxSliderLook::Tint),
			Percent("vibrance", tr::lng_oblivion_photo_adj_vibrance),
			Percent("saturation", tr::lng_oblivion_photo_adj_saturation),
		},
		.order = 0,
		.apply = [=](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			return ApplyColor(image, readColor(params), context);
		},
		.identity = [=](const FxParams &params) {
			return readColor(params).neutral();
		},
	});
	RegisterFx({
		.id = "adjust.hsl",
		.group = FxGroup::Color,
		.name = tr::lng_oblivion_photo_adj_hsl,
		.params = {
			FxCustom(
				"table",
				tr::lng_oblivion_photo_adj_hsl,
				kAdjustHslType),
		},
		.order = 1,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto table = ParseHslTable(params.data("table"));
			return table.identity()
				? !context.cancelled()
				: ApplyHsl(image, table, context);
		},
		.identity = [](const FxParams &params) {
			return ParseHslTable(params.data("table")).identity();
		},
	});

	const auto readGrading = [](const FxParams &params) {
		return GradingValues{
			.wheels = ParseGradeWheels(params.data("wheels")),
			.luminance = { {
				float(params.number("shadows_lum") / 100.),
				float(params.number("midtones_lum") / 100.),
				float(params.number("highlights_lum") / 100.),
			} },
			.blending = float(params.number("blending") / 100.),
			.balance = float(params.number("balance") / 100.),
		};
	};
	RegisterFx({
		.id = "adjust.grading",
		.group = FxGroup::Color,
		.name = tr::lng_oblivion_photo_adj_grading,
		.params = {
			FxCustom(
				"wheels",
				tr::lng_oblivion_photo_adj_grading,
				kAdjustWheelsType),
			Percent("shadows_lum", tr::lng_oblivion_photo_adj_shadows_lum),
			Percent("midtones_lum", tr::lng_oblivion_photo_adj_midtones_lum),
			Percent(
				"highlights_lum",
				tr::lng_oblivion_photo_adj_highlights_lum),
			Percent("blending", tr::lng_oblivion_photo_adj_blending, 50, 0),
			Percent("balance", tr::lng_oblivion_photo_adj_balance),
		},
		.order = 2,
		.apply = [=](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			return ApplyGrading(image, readGrading(params), context);
		},
		.identity = [=](const FxParams &params) {
			return readGrading(params).neutral();
		},
	});

	const auto mixNames = std::array<tr::phrase<>, kHslRanges>{ {
		tr::lng_oblivion_photo_adj_red,
		tr::lng_oblivion_photo_adj_orange,
		tr::lng_oblivion_photo_adj_yellow,
		tr::lng_oblivion_photo_adj_green,
		tr::lng_oblivion_photo_adj_aqua,
		tr::lng_oblivion_photo_adj_blue,
		tr::lng_oblivion_photo_adj_purple,
		tr::lng_oblivion_photo_adj_magenta,
	} };
	static constexpr auto kMixIds = std::array<const char*, kHslRanges>{ {
		"red",
		"orange",
		"yellow",
		"green",
		"aqua",
		"blue",
		"purple",
		"magenta",
	} };
	auto mixParams = std::vector<FxParam>();
	for (auto i = 0; i != kHslRanges; ++i) {
		mixParams.push_back(Percent(kMixIds[i], mixNames[i]));
	}
	RegisterFx({
		.id = "adjust.bw",
		.group = FxGroup::Color,
		.name = tr::lng_oblivion_photo_adj_bw,
		.params = std::move(mixParams),
		.order = 3,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			auto mix = std::array<float, kHslRanges>();
			for (auto i = 0; i != kHslRanges; ++i) {
				mix[i] = float(params.number(kMixIds[i]) / 100.);
			}
			return ApplyBlackWhite(image, mix, context);
		},
	});

	const auto readPresence = [](const FxParams &params) {
		return PresenceValues{
			.texture = float(params.number("texture") / 100.),
			.clarity = float(params.number("clarity") / 100.),
			.dehaze = float(params.number("dehaze") / 100.),
		};
	};
	RegisterFx({
		.id = "adjust.presence",
		.group = FxGroup::Detail,
		.name = tr::lng_oblivion_photo_adj_presence,
		.params = {
			Percent("texture", tr::lng_oblivion_photo_adj_texture),
			Percent("clarity", tr::lng_oblivion_photo_adj_clarity),
			Percent("dehaze", tr::lng_oblivion_photo_adj_dehaze),
		},
		.flags = kFxNeighbours,
		.order = 0,
		.apply = [=](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			return ApplyPresence(image, readPresence(params), context);
		},
		.identity = [=](const FxParams &params) {
			return readPresence(params).neutral();
		},
	});
	RegisterFx({
		.id = "adjust.sharpen",
		.group = FxGroup::Detail,
		.name = tr::lng_oblivion_photo_adj_sharpen,
		.params = {
			FxInt("amount", tr::lng_oblivion_photo_adj_amount, 0, 150, 40),
			FxPixels(
				"radius",
				tr::lng_oblivion_photo_adj_radius,
				0.5,
				3.,
				1.,
				1),
			FxInt("masking", tr::lng_oblivion_photo_adj_masking, 0, 100, 0),
		},
		.flags = kFxNeighbours,
		.order = 1,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			return ApplySharpen(
				image,
				float(params.number("amount") / 100.),
				context.px(params.number("radius")),
				float(params.number("masking") / 100.),
				context);
		},
		.identity = [](const FxParams &params) {
			return params.number("amount") <= 0.;
		},
	});
	RegisterFx({
		.id = "adjust.denoise",
		.group = FxGroup::Detail,
		.name = tr::lng_oblivion_photo_adj_denoise,
		.params = {
			FxInt(
				"luminance",
				tr::lng_oblivion_photo_adj_noise_luminance,
				0,
				100,
				30),
			FxInt(
				"color",
				tr::lng_oblivion_photo_adj_noise_color,
				0,
				100,
				25),
		},
		.flags = kFxNeighbours,
		.order = 2,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			return ApplyDenoise(
				image,
				float(params.number("luminance") / 100.),
				float(params.number("color") / 100.),
				context);
		},
		.identity = [](const FxParams &params) {
			return (params.number("luminance") <= 0.)
				&& (params.number("color") <= 0.);
		},
	});

	RegisterFx({
		.id = "adjust.vignette",
		.group = FxGroup::Finish,
		.name = tr::lng_oblivion_photo_adj_vignette,
		.params = {
			// "Edge brightness", not "Amount": the sign of the value
			// explains itself then (negative darkens the edges).
			Percent(
				"amount",
				tr::lng_oblivion_photo_adj_vignette_amount,
				-30),
			Percent("midpoint", tr::lng_oblivion_photo_adj_midpoint, 50, 0),
			Percent("roundness", tr::lng_oblivion_photo_adj_roundness),
			Percent("feather", tr::lng_oblivion_photo_adj_feather, 50, 0),
		},
		.order = 0,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			return ApplyVignette(image, VignetteValues{
				.amount = float(params.number("amount") / 100.),
				.midpoint = float(params.number("midpoint") / 100.),
				.roundness = float(params.number("roundness") / 100.),
				.feather = float(params.number("feather") / 100.),
			}, context);
		},
		.identity = [](const FxParams &params) {
			return params.number("amount") == 0.;
		},
	});
	RegisterFx({
		.id = "adjust.grain",
		.group = FxGroup::Finish,
		.name = tr::lng_oblivion_photo_adj_grain,
		.params = {
			Percent("amount", tr::lng_oblivion_photo_adj_amount, 25, 0),
			Percent("size", tr::lng_oblivion_photo_adj_size, 25, 0),
			Percent("roughness", tr::lng_oblivion_photo_adj_roughness, 50, 0),
			FxSeed(),
		},
		.flags = kFxSeeded,
		.order = 1,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			return ApplyGrain(
				image,
				float(params.number("amount") / 100.),
				float(params.number("size") / 100.),
				float(params.number("roughness") / 100.),
				uint32(params.integer("seed")),
				context);
		},
		.identity = [](const FxParams &params) {
			return params.number("amount") <= 0.;
		},
	});
});

//
// Self-test.
//

struct Stats {
	double r = 0.; // Means of the straight channels, 0..255.
	double g = 0.;
	double b = 0.;
	double luma = 0.; // The mean of the Rec. 601 luma, 0..255.
	double deviation = 0.; // Of the luma.
	double chroma = 0.; // The mean of max - min of the channels.
	double detail = 0.; // The mean luma difference of neighbour pixels.
	std::array<int, 256> histogram = {};
	int count = 0;

	[[nodiscard]] int percentile(double part) const {
		auto left = int(std::lround(count * part));
		for (auto i = 0; i != 256; ++i) {
			left -= histogram[i];
			if (left <= 0) {
				return i;
			}
		}
		return 255;
	}
};

[[nodiscard]] Stats Measure(const QImage &image, QRect rect = QRect()) {
	auto result = Stats();
	if (rect.isEmpty()) {
		rect = image.rect();
	}
	rect = rect.intersected(image.rect());
	auto squares = 0.;
	auto differences = 0.;
	auto pairs = 0;
	const auto level = [](uint32 pixel) {
		const auto color = FxUnpack(pixel);
		return 255.f * FxLuma(color.r, color.g, color.b);
	};
	for (auto y = rect.top(); y <= rect.bottom(); ++y) {
		const auto line = FxRow(image, y);
		for (auto x = rect.left(); x <= rect.right(); ++x) {
			if (!(line[x] >> 24)) {
				continue;
			}
			const auto color = FxUnpack(line[x]);
			const auto value = level(line[x]);
			result.r += 255. * color.r;
			result.g += 255. * color.g;
			result.b += 255. * color.b;
			result.luma += value;
			squares += double(value) * value;
			result.chroma += 255. * (std::max({ color.r, color.g, color.b })
				- std::min({ color.r, color.g, color.b }));
			++result.histogram[std::clamp(int(value + 0.5f), 0, 255)];
			++result.count;
			if (x < rect.right() && (line[x + 1] >> 24)) {
				differences += std::abs(level(line[x + 1]) - value);
				++pairs;
			}
			if (y < rect.bottom()) {
				const auto below = FxRow(image, y + 1)[x];
				if (below >> 24) {
					differences += std::abs(level(below) - value);
					++pairs;
				}
			}
		}
	}
	if (result.count) {
		result.r /= result.count;
		result.g /= result.count;
		result.b /= result.count;
		result.luma /= result.count;
		result.chroma /= result.count;
		result.deviation = std::sqrt(std::max(
			squares / result.count - result.luma * result.luma,
			0.));
	}
	if (pairs) {
		result.detail = differences / pairs;
	}
	return result;
}

[[nodiscard]] int MaxDifference(const QImage &a, const QImage &b) {
	if (a.size() != b.size()) {
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

[[nodiscard]] bool SameAlpha(const QImage &a, const QImage &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto y = 0; y != a.height(); ++y) {
		const auto one = FxRow(a, y);
		const auto two = FxRow(b, y);
		for (auto x = 0; x != a.width(); ++x) {
			const auto alpha = (two[x] >> 24);
			if ((one[x] >> 24) != alpha
				|| ((two[x] >> 16) & 0xFFU) > alpha
				|| ((two[x] >> 8) & 0xFFU) > alpha
				|| (two[x] & 0xFFU) > alpha) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] QImage SolidImage(int width, int height, QColor color) {
	auto result = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	result.fill(color);
	return result;
}

// Vertical stripes of the given colors, each of the same width.
[[nodiscard]] QImage PatchImage(
		const std::vector<QColor> &colors,
		int patch,
		int height) {
	auto result = QImage(
		patch * int(colors.size()),
		height,
		QImage::Format_ARGB32_Premultiplied);
	for (auto y = 0; y != result.height(); ++y) {
		const auto line = FxRow(result, y);
		for (auto x = 0; x != result.width(); ++x) {
			line[x] = colors[x / patch].rgb() | 0xFF000000U;
		}
	}
	return result;
}

[[nodiscard]] QRect PatchRect(int index, int patch, int height) {
	return QRect(index * patch + 2, 2, patch - 4, height - 4);
}

// A gray picture: the left half is darker than the right one, with
// noise of the given strength (0..1 of the full range) in every channel.
[[nodiscard]] QImage NoisyStep(int width, int height, float noise) {
	auto result = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	for (auto y = 0; y != height; ++y) {
		const auto line = FxRow(result, y);
		for (auto x = 0; x != width; ++x) {
			const auto base = (x < width / 2) ? 0.35f : 0.65f;
			line[x] = FxPack({
				base + noise * FxNoise(uint32(x), uint32(y), 11U),
				base + noise * FxNoise(uint32(x), uint32(y), 12U),
				base + noise * FxNoise(uint32(x), uint32(y), 13U),
				1.f,
			});
		}
	}
	return result;
}

// A gray picture of random square cells: fine detail with hard edges.
[[nodiscard]] QImage CellTexture(int width, int height, int cell) {
	auto result = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	FxParallelRows(width, height, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(result, y);
			for (auto x = 0; x != width; ++x) {
				const auto value = 0.5f + 0.3f * FxNoise(
					uint32(x / cell),
					uint32(y / cell),
					21U);
				line[x] = FxPack({ value, value, value, 1.f });
			}
		}
	});
	return result;
}

[[nodiscard]] FxInstance Instance(
		const QByteArray &id,
		const std::vector<FxParams::Entry> &values = {}) {
	auto result = MakeFx(id);
	const auto descriptor = FindFx(id);
	for (const auto &entry : values) {
		const auto param = descriptor ? descriptor->param(entry.id) : nullptr;
		if (param) {
			result.params.set(entry.id, param->normalized(entry.value));
		}
	}
	return result;
}

[[nodiscard]] QImage Applied(
		const QImage &source,
		const FxInstance &instance,
		double scale = 1.,
		QSize fullSize = QSize()) {
	auto result = source;
	const auto context = FxContext{
		.scale = scale,
		.seed = 77,
		.fullSize = fullSize.isEmpty() ? source.size() : fullSize,
		.preview = (scale < 1.),
	};
	return ApplyFx(result, instance, context) ? result : QImage();
}

[[nodiscard]] FxParams::Entry Value(const char *id, double value) {
	return { QByteArray(id), FxValue::Number(value) };
}

[[nodiscard]] FxParams::Entry Bytes(const char *id, const QByteArray &value) {
	return { QByteArray(id), FxValue::Data(value) };
}

bool RunAdjustSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto info = [&](const QString &what) {
		log.push_back(u"   "_q + what);
	};
	const auto number = [](double value, int digits = 2) {
		return QString::number(value, 'f', digits);
	};

	// The models of the custom parameters.
	{
		auto curve = ToneCurve();
		check(
			curve.identity()
				&& SerializeToneCurve(curve).isEmpty()
				&& ParseToneCurve(QByteArray()) == curve,
			u"the default curve is the identity and an empty value"_q);
		curve.channels[0] = { { 0, 0 }, { 250, 180 }, { 1000, 1000 } };
		curve.channels[3] = { { 0, 60 }, { 1000, 900 } };
		const auto bytes = SerializeToneCurve(curve);
		check(
			bytes == "m=0,0;250,180;1000,1000/b=0,60;1000,900"
				&& ParseToneCurve(bytes) == curve
				&& NormalizeToneCurve(bytes) == bytes
				&& !curve.identity()
				&& curve.identity(1)
				&& !curve.identity(3),
			u"the curve round trip is canonical"_q);
		const auto messy = ParseToneCurve(
			"m=900,2000;0,-5;904,10;400,400/r=1,2/g=oops/zz=1,1;2,2");
		check(
			messy.channels[0]
					== CurvePoints{ { 0, 0 }, { 400, 400 }, { 900, 1000 } }
				&& messy.channels[1] == DefaultCurvePoints()
				&& messy.channels[2] == DefaultCurvePoints(),
			u"a broken curve is repaired"_q);
		check(
			NormalizeToneCurve("garbage").isEmpty()
				&& NormalizeToneCurve(QByteArray(100000, 'x')).isEmpty()
				&& NormalizeToneCurve("m=0,0;1000,1000").isEmpty(),
			u"garbage and the default curve normalize to nothing"_q);
		auto many = CurvePoints();
		for (auto i = 0; i != 40; ++i) {
			many.push_back({ i * 25, i * 20 });
		}
		check(
			NormalizedCurvePoints(many).size() == size_t(kCurveMaxPoints),
			u"a curve has a limit of points"_q);

		auto table = HslTable();
		check(
			table.identity() && SerializeHslTable(table).isEmpty(),
			u"the default color table is neutral and an empty value"_q);
		table.hue[2] = 20;
		table.saturation[5] = -40;
		table.luminance[0] = 300;
		const auto tableBytes = SerializeHslTable(table);
		auto clamped = table;
		clamped.luminance[0] = 100;
		check(
			ParseHslTable(tableBytes) == clamped
				&& NormalizeHslTable(tableBytes)
					== "h=0,0,20,0,0,0,0,0/s=0,0,0,0,0,-40,0,0"
						"/l=100,0,0,0,0,0,0,0"
				&& NormalizeHslTable("h=1,2,3").isEmpty()
				&& NormalizeHslTable("s=a,b,c,d,e,f,g,h").isEmpty(),
			u"the color table round trip clamps and is canonical"_q);

		auto wheels = GradeWheels();
		check(
			wheels.identity() && SerializeGradeWheels(wheels).isEmpty(),
			u"no tint is an empty value"_q);
		wheels.ranges[0] = { 210, 35 };
		wheels.ranges[2] = { 40, 20 };
		wheels.ranges[1] = { 123, 0 };
		const auto wheelBytes = SerializeGradeWheels(wheels);
		check(
			wheelBytes == "s=210,35/h=40,20"
				&& ParseGradeWheels(wheelBytes).ranges[1] == GradeWheel()
				&& NormalizeGradeWheels("s=-30,500") == "s=330,100"
				&& NormalizeGradeWheels("s=10").isEmpty(),
			u"the wheels round trip is canonical"_q);
	}

	// The curve interpolation.
	{
		const auto points = CurvePoints{
			{ 0, 0 },
			{ 200, 120 },
			{ 500, 520 },
			{ 800, 900 },
			{ 1000, 1000 },
		};
		auto table = std::vector<float>(kCurveTable);
		FillCurveTable(points, table.data(), kCurveTable);
		auto monotone = true;
		for (auto i = 1; i != kCurveTable; ++i) {
			monotone = monotone && (table[i] >= table[i - 1]);
		}
		auto through = true;
		for (const auto &point : points) {
			through = through && (std::abs(
				CurveValue(points, point.x / 1000.) - point.y / 1000.) < 1e-9);
		}
		check(
			monotone && through,
			u"the curve is monotone and goes through its points"_q);

		// Points that go up and down: no overshoot between neighbours.
		const auto wavy = CurvePoints{
			{ 0, 500 },
			{ 300, 900 },
			{ 320, 100 },
			{ 700, 100 },
			{ 1000, 800 },
		};
		auto inside = true;
		for (auto i = 0; i <= 1000; ++i) {
			const auto x = i / 1000.;
			const auto value = CurveValue(wavy, x);
			auto from = 1.;
			auto till = 0.;
			for (auto k = 0; k + 1 != int(wavy.size()); ++k) {
				if (i >= wavy[k].x && i <= wavy[k + 1].x) {
					const auto one = wavy[k].y / 1000.;
					const auto two = wavy[k + 1].y / 1000.;
					from = std::min({ from, one, two });
					till = std::max({ till, one, two });
				}
			}
			inside = inside && (value >= from - 1e-9) && (value <= till + 1e-9);
		}
		check(inside, u"the curve never overshoots its points"_q);

		const auto levels = CurvePoints{ { 100, 0 }, { 900, 1000 } };
		check(
			CurveValue(levels, 0.05) == 0.
				&& CurveValue(levels, 0.95) == 1.
				&& std::abs(CurveValue(levels, 0.5) - 0.5) < 1e-9,
			u"the curve is flat outside of its end points"_q);

		auto identity = std::vector<float>(kCurveTable);
		FillCurveTable(DefaultCurvePoints(), identity.data(), kCurveTable);
		auto exact = true;
		for (auto i = 0; i != 256; ++i) {
			exact = exact
				&& (int(identity[i * 4] * 255.f + 0.5f) == i);
		}
		check(exact, u"the default curve maps every level to itself"_q);
	}

	// The tone functions.
	{
		auto monotone = true;
		auto anchored = true;
		for (const auto contrast : { -1.f, -0.5f, 0.f, 0.5f, 1.f }) {
			for (const auto whites : { -1.f, 0.f, 1.f }) {
				for (const auto blacks : { -1.f, 0.f, 1.f }) {
					const auto values = LightValues{
						.contrast = contrast,
						.whites = whites,
						.blacks = blacks,
					};
					auto last = -1.;
					for (auto i = 0; i <= 1000; ++i) {
						const auto value = GlobalTone(i / 1000., values);
						monotone = monotone && (value >= last - 1e-12);
						last = value;
					}
					if (whites == 0.f && blacks == 0.f) {
						const auto close = [&](double x, double expected) {
							return std::abs(GlobalTone(x, values) - expected)
								< 1e-12;
						};
						anchored = anchored
							&& close(0., 0.)
							&& close(0.5, 0.5)
							&& close(1., 1.);
					}
				}
			}
		}
		check(monotone, u"the tone curve is monotone for every setting"_q);
		check(anchored, u"contrast keeps black, white and the middle"_q);

		auto local = true;
		for (const auto shadows : { -1.f, 1.f }) {
			for (const auto highlights : { -1.f, 1.f }) {
				const auto values = LightValues{
					.highlights = highlights,
					.shadows = shadows,
				};
				auto last = -1.;
				for (auto i = 0; i <= 1000; ++i) {
					const auto x = i / 1000.;
					const auto value = x + LocalToneDelta(x, values);
					local = local && (value >= last - 1e-12);
					last = value;
				}
				local = local
					&& (LocalToneDelta(0., values) == 0.)
					&& (std::abs(LocalToneDelta(1., values)) < 1e-12);
			}
		}
		check(local, u"highlights and shadows are monotone, the ends stay"_q);
	}

	// Colors.
	{
		auto worst = 0.f;
		for (auto r = 0; r <= 255; r += 15) {
			for (auto g = 0; g <= 255; g += 15) {
				for (auto b = 0; b <= 255; b += 15) {
					const auto lr = float(FxSrgbToLinear(r / 255.));
					const auto lg = float(FxSrgbToLinear(g / 255.));
					const auto lb = float(FxSrgbToLinear(b / 255.));
					float br, bg, bb;
					FromLab(ToLab(lr, lg, lb), br, bg, bb);
					worst = std::max({
						worst,
						std::abs(br - lr),
						std::abs(bg - lg),
						std::abs(bb - lb),
					});
				}
			}
		}
		check(
			worst < 2e-4f,
			u"Oklab round trip (max error %1)"_q.arg(number(worst, 6)));

		auto unity = true;
		for (auto i = 0; i != 720; ++i) {
			const auto weights = RangeWeights(i * 0.5f);
			unity = unity
				&& (weights.t >= 0.f)
				&& (weights.t <= 1.f)
				&& (weights.first != weights.second);
		}
		auto centered = true;
		for (auto i = 0; i != kHslRanges; ++i) {
			const auto color = HslRangeColor(i);
			const auto lab = ToLab(
				float(FxSrgbToLinear(color.redF())),
				float(FxSrgbToLinear(color.greenF())),
				float(FxSrgbToLinear(color.blueF())));
			const auto degrees = HuePosition(lab.a, lab.b) * (360.f / kHueSize);
			const auto weights = RangeWeights(degrees);
			const auto own = (weights.first == i)
				? (1.f - weights.t)
				: (weights.second == i)
				? weights.t
				: 0.f;
			centered = centered && (own > 0.75f);
		}
		check(unity, u"every hue belongs to two neighbour ranges"_q);
		check(centered, u"the interface colors are inside their ranges"_q);

		const auto warm = WhiteBalance(1.f, 0.f);
		const auto cold = WhiteBalance(-1.f, 0.f);
		const auto magenta = WhiteBalance(0.f, 1.f);
		const auto none = WhiteBalance(0.f, 0.f);
		check(
			std::abs(none.r - 1.f) < 1e-5f
				&& std::abs(none.g - 1.f) < 1e-5f
				&& std::abs(none.b - 1.f) < 1e-5f
				&& warm.r > 1.15f
				&& warm.b < 0.75f
				&& cold.r < 0.9f
				&& cold.b > 1.3f
				&& magenta.g < 0.95f
				&& magenta.r > 1.f
				&& std::abs(Luma(warm.r, warm.g, warm.b) - 1.f) < 1e-4f,
			u"white balance gains (warm %1 %2 %3, cold %4 %5 %6)"_q.arg(
				number(warm.r),
				number(warm.g),
				number(warm.b),
				number(cold.r),
				number(cold.g),
				number(cold.b)));
	}

	const auto photo = FxTestImage(256, 192);
	const auto cut = FxTestImage(256, 192, true);
	const auto base = Measure(photo);

	// Neutral values change nothing, even when the code runs.
	{
		struct Neutral {
			const char *id = nullptr;
			std::vector<FxParams::Entry> values;
		};
		const auto list = std::vector<Neutral>{
			{ "adjust.light", {} },
			{ "adjust.curve", {} },
			{ "adjust.color", {} },
			{ "adjust.hsl", {} },
			{ "adjust.grading", {} },
			{ "adjust.presence", {} },
			{ "adjust.sharpen", { Value("amount", 0.) } },
			{ "adjust.denoise", {
				Value("luminance", 0.),
				Value("color", 0.),
			} },
			{ "adjust.vignette", { Value("amount", 0.) } },
			{ "adjust.grain", { Value("amount", 0.) } },
		};
		for (const auto &entry : list) {
			const auto name = QString::fromLatin1(entry.id);
			const auto descriptor = FindFx(entry.id);
			if (!descriptor) {
				check(false, name + u" is registered"_q);
				continue;
			}
			const auto instance = Instance(entry.id, entry.values);
			auto direct = cut;
			const auto applied = FxPrepare(direct)
				&& descriptor->apply(
					direct,
					NormalizedFxParams(*descriptor, instance.params),
					FxContext{ .seed = 5, .fullSize = cut.size() });
			const auto difference = applied ? MaxDifference(direct, cut) : 255;
			check(
				FxIsIdentity(instance) && difference <= 1,
				name + u": neutral values are the identity (max "
					"difference %1)"_q.arg(difference));
		}

		// Nearly black saturated colors: a tone that does not change keeps
		// them as they are, and nothing jumps right next to the neutral
		// values.
		const auto deep = PatchImage({
			QColor(0, 0, 40),
			QColor(30, 0, 0),
			QColor(2, 4, 30),
			QColor(12, 0, 30),
			QColor(0, 0, 0),
		}, 12, 12);
		auto still = 0;
		for (const auto id : { "adjust.sharpen", "adjust.grain" }) {
			const auto descriptor = FindFx(id);
			auto direct = deep;
			const auto applied = descriptor
				&& FxPrepare(direct)
				&& descriptor->apply(
					direct,
					NormalizedFxParams(
						*descriptor,
						Instance(id, { Value("amount", 0.) }).params),
					FxContext{ .seed = 5, .fullSize = deep.size() });
			still = std::max(
				still,
				applied ? MaxDifference(direct, deep) : 255);
		}
		auto close = 0;
		for (const auto &instance : {
			Instance("adjust.light", { Value("contrast", 1.) }),
			Instance("adjust.light", { Value("shadows", 1.) }),
			Instance("adjust.presence", { Value("texture", 1.) }),
			Instance("adjust.sharpen", { Value("amount", 1.) }),
			Instance("adjust.grain", { Value("amount", 1.) }),
		}) {
			const auto result = Applied(deep, instance);
			close = std::max(
				close,
				result.isNull() ? 255 : MaxDifference(result, deep));
		}
		check(
			still <= 1 && close <= 2,
			u"deep saturated shadows keep their color (max difference %1 "
			"with an unchanged tone, %2 next to the neutral values)"_q.arg(
				still).arg(close));
	}

	// adjust.light.
	{
		const auto light = [&](const char *id, double value) {
			return Measure(Applied(photo, Instance("adjust.light", {
				Value(id, value),
			})));
		};
		const auto brighter = light("exposure", 1.);
		const auto darker = light("exposure", -1.);
		check(
			brighter.luma > base.luma + 20. && darker.luma < base.luma - 20.,
			u"exposure moves the mean (%1 / %2 / %3)"_q.arg(
				number(darker.luma, 1),
				number(base.luma, 1),
				number(brighter.luma, 1)));
		const auto gray = SolidImage(8, 8, QColor(128, 128, 128));
		const auto stop = Measure(Applied(gray, Instance("adjust.light", {
			Value("exposure", -1.),
		})));
		const auto expected = 255. * FxLinearToSrgb(
			FxSrgbToLinear(128 / 255.) / 2.);
		check(
			std::abs(stop.luma - expected) < 1.,
			u"-1 EV halves the light (%1, expected %2)"_q.arg(
				number(stop.luma, 1),
				number(expected, 1)));
		const auto white = Measure(Applied(
			SolidImage(8, 8, QColor(255, 255, 255)),
			Instance("adjust.light", { Value("exposure", 2.) })));
		check(white.luma > 253., u"exposure keeps white white"_q);

		const auto more = light("contrast", 60.);
		const auto less = light("contrast", -60.);
		check(
			more.deviation > base.deviation * 1.1
				&& less.deviation < base.deviation * 0.9,
			u"contrast moves the deviation (%1 / %2 / %3)"_q.arg(
				number(less.deviation, 1),
				number(base.deviation, 1),
				number(more.deviation, 1)));

		// A dark and a bright half with some texture.
		auto halves = NoisyStep(240, 120, 0.04f);
		for (auto y = 0; y != halves.height(); ++y) {
			const auto line = FxRow(halves, y);
			for (auto x = 0; x != halves.width(); ++x) {
				const auto color = FxUnpack(line[x]);
				const auto shift = (x < halves.width() / 2) ? -0.2f : 0.2f;
				line[x] = FxPack({
					color.r + shift,
					color.g + shift,
					color.b + shift,
					1.f,
				});
			}
		}
		const auto darkRect = QRect(10, 10, 90, 100);
		const auto brightRect = QRect(140, 10, 90, 100);
		const auto darkBase = Measure(halves, darkRect);
		const auto brightBase = Measure(halves, brightRect);
		const auto lifted = Applied(halves, Instance("adjust.light", {
			Value("shadows", 80.),
		}));
		const auto recovered = Applied(halves, Instance("adjust.light", {
			Value("highlights", -80.),
		}));
		const auto liftedDark = Measure(lifted, darkRect);
		const auto liftedBright = Measure(lifted, brightRect);
		const auto recoveredDark = Measure(recovered, darkRect);
		const auto recoveredBright = Measure(recovered, brightRect);
		check(
			liftedDark.luma > darkBase.luma + 15.
				&& std::abs(liftedBright.luma - brightBase.luma) < 4.,
			u"shadows lift the dark half only (%1 -> %2, bright %3 -> "
			"%4)"_q.arg(
				number(darkBase.luma, 1),
				number(liftedDark.luma, 1),
				number(brightBase.luma, 1),
				number(liftedBright.luma, 1)));
		check(
			recoveredBright.luma < brightBase.luma - 15.
				&& std::abs(recoveredDark.luma - darkBase.luma) < 4.,
			u"highlights recover the bright half only (%1 -> %2, dark %3 "
			"-> %4)"_q.arg(
				number(brightBase.luma, 1),
				number(recoveredBright.luma, 1),
				number(darkBase.luma, 1),
				number(recoveredDark.luma, 1)));
		// A global curve that lifts the shadows this much would flatten
		// the texture of the dark midtones to about a third.
		const auto textured = NoisyStep(240, 120, 0.04f);
		const auto texturedBase = Measure(textured, darkRect);
		const auto texturedLifted = Measure(
			Applied(textured, Instance("adjust.light", {
				Value("shadows", 100.),
			})),
			darkRect);
		check(
			texturedLifted.luma > texturedBase.luma + 15.
				&& texturedLifted.detail > texturedBase.detail * 0.8,
			u"lifted shadows keep the local contrast (%1 -> %2)"_q.arg(
				number(texturedBase.detail, 2),
				number(texturedLifted.detail, 2)));

		const auto whiter = light("whites", 80.);
		const auto blacker = light("blacks", -80.);
		check(
			whiter.percentile(0.95) > base.percentile(0.95)
				&& std::abs(whiter.percentile(0.1) - base.percentile(0.1)) <= 2,
			u"whites move the bright end (%1 -> %2)"_q.arg(
				base.percentile(0.95)).arg(whiter.percentile(0.95)));
		check(
			blacker.percentile(0.05) < base.percentile(0.05)
				&& std::abs(
					blacker.percentile(0.9) - base.percentile(0.9)) <= 2,
			u"blacks move the dark end (%1 -> %2)"_q.arg(
				base.percentile(0.05)).arg(blacker.percentile(0.05)));
	}

	// adjust.curve.
	{
		auto curve = ToneCurve();
		curve.channels[0] = { { 0, 0 }, { 500, 650 }, { 1000, 1000 } };
		const auto lifted = Measure(Applied(photo, Instance("adjust.curve", {
			Bytes("curve", SerializeToneCurve(curve)),
		})));
		check(
			lifted.luma > base.luma + 10.,
			u"a lifted curve brightens (%1 -> %2)"_q.arg(
				number(base.luma, 1),
				number(lifted.luma, 1)));
		auto red = ToneCurve();
		red.channels[1] = { { 0, 0 }, { 500, 300 }, { 1000, 1000 } };
		const auto less = Measure(Applied(photo, Instance("adjust.curve", {
			Bytes("curve", SerializeToneCurve(red)),
		})));
		check(
			less.r < base.r - 10.
				&& std::abs(less.g - base.g) < 0.01
				&& std::abs(less.b - base.b) < 0.01,
			u"a channel curve changes only its channel"_q);
		auto inverted = ToneCurve();
		inverted.channels[0] = { { 0, 1000 }, { 1000, 0 } };
		const auto negative = Applied(
			SolidImage(4, 4, QColor(10, 100, 250)),
			Instance("adjust.curve", {
				Bytes("curve", SerializeToneCurve(inverted)),
			}));
		check(
			!negative.isNull()
				&& negative.pixelColor(1, 1) == QColor(245, 155, 5),
			u"an inverted curve gives the exact negative"_q);
		const auto soft = Applied(cut, Instance("adjust.curve", {
			Bytes("curve", SerializeToneCurve(curve)),
		}));
		check(
			!soft.isNull() && SameAlpha(cut, soft),
			u"the curve keeps the transparency"_q);
	}

	// adjust.color.
	{
		const auto color = [&](const char *id, double value) {
			return Measure(Applied(photo, Instance("adjust.color", {
				Value(id, value),
			})));
		};
		const auto warm = color("temperature", 60.);
		const auto cold = color("temperature", -60.);
		check(
			(warm.r - warm.b) > (base.r - base.b) + 15.
				&& (cold.r - cold.b) < (base.r - base.b) - 15.,
			u"temperature moves red against blue (%1 / %2 / %3)"_q.arg(
				number(cold.r - cold.b, 1),
				number(base.r - base.b, 1),
				number(warm.r - warm.b, 1)));
		const auto magenta = color("tint", 60.);
		const auto green = color("tint", -60.);
		const auto balance = [](const Stats &stats) {
			return stats.g - (stats.r + stats.b) / 2.;
		};
		check(
			balance(magenta) < balance(base) - 5.
				&& balance(green) > balance(base) + 5.,
			u"tint moves green against magenta"_q);
		check(
			std::abs(warm.luma - base.luma) < 12.,
			u"white balance keeps the brightness (%1 -> %2)"_q.arg(
				number(base.luma, 1),
				number(warm.luma, 1)));

		const auto vivid = color("saturation", 50.);
		const auto muted = color("saturation", -50.);
		check(
			vivid.chroma > base.chroma * 1.15
				&& muted.chroma < base.chroma * 0.75,
			u"saturation moves the chroma (%1 / %2 / %3)"_q.arg(
				number(muted.chroma, 1),
				number(base.chroma, 1),
				number(vivid.chroma, 1)));
		const auto gray = Applied(photo, Instance("adjust.color", {
			Value("saturation", -100.),
		}));
		auto exact = !gray.isNull();
		for (auto y = 0; exact && y != gray.height(); ++y) {
			const auto line = FxRow(gray, y);
			for (auto x = 0; x != gray.width(); ++x) {
				const auto pixel = line[x];
				exact = exact
					&& (((pixel >> 16) & 0xFFU) == ((pixel >> 8) & 0xFFU))
					&& (((pixel >> 8) & 0xFFU) == (pixel & 0xFFU));
			}
		}
		check(exact, u"saturation -100 is an exact gray"_q);

		// A muted and a vivid patch of the same hue.
		const auto patches = PatchImage({
			QColor(120, 140, 170),
			QColor(20, 90, 230),
		}, 24, 24);
		const auto boosted = Applied(patches, Instance("adjust.color", {
			Value("vibrance", 80.),
		}));
		const auto mutedBefore = Measure(patches, PatchRect(0, 24, 24)).chroma;
		const auto mutedAfter = Measure(boosted, PatchRect(0, 24, 24)).chroma;
		const auto vividBefore = Measure(patches, PatchRect(1, 24, 24)).chroma;
		const auto vividAfter = Measure(boosted, PatchRect(1, 24, 24)).chroma;
		check(
			mutedAfter > mutedBefore * 1.3
				&& (vividAfter / vividBefore) < (mutedAfter / mutedBefore),
			u"vibrance prefers muted colors (x%1 against x%2)"_q.arg(
				number(mutedAfter / mutedBefore),
				number(vividAfter / vividBefore)));
	}

	// adjust.hsl and adjust.bw on patches of the eight ranges and a gray.
	{
		constexpr auto kPatch = 20;
		auto colors = std::vector<QColor>();
		for (auto i = 0; i != kHslRanges; ++i) {
			colors.push_back(HslRangeColor(i));
		}
		colors.push_back(QColor(128, 128, 128));
		const auto patches = PatchImage(colors, kPatch, kPatch);
		const auto patch = [&](const QImage &image, int index) {
			return Measure(image, PatchRect(index, kPatch, kPatch));
		};
		const auto blue = int(HslRange::Blue);
		const auto red = int(HslRange::Red);
		const auto green = int(HslRange::Green);

		auto table = HslTable();
		table.saturation[blue] = -100;
		const auto faded = Applied(patches, Instance("adjust.hsl", {
			Bytes("table", SerializeHslTable(table)),
		}));
		check(
			patch(faded, blue).chroma < patch(patches, blue).chroma * 0.3
				&& std::abs(patch(faded, red).chroma
					- patch(patches, red).chroma) < 1.5
				&& std::abs(patch(faded, green).chroma
					- patch(patches, green).chroma) < 1.5
				&& MaxDifference(
					faded.copy(PatchRect(8, kPatch, kPatch)),
					patches.copy(PatchRect(8, kPatch, kPatch))) <= 1,
			u"HSL saturation changes only its range (blue %1 -> %2)"_q.arg(
				number(patch(patches, blue).chroma, 1),
				number(patch(faded, blue).chroma, 1)));

		table = HslTable();
		table.luminance[blue] = 80;
		const auto bright = Applied(patches, Instance("adjust.hsl", {
			Bytes("table", SerializeHslTable(table)),
		}));
		check(
			patch(bright, blue).luma > patch(patches, blue).luma + 10.
				&& std::abs(patch(bright, red).luma
					- patch(patches, red).luma) < 1.5,
			u"HSL luminance changes only its range (blue %1 -> %2)"_q.arg(
				number(patch(patches, blue).luma, 1),
				number(patch(bright, blue).luma, 1)));

		table = HslTable();
		table.hue[red] = 100;
		const auto turned = Applied(patches, Instance("adjust.hsl", {
			Bytes("table", SerializeHslTable(table)),
		}));
		const auto before = patch(patches, red);
		const auto after = patch(turned, red);
		check(
			(after.g - after.b) > (before.g - before.b) + 20.
				&& std::abs(after.luma - before.luma) < 25.
				&& std::abs(patch(turned, blue).b
					- patch(patches, blue).b) < 1.5,
			u"HSL hue turns red towards orange (green - blue %1 -> "
			"%2)"_q.arg(
				number(before.g - before.b, 1),
				number(after.g - after.b, 1)));

		const auto plain = Applied(patches, Instance("adjust.bw"));
		const auto mixed = Applied(patches, Instance("adjust.bw", {
			Value("red", 80.),
			Value("blue", -80.),
		}));
		check(
			!plain.isNull()
				&& patch(plain, red).chroma == 0.
				&& patch(plain, blue).chroma == 0.
				&& patch(mixed, red).luma > patch(plain, red).luma + 15.
				&& patch(mixed, blue).luma < patch(plain, blue).luma - 10.
				&& std::abs(patch(mixed, green).luma
					- patch(plain, green).luma) < 1.5
				&& std::abs(patch(mixed, 8).luma - patch(plain, 8).luma) < 1.,
			u"black and white mix (red %1 -> %2, blue %3 -> %4)"_q.arg(
				number(patch(plain, red).luma, 1),
				number(patch(mixed, red).luma, 1),
				number(patch(plain, blue).luma, 1),
				number(patch(mixed, blue).luma, 1)));
	}

	// adjust.grading.
	{
		const auto tones = PatchImage({
			QColor(30, 30, 30),
			QColor(120, 120, 120),
			QColor(225, 225, 225),
		}, 24, 24);
		auto wheels = GradeWheels();
		wheels.ranges[0] = { 220, 80 };
		const auto cool = Applied(tones, Instance("adjust.grading", {
			Bytes("wheels", SerializeGradeWheels(wheels)),
		}));
		const auto tone = [&](const QImage &image, int index) {
			return Measure(image, PatchRect(index, 24, 24));
		};
		const auto warmth = [](const Stats &stats) {
			return stats.r - stats.b;
		};
		check(
			warmth(tone(cool, 0)) < -12.
				&& std::abs(warmth(tone(cool, 2))) < 2.,
			u"a shadows tint colors the dark tones only (%1 / %2 / "
			"%3)"_q.arg(
				number(warmth(tone(cool, 0)), 1),
				number(warmth(tone(cool, 1)), 1),
				number(warmth(tone(cool, 2)), 1)));
		wheels = GradeWheels();
		wheels.ranges[2] = { 35, 80 };
		const auto warm = Applied(tones, Instance("adjust.grading", {
			Bytes("wheels", SerializeGradeWheels(wheels)),
		}));
		check(
			warmth(tone(warm, 2)) > 12.
				&& std::abs(warmth(tone(warm, 0))) < 2.,
			u"a highlights tint colors the bright tones only (%1 / %2 / "
			"%3)"_q.arg(
				number(warmth(tone(warm, 0)), 1),
				number(warmth(tone(warm, 1)), 1),
				number(warmth(tone(warm, 2)), 1)));
		const auto shifted = Applied(tones, Instance("adjust.grading", {
			Bytes("wheels", SerializeGradeWheels(wheels)),
			Value("balance", 100.),
		}));
		check(
			warmth(tone(shifted, 1)) > warmth(tone(warm, 1)) + 3.,
			u"the balance gives the midtones to the highlights (%1 -> "
			"%2)"_q.arg(
				number(warmth(tone(warm, 1)), 1),
				number(warmth(tone(shifted, 1)), 1)));
		const auto dim = Applied(tones, Instance("adjust.grading", {
			Value("midtones_lum", -80.),
		}));
		check(
			tone(dim, 1).luma < tone(tones, 1).luma - 10.
				&& std::abs(tone(dim, 1).chroma) < 1.5,
			u"the midtones luminance darkens the midtones"_q);
	}

	// adjust.presence.
	{
		const auto textured = NoisyStep(240, 160, 0.05f);
		const auto flat = QRect(20, 20, 80, 120);
		const auto before = Measure(textured, flat);
		const auto presence = [&](const char *id, double value) {
			return Applied(textured, Instance("adjust.presence", {
				Value(id, value),
			}));
		};
		const auto crisp = presence("texture", 80.);
		const auto smooth = presence("texture", -80.);
		check(
			Measure(crisp, flat).detail > before.detail * 1.2
				&& Measure(smooth, flat).detail < before.detail * 0.8,
			u"texture moves the fine detail (%1 / %2 / %3)"_q.arg(
				number(Measure(smooth, flat).detail, 2),
				number(before.detail, 2),
				number(Measure(crisp, flat).detail, 2)));

		const auto clear = Applied(photo, Instance("adjust.presence", {
			Value("clarity", 80.),
		}));
		const auto soft = Applied(photo, Instance("adjust.presence", {
			Value("clarity", -80.),
		}));
		check(
			Measure(clear).detail > base.detail * 1.05
				&& Measure(soft).detail < base.detail * 0.95,
			u"clarity moves the local contrast (%1 / %2 / %3)"_q.arg(
				number(Measure(soft).detail, 2),
				number(base.detail, 2),
				number(Measure(clear).detail, 2)));

		// No halo: a clean step keeps its two levels.
		const auto step = NoisyStep(240, 160, 0.f);
		const auto edged = Applied(step, Instance("adjust.presence", {
			Value("clarity", 100.),
		}));
		const auto leftSide = Measure(edged, QRect(0, 0, 120, 160));
		const auto rightSide = Measure(edged, QRect(120, 0, 120, 160));
		const auto low = int(std::lround(255 * 0.35));
		const auto high = int(std::lround(255 * 0.65));
		check(
			leftSide.percentile(0.001) >= low - 12
				&& leftSide.percentile(0.999) <= low + 12
				&& rightSide.percentile(0.001) >= high - 12
				&& rightSide.percentile(0.999) <= high + 12,
			u"clarity makes almost no halo at a strong edge (%1..%2, "
			"%3..%4)"_q.arg(
				leftSide.percentile(0.001)).arg(
				leftSide.percentile(0.999)).arg(
				rightSide.percentile(0.001)).arg(
				rightSide.percentile(0.999)));

		// Haze: the picture mixed with a bright gray.
		auto hazy = photo;
		for (auto y = 0; y != hazy.height(); ++y) {
			const auto line = FxRow(hazy, y);
			for (auto x = 0; x != hazy.width(); ++x) {
				const auto color = FxUnpack(line[x]);
				line[x] = FxPack({
					color.r * 0.5f + 0.45f,
					color.g * 0.5f + 0.45f,
					color.b * 0.5f + 0.45f,
					1.f,
				});
			}
		}
		const auto hazyStats = Measure(hazy);
		const auto dehazed = Measure(Applied(hazy, Instance("adjust.presence", {
			Value("dehaze", 80.),
		})));
		const auto foggy = Measure(Applied(photo, Instance("adjust.presence", {
			Value("dehaze", -80.),
		})));
		check(
			dehazed.deviation > hazyStats.deviation * 1.2
				&& dehazed.luma < hazyStats.luma - 10.
				&& dehazed.chroma > hazyStats.chroma * 1.2,
			u"dehaze brings the contrast back (deviation %1 -> %2, mean "
			"%3 -> %4)"_q.arg(
				number(hazyStats.deviation, 1),
				number(dehazed.deviation, 1),
				number(hazyStats.luma, 1),
				number(dehazed.luma, 1)));
		check(
			foggy.deviation < base.deviation * 0.9 && foggy.luma > base.luma,
			u"a negative dehaze adds haze (deviation %1 -> %2)"_q.arg(
				number(base.deviation, 1),
				number(foggy.deviation, 1)));
	}

	// adjust.sharpen and adjust.denoise.
	{
		const auto noisy = NoisyStep(240, 160, 0.04f);
		const auto flat = QRect(20, 20, 80, 120);
		const auto before = Measure(noisy, flat);
		const auto sharp = Applied(noisy, Instance("adjust.sharpen", {
			Value("amount", 100.),
		}));
		const auto masked = Applied(noisy, Instance("adjust.sharpen", {
			Value("amount", 100.),
			Value("masking", 100.),
		}));
		const auto sharpFlat = Measure(sharp, flat);
		const auto maskedFlat = Measure(masked, flat);
		check(
			sharpFlat.detail > before.detail * 1.3,
			u"sharpening raises the fine detail (%1 -> %2)"_q.arg(
				number(before.detail, 2),
				number(sharpFlat.detail, 2)));
		check(
			maskedFlat.detail < before.detail * 1.05
				&& maskedFlat.detail < sharpFlat.detail * 0.8,
			u"masking keeps the flat areas (%1 against %2)"_q.arg(
				number(maskedFlat.detail, 2),
				number(sharpFlat.detail, 2)));
		// The edge itself is still sharpened with the masking.
		const auto soft = [&] {
			auto result = NoisyStep(240, 160, 0.f);
			FxGaussianBlur(result, 1.5);
			return result;
		}();
		// How much the luminance rises over the four pixels around the
		// edge: a sharper edge rises more.
		const auto rise = [](const QImage &image) {
			return Measure(image, QRect(121, 20, 2, 120)).luma
				- Measure(image, QRect(117, 20, 2, 120)).luma;
		};
		const auto edgeBefore = rise(soft);
		const auto edgeSharp = rise(Applied(soft, Instance("adjust.sharpen", {
			Value("amount", 100.),
			Value("radius", 1.5),
		})));
		const auto edgeMasked = rise(Applied(soft, Instance("adjust.sharpen", {
			Value("amount", 100.),
			Value("radius", 1.5),
			Value("masking", 100.),
		})));
		check(
			edgeSharp > edgeBefore * 1.15
				&& (edgeMasked - edgeBefore) > (edgeSharp - edgeBefore) * 0.8,
			u"masking still sharpens the edges (%1 -> %2, without "
			"masking %3)"_q.arg(
				number(edgeBefore, 1),
				number(edgeMasked, 1),
				number(edgeSharp, 1)));
		const auto narrow = Applied(noisy, Instance("adjust.sharpen", {
			Value("amount", 100.),
			Value("radius", 0.5),
		}));
		const auto wide = Applied(noisy, Instance("adjust.sharpen", {
			Value("amount", 100.),
			Value("radius", 3.),
		}));
		check(
			Measure(narrow, flat).detail > before.detail
				&& Measure(wide, flat).detail > Measure(narrow, flat).detail,
			u"a larger radius sharpens more (%1 / %2)"_q.arg(
				number(Measure(narrow, flat).detail, 2),
				number(Measure(wide, flat).detail, 2)));
		const auto cutSharp = Applied(cut, Instance("adjust.sharpen", {
			Value("amount", 120.),
			Value("masking", 30.),
		}));
		check(
			!cutSharp.isNull() && SameAlpha(cut, cutSharp),
			u"sharpening keeps the transparency"_q);

		const auto clean = Applied(noisy, Instance("adjust.denoise", {
			Value("luminance", 70.),
			Value("color", 0.),
		}));
		const auto cleanFlat = Measure(clean, flat);
		const auto leftSide = Measure(clean, QRect(40, 20, 40, 120)).luma;
		const auto rightSide = Measure(clean, QRect(160, 20, 40, 120)).luma;
		// The mean over a strip right at the edge: a blur would mix it.
		const auto nearLeft = Measure(clean, QRect(114, 20, 5, 120)).luma;
		const auto nearRight = Measure(clean, QRect(121, 20, 5, 120)).luma;
		check(
			cleanFlat.deviation < before.deviation * 0.6,
			u"luminance noise reduction (deviation %1 -> %2)"_q.arg(
				number(before.deviation, 2),
				number(cleanFlat.deviation, 2)));
		check(
			(nearRight - nearLeft) > (rightSide - leftSide) * 0.9,
			u"noise reduction keeps the edge (%1 of %2)"_q.arg(
				number(nearRight - nearLeft, 1),
				number(rightSide - leftSide, 1)));
		const auto colorless = Applied(noisy, Instance("adjust.denoise", {
			Value("luminance", 0.),
			Value("color", 80.),
		}));
		const auto colorFlat = Measure(colorless, flat);
		check(
			colorFlat.chroma < before.chroma * 0.5
				&& colorFlat.deviation > before.deviation * 0.7,
			u"color noise reduction (chroma %1 -> %2, the luminance "
			"detail stays: %3 -> %4)"_q.arg(
				number(before.chroma, 2),
				number(colorFlat.chroma, 2),
				number(before.deviation, 2),
				number(colorFlat.deviation, 2)));

		// A 24 MP photo in a preview that fills a large screen: more than
		// three megapixels with a blur narrower than a pixel. The filter
		// must keep the fine detail there as it does in the export.
		const auto source = CellTexture(6000, 4000, 4);
		const auto reduced = FxResized(source, QSize(2268, 1512));
		const auto scale = reduced.width() / double(source.width());
		const auto part = QRect(1000, 600, 256, 256);
		const auto fine = Measure(reduced, part).detail;
		auto kept = 1.;
		auto worst = 0.;
		for (const auto luminance : { 10., 30. }) {
			const auto instance = Instance("adjust.denoise", {
				Value("luminance", luminance),
				Value("color", 0.),
			});
			const auto preview = Applied(
				reduced,
				instance,
				scale,
				source.size());
			const auto exported = Applied(source, instance);
			kept = std::min(
				kept,
				Measure(preview, part).detail / std::max(fine, 1e-3));
			worst = std::max(worst, FxImageDifference(
				preview,
				FxResized(exported, reduced.size())));
		}
		check(
			kept > 0.85 && worst < 1.5,
			u"noise reduction keeps the detail in a large preview (%1 of "
			"it, the preview differs by %2)"_q.arg(
				number(kept),
				number(worst)));
	}

	// adjust.vignette.
	{
		const auto gray = SolidImage(200, 120, QColor(150, 150, 150));
		const auto corner = QRect(0, 0, 12, 8);
		const auto center = QRect(90, 52, 20, 16);
		const auto side = QRect(94, 0, 12, 6);
		const auto vignette = [&](const std::vector<FxParams::Entry> &list) {
			return Applied(gray, Instance("adjust.vignette", list));
		};
		const auto dark = vignette({ Value("amount", -60.) });
		const auto bright = vignette({ Value("amount", 60.) });
		check(
			Measure(dark, corner).luma < 110.
				&& std::abs(Measure(dark, center).luma - 150.) < 1.
				&& Measure(bright, corner).luma > 190.
				&& std::abs(Measure(bright, center).luma - 150.) < 1.,
			u"vignette darkens or lightens the corners (%1 / %2), the "
			"center stays"_q.arg(
				number(Measure(dark, corner).luma, 1),
				number(Measure(bright, corner).luma, 1)));
		const auto close = vignette({
			Value("amount", -60.),
			Value("midpoint", 10.),
		});
		const auto distant = vignette({
			Value("amount", -60.),
			Value("midpoint", 90.),
		});
		check(
			Measure(close, side).luma < Measure(dark, side).luma - 5.
				&& Measure(distant, side).luma > Measure(dark, side).luma + 5.,
			u"the midpoint moves the vignette (%1 / %2 / %3)"_q.arg(
				number(Measure(close, side).luma, 1),
				number(Measure(dark, side).luma, 1),
				number(Measure(distant, side).luma, 1)));
		const auto hard = vignette({
			Value("amount", -60.),
			Value("feather", 0.),
		});
		const auto softer = vignette({
			Value("amount", -60.),
			Value("feather", 100.),
		});
		// Inside of the middle radius a hard vignette is not there yet,
		// in the corner it is complete.
		const auto inner = QRect(165, 55, 10, 10);
		check(
			std::abs(Measure(hard, inner).luma - 150.) < 1.
				&& Measure(softer, inner).luma < 145.
				&& Measure(hard, corner).luma < Measure(softer, corner).luma,
			u"the feather softens the edge (inside %1 / %2, corner %3 / "
			"%4)"_q.arg(
				number(Measure(hard, inner).luma, 1),
				number(Measure(softer, inner).luma, 1),
				number(Measure(hard, corner).luma, 1),
				number(Measure(softer, corner).luma, 1)));
		const auto circle = vignette({
			Value("amount", -60.),
			Value("roundness", 100.),
		});
		const auto square = vignette({
			Value("amount", -60.),
			Value("roundness", -100.),
		});
		// A circle on a wide picture leaves the middle of the long sides
		// as it was and darkens the short sides more, a rectangle
		// darkens all the sides like the corners.
		const auto shortSide = QRect(0, 54, 6, 12);
		check(
			std::abs(Measure(circle, side).luma - Measure(dark, side).luma) < 1.
				&& Measure(circle, shortSide).luma
					< Measure(dark, shortSide).luma - 3.
				&& Measure(square, side).luma
					< Measure(dark, side).luma - 3.
				&& std::abs(Measure(square, corner).luma
					- Measure(dark, corner).luma) < 6.,
			u"the roundness changes the shape (long side %1 / %2 / %3, "
			"short side %4 / %5)"_q.arg(
				number(Measure(square, side).luma, 1),
				number(Measure(dark, side).luma, 1),
				number(Measure(circle, side).luma, 1),
				number(Measure(dark, shortSide).luma, 1),
				number(Measure(circle, shortSide).luma, 1)));
	}

	// adjust.grain.
	{
		const auto gray = SolidImage(256, 192, QColor(128, 128, 128));
		const auto grain = [&](const std::vector<FxParams::Entry> &list) {
			return Applied(gray, Instance("adjust.grain", list));
		};
		const auto light = grain({ Value("amount", 20.) });
		const auto heavy = grain({ Value("amount", 80.) });
		check(
			Measure(light).deviation > 1.
				&& Measure(heavy).deviation > Measure(light).deviation * 2.
				&& std::abs(Measure(heavy).luma - 128.) < 2.
				&& Measure(heavy).chroma < 0.05,
			u"grain grows with the amount (%1 / %2), the mean and the "
			"color stay"_q.arg(
				number(Measure(light).deviation, 2),
				number(Measure(heavy).deviation, 2)));
		// The size is relative to the layer: a larger picture is needed
		// to see grains of several pixels.
		const auto wide = SolidImage(800, 600, QColor(128, 128, 128));
		const auto fine = Applied(wide, Instance("adjust.grain", {
			Value("amount", 60.),
			Value("size", 0.),
		}));
		const auto coarse = Applied(wide, Instance("adjust.grain", {
			Value("amount", 60.),
			Value("size", 100.),
		}));
		// A larger grain: neighbour pixels differ less for the same
		// overall strength.
		const auto ratio = [](const Stats &stats) {
			return stats.detail / std::max(stats.deviation, 1e-3);
		};
		check(
			ratio(Measure(coarse)) < ratio(Measure(fine)) * 0.7,
			u"the size makes the grain larger (%1 against %2)"_q.arg(
				number(ratio(Measure(coarse))),
				number(ratio(Measure(fine)))));
		const auto even = grain({
			Value("amount", 60.),
			Value("roughness", 0.),
		});
		const auto rough = grain({
			Value("amount", 60.),
			Value("roughness", 100.),
		});
		check(
			Measure(rough).deviation > Measure(even).deviation * 1.15,
			u"the roughness makes the grain uneven (%1 against %2)"_q.arg(
				number(Measure(even).deviation, 2),
				number(Measure(rough).deviation, 2)));
		const auto other = grain({ Value("amount", 20.), Value("seed", 7.) });
		check(
			MaxDifference(light, grain({ Value("amount", 20.) })) == 0
				&& MaxDifference(light, other) > 0,
			u"grain is deterministic, the seed changes it"_q);
	}

	// Every effect with real values: the transparency stays, the preview
	// looks like the export, the time for a 1920 x 1080 picture.
	{
		auto curve = ToneCurve();
		curve.channels[0] = {
			{ 0, 0 },
			{ 250, 190 },
			{ 750, 820 },
			{ 1000, 1000 },
		};
		curve.channels[3] = { { 0, 40 }, { 1000, 960 } };
		auto table = HslTable();
		table.hue[int(HslRange::Blue)] = -30;
		table.saturation[int(HslRange::Orange)] = 40;
		table.luminance[int(HslRange::Blue)] = -25;
		auto wheels = GradeWheels();
		wheels.ranges[0] = { 210, 40 };
		wheels.ranges[2] = { 40, 35 };
		struct Case {
			FxInstance instance;
			double limit = 0.; // For the preview difference, 0: only log.
		};
		const auto list = std::vector<Case>{
			{ Instance("adjust.light", {
				Value("exposure", 0.4),
				Value("contrast", 25.),
				Value("highlights", -50.),
				Value("shadows", 45.),
				Value("whites", 15.),
				Value("blacks", -10.),
			}), 2.5 },
			{ Instance("adjust.curve", {
				Bytes("curve", SerializeToneCurve(curve)),
			}), 2.5 },
			{ Instance("adjust.color", {
				Value("temperature", 20.),
				Value("tint", -10.),
				Value("vibrance", 40.),
				Value("saturation", 10.),
			}), 2.5 },
			{ Instance("adjust.hsl", {
				Bytes("table", SerializeHslTable(table)),
			}), 2.5 },
			{ Instance("adjust.grading", {
				Bytes("wheels", SerializeGradeWheels(wheels)),
				Value("midtones_lum", 10.),
				Value("balance", -20.),
			}), 2.5 },
			{ Instance("adjust.bw", {
				Value("red", 30.),
				Value("blue", -40.),
			}), 2.5 },
			{ Instance("adjust.presence", {
				Value("texture", 40.),
				Value("clarity", 40.),
				Value("dehaze", 30.),
			}), 4. },
			{ Instance("adjust.sharpen", {
				Value("amount", 80.),
				Value("radius", 1.2),
				Value("masking", 30.),
			}), 4. },
			{ Instance("adjust.denoise", {
				Value("luminance", 50.),
				Value("color", 50.),
			}), 4. },
			{ Instance("adjust.vignette", {
				Value("amount", -50.),
				Value("roundness", -40.),
			}), 2.5 },
			{ Instance("adjust.grain", {
				Value("amount", 40.),
				Value("size", 40.),
			}), 0. },
		};
		const auto full = FxTestImage(512, 384);
		const auto half = FxResized(full, full.size() / 2);
		const auto large = FxTestImage(1920, 1080);
		auto timer = QElapsedTimer();
		for (const auto &entry : list) {
			const auto name = QString::fromLatin1(entry.instance.id);
			const auto soft = Applied(cut, entry.instance);
			if (soft.isNull() || !SameAlpha(cut, soft)) {
				check(false, name + u": keeps the transparency"_q);
				continue;
			}
			const auto exported = Applied(full, entry.instance);
			const auto preview = Applied(
				half,
				entry.instance,
				0.5,
				full.size());
			const auto difference = FxImageDifference(
				preview,
				FxResized(exported, half.size()));
			const auto changed = FxImageDifference(exported, full);
			if (entry.limit > 0. && !(difference < entry.limit)) {
				check(false, name + u": the preview differs by %1"_q.arg(
					number(difference)));
				continue;
			} else if (!(changed > 0.3)) {
				check(false, name + u": changes the picture"_q);
				continue;
			}
			auto copy = large;
			timer.start();
			const auto done = ApplyFx(copy, entry.instance, FxContext{
				.seed = 77,
				.fullSize = large.size(),
			});
			const auto ms = timer.nsecsElapsed() / 1e6;
			if (!done) {
				check(false, name + u": applies to 1920 x 1080"_q);
				continue;
			}
			info(u"%1: 1920 x 1080 in %2 ms, changes %3, the preview "
				"differs by %4"_q.arg(
					name,
					number(ms, 1),
					number(changed),
					number(difference)));
		}
		check(true, u"every adjustment ran with real values"_q);

		// The whole stack on a 12 MP picture.
		auto stack = std::vector<FxInstance>();
		for (const auto &entry : list) {
			if (entry.instance.id != "adjust.bw") {
				stack.push_back(entry.instance);
			}
		}
		auto huge = FxTestImage(4000, 3000);
		timer.start();
		const auto done = ApplyFxStack(huge, stack, FxContext{
			.seed = 77,
			.fullSize = huge.size(),
		});
		info(u"all ten adjustments on 4000 x 3000 in %1 ms"_q.arg(
			number(timer.nsecsElapsed() / 1e6, 0)));
		check(done, u"the whole stack applies to a 12 MP picture"_q);

		// Cancelling.
		const auto cancel = std::atomic<bool>(true);
		auto cancelled = true;
		for (const auto &entry : list) {
			auto copy = full;
			cancelled = cancelled && !ApplyFx(copy, entry.instance, FxContext{
				.fullSize = full.size(),
				.cancel = &cancel,
			});
		}
		check(cancelled, u"a cancelled adjustment reports it"_q);
	}

	// Tiny pictures must not break anything.
	{
		auto fine = true;
		for (const auto size : { QSize(1, 1), QSize(3, 2), QSize(1, 40) }) {
			const auto tiny = FxTestImage(size.width(), size.height(), true);
			for (const auto descriptor : AllFx()) {
				if (!descriptor->id.startsWith("adjust.")) {
					continue;
				}
				auto instance = MakeFx(descriptor->id);
				for (const auto &param : descriptor->params) {
					if (param.kind == FxParamKind::Int
						|| param.kind == FxParamKind::Float) {
						instance.params.set(
							param.id,
							param.normalized(FxValue::Number(
								param.min + (param.max - param.min) * 0.8)));
					}
				}
				const auto result = Applied(tiny, instance);
				fine = fine && !result.isNull() && SameAlpha(tiny, result);
			}
		}
		check(fine, u"tiny pictures are handled"_q);
	}
	return ok;
}

const auto SelfTest = SelfTestRegistrar(
	SelfTestSuite::Fx,
	"adjust",
	&RunAdjustSelfTest);

} // namespace

ToneCurve::ToneCurve() {
	for (auto &channel : channels) {
		channel = DefaultCurvePoints();
	}
}

bool ToneCurve::identity(int channel) const {
	if (channel < 0 || channel >= kCurveChannels) {
		return true;
	}
	const auto &points = channels[channel];
	if (points.size() < 2
		|| points.front().x != 0
		|| points.back().x != kCurveUnit) {
		return false;
	}
	return ranges::all_of(points, [](const CurvePoint &point) {
		return point.x == point.y;
	});
}

bool ToneCurve::identity() const {
	for (auto i = 0; i != kCurveChannels; ++i) {
		if (!identity(i)) {
			return false;
		}
	}
	return true;
}

CurvePoints DefaultCurvePoints() {
	return { { 0, 0 }, { kCurveUnit, kCurveUnit } };
}

CurvePoints NormalizedCurvePoints(CurvePoints points) {
	for (auto &point : points) {
		point.x = std::clamp(point.x, 0, kCurveUnit);
		point.y = std::clamp(point.y, 0, kCurveUnit);
	}
	std::stable_sort(
		begin(points),
		end(points),
		[](const CurvePoint &a, const CurvePoint &b) {
			return a.x < b.x;
		});
	auto result = CurvePoints();
	for (const auto &point : points) {
		if (int(result.size()) == kCurveMaxPoints) {
			break;
		} else if (result.empty()
			|| point.x >= result.back().x + kCurveMinGap) {
			result.push_back(point);
		}
	}
	return (result.size() < 2) ? DefaultCurvePoints() : result;
}

QByteArray SerializeToneCurve(const ToneCurve &curve) {
	auto result = QByteArray();
	const auto usual = DefaultCurvePoints();
	for (auto i = 0; i != kCurveChannels; ++i) {
		const auto points = NormalizedCurvePoints(curve.channels[i]);
		if (points == usual) {
			continue;
		}
		if (!result.isEmpty()) {
			result.append('/');
		}
		result.append(kCurveKeys[i]);
		result.append('=');
		auto first = true;
		for (const auto &point : points) {
			if (!first) {
				result.append(';');
			}
			first = false;
			result.append(QByteArray::number(point.x));
			result.append(',');
			result.append(QByteArray::number(point.y));
		}
	}
	return result;
}

ToneCurve ParseToneCurve(const QByteArray &data) {
	auto result = ToneCurve();
	if (data.isEmpty()) {
		return result;
	}
	for (auto i = 0; i != kCurveChannels; ++i) {
		const auto row = FindRow(data, kCurveKeys[i]);
		if (row.isEmpty()) {
			continue;
		}
		auto points = CurvePoints();
		auto good = true;
		for (const auto &part : row.split(';')) {
			const auto pair = ParseNumbers(part, ',');
			if (!pair || pair->size() != 2) {
				good = false;
				break;
			}
			points.push_back({ (*pair)[0], (*pair)[1] });
		}
		if (good) {
			result.channels[i] = NormalizedCurvePoints(std::move(points));
		}
	}
	return result;
}

QByteArray NormalizeToneCurve(const QByteArray &data) {
	return data.isEmpty()
		? QByteArray()
		: SerializeToneCurve(ParseToneCurve(data));
}

double CurveValue(const CurvePoints &points, double x) {
	return CurveValueWith(points, CurveSlopes(points), x);
}

void FillCurveTable(const CurvePoints &points, float *table, int size) {
	if (size < 2) {
		return;
	}
	const auto slopes = CurveSlopes(points);
	for (auto i = 0; i != size; ++i) {
		table[i] = float(CurveValueWith(points, slopes, i / double(size - 1)));
	}
}

bool HslTable::identity() const {
	const auto zero = [](int value) {
		return !value;
	};
	return ranges::all_of(hue, zero)
		&& ranges::all_of(saturation, zero)
		&& ranges::all_of(luminance, zero);
}

QByteArray SerializeHslTable(const HslTable &table) {
	auto result = QByteArray();
	const auto add = [&](char key, const std::array<int, kHslRanges> &row) {
		if (ranges::all_of(row, [](int value) { return !value; })) {
			return;
		}
		if (!result.isEmpty()) {
			result.append('/');
		}
		result.append(key);
		result.append('=');
		for (auto i = 0; i != kHslRanges; ++i) {
			if (i) {
				result.append(',');
			}
			result.append(QByteArray::number(std::clamp(row[i], -100, 100)));
		}
	};
	add('h', table.hue);
	add('s', table.saturation);
	add('l', table.luminance);
	return result;
}

HslTable ParseHslTable(const QByteArray &data) {
	auto result = HslTable();
	if (data.isEmpty()) {
		return result;
	}
	const auto read = [&](char key, std::array<int, kHslRanges> &row) {
		const auto text = FindRow(data, key);
		if (text.isEmpty()) {
			return;
		}
		const auto values = ParseNumbers(text, ',');
		if (!values || values->size() != size_t(kHslRanges)) {
			return;
		}
		for (auto i = 0; i != kHslRanges; ++i) {
			row[i] = std::clamp((*values)[i], -100, 100);
		}
	};
	read('h', result.hue);
	read('s', result.saturation);
	read('l', result.luminance);
	return result;
}

QByteArray NormalizeHslTable(const QByteArray &data) {
	return data.isEmpty()
		? QByteArray()
		: SerializeHslTable(ParseHslTable(data));
}

QColor HslRangeColor(int range) {
	switch (HslRange(std::clamp(range, 0, kHslRanges - 1))) {
	case HslRange::Red: return QColor(232, 60, 54);
	case HslRange::Orange: return QColor(243, 142, 38);
	case HslRange::Yellow: return QColor(238, 212, 46);
	case HslRange::Green: return QColor(74, 190, 84);
	case HslRange::Aqua: return QColor(48, 198, 208);
	case HslRange::Blue: return QColor(58, 112, 236);
	case HslRange::Purple: return QColor(146, 84, 232);
	case HslRange::Magenta: return QColor(226, 66, 188);
	}
	return QColor(128, 128, 128);
}

bool GradeWheels::identity() const {
	return ranges::all_of(ranges, [](const GradeWheel &wheel) {
		return !wheel.saturation;
	});
}

QByteArray SerializeGradeWheels(const GradeWheels &wheels) {
	constexpr auto kKeys = std::array<char, kGradeRanges>{ { 's', 'm', 'h' } };
	auto result = QByteArray();
	for (auto i = 0; i != kGradeRanges; ++i) {
		const auto saturation = std::clamp(wheels.ranges[i].saturation, 0, 100);
		if (!saturation) {
			continue;
		}
		if (!result.isEmpty()) {
			result.append('/');
		}
		result.append(kKeys[i]);
		result.append('=');
		result.append(QByteArray::number(
			((wheels.ranges[i].hue % 360) + 360) % 360));
		result.append(',');
		result.append(QByteArray::number(saturation));
	}
	return result;
}

GradeWheels ParseGradeWheels(const QByteArray &data) {
	constexpr auto kKeys = std::array<char, kGradeRanges>{ { 's', 'm', 'h' } };
	auto result = GradeWheels();
	if (data.isEmpty()) {
		return result;
	}
	for (auto i = 0; i != kGradeRanges; ++i) {
		const auto text = FindRow(data, kKeys[i]);
		if (text.isEmpty()) {
			continue;
		}
		const auto values = ParseNumbers(text, ',');
		if (!values || values->size() != 2) {
			continue;
		}
		const auto saturation = std::clamp((*values)[1], 0, 100);
		if (saturation) {
			result.ranges[i] = {
				(((*values)[0] % 360) + 360) % 360,
				saturation,
			};
		}
	}
	return result;
}

QByteArray NormalizeGradeWheels(const QByteArray &data) {
	return data.isEmpty()
		? QByteArray()
		: SerializeGradeWheels(ParseGradeWheels(data));
}

QColor GradeWheelColor(int hue, int saturation) {
	return QColor::fromHsvF(
		(((hue % 360) + 360) % 360) / 360.f,
		std::clamp(saturation, 0, 100) / 100.f,
		1.f);
}

} // namespace Oblivion::Photo
