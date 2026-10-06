/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_fx_glitch.h"

#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_photo_fx.h"
#include "oblivion/oblivion_photo_fx_lofi.h"
#include "oblivion/oblivion_photo_panels.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/rp_widget.h"
#include "ui/wrap/vertical_layout.h"

#include <QtCore/QElapsedTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Oblivion::Photo {
namespace {

constexpr auto kPi = 3.14159265358979323846;
constexpr auto kTwoPi = float(2. * kPi);

[[nodiscard]] float GlitchUnit(const FxContext &context) {
	return float(std::clamp(context.scale, 0.001, 8.));
}

[[nodiscard]] QSize GlitchFullSize(
		const QImage &image,
		const FxContext &context) {
	if (!context.fullSize.isEmpty()) {
		return context.fullSize;
	}
	const auto scale = (context.scale > 0.) ? context.scale : 1.;
	return QSize(
		std::max(int(std::lround(image.width() / scale)), 1),
		std::max(int(std::lround(image.height() / scale)), 1));
}

[[nodiscard]] uint64 GlitchSeed(
		const FxParams &params,
		const FxContext &context,
		uint64 salt) {
	return FxHashCombine(
		FxHashCombine(context.seed, uint64(params.integer("seed"))),
		salt);
}

[[nodiscard]] float Portion(const FxParams &params, QByteArrayView id) {
	return float(params.number(id) / 100.);
}

[[nodiscard]] inline uint32 BlendPixels(uint32 a, uint32 b, uint32 weight) {
	const auto inverse = 256U - weight;
	const auto rb = (((a & 0x00FF00FFU) * inverse
		+ (b & 0x00FF00FFU) * weight) >> 8) & 0x00FF00FFU;
	const auto ag = (((a >> 8) & 0x00FF00FFU) * inverse
		+ ((b >> 8) & 0x00FF00FFU) * weight) & 0xFF00FF00U;
	return rb | ag;
}

[[nodiscard]] inline uint32 JoinChannels(
		uint32 red,
		uint32 green,
		uint32 blue) {
	const auto a = std::max({ red >> 24, green >> 24, blue >> 24 });
	return (a << 24)
		| (red & 0x00FF0000U)
		| (green & 0x0000FF00U)
		| (blue & 0x000000FFU);
}

[[nodiscard]] inline uint32 Weight256(float value) {
	return uint32(std::lround(FxClamp01(value) * 256.f));
}

// A smooth noise over a lattice of FxNoise values, about [-1, 1].
[[nodiscard]] float SmoothNoise(float x, float y, uint32 seed) {
	const auto fx = std::floor(x);
	const auto fy = std::floor(y);
	const auto ix = int(fx);
	const auto iy = int(fy);
	auto tx = x - fx;
	auto ty = y - fy;
	tx = tx * tx * (3.f - 2.f * tx);
	ty = ty * ty * (3.f - 2.f * ty);
	const auto a = FxNoise(uint32(ix), uint32(iy), seed);
	const auto b = FxNoise(uint32(ix + 1), uint32(iy), seed);
	const auto c = FxNoise(uint32(ix), uint32(iy + 1), seed);
	const auto d = FxNoise(uint32(ix + 1), uint32(iy + 1), seed);
	return FxMix(FxMix(a, b, tx), FxMix(c, d, tx), ty);
}

//
// RGB shift.
//

[[nodiscard]] bool ApplyRgbShift(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto unit = GlitchUnit(context);
	struct Offset {
		float x = 0.f;
		float y = 0.f;
	};
	const auto offsets = std::array<Offset, 3>{ {
		{
			float(params.number("red_x")) * unit,
			float(params.number("red_y")) * unit,
		},
		{
			float(params.number("green_x")) * unit,
			float(params.number("green_y")) * unit,
		},
		{
			float(params.number("blue_x")) * unit,
			float(params.number("blue_y")) * unit,
		},
	} };
	const auto jitter = Portion(params, "jitter");
	auto factors = std::vector<float>(h, 1.f);
	if (jitter > 0.f) {
		auto random = FxRandom(GlitchSeed(params, context, 0x26B5ULL));
		const auto bands = 5 + random.range(0, 9);
		auto edges = std::vector<float>();
		for (auto i = 0; i != bands; ++i) {
			edges.push_back(random.unit());
		}
		std::sort(begin(edges), end(edges));
		edges.push_back(1.f);
		auto y = 0;
		for (const auto edge : edges) {
			const auto factor = 1.f + jitter * (random.unit() * 3.f - 1.f);
			const auto till = std::clamp(int(edge * h), y, h);
			std::fill(begin(factors) + y, begin(factors) + till, factor);
			y = till;
		}
	}
	const auto source = image.copy();
	if (source.isNull()) {
		return false;
	}
	FxParallelRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto factor = factors[y];
			auto dx = std::array<int, 3>();
			auto rows = std::array<const uint32*, 3>();
			for (auto channel = 0; channel != 3; ++channel) {
				dx[channel] = int(std::lround(offsets[channel].x * factor));
				const auto dy = int(std::lround(offsets[channel].y * factor));
				rows[channel] = FxRow(source, std::clamp(y - dy, 0, h - 1));
			}
			for (auto x = 0; x != w; ++x) {
				line[x] = JoinChannels(
					rows[0][std::clamp(x - dx[0], 0, w - 1)],
					rows[1][std::clamp(x - dx[1], 0, w - 1)],
					rows[2][std::clamp(x - dx[2], 0, w - 1)]);
			}
		}
	});
	return !context.cancelled();
}

//
// Slices.
//

[[nodiscard]] bool ApplySlices(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto vertical = (params.integer("direction") == 1);
	const auto count = params.integer("count");
	const auto amount = Portion(params, "amount");
	const auto wrap = params.boolean("wrap");
	const auto split = Portion(params, "split");
	const auto across = vertical ? w : h;
	const auto along = vertical ? h : w;
	struct Shift {
		int main = 0;
		int color = 0;
	};
	auto shifts = std::vector<Shift>(across);
	auto random = FxRandom(GlitchSeed(params, context, 0x511CE5ULL));
	for (auto i = 0; i != count; ++i) {
		const auto position = random.unit();
		const auto size = (0.2f + 1.6f * random.unit()) * 0.6f / count;
		const auto move = (random.unit() * 2.f - 1.f) * amount;
		const auto color = random.unit() * split * amount * 0.25f;
		const auto from = std::clamp(int(position * across), 0, across - 1);
		const auto till = std::clamp(
			from + std::max(int(std::lround(size * across)), 1),
			from + 1,
			across);
		const auto shift = Shift{
			int(std::lround(move * along)),
			int(std::lround(color * along)),
		};
		std::fill(begin(shifts) + from, begin(shifts) + till, shift);
	}
	const auto source = image.copy();
	if (source.isNull()) {
		return false;
	}
	const auto index = [&](int value) {
		if (!wrap) {
			return std::clamp(value, 0, along - 1);
		}
		value %= along;
		return (value < 0) ? (value + along) : value;
	};
	FxParallelRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			if (!vertical) {
				const auto shift = shifts[y];
				if (!shift.main && !shift.color) {
					continue;
				}
				const auto row = FxRow(source, y);
				for (auto x = 0; x != w; ++x) {
					const auto green = row[index(x - shift.main)];
					line[x] = shift.color
						? JoinChannels(
							row[index(x - shift.main - shift.color)],
							green,
							row[index(x - shift.main + shift.color)])
						: green;
				}
				continue;
			}
			for (auto x = 0; x != w; ++x) {
				const auto shift = shifts[x];
				if (!shift.main && !shift.color) {
					continue;
				}
				const auto green = FxRow(source, index(y - shift.main))[x];
				line[x] = shift.color
					? JoinChannels(
						FxRow(source, index(y - shift.main - shift.color))[x],
						green,
						FxRow(source, index(y - shift.main + shift.color))[x])
					: green;
			}
		}
	});
	return !context.cancelled();
}

//
// Block corruption.
//

enum class BlockMode : uchar {
	Mix,
	Move,
	Channels,
	Invert,
	Noise,
	Stretch,
};

