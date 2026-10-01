/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_online.h"

#include "api/api_updates.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/timer_rpl.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/version.h"
#include "data/data_changes.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/sender.h"
#include "oblivion/oblivion_interface.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings/settings_common.h"
#include "settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/ripple_animation.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/notifications_manager.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QTimeZone>
#include <QtGui/QPainterPath>
#include <QtGui/QWindow>

#include <array>
#include <map>
#include <mutex>

namespace Oblivion {
namespace Online {
namespace {

// Journal model begin: std and QtCore only, covered by RunSelfTest.

constexpr auto kFileMagic = uint32(0x4C4E424F);
constexpr auto kFileVersion = uchar(1);
constexpr auto kFileHeader = 5;
constexpr auto kFileFooter = 4;
constexpr auto kDaySeconds = TimeId(86400);
constexpr auto kHourSeconds = TimeId(3600);
constexpr auto kMaxTime = uint64(0x7FFFFFFF);
constexpr auto kFlagsBits = 3;
constexpr auto kTypicalMinTotal = TimeId(15 * 60);
constexpr auto kTypicalRanges = 3;

// The longest time a status stays valid without being prolonged.
constexpr auto kStatusLife = TimeId(330);

enum : uchar {
	kStartUnknown = 0x01, // Was online already when the watching began.
	kEndUnknown = 0x02, // Was still online when the watching stopped.
	kEndGuessed = 0x04, // No "offline" update came, the status expired.
	kOngoing = 0x08, // Online right now, never stored.
};
constexpr auto kStoredFlags = uchar(kStartUnknown | kEndUnknown | kEndGuessed);

struct Limits {
	TimeId retention = 60 * kDaySeconds;
	TimeId mergeGap = 20;
	TimeId sleepGap = 150;
	TimeId startupQuiet = 15;
	int perUser = 12000;
	int users = 5000;
	int total = 600000;
	int watched = 60000;
	int covered = 4000;
};

struct Interval {
	TimeId from = 0;
	TimeId till = 0;
	uchar flags = 0;

	friend inline bool operator==(
		const Interval &,
		const Interval &) = default;
};

struct Period {
	TimeId from = 0;
	TimeId till = 0;

	friend inline bool operator==(const Period &, const Period &) = default;
};

void WriteVarint(QByteArray &to, uint64 value) {
	while (value >= 0x80) {
		to.push_back(char(uchar((value & 0x7F) | 0x80)));
		value >>= 7;
	}
	to.push_back(char(uchar(value)));
}

void WriteUint32(QByteArray &to, uint32 value) {
	for (auto i = 0; i != 4; ++i) {
		to.push_back(char(uchar((value >> (i * 8)) & 0xFF)));
	}
}

[[nodiscard]] uint32 ReadUint32(const uchar *data) {
	return uint32(data[0])
		| (uint32(data[1]) << 8)
		| (uint32(data[2]) << 16)
		| (uint32(data[3]) << 24);
}

[[nodiscard]] uint32 Checksum(const uchar *data, qsizetype size) {
	auto result = uint32(2166136261U);
	for (auto i = qsizetype(0); i != size; ++i) {
		result = (result ^ data[i]) * uint32(16777619U);
	}
	return result;
}

class Reader final {
public:
	Reader(const uchar *data, qsizetype size)
	: _data(data)
	, _end(data + size) {
	}

	[[nodiscard]] uint64 varint() {
		auto result = uint64(0);
		for (auto shift = 0; shift < 64 && _data != _end; shift += 7) {
			const auto byte = *_data++;
			result |= uint64(byte & 0x7F) << shift;
			if (!(byte & 0x80)) {
				return result;
			}
		}
		_failed = true;
		return 0;
	}
	[[nodiscard]] int64 time() {
		const auto result = varint();
		if (result > kMaxTime) {
			_failed = true;
			return 0;
		}
		return int64(result);
	}
	[[nodiscard]] bool failed() const {
		return _failed;
	}
	[[nodiscard]] bool atEnd() const {
		return (_data == _end);
	}
	[[nodiscard]] qsizetype left() const {
		return _end - _data;
	}

private:
	const uchar *_data = nullptr;
	const uchar *_end = nullptr;
	bool _failed = false;

};

// All the times are unixtime seconds. A status of a user is one number:
// "online till" if it is in the future, "was online at" if it is in the
// past and zero if the exact time is hidden.
//
// An interval is opened when a user comes online and closed at the exact
// "was online at" moment, or at the expiration time of the status if no
// update arrives. A "was online at" for a user who wasn't seen online is
// kept as a zero-length interval: a visit that nobody watched.
//
// The periods while the journal was watching are kept as well, outside
// of them nothing is known (the app was closed, asleep or disconnected).
//
// When the watching stops for a short while (a lost connection, a nap)
// and starts again before a status has expired, the user is considered
// online the whole time: the visit continues, it is not a new one.
//
// The server sends the statuses only while it shows this account as
// online. Without that the journal is not observing: the statuses that
// still arrive are applied, but the time is not counted as watched. The
// users whose statuses are requested regularly have their own covered
// periods, for them the time is known in any case.
class Journal final {
public:
	explicit Journal(Limits limits = Limits())
	: _limits(limits) {
	}

	void start(TimeId now);
	void stop(TimeId now);

	// Stops at the last moment the journal was known to be alive.
	void pause();
	[[nodiscard]] bool started() const {
		return _started;
	}
	void setRecording(bool recording, TimeId now);
	[[nodiscard]] bool recording() const {
		return _recording;
	}
	void setObserving(bool observing, TimeId now);
	[[nodiscard]] bool observing() const {
		return _observing;
	}

	// The status of the user was known for the whole period.
	void cover(uint64 user, Period period);

	// Returns true if the user came online right now.
	bool apply(uint64 user, TimeId till, TimeId now);

	// Returns false after a long silence (the app was asleep): everything
	// was closed at the previous call and the watching started again.
	bool alive(TimeId now);
	[[nodiscard]] bool asleep(TimeId now) const {
		return _started
			&& ((now - _alive > _limits.sleepGap)
				|| (_alive - now > _limits.sleepGap));
	}

	// Closes the expired statuses, returns the nearest expiration or zero.
	TimeId expire(TimeId now);

	[[nodiscard]] bool online(uint64 user) const {
		return _open.find(user) != end(_open);
	}
	[[nodiscard]] std::vector<uint64> onlineUsers() const;
	[[nodiscard]] std::vector<Interval> intervals(
		uint64 user,
		TimeId now) const;
	[[nodiscard]] std::vector<Period> watched(TimeId now) const;

	// The watched periods together with the covered ones of the user.
	[[nodiscard]] std::vector<Period> known(uint64 user, TimeId now) const;
	[[nodiscard]] int total() const {
		return _total;
	}
	[[nodiscard]] int usersCount() const {
		return int(_users.size());
	}
	[[nodiscard]] bool empty() const {
		return _users.empty() && _watched.empty() && _covered.empty();
	}

	void forget(uint64 user, TimeId now);
	void clear();
	void trim(TimeId now);
	[[nodiscard]] bool takeChanged() {
		return std::exchange(_changed, false);
	}

	[[nodiscard]] QByteArray serialize(TimeId now) const;
	bool parse(const QByteArray &bytes);

private:
	struct Open {
		TimeId from = 0;
		TimeId till = 0;
		bool startUnknown = false;
	};
	struct Resume {
		uint64 user = 0;
		TimeId till = 0;
	};

	void stopAt(TimeId when);
	void close(uint64 user, const Open &open, TimeId end, uchar flags);
	void append(uint64 user, Interval interval);
	void seen(uint64 user, TimeId when, TimeId now);
	void addWatched(std::vector<Period> &to, Period period) const;
	[[nodiscard]] TimeId watchedTill(TimeId now) const {
		return asleep(now) ? _alive : std::max(now, _alive);
	}
	bool clipPeriods(std::vector<Period> &list, TimeId cutoff) const;
	void trimTo(TimeId cutoff);

	const Limits _limits;
	std::map<uint64, std::vector<Interval>> _users;
	std::map<uint64, Open> _open;
	std::vector<Period> _watched;
	std::map<uint64, std::vector<Period>> _covered;
	std::vector<Resume> _resume;

