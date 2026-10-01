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
#include <mutex>
#include <random>
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

	[[nodiscard]] std::optional<Value> parse(QString *error);

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

};

std::optional<Value> Parser::parse(QString *error) {
	if ((_end - _p) >= 3
		&& uchar(_p[0]) == 0xEF
		&& uchar(_p[1]) == 0xBB
		&& uchar(_p[2]) == 0xBF) {
		_p += 3;
	}
	auto result = Value();
	auto ok = parseValue(result, 0);
	if (ok) {
		skipSpace();
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
		members.push_back({ std::move(key), std::move(value) });
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
	return ranges::any_of(issues, &Issue::fixable);
}

struct Document::Data {
	Value root;
	std::vector<NodeInfo> nodes;
	std::vector<Path> paths;
	std::unordered_map<NodeId, int> byId;

	mutable std::once_flag jsonOnce;
	mutable QByteArray json;
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
				node.parentLayer = i->second;
			}
		}
		if (node.layerType == LayerType::Precomp && !node.refId.isEmpty()) {
			node.precomp = assets.value(node.refId);
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
};

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
			addList("d", PropertyRole::Dash, [](const Value &) {
				return PropertyType::Scalar;
			});
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
				addList("d", PropertyRole::Dash, [](const Value &) {
					return PropertyType::Scalar;
				});
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

[[nodiscard]] std::optional<PropertySpec> SpecForMissing(
		const NodeInfo &node,
		QByteArrayView path) {
	const auto transformKey = [&](QByteArrayView key)
	-> std::optional<PropertySpec> {
		struct Entry {
			const char *key;
			PropertyRole role;
			PropertyType type;
		};
		static constexpr auto kEntries = std::array{
			Entry{ "a", PropertyRole::Anchor, PropertyType::Vector },
			Entry{ "p", PropertyRole::Position, PropertyType::Vector },
			Entry{ "s", PropertyRole::Scale, PropertyType::Vector },
			Entry{ "r", PropertyRole::Rotation, PropertyType::Scalar },
			Entry{ "o", PropertyRole::Opacity, PropertyType::Scalar },
			Entry{ "sk", PropertyRole::Skew, PropertyType::Scalar },
			Entry{ "sa", PropertyRole::SkewAxis, PropertyType::Scalar },
		};
		for (const auto &entry : kEntries) {
			if (key == QByteArrayView(entry.key)) {
				return PropertySpec{
					path.toByteArray(),
					entry.role,
					entry.type,
				};
			}
		}
		return std::nullopt;
	};
	if (node.kind == NodeKind::Layer && path.startsWith("ks.")) {
		return transformKey(path.mid(3));
	} else if (node.kind == NodeKind::Shape
		&& node.shapeType == ShapeType::Transform) {
		return transformKey(path);
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
			spec = SpecForMissing(*node, ref.path);
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
		} else if (last) {
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
	const auto merged = MergeValue(value, DecodeValue(previous, resolved.spec.type));
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
}

void WriteKeyframes(
		Mutation &mutation,
		const Resolved &resolved,
		ModelList models) {
	mutation.set(
		resolved.path,
		WriteModels(resolved.json, std::move(models), resolved.spec.type));
	mutation.changed(resolved.node->id);
}

void SetValueAtImpl(
		Mutation &mutation,
		const Resolved &resolved,
		const PropValue &value,
		double frame) {
	if (resolved.solid || !IsKeyframed(resolved.json)) {
		WriteStatic(mutation, resolved, value);
		return;
	}
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
[[nodiscard]] Value MapStoredValues(const Value &property, Callback &&callback) {
	if (IsKeyframed(property)) {
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
	const auto resolved = Resolve(mutation.data(), ref);
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
	const auto resolved = Resolve(mutation.data(), ref);
	if (!resolved || resolved->solid) {
		return Failed(u"AddKeyframe: property not found"_q);
	}
	time = RoundTime(time);
	auto models = ReadModels(resolved->json, resolved->spec.type);
	const auto current = ValueOf(*resolved, time);
	const auto merged = value ? MergeValue(*value, current) : current;
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
	const auto merged = MergeValue(value, model.value);
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
		const Easing &easing) {
	auto mutation = Mutation(document);
	for (const auto &group : GroupKeyframes(keyframes)) {
		const auto resolved = Resolve(mutation.data(), group.property);
		if (!resolved || !IsKeyframed(resolved->json)) {
			continue;
		}
		auto models = ReadModels(resolved->json, resolved->spec.type);
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
				const auto &parent = copy.get("parent");
				if (parent.isNumber() && remap.contains(parent.toInt())) {
					copy = copy.with(
						"parent",
						Number(remap[parent.toInt()]));
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
	} else if (node->kind != NodeKind::Shape) {
		return Failed(u"MoveNode: only layers and shape items move"_q);
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
	} else if (node->kind == NodeKind::Shape) {
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
	for (const auto &member : result.members()) {
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
	if (root.get("ddd").toInt(0) == 1) {
		layers3d.push_back(rootId);
	}
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
			break;
		case NodeKind::Shape:
			switch (node.shapeType) {
			case ShapeType::MergePaths: merges.push_back(node.id); break;
			case ShapeType::Repeater: repeaters.push_back(node.id); break;
			case ShapeType::Star: stars.push_back(node.id); break;
			case ShapeType::GradientStroke:
				gradientStrokes.push_back(node.id);
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
	const auto wants = [&](IssueType type) {
		return types.empty() || ranges::contains(types, type);
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
	for (const auto bad : { "[1,]", "{\"a\"1}", "[01x]", "\"abc", "[1] 2" }) {
		context.check(
			!Json::Parse(QByteArrayView(bad)).has_value(),
			u"json must reject "_q + QString::fromLatin1(bad));
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
	const auto first = controller.firstFrame();
	const auto last = controller.lastFrame();
	const auto type = randomInt(0, 17);
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
	TestBundled(context);
	log.push_back(u"lottie_doc: %1 checks, %2 failed, %3 ms"_q
		.arg(context.checks)
		.arg(context.failures)
		.arg(crl::now() - started));
	return !context.failures;
}

} // namespace Oblivion::LottieEdit
