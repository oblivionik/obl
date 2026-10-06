/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_fx.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtGui/QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace Oblivion::Photo {
namespace {

constexpr auto kPointMin = -4.;
constexpr auto kPointMax = 5.;
constexpr auto kLegacySeedCount = 1000;

struct Registry {
	std::vector<std::unique_ptr<FxDescriptor>> descriptors;
	std::vector<const FxDescriptor*> sorted;
	std::vector<std::unique_ptr<FxPreset>> presets;
	std::vector<const FxPreset*> sortedPresets;
	std::vector<std::unique_ptr<FxCustomEditor>> editors;
};

struct SubTest {
	SelfTestSuite suite = SelfTestSuite::Doc;
	const char *name = nullptr;
	bool (*run)(QStringList &log) = nullptr;
};

// Set only on the thread that runs the registrars, only while it does:
// other threads wait for the finished registry in Instance().
thread_local Registry *Building/* = nullptr*/;

[[nodiscard]] std::vector<Fn<void()>> &Registrars() {
	static auto result = std::vector<Fn<void()>>();
	return result;
}

[[nodiscard]] std::vector<SubTest> &SubTests() {
	static auto result = std::vector<SubTest>();
	return result;
}

void RegisterClassic();

[[nodiscard]] const Registry &Instance() {
	// Never destroyed: a render may still run on a worker thread while
	// the process exits and the static objects are taken down.
	static auto &result = *new Registry();
	static auto once = std::once_flag();
	std::call_once(once, [] {
		Building = &result;
		RegisterClassic();
		for (const auto &callback : Registrars()) {
			if (callback) {
				callback();
			}
		}
		Building = nullptr;
		const auto groups = FxGroups();
		const auto position = [&](FxGroup group) {
			return int(ranges::find(groups, group) - begin(groups));
		};
		for (const auto &descriptor : result.descriptors) {
			result.sorted.push_back(descriptor.get());
		}
		std::stable_sort(
			begin(result.sorted),
			end(result.sorted),
			[&](const FxDescriptor *a, const FxDescriptor *b) {
				const auto ga = position(a->group);
				const auto gb = position(b->group);
				return (ga != gb) ? (ga < gb) : (a->order < b->order);
			});
		for (const auto &preset : result.presets) {
			result.sortedPresets.push_back(preset.get());
		}
		std::stable_sort(
			begin(result.sortedPresets),
			end(result.sortedPresets),
			[&](const FxPreset *a, const FxPreset *b) {
				const auto ga = position(a->group);
				const auto gb = position(b->group);
				return (ga != gb) ? (ga < gb) : (a->order < b->order);
			});
	});
	return result;
}

[[nodiscard]] bool ValidId(const QByteArray &id) {
	if (id.isEmpty()) {
		return false;
	}
	for (const auto ch : id) {
		const auto ok = (ch >= 'a' && ch <= 'z')
			|| (ch >= '0' && ch <= '9')
			|| (ch == '_')
			|| (ch == '.');
		if (!ok) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] uint64 HashBytes(uint64 seed, QByteArrayView bytes) {
	auto result = seed ^ 0xCBF29CE484222325ULL;
	for (const auto ch : bytes) {
		result ^= uchar(ch);
		result *= 0x100000001B3ULL;
	}
	return FxHashCombine(result, uint64(bytes.size()));
}

[[nodiscard]] uint64 HashDouble(uint64 seed, double value) {
	auto bits = uint64(0);
	if (value == 0.) {
		value = 0.; // No negative zero.
	}
	std::memcpy(&bits, &value, sizeof(bits));
	return FxHashCombine(seed, bits);
}

[[nodiscard]] uint64 HashValue(uint64 seed, const FxValue &value) {
	switch (value.type()) {
	case FxValue::Type::None: return FxHashCombine(seed, 1);
	case FxValue::Type::Number:
	case FxValue::Type::Integer:
		return HashDouble(FxHashCombine(seed, 2), value.number());
	case FxValue::Type::Boolean:
		return FxHashCombine(seed, value.boolean() ? 4 : 3);
	case FxValue::Type::Color:
		return FxHashCombine(FxHashCombine(seed, 5), value.color().rgba());
	case FxValue::Type::Point: {
		const auto point = value.point();
		return HashDouble(
			HashDouble(FxHashCombine(seed, 6), point.x()),
			point.y());
	}
	case FxValue::Type::Data:
		return HashBytes(FxHashCombine(seed, 7), value.data());
	}
	return seed;
}

[[nodiscard]] QString ColorToText(QColor color) {
	return u"#%1"_q.arg(uint(color.rgba()), 8, 16, QChar('0')).toUpper();
}

[[nodiscard]] std::optional<QColor> ColorFromText(const QString &text) {
	if (!text.startsWith('#') || (text.size() != 9 && text.size() != 7)) {
		return std::nullopt;
	}
	auto ok = false;
	const auto value = text.mid(1).toUInt(&ok, 16);
	if (!ok) {
		return std::nullopt;
	}
	return (text.size() == 7)
		? QColor::fromRgb(QRgb(value | 0xFF000000U))
		: QColor::fromRgba(QRgb(value));
}

[[nodiscard]] QJsonValue ValueToJson(const FxValue &value) {
	switch (value.type()) {
	case FxValue::Type::Number: return value.number();
	case FxValue::Type::Integer: return value.integer();
	case FxValue::Type::Boolean: return value.boolean();
	case FxValue::Type::Color: return ColorToText(value.color());
	case FxValue::Type::Point: {
		const auto point = value.point();
		return QJsonArray{ point.x(), point.y() };
	}
	case FxValue::Type::Data:
		return QJsonObject{
			{ u"data"_q, QString::fromLatin1(value.data().toBase64()) },
		};
	case FxValue::Type::None: break;
	}
	return QJsonValue();
}

[[nodiscard]] FxValue ValueFromJson(const QJsonValue &value) {
	if (value.isBool()) {
		return FxValue::Boolean(value.toBool());
	} else if (value.isDouble()) {
		const auto number = value.toDouble();
		return std::isfinite(number) ? FxValue::Number(number) : FxValue();
	} else if (value.isString()) {
		const auto color = ColorFromText(value.toString());
		return color ? FxValue::Color(*color) : FxValue();
	} else if (value.isArray()) {
		const auto array = value.toArray();
		if (array.size() == 2
			&& array.at(0).isDouble()
			&& array.at(1).isDouble()) {
			return FxValue::Point(
				QPointF(array.at(0).toDouble(), array.at(1).toDouble()));
		}
	} else if (value.isObject()) {
		const auto data = value.toObject().value(u"data"_q);
		if (data.isString()) {
			return FxValue::Data(
				QByteArray::fromBase64(data.toString().toLatin1()));
		}
	}
	return FxValue();
}

//
// The effects and adjustments of oblivion_photo_core.h as layer effects.
//

struct ClassicParam {
	EffectParam param = EffectParam::Amount;
	const char *id = nullptr;
};

constexpr auto kClassicParams = std::array<ClassicParam, 16>{ {
	{ EffectParam::Amount, "amount" },
	{ EffectParam::Size, "size" },
	{ EffectParam::Levels, "levels" },
	{ EffectParam::Red, "red" },
	{ EffectParam::Green, "green" },
	{ EffectParam::Blue, "blue" },
	{ EffectParam::Position, "position" },
	{ EffectParam::Width, "width" },
	{ EffectParam::Feather, "feather" },
	{ EffectParam::Angle, "angle" },
	{ EffectParam::Seed, "seed" },
	{ EffectParam::Mode, "mode" },
	{ EffectParam::Grain, "grain" },
	{ EffectParam::Threshold, "threshold" },
	{ EffectParam::Color1, "color1" },
	{ EffectParam::Color2, "color2" },
} };

[[nodiscard]] QByteArray ClassicParamId(EffectParam param) {
	for (const auto &entry : kClassicParams) {
		if (entry.param == param) {
			return QByteArray(entry.id);
		}
	}
	return QByteArray("value");
}

// The classic effects that got a better layer effect of their own (one
// that looks the same on the preview and on the export, with more
// settings) are not offered in the "add effect" menu any more. They stay
// registered: stacks saved before still render, and the Effects tab of
// the whole picture has them as it always had.
//
//   pixelate, posterize, halftone, vhs -> lofi.*
//   glitch -> glitch.*, chromatic -> lofi.aberration, glow -> lofi.bloom
//   bw -> adjust.bw, lens_blur -> blur.tilt_shift / blur.lens
[[nodiscard]] bool ClassicSuperseded(EffectType type) {
	switch (type) {
	case EffectType::Pixelate:
	case EffectType::Glitch:
	case EffectType::Vhs:
	case EffectType::Posterize:
	case EffectType::Halftone:
	case EffectType::ChromaticAberration:
	case EffectType::Glow:
	case EffectType::BlackWhite:
	case EffectType::LensBlur:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] uint32 ClassicFlags(EffectType type) {
	const auto hidden = ClassicSuperseded(type) ? uint32(kFxHidden) : 0U;
	switch (type) {
	case EffectType::Glitch:
	case EffectType::Vhs:
		return kFxNeighbours | kFxSeeded | hidden;
	case EffectType::Pixelate:
	case EffectType::Halftone:
	case EffectType::Emboss:
	case EffectType::ChromaticAberration:
	case EffectType::Glow:
	case EffectType::Film:
	case EffectType::LensBlur:
		return kFxNeighbours | hidden;
	default:
		return hidden;
	}
}

void RegisterClassicEffect(EffectType type, int order) {
	auto descriptor = FxDescriptor{
		.id = QByteArray("classic.") + EffectKey(type).toLatin1(),
		.group = FxGroup::Classic,
		.name = FxText::Custom([=] { return EffectName(type); }),
		.flags = ClassicFlags(type),
		.order = 100 + order,
	};
	const auto defaults = DefaultEffect(type);
	for (const auto &info : EffectParams(type)) {
		const auto param = info.param;
		const auto id = ClassicParamId(param);
		const auto name = FxText::Custom([=] {
			return EffectParamName(param);
		});
		if (info.color) {
			descriptor.params.push_back(FxColor(
				id,
				name,
				QColor::fromRgb((param == EffectParam::Color1)
					? defaults.color1
					: defaults.color2)));
		} else if (info.toggle) {
			descriptor.params.push_back(
				FxBool(id, name, info.defaultValue != 0));
		} else if (param == EffectParam::Seed) {
			descriptor.params.push_back(
				FxSeed(id, name, info.defaultValue));
		} else if (param == EffectParam::Angle) {
			descriptor.params.push_back(FxAngle(
				id,
				name,
				info.defaultValue,
				info.min,
				info.max));
		} else {
			const auto percent = (param == EffectParam::Red)
				|| (param == EffectParam::Green)
				|| (param == EffectParam::Blue);
			descriptor.params.push_back(FxInt(
				id,
				name,
				info.min,
				info.max,
				info.defaultValue,
				percent ? u"%"_q : QString()));
		}
	}
	descriptor.apply = [=](
			QImage &image,
			const FxParams &params,
			const FxContext &context) {
		auto effect = DefaultEffect(type);
		for (const auto &info : EffectParams(type)) {
			const auto id = ClassicParamId(info.param);
			if (info.color) {
				((info.param == EffectParam::Color1)
					? effect.color1
					: effect.color2) = params.color(id).rgb();
			} else if (info.toggle) {
				effect.setValue(info.param, params.boolean(id) ? 1 : 0);
			} else if (info.param == EffectParam::Seed) {
				effect.setValue(
					info.param,
					params.integer(id) % kLegacySeedCount);
			} else {
				effect.setValue(info.param, params.integer(id));
			}
		}
		auto state = EditState();
		state.effects.push_back(effect);
		return ApplyEdits(image, state, context.cancel);
	};
	RegisterFx(std::move(descriptor));
}

// "vignetteFeather" (the JSON key of the core) -> "vignette_feather".
[[nodiscard]] QByteArray ClassicAdjustId(Adjust adjust) {
	auto result = QByteArray();
	for (auto ch = AdjustDescriptor(adjust).key; *ch; ++ch) {
		if (*ch >= 'A' && *ch <= 'Z') {
			result.append('_');
			result.append(char(*ch - 'A' + 'a'));
		} else {
			result.append(*ch);
		}
	}
	return result;
}

void RegisterClassicAdjust(
		QByteArray id,
		FxText name,
		std::vector<Adjust> list,
		uint32 flags,
		int order) {
	auto descriptor = FxDescriptor{
		.id = std::move(id),
		.group = FxGroup::Classic,
		.name = std::move(name),
		.flags = flags,
		.order = order,
	};
	for (const auto adjust : list) {
		const auto &info = AdjustDescriptor(adjust);
		descriptor.params.push_back(FxInt(
			ClassicAdjustId(adjust),
			FxText::Custom([=] { return AdjustName(adjust); }),
			info.min,
			info.max,
			info.defaultValue));
	}
	const auto build = [=](const FxParams &params) {
		auto state = EditState();
		for (const auto adjust : list) {
			state.setValue(adjust, params.integer(ClassicAdjustId(adjust)));
		}
		return state;
	};
	descriptor.apply = [=](
			QImage &image,
			const FxParams &params,
			const FxContext &context) {
		return ApplyEdits(image, build(params), context.cancel);
	};
	descriptor.identity = [=](const FxParams &params) {
		return !HasColorEdits(build(params));
	};
	RegisterFx(std::move(descriptor));
}

void RegisterClassic() {
	// The adjustments of the Adjust tab as layer effects. The "add effect"
	// menu offers the adjust.* effects of oblivion_photo_fx_adjust.cpp
	// instead (the same sliders and more, done in linear light), these
	// are kept for the stacks that have them.
	RegisterClassicAdjust(
		"classic.light",
		tr::lng_oblivion_photo_ui_group_light,
		{
			Adjust::Exposure,
			Adjust::Brightness,
			Adjust::Contrast,
			Adjust::Highlights,
			Adjust::Shadows,
			Adjust::Whites,
			Adjust::Blacks,
			Adjust::Fade,
		},
		kFxHidden,
		0);
	RegisterClassicAdjust(
		"classic.color",
		tr::lng_oblivion_photo_ui_group_color,
		{
			Adjust::Temperature,
			Adjust::Tint,
			Adjust::Saturation,
			Adjust::Vibrance,
		},
		kFxHidden,
		1);
	RegisterClassicAdjust(
		"classic.detail",
		tr::lng_oblivion_photo_ui_group_details,
		{ Adjust::Clarity, Adjust::Sharpen, Adjust::Blur },
		kFxNeighbours | kFxHidden,
		2);
	RegisterClassicAdjust(
		"classic.vignette",
		FxText::Custom([] { return AdjustName(Adjust::Vignette); }),
		{ Adjust::Vignette, Adjust::VignetteFeather },
		kFxHidden,
		3);
	RegisterClassicAdjust(
		"classic.grain",
		FxText::Custom([] { return AdjustName(Adjust::Grain); }),
		{ Adjust::Grain, Adjust::GrainSize },
		kFxNeighbours | kFxHidden,
		4);

	auto filters = std::vector<QString>();
	auto names = std::vector<FxText>();
	for (const auto &id : FilterIds()) {
		if (id != kOriginalFilter) {
			filters.push_back(id);
			names.push_back(FxText::Custom([=] { return FilterName(id); }));
		}
	}
	RegisterFx({
		.id = "classic.filter",
		.group = FxGroup::Classic,
		.name = tr::lng_oblivion_photo_panel_fx_filter,
		.params = {
			FxChoice(
				"filter",
				tr::lng_oblivion_photo_panel_fx_filter,
				std::move(names)),
			FxInt(
				"intensity",
				tr::lng_oblivion_photo_ui_intensity,
				0,
				100,
				100),
		},
		.order = 5,
		.apply = [=](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto index = params.integer("filter");
			if (index < 0 || index >= int(filters.size())) {
				return !context.cancelled();
			}
			auto state = EditState();
			state.filter = filters[index];
			state.filterIntensity = params.integer("intensity");
			return ApplyEdits(image, state, context.cancel);
		},
		.identity = [](const FxParams &params) {
			return params.integer("intensity") <= 0;
		},
	});

	auto order = 0;
	for (const auto type : EffectTypes()) {
		RegisterClassicEffect(type, order++);
	}
}

[[nodiscard]] bool ValidPremultiplied(const QImage &image) {
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

} // namespace

FxText::FxText(tr::phrase<> phrase)
: _type(Type::Phrase)
, _phrase(phrase) {
}

FxText::FxText(QString fixed)
: _type(Type::Fixed)
, _fixed(std::move(fixed)) {
}

FxText FxText::Custom(Fn<QString()> compute) {
	auto result = FxText();
	if (compute) {
		result._type = Type::Custom;
		result._custom = std::move(compute);
	}
	return result;
}

bool FxText::empty() const {
	return (_type == Type::None);
}

QString FxText::now() const {
	switch (_type) {
	case Type::Phrase: return _phrase(tr::now);
	case Type::Fixed: return _fixed;
	case Type::Custom: return _custom();
	case Type::None: break;
	}
	return QString();
}

FxValue FxValue::Number(double value) {
	auto result = FxValue();
	result._type = Type::Number;
	result._a = std::isfinite(value) ? value : 0.;
	return result;
}

FxValue FxValue::Integer(int value) {
	auto result = FxValue();
	result._type = Type::Integer;
	result._a = value;
	return result;
}

FxValue FxValue::Boolean(bool value) {
	auto result = FxValue();
	result._type = Type::Boolean;
	result._a = value ? 1. : 0.;
	return result;
}

FxValue FxValue::Color(QColor value) {
	auto result = FxValue();
	result._type = Type::Color;
	result._a = double(uint32(value.isValid() ? value.rgba() : 0U));
	return result;
}

FxValue FxValue::Point(QPointF value) {
	auto result = FxValue();
	result._type = Type::Point;
	result._a = std::isfinite(value.x()) ? value.x() : 0.;
	result._b = std::isfinite(value.y()) ? value.y() : 0.;
	return result;
}

FxValue FxValue::Data(QByteArray value) {
	auto result = FxValue();
	result._type = Type::Data;
	result._data = std::move(value);
	return result;
}

FxValue::Type FxValue::type() const {
	return _type;
}

bool FxValue::empty() const {
	return (_type == Type::None);
}

double FxValue::number() const {
	return (_type == Type::Number
		|| _type == Type::Integer
		|| _type == Type::Boolean)
		? _a
		: 0.;
}

int FxValue::integer() const {
	const auto value = number();
	return int(std::lround(std::clamp(value, -2e9, 2e9)));
}

bool FxValue::boolean() const {
	return number() != 0.;
}

QColor FxValue::color() const {
	return (_type == Type::Color)
		? QColor::fromRgba(QRgb(uint32(_a)))
		: QColor(0, 0, 0, 0);
}

QPointF FxValue::point() const {
	return (_type == Type::Point) ? QPointF(_a, _b) : QPointF();
}

QByteArray FxValue::data() const {
	return (_type == Type::Data) ? _data : QByteArray();
}

bool operator==(const FxValue &a, const FxValue &b) {
	const auto numeric = [](FxValue::Type type) {
		return (type == FxValue::Type::Number)
			|| (type == FxValue::Type::Integer);
	};
	if (numeric(a._type) && numeric(b._type)) {
		return (a._a == b._a);
	}
	return (a._type == b._type)
		&& (a._a == b._a)
		&& (a._b == b._b)
		&& (a._data == b._data);
}

bool FxParams::has(QByteArrayView id) const {
	return !value(id).empty();
}

const FxValue &FxParams::value(QByteArrayView id) const {
	static const auto empty = FxValue();
	const auto i = std::lower_bound(
		begin(_list),
		end(_list),
		id,
		[](const Entry &entry, QByteArrayView id) {
			return QByteArrayView(entry.id) < id;
		});
	return (i != end(_list) && QByteArrayView(i->id) == id)
		? i->value
		: empty;
}

void FxParams::set(const QByteArray &id, FxValue value) {
	const auto view = QByteArrayView(id);
	const auto i = std::lower_bound(
		begin(_list),
		end(_list),
		view,
		[](const Entry &entry, QByteArrayView id) {
			return QByteArrayView(entry.id) < id;
		});
	if (i != end(_list) && i->id == id) {
		i->value = std::move(value);
	} else {
		_list.insert(i, Entry{ id, std::move(value) });
	}
}

void FxParams::remove(QByteArrayView id) {
	_list.erase(
		std::remove_if(begin(_list), end(_list), [&](const Entry &entry) {
			return QByteArrayView(entry.id) == id;
		}),
		end(_list));
}

double FxParams::number(QByteArrayView id) const {
	return value(id).number();
}

int FxParams::integer(QByteArrayView id) const {
	return value(id).integer();
}

bool FxParams::boolean(QByteArrayView id) const {
	return value(id).boolean();
}

QColor FxParams::color(QByteArrayView id) const {
	return value(id).color();
}

QPointF FxParams::point(QByteArrayView id) const {
	return value(id).point();
}

QByteArray FxParams::data(QByteArrayView id) const {
	return value(id).data();
}

const std::vector<FxParams::Entry> &FxParams::list() const {
	return _list;
}

bool FxParams::empty() const {
	return _list.empty();
}

FxParam FxParam::when(Fn<bool(const FxParams&)> predicate) const {
	auto result = *this;
	result.visible = std::move(predicate);
	return result;
}

FxParam FxParam::under(FxText title) const {
	auto result = *this;
	result.section = std::move(title);
	return result;
}

FxParam FxParam::stepped(double step) const {
	auto result = *this;
	result.step = (step > 0.) ? step : 1.;
	return result;
}

FxParam FxParam::styled(FxSliderLook look) const {
	auto result = *this;
	result.look = look;
	return result;
}

FxValue FxParam::defaultValue() const {
	switch (kind) {
	case FxParamKind::Float:
	case FxParamKind::Angle:
		return FxValue::Number(std::clamp(value, min, max));
	case FxParamKind::Int:
		return FxValue::Integer(
			int(std::lround(std::clamp(value, min, max))));
	case FxParamKind::Bool:
		return FxValue::Boolean(value != 0.);
	case FxParamKind::Choice:
		return FxValue::Integer(std::clamp(
			int(std::lround(value)),
			0,
			std::max(int(choices.size()) - 1, 0)));
	case FxParamKind::Color:
		return FxValue::Color(color.isValid() ? color : QColor(0, 0, 0));
	case FxParamKind::Point:
		return FxValue::Point(point);
	case FxParamKind::Seed:
		return FxValue::Integer(
			std::clamp(int(std::lround(value)), 0, kFxSeedMax));
	case FxParamKind::Custom:
		return FxValue::Data(data);
	}
	return FxValue();
}

FxValue FxParam::normalized(const FxValue &value) const {
	using Type = FxValue::Type;
	const auto numeric = (value.type() == Type::Number)
		|| (value.type() == Type::Integer)
		|| (value.type() == Type::Boolean);
	switch (kind) {
	case FxParamKind::Float:
	case FxParamKind::Angle:
		return numeric
			? FxValue::Number(std::clamp(value.number(), min, max))
			: defaultValue();
	case FxParamKind::Int:
		return numeric
			? FxValue::Integer(
				int(std::lround(std::clamp(value.number(), min, max))))
			: defaultValue();
	case FxParamKind::Bool:
		return numeric ? FxValue::Boolean(value.boolean()) : defaultValue();
	case FxParamKind::Choice:
		return numeric
			? FxValue::Integer(std::clamp(
				value.integer(),
				0,
				std::max(int(choices.size()) - 1, 0)))
			: defaultValue();
	case FxParamKind::Color: {
		if (value.type() != Type::Color) {
			return defaultValue();
		}
		auto result = value.color();
		if (!alpha) {
			result.setAlpha(255);
		}
		return FxValue::Color(result);
	}
	case FxParamKind::Point: {
		if (value.type() != Type::Point) {
			return defaultValue();
		}
		const auto result = value.point();
		return FxValue::Point(QPointF(
			std::clamp(result.x(), kPointMin, kPointMax),
			std::clamp(result.y(), kPointMin, kPointMax)));
	}
	case FxParamKind::Seed:
		return numeric
			? FxValue::Integer(std::clamp(value.integer(), 0, kFxSeedMax))
			: defaultValue();
	case FxParamKind::Custom: {
		if (value.type() != Type::Data) {
			return defaultValue();
		}
		const auto editor = FindFxCustomEditor(customType);
		return (editor && editor->normalize)
			? FxValue::Data(editor->normalize(value.data()))
			: value;
	}
	}
	return defaultValue();
}

FxParam FxFloat(
		QByteArray id,
		FxText name,
		double min,
		double max,
		double value,
		int decimals,
		QString suffix) {
	auto result = FxParam();
	result.id = std::move(id);
	result.name = std::move(name);
	result.kind = FxParamKind::Float;
	result.min = std::min(min, max);
	result.max = std::max(min, max);
	result.value = value;
	result.decimals = std::clamp(decimals, 0, 4);
	result.step = std::pow(10., -result.decimals);
	result.suffix = std::move(suffix);
	return result;
}

FxParam FxInt(
		QByteArray id,
		FxText name,
		int min,
		int max,
		int value,
		QString suffix) {
	auto result = FxFloat(
		std::move(id),
		std::move(name),
		min,
		max,
		value,
		0,
		std::move(suffix));
	result.kind = FxParamKind::Int;
	return result;
}

FxParam FxPixels(
		QByteArray id,
		FxText name,
		double min,
		double max,
		double value,
		int decimals) {
	auto result = FxFloat(
		std::move(id),
		std::move(name),
		min,
		max,
		value,
		decimals,
		u" px"_q);
	result.sourcePixels = true;
	return result;
}

FxParam FxBool(QByteArray id, FxText name, bool value) {
	auto result = FxParam();
	result.id = std::move(id);
	result.name = std::move(name);
	result.kind = FxParamKind::Bool;
	result.min = 0.;
	result.max = 1.;
	result.value = value ? 1. : 0.;
	return result;
}

FxParam FxChoice(
		QByteArray id,
		FxText name,
		std::vector<FxText> choices,
		int value) {
	auto result = FxParam();
	result.id = std::move(id);
	result.name = std::move(name);
	result.kind = FxParamKind::Choice;
	result.min = 0.;
	result.max = std::max(int(choices.size()) - 1, 0);
	result.value = value;
	result.choices = std::move(choices);
	return result;
}

FxParam FxColor(QByteArray id, FxText name, QColor value, bool alpha) {
	auto result = FxParam();
	result.id = std::move(id);
	result.name = std::move(name);
	result.kind = FxParamKind::Color;
	result.color = value;
	result.alpha = alpha;
	return result;
}

FxParam FxAngle(
		QByteArray id,
		FxText name,
		double value,
		double min,
		double max) {
	auto result = FxFloat(std::move(id), std::move(name), min, max, value);
	result.kind = FxParamKind::Angle;
	result.suffix = QString(QChar(0x00B0));
	return result;
}

FxParam FxPoint(QByteArray id, FxText name, QPointF value) {
	auto result = FxParam();
	result.id = std::move(id);
	result.name = std::move(name);
	result.kind = FxParamKind::Point;
	result.point = value;
	return result;
}

FxParam FxSeed(QByteArray id, FxText name, int value) {
	auto result = FxParam();
	result.id = std::move(id);
	result.name = name.empty()
		? FxText(tr::lng_oblivion_photo_panel_seed)
		: std::move(name);
	result.kind = FxParamKind::Seed;
	result.min = 0.;
	result.max = kFxSeedMax;
	result.value = value;
	return result;
}

FxParam FxCustom(
		QByteArray id,
		FxText name,
		QByteArray customType,
		QByteArray value) {
	auto result = FxParam();
	result.id = std::move(id);
	result.name = std::move(name);
	result.kind = FxParamKind::Custom;
	result.customType = std::move(customType);
	result.data = std::move(value);
	return result;
}

const FxParam *FxDescriptor::param(QByteArrayView id) const {
	for (const auto &param : params) {
		if (QByteArrayView(param.id) == id) {
			return &param;
		}
	}
	return nullptr;
}

void RegisterFx(FxDescriptor &&descriptor) {
	if (!Building || !ValidId(descriptor.id)) {
		return;
	}
	for (const auto &existing : Building->descriptors) {
		if (existing->id == descriptor.id) {
			return;
		}
	}
	Building->descriptors.push_back(
		std::make_unique<FxDescriptor>(std::move(descriptor)));
}

void RegisterFxPreset(FxPreset &&preset) {
	if (!Building || !ValidId(preset.id) || preset.stack.empty()) {
		return;
	}
	Building->presets.push_back(
		std::make_unique<FxPreset>(std::move(preset)));
}

void RegisterFxCustomEditor(FxCustomEditor &&editor) {
	if (!Building || editor.type.isEmpty()) {
		return;
	}
	Building->editors.push_back(
		std::make_unique<FxCustomEditor>(std::move(editor)));
}

FxRegistrar::FxRegistrar(Fn<void()> registerAll) {
	Registrars().push_back(std::move(registerAll));
}

const FxDescriptor *FindFx(QByteArrayView id) {
	if (Building) {
		// A registrar looks up an effect registered before it.
		for (const auto &descriptor : Building->descriptors) {
			if (QByteArrayView(descriptor->id) == id) {
				return descriptor.get();
			}
		}
		return nullptr;
	}
	for (const auto descriptor : Instance().sorted) {
		if (QByteArrayView(descriptor->id) == id) {
			return descriptor;
		}
	}
	return nullptr;
}

const std::vector<const FxDescriptor*> &AllFx() {
	return Instance().sorted;
}

std::vector<const FxDescriptor*> FxInGroup(FxGroup group) {
	auto result = std::vector<const FxDescriptor*>();
	for (const auto descriptor : Instance().sorted) {
		if (descriptor->group == group) {
			result.push_back(descriptor);
		}
	}
	return result;
}

const std::vector<const FxPreset*> &AllFxPresets() {
	return Instance().sortedPresets;
}

const FxCustomEditor *FindFxCustomEditor(QByteArrayView type) {
	const auto &editors = Building ? Building->editors : Instance().editors;
	for (const auto &editor : editors) {
		if (QByteArrayView(editor->type) == type) {
			return editor.get();
		}
	}
	return nullptr;
}

const std::vector<FxGroup> &FxGroups() {
	static const auto result = std::vector<FxGroup>{
		FxGroup::Light,
		FxGroup::Color,
		FxGroup::Detail,
		FxGroup::Finish,
		FxGroup::Blur,
		FxGroup::Distort,
		FxGroup::Lofi,
		FxGroup::Glitch,
		FxGroup::Stylize,
		FxGroup::Classic,
	};
	return result;
}

QByteArray FxGroupKey(FxGroup group) {
	switch (group) {
	case FxGroup::Light: return "light";
	case FxGroup::Color: return "color";
	case FxGroup::Detail: return "detail";
	case FxGroup::Finish: return "finish";
	case FxGroup::Blur: return "blur";
	case FxGroup::Distort: return "distort";
	case FxGroup::Lofi: return "lofi";
	case FxGroup::Glitch: return "glitch";
	case FxGroup::Stylize: return "stylize";
	case FxGroup::Classic: return "classic";
	}
	return "stylize";
}

FxParams DefaultFxParams(const FxDescriptor &descriptor) {
	auto result = FxParams();
	for (const auto &param : descriptor.params) {
		result.set(param.id, param.defaultValue());
	}
	return result;
}

FxParams NormalizedFxParams(
		const FxDescriptor &descriptor,
		const FxParams &params) {
	auto result = FxParams();
	for (const auto &param : descriptor.params) {
		result.set(param.id, param.normalized(params.value(param.id)));
	}
	return result;
}

FxInstance MakeFx(
		const QByteArray &id,
		std::initializer_list<FxParams::Entry> values) {
	auto result = FxInstance{ .id = id };
	const auto descriptor = FindFx(id);
	if (descriptor) {
		result.params = DefaultFxParams(*descriptor);
	}
	for (const auto &entry : values) {
		if (!descriptor) {
			result.params.set(entry.id, entry.value);
		} else if (const auto param = descriptor->param(entry.id)) {
			result.params.set(entry.id, param->normalized(entry.value));
		}
	}
	return result;
}

bool FxIsIdentity(const FxInstance &instance) {
	if (!instance.enabled) {
		return true;
	}
	const auto descriptor = FindFx(instance.id);
	if (!descriptor || !descriptor->apply) {
		return true;
	}
	return descriptor->identity
		&& descriptor->identity(
			NormalizedFxParams(*descriptor, instance.params));
}

uint64 FxHashCombine(uint64 seed, uint64 value) {
	auto z = seed + 0x9E3779B97F4A7C15ULL + (value << 6) + (value >> 2);
	z ^= value * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

uint64 FxHash(const FxInstance &instance) {
	auto result = HashBytes(0x0B11710ULL, instance.id);
	result = FxHashCombine(result, instance.enabled ? 1 : 0);
	const auto descriptor = FindFx(instance.id);
	const auto params = descriptor
		? NormalizedFxParams(*descriptor, instance.params)
		: instance.params;
	for (const auto &entry : params.list()) {
		result = HashBytes(result, entry.id);
		result = HashValue(result, entry.value);
	}
	return result;
}

bool FxPrepare(QImage &image) {
	if (image.isNull()) {
		return false;
	}
	if (image.format() != QImage::Format_ARGB32_Premultiplied) {
		image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	if (image.isNull()) {
		return false;
	}
	image.setDevicePixelRatio(1.);
	image.bits();
	return !image.isNull();
}

QImage FxResized(const QImage &image, QSize size) {
	if (image.isNull() || size.isEmpty()) {
		return QImage();
	}
	auto result = (image.size() == size)
		? image
		: image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	if (result.format() != QImage::Format_ARGB32_Premultiplied) {
		result = result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	result.setDevicePixelRatio(1.);
	return result;
}

bool ApplyFx(
		QImage &image,
		const FxInstance &instance,
		const FxContext &context) {
	if (image.isNull()) {
		return false;
	} else if (context.cancelled()) {
		return false;
	} else if (!instance.enabled) {
		return true;
	}
	const auto descriptor = FindFx(instance.id);
	if (!descriptor || !descriptor->apply) {
		return true;
	}
	const auto params = NormalizedFxParams(*descriptor, instance.params);
	if (descriptor->identity && descriptor->identity(params)) {
		return true;
	} else if (!FxPrepare(image)) {
		return false;
	}
	const auto size = image.size();
	if (!descriptor->apply(image, params, context) || image.isNull()) {
		return false;
	}
	// A broken effect must not break the layer geometry.
	if (image.size() != size
		|| image.format() != QImage::Format_ARGB32_Premultiplied) {
		image = FxResized(image, size);
	}
	image.setDevicePixelRatio(1.);
	return !image.isNull() && !context.cancelled();
}

bool ApplyFxStack(
		QImage &image,
		const std::vector<FxInstance> &stack,
		const FxContext &context) {
	if (image.isNull()) {
		return false;
	}
	for (const auto &instance : stack) {
		auto local = context;
		local.seed = uint32(FxHashCombine(context.seed, instance.uid));
		if (!ApplyFx(image, instance, local)) {
			return false;
		}
	}
	return !context.cancelled();
}

QJsonObject FxToJson(const FxInstance &instance) {
	auto result = QJsonObject();
	result.insert(u"id"_q, QString::fromLatin1(instance.id));
	if (!instance.enabled) {
		result.insert(u"off"_q, true);
	}
	const auto descriptor = FindFx(instance.id);
	auto params = QJsonObject();
	if (descriptor) {
		const auto values = NormalizedFxParams(*descriptor, instance.params);
		for (const auto &param : descriptor->params) {
			const auto &value = values.value(param.id);
			if (!(value == param.defaultValue())) {
				params.insert(
					QString::fromLatin1(param.id),
					ValueToJson(value));
			}
		}
	} else {
		for (const auto &entry : instance.params.list()) {
			if (!entry.value.empty()) {
				params.insert(
					QString::fromLatin1(entry.id),
					ValueToJson(entry.value));
			}
		}
	}
	if (!params.isEmpty()) {
		result.insert(u"params"_q, params);
	}
	return result;
}

std::optional<FxInstance> FxFromJson(const QJsonObject &object) {
	const auto id = object.value(u"id"_q).toString().toLatin1();
	if (!ValidId(id)) {
		return std::nullopt;
	}
	auto result = FxInstance{
		.id = id,
		.enabled = !object.value(u"off"_q).toBool(),
	};
	const auto params = object.value(u"params"_q).toObject();
	auto values = FxParams();
	for (auto i = params.begin(); i != params.end(); ++i) {
		const auto value = ValueFromJson(i.value());
		if (!value.empty()) {
			values.set(i.key().toLatin1(), value);
		}
	}
	const auto descriptor = FindFx(id);
	result.params = descriptor
		? NormalizedFxParams(*descriptor, values)
		: values;
	return result;
}

QByteArray SerializeFxStack(const std::vector<FxInstance> &stack) {
	auto array = QJsonArray();
	for (const auto &instance : stack) {
		array.push_back(FxToJson(instance));
	}
	return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

std::vector<FxInstance> DeserializeFxStack(const QByteArray &json) {
	auto result = std::vector<FxInstance>();
	const auto document = QJsonDocument::fromJson(json);
	if (!document.isArray()) {
		return result;
	}
	for (const auto &value : document.array()) {
		if (auto instance = FxFromJson(value.toObject())) {
			result.push_back(std::move(*instance));
		}
	}
	return result;
}

SelfTestRegistrar::SelfTestRegistrar(
		SelfTestSuite suite,
		const char *name,
		bool (*run)(QStringList &log)) {
	SubTests().push_back({ suite, name, run });
}

bool RunRegisteredSelfTests(SelfTestSuite suite, QStringList &log) {
	auto ok = true;
	for (const auto &test : SubTests()) {
		if (test.suite != suite || !test.run) {
			continue;
		}
		const auto name = QString::fromLatin1(test.name ? test.name : "?");
		log.push_back(u"-- "_q + name);
		if (!test.run(log)) {
			log.push_back(u"FAIL: sub-test "_q + name);
			ok = false;
		}
	}
	return ok;
}

QImage FxTestImage(int width, int height, bool alpha) {
	width = std::max(width, 1);
	height = std::max(height, 1);
	auto result = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return result;
	}
	for (auto y = 0; y != height; ++y) {
		const auto line = FxRow(result, y);
		const auto fy = y / float(height);
		for (auto x = 0; x != width; ++x) {
			const auto fx = x / float(width);
			auto color = FxRgba{
				0.15f + 0.8f * fx,
				0.2f + 0.7f * fy,
				0.85f - 0.6f * fx * fy,
				1.f,
			};
			const auto dx = fx - 0.68f;
			const auto dy = (fy - 0.35f) * height / float(width);
			if (dx * dx + dy * dy < 0.012f) {
				color = FxRgba{ 1.f, 0.92f, 0.55f, 1.f };
			}
			if (fy > 0.7f) {
				// Fine detail: stripes two source pixels of a 256 px wide
				// picture wide, so they survive a 2x downscale as texture.
				const auto stripe = (int(fx * 128.f) & 1) != 0;
				color.r *= stripe ? 0.55f : 1.f;
				color.g *= stripe ? 0.55f : 1.f;
				color.b *= stripe ? 0.75f : 1.f;
			}
			if (alpha) {
				const auto ax = fx - 0.25f;
				const auto ay = fy - 0.5f;
				const auto distance = std::sqrt(ax * ax + ay * ay);
				color.a = FxSmoothStep(0.08f, 0.2f, distance);
			}
			line[x] = FxPack(color);
		}
	}
	return result;
}

double FxImageDifference(const QImage &a, const QImage &b) {
	if (a.isNull() || b.isNull()) {
		return 255.;
	}
	const auto first = FxResized(a, a.size());
	const auto second = FxResized(b, a.size());
	if (first.isNull() || second.isNull()) {
		return 255.;
	}
	auto sum = 0.;
	for (auto y = 0; y != first.height(); ++y) {
		const auto one = FxRow(first, y);
		const auto two = FxRow(second, y);
		for (auto x = 0; x != first.width(); ++x) {
			for (auto shift = 0; shift != 32; shift += 8) {
				sum += std::abs(
					int((one[x] >> shift) & 0xFFU)
						- int((two[x] >> shift) & 0xFFU));
			}
		}
	}
	return sum / (4. * first.width() * first.height());
}

bool RunFxSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto info = [&](const QString &what) {
		log.push_back(u"   "_q + what);
	};

	// Values and parameters.
	{
		auto params = FxParams();
		params.set("b", FxValue::Integer(3));
		params.set("a", FxValue::Number(1.5));
		params.set("c", FxValue::Color(QColor(10, 20, 30, 40)));
		params.set("a", FxValue::Number(2.5));
		check(
			params.list().size() == 3
				&& params.number("a") == 2.5
				&& params.integer("b") == 3
				&& params.color("c") == QColor(10, 20, 30, 40)
				&& !params.has("d")
				&& params.number("d") == 0.,
			u"parameter values are stored by id"_q);
		check(
			FxValue::Number(3.) == FxValue::Integer(3)
				&& !(FxValue::Number(3.) == FxValue::Boolean(true))
				&& !(FxValue::Point(QPointF(1., 2.))
					== FxValue::Point(QPointF(1., 3.))),
			u"value comparison"_q);

		const auto ranged = FxFloat("x", FxText(), -10., 10., 2., 1);
		check(
			ranged.normalized(FxValue::Number(25.)).number() == 10.
				&& ranged.normalized(FxValue()).number() == 2.
				&& ranged.normalized(
					FxValue::Data("zz")).number() == 2.,
			u"a float parameter clamps and falls back"_q);
		const auto whole = FxInt("n", FxText(), 2, 16, 5);
		check(
			whole.normalized(FxValue::Number(7.6)).integer() == 8
				&& whole.normalized(FxValue::Number(7.6)).type()
					== FxValue::Type::Integer,
			u"an int parameter rounds"_q);
		const auto length = FxPixels("r", FxText(), 0., 250., 12.);
		const auto warm = ranged.styled(FxSliderLook::Temperature);
		check(
			length.sourcePixels
				&& (length.suffix == u" px"_q)
				&& ranged.suffix.isEmpty()
				&& (ranged.look == FxSliderLook::Plain)
				&& (warm.look == FxSliderLook::Temperature)
				&& (warm.id == ranged.id),
			u"a length in pixels names its unit, a slider look is kept"_q);
		const auto choice = FxChoice(
			"c",
			FxText(),
			{ FxText(u"a"_q), FxText(u"b"_q), FxText(u"c"_q) },
			1);
		check(
			choice.normalized(FxValue::Integer(9)).integer() == 2
				&& choice.defaultValue().integer() == 1,
			u"a choice parameter clamps the index"_q);
		const auto solid = FxColor("k", FxText(), QColor(1, 2, 3));
		check(
			solid.normalized(
				FxValue::Color(QColor(9, 8, 7, 6))).color()
				== QColor(9, 8, 7, 255),
			u"a color without alpha is made opaque"_q);
	}

	// Parallel helper.
	{
		const auto count = 100003;
		auto marks = std::vector<uchar>(count, 0);
		auto overlap = std::atomic<int>(0);
		FxParallel(count, 64, [&](int from, int till) {
			for (auto i = from; i != till; ++i) {
				if (marks[i]++) {
					++overlap;
				}
			}
		});
		const auto all = ranges::all_of(marks, [](uchar value) {
			return value == 1;
		});
		check(all && !overlap.load(), u"parallel chunks cover the range once"_q);
	}

	// The registry.
	const auto &all = AllFx();
	{
		auto unique = true;
		auto valid = true;
		for (const auto descriptor : all) {
			auto count = 0;
			for (const auto other : all) {
				count += (other->id == descriptor->id) ? 1 : 0;
			}
			unique = unique && (count == 1);
			valid = valid && descriptor->apply && ValidId(descriptor->id);
			auto ids = std::vector<QByteArray>();
			for (const auto &param : descriptor->params) {
				const auto bad = !ValidId(param.id)
					|| param.id.contains('.')
					|| ranges::contains(ids, param.id)
					|| (param.kind == FxParamKind::Choice
						&& param.choices.empty())
					|| ((param.kind == FxParamKind::Float
						|| param.kind == FxParamKind::Int
						|| param.kind == FxParamKind::Angle)
						&& !(param.min < param.max))
					|| (param.kind == FxParamKind::Custom
						&& param.customType.isEmpty());
				if (bad) {
					valid = false;
					log.push_back(u"FAIL: bad parameter %1 of %2"_q.arg(
						QString::fromLatin1(param.id),
						QString::fromLatin1(descriptor->id)));
				}
				ids.push_back(param.id);
			}
		}
		check(!all.empty() && unique, u"%1 effects, unique ids"_q.arg(
			all.size()));
		check(valid, u"every effect has apply and valid parameters"_q);
		check(
			FindFx("classic.glitch")
				&& FindFx("classic.light")
				&& FindFx("classic.filter")
				&& FxInGroup(FxGroup::Classic).size()
					== size_t(kEffectTypeCount + 6),
			u"the classic effects and adjustments are bridged"_q);

		// A classic effect is hidden from the "add effect" menu only while
		// the effect that replaced it is really there to be offered.
		struct Replaced {
			const char *classic = nullptr;
			const char *by = nullptr;
			FxGroup group = FxGroup::Classic;
		};
		const Replaced replaced[] = {
			Replaced{ "classic.light", "adjust.light", FxGroup::Light },
			Replaced{ "classic.color", "adjust.color", FxGroup::Color },
			Replaced{ "classic.detail", "adjust.presence", FxGroup::Detail },
			Replaced{ "classic.vignette", "adjust.vignette", FxGroup::Finish },
			Replaced{ "classic.grain", "adjust.grain", FxGroup::Finish },
			Replaced{ "classic.bw", "adjust.bw", FxGroup::Color },
			Replaced{ "classic.pixelate", "lofi.pixelate", FxGroup::Lofi },
			Replaced{ "classic.posterize", "lofi.posterize", FxGroup::Lofi },
			Replaced{ "classic.halftone", "lofi.halftone", FxGroup::Lofi },
			Replaced{ "classic.vhs", "lofi.vhs", FxGroup::Lofi },
			Replaced{ "classic.chromatic", "lofi.aberration", FxGroup::Lofi },
			Replaced{ "classic.glow", "lofi.bloom", FxGroup::Lofi },
			Replaced{ "classic.glitch", "glitch.rgb", FxGroup::Glitch },
			Replaced{ "classic.lens_blur", "blur.tilt_shift", FxGroup::Blur },
		};
		auto hiddenRight = true;
		auto hiddenCount = 0;
		for (const auto &entry : replaced) {
			const auto classic = FindFx(entry.classic);
			const auto by = FindFx(entry.by);
			// A build without the module of the group (a test harness)
			// has nothing to offer instead, that is not checked here.
			const auto linked = !FxInGroup(entry.group).empty();
			if (!classic
				|| !(classic->flags & kFxHidden)
				|| (linked && (!by || (by->flags & kFxHidden)))) {
				hiddenRight = false;
				log.push_back(u"FAIL: %1 is hidden without %2"_q.arg(
					QString::fromLatin1(entry.classic),
					QString::fromLatin1(entry.by)));
			}
			++hiddenCount;
		}
		auto visibleClassic = 0;
		for (const auto descriptor : FxInGroup(FxGroup::Classic)) {
			if (!(descriptor->flags & kFxHidden)) {
				++visibleClassic;
			}
		}
		check(
			hiddenRight
				&& (visibleClassic + hiddenCount
					== int(FxInGroup(FxGroup::Classic).size())),
			u"%1 replaced classic effects are hidden, %2 stay in the menu"_q
				.arg(hiddenCount)
				.arg(visibleClassic));
		for (const auto preset : AllFxPresets()) {
			for (const auto &instance : preset->stack) {
				if (!FindFx(instance.id)) {
					check(false, u"preset %1 uses an unknown effect %2"_q.arg(
						QString::fromLatin1(preset->id),
						QString::fromLatin1(instance.id)));
				}
			}
		}
	}

	// The bridge gives exactly what the core gives.
	{
		const auto source = FxTestImage(160, 120, true);
		auto state = EditState();
		state.effects.push_back(DefaultEffect(EffectType::Posterize));
		auto viaFx = source;
		const auto applied = ApplyFx(
			viaFx,
			MakeFx("classic.posterize"),
			FxContext{ .fullSize = source.size() });
		check(
			applied && SamePixels(viaFx, Render(source, state)),
			u"classic.posterize equals the core effect"_q);

		auto light = EditState();
		light.exposure = 20;
		light.contrast = 15;
		auto viaLight = source;
		const auto lightApplied = ApplyFx(
			viaLight,
			MakeFx("classic.light", {
				{ "exposure", FxValue::Integer(20) },
				{ "contrast", FxValue::Integer(15) },
			}),
			FxContext{ .fullSize = source.size() });
		check(
			lightApplied && SamePixels(viaLight, Render(source, light)),
			u"classic.light equals the core adjustments"_q);
		auto untouched = source;
		check(
			FxIsIdentity(MakeFx("classic.light"))
				&& ApplyFx(untouched, MakeFx("classic.light"), FxContext())
				&& untouched.cacheKey() == source.cacheKey(),
			u"an identity effect doesn't copy the image"_q);
		auto disabled = MakeFx("classic.invert");
		disabled.enabled = false;
		check(
			ApplyFx(untouched, disabled, FxContext())
				&& SamePixels(untouched, source),
			u"a disabled effect is skipped"_q);
		auto unknown = FxInstance{ .id = "no.such_effect" };
		unknown.params.set("x", FxValue::Number(1.));
		check(
			ApplyFx(untouched, unknown, FxContext())
				&& SamePixels(untouched, source),
			u"an unknown effect is skipped"_q);
		const auto restored = FxFromJson(FxToJson(unknown));
		check(
			restored && (*restored == unknown),
			u"an unknown effect survives the JSON round trip"_q);
	}

	// Cancelling.
	{
		auto image = FxTestImage(64, 64);
		const auto cancel = std::atomic<bool>(true);
		check(
			!ApplyFx(
				image,
				MakeFx("classic.glow"),
				FxContext{ .cancel = &cancel }),
			u"a cancelled effect reports it"_q);
	}

	// Every registered effect on a reduced picture.
	{
		const auto full = FxTestImage(256, 192, true);
		const auto half = FxResized(full, full.size() / 2);
		auto timer = QElapsedTimer();
		for (const auto descriptor : all) {
			const auto name = QString::fromLatin1(descriptor->id);
			const auto instance = MakeFx(descriptor->id);
			const auto context = FxContext{
				.scale = 1.,
				.seed = 77,
				.fullSize = full.size(),
			};
			timer.start();
			auto first = full;
			const auto applied = ApplyFx(first, instance, context);
			const auto ms = timer.nsecsElapsed() / 1e6;
			auto second = full;
			const auto again = ApplyFx(second, instance, context);
			const auto fine = applied
				&& again
				&& (first.size() == full.size())
				&& (first.format() == QImage::Format_ARGB32_Premultiplied)
				&& ValidPremultiplied(first);
			if (!fine) {
				check(false, name + u": applies, keeps the size, valid pixels"_q);
				continue;
			}
			if (!SamePixels(first, second)) {
				check(false, name + u": deterministic"_q);
				continue;
			}
			const auto restored = FxFromJson(FxToJson(instance));
			if (!restored
				|| !(*restored == instance)
				|| FxHash(*restored) != FxHash(instance)) {
				check(false, name + u": JSON round trip"_q);
				continue;
			}
			auto reduced = half;
			auto preview = context;
			preview.scale = 0.5;
			preview.preview = true;
			const auto previewed = ApplyFx(reduced, instance, preview);
			if (!previewed || reduced.size() != half.size()) {
				check(false, name + u": applies to the preview"_q);
				continue;
			}
			info(u"%1: %2 ms, preview differs by %3"_q.arg(
				name,
				QString::number(ms, 'f', 1),
				QString::number(
					FxImageDifference(
						reduced,
						FxResized(first, half.size())),
					'f',
					2)));
		}
		check(true, u"every effect ran on the test picture"_q);
	}

	// The preview of a size-relative effect looks like the export.
	{
		const auto full = FxTestImage(512, 384);
		const auto half = FxResized(full, full.size() / 2);
		const auto blur = MakeFx("classic.detail", {
			{ "blur", FxValue::Integer(30) },
		});
		auto large = full;
		auto reduced = half;
		const auto done = ApplyFx(
			large,
			blur,
			FxContext{ .scale = 1., .fullSize = full.size() })
			&& ApplyFx(
				reduced,
				blur,
				FxContext{ .scale = 0.5, .fullSize = full.size() });
		const auto difference = FxImageDifference(
			reduced,
			FxResized(large, half.size()));
		const auto changed = FxImageDifference(large, full);
		check(
			done && difference < 2.5 && changed > 2.5,
			u"blur preview matches the export (differs by %1, the effect "
			"changes %2)"_q.arg(
				QString::number(difference, 'f', 2),
				QString::number(changed, 'f', 2)));
	}

	// Serialization.
	{
		auto stack = std::vector<FxInstance>();
		stack.push_back(MakeFx("classic.duotone", {
			{ "color1", FxValue::Color(QColor(12, 34, 56)) },
			{ "amount", FxValue::Integer(70) },
		}));
		stack.push_back(MakeFx("classic.glitch", {
			{ "seed", FxValue::Integer(123) },
		}));
		stack.back().enabled = false;
		stack.push_back(MakeFx("classic.filter", {
			{ "filter", FxValue::Integer(3) },
			{ "intensity", FxValue::Integer(40) },
		}));
		const auto bytes = SerializeFxStack(stack);
		const auto restored = DeserializeFxStack(bytes);
		check(
			restored == stack && SerializeFxStack(restored) == bytes,
			u"stack JSON round trip (%1 bytes)"_q.arg(bytes.size()));
		check(
			FxHash(stack[0]) == FxHash(restored[0])
				&& FxHash(stack[0]) != FxHash(stack[1])
				&& FxHash(stack[0]) != FxHash(MakeFx("classic.duotone")),
			u"hashes follow the parameters"_q);
		check(
			DeserializeFxStack("not json").empty()
				&& DeserializeFxStack("[{\"id\":\"Bad Id\"}]").empty(),
			u"broken JSON gives an empty stack"_q);
		const auto tolerant = DeserializeFxStack(
			"[{\"id\":\"classic.posterize\","
			"\"params\":{\"levels\":99,\"nope\":1}}]");
		check(
			tolerant.size() == 1
				&& tolerant[0].params.integer("levels") == 16
				&& !tolerant[0].params.has("nope"),
			u"reading clamps values and drops unknown parameters"_q);
	}

	ok = RunRegisteredSelfTests(SelfTestSuite::Fx, log) && ok;
	return ok;
}

} // namespace Oblivion::Photo