	// The visits cut while their statuses were still valid, with those
	// statuses: the same value arriving later is not a new moment.
	std::map<uint64, TimeId> _cut;
	TimeId _stoppedAt = 0;
	TimeId _watchFrom = 0;
	TimeId _alive = 0;
	int _total = 0;
	bool _started = false;
	bool _recording = true;
	bool _observing = true;
	bool _changed = false;

};

void Journal::start(TimeId now) {
	if (_started) {
		return;
	}
	_started = true;
	_watchFrom = _alive = now;

	// Those who were online when the watching stopped and whose status
	// has not expired since then: the visit that was cut continues.
	for (const auto &[user, till] : std::exchange(_resume, {})) {
		if (till <= now) {
			continue;
		}
		_cut.erase(user);
		auto open = Open{ .from = now, .till = till, .startUnknown = true };
		const auto i = _recording ? _users.find(user) : end(_users);
		if (i != end(_users) && !i->second.empty()) {
			const auto &last = i->second.back();
			if ((last.flags & kEndUnknown)
				&& (last.till == _stoppedAt)
				&& (last.till <= now)) {
				open.from = last.from;
				open.startUnknown = (last.flags & kStartUnknown) != 0;
				i->second.pop_back();
				--_total;
				_changed = true;
			}
		}
		_open.emplace(user, open);
	}
}

void Journal::stop(TimeId now) {
	if (!_started) {
		return;
	}
	alive(now);
	stopAt(std::max(now, _alive));
}

void Journal::pause() {
	if (_started) {
		stopAt(_alive);
	}
}

void Journal::stopAt(TimeId when) {
	_resume.clear();
	_stoppedAt = when;
	for (const auto &[user, open] : _open) {
		close(
			user,
			open,
			std::min(open.till, when),
			(open.till > when) ? kEndUnknown : kEndGuessed);
		if (open.till > when) {
			_resume.push_back({ user, open.till });
			_cut[user] = open.till;
		}
	}
	_open.clear();
	if (_recording) {
		if (_observing) {
			addWatched(_watched, { _watchFrom, when });
		}
		_changed = true;
	}
	_started = false;
}

void Journal::setRecording(bool recording, TimeId now) {
	if (_recording == recording) {
		return;
	}
	_cut.clear();
	if (!_started) {
		_recording = recording;
		_resume.clear();
		return;
	}
	if (!recording) {
		for (const auto &[user, open] : _open) {
			close(user, open, std::min(open.till, now), kEndUnknown);
		}
		if (_observing) {
			addWatched(_watched, { _watchFrom, now });
		}
		_recording = false;
	} else {
		_recording = true;
		_watchFrom = _alive = now;
		for (auto &[user, open] : _open) {
			open.from = now;
			open.startUnknown = true;
		}
	}
	_changed = true;
}

void Journal::setObserving(bool observing, TimeId now) {
	if (_observing == observing) {
		return;
	}
	_observing = observing;
	if (observing) {
		_watchFrom = now;
	} else if (_started && _recording) {
		addWatched(_watched, { _watchFrom, watchedTill(now) });
		_changed = true;
	}
}

void Journal::cover(uint64 user, Period period) {
	if (!_recording || period.till <= period.from) {
		return;
	}
	auto &list = _covered[user];
	const auto count = list.size();
	addWatched(list, period);
	if (list.size() != count) {
		_changed = true;
	}
}

bool Journal::apply(uint64 user, TimeId till, TimeId now) {
	if (!_started) {
		return false;
	}
	const auto i = _open.find(user);
	if (till > now) {
		_cut.erase(user);
		if (i != end(_open)) {
			i->second.till = till;
			return false;
		}
		auto open = Open{
			.from = now,
			.till = till,
			.startUnknown = !_observing
				|| (now - _watchFrom < _limits.startupQuiet),
		};
		if (_recording) {
			const auto j = _users.find(user);
			if (j != end(_users) && !j->second.empty()) {
				const auto &last = j->second.back();
				if (now - last.till <= _limits.mergeGap) {
					open.from = std::min(last.from, now);
					open.startUnknown = (last.flags & kStartUnknown) != 0;
					j->second.pop_back();
					--_total;
				}
			}
			_changed = true;
		}
		_open.emplace(user, open);
		return true;
	} else if (i != end(_open)) {
		const auto open = i->second;
		_open.erase(i);
		if (till > 0) {
			close(user, open, std::min(till, now), 0);
		} else {
			close(user, open, std::min(open.till, now), kEndGuessed);
		}
	} else if (till > 0) {
		seen(user, till, now);
	}
	return false;
}

bool Journal::alive(TimeId now) {
	if (!_started) {
		return true;
	} else if (asleep(now)) {
		stopAt(_alive);
		start(now);
		return false;
	}
	_alive = std::max(_alive, now);
	return true;
}

TimeId Journal::expire(TimeId now) {
	auto nearest = TimeId(0);
	for (auto i = begin(_open); i != end(_open);) {
		const auto till = i->second.till;
		if (till <= now) {
			close(i->first, i->second, till, kEndGuessed);
			i = _open.erase(i);
		} else {
			nearest = nearest ? std::min(nearest, till) : till;
			++i;
		}
	}
	return nearest;
}

void Journal::close(
		uint64 user,
		const Open &open,
		TimeId end,
		uchar flags) {
	if (!_recording) {
		return;
	}
	append(user, {
		.from = open.from,
		.till = std::max(end, open.from),
		.flags = uchar(flags | (open.startUnknown ? kStartUnknown : 0)),
	});
}

void Journal::append(uint64 user, Interval interval) {
	auto &list = _users[user];
	_changed = true;
	if (!list.empty()) {
		auto &last = list.back();
		interval.from = std::max(interval.from, last.till);
		interval.till = std::max(interval.till, interval.from);
		if (interval.from - last.till <= _limits.mergeGap) {
			last.till = interval.till;
			last.flags = uchar((last.flags & kStartUnknown)
				| (interval.flags & (kEndUnknown | kEndGuessed)));
			return;
		}
	} else {
		interval.till = std::max(interval.till, interval.from);
	}
	list.push_back(interval);
	++_total;
}

void Journal::seen(uint64 user, TimeId when, TimeId now) {
	// After a cut the status that was valid then expires without any
	// update: that is not a moment the user was seen at. An earlier time
	// is the real end of the visit that was cut.
	auto cut = TimeId(0);
	if (const auto j = _cut.find(user); j != end(_cut)) {
		if (j->second == when) {
			return;
		}
		cut = j->second;
		_cut.erase(j);
	}
	if (!_recording || (when < now - _limits.retention)) {
		return;
	}
	const auto i = _users.find(user);
	if (i != end(_users) && !i->second.empty()) {
		auto &last = i->second.back();
		if (when < last.from) {
			return;
		} else if (when <= last.till) {
			if (last.flags & kEndGuessed) {
				last.till = when;
				last.flags &= ~kEndGuessed;
				_changed = true;
			}
			return;
		} else if ((when - last.till <= _limits.mergeGap)
			|| ((last.flags & kEndUnknown) && when <= cut)) {
			last.till = when;
			last.flags &= kStartUnknown;
			_changed = true;
			return;
		}
	}
	append(user, { when, when, kStartUnknown });
}

void Journal::addWatched(std::vector<Period> &to, Period period) const {
	if (!to.empty()) {
		auto &last = to.back();
		period.from = std::max(period.from, last.till);
		if (period.till <= period.from) {
			return;
		} else if (period.from - last.till <= _limits.mergeGap) {
			last.till = period.till;
			return;
		}
	} else if (period.till <= period.from) {
		return;
	}
	to.push_back(period);
}

std::vector<uint64> Journal::onlineUsers() const {
	auto result = std::vector<uint64>();
	result.reserve(_open.size());
	for (const auto &[user, open] : _open) {
		result.push_back(user);
	}
	return result;
}

std::vector<Interval> Journal::intervals(uint64 user, TimeId now) const {
	const auto i = _users.find(user);
	auto result = (i != end(_users)) ? i->second : std::vector<Interval>();
	const auto j = _open.find(user);
	if (!_recording || j == end(_open)) {
		return result;
	}
	const auto &open = j->second;
	auto ongoing = Interval{
		.from = open.from,
		.till = std::min(now, open.till),
		.flags = uchar(kOngoing | (open.startUnknown ? kStartUnknown : 0)),
	};
	if (!result.empty()) {
		auto &last = result.back();
		ongoing.from = std::max(ongoing.from, last.till);
		if (ongoing.from - last.till <= _limits.mergeGap) {
			last.till = std::max(ongoing.till, ongoing.from);
			last.flags = uchar((last.flags & kStartUnknown) | kOngoing);
			return result;
		}
	}
	ongoing.till = std::max(ongoing.till, ongoing.from);
	result.push_back(ongoing);
	return result;
}

std::vector<Period> Journal::watched(TimeId now) const {
	auto result = _watched;
	if (_started && _recording && _observing) {
		addWatched(result, { _watchFrom, watchedTill(now) });
	}
	return result;
}

std::vector<Period> Journal::known(uint64 user, TimeId now) const {
	auto result = watched(now);
	const auto i = _covered.find(user);
	if (i == end(_covered) || i->second.empty()) {
		return result;
	}
	result.insert(end(result), begin(i->second), end(i->second));
	std::sort(begin(result), end(result), [](
			const Period &a,
			const Period &b) {
		return (a.from < b.from);
	});
	auto to = begin(result);
	for (auto from = to + 1; from != end(result); ++from) {
		if (from->from - to->till <= _limits.mergeGap) {
			to->till = std::max(to->till, from->till);
		} else {
			*++to = *from;
		}
	}
	result.erase(to + 1, end(result));
	return result;
}

void Journal::forget(uint64 user, TimeId now) {
	const auto i = _users.find(user);
	if (i != end(_users)) {
		_total -= int(i->second.size());
		_users.erase(i);
		_changed = true;
	}
	if (_covered.erase(user)) {
		_changed = true;
	}
	_cut.erase(user);
	const auto j = _open.find(user);
	if (j != end(_open)) {
		j->second.from = now;
		j->second.startUnknown = true;
	}
}

void Journal::clear() {
	_users.clear();
	_open.clear();
	_watched.clear();
	_covered.clear();
	_resume.clear();
	_cut.clear();
	_total = 0;
	_changed = true;
}

bool Journal::clipPeriods(std::vector<Period> &list, TimeId cutoff) const {
	const auto from = std::find_if(begin(list), end(list), [&](
			const Period &period) {
		return (period.till > cutoff);
	});
	const auto changed = (from != begin(list));
	if (changed) {
		list.erase(begin(list), from);
	}
	if (!list.empty() && list.front().from < cutoff) {
		list.front().from = cutoff;
	}
	return changed;
}

void Journal::trimTo(TimeId cutoff) {
	for (auto i = begin(_users); i != end(_users);) {
		auto &list = i->second;
		auto from = std::find_if(begin(list), end(list), [&](
				const Interval &interval) {
			return (interval.till >= cutoff);
		});
		const auto extra = int(end(list) - from) - _limits.perUser;
		if (extra > 0) {
			from += extra;
		}
		if (from != begin(list)) {
			_total -= int(from - begin(list));
			list.erase(begin(list), from);
			_changed = true;
		}
		if (list.empty()) {
			i = _users.erase(i);
		} else {
			++i;
		}
	}
	if (clipPeriods(_watched, cutoff)) {
		_changed = true;
	}
	for (auto i = begin(_covered); i != end(_covered);) {
		if (clipPeriods(i->second, cutoff)) {
			_changed = true;
		}
		if (i->second.empty()) {
			i = _covered.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = begin(_cut); i != end(_cut);) {
		if (i->second <= cutoff) {
			i = _cut.erase(i);
		} else {
			++i;
		}
	}
}

void Journal::trim(TimeId now) {
	auto cutoff = now - _limits.retention;
	trimTo(cutoff);
	while (_total > _limits.total && cutoff < now) {
		cutoff += std::max((now - cutoff) / 4, TimeId(1));
		trimTo(cutoff);
	}
	if (int(_users.size()) > _limits.users) {
		auto last = std::vector<std::pair<TimeId, uint64>>();
		last.reserve(_users.size());
		for (const auto &[user, list] : _users) {
			last.emplace_back(list.back().till, user);
		}
		std::sort(begin(last), end(last));
		const auto remove = int(last.size()) - _limits.users;
		for (auto i = 0; i != remove; ++i) {
			const auto j = _users.find(last[i].second);
			_total -= int(j->second.size());
			_users.erase(j);
		}
		_changed = true;
	}
	if (int(_watched.size()) > _limits.watched) {
		_watched.erase(
			begin(_watched),
			end(_watched) - _limits.watched);
		_changed = true;
	}
	for (auto &[user, list] : _covered) {
		if (int(list.size()) > _limits.covered) {
			list.erase(begin(list), end(list) - _limits.covered);
			_changed = true;
		}
	}
}

QByteArray Journal::serialize(TimeId now) const {
	auto body = QByteArray();
	const auto periods = watched(now);
	WriteVarint(body, periods.size());
	auto previous = TimeId(0);
	for (const auto &period : periods) {
		WriteVarint(body, uint64(period.from - previous));
		WriteVarint(body, uint64(period.till - period.from));
		previous = period.till;
	}

	auto ids = std::vector<uint64>();
	ids.reserve(_users.size() + _open.size());
	for (const auto &[user, list] : _users) {
		ids.push_back(user);
	}
	if (_recording) {
		for (const auto &[user, open] : _open) {
			if (_users.find(user) == end(_users)) {
				ids.push_back(user);
			}
		}
	}
	std::sort(begin(ids), end(ids));
	WriteVarint(body, ids.size());
	for (const auto user : ids) {
		const auto list = intervals(user, now);
		WriteVarint(body, user);
		WriteVarint(body, list.size());
		previous = 0;
		for (const auto &interval : list) {
			const auto flags = (interval.flags & kOngoing)
				? uchar((interval.flags & kStartUnknown) | kEndUnknown)
				: uchar(interval.flags & kStoredFlags);
			const auto length = uint64(interval.till - interval.from);
			WriteVarint(body, uint64(interval.from - previous));
			WriteVarint(body, (length << kFlagsBits) | flags);
			previous = interval.till;
		}
	}

	// The covered periods were added later: the part is optional, so the
	// files written before are read as well.
	if (!_covered.empty()) {
		WriteVarint(body, _covered.size());
		for (const auto &[user, list] : _covered) {
			WriteVarint(body, user);
			WriteVarint(body, list.size());
			previous = 0;
			for (const auto &period : list) {
				WriteVarint(body, uint64(period.from - previous));
				WriteVarint(body, uint64(period.till - period.from));
				previous = period.till;
			}
		}
	}

	auto result = QByteArray();
	result.reserve(kFileHeader + body.size() + kFileFooter);
	WriteUint32(result, kFileMagic);
	result.push_back(char(kFileVersion));
	result.append(body);
	WriteUint32(result, Checksum(
		reinterpret_cast<const uchar*>(body.constData()),
		body.size()));
	return result;
}

bool Journal::parse(const QByteArray &bytes) {
	clear();
	_changed = false;
	if (bytes.size() < kFileHeader + kFileFooter) {
		return false;
	}
	const auto data = reinterpret_cast<const uchar*>(bytes.constData());
	const auto size = bytes.size() - kFileHeader - kFileFooter;
	const auto body = data + kFileHeader;
	if (ReadUint32(data) != kFileMagic
		|| data[4] != kFileVersion
		|| ReadUint32(body + size) != Checksum(body, size)) {
		return false;
	}
	auto reader = Reader(body, size);
	const auto fail = [&] {
		clear();
		_changed = false;
		return false;
	};
	const auto periods = reader.varint();
	if (reader.failed() || periods > uint64(reader.left())) {
		return fail();
	}
	auto previous = int64(0);
	for (auto i = uint64(0); i != periods; ++i) {
		const auto from = previous + reader.time();
		const auto till = from + reader.time();
		if (reader.failed() || till > int64(kMaxTime)) {
			return fail();
		}
		addWatched(_watched, { TimeId(from), TimeId(till) });
		previous = till;
	}
	const auto users = reader.varint();
	if (reader.failed() || users > uint64(reader.left())) {
		return fail();
	}
	for (auto i = uint64(0); i != users; ++i) {
		const auto user = reader.varint();
		const auto count = reader.varint();
		if (reader.failed() || count > uint64(reader.left())) {
			return fail();
		}
		previous = 0;
		for (auto j = uint64(0); j != count; ++j) {
			const auto from = previous + reader.time();
			const auto packed = reader.varint();
			const auto till = from + int64(packed >> kFlagsBits);
			if (reader.failed()
				|| (packed >> kFlagsBits) > kMaxTime
				|| till > int64(kMaxTime)) {
				return fail();
			}
			append(user, {
				.from = TimeId(from),
				.till = TimeId(till),
				.flags = uchar(packed & kStoredFlags),
			});
			previous = till;
		}
	}
	if (!reader.failed() && !reader.atEnd()) {
		const auto covered = reader.varint();
		if (reader.failed() || covered > uint64(reader.left())) {
			return fail();
		}
		for (auto i = uint64(0); i != covered; ++i) {
			const auto user = reader.varint();
			const auto count = reader.varint();
			if (reader.failed() || count > uint64(reader.left())) {
				return fail();
			}
			auto list = std::vector<Period>();
			previous = 0;
			for (auto j = uint64(0); j != count; ++j) {
				const auto from = previous + reader.time();
				const auto till = from + reader.time();
				if (reader.failed() || till > int64(kMaxTime)) {
					return fail();
				}
				addWatched(list, { TimeId(from), TimeId(till) });
				previous = till;
			}
			if (!list.empty()) {
				_covered[user] = std::move(list);
			}
		}
	}
	if (reader.failed() || !reader.atEnd()) {
		return fail();
	}
	_changed = false;
	return true;
}

struct Day {
	QDate date;
	TimeId from = 0;
	TimeId till = 0;
	std::vector<Interval> online;
	std::vector<Period> unknown;
	TimeId total = 0;
	int visits = 0;
};

struct Summary {
	TimeId total = 0;
	TimeId average = 0;
	int visits = 0;
	std::array<TimeId, 24> hours = {};
	std::vector<std::pair<int, int>> typical;
};

[[nodiscard]] TimeId DayStart(const QDate &date, const QTimeZone &zone) {
	return TimeId(date.startOfDay(zone).toSecsSinceEpoch());
}

// A visit was cut while the status was valid (the app was closed) and
// then only the moment that status ran out is known: it is the end of
// the same visit, not one more of them.
[[nodiscard]] bool StatusEcho(const Interval &previous, const Interval &mark) {
	return (mark.till == mark.from)
		&& (mark.flags & kStartUnknown)
		&& (previous.flags & kEndUnknown)
		&& (mark.from - previous.till <= kStatusLife);
}

// The newest day goes first. An interval crossing the local midnight
// is cut in two, the visit itself is counted in the day it started in.
[[nodiscard]] std::vector<Day> SplitByDays(
		const std::vector<Interval> &intervals,
		const std::vector<Period> &watched,
		TimeId now,
		int days,
		const QTimeZone &zone) {
	auto result = std::vector<Day>();
	result.reserve(days);
	const auto today = QDateTime::fromSecsSinceEpoch(now, zone).date();
	for (auto index = 0; index != days; ++index) {
		auto day = Day{ .date = today.addDays(-index) };
		day.from = DayStart(day.date, zone);
		day.till = DayStart(day.date.addDays(1), zone);

		const auto first = std::lower_bound(
			begin(intervals),
			end(intervals),
			day.from,
			[](const Interval &interval, TimeId value) {
				return (interval.till < value);
			});
		for (auto i = first; i != end(intervals); ++i) {
			if (i->from >= day.till) {
				break;
			} else if (i->till == day.from && i->from < day.from) {
				continue;
			}
			const auto from = std::max(i->from, day.from);
			const auto till = std::min(i->till, day.till);
			day.online.push_back({ from, till, i->flags });
			day.total += (till - from);
			if ((i->from >= day.from)
				&& (i == begin(intervals) || !StatusEcho(*(i - 1), *i))) {
				++day.visits;
			}
		}

		const auto limit = std::min(day.till, now);
		auto cursor = day.from;
		const auto start = std::lower_bound(
			begin(watched),
			end(watched),
			day.from,
			[](const Period &period, TimeId value) {
				return (period.till <= value);
			});
		for (auto i = start; i != end(watched); ++i) {
			const auto &period = *i;
			if (cursor >= limit || period.from >= limit) {
				break;
			} else if (period.till <= cursor) {
				continue;
			} else if (period.from > cursor) {
				day.unknown.push_back({ cursor, period.from });
			}
			cursor = std::max(cursor, period.till);
		}
		if (cursor < limit) {
			day.unknown.push_back({ cursor, limit });
		}
		result.push_back(std::move(day));
	}
	return result;
}

// The typical hours are the hours of the day with at least a half of
// the time of the busiest one, joined into ranges (a range may wrap
// around the midnight, its end is then greater than 24). Up to three
// ranges with the most online time are kept, in the order of the day.
[[nodiscard]] Summary Summarize(const std::vector<Day> &days, TimeId now) {
	auto result = Summary();
	auto covered = 0;
	for (const auto &day : days) {
		result.total += day.total;
		result.visits += day.visits;
		auto unknown = TimeId(0);
		for (const auto &period : day.unknown) {
			unknown += (period.till - period.from);
		}
		const auto elapsed = std::min(day.till, now) - day.from;
		if (day.total > 0 || (elapsed > 0 && unknown < elapsed)) {
			++covered;
		}
		for (const auto &interval : day.online) {
			auto time = interval.from;
			while (time < interval.till) {
				const auto hour = std::clamp(
					int((time - day.from) / kHourSeconds),
					0,
					23);
				const auto hourEnd = (hour == 23)
					? day.till
					: (day.from + (hour + 1) * kHourSeconds);
				const auto till = std::min(interval.till, hourEnd);
				result.hours[hour] += (till - time);
				time = till;
			}
		}
	}
	result.average = covered ? (result.total / covered) : 0;

	const auto top = *std::max_element(
		begin(result.hours),
		end(result.hours));
	if (result.total < kTypicalMinTotal || top <= 0) {
		return result;
	}
	const auto active = [&](int hour) {
		return result.hours[hour % 24] * 2 >= top;
	};
	auto start = -1;
	for (auto hour = 0; hour != 24; ++hour) {
		if (!active(hour)) {
			start = hour;
			break;
		}
	}
	if (start < 0) {
		result.typical.emplace_back(0, 24);
		return result;
	}
	struct Range {
		int from = 0;
		int till = 0;
		TimeId weight = 0;
	};
	auto ranges = std::vector<Range>();
	auto current = Range();
	for (auto step = 1; step <= 24; ++step) {
		const auto hour = (start + step) % 24;
		if (active(hour)) {
			if (!current.weight) {
				current.from = hour;
				current.till = hour;
			}
			++current.till;
			current.weight += result.hours[hour];
		} else if (current.weight) {
			ranges.push_back(current);
			current = Range();
		}
	}
	std::stable_sort(begin(ranges), end(ranges), [](
			const Range &a,
			const Range &b) {
		return (a.weight > b.weight);
	});
	if (int(ranges.size()) > kTypicalRanges) {
		ranges.resize(kTypicalRanges);
	}
	std::sort(begin(ranges), end(ranges), [](
			const Range &a,
			const Range &b) {
		return (a.from < b.from);
	});
	for (const auto &range : ranges) {
		result.typical.emplace_back(range.from, range.till);
	}
	return result;
}

// Journal model end.

constexpr auto kHeartbeat = crl::time(30) * 1000;
constexpr auto kSaveDelay = crl::time(300) * 1000;
constexpr auto kIdleSaveInterval = crl::time(600) * 1000;
constexpr auto kTrimEachBeats = 120;
constexpr auto kDisconnectedBeats = 2;
constexpr auto kNotifyCooldown = TimeId(600);
constexpr auto kStartupQuiet = TimeId(15);
constexpr auto kStartupSlow = TimeId(120);
constexpr auto kStartupNames = 3;
constexpr auto kReportDelay = crl::time(500);

// The poll timer ticks every 60-90 seconds, the first time 5-45 seconds
// after the launch and 1-10 seconds after pollSoon(): the delays are
// random, so several accounts don't send their requests together (all
// of them start at the launch and see the same changes of the list).
constexpr auto kPollInterval = crl::time(60) * 1000;
constexpr auto kPollJitter = crl::time(30) * 1000;
constexpr auto kPollFirstDelay = crl::time(5) * 1000;
constexpr auto kPollFirstJitter = crl::time(40) * 1000;
constexpr auto kPollSoonDelay = crl::time(1000);
constexpr auto kPollSoonJitter = crl::time(9) * 1000;

// Never two requests of one account closer than this, whatever happens.
constexpr auto kPollMinGap = crl::time(30) * 1000;

// Never two requests of different accounts closer than this: the random
// delays may still match, then the account that comes second waits
// 3-7 seconds more.
constexpr auto kPollApart = crl::time(3) * 1000;
constexpr auto kPollApartJitter = crl::time(4) * 1000;

// While the account is shown as online the server sends the statuses
// itself, the requests are that rare then.
constexpr auto kPollShownInterval = crl::time(300) * 1000;

// A request without any answer for that long is dropped as failed.
constexpr auto kPollStuckAfter = crl::time(300) * 1000;

// The longest time between two ticks that send a request, with one wait
// for another account.
constexpr auto kPollMaxInterval = kPollInterval
	+ kPollJitter
	+ kPollApart
	+ kPollApartJitter;

// Two answers at most that far from each other cover the time between
// them in the journal: two of the longest intervals, so one missed tick
// makes no hole, and some time for the answer to arrive.
constexpr auto kPollCoverGap = TimeId(220);
static_assert(crl::time(kPollCoverGap) * 1000
	>= 2 * kPollMaxInterval + kPollSoonDelay + kPollSoonJitter);

constexpr auto kPollUsersLimit = 100;
constexpr auto kPollMaxSkips = 10;
constexpr auto kPollFloodMargin = TimeId(5);
constexpr auto kPollFloodFallback = TimeId(300);
constexpr auto kPollFloodMax = 365 * kDaySeconds;

constexpr auto kMaxFileSize = qint64(32) * 1024 * 1024;
constexpr auto kAlertDuration = crl::time(1000);
constexpr auto kFallbackToastDuration = crl::time(5000);
constexpr auto kJournalRefresh = crl::time(30) * 1000;
constexpr auto kWeekDays = 7;
constexpr auto kMonthDays = 30;
constexpr auto kJournalWidth = 480;
constexpr auto kJournalMaxHeight = 640;
constexpr auto kHeaderSkip = 12;
constexpr auto kRowHeight = 26;
constexpr auto kRowStretch = 16;
constexpr auto kBarHeight = 14;
constexpr auto kBarRadius = 3;
constexpr auto kHistogramHeight = 48;
constexpr auto kAxisHeight = 22;
constexpr auto kColumnSkip = 12;
constexpr auto kLegendSkip = 12;
constexpr auto kLegendHeight = 20;
constexpr auto kLegendSwatchWidth = 16;
constexpr auto kLegendSwatchHeight = 10;
constexpr auto kHatchStep = 5;
constexpr auto kMinSegment = 2;
constexpr auto kAxisFontSize = 11;
constexpr auto kTooltipDelay = 300;
constexpr auto kTooltipTolerance = 3;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] bool Trackable(not_null<UserData*> user) {
	return !user->isSelf()
		&& !user->isBot()
		&& !user->isServiceUser()
		&& !user->isInaccessible();
}

[[nodiscard]] QString JournalPath(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/oblivion/"_q
		+ (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare)
		+ u"/online.dat"_q;
}

// Writes go to a thread pool and may run in any order, so every write
// (or removal) of a file gets a generation and only the newest one for
// that path is performed. The disk mutex is held for the whole write, a
// final synchronous write waits for the one in progress. Taking a new
// generation never waits for the disk.
struct FileGenerations {
	std::mutex mutex;
	std::mutex disk;
	std::map<QString, uint64> latest;
};

[[nodiscard]] FileGenerations &Generations() {
	static const auto result = new FileGenerations();
	return *result;
}

[[nodiscard]] uint64 NextGeneration(const QString &path) {
	auto &generations = Generations();
	const auto lock = std::unique_lock(generations.mutex);
	return ++generations.latest[path];
}

[[nodiscard]] bool LatestGeneration(const QString &path, uint64 generation) {
	auto &generations = Generations();
	const auto lock = std::unique_lock(generations.mutex);
	return (generations.latest[path] == generation);
}

void WriteFile(
		const QString &path,
		uint64 generation,
		const QByteArray &bytes) {
	const auto lock = std::unique_lock(Generations().disk);
	if (!LatestGeneration(path, generation)) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)
		&& file.write(bytes) == bytes.size()) {
		file.commit();
	}
}

void RemoveFile(const QString &path, uint64 generation) {
	const auto lock = std::unique_lock(Generations().disk);
	if (LatestGeneration(path, generation)) {
		QFile::remove(path);
	}
}

[[nodiscard]] QByteArray ReadFile(const QString &path) {
	const auto lock = std::unique_lock(Generations().disk);
	auto file = QFile(path);
	if (file.size() > kMaxFileSize || !file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	return file.readAll();
}

[[nodiscard]] base::flat_map<uint64, TimeId> &LastNotified() {
	static auto result = base::flat_map<uint64, TimeId>();
	return result;
}

[[nodiscard]] bool NotifyAllowed(uint64 id, TimeId now) {
	auto &last = LastNotified()[id];
	if (last && now >= last && now - last < kNotifyCooldown) {
		return false;
	}
	last = now;
	return true;
}

// Till when each listed user is online as far as any account of this
// app knows, see PollArrival().
[[nodiscard]] base::flat_map<uint64, TimeId> &KnownOnlineTill() {
	static auto result = base::flat_map<uint64, TimeId>();
	return result;
}

// The server sends the statuses of the contacts and of those there is a
// chat with to an account that it shows as online.
[[nodiscard]] bool StatusPushed(not_null<UserData*> user) {
	if (user->isContact()) {
		return true;
	}
	const auto history = user->owner().historyLoaded(user);
	return history && history->inChatList();
}

// How long ago a request was sent. Long enough for everything that
// looks at it if there was none yet.
[[nodiscard]] crl::time PollAgo(crl::time sentAt) {
	return sentAt
		? std::max(crl::now() - sentAt, crl::time(0))
		: std::max({ kPollShownInterval, kPollMinGap, kPollApart });
}

// When the last request was sent by any account of this app.
[[nodiscard]] crl::time &PollSentByAnyAccount() {
	static auto result = crl::time(0);
	return result;
}

// The same rule as for the notifications about messages.
[[nodiscard]] bool NotifyFromAccount(not_null<Main::Session*> session) {
	return !Core::Quitting()
		&& (Core::App().settings().notifyFromAll()
			|| (&session->account() == &Core::App().domain().active()));
}

// The system replaces a delivered notification that has the same peer
// and message id instead of showing a new one. These ids are unique for
// one launch and can't match a message: server ids are positive, local
// ones start far below zero.
[[nodiscard]] MsgId NextNotificationId() {
	static auto counter = int64(0);
	return MsgId(--counter);
}

// With a summary several users are reported at once: a click opens the
// chat with the given one, their photo is not shown.
void ShowOnlineNotification(
		not_null<UserData*> user,
		const QString &summary = QString()) {
	const auto session = &user->session();
	auto &app = Core::App();
	const auto name = user->name();
	const auto several = !summary.isEmpty();
	const auto window = app.passcodeLocked() ? nullptr : app.activeWindow();
	if (window
		&& window->widget()->isActive()
		&& (window->account().maybeSession() == session)) {
		window->showToast(several
			? summary
			: tr::lng_oblivion_online_now(tr::now, lt_user, name));
		return;
	}
	using namespace Window::Notifications;
	auto &manager = app.notifications().manager();
	const auto &settings = app.settings();
	const auto notify = settings.desktopNotify() && !manager.skipToast();
	if (notify && manager.type() == ManagerType::Default) {
		// The notifications drawn by the app itself (the default ones on
		// Windows, where the system ones are an option) are made only from
		// messages. Till they learn to show this one, it is a toast in a
		// window of the app, which is seen if the window is.
		const auto own = ExistingWindow(session, user);
		const auto target = app.passcodeLocked()
			? nullptr
			: own
			? &own->window()
			: app.activePrimaryWindow();
		if (target) {
			target->showToast(
				(several
					? summary
					: tr::lng_oblivion_online_now(tr::now, lt_user, name)),
				kFallbackToastDuration);
		}
	} else if (notify) {
		auto options = manager.getNotificationOptions(
			nullptr,
			Data::ItemNotificationType::Message);
		options.hideMarkAsRead = true;
		options.hideReplyButton = true;
		const auto hidden = options.hideNameAndPhoto;
		options.hideNameAndPhoto = hidden || several;
		auto view = user->createUserpicView();
		static_cast<NativeManager&>(manager).showOblivionNotification({
			.peer = user,
			.itemId = NextNotificationId(),
			.title = (hidden
				? AppName.utf16()
				: manager.addTargetAccountName(
					several ? AppName.utf16() : name,
					session)),
			.message = (hidden
				? tr::lng_oblivion_online_now_hidden(tr::now)
				: several
				? summary
				: tr::lng_oblivion_online_now_text(tr::now)),
			.options = options,
		}, view);
	}
	if (settings.flashBounceNotify()) {
		// An account that is not shown in any window has no window of its
		// own, the alert goes to the main one like for its new messages.
		const auto own = ExistingWindow(session, user);
		const auto target = own ? &own->window() : app.activePrimaryWindow();
		if (target) {
			manager.maybeFlashBounce(crl::guard(target, [=] {
				const auto widget = target->widget();
				if (const auto handle = widget->windowHandle()) {
					handle->alert(kAlertDuration);
				}
			}));
		}
	}
	if (settings.soundNotify()) {
		manager.maybePlaySound(crl::guard(session, [=] {
			Core::App().notifications().playSound(session, 0);
		}));
	}
}

// Poll logic begin: no session and no clock, covered by RunSelfTest.

// One account that could ask about a listed user. The accounts have
// different non-zero ids, `knows` is "has this user fully loaded", `sees`
// is "the server tells this account the exact status of the user" (it
// means nothing without `knows`).
struct PollCandidate {
	uint64 account = 0;
	bool active = false;
	bool knows = false;
	bool sees = false;
};

// One listed user is asked about by one account only: the active one if
// it knows the user, otherwise the one with the smallest id of those
// that do. The accounts that see the exact status go first: an account
// that gets only "last seen recently" in the answer learns nothing from
// it, so it asks only if no account sees the status. Zero if nobody
// knows the user.
[[nodiscard]] uint64 PollOwner(const std::vector<PollCandidate> &list) {
	const auto choose = [&](bool seeing) {
		auto result = uint64(0);
		for (const auto &candidate : list) {
			if (!candidate.account
				|| !candidate.knows
				|| (seeing && !candidate.sees)) {
				continue;
			} else if (candidate.active) {
				return candidate.account;
			} else if (!result || candidate.account < result) {
				result = candidate.account;
			}
		}
		return result;
	};
	const auto seeing = choose(true);
	return seeing ? seeing : choose(false);
}

// One request has at most `limit` users. A longer list is asked in
// turns: each time the ids that follow the last asked one, from the
// beginning again after the end. The ids are sorted.
[[nodiscard]] std::vector<uint64> PollChunk(
		const std::vector<uint64> &sorted,
		uint64 after,
		int limit) {
	if (limit <= 0) {
		return {};
	} else if (int(sorted.size()) <= limit) {
		return sorted;
	}
	auto from = std::upper_bound(begin(sorted), end(sorted), after);
	if (from == end(sorted)) {
		from = begin(sorted);
	}
	const auto count = std::min(int(end(sorted) - from), limit);
	return std::vector<uint64>(from, from + count);
}

// In how many requests the whole list is asked.
[[nodiscard]] int PollChunks(int count, int limit) {
	return (limit <= 0 || count <= limit)
		? 1
		: ((count + limit - 1) / limit);
}

// The seconds to wait from a FLOOD_WAIT_N or FLOOD_PREMIUM_WAIT_N error,
// zero for any other error.
[[nodiscard]] TimeId PollFloodSeconds(const QString &type) {
	if (!MTP::IsFloodError(type)) {
		return 0;
	}
	auto ok = false;
	const auto digits = type.mid(type.lastIndexOf(QChar('_')) + 1);
	const auto value = digits.toLongLong(&ok);
	return (!ok || value <= 0)
		? kPollFloodFallback
		: TimeId(std::min(value, qlonglong(kPollFloodMax)));
}

// The wait asked by the server is kept as a moment in unixtime, so it
// goes on through a sleep, and nothing but the time itself ends it.
struct PollFlood {
	TimeId till = 0;
	TimeId wait = 0;

	void start(TimeId now, TimeId seconds) {
		wait = std::max(seconds, TimeId(1)) + kPollFloodMargin;
		till = now + wait;
	}

	// After the clock was set back the wait is counted from the start
	// again: it never gets longer than it was asked, and never shorter.
	[[nodiscard]] bool waiting(TimeId now) {
		if (!till) {
			return false;
		} else if (now >= till) {
			till = wait = 0;
			return false;
		} else if (till - now > wait) {
			till = now + wait;
		}
		return true;
	}
};

// Any `random` gives a delay from `from` to `from + spread`.
[[nodiscard]] crl::time PollSpread(
		crl::time from,
		crl::time spread,
		uint32 random) {
	return (spread > 0)
		? (from + crl::time(random % uint64(spread + 1)))
		: from;
}

// pollSoon() waits out the rest of the smallest gap between requests.
[[nodiscard]] crl::time PollSoonDelay(crl::time delay, crl::time sinceSent) {
	return delay + std::max(kPollMinGap - sinceSent, crl::time(0));
}

// Whether a request has to wait because another account of this app
// has sent its own one just now.
[[nodiscard]] bool PollTooClose(crl::time sinceAnyAccount) {
	return (sinceAnyAccount < kPollApart);
}

// Whether a tick of the timer asks about everyone. An account shown as
// online gets the statuses of its contacts and of those it has a chat
// with from the server: it asks about them rarely, and at the other
// ticks only about the rest. `due` is set by everything that needs a
// fresh answer at once: the account became or stopped being shown as
// online, the list has changed, the app woke up or connected again, a
// request has failed.
[[nodiscard]] bool PollEveryone(bool shown, bool due, crl::time sinceAll) {
	return !shown || due || (sinceAll >= kPollShownInterval);
}

// A list longer than one request is asked about in `turns` requests, so
// "everyone was asked" is true only after that many ticks in a row.
// `left` is how many of them remain, zero between such rounds. Called
// for each request about everyone, returns whether it is the last one.
[[nodiscard]] bool PollRoundEnds(int &left, int turns) {
	left = (left > 0) ? (left - 1) : (std::max(turns, 1) - 1);
	return !left;
}

// Only one account asks about a listed user. When that account changes,
// the new one sees the user online for the first time, which is not an
// arrival if some account knew the user as online till later than now.
// `known` is that moment, one for all the accounts. `left` is "this
// account saw the visit end". Returns whether to report the arrival.
[[nodiscard]] bool PollArrival(
		TimeId &known,
		bool came,
		bool left,
		TimeId till,
		TimeId now) {
	if (known > now + kStatusLife) {
		// The clock was set back.
		known = 0;
	}
	const auto result = came && (known <= now);
	if (till > now) {
		known = std::min(till, now + kStatusLife);
	} else if (left) {
		known = 0;
	}
	return result;
}

// The statuses of the asked users were known between two answers that
// came close enough. The end of a time watched by the journal counts as
// an answer: the statuses were known till then. The period starts at
// the earlier of the two that is close enough, it is empty if both are
// too far.
[[nodiscard]] Period PollCovered(
		TimeId polledAt,
		TimeId watchedTill,
		TimeId now,
		TimeId gap) {
	const auto close = [&](TimeId from) {
		return (from > 0) && (now > from) && (now - from <= gap);
	};
	const auto from = !close(polledAt)
		? watchedTill
		: close(watchedTill)
		? std::min(polledAt, watchedTill)
		: polledAt;
	return close(from) ? Period{ from, now } : Period();
}

// Poll logic end.

struct JournalData {
	std::vector<Interval> intervals;
	std::vector<Period> watched;
	TimeId now = 0;
	QString status;
	bool online = false;
	bool hidden = false;
	bool recording = true;
	bool listed = true;
};

// The accounts with a working tracker, defined after the trackers map.
[[nodiscard]] std::vector<not_null<Main::Session*>> PollSessions();

class Tracker final : public base::has_weak_ptr {
public:
	explicit Tracker(not_null<Main::Session*> session);
	~Tracker();

	void forget();
	[[nodiscard]] bool forgotten() const {
		return _forgotten;
	}
	void clear(not_null<UserData*> user);
	[[nodiscard]] JournalData data(not_null<UserData*> user);
	[[nodiscard]] rpl::producer<uint64> changes() const {
		return _changes.events();
	}

private:
	void load();
	void firstUpdateCheck();
	void apply(not_null<UserData*> user, bool fireChanges = true);
	void checkAlive(TimeId now);
	void resumeWatching(TimeId now);
	[[nodiscard]] bool connected() const;
	[[nodiscard]] bool shownOnline() const;
	void updateObserving();
	void heartbeat();
	void checkExpired();
	void settingsChanged();
	void cameOnline(not_null<UserData*> user, TimeId now);
	void reportArrived();
	void pollSoon();
	void poll();
	void pollStop();
	void pollFailed();
	void pollEveryoneNext();
	void polled(const std::vector<uint64> &ids, TimeId gap);
	[[nodiscard]] std::vector<uint64> pollIds() const;
	void saveLater();
	void save(bool sync);
	void savedOrChanged(uint64 user);

	const not_null<Main::Session*> _session;
	const QString _path;
	MTP::Sender _api;
	Journal _journal;
	base::Timer _heartbeatTimer;
	base::Timer _expireTimer;
	base::Timer _saveTimer;
	base::Timer _reportTimer;
	base::Timer _pollTimer;
	std::vector<not_null<UserData*>> _arrived;
	base::flat_set<uint64> _pollList;
	base::flat_map<uint64, TimeId> _polledAt;
	rpl::event_stream<uint64> _changes;
	crl::time _lastSaved = 0;
	TimeId _startedAt = 0;
	TimeId _quietTill = 0;
	mtpRequestId _pollRequest = 0;
	PollFlood _pollFlood;
	crl::time _pollSentAt = 0;
	crl::time _pollAllAt = 0;
	TimeId _pollWatchedTill = 0;
	uint64 _pollCursor = 0;
	int _pollAllLeft = 0;
	int _pollFails = 0;
	int _pollSkip = 0;
	bool _pollDue = true;
	bool _polling = true;
	bool _firstUpdateSeen = false;
	int _beats = 0;
	int _disconnectedBeats = 0;
	bool _forgotten = false;

	rpl::lifetime _lifetime;

};

Tracker::Tracker(not_null<Main::Session*> session)
: _session(session)
, _path(JournalPath(session))
, _api(&session->mtp())
, _heartbeatTimer([=] { heartbeat(); })
, _expireTimer([=] { checkExpired(); })
, _saveTimer([=] { save(false); })
, _reportTimer([=] { reportArrived(); })
, _pollTimer([=] { poll(); })
, _pollList(Get().onlineNotifyList())
, _lastSaved(crl::now())
, _startedAt(base::unixtime::now())
, _polling(Get().onlinePolling()) {
	_journal.setRecording(Get().onlineJournal(), _startedAt);
	load();
	_journal.trim(_startedAt);
	_journal.setObserving(shownOnline(), _startedAt);
	_journal.start(_startedAt);

	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::OnlineStatus
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		const auto user = update.peer->asUser();
		if (!user) {
			return;
		} else if (user->isSelf()) {
			updateObserving();
		} else {
			firstUpdateCheck();
			apply(user);
		}
	}, _lifetime);