[[nodiscard]] bool ApplyBlocks(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto count = params.integer("count");
	const auto size = Portion(params, "size");
	const auto mode = BlockMode(std::clamp(params.integer("mode"), 0, 5));
	const auto grid = std::max(
		int(std::lround(params.number("grid") * GlitchUnit(context))),
		1);
	const auto snap = [&](float value) {
		return int(std::floor(value / grid)) * grid;
	};
	const auto source = image.copy();
	if (source.isNull()) {
		return false;
	}
	const auto wrapX = [&](int x) {
		x %= w;
		return (x < 0) ? (x + w) : x;
	};
	const auto wrapY = [&](int y) {
		y %= h;
		return (y < 0) ? (y + h) : y;
	};
	auto random = FxRandom(GlitchSeed(params, context, 0xB10C5ULL));
	const auto noiseSeed = uint32(GlitchSeed(params, context, 0x5701CULL));
	for (auto i = 0; i != count; ++i) {
		if (!(i % 16) && context.cancelled()) {
			return false;
		}
		const auto bw = std::max(
			snap((0.15f + 0.85f * random.unit()) * size * w),
			grid);
		const auto shape = random.unit();
		const auto bh = std::max(
			snap((0.08f + 0.6f * shape * shape) * size * h),
			grid);
		const auto left = snap(random.unit() * (w + bw)) - bw;
		const auto top = snap(random.unit() * (h + bh)) - bh;
		const auto picked = BlockMode(1 + random.range(0, 4));
		const auto variant = uint32(random.next() >> 20);
		const auto moveX = snap((random.unit() * 2.f - 1.f) * 0.3f * w);
		const auto moveY = (random.unit() < 0.3f)
			? snap((random.unit() * 2.f - 1.f) * 0.1f * h)
			: 0;
		const auto kind = (mode == BlockMode::Mix) ? picked : mode;
		const auto x0 = std::max(left, 0);
		const auto x1 = std::min(left + bw, w);
		const auto y0 = std::max(top, 0);
		const auto y1 = std::min(top + bh, h);
		if (x0 >= x1 || y0 >= y1) {
			continue;
		}
		for (auto y = y0; y != y1; ++y) {
			const auto line = FxRow(image, y);
			const auto row = FxRow(source, y);
			switch (kind) {
			case BlockMode::Mix:
			case BlockMode::Move: {
				const auto moved = FxRow(source, wrapY(y + moveY));
				const auto dx = moveX ? moveX : grid;
				for (auto x = x0; x != x1; ++x) {
					line[x] = moved[wrapX(x + dx)];
				}
			} break;
			case BlockMode::Channels: {
				const auto which = variant % 3U;
				for (auto x = x0; x != x1; ++x) {
					const auto p = row[x];
					const auto a = (p & 0xFF000000U);
					const auto r = (p >> 16) & 0xFFU;
					const auto g = (p >> 8) & 0xFFU;
					const auto b = p & 0xFFU;
					line[x] = (which == 0)
						? (a | (g << 16) | (b << 8) | r)
						: (which == 1)
						? (a | (b << 16) | (r << 8) | g)
						: JoinChannels(row[wrapX(x + (moveX ? moveX : grid))], p, p);
				}
			} break;
			case BlockMode::Invert: {
				for (auto x = x0; x != x1; ++x) {
					const auto p = row[x];
					const auto a = (p >> 24);
					line[x] = (a << 24)
						| ((a - ((p >> 16) & 0xFFU)) << 16)
						| ((a - ((p >> 8) & 0xFFU)) << 8)
						| (a - (p & 0xFFU));
				}
			} break;
			case BlockMode::Noise: {
				const auto cy = uint32((y - top) / grid);
				for (auto x = x0; x != x1; ++x) {
					const auto p = row[x];
					const auto a = (p >> 24);
					const auto value = FxHash32(
						uint32((x - left) / grid) + uint32(i) * 977U,
						cy,
						noiseSeed) & 0xFFU;
					const auto level = value * a / 255U;
					line[x] = BlendPixels(
						p,
						(a << 24) | (level << 16) | (level << 8) | level,
						210U);
				}
			} break;
			case BlockMode::Stretch: {
				if (variant & 1U) {
					const auto first = FxRow(source, y0);
					for (auto x = x0; x != x1; ++x) {
						line[x] = first[x];
					}
				} else {
					const auto first = row[x0];
					for (auto x = x0; x != x1; ++x) {
						line[x] = first;
					}
				}
			} break;
			}
		}
	}
	return !context.cancelled();
}

//
// Pixel sorting.
//

enum class SortKey : uchar {
	Luma,
	Hue,
	Saturation,
};

[[nodiscard]] inline float KeyOf(uint32 pixel, SortKey key) {
	const auto c = FxUnpack(pixel);
	if (key == SortKey::Luma) {
		return FxLuma(c.r, c.g, c.b);
	}
	const auto high = std::max({ c.r, c.g, c.b });
	const auto low = std::min({ c.r, c.g, c.b });
	const auto range = high - low;
	if (key == SortKey::Saturation) {
		return (high > 0.f) ? (range / high) : 0.f;
	} else if (range < 0.0001f) {
		return 0.f;
	}
	const auto hue = (high == c.r)
		? std::fmod((c.g - c.b) / range + 6.f, 6.f)
		: (high == c.g)
		? ((c.b - c.r) / range + 2.f)
		: ((c.r - c.g) / range + 4.f);
	return FxClamp01(hue / 6.f);
}

struct SortSpec {
	float low = 0.f;
	float high = 1.f;
	bool descending = false;
	SortKey key = SortKey::Luma;
	float length = 1.f;
	float chaos = 0.f;
};

struct SortBuffers {
	std::vector<float> keys;
	std::vector<uint64> order;
	std::vector<uint32> sorted;
};

// Sorts the runs of pixels whose key is between the thresholds. The order
// of equal keys is fixed by the position, so the result is the same with
// every standard library.
void SortLine(
		uint32 *pixels,
		int count,
		const SortSpec &spec,
		uint64 seed,
		SortBuffers &buffers) {
	auto &keys = buffers.keys;
	keys.resize(count);
	for (auto i = 0; i != count; ++i) {
		keys[i] = (pixels[i] >> 24) ? KeyOf(pixels[i], spec.key) : -1.f;
	}
	const auto inside = [&](int index) {
		return (keys[index] >= spec.low) && (keys[index] <= spec.high);
	};
	auto random = FxRandom(seed);
	const auto limit = std::max(int(spec.length * count), 2);
	for (auto i = 0; i < count;) {
		if (!inside(i)) {
			++i;
			continue;
		}
		auto longest = limit;
		auto skip = false;
		if (spec.chaos > 0.f) {
			longest = std::max(
				int(limit * (1.f - spec.chaos * random.unit())),
				2);
			skip = (random.unit() < spec.chaos * 0.35f);
		}
		auto stop = i;
		while (stop < count && stop - i < longest && inside(stop)) {
			++stop;
		}
		const auto length = stop - i;
		if (!skip && length > 1) {
			auto &order = buffers.order;
			auto &sorted = buffers.sorted;
			order.resize(length);
			sorted.resize(length);
			for (auto k = 0; k != length; ++k) {
				order[k] = (uint64(uint32(keys[i + k] * 65535.f)) << 32)
					| uint32(k);
			}
			std::sort(order.begin(), order.end());
			for (auto k = 0; k != length; ++k) {
				sorted[k] = pixels[i + int(order[k] & 0xFFFFFFFFULL)];
			}
			for (auto k = 0; k != length; ++k) {
				pixels[i + k] = spec.descending
					? sorted[length - 1 - k]
					: sorted[k];
			}
		}
		i = stop;
	}
}

