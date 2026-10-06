/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_video_fx.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>

namespace Oblivion::VideoFx {
namespace {

using Pixel = uint32; // QImage::Format_ARGB32_Premultiplied: 0xAARRGGBB.

constexpr auto kPi = 3.14159265358979323846f;

// Sizes in the parameters are pixels of a frame with this shorter side.
constexpr auto kReferenceSide = 720.f;

// "One frame" for everything that is counted per frame: the states
// change by the time between the frames, not by their number.
constexpr auto kFrameMs = 1000.f / 30.f;
constexpr auto kMaxStepMs = 250.f;
constexpr auto kResetGap = crl::time(3000);
constexpr auto kMaxPreroll = crl::time(3000);

// The analysis of the tracking and of the datamosh runs on frames fitted
// into these sizes.
constexpr auto kTrackSide = 192;
constexpr auto kMoshSide = 320;
constexpr auto kDisplaceSide = 256;

// Frames kept by one slit-scan.
constexpr auto kSlitBudget = int64(128) * 1024 * 1024;
constexpr auto kSlitMinFrames = 4;
constexpr auto kSlitMaxFrames = 96;

//
// Threading.
//

std::atomic<bool> SingleThreadForTests{ false };

[[nodiscard]] int ThreadCount() {
	static const auto result = std::clamp(
		int(std::thread::hardware_concurrency()),
		1,
		16);
	return SingleThreadForTests.load(std::memory_order_relaxed)
		? 1
		: result;
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

// Rows in bands of at least ~24K pixels.
template <typename Body>
void ForRows(int width, int height, const Body &body) {
	ParallelFor(height, std::max(1, 24576 / std::max(width, 1)), body);
}

//
// Pixels.
//

struct Buf {
	Pixel *data = nullptr;
	int width = 0;
	int height = 0;
	int stride = 0; // In pixels.

	[[nodiscard]] Pixel *row(int y) const {
		return data + ptrdiff_t(y) * stride;
	}
	[[nodiscard]] Pixel &at(int x, int y) const {
		return row(y)[x];
	}
	[[nodiscard]] bool empty() const {
		return !data || (width <= 0) || (height <= 0);
	}
};

[[nodiscard]] inline int PxA(Pixel p) {
	return int(p >> 24);
}

[[nodiscard]] inline int PxR(Pixel p) {
	return int((p >> 16) & 0xFFU);
}

[[nodiscard]] inline int PxG(Pixel p) {
	return int((p >> 8) & 0xFFU);
}

[[nodiscard]] inline int PxB(Pixel p) {
	return int(p & 0xFFU);
}

[[nodiscard]] inline Pixel Pack(int a, int r, int g, int b) {
	return (Pixel(a) << 24) | (Pixel(r) << 16) | (Pixel(g) << 8) | Pixel(b);
}

[[nodiscard]] inline Pixel Opaque(int r, int g, int b) {
	return Pack(255, r, g, b);
}

[[nodiscard]] inline int Clamp255(int v) {
	return (v < 0) ? 0 : (v > 255) ? 255 : v;
}

// NaN -> low.
[[nodiscard]] inline float ClampF(float v, float low, float high) {
	return (v > low) ? ((v < high) ? v : high) : low;
}

[[nodiscard]] inline int LumaOf(int r, int g, int b) {
	return (r * 77 + g * 150 + b * 29 + 128) >> 8;
}

[[nodiscard]] inline int Luma(Pixel p) {
	return LumaOf(PxR(p), PxG(p), PxB(p));
}

// a * b / 255, rounded.
[[nodiscard]] inline int Mul255(int a, int b) {
	const auto t = a * b + 128;
	return (t + (t >> 8)) >> 8;
}

// All four channels by t / 256, t in 0..256.
[[nodiscard]] inline Pixel Scale(Pixel p, uint32 t) {
	const auto rb = (((p & 0x00FF00FFU) * t) >> 8) & 0x00FF00FFU;
	const auto ag = (((p >> 8) & 0x00FF00FFU) * t) & 0xFF00FF00U;
	return rb | ag;
}

// The colours by t / 256, the alpha stays.
[[nodiscard]] inline Pixel ScaleColor(Pixel p, uint32 t) {
	return (Scale(p, t) & 0x00FFFFFFU) | (p & 0xFF000000U);
}

// a + (b - a) * t / 256, t in 0..256.
[[nodiscard]] inline Pixel Lerp(Pixel a, Pixel b, uint32 t) {
	const auto s = 256U - t;
	const auto rb = (((a & 0x00FF00FFU) * s + (b & 0x00FF00FFU) * t) >> 8)
		& 0x00FF00FFU;
	const auto ag = (((a >> 8) & 0x00FF00FFU) * s
		+ ((b >> 8) & 0x00FF00FFU) * t) & 0xFF00FF00U;
	return rb | ag;
}

// b shown over a with the opacity t / 256, t in 0..256: Lerp() where b is
// opaque, a itself where b is transparent. An opaque a stays opaque.
[[nodiscard]] inline Pixel LerpOver(Pixel a, Pixel b, uint32 t) {
	const auto s = 256U - (t * uint32(PxA(b))) / 255U;
	const auto rb = (((a & 0x00FF00FFU) * s + (b & 0x00FF00FFU) * t) >> 8)
		& 0x00FF00FFU;
	const auto ag = (((a >> 8) & 0x00FF00FFU) * s
		+ ((b >> 8) & 0x00FF00FFU) * t) & 0xFF00FF00U;
	return rb | ag;
}

[[nodiscard]] inline Pixel Premultiplied(Pixel opaque, int a) {
	return Pack(
		a,
		Mul255(PxR(opaque), a),
		Mul255(PxG(opaque), a),
		Mul255(PxB(opaque), a));
}

// map(r, g, b) gets the real colour of the pixel and gives an opaque one,
// the alpha of the pixel is kept.
template <typename Map>
[[nodiscard]] inline Pixel MapColor(Pixel p, Map &&map) {
	const auto a = PxA(p);
	if (a == 255) {
		return map(PxR(p), PxG(p), PxB(p));
	} else if (!a) {
		return 0;
	}
	return Premultiplied(
		map(
			std::min(PxR(p) * 255 / a, 255),
			std::min(PxG(p) * 255 / a, 255),
			std::min(PxB(p) * 255 / a, 255)),
		a);
}

// An opaque colour over an opaque or a transparent pixel: the alpha of
// the pixel is kept.
[[nodiscard]] inline Pixel WithAlphaOf(Pixel opaque, Pixel p) {
	const auto a = PxA(p);
	return (a == 255) ? opaque : a ? Premultiplied(opaque, a) : 0;
}

[[nodiscard]] inline Pixel ColorValue(float64 value) {
	return 0xFF000000U | (uint32(std::llround(value)) & 0x00FFFFFFU);
}

[[nodiscard]] inline Pixel MaxChannels(Pixel a, Pixel b) {
	return Pack(
		std::max(PxA(a), PxA(b)),
		std::max(PxR(a), PxR(b)),
		std::max(PxG(a), PxG(b)),
		std::max(PxB(a), PxB(b)));
}

// a + b - a * b: never darker than both, never brighter than white.
[[nodiscard]] inline Pixel ScreenChannels(Pixel a, Pixel b) {
	const auto screen = [](int x, int y) {
		return x + y - Mul255(x, y);
	};
	return Pack(
		screen(PxA(a), PxA(b)),
		screen(PxR(a), PxR(b)),
		screen(PxG(a), PxG(b)),
		screen(PxB(a), PxB(b)));
}

// Hue 0..360 (any real number), saturation and value 0..1.
[[nodiscard]] Pixel FromHsv(float hue, float saturation, float value) {
	hue = std::fmod(hue, 360.f);
	if (hue < 0.f) {
		hue += 360.f;
	}
	const auto sector = hue / 60.f;
	const auto index = int(sector) % 6;
	const auto f = sector - int(sector);
	const auto p = value * (1.f - saturation);
	const auto q = value * (1.f - saturation * f);
	const auto t = value * (1.f - saturation * (1.f - f));
	auto r = 0.f;
	auto g = 0.f;
	auto b = 0.f;
	switch (index) {
	case 0: r = value; g = t; b = p; break;
	case 1: r = q; g = value; b = p; break;
	case 2: r = p; g = value; b = t; break;
	case 3: r = p; g = q; b = value; break;
	case 4: r = t; g = p; b = value; break;
	default: r = value; g = p; b = q; break;
	}
	return Opaque(
		Clamp255(int(r * 255.f + 0.5f)),
		Clamp255(int(g * 255.f + 0.5f)),
		Clamp255(int(b * 255.f + 0.5f)));
}

// 0..359 and 0..255.
[[nodiscard]] inline int HueOf(int r, int g, int b) {
	const auto high = std::max({ r, g, b });
	const auto low = std::min({ r, g, b });
	const auto range = high - low;
	if (!range) {
		return 0;
	}
	auto hue = 0;
	if (high == r) {
		hue = 60 * (g - b) / range;
	} else if (high == g) {
		hue = 120 + 60 * (b - r) / range;
	} else {
		hue = 240 + 60 * (r - g) / range;
	}
	return (hue < 0) ? (hue + 360) : hue;
}

[[nodiscard]] inline int SaturationOf(int r, int g, int b) {
	const auto high = std::max({ r, g, b });
	const auto low = std::min({ r, g, b });
	return high ? ((high - low) * 255 / high) : 0;
}

// Pixel centers are at whole coordinates, the edge pixels are repeated.
[[nodiscard]] inline Pixel SampleClamped(const Buf &from, float x, float y) {
	x = ClampF(x, 0.f, float(from.width - 1));
	y = ClampF(y, 0.f, float(from.height - 1));
	const auto ix = int(x);
	const auto iy = int(y);
	const auto tx = uint32((x - ix) * 256.f);
	const auto ty = uint32((y - iy) * 256.f);
	const auto nx = std::min(ix + 1, from.width - 1);
	const auto top = from.row(iy);
	const auto bottom = from.row(std::min(iy + 1, from.height - 1));
	return Lerp(
		Lerp(top[ix], top[nx], tx),
		Lerp(bottom[ix], bottom[nx], tx),
		ty);
}

// Nothing outside of the picture.
[[nodiscard]] inline Pixel SampleOrNothing(const Buf &from, float x, float y) {
	return ((x > -0.5f)
		&& (y > -0.5f)
		&& (x < from.width - 0.5f)
		&& (y < from.height - 0.5f))
		? SampleClamped(from, x, y)
		: Pixel(0);
}

// The picture repeated as in a mirror, limit = size - 1.
[[nodiscard]] inline float MirrorCoord(float v, float limit) {
	if (!(limit > 0.f) || !(std::abs(v) < 1e7f)) {
		return 0.f;
	}
	v = std::abs(v);
	const auto period = 2.f * limit;
	if (v > period) {
		v = std::fmod(v, period);
	}
	return (v > limit) ? (period - v) : v;
}

// A compact copy (stride == width) in the given storage.
[[nodiscard]] Buf CopyOf(const Buf &frame, std::vector<Pixel> &storage) {
	const auto count = size_t(frame.width) * frame.height;
	if (storage.size() != count) {
		storage.resize(count);
	}
	auto result = Buf{
		.data = storage.data(),
		.width = frame.width,
		.height = frame.height,
		.stride = frame.width,
	};
	for (auto y = 0; y != frame.height; ++y) {
		memcpy(
			result.row(y),
			frame.row(y),
			size_t(frame.width) * sizeof(Pixel));
	}
	return result;
}

// Keeps the frame for the next one.
void Remember(const Buf &frame, std::vector<Pixel> &storage) {
	[[maybe_unused]] const auto copy = CopyOf(frame, storage);
}

[[nodiscard]] Buf BufOf(std::vector<Pixel> &storage, int width, int height) {
	return Buf{
		.data = storage.data(),
		.width = width,
		.height = height,
		.stride = width,
	};
}

//
// Random numbers that depend only on what they are made of.
//

[[nodiscard]] inline uint32 Hash(uint32 x) {
	x ^= x >> 16;
	x *= 0x7FEB352DU;
	x ^= x >> 15;
	x *= 0x846CA68BU;
	x ^= x >> 16;
	return x;
}

[[nodiscard]] inline uint32 Hash(uint32 a, uint32 b) {
	return Hash(a ^ (Hash(b) + 0x9E3779B9U + (a << 6) + (a >> 2)));
}

[[nodiscard]] inline uint32 Hash(uint32 a, uint32 b, uint32 c) {
	return Hash(Hash(a, b), c);
}

class Random final {
public:
	explicit Random(uint32 seed) : _state(Hash(seed) | 1U) {
	}

	[[nodiscard]] uint32 next() {
		_state ^= _state << 13;
		_state ^= _state >> 17;
		_state ^= _state << 5;
		return _state;
	}

	// [0, 1).
	[[nodiscard]] float unit() {
		return float(next() >> 8) * (1.f / 16777216.f);
	}

	// [-1, 1).
	[[nodiscard]] float spread() {
		return unit() * 2.f - 1.f;
	}

	// 0..count - 1.
	[[nodiscard]] int below(int count) {
		return (count > 1)
			? std::min(int(unit() * float(count)), count - 1)
			: 0;
	}

private:
	uint32 _state = 1;

};

// How many times something that happens `rate` times a second has
// happened by the position.
[[nodiscard]] inline uint32 TickAt(crl::time position, float64 rate) {
	return uint32(std::max(
		int64(std::floor(float64(position) * rate / 1000.)),
		int64(0)));
}

//
// A painted mono font: 5x7 dots, columns from left to right, the lowest
// bit is the top dot. Codes 32..95, small letters are shown as capitals.
//

constexpr auto kGlyphWidth = 5;
constexpr auto kGlyphHeight = 7;
constexpr auto kGlyphAdvance = 6;
constexpr auto kGlyphLine = 8;
constexpr auto kGlyphFirst = 32;
constexpr auto kGlyphCount = 64;

constexpr uchar kFont[kGlyphCount * kGlyphWidth] = {
	0x00, 0x00, 0x00, 0x00, 0x00, // space
	0x00, 0x00, 0x5F, 0x00, 0x00, // !
	0x00, 0x07, 0x00, 0x07, 0x00, // "
	0x14, 0x7F, 0x14, 0x7F, 0x14, // #
	0x24, 0x2A, 0x7F, 0x2A, 0x12, // $
	0x23, 0x13, 0x08, 0x64, 0x62, // %
	0x36, 0x49, 0x55, 0x22, 0x50, // &
	0x00, 0x05, 0x03, 0x00, 0x00, // '
	0x00, 0x1C, 0x22, 0x41, 0x00, // (
	0x00, 0x41, 0x22, 0x1C, 0x00, // )
	0x08, 0x2A, 0x1C, 0x2A, 0x08, // *
	0x08, 0x08, 0x3E, 0x08, 0x08, // +
	0x00, 0x50, 0x30, 0x00, 0x00, // ,
	0x08, 0x08, 0x08, 0x08, 0x08, // -
	0x00, 0x60, 0x60, 0x00, 0x00, // .
	0x20, 0x10, 0x08, 0x04, 0x02, // /
	0x3E, 0x51, 0x49, 0x45, 0x3E, // 0
	0x00, 0x42, 0x7F, 0x40, 0x00, // 1
	0x42, 0x61, 0x51, 0x49, 0x46, // 2
	0x21, 0x41, 0x45, 0x4B, 0x31, // 3
	0x18, 0x14, 0x12, 0x7F, 0x10, // 4
	0x27, 0x45, 0x45, 0x45, 0x39, // 5
	0x3C, 0x4A, 0x49, 0x49, 0x30, // 6
	0x01, 0x71, 0x09, 0x05, 0x03, // 7
	0x36, 0x49, 0x49, 0x49, 0x36, // 8
	0x06, 0x49, 0x49, 0x29, 0x1E, // 9
	0x00, 0x36, 0x36, 0x00, 0x00, // :
	0x00, 0x56, 0x36, 0x00, 0x00, // ;
	0x00, 0x08, 0x14, 0x22, 0x41, // <
	0x14, 0x14, 0x14, 0x14, 0x14, // =
	0x41, 0x22, 0x14, 0x08, 0x00, // >
	0x02, 0x01, 0x51, 0x09, 0x06, // ?
	0x32, 0x49, 0x79, 0x41, 0x3E, // @
	0x7E, 0x11, 0x11, 0x11, 0x7E, // A
	0x7F, 0x49, 0x49, 0x49, 0x36, // B
	0x3E, 0x41, 0x41, 0x41, 0x22, // C
	0x7F, 0x41, 0x41, 0x22, 0x1C, // D
	0x7F, 0x49, 0x49, 0x49, 0x41, // E
	0x7F, 0x09, 0x09, 0x01, 0x01, // F
	0x3E, 0x41, 0x41, 0x51, 0x32, // G
	0x7F, 0x08, 0x08, 0x08, 0x7F, // H
	0x00, 0x41, 0x7F, 0x41, 0x00, // I
	0x20, 0x40, 0x41, 0x3F, 0x01, // J
	0x7F, 0x08, 0x14, 0x22, 0x41, // K
	0x7F, 0x40, 0x40, 0x40, 0x40, // L
	0x7F, 0x02, 0x04, 0x02, 0x7F, // M
	0x7F, 0x04, 0x08, 0x10, 0x7F, // N
	0x3E, 0x41, 0x41, 0x41, 0x3E, // O
	0x7F, 0x09, 0x09, 0x09, 0x06, // P
	0x3E, 0x41, 0x51, 0x21, 0x5E, // Q
	0x7F, 0x09, 0x19, 0x29, 0x46, // R
	0x46, 0x49, 0x49, 0x49, 0x31, // S
	0x01, 0x01, 0x7F, 0x01, 0x01, // T
	0x3F, 0x40, 0x40, 0x40, 0x3F, // U
	0x1F, 0x20, 0x40, 0x20, 0x1F, // V
	0x7F, 0x20, 0x18, 0x20, 0x7F, // W
	0x63, 0x14, 0x08, 0x14, 0x63, // X
	0x03, 0x04, 0x78, 0x04, 0x03, // Y
	0x61, 0x51, 0x49, 0x45, 0x43, // Z
	0x00, 0x00, 0x7F, 0x41, 0x41, // [
	0x02, 0x04, 0x08, 0x10, 0x20, // backslash
	0x41, 0x41, 0x7F, 0x00, 0x00, // ]
	0x04, 0x02, 0x01, 0x02, 0x04, // ^
	0x40, 0x40, 0x40, 0x40, 0x40, // _
};

[[nodiscard]] inline const uchar *GlyphOf(char symbol) {
	auto code = int(uchar(symbol));
	if (code >= 'a' && code <= 'z') {
		code -= 'a' - 'A';
	}
	if (code < kGlyphFirst || code >= kGlyphFirst + kGlyphCount) {
		code = kGlyphFirst;
	}
	return kFont + (code - kGlyphFirst) * kGlyphWidth;
}

//
// Painting over a frame.
//

// A real (not premultiplied) colour with an opacity.
struct Ink {
	int r = 255;
	int g = 255;
	int b = 255;
	int a = 255;
};

[[nodiscard]] inline Ink InkOf(Pixel color, float opacity) {
	return Ink{
		.r = PxR(color),
		.g = PxG(color),
		.b = PxB(color),
		.a = Clamp255(int(ClampF(opacity, 0.f, 1.f) * 255.f + 0.5f)),
	};
}

// coverage: 0..256.
inline void BlendPixel(Pixel &to, const Ink &ink, int coverage) {
	const auto alpha = (ink.a * coverage) >> 8;
	if (alpha <= 0) {
		return;
	}
	const auto rest = 255 - alpha;
	to = Pack(
		alpha + Mul255(PxA(to), rest),
		Mul255(ink.r, alpha) + Mul255(PxR(to), rest),
		Mul255(ink.g, alpha) + Mul255(PxG(to), rest),
		Mul255(ink.b, alpha) + Mul255(PxB(to), rest));
}

[[nodiscard]] inline bool Paintable(float a, float b, float c, float d) {
	constexpr auto kLimit = 1e6f;
	return (std::abs(a) < kLimit)
		&& (std::abs(b) < kLimit)
		&& (std::abs(c) < kLimit)
		&& (std::abs(d) < kLimit);
}

// Pixel (x, y) is the square from (x, y) to (x + 1, y + 1), the edges of
// the box may be anywhere: a pixel gets as much as the box covers of it.
void FillBox(
		const Buf &to,
		float left,
		float top,
		float right,
		float bottom,
		const Ink &ink) {
	if (!Paintable(left, top, right, bottom)
		|| !(right > left)
		|| !(bottom > top)) {
		return;
	}
	const auto x0 = std::max(int(std::floor(left)), 0);
	const auto y0 = std::max(int(std::floor(top)), 0);
	const auto x1 = std::min(int(std::ceil(right)), to.width);
	const auto y1 = std::min(int(std::ceil(bottom)), to.height);
	for (auto y = y0; y < y1; ++y) {
		const auto cy = std::min(float(y + 1), bottom)
			- std::max(float(y), top);
		if (cy <= 0.f) {
			continue;
		}
		const auto row = to.row(y);
		for (auto x = x0; x < x1; ++x) {
			const auto cx = std::min(float(x + 1), right)
				- std::max(float(x), left);
			if (cx > 0.f) {
				BlendPixel(row[x], ink, int(cx * cy * 256.f + 0.5f));
			}
		}
	}
}

// The outline of a box, drawn inside of it.
void FrameBox(
		const Buf &to,
		float left,
		float top,
		float right,
		float bottom,
		float width,
		const Ink &ink) {
	if (!(right > left) || !(bottom > top)) {
		return;
	}
	width = std::min({ width, (right - left) / 2.f, (bottom - top) / 2.f });
	FillBox(to, left, top, right, top + width, ink);
	FillBox(to, left, bottom - width, right, bottom, ink);
	FillBox(to, left, top + width, left + width, bottom - width, ink);
	FillBox(to, right - width, top + width, right, bottom - width, ink);
}

void PaintLine(
		const Buf &to,
		float x0,
		float y0,
		float x1,
		float y1,
		float width,
		const Ink &ink) {
	if (!Paintable(x0, y0, x1, y1)) {
		return;
	}
	const auto half = std::max(width, 0.25f) / 2.f;
	auto dx = x1 - x0;
	auto dy = y1 - y0;
	const auto length = std::sqrt(dx * dx + dy * dy);
	if (length < 0.5f) {
		FillBox(to, x0 - half, y0 - half, x0 + half, y0 + half, ink);
		return;
	}
	if (std::abs(dx) >= std::abs(dy)) {
		if (x0 > x1) {
			std::swap(x0, x1);
			std::swap(y0, y1);
			dx = -dx;
			dy = -dy;
		}
		const auto slope = dy / dx;
		const auto reach = half * length / dx;
		const auto from = std::max(int(std::floor(x0)), 0);
		const auto till = std::min(int(std::ceil(x1)), to.width);
		for (auto x = from; x < till; ++x) {
			const auto cx = std::min(float(x + 1), x1)
				- std::max(float(x), x0);
			if (cx <= 0.f) {
				continue;
			}
			const auto middle = y0 + (float(x) + 0.5f - x0) * slope;
			const auto top = middle - reach;
			const auto bottom = middle + reach;
			const auto ya = std::max(int(std::floor(top)), 0);
			const auto yb = std::min(int(std::ceil(bottom)), to.height);
			for (auto y = ya; y < yb; ++y) {
				const auto cy = std::min(float(y + 1), bottom)
					- std::max(float(y), top);
				if (cy > 0.f) {
					BlendPixel(
						to.at(x, y),
						ink,
						int(cx * cy * 256.f + 0.5f));
				}
			}
		}
	} else {
		if (y0 > y1) {
			std::swap(x0, x1);
			std::swap(y0, y1);
			dx = -dx;
			dy = -dy;
		}
		const auto slope = dx / dy;
		const auto reach = half * length / dy;
		const auto from = std::max(int(std::floor(y0)), 0);
		const auto till = std::min(int(std::ceil(y1)), to.height);
		for (auto y = from; y < till; ++y) {
			const auto cy = std::min(float(y + 1), y1)
				- std::max(float(y), y0);
			if (cy <= 0.f) {
				continue;
			}
			const auto middle = x0 + (float(y) + 0.5f - y0) * slope;
			const auto left = middle - reach;
			const auto right = middle + reach;
			const auto xa = std::max(int(std::floor(left)), 0);
			const auto xb = std::min(int(std::ceil(right)), to.width);
			const auto row = to.row(y);
			for (auto x = xa; x < xb; ++x) {
				const auto cx = std::min(float(x + 1), right)
					- std::max(float(x), left);
				if (cx > 0.f) {
					BlendPixel(row[x], ink, int(cx * cy * 256.f + 0.5f));
				}
			}
		}
	}
}

[[nodiscard]] inline float LabelWidth(int symbols, float dot) {
	return (symbols > 0) ? ((symbols * kGlyphAdvance - 1) * dot) : 0.f;
}

[[nodiscard]] inline float LabelHeight(float dot) {
	return kGlyphHeight * dot;
}

// dot: the size of one dot of the font, any real number: every pixel is
// covered by the part of its four samples that hit the dots.
void PaintLabel(
		const Buf &to,
		float left,
		float top,
		const QByteArray &text,
		float dot,
		const Ink &ink) {
	const auto symbols = int(text.size());
	if (!symbols || !(dot > 0.05f) || !Paintable(left, top, dot, 0.f)) {
		return;
	}
	const auto columns = symbols * kGlyphAdvance - 1;
	const auto x0 = std::max(int(std::floor(left)), 0);
	const auto y0 = std::max(int(std::floor(top)), 0);
	const auto x1 = std::min(
		int(std::ceil(left + columns * dot)),
		to.width);
	const auto y1 = std::min(
		int(std::ceil(top + kGlyphHeight * dot)),
		to.height);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	const auto scale = 1.f / dot;

	// The column of the font under both samples of every pixel column.
	auto bits = std::vector<uchar>(size_t(x1 - x0) * 2, uchar(0));
	for (auto x = x0; x != x1; ++x) {
		for (auto sample = 0; sample != 2; ++sample) {
			const auto at = (float(x) + 0.25f + 0.5f * sample - left) * scale;
			if (at < 0.f) {
				continue;
			}
			const auto column = int(at);
			if (column >= columns) {
				continue;
			}
			const auto inside = column % kGlyphAdvance;
			if (inside < kGlyphWidth) {
				bits[size_t(x - x0) * 2 + sample] = GlyphOf(
					text[column / kGlyphAdvance])[inside];
			}
		}
	}
	for (auto y = y0; y != y1; ++y) {
		auto mask = std::array<int, 2>{ { -1, -1 } };
		for (auto sample = 0; sample != 2; ++sample) {
			const auto at = (float(y) + 0.25f + 0.5f * sample - top) * scale;
			if (at >= 0.f && int(at) < kGlyphHeight) {
				mask[sample] = int(at);
			}
		}
		if (mask[0] < 0 && mask[1] < 0) {
			continue;
		}
		const auto row = to.row(y);
		for (auto x = x0; x != x1; ++x) {
			const auto a = bits[size_t(x - x0) * 2];
			const auto b = bits[size_t(x - x0) * 2 + 1];
			if (!(a | b)) {
				continue;
			}
			auto hits = 0;
			for (const auto line : mask) {
				if (line >= 0) {
					hits += ((a >> line) & 1) + ((b >> line) & 1);
				}
			}
			if (hits) {
				BlendPixel(row[x], ink, hits * 64);
			}
		}
	}
}

inline void AppendNumber(QByteArray &to, int value, int digits) {
	value = std::max(value, 0);
	auto buffer = std::array<char, 12>{};
	auto length = 0;
	do {
		buffer[length++] = char('0' + (value % 10));
		value /= 10;
	} while (value && length < int(buffer.size()));
	for (auto i = length; i < digits; ++i) {
		to.append('0');
	}
	while (length) {
		to.append(buffer[--length]);
	}
}

//
// Planes of bytes: brightness, masks, maps.
//

struct Plane {
	int width = 0;
	int height = 0;
	std::vector<uchar> data;

	void resize(int w, int h) {
		width = w;
		height = h;
		data.resize(size_t(w) * h);
	}
	[[nodiscard]] uchar *row(int y) {
		return data.data() + size_t(y) * width;
	}
	[[nodiscard]] const uchar *row(int y) const {
		return data.data() + size_t(y) * width;
	}
	[[nodiscard]] bool empty() const {
		return data.empty();
	}
	[[nodiscard]] bool sameSize(const Plane &other) const {
		return (width == other.width) && (height == other.height);
	}
};

[[nodiscard]] QSize ReducedSize(int width, int height, int maxSide) {
	const auto longer = std::max(width, height);
	if (longer <= maxSide) {
		return QSize(width, height);
	}
	return QSize(
		std::max(int(int64(width) * maxSide / longer), 1),
		std::max(int(int64(height) * maxSide / longer), 1));
}

// The brightness of the frame fitted into maxSide x maxSide, every value
// is the average of up to 4x4 pixels spread over its block.
void ReduceLuma(const Buf &from, int maxSide, Plane &to) {
	const auto size = ReducedSize(from.width, from.height, maxSide);
	to.resize(size.width(), size.height());
	const auto w = to.width;
	const auto h = to.height;
	ParallelFor(h, std::max(1, 2048 / w), [&](int fromRow, int tillRow) {
		for (auto y = fromRow; y != tillRow; ++y) {
			const auto y0 = int(int64(y) * from.height / h);
			const auto y1 = std::max(
				int(int64(y + 1) * from.height / h),
				y0 + 1);
			const auto stepY = std::max((y1 - y0) / 4, 1);
			const auto line = to.row(y);
			for (auto x = 0; x != w; ++x) {
				const auto x0 = int(int64(x) * from.width / w);
				const auto x1 = std::max(
					int(int64(x + 1) * from.width / w),
					x0 + 1);
				const auto stepX = std::max((x1 - x0) / 4, 1);
				auto sum = 0;
				auto count = 0;
				for (auto sy = y0 + stepY / 2; sy < y1; sy += stepY) {
					const auto row = from.row(sy);
					for (auto sx = x0 + stepX / 2; sx < x1; sx += stepX) {
						sum += Luma(row[sx]);
						++count;
					}
				}
				line[x] = uchar(count ? (sum / count) : 0);
			}
		}
	});
}

// The same for the colours: a compact picture of to.width x to.height
// pixels in colors and its brightness in to.
void ReduceColor(
		const Buf &from,
		int maxSide,
		std::vector<Pixel> &colors,
		Plane &to) {
	const auto size = ReducedSize(from.width, from.height, maxSide);
	to.resize(size.width(), size.height());
	const auto w = to.width;
	const auto h = to.height;
	colors.resize(size_t(w) * h);
	ParallelFor(h, std::max(1, 2048 / w), [&](int fromRow, int tillRow) {
		for (auto y = fromRow; y != tillRow; ++y) {
			const auto y0 = int(int64(y) * from.height / h);
			const auto y1 = std::max(
				int(int64(y + 1) * from.height / h),
				y0 + 1);
			const auto stepY = std::max((y1 - y0) / 4, 1);
			const auto line = to.row(y);
			const auto pixels = colors.data() + size_t(y) * w;
			for (auto x = 0; x != w; ++x) {
				const auto x0 = int(int64(x) * from.width / w);
				const auto x1 = std::max(
					int(int64(x + 1) * from.width / w),
					x0 + 1);
				const auto stepX = std::max((x1 - x0) / 4, 1);
				auto r = 0;
				auto g = 0;
				auto b = 0;
				auto count = 0;
				for (auto sy = y0 + stepY / 2; sy < y1; sy += stepY) {
					const auto row = from.row(sy);
					for (auto sx = x0 + stepX / 2; sx < x1; sx += stepX) {
						const auto p = row[sx];
						r += PxR(p);
						g += PxG(p);
						b += PxB(p);
						++count;
					}
				}
				const auto color = count
					? Opaque(r / count, g / count, b / count)
					: Opaque(0, 0, 0);
				pixels[x] = color;
				line[x] = uchar(Luma(color));
			}
		}
	});
}

// A box blur of the given radius, the edges are repeated.
void BoxBlur(Plane &plane, int radius, std::vector<uchar> &temp) {
	if (radius <= 0 || plane.empty()) {
		return;
	}
	const auto w = plane.width;
	const auto h = plane.height;
	const auto window = 2 * radius + 1;
	temp.resize(plane.data.size());
	for (auto y = 0; y != h; ++y) {
		const auto from = plane.row(y);
		const auto to = temp.data() + size_t(y) * w;
		auto sum = 0;
		for (auto i = -radius; i <= radius; ++i) {
			sum += from[std::clamp(i, 0, w - 1)];
		}
		for (auto x = 0; x != w; ++x) {
			to[x] = uchar(sum / window);
			sum += from[std::min(x + radius + 1, w - 1)]
				- from[std::max(x - radius, 0)];
		}
	}
	auto sums = std::vector<int>(size_t(w), 0);
	for (auto i = -radius; i <= radius; ++i) {
		const auto from = temp.data() + size_t(std::clamp(i, 0, h - 1)) * w;
		for (auto x = 0; x != w; ++x) {
			sums[x] += from[x];
		}
	}
	for (auto y = 0; y != h; ++y) {
		const auto to = plane.row(y);
		const auto add = temp.data()
			+ size_t(std::min(y + radius + 1, h - 1)) * w;
		const auto remove = temp.data()
			+ size_t(std::max(y - radius, 0)) * w;
		for (auto x = 0; x != w; ++x) {
			to[x] = uchar(sums[x] / window);
			sums[x] += add[x] - remove[x];
		}
	}
}

// The average over a box of size x size pixels around every pixel, for
// any real size above one: the pixels at the ends of the box count by the
// part of them that is inside. The edges are repeated.
void BoxAverage(Plane &plane, float size, Plane &temp) {
	if (!(size > 1.f) || plane.empty()) {
		return;
	}
	const auto w = plane.width;
	const auto h = plane.height;
	const auto half = (size - 1.f) / 2.f;
	const auto radius = int(half);
	const auto part = int((half - float(radius)) * 256.f);
	if (!radius && !part) {
		return;
	}
	const auto total = (2 * radius + 1) * 256 + 2 * part;
	temp.resize(w, h);
	ForRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = plane.row(y);
			const auto to = temp.row(y);
			for (auto x = 0; x != w; ++x) {
				auto sum = 0;
				for (auto i = x - radius; i <= x + radius; ++i) {
					sum += line[std::clamp(i, 0, w - 1)];
				}
				const auto ends = line[std::max(x - radius - 1, 0)]
					+ line[std::min(x + radius + 1, w - 1)];
				to[x] = uchar((sum * 256 + ends * part + total / 2) / total);
			}
		}
	});
	ForRows(w, h, [&](int from, int till) {
		auto sums = std::vector<int>(size_t(w));
		for (auto y = from; y != till; ++y) {
			std::fill(sums.begin(), sums.end(), 0);
			for (auto i = y - radius; i <= y + radius; ++i) {
				const auto line = temp.row(std::clamp(i, 0, h - 1));
				for (auto x = 0; x != w; ++x) {
					sums[x] += line[x];
				}
			}
			const auto above = temp.row(std::max(y - radius - 1, 0));
			const auto below = temp.row(std::min(y + radius + 1, h - 1));
			const auto to = plane.row(y);
			for (auto x = 0; x != w; ++x) {
				const auto ends = above[x] + below[x];
				to[x] = uchar(
					(sums[x] * 256 + ends * part + total / 2) / total);
			}
		}
	});
}