	Get().changes() | rpl::on_next([=] {
		settingsChanged();
	}, _lifetime);

	_quietTill = _startedAt + kStartupQuiet;
	_heartbeatTimer.callEach(kHeartbeat);
	_reportTimer.callOnce(crl::time(kStartupQuiet + 1) * 1000);
	_pollTimer.callOnce(PollSpread(
		kPollFirstDelay,
		kPollFirstJitter,
		base::RandomValue<uint32>()));
}

// The statuses of those who are online already come with the first
// server answers. On a slow connection that is later than the usual
// quiet time after the launch, so it is counted from the first update.
void Tracker::firstUpdateCheck() {
	if (_firstUpdateSeen) {
		return;
	}
	_firstUpdateSeen = true;
	const auto now = base::unixtime::now();
	if (now >= _startedAt && now < _startedAt + kStartupSlow) {
		_quietTill = now + kStartupQuiet;
		_reportTimer.callOnce(crl::time(kStartupQuiet + 1) * 1000);
	}
}

Tracker::~Tracker() {
	if (_forgotten) {
		return;
	}
	_journal.stop(base::unixtime::now());
	if (!_journal.empty() || QFile::exists(_path)) {
		save(true);
	}
}

void Tracker::load() {
	const auto bytes = ReadFile(_path);
	if (!bytes.isEmpty() && !_journal.parse(bytes)) {
		LOG(("Oblivion Online: the journal file is damaged, starting anew."));
	}
}

void Tracker::forget() {
	_forgotten = true;
	_heartbeatTimer.cancel();
	_expireTimer.cancel();
	_saveTimer.cancel();
	_reportTimer.cancel();
	_pollTimer.cancel();
	_api.request(base::take(_pollRequest)).cancel();
	_arrived.clear();
	_polledAt.clear();
	_lifetime.destroy();
	_journal.stop(base::unixtime::now());
	_journal.clear();
	crl::async([path = _path, generation = NextGeneration(_path)] {
		RemoveFile(path, generation);
	});
}

void Tracker::clear(not_null<UserData*> user) {
	const auto id = user->id.value;
	_journal.forget(id, base::unixtime::now());
	savedOrChanged(id);
}

JournalData Tracker::data(not_null<UserData*> user) {
	apply(user, false);
	const auto now = base::unixtime::now();
	const auto id = user->id.value;
	return {
		.intervals = _journal.intervals(id, now),
		.watched = _journal.known(id, now),
		.now = now,
		.status = Data::OnlineTextFull(user, now),
		.online = Data::IsUserOnline(user, now),
		.hidden = user->lastseen().isHidden(),
		.recording = _journal.recording(),
		.listed = Get().isOnlineNotify(id) && Get().onlinePolling(),
	};
}

// The watching stopped by a lost connection starts again only by a status
// that came from the server: reading the journal, or a status expiring
// locally while there is still no connection, doesn't count.
void Tracker::apply(not_null<UserData*> user, bool fireChanges) {
	if (_forgotten || !Trackable(user)) {
		return;
	}
	const auto now = base::unixtime::now();
	const auto id = user->id.value;
	if (_journal.started()) {
		checkAlive(now);
	} else if (fireChanges && connected()) {
		resumeWatching(now);
	} else {
		return;
	}
	const auto was = _journal.online(id);
	const auto till = user->lastseen().onlineTill();
	const auto came = _journal.apply(id, till, now);
	const auto left = was && !_journal.online(id);
	if (!_expireTimer.isActive()) {
		checkExpired();
	}
	if (Get().isOnlineNotify(id)) {
		// Only the accounts that may notify share what they know: an
		// arrival seen first by an account whose notifications are
		// dropped must still be reported by the one that shows them.
		const auto arrived = NotifyFromAccount(_session)
			? PollArrival(KnownOnlineTill()[id], came, left, till, now)
			: came;
		if (arrived) {
			cameOnline(user, now);
		}
	}
	if (fireChanges && (came || was != _journal.online(id))) {
		_changes.fire_copy(id);
	}
	if (_journal.takeChanged()) {
		saveLater();
	}
}

// After a sleep all the visits were cut at the last heartbeat. The journal
// itself continues the visits of those whose status has not expired, so
// they are not reported as coming online once more. Nothing is known
// about the time of the sleep, the answers before it cover nothing.
void Tracker::checkAlive(TimeId now) {
	if (!_journal.alive(now)) {
		_polledAt.clear();
		_pollWatchedTill = 0;
		pollSoon();
		_changes.fire(0);
	}
}

// The watching starts again after a lost connection.
void Tracker::resumeWatching(TimeId now) {
	_disconnectedBeats = 0;
	if (_journal.started()) {
		return;
	}
	_journal.start(now);
	pollSoon();
	_changes.fire(0);
}

bool Tracker::connected() const {
	return (_session->mtp().dcstate() == MTP::ConnectedState);
}

// The server sends the statuses of other users only to an account that
// it shows as online itself: its window is active and was used recently,
// and the "don't update the online status" ghost switch is off.
bool Tracker::shownOnline() const {
	return !Get().ghostOnline() && _session->updates().lastWasOnline();
}

void Tracker::updateObserving() {
	const auto observing = shownOnline();
	if (_forgotten || _journal.observing() == observing) {
		return;
	}
	const auto now = base::unixtime::now();
	const auto watching = _journal.started() && !_journal.asleep(now);
	if (_journal.started()) {
		checkAlive(now);
	}
	_journal.setObserving(observing, now);

	// The next tick of the poll timer asks about everyone in any case.
	// The time around the change must be covered by the answers: where
	// the watched time ends, the covered one begins. And an account that
	// has just become shown as online knows only the old statuses until
	// the server sends it a change.
	pollEveryoneNext();
	if (!observing) {
		_pollWatchedTill = watching ? now : 0;
	}
	savedOrChanged(0);
}

