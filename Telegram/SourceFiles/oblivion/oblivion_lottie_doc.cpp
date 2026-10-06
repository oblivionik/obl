/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie_doc.h"

#include "oblivion/oblivion_lottie.h"

#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QLocale>
#include <QtGui/QImage>
#include <QtGui/QPainterPathStroker>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Oblivion::LottieEdit {
namespace Json {

struct Value::Heap {
	QString string;
	std::vector<Value> items;
	std::vector<Member> members;
	NodeId id = 0;
};

namespace {

constexpr auto kMaxDepth = 512;

std::atomic<NodeId> GlobalNextNodeId = 1;

[[nodiscard]] NodeId NextNodeId() {
	return GlobalNextNodeId.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] const Value &NullValue() {
	static const auto result = Value();
	return result;
}

[[nodiscard]] const std::vector<Value> &NoItems() {
	static const auto result = std::vector<Value>();
	return result;
}

[[nodiscard]] const std::vector<Member> &NoMembers() {
	static const auto result = std::vector<Member>();
	return result;
}

[[nodiscard]] bool IsDigit(char ch) {
	return (ch >= '0') && (ch <= '9');
}

[[nodiscard]] int HexDigit(char ch) {
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	} else if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	} else if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	return -1;
}

class Parser final {
public:
	explicit Parser(QByteArrayView data)
	: _begin(data.data())
	, _p(data.data())
	, _end(data.data() + data.size()) {
	}

	[[nodiscard]] std::optional<Value> parse(
		QString *error,
		bool rootOnly = false);

	// The result is not what the bytes say: an object had a key more than
	// once (see parseObject()) or a zero had a minus sign.
	[[nodiscard]] bool rewritten() const {
		return _rewritten;
	}

private:
	bool fail(const char *reason);
	void skipSpace();
	bool parseValue(Value &out, int depth);
	bool parseObject(Value &out, int depth);
	bool parseArray(Value &out, int depth);
	bool parseNumber(Value &out);
	bool parseLiteral(std::string_view word, Value value, Value &out);
	bool parseString(QString &out);
	bool parseKey(QByteArray &out);

	const char *_begin = nullptr;
	const char *_p = nullptr;
	const char *_end = nullptr;
	QString _error;
	std::unordered_map<std::string_view, QByteArray> _keys;
	bool _rewritten = false;

};

std::optional<Value> Parser::parse(QString *error, bool rootOnly) {
	if ((_end - _p) >= 3
		&& uchar(_p[0]) == 0xEF
		&& uchar(_p[1]) == 0xBB
		&& uchar(_p[2]) == 0xBF) {
		_p += 3;
	}
	auto result = Value();
	auto ok = parseValue(result, 0);
	if (ok && !rootOnly) {
		// Zero bytes after the root are padding, not data: the renderer
		// stops at the first of them, Oblivion::Lottie skips them too.
		skipSpace();
		while (_p != _end && *_p == '\0') {
			++_p;
		}
		if (_p != _end) {
			ok = fail("unexpected data after the root value");
		}
	}
	if (!ok) {
		if (error) {
			*error = _error;
		}
		return std::nullopt;
	}
	return result;
}

bool Parser::fail(const char *reason) {
	if (_error.isEmpty()) {
		_error = QString::fromLatin1(reason)
			+ u" at offset "_q
			+ QString::number(_p - _begin);
	}
	return false;
}

void Parser::skipSpace() {
	while (_p != _end
		&& (*_p == ' ' || *_p == '\n' || *_p == '\r' || *_p == '\t')) {
		++_p;
	}
}

bool Parser::parseValue(Value &out, int depth) {
	if (depth > kMaxDepth) {
		return fail("too deep nesting");
	}
	skipSpace();
	if (_p == _end) {
		return fail("unexpected end");
	}
	switch (*_p) {
	case '{': return parseObject(out, depth);
	case '[': return parseArray(out, depth);
	case '"': {
		auto string = QString();
		if (!parseString(string)) {
			return false;
		}
		out = Value::FromString(string);
		return true;
	}
	case 't': return parseLiteral("true", Value::FromBool(true), out);
	case 'f': return parseLiteral("false", Value::FromBool(false), out);
	case 'n': return parseLiteral("null", Value(), out);
	}
	return parseNumber(out);
}

bool Parser::parseLiteral(std::string_view word, Value value, Value &out) {
	if (size_t(_end - _p) < word.size()
		|| std::memcmp(_p, word.data(), word.size()) != 0) {
		return fail("invalid literal");
	}
	_p += word.size();
	out = std::move(value);
	return true;
}

bool Parser::parseNumber(Value &out) {
	const auto start = _p;
	if (_p != _end && *_p == '-') {
		++_p;
	}
	if (_p == _end || !IsDigit(*_p)) {
		return fail("invalid value");
	}
	while (_p != _end && IsDigit(*_p)) {
		++_p;
	}
	if (_p != _end && *_p == '.') {
		++_p;
		if (_p == _end || !IsDigit(*_p)) {
			return fail("invalid number");
		}
		while (_p != _end && IsDigit(*_p)) {
			++_p;
		}
	}
	if (_p != _end && (*_p == 'e' || *_p == 'E')) {
		++_p;
		if (_p != _end && (*_p == '+' || *_p == '-')) {
			++_p;
		}
		if (_p == _end || !IsDigit(*_p)) {
			return fail("invalid number");
		}
		while (_p != _end && IsDigit(*_p)) {
			++_p;
		}
	}
	auto ok = false;
	const auto value = QByteArrayView(start, _p - start).toDouble(&ok);
	if (!ok || !std::isfinite(value)) {
		return fail("invalid number");
	} else if (value == 0. && std::signbit(value)) {
		// "-0" is written back as "0", so it is read as that as well.
		_rewritten = true;
		out = Value::FromNumber(0.);
		return true;
	}
	out = Value::FromNumber(value);
	return true;
}

bool Parser::parseString(QString &out) {
	++_p;
	const auto start = _p;
	while (_p != _end && *_p != '"' && *_p != '\\') {
		if (uchar(*_p) < 0x20) {
			return fail("control character in a string");
		}
		++_p;
	}
	if (_p == _end) {
		return fail("unterminated string");
	} else if (*_p == '"') {
		out = QString::fromUtf8(start, _p - start);
		++_p;
		return true;
	}
	auto result = QString::fromUtf8(start, _p - start);
	while (true) {
		if (_p == _end) {
			return fail("unterminated string");
		}
		const auto ch = *_p;
		if (ch == '"') {
			++_p;
			break;
		} else if (ch == '\\') {
			++_p;
			if (_p == _end) {
				return fail("unterminated string");
			}
			switch (*_p++) {
			case '"': result.append(QChar('"')); break;
			case '\\': result.append(QChar('\\')); break;
			case '/': result.append(QChar('/')); break;
			case 'b': result.append(QChar('\b')); break;
			case 'f': result.append(QChar('\f')); break;
			case 'n': result.append(QChar('\n')); break;
			case 'r': result.append(QChar('\r')); break;
			case 't': result.append(QChar('\t')); break;
			case 'u': {
				if (_end - _p < 4) {
					return fail("invalid unicode escape");
				}
				auto code = 0;
				for (auto i = 0; i != 4; ++i) {
					const auto digit = HexDigit(_p[i]);
					if (digit < 0) {
						return fail("invalid unicode escape");
					}
					code = code * 16 + digit;
				}
				_p += 4;
				result.append(QChar(char16_t(code)));
			} break;
			default: return fail("invalid escape");
			}
		} else {
			const auto segment = _p;
			while (_p != _end && *_p != '"' && *_p != '\\') {
				if (uchar(*_p) < 0x20) {
					return fail("control character in a string");
				}
				++_p;
			}
			result.append(QString::fromUtf8(segment, _p - segment));
		}
	}
	out = std::move(result);
	return true;
}

bool Parser::parseKey(QByteArray &out) {
	if (_p == _end || *_p != '"') {
		return fail("expected a key");
	}
	const auto start = _p + 1;
	auto scan = start;
	while (scan != _end && *scan != '"' && *scan != '\\') {
		++scan;
	}
	if (scan != _end && *scan == '"') {
		const auto view = std::string_view(start, scan - start);
		auto i = _keys.find(view);
		if (i == end(_keys)) {
			i = _keys.emplace(view, QByteArray(start, scan - start)).first;
		}
		out = i->second;
		_p = scan + 1;
		return true;
	}
	auto string = QString();
	if (!parseString(string)) {
		return false;
	}
	out = string.toUtf8();
	return true;
}

bool Parser::parseObject(Value &out, int depth) {
	++_p;
	auto members = std::vector<Member>();
	skipSpace();
	if (_p != _end && *_p == '}') {
		++_p;
		out = Value::FromObject();
		return true;
	}
	// A key that is written twice: the last value wins, like Value::get()
	// reads such an object, and stays where the key was first. rlottie has
	// no rule for them (it may add the keyframes of both up), so they are
	// not kept: what is drawn and saved is what the document shows.
	constexpr auto kIndexFrom = 32;
	auto index = std::unique_ptr<std::unordered_map<std::string, int>>();
	const auto find = [&](const QByteArray &key) {
		const auto count = int(members.size());
		if (count < kIndexFrom) {
			for (auto i = 0; i != count; ++i) {
				if (members[i].key == key) {
					return i;
				}
			}
			return -1;
		} else if (!index) {
			index = std::make_unique<std::unordered_map<std::string, int>>();
			for (auto i = 0; i != count; ++i) {
				index->emplace(members[i].key.toStdString(), i);
			}
		}
		const auto i = index->find(key.toStdString());
		return (i != index->end()) ? i->second : -1;
	};
	while (true) {
		skipSpace();
		auto key = QByteArray();
		if (!parseKey(key)) {
			return false;
		}
		skipSpace();
		if (_p == _end || *_p != ':') {
			return fail("expected ':'");
		}
		++_p;
		auto value = Value();
		if (!parseValue(value, depth + 1)) {
			return false;
		}
		if (const auto existing = find(key); existing >= 0) {
			members[existing].value = std::move(value);
			_rewritten = true;
		} else {
			if (index) {
				index->emplace(key.toStdString(), int(members.size()));
			}
			members.push_back({ std::move(key), std::move(value) });
		}
		skipSpace();
		if (_p == _end) {
			return fail("unterminated object");
		} else if (*_p == ',') {
			++_p;
		} else if (*_p == '}') {
			++_p;
			break;
		} else {
			return fail("expected ',' or '}'");
		}
	}
	out = Value::FromObject(std::move(members));
	return true;
}

bool Parser::parseArray(Value &out, int depth) {
	++_p;
	auto items = std::vector<Value>();
	skipSpace();
	if (_p != _end && *_p == ']') {
		++_p;
		out = Value::FromArray();
		return true;
	}
	while (true) {
		auto value = Value();
		if (!parseValue(value, depth + 1)) {
			return false;
		}
		items.push_back(std::move(value));
		skipSpace();
		if (_p == _end) {
			return fail("unterminated array");
		} else if (*_p == ',') {
			++_p;
		} else if (*_p == ']') {
			++_p;
			break;
		} else {
			return fail("expected ',' or ']'");
		}
	}
	out = Value::FromArray(std::move(items));
	return true;
}

void WriteEscaped(QByteArray &out, QByteArrayView utf8) {
	out.append('"');
	const auto from = utf8.data();
	const auto till = from + utf8.size();
	auto run = from;
	for (auto ptr = from; ptr != till; ++ptr) {
		const auto ch = uchar(*ptr);
		const char *escape = nullptr;
		switch (ch) {
		case '"': escape = "\\\""; break;
		case '\\': escape = "\\\\"; break;
		case '\n': escape = "\\n"; break;
		case '\r': escape = "\\r"; break;
		case '\t': escape = "\\t"; break;
		case '\b': escape = "\\b"; break;
		case '\f': escape = "\\f"; break;
		}
		if (!escape && ch >= 0x20) {
			continue;
		}
		if (ptr > run) {
			out.append(run, ptr - run);
		}
		if (escape) {
			out.append(escape);
		} else {
			char buffer[8] = { 0 };
			std::snprintf(buffer, sizeof(buffer), "\\u%04x", unsigned(ch));
			out.append(buffer);
		}
		run = ptr + 1;
	}
	if (till > run) {
		out.append(run, till - run);
	}
	out.append('"');
}

void WriteNumber(QByteArray &out, double value) {
	if (value == 0.) {
		out.append('0');
	} else if (std::abs(value) < 1e15 && std::floor(value) == value) {
		out.append(QByteArray::number(qint64(value)));
	} else {
		out.append(QByteArray::number(
			value,
			'g',
			QLocale::FloatingPointShortest));
	}
}

void WriteValue(QByteArray &out, const Value &value) {
	switch (value.type()) {
	case Value::Type::Null: out.append("null"); return;
	case Value::Type::Bool:
		out.append(value.toBool() ? "true" : "false");
		return;
	case Value::Type::Number: WriteNumber(out, value.toDouble()); return;
	case Value::Type::String: WriteEscaped(out, value.toString().toUtf8()); return;
	case Value::Type::Array: {
		out.append('[');
		auto first = true;
		for (const auto &item : value.items()) {
			if (!first) {
				out.append(',');
			}
			first = false;
			WriteValue(out, item);
		}
		out.append(']');
	} return;
	case Value::Type::Object: {
		out.append('{');
		auto first = true;
		for (const auto &member : value.members()) {
			if (!first) {
				out.append(',');
			}
			first = false;
			WriteEscaped(out, member.key);
			out.append(':');
			WriteValue(out, member.value);
		}
		out.append('}');
	} return;
	}
}

} // namespace

Value Value::FromBool(bool value) {
	auto result = Value();
	result._type = Type::Bool;
	result._bool = value;
	return result;
}

Value Value::FromNumber(double value) {
	auto result = Value();
	result._type = Type::Number;
	result._number = std::isfinite(value) ? value : 0.;
	return result;
}

Value Value::FromString(const QString &value) {
	auto heap = std::make_shared<Heap>();
	heap->string = value;
	auto result = Value();
	result._type = Type::String;
	result._heap = std::move(heap);
	return result;
}

Value Value::FromNumbers(const std::vector<double> &values) {
	auto items = std::vector<Value>();
	items.reserve(values.size());
	for (const auto value : values) {
		items.push_back(FromNumber(value));
	}
	return FromArray(std::move(items));
}

Value Value::FromArray(std::vector<Value> items) {
	auto heap = std::make_shared<Heap>();
	heap->items = std::move(items);
	auto result = Value();
	result._type = Type::Array;
	result._heap = std::move(heap);
	return result;
}

Value Value::FromObject() {
	return FromObject(std::vector<Member>());
}

Value Value::FromObject(std::vector<Member> members) {
	auto heap = std::make_shared<Heap>();
	heap->members = std::move(members);
	heap->id = NextNodeId();
	auto result = Value();
	result._type = Type::Object;
	result._heap = std::move(heap);
	return result;
}

double Value::toDouble(double fallback) const {
	switch (_type) {
	case Type::Number: return _number;
	case Type::Bool: return _bool ? 1. : 0.;
	default: return fallback;
	}
}

double Value::toNumber(double fallback) const {
	if (_type == Type::Array) {
		const auto &list = items();
		return list.empty() ? fallback : list.front().toDouble(fallback);
	}
	return toDouble(fallback);
}

int Value::toInt(int fallback) const {
	const auto value = toNumber(std::numeric_limits<double>::quiet_NaN());
	if (std::isnan(value)) {
		return fallback;
	}
	return int(std::clamp(
		std::round(value),
		double(std::numeric_limits<int>::min()),
		double(std::numeric_limits<int>::max())));
}

bool Value::toBool(bool fallback) const {
	switch (_type) {
	case Type::Bool: return _bool;
	case Type::Number: return (_number != 0.);
	default: return fallback;
	}
}

QString Value::toString() const {
	return (_type == Type::String) ? _heap->string : QString();
}

std::vector<double> Value::numbers() const {
	if (_type == Type::Number) {
		return { _number };
	} else if (_type != Type::Array) {
		return {};
	}
	auto result = std::vector<double>();
	result.reserve(_heap->items.size());
	for (const auto &item : _heap->items) {
		result.push_back(item.toDouble(0.));
	}
	return result;
}

int Value::size() const {
	switch (_type) {
	case Type::Array: return int(_heap->items.size());
	case Type::Object: return int(_heap->members.size());
	default: return 0;
	}
}

const std::vector<Value> &Value::items() const {
	return (_type == Type::Array) ? _heap->items : NoItems();
}

const std::vector<Member> &Value::members() const {
	return (_type == Type::Object) ? _heap->members : NoMembers();
}

const Value &Value::at(int index) const {
	const auto &list = items();
	return (index >= 0 && index < int(list.size()))
		? list[index]
		: NullValue();
}

const Value &Value::get(QByteArrayView key) const {
	const auto index = indexOf(key);
	return (index >= 0) ? _heap->members[index].value : NullValue();
}

bool Value::has(QByteArrayView key) const {
	return (indexOf(key) >= 0);
}

int Value::indexOf(QByteArrayView key) const {
	if (_type != Type::Object) {
		return -1;
	}
	const auto &list = _heap->members;
	for (auto i = int(list.size()); i != 0;) {
		if (list[--i].key == key) {
			return i;
		}
	}
	return -1;
}

NodeId Value::id() const {
	return (_type == Type::Object) ? _heap->id : 0;
}

Value Value::with(QByteArrayView key, Value value) const {
	const auto index = indexOf(key);
	if (index >= 0 && _heap->members[index].value.sameAs(value)) {
		return *this;
	}
	auto heap = (_type == Type::Object)
		? std::make_shared<Heap>(*_heap)
		: std::make_shared<Heap>();
	if (_type != Type::Object) {
		heap->id = NextNodeId();
	}
	if (index >= 0) {
		heap->members[index].value = std::move(value);
	} else {
		heap->members.push_back({ key.toByteArray(), std::move(value) });
	}
	auto result = Value();
	result._type = Type::Object;
	result._heap = std::move(heap);
	return result;
}

Value Value::withInserted(
		QByteArrayView key,
		Value value,
		int position) const {
	if (has(key)) {
		return with(key, std::move(value));
	}
	auto heap = (_type == Type::Object)
		? std::make_shared<Heap>(*_heap)
		: std::make_shared<Heap>();
	if (_type != Type::Object) {
		heap->id = NextNodeId();
	}
	auto &list = heap->members;
	position = std::clamp(position, 0, int(list.size()));
	list.insert(
		begin(list) + position,
		Member{ key.toByteArray(), std::move(value) });
	auto result = Value();
	result._type = Type::Object;
	result._heap = std::move(heap);
	return result;
}

Value Value::without(QByteArrayView key) const {
	const auto index = indexOf(key);
	if (index < 0) {
		return *this;
	}
	auto heap = std::make_shared<Heap>(*_heap);
	heap->members.erase(begin(heap->members) + index);
	auto result = Value();
	result._type = Type::Object;
	result._heap = std::move(heap);
	return result;
}

Value Value::withItem(int index, Value value) const {
	const auto count = size();
	if (_type == Type::Array
		&& index >= 0
		&& index < count
		&& _heap->items[index].sameAs(value)) {
		return *this;
	}
	auto heap = (_type == Type::Array)
		? std::make_shared<Heap>(*_heap)
		: std::make_shared<Heap>();
	auto &list = heap->items;
	if (index >= 0 && index < int(list.size())) {
		list[index] = std::move(value);
	} else if (index == int(list.size())) {
		list.push_back(std::move(value));
	} else {
		return *this;
	}
	auto result = Value();
	result._type = Type::Array;
	result._heap = std::move(heap);
	return result;
}

Value Value::withInsertedItem(int index, Value value) const {
	auto heap = (_type == Type::Array)
		? std::make_shared<Heap>(*_heap)
		: std::make_shared<Heap>();
	auto &list = heap->items;
	index = std::clamp(index, 0, int(list.size()));
	list.insert(begin(list) + index, std::move(value));
	auto result = Value();
	result._type = Type::Array;
	result._heap = std::move(heap);
	return result;
}

Value Value::withoutItem(int index) const {
	if (_type != Type::Array || index < 0 || index >= size()) {
		return *this;
	}
	auto heap = std::make_shared<Heap>(*_heap);
	heap->items.erase(begin(heap->items) + index);
	auto result = Value();
	result._type = Type::Array;
	result._heap = std::move(heap);
	return result;
}

Value Value::withNewIds() const {
	switch (_type) {
	case Type::Array: {
		auto items = std::vector<Value>();
		items.reserve(_heap->items.size());
		for (const auto &item : _heap->items) {
			items.push_back(item.withNewIds());
		}
		return FromArray(std::move(items));
	}
	case Type::Object: {
		auto members = std::vector<Member>();
		members.reserve(_heap->members.size());
		for (const auto &member : _heap->members) {
			members.push_back({ member.key, member.value.withNewIds() });
		}
		return FromObject(std::move(members));
	}
	default: return *this;
	}
}

bool Value::sameAs(const Value &other) const {
	if (_type != other._type) {
		return false;
	}
	switch (_type) {
	case Type::Null: return true;
	case Type::Bool: return (_bool == other._bool);
	case Type::Number: return (_number == other._number);
	default: return (_heap == other._heap);
	}
}

bool operator==(const Value &a, const Value &b) {
	if (a._type != b._type) {
		return false;
	}
	switch (a._type) {
	case Value::Type::Null: return true;
	case Value::Type::Bool: return (a._bool == b._bool);
	case Value::Type::Number: return (a._number == b._number);
	case Value::Type::String:
		return (a._heap == b._heap) || (a._heap->string == b._heap->string);
	case Value::Type::Array: {
		if (a._heap == b._heap) {
			return true;
		}
		const auto &x = a._heap->items;
		const auto &y = b._heap->items;
		if (x.size() != y.size()) {
			return false;
		}
		for (auto i = size_t(0); i != x.size(); ++i) {
			if (!(x[i] == y[i])) {
				return false;
			}
		}
		return true;
	}
	case Value::Type::Object: {
		if (a._heap == b._heap) {
			return true;
		}
		const auto &x = a._heap->members;
		const auto &y = b._heap->members;
		if (x.size() != y.size()) {
			return false;
		}
		for (auto i = size_t(0); i != x.size(); ++i) {
			if (x[i].key != y[i].key || !(x[i].value == y[i].value)) {
				return false;
			}
		}
		return true;
	}
	}
	return false;
}

std::optional<Value> Parse(QByteArrayView json, QString *error) {
	return Parser(json).parse(error);
}

std::optional<Value> ParseRoot(QByteArrayView json, bool *rewritten) {
	auto parser = Parser(json);
	auto result = parser.parse(nullptr, true);
	if (rewritten) {
		*rewritten = parser.rewritten();
	}
	return result;
}

QByteArray Serialize(const Value &value) {
	auto result = QByteArray();
	result.reserve(64 * 1024);
	WriteValue(result, value);
	return result;
}

} // namespace Json

namespace {

using Json::Member;
using Json::Value;

constexpr auto kSameTime = 1e-6;
constexpr auto kMergeTimeout = crl::time(1500);
constexpr auto kMaxUndoSteps = 256;
constexpr auto kMaxUnpackedSize = 8 * 1024 * 1024;
constexpr auto kMaxPrecompDepth = 8;

struct PathStep {
	QByteArray key;
	int index = -1;
};
using Path = std::vector<PathStep>;

[[nodiscard]] Path Append(Path path, const QByteArray &key) {
	path.push_back({ key });
	return path;
}

[[nodiscard]] Path Append(Path path, int index) {
	path.push_back({ QByteArray(), index });
	return path;
}

[[nodiscard]] Path Concat(Path path, const Path &tail) {
	path.insert(end(path), begin(tail), end(tail));
	return path;
}

[[nodiscard]] const Value &GetIn(const Value &root, const Path &path) {
	auto current = &root;
	for (const auto &step : path) {
		current = (step.index >= 0)
			? &current->at(step.index)
			: &current->get(step.key);
	}
	return *current;
}

[[nodiscard]] Value SetIn(
		const Value &root,
		const Path &path,
		Value value,
		int from = 0) {
	if (from == int(path.size())) {
		return value;
	}
	const auto &step = path[from];
	if (step.index >= 0) {
		if (!root.isArray() || step.index >= root.size()) {
			return root;
		}
		const auto &child = root.at(step.index);
		return root.withItem(
			step.index,
			SetIn(child, path, std::move(value), from + 1));
	}
	const auto &child = root.get(step.key);
	auto updated = SetIn(child, path, std::move(value), from + 1);
	if (updated.sameAs(child) && root.has(step.key)) {
		return root;
	}
	return root.with(step.key, std::move(updated));
}

[[nodiscard]] std::optional<Path> RelativePath(
		const Value &start,
		QByteArrayView path) {
	if (path.isEmpty()) {
		return std::nullopt;
	}
	auto result = Path();
	auto current = &start;
	for (const auto &part : path.toByteArray().split('.')) {
		if (part.isEmpty()) {
			return std::nullopt;
		} else if (current->isArray()) {
			auto ok = false;
			const auto index = part.toInt(&ok);
			if (!ok || index < 0) {
				return std::nullopt;
			}
			result.push_back({ QByteArray(), index });
			current = &current->at(index);
		} else {
			result.push_back({ part });
			current = &current->get(part);
		}
	}
	return result;
}

[[nodiscard]] double RoundTo(double value, int decimals) {
	static const auto kFactors = std::array<double, 9>{
		1., 10., 100., 1e3, 1e4, 1e5, 1e6, 1e7, 1e8
	};
	const auto factor = kFactors[std::clamp(decimals, 0, 8)];
	const auto result = std::round(value * factor) / factor;
	return (result == 0.) ? 0. : result;
}

[[nodiscard]] double RoundValue(double value) {
	return RoundTo(value, 4);
}

[[nodiscard]] double RoundTime(double value) {
	return RoundTo(value, 3);
}

[[nodiscard]] Value Number(double value) {
	return Value::FromNumber(value);
}

[[nodiscard]] Value Numbers(const std::vector<double> &values) {
	return Value::FromNumbers(values);
}

[[nodiscard]] Value Object(std::vector<Member> members) {
	return Value::FromObject(std::move(members));
}

[[nodiscard]] Value StaticProperty(Value k) {
	return Object({ { "a", Number(0) }, { "k", std::move(k) } });
}

[[nodiscard]] bool Near(double a, double b, double epsilon = 1e-6) {
	return std::abs(a - b) <= epsilon;
}

[[nodiscard]] bool Near(QPointF a, QPointF b, double epsilon = 1e-6) {
	return Near(a.x(), b.x(), epsilon) && Near(a.y(), b.y(), epsilon);
}

[[nodiscard]] QByteArray GunzipFallback(const QByteArray &data) {
	auto stream = z_stream();
	stream.zalloc = nullptr;
	stream.zfree = nullptr;
	stream.opaque = nullptr;
	if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
		return QByteArray();
	}
	auto result = QByteArray();
	auto buffer = QByteArray(64 * 1024, Qt::Uninitialized);
	stream.avail_in = uInt(data.size());
	stream.next_in = reinterpret_cast<Bytef*>(
		const_cast<char*>(data.constData()));
	auto status = Z_OK;
	while (status == Z_OK) {
		stream.avail_out = uInt(buffer.size());
		stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
		status = inflate(&stream, Z_NO_FLUSH);
		if (status != Z_OK && status != Z_STREAM_END) {
			result = QByteArray();
			break;
		}
		result.append(buffer.constData(), buffer.size() - stream.avail_out);
		if (result.size() > kMaxUnpackedSize) {
			result = QByteArray();
			break;
		}
	}
	inflateEnd(&stream);
	return result;
}

[[nodiscard]] QByteArray GzipFallback(const QByteArray &data) {
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
	auto result = QByteArray(
		qsizetype(deflateBound(&stream, uLong(data.size())) + 64),
		Qt::Uninitialized);
	stream.avail_in = uInt(data.size());
	stream.next_in = reinterpret_cast<Bytef*>(
		const_cast<char*>(data.constData()));
	stream.avail_out = uInt(result.size());
	stream.next_out = reinterpret_cast<Bytef*>(result.data());
	const auto status = deflate(&stream, Z_FINISH);
	const auto total = qsizetype(stream.total_out);
	deflateEnd(&stream);
	if (status != Z_STREAM_END) {
		return QByteArray();
	}
	result.resize(total);
	return result;
}

// rlottie's VInterpolator: cubic bezier easing through (0, 0), (x1, y1),
// (x2, y2), (1, 1), solved for t with a sample table, Newton-Raphson
// iterations and bisection fallback, in float precision. The same math
// is used here so that the editor shows the values the renderer uses.
class BezierSolver final {
public:
	BezierSolver(float x1, float y1, float x2, float y2)
	: _x1(x1)
	, _y1(y1)
	, _x2(x2)
	, _y2(y2) {
		if (_x1 != _y1 || _x2 != _y2) {
			for (auto i = 0; i != kTableSize; ++i) {
				_samples[i] = Calc(float(i) * kStep, _x1, _x2);
			}
		}
	}

	[[nodiscard]] float value(float x) const {
		if (_x1 == _y1 && _x2 == _y2) {
			return x;
		}
		return Calc(tForX(x), _y1, _y2);
	}

private:
	static constexpr auto kTableSize = 11;
	static constexpr auto kStep = 1.f / float(kTableSize - 1);

	[[nodiscard]] static float A(float a1, float a2) {
		return 1.f - 3.f * a2 + 3.f * a1;
	}
	[[nodiscard]] static float B(float a1, float a2) {
		return 3.f * a2 - 6.f * a1;
	}
	[[nodiscard]] static float C(float a1) {
		return 3.f * a1;
	}
	[[nodiscard]] static float Calc(float t, float a1, float a2) {
		return ((A(a1, a2) * t + B(a1, a2)) * t + C(a1)) * t;
	}
	[[nodiscard]] static float Slope(float t, float a1, float a2) {
		return 3.f * A(a1, a2) * t * t + 2.f * B(a1, a2) * t + C(a1);
	}

	[[nodiscard]] float tForX(float x) const {
		auto intervalStart = 0.f;
		auto current = 1;
		const auto last = kTableSize - 1;
		for (; current != last && _samples[current] <= x; ++current) {
			intervalStart += kStep;
		}
		--current;
		const auto dist = (x - _samples[current])
			/ (_samples[current + 1] - _samples[current]);
		const auto guess = intervalStart + dist * kStep;
		if (!std::isfinite(guess)) {
			return x;
		}
		const auto slope = Slope(guess, _x1, _x2);
		if (slope >= 0.02f) {
			return newton(x, guess);
		} else if (slope == 0.f) {
			return guess;
		}
		return subdivide(x, intervalStart, intervalStart + kStep);
	}

	[[nodiscard]] float newton(float x, float guess) const {
		for (auto i = 0; i != 4; ++i) {
			const auto currentX = Calc(guess, _x1, _x2) - x;
			const auto slope = Slope(guess, _x1, _x2);
			if (slope == 0.f) {
				return guess;
			}
			guess -= currentX / slope;
		}
		return guess;
	}

	[[nodiscard]] float subdivide(float x, float a, float b) const {
		auto currentX = 0.f;
		auto currentT = 0.f;
		auto i = 0;
		do {
			currentT = a + (b - a) / 2.f;
			currentX = Calc(currentT, _x1, _x2) - x;
			if (currentX > 0.f) {
				b = currentT;
			} else {
				a = currentT;
			}
		} while (std::fabs(currentX) > 0.0000001f && ++i < 10);
		return currentT;
	}

	float _x1 = 0.f;
	float _y1 = 0.f;
	float _x2 = 0.f;
	float _y2 = 0.f;
	std::array<float, kTableSize> _samples = {};

};

} // namespace

PropValue PropValue::Scalar(double value) {
	return PropValue{ { value } };
}

PropValue PropValue::Point(QPointF value) {
	return PropValue{ { value.x(), value.y() } };
}

PropValue PropValue::Color(const QColor &color) {
	return PropValue{ { color.redF(), color.greenF(), color.blueF(), 1. } };
}

PropValue PropValue::Path(PathData path) {
	auto result = PropValue();
	result.path = std::move(path);
	return result;
}

double PropValue::scalar(double fallback) const {
	return numbers.empty() ? fallback : numbers.front();
}

QPointF PropValue::point() const {
	return QPointF(
		(numbers.size() > 0) ? numbers[0] : 0.,
		(numbers.size() > 1) ? numbers[1] : 0.);
}

QColor PropValue::color() const {
	const auto channel = [&](size_t index) {
		return (index < numbers.size())
			? std::clamp(numbers[index], 0., 1.)
			: 0.;
	};
	return QColor::fromRgbF(
		float(channel(0)),
		float(channel(1)),
		float(channel(2)));
}

std::vector<GradientStop> ColorStops(
		const PropValue &gradient,
		int colorStops) {
	auto result = std::vector<GradientStop>();
	const auto &list = gradient.numbers;
	for (auto i = 0; i < colorStops; ++i) {
		const auto base = size_t(i) * 4;
		if (base + 3 >= list.size()) {
			break;
		}
		result.push_back({
			list[base],
			QColor::fromRgbF(
				float(std::clamp(list[base + 1], 0., 1.)),
				float(std::clamp(list[base + 2], 0., 1.)),
				float(std::clamp(list[base + 3], 0., 1.))),
		});
	}
	return result;
}

std::vector<GradientStop> OpacityStops(
		const PropValue &gradient,
		int colorStops) {
	auto result = std::vector<GradientStop>();
	const auto &list = gradient.numbers;
	for (auto base = size_t(std::max(colorStops, 0)) * 4;
		base + 1 < list.size();
		base += 2) {
		auto color = QColor(0, 0, 0);
		color.setAlphaF(float(std::clamp(list[base + 1], 0., 1.)));
		result.push_back({ list[base], color });
	}
	return result;
}

Easing Easing::Linear() {
	return Easing();
}

Easing Easing::EaseIn() {
	return Easing{ QPointF(0.42, 0.), QPointF(1., 1.) };
}

Easing Easing::EaseOut() {
	return Easing{ QPointF(0., 0.), QPointF(0.58, 1.) };
}

Easing Easing::EaseInOut() {
	return Easing{ QPointF(0.42, 0.), QPointF(0.58, 1.) };
}

Easing Easing::Hold() {
	auto result = Easing();
	result.hold = true;
	return result;
}

Easing Easing::FromPreset(EasingPreset preset) {
	switch (preset) {
	case EasingPreset::Linear: return Linear();
	case EasingPreset::EaseIn: return EaseIn();
	case EasingPreset::EaseOut: return EaseOut();
	case EasingPreset::EaseInOut: return EaseInOut();
	case EasingPreset::Hold: return Hold();
	case EasingPreset::Custom: return Linear();
	}
	return Linear();
}

EasingPreset Easing::preset() const {
	if (hold) {
		return EasingPreset::Hold;
	}
	constexpr auto kEpsilon = 1e-3;
	const auto outLinear = Near(out.x(), out.y(), kEpsilon);
	const auto inLinear = Near(in.x(), in.y(), kEpsilon);
	const auto outEase = (out.x() > out.y() + kEpsilon)
		&& Near(out.y(), 0., kEpsilon);
	const auto inEase = (in.y() > in.x() + kEpsilon)
		&& Near(in.y(), 1., kEpsilon);
	if (outLinear && inLinear) {
		return EasingPreset::Linear;
	} else if (outEase && inLinear) {
		return EasingPreset::EaseIn;
	} else if (outLinear && inEase) {
		return EasingPreset::EaseOut;
	} else if (outEase && inEase) {
		return EasingPreset::EaseInOut;
	}
	return EasingPreset::Custom;
}

double Easing::apply(double x) const {
	if (hold) {
		return 0.;
	}
	return BezierSolver(
		float(out.x()),
		float(out.y()),
		float(in.x()),
		float(in.y())).value(float(std::clamp(x, 0., 1.)));
}

bool ValidationResult::ok() const {
	return ranges::none_of(issues, [](const Issue &issue) {
		return (issue.severity == IssueSeverity::Error);
	});
}

bool ValidationResult::hasFixable() const {
	return ranges::any_of(issues, [](const Issue &issue) {
		return issue.fixable && !issue.fixChangesPicture;
	});
}

std::vector<Issue> ValidationResult::of(IssueCategory category) const {
	auto result = std::vector<Issue>();
	for (const auto &issue : issues) {
		if (issue.category == category) {
			result.push_back(issue);
		}
	}
	return result;
}

const Issue *ValidationResult::find(IssueType type) const & {
	for (const auto &issue : issues) {
		if (issue.type == type) {
			return &issue;
		}
	}
	return nullptr;
}

IssueCategory CategoryOf(IssueType type) {
	switch (type) {
	case IssueType::InvalidComposition:
	case IssueType::MissingVersion:
	case IssueType::CanvasSize:
	case IssueType::FrameRate:
	case IssueType::Duration:
	case IssueType::FileSize:
	case IssueType::MissingTgsMarker:
		return IssueCategory::File;
	case IssueType::Images:
	case IssueType::Expressions:
	case IssueType::Layers3D:
	case IssueType::TextLayers:
	case IssueType::Masks:
	case IssueType::Effects:
	case IssueType::Solids:
	case IssueType::TimeStretch:
	case IssueType::TimeRemap:
	case IssueType::MergePaths:
	case IssueType::Repeaters:
	case IssueType::StarShapes:
	case IssueType::GradientStrokes:
	case IssueType::AutoOrient:
	case IssueType::TrackMattes:
		return IssueCategory::Forbidden;
	case IssueType::BrokenKeyframes:
	case IssueType::UnsupportedShapes:
	case IssueType::BrokenMattes:
	case IssueType::MatteLinks:
	case IssueType::MasksOff:
	case IssueType::MaskModes:
	case IssueType::MaskOptions:
	case IssueType::MaskInverted:
	case IssueType::PathVertices:
	case IssueType::KeyOrder:
	case IssueType::ParentLinks:
	case IssueType::RendererHang:
		return IssueCategory::NotRendered;
	case IssueType::OutOfCanvas:
		return IssueCategory::Advice;
	}
	return IssueCategory::File;
}

bool RenderedByTelegram(IssueType type) {
	switch (CategoryOf(type)) {
	case IssueCategory::File:
	case IssueCategory::Advice:
		return true;
	case IssueCategory::NotRendered:
		return false;
	case IssueCategory::Forbidden:
		break;
	}
	switch (type) {
	case IssueType::Masks:
	case IssueType::Solids:
	case IssueType::TimeStretch:
	case IssueType::TimeRemap:
	case IssueType::Repeaters:
	case IssueType::StarShapes:
	case IssueType::GradientStrokes:
	case IssueType::AutoOrient:
	case IssueType::TrackMattes:
		return true;
	default:
		return false;
	}
}

bool FixChangesPicture(IssueType type) {
	switch (type) {
	case IssueType::MasksOff:
	case IssueType::MaskModes:
	case IssueType::MaskInverted:
	case IssueType::KeyOrder:
		return true;
	default:
		return false;
	}
}

struct Document::Data {
	Value root;
	std::vector<NodeInfo> nodes;
	std::vector<Path> paths;
	std::unordered_map<NodeId, int> byId;

	mutable std::once_flag jsonOnce;
	mutable QByteArray json;
	mutable std::once_flag renderOnce;
	mutable QByteArray renderJson;
};

struct DocumentAccess {
	using Data = Document::Data;

	[[nodiscard]] static const Document::Data &data(
			const Document &document) {
		static const auto empty = Document::Data();
		return document._data ? *document._data : empty;
	}
};

namespace {

using Data = DocumentAccess::Data;

[[nodiscard]] LayerType LayerTypeFrom(int type) {
	switch (type) {
	case 0: return LayerType::Precomp;
	case 1: return LayerType::Solid;
	case 2: return LayerType::Image;
	case 3: return LayerType::Null;
	case 4: return LayerType::Shape;
	case 5: return LayerType::Text;
	}
	return LayerType::Unknown;
}

[[nodiscard]] ShapeType ShapeTypeFrom(QByteArrayView code) {
	struct Entry {
		const char *code;
		ShapeType type;
	};
	static constexpr auto kEntries = std::array{
		Entry{ "gr", ShapeType::Group },
		Entry{ "rc", ShapeType::Rectangle },
		Entry{ "el", ShapeType::Ellipse },
		Entry{ "sr", ShapeType::Star },
		Entry{ "sh", ShapeType::Path },
		Entry{ "fl", ShapeType::Fill },
		Entry{ "st", ShapeType::Stroke },
		Entry{ "gf", ShapeType::GradientFill },
		Entry{ "gs", ShapeType::GradientStroke },
		Entry{ "tr", ShapeType::Transform },
		Entry{ "tm", ShapeType::TrimPaths },
		Entry{ "rp", ShapeType::Repeater },
		Entry{ "mm", ShapeType::MergePaths },
		Entry{ "rd", ShapeType::RoundCorners },
		Entry{ "op", ShapeType::OffsetPath },
		Entry{ "pb", ShapeType::PuckerBloat },
		Entry{ "tw", ShapeType::Twist },
		Entry{ "zz", ShapeType::ZigZag },
	};
	for (const auto &entry : kEntries) {
		if (code == QByteArrayView(entry.code)) {
			return entry.type;
		}
	}
	return ShapeType::Unknown;
}

[[nodiscard]] MatteMode MatteFrom(int value) {
	switch (value) {
	case 1: return MatteMode::Alpha;
	case 2: return MatteMode::AlphaInverted;
	case 3: return MatteMode::Luma;
	case 4: return MatteMode::LumaInverted;
	}
	return MatteMode::None;
}

[[nodiscard]] bool IsKeyframed(const Value &property) {
	const auto &k = property.get("k");
	if (!k.isArray() || k.items().empty()) {
		return false;
	}
	const auto &first = k.items().front();
	return first.isObject() && first.has("t");
}

// Round 4: plain options. The readers follow rlottie's parser (what it
// does with missing and unknown values), the writers follow bodymovin.
[[nodiscard]] MaskMode MaskModeFrom(const QString &mode) {
	if (mode.isEmpty()) {
		return MaskMode::Add;
	}
	switch (mode.at(0).unicode()) {
	case 'n': return MaskMode::None;
	case 'a': return MaskMode::Add;
	case 's': return MaskMode::Subtract;
	case 'i': return MaskMode::Intersect;
	case 'l': return MaskMode::Lighten;
	case 'd': return MaskMode::Darken;
	case 'f': return MaskMode::Difference;
	}
	return MaskMode::None;
}

[[nodiscard]] QString MaskModeCode(MaskMode mode) {
	switch (mode) {
	case MaskMode::None: return u"n"_q;
	case MaskMode::Add: return u"a"_q;
	case MaskMode::Subtract: return u"s"_q;
	case MaskMode::Intersect: return u"i"_q;
	case MaskMode::Lighten: return u"l"_q;
	case MaskMode::Darken: return u"d"_q;
	case MaskMode::Difference: return u"f"_q;
	}
	return u"a"_q;
}

[[nodiscard]] int MatteCode(MatteMode mode) {
	switch (mode) {
	case MatteMode::None: return 0;
	case MatteMode::Alpha: return 1;
	case MatteMode::AlphaInverted: return 2;
	case MatteMode::Luma: return 3;
	case MatteMode::LumaInverted: return 4;
	}
	return 0;
}

[[nodiscard]] LineCap LineCapFrom(const Value &value) {
	if (!value.isNumber()) {
		return LineCap::Butt;
	}
	switch (value.toInt(0)) {
	case 1: return LineCap::Butt;
	case 2: return LineCap::Round;
	}
	return LineCap::Square;
}

[[nodiscard]] LineJoin LineJoinFrom(const Value &value) {
	if (!value.isNumber()) {
		return LineJoin::Miter;
	}
	switch (value.toInt(0)) {
	case 1: return LineJoin::Miter;
	case 2: return LineJoin::Round;
	}
	return LineJoin::Bevel;
}

class Indexer final {
public:
	explicit Indexer(Data &data) : _data(data) {
	}

	[[nodiscard]] std::vector<Path> run();

private:
	[[nodiscard]] int add(NodeInfo &&info, const Path &path, NodeId id);
	void addLayers(const Value &list, const Path &path, int owner, int depth);
	void addLayer(
		const Value &json,
		const Path &path,
		int index,
		int owner,
		int depth);
	void addShapes(
		const Value &list,
		const Path &path,
		int owner,
		NodeId layer,
		int depth);
	void addAssets(int root);
	void resolveLinks();

	Data &_data;
	std::vector<Path> _duplicates;

};

std::vector<Path> Indexer::run() {
	_data.nodes.clear();
	_data.paths.clear();
	_data.byId.clear();
	if (!_data.root.isObject()) {
		return {};
	}
	auto info = NodeInfo();
	info.kind = NodeKind::Composition;
	info.name = _data.root.get("nm").toString();
	const auto root = add(std::move(info), {}, _data.root.id());
	if (root >= 0) {
		addLayers(_data.root.get("layers"), { { "layers" } }, root, 1);
		addAssets(root);
		resolveLinks();
	}
	return std::move(_duplicates);
}

int Indexer::add(NodeInfo &&info, const Path &path, NodeId id) {
	if (!id || _data.byId.contains(id)) {
		_duplicates.push_back(path);
		return -1;
	}
	info.id = id;
	const auto result = int(_data.nodes.size());
	_data.byId.emplace(id, result);
	_data.nodes.push_back(std::move(info));
	_data.paths.push_back(path);
	return result;
}

void Indexer::addLayers(
		const Value &list,
		const Path &path,
		int owner,
		int depth) {
	const auto &items = list.items();
	for (auto i = 0; i != int(items.size()); ++i) {
		if (items[i].isObject()) {
			addLayer(items[i], Append(path, i), i, owner, depth);
		}
	}
}

void Indexer::addLayer(
		const Value &json,
		const Path &path,
		int index,
		int owner,
		int depth) {
	auto info = NodeInfo();
	info.kind = NodeKind::Layer;
	info.parent = _data.nodes[owner].id;
	info.composition = info.parent;
	info.name = json.get("nm").toString();
	info.hidden = json.get("hd").toBool(false);
	info.index = index;
	info.depth = depth;
	info.layerType = LayerTypeFrom(json.get("ty").toInt(-1));
	if (json.get("ind").isNumber()) {
		info.ind = json.get("ind").toInt();
	}
	if (json.get("parent").isNumber()) {
		info.parentInd = json.get("parent").toInt();
	}
	info.refId = json.get("refId").toString();
	info.inPoint = json.get("ip").toDouble(0.);
	info.outPoint = json.get("op").toDouble(0.);
	info.startTime = json.get("st").toDouble(0.);
	info.stretch = json.get("sr").toDouble(1.);
	if (info.stretch == 0.) {
		info.stretch = 1.;
	}
	info.matte = MatteFrom(json.get("tt").toInt(0));
	info.matteSource = (json.get("td").toInt(0) != 0);
	info.is3d = (json.get("ddd").toInt(0) == 1);
	info.hasTimeRemap = json.get("tm").isObject();
	info.masksEnabled = json.get("hasMask").toBool(false);
	if (json.get("tp").isNumber()) {
		info.matteParentInd = json.get("tp").toInt();
	}
	const auto self = add(std::move(info), path, json.id());
	if (self < 0) {
		return;
	}
	const auto id = _data.nodes[self].id;
	_data.nodes[owner].children.push_back(id);

	const auto &masks = json.get("masksProperties").items();
	for (auto i = 0; i != int(masks.size()); ++i) {
		const auto &mask = masks[i];
		if (!mask.isObject()) {
			continue;
		}
		auto maskInfo = NodeInfo();
		maskInfo.kind = NodeKind::Mask;
		maskInfo.parent = id;
		maskInfo.layer = id;
		maskInfo.name = mask.get("nm").toString();
		maskInfo.index = i;
		maskInfo.depth = depth + 1;
		maskInfo.maskMode = MaskModeFrom(mask.get("mode").toString());
		maskInfo.maskInverted = mask.get("inv").toBool(false);
		const auto maskPath = Append(Append(path, "masksProperties"), i);
		if (add(std::move(maskInfo), maskPath, mask.id()) >= 0) {
			_data.nodes[self].masks.push_back(mask.id());
		}
	}
	const auto &effects = json.get("ef").items();
	for (auto i = 0; i != int(effects.size()); ++i) {
		const auto &effect = effects[i];
		if (!effect.isObject()) {
			continue;
		}
		auto effectInfo = NodeInfo();
		effectInfo.kind = NodeKind::Effect;
		effectInfo.parent = id;
		effectInfo.layer = id;
		effectInfo.name = effect.get("nm").toString();
		effectInfo.hidden = !effect.get("en").toBool(true);
		effectInfo.index = i;
		effectInfo.depth = depth + 1;
		const auto effectPath = Append(Append(path, "ef"), i);
		if (add(std::move(effectInfo), effectPath, effect.id()) >= 0) {
			_data.nodes[self].effects.push_back(effect.id());
		}
	}
	addShapes(json.get("shapes"), Append(path, "shapes"), self, id, depth + 1);
}

void Indexer::addShapes(
		const Value &list,
		const Path &path,
		int owner,
		NodeId layer,
		int depth) {
	const auto &items = list.items();
	for (auto i = 0; i != int(items.size()); ++i) {
		const auto &item = items[i];
		if (!item.isObject()) {
			continue;
		}
		auto info = NodeInfo();
		info.kind = NodeKind::Shape;
		info.parent = _data.nodes[owner].id;
		info.layer = layer;
		info.name = item.get("nm").toString();
		info.hidden = item.get("hd").toBool(false);
		info.index = i;
		info.depth = depth;
		info.typeCode = item.get("ty").toString().toLatin1();
		info.shapeType = ShapeTypeFrom(info.typeCode);
		const auto type = info.shapeType;
		switch (type) {
		case ShapeType::GradientFill:
		case ShapeType::GradientStroke:
			info.gradientType = (item.get("t").toInt(1) == 1)
				? GradientType::Linear
				: GradientType::Radial;
			break;
		case ShapeType::TrimPaths:
			info.trimMode = (item.get("m").toInt(1) == 2)
				? TrimMode::Individually
				: TrimMode::Simultaneously;
			break;
		default: break;
		}
		if (type == ShapeType::Stroke || type == ShapeType::GradientStroke) {
			info.lineCap = LineCapFrom(item.get("lc"));
			info.lineJoin = LineJoinFrom(item.get("lj"));
			info.miterLimit = item.get("ml").toDouble(0.);
			info.dashValues = item.get("d").isArray()
				? item.get("d").size()
				: 0;
		} else if (type == ShapeType::Fill || type == ShapeType::GradientFill) {
			info.fillRule = (item.get("r").toInt(1) == 2)
				? FillRule::EvenOdd
				: FillRule::NonZero;
		}
		const auto itemPath = Append(path, i);
		const auto self = add(std::move(info), itemPath, item.id());
		if (self < 0) {
			continue;
		}
		_data.nodes[owner].children.push_back(item.id());
		auto &parent = _data.nodes[owner];
		if (type == ShapeType::Transform
			&& parent.kind == NodeKind::Shape
			&& parent.shapeType == ShapeType::Group
			&& !parent.transform) {
			parent.transform = item.id();
		}
		if (type == ShapeType::Group) {
			addShapes(
				item.get("it"),
				Append(itemPath, "it"),
				self,
				layer,
				depth + 1);
		}
	}
}

void Indexer::addAssets(int root) {
	const auto &items = _data.root.get("assets").items();
	for (auto i = 0; i != int(items.size()); ++i) {
		const auto &item = items[i];
		if (!item.isObject()) {
			continue;
		}
		auto info = NodeInfo();
		info.kind = NodeKind::Asset;
		info.parent = _data.nodes[root].id;
		info.refId = item.get("id").toString();
		info.name = item.get("nm").toString();
		if (info.name.isEmpty()) {
			info.name = info.refId;
		}
		info.index = i;
		info.depth = 1;
		info.imageAsset = !item.has("layers")
			&& (item.has("p") || item.has("u"));
		const auto path = Path{ { "assets" }, { QByteArray(), i } };
		const auto self = add(std::move(info), path, item.id());
		if (self < 0) {
			continue;
		}
		_data.nodes[root].assets.push_back(item.id());
		addLayers(item.get("layers"), Append(path, "layers"), self, 2);
	}
}

void Indexer::resolveLinks() {
	auto assets = QHash<QString, NodeId>();
	auto indices = std::map<std::pair<NodeId, int>, NodeId>();
	for (const auto &node : _data.nodes) {
		if (node.kind == NodeKind::Asset && !node.refId.isEmpty()) {
			if (!assets.contains(node.refId)) {
				assets.insert(node.refId, node.id);
			}
		} else if (node.kind == NodeKind::Layer && node.ind) {
			indices.emplace(std::make_pair(node.composition, *node.ind), node.id);
		}
	}
	for (auto &node : _data.nodes) {
		if (node.kind != NodeKind::Layer) {
			continue;
		}
		if (node.parentInd) {
			const auto i = indices.find(
				std::make_pair(node.composition, *node.parentInd));
			if (i != end(indices) && i->second != node.id) {
				// rlottie's isGoodParentLayer(): layers get their parents
				// in array order, a link that would close a cycle over the
				// links accepted so far is dropped.
				auto good = false;
				auto current = i->second;
				for (auto guard = 0; guard != 4096; ++guard) {
					if (current == node.id) {
						break;
					}
					const auto up = _data.nodes[
						_data.byId.find(current)->second].parentLayer;
					if (!up) {
						good = true;
						break;
					}
					current = up;
				}
				if (good) {
					node.parentLayer = i->second;
				}
			}
		}
		if (node.layerType == LayerType::Precomp && !node.refId.isEmpty()) {
			node.precomp = assets.value(node.refId);
		}
	}

	// Track mattes the way rlottie pairs them: a layer with "tt" takes the
	// layer right above it in the array, unless that one has "tt" too.
	for (auto &owner : _data.nodes) {
		if (owner.kind != NodeKind::Composition
			&& owner.kind != NodeKind::Asset) {
			continue;
		}
		auto above = -1;
		for (const auto id : owner.children) {
			const auto current = _data.byId.find(id)->second;
			auto &layer = _data.nodes[current];
			if (layer.kind != NodeKind::Layer) {
				continue;
			}
			if (layer.matte != MatteMode::None && above >= 0) {
				auto &source = _data.nodes[above];
				if (source.matte == MatteMode::None
					&& source.index + 1 == layer.index) {
					layer.matteLayer = source.id;
					source.matteTarget = layer.id;
				}
			}
			above = current;
		}
	}
}

[[nodiscard]] const NodeInfo *FindNode(const Data &data, NodeId id) {
	const auto i = data.byId.find(id);
	return (i != end(data.byId)) ? &data.nodes[i->second] : nullptr;
}

[[nodiscard]] const Path *FindPath(const Data &data, NodeId id) {
	const auto i = data.byId.find(id);
	return (i != end(data.byId)) ? &data.paths[i->second] : nullptr;
}

[[nodiscard]] const Value &NodeJson(const Data &data, NodeId id) {
	const auto path = FindPath(data, id);
	return path ? GetIn(data.root, *path) : GetIn(data.root, { { "?" } });
}

[[nodiscard]] QPointF ReadPoint(const Value &value) {
	return QPointF(value.at(0).toDouble(0.), value.at(1).toDouble(0.));
}

[[nodiscard]] std::vector<QPointF> ReadPoints(const Value &list) {
	auto result = std::vector<QPointF>();
	result.reserve(list.size());
	for (const auto &item : list.items()) {
		result.push_back(ReadPoint(item));
	}
	return result;
}

[[nodiscard]] PathData DecodePath(const Value &value) {
	const auto &object = value.isArray() ? value.at(0) : value;
	auto result = PathData();
	result.vertices = ReadPoints(object.get("v"));
	result.inTangents = ReadPoints(object.get("i"));
	result.outTangents = ReadPoints(object.get("o"));
	result.closed = object.get("c").toBool(false);
	return result;
}

[[nodiscard]] PropValue DecodeValue(const Value &value, PropertyType type) {
	if (type == PropertyType::Path) {
		return PropValue::Path(DecodePath(value));
	}
	return PropValue{ value.numbers() };
}

[[nodiscard]] Value EncodePoints(const std::vector<QPointF> &points) {
	auto items = std::vector<Value>();
	items.reserve(points.size());
	for (const auto &point : points) {
		items.push_back(Numbers({
			RoundValue(point.x()),
			RoundValue(point.y()),
		}));
	}
	return Value::FromArray(std::move(items));
}

[[nodiscard]] Value EncodePath(const PathData &path, const Value &previous) {
	const auto &base = previous.isArray() ? previous.at(0) : previous;
	auto result = base.isObject() ? base : Object({});
	return result
		.with("i", EncodePoints(path.inTangents))
		.with("o", EncodePoints(path.outTangents))
		.with("v", EncodePoints(path.vertices))
		.with("c", Value::FromBool(path.closed));
}

[[nodiscard]] std::vector<double> MergeNumbers(
		const std::vector<double> &numbers,
		const Value &previous) {
	auto result = std::vector<double>();
	result.reserve(numbers.size());
	for (const auto number : numbers) {
		result.push_back(RoundValue(number));
	}
	const auto old = previous.numbers();
	for (auto i = result.size(); i < old.size(); ++i) {
		result.push_back(old[i]);
	}
	return result;
}

[[nodiscard]] Value EncodeKeyframeValue(
		const PropValue &value,
		const Value &previous,
		PropertyType type) {
	if (type == PropertyType::Path) {
		return Value::FromArray({
			EncodePath(value.path.value_or(PathData()), previous),
		});
	}
	return Numbers(MergeNumbers(value.numbers, previous));
}

[[nodiscard]] Value EncodeStaticValue(
		const PropValue &value,
		const Value &previous,
		PropertyType type) {
	if (type == PropertyType::Path) {
		return EncodePath(value.path.value_or(PathData()), previous);
	}
	const auto numbers = MergeNumbers(value.numbers, previous);
	if (type == PropertyType::Scalar
		&& numbers.size() == 1
		&& !previous.isArray()) {
		return Number(numbers.front());
	}
	return Numbers(numbers);
}

[[nodiscard]] QColor ParseHexColor(const QString &text) {
	const auto color = QColor(text.trimmed());
	return color.isValid() ? color : QColor();
}

[[nodiscard]] QString HexColor(const QColor &color) {
	return color.name(QColor::HexRgb);
}

struct PropertySpec {
	QByteArray path;
	PropertyRole role = PropertyRole::Other;
	PropertyType type = PropertyType::Scalar;
	QString name;
	bool dashOffset = false; // See PropertyInfo::dashOffset.
};

struct DashEntry {
	int index = 0; // In "d".
	char kind = 'd'; // 'd' dash, 'g' gap, 'o' offset.
	Value json;
};

// By the "n" names like lottie-web. If a name is missing, by position the
// way rlottie does it: only the items that have a "v" count, the last of
// them is the offset, the ones before it are dash, gap, dash, gap...
[[nodiscard]] std::vector<DashEntry> ReadDashEntries(const Value &list) {
	auto result = std::vector<DashEntry>();
	const auto &items = list.items();
	const auto kindOf = [](const Value &item) {
		const auto name = item.get("n").toString();
		return (name == u"d"_q)
			? 'd'
			: (name == u"g"_q)
			? 'g'
			: (name == u"o"_q)
			? 'o'
			: char(0);
	};
	auto named = true;
	for (auto i = 0; i != int(items.size()); ++i) {
		if (!items[i].get("v").isObject()) {
			continue;
		}
		const auto kind = kindOf(items[i]);
		named = named && (kind != 0);
		result.push_back({ i, kind, items[i] });
	}
	if (!named) {
		const auto count = int(result.size());
		for (auto i = 0; i != count; ++i) {
			result[i].kind = (i + 1 == count) ? 'o' : (i % 2) ? 'g' : 'd';
		}
	}
	return result;
}

[[nodiscard]] PropertyType InferType(const Value &property) {
	auto sample = &property.get("k");
	if (IsKeyframed(property)) {
		sample = &sample->at(0).get("s");
	}
	if (sample->isObject()
		|| (sample->isArray()
			&& sample->size() > 0
			&& sample->at(0).isObject())) {
		return PropertyType::Path;
	} else if (sample->isNumber()
		|| (sample->isArray() && sample->size() <= 1)) {
		return PropertyType::Scalar;
	}
	return PropertyType::Vector;
}

void AppendTransformSpecs(
		std::vector<PropertySpec> &out,
		const Value &transform,
		const QByteArray &prefix,
		bool repeater) {
	const auto add = [&](
			const char *key,
			PropertyRole role,
			PropertyType type) {
		if (transform.get(key).isObject()) {
			out.push_back({ prefix + key, role, type });
		}
	};
	add("a", PropertyRole::Anchor, PropertyType::Vector);
	const auto &position = transform.get("p");
	if (position.isObject()) {
		if (position.get("s").toBool(false)
			&& (position.get("x").isObject()
				|| position.get("y").isObject())) {
			const auto part = [&](const char *key, PropertyRole role) {
				if (position.get(key).isObject()) {
					out.push_back({
						prefix + "p." + key,
						role,
						PropertyType::Scalar,
					});
				}
			};
			part("x", PropertyRole::PositionX);
			part("y", PropertyRole::PositionY);
			part("z", PropertyRole::PositionZ);
		} else {
			out.push_back({
				prefix + "p",
				PropertyRole::Position,
				PropertyType::Vector,
			});
		}
	}
	add("s", PropertyRole::Scale, PropertyType::Vector);
	add("r", PropertyRole::Rotation, PropertyType::Scalar);
	add("rz", PropertyRole::Rotation, PropertyType::Scalar);
	add("rx", PropertyRole::RotationX, PropertyType::Scalar);
	add("ry", PropertyRole::RotationY, PropertyType::Scalar);
	add("or", PropertyRole::Other, PropertyType::Vector);
	add("o", PropertyRole::Opacity, PropertyType::Scalar);
	add("sk", PropertyRole::Skew, PropertyType::Scalar);
	add("sa", PropertyRole::SkewAxis, PropertyType::Scalar);
	if (repeater) {
		add("so", PropertyRole::StartOpacity, PropertyType::Scalar);
		add("eo", PropertyRole::EndOpacity, PropertyType::Scalar);
	}
}

[[nodiscard]] std::vector<PropertySpec> SpecsFor(
		const NodeInfo &node,
		const Value &json) {
	auto result = std::vector<PropertySpec>();
	const auto add = [&](
			const char *key,
			PropertyRole role,
			PropertyType type) {
		const auto path = QByteArray(key);
		const auto relative = RelativePath(json, path);
		if (relative && GetIn(json, *relative).isObject()) {
			result.push_back({ path, role, type });
		}
	};
	const auto addList = [&](
			const char *list,
			PropertyRole role,
			Fn<PropertyType(const Value&)> type) {
		const auto &items = json.get(list).items();
		for (auto i = 0; i != int(items.size()); ++i) {
			const auto &item = items[i];
			if (!item.get("v").isObject()) {
				continue;
			}
			auto name = item.get("nm").toString();
			if (name.isEmpty()) {
				name = item.get("n").toString();
			}
			result.push_back({
				QByteArray(list) + '.' + QByteArray::number(i) + ".v",
				role,
				type(item),
				name,
			});
		}
	};
	const auto addDashes = [&] {
		const auto &list = json.get("d");
		const auto from = result.size();
		addList("d", PropertyRole::Dash, [](const Value &) {
			return PropertyType::Scalar;
		});
		if (result.size() == from) {
			return;
		}
		// Items without a name are told apart by their position.
		const auto entries = ReadDashEntries(list);
		const auto count = std::min(result.size() - from, entries.size());
		for (auto i = size_t(0); i != count; ++i) {
			auto &name = result[from + i].name;
			if (name.isEmpty()) {
				name = QString(QChar::fromLatin1(entries[i].kind));
			}
		}
		// Whatever the names say, the renderer reads the last item as the
		// offset.
		result.back().dashOffset = true;
	};
	switch (node.kind) {
	case NodeKind::Composition:
	case NodeKind::Asset:
		break;
	case NodeKind::Layer:
		AppendTransformSpecs(result, json.get("ks"), "ks.", false);
		add("tm", PropertyRole::TimeRemap, PropertyType::Scalar);
		if (json.get("sc").isString()) {
			result.push_back({
				"sc",
				PropertyRole::Color,
				PropertyType::Color,
			});
		}
		break;
	case NodeKind::Mask:
		add("pt", PropertyRole::MaskPath, PropertyType::Path);
		add("o", PropertyRole::MaskOpacity, PropertyType::Scalar);
		add("x", PropertyRole::MaskExpansion, PropertyType::Scalar);
		if (json.get("f").get("k").isArray()
			|| json.get("f").get("k").isNumber()) {
			add("f", PropertyRole::MaskFeather, PropertyType::Vector);
		}
		break;
	case NodeKind::Effect:
		addList("ef", PropertyRole::EffectValue, [](const Value &item) {
			switch (item.get("ty").toInt(-1)) {
			case 2: return PropertyType::Color;
			case 3: return PropertyType::Vector;
			}
			return InferType(item.get("v"));
		});
		break;
	case NodeKind::Shape:
		switch (node.shapeType) {
		case ShapeType::Group:
			break;
		case ShapeType::Rectangle:
			add("p", PropertyRole::Position, PropertyType::Vector);
			add("s", PropertyRole::Size, PropertyType::Vector);
			add("r", PropertyRole::Roundness, PropertyType::Scalar);
			break;
		case ShapeType::Ellipse:
			add("p", PropertyRole::Position, PropertyType::Vector);
			add("s", PropertyRole::Size, PropertyType::Vector);
			break;
		case ShapeType::Star:
			add("p", PropertyRole::Position, PropertyType::Vector);
			add("pt", PropertyRole::Points, PropertyType::Scalar);
			add("r", PropertyRole::Rotation, PropertyType::Scalar);
			add("ir", PropertyRole::InnerRadius, PropertyType::Scalar);
			add("is", PropertyRole::InnerRoundness, PropertyType::Scalar);
			add("or", PropertyRole::OuterRadius, PropertyType::Scalar);
			add("os", PropertyRole::OuterRoundness, PropertyType::Scalar);
			break;
		case ShapeType::Path:
			add("ks", PropertyRole::Path, PropertyType::Path);
			break;
		case ShapeType::Fill:
			add("c", PropertyRole::Color, PropertyType::Color);
			add("o", PropertyRole::Opacity, PropertyType::Scalar);
			break;
		case ShapeType::Stroke:
			add("c", PropertyRole::Color, PropertyType::Color);
			add("o", PropertyRole::Opacity, PropertyType::Scalar);
			add("w", PropertyRole::StrokeWidth, PropertyType::Scalar);
			addDashes();
			break;
		case ShapeType::GradientFill:
		case ShapeType::GradientStroke:
			add("o", PropertyRole::Opacity, PropertyType::Scalar);
			add("s", PropertyRole::StartPoint, PropertyType::Vector);
			add("e", PropertyRole::EndPoint, PropertyType::Vector);
			add("g.k", PropertyRole::Gradient, PropertyType::Gradient);
			add("h", PropertyRole::HighlightLength, PropertyType::Scalar);
			add("a", PropertyRole::HighlightAngle, PropertyType::Scalar);
			if (node.shapeType == ShapeType::GradientStroke) {
				add("w", PropertyRole::StrokeWidth, PropertyType::Scalar);
				addDashes();
			}
			break;
		case ShapeType::TrimPaths:
			add("s", PropertyRole::TrimStart, PropertyType::Scalar);
			add("e", PropertyRole::TrimEnd, PropertyType::Scalar);
			add("o", PropertyRole::TrimOffset, PropertyType::Scalar);
			break;
		case ShapeType::Repeater:
			add("c", PropertyRole::Copies, PropertyType::Scalar);
			add("o", PropertyRole::Offset, PropertyType::Scalar);
			AppendTransformSpecs(result, json.get("tr"), "tr.", true);
			break;
		case ShapeType::RoundCorners:
			add("r", PropertyRole::Radius, PropertyType::Scalar);
			break;
		case ShapeType::Transform:
			AppendTransformSpecs(result, json, QByteArray(), false);
			break;
		default:
			for (const auto &member : json.members()) {
				if (member.value.isObject() && member.value.has("k")) {
					result.push_back({
						member.key,
						(member.key == "a")
							? PropertyRole::Amount
							: PropertyRole::Other,
						InferType(member.value),
					});
				}
			}
			break;
		}
		break;
	}
	return result;
}

struct Resolved {
	const NodeInfo *node = nullptr;
	Path path;
	Value json;
	PropertySpec spec;
	int colorStops = 0;
	bool solid = false;
	bool missing = false; // Not in the file yet, see KnownMissing().
	PropValue fallback;
};

[[nodiscard]] std::optional<PropertySpec> FindSpec(
		const NodeInfo &node,
		const Value &nodeJson,
		QByteArrayView path) {
	for (auto &spec : SpecsFor(node, nodeJson)) {
		if (spec.path == path) {
			return std::move(spec);
		}
	}
	const auto relative = RelativePath(nodeJson, path);
	if (!relative) {
		return std::nullopt;
	}
	const auto &json = GetIn(nodeJson, *relative);
	if (!json.isObject() || !json.has("k")) {
		return std::nullopt;
	}
	return PropertySpec{
		path.toByteArray(),
		PropertyRole::Other,
		InferType(json),
	};
}

// A property the renderer has a default for while it is not in the file:
// it can be read (Document::defaultValue()) and is created on a write.
struct MissingProperty {
	PropertySpec spec;
	PropValue value;
};

[[nodiscard]] std::optional<MissingProperty> KnownMissing(
		const NodeInfo &node,
		QByteArrayView path) {
	struct Entry {
		const char *key;
		PropertyRole role;
		PropertyType type;
		double first;
		double second;
	};
	const auto find = [&](
			QByteArrayView key,
			const Entry *entries,
			size_t count) -> std::optional<MissingProperty> {
		for (auto i = size_t(0); i != count; ++i) {
			const auto &entry = entries[i];
			if (key != QByteArrayView(entry.key)) {
				continue;
			}
			auto value = PropValue::Scalar(entry.first);
			if (entry.type == PropertyType::Vector) {
				value.numbers.push_back(entry.second);
			}
			return MissingProperty{
				PropertySpec{ path.toByteArray(), entry.role, entry.type },
				std::move(value),
			};
		}
		return std::nullopt;
	};
	const auto transformKey = [&](QByteArrayView key, bool repeater) {
		static constexpr auto kEntries = std::array{
			Entry{ "a", PropertyRole::Anchor, PropertyType::Vector, 0., 0. },
			Entry{ "p", PropertyRole::Position, PropertyType::Vector, 0., 0. },
			Entry{ "s", PropertyRole::Scale, PropertyType::Vector, 100., 100. },
			Entry{ "r", PropertyRole::Rotation, PropertyType::Scalar, 0., 0. },
			Entry{ "o", PropertyRole::Opacity, PropertyType::Scalar, 100., 0. },
			Entry{ "sk", PropertyRole::Skew, PropertyType::Scalar, 0., 0. },
			Entry{ "sa", PropertyRole::SkewAxis, PropertyType::Scalar, 0., 0. },
		};
		static constexpr auto kRepeater = std::array{
			Entry{ "a", PropertyRole::Anchor, PropertyType::Vector, 0., 0. },
			Entry{ "p", PropertyRole::Position, PropertyType::Vector, 0., 0. },
			Entry{ "s", PropertyRole::Scale, PropertyType::Vector, 100., 100. },
			Entry{ "r", PropertyRole::Rotation, PropertyType::Scalar, 0., 0. },
			Entry{
				"so",
				PropertyRole::StartOpacity,
				PropertyType::Scalar,
				100.,
				0.,
			},
			Entry{
				"eo",
				PropertyRole::EndOpacity,
				PropertyType::Scalar,
				100.,
				0.,
			},
		};
		return repeater
			? find(key, kRepeater.data(), kRepeater.size())
			: find(key, kEntries.data(), kEntries.size());
	};
	if (node.kind == NodeKind::Layer) {
		return path.startsWith("ks.")
			? transformKey(path.mid(3), false)
			: std::nullopt;
	} else if (node.kind == NodeKind::Mask) {
		static constexpr auto kEntries = std::array{
			Entry{
				"o",
				PropertyRole::MaskOpacity,
				PropertyType::Scalar,
				100.,
				0.,
			},
			Entry{
				"x",
				PropertyRole::MaskExpansion,
				PropertyType::Scalar,
				0.,
				0.,
			},
			Entry{
				"f",
				PropertyRole::MaskFeather,
				PropertyType::Vector,
				0.,
				0.,
			},
		};
		return find(path, kEntries.data(), kEntries.size());
	} else if (node.kind != NodeKind::Shape) {
		return std::nullopt;
	}
	switch (node.shapeType) {
	case ShapeType::Transform:
		return transformKey(path, false);
	case ShapeType::Repeater: {
		static constexpr auto kEntries = std::array{
			Entry{ "c", PropertyRole::Copies, PropertyType::Scalar, 0., 0. },
			Entry{ "o", PropertyRole::Offset, PropertyType::Scalar, 0., 0. },
		};
		return path.startsWith("tr.")
			? transformKey(path.mid(3), true)
			: find(path, kEntries.data(), kEntries.size());
	}
	case ShapeType::GradientFill:
	case ShapeType::GradientStroke: {
		static constexpr auto kEntries = std::array{
			Entry{ "o", PropertyRole::Opacity, PropertyType::Scalar, 100., 0. },
			Entry{
				"h",
				PropertyRole::HighlightLength,
				PropertyType::Scalar,
				0.,
				0.,
			},
			Entry{
				"a",
				PropertyRole::HighlightAngle,
				PropertyType::Scalar,
				0.,
				0.,
			},
			Entry{
				"w",
				PropertyRole::StrokeWidth,
				PropertyType::Scalar,
				0.,
				0.,
			},
		};
		const auto count = (node.shapeType == ShapeType::GradientStroke)
			? kEntries.size()
			: (kEntries.size() - 1);
		return find(path, kEntries.data(), count);
	}
	case ShapeType::Fill:
	case ShapeType::Stroke: {
		static constexpr auto kEntries = std::array{
			Entry{ "o", PropertyRole::Opacity, PropertyType::Scalar, 100., 0. },
			Entry{
				"w",
				PropertyRole::StrokeWidth,
				PropertyType::Scalar,
				0.,
				0.,
			},
		};
		const auto count = (node.shapeType == ShapeType::Stroke)
			? kEntries.size()
			: (kEntries.size() - 1);
		return find(path, kEntries.data(), count);
	}
	case ShapeType::Rectangle: {
		static constexpr auto kEntries = std::array{
			Entry{ "r", PropertyRole::Roundness, PropertyType::Scalar, 0., 0. },
			Entry{ "p", PropertyRole::Position, PropertyType::Vector, 0., 0. },
		};
		return find(path, kEntries.data(), kEntries.size());
	}
	case ShapeType::Ellipse: {
		static constexpr auto kEntries = std::array{
			Entry{ "p", PropertyRole::Position, PropertyType::Vector, 0., 0. },
		};
		return find(path, kEntries.data(), kEntries.size());
	}
	case ShapeType::RoundCorners: {
		static constexpr auto kEntries = std::array{
			Entry{ "r", PropertyRole::Radius, PropertyType::Scalar, 0., 0. },
		};
		return find(path, kEntries.data(), kEntries.size());
	}
	case ShapeType::TrimPaths: {
		static constexpr auto kEntries = std::array{
			Entry{ "s", PropertyRole::TrimStart, PropertyType::Scalar, 0., 0. },
			Entry{ "e", PropertyRole::TrimEnd, PropertyType::Scalar, 0., 0. },
			Entry{
				"o",
				PropertyRole::TrimOffset,
				PropertyType::Scalar,
				0.,
				0.,
			},
		};
		return find(path, kEntries.data(), kEntries.size());
	}
	default: break;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<Resolved> Resolve(
		const Data &data,
		const PropertyRef &ref,
		bool allowMissing = false) {
	const auto node = FindNode(data, ref.node);
	const auto nodePath = FindPath(data, ref.node);
	if (!node || !nodePath || ref.path.isEmpty()) {
		return std::nullopt;
	}
	const auto &nodeJson = GetIn(data.root, *nodePath);
	const auto relative = RelativePath(nodeJson, ref.path);
	if (!relative) {
		return std::nullopt;
	}
	auto result = Resolved();
	result.node = node;
	result.path = Concat(*nodePath, *relative);
	result.json = GetIn(nodeJson, *relative);
	if (node->kind == NodeKind::Layer && ref.path == "sc") {
		if (!result.json.isString()) {
			return std::nullopt;
		}
		result.solid = true;
		result.spec = { "sc", PropertyRole::Color, PropertyType::Color };
		return result;
	}
	auto spec = result.json.isObject()
		? FindSpec(*node, nodeJson, ref.path)
		: std::nullopt;
	if (!spec && allowMissing && result.json.isNull()) {
		const auto parentPath = Path(relative->begin(), relative->end() - 1);
		if (GetIn(nodeJson, parentPath).isObject()) {
			if (auto known = KnownMissing(*node, ref.path)) {
				spec = std::move(known->spec);
				result.missing = true;
				result.fallback = std::move(known->value);
			}
		}
	}
	if (!spec) {
		return std::nullopt;
	}
	result.spec = std::move(*spec);
	if (result.spec.type == PropertyType::Gradient) {
		result.colorStops = nodeJson.get("g").get("p").toInt(0);
	}
	return result;
}

struct RawKeyframe {
	Value json;
	double time = 0.;
	std::optional<PropValue> start;
	std::optional<PropValue> end;
	bool hold = false;
	bool hasIn = false;
	QPointF out;
	QPointF in;
	std::vector<double> outTangent;
	std::vector<double> inTangent;
	bool spatial = false;
};

[[nodiscard]] QPointF ReadControlPoint(const Value &value) {
	return QPointF(
		value.get("x").toNumber(0.),
		value.get("y").toNumber(0.));
}

[[nodiscard]] std::vector<RawKeyframe> ReadRawKeyframes(
		const Value &property,
		PropertyType type) {
	auto result = std::vector<RawKeyframe>();
	if (!IsKeyframed(property)) {
		return result;
	}
	for (const auto &item : property.get("k").items()) {
		if (!item.isObject()) {
			continue;
		}
		auto keyframe = RawKeyframe();
		keyframe.json = item;
		keyframe.time = item.get("t").toNumber(0.);
		if (item.has("s")) {
			keyframe.start = DecodeValue(item.get("s"), type);
		}
		if (item.has("e")) {
			keyframe.end = DecodeValue(item.get("e"), type);
		}
		keyframe.hold = (item.get("h").toNumber(0.) != 0.);
		keyframe.hasIn = item.has("i");
		keyframe.in = ReadControlPoint(item.get("i"));
		keyframe.out = ReadControlPoint(item.get("o"));
		if (item.has("ti") || item.has("to")) {
			keyframe.spatial = true;
			keyframe.inTangent = item.get("ti").numbers();
			keyframe.outTangent = item.get("to").numbers();
		}
		result.push_back(std::move(keyframe));
	}
	return result;
}

[[nodiscard]] PropValue Lerp(
		const PropValue &a,
		const PropValue &b,
		double t) {
	if (a.path) {
		if (!b.path) {
			return a;
		}
		auto result = PathData();
		result.closed = a.path->closed;
		const auto lerp = [&](
				const std::vector<QPointF> &x,
				const std::vector<QPointF> &y) {
			auto list = std::vector<QPointF>();
			const auto count = std::min(x.size(), y.size());
			list.reserve(count);
			for (auto i = size_t(0); i != count; ++i) {
				list.push_back(x[i] + t * (y[i] - x[i]));
			}
			return list;
		};
		result.vertices = lerp(a.path->vertices, b.path->vertices);
		result.inTangents = lerp(a.path->inTangents, b.path->inTangents);
		result.outTangents = lerp(a.path->outTangents, b.path->outTangents);
		return PropValue::Path(std::move(result));
	}
	auto result = a;
	const auto count = std::min(a.numbers.size(), b.numbers.size());
	for (auto i = size_t(0); i != count; ++i) {
		result.numbers[i] = a.numbers[i] + t * (b.numbers[i] - a.numbers[i]);
	}
	return result;
}

[[nodiscard]] QPointF BezierPoint(
		QPointF p0,
		QPointF p1,
		QPointF p2,
		QPointF p3,
		double t) {
	const auto u = 1. - t;
	return u * u * u * p0
		+ 3. * u * u * t * p1
		+ 3. * u * t * t * p2
		+ t * t * t * p3;
}

[[nodiscard]] QPointF SpatialPoint(
		QPointF from,
		QPointF control1,
		QPointF control2,
		QPointF to,
		double progress) {
	constexpr auto kSteps = 64;
	auto lengths = std::array<double, kSteps + 1>();
	auto previous = from;
	lengths[0] = 0.;
	for (auto i = 1; i <= kSteps; ++i) {
		const auto point = BezierPoint(
			from,
			control1,
			control2,
			to,
			double(i) / kSteps);
		const auto delta = point - previous;
		lengths[i] = lengths[i - 1] + std::hypot(delta.x(), delta.y());
		previous = point;
	}
	const auto total = lengths[kSteps];
	if (total <= 0.) {
		return from;
	}
	const auto target = std::clamp(progress, 0., 1.) * total;
	const auto i = std::upper_bound(
		lengths.begin(),
		lengths.end(),
		target) - lengths.begin();
	const auto index = std::clamp(int(i), 1, kSteps);
	const auto a = lengths[index - 1];
	const auto b = lengths[index];
	const auto local = (b > a) ? ((target - a) / (b - a)) : 0.;
	return BezierPoint(
		from,
		control1,
		control2,
		to,
		(double(index - 1) + local) / kSteps);
}

struct Segment {
	double startTime = 0.;
	double endTime = 0.;
	bool endSet = false;
	PropValue start;
	PropValue end;
	bool endKnown = false;
	bool interpolated = false;
	Easing easing;
	bool spatial = false;
	QPointF outTangent;
	QPointF inTangent;
};

// Mirrors rlottie's LottieParserImpl::parseKeyFrame(): every keyframe
// closes the previous segment (its "t" is the end time, its "s" the end
// value unless it has an explicit "e"), hold keyframes and keyframes
// with "i" open a new segment, a keyframe without both is the last one.
[[nodiscard]] std::vector<Segment> BuildSegments(
		const std::vector<RawKeyframe> &list) {
	auto result = std::vector<Segment>();
	for (const auto &keyframe : list) {
		if (!result.empty()) {
			auto &back = result.back();
			back.endTime = keyframe.time;
			back.endSet = true;
			if (keyframe.start && !keyframe.end) {
				back.end = *keyframe.start;
				back.endKnown = true;
			}
		}
		auto segment = Segment();
		segment.startTime = keyframe.time;
		segment.start = keyframe.start
			? *keyframe.start
			: !result.empty()
			? result.back().end
			: PropValue();
		if (keyframe.end) {
			segment.end = *keyframe.end;
			segment.endKnown = true;
		}
		if (keyframe.hold) {
			segment.end = segment.start;
			segment.endKnown = true;
			result.push_back(std::move(segment));
		} else if (keyframe.hasIn) {
			segment.interpolated = true;
			segment.easing = Easing{ keyframe.out, keyframe.in };
			if (keyframe.spatial) {
				segment.spatial = true;
				segment.outTangent = QPointF(
					(keyframe.outTangent.size() > 0)
						? keyframe.outTangent[0]
						: 0.,
					(keyframe.outTangent.size() > 1)
						? keyframe.outTangent[1]
						: 0.);
				segment.inTangent = QPointF(
					(keyframe.inTangent.size() > 0)
						? keyframe.inTangent[0]
						: 0.,
					(keyframe.inTangent.size() > 1)
						? keyframe.inTangent[1]
						: 0.);
			}
			result.push_back(std::move(segment));
		}
	}
	return result;
}

[[nodiscard]] PropValue SegmentValue(
		const Segment &segment,
		PropertyType type,
		double frame) {
	if (!segment.interpolated || segment.endTime <= segment.startTime) {
		return segment.start;
	}
	const auto end = segment.endKnown ? segment.end : segment.start;
	const auto linear = (frame - segment.startTime)
		/ (segment.endTime - segment.startTime);
	const auto progress = segment.easing.apply(linear);
	if (segment.spatial
		&& type == PropertyType::Vector
		&& segment.start.numbers.size() >= 2
		&& end.numbers.size() >= 2) {
		const auto from = segment.start.point();
		const auto to = end.point();
		const auto point = SpatialPoint(
			from,
			from + segment.outTangent,
			to + segment.inTangent,
			to,
			progress);
		auto result = Lerp(segment.start, end, progress);
		result.numbers[0] = point.x();
		result.numbers[1] = point.y();
		return result;
	}
	return Lerp(segment.start, end, progress);
}

[[nodiscard]] PropValue EvaluateKeyframes(
		const std::vector<RawKeyframe> &list,
		PropertyType type,
		double frame) {
	const auto segments = BuildSegments(list);
	if (segments.empty()) {
		for (const auto &keyframe : list) {
			if (keyframe.start) {
				return *keyframe.start;
			}
		}
		return PropValue();
	}
	const auto &front = segments.front();
	if (front.startTime >= frame) {
		return front.start;
	}
	const auto &back = segments.back();
	const auto backEnd = back.endSet ? back.endTime : back.startTime;
	if (backEnd <= frame) {
		return back.endKnown ? back.end : back.start;
	}
	for (const auto &segment : segments) {
		if (frame >= segment.startTime && frame < segment.endTime) {
			return SegmentValue(segment, type, frame);
		}
	}
	return front.start;
}

struct Model {
	double time = 0.;
	PropValue value;
	Easing easing;
	std::vector<double> outTangent;
	std::vector<double> inTangent;
	Value raw;
	bool touched = false;
};

struct ModelList {
	std::vector<Model> list;
	bool oldStyle = false;
	bool spatial = false;
	bool easeKnown = false;
	bool easeArrays = true;
	int easeLength = 1;
};

[[nodiscard]] ModelList ReadModels(
		const Value &property,
		PropertyType type) {
	auto result = ModelList();
	const auto raw = ReadRawKeyframes(property, type);
	const auto count = int(raw.size());
	auto previous = std::optional<PropValue>();
	auto previousEnd = std::optional<PropValue>();
	for (auto i = 0; i != count; ++i) {
		const auto &keyframe = raw[i];
		auto model = Model();
		model.time = keyframe.time;
		model.value = keyframe.start
			? *keyframe.start
			: previousEnd
			? *previousEnd
			: previous
			? *previous
			: PropValue();
		const auto autoHold = (count == 1) && keyframe.hasIn;
		model.easing = (keyframe.hold && !autoHold)
			? Easing::Hold()
			: keyframe.hasIn
			? Easing{ keyframe.out, keyframe.in }
			: Easing::Linear();
		model.outTangent = keyframe.outTangent;
		model.inTangent = keyframe.inTangent;
		model.raw = keyframe.json;
		if (keyframe.end) {
			result.oldStyle = true;
		}
		if (keyframe.spatial) {
			result.spatial = true;
		}
		if (!result.easeKnown && keyframe.json.get("o").isObject()) {
			const auto &x = keyframe.json.get("o").get("x");
			result.easeKnown = true;
			result.easeArrays = x.isArray();
			result.easeLength = std::max(1, x.size());
		}
		previous = model.value;
		previousEnd = keyframe.end;
		result.list.push_back(std::move(model));
	}
	return result;
}

[[nodiscard]] Value EncodeControlPoint(
		QPointF point,
		bool arrays,
		int length) {
	const auto x = RoundValue(point.x());
	const auto y = RoundValue(point.y());
	if (!arrays) {
		return Object({ { "x", Number(x) }, { "y", Number(y) } });
	}
	return Object({
		{ "x", Numbers(std::vector<double>(length, x)) },
		{ "y", Numbers(std::vector<double>(length, y)) },
	});
}

[[nodiscard]] bool KeyframeConstraintsOk(
		const Value &object,
		bool last,
		bool single,
		bool userHold) {
	const auto hold = (object.get("h").toNumber(0.) != 0.);
	if (single) {
		return hold;
	} else if (last) {
		return !object.has("i") && !hold;
	} else if (userHold) {
		return hold;
	}
	return object.has("i") && !hold;
}

[[nodiscard]] int DefaultEaseLength(
		PropertyType type,
		const PropValue &value,
		bool spatial) {
	return (type == PropertyType::Vector && !spatial)
		? std::clamp(int(value.numbers.size()), 1, 3)
		: 1;
}

[[nodiscard]] Value WriteModels(
		const Value &property,
		ModelList models,
		PropertyType type) {
	auto &list = models.list;
	std::stable_sort(begin(list), end(list), [](
			const Model &a,
			const Model &b) {
		return a.time < b.time;
	});
	const auto count = int(list.size());
	if (!count) {
		return property;
	}
	const auto arrays = models.easeKnown ? models.easeArrays : true;
	const auto length = models.easeKnown
		? models.easeLength
		: DefaultEaseLength(type, list.front().value, models.spatial);
	auto items = std::vector<Value>();
	items.reserve(count);
	for (auto i = 0; i != count; ++i) {
		const auto &model = list[i];
		const auto last = (i + 1 == count);
		const auto single = (count == 1);
		const auto userHold = model.easing.hold;
		auto object = model.raw.isObject() ? model.raw : Value();
		const auto fresh = !object.isObject();
		if (!fresh
			&& !model.touched
			&& !models.oldStyle
			&& KeyframeConstraintsOk(object, last, single, userHold)) {
			items.push_back(object);
			continue;
		}
		if (fresh) {
			object = Object({});
		}
		const auto wantsEasing = single ? !userHold : (!last && !userHold);
		if (wantsEasing) {
			const auto same = object.has("i")
				&& object.has("o")
				&& Near(ReadControlPoint(object.get("o")), model.easing.out)
				&& Near(ReadControlPoint(object.get("i")), model.easing.in);
			if (!same) {
				object = object
					.with("i", EncodeControlPoint(
						model.easing.in,
						arrays,
						length))
					.with("o", EncodeControlPoint(
						model.easing.out,
						arrays,
						length))
					.without("n");
			}
		} else {
			object = object.without("i").without("o").without("n");
		}
		if (!object.has("t") || object.get("t").toNumber() != model.time) {
			object = object.with("t", Number(RoundTime(model.time)));
		}
		const auto &start = object.get("s");
		if (!object.has("s") || !(DecodeValue(start, type) == model.value)) {
			object = object.with(
				"s",
				EncodeKeyframeValue(model.value, start, type));
		}
		object = object.without("e");
		if (single || (!last && userHold)) {
			object = object.with("h", Number(1));
		} else {
			object = object.without("h");
		}
		if (models.spatial && !last) {
			const auto dimensions = std::clamp(
				int(model.value.numbers.size()),
				2,
				3);
			const auto tangent = [&](const std::vector<double> &value) {
				return value.empty()
					? std::vector<double>(dimensions, 0.)
					: value;
			};
			const auto to = tangent(model.outTangent);
			const auto ti = tangent(model.inTangent);
			if (!(object.get("to").numbers() == to)) {
				object = object.with("to", Numbers(to));
			}
			if (!(object.get("ti").numbers() == ti)) {
				object = object.with("ti", Numbers(ti));
			}
		} else {
			// The last keyframe never has them, a property without
			// a motion path has them nowhere (see SetMotionPath()).
			object = object.without("to").without("ti");
		}
		items.push_back(std::move(object));
	}
	auto result = property.isObject() ? property : Object({});
	if (!result.has("a")) {
		result = result.withInserted("a", Number(1), 0);
	} else if (result.get("a").toInt(0) != 1) {
		result = result.with("a", Number(1));
	}
	return result.with("k", Value::FromArray(std::move(items)));
}

[[nodiscard]] int FindModel(const ModelList &models, double time) {
	for (auto i = 0; i != int(models.list.size()); ++i) {
		if (Near(models.list[i].time, time, kSameTime)) {
			return i;
		}
	}
	return -1;
}

[[nodiscard]] Easing EasingForInsert(const ModelList &models, double time) {
	auto previous = -1;
	for (auto i = 0; i != int(models.list.size()); ++i) {
		if (models.list[i].time < time) {
			previous = i;
		}
	}
	if (previous >= 0 && previous + 1 < int(models.list.size())) {
		return models.list[previous].easing;
	}
	return Easing::Linear();
}

[[nodiscard]] PropValue MergeValue(
		const PropValue &value,
		const PropValue &previous) {
	if (value.path) {
		return value;
	}
	auto result = value;
	for (auto &number : result.numbers) {
		number = RoundValue(number);
	}
	for (auto i = result.numbers.size(); i < previous.numbers.size(); ++i) {
		result.numbers.push_back(previous.numbers[i]);
	}
	return result;
}

// Renderer safety on the writing side, see RenderSafeJson(): for these
// properties a value outside of the keyframe values hangs rlottie, so
// their easing may not overshoot and their values stay in range.
[[nodiscard]] bool StrictEasing(const Resolved &resolved, bool spatial) {
	if (spatial) {
		return true;
	}
	switch (resolved.spec.role) {
	case PropertyRole::Dash:
		return !resolved.spec.dashOffset;
	case PropertyRole::Copies:
	case PropertyRole::Points:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] EasingLimits LimitsFor(bool strict) {
	return strict ? EasingLimits{ 0., 1. } : EasingLimits();
}

[[nodiscard]] Easing SafeEasing(Easing easing, bool strict) {
	if (easing.hold) {
		return easing;
	}
	const auto limits = LimitsFor(strict);
	const auto clamp = [&](QPointF point) {
		return QPointF(
			std::clamp(point.x(), 0., 1.),
			std::clamp(point.y(), limits.minY, limits.maxY));
	};
	easing.out = clamp(easing.out);
	easing.in = clamp(easing.in);
	return easing;
}

[[nodiscard]] PropValue SafeValue(const Resolved &resolved, PropValue value) {
	auto min = -std::numeric_limits<double>::infinity();
	auto max = std::numeric_limits<double>::infinity();
	switch (resolved.spec.role) {
	case PropertyRole::Dash:
		if (resolved.spec.dashOffset) {
			// Any offset is fine: the renderer wraps it around the pattern.
			for (auto &number : value.numbers) {
				if (!std::isfinite(number)) {
					number = 0.;
				}
			}
			return value;
		}
		min = 0.;
		break;
	case PropertyRole::TrimStart:
	case PropertyRole::TrimEnd: min = 0.; max = 100.; break;
	case PropertyRole::Copies: min = 0.; max = kMaxRepeaterCopies; break;
	case PropertyRole::Points: min = 0.; max = kMaxStarPoints; break;
	default: return value;
	}
	for (auto &number : value.numbers) {
		number = std::isfinite(number) ? std::clamp(number, min, max) : min;
	}
	return value;
}

constexpr auto kNoLimit = 1e12;

// Keyframe names ("n") that stand for different easing curves in one
// file, see MixedEasingNames().
using EasingNames = std::unordered_set<std::string>;

[[nodiscard]] Value SafeAnimatable(
	const Value &property,
	bool strict,
	double min = -kNoLimit,
	double max = kNoLimit,
	const EasingNames *mixed = nullptr);
[[nodiscard]] Value SafeDashes(
	const Value &list,
	const EasingNames *mixed = nullptr);
[[nodiscard]] Value SafeShapeItem(
	const Value &item,
	const EasingNames *mixed = nullptr);

class Mutation final {
public:
	explicit Mutation(const Document &document)
	: _document(document)
	, _data(DocumentAccess::data(document))
	, _root(_data.root) {
	}

	[[nodiscard]] const Data &data() const {
		return _data;
	}
	[[nodiscard]] const Value &root() const {
		return _root;
	}
	[[nodiscard]] const NodeInfo *node(NodeId id) const {
		return FindNode(_data, id);
	}
	[[nodiscard]] const Path *path(NodeId id) const {
		return FindPath(_data, id);
	}
	[[nodiscard]] const Value &get(const Path &path) const {
		return GetIn(_root, path);
	}
	[[nodiscard]] const Value &nodeJson(NodeId id) const {
		const auto found = path(id);
		return found ? GetIn(_root, *found) : GetIn(_root, { { "?" } });
	}
	void set(const Path &path, Value value) {
		_root = SetIn(_root, path, std::move(value));
	}
	void setRoot(Value root) {
		_root = std::move(root);
	}
	void setNode(NodeId id, Value value) {
		if (const auto found = path(id)) {
			set(*found, std::move(value));
			changed(id);
		}
	}
	void changed(NodeId id) {
		if (id && !ranges::contains(_changed, id)) {
			_changed.push_back(id);
		}
	}
	void created(NodeId id) {
		_created.push_back(id);
	}
	void removed(NodeId id) {
		_removed.push_back(id);
	}
	void structural() {
		_structural = true;
	}

	[[nodiscard]] Edit finish() {
		if (_root.sameAs(_data.root)) {
			return Edit{ .document = _document };
		}
		auto result = Edit();
		result.document = Document(_root);
		result.changed = std::move(_changed);
		result.created = std::move(_created);
		result.removed = std::move(_removed);
		result.structural = _structural;
		return result;
	}

private:
	const Document &_document;
	const Data &_data;
	Value _root;
	std::vector<NodeId> _changed;
	std::vector<NodeId> _created;
	std::vector<NodeId> _removed;
	bool _structural = false;

};

[[nodiscard]] Edit Failed(const QString &reason) {
	auto result = Edit();
	result.error = reason;
	return result;
}

[[nodiscard]] Edit Unchanged(const Document &document) {
	return Edit{ .document = document };
}

// After a write into a stroke (dashes), a trim, a repeater or a star: the
// values of the item depend on each other (trim start / end on the
// offset, dashes on their sum), so the whole item is checked again.
void KeepShapeSafe(Mutation &mutation, const Resolved &resolved) {
	const auto node = resolved.node;
	if (node->kind != NodeKind::Shape) {
		return;
	}
	switch (node->shapeType) {
	case ShapeType::Stroke:
	case ShapeType::GradientStroke:
	case ShapeType::TrimPaths:
	case ShapeType::Repeater:
	case ShapeType::Star:
		break;
	default:
		return;
	}
	const auto &json = mutation.nodeJson(node->id);
	auto safe = SafeShapeItem(json);
	if (!safe.sameAs(json)) {
		mutation.setNode(node->id, std::move(safe));
	}
}

[[nodiscard]] PropValue StaticOrFirst(const Value &property, PropertyType type) {
	if (IsKeyframed(property)) {
		const auto raw = ReadRawKeyframes(property, type);
		for (const auto &keyframe : raw) {
			if (keyframe.start) {
				return *keyframe.start;
			}
		}
		return PropValue();
	}
	return DecodeValue(property.get("k"), type);
}

[[nodiscard]] PropValue ValueOf(const Resolved &resolved, double frame) {
	if (resolved.solid) {
		return PropValue::Color(ParseHexColor(resolved.json.toString()));
	} else if (resolved.missing) {
		return resolved.fallback;
	} else if (IsKeyframed(resolved.json)) {
		return EvaluateKeyframes(
			ReadRawKeyframes(resolved.json, resolved.spec.type),
			resolved.spec.type,
			frame);
	}
	return DecodeValue(resolved.json.get("k"), resolved.spec.type);
}

void WriteStatic(
		Mutation &mutation,
		const Resolved &resolved,
		const PropValue &value) {
	if (resolved.solid) {
		mutation.set(resolved.path, Value::FromString(HexColor(value.color())));
		mutation.changed(resolved.node->id);
		return;
	}
	const auto &previous = resolved.json.get("k");
	const auto merged = MergeValue(
		SafeValue(resolved, value),
		resolved.missing
			? resolved.fallback
			: DecodeValue(previous, resolved.spec.type));
	auto property = resolved.json.isObject()
		? resolved.json
		: StaticProperty(Value());
	if (property.has("a") && property.get("a").toInt(0) != 0) {
		property = property.with("a", Number(0));
	}
	property = property.with(
		"k",
		EncodeStaticValue(merged, previous, resolved.spec.type));
	mutation.set(resolved.path, property);
	mutation.changed(resolved.node->id);
	KeepShapeSafe(mutation, resolved);
}

void WriteKeyframes(
		Mutation &mutation,
		const Resolved &resolved,
		ModelList models) {
	// An easing that was fine next to the old values may hang the
	// renderer next to the new ones, see MadeRenderSafe().
	mutation.set(resolved.path, SafeAnimatable(
		WriteModels(resolved.json, std::move(models), resolved.spec.type),
		false));
	mutation.changed(resolved.node->id);
	KeepShapeSafe(mutation, resolved);
}

void SetValueAtImpl(
		Mutation &mutation,
		const Resolved &resolved,
		const PropValue &given,
		double frame) {
	if (resolved.solid || !IsKeyframed(resolved.json)) {
		WriteStatic(mutation, resolved, given);
		return;
	}
	const auto value = SafeValue(resolved, given);
	auto models = ReadModels(resolved.json, resolved.spec.type);
	const auto time = RoundTime(frame);
	const auto index = FindModel(models, time);
	if (index >= 0) {
		auto &model = models.list[index];
		const auto merged = MergeValue(value, model.value);
		if (merged == model.value) {
			return;
		}
		model.value = merged;
		model.touched = true;
	} else {
		auto model = Model();
		model.time = time;
		model.value = MergeValue(value, ValueOf(resolved, time));
		model.easing = EasingForInsert(models, time);
		model.touched = true;
		models.list.push_back(std::move(model));
	}
	WriteKeyframes(mutation, resolved, std::move(models));
}

struct KeyframeGroup {
	PropertyRef property;
	std::vector<double> times;
};

[[nodiscard]] std::vector<KeyframeGroup> GroupKeyframes(
		const std::vector<KeyframeRef> &keyframes) {
	auto result = std::vector<KeyframeGroup>();
	for (const auto &keyframe : keyframes) {
		auto i = ranges::find(
			result,
			keyframe.property,
			&KeyframeGroup::property);
		if (i == end(result)) {
			result.push_back({ keyframe.property });
			i = end(result) - 1;
		}
		i->times.push_back(keyframe.time);
	}
	return result;
}

[[nodiscard]] bool ContainsTime(const std::vector<double> &times, double time) {
	return ranges::any_of(times, [&](double value) {
		return Near(value, time, kSameTime);
	});
}

[[nodiscard]] std::vector<NodeId> TopLevelTargets(
		const Data &data,
		const std::vector<NodeId> &ids) {
	auto result = std::vector<NodeId>();
	const auto selected = std::unordered_set<NodeId>(begin(ids), end(ids));
	for (const auto id : ids) {
		const auto node = FindNode(data, id);
		if (!node || ranges::contains(result, id)) {
			continue;
		}
		auto covered = false;
		for (auto parent = node->parent; parent;) {
			if (selected.contains(parent)) {
				covered = true;
				break;
			}
			const auto up = FindNode(data, parent);
			parent = up ? up->parent : 0;
		}
		if (!covered) {
			result.push_back(id);
		}
	}
	return result;
}

struct Unit {
	int from = 0;
	int till = 0;
};

[[nodiscard]] bool HasMatte(const Value &layer) {
	return (layer.get("tt").toInt(0) != 0);
}

[[nodiscard]] std::vector<Unit> LayerUnits(const std::vector<Value> &layers) {
	auto result = std::vector<Unit>();
	for (auto i = 0; i != int(layers.size()); ++i) {
		if (i > 0 && HasMatte(layers[i]) && !result.empty()) {
			result.back().till = i + 1;
		} else {
			result.push_back({ i, i + 1 });
		}
	}
	return result;
}

[[nodiscard]] int UnitOf(const std::vector<Unit> &units, int index) {
	for (auto i = 0; i != int(units.size()); ++i) {
		if (index >= units[i].from && index < units[i].till) {
			return i;
		}
	}
	return -1;
}

[[nodiscard]] Value WithoutParentOf(const Value &layer) {
	return layer.without("parent");
}

[[nodiscard]] Path ContainerPathOf(const Path &path) {
	return Path(path.begin(), path.end() - 1);
}

void AdjustAfterRemoval(Path &target, const Path &array, int removed) {
	if (target.size() <= array.size()) {
		return;
	}
	for (auto i = size_t(0); i != array.size(); ++i) {
		if (target[i].key != array[i].key
			|| target[i].index != array[i].index) {
			return;
		}
	}
	auto &step = target[array.size()];
	if (step.index > removed) {
		--step.index;
	}
}

[[nodiscard]] Value MakeTransform(QPointF position, bool layer) {
	if (layer) {
		return Object({
			{ "o", StaticProperty(Number(100)) },
			{ "r", StaticProperty(Number(0)) },
			{ "p", StaticProperty(Numbers({ position.x(), position.y(), 0 })) },
			{ "a", StaticProperty(Numbers({ 0, 0, 0 })) },
			{ "s", StaticProperty(Numbers({ 100, 100, 100 })) },
		});
	}
	return Object({
		{ "ty", Value::FromString(u"tr"_q) },
		{ "p", StaticProperty(Numbers({ position.x(), position.y() })) },
		{ "a", StaticProperty(Numbers({ 0, 0 })) },
		{ "s", StaticProperty(Numbers({ 100, 100 })) },
		{ "r", StaticProperty(Number(0)) },
		{ "o", StaticProperty(Number(100)) },
		{ "sk", StaticProperty(Number(0)) },
		{ "sa", StaticProperty(Number(0)) },
		{ "nm", Value::FromString(u"Transform"_q) },
	});
}

[[nodiscard]] Value ColorNumbers(const QColor &color) {
	return Numbers({
		RoundValue(color.redF()),
		RoundValue(color.greenF()),
		RoundValue(color.blueF()),
		1.,
	});
}

[[nodiscard]] Value MakeShapeItem(
		ShapeTemplate type,
		const QString &name,
		const QColor &color,
		QSizeF canvas) {
	const auto side = std::max(
		8.,
		std::round(std::min(canvas.width(), canvas.height()) / 2.));
	const auto named = [&](Value item) {
		return name.isEmpty()
			? item
			: item.with("nm", Value::FromString(name));
	};
	const auto fill = [&] {
		return Object({
			{ "ty", Value::FromString(u"fl"_q) },
			{ "c", StaticProperty(ColorNumbers(color)) },
			{ "o", StaticProperty(Number(100)) },
			{ "r", Number(1) },
			{ "nm", Value::FromString(u"Fill"_q) },
		});
	};
	const auto group = [&](std::vector<Value> items) {
		items.push_back(MakeTransform(QPointF(), false));
		return Object({
			{ "ty", Value::FromString(u"gr"_q) },
			{ "it", Value::FromArray(std::move(items)) },
			{ "nm", Value::FromString(name) },
		});
	};
	switch (type) {
	case ShapeTemplate::Group:
		return group({});
	case ShapeTemplate::Rectangle:
		return group({
			Object({
				{ "ty", Value::FromString(u"rc"_q) },
				{ "d", Number(1) },
				{ "s", StaticProperty(Numbers({ side, side })) },
				{ "p", StaticProperty(Numbers({ 0, 0 })) },
				{ "r", StaticProperty(Number(0)) },
				{ "nm", Value::FromString(u"Rectangle"_q) },
			}),
			fill(),
		});
	case ShapeTemplate::Ellipse:
		return group({
			Object({
				{ "ty", Value::FromString(u"el"_q) },
				{ "d", Number(1) },
				{ "s", StaticProperty(Numbers({ side, side })) },
				{ "p", StaticProperty(Numbers({ 0, 0 })) },
				{ "nm", Value::FromString(u"Ellipse"_q) },
			}),
			fill(),
		});
	case ShapeTemplate::Star:
		return group({
			Object({
				{ "ty", Value::FromString(u"sr"_q) },
				{ "sy", Number(1) },
				{ "d", Number(1) },
				{ "pt", StaticProperty(Number(5)) },
				{ "p", StaticProperty(Numbers({ 0, 0 })) },
				{ "r", StaticProperty(Number(0)) },
				{ "ir", StaticProperty(Number(RoundValue(side * 0.2))) },
				{ "is", StaticProperty(Number(0)) },
				{ "or", StaticProperty(Number(RoundValue(side * 0.5))) },
				{ "os", StaticProperty(Number(0)) },
				{ "nm", Value::FromString(u"Star"_q) },
			}),
			fill(),
		});
	case ShapeTemplate::Fill:
		return named(fill());
	case ShapeTemplate::Stroke:
		return named(Object({
			{ "ty", Value::FromString(u"st"_q) },
			{ "c", StaticProperty(ColorNumbers(color)) },
			{ "o", StaticProperty(Number(100)) },
			{ "w", StaticProperty(Number(4)) },
			{ "lc", Number(2) },
			{ "lj", Number(2) },
			{ "ml", Number(4) },
			{ "nm", Value::FromString(u"Stroke"_q) },
		}));
	case ShapeTemplate::GradientFill: {
		const auto dark = color.darker(160);
		return named(Object({
			{ "ty", Value::FromString(u"gf"_q) },
			{ "o", StaticProperty(Number(100)) },
			{ "r", Number(1) },
			{ "g", Object({
				{ "p", Number(2) },
				{ "k", StaticProperty(Numbers({
					0,
					RoundValue(color.redF()),
					RoundValue(color.greenF()),
					RoundValue(color.blueF()),
					1,
					RoundValue(dark.redF()),
					RoundValue(dark.greenF()),
					RoundValue(dark.blueF()),
				})) },
			}) },
			{ "s", StaticProperty(Numbers({ 0, -side / 2. })) },
			{ "e", StaticProperty(Numbers({ 0, side / 2. })) },
			{ "t", Number(1) },
			{ "nm", Value::FromString(u"Gradient Fill"_q) },
		}));
	}
	case ShapeTemplate::TrimPaths:
		return named(Object({
			{ "ty", Value::FromString(u"tm"_q) },
			{ "s", StaticProperty(Number(0)) },
			{ "e", StaticProperty(Number(100)) },
			{ "o", StaticProperty(Number(0)) },
			{ "m", Number(1) },
			{ "nm", Value::FromString(u"Trim Paths"_q) },
		}));
	case ShapeTemplate::GradientStroke: {
		const auto dark = color.darker(160);
		return named(Object({
			{ "ty", Value::FromString(u"gs"_q) },
			{ "o", StaticProperty(Number(100)) },
			{ "w", StaticProperty(Number(4)) },
			{ "g", Object({
				{ "p", Number(2) },
				{ "k", StaticProperty(Numbers({
					0,
					RoundValue(color.redF()),
					RoundValue(color.greenF()),
					RoundValue(color.blueF()),
					1,
					RoundValue(dark.redF()),
					RoundValue(dark.greenF()),
					RoundValue(dark.blueF()),
				})) },
			}) },
			{ "s", StaticProperty(Numbers({ -side / 2., 0 })) },
			{ "e", StaticProperty(Numbers({ side / 2., 0 })) },
			{ "t", Number(1) },
			{ "lc", Number(2) },
			{ "lj", Number(2) },
			{ "ml", Number(4) },
			{ "nm", Value::FromString(u"Gradient Stroke"_q) },
		}));
	}
	case ShapeTemplate::Repeater:
		// rlottie repeats every item that stands before the repeater in
		// the list and reads only these keys of its transform.
		return named(Object({
			{ "ty", Value::FromString(u"rp"_q) },
			{ "c", StaticProperty(Number(3)) },
			{ "o", StaticProperty(Number(0)) },
			{ "m", Number(1) },
			{ "tr", Object({
				{ "ty", Value::FromString(u"tr"_q) },
				{ "p", StaticProperty(Numbers({
					std::round(side / 4.),
					0,
				})) },
				{ "a", StaticProperty(Numbers({ 0, 0 })) },
				{ "s", StaticProperty(Numbers({ 100, 100 })) },
				{ "r", StaticProperty(Number(0)) },
				{ "so", StaticProperty(Number(100)) },
				{ "eo", StaticProperty(Number(100)) },
				{ "nm", Value::FromString(u"Transform"_q) },
			}) },
			{ "nm", Value::FromString(u"Repeater"_q) },
		}));
	case ShapeTemplate::RoundCorners:
		return named(Object({
			{ "ty", Value::FromString(u"rd"_q) },
			{ "r", StaticProperty(Number(std::round(side / 10.))) },
			{ "nm", Value::FromString(u"Round Corners"_q) },
		}));
	}
	return group({});
}

[[nodiscard]] bool IsPathLike(ShapeTemplate type) {
	switch (type) {
	case ShapeTemplate::Group:
	case ShapeTemplate::Rectangle:
	case ShapeTemplate::Ellipse:
	case ShapeTemplate::Star:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] int MaxInd(const std::vector<Value> &layers) {
	auto result = 0;
	for (const auto &layer : layers) {
		result = std::max(result, layer.get("ind").toInt(0));
	}
	return result;
}

template <typename Callback>
[[nodiscard]] Value MapStoredValues(
		const Value &property,
		bool keyframed,
		Callback &&callback) {
	if (keyframed) {
		auto items = property.get("k").items();
		auto changed = false;
		for (auto i = 0; i != int(items.size()); ++i) {
			auto keyframe = items[i];
			for (const auto key : { "s", "e" }) {
				if (!keyframe.has(key)) {
					continue;
				}
				const auto &value = keyframe.get(key);
				auto mapped = callback(value, i, (key[0] == 'e'));
				if (!mapped.sameAs(value)) {
					keyframe = keyframe.with(key, std::move(mapped));
				}
			}
			if (!keyframe.sameAs(items[i])) {
				items[i] = std::move(keyframe);
				changed = true;
			}
		}
		return changed
			? property.with("k", Value::FromArray(std::move(items)))
			: property;
	} else if (property.has("k")) {
		const auto &value = property.get("k");
		auto mapped = callback(value, -1, false);
		return mapped.sameAs(value)
			? property
			: property.with("k", std::move(mapped));
	}
	return property;
}

template <typename Callback>
[[nodiscard]] Value MapStoredValues(const Value &property, Callback &&callback) {
	return MapStoredValues(
		property,
		IsKeyframed(property),
		std::forward<Callback>(callback));
}

[[nodiscard]] Value MapNumbers(
		const Value &value,
		const Fn<std::vector<double>(std::vector<double>)> &map) {
	if (!value.isNumber() && !value.isArray()) {
		return value;
	}
	const auto numbers = value.numbers();
	const auto mapped = map(numbers);
	if (mapped == numbers) {
		return value;
	}
	if (value.isNumber() && mapped.size() == 1) {
		return Number(mapped.front());
	}
	return Numbers(mapped);
}

[[nodiscard]] Value MapPointProperty(
		const Value &property,
		double scale,
		QPointF offset) {
	auto result = MapStoredValues(property, [&](
			const Value &value,
			int,
			bool) {
		return MapNumbers(value, [&](std::vector<double> numbers) {
			if (numbers.size() > 0) {
				numbers[0] = RoundValue(numbers[0] * scale + offset.x());
			}
			if (numbers.size() > 1) {
				numbers[1] = RoundValue(numbers[1] * scale + offset.y());
			}
			return numbers;
		});
	});
	if (scale == 1. || !IsKeyframed(result)) {
		return result;
	}
	auto items = result.get("k").items();
	for (auto &keyframe : items) {
		for (const auto key : { "ti", "to" }) {
			if (keyframe.has(key)) {
				keyframe = keyframe.with(key, MapNumbers(
					keyframe.get(key),
					[&](std::vector<double> numbers) {
						for (auto i = size_t(0);
							i < std::min(numbers.size(), size_t(2));
							++i) {
							numbers[i] = RoundValue(numbers[i] * scale);
						}
						return numbers;
					}));
			}
		}
	}
	return result.with("k", Value::FromArray(std::move(items)));
}

[[nodiscard]] Value MapScalarProperty(
		const Value &property,
		const Fn<double(double)> &map) {
	return MapStoredValues(property, [&](const Value &value, int, bool) {
		return MapNumbers(value, [&](std::vector<double> numbers) {
			for (auto &number : numbers) {
				number = map(number);
			}
			return numbers;
		});
	});
}

[[nodiscard]] Value MapTimesDeep(
		const Value &value,
		const Fn<double(double)> &map) {
	if (value.isArray()) {
		auto items = value.items();
		auto changed = false;
		for (auto &item : items) {
			if (!item.isObject() && !item.isArray()) {
				continue;
			}
			auto mapped = MapTimesDeep(item, map);
			if (!mapped.sameAs(item)) {
				item = std::move(mapped);
				changed = true;
			}
		}
		return changed ? Value::FromArray(std::move(items)) : value;
	} else if (!value.isObject()) {
		return value;
	} else if (IsKeyframed(value)) {
		auto items = value.get("k").items();
		auto changed = false;
		for (auto &keyframe : items) {
			if (!keyframe.has("t")) {
				continue;
			}
			const auto time = keyframe.get("t").toDouble(0.);
			const auto mapped = RoundTime(map(time));
			if (mapped != time) {
				keyframe = keyframe.with("t", Number(mapped));
				changed = true;
			}
		}
		return changed
			? value.with("k", Value::FromArray(std::move(items)))
			: value;
	}
	auto result = value;
	for (const auto &member : value.members()) {
		if (!member.value.isObject() && !member.value.isArray()) {
			continue;
		}
		auto mapped = MapTimesDeep(member.value, map);
		if (!mapped.sameAs(member.value)) {
			result = result.with(member.key, std::move(mapped));
		}
	}
	return result;
}

[[nodiscard]] Value MapLayerTimes(
		const Value &layer,
		const Fn<double(double)> &map,
		double remapScale,
		bool shiftStart) {
	auto result = MapTimesDeep(layer, map);
	for (const auto key : { "ip", "op" }) {
		if (result.get(key).isNumber()) {
			const auto value = result.get(key).toDouble();
			const auto mapped = RoundTime(map(value));
			if (mapped != value) {
				result = result.with(key, Number(mapped));
			}
		}
	}
	if (shiftStart && result.get("st").isNumber()) {
		const auto value = result.get("st").toDouble();
		const auto mapped = RoundTime(map(value));
		if (mapped != value) {
			result = result.with("st", Number(mapped));
		}
	}
	if (remapScale != 1. && result.get("tm").isObject()) {
		result = result.with("tm", MapScalarProperty(
			result.get("tm"),
			[&](double value) { return RoundTo(value * remapScale, 6); }));
	}
	return result;
}

// Scales / shifts every frame time of the document. The root composition
// uses t * scale + shift, precomposition assets only scale (their local
// time follows the "st" of the precomp layers, which is shifted too).
[[nodiscard]] Value TransformTimes(
		const Value &root,
		double scale,
		double shift,
		double remapScale) {
	if (scale == 1. && shift == 0. && remapScale == 1.) {
		return root;
	}
	const auto rootMap = Fn<double(double)>([=](double time) {
		return time * scale + shift;
	});
	const auto localMap = Fn<double(double)>([=](double time) {
		return time * scale;
	});
	auto result = root;
	for (const auto key : { "ip", "op" }) {
		if (result.get(key).isNumber()) {
			result = result.with(
				key,
				Number(std::round(rootMap(result.get(key).toDouble()))));
		}
	}
	auto layers = result.get("layers").items();
	for (auto &layer : layers) {
		if (layer.isObject()) {
			layer = MapLayerTimes(layer, rootMap, remapScale, true);
		}
	}
	result = result.with("layers", Value::FromArray(std::move(layers)));
	if (result.get("assets").isArray() && scale != 1.) {
		auto assets = result.get("assets").items();
		for (auto &asset : assets) {
			if (!asset.get("layers").isArray()) {
				continue;
			}
			auto list = asset.get("layers").items();
			for (auto &layer : list) {
				if (layer.isObject()) {
					layer = MapLayerTimes(layer, localMap, remapScale, true);
				}
			}
			asset = asset.with("layers", Value::FromArray(std::move(list)));
		}
		result = result.with("assets", Value::FromArray(std::move(assets)));
	}
	if (result.get("markers").isArray()) {
		auto markers = result.get("markers").items();
		for (auto &marker : markers) {
			if (marker.get("tm").isNumber()) {
				marker = marker.with("tm", Number(RoundTime(
					rootMap(marker.get("tm").toDouble()))));
			}
			if (marker.get("dr").isNumber()) {
				marker = marker.with("dr", Number(RoundTime(
					marker.get("dr").toDouble() * scale)));
			}
		}
		result = result.with("markers", Value::FromArray(std::move(markers)));
	}
	return result;
}

struct Rgb {
	double r = 0.;
	double g = 0.;
	double b = 0.;
};

[[nodiscard]] QRgb Quantize(const Rgb &color) {
	const auto channel = [](double value) {
		return int(std::lround(std::clamp(value, 0., 1.) * 255.));
	};
	return qRgb(channel(color.r), channel(color.g), channel(color.b));
}

[[nodiscard]] Rgb ToRgb(const QColor &color) {
	return { color.redF(), color.greenF(), color.blueF() };
}

[[nodiscard]] QColor ToColor(const Rgb &color) {
	return QColor::fromRgbF(
		float(std::clamp(color.r, 0., 1.)),
		float(std::clamp(color.g, 0., 1.)),
		float(std::clamp(color.b, 0., 1.)));
}

// Same math as Oblivion::Lottie::AdjustColors() (lottie core AdjustRgb).
[[nodiscard]] Rgb AdjustRgb(Rgb color, double hue, double sat, double light) {
	auto r = std::clamp(color.r, 0., 1.);
	auto g = std::clamp(color.g, 0., 1.);
	auto b = std::clamp(color.b, 0., 1.);
	const auto max = std::max({ r, g, b });
	const auto min = std::min({ r, g, b });
	const auto delta = max - min;
	auto lightness = (max + min) / 2.;
	auto saturation = 0.;
	auto h = 0.;
	if (delta > 1e-12) {
		saturation = (lightness > 0.5)
			? (delta / (2. - max - min))
			: (delta / (max + min));
		if (max == r) {
			h = (g - b) / delta + ((g < b) ? 6. : 0.);
		} else if (max == g) {
			h = (b - r) / delta + 2.;
		} else {
			h = (r - g) / delta + 4.;
		}
		h *= 60.;
	}
	h = std::fmod(h + hue, 360.);
	if (h < 0.) {
		h += 360.;
	}
	if (sat > 0. && saturation > 0.) {
		saturation += (1. - saturation) * sat;
	} else if (sat < 0.) {
		saturation *= (1. + sat);
	}
	if (light > 0.) {
		lightness += (1. - lightness) * light;
	} else if (light < 0.) {
		lightness *= (1. + light);
	}
	saturation = std::clamp(saturation, 0., 1.);
	lightness = std::clamp(lightness, 0., 1.);
	if (saturation <= 0.) {
		return { lightness, lightness, lightness };
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
	const auto normalized = h / 360.;
	return {
		std::clamp(channel(normalized + 1. / 3.), 0., 1.),
		std::clamp(channel(normalized), 0., 1.),
		std::clamp(channel(normalized - 1. / 3.), 0., 1.),
	};
}

struct ColorProperty {
	const NodeInfo *node = nullptr;
	QByteArray path;
	ColorKind kind = ColorKind::Fill;
	int stops = 0;
};

[[nodiscard]] std::unordered_set<NodeId> ExpandScope(
		const Data &data,
		const std::vector<NodeId> &scope) {
	auto result = std::unordered_set<NodeId>();
	if (scope.empty()) {
		for (const auto &node : data.nodes) {
			result.insert(node.id);
		}
		return result;
	}
	auto roots = std::unordered_set<NodeId>(begin(scope), end(scope));
	auto grown = true;
	while (grown) {
		grown = false;
		for (const auto &node : data.nodes) {
			if (result.contains(node.id)) {
				continue;
			}
			auto included = roots.contains(node.id);
			for (auto parent = node.parent; !included && parent;) {
				if (roots.contains(parent)) {
					included = true;
				}
				const auto up = FindNode(data, parent);
				parent = up ? up->parent : 0;
			}
			if (included) {
				result.insert(node.id);
				if (node.precomp && !roots.contains(node.precomp)) {
					roots.insert(node.precomp);
					grown = true;
				}
			}
		}
	}
	return result;
}

[[nodiscard]] std::vector<ColorProperty> CollectColorProperties(
		const Data &data,
		const std::vector<NodeId> &scope) {
	const auto included = ExpandScope(data, scope);
	auto result = std::vector<ColorProperty>();
	for (const auto &node : data.nodes) {
		if (!included.contains(node.id)) {
			continue;
		}
		const auto &json = GetIn(data.root, *FindPath(data, node.id));
		switch (node.kind) {
		case NodeKind::Layer:
			if (node.layerType == LayerType::Solid
				&& json.get("sc").isString()) {
				result.push_back({ &node, "sc", ColorKind::Solid });
			} else if (node.layerType == LayerType::Text
				&& json.get("t").get("d").isObject()) {
				result.push_back({ &node, "t.d", ColorKind::TextFill });
				result.push_back({ &node, "t.d", ColorKind::TextStroke });
			}
			break;
		case NodeKind::Effect: {
			const auto &items = json.get("ef").items();
			for (auto i = 0; i != int(items.size()); ++i) {
				if (items[i].get("ty").toInt(-1) == 2
					&& items[i].get("v").isObject()) {
					result.push_back({
						&node,
						"ef." + QByteArray::number(i) + ".v",
						ColorKind::Effect,
					});
				}
			}
		} break;
		case NodeKind::Shape:
			switch (node.shapeType) {
			case ShapeType::Fill:
			case ShapeType::Stroke:
				if (json.get("c").isObject()) {
					result.push_back({
						&node,
						"c",
						(node.shapeType == ShapeType::Fill)
							? ColorKind::Fill
							: ColorKind::Stroke,
					});
				}
				break;
			case ShapeType::GradientFill:
			case ShapeType::GradientStroke:
				if (json.get("g").get("k").isObject()) {
					result.push_back({
						&node,
						"g.k",
						(node.shapeType == ShapeType::GradientFill)
							? ColorKind::GradientFill
							: ColorKind::GradientStroke,
						json.get("g").get("p").toInt(0),
					});
				}
				break;
			default: break;
			}
			break;
		default: break;
		}
	}
	return result;
}

using ColorMap = Fn<std::optional<Rgb>(
	Rgb color,
	int keyframe,
	bool end,
	int stop)>;

[[nodiscard]] Value MapColorNumbers(
		const Value &value,
		int stops,
		int keyframe,
		bool end,
		const ColorMap &map) {
	if (!value.isArray()) {
		return value;
	}
	auto items = value.items();
	auto changed = false;
	const auto apply = [&](size_t base, int stop) {
		if (base + 2 >= items.size()) {
			return;
		}
		const auto color = Rgb{
			items[base].toDouble(0.),
			items[base + 1].toDouble(0.),
			items[base + 2].toDouble(0.),
		};
		const auto mapped = map(color, keyframe, end, stop);
		if (!mapped) {
			return;
		}
		const auto values = std::array{ mapped->r, mapped->g, mapped->b };
		for (auto i = size_t(0); i != 3; ++i) {
			const auto rounded = RoundValue(values[i]);
			if (items[base + i].toDouble(-1.) != rounded) {
				items[base + i] = Number(rounded);
				changed = true;
			}
		}
	};
	if (stops > 0) {
		// "p" comes from the file, only the stops present are mapped.
		const auto count = std::min(size_t(stops), items.size() / 4);
		for (auto stop = size_t(0); stop != count; ++stop) {
			apply(stop * 4 + 1, int(stop));
		}
	} else {
		apply(0, -1);
	}
	return changed ? Value::FromArray(std::move(items)) : value;
}

[[nodiscard]] Value MapColorProperty(
		const Value &json,
		const ColorProperty &property,
		const ColorMap &map) {
	switch (property.kind) {
	case ColorKind::Solid: {
		const auto color = ParseHexColor(json.toString());
		if (!color.isValid()) {
			return json;
		}
		const auto mapped = map(ToRgb(color), -1, false, -1);
		if (!mapped) {
			return json;
		}
		const auto result = ToColor(*mapped);
		return (result.rgb() == color.rgb())
			? json
			: Value::FromString(HexColor(result));
	}
	case ColorKind::TextFill:
	case ColorKind::TextStroke: {
		const auto key = (property.kind == ColorKind::TextFill)
			? QByteArray("fc")
			: QByteArray("sc");
		auto items = json.get("k").items();
		auto changed = false;
		for (auto i = 0; i != int(items.size()); ++i) {
			const auto &document = items[i].get("s");
			const auto &color = document.get(key);
			if (!color.isArray()) {
				continue;
			}
			auto mapped = MapColorNumbers(color, 0, i, false, map);
			if (!mapped.sameAs(color)) {
				items[i] = items[i].with(
					"s",
					document.with(key, std::move(mapped)));
				changed = true;
			}
		}
		return changed
			? json.with("k", Value::FromArray(std::move(items)))
			: json;
	}
	default:
		return MapStoredValues(json, [&](
				const Value &value,
				int keyframe,
				bool end) {
			return MapColorNumbers(value, property.stops, keyframe, end, map);
		});
	}
}

[[nodiscard]] Edit MapColors(
		const Document &document,
		const std::vector<NodeId> &scope,
		const ColorMap &map) {
	auto mutation = Mutation(document);
	const auto &data = mutation.data();
	for (const auto &property : CollectColorProperties(data, scope)) {
		const auto nodePath = FindPath(data, property.node->id);
		const auto &nodeJson = mutation.get(*nodePath);
		const auto relative = RelativePath(nodeJson, property.path);
		if (!relative) {
			continue;
		}
		const auto path = Concat(*nodePath, *relative);
		const auto &json = mutation.get(path);
		auto mapped = MapColorProperty(json, property, map);
		if (!mapped.sameAs(json)) {
			mutation.set(path, std::move(mapped));
			mutation.changed(property.node->id);
		}
	}
	return mutation.finish();
}

void AppendOccurrences(
		std::vector<ColorOccurrence> &out,
		const Value &json,
		const ColorProperty &property) {
	const auto ref = PropertyRef{ property.node->id, property.path };
	const auto map = ColorMap([&](
			Rgb color,
			int keyframe,
			bool end,
			int stop) -> std::optional<Rgb> {
		out.push_back({
			ref,
			property.kind,
			keyframe,
			end,
			stop,
			ToColor(color),
		});
		return std::nullopt;
	});
	[[maybe_unused]] const auto unchanged = MapColorProperty(
		json,
		property,
		map);
}

[[nodiscard]] bool LayerIsTopLevel(const NodeInfo &layer) {
	return !layer.parentLayer;
}

[[nodiscard]] double MapIntoPrecomp(
	const Document &document,
	const NodeInfo &user,
	double frame);

} // namespace

Document::Document() = default;

Document::Document(Value root) {
	auto data = std::make_shared<Data>();
	data->root = std::move(root);
	for (auto attempt = 0; attempt != 4; ++attempt) {
		auto duplicates = Indexer(*data).run();
		if (duplicates.empty()) {
			break;
		}
		for (const auto &path : duplicates) {
			data->root = SetIn(
				data->root,
				path,
				GetIn(data->root, path).withNewIds());
		}
	}
	_data = std::move(data);
}

Document Document::FromJson(QByteArrayView json, QString *error) {
	auto parsed = Json::Parse(json, error);
	if (!parsed) {
		return Document();
	} else if (!parsed->isObject() || !parsed->get("layers").isArray()) {
		if (error) {
			*error = u"not a Lottie animation (no \"layers\")"_q;
		}
		return Document();
	}
	return Document(std::move(*parsed));
}

Document Document::FromData(const QByteArray &data, QString *error) {
	auto json = Oblivion::Lottie::Unpack(data);
	if (json.isEmpty()
		&& data.size() > 2
		&& uchar(data[0]) == 0x1F
		&& uchar(data[1]) == 0x8B) {
		json = GunzipFallback(data);
	}
	if (json.isEmpty()) {
		json = data;
	}
	return FromJson(json, error);
}

Document Document::Blank(QSize size, double fps, int frames) {
	return Document(Object({
		{ "tgs", Number(1) },
		{ "v", Value::FromString(u"5.5.2"_q) },
		{ "fr", Number(fps) },
		{ "ip", Number(0) },
		{ "op", Number(std::max(frames, 1)) },
		{ "w", Number(std::max(size.width(), 1)) },
		{ "h", Number(std::max(size.height(), 1)) },
		{ "nm", Value::FromString(QString()) },
		{ "ddd", Number(0) },
		{ "assets", Value::FromArray() },
		{ "layers", Value::FromArray() },
	}));
}

bool Document::valid() const {
	return _data
		&& _data->root.isObject()
		&& _data->root.get("layers").isArray();
}

const Value &Document::root() const {
	return DocumentAccess::data(*this).root;
}

bool Document::sameAs(const Document &other) const {
	return (_data == other._data)
		|| (_data && other._data && _data->root.sameAs(other._data->root));
}

QByteArray Document::toJson() const {
	if (!valid()) {
		return QByteArray();
	}
	const auto data = _data.get();
	std::call_once(data->jsonOnce, [&] {
		data->json = Json::Serialize(data->root);
	});
	return data->json;
}

QByteArray Document::toTgs() const {
	const auto json = toJson();
	if (json.isEmpty()) {
		return QByteArray();
	}
	auto result = Oblivion::Lottie::PackTgs(json);
	return result.isEmpty() ? GzipFallback(json) : result;
}

QSize Document::size() const {
	return QSize(root().get("w").toInt(0), root().get("h").toInt(0));
}

double Document::frameRate() const {
	return root().get("fr").toDouble(0.);
}

double Document::inPoint() const {
	return root().get("ip").toDouble(0.);
}

double Document::outPoint() const {
	return root().get("op").toDouble(0.);
}

int Document::frames() const {
	const auto from = int64(std::trunc(inPoint()));
	const auto till = int64(std::trunc(outPoint()));
	return int(std::clamp(till - from, int64(0), int64(1 << 24)));
}

QString Document::name() const {
	return root().get("nm").toString();
}

NodeId Document::rootId() const {
	return root().id();
}

const std::vector<NodeInfo> &Document::nodes() const {
	return DocumentAccess::data(*this).nodes;
}

const NodeInfo *Document::node(NodeId id) const {
	return FindNode(DocumentAccess::data(*this), id);
}

bool Document::contains(NodeId id) const {
	return (node(id) != nullptr);
}

const Value &Document::json(NodeId id) const {
	return NodeJson(DocumentAccess::data(*this), id);
}

bool Document::isDescendant(NodeId id, NodeId ancestor) const {
	const auto &data = DocumentAccess::data(*this);
	auto current = FindNode(data, id);
	while (current && current->parent) {
		if (current->parent == ancestor) {
			return true;
		}
		current = FindNode(data, current->parent);
	}
	return false;
}

std::vector<NodeId> Document::layers(NodeId composition) const {
	const auto found = node(composition ? composition : rootId());
	return found ? found->children : std::vector<NodeId>();
}

NodeId Document::owningLayer(NodeId id) const {
	const auto found = node(id);
	if (!found) {
		return 0;
	}
	switch (found->kind) {
	case NodeKind::Layer: return found->id;
	case NodeKind::Shape:
	case NodeKind::Mask:
	case NodeKind::Effect: return found->layer;
	default: return 0;
	}
}

std::vector<NodeId> Document::assetUsers(NodeId asset) const {
	auto result = std::vector<NodeId>();
	for (const auto &node : nodes()) {
		if (node.kind == NodeKind::Layer && node.precomp == asset) {
			result.push_back(node.id);
		}
	}
	return result;
}

double Document::localFrame(NodeId id, double rootFrame) const {
	auto chain = std::vector<NodeId>();
	auto current = node(id);
	if (current && current->kind != NodeKind::Layer) {
		current = node(owningLayer(id));
	}
	for (auto depth = 0; current && depth != kMaxPrecompDepth; ++depth) {
		const auto composition = node(current->composition);
		if (!composition || composition->kind != NodeKind::Asset) {
			break;
		}
		const auto users = assetUsers(composition->id);
		if (users.empty()) {
			break;
		}
		chain.push_back(users.front());
		current = node(users.front());
	}
	auto frame = rootFrame;
	for (auto i = int(chain.size()); i != 0;) {
		frame = MapIntoPrecomp(*this, *node(chain[--i]), frame);
	}
	return frame;
}

std::pair<double, double> Document::layerRangeInRoot(NodeId layer) const {
	const auto found = node(layer);
	if (!found || found->kind != NodeKind::Layer) {
		return { 0., 0. };
	}
	auto from = found->inPoint;
	auto till = found->outPoint;
	auto current = found;
	for (auto depth = 0; current && depth != kMaxPrecompDepth; ++depth) {
		const auto composition = node(current->composition);
		if (!composition || composition->kind != NodeKind::Asset) {
			break;
		}
		const auto users = assetUsers(composition->id);
		if (users.empty()) {
			break;
		}
		const auto user = node(users.front());
		from = std::max(from * user->stretch + user->startTime, user->inPoint);
		till = std::min(till * user->stretch + user->startTime, user->outPoint);
		current = user;
	}
	return { from, std::max(from, till) };
}

std::vector<PropertyInfo> Document::properties(NodeId id) const {
	const auto &data = DocumentAccess::data(*this);
	auto found = FindNode(data, id);
	if (found
		&& found->kind == NodeKind::Shape
		&& found->shapeType == ShapeType::Group) {
		found = FindNode(data, found->transform);
	}
	if (!found) {
		return {};
	}
	auto result = std::vector<PropertyInfo>();
	const auto &json = NodeJson(data, found->id);
	for (const auto &spec : SpecsFor(*found, json)) {
		if (auto info = property({ found->id, spec.path })) {
			result.push_back(std::move(*info));
		}
	}
	return result;
}

std::optional<PropertyInfo> Document::property(const PropertyRef &ref) const {
	const auto resolved = Resolve(DocumentAccess::data(*this), ref);
	if (!resolved) {
		return std::nullopt;
	}
	auto result = PropertyInfo();
	result.ref = ref;
	result.role = resolved->spec.role;
	result.type = resolved->spec.type;
	result.name = resolved->spec.name;
	result.dashOffset = resolved->spec.dashOffset;
	result.colorStops = resolved->colorStops;
	if (resolved->solid) {
		result.dimensions = 3;
		return result;
	}
	const auto &json = resolved->json;
	result.animated = IsKeyframed(json);
	result.expression = json.get("x").isString();
	if (result.animated) {
		const auto &items = json.get("k").items();
		result.keyframes = int(items.size());
		result.spatial = ranges::any_of(items, [](const Value &item) {
			return item.has("ti") || item.has("to");
		});
	}
	const auto base = StaticOrFirst(json, resolved->spec.type);
	result.dimensions = base.path
		? 1
		: std::max(int(base.numbers.size()), 1);
	return result;
}

const Value &Document::propertyJson(const PropertyRef &ref) const {
	const auto &data = DocumentAccess::data(*this);
	const auto path = FindPath(data, ref.node);
	if (!path) {
		return NodeJson(data, 0);
	}
	const auto &nodeJson = GetIn(data.root, *path);
	const auto relative = RelativePath(nodeJson, ref.path);
	return relative ? GetIn(nodeJson, *relative) : NodeJson(data, 0);
}

bool Document::animated(const PropertyRef &ref) const {
	return IsKeyframed(propertyJson(ref));
}

std::vector<Keyframe> Document::keyframes(const PropertyRef &ref) const {
	const auto resolved = Resolve(DocumentAccess::data(*this), ref);
	if (!resolved || resolved->solid) {
		return {};
	}
	const auto models = ReadModels(resolved->json, resolved->spec.type);
	auto result = std::vector<Keyframe>();
	result.reserve(models.list.size());
	for (auto i = 0; i != int(models.list.size()); ++i) {
		const auto &model = models.list[i];
		auto keyframe = Keyframe();
		keyframe.time = model.time;
		keyframe.value = model.value;
		keyframe.easing = model.easing;
		keyframe.last = (i + 1 == int(models.list.size()));
		keyframe.outTangent = model.outTangent;
		keyframe.inTangent = model.inTangent;
		result.push_back(std::move(keyframe));
	}
	return result;
}

std::vector<double> Document::keyframeTimes(const PropertyRef &ref) const {
	auto result = std::vector<double>();
	const auto &json = propertyJson(ref);
	if (IsKeyframed(json)) {
		for (const auto &item : json.get("k").items()) {
			if (item.isObject()) {
				result.push_back(item.get("t").toNumber(0.));
			}
		}
	}
	return result;
}

std::optional<PropValue> Document::valueAt(
		const PropertyRef &ref,
		double frame) const {
	const auto resolved = Resolve(DocumentAccess::data(*this), ref);
	if (!resolved) {
		return std::nullopt;
	}
	return ValueOf(*resolved, frame);
}

std::optional<PropValue> Document::baseValue(const PropertyRef &ref) const {
	const auto resolved = Resolve(DocumentAccess::data(*this), ref);
	if (!resolved) {
		return std::nullopt;
	} else if (resolved->solid) {
		return ValueOf(*resolved, 0.);
	}
	return StaticOrFirst(resolved->json, resolved->spec.type);
}

std::vector<PropValue> Document::valuesAt(
		const PropertyRef &ref,
		const std::vector<double> &frames) const {
	auto result = std::vector<PropValue>();
	const auto resolved = Resolve(DocumentAccess::data(*this), ref, true);
	if (!resolved) {
		return result;
	}
	result.reserve(frames.size());
	if (resolved->solid || resolved->missing || !IsKeyframed(resolved->json)) {
		result.assign(frames.size(), ValueOf(*resolved, 0.));
		return result;
	}
	const auto raw = ReadRawKeyframes(resolved->json, resolved->spec.type);
	for (const auto frame : frames) {
		result.push_back(EvaluateKeyframes(raw, resolved->spec.type, frame));
	}
	return result;
}

std::optional<PropValue> Document::defaultValue(const PropertyRef &ref) const {
	const auto found = node(ref.node);
	if (!found) {
		return std::nullopt;
	}
	auto known = KnownMissing(*found, ref.path);
	return known
		? std::make_optional(std::move(known->value))
		: std::nullopt;
}

std::optional<GradientData> Document::gradientAt(
		NodeId shape,
		double frame) const {
	const auto found = node(shape);
	if (!found
		|| found->kind != NodeKind::Shape
		|| (found->shapeType != ShapeType::GradientFill
			&& found->shapeType != ShapeType::GradientStroke)) {
		return std::nullopt;
	}
	const auto resolved = Resolve(
		DocumentAccess::data(*this),
		{ shape, "g.k" });
	if (!resolved) {
		return std::nullopt;
	}
	const auto &count = json(shape).get("g").get("p");
	return DecodeGradient(
		ValueOf(*resolved, frame),
		count.isNumber() ? count.toInt() : -1);
}

std::vector<PropertyRef> Document::animatedProperties(NodeId id) const {
	auto result = std::vector<PropertyRef>();
	const auto &data = DocumentAccess::data(*this);
	for (const auto &node : data.nodes) {
		if (id
			&& node.id != id
			&& !isDescendant(node.id, id)) {
			continue;
		}
		const auto &json = NodeJson(data, node.id);
		for (const auto &spec : SpecsFor(node, json)) {
			const auto relative = RelativePath(json, spec.path);
			if (relative && IsKeyframed(GetIn(json, *relative))) {
				result.push_back({ node.id, spec.path });
			}
		}
	}
	return result;
}

std::vector<ColorOccurrence> Document::colorOccurrences(
		const std::vector<NodeId> &scope) const {
	const auto &data = DocumentAccess::data(*this);
	auto result = std::vector<ColorOccurrence>();
	for (const auto &property : CollectColorProperties(data, scope)) {
		const auto &nodeJson = NodeJson(data, property.node->id);
		const auto relative = RelativePath(nodeJson, property.path);
		if (relative) {
			AppendOccurrences(result, GetIn(nodeJson, *relative), property);
		}
	}
	return result;
}

std::vector<PaletteEntry> Document::palette(
		const std::vector<NodeId> &scope) const {
	auto result = std::vector<PaletteEntry>();
	auto indices = std::unordered_map<QRgb, int>();
	for (auto &occurrence : colorOccurrences(scope)) {
		const auto key = occurrence.color.rgb();
		const auto i = indices.find(key);
		if (i != end(indices)) {
			result[i->second].occurrences.push_back(std::move(occurrence));
		} else {
			indices.emplace(key, int(result.size()));
			result.push_back({
				QColor::fromRgb(key),
				{ std::move(occurrence) },
			});
		}
	}
	std::stable_sort(begin(result), end(result), [](
			const PaletteEntry &a,
			const PaletteEntry &b) {
		return a.occurrences.size() > b.occurrences.size();
	});
	return result;
}

namespace {

// rlottie's LOTLayerData::timeRemap(): the time of a precomposition's
// content at a frame of the composition that holds the precomp layer.
// A static time remap is ignored, an animated one gives seconds that are
// converted with the root composition's frame rate and length.
[[nodiscard]] double MapIntoPrecomp(
		const Document &document,
		const NodeInfo &user,
		double frame) {
	const auto &remap = document.json(user.id).get("tm");
	if (remap.isObject() && IsKeyframed(remap)) {
		const auto seconds = EvaluateKeyframes(
			ReadRawKeyframes(remap, PropertyType::Scalar),
			PropertyType::Scalar,
			frame).scalar();
		const auto limit = std::max(
			document.outPoint() - document.inPoint() - 1.,
			0.);
		frame = std::clamp(seconds * document.frameRate(), 0., limit);
	} else {
		frame -= user.startTime;
	}
	return frame / user.stretch;
}

[[nodiscard]] PropValue EvalProperty(
		const Value &property,
		PropertyType type,
		double frame) {
	if (IsKeyframed(property)) {
		return EvaluateKeyframes(
			ReadRawKeyframes(property, type),
			type,
			frame);
	}
	return DecodeValue(property.get("k"), type);
}

[[nodiscard]] QPointF EvalPoint(
		const Value &property,
		double frame,
		QPointF fallback = QPointF()) {
	if (!property.isObject()) {
		return fallback;
	}
	const auto value = EvalProperty(property, PropertyType::Vector, frame);
	if (value.numbers.size() >= 2) {
		return value.point();
	} else if (value.numbers.size() == 1) {
		return QPointF(value.numbers[0], value.numbers[0]);
	}
	return fallback;
}

[[nodiscard]] double EvalScalar(
		const Value &property,
		double frame,
		double fallback = 0.) {
	return property.isObject()
		? EvalProperty(property, PropertyType::Scalar, frame).scalar(fallback)
		: fallback;
}

[[nodiscard]] QTransform TransformMatrix(
		const Value &transform,
		double frame,
		bool is3d) {
	if (!transform.isObject()) {
		return QTransform();
	}
	const auto &p = transform.get("p");
	const auto position = (p.get("s").toBool(false)
		&& (p.get("x").isObject() || p.get("y").isObject()))
		? QPointF(
			EvalScalar(p.get("x"), frame),
			EvalScalar(p.get("y"), frame))
		: EvalPoint(p, frame);
	const auto anchor = EvalPoint(transform.get("a"), frame);
	const auto scale = EvalPoint(
		transform.get("s"),
		frame,
		QPointF(100., 100.));
	const auto rotation = EvalScalar(
		transform.get(is3d ? "rz" : "r"),
		frame);
	auto result = QTransform();
	result.translate(position.x(), position.y());
	result.rotate(rotation);
	result.scale(scale.x() / 100., scale.y() / 100.);
	result.translate(-anchor.x(), -anchor.y());
	return result;
}

// rlottie treats a layer without "ddd" as 3D (its rotation is "rz").
[[nodiscard]] QTransform LayerMatrix(const Value &layer, double frame) {
	return TransformMatrix(
		layer.get("ks"),
		frame,
		layer.get("ddd").toInt(1) != 0);
}

[[nodiscard]] QTransform LayerInComposition(
		const Document &document,
		const NodeInfo &layer,
		double frame) {
	auto result = LayerMatrix(document.json(layer.id), frame);
	auto parent = layer.parentLayer;
	for (auto guard = 0; parent && guard != 64; ++guard) {
		const auto node = document.node(parent);
		if (!node) {
			break;
		}
		result = result * LayerMatrix(document.json(parent), frame);
		parent = node->parentLayer;
	}
	return result;
}

[[nodiscard]] QTransform LayerToRoot(
		const Document &document,
		NodeId id,
		double rootFrame) {
	auto result = QTransform();
	auto current = document.node(id);
	for (auto depth = 0; current && depth != kMaxPrecompDepth; ++depth) {
		result = result * LayerInComposition(
			document,
			*current,
			document.localFrame(current->id, rootFrame));
		const auto composition = document.node(current->composition);
		if (!composition || composition->kind != NodeKind::Asset) {
			break;
		}
		const auto users = document.assetUsers(composition->id);
		current = users.empty() ? nullptr : document.node(users.front());
	}
	return result;
}

[[nodiscard]] QTransform ContentToRoot(
		const Document &document,
		NodeId container,
		double rootFrame) {
	auto result = QTransform();
	auto current = document.node(container);
	for (auto guard = 0; current && guard != 256; ++guard) {
		if (current->kind == NodeKind::Layer) {
			return result * LayerToRoot(document, current->id, rootFrame);
		} else if (current->kind != NodeKind::Shape) {
			break;
		}
		if (current->shapeType == ShapeType::Group && current->transform) {
			result = result * TransformMatrix(
				document.json(current->transform),
				document.localFrame(current->id, rootFrame),
				false);
		}
		current = document.node(current->parent);
	}
	return result;
}

void AppendPathData(QPainterPath &out, const PathData &path) {
	const auto count = path.vertices.size();
	if (!count) {
		return;
	}
	const auto at = [](const std::vector<QPointF> &list, size_t index) {
		return (index < list.size()) ? list[index] : QPointF();
	};
	const auto &v = path.vertices;
	out.moveTo(v[0]);
	for (auto i = size_t(1); i != count; ++i) {
		out.cubicTo(
			v[i - 1] + at(path.outTangents, i - 1),
			v[i] + at(path.inTangents, i),
			v[i]);
	}
	if (path.closed) {
		out.cubicTo(
			v[count - 1] + at(path.outTangents, count - 1),
			v[0] + at(path.inTangents, 0),
			v[0]);
		out.closeSubpath();
	}
}

[[nodiscard]] QPainterPath EmptyPath() {
	auto result = QPainterPath();
	result.setFillRule(Qt::WindingFill);
	return result;
}

[[nodiscard]] QPainterPath ItemsGeometry(
	const Document &document,
	const std::vector<NodeId> &items,
	double frame,
	int depth);

[[nodiscard]] QPainterPath ShapeGeometry(
		const Document &document,
		const NodeInfo &node,
		double frame,
		int depth) {
	auto result = EmptyPath();
	if (node.hidden || depth > 64) {
		return result;
	}
	const auto &json = document.json(node.id);
	switch (node.shapeType) {
	case ShapeType::Rectangle: {
		const auto center = EvalPoint(json.get("p"), frame);
		const auto size = EvalPoint(json.get("s"), frame);
		const auto rect = QRectF(
			center - QPointF(size.x() / 2., size.y() / 2.),
			QSizeF(size.x(), size.y())).normalized();
		const auto radius = std::clamp(
			EvalScalar(json.get("r"), frame),
			0.,
			std::min(rect.width(), rect.height()) / 2.);
		result.addRoundedRect(rect, radius, radius);
	} break;
	case ShapeType::Ellipse: {
		const auto center = EvalPoint(json.get("p"), frame);
		const auto size = EvalPoint(json.get("s"), frame);
		result.addEllipse(center, size.x() / 2., size.y() / 2.);
	} break;
	case ShapeType::Path: {
		const auto value = EvalProperty(
			json.get("ks"),
			PropertyType::Path,
			frame);
		if (value.path) {
			AppendPathData(result, *value.path);
		}
	} break;
	case ShapeType::Star: {
		const auto center = EvalPoint(json.get("p"), frame);
		const auto points = std::clamp(
			int(std::round(EvalScalar(json.get("pt"), frame, 5.))),
			3,
			100);
		const auto star = (json.get("sy").toInt(1) == 1);
		const auto outer = EvalScalar(json.get("or"), frame);
		const auto inner = EvalScalar(json.get("ir"), frame);
		const auto start = (EvalScalar(json.get("r"), frame) - 90.)
			* M_PI
			/ 180.;
		const auto steps = star ? (points * 2) : points;
		for (auto i = 0; i != steps; ++i) {
			const auto angle = start + i * 2. * M_PI / steps;
			const auto radius = (star && (i % 2)) ? inner : outer;
			const auto point = center + QPointF(
				radius * std::cos(angle),
				radius * std::sin(angle));
			if (i) {
				result.lineTo(point);
			} else {
				result.moveTo(point);
			}
		}
		result.closeSubpath();
	} break;
	case ShapeType::Group: {
		const auto content = ItemsGeometry(
			document,
			node.children,
			frame,
			depth + 1);
		const auto matrix = node.transform
			? TransformMatrix(document.json(node.transform), frame, false)
			: QTransform();
		result.addPath(matrix.map(content));
	} break;
	default: break;
	}
	return result;
}

// Geometry of a shape list (a layer's "shapes" or a group's "it") with
// the widest visible stroke applied to it, so that line art can be hit.
QPainterPath ItemsGeometry(
		const Document &document,
		const std::vector<NodeId> &items,
		double frame,
		int depth) {
	auto result = EmptyPath();
	auto strokeWidth = 0.;
	for (const auto id : items) {
		const auto item = document.node(id);
		if (!item || item->hidden) {
			continue;
		} else if (item->shapeType == ShapeType::Stroke
			|| item->shapeType == ShapeType::GradientStroke) {
			strokeWidth = std::max(
				strokeWidth,
				EvalScalar(document.json(id).get("w"), frame));
		} else if (item->shapeType != ShapeType::Transform) {
			result.addPath(ShapeGeometry(document, *item, frame, depth));
		}
	}
	if (strokeWidth > 0. && !result.isEmpty()) {
		auto stroker = QPainterPathStroker();
		stroker.setWidth(strokeWidth);
		stroker.setJoinStyle(Qt::RoundJoin);
		stroker.setCapStyle(Qt::RoundCap);
		result.addPath(stroker.createStroke(result));
	}
	return result;
}

[[nodiscard]] QPainterPath LayerContent(
		const Document &document,
		const NodeInfo &layer,
		double frame,
		int depth) {
	auto result = EmptyPath();
	if (depth > kMaxPrecompDepth) {
		return result;
	}
	const auto &json = document.json(layer.id);
	switch (layer.layerType) {
	case LayerType::Shape:
		result.addPath(ItemsGeometry(document, layer.children, frame, 0));
		break;
	case LayerType::Solid:
		result.addRect(QRectF(
			0.,
			0.,
			json.get("sw").toDouble(0.),
			json.get("sh").toDouble(0.)));
		break;
	case LayerType::Image:
		for (const auto &node : document.nodes()) {
			if (node.kind == NodeKind::Asset && node.refId == layer.refId) {
				const auto &asset = document.json(node.id);
				result.addRect(QRectF(
					0.,
					0.,
					asset.get("w").toDouble(0.),
					asset.get("h").toDouble(0.)));
				break;
			}
		}
		break;
	case LayerType::Precomp:
		if (const auto asset = document.node(layer.precomp)) {
			const auto local = MapIntoPrecomp(document, layer, frame);
			for (const auto child : asset->children) {
				const auto item = document.node(child);
				if (!item || item->hidden || item->matteSource) {
					continue;
				} else if (local < item->inPoint || local >= item->outPoint) {
					continue;
				}
				const auto content = LayerContent(
					document,
					*item,
					local,
					depth + 1);
				result.addPath(
					LayerInComposition(document, *item, local).map(content));
			}
		}
		break;
	default: break;
	}
	return result;
}

} // namespace

QTransform Document::transformAt(NodeId id, double rootFrame) const {
	const auto found = node(id);
	if (!found) {
		return QTransform();
	}
	switch (found->kind) {
	case NodeKind::Layer:
		return LayerToRoot(*this, found->id, rootFrame);
	case NodeKind::Mask:
	case NodeKind::Effect:
		return LayerToRoot(*this, found->layer, rootFrame);
	case NodeKind::Shape:
		return ContentToRoot(
			*this,
			(found->shapeType == ShapeType::Group)
				? found->id
				: found->parent,
			rootFrame);
	case NodeKind::Asset: {
		const auto users = assetUsers(found->id);
		return users.empty()
			? QTransform()
			: LayerToRoot(*this, users.front(), rootFrame);
	}
	case NodeKind::Composition:
		break;
	}
	return QTransform();
}

QPainterPath Document::outlineAt(NodeId id, double rootFrame) const {
	const auto found = node(id);
	if (!found) {
		return EmptyPath();
	}
	const auto local = localFrame(id, rootFrame);
	switch (found->kind) {
	case NodeKind::Composition: {
		auto result = EmptyPath();
		result.addRect(QRectF(QPointF(), QSizeF(size())));
		return result;
	}
	case NodeKind::Layer:
		return LayerToRoot(*this, id, rootFrame).map(
			LayerContent(*this, *found, local, 0));
	case NodeKind::Shape:
		return ContentToRoot(*this, found->parent, rootFrame).map(
			ShapeGeometry(*this, *found, local, 0));
	case NodeKind::Mask: {
		auto result = EmptyPath();
		const auto value = EvalProperty(
			json(id).get("pt"),
			PropertyType::Path,
			local);
		if (value.path) {
			AppendPathData(result, *value.path);
		}
		return LayerToRoot(*this, found->layer, rootFrame).map(result);
	}
	default: break;
	}
	return EmptyPath();
}

namespace {

// The usual circle approximation with cubic beziers, the same constant
// lottie-web and rlottie use for rounded corners.
constexpr auto kArcHandle = 0.5519;
constexpr auto kMaxDashPairs = 16;
constexpr auto kMaxGradientStops = 64;

struct Cubic {
	QPointF p0;
	QPointF p1;
	QPointF p2;
	QPointF p3;

	[[nodiscard]] bool straight() const {
		return Near(p0, p1, 1e-9) && Near(p2, p3, 1e-9);
	}
};

[[nodiscard]] QPointF TangentAt(const std::vector<QPointF> &list, int index) {
	return (index >= 0 && index < int(list.size()))
		? list[index]
		: QPointF();
}

[[nodiscard]] Cubic SegmentCubic(const PathData &path, int segment) {
	const auto count = int(path.vertices.size());
	const auto from = segment;
	const auto to = (segment + 1) % count;
	return {
		path.vertices[from],
		path.vertices[from] + TangentAt(path.outTangents, from),
		path.vertices[to] + TangentAt(path.inTangents, to),
		path.vertices[to],
	};
}

[[nodiscard]] QPointF Mix(QPointF a, QPointF b, double t) {
	return a + (b - a) * t;
}

[[nodiscard]] double Distance(QPointF a, QPointF b) {
	return std::hypot(a.x() - b.x(), a.y() - b.y());
}

[[nodiscard]] QPointF RoundPoint(QPointF point) {
	return QPointF(RoundValue(point.x()), RoundValue(point.y()));
}

// corners: which vertices to round (null: every corner vertex), so that
// all keyframes of an animated path get the same new vertices.
[[nodiscard]] PathData RoundedPathWith(
		const PathData &source,
		double radius,
		const std::vector<bool> *corners) {
	const auto path = NormalizedPath(source);
	const auto count = int(path.vertices.size());
	if (radius <= 0. || count < 2) {
		return path;
	}
	auto result = PathData();
	result.closed = path.closed;
	const auto push = [&](QPointF vertex, QPointF in, QPointF out) {
		result.vertices.push_back(RoundPoint(vertex));
		result.inTangents.push_back(RoundPoint(in));
		result.outTangents.push_back(RoundPoint(out));
	};
	for (auto i = 0; i != count; ++i) {
		const auto vertex = path.vertices[i];
		const auto corner = corners
			? ((i < int(corners->size())) && (*corners)[i])
			: IsCornerVertex(path, i);
		const auto end = !path.closed && (i == 0 || i == count - 1);
		if (!corner || end) {
			push(vertex, path.inTangents[i], path.outTangents[i]);
			continue;
		}
		const auto side = [&](QPointF neighbour) {
			const auto distance = Distance(vertex, neighbour);
			const auto part = (distance > 0.)
				? (std::min(distance / 2., radius) / distance)
				: 0.;
			return Mix(vertex, neighbour, part);
		};
		const auto before = side(path.vertices[(i + count - 1) % count]);
		const auto after = side(path.vertices[(i + 1) % count]);
		push(before, QPointF(), (vertex - before) * kArcHandle);
		push(after, (vertex - after) * kArcHandle, QPointF());
	}
	return result;
}

[[nodiscard]] int EffectiveColorStops(const PropValue &gradient, int stops) {
	const auto available = int(gradient.numbers.size() / 4);
	return (stops < 0 || stops > available) ? available : stops;
}

[[nodiscard]] double GradientAlphaAt(
		const std::vector<GradientStop> &alphas,
		double offset) {
	if (alphas.empty()) {
		return 1.;
	}
	auto before = (const GradientStop*)nullptr;
	auto after = (const GradientStop*)nullptr;
	for (const auto &stop : alphas) {
		if (stop.offset <= offset && (!before || stop.offset >= before->offset)) {
			before = &stop;
		}
		if (stop.offset >= offset && (!after || stop.offset < after->offset)) {
			after = &stop;
		}
	}
	if (!before) {
		return after->color.alphaF();
	} else if (!after || after->offset <= before->offset) {
		return before->color.alphaF();
	}
	const auto t = (offset - before->offset) / (after->offset - before->offset);
	return before->color.alphaF()
		+ t * (after->color.alphaF() - before->color.alphaF());
}

} // namespace

GradientData DecodeGradient(const PropValue &gradient, int colorStops) {
	const auto count = EffectiveColorStops(gradient, colorStops);
	auto result = GradientData();
	result.colors = ColorStops(gradient, count);
	result.alphas = OpacityStops(gradient, count);
	return result;
}

PropValue EncodeGradient(const GradientData &data) {
	auto colors = data.colors;
	auto alphas = data.alphas;
	const auto byOffset = [](const GradientStop &a, const GradientStop &b) {
		return a.offset < b.offset;
	};
	std::stable_sort(begin(colors), end(colors), byOffset);
	std::stable_sort(begin(alphas), end(alphas), byOffset);
	if (alphas.size() == 1) {
		auto first = alphas.front();
		auto second = first;
		first.offset = 0.;
		second.offset = 1.;
		alphas = { first, second };
	}
	const auto unit = [](double value) {
		return RoundValue(std::clamp(value, 0., 1.));
	};
	auto result = PropValue();
	result.numbers.reserve(colors.size() * 4 + alphas.size() * 2);
	for (const auto &stop : colors) {
		result.numbers.push_back(unit(stop.offset));
		result.numbers.push_back(unit(stop.color.redF()));
		result.numbers.push_back(unit(stop.color.greenF()));
		result.numbers.push_back(unit(stop.color.blueF()));
	}
	for (const auto &stop : alphas) {
		result.numbers.push_back(unit(stop.offset));
		result.numbers.push_back(unit(stop.color.alphaF()));
	}
	return result;
}

QColor GradientColorAt(const GradientData &data, double offset) {
	auto before = (const GradientStop*)nullptr;
	auto after = (const GradientStop*)nullptr;
	for (const auto &stop : data.colors) {
		if (stop.offset <= offset && (!before || stop.offset >= before->offset)) {
			before = &stop;
		}
		if (stop.offset >= offset && (!after || stop.offset < after->offset)) {
			after = &stop;
		}
	}
	auto result = QColor(255, 255, 255);
	if (before && after && after->offset > before->offset) {
		const auto t = (offset - before->offset)
			/ (after->offset - before->offset);
		const auto mix = [&](float a, float b) {
			return float(std::clamp(a + t * (b - a), 0., 1.));
		};
		result = QColor::fromRgbF(
			mix(before->color.redF(), after->color.redF()),
			mix(before->color.greenF(), after->color.greenF()),
			mix(before->color.blueF(), after->color.blueF()));
	} else if (before || after) {
		const auto &color = (before ? before : after)->color;
		result = QColor::fromRgbF(
			color.redF(),
			color.greenF(),
			color.blueF());
	}
	result.setAlphaF(float(std::clamp(
		GradientAlphaAt(data.alphas, offset),
		0.,
		1.)));
	return result;
}

PathData NormalizedPath(PathData path) {
	path.inTangents.resize(path.vertices.size());
	path.outTangents.resize(path.vertices.size());
	return path;
}

PathData RectanglePath(const QRectF &source, double radius) {
	const auto rect = source.normalized();
	const auto r = std::clamp(
		radius,
		0.,
		std::min(rect.width(), rect.height()) / 2.);
	auto result = PathData();
	result.closed = true;
	const auto push = [&](QPointF vertex, QPointF in, QPointF out) {
		result.vertices.push_back(vertex);
		result.inTangents.push_back(in);
		result.outTangents.push_back(out);
	};
	if (r <= 0.) {
		push(rect.topLeft(), QPointF(), QPointF());
		push(rect.topRight(), QPointF(), QPointF());
		push(rect.bottomRight(), QPointF(), QPointF());
		push(rect.bottomLeft(), QPointF(), QPointF());
		return result;
	}
	const auto h = r * kArcHandle;
	const auto left = rect.left();
	const auto top = rect.top();
	const auto right = rect.right();
	const auto bottom = rect.bottom();
	push(QPointF(left + r, top), QPointF(-h, 0.), QPointF());
	push(QPointF(right - r, top), QPointF(), QPointF(h, 0.));
	push(QPointF(right, top + r), QPointF(0., -h), QPointF());
	push(QPointF(right, bottom - r), QPointF(), QPointF(0., h));
	push(QPointF(right - r, bottom), QPointF(h, 0.), QPointF());
	push(QPointF(left + r, bottom), QPointF(), QPointF(-h, 0.));
	push(QPointF(left, bottom - r), QPointF(0., h), QPointF());
	push(QPointF(left, top + r), QPointF(), QPointF(0., -h));
	return result;
}

PathData EllipsePath(const QRectF &source) {
	const auto rect = source.normalized();
	const auto center = rect.center();
	const auto rx = rect.width() / 2.;
	const auto ry = rect.height() / 2.;
	const auto hx = rx * kArcHandle;
	const auto hy = ry * kArcHandle;
	auto result = PathData();
	result.closed = true;
	result.vertices = {
		QPointF(center.x(), rect.top()),
		QPointF(rect.right(), center.y()),
		QPointF(center.x(), rect.bottom()),
		QPointF(rect.left(), center.y()),
	};
	result.inTangents = {
		QPointF(-hx, 0.),
		QPointF(0., -hy),
		QPointF(hx, 0.),
		QPointF(0., hy),
	};
	result.outTangents = {
		QPointF(hx, 0.),
		QPointF(0., hy),
		QPointF(-hx, 0.),
		QPointF(0., -hy),
	};
	return result;
}

QPainterPath PainterPath(const PathData &path) {
	auto result = EmptyPath();
	AppendPathData(result, path);
	return result;
}

int PathSegmentCount(const PathData &path) {
	const auto count = int(path.vertices.size());
	return (count < 2) ? 0 : path.closed ? count : (count - 1);
}

QPointF PathPointAt(const PathData &path, int segment, double t) {
	if (path.vertices.empty()) {
		return QPointF();
	} else if (segment < 0 || segment >= PathSegmentCount(path)) {
		return path.vertices.front();
	}
	const auto cubic = SegmentCubic(path, segment);
	return BezierPoint(
		cubic.p0,
		cubic.p1,
		cubic.p2,
		cubic.p3,
		std::clamp(t, 0., 1.));
}

PathHit NearestPathPoint(const PathData &path, QPointF point) {
	auto result = PathHit();
	if (path.vertices.empty()) {
		return result;
	}
	result.segment = 0;
	result.point = path.vertices.front();
	result.distance = Distance(point, result.point);
	const auto segments = PathSegmentCount(path);
	constexpr auto kSamples = 24;
	for (auto segment = 0; segment != segments; ++segment) {
		const auto cubic = SegmentCubic(path, segment);
		const auto at = [&](double t) {
			return BezierPoint(cubic.p0, cubic.p1, cubic.p2, cubic.p3, t);
		};
		auto best = 0.;
		auto bestDistance = Distance(point, cubic.p0);
		for (auto i = 1; i <= kSamples; ++i) {
			const auto t = double(i) / kSamples;
			const auto distance = Distance(point, at(t));
			if (distance < bestDistance) {
				bestDistance = distance;
				best = t;
			}
		}
		auto from = std::max(best - 1. / kSamples, 0.);
		auto till = std::min(best + 1. / kSamples, 1.);
		for (auto i = 0; i != 32; ++i) {
			const auto a = from + (till - from) / 3.;
			const auto b = till - (till - from) / 3.;
			if (Distance(point, at(a)) < Distance(point, at(b))) {
				till = b;
			} else {
				from = a;
			}
		}
		const auto t = (from + till) / 2.;
		const auto candidate = at(t);
		const auto distance = Distance(point, candidate);
		if (distance < result.distance || segment == 0) {
			result.segment = segment;
			result.t = t;
			result.point = candidate;
			result.distance = distance;
		}
	}
	return result;
}

PathData WithInsertedVertex(const PathData &source, int segment, double t) {
	auto path = NormalizedPath(source);
	if (segment < 0 || segment >= PathSegmentCount(path)) {
		return path;
	}
	t = std::clamp(t, 0., 1.);
	const auto count = int(path.vertices.size());
	const auto from = segment;
	const auto to = (segment + 1) % count;
	const auto cubic = SegmentCubic(path, segment);
	auto vertex = QPointF();
	auto in = QPointF();
	auto out = QPointF();
	if (cubic.straight()) {
		// The same point PathPointAt() gives, the segment stays straight.
		vertex = BezierPoint(cubic.p0, cubic.p1, cubic.p2, cubic.p3, t);
	} else {
		const auto p01 = Mix(cubic.p0, cubic.p1, t);
		const auto p12 = Mix(cubic.p1, cubic.p2, t);
		const auto p23 = Mix(cubic.p2, cubic.p3, t);
		const auto p012 = Mix(p01, p12, t);
		const auto p123 = Mix(p12, p23, t);
		vertex = Mix(p012, p123, t);
		in = p012 - vertex;
		out = p123 - vertex;
		path.outTangents[from] = p01 - cubic.p0;
		path.inTangents[to] = p23 - cubic.p3;
	}
	const auto index = segment + 1;
	path.vertices.insert(begin(path.vertices) + index, vertex);
	path.inTangents.insert(begin(path.inTangents) + index, in);
	path.outTangents.insert(begin(path.outTangents) + index, out);
	return path;
}

PathData WithoutVertex(const PathData &source, int index) {
	auto path = NormalizedPath(source);
	if (index < 0
		|| index >= int(path.vertices.size())
		|| path.vertices.size() < 2) {
		return path;
	}
	path.vertices.erase(begin(path.vertices) + index);
	path.inTangents.erase(begin(path.inTangents) + index);
	path.outTangents.erase(begin(path.outTangents) + index);
	return path;
}

bool IsCornerVertex(const PathData &path, int index) {
	if (index < 0 || index >= int(path.vertices.size())) {
		return false;
	}
	return Near(TangentAt(path.inTangents, index), QPointF(), 1e-6)
		&& Near(TangentAt(path.outTangents, index), QPointF(), 1e-6);
}

PathData WithCornerVertex(const PathData &source, int index) {
	auto path = NormalizedPath(source);
	if (index >= 0 && index < int(path.vertices.size())) {
		path.inTangents[index] = QPointF();
		path.outTangents[index] = QPointF();
	}
	return path;
}

PathData WithSmoothVertex(const PathData &source, int index) {
	auto path = NormalizedPath(source);
	const auto count = int(path.vertices.size());
	if (index < 0 || index >= count || count < 2) {
		return path;
	}
	const auto vertex = path.vertices[index];
	const auto hasPrevious = path.closed || (index > 0);
	const auto hasNext = path.closed || (index + 1 < count);
	const auto previous = hasPrevious
		? path.vertices[(index + count - 1) % count]
		: vertex;
	const auto next = hasNext ? path.vertices[(index + 1) % count] : vertex;
	const auto direction = next - previous;
	const auto length = std::hypot(direction.x(), direction.y());
	if (length <= 0.) {
		return path;
	}
	const auto unit = direction / length;
	path.inTangents[index] = hasPrevious
		? (-unit * (Distance(vertex, previous) / 3.))
		: QPointF();
	path.outTangents[index] = hasNext
		? (unit * (Distance(vertex, next) / 3.))
		: QPointF();
	return path;
}

PathData ReversedPath(const PathData &source) {
	auto path = NormalizedPath(source);
	if (path.vertices.size() < 2) {
		return path;
	}
	std::reverse(begin(path.vertices), end(path.vertices));
	std::reverse(begin(path.inTangents), end(path.inTangents));
	std::reverse(begin(path.outTangents), end(path.outTangents));
	std::swap(path.inTangents, path.outTangents);
	if (path.closed) {
		std::rotate(
			path.vertices.rbegin(),
			path.vertices.rbegin() + 1,
			path.vertices.rend());
		std::rotate(
			path.inTangents.rbegin(),
			path.inTangents.rbegin() + 1,
			path.inTangents.rend());
		std::rotate(
			path.outTangents.rbegin(),
			path.outTangents.rbegin() + 1,
			path.outTangents.rend());
	}
	return path;
}

PathData RoundedPath(const PathData &path, double radius) {
	return RoundedPathWith(path, radius, nullptr);
}

Edit SetStaticValue(
		const Document &document,
		const PropertyRef &ref,
		const PropValue &value) {
	auto mutation = Mutation(document);
	const auto resolved = Resolve(mutation.data(), ref, true);
	if (!resolved) {
		return Failed(u"SetStaticValue: property not found"_q);
	} else if (IsKeyframed(resolved->json)) {
		return Failed(u"SetStaticValue: the property is animated"_q);
	}
	WriteStatic(mutation, *resolved, value);
	return mutation.finish();
}

Edit SetValueAt(
		const Document &document,
		const PropertyRef &ref,
		const PropValue &value,
		double frame) {
	auto mutation = Mutation(document);
	const auto resolved = Resolve(mutation.data(), ref, true);
	if (!resolved) {
		return Failed(u"SetValueAt: property not found"_q);
	}
	SetValueAtImpl(mutation, *resolved, value, frame);
	return mutation.finish();
}

Edit SetAnimated(
		const Document &document,
		const PropertyRef &ref,
		bool animated,
		double frame) {
	auto mutation = Mutation(document);
	const auto resolved = Resolve(mutation.data(), ref, true);
	if (!resolved || resolved->solid) {
		return Failed(u"SetAnimated: property not found"_q);
	} else if (IsKeyframed(resolved->json) == animated) {
		return Unchanged(document);
	}
	if (animated) {
		auto models = ModelList();
		auto model = Model();
		model.time = RoundTime(frame);
		model.value = ValueOf(*resolved, frame);
		model.touched = true;
		models.list.push_back(std::move(model));
		WriteKeyframes(mutation, *resolved, std::move(models));
	} else {
		const auto value = ValueOf(*resolved, frame);
		auto first = resolved->json.get("k").at(0).get("s");
		const auto previous = (resolved->spec.type == PropertyType::Path)
			? (first.isArray() ? first.at(0) : first)
			: (resolved->spec.type == PropertyType::Scalar)
			? Value()
			: first;
		auto property = resolved->json;
		if (property.has("a")) {
			property = property.with("a", Number(0));
		}
		property = property.with(
			"k",
			EncodeStaticValue(
				MergeValue(value, PropValue()),
				previous,
				resolved->spec.type));
		mutation.set(resolved->path, property);
		mutation.changed(resolved->node->id);
	}
	return mutation.finish();
}

Edit AddKeyframe(
		const Document &document,
		const PropertyRef &ref,
		double time,
		std::optional<PropValue> value,
		std::optional<Easing> easing) {
	auto mutation = Mutation(document);
	const auto resolved = Resolve(mutation.data(), ref, true);
	if (!resolved || resolved->solid) {
		return Failed(u"AddKeyframe: property not found"_q);
	}
	time = RoundTime(time);
	auto models = ReadModels(resolved->json, resolved->spec.type);
	const auto current = ValueOf(*resolved, time);
	const auto merged = value
		? MergeValue(SafeValue(*resolved, *value), current)
		: current;
	if (easing) {
		easing = SafeEasing(
			*easing,
			StrictEasing(*resolved, models.spatial));
	}
	const auto index = FindModel(models, time);
	if (index >= 0) {
		auto &model = models.list[index];
		if (model.value == merged && (!easing || model.easing == *easing)) {
			return Unchanged(document);
		}
		model.value = merged;
		if (easing) {
			model.easing = *easing;
		}
		model.touched = true;
	} else {
		auto model = Model();
		model.time = time;
		model.value = merged;
		model.easing = easing.value_or(EasingForInsert(models, time));
		model.touched = true;
		models.list.push_back(std::move(model));
	}
	WriteKeyframes(mutation, *resolved, std::move(models));
	return mutation.finish();
}

Edit SetKeyframeValue(
		const Document &document,
		const KeyframeRef &keyframe,
		const PropValue &value) {
	auto mutation = Mutation(document);
	const auto resolved = Resolve(mutation.data(), keyframe.property);
	if (!resolved || !IsKeyframed(resolved->json)) {
		return Failed(u"SetKeyframeValue: property not found"_q);
	}
	auto models = ReadModels(resolved->json, resolved->spec.type);
	const auto index = FindModel(models, keyframe.time);
	if (index < 0) {
		return Failed(u"SetKeyframeValue: keyframe not found"_q);
	}
	auto &model = models.list[index];
	const auto merged = MergeValue(SafeValue(*resolved, value), model.value);
	if (merged == model.value) {
		return Unchanged(document);
	}
	model.value = merged;
	model.touched = true;
	WriteKeyframes(mutation, *resolved, std::move(models));
	return mutation.finish();
}

Edit SetKeyframeEasing(
		const Document &document,
		const std::vector<KeyframeRef> &keyframes,
		const Easing &given) {
	auto mutation = Mutation(document);
	for (const auto &group : GroupKeyframes(keyframes)) {
		const auto resolved = Resolve(mutation.data(), group.property);
		if (!resolved || !IsKeyframed(resolved->json)) {
			continue;
		}
		auto models = ReadModels(resolved->json, resolved->spec.type);
		const auto easing = SafeEasing(
			given,
			StrictEasing(*resolved, models.spatial));
		auto changed = false;
		for (auto i = 0; i + 1 < int(models.list.size()); ++i) {
			auto &model = models.list[i];
			if (ContainsTime(group.times, model.time)
				&& !(model.easing == easing)) {
				model.easing = easing;
				model.touched = true;
				changed = true;
			}
		}
		if (changed) {
			WriteKeyframes(mutation, *resolved, std::move(models));
		}
	}
	return mutation.finish();
}

Edit RemoveKeyframes(
		const Document &document,
		const std::vector<KeyframeRef> &keyframes) {
	auto mutation = Mutation(document);
	for (const auto &group : GroupKeyframes(keyframes)) {
		const auto resolved = Resolve(mutation.data(), group.property);
		if (!resolved || !IsKeyframed(resolved->json)) {
			continue;
		}
		auto models = ReadModels(resolved->json, resolved->spec.type);
		auto removed = std::optional<PropValue>();
		auto &list = models.list;
		for (auto i = list.begin(); i != list.end();) {
			if (ContainsTime(group.times, i->time)) {
				if (!removed) {
					removed = i->value;
				}
				i = list.erase(i);
			} else {
				++i;
			}
		}
		if (!removed) {
			continue;
		} else if (list.empty()) {
			auto first = resolved->json.get("k").at(0).get("s");
			const auto previous = (resolved->spec.type == PropertyType::Path)
				? (first.isArray() ? first.at(0) : first)
				: (resolved->spec.type == PropertyType::Scalar)
				? Value()
				: first;
			auto property = resolved->json;
			if (property.has("a")) {
				property = property.with("a", Number(0));
			}
			property = property.with("k", EncodeStaticValue(
				*removed,
				previous,
				resolved->spec.type));
			mutation.set(resolved->path, property);
			mutation.changed(resolved->node->id);
		} else {
			WriteKeyframes(mutation, *resolved, std::move(models));
		}
	}
	return mutation.finish();
}

Edit MoveKeyframes(
		const Document &document,
		const std::vector<KeyframeRef> &keyframes,
		double delta) {
	if (delta == 0.) {
		return Unchanged(document);
	}
	auto mutation = Mutation(document);
	for (const auto &group : GroupKeyframes(keyframes)) {
		const auto resolved = Resolve(mutation.data(), group.property);
		if (!resolved || !IsKeyframed(resolved->json)) {
			continue;
		}
		auto models = ReadModels(resolved->json, resolved->spec.type);
		auto moved = std::vector<double>();
		for (auto &model : models.list) {
			if (ContainsTime(group.times, model.time)) {
				model.time = RoundTime(model.time + delta);
				model.touched = true;
				moved.push_back(model.time);
			}
		}
		if (moved.empty()) {
			continue;
		}
		auto &list = models.list;
		for (auto i = list.begin(); i != list.end();) {
			if (!i->touched && ContainsTime(moved, i->time)) {
				i = list.erase(i);
			} else {
				++i;
			}
		}
		WriteKeyframes(mutation, *resolved, std::move(models));
	}
	return mutation.finish();
}

Edit Rename(const Document &document, NodeId id, const QString &name) {
	auto mutation = Mutation(document);
	const auto node = mutation.node(id);
	if (!node) {
		return Failed(u"Rename: node not found"_q);
	}
	const auto &json = mutation.nodeJson(id);
	if (json.get("nm").toString() == name && json.has("nm")) {
		return Unchanged(document);
	}
	mutation.setNode(id, json.with("nm", Value::FromString(name)));
	return mutation.finish();
}

Edit SetHidden(
		const Document &document,
		const std::vector<NodeId> &ids,
		bool hidden) {
	auto mutation = Mutation(document);
	for (const auto id : ids) {
		const auto node = mutation.node(id);
		if (!node
			|| (node->kind != NodeKind::Layer
				&& node->kind != NodeKind::Shape)) {
			continue;
		}
		const auto &json = mutation.nodeJson(id);
		if (json.get("hd").toBool(false) == hidden) {
			continue;
		}
		mutation.setNode(id, (hidden || json.has("hd"))
			? json.with("hd", Value::FromBool(hidden))
			: json);
	}
	return mutation.finish();
}

Edit DeleteNodes(const Document &document, const std::vector<NodeId> &ids) {
	auto mutation = Mutation(document);
	const auto &data = mutation.data();
	const auto targets = TopLevelTargets(data, ids);
	auto removals = std::vector<std::pair<Path, NodeId>>();
	auto layersByComposition = std::map<NodeId, std::vector<NodeId>>();
	auto deletedLayers = std::unordered_set<NodeId>();
	auto assetTargets = std::vector<NodeId>();
	for (const auto id : targets) {
		const auto node = mutation.node(id);
		switch (node->kind) {
		case NodeKind::Composition:
			break;
		case NodeKind::Layer:
			layersByComposition[node->composition].push_back(id);
			deletedLayers.insert(id);
			break;
		case NodeKind::Asset:
			assetTargets.push_back(id);
			break;
		case NodeKind::Shape:
			if (node->shapeType == ShapeType::Transform) {
				const auto parent = mutation.node(node->parent);
				if (parent
					&& parent->kind == NodeKind::Shape
					&& parent->shapeType == ShapeType::Group) {
					break;
				}
			}
			removals.push_back({ *mutation.path(id), id });
			break;
		case NodeKind::Mask:
		case NodeKind::Effect:
			removals.push_back({ *mutation.path(id), id });
			break;
		}
	}

	std::sort(begin(removals), end(removals), [](
			const auto &a,
			const auto &b) {
		if (a.first.size() != b.first.size()) {
			return a.first.size() > b.first.size();
		}
		return a.first.back().index > b.first.back().index;
	});
	auto maskOwners = std::vector<NodeId>();
	for (const auto &[path, id] : removals) {
		const auto node = mutation.node(id);
		if (node->kind == NodeKind::Mask) {
			maskOwners.push_back(node->layer);
		}
		const auto container = ContainerPathOf(path);
		mutation.set(
			container,
			mutation.get(container).withoutItem(path.back().index));
		mutation.changed(node->parent);
		mutation.removed(id);
		mutation.structural();
	}
	for (const auto layer : maskOwners) {
		const auto &json = mutation.nodeJson(layer);
		if (json.get("masksProperties").size() == 0
			&& json.get("hasMask").toBool(false)) {
			mutation.setNode(layer, json.with("hasMask", Value::FromBool(false)));
		}
	}

	for (const auto &[composition, list] : layersByComposition) {
		const auto compositionNode = mutation.node(composition);
		const auto compositionPath = mutation.path(composition);
		if (!compositionNode || !compositionPath) {
			continue;
		}
		const auto layersPath = Append(*compositionPath, "layers");
		auto layers = mutation.get(layersPath).items();
		auto parentOf = std::map<int, std::optional<int>>();
		for (const auto id : list) {
			const auto node = mutation.node(id);
			if (node->ind) {
				parentOf[*node->ind] = node->parentInd;
			}
		}
		const auto resolveParent = [&](int ind) {
			auto result = std::optional<int>(ind);
			for (auto guard = 0; result && guard != 64; ++guard) {
				const auto i = parentOf.find(*result);
				if (i == end(parentOf)) {
					break;
				}
				result = i->second;
			}
			return result;
		};
		auto deletedIndices = std::unordered_set<int>();
		for (const auto id : list) {
			deletedIndices.insert(mutation.node(id)->index);
		}
		for (auto i = 0; i != int(layers.size()); ++i) {
			if (deletedIndices.contains(i)) {
				continue;
			}
			auto layer = layers[i];
			const auto &parent = layer.get("parent");
			if (parent.isNumber() && parentOf.contains(parent.toInt())) {
				const auto replacement = resolveParent(parent.toInt());
				layer = replacement
					? layer.with("parent", Number(*replacement))
					: WithoutParentOf(layer);
			}
			if (HasMatte(layer)
				&& i > 0
				&& deletedIndices.contains(i - 1)) {
				layer = layer.without("tt").without("tp");
			}
			if (i + 1 < int(layers.size())
				&& HasMatte(layers[i + 1])
				&& deletedIndices.contains(i + 1)
				&& layer.get("td").toInt(0) != 0) {
				layer = layer.with("hd", Value::FromBool(true));
			}
			if (!layer.sameAs(layers[i])) {
				mutation.changed(layers[i].id());
				layers[i] = std::move(layer);
			}
		}
		auto kept = std::vector<Value>();
		kept.reserve(layers.size());
		for (auto i = 0; i != int(layers.size()); ++i) {
			if (deletedIndices.contains(i)) {
				mutation.removed(layers[i].id());
			} else {
				kept.push_back(layers[i]);
			}
		}
		mutation.set(layersPath, Value::FromArray(std::move(kept)));
		mutation.changed(composition);
		mutation.structural();
	}

	auto assetIndices = std::vector<int>();
	for (const auto id : assetTargets) {
		auto used = false;
		for (const auto user : document.assetUsers(id)) {
			if (!deletedLayers.contains(user)) {
				used = true;
			}
		}
		if (!used) {
			assetIndices.push_back(mutation.node(id)->index);
			mutation.removed(id);
		}
	}
	std::sort(begin(assetIndices), end(assetIndices), std::greater<>());
	for (const auto index : assetIndices) {
		mutation.set(
			{ { "assets" } },
			mutation.get({ { "assets" } }).withoutItem(index));
		mutation.changed(document.rootId());
		mutation.structural();
	}
	return mutation.finish();
}

Edit DuplicateNodes(
		const Document &document,
		const std::vector<NodeId> &ids) {
	auto mutation = Mutation(document);
	const auto &data = mutation.data();
	const auto targets = TopLevelTargets(data, ids);
	auto items = std::vector<std::pair<Path, NodeId>>();
	auto layerTargets = std::map<NodeId, std::vector<int>>();
	for (const auto id : targets) {
		const auto node = mutation.node(id);
		if (node->kind == NodeKind::Layer) {
			layerTargets[node->composition].push_back(node->index);
		} else if (node->kind == NodeKind::Mask
			|| node->kind == NodeKind::Effect
			|| (node->kind == NodeKind::Shape
				&& node->shapeType != ShapeType::Transform)) {
			items.push_back({ *mutation.path(id), id });
		}
	}
	std::sort(begin(items), end(items), [](const auto &a, const auto &b) {
		if (a.first.size() != b.first.size()) {
			return a.first.size() > b.first.size();
		}
		return a.first.back().index > b.first.back().index;
	});
	for (const auto &[path, id] : items) {
		const auto container = ContainerPathOf(path);
		const auto &original = mutation.get(path);
		auto copy = original.withNewIds();
		const auto copyId = copy.id();
		mutation.set(
			container,
			mutation.get(container).withInsertedItem(
				path.back().index,
				std::move(copy)));
		mutation.changed(mutation.node(id)->parent);
		mutation.created(copyId);
		mutation.structural();
	}
	for (const auto &[composition, indices] : layerTargets) {
		const auto compositionPath = mutation.path(composition);
		if (!compositionPath) {
			continue;
		}
		const auto layersPath = Append(*compositionPath, "layers");
		auto layers = mutation.get(layersPath).items();
		const auto units = LayerUnits(layers);
		auto selectedUnits = std::vector<int>();
		for (const auto index : indices) {
			const auto unit = UnitOf(units, index);
			if (unit >= 0 && !ranges::contains(selectedUnits, unit)) {
				selectedUnits.push_back(unit);
			}
		}
		std::sort(begin(selectedUnits), end(selectedUnits), std::greater<>());
		auto nextInd = MaxInd(layers) + 1;
		for (const auto unitIndex : selectedUnits) {
			const auto unit = units[unitIndex];
			auto remap = std::map<int, int>();
			auto copies = std::vector<Value>();
			for (auto i = unit.from; i != unit.till; ++i) {
				auto copy = layers[i].withNewIds();
				if (copy.get("ind").isNumber()) {
					remap[copy.get("ind").toInt()] = nextInd;
				}
				copy = copy.with("ind", Number(nextInd++));
				copies.push_back(std::move(copy));
			}
			for (auto &copy : copies) {
				// "parent" and the matte link "tp" inside the copied unit
				// follow the copies.
				for (const auto key : { "parent", "tp" }) {
					const auto &link = copy.get(key);
					if (link.isNumber() && remap.contains(link.toInt())) {
						copy = copy.with(key, Number(remap[link.toInt()]));
					}
				}
				mutation.created(copy.id());
			}
			layers.insert(
				begin(layers) + unit.from,
				begin(copies),
				end(copies));
		}
		mutation.set(layersPath, Value::FromArray(std::move(layers)));
		mutation.changed(composition);
		mutation.structural();
	}
	return mutation.finish();
}

Edit MoveNode(
		const Document &document,
		NodeId id,
		NodeId container,
		int index) {
	auto mutation = Mutation(document);
	const auto node = mutation.node(id);
	if (!node) {
		return Failed(u"MoveNode: node not found"_q);
	}
	if (node->kind == NodeKind::Layer) {
		if (container && container != node->composition) {
			return Failed(u"MoveNode: layers move inside their composition"_q);
		}
		const auto compositionPath = mutation.path(node->composition);
		const auto layersPath = Append(*compositionPath, "layers");
		auto layers = mutation.get(layersPath).items();
		const auto units = LayerUnits(layers);
		const auto unitIndex = UnitOf(units, node->index);
		if (unitIndex < 0) {
			return Failed(u"MoveNode: bad layer index"_q);
		}
		const auto unit = units[unitIndex];
		index = std::clamp(index, 0, int(layers.size()));
		if (index >= unit.from && index <= unit.till) {
			return Unchanged(document);
		}
		auto moving = std::vector<Value>(
			begin(layers) + unit.from,
			begin(layers) + unit.till);
		layers.erase(begin(layers) + unit.from, begin(layers) + unit.till);
		if (index > unit.from) {
			index -= (unit.till - unit.from);
		}
		index = std::clamp(index, 0, int(layers.size()));
		while (index > 0
			&& index < int(layers.size())
			&& HasMatte(layers[index])) {
			--index;
		}
		layers.insert(begin(layers) + index, begin(moving), end(moving));
		mutation.set(layersPath, Value::FromArray(std::move(layers)));
		mutation.changed(node->composition);
		mutation.structural();
		return mutation.finish();
	} else if (node->kind == NodeKind::Mask) {
		// The order of masks matters: each one is combined with the
		// result of those before it.
		if (container && container != node->layer) {
			return Failed(u"MoveNode: masks move inside their layer"_q);
		}
		const auto maskPath = *mutation.path(id);
		const auto listPath = ContainerPathOf(maskPath);
		const auto from = maskPath.back().index;
		auto masks = mutation.get(listPath).items();
		if (from < 0 || from >= int(masks.size())) {
			return Failed(u"MoveNode: bad mask index"_q);
		}
		index = std::clamp(index, 0, int(masks.size()));
		if (index == from || index == from + 1) {
			return Unchanged(document);
		}
		auto moving = masks[from];
		masks.erase(begin(masks) + from);
		if (index > from) {
			--index;
		}
		masks.insert(begin(masks) + index, std::move(moving));
		mutation.set(listPath, Value::FromArray(std::move(masks)));
		mutation.changed(node->layer);
		mutation.structural();
		return mutation.finish();
	} else if (node->kind != NodeKind::Shape) {
		return Failed(u"MoveNode: only layers, masks and shape items move"_q);
	}
	const auto target = mutation.node(container ? container : node->parent);
	if (!target) {
		return Failed(u"MoveNode: container not found"_q);
	} else if (target->id == id || document.isDescendant(target->id, id)) {
		return Failed(u"MoveNode: can't move into itself"_q);
	}
	const auto targetIsGroup = (target->kind == NodeKind::Shape)
		&& (target->shapeType == ShapeType::Group);
	const auto targetIsLayer = (target->kind == NodeKind::Layer)
		&& (target->layerType == LayerType::Shape);
	if (!targetIsGroup && !targetIsLayer) {
		return Failed(u"MoveNode: bad container"_q);
	}
	const auto sourcePath = *mutation.path(id);
	const auto sourceArray = ContainerPathOf(sourcePath);
	const auto sourceIndex = sourcePath.back().index;
	auto targetArray = Append(
		*mutation.path(target->id),
		targetIsGroup ? "it" : "shapes");
	const auto sameArray = (target->id == node->parent);
	const auto moving = mutation.get(sourcePath);
	mutation.set(
		sourceArray,
		mutation.get(sourceArray).withoutItem(sourceIndex));
	AdjustAfterRemoval(targetArray, sourceArray, sourceIndex);
	auto list = mutation.get(targetArray).items();
	if (sameArray && index > sourceIndex) {
		--index;
	}
	index = std::clamp(index, 0, int(list.size()));
	if (targetIsGroup) {
		for (auto i = 0; i != int(list.size()); ++i) {
			if (list[i].get("ty").toString() == u"tr"_q) {
				index = std::min(index, i);
				break;
			}
		}
	}
	if (sameArray && index == sourceIndex) {
		return Unchanged(document);
	}
	list.insert(begin(list) + index, moving);
	mutation.set(targetArray, Value::FromArray(std::move(list)));
	mutation.changed(node->parent);
	mutation.changed(target->id);
	mutation.structural();
	return mutation.finish();
}

Edit ReorderNode(const Document &document, NodeId id, int delta) {
	const auto node = document.node(id);
	if (!node || !delta) {
		return Unchanged(document);
	}
	if (node->kind == NodeKind::Layer) {
		const auto &layers = document.json(node->composition)
			.get("layers")
			.items();
		const auto units = LayerUnits(layers);
		const auto unit = UnitOf(units, node->index);
		if (unit < 0) {
			return Unchanged(document);
		}
		const auto target = std::clamp(unit + delta, 0, int(units.size()) - 1);
		if (target == unit) {
			return Unchanged(document);
		}
		const auto index = (target < unit)
			? units[target].from
			: units[target].till;
		return MoveNode(document, id, node->composition, index);
	} else if (node->kind == NodeKind::Shape
		|| node->kind == NodeKind::Mask) {
		const auto index = (delta < 0)
			? (node->index + delta)
			: (node->index + delta + 1);
		if (index < 0) {
			return Unchanged(document);
		}
		return MoveNode(document, id, node->parent, index);
	}
	return Unchanged(document);
}

Edit AddLayer(
		const Document &document,
		LayerTemplate type,
		const QString &name,
		NodeId composition,
		int index,
		std::optional<ShapeTemplate> content) {
	auto mutation = Mutation(document);
	const auto owner = mutation.node(composition
		? composition
		: document.rootId());
	if (!owner
		|| (owner->kind != NodeKind::Composition
			&& owner->kind != NodeKind::Asset)) {
		return Failed(u"AddLayer: composition not found"_q);
	}
	const auto layersPath = Append(*mutation.path(owner->id), "layers");
	auto layers = mutation.get(layersPath).items();
	const auto size = document.size();
	const auto center = QPointF(size.width() / 2., size.height() / 2.);
	auto layer = Object({
		{ "ddd", Number(0) },
		{ "ind", Number(MaxInd(layers) + 1) },
		{ "ty", Number((type == LayerTemplate::Null) ? 3 : 4) },
		{ "nm", Value::FromString(name) },
		{ "sr", Number(1) },
		{ "ks", MakeTransform(center, true) },
		{ "ao", Number(0) },
	});
	if (type == LayerTemplate::Shape) {
		auto shapes = std::vector<Value>();
		if (content) {
			shapes.push_back(MakeShapeItem(
				*content,
				name,
				QColor(64, 140, 255),
				QSizeF(size)));
		}
		layer = layer.with("shapes", Value::FromArray(std::move(shapes)));
	}
	layer = layer
		.with("ip", Number(document.inPoint()))
		.with("op", Number(document.outPoint()))
		.with("st", Number(0))
		.with("bm", Number(0));
	index = std::clamp(index, 0, int(layers.size()));
	while (index > 0
		&& index < int(layers.size())
		&& HasMatte(layers[index])) {
		--index;
	}
	mutation.created(layer.id());
	layers.insert(begin(layers) + index, std::move(layer));
	mutation.set(layersPath, Value::FromArray(std::move(layers)));
	mutation.changed(owner->id);
	mutation.structural();
	return mutation.finish();
}

Edit AddShape(
		const Document &document,
		NodeId container,
		ShapeTemplate type,
		const QString &name,
		int index,
		QColor color) {
	auto mutation = Mutation(document);
	const auto target = mutation.node(container);
	if (!target) {
		return Failed(u"AddShape: container not found"_q);
	}
	const auto isGroup = (target->kind == NodeKind::Shape)
		&& (target->shapeType == ShapeType::Group);
	const auto isLayer = (target->kind == NodeKind::Layer)
		&& (target->layerType == LayerType::Shape);
	if (!isGroup && !isLayer) {
		return Failed(u"AddShape: bad container"_q);
	}
	const auto arrayPath = Append(
		*mutation.path(container),
		isGroup ? "it" : "shapes");
	auto list = mutation.get(arrayPath).items();
	auto transformIndex = int(list.size());
	for (auto i = 0; i != int(list.size()); ++i) {
		if (list[i].get("ty").toString() == u"tr"_q) {
			transformIndex = i;
			break;
		}
	}
	if (index < 0) {
		index = IsPathLike(type) ? 0 : transformIndex;
	}
	index = std::clamp(index, 0, transformIndex);
	auto item = MakeShapeItem(type, name, color, QSizeF(document.size()));
	mutation.created(item.id());
	list.insert(begin(list) + index, std::move(item));
	mutation.set(arrayPath, Value::FromArray(std::move(list)));
	mutation.changed(container);
	mutation.structural();
	return mutation.finish();
}

Edit SetLayerTiming(
		const Document &document,
		NodeId layer,
		double inPoint,
		double outPoint) {
	auto mutation = Mutation(document);
	const auto node = mutation.node(layer);
	if (!node || node->kind != NodeKind::Layer) {
		return Failed(u"SetLayerTiming: layer not found"_q);
	}
	inPoint = RoundTime(inPoint);
	outPoint = RoundTime(std::max(outPoint, inPoint + 1.));
	const auto &json = mutation.nodeJson(layer);
	if (json.get("ip").toDouble() == inPoint
		&& json.get("op").toDouble() == outPoint) {
		return Unchanged(document);
	}
	mutation.setNode(layer, json
		.with("ip", Number(inPoint))
		.with("op", Number(outPoint)));
	return mutation.finish();
}

Edit ShiftLayers(
		const Document &document,
		const std::vector<NodeId> &layers,
		double delta) {
	if (delta == 0.) {
		return Unchanged(document);
	}
	auto mutation = Mutation(document);
	const auto map = Fn<double(double)>([=](double time) {
		return time + delta;
	});
	for (const auto id : layers) {
		const auto node = mutation.node(id);
		if (!node || node->kind != NodeKind::Layer) {
			continue;
		}
		const auto precomp = (node->layerType == LayerType::Precomp);
		mutation.setNode(id, MapLayerTimes(
			mutation.nodeJson(id),
			map,
			1.,
			precomp));
	}
	return mutation.finish();
}

std::optional<PropertyRef> TransformProperty(
		const Document &document,
		NodeId id,
		TransformField field) {
	auto node = document.node(id);
	if (!node) {
		return std::nullopt;
	}
	auto prefix = QByteArray();
	if (node->kind == NodeKind::Layer) {
		prefix = "ks.";
	} else if (node->kind == NodeKind::Shape
		&& node->shapeType == ShapeType::Group) {
		node = document.node(node->transform);
		if (!node) {
			return std::nullopt;
		}
	} else if (node->kind == NodeKind::Shape
		&& node->shapeType == ShapeType::Repeater) {
		prefix = "tr.";
	} else if (node->kind != NodeKind::Shape
		|| node->shapeType != ShapeType::Transform) {
		return std::nullopt;
	}
	const auto &json = document.json(node->id);
	const auto transform = prefix.isEmpty()
		? json
		: json.get(prefix.left(prefix.size() - 1));
	const auto key = [&]() -> QByteArray {
		switch (field) {
		case TransformField::Anchor: return "a";
		case TransformField::Position: return "p";
		case TransformField::Scale: return "s";
		case TransformField::Rotation:
			return (transform.has("rz") && !transform.has("r"))
				? "rz"
				: "r";
		case TransformField::Opacity: return "o";
		case TransformField::Skew: return "sk";
		case TransformField::SkewAxis: return "sa";
		}
		return "o";
	}();
	return PropertyRef{ node->id, prefix + key };
}

Edit SetTransformAt(
		const Document &document,
		NodeId id,
		TransformField field,
		const PropValue &value,
		double frame) {
	const auto ref = TransformProperty(document, id, field);
	if (!ref) {
		return Failed(u"SetTransformAt: no transform"_q);
	}
	const auto &position = document.propertyJson(*ref);
	if (field == TransformField::Position
		&& position.get("s").toBool(false)
		&& (position.get("x").isObject() || position.get("y").isObject())) {
		auto result = SetValueAt(
			document,
			{ ref->node, ref->path + ".x" },
			PropValue::Scalar(value.point().x()),
			frame);
		if (!result) {
			return result;
		}
		auto second = SetValueAt(
			result.document,
			{ ref->node, ref->path + ".y" },
			PropValue::Scalar(value.point().y()),
			frame);
		if (!second) {
			return second;
		}
		second.changed.insert(
			end(second.changed),
			begin(result.changed),
			end(result.changed));
		return second;
	}
	return SetValueAt(document, *ref, value, frame);
}

namespace {

[[nodiscard]] bool IsGradientShape(const NodeInfo *node) {
	return node
		&& (node->kind == NodeKind::Shape)
		&& (node->shapeType == ShapeType::GradientFill
			|| node->shapeType == ShapeType::GradientStroke);
}

[[nodiscard]] bool IsStrokeShape(const NodeInfo *node) {
	return node
		&& (node->kind == NodeKind::Shape)
		&& (node->shapeType == ShapeType::Stroke
			|| node->shapeType == ShapeType::GradientStroke);
}

[[nodiscard]] bool IsFillShape(const NodeInfo *node) {
	return node
		&& (node->kind == NodeKind::Shape)
		&& (node->shapeType == ShapeType::Fill
			|| node->shapeType == ShapeType::GradientFill);
}

[[nodiscard]] Edit SetPlainMember(
		const Document &document,
		NodeId id,
		bool accepted,
		const char *key,
		Value value,
		const QString &error) {
	if (!accepted) {
		return Failed(error);
	}
	auto mutation = Mutation(document);
	const auto &json = mutation.nodeJson(id);
	if (json.has(key) && json.get(key) == value) {
		return Unchanged(document);
	}
	mutation.setNode(id, json.with(key, std::move(value)));
	return mutation.finish();
}

struct Decomposed {
	double rotation = 0.; // Degrees.
	QPointF scale = QPointF(100., 100.); // Percent.
};

// The linear part of a transform built like TransformMatrix() does is
// scale * rotation (row vectors): (sx cos, sx sin), (-sy sin, sy cos).
// A matrix with shear gets the closest values.
[[nodiscard]] Decomposed DecomposeLinear(
		const QTransform &matrix,
		QPointF previousScale) {
	constexpr auto kDegrees = 180. / 3.14159265358979323846;
	auto result = Decomposed();
	auto sx = std::hypot(matrix.m11(), matrix.m12());
	if (sx > 1e-12) {
		if (previousScale.x() < 0.) {
			sx = -sx;
		}
		const auto cos = matrix.m11() / sx;
		const auto sin = matrix.m12() / sx;
		result.rotation = std::atan2(sin, cos) * kDegrees;
		result.scale = QPointF(
			sx * 100.,
			(-matrix.m21() * sin + matrix.m22() * cos) * 100.);
		return result;
	}
	auto sy = std::hypot(matrix.m21(), matrix.m22());
	if (sy > 1e-12) {
		if (previousScale.y() < 0.) {
			sy = -sy;
		}
		result.rotation = std::atan2(-matrix.m21() / sy, matrix.m22() / sy)
			* kDegrees;
	}
	result.scale = QPointF(0., sy * 100.);
	return result;
}

[[nodiscard]] Value MapAffineProperty(
		const Value &property,
		const QTransform &matrix) {
	auto result = MapStoredValues(property, [&](
			const Value &value,
			int,
			bool) {
		return MapNumbers(value, [&](std::vector<double> numbers) {
			if (numbers.size() > 1) {
				const auto point = matrix.map(QPointF(numbers[0], numbers[1]));
				numbers[0] = RoundValue(point.x());
				numbers[1] = RoundValue(point.y());
			}
			return numbers;
		});
	});
	if (!IsKeyframed(result)) {
		return result;
	}
	auto items = result.get("k").items();
	auto changed = false;
	for (auto &keyframe : items) {
		for (const auto key : { "ti", "to" }) {
			if (!keyframe.has(key)) {
				continue;
			}
			auto mapped = MapNumbers(
				keyframe.get(key),
				[&](std::vector<double> numbers) {
					if (numbers.size() > 1) {
						const auto x = numbers[0] * matrix.m11()
							+ numbers[1] * matrix.m21();
						const auto y = numbers[0] * matrix.m12()
							+ numbers[1] * matrix.m22();
						numbers[0] = RoundValue(x);
						numbers[1] = RoundValue(y);
					}
					return numbers;
				});
			if (!mapped.sameAs(keyframe.get(key))) {
				keyframe = keyframe.with(key, std::move(mapped));
				changed = true;
			}
		}
	}
	return changed
		? result.with("k", Value::FromArray(std::move(items)))
		: result;
}

// Rewrites position, rotation and scale of a layer so that it stays in
// place when the space it lives in changes: correction maps the old parent
// space to the new one. Exact at the frame, the same correction goes to
// every keyframe.
[[nodiscard]] Value ReparentedLayer(
		const Value &layer,
		const QTransform &correction,
		double frame) {
	auto transform = layer.get("ks");
	if (!transform.isObject()) {
		return layer;
	}
	const auto is3d = (layer.get("ddd").toInt(1) != 0);
	const auto rotationKey = is3d ? "rz" : "r";
	const auto target = TransformMatrix(transform, frame, is3d) * correction;
	const auto oldScale = EvalPoint(
		transform.get("s"),
		frame,
		QPointF(100., 100.));
	const auto oldRotation = EvalScalar(transform.get(rotationKey), frame);
	const auto decomposed = DecomposeLinear(target, oldScale);
	const auto delta = std::remainder(
		decomposed.rotation - oldRotation,
		360.);
	const auto factorX = (std::abs(oldScale.x()) > 1e-9)
		? (decomposed.scale.x() / oldScale.x())
		: 1.;
	const auto factorY = (std::abs(oldScale.y()) > 1e-9)
		? (decomposed.scale.y() / oldScale.y())
		: 1.;
	const auto sane = [](double value) {
		return std::isfinite(value) && (std::abs(value) < 1e9);
	};
	if (!sane(delta)
		|| !sane(factorX)
		|| !sane(factorY)
		|| !sane(correction.m11())
		|| !sane(correction.m12())
		|| !sane(correction.m21())
		|| !sane(correction.m22())
		|| !sane(correction.dx())
		|| !sane(correction.dy())) {
		return layer;
	}

	const auto &position = transform.get("p");
	if (position.get("s").toBool(false)
		&& (position.get("x").isObject() || position.get("y").isObject())) {
		// Separated dimensions can't follow a rotation between keyframes:
		// an axis aligned correction is exact, any other one shifts the
		// values so that the layer is in place at the frame.
		const auto aligned = Near(correction.m12(), 0.)
			&& Near(correction.m21(), 0.);
		const auto oldPoint = QPointF(
			EvalScalar(position.get("x"), frame),
			EvalScalar(position.get("y"), frame));
		const auto newPoint = correction.map(oldPoint);
		auto separated = position;
		const auto part = [&](const char *key, double scale, double shift) {
			if (!position.get(key).isObject()) {
				return;
			}
			separated = separated.with(key, MapScalarProperty(
				position.get(key),
				[=](double value) {
					return RoundValue(value * scale + shift);
				}));
		};
		if (aligned) {
			part("x", correction.m11(), correction.dx());
			part("y", correction.m22(), correction.dy());
		} else {
			part("x", 1., newPoint.x() - oldPoint.x());
			part("y", 1., newPoint.y() - oldPoint.y());
		}
		transform = transform.with("p", separated);
	} else if (position.isObject()) {
		transform = transform.with(
			"p",
			MapAffineProperty(position, correction));
	} else {
		const auto point = correction.map(QPointF());
		if (!Near(point, QPointF())) {
			transform = transform.with("p", StaticProperty(Numbers({
				RoundValue(point.x()),
				RoundValue(point.y()),
				0.,
			})));
		}
	}
	if (!Near(delta, 0.)) {
		const auto &rotation = transform.get(rotationKey);
		transform = transform.with(rotationKey, rotation.isObject()
			? MapScalarProperty(rotation, [=](double value) {
				return RoundValue(value + delta);
			})
			: StaticProperty(Number(RoundValue(delta))));
	}
	if (!Near(factorX, 1.) || !Near(factorY, 1.)) {
		const auto &scale = transform.get("s");
		transform = transform.with("s", scale.isObject()
			? MapStoredValues(scale, [&](const Value &value, int, bool) {
				return MapNumbers(value, [&](std::vector<double> numbers) {
					if (numbers.size() > 0) {
						numbers[0] = RoundValue(numbers[0] * factorX);
					}
					if (numbers.size() > 1) {
						numbers[1] = RoundValue(numbers[1] * factorY);
					}
					return numbers;
				});
			})
			: StaticProperty(Numbers({
				RoundValue(100. * factorX),
				RoundValue(100. * factorY),
				100.,
			})));
	}
	return layer.with("ks", transform);
}

struct GradientTarget {
	Resolved resolved; // { shape, "g.k" }
	Path countPath; // "g.p"
	int colors = 0; // Color stops of the stored values.
};

[[nodiscard]] std::optional<GradientTarget> FindGradient(
		const Mutation &mutation,
		NodeId shape) {
	if (!IsGradientShape(mutation.node(shape))) {
		return std::nullopt;
	}
	auto resolved = Resolve(mutation.data(), { shape, "g.k" });
	if (!resolved || resolved->spec.type != PropertyType::Gradient) {
		return std::nullopt;
	}
	auto result = GradientTarget();
	result.countPath = Append(ContainerPathOf(resolved->path), "p");
	const auto &count = mutation.get(result.countPath);
	result.colors = EffectiveColorStops(
		StaticOrFirst(resolved->json, PropertyType::Gradient),
		count.isNumber() ? count.toInt() : -1);
	result.resolved = std::move(*resolved);
	return result;
}

// Rewrites every stored value of a gradient (the static one, "s" and "e"
// of every keyframe) without keeping any old tail, so the number of stops
// may change. The callback gets the keyframe index (-1 for static).
[[nodiscard]] Value MapGradientValues(
		const Value &property,
		int colors,
		const Fn<GradientData(const GradientData&, int, bool)> &map) {
	return MapStoredValues(property, [&](
			const Value &value,
			int keyframe,
			bool end) {
		if (!value.isArray()) {
			return value;
		}
		const auto numbers = value.numbers();
		const auto encoded = EncodeGradient(map(
			DecodeGradient(PropValue{ numbers }, colors),
			keyframe,
			end));
		return (encoded.numbers == numbers)
			? value
			: Numbers(encoded.numbers);
	});
}

// The old gradient with the stop offsets of another one.
[[nodiscard]] GradientData ResampledGradient(
		const GradientData &old,
		const GradientData &layout) {
	auto result = layout;
	for (auto &stop : result.colors) {
		auto color = GradientColorAt(old, stop.offset);
		color.setAlpha(255);
		stop.color = color;
	}
	for (auto &stop : result.alphas) {
		auto color = QColor(0, 0, 0);
		color.setAlphaF(float(std::clamp(
			GradientAlphaAt(old.alphas, stop.offset),
			0.,
			1.)));
		stop.color = color;
	}
	return result;
}

void WriteGradientCount(
		Mutation &mutation,
		const GradientTarget &target,
		NodeId shape,
		int colors) {
	const auto &count = mutation.get(target.countPath);
	if (!count.isNumber() || count.toInt() != colors) {
		mutation.set(target.countPath, Number(colors));
		mutation.changed(shape);
	}
}

[[nodiscard]] Value MakeDashItem(char kind, int number, double value) {
	const auto base = (kind == 'd')
		? u"dash"_q
		: (kind == 'g')
		? u"gap"_q
		: u"offset"_q;
	return Object({
		{ "n", Value::FromString(QString(QChar::fromLatin1(kind))) },
		{ "nm", Value::FromString((number > 0 && kind != 'o')
			? (base + QString::number(number + 1))
			: base) },
		{ "v", StaticProperty(Number(RoundValue(value))) },
	});
}

[[nodiscard]] Value RoundPathProperty(const Value &property, double radius) {
	if (!property.isObject()) {
		return property;
	}
	// The same vertices get rounded in every keyframe: those that are
	// corners in all of them.
	auto count = -1;
	auto consistent = true;
	auto corners = std::vector<bool>();
	[[maybe_unused]] const auto scanned = MapStoredValues(property, [&](
			const Value &value,
			int,
			bool) {
		const auto path = NormalizedPath(DecodePath(value));
		const auto size = int(path.vertices.size());
		if (count < 0) {
			count = size;
			corners.assign(size, true);
		} else if (count != size) {
			consistent = false;
			return value;
		}
		for (auto i = 0; i != size; ++i) {
			if (!IsCornerVertex(path, i)) {
				corners[i] = false;
			}
		}
		return value;
	});
	if (!consistent || count < 2) {
		return property;
	}
	return MapStoredValues(property, [&](const Value &value, int, bool) {
		const auto path = NormalizedPath(DecodePath(value));
		const auto rounded = RoundedPathWith(path, radius, &corners);
		if (rounded.vertices.size() == path.vertices.size()) {
			return value;
		}
		auto encoded = EncodePath(rounded, value);
		return value.isArray()
			? Value::FromArray({ std::move(encoded) })
			: encoded;
	});
}

[[nodiscard]] Value RoundShapeItem(
		const Value &item,
		double radius,
		std::vector<NodeId> &changed,
		int depth) {
	if (!item.isObject() || depth > 64) {
		return item;
	}
	const auto type = item.get("ty").toString();
	auto result = item;
	if (type == u"sh"_q) {
		if (item.get("ks").isObject()) {
			result = item.with(
				"ks",
				RoundPathProperty(item.get("ks"), radius));
		}
	} else if (type == u"rc"_q) {
		const auto &roundness = item.get("r");
		if (!roundness.isObject()) {
			result = item.with(
				"r",
				StaticProperty(Number(RoundValue(radius))));
		} else if (!IsKeyframed(roundness)
			&& roundness.get("k").toNumber(0.) == 0.) {
			result = item.with(
				"r",
				roundness.with("k", Number(RoundValue(radius))));
		}
	} else if (type == u"gr"_q) {
		auto items = item.get("it").items();
		auto any = false;
		for (auto &child : items) {
			auto rounded = RoundShapeItem(child, radius, changed, depth + 1);
			if (!rounded.sameAs(child)) {
				child = std::move(rounded);
				any = true;
			}
		}
		if (any) {
			result = item.with("it", Value::FromArray(std::move(items)));
		}
	}
	if (!result.sameAs(item)) {
		changed.push_back(item.id());
	}
	return result;
}

// Applies a structure change to every stored value of a path property.
// The callback returns an empty optional if it can't be applied.
template <typename Map>
[[nodiscard]] Edit MapPathValues(
		const Document &document,
		const PropertyRef &ref,
		const QString &name,
		Map &&map) {
	auto mutation = Mutation(document);
	const auto resolved = Resolve(mutation.data(), ref);
	if (!resolved || resolved->spec.type != PropertyType::Path) {
		return Failed(name + u": path not found"_q);
	}
	auto failed = false;
	auto mapped = MapStoredValues(resolved->json, [&](
			const Value &value,
			int,
			bool) {
		const auto path = NormalizedPath(DecodePath(value));
		const auto result = map(path);
		if (!result) {
			failed = true;
			return value;
		} else if (*result == path) {
			return value;
		}
		auto encoded = EncodePath(*result, value);
		return value.isArray()
			? Value::FromArray({ std::move(encoded) })
			: encoded;
	});
	if (failed) {
		return Failed(name + u": can't be applied to the path"_q);
	} else if (mapped.sameAs(resolved->json)) {
		return Unchanged(document);
	}
	mutation.set(resolved->path, std::move(mapped));
	mutation.changed(resolved->node->id);
	return mutation.finish();
}

[[nodiscard]] QPointF ClampHandle(QPointF handle, bool strict) {
	const auto limits = LimitsFor(strict);
	return QPointF(
		RoundValue(std::clamp(handle.x(), 0., 1.)),
		RoundValue(std::clamp(handle.y(), limits.minY, limits.maxY)));
}

// Where a new shape item goes in a "shapes" / "it" list, see AddShape().
[[nodiscard]] int ShapeInsertIndex(
		const std::vector<Value> &list,
		int index,
		bool pathLike) {
	auto transformIndex = int(list.size());
	for (auto i = 0; i != int(list.size()); ++i) {
		if (list[i].get("ty").toString() == u"tr"_q) {
			transformIndex = i;
			break;
		}
	}
	if (index < 0) {
		index = pathLike ? 0 : transformIndex;
	}
	return std::clamp(index, 0, transformIndex);
}

} // namespace

Edit Combined(Edit first, Edit second) {
	if (!first.ok()) {
		return first;
	} else if (!second.ok()) {
		return second;
	}
	const auto has = [](const std::vector<NodeId> &list, NodeId id) {
		return ranges::contains(list, id);
	};
	auto result = Edit();
	result.document = std::move(second.document);
	result.structural = first.structural || second.structural;
	for (const auto id : first.created) {
		if (!has(second.removed, id)) {
			result.created.push_back(id);
		}
	}
	for (const auto id : second.created) {
		if (!has(result.created, id)) {
			result.created.push_back(id);
		}
	}
	result.removed = first.removed;
	for (const auto id : second.removed) {
		if (!has(first.created, id) && !has(result.removed, id)) {
			result.removed.push_back(id);
		}
	}
	for (const auto id : first.changed) {
		if (!has(second.removed, id)) {
			result.changed.push_back(id);
		}
	}
	for (const auto id : second.changed) {
		if (!has(result.changed, id)) {
			result.changed.push_back(id);
		}
	}
	return result;
}

Edit AddMask(
		const Document &document,
		NodeId layer,
		const PathData &path,
		MaskMode mode,
		const QString &name,
		int index) {
	auto mutation = Mutation(document);
	const auto node = mutation.node(layer);
	if (!node || node->kind != NodeKind::Layer) {
		return Failed(u"AddMask: layer not found"_q);
	}
	const auto normalized = NormalizedPath(path);
	if (normalized.vertices.size() < 2) {
		return Failed(u"AddMask: the path needs at least two vertices"_q);
	}
	auto json = mutation.nodeJson(layer);
	auto masks = json.get("masksProperties").items();
	auto mask = Object({
		{ "inv", Value::FromBool(false) },
		{ "mode", Value::FromString(MaskModeCode(mode)) },
		{ "pt", StaticProperty(EncodePath(normalized, Value())) },
		{ "o", StaticProperty(Number(100)) },
		{ "x", StaticProperty(Number(0)) },
		{ "nm", Value::FromString(name.isEmpty()
			? (u"Mask "_q + QString::number(int(masks.size()) + 1))
			: name) },
	});
	mutation.created(mask.id());
	index = (index < 0)
		? int(masks.size())
		: std::clamp(index, 0, int(masks.size()));
	masks.insert(begin(masks) + index, std::move(mask));
	json = json.with("masksProperties", Value::FromArray(std::move(masks)));
	if (!json.get("hasMask").isBool() || !json.get("hasMask").toBool()) {
		json = json.with("hasMask", Value::FromBool(true));
	}
	mutation.setNode(layer, std::move(json));
	mutation.structural();
	return mutation.finish();
}

Edit SetMaskMode(const Document &document, NodeId mask, MaskMode mode) {
	const auto node = document.node(mask);
	return SetPlainMember(
		document,
		mask,
		node && (node->kind == NodeKind::Mask),
		"mode",
		Value::FromString(MaskModeCode(mode)),
		u"SetMaskMode: mask not found"_q);
}

Edit SetMaskInverted(const Document &document, NodeId mask, bool inverted) {
	const auto node = document.node(mask);
	return SetPlainMember(
		document,
		mask,
		node && (node->kind == NodeKind::Mask),
		"inv",
		Value::FromBool(inverted),
		u"SetMaskInverted: mask not found"_q);
}

PathData DefaultMaskPath(
		const Document &document,
		NodeId layer,
		double rootFrame) {
	const auto node = document.node(layer);
	if (!node || node->kind != NodeKind::Layer) {
		return PathData();
	}
	const auto canvas = QRectF(QPointF(), QSizeF(document.size()));
	auto bounds = LayerContent(
		document,
		*node,
		document.localFrame(layer, rootFrame),
		0).boundingRect();
	if (bounds.width() < 1. || bounds.height() < 1.) {
		const auto matrix = document.transformAt(layer, rootFrame);
		bounds = matrix.isInvertible()
			? matrix.inverted().mapRect(canvas)
			: canvas;
	}
	if (bounds.width() < 1. || bounds.height() < 1.) {
		bounds = canvas;
	}
	const auto round = [](double value) {
		return RoundTo(value, 2);
	};
	return RectanglePath(QRectF(
		QPointF(round(bounds.left()), round(bounds.top())),
		QPointF(round(bounds.right()), round(bounds.bottom()))));
}

Edit SetTrackMatte(
		const Document &document,
		NodeId layer,
		MatteMode mode,
		NodeId source) {
	auto mutation = Mutation(document);
	const auto node = mutation.node(layer);
	if (!node || node->kind != NodeKind::Layer) {
		return Failed(u"SetTrackMatte: layer not found"_q);
	}
	const auto compositionPath = mutation.path(node->composition);
	if (!compositionPath) {
		return Failed(u"SetTrackMatte: composition not found"_q);
	}
	const auto layersPath = Append(*compositionPath, "layers");
	auto layers = mutation.get(layersPath).items();
	const auto count = int(layers.size());
	auto index = node->index;
	if (index < 0 || index >= count || layers[index].id() != layer) {
		return Failed(u"SetTrackMatte: bad layer index"_q);
	}
	const auto wasMatted = HasMatte(layers[index]);
	const auto current = (wasMatted && index > 0 && !HasMatte(layers[index - 1]))
		? (index - 1)
		: -1;
	const auto release = [&](int position) {
		if (layers[position].has("td")) {
			mutation.changed(layers[position].id());
			layers[position] = layers[position].without("td");
		}
	};
	if (mode == MatteMode::None) {
		if (!wasMatted && !layers[index].has("tp")) {
			return Unchanged(document);
		}
		layers[index] = layers[index].without("tt").without("tp");
		if (current >= 0) {
			release(current);
		}
		mutation.changed(layer);
		mutation.set(layersPath, Value::FromArray(std::move(layers)));
		mutation.structural();
		return mutation.finish();
	}

	auto sourceIndex = -1;
	if (source) {
		const auto found = mutation.node(source);
		if (!found
			|| found->kind != NodeKind::Layer
			|| found->composition != node->composition
			|| found->index < 0
			|| found->index >= count) {
			return Failed(
				u"SetTrackMatte: the matte must be a layer of the same "
				"composition"_q);
		} else if (source == layer) {
			return Failed(u"SetTrackMatte: a layer can't be its own matte"_q);
		}
		sourceIndex = found->index;
	} else if (current >= 0) {
		sourceIndex = current;
	} else if (index > 0) {
		sourceIndex = index - 1;
	} else {
		return Failed(u"SetTrackMatte: no layer above to use as a matte"_q);
	}
	if (HasMatte(layers[sourceIndex])) {
		return Failed(u"SetTrackMatte: the matte layer is matted itself"_q);
	} else if (sourceIndex != current
		&& sourceIndex + 1 < count
		&& sourceIndex + 1 != index
		&& HasMatte(layers[sourceIndex + 1])) {
		return Failed(
			u"SetTrackMatte: the layer is the matte of another layer"_q);
	} else if (!wasMatted && index + 1 < count && HasMatte(layers[index + 1])) {
		return Failed(
			u"SetTrackMatte: a matte layer can't be matted itself"_q);
	}
	if (current >= 0 && current != sourceIndex) {
		release(current);
	}
	if (layers[sourceIndex].get("td").toInt(0) != 1) {
		layers[sourceIndex] = layers[sourceIndex].with("td", Number(1));
		mutation.changed(layers[sourceIndex].id());
	}
	auto matted = layers[index];
	if (matted.get("tt").toInt(0) != MatteCode(mode)) {
		matted = matted.with("tt", Number(MatteCode(mode)));
	}
	if (matted.has("tp")) {
		const auto &ind = layers[sourceIndex].get("ind");
		matted = ind.isNumber()
			? matted.with("tp", Number(ind.toInt()))
			: matted.without("tp");
	}
	if (!matted.sameAs(layers[index])) {
		layers[index] = std::move(matted);
		mutation.changed(layer);
	}
	if (sourceIndex != index - 1) {
		auto moving = layers[sourceIndex];
		layers.erase(begin(layers) + sourceIndex);
		if (sourceIndex < index) {
			--index;
		}
		layers.insert(begin(layers) + index, std::move(moving));
		mutation.changed(node->composition);
	}
	const auto &original = mutation.get(layersPath).items();
	if (original.size() == layers.size()
		&& std::equal(
			begin(layers),
			end(layers),
			begin(original),
			[](const Value &a, const Value &b) { return a.sameAs(b); })) {
		return Unchanged(document);
	}
	mutation.set(layersPath, Value::FromArray(std::move(layers)));
	mutation.structural();
	return mutation.finish();
}

bool CanSetLayerParent(
		const Document &document,
		NodeId layer,
		NodeId parent) {
	const auto node = document.node(layer);
	if (!node || node->kind != NodeKind::Layer) {
		return false;
	} else if (!parent) {
		return true;
	}
	auto current = document.node(parent);
	if (!current
		|| current->kind != NodeKind::Layer
		|| current->composition != node->composition
		|| parent == layer) {
		return false;
	}
	for (auto guard = 0; guard != 1024; ++guard) {
		if (!current->parentLayer) {
			return true;
		} else if (current->parentLayer == layer) {
			return false;
		}
		current = document.node(current->parentLayer);
		if (!current) {
			return true;
		}
	}
	return false;
}

Edit SetLayerParent(
		const Document &document,
		NodeId layer,
		NodeId parent,
		std::optional<double> keepAtFrame) {
	if (!CanSetLayerParent(document, layer, parent)) {
		return Failed(u"SetLayerParent: bad layer or parent"_q);
	}
	auto mutation = Mutation(document);
	const auto node = mutation.node(layer);
	if (node->parentLayer == parent && (parent || !node->parentInd)) {
		return Unchanged(document);
	}
	const auto composition = mutation.node(node->composition);
	const auto compositionPath = mutation.path(node->composition);
	if (!composition || !compositionPath) {
		return Failed(u"SetLayerParent: composition not found"_q);
	}
	const auto layersPath = Append(*compositionPath, "layers");
	auto layers = mutation.get(layersPath).items();
	const auto count = int(layers.size());
	if (node->index < 0 || node->index >= count) {
		return Failed(u"SetLayerParent: bad layer index"_q);
	}
	auto correction = QTransform();
	if (keepAtFrame) {
		const auto chain = [&](NodeId id) {
			const auto found = id ? mutation.node(id) : nullptr;
			return found
				? LayerInComposition(document, *found, *keepAtFrame)
				: QTransform();
		};
		const auto target = chain(parent);
		if (target.isInvertible()) {
			correction = chain(node->parentLayer) * target.inverted();
		}
	}
	auto value = layers[node->index];
	if (parent) {
		const auto target = mutation.node(parent);
		if (target->index < 0 || target->index >= count) {
			return Failed(u"SetLayerParent: bad parent index"_q);
		}
		auto ind = target->ind;
		auto usable = ind.has_value() && (*ind >= 0);
		if (usable) {
			// rlottie takes the first layer that has this "ind".
			for (const auto id : composition->children) {
				const auto other = mutation.node(id);
				if (other && other->ind == ind) {
					usable = (other->id == parent);
					break;
				}
			}
		}
		if (!usable) {
			const auto fresh = MaxInd(layers) + 1;
			for (const auto id : composition->children) {
				const auto other = mutation.node(id);
				if (other
					&& other->id != layer
					&& other->parentLayer == parent
					&& other->index >= 0
					&& other->index < count) {
					layers[other->index] = layers[other->index].with(
						"parent",
						Number(fresh));
					mutation.changed(other->id);
				}
			}
			layers[target->index] = layers[target->index].with(
				"ind",
				Number(fresh));
			mutation.changed(parent);
			ind = fresh;
		}
		value = value.with("parent", Number(*ind));
	} else {
		value = value.without("parent");
	}
	if (keepAtFrame && !qFuzzyCompare(correction, QTransform())) {
		value = ReparentedLayer(value, correction, *keepAtFrame);
	}
	layers[node->index] = std::move(value);
	mutation.set(layersPath, Value::FromArray(std::move(layers)));
	mutation.changed(layer);
	return mutation.finish();
}

Edit SetGradientType(
		const Document &document,
		NodeId shape,
		GradientType type) {
	return SetPlainMember(
		document,
		shape,
		IsGradientShape(document.node(shape)),
		"t",
		Number((type == GradientType::Radial) ? 2 : 1),
		u"SetGradientType: not a gradient"_q);
}

Edit SetTrimMode(const Document &document, NodeId shape, TrimMode mode) {
	const auto node = document.node(shape);
	return SetPlainMember(
		document,
		shape,
		node
			&& (node->kind == NodeKind::Shape)
			&& (node->shapeType == ShapeType::TrimPaths),
		"m",
		Number((mode == TrimMode::Individually) ? 2 : 1),
		u"SetTrimMode: not a trim paths item"_q);
}

Edit SetLineCap(const Document &document, NodeId shape, LineCap cap) {
	return SetPlainMember(
		document,
		shape,
		IsStrokeShape(document.node(shape)),
		"lc",
		Number((cap == LineCap::Butt) ? 1 : (cap == LineCap::Round) ? 2 : 3),
		u"SetLineCap: not a stroke"_q);
}

Edit SetLineJoin(const Document &document, NodeId shape, LineJoin join) {
	return SetPlainMember(
		document,
		shape,
		IsStrokeShape(document.node(shape)),
		"lj",
		Number((join == LineJoin::Miter)
			? 1
			: (join == LineJoin::Round)
			? 2
			: 3),
		u"SetLineJoin: not a stroke"_q);
}

Edit SetMiterLimit(const Document &document, NodeId shape, double limit) {
	return SetPlainMember(
		document,
		shape,
		IsStrokeShape(document.node(shape)),
		"ml",
		Number(RoundValue(std::clamp(limit, 0., 1000.))),
		u"SetMiterLimit: not a stroke"_q);
}

Edit SetFillRule(const Document &document, NodeId shape, FillRule rule) {
	return SetPlainMember(
		document,
		shape,
		IsFillShape(document.node(shape)),
		"r",
		Number((rule == FillRule::EvenOdd) ? 2 : 1),
		u"SetFillRule: not a fill"_q);
}

Edit SetGradient(
		const Document &document,
		NodeId shape,
		const GradientData &data,
		double frame) {
	if (!data.valid()
		|| int(data.colors.size()) > kMaxGradientStops
		|| int(data.alphas.size()) > kMaxGradientStops) {
		return Failed(u"SetGradient: bad stops"_q);
	}
	auto mutation = Mutation(document);
	const auto target = FindGradient(mutation, shape);
	if (!target) {
		return Failed(u"SetGradient: gradient not found"_q);
	}
	const auto encoded = EncodeGradient(data);
	const auto colors = int(data.colors.size());
	const auto normalized = DecodeGradient(encoded, colors);
	const auto &json = target->resolved.json;
	const auto sample = DecodeGradient(
		StaticOrFirst(json, PropertyType::Gradient),
		target->colors);
	if (sample.colors.size() == normalized.colors.size()
		&& sample.alphas.size() == normalized.alphas.size()) {
		SetValueAtImpl(mutation, target->resolved, encoded, frame);
	} else {
		auto property = json;
		auto keyframe = -1;
		if (IsKeyframed(property)) {
			const auto time = RoundTime(frame);
			auto models = ReadModels(property, PropertyType::Gradient);
			if (FindModel(models, time) < 0) {
				auto model = Model();
				model.time = time;
				model.value = ValueOf(target->resolved, time);
				model.easing = EasingForInsert(models, time);
				model.touched = true;
				models.list.push_back(std::move(model));
				property = WriteModels(
					property,
					std::move(models),
					PropertyType::Gradient);
			}
			const auto &items = property.get("k").items();
			for (auto i = 0; i != int(items.size()); ++i) {
				if (Near(items[i].get("t").toNumber(0.), time, kSameTime)) {
					keyframe = i;
					break;
				}
			}
		}
		property = MapGradientValues(property, target->colors, [&](
				const GradientData &old,
				int index,
				bool end) {
			return (index == keyframe && !end)
				? normalized
				: ResampledGradient(old, normalized);
		});
		mutation.set(target->resolved.path, std::move(property));
		mutation.changed(shape);
	}
	WriteGradientCount(mutation, *target, shape, colors);
	return mutation.finish();
}

Edit AddGradientStop(
		const Document &document,
		NodeId shape,
		double offset,
		bool alpha) {
	auto mutation = Mutation(document);
	const auto target = FindGradient(mutation, shape);
	if (!target) {
		return Failed(u"AddGradientStop: gradient not found"_q);
	}
	const auto &json = target->resolved.json;
	const auto sample = DecodeGradient(
		StaticOrFirst(json, PropertyType::Gradient),
		target->colors);
	if (int(alpha ? sample.alphas.size() : sample.colors.size())
		>= kMaxGradientStops) {
		return Failed(u"AddGradientStop: too many stops"_q);
	}
	offset = RoundValue(std::clamp(offset, 0., 1.));
	auto property = MapGradientValues(json, target->colors, [&](
			const GradientData &old,
			int,
			bool) {
		auto data = old;
		if (!alpha) {
			auto color = GradientColorAt(old, offset);
			color.setAlpha(255);
			data.colors.push_back({ offset, color });
		} else if (data.alphas.empty()) {
			const auto opaque = QColor(0, 0, 0, 255);
			data.alphas.push_back({ 0., opaque });
			if (offset > 0. && offset < 1.) {
				data.alphas.push_back({ offset, opaque });
			}
			data.alphas.push_back({ 1., opaque });
		} else {
			auto color = QColor(0, 0, 0);
			color.setAlphaF(float(std::clamp(
				GradientAlphaAt(old.alphas, offset),
				0.,
				1.)));
			data.alphas.push_back({ offset, color });
		}
		return data;
	});
	if (!property.sameAs(json)) {
		mutation.set(target->resolved.path, std::move(property));
		mutation.changed(shape);
	}
	WriteGradientCount(
		mutation,
		*target,
		shape,
		target->colors + (alpha ? 0 : 1));
	return mutation.finish();
}

Edit RemoveGradientStop(
		const Document &document,
		NodeId shape,
		int index,
		bool alpha) {
	auto mutation = Mutation(document);
	const auto target = FindGradient(mutation, shape);
	if (!target) {
		return Failed(u"RemoveGradientStop: gradient not found"_q);
	}
	const auto &json = target->resolved.json;
	const auto sample = DecodeGradient(
		StaticOrFirst(json, PropertyType::Gradient),
		target->colors);
	const auto count = int(alpha
		? sample.alphas.size()
		: sample.colors.size());
	if (index < 0 || index >= count || (!alpha && count < 2)) {
		return Failed(u"RemoveGradientStop: bad stop"_q);
	}
	auto property = MapGradientValues(json, target->colors, [&](
			const GradientData &old,
			int,
			bool) {
		auto data = old;
		if (!alpha) {
			if (index < int(data.colors.size()) && data.colors.size() > 1) {
				data.colors.erase(begin(data.colors) + index);
			}
		} else if (data.alphas.size() <= 2) {
			data.alphas.clear();
		} else if (index < int(data.alphas.size())) {
			data.alphas.erase(begin(data.alphas) + index);
		}
		return data;
	});
	if (!property.sameAs(json)) {
		mutation.set(target->resolved.path, std::move(property));
		mutation.changed(shape);
	}
	WriteGradientCount(
		mutation,
		*target,
		shape,
		target->colors - (alpha ? 0 : 1));
	return mutation.finish();
}

Edit ConvertPaint(
		const Document &document,
		NodeId shape,
		ShapeType type,
		double frame) {
	auto mutation = Mutation(document);
	const auto node = mutation.node(shape);
	if (!node || node->kind != NodeKind::Shape) {
		return Failed(u"ConvertPaint: shape not found"_q);
	} else if (node->shapeType == type) {
		return Unchanged(document);
	}
	const auto from = node->shapeType;
	const auto fills = (from == ShapeType::Fill && type == ShapeType::GradientFill)
		|| (from == ShapeType::GradientFill && type == ShapeType::Fill);
	const auto strokes = (from == ShapeType::Stroke
			&& type == ShapeType::GradientStroke)
		|| (from == ShapeType::GradientStroke && type == ShapeType::Stroke);
	if (!fills && !strokes) {
		return Failed(u"ConvertPaint: unsupported conversion"_q);
	}
	auto json = mutation.nodeJson(shape);
	if (type == ShapeType::GradientFill || type == ShapeType::GradientStroke) {
		const auto base = StaticOrFirst(json.get("c"), PropertyType::Color);
		const auto color = base.numbers.empty()
			? QColor(64, 140, 255)
			: base.color();
		const auto dark = color.darker(160);
		auto bounds = QRectF();
		if (const auto parent = mutation.node(node->parent)) {
			bounds = ItemsGeometry(
				document,
				parent->children,
				frame,
				0).boundingRect();
		}
		if (bounds.width() < 1.) {
			bounds = QRectF(-50., -50., 100., 100.);
		}
		const auto middle = RoundValue(bounds.center().y());
		json = json
			.with("ty", Value::FromString((type == ShapeType::GradientFill)
				? u"gf"_q
				: u"gs"_q))
			.without("c")
			.with("g", Object({
				{ "p", Number(2) },
				{ "k", StaticProperty(Numbers({
					0.,
					RoundValue(color.redF()),
					RoundValue(color.greenF()),
					RoundValue(color.blueF()),
					1.,
					RoundValue(dark.redF()),
					RoundValue(dark.greenF()),
					RoundValue(dark.blueF()),
				})) },
			}))
			.with("s", StaticProperty(Numbers({
				RoundValue(bounds.left()),
				middle,
			})))
			.with("e", StaticProperty(Numbers({
				RoundValue(bounds.right()),
				middle,
			})))
			.with("t", Number(1));
	} else {
		const auto &gradient = json.get("g");
		const auto &count = gradient.get("p");
		const auto data = DecodeGradient(
			StaticOrFirst(gradient.get("k"), PropertyType::Gradient),
			count.isNumber() ? count.toInt() : -1);
		const auto color = data.colors.empty()
			? QColor(64, 140, 255)
			: data.colors.front().color;
		json = json
			.with("ty", Value::FromString((type == ShapeType::Fill)
				? u"fl"_q
				: u"st"_q))
			.without("g")
			.without("s")
			.without("e")
			.without("t")
			.without("h")
			.without("a")
			.with("c", StaticProperty(ColorNumbers(color)));
	}
	mutation.setNode(shape, std::move(json));
	mutation.structural();
	return mutation.finish();
}

DashInfo DashesOf(const Document &document, NodeId stroke) {
	auto result = DashInfo();
	if (!IsStrokeShape(document.node(stroke))) {
		return result;
	}
	for (const auto &entry : ReadDashEntries(document.json(stroke).get("d"))) {
		const auto ref = PropertyRef{
			stroke,
			QByteArray("d.") + QByteArray::number(entry.index) + ".v",
		};
		if (entry.kind == 'd') {
			result.dashes.push_back(ref);
		} else if (entry.kind == 'g') {
			result.gaps.push_back(ref);
		} else if (!result.offset.valid()) {
			result.offset = ref;
		}
	}
	return result;
}

Edit SetDashes(
		const Document &document,
		NodeId stroke,
		const std::vector<double> &pattern,
		double offset) {
	if (!IsStrokeShape(document.node(stroke))) {
		return Failed(u"SetDashes: not a stroke"_q);
	}
	auto mutation = Mutation(document);
	const auto &json = mutation.nodeJson(stroke);
	if (pattern.empty()) {
		if (!json.has("d")) {
			return Unchanged(document);
		}
		mutation.setNode(stroke, json.without("d"));
		return mutation.finish();
	}
	auto values = std::vector<double>();
	for (const auto value : pattern) {
		if (int(values.size()) == kMaxDashPairs * 2) {
			break;
		}
		values.push_back(std::isfinite(value) ? std::max(value, 0.) : 0.);
	}
	if (values.size() % 2) {
		values.push_back(values.back());
	}
	auto items = std::vector<Value>();
	for (auto i = size_t(0); i != values.size(); i += 2) {
		items.push_back(MakeDashItem('d', int(i / 2), values[i]));
		items.push_back(MakeDashItem('g', int(i / 2), values[i + 1]));
	}
	items.push_back(MakeDashItem(
		'o',
		0,
		std::isfinite(offset) ? offset : 0.));
	// A pattern that is too short stalls the renderer, see SafeDashes().
	auto list = SafeDashes(Value::FromArray(std::move(items)));
	if (json.get("d") == list) {
		return Unchanged(document);
	}
	mutation.setNode(stroke, json.with("d", std::move(list)));
	return mutation.finish();
}

Edit SetDashCount(const Document &document, NodeId stroke, int pairs) {
	if (!IsStrokeShape(document.node(stroke))) {
		return Failed(u"SetDashCount: not a stroke"_q);
	}
	auto mutation = Mutation(document);
	const auto &json = mutation.nodeJson(stroke);
	if (pairs <= 0) {
		if (!json.has("d")) {
			return Unchanged(document);
		}
		mutation.setNode(stroke, json.without("d"));
		return mutation.finish();
	}
	pairs = std::min(pairs, kMaxDashPairs);
	auto dashes = std::vector<Value>();
	auto gaps = std::vector<Value>();
	auto offset = Value();
	for (auto &entry : ReadDashEntries(json.get("d"))) {
		if (entry.kind == 'd') {
			dashes.push_back(std::move(entry.json));
		} else if (entry.kind == 'g') {
			gaps.push_back(std::move(entry.json));
		} else if (offset.isNull()) {
			offset = std::move(entry.json);
		}
	}
	const auto valueOf = [](const Value &item, double fallback) {
		return item.isObject()
			? StaticOrFirst(item.get("v"), PropertyType::Scalar).scalar(
				fallback)
			: fallback;
	};
	auto items = std::vector<Value>();
	auto lastDash = dashes.empty() ? 10. : valueOf(dashes.back(), 10.);
	for (auto i = 0; i != pairs; ++i) {
		auto dash = (i < int(dashes.size()))
			? dashes[i]
			: MakeDashItem('d', i, lastDash);
		const auto dashValue = valueOf(dash, lastDash);
		auto gap = (i < int(gaps.size()))
			? gaps[i]
			: MakeDashItem(
				'g',
				i,
				std::max(dashValue, kMinDashPeriod));
		items.push_back(std::move(dash));
		items.push_back(std::move(gap));
	}
	items.push_back(offset.isObject() ? offset : MakeDashItem('o', 0, 0.));
	auto list = SafeDashes(Value::FromArray(std::move(items)));
	if (json.get("d") == list) {
		return Unchanged(document);
	}
	mutation.setNode(stroke, json.with("d", std::move(list)));
	return mutation.finish();
}

Edit BakeRoundCorners(
		const Document &document,
		NodeId roundCorners,
		double frame) {
	auto mutation = Mutation(document);
	const auto node = mutation.node(roundCorners);
	const auto path = mutation.path(roundCorners);
	if (!node
		|| !path
		|| node->kind != NodeKind::Shape
		|| node->shapeType != ShapeType::RoundCorners) {
		return Failed(u"BakeRoundCorners: not a round corners item"_q);
	}
	const auto radius = EvalScalar(
		mutation.nodeJson(roundCorners).get("r"),
		frame);
	const auto container = ContainerPathOf(*path);
	auto items = mutation.get(container).items();
	const auto index = path->back().index;
	if (index < 0 || index >= int(items.size())) {
		return Failed(u"BakeRoundCorners: bad index"_q);
	}
	auto changed = std::vector<NodeId>();
	if (radius > 0.) {
		for (auto i = 0; i != index; ++i) {
			items[i] = RoundShapeItem(items[i], radius, changed, 0);
		}
	}
	items.erase(begin(items) + index);
	mutation.set(container, Value::FromArray(std::move(items)));
	for (const auto id : changed) {
		mutation.changed(id);
	}
	mutation.changed(node->parent);
	mutation.removed(roundCorners);
	mutation.structural();
	return mutation.finish();
}

Edit AddPath(
		const Document &document,
		NodeId container,
		const PathData &path,
		const QString &name,
		int index) {
	auto mutation = Mutation(document);
	const auto target = mutation.node(container);
	if (!target) {
		return Failed(u"AddPath: container not found"_q);
	}
	const auto isGroup = (target->kind == NodeKind::Shape)
		&& (target->shapeType == ShapeType::Group);
	const auto isLayer = (target->kind == NodeKind::Layer)
		&& (target->layerType == LayerType::Shape);
	if (!isGroup && !isLayer) {
		return Failed(u"AddPath: bad container"_q);
	}
	const auto normalized = NormalizedPath(path);
	if (normalized.vertices.empty()) {
		return Failed(u"AddPath: empty path"_q);
	}
	const auto arrayPath = Append(
		*mutation.path(container),
		isGroup ? "it" : "shapes");
	auto list = mutation.get(arrayPath).items();
	auto item = Object({
		{ "ty", Value::FromString(u"sh"_q) },
		{ "d", Number(1) },
		{ "ks", StaticProperty(EncodePath(normalized, Value())) },
		{ "nm", Value::FromString(name.isEmpty() ? u"Path"_q : name) },
	});
	mutation.created(item.id());
	list.insert(
		begin(list) + ShapeInsertIndex(list, index, true),
		std::move(item));
	mutation.set(arrayPath, Value::FromArray(std::move(list)));
	mutation.changed(container);
	mutation.structural();
	return mutation.finish();
}

Edit InsertPathVertex(
		const Document &document,
		const PropertyRef &path,
		int segment,
		double t) {
	return MapPathValues(document, path, u"InsertPathVertex"_q, [&](
			const PathData &data) -> std::optional<PathData> {
		if (segment < 0 || segment >= PathSegmentCount(data)) {
			return std::nullopt;
		}
		return WithInsertedVertex(data, segment, t);
	});
}

Edit RemovePathVertex(
		const Document &document,
		const PropertyRef &path,
		int index) {
	return MapPathValues(document, path, u"RemovePathVertex"_q, [&](
			const PathData &data) -> std::optional<PathData> {
		if (index < 0
			|| index >= int(data.vertices.size())
			|| data.vertices.size() < 3) {
			return std::nullopt;
		}
		return WithoutVertex(data, index);
	});
}

Edit SetPathClosed(
		const Document &document,
		const PropertyRef &path,
		bool closed) {
	return MapPathValues(document, path, u"SetPathClosed"_q, [&](
			const PathData &data) -> std::optional<PathData> {
		auto result = data;
		result.closed = closed;
		return result;
	});
}

Edit ReversePath(const Document &document, const PropertyRef &path) {
	return MapPathValues(document, path, u"ReversePath"_q, [&](
			const PathData &data) -> std::optional<PathData> {
		return ReversedPath(data);
	});
}

std::optional<KeyframeHandles> HandlesOf(
		const Document &document,
		const KeyframeRef &keyframe) {
	const auto resolved = Resolve(
		DocumentAccess::data(document),
		keyframe.property);
	if (!resolved || resolved->solid || !IsKeyframed(resolved->json)) {
		return std::nullopt;
	}
	const auto models = ReadModels(resolved->json, resolved->spec.type);
	const auto index = FindModel(models, keyframe.time);
	if (index < 0) {
		return std::nullopt;
	}
	auto result = KeyframeHandles();
	if (index > 0) {
		const auto &previous = models.list[index - 1];
		result.hasIn = true;
		result.holdIn = previous.easing.hold;
		result.in = previous.easing.hold
			? QPointF(1., 1.)
			: previous.easing.in;
		result.previousTime = previous.time;
	}
	if (index + 1 < int(models.list.size())) {
		const auto &own = models.list[index];
		result.hasOut = true;
		result.holdOut = own.easing.hold;
		result.out = own.easing.hold ? QPointF(0., 0.) : own.easing.out;
		result.nextTime = models.list[index + 1].time;
	}
	return result;
}

Edit SetKeyframeHandles(
		const Document &document,
		const KeyframeRef &keyframe,
		std::optional<QPointF> in,
		std::optional<QPointF> out) {
	auto mutation = Mutation(document);
	const auto resolved = Resolve(mutation.data(), keyframe.property);
	if (!resolved || resolved->solid || !IsKeyframed(resolved->json)) {
		return Failed(u"SetKeyframeHandles: property not found"_q);
	}
	auto models = ReadModels(resolved->json, resolved->spec.type);
	const auto index = FindModel(models, keyframe.time);
	if (index < 0) {
		return Failed(u"SetKeyframeHandles: keyframe not found"_q);
	}
	auto changed = false;
	const auto strict = StrictEasing(*resolved, models.spatial);
	const auto apply = [&](Model &model, const Easing &easing) {
		const auto safe = SafeEasing(easing, strict);
		if (!(model.easing == safe)) {
			model.easing = safe;
			model.touched = true;
			changed = true;
		}
	};
	if (out && index + 1 < int(models.list.size())) {
		auto &model = models.list[index];
		apply(model, Easing{
			ClampHandle(*out, strict),
			model.easing.hold ? QPointF(1., 1.) : model.easing.in,
			false,
		});
	}
	if (in && index > 0) {
		auto &model = models.list[index - 1];
		apply(model, Easing{
			model.easing.hold ? QPointF(0., 0.) : model.easing.out,
			ClampHandle(*in, strict),
			false,
		});
	}
	if (!changed) {
		return Unchanged(document);
	}
	WriteKeyframes(mutation, *resolved, std::move(models));
	return mutation.finish();
}

Edit SetKeyframeEasings(
		const Document &document,
		const std::vector<std::pair<KeyframeRef, Easing>> &easings) {
	auto mutation = Mutation(document);
	auto properties = std::vector<PropertyRef>();
	for (const auto &[keyframe, easing] : easings) {
		if (!ranges::contains(properties, keyframe.property)) {
			properties.push_back(keyframe.property);
		}
	}
	for (const auto &property : properties) {
		const auto resolved = Resolve(mutation.data(), property);
		if (!resolved || resolved->solid || !IsKeyframed(resolved->json)) {
			continue;
		}
		auto models = ReadModels(resolved->json, resolved->spec.type);
		const auto strict = StrictEasing(*resolved, models.spatial);
		auto changed = false;
		for (auto i = 0; i + 1 < int(models.list.size()); ++i) {
			auto &model = models.list[i];
			for (const auto &[keyframe, given] : easings) {
				const auto easing = SafeEasing(given, strict);
				if (keyframe.property == property
					&& Near(keyframe.time, model.time, kSameTime)
					&& !(model.easing == easing)) {
					model.easing = easing;
					model.touched = true;
					changed = true;
				}
			}
		}
		if (changed) {
			WriteKeyframes(mutation, *resolved, std::move(models));
		}
	}
	return mutation.finish();
}

EasingLimits EasingRange(const Document &document, const PropertyRef &ref) {
	const auto resolved = Resolve(DocumentAccess::data(document), ref, true);
	if (!resolved) {
		return EasingLimits();
	}
	const auto spatial = !resolved->missing
		&& !resolved->solid
		&& IsKeyframed(resolved->json)
		&& ReadModels(resolved->json, resolved->spec.type).spatial;
	return LimitsFor(StrictEasing(*resolved, spatial));
}

Edit SetMotionPath(
		const Document &document,
		const PropertyRef &ref,
		bool enabled) {
	auto mutation = Mutation(document);
	const auto resolved = Resolve(mutation.data(), ref);
	if (!resolved
		|| resolved->solid
		|| resolved->spec.type != PropertyType::Vector) {
		return Failed(u"SetMotionPath: not a point property"_q);
	} else if (!IsKeyframed(resolved->json)) {
		return Unchanged(document);
	}
	auto models = ReadModels(resolved->json, PropertyType::Vector);
	if (models.spatial == enabled) {
		return Unchanged(document);
	} else if (enabled) {
		// rlottie moves any point value along the curve, but only these
		// are positions.
		switch (resolved->spec.role) {
		case PropertyRole::Position:
		case PropertyRole::Anchor:
		case PropertyRole::StartPoint:
		case PropertyRole::EndPoint:
			break;
		default:
			return Failed(u"SetMotionPath: not a position"_q);
		}
	}
	models.spatial = enabled;
	for (auto &model : models.list) {
		model.touched = true;
		if (enabled) {
			model.easing = SafeEasing(model.easing, true);
		} else {
			model.outTangent.clear();
			model.inTangent.clear();
		}
	}
	WriteKeyframes(mutation, *resolved, std::move(models));
	return mutation.finish();
}

Edit ReplaceColor(
		const Document &document,
		const QColor &from,
		const QColor &to,
		const std::vector<NodeId> &scope) {
	const auto key = qRgb(from.red(), from.green(), from.blue());
	const auto target = ToRgb(to);
	return MapColors(document, scope, [&](
			Rgb color,
			int,
			bool,
			int) -> std::optional<Rgb> {
		if (Quantize(color) != key) {
			return std::nullopt;
		}
		return target;
	});
}

Edit SetColor(
		const Document &document,
		const ColorOccurrence &occurrence,
		const QColor &color) {
	auto mutation = Mutation(document);
	const auto &data = mutation.data();
	const auto node = FindNode(data, occurrence.property.node);
	if (!node) {
		return Failed(u"SetColor: node not found"_q);
	}
	const auto target = ToRgb(color);
	const auto map = ColorMap([&](
			Rgb,
			int keyframe,
			bool end,
			int stop) -> std::optional<Rgb> {
		if (keyframe != occurrence.keyframe
			|| end != occurrence.end
			|| stop != occurrence.stop) {
			return std::nullopt;
		}
		return target;
	});
	const auto property = ColorProperty{
		node,
		occurrence.property.path,
		occurrence.kind,
		(occurrence.kind == ColorKind::GradientFill
			|| occurrence.kind == ColorKind::GradientStroke)
			? NodeJson(data, node->id).get("g").get("p").toInt(0)
			: 0,
	};
	const auto nodePath = FindPath(data, node->id);
	const auto relative = RelativePath(
		mutation.get(*nodePath),
		occurrence.property.path);
	if (!relative) {
		return Failed(u"SetColor: property not found"_q);
	}
	const auto path = Concat(*nodePath, *relative);
	const auto &json = mutation.get(path);
	auto mapped = MapColorProperty(json, property, map);
	if (!mapped.sameAs(json)) {
		mutation.set(path, std::move(mapped));
		mutation.changed(node->id);
	}
	return mutation.finish();
}

Edit AdjustHsl(
		const Document &document,
		int hueDegrees,
		int saturationPercent,
		int lightnessPercent,
		const std::vector<NodeId> &scope) {
	auto hue = std::fmod(double(hueDegrees), 360.);
	if (hue < 0.) {
		hue += 360.;
	}
	const auto sat = std::clamp(saturationPercent, -100, 100) / 100.;
	const auto light = std::clamp(lightnessPercent, -100, 100) / 100.;
	if (hue == 0. && sat == 0. && light == 0.) {
		return Unchanged(document);
	}
	return MapColors(document, scope, [&](
			Rgb color,
			int,
			bool,
			int) -> std::optional<Rgb> {
		return AdjustRgb(color, hue, sat, light);
	});
}

Edit SetCanvasSize(
		const Document &document,
		QSize size,
		bool scaleContent) {
	if (size.isEmpty()) {
		return Failed(u"SetCanvasSize: empty size"_q);
	}
	const auto old = document.size();
	if (old == size) {
		return Unchanged(document);
	}
	auto mutation = Mutation(document);
	auto root = mutation.root()
		.with("w", Number(size.width()))
		.with("h", Number(size.height()));
	const auto scale = (scaleContent && !old.isEmpty())
		? std::min(
			double(size.width()) / old.width(),
			double(size.height()) / old.height())
		: 1.;
	const auto offset = QPointF(
		(size.width() - old.width() * scale) / 2.,
		(size.height() - old.height() * scale) / 2.);
	if (scale != 1. || offset != QPointF()) {
		const auto &data = mutation.data();
		for (const auto id : document.layers()) {
			const auto node = FindNode(data, id);
			if (!node || !LayerIsTopLevel(*node)) {
				continue;
			}
			const auto path = Append(*FindPath(data, id), "ks");
			auto transform = GetIn(root, path);
			if (!transform.isObject()) {
				continue;
			}
			const auto &position = transform.get("p");
			if (position.get("s").toBool(false)
				&& (position.get("x").isObject()
					|| position.get("y").isObject())) {
				auto separated = position;
				if (position.get("x").isObject()) {
					separated = separated.with("x", MapScalarProperty(
						position.get("x"),
						[&](double v) {
							return RoundValue(v * scale + offset.x());
						}));
				}
				if (position.get("y").isObject()) {
					separated = separated.with("y", MapScalarProperty(
						position.get("y"),
						[&](double v) {
							return RoundValue(v * scale + offset.y());
						}));
				}
				transform = transform.with("p", separated);
			} else if (position.isObject()) {
				transform = transform.with(
					"p",
					MapPointProperty(position, scale, offset));
			} else if (offset != QPointF()) {
				transform = transform.with("p", StaticProperty(Numbers({
					RoundValue(offset.x()),
					RoundValue(offset.y()),
					0.,
				})));
			}
			if (scale != 1.) {
				transform = transform.get("s").isObject()
					? transform.with("s", MapPointProperty(
						transform.get("s"),
						scale,
						QPointF()))
					: transform.with("s", StaticProperty(Numbers({
						RoundValue(100. * scale),
						RoundValue(100. * scale),
						100.,
					})));
			}
			root = SetIn(root, path, transform);
			mutation.changed(id);
		}
	}
	mutation.setRoot(std::move(root));
	mutation.changed(document.rootId());
	return mutation.finish();
}

Edit SetFrameRate(const Document &document, double fps, bool retime) {
	const auto old = document.frameRate();
	if (fps <= 0. || !std::isfinite(fps)) {
		return Failed(u"SetFrameRate: bad frame rate"_q);
	} else if (old == fps) {
		return Unchanged(document);
	}
	auto mutation = Mutation(document);
	auto root = (retime && old > 0.)
		? TransformTimes(mutation.root(), fps / old, 0., 1.)
		: mutation.root();
	mutation.setRoot(root.with("fr", Number(RoundTo(fps, 3))));
	mutation.changed(document.rootId());
	return mutation.finish();
}

Edit SetDuration(const Document &document, int frames) {
	if (frames <= 0) {
		return Failed(u"SetDuration: bad duration"_q);
	}
	const auto oldOut = document.outPoint();
	const auto newOut = std::trunc(document.inPoint()) + frames;
	if (newOut == oldOut) {
		return Unchanged(document);
	}
	auto mutation = Mutation(document);
	auto root = mutation.root().with("op", Number(newOut));
	if (newOut > oldOut) {
		const auto extend = [&](const Value &list) {
			auto items = list.items();
			auto changed = false;
			for (auto &layer : items) {
				if (layer.get("op").toDouble(0.) >= oldOut - 0.5) {
					layer = layer.with("op", Number(newOut));
					changed = true;
				}
			}
			return changed ? Value::FromArray(std::move(items)) : list;
		};
		root = root.with("layers", extend(root.get("layers")));
		if (root.get("assets").isArray()) {
			auto assets = root.get("assets").items();
			for (auto &asset : assets) {
				if (asset.get("layers").isArray()) {
					asset = asset.with("layers", extend(asset.get("layers")));
				}
			}
			root = root.with("assets", Value::FromArray(std::move(assets)));
		}
	}
	mutation.setRoot(std::move(root));
	mutation.changed(document.rootId());
	return mutation.finish();
}

Edit ChangeSpeed(const Document &document, double factor) {
	if (factor <= 0. || !std::isfinite(factor)) {
		return Failed(u"ChangeSpeed: bad factor"_q);
	} else if (factor == 1.) {
		return Unchanged(document);
	}
	auto mutation = Mutation(document);
	mutation.setRoot(TransformTimes(
		mutation.root(),
		1. / factor,
		0.,
		1. / factor));
	mutation.changed(document.rootId());
	return mutation.finish();
}

Edit TrimRange(
		const Document &document,
		double from,
		double to,
		bool rebase) {
	from = std::round(from);
	to = std::round(to);
	if (to <= from) {
		return Failed(u"TrimRange: empty range"_q);
	}
	auto mutation = Mutation(document);
	auto root = mutation.root()
		.with("ip", Number(from))
		.with("op", Number(to));
	if (rebase && from != 0.) {
		root = TransformTimes(root, 1., -from, 1.);
	}
	mutation.setRoot(std::move(root));
	mutation.changed(document.rootId());
	return mutation.finish();
}

namespace {

[[nodiscard]] bool IsMetadataKey(QByteArrayView key) {
	static constexpr auto kKeys = std::array{
		"mn", "ix", "cix", "np", "cl", "ln", "meta",
	};
	for (const auto candidate : kKeys) {
		if (key == QByteArrayView(candidate)) {
			return true;
		}
	}
	return false;
}

enum class OptimizeContext : uchar {
	Root,
	Layer,
	Other,
};

[[nodiscard]] Value OptimizeValue(
		const Value &value,
		const OptimizeOptions &options,
		OptimizeContext context) {
	if (value.isNumber()) {
		const auto rounded = RoundTo(value.toDouble(), options.decimals);
		return (rounded == value.toDouble()) ? value : Number(rounded);
	} else if (value.isArray()) {
		auto items = value.items();
		auto changed = false;
		for (auto &item : items) {
			auto optimized = OptimizeValue(item, options, context);
			if (!optimized.sameAs(item)) {
				item = std::move(optimized);
				changed = true;
			}
		}
		return changed ? Value::FromArray(std::move(items)) : value;
	} else if (!value.isObject()) {
		return value;
	}
	auto result = value;
	for (const auto &member : value.members()) {
		const auto &key = member.key;
		const auto remove = IsMetadataKey(key)
			|| (key == "hd" && member.value.isBool() && !member.value.toBool())
			|| (key == "bm" && member.value.toDouble(-1.) == 0.)
			|| (key == "ao" && member.value.toDouble(-1.) == 0.)
			|| (key == "nm"
				&& ((context == OptimizeContext::Other
						&& options.stripShapeNames)
					|| (context == OptimizeContext::Layer
						&& options.stripLayerNames)));
		if (remove) {
			result = result.without(key);
			continue;
		}
		const auto childContext = (key == "layers"
				|| (context == OptimizeContext::Root && key == "assets"))
			? OptimizeContext::Layer
			: OptimizeContext::Other;
		auto optimized = OptimizeValue(member.value, options, childContext);
		if (!optimized.sameAs(member.value)) {
			result = result.with(key, std::move(optimized));
		}
	}
	return result;
}

[[nodiscard]] Value StripExpressions(const Value &value) {
	if (value.isArray()) {
		auto items = value.items();
		auto changed = false;
		for (auto &item : items) {
			auto stripped = StripExpressions(item);
			if (!stripped.sameAs(item)) {
				item = std::move(stripped);
				changed = true;
			}
		}
		return changed ? Value::FromArray(std::move(items)) : value;
	} else if (!value.isObject()) {
		return value;
	}
	auto result = value;
	if (value.get("x").isString() && value.has("k")) {
		result = result.without("x");
	}
	// A copy: result is replaced while its members are walked.
	const auto members = result.members();
	for (const auto &member : members) {
		if (member.value.isObject() || member.value.isArray()) {
			auto stripped = StripExpressions(member.value);
			if (!stripped.sameAs(member.value)) {
				result = result.with(member.key, std::move(stripped));
			}
		}
	}
	return result;
}

[[nodiscard]] bool HasExpressions(const Value &value, bool deep) {
	if (value.isArray()) {
		for (const auto &item : value.items()) {
			if (HasExpressions(item, deep)) {
				return true;
			}
		}
		return false;
	} else if (!value.isObject()) {
		return false;
	} else if (value.get("x").isString() && value.has("k")) {
		return true;
	}
	for (const auto &member : value.members()) {
		const auto &key = member.key;
		if (!deep
			&& (key == "shapes"
				|| key == "it"
				|| key == "masksProperties"
				|| key == "ef"
				|| key == "layers")) {
			continue;
		}
		if (HasExpressions(member.value, deep)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool HasBrokenKeyframes(const Value &value) {
	if (value.isArray()) {
		for (const auto &item : value.items()) {
			if (HasBrokenKeyframes(item)) {
				return true;
			}
		}
		return false;
	} else if (!value.isObject()) {
		return false;
	} else if (IsKeyframed(value)) {
		const auto &items = value.get("k").items();
		const auto count = int(items.size());
		for (auto i = 0; i != count; ++i) {
			const auto &item = items[i];
			const auto last = (i + 1 == count);
			const auto hold = (item.get("h").toNumber(0.) != 0.);
			if ((count == 1 && !hold)
				|| (count > 1 && last && item.has("i") && !hold)
				|| (!last && !hold && !item.has("i"))) {
				return true;
			}
		}
		return false;
	}
	for (const auto &member : value.members()) {
		const auto &key = member.key;
		if (key == "shapes"
			|| key == "it"
			|| key == "masksProperties"
			|| key == "ef"
			|| key == "layers") {
			continue;
		}
		if (HasBrokenKeyframes(member.value)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] Value FixBrokenKeyframes(const Value &value) {
	if (value.isArray()) {
		auto items = value.items();
		auto changed = false;
		for (auto &item : items) {
			auto fixed = FixBrokenKeyframes(item);
			if (!fixed.sameAs(item)) {
				item = std::move(fixed);
				changed = true;
			}
		}
		return changed ? Value::FromArray(std::move(items)) : value;
	} else if (!value.isObject()) {
		return value;
	} else if (IsKeyframed(value)) {
		if (!HasBrokenKeyframes(value)) {
			return value;
		}
		const auto type = InferType(value);
		return WriteModels(value, ReadModels(value, type), type);
	}
	auto result = value;
	for (const auto &member : value.members()) {
		if (member.value.isObject() || member.value.isArray()) {
			auto fixed = FixBrokenKeyframes(member.value);
			if (!fixed.sameAs(member.value)) {
				result = result.with(member.key, std::move(fixed));
			}
		}
	}
	return result;
}

[[nodiscard]] Value FlattenLayer(const Value &layer) {
	auto result = layer;
	if (result.get("ddd").toInt(0) != 0) {
		result = result.with("ddd", Number(0));
	}
	auto transform = result.get("ks");
	if (transform.isObject()) {
		if (transform.has("rz") && !transform.has("r")) {
			auto members = transform.members();
			for (auto &member : members) {
				if (member.key == "rz") {
					member.key = "r";
				}
			}
			auto renamed = Object(std::move(members));
			transform = renamed;
		}
		transform = transform.without("rx").without("ry").without("or");
		result = result.with("ks", transform);
	}
	return result;
}

[[nodiscard]] Value SolidToShape(const Value &layer) {
	const auto width = layer.get("sw").toDouble(0.);
	const auto height = layer.get("sh").toDouble(0.);
	const auto color = ParseHexColor(layer.get("sc").toString());
	auto rectangle = Object({
		{ "ty", Value::FromString(u"rc"_q) },
		{ "d", Number(1) },
		{ "s", StaticProperty(Numbers({ width, height })) },
		{ "p", StaticProperty(Numbers({ width / 2., height / 2. })) },
		{ "r", StaticProperty(Number(0)) },
		{ "nm", Value::FromString(u"Solid"_q) },
	});
	auto fill = Object({
		{ "ty", Value::FromString(u"fl"_q) },
		{ "c", StaticProperty(ColorNumbers(
			color.isValid() ? color : QColor(0, 0, 0))) },
		{ "o", StaticProperty(Number(100)) },
		{ "r", Number(1) },
		{ "nm", Value::FromString(u"Fill"_q) },
	});
	auto group = Object({
		{ "ty", Value::FromString(u"gr"_q) },
		{ "it", Value::FromArray({
			std::move(rectangle),
			std::move(fill),
			MakeTransform(QPointF(), false),
		}) },
		{ "nm", Value::FromString(u"Solid"_q) },
	});
	return layer
		.with("ty", Number(4))
		.without("sc")
		.without("sw")
		.without("sh")
		.with("shapes", Value::FromArray({ std::move(group) }));
}

[[nodiscard]] Value MapLayers(
		const Value &root,
		const Fn<Value(const Value&)> &map) {
	auto result = root;
	const auto mapList = [&](const Value &list) {
		auto items = list.items();
		auto changed = false;
		for (auto &layer : items) {
			if (!layer.isObject()) {
				continue;
			}
			auto mapped = map(layer);
			if (!mapped.sameAs(layer)) {
				layer = std::move(mapped);
				changed = true;
			}
		}
		return changed ? Value::FromArray(std::move(items)) : list;
	};
	result = result.with("layers", mapList(result.get("layers")));
	if (result.get("assets").isArray()) {
		auto assets = result.get("assets").items();
		auto changed = false;
		for (auto &asset : assets) {
			if (asset.get("layers").isArray()) {
				auto mapped = mapList(asset.get("layers"));
				if (!mapped.sameAs(asset.get("layers"))) {
					asset = asset.with("layers", std::move(mapped));
					changed = true;
				}
			}
		}
		if (changed) {
			result = result.with("assets", Value::FromArray(std::move(assets)));
		}
	}
	return result;
}

[[nodiscard]] Edit ApplyRoot(const Document &document, Value root) {
	auto mutation = Mutation(document);
	mutation.setRoot(std::move(root));
	mutation.changed(document.rootId());
	mutation.structural();
	return mutation.finish();
}

[[nodiscard]] bool IsUnsupportedShape(ShapeType type) {
	switch (type) {
	case ShapeType::RoundCorners:
	case ShapeType::OffsetPath:
	case ShapeType::PuckerBloat:
	case ShapeType::Twist:
	case ShapeType::ZigZag:
		return true;
	default:
		return false;
	}
}

// Round 4 checks, see IssueType.

[[nodiscard]] bool PropertyIsZero(const Value &property) {
	if (!property.isObject()) {
		return true;
	}
	auto zero = true;
	[[maybe_unused]] const auto scanned = MapStoredValues(property, [&](
			const Value &value,
			int,
			bool) {
		for (const auto number : value.numbers()) {
			if (number != 0.) {
				zero = false;
			}
		}
		return value;
	});
	return zero;
}

// rlottie draws add / subtract / intersect / difference, skips "n" on
// purpose and anything else by accident (a missing mode stays garbage).
[[nodiscard]] bool MaskModeIgnored(const Value &mask) {
	const auto text = mask.get("mode").toString();
	if (text.isEmpty()) {
		return true;
	}
	switch (text.at(0).unicode()) {
	case 'n':
	case 'a':
	case 's':
	case 'i':
	case 'f':
		return false;
	}
	return true;
}

[[nodiscard]] bool MaskModeIsNone(const Value &mask) {
	return mask.get("mode").toString().startsWith(QChar('n'));
}

[[nodiscard]] Value FixMaskMode(const Value &mask) {
	if (!MaskModeIgnored(mask)) {
		return mask;
	}
	const auto text = mask.get("mode").toString();
	const auto darken = !text.isEmpty() && (text.at(0) == QChar('d'));
	return mask.with("mode", Value::FromString(darken ? u"i"_q : u"a"_q));
}

// Mask opacity: rlottie always draws 100%.
[[nodiscard]] bool MaskOpacityIsFull(const Value &mask) {
	const auto &property = mask.get("o");
	if (!property.isObject()) {
		return true;
	}
	auto full = true;
	[[maybe_unused]] const auto scanned = MapStoredValues(property, [&](
			const Value &value,
			int,
			bool) {
		if (!Near(value.toNumber(100.), 100., 1e-3)) {
			full = false;
		}
		return value;
	});
	return full;
}

[[nodiscard]] bool MaskOptionsIgnored(const Value &mask) {
	return !MaskOpacityIsFull(mask)
		|| !PropertyIsZero(mask.get("x"))
		|| !PropertyIsZero(mask.get("f"));
}

[[nodiscard]] Value FixMaskOptions(const Value &mask) {
	auto result = mask;
	if (!MaskOpacityIsFull(mask)) {
		result = result.with("o", StaticProperty(Number(100)));
	}
	if (!PropertyIsZero(mask.get("x"))) {
		result = result.with("x", StaticProperty(Number(0)));
	}
	if (!PropertyIsZero(mask.get("f"))) {
		result = result.without("f");
	}
	return result;
}

// A mask that gives a shape in rlottie (the mode is drawn).
[[nodiscard]] bool MaskIsDrawn(const Value &mask) {
	return !MaskModeIsNone(mask) && !MaskModeIgnored(mask);
}

// Whether no mask before this one in the list gives a shape.
[[nodiscard]] bool MaskIsFirstDrawn(const Value &masks, int index) {
	for (auto i = 0; i < index; ++i) {
		if (MaskIsDrawn(masks.at(i))) {
			return false;
		}
	}
	return true;
}

// The mask with the mode that cuts the complement and without "inv",
// the same value if there is no such mode.
[[nodiscard]] Value InvertedMask(const Value &mask, bool first) {
	const auto mode = InvertedMaskMode(
		MaskModeFrom(mask.get("mode").toString()),
		first);
	if (!mode || !MaskIsDrawn(mask)) {
		return mask;
	}
	auto result = mask.with("mode", Value::FromString(MaskModeCode(*mode)));
	return result.has("inv")
		? result.with("inv", Value::FromBool(false))
		: result;
}

[[nodiscard]] bool PathVerticesDiffer(const Value &property) {
	if (!IsKeyframed(property)) {
		return false;
	}
	auto count = -1;
	auto differ = false;
	[[maybe_unused]] const auto scanned = MapStoredValues(property, [&](
			const Value &value,
			int,
			bool) {
		const auto &object = value.isArray() ? value.at(0) : value;
		const auto size = object.get("v").size();
		if (count < 0) {
			count = size;
		} else if (count != size) {
			differ = true;
		}
		return value;
	});
	return differ;
}

// rlottie's parseObject() skips every key of a shape item until it meets
// "ty". These keys are not read anyway (or mean the default).
[[nodiscard]] bool HarmlessBeforeType(const Member &member) {
	static constexpr auto kKeys = std::array{
		"nm", "mn", "ix", "cix", "np", "cl", "ln", "bm", "ind",
	};
	for (const auto key : kKeys) {
		if (member.key == QByteArrayView(key)) {
			return true;
		}
	}
	return (member.key == "hd" && !member.value.toBool(false))
		|| (member.key == "d"
			&& member.value.isNumber()
			&& member.value.toInt(1) == 1);
}

[[nodiscard]] bool ShapeKeyOrderBroken(const Value &shape) {
	for (const auto &member : shape.members()) {
		if (member.key == "ty") {
			return false;
		} else if (!HarmlessBeforeType(member)) {
			return shape.has("ty");
		}
	}
	return false;
}

[[nodiscard]] Value FixShapeKeyOrder(const Value &shape) {
	if (!shape.has("ty") || shape.indexOf("ty") == 0) {
		return shape;
	}
	const auto type = shape.get("ty");
	return shape.without("ty").withInserted("ty", type, 0);
}

// rlottie reads "ks" as a 3D transform (rotation in "rz", "r" ignored)
// unless "ddd": 0 stands before it.
[[nodiscard]] bool LayerKeyOrderBroken(const Value &layer) {
	const auto transformIndex = layer.indexOf("ks");
	const auto flagIndex = layer.indexOf("ddd");
	if (transformIndex < 0 || (flagIndex >= 0 && flagIndex < transformIndex)) {
		return false;
	}
	const auto &transform = layer.get("ks");
	return transform.has("r")
		&& !transform.has("rz")
		&& !PropertyIsZero(transform.get("r"));
}

[[nodiscard]] Value FixLayerKeyOrder(const Value &layer) {
	if (!LayerKeyOrderBroken(layer)) {
		return layer;
	}
	const auto flag = layer.has("ddd") ? layer.get("ddd") : Number(0);
	return layer.without("ddd").withInserted("ddd", flag, 0);
}

// NodeInfo::parentLayer has only the links rlottie accepts.
[[nodiscard]] bool ParentLinkBroken(const Document &, const NodeInfo &node) {
	return node.parentInd.has_value() && !node.parentLayer;
}

// Values rlottie never returns from (checked in its sources and by
// rendering): VBezier::tAtLength() loops forever for a negative length,
// which it gets from a motion path keyframe ("ti" / "to") whose easing
// goes below zero and from negative trim values; VDasher never ends with
// negative dashes; repeater copies and star points are loop counters.
// MadeRenderSafe() clamps exactly these values and nothing else.
//
// The easing rlottie draws a keyframe with is not always the keyframe's
// own: it keeps one curve per keyframe name ("n"), without a name one per
// handles printed as "%.2f_%.2f_%.2f_%.2f" (cut at 19 characters), and
// every keyframe gets the curve that came first in the file under its
// key. So a keyframe that has to be safe may not carry a name that stands
// for another curve as well, and the handles of a motion path keyframe
// are kept in a range where every curve that prints the same is safe too:
// at zero or above (a handle a bit below zero prints as "-0.00", and so
// would a zero with a minus sign, which the parser reads as a plain zero)
// and not so large that the end of the key is cut off.
[[nodiscard]] Value ClampedNumbers(const Value &value, double min, double max) {
	return MapNumbers(value, [&](std::vector<double> numbers) {
		for (auto &number : numbers) {
			number = std::isfinite(number)
				? std::clamp(number, min, max)
				: min;
		}
		return numbers;
	});
}

// rlottie reads a property as keyframes if "k" is a list that starts with
// an object, with or without a "t" in it.
[[nodiscard]] bool RendererKeyframed(const Value &property) {
	const auto &k = property.get("k");
	return k.isArray() && !k.items().empty() && k.items().front().isObject();
}

// With larger handles the sign of the last one would not fit into
// rlottie's curve key any more.
constexpr auto kMaxPathEasing = 1000.;

// Handles in x [0, 1] (the renderer's solver is made for that range, the
// progress it returns for others can be anything) and y [min, max]. A
// changed keyframe loses its name: by the name rlottie could find the
// curve as it was.
[[nodiscard]] Value ClampedEasing(
		const Value &keyframe,
		double min,
		double max) {
	auto result = keyframe;
	for (const auto key : { "o", "i" }) {
		const auto &control = keyframe.get(key);
		if (!control.isObject()) {
			continue;
		}
		auto safe = control;
		const auto limit = [&](const char *axis, double low, double high) {
			const auto &value = control.get(axis);
			auto clamped = ClampedNumbers(value, low, high);
			if (!clamped.sameAs(value)) {
				safe = safe.with(axis, std::move(clamped));
			}
		};
		limit("x", 0., 1.);
		limit("y", min, max);
		if (!safe.sameAs(control)) {
			result = result.with(key, std::move(safe));
		}
	}
	return result.sameAs(keyframe) ? result : result.without("n");
}

[[nodiscard]] std::string EasingName(const Value &keyframe) {
	const auto &name = keyframe.get("n");
	if (name.isString()) {
		return name.toString().toStdString();
	}
	for (const auto &item : name.items()) {
		if (item.isString() && !item.toString().isEmpty()) {
			return item.toString().toStdString();
		}
	}
	return std::string();
}

// The key rlottie gives the curve of a keyframe without a name.
[[nodiscard]] std::string EasingValueKey(const Value &keyframe) {
	const auto single = [](double value) {
		constexpr auto kMax = double(std::numeric_limits<float>::max());
		return (value > kMax)
			? std::numeric_limits<double>::infinity()
			: (value < -kMax)
			? -std::numeric_limits<double>::infinity()
			: double(float(value));
	};
	const auto in = ReadControlPoint(keyframe.get("i"));
	const auto out = ReadControlPoint(keyframe.get("o"));
	char buffer[20] = { 0 };
	std::snprintf(
		buffer,
		sizeof(buffer),
		"%.2f_%.2f_%.2f_%.2f",
		single(in.x()),
		single(in.y()),
		single(out.x()),
		single(out.y()));
	return std::string(buffer);
}

void CollectEasingNames(
		const Value &value,
		std::unordered_map<std::string, std::string> &keys,
		EasingNames &mixed,
		int depth) {
	if (depth > Json::kMaxDepth) {
		return;
	} else if (value.isArray()) {
		for (const auto &item : value.items()) {
			if (item.isObject() || item.isArray()) {
				CollectEasingNames(item, keys, mixed, depth + 1);
			}
		}
		return;
	} else if (!value.isObject()) {
		return;
	}
	const auto keyframed = RendererKeyframed(value);
	if (keyframed) {
		for (const auto &item : value.get("k").items()) {
			if (!item.isObject() || !item.has("i") || !item.has("n")) {
				continue;
			}
			auto name = EasingName(item);
			if (name.empty()) {
				continue;
			}
			auto key = EasingValueKey(item);
			const auto i = keys.find(name);
			if (i == end(keys)) {
				keys.emplace(std::move(name), std::move(key));
			} else if (i->second != key) {
				mixed.insert(std::move(name));
			}
		}
	}
	for (const auto &member : value.members()) {
		if ((member.value.isObject() || member.value.isArray())
			&& !(keyframed && member.key == "k")) {
			CollectEasingNames(member.value, keys, mixed, depth + 1);
		}
	}
}

// The names that different curves share. A file made by an exporter has
// none: there a name is made of the handles.
[[nodiscard]] EasingNames MixedEasingNames(const Value &root) {
	auto keys = std::unordered_map<std::string, std::string>();
	auto result = EasingNames();
	CollectEasingNames(root, keys, result, 0);
	return result;
}

// The lowest and the highest progress of an easing curve
// y(t) = 3 (1 - t)^2 t y1 + 3 (1 - t) t^2 y2 + t^3 on [0, 1].
[[nodiscard]] std::pair<double, double> EasingExtremes(QPointF out, QPointF in) {
	const auto y1 = out.y();
	const auto y2 = in.y();
	auto low = 0.;
	auto high = 1.;
	const auto consider = [&](double t) {
		if (!(t > 0.) || !(t < 1.)) {
			return;
		}
		const auto u = 1. - t;
		const auto y = 3. * u * u * t * y1 + 3. * u * t * t * y2 + t * t * t;
		low = std::min(low, y);
		high = std::max(high, y);
	};
	// y'(t) / 3 = a t^2 + b t + c.
	const auto a = 3. * y1 - 3. * y2 + 1.;
	const auto b = 2. * y2 - 4. * y1;
	const auto c = y1;
	if (std::abs(a) < 1e-12) {
		if (std::abs(b) > 1e-12) {
			consider(-c / b);
		}
	} else if (const auto d = b * b - 4. * a * c; d >= 0.) {
		const auto root = std::sqrt(d);
		consider((-b + root) / (2. * a));
		consider((-b - root) / (2. * a));
	}
	return { low, high };
}

// strict: every stored value is kept in [min, max], and a segment whose
// easing would take the value out of that range gets its easing limited
// to [0, 1]. Otherwise only keyframes with motion path tangents are
// looked at: their easing handles may not be below zero. In both cases
// such a keyframe may not have one of the mixed names.
//
// Hold keyframes ("h") are checked like the others: they do not use
// their easing, and rlottie takes less for a hold than this reader does.
[[nodiscard]] Value SafeAnimatable(
		const Value &property,
		bool strict,
		double min,
		double max,
		const EasingNames *mixed) {
	if (!property.isObject()) {
		return property;
	}
	const auto keyframed = RendererKeyframed(property);
	auto result = strict
		? MapStoredValues(property, keyframed, [&](
				const Value &value,
				int,
				bool) {
			return ClampedNumbers(value, min, max);
		})
		: property;
	if (!keyframed) {
		return result;
	}
	constexpr auto kEpsilon = 1e-6;
	auto items = result.get("k").items();
	const auto count = int(items.size());
	auto changed = false;
	for (auto i = 0; i != count; ++i) {
		if (!items[i].isObject() || !items[i].has("i")) {
			continue;
		}
		const auto path = items[i].has("ti") || items[i].has("to");
		if (!path && !strict) {
			continue;
		}
		auto item = path
			? ClampedEasing(items[i], 0., kMaxPathEasing)
			: ClampedEasing(items[i], -kNoLimit, kNoLimit);
		if (strict) {
			const auto [low, high] = EasingExtremes(
				ReadControlPoint(item.get("o")),
				ReadControlPoint(item.get("i")));
			const auto from = item.get("s").numbers();
			const auto to = item.has("e")
				? item.get("e").numbers()
				: (i + 1 < count)
				? items[i + 1].get("s").numbers()
				: std::vector<double>();
			auto leaves = false;
			const auto size = std::min(from.size(), to.size());
			for (auto j = size_t(0); j < size; ++j) {
				const auto delta = to[j] - from[j];
				const auto lowest = from[j]
					+ std::min({ low * delta, high * delta, 0., delta });
				const auto highest = from[j]
					+ std::max({ low * delta, high * delta, 0., delta });
				if (lowest < min - kEpsilon || highest > max + kEpsilon) {
					leaves = true;
				}
			}
			if (leaves) {
				item = ClampedEasing(item, 0., 1.);
			}
		}
		if (mixed
			&& !mixed->empty()
			&& item.has("n")
			&& mixed->contains(EasingName(item))) {
			item = item.without("n");
		}
		if (!item.sameAs(items[i])) {
			items[i] = std::move(item);
			changed = true;
		}
	}
	return changed
		? result.with("k", Value::FromArray(std::move(items)))
		: result;
}

// The lowest and the highest stored value of a property.
[[nodiscard]] std::pair<double, double> StoredRange(const Value &property) {
	auto low = std::numeric_limits<double>::infinity();
	auto high = -std::numeric_limits<double>::infinity();
	[[maybe_unused]] const auto scanned = MapStoredValues(
		property,
		RendererKeyframed(property),
		[&](const Value &value, int, bool) {
			for (const auto number : value.numbers()) {
				low = std::min(low, number);
				high = std::max(high, number);
			}
			return value;
		});
	return (low <= high) ? std::make_pair(low, high) : std::make_pair(0., 0.);
}

// Stroke dashes the way rlottie reads them: the items that have a "v", in
// their order, the last one is the offset (any value is fine there) and
// the others are lengths: dash, gap, dash, gap... A negative length never
// ends on a curve, and a pattern shorter than about a thousandth of the
// curve it runs on stalls for minutes (measured: 0.2 on a 480 px circle,
// 0.5 on a 2000 px one), so the gaps are kept wide enough.
[[nodiscard]] Value SafeDashes(const Value &list, const EasingNames *mixed) {
	auto items = list.items();
	auto lengths = std::vector<int>();
	for (auto i = 0; i != int(items.size()); ++i) {
		if (items[i].get("v").isObject()) {
			lengths.push_back(i);
		}
	}
	if (lengths.size() < 2) {
		return list;
	}
	lengths.pop_back();
	auto changed = false;
	const auto clamp = [&](int index, double min) {
		const auto &length = items[index].get("v");
		auto safe = SafeAnimatable(length, true, min, kNoLimit, mixed);
		if (!safe.sameAs(length)) {
			items[index] = items[index].with("v", std::move(safe));
			changed = true;
		}
	};
	for (const auto index : lengths) {
		clamp(index, 0.);
	}
	// With an odd number of lengths rlottie takes the last dash as its
	// gap as well.
	auto period = 0.;
	auto dashes = false;
	auto gaps = false;
	const auto count = int(lengths.size());
	for (auto i = 0; i != count; ++i) {
		const auto [low, high] = StoredRange(items[lengths[i]].get("v"));
		const auto twice = (i + 1 == count) && (count % 2);
		period += twice ? (low * 2.) : low;
		((i % 2) ? gaps : dashes) |= (high > 0.);
		gaps |= twice && (high > 0.);
	}
	if (dashes && gaps && period < kMinDashPeriod) {
		if (count > 1) {
			for (auto i = 1; i < count; i += 2) {
				clamp(lengths[i], kMinDashPeriod);
			}
		} else {
			clamp(lengths[0], kMinDashPeriod / 2.);
		}
	}
	return changed ? Value::FromArray(std::move(items)) : list;
}

// The lowest trim start / end (percent) that is safe with the offset of
// the item, measured on rlottie: with an offset above zero the values
// may go as low as minus the offset, otherwise down to -100% minus the
// (negative) offset. An animated offset can be anything: nothing below
// zero then. Zero and above is fine with any offset.
[[nodiscard]] double TrimLowerBound(const Value &trim) {
	constexpr auto kMargin = 0.01;
	const auto &offset = trim.get("o");
	if (!offset.isObject()) {
		return -100. + kMargin;
	} else if (RendererKeyframed(offset)) {
		return 0.;
	}
	const auto part = std::fmod(offset.get("k").toNumber(0.), 360.) / 360.;
	const auto bound = (part > 0.) ? (-100. * part) : (-100. * (1. + part));
	return std::min(bound + kMargin, 0.);
}

Value SafeShapeItem(const Value &item, const EasingNames *mixed) {
	auto result = item;
	const auto strict = [&](const char *key, double min, double max) {
		const auto &property = result.get(key);
		auto safe = SafeAnimatable(property, true, min, max, mixed);
		if (!safe.sameAs(property)) {
			result = result.with(key, std::move(safe));
		}
	};
	const auto type = item.get("ty").toString();
	if (type == u"tm"_q) {
		const auto bound = TrimLowerBound(item);
		strict("s", bound, kNoLimit);
		strict("e", bound, kNoLimit);
	} else if (type == u"rp"_q) {
		strict("c", -kNoLimit, kMaxRepeaterCopies);
	} else if (type == u"sr"_q) {
		strict("pt", -kNoLimit, kMaxStarPoints);
	} else if ((type == u"st"_q || type == u"gs"_q)
		&& item.get("d").isArray()) {
		const auto &list = item.get("d");
		auto safe = SafeDashes(list, mixed);
		if (!safe.sameAs(list)) {
			result = result.with("d", std::move(safe));
		}
	}
	return result;
}

// mixed: MixedEasingNames() of the whole file the value is from.
[[nodiscard]] Value MadeRenderSafe(
		const Value &value,
		const EasingNames &mixed,
		int depth = 0) {
	if (depth > Json::kMaxDepth) {
		return value;
	} else if (value.isArray()) {
		auto items = value.items();
		auto changed = false;
		for (auto &item : items) {
			if (!item.isObject() && !item.isArray()) {
				continue;
			}
			auto safe = MadeRenderSafe(item, mixed, depth + 1);
			if (!safe.sameAs(item)) {
				item = std::move(safe);
				changed = true;
			}
		}
		return changed ? Value::FromArray(std::move(items)) : value;
	} else if (!value.isObject()) {
		return value;
	}
	// An object with keyframes in "k" is not always a property: whatever
	// else it has is walked as well, so that a stray "k" in a layer or in
	// a shape hides nothing.
	const auto keyframed = RendererKeyframed(value);
	auto result = keyframed
		? SafeAnimatable(value, false, -kNoLimit, kNoLimit, &mixed)
		: value;
	if (result.get("ty").isString()) {
		result = SafeShapeItem(result, &mixed);
	}
	// A copy: result is replaced while its members are walked.
	const auto members = result.members();
	for (const auto &member : members) {
		if ((!member.value.isObject() && !member.value.isArray())
			|| (keyframed && member.key == "k")) {
			continue;
		}
		auto safe = MadeRenderSafe(member.value, mixed, depth + 1);
		if (!safe.sameAs(member.value)) {
			result = result.with(member.key, std::move(safe));
		}
	}
	return result;
}

[[nodiscard]] Value MadeRenderSafe(const Value &root) {
	return MadeRenderSafe(root, MixedEasingNames(root));
}

[[nodiscard]] Edit MapNodes(
		const Document &document,
		const std::vector<NodeId> &ids,
		const Fn<Value(const NodeInfo&, const Value&)> &map) {
	auto mutation = Mutation(document);
	for (const auto id : ids) {
		const auto node = mutation.node(id);
		if (!node) {
			continue;
		}
		const auto &json = mutation.nodeJson(id);
		auto mapped = map(*node, json);
		if (!mapped.sameAs(json)) {
			mutation.setNode(id, std::move(mapped));
		}
	}
	return mutation.finish();
}

[[nodiscard]] int64 PackedSize(const Document &document, bool *estimated) {
	const auto json = document.toJson();
	auto packed = Oblivion::Lottie::PackTgs(json);
	if (packed.isEmpty()) {
		packed = GzipFallback(json);
		if (estimated) {
			*estimated = true;
		}
	}
	return packed.size();
}

[[nodiscard]] bool TouchesEdge(const QImage &image) {
	if (image.isNull()) {
		return false;
	}
	const auto frame = image.convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
	const auto width = frame.width();
	const auto height = frame.height();
	const auto alpha = [&](int x, int y) {
		return qAlpha(reinterpret_cast<const QRgb*>(
			frame.constScanLine(y))[x]);
	};
	constexpr auto kThreshold = 24;
	for (auto x = 0; x != width; ++x) {
		if (alpha(x, 0) > kThreshold || alpha(x, height - 1) > kThreshold) {
			return true;
		}
	}
	for (auto y = 0; y != height; ++y) {
		if (alpha(0, y) > kThreshold || alpha(width - 1, y) > kThreshold) {
			return true;
		}
	}
	return false;
}

// An undo history version (EditorController::Step) as a document again.
[[nodiscard]] Document HistoryDocument(const Value &root) {
	return root.isNull() ? Document() : Document(root);
}

} // namespace

QByteArray Document::toRenderJson() const {
	if (!valid()) {
		return QByteArray();
	}
	const auto data = _data.get();
	std::call_once(data->renderOnce, [&] {
		const auto safe = MadeRenderSafe(data->root);
		data->renderJson = safe.sameAs(data->root)
			? toJson()
			: Json::Serialize(safe);
	});
	return data->renderJson;
}

QByteArray RenderSafeJson(const QByteArray &json) {
	auto rewritten = false;
	const auto parsed = Json::ParseRoot(json, &rewritten);
	if (!parsed) {
		return QByteArray();
	}
	const auto safe = MadeRenderSafe(*parsed);
	return (safe.sameAs(*parsed) && !rewritten)
		? json
		: Json::Serialize(safe);
}

std::optional<MaskMode> InvertedMaskMode(MaskMode mode, bool first) {
	switch (mode) {
	case MaskMode::Add:
	case MaskMode::Difference:
		return first
			? std::make_optional(MaskMode::Subtract)
			: std::nullopt;
	case MaskMode::Subtract:
		return first ? MaskMode::Add : MaskMode::Intersect;
	case MaskMode::Intersect:
		return MaskMode::Subtract;
	case MaskMode::None:
	case MaskMode::Lighten:
	case MaskMode::Darken:
		break;
	}
	return std::nullopt;
}

Edit InvertMask(const Document &document, NodeId mask) {
	auto mutation = Mutation(document);
	const auto node = mutation.node(mask);
	if (!node || node->kind != NodeKind::Mask) {
		return Failed(u"InvertMask: mask not found"_q);
	}
	const auto &json = mutation.nodeJson(mask);
	const auto &list = mutation.nodeJson(node->layer).get("masksProperties");
	auto inverted = InvertedMask(json, MaskIsFirstDrawn(list, node->index));
	if (inverted.sameAs(json)) {
		return Failed(u"InvertMask: no mode gives the inverted shape"_q);
	}
	mutation.setNode(mask, std::move(inverted));
	return mutation.finish();
}

Edit Optimize(const Document &document, const OptimizeOptions &options) {
	auto root = OptimizeValue(
		document.root(),
		options,
		OptimizeContext::Root);
	return root.sameAs(document.root())
		? Unchanged(document)
		: ApplyRoot(document, std::move(root));
}

ValidationResult Validate(
		const Document &document,
		const ValidateOptions &options) {
	auto result = ValidationResult();
	if (!document.valid()) {
		result.issues.push_back({ IssueType::InvalidComposition });
		return result;
	}
	const auto &root = document.root();
	const auto rootId = document.rootId();
	const auto add = [&](
			IssueType type,
			IssueSeverity severity,
			std::vector<NodeId> nodes,
			bool fixable,
			double value = 0.) {
		result.issues.push_back({
			type,
			severity,
			std::move(nodes),
			fixable,
			value,
			CategoryOf(type),
			RenderedByTelegram(type),
			FixChangesPicture(type),
		});
	};
	const auto size = document.size();
	const auto fps = document.frameRate();
	if (size.isEmpty()
		|| fps <= 0.
		|| document.frames() <= 0) {
		add(IssueType::InvalidComposition, IssueSeverity::Error, {}, false);
	}
	if (!root.get("v").isString()) {
		add(IssueType::MissingVersion, IssueSeverity::Error, {}, true);
	}
	if (size.width() != kTgsCanvasSize || size.height() != kTgsCanvasSize) {
		add(
			IssueType::CanvasSize,
			IssueSeverity::Error,
			{},
			!size.isEmpty(),
			size.width());
	}
	if (fps > 0. && !Near(fps, 30., 0.01) && !Near(fps, 60., 0.01)) {
		add(IssueType::FrameRate, IssueSeverity::Error, {}, true, fps);
	}
	const auto seconds = (fps > 0.) ? (document.frames() / fps) : 0.;
	if (seconds > kTgsMaxDuration + 1e-6) {
		add(IssueType::Duration, IssueSeverity::Error, {}, true, seconds);
	}

	auto images = std::vector<NodeId>();
	auto texts = std::vector<NodeId>();
	auto layers3d = std::vector<NodeId>();
	auto expressions = std::vector<NodeId>();
	auto broken = std::vector<NodeId>();
	auto masks = std::vector<NodeId>();
	auto effects = std::vector<NodeId>();
	auto solids = std::vector<NodeId>();
	auto stretched = std::vector<NodeId>();
	auto remapped = std::vector<NodeId>();
	auto merges = std::vector<NodeId>();
	auto unsupported = std::vector<NodeId>();
	auto repeaters = std::vector<NodeId>();
	auto stars = std::vector<NodeId>();
	auto gradientStrokes = std::vector<NodeId>();
	auto autoOriented = std::vector<NodeId>();
	auto mattes = std::vector<NodeId>();
	auto brokenMattes = std::vector<NodeId>();
	auto matteLinks = std::vector<NodeId>();
	auto masksOff = std::vector<NodeId>();
	auto maskModes = std::vector<NodeId>();
	auto maskOptions = std::vector<NodeId>();
	auto maskInverted = std::vector<NodeId>();
	auto invertible = false;
	auto pathVertices = std::vector<NodeId>();
	auto keyOrder = std::vector<NodeId>();
	auto parentLinks = std::vector<NodeId>();
	if (root.get("ddd").toInt(0) == 1) {
		layers3d.push_back(rootId);
	}
	const auto mixed = MixedEasingNames(root);
	const auto unsafe = !MadeRenderSafe(root, mixed).sameAs(root);
	auto hangs = std::vector<NodeId>();
	for (const auto &node : document.nodes()) {
		const auto &json = document.json(node.id);
		if (node.kind != NodeKind::Composition
			&& node.kind != NodeKind::Asset) {
			if (HasExpressions(json, false)) {
				expressions.push_back(node.id);
			}
			if (HasBrokenKeyframes(json)) {
				broken.push_back(node.id);
			}
			if (unsafe) {
				// The node's own values, without the nodes inside it.
				auto own = json;
				for (const auto key : {
						"shapes",
						"it",
						"masksProperties",
						"ef",
						"layers" }) {
					own = own.without(key);
				}
				if (!MadeRenderSafe(own, mixed).sameAs(own)) {
					hangs.push_back(node.id);
				}
			}
		}
		switch (node.kind) {
		case NodeKind::Asset:
			if (node.imageAsset) {
				images.push_back(node.id);
			}
			break;
		case NodeKind::Layer:
			switch (node.layerType) {
			case LayerType::Image: images.push_back(node.id); break;
			case LayerType::Text: texts.push_back(node.id); break;
			case LayerType::Solid: solids.push_back(node.id); break;
			default: break;
			}
			if (node.is3d) {
				layers3d.push_back(node.id);
			}
			if (!node.masks.empty()) {
				masks.push_back(node.id);
				// "None" masks are meant to do nothing, but rlottie does
				// not draw a layer whose masks give no shape at all.
				auto meant = false;
				auto drawn = false;
				for (const auto id : node.masks) {
					const auto &mask = document.json(id);
					meant = meant || !MaskModeIsNone(mask);
					drawn = drawn
						|| (!MaskModeIsNone(mask) && !MaskModeIgnored(mask));
				}
				if (!node.masksEnabled && meant) {
					masksOff.push_back(node.id);
				} else if (node.masksEnabled && !drawn) {
					for (const auto id : node.masks) {
						if (MaskModeIsNone(document.json(id))) {
							maskModes.push_back(id);
						}
					}
				}
			}
			if (!node.effects.empty()) {
				effects.push_back(node.id);
			}
			if (!Near(node.stretch, 1.)) {
				stretched.push_back(node.id);
			}
			if (node.hasTimeRemap) {
				remapped.push_back(node.id);
			}
			if (json.get("ao").toInt(0) == 1) {
				autoOriented.push_back(node.id);
			}
			if (node.matte != MatteMode::None) {
				mattes.push_back(node.id);
				const auto source = document.node(node.matteLayer);
				if (!source) {
					brokenMattes.push_back(node.id);
				} else if (node.matteParentInd
					&& source->ind != node.matteParentInd) {
					matteLinks.push_back(node.id);
				}
			}
			if (LayerKeyOrderBroken(json)) {
				keyOrder.push_back(node.id);
			}
			if (ParentLinkBroken(document, node)) {
				parentLinks.push_back(node.id);
			}
			break;
		case NodeKind::Mask:
			if (MaskModeIgnored(json)) {
				maskModes.push_back(node.id);
			}
			if (MaskOptionsIgnored(json)) {
				maskOptions.push_back(node.id);
			}
			if (node.maskInverted && MaskIsDrawn(json)) {
				maskInverted.push_back(node.id);
				const auto &list = document.json(node.layer).get(
					"masksProperties");
				if (InvertedMaskMode(
						node.maskMode,
						MaskIsFirstDrawn(list, node.index))) {
					invertible = true;
				}
			}
			if (PathVerticesDiffer(json.get("pt"))) {
				pathVertices.push_back(node.id);
			}
			break;
		case NodeKind::Shape:
			if (ShapeKeyOrderBroken(json)) {
				keyOrder.push_back(node.id);
			}
			switch (node.shapeType) {
			case ShapeType::MergePaths: merges.push_back(node.id); break;
			case ShapeType::Repeater: repeaters.push_back(node.id); break;
			case ShapeType::Star: stars.push_back(node.id); break;
			case ShapeType::GradientStroke:
				gradientStrokes.push_back(node.id);
				break;
			case ShapeType::Path:
				if (PathVerticesDiffer(json.get("ks"))) {
					pathVertices.push_back(node.id);
				}
				break;
			default:
				if (IsUnsupportedShape(node.shapeType)) {
					unsupported.push_back(node.id);
				}
				break;
			}
			break;
		default: break;
		}
	}
	const auto addList = [&](
			IssueType type,
			IssueSeverity severity,
			std::vector<NodeId> &list,
			bool fixable) {
		if (!list.empty()) {
			add(type, severity, std::move(list), fixable);
		}
	};
	constexpr auto kError = IssueSeverity::Error;
	constexpr auto kWarning = IssueSeverity::Warning;
	addList(IssueType::Images, kError, images, true);
	addList(IssueType::Expressions, kError, expressions, true);
	addList(IssueType::Layers3D, kError, layers3d, true);
	addList(IssueType::TextLayers, kError, texts, true);
	addList(IssueType::BrokenKeyframes, kError, broken, true);
	if (unsafe) {
		add(IssueType::RendererHang, kError, std::move(hangs), true);
	}
	if (options.measureSize) {
		auto estimated = false;
		result.packedSize = PackedSize(document, &estimated);
		result.packedSizeEstimated = estimated;
		if (result.packedSize > kTgsMaxPackedSize) {
			add(
				IssueType::FileSize,
				kError,
				{},
				true,
				double(result.packedSize));
		}
	}
	if (!root.has("tgs")) {
		add(IssueType::MissingTgsMarker, kWarning, {}, true);
	}
	addList(IssueType::Masks, kWarning, masks, false);
	addList(IssueType::Effects, kWarning, effects, true);
	addList(IssueType::Solids, kWarning, solids, true);
	addList(IssueType::TimeStretch, kWarning, stretched, false);
	addList(IssueType::TimeRemap, kWarning, remapped, false);
	addList(IssueType::MergePaths, kWarning, merges, true);
	addList(IssueType::UnsupportedShapes, kWarning, unsupported, true);
	addList(IssueType::Repeaters, kWarning, repeaters, false);
	addList(IssueType::StarShapes, kWarning, stars, false);
	addList(IssueType::GradientStrokes, kWarning, gradientStrokes, false);
	addList(IssueType::AutoOrient, kWarning, autoOriented, true);
	addList(IssueType::BrokenMattes, kWarning, brokenMattes, false);
	addList(IssueType::MatteLinks, kWarning, matteLinks, false);
	addList(IssueType::MasksOff, kWarning, masksOff, true);
	addList(IssueType::MaskModes, kWarning, maskModes, true);
	addList(IssueType::MaskOptions, kWarning, maskOptions, true);
	addList(IssueType::MaskInverted, kWarning, maskInverted, invertible);
	addList(IssueType::PathVertices, kWarning, pathVertices, false);
	addList(IssueType::KeyOrder, kWarning, keyOrder, true);
	addList(IssueType::ParentLinks, kWarning, parentLinks, true);
	addList(IssueType::TrackMattes, kWarning, mattes, false);
	if (options.renderChecks) {
		const auto json = document.toJson();
		auto renderer = Oblivion::Lottie::Renderer(json);
		if (renderer.valid()) {
			const auto frames = renderer.info().frames;
			const auto step = std::max(1, frames / 24);
			for (auto frame = 0; frame < frames; frame += step) {
				if (TouchesEdge(renderer.render(frame, QSize(128, 128)))) {
					add(IssueType::OutOfCanvas, kWarning, {}, false);
					break;
				}
			}
		}
	}
	std::stable_sort(
		begin(result.issues),
		end(result.issues),
		[](const Issue &a, const Issue &b) {
			return (a.severity == IssueSeverity::Error)
				&& (b.severity != IssueSeverity::Error);
		});
	return result;
}

Edit AutoFix(const Document &document, const std::vector<IssueType> &types) {
	// "Fix everything" never changes what Telegram shows, such fixes have
	// to be asked for by type.
	const auto wants = [&](IssueType type) {
		return types.empty()
			? !FixChangesPicture(type)
			: ranges::contains(types, type);
	};
	const auto validation = Validate(document, { .measureSize = false });
	const auto present = [&](IssueType type) {
		return wants(type) && ranges::any_of(
			validation.issues,
			[&](const Issue &issue) { return issue.type == type; });
	};
	const auto nodesOf = [&](IssueType type) {
		auto result = std::vector<NodeId>();
		for (const auto &issue : validation.issues) {
			if (issue.type == type) {
				result.insert(end(result), begin(issue.nodes), end(issue.nodes));
			}
		}
		return result;
	};
	auto current = document;
	auto changed = std::vector<NodeId>();
	auto removed = std::vector<NodeId>();
	const auto step = [&](Edit &&edit) {
		if (edit.ok() && !edit.document.sameAs(current)) {
			changed.insert(end(changed), begin(edit.changed), end(edit.changed));
			removed.insert(end(removed), begin(edit.removed), end(edit.removed));
			current = std::move(edit.document);
		}
	};
	if (present(IssueType::RendererHang)) {
		step(ApplyRoot(current, MadeRenderSafe(current.root())));
	}
	if (present(IssueType::MissingVersion)) {
		step(ApplyRoot(current, current.root().withInserted(
			"v",
			Value::FromString(u"5.5.2"_q),
			current.root().has("tgs") ? 1 : 0)));
	}
	if (present(IssueType::Images) || present(IssueType::TextLayers)) {
		auto ids = std::vector<NodeId>();
		if (present(IssueType::Images)) {
			ids = nodesOf(IssueType::Images);
		}
		if (present(IssueType::TextLayers)) {
			const auto texts = nodesOf(IssueType::TextLayers);
			ids.insert(end(ids), begin(texts), end(texts));
		}
		step(DeleteNodes(current, ids));
		if (present(IssueType::TextLayers)) {
			auto root = current.root().without("fonts").without("chars");
			if (!root.sameAs(current.root())) {
				step(ApplyRoot(current, std::move(root)));
			}
		}
	}
	if (present(IssueType::Expressions)) {
		step(ApplyRoot(current, StripExpressions(current.root())));
	}
	if (present(IssueType::Layers3D)) {
		auto root = MapLayers(current.root(), FlattenLayer);
		if (root.get("ddd").toInt(0) != 0) {
			root = root.with("ddd", Number(0));
		}
		step(ApplyRoot(current, std::move(root)));
	}
	if (present(IssueType::Effects)) {
		step(ApplyRoot(current, MapLayers(current.root(), [](const Value &layer) {
			return layer.without("ef");
		})));
	}
	if (present(IssueType::AutoOrient)) {
		step(ApplyRoot(current, MapLayers(current.root(), [](const Value &layer) {
			return (layer.get("ao").toInt(0) == 1)
				? layer.with("ao", Number(0))
				: layer;
		})));
	}
	if (present(IssueType::Solids)) {
		step(ApplyRoot(current, MapLayers(current.root(), [](const Value &layer) {
			return (layer.get("ty").toInt(-1) == 1)
				? SolidToShape(layer)
				: layer;
		})));
	}
	if (present(IssueType::KeyOrder)) {
		step(MapNodes(current, nodesOf(IssueType::KeyOrder), [](
				const NodeInfo &node,
				const Value &json) {
			return (node.kind == NodeKind::Layer)
				? FixLayerKeyOrder(json)
				: (node.kind == NodeKind::Shape)
				? FixShapeKeyOrder(json)
				: json;
		}));
	}
	if (present(IssueType::MasksOff)) {
		step(MapNodes(current, nodesOf(IssueType::MasksOff), [](
				const NodeInfo &node,
				const Value &json) {
			return (node.kind == NodeKind::Layer)
				? json.with("hasMask", Value::FromBool(true))
				: json;
		}));
	}
	if (present(IssueType::MaskModes)) {
		step(MapNodes(current, nodesOf(IssueType::MaskModes), [](
				const NodeInfo &node,
				const Value &json) {
			return (node.kind == NodeKind::Mask) ? FixMaskMode(json) : json;
		}));
		// Layers with nothing but "none" masks are drawn without masks.
		auto hidden = std::vector<NodeId>();
		for (const auto &node : current.nodes()) {
			if (node.kind == NodeKind::Layer
				&& node.masksEnabled
				&& !node.masks.empty()
				&& ranges::all_of(node.masks, [&](NodeId id) {
					return MaskModeIsNone(current.json(id));
				})) {
				hidden.push_back(node.id);
			}
		}
		step(MapNodes(current, hidden, [](
				const NodeInfo &,
				const Value &json) {
			return json.with("hasMask", Value::FromBool(false));
		}));
	}
	if (present(IssueType::MaskOptions)) {
		step(MapNodes(current, nodesOf(IssueType::MaskOptions), [](
				const NodeInfo &node,
				const Value &json) {
			return (node.kind == NodeKind::Mask)
				? FixMaskOptions(json)
				: json;
		}));
	}
	if (present(IssueType::MaskInverted)) {
		// Whole lists: a mask is "first" by the masks before it.
		auto layers = std::vector<NodeId>();
		for (const auto id : nodesOf(IssueType::MaskInverted)) {
			const auto mask = current.node(id);
			if (mask && !ranges::contains(layers, mask->layer)) {
				layers.push_back(mask->layer);
			}
		}
		step(MapNodes(current, layers, [](
				const NodeInfo &node,
				const Value &json) {
			const auto &list = json.get("masksProperties");
			auto masks = list.items();
			auto changed = false;
			for (auto i = 0; i != int(masks.size()); ++i) {
				if (!masks[i].get("inv").toBool(false)) {
					continue;
				}
				auto inverted = InvertedMask(
					masks[i],
					MaskIsFirstDrawn(list, i));
				if (!inverted.sameAs(masks[i])) {
					masks[i] = std::move(inverted);
					changed = true;
				}
			}
			return changed
				? json.with(
					"masksProperties",
					Value::FromArray(std::move(masks)))
				: json;
		}));
	}
	if (present(IssueType::ParentLinks)) {
		step(MapNodes(current, nodesOf(IssueType::ParentLinks), [](
				const NodeInfo &node,
				const Value &json) {
			return (node.kind == NodeKind::Layer && !node.parentLayer)
				? json.without("parent")
				: json;
		}));
	}
	if (present(IssueType::MergePaths) || present(IssueType::UnsupportedShapes)) {
		auto ids = std::vector<NodeId>();
		for (const auto &node : current.nodes()) {
			if (node.kind != NodeKind::Shape) {
				continue;
			}
			if ((present(IssueType::MergePaths)
					&& node.shapeType == ShapeType::MergePaths)
				|| (present(IssueType::UnsupportedShapes)
					&& IsUnsupportedShape(node.shapeType))) {
				ids.push_back(node.id);
			}
		}
		step(DeleteNodes(current, ids));
	}
	if (present(IssueType::BrokenKeyframes)) {
		step(ApplyRoot(current, FixBrokenKeyframes(current.root())));
	}
	if (present(IssueType::FrameRate)) {
		const auto fps = current.frameRate();
		step(SetFrameRate(current, (fps > 30.) ? 60. : 30., true));
	}
	if (present(IssueType::Duration)) {
		const auto limit = int(std::floor(
			kTgsMaxDuration * current.frameRate() + 1e-6));
		step(TrimRange(
			current,
			std::trunc(current.inPoint()),
			std::trunc(current.inPoint()) + limit,
			false));
	}
	if (present(IssueType::CanvasSize)) {
		step(SetCanvasSize(
			current,
			QSize(kTgsCanvasSize, kTgsCanvasSize),
			true));
	}
	if (present(IssueType::MissingTgsMarker)) {
		step(ApplyRoot(
			current,
			current.root().withInserted("tgs", Number(1), 0)));
	}
	if (wants(IssueType::FileSize)) {
		if (PackedSize(current, nullptr) > kTgsMaxPackedSize) {
			step(Optimize(current, { .decimals = 3, .stripShapeNames = true }));
		}
	}
	if (current.sameAs(document)) {
		return Unchanged(document);
	}
	auto result = Edit();
	result.document = std::move(current);
	result.changed = std::move(changed);
	result.changed.push_back(document.rootId());
	result.removed = std::move(removed);
	result.structural = true;
	return result;
}

EditorController::EditorController()
: EditorController(Document::Blank()) {
}

EditorController::EditorController(
	Document document,
	QString name,
	QString path)
: _document(document)
, _saved(document)
, _name(std::move(name))
, _path(std::move(path))
, _playTimer([=] { playbackTick(); }) {
	_frame = firstFrame();
}

EditorController::~EditorController() = default;

const Document &EditorController::document() const {
	return _document;
}

void EditorController::load(Document document, QString name, QString path) {
	setPlaying(false);
	_undo.clear();
	_redo.clear();
	_saved = document;
	_name = std::move(name);
	_path = std::move(path);
	const auto hadSelection = !_selection.empty()
		|| !_selectedKeyframes.empty()
		|| _activeProperty.has_value();
	_selection.clear();
	_selectedKeyframes.clear();
	_activeProperty = std::nullopt;
	_document = std::move(document);
	_dirty = false;
	refreshHistoryState();
	_frame = firstFrame();
	_documentChanges.fire({
		.nodes = {},
		.structural = true,
		.source = ChangeSource::Load,
	});
	if (hadSelection) {
		_selectionChanges.fire({});
		_keyframeSelectionChanges.fire({});
		_activePropertyChanges.fire({});
	}
}

QString EditorController::name() const {
	return _name.current();
}

rpl::producer<QString> EditorController::nameValue() const {
	return _name.value();
}

void EditorController::setName(QString name) {
	_name = std::move(name);
}

QString EditorController::filePath() const {
	return _path;
}

void EditorController::setFilePath(QString path) {
	_path = std::move(path);
}

bool EditorController::dirty() const {
	return _dirty.current();
}

rpl::producer<bool> EditorController::dirtyValue() const {
	return _dirty.value();
}

void EditorController::markSaved() {
	_saved = _document;
	for (auto &step : _undo) {
		step.sealed = true;
	}
	_dirty = false;
}

rpl::producer<DocumentChange> EditorController::documentChanged() const {
	return _documentChanges.events();
}

bool EditorController::perform(
		Command command,
		Edit &&edit,
		QByteArray mergeKey) {
	if (!edit.ok()) {
		if (!edit.error.isEmpty()) {
			LOG(("Oblivion LottieEdit: %1").arg(edit.error));
		}
		return false;
	} else if (edit.document.sameAs(_document)) {
		return false;
	}
	auto nodes = std::move(edit.changed);
	for (const auto list : { &edit.created, &edit.removed }) {
		for (const auto id : *list) {
			if (!ranges::contains(nodes, id)) {
				nodes.push_back(id);
			}
		}
	}
	const auto now = crl::now();
	const auto gesture = !mergeKey.isEmpty() && (mergeKey == _gestureKey);
	const auto merge = !mergeKey.isEmpty()
		&& !_undo.empty()
		&& !_undo.back().sealed
		&& _undo.back().mergeKey == mergeKey
		&& _undo.back().command == command
		&& (gesture || (now - _undo.back().updated) < kMergeTimeout);
	if (merge) {
		auto &step = _undo.back();
		step.after = edit.document.root();
		step.updated = now;
		step.structural = step.structural || edit.structural;
		for (const auto id : nodes) {
			if (!ranges::contains(step.nodes, id)) {
				step.nodes.push_back(id);
			}
		}
	} else {
		if (!_undo.empty()) {
			_undo.back().sealed = true;
		}
		_undo.push_back({
			.before = _document.root(),
			.after = edit.document.root(),
			.command = command,
			.mergeKey = mergeKey,
			.nodes = nodes,
			.structural = edit.structural,
			.updated = now,
			.sealed = mergeKey.isEmpty(),
		});
		if (int(_undo.size()) > kMaxUndoSteps) {
			_undo.erase(begin(_undo));
		}
	}
	_redo.clear();
	setDocument(std::move(edit.document), {
		.nodes = std::move(nodes),
		.structural = edit.structural,
		.source = ChangeSource::Edit,
		.command = command,
	});
	return true;
}

void EditorController::finishMerge() {
	_gestureKey = QByteArray();
	if (!_undo.empty()) {
		_undo.back().sealed = true;
	}
}

void EditorController::beginGesture(QByteArray mergeKey) {
	_gestureKey = std::move(mergeKey);
}

bool EditorController::cancelGesture(const QByteArray &mergeKey) {
	if (_gestureKey == mergeKey) {
		_gestureKey = QByteArray();
	}
	if (mergeKey.isEmpty()
		|| _undo.empty()
		|| _undo.back().mergeKey != mergeKey) {
		return false;
	}
	auto change = DocumentChange();
	change.source = ChangeSource::Undo;
	auto before = Value();
	while (!_undo.empty() && _undo.back().mergeKey == mergeKey) {
		auto &step = _undo.back();
		for (const auto id : step.nodes) {
			if (!ranges::contains(change.nodes, id)) {
				change.nodes.push_back(id);
			}
		}
		change.structural = change.structural || step.structural;
		change.command = step.command;
		before = std::move(step.before);
		_undo.pop_back();
	}
	setDocument(HistoryDocument(before), std::move(change));
	return true;
}

bool EditorController::canUndo() const {
	return !_undo.empty();
}

bool EditorController::canRedo() const {
	return !_redo.empty();
}

Command EditorController::undoCommand() const {
	return _undo.empty() ? Command::Unknown : _undo.back().command;
}

Command EditorController::redoCommand() const {
	return _redo.empty() ? Command::Unknown : _redo.back().command;
}

bool EditorController::undo() {
	if (_undo.empty()) {
		return false;
	}
	auto step = std::move(_undo.back());
	_undo.pop_back();
	step.sealed = true;
	auto change = DocumentChange{
		.nodes = step.nodes,
		.structural = step.structural,
		.source = ChangeSource::Undo,
		.command = step.command,
	};
	auto document = HistoryDocument(step.before);
	_redo.push_back(std::move(step));
	setDocument(std::move(document), std::move(change));
	return true;
}

bool EditorController::redo() {
	if (_redo.empty()) {
		return false;
	}
	auto step = std::move(_redo.back());
	_redo.pop_back();
	if (!_undo.empty()) {
		_undo.back().sealed = true;
	}
	auto change = DocumentChange{
		.nodes = step.nodes,
		.structural = step.structural,
		.source = ChangeSource::Redo,
		.command = step.command,
	};
	auto document = HistoryDocument(step.after);
	_undo.push_back(std::move(step));
	setDocument(std::move(document), std::move(change));
	return true;
}

void EditorController::clearHistory() {
	_undo.clear();
	_redo.clear();
	refreshHistoryState();
}

rpl::producer<bool> EditorController::undoAvailable() const {
	return _canUndo.value();
}

rpl::producer<bool> EditorController::redoAvailable() const {
	return _canRedo.value();
}

void EditorController::setDocument(
		Document document,
		DocumentChange &&change) {
	const auto timing = std::make_tuple(firstFrame(), frameCount(), fps());
	_document = std::move(document);
	const auto selectionBefore = _selection.size();
	const auto keyframesBefore = _selectedKeyframes.size();
	const auto hadActive = _activeProperty.has_value();
	pruneSelection();
	_dirty = !_document.sameAs(_saved);
	refreshHistoryState();
	// Before the change signal: its observers read currentFrame() and must
	// see a frame inside the new range (after a trim or a shorter duration).
	clampFrame();
	if (playing()
		&& timing != std::make_tuple(firstFrame(), frameCount(), fps())) {
		startPlaybackClock();
	}
	_documentChanges.fire(std::move(change));
	if (_selection.size() != selectionBefore) {
		_selectionChanges.fire({});
	}
	if (_selectedKeyframes.size() != keyframesBefore) {
		_keyframeSelectionChanges.fire({});
	}
	if (hadActive != _activeProperty.has_value()) {
		_activePropertyChanges.fire({});
	}
}

void EditorController::pruneSelection() {
	_selection.erase(ranges::remove_if(_selection, [&](NodeId id) {
		return !_document.contains(id);
	}), end(_selection));
	_selectedKeyframes.erase(ranges::remove_if(
		_selectedKeyframes,
		[&](const KeyframeRef &ref) {
			return !ContainsTime(
				_document.keyframeTimes(ref.property),
				ref.time);
		}), end(_selectedKeyframes));
	if (_activeProperty && !_document.property(*_activeProperty)) {
		_activeProperty = std::nullopt;
	}
}

void EditorController::clampFrame() {
	_frame = std::clamp(_frame.current(), firstFrame(), lastFrame());
}

void EditorController::refreshHistoryState() {
	_canUndo = !_undo.empty();
	_canRedo = !_redo.empty();
}

bool EditorController::performWithSelection(
		Command command,
		Edit &&edit,
		QByteArray mergeKey) {
	auto created = edit.created;
	if (!perform(command, std::move(edit), std::move(mergeKey))) {
		return false;
	}
	if (!created.empty()) {
		setSelection(std::move(created));
	}
	return true;
}

bool EditorController::setValue(
		const PropertyRef &ref,
		const PropValue &value,
		QByteArray mergeKey) {
	return perform(
		Command::SetValue,
		SetValueAt(_document, ref, value, localFrame(ref.node)),
		std::move(mergeKey));
}

bool EditorController::setStaticValue(
		const PropertyRef &ref,
		const PropValue &value,
		QByteArray mergeKey) {
	return perform(
		Command::SetValue,
		SetStaticValue(_document, ref, value),
		std::move(mergeKey));
}

bool EditorController::setAnimated(const PropertyRef &ref, bool animated) {
	return perform(
		Command::ToggleAnimated,
		SetAnimated(_document, ref, animated, localFrame(ref.node)));
}

bool EditorController::addKeyframe(
		const PropertyRef &ref,
		std::optional<double> time,
		std::optional<PropValue> value,
		std::optional<Easing> easing) {
	return perform(Command::AddKeyframe, AddKeyframe(
		_document,
		ref,
		time.value_or(localFrame(ref.node)),
		std::move(value),
		std::move(easing)));
}

bool EditorController::setKeyframeValue(
		const KeyframeRef &keyframe,
		const PropValue &value,
		QByteArray mergeKey) {
	return perform(
		Command::ChangeKeyframe,
		SetKeyframeValue(_document, keyframe, value),
		std::move(mergeKey));
}

bool EditorController::setKeyframeEasing(
		const std::vector<KeyframeRef> &keyframes,
		const Easing &easing) {
	return perform(
		Command::SetEasing,
		SetKeyframeEasing(_document, keyframes, easing));
}

bool EditorController::removeKeyframes(
		const std::vector<KeyframeRef> &keyframes) {
	return perform(
		Command::RemoveKeyframes,
		RemoveKeyframes(_document, keyframes));
}

bool EditorController::moveKeyframes(
		const std::vector<KeyframeRef> &keyframes,
		double delta,
		QByteArray mergeKey) {
	auto selected = _selectedKeyframes;
	if (!perform(
			Command::MoveKeyframes,
			MoveKeyframes(_document, keyframes, delta),
			std::move(mergeKey))) {
		return false;
	}
	auto updated = false;
	for (auto &ref : selected) {
		if (ranges::contains(keyframes, ref)) {
			ref.time = RoundTime(ref.time + delta);
			updated = true;
		}
	}
	if (updated) {
		setSelectedKeyframes(std::move(selected));
	}
	return true;
}

bool EditorController::setTransform(
		NodeId id,
		TransformField field,
		const PropValue &value,
		QByteArray mergeKey) {
	return perform(
		Command::SetValue,
		SetTransformAt(_document, id, field, value, localFrame(id)),
		std::move(mergeKey));
}

bool EditorController::rename(NodeId id, const QString &name) {
	return perform(Command::Rename, Rename(_document, id, name));
}

bool EditorController::setHidden(const std::vector<NodeId> &ids, bool hidden) {
	return perform(Command::SetHidden, SetHidden(_document, ids, hidden));
}

bool EditorController::deleteNodes(const std::vector<NodeId> &ids) {
	return perform(Command::Delete, DeleteNodes(_document, ids));
}

bool EditorController::duplicateNodes(const std::vector<NodeId> &ids) {
	return performWithSelection(
		Command::Duplicate,
		DuplicateNodes(_document, ids),
		QByteArray());
}

bool EditorController::moveNode(NodeId id, NodeId container, int index) {
	return perform(Command::Move, MoveNode(_document, id, container, index));
}

bool EditorController::reorderNode(NodeId id, int delta) {
	return perform(Command::Move, ReorderNode(_document, id, delta));
}

bool EditorController::addLayer(
		LayerTemplate type,
		const QString &name,
		std::optional<ShapeTemplate> content) {
	return performWithSelection(
		Command::AddLayer,
		AddLayer(_document, type, name, 0, 0, content),
		QByteArray());
}

bool EditorController::addShape(
		NodeId container,
		ShapeTemplate type,
		const QString &name) {
	return performWithSelection(
		Command::AddShape,
		AddShape(_document, container, type, name),
		QByteArray());
}

bool EditorController::setLayerTiming(
		NodeId layer,
		double inPoint,
		double outPoint,
		QByteArray mergeKey) {
	return perform(
		Command::LayerTiming,
		SetLayerTiming(_document, layer, inPoint, outPoint),
		std::move(mergeKey));
}

bool EditorController::shiftLayers(
		const std::vector<NodeId> &layers,
		double delta,
		QByteArray mergeKey) {
	return perform(
		Command::LayerTiming,
		ShiftLayers(_document, layers, delta),
		std::move(mergeKey));
}

bool EditorController::replaceColor(
		const QColor &from,
		const QColor &to,
		const std::vector<NodeId> &scope) {
	return perform(
		Command::ReplaceColor,
		ReplaceColor(_document, from, to, scope));
}

bool EditorController::setColor(
		const ColorOccurrence &occurrence,
		const QColor &color,
		QByteArray mergeKey) {
	return perform(
		Command::ReplaceColor,
		SetColor(_document, occurrence, color),
		std::move(mergeKey));
}

bool EditorController::adjustHsl(
		int hueDegrees,
		int saturationPercent,
		int lightnessPercent,
		const std::vector<NodeId> &scope,
		QByteArray mergeKey) {
	return perform(Command::Recolor, AdjustHsl(
		_document,
		hueDegrees,
		saturationPercent,
		lightnessPercent,
		scope), std::move(mergeKey));
}

bool EditorController::setCanvasSize(QSize size, bool scaleContent) {
	return perform(
		Command::CanvasSize,
		SetCanvasSize(_document, size, scaleContent));
}

bool EditorController::setFrameRate(double fps, bool retime) {
	return perform(Command::FrameRate, SetFrameRate(_document, fps, retime));
}

bool EditorController::setDuration(int frames) {
	return perform(Command::Duration, SetDuration(_document, frames));
}

bool EditorController::changeSpeed(double factor) {
	return perform(Command::Speed, ChangeSpeed(_document, factor));
}

bool EditorController::trimRange(double from, double to, bool rebase) {
	return perform(Command::Trim, TrimRange(_document, from, to, rebase));
}

bool EditorController::autoFix(const std::vector<IssueType> &types) {
	return perform(Command::AutoFix, AutoFix(_document, types));
}

bool EditorController::optimize(const OptimizeOptions &options) {
	return perform(Command::Optimize, Optimize(_document, options));
}

bool EditorController::addMask(
		NodeId layer,
		const PathData &path,
		MaskMode mode,
		const QString &name) {
	return performWithSelection(
		Command::AddMask,
		AddMask(_document, layer, path, mode, name),
		QByteArray());
}

bool EditorController::setMaskMode(NodeId mask, MaskMode mode) {
	return perform(Command::ChangeMask, SetMaskMode(_document, mask, mode));
}

bool EditorController::setMaskInverted(NodeId mask, bool inverted) {
	return perform(
		Command::ChangeMask,
		SetMaskInverted(_document, mask, inverted));
}

bool EditorController::invertMask(NodeId mask) {
	return perform(Command::ChangeMask, InvertMask(_document, mask));
}

bool EditorController::setTrackMatte(
		NodeId layer,
		MatteMode mode,
		NodeId source) {
	return perform(
		Command::TrackMatte,
		SetTrackMatte(_document, layer, mode, source));
}

bool EditorController::setLayerParent(
		NodeId layer,
		NodeId parent,
		bool keepPlace) {
	return perform(Command::Parent, SetLayerParent(
		_document,
		layer,
		parent,
		keepPlace ? std::make_optional(localFrame(layer)) : std::nullopt));
}

bool EditorController::setGradientType(NodeId shape, GradientType type) {
	return perform(
		Command::ShapeOption,
		SetGradientType(_document, shape, type));
}

bool EditorController::setTrimMode(NodeId shape, TrimMode mode) {
	return perform(Command::ShapeOption, SetTrimMode(_document, shape, mode));
}

bool EditorController::setLineCap(NodeId shape, LineCap cap) {
	return perform(Command::ShapeOption, SetLineCap(_document, shape, cap));
}

bool EditorController::setLineJoin(NodeId shape, LineJoin join) {
	return perform(Command::ShapeOption, SetLineJoin(_document, shape, join));
}

bool EditorController::setMiterLimit(
		NodeId shape,
		double limit,
		QByteArray mergeKey) {
	return perform(
		Command::ShapeOption,
		SetMiterLimit(_document, shape, limit),
		std::move(mergeKey));
}

bool EditorController::setFillRule(NodeId shape, FillRule rule) {
	return perform(Command::ShapeOption, SetFillRule(_document, shape, rule));
}

bool EditorController::setGradient(
		NodeId shape,
		const GradientData &data,
		QByteArray mergeKey) {
	return perform(
		Command::Gradient,
		SetGradient(_document, shape, data, localFrame(shape)),
		std::move(mergeKey));
}

bool EditorController::addGradientStop(
		NodeId shape,
		double offset,
		bool alpha) {
	return perform(
		Command::Gradient,
		AddGradientStop(_document, shape, offset, alpha));
}

bool EditorController::removeGradientStop(
		NodeId shape,
		int index,
		bool alpha) {
	return perform(
		Command::Gradient,
		RemoveGradientStop(_document, shape, index, alpha));
}

bool EditorController::convertPaint(NodeId shape, ShapeType type) {
	return perform(
		Command::ConvertPaint,
		ConvertPaint(_document, shape, type, localFrame(shape)));
}

bool EditorController::setDashes(
		NodeId stroke,
		const std::vector<double> &pattern,
		double offset,
		QByteArray mergeKey) {
	return perform(
		Command::Dashes,
		SetDashes(_document, stroke, pattern, offset),
		std::move(mergeKey));
}

bool EditorController::setDashCount(NodeId stroke, int pairs) {
	return perform(Command::Dashes, SetDashCount(_document, stroke, pairs));
}

bool EditorController::bakeRoundCorners(NodeId roundCorners) {
	return perform(Command::BakeCorners, BakeRoundCorners(
		_document,
		roundCorners,
		localFrame(roundCorners)));
}

bool EditorController::addPath(
		NodeId container,
		const PathData &path,
		const QString &name) {
	return performWithSelection(
		Command::AddPath,
		AddPath(_document, container, path, name),
		QByteArray());
}

bool EditorController::insertPathVertex(
		const PropertyRef &path,
		int segment,
		double t) {
	return perform(
		Command::EditPath,
		InsertPathVertex(_document, path, segment, t));
}

bool EditorController::removePathVertex(const PropertyRef &path, int index) {
	return perform(
		Command::EditPath,
		RemovePathVertex(_document, path, index));
}

bool EditorController::setPathClosed(const PropertyRef &path, bool closed) {
	return perform(Command::EditPath, SetPathClosed(_document, path, closed));
}

bool EditorController::reversePath(const PropertyRef &path) {
	return perform(Command::EditPath, ReversePath(_document, path));
}

bool EditorController::setKeyframeHandles(
		const KeyframeRef &keyframe,
		std::optional<QPointF> in,
		std::optional<QPointF> out,
		QByteArray mergeKey) {
	return perform(
		Command::SetEasing,
		SetKeyframeHandles(_document, keyframe, in, out),
		std::move(mergeKey));
}

bool EditorController::setKeyframeEasings(
		const std::vector<std::pair<KeyframeRef, Easing>> &easings) {
	return perform(
		Command::SetEasing,
		SetKeyframeEasings(_document, easings));
}

bool EditorController::setMotionPath(const PropertyRef &ref, bool enabled) {
	return perform(
		Command::SetEasing,
		SetMotionPath(_document, ref, enabled));
}

bool EditorController::deleteSelection() {
	if (!_selectedKeyframes.empty()) {
		return removeKeyframes(_selectedKeyframes);
	} else if (!_selection.empty()) {
		return deleteNodes(_selection);
	}
	return false;
}

bool EditorController::duplicateSelection() {
	return !_selection.empty() && duplicateNodes(_selection);
}

const std::vector<NodeId> &EditorController::selection() const {
	return _selection;
}

NodeId EditorController::primarySelection() const {
	return _selection.empty() ? NodeId(0) : _selection.back();
}

bool EditorController::isSelected(NodeId id) const {
	return ranges::contains(_selection, id);
}

void EditorController::setSelection(std::vector<NodeId> ids) {
	auto filtered = std::vector<NodeId>();
	filtered.reserve(ids.size());
	for (const auto id : ids) {
		if (_document.contains(id) && !ranges::contains(filtered, id)) {
			filtered.push_back(id);
		}
	}
	if (filtered == _selection) {
		return;
	}
	_selection = std::move(filtered);

	// Keyframes and the active property follow the node selection: the ones
	// of nodes unrelated to the new selection are dropped, so that Delete,
	// paste and the inspector never act on something the user moved away
	// from (a layer's transform stays related while a shape inside it is
	// selected, and the other way around).
	const auto related = [&](NodeId node) {
		return ranges::any_of(_selection, [&](NodeId selected) {
			return (node == selected)
				|| _document.isDescendant(node, selected)
				|| _document.isDescendant(selected, node);
		});
	};
	const auto keyframesBefore = _selectedKeyframes.size();
	_selectedKeyframes.erase(ranges::remove_if(
		_selectedKeyframes,
		[&](const KeyframeRef &ref) { return !related(ref.property.node); }
	), end(_selectedKeyframes));
	const auto activeDropped = _activeProperty
		&& !related(_activeProperty->node);
	if (activeDropped) {
		_activeProperty = std::nullopt;
	}

	_selectionChanges.fire({});
	if (_selectedKeyframes.size() != keyframesBefore) {
		_keyframeSelectionChanges.fire({});
	}
	if (activeDropped) {
		_activePropertyChanges.fire({});
	}
}

void EditorController::select(NodeId id, SelectMode mode) {
	auto selection = _selection;
	switch (mode) {
	case SelectMode::Replace:
		selection = id ? std::vector<NodeId>{ id } : std::vector<NodeId>();
		break;
	case SelectMode::Add:
		selection.erase(ranges::remove(selection, id), end(selection));
		selection.push_back(id);
		break;
	case SelectMode::Toggle:
		if (ranges::contains(selection, id)) {
			selection.erase(ranges::remove(selection, id), end(selection));
		} else {
			selection.push_back(id);
		}
		break;
	}
	setSelection(std::move(selection));
}

void EditorController::clearSelection() {
	setSelection({});
}

rpl::producer<> EditorController::selectionChanged() const {
	return _selectionChanges.events();
}

std::optional<PropertyRef> EditorController::activeProperty() const {
	return _activeProperty;
}

void EditorController::setActiveProperty(std::optional<PropertyRef> ref) {
	if (ref && !_document.property(*ref)) {
		ref = std::nullopt;
	}
	if (_activeProperty == ref) {
		return;
	}
	_activeProperty = std::move(ref);
	_activePropertyChanges.fire({});
}

rpl::producer<> EditorController::activePropertyChanged() const {
	return _activePropertyChanges.events();
}

const std::vector<KeyframeRef> &EditorController::selectedKeyframes() const {
	return _selectedKeyframes;
}

void EditorController::setSelectedKeyframes(
		std::vector<KeyframeRef> keyframes) {
	if (keyframes == _selectedKeyframes) {
		return;
	}
	_selectedKeyframes = std::move(keyframes);
	_keyframeSelectionChanges.fire({});
}

bool EditorController::isKeyframeSelected(const KeyframeRef &ref) const {
	return ranges::any_of(_selectedKeyframes, [&](const KeyframeRef &item) {
		return (item.property == ref.property)
			&& Near(item.time, ref.time, kSameTime);
	});
}

rpl::producer<> EditorController::keyframeSelectionChanged() const {
	return _keyframeSelectionChanges.events();
}

int EditorController::currentFrame() const {
	return _frame.current();
}

int EditorController::frameIndex() const {
	return currentFrame() - firstFrame();
}

int EditorController::firstFrame() const {
	return int(std::trunc(_document.inPoint()));
}

int EditorController::lastFrame() const {
	return std::max(firstFrame(), firstFrame() + _document.frames() - 1);
}

int EditorController::frameCount() const {
	return _document.frames();
}

double EditorController::fps() const {
	return _document.frameRate();
}

void EditorController::setCurrentFrame(int frame) {
	_frame = std::clamp(frame, firstFrame(), lastFrame());
	if (playing()) {
		_playStarted = crl::now();
		_playStartFrame = _frame.current();
	}
}

void EditorController::stepFrame(int delta) {
	const auto count = std::max(frameCount(), 1);
	const auto index = ((frameIndex() + delta) % count + count) % count;
	setCurrentFrame(firstFrame() + index);
}

rpl::producer<int> EditorController::currentFrameChanged() const {
	return _frame.changes();
}

rpl::producer<int> EditorController::currentFrameValue() const {
	return _frame.value();
}

double EditorController::localFrame(NodeId id) const {
	return _document.localFrame(id, currentFrame());
}

bool EditorController::playing() const {
	return _playing.current();
}

void EditorController::setPlaying(bool playing) {
	if (playing && (frameCount() <= 1 || fps() <= 0.)) {
		playing = false;
	}
	if (_playing.current() == playing) {
		return;
	}
	if (playing) {
		if (!_looping && currentFrame() >= lastFrame()) {
			_frame = firstFrame();
		}
		startPlaybackClock();
	} else {
		_playTimer.cancel();
	}
	_playing = playing;
}

void EditorController::startPlaybackClock() {
	const auto rate = fps();
	_playStarted = crl::now();
	_playStartFrame = currentFrame();
	const auto interval = (rate > 0.)
		? std::clamp(std::floor(500. / rate), 4., 100.)
		: 100.;
	_playTimer.callEach(crl::time(interval));
}

void EditorController::togglePlaying() {
	setPlaying(!playing());
}

rpl::producer<bool> EditorController::playingValue() const {
	return _playing.value();
}

bool EditorController::looping() const {
	return _looping;
}

void EditorController::setLooping(bool looping) {
	_looping = looping;
}

void EditorController::playbackTick() {
	const auto count = frameCount();
	const auto rate = fps();
	if (!playing() || count <= 0 || rate <= 0.) {
		setPlaying(false);
		return;
	}
	const auto elapsed = crl::now() - _playStarted;
	const auto passed = int64(std::floor(elapsed * rate / 1000.));
	const auto start = int64(_playStartFrame - firstFrame());
	const auto index = start + passed;
	if (!_looping && index >= count - 1) {
		_frame = lastFrame();
		setPlaying(false);
		return;
	}
	_frame = firstFrame() + int(((index % count) + count) % count);
}

double EditorController::zoom() const {
	return _zoom.current();
}

void EditorController::setZoom(double zoom) {
	_zoom = std::clamp(zoom, 0.1, 32.);
}

rpl::producer<double> EditorController::zoomValue() const {
	return _zoom.value();
}

QPointF EditorController::pan() const {
	return _pan.current();
}

void EditorController::setPan(QPointF pan) {
	_pan = pan;
}

rpl::producer<QPointF> EditorController::panValue() const {
	return _pan.value();
}

void EditorController::setShow(std::shared_ptr<Ui::Show> show) {
	_show = std::move(show);
}

std::shared_ptr<Ui::Show> EditorController::uiShow() const {
	return _show;
}

void EditorController::requestAction(EditorAction action) {
	_actionRequests.fire_copy(action);
}

rpl::producer<EditorAction> EditorController::actionRequests() const {
	return _actionRequests.events();
}

rpl::lifetime &EditorController::lifetime() {
	return _lifetime;
}

namespace {

struct TestContext {
	QStringList &log;
	int checks = 0;
	int failures = 0;

	void check(bool condition, const QString &what) {
		++checks;
		if (!condition) {
			++failures;
			log.push_back(u"lottie_doc: FAILED: "_q + what);
		}
	}
};

constexpr auto kTestDocument = R"({"v":"5.5.2","fr":60,"ip":0,"op":120,)"
R"("w":100,"h":100,"nm":"Test","ddd":0,"assets":[],"layers":[)"
R"({"ddd":0,"ind":1,"ty":4,"nm":"Matte","td":1,"sr":1,"ks":{)"
R"("o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[50,50,0]},)"
R"("a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,)"
R"("shapes":[{"ty":"gr","it":[{"ty":"rc","d":1,"s":{"a":0,"k":[40,40]},)"
R"("p":{"a":0,"k":[0,0]},"r":{"a":0,"k":0},"nm":"Rect"},)"
R"({"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100},"r":1,)"
R"("nm":"Fill"},{"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},)"
R"("s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100},)"
R"("sk":{"a":0,"k":0},"sa":{"a":0,"k":0},"nm":"Transform"}],"nm":"Group"}],)"
R"("ip":0,"op":120,"st":0,"bm":0},)"
R"({"ddd":0,"ind":2,"ty":4,"nm":"Matted","tt":1,"parent":3,"sr":1,"ks":{)"
R"("o":{"a":1,"k":[{"i":{"x":[0.833],"y":[0.833]},)"
R"("o":{"x":[0.167],"y":[0.167]},"t":0,"s":[0]},{"t":60,"s":[100]}]},)"
R"("r":{"a":0,"k":0},"p":{"a":0,"k":[0,0,0]},"a":{"a":0,"k":[0,0,0]},)"
R"("s":{"a":0,"k":[100,100,100]}},"ao":0,"shapes":[{"ty":"gr","it":[)"
R"({"ty":"el","d":1,"s":{"a":0,"k":[60,60]},"p":{"a":0,"k":[0,0]},)"
R"("nm":"Ellipse"},{"ty":"fl","c":{"a":0,"k":[0,0,1,1]},)"
R"("o":{"a":0,"k":100},"r":1,"nm":"Fill"},{"ty":"tr","p":{"a":0,"k":[0,0]},)"
R"("a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},)"
R"("o":{"a":0,"k":100},"sk":{"a":0,"k":0},"sa":{"a":0,"k":0},)"
R"("nm":"Transform"}],"nm":"Group"}],"ip":0,"op":120,"st":0,"bm":0},)"
R"({"ddd":0,"ind":3,"ty":3,"nm":"Null","sr":1,"ks":{"o":{"a":0,"k":0},)"
R"("r":{"a":0,"k":0},"p":{"a":1,"k":[{"i":{"x":0.5,"y":1},)"
R"("o":{"x":0.5,"y":0},"t":0,"s":[10,10,0],"to":[5,0,0],"ti":[-5,0,0]},)"
R"({"i":{"x":0.5,"y":1},"o":{"x":0.5,"y":0},"t":30,"s":[40,10,0],"h":1},)"
R"({"t":60,"s":[40,40,0]}]},"a":{"a":0,"k":[0,0,0]},)"
R"("s":{"a":0,"k":[100,100,100]}},"ao":0,"ip":0,"op":120,"st":0,"bm":0}]})";

[[nodiscard]] NodeId FindByName(const Document &document, const QString &name) {
	for (const auto &node : document.nodes()) {
		if (node.name == name) {
			return node.id;
		}
	}
	return 0;
}

[[nodiscard]] std::vector<QString> LayerNames(const Document &document) {
	auto result = std::vector<QString>();
	for (const auto id : document.layers()) {
		result.push_back(document.node(id)->name);
	}
	return result;
}

void TestJson(TestContext &context) {
	const auto roundTrip = [&](QByteArrayView source, QByteArrayView expected) {
		auto error = QString();
		const auto parsed = Json::Parse(source, &error);
		const auto output = parsed ? Json::Serialize(*parsed) : QByteArray();
		context.check(
			parsed && output == expected,
			u"json round trip of "_q
				+ QString::fromUtf8(source.toByteArray())
				+ u" -> "_q
				+ QString::fromUtf8(output)
				+ (error.isEmpty() ? QString() : (u" ("_q + error + ')')));
	};
	roundTrip(R"({"b":1,"a":[1,2.5,-0.25,1e-7,1.5e300,123456789012]})",
		R"({"b":1,"a":[1,2.5,-0.25,1e-07,1.5e+300,123456789012]})");
	roundTrip(R"( { "x" : "a\"b\\c\né😀" , "y":[ ] } )",
		"{\"x\":\"a\\\"b\\\\c\\n\xc3\xa9\xf0\x9f\x98\x80\",\"y\":[]}");
	roundTrip(R"([true,false,null,{},0.1,0.30000000000000004,-0])",
		R"([true,false,null,{},0.1,0.30000000000000004,0])");
	roundTrip("\xEF\xBB\xBF[1]", "[1]");
	roundTrip(QByteArrayView("[1] \0\0", 6), "[1]");
	roundTrip(R"({"a":1,"b":{"a":4,"a":5},"a":3})", R"({"a":3,"b":{"a":5}})");
	for (const auto bad : { "[1,]", "{\"a\"1}", "[01x]", "\"abc", "[1] 2" }) {
		context.check(
			!Json::Parse(QByteArrayView(bad)).has_value(),
			u"json must reject "_q + QString::fromLatin1(bad));
	}
	{
		auto wide = QByteArray("{");
		for (auto i = 0; i != 100; ++i) {
			wide += "\"k" + QByteArray::number(i % 50) + "\":"
				+ QByteArray::number(i) + ((i == 99) ? "}" : ",");
		}
		const auto parsed = Json::Parse(wide);
		auto repeated = false;
		auto plain = true;
		auto zero = false;
		const auto root = Json::ParseRoot(
			"[{\"a\":1,\"a\":2}] junk",
			&repeated);
		const auto single = Json::ParseRoot("{\"a\":1,\"b\":-0.5}", &plain);
		const auto negative = Json::ParseRoot("[-0.0]", &zero);
		context.check(
			!Json::Parse(QByteArrayView("[1]\0x", 5)).has_value()
				&& parsed
				&& parsed->size() == 50
				&& parsed->members().front().key == "k0"
				&& parsed->get("k0").toInt() == 50
				&& parsed->get("k49").toInt() == 99
				&& root
				&& repeated
				&& Json::Serialize(*root) == "[{\"a\":2}]"
				&& single
				&& !plain
				&& negative
				&& zero
				&& !std::signbit(negative->at(0).toDouble(1.))
				&& !Json::ParseRoot("[1").has_value(),
			u"json padding, repeated keys and the root alone"_q);
	}
	const auto parsed = Json::Parse(R"({"k":{"a":1},"k2":[{"z":1}]})");
	context.check(parsed.has_value(), u"json parse objects"_q);
	if (parsed) {
		const auto &root = *parsed;
		const auto copy = root.with("k", root.get("k").with("a", Number(2)));
		context.check(
			copy.id() == root.id()
				&& copy.get("k").id() == root.get("k").id()
				&& copy.get("k2").sameAs(root.get("k2"))
				&& root.get("k").get("a").toInt() == 1
				&& copy.get("k").get("a").toInt() == 2,
			u"json persistent update keeps ids and shares subtrees"_q);
		const auto fresh = root.withNewIds();
		context.check(
			fresh == root
				&& fresh.id() != root.id()
				&& fresh.get("k2").at(0).id() != root.get("k2").at(0).id(),
			u"json withNewIds"_q);
		const auto inserted = root.withInserted("first", Number(0), 0);
		context.check(
			Json::Serialize(inserted)
				== R"({"first":0,"k":{"a":1},"k2":[{"z":1}]})",
			u"json withInserted keeps order"_q);
	}
}

void TestEasing(TestContext &context) {
	auto random = std::mt19937(12345);
	auto uniform = std::uniform_real_distribution<double>(0., 1.);
	auto easings = std::vector<Easing>{
		Easing::Linear(),
		Easing::EaseIn(),
		Easing::EaseOut(),
		Easing::EaseInOut(),
		Easing{ QPointF(0.167, 0.167), QPointF(0.833, 0.833) },
		Easing{ QPointF(0.333, 0.), QPointF(0.667, 1.) },
	};
	for (auto i = 0; i != 40; ++i) {
		easings.push_back(Easing{
			QPointF(uniform(random), uniform(random)),
			QPointF(uniform(random), uniform(random)),
		});
	}
	auto maxError = 0.;
	auto monotonic = true;
	for (const auto &easing : easings) {
		auto previous = -1.;
		for (auto step = 0; step <= 1000; ++step) {
			const auto x = step / 1000.;
			const auto y = easing.apply(x);
			if (y < previous - 1e-4) {
				monotonic = false;
			}
			previous = y;
			auto a = 0.;
			auto b = 1.;
			for (auto i = 0; i != 60; ++i) {
				const auto t = (a + b) / 2.;
				const auto u = 1. - t;
				const auto bx = 3. * u * u * t * easing.out.x()
					+ 3. * u * t * t * easing.in.x()
					+ t * t * t;
				((bx < x) ? a : b) = t;
			}
			const auto t = (a + b) / 2.;
			const auto u = 1. - t;
			const auto by = 3. * u * u * t * easing.out.y()
				+ 3. * u * t * t * easing.in.y()
				+ t * t * t;
			maxError = std::max(maxError, std::abs(by - y));
		}
		context.check(
			Near(easing.apply(0.), 0., 1e-3) && Near(easing.apply(1.), 1., 1e-3),
			u"easing endpoints"_q);
	}
	context.check(monotonic, u"bezier easing is monotonic"_q);
	context.check(
		maxError < 2e-3,
		u"bezier easing matches the exact curve, max error "_q
			+ QString::number(maxError));
	context.check(
		Easing::Linear().preset() == EasingPreset::Linear
			&& Easing::EaseIn().preset() == EasingPreset::EaseIn
			&& Easing::EaseOut().preset() == EasingPreset::EaseOut
			&& Easing::EaseInOut().preset() == EasingPreset::EaseInOut
			&& Easing::Hold().preset() == EasingPreset::Hold
			&& Easing{ QPointF(0.333, 0.), QPointF(0.667, 1.) }.preset()
				== EasingPreset::EaseInOut
			&& Easing{ QPointF(0.2, 0.9), QPointF(0.1, 0.8) }.preset()
				== EasingPreset::Custom,
		u"easing presets"_q);
	context.check(Easing::Hold().apply(0.7) == 0., u"hold easing"_q);
}

void TestKeyframes(TestContext &context) {
	const auto document = Document::FromJson(kTestDocument);
	context.check(document.valid(), u"test document parses"_q);
	if (!document.valid()) {
		return;
	}
	const auto matted = FindByName(document, u"Matted"_q);
	const auto null = FindByName(document, u"Null"_q);
	const auto opacity = PropertyRef{ matted, "ks.o" };
	const auto position = PropertyRef{ null, "ks.p" };
	const auto value = [&](const PropertyRef &ref, double frame) {
		return document.valueAt(ref, frame).value_or(PropValue());
	};
	context.check(
		Near(value(opacity, 30).scalar(), 50., 1e-3),
		u"linear-like easing midpoint"_q);
	context.check(
		Near(value(opacity, -10).scalar(), 0.)
			&& Near(value(opacity, 60).scalar(), 100.)
			&& Near(value(opacity, 90).scalar(), 100.),
		u"values before / after keyframes"_q);
	context.check(
		Near(value(position, 45).point().x(), 40.)
			&& Near(value(position, 45).point().y(), 10.)
			&& Near(value(position, 59.9).point().y(), 10.)
			&& Near(value(position, 60).point().y(), 40.),
		u"hold keyframe keeps the value until the next keyframe"_q);
	const auto spatial = value(position, 15).point();
	context.check(
		Near(spatial.y(), 10., 1e-3)
			&& spatial.x() > 10.
			&& spatial.x() < 40.,
		u"spatial keyframe on a straight line"_q);
	const auto old = Json::Parse(
		R"({"a":1,"k":[{"i":{"x":1,"y":1},"o":{"x":0,"y":0},"t":0,)"
		R"("s":[0],"e":[10]},{"i":{"x":1,"y":1},"o":{"x":0,"y":0},)"
		R"("t":10,"s":[10],"e":[20]},{"t":20}]})");
	const auto raw = ReadRawKeyframes(*old, PropertyType::Scalar);
	context.check(
		Near(EvaluateKeyframes(raw, PropertyType::Scalar, 5).scalar(), 5.)
			&& Near(EvaluateKeyframes(raw, PropertyType::Scalar, 15).scalar(), 15.)
			&& Near(EvaluateKeyframes(raw, PropertyType::Scalar, 25).scalar(), 20.),
		u"old style \"e\" keyframes"_q);
	const auto times = document.keyframeTimes(position);
	context.check(
		times == std::vector<double>{ 0., 30., 60. },
		u"keyframe times"_q);
	const auto keyframes = document.keyframes(position);
	context.check(
		keyframes.size() == 3
			&& keyframes[1].easing.hold
			&& keyframes[2].last
			&& !keyframes[0].outTangent.empty(),
		u"keyframe list"_q);
}

void TestOperations(TestContext &context) {
	const auto document = Document::FromJson(kTestDocument);
	if (!document.valid()) {
		return;
	}
	const auto matte = FindByName(document, u"Matte"_q);
	const auto matted = FindByName(document, u"Matted"_q);
	const auto null = FindByName(document, u"Null"_q);
	context.check(
		document.node(matted)->parentLayer == null
			&& document.node(matted)->matte == MatteMode::Alpha
			&& document.node(matte)->matteSource,
		u"layer links"_q);

	const auto withoutNull = DeleteNodes(document, { null });
	context.check(
		withoutNull
			&& !withoutNull.document.json(matted).has("parent")
			&& withoutNull.document.layers().size() == 2,
		u"delete a parent layer"_q);
	const auto withoutMatte = DeleteNodes(document, { matte });
	context.check(
		withoutMatte && !withoutMatte.document.json(matted).has("tt"),
		u"delete a matte source"_q);
	const auto withoutMatted = DeleteNodes(document, { matted });
	context.check(
		withoutMatted
			&& withoutMatted.document.json(matte).get("hd").toBool(false),
		u"delete a matted layer hides its matte"_q);

	const auto duplicated = DuplicateNodes(document, { matted });
	auto inds = std::unordered_set<int>();
	auto unique = true;
	if (duplicated) {
		for (const auto id : duplicated.document.layers()) {
			unique = inds.insert(
				duplicated.document.node(id)->ind.value_or(-1)).second
				&& unique;
		}
	}
	context.check(
		duplicated
			&& duplicated.created.size() == 2
			&& LayerNames(duplicated.document) == std::vector<QString>{
				u"Matte"_q,
				u"Matted"_q,
				u"Matte"_q,
				u"Matted"_q,
				u"Null"_q,
			}
			&& unique
			&& duplicated.document.node(duplicated.created[1])->parentLayer
				== null,
		u"duplicate a track matte pair"_q);

	const auto reordered = ReorderNode(document, null, -1);
	context.check(
		reordered && LayerNames(reordered.document) == std::vector<QString>{
			u"Null"_q,
			u"Matte"_q,
			u"Matted"_q,
		},
		u"reorder keeps track matte pairs"_q);
	const auto back = ReorderNode(reordered.document, null, 1);
	context.check(
		back && LayerNames(back.document) == LayerNames(document),
		u"reorder back"_q);

	const auto retimed = SetFrameRate(document, 30., true);
	context.check(
		retimed
			&& retimed.document.outPoint() == 60.
			&& retimed.document.keyframeTimes({ null, "ks.p" })
				== std::vector<double>{ 0., 15., 30. },
		u"retime to 30 fps"_q);
	const auto faster = ChangeSpeed(document, 2.);
	context.check(
		faster
			&& faster.document.outPoint() == 60.
			&& faster.document.frameRate() == 60.
			&& faster.document.keyframeTimes({ matted, "ks.o" })
				== std::vector<double>{ 0., 30. },
		u"speed x2"_q);
	const auto trimmed = TrimRange(document, 10., 50., true);
	context.check(
		trimmed
			&& trimmed.document.inPoint() == 0.
			&& trimmed.document.outPoint() == 40.
			&& trimmed.document.keyframeTimes({ null, "ks.p" })
				== std::vector<double>{ -10., 20., 50. },
		u"trim with rebase"_q);
	const auto resized = SetCanvasSize(document, QSize(200, 200), true);
	context.check(
		resized
			&& resized.document.size() == QSize(200, 200)
			&& resized.document.valueAt({ matte, "ks.p" }, 0.)->point()
				== QPointF(100., 100.)
			&& resized.document.valueAt({ matte, "ks.s" }, 0.)->point()
				== QPointF(200., 200.)
			&& resized.document.valueAt({ matted, "ks.p" }, 0.)->point()
				== QPointF(0., 0.),
		u"canvas resize scales top level layers"_q);

	const auto palette = document.palette();
	context.check(palette.size() == 2, u"palette"_q);
	const auto recolored = ReplaceColor(document, Qt::red, Qt::green);
	context.check(
		recolored && recolored.document.palette().size() == 2
			&& ranges::any_of(recolored.document.palette(), [](
					const PaletteEntry &entry) {
				return entry.color == QColor(Qt::green);
			}),
		u"replace color"_q);
	const auto rotated = AdjustHsl(document, 120, 0, 0);
	context.check(
		rotated && ranges::any_of(rotated.document.palette(), [](
				const PaletteEntry &entry) {
			return entry.color == QColor(0, 255, 0);
		}),
		u"hue rotation"_q);

	auto keyed = AddKeyframe(document, { matte, "ks.o" }, 20., PropValue::Scalar(50.));
	context.check(
		keyed
			&& keyed.document.animated({ matte, "ks.o" })
			&& Near(keyed.document.valueAt({ matte, "ks.o" }, 90)->scalar(), 50.),
		u"single keyframe holds its value"_q);
	if (keyed) {
		keyed = SetValueAt(keyed.document, { matte, "ks.o" }, PropValue::Scalar(100.), 40.);
		context.check(
			keyed
				&& Near(keyed.document.valueAt({ matte, "ks.o" }, 30)->scalar(), 75.)
				&& !HasBrokenKeyframes(keyed.document.propertyJson({ matte, "ks.o" })),
			u"auto keyframe"_q);
	}
	if (keyed) {
		const auto removed = RemoveKeyframes(keyed.document, {
			{ { matte, "ks.o" }, 20. },
			{ { matte, "ks.o" }, 40. },
		});
		context.check(
			removed
				&& !removed.document.animated({ matte, "ks.o" })
				&& Near(removed.document.valueAt({ matte, "ks.o" }, 0)->scalar(), 50.),
			u"removing all keyframes makes the property static"_q);
	}

	const auto validation = Validate(document);
	const auto has = [&](const ValidationResult &result, IssueType type) {
		return ranges::any_of(result.issues, [&](const Issue &issue) {
			return issue.type == type;
		});
	};
	context.check(
		has(validation, IssueType::CanvasSize)
			&& has(validation, IssueType::MissingTgsMarker)
			&& !validation.ok(),
		u"validation finds issues"_q);
	const auto fixed = AutoFix(document);
	const auto after = fixed ? Validate(fixed.document) : ValidationResult();
	context.check(
		fixed && after.ok() && !has(after, IssueType::MissingTgsMarker),
		u"auto fix"_q);
	{
		const auto &layers = document.root().get("layers");
		const auto oriented = Document(document.root().with(
			"layers",
			layers.withItem(0, layers.at(0).with("ao", Number(1)))));
		const auto unoriented = AutoFix(oriented, { IssueType::AutoOrient });
		context.check(
			has(Validate(oriented), IssueType::AutoOrient)
				&& unoriented
				&& !has(Validate(unoriented.document), IssueType::AutoOrient)
				&& has(Validate(unoriented.document), IssueType::CanvasSize),
			u"auto-orient check and fix"_q);
	}

	auto controller = EditorController(document);
	const auto original = controller.document().toJson();
	controller.rename(matte, u"Renamed"_q);
	controller.setHidden({ null }, true);
	controller.setValue({ matte, "ks.o" }, PropValue::Scalar(10.), "drag");
	controller.setValue({ matte, "ks.o" }, PropValue::Scalar(20.), "drag");
	controller.setValue({ matte, "ks.o" }, PropValue::Scalar(30.), "drag");
	context.check(
		controller.canUndo() && controller.dirty(),
		u"controller edits"_q);
	controller.undo();
	context.check(
		Near(controller.document().valueAt({ matte, "ks.o" }, 0)->scalar(), 100.),
		u"merged edits undo as one step"_q);
	while (controller.undo()) {
	}
	context.check(
		controller.document().toJson() == original && !controller.dirty(),
		u"undo restores the original"_q);
	controller.redo();
	controller.redo();
	controller.redo();
	context.check(
		Near(controller.document().valueAt({ matte, "ks.o" }, 0)->scalar(), 30.)
			&& !controller.canRedo(),
		u"redo"_q);

	auto gesture = EditorController(document);
	gesture.beginGesture("gesture");
	gesture.setValue({ matte, "ks.o" }, PropValue::Scalar(10.), "gesture");
	gesture.markSaved(); // Splits the gesture into two undo steps.
	gesture.setValue({ matte, "ks.o" }, PropValue::Scalar(20.), "gesture");
	context.check(
		gesture.cancelGesture("gesture")
			&& gesture.document().root().sameAs(document.root())
			&& !gesture.canUndo()
			&& !gesture.canRedo(),
		u"cancelled gesture reverts all its steps"_q);
}

// Round 4: After Effects features. Hand-written JSON in the shape
// bodymovin exports, with unknown keys ("customRoot", "zz") that have to
// survive everything.

#define OBLIVION_AE_KS(x, y) "\"ks\":{\"o\":{\"a\":0,\"k\":100}," \
	"\"r\":{\"a\":0,\"k\":0},\"p\":{\"a\":0,\"k\":[" #x "," #y ",0]}," \
	"\"a\":{\"a\":0,\"k\":[0,0,0]},\"s\":{\"a\":0,\"k\":[100,100,100]}}"
#define OBLIVION_AE_TR "{\"ty\":\"tr\",\"p\":{\"a\":0,\"k\":[0,0]}," \
	"\"a\":{\"a\":0,\"k\":[0,0]},\"s\":{\"a\":0,\"k\":[100,100]}," \
	"\"r\":{\"a\":0,\"k\":0},\"o\":{\"a\":0,\"k\":100},\"nm\":\"Transform\"}"
#define OBLIVION_AE_GROUP(w, h, color) "{\"ty\":\"gr\",\"it\":[" \
	"{\"ty\":\"rc\",\"d\":1,\"s\":{\"a\":0,\"k\":[" #w "," #h "]}," \
	"\"p\":{\"a\":0,\"k\":[0,0]},\"r\":{\"a\":0,\"k\":0},\"nm\":\"Rect\"}," \
	"{\"ty\":\"fl\",\"c\":{\"a\":0,\"k\":[" color ",1]}," \
	"\"o\":{\"a\":0,\"k\":100},\"r\":1,\"nm\":\"Fill\"}," \
	OBLIVION_AE_TR "],\"nm\":\"Group\"}"
#define OBLIVION_AE_LINEAR "\"i\":{\"x\":[1],\"y\":[1]}," \
	"\"o\":{\"x\":[0],\"y\":[0]}"
#define OBLIVION_AE_SQUARE(a) "{\"i\":[[0,0],[0,0],[0,0],[0,0]]," \
	"\"o\":[[0,0],[0,0],[0,0],[0,0]],\"v\":[[-" #a ",-" #a "],[" #a ",-" #a \
	"],[" #a "," #a "],[-" #a "," #a "]],\"c\":true}"
#define OBLIVION_AE_TAIL "\"ip\":0,\"op\":60,\"st\":0,\"bm\":0}"

constexpr auto kAeDocument = "{\"v\":\"5.5.2\",\"fr\":60,\"ip\":0,\"op\":60,"
"\"w\":100,\"h\":100,\"nm\":\"Ae\",\"ddd\":0,"
"\"customRoot\":{\"a\":[1,2,{\"b\":null}]},\"assets\":[],\"layers\":["
"{\"ddd\":0,\"ind\":1,\"ty\":4,\"nm\":\"Masked\",\"sr\":1,"
OBLIVION_AE_KS(50, 50) ",\"ao\":0,\"hasMask\":true,\"masksProperties\":["
"{\"inv\":false,\"mode\":\"a\",\"pt\":{\"a\":0,\"k\":"
"{\"i\":[[0,0],[0,0],[0,0],[0,0]],\"o\":[[0,0],[0,0],[0,0],[0,0]],"
"\"v\":[[-30,-30],[0,-30],[0,30],[-30,30]],\"c\":true}},"
"\"o\":{\"a\":0,\"k\":100},\"x\":{\"a\":0,\"k\":0},\"nm\":\"Mask A\","
"\"zz\":123},"
"{\"inv\":true,\"mode\":\"s\",\"pt\":{\"a\":1,\"k\":[{\"i\":{\"x\":0.833,"
"\"y\":0.833},\"o\":{\"x\":0.167,\"y\":0.167},\"t\":0,\"s\":["
OBLIVION_AE_SQUARE(10) "]},{\"t\":40,\"s\":[" OBLIVION_AE_SQUARE(20) "]}]},"
"\"o\":{\"a\":1,\"k\":[{" OBLIVION_AE_LINEAR ",\"t\":0,\"s\":[0]},"
"{\"t\":40,\"s\":[100]}]},\"x\":{\"a\":0,\"k\":5},"
"\"f\":{\"a\":0,\"k\":[3,3]},\"nm\":\"Mask B\"}],"
"\"shapes\":[" OBLIVION_AE_GROUP(60, 60, "1,0,0") "]," OBLIVION_AE_TAIL ","
"{\"ddd\":0,\"ind\":2,\"ty\":4,\"nm\":\"MatteSrc\",\"td\":1,\"sr\":1,"
OBLIVION_AE_KS(50, 50) ",\"ao\":0,\"shapes\":["
OBLIVION_AE_GROUP(30, 30, "1,1,1") "]," OBLIVION_AE_TAIL ","
"{\"ddd\":0,\"ind\":3,\"ty\":4,\"nm\":\"Luma\",\"tt\":3,\"tp\":2,\"sr\":1,"
OBLIVION_AE_KS(50, 50) ",\"ao\":0,\"shapes\":["
OBLIVION_AE_GROUP(60, 60, "0,0,1") "]," OBLIVION_AE_TAIL ","
"{\"ddd\":0,\"ind\":4,\"ty\":4,\"nm\":\"Styled\",\"parent\":5,\"sr\":1,"
OBLIVION_AE_KS(10, 5) ",\"ao\":0,\"shapes\":[{\"ty\":\"gr\",\"it\":["
"{\"ty\":\"sh\",\"d\":1,\"ks\":{\"a\":0,\"k\":{\"i\":[[0,0],[0,0],[0,0]],"
"\"o\":[[0,0],[0,0],[0,0]],\"v\":[[0,-30],[30,30],[-30,30]],\"c\":true}},"
"\"nm\":\"Tri\"},"
"{\"ty\":\"rd\",\"r\":{\"a\":0,\"k\":10},\"nm\":\"Round\"},"
"{\"ty\":\"tm\",\"s\":{\"a\":0,\"k\":0},\"e\":{\"a\":1,\"k\":[{"
OBLIVION_AE_LINEAR ",\"t\":0,\"s\":[0]},{\"t\":40,\"s\":[100]}]},"
"\"o\":{\"a\":0,\"k\":0},\"m\":2,\"nm\":\"Trim\"},"
"{\"ty\":\"gf\",\"o\":{\"a\":0,\"k\":100},\"r\":2,\"g\":{\"p\":3,\"k\":"
"{\"a\":0,\"k\":[0,1,0,0,0.5,0,1,0,1,0,0,1,0,1,1,0.5]}},"
"\"s\":{\"a\":0,\"k\":[-30,0]},\"e\":{\"a\":0,\"k\":[30,0]},\"t\":2,"
"\"h\":{\"a\":0,\"k\":20},\"a\":{\"a\":0,\"k\":45},\"nm\":\"GFill\"},"
"{\"ty\":\"gs\",\"o\":{\"a\":0,\"k\":100},\"w\":{\"a\":0,\"k\":6},"
"\"g\":{\"p\":2,\"k\":{\"a\":1,\"k\":[{" OBLIVION_AE_LINEAR ",\"t\":0,"
"\"s\":[0,1,0,0,1,0,0,1]},{\"t\":40,\"s\":[0,0,1,0,1,1,1,0]}]}},"
"\"s\":{\"a\":0,\"k\":[-30,0]},\"e\":{\"a\":0,\"k\":[30,0]},\"t\":1,"
"\"lc\":1,\"lj\":3,\"ml\":7,\"d\":["
"{\"n\":\"d\",\"nm\":\"dash\",\"v\":{\"a\":0,\"k\":10}},"
"{\"n\":\"g\",\"nm\":\"gap\",\"v\":{\"a\":0,\"k\":5}},"
"{\"n\":\"o\",\"nm\":\"offset\",\"v\":{\"a\":1,\"k\":[{" OBLIVION_AE_LINEAR
",\"t\":0,\"s\":[0]},{\"t\":40,\"s\":[30]}]}}],\"nm\":\"GStroke\"},"
"{\"ty\":\"rp\",\"c\":{\"a\":0,\"k\":3},\"o\":{\"a\":0,\"k\":0},\"m\":1,"
"\"tr\":{\"ty\":\"tr\",\"p\":{\"a\":0,\"k\":[20,0]},"
"\"a\":{\"a\":0,\"k\":[0,0]},\"s\":{\"a\":0,\"k\":[100,100]},"
"\"r\":{\"a\":0,\"k\":0},\"so\":{\"a\":0,\"k\":100},"
"\"eo\":{\"a\":0,\"k\":50},\"nm\":\"Transform\"},\"nm\":\"Rep\"},"
OBLIVION_AE_TR "],\"nm\":\"Art\"}]," OBLIVION_AE_TAIL ","
"{\"ddd\":0,\"ind\":5,\"ty\":3,\"nm\":\"Parent\",\"sr\":1,\"ks\":{"
"\"o\":{\"a\":0,\"k\":100},\"r\":{\"a\":0,\"k\":90},\"p\":{\"a\":1,\"k\":[{"
"\"i\":{\"x\":1,\"y\":1},\"o\":{\"x\":0,\"y\":0},\"t\":0,\"s\":[20,20,0]},"
"{\"t\":40,\"s\":[60,20,0]}]},\"a\":{\"a\":0,\"k\":[0,0,0]},"
"\"s\":{\"a\":0,\"k\":[200,200,100]}},\"ao\":0," OBLIVION_AE_TAIL "]}";

// One 100x100 composition for the rendering checks.
[[nodiscard]] QByteArray AeComposition(const QByteArray &layers) {
	return QByteArray("{\"v\":\"5.5.2\",\"fr\":60,\"ip\":0,\"op\":60,"
		"\"w\":100,\"h\":100,\"nm\":\"Render\",\"ddd\":0,\"assets\":[],"
		"\"layers\":[") + layers + QByteArray("]}");
}

// A shape layer at (x, y) with the given "shapes" items.
[[nodiscard]] QByteArray AeLayer(
		int ind,
		const char *name,
		int x,
		int y,
		const QByteArray &shapes,
		const QByteArray &extra = QByteArray()) {
	return QByteArray("{\"ddd\":0,\"ind\":") + QByteArray::number(ind)
		+ QByteArray(",\"ty\":4,\"nm\":\"") + QByteArray(name)
		+ QByteArray("\",\"sr\":1,") + extra
		+ QByteArray("\"ks\":{\"o\":{\"a\":0,\"k\":100},"
			"\"r\":{\"a\":0,\"k\":0},\"p\":{\"a\":0,\"k\":[")
		+ QByteArray::number(x) + QByteArray(",") + QByteArray::number(y)
		+ QByteArray(",0]},\"a\":{\"a\":0,\"k\":[0,0,0]},"
			"\"s\":{\"a\":0,\"k\":[100,100,100]}},\"ao\":0,\"shapes\":[")
		+ shapes
		+ QByteArray("]," OBLIVION_AE_TAIL);
}

[[nodiscard]] bool SameMatrix(
		const QTransform &a,
		const QTransform &b,
		double epsilon = 1e-2) {
	return Near(a.m11(), b.m11(), epsilon)
		&& Near(a.m12(), b.m12(), epsilon)
		&& Near(a.m21(), b.m21(), epsilon)
		&& Near(a.m22(), b.m22(), epsilon)
		&& Near(a.dx(), b.dx(), epsilon)
		&& Near(a.dy(), b.dy(), epsilon);
}

[[nodiscard]] bool SameColor(const QColor &a, const QColor &b) {
	return std::abs(a.red() - b.red()) <= 2
		&& std::abs(a.green() - b.green()) <= 2
		&& std::abs(a.blue() - b.blue()) <= 2;
}

[[nodiscard]] bool HasIssue(const ValidationResult &result, IssueType type) {
	return result.find(type) != nullptr;
}

[[nodiscard]] std::optional<Issue> FindIssue(
		const Document &document,
		IssueType type) {
	const auto result = Validate(document, { .measureSize = false });
	const auto found = result.find(type);
	return found ? std::make_optional(*found) : std::nullopt;
}

[[nodiscard]] bool SameJson(const Document &a, const Document &b) {
	return a.valid() && b.valid() && (a.root() == b.root());
}

void TestAeModel(TestContext &context) {
	auto error = QString();
	const auto document = Document::FromJson(kAeDocument, &error);
	context.check(document.valid(), u"ae: test document parses: "_q + error);
	if (!document.valid()) {
		return;
	}
	const auto original = Json::Parse(kAeDocument);
	const auto reparsed = Json::Parse(document.toJson());
	context.check(
		original && reparsed && (*original == *reparsed),
		u"ae: round trip keeps masks, mattes, modifiers and unknown keys"_q);

	const auto masked = FindByName(document, u"Masked"_q);
	const auto maskA = FindByName(document, u"Mask A"_q);
	const auto maskB = FindByName(document, u"Mask B"_q);
	const auto source = FindByName(document, u"MatteSrc"_q);
	const auto luma = FindByName(document, u"Luma"_q);
	const auto styled = FindByName(document, u"Styled"_q);
	const auto parent = FindByName(document, u"Parent"_q);
	const auto trim = FindByName(document, u"Trim"_q);
	const auto gfill = FindByName(document, u"GFill"_q);
	const auto gstroke = FindByName(document, u"GStroke"_q);
	const auto repeater = FindByName(document, u"Rep"_q);
	const auto round = FindByName(document, u"Round"_q);
	context.check(
		masked && maskA && maskB && source && luma && styled && parent
			&& trim && gfill && gstroke && repeater && round,
		u"ae: nodes are indexed"_q);
	if (!masked || !maskA || !maskB || !source || !luma || !styled
		|| !parent || !trim || !gfill || !gstroke || !repeater || !round) {
		return;
	}

	// Masks.
	const auto maskedNode = document.node(masked);
	const auto a = document.node(maskA);
	const auto b = document.node(maskB);
	context.check(
		maskedNode->masksEnabled
			&& maskedNode->masks == std::vector<NodeId>{ maskA, maskB }
			&& a->kind == NodeKind::Mask
			&& a->maskMode == MaskMode::Add
			&& !a->maskInverted
			&& b->maskMode == MaskMode::Subtract
			&& b->maskInverted
			&& b->layer == masked,
		u"ae: mask modes and flags"_q);
	auto roles = std::vector<PropertyRole>();
	for (const auto &info : document.properties(maskB)) {
		roles.push_back(info.role);
	}
	context.check(
		roles == std::vector<PropertyRole>{
			PropertyRole::MaskPath,
			PropertyRole::MaskOpacity,
			PropertyRole::MaskExpansion,
			PropertyRole::MaskFeather,
		},
		u"ae: mask properties"_q);
	const auto pathInfo = document.property({ maskB, "pt" });
	const auto middle = document.valueAt({ maskB, "pt" }, 20.);
	context.check(
		pathInfo
			&& pathInfo->type == PropertyType::Path
			&& pathInfo->animated
			&& pathInfo->keyframes == 2
			&& middle
			&& middle->path
			&& middle->path->vertices.size() == 4
			&& Near(middle->path->vertices[0], QPointF(-15., -15.), 0.05)
			&& Near(middle->path->vertices[2], QPointF(15., 15.), 0.05),
		u"ae: animated mask path is interpolated"_q);
	context.check(
		Near(document.valueAt({ maskB, "o" }, 20.)->scalar(), 50., 1e-3)
			&& Near(document.valueAt({ maskB, "x" }, 0.)->scalar(), 5.)
			&& document.valueAt({ maskB, "f" }, 0.)->point()
				== QPointF(3., 3.)
			&& !document.valueAt({ maskA, "f" }, 0.)
			&& document.defaultValue({ maskA, "f" })
			&& document.defaultValue({ maskA, "f" })->point() == QPointF()
			&& Near(document.defaultValue({ maskA, "o" })->scalar(), 100.)
			&& !document.defaultValue({ maskA, "pt" }),
		u"ae: mask opacity, expansion, feather and defaults"_q);
	const auto outline = document.outlineAt(maskA, 0.).boundingRect();
	context.check(
		Near(outline.left(), 20., 0.01)
			&& Near(outline.right(), 50., 0.01)
			&& Near(outline.top(), 20., 0.01)
			&& Near(outline.bottom(), 80., 0.01),
		u"ae: mask outline in canvas coordinates"_q);

	// Track mattes.
	const auto lumaNode = document.node(luma);
	const auto sourceNode = document.node(source);
	context.check(
		lumaNode->matte == MatteMode::Luma
			&& lumaNode->matteLayer == source
			&& lumaNode->matteParentInd == 2
			&& !lumaNode->matteTarget
			&& sourceNode->matteSource
			&& sourceNode->matteTarget == luma
			&& !sourceNode->matteLayer
			&& !maskedNode->matteLayer
			&& !maskedNode->matteTarget,
		u"ae: track matte links"_q);

	// Parenting.
	context.check(
		document.node(styled)->parentLayer == parent
			&& document.node(styled)->parentInd == 5
			&& !document.node(parent)->parentLayer,
		u"ae: parent links"_q);
	{
		// (10, 5) scaled by 200%, turned by 90 degrees, moved to (20, 20).
		const auto point = document.transformAt(styled, 0.).map(QPointF());
		const auto later = document.transformAt(styled, 40.).map(QPointF());
		context.check(
			Near(point, QPointF(10., 40.), 0.01)
				&& Near(later, QPointF(50., 40.), 0.01),
			u"ae: a child follows its animated parent"_q);
	}

	// Shape options.
	const auto gfillNode = document.node(gfill);
	const auto gstrokeNode = document.node(gstroke);
	context.check(
		gfillNode->shapeType == ShapeType::GradientFill
			&& gfillNode->gradientType == GradientType::Radial
			&& gfillNode->fillRule == FillRule::EvenOdd
			&& gstrokeNode->shapeType == ShapeType::GradientStroke
			&& gstrokeNode->gradientType == GradientType::Linear
			&& gstrokeNode->lineCap == LineCap::Butt
			&& gstrokeNode->lineJoin == LineJoin::Bevel
			&& Near(gstrokeNode->miterLimit, 7.)
			&& gstrokeNode->dashValues == 3
			&& document.node(trim)->trimMode == TrimMode::Individually
			&& document.node(round)->shapeType == ShapeType::RoundCorners,
		u"ae: gradient, stroke and trim options"_q);

	// Gradients.
	const auto fillStops = document.gradientAt(gfill, 0.);
	context.check(
		fillStops
			&& fillStops->colors.size() == 3
			&& fillStops->alphas.size() == 2
			&& Near(fillStops->colors[1].offset, 0.5)
			&& SameColor(fillStops->colors[1].color, QColor(0, 255, 0))
			&& Near(fillStops->alphas[1].offset, 1.)
			&& Near(fillStops->alphas[1].color.alphaF(), 0.5, 0.01)
			&& document.property({ gfill, "g.k" })
			&& document.property({ gfill, "g.k" })->colorStops == 3
			&& Near(document.valueAt({ gfill, "h" }, 0.)->scalar(), 20.)
			&& Near(document.valueAt({ gfill, "a" }, 0.)->scalar(), 45.),
		u"ae: gradient fill stops, opacity stops and highlight"_q);
	const auto strokeStops = document.gradientAt(gstroke, 20.);
	context.check(
		strokeStops
			&& strokeStops->colors.size() == 2
			&& strokeStops->alphas.empty()
			&& SameColor(strokeStops->colors[0].color, QColor(128, 128, 0))
			&& SameColor(strokeStops->colors[1].color, QColor(128, 128, 128))
			&& !document.gradientAt(trim, 0.),
		u"ae: animated gradient stroke stops"_q);
	{
		const auto data = *fillStops;
		const auto encoded = EncodeGradient(data);
		const auto mixed = GradientColorAt(data, 0.25);
		auto single = data;
		single.alphas.resize(1);
		auto shuffled = data;
		std::swap(shuffled.colors[0], shuffled.colors[2]);
		context.check(
			encoded.numbers == document.valueAt({ gfill, "g.k" }, 0.)->numbers
				&& DecodeGradient(encoded, 3) == data
				&& SameColor(mixed, QColor(128, 128, 0))
				&& Near(mixed.alphaF(), 0.875, 0.01)
				&& Near(GradientColorAt(data, 2.).alphaF(), 0.5, 0.01)
				&& EncodeGradient(single).numbers.size() == 16
				&& EncodeGradient(shuffled).numbers == encoded.numbers,
			u"ae: gradient encode / decode / sample"_q);
	}

	// Dashes, trim, repeater.
	const auto dashes = DashesOf(document, gstroke);
	context.check(
		dashes.dashes == std::vector<PropertyRef>{ { gstroke, "d.0.v" } }
			&& dashes.gaps == std::vector<PropertyRef>{ { gstroke, "d.1.v" } }
			&& dashes.offset == PropertyRef{ gstroke, "d.2.v" }
			&& Near(document.valueAt(dashes.dashes[0], 0.)->scalar(), 10.)
			&& Near(document.valueAt(dashes.gaps[0], 0.)->scalar(), 5.)
			&& Near(document.valueAt(dashes.offset, 20.)->scalar(), 15., 1e-3)
			&& document.property(dashes.offset)->role == PropertyRole::Dash
			&& document.property(dashes.offset)->dashOffset
			&& !document.property(dashes.dashes[0])->dashOffset
			&& !document.property(dashes.gaps[0])->dashOffset
			&& DashesOf(document, trim).empty(),
		u"ae: stroke dashes"_q);
	context.check(
		Near(document.valueAt({ trim, "e" }, 20.)->scalar(), 50., 1e-3)
			&& Near(document.valueAt({ trim, "s" }, 20.)->scalar(), 0.),
		u"ae: trim paths values"_q);
	roles.clear();
	for (const auto &info : document.properties(repeater)) {
		roles.push_back(info.role);
	}
	context.check(
		ranges::contains(roles, PropertyRole::Copies)
			&& ranges::contains(roles, PropertyRole::Offset)
			&& ranges::contains(roles, PropertyRole::Position)
			&& ranges::contains(roles, PropertyRole::StartOpacity)
			&& ranges::contains(roles, PropertyRole::EndOpacity)
			&& Near(document.valueAt({ repeater, "c" }, 0.)->scalar(), 3.)
			&& document.valueAt({ repeater, "tr.p" }, 0.)->point()
				== QPointF(20., 0.)
			&& TransformProperty(
				document,
				repeater,
				TransformField::Position) == PropertyRef{ repeater, "tr.p" },
		u"ae: repeater properties"_q);
	context.check(
		Near(document.valueAt({ round, "r" }, 0.)->scalar(), 10.)
			&& document.property({ round, "r" })->role == PropertyRole::Radius,
		u"ae: round corners radius"_q);
}

void TestAeMasks(TestContext &context) {
	const auto document = Document::FromJson(kAeDocument);
	if (!document.valid()) {
		return;
	}
	const auto masked = FindByName(document, u"Masked"_q);
	const auto maskA = FindByName(document, u"Mask A"_q);
	const auto maskB = FindByName(document, u"Mask B"_q);
	const auto styled = FindByName(document, u"Styled"_q);
	const auto unknownKept = [&](const Document &edited) {
		return edited.root().get("customRoot") == document.root().get("customRoot")
			&& (!edited.contains(maskA)
				|| edited.json(maskA).get("zz").toInt() == 123);
	};

	const auto defaultPath = DefaultMaskPath(document, styled, 0.);
	context.check(
		defaultPath.closed
			&& defaultPath.vertices.size() == 4
			&& PainterPath(defaultPath).boundingRect().width() > 50.,
		u"ae: default mask path covers the layer content"_q);
	const auto added = AddMask(
		document,
		styled,
		EllipsePath(QRectF(-20., -20., 40., 40.)),
		MaskMode::Intersect,
		u"Oval"_q);
	context.check(
		added
			&& added.created.size() == 1
			&& added.structural
			&& added.document.node(styled)->masksEnabled
			&& added.document.node(styled)->masks.size() == 1
			&& added.document.json(styled).get("hasMask").isBool()
			&& unknownKept(added.document),
		u"ae: add the first mask turns hasMask on"_q);
	if (added) {
		const auto mask = added.created.front();
		const auto node = added.document.node(mask);
		const auto path = added.document.valueAt({ mask, "pt" }, 0.);
		context.check(
			node
				&& node->kind == NodeKind::Mask
				&& node->maskMode == MaskMode::Intersect
				&& !node->maskInverted
				&& node->name == u"Oval"_q
				&& path
				&& path->path
				&& path->path->closed
				&& path->path->vertices.size() == 4
				&& Near(path->path->vertices[0], QPointF(0., -20.))
				&& Near(added.document.valueAt({ mask, "o" }, 0.)->scalar(), 100.)
				&& Near(added.document.valueAt({ mask, "x" }, 0.)->scalar(), 0.),
			u"ae: added mask content"_q);
		const auto reparsed = Document::FromJson(added.document.toJson());
		context.check(
			reparsed.valid()
				&& SameJson(reparsed, added.document)
				&& reparsed.node(FindByName(reparsed, u"Oval"_q))
				&& reparsed.node(FindByName(reparsed, u"Styled"_q))->masksEnabled,
			u"ae: added mask survives a round trip"_q);
		const auto removed = DeleteNodes(added.document, { mask });
		context.check(
			removed
				&& removed.document.node(styled)->masks.empty()
				&& !removed.document.node(styled)->masksEnabled,
			u"ae: deleting the last mask turns hasMask off"_q);
	}
	context.check(
		!AddMask(document, maskA, defaultPath).ok()
			&& !AddMask(document, styled, PathData()).ok(),
		u"ae: add mask rejects bad input"_q);

	const auto mode = SetMaskMode(document, maskA, MaskMode::Difference);
	const auto inverted = SetMaskInverted(document, maskA, true);
	context.check(
		mode
			&& mode.document.node(maskA)->maskMode == MaskMode::Difference
			&& mode.document.json(maskA).get("mode").toString() == u"f"_q
			&& inverted
			&& inverted.document.node(maskA)->maskInverted
			&& unknownKept(mode.document)
			&& unknownKept(inverted.document)
			&& SetMaskMode(document, maskA, MaskMode::Add).document.sameAs(
				document)
			&& !SetMaskMode(document, masked, MaskMode::Add).ok(),
		u"ae: mask mode and inverted"_q);

	// Feather is not in "Mask A": created on the first write.
	const auto feather = SetValueAt(
		document,
		{ maskA, "f" },
		PropValue::Point(QPointF(4., 6.)),
		0.);
	context.check(
		feather
			&& feather.document.valueAt({ maskA, "f" }, 0.)
			&& feather.document.valueAt({ maskA, "f" }, 0.)->point()
				== QPointF(4., 6.)
			&& feather.document.property({ maskA, "f" })
			&& feather.document.property({ maskA, "f" })->role
				== PropertyRole::MaskFeather
			&& unknownKept(feather.document),
		u"ae: a missing mask property is created on write"_q);
	const auto keyed = AddKeyframe(
		document,
		{ maskA, "x" },
		10.,
		PropValue::Scalar(8.));
	context.check(
		keyed
			&& keyed.document.animated({ maskA, "x" })
			&& Near(keyed.document.valueAt({ maskA, "x" }, 30.)->scalar(), 8.),
		u"ae: mask expansion keyframe"_q);

	// Order, copies.
	const auto moved = MoveNode(document, maskB, 0, 0);
	context.check(
		moved
			&& moved.document.node(masked)->masks
				== std::vector<NodeId>{ maskB, maskA }
			&& MoveNode(document, maskA, 0, 0).document.sameAs(document)
			&& !MoveNode(document, maskA, styled, 0).ok(),
		u"ae: masks are reordered inside their layer"_q);
	const auto reordered = ReorderNode(document, maskA, 1);
	context.check(
		reordered
			&& reordered.document.node(masked)->masks
				== std::vector<NodeId>{ maskB, maskA },
		u"ae: mask reorder by delta"_q);
	const auto duplicated = DuplicateNodes(document, { maskB });
	context.check(
		duplicated
			&& duplicated.created.size() == 1
			&& duplicated.document.node(masked)->masks.size() == 3
			&& duplicated.document.node(duplicated.created[0])->maskInverted,
		u"ae: duplicate a mask"_q);

	// Path structure on an animated path: every keyframe changes.
	const auto path = PropertyRef{ maskB, "pt" };
	const auto inserted = InsertPathVertex(document, path, 0, 0.5);
	context.check(inserted.ok(), u"ae: insert a path vertex"_q);
	if (inserted) {
		const auto first = inserted.document.valueAt(path, 0.);
		const auto last = inserted.document.valueAt(path, 40.);
		context.check(
			first->path->vertices.size() == 5
				&& last->path->vertices.size() == 5
				&& Near(first->path->vertices[1], QPointF(0., -10.))
				&& Near(last->path->vertices[1], QPointF(0., -20.))
				&& !HasIssue(
					Validate(inserted.document, { .measureSize = false }),
					IssueType::PathVertices),
			u"ae: the vertex is inserted into every keyframe"_q);
		const auto removed = RemovePathVertex(inserted.document, path, 1);
		context.check(
			removed && SameJson(removed.document, document),
			u"ae: removing the vertex restores the path"_q);
	}
	const auto opened = SetPathClosed(document, path, false);
	context.check(
		opened
			&& !opened.document.valueAt(path, 0.)->path->closed
			&& !opened.document.valueAt(path, 40.)->path->closed
			&& SetPathClosed(document, path, true).document.sameAs(document),
		u"ae: open / close a path"_q);
	const auto reversed = ReversePath(document, path);
	context.check(
		reversed
			&& Near(
				reversed.document.valueAt(path, 0.)->path->vertices[1],
				QPointF(-10., 10.))
			&& SameJson(
				ReversePath(reversed.document, path).document,
				document),
		u"ae: reverse a path twice"_q);
	{
		const auto staticPath = PropertyRef{ maskA, "pt" };
		auto current = RemovePathVertex(document, staticPath, 0);
		auto two = current
			? RemovePathVertex(current.document, staticPath, 0)
			: Edit();
		context.check(
			current
				&& two
				&& two.document.valueAt(staticPath, 0.)->path->vertices.size()
					== 2
				&& !RemovePathVertex(two.document, staticPath, 0).ok()
				&& !InsertPathVertex(document, staticPath, 9, 0.5).ok()
				&& !InsertPathVertex(document, { maskA, "o" }, 0, 0.5).ok(),
			u"ae: path edits keep two vertices and check indices"_q);
	}

	// Undo through the controller restores the exact original.
	auto controller = EditorController(document);
	const auto before = controller.document().toJson();
	controller.addMask(styled, defaultPath);
	const auto selected = controller.primarySelection();
	context.check(
		controller.document().node(selected)
			&& controller.document().node(selected)->kind == NodeKind::Mask
			&& controller.undoCommand() == Command::AddMask,
		u"ae: controller selects the added mask"_q);
	controller.setMaskMode(selected, MaskMode::Subtract);
	controller.setMaskInverted(selected, true);
	controller.insertPathVertex({ selected, "pt" }, 1, 0.25);
	controller.setValue({ selected, "o" }, PropValue::Scalar(40.));
	while (controller.undo()) {
	}
	context.check(
		controller.document().toJson() == before,
		u"ae: undo of mask edits restores the original"_q);
}

void TestAeMattesAndParents(TestContext &context) {
	const auto document = Document::FromJson(kAeDocument);
	if (!document.valid()) {
		return;
	}
	const auto masked = FindByName(document, u"Masked"_q);
	const auto source = FindByName(document, u"MatteSrc"_q);
	const auto luma = FindByName(document, u"Luma"_q);
	const auto styled = FindByName(document, u"Styled"_q);
	const auto parent = FindByName(document, u"Parent"_q);
	const auto names = [](const Document &edited) {
		return LayerNames(edited);
	};

	const auto cleared = SetTrackMatte(document, luma, MatteMode::None);
	context.check(
		cleared
			&& cleared.document.node(luma)->matte == MatteMode::None
			&& !cleared.document.json(luma).has("tt")
			&& !cleared.document.json(luma).has("tp")
			&& !cleared.document.node(source)->matteSource
			&& !cleared.document.node(source)->matteTarget
			&& names(cleared.document) == names(document),
		u"ae: removing a track matte frees the matte layer"_q);
	const auto inverted = SetTrackMatte(
		document,
		luma,
		MatteMode::AlphaInverted);
	context.check(
		inverted
			&& inverted.document.node(luma)->matte == MatteMode::AlphaInverted
			&& inverted.document.node(luma)->matteLayer == source
			&& inverted.document.node(luma)->matteParentInd == 2
			&& names(inverted.document) == names(document)
			&& SetTrackMatte(document, luma, MatteMode::Luma).document.sameAs(
				document),
		u"ae: changing the matte mode keeps the matte layer"_q);

	// A matte layer from below: it is moved right above the matted one.
	const auto picked = SetTrackMatte(
		document,
		masked,
		MatteMode::Alpha,
		parent);
	context.check(
		picked
			&& names(picked.document) == std::vector<QString>{
				u"Parent"_q,
				u"Masked"_q,
				u"MatteSrc"_q,
				u"Luma"_q,
				u"Styled"_q,
			}
			&& picked.document.node(masked)->matte == MatteMode::Alpha
			&& picked.document.node(masked)->matteLayer == parent
			&& picked.document.node(parent)->matteSource
			&& picked.document.node(parent)->matteTarget == masked
			&& picked.document.node(luma)->matteLayer == source
			&& picked.document.node(styled)->parentLayer == parent,
		u"ae: a picked matte layer moves above the matted layer"_q);
	// Replacing the matte of "Luma": the old one becomes a plain layer.
	const auto replaced = SetTrackMatte(document, luma, MatteMode::Luma, parent);
	context.check(
		replaced
			&& names(replaced.document) == std::vector<QString>{
				u"Masked"_q,
				u"MatteSrc"_q,
				u"Parent"_q,
				u"Luma"_q,
				u"Styled"_q,
			}
			&& replaced.document.node(luma)->matteLayer == parent
			&& replaced.document.node(luma)->matteParentInd == 5
			&& !replaced.document.node(source)->matteSource
			&& !replaced.document.node(source)->matteTarget,
		u"ae: replacing the matte layer"_q);
	context.check(
		!SetTrackMatte(document, masked, MatteMode::Alpha).ok()
			&& !SetTrackMatte(document, luma, MatteMode::Alpha, luma).ok()
			&& !SetTrackMatte(document, styled, MatteMode::Alpha, luma).ok()
			&& !SetTrackMatte(document, styled, MatteMode::Alpha, source).ok()
			&& !SetTrackMatte(document, source, MatteMode::Alpha, masked).ok()
			&& SetTrackMatte(document, masked, MatteMode::None).document.sameAs(
				document),
		u"ae: track matte refuses layouts the renderer can't draw"_q);
	const auto above = SetTrackMatte(document, styled, MatteMode::Luma);
	context.check(
		!above.ok(),
		u"ae: a matted layer can't be used as a matte"_q);
	const auto plain = SetTrackMatte(document, parent, MatteMode::Alpha);
	context.check(
		plain
			&& plain.document.node(parent)->matteLayer == styled
			&& plain.document.node(styled)->matteSource
			&& names(plain.document) == names(document),
		u"ae: the layer above becomes the matte by default"_q);

	// Copies of a pair keep their own link.
	const auto duplicated = DuplicateNodes(document, { luma });
	if (duplicated && duplicated.created.size() == 2) {
		const auto sourceCopy = duplicated.document.node(duplicated.created[0]);
		const auto lumaCopy = duplicated.document.node(duplicated.created[1]);
		context.check(
			sourceCopy
				&& lumaCopy
				&& lumaCopy->matteLayer == sourceCopy->id
				&& lumaCopy->matteParentInd == sourceCopy->ind
				&& duplicated.document.node(luma)->matteParentInd == 2,
			u"ae: a duplicated matte pair links to its own copy"_q);
	} else {
		context.check(false, u"ae: duplicate a matte pair"_q);
	}

	// Parenting.
	context.check(
		CanSetLayerParent(document, masked, parent)
			&& CanSetLayerParent(document, styled, 0)
			&& !CanSetLayerParent(document, parent, styled)
			&& !CanSetLayerParent(document, styled, styled)
			&& !CanSetLayerParent(document, styled, 12345)
			&& !SetLayerParent(document, parent, styled).ok()
			&& SetLayerParent(document, styled, parent).document.sameAs(
				document),
		u"ae: parent links can't make a cycle"_q);
	const auto world = document.transformAt(styled, 0.);
	const auto detached = SetLayerParent(document, styled, 0, 0.);
	context.check(
		detached
			&& !detached.document.node(styled)->parentLayer
			&& !detached.document.json(styled).has("parent")
			&& SameMatrix(detached.document.transformAt(styled, 0.), world)
			&& Near(
				detached.document.valueAt({ styled, "ks.p" }, 0.)->point(),
				QPointF(10., 40.),
				0.01)
			&& Near(
				detached.document.valueAt({ styled, "ks.r" }, 0.)->scalar(),
				90.,
				0.01)
			&& Near(
				detached.document.valueAt({ styled, "ks.s" }, 0.)->point(),
				QPointF(200., 200.),
				0.01),
		u"ae: unparenting keeps the layer in place"_q);
	if (detached) {
		const auto attached = SetLayerParent(
			detached.document,
			styled,
			parent,
			0.);
		context.check(
			attached
				&& attached.document.node(styled)->parentLayer == parent
				&& SameMatrix(attached.document.transformAt(styled, 0.), world)
				&& Near(
					attached.document.valueAt({ styled, "ks.p" }, 0.)->point(),
					QPointF(10., 5.),
					0.01)
				&& Near(
					attached.document.valueAt({ styled, "ks.s" }, 0.)->point(),
					QPointF(100., 100.),
					0.01),
			u"ae: parenting back restores the values"_q);
	}
	const auto jumped = SetLayerParent(document, styled, 0);
	context.check(
		jumped
			&& jumped.document.valueAt({ styled, "ks.p" }, 0.)->point()
				== QPointF(10., 5.)
			&& !SameMatrix(jumped.document.transformAt(styled, 0.), world),
		u"ae: unparenting without keeping the place"_q);
	const auto maskedWorld = document.transformAt(masked, 20.);
	const auto adopted = SetLayerParent(document, masked, parent, 20.);
	context.check(
		adopted
			&& adopted.document.node(masked)->parentLayer == parent
			&& adopted.document.json(masked).get("parent").toInt() == 5
			&& SameMatrix(
				adopted.document.transformAt(masked, 20.),
				maskedWorld)
			&& !SameMatrix(
				adopted.document.transformAt(masked, 40.),
				maskedWorld),
		u"ae: parenting keeps the layer in place at the frame"_q);
	{
		// Two layers with one "ind": the renderer takes the first one.
		const auto &layers = document.root().get("layers");
		const auto twin = Document(document.root().with(
			"layers",
			layers.withItem(0, layers.at(0).with("ind", Number(5)))));
		const auto fixed = SetLayerParent(twin, luma, parent);
		context.check(
			twin.node(styled)->parentLayer == masked
				&& fixed
				&& fixed.document.node(luma)->parentLayer == parent
				&& fixed.document.node(parent)->ind == 6
				&& fixed.document.node(styled)->parentLayer == masked,
			u"ae: a parent with a duplicated ind gets a unique one"_q);
	}
	{
		// A cycle in the file: rlottie drops the link that closes it.
		const auto &layers = document.root().get("layers");
		const auto cycle = Document(document.root().with(
			"layers",
			layers.withItem(4, layers.at(4).with("parent", Number(4)))));
		const auto validation = Validate(cycle, { .measureSize = false });
		const auto issue = validation.find(IssueType::ParentLinks);
		const auto fixed = AutoFix(cycle, { IssueType::ParentLinks });
		context.check(
			cycle.node(styled)->parentLayer == parent
				&& !cycle.node(parent)->parentLayer
				&& issue
				&& issue->nodes == std::vector<NodeId>{ parent }
				&& issue->category == IssueCategory::NotRendered
				&& fixed
				&& !fixed.document.json(parent).has("parent")
				&& fixed.document.node(styled)->parentLayer == parent
				&& SameMatrix(
					fixed.document.transformAt(styled, 10.),
					cycle.transformAt(styled, 10.)),
			u"ae: parent cycles are reported and fixed"_q);
	}

	auto controller = EditorController(document);
	const auto before = controller.document().toJson();
	controller.setTrackMatte(masked, MatteMode::Alpha, parent);
	controller.setLayerParent(luma, parent);
	controller.setTrackMatte(luma, MatteMode::None);
	context.check(
		controller.undoCommand() == Command::TrackMatte,
		u"ae: controller matte and parent commands"_q);
	while (controller.undo()) {
	}
	context.check(
		controller.document().toJson() == before,
		u"ae: undo of matte and parent edits restores the original"_q);
}

void TestAeShapes(TestContext &context) {
	const auto document = Document::FromJson(kAeDocument);
	if (!document.valid()) {
		return;
	}
	const auto styled = FindByName(document, u"Styled"_q);
	const auto art = FindByName(document, u"Art"_q);
	const auto tri = FindByName(document, u"Tri"_q);
	const auto trim = FindByName(document, u"Trim"_q);
	const auto gfill = FindByName(document, u"GFill"_q);
	const auto gstroke = FindByName(document, u"GStroke"_q);
	const auto round = FindByName(document, u"Round"_q);
	const auto stops = PropertyRef{ gfill, "g.k" };
	const auto numbers = [](const Document &edited, const PropertyRef &ref) {
		const auto value = edited.valueAt(ref, 0.);
		return value ? value->numbers : std::vector<double>();
	};

	// Gradient stops.
	const auto added = AddGradientStop(document, gfill, 0.25);
	context.check(
		added
			&& added.document.gradientAt(gfill, 0.)->colors.size() == 4
			&& added.document.gradientAt(gfill, 0.)->alphas.size() == 2
			&& Near(added.document.gradientAt(gfill, 0.)->colors[1].offset, 0.25)
			&& SameColor(
				added.document.gradientAt(gfill, 0.)->colors[1].color,
				QColor(128, 128, 0))
			&& added.document.json(gfill).get("g").get("p").toInt() == 4
			&& numbers(added.document, stops).size() == 20,
		u"ae: add a gradient color stop"_q);
	if (added) {
		const auto removed = RemoveGradientStop(added.document, gfill, 1);
		context.check(
			removed && SameJson(removed.document, document),
			u"ae: removing the stop restores the gradient"_q);
	}
	const auto alpha = AddGradientStop(document, gstroke, 0.5, true);
	if (alpha) {
		const auto first = alpha.document.gradientAt(gstroke, 0.);
		const auto last = alpha.document.gradientAt(gstroke, 40.);
		const auto fewer = RemoveGradientStop(alpha.document, gstroke, 1, true);
		const auto none = fewer
			? RemoveGradientStop(fewer.document, gstroke, 0, true)
			: Edit();
		context.check(
			first->alphas.size() == 3
				&& last->alphas.size() == 3
				&& first->colors.size() == 2
				&& Near(first->alphas[1].offset, 0.5)
				&& Near(first->alphas[1].color.alphaF(), 1., 0.01)
				&& alpha.document.json(gstroke).get("g").get("p").toInt() == 2
				&& fewer
				&& fewer.document.gradientAt(gstroke, 40.)->alphas.size() == 2
				&& none
				&& SameJson(none.document, document),
			u"ae: opacity stops are added to every keyframe"_q);
	} else {
		context.check(false, u"ae: add an opacity stop"_q);
	}
	context.check(
		!RemoveGradientStop(document, gfill, 7).ok()
			&& !RemoveGradientStop(document, gstroke, 0, true).ok()
			&& !AddGradientStop(document, trim, 0.5).ok(),
		u"ae: gradient stop edits check their input"_q);
	{
		auto data = *document.gradientAt(gstroke, 40.);
		data.colors.push_back({ 0.5, QColor(255, 255, 255) });
		const auto more = SetGradient(document, gstroke, data, 40.);
		context.check(more.ok(), u"ae: set a gradient with more stops"_q);
		if (more) {
			const auto first = more.document.gradientAt(gstroke, 0.);
			const auto last = more.document.gradientAt(gstroke, 40.);
			context.check(
				first->colors.size() == 3
					&& last->colors.size() == 3
					&& Near(last->colors[1].offset, 0.5)
					&& SameColor(last->colors[1].color, QColor(255, 255, 255))
					&& SameColor(first->colors[0].color, QColor(255, 0, 0))
					&& SameColor(first->colors[1].color, QColor(128, 0, 128))
					&& SameColor(first->colors[2].color, QColor(0, 0, 255))
					&& more.document.json(gstroke).get("g").get("p").toInt() == 3
					&& more.document.keyframeTimes({ gstroke, "g.k" }).size() == 2,
				u"ae: other keyframes are rebuilt with the new stops"_q);
		}
		auto same = *document.gradientAt(gstroke, 20.);
		same.colors[0].color = QColor(0, 0, 0);
		const auto keyed = SetGradient(document, gstroke, same, 20.);
		context.check(
			keyed
				&& keyed.document.keyframeTimes({ gstroke, "g.k" })
					== std::vector<double>{ 0., 20., 40. }
				&& SameColor(
					keyed.document.gradientAt(gstroke, 20.)->colors[0].color,
					QColor(0, 0, 0))
				&& SameColor(
					keyed.document.gradientAt(gstroke, 0.)->colors[0].color,
					QColor(255, 0, 0)),
			u"ae: set gradient adds a keyframe"_q);
		auto fewer = *document.gradientAt(gfill, 0.);
		fewer.colors.erase(begin(fewer.colors) + 1);
		fewer.alphas.clear();
		const auto less = SetGradient(document, gfill, fewer, 0.);
		context.check(
			less
				&& numbers(less.document, stops)
					== std::vector<double>{ 0., 1., 0., 0., 1., 0., 0., 1. }
				&& less.document.json(gfill).get("g").get("p").toInt() == 2
				&& !SetGradient(document, gfill, GradientData(), 0.).ok(),
			u"ae: set a static gradient with fewer stops"_q);
	}

	// Plain options.
	const auto linear = SetGradientType(document, gfill, GradientType::Linear);
	const auto together = SetTrimMode(document, trim, TrimMode::Simultaneously);
	const auto cap = SetLineCap(document, gstroke, LineCap::Round);
	const auto join = SetLineJoin(document, gstroke, LineJoin::Miter);
	const auto miter = SetMiterLimit(document, gstroke, 3.);
	const auto rule = SetFillRule(document, gfill, FillRule::NonZero);
	context.check(
		linear
			&& linear.document.node(gfill)->gradientType == GradientType::Linear
			&& linear.document.json(gfill).get("t").toInt() == 1
			&& together
			&& together.document.node(trim)->trimMode
				== TrimMode::Simultaneously
			&& cap
			&& cap.document.node(gstroke)->lineCap == LineCap::Round
			&& join
			&& join.document.node(gstroke)->lineJoin == LineJoin::Miter
			&& miter
			&& Near(miter.document.node(gstroke)->miterLimit, 3.)
			&& rule
			&& rule.document.node(gfill)->fillRule == FillRule::NonZero
			&& !SetGradientType(document, trim, GradientType::Radial).ok()
			&& !SetTrimMode(document, gfill, TrimMode::Individually).ok()
			&& !SetLineCap(document, gfill, LineCap::Round).ok()
			&& !SetFillRule(document, gstroke, FillRule::EvenOdd).ok()
			&& SetLineCap(document, gstroke, LineCap::Butt).document.sameAs(
				document),
		u"ae: plain shape options"_q);

	// Fill <-> gradient.
	const auto flat = ConvertPaint(document, gfill, ShapeType::Fill);
	context.check(
		flat
			&& flat.document.node(gfill)
			&& flat.document.node(gfill)->shapeType == ShapeType::Fill
			&& flat.document.node(gfill)->fillRule == FillRule::EvenOdd
			&& SameColor(
				flat.document.valueAt({ gfill, "c" }, 0.)->color(),
				QColor(255, 0, 0))
			&& !flat.document.json(gfill).has("g")
			&& flat.document.json(gfill).indexOf("ty") == 0,
		u"ae: gradient fill to fill keeps the node"_q);
	if (flat) {
		const auto again = ConvertPaint(
			flat.document,
			gfill,
			ShapeType::GradientFill);
		const auto data = again
			? again.document.gradientAt(gfill, 0.)
			: std::optional<GradientData>();
		context.check(
			data
				&& data->colors.size() == 2
				&& SameColor(data->colors[0].color, QColor(255, 0, 0))
				&& again.document.valueAt({ gfill, "s" }, 0.)->point().x()
					< again.document.valueAt({ gfill, "e" }, 0.)->point().x()
				&& again.document.node(gfill)->gradientType
					== GradientType::Linear,
			u"ae: fill to gradient fill"_q);
	}
	const auto solid = ConvertPaint(document, gstroke, ShapeType::Stroke);
	context.check(
		solid
			&& solid.document.node(gstroke)->shapeType == ShapeType::Stroke
			&& solid.document.node(gstroke)->dashValues == 3
			&& solid.document.node(gstroke)->lineJoin == LineJoin::Bevel
			&& Near(solid.document.valueAt({ gstroke, "w" }, 0.)->scalar(), 6.)
			&& !DashesOf(solid.document, gstroke).empty()
			&& !ConvertPaint(document, gstroke, ShapeType::Fill).ok()
			&& !ConvertPaint(document, trim, ShapeType::Fill).ok(),
		u"ae: gradient stroke to stroke keeps width and dashes"_q);

	// Dashes.
	const auto longer = SetDashCount(document, gstroke, 2);
	if (longer) {
		const auto info = DashesOf(longer.document, gstroke);
		const auto back = SetDashCount(longer.document, gstroke, 1);
		context.check(
			info.dashes.size() == 2
				&& info.gaps.size() == 2
				&& info.offset == PropertyRef{ gstroke, "d.4.v" }
				&& Near(longer.document.valueAt(info.dashes[1], 0.)->scalar(), 10.)
				&& longer.document.animated(info.offset)
				&& back
				&& SameJson(back.document, document),
			u"ae: dash pairs are added and removed"_q);
	} else {
		context.check(false, u"ae: set dash count"_q);
	}
	const auto pattern = SetDashes(document, gstroke, { 8., 4., 2. }, 3.);
	if (pattern) {
		const auto info = DashesOf(pattern.document, gstroke);
		auto values = std::vector<double>();
		for (const auto &item : pattern.document.json(gstroke).get("d").items()) {
			values.push_back(item.get("v").get("k").toDouble(-1.));
		}
		context.check(
			info.dashes.size() == 2
				&& info.gaps.size() == 2
				&& values == std::vector<double>{ 8., 4., 2., 2., 3. }
				&& pattern.document.json(gstroke).get("d").at(4).get("n")
					.toString() == u"o"_q,
			u"ae: set a dash pattern"_q);
	} else {
		context.check(false, u"ae: set dashes"_q);
	}
	const auto tiny = SetDashes(document, gstroke, { 0.01, 0.01 });
	const auto solidLine = SetDashes(document, gstroke, {});
	const auto removedCount = SetDashCount(document, gstroke, 0);
	context.check(
		tiny
			&& Near(
				tiny.document.valueAt({ gstroke, "d.1.v" }, 0.)->scalar(),
				kMinDashPeriod,
				1e-6)
			&& solidLine
			&& !solidLine.document.json(gstroke).has("d")
			&& DashesOf(solidLine.document, gstroke).empty()
			&& removedCount
			&& removedCount.document.node(gstroke)->dashValues == 0
			&& !SetDashes(document, gfill, { 1., 1. }).ok(),
		u"ae: dash limits and removal"_q);
	{
		// The offset is not a length: it may be negative and overshoot.
		const auto length = PropertyRef{ gstroke, "d.0.v" };
		const auto offset = PropertyRef{ gstroke, "d.2.v" };
		const auto wild = Easing{ QPointF(0.3, -0.5), QPointF(0.6, 1.8) };
		const auto moved = SetValueAt(
			document,
			offset,
			PropValue::Scalar(-20.),
			0.);
		const auto keyed = AddKeyframe(
			document,
			offset,
			20.,
			PropValue::Scalar(-25.));
		const auto changed = SetKeyframeValue(
			document,
			{ offset, 40. },
			PropValue::Scalar(-40.));
		const auto eased = SetKeyframeEasing(
			document,
			{ KeyframeRef{ offset, 0. } },
			wild);
		const auto still = SetDashes(document, gstroke, { 8., 4. }, 3.);
		const auto back = SetValueAt(
			still.document,
			offset,
			PropValue::Scalar(-7.),
			0.);
		const auto shorter = SetValueAt(
			document,
			length,
			PropValue::Scalar(-4.),
			0.);
		const auto clean = [](const Edit &edit) {
			return edit.ok()
				&& edit.document.toRenderJson() == edit.document.toJson();
		};
		context.check(
			clean(moved)
				&& Near(moved.document.valueAt(offset, 0.)->scalar(), -20.)
				&& clean(keyed)
				&& Near(keyed.document.valueAt(offset, 20.)->scalar(), -25.)
				&& clean(changed)
				&& Near(changed.document.valueAt(offset, 40.)->scalar(), -40.)
				&& EasingRange(document, offset).overshoot()
				&& !EasingRange(document, length).overshoot()
				&& clean(eased)
				&& eased.document.keyframes(offset)[0].easing == wild
				&& clean(back)
				&& Near(back.document.valueAt(offset, 0.)->scalar(), -7.)
				&& clean(shorter)
				&& Near(shorter.document.valueAt(length, 0.)->scalar(), 0.),
			u"ae: a dash offset may be negative, a dash may not"_q);
	}

	// New items.
	for (const auto type : {
			ShapeTemplate::GradientStroke,
			ShapeTemplate::Repeater,
			ShapeTemplate::RoundCorners }) {
		const auto edit = AddShape(document, art, type, u"New"_q);
		const auto node = (edit && !edit.created.empty())
			? edit.document.node(edit.created.front())
			: nullptr;
		const auto expected = (type == ShapeTemplate::GradientStroke)
			? ShapeType::GradientStroke
			: (type == ShapeTemplate::Repeater)
			? ShapeType::Repeater
			: ShapeType::RoundCorners;
		const auto &items = edit.document.json(art).get("it");
		context.check(
			node
				&& node->shapeType == expected
				&& node->index == items.size() - 2
				&& items.at(items.size() - 1).get("ty").toString() == u"tr"_q
				&& edit.document.json(node->id).indexOf("ty") == 0
				&& !edit.document.properties(node->id).empty(),
			u"ae: add shape template %1"_q.arg(int(type)));
	}
	const auto blob = AddPath(
		document,
		styled,
		EllipsePath(QRectF(-5., -5., 10., 10.)),
		u"Blob"_q);
	context.check(
		blob
			&& blob.created.size() == 1
			&& blob.document.node(blob.created[0])->shapeType == ShapeType::Path
			&& blob.document.node(blob.created[0])->index == 0
			&& blob.document.node(blob.created[0])->parent == styled
			&& blob.document.valueAt({ blob.created[0], "ks" }, 0.)->path
			&& blob.document.valueAt({ blob.created[0], "ks" }, 0.)->path
				->vertices.size() == 4
			&& !AddPath(document, gfill, EllipsePath(QRectF(0, 0, 1, 1)), {}).ok()
			&& !AddPath(document, styled, PathData(), {}).ok(),
		u"ae: add a path item"_q);

	// Round corners.
	const auto square = RectanglePath(QRectF(-30., -30., 60., 60.));
	const auto rounded = RoundedPath(square, 10.);
	context.check(
		square.vertices.size() == 4
			&& square.closed
			&& rounded.vertices.size() == 8
			&& rounded.closed
			&& Near(rounded.vertices[0], QPointF(-30., -20.))
			&& Near(rounded.vertices[1], QPointF(-20., -30.))
			&& Near(rounded.outTangents[0], QPointF(0., -5.519))
			&& Near(rounded.inTangents[1], QPointF(-5.519, 0.))
			&& Near(rounded.inTangents[0], QPointF())
			&& RoundedPath(square, 100.).vertices.size() == 8
			&& Near(RoundedPath(square, 100.).vertices[0], QPointF(-30., 0.))
			&& RoundedPath(EllipsePath(QRectF(0., 0., 10., 10.)), 5.)
				.vertices.size() == 4
			&& RectanglePath(QRectF(0., 0., 40., 20.), 5.).vertices.size() == 8,
		u"ae: rounded path geometry"_q);
	const auto baked = BakeRoundCorners(document, round);
	context.check(
		baked
			&& !baked.document.contains(round)
			&& baked.removed == std::vector<NodeId>{ round }
			&& baked.document.valueAt({ tri, "ks" }, 0.)->path->vertices.size()
				== 6
			&& HasIssue(
				Validate(document, { .measureSize = false }),
				IssueType::UnsupportedShapes)
			&& !HasIssue(
				Validate(baked.document, { .measureSize = false }),
				IssueType::UnsupportedShapes)
			&& !BakeRoundCorners(document, trim).ok(),
		u"ae: round corners are baked into the paths"_q);
	if (baked) {
		const auto before = document.outlineAt(tri, 0.).boundingRect();
		const auto after = baked.document.outlineAt(tri, 0.).boundingRect();
		context.check(
			after.width() < before.width()
				&& after.width() > before.width() - 20.
				&& before.contains(after),
			u"ae: baked corners stay inside the original shape"_q);
	}

	auto controller = EditorController(document);
	const auto original = controller.document().toJson();
	controller.addGradientStop(gfill, 0.75);
	controller.setGradientType(gfill, GradientType::Linear);
	controller.setDashes(gstroke, { 4., 4. }, 1., "dash");
	controller.setDashes(gstroke, { 6., 4. }, 1., "dash");
	controller.setDashCount(gstroke, 3);
	controller.setLineCap(gstroke, LineCap::Square);
	controller.convertPaint(gstroke, ShapeType::Stroke);
	controller.bakeRoundCorners(round);
	controller.setTrimMode(trim, TrimMode::Simultaneously);
	context.check(
		controller.document().node(gstroke)->shapeType == ShapeType::Stroke
			&& !controller.document().contains(round)
			&& controller.undoCommand() == Command::ShapeOption,
		u"ae: controller shape commands"_q);
	while (controller.undo()) {
	}
	context.check(
		controller.document().toJson() == original,
		u"ae: undo of shape edits restores the original"_q);
}

void TestAePathsAndEasing(TestContext &context) {
	// Path helpers.
	const auto ellipse = EllipsePath(QRectF(-40., -20., 80., 40.));
	const auto bounds = PainterPath(ellipse).boundingRect();
	context.check(
		ellipse.closed
			&& PathSegmentCount(ellipse) == 4
			&& Near(bounds.left(), -40., 0.2)
			&& Near(bounds.right(), 40., 0.2)
			&& Near(bounds.top(), -20., 0.2)
			&& Near(bounds.bottom(), 20., 0.2)
			&& Near(PathPointAt(ellipse, 0, 0.), QPointF(0., -20.))
			&& Near(PathPointAt(ellipse, 3, 1.), QPointF(0., -20.))
			&& PathSegmentCount(PathData()) == 0,
		u"ae: ellipse path"_q);
	{
		// Splitting a curve keeps its shape.
		const auto split = WithInsertedVertex(ellipse, 1, 0.3);
		auto maxError = 0.;
		for (auto i = 0; i <= 20; ++i) {
			const auto t = i / 20.;
			const auto expected = PathPointAt(ellipse, 1, t);
			const auto actual = (t <= 0.3)
				? PathPointAt(split, 1, t / 0.3)
				: PathPointAt(split, 2, (t - 0.3) / 0.7);
			maxError = std::max(maxError, Distance(expected, actual));
		}
		context.check(
			split.vertices.size() == 5
				&& PathSegmentCount(split) == 5
				&& maxError < 1e-6
				&& !IsCornerVertex(split, 2)
				&& WithInsertedVertex(ellipse, 9, 0.5).vertices.size() == 4,
			u"ae: inserting a vertex keeps the curve, error "_q
				+ QString::number(maxError));
		const auto merged = WithoutVertex(split, 2);
		context.check(
			merged.vertices == ellipse.vertices,
			u"ae: removing a vertex"_q);
	}
	{
		const auto square = RectanglePath(QRectF(0., 0., 10., 10.));
		const auto line = WithInsertedVertex(square, 0, 0.25);
		const auto hit = NearestPathPoint(square, QPointF(7., -3.));
		const auto aside = NearestPathPoint(square, QPointF(14., 6.));
		const auto smooth = WithSmoothVertex(square, 1);
		const auto corner = WithCornerVertex(smooth, 1);
		const auto reversed = ReversedPath(square);
		const auto landed = WithInsertedVertex(square, hit.segment, hit.t);
		context.check(
			Near(line.vertices[1], PathPointAt(square, 0, 0.25))
				&& Near(line.vertices[1].y(), 0.)
				&& line.vertices[1].x() > 1.
				&& line.vertices[1].x() < 3.
				&& IsCornerVertex(line, 1)
				&& hit.segment == 0
				&& Near(landed.vertices[1], hit.point, 1e-6)
				&& Near(hit.point, QPointF(7., 0.), 0.05)
				&& Near(hit.distance, 3., 0.05)
				&& aside.segment == 1
				&& Near(aside.point, QPointF(10., 6.), 0.05)
				&& NearestPathPoint(PathData(), QPointF()).segment == -1
				&& !IsCornerVertex(smooth, 1)
				&& IsCornerVertex(smooth, 0)
				&& smooth.outTangents[1].y() > 0.
				&& smooth.inTangents[1].x() < 0.
				&& corner == square
				&& reversed.vertices == std::vector<QPointF>{
					QPointF(0., 0.),
					QPointF(0., 10.),
					QPointF(10., 10.),
					QPointF(10., 0.),
				}
				&& ReversedPath(reversed) == square,
			u"ae: path helpers"_q);
	}

	// Easing handles.
	const auto document = Document::FromJson(kAeDocument);
	if (!document.valid()) {
		return;
	}
	const auto parent = FindByName(document, u"Parent"_q);
	const auto trim = FindByName(document, u"Trim"_q);
	const auto position = PropertyRef{ parent, "ks.p" };
	const auto first = KeyframeRef{ position, 0. };
	const auto last = KeyframeRef{ position, 40. };
	const auto firstHandles = HandlesOf(document, first);
	const auto lastHandles = HandlesOf(document, last);
	context.check(
		firstHandles
			&& !firstHandles->hasIn
			&& firstHandles->hasOut
			&& !firstHandles->holdOut
			&& firstHandles->out == QPointF(0., 0.)
			&& Near(firstHandles->nextTime, 40.)
			&& lastHandles
			&& lastHandles->hasIn
			&& !lastHandles->hasOut
			&& lastHandles->in == QPointF(1., 1.)
			&& Near(lastHandles->previousTime, 0.)
			&& !HandlesOf(document, { position, 7. })
			&& !HandlesOf(document, { { parent, "ks.o" }, 0. }),
		u"ae: keyframe handles"_q);
	const auto eased = SetKeyframeHandles(
		document,
		last,
		QPointF(0.2, 1.5),
		QPointF(0.5, 0.5));
	context.check(eased.ok(), u"ae: set the in handle"_q);
	if (eased) {
		const auto keyframes = eased.document.keyframes(position);
		const auto value = eased.document.valueAt(position, 20.)->point();
		const auto expected = 20. + 40. * Easing{
			QPointF(0., 0.),
			QPointF(0.2, 1.5),
		}.apply(0.5);
		const auto both = SetKeyframeHandles(
			eased.document,
			first,
			QPointF(0.3, 0.3),
			QPointF(1.7, 0.25));
		context.check(
			keyframes.size() == 2
				&& keyframes[0].easing.in == QPointF(0.2, 1.5)
				&& keyframes[0].easing.out == QPointF(0., 0.)
				&& Near(value.x(), expected, 0.01)
				&& value.x() > 50.
				&& both
				&& both.document.keyframes(position)[0].easing.out
					== QPointF(1., 0.25)
				&& both.document.keyframes(position)[0].easing.in
					== QPointF(0.2, 1.5)
				&& !HasBrokenKeyframes(eased.document.propertyJson(position))
				&& SetKeyframeHandles(
					document,
					last,
					std::nullopt,
					QPointF(0.5, 0.5)).document.sameAs(document)
				&& !SetKeyframeHandles(
					document,
					{ position, 7. },
					QPointF(),
					QPointF()).ok(),
			u"ae: handles change the easing of the right segment"_q);
	}
	const auto held = SetKeyframeEasing(document, { first }, Easing::Hold());
	if (held) {
		const auto handles = HandlesOf(held.document, first);
		const auto after = HandlesOf(held.document, last);
		const auto released = SetKeyframeHandles(
			held.document,
			first,
			std::nullopt,
			QPointF(0.4, 0.));
		context.check(
			handles
				&& handles->holdOut
				&& after
				&& after->holdIn
				&& released
				&& !released.document.keyframes(position)[0].easing.hold
				&& released.document.keyframes(position)[0].easing
					== Easing{ QPointF(0.4, 0.), QPointF(1., 1.) },
			u"ae: a handle turns a hold segment into a curve"_q);
	} else {
		context.check(false, u"ae: hold easing"_q);
	}
	{
		const auto end = PropertyRef{ trim, "e" };
		const auto mixed = SetKeyframeEasings(document, {
			{ first, Easing::EaseIn() },
			{ { end, 0. }, Easing::Hold() },
			{ { end, 40. }, Easing::EaseOut() }, // The last one: ignored.
		});
		context.check(
			mixed
				&& mixed.document.keyframes(position)[0].easing
					== Easing::EaseIn()
				&& mixed.document.keyframes(end)[0].easing.hold
				&& Near(mixed.document.valueAt(end, 39.)->scalar(), 0.)
				&& Near(mixed.document.valueAt(end, 40.)->scalar(), 100.)
				&& !HasBrokenKeyframes(mixed.document.propertyJson(end)),
			u"ae: different easings in one edit"_q);
	}
	{
		const auto frames = std::vector<double>{ -5., 0., 10., 20., 40., 55. };
		const auto values = document.valuesAt(position, frames);
		auto same = (values.size() == frames.size());
		for (auto i = size_t(0); same && i != frames.size(); ++i) {
			same = (values[i] == *document.valueAt(position, frames[i]));
		}
		const auto fixed = document.valuesAt({ parent, "ks.r" }, frames);
		context.check(
			same
				&& fixed.size() == frames.size()
				&& Near(fixed[3].scalar(), 90.)
				&& document.valuesAt({ parent, "nope" }, frames).empty(),
			u"ae: values at several frames"_q);
	}
	{
		auto controller = EditorController(document);
		const auto original = controller.document().toJson();
		controller.setKeyframeHandles(last, QPointF(0.3, 1.), {}, "handle");
		controller.setKeyframeHandles(last, QPointF(0.4, 1.), {}, "handle");
		controller.setKeyframeEasings({ { first, Easing::EaseInOut() } });
		context.check(
			controller.undoCommand() == Command::SetEasing,
			u"ae: controller easing commands"_q);
		controller.undo();
		controller.undo();
		context.check(
			controller.document().toJson() == original && !controller.canUndo(),
			u"ae: dragging a handle is one undo step"_q);
	}
	{
		const auto one = AddMask(
			document,
			parent,
			RectanglePath(QRectF(0., 0., 10., 10.)));
		const auto two = one
			? SetMaskInverted(one.document, one.created.front(), true)
			: Edit();
		const auto three = two
			? DeleteNodes(two.document, { one.created.front() })
			: Edit();
		const auto both = Combined(one, two);
		const auto all = Combined(both, three);
		context.check(
			both
				&& both.created == one.created
				&& both.structural
				&& both.document.sameAs(two.document)
				&& all
				&& all.created.empty()
				&& all.removed.empty()
				&& !Combined(one, Edit()).ok(),
			u"ae: combined edits"_q);
	}
	{
		// Only a fix that changes the picture is left: nothing to do for
		// "fix everything".
		auto blank = AddLayer(
			Document::Blank(),
			LayerTemplate::Shape,
			u"Layer"_q,
			0,
			0,
			ShapeTemplate::Rectangle);
		const auto layer = blank.created.empty()
			? NodeId(0)
			: blank.created.front();
		blank = AddMask(
			blank.document,
			layer,
			RectanglePath(QRectF(-50., -50., 100., 100.)));
		auto mutation = Mutation(blank.document);
		mutation.setNode(
			layer,
			blank.document.json(layer).without("hasMask"));
		const auto off = mutation.finish().document;
		const auto result = Validate(off, { .measureSize = false });
		context.check(
			result.ok()
				&& HasIssue(result, IssueType::MasksOff)
				&& HasIssue(result, IssueType::Masks)
				&& !result.hasFixable()
				&& AutoFix(off).document.sameAs(off)
				&& AutoFix(off, { IssueType::MasksOff }).document.node(layer)
					->masksEnabled,
			u"ae: fix everything leaves picture changing fixes alone"_q);
	}
}

void TestAeValidator(TestContext &context) {
	const auto document = Document::FromJson(kAeDocument);
	if (!document.valid()) {
		return;
	}
	const auto options = ValidateOptions{ .measureSize = false };
	const auto masked = FindByName(document, u"Masked"_q);
	const auto maskA = FindByName(document, u"Mask A"_q);
	const auto maskB = FindByName(document, u"Mask B"_q);
	const auto source = FindByName(document, u"MatteSrc"_q);
	const auto luma = FindByName(document, u"Luma"_q);
	const auto styled = FindByName(document, u"Styled"_q);
	const auto gfill = FindByName(document, u"GFill"_q);
	const auto validation = Validate(document, options);
	const auto issue = [&](IssueType type) {
		const auto found = validation.find(type);
		return found ? *found : Issue();
	};
	context.check(
		HasIssue(validation, IssueType::Masks)
			&& issue(IssueType::Masks).category == IssueCategory::Forbidden
			&& issue(IssueType::Masks).rendered
			&& !issue(IssueType::Masks).fixable
			&& issue(IssueType::Masks).nodes == std::vector<NodeId>{ masked }
			&& HasIssue(validation, IssueType::Repeaters)
			&& issue(IssueType::Repeaters).category == IssueCategory::Forbidden
			&& issue(IssueType::Repeaters).rendered
			&& HasIssue(validation, IssueType::GradientStrokes)
			&& issue(IssueType::GradientStrokes).rendered
			&& HasIssue(validation, IssueType::UnsupportedShapes)
			&& issue(IssueType::UnsupportedShapes).category
				== IssueCategory::NotRendered
			&& !issue(IssueType::UnsupportedShapes).rendered,
		u"ae: forbidden features are told from not rendered ones"_q);
	context.check(
		HasIssue(validation, IssueType::TrackMattes)
			&& issue(IssueType::TrackMattes).category
				== IssueCategory::Forbidden
			&& issue(IssueType::TrackMattes).rendered
			&& !issue(IssueType::TrackMattes).fixable
			&& issue(IssueType::TrackMattes).severity == IssueSeverity::Warning
			&& issue(IssueType::TrackMattes).nodes == std::vector<NodeId>{ luma }
			&& !HasIssue(validation, IssueType::BrokenMattes)
			&& !HasIssue(validation, IssueType::MatteLinks)
			&& HasIssue(validation, IssueType::MaskOptions)
			&& issue(IssueType::MaskOptions).nodes
				== std::vector<NodeId>{ maskB }
			&& issue(IssueType::MaskOptions).fixable
			&& !issue(IssueType::MaskOptions).fixChangesPicture
			&& !issue(IssueType::MaskOptions).rendered
			&& HasIssue(validation, IssueType::MaskInverted)
			&& issue(IssueType::MaskInverted).nodes
				== std::vector<NodeId>{ maskB }
			&& issue(IssueType::MaskInverted).fixable
			&& issue(IssueType::MaskInverted).fixChangesPicture
			&& !HasIssue(validation, IssueType::MasksOff)
			&& !HasIssue(validation, IssueType::MaskModes)
			&& !HasIssue(validation, IssueType::PathVertices)
			&& !HasIssue(validation, IssueType::KeyOrder)
			&& !HasIssue(validation, IssueType::ParentLinks)
			&& validation.of(IssueCategory::Advice).empty()
			&& ranges::contains(
				validation.of(IssueCategory::Forbidden),
				IssueType::TrackMattes,
				&Issue::type),
		u"ae: matte and mask verdicts of a clean file"_q);
	context.check(
		CategoryOf(IssueType::CanvasSize) == IssueCategory::File
			&& CategoryOf(IssueType::Expressions) == IssueCategory::Forbidden
			&& !RenderedByTelegram(IssueType::Expressions)
			&& !RenderedByTelegram(IssueType::Effects)
			&& !RenderedByTelegram(IssueType::MergePaths)
			&& RenderedByTelegram(IssueType::Solids)
			&& RenderedByTelegram(IssueType::StarShapes)
			&& CategoryOf(IssueType::TrackMattes) == IssueCategory::Forbidden
			&& RenderedByTelegram(IssueType::TrackMattes)
			&& CategoryOf(IssueType::OutOfCanvas) == IssueCategory::Advice
			&& FixChangesPicture(IssueType::KeyOrder)
			&& !FixChangesPicture(IssueType::Solids),
		u"ae: issue categories"_q);

	// "Fix everything" keeps the picture: feather goes, masks stay.
	const auto fixed = AutoFix(document);
	if (fixed) {
		const auto after = Validate(fixed.document, options);
		context.check(
			!HasIssue(after, IssueType::MaskOptions)
				&& !fixed.document.json(maskB).has("f")
				&& Near(fixed.document.valueAt({ maskB, "x" }, 0.)->scalar(), 0.)
				&& !fixed.document.animated({ maskB, "o" })
				&& Near(
					fixed.document.valueAt({ maskB, "o" }, 0.)->scalar(),
					100.)
				&& HasIssue(after, IssueType::MaskInverted)
				&& fixed.document.node(maskB)->maskInverted
				&& fixed.document.node(maskB)->maskMode == MaskMode::Subtract
				&& HasIssue(after, IssueType::Masks)
				&& HasIssue(after, IssueType::TrackMattes)
				&& fixed.document.json(maskA).get("zz").toInt() == 123
				&& fixed.document.root().get("customRoot")
					== document.root().get("customRoot"),
			u"ae: auto fix resets ignored mask options, keeps the rest"_q);
	} else {
		context.check(false, u"ae: auto fix"_q);
	}
	{
		// The second mask: inverted subtract is intersect.
		const auto asked = AutoFix(document, { IssueType::MaskInverted });
		const auto direct = InvertMask(document, maskB);
		context.check(
			asked
				&& asked.document.node(maskB)->maskMode == MaskMode::Intersect
				&& !asked.document.node(maskB)->maskInverted
				&& !HasIssue(
					Validate(asked.document, options),
					IssueType::MaskInverted)
				&& direct
				&& SameJson(direct.document, asked.document)
				&& InvertMask(document, maskA).document.node(maskA)->maskMode
					== MaskMode::Subtract
				&& !InvertMask(document, masked).ok(),
			u"ae: inverted masks get a mode that is drawn"_q);
		context.check(
			InvertedMaskMode(MaskMode::Add, true) == MaskMode::Subtract
				&& InvertedMaskMode(MaskMode::Subtract, true) == MaskMode::Add
				&& InvertedMaskMode(MaskMode::Intersect, true)
					== MaskMode::Subtract
				&& InvertedMaskMode(MaskMode::Difference, true)
					== MaskMode::Subtract
				&& !InvertedMaskMode(MaskMode::Add, false)
				&& InvertedMaskMode(MaskMode::Subtract, false)
					== MaskMode::Intersect
				&& InvertedMaskMode(MaskMode::Intersect, false)
					== MaskMode::Subtract
				&& !InvertedMaskMode(MaskMode::Difference, false)
				&& !InvertedMaskMode(MaskMode::None, true)
				&& !InvertedMaskMode(MaskMode::Lighten, true),
			u"ae: inverted mask modes"_q);
	}

	const auto &layers = document.root().get("layers");
	const auto withLayer = [&](int index, Value layer) {
		return Document(document.root().with(
			"layers",
			layers.withItem(index, std::move(layer))));
	};
	{
		const auto off = withLayer(0, layers.at(0).without("hasMask"));
		const auto result = Validate(off, options);
		const auto found = result.find(IssueType::MasksOff);
		const auto silent = AutoFix(off);
		const auto asked = AutoFix(off, { IssueType::MasksOff });
		context.check(
			found
				&& found->nodes == std::vector<NodeId>{ masked }
				&& found->fixable
				&& found->fixChangesPicture
				&& found->category == IssueCategory::NotRendered
				&& (!silent.ok()
					|| HasIssue(
						Validate(silent.document, options),
						IssueType::MasksOff))
				&& asked
				&& asked.document.node(masked)->masksEnabled
				&& !HasIssue(
					Validate(asked.document, options),
					IssueType::MasksOff),
			u"ae: masks without hasMask are reported, fixed on request"_q);
	}
	{
		const auto masks = layers.at(0).get("masksProperties");
		const auto lighten = withLayer(0, layers.at(0).with(
			"masksProperties",
			masks
				.withItem(0, masks.at(0).with("mode", Value::FromString(u"l"_q)))
				.withItem(1, masks.at(1).without("mode"))));
		const auto result = Validate(lighten, options);
		const auto found = result.find(IssueType::MaskModes);
		const auto asked = AutoFix(lighten, { IssueType::MaskModes });
		context.check(
			found
				&& found->nodes == std::vector<NodeId>{ maskA, maskB }
				&& found->fixChangesPicture
				&& asked
				&& asked.document.node(maskA)->maskMode == MaskMode::Add
				&& asked.document.json(maskB).get("mode").toString() == u"a"_q
				&& !HasIssue(
					Validate(asked.document, options),
					IssueType::MaskModes),
			u"ae: mask modes the renderer skips"_q);
		const auto none = withLayer(0, layers.at(0).with(
			"masksProperties",
			Value::FromArray({
				masks.at(0).with("mode", Value::FromString(u"n"_q)),
			})));
		const auto hidden = FindIssue(none, IssueType::MaskModes);
		const auto shown = AutoFix(none, { IssueType::MaskModes });
		context.check(
			hidden
				&& hidden->nodes == std::vector<NodeId>{ maskA }
				&& shown
				&& !shown.document.node(masked)->masksEnabled
				&& shown.document.node(maskA)->maskMode == MaskMode::None
				&& !HasIssue(
					Validate(shown.document, options),
					IssueType::MaskModes),
			u"ae: a layer with only \"none\" masks is not drawn"_q);
	}
	{
		// The matted layer first / two matted layers in a row.
		const auto first = Document(document.root().with(
			"layers",
			layers.withoutItem(1).withoutItem(0)));
		const auto chain = withLayer(1, layers.at(1).with("tt", Number(1)));
		const auto firstIssue = FindIssue(first, IssueType::BrokenMattes);
		const auto chainIssue = FindIssue(chain, IssueType::BrokenMattes);
		context.check(
			firstIssue
				&& firstIssue->nodes == std::vector<NodeId>{ luma }
				&& !firstIssue->fixable
				&& !first.node(luma)->matteLayer
				&& chainIssue
				&& chainIssue->nodes == std::vector<NodeId>{ luma }
				&& chain.node(source)->matteLayer == masked
				&& !chain.node(luma)->matteLayer,
			u"ae: matted layers the renderer drops"_q);
		const auto linked = withLayer(2, layers.at(2).with("tp", Number(5)));
		const auto link = FindIssue(linked, IssueType::MatteLinks);
		context.check(
			link
				&& link->nodes == std::vector<NodeId>{ luma }
				&& link->category == IssueCategory::NotRendered
				&& !HasIssue(Validate(linked, options), IssueType::BrokenMattes),
			u"ae: a matte link to another layer"_q);
	}
	{
		// Keys before "ty" and a layer without "ddd".
		const auto fill = document.json(gfill);
		const auto moved = fill.without("ty").with("ty", fill.get("ty"));
		auto mutation = Mutation(document);
		mutation.setNode(gfill, moved);
		const auto shuffled = mutation.finish().document;
		const auto found = FindIssue(shuffled, IssueType::KeyOrder);
		const auto silent = AutoFix(shuffled);
		const auto asked = AutoFix(shuffled, { IssueType::KeyOrder });
		context.check(
			found
				&& found->nodes == std::vector<NodeId>{ gfill }
				&& found->fixable
				&& found->fixChangesPicture
				&& (!silent.ok()
					|| HasIssue(
						Validate(silent.document, options),
						IssueType::KeyOrder))
				&& asked
				&& asked.document.json(gfill).indexOf("ty") == 0
				&& asked.document.node(gfill)
				&& asked.document.json(gfill).without("ty")
					== fill.without("ty"),
			u"ae: shape keys before \"ty\""_q);
		const auto harmless = fill
			.without("ty")
			.without("nm")
			.withInserted("ty", fill.get("ty"), 0)
			.withInserted("nm", fill.get("nm"), 0);
		auto second = Mutation(document);
		second.setNode(gfill, harmless);
		context.check(
			!HasIssue(
				Validate(second.finish().document, options),
				IssueType::KeyOrder),
			u"ae: a name before \"ty\" is harmless"_q);
		const auto turned = layers.at(3)
			.without("ddd")
			.with("ks", layers.at(3).get("ks").with(
				"r",
				StaticProperty(Number(30))));
		const auto flat = withLayer(3, turned);
		const auto layerIssue = FindIssue(flat, IssueType::KeyOrder);
		const auto layerFixed = AutoFix(flat, { IssueType::KeyOrder });
		context.check(
			layerIssue
				&& layerIssue->nodes == std::vector<NodeId>{ styled }
				&& layerFixed
				&& layerFixed.document.json(styled).indexOf("ddd") == 0
				&& layerFixed.document.json(styled).get("ddd").toInt(1) == 0
				&& !HasIssue(
					Validate(layerFixed.document, options),
					IssueType::KeyOrder),
			u"ae: a turned layer without \"ddd\""_q);
	}
	{
		// A keyframe with another number of vertices.
		const auto path = PropertyRef{ maskB, "pt" };
		auto shorter = *document.valueAt(path, 40.)->path;
		shorter = WithoutVertex(shorter, 3);
		const auto edited = SetKeyframeValue(
			document,
			{ path, 40. },
			PropValue::Path(shorter));
		const auto found = edited
			? FindIssue(edited.document, IssueType::PathVertices)
			: std::nullopt;
		context.check(
			edited.ok()
				&& found
				&& found->nodes == std::vector<NodeId>{ maskB }
				&& !found->fixable,
			u"ae: path keyframes with different vertices"_q);
	}
}

// What rlottie really draws: every claim of the header about rendering is
// checked here on dot compositions, pixel by pixel.
void TestAeRendering(TestContext &context) {
	const auto size = QSize(100, 100);
	const auto render = [&](const QByteArray &json, int frame = 0) {
		return Oblivion::Lottie::RenderFrame(json, frame, size);
	};
	const auto red = QByteArray(OBLIVION_AE_GROUP(60, 60, "1,0,0"));
	const auto base = AeComposition(AeLayer(1, "Red", 50, 50, red));
	const auto probe = render(base);
	if (probe.isNull()) {
		context.log.push_back(
			u"lottie_doc: renderer unavailable, rendering checks skipped"_q);
		return;
	}
	const auto alphaAt = [](const QImage &image, int x, int y) {
		return image.isNull() ? -1 : image.pixelColor(x, y).alpha();
	};
	const auto colorAt = [](const QImage &image, int x, int y) {
		return image.isNull() ? QColor() : image.pixelColor(x, y);
	};
	const auto solid = [&](const QImage &image, int x, int y) {
		return alphaAt(image, x, y) > 240;
	};
	const auto empty = [&](const QImage &image, int x, int y) {
		return alphaAt(image, x, y) >= 0 && alphaAt(image, x, y) < 16;
	};
	const auto isRed = [&](const QImage &image, int x, int y) {
		const auto color = colorAt(image, x, y);
		return solid(image, x, y)
			&& color.red() > 200
			&& color.green() < 60
			&& color.blue() < 60;
	};
	context.check(
		isRed(probe, 50, 50)
			&& isRed(probe, 22, 22)
			&& empty(probe, 15, 50)
			&& empty(probe, 85, 50),
		u"render: a 60x60 square in the middle"_q);

	const auto document = Document::FromJson(base);
	const auto layer = FindByName(document, u"Red"_q);
	const auto left = RectanglePath(QRectF(-30., -30., 30., 60.));
	const auto masked = [&](MaskMode mode, bool inverted = false) {
		auto edit = AddMask(document, layer, left, mode);
		if (edit && inverted) {
			edit = SetMaskInverted(edit.document, edit.created.front(), true);
		}
		return edit.document;
	};
	const auto leftOnly = [&](const QImage &image) {
		return isRed(image, 35, 50) && empty(image, 65, 50);
	};
	const auto rightOnly = [&](const QImage &image) {
		return empty(image, 35, 50) && isRed(image, 65, 50);
	};
	const auto both = [&](const QImage &image) {
		return isRed(image, 35, 50) && isRed(image, 65, 50);
	};
	const auto nothing = [&](const QImage &image) {
		return empty(image, 35, 50) && empty(image, 65, 50);
	};

	// Masks.
	const auto added = masked(MaskMode::Add);
	const auto addedImage = render(added.toJson());
	context.check(leftOnly(addedImage), u"render: mask add"_q);
	context.check(
		rightOnly(render(masked(MaskMode::Subtract).toJson())),
		u"render: mask subtract"_q);
	context.check(
		leftOnly(render(masked(MaskMode::Intersect).toJson())),
		u"render: mask intersect"_q);
	context.check(
		leftOnly(render(masked(MaskMode::Difference).toJson())),
		u"render: mask difference"_q);
	{
		// "inv" is not rendered; InvertMask() gives what it means.
		const auto flagged = masked(MaskMode::Add, true);
		const auto inverted = InvertMask(
			flagged,
			FindByName(flagged, u"Mask 1"_q));
		const auto fixed = AutoFix(flagged, { IssueType::MaskInverted });
		context.check(
			render(flagged.toJson()) == addedImage
				&& render(masked(MaskMode::Subtract, true).toJson())
					== render(masked(MaskMode::Subtract).toJson())
				&& HasIssue(
					Validate(flagged, { .measureSize = false }),
					IssueType::MaskInverted)
				&& inverted
				&& rightOnly(render(inverted.document.toJson()))
				&& fixed
				&& rightOnly(render(fixed.document.toJson()))
				&& leftOnly(render(AutoFix(flagged).document.toJson())),
			u"render: the inverted flag is ignored, the mode is not"_q);
	}
	context.check(
		nothing(render(masked(MaskMode::None).toJson()))
			&& nothing(render(masked(MaskMode::Lighten).toJson()))
			&& nothing(render(masked(MaskMode::Darken).toJson())),
		u"render: a layer with only none / lighten / darken masks is "
		"not drawn"_q);
	const auto mask = FindByName(added, u"Mask 1"_q);
	{
		// Two masks: the second one is combined with the first.
		const auto top = RectanglePath(QRectF(-30., -30., 60., 30.));
		const auto second = [&](MaskMode mode) {
			return render(AddMask(added, layer, top, mode).document.toJson());
		};
		const auto subtract = second(MaskMode::Subtract);
		const auto intersect = second(MaskMode::Intersect);
		const auto lighten = second(MaskMode::Lighten);
		context.check(
			empty(subtract, 35, 35)
				&& isRed(subtract, 35, 65)
				&& empty(subtract, 65, 65)
				&& isRed(intersect, 35, 35)
				&& empty(intersect, 35, 65)
				&& empty(intersect, 65, 35)
				&& lighten == addedImage,
			u"render: masks are combined in order, lighten is skipped"_q);
	}
	{
		const auto off = Document(added.root().with(
			"layers",
			added.root().get("layers").withItem(
				0,
				added.json(layer).without("hasMask"))));
		const auto fixed = AutoFix(off, { IssueType::MasksOff });
		context.check(
			both(render(off.toJson()))
				&& fixed
				&& leftOnly(render(fixed.document.toJson())),
			u"render: masks need hasMask"_q);
	}
	{
		const auto half = SetValueAt(
			added,
			{ mask, "o" },
			PropValue::Scalar(50.),
			0.);
		const auto none = SetValueAt(
			added,
			{ mask, "o" },
			PropValue::Scalar(0.),
			0.);
		context.check(
			half
				&& render(half.document.toJson()) == addedImage
				&& none
				&& render(none.document.toJson()) == addedImage
				&& HasIssue(
					Validate(half.document, { .measureSize = false }),
					IssueType::MaskOptions),
			u"render: mask opacity is ignored"_q);
		const auto feathered = SetValueAt(
			SetValueAt(
				added,
				{ mask, "x" },
				PropValue::Scalar(20.),
				0.).document,
			{ mask, "f" },
			PropValue::Point(QPointF(20., 20.)),
			0.);
		const auto cleaned = AutoFix(
			feathered.document,
			{ IssueType::MaskOptions });
		context.check(
			feathered
				&& HasIssue(
					Validate(feathered.document, { .measureSize = false }),
					IssueType::MaskOptions)
				&& render(feathered.document.toJson()) == addedImage
				&& cleaned
				&& render(cleaned.document.toJson()) == addedImage,
			u"render: mask feather and expansion are ignored"_q);
	}
	{
		// An animated mask path: the right edge moves from 0 to 30.
		const auto full = RectanglePath(QRectF(-30., -30., 60., 60.));
		auto edit = AddKeyframe(added, { mask, "pt" }, 0.);
		if (edit) {
			edit = AddKeyframe(
				edit.document,
				{ mask, "pt" },
				40.,
				PropValue::Path(full));
		}
		const auto json = edit.document.toJson();
		context.check(
			edit.ok()
				&& leftOnly(render(json, 0))
				&& isRed(render(json, 20), 60, 50)
				&& empty(render(json, 20), 70, 50)
				&& both(render(json, 40)),
			u"render: animated mask path"_q);
	}

	// Track mattes: a 30x60 matte over the left half.
	const auto white = QByteArray(OBLIVION_AE_GROUP(30, 60, "1,1,1"));
	const auto black = QByteArray(OBLIVION_AE_GROUP(30, 60, "0,0,0"));
	const auto pair = [&](const QByteArray &matte) {
		return Document::FromJson(AeComposition(
			AeLayer(1, "Matte", 35, 50, matte)
				+ QByteArray(",")
				+ AeLayer(2, "Red", 50, 50, red)));
	};
	{
		const auto plain = pair(white);
		const auto target = FindByName(plain, u"Red"_q);
		const auto matte = [&](const Document &from, MatteMode mode) {
			return render(SetTrackMatte(
				from,
				FindByName(from, u"Red"_q),
				mode).document.toJson());
		};
		context.check(
			leftOnly(matte(plain, MatteMode::Alpha)),
			u"render: alpha matte"_q);
		context.check(
			rightOnly(matte(plain, MatteMode::AlphaInverted)),
			u"render: inverted alpha matte"_q);
		context.check(
			leftOnly(matte(plain, MatteMode::Luma))
				&& nothing(matte(pair(black), MatteMode::Luma)),
			u"render: luma matte"_q);
		context.check(
			rightOnly(matte(plain, MatteMode::LumaInverted))
				&& both(matte(pair(black), MatteMode::LumaInverted)),
			u"render: inverted luma matte"_q);
		// "td" is not needed, the layer above is taken as it is.
		const auto raw = Document(plain.root().with(
			"layers",
			plain.root().get("layers").withItem(
				1,
				plain.json(target).with("tt", Number(1)))));
		context.check(
			leftOnly(render(raw.toJson())),
			u"render: the matte is the layer above, with or without td"_q);
		// The matte layer picked from below is moved above.
		const auto below = Document::FromJson(AeComposition(
			AeLayer(2, "Red", 50, 50, red)
				+ QByteArray(",")
				+ AeLayer(1, "Matte", 35, 50, white)));
		const auto picked = SetTrackMatte(
			below,
			FindByName(below, u"Red"_q),
			MatteMode::Alpha,
			FindByName(below, u"Matte"_q));
		context.check(
			picked && leftOnly(render(picked.document.toJson())),
			u"render: a matte layer picked from below"_q);
		// A matted layer without a layer above is not drawn.
		const auto alone = Document::FromJson(AeComposition(
			AeLayer(2, "Red", 50, 50, red, "\"tt\":1,")));
		context.check(
			nothing(render(alone.toJson()))
				&& HasIssue(
					Validate(alone, { .measureSize = false }),
					IssueType::BrokenMattes),
			u"render: a matted layer without a matte is not drawn"_q);
		// Two matted layers in a row: the lower one is dropped.
		const auto green = QByteArray(OBLIVION_AE_GROUP(60, 60, "0,1,0"));
		const auto chain = Document::FromJson(AeComposition(
			AeLayer(1, "Matte", 35, 50, white)
				+ QByteArray(",")
				+ AeLayer(2, "Red", 50, 50, red, "\"tt\":1,")
				+ QByteArray(",")
				+ AeLayer(3, "Green", 50, 50, green, "\"tt\":1,")));
		const auto issue = FindIssue(chain, IssueType::BrokenMattes);
		context.check(
			leftOnly(render(chain.toJson()))
				&& issue
				&& issue->nodes == std::vector<NodeId>{
					FindByName(chain, u"Green"_q),
				},
			u"render: the second matted layer in a row is not drawn"_q);
	}

	// Trim paths, dashes and the gradient stroke on a horizontal line.
	const auto line = QByteArray("{\"ty\":\"gr\",\"it\":[{\"ty\":\"sh\","
		"\"d\":1,\"ks\":{\"a\":0,\"k\":{\"i\":[[0,0],[0,0]],"
		"\"o\":[[0,0],[0,0]],\"v\":[[-40,0],[40,0]],\"c\":false}},"
		"\"nm\":\"Line\"},{\"ty\":\"st\",\"c\":{\"a\":0,\"k\":[1,0,0,1]},"
		"\"o\":{\"a\":0,\"k\":100},\"w\":{\"a\":0,\"k\":10},\"lc\":1,"
		"\"lj\":1,\"ml\":4,\"nm\":\"Stroke\"}," OBLIVION_AE_TR
		"],\"nm\":\"Group\"}");
	const auto lineDocument = Document::FromJson(
		AeComposition(AeLayer(1, "Line", 50, 50, line)));
	const auto group = FindByName(lineDocument, u"Group"_q);
	const auto stroke = FindByName(lineDocument, u"Stroke"_q);
	context.check(
		isRed(render(lineDocument.toJson()), 15, 50)
			&& isRed(render(lineDocument.toJson()), 85, 50)
			&& empty(render(lineDocument.toJson()), 50, 60),
		u"render: a stroked line"_q);
	{
		const auto trimmed = AddShape(
			lineDocument,
			group,
			ShapeTemplate::TrimPaths,
			u"Trim"_q);
		const auto trim = trimmed.created.empty()
			? NodeId(0)
			: trimmed.created.front();
		const auto half = SetValueAt(
			trimmed.document,
			{ trim, "e" },
			PropValue::Scalar(50.),
			0.);
		const auto shifted = SetValueAt(
			half.document,
			{ trim, "o" },
			PropValue::Scalar(180.),
			0.);
		context.check(
			half
				&& isRed(render(half.document.toJson()), 30, 50)
				&& empty(render(half.document.toJson()), 70, 50)
				&& shifted
				&& empty(render(shifted.document.toJson()), 30, 50)
				&& isRed(render(shifted.document.toJson()), 70, 50),
			u"render: trim paths end and offset"_q);
	}
	{
		const auto dashed = SetDashes(lineDocument, stroke, { 10., 10. });
		const auto image = render(dashed.document.toJson());
		const auto moved = SetDashes(lineDocument, stroke, { 10., 10. }, 10.);
		const auto movedImage = render(moved.document.toJson());
		context.check(
			dashed
				&& isRed(image, 15, 50)
				&& empty(image, 25, 50)
				&& isRed(image, 35, 50)
				&& empty(image, 45, 50)
				&& moved
				&& empty(movedImage, 15, 50)
				&& isRed(movedImage, 25, 50),
			u"render: stroke dashes and their offset"_q);
	}
	{
		const auto converted = ConvertPaint(
			lineDocument,
			stroke,
			ShapeType::GradientStroke);
		auto data = GradientData();
		data.colors = {
			{ 0., QColor(255, 0, 0) },
			{ 1., QColor(0, 0, 255) },
		};
		const auto colored = SetGradient(
			converted.document,
			stroke,
			data,
			0.);
		const auto image = render(colored.document.toJson());
		const auto start = colorAt(image, 14, 50);
		const auto end = colorAt(image, 86, 50);
		context.check(
			colored
				&& solid(image, 14, 50)
				&& solid(image, 86, 50)
				&& start.red() > 180
				&& start.blue() < 80
				&& end.blue() > 180
				&& end.red() < 80,
			u"render: gradient stroke"_q);
	}

	// Gradient fill: red to blue from left to right, with opacity stops.
	{
		const auto fill = FindByName(document, u"Fill"_q);
		const auto converted = ConvertPaint(
			document,
			fill,
			ShapeType::GradientFill);
		auto data = GradientData();
		data.colors = {
			{ 0., QColor(255, 0, 0) },
			{ 1., QColor(0, 0, 255) },
		};
		const auto colored = SetGradient(converted.document, fill, data, 0.);
		const auto image = render(colored.document.toJson());
		const auto start = colorAt(image, 23, 50);
		const auto middle = colorAt(image, 50, 50);
		const auto end = colorAt(image, 77, 50);
		context.check(
			colored
				&& solid(image, 23, 50)
				&& start.red() > 200
				&& start.blue() < 60
				&& end.blue() > 200
				&& end.red() < 60
				&& std::abs(middle.red() - middle.blue()) < 40,
			u"render: linear gradient fill"_q);
		auto faded = data;
		auto opaque = QColor(0, 0, 0);
		auto clear = QColor(0, 0, 0);
		clear.setAlphaF(0.f);
		faded.alphas = { { 0., opaque }, { 1., clear } };
		const auto transparent = SetGradient(
			colored.document,
			fill,
			faded,
			0.);
		const auto fadedImage = render(transparent.document.toJson());
		context.check(
			transparent
				&& alphaAt(fadedImage, 23, 50) > 200
				&& alphaAt(fadedImage, 77, 50) < 60
				&& alphaAt(fadedImage, 50, 50) > 90
				&& alphaAt(fadedImage, 50, 50) < 165,
			u"render: gradient opacity stops"_q);
		const auto radial = SetGradientType(
			SetValueAt(
				SetValueAt(
					colored.document,
					{ fill, "s" },
					PropValue::Point(QPointF(0., 0.)),
					0.).document,
				{ fill, "e" },
				PropValue::Point(QPointF(30., 0.)),
				0.).document,
			fill,
			GradientType::Radial);
		const auto radialImage = render(radial.document.toJson());
		context.check(
			radial
				&& colorAt(radialImage, 50, 50).red() > 200
				&& colorAt(radialImage, 50, 78).blue() > 180
				&& colorAt(radialImage, 78, 50).blue() > 180
				&& colorAt(radialImage, 22, 50).blue() > 180,
			u"render: radial gradient fill"_q);
	}

	// Repeater: a 10x10 square repeated three times with a step of 25.
	{
		const auto dot = QByteArray(OBLIVION_AE_GROUP(10, 10, "1,0,0"));
		const auto dots = Document::FromJson(
			AeComposition(AeLayer(1, "Dot", 20, 50, dot)));
		const auto inner = FindByName(dots, u"Group"_q);
		const auto repeated = AddShape(
			dots,
			inner,
			ShapeTemplate::Repeater,
			u"Rep"_q);
		const auto repeater = repeated.created.empty()
			? NodeId(0)
			: repeated.created.front();
		const auto spaced = SetValueAt(
			repeated.document,
			{ repeater, "tr.p" },
			PropValue::Point(QPointF(25., 0.)),
			0.);
		const auto image = render(spaced.document.toJson());
		const auto four = SetValueAt(
			spaced.document,
			{ repeater, "c" },
			PropValue::Scalar(4.),
			0.);
		const auto faded = SetValueAt(
			spaced.document,
			{ repeater, "tr.eo" },
			PropValue::Scalar(0.),
			0.);
		const auto fadedImage = render(faded.document.toJson());
		context.check(
			spaced
				&& isRed(image, 20, 50)
				&& isRed(image, 45, 50)
				&& isRed(image, 70, 50)
				&& empty(image, 32, 50)
				&& empty(image, 95, 50)
				&& four
				&& isRed(render(four.document.toJson()), 95, 50)
				&& faded
				&& solid(fadedImage, 20, 50)
				&& alphaAt(fadedImage, 45, 50) < 200
				&& alphaAt(fadedImage, 70, 50) < alphaAt(fadedImage, 45, 50),
			u"render: repeater copies, step and end opacity"_q);
	}

	// Rounded corners.
	{
		const auto rect = FindByName(document, u"Rect"_q);
		const auto round = SetValueAt(
			document,
			{ rect, "r" },
			PropValue::Scalar(20.),
			0.);
		const auto image = render(round.document.toJson());
		context.check(
			round && empty(image, 22, 22) && isRed(image, 50, 22),
			u"render: rectangle roundness"_q);
		const auto path = QByteArray("{\"ty\":\"gr\",\"it\":[{\"ty\":\"sh\","
			"\"d\":1,\"ks\":{\"a\":0,\"k\":" OBLIVION_AE_SQUARE(30) "},"
			"\"nm\":\"Square\"},{\"ty\":\"rd\",\"r\":{\"a\":0,\"k\":20},"
			"\"nm\":\"Round\"},{\"ty\":\"fl\",\"c\":{\"a\":0,\"k\":[1,0,0,1]},"
			"\"o\":{\"a\":0,\"k\":100},\"r\":1,\"nm\":\"Fill\"},"
			OBLIVION_AE_TR "],\"nm\":\"Group\"}");
		const auto modifier = Document::FromJson(
			AeComposition(AeLayer(1, "Square", 50, 50, path)));
		const auto baked = BakeRoundCorners(
			modifier,
			FindByName(modifier, u"Round"_q));
		const auto bakedImage = render(baked.document.toJson());
		context.check(
			isRed(render(modifier.toJson()), 22, 22)
				&& isRed(render(modifier.toJson()), 78, 78)
				&& baked
				&& empty(bakedImage, 22, 22)
				&& empty(bakedImage, 78, 78)
				&& isRed(bakedImage, 50, 22)
				&& isRed(bakedImage, 50, 50),
			u"render: the round corners modifier is ignored until baked"_q);
	}

	// Parenting.
	{
		const auto dot = QByteArray(OBLIVION_AE_GROUP(10, 10, "1,0,0"));
		const auto family = Document::FromJson(AeComposition(
			AeLayer(1, "Child", 10, 10, dot)
				+ QByteArray(",")
				+ AeLayer(2, "Parent", 50, 50, QByteArray())));
		const auto child = FindByName(family, u"Child"_q);
		const auto parent = FindByName(family, u"Parent"_q);
		const auto jumped = SetLayerParent(family, child, parent);
		const auto kept = SetLayerParent(family, child, parent, 0.);
		const auto jumpedImage = render(jumped.document.toJson());
		const auto keptImage = render(kept.document.toJson());
		context.check(
			isRed(render(family.toJson()), 10, 10)
				&& jumped
				&& isRed(jumpedImage, 60, 60)
				&& empty(jumpedImage, 10, 10)
				&& kept
				&& isRed(keptImage, 10, 10)
				&& empty(keptImage, 60, 60),
			u"render: parenting"_q);
	}

	// Easing: hold and a bezier segment on the layer opacity.
	{
		const auto opacity = PropertyRef{ layer, "ks.o" };
		auto edit = AddKeyframe(
			document,
			opacity,
			0.,
			PropValue::Scalar(0.),
			Easing::Hold());
		if (edit) {
			edit = AddKeyframe(
				edit.document,
				opacity,
				30.,
				PropValue::Scalar(100.));
		}
		const auto held = edit.document.toJson();
		context.check(
			edit.ok()
				&& empty(render(held, 15), 50, 50)
				&& empty(render(held, 29), 50, 50)
				&& solid(render(held, 30), 50, 50),
			u"render: hold keyframe"_q);
		const auto eased = SetKeyframeHandles(
			edit.document,
			{ opacity, 0. },
			std::nullopt,
			QPointF(0.8, 0.));
		const auto expected = eased
			? eased.document.valueAt(opacity, 15.)->scalar()
			: -1.;
		const auto actual = alphaAt(render(eased.document.toJson(), 15), 50, 50);
		context.check(
			eased
				&& expected > 5.
				&& expected < 40.
				&& std::abs(actual - expected * 2.55) < 6.,
			u"render: bezier easing matches the model, %1 vs %2"_q
				.arg(actual)
				.arg(expected * 2.55));
	}

	// Parser quirks.
	{
		const auto shuffled = QByteArray("{\"ty\":\"gr\",\"it\":[{\"ty\":\"rc\","
			"\"d\":1,\"s\":{\"a\":0,\"k\":[60,60]},\"p\":{\"a\":0,\"k\":[0,0]},"
			"\"r\":{\"a\":0,\"k\":0},\"nm\":\"Rect\"},"
			"{\"c\":{\"a\":0,\"k\":[1,0,0,1]},\"o\":{\"a\":0,\"k\":100},"
			"\"r\":1,\"ty\":\"fl\",\"nm\":\"Fill\"}," OBLIVION_AE_TR
			"],\"nm\":\"Group\"}");
		const auto order = Document::FromJson(
			AeComposition(AeLayer(1, "Red", 50, 50, shuffled)));
		const auto image = render(order.toJson());
		const auto fixed = AutoFix(order, { IssueType::KeyOrder });
		context.check(
			HasIssue(
				Validate(order, { .measureSize = false }),
				IssueType::KeyOrder)
				&& solid(image, 50, 50)
				&& !isRed(image, 50, 50)
				&& fixed
				&& isRed(render(fixed.document.toJson()), 50, 50),
			u"render: keys before \"ty\" are lost"_q);
		const auto bar = QByteArray(OBLIVION_AE_GROUP(60, 10, "1,0,0"));
		const auto turned = QByteArray("{\"ind\":1,\"ty\":4,\"nm\":\"Bar\","
			"\"sr\":1,\"ks\":{\"o\":{\"a\":0,\"k\":100},"
			"\"r\":{\"a\":0,\"k\":90},\"p\":{\"a\":0,\"k\":[50,50,0]},"
			"\"a\":{\"a\":0,\"k\":[0,0,0]},\"s\":{\"a\":0,\"k\":[100,100,100]}},"
			"\"ao\":0,\"shapes\":[") + bar + QByteArray("],"
			OBLIVION_AE_TAIL);
		const auto flat = Document::FromJson(AeComposition(turned));
		const auto flatImage = render(flat.toJson());
		const auto upright = AutoFix(flat, { IssueType::KeyOrder });
		const auto uprightImage = render(upright.document.toJson());
		context.check(
			HasIssue(
				Validate(flat, { .measureSize = false }),
				IssueType::KeyOrder)
				&& isRed(flatImage, 75, 50)
				&& empty(flatImage, 50, 75)
				&& upright
				&& isRed(uprightImage, 50, 75)
				&& empty(uprightImage, 75, 50),
			u"render: a layer without \"ddd\" loses its rotation"_q);
	}
}

// Values that hang rlottie: never written by the operations, reported and
// clamped in files from elsewhere, clamped before anything is drawn.
void TestAeSafety(TestContext &context) {
	const auto options = ValidateOptions{ .measureSize = false };
	const auto dot = QByteArray(OBLIVION_AE_GROUP(10, 10, "1,0,0"));
	const auto layer = [&](
			const QByteArray &position,
			const QByteArray &shapes) -> QByteArray {
		return AeComposition(QByteArray("{\"ddd\":0,\"ind\":1,\"ty\":4,"
			"\"nm\":\"Layer\",\"sr\":1,\"ks\":{\"o\":{\"a\":0,\"k\":100},"
			"\"r\":{\"a\":0,\"k\":0},\"p\":") + position + QByteArray(","
			"\"a\":{\"a\":0,\"k\":[0,0,0]},"
			"\"s\":{\"a\":0,\"k\":[100,100,100]}},\"ao\":0,\"shapes\":[")
			+ shapes + QByteArray("]," OBLIVION_AE_TAIL));
	};
	const auto center = QByteArray("{\"a\":0,\"k\":[50,50,0]}");
	const auto moving = [](
			const char *out,
			const char *tangents) -> QByteArray {
		return QByteArray("{\"a\":1,\"k\":[{\"i\":{\"x\":0.5,\"y\":1},"
			"\"o\":{\"x\":0.5,\"y\":") + QByteArray(out)
			+ QByteArray("},\"t\":0,\"s\":[20,50,0]") + QByteArray(tangents)
			+ QByteArray("},{\"t\":40,\"s\":[80,50,0]}]}");
	};
	const auto tangents = ",\"to\":[10,0,0],\"ti\":[-10,0,0]";
	const auto curve = QByteArray("{\"ty\":\"el\",\"d\":1,"
		"\"s\":{\"a\":0,\"k\":[60,60]},\"p\":{\"a\":0,\"k\":[0,0]},"
		"\"nm\":\"Oval\"}");
	const auto stroked = [&](
			const QByteArray &extra,
			const QByteArray &more) -> QByteArray {
		return QByteArray("{\"ty\":\"gr\",\"it\":[") + curve
			+ QByteArray(",{\"ty\":\"st\",\"c\":{\"a\":0,\"k\":[1,0,0,1]},"
				"\"o\":{\"a\":0,\"k\":100},\"w\":{\"a\":0,\"k\":6},\"lc\":1,"
				"\"lj\":1,\"ml\":4") + extra + QByteArray(",\"nm\":\"Stroke\"}")
			+ more + QByteArray("," OBLIVION_AE_TR "],\"nm\":\"Group\"}");
	};
	const auto dash = [](const char *value, const char *gap) -> QByteArray {
		return QByteArray(",\"d\":[{\"n\":\"d\",\"nm\":\"dash\",\"v\":")
			+ QByteArray(value)
			+ QByteArray("},{\"n\":\"g\",\"nm\":\"gap\",\"v\":")
			+ QByteArray(gap)
			+ QByteArray("},{\"n\":\"o\",\"nm\":\"offset\","
				"\"v\":{\"a\":0,\"k\":0}}]");
	};
	const auto trim = [](
			const char *start,
			const char *offset = "0") -> QByteArray {
		return QByteArray(",{\"ty\":\"tm\",\"s\":") + QByteArray(start)
			+ QByteArray(",\"e\":{\"a\":0,\"k\":100},\"o\":{\"a\":0,\"k\":")
			+ QByteArray(offset)
			+ QByteArray("},\"m\":1,\"nm\":\"Trim\"}");
	};
	const auto under = "{\"a\":1,\"k\":[{\"i\":{\"x\":[0.5],\"y\":[1]},"
		"\"o\":{\"x\":[0.5],\"y\":[-0.6]},\"t\":0,\"s\":[0]},"
		"{\"t\":40,\"s\":[10]}]}";
	const auto raised = "{\"a\":1,\"k\":[{\"i\":{\"x\":[0.5],\"y\":[1]},"
		"\"o\":{\"x\":[0.5],\"y\":[-0.2]},\"t\":0,\"s\":[6]},"
		"{\"t\":40,\"s\":[10]}]}";
	const auto over = "{\"a\":1,\"k\":[{\"i\":{\"x\":[0.5],\"y\":[1.6]},"
		"\"o\":{\"x\":[0.5],\"y\":[0]},\"t\":0,\"s\":[0]},"
		"{\"t\":40,\"s\":[100]}]}";
	// Two keyframes with one name, opacity first in the file: rlottie
	// draws both with the easing of the first one.
	const auto named = [&](
			const char *opacityOut,
			const char *positionOut) -> QByteArray {
		const auto easing = [](const char *out) -> QByteArray {
			return QByteArray("\"i\":{\"x\":0.5,\"y\":1},\"o\":{\"x\":0.5,"
				"\"y\":") + QByteArray(out)
				+ QByteArray("},\"n\":\"curve\"");
		};
		return AeComposition(QByteArray("{\"ddd\":0,\"ind\":1,\"ty\":4,"
			"\"nm\":\"Layer\",\"sr\":1,\"ks\":{\"o\":{\"a\":1,\"k\":[{")
			+ easing(opacityOut)
			+ QByteArray(",\"t\":0,\"s\":[100]},{\"t\":40,\"s\":[60]}]},"
				"\"r\":{\"a\":0,\"k\":0},\"p\":{\"a\":1,\"k\":[{")
			+ easing(positionOut)
			+ QByteArray(",\"t\":0,\"s\":[20,50,0]") + QByteArray(tangents)
			+ QByteArray("},{\"t\":40,\"s\":[80,50,0]}]},"
				"\"a\":{\"a\":0,\"k\":[0,0,0]},"
				"\"s\":{\"a\":0,\"k\":[100,100,100]}},\"ao\":0,\"shapes\":[")
			+ dot + QByteArray("]," OBLIVION_AE_TAIL));
	};
	const auto pathWith = [&](
			const char *easing,
			const char *time = ",\"t\":0") -> QByteArray {
		return QByteArray("{\"a\":1,\"k\":[{") + QByteArray(easing)
			+ QByteArray(time) + QByteArray(",\"s\":[20,50,0]")
			+ QByteArray(tangents)
			+ QByteArray("},{\"t\":40,\"s\":[80,50,0]}]}");
	};
	const auto negativeDash = dash("{\"a\":0,\"k\":-5}", "{\"a\":0,\"k\":3}");

	struct Case {
		const char *name;
		QByteArray json;
		bool hangs;
	};
	const auto cases = std::vector<Case>{
		{ "keyframe names made of the handles",
			named("0.3", "0.3"), false },
		{ "a keyframe name shared with an easing below zero",
			named("-5", "0.3"), true },
		{ "a keyframe name shared by two easings below zero",
			named("-5", "-5"), true },
		{ "motion path with an easing handle a hair below zero",
			layer(moving("-0.0000001", tangents), dot), true },
		{ "motion path with an easing handle outside of the time range",
			layer(pathWith("\"i\":{\"x\":0.5,\"y\":1},"
				"\"o\":{\"x\":1.5,\"y\":0.3}"), dot), true },
		{ "motion path keyframes without a time",
			layer(pathWith("\"i\":{\"x\":0.5,\"y\":1},"
				"\"o\":{\"x\":0.5,\"y\":-0.4}", ""), dot), true },
		{ "motion path next to a stray list of keyframes",
			layer(moving("-0.4", tangents)
				+ QByteArray(",\"k\":[{\"t\":0,\"s\":[0],\"h\":1}]"), dot),
			true },
		{ "negative dash under an escaped key",
			layer(center, stroked(
				QByteArray(negativeDash).replace(
					QByteArray("\"d\":["),
					QByteArray("\"\\u0064\":[")),
				QByteArray())), true },
		{ "negative dash in a file padded with zero bytes",
			layer(center, stroked(negativeDash, QByteArray()))
				+ QByteArray(2, '\0'), true },
		{ "motion path with easing below zero",
			layer(moving("-0.4", tangents), dot), true },
		{ "motion path with usual easing",
			layer(moving("0.3", tangents), dot), false },
		{ "motion path with easing above one",
			layer(moving("1.7", tangents), dot), false },
		{ "straight motion with easing below zero",
			layer(moving("-0.4", ""), dot), false },
		{ "negative dash",
			layer(center, stroked(
				dash("{\"a\":0,\"k\":-5}", "{\"a\":0,\"k\":3}"),
				QByteArray())), true },
		{ "negative gap",
			layer(center, stroked(
				dash("{\"a\":0,\"k\":5}", "{\"a\":0,\"k\":-8}"),
				QByteArray())), true },
		{ "dash easing that goes below zero",
			layer(center, stroked(
				dash(under, "{\"a\":0,\"k\":3}"),
				QByteArray())), true },
		{ "dash easing that stays above zero",
			layer(center, stroked(
				dash(raised, "{\"a\":0,\"k\":3}"),
				QByteArray())), false },
		{ "usual dashes",
			layer(center, stroked(
				dash("{\"a\":0,\"k\":5}", "{\"a\":0,\"k\":3}"),
				QByteArray())), false },
		{ "a dash pattern that is too short",
			layer(center, stroked(
				dash("{\"a\":0,\"k\":0.1}", "{\"a\":0,\"k\":0.1}"),
				QByteArray())), true },
		{ "a dot and a wide gap",
			layer(center, stroked(
				dash("{\"a\":0,\"k\":0.01}", "{\"a\":0,\"k\":6}"),
				QByteArray())), false },
		{ "a dash with a negative offset",
			layer(center, stroked(
				QByteArray(",\"d\":[{\"n\":\"d\",\"nm\":\"dash\","
					"\"v\":{\"a\":0,\"k\":0}},{\"n\":\"o\",\"nm\":\"offset\","
					"\"v\":{\"a\":0,\"k\":-17}}]"),
				QByteArray())), false },
		{ "trim start far below zero",
			layer(center, stroked(
				QByteArray(),
				trim("{\"a\":0,\"k\":-150}"))), true },
		{ "trim start a little below zero",
			layer(center, stroked(
				QByteArray(),
				trim("{\"a\":0,\"k\":-3}"))), false },
		{ "trim easing that dips below zero",
			layer(center, stroked(QByteArray(), trim(under))), false },
		{ "trim start below zero behind a positive offset",
			layer(center, stroked(
				QByteArray(),
				trim("{\"a\":0,\"k\":-30}", "90"))), true },
		{ "trim start below zero within a positive offset",
			layer(center, stroked(
				QByteArray(),
				trim("{\"a\":0,\"k\":-3}", "90"))), false },
		{ "trim start below zero with a negative offset",
			layer(center, stroked(
				QByteArray(),
				trim("{\"a\":0,\"k\":-30}", "-300"))), true },
		{ "trim that overshoots upwards",
			layer(center, stroked(QByteArray(), trim(over))), false },
		{ "huge repeater",
			layer(center, dot + QByteArray(",{\"ty\":\"rp\","
				"\"c\":{\"a\":0,\"k\":90000000},\"o\":{\"a\":0,\"k\":0},"
				"\"m\":1,\"tr\":{\"ty\":\"tr\",\"p\":{\"a\":0,\"k\":[1,0]},"
				"\"a\":{\"a\":0,\"k\":[0,0]},\"s\":{\"a\":0,\"k\":[100,100]},"
				"\"r\":{\"a\":0,\"k\":0},\"so\":{\"a\":0,\"k\":100},"
				"\"eo\":{\"a\":0,\"k\":100}},\"nm\":\"Rep\"}")), true },
		{ "huge star",
			layer(center, QByteArray("{\"ty\":\"sr\",\"sy\":1,\"d\":1,"
				"\"pt\":{\"a\":0,\"k\":80000000},\"p\":{\"a\":0,\"k\":[0,0]},"
				"\"r\":{\"a\":0,\"k\":0},\"ir\":{\"a\":0,\"k\":10},"
				"\"is\":{\"a\":0,\"k\":0},\"or\":{\"a\":0,\"k\":20},"
				"\"os\":{\"a\":0,\"k\":0},\"nm\":\"Star\"}")), true },
	};
	auto renderAvailable = !Oblivion::Lottie::RenderFrame(
		layer(center, dot),
		0,
		QSize(100, 100)).isNull();
	for (const auto &entry : cases) {
		const auto name = QString::fromLatin1(entry.name);
		const auto document = Document::FromJson(entry.json);
		context.check(document.valid(), u"safety: parses: "_q + name);
		if (!document.valid()) {
			continue;
		}
		const auto safe = RenderSafeJson(entry.json);
		const auto validation = Validate(document, options);
		const auto issue = validation.find(IssueType::RendererHang);
		context.check(
			(safe != entry.json) == entry.hangs
				&& (issue != nullptr) == entry.hangs
				&& (document.toRenderJson() != document.toJson())
					== entry.hangs
				&& RenderSafeJson(safe) == safe,
			u"safety: "_q + name);
		if (!entry.hangs) {
			continue;
		}
		const auto fixed = AutoFix(document, { IssueType::RendererHang });
		const auto all = AutoFix(document);
		context.check(
			issue->severity == IssueSeverity::Error
				&& issue->fixable
				&& !issue->fixChangesPicture
				&& !issue->nodes.empty()
				&& !validation.ok()
				&& fixed
				&& fixed.document.toJson() == safe
				&& document.toRenderJson() == safe
				&& !HasIssue(
					Validate(fixed.document, options),
					IssueType::RendererHang)
				&& all
				&& !HasIssue(
					Validate(all.document, options),
					IssueType::RendererHang),
			u"safety: reported and fixed: "_q + name);
		if (renderAvailable) {
			// The original bytes: the toolkit clamps them on its own.
			const auto guarded = Oblivion::Lottie::RenderFrame(
				entry.json,
				10,
				QSize(100, 100));
			const auto clean = Oblivion::Lottie::RenderFrame(
				safe,
				10,
				QSize(100, 100));
			context.check(
				!guarded.isNull() && guarded == clean,
				u"safety: drawn without hanging: "_q + name);
		}
	}

	{
		// The keyframe that has to be safe loses the shared name, the
		// other one keeps its name and its easing.
		const auto first = [&](
				const QByteArray &json,
				const QByteArray &path) -> Value {
			const auto fixed = AutoFix(
				Document::FromJson(json),
				{ IssueType::RendererHang });
			return fixed.document.propertyJson({
				FindByName(fixed.document, u"Layer"_q),
				path,
			}).get("k").at(0);
		};
		const auto shared = named("-5", "0.3");
		const auto both = named("-5", "-5");
		context.check(
			first(shared, "ks.o").get("n").toString() == u"curve"_q
				&& first(shared, "ks.o").get("o").get("y").toNumber() == -5.
				&& !first(shared, "ks.p").has("n")
				&& first(shared, "ks.p").get("o").get("y").toNumber() == 0.3
				&& first(both, "ks.o").get("n").toString() == u"curve"_q
				&& first(both, "ks.o").get("o").get("y").toNumber() == -5.
				&& !first(both, "ks.p").has("n")
				&& first(both, "ks.p").get("o").get("y").toNumber() == 0.
				&& first(both, "ks.p").has("ti"),
			u"safety: a shared keyframe name is taken from the path"_q);
	}
	{
		// What can't be read here is not handed to rlottie: it draws
		// whatever it has read before an error.
		const auto hanging = layer(moving("-0.4", tangents), dot);
		const auto harmless = layer(moving("0.3", tangents), dot);
		const auto tail = QByteArray(" tail");
		auto cut = hanging;
		cut.chop(1);
		const auto twice = layer(
			moving("-0.4", tangents) + QByteArray(",\"p\":") + center,
			dot);
		const auto once = Document::FromJson(twice);
		const auto signedZero = layer(moving("-0", tangents), dot);
		const auto plainZero = Document::FromJson(signedZero);
		context.check(
			plainZero.valid()
				&& RenderSafeJson(signedZero) != signedZero
				&& RenderSafeJson(signedZero) == plainZero.toJson()
				&& plainZero.toRenderJson() == plainZero.toJson()
				&& !HasIssue(
					Validate(plainZero, options),
					IssueType::RendererHang),
			u"safety: a zero with a minus sign is read as zero"_q);
		context.check(
			RenderSafeJson(cut).isEmpty()
				&& !Document::FromJson(cut).valid()
				&& !RenderSafeJson(hanging).isEmpty()
				&& RenderSafeJson(hanging + tail) == RenderSafeJson(hanging)
				&& RenderSafeJson(harmless + tail) == harmless + tail
				&& !Document::FromJson(hanging + tail).valid(),
			u"safety: a file that can't be read is not drawn"_q);
		context.check(
			once.valid()
				&& !once.animated({ FindByName(once, u"Layer"_q), "ks.p" })
				&& !HasIssue(Validate(once, options), IssueType::RendererHang)
				&& RenderSafeJson(twice) == once.toJson()
				&& RenderSafeJson(once.toJson()) == once.toJson(),
			u"safety: a key written twice is kept once"_q);
		if (renderAvailable) {
			const auto size = QSize(100, 100);
			context.check(
				Oblivion::Lottie::RenderFrame(cut, 10, size).isNull()
					&& (Oblivion::Lottie::RenderFrame(twice, 10, size)
						== Oblivion::Lottie::RenderFrame(
							layer(center, dot),
							10,
							size)),
				u"safety: broken files are drawn without hanging"_q);
		}
	}
	{
		// Dash lists without names: the last item is the offset.
		const auto plain = [](
				std::initializer_list<const char*> values) -> QByteArray {
			auto result = QByteArray(",\"d\":[");
			for (const auto value : values) {
				if (!result.endsWith('[')) {
					result += ',';
				}
				result += QByteArray("{\"v\":{\"a\":0,\"k\":")
					+ QByteArray(value)
					+ QByteArray("}}");
			}
			return result + QByteArray("]");
		};
		const auto values = [](const Document &document, NodeId stroke) {
			auto result = std::vector<double>();
			for (const auto &item : document.json(stroke).get("d").items()) {
				result.push_back(item.get("v").get("k").toDouble(-1.));
			}
			return result;
		};
		const auto two = Document::FromJson(
			layer(center, stroked(plain({ "6", "-4" }), QByteArray())));
		const auto four = Document::FromJson(layer(
			center,
			stroked(plain({ "5", "3", "2", "7" }), QByteArray())));
		const auto strokeTwo = FindByName(two, u"Stroke"_q);
		const auto strokeFour = FindByName(four, u"Stroke"_q);
		if (strokeTwo && strokeFour) {
			const auto infoTwo = DashesOf(two, strokeTwo);
			const auto infoFour = DashesOf(four, strokeFour);
			const auto same = SetDashCount(two, strokeTwo, 1);
			const auto kept = SetDashCount(four, strokeFour, 2);
			context.check(
				infoTwo.dashes
					== std::vector<PropertyRef>{ { strokeTwo, "d.0.v" } }
					&& infoTwo.gaps.empty()
					&& infoTwo.offset == PropertyRef{ strokeTwo, "d.1.v" }
					&& two.property(infoTwo.offset)->dashOffset
					&& two.property(infoTwo.offset)->name == u"o"_q
					&& two.property(infoTwo.dashes[0])->name == u"d"_q
					&& !HasIssue(
						Validate(two, options),
						IssueType::RendererHang)
					&& same.ok()
					&& values(same.document, strokeTwo)
						== std::vector<double>{ 6., 6., -4. },
				u"safety: an unnamed dash and its offset"_q);
			context.check(
				infoFour.dashes.size() == 2
					&& infoFour.gaps
						== std::vector<PropertyRef>{ { strokeFour, "d.1.v" } }
					&& infoFour.offset == PropertyRef{ strokeFour, "d.3.v" }
					&& four.property(infoFour.gaps[0])->name == u"g"_q
					&& four.property(infoFour.offset)->dashOffset
					&& kept.ok()
					&& values(kept.document, strokeFour)
						== std::vector<double>{ 5., 3., 2., 2., 7. },
				u"safety: an unnamed dash list keeps its pattern"_q);
		} else {
			context.check(false, u"safety: unnamed dash lists parse"_q);
		}
	}

	// The operations do not write such values.
	const auto document = Document::FromJson(
		layer(moving("0.3", tangents), stroked(
			dash("{\"a\":0,\"k\":5}", "{\"a\":0,\"k\":3}"),
			trim("{\"a\":0,\"k\":0}"))));
	if (!document.valid()) {
		return;
	}
	const auto mover = FindByName(document, u"Layer"_q);
	const auto stroke = FindByName(document, u"Stroke"_q);
	const auto trimmed = FindByName(document, u"Trim"_q);
	const auto position = PropertyRef{ mover, "ks.p" };
	const auto opacity = PropertyRef{ mover, "ks.o" };
	const auto first = KeyframeRef{ position, 0. };
	const auto clean = [&](const Edit &edit) {
		return edit.ok()
			&& edit.document.toRenderJson() == edit.document.toJson();
	};
	const auto low = SetKeyframeHandles(
		document,
		first,
		std::nullopt,
		QPointF(0.5, -0.7));
	const auto preset = SetKeyframeEasing(
		document,
		{ first },
		Easing{ QPointF(0.3, -0.5), QPointF(0.6, 1.8) });
	context.check(
		!EasingRange(document, position).overshoot()
			&& EasingRange(document, opacity).overshoot()
			&& EasingRange(document, { stroke, "d.0.v" }).minY == 0.
			&& EasingRange(document, { stroke, "d.0.v" }).maxY == 1.
			&& EasingRange(document, { trimmed, "s" }).overshoot()
			&& clean(low)
			&& low.document.keyframes(position)[0].easing.out
				== QPointF(0.5, 0.)
			&& clean(preset)
			&& preset.document.keyframes(position)[0].easing
				== Easing{ QPointF(0.3, 0.), QPointF(0.6, 1.) },
		u"safety: motion path keyframes can't overshoot"_q);
	{
		auto keyed = AddKeyframe(document, opacity, 0., PropValue::Scalar(0.));
		keyed = AddKeyframe(
			keyed.document,
			opacity,
			40.,
			PropValue::Scalar(100.));
		const auto free = SetKeyframeHandles(
			keyed.document,
			{ opacity, 0. },
			std::nullopt,
			QPointF(0.5, -0.7));
		const auto wild = SetKeyframeHandles(
			keyed.document,
			{ opacity, 0. },
			std::nullopt,
			QPointF(0.5, -50.));
		context.check(
			clean(free)
				&& free.document.keyframes(opacity)[0].easing.out
					== QPointF(0.5, -0.7)
				&& free.document.valueAt(opacity, 8.)->scalar() < 0.
				&& clean(wild)
				&& wild.document.keyframes(opacity)[0].easing.out
					== QPointF(0.5, -2.),
			u"safety: other properties may overshoot, within limits"_q);
	}
	const auto straight = SetMotionPath(document, position, false);
	if (straight) {
		const auto free = SetKeyframeHandles(
			straight.document,
			first,
			std::nullopt,
			QPointF(0.5, -0.7));
		const auto back = SetMotionPath(free.document, position, true);
		context.check(
			clean(straight)
				&& !straight.document.property(position)->spatial
				&& !straight.document.propertyJson(position)
					.get("k").at(0).has("to")
				&& EasingRange(straight.document, position).overshoot()
				&& clean(free)
				&& free.document.keyframes(position)[0].easing.out
					== QPointF(0.5, -0.7)
				&& free.document.valueAt(position, 6.)->point().x() < 20.
				&& clean(back)
				&& back.document.property(position)->spatial
				&& back.document.keyframes(position)[0].easing.out
					== QPointF(0.5, 0.)
				&& SetMotionPath(document, position, true).document.sameAs(
					document)
				&& !SetMotionPath(document, opacity, true).ok(),
			u"safety: a motion path can be turned off to overshoot"_q);
	} else {
		context.check(false, u"safety: turn the motion path off"_q);
	}
	{
		const auto length = PropertyRef{ stroke, "d.0.v" };
		const auto start = PropertyRef{ trimmed, "s" };
		const auto negative = SetValueAt(
			document,
			length,
			PropValue::Scalar(-4.),
			0.);
		auto animated = AddKeyframe(document, length, 0., PropValue::Scalar(0.));
		animated = AddKeyframe(
			animated.document,
			length,
			40.,
			PropValue::Scalar(-9.),
			Easing{ QPointF(0.5, -1.), QPointF(0.5, 2.) });
		const auto handled = SetKeyframeHandles(
			animated.document,
			{ length, 0. },
			std::nullopt,
			QPointF(0.2, -1.));
		const auto beyond = SetValueAt(
			document,
			start,
			PropValue::Scalar(-30.),
			0.);
		const auto pattern = SetDashes(document, stroke, { -3., -2. }, -1.);
		context.check(
			clean(negative)
				&& Near(negative.document.valueAt(length, 0.)->scalar(), 0.)
				&& clean(animated)
				&& Near(animated.document.valueAt(length, 40.)->scalar(), 0.)
				&& clean(handled)
				&& handled.document.keyframes(length)[0].easing.out
					== QPointF(0.2, 0.)
				&& clean(beyond)
				&& Near(beyond.document.valueAt(start, 0.)->scalar(), 0.)
				&& clean(pattern),
			u"safety: dash and trim values stay in range"_q);
	}
	{
		const auto group = FindByName(document, u"Group"_q);
		const auto added = AddShape(
			document,
			group,
			ShapeTemplate::Repeater,
			u"Rep"_q);
		const auto copies = PropertyRef{ added.created.front(), "c" };
		const auto many = SetValueAt(
			added.document,
			copies,
			PropValue::Scalar(1e9),
			0.);
		context.check(
			clean(many)
				&& Near(
					many.document.valueAt(copies, 0.)->scalar(),
					kMaxRepeaterCopies),
			u"safety: repeater copies are limited"_q);
	}
}

#undef OBLIVION_AE_TAIL
#undef OBLIVION_AE_SQUARE
#undef OBLIVION_AE_LINEAR
#undef OBLIVION_AE_GROUP
#undef OBLIVION_AE_TR
#undef OBLIVION_AE_KS

[[nodiscard]] QByteArray ReadResource(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void RandomOperation(
		EditorController &controller,
		std::mt19937 &random,
		QStringList &trace) {
	const auto &document = controller.document();
	const auto &nodes = document.nodes();
	if (nodes.size() < 2) {
		return;
	}
	const auto randomInt = [&](int from, int till) {
		return (till <= from)
			? from
			: from + int(random() % uint32(till - from));
	};
	const auto randomNode = [&](Fn<bool(const NodeInfo&)> filter) {
		auto list = std::vector<NodeId>();
		for (const auto &node : nodes) {
			if (filter(node)) {
				list.push_back(node.id);
			}
		}
		return list.empty()
			? NodeId(0)
			: list[randomInt(0, int(list.size()))];
	};
	const auto anyNode = [](const NodeInfo &node) {
		return node.kind != NodeKind::Composition;
	};
	const auto layerOrShape = [](const NodeInfo &node) {
		return node.kind == NodeKind::Layer
			|| (node.kind == NodeKind::Shape
				&& node.shapeType != ShapeType::Transform);
	};
	const auto randomProperty = [&](bool animatedOnly) {
		auto list = std::vector<PropertyInfo>();
		for (auto attempt = 0; attempt != 8 && list.empty(); ++attempt) {
			const auto id = randomNode(anyNode);
			for (auto &info : document.properties(id)) {
				if ((!animatedOnly || info.animated)
					&& info.type != PropertyType::Path) {
					list.push_back(std::move(info));
				}
			}
		}
		return list.empty()
			? std::optional<PropertyInfo>()
			: std::make_optional(list[randomInt(0, int(list.size()))]);
	};
	const auto anyLayer = [](const NodeInfo &node) {
		return node.kind == NodeKind::Layer;
	};
	const auto first = controller.firstFrame();
	const auto last = controller.lastFrame();
	const auto type = randomInt(0, 27);
	trace.push_back(QString::number(type));
	switch (type) {
	case 0: controller.rename(randomNode(anyNode), u"Fuzz"_q); break;
	case 1:
		if (const auto id = randomNode(layerOrShape)) {
			controller.setHidden({ id }, !document.node(id)->hidden);
		}
		break;
	case 2:
		controller.adjustHsl(
			randomInt(-180, 180),
			randomInt(-50, 50),
			randomInt(-50, 50),
			(random() % 2) ? std::vector<NodeId>() : std::vector<NodeId>{
				randomNode(layerOrShape),
			});
		break;
	case 3: {
		const auto palette = document.palette();
		if (!palette.empty()) {
			controller.replaceColor(
				palette[randomInt(0, int(palette.size()))].color,
				QColor(randomInt(0, 256), randomInt(0, 256), randomInt(0, 256)));
		}
	} break;
	case 4:
		if (const auto info = randomProperty(false)) {
			controller.addKeyframe(info->ref, randomInt(first, last + 1));
		}
		break;
	case 5:
		if (const auto info = randomProperty(true)) {
			const auto times = document.keyframeTimes(info->ref);
			if (!times.empty()) {
				controller.moveKeyframes(
					{ { info->ref, times[randomInt(0, int(times.size()))] } },
					randomInt(-10, 11));
			}
		}
		break;
	case 6:
		if (const auto info = randomProperty(false)) {
			const auto frame = randomInt(first, last + 1);
			controller.setCurrentFrame(frame);
			auto value = document.valueAt(
				info->ref,
				controller.localFrame(info->ref.node));
			if (value && !value->numbers.empty()) {
				for (auto &number : value->numbers) {
					number = number * 0.9 + 1.;
				}
				controller.setValue(info->ref, *value);
			}
		}
		break;
	case 7:
		controller.reorderNode(
			randomNode(layerOrShape),
			(random() % 2) ? 1 : -1);
		break;
	case 8: controller.duplicateNodes({ randomNode(layerOrShape) }); break;
	case 9: controller.deleteNodes({ randomNode(anyNode) }); break;
	case 10:
		if (const auto info = randomProperty(true)) {
			const auto times = document.keyframeTimes(info->ref);
			if (!times.empty()) {
				controller.removeKeyframes({
					{ info->ref, times[randomInt(0, int(times.size()))] },
				});
			}
		}
		break;
	case 11:
		if (const auto info = randomProperty(true)) {
			const auto times = document.keyframeTimes(info->ref);
			if (!times.empty()) {
				controller.setKeyframeEasing(
					{ { info->ref, times[randomInt(0, int(times.size()))] } },
					Easing::FromPreset(EasingPreset(randomInt(0, 5))));
			}
		}
		break;
	case 12:
		if (const auto info = randomProperty(false)) {
			controller.setAnimated(info->ref, !info->animated);
		}
		break;
	case 13: {
		const auto layer = randomNode([](const NodeInfo &node) {
			return node.kind == NodeKind::Layer;
		});
		if (layer) {
			controller.shiftLayers({ layer }, randomInt(-5, 6));
		}
	} break;
	case 14: {
		const auto shape = randomNode([](const NodeInfo &node) {
			return node.kind == NodeKind::Shape
				&& node.shapeType != ShapeType::Transform;
		});
		const auto group = randomNode([](const NodeInfo &node) {
			return node.kind == NodeKind::Shape
				&& node.shapeType == ShapeType::Group;
		});
		if (shape && group) {
			controller.moveNode(shape, group, randomInt(0, 4));
		}
	} break;
	case 15:
		if (random() % 2) {
			controller.addLayer(
				LayerTemplate::Shape,
				u"Added"_q,
				ShapeTemplate(randomInt(0, 4)));
		} else if (const auto group = randomNode([](const NodeInfo &node) {
				return node.kind == NodeKind::Shape
					&& node.shapeType == ShapeType::Group;
			})) {
			controller.addShape(group, ShapeTemplate(randomInt(0, 8)), u"Added"_q);
		}
		break;
	case 16:
		if (const auto layer = randomNode(anyLayer)) {
			controller.addMask(
				layer,
				DefaultMaskPath(document, layer, controller.currentFrame()),
				MaskMode(randomInt(0, 7)));
		}
		break;
	case 17:
		if (const auto mask = randomNode([](const NodeInfo &node) {
				return node.kind == NodeKind::Mask;
			})) {
			const auto path = PropertyRef{ mask, "pt" };
			switch (randomInt(0, 6)) {
			case 0: controller.setMaskMode(mask, MaskMode(randomInt(0, 7))); break;
			case 1:
				if (random() % 2) {
					controller.setMaskInverted(
						mask,
						!document.node(mask)->maskInverted);
				} else {
					controller.invertMask(mask);
				}
				break;
			case 2: controller.insertPathVertex(path, randomInt(0, 4), 0.4); break;
			case 3: controller.removePathVertex(path, randomInt(0, 4)); break;
			case 4: controller.reorderNode(mask, (random() % 2) ? 1 : -1); break;
			case 5:
				if (random() % 2) {
					controller.setValue(
						{ mask, "x" },
						PropValue::Scalar(randomInt(0, 9)));
				} else {
					controller.setValue(
						{ mask, "f" },
						PropValue::Point(QPointF(randomInt(0, 9), 2.)));
				}
				break;
			}
		}
		break;
	case 18:
		if (const auto layer = randomNode(anyLayer)) {
			controller.setTrackMatte(
				layer,
				MatteMode(randomInt(0, 5)),
				(random() % 2) ? randomNode(anyLayer) : NodeId(0));
		}
		break;
	case 19:
		if (const auto layer = randomNode(anyLayer)) {
			controller.setLayerParent(
				layer,
				(random() % 4) ? randomNode(anyLayer) : NodeId(0),
				random() % 2);
		}
		break;
	case 20:
		if (const auto shape = randomNode([](const NodeInfo &node) {
				return node.kind == NodeKind::Shape
					&& (node.shapeType == ShapeType::GradientFill
						|| node.shapeType == ShapeType::GradientStroke);
			})) {
			switch (randomInt(0, 5)) {
			case 0:
				controller.addGradientStop(
					shape,
					randomInt(0, 101) / 100.,
					random() % 2);
				break;
			case 1:
				controller.removeGradientStop(
					shape,
					randomInt(0, 3),
					random() % 2);
				break;
			case 2:
				controller.setGradientType(
					shape,
					(random() % 2)
						? GradientType::Radial
						: GradientType::Linear);
				break;
			case 3:
				if (auto data = document.gradientAt(
						shape,
						controller.localFrame(shape))) {
					data->colors.push_back({
						randomInt(0, 101) / 100.,
						QColor(randomInt(0, 256), 0, 255),
					});
					controller.setGradient(shape, *data);
				}
				break;
			case 4:
				controller.convertPaint(
					shape,
					(document.node(shape)->shapeType
						== ShapeType::GradientFill)
						? ShapeType::Fill
						: ShapeType::Stroke);
				break;
			}
		}
		break;
	case 21:
		if (const auto shape = randomNode([](const NodeInfo &node) {
				return node.kind == NodeKind::Shape
					&& (node.shapeType == ShapeType::Stroke
						|| node.shapeType == ShapeType::Fill);
			})) {
			const auto stroke = (document.node(shape)->shapeType
				== ShapeType::Stroke);
			switch (randomInt(0, stroke ? 6 : 2)) {
			case 0:
				controller.convertPaint(shape, stroke
					? ShapeType::GradientStroke
					: ShapeType::GradientFill);
				break;
			case 1:
				if (stroke) {
					controller.setLineCap(shape, LineCap(randomInt(0, 3)));
				} else {
					controller.setFillRule(
						shape,
						(random() % 2) ? FillRule::EvenOdd : FillRule::NonZero);
				}
				break;
			case 2:
				controller.setDashes(
					shape,
					{ double(randomInt(0, 20)), randomInt(0, 30) / 3. },
					randomInt(0, 10));
				break;
			case 3: controller.setDashCount(shape, randomInt(0, 4)); break;
			case 4: controller.setLineJoin(shape, LineJoin(randomInt(0, 3))); break;
			case 5: controller.setMiterLimit(shape, randomInt(0, 10)); break;
			}
		}
		break;
	case 22:
		if (const auto shape = randomNode([](const NodeInfo &node) {
				return node.kind == NodeKind::Shape
					&& node.shapeType == ShapeType::Path;
			})) {
			const auto path = PropertyRef{ shape, "ks" };
			switch (randomInt(0, 4)) {
			case 0: controller.insertPathVertex(path, randomInt(0, 3), 0.5); break;
			case 1: controller.removePathVertex(path, randomInt(0, 3)); break;
			case 2: controller.setPathClosed(path, random() % 2); break;
			case 3: controller.reversePath(path); break;
			}
		}
		break;
	case 23:
		if (const auto info = randomProperty(true)) {
			const auto times = document.keyframeTimes(info->ref);
			if (!times.empty()) {
				controller.setKeyframeHandles(
					{ info->ref, times[randomInt(0, int(times.size()))] },
					QPointF(randomInt(0, 11) / 10., randomInt(-5, 16) / 10.),
					QPointF(randomInt(0, 11) / 10., randomInt(-5, 16) / 10.));
			}
		}
		break;
	case 24:
		if (const auto group = randomNode([](const NodeInfo &node) {
				return node.kind == NodeKind::Shape
					&& node.shapeType == ShapeType::Group;
			})) {
			controller.addShape(
				group,
				(random() % 2)
					? ShapeTemplate::RoundCorners
					: (random() % 2)
					? ShapeTemplate::GradientStroke
					: ShapeTemplate::Repeater,
				u"Added"_q);
		}
		break;
	case 25:
		if (const auto round = randomNode([](const NodeInfo &node) {
				return node.kind == NodeKind::Shape
					&& node.shapeType == ShapeType::RoundCorners;
			})) {
			controller.bakeRoundCorners(round);
		} else if (const auto layer = randomNode([](const NodeInfo &node) {
				return node.kind == NodeKind::Layer
					&& node.layerType == LayerType::Shape;
			})) {
			controller.addPath(
				layer,
				EllipsePath(QRectF(-20., -10., 40., 20.)),
				u"Added"_q);
		}
		break;
	default:
		switch (randomInt(0, 6)) {
		case 0:
			controller.setFrameRate(
				(controller.fps() > 45.) ? 30. : 60.,
				true);
			break;
		case 1: controller.changeSpeed(1.25); break;
		case 2:
			controller.setCanvasSize(
				(document.size().width() == 512)
					? QSize(256, 256)
					: QSize(512, 512),
				random() % 2);
			break;
		case 3:
			if (last - first > 4) {
				controller.trimRange(first + 1, last, random() % 2);
			}
			break;
		case 4: controller.setDuration(controller.frameCount() + 10); break;
		case 5: controller.optimize(); break;
		}
		break;
	}
}

void TestBundled(TestContext &context) {
	auto files = QStringList();
	auto iterator = QDirIterator(
		u":/animations"_q,
		{ u"*.tgs"_q },
		QDir::Files,
		QDirIterator::Subdirectories);
	while (iterator.hasNext()) {
		files.push_back(iterator.next());
	}
	files.sort();
	context.check(!files.isEmpty(), u"bundled .tgs files found"_q);

	auto renderAvailable = true;
	auto rendered = 0;
	auto usedFallback = false;
	auto issuesTotal = std::map<IssueType, int>();
	auto errorFiles = 0;
	auto fuzzOperations = 0;
	for (auto index = 0; index != int(files.size()); ++index) {
		const auto &path = files[index];
		const auto name = path.mid(path.lastIndexOf('/') + 1);
		const auto bytes = ReadResource(path);
		auto json = Oblivion::Lottie::Unpack(bytes);
		if (json.isEmpty()) {
			json = GunzipFallback(bytes);
			usedFallback = true;
		}
		auto error = QString();
		const auto document = Document::FromJson(json, &error);
		context.check(
			document.valid(),
			name + u": parse failed: "_q + error);
		if (!document.valid()) {
			continue;
		}
		const auto serialized = document.toJson();
		const auto reparsed = Json::Parse(serialized);
		const auto original = Json::Parse(json);
		context.check(
			reparsed && original && *reparsed == *original,
			name + u": serialized JSON differs from the original"_q);
		context.check(
			reparsed && Json::Serialize(*reparsed) == serialized,
			name + u": serialization is not stable"_q);
		context.check(
			document.toRenderJson() == serialized
				&& RenderSafeJson(json) == json,
			name + u": a bundled animation is reported as dangerous"_q);

		const auto frames = document.frames();
		for (const auto frame : { 0, frames / 2, std::max(frames - 1, 0) }) {
			if (!renderAvailable) {
				break;
			}
			const auto a = Oblivion::Lottie::RenderFrame(
				json,
				frame,
				QSize(160, 160));
			const auto b = Oblivion::Lottie::RenderFrame(
				serialized,
				frame,
				QSize(160, 160));
			if (a.isNull() && b.isNull()) {
				renderAvailable = false;
				break;
			}
			++rendered;
			context.check(
				!a.isNull() && !b.isNull() && a == b,
				name + u": frame "_q + QString::number(frame)
					+ u" renders differently after round trip"_q);
		}

		const auto validation = Validate(document);
		if (!validation.ok()) {
			++errorFiles;
		}
		for (const auto &issue : validation.issues) {
			++issuesTotal[issue.type];
		}

		auto random = std::mt19937(uint32(index * 7919 + 17));
		auto controller = EditorController(document);
		auto trace = QStringList();
		constexpr auto kOperations = 20;
		for (auto i = 0; i != kOperations; ++i) {
			RandomOperation(controller, random, trace);
			++fuzzOperations;
			const auto &current = controller.document();
			const auto currentJson = current.toJson();
			const auto parsedAgain = Json::Parse(currentJson);
			if (!current.valid()
				|| !parsedAgain
				|| !(*parsedAgain == current.root())) {
				context.check(
					false,
					name + u": invalid document after operations "_q
						+ trace.join(','));
				break;
			}
			if (renderAvailable && (i % 5 == 4)) {
				const auto image = Oblivion::Lottie::RenderFrame(
					currentJson,
					0,
					QSize(64, 64));
				context.check(
					!image.isNull() || current.frames() <= 0,
					name + u": edited document does not render, operations "_q
						+ trace.join(','));
			}
		}
		const auto edited = controller.document().toJson();
		while (controller.undo()) {
		}
		context.check(
			controller.document().toJson() == serialized
				&& controller.document().root().sameAs(document.root())
				&& !controller.dirty(),
			name + u": undo did not restore the original, operations "_q
				+ trace.join(','));
		while (controller.redo()) {
		}
		context.check(
			controller.document().toJson() == edited,
			name + u": redo did not re-apply, operations "_q
				+ trace.join(','));
	}
	context.log.push_back(u"lottie_doc: %1 bundled files, %2 frames compared%3%4"_q
		.arg(files.size())
		.arg(rendered)
		.arg(renderAvailable
			? QString()
			: u" (renderer unavailable, render comparison skipped)"_q)
		.arg(usedFallback
			? u", local gunzip fallback used"_q
			: QString()));
	context.log.push_back(u"lottie_doc: %1 random operations with undo / redo"_q
		.arg(fuzzOperations));
	auto summary = QStringList();
	for (const auto &[type, count] : issuesTotal) {
		summary.push_back(u"%1:%2"_q.arg(int(type)).arg(count));
	}
	context.log.push_back(u"lottie_doc: validator: %1 of %2 files have errors, issue types %3"_q
		.arg(errorFiles)
		.arg(files.size())
		.arg(summary.join(' ')));
}

} // namespace

bool RunSelfTest(QStringList &log) {
	auto context = TestContext{ log };
	const auto started = crl::now();
	TestJson(context);
	TestEasing(context);
	TestKeyframes(context);
	TestOperations(context);
	TestAeModel(context);
	TestAeMasks(context);
	TestAeMattesAndParents(context);
	TestAeShapes(context);
	TestAePathsAndEasing(context);
	TestAeValidator(context);
	TestAeRendering(context);
	TestAeSafety(context);
	TestBundled(context);
	log.push_back(u"lottie_doc: %1 checks, %2 failed, %3 ms"_q
		.arg(context.checks)
		.arg(context.failures)
		.arg(crl::now() - started));
	return !context.failures;
}

} // namespace Oblivion::LottieEdit