[[nodiscard]] bool ApplyPixelSort(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto direction = std::clamp(params.integer("direction"), 0, 3);
	const auto low = Portion(params, "low");
	const auto high = Portion(params, "high");
	const auto spec = SortSpec{
		.low = std::min(low, high),
		.high = std::max(low, high),
		.descending = (direction == 1 || direction == 3),
		.key = SortKey(std::clamp(params.integer("key"), 0, 2)),
		.length = Portion(params, "length"),
		.chaos = Portion(params, "chaos"),
	};
	const auto seed = GlitchSeed(params, context, 0x5027ULL);
	const auto inverse = 1.f / GlitchUnit(context);
	if (direction < 2) {
		FxParallelRows(w, h, [&](int from, int till) {
			auto buffers = SortBuffers();
			for (auto y = from; y != till; ++y) {
				SortLine(
					FxRow(image, y),
					w,
					spec,
					FxHashCombine(seed, uint64(y * inverse)),
					buffers);
			}
		});
	} else {
		const auto stride = int(image.bytesPerLine() / 4);
		const auto data = reinterpret_cast<uint32*>(image.bits());
		FxParallel(w, std::max(32768 / std::max(h, 1), 1), [&](
				int from,
				int till) {
			auto buffers = SortBuffers();
			auto column = std::vector<uint32>(h);
			for (auto x = from; x != till; ++x) {
				for (auto y = 0; y != h; ++y) {
					column[y] = data[size_t(y) * stride + x];
				}
				SortLine(
					column.data(),
					h,
					spec,
					FxHashCombine(seed, uint64(x * inverse)),
					buffers);
				for (auto y = 0; y != h; ++y) {
					data[size_t(y) * stride + x] = column[y];
				}
			}
		});
	}
	return !context.cancelled();
}

//
// Smear.
//

[[nodiscard]] bool ApplySmear(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto direction = std::clamp(params.integer("direction"), 0, 3);
	const auto vertical = (direction >= 2);
	const auto backward = (direction == 1 || direction == 3);
	const auto count = params.integer("count");
	const auto size = Portion(params, "size");
	const auto length = Portion(params, "length");
	const auto fade = Portion(params, "fade");
	const auto across = vertical ? w : h;
	const auto along = vertical ? h : w;
	const auto inverse = 1.f / GlitchUnit(context);
	const auto stride = int(image.bytesPerLine() / 4);
	const auto data = reinterpret_cast<uint32*>(image.bits());
	const auto at = [&](int line, int position) -> uint32& {
		return vertical
			? data[size_t(position) * stride + line]
			: data[size_t(line) * stride + position];
	};
	auto random = FxRandom(GlitchSeed(params, context, 0x53EA2ULL));
	const auto roughSeed = uint32(GlitchSeed(params, context, 0x20F6ULL));
	for (auto i = 0; i != count; ++i) {
		if (context.cancelled()) {
			return false;
		}
		const auto position = random.unit();
		const auto thick = (0.3f + 1.4f * random.unit()) * size;
		const auto start = 0.05f + random.unit() * 0.85f;
		const auto reach = (0.4f + 0.6f * random.unit()) * length;
		const auto from = std::clamp(int(position * across), 0, across - 1);
		const auto till = std::min(
			from + std::max(int(std::lround(thick * across)), 1),
			across);
		const auto steps = int(reach * along);
		if (steps <= 0) {
			continue;
		}
		for (auto line = from; line != till; ++line) {
			const auto rough = FxNoise(
				uint32(line * inverse),
				uint32(i),
				roughSeed) * 0.02f;
			const auto origin = std::clamp(
				int((start + rough) * along),
				0,
				along - 1);
			const auto color = at(line, origin);
			for (auto k = 1; k <= steps; ++k) {
				const auto target = backward ? (origin - k) : (origin + k);
				if (target < 0 || target >= along) {
					break;
				}
				const auto t = k / float(steps);
				auto &pixel = at(line, target);
				pixel = BlendPixels(
					pixel,
					color,
					256U - Weight256(fade * t * t));
			}
		}
	}
	return !context.cancelled();
}

//
// Datamosh.
//

[[nodiscard]] bool ApplyDatamosh(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto amount = Portion(params, "amount");
	const auto cell = std::max(
		float(params.number("block")) * GlitchUnit(context),
		1.f);
	const auto distance = Portion(params, "distance");
	const auto chaos = Portion(params, "chaos");
	const auto passes = std::clamp(params.integer("passes"), 1, 8);
	const auto cols = std::max(int(std::ceil(w / cell)), 1);
	const auto rows = std::max(int(std::ceil(h / cell)), 1);
	const auto seed = uint32(GlitchSeed(params, context, 0xDA7AULL));
	const auto total = size_t(cols) * rows;
	auto cover = std::vector<float>(total);
	for (auto by = 0; by != rows; ++by) {
		for (auto bx = 0; bx != cols; ++bx) {
			cover[size_t(by) * cols + bx] = SmoothNoise(
				bx / 4.f,
				by / 4.f,
				seed) + 0.15f * FxNoise(uint32(bx), uint32(by), seed ^ 0x77U);
		}
	}
	// Exactly the asked part of the blocks moves: the threshold is taken
	// from the sorted values, the smooth noise keeps the blocks in blobs.
	auto sorted = cover;
	std::sort(begin(sorted), end(sorted));
	const auto chosen = std::clamp(
		size_t(std::lround(amount * total)),
		size_t(0),
		total);
	if (!chosen) {
		return !context.cancelled();
	}
	const auto threshold = sorted[chosen - 1];
	const auto base = float(params.number("angle") * kPi / 180.);
	struct Motion {
		int dx = 0;
		int dy = 0;
	};
	auto motion = std::vector<Motion>(total);
	auto moving = false;
	for (auto by = 0; by != rows; ++by) {
		for (auto bx = 0; bx != cols; ++bx) {
			const auto index = size_t(by) * cols + bx;
			if (cover[index] > threshold) {
				continue;
			}
			const auto angle = base
				+ chaos * float(kPi) * SmoothNoise(
					bx / 6.f,
					by / 6.f,
					seed ^ 0xA11CEU)
				+ chaos * 0.6f * FxNoise(
					uint32(bx),
					uint32(by),
					seed ^ 0xB0BU);
			const auto magnitude = distance * w
				* (0.35f + 0.65f * std::abs(FxNoise(
					uint32(bx),
					uint32(by),
					seed ^ 0xC0DEU)))
				/ passes;
			motion[index] = {
				int(std::lround(std::cos(angle) * magnitude)),
				int(std::lround(std::sin(angle) * magnitude)),
			};
			moving = moving || motion[index].dx || motion[index].dy;
		}
	}
	if (!moving) {
		return !context.cancelled();
	}
	auto columnOf = std::vector<int>(w);
	for (auto x = 0; x != w; ++x) {
		columnOf[x] = std::min(int(x / cell), cols - 1);
	}
	auto other = image.copy();
	if (other.isNull()) {
		return false;
	}
	auto current = &image;
	auto next = &other;
	for (auto pass = 0; pass != passes; ++pass) {
		if (context.cancelled()) {
			return false;
		}
		const auto &input = std::as_const(*current);
		auto &output = *next;
		FxParallelRows(w, h, [&](int from, int till) {
			for (auto y = from; y != till; ++y) {
				const auto line = FxRow(output, y);
				const auto same = FxRow(input, y);
				const auto row = motion.data()
					+ size_t(std::min(int(y / cell), rows - 1)) * cols;
				for (auto x = 0; x != w; ++x) {
					const auto move = row[columnOf[x]];
					line[x] = (move.dx || move.dy)
						? FxRow(input, std::clamp(y - move.dy, 0, h - 1))[
							std::clamp(x - move.dx, 0, w - 1)]
						: same[x];
				}
			}
		});
		std::swap(current, next);
	}
	if (current != &image) {
		image = other;
	}
	return !context.cancelled();
}

//
// Scanline jitter.
//

[[nodiscard]] bool ApplyJitter(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto unit = GlitchUnit(context);
	const auto amount = float(params.number("amount")) * unit;
	const auto group = std::max(float(params.number("height")) * unit, 0.05f);
	const auto limit = uint32(std::clamp(
		params.number("density") / 100. * 4294967295.,
		0.,
		4294967295.));
	const auto seed = uint32(GlitchSeed(params, context, 0x717732ULL));
	FxParallelRows(w, h, [&](int from, int till) {
		auto copy = std::vector<uint32>(w);
		for (auto y = from; y != till; ++y) {
			const auto index = uint32(y / group);
			if (FxHash32(index, 1U, seed) > limit) {
				continue;
			}
			const auto dx = int(std::lround(FxNoise(index, 2U, seed) * amount));
			if (!dx) {
				continue;
			}
			const auto line = FxRow(image, y);
			std::copy_n(line, w, copy.data());
			for (auto x = 0; x != w; ++x) {
				line[x] = copy[std::clamp(x - dx, 0, w - 1)];
			}
		}
	});
	return !context.cancelled();
}