void Tracker::heartbeat() {
	if (_forgotten) {
		return;
	}
	const auto now = base::unixtime::now();
	if (connected()) {
		if (!_journal.started()) {
			resumeWatching(now);
		} else {
			_disconnectedBeats = 0;
			checkAlive(now);
		}
	} else if (_journal.started()
		&& ++_disconnectedBeats >= kDisconnectedBeats) {
		_journal.pause();
		_changes.fire(0);
	}
	updateObserving();
	checkExpired();
	if (!(++_beats % kTrimEachBeats)) {
		_journal.trim(now);
	}
	if (_journal.takeChanged()) {
		saveLater();
	} else if (_journal.recording()
		&& !_saveTimer.isActive()
		&& crl::now() - _lastSaved >= kIdleSaveInterval) {
		save(false);
	}
}

void Tracker::checkExpired() {
	if (_forgotten) {
		return;
	}
	const auto now = base::unixtime::now();
	if (_journal.asleep(now)) {
		// The timer has fired after a sleep: the statuses must be cut at
		// the last heartbeat, not closed at their expiration times.
		checkAlive(now);
	}
	const auto users = _journal.onlineUsers();
	const auto nearest = _journal.expire(now);
	for (const auto id : users) {
		if (!_journal.online(id)) {
			_changes.fire_copy(id);
		}
	}
	if (nearest) {
		const auto wait = crl::time(std::max(nearest - now, 0) + 1) * 1000;
		_expireTimer.callOnce(wait);
	} else {
		_expireTimer.cancel();
	}
	if (_journal.takeChanged()) {
		saveLater();
	}
}

void Tracker::settingsChanged() {
	if (_forgotten) {
		return;
	}
	updateObserving();
	const auto polling = Get().onlinePolling();
	const auto switched = (_polling != polling);
	const auto listed = (_pollList != Get().onlineNotifyList());
	_polling = polling;
	if (listed) {
		_pollList = Get().onlineNotifyList();
	}
	if (!polling) {
		pollStop();
	} else if (switched || listed) {
		pollSoon();
	}
	const auto recording = Get().onlineJournal();
	if (_journal.recording() == recording) {
		return;
	}
	_journal.setRecording(recording, base::unixtime::now());
	savedOrChanged(0);
}

void Tracker::savedOrChanged(uint64 user) {
	if (_journal.takeChanged()) {
		saveLater();
	}
	_changes.fire_copy(user);
}

// Those who are online already at the launch are collected for the whole
// quiet time, later arrivals only for a moment: several statuses often
// come together and are reported together.
void Tracker::cameOnline(not_null<UserData*> user, TimeId now) {
	if (!ranges::contains(_arrived, user)) {
		_arrived.push_back(user);
	}
	const auto quiet = (now >= _startedAt && now < _quietTill);
	if (!_reportTimer.isActive()) {
		_reportTimer.callOnce(quiet
			? (crl::time(_quietTill - now + 1) * 1000)
			: kReportDelay);
	}
}

// The cooldown of a user is taken only when the user is really reported.
void Tracker::reportArrived() {
	auto users = base::take(_arrived);
	if (_forgotten || users.empty() || !NotifyFromAccount(_session)) {
		return;
	}
	const auto now = base::unixtime::now();
	users.erase(ranges::remove_if(users, [&](not_null<UserData*> user) {
		const auto id = user->id.value;
		return !Get().isOnlineNotify(id)
			|| !Data::IsUserOnline(user, now)
			|| !NotifyAllowed(id, now);
	}), end(users));
	if (users.empty()) {
		return;
	} else if (users.size() == 1) {
		ShowOnlineNotification(users.front());
		return;
	}
	auto names = QStringList();
	for (const auto &user : users) {
		if (names.size() == kStartupNames) {
			break;
		}
		names.push_back(user->shortName());
	}
	auto list = names.join(u", "_q);
	const auto more = int(users.size()) - int(names.size());
	if (more > 0) {
		list = tr::lng_oblivion_online_now_more(
			tr::now,
			lt_users,
			list,
			lt_value,
			QString::number(more));
	}
	ShowOnlineNotification(
		users.front(),
		tr::lng_oblivion_online_now_list(tr::now, lt_users, list));
}

// The wait asked by the server (_pollFlood) is not touched here.
void Tracker::pollSoon() {
	if (_forgotten) {
		return;
	}
	_pollSkip = 0;
	pollEveryoneNext();
	_pollTimer.callOnce(PollSoonDelay(
		PollSpread(
			kPollSoonDelay,
			kPollSoonJitter,
			base::RandomValue<uint32>()),
		PollAgo(_pollSentAt)));
}

void Tracker::pollStop() {
	_api.request(base::take(_pollRequest)).cancel();
	_polledAt.clear();
}

// Errors make the requests rarer: after each one in a row one more tick
// is skipped, up to kPollMaxSkips of them.
void Tracker::pollFailed() {
	_pollFails = std::min(_pollFails + 1, kPollMaxSkips);
	_pollSkip = _pollFails;
	pollEveryoneNext();
}

// A fresh answer about everyone is needed, see PollEveryone(). A round
// of requests about everyone that was going on starts again.
void Tracker::pollEveryoneNext() {
	_pollDue = true;
	_pollAllLeft = 0;
}

// Every listed user is asked about by one account only, see PollOwner():
// several accounts of one person asking about the same people at once
// is what the official apps never do. The result is sorted.
//
// The price: the journals of the other accounts that know the user get
// no covered periods for them, the time while those accounts are not
// shown as online is "no data" there. And when the owner changes (the
// active account was switched, or the status became visible to another
// account), the new one sees the user online for the first time: its
// journal starts a visit at that moment, and PollArrival() keeps it
// from being reported as an arrival.
//
// An account that the user hides the last seen time from (or that hides
// its own one, so the server hides everyone's from it) gets nothing from
// the answers. So an account that sees the exact status is chosen first,
// even if it is not the active one: otherwise the active account would
// ask in vain while the one that could tell stays silent. Each account
// judges by the last status it has got. The owner gets a fresh one with
// each answer, so a change of the privacy moves the user to the right
// account by itself. And if nobody sees the status, the active account
// asks as before, so it notices when the status becomes visible to it.
// Only an account that neither asks nor is shown as online may keep an
// outdated "hidden" until something else loads the user there.
//
// The choice doesn't look at whether the owner can ask right now: while
// it has no connection or the server asked it to wait, nobody asks in
// its place.
std::vector<uint64> Tracker::pollIds() const {
	auto result = std::vector<uint64>();
	const auto &list = Get().onlineNotifyList();
	if (list.empty()) {
		return result;
	}
	const auto sessions = PollSessions();
	const auto active = Core::App().domain().active().maybeSession();
	const auto self = _session->uniqueId();
	auto candidates = std::vector<PollCandidate>();
	candidates.reserve(sessions.size());
	for (const auto id : list) {
		const auto userId = peerToUser(PeerId(id));
		const auto mine = _session->data().userLoaded(userId);
		if (!mine || !Trackable(mine)) {
			continue;
		}
		candidates.clear();
		for (const auto &session : sessions) {
			const auto user = session->data().userLoaded(userId);
			const auto knows = user && Trackable(user);
			candidates.push_back({
				.account = session->uniqueId(),
				.active = (session.get() == active),
				.knows = knows,
				.sees = knows && !user->lastseen().isHidden(),
			});
		}
		if (PollOwner(candidates) == self) {
			result.push_back(id);
		}
	}
	return result;
}

// The statuses of the users from the notify list are requested: the
// server doesn't send them while this account is not shown as online,
// and never does for those who are neither contacts nor have a chat.
//
// The official apps send nothing like that, so a request goes out only
// if "Check statuses in background" is on, its answer can be used and
// the server didn't ask to wait.
void Tracker::poll() {
	if (_forgotten) {
		return;
	}
	_pollTimer.callOnce(PollSpread(
		kPollInterval,
		kPollJitter,
		base::RandomValue<uint32>()));
	if (Core::Quitting()) {
		return;
	} else if (!Get().onlinePolling()) {
		pollStop();
		return;
	} else if (_pollRequest) {
		// No answer for too long, like when the request is repeated
		// after server errors. It is dropped and counted as a failure,
		// a new one is sent by one of the next ticks, never by this one.
		if (PollAgo(_pollSentAt) >= kPollStuckAfter) {
			_api.request(base::take(_pollRequest)).cancel();
			pollFailed();
		}
		return;
	} else if (_pollFlood.waiting(base::unixtime::now())) {
		return;
	} else if (_pollSkip > 0) {
		--_pollSkip;
		return;
	} else if (!connected()) {
		return;
	} else if (!Get().onlineJournal() && !NotifyFromAccount(_session)) {
		// Nothing to write the answer to and nobody to tell about it.
		_polledAt.clear();
		return;
	}
	const auto owned = pollIds();
	for (auto i = begin(_polledAt); i != end(_polledAt);) {
		if (std::binary_search(begin(owned), end(owned), i->first)) {
			++i;
		} else {
			i = _polledAt.erase(i);
		}
	}

	// While this account is shown as online, most of the ticks ask only
	// about those whose statuses the server doesn't send by itself.
	const auto everyone = PollEveryone(
		_journal.observing(),
		_pollDue,
		PollAgo(_pollAllAt));
	auto asked = owned;
	if (!everyone) {
		asked.erase(ranges::remove_if(asked, [&](uint64 id) {
			const auto userId = peerToUser(PeerId(id));
			const auto user = _session->data().userLoaded(userId);
			return !user || StatusPushed(user);
		}), end(asked));
	}
	const auto chunk = PollChunk(asked, _pollCursor, kPollUsersLimit);
	auto inputs = QVector<MTPInputUser>();
	inputs.reserve(chunk.size());
	for (const auto id : chunk) {
		const auto userId = peerToUser(PeerId(id));
		if (const auto user = _session->data().userLoaded(userId)) {
			inputs.push_back(user->inputUser());
		}
	}
	if (inputs.isEmpty()) {
		return;
	} else if (PollTooClose(PollAgo(PollSentByAnyAccount()))) {
		// Another account has asked just now. Nothing was changed yet,
		// this tick is simply repeated a few seconds later.
		_pollTimer.callOnce(PollSpread(
			kPollApart,
			kPollApartJitter,
			base::RandomValue<uint32>()));
		return;
	}

	// A long list is asked in turns, so each user is asked about less
	// often and the answers that cover the time between them are farther
	// from each other.
	const auto turns = PollChunks(int(asked.size()), kPollUsersLimit);
	const auto gap = kPollCoverGap * turns;
	if (turns > 1) {
		_pollCursor = chunk.back();
	}
	_pollSentAt = crl::now();
	PollSentByAnyAccount() = _pollSentAt;
	if (everyone && PollRoundEnds(_pollAllLeft, turns)) {
		_pollAllAt = _pollSentAt;
		_pollDue = false;
	}
	_pollRequest = _api.request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(std::move(inputs))
	)).done([=](const MTPVector<MTPUser> &result) {
		_pollRequest = 0;
		_pollFails = 0;
		_session->data().processUsers(result);
		auto ids = std::vector<uint64>();
		ids.reserve(result.v.size());
		for (const auto &user : result.v) {
			user.match([&](const MTPDuser &data) {
				ids.push_back(peerFromUser(data.vid()).value);
			}, [](const MTPDuserEmpty &) {
			});
		}
		polled(ids, gap);
	}).fail([=](const MTP::Error &error) {
		// FLOOD_WAIT comes here as well (handleFloodErrors), instead of
		// being waited out inside of MTP: the tracker waits in full by
		// itself and asks nothing till then.
		_pollRequest = 0;
		if (const auto wait = PollFloodSeconds(error.type())) {
			_pollFlood.start(base::unixtime::now(), wait);
		}
		pollFailed();
	}).handleFloodErrors().send();
}

// Between two answers that came one right after another the statuses of
// these users were known, see PollCovered(). An answer that is the first
// thing to happen after a sleep must not cover the time of the sleep.
void Tracker::polled(const std::vector<uint64> &ids, TimeId gap) {
	if (_forgotten) {
		return;
	}
	const auto now = base::unixtime::now();
	if (_journal.started()) {
		checkAlive(now);
	}
	for (const auto id : ids) {
		auto &at = _polledAt[id];
		const auto period = PollCovered(at, _pollWatchedTill, now, gap);
		if (period.till > period.from) {
			_journal.cover(id, period);
		}
		at = now;
	}
	if (_journal.takeChanged()) {
		saveLater();
	}
}

void Tracker::saveLater() {
	if (!_forgotten && !_saveTimer.isActive()) {
		_saveTimer.callOnce(kSaveDelay);
	}
}

// The serialization of a big journal takes time, so a copy of it is
// serialized on the thread that writes the file.
void Tracker::save(bool sync) {
	if (_forgotten) {
		return;
	}
	_saveTimer.cancel();
	_lastSaved = crl::now();
	[[maybe_unused]] const auto changed = _journal.takeChanged();
	const auto now = base::unixtime::now();
	const auto generation = NextGeneration(_path);
	if (sync) {
		WriteFile(_path, generation, _journal.serialize(now));
	} else {
		crl::async([path = _path, journal = _journal, generation, now] {
			WriteFile(path, generation, journal.serialize(now));
		});
	}
}

using TrackersMap = base::flat_map<
	not_null<Main::Session*>,
	std::unique_ptr<Tracker>>;

// Never destroyed: a tracker is removed (and saved) together with its
// session, nothing here should run during the static destruction.
[[nodiscard]] TrackersMap &Trackers() {
	static const auto result = new TrackersMap();
	return *result;
}

[[nodiscard]] Tracker *TrackerFor(not_null<Main::Session*> session) {
	const auto &trackers = Trackers();
	const auto i = trackers.find(session);
	return (i != end(trackers)) ? i->second.get() : nullptr;
}

std::vector<not_null<Main::Session*>> PollSessions() {
	auto result = std::vector<not_null<Main::Session*>>();
	for (const auto &[session, tracker] : Trackers()) {
		if (!tracker->forgotten()) {
			result.push_back(session);
		}
	}
	return result;
}

// A listed user that this account doesn't know is tracked by another
// account of this app that does, if there is one.
[[nodiscard]] bool KnownToOtherAccount(
		not_null<Main::Session*> session,
		uint64 id) {
	const auto userId = peerToUser(PeerId(id));
	for (const auto &other : PollSessions()) {
		if (other != session) {
			const auto user = other->data().userLoaded(userId);
			if (user && Trackable(user)) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] QLocale DateLocale() {
	return QLocale(CurrentLanguageIsRussian()
		? QLocale::Russian
		: QLocale::English);
}

[[nodiscard]] QString FormatTime(TimeId time, const QTimeZone &zone) {
	return QDateTime::fromSecsSinceEpoch(
		time,
		zone).time().toString(u"HH:mm"_q);
}

[[nodiscard]] QString FormatDuration(TimeId seconds) {
	if (seconds <= 0) {
		return QString(QChar(0x2014));
	} else if (seconds < 60) {
		return tr::lng_oblivion_online_duration_less(tr::now);
	}
	const auto minutes = seconds / 60;
	const auto hours = QString::number(minutes / 60);
	const auto rest = QString::number(minutes % 60);
	return (minutes < 60)
		? tr::lng_oblivion_online_duration_m(tr::now, lt_minutes, rest)
		: (minutes % 60)
		? tr::lng_oblivion_online_duration_hm(
			tr::now,
			lt_hours,
			hours,
			lt_minutes,
			rest)
		: tr::lng_oblivion_online_duration_h(tr::now, lt_hours, hours);
}

[[nodiscard]] QString FormatDay(const QDate &date, const QDate &today) {
	if (date == today) {
		return tr::lng_oblivion_online_journal_today(tr::now);
	} else if (date.addDays(1) == today) {
		return tr::lng_oblivion_online_journal_yesterday(tr::now);
	}
	return DateLocale().toString(date, u"ddd, d MMM"_q);
}

[[nodiscard]] QString FormatHour(int hour) {
	return u"%1:00"_q.arg(hour, 2, 10, QChar('0'));
}

[[nodiscard]] QString FormatTypical(const Summary &summary) {
	if (summary.typical.empty()) {
		return tr::lng_oblivion_online_journal_typical_none(tr::now);
	}
	auto list = QStringList();
	for (const auto &[from, till] : summary.typical) {
		list.push_back(FormatHour(from)
			+ u"–"_q
			+ FormatHour((till > 24) ? (till - 24) : till));
	}
	return tr::lng_oblivion_online_journal_typical(
		tr::now,
		lt_hours,
		list.join(u", "_q));
}

// A dash must not start a line: the space before it doesn't break.
[[nodiscard]] QString KeepDashOnLine(QString text) {
	text.replace(u" \u2014"_q, u"\u00A0\u2014"_q);
	return text;
}

[[nodiscard]] style::font AxisFont() {
	return style::font(Scaled(kAxisFontSize), 0, st::normalFont->family());
}

// Clipping by a rounded path is not antialiased, so the bars are painted
// as plain rectangles and their corners are covered with the background.
void PaintCorners(QPainter &p, QRectF rect, float64 radius) {
	auto path = QPainterPath();
	path.addRect(rect);
	path.addRoundedRect(rect, radius, radius);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::boxBg);
	p.drawPath(path);
}

void PaintHatch(QPainter &p, QRectF rect, QRectF bar, const QColor &color) {
	p.save();
	p.setClipRect(rect, Qt::IntersectClip);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(QPen(color, 1.));
	const auto step = float64(Scaled(kHatchStep));
	for (auto x = bar.left() - bar.height(); x < bar.right(); x += step) {
		p.drawLine(
			QPointF(x, bar.bottom()),
			QPointF(x + bar.height(), bar.top()));
	}
	p.restore();
}

class Stats final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	struct Cell {
		QString value;
		QString caption;
	};
	void setCells(std::vector<Cell> cells);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	std::vector<Cell> _cells;

};

void Stats::setCells(std::vector<Cell> cells) {
	_cells = std::move(cells);
	update();
}

int Stats::resizeGetHeight(int newWidth) {
	return st::boxTitleFont->height + st::normalFont->height + Scaled(2);
}

void Stats::paintEvent(QPaintEvent *e) {
	if (_cells.empty()) {
		return;
	}
	auto p = Painter(this);
	const auto count = int(_cells.size());
	const auto skip = Scaled(kColumnSkip);
	for (auto i = 0; i != count; ++i) {
		const auto left = width() * i / count;
		const auto available = (width() * (i + 1) / count) - left - skip;
		if (available <= 0) {
			continue;
		}
		p.setFont(st::boxTitleFont);
		p.setPen(st::boxTextFg);
		p.drawTextLeft(
			left,
			0,
			width(),
			st::boxTitleFont->elided(_cells[i].value, available));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(
			left,
			st::boxTitleFont->height + Scaled(2),
			width(),
			st::normalFont->elided(_cells[i].caption, available));
	}
}

class Timeline final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	Timeline(QWidget *parent, QTimeZone zone);

	void setData(std::vector<Day> days, Summary summary, TimeId now);

	// A few rows are spread to fill this height, so the box keeps its
	// size when the number of days is switched.
	void setMinHeight(int height);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect barRect(int row) const;
	[[nodiscard]] QString tipAt(QPoint point) const;
	void updateTip(const QString &tip);
	void paintHistogram(Painter &p) const;
	void paintAxis(Painter &p) const;
	void paintRow(Painter &p, int row) const;
	void paintBar(Painter &p, const Day &day, QRectF bar) const;
	void paintLegend(Painter &p) const;

	const QTimeZone _zone;
	std::vector<Day> _days;
	Summary _summary;
	TimeId _now = 0;
	QStringList _labels;
	QStringList _totals;
	int _labelWidth = 0;
	int _totalWidth = 0;
	int _barLeft = 0;
	int _barWidth = 0;
	int _rowsTop = 0;
	int _legendTop = 0;
	int _minHeight = 0;
	int _rowHeight = 0;
	int _barHeight = 0;
	QString _tip;
	QPoint _tipLocal;
	QPoint _tipPoint;

};