// The same for all four channels of a compact picture.
void BoxBlur(
		std::vector<Pixel> &pixels,
		int w,
		int h,
		int radius,
		std::vector<Pixel> &temp) {
	if (radius <= 0 || pixels.empty()) {
		return;
	}
	const auto window = 2 * radius + 1;
	temp.resize(pixels.size());
	const auto pass = [&](
			const Pixel *from,
			Pixel *to,
			int count,
			int fromStep,
			int toStep) {
		auto a = 0;
		auto r = 0;
		auto g = 0;
		auto b = 0;
		for (auto i = -radius; i <= radius; ++i) {
			const auto p = from[size_t(std::clamp(i, 0, count - 1)) * fromStep];
			a += PxA(p);
			r += PxR(p);
			g += PxG(p);
			b += PxB(p);
		}
		for (auto i = 0; i != count; ++i) {
			to[size_t(i) * toStep] = Pack(
				a / window,
				r / window,
				g / window,
				b / window);
			const auto add = from[
				size_t(std::min(i + radius + 1, count - 1)) * fromStep];
			const auto remove = from[
				size_t(std::max(i - radius, 0)) * fromStep];
			a += PxA(add) - PxA(remove);
			r += PxR(add) - PxR(remove);
			g += PxG(add) - PxG(remove);
			b += PxB(add) - PxB(remove);
		}
	};
	ParallelFor(h, std::max(1, 4096 / w), [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			pass(
				pixels.data() + size_t(y) * w,
				temp.data() + size_t(y) * w,
				w,
				1,
				1);
		}
	});
	ParallelFor(w, std::max(1, 4096 / h), [&](int from, int till) {
		for (auto x = from; x != till; ++x) {
			pass(temp.data() + x, pixels.data() + x, h, w, w);
		}
	});
}

// Pixel centers are at whole coordinates, the edges are repeated.
[[nodiscard]] inline int SamplePlane(const Plane &plane, float x, float y) {
	x = ClampF(x, 0.f, float(plane.width - 1));
	y = ClampF(y, 0.f, float(plane.height - 1));
	const auto ix = int(x);
	const auto iy = int(y);
	const auto tx = int((x - ix) * 256.f);
	const auto ty = int((y - iy) * 256.f);
	const auto nx = std::min(ix + 1, plane.width - 1);
	const auto top = plane.row(iy);
	const auto bottom = plane.row(std::min(iy + 1, plane.height - 1));
	const auto a = top[ix] * (256 - tx) + top[nx] * tx;
	const auto b = bottom[ix] * (256 - tx) + bottom[nx] * tx;
	return (a * (256 - ty) + b * ty) >> 16;
}

//
// Effects.
//

// Buffers shared by the effects of one processor: an effect uses them
// only inside of one apply() call.
struct Scratch {
	std::vector<Pixel> copy;
	std::vector<Pixel> reduced;
	std::vector<Pixel> reducedTemp;
	Plane plane;
	Plane second;
	std::vector<uchar> bytes;
};

struct Context {
	const float64 *values = nullptr;
	crl::time position = 0;
	float step = kFrameMs; // Since the previous frame, in ms.
	float unit = 1.f; // Pixels of this frame in a pixel of a 720p one.
	Scratch *scratch = nullptr;

	[[nodiscard]] float value(int index) const {
		return float(values[index]);
	}
	[[nodiscard]] int integer(int index) const {
		return int(std::lround(values[index]));
	}
	[[nodiscard]] bool toggled(int index) const {
		return (values[index] >= 0.5);
	}
	[[nodiscard]] Pixel color(int index) const {
		return ColorValue(values[index]);
	}
	[[nodiscard]] float seconds() const {
		return float(float64(position) / 1000.);
	}
	[[nodiscard]] float stepSeconds() const {
		return step / 1000.f;
	}
};

class Effect {
public:
	virtual ~Effect() = default;

	// Forgets what was seen in the previous frames.
	virtual void reset() {
	}

	// The same when the effect is switched off for a while: big buffers
	// should not wait for it to be switched on again.
	virtual void release() {
		reset();
	}

	virtual void apply(const Context &context, const Buf &frame) = 0;

	virtual void collectBlobs(std::vector<Blob> &to) const {
	}

};

[[nodiscard]] Param SliderParam(
		const char *id,
		tr::phrase<> name,
		float64 min,
		float64 max,
		float64 value,
		float64 step = 1.,
		Unit unit = Unit::Plain) {
	auto result = Param();
	result.id = id;
	result.name = name;
	result.kind = ParamKind::Slider;
	result.unit = unit;
	result.min = min;
	result.max = max;
	result.value = value;
	result.step = step;
	return result;
}

[[nodiscard]] Param PercentParam(
		const char *id,
		tr::phrase<> name,
		float64 value,
		float64 min = 0.,
		float64 max = 100.) {
	return SliderParam(id, name, min, max, value, 1., Unit::Percent);
}

[[nodiscard]] Param IntegerParam(
		const char *id,
		tr::phrase<> name,
		int min,
		int max,
		int value) {
	auto result = SliderParam(id, name, min, max, value);
	result.kind = ParamKind::Integer;
	return result;
}

[[nodiscard]] Param ToggleParam(
		const char *id,
		tr::phrase<> name,
		bool value) {
	auto result = SliderParam(id, name, 0., 1., value ? 1. : 0.);
	result.kind = ParamKind::Toggle;
	return result;
}

[[nodiscard]] Param ChoiceParam(
		const char *id,
		tr::phrase<> name,
		std::vector<tr::phrase<>> options,
		int value = 0) {
	auto result = SliderParam(
		id,
		name,
		0.,
		float64(options.size()) - 1.,
		value);
	result.kind = ParamKind::Choice;
	result.options = std::move(options);
	return result;
}

[[nodiscard]] Param ColorParam(
		const char *id,
		tr::phrase<> name,
		uint32 color) {
	auto result = SliderParam(id, name, 0., float64(0xFFFFFF), color);
	result.kind = ParamKind::Color;
	return result;
}

[[nodiscard]] Param SeedParam() {
	auto result = SliderParam(
		"seed",
		tr::lng_oblivion_vfx_p_seed,
		0.,
		9999.,
		1.);
	result.kind = ParamKind::Seed;
	return result;
}

[[nodiscard]] Info MakeInfo(
		Type type,
		const char *id,
		tr::phrase<> name,
		Group group,
		bool temporal,
		std::vector<Param> params) {
	auto result = Info();
	result.type = type;
	result.id = id;
	result.name = name;
	result.group = group;
	result.temporal = temporal;
	result.params = std::move(params);
	return result;
}

//
// Tracking: blobs of motion, contrast or brightness are found on a small
// copy of the frame (a difference with a slowly following background,
// a threshold, connected components), matched with the blobs of the
// previous frame by the nearest predicted position, so every blob keeps
// its number, and drawn over the frame: boxes, lines between them,
// numbers and coordinates, trails.
//

class TrackingEffect final : public Effect {
public:
	enum : int {
		kSource,
		kSensitivity,
		kMinSize,
		kLimit,
		kStyle,
		kColor,
		kWidth,
		kLinks,
		kLabels,
		kTextSize,
		kTrails,
		kSmoothing,
		kDim,
		kParams,
	};
	enum : int {
		SourceAuto,
		SourceMotion,
		SourceContrast,
		SourceBright,
		SourceDark,
	};
	enum : int { StyleBoxes, StyleCorners, StylePoints, StyleInverted };
	enum : int { LinksOff, LinksNearest, LinksChain, LinksWeb };
	enum : int { LabelsOff, LabelsId, LabelsCoords, LabelsBoth };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Tracking,
			"tracking",
			tr::lng_oblivion_vfx_fx_tracking,
			Group::Analysis,
			true,
			{
				ChoiceParam("source", tr::lng_oblivion_vfx_p_source, {
					tr::lng_oblivion_vfx_o_auto,
					tr::lng_oblivion_vfx_o_motion,
					tr::lng_oblivion_vfx_o_contrast,
					tr::lng_oblivion_vfx_o_bright,
					tr::lng_oblivion_vfx_o_dark,
				}),
				PercentParam(
					"sensitivity",
					tr::lng_oblivion_vfx_p_sensitivity,
					55.),
				PercentParam(
					"min_size",
					tr::lng_oblivion_vfx_p_min_size,
					15.),
				IntegerParam("limit", tr::lng_oblivion_vfx_p_limit, 1, 40, 12),
				ChoiceParam("style", tr::lng_oblivion_vfx_p_style, {
					tr::lng_oblivion_vfx_o_boxes,
					tr::lng_oblivion_vfx_o_corners,
					tr::lng_oblivion_vfx_o_points,
					tr::lng_oblivion_vfx_o_inverted,
				}),
				ColorParam("color", tr::lng_oblivion_vfx_p_color, 0xFFFFFFU),
				SliderParam(
					"width",
					tr::lng_oblivion_vfx_p_width,
					0.5,
					6.,
					1.5,
					0.25),
				ChoiceParam("links", tr::lng_oblivion_vfx_p_links, {
					tr::lng_oblivion_vfx_o_off,
					tr::lng_oblivion_vfx_o_nearest,
					tr::lng_oblivion_vfx_o_chain,
					tr::lng_oblivion_vfx_o_web,
				}, LinksNearest),
				ChoiceParam("labels", tr::lng_oblivion_vfx_p_labels, {
					tr::lng_oblivion_vfx_o_off,
					tr::lng_oblivion_vfx_o_id,
					tr::lng_oblivion_vfx_o_coords,
					tr::lng_oblivion_vfx_o_id_coords,
				}, LabelsBoth),
				SliderParam(
					"text_size",
					tr::lng_oblivion_vfx_p_text_size,
					0.5,
					3.,
					1.,
					0.1),
				SliderParam(
					"trails",
					tr::lng_oblivion_vfx_p_trails,
					0.,
					3.,
					0.6,
					0.1,
					Unit::Seconds),
				PercentParam(
					"smoothing",
					tr::lng_oblivion_vfx_p_smoothing,
					40.),
				PercentParam("dim", tr::lng_oblivion_vfx_p_dim, 0.),
			});
	}

	void reset() override {
		_started = false;
		_tracks.clear();
		_nextId = 1;
	}

	void apply(const Context &context, const Buf &frame) override {
		analyse(context, frame);
		paint(context, frame);
	}

	void collectBlobs(std::vector<Blob> &to) const override {
		for (const auto &track : _tracks) {
			if (visible(track)) {
				to.push_back({
					.id = track.id,
					.box = QRectF(
						track.x - track.width / 2.,
						track.y - track.height / 2.,
						track.width,
						track.height),
				});
			}
		}
	}