//
// Wave.
//

enum class WaveShape : uchar {
	Sine,
	Triangle,
	Steps,
	Noise,
};

[[nodiscard]] float WaveValue(WaveShape shape, float argument, uint32 seed) {
	const auto part = argument - std::floor(argument);
	switch (shape) {
	case WaveShape::Sine: return std::sin(argument * kTwoPi);
	case WaveShape::Triangle: return 4.f * std::abs(part - 0.5f) - 1.f;
	case WaveShape::Steps: return (part < 0.5f) ? 1.f : -1.f;
	case WaveShape::Noise: return SmoothNoise(argument * 2.f, 0.5f, seed) * 1.6f;
	}
	return 0.f;
}

[[nodiscard]] bool ApplyWave(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto unit = GlitchUnit(context);
	const auto amplitude = float(params.number("amplitude")) * unit;
	const auto period = std::max(float(params.number("wavelength")) * unit, 1.f);
	const auto vertical = (params.integer("direction") == 1);
	const auto shape = WaveShape(std::clamp(params.integer("shape"), 0, 3));
	const auto split = Portion(params, "split");
	const auto seed = GlitchSeed(params, context, 0x3A7EULL);
	const auto phase = float(params.number("phase") / 360.)
		+ FxRandom(seed).unit();
	const auto lines = vertical ? w : h;
	auto moves = std::array<std::vector<float>, 3>();
	for (auto channel = 0; channel != 3; ++channel) {
		moves[channel].resize(lines);
		const auto shifted = phase + split * 0.25f * (channel - 1);
		for (auto i = 0; i != lines; ++i) {
			moves[channel][i] = amplitude * WaveValue(
				shape,
				(i + 0.5f) / period + shifted,
				uint32(seed));
		}
	}
	const auto source = image.copy();
	if (source.isNull()) {
		return false;
	}
	const auto colored = (split > 0.f);
	FxParallelRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			for (auto x = 0; x != w; ++x) {
				const auto i = vertical ? x : y;
				const auto sample = [&](int channel) {
					const auto move = moves[channel][i];
					return vertical
						? FxSample(source, float(x), y - move)
						: FxSample(source, x - move, float(y));
				};
				const auto green = sample(1);
				line[x] = colored
					? JoinChannels(sample(0), green, sample(2))
					: green;
			}
		}
	});
	return !context.cancelled();
}

//
// Noise bands.
//

[[nodiscard]] bool ApplyBands(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto unit = GlitchUnit(context);
	const auto count = params.integer("count");
	const auto height = Portion(params, "height");
	const auto kind = std::clamp(params.integer("kind"), 0, 2);
	const auto cell = std::max(float(params.number("grain")) * unit, 0.05f);
	const auto amount = Portion(params, "amount")
		* ((kind == 2) ? 1.f : std::clamp(cell, 0.35f, 1.f));
	const auto shift = float(params.number("shift")) * unit;
	const auto seed = uint32(GlitchSeed(params, context, 0xBA2D5ULL));
	auto strength = std::vector<float>(h, 0.f);
	auto random = FxRandom(GlitchSeed(params, context, 0x90151ULL));
	for (auto i = 0; i != count; ++i) {
		const auto position = random.unit();
		const auto size = (0.2f + 0.8f * random.unit()) * height;
		const auto from = std::clamp(int(position * h), 0, h - 1);
		const auto till = std::min(
			from + std::max(int(std::lround(size * h)), 1),
			h);
		for (auto y = from; y != till; ++y) {
			const auto t = (y - from + 0.5f) / (till - from);
			strength[y] = std::max(
				strength[y],
				FxSmoothStep(0.f, 0.3f, std::min(t, 1.f - t) * 2.f));
		}
	}
	FxParallelRows(w, h, [&](int from, int till) {
		auto copy = std::vector<uint32>(w);
		for (auto y = from; y != till; ++y) {
			const auto power = strength[y];
			if (power <= 0.f) {
				continue;
			}
			const auto line = FxRow(image, y);
			const auto ny = uint32(y / cell);
			const auto dx = int(std::lround(
				FxNoise(3U, ny, seed) * shift * power));
			if (dx) {
				std::copy_n(line, w, copy.data());
				for (auto x = 0; x != w; ++x) {
					line[x] = copy[std::clamp(x - dx, 0, w - 1)];
				}
			}
			const auto weight = Weight256(amount * power);
			const auto run = 8U + (FxHash32(7U, ny, seed) % 40U);
			for (auto x = 0; x != w; ++x) {
				const auto p = line[x];
				const auto a = (p >> 24);
				if (!a) {
					continue;
				}
				const auto nx = uint32(x / cell);
				if (kind == 2) {
					if ((FxHash32(nx / run, ny, seed ^ 0xD0U) % 5U) == 0U) {
						line[x] = BlendPixels(
							p,
							(a << 24) | (a << 16) | (a << 8) | a,
							weight);
					}
					continue;
				}
				const auto value = FxHash32(nx, ny, seed);
				const auto r = (value & 0xFFU) * a / 255U;
				const auto g = (kind == 1)
					? (((value >> 8) & 0xFFU) * a / 255U)
					: r;
				const auto b = (kind == 1)
					? (((value >> 16) & 0xFFU) * a / 255U)
					: r;
				line[x] = BlendPixels(
					p,
					(a << 24) | (r << 16) | (g << 8) | b,
					weight);
			}
		}
	});
	return !context.cancelled();
}

//
// Bit crush.
//

[[nodiscard]] uint32 CrushValue(uint32 value, int bits, int mode) {
	if (mode == 1) {
		const auto turn = 8 - bits;
		return ((value << turn) | (value >> (8 - turn))) & 0xFFU;
	} else if (mode == 2) {
		return (value ^ (value << (8 - bits))) & 0xFFU;
	}
	const auto kept = value & (0xFFU << (8 - bits)) & 0xFFU;
	auto result = 0U;
	for (auto shift = 0; shift < 8; shift += bits) {
		result |= (kept >> shift);
	}
	return result & 0xFFU;
}

[[nodiscard]] bool ApplyCrush(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	constexpr auto kVariants = 3;
	const auto w = image.width();
	const auto h = image.height();
	const auto bits = std::clamp(params.integer("bits"), 1, 7);
	const auto mode = std::clamp(params.integer("mode"), 0, 2);
	const auto weight = Weight256(Portion(params, "amount"));
	const auto bands = Portion(params, "bands");
	auto tables = std::array<std::array<uchar, 256>, kVariants>();
	for (auto variant = 0; variant != kVariants; ++variant) {
		const auto used = std::clamp(bits + variant - 1, 1, 7);
		for (auto value = 0; value != 256; ++value) {
			tables[variant][value] = uchar(CrushValue(value, used, mode));
		}
	}
	auto variants = std::vector<int>(h, (bands > 0.f) ? -1 : 1);
	if (bands > 0.f) {
		auto random = FxRandom(GlitchSeed(params, context, 0xC2054ULL));
		const auto count = 4 + random.range(0, 8);
		for (auto i = 0; i != count; ++i) {
			const auto position = random.unit();
			const auto size = bands * (0.5f + random.unit()) / count;
			const auto variant = random.range(0, kVariants - 1);
			const auto from = std::clamp(int(position * h), 0, h - 1);
			const auto till = std::min(
				from + std::max(int(std::lround(size * h)), 1),
				h);
			std::fill(begin(variants) + from, begin(variants) + till, variant);
		}
	}
	FxParallelRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			if (variants[y] < 0) {
				continue;
			}
			const auto &table = tables[variants[y]];
			const auto line = FxRow(image, y);
			for (auto x = 0; x != w; ++x) {
				const auto p = line[x];
				const auto a = (p >> 24);
				if (!a) {
					continue;
				}
				auto result = (a << 24);
				for (auto shift = 0; shift != 24; shift += 8) {
					const auto value = (p >> shift) & 0xFFU;
					const auto crushed = (a == 0xFFU)
						? uint32(table[value])
						: (uint32(table[std::min(value * 255U / a, 255U)])
							* a / 255U);
					result |= (crushed << shift);
				}
				line[x] = (weight >= 256U)
					? result
					: BlendPixels(p, result, weight);
			}
		}
	});
	return !context.cancelled();
}