Timeline::Timeline(QWidget *parent, QTimeZone zone)
: RpWidget(parent)
, _zone(std::move(zone))
, _rowHeight(Scaled(kRowHeight))
, _barHeight(Scaled(kBarHeight)) {
	setMouseTracking(true);
}

void Timeline::setMinHeight(int height) {
	if (_minHeight == height) {
		return;
	}
	_minHeight = height;
	resizeToWidth(width());
	update();
}

void Timeline::setData(std::vector<Day> days, Summary summary, TimeId now) {
	_days = std::move(days);
	_summary = std::move(summary);
	_now = now;
	_labels.clear();
	_totals.clear();
	const auto today = _days.empty() ? QDate() : _days.front().date;
	const auto &font = st::normalFont;
	_labelWidth = font->width(
		tr::lng_oblivion_online_journal_by_hours(tr::now));
	_totalWidth = 0;
	for (const auto &day : _days) {
		_labels.push_back(FormatDay(day.date, today));
		_totals.push_back(FormatDuration(day.total));
		_labelWidth = std::max(_labelWidth, font->width(_labels.back()));
		_totalWidth = std::max(_totalWidth, font->width(_totals.back()));
	}
	resizeToWidth(width());
	update();
	if (!_tip.isEmpty()) {
		updateTip(tipAt(_tipLocal));
	}
}

int Timeline::resizeGetHeight(int newWidth) {
	const auto skip = Scaled(kColumnSkip);
	_barLeft = _labelWidth + skip;
	_barWidth = std::max(
		newWidth - _barLeft - skip - _totalWidth,
		Scaled(kHistogramHeight));
	_rowsTop = Scaled(kHistogramHeight) + Scaled(kAxisHeight);
	const auto rows = int(_days.size());
	const auto fixed = _rowsTop + Scaled(kLegendSkip) + Scaled(kLegendHeight);
	const auto extra = rows
		? std::clamp(
			(_minHeight - fixed) / rows - Scaled(kRowHeight),
			0,
			Scaled(kRowStretch))
		: 0;
	_rowHeight = Scaled(kRowHeight) + extra;
	_barHeight = Scaled(kBarHeight) + extra / 3;
	_legendTop = _rowsTop + rows * _rowHeight + Scaled(kLegendSkip);
	return _legendTop + Scaled(kLegendHeight);
}

QRect Timeline::barRect(int row) const {
	return QRect(
		_barLeft,
		_rowsTop + row * _rowHeight + (_rowHeight - _barHeight) / 2,
		_barWidth,
		_barHeight);
}

void Timeline::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	const auto height = _rowHeight;
	if (clip.y() < _rowsTop) {
		paintHistogram(p);
		paintAxis(p);
	}
	const auto count = int(_days.size());
	const auto line = st::lineWidth;
	const auto grid = anim::with_alpha(st::windowSubTextFg->c, 0.16);
	for (const auto hour : { 6, 12, 18 }) {
		p.fillRect(
			_barLeft + (_barWidth * hour / 24),
			_rowsTop,
			line,
			count * height,
			grid);
	}
	const auto from = std::clamp((clip.y() - _rowsTop) / height, 0, count);
	const auto till = std::clamp(
		(clip.y() + clip.height() - _rowsTop + height - 1) / height,
		0,
		count);
	for (auto row = from; row < till; ++row) {
		paintRow(p, row);
	}
	if (clip.y() + clip.height() > _legendTop) {
		paintLegend(p);
	}
}

void Timeline::paintHistogram(Painter &p) const {
	const auto height = Scaled(kHistogramHeight);
	const auto base = height - st::lineWidth;
	const auto top = *std::max_element(
		begin(_summary.hours),
		end(_summary.hours));
	const auto column = _barWidth / 24.;
	const auto gap = std::min(float64(st::lineWidth), column / 3.);
	const auto available = base - Scaled(4);
	const auto color = st::windowBgActive->c;
	for (auto hour = 0; hour != 24; ++hour) {
		const auto value = _summary.hours[hour];
		if (value <= 0 || top <= 0) {
			continue;
		}
		const auto size = std::max(
			available * (value / float64(top)),
			float64(Scaled(kMinSegment)));
		p.fillRect(
			QRectF(
				_barLeft + hour * column + gap / 2.,
				base - size,
				column - gap,
				size),
			color);
	}
	p.fillRect(
		_barLeft,
		base,
		_barWidth,
		st::lineWidth,
		anim::with_alpha(st::windowSubTextFg->c, 0.4));
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	p.drawTextLeft(
		0,
		base - st::normalFont->height,
		width(),
		tr::lng_oblivion_online_journal_by_hours(tr::now));
}

void Timeline::paintAxis(Painter &p) const {
	const auto font = AxisFont();
	const auto top = Scaled(kHistogramHeight) + Scaled(3);
	p.setFont(font);
	p.setPen(st::windowSubTextFg);
	for (const auto hour : { 0, 6, 12, 18, 24 }) {
		const auto text = QString::number(hour);
		const auto textWidth = font->width(text);
		const auto x = _barLeft + (_barWidth * hour / 24);
		const auto left = !hour
			? x
			: (hour == 24)
			? (x - textWidth)
			: (x - textWidth / 2);
		p.drawTextLeft(left, top, width(), text);
	}
}

void Timeline::paintRow(Painter &p, int row) const {
	const auto &day = _days[row];
	const auto height = _rowHeight;
	const auto top = _rowsTop + row * height;
	const auto textTop = top + (height - st::normalFont->height) / 2;
	p.setFont(st::normalFont);
	p.setPen(row ? st::windowSubTextFg : st::boxTextFg);
	p.drawTextLeft(0, textTop, width(), _labels[row]);
	p.setPen(day.total > 0 ? st::boxTextFg : st::windowSubTextFg);
	p.drawTextRight(0, textTop, width(), _totals[row]);
	paintBar(p, day, QRectF(barRect(row)));
}

void Timeline::paintBar(Painter &p, const Day &day, QRectF bar) const {
	const auto span = float64(std::max(day.till - day.from, 1));
	const auto x = [&](TimeId time) {
		return bar.x() + (time - day.from) * bar.width() / span;
	};
	const auto part = [&](float64 left, float64 right) {
		return QRectF(left, bar.y(), right - left, bar.height());
	};
	const auto sub = st::windowSubTextFg->c;
	const auto limit = std::clamp(_now, day.from, day.till);

	p.fillRect(bar, st::boxBg);
	p.fillRect(bar, anim::with_alpha(sub, 0.07));
	auto cursor = day.from;
	const auto known = anim::with_alpha(sub, 0.22);
	const auto hatch = anim::with_alpha(sub, 0.45);
	for (const auto &period : day.unknown) {
		if (period.from > cursor) {
			p.fillRect(part(x(cursor), x(period.from)), known);
		}
		PaintHatch(p, part(x(period.from), x(period.till)), bar, hatch);
		cursor = period.till;
	}
	if (limit > cursor) {
		p.fillRect(part(x(cursor), x(limit)), known);
	}
	const auto minimal = float64(Scaled(kMinSegment));
	const auto active = st::windowBgActive->c;
	for (const auto &interval : day.online) {
		auto left = x(interval.from);
		auto right = x(interval.till);
		if (right - left < minimal) {
			left = std::clamp(
				(left + right - minimal) / 2.,
				bar.left(),
				bar.right() - minimal);
			right = left + minimal;
		}
		p.fillRect(part(left, right), active);
	}
	PaintCorners(p, bar, Scaled(kBarRadius));
}

void Timeline::paintLegend(Painter &p) const {
	const auto swatch = QSize(
		Scaled(kLegendSwatchWidth),
		Scaled(kLegendSwatchHeight));
	const auto sub = st::windowSubTextFg->c;
	const auto font = AxisFont();
	const auto top = _legendTop + (Scaled(kLegendHeight) - swatch.height()) / 2;
	const auto textTop = _legendTop
		+ (Scaled(kLegendHeight) - font->height) / 2;
	const auto radius = float64(Scaled(kBarRadius)) / 2.;
	auto left = 0;
	const auto add = [&](const QString &text, int type) {
		const auto rect = QRectF(left, top, swatch.width(), swatch.height());
		if (type == 0) {
			p.fillRect(rect, st::windowBgActive->c);
		} else if (type == 1) {
			p.fillRect(rect, anim::with_alpha(sub, 0.29));
		} else {
			p.fillRect(rect, anim::with_alpha(sub, 0.07));
			PaintHatch(p, rect, rect, anim::with_alpha(sub, 0.45));
		}
		PaintCorners(p, rect, radius);
		left += swatch.width() + Scaled(6);
		p.setFont(font);
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(left, textTop, width(), text);
		left += font->width(text) + Scaled(16);
	};
	add(tr::lng_oblivion_online_journal_legend_online(tr::now), 0);
	add(tr::lng_oblivion_online_journal_legend_offline(tr::now), 1);
	add(tr::lng_oblivion_online_journal_legend_unknown(tr::now), 2);
}

QString Timeline::tipAt(QPoint point) const {
	const auto height = _rowHeight;
	if (point.x() < _barLeft
		|| point.x() >= _barLeft + _barWidth
		|| point.y() < 0) {
		return QString();
	} else if (point.y() < Scaled(kHistogramHeight)) {
		const auto hour = std::clamp(
			(point.x() - _barLeft) * 24 / _barWidth,
			0,
			23);
		const auto value = _summary.hours[hour];
		return (value > 0)
			? (FormatHour(hour)
				+ u"–"_q
				+ FormatHour(hour + 1)
				+ u" · "_q
				+ FormatDuration(value))
			: QString();
	} else if (point.y() < _rowsTop) {
		return QString();
	}
	const auto row = (point.y() - _rowsTop) / height;
	if (row >= int(_days.size())) {
		return QString();
	}
	const auto &day = _days[row];
	const auto span = float64(day.till - day.from);
	const auto time = day.from
		+ TimeId(base::SafeRound((point.x() - _barLeft) * span / _barWidth));
	const auto tolerance = TimeId(
		base::SafeRound(Scaled(kTooltipTolerance) * span / _barWidth));
	auto found = (const Interval*)nullptr;
	auto distance = tolerance + 1;
	for (const auto &interval : day.online) {
		const auto now = (time < interval.from)
			? (interval.from - time)
			: (time > interval.till)
			? (time - interval.till)
			: 0;
		if (now < distance) {
			distance = now;
			found = &interval;
		}
	}
	if (found) {
		const auto from = FormatTime(found->from, _zone);
		if (found->flags & kOngoing) {
			return tr::lng_oblivion_online_tip_since(tr::now, lt_time, from);
		} else if (found->till == found->from) {
			return tr::lng_oblivion_online_tip_seen(tr::now, lt_time, from);
		}
		return from
			+ u" – "_q
			+ FormatTime(found->till, _zone)
			+ u" · "_q
			+ FormatDuration(found->till - found->from);
	}
	for (const auto &period : day.unknown) {
		if (time >= period.from && time < period.till) {
			return tr::lng_oblivion_online_tip_unknown(tr::now);
		}
	}
	if (time >= _now) {
		return QString();
	}
	return tr::lng_oblivion_online_tip_offline(
		tr::now,
		lt_time,
		FormatTime(time, _zone));
}

void Timeline::updateTip(const QString &tip) {
	if (_tip == tip) {
		return;
	}
	_tip = tip;
	if (_tip.isEmpty()) {
		Ui::Tooltip::Hide();
	} else {
		Ui::Tooltip::Show(kTooltipDelay, this);
	}
}

void Timeline::mouseMoveEvent(QMouseEvent *e) {
	_tipLocal = e->pos();
	_tipPoint = e->globalPosition().toPoint();
	updateTip(tipAt(_tipLocal));
}

void Timeline::leaveEventHook(QEvent *e) {
	updateTip(QString());
}

QString Timeline::tooltipText() const {
	return _tip;
}

QPoint Timeline::tooltipPos() const {
	return _tipPoint;
}

bool Timeline::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

struct JournalArgs {
	std::shared_ptr<Ui::Show> show;
	QString name;
	rpl::producer<JournalData> data;
	rpl::producer<bool> notify;
	Fn<void(bool)> setNotify;
	Fn<void()> clear;
	QTimeZone zone = QTimeZone::systemTimeZone();
	int days = kWeekDays;
};