private:
	// In pixels of the analysed copy.
	struct Found {
		float left = 0.f;
		float top = 0.f;
		float right = 0.f;
		float bottom = 0.f;
		float score = 0.f;
		bool taken = false;
	};
	struct TrailPoint {
		float x = 0.f;
		float y = 0.f;
		crl::time time = 0;
	};

	// In parts of the frame.
	struct Track {
		int id = 0; // Given when the blob is shown for the first time.
		float x = 0.f; // The center.
		float y = 0.f;
		float width = 0.f;
		float height = 0.f;
		float speedX = 0.f; // In a millisecond.
		float speedY = 0.f;
		float age = 0.f; // Ms.
		float lost = 0.f; // Ms since it was seen.
		int hits = 0;
		bool instant = false; // Was there in the very first frame.
		bool matched = false;
		std::vector<TrailPoint> trail;
	};
	struct Pair {
		float distance = 0.f;
		int track = 0;
		int found = 0;
	};

	// In how many milliseconds the background becomes twice closer to
	// the picture: where nothing differs from it, under the things that
	// move and where the difference doesn't move (something has stopped
	// there or has left that place).
	static constexpr auto kCalmHalfLife = 250.f;
	static constexpr auto kLiveHalfLife = 4000.f;
	static constexpr auto kDeadHalfLife = 100.f;

	enum : uchar {
		kMaskNothing,
		kMaskSet,
		kMaskLive,
		kMaskDead,
		kMaskSeen,
		kMaskKinds,
	};
	static constexpr auto kLostShown = 130.f;
	static constexpr auto kLostDropped = 260.f;
	static constexpr auto kFadeIn = 100.f;
	static constexpr auto kMaxFound = 96;
	static constexpr auto kMaxTrail = 200;

	[[nodiscard]] static bool visible(const Track &track) {
		return (track.instant || track.hits >= 2)
			&& (track.lost <= kLostShown);
	}

	void analyse(const Context &context, const Buf &frame) {
		ReduceColor(frame, kTrackSide, _colors, _gray);
		const auto w = _gray.width;
		const auto h = _gray.height;
		const auto count = size_t(w) * h;
		const auto first = !_started || (_previous.size() != count);
		if (first) {
			_previous = _colors;
			resetBackground();
			_front.resize(w, h);
			std::fill(_front.data.begin(), _front.data.end(), uchar(0));
			_frontCore.assign(count, uchar(0));
			_tracks.clear();
		}
		const auto step = first ? kFrameMs : context.step;
		const auto source = context.integer(kSource);
		const auto sensitivity = context.value(kSensitivity);
		const auto threshold = (source == SourceBright
			|| source == SourceDark)
			? int(250.f - sensitivity * 2.2f)
			: int(6.f + (100.f - sensitivity) * 0.6f);

		// _signal above the threshold is what the blobs are made of,
		// _core tells which of those pixels count for their boxes.
		_signal.resize(w, h);
		_core.assign(count, uchar(1));
		if (source == SourceAuto || source == SourceMotion) {
			subtract(threshold, step);
			_signal.data = _front.data;
			_core = _frontCore;
		} else if (source == SourceBright) {
			_signal.data = _gray.data;
			BoxBlur(_signal, 1, _temp);
		} else if (source == SourceDark) {
			for (auto i = size_t(0); i != count; ++i) {
				_signal.data[i] = uchar(255 - _gray.data[i]);
			}
			BoxBlur(_signal, 1, _temp);
		} else {
			std::fill(_signal.data.begin(), _signal.data.end(), uchar(0));
		}
		if (source == SourceAuto || source == SourceContrast) {
			// How much a place differs from what is around it.
			_mean = _gray;
			BoxBlur(_mean, std::max(std::min(w, h) / 16, 2), _temp);
			const auto weight = (source == SourceAuto) ? 0.5f : 1.f;
			_detail.resize(w, h);
			for (auto i = size_t(0); i != count; ++i) {
				const auto local = std::abs(
					int(_gray.data[i]) - int(_mean.data[i]));
				_detail.data[i] = uchar(std::min(int(local * weight), 255));
			}
			BoxBlur(_detail, 1, _temp);
			for (auto i = size_t(0); i != count; ++i) {
				const auto value = _detail.data[i];
				if (value > threshold) {
					_core[i] = 1;
				}
				_signal.data[i] = std::max(_signal.data[i], value);
			}
		}
		_previous = _colors;

		detect(threshold, context.value(kMinSize));
		associate(context, step, first);
		_started = true;
	}

	// What differs from the background (_front) and what of it counts
	// (_frontCore): only the pixels next to something that changes right
	// now. A thing of one colour changes only at its edges, the place
	// a thing has left doesn't change at all.
	//
	// The background follows the picture slowly under the things that
	// move (a blob with changes in it) and quickly where the difference
	// stays still: something has stopped there or has left that place.
	void subtract(int threshold, float step) {
		const auto w = _gray.width;
		const auto h = _gray.height;
		const auto count = size_t(w) * h;
		const auto moving = std::max(threshold / 2, 4);
		_moved.resize(w, h);
		auto changes = size_t(0);
		for (auto i = size_t(0); i != count; ++i) {
			const auto now = _colors[i];
			const auto was = _previous[i];
			const auto changed = std::max({
				std::abs(PxR(now) - PxR(was)),
				std::abs(PxG(now) - PxG(was)),
				std::abs(PxB(now) - PxB(was)),
			}) > moving;
			_moved.data[i] = changed ? 255 : 0;
			changes += changed ? 1 : 0;
		}
		if (!changes) {
			// The same frame once more (a paused video, a video with
			// repeated frames): what was found stays as it is.
			return;
		}
		for (auto i = size_t(0); i != count; ++i) {
			const auto now = _colors[i];
			const auto was = _background.data() + i * 3;
			_front.data[i] = uchar(std::min(
				int(std::max({
					std::abs(float(PxR(now)) - was[0]),
					std::abs(float(PxG(now)) - was[1]),
					std::abs(float(PxB(now)) - was[2]),
				})),
				255));
		}
		BoxBlur(_front, 1, _temp);

		// Not zero within this distance from every changed pixel.
		_near = _moved;
		BoxBlur(_near, std::max(std::min(w, h) / 36, 2), _temp);

		auto front = size_t(0);
		_mask.resize(count);
		for (auto i = size_t(0); i != count; ++i) {
			const auto set = (_front.data[i] > threshold);
			_mask[i] = set ? kMaskSet : kMaskNothing;
			front += set ? 1 : 0;
		}
		if (front * 20 > count * 9) {
			// Almost everything has changed: a cut or a camera move.
			resetBackground();
			std::fill(_front.data.begin(), _front.data.end(), uchar(0));
			return;
		}
		for (auto start = size_t(0); start != count; ++start) {
			if (_mask[start] != kMaskSet) {
				continue;
			}
			auto changed = 0;
			_members.clear();
			_queue.clear();
			_queue.push_back(int(start));
			_mask[start] = kMaskSeen;
			while (!_queue.empty()) {
				const auto index = _queue.back();
				_queue.pop_back();
				_members.push_back(index);
				changed += _moved.data[index] ? 1 : 0;
				const auto x = index % w;
				const auto y = index / w;
				for (auto ny = std::max(y - 1, 0);
					ny <= std::min(y + 1, h - 1);
					++ny) {
					for (auto nx = std::max(x - 1, 0);
						nx <= std::min(x + 1, w - 1);
						++nx) {
						const auto next = ny * w + nx;
						if (_mask[next] == kMaskSet) {
							_mask[next] = kMaskSeen;
							_queue.push_back(next);
						}
					}
				}
			}
			const auto kind = (changed >= 2) ? kMaskLive : kMaskDead;
			for (const auto index : _members) {
				_mask[index] = kind;
			}
		}
		const auto rate = [&](float halfLife) {
			return 1.f - std::pow(0.5f, step / halfLife);
		};
		auto rates = std::array<float, kMaskKinds>();
		rates[kMaskNothing] = rate(kCalmHalfLife);
		rates[kMaskLive] = rate(kLiveHalfLife);
		rates[kMaskDead] = rate(kDeadHalfLife);
		for (auto i = size_t(0); i != count; ++i) {
			const auto kind = _mask[i];
			const auto now = _colors[i];
			const auto was = _background.data() + i * 3;
			was[0] += (float(PxR(now)) - was[0]) * rates[kind];
			was[1] += (float(PxG(now)) - was[1]) * rates[kind];
			was[2] += (float(PxB(now)) - was[2]) * rates[kind];
			if (kind == kMaskDead) {
				_front.data[i] = 0;
			}
			_frontCore[i] = _near.data[i] ? 1 : 0;
		}
	}

	void resetBackground() {
		const auto count = _colors.size();
		_background.resize(count * 3);
		for (auto i = size_t(0); i != count; ++i) {
			const auto now = _colors[i];
			_background[i * 3] = float(PxR(now));
			_background[i * 3 + 1] = float(PxG(now));
			_background[i * 3 + 2] = float(PxB(now));
		}
	}

	void detect(int threshold, float minSize) {
		const auto w = _signal.width;
		const auto h = _signal.height;
		const auto count = size_t(w) * h;

		// Blobs that almost touch are one blob.
		_mask.assign(count, uchar(kMaskNothing));
		for (auto y = 0; y != h; ++y) {
			const auto line = _signal.row(y);
			for (auto x = 0; x != w; ++x) {
				if (line[x] <= threshold) {
					continue;
				}
				for (auto ny = std::max(y - 1, 0);
					ny <= std::min(y + 1, h - 1);
					++ny) {
					const auto to = _mask.data() + size_t(ny) * w;
					for (auto nx = std::max(x - 1, 0);
						nx <= std::min(x + 1, w - 1);
						++nx) {
						to[nx] = kMaskSet;
					}
				}
			}
		}

		const auto part = minSize / 100.f;
		const auto minArea = 3 + int(part * part * float(count) * 0.03f);
		const auto maxArea = float(count) * 0.6f;
		_found.clear();
		for (auto start = size_t(0); start != count; ++start) {
			if (_mask[start] != kMaskSet) {
				continue;
			}
			auto left = w;
			auto top = h;
			auto right = -1;
			auto bottom = -1;
			auto area = 0;
			auto strength = 0;
			_mask[start] = kMaskSeen;
			_queue.clear();
			_queue.push_back(int(start));
			while (!_queue.empty()) {
				const auto index = _queue.back();
				_queue.pop_back();
				const auto x = index % w;
				const auto y = index / w;
				const auto value = int(_signal.data[index]);
				if (value > threshold && _core[index]) {
					left = std::min(left, x);
					right = std::max(right, x);
					top = std::min(top, y);
					bottom = std::max(bottom, y);
					++area;
					strength += value - threshold;
				}
				for (auto ny = std::max(y - 1, 0);
					ny <= std::min(y + 1, h - 1);
					++ny) {
					for (auto nx = std::max(x - 1, 0);
						nx <= std::min(x + 1, w - 1);
						++nx) {
						const auto next = ny * w + nx;
						if (_mask[next] == kMaskSet) {
							_mask[next] = kMaskSeen;
							_queue.push_back(next);
						}
					}
				}
			}
			const auto boxWidth = right - left + 1;
			const auto boxHeight = bottom - top + 1;
			if (area < minArea
				|| float(boxWidth) * float(boxHeight) > maxArea) {
				continue;
			}

			// The smoothing of the signal has made it half a pixel wider.
			const auto shrinkX = (boxWidth > 3) ? 0.5f : 0.f;
			const auto shrinkY = (boxHeight > 3) ? 0.5f : 0.f;
			_found.push_back({
				.left = float(left) + shrinkX,
				.top = float(top) + shrinkY,
				.right = float(right + 1) - shrinkX,
				.bottom = float(bottom + 1) - shrinkY,
				.score = float(area) + float(strength) / 64.f,
			});
		}
		std::stable_sort(
			_found.begin(),
			_found.end(),
			[](const Found &a, const Found &b) {
				return a.score > b.score;
			});
		if (int(_found.size()) > kMaxFound) {
			_found.resize(kMaxFound);
		}
	}

	void associate(const Context &context, float step, bool first) {
		const auto w = float(_signal.width);
		const auto h = float(_signal.height);
		const auto limit = context.integer(kLimit);
		const auto smoothing = context.value(kSmoothing) / 100.f;
		const auto follow = 1.f - std::pow(
			smoothing * 0.85f,
			step / kFrameMs);
		const auto trails = crl::time(context.value(kTrails) * 1000.f);

		_pairs.clear();
		for (auto i = 0; i != int(_tracks.size()); ++i) {
			auto &track = _tracks[i];
			track.matched = false;
			const auto guessX = track.x + track.speedX * step;
			const auto guessY = track.y + track.speedY * step;
			const auto trackW = track.width * w;
			const auto trackH = track.height * h;
			const auto moved = std::sqrt(
				track.speedX * w * track.speedX * w
					+ track.speedY * h * track.speedY * h) * step;
			for (auto j = 0; j != int(_found.size()); ++j) {
				const auto &found = _found[j];
				const auto foundW = found.right - found.left;
				const auto foundH = found.bottom - found.top;
				const auto ratio = (foundW * foundH)
					/ std::max(trackW * trackH, 1.f);
				if (ratio > 6.f || ratio < 1.f / 6.f) {
					continue;
				}
				const auto dx = (found.left + found.right) / 2.f - guessX * w;
				const auto dy = (found.top + found.bottom) / 2.f - guessY * h;
				const auto distance = std::sqrt(dx * dx + dy * dy);
				const auto gate = std::max(
					0.6f * std::max({ trackW, trackH, foundW, foundH }),
					5.f) + moved;
				if (distance <= gate) {
					_pairs.push_back({
						.distance = distance,
						.track = i,
						.found = j,
					});
				}
			}
		}
		std::sort(
			_pairs.begin(),
			_pairs.end(),
			[](const Pair &a, const Pair &b) {
				return (a.distance != b.distance)
					? (a.distance < b.distance)
					: (a.track != b.track)
					? (a.track < b.track)
					: (a.found < b.found);
			});
		for (const auto &pair : _pairs) {
			auto &track = _tracks[pair.track];
			auto &found = _found[pair.found];
			if (track.matched || found.taken) {
				continue;
			}
			track.matched = true;
			found.taken = true;

			const auto guessX = track.x + track.speedX * step;
			const auto guessY = track.y + track.speedY * step;
			const auto seenX = (found.left + found.right) / 2.f / w;
			const auto seenY = (found.top + found.bottom) / 2.f / h;
			const auto x = guessX + (seenX - guessX) * follow;
			const auto y = guessY + (seenY - guessY) * follow;
			track.speedX += ((x - track.x) / step - track.speedX) * 0.5f;
			track.speedY += ((y - track.y) / step - track.speedY) * 0.5f;
			track.x = x;
			track.y = y;
			const auto resize = follow * 0.6f;
			track.width += ((found.right - found.left) / w - track.width)
				* resize;
			track.height += ((found.bottom - found.top) / h - track.height)
				* resize;
			track.age += step;
			track.lost = 0.f;
			++track.hits;
		}
		for (auto &track : _tracks) {
			if (!track.matched) {
				track.age += step;
				track.lost += step;
				track.speedX *= 0.5f;
				track.speedY *= 0.5f;
			}
		}
		_tracks.erase(
			std::remove_if(
				_tracks.begin(),
				_tracks.end(),
				[](const Track &track) {
					return (track.lost > kLostDropped);
				}),
			_tracks.end());
		if (int(_tracks.size()) > limit) {
			// The limit was lowered: the youngest ones go.
			_tracks.resize(limit);
		}
		for (const auto &found : _found) {
			if (int(_tracks.size()) >= limit) {
				break;
			} else if (found.taken) {
				continue;
			}
			auto track = Track();
			track.x = (found.left + found.right) / 2.f / w;
			track.y = (found.top + found.bottom) / 2.f / h;
			track.width = (found.right - found.left) / w;
			track.height = (found.bottom - found.top) / h;
			track.instant = first;
			track.matched = true;
			track.hits = 1;
			_tracks.push_back(std::move(track));
		}
		for (auto &track : _tracks) {
			if (!track.id && (track.instant || track.hits >= 2)) {
				// Blobs that are seen only once don't take the numbers.
				track.id = _nextId++;
			}
			if (track.matched && trails > 0) {
				track.trail.push_back({
					.x = track.x,
					.y = track.y,
					.time = context.position,
				});
			}
			auto skip = 0;
			const auto size = int(track.trail.size());
			while (skip < size
				&& ((context.position - track.trail[skip].time > trails)
					|| (size - skip > kMaxTrail))) {
				++skip;
			}
			if (skip) {
				track.trail.erase(
					track.trail.begin(),
					track.trail.begin() + skip);
			}
		}
	}

	struct Shown {
		const Track *track = nullptr;
		float left = 0.f; // In pixels of the frame.
		float top = 0.f;
		float right = 0.f;
		float bottom = 0.f;
		float x = 0.f;
		float y = 0.f;
		float opacity = 1.f;
	};

	// The part of the segment between the two boxes, false if nothing
	// of it is outside of them.
	[[nodiscard]] static bool Between(
			const Shown &a,
			const Shown &b,
			float &x0,
			float &y0,
			float &x1,
			float &y1) {
		const auto dx = b.x - a.x;
		const auto dy = b.y - a.y;
		const auto leave = [&](const Shown &box, float sign) {
			// How far from the center of the box along the segment
			// its edge is, in parts of the segment.
			auto result = 1e9f;
			if (std::abs(dx) > 1e-3f) {
				const auto edge = (dx * sign > 0.f) ? box.right : box.left;
				result = std::min(result, (edge - box.x) / (dx * sign));
			}
			if (std::abs(dy) > 1e-3f) {
				const auto edge = (dy * sign > 0.f) ? box.bottom : box.top;
				result = std::min(result, (edge - box.y) / (dy * sign));
			}
			return result;
		};
		const auto from = leave(a, 1.f);
		const auto till = 1.f - leave(b, -1.f);
		if (!(from < till)) {
			return false;
		}
		x0 = a.x + dx * from;
		y0 = a.y + dy * from;
		x1 = a.x + dx * till;
		y1 = a.y + dy * till;
		return true;
	}

	void paint(const Context &context, const Buf &frame) {
		const auto dim = context.value(kDim) / 100.f;
		if (dim > 0.f) {
			const auto keep = uint32((1.f - dim * 0.85f) * 256.f);
			ForRows(frame.width, frame.height, [&](int from, int till) {
				for (auto y = from; y != till; ++y) {
					const auto row = frame.row(y);
					for (auto x = 0; x != frame.width; ++x) {
						row[x] = ScaleColor(row[x], keep);
					}
				}
			});
		}

		const auto w = float(frame.width);
		const auto h = float(frame.height);
		const auto unit = context.unit;
		const auto style = context.integer(kStyle);
		const auto color = context.color(kColor);
		const auto width = std::max(context.value(kWidth) * unit, 0.6f);
		const auto margin = 2.f * unit;
		const auto least = 5.f * unit;

		_shown.clear();
		for (const auto &track : _tracks) {
			if (!visible(track)) {
				continue;
			}
			const auto halfW = std::max(track.width * w / 2.f + margin, least);
			const auto halfH = std::max(track.height * h / 2.f + margin, least);
			const auto x = track.x * w;
			const auto y = track.y * h;
			const auto appear = track.instant
				? 1.f
				: ClampF(track.age / kFadeIn, 0.35f, 1.f);
			const auto vanish = 1.f - ClampF(
				track.lost / kLostDropped,
				0.f,
				1.f);

			// Whole pixels: the lines of a box stay sharp, in a small
			// preview and in a sticker as well. The size is rounded on its
			// own, a box that only moves doesn't change it.
			const auto left = std::round(x - halfW);
			const auto top = std::round(y - halfH);
			_shown.push_back({
				.track = &track,
				.left = left,
				.top = top,
				.right = left + std::max(std::round(2.f * halfW), 1.f),
				.bottom = top + std::max(std::round(2.f * halfH), 1.f),
				.x = x,
				.y = y,
				.opacity = appear * vanish,
			});
		}
		const auto count = int(_shown.size());

		// Trails.
		const auto trails = context.value(kTrails) * 1000.f;
		if (trails > 0.f) {
			for (const auto &shown : _shown) {
				const auto &trail = shown.track->trail;
				for (auto i = 1; i < int(trail.size()); ++i) {
					const auto age = float(context.position - trail[i].time);
					const auto fade = 1.f - ClampF(age / trails, 0.f, 1.f);
					PaintLine(
						frame,
						trail[i - 1].x * w,
						trail[i - 1].y * h,
						trail[i].x * w,
						trail[i].y * h,
						width * 0.8f,
						InkOf(color, 0.75f * fade * shown.opacity));
				}
			}
		}

		// Lines between the blobs.
		const auto links = context.integer(kLinks);
		const auto link = [&](int i, int j, float opacity) {
			const auto &a = _shown[i];
			const auto &b = _shown[j];
			auto x0 = a.x;
			auto y0 = a.y;
			auto x1 = b.x;
			auto y1 = b.y;
			if (style != StylePoints && !Between(a, b, x0, y0, x1, y1)) {
				return;
			}
			PaintLine(
				frame,
				x0,
				y0,
				x1,
				y1,
				width * 0.6f,
				InkOf(color, opacity * std::min(a.opacity, b.opacity)));
		};
		const auto distance = [&](int i, int j) {
			const auto dx = _shown[i].x - _shown[j].x;
			const auto dy = _shown[i].y - _shown[j].y;
			return dx * dx + dy * dy;
		};
		if (links == LinksChain) {
			for (auto i = 1; i < count; ++i) {
				link(i - 1, i, 0.7f);
			}
		} else if (links == LinksNearest) {
			_nearest.assign(count, -1);
			for (auto i = 0; i != count; ++i) {
				auto best = 0.f;
				for (auto j = 0; j != count; ++j) {
					if (j == i) {
						continue;
					}
					const auto value = distance(i, j);
					if (_nearest[i] < 0 || value < best) {
						_nearest[i] = j;
						best = value;
					}
				}
			}
			for (auto i = 0; i != count; ++i) {
				const auto j = _nearest[i];
				if (j >= 0 && (i < j || _nearest[j] != i)) {
					link(i, j, 0.7f);
				}
			}
		} else if (links == LinksWeb) {
			const auto reach = 0.16f * (w * w + h * h);
			for (auto i = 0; i != count; ++i) {
				for (auto j = i + 1; j != count; ++j) {
					if (count <= 6 || distance(i, j) < reach) {
						link(i, j, 0.5f);
					}
				}
			}
		}

		// Boxes and labels.
		const auto labels = context.integer(kLabels);

		// A dot of the font is two pixels of a 720p frame, three of a 1080p
		// one and one of a playing preview: whole pixels, sharp digits.
		const auto dot = context.value(kTextSize) * unit * 2.f;
		const auto pad = 2.f * dot;
		const auto textHeight = LabelHeight(dot);
		const auto dark = (Luma(color) > 140);
		const auto contrast = dark ? Opaque(0, 0, 0) : Opaque(255, 255, 255);

		// What is put under the text that has no tag of its own, so it is
		// read over a bright sky and over a crowd as well.
		const auto shade = (Luma(color) > 72)
			? Opaque(0, 0, 0)
			: Opaque(255, 255, 255);
		for (const auto &shown : _shown) {
			const auto ink = InkOf(color, shown.opacity);
			const auto boxWidth = shown.right - shown.left;
			const auto boxHeight = shown.bottom - shown.top;
			if (style == StyleInverted) {
				invert(frame, shown);
				FrameBox(
					frame,
					shown.left,
					shown.top,
					shown.right,
					shown.bottom,
					width * 0.75f,
					ink);
			} else if (style == StyleBoxes) {
				FrameBox(
					frame,
					shown.left,
					shown.top,
					shown.right,
					shown.bottom,
					width,
					ink);
			} else if (style == StyleCorners) {
				const auto arm = ClampF(
					0.28f * std::min(boxWidth, boxHeight),
					4.f * unit,
					40.f * unit);
				for (const auto right : { false, true }) {
					for (const auto bottom : { false, true }) {
						const auto x = right ? shown.right : shown.left;
						const auto y = bottom ? shown.bottom : shown.top;
						const auto sx = right ? -1.f : 1.f;
						const auto sy = bottom ? -1.f : 1.f;
						FillBox(
							frame,
							std::min(x, x + sx * arm),
							std::min(y, y + sy * width),
							std::max(x, x + sx * arm),
							std::max(y, y + sy * width),
							ink);
						FillBox(
							frame,
							std::min(x, x + sx * width),
							std::min(y + sy * width, y + sy * arm),
							std::max(x, x + sx * width),
							std::max(y + sy * width, y + sy * arm),
							ink);
					}
				}
				const auto cross = 3.f * unit + width;
				const auto thin = InkOf(color, 0.8f * shown.opacity);
				FillBox(
					frame,
					shown.x - cross,
					shown.y - width * 0.3f,
					shown.x + cross,
					shown.y + width * 0.3f,
					thin);
				FillBox(
					frame,
					shown.x - width * 0.3f,
					shown.y - cross,
					shown.x + width * 0.3f,
					shown.y + cross,
					thin);
			} else {
				const auto half = std::max(2.f * width, 1.5f * unit);
				FillBox(
					frame,
					shown.x - half,
					shown.y - half,
					shown.x + half,
					shown.y + half,
					ink);
				FrameBox(
					frame,
					shown.x - half * 2.2f,
					shown.y - half * 2.2f,
					shown.x + half * 2.2f,
					shown.y + half * 2.2f,
					width * 0.5f,
					InkOf(color, 0.6f * shown.opacity));
			}
			if (labels == LabelsOff) {
				continue;
			}

			auto number = QByteArray();
			if (labels != LabelsCoords) {
				AppendNumber(number, shown.track->id % 1000, 2);
			}
			auto place = QByteArray();
			if (labels != LabelsId) {
				place.append("X ");
				AppendNumber(
					place,
					int(ClampF(shown.track->x, 0.f, 1.f) * 999.f + 0.5f),
					3);
				place.append(" Y ");
				AppendNumber(
					place,
					int(ClampF(shown.track->y, 0.f, 1.f) * 999.f + 0.5f),
					3);
			}
			const auto numberWidth = LabelWidth(int(number.size()), dot);
			const auto placeWidth = LabelWidth(int(place.size()), dot);
			const auto tagged = (style == StyleBoxes)
				|| (style == StyleInverted);
			const auto tagWidth = number.isEmpty()
				? 0.f
				: (numberWidth + 2.f * pad);
			const auto lineHeight = textHeight + 2.f * pad;
			const auto whole = tagWidth
				+ (place.isEmpty() ? 0.f : (placeWidth + 2.f * pad));

			// Above the box, under it when there is no place above,
			// to the right of a point.
			auto left = shown.left;
			auto top = shown.top - lineHeight;
			if (style == StylePoints) {
				const auto half = std::max(2.f * width, 1.5f * unit) * 2.2f;
				left = shown.x + half + pad;
				top = shown.y - lineHeight / 2.f;
				if (left + whole > w && shown.x - half - pad - whole >= 0.f) {
					left = shown.x - half - pad - whole;
				}
			} else {
				if (style == StyleCorners) {
					top = shown.bottom + pad * 0.5f;
					if (top + lineHeight > h) {
						top = shown.top - lineHeight - pad * 0.5f;
					}
				} else if (top < 0.f) {
					top = shown.bottom;
				}
				left = std::min(left, w - whole);
			}

			// Whole pixels for the dots of the font. A label that stands
			// on its box or hangs under it leaves no gap between them.
			left = std::max(std::floor(left), 0.f);
			top = ClampF(
				(top < shown.top) ? std::ceil(top) : std::floor(top),
				0.f,
				std::max(h - lineHeight, 0.f));

			const auto covered = (tagged && !number.isEmpty())
				? tagWidth
				: 0.f;
			if (whole > covered) {
				FillBox(
					frame,
					left + covered,
					top,
					left + whole,
					top + lineHeight,
					InkOf(shade, 0.55f * shown.opacity));
			}
			if (!number.isEmpty()) {
				if (tagged) {
					FillBox(
						frame,
						left,
						top,
						left + tagWidth,
						top + lineHeight,
						ink);
					PaintLabel(
						frame,
						left + pad,
						top + pad,
						number,
						dot,
						InkOf(contrast, shown.opacity));
				} else {
					PaintLabel(frame, left + pad, top + pad, number, dot, ink);
				}
			}
			if (!place.isEmpty()) {
				PaintLabel(
					frame,
					left + tagWidth + pad,
					top + pad,
					place,
					dot,
					ink);
			}
		}
	}

	static void invert(const Buf &frame, const Shown &shown) {
		const auto x0 = std::max(int(std::lround(shown.left)), 0);
		const auto y0 = std::max(int(std::lround(shown.top)), 0);
		const auto x1 = std::min(int(std::lround(shown.right)), frame.width);
		const auto y1 = std::min(int(std::lround(shown.bottom)), frame.height);
		for (auto y = y0; y < y1; ++y) {
			const auto row = frame.row(y);
			for (auto x = x0; x < x1; ++x) {
				const auto p = row[x];
				const auto a = PxA(p);
				row[x] = Pack(a, a - PxR(p), a - PxG(p), a - PxB(p));
			}
		}
	}

	Plane _gray;
	Plane _signal;
	Plane _mean;
	Plane _detail;
	Plane _moved;
	Plane _near;
	Plane _front;
	std::vector<Pixel> _colors; // The small copy of the frame.
	std::vector<Pixel> _previous; // The same of the previous frame.
	std::vector<float> _background; // Three channels for every pixel.
	std::vector<uchar> _temp;
	std::vector<uchar> _mask;
	std::vector<uchar> _core;
	std::vector<uchar> _frontCore;
	std::vector<int> _queue;
	std::vector<int> _members;
	std::vector<Found> _found;
	std::vector<Pair> _pairs;
	std::vector<Track> _tracks;
	std::vector<Shown> _shown;
	std::vector<int> _nearest;
	int _nextId = 1;
	bool _started = false;

};

//
// Edges: a Sobel filter over the brightness, the edges are coloured and
// get a glow (a blurred small copy of them added on top).
//

class EdgesEffect final : public Effect {
public:
	enum : int {
		kThreshold,
		kAmount,
		kGlow,
		kColors,
		kColor,
		kOriginal,
		kParams,
	};
	enum : int { ColorsTint, ColorsSource, ColorsRainbow };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Edges,
			"edges",
			tr::lng_oblivion_vfx_fx_edges,
			Group::Analysis,
			false,
			{
				PercentParam(
					"threshold",
					tr::lng_oblivion_vfx_p_threshold,
					15.),
				PercentParam(
					"amount",
					tr::lng_oblivion_vfx_p_amount,
					150.,
					0.,
					300.),
				PercentParam("glow", tr::lng_oblivion_vfx_p_glow, 50.),
				ChoiceParam("colors", tr::lng_oblivion_vfx_p_colors, {
					tr::lng_oblivion_vfx_o_tint,
					tr::lng_oblivion_vfx_o_source,
					tr::lng_oblivion_vfx_o_rainbow,
				}),
				ColorParam("color", tr::lng_oblivion_vfx_p_color, 0x35E0FFU),
				PercentParam(
					"original",
					tr::lng_oblivion_vfx_p_original,
					0.),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		constexpr auto kShrink = 4;

		const auto w = frame.width;
		const auto h = frame.height;
		auto &scratch = *context.scratch;
		auto &luma = scratch.plane;
		luma.resize(w, h);
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				const auto line = luma.row(y);
				for (auto x = 0; x != w; ++x) {
					line[x] = uchar(Luma(row[x]));
				}
			}
		});

		// A frame bigger than a 720p one is looked at the way its 720p
		// copy would be: the brightness is averaged over a pixel of that
		// copy and the filter reaches as far as its neighbours are, a mix
		// of the two whole distances around the real one. So the lines
		// are as thick and as bright in a big export as in the preview.
		const auto scale = std::max(context.unit, 1.f);
		const auto reach = int(scale);
		const auto further = int((scale - float(reach)) * 256.f);
		BoxAverage(luma, scale, scratch.second);

		// A smaller frame can't have thinner lines, and everything in it
		// is sharper: weaker lines give about as much light as the lines
		// of the big frame do.
		const auto gain = int(context.value(kAmount)
			/ 100.f
			* 96.f
			* std::sqrt(ClampF(context.unit, 0.25f, 1.f)));
		const auto threshold = int(context.value(kThreshold) * 1.5f);
		const auto colors = context.integer(kColors);
		const auto tint = context.color(kColor);
		auto rainbow = std::array<Pixel, 256>();
		if (colors == ColorsRainbow) {
			for (auto i = 0; i != 256; ++i) {
				rainbow[i] = FromHsv(i * 360.f / 256.f, 0.9f, 1.f);
			}
		}
		scratch.copy.resize(size_t(w) * h);
		const auto edges = BufOf(scratch.copy, w, h);
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto up = luma.row(std::max(y - reach, 0));
				const auto mid = luma.row(y);
				const auto down = luma.row(std::min(y + reach, h - 1));
				const auto upper = luma.row(std::max(y - reach - 1, 0));
				const auto lower = luma.row(std::min(y + reach + 1, h - 1));
				const auto row = frame.row(y);
				const auto to = edges.row(y);
				for (auto x = 0; x != w; ++x) {
					const auto l = std::max(x - reach, 0);
					const auto r = std::min(x + reach, w - 1);
					auto gx = (up[r] + 2 * mid[r] + down[r])
						- (up[l] + 2 * mid[l] + down[l]);
					auto gy = (down[l] + 2 * down[x] + down[r])
						- (up[l] + 2 * up[x] + up[r]);
					if (further) {
						const auto fl = std::max(l - 1, 0);
						const auto fr = std::min(r + 1, w - 1);
						const auto fx = (upper[fr] + 2 * mid[fr] + lower[fr])
							- (upper[fl] + 2 * mid[fl] + lower[fl]);
						const auto fy = (lower[fl] + 2 * lower[x] + lower[fr])
							- (upper[fl] + 2 * upper[x] + upper[fr]);
						gx += ((fx - gx) * further) / 256;
						gy += ((fy - gy) * further) / 256;
					}
					const auto edge = std::min(
						(((std::abs(gx) + std::abs(gy)) * gain) >> 8)
							- threshold,
						255);
					if (edge <= 0) {
						to[x] = 0;
						continue;
					}
					auto color = tint;
					if (colors == ColorsSource) {
						const auto p = row[x];
						const auto top = std::max({ PxR(p), PxG(p), PxB(p) });
						color = (top > 0)
							? Opaque(
								PxR(p) * 255 / top,
								PxG(p) * 255 / top,
								PxB(p) * 255 / top)
							: Opaque(255, 255, 255);
					} else if (colors == ColorsRainbow) {
						const auto angle = std::atan2(float(gy), float(gx));
						color = rainbow[
							int((angle + kPi) * (255.f / (2.f * kPi))) & 255];
					}
					to[x] = Scale(color, uint32(edge + (edge >> 7)));
				}
			}
		});

		const auto glow = context.value(kGlow) / 100.f;
		const auto sw = (w + kShrink - 1) / kShrink;
		const auto sh = (h + kShrink - 1) / kShrink;
		auto strength = uint32(0);
		if (glow > 0.f) {
			scratch.reduced.resize(size_t(sw) * sh);
			ParallelFor(sh, std::max(1, 2048 / sw), [&](int from, int till) {
				for (auto sy = from; sy != till; ++sy) {
					const auto y0 = sy * kShrink;
					const auto y1 = std::min(y0 + kShrink, h);
					const auto to = scratch.reduced.data() + size_t(sy) * sw;
					for (auto sx = 0; sx != sw; ++sx) {
						const auto x0 = sx * kShrink;
						const auto x1 = std::min(x0 + kShrink, w);
						auto r = 0;
						auto g = 0;
						auto b = 0;
						for (auto y = y0; y != y1; ++y) {
							const auto line = edges.row(y);
							for (auto x = x0; x != x1; ++x) {
								const auto p = line[x];
								r += PxR(p);
								g += PxG(p);
								b += PxB(p);
							}
						}
						const auto count = (y1 - y0) * (x1 - x0);
						to[sx] = Opaque(r / count, g / count, b / count);
					}
				}
			});
			const auto radius = std::max(
				int(std::lround(glow * 8.f * context.unit)),
				1);
			BoxBlur(scratch.reduced, sw, sh, radius, scratch.reducedTemp);
			BoxBlur(scratch.reduced, sw, sh, radius, scratch.reducedTemp);

			// Above 720p the lines and the blur grow with the frame, the
			// glow is as bright as it is in a 720p one.
			const auto reference = std::max(int(std::lround(glow * 8.f)), 1);
			strength = (scale > 1.f)
				? uint32(glow
					* (2.f + 0.8f * reference)
					* float(2 * radius + 1)
					/ (float(2 * reference + 1) * scale)
					* 256.f)
				: uint32(glow * (2.f + 0.8f * radius) * 256.f);
		}
		const auto halo = BufOf(scratch.reduced, sw, sh);
		const auto keep = uint32(context.value(kOriginal) / 100.f * 256.f);
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				const auto line = edges.row(y);
				const auto haloY = (float(y) + 0.5f) / kShrink - 0.5f;
				for (auto x = 0; x != w; ++x) {
					const auto p = row[x];
					const auto base = Scale(p, keep);
					const auto edge = line[x];
					auto r = PxR(base) + PxR(edge);
					auto g = PxG(base) + PxG(edge);
					auto b = PxB(base) + PxB(edge);
					if (strength) {
						const auto light = SampleClamped(
							halo,
							(float(x) + 0.5f) / kShrink - 0.5f,
							haloY);
						r += int((uint32(PxR(light)) * strength) >> 8);
						g += int((uint32(PxG(light)) * strength) >> 8);
						b += int((uint32(PxB(light)) * strength) >> 8);
					}
					r = std::min(r, 255);
					g = std::min(g, 255);
					b = std::min(b, 255);
					row[x] = Pack(std::max({ PxA(p), r, g, b }), r, g, b);
				}
			}
		});
	}

};