//
// Databend.
//

// What a JPEG file looks like after some of its bytes were damaged. The
// picture is a sequence of blocks (MCU) in the reading order. After
// a broken place the decoder is out of step: the rest of the picture is
// shifted by some blocks (the shift wraps over the rows), the colors
// drift because the values are stored as differences from the previous
// block, and the blocks right at the break are garbage. All of that is
// done here on the pixels, with the breaks placed by the seed in the
// block grid of the full size picture, so the preview shows the same
// breaks as the export.
struct BendBreak {
	int64 index = 0;
	int64 move = 0;
	int64 garbageTill = 0;
	float luma = 0.f;
	float blue = 0.f;
	float red = 0.f;
	uint32 salt = 0;
};

[[nodiscard]] bool ApplyDatabend(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto w = image.width();
	const auto h = image.height();
	const auto full = GlitchFullSize(image, context);
	const auto block = std::max(float(params.number("block")), 4.f);
	const auto cols = std::max(int(std::ceil(full.width() / block)), 1);
	const auto rows = std::max(int(std::ceil(full.height() / block)), 1);
	const auto total = int64(cols) * rows;
	const auto count = params.integer("breaks");
	const auto reach = Portion(params, "shift");
	const auto drift = Portion(params, "drift");
	const auto garbage = Portion(params, "garbage");
	auto random = FxRandom(GlitchSeed(params, context, 0xDA7ABE2DULL));
	auto breaks = std::vector<BendBreak>(count);
	for (auto &entry : breaks) {
		entry.index = std::clamp(
			int64(random.unit() * total),
			int64(0),
			total - 1);
		entry.move = int64(std::lround(
			(random.unit() * 2.f - 1.f) * reach * cols));
		entry.luma = (random.unit() * 2.f - 1.f) * drift * 0.3f;
		entry.blue = (random.unit() * 2.f - 1.f) * drift * 0.4f;
		entry.red = (random.unit() * 2.f - 1.f) * drift * 0.4f;
		entry.garbageTill = entry.index
			+ int64(random.unit() * garbage * 14.f);
		entry.salt = uint32(random.next() >> 16);
	}
	std::sort(begin(breaks), end(breaks), [](
			const BendBreak &a,
			const BendBreak &b) {
		return (a.index != b.index) ? (a.index < b.index) : (a.salt < b.salt);
	});
	auto moved = int64(0);
	for (auto &entry : breaks) {
		moved += entry.move;
		entry.move = moved;
	}
	const auto source = image.copy();
	if (source.isNull()) {
		return false;
	}
	const auto toNominalX = full.width() / float(w);
	const auto toNominalY = full.height() / float(h);
	const auto toRenderedX = w / float(full.width());
	const auto toRenderedY = h / float(full.height());
	FxParallelRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto row = FxRow(source, y);
			const auto fullY = (y + 0.5f) * toNominalY;
			const auto blockRow = std::min(int(fullY / block), rows - 1);
			const auto inY = fullY - blockRow * block;
			const auto rowStart = int64(blockRow) * cols;
			auto current = int(std::upper_bound(
				begin(breaks),
				end(breaks),
				rowStart,
				[](int64 value, const BendBreak &entry) {
					return value < entry.index;
				}) - begin(breaks)) - 1;
			for (auto x = 0; x != w; ++x) {
				const auto fullX = (x + 0.5f) * toNominalX;
				const auto blockColumn = std::min(int(fullX / block), cols - 1);
				const auto index = rowStart + blockColumn;
				while (current + 1 < count
					&& breaks[current + 1].index <= index) {
					++current;
				}
				if (current < 0) {
					continue;
				}
				const auto &entry = breaks[current];
				const auto inX = fullX - blockColumn * block;
				if (index < entry.garbageTill) {
					const auto a = (row[x] >> 24);
					if (!a) {
						continue;
					}
					const auto mix = FxHash32(
						uint32(index),
						entry.salt,
						0x6A2BA6EU);
					const auto u = int(inX / block * 16.f) & 7;
					const auto v = int(inY / block * 16.f) & 7;
					const auto fu = float((mix >> 3) & 7U);
					const auto fv = float((mix >> 6) & 7U);
					const auto wave = std::cos((2 * u + 1) * fu * float(kPi) / 16.f)
						* std::cos((2 * v + 1) * fv * float(kPi) / 16.f);
					const auto level = 0.5f
						+ (((mix >> 9) & 0xFFU) / 255.f - 0.5f) * 0.5f
						+ wave * 0.45f;
					const auto blue = (((mix >> 17) & 0x7FU) / 127.f - 0.5f) * 0.7f;
					const auto red = (((mix >> 24) & 0x7FU) / 127.f - 0.5f) * 0.7f;
					line[x] = FxPack({
						level + 1.402f * red,
						level - 0.344f * blue - 0.714f * red,
						level + 1.772f * blue,
						a / 255.f,
					});
					continue;
				}
				auto origin = (index + entry.move) % total;
				if (origin < 0) {
					origin += total;
				}
				const auto sx = std::clamp(
					int(((origin % cols) * block + inX) * toRenderedX),
					0,
					w - 1);
				const auto sy = std::clamp(
					int(((origin / cols) * block + inY) * toRenderedY),
					0,
					h - 1);
				const auto p = FxRow(source, sy)[sx];
				if (!(p >> 24)) {
					line[x] = 0;
					continue;
				}
				auto c = FxUnpack(p);
				c.r += entry.luma + 1.402f * entry.red;
				c.g += entry.luma - 0.344f * entry.blue - 0.714f * entry.red;
				c.b += entry.luma + 1.772f * entry.blue;
				line[x] = FxPack(c);
			}
		}
	});
	return !context.cancelled();
}

[[nodiscard]] std::vector<FxText> AxisNames() {
	return {
		tr::lng_oblivion_photo_glitch_axis_horizontal,
		tr::lng_oblivion_photo_glitch_axis_vertical,
	};
}

[[nodiscard]] std::vector<FxText> DirectionNames() {
	return {
		tr::lng_oblivion_photo_glitch_dir_right,
		tr::lng_oblivion_photo_glitch_dir_left,
		tr::lng_oblivion_photo_glitch_dir_down,
		tr::lng_oblivion_photo_glitch_dir_up,
	};
}