void JournalBox(not_null<Ui::GenericBox*> box, JournalArgs &&args) {
	struct State {
		JournalData data;
		int days = kWeekDays;
		rpl::variable<TextWithEntities> status;
		rpl::variable<QString> note;
		rpl::variable<QString> typical;
		rpl::variable<QString> empty;
		Fn<void()> refresh;
	};
	const auto state = box->lifetime().make_state<State>();
	state->days = args.days;
	const auto zone = args.zone;
	const auto show = args.show;
	const auto name = args.name;

	box->setWidth(Scaled(kJournalWidth));
	box->setMaxHeight(Scaled(kJournalMaxHeight));
	box->setTitle(tr::lng_oblivion_online_menu_journal());

	// The name, the switch of days and the numbers stay in sight above the
	// scrolled days, the box draws a line under the pinned content.
	const auto content = box->verticalLayout();
	const auto top = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));
	const auto &padding = st::boxRowPadding;
	top->add(
		object_ptr<Ui::FlatLabel>(
			top,
			rpl::single(tr::bold(name)),
			st::boxLabel),
		padding);
	top->add(
		object_ptr<Ui::FlatLabel>(
			top,
			state->status.value(),
			st::defaultSubTextLabel),
		padding + QMargins(0, Scaled(2), 0, 0));
	// The note may take several lines, and a label of a style without
	// minWidth (like st::defaultSubTextLabel) is always one line high: the
	// rest of the text would be cut off.
	const auto note = top->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			top,
			object_ptr<Ui::FlatLabel>(
				top,
				state->note.value(),
				st::boxDividerLabel),
			padding + QMargins(0, Scaled(2), 0, 0)),
		style::margins());

	const auto header = top->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			top,
			object_ptr<Ui::VerticalLayout>(top)),
		style::margins());
	const auto inner = header->entity();
	const auto slider = inner->add(
		object_ptr<Ui::SettingsSlider>(inner, st::settingsSlider),
		padding + QMargins(0, Scaled(10), 0, 0));
	slider->setSections(std::vector<QString>{
		tr::lng_oblivion_online_journal_week(tr::now),
		tr::lng_oblivion_online_journal_month(tr::now),
	});
	slider->setActiveSectionFast((state->days == kMonthDays) ? 1 : 0);
	const auto stats = inner->add(
		object_ptr<Stats>(inner),
		padding + QMargins(0, Scaled(14), 0, 0));
	inner->add(
		object_ptr<Ui::FlatLabel>(
			inner,
			state->typical.value(),
			st::defaultSubTextLabel),
		padding + QMargins(0, Scaled(10), 0, 0));
	Ui::AddSkip(top, Scaled(kHeaderSkip));

	const auto timelinePadding = padding
		+ QMargins(0, Scaled(kHeaderSkip), 0, Scaled(kHeaderSkip));
	const auto chart = content->add(
		object_ptr<Ui::SlideWrap<Timeline>>(
			content,
			object_ptr<Timeline>(content, zone),
			timelinePadding),
		style::margins());
	const auto timeline = chart->entity();

	const auto empty = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				state->empty.value(),
				st::membersAbout),
			padding + QMargins(0, Scaled(28), 0, Scaled(28))),
		style::margins(),
		style::al_top);

	auto bottomHeight = rpl::producer<int>(rpl::single(0));
	if (args.setNotify) {
		// Pinned under the scrolled part, so it stays in sight in the long
		// 30 days view; the box draws a line above the pinned content. The
		// text and the toggle are in one column with the rest of the box.
		const auto toggleSt = box->lifetime().make_state<
			style::SettingsButton
		>(st::settingsButtonNoIcon);
		toggleSt->padding.setLeft(padding.left());
		toggleSt->toggleSkip = padding.right() - (toggleSt->toggle.border / 2);
		const auto bottom = box->setPinnedToBottomContent(
			object_ptr<Ui::VerticalLayout>(box));
		Ui::AddSkip(bottom);
		const auto button = bottom->add(
			object_ptr<Ui::SettingsButton>(
				bottom,
				tr::lng_oblivion_online_menu_notify_on(),
				*toggleSt));
		button->toggleOn(std::move(args.notify));
		button->toggledChanges(
		) | rpl::on_next(args.setNotify, button->lifetime());
		bottomHeight = bottom->heightValue();
	}

	// With the journal shown the box has one height for 7 and for 30 days:
	// the few rows of a week take all the place between the pinned parts.
	rpl::combine(
		box->heightValue(),
		top->heightValue(),
		std::move(bottomHeight)
	) | rpl::on_next([=](int outer, int above, int below) {
		const auto margins = timelinePadding.top() + timelinePadding.bottom();
		timeline->setMinHeight(outer - above - below - margins);
	}, timeline->lifetime());

	state->refresh = [=] {
		const auto &data = state->data;
		const auto none = data.intervals.empty();
		box->setMinHeight(none ? 0 : Scaled(kJournalMaxHeight));
		// The same accent as in the profile and in the lists of chats.
		state->status = data.online
			? Ui::Text::Colorized(data.status)
			: TextWithEntities{ .text = data.status };
		state->empty = !data.recording
			? tr::lng_oblivion_online_journal_disabled(tr::now)
			: data.hidden
			? tr::lng_oblivion_online_journal_empty_hidden(tr::now)
			: tr::lng_oblivion_online_journal_empty(tr::now);
		state->note = !data.recording
			? tr::lng_oblivion_online_journal_disabled(tr::now)
			: data.hidden
			? tr::lng_oblivion_online_journal_hidden_note(tr::now)
			: KeepDashOnLine(
				tr::lng_oblivion_online_journal_partial_note(tr::now));
		note->toggle(
			!none && (data.hidden || !data.recording || !data.listed),
			anim::type::instant);
		header->toggle(!none, anim::type::instant);
		chart->toggle(!none, anim::type::instant);
		empty->toggle(none, anim::type::instant);
		if (none) {
			return;
		}
		auto days = SplitByDays(
			data.intervals,
			data.watched,
			data.now,
			state->days,
			zone);
		auto summary = Summarize(days, data.now);
		state->typical = FormatTypical(summary);
		stats->setCells({
			{
				FormatDuration(summary.total),
				tr::lng_oblivion_online_journal_total(tr::now),
			},
			{
				FormatDuration(summary.average),
				tr::lng_oblivion_online_journal_average(tr::now),
			},
			{
				QString::number(summary.visits),
				tr::lng_oblivion_online_journal_visits(tr::now),
			},
		});
		timeline->setData(std::move(days), std::move(summary), data.now);
	};

	std::move(
		args.data
	) | rpl::on_next([=](JournalData &&data) {
		state->data = std::move(data);
		state->refresh();
	}, box->lifetime());

	slider->sectionActivated(
	) | rpl::on_next([=](int index) {
		state->days = index ? kMonthDays : kWeekDays;
		state->refresh();
	}, slider->lifetime());

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	if (const auto clear = args.clear) {
		box->addLeftButton(tr::lng_oblivion_online_journal_clear(), [=] {
			show->showBox(Ui::MakeConfirmBox({
				.text = tr::lng_oblivion_online_journal_clear_sure(
					tr::now,
					lt_user,
					name),
				.confirmed = [=](Fn<void()> close) {
					clear();
					close();
				},
				.confirmText = tr::lng_oblivion_online_journal_clear(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}, st::attentionBoxButton);
	}
}

using UserpicPainter = Fn<void(
	Painter &p,
	int x,
	int y,
	int outerWidth,
	int size)>;

struct NotifyEntry {
	uint64 id = 0;
	QString name;
	QString status;
	bool online = false;
	bool known = true;
	UserpicPainter paintUserpic;
};

[[nodiscard]] bool SameEntries(
		const std::vector<NotifyEntry> &a,
		const std::vector<NotifyEntry> &b) {
	return ranges::equal(a, b, [](const NotifyEntry &a, const NotifyEntry &b) {
		return (a.id == b.id)
			&& (a.name == b.name)
			&& (a.status == b.status)
			&& (a.online == b.online)
			&& (a.known == b.known);
	});
}

struct NotifyListArgs {
	std::shared_ptr<Ui::Show> show;
	rpl::producer<std::vector<NotifyEntry>> entries;
	rpl::producer<> repaint;
	Fn<void()> add;
	Fn<void(uint64)> remove;
	Fn<void(uint64)> open;
};

class NotifyRow final : public Ui::RippleButton {
public:
	NotifyRow(QWidget *parent, NotifyEntry entry);

	[[nodiscard]] rpl::producer<> removeRequests() const {
		return _remove->clicks() | rpl::to_empty;
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	const style::PeerListItem &_st;
	const NotifyEntry _entry;
	const not_null<Ui::IconButton*> _remove;

};

NotifyRow::NotifyRow(QWidget *parent, NotifyEntry entry)
: RippleButton(parent, st::defaultRippleAnimation)
, _st(st::defaultPeerListItem)
, _entry(std::move(entry))
, _remove(Ui::CreateChild<Ui::IconButton>(this, st::stickersRemove)) {
	setAccessibleName(_entry.name);
	resize(width(), _st.height);
}

int NotifyRow::resizeGetHeight(int newWidth) {
	_remove->moveToRight(
		std::max(st::boxRowPadding.right() - _remove->width() / 2, 0),
		(_st.height - _remove->height()) / 2,
		newWidth);
	return _st.height;
}

void NotifyRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto over = isOver() || isDown();
	p.fillRect(e->rect(), over ? st::windowBgOver : st::windowBg);
	paintRipple(p, 0, 0);

	const auto shift = st::boxRowPadding.left() - _st.photoPosition.x();
	if (_entry.paintUserpic) {
		_entry.paintUserpic(
			p,
			shift + _st.photoPosition.x(),
			_st.photoPosition.y(),
			width(),
			_st.photoSize);
	}
	const auto left = shift + _st.namePosition.x();
	const auto available = _remove->x() - left - st::boxLittleSkip;
	if (available <= 0) {
		return;
	}
	p.setFont(st::semiboldFont);
	p.setPen(_entry.known ? st::contactsNameFg : st::windowSubTextFg);
	p.drawTextLeft(
		left,
		_st.namePosition.y(),
		width(),
		st::semiboldFont->elided(_entry.name, available));
	p.setFont(st::normalFont);
	p.setPen(_entry.online
		? st::contactsStatusFgOnline
		: over
		? st::windowSubTextFgOver
		: st::contactsStatusFg);
	p.drawTextLeft(
		left,
		_st.statusPosition.y(),
		width(),
		st::normalFont->elided(_entry.status, available));
}

QImage NotifyRow::prepareRippleMask() const {
	return Ui::RippleAnimation::RectMask(size());
}

void NotifyListBox(not_null<Ui::GenericBox*> box, NotifyListArgs &&args) {
	struct State {
		std::vector<NotifyEntry> entries;
		std::vector<NotifyEntry> pending;
		std::vector<not_null<NotifyRow*>> rows;
		bool built = false;
		bool scheduled = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto remove = args.remove;
	const auto open = args.open;

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_online_notify());

	const auto content = box->verticalLayout();
	if (const auto add = args.add) {
		// The round icon is centered over the userpics of the rows and the
		// text starts where their names do (see NotifyRow::paintEvent).
		const auto &row = st::defaultPeerListItem;
		const auto addSt = box->lifetime().make_state<
			style::SettingsButton
		>(st::settingsButtonActive);
		addSt->iconLeft = st::boxRowPadding.left()
			+ (row.photoSize - st::settingsIconAdd.width()) / 2;
		addSt->padding.setLeft(st::boxRowPadding.left()
			- row.photoPosition.x()
			+ row.namePosition.x());
		::Settings::AddButtonWithIcon(
			content,
			tr::lng_oblivion_online_list_add(),
			*addSt,
			{
				&st::settingsIconAdd,
				::Settings::IconType::Round,
				&st::windowBgActive,
			}
		)->setClickedCallback(add);
	}
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto empty = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				tr::lng_oblivion_online_list_empty(),
				st::membersAbout),
			st::boxRowPadding + style::margins(
				0,
				st::boxMediumSkip,
				0,
				st::boxMediumSkip)),
		style::margins(),
		style::al_top);
	Ui::AddSkip(content);
	const auto &aboutPadding = st::defaultBoxDividerLabelPadding;
	Ui::AddDividerText(
		content,
		tr::lng_oblivion_online_list_about(
		) | rpl::map([](const QString &text) {
			return KeepDashOnLine(text);
		}),
		style::margins(
			st::boxRowPadding.left(),
			aboutPadding.top(),
			st::boxRowPadding.right(),
			aboutPadding.bottom()));

	const auto rebuild = [=] {
		if (state->built && SameEntries(state->entries, state->pending)) {
			return;
		}
		state->built = true;
		state->entries = base::take(state->pending);
		state->rows.clear();
		list->clear();
		for (const auto &entry : state->entries) {
			const auto id = entry.id;
			const auto known = entry.known;
			const auto row = list->add(object_ptr<NotifyRow>(list, entry));
			row->setClickedCallback([=] {
				if (known && open) {
					open(id);
				}
			});
			row->removeRequests() | rpl::on_next([=] {
				if (remove) {
					remove(id);
				}
			}, row->lifetime());
			state->rows.push_back(row);
		}
		empty->toggle(state->entries.empty(), anim::type::instant);
	};

	// The list may change from a click on a row (its remove button), so
	// all the rebuilds except the first one are postponed: the row is not
	// destroyed while its click is still being handled.
	std::move(
		args.entries
	) | rpl::on_next([=](std::vector<NotifyEntry> &&entries) {
		state->pending = std::move(entries);
		if (!state->built) {
			rebuild();
		} else if (!state->scheduled) {
			state->scheduled = true;
			Ui::PostponeCall(box, [=] {
				state->scheduled = false;
				rebuild();
			});
		}
	}, box->lifetime());

	if (args.repaint) {
		std::move(
			args.repaint
		) | rpl::on_next([=] {
			for (const auto &row : state->rows) {
				row->update();
			}
		}, box->lifetime());
	}

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

using UserpicViews = base::flat_map<uint64, Ui::PeerUserpicView>;

[[nodiscard]] UserpicPainter EmptyUserpicPainter(
		uint64 id,
		const QString &name) {
	const auto userpic = std::make_shared<Ui::EmptyUserpic>(
		Ui::EmptyUserpic::UserpicColor(Ui::EmptyUserpic::ColorIndex(id)),
		name);
	return [=](Painter &p, int x, int y, int outerWidth, int size) {
		userpic->paintCircle(p, x, y, outerWidth, size);
	};
}

[[nodiscard]] std::vector<NotifyEntry> CollectEntries(
		not_null<Main::Session*> session,
		std::shared_ptr<UserpicViews> views) {
	auto result = std::vector<NotifyEntry>();
	const auto now = base::unixtime::now();
	for (const auto id : Get().onlineNotifyList()) {
		const auto userId = peerToUser(PeerId(id));
		const auto user = session->data().userLoaded(userId);
		if (!user) {
			result.push_back({
				.id = id,
				.name = tr::lng_oblivion_online_list_unknown(
					tr::now,
					lt_value,
					QString::number(id)),
				.status = (KnownToOtherAccount(session, id)
					? tr::lng_oblivion_online_list_other_account
					: tr::lng_oblivion_online_list_unknown_status)(tr::now),
				.known = false,
				.paintUserpic = EmptyUserpicPainter(
					id,
					Ui::EmptyUserpic::InaccessibleName()),
			});
			continue;
		}
		result.push_back({
			.id = id,
			.name = user->name(),
			.status = Data::OnlineTextFull(user, now),
			.online = Data::IsUserOnline(user, now),
			.paintUserpic = [=](
					Painter &p,
					int x,
					int y,
					int outerWidth,
					int size) {
				user->paintUserpicLeft(
					p,
					(*views)[id],
					x,
					y,
					outerWidth,
					size);
			},
		});
	}
	ranges::stable_sort(result, [](
			const NotifyEntry &a,
			const NotifyEntry &b) {
		return (a.known != b.known)
			? a.known
			: (a.online != b.online)
			? a.online
			: (a.name.compare(b.name, Qt::CaseInsensitive) < 0);
	});
	return result;
}

class AddNotifyController final : public ChooseRecipientBoxController {
public:
	using ChooseRecipientBoxController::ChooseRecipientBoxController;

protected:
	void prepareViewHook() override {
		delegate()->peerListSetTitle(tr::lng_oblivion_online_list_choose());
	}

};

void ShowAddNotifyBox(not_null<Window::SessionController*> controller) {
	const auto weak = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
	auto callback = [=](not_null<Data::Thread*> thread) {
		Get().setOnlineNotify(thread->peer()->id.value, true);
		if (const auto strong = weak->get()) {
			strong->closeBox();
		}
	};
	auto filter = [](not_null<Data::Thread*> thread) {
		const auto user = thread->peer()->asUser();
		return user
			&& Trackable(user)
			&& !Get().isOnlineNotify(user->id.value);
	};
	auto init = [](not_null<PeerListBox*> box) {
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};
	auto box = Box<PeerListBox>(
		std::make_unique<AddNotifyController>(
			&controller->session(),
			std::move(callback),
			std::move(filter)),
		std::move(init));
	*weak = box.data();
	controller->show(std::move(box));
}

// Snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h):
// a fixed moment in a fixed time zone and generated visits, nothing is
// read or saved.

[[nodiscard]] QTimeZone SampleZone() {
	return QTimeZone::fromSecondsAheadOfUtc(3 * kHourSeconds);
}

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

[[nodiscard]] JournalData SampleJournal(bool filled) {
	const auto zone = SampleZone();
	const auto today = QDate(2026, 9, 30);
	const auto now = TimeId(
		QDateTime(today, QTime(17, 42), zone).toSecsSinceEpoch());
	auto result = JournalData{
		.now = now,
		.status = SampleText("в сети", "online"),
		.online = true,
	};
	if (!filled) {
		result.online = false;
		result.status = SampleText(
			"был(а) в сети вчера в 21:10",
			"last seen yesterday at 21:10");
		result.watched.push_back({ now - 5 * kHourSeconds, now });
		return result;
	}
	auto seed = uint32(20260930);
	const auto next = [&](int limit) {
		seed = seed * 1664525U + 1013904223U;
		return int((seed >> 8) % uint32(limit));
	};
	for (auto index = kMonthDays - 1; index >= 0; --index) {
		const auto start = DayStart(today.addDays(-index), zone);
		if (index == 12 || index == 13 || index == 23) {
			const auto seen = start + 21 * kHourSeconds + next(kHourSeconds);
			result.intervals.push_back({ seen, seen, kStartUnknown });
			continue;
		}
		const auto from = start + 7 * kHourSeconds + next(5400);
		const auto till = index
			? (start + 22 * kHourSeconds + next(3 * kHourSeconds))
			: now;
		result.watched.push_back({ from, till });
		const auto add = [&](int hour, int spread, int length) {
			const auto begin = start
				+ hour * kHourSeconds
				+ next(spread * 60);
			const auto end = begin + 60 + next(length * 60);
			const auto after = result.intervals.empty()
				? 0
				: result.intervals.back().till;
			if (begin > after + 60 && begin >= from && end <= till) {
				result.intervals.push_back({ begin, end, 0 });
			}
		};
		add(8, 50, 12);
		add(9, 40, 25);
		if (next(3)) {
			add(11, 30, 6);
		}
		add(13, 40, 30);
		if (next(2)) {
			add(15, 50, 8);
		}
		add(18, 30, 20);
		add(19, 45, 50);
		add(21, 30, 70);
		if (!next(3)) {
			add(23, 20, 40);
		}
	}
	const auto last = result.intervals.back().till;
	result.intervals.push_back({
		.from = std::min(std::max(now - 9 * 60, last + 60), now),
		.till = now,
		.flags = kOngoing,
	});
	return result;
}

// With "hidden" the user hides the last seen time: not online right now,
// the note about the incomplete journal is shown under the status. The
// users not "listed" for notifications have a note of their own.
[[nodiscard]] object_ptr<Ui::BoxContent> SampleJournalBox(
		std::shared_ptr<Ui::Show> show,
		bool filled,
		int days,
		bool hidden = false,
		bool listed = true) {
	auto data = SampleJournal(filled);
	data.listed = filled && !hidden && listed;
	if (hidden) {
		data.hidden = true;
		data.online = false;
		data.status = SampleText("был(а) недавно", "last seen recently");
		if (!data.intervals.empty()
			&& (data.intervals.back().flags & kOngoing)) {
			data.intervals.pop_back();
		}
	}
	const auto notify = data.listed;
	return Box(JournalBox, JournalArgs{
		.show = std::move(show),
		.name = SampleText("Аня Смирнова", "Anna Smirnova"),
		.data = rpl::single(std::move(data)),
		.notify = rpl::single(notify),
		.setNotify = [](bool) {},
		.clear = [] {},
		.zone = SampleZone(),
		.days = days,
	});
}

[[nodiscard]] std::vector<NotifyEntry> SampleEntries() {
	struct Sample {
		const char *ru = nullptr;
		const char *en = nullptr;
		const char *statusRu = nullptr;
		const char *statusEn = nullptr;
		bool online = false;
	};
	const auto samples = std::vector<Sample>{
		{ "Аня Смирнова", "Anna Smirnova", "в сети", "online", true },
		{
			"Борис Ковалёв",
			"Boris Kovalev",
			"был(а) в сети 5 минут назад",
			"last seen 5 minutes ago",
		},
		{
			"Вера Орлова",
			"Vera Orlova",
			"был(а) в сети сегодня в 09:14",
			"last seen today at 09:14",
		},
		{
			"Глеб Назаров",
			"Gleb Nazarov",
			"был(а) недавно",
			"last seen recently",
		},
	};
	auto result = std::vector<NotifyEntry>();
	auto id = uint64(1000001);
	for (const auto &sample : samples) {
		const auto name = SampleText(sample.ru, sample.en);
		result.push_back({
			.id = id,
			.name = name,
			.status = SampleText(sample.statusRu, sample.statusEn),
			.online = sample.online,
			.paintUserpic = EmptyUserpicPainter(id, name),
		});
		id += 7;
	}
	const auto unknown = [&](uint64 shown, const QString &status) {
		result.push_back({
			.id = id,
			.name = tr::lng_oblivion_online_list_unknown(
				tr::now,
				lt_value,
				QString::number(shown)),
			.status = status,
			.known = false,
			.paintUserpic = EmptyUserpicPainter(
				id,
				Ui::EmptyUserpic::InaccessibleName()),
		});
		id += 7;
	};
	unknown(
		5098765432ULL,
		tr::lng_oblivion_online_list_other_account(tr::now));
	unknown(
		5012345678ULL,
		tr::lng_oblivion_online_list_unknown_status(tr::now));
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto size = QSize(Scaled(kJournalWidth + 80), 0);
	RegisterBoxScene(u"online_journal_week"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleJournalBox(std::move(show), true, kWeekDays);
	});
	RegisterBoxScene(u"online_journal_month"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleJournalBox(std::move(show), true, kMonthDays);
	});
	RegisterBoxScene(u"online_journal_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleJournalBox(std::move(show), false, kWeekDays);
	});
	RegisterBoxScene(u"online_journal_hidden"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleJournalBox(std::move(show), true, kWeekDays, true);
	});
	RegisterBoxScene(u"online_journal_partial"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleJournalBox(
			std::move(show),
			true,
			kWeekDays,
			false,
			false);
	});
	RegisterBoxScene(u"online_notify_list"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(NotifyListBox, NotifyListArgs{
			.show = std::move(show),
			.entries = rpl::single(SampleEntries()),
			.add = [] {},
		});
	});
	RegisterBoxScene(u"online_notify_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(NotifyListBox, NotifyListArgs{
			.show = std::move(show),
			.entries = rpl::single(std::vector<NotifyEntry>()),
			.add = [] {},
		});
	});
});

// Self-test begin.