//
// Feedback: what was shown before stays on the screen, fading, zoomed,
// rotated and shifted a bit more with every frame.
//

class FeedbackEffect final : public Effect {
public:
	enum : int {
		kPersistence,
		kZoom,
		kRotation,
		kShiftX,
		kShiftY,
		kMode,
		kHue,
		kParams,
	};
	enum : int { ModeLighten, ModeScreen, ModeBlend };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Feedback,
			"feedback",
			tr::lng_oblivion_vfx_fx_feedback,
			Group::Time,
			true,
			{
				PercentParam(
					"persistence",
					tr::lng_oblivion_vfx_p_persistence,
					60.),
				PercentParam(
					"zoom",
					tr::lng_oblivion_vfx_p_zoom,
					10.,
					-100.,
					100.),
				SliderParam(
					"rotation",
					tr::lng_oblivion_vfx_p_rotation,
					-180.,
					180.,
					0.,
					1.,
					Unit::Degrees),
				PercentParam(
					"shift_x",
					tr::lng_oblivion_vfx_p_shift_x,
					0.,
					-100.,
					100.),
				PercentParam(
					"shift_y",
					tr::lng_oblivion_vfx_p_shift_y,
					0.,
					-100.,
					100.),
				ChoiceParam("mode", tr::lng_oblivion_vfx_p_mode, {
					tr::lng_oblivion_vfx_o_lighten,
					tr::lng_oblivion_vfx_o_screen,
					tr::lng_oblivion_vfx_o_blend,
				}),
				PercentParam("hue", tr::lng_oblivion_vfx_p_hue, 0.),
			});
	}

	// In how many milliseconds a trail becomes twice weaker.
	[[nodiscard]] static float HalfLife(float persistence) {
		return 30.f * std::pow(2.f, persistence / 100.f * 7.f);
	}

	void reset() override {
		_width = _height = 0;
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		if (_width != w || _height != h) {
			Remember(frame, _state);
			_width = w;
			_height = h;
			return;
		}
		const auto seconds = context.stepSeconds();
		const auto keep = std::pow(
			0.5f,
			context.step / HalfLife(context.value(kPersistence)));
		const auto mode = context.integer(kMode);
		const auto zoom = std::pow(
			2.f,
			context.value(kZoom) / 100.f * 1.5f * seconds);
		const auto angle = context.value(kRotation) * kPi / 180.f * seconds;
		const auto side = float(std::min(w, h));
		const auto shiftX = context.value(kShiftX) / 100.f
			* 0.5f
			* side
			* seconds;
		const auto shiftY = context.value(kShiftY) / 100.f
			* 0.5f
			* side
			* seconds;
		const auto moved = (std::abs(zoom - 1.f) > 1e-5f)
			|| (std::abs(angle) > 1e-6f)
			|| (std::abs(shiftX) > 1e-3f)
			|| (std::abs(shiftY) > 1e-3f);
		const auto cosine = std::cos(angle) / zoom;
		const auto sine = std::sin(angle) / zoom;
		const auto cx = (w - 1) / 2.f;
		const auto cy = (h - 1) / 2.f;

		// A rotation of the colours around the grey axis.
		const auto hue = context.value(kHue) / 100.f * 2.f * kPi * seconds;
		const auto tinted = (hue > 1e-5f);
		auto matrix = std::array<int, 9>();
		if (tinted) {
			const auto c = std::cos(hue);
			const auto s = std::sin(hue) / std::sqrt(3.f);
			const auto third = (1.f - c) / 3.f;
			const auto values = std::array<float, 9>{ {
				c + third, third - s, third + s,
				third + s, c + third, third - s,
				third - s, third + s, c + third,
			} };
			for (auto i = 0; i != 9; ++i) {
				matrix[i] = int(std::lround(values[i] * 4096.f));
			}
		}
		// A blend follows the video much closer than a trail does,
		// a screen must not burn out what stays still.
		const auto weight = (mode == ModeBlend)
			? uint32(std::min(std::pow(keep, 6.f), 0.97f) * 256.f)
			: (mode == ModeScreen)
			? uint32(keep * 0.6f * 256.f)
			: uint32(keep * 256.f);
		const auto state = BufOf(_state, w, h);
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				const auto same = state.row(y);
				const auto qy = float(y) - cy - shiftY;
				auto sx = cx + (0.f - cx - shiftX) * cosine + qy * sine;
				auto sy = cy - (0.f - cx - shiftX) * sine + qy * cosine;
				for (auto x = 0; x != w; ++x, sx += cosine, sy -= sine) {
					auto old = moved
						? SampleOrNothing(state, sx, sy)
						: same[x];
					if (tinted && old) {
						const auto a = PxA(old);
						const auto r = PxR(old);
						const auto g = PxG(old);
						const auto b = PxB(old);
						const auto turned = [&](const int *m) {
							return std::clamp(
								(r * m[0] + g * m[1] + b * m[2]) >> 12,
								0,
								a);
						};
						old = Pack(
							a,
							turned(matrix.data()),
							turned(matrix.data() + 3),
							turned(matrix.data() + 6));
					}
					if (mode == ModeBlend) {
						// Nothing of the old picture comes from outside of
						// the frame: the video itself is seen there.
						row[x] = LerpOver(row[x], old, weight);
					} else if (mode == ModeScreen) {
						row[x] = ScreenChannels(row[x], Scale(old, weight));
					} else {
						row[x] = MaxChannels(row[x], Scale(old, weight));
					}
				}
			}
		});
		Remember(frame, _state);
	}

private:
	std::vector<Pixel> _state;
	int _width = 0;
	int _height = 0;

};

//
// Difference: only what has changed since the previous frame is seen.
//

class DifferenceEffect final : public Effect {
public:
	enum : int {
		kAmount,
		kThreshold,
		kMode,
		kColor,
		kParams,
	};
	enum : int { ModeSource, ModeTint, ModeOver };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Difference,
			"difference",
			tr::lng_oblivion_vfx_fx_difference,
			Group::Analysis,
			true,
			{
				PercentParam("amount", tr::lng_oblivion_vfx_p_amount, 50.),
				PercentParam(
					"threshold",
					tr::lng_oblivion_vfx_p_threshold,
					5.),
				ChoiceParam("mode", tr::lng_oblivion_vfx_p_mode, {
					tr::lng_oblivion_vfx_o_source,
					tr::lng_oblivion_vfx_o_tint,
					tr::lng_oblivion_vfx_o_over,
				}),
				ColorParam("color", tr::lng_oblivion_vfx_p_color, 0xFFFFFFU),
			});
	}

	void reset() override {
		_width = _height = 0;
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto mode = context.integer(kMode);
		if (_width != w || _height != h) {
			Remember(frame, _previous);
			_width = w;
			_height = h;
			if (mode != ModeOver) {
				// Nothing has changed yet.
				for (auto y = 0; y != h; ++y) {
					const auto row = frame.row(y);
					for (auto x = 0; x != w; ++x) {
						row[x] &= 0xFF000000U;
					}
				}
			}
			return;
		}
		const auto gain = int((1.f + context.value(kAmount) / 100.f * 9.f)
			* ClampF(kFrameMs / context.step, 0.5f, 3.f)
			* 256.f);
		const auto threshold = int(context.value(kThreshold) / 100.f * 64.f);
		const auto tint = context.color(kColor);
		const auto previous = BufOf(_previous, w, h);
		const auto changed = [=](int now, int was) {
			return std::min(
				(std::max(std::abs(now - was) - threshold, 0) * gain) >> 8,
				255);
		};
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				const auto old = previous.row(y);
				for (auto x = 0; x != w; ++x) {
					const auto now = row[x];
					const auto was = old[x];
					old[x] = now;
					const auto a = PxA(now);
					const auto r = changed(PxR(now), PxR(was));
					const auto g = changed(PxG(now), PxG(was));
					const auto b = changed(PxB(now), PxB(was));
					if (mode == ModeSource) {
						row[x] = Pack(
							a,
							std::min(r, a),
							std::min(g, a),
							std::min(b, a));
						continue;
					}
					const auto top = std::max({ r, g, b });
					const auto light = Scale(tint, uint32(top + (top >> 7)));
					if (mode == ModeTint) {
						row[x] = Pack(
							a,
							std::min(PxR(light), a),
							std::min(PxG(light), a),
							std::min(PxB(light), a));
					} else {
						const auto nr = std::min(PxR(now) + PxR(light), 255);
						const auto ng = std::min(PxG(now) + PxG(light), 255);
						const auto nb = std::min(PxB(now) + PxB(light), 255);
						row[x] = Pack(std::max({ a, nr, ng, nb }), nr, ng, nb);
					}
				}
			}
		});
	}

private:
	std::vector<Pixel> _previous;
	int _width = 0;
	int _height = 0;

};

//
// Slit-scan: every row (column, ring) of the picture is taken from
// another moment, the further from the start the older.
//

class SlitScanEffect final : public Effect {
public:
	enum : int {
		kDepth,
		kDirection,
		kSmooth,
		kParams,
	};
	enum : int { ToBottom, ToTop, ToRight, ToLeft, Outward, Inward };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::SlitScan,
			"slitscan",
			tr::lng_oblivion_vfx_fx_slitscan,
			Group::Time,
			true,
			{
				SliderParam(
					"depth",
					tr::lng_oblivion_vfx_p_depth,
					0.1,
					3.,
					1.,
					0.1,
					Unit::Seconds),
				ChoiceParam("direction", tr::lng_oblivion_vfx_p_direction, {
					tr::lng_oblivion_vfx_o_down,
					tr::lng_oblivion_vfx_o_up,
					tr::lng_oblivion_vfx_o_right,
					tr::lng_oblivion_vfx_o_left,
					tr::lng_oblivion_vfx_o_outward,
					tr::lng_oblivion_vfx_o_inward,
				}),
				ToggleParam("smooth", tr::lng_oblivion_vfx_p_smooth, true),
			});
	}

	void reset() override {
		_count = 0;
		_head = -1;
	}

	void release() override {
		_frames = {};
		_times = {};
		_width = _height = 0;
		reset();
	}

	void apply(const Context &context, const Buf &frame) override {
		constexpr auto kLevels = 256;

		const auto w = frame.width;
		const auto h = frame.height;
		const auto capacity = int(std::clamp(
			kSlitBudget / (int64(w) * h * int64(sizeof(Pixel))),
			int64(kSlitMinFrames),
			int64(kSlitMaxFrames)));
		if (_width != w || _height != h || int(_frames.size()) != capacity) {
			_frames.clear();
			_frames.resize(capacity);
			_times.assign(capacity, crl::time(0));
			_width = w;
			_height = h;
			reset();
		}
		// Frames are kept at fixed moments, as close to them as the video
		// allows, and one frame is spare: whatever the frame rate is,
		// the kept ones cover the whole depth.
		const auto depth = std::max(context.value(kDepth) * 1000.f, 1.f);
		const auto interval = float64(depth) / (capacity - 2);
		const auto position = context.position;
		if (!_count || float64(position) >= _due - interval * 0.1) {
			_due = (_count && (_due + interval > float64(position)))
				? (_due + interval)
				: (float64(position) + interval);
			_head = (_head + 1) % capacity;
			Remember(frame, _frames[_head]);
			_times[_head] = position;
			_count = std::min(_count + 1, capacity);
		}

		// What is shown for every delay: -1 is the frame itself, then
		// the kept ones from the newest.
		const auto place = [&](int index) {
			return (_head - index + capacity) % capacity;
		};
		const auto timeOf = [&](int index) {
			return (index < 0) ? position : _times[place(index)];
		};
		const auto dataOf = [&](int index) {
			return (index < 0)
				? static_cast<const Pixel*>(nullptr)
				: _frames[place(index)].data();
		};
		const auto smooth = context.toggled(kSmooth);
		auto first = std::array<const Pixel*, kLevels>();
		auto second = std::array<const Pixel*, kLevels>();
		auto weight = std::array<uint32, kLevels>();
		auto index = -1;
		for (auto level = 0; level != kLevels; ++level) {
			const auto target = float64(position)
				- float64(depth) * level / (kLevels - 1);
			while (index + 1 < _count
				&& float64(timeOf(index + 1)) >= target) {
				++index;
			}
			auto a = index;
			auto b = index;
			auto t = uint32(0);
			if (index + 1 < _count) {
				const auto newer = float64(timeOf(index));
				const auto older = float64(timeOf(index + 1));
				const auto part = (newer > older)
					? std::clamp((newer - target) / (newer - older), 0., 1.)
					: 0.;
				if (smooth) {
					b = index + 1;
					t = uint32(part * 256.);
				} else if (part >= 0.5) {
					a = b = index + 1;
				}
			}
			first[level] = dataOf(a);
			second[level] = dataOf(b);
			weight[level] = t;
		}

		const auto direction = context.integer(kDirection);
		if (direction == ToBottom || direction == ToTop) {
			ForRows(w, h, [&](int from, int till) {
				for (auto y = from; y != till; ++y) {
					auto level = (h > 1) ? (y * (kLevels - 1) / (h - 1)) : 0;
					if (direction == ToTop) {
						level = kLevels - 1 - level;
					}
					const auto row = frame.row(y);
					const auto a = first[level]
						? (first[level] + size_t(y) * w)
						: row;
					const auto b = second[level]
						? (second[level] + size_t(y) * w)
						: row;
					const auto t = weight[level];
					if (t) {
						for (auto x = 0; x != w; ++x) {
							row[x] = Lerp(a[x], b[x], t);
						}
					} else if (a != row) {
						memcpy(row, a, size_t(w) * sizeof(Pixel));
					}
				}
			});
			return;
		}
		const auto cx = (w - 1) / 2.f;
		const auto cy = (h - 1) / 2.f;
		const auto reach = std::max(std::sqrt(cx * cx + cy * cy), 1.f);
		const auto radial = (direction == Outward) || (direction == Inward);
		const auto flipped = (direction == ToLeft) || (direction == Inward);
		ForRows(w, h, [&](int from, int till) {
			auto levels = std::vector<uchar>(size_t(w));
			const auto fill = [&](int y) {
				for (auto x = 0; x != w; ++x) {
					auto level = 0;
					if (radial) {
						const auto dx = float(x) - cx;
						const auto dy = float(y) - cy;
						level = std::min(
							int(std::sqrt(dx * dx + dy * dy)
								/ reach
								* (kLevels - 1)),
							kLevels - 1);
					} else {
						level = (w > 1) ? (x * (kLevels - 1) / (w - 1)) : 0;
					}
					levels[x] = uchar(flipped ? (kLevels - 1 - level) : level);
				}
			};
			if (!radial) {
				fill(0);
			}
			for (auto y = from; y != till; ++y) {
				if (radial) {
					fill(y);
				}
				const auto row = frame.row(y);
				const auto shift = size_t(y) * w;
				for (auto x = 0; x != w; ++x) {
					const auto level = levels[x];
					const auto a = first[level]
						? first[level][shift + x]
						: row[x];
					const auto t = weight[level];
					if (!t) {
						row[x] = a;
						continue;
					}
					const auto b = second[level]
						? second[level][shift + x]
						: row[x];
					row[x] = Lerp(a, b, t);
				}
			}
		});
	}

private:
	std::vector<std::vector<Pixel>> _frames;
	std::vector<crl::time> _times;
	float64 _due = 0.; // When the next frame is to be kept.
	int _head = -1;
	int _count = 0;
	int _width = 0;
	int _height = 0;

};

//
// Pixel sorting: runs of pixels with a brightness inside the range are
// sorted along the rows or the columns.
//

class PixelSortEffect final : public Effect {
public:
	enum : int {
		kLow,
		kHigh,
		kDirection,
		kKey,
		kLength,
		kParams,
	};
	enum : int { ToRight, ToLeft, ToBottom, ToTop };
	enum : int { ByBrightness, ByHue, BySaturation };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::PixelSort,
			"pixelsort",
			tr::lng_oblivion_vfx_fx_pixelsort,
			Group::Distort,
			false,
			{
				PercentParam("low", tr::lng_oblivion_vfx_p_low, 25.),
				PercentParam("high", tr::lng_oblivion_vfx_p_high, 85.),
				ChoiceParam("direction", tr::lng_oblivion_vfx_p_direction, {
					tr::lng_oblivion_vfx_o_right,
					tr::lng_oblivion_vfx_o_left,
					tr::lng_oblivion_vfx_o_down,
					tr::lng_oblivion_vfx_o_up,
				}),
				ChoiceParam("key", tr::lng_oblivion_vfx_p_key, {
					tr::lng_oblivion_vfx_o_brightness,
					tr::lng_oblivion_vfx_o_hue,
					tr::lng_oblivion_vfx_o_saturation,
				}),
				PercentParam(
					"length",
					tr::lng_oblivion_vfx_p_length,
					100.,
					1.,
					100.),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		auto low = int(context.value(kLow) * 2.55f);
		auto high = int(context.value(kHigh) * 2.55f + 0.5f);
		if (low > high) {
			std::swap(low, high);
		}
		const auto direction = context.integer(kDirection);
		const auto vertical = (direction == ToBottom) || (direction == ToTop);
		const auto backwards = (direction == ToLeft) || (direction == ToTop);
		const auto key = context.integer(kKey);
		const auto longest = std::clamp(
			int((vertical ? h : w) * context.value(kLength) / 100.f),
			2,
			65535);
		const auto inside = [=](Pixel p) {
			const auto value = Luma(p);
			return (value >= low) && (value <= high);
		};
		const auto keyOf = [=](Pixel p) {
			return (key == ByHue)
				? HueOf(PxR(p), PxG(p), PxB(p))
				: (key == BySaturation)
				? SaturationOf(PxR(p), PxG(p), PxB(p))
				: Luma(p);
		};
		const auto sortLine = [&](
				Pixel *line,
				int count,
				std::vector<uint64> &items) {
			auto i = 0;
			while (i < count) {
				if (!inside(line[i])) {
					++i;
					continue;
				}
				auto j = i + 1;
				while (j < count && (j - i) < longest && inside(line[j])) {
					++j;
				}
				const auto length = j - i;
				if (length > 1) {
					// The key, then the place (equal keys keep their
					// order), then the pixel itself.
					items.clear();
					for (auto k = 0; k != length; ++k) {
						const auto p = line[i + k];
						items.push_back((uint64(keyOf(p)) << 48)
							| (uint64(k) << 32)
							| uint64(p));
					}
					std::sort(items.begin(), items.end());
					for (auto k = 0; k != length; ++k) {
						line[i + k] = Pixel(
							items[backwards ? (length - 1 - k) : k]
								& 0xFFFFFFFFULL);
					}
				}
				i = j;
			}
		};
		if (!vertical) {
			ForRows(w, h, [&](int from, int till) {
				auto items = std::vector<uint64>();
				items.reserve(size_t(w));
				for (auto y = from; y != till; ++y) {
					sortLine(frame.row(y), w, items);
				}
			});
			return;
		}
		ParallelFor(w, std::max(1, 16384 / h), [&](int from, int till) {
			auto items = std::vector<uint64>();
			items.reserve(size_t(h));
			auto column = std::vector<Pixel>(size_t(h));
			for (auto x = from; x != till; ++x) {
				for (auto y = 0; y != h; ++y) {
					column[y] = frame.at(x, y);
				}
				sortLine(column.data(), h, items);
				for (auto y = 0; y != h; ++y) {
					frame.at(x, y) = column[y];
				}
			}
		});
	}

};

//
// RGB split: the red and the blue pictures go to the opposite sides.
//

class RgbSplitEffect final : public Effect {
public:
	enum : int {
		kAmount,
		kAngle,
		kMode,
		kParams,
	};
	enum : int { ModeLinear, ModeRadial };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::RgbSplit,
			"rgbsplit",
			tr::lng_oblivion_vfx_fx_rgbsplit,
			Group::Distort,
			false,
			{
				SliderParam(
					"amount",
					tr::lng_oblivion_vfx_p_amount,
					0.,
					100.,
					12.),
				SliderParam(
					"angle",
					tr::lng_oblivion_vfx_p_angle,
					0.,
					360.,
					0.,
					1.,
					Unit::Degrees),
				ChoiceParam("mode", tr::lng_oblivion_vfx_p_mode, {
					tr::lng_oblivion_vfx_o_linear,
					tr::lng_oblivion_vfx_o_radial,
				}),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto amount = context.value(kAmount) * context.unit;
		if (amount < 0.25f) {
			return;
		}
		const auto combine = [](Pixel red, Pixel green, Pixel blue) {
			const auto r = PxR(red);
			const auto b = PxB(blue);
			return Pack(std::max({ PxA(green), r, b }), r, PxG(green), b);
		};
		if (context.integer(kMode) == ModeRadial) {
			const auto from = CopyOf(frame, context.scratch->copy);
			const auto cx = (w - 1) / 2.f;
			const auto cy = (h - 1) / 2.f;
			const auto part = amount
				/ std::max(std::sqrt(cx * cx + cy * cy), 1.f);
			const auto more = 1.f + part;
			const auto less = 1.f - part;
			ForRows(w, h, [&](int fromRow, int tillRow) {
				for (auto y = fromRow; y != tillRow; ++y) {
					const auto row = frame.row(y);
					const auto dy = float(y) - cy;
					for (auto x = 0; x != w; ++x) {
						const auto dx = float(x) - cx;
						row[x] = combine(
							SampleClamped(
								from,
								cx + dx * more,
								cy + dy * more),
							row[x],
							SampleClamped(
								from,
								cx + dx * less,
								cy + dy * less));
					}
				}
			});
			return;
		}
		const auto angle = context.value(kAngle) * kPi / 180.f;
		const auto dx = int(std::lround(std::cos(angle) * amount));
		const auto dy = int(std::lround(std::sin(angle) * amount));
		if (!dx && !dy) {
			return;
		}
		const auto from = CopyOf(frame, context.scratch->copy);
		ForRows(w, h, [&](int fromRow, int tillRow) {
			for (auto y = fromRow; y != tillRow; ++y) {
				const auto row = frame.row(y);
				const auto red = from.row(std::clamp(y - dy, 0, h - 1));
				const auto blue = from.row(std::clamp(y + dy, 0, h - 1));
				for (auto x = 0; x != w; ++x) {
					row[x] = combine(
						red[std::clamp(x - dx, 0, w - 1)],
						row[x],
						blue[std::clamp(x + dx, 0, w - 1)]);
				}
			}
		});
	}

};

//
// Displacement: the brighter (or the darker) a place of the picture is,
// the further its pixels are pushed.
//

class DisplaceEffect final : public Effect {
public:
	enum : int {
		kAmount,
		kAngle,
		kSmoothness,
		kMode,
		kParams,
	};
	enum : int { ModeLinear, ModeRadial };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Displace,
			"displace",
			tr::lng_oblivion_vfx_fx_displace,
			Group::Distort,
			false,
			{
				SliderParam(
					"amount",
					tr::lng_oblivion_vfx_p_amount,
					0.,
					200.,
					40.),
				SliderParam(
					"angle",
					tr::lng_oblivion_vfx_p_angle,
					0.,
					360.,
					0.,
					1.,
					Unit::Degrees),
				PercentParam(
					"smoothness",
					tr::lng_oblivion_vfx_p_smoothness,
					30.),
				ChoiceParam("mode", tr::lng_oblivion_vfx_p_mode, {
					tr::lng_oblivion_vfx_o_linear,
					tr::lng_oblivion_vfx_o_radial,
				}),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto amount = context.value(kAmount) * context.unit;
		if (amount < 0.25f) {
			return;
		}
		auto &scratch = *context.scratch;
		auto &map = scratch.plane;
		ReduceLuma(frame, kDisplaceSide, map);
		const auto radius = int(context.value(kSmoothness) / 100.f * 12.f
			+ 0.5f);
		BoxBlur(map, radius, scratch.bytes);
		BoxBlur(map, radius, scratch.bytes);

		const auto from = CopyOf(frame, scratch.copy);
		const auto radial = (context.integer(kMode) == ModeRadial);
		const auto angle = context.value(kAngle) * kPi / 180.f;
		const auto cosine = std::cos(angle);
		const auto sine = std::sin(angle);
		const auto mapX = float(map.width) / w;
		const auto mapY = float(map.height) / h;
		const auto cx = (w - 1) / 2.f;
		const auto cy = (h - 1) / 2.f;
		ForRows(w, h, [&](int fromRow, int tillRow) {
			for (auto y = fromRow; y != tillRow; ++y) {
				const auto row = frame.row(y);
				const auto my = (float(y) + 0.5f) * mapY - 0.5f;
				for (auto x = 0; x != w; ++x) {
					const auto value = SamplePlane(
						map,
						(float(x) + 0.5f) * mapX - 0.5f,
						my);
					const auto push = float(value - 128) / 128.f * amount;
					if (radial) {
						const auto dx = float(x) - cx;
						const auto dy = float(y) - cy;
						const auto scale = push
							/ (std::sqrt(dx * dx + dy * dy) + 1e-3f);
						row[x] = SampleClamped(
							from,
							float(x) + dx * scale,
							float(y) + dy * scale);
					} else {
						row[x] = SampleClamped(
							from,
							float(x) + push * cosine,
							float(y) + push * sine);
					}
				}
			}
		});
	}

};

//
// Kaleidoscope: one sector of the picture is repeated around the center,
// every second one mirrored.
//