void RegisterShifts() {
	RegisterFx({
		.id = "glitch.rgb",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_rgb,
		.params = {
			FxPixels(
				"red_x",
				tr::lng_oblivion_photo_glitch_shift_x,
				-300.,
				300.,
				-14.).under(tr::lng_oblivion_photo_glitch_red),
			FxPixels(
				"red_y",
				tr::lng_oblivion_photo_glitch_shift_y,
				-300.,
				300.,
				0.),
			FxPixels(
				"green_x",
				tr::lng_oblivion_photo_glitch_shift_x,
				-300.,
				300.,
				0.).under(tr::lng_oblivion_photo_glitch_green),
			FxPixels(
				"green_y",
				tr::lng_oblivion_photo_glitch_shift_y,
				-300.,
				300.,
				0.),
			FxPixels(
				"blue_x",
				tr::lng_oblivion_photo_glitch_shift_x,
				-300.,
				300.,
				14.).under(tr::lng_oblivion_photo_glitch_blue),
			FxPixels(
				"blue_y",
				tr::lng_oblivion_photo_glitch_shift_y,
				-300.,
				300.,
				0.),
			FxInt(
				"jitter",
				tr::lng_oblivion_photo_glitch_amount,
				0,
				100,
				30).under(tr::lng_oblivion_photo_glitch_chaos),
			FxSeed(),
		},
		.flags = kFxGeometry | kFxNeighbours | kFxSeeded,
		.order = 0,
		.apply = ApplyRgbShift,
		.identity = [](const FxParams &params) {
			for (const auto id : {
				"red_x",
				"red_y",
				"green_x",
				"green_y",
				"blue_x",
				"blue_y",
			}) {
				if (params.number(id) != 0.) {
					return false;
				}
			}
			return true;
		},
	});
	RegisterFx({
		.id = "glitch.slices",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_slices,
		.params = {
			FxInt("count", tr::lng_oblivion_photo_glitch_count, 1, 200, 24),
			FxInt(
				"amount",
				tr::lng_oblivion_photo_glitch_amplitude,
				0,
				100,
				12,
				u"%"_q),
			FxChoice(
				"direction",
				tr::lng_oblivion_photo_glitch_direction,
				AxisNames()),
			FxBool("wrap", tr::lng_oblivion_photo_glitch_wrap, true),
			FxInt("split", tr::lng_oblivion_photo_glitch_split, 0, 100, 0),
			FxSeed(),
		},
		.flags = kFxGeometry | kFxNeighbours | kFxSeeded,
		.order = 10,
		.apply = ApplySlices,
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0;
		},
	});
	RegisterFx({
		.id = "glitch.blocks",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_blocks,
		.params = {
			FxInt("count", tr::lng_oblivion_photo_glitch_count, 0, 300, 30),
			FxInt(
				"size",
				tr::lng_oblivion_photo_glitch_size,
				1,
				60,
				14,
				u"%"_q),
			FxChoice(
				"mode",
				tr::lng_oblivion_photo_glitch_mode,
				{
					tr::lng_oblivion_photo_glitch_mode_mix,
					tr::lng_oblivion_photo_glitch_mode_move,
					tr::lng_oblivion_photo_glitch_mode_channels,
					tr::lng_oblivion_photo_glitch_mode_invert,
					tr::lng_oblivion_photo_glitch_mode_noise,
					tr::lng_oblivion_photo_glitch_mode_stretch,
				}),
			FxPixels("grid", tr::lng_oblivion_photo_glitch_grid, 1., 64., 16.),
			FxSeed(),
		},
		.flags = kFxGeometry | kFxNeighbours | kFxSeeded,
		.order = 20,
		.apply = ApplyBlocks,
		.identity = [](const FxParams &params) {
			return params.integer("count") <= 0;
		},
	});
	RegisterFx({
		.id = "glitch.sort",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_sort,
		.params = {
			FxInt("low", tr::lng_oblivion_photo_glitch_low, 0, 100, 25, u"%"_q),
			FxInt(
				"high",
				tr::lng_oblivion_photo_glitch_high,
				0,
				100,
				80,
				u"%"_q),
			FxChoice(
				"direction",
				tr::lng_oblivion_photo_glitch_direction,
				DirectionNames(),
				2),
			FxChoice(
				"key",
				tr::lng_oblivion_photo_glitch_key,
				{
					tr::lng_oblivion_photo_glitch_key_luma,
					tr::lng_oblivion_photo_glitch_key_hue,
					tr::lng_oblivion_photo_glitch_key_saturation,
				}),
			FxInt(
				"length",
				tr::lng_oblivion_photo_glitch_length,
				1,
				100,
				60,
				u"%"_q),
			FxInt("chaos", tr::lng_oblivion_photo_glitch_chaos, 0, 100, 25),
			FxSeed(),
		},
		.flags = kFxGeometry | kFxNeighbours | kFxSeeded,
		.order = 30,
		.apply = ApplyPixelSort,
	});
	RegisterFx({
		.id = "glitch.smear",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_smear,
		.params = {
			FxInt("count", tr::lng_oblivion_photo_glitch_count, 1, 80, 10),
			FxFloat(
				"size",
				tr::lng_oblivion_photo_glitch_size,
				0.5,
				40.,
				5.,
				1,
				u"%"_q),
			FxInt(
				"length",
				tr::lng_oblivion_photo_glitch_length,
				0,
				100,
				55,
				u"%"_q),
			FxChoice(
				"direction",
				tr::lng_oblivion_photo_glitch_direction,
				DirectionNames()),
			FxInt("fade", tr::lng_oblivion_photo_glitch_fade, 0, 100, 35),
			FxSeed(),
		},
		.flags = kFxGeometry | kFxNeighbours | kFxSeeded,
		.order = 40,
		.apply = ApplySmear,
		.identity = [](const FxParams &params) {
			return params.integer("length") <= 0;
		},
	});
	RegisterFx({
		.id = "glitch.mosh",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_mosh,
		.params = {
			FxInt("amount", tr::lng_oblivion_photo_glitch_amount, 0, 100, 55),
			FxPixels(
				"block",
				tr::lng_oblivion_photo_glitch_block,
				4.,
				128.,
				24.),
			FxFloat(
				"distance",
				tr::lng_oblivion_photo_glitch_distance,
				0.,
				40.,
				6.,
				1,
				u"%"_q),
			FxAngle("angle", tr::lng_oblivion_photo_glitch_angle),
			FxInt("chaos", tr::lng_oblivion_photo_glitch_chaos, 0, 100, 40),
			FxInt("passes", tr::lng_oblivion_photo_glitch_passes, 1, 8, 4),
			FxSeed(),
		},
		.flags = kFxGeometry | kFxNeighbours | kFxSeeded,
		.order = 50,
		.apply = ApplyDatamosh,
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0
				|| params.number("distance") <= 0.;
		},
	});
}

void RegisterSignals() {
	RegisterFx({
		.id = "glitch.jitter",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_jitter,
		.params = {
			FxPixels(
				"amount",
				tr::lng_oblivion_photo_glitch_amplitude,
				0.,
				400.,
				24.),
			FxInt(
				"density",
				tr::lng_oblivion_photo_glitch_density,
				0,
				100,
				45,
				u"%"_q),
			FxPixels(
				"height",
				tr::lng_oblivion_photo_glitch_line_height,
				1.,
				80.,
				3.),
			FxSeed(),
		},
		.flags = kFxGeometry | kFxNeighbours | kFxSeeded,
		.order = 60,
		.apply = ApplyJitter,
		.identity = [](const FxParams &params) {
			return params.number("amount") <= 0.
				|| params.integer("density") <= 0;
		},
	});
	RegisterFx({
		.id = "glitch.wave",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_wave,
		.params = {
			FxPixels(
				"amplitude",
				tr::lng_oblivion_photo_glitch_amplitude,
				0.,
				400.,
				18.),
			FxPixels(
				"wavelength",
				tr::lng_oblivion_photo_glitch_wavelength,
				4.,
				3000.,
				140.),
			FxAngle("phase", tr::lng_oblivion_photo_glitch_phase, 0., 0., 360.),
			FxChoice(
				"direction",
				tr::lng_oblivion_photo_glitch_direction,
				AxisNames()),
			FxChoice(
				"shape",
				tr::lng_oblivion_photo_glitch_shape,
				{
					tr::lng_oblivion_photo_glitch_shape_sine,
					tr::lng_oblivion_photo_glitch_shape_triangle,
					tr::lng_oblivion_photo_glitch_shape_steps,
					tr::lng_oblivion_photo_glitch_shape_noise,
				}),
			FxInt("split", tr::lng_oblivion_photo_glitch_split, 0, 100, 0),
			FxSeed(),
		},
		.flags = kFxGeometry | kFxNeighbours | kFxSeeded,
		.order = 70,
		.apply = ApplyWave,
		.identity = [](const FxParams &params) {
			return params.number("amplitude") <= 0.;
		},
	});
	RegisterFx({
		.id = "glitch.bands",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_bands,
		.params = {
			FxInt("count", tr::lng_oblivion_photo_glitch_count, 1, 60, 7),
			FxFloat(
				"height",
				tr::lng_oblivion_photo_glitch_band_height,
				0.2,
				30.,
				4.,
				1,
				u"%"_q),
			FxInt("amount", tr::lng_oblivion_photo_glitch_amount, 0, 100, 75),
			FxChoice(
				"kind",
				tr::lng_oblivion_photo_glitch_kind,
				{
					tr::lng_oblivion_photo_glitch_kind_static,
					tr::lng_oblivion_photo_glitch_kind_color,
					tr::lng_oblivion_photo_glitch_kind_dropouts,
				}),
			FxPixels("grain", tr::lng_oblivion_photo_glitch_grain, 1., 24., 3.),
			FxPixels("shift", tr::lng_oblivion_photo_glitch_shift, 0., 300., 0.),
			FxSeed(),
		},
		.flags = kFxNeighbours | kFxSeeded,
		.order = 80,
		.apply = ApplyBands,
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0
				&& params.number("shift") <= 0.;
		},
	});
	RegisterFx({
		.id = "glitch.crush",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_crush,
		.params = {
			FxInt("bits", tr::lng_oblivion_photo_glitch_bits, 1, 7, 2),
			FxChoice(
				"mode",
				tr::lng_oblivion_photo_glitch_mode,
				{
					tr::lng_oblivion_photo_glitch_crush_cut,
					tr::lng_oblivion_photo_glitch_crush_rotate,
					tr::lng_oblivion_photo_glitch_crush_xor,
				}),
			FxInt("amount", tr::lng_oblivion_photo_glitch_amount, 0, 100, 100),
			FxInt(
				"bands",
				tr::lng_oblivion_photo_glitch_in_bands,
				0,
				100,
				60,
				u"%"_q),
			FxSeed(),
		},
		.flags = kFxSeeded,
		.order = 90,
		.apply = ApplyCrush,
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0;
		},
	});
	RegisterFx({
		.id = "glitch.bend",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_bend,
		.params = {
			FxInt("breaks", tr::lng_oblivion_photo_glitch_breaks, 0, 40, 4),
			FxInt(
				"shift",
				tr::lng_oblivion_photo_glitch_shift,
				0,
				100,
				25,
				u"%"_q),
			FxInt("drift", tr::lng_oblivion_photo_glitch_drift, 0, 100, 35),
			FxPixels("block", tr::lng_oblivion_photo_glitch_block, 8., 64., 16.),
			FxInt("garbage", tr::lng_oblivion_photo_glitch_garbage, 0, 100, 30),
			FxSeed(),
		},
		.flags = kFxGeometry | kFxNeighbours | kFxSeeded,
		.order = 100,
		.apply = ApplyDatabend,
		.identity = [](const FxParams &params) {
			return params.integer("breaks") <= 0;
		},
	});
}