[[nodiscard]] bool RunModelSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	using Intervals = std::vector<Interval>;
	using Periods = std::vector<Period>;
	const auto base = TimeId(1700000000);

	// Building intervals from status updates.
	{
		auto journal = Journal();
		check(!journal.apply(1, base + 300, base), u"not started: ignored"_q);
		journal.start(base);
		check(
			journal.apply(1, base + 300, base + 5) && journal.online(1),
			u"online at the start is a visit"_q);
		check(
			!journal.apply(2, base - 1000, base + 6)
				&& (journal.intervals(2, base + 6) == Intervals{
					{ base - 1000, base - 1000, kStartUnknown },
				}),
			u"last seen of an unwatched visit is a zero interval"_q);
		check(
			!journal.apply(2, base - 1000, base + 7)
				&& !journal.apply(2, base - 2000, base + 8)
				&& (journal.intervals(2, base + 8).size() == 1),
			u"repeated and older last seen are ignored"_q);
		check(
			journal.apply(2, base + 400, base + 100),
			u"offline -> online is reported"_q);
		check(
			!journal.apply(2, base + 700, base + 390),
			u"prolonged online is not a new visit"_q);
		check(
			journal.intervals(2, base + 500) == Intervals{
				{ base - 1000, base - 1000, kStartUnknown },
				{ base + 100, base + 500, kOngoing },
			},
			u"an open visit ends now"_q);
		journal.apply(2, base + 650, base + 655);
		check(
			!journal.online(2)
				&& (journal.intervals(2, base + 656).back()
					== Interval{ base + 100, base + 650, 0 }),
			u"offline closes at the exact last seen time"_q);
		check(
			journal.apply(2, base + 960, base + 660)
				&& (journal.intervals(2, base + 700).back()
					== Interval{ base + 100, base + 700, kOngoing })
				&& (journal.intervals(2, base + 700).size() == 2),
			u"a flap continues the same visit"_q);
		const auto nearest = journal.expire(base + 310);
		check(
			!journal.online(1)
				&& (nearest == base + 960)
				&& (journal.intervals(1, base + 310) == Intervals{
					{
						base + 5,
						base + 300,
						uchar(kStartUnknown | kEndGuessed),
					},
				}),
			u"an expired status closes at its expiration"_q);
		check(
			!journal.expire(base + 1000)
				&& (journal.intervals(2, base + 1000).back()
					== Interval{ base + 100, base + 960, kEndGuessed }),
			u"expire returns zero when nobody is online"_q);
		journal.apply(2, base + 900, base + 1200);
		check(
			journal.intervals(2, base + 1200).back()
				== Interval{ base + 100, base + 900, 0 },
			u"a late exact time fixes a guessed end"_q);
		journal.apply(2, base + 915, base + 1210);
		journal.apply(2, base + 2000, base + 2100);
		check(
			journal.intervals(2, base + 2100) == Intervals{
				{ base - 1000, base - 1000, kStartUnknown },
				{ base + 100, base + 915, 0 },
				{ base + 2000, base + 2000, kStartUnknown },
			},
			u"last seen: near extends, far adds a visit"_q);
		journal.apply(3, base + 2600, base + 2300);
		journal.apply(3, 0, base + 2400);
		check(
			journal.intervals(3, base + 2400) == Intervals{
				{ base + 2300, base + 2400, kEndGuessed },
			},
			u"a hidden status closes the visit now"_q);
		journal.apply(4, base + 3030, base + 3000);
		journal.apply(4, base + 2000, base + 3010);
		check(
			journal.intervals(4, base + 3010) == Intervals{
				{ base + 3000, base + 3000, 0 },
			},
			u"a stale last seen can't end a visit before its start"_q);
		check(journal.total() == 6, u"intervals are counted"_q);
	}

	// Gaps: sleep, stop and start, recording switched off.
	{
		auto journal = Journal();
		journal.start(base);
		journal.apply(1, base + 5000, base + 100);
		check(
			journal.alive(base + 130) && journal.alive(base + 160),
			u"heartbeats keep watching"_q);
		check(
			journal.watched(base + 200) == Periods{ { base, base + 200 } },
			u"the watched period grows"_q);
		check(
			journal.watched(base + 9000) == Periods{ { base, base + 160 } },
			u"a silent watcher is not trusted"_q);
		check(
			!journal.alive(base + 9000) && !journal.online(1),
			u"a long silence is a gap"_q);
		check(
			(journal.intervals(1, base + 9000) == Intervals{
				{ base + 100, base + 160, kEndUnknown },
			}) && (journal.watched(base + 9100) == Periods{
				{ base, base + 160 },
				{ base + 9000, base + 9100 },
			}),
			u"the gap cuts visits and watched periods"_q);
		check(
			journal.apply(1, base + 9500, base + 9001)
				&& (journal.intervals(1, base + 9002).back() == Interval{
					base + 9001,
					base + 9002,
					uchar(kOngoing | kStartUnknown),
				}),
			u"online right after a gap has an unknown start"_q);
		journal.alive(base + 9100);
		journal.stop(base + 9200);
		check(
			!journal.started()
				&& !journal.online(1)
				&& (journal.intervals(1, base + 9300) == Intervals{
					{ base + 100, base + 160, kEndUnknown },
					{
						base + 9001,
						base + 9200,
						uchar(kStartUnknown | kEndUnknown),
					},
				})
				&& (journal.watched(base + 9300) == Periods{
					{ base, base + 160 },
					{ base + 9000, base + 9200 },
				}),
			u"stop closes everything"_q);
		journal.start(base + 9210);
		journal.alive(base + 9300);
		journal.stop(base + 9400);
		check(
			journal.watched(base + 9500) == Periods{
				{ base, base + 160 },
				{ base + 9000, base + 9400 },
			},
			u"a quick restart continues the watched period"_q);

		auto napping = Journal();
		napping.start(base);
		napping.apply(1, base + 400, base + 100);
		napping.apply(2, base + 250, base + 110);
		napping.alive(base + 120);
		napping.pause();
		check(
			!napping.started()
				&& !napping.online(1)
				&& (napping.intervals(1, base + 200) == Intervals{
					{ base + 100, base + 120, kEndUnknown },
				}),
			u"a pause cuts the visits at the last heartbeat"_q);
		napping.start(base + 300);
		check(
			napping.online(1)
				&& !napping.online(2)
				&& !napping.apply(1, base + 700, base + 310)
				&& (napping.intervals(1, base + 350) == Intervals{
					{ base + 100, base + 350, kOngoing },
				})
				&& (napping.intervals(2, base + 350) == Intervals{
					{ base + 110, base + 120, kEndUnknown },
				}),
			u"a status that has not expired continues the visit"_q);
		check(
			napping.apply(2, base + 900, base + 360),
			u"an expired status is a new visit after the pause"_q);
		napping.alive(base + 370);
		napping.pause();
		napping.start(base + 500);
		napping.apply(1, base + 420, base + 510);
		check(
			!napping.online(1)
				&& napping.online(2)
				&& (napping.intervals(1, base + 510) == Intervals{
					{ base + 100, base + 420, 0 },
				})
				&& (napping.watched(base + 510) == Periods{
					{ base, base + 120 },
					{ base + 300, base + 370 },
					{ base + 500, base + 510 },
				}),
			u"the exact end may be inside of a pause"_q);
		napping.alive(base + 520);
		check(
			!napping.alive(base + 800)
				&& napping.online(2)
				&& (napping.intervals(2, base + 810).back() == Interval{
					base + 360,
					base + 810,
					kOngoing,
				}),
			u"a short sleep does not end a visit"_q);

		auto silent = Journal();
		silent.setRecording(false, base);
		silent.start(base);
		check(
			silent.apply(1, base + 300, base + 100)
				&& !silent.apply(1, base + 150, base + 200)
				&& silent.apply(1, base + 600, base + 250)
				&& silent.intervals(1, base + 260).empty()
				&& silent.watched(base + 260).empty()
				&& silent.empty(),
			u"not recording: transitions only"_q);
		silent.setRecording(true, base + 300);
		silent.apply(1, base + 320, base + 330);
		silent.setRecording(false, base + 400);
		check(
			(silent.intervals(1, base + 400) == Intervals{
				{ base + 300, base + 320, kStartUnknown },
			}) && (silent.watched(base + 500) == Periods{
				{ base + 300, base + 400 },
			}),
			u"recording switched on and off"_q);
	}

	// A visit that was cut: its status runs out without any update.
	{
		auto journal = Journal();
		journal.start(base);
		journal.apply(1, base + 400, base + 100);
		journal.alive(base + 120);
		journal.alive(base + 9000);
		check(
			!journal.apply(1, base + 400, base + 9001)
				&& !journal.apply(1, base + 400, base + 9100)
				&& (journal.intervals(1, base + 9100) == Intervals{
					{ base + 100, base + 120, kEndUnknown },
				}),
			u"an expired status of a cut visit is not one more visit"_q);
		journal.apply(1, base + 300, base + 9200);
		check(
			journal.intervals(1, base + 9200) == Intervals{
				{ base + 100, base + 300, 0 },
			},
			u"the exact end of a cut visit is applied to it"_q);

		auto later = Journal();
		later.start(base);
		later.apply(1, base + 400, base + 100);
		later.alive(base + 120);
		later.pause();
		later.start(base + 9000);
		later.apply(1, base + 5000, base + 9001);
		check(
			later.intervals(1, base + 9001) == Intervals{
				{ base + 100, base + 120, kEndUnknown },
				{ base + 5000, base + 5000, kStartUnknown },
			},
			u"a later last seen after a cut is another visit"_q);

		auto resumed = Journal();
		resumed.start(base);
		resumed.apply(1, base + 400, base + 100);
		resumed.alive(base + 120);
		resumed.pause();
		resumed.start(base + 200);
		resumed.expire(base + 500);
		resumed.apply(1, base + 400, base + 501);
		check(
			resumed.intervals(1, base + 501) == Intervals{
				{ base + 100, base + 400, 0 },
			},
			u"a continued visit ends as usual"_q);
	}

	// Not observing (this account is not shown as online) and the
	// periods covered by asking for the statuses.
	{
		auto journal = Journal();
		journal.start(base);
		journal.alive(base + 100);
		journal.setObserving(false, base + 100);
		check(
			journal.apply(1, base + 500, base + 200)
				&& (journal.watched(base + 300) == Periods{
					{ base, base + 100 },
				})
				&& (journal.intervals(1, base + 300) == Intervals{
					{
						base + 200,
						base + 300,
						uchar(kOngoing | kStartUnknown),
					},
				}),
			u"not observing: visits are kept, the time is not watched"_q);
		journal.alive(base + 200);
		journal.alive(base + 300);
		journal.alive(base + 400);
		journal.setObserving(true, base + 400);
		journal.cover(1, { base + 110, base + 250 });
		journal.cover(1, { base + 250, base + 300 });
		journal.cover(1, { base + 300, base + 300 });
		check(
			(journal.watched(base + 450) == Periods{
				{ base, base + 100 },
				{ base + 400, base + 450 },
			}) && (journal.known(1, base + 450) == Periods{
				{ base, base + 300 },
				{ base + 400, base + 450 },
			}) && (journal.known(2, base + 450)
				== journal.watched(base + 450)),
			u"covered periods fill the unwatched time of one user"_q);
		const auto bytes = journal.serialize(base + 450);
		auto loaded = Journal();
		check(
			loaded.parse(bytes)
				&& (loaded.known(1, base + 450)
					== journal.known(1, base + 450))
				&& (loaded.known(2, base + 450)
					== journal.watched(base + 450))
				&& (loaded.serialize(base + 450) == bytes),
			u"covered periods round-trip"_q);
		const auto cut = bytes.mid(
			kFileHeader,
			bytes.size() - kFileHeader - kFileFooter - 2);
		auto damaged = bytes.left(kFileHeader) + cut;
		WriteUint32(damaged, Checksum(
			reinterpret_cast<const uchar*>(cut.constData()),
			cut.size()));
		check(
			!loaded.parse(damaged) && loaded.empty(),
			u"cut off covered periods are rejected"_q);
		journal.forget(1, base + 450);
		check(
			journal.known(1, base + 450) == journal.watched(base + 450),
			u"forgetting a user drops the covered periods"_q);
	}

	// Asking for the statuses: who asks, about whom, when.
	{
		using Candidates = std::vector<PollCandidate>;
		check(
			(PollOwner(Candidates{
				{ .account = 30, .active = false, .knows = true },
				{ .account = 20, .active = true, .knows = true },
				{ .account = 10, .active = false, .knows = true },
			}) == 20) && (PollOwner(Candidates{
				{ .account = 20, .active = true, .knows = true },
			}) == 20),
			u"poll: the active account asks about those it knows"_q);
		check(
			(PollOwner(Candidates{
				{ .account = 30, .active = false, .knows = true },
				{ .account = 20, .active = true, .knows = false },
				{ .account = 10, .active = false, .knows = false },
				{ .account = 25, .active = false, .knows = true },
			}) == 25) && (PollOwner(Candidates{
				{ .account = 25, .active = false, .knows = true },
				{ .account = 10, .active = false, .knows = false },
				{ .account = 30, .active = false, .knows = true },
				{ .account = 20, .active = true, .knows = false },
			}) == 25),
			u"poll: otherwise the first account in the order of ids"_q);
		check(
			!PollOwner(Candidates())
				&& !PollOwner(Candidates{
					{ .account = 20, .active = true, .knows = false },
					{ .account = 10, .active = false, .knows = false },
				}),
			u"poll: nobody asks about a user that nobody knows"_q);
		check(
			(PollOwner(Candidates{
				{ .account = 20, .active = true, .knows = true },
				{ .account = 30, .knows = true, .sees = true },
				{ .account = 10, .knows = true },
			}) == 30) && (PollOwner(Candidates{
				{ .account = 20, .active = true, .knows = true },
				{ .account = 30, .knows = true, .sees = true },
				{ .account = 25, .knows = true, .sees = true },
				{ .account = 10, .knows = true },
			}) == 25),
			u"poll: an account that sees the status asks first"_q);
		check(
			(PollOwner(Candidates{
				{ .account = 30, .knows = true, .sees = true },
				{
					.account = 20,
					.active = true,
					.knows = true,
					.sees = true,
				},
				{ .account = 10, .knows = true, .sees = true },
			}) == 20) && (PollOwner(Candidates{
				{ .account = 30, .knows = true },
				{ .account = 20, .active = true, .knows = true },
				{ .account = 10, .knows = true },
			}) == 20) && (PollOwner(Candidates{
				{ .account = 30, .knows = true },
				{ .account = 20, .active = true },
				{ .account = 25, .knows = true },
			}) == 25),
			u"poll: the old choice if all see the status or nobody"_q);
		check(
			(PollOwner(Candidates{
				{ .account = 20, .active = true, .knows = true },
				{ .account = 10, .sees = true },
			}) == 20) && !PollOwner(Candidates{
				{ .account = 20, .active = true, .sees = true },
				{ .account = 0, .knows = true, .sees = true },
			}),
			u"poll: seeing the status of an unknown user is nothing"_q);

		auto ids = std::vector<uint64>();
		for (auto i = 1; i <= 250; ++i) {
			ids.push_back(uint64(i) * 10);
		}
		const auto few = std::vector<uint64>{ 5, 7, 9 };
		check(
			(PollChunk(few, 0, 100) == few)
				&& (PollChunk(few, 9, 100) == few)
				&& (PollChunk(ids, 0, 250) == ids)
				&& PollChunk({}, 7, 100).empty()
				&& PollChunk(few, 0, 0).empty()
				&& (PollChunks(0, 100) == 1)
				&& (PollChunks(100, 100) == 1)
				&& (PollChunks(101, 100) == 2)
				&& (PollChunks(250, 100) == 3),
			u"poll: a short list is asked in one request"_q);
		auto asked = std::map<uint64, int>();
		auto cursor = uint64(0);
		auto sizes = std::vector<int>();
		for (auto turn = 0; turn != 6; ++turn) {
			const auto chunk = PollChunk(ids, cursor, 100);
			sizes.push_back(int(chunk.size()));
			for (const auto id : chunk) {
				++asked[id];
			}
			cursor = chunk.empty() ? 0 : chunk.back();
		}
		check(
			(sizes == std::vector<int>{ 100, 100, 50, 100, 100, 50 })
				&& (asked.size() == ids.size())
				&& ranges::all_of(asked, [](const auto &pair) {
					return (pair.second == 2);
				}),
			u"poll: a long list is asked in turns, nobody is left out"_q);
		check(
			(PollChunk(ids, 995, 100).front() == 1000)
				&& (PollChunk(ids, 2400, 100).size() == 10)
				&& (PollChunk(ids, 2500, 100).front() == 10)
				&& (PollChunk(ids, 99999, 100).front() == 10),
			u"poll: the turns go on after a removed id and wrap"_q);

		check(
			(PollFloodSeconds(u"FLOOD_WAIT_35"_q) == 35)
				&& (PollFloodSeconds(u"FLOOD_PREMIUM_WAIT_7"_q) == 7)
				&& (PollFloodSeconds(u"FLOOD_WAIT_86400"_q) == 86400)
				&& (PollFloodSeconds(u"FLOOD_WAIT_99999999999"_q)
					== kPollFloodMax)
				&& (PollFloodSeconds(u"FLOOD_WAIT_soon"_q)
					== kPollFloodFallback)
				&& (PollFloodSeconds(u"FLOOD_WAIT_0"_q)
					== kPollFloodFallback)
				&& !PollFloodSeconds(u"USER_ID_INVALID"_q)
				&& !PollFloodSeconds(u"INTERNAL_500"_q)
				&& !PollFloodSeconds(QString()),
			u"poll: the wait is read from a flood error only"_q);
		auto flood = PollFlood();
		check(!flood.waiting(base), u"poll: no wait without an error"_q);
		flood.start(base, 3600);
		check(
			flood.waiting(base)
				&& flood.waiting(base + 1800)
				&& flood.waiting(base + 3600)
				&& flood.waiting(base + 3600 + kPollFloodMargin - 1)
				&& !flood.waiting(base + 3600 + kPollFloodMargin)
				&& !flood.waiting(base + 1800)
				&& !flood.till,
			u"poll: a flood wait lasts in full and then ends"_q);
		flood.start(base, 600);
		check(
			flood.waiting(base - 5000)
				&& (flood.till == base - 5000 + 600 + kPollFloodMargin)
				&& flood.waiting(flood.till - 1)
				&& !flood.waiting(flood.till),
			u"poll: a clock set back restarts the wait, not extends it"_q);

		const auto spread = [](crl::time from, crl::time jitter) {
			const auto randoms = {
				uint32(0),
				uint32(1),
				uint32(jitter),
				uint32(jitter + 1),
				uint32(123456789),
				uint32(0xFFFFFFFFU),
			};
			for (const auto random : randoms) {
				const auto delay = PollSpread(from, jitter, random);
				if (delay < from || delay > from + jitter) {
					return false;
				}
			}
			return (PollSpread(from, jitter, 0) == from)
				&& (PollSpread(from, jitter, uint32(jitter))
					== from + jitter);
		};
		check(
			spread(kPollInterval, kPollJitter)
				&& spread(kPollFirstDelay, kPollFirstJitter)
				&& spread(kPollSoonDelay, kPollSoonJitter)
				&& (PollSpread(1000, 0, 77) == 1000)
				&& (kPollInterval == 60000)
				&& (kPollInterval + kPollJitter == 90000)
				&& (kPollSoonDelay == 1000)
				&& (kPollSoonDelay + kPollSoonJitter == 10000),
			u"poll: the delays are random within their limits"_q);
		check(
			(PollSoonDelay(3000, kPollMinGap) == 3000)
				&& (PollSoonDelay(3000, kPollMinGap * 10) == 3000)
				&& (PollSoonDelay(3000, 0) == 3000 + kPollMinGap)
				&& (PollSoonDelay(3000, kPollMinGap - 4000) == 7000),
			u"poll: an early request waits for the smallest gap"_q);
		check(
			PollTooClose(0)
				&& PollTooClose(kPollApart - 1)
				&& !PollTooClose(kPollApart)
				&& !PollTooClose(kPollMinGap)
				&& (kPollApart >= 2000)
				&& spread(kPollApart, kPollApartJitter)
				&& (kPollApart + kPollApartJitter < kPollMinGap),
			u"poll: two accounts never ask in the same second"_q);
		check(
			PollEveryone(false, false, 0)
				&& PollEveryone(true, true, 0)
				&& !PollEveryone(true, false, 0)
				&& !PollEveryone(true, false, kPollShownInterval - 1)
				&& PollEveryone(true, false, kPollShownInterval)
				&& (kPollShownInterval == 300000),
			u"poll: shown as online asks about everyone rarely"_q);
		auto left = 0;
		auto ends = std::vector<bool>();
		for (auto request = 0; request != 3; ++request) {
			ends.push_back(PollRoundEnds(left, 1));
		}
		for (auto request = 0; request != 6; ++request) {
			ends.push_back(PollRoundEnds(left, 3));
		}
		ends.push_back(PollRoundEnds(left, 3));
		left = 0;
		ends.push_back(PollRoundEnds(left, 2));
		ends.push_back(PollRoundEnds(left, 2));
		ends.push_back(PollRoundEnds(left, 0));
		check(
			(ends == std::vector<bool>{
				true, true, true,
				false, false, true, false, false, true,
				false,
				false, true,
				true,
			}) && !left,
			u"poll: a long list is asked about in a round of requests"_q);

		const auto gap = kPollCoverGap;
		check(
			(PollCovered(base, 0, base + 90, gap)
				== Period{ base, base + 90 })
				&& (PollCovered(base, 0, base + gap, gap)
					== Period{ base, base + gap })
				&& (PollCovered(base, 0, base + gap + 1, gap) == Period())
				&& (PollCovered(0, 0, base, gap) == Period())
				&& (PollCovered(base, 0, base, gap) == Period())
				&& (PollCovered(base + 10, 0, base, gap) == Period()),
			u"poll: close answers cover the time between them"_q);
		check(
			(PollCovered(base - 400, base, base + 60, gap)
				== Period{ base, base + 60 })
				&& (PollCovered(0, base, base + 60, gap)
					== Period{ base, base + 60 })
				&& (PollCovered(base + 20, base, base + 60, gap)
					== Period{ base, base + 60 })
				&& (PollCovered(base - 50, base, base + 60, gap)
					== Period{ base - 50, base + 60 })
				&& (PollCovered(base + 20, base - 400, base + 60, gap)
					== Period{ base + 20, base + 60 })
				&& (PollCovered(0, base, base + gap + 1, gap) == Period())
				&& (PollCovered(base - 1, base, base + gap + 1, gap)
					== Period())
				&& (PollCovered(base, 0, base + 3 * gap, 3 * gap)
					== Period{ base, base + 3 * gap }),
			u"poll: the covered time goes on from the watched one"_q);
		check(
			(kPollMaxInterval == 97000)
				&& (crl::time(kPollCoverGap) * 1000 > 2 * kPollMaxInterval)
				&& (crl::time(kPollCoverGap) * 1000 < kPollShownInterval),
			u"poll: one missed tick makes no hole in the journal"_q);

		auto known = TimeId(0);
		check(
			PollArrival(known, true, false, base + 300, base)
				&& (known == base + 300)
				&& !PollArrival(known, false, false, base + 400, base + 100)
				&& (known == base + 400),
			u"poll: an arrival is reported, a longer visit is not"_q);
		check(
			!PollArrival(known, true, false, base + 500, base + 200)
				&& (known == base + 500),
			u"poll: known as online by another account is no arrival"_q);
		check(
			!PollArrival(known, false, true, base + 250, base + 260)
				&& !known
				&& PollArrival(known, true, false, base + 600, base + 300),
			u"poll: an arrival after a seen leaving is reported"_q);
		check(
			!PollArrival(known, false, false, base + 250, base + 310)
				&& (known == base + 600)
				&& PollArrival(known, true, false, base + 900, base + 601)
				&& PollArrival(known, true, false, base + 300, base - 5000)
				&& (known == base - 5000 + kStatusLife),
			u"poll: a run out status and a clock set back hide nothing"_q);
	}

	// Retention and size caps.
	{
		const auto now = base + 100 * kDaySeconds;
		const auto limits = Limits{
			.retention = 60 * kDaySeconds,
			.sleepGap = 200 * kDaySeconds,
			.startupQuiet = 0,
			.perUser = 5,
			.users = 3,
			.total = 12,
		};
		auto journal = Journal(limits);
		journal.start(now - 90 * kDaySeconds);
		const auto visit = [&](uint64 user, TimeId from, TimeId length) {
			journal.alive(from);
			journal.apply(user, from + length + 100, from);
			journal.alive(from + length);
			journal.apply(user, from + length, from + length);
		};
		visit(1, now - 80 * kDaySeconds, 60);
		visit(1, now - 61 * kDaySeconds, 2 * kDaySeconds);
		visit(1, now - 10 * kDaySeconds, 60);
		visit(2, now - 70 * kDaySeconds, 60);
		journal.cover(9, { now - 80 * kDaySeconds, now - 70 * kDaySeconds });
		journal.cover(9, { now - 4 * kDaySeconds, now - 3 * kDaySeconds });
		journal.stop(now - 5 * kDaySeconds);
		journal.trim(now);
		check(
			(journal.intervals(1, now) == Intervals{
				{ now - 61 * kDaySeconds, now - 59 * kDaySeconds, 0 },
				{ now - 10 * kDaySeconds, now - 10 * kDaySeconds + 60, 0 },
			}) && journal.intervals(2, now).empty(),
			u"trim drops visits older than the retention"_q);
		check(
			journal.usersCount() == 1 && journal.total() == 2,
			u"trim drops empty users"_q);
		check(
			journal.watched(now) == Periods{
				{ now - 60 * kDaySeconds, now - 5 * kDaySeconds },
			},
			u"trim clips the watched periods"_q);
		check(
			journal.known(9, now) == Periods{
				{ now - 60 * kDaySeconds, now - 5 * kDaySeconds },
				{ now - 4 * kDaySeconds, now - 3 * kDaySeconds },
			},
			u"trim drops the old covered periods"_q);

		auto capped = Journal(limits);
		capped.start(now - kDaySeconds);
		const auto fill = [&](uint64 user, int count, TimeId from) {
			for (auto i = 0; i != count; ++i) {
				const auto at = from + i * 1000;
				capped.alive(at);
				capped.apply(user, at + 500, at);
				capped.apply(user, at + 100, at + 100);
			}
		};
		fill(1, 8, now - kDaySeconds);
		capped.trim(now);
		check(
			capped.intervals(1, now).size() == 5
				&& (capped.intervals(1, now).front().from
					== now - kDaySeconds + 3000),
			u"the per-user cap keeps the newest visits"_q);
		fill(2, 5, now - kDaySeconds + 10000);
		fill(3, 5, now - kDaySeconds + 20000);
		capped.trim(now);
		check(
			capped.total() <= 12 && capped.intervals(1, now).empty(),
			u"the total cap drops the oldest visits"_q);
		fill(4, 1, now - 9000);
		fill(5, 1, now - 8000);
		fill(6, 1, now - 7000);
		capped.trim(now);
		check(
			capped.usersCount() == 3
				&& capped.intervals(2, now).empty()
				&& !capped.intervals(6, now).empty(),
			u"the users cap drops those seen longest ago"_q);
	}

	// Serialization.
	{
		auto journal = Journal();
		journal.start(base);
		journal.apply(7, base - 500, base + 1);
		journal.apply(7, base + 900, base + 100);
		journal.alive(base + 120);
		journal.apply(7, base + 200, base + 201);
		journal.apply(0xFFFFFFFFFFULL, base + 2000, base + 250);
		journal.alive(base + 260);
		journal.alive(base + 5000);
		journal.apply(9, base + 9000, base + 5020);
		const auto now = base + 5100;
		const auto bytes = journal.serialize(now);
		auto loaded = Journal();
		const auto parsed = loaded.parse(bytes);
		const auto ongoing = [](Intervals list) {
			for (auto &interval : list) {
				if (interval.flags & kOngoing) {
					interval.flags = (interval.flags & kStartUnknown)
						| kEndUnknown;
				}
			}
			return list;
		};
		check(
			parsed
				&& (loaded.intervals(7, now) == journal.intervals(7, now))
				&& (loaded.intervals(7, now).size() == 2)
				&& (loaded.intervals(0xFFFFFFFFFFULL, now) == Intervals{
					{ base + 250, base + 260, kEndUnknown },
				})
				&& (loaded.intervals(9, now)
					== ongoing(journal.intervals(9, now)))
				&& (loaded.watched(now) == journal.watched(now))
				&& (loaded.watched(now).size() == 2)
				&& (loaded.total() == journal.total() + 1),
			u"serialization round-trip"_q);
		check(
			loaded.serialize(now) == bytes && !loaded.takeChanged(),
			u"a loaded journal serializes to the same bytes"_q);
		loaded.start(now + 10);
		loaded.stop(now + 50);
		check(
			loaded.watched(now + 60).back() == Period{ base + 5000, now + 50 },
			u"a restart continues the stored watched period"_q);

		auto damaged = bytes;
		damaged[damaged.size() / 2] = char(damaged[damaged.size() / 2] ^ 0x55);
		auto broken = Journal();
		check(
			!broken.parse(damaged)
				&& !broken.parse(bytes.left(bytes.size() - 3))
				&& !broken.parse(QByteArray())
				&& !broken.parse(QByteArray(64, char(0x7F)))
				&& broken.empty()
				&& !broken.total(),
			u"damaged data is rejected"_q);
		auto none = Journal();
		check(
			none.parse(Journal().serialize(now)) && none.empty(),
			u"an empty journal round-trip"_q);
		check(bytes.size() < 80, u"the format is compact"_q);
		log.push_back(u"   %1 bytes for %2 intervals"_q.arg(
			bytes.size()
		).arg(loaded.total()));
	}

	// Days in local time.
	{
		const auto zone = QTimeZone::fromSecondsAheadOfUtc(3 * kHourSeconds);
		const auto at = [&](int day, int hour, int minute) {
			return TimeId(QDateTime(
				QDate(2026, 3, day),
				QTime(hour, minute),
				zone).toSecsSinceEpoch());
		};
		const auto intervals = Intervals{
			{ at(9, 12, 0), at(9, 12, 0), kStartUnknown },
			{ at(10, 23, 30), at(11, 0, 45), 0 },
			{ at(11, 9, 0), at(11, 9, 20), 0 },
			{ at(12, 0, 0), at(12, 0, 0), kStartUnknown },
			{ at(12, 11, 50), at(12, 12, 0), kOngoing },
		};
		const auto watched = Periods{
			{ at(10, 20, 0), at(11, 2, 0) },
			{ at(11, 8, 0), at(11, 10, 0) },
			{ at(12, 10, 0), at(12, 12, 0) },
		};
		const auto now = at(12, 12, 0);
		const auto days = SplitByDays(intervals, watched, now, 4, zone);
		check(
			days.size() == 4
				&& days[0].date == QDate(2026, 3, 12)
				&& days[3].date == QDate(2026, 3, 9)
				&& days[1].from == at(11, 0, 0)
				&& days[1].till == at(12, 0, 0),
			u"days go from today back, by local midnights"_q);
		check(
			(days[2].online == Intervals{
				{ at(10, 23, 30), at(11, 0, 0), 0 },
			}) && days[2].total == 1800 && days[2].visits == 1,
			u"a visit over midnight: the first day"_q);
		check(
			(days[1].online == Intervals{
				{ at(11, 0, 0), at(11, 0, 45), 0 },
				{ at(11, 9, 0), at(11, 9, 20), 0 },
			}) && days[1].total == 3900 && days[1].visits == 1,
			u"a visit over midnight: the second day"_q);
		check(
			(days[0].online == Intervals{
				{ at(12, 0, 0), at(12, 0, 0), kStartUnknown },
				{ at(12, 11, 50), at(12, 12, 0), kOngoing },
			}) && days[0].total == 600 && days[0].visits == 2,
			u"a visit exactly at midnight belongs to the new day"_q);
		check(
			(days[2].unknown == Periods{ { at(10, 0, 0), at(10, 20, 0) } })
				&& (days[1].unknown == Periods{
					{ at(11, 2, 0), at(11, 8, 0) },
					{ at(11, 10, 0), at(12, 0, 0) },
				})
				&& (days[0].unknown == Periods{
					{ at(12, 0, 0), at(12, 10, 0) },
				})
				&& (days[3].unknown == Periods{
					{ at(9, 0, 0), at(10, 0, 0) },
				}),
			u"unknown periods are the unwatched time before now"_q);

		const auto echo = SplitByDays({
			{ at(11, 9, 0), at(11, 9, 20), kEndUnknown },
			{ at(11, 9, 24), at(11, 9, 24), kStartUnknown },
			{ at(11, 15, 0), at(11, 15, 0), kStartUnknown },
		}, watched, now, 2, zone);
		check(
			echo[1].visits == 2 && echo[1].online.size() == 3,
			u"a status that ran out after a cut is not one more visit"_q);

		const auto summary = Summarize(days, now);
		check(
			summary.total == 6300
				&& summary.visits == 5
				&& summary.average == 2100
				&& summary.hours[23] == 1800
				&& summary.hours[0] == 2700
				&& summary.hours[9] == 1200
				&& summary.hours[11] == 600,
			u"summary: totals and hours of the day"_q);
		check(
			summary.typical == std::vector<std::pair<int, int>>{ { 23, 25 } },
			u"summary: typical hours wrap around midnight"_q);

		auto busy = std::vector<Day>(1);
		busy[0].from = at(11, 0, 0);
		busy[0].till = at(12, 0, 0);
		const auto visit = [&](int hour, int minutes) {
			busy[0].online.push_back({
				at(11, hour, 0),
				at(11, hour, 0) + minutes * 60,
				0,
			});
			busy[0].total += minutes * 60;
		};
		visit(2, 20);
		visit(8, 50);
		visit(9, 60);
		visit(13, 10);
		visit(19, 40);
		visit(20, 45);
		visit(21, 30);
		visit(23, 35);
		check(
			Summarize(busy, now).typical == std::vector<std::pair<int, int>>{
				{ 8, 10 },
				{ 19, 22 },
				{ 23, 24 },
			},
			u"summary: the three busiest ranges in day order"_q);
		check(
			Summarize({}, now).typical.empty()
				&& Summarize(std::vector<Day>(3), now).average == 0,
			u"summary: nothing from no data"_q);

		const auto berlin = QTimeZone("Europe/Berlin");
		if (berlin.isValid()) {
			const auto time = TimeId(QDateTime(
				QDate(2026, 10, 25),
				QTime(12, 0),
				berlin).toSecsSinceEpoch());
			const auto dst = SplitByDays({}, {}, time, 1, berlin);
			check(
				dst[0].till - dst[0].from == 25 * kHourSeconds,
				u"a daylight saving day is 25 hours long"_q);
		} else {
			log.push_back(u"   no time zone database, DST check skipped"_q);
		}
	}
	return ok;
}

