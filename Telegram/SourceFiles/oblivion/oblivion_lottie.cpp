/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie.h"

#include "core/application.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_common.h"
#include "lottie/lottie_wrap.h"
#include "ui/image/image_prepare.h"

#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QThread>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>

#include <rlottie.h>
#include <rlottiecommon.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Oblivion::Lottie {
namespace {

// Same as Images::UnpackGzip: tdesktop can't unpack larger .tgs files
// and plays only up to ::Lottie::kMaxFileSize (2 MB) of JSON anyway.
constexpr auto kMaxUnpackedSize = 5 * 1024 * 1024;
constexpr auto kMaxRenderSide = 4096;
constexpr auto kRasterMinSide = 512;
constexpr auto kCacheSize = 2;
constexpr auto kMaxFrames = 10'000'000;
constexpr auto kMaxSide = 100'000;

// Real animations never run slower than 1 fps (lib_lottie refuses less
// than 0.5), both bounds keep the frame / duration math of the callers
// far away from overflows.
constexpr auto kMinFrameRate = 1.;
constexpr auto kMaxFrameRate = 1000.;

// Locale-independent number conversions (Qt may call setlocale()).
[[nodiscard]] bool ParseNumber(std::string_view text, double &result) {
	auto ok = false;
	result = QByteArray::fromRawData(
		text.data(),
		qsizetype(text.size())).toDouble(&ok);
	return ok && std::isfinite(result);
}

[[nodiscard]] std::string FormatNumber(double value) {
	if (!std::isfinite(value)) {
		return "0";
	}
	return QByteArray::number(value, 'g', 10).toStdString();
}

// Portable core: JSON document, color adjustment and the SVG writer.
// Uses only the standard library and rlottie's render tree structures.

constexpr auto kMaxJsonDepth = 256;
constexpr auto kMaxSvgDepth = 128;

// Every value costs about 100 bytes in the tree, while dense arrays like
// "0,0,0" cost only 2 bytes of text per value. Real animations have 4-6
// bytes per value, so their kMaxUnpackedSize of text fits the budget.
constexpr auto kMaxJsonValues = 2'000'000;

// JSON document that keeps the member order and the original number
// text: rlottie's streaming parser depends on the key order ("ty" first
// in shape items, "ddd" before "ks" in layers), so the usual JSON
// libraries that sort keys would break animations.
struct Json {
	enum class Type : unsigned char {
		Null,
		False,
		True,
		Number,
		String,
		Array,
		Object,
	};

	Type type = Type::Null;
	double number = 0.;
	std::string text; // Raw number text or raw string contents.
	std::vector<Json> items;
	std::vector<std::pair<std::string, Json>> members;

	[[nodiscard]] bool is(Type value) const {
		return (type == value);
	}
	[[nodiscard]] Json *find(std::string_view key) {
		for (auto &[name, value] : members) {
			if (name == key) {
				return &value;
			}
		}
		return nullptr;
	}
	[[nodiscard]] const Json *find(std::string_view key) const {
		for (const auto &[name, value] : members) {
			if (name == key) {
				return &value;
			}
		}
		return nullptr;
	}
	[[nodiscard]] std::optional<double> numberAt(std::string_view key) const {
		const auto value = find(key);
		return (value && value->is(Type::Number))
			? std::make_optional(value->number)
			: std::nullopt;
	}
	void set(std::string_view key, Json value) {
		if (const auto existing = find(key)) {
			*existing = std::move(value);
		} else {
			members.emplace_back(std::string(key), std::move(value));
		}
	}

	[[nodiscard]] static Json MakeNumber(double value) {
		auto result = Json();
		result.type = Type::Number;
		result.number = value;
		result.text = FormatNumber(value);
		return result;
	}
	[[nodiscard]] static Json MakeBool(bool value) {
		auto result = Json();
		result.type = value ? Type::True : Type::False;
		return result;
	}
	[[nodiscard]] static Json MakeObject() {
		auto result = Json();
		result.type = Type::Object;
		return result;
	}
};

class JsonReader final {
public:
	explicit JsonReader(std::string_view data)
	: _ptr(data.data())
	, _end(data.data() + data.size()) {
	}

	[[nodiscard]] bool read(Json &result) {
		if ((_end - _ptr) >= 3
			&& static_cast<unsigned char>(_ptr[0]) == 0xEF
			&& static_cast<unsigned char>(_ptr[1]) == 0xBB
			&& static_cast<unsigned char>(_ptr[2]) == 0xBF) {
			_ptr += 3;
		}
		skipSpace();
		if (!value(result, 0)) {
			return false;
		}
		skipSpace();
		while (_ptr != _end && *_ptr == '\0') {
			++_ptr;
		}
		return (_ptr == _end);
	}

private:
	void skipSpace() {
		while (_ptr != _end
			&& (*_ptr == ' '
				|| *_ptr == '\n'
				|| *_ptr == '\r'
				|| *_ptr == '\t')) {
			++_ptr;
		}
	}
	[[nodiscard]] bool consume(char ch) {
		if (_ptr != _end && *_ptr == ch) {
			++_ptr;
			return true;
		}
		return false;
	}
	[[nodiscard]] bool literal(
			std::string_view text,
			Json &result,
			Json::Type type) {
		if (size_t(_end - _ptr) < text.size()
			|| std::string_view(_ptr, text.size()) != text) {
			return false;
		}
		_ptr += text.size();
		result.type = type;
		return true;
	}
	[[nodiscard]] bool value(Json &result, int depth) {
		if (_ptr == _end
			|| depth > kMaxJsonDepth
			|| ++_values > kMaxJsonValues) {
			return false;
		}
		switch (*_ptr) {
		case '{': return object(result, depth);
		case '[': return array(result, depth);
		case '"':
			result.type = Json::Type::String;
			return string(result.text);
		case 't': return literal("true", result, Json::Type::True);
		case 'f': return literal("false", result, Json::Type::False);
		case 'n': return literal("null", result, Json::Type::Null);
		}
		return number(result);
	}
	[[nodiscard]] bool object(Json &result, int depth) {
		++_ptr;
		result.type = Json::Type::Object;
		skipSpace();
		if (consume('}')) {
			return true;
		}
		while (true) {
			skipSpace();
			if (_ptr == _end || *_ptr != '"') {
				return false;
			}
			auto key = std::string();
			if (!string(key)) {
				return false;
			}
			skipSpace();
			if (!consume(':')) {
				return false;
			}
			skipSpace();
			result.members.emplace_back(std::move(key), Json());
			if (!value(result.members.back().second, depth + 1)) {
				return false;
			}
			skipSpace();
			if (consume(',')) {
				continue;
			}
			return consume('}');
		}
	}
	[[nodiscard]] bool array(Json &result, int depth) {
		++_ptr;
		result.type = Json::Type::Array;
		skipSpace();
		if (consume(']')) {
			return true;
		}
		while (true) {
			skipSpace();
			result.items.emplace_back();
			if (!value(result.items.back(), depth + 1)) {
				return false;
			}
			skipSpace();
			if (consume(',')) {
				continue;
			}
			return consume(']');
		}
	}
	[[nodiscard]] bool string(std::string &result) {
		++_ptr;
		const auto start = _ptr;
		while (_ptr != _end) {
			const auto ch = *_ptr;
			if (ch == '"') {
				result.assign(start, _ptr);
				++_ptr;
				return true;
			} else if (ch == '\\') {
				++_ptr;
				if (_ptr == _end) {
					return false;
				}
			}
			++_ptr;
		}
		return false;
	}
	[[nodiscard]] bool number(Json &result) {
		const auto start = _ptr;
		const auto digits = [&] {
			const auto from = _ptr;
			while (_ptr != _end && *_ptr >= '0' && *_ptr <= '9') {
				++_ptr;
			}
			return (_ptr != from);
		};
		const auto sign = [&] {
			if (_ptr != _end && (*_ptr == '-' || *_ptr == '+')) {
				++_ptr;
			}
		};
		if (_ptr != _end && *_ptr == '-') {
			++_ptr;
		}
		if (!digits()) {
			return false;
		}
		if (consume('.') && !digits()) {
			return false;
		}
		if (_ptr != _end && (*_ptr == 'e' || *_ptr == 'E')) {
			++_ptr;
			sign();
			if (!digits()) {
				return false;
			}
		}
		result.type = Json::Type::Number;
		result.text.assign(start, _ptr);
		return ParseNumber(result.text, result.number);
	}

	const char *_ptr = nullptr;
	const char *_end = nullptr;
	int _values = 0;

};

void WriteJson(const Json &value, std::string &out) {
	switch (value.type) {
	case Json::Type::Null: out += "null"; return;
	case Json::Type::False: out += "false"; return;
	case Json::Type::True: out += "true"; return;
	case Json::Type::Number: out += value.text; return;
	case Json::Type::String:
		out += '"';
		out += value.text;
		out += '"';
		return;
	case Json::Type::Array: {
		out += '[';
		auto first = true;
		for (const auto &item : value.items) {
			if (!first) {
				out += ',';
			}
			first = false;
			WriteJson(item, out);
		}
		out += ']';
	} return;
	case Json::Type::Object: {
		out += '{';
		auto first = true;
		for (const auto &[key, item] : value.members) {
			if (!first) {
				out += ',';
			}
			first = false;
			out += '"';
			out += key;
			out += "\":";
			WriteJson(item, out);
		}
		out += '}';
	} return;
	}
}

[[nodiscard]] std::string SerializeJson(const Json &value) {
	auto result = std::string();
	WriteJson(value, result);
	return result;
}

// Calls callback for every value of an animatable property:
// {"a":0,"k":value}, {"a":1,"k":[{"s":value,"e":value},...]} or
// a bare legacy value.
template <typename Callback>
void ForEachPropertyValue(Json &property, Callback &&callback) {
	const auto value = property.is(Json::Type::Object)
		? property.find("k")
		: &property;
	if (!value) {
		return;
	}
	if (value->is(Json::Type::Array)
		&& !value->items.empty()
		&& value->items.front().is(Json::Type::Object)) {
		for (auto &keyframe : value->items) {
			if (!keyframe.is(Json::Type::Object)) {
				continue;
			}
			if (const auto start = keyframe.find("s")) {
				callback(*start);
			}
			if (const auto end = keyframe.find("e")) {
				callback(*end);
			}
		}
	} else {
		callback(*value);
	}
}

struct ColorAdjust {
	double hue = 0.; // Degrees, [0, 360).
	double saturation = 0.; // [-1, 1].
	double lightness = 0.; // [-1, 1].
};

struct ColorStats {
	int found = 0;
	int changed = 0;
};

void AdjustRgb(double &r, double &g, double &b, const ColorAdjust &adjust) {
	r = std::clamp(r, 0., 1.);
	g = std::clamp(g, 0., 1.);
	b = std::clamp(b, 0., 1.);
	const auto max = std::max({ r, g, b });
	const auto min = std::min({ r, g, b });
	const auto delta = max - min;
	auto lightness = (max + min) / 2.;
	auto saturation = 0.;
	auto hue = 0.;
	if (delta > 1e-12) {
		saturation = (lightness > 0.5)
			? (delta / (2. - max - min))
			: (delta / (max + min));
		if (max == r) {
			hue = (g - b) / delta + ((g < b) ? 6. : 0.);
		} else if (max == g) {
			hue = (b - r) / delta + 2.;
		} else {
			hue = (r - g) / delta + 4.;
		}
		hue *= 60.;
	}
	hue = std::fmod(hue + adjust.hue, 360.);
	if (hue < 0.) {
		hue += 360.;
	}
	if (adjust.saturation > 0. && saturation > 0.) {
		saturation += (1. - saturation) * adjust.saturation;
	} else if (adjust.saturation < 0.) {
		saturation *= (1. + adjust.saturation);
	}
	if (adjust.lightness > 0.) {
		lightness += (1. - lightness) * adjust.lightness;
	} else if (adjust.lightness < 0.) {
		lightness *= (1. + adjust.lightness);
	}
	saturation = std::clamp(saturation, 0., 1.);
	lightness = std::clamp(lightness, 0., 1.);
	if (saturation <= 0.) {
		r = g = b = lightness;
		return;
	}
	const auto q = (lightness < 0.5)
		? (lightness * (1. + saturation))
		: (lightness + saturation - lightness * saturation);
	const auto p = 2. * lightness - q;
	const auto channel = [&](double t) {
		if (t < 0.) {
			t += 1.;
		} else if (t > 1.) {
			t -= 1.;
		}
		if (t < 1. / 6.) {
			return p + (q - p) * 6. * t;
		} else if (t < 0.5) {
			return q;
		} else if (t < 2. / 3.) {
			return p + (q - p) * (2. / 3. - t) * 6.;
		}
		return p;
	};
	const auto normalized = hue / 360.;
	r = std::clamp(channel(normalized + 1. / 3.), 0., 1.);
	g = std::clamp(channel(normalized), 0., 1.);
	b = std::clamp(channel(normalized - 1. / 3.), 0., 1.);
}

class ColorAdjuster final {
public:
	explicit ColorAdjuster(ColorAdjust adjust) : _adjust(adjust) {
	}