void RegisterGlitchPresets() {
	const auto whole = [](int value) {
		return FxValue::Integer(value);
	};
	const auto real = [](double value) {
		return FxValue::Number(value);
	};
	RegisterFxPreset({
		.id = "glitch.preset_signal",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_preset_signal,
		.stack = {
			MakeFx("glitch.jitter", {
				{ "amount", real(20.) },
				{ "density", whole(35) },
			}),
			MakeFx("glitch.bands", {
				{ "count", whole(6) },
				{ "amount", whole(80) },
				{ "shift", real(40.) },
			}),
			MakeFx("glitch.rgb", {
				{ "red_x", real(-10.) },
				{ "blue_x", real(10.) },
				{ "jitter", whole(40) },
			}),
		},
		.order = 0,
	});
	RegisterFxPreset({
		.id = "glitch.preset_file",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_preset_file,
		.stack = {
			MakeFx("glitch.bend", {
				{ "breaks", whole(5) },
				{ "garbage", whole(45) },
			}),
			MakeFx("glitch.blocks", {
				{ "count", whole(14) },
				{ "size", whole(10) },
			}),
		},
		.order = 1,
	});
	RegisterFxPreset({
		.id = "glitch.preset_mosh",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_preset_mosh,
		.stack = {
			MakeFx("glitch.mosh", {
				{ "amount", whole(65) },
				{ "distance", real(8.) },
				{ "passes", whole(6) },
			}),
			MakeFx("glitch.smear", {
				{ "count", whole(6) },
				{ "direction", whole(2) },
			}),
		},
		.order = 2,
	});
	RegisterFxPreset({
		.id = "glitch.preset_vapor",
		.group = FxGroup::Glitch,
		.name = tr::lng_oblivion_photo_glitch_preset_vapor,
		.stack = {
			MakeFx("glitch.sort", {
				{ "low", whole(55) },
				{ "high", whole(100) },
				{ "length", whole(30) },
			}),
			MakeFx("glitch.wave", {
				{ "amplitude", real(10.) },
				{ "wavelength", real(260.) },
				{ "split", whole(60) },
			}),
			MakeFx("glitch.rgb", {
				{ "red_x", real(-8.) },
				{ "blue_x", real(8.) },
				{ "jitter", whole(0) },
			}),
		},
		.order = 3,
	});
}

const auto Registered = FxRegistrar([] {
	RegisterShifts();
	RegisterSignals();
	RegisterGlitchPresets();
});

//
// Self-test.
//