class KaleidoscopeEffect final : public Effect {
public:
	enum : int {
		kSegments,
		kAngle,
		kSpin,
		kZoom,
		kCenterX,
		kCenterY,
		kParams,
	};

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Kaleidoscope,
			"kaleidoscope",
			tr::lng_oblivion_vfx_fx_kaleidoscope,
			Group::Distort,
			false,
			{
				IntegerParam(
					"segments",
					tr::lng_oblivion_vfx_p_segments,
					2,
					24,
					6),
				SliderParam(
					"angle",
					tr::lng_oblivion_vfx_p_angle,
					0.,
					360.,
					0.,
					1.,
					Unit::Degrees),
				SliderParam(
					"spin",
					tr::lng_oblivion_vfx_p_spin,
					-180.,
					180.,
					0.,
					1.,
					Unit::Degrees),
				PercentParam(
					"zoom",
					tr::lng_oblivion_vfx_p_zoom,
					100.,
					50.,
					300.),
				PercentParam(
					"center_x",
					tr::lng_oblivion_vfx_p_center_x,
					50.),
				PercentParam(
					"center_y",
					tr::lng_oblivion_vfx_p_center_y,
					50.),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto from = CopyOf(frame, context.scratch->copy);
		const auto wedge = 2.f * kPi
			/ float(std::max(context.integer(kSegments), 2));
		const auto turn = std::fmod(
			context.value(kAngle) + context.value(kSpin) * context.seconds(),
			360.f) * kPi / 180.f;
		const auto shrink = 100.f / std::max(context.value(kZoom), 1.f);
		const auto cx = context.value(kCenterX) / 100.f * (w - 1);
		const auto cy = context.value(kCenterY) / 100.f * (h - 1);
		const auto limitX = float(w - 1);
		const auto limitY = float(h - 1);
		ForRows(w, h, [&](int fromRow, int tillRow) {
			for (auto y = fromRow; y != tillRow; ++y) {
				const auto row = frame.row(y);
				const auto dy = float(y) - cy;
				for (auto x = 0; x != w; ++x) {
					const auto dx = float(x) - cx;
					const auto radius = std::sqrt(dx * dx + dy * dy) * shrink;
					auto angle = std::atan2(dy, dx);
					angle -= wedge * std::floor(angle / wedge);
					if (angle > wedge / 2.f) {
						angle = wedge - angle;
					}
					angle += turn;
					row[x] = SampleClamped(
						from,
						MirrorCoord(cx + radius * std::cos(angle), limitX),
						MirrorCoord(cy + radius * std::sin(angle), limitY));
				}
			}
		});
	}

};

//
// Mirror: one part of the picture is reflected onto the rest of it.
//

class MirrorEffect final : public Effect {
public:
	enum : int {
		kMode,
		kPosition,
		kParams,
	};
	enum : int { KeepLeft, KeepRight, KeepTop, KeepBottom, KeepCorner };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Mirror,
			"mirror",
			tr::lng_oblivion_vfx_fx_mirror,
			Group::Distort,
			false,
			{
				ChoiceParam("mode", tr::lng_oblivion_vfx_p_mode, {
					tr::lng_oblivion_vfx_o_mirror_left,
					tr::lng_oblivion_vfx_o_mirror_right,
					tr::lng_oblivion_vfx_o_mirror_top,
					tr::lng_oblivion_vfx_o_mirror_bottom,
					tr::lng_oblivion_vfx_o_mirror_quad,
				}),
				PercentParam(
					"position",
					tr::lng_oblivion_vfx_p_position,
					50.,
					5.,
					95.),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto mode = context.integer(kMode);
		const auto position = context.value(kPosition) / 100.f;

		// The part before the axis stays, the rest is its reflections.
		const auto fill = [&](std::vector<int> &map, int size, int kind) {
			map.resize(size);
			for (auto i = 0; i != size; ++i) {
				map[i] = i;
			}
			if (kind == 0 || size < 2) {
				return;
			}
			const auto last = size - 1;
			const auto axis = std::clamp(
				int(std::lround((kind > 0 ? position : (1.f - position))
					* last)),
				1,
				last);
			const auto period = 2 * axis;
			for (auto i = 0; i != size; ++i) {
				const auto place = (kind > 0) ? i : (last - i);
				const auto folded = place % period;
				const auto source = (folded <= axis)
					? folded
					: (period - folded);
				map[i] = (kind > 0) ? source : (last - source);
			}
		};
		auto mapX = std::vector<int>();
		auto mapY = std::vector<int>();
		fill(
			mapX,
			w,
			(mode == KeepLeft || mode == KeepCorner)
				? 1
				: (mode == KeepRight)
				? -1
				: 0);
		fill(
			mapY,
			h,
			(mode == KeepTop || mode == KeepCorner)
				? 1
				: (mode == KeepBottom)
				? -1
				: 0);
		const auto from = CopyOf(frame, context.scratch->copy);
		ForRows(w, h, [&](int fromRow, int tillRow) {
			for (auto y = fromRow; y != tillRow; ++y) {
				const auto row = frame.row(y);
				const auto source = from.row(mapY[y]);
				for (auto x = 0; x != w; ++x) {
					row[x] = source[mapX[x]];
				}
			}
		});
	}

};

//
// Averages of the cells of a grid, for the effects made of big "pixels".
//

struct CellGrid {
	float cellWidth = 1.f;
	float cellHeight = 1.f;
	int columns = 0;
	int rows = 0;
};

[[nodiscard]] CellGrid MakeGrid(
		int width,
		int height,
		float cellWidth,
		float cellHeight) {
	auto result = CellGrid();
	result.cellWidth = cellWidth;
	result.cellHeight = cellHeight;
	result.columns = std::max(int(std::ceil(width / cellWidth)), 1);
	result.rows = std::max(int(std::ceil(height / cellHeight)), 1);
	return result;
}

// The real (not premultiplied) average colour of every cell, opaque, and
// how much of the cell is covered (the average alpha).
void AverageCells(
		const Buf &frame,
		const CellGrid &grid,
		std::vector<Pixel> &colors,
		std::vector<uchar> &cover) {
	const auto count = size_t(grid.columns) * grid.rows;
	colors.resize(count);
	cover.resize(count);
	ParallelFor(
		grid.rows,
		std::max(1, 1024 / grid.columns),
		[&](int from, int till) {
		for (auto j = from; j != till; ++j) {
			const auto y0 = std::min(
				int(j * grid.cellHeight),
				frame.height - 1);
			const auto y1 = std::clamp(
				int((j + 1) * grid.cellHeight),
				y0 + 1,
				frame.height);
			const auto stepY = std::max((y1 - y0) / 4, 1);
			for (auto i = 0; i != grid.columns; ++i) {
				const auto x0 = std::min(
					int(i * grid.cellWidth),
					frame.width - 1);
				const auto x1 = std::clamp(
					int((i + 1) * grid.cellWidth),
					x0 + 1,
					frame.width);
				const auto stepX = std::max((x1 - x0) / 4, 1);
				auto a = 0;
				auto r = 0;
				auto g = 0;
				auto b = 0;
				auto samples = 0;
				for (auto y = y0 + stepY / 2; y < y1; y += stepY) {
					const auto row = frame.row(y);
					for (auto x = x0 + stepX / 2; x < x1; x += stepX) {
						const auto p = row[x];
						a += PxA(p);
						r += PxR(p);
						g += PxG(p);
						b += PxB(p);
						++samples;
					}
				}
				const auto index = size_t(j) * grid.columns + i;
				if (!a || !samples) {
					colors[index] = Opaque(0, 0, 0);
					cover[index] = 0;
					continue;
				}
				colors[index] = Opaque(
					std::min(r * 255 / a, 255),
					std::min(g * 255 / a, 255),
					std::min(b * 255 / a, 255));
				cover[index] = uchar(a / samples);
			}
		}
	});
}

// The brightest colour of the same hue.
[[nodiscard]] inline Pixel Brightest(Pixel color) {
	const auto top = std::max({ PxR(color), PxG(color), PxB(color) });
	return (top > 0)
		? Opaque(
			PxR(color) * 255 / top,
			PxG(color) * 255 / top,
			PxB(color) * 255 / top)
		: Opaque(255, 255, 255);
}

constexpr uchar kBayer4[16] = {
	0, 8, 2, 10,
	12, 4, 14, 6,
	3, 11, 1, 9,
	15, 7, 13, 5,
};

// Four greens of an old handheld console, from the darkest one.
constexpr Pixel kRetroPalette[4] = {
	0xFF0F380FU,
	0xFF306230U,
	0xFF8BAC0FU,
	0xFF9BBC0FU,
};

// 0..63.
[[nodiscard]] inline int Bayer8(int x, int y) {
	constexpr int kBayer2[4] = { 0, 2, 3, 1 };
	return 4 * kBayer4[(y & 3) * 4 + (x & 3)]
		+ kBayer2[((y >> 2) & 1) * 2 + ((x >> 2) & 1)];
}

//
// ASCII: the picture is made of symbols of the painted font, the darker
// a cell the lighter its symbol.
//

class AsciiEffect final : public Effect {
public:
	enum : int {
		kSize,
		kCharset,
		kColors,
		kColor,
		kInvert,
		kParams,
	};
	enum : int { SetClassic, SetBlocks, SetBinary };
	enum : int { ColorsSource, ColorsTint };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Ascii,
			"ascii",
			tr::lng_oblivion_vfx_fx_ascii,
			Group::Stylize,
			false,
			{
				SliderParam(
					"size",
					tr::lng_oblivion_vfx_p_size,
					4.,
					48.,
					10.),
				ChoiceParam("charset", tr::lng_oblivion_vfx_p_charset, {
					tr::lng_oblivion_vfx_o_classic,
					tr::lng_oblivion_vfx_o_blocks,
					tr::lng_oblivion_vfx_o_binary,
				}),
				ChoiceParam("colors", tr::lng_oblivion_vfx_p_colors, {
					tr::lng_oblivion_vfx_o_source,
					tr::lng_oblivion_vfx_o_tint,
				}),
				ColorParam("color", tr::lng_oblivion_vfx_p_color, 0x33FF66U),
				ToggleParam("invert", tr::lng_oblivion_vfx_p_invert, false),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		constexpr char kRamp[] = " .:-=+*#%@";
		constexpr auto kRampSize = int(sizeof(kRamp)) - 1;

		const auto w = frame.width;
		const auto h = frame.height;
		const auto cellWidth = std::max(
			context.value(kSize) * context.unit,
			3.f);
		const auto grid = MakeGrid(
			w,
			h,
			cellWidth,
			cellWidth * kGlyphLine / kGlyphAdvance);
		auto &scratch = *context.scratch;
		auto &colors = scratch.reduced;
		auto &levels = scratch.bytes;
		AverageCells(frame, grid, colors, levels);

		// What every cell shows: the colour of the symbol and the symbol
		// (the brightness of the cell, for the blocks).
		const auto charset = context.integer(kCharset);
		const auto source = (context.integer(kColors) == ColorsSource);
		const auto tint = context.color(kColor);
		const auto tick = TickAt(context.position, 8.);
		const auto cells = size_t(grid.columns) * grid.rows;
		for (auto i = size_t(0); i != cells; ++i) {
			const auto color = colors[i];
			const auto level = Luma(color) * levels[i] / 255;
			auto ink = source ? Brightest(color) : tint;
			if (charset == SetBinary) {
				ink = Scale(ink, uint32(48 + level * 208 / 255))
					| 0xFF000000U;
				levels[i] = uchar((level < 12)
					? ' '
					: ((Hash(uint32(i), tick) >> 7) & 1)
					? '1'
					: '0');
			} else if (charset == SetClassic) {
				levels[i] = uchar(kRamp[level * kRampSize / 256]);
			} else {
				levels[i] = uchar(level);
			}
			colors[i] = ink;
		}

		// The cell and the column of the font under every pixel column.
		auto cellOf = std::vector<int>(size_t(w));
		auto columnOf = std::vector<uchar>(size_t(w));
		for (auto x = 0; x != w; ++x) {
			const auto cell = std::min(
				int(float(x) / grid.cellWidth),
				grid.columns - 1);
			cellOf[x] = cell;
			columnOf[x] = uchar(std::clamp(
				int((float(x) - cell * grid.cellWidth)
					* kGlyphAdvance
					/ grid.cellWidth),
				0,
				kGlyphAdvance - 1));
		}
		const auto inverted = context.toggled(kInvert);
		const auto blocks = (charset == SetBlocks);
		const auto black = Opaque(0, 0, 0);
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				const auto j = std::min(
					int(float(y) / grid.cellHeight),
					grid.rows - 1);
				const auto line = std::clamp(
					int((float(y) - j * grid.cellHeight)
						* kGlyphLine
						/ grid.cellHeight),
					0,
					kGlyphLine - 1);
				const auto inks = colors.data() + size_t(j) * grid.columns;
				const auto symbols = levels.data() + size_t(j) * grid.columns;
				for (auto x = 0; x != w; ++x) {
					const auto cell = cellOf[x];
					const auto column = int(columnOf[x]);
					auto lit = false;
					if (blocks) {
						lit = (kBayer4[(line & 3) * 4 + (column & 3)] * 16 + 8)
							< int(symbols[cell]);
					} else if (column < kGlyphWidth && line < kGlyphHeight) {
						lit = ((GlyphOf(char(symbols[cell]))[column] >> line)
							& 1) != 0;
					}
					row[x] = WithAlphaOf(
						(lit != inverted) ? inks[cell] : black,
						row[x]);
				}
			}
		});
	}

};

//
// Halftone: the picture is printed with dots (lines, squares) of
// a regular screen, the darker a place the bigger they are.
//

class HalftoneEffect final : public Effect {
public:
	enum : int {
		kSize,
		kAngle,
		kShape,
		kMode,
		kColor,
		kBackground,
		kParams,
	};
	enum : int { ShapeDots, ShapeLines, ShapeSquares };
	enum : int { ModeTwoColors, ModeCmyk, ModeSource };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Halftone,
			"halftone",
			tr::lng_oblivion_vfx_fx_halftone,
			Group::Stylize,
			false,
			{
				SliderParam(
					"size",
					tr::lng_oblivion_vfx_p_size,
					3.,
					40.,
					8.),
				SliderParam(
					"angle",
					tr::lng_oblivion_vfx_p_angle,
					0.,
					90.,
					45.,
					1.,
					Unit::Degrees),
				ChoiceParam("shape", tr::lng_oblivion_vfx_p_shape, {
					tr::lng_oblivion_vfx_o_dots,
					tr::lng_oblivion_vfx_o_lines,
					tr::lng_oblivion_vfx_o_squares,
				}),
				ChoiceParam("mode", tr::lng_oblivion_vfx_p_mode, {
					tr::lng_oblivion_vfx_o_two_colors,
					tr::lng_oblivion_vfx_o_cmyk,
					tr::lng_oblivion_vfx_o_source,
				}),
				ColorParam("color", tr::lng_oblivion_vfx_p_color, 0x141414U),
				ColorParam(
					"background",
					tr::lng_oblivion_vfx_p_background,
					0xF4F0E6U),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		constexpr auto kWave = 256;
		struct Screen {
			float stepU = 0.f; // By one pixel to the right.
			float stepV = 0.f;
			float downU = 0.f; // By one pixel down.
			float downV = 0.f;
		};

		const auto w = frame.width;
		const auto h = frame.height;
		const auto cell = std::max(context.value(kSize) * context.unit, 2.f);
		const auto shape = context.integer(kShape);
		const auto mode = context.integer(kMode);
		const auto ink = context.color(kColor);
		const auto paper = context.color(kBackground);
		const auto sharp = cell / 2.5f;

		// How dark a place must be for the ink to reach a point of the
		// cell: 0 in the center of a dot, 1 between the dots.
		auto wave = std::array<float, kWave>();
		for (auto i = 0; i != kWave; ++i) {
			wave[i] = (shape == ShapeDots)
				? (0.5f - 0.5f * std::cos(2.f * kPi * i / kWave))
				: (1.f - std::abs(2.f * i / kWave - 1.f));
		}
		const auto screen = [&](float degrees) {
			const auto angle = degrees * kPi / 180.f;
			return Screen{
				.stepU = std::cos(angle) / cell,
				.stepV = -std::sin(angle) / cell,
				.downU = std::sin(angle) / cell,
				.downV = std::cos(angle) / cell,
			};
		};
		const auto angle = context.value(kAngle);
		const auto screens = std::array<Screen, 3>{ {
			screen(angle),
			screen(angle + 30.f),
			screen(angle + 60.f),
		} };

		// 0..256: how much of the pixel is covered with ink.
		const auto covered = [&](float u, float v, int dark) {
			const auto a = wave[int((u - std::floor(u)) * kWave) & (kWave - 1)];
			const auto b = wave[int((v - std::floor(v)) * kWave) & (kWave - 1)];
			const auto level = (shape == ShapeLines)
				? b
				: (shape == ShapeSquares)
				? std::max(a, b)
				: ((a + b) / 2.f);
			return int(ClampF(
				(dark / 255.f - level) * sharp + 0.5f,
				0.f,
				1.f) * 256.f);
		};
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				auto u = std::array<float, 3>();
				auto v = std::array<float, 3>();
				for (auto i = 0; i != 3; ++i) {
					u[i] = y * screens[i].downU;
					v[i] = y * screens[i].downV;
				}
				for (auto x = 0; x != w; ++x) {
					const auto p = row[x];
					if (PxA(p)) {
						row[x] = MapColor(p, [&](int r, int g, int b) {
							if (mode == ModeCmyk) {
								const auto c = covered(u[0], v[0], 255 - r);
								const auto m = covered(u[1], v[1], 255 - g);
								const auto k = covered(u[2], v[2], 255 - b);
								return Opaque(
									255 - ((c * 255) >> 8),
									255 - ((m * 255) >> 8),
									255 - ((k * 255) >> 8));
							} else if (mode == ModeSource) {
								// The ink has the colour of the place.
								return Lerp(
									paper,
									Opaque(r, g, b),
									uint32(covered(
										u[0],
										v[0],
										255 - LumaOf(r, g, b) / 2)));
							}
							return Lerp(
								paper,
								ink,
								uint32(covered(
									u[0],
									v[0],
									255 - LumaOf(r, g, b))));
						});
					}
					for (auto i = 0; i != 3; ++i) {
						u[i] += screens[i].stepU;
						v[i] += screens[i].stepV;
					}
				}
			}
		});
	}

};

//
// Dither: a few levels of every channel (or two colours) with a pattern
// or with an error diffusion, in big pixels.
//

class DitherEffect final : public Effect {
public:
	enum : int {
		kLevels,
		kMethod,
		kPixel,
		kPalette,
		kColor,
		kBackground,
		kParams,
	};
	enum : int { MethodBayer4, MethodBayer8, MethodFloyd, MethodNoise };
	enum : int { PaletteSource, PaletteTwoColors, PaletteRetro };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Dither,
			"dither",
			tr::lng_oblivion_vfx_fx_dither,
			Group::Stylize,
			false,
			{
				IntegerParam("levels", tr::lng_oblivion_vfx_p_levels, 2, 8, 2),
				ChoiceParam("method", tr::lng_oblivion_vfx_p_method, {
					tr::lng_oblivion_vfx_o_bayer4,
					tr::lng_oblivion_vfx_o_bayer8,
					tr::lng_oblivion_vfx_o_floyd,
					tr::lng_oblivion_vfx_o_noise,
				}),
				SliderParam(
					"pixel",
					tr::lng_oblivion_vfx_p_pixel,
					1.,
					16.,
					3.),
				ChoiceParam("palette", tr::lng_oblivion_vfx_p_palette, {
					tr::lng_oblivion_vfx_o_source,
					tr::lng_oblivion_vfx_o_two_colors,
					tr::lng_oblivion_vfx_o_retro,
				}, PaletteTwoColors),
				ColorParam("color", tr::lng_oblivion_vfx_p_color, 0xFFFFFFU),
				ColorParam(
					"background",
					tr::lng_oblivion_vfx_p_background,
					0x000000U),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto block = std::max(
			int(std::lround(context.value(kPixel) * context.unit)),
			1);
		const auto grid = MakeGrid(w, h, float(block), float(block));
		auto &scratch = *context.scratch;
		auto &cells = scratch.reduced;
		AverageCells(frame, grid, cells, scratch.bytes);

		const auto method = context.integer(kMethod);
		const auto palette = context.integer(kPalette);
		const auto levels = (palette == PaletteRetro)
			? 4
			: std::clamp(context.integer(kLevels), 2, 8);
		const auto step = 255.f / float(levels - 1);
		const auto light = context.color(kColor);
		const auto dark = context.color(kBackground);
		const auto level = [=](float value) {
			return std::clamp(int(value / step + 0.5f), 0, levels - 1);
		};
		const auto mono = [=](int index) {
			return (palette == PaletteRetro)
				? kRetroPalette[index]
				: Lerp(dark, light, uint32(index * 256 / (levels - 1)));
		};
		const auto channel = [=](int index) {
			return index * 255 / (levels - 1);
		};
		const auto sw = grid.columns;
		const auto sh = grid.rows;
		if (method == MethodFloyd) {
			// Errors of the current and of the next row, three channels
			// (only the first one for two colours).
			auto errors = std::vector<float>(size_t(sw + 2) * 6, 0.f);
			auto current = errors.data();
			auto next = errors.data() + size_t(sw + 2) * 3;
			const auto channels = (palette == PaletteSource) ? 3 : 1;
			for (auto y = 0; y != sh; ++y) {
				const auto row = cells.data() + size_t(y) * sw;
				std::fill(next, next + size_t(sw + 2) * 3, 0.f);
				for (auto x = 0; x != sw; ++x) {
					const auto p = row[x];
					const auto values = std::array<int, 3>{ {
						(channels == 3) ? PxR(p) : Luma(p),
						PxG(p),
						PxB(p),
					} };
					auto indexes = std::array<int, 3>();
					for (auto c = 0; c != channels; ++c) {
						const auto at = size_t(x + 1) * 3 + c;
						const auto value = float(values[c]) + current[at];
						indexes[c] = level(value);
						const auto error = value - indexes[c] * step;
						current[at + 3] += error * (7.f / 16.f);
						next[at - 3] += error * (3.f / 16.f);
						next[at] += error * (5.f / 16.f);
						next[at + 3] += error * (1.f / 16.f);
					}
					row[x] = (channels == 3)
						? Opaque(
							channel(indexes[0]),
							channel(indexes[1]),
							channel(indexes[2]))
						: mono(indexes[0]);
				}
				std::swap(current, next);
			}
		} else {
			ParallelFor(sh, std::max(1, 4096 / sw), [&](int from, int till) {
				for (auto y = from; y != till; ++y) {
					const auto row = cells.data() + size_t(y) * sw;
					for (auto x = 0; x != sw; ++x) {
						const auto shift = (method == MethodBayer4)
							? ((kBayer4[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.f)
							: (method == MethodBayer8)
							? ((Bayer8(x, y) + 0.5f) / 64.f)
							: (float(Hash(uint32(x), uint32(y)) >> 8)
								/ 16777216.f);
						const auto add = (shift - 0.5f) * step;
						const auto p = row[x];
						row[x] = (palette == PaletteSource)
							? Opaque(
								channel(level(PxR(p) + add)),
								channel(level(PxG(p) + add)),
								channel(level(PxB(p) + add)))
							: mono(level(Luma(p) + add));
					}
				}
			});
		}
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				const auto source = cells.data() + size_t(y / block) * sw;
				for (auto x = 0; x != w; ++x) {
					row[x] = WithAlphaOf(source[x / block], row[x]);
				}
			}
		});
	}

};

//
// Threshold: two colours, by the brightness.
//

class ThresholdEffect final : public Effect {
public:
	enum : int {
		kLevel,
		kSoftness,
		kColor,
		kBackground,
		kInvert,
		kParams,
	};

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Threshold,
			"threshold",
			tr::lng_oblivion_vfx_fx_threshold,
			Group::Stylize,
			false,
			{
				PercentParam("level", tr::lng_oblivion_vfx_p_level, 50.),
				PercentParam("softness", tr::lng_oblivion_vfx_p_softness, 5.),
				ColorParam("color", tr::lng_oblivion_vfx_p_color, 0xFFFFFFU),
				ColorParam(
					"background",
					tr::lng_oblivion_vfx_p_background,
					0x000000U),
				ToggleParam("invert", tr::lng_oblivion_vfx_p_invert, false),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto level = context.value(kLevel) * 2.55f;
		const auto soft = std::max(context.value(kSoftness) * 1.28f, 0.5f);
		const auto light = context.color(kColor);
		const auto dark = context.color(kBackground);
		const auto inverted = context.toggled(kInvert);
		auto table = std::array<Pixel, 256>();
		for (auto i = 0; i != 256; ++i) {
			const auto part = ClampF(
				(float(i) - level) / soft * 0.5f + 0.5f,
				0.f,
				1.f);
			table[i] = Lerp(
				dark,
				light,
				uint32((inverted ? (1.f - part) : part) * 256.f));
		}
		ForRows(frame.width, frame.height, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				for (auto x = 0; x != frame.width; ++x) {
					row[x] = MapColor(row[x], [&](int r, int g, int b) {
						return table[LumaOf(r, g, b)];
					});
				}
			}
		});
	}

};

//
// Posterize: a few levels of every channel, without any dithering.
//

class PosterizeEffect final : public Effect {
public:
	enum : int {
		kLevels,
		kKeepHues,
		kParams,
	};

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Posterize,
			"posterize",
			tr::lng_oblivion_vfx_fx_posterize,
			Group::Stylize,
			false,
			{
				IntegerParam(
					"levels",
					tr::lng_oblivion_vfx_p_levels,
					2,
					16,
					4),
				ToggleParam(
					"keep_hues",
					tr::lng_oblivion_vfx_p_keep_hues,
					false),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto levels = std::clamp(context.integer(kLevels), 2, 16);
		const auto keepHues = context.toggled(kKeepHues);
		auto table = std::array<uchar, 256>();
		for (auto i = 0; i != 256; ++i) {
			table[i] = uchar((i * levels / 256) * 255 / (levels - 1));
		}
		ForRows(frame.width, frame.height, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				for (auto x = 0; x != frame.width; ++x) {
					row[x] = MapColor(row[x], [&](int r, int g, int b) {
						if (!keepHues) {
							return Opaque(table[r], table[g], table[b]);
						}
						const auto was = LumaOf(r, g, b);
						if (!was) {
							return Opaque(0, 0, 0);
						}
						const auto now = int(table[was]);
						return Opaque(
							std::min(r * now / was, 255),
							std::min(g * now / was, 255),
							std::min(b * now / was, 255));
					});
				}
			}
		});
	}

};

//
// False colour: the brightness is shown with a palette, like a thermal
// camera does.
//