	void process(Json &root) {
		walk(root, false, 0);
	}
	[[nodiscard]] const ColorStats &stats() const {
		return _stats;
	}

private:
	[[nodiscard]] bool setComponent(Json &number, double value) {
		if (std::abs(number.number - value) < 1e-9) {
			return false;
		}
		// Drop floating point noise like 1e-17 from the HSL round trip.
		number = Json::MakeNumber(std::round(value * 1e7) / 1e7);
		return true;
	}

	// r, g, b numbers at items[index], items[index + 1], items[index + 2].
	void colorAt(Json &array, size_t index) {
		if (!array.is(Json::Type::Array) || array.items.size() < index + 3) {
			return;
		}
		auto &r = array.items[index];
		auto &g = array.items[index + 1];
		auto &b = array.items[index + 2];
		if (!r.is(Json::Type::Number)
			|| !g.is(Json::Type::Number)
			|| !b.is(Json::Type::Number)) {
			return;
		}
		++_stats.found;
		const auto max = std::max({ r.number, g.number, b.number });
		const auto min = std::min({ r.number, g.number, b.number });
		if (min < 0. || max > 255.) {
			return;
		}
		// Very old exports store 0..255 components.
		const auto scale = (max > 1. + 1e-6) ? 255. : 1.;
		auto red = r.number / scale;
		auto green = g.number / scale;
		auto blue = b.number / scale;
		AdjustRgb(red, green, blue, _adjust);
		auto changed = setComponent(r, red * scale);
		changed = setComponent(g, green * scale) || changed;
		changed = setComponent(b, blue * scale) || changed;
		if (changed) {
			++_stats.changed;
		}
	}

	void colorValue(Json &value) {
		if (value.is(Json::Type::Array)
			&& !value.items.empty()
			&& value.items.front().is(Json::Type::Number)) {
			colorAt(value, 0);
		}
	}

	void colorProperty(Json &property) {
		ForEachPropertyValue(property, [&](Json &value) {
			colorValue(value);
		});
	}

	// {"p": color stop count, "k": property of [offset, r, g, b, ...]
	// followed by [offset, alpha, ...] opacity stops}.
	void gradientProperty(Json &gradient) {
		if (!gradient.is(Json::Type::Object)) {
			return;
		}
		auto points = -1;
		if (const auto p = gradient.numberAt("p")) {
			if (*p >= 0. && *p < 1e6) {
				points = int(*p);
			}
		}
		const auto stops = gradient.find("k");
		if (!stops) {
			return;
		}
		ForEachPropertyValue(*stops, [&](Json &value) {
			if (!value.is(Json::Type::Array)) {
				return;
			}
			const auto size = int(value.items.size());
			auto count = points;
			if (count < 0 || count > size / 4) {
				// Same fallback as rlottie for legacy files.
				count = size / 4;
			}
			for (auto i = 0; i != count; ++i) {
				colorAt(value, size_t(i) * 4 + 1);
			}
		});
	}

	void hexColor(Json &value) {
		const auto &text = value.text;
		if (text.size() != 7 || text[0] != '#') {
			return;
		}
		auto parsed = 0;
		for (auto i = 1; i != 7; ++i) {
			const auto ch = text[i];
			const auto digit = (ch >= '0' && ch <= '9')
				? (ch - '0')
				: (ch >= 'a' && ch <= 'f')
				? (ch - 'a' + 10)
				: (ch >= 'A' && ch <= 'F')
				? (ch - 'A' + 10)
				: -1;
			if (digit < 0) {
				return;
			}
			parsed = (parsed << 4) | digit;
		}
		++_stats.found;
		const auto adjusted = adjustPacked(parsed);
		if (adjusted == parsed) {
			return;
		}
		static constexpr char kDigits[] = "0123456789abcdef";
		auto result = std::string("#");
		for (auto shift = 20; shift >= 0; shift -= 4) {
			result += kDigits[(adjusted >> shift) & 0x0F];
		}
		value.text = result;
		++_stats.changed;
	}

	[[nodiscard]] int adjustPacked(int rgb) {
		auto red = ((rgb >> 16) & 0xFF) / 255.;
		auto green = ((rgb >> 8) & 0xFF) / 255.;
		auto blue = (rgb & 0xFF) / 255.;
		AdjustRgb(red, green, blue, _adjust);
		const auto part = [](double value) {
			return std::clamp(int(std::lround(value * 255.)), 0, 255);
		};
		return (part(red) << 16) | (part(green) << 8) | part(blue);
	}

	// Emoji skin tone replacements: [{"o": 0xRRGGBB, "f12": ..., ...}].
	void fitz(Json &value) {
		if (!value.is(Json::Type::Array)) {
			return;
		}
		for (auto &entry : value.items) {
			if (!entry.is(Json::Type::Object)) {
				continue;
			}
			for (auto &[key, color] : entry.members) {
				if ((key != "o" && (key.empty() || key[0] != 'f'))
					|| !color.is(Json::Type::Number)
					|| color.number < 0.
					|| color.number > double(0xFFFFFF)) {
					continue;
				}
				++_stats.found;
				const auto rgb = int(color.number);
				const auto adjusted = adjustPacked(rgb);
				if (adjusted != rgb) {
					color.number = adjusted;
					color.text = std::to_string(adjusted);
					++_stats.changed;
				}
			}
		}
	}

	// "fc" / "sc": solid layer hex string, text document color array or
	// text animator color property.
	void anyColor(Json &value) {
		if (value.is(Json::Type::String)) {
			hexColor(value);
		} else {
			colorProperty(value);
		}
	}

	void walk(Json &value, bool effects, int depth) {
		if (depth > kMaxJsonDepth) {
			return;
		} else if (value.is(Json::Type::Array)) {
			for (auto &item : value.items) {
				walk(item, effects, depth + 1);
			}
			return;
		} else if (!value.is(Json::Type::Object)) {
			return;
		}
		auto skip = std::string_view();
		if (const auto type = value.find("ty")) {
			if (type->is(Json::Type::String)) {
				if (type->text == "fl" || type->text == "st") {
					if (const auto color = value.find("c")) {
						colorProperty(*color);
					}
					skip = "c";
				} else if (type->text == "gf" || type->text == "gs") {
					if (const auto gradient = value.find("g")) {
						gradientProperty(*gradient);
					}
					skip = "g";
				}
			} else if (effects
				&& type->is(Json::Type::Number)
				&& type->number == 2.) {
				// Color control value of an effect.
				if (const auto color = value.find("v")) {
					colorProperty(*color);
				}
				skip = "v";
			}
		}
		for (auto &[key, member] : value.members) {
			if (!skip.empty() && key == skip) {
				continue;
			} else if (key == "fc" || key == "sc") {
				anyColor(member);
			} else if (key == "fitz" && !depth) {
				fitz(member);
			} else {
				walk(member, effects || (key == "ef"), depth + 1);
			}
		}
	}

	ColorAdjust _adjust;
	ColorStats _stats;

};

[[nodiscard]] Json StaticPointProperty(double x, double y) {
	auto point = Json();
	point.type = Json::Type::Array;
	point.items.push_back(Json::MakeNumber(x));
	point.items.push_back(Json::MakeNumber(y));
	auto result = Json::MakeObject();
	result.set("a", Json::MakeNumber(0.));
	result.set("k", std::move(point));
	return result;
}

// The render tree reports mask modes, but a "mode": "n" (none) mask is
// reported as "add" while rlottie's raster skips it, and a layer with
// "hasMask" and nothing to mask with is not drawn by rlottie at all.
// Both are baked into the JSON copy used for the SVG export in a way
// that keeps rlottie's own output unchanged: such masks are removed and
// such layers are hidden.
[[nodiscard]] bool PrepareMasksForSvg(Json &root) {
	auto changed = false;
	const auto layers = [&](Json *list) {
		if (!list || !list->is(Json::Type::Array)) {
			return;
		}
		for (auto &layer : list->items) {
			if (!layer.is(Json::Type::Object)) {
				continue;
			}
			const auto hasMask = layer.find("hasMask");
			if (!hasMask || !hasMask->is(Json::Type::True)) {
				continue;
			}
			const auto masks = layer.find("masksProperties");
			auto kept = std::vector<Json>();
			if (masks && masks->is(Json::Type::Array)) {
				for (auto &mask : masks->items) {
					if (!mask.is(Json::Type::Object)) {
						kept.push_back(std::move(mask));
						continue;
					}
					const auto mode = mask.find("mode");
					const auto letter = (mode
						&& mode->is(Json::Type::String)
						&& !mode->text.empty())
						? mode->text[0]
						: 'n';
					if (letter != 'a'
						&& letter != 's'
						&& letter != 'i'
						&& letter != 'f') {
						changed = true;
						continue;
					}
					kept.push_back(std::move(mask));
				}
			}
			if (kept.empty()) {
				layer.set("hd", Json::MakeBool(true));
				changed = true;
			} else {
				masks->items = std::move(kept);
			}
		}
	};
	layers(root.find("layers"));
	if (const auto assets = root.find("assets")) {
		if (assets->is(Json::Type::Array)) {
			for (auto &asset : assets->items) {
				if (asset.is(Json::Type::Object)) {
					layers(asset.find("layers"));
				}
			}
		}
	}
	return changed;
}

// rlottie paints a gradient through the full matrix of its shape group
// (VSpanData::setupMatrix), but the render tree only reports the mapped
// points and a radius scaled uniformly, which is wrong for non-uniform
// scale or skew. To recover the matrix the export builds two more trees
// from copies where every gradient is linear from (0, 0) to (L, 0) or
// from (0, L) to (L, L): their mapped points give M * (0, 0), M * (L, 0)
// and M * (0, L).
constexpr auto kProbeLength = 1000.;

[[nodiscard]] Json MakeGradientProbe(const Json &root, bool vertical) {
	auto result = root;
	const auto startY = vertical ? kProbeLength : 0.;
	const auto process = [&](const auto &self, Json &value, int depth) {
		if (depth > kMaxJsonDepth) {
			return;
		} else if (value.is(Json::Type::Array)) {
			for (auto &item : value.items) {
				self(self, item, depth + 1);
			}
			return;
		} else if (!value.is(Json::Type::Object)) {
			return;
		}
		const auto type = value.find("ty");
		if (type
			&& type->is(Json::Type::String)
			&& (type->text == "gf" || type->text == "gs")) {
			value.set("t", Json::MakeNumber(1.));
			value.set("s", StaticPointProperty(0., startY));
			value.set("e", StaticPointProperty(kProbeLength, startY));
		}
		for (auto &[key, member] : value.members) {
			self(self, member, depth + 1);
		}
	};
	process(process, result, 0);
	return result;
}

struct SvgMatrix {
	// x' = a * x + c * y + e, y' = b * x + d * y + f, like SVG matrix().
	double a = 1.;
	double b = 0.;
	double c = 0.;
	double d = 1.;
	double e = 0.;
	double f = 0.;