// Self-test end.

[[nodiscard]] bool RunFileSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto folder = cWorkingDir() + u"tdata/oblivion/selftest_online/"_q;
	const auto path = folder + u"online.dat"_q;
	QFile::remove(path);

	const auto now = TimeId(1700000000);
	auto journal = Journal();
	journal.start(now - 100);
	journal.apply(1, now - 5000, now - 90);
	journal.apply(1, now + 300, now - 60);
	journal.apply(2, now + 300, now - 50);
	journal.apply(2, now - 20, now - 19);
	const auto bytes = journal.serialize(now);

	const auto stale = NextGeneration(path);
	const auto fresh = NextGeneration(path);
	WriteFile(path, stale, QByteArray("stale"));
	check(!QFile::exists(path), u"an outdated write is skipped"_q);
	WriteFile(path, fresh, bytes);
	const auto read = ReadFile(path);
	auto loaded = Journal();
	check(
		(read == bytes)
			&& loaded.parse(read)
			&& (loaded.total() == 3)
			&& (loaded.serialize(now) == bytes),
		u"the journal file round-trip"_q);
	RemoveFile(path, stale);
	check(QFile::exists(path), u"an outdated removal is skipped"_q);
	RemoveFile(path, NextGeneration(path));
	check(!QFile::exists(path), u"the journal file is removed"_q);
	check(ReadFile(path).isEmpty(), u"a missing file reads as empty"_q);
	QDir(folder).removeRecursively();
	return ok;
}

} // namespace

bool RunSelfTest(QStringList &log) {
	const auto model = RunModelSelfTest(log);
	const auto files = RunFileSelfTest(log);
	return model && files;
}

} // namespace Online

void StartOnlineTracker(not_null<Main::Session*> session) {
	auto &trackers = Online::Trackers();
	if (trackers.contains(session)) {
		return;
	}
	trackers.emplace(session, std::make_unique<Online::Tracker>(session));
	session->lifetime().add([=] {
		Online::Trackers().remove(session);
	});
}

void ForgetOnlineJournal(not_null<Main::Session*> session) {
	if (const auto tracker = Online::TrackerFor(session)) {
		tracker->forget();
	} else {
		const auto path = Online::JournalPath(session);
		crl::async([=, generation = Online::NextGeneration(path)] {
			Online::RemoveFile(path, generation);
		});
	}
}

void ShowOnlineNotifyList(not_null<Window::SessionController*> controller) {
	using namespace Online;

	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	const auto views = std::make_shared<UserpicViews>();
	auto entries = rpl::single(
		rpl::empty_value()
	) | rpl::then(rpl::merge(
		Get().changes(),
		base::timer_each(kJournalRefresh),
		session->changes().peerUpdates(
			Data::PeerUpdate::Flag::OnlineStatus
			| Data::PeerUpdate::Flag::Name
		) | rpl::filter([](const Data::PeerUpdate &update) {
			return Get().isOnlineNotify(update.peer->id.value);
		}) | rpl::to_empty
	)) | rpl::map([=] {
		return CollectEntries(session, views);
	});
	controller->show(Box(NotifyListBox, NotifyListArgs{
		.show = controller->uiShow(),
		.entries = std::move(entries),
		.repaint = session->downloaderTaskFinished(),
		.add = [=] {
			if (const auto strong = weak.get()) {
				ShowAddNotifyBox(strong);
			}
		},
		.remove = [](uint64 id) {
			Get().setOnlineNotify(id, false);
		},
		.open = [=](uint64 id) {
			const auto strong = weak.get();
			const auto userId = peerToUser(PeerId(id));
			const auto user = strong
				? strong->session().data().userLoaded(userId)
				: nullptr;
			if (user) {
				ShowOnlineJournal(strong, user);
			}
		},
	}));
}

void ShowOnlineJournal(
		not_null<Window::SessionController*> controller,
		not_null<UserData*> user) {
	using namespace Online;

	const auto tracker = TrackerFor(&user->session());
	if (!tracker) {
		return;
	}
	const auto id = user->id.value;
	const auto weak = base::make_weak(tracker);
	auto data = rpl::single(
		rpl::empty_value()
	) | rpl::then(rpl::merge(
		Get().changes(),
		base::timer_each(kJournalRefresh),
		tracker->changes() | rpl::filter([=](uint64 changed) {
			return !changed || (changed == id);
		}) | rpl::to_empty
	)) | rpl::map([=] {
		const auto strong = weak.get();
		return strong ? strong->data(user) : JournalData();
	});
	auto notify = rpl::single(
		rpl::empty_value()
	) | rpl::then(
		Get().changes()
	) | rpl::map([=] {
		return Get().isOnlineNotify(id);
	}) | rpl::distinct_until_changed();
	controller->show(Box(JournalBox, JournalArgs{
		.show = controller->uiShow(),
		.name = user->name(),
		.data = std::move(data),
		.notify = std::move(notify),
		.setNotify = [=](bool enabled) {
			Get().setOnlineNotify(id, enabled);
		},
		.clear = [=] {
			if (const auto strong = weak.get()) {
				strong->clear(user);
			}
		},
	}));
}

void AddOnlineActions(
		const Ui::Menu::MenuCallback &addAction,
		not_null<Window::SessionController*> controller,
		PeerData *peer) {
	const auto user = peer ? peer->asUser() : nullptr;
	if (!user || !Online::Trackable(user)) {
		return;
	}
	const auto id = user->id.value;
	const auto weak = base::make_weak(controller);
	const auto notify = Get().isOnlineNotify(id);
	addAction(
		(notify
			? tr::lng_oblivion_online_menu_notify_off
			: tr::lng_oblivion_online_menu_notify_on)(tr::now),
		[=] {
			Get().setOnlineNotify(id, !notify);
			if (const auto strong = weak.get()) {
				strong->showToast((notify
					? tr::lng_oblivion_online_notify_off_toast
					: tr::lng_oblivion_online_notify_on_toast)(
						tr::now,
						lt_user,
						user->shortName()));
			}
		},
		notify ? &st::menuIconMute : &st::menuIconNotifications);
	addAction(
		tr::lng_oblivion_online_menu_journal(tr::now),
		[=] {
			if (const auto strong = weak.get()) {
				ShowOnlineJournal(strong, user);
			}
		},
		&st::menuIconStats);
}

} // namespace Oblivion