class FalseColorEffect final : public Effect {
public:
	enum : int {
		kPalette,
		kShift,
		kCycle,
		kBands,
		kParams,
	};

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::FalseColor,
			"falsecolor",
			tr::lng_oblivion_vfx_fx_falsecolor,
			Group::Stylize,
			false,
			{
				ChoiceParam("palette", tr::lng_oblivion_vfx_p_palette, {
					tr::lng_oblivion_vfx_o_thermal,
					tr::lng_oblivion_vfx_o_rainbow,
					tr::lng_oblivion_vfx_o_night,
					tr::lng_oblivion_vfx_o_fire,
					tr::lng_oblivion_vfx_o_ice,
					tr::lng_oblivion_vfx_o_neon,
					tr::lng_oblivion_vfx_o_xray,
				}),
				PercentParam("shift", tr::lng_oblivion_vfx_p_shift, 0.),
				PercentParam("cycle", tr::lng_oblivion_vfx_p_cycle, 0.),
				IntegerParam("bands", tr::lng_oblivion_vfx_p_bands, 0, 16, 0),
			});
	}

	struct Stop {
		float position = 0.f;
		uint32 color = 0;
	};

	[[nodiscard]] static std::vector<Stop> Stops(int palette) {
		switch (palette) {
		case 0: return {
			{ 0.f, 0x00000AU },
			{ 0.15f, 0x20008CU },
			{ 0.35f, 0x9100A5U },
			{ 0.55f, 0xE63C14U },
			{ 0.75f, 0xFF9600U },
			{ 0.9f, 0xFFE632U },
			{ 1.f, 0xFFFFFFU },
		};
		case 1: return {
			{ 0.f, 0x000080U },
			{ 0.125f, 0x0000FFU },
			{ 0.375f, 0x00FFFFU },
			{ 0.625f, 0xFFFF00U },
			{ 0.875f, 0xFF0000U },
			{ 1.f, 0x800000U },
		};
		case 2: return {
			{ 0.f, 0x000000U },
			{ 0.5f, 0x0A6E14U },
			{ 0.85f, 0x50F050U },
			{ 1.f, 0xDCFFDCU },
		};
		case 3: return {
			{ 0.f, 0x000000U },
			{ 0.3f, 0x7A0000U },
			{ 0.55f, 0xF03C00U },
			{ 0.8f, 0xFFC800U },
			{ 1.f, 0xFFFFE0U },
		};
		case 4: return {
			{ 0.f, 0x000014U },
			{ 0.35f, 0x0A3C96U },
			{ 0.7f, 0x28C8F0U },
			{ 1.f, 0xF0FFFFU },
		};
		case 5: return {
			{ 0.f, 0x0A0028U },
			{ 0.3f, 0x7800C8U },
			{ 0.55f, 0xFF1493U },
			{ 0.8f, 0x00E5FFU },
			{ 1.f, 0xF0FFFFU },
		};
		}
		return {
			{ 0.f, 0xE6F0FFU },
			{ 0.5f, 0x5A7896U },
			{ 1.f, 0x00050FU },
		};
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto stops = Stops(context.integer(kPalette));
		const auto bands = context.integer(kBands);
		auto table = std::array<Pixel, 256>();
		for (auto i = 0; i != 256; ++i) {
			const auto index = (bands > 1)
				? ((i * bands / 256) * 255 / (bands - 1))
				: i;
			const auto at = index / 255.f;
			auto next = 1;
			while (next + 1 < int(stops.size())
				&& stops[next].position < at) {
				++next;
			}
			const auto &a = stops[next - 1];
			const auto &b = stops[next];
			const auto part = ClampF(
				(at - a.position)
					/ std::max(b.position - a.position, 1e-4f),
				0.f,
				1.f);
			table[i] = Lerp(
				0xFF000000U | a.color,
				0xFF000000U | b.color,
				uint32(part * 256.f));
		}
		const auto offset = int(std::fmod(
			context.value(kShift) / 100.f
				+ context.value(kCycle) / 100.f * 1.5f * context.seconds(),
			1.f) * 256.f);
		ForRows(frame.width, frame.height, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				for (auto x = 0; x != frame.width; ++x) {
					row[x] = MapColor(row[x], [&](int r, int g, int b) {
						return table[(LumaOf(r, g, b) + offset) & 255];
					});
				}
			}
		});
	}

};

//
// Glitch: several times a second slices of the picture jump aside,
// blocks get broken, the colours split.
//

class GlitchEffect final : public Effect {
public:
	enum : int {
		kAmount,
		kSlices,
		kBlocks,
		kRgb,
		kSpeed,
		kSeed,
		kParams,
	};

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Glitch,
			"glitch",
			tr::lng_oblivion_vfx_fx_glitch,
			Group::Distort,
			false,
			{
				PercentParam("amount", tr::lng_oblivion_vfx_p_amount, 50.),
				PercentParam("slices", tr::lng_oblivion_vfx_p_slices, 40.),
				PercentParam("blocks", tr::lng_oblivion_vfx_p_blocks, 30.),
				PercentParam("rgb", tr::lng_oblivion_vfx_p_rgb, 40.),
				SliderParam(
					"speed",
					tr::lng_oblivion_vfx_p_speed,
					1.,
					30.,
					10.,
					1.,
					Unit::Hertz),
				SeedParam(),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto amount = context.value(kAmount) / 100.f;
		auto random = Random(Hash(
			uint32(context.integer(kSeed)),
			TickAt(context.position, context.value(kSpeed)),
			0x61C88647U));
		if (amount <= 0.f || random.unit() >= 0.1f + amount * 0.9f) {
			// A quiet moment.
			return;
		}

		// All the random numbers are parts of the frame, not pixels:
		// a preview and an export glitch at the same places.
		const auto split = int(std::lround(random.spread()
			* context.value(kRgb) / 100.f
			* 28.f
			* context.unit));
		if (split) {
			ForRows(w, h, [&](int from, int till) {
				auto line = std::vector<Pixel>(size_t(w));
				for (auto y = from; y != till; ++y) {
					const auto row = frame.row(y);
					memcpy(line.data(), row, size_t(w) * sizeof(Pixel));
					for (auto x = 0; x != w; ++x) {
						const auto p = line[x];
						const auto r = PxR(
							line[std::clamp(x + split, 0, w - 1)]);
						const auto b = PxB(
							line[std::clamp(x - split, 0, w - 1)]);
						row[x] = Pack(std::max({ PxA(p), r, b }), r, PxG(p), b);
					}
				}
			});
		}

		const auto reach = context.value(kSlices) / 100.f * 0.3f * w;
		const auto slices = 1 + random.below(1 + int(amount * 10.f));
		for (auto i = 0; i != slices; ++i) {
			const auto top = std::min(int(random.unit() * h), h - 1);
			const auto size = 1
				+ int(random.unit() * random.unit() * h * 0.25f);
			const auto shift = int(random.spread() * reach) % w;
			if (!shift) {
				continue;
			}
			const auto middle = (shift > 0) ? (w - shift) : -shift;
			for (auto y = top; y < std::min(top + size, h); ++y) {
				const auto row = frame.row(y);
				std::rotate(row, row + middle, row + w);
			}
		}

		const auto cell = std::max(int(std::lround(16.f * context.unit)), 4);
		const auto columns = std::max(w / cell, 1);
		const auto rows = std::max(h / cell, 1);
		const auto blocks = int(context.value(kBlocks) / 100.f
			* (2.f + 22.f * amount * random.unit()));
		auto saved = std::vector<Pixel>();
		for (auto i = 0; i != blocks; ++i) {
			const auto x0 = random.below(columns) * cell;
			const auto y0 = random.below(rows) * cell;
			const auto x1 = std::min(x0 + cell * (1 + random.below(8)), w);
			const auto y1 = std::min(y0 + cell * (1 + random.below(4)), h);
			const auto kind = random.below(5);
			const auto fromX = random.unit();
			const auto fromY = random.unit();
			const auto bw = x1 - x0;
			const auto bh = y1 - y0;
			if (bw <= 0 || bh <= 0) {
				continue;
			}
			if (kind == 0) {
				// A piece from another place.
				const auto sx = std::min(int(fromX * (w - bw + 1)), w - bw);
				const auto sy = std::min(int(fromY * (h - bh + 1)), h - bh);
				saved.resize(size_t(bw) * bh);
				for (auto y = 0; y != bh; ++y) {
					memcpy(
						saved.data() + size_t(y) * bw,
						frame.row(sy + y) + sx,
						size_t(bw) * sizeof(Pixel));
				}
				for (auto y = 0; y != bh; ++y) {
					memcpy(
						frame.row(y0 + y) + x0,
						saved.data() + size_t(y) * bw,
						size_t(bw) * sizeof(Pixel));
				}
				continue;
			}
			for (auto y = y0; y != y1; ++y) {
				const auto row = frame.row(y);
				if (kind == 3) {
					// The first row all the way down.
					if (y != y0) {
						memcpy(
							row + x0,
							frame.row(y0) + x0,
							size_t(bw) * sizeof(Pixel));
					}
					continue;
				}
				for (auto x = x0; x != x1; ++x) {
					const auto p = row[x];
					const auto a = PxA(p);
					row[x] = (kind == 1)
						? Pack(a, PxG(p), PxB(p), PxR(p))
						: (kind == 2)
						? (p & 0xFFC0C0C0U)
						: Pack(a, a - PxR(p), a - PxG(p), a - PxB(p));
				}
			}
		}
	}

};

//
// Datamosh: instead of showing a new frame the old picture is moved the
// way the video moves (blocks are matched with the previous frame on
// a small copy), so things smear over each other till the next refresh.
//

class DatamoshEffect final : public Effect {
public:
	enum : int {
		kAmount,
		kInterval,
		kBlock,
		kThreshold,
		kParams,
	};

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Datamosh,
			"datamosh",
			tr::lng_oblivion_vfx_fx_datamosh,
			Group::Time,
			true,
			{
				PercentParam("amount", tr::lng_oblivion_vfx_p_amount, 92.),
				SliderParam(
					"interval",
					tr::lng_oblivion_vfx_p_interval,
					0.2,
					10.,
					2.,
					0.1,
					Unit::Seconds),
				SliderParam(
					"block",
					tr::lng_oblivion_vfx_p_block,
					4.,
					64.,
					16.),
				PercentParam(
					"threshold",
					tr::lng_oblivion_vfx_p_threshold,
					60.),
			});
	}

	// The time between two refreshes, in ms.
	[[nodiscard]] static crl::time Interval(float64 value) {
		return std::max(crl::time(std::llround(value * 1000.)), crl::time(1));
	}

	void reset() override {
		_width = _height = 0;
	}

	void apply(const Context &context, const Buf &frame) override {
		constexpr auto kSearch = 4;

		const auto w = frame.width;
		const auto h = frame.height;
		auto &gray = context.scratch->plane;
		ReduceLuma(frame, kMoshSide, gray);

		// The refreshes stand at fixed moments of the result, wherever
		// the frames start from: a paused frame after a pre-roll, the
		// playback from any place and the export show the same smear.
		const auto refresh = std::max(context.position, crl::time(0))
			/ Interval(context.values[kInterval]);
		if (_width != w
			|| _height != h
			|| !_gray.sameSize(gray)
			|| (gray.width < 2)
			|| (gray.height < 2)
			|| (refresh != _refresh)) {
			// A "key frame": the picture as it is.
			Remember(frame, _output);
			_gray = gray;
			_width = w;
			_height = h;
			_refresh = refresh;
			return;
		}
		const auto block = std::clamp(
			int(std::lround(context.value(kBlock) * context.unit)),
			4,
			128);
		const auto columns = (w + block - 1) / block;
		const auto rows = (h + block - 1) / block;
		const auto gw = gray.width;
		const auto gh = gray.height;
		const auto keep = uint32(context.value(kAmount) / 100.f * 256.f);
		const auto limit = 4.f + context.value(kThreshold) / 100.f * 60.f;
		const auto output = BufOf(_output, w, h);
		ParallelFor(rows, 1, [&](int from, int till) {
			for (auto j = from; j != till; ++j) {
				const auto y0 = j * block;
				const auto y1 = std::min(y0 + block, h);
				const auto ay1 = std::clamp(
					int((int64(y1) * gh + h - 1) / h),
					2,
					gh);
				const auto ay0 = std::min(int(int64(y0) * gh / h), ay1 - 2);
				for (auto i = 0; i != columns; ++i) {
					const auto x0 = i * block;
					const auto x1 = std::min(x0 + block, w);
					const auto ax1 = std::clamp(
						int((int64(x1) * gw + w - 1) / w),
						2,
						gw);
					const auto ax0 = std::min(int(int64(x0) * gw / w), ax1 - 2);
					const auto area = (ax1 - ax0) * (ay1 - ay0);
					const auto match = [&](int dx, int dy) {
						auto sum = 0;
						for (auto y = ay0; y != ay1; ++y) {
							const auto now = gray.row(y) + ax0;
							const auto was = _gray.row(y + dy) + ax0 + dx;
							for (auto x = 0; x != ax1 - ax0; ++x) {
								sum += std::abs(int(now[x]) - int(was[x]));
							}
						}
						return sum;
					};
					const auto still = match(0, 0);
					auto best = still;
					auto bestX = 0;
					auto bestY = 0;
					for (auto dy = -kSearch; dy <= kSearch; ++dy) {
						if (ay0 + dy < 0 || ay1 + dy > gh) {
							continue;
						}
						for (auto dx = -kSearch; dx <= kSearch; ++dx) {
							if ((!dx && !dy) || ax0 + dx < 0 || ax1 + dx > gw) {
								continue;
							}
							const auto value = match(dx, dy);
							if (value < best) {
								best = value;
								bestX = dx;
								bestY = dy;
							}
						}
					}
					// A motion must explain the frame clearly better
					// than no motion does.
					if (best * 10 > still * 9 - area * 10) {
						best = still;
						bestX = bestY = 0;
					}
					if (float(best) > limit * area) {
						// Nothing like that was there: the new picture.
						continue;
					}
					const auto moveX = int(std::lround(bestX * float(w) / gw));
					const auto moveY = int(std::lround(bestY * float(h) / gh));
					for (auto y = y0; y != y1; ++y) {
						const auto row = frame.row(y);
						const auto old = output.row(
							std::clamp(y + moveY, 0, h - 1));
						for (auto x = x0; x != x1; ++x) {
							row[x] = Lerp(
								row[x],
								old[std::clamp(x + moveX, 0, w - 1)],
								keep);
						}
					}
				}
			}
		});
		Remember(frame, _output);
		_gray = gray;
	}

private:
	std::vector<Pixel> _output;
	Plane _gray;
	crl::time _refresh = 0; // The number of the last one.
	int _width = 0;
	int _height = 0;

};

//
// CRT: scanlines, an RGB mask, a curved screen with dark corners and
// a flicker.
//

class CrtEffect final : public Effect {
public:
	enum : int {
		kScanlines,
		kSize,
		kCurvature,
		kMask,
		kVignette,
		kFlicker,
		kParams,
	};

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Crt,
			"crt",
			tr::lng_oblivion_vfx_fx_crt,
			Group::Stylize,
			false,
			{
				PercentParam(
					"scanlines",
					tr::lng_oblivion_vfx_p_scanlines,
					50.),
				SliderParam(
					"size",
					tr::lng_oblivion_vfx_p_size,
					2.,
					12.,
					4.),
				PercentParam(
					"curvature",
					tr::lng_oblivion_vfx_p_curvature,
					20.),
				PercentParam("mask", tr::lng_oblivion_vfx_p_mask, 30.),
				PercentParam("vignette", tr::lng_oblivion_vfx_p_vignette, 30.),
				PercentParam("flicker", tr::lng_oblivion_vfx_p_flicker, 10.),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto lines = context.value(kScanlines) / 100.f * 0.85f;
		const auto period = std::max(context.value(kSize) * context.unit, 2.f);
		const auto curve = context.value(kCurvature) / 100.f * 0.25f;
		const auto mask = context.value(kMask) / 100.f * 0.7f;
		const auto vignette = context.value(kVignette) / 100.f * 0.6f;
		const auto flicker = context.value(kFlicker) / 100.f;

		// The lines and the mask take a part of the light away.
		const auto gain = std::min(
			1.f / ((1.f - lines * 0.5f) * (1.f - mask * 0.53f)),
			1.6f);

		// By rows: the scanlines, a bright band that rolls down and
		// the whole picture trembling a bit, 0..256 and a bit more.
		const auto seconds = context.seconds();
		const auto tremble = 1.f - flicker * 0.12f * float(
			Hash(TickAt(context.position, 30.), 0x7F4A7C15U) >> 8)
			/ 16777216.f;
		const auto band = (std::fmod(seconds * 0.35f, 1.f) * 1.4f - 0.2f) * h;
		const auto bandSize = 0.07f * h;
		auto byRow = std::vector<float>(size_t(h));
		for (auto y = 0; y != h; ++y) {
			const auto wave = 0.5f - 0.5f * std::cos(
				2.f * kPi * (float(y) + 0.5f) / period);
			const auto away = (float(y) - band) / bandSize;
			byRow[y] = (1.f - lines * wave)
				* gain
				* tremble
				* (1.f + flicker * 0.25f * std::exp(-away * away));
		}

		// By columns: stripes of the three colours.
		const auto stripe = std::max(int(std::lround(context.unit)), 1);
		auto byColumn = std::vector<std::array<ushort, 3>>(size_t(w));
		for (auto x = 0; x != w; ++x) {
			const auto lit = (x / stripe) % 3;
			for (auto c = 0; c != 3; ++c) {
				byColumn[x][c] = ushort(((c == lit)
					? (1.f + mask * 0.4f)
					: (1.f - mask)) * 256.f);
			}
		}

		const auto curved = (curve > 0.f);
		const auto from = curved
			? CopyOf(frame, context.scratch->copy)
			: frame;
		const auto cx = (w - 1) / 2.f;
		const auto cy = (h - 1) / 2.f;
		const auto scaleX = 1.f / std::max(cx, 1.f);
		const auto scaleY = 1.f / std::max(cy, 1.f);
		ForRows(w, h, [&](int fromRow, int tillRow) {
			for (auto y = fromRow; y != tillRow; ++y) {
				const auto row = frame.row(y);
				const auto ny = (float(y) - cy) * scaleY;
				const auto rowLight = byRow[y];
				for (auto x = 0; x != w; ++x) {
					const auto nx = (float(x) - cx) * scaleX;
					const auto distance = nx * nx + ny * ny;
					auto p = row[x];
					if (curved) {
						const auto bend = (1.f + curve * distance)
							/ (1.f + curve);
						const auto sx = nx * bend;
						const auto sy = ny * bend;
						p = (std::abs(sx) > 1.f || std::abs(sy) > 1.f)
							? (p & 0xFF000000U)
							: SampleClamped(
								from,
								cx + sx * cx,
								cy + sy * cy);
					}
					const auto light = int(rowLight
						* std::max(1.f - vignette * distance, 0.f)
						* 256.f);
					const auto &stripes = byColumn[x];
					const auto a = PxA(p);
					row[x] = Pack(
						a,
						std::min((PxR(p) * light * stripes[0]) >> 16, a),
						std::min((PxG(p) * light * stripes[1]) >> 16, a),
						std::min((PxB(p) * light * stripes[2]) >> 16, a));
				}
			}
		});
	}

};

//
// Grain: a noise that changes 24 times a second.
//

class GrainEffect final : public Effect {
public:
	enum : int {
		kAmount,
		kSize,
		kColored,
		kSeed,
		kParams,
	};

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Grain,
			"grain",
			tr::lng_oblivion_vfx_fx_grain,
			Group::Stylize,
			false,
			{
				PercentParam("amount", tr::lng_oblivion_vfx_p_amount, 25.),
				SliderParam(
					"size",
					tr::lng_oblivion_vfx_p_size,
					1.,
					8.,
					1.5,
					0.5),
				ToggleParam("colored", tr::lng_oblivion_vfx_p_colored, false),
				SeedParam(),
			});
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto seed = Hash(
			uint32(context.integer(kSeed)),
			TickAt(context.position, 24.),
			0x2545F491U);
		const auto colored = context.toggled(kColored);
		const auto cell = context.value(kSize) * context.unit;
		const auto coarse = (cell >= 1.25f);
		const auto strength = int(context.value(kAmount) / 100.f
			* (coarse ? 150.f : 96.f));
		if (strength <= 0) {
			return;
		}
		const auto add = [=](Pixel p, int r, int g, int b) {
			const auto a = PxA(p);
			return Pack(
				a,
				std::clamp(PxR(p) + ((r * strength) >> 7), 0, a),
				std::clamp(PxG(p) + ((g * strength) >> 7), 0, a),
				std::clamp(PxB(p) + ((b * strength) >> 7), 0, a));
		};
		if (!coarse) {
			ForRows(w, h, [&](int from, int till) {
				for (auto y = from; y != till; ++y) {
					const auto row = frame.row(y);
					auto random = Random(Hash(seed, uint32(y)));
					for (auto x = 0; x != w; ++x) {
						const auto value = random.next();
						if (!PxA(row[x])) {
							continue;
						}
						const auto r = int((value >> 8) & 0xFFU) - 128;
						row[x] = colored
							? add(
								row[x],
								r,
								int((value >> 16) & 0xFFU) - 128,
								int(value >> 24) - 128)
							: add(row[x], r, r, r);
					}
				}
			});
			return;
		}

		// Soft grains: a small picture of noise is stretched.
		auto &noise = context.scratch->reduced;
		const auto nw = int(std::ceil(w / cell)) + 2;
		const auto nh = int(std::ceil(h / cell)) + 2;
		noise.resize(size_t(nw) * nh);
		for (auto y = 0; y != nh; ++y) {
			auto random = Random(Hash(seed, uint32(y)));
			const auto row = noise.data() + size_t(y) * nw;
			for (auto x = 0; x != nw; ++x) {
				row[x] = random.next() | 0xFF000000U;
			}
		}
		const auto map = BufOf(noise, nw, nh);
		const auto scale = 1.f / cell;
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				const auto my = float(y) * scale;
				for (auto x = 0; x != w; ++x) {
					if (!PxA(row[x])) {
						continue;
					}
					const auto value = SampleClamped(map, float(x) * scale, my);
					const auto r = PxR(value) - 128;
					row[x] = colored
						? add(row[x], r, PxG(value) - 128, PxB(value) - 128)
						: add(row[x], r, r, r);
				}
			}
		});
	}

};

//
// Strobe: a part of every period the picture is black, white, negative
// or frozen.
//

class StrobeEffect final : public Effect {
public:
	enum : int {
		kFrequency,
		kDuty,
		kMode,
		kParams,
	};
	enum : int { ModeBlack, ModeWhite, ModeNegative, ModeFreeze };

	[[nodiscard]] static Info Describe() {
		return MakeInfo(
			Type::Strobe,
			"strobe",
			tr::lng_oblivion_vfx_fx_strobe,
			Group::Time,
			true,
			{
				SliderParam(
					"frequency",
					tr::lng_oblivion_vfx_p_frequency,
					1.,
					15.,
					3.,
					0.5,
					Unit::Hertz),
				PercentParam(
					"duty",
					tr::lng_oblivion_vfx_p_duty,
					50.,
					5.,
					95.),
				ChoiceParam("mode", tr::lng_oblivion_vfx_p_mode, {
					tr::lng_oblivion_vfx_o_black,
					tr::lng_oblivion_vfx_o_white,
					tr::lng_oblivion_vfx_o_negative,
					tr::lng_oblivion_vfx_o_freeze,
				}),
			});
	}

	void reset() override {
		_width = _height = 0;
	}

	void apply(const Context &context, const Buf &frame) override {
		const auto w = frame.width;
		const auto h = frame.height;
		const auto mode = context.integer(kMode);
		const auto turns = float64(context.position)
			* context.value(kFrequency)
			/ 1000.;
		const auto phase = turns - std::floor(turns);

		// The flash goes first, the plain picture after it.
		const auto flash = (phase < context.value(kDuty) / 100.);
		if (mode == ModeFreeze) {
			if (!flash || _width != w || _height != h) {
				Remember(frame, _held);
				_width = w;
				_height = h;
			} else {
				const auto held = BufOf(_held, w, h);
				for (auto y = 0; y != h; ++y) {
					memcpy(
						frame.row(y),
						held.row(y),
						size_t(w) * sizeof(Pixel));
				}
			}
			return;
		} else if (!flash) {
			return;
		}
		ForRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto row = frame.row(y);
				for (auto x = 0; x != w; ++x) {
					const auto p = row[x];
					const auto a = PxA(p);
					row[x] = (mode == ModeBlack)
						? (p & 0xFF000000U)
						: (mode == ModeWhite)
						? Pack(a, a, a, a)
						: Pack(a, a - PxR(p), a - PxG(p), a - PxB(p));
				}
			}
		});
	}

private:
	std::vector<Pixel> _held;
	int _width = 0;
	int _height = 0;

};

//
// The registry.
//

struct Registration {
	Info info;
	int declared = 0; // How many parameters the code of the effect reads.
	std::unique_ptr<Effect> (*create)() = nullptr;
};

template <typename EffectType>
[[nodiscard]] Registration Register() {
	auto result = Registration();
	result.info = EffectType::Describe();
	result.declared = EffectType::kParams;
	result.create = []() -> std::unique_ptr<Effect> {
		return std::make_unique<EffectType>();
	};
	return result;
}

// In the order of Type. Never destroyed (like the lists of effects and
// presets below): a frame may be still processed on another thread while
// the application quits.
[[nodiscard]] const std::vector<Registration> &Registry() {
	static const auto &result = *[] {
		auto &list = *new std::vector<Registration>();
		list.reserve(int(Type::kCount));
		list.push_back(Register<TrackingEffect>());
		list.push_back(Register<EdgesEffect>());
		list.push_back(Register<FeedbackEffect>());
		list.push_back(Register<DifferenceEffect>());
		list.push_back(Register<SlitScanEffect>());
		list.push_back(Register<PixelSortEffect>());
		list.push_back(Register<RgbSplitEffect>());
		list.push_back(Register<DisplaceEffect>());
		list.push_back(Register<KaleidoscopeEffect>());
		list.push_back(Register<MirrorEffect>());
		list.push_back(Register<AsciiEffect>());
		list.push_back(Register<HalftoneEffect>());
		list.push_back(Register<DitherEffect>());
		list.push_back(Register<ThresholdEffect>());
		list.push_back(Register<PosterizeEffect>());
		list.push_back(Register<FalseColorEffect>());
		list.push_back(Register<GlitchEffect>());
		list.push_back(Register<DatamoshEffect>());
		list.push_back(Register<CrtEffect>());
		list.push_back(Register<GrainEffect>());
		list.push_back(Register<StrobeEffect>());
		return &list;
	}();
	return result;
}

[[nodiscard]] const Registration &RegistrationOf(Type type) {
	const auto &list = Registry();
	const auto index = int(type);
	return list[(index >= 0 && index < int(list.size())) ? index : 0];
}

[[nodiscard]] bool Active(const Entry &entry) {
	return entry.enabled && (entry.mix > 0.);
}

// What the effect wants to see before a frame, 0 for the ones that keep
// nothing.
[[nodiscard]] crl::time EntryPreroll(const Entry &entry) {
	const auto value = [&](int index) {
		return (index < int(entry.values.size())) ? entry.values[index] : 0.;
	};
	switch (entry.type) {
	case Type::Tracking:
		return std::max(
			crl::time(600),
			crl::time(value(TrackingEffect::kTrails) * 1000.));
	case Type::Feedback:
		return std::clamp(
			crl::time(2.5 * FeedbackEffect::HalfLife(
				float(value(FeedbackEffect::kPersistence)))),
			crl::time(300),
			kMaxPreroll);
	case Type::Difference:
		return crl::time(100);
	case Type::SlitScan:
		return crl::time(value(SlitScanEffect::kDepth) * 1000.);
	case Type::Datamosh:
		return DatamoshEffect::Interval(value(DatamoshEffect::kInterval));
	case Type::Strobe:
		return (std::lround(value(StrobeEffect::kMode))
			== StrobeEffect::ModeFreeze)
			? crl::time(1000. / std::max(
				value(StrobeEffect::kFrequency),
				1.))
			: crl::time(0);
	default:
		break;
	}
	return 0;
}