	[[nodiscard]] double determinant() const {
		return a * d - b * c;
	}
	[[nodiscard]] std::pair<double, double> map(double x, double y) const {
		return { a * x + c * y + e, b * x + d * y + f };
	}
	[[nodiscard]] std::pair<double, double> unmap(double x, double y) const {
		const auto det = determinant();
		x -= e;
		y -= f;
		return { (d * x - c * y) / det, (a * y - b * x) / det };
	}
	// rlottie's getScale() of the matrix, used for radii and widths.
	[[nodiscard]] double rlottieScale() const {
		const auto k = double(1.41421f);
		const auto x = (a + c) * k;
		const auto y = (b + d) * k;
		return std::sqrt(x * x + y * y) / 2.;
	}
};

using GradientMatrices = std::unordered_map<const LOTNode*, SvgMatrix>;

// Walks the main tree and the two probe trees (same structure, only
// gradient points differ) in the same order and pairs gradient nodes.
[[nodiscard]] bool CollectGradientMatrices(
		const LOTLayerNode *main,
		const LOTLayerNode *horizontal,
		const LOTLayerNode *vertical,
		GradientMatrices &result,
		int depth = 0) {
	if (!main || !horizontal || !vertical) {
		return !main;
	} else if (depth > kMaxSvgDepth
		|| main->mNodeList.size != horizontal->mNodeList.size
		|| main->mNodeList.size != vertical->mNodeList.size
		|| main->mLayerList.size != horizontal->mLayerList.size
		|| main->mLayerList.size != vertical->mLayerList.size) {
		return false;
	}
	const auto nodes = main->mNodeList.size;
	for (auto i = size_t(0); i != nodes; ++i) {
		const auto node = main->mNodeList.ptr[i];
		if (!node || node->mBrushType != BrushGradient) {
			continue;
		}
		const auto h = horizontal->mNodeList.ptr[i];
		const auto v = vertical->mNodeList.ptr[i];
		if (!h
			|| !v
			|| h->mBrushType != BrushGradient
			|| v->mBrushType != BrushGradient
			|| h->mGradient.type != GradientLinear
			|| v->mGradient.type != GradientLinear) {
			return false;
		}
		auto matrix = SvgMatrix();
		matrix.e = h->mGradient.start.x;
		matrix.f = h->mGradient.start.y;
		matrix.a = (h->mGradient.end.x - matrix.e) / kProbeLength;
		matrix.b = (h->mGradient.end.y - matrix.f) / kProbeLength;
		matrix.c = (v->mGradient.start.x - matrix.e) / kProbeLength;
		matrix.d = (v->mGradient.start.y - matrix.f) / kProbeLength;
		result.emplace(node, matrix);
	}
	const auto layers = main->mLayerList.size;
	for (auto i = size_t(0); i != layers; ++i) {
		if (!CollectGradientMatrices(
				main->mLayerList.ptr[i],
				horizontal->mLayerList.ptr[i],
				vertical->mLayerList.ptr[i],
				result,
				depth + 1)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool HasGradients(const LOTLayerNode *layer, int depth = 0) {
	if (!layer || depth > kMaxSvgDepth) {
		return false;
	}
	for (auto i = size_t(0); i != layer->mNodeList.size; ++i) {
		const auto node = layer->mNodeList.ptr[i];
		if (node && node->mBrushType == BrushGradient) {
			return true;
		}
	}
	for (auto i = size_t(0); i != layer->mLayerList.size; ++i) {
		if (HasGradients(layer->mLayerList.ptr[i], depth + 1)) {
			return true;
		}
	}
	return false;
}

void AppendNumber(std::string &out, double value, int decimals = 3) {
	static constexpr std::int64_t kScales[] = {
		1,
		10,
		100,
		1000,
		10000,
		100000,
		1000000,
	};
	decimals = std::clamp(decimals, 0, 6);
	if (!std::isfinite(value)) {
		out += '0';
		return;
	}
	value = std::clamp(value, -1e12, 1e12);
	const auto scale = kScales[decimals];
	auto scaled = std::int64_t(std::llround(value * double(scale)));
	if (scaled < 0) {
		out += '-';
		scaled = -scaled;
	}
	out += std::to_string(scaled / scale);
	auto fraction = scaled % scale;
	if (!fraction) {
		return;
	}
	auto digits = decimals;
	while (!(fraction % 10)) {
		fraction /= 10;
		--digits;
	}
	const auto text = std::to_string(fraction);
	out += '.';
	out.append(size_t(std::max(digits - int(text.size()), 0)), '0');
	out += text;
}

enum class SvgPaint : unsigned char {
	Normal,
	White, // Alpha matte source.
	Black, // Inverted alpha matte source, drawn over white.
	Luma, // Luma matte source.
	InvertedLuma, // Inverted luma matte source, drawn over white.
};

struct SvgColor {
	int r = 0;
	int g = 0;
	int b = 0;
	int a = 0;
};

[[nodiscard]] SvgColor MapPaint(SvgColor color, SvgPaint paint) {
	const auto luma = [&] {
		// Same weights and truncation as rlottie's VBitmap::updateLuma().
		return std::clamp(
			int(0.299f * color.r + 0.587f * color.g + 0.114f * color.b),
			0,
			255);
	};
	switch (paint) {
	case SvgPaint::Normal: return color;
	case SvgPaint::White: return { 255, 255, 255, color.a };
	case SvgPaint::Black: return { 0, 0, 0, color.a };
	case SvgPaint::Luma: {
		const auto value = luma();
		return { value, value, value, color.a };
	}
	case SvgPaint::InvertedLuma: {
		const auto value = 255 - luma();
		return { value, value, value, color.a };
	}
	}
	return color;
}

void AppendHexColor(std::string &out, const SvgColor &color) {
	static constexpr char kDigits[] = "0123456789abcdef";
	out += '#';
	for (const auto component : { color.r, color.g, color.b }) {
		const auto value = std::clamp(component, 0, 255);
		out += kDigits[value >> 4];
		out += kDigits[value & 0x0F];
	}
}

// Writes rlottie's render tree of one frame (LOTLayerNode, all paths
// already in composition coordinates) as SVG 1.1, reproducing the way
// rlottie composites it (LOTCompLayerItem::renderHelper and friends).
class SvgWriter final {
public:
	SvgWriter(
		int width,
		int height,
		const GradientMatrices *gradientMatrices = nullptr)
	: _width(width)
	, _height(height)
	, _gradientMatrices(gradientMatrices) {
	}

	[[nodiscard]] bool write(const LOTLayerNode *root) {
		writeLayer(root, SvgPaint::Normal, _body, 0);
		return !_failed;
	}
	[[nodiscard]] std::string document() const {
		const auto width = std::to_string(_width);
		const auto height = std::to_string(_height);
		auto result = std::string();
		result.reserve(_defs.size() + _body.size() + 512);
		result += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		result += "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\"";
		result += " width=\"" + width + "\" height=\"" + height + "\"";
		result += " viewBox=\"0 0 " + width + ' ' + height + "\">\n";
		if (!_defs.empty()) {
			result += "<defs>\n";
			result += _defs;
			result += "</defs>\n";
		}
		result += _body;
		result += "</svg>\n";
		return result;
	}
	[[nodiscard]] const std::string &error() const {
		return _error;
	}

private:
	void fail(std::string reason) {
		if (!_failed) {
			_failed = true;
			_error = std::move(reason);
		}
	}

	[[nodiscard]] std::string nextId(char prefix) {
		return prefix + std::to_string(++_ids);
	}

	[[nodiscard]] static std::string Wrap(
			std::string_view attributes,
			const std::string &content) {
		auto result = std::string("<g ");
		result += attributes;
		result += ">\n";
		result += content;
		result += "</g>\n";
		return result;
	}

	[[nodiscard]] static std::string MaskAttribute(const std::string &id) {
		return "mask=\"url(#" + id + ")\"";
	}

	[[nodiscard]] std::string canvas(
			std::string_view fill,
			const std::string &maskId = std::string()) const {
		auto result = std::string("<rect x=\"0\" y=\"0\" width=\"");
		result += std::to_string(_width);
		result += "\" height=\"";
		result += std::to_string(_height);
		result += "\" fill=\"";
		result += fill;
		result += '"';
		if (!maskId.empty()) {
			result += ' ';
			result += MaskAttribute(maskId);
		}
		result += "/>\n";
		return result;
	}

	[[nodiscard]] std::string addMask(const std::string &content) {
		const auto id = nextId('m');
		_defs += "<mask id=\"" + id + "\" maskUnits=\"userSpaceOnUse\"";
		_defs += " x=\"0\" y=\"0\" width=\"" + std::to_string(_width);
		_defs += "\" height=\"" + std::to_string(_height) + "\">\n";
		_defs += content;
		_defs += "</mask>\n";
		return id;
	}

	// floatCount is the number of floats (two per point) in points.
	[[nodiscard]] bool pathData(
			const char *elements,
			size_t elementCount,
			const float *points,
			size_t floatCount,
			std::string &out) {
		if (!elementCount) {
			return true;
		} else if (!elements) {
			fail("broken path in the render tree");
			return false;
		}
		auto index = size_t(0);
		const auto take = [&](size_t pointCount) {
			if (!points || index + pointCount * 2 > floatCount) {
				fail("broken path in the render tree");
				return false;
			}
			for (auto i = size_t(0); i != pointCount * 2; ++i) {
				if (i) {
					out += ' ';
				}
				AppendNumber(out, points[index++]);
			}
			return true;
		};
		for (auto i = size_t(0); i != elementCount; ++i) {
			// VPath::Element: MoveTo, LineTo, CubicTo, Close.
			switch (static_cast<unsigned char>(elements[i])) {
			case 0:
				out += 'M';
				if (!take(1)) {
					return false;
				}
				break;
			case 1:
				out += 'L';
				if (!take(1)) {
					return false;
				}
				break;
			case 2:
				out += 'C';
				if (!take(3)) {
					return false;
				}
				break;
			case 3:
				out += 'Z';
				break;
			default:
				fail("unknown path element in the render tree");
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] const SvgMatrix *gradientMatrix(const LOTNode *node) const {
		if (!_gradientMatrices) {
			return nullptr;
		}
		const auto i = _gradientMatrices->find(node);
		if (i == end(*_gradientMatrices)) {
			return nullptr;
		}
		const auto &matrix = i->second;
		const auto determinant = matrix.determinant();
		const auto scale = matrix.rlottieScale();
		return (std::isfinite(determinant)
			&& std::abs(determinant) > 1e-9
			&& std::isfinite(scale)
			&& scale > 1e-9)
			? &matrix
			: nullptr;
	}

	[[nodiscard]] std::string addGradient(
			const LOTNode *node,
			SvgPaint paint) {
		const auto &gradient = node->mGradient;
		if (!gradient.stopPtr || !gradient.stopCount) {
			return std::string();
		}
		auto visible = false;
		for (auto i = size_t(0); i != gradient.stopCount; ++i) {
			if (gradient.stopPtr[i].a) {
				visible = true;
				break;
			}
		}
		if (!visible) {
			return std::string();
		}
		const auto id = nextId('g');
		auto &out = _defs;
		const auto radial = (gradient.type == GradientRadial);
		const auto matrix = gradientMatrix(node);
		const auto point = [&](
				std::string_view nameX,
				std::string_view nameY,
				float x,
				float y) {
			// In the gradient's own space when its matrix is known.
			const auto local = matrix
				? matrix->unmap(x, y)
				: std::make_pair(double(x), double(y));
			out += ' ';
			out += nameX;
			out += "=\"";
			AppendNumber(out, local.first);
			out += "\" ";
			out += nameY;
			out += "=\"";
			AppendNumber(out, local.second);
			out += '"';
		};
		if (radial) {
			out += "<radialGradient id=\"" + id + '"';
			out += " gradientUnits=\"userSpaceOnUse\"";
			point("cx", "cy", gradient.center.x, gradient.center.y);
			auto radius = double(std::max(gradient.cradius, 0.f));
			if (matrix) {
				radius /= matrix->rlottieScale();
			}
			out += " r=\"";
			AppendNumber(out, radius);
			out += '"';
			point("fx", "fy", gradient.focal.x, gradient.focal.y);
		} else {
			out += "<linearGradient id=\"" + id + '"';
			out += " gradientUnits=\"userSpaceOnUse\"";
			point("x1", "y1", gradient.start.x, gradient.start.y);
			point("x2", "y2", gradient.end.x, gradient.end.y);
		}
		if (matrix) {
			out += " gradientTransform=\"matrix(";
			AppendNumber(out, matrix->a, 6);
			out += ' ';
			AppendNumber(out, matrix->b, 6);
			out += ' ';
			AppendNumber(out, matrix->c, 6);
			out += ' ';
			AppendNumber(out, matrix->d, 6);
			out += ' ';
			AppendNumber(out, matrix->e, 4);
			out += ' ';
			AppendNumber(out, matrix->f, 4);
			out += ")\"";
		}
		out += ">\n";
		auto last = 0.f;
		for (auto i = size_t(0); i != gradient.stopCount; ++i) {
			const auto &stop = gradient.stopPtr[i];
			const auto position = std::isfinite(stop.pos) ? stop.pos : 0.f;
			const auto offset = std::max(last, std::clamp(position, 0.f, 1.f));
			last = offset;
			const auto color = MapPaint(
				{ stop.r, stop.g, stop.b, stop.a },
				paint);
			out += "<stop offset=\"";
			AppendNumber(out, offset, 4);
			out += "\" stop-color=\"";
			AppendHexColor(out, color);
			out += '"';
			if (color.a < 255) {
				out += " stop-opacity=\"";
				AppendNumber(out, color.a / 255., 4);
				out += '"';
			}
			out += "/>\n";
		}
		out += radial ? "</radialGradient>\n" : "</linearGradient>\n";
		return id;
	}

	void writeNode(const LOTNode *node, SvgPaint paint, std::string &out) {
		if (!node || node->mImageInfo.data) {
			// Image layers are not drawn: rlottie is built without
			// the image loader, so its raster output skips them too.
			return;
		}
		const auto stroke = (node->mStroke.enable != 0);
		if (stroke && !(node->mStroke.width > 0.f)) {
			return;
		}
		auto d = std::string();
		if (!pathData(
				node->mPath.elmPtr,
				node->mPath.elmCount,
				node->mPath.ptPtr,
				node->mPath.ptCount,
				d)
			|| d.empty()) {
			return;
		}
		auto brush = std::string();
		auto opacity = 255;
		if (node->mBrushType == BrushSolid) {
			const auto color = MapPaint({
				node->mColor.r,
				node->mColor.g,
				node->mColor.b,
				node->mColor.a,
			}, paint);
			if (!color.a) {
				return;
			}
			AppendHexColor(brush, color);
			opacity = color.a;
		} else if (node->mBrushType == BrushGradient) {
			const auto id = addGradient(node, paint);
			if (id.empty()) {
				return;
			}
			brush = "url(#" + id + ")";
		} else {
			return;
		}
		out += "<path d=\"";
		out += d;
		out += '"';
		if (stroke) {
			out += " fill=\"none\" stroke=\"" + brush + '"';
			if (opacity < 255) {
				out += " stroke-opacity=\"";
				AppendNumber(out, opacity / 255., 4);
				out += '"';
			}
			out += " stroke-width=\"";
			AppendNumber(out, node->mStroke.width);
			out += '"';
			switch (node->mStroke.cap) {
			case CapFlat: break;
			case CapSquare: out += " stroke-linecap=\"square\""; break;
			case CapRound: out += " stroke-linecap=\"round\""; break;
			}
			switch (node->mStroke.join) {
			case JoinMiter:
				// rlottie clamps the limit to 1 and bevels above it,
				// like SVG does.
				out += " stroke-miterlimit=\"";
				AppendNumber(out, std::max(node->mStroke.miterLimit, 1.f));
				out += '"';
				break;
			case JoinBevel: out += " stroke-linejoin=\"bevel\""; break;
			case JoinRound: out += " stroke-linejoin=\"round\""; break;
			}
		} else {
			out += " fill=\"" + brush + '"';
			if (opacity < 255) {
				out += " fill-opacity=\"";
				AppendNumber(out, opacity / 255., 4);
				out += '"';
			}
			if (node->mFillRule == FillEvenOdd) {
				out += " fill-rule=\"evenodd\"";
			}
		}
		out += "/>\n";
	}

	// Combines the layer masks like LOTLayerMaskItem::maskRle(), returns
	// the id of the resulting <mask> or an empty string.
	//
	// Mask opacity and "inv" are ignored on purpose: LOTMaskItem::rle()
	// applies them to the copy returned by VRasterizer::rle(), so they
	// never reach rlottie's output (and Telegram shows masks that way).
	[[nodiscard]] std::string layerMasks(const LOTLayerNode *layer) {
		auto current = std::string();
		for (auto i = size_t(0); i != layer->mMaskList.size; ++i) {
			const auto &mask = layer->mMaskList.ptr[i];
			auto d = std::string();
			// Unlike LOTNode, LOTMask::ptCount counts points, not floats.
			if (!pathData(
					mask.mPath.elmPtr,
					mask.mPath.elmCount,
					mask.mPath.ptPtr,
					mask.mPath.ptCount * 2,
					d)) {
				return std::string();
			}
			const auto shape = [&](
					std::string_view fill,
					const std::string &maskId = std::string()) {
				if (d.empty()) {
					return std::string();
				}
				auto result = "<path d=\"" + d + "\" fill=\"";
				result += fill;
				result += '"';
				if (!maskId.empty()) {
					result += ' ';
					result += MaskAttribute(maskId);
				}
				result += "/>\n";
				return result;
			};
			auto content = std::string();
			switch (mask.mMode) {
			case MaskAdd:
				if (!current.empty()) {
					content += canvas("#fff", current);
				}
				content += shape("#fff");
				break;
			case MaskSubstract:
				content += canvas("#fff", current);
				content += shape("#000");
				break;
			case MaskIntersect:
				content += shape("#fff", current);
				break;
			case MaskDifference:
				if (current.empty()) {
					content += shape("#fff");
				} else {
					// XOR: (current - path) + (path - current).
					const auto outside = addMask(
						canvas("#fff", current) + shape("#000"));
					const auto notCurrent = addMask(
						canvas("#fff") + canvas("#000", current));
					content += canvas("#fff", outside);
					content += shape("#fff", notCurrent);
				}
				break;
			}
			current = addMask(content);
		}
		return current;
	}

	// Like LOTCompLayerItem::renderMatteLayer(): target is the layer with
	// "tt", source is the one right after it in the back-to-front list.
	void writeMatted(
			const LOTLayerNode *target,
			const LOTLayerNode *source,
			SvgPaint paint,
			std::string &out,
			int depth) {
		auto inverted = false;
		auto sourcePaint = SvgPaint::White;
		switch (target->mMatte) {
		case MatteAlpha: break;
		case MatteAlphaInv:
			sourcePaint = SvgPaint::Black;
			inverted = true;
			break;
		case MatteLuma: sourcePaint = SvgPaint::Luma; break;
		case MatteLumaInv:
			sourcePaint = SvgPaint::InvertedLuma;
			inverted = true;
			break;
		default: return;
		}
		auto content = std::string();
		writeLayer(target, paint, content, depth);
		if (_failed || content.empty()) {
			return;
		}
		auto matte = inverted ? canvas("#fff") : std::string();
		writeLayer(source, sourcePaint, matte, depth);
		if (_failed || (!inverted && matte.empty())) {
			return;
		}
		out += Wrap(MaskAttribute(addMask(matte)), content);
	}

	void writeLayer(
			const LOTLayerNode *layer,
			SvgPaint paint,
			std::string &out,
			int depth) {
		if (!layer || !layer->mVisible || !layer->mAlpha || _failed) {
			return;
		} else if (depth > kMaxSvgDepth) {
			fail("too deeply nested layers");
			return;
		}
		auto content = std::string();
		if (layer->mNodeList.ptr) {
			for (auto i = size_t(0); i != layer->mNodeList.size; ++i) {
				writeNode(layer->mNodeList.ptr[i], paint, content);
			}
		}
		const LOTLayerNode *matte = nullptr;
		if (layer->mLayerList.ptr) {
			for (auto i = size_t(0); i != layer->mLayerList.size; ++i) {
				const auto child = layer->mLayerList.ptr[i];
				if (!child) {
					continue;
				} else if (child->mMatte != MatteNone) {
					matte = child;
					continue;
				} else if (child->mVisible) {
					if (!matte) {
						writeLayer(child, paint, content, depth + 1);
					} else if (matte->mVisible) {
						writeMatted(matte, child, paint, content, depth + 1);
					}
				}
				matte = nullptr;
			}
		}
		if (_failed || content.empty()) {
			return;
		}
		if (layer->mClipPath.elmCount) {
			auto d = std::string();
			if (!pathData(
					layer->mClipPath.elmPtr,
					layer->mClipPath.elmCount,
					layer->mClipPath.ptPtr,
					layer->mClipPath.ptCount,
					d)) {
				return;
			}
			const auto id = addMask(
				"<path d=\"" + d + "\" fill=\"#fff\"/>\n");
			content = Wrap(MaskAttribute(id), content);
		}
		if (layer->mMaskList.ptr && layer->mMaskList.size) {
			const auto id = layerMasks(layer);
			if (_failed) {
				return;
			} else if (!id.empty()) {
				content = Wrap(MaskAttribute(id), content);
			}
		}
		if (layer->mAlpha < 255) {
			auto opacity = std::string("opacity=\"");
			AppendNumber(opacity, layer->mAlpha / 255., 4);
			opacity += '"';
			content = Wrap(opacity, content);
		}
		out += content;
	}

	int _width = 0;
	int _height = 0;
	std::string _defs;
	std::string _body;
	std::string _error;
	const GradientMatrices *_gradientMatrices = nullptr;
	int _ids = 0;
	bool _failed = false;

};

// Portable core end.

[[nodiscard]] std::string_view View(const QByteArray &data) {
	return std::string_view(data.constData(), size_t(data.size()));
}

[[nodiscard]] bool IsGzip(const QByteArray &data) {
	return (data.size() > 2)
		&& (uchar(data[0]) == 0x1F)
		&& (uchar(data[1]) == 0x8B);
}

[[nodiscard]] bool LooksLikeJson(const QByteArray &data) {
	auto from = data.constData();
	const auto till = from + data.size();
	if ((till - from) >= 3
		&& uchar(from[0]) == 0xEF
		&& uchar(from[1]) == 0xBB
		&& uchar(from[2]) == 0xBF) {
		from += 3;
	}
	while (from != till
		&& (*from == ' ' || *from == '\n' || *from == '\r' || *from == '\t')) {
		++from;
	}
	return (from != till) && (*from == '{');
}

[[nodiscard]] QByteArray Inflate(const QByteArray &data) {
	auto stream = z_stream();
	stream.zalloc = nullptr;
	stream.zfree = nullptr;
	stream.opaque = nullptr;
	stream.avail_in = 0;
	stream.next_in = nullptr;
	if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
		return QByteArray();
	}
	const auto guard = gsl::finally([&] { inflateEnd(&stream); });

	stream.avail_in = uInt(data.size());
	stream.next_in = reinterpret_cast<Bytef*>(
		const_cast<char*>(data.constData()));
	auto result = QByteArray();
	auto buffer = QByteArray(256 * 1024, Qt::Uninitialized);
	while (true) {
		stream.avail_out = uInt(buffer.size());
		stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
		const auto code = inflate(&stream, Z_NO_FLUSH);
		if (code != Z_OK && code != Z_STREAM_END) {
			return QByteArray();
		}
		result.append(
			buffer.constData(),
			buffer.size() - qsizetype(stream.avail_out));
		if (result.size() > kMaxUnpackedSize) {
			return QByteArray();
		} else if (code == Z_STREAM_END) {
			return result;
		} else if (!stream.avail_in && stream.avail_out) {
			// Truncated input.
			return QByteArray();
		}
	}
}

[[nodiscard]] std::optional<Json> ParseJson(const QByteArray &json) {
	if (json.isEmpty() || json.size() > kMaxUnpackedSize) {
		return std::nullopt;
	}
	auto result = Json();
	auto reader = JsonReader(View(json));
	if (!reader.read(result) || !result.is(Json::Type::Object)) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] std::unique_ptr<rlottie::Animation> LoadAnimation(
		const QByteArray &json) {
	if (json.isEmpty()) {
		return nullptr;
	}
	return ::Lottie::LoadAnimationFromData(
		::Lottie::ReadUtf8(json),
		std::string(),
		std::string(),
		false);
}

// Invalid Info unless every value is in the sane range.
[[nodiscard]] Info MakeInfo(
		double frames,
		double fps,
		double width,
		double height) {
	const auto within = [](double value, double from, double till) {
		return (value >= from) && (value <= till); // False for NaN.
	};
	if (!within(frames, 1., kMaxFrames - 1.)
		|| !within(fps, kMinFrameRate, kMaxFrameRate)
		|| !within(width, 1., kMaxSide)
		|| !within(height, 1., kMaxSide)) {
		return Info();
	}
	auto result = Info();
	result.frames = int(frames);
	result.fps = fps;
	result.size = QSize(int(width), int(height));
	result.duration = crl::time(std::llround(frames * 1000. / fps));
	return result;
}

[[nodiscard]] Info AnimationInfo(const rlottie::Animation &animation) {
	auto width = size_t();
	auto height = size_t();
	animation.size(width, height);
	return MakeInfo(
		double(animation.totalFrame()),
		animation.frameRate(),
		double(width),
		double(height));
}

[[nodiscard]] QImage RenderWith(
		rlottie::Animation &animation,
		const Info &info,
		int frame,
		QSize size) {
	if (!info.valid()
		|| size.isEmpty()
		|| size.width() > kMaxRenderSide
		|| size.height() > kMaxRenderSide) {
		return QImage();
	}
	auto image = QImage(size, QImage::Format_ARGB32_Premultiplied);
	if (image.isNull()) {
		return QImage();
	}
	image.fill(Qt::transparent);
	auto surface = rlottie::Surface(
		reinterpret_cast<uint32_t*>(image.bits()),
		size_t(size.width()),
		size_t(size.height()),
		size_t(image.bytesPerLine()));
	animation.renderSync(
		size_t(std::clamp(frame, 0, info.frames - 1)),
		std::move(surface),
		true);
	return image;
}

struct CachedAnimation {
	QByteArray data; // As passed by the caller, .tgs or JSON.
	std::unique_ptr<rlottie::Animation> animation;
	Info info;
	std::mutex mutex;
};

struct AnimationCache {
	std::mutex mutex;
	std::vector<std::shared_ptr<CachedAnimation>> entries;
};

[[nodiscard]] AnimationCache &Cache() {
	// Never destroyed: rlottie's own statics may be gone at exit.
	static const auto result = new AnimationCache();
	return *result;
}

[[nodiscard]] std::shared_ptr<CachedAnimation> LookupAnimation(
		const QByteArray &data) {
	if (data.isEmpty()) {
		return nullptr;
	}
	auto &cache = Cache();
	{
		const auto lock = std::lock_guard(cache.mutex);
		auto &entries = cache.entries;
		for (auto i = entries.begin(); i != entries.end(); ++i) {
			if ((*i)->data == data) {
				auto result = *i;
				entries.erase(i);
				entries.insert(entries.begin(), result);
				return result;
			}
		}
	}
	auto result = std::make_shared<CachedAnimation>();
	result->data = data;
	result->animation = LoadAnimation(Unpack(data));
	if (!result->animation) {
		return nullptr;
	}
	result->info = AnimationInfo(*result->animation);
	if (!result->info.valid()) {
		return nullptr;
	}
	const auto lock = std::lock_guard(cache.mutex);
	cache.entries.insert(cache.entries.begin(), result);
	if (cache.entries.size() > kCacheSize) {
		cache.entries.pop_back();
	}
	return result;
}

[[nodiscard]] QByteArray AdjustColorsWithStats(
		const QByteArray &json,
		int hueDegrees,
		int saturationPercent,
		int lightnessPercent,
		ColorStats *stats) {
	auto document = ParseJson(Unpack(json));
	if (!document) {
		return QByteArray();
	}
	auto adjust = ColorAdjust();
	adjust.hue = std::fmod(double(hueDegrees), 360.);
	if (adjust.hue < 0.) {
		adjust.hue += 360.;
	}
	adjust.saturation = std::clamp(saturationPercent, -100, 100) / 100.;
	adjust.lightness = std::clamp(lightnessPercent, -100, 100) / 100.;
	auto adjuster = ColorAdjuster(adjust);
	adjuster.process(*document);
	if (stats) {
		*stats = adjuster.stats();
	}
	return QByteArray::fromStdString(SerializeJson(*document));
}

[[nodiscard]] QByteArray RasterSvg(const QByteArray &json, int frame) {
	const auto animation = LoadAnimation(json);
	const auto info = animation ? AnimationInfo(*animation) : Info();
	if (!info.valid()) {
		return QByteArray();
	}
	const auto width = info.size.width();
	const auto height = info.size.height();
	const auto side = std::max(width, height);
	auto size = (side < kRasterMinSide)
		? (info.size * (double(kRasterMinSide) / side))
		: info.size;
	if (size.width() > kMaxRenderSide || size.height() > kMaxRenderSide) {
		size = size.scaled(
			QSize(kMaxRenderSide, kMaxRenderSide),
			Qt::KeepAspectRatio);
	}
	const auto image = RenderWith(
		*animation,
		info,
		frame,
		size.expandedTo(QSize(1, 1)));
	if (image.isNull()) {
		return QByteArray();
	}
	auto png = QByteArray();
	{
		auto buffer = QBuffer(&png);
		if (!buffer.open(QIODevice::WriteOnly)
			|| !image.save(&buffer, "PNG")) {
			return QByteArray();
		}
	}
	const auto w = QByteArray::number(width);
	const auto h = QByteArray::number(height);
	auto result = QByteArray();
	result.reserve(png.size() * 4 / 3 + 512);
	result += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	result += "<svg xmlns=\"http://www.w3.org/2000/svg\"";
	result += " xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\"";
	result += " width=\"" + w + "\" height=\"" + h + "\"";
	result += " viewBox=\"0 0 " + w + ' ' + h + "\">\n";
	result += "<image x=\"0\" y=\"0\" width=\"" + w + "\" height=\"" + h;
	result += "\" preserveAspectRatio=\"none\"";
	result += " xlink:href=\"data:image/png;base64,";
	result += png.toBase64();
	result += "\"/>\n</svg>\n";
	return result;
}

[[nodiscard]] bool OnMainThreadOfLaunchedApp() {
	const auto app = QCoreApplication::instance();
	return app
		&& (QThread::currentThread() == app->thread())
		&& Core::IsAppLaunched();
}

} // namespace

QByteArray Unpack(const QByteArray &data) {
	if (data.isEmpty()) {
		return QByteArray();
	} else if (IsGzip(data)) {
		auto result = Inflate(data);
		return LooksLikeJson(result) ? result : QByteArray();
	}
	return LooksLikeJson(data) ? data : QByteArray();
}

QByteArray PackTgs(const QByteArray &json) {
	const auto plain = IsGzip(json) ? Unpack(json) : json;
	if (!LooksLikeJson(plain) || plain.size() > kMaxUnpackedSize) {
		return QByteArray();
	}
	auto stream = z_stream();
	stream.zalloc = nullptr;
	stream.zfree = nullptr;
	stream.opaque = nullptr;
	if (deflateInit2(
			&stream,
			Z_BEST_COMPRESSION,
			Z_DEFLATED,
			16 + MAX_WBITS,
			8,
			Z_DEFAULT_STRATEGY) != Z_OK) {
		return QByteArray();
	}
	const auto guard = gsl::finally([&] { deflateEnd(&stream); });

	auto result = QByteArray(
		qsizetype(deflateBound(&stream, uLong(plain.size())) + 64),
		Qt::Uninitialized);
	stream.avail_in = uInt(plain.size());
	stream.next_in = reinterpret_cast<Bytef*>(
		const_cast<char*>(plain.constData()));
	stream.avail_out = uInt(result.size());
	stream.next_out = reinterpret_cast<Bytef*>(result.data());
	if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
		return QByteArray();
	}
	result.resize(qsizetype(stream.total_out));
	return result;
}

Info ReadInfo(const QByteArray &json) {
	const auto document = ParseJson(Unpack(json));
	if (!document) {
		return Info();
	}
	const auto width = document->numberAt("w");
	const auto height = document->numberAt("h");
	const auto from = document->numberAt("ip");
	const auto till = document->numberAt("op");
	const auto fps = document->numberAt("fr");
	const auto good = [](const std::optional<double> &value) {
		return value && std::abs(*value) < 1e7;
	};
	if (!good(width)
		|| !good(height)
		|| !good(from)
		|| !good(till)
		|| !good(fps)) {
		return Info();
	}
	// rlottie keeps "ip" / "op" as long, "w" / "h" as int.
	return MakeInfo(
		double(std::int64_t(*till) - std::int64_t(*from)),
		*fps,
		double(std::int64_t(*width)),
		double(std::int64_t(*height)));
}

QByteArray AdjustColors(
		const QByteArray &json,
		int hueDegrees,
		int saturationPercent,
		int lightnessPercent) {
	return AdjustColorsWithStats(
		json,
		hueDegrees,
		saturationPercent,
		lightnessPercent,
		nullptr);
}

QImage RenderFrame(const QByteArray &json, int frame, QSize size) {
	const auto entry = LookupAnimation(json);
	if (!entry) {
		return QImage();
	}
	const auto lock = std::lock_guard(entry->mutex);
	return RenderWith(*entry->animation, entry->info, frame, size);
}

struct Renderer::Private {
	std::unique_ptr<rlottie::Animation> animation;
	Info info;
};

Renderer::Renderer(const QByteArray &json)
: _private(std::make_unique<Private>()) {
	_private->animation = LoadAnimation(Unpack(json));
	if (_private->animation) {
		_private->info = AnimationInfo(*_private->animation);
		if (!_private->info.valid()) {
			_private->animation = nullptr;
		}
	}
}

Renderer::~Renderer() = default;

bool Renderer::valid() const {
	return (_private->animation != nullptr);
}

const Info &Renderer::info() const {
	return _private->info;
}

QImage Renderer::render(int frame, QSize size) {
	return _private->animation
		? RenderWith(*_private->animation, _private->info, frame, size)
		: QImage();
}

SvgResult ExportSvg(const QByteArray &json, int frame) {
	const auto plain = Unpack(json);
	auto document = ParseJson(plain);
	if (!document) {
		return SvgResult();
	}
	const auto prepared = PrepareMasksForSvg(*document)
		? QByteArray::fromStdString(SerializeJson(*document))
		: plain;

	// Fresh animations: the render tree is built for one update only,
	// and building it mutates drawables (dashes are applied in place).
	const auto animation = LoadAnimation(prepared);
	const auto info = animation ? AnimationInfo(*animation) : Info();
	if (!info.valid()) {
		return SvgResult();
	}
	frame = std::clamp(frame, 0, info.frames - 1);
	const auto width = info.size.width();
	const auto height = info.size.height();
	const auto buildTree = [&](const rlottie::Animation *source) {
		return source
			? source->renderTree(
				size_t(frame),
				size_t(width),
				size_t(height))
			: nullptr;
	};
	auto result = SvgResult();
	const auto tree = buildTree(animation.get());
	auto matrices = GradientMatrices();
	if (HasGradients(tree)) {
		const auto probe = [&](bool vertical) {
			return LoadAnimation(QByteArray::fromStdString(SerializeJson(
				MakeGradientProbe(*document, vertical))));
		};
		const auto horizontal = probe(false);
		const auto vertical = probe(true);
		if (!CollectGradientMatrices(
				tree,
				buildTree(horizontal.get()),
				buildTree(vertical.get()),
				matrices)) {
			matrices.clear();
			result.reason = u"gradient matrices are unknown, "
				"gradients under non-uniform scale are approximate"_q;
		}
	}
	document = std::nullopt;

	auto writer = SvgWriter(width, height, &matrices);
	if (tree && writer.write(tree)) {
		result.svg = QByteArray::fromStdString(writer.document());
		result.vector = true;
		return result;
	}
	result.reason = tree
		? QString::fromStdString(writer.error())
		: u"rlottie returned no render tree"_q;
	result.svg = RasterSvg(plain, frame);
	if (result.svg.isEmpty()) {
		return SvgResult();
	}
	if (OnMainThreadOfLaunchedApp()) {
		result.note = SvgRasterNote();
	}
	return result;
}

QString SvgRasterNote() {
	return tr::lng_oblivion_lottie_svg_raster(tr::now);
}

// Self-test.

namespace {

constexpr auto kSvgTestSide = 512;
constexpr auto kColorTestSide = 256;
constexpr auto kIdentityMeanLimit = 0.5;
constexpr auto kRoundTripMeanLimit = 1.;
constexpr auto kPixelDelta = 32;
constexpr auto kTolerance = 8;

// An SVG frame fails when more than kSvgOverLimit percent of the pixels
// differ by more than kPixelDelta (a shape or a paint is wrong), or when
// both the plain mean difference and the mean beyond kTolerance levels
// are high. The tolerant mean is needed because rlottie itself is not
// exact: every matte and every group opacity goes through a canvas-sized
// offscreen buffer blended with 8-bit ">> 8" math, which takes about
// 1/256 of the alpha of everything drawn below, so animations with many
// mattes come out a few levels more transparent than the exact result
// (location.tgs: alpha 248 instead of 255 under seven mattes).
constexpr auto kSvgMeanLimit = 2.5;
constexpr auto kSvgTolerantLimit = 1.;
constexpr auto kSvgOverLimit = 2.;

struct ImageDiff {
	double mean = 0.; // Mean absolute difference of premultiplied ARGB.
	double tolerant = 0.; // Mean of the differences beyond kTolerance.
	double over = 0.; // Percent of pixels with a channel off by > 32.
	int max = 0;
	bool valid = false;
};

[[nodiscard]] ImageDiff CompareImages(QImage a, QImage b) {
	if (a.isNull() || b.isNull() || a.size() != b.size()) {
		return ImageDiff();
	}
	a = std::move(a).convertToFormat(QImage::Format_ARGB32_Premultiplied);
	b = std::move(b).convertToFormat(QImage::Format_ARGB32_Premultiplied);
	auto sum = uint64();
	auto tolerant = uint64();
	auto over = int64();
	auto max = 0;
	const auto width = a.width();
	const auto height = a.height();
	for (auto y = 0; y != height; ++y) {
		const auto left = reinterpret_cast<const uint32*>(a.constScanLine(y));
		const auto right = reinterpret_cast<const uint32*>(
			b.constScanLine(y));
		for (auto x = 0; x != width; ++x) {
			auto pixelMax = 0;
			for (auto shift = 0; shift != 32; shift += 8) {
				const auto delta = std::abs(
					int((left[x] >> shift) & 0xFF)
					- int((right[x] >> shift) & 0xFF));
				sum += delta;
				tolerant += std::max(delta - kTolerance, 0);
				pixelMax = std::max(pixelMax, delta);
			}
			if (pixelMax > kPixelDelta) {
				++over;
			}
			max = std::max(max, pixelMax);
		}
	}
	const auto pixels = double(width) * height;
	auto result = ImageDiff();
	result.mean = sum / (pixels * 4.);
	result.tolerant = tolerant / (pixels * 4.);
	result.over = over * 100. / pixels;
	result.max = max;
	result.valid = true;
	return result;
}

[[nodiscard]] QImage RenderSvgImage(const QByteArray &svg, QSize size) {
	auto renderer = QSvgRenderer();
	// Deep precomps nest more than the 32 levels Qt SVG draws by default.
	renderer.setOptions(QtSvg::AssumeTrustedSource);
	if (!renderer.load(svg) || !renderer.isValid()) {
		return QImage();
	}
	auto image = QImage(size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto painter = QPainter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	const auto box = renderer.viewBoxF().size();
	auto target = QRectF(QPointF(), QSizeF(size));
	if (!box.isEmpty()) {
		// Fit and center like rlottie's keepAspectRatio.
		const auto scaled = box.scaled(QSizeF(size), Qt::KeepAspectRatio);
		target = QRectF(
			QPointF(
				(size.width() - scaled.width()) / 2.,
				(size.height() - scaled.height()) / 2.),
			scaled);
	}
	renderer.render(&painter, target);
	painter.end();
	return image;
}

[[nodiscard]] QString FormatDiff(const ImageDiff &diff) {
	return u"%1 (tolerant %2, %3% >%4)"_q.arg(
		QString::number(diff.mean, 'f', 2),
		QString::number(diff.tolerant, 'f', 2),
		QString::number(diff.over, 'f', 2),
		QString::number(kPixelDelta));
}

[[nodiscard]] bool SvgDiffGood(const ImageDiff &diff) {
	return diff.valid
		&& (diff.over <= kSvgOverLimit)
		&& (diff.mean <= kSvgMeanLimit
			|| diff.tolerant <= kSvgTolerantLimit);
}

struct TestTotals {
	int files = 0;
	int passed = 0;
	int failed = 0;
	int skipped = 0;
	int vectorFrames = 0;
	int rasterFrames = 0;
	double vectorDiffSum = 0.;
	double worstVector = 0.;
	QString worstVectorName;
	QStringList rasterList;
	QStringList failedList;
};

// The synthetic animations cover features that the bundled ones may
// not use: masks of every mode, all track matte types, gradients with
// opacity stops, dashes, trim paths, repeaters, polystars, precomps,
// parenting, time remapping and solid layers.
[[nodiscard]] std::vector<std::pair<QString, QByteArray>> Synthetic() {
	auto result = std::vector<std::pair<QString, QByteArray>>();
	const auto add = [&](const char *name, const char *json) {
		result.emplace_back(
			u"synthetic/"_q + QString::fromLatin1(name),
			QByteArray(json));
	};
	add("shapes.json", R"json({"v":"5.7.4","fr":30,"ip":0,"op":30,"w":512,"h":512,"nm":"shapes","ddd":0,"assets":[],"layers":[
{"ddd":0,"ind":1,"ty":4,"nm":"shapes","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":1,"k":[{"i":{"x":[0.4],"y":[1]},"o":{"x":[0.6],"y":[0]},"t":0,"s":[0]},{"t":29,"s":[30]}]},"p":{"a":0,"k":[256,256,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[
{"ty":"gr","nm":"rect","it":[
{"ty":"rc","d":1,"s":{"a":0,"k":[160,120]},"p":{"a":0,"k":[0,0]},"r":{"a":0,"k":24},"nm":"r"},
{"ty":"fl","c":{"a":1,"k":[{"i":{"x":[0.5],"y":[1]},"o":{"x":[0.5],"y":[0]},"t":0,"s":[0.9,0.2,0.3,1]},{"t":29,"s":[0.2,0.4,0.9,1]}]},"o":{"a":0,"k":90},"r":1,"nm":"f"},
{"ty":"st","c":{"a":0,"k":[0.1,0.1,0.1,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":8},"lc":2,"lj":2,"ml":4,"nm":"s"},
{"ty":"tr","p":{"a":0,"k":[-120,-120]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":15},"o":{"a":0,"k":100},"nm":"t"}]},
{"ty":"gr","nm":"ellipse","it":[
{"ty":"el","d":1,"s":{"a":0,"k":[180,120]},"p":{"a":0,"k":[0,0]},"nm":"e"},
{"ty":"gf","o":{"a":0,"k":100},"r":1,"g":{"p":3,"k":{"a":0,"k":[0,1,0.8,0,0.5,1,0,0.5,1,0,0.4,0.9,0,1,1,0.3]}},"s":{"a":0,"k":[-90,0]},"e":{"a":0,"k":[90,0]},"t":1,"nm":"g"},
{"ty":"tr","p":{"a":0,"k":[120,-120]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]},
{"ty":"gr","nm":"star","it":[
{"ty":"sr","sy":1,"d":1,"pt":{"a":0,"k":5},"p":{"a":0,"k":[0,0]},"r":{"a":0,"k":0},"ir":{"a":0,"k":35},"is":{"a":0,"k":0},"or":{"a":0,"k":80},"os":{"a":0,"k":0},"nm":"s"},
{"ty":"gs","o":{"a":0,"k":100},"w":{"a":0,"k":10},"g":{"p":2,"k":{"a":0,"k":[0,1,1,0,1,0.8,0,0.2]}},"s":{"a":0,"k":[0,0]},"e":{"a":0,"k":[80,0]},"t":2,"h":{"a":0,"k":20},"a":{"a":0,"k":30},"lc":1,"lj":1,"ml":4,"nm":"g"},
{"ty":"fl","c":{"a":0,"k":[0.2,0.7,0.4,1]},"o":{"a":0,"k":60},"r":2,"nm":"f"},
{"ty":"tr","p":{"a":0,"k":[-120,120]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]},
{"ty":"gr","nm":"dashed","it":[
{"ty":"sh","d":1,"ks":{"a":0,"k":{"i":[[0,0],[-40,0],[0,0]],"o":[[40,0],[0,0],[0,0]],"v":[[-80,40],[0,-60],[80,40]],"c":false}},"nm":"p"},
{"ty":"tm","s":{"a":0,"k":5},"e":{"a":1,"k":[{"i":{"x":[0.3],"y":[1]},"o":{"x":[0.7],"y":[0]},"t":0,"s":[30]},{"t":29,"s":[95]}]},"o":{"a":0,"k":0},"m":1,"nm":"tm"},
{"ty":"st","c":{"a":0,"k":[0.95,0.6,0.1,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":12},"lc":1,"lj":1,"ml":4,"d":[{"n":"d","nm":"dash","v":{"a":0,"k":24}},{"n":"g","nm":"gap","v":{"a":0,"k":14}},{"n":"o","nm":"offset","v":{"a":0,"k":0}}],"nm":"s"},
{"ty":"tr","p":{"a":0,"k":[120,120]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]},
{"ty":"gr","nm":"repeater","it":[
{"ty":"el","d":1,"s":{"a":0,"k":[24,24]},"p":{"a":0,"k":[0,0]},"nm":"e"},
{"ty":"fl","c":{"a":0,"k":[0.5,0.3,0.9,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},
{"ty":"rp","c":{"a":0,"k":6},"o":{"a":0,"k":0},"m":1,"tr":{"ty":"tr","p":{"a":0,"k":[30,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[90,90]},"r":{"a":0,"k":12},"so":{"a":0,"k":100},"eo":{"a":0,"k":30}},"nm":"rp"},
{"ty":"tr","p":{"a":0,"k":[-80,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":80},"nm":"t"}]}
],"ip":0,"op":30,"st":0,"bm":0}]})json");
	add("masks.json", R"json({"v":"5.7.4","fr":30,"ip":0,"op":30,"w":512,"h":512,"nm":"masks","ddd":0,"assets":[],"layers":[
{"ddd":0,"ind":1,"ty":4,"nm":"add","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[0,0,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"hasMask":true,"masksProperties":[
{"inv":false,"mode":"a","pt":{"a":0,"k":{"i":[[-50,0],[0,-50],[50,0],[0,50]],"o":[[50,0],[0,50],[-50,0],[0,-50]],"v":[[128,38],[218,128],[128,218],[38,128]],"c":true}},"o":{"a":0,"k":100},"x":{"a":0,"k":0},"nm":"m1"}],
"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[240,240]},"p":{"a":0,"k":[128,128]},"r":{"a":0,"k":0},"nm":"r"},{"ty":"fl","c":{"a":0,"k":[0.9,0.3,0.2,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":2,"ty":4,"nm":"subtract","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[256,0,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"hasMask":true,"masksProperties":[
{"inv":false,"mode":"a","pt":{"a":0,"k":{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],"v":[[20,20],[236,20],[236,236],[20,236]],"c":true}},"o":{"a":0,"k":100},"x":{"a":0,"k":0},"nm":"m1"},
{"inv":false,"mode":"s","pt":{"a":0,"k":{"i":[[-40,0],[0,-40],[40,0],[0,40]],"o":[[40,0],[0,40],[-40,0],[0,-40]],"v":[[128,58],[198,128],[128,198],[58,128]],"c":true}},"o":{"a":0,"k":60},"x":{"a":0,"k":0},"nm":"m2"}],
"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[256,256]},"p":{"a":0,"k":[128,128]},"r":{"a":0,"k":0},"nm":"r"},{"ty":"fl","c":{"a":0,"k":[0.2,0.6,0.9,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":3,"ty":4,"nm":"intersect","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[0,256,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"hasMask":true,"masksProperties":[
{"inv":false,"mode":"a","pt":{"a":0,"k":{"i":[[-50,0],[0,-50],[50,0],[0,50]],"o":[[50,0],[0,50],[-50,0],[0,-50]],"v":[[100,30],[190,120],[100,210],[10,120]],"c":true}},"o":{"a":0,"k":100},"x":{"a":0,"k":0},"nm":"m1"},
{"inv":false,"mode":"i","pt":{"a":1,"k":[{"i":{"x":0.5,"y":1},"o":{"x":0.5,"y":0},"t":0,"s":[{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],"v":[[60,60],[240,60],[240,240],[60,240]],"c":true}]},{"t":29,"s":[{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],"v":[[110,90],[250,90],[250,250],[110,250]],"c":true}]}]},"o":{"a":0,"k":100},"x":{"a":0,"k":0},"nm":"m2"}],
"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[256,256]},"p":{"a":0,"k":[128,128]},"r":{"a":0,"k":0},"nm":"r"},{"ty":"fl","c":{"a":0,"k":[0.3,0.8,0.3,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":4,"ty":4,"nm":"inverted","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[256,256,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"hasMask":true,"masksProperties":[
{"inv":true,"mode":"a","pt":{"a":0,"k":{"i":[[-50,0],[0,-50],[50,0],[0,50]],"o":[[50,0],[0,50],[-50,0],[0,-50]],"v":[[128,38],[218,128],[128,218],[38,128]],"c":true}},"o":{"a":1,"k":[{"i":{"x":[0.5],"y":[1]},"o":{"x":[0.5],"y":[0]},"t":0,"s":[30]},{"t":29,"s":[70]}]},"x":{"a":0,"k":0},"nm":"m1"},
{"inv":false,"mode":"n","pt":{"a":0,"k":{"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]],"v":[[0,0],[50,0],[0,50]],"c":true}},"o":{"a":0,"k":100},"x":{"a":0,"k":0},"nm":"m2"}],
"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[256,256]},"p":{"a":0,"k":[128,128]},"r":{"a":0,"k":0},"nm":"r"},{"ty":"fl","c":{"a":0,"k":[0.9,0.8,0.1,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":5,"ty":4,"nm":"no masks","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[0,0,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"hasMask":true,
"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[512,512]},"p":{"a":0,"k":[256,256]},"r":{"a":0,"k":0},"nm":"r"},{"ty":"fl","c":{"a":0,"k":[0,0,0,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0}
]})json");
	add("mattes.json", R"json({"v":"5.7.4","fr":30,"ip":0,"op":30,"w":512,"h":512,"nm":"mattes","ddd":0,"assets":[],"layers":[
{"ddd":0,"ind":1,"ty":4,"nm":"src alpha","td":1,"sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[128,128,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"el","d":1,"s":{"a":1,"k":[{"i":{"x":[0.5,0.5],"y":[1,1]},"o":{"x":[0.5,0.5],"y":[0,0]},"t":0,"s":[120,120]},{"t":29,"s":[220,180]}]},"p":{"a":0,"k":[0,0]},"nm":"e"},{"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":80},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":2,"ty":4,"nm":"alpha","tt":1,"sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[128,128,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[200,120]},"p":{"a":0,"k":[0,0]},"r":{"a":0,"k":10},"nm":"r"},{"ty":"fl","c":{"a":0,"k":[0.9,0.2,0.4,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":20},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":3,"ty":4,"nm":"src alpha inv","td":1,"sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[384,128,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"el","d":1,"s":{"a":0,"k":[140,140]},"p":{"a":0,"k":[0,0]},"nm":"e"},{"ty":"fl","c":{"a":0,"k":[0.2,0.2,0.2,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":4,"ty":4,"nm":"alpha inv","tt":2,"sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[384,128,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[220,200]},"p":{"a":0,"k":[0,0]},"r":{"a":0,"k":0},"nm":"r"},{"ty":"fl","c":{"a":0,"k":[0.1,0.5,0.9,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":5,"ty":4,"nm":"src luma","td":1,"sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[128,384,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[220,220]},"p":{"a":0,"k":[0,0]},"r":{"a":0,"k":0},"nm":"r"},{"ty":"gf","o":{"a":0,"k":100},"r":1,"g":{"p":3,"k":{"a":0,"k":[0,1,1,1,0.5,0.9,0.1,0.1,1,0,0,0]}},"s":{"a":0,"k":[-110,0]},"e":{"a":0,"k":[110,0]},"t":1,"nm":"g"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":6,"ty":4,"nm":"luma","tt":3,"sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[128,384,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"el","d":1,"s":{"a":0,"k":[200,200]},"p":{"a":0,"k":[0,0]},"nm":"e"},{"ty":"fl","c":{"a":0,"k":[0.3,0.9,0.4,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":7,"ty":4,"nm":"src luma inv","td":1,"sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[384,384,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[220,220]},"p":{"a":0,"k":[0,0]},"r":{"a":0,"k":0},"nm":"r"},{"ty":"gf","o":{"a":0,"k":100},"r":1,"g":{"p":2,"k":{"a":0,"k":[0,1,1,1,1,0.1,0.1,0.1]}},"s":{"a":0,"k":[0,-110]},"e":{"a":0,"k":[0,110]},"t":1,"nm":"g"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0},
{"ddd":0,"ind":8,"ty":4,"nm":"luma inv","tt":4,"sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[384,384,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"el","d":1,"s":{"a":0,"k":[210,210]},"p":{"a":0,"k":[0,0]},"nm":"e"},{"ty":"fl","c":{"a":0,"k":[0.8,0.4,0.9,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":30,"st":0,"bm":0}
]})json");
	add("precomp.json", R"json({"v":"5.7.4","fr":60,"ip":0,"op":60,"w":512,"h":512,"nm":"precomp","ddd":0,"assets":[
{"id":"comp_0","layers":[
{"ddd":0,"ind":1,"ty":4,"nm":"circle","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":1,"k":[{"i":{"x":0.4,"y":1},"o":{"x":0.6,"y":0},"t":0,"s":[60,100,0],"to":[20,-10,0],"ti":[-20,10,0]},{"t":59,"s":[180,100,0]}]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","nm":"g","it":[{"ty":"el","d":1,"s":{"a":0,"k":[90,90]},"p":{"a":0,"k":[0,0]},"nm":"e"},{"ty":"fl","c":{"a":0,"k":[0.95,0.5,0.1,1]},"o":{"a":0,"k":100},"r":1,"nm":"f"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},"nm":"t"}]}],"ip":0,"op":60,"st":0,"bm":0},
{"ddd":0,"ind":2,"ty":1,"nm":"solid","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[120,120,0]},"a":{"a":0,"k":[80,80,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"sw":160,"sh":160,"sc":"#3366cc","ip":0,"op":60,"st":0,"bm":0}]}],
"layers":[
{"ddd":0,"ind":1,"ty":3,"nm":"null","sr":1,"ks":{"o":{"a":0,"k":0},"r":{"a":1,"k":[{"i":{"x":[0.5],"y":[1]},"o":{"x":[0.5],"y":[0]},"t":0,"s":[0]},{"t":59,"s":[45]}]},"p":{"a":0,"k":[256,256,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"ip":0,"op":60,"st":0,"bm":0},
{"ddd":0,"ind":2,"ty":0,"nm":"pre a","parent":1,"refId":"comp_0","sr":1,"ks":{"o":{"a":0,"k":60},"r":{"a":0,"k":0},"p":{"a":0,"k":[-120,-120,0]},"a":{"a":0,"k":[120,120,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"w":240,"h":240,"ip":0,"op":60,"st":0,"bm":0},
{"ddd":0,"ind":3,"ty":0,"nm":"pre b","refId":"comp_0","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[380,380,0]},"a":{"a":0,"k":[120,120,0]},"s":{"a":0,"k":[80,80,100]}},"ao":0,"hasMask":true,"masksProperties":[{"inv":false,"mode":"a","pt":{"a":0,"k":{"i":[[-60,0],[0,-60],[60,0],[0,60]],"o":[[60,0],[0,60],[-60,0],[0,-60]],"v":[[120,10],[230,120],[120,230],[10,120]],"c":true}},"o":{"a":0,"k":100},"x":{"a":0,"k":0},"nm":"m"}],"w":240,"h":240,"tm":{"a":1,"k":[{"i":{"x":[0.5],"y":[1]},"o":{"x":[0.5],"y":[0]},"t":0,"s":[0.5]},{"t":59,"s":[0]}]},"ip":0,"op":60,"st":0,"bm":0},
{"ddd":0,"ind":4,"ty":1,"nm":"backdrop","sr":1,"ks":{"o":{"a":0,"k":40},"r":{"a":0,"k":0},"p":{"a":0,"k":[256,256,0]},"a":{"a":0,"k":[100,60,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,"sw":200,"sh":120,"sc":"#22aa55","ip":10,"op":50,"st":0,"bm":0}
]})json");
	return result;
}

[[nodiscard]] std::vector<int> TestFrames(int frames) {
	auto result = std::vector<int>{ 0, frames / 2, frames - 1 };
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

void TestAnimation(
		const QString &name,
		const QByteArray &data,
		QStringList &log,
		TestTotals &totals) {
	const auto progress = (u"lottie self-test: "_q + name + '\n').toUtf8();
	std::fwrite(progress.constData(), 1, progress.size(), stderr);
	std::fflush(stderr);

	auto timer = QElapsedTimer();
	timer.start();
	auto problems = QStringList();
	auto details = QStringList();

	// 1. Gzip round trip, compatible with tdesktop's own loader.
	const auto json = Unpack(data);
	if (json.isEmpty()) {
		++totals.skipped;
		log.push_back(u"skip "_q + name + u": not a Lottie animation"_q);
		return;
	}
	++totals.files;
	if (IsGzip(data) && Images::UnpackGzip(data) != json) {
		problems.push_back(u"Unpack differs from Images::UnpackGzip"_q);
	}
	const auto packed = PackTgs(json);
	if (packed.isEmpty()) {
		problems.push_back(u"PackTgs failed"_q);
	} else if (Unpack(packed) != json) {
		problems.push_back(u"Unpack(PackTgs(json)) != json"_q);
	} else if (Images::UnpackGzip(packed) != json) {
		problems.push_back(u"Images::UnpackGzip(PackTgs(json)) != json"_q);
	} else {
		details.push_back(u"gzip %1 -> %2 KB ok"_q.arg(
			QString::number(json.size() / 1024., 'f', 1),
			QString::number(packed.size() / 1024., 'f', 1)));
	}

	// 2. Info.
	auto renderer = Renderer(json);
	const auto info = ReadInfo(json);
	if (!renderer.valid()) {
		problems.push_back(u"rlottie could not load the animation"_q);
	} else {
		const auto &real = renderer.info();
		details.push_back(u"%1x%2, %3 frames @ %4 fps"_q.arg(
			QString::number(real.size.width()),
			QString::number(real.size.height()),
			QString::number(real.frames),
			QString::number(real.fps)));
		if (info.frames != real.frames
			|| info.size != real.size
			|| std::abs(info.fps - real.fps) > 0.01) {
			problems.push_back(u"ReadInfo: %1x%2 %3 frames %4 fps, "
				"rlottie: %5x%6 %7 frames %8 fps"_q
				.arg(info.size.width())
				.arg(info.size.height())
				.arg(info.frames)
				.arg(info.fps)
				.arg(real.size.width())
				.arg(real.size.height())
				.arg(real.frames)
				.arg(real.fps));
		}
	}
	const auto frames = renderer.valid()
		? TestFrames(renderer.info().frames)
		: std::vector<int>();

	// 3. Colors.
	if (renderer.valid()) {
		const auto colorSize = QSize(kColorTestSide, kColorTestSide);
		auto stats = ColorStats();
		auto shiftedStats = ColorStats();
		const auto identity = AdjustColorsWithStats(json, 0, 0, 0, &stats);
		const auto full = AdjustColors(json, 360);
		const auto there = AdjustColorsWithStats(
			json,
			150,
			0,
			0,
			&shiftedStats);
		const auto back = AdjustColors(there, -150);
		const auto graded = AdjustColors(json, -40, -30, 20);
		struct Variant {
			QString name;
			QByteArray json;
			double limit = 0.;
		};
		const auto variants = std::vector<Variant>{
			{ u"hue 0"_q, identity, kIdentityMeanLimit },
			{ u"hue 360"_q, full, kIdentityMeanLimit },
			{ u"hue +150 -150"_q, back, kRoundTripMeanLimit },
			{ u"hsl -40 -30 +20"_q, graded, -1. },
		};
		auto worst = 0.;
		for (const auto &variant : variants) {
			auto adjusted = Renderer(variant.json);
			if (!adjusted.valid()
				|| adjusted.info().frames != renderer.info().frames) {
				problems.push_back(u"AdjustColors (%1) broke the animation"_q
					.arg(variant.name));
				continue;
			} else if (variant.limit < 0.) {
				continue;
			}
			for (const auto frame : frames) {
				const auto diff = CompareImages(
					renderer.render(frame, colorSize),
					adjusted.render(frame, colorSize));
				if (!diff.valid) {
					problems.push_back(u"AdjustColors (%1) frame %2: "
						"render failed"_q.arg(variant.name).arg(frame));
				} else if (diff.mean > variant.limit) {
					problems.push_back(u"AdjustColors (%1) frame %2: "
						"diff %3, limit %4"_q
						.arg(variant.name)
						.arg(frame)
						.arg(FormatDiff(diff))
						.arg(variant.limit));
				}
				worst = std::max(worst, diff.mean);
			}
		}
		details.push_back(u"colors %1 (+150: %2 changed), identity diff "
			"%3"_q
			.arg(stats.found)
			.arg(shiftedStats.changed)
			.arg(QString::number(worst, 'f', 2)));
		if (stats.changed) {
			problems.push_back(u"AdjustColors(0, 0, 0) rewrote %1 colors"_q
				.arg(stats.changed));
		}
		if (stats.found != shiftedStats.found) {
			problems.push_back(u"AdjustColors found %1 colors, then %2"_q
				.arg(stats.found)
				.arg(shiftedStats.found));
		}
	}

	// 4. SVG.
	auto svgParts = QStringList();
	const auto size = QSize(kSvgTestSide, kSvgTestSide);
	for (const auto frame : frames) {
		const auto svg = ExportSvg(json, frame);
		if (svg.svg.isEmpty()) {
			problems.push_back(u"ExportSvg frame %1: empty result"_q
				.arg(frame));
			continue;
		}
		const auto expected = renderer.render(frame, size);
		const auto actual = RenderSvgImage(svg.svg, size);
		if (actual.isNull()) {
			problems.push_back(u"ExportSvg frame %1: QSvgRenderer could "
				"not load the SVG (%2 KB)"_q
				.arg(frame)
				.arg(svg.svg.size() / 1024));
			continue;
		}
		const auto diff = CompareImages(expected, actual);
		if (!diff.valid) {
			problems.push_back(u"ExportSvg frame %1: compare failed"_q
				.arg(frame));
			continue;
		}
		const auto kind = !svg.vector
			? (u"RASTER (%1)"_q.arg(svg.reason))
			: svg.reason.isEmpty()
			? u"vector"_q
			: (u"vector (%1)"_q.arg(svg.reason));
		svgParts.push_back(u"f%1 %2 %3, %4 KB"_q
			.arg(frame)
			.arg(kind)
			.arg(FormatDiff(diff))
			.arg(svg.svg.size() / 1024));
		if (svg.vector) {
			++totals.vectorFrames;
			totals.vectorDiffSum += diff.mean;
			if (diff.mean > totals.worstVector) {
				totals.worstVector = diff.mean;
				totals.worstVectorName = u"%1 f%2"_q.arg(name).arg(frame);
			}
		} else {
			++totals.rasterFrames;
			totals.rasterList.push_back(u"%1 f%2: %3"_q
				.arg(name)
				.arg(frame)
				.arg(svg.reason));
		}
		if (!SvgDiffGood(diff)) {
			problems.push_back(u"ExportSvg frame %1 (%2) differs from "
				"rlottie: %3, limits: over %4% or mean %5 and tolerant %6"_q
				.arg(frame)
				.arg(svg.vector ? u"vector"_q : u"raster"_q)
				.arg(FormatDiff(diff))
				.arg(kSvgOverLimit)
				.arg(kSvgMeanLimit)
				.arg(kSvgTolerantLimit));
		}
	}
	if (!svgParts.isEmpty()) {
		details.push_back(u"svg "_q + svgParts.join(u"; "_q));
	}
	details.push_back(u"%1 ms"_q.arg(timer.elapsed()));

	const auto ok = problems.isEmpty();
	if (ok) {
		++totals.passed;
	} else {
		++totals.failed;
		totals.failedList.push_back(name);
	}
	log.push_back((ok ? u"ok   "_q : u"FAIL "_q)
		+ name
		+ u": "_q
		+ details.join(u", "_q));
	for (const auto &problem : problems) {
		log.push_back(u"       - "_q + problem);
	}
}

[[nodiscard]] bool TestBasics(QStringList &log) {
	auto problems = QStringList();
	if (!Unpack(QByteArray()).isEmpty()) {
		problems.push_back(u"Unpack(empty) is not empty"_q);
	}
	if (!Unpack("garbage"_q).isEmpty()) {
		problems.push_back(u"Unpack(garbage) is not empty"_q);
	}
	if (!PackTgs(QByteArray()).isEmpty()) {
		problems.push_back(u"PackTgs(empty) is not empty"_q);
	}
	const auto json = QByteArray(R"({"v":"5.5.2","w":100,"h":50})");
	const auto packed = PackTgs(json);
	if (Unpack(packed) != json) {
		problems.push_back(u"small gzip round trip failed"_q);
	}
	if (!Unpack(packed.left(packed.size() / 2)).isEmpty()) {
		problems.push_back(u"Unpack(truncated gzip) is not empty"_q);
	}
	if (ReadInfo(json).valid()) {
		problems.push_back(u"ReadInfo without ip / op / fr is valid"_q);
	}
	if (!AdjustColors("{\"broken\":"_q, 10).isEmpty()) {
		problems.push_back(u"AdjustColors(broken json) is not empty"_q);
	}
	const auto order = QByteArray(
		R"({"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"b":[1e-7,-2.50,"\"x\""]})");
	const auto shifted = AdjustColors(order, 120);
	const auto expected = QByteArray(
		R"({"ty":"fl","c":{"a":0,"k":[0,1,0,1]},"b":[1e-7,-2.50,"\"x\""]})");
	if (shifted != expected) {
		problems.push_back(u"AdjustColors(red, +120): "_q
			+ QString::fromUtf8(shifted));
	}
	const auto hex = AdjustColors(R"({"ty":1,"sc":"#ff0000"})"_q, -120);
	if (hex != R"({"ty":1,"sc":"#0000ff"})"_q) {
		problems.push_back(u"AdjustColors(#ff0000, -120): "_q
			+ QString::fromUtf8(hex));
	}
	if (!RenderFrame(QByteArray(), 0, QSize(10, 10)).isNull()) {
		problems.push_back(u"RenderFrame(empty) is not null"_q);
	}
	if (!ExportSvg(QByteArray(), 0).svg.isEmpty()) {
		problems.push_back(u"ExportSvg(empty) is not empty"_q);
	}
	log.push_back((problems.isEmpty() ? u"ok   "_q : u"FAIL "_q)
		+ u"basics: gzip, JSON order, hex / array colors, empty input"_q);
	for (const auto &problem : problems) {
		log.push_back(u"       - "_q + problem);
	}
	return problems.isEmpty();
}

} // namespace

bool RunSelfTest(QStringList &log) {
	auto timer = QElapsedTimer();
	timer.start();
	auto totals = TestTotals();
	const auto basics = TestBasics(log);

	auto files = std::vector<std::pair<QString, QByteArray>>();
	auto names = QStringList();
	auto it = QDirIterator(
		u":/animations"_q,
		QDir::Files,
		QDirIterator::Subdirectories);
	while (it.hasNext()) {
		names.push_back(it.next());
	}
	names.sort();
	for (const auto &path : names) {
		auto file = QFile(path);
		if (!file.open(QIODevice::ReadOnly)) {
			log.push_back(u"FAIL "_q + path + u": could not read"_q);
			++totals.failed;
			totals.failedList.push_back(path);
			continue;
		}
		files.emplace_back(path.mid(2), file.readAll());
	}
	log.push_back(u"lottie: %1 resource files under :/animations"_q
		.arg(files.size()));
	for (auto &entry : Synthetic()) {
		files.push_back(std::move(entry));
	}
	for (const auto &[name, data] : files) {
		TestAnimation(name, data, log, totals);
	}

	log.push_back(QString());
	log.push_back(u"lottie summary: %1 animations, %2 passed, %3 failed, "
		"%4 skipped (not Lottie)"_q
		.arg(totals.files)
		.arg(totals.passed)
		.arg(totals.failed)
		.arg(totals.skipped));
	log.push_back(u"svg frames: %1 vector (mean diff %2, worst %3 in %4), "
		"%5 raster fallbacks; limits: >%6% pixels off by >%7 or mean %8 "
		"and tolerant mean %9 on the 0..255 scale"_q
		.arg(totals.vectorFrames)
		.arg(QString::number(totals.vectorFrames
			? (totals.vectorDiffSum / totals.vectorFrames)
			: 0., 'f', 2))
		.arg(QString::number(totals.worstVector, 'f', 2))
		.arg(totals.worstVectorName.isEmpty()
			? u"-"_q
			: totals.worstVectorName)
		.arg(totals.rasterFrames)
		.arg(kSvgOverLimit)
		.arg(kPixelDelta)
		.arg(kSvgMeanLimit)
		.arg(kSvgTolerantLimit));
	for (const auto &raster : totals.rasterList) {
		log.push_back(u"  raster fallback: "_q + raster);
	}
	for (const auto &failed : totals.failedList) {
		log.push_back(u"  failed: "_q + failed);
	}
	log.push_back(u"lottie self-test took %1 ms"_q.arg(timer.elapsed()));
	const auto enough = (totals.files > 0);
	if (!enough) {
		log.push_back(u"FAIL no animations were tested"_q);
	}
	return basics && enough && !totals.failed;
}

} // namespace Oblivion::Lottie