[[nodiscard]] bool GlitchSamePixels(const QImage &a, const QImage &b) {
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

[[nodiscard]] bool GlitchValidPixels(const QImage &image) {
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

[[nodiscard]] bool RunGlitchSelfTest(QStringList &log) {
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
		.seed = 2424,
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
		"glitch.rgb",
		"glitch.slices",
		"glitch.blocks",
		"glitch.sort",
		"glitch.smear",
		"glitch.mosh",
		"glitch.jitter",
		"glitch.wave",
		"glitch.bands",
		"glitch.crush",
		"glitch.bend",
	};

	// Every effect: registered, has a seed, changes the picture, keeps
	// valid pixels of a picture with transparency, repeats itself, and
	// another seed (or another instance) gives another picture.
	{
		auto registered = true;
		auto changes = true;
		auto valid = true;
		auto repeats = true;
		auto seeded = true;
		auto timer = QElapsedTimer();
		auto timings = QStringList();
		for (const auto &id : ids) {
			const auto name = QString::fromLatin1(id);
			const auto descriptor = FindFx(id);
			const auto seed = descriptor ? descriptor->param("seed") : nullptr;
			if (!descriptor
				|| descriptor->group != FxGroup::Glitch
				|| !seed
				|| seed->kind != FxParamKind::Seed
				|| !(descriptor->flags & kFxSeeded)) {
				registered = false;
				info(name + u" is not registered with a seed"_q);
				continue;
			}
			const auto one = MakeFx(id, { { "seed", FxValue::Integer(11) } });
			const auto two = MakeFx(id, { { "seed", FxValue::Integer(12) } });
			timer.start();
			const auto first = run(one, full, context);
			timings.push_back(u"%1 %2"_q.arg(
				name.mid(7),
				QString::number(timer.nsecsElapsed() / 1e6, 'f', 1)));
			if (first.isNull() || FxImageDifference(first, full) < 0.05) {
				changes = false;
				info(name + u" does not change the picture"_q);
			}
			if (!GlitchSamePixels(first, run(one, full, context))) {
				repeats = false;
				info(name + u" is not deterministic"_q);
			}
			auto other = context;
			other.seed = 999;
			if (GlitchSamePixels(first, run(two, full, context))
				|| GlitchSamePixels(first, run(one, full, other))) {
				seeded = false;
				info(name + u" ignores the seed"_q);
			}
			const auto transparent = run(one, cut, context);
			if (transparent.isNull()
				|| transparent.size() != cut.size()
				|| !GlitchValidPixels(transparent)) {
				valid = false;
				info(name + u" breaks premultiplied pixels"_q);
			}
		}
		check(registered, u"%1 glitch effects are registered, with seeds"_q.arg(
			ids.size()));
		check(changes, u"every effect changes the picture by default"_q);
		check(repeats, u"the same seed gives the same picture"_q);
		check(seeded, u"another seed or instance gives another picture"_q);
		check(valid, u"every effect keeps valid premultiplied pixels"_q);
		info(u"640x480, ms: "_q + timings.join(u", "_q));
	}

	// Zero strength.
	{
		const auto zero = FxValue::Integer(0);
		const auto none = std::vector<FxInstance>{
			MakeFx("glitch.rgb", {
				{ "red_x", FxValue::Number(0.) },
				{ "blue_x", FxValue::Number(0.) },
			}),
			MakeFx("glitch.slices", { { "amount", zero } }),
			MakeFx("glitch.blocks", { { "count", zero } }),
			MakeFx("glitch.smear", { { "length", zero } }),
			MakeFx("glitch.mosh", { { "amount", zero } }),
			MakeFx("glitch.mosh", { { "distance", FxValue::Number(0.) } }),
			MakeFx("glitch.jitter", { { "amount", FxValue::Number(0.) } }),
			MakeFx("glitch.wave", { { "amplitude", FxValue::Number(0.) } }),
			MakeFx("glitch.bands", { { "amount", zero } }),
			MakeFx("glitch.crush", { { "amount", zero } }),
			MakeFx("glitch.bend", { { "breaks", zero } }),
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
		check(identity, u"zero strength changes nothing (%1 cases)"_q.arg(
			none.size()));
		// Nothing between the thresholds: nothing to sort.
		const auto flat = run(MakeFx("glitch.sort", {
			{ "low", FxValue::Integer(100) },
			{ "high", FxValue::Integer(100) },
		}), full, context);
		check(
			FxImageDifference(flat, full) < 0.2,
			u"pixel sorting leaves pixels outside the thresholds"_q);
	}

	// Pixel sorting really sorts and only moves pixels around.
	{
		auto line = std::vector<uint32>();
		for (const auto value : { 200, 10, 250, 90, 40, 130, 5, 60 }) {
			line.push_back(0xFF000000U
				| (uint32(value) << 16)
				| (uint32(value) << 8)
				| uint32(value));
		}
		auto buffers = SortBuffers();
		auto up = line;
		SortLine(up.data(), int(up.size()), SortSpec(), 1, buffers);
		auto down = line;
		SortLine(
			down.data(),
			int(down.size()),
			SortSpec{ .descending = true },
			1,
			buffers);
		check(
			std::is_sorted(begin(up), end(up))
				&& std::is_sorted(rbegin(down), rend(down))
				&& std::is_permutation(begin(up), end(up), begin(line)),
			u"a line is sorted by brightness in both directions"_q);
		auto part = line;
		SortLine(
			part.data(),
			int(part.size()),
			SortSpec{ .low = 0.3f, .high = 1.f },
			1,
			buffers);
		check(
			part[0] == line[0]
				&& part[1] == line[1]
				&& part[2] == line[3]
				&& part[3] == line[2]
				&& part[4] == line[4]
				&& part[5] == line[5]
				&& part[6] == line[6]
				&& part[7] == line[7],
			u"only the runs between the thresholds are sorted"_q);
		const auto sorted = run(MakeFx("glitch.sort", {
			{ "low", FxValue::Integer(0) },
			{ "high", FxValue::Integer(100) },
			{ "chaos", FxValue::Integer(0) },
		}), full, context);
		auto sum = uint64(0);
		auto sumSorted = uint64(0);
		for (auto y = 0; y != full.height(); ++y) {
			for (auto x = 0; x != full.width(); ++x) {
				sum += FxRow(full, y)[x] & 0xFFU;
				sumSorted += FxRow(sorted, y)[x] & 0xFFU;
			}
		}
		check(
			!sorted.isNull() && sum == sumSorted,
			u"pixel sorting keeps every pixel"_q);
	}

	// Bit crush tables.
	check(
		CrushValue(0xFF, 3, 0) == 0xFF
			&& CrushValue(0x00, 3, 0) == 0x00
			&& CrushValue(0x9F, 1, 0) == 0xFF
			&& CrushValue(0x7F, 1, 0) == 0x00
			&& CrushValue(0x81, 7, 1) == 0x03
			&& CrushValue(0x0F, 4, 2) == 0xFF,
		u"bit crush: truncate, rotate, xor"_q);

	// Presets only set parameters.
	{
		auto count = 0;
		auto fine = true;
		for (const auto preset : AllFxPresets()) {
			if (!preset->id.startsWith("glitch.")) {
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
			count == 4 && fine,
			u"%1 presets are valid parameter sets of registered effects"_q.arg(
				count));
	}

	// Random things are placed in the coordinates of the full picture, so
	// the preview shows the same breaks as the export.
	{
		const auto large = FxTestImage(1280, 960);
		const auto half = FxResized(large, large.size() / 2);
		const auto compare = [&](const QByteArray &id) {
			const auto instance = MakeFx(id);
			auto exported = large;
			auto preview = half;
			const auto done = ApplyFx(exported, instance, FxContext{
				.scale = 1.,
				.seed = 5,
				.fullSize = large.size(),
			}) && ApplyFx(preview, instance, FxContext{
				.scale = 0.5,
				.seed = 5,
				.fullSize = large.size(),
				.preview = true,
			});
			return done
				? FxImageDifference(preview, FxResized(exported, half.size()))
				: 255.;
		};
		auto worst = 0.;
		auto values = QStringList();
		for (const auto &id : ids) {
			const auto difference = compare(id);
			worst = std::max(worst, difference);
			values.push_back(u"%1 %2"_q.arg(
				QString::fromLatin1(id).mid(7),
				QString::number(difference, 'f', 2)));
		}
		check(
			worst < 6.,
			u"the preview matches the export: "_q + values.join(u", "_q));
	}

	// Cancelling.
	{
		const auto cancel = std::atomic<bool>(true);
		auto image = full;
		check(
			!ApplyFx(image, MakeFx("glitch.mosh"), FxContext{
				.fullSize = full.size(),
				.cancel = &cancel,
			}),
			u"a cancelled effect reports it"_q);
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
				QString::fromLatin1(id).mid(7),
				QString::number(timer.nsecsElapsed() / 1e6, 'f', 0),
				done ? QString() : u" (failed)"_q));
		}
		info(u"2048x1536, ms: "_q + timings.join(u", "_q));
	}
	return ok;
}

const auto GlitchSelfTest = SelfTestRegistrar(
	SelfTestSuite::Fx,
	"glitch",
	&RunGlitchSelfTest);

} // namespace

// Interface: the snapshot scenes.

namespace {

constexpr auto kPanelSceneWidth = 340;
// The padding of the editor panel plus the padding of an effect card: the
// parameters get exactly the width they have in a card of the editor.
constexpr auto kPanelScenePadding = 28;

[[nodiscard]] std::vector<FxInstance> GlitchPresetStack(QByteArrayView id) {
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

[[nodiscard]] object_ptr<Ui::RpWidget> GlitchParamsScene(
		not_null<QWidget*> parent,
		not_null<Controller*> controller,
		const QByteArray &id) {
	using namespace EditorUi;
	auto result = object_ptr<Ui::VerticalLayout>(parent);
	if (const auto descriptor = FindFx(id)) {
		result->add(
			CreateParamsPanel(result.data(), ParamsPanelArgs{
				.params = descriptor->params,
				.values = DefaultFxParams(*descriptor),
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
	RegisterFxGalleryScene(u"photo_glitch_gallery"_q, FxGroup::Glitch, false);
	RegisterFxGalleryScene(u"photo_glitch_presets"_q, FxGroup::Glitch, true);

	// The photo is opened at the size of a real one (see
	// FxSceneSampleImage): shifts and blocks are measured in its pixels.
	RegisterEditorScene({
		.name = u"photo_glitch_file"_q,
		.document = [] { return FxSceneSampleDocument(); },
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			AppendToActiveLayer(
				controller,
				GlitchPresetStack("glitch.preset_file"));
		},
	});
	RegisterEditorScene({
		.name = u"photo_glitch_sort"_q,
		.document = [] { return FxSceneSampleDocument(); },
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			AppendToActiveLayer(controller, { MakeFx("glitch.sort") });
		},
	});

	// Both cards of a preset the way the Layer tab shows them.
	RegisterFxStackScene(u"photo_glitch_stack"_q, "glitch.preset_file");

	const auto width = EditorUi::Px(kPanelSceneWidth);
	RegisterPanelScene({
		.name = u"photo_glitch_rgb_panel"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return GlitchParamsScene(parent, controller, "glitch.rgb");
		},
	});
	RegisterPanelScene({
		.name = u"photo_glitch_bend_panel"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return GlitchParamsScene(parent, controller, "glitch.bend");
		},
	});
});

} // namespace
} // namespace Oblivion::Photo