// For the presets.
[[nodiscard]] Entry PresetEntry(
		Type type,
		std::initializer_list<std::pair<int, float64>> values,
		float64 mix = 1.) {
	auto result = MakeEntry(type);
	result.mix = mix;
	for (const auto &[index, value] : values) {
		if (index >= 0 && index < int(result.values.size())) {
			result.values[index] = value;
		}
	}
	return Sanitized(std::move(result));
}

} // namespace

tr::phrase<> GroupName(Group group) {
	switch (group) {
	case Group::Analysis: return tr::lng_oblivion_vfx_group_analysis;
	case Group::Time: return tr::lng_oblivion_vfx_group_time;
	case Group::Distort: return tr::lng_oblivion_vfx_group_distort;
	}
	return tr::lng_oblivion_vfx_group_stylize;
}

const std::vector<Info> &Effects() {
	static const auto &result = *[] {
		auto &list = *new std::vector<Info>();
		for (const auto &registration : Registry()) {
			list.push_back(registration.info);
		}
		return &list;
	}();
	return result;
}

const Info &EffectInfo(Type type) {
	const auto &list = Effects();
	const auto index = int(type);
	return list[(index >= 0 && index < int(list.size())) ? index : 0];
}

const Info *FindEffect(const QString &id) {
	for (const auto &info : Effects()) {
		if (id == QLatin1String(info.id)) {
			return &info;
		}
	}
	return nullptr;
}

int FindParam(Type type, const QString &id) {
	const auto &params = EffectInfo(type).params;
	for (auto i = 0; i != int(params.size()); ++i) {
		if (id == QLatin1String(params[i].id)) {
			return i;
		}
	}
	return -1;
}

Entry MakeEntry(Type type) {
	const auto &info = EffectInfo(type);
	auto result = Entry();
	result.type = info.type;
	result.values.reserve(info.params.size());
	for (const auto &param : info.params) {
		result.values.push_back(param.value);
	}
	return result;
}

float64 SanitizedValue(const Param &param, float64 value) {
	if (!std::isfinite(value)) {
		return param.value;
	}
	value = std::clamp(value, param.min, param.max);
	switch (param.kind) {
	case ParamKind::Toggle:
		return (value >= 0.5) ? 1. : 0.;
	case ParamKind::Integer:
	case ParamKind::Choice:
	case ParamKind::Color:
	case ParamKind::Seed:
		return std::clamp(std::round(value), param.min, param.max);
	case ParamKind::Slider:
		break;
	}
	if (param.step > 0.) {
		// Rounded, so 0.1 * 6 is the same number as 0.6.
		value = std::round((param.min
			+ std::round((value - param.min) / param.step) * param.step)
			* 1e6) / 1e6;
	}
	return std::clamp(value, param.min, param.max);
}

Entry Sanitized(Entry entry) {
	const auto index = int(entry.type);
	if (index < 0 || index >= int(Type::kCount)) {
		entry.type = Type::Tracking;
		entry.values.clear();
	}
	const auto &params = EffectInfo(entry.type).params;
	const auto had = int(entry.values.size());
	entry.values.resize(params.size());
	for (auto i = 0; i != int(params.size()); ++i) {
		entry.values[i] = (i < had)
			? SanitizedValue(params[i], entry.values[i])
			: params[i].value;
	}
	entry.mix = std::isfinite(entry.mix)
		? std::clamp(entry.mix, 0., 1.)
		: 1.;
	return entry;
}

QString FormatValue(const Param &param, float64 value) {
	value = SanitizedValue(param, value);
	switch (param.kind) {
	case ParamKind::Toggle:
		return QString();
	case ParamKind::Choice: {
		const auto index = int(std::lround(value));
		return (index >= 0 && index < int(param.options.size()))
			? param.options[index](tr::now)
			: QString();
	} break;
	case ParamKind::Color:
		return u"#%1"_q.arg(
			uint32(std::llround(value)) & 0xFFFFFFU,
			6,
			16,
			QChar('0')).toUpper();
	case ParamKind::Integer:
	case ParamKind::Seed:
	case ParamKind::Slider:
		break;
	}
	const auto whole = (param.kind != ParamKind::Slider)
		|| (param.step >= 1.);
	const auto tenths = param.step * 10.;
	const auto digits = whole
		? 0
		: (tenths > 0.5 && std::abs(tenths - std::round(tenths)) < 1e-6)
		? 1
		: 2;
	const auto number = QString::number(value, 'f', digits);
	switch (param.unit) {
	case Unit::Percent: return number + QChar('%');
	case Unit::Degrees: return number + QChar(0xB0);
	case Unit::Seconds:
		return number
			+ QChar(0xA0)
			+ tr::lng_oblivion_vfx_unit_seconds(tr::now);
	case Unit::Hertz:
		return number
			+ QChar(0xA0)
			+ tr::lng_oblivion_vfx_unit_hertz(tr::now);
	case Unit::Plain:
		break;
	}
	return number;
}

bool HasEnabled(const Stack &stack) {
	return std::any_of(stack.begin(), stack.end(), Active);
}

bool IsTemporal(const Stack &stack) {
	return std::any_of(stack.begin(), stack.end(), [](const Entry &entry) {
		return Active(entry) && EffectInfo(entry.type).temporal;
	});
}

crl::time Preroll(const Stack &stack) {
	auto result = crl::time(0);
	for (const auto &entry : stack) {
		if (Active(entry) && EffectInfo(entry.type).temporal) {
			result = std::max(result, EntryPreroll(Sanitized(entry)));
		}
	}
	return std::clamp(result, crl::time(0), kMaxPreroll);
}

QByteArray Serialize(const Stack &stack) {
	auto list = QJsonArray();
	for (const auto &raw : stack) {
		const auto entry = Sanitized(raw);
		const auto &info = EffectInfo(entry.type);
		auto values = QJsonObject();
		for (auto i = 0; i != int(info.params.size()); ++i) {
			values.insert(QLatin1String(info.params[i].id), entry.values[i]);
		}
		auto object = QJsonObject();
		object.insert(u"fx"_q, QString::fromLatin1(info.id));
		object.insert(u"on"_q, entry.enabled);
		object.insert(u"mix"_q, entry.mix);
		object.insert(u"p"_q, values);
		list.push_back(object);
	}
	return QJsonDocument(list).toJson(QJsonDocument::Compact);
}

Stack Deserialize(const QByteArray &data) {
	constexpr auto kMaxEntries = 64;

	auto result = Stack();
	const auto document = QJsonDocument::fromJson(data);
	if (!document.isArray()) {
		return result;
	}
	for (const auto &value : document.array()) {
		if (int(result.size()) >= kMaxEntries) {
			break;
		}
		const auto object = value.toObject();
		const auto info = FindEffect(object.value(u"fx"_q).toString());
		if (!info) {
			continue;
		}
		auto entry = MakeEntry(info->type);
		entry.enabled = object.value(u"on"_q).toBool(true);
		entry.mix = object.value(u"mix"_q).toDouble(1.);
		const auto values = object.value(u"p"_q).toObject();
		for (auto i = 0; i != int(info->params.size()); ++i) {
			const auto &param = info->params[i];
			entry.values[i] = values.value(
				QLatin1String(param.id)).toDouble(param.value);
		}
		result.push_back(Sanitized(std::move(entry)));
	}
	return result;
}

const std::vector<Preset> &Presets() {
	static const auto &result = *[] {
		using Values = std::initializer_list<std::pair<int, float64>>;
		const auto add = [](
				std::vector<Preset> &list,
				const char *id,
				tr::phrase<> name,
				Stack stack) {
			auto preset = Preset();
			preset.id = id;
			preset.name = name;
			preset.stack = std::move(stack);
			list.push_back(std::move(preset));
		};
		const auto entry = [](Type type, Values values, float64 mix = 1.) {
			return PresetEntry(type, values, mix);
		};
		auto &list = *new std::vector<Preset>();
		add(list, "tracker", tr::lng_oblivion_vfx_preset_tracker, {
			entry(Type::Tracking, {
				{ TrackingEffect::kDim, 35. },
				{ TrackingEffect::kTrails, 0.8 },
			}),
		});
		add(list, "thermal", tr::lng_oblivion_vfx_preset_thermal, {
			entry(Type::FalseColor, {}),
			entry(Type::Tracking, {
				{ TrackingEffect::kSource, TrackingEffect::SourceBright },
				{ TrackingEffect::kStyle, TrackingEffect::StyleCorners },
				{ TrackingEffect::kLinks, TrackingEffect::LinksOff },
				{ TrackingEffect::kTrails, 0. },
			}),
		});
		add(list, "neon", tr::lng_oblivion_vfx_preset_neon, {
			entry(Type::Edges, {
				{ EdgesEffect::kGlow, 70. },
				{ EdgesEffect::kColors, EdgesEffect::ColorsRainbow },
			}),
			entry(Type::Feedback, {
				{ FeedbackEffect::kPersistence, 45. },
				{ FeedbackEffect::kZoom, 6. },
			}),
		});
		add(list, "tv", tr::lng_oblivion_vfx_preset_tv, {
			entry(Type::RgbSplit, { { RgbSplitEffect::kAmount, 3. } }),
			entry(Type::Crt, {}),
			entry(Type::Grain, { { GrainEffect::kAmount, 18. } }),
		});
		add(list, "mosh", tr::lng_oblivion_vfx_preset_mosh, {
			entry(Type::Datamosh, {}),
			entry(Type::Glitch, {
				{ GlitchEffect::kAmount, 25. },
				{ GlitchEffect::kBlocks, 15. },
			}),
		});
		add(list, "terminal", tr::lng_oblivion_vfx_preset_terminal, {
			entry(Type::Ascii, {
				{ AsciiEffect::kColors, AsciiEffect::ColorsTint },
			}),
			entry(Type::Crt, {
				{ CrtEffect::kScanlines, 25. },
				{ CrtEffect::kMask, 0. },
			}),
		});
		add(list, "echo", tr::lng_oblivion_vfx_preset_echo, {
			entry(Type::Feedback, {
				{ FeedbackEffect::kPersistence, 70. },
				{ FeedbackEffect::kZoom, 0. },
				{ FeedbackEffect::kMode, FeedbackEffect::ModeBlend },
			}),
		});
		add(list, "print", tr::lng_oblivion_vfx_preset_print, {
			entry(Type::Halftone, {}),
		});
		add(list, "scanner", tr::lng_oblivion_vfx_preset_scanner, {
			entry(Type::SlitScan, { { SlitScanEffect::kDepth, 1.5 } }),
		});
		add(list, "motion", tr::lng_oblivion_vfx_preset_motion, {
			entry(Type::Difference, {
				{ DifferenceEffect::kMode, DifferenceEffect::ModeTint },
				{ DifferenceEffect::kColor, float64(0x35E0FFU) },
			}),
			entry(Type::Feedback, {
				{ FeedbackEffect::kPersistence, 55. },
				{ FeedbackEffect::kZoom, 0. },
				{ FeedbackEffect::kHue, 25. },
			}),
		});
		return &list;
	}();
	return result;
}

//
// The processor.
//

struct Processor::Private {
	Stack stack; // Sanitized.
	std::vector<std::unique_ptr<Effect>> effects; // For every entry.
	Scratch scratch;
	std::vector<Pixel> before; // For Entry::mix.
	QSize size;
	crl::time position = 0;
	bool started = false;
	bool enabled = false;
	int temporalTill = 0; // The entries before it are run by feed().
	crl::time preroll = 0;

	void reset();
	void run(QImage &frame, crl::time position, int till);
};

void Processor::Private::reset() {
	for (const auto &effect : effects) {
		effect->reset();
	}
	started = false;
}

void Processor::Private::run(QImage &frame, crl::time at, int till) {
	if (frame.format() != QImage::Format_ARGB32_Premultiplied) {
		frame = std::move(frame).convertToFormat(
			QImage::Format_ARGB32_Premultiplied);
	}
	const auto bits = frame.bits(); // Detaches.
	if (!bits || frame.width() <= 0 || frame.height() <= 0) {
		return;
	}
	if (!started
		|| (frame.size() != size)
		|| (at < position)
		|| (at - position > kResetGap)) {
		reset();
	}
	const auto buf = Buf{
		.data = reinterpret_cast<Pixel*>(bits),
		.width = frame.width(),
		.height = frame.height(),
		.stride = int(frame.bytesPerLine() / sizeof(Pixel)),
	};
	auto context = Context();
	context.position = at;
	context.step = started
		? ClampF(float(at - position), 1.f, kMaxStepMs)
		: kFrameMs;
	context.unit = std::max(
		std::min(buf.width, buf.height) / kReferenceSide,
		0.05f);
	context.scratch = &scratch;
	for (auto i = 0; i != till; ++i) {
		const auto &entry = stack[i];
		if (!Active(entry)) {
			continue;
		}
		context.values = entry.values.data();
		const auto mixed = (entry.mix < 1.);
		const auto was = mixed ? CopyOf(buf, before) : Buf();
		effects[i]->apply(context, buf);
		if (mixed) {
			const auto part = uint32(std::lround(entry.mix * 256.));
			ForRows(buf.width, buf.height, [&](int from, int tillRow) {
				for (auto y = from; y != tillRow; ++y) {
					const auto row = buf.row(y);
					const auto old = was.row(y);
					for (auto x = 0; x != buf.width; ++x) {
						row[x] = Lerp(old[x], row[x], part);
					}
				}
			});
		}
	}
	size = frame.size();
	position = at;
	started = true;
}

Processor::Processor() : _private(std::make_unique<Private>()) {
}

Processor::Processor(Stack stack) : Processor() {
	setStack(std::move(stack));
}

Processor::~Processor() = default;

void Processor::setStack(Stack stack) {
	auto &data = *_private;
	auto effects = std::vector<std::unique_ptr<Effect>>();
	effects.reserve(stack.size());
	for (auto i = 0; i != int(stack.size()); ++i) {
		stack[i] = Sanitized(std::move(stack[i]));
		const auto &entry = stack[i];
		const auto kept = (i < int(data.stack.size()))
			&& (data.stack[i].type == entry.type);
		if (kept) {
			effects.push_back(std::move(data.effects[i]));
			if (Active(entry) != Active(data.stack[i])) {
				// What it has seen will be too old when it is back.
				effects.back()->release();
			}
		} else {
			effects.push_back(RegistrationOf(entry.type).create());
		}
	}
	data.effects = std::move(effects);
	data.stack = std::move(stack);
	data.enabled = HasEnabled(data.stack);
	data.preroll = Preroll(data.stack);
	data.temporalTill = 0;
	for (auto i = 0; i != int(data.stack.size()); ++i) {
		const auto &entry = data.stack[i];
		if (Active(entry) && EffectInfo(entry.type).temporal) {
			data.temporalTill = i + 1;
		}
	}
}

const Stack &Processor::stack() const {
	return _private->stack;
}

bool Processor::empty() const {
	return !_private->enabled;
}

bool Processor::temporal() const {
	return (_private->temporalTill > 0);
}

crl::time Processor::preroll() const {
	return _private->preroll;
}

void Processor::reset() {
	_private->reset();
}

QImage Processor::process(QImage frame, crl::time position) {
	if (frame.isNull() || !_private->enabled) {
		return frame;
	}
	_private->run(frame, position, int(_private->stack.size()));
	return frame;
}

void Processor::feed(const QImage &frame, crl::time position) {
	if (frame.isNull() || !_private->temporalTill) {
		return;
	}
	auto copy = frame;
	_private->run(copy, position, _private->temporalTill);
}

std::vector<Blob> Processor::blobs() const {
	auto result = std::vector<Blob>();
	const auto &data = *_private;
	for (auto i = 0; i != int(data.stack.size()); ++i) {
		const auto &entry = data.stack[i];
		if (entry.type == Type::Tracking && Active(entry)) {
			data.effects[i]->collectBlobs(result);
			break;
		}
	}
	std::sort(result.begin(), result.end(), [](const Blob &a, const Blob &b) {
		return a.id < b.id;
	});
	return result;
}

QImage Apply(const Stack &stack, QImage frame, crl::time position) {
	auto processor = Processor(stack);
	return processor.process(std::move(frame), position);
}

QImage ProcessStill(
		Processor &processor,
		crl::time position,
		Fn<QImage(crl::time)> frameAt,
		crl::time step,
		Fn<bool()> cancelled) {
	if (!frameAt) {
		return QImage();
	}
	processor.reset();
	const auto preroll = processor.preroll();
	if (preroll > 0 && position > 0) {
		step = std::max(step, crl::time(1));
		const auto count = std::min(preroll, position) / step;
		for (auto i = count; i > 0; --i) {
			if (cancelled && cancelled()) {
				return QImage();
			}
			const auto time = position - i * step;
			processor.feed(frameAt(time), time);
		}
	}
	if (cancelled && cancelled()) {
		return QImage();
	}
	return processor.process(frameAt(position), position);
}

// Self-test.

namespace {

constexpr auto kTestSize = QSize(320, 180);
constexpr auto kTestFrames = 12;
constexpr auto kTestStep = crl::time(33);
constexpr auto kTestSpeedSize = QSize(1280, 720);
constexpr auto kTestDumpVariable = "OBLIVION_VFX_DUMP";

[[nodiscard]] QRect TestSquare(QSize size, int index) {
	const auto side = size.height() / 5;
	return QRect(
		size.width() / 10 + index * size.width() / 80,
		size.height() / 8 + index * size.height() / 90,
		side,
		side);
}

// A dim background with a texture, a bright square that goes right and
// down and a blue disc that goes slowly left above it (they never touch).
// With transparent corners and a half-transparent ring if asked.
[[nodiscard]] QImage TestFrame(
		QSize size,
		int index,
		bool transparent = false) {
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	const auto w = size.width();
	const auto h = size.height();
	const auto square = TestSquare(size, index);
	const auto discX = w * 9 / 10 - index * w / 120;
	const auto discY = h / 4 + index * h / 600;
	const auto discRadius = std::max(h / 9, 1);
	const auto cell = std::max(w / 16, 1);
	for (auto y = 0; y != h; ++y) {
		const auto row = reinterpret_cast<Pixel*>(result.scanLine(y));
		for (auto x = 0; x != w; ++x) {
			auto color = Opaque(
				28 + 36 * x / w,
				36 + 44 * y / h,
				64 + (((x / cell) + (y / cell)) & 1) * 14);
			const auto dx = x - discX;
			const auto dy = y - discY;
			if (square.contains(x, y)) {
				color = Opaque(240, 230, 90);
			} else if (dx * dx + dy * dy <= discRadius * discRadius) {
				color = Opaque(60, 200, 240);
			}
			if (transparent) {
				const auto nx = (x - w / 2.) / (0.48 * w);
				const auto ny = (y - h / 2.) / (0.48 * h);
				const auto distance = nx * nx + ny * ny;
				color = (distance < 0.7)
					? color
					: (distance < 1.)
					? Premultiplied(color, 128)
					: Pixel(0);
			}
			row[x] = color;
		}
	}
	return result;
}

[[nodiscard]] bool TestSame(const QImage &a, const QImage &b) {
	if (a.size() != b.size() || a.format() != b.format()) {
		return false;
	}
	for (auto y = 0; y != a.height(); ++y) {
		if (memcmp(
				a.constScanLine(y),
				b.constScanLine(y),
				size_t(a.width()) * sizeof(Pixel)) != 0) {
			return false;
		}
	}
	return true;
}

// The part of the pixels that differ.
[[nodiscard]] float64 TestChanged(const QImage &a, const QImage &b) {
	if (a.size() != b.size()
		|| a.format() != b.format()
		|| a.isNull()) {
		return 1.;
	}
	auto count = int64(0);
	for (auto y = 0; y != a.height(); ++y) {
		const auto one = reinterpret_cast<const Pixel*>(a.constScanLine(y));
		const auto two = reinterpret_cast<const Pixel*>(b.constScanLine(y));
		for (auto x = 0; x != a.width(); ++x) {
			count += (one[x] != two[x]) ? 1 : 0;
		}
	}
	return float64(count) / (float64(a.width()) * a.height());
}

[[nodiscard]] bool TestPremultiplied(const QImage &image) {
	if (image.format() != QImage::Format_ARGB32_Premultiplied) {
		return false;
	}
	for (auto y = 0; y != image.height(); ++y) {
		const auto row = reinterpret_cast<const Pixel*>(
			image.constScanLine(y));
		for (auto x = 0; x != image.width(); ++x) {
			const auto p = row[x];
			if (std::max({ PxR(p), PxG(p), PxB(p) }) > PxA(p)) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] bool TestOpaque(const QImage &image) {
	if (image.format() != QImage::Format_ARGB32_Premultiplied) {
		return false;
	}
	for (auto y = 0; y != image.height(); ++y) {
		const auto row = reinterpret_cast<const Pixel*>(
			image.constScanLine(y));
		for (auto x = 0; x != image.width(); ++x) {
			if (PxA(row[x]) != 255) {
				return false;
			}
		}
	}
	return true;
}

// The sum of the three channels of an average pixel.
[[nodiscard]] float64 TestLight(const QImage &image) {
	if (image.isNull()
		|| image.format() != QImage::Format_ARGB32_Premultiplied) {
		return 0.;
	}
	auto sum = int64(0);
	for (auto y = 0; y != image.height(); ++y) {
		const auto row = reinterpret_cast<const Pixel*>(
			image.constScanLine(y));
		for (auto x = 0; x != image.width(); ++x) {
			sum += PxR(row[x]) + PxG(row[x]) + PxB(row[x]);
		}
	}
	return float64(sum) / (float64(image.width()) * image.height());
}

[[nodiscard]] std::vector<QImage> TestRun(
		const Stack &stack,
		QSize size,
		int count,
		bool transparent = false) {
	auto processor = Processor(stack);
	auto result = std::vector<QImage>();
	for (auto i = 0; i != count; ++i) {
		result.push_back(processor.process(
			TestFrame(size, i, transparent),
			i * kTestStep));
	}
	return result;
}

[[nodiscard]] bool TestSame(
		const std::vector<QImage> &a,
		const std::vector<QImage> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto i = 0; i != int(a.size()); ++i) {
		if (!TestSame(a[i], b[i])) {
			return false;
		}
	}
	return true;
}

// Something of every effect is seen in the test video with it.
[[nodiscard]] Entry TestEntry(Type type) {
	auto result = MakeEntry(type);
	if (type == Type::Glitch) {
		result.values[GlitchEffect::kAmount] = 100.;
	}
	return result;
}

} // namespace

bool RunSelfTest(QStringList &log) {
	auto passed = true;
	const auto check = [&](bool ok, const QString &name, const QString &info) {
		log.push_back((ok ? u"ok   "_q : u"FAIL "_q)
			+ name
			+ (info.isEmpty() ? QString() : (u": "_q + info)));
		passed = passed && ok;
	};
	const auto percent = [](float64 part) {
		return QString::number(part * 100., 'f', 1) + QChar('%');
	};
	const auto dump = qEnvironmentVariable(kTestDumpVariable);

	// The registry.
	{
		const auto &list = Registry();
		auto ok = (int(list.size()) == int(Type::kCount))
			&& (Effects().size() == list.size());
		auto problem = QString();
		auto ids = QStringList();
		auto params = 0;
		for (auto i = 0; ok && i != int(list.size()); ++i) {
			const auto &info = list[i].info;
			const auto id = QString::fromLatin1(info.id);
			const auto fail = [&](const QString &what) {
				ok = false;
				problem = id + u": "_q + what;
			};
			if (int(info.type) != i) {
				fail(u"wrong place"_q);
			} else if (id.isEmpty() || ids.contains(id)) {
				fail(u"bad id"_q);
			} else if (list[i].declared != int(info.params.size())) {
				fail(u"%1 parameters described, %2 read"_q
					.arg(info.params.size())
					.arg(list[i].declared));
			} else if (FindEffect(id) != &EffectInfo(info.type)) {
				fail(u"not found by id"_q);
			}
			ids.push_back(id);
			auto names = QStringList();
			for (auto j = 0; ok && j != int(info.params.size()); ++j) {
				const auto &param = info.params[j];
				const auto name = QString::fromLatin1(param.id);
				if (name.isEmpty() || names.contains(name)) {
					fail(u"bad parameter id "_q + name);
				} else if (!(param.min < param.max)
					|| SanitizedValue(param, param.value) != param.value) {
					fail(u"bad range of "_q + name);
				} else if ((param.kind == ParamKind::Choice)
					&& (param.options.size() < 2
						|| param.max != float64(param.options.size()) - 1.)) {
					fail(u"bad options of "_q + name);
				} else if (FindParam(info.type, name) != j) {
					fail(u"parameter not found: "_q + name);
				}
				names.push_back(name);
				++params;
			}
		}
		check(
			ok,
			u"registry"_q,
			ok
				? u"%1 effects, %2 parameters"_q.arg(list.size()).arg(params)
				: problem);
	}

	// Values.
	{
		auto entry = MakeEntry(Type::Tracking);
		const auto defaults = entry;
		entry.values[TrackingEffect::kSensitivity] = 1e9;
		entry.values[TrackingEffect::kLimit] = -5.;
		entry.values[TrackingEffect::kWidth] = 1.6;
		entry.values[TrackingEffect::kStyle] = std::nan("");
		entry.values.resize(5);
		entry.mix = 7.;
		const auto fixed = Sanitized(entry);
		check(
			(fixed.values.size() == defaults.values.size())
				&& (fixed.values[TrackingEffect::kSensitivity] == 100.)
				&& (fixed.values[TrackingEffect::kLimit] == 1.)
				&& (fixed.values[TrackingEffect::kStyle]
					== defaults.values[TrackingEffect::kStyle])
				&& (fixed.values[TrackingEffect::kWidth]
					== defaults.values[TrackingEffect::kWidth])
				&& (fixed.mix == 1.)
				&& (Sanitized(defaults) == defaults),
			u"sanitized values"_q,
			QString());

		const auto &width = EffectInfo(
			Type::Tracking).params[TrackingEffect::kWidth];
		check(
			SanitizedValue(width, 1.6) == 1.5
				&& SanitizedValue(width, 1.7) == 1.75
				&& SanitizedValue(width, 100.) == 6.,
			u"values follow the step"_q,
			QString::number(SanitizedValue(width, 1.6)));

		const auto &text = EffectInfo(
			Type::Tracking).params[TrackingEffect::kTextSize];
		const auto &limit = EffectInfo(
			Type::Tracking).params[TrackingEffect::kLimit];
		check(
			FormatValue(width, 1.25) == u"1.25"_q
				&& FormatValue(width, 2.) == u"2.00"_q
				&& FormatValue(text, 1.3) == u"1.3"_q
				&& FormatValue(limit, 12.) == u"12"_q,
			u"values are shown with all their digits"_q,
			FormatValue(width, 1.25));

		auto bad = Entry();
		bad.type = Type(200);
		bad.values = { 1., 2. };
		const auto replaced = Sanitized(bad);
		check(
			replaced.type == Type::Tracking
				&& replaced.values == defaults.values,
			u"unknown effect type"_q,
			QString());
	}

	// Stacks.
	{
		auto stack = Stack();
		for (const auto &preset : Presets()) {
			for (const auto &entry : preset.stack) {
				stack.push_back(entry);
			}
		}
		stack[1].enabled = false;
		stack[2].mix = 0.25;
		const auto saved = Serialize(stack);
		const auto restored = Deserialize(saved);
		check(
			!stack.empty() && restored == stack,
			u"stack saved and restored"_q,
			u"%1 effects, %2 bytes"_q.arg(stack.size()).arg(saved.size()));
		check(
			Deserialize("garbage").empty()
				&& Deserialize("{}").empty()
				&& Deserialize(QByteArray()).empty(),
			u"garbage instead of a stack"_q,
			QString());
		const auto partial = Deserialize(
			"[{\"fx\":\"nothing\"},{\"fx\":\"grain\",\"p\":{\"amount\":55,"
			"\"what\":1}},{\"fx\":\"mirror\",\"on\":false}]");
		check(
			partial.size() == 2
				&& partial[0].type == Type::Grain
				&& partial[0].enabled
				&& partial[0].values[GrainEffect::kAmount] == 55.
				&& partial[0].values[GrainEffect::kSize]
					== MakeEntry(Type::Grain).values[GrainEffect::kSize]
				&& partial[1].type == Type::Mirror
				&& !partial[1].enabled,
			u"unknown effects and parameters are skipped"_q,
			QString::number(partial.size()));

		auto off = MakeEntry(Type::Feedback);
		off.enabled = false;
		auto none = MakeEntry(Type::Feedback);
		none.mix = 0.;
		const auto feedback = Stack{ MakeEntry(Type::Feedback) };
		check(
			!HasEnabled({})
				&& !HasEnabled({ off, none })
				&& HasEnabled(feedback)
				&& !IsTemporal({ MakeEntry(Type::Grain), off })
				&& IsTemporal(feedback)
				&& Preroll({ MakeEntry(Type::Grain), off }) == 0
				&& Preroll(feedback) > 0
				&& Preroll(feedback) <= kMaxPreroll
				&& Preroll({ MakeEntry(Type::SlitScan) }) == 1000
				&& Preroll({ MakeEntry(Type::Datamosh) }) == 2000,
			u"enabled, temporal, pre-roll"_q,
			u"feedback wants %1 ms"_q.arg(Preroll(feedback)));
	}

	// Painting.
	{
		auto pixels = std::vector<Pixel>(64 * 32, Pixel(0xFF000000U));
		const auto canvas = BufOf(pixels, 64, 32);
		const auto white = InkOf(Opaque(255, 255, 255), 1.f);
		FillBox(canvas, 2.f, 2.f, 4.f, 4.f, white);
		FillBox(canvas, 10.5f, 2.f, 11.5f, 3.f, white);
		auto filled = 0;
		for (const auto p : pixels) {
			filled += (p == 0xFFFFFFFFU) ? 1 : 0;
		}
		const auto halves = (canvas.at(10, 2) == canvas.at(11, 2))
			&& (PxR(canvas.at(10, 2)) > 120)
			&& (PxR(canvas.at(10, 2)) < 136);
		check(
			filled == 4 && halves,
			u"boxes cover what they should"_q,
			u"%1 full, half is %2"_q.arg(filled).arg(PxR(canvas.at(10, 2))));

		std::fill(pixels.begin(), pixels.end(), Pixel(0xFF000000U));
		PaintLine(canvas, 4.f, 4.5f, 60.f, 4.5f, 1.f, white);
		PaintLine(canvas, 62.5f, 8.f, 62.5f, 30.f, 1.f, white);
		PaintLine(canvas, 20.f, 10.f, 40.f, 30.f, 2.f, white);
		auto line = 0;
		for (auto x = 0; x != 64; ++x) {
			line += (canvas.at(x, 4) == 0xFFFFFFFFU) ? 1 : 0;
		}
		auto column = 0;
		for (auto y = 0; y != 32; ++y) {
			column += (canvas.at(62, y) == 0xFFFFFFFFU) ? 1 : 0;
		}
		check(
			line == 56
				&& column == 22
				&& PxR(canvas.at(30, 20)) > 200
				&& PxR(canvas.at(30, 26)) == 0,
			u"lines"_q,
			u"%1 and %2 pixels"_q.arg(line).arg(column));

		std::fill(pixels.begin(), pixels.end(), Pixel(0xFF000000U));
		PaintLabel(canvas, 1.f, 1.f, "X 512", 2.f, white);
		auto lit = 0;
		auto right = 0;
		auto bottom = 0;
		for (auto y = 0; y != 32; ++y) {
			for (auto x = 0; x != 64; ++x) {
				if (canvas.at(x, y) != 0xFF000000U) {
					++lit;
					right = std::max(right, x);
					bottom = std::max(bottom, y);
				}
			}
		}
		auto dots = 0;
		for (const auto symbol : QByteArray("X512")) {
			const auto glyph = GlyphOf(symbol);
			for (auto i = 0; i != kGlyphWidth; ++i) {
				for (auto bit = 0; bit != kGlyphHeight; ++bit) {
					dots += (glyph[i] >> bit) & 1;
				}
			}
		}
		auto distinct = true;
		for (auto a = '0'; a <= '9'; ++a) {
			for (auto b = char(a + 1); b <= '9'; ++b) {
				distinct = distinct
					&& (memcmp(GlyphOf(a), GlyphOf(b), kGlyphWidth) != 0);
			}
		}
		check(
			lit == dots * 4
				&& right == int(LabelWidth(5, 2.f))
				&& bottom == int(LabelHeight(2.f))
				&& distinct
				&& GlyphOf('a') == GlyphOf('A'),
			u"painted font"_q,
			u"%1 pixels for %2 dots, till %3,%4"_q
				.arg(lit)
				.arg(dots)
				.arg(right)
				.arg(bottom));
	}

	// Every effect: changes the video, gives the same result every time
	// and with any number of threads, keeps the pixels valid.
	for (const auto &info : Effects()) {
		const auto id = QString::fromLatin1(info.id);
		const auto stack = Stack{ TestEntry(info.type) };
		const auto first = TestRun(stack, kTestSize, kTestFrames);
		auto valid = (int(first.size()) == kTestFrames);
		auto changed = 0.;
		auto opaque = true;
		for (auto i = 0; valid && i != kTestFrames; ++i) {
			valid = (first[i].size() == kTestSize)
				&& (first[i].format() == QImage::Format_ARGB32_Premultiplied);
			changed = std::max(
				changed,
				TestChanged(TestFrame(kTestSize, i), first[i]));
			opaque = opaque && TestOpaque(first[i]);
		}
		const auto repeated = TestSame(
			first,
			TestRun(stack, kTestSize, kTestFrames));
		SingleThreadForTests = true;
		const auto single = TestRun(stack, kTestSize, kTestFrames);
		SingleThreadForTests = false;
		const auto threads = TestSame(first, single);

		auto alpha = true;
		for (const auto &frame : TestRun(stack, QSize(160, 90), 8, true)) {
			alpha = alpha && TestPremultiplied(frame);
		}
		auto sizes = true;
		for (const auto size : { QSize(1, 1), QSize(2, 3), QSize(37, 23) }) {
			for (const auto &frame : TestRun(stack, size, 4, true)) {
				sizes = sizes
					&& (frame.size() == size)
					&& TestPremultiplied(frame);
			}
		}
		check(
			valid
				&& (changed > 0.005)
				&& opaque
				&& repeated
				&& threads
				&& alpha
				&& sizes,
			u"effect "_q + id,
			u"changes %1"_q.arg(percent(changed))
				+ (opaque ? QString() : u", makes holes in an opaque video"_q)
				+ (repeated ? QString() : u", differs when repeated"_q)
				+ (threads ? QString() : u", depends on the threads"_q)
				+ (alpha ? QString() : u", breaks transparent pixels"_q)
				+ (sizes ? QString() : u", fails on tiny frames"_q));

		if (!dump.isEmpty()) {
			const auto big = TestRun(stack, kTestSpeedSize, 16);
			big.back().save(dump + u"/fx_"_q + id + u".png"_q);
		}
	}

	// Every option of every list, both ends of every range.
	{
		auto runs = 0;
		auto problem = QString();
		const auto tried = [&](
				const Info &info,
				int index,
				float64 value,
				const QString &name) {
			auto entry = TestEntry(info.type);
			entry.values[index] = value;
			const auto stack = Stack{ entry };
			auto fine = true;
			for (const auto &frame : TestRun(stack, QSize(160, 90), 5, true)) {
				fine = fine
					&& (frame.size() == QSize(160, 90))
					&& TestPremultiplied(frame);
			}
			for (const auto &frame : TestRun(stack, QSize(3, 2), 3, true)) {
				fine = fine && TestPremultiplied(frame);
			}
			for (const auto &frame : TestRun(stack, QSize(160, 90), 5)) {
				fine = fine && TestOpaque(frame);
			}
			if (!fine) {
				problem = QString::fromLatin1(info.id) + ' ' + name;
			}
			++runs;
			if (!dump.isEmpty()
				&& info.params[index].kind == ParamKind::Choice) {
				TestRun(stack, QSize(640, 360), 14).back().save(dump
					+ u"/option_"_q
					+ QString::fromLatin1(info.id)
					+ '_'
					+ name
					+ u".png"_q);
			}
		};
		for (const auto &info : Effects()) {
			for (auto i = 0; i != int(info.params.size()); ++i) {
				const auto &param = info.params[i];
				const auto name = QString::fromLatin1(param.id);
				if (param.kind == ParamKind::Choice) {
					for (auto j = 0; j != int(param.options.size()); ++j) {
						tried(info, i, j, name + QString::number(j));
					}
				} else {
					tried(info, i, param.min, name + u"_min"_q);
					tried(info, i, param.max, name + u"_max"_q);
				}
			}
		}
		check(
			problem.isEmpty(),
			u"all options and limits"_q,
			problem.isEmpty() ? u"%1 runs"_q.arg(runs) : problem);
	}

	// Tracking: the moving square keeps its number.
	const auto follows = [&](int source, const QString &name) {
		constexpr auto kFrames = 45;
		constexpr auto kFrom = 8;

		auto entry = MakeEntry(Type::Tracking);
		entry.values[TrackingEffect::kSource] = source;
		auto processor = Processor(Stack{ entry });
		auto ids = std::vector<int>();
		auto error = 0.;
		auto painted = true;
		for (auto i = 0; i != kFrames; ++i) {
			const auto input = TestFrame(kTestSize, i);
			const auto frame = processor.process(input, i * kTestStep);
			if (i < kFrom) {
				continue;
			}
			const auto square = TestSquare(kTestSize, i);
			const auto center = QPointF(
				(square.x() + square.width() / 2.) / kTestSize.width(),
				(square.y() + square.height() / 2.) / kTestSize.height());
			auto best = 0;
			auto nearest = 1.;
			for (const auto &blob : processor.blobs()) {
				const auto delta = blob.box.center() - center;
				const auto distance = std::hypot(delta.x(), delta.y());
				if (blob.box.contains(center) && distance < nearest) {
					best = blob.id;
					nearest = distance;
				}
			}
			if (best) {
				ids.push_back(best);
				error += nearest;
			}
			painted = painted && (TestChanged(input, frame) > 0.001);
		}
		const auto seen = int(ids.size());
		auto sequence = QString();
		for (const auto id : ids) {
			sequence += u" %1"_q.arg(id);
		}
		const auto stable = seen
			&& (std::count(ids.begin(), ids.end(), ids.front()) == seen);
		const auto average = seen ? (error / seen) : 1.;
		check(
			(seen >= (kFrames - kFrom) * 9 / 10)
				&& stable
				&& (average < 0.04)
				&& painted,
			name,
			u"seen in %1 of %2 frames, %3, off by %4 of the frame"_q
				.arg(seen)
				.arg(kFrames - kFrom)
				.arg(stable
					? u"number %1"_q.arg(ids.front())
					: (u"the number changes:"_q + sequence))
				.arg(percent(average)));
	};
	follows(TrackingEffect::SourceMotion, u"tracking by motion"_q);
	follows(TrackingEffect::SourceAuto, u"tracking, automatic"_q);
	follows(TrackingEffect::SourceBright, u"tracking of bright areas"_q);

	// A still picture: nothing moves, the bright things are found.
	{
		auto motion = MakeEntry(Type::Tracking);
		motion.values[TrackingEffect::kSource] = TrackingEffect::SourceMotion;
		auto bright = MakeEntry(Type::Tracking);
		bright.values[TrackingEffect::kSource] = TrackingEffect::SourceBright;
		auto still = Processor(Stack{ motion });
		auto light = Processor(Stack{ bright });
		auto moving = 0;
		for (auto i = 0; i != 10; ++i) {
			const auto frame = still.process(
				TestFrame(kTestSize, 5),
				i * kTestStep);
			moving += int(still.blobs().size());
		}
		const auto once = light.process(TestFrame(kTestSize, 5), 0);
		check(
			!moving && light.blobs().size() == 2,
			u"tracking of a still picture"_q,
			u"%1 moving, %2 bright"_q.arg(moving).arg(light.blobs().size()));
	}

	// The seed, the mix.
	{
		const auto seeded = [&](Type type, int index, int seed) {
			auto entry = TestEntry(type);
			entry.values[index] = seed;
			return TestRun(Stack{ entry }, kTestSize, 4);
		};
		check(
			!TestSame(
				seeded(Type::Grain, GrainEffect::kSeed, 1),
				seeded(Type::Grain, GrainEffect::kSeed, 2))
				&& !TestSame(
					seeded(Type::Glitch, GlitchEffect::kSeed, 1),
					seeded(Type::Glitch, GlitchEffect::kSeed, 2))
				&& TestSame(
					seeded(Type::Glitch, GlitchEffect::kSeed, 7),
					seeded(Type::Glitch, GlitchEffect::kSeed, 7)),
			u"the seed changes the result"_q,
			QString());

		const auto input = TestFrame(kTestSize, 3);
		auto entry = MakeEntry(Type::Threshold);
		const auto full = Apply({ entry }, input);
		entry.mix = 0.5;
		const auto half = Apply({ entry }, input);
		entry.mix = 0.;
		const auto nothing = Apply({ entry }, input);
		const auto at = QPoint(40, 90);
		const auto was = PxG(input.pixel(at));
		const auto now = PxG(full.pixel(at));
		const auto between = PxG(half.pixel(at));
		check(
			TestSame(nothing, input)
				&& !TestSame(half, input)
				&& !TestSame(half, full)
				&& std::abs(between * 2 - was - now) <= 2,
			u"the mix of an effect"_q,
			u"%1 between %2 and %3"_q.arg(between).arg(was).arg(now));
	}

	// The states: kept when the parameters change, dropped when the
	// video goes back, restored by a pre-roll.
	{
		const auto stack = Stack{
			MakeEntry(Type::Posterize),
			MakeEntry(Type::Feedback),
			MakeEntry(Type::Grain),
		};
		auto ordered = Processor(stack);
		auto last = QImage();
		for (auto i = 0; i != 8; ++i) {
			last = ordered.process(TestFrame(kTestSize, i), i * kTestStep);
		}
		const auto alone = Apply(stack, TestFrame(kTestSize, 7), 7 * kTestStep);

		auto rolled = Processor(stack);
		rolled.reset();
		for (auto i = 0; i != 7; ++i) {
			rolled.feed(TestFrame(kTestSize, i), i * kTestStep);
		}
		const auto fed = rolled.process(
			TestFrame(kTestSize, 7),
			7 * kTestStep);
		check(
			!TestSame(last, alone) && TestSame(last, fed),
			u"pre-roll gives what the playback gives"_q,
			u"a trail of %1"_q.arg(percent(TestChanged(last, alone))));

		auto paused = Processor(stack);
		auto asked = std::vector<crl::time>();
		const auto still = ProcessStill(paused, 7 * kTestStep, [&](
				crl::time time) {
			asked.push_back(time);
			return TestFrame(kTestSize, int(time / kTestStep));
		}, kTestStep);
		auto stopped = 0;
		const auto cancelled = ProcessStill(paused, 7 * kTestStep, [&](
				crl::time time) {
			return TestFrame(kTestSize, int(time / kTestStep));
		}, kTestStep, [&] {
			return (++stopped > 3);
		});
		check(
			TestSame(still, last)
				&& asked.size() == 8
				&& asked.front() == 0
				&& asked.back() == 7 * kTestStep
				&& cancelled.isNull(),
			u"a paused frame with a pre-roll"_q,
			u"%1 frames read"_q.arg(asked.size()));

		const auto back = ordered.process(TestFrame(kTestSize, 0), 0);
		check(
			TestSame(back, Apply(stack, TestFrame(kTestSize, 0), 0)),
			u"going back drops the states"_q,
			QString());

		auto changing = Processor(stack);
		for (auto i = 0; i != 7; ++i) {
			const auto frame = changing.process(
				TestFrame(kTestSize, i),
				i * kTestStep);
		}
		auto changed = stack;
		changed[2].values[GrainEffect::kAmount] = 60.;
		changing.setStack(changed);
		const auto kept = changing.process(
			TestFrame(kTestSize, 7),
			7 * kTestStep);
		const auto untrailed = Apply(
			changed,
			TestFrame(kTestSize, 7),
			7 * kTestStep);
		changed.erase(changed.begin());
		changing.setStack(changed);
		const auto dropped = changing.process(
			TestFrame(kTestSize, 8),
			8 * kTestStep);
		const auto fresh = Apply(
			changing.stack(),
			TestFrame(kTestSize, 8),
			8 * kTestStep);
		check(
			TestChanged(kept, untrailed) > 0.01
				&& TestChanged(kept, last) > 0.2
				&& TestSame(dropped, fresh),
			u"states survive a change of the parameters"_q,
			QString());

		auto toggled = Processor(stack);
		for (auto i = 0; i != 5; ++i) {
			const auto frame = toggled.process(
				TestFrame(kTestSize, i),
				i * kTestStep);
		}
		auto without = stack;
		without[1].enabled = false;
		toggled.setStack(without);
		const auto skipped = toggled.process(
			TestFrame(kTestSize, 5),
			5 * kTestStep);
		toggled.setStack(stack);
		const auto again = toggled.process(
			TestFrame(kTestSize, 6),
			6 * kTestStep);
		check(
			TestSame(
				skipped,
				Apply(without, TestFrame(kTestSize, 5), 5 * kTestStep))
				&& TestSame(
					again,
					Apply(stack, TestFrame(kTestSize, 6), 6 * kTestStep)),
			u"an effect switched off and on starts from scratch"_q,
			QString());

		auto empty = Processor();
		const auto input = TestFrame(kTestSize, 1).convertToFormat(
			QImage::Format_RGB32);
		const auto untouched = empty.process(input, 0);
		const auto converted = Apply(
			{ MakeEntry(Type::Mirror) },
			input);
		check(
			empty.empty()
				&& untouched.format() == QImage::Format_RGB32
				&& TestSame(untouched, input)
				&& converted.format() == QImage::Format_ARGB32_Premultiplied
				&& converted.size() == kTestSize
				&& Apply({ MakeEntry(Type::Mirror) }, QImage()).isNull(),
			u"empty stacks and other formats"_q,
			QString());
	}

	// Datamosh: the refreshes stand at fixed moments of the result, a
	// paused frame after a pre-roll has the smear the playback has there.
	{
		constexpr auto kStep = crl::time(50);
		constexpr auto kRefresh = crl::time(400);
		constexpr auto kAt = crl::time(650);

		auto entry = MakeEntry(Type::Datamosh);
		entry.values[DatamoshEffect::kInterval] = kRefresh / 1000.;
		const auto stack = Stack{ entry };
		const auto frameAt = [&](crl::time time) {
			return TestFrame(kTestSize, int(time / kStep));
		};
		auto ordered = Processor(stack);
		auto played = QImage();
		auto before = QImage();
		auto refreshed = QImage();
		for (auto time = crl::time(0); time <= kAt; time += kStep) {
			played = ordered.process(frameAt(time), time);
			if (time == kRefresh - kStep) {
				before = played;
			} else if (time == kRefresh) {
				refreshed = played;
			}
		}
		auto paused = Processor(stack);
		auto asked = std::vector<crl::time>();
		const auto still = ProcessStill(paused, kAt, [&](crl::time time) {
			asked.push_back(time);
			return frameAt(time);
		}, kStep);
		const auto smear = TestChanged(frameAt(kAt), still);
		check(
			Preroll(stack) == kRefresh
				&& !asked.empty()
				&& asked.front() == kAt - kRefresh
				&& (smear > 0.005)
				&& TestSame(still, played)
				&& TestSame(refreshed, frameAt(kRefresh))
				&& !TestSame(before, frameAt(kRefresh - kStep)),
			u"datamosh in a paused frame"_q,
			u"a smear of %1 after %2 frames"_q
				.arg(percent(smear))
				.arg(asked.size()));
	}

	// Feedback: a trail that is moved brings nothing from outside of the
	// frame, an opaque video stays opaque in every mode.
	{
		auto holes = QString();
		auto trail = 0.;
		for (auto mode = 0; mode != 3; ++mode) {
			auto entry = MakeEntry(Type::Feedback);
			entry.values[FeedbackEffect::kMode] = mode;
			entry.values[FeedbackEffect::kPersistence] = 90.;
			entry.values[FeedbackEffect::kZoom] = -60.;
			entry.values[FeedbackEffect::kRotation] = 120.;
			entry.values[FeedbackEffect::kShiftX] = 80.;
			entry.values[FeedbackEffect::kHue] = 30.;
			const auto frames = TestRun(Stack{ entry }, kTestSize, 10);
			for (const auto &frame : frames) {
				if (!TestOpaque(frame)) {
					holes = u"mode %1"_q.arg(mode);
				}
			}
			if (mode == FeedbackEffect::ModeBlend) {
				trail = TestChanged(TestFrame(kTestSize, 9), frames.back());
			}
		}
		check(
			holes.isEmpty() && (trail > 0.01),
			u"feedback keeps the video opaque"_q,
			holes.isEmpty()
				? u"a trail of %1"_q.arg(percent(trail))
				: (u"holes in "_q + holes));
	}

	// Edges: a big frame gets as much light as its 720p copy does.
	{
		auto lines = MakeEntry(Type::Edges);
		lines.values[EdgesEffect::kGlow] = 0.;
		const auto glowing = MakeEntry(Type::Edges);
		const auto light = [&](const Entry &entry, QSize size) {
			return TestLight(Apply({ entry }, TestFrame(size, 3)));
		};
		auto ok = true;
		auto info = QStringList();
		for (const auto size : { QSize(1920, 1080), QSize(2560, 1440) }) {
			const auto thin = light(lines, size)
				/ light(lines, kTestSpeedSize);
			const auto glow = light(glowing, size)
				/ light(glowing, kTestSpeedSize);
			ok = ok
				&& (std::abs(thin - 1.) < 0.2)
				&& (std::abs(glow - 1.) < 0.2);
			info.push_back(u"%1p: %2 of the light of 720p, %3 with a glow"_q
				.arg(size.height())
				.arg(percent(thin))
				.arg(percent(glow)));
		}
		const auto input = TestFrame(QSize(1600, 900), 3);
		const auto many = Apply({ glowing }, input);
		SingleThreadForTests = true;
		const auto single = Apply({ glowing }, input);
		SingleThreadForTests = false;
		const auto threads = TestSame(many, single);
		check(
			ok && threads && TestOpaque(many),
			u"edges in a big frame"_q,
			info.join(u"; "_q)
				+ (threads ? QString() : u", depends on the threads"_q));
	}

	// Slit-scan: the kept frames cover the whole depth when the frames
	// come much more often than they are kept.
	{
		constexpr auto kStep = crl::time(5);
		constexpr auto kFrames = 420;
		constexpr auto kHeight = 256;

		auto entry = MakeEntry(Type::SlitScan);
		entry.values[SlitScanEffect::kSmooth] = 0.;
		const auto depth = crl::time(
			entry.values[SlitScanEffect::kDepth] * 1000.);
		auto processor = Processor(Stack{ entry });
		auto last = QImage();
		for (auto i = 0; i != kFrames; ++i) {
			auto frame = QImage(
				QSize(8, kHeight),
				QImage::Format_ARGB32_Premultiplied);
			frame.fill(Opaque(i & 0xFF, i >> 8, 0));
			last = processor.process(std::move(frame), i * kStep);
		}
		const auto shown = [&](int y) {
			const auto p = *reinterpret_cast<const Pixel*>(
				last.constScanLine(y));
			return crl::time(PxR(p) | (PxG(p) << 8)) * kStep;
		};
		const auto now = (kFrames - 1) * kStep;
		const auto newest = now - shown(0);
		const auto oldest = now - shown(kHeight - 1);
		check(
			!newest && std::abs(oldest - depth) <= 4 * kStep,
			u"slit-scan covers its depth"_q,
			u"from %1 to %2 ms ago"_q.arg(newest).arg(oldest));
	}

	// Presets.
	{
		auto ok = !Presets().empty();
		auto problem = QString();
		auto ids = QStringList();
		for (const auto &preset : Presets()) {
			const auto id = QString::fromLatin1(preset.id);
			auto fine = !id.isEmpty()
				&& !ids.contains(id)
				&& !preset.stack.empty();
			for (const auto &entry : preset.stack) {
				fine = fine && (Sanitized(entry) == entry);
			}
			const auto frames = TestRun(preset.stack, kTestSize, 6);
			fine = fine
				&& (TestChanged(TestFrame(kTestSize, 5), frames.back()) > 0.01)
				&& TestPremultiplied(frames.back());
			if (!fine) {
				ok = false;
				problem = id;
			}
			ids.push_back(id);
			if (!dump.isEmpty()) {
				TestRun(preset.stack, kTestSpeedSize, 16).back().save(
					dump + u"/preset_"_q + id + u".png"_q);
			}
		}
		check(
			ok,
			u"presets"_q,
			ok ? QString::number(Presets().size()) : problem);
	}

	// The speed, only for the log.
	{
		auto frames = std::vector<QImage>();
		for (auto i = 0; i != 8; ++i) {
			frames.push_back(TestFrame(kTestSpeedSize, i));
		}
		auto slowest = 0.;
		auto slowestId = QString();
		const auto measure = [&](bool single) {
			const auto prefix = single
				? u"     ms per 720p frame, one thread: "_q
				: u"     ms per 720p frame: "_q;
			SingleThreadForTests = single;
			auto line = QStringList();
			for (const auto &info : Effects()) {
				auto processor = Processor(Stack{ TestEntry(info.type) });
				auto timer = QElapsedTimer();
				auto spent = int64(0);
				auto counted = 0;
				for (auto i = 0; i != int(frames.size()); ++i) {
					auto frame = frames[i].copy();
					timer.start();
					frame = processor.process(
						std::move(frame),
						i * kTestStep);
					if (i >= 2) {
						spent += timer.nsecsElapsed();
						++counted;
					}
				}
				const auto ms = float64(spent) / counted / 1'000'000.;
				if (single && ms > slowest) {
					slowest = ms;
					slowestId = QString::fromLatin1(info.id);
				}
				line.push_back(u"%1 %2"_q
					.arg(QString::fromLatin1(info.id))
					.arg(QString::number(ms, 'f', 1)));
				if (line.size() == 5) {
					log.push_back(prefix + line.join(u", "_q));
					line.clear();
				}
			}
			if (!line.isEmpty()) {
				log.push_back(prefix + line.join(u", "_q));
			}
			SingleThreadForTests = false;
		};
		const auto threads = ThreadCount();
		measure(false);
		measure(true);
		check(
			slowest > 0.,
			u"speed measured"_q,
			u"%1 threads, the slowest on one thread is %2 with %3 ms"_q
				.arg(threads)
				.arg(slowestId)
				.arg(QString::number(slowest, 'f', 1)));
	}
	return passed;
}

} // namespace Oblivion::VideoFx
