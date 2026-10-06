/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_listen.h"

#include "api/api_common.h"
#include "api/api_text_entities.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/options.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "core/core_settings.h"
#include "data/data_audio_msg_id.h"
#include "data/data_changes.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_drafts.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "history/view/controls/history_view_forward_panel.h"
#include "history/view/history_view_element.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "media/media_common.h"
#include "mtproto/mtproto_response.h"
#include "oblivion/oblivion_deleted_store.h"
#include "oblivion/oblivion_forward_copy.h"
#include "oblivion/oblivion_listen_ui.h"
#include "oblivion/oblivion_playlists.h"
#include "oblivion/oblivion_sending.h"
#include "oblivion/oblivion_settings.h"
#include "storage/storage_facade.h"
#include "storage/storage_shared_media.h"
#include "ui/text/format_song_document_name.h"
#include "ui/text/format_values.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>

#include "styles/style_menu_icons.h"

namespace Oblivion::Listen {
namespace {

using PlayerState = Media::Player::State;
using SongType = AudioMsgId::Type;

constexpr auto kVersion = uchar(1);
constexpr auto kFlagPlaying = uchar(0x01);
constexpr auto kFlagEnded = uchar(0x02);
constexpr auto kFlagAway = uchar(0x04);
constexpr auto kSessionBytes = 6;
constexpr auto kSessionMask = (uint64(1) << (8 * kSessionBytes)) - 1;
constexpr auto kDocumentBytes = 8;
constexpr auto kMaxQueue = 3;
constexpr auto kMaxTime = int64(48) * 60 * 60 * 1000;
constexpr auto kMaxTrackId = int64(1) << 40;
constexpr auto kMaxDate = int64(0x7FFFFFFF);
constexpr auto kMaxPayloadBytes = 256;
constexpr auto kMaxPayloadChars = 400;
constexpr auto kMaxVisibleLength = 200;

constexpr auto kMinEditGap = crl::time(2000);
constexpr auto kMaxEditGap = crl::time(16000);
constexpr auto kEditsPerMinute = 12;
constexpr auto kMinute = crl::time(60) * 1000;
constexpr auto kEditCost = crl::time(20) * 1000;
constexpr auto kEditBurst = 30;
constexpr auto kEditCreditMax = kEditBurst * kEditCost;
constexpr auto kFailDelay = crl::time(5000);
constexpr auto kMaxFailures = 3;

constexpr auto kHostTolerance = int64(700);
constexpr auto kHostGrace = int64(30) * 1000;
constexpr auto kPausedLive = int64(2) * 60 * 60 * 1000;
constexpr auto kUnknownDuration = int64(2) * 60 * 60 * 1000;
constexpr auto kFutureSkew = int64(10) * 60 * 1000;
constexpr auto kHostPausedEnd = crl::time(60) * 60 * 1000;
constexpr auto kStartTimeout = crl::time(90) * 1000;
constexpr auto kStartDelay = crl::time(400);
constexpr auto kEndTimeout = crl::time(20) * 1000;
constexpr auto kMaxDead = 32;
constexpr auto kMaxOrphans = 8;
constexpr auto kProgressStep = int64(20);
constexpr auto kProgressJump = int64(1500);
constexpr auto kSettleDelay = crl::time(3000);
constexpr auto kQueueCheckEach = crl::time(3000);

constexpr auto kSeekThreshold = int64(1000);
constexpr auto kSeekCooldown = crl::time(4000);
constexpr auto kSeekCooldownMax = crl::time(64000);
constexpr auto kLeadDefault = crl::time(300);
constexpr auto kLeadMax = crl::time(4000);
constexpr auto kAwaitStep = int64(40);
constexpr auto kAwaitSlack = int64(2000);
constexpr auto kAwaitTimeout = crl::time(15000);
constexpr auto kEndMargin = int64(300);
constexpr auto kSeekEndMargin = int64(1500);
constexpr auto kRestartMargin = int64(5000);
constexpr auto kPredictedWait = crl::time(30) * 1000;
constexpr auto kScanLimit = 300;

constexpr auto kLookupGap = crl::time(3000);
constexpr auto kLookupWindow = crl::time(5) * 60 * 1000;
constexpr auto kLookupsInWindow = 10;
constexpr auto kLookupLimit = 100;
constexpr auto kLookupBefore = int64(60);
constexpr auto kLookupClose = int64(10);
constexpr auto kLookupAfter = int64(60) * 60;
constexpr auto kLookupStages = 2;
constexpr auto kLookupFloods = 1;
constexpr auto kMaxBooks = 16;
constexpr auto kMusicSnapshot = 200;

constexpr auto kProbeCount = 5;
constexpr auto kProbeGap = crl::time(1300);
constexpr auto kProbeEach = crl::time(10) * 60 * 1000;
constexpr auto kProbeRetry = crl::time(30) * 1000;
constexpr auto kProbeMaxTrip = int64(4000);
constexpr auto kClockStep = int64(300);

[[nodiscard]] QString Marker() {
	return u"#oblivion-listen."_q;
}

[[nodiscard]] QString Headphones() {
	return u"\U0001F3A7"_q;
}

void PutVarint(QByteArray &to, uint64 value) {
	while (value >= 0x80) {
		to.push_back(char(uchar(value & 0x7F) | 0x80));
		value >>= 7;
	}
	to.push_back(char(uchar(value)));
}

[[nodiscard]] bool GetVarint(
		const QByteArray &from,
		int limit,
		int &offset,
		uint64 &value) {
	value = 0;
	for (auto shift = 0; shift < 64; shift += 7) {
		if (offset >= limit) {
			return false;
		}
		const auto byte = uchar(from[offset++]);
		value |= uint64(byte & 0x7F) << shift;
		if (!(byte & 0x80)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] uint64 ZigZag(int64 value) {
	return (uint64(value) << 1) ^ uint64(value >> 63);
}

[[nodiscard]] int64 UnZigZag(uint64 value) {
	return int64(value >> 1) ^ -int64(value & 1);
}

// CRC-16/CCITT-FALSE. It catches every single bit error and nearly all
// the longer ones: an address cut or changed on its way through other
// apps is not taken for a state.
[[nodiscard]] uint16 Crc16(const QByteArray &data, int size) {
	auto crc = uint32(0xFFFF);
	for (auto i = 0; i != size; ++i) {
		crc ^= uint32(uchar(data[i])) << 8;
		for (auto bit = 0; bit != 8; ++bit) {
			crc = (crc & 0x8000)
				? (((crc << 1) ^ 0x1021) & 0xFFFF)
				: ((crc << 1) & 0xFFFF);
		}
	}
	return uint16(crc);
}

void PutFileId(QByteArray &to, uint64 value) {
	for (auto i = 0; i != kDocumentBytes; ++i) {
		to.push_back(char(uchar((value >> (8 * i)) & 0xFF)));
	}
}

[[nodiscard]] bool TakeFileId(
		const QByteArray &from,
		int limit,
		int &offset,
		uint64 &value) {
	value = 0;
	if (offset + kDocumentBytes > limit) {
		return false;
	}
	for (auto i = 0; i != kDocumentBytes; ++i) {
		value |= uint64(uchar(from[offset++])) << (8 * i);
	}
	return true;
}

// version, flags, six bytes of the session id, then the track (a varint
// of its id, eight bytes of the id of its file, a varint of its date),
// varints of seq, position, at, duration, the size of the queue and its
// tracks (the id and the date as zigzag differences from the previous
// track, the first one from the track itself), then two bytes of the
// checksum of everything before them. Unknown flags and bytes between
// the queue and the checksum are skipped, so a later version of this
// code may add fields without changing the version.
[[nodiscard]] QByteArray Serialize(const State &state) {
	auto result = QByteArray();
	result.reserve(96);
	result.push_back(char(kVersion));
	result.push_back(char((state.playing ? kFlagPlaying : uchar(0))
		| (state.ended ? kFlagEnded : uchar(0))
		| (state.away ? kFlagAway : uchar(0))));
	for (auto i = 0; i != kSessionBytes; ++i) {
		result.push_back(char(uchar((state.session >> (8 * i)) & 0xFF)));
	}
	const auto id = std::clamp(state.track.id, int64(0), kMaxTrackId);
	const auto date = std::clamp(state.track.date, int64(0), kMaxDate);
	PutVarint(result, uint64(id));
	PutFileId(result, state.track.document);
	PutVarint(result, uint64(date));
	PutVarint(result, state.seq);
	PutVarint(
		result,
		uint64(std::clamp(state.position, int64(0), kMaxTime)));
	PutVarint(result, uint64(std::max(state.at, int64(0))));
	PutVarint(
		result,
		uint64(std::clamp(state.duration, int64(0), kMaxTime)));
	const auto count = std::min(int(state.queue.size()), kMaxQueue);
	PutVarint(result, uint64(count));
	auto previous = Track{ .id = id, .date = date };
	for (auto i = 0; i != count; ++i) {
		const auto &next = state.queue[i];
		PutVarint(result, ZigZag(next.id - previous.id));
		PutFileId(result, next.document);
		PutVarint(result, ZigZag(next.date - previous.date));
		previous = next;
	}
	const auto crc = Crc16(result, int(result.size()));
	result.push_back(char(uchar(crc & 0xFF)));
	result.push_back(char(uchar(crc >> 8)));
	return result;
}

[[nodiscard]] std::optional<State> Deserialize(const QByteArray &bytes) {
	const auto size = int(bytes.size());
	const auto header = 2 + kSessionBytes;
	if (size < header + kDocumentBytes + 7 + 2 || size > kMaxPayloadBytes) {
		return std::nullopt;
	}
	const auto body = size - 2;
	const auto crc = uint16(uchar(bytes[body]))
		| uint16(uint16(uchar(bytes[body + 1])) << 8);
	if (Crc16(bytes, body) != crc || uchar(bytes[0]) != kVersion) {
		return std::nullopt;
	}
	auto result = State();
	const auto flags = uchar(bytes[1]);
	result.playing = (flags & kFlagPlaying) != 0;
	result.ended = (flags & kFlagEnded) != 0;
	result.away = (flags & kFlagAway) != 0;
	for (auto i = 0; i != kSessionBytes; ++i) {
		result.session |= uint64(uchar(bytes[2 + i])) << (8 * i);
	}
	auto offset = header;
	auto track = uint64();
	auto document = uint64();
	auto date = uint64();
	auto seq = uint64();
	auto position = uint64();
	auto at = uint64();
	auto duration = uint64();
	auto count = uint64();
	if (!GetVarint(bytes, body, offset, track)
		|| !TakeFileId(bytes, body, offset, document)
		|| !GetVarint(bytes, body, offset, date)
		|| !GetVarint(bytes, body, offset, seq)
		|| !GetVarint(bytes, body, offset, position)
		|| !GetVarint(bytes, body, offset, at)
		|| !GetVarint(bytes, body, offset, duration)
		|| !GetVarint(bytes, body, offset, count)
		|| seq > uint64(std::numeric_limits<uint32>::max())
		|| !track
		|| track > uint64(kMaxTrackId)
		|| !document
		|| date > uint64(kMaxDate)
		|| position > uint64(kMaxTime)
		|| at > uint64(std::numeric_limits<int64>::max() / 2)
		|| duration > uint64(kMaxTime)
		|| count > uint64(kMaxQueue)) {
		return std::nullopt;
	}
	result.seq = uint32(seq);
	result.track = Track{
		.id = int64(track),
		.document = document,
		.date = int64(date),
	};
	result.position = int64(position);
	result.at = int64(at);
	result.duration = int64(duration);
	auto previous = result.track;
	for (auto i = uint64(); i != count; ++i) {
		auto idDelta = uint64();
		auto dateDelta = uint64();
		auto next = Track();
		if (!GetVarint(bytes, body, offset, idDelta)
			|| !TakeFileId(bytes, body, offset, next.document)
			|| !GetVarint(bytes, body, offset, dateDelta)) {
			return std::nullopt;
		}
		const auto idShift = UnZigZag(idDelta);
		const auto dateShift = UnZigZag(dateDelta);
		if (idShift < -kMaxTrackId
			|| idShift > kMaxTrackId
			|| dateShift < -kMaxDate
			|| dateShift > kMaxDate) {
			return std::nullopt;
		}
		next.id = previous.id + idShift;
		next.date = previous.date + dateShift;
		if (next.id <= 0
			|| next.id > kMaxTrackId
			|| !next.document
			|| next.date < 0
			|| next.date > kMaxDate) {
			return std::nullopt;
		}
		result.queue.push_back(next);
		previous = next;
	}
	return result;
}

[[nodiscard]] QString EncodeState(const State &state) {
	return u"https://t.me/"_q
		+ Marker()
		+ QString::fromLatin1(Serialize(state).toBase64(
			QByteArray::Base64UrlEncoding
			| QByteArray::OmitTrailingEquals));
}

[[nodiscard]] std::optional<State> DecodeState(const QString &url) {
	const auto marker = Marker();
	const auto index = url.indexOf(marker);
	if (index < 0) {
		return std::nullopt;
	}
	const auto from = index + int(marker.size());
	auto till = from;
	const auto size = int(url.size());
	while (till != size) {
		const auto ch = url[till].unicode();
		const auto fits = (ch >= 'A' && ch <= 'Z')
			|| (ch >= 'a' && ch <= 'z')
			|| (ch >= '0' && ch <= '9')
			|| (ch == '-')
			|| (ch == '_');
		if (!fits) {
			break;
		}
		++till;
	}
	const auto length = till - from;
	if (till != size || length <= 0 || length > kMaxPayloadChars) {
		// The state is the whole fragment, nothing may follow it.
		return std::nullopt;
	}
	return Deserialize(QByteArray::fromBase64(
		url.mid(from, length).toLatin1(),
		QByteArray::Base64UrlEncoding));
}

[[nodiscard]] TextWithEntities MakeControlText(
		const QString &visible,
		const State &state) {
	const auto emoji = Headphones();
	auto result = TextWithEntities{
		emoji + ' ' + visible.trimmed().left(kMaxVisibleLength),
	};
	result.entities.push_back(EntityInText(
		EntityType::CustomUrl,
		0,
		int(emoji.size()),
		EncodeState(state)));
	return result;
}

// A control message is exactly what MakeControlText() makes: the
// headphones, a space, a short text, and the link with the state over
// the headphones. A link with the marker anywhere else in a text is not
// a state: nobody can turn an ordinary message into a control one by
// adding a link to a word of it.
[[nodiscard]] std::optional<State> ParseControlText(
		const TextWithEntities &text) {
	const auto emoji = Headphones();
	const auto prefix = int(emoji.size()) + 1;
	if (!text.text.startsWith(emoji + ' ')
		|| text.text.size() > prefix + kMaxVisibleLength) {
		return std::nullopt;
	}
	for (const auto &entity : text.entities) {
		if (entity.type() == EntityType::CustomUrl
			&& entity.offset() == 0
			&& entity.length() == int(emoji.size())) {
			return DecodeState(entity.data());
		}
	}
	return std::nullopt;
}

// The edit of a control message that the host of its session makes:
// both texts are control messages of one session and the state has gone
// forward. An ended session is never edited by its host again.
[[nodiscard]] bool IsSessionEdit(
		const TextWithEntities &was,
		const TextWithEntities &now) {
	const auto before = ParseControlText(was);
	const auto after = before ? ParseControlText(now) : std::nullopt;
	return after
		&& !before->ended
		&& (before->session == after->session)
		&& (after->seq > before->seq);
}

[[nodiscard]] int64 ExpectedPosition(const State &state, int64 now) {
	auto result = state.position;
	if (state.playing && !state.ended && now > state.at) {
		result += (now - state.at);
	}
	if (state.duration > 0) {
		result = std::min(result, state.duration);
	}
	return std::max(result, int64(0));
}

// How long the state stays trustworthy without a new edit of the host,
// zero or less means the session is over. A playing track needs no edits
// until its end; after that the host must have switched to the next
// track or ended the session, the grace covers the time his edit needs.
[[nodiscard]] int64 LiveLeft(const State &state, int64 now) {
	if (state.ended || !state.track.id || state.at > now + kFutureSkew) {
		return 0;
	} else if (!state.playing) {
		return state.at + kPausedLive - now;
	}
	const auto left = (state.duration > 0)
		? std::max(state.duration - state.position, int64(0))
		: kUnknownDuration;
	return state.at + left + kHostGrace - now;
}

[[nodiscard]] bool SameContent(const State &a, const State &b) {
	return (a.track == b.track)
		&& (a.position == b.position)
		&& (a.at == b.at)
		&& (a.duration == b.duration)
		&& (a.playing == b.playing)
		&& (a.ended == b.ended)
		&& (a.away == b.away)
		&& (a.queue == b.queue);
}

// True when the listeners, who compute the position from the state and
// the time, would be wrong about what the player of the host does now:
// another track, play instead of pause, a position that is not where
// the time has brought it (a seek, a stall), the host is back to a track
// of the chat. Only then the message is edited, a track that simply
// plays needs no edits at all.
[[nodiscard]] bool HostMoved(
		const State &state,
		const Track &track,
		bool playing,
		int64 position,
		int64 duration,
		int64 now) {
	return (state.track != track)
		|| (state.playing != playing)
		|| state.away
		|| (std::abs(position - ExpectedPosition(state, now)) > kHostTolerance)
		|| (!state.duration && duration > 0);
}

// What a player that reports "playing" really does. A track that has
// just been started or sought reports "playing" at once, but the sound
// goes only when its data is loaded, and a track that waits for more
// data in the middle keeps reporting "playing" with a position that
// stands still. The position is published only while it really moves:
// otherwise every listener would be ahead of the host by the time the
// loading took. A jump is a position that went back or further than the
// time that has passed since the previous report.
struct Progress {
	FullMsgId id;
	int64 position = 0;
	crl::time at = 0;
	bool moving = false;
};

void UpdateProgress(
		Progress &progress,
		FullMsgId id,
		bool loaded,
		bool playing,
		int64 position,
		crl::time now) {
	const auto passed = int64(now - progress.at);
	const auto was = progress.position;
	if (!loaded
		|| progress.id != id
		|| position < was
		|| position > was + passed + kProgressJump) {
		progress.id = id;
		progress.position = position;
		progress.at = now;
		progress.moving = false;
	} else if (position >= was + kProgressStep) {
		progress.position = position;
		progress.at = now;
		progress.moving = true;
	} else if (progress.moving && playing && passed > kHostTolerance) {
		// The position and the time of it are kept: the first real
		// movement switches the flag back on.
		progress.moving = false;
	}
}

// Which message of the chat is the track that the host means, zero if
// none of them is. Only a message with the same music file fits. With
// several of them: in a supergroup the one with the id of the host (the
// ids are the same for everybody there), otherwise the one whose date
// is the closest to the date of the host, the newest of the equal ones.
struct Candidate {
	int64 id = 0;
	uint64 document = 0;
	int64 date = 0;
};

[[nodiscard]] int64 PickTrack(
		const Track &track,
		const std::vector<Candidate> &list,
		bool sharedIds) {
	auto result = int64(0);
	auto best = int64(-1);
	if (!track.document) {
		return result;
	}
	for (const auto &entry : list) {
		if (entry.id <= 0 || entry.document != track.document) {
			continue;
		} else if (sharedIds && entry.id == track.id) {
			return entry.id;
		}
		const auto distance = (track.date > 0 && entry.date > 0)
			? std::abs(entry.date - track.date)
			: kMaxDate;
		if (best < 0
			|| distance < best
			|| (distance == best && entry.id > result)) {
			best = distance;
			result = entry.id;
		}
	}
	return result;
}

// The dates between which the music of the chat is searched for a track
// (the server gives the newest hundred of what is between them). First
// around the date that the host has for the message: that is the date
// the server has, and the track is found even among many others sent
// together. If it is not there, once more in the hour after that date:
// for a message that the host has sent himself since his app was started
// his date is the moment he pressed "send", while the server dated the
// message when the file was uploaded.
struct DateWindow {
	int64 from = 0;
	int64 till = 0;
};

[[nodiscard]] DateWindow LookupWindow(const Track &track, int stage) {
	if (track.date <= 0) {
		return {};
	}
	const auto edge = track.date + kLookupClose;
	const auto from = (stage > 1)
		? (edge - 2)
		: (track.date - kLookupBefore);
	const auto till = (stage > 1) ? (track.date + kLookupAfter) : edge;
	return {
		.from = std::clamp(from, int64(1), kMaxDate),
		.till = std::clamp(till, int64(1), kMaxDate),
	};
}

// How many searches a track gets: both stages if its date is known,
// otherwise there are no dates to search between and one is enough.
[[nodiscard]] int LookupStages(const Track &track) {
	return (track.date > 0) ? kLookupStages : 1;
}

// When a listener may ask the server for a track message: one request
// at a time (the caller keeps that), three seconds between two of them,
// not more than ten in five minutes, FLOOD_WAIT waited out in full.
// One pace for the whole app: leaving a session and joining it or
// another one again starts nothing over.
class LookupPace final {
public:
	[[nodiscard]] crl::time delay(crl::time now) const;
	void sent(crl::time now);
	void flood(crl::time now, int seconds);

private:
	std::deque<crl::time> _recent;
	crl::time _notBefore = 0;

};

crl::time LookupPace::delay(crl::time now) const {
	auto at = _notBefore;
	const auto count = int(_recent.size());
	if (count > 0) {
		at = std::max(at, _recent.back() + kLookupGap);
	}
	if (count >= kLookupsInWindow) {
		at = std::max(at, _recent[count - kLookupsInWindow] + kLookupWindow);
	}
	return std::max(at - now, crl::time(0));
}

void LookupPace::sent(crl::time now) {
	while (!_recent.empty() && _recent.front() + kLookupWindow <= now) {
		_recent.pop_front();
	}
	_recent.push_back(now);
}

void LookupPace::flood(crl::time now, int seconds) {
	_notBefore = std::max(
		_notBefore,
		now + (std::max(seconds, 1) + 1) * crl::time(1000));
}

// What the listeners of this app have found out about the track messages
// of a session: which message of the chat has which music file, how many
// times the server has been asked about a file (a fixed small number at
// most, see LookupWindow) and how many of the requests were sent once
// more after FLOOD_WAIT, the files that the chat does not have. It is
// kept for the session, not for one joining of it: "Leave" and "Join"
// again asks the server nothing that it has answered already.
struct LookupBook {
	FullMsgId message; // The control message of the session.
	base::flat_map<uint64, MsgId> found;
	base::flat_map<uint64, int> asked;
	base::flat_map<uint64, int> floods;
	base::flat_set<uint64> missing;
};

[[nodiscard]] bool LookupExhausted(
		const LookupBook &book,
		const Track &track) {
	const auto i = book.asked.find(track.document);
	return (i != end(book.asked)) && (i->second >= LookupStages(track));
}

// A search for the track goes out: the number of it, see LookupWindow().
int LookupBegin(LookupBook &book, const Track &track) {
	return ++book.asked[track.document];
}

// The search was taken back before its answer (the listener has left):
// it has told nothing, the same one is made when the track is needed
// again.
void LookupAborted(LookupBook &book, const Track &track) {
	const auto i = book.asked.find(track.document);
	if (i != end(book.asked) && i->second > 0) {
		--i->second;
	}
}

// FLOOD_WAIT instead of an answer: true if the same search is made once
// more after the wait, false if that was done already and the track is
// given up.
[[nodiscard]] bool LookupFlooded(LookupBook &book, const Track &track) {
	if (book.floods[track.document]++ < kLookupFloods) {
		LookupAborted(book, track);
		return true;
	}
	book.asked[track.document] = kLookupStages;
	return false;
}

// The search has not found the track: true if the next one is to be
// made, otherwise everything has been asked and the chat does not have
// the file.
[[nodiscard]] bool LookupGoesOn(LookupBook &book, const Track &track) {
	if (!LookupExhausted(book, track)) {
		return true;
	}
	book.missing.emplace(track.document);
	return false;
}

// A message with the music file is in the chat, whatever was found out
// about the file before.
void LookupAdopt(LookupBook &book, uint64 document, MsgId id) {
	if (!book.found.contains(document)) {
		book.found.emplace(document, id);
	}
	book.missing.remove(document);
}

// A group with the slow mode on takes one message at a time from those
// it applies to, and the next one only after a wait. A session needs
// one message (the control one, or a track sent by "Send to chat") or
// two in a row (a track that the chat does not have, then the control
// message): the second one would be refused when the first is there.
enum class SlowmodeBlock {
	None,
	Wait, // The countdown after the previous message goes on.
	Busy, // A message is being sent to the group right now.
	TrackFirst, // Two messages in a row are not possible at all.
};

[[nodiscard]] SlowmodeBlock CheckSlowmode(
		bool applied,
		int secondsLeft,
		bool sendingNow,
		int count) {
	return (secondsLeft > 0)
		? SlowmodeBlock::Wait
		: !applied
		? SlowmodeBlock::None
		: (count > 1)
		? SlowmodeBlock::TrackFirst
		: sendingNow
		? SlowmodeBlock::Busy
		: SlowmodeBlock::None;
}

// The refusals of a send that ApiWrap::sendMessageFail() explains to the
// user by itself (a box, a toast of its own) or has something to remember
// about. Any other one is a bare code there.
[[nodiscard]] bool SendRefusalExplained(const QString &type) {
	return (type == u"PEER_FLOOD"_q)
		|| (type == u"USER_BANNED_IN_CHANNEL"_q)
		|| (type == u"CHAT_FORWARDS_RESTRICTED"_q)
		|| type.startsWith(u"ALLOW_PAYMENT_REQUIRED_"_q);
}

[[nodiscard]] bool SendRefusalSlowmode(const QString &type) {
	return type.startsWith(u"SLOWMODE_WAIT_"_q);
}

// How far the clock of this computer is from the clock of the server.
//
// Every answer of the server has an id that the server made at some
// moment between the request and the answer, and the upper half of the
// id is the second that the clock of the server showed then. So each
// answer gives two bounds for the difference of the clocks, a second
// plus the round trip apart, and several answers that came at different
// parts of a second bring the bounds together. The lower half of the id
// is not used: nothing promises that it is a real fraction of a second.
//
// The difference that the app already has (whole seconds, and zero
// while the local clock is wrong by less than three seconds) is moved
// only as far as the bounds make it: a computer with a right clock stays
// exactly as it is, a computer with a wrong one is brought to the server
// time within a few tenths of a second.
class ClockSync final {
public:
	void add(int64 sent, int64 received, uint64 msgId);
	void reset();

	[[nodiscard]] bool ready() const {
		return (_count > 0);
	}
	[[nodiscard]] int64 apply(int64 prior) const {
		return _count ? std::clamp(prior, _low, _high) : prior;
	}

private:
	int64 _low = 0;
	int64 _high = 0;
	int _count = 0;

};

void ClockSync::add(int64 sent, int64 received, uint64 msgId) {
	const auto trip = received - sent;
	const auto seconds = int64(msgId >> 32);
	if (trip < 0 || trip > kProbeMaxTrip || seconds <= 0) {
		return;
	}
	const auto low = seconds * 1000 - received;
	const auto high = seconds * 1000 + 1000 - sent;
	if (!_count || low > _high || high < _low) {
		// The first answer, or the local clock has been set meanwhile.
		_low = low;
		_high = high;
		_count = 1;
		return;
	}
	_low = std::max(_low, low);
	_high = std::min(_high, high);
	++_count;
}

void ClockSync::reset() {
	_count = 0;
}

[[nodiscard]] int FloodSeconds(const QString &type) {
	const auto index = type.lastIndexOf('_');
	const auto value = (index >= 0) ? type.mid(index + 1).toInt() : 0;
	return std::clamp(value, 1, 24 * 60 * 60);
}

// When the control message may be edited. One request at a time, at
// least two seconds between two edits, not more than twelve in a minute,
// and over a long time one edit in twenty seconds: thirty edits may be
// made faster than that (a host who seeks and switches tracks a lot for
// a while), after that the pace falls to three in a minute until the
// host calms down. FLOOD_WAIT is waited out in full (and the gap doubles
// after each one), other errors give growing pauses and a stop after
// three of them in a row. Everything asked for in between is one edit
// with the newest state: request() only raises a flag.
class Throttle final {
public:
	void request() {
		_dirty = true;
	}
	[[nodiscard]] bool dirty() const {
		return _dirty;
	}
	[[nodiscard]] bool inFlight() const {
		return _inFlight;
	}
	[[nodiscard]] bool broken() const {
		return (_failures >= kMaxFailures);
	}
	[[nodiscard]] bool flooded(crl::time now) const {
		return (_floodTill > now);
	}

	// Nothing: there is nothing to send or the request is in flight
	// (ask again when it finishes). Zero: send now. Otherwise ask again
	// after that many milliseconds. The last edit of a session, the
	// "ended" one, is not held back by the limits that this code sets
	// for itself (twelve in a minute, the long time budget): it waits
	// only for the two seconds after the previous edit, for FLOOD_WAIT
	// and for the pause after an error.
	[[nodiscard]] std::optional<crl::time> delay(
		crl::time now,
		bool last = false) const;

	void sent(crl::time now);
	void done();
	void flood(crl::time now, int seconds);
	void failed(crl::time now);

private:
	[[nodiscard]] crl::time credit(crl::time now) const;

	std::deque<crl::time> _recent;
	crl::time _lastSent = 0;
	crl::time _notBefore = 0;
	crl::time _floodTill = 0;
	crl::time _gap = kMinEditGap;
	crl::time _credit = kEditCreditMax;
	int _failures = 0;
	bool _sentOnce = false;
	bool _dirty = false;
	bool _inFlight = false;

};

// The long time budget in milliseconds: every edit costs twenty seconds
// of it, every second of the time gives a second back.
crl::time Throttle::credit(crl::time now) const {
	return _sentOnce
		? std::min(_credit + (now - _lastSent), kEditCreditMax)
		: _credit;
}

std::optional<crl::time> Throttle::delay(crl::time now, bool last) const {
	if (!_dirty || _inFlight) {
		return std::nullopt;
	}
	auto at = std::max(_notBefore, _floodTill);
	if (_sentOnce) {
		at = std::max(at, _lastSent + _gap);
	}
	if (last) {
		return std::max(at - now, crl::time(0));
	}
	const auto count = int(_recent.size());
	if (count >= kEditsPerMinute) {
		at = std::max(at, _recent[count - kEditsPerMinute] + kMinute);
	}
	const auto have = credit(now);
	if (have < kEditCost) {
		at = std::max(at, now + (kEditCost - have));
	}
	return std::max(at - now, crl::time(0));
}

void Throttle::sent(crl::time now) {
	_credit = std::max(credit(now) - kEditCost, crl::time(0));
	_dirty = false;
	_inFlight = true;
	_sentOnce = true;
	_lastSent = now;
	while (!_recent.empty() && _recent.front() + kMinute <= now) {
		_recent.pop_front();
	}
	_recent.push_back(now);
}

void Throttle::done() {
	_inFlight = false;
	_failures = 0;
}

void Throttle::flood(crl::time now, int seconds) {
	_inFlight = false;
	_dirty = true;
	_floodTill = now + (std::max(seconds, 1) + 1) * crl::time(1000);
	_gap = std::min(_gap * 2, kMaxEditGap);
}

void Throttle::failed(crl::time now) {
	_inFlight = false;
	_dirty = true;
	++_failures;
	const auto shift = std::min(_failures - 1, 4);
	_notBefore = std::max(_notBefore, now + (kFailDelay << shift));
}

// Keeps the player of a listener near the position of the host.
//
// The player is told to seek only when it is more than a second away
// (a seek is an audible jump, and the clocks of two computers are not
// equal to a few tenths of a second anyway), and not again before the
// previous seek had its chance: four seconds, doubled every time the
// seek did not help (a slow connection), back to four once the player
// is in place.
//
// A seek takes time, so it aims ahead of the host by the time the
// previous seeks have taken (the lead). The first start of a track is
// "cold": the file is not loaded yet and the time it takes says nothing
// about the later seeks, so it is not put into the lead.
class Corrector final {
public:
	struct Result {
		bool seek = false;
		int64 target = 0;
	};

	// A new state has come from the host.
	void reset();

	// The player was told to play from target.
	void started(crl::time now, int64 target, bool cold);

	// expected is where the host is now, actual is where the player is.
	[[nodiscard]] Result update(crl::time now, int64 expected, int64 actual);

	[[nodiscard]] bool loading() const {
		return _awaiting && _cold;
	}
	[[nodiscard]] crl::time lead() const {
		return _lead;
	}

private:
	crl::time _lead = kLeadDefault;
	crl::time _issuedAt = 0;
	int64 _issuedTarget = 0;
	crl::time _lastSeek = 0;
	crl::time _cooldown = kSeekCooldown;
	bool _awaiting = false;
	bool _cold = false;
	bool _seeked = false;

};

void Corrector::reset() {
	_seeked = false;
	_cooldown = kSeekCooldown;
}

void Corrector::started(crl::time now, int64 target, bool cold) {
	_awaiting = true;
	_cold = cold;
	_issuedAt = now;
	_issuedTarget = target;
}

Corrector::Result Corrector::update(
		crl::time now,
		int64 expected,
		int64 actual) {
	if (_awaiting) {
		// Until the sound really goes the player reports the position it
		// was sent to. It can't be further than the time that has passed.
		const auto elapsed = int64(now - _issuedAt);
		const auto moved = actual - _issuedTarget;
		if (moved < kAwaitStep || moved > elapsed + kAwaitSlack) {
			if (now - _issuedAt > kAwaitTimeout) {
				_awaiting = false;
			}
			return {};
		}
		_awaiting = false;
		if (!_cold) {
			const auto latency = crl::time(elapsed - moved);
			_lead = std::clamp(
				(_lead + latency) / 2,
				crl::time(0),
				kLeadMax);
		}
	}
	const auto drift = actual - expected;
	if (std::abs(drift) <= kSeekThreshold) {
		_cooldown = kSeekCooldown;
		return {};
	} else if (_seeked && (now - _lastSeek < _cooldown)) {
		return {};
	}
	if (_seeked) {
		_cooldown = std::min(_cooldown * 2, kSeekCooldownMax);
	}
	_seeked = true;
	_lastSeek = now;
	const auto target = std::max(expected + int64(_lead), int64(0));
	started(now, target, false);
	return { .seek = true, .target = target };
}

struct ClockState {
	ClockSync sync;
	int64 base = 0;
	crl::time probedAt = 0;
};

[[nodiscard]] ClockState &Clock() {
	static auto result = ClockState();
	return result;
}

[[nodiscard]] int64 WallMs() {
	return int64(QDateTime::currentMSecsSinceEpoch());
}

// The local clock against the clock that only counts the time: if they
// have parted, the local clock was set (or the computer has slept) and
// what was measured about it is not true any more.
[[nodiscard]] int64 ClockBase() {
	return WallMs() - int64(crl::now());
}

void ClockSample(int64 sent, uint64 msgId) {
	auto &clock = Clock();
	const auto base = ClockBase();
	if (clock.sync.ready() && std::abs(base - clock.base) > kClockStep) {
		clock.sync.reset();
	}
	if (!clock.sync.ready()) {
		clock.base = base;
	}
	clock.sync.add(sent, WallMs(), msgId);
}

// The time of the server in milliseconds. base::unixtime keeps the
// difference between the server clock and the local one in whole seconds
// (and it is zero while the local clock is right within three seconds),
// ClockSync makes it exact where the server has shown that it is not.
// Main thread.
[[nodiscard]] int64 NowMs() {
	auto shift = int64(0);
	for (auto i = 0; i != 3; ++i) {
		const auto before = QDateTime::currentSecsSinceEpoch();
		const auto server = int64(base::unixtime::now());
		const auto after = QDateTime::currentSecsSinceEpoch();
		shift = server - int64(before);
		if (before == after) {
			break;
		}
	}
	auto &clock = Clock();
	if (clock.sync.ready()
		&& std::abs(ClockBase() - clock.base) > kClockStep) {
		clock.sync.reset();
	}
	return WallMs() + clock.sync.apply(shift * 1000);
}

[[nodiscard]] DocumentData *TrackDocument(not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	return (document && document->isAudioFile() && !media->ttlSeconds())
		? document
		: nullptr;
}

[[nodiscard]] QString TrackTitle(not_null<DocumentData*> document) {
	return Ui::Text::FormatSongNameFor(document).string();
}

// A message of a chat that can be the track of a session there.
[[nodiscard]] DocumentData *ChatTrackDocument(not_null<HistoryItem*> item) {
	return (item->isRegular() && IsServerMsgId(item->id) && !IsKeptDeleted(item))
		? TrackDocument(item)
		: nullptr;
}

[[nodiscard]] Track TrackOf(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document) {
	return {
		.id = item->id.bare,
		.document = document->id,
		.date = int64(item->date()),
	};
}

// A track is sent to the session chat by a plain forward without the
// sender name and without the caption. A track from a chat with
// protected content is not sent: the "forward as a copy" of Oblivion
// would upload it as another file, and the protection is there for
// a reason. Neither is a message that can't be forwarded without the
// name of its author.
[[nodiscard]] bool Shareable(not_null<HistoryItem*> item) {
	using Options = Data::ForwardOptions;
	return item->isRegular()
		&& !IsKeptDeleted(item)
		&& ItemForwardAllowed(item)
		&& !ItemForwardProtected(item)
		&& (HistoryView::Controls::NormalizeForwardOptions(
			&item->history()->session(),
			{ item },
			Options::NoNamesAndCaptions) == Options::NoNamesAndCaptions);
}

[[nodiscard]] bool CanSendTrackTo(not_null<PeerData*> peer) {
	return Data::CanSend(peer, ChatRestriction::SendMusic)
		&& (peer->starsPerMessageChecked() <= 0);
}

[[nodiscard]] QString VisibleText(const QString &title, bool ended) {
	return ended
		? tr::lng_oblivion_listen_message_ended(tr::now, lt_track, title)
		: tr::lng_oblivion_listen_message(tr::now, lt_track, title);
}

[[nodiscard]] MTPmessages_EditMessage EditRequest(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		MsgId id,
		const TextWithEntities &text) {
	using Flag = MTPmessages_EditMessage::Flag;
	return MTPmessages_EditMessage(
		MTP_flags(Flag::f_message | Flag::f_no_webpage | Flag::f_entities),
		peer->input(),
		MTP_int(id),
		MTP_string(text.text),
		MTPInputMedia(),
		MTPReplyMarkup(),
		Api::EntitiesToMTP(
			session,
			text.entities,
			Api::ConvertOption::SkipLocal),
		MTPint(), // schedule_date
		MTPint(), // schedule_repeat_period
		MTPint(), // quick_reply_shortcut_id
		MTPInputRichMessage());
}

// A control message is an ordinary message that its author has written
// to the chat himself: not a service one, not a post of a channel, not
// forwarded, not sent via a bot. The author is the host of the session.
// It is not always a person: for a host who is an anonymous admin of the
// group the author is the group itself, for a host who writes there as
// a channel it is that channel, such sessions are joined as any other.
[[nodiscard]] bool WrittenToChat(not_null<const HistoryItem*> item) {
	return !item->isService()
		&& !item->isPost()
		&& !item->Has<HistoryMessageForwarded>()
		&& !item->Has<HistoryMessageVia>();
}

[[nodiscard]] std::optional<State> ParseItem(
		not_null<const HistoryItem*> item) {
	if (!item->isRegular()
		|| !WrittenToChat(item)
		|| IsKeptDeleted(item)) {
		return std::nullopt;
	}
	return ParseControlText(item->originalText());
}

// What stands in the way of count messages sent to the chat in a row
// right now, empty if nothing does. See CheckSlowmode().
[[nodiscard]] QString SlowmodeError(not_null<PeerData*> peer, int count) {
	const auto left = peer->slowmodeSecondsLeft();
	const auto history = peer->owner().historyLoaded(peer);
	const auto block = CheckSlowmode(
		peer->slowmodeApplied(),
		left,
		history && (history->latestSendingMessage() != nullptr),
		count);
	switch (block) {
	case SlowmodeBlock::None: break;
	case SlowmodeBlock::Wait:
		return tr::lng_slowmode_enabled(
			tr::now,
			lt_left,
			Ui::FormatDurationWordsSlowmode(left));
	case SlowmodeBlock::Busy: return tr::lng_slowmode_no_many(tr::now);
	case SlowmodeBlock::TrackFirst:
		return tr::lng_oblivion_listen_error_slowmode(tr::now);
	}
	return QString();
}

// The loaded message of the chat with the music file of track, see
// PickTrack(). Looks through the newest loaded messages of the chat and
// through its list of music if the app has it. Sends nothing.
[[nodiscard]] HistoryItem *LocateLoaded(
		not_null<PeerData*> peer,
		const Track &track) {
	using namespace Storage;

	if (!track.document) {
		return nullptr;
	}
	const auto session = &peer->session();
	const auto sharedIds = peer->isChannel();
	if (sharedIds && track.id > 0) {
		const auto item = session->data().message(peer->id, MsgId(track.id));
		const auto document = item ? ChatTrackDocument(item) : nullptr;
		if (document && document->id == track.document) {
			return item;
		}
	}
	auto list = std::vector<Candidate>();
	const auto add = [&](not_null<HistoryItem*> item) {
		const auto document = ChatTrackDocument(item);
		if (document && document->id == track.document) {
			list.push_back({
				.id = item->id.bare,
				.document = document->id,
				.date = int64(item->date()),
			});
		}
	};
	if (const auto history = session->data().historyLoaded(peer)) {
		auto left = kScanLimit;
		for (auto i = history->blocks.rbegin()
			; left > 0 && i != history->blocks.rend()
			; ++i) {
			const auto &messages = (*i)->messages;
			for (auto j = messages.rbegin()
				; left > 0 && j != messages.rend()
				; ++j, --left) {
				add((*j)->data());
			}
		}
	}
	if (list.empty()) {
		const auto slice = session->storage().snapshot(SharedMediaQuery(
			SharedMediaKey(
				peer->id,
				MsgId(0),
				PeerId(0),
				SharedMediaType::MusicFile,
				ServerMaxMsgId - 1),
			kMusicSnapshot,
			0));
		for (const auto &id : slice.messageIds) {
			if (const auto item = session->data().message(peer->id, id)) {
				add(item);
			}
		}
	}
	const auto id = PickTrack(track, list, sharedIds);
	return id ? session->data().message(peer->id, MsgId(id)) : nullptr;
}

// The position of the host is "where he was plus the time that has
// passed", which is true only at the normal speed. Long tracks may be
// played faster or slower, such a playback is not shared and not
// corrected.
[[nodiscard]] bool NormalSpeed(const AudioMsgId &id) {
	return !id.changeablePlaybackSpeed()
		|| (std::abs(Core::App().settings().audioPlaybackSpeed() - 1.)
			< 0.01);
}

[[nodiscard]] int64 ToMs(int64 value, int frequency) {
	return (value > 0 && frequency > 0) ? (value * 1000 / frequency) : 0;
}

void Toast(Main::Session *session, const QString &text) {
	if (!session || text.isEmpty()) {
		return;
	}
	const auto &windows = session->windows();
	if (windows.empty()) {
		return;
	}
	for (const auto &window : windows) {
		if (window->isPrimary()) {
			window->showToast(text);
			return;
		}
	}
	windows.front()->showToast(text);
}

struct StartPosition {
	DocumentData *document = nullptr;
	FullMsgId id;
	crl::time position = -1;
};

[[nodiscard]] StartPosition &PendingStart() {
	static auto result = StartPosition();
	return result;
}

[[nodiscard]] rpl::event_stream<> &FollowStream() {
	static const auto result = new rpl::event_stream<>();
	return *result;
}

class Manager final : public base::has_weak_ptr {
public:
	Manager();

	[[nodiscard]] static Manager &Instance();
	[[nodiscard]] static Manager *Existing();

	void start(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		FullMsgId track);
	void join(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer);
	void leave();
	void finishHosting();
	void sendTrack(DocumentId id);
	bool clicked(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item);

	[[nodiscard]] ChatView lookup(not_null<PeerData*> peer);
	[[nodiscard]] bool rescan(not_null<PeerData*> peer);
	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}

	[[nodiscard]] bool hosting() const {
		return (_host != nullptr);
	}
	[[nodiscard]] bool follows(const AudioMsgId &current) const;
	bool followerMove(int delta, bool autonext);
	[[nodiscard]] bool quitPrevent();

private:
	enum class Finish {
		Ended,
		Vanished,
		Left,
		Quiet,
	};
	struct Watch {
		rpl::lifetime lifetime;
	};
	struct Observed {
		FullMsgId message;
		State state;
		PeerId host = 0;
		MsgId track = 0; // The track message for this account, if found.
	};
	struct Sample {
		enum class Kind {
			Idle,
			Loading,
			Finished,
			Paused,
			Playing,
		};
		Kind kind = Kind::Idle;
		int64 position = 0;
		int64 duration = 0;
	};
	struct Host {
		enum class Phase {
			Sharing,
			Posting,
			Live,
			Ending,
		};

		base::weak_ptr<Main::Session> session;
		PeerData *peer = nullptr;
		uint64 sid = 0;
		Phase phase = Phase::Sharing;

		// The track the session was started with and whether the host
		// has agreed in the start box that it is sent to the chat.
		FullMsgId start;
		bool startSends = false;
		bool startSent = false;

		// The messages of the chat with the files that the host has
		// played, the files that the chat does not have, the one that
		// is being sent there after a click of the host (till its copy
		// is in the chat or the server refuses it) and the request with
		// it while the server has not answered.
		base::flat_map<DocumentId, MsgId> known;
		base::flat_set<DocumentId> absent;
		DocumentId sharing = 0;
		int shareRequest = 0;

		// What the player has now, if it is not a message of the chat.
		DocumentId away = 0;
		QString awayTitle;
		bool awaySendable = false;

		MsgId localId = 0;
		FullMsgId message;
		uint32 seq = 0;
		State desired;
		State sending;
		QString title;
		Throttle throttle;
		mtpRequestId requestId = 0;
		bool verified = false;
		bool quitting = false;

		Progress progress;
		crl::time queueCheckedAt = 0;
	};
	struct Listener {
		uint64 serial = 0;
		base::weak_ptr<Main::Session> session;
		PeerData *peer = nullptr;
		FullMsgId message;
		PeerId host = 0;
		State state;
		QString title;
		Corrector corrector;

		// The message that the track of the state is for this account,
		// empty while it is not found, and the message the player has
		// been given.
		FullMsgId track;
		FullMsgId playing;

		// What is known about the track messages of the session (the
		// manager keeps it when the listener leaves, see LookupBook),
		// the tracks that wait for their turn to be searched for and
		// the one whose search is in flight.
		std::shared_ptr<LookupBook> book = std::make_shared<LookupBook>();
		std::deque<Track> wanted;
		Track lookup;
		mtpRequestId lookupId = 0;

		bool predicted = false;
		bool loading = false;
		bool unavailable = false;
		bool ownPause = false;
		bool pausedByUs = false;
		bool waiting = false;
		bool acting = false;
	};
	struct Orphan {
		base::weak_ptr<Main::Session> session;
		PeerId peer = 0;
		uint64 sid = 0;
		QString title;
	};
	enum class Lookup {
		Pending,
		Missing,
	};
	using Key = std::pair<Main::Session*, PeerId>;

	void watch(not_null<Main::Session*> session);
	void sessionGone(not_null<Main::Session*> session);
	void messageChanged(not_null<HistoryItem*> item);
	void messageRemoved(not_null<const HistoryItem*> item);
	void messageLost(FullMsgId id, Main::Session *session, bool removed);
	void observe(
		not_null<HistoryItem*> item,
		const State &state,
		bool replace);
	void changed();

	void playerUpdated();
	void playerTrackChanged();
	void playerSeeked();
	void playerClosed();
	void settingsChanged();

	void probeClock(not_null<Main::Session*> session);
	void probeSend();

	void markDead(uint64 sid);
	[[nodiscard]] bool dead(uint64 sid) const;
	void orphanEnd(not_null<HistoryItem*> item, const State &state);

	void hostBegin(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		FullMsgId track,
		bool sends);
	void hostAdvance();
	void hostPost(const Track &track);
	void hostRefresh(bool settled = false);
	void hostSchedule();
	void hostSend();
	void hostEditDone(uint64 sid);
	void hostEditFailed(uint64 sid, const QString &type);
	void hostEnd();
	void hostFail(const QString &text);
	void hostClear();
	void hostUpdateProgress();
	[[nodiscard]] Track hostResolveTrack();
	[[nodiscard]] HistoryItem *hostLocate(not_null<DocumentData*> document);
	[[nodiscard]] HistoryItem *hostSendable(DocumentId id) const;
	void hostUpdateAway(const Track &track);
	void hostTellAway();
	void hostForward(not_null<HistoryItem*> item, DocumentId id);
	void hostShareDone(uint64 sid);
	void hostShareFailed(uint64 sid, const QString &type);
	void hostShareWaits();
	void hostShareCancel(not_null<Host*> host);
	void hostAdopt(not_null<HistoryItem*> item, const State *state);
	[[nodiscard]] Sample hostSample() const;
	[[nodiscard]] std::vector<Track> hostQueue(const Track &track) const;
	[[nodiscard]] QString hostTitleFor(const Track &track) const;

	void listenerApply(const State &state);
	void listenerSync();
	void listenerHold();
	[[nodiscard]] std::shared_ptr<LookupBook> listenerBook(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		FullMsgId message);
	[[nodiscard]] HistoryItem *listenerLocate(const Track &track);
	[[nodiscard]] Lookup listenerRequest(const Track &track, bool urgent);
	void listenerFetched(
		const std::weak_ptr<LookupBook> &weak,
		Track track);
	void listenerLookupSchedule();
	void listenerLookupSend();
	void listenerLookupDone(
		uint64 serial,
		const MTPmessages_Messages &result);
	void listenerLookupFailed(uint64 serial, const QString &type);
	void listenerLookupFinish(bool found);
	void tracksAdopt(not_null<HistoryItem*> item);
	void listenerPlay(
		not_null<DocumentData*> document,
		FullMsgId id,
		int64 position);
	void listenerSeek(int64 target, bool measure);
	void listenerResume(int64 expected);
	void listenerPause();
	void listenerPreloadNext();
	void listenerPlayerUpdated();
	void listenerArm();
	void listenerCheck();
	void listenerFinish(Finish reason);

	base::flat_map<not_null<Main::Session*>, std::unique_ptr<Watch>> _watches;
	std::map<Key, Observed> _observed;
	std::unique_ptr<Host> _host;
	std::unique_ptr<Listener> _listener;
	std::map<Key, std::shared_ptr<LookupBook>> _books;
	LookupPace _lookupPace;
	std::vector<Orphan> _orphans;
	std::deque<uint64> _dead;
	uint64 _listenerSerial = 0;
	base::weak_ptr<Main::Session> _probeSession;
	int _probesLeft = 0;
	bool _probing = false;
	base::Timer _hostTimer;
	base::Timer _hostStartTimer;
	base::Timer _hostShareTimeout;
	base::Timer _hostSettleTimer;
	base::Timer _hostIdleTimer;
	base::Timer _hostEndTimer;
	base::Timer _listenerTimer;
	base::Timer _listenerLookupTimer;
	base::Timer _probeTimer;
	rpl::event_stream<> _changes;
	bool _changedScheduled = false;
	rpl::lifetime _lifetime;

};

Manager *ManagerInstance = nullptr;

[[nodiscard]] bool PeerFits(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return !user->isSelf()
			&& !user->isBot()
			&& !user->isInaccessible()
			&& !user->isServiceUser()
			&& !user->isRepliesChat()
			&& !user->isVerifyCodes();
	}
	return (peer->isChat() || peer->isMegagroup())
		&& !peer->isForum()
		&& !peer->isMonoforum();
}

Manager::Manager()
: _hostTimer([=] { hostSchedule(); })
, _hostStartTimer([=] {
	hostFail(tr::lng_oblivion_listen_error_start(tr::now));
})
, _hostShareTimeout([=] { hostShareWaits(); })
, _hostSettleTimer([=] { hostRefresh(true); })
, _hostIdleTimer([=] { hostEnd(); })
, _hostEndTimer([=] { hostClear(); })
, _listenerTimer([=] { listenerCheck(); })
, _listenerLookupTimer([=] { listenerLookupSchedule(); })
, _probeTimer([=] { probeSend(); }) {
	const auto player = Media::Player::instance();
	player->updatedNotifier(
	) | rpl::filter([](const Media::Player::TrackState &state) {
		return (state.id.type() == SongType::Song);
	}) | rpl::on_next([=] {
		playerUpdated();
	}, _lifetime);

	player->trackChanged(
	) | rpl::filter([](AudioMsgId::Type type) {
		return (type == SongType::Song);
	}) | rpl::on_next([=] {
		playerTrackChanged();
	}, _lifetime);

	player->stops(
		SongType::Song
	) | rpl::on_next([=] {
		playerUpdated();
	}, _lifetime);

	player->seekingChanges(
		SongType::Song
	) | rpl::filter([](Media::Player::Instance::Seeking seeking) {
		return (seeking == Media::Player::Instance::Seeking::Finish);
	}) | rpl::on_next([=] {
		playerSeeked();
	}, _lifetime);

	player->closePlayerRequests(
	) | rpl::on_next([=] {
		playerClosed();
	}, _lifetime);

	Get().changes(
	) | rpl::on_next([=] {
		settingsChanged();
	}, _lifetime);
}

Manager &Manager::Instance() {
	if (!ManagerInstance) {
		ManagerInstance = new Manager();
	}
	return *ManagerInstance;
}

Manager *Manager::Existing() {
	return ManagerInstance;
}

void Manager::changed() {
	if (_changedScheduled) {
		return;
	}
	_changedScheduled = true;
	crl::on_main(this, [=] {
		_changedScheduled = false;
		_changes.fire({});
	});
}

void Manager::watch(not_null<Main::Session*> session) {
	if (_watches.contains(session)) {
		return;
	}
	const auto raw = _watches.emplace(
		session,
		std::make_unique<Watch>()).first->second.get();

	using Flag = Data::MessageUpdate::Flag;
	session->changes().messageUpdates(
		Flag::Edited | Flag::NewAdded | Flag::Destroyed
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		if (update.flags & Flag::Destroyed) {
			messageRemoved(update.item);
		} else {
			messageChanged(update.item);
		}
	}, raw->lifetime);

	session->data().itemIdChanged(
	) | rpl::on_next([=](Data::Session::IdChange change) {
		const auto item = session->data().message(change.newId);
		if (!item) {
			return;
		}
		const auto host = _host.get();
		const auto own = host
			&& (host->phase == Host::Phase::Posting)
			&& (host->session.get() == session)
			&& (host->localId == change.oldId)
			&& (change.newId.peer == host->peer->id);
		messageChanged(item);

		// The message is in the chat, but the server has not kept the
		// address with the state in it: nobody could join such a session.
		// A message sent "without going online" is only scheduled at
		// this moment, it comes as a new one when the server delivers it.
		if (own
			&& item->isRegular()
			&& IsServerMsgId(item->id)
			&& _host
			&& _host->phase == Host::Phase::Posting) {
			hostFail(tr::lng_oblivion_listen_error_start(tr::now));
		}
	}, raw->lifetime);

	session->data().itemDataChanges(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		// The control message could not be sent at all.
		const auto host = _host.get();
		if (host
			&& (host->phase == Host::Phase::Posting)
			&& (host->session.get() == session)
			&& (item->id == host->localId)
			&& (item->history()->peer == host->peer)
			&& item->hasFailed()) {
			hostFail(tr::lng_oblivion_listen_error_start(tr::now));
		}
	}, raw->lifetime);

	session->account().sessionChanges(
	) | rpl::on_next([=] {
		sessionGone(session);
	}, raw->lifetime);
}

void Manager::sessionGone(not_null<Main::Session*> session) {
	// The account logs out or the app quits: nothing can be sent any
	// more, only what points into the session is dropped.
	if (_host && _host->session.get() == session.get()) {
		_host->quitting = false;
		hostClear();
	}
	if (_listener && _listener->session.get() == session.get()) {
		listenerFinish(Finish::Quiet);
	}
	for (auto i = begin(_observed); i != end(_observed);) {
		if (i->first.first == session.get()) {
			i = _observed.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = begin(_books); i != end(_books);) {
		if (i->first.first == session.get()) {
			i = _books.erase(i);
		} else {
			++i;
		}
	}
	_orphans.erase(
		ranges::remove_if(_orphans, [&](const Orphan &orphan) {
			const auto strong = orphan.session.get();
			return !strong || (strong == session.get());
		}),
		end(_orphans));
	if (_probeSession.get() == session.get()) {
		_probeTimer.cancel();
		_probesLeft = 0;
		_probing = false;
	}
	changed();
	_watches.remove(session);
}

// The offset of the clock is measured with a few small requests, a bit
// more than a second apart so that their answers come at different parts
// of a second (see ClockSync): when a session is being started or joined
// and while it lasts, five requests not more often than once in ten
// minutes (once in half a minute while nothing is measured yet).
void Manager::probeClock(not_null<Main::Session*> session) {
	auto &clock = Clock();
	const auto now = crl::now();
	const auto each = clock.sync.ready() ? kProbeEach : kProbeRetry;
	if (_probing
		|| (_probesLeft > 0)
		|| (clock.probedAt && (now - clock.probedAt < each))) {
		return;
	}
	clock.probedAt = now;
	_probeSession = base::make_weak(session);
	_probesLeft = kProbeCount;
	probeSend();
}

void Manager::probeSend() {
	const auto session = _probeSession.get();
	if (_probing) {
		return;
	} else if (!session || _probesLeft <= 0) {
		_probesLeft = 0;
		return;
	}
	--_probesLeft;
	_probing = true;
	const auto sent = WallMs();
	const auto weak = base::make_weak(this);
	session->api().request(MTPupdates_GetState(
	)).done([=](const MTPupdates_State &, const MTP::Response &response) {
		ClockSample(sent, response.outerMsgId);
		if (const auto strong = weak.get()) {
			strong->_probing = false;
			if (strong->_probesLeft > 0) {
				strong->_probeTimer.callOnce(kProbeGap);
			}
		}
	}).fail([=] {
		if (const auto strong = weak.get()) {
			strong->_probing = false;
			strong->_probesLeft = 0;
		}
	}).send();
}

void Manager::markDead(uint64 sid) {
	if (dead(sid)) {
		return;
	} else if (int(_dead.size()) >= kMaxDead) {
		_dead.pop_front();
	}
	_dead.push_back(sid);
}

// The sessions that this app has ended or has given up while their
// message was on its way: whatever comes about them later, they are
// never offered to join.
bool Manager::dead(uint64 sid) const {
	return ranges::contains(_dead, sid);
}

void Manager::observe(
		not_null<HistoryItem*> item,
		const State &state,
		bool replace) {
	const auto peer = item->history()->peer;
	const auto key = Key(&peer->session(), peer->id);
	const auto id = item->fullId();
	auto fresh = Observed{ id, state, item->from()->id };
	if (dead(state.session)) {
		fresh.state.ended = true;
	}
	const auto i = _observed.find(key);
	if (i == end(_observed)) {
		_observed.emplace(key, std::move(fresh));
		return;
	}
	// One session is remembered for a chat, the newest one, but a live
	// session is never replaced by a finished one of another message.
	const auto now = NowMs();
	const auto newer = (i->second.message.msg < id.msg);
	const auto wasLive = (LiveLeft(i->second.state, now) > 0);
	const auto nowLive = (LiveLeft(fresh.state, now) > 0);
	if (replace
		|| (i->second.message == id)
		|| (nowLive ? (newer || !wasLive) : (newer && !wasLive))) {
		i->second = std::move(fresh);
	}
}

void Manager::messageChanged(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const auto id = item->fullId();
	const auto state = ParseItem(item);
	if (_host) {
		hostAdopt(item, state ? &*state : nullptr);
	}
	tracksAdopt(item);
	if (!state) {
		messageLost(id, &peer->session(), false);
		return;
	} else if (!PeerFits(peer)) {
		return;
	}
	if (item->out() && IsServerMsgId(item->id) && !_orphans.empty()) {
		orphanEnd(item, *state);
	}
	observe(item, *state, false);
	if (_listener && _listener->message == id) {
		listenerApply(*state);
	}
	changed();
}

void Manager::messageRemoved(not_null<const HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const auto session = &item->history()->session();
	messageLost(item->fullId(), session, true);

	// The track of the hosted session may have been deleted from the
	// chat: looked at when the message is really gone.
	const auto host = _host.get();
	if (host
		&& host->phase == Host::Phase::Live
		&& host->peer == peer
		&& host->session.get() == session
		&& item->media()
		&& item->media()->document()) {
		const auto sid = host->sid;
		crl::on_main(this, [=] {
			if (_host && _host->sid == sid) {
				hostRefresh();
			}
		});
	}
}

// The message of a session that this app has given up while the message
// was being sent (the host has pressed "Cancel", the start has taken too
// long) has got to the chat after all: it is edited to "ended" at once.
// One request for one message, never repeated whatever the answer is.
void Manager::orphanEnd(not_null<HistoryItem*> item, const State &state) {
	const auto peer = item->history()->peer;
	const auto session = &peer->session();
	const auto i = ranges::find_if(_orphans, [&](const Orphan &orphan) {
		return (orphan.session.get() == session)
			&& (orphan.peer == peer->id)
			&& (orphan.sid == state.session);
	});
	if (i == end(_orphans)) {
		return;
	}
	const auto title = i->title;
	_orphans.erase(i);
	if (state.ended) {
		return;
	}
	const auto now = NowMs();
	auto ended = state;
	ended.seq = state.seq + 1;
	ended.position = ExpectedPosition(state, now);
	ended.at = now;
	ended.playing = false;
	ended.ended = true;
	ended.away = false;
	ended.queue.clear();
	const auto api = &session->api();
	api->request(EditRequest(
		session,
		peer,
		item->id,
		MakeControlText(VisibleText(title, true), ended)
	)).done([=](const MTPUpdates &result) {
		api->applyUpdates(result);
	}).fail([] {
	}).handleFloodErrors().send();
}

// The message is deleted (removed) or it is not a control message any
// more: the server has not kept the address with the state, or it was
// deleted and is only kept on the screen by "keep deleted messages".
void Manager::messageLost(
		FullMsgId id,
		Main::Session *session,
		bool removed) {
	const auto i = _observed.find(Key(session, id.peer));
	if (i != end(_observed) && i->second.message == id) {
		_observed.erase(i);
		changed();
	}
	if (_host
		&& _host->message == id
		&& _host->session.get() == session) {
		if (!removed) {
			Toast(session, tr::lng_oblivion_listen_error_edit(tr::now));
		}
		hostClear();
	}
	if (_listener
		&& _listener->message == id
		&& _listener->session.get() == session) {
		listenerFinish(Finish::Ended);
	}
}

bool Manager::rescan(not_null<PeerData*> peer) {
	if (!Get().listenTogether() || !PeerFits(peer)) {
		return false;
	}
	const auto session = &peer->session();
	watch(session);
	if (_observed.contains(Key(session, peer->id))) {
		return false;
	}
	const auto history = session->data().historyLoaded(peer);
	if (!history) {
		return false;
	}
	auto left = kScanLimit;
	for (auto i = history->blocks.rbegin()
		; i != history->blocks.rend()
		; ++i) {
		const auto &messages = (*i)->messages;
		for (auto j = messages.rbegin(); j != messages.rend(); ++j) {
			const auto item = (*j)->data();
			if (const auto state = ParseItem(item)) {
				observe(item, *state, true);
				return true;
			} else if (!--left) {
				return false;
			}
		}
	}
	return false;
}

ChatView Manager::lookup(not_null<PeerData*> peer) {
	using Kind = ChatView::Kind;
	using Sync = ChatView::Sync;

	auto result = ChatView();
	if (!Get().listenTogether() || !PeerFits(peer)) {
		return result;
	}
	const auto session = &peer->session();
	if (const auto host = _host.get()) {
		if (host->peer == peer && host->session.get() == session) {
			if (host->phase != Host::Phase::Ending) {
				const auto live = (host->phase == Host::Phase::Live);
				result.kind = live ? Kind::Hosting : Kind::Starting;
				result.track = host->title;
				result.playing = host->desired.playing;
				if (live && host->away) {
					// The button is offered when a click on it can send
					// the track at once: nothing else is on its way to
					// the chat and the slow mode of a group lets it in
					// (the bar looks again when its countdown is over).
					const auto busy = host->sharing || host->shareRequest;
					const auto wait = (host->awaySendable && !busy)
						? peer->slowmodeSecondsLeft()
						: 0;
					result.sync = Sync::Away;
					result.track = host->awayTitle;
					result.sending = (host->sharing == host->away);
					result.canSend = host->awaySendable && !busy && !wait;
					result.sendId = host->away;
					result.staleIn = wait
						? (crl::time(wait) * 1000 + 200)
						: crl::time(0);
				}
			}
			return result;
		}
	}
	if (const auto listener = _listener.get()) {
		if (listener->peer == peer && listener->session.get() == session) {
			const auto host = session->data().peerLoaded(listener->host);
			const auto &state = listener->state;
			result.kind = Kind::Joined;
			result.track = listener->title;
			result.host = host ? host->shortName() : QString();
			result.playing = state.playing;
			result.sync = (state.away && !state.playing)
				? Sync::Away
				: listener->unavailable
				? Sync::Unavailable
				: listener->loading
				? Sync::Loading
				: listener->ownPause
				? Sync::OwnPause
				: listener->waiting
				? Sync::Waiting
				: (state.playing && listener->corrector.loading())
				? Sync::Loading
				: Sync::Fine;
			return result;
		}
	}
	const auto i = _observed.find(Key(session, peer->id));
	if (i == end(_observed)) {
		return result;
	}
	auto &observed = i->second;
	const auto left = LiveLeft(observed.state, NowMs());
	if (left <= 0) {
		return result;
	}
	const auto host = session->data().peerLoaded(observed.host);
	const auto wanted = observed.state.track.document;
	const auto fits = [&](HistoryItem *item) {
		const auto document = item ? ChatTrackDocument(item) : nullptr;
		return document && (document->id == wanted);
	};
	auto track = observed.track
		? session->data().message(peer->id, observed.track)
		: nullptr;
	if (!fits(track)) {
		track = LocateLoaded(peer, observed.state.track);
		observed.track = track ? track->id : MsgId(0);
	}
	const auto document = track ? TrackDocument(track) : nullptr;
	result.kind = Kind::Offer;
	result.sync = (observed.state.away && !observed.state.playing)
		? Sync::Away
		: Sync::Fine;
	result.track = document ? TrackTitle(document) : QString();
	result.host = host ? host->shortName() : QString();
	result.playing = observed.state.playing;
	result.staleIn = crl::time(left) + 200;
	return result;
}

void Manager::settingsChanged() {
	if (Get().listenTogether()) {
		return;
	}
	if (_listener) {
		listenerFinish(Finish::Quiet);
	}
	if (_host) {
		hostEnd();
	}
	changed();
}

void Manager::playerUpdated() {
	if (_host) {
		hostUpdateProgress();
		hostRefresh();
	}
	if (_listener) {
		listenerPlayerUpdated();
	}
}

void Manager::playerTrackChanged() {
	if (const auto listener = _listener.get()) {
		if (!listener->acting && listener->playing) {
			const auto current = Media::Player::instance()->current(
				SongType::Song);
			const auto id = current.contextId();
			if (current.audio()
				&& id != listener->playing
				&& (!listener->track || id != listener->track)) {
				// The listener has started another song himself.
				listenerFinish(Finish::Left);
			}
		}
	}
	if (_host) {
		hostUpdateProgress();
		hostRefresh();
	}
}

void Manager::playerSeeked() {
	if (const auto host = _host.get()) {
		host->progress.id = FullMsgId();
		hostUpdateProgress();
	}
}

// The player is being closed. It also closes by itself when a voice or
// a video message is over and no song is loaded or the song has stopped
// (MainWidget, tracksFinished): that ends nothing. Only closing a player
// that holds a song, playing or paused, is the wish of the user to stop.
void Manager::playerClosed() {
	const auto player = Media::Player::instance();
	const auto state = player->getState(SongType::Song);
	if (!state.id || Media::Player::IsStoppedOrStopping(state.state)) {
		return;
	}
	if (const auto listener = _listener.get()) {
		const auto current = player->current(SongType::Song);
		if (listener->playing && current.contextId() == listener->playing) {
			listenerFinish(Finish::Left);
		}
	}
	if (_host) {
		hostEnd();
	}
}

void Manager::start(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		FullMsgId track) {
	const auto session = &controller->session();
	const auto item = session->data().message(track);
	const auto document = item ? TrackDocument(item) : nullptr;
	const auto fits = (&peer->session() == session) && PeerFits(peer);
	const auto there = fits
		&& document
		&& ((item->history()->peer == peer && ChatTrackDocument(item))
			|| LocateLoaded(peer, Track{ .document = document->id }));
	const auto sends = document && !there;
	const auto error = !Get().listenTogether()
		? tr::lng_oblivion_listen_error_off(tr::now)
		: !fits
		? tr::lng_oblivion_listen_error_chat(tr::now)
		: !Data::CanSendTexts(peer)
		? tr::lng_oblivion_listen_error_rights(tr::now)
		: (peer->starsPerMessageChecked() > 0)
		? tr::lng_oblivion_listen_error_paid(tr::now)
		: (!document || !item->isRegular())
		? tr::lng_oblivion_listen_error_track(tr::now)
		: _host
		? tr::lng_oblivion_listen_error_hosting(tr::now)
		: (sends && (!CanSendTrackTo(peer) || !Shareable(item)))
		? tr::lng_oblivion_listen_error_send_track(tr::now)
		: SlowmodeError(peer, sends ? 2 : 1);
	if (!error.isEmpty()) {
		controller->showToast(error);
		return;
	}
	watch(session);
	probeClock(session);
	const auto weak = base::make_weak(controller);
	controller->show(MakeStartBox({
		.chat = peer->name(),
		.track = TrackTitle(document),
		.sendsTrack = sends,
		.onlineNote = Get().offlineSend(),
		.confirmed = crl::guard(this, [=] {
			const auto strong = weak.get();
			if (!strong) {
				return;
			} else if (_host) {
				strong->showToast(
					tr::lng_oblivion_listen_error_hosting(tr::now));
				return;
			}
			hostBegin(&strong->session(), peer, track, sends);
		}),
	}));
}

// sends: the box has said that the track will be sent to the chat and
// the host has pressed "Start". Without it nothing but the control
// message is sent.
void Manager::hostBegin(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		FullMsgId track,
		bool sends) {
	const auto item = session->data().message(track);
	const auto document = item ? TrackDocument(item) : nullptr;
	if (!document || !Get().listenTogether() || !PeerFits(peer)) {
		Toast(session, tr::lng_oblivion_listen_error_track(tr::now));
		return;
	}
	if (_listener) {
		listenerFinish(Finish::Quiet);
	}
	watch(session);
	probeClock(session);

	_host = std::make_unique<Host>();
	_host->session = base::make_weak(session);
	_host->peer = peer;
	_host->sid = (base::RandomValue<uint64>() & kSessionMask) | 1;
	_host->title = TrackTitle(document);
	_host->start = track;
	_host->startSends = sends;

	const auto player = Media::Player::instance();
	const auto current = player->current(SongType::Song);
	const auto state = player->getState(SongType::Song);
	if (current.audio() != document
		|| current.contextId() != track
		|| !state.id
		|| Media::Player::IsStopped(state.state)) {
		player->play(AudioMsgId(document, track));
	}
	_hostStartTimer.callOnce(kStartTimeout);
	changed();

	// A moment for the player to show whether the track really plays,
	// so that the very first state in the message is already right.
	const auto sid = _host->sid;
	base::call_delayed(kStartDelay, this, [=] {
		if (_host && _host->sid == sid) {
			hostAdvance();
		}
	});
}

void Manager::hostAdvance() {
	const auto host = _host.get();
	if (!host) {
		return;
	} else if (host->phase == Host::Phase::Sharing) {
		// The session starts when the track that the host has chosen is
		// a message of the chat. If it is not, it is sent there once,
		// that very message, and only if the start box has said so.
		const auto session = host->session.get();
		const auto item = session
			? session->data().message(host->start)
			: nullptr;
		const auto document = item ? TrackDocument(item) : nullptr;
		if (!document) {
			hostFail(tr::lng_oblivion_listen_error_track(tr::now));
			return;
		}
		const auto there = (item->history()->peer == host->peer
			&& ChatTrackDocument(item))
			? item
			: hostLocate(document);
		const auto sends = !there
			&& host->startSends
			&& !host->startSent
			&& Shareable(item)
			&& CanSendTrackTo(host->peer);
		if (!there && (host->sharing || host->shareRequest)) {
			return;
		} else if (!there && !sends) {
			hostFail(tr::lng_oblivion_listen_error_send_track(tr::now));
			return;
		}
		// The slow mode of a group may have started its countdown since
		// the start box was shown: a message that the server refuses is
		// not sent at all, the track does not go without its session.
		const auto slowmode = SlowmodeError(host->peer, sends ? 2 : 1);
		if (!slowmode.isEmpty()) {
			hostFail(slowmode);
		} else if (there) {
			host->phase = Host::Phase::Posting;
			hostPost(TrackOf(there, document));
		} else {
			host->startSent = true;
			hostForward(item, document->id);
		}
	} else if (host->phase == Host::Phase::Live) {
		if (!host->verified) {
			// Read back once, when the answer of the server has been
			// applied to the message: if the server has not kept the
			// address with the state, nobody could ever join.
			host->verified = true;
			const auto session = host->session.get();
			const auto item = session
				? session->data().message(host->message)
				: nullptr;
			const auto state = item ? ParseItem(item) : std::nullopt;
			if (!state || state->session != host->sid) {
				hostFail(tr::lng_oblivion_listen_error_start(tr::now));
				return;
			}
		}
		hostRefresh();
	}
}

// The message of the session chat with the song that the player has
// now, an empty track if the chat has none. The host may play the song
// from that chat or from anywhere else (another chat, a playlist): what
// matters is that the chat has a message with the same file, so that
// the listeners can play it. Nothing is ever sent from here.
Track Manager::hostResolveTrack() {
	const auto host = _host.get();
	const auto session = host->session.get();
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto document = current.audio();
	if (!session || !document || (&document->session() != session)) {
		return {};
	}
	const auto id = current.contextId();
	if (id.peer == host->peer->id && IsServerMsgId(id.msg)) {
		const auto item = session->data().message(id);
		if (item && ChatTrackDocument(item) == document) {
			return TrackOf(item, document);
		}
	}
	if (const auto item = hostLocate(document)) {
		return TrackOf(item, document);
	}
	return {};
}

HistoryItem *Manager::hostLocate(not_null<DocumentData*> document) {
	const auto host = _host.get();
	const auto session = host->session.get();
	if (!session) {
		return nullptr;
	}
	const auto id = document->id;
	const auto i = host->known.find(id);
	if (i != end(host->known)) {
		const auto item = session->data().message(host->peer->id, i->second);
		if (item && ChatTrackDocument(item) == document) {
			return item;
		}
		host->known.erase(i);
	} else if (host->absent.contains(id)) {
		return nullptr;
	}
	const auto item = LocateLoaded(host->peer, Track{ .document = id });
	if (item) {
		host->known.emplace(id, item->id);
	} else {
		host->absent.emplace(id);
	}
	return item;
}

// The message from which the player plays the file id, if the player
// has that file now and the message may be forwarded to the session
// chat without the name of its author.
HistoryItem *Manager::hostSendable(DocumentId id) const {
	const auto host = _host.get();
	const auto session = host->session.get();
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto document = current.audio();
	if (!session
		|| !document
		|| document->id != id
		|| (&document->session() != session)) {
		return nullptr;
	}
	const auto item = session->data().message(current.contextId());
	return (item
		&& TrackDocument(item) == document
		&& Shareable(item)
		&& CanSendTrackTo(host->peer))
		? item
		: nullptr;
}

// What the bar of the host says while the player has a song that is not
// a message of the chat, and whether "Send to chat" is offered for it.
void Manager::hostUpdateAway(const Track &track) {
	const auto host = _host.get();
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto document = current.audio();
	const auto away = (!track.id && document) ? document->id : DocumentId(0);
	if (host->away == away) {
		return;
	}
	host->away = away;
	host->awayTitle = away ? TrackTitle(document) : QString();
	host->awaySendable = away && (hostSendable(away) != nullptr);
	changed();
}

void Manager::hostTellAway() {
	const auto host = _host.get();
	if (!host->away) {
		return;
	}
	Toast(
		host->session.get(),
		tr::lng_oblivion_listen_toast_away(
			tr::now,
			lt_track,
			host->awayTitle,
			lt_chat,
			host->peer->name()));
}

// "Send to chat" in the bar of the host: the track named there, the one
// the player has now, is forwarded to the session chat. This and the
// start of a session with a track from another chat are the only places
// where a track is sent, each by a click for that very track.
void Manager::sendTrack(DocumentId id) {
	const auto host = _host.get();
	if (!host || host->phase != Host::Phase::Live || !host->away) {
		return;
	} else if (host->away != id || host->sharing || host->shareRequest) {
		// The bar has named another track or has offered the button
		// while a track is on its way, it is right in a moment.
		changed();
		return;
	}
	const auto slowmode = SlowmodeError(host->peer, 1);
	if (!slowmode.isEmpty()) {
		Toast(host->session.get(), slowmode);
		changed();
		return;
	}
	const auto item = hostSendable(host->away);
	if (item) {
		hostForward(item, host->away);
		return;
	}
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto document = current.audio();
	if (document && document->id == host->away) {
		host->awaySendable = false;
		Toast(
			host->session.get(),
			tr::lng_oblivion_listen_error_send_track(tr::now));
		changed();
	} else {
		// The player has gone to another track since the bar was shown.
		hostRefresh();
	}
}

// The forward of one message, without the name of its author and without
// its caption (see Shareable). The request is made here and not by
// ApiWrap::forwardMessages(), which tells nothing about what became of
// it: this way the manager knows whether the server has taken the track,
// has refused it or has not answered yet, so the button is offered once
// more only when nothing was sent and nothing is on its way, and
// a request that still waits for the connection is taken back when the
// session is over. It goes through the queue of the sends of the chat as
// any other send, "send without going online" applies to it, and the
// chat is not marked as read by it: the host may not be looking at it.
void Manager::hostForward(not_null<HistoryItem*> item, DocumentId id) {
	using Flag = MTPmessages_ForwardMessages::Flag;

	const auto host = _host.get();
	const auto session = host->session.get();
	if (!session) {
		return;
	}
	const auto history = session->data().history(host->peer);
	const auto scheduled = OfflineScheduleFor(
		history,
		OfflineShareOptions(history, Api::SendOptions(), QString(), 1));
	const auto flags = Flag::f_drop_author
		| Flag::f_drop_media_captions
		| (scheduled ? Flag::f_schedule_date : Flag(0));
	const auto from = item->history()->peer;
	const auto source = item->id;
	const auto randomId = base::RandomValue<uint64>();
	const auto sid = host->sid;
	const auto weak = base::make_weak(this);
	host->sharing = id;
	host->shareRequest = session->data().histories().sendPreparedMessage(
		history,
		FullReplyTo(),
		uint64(0),
		[=](not_null<History*> to, FullReplyTo)
		-> Data::Histories::PreparedMessage {
			return MTPmessages_ForwardMessages(
				MTP_flags(flags),
				from->input(),
				MTP_vector<MTPint>(1, MTP_int(source)),
				MTP_vector<MTPlong>(1, MTP_long(randomId)),
				to->peer->input(),
				MTP_int(0), // top_msg_id
				MTPInputReplyTo(),
				MTP_int(scheduled),
				MTP_int(0), // schedule_repeat_period
				MTP_inputPeerEmpty(), // send_as
				MTPInputQuickReplyShortcut(),
				MTP_long(0), // effect
				MTP_int(0), // video_timestamp
				MTP_long(0), // allow_paid_stars
				MTPSuggestedPost());
		},
		[=](const MTPUpdates &, const MTP::Response &) {
			if (const auto strong = weak.get()) {
				strong->hostShareDone(sid);
			}
		},
		[=](const MTP::Error &error, const MTP::Response &) {
			if (const auto strong = weak.get()) {
				strong->hostShareFailed(sid, error.type());
			}
		});
	_hostShareTimeout.callOnce(kStartTimeout);
	changed();
}

// The server has taken the forward. Its copy is in the chat by now or
// comes in a moment (a message sent "without going online" is delivered
// by the server a bit later), hostAdopt() sees it. Nothing more is sent
// for this file meanwhile, the chat has it.
void Manager::hostShareDone(uint64 sid) {
	const auto host = _host.get();
	if (!host || host->sid != sid) {
		return;
	}
	host->shareRequest = 0;
	_hostShareTimeout.cancel();
	changed();
}

// The server has refused the forward: nothing was sent and nothing is on
// its way, so the track is offered to be sent once more, by another
// click. The refusals that need more than a toast (the account is limited
// in sending, the stars) are shown as upstream shows them for any send.
void Manager::hostShareFailed(uint64 sid, const QString &type) {
	const auto host = _host.get();
	if (!host || host->sid != sid) {
		return;
	}
	host->shareRequest = 0;
	host->sharing = 0;
	_hostShareTimeout.cancel();

	const auto session = host->session.get();
	const auto explained = SendRefusalExplained(type);
	if (session && (explained || SendRefusalSlowmode(type))) {
		session->api().sendMessageFail(type, host->peer);
	}
	const auto slowmode = SendRefusalSlowmode(type)
		? SlowmodeError(host->peer, 1)
		: QString();
	if (host->phase == Host::Phase::Sharing) {
		hostFail(!slowmode.isEmpty()
			? slowmode
			: tr::lng_oblivion_listen_error_send_track(tr::now));
		return;
	}
	Toast(session, !slowmode.isEmpty()
		? slowmode
		: explained
		? QString()
		: tr::lng_oblivion_listen_error_share_failed(tr::now));
	changed();
}

// A minute and a half without an answer about the forward: there is no
// connection, the request waits for it and goes when it is back. Nothing
// is offered to be pressed again till then, the chat would get the track
// twice. A session that is only being started gives up by its own timer
// and takes the request back.
void Manager::hostShareWaits() {
	const auto host = _host.get();
	if (!host || !host->shareRequest || host->phase != Host::Phase::Live) {
		return;
	}
	Toast(
		host->session.get(),
		tr::lng_oblivion_listen_error_share_later(tr::now));
}

// The session is over or is not started after all: a forward that the
// server has not got yet is not sent later, without its session.
void Manager::hostShareCancel(not_null<Host*> host) {
	_hostShareTimeout.cancel();
	host->sharing = 0;
	if (const auto id = base::take(host->shareRequest)) {
		if (const auto session = host->session.get()) {
			session->data().histories().cancelRequest(id);
		}
	}
}

// A message of the session chat has come or has changed: a music file
// (any, from anybody: the chat has that track now) or the control
// message of this session.
void Manager::hostAdopt(not_null<HistoryItem*> item, const State *state) {
	const auto host = _host.get();
	if (!IsServerMsgId(item->id)
		|| item->history()->peer != host->peer
		|| (&item->history()->session() != host->session.get())) {
		return;
	}
	if (const auto document = ChatTrackDocument(item)) {
		const auto id = document->id;
		const auto awaited = (host->sharing == id);
		host->absent.remove(id);
		if (!host->known.contains(id)) {
			host->known.emplace(id, item->id);
		}
		if (awaited) {
			// The chat has the track. The request has done its work, or
			// somebody else has sent the same file there meanwhile and
			// the request, if it has not gone yet, is not needed.
			hostShareCancel(host);
		}
		if (awaited
			|| host->away == id
			|| host->phase == Host::Phase::Sharing) {
			const auto sid = host->sid;
			crl::on_main(this, [=] {
				if (_host && _host->sid == sid) {
					hostAdvance();
				}
			});
		}
	}
	if (item->out()
		&& host->phase == Host::Phase::Posting
		&& state
		&& state->session == host->sid) {
		host->message = item->fullId();
		host->phase = Host::Phase::Live;
		_hostStartTimer.cancel();
		const auto sid = host->sid;
		crl::on_main(this, [=] {
			if (_host && _host->sid == sid) {
				hostAdvance();
			}
		});
	}
}

QString Manager::hostTitleFor(const Track &track) const {
	const auto host = _host.get();
	const auto session = host->session.get();
	const auto item = session
		? session->data().message(host->peer->id, MsgId(track.id))
		: nullptr;
	const auto document = item ? TrackDocument(item) : nullptr;
	if (document) {
		return TrackTitle(document);
	}
	const auto current = Media::Player::instance()->current(SongType::Song);
	return current.audio() ? TrackTitle(current.audio()) : host->title;
}

Manager::Sample Manager::hostSample() const {
	const auto state = Media::Player::instance()->getState(SongType::Song);
	using Kind = Sample::Kind;
	return Sample{
		.kind = (!state.id
			? Kind::Idle
			: (state.state == PlayerState::StoppedAtEnd)
			? Kind::Finished
			: Media::Player::IsStopped(state.state)
			? Kind::Loading
			: Media::Player::ShowPauseIcon(state.state)
			? Kind::Playing
			: Kind::Paused),
		.position = ToMs(state.position, state.frequency),
		.duration = ToMs(state.length, state.frequency),
	};
}

void Manager::hostUpdateProgress() {
	const auto host = _host.get();
	const auto player = Media::Player::instance();
	const auto state = player->getState(SongType::Song);
	UpdateProgress(
		host->progress,
		state.id.contextId(),
		bool(state.id),
		Media::Player::ShowPauseIcon(state.state),
		ToMs(state.position, state.frequency),
		crl::now());
}

std::vector<Track> Manager::hostQueue(const Track &track) const {
	using namespace Storage;

	// The queue is known only while the player takes the songs from the
	// session chat in the order of the chat, as it does by default.
	const auto host = _host.get();
	const auto session = host->session.get();
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto &settings = Core::App().settings();
	const auto order = settings.playerOrderMode();
	const auto from = MsgId(track.id);
	if (!session
		|| current.contextId() != FullMsgId(host->peer->id, from)
		|| PlaylistDrivesPlayer(current)
		|| order == Media::OrderMode::Shuffle
		|| base::options::lookup<bool>(
			Media::Player::kOptionDisableAutoplayNext).value()) {
		return {};
	} else if (settings.playerRepeatMode() == Media::RepeatMode::One) {
		return { track };
	}
	const auto reverse = (order == Media::OrderMode::Reverse);
	const auto slice = session->storage().snapshot(SharedMediaQuery(
		SharedMediaKey(
			host->peer->id,
			MsgId(0),
			PeerId(0),
			SharedMediaType::MusicFile,
			from),
		reverse ? kMaxQueue : 0,
		reverse ? 0 : kMaxQueue));
	auto ids = std::vector<MsgId>();
	for (const auto &id : slice.messageIds) {
		if (reverse ? (id < from) : (id > from)) {
			ids.push_back(id);
		}
	}
	if (reverse) {
		std::reverse(begin(ids), end(ids));
	}
	auto result = std::vector<Track>();
	for (const auto &id : ids) {
		const auto item = session->data().message(host->peer->id, id);
		const auto document = item ? ChatTrackDocument(item) : nullptr;
		if (!document) {
			break;
		}
		result.push_back(TrackOf(item, document));
		if (int(result.size()) == kMaxQueue) {
			break;
		}
	}
	return result;
}

void Manager::hostPost(const Track &track) {
	const auto host = _host.get();
	const auto session = host->session.get();
	if (!session) {
		hostClear();
		return;
	}
	using Kind = Sample::Kind;
	const auto sample = hostSample();
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto same = current.audio()
		&& (current.audio()->id == track.document)
		&& (&current.audio()->session() == session);
	const auto playing = same
		&& (sample.kind == Kind::Playing)
		&& host->progress.moving
		&& NormalSpeed(current);
	const auto steady = playing || (same && (sample.kind == Kind::Paused));
	auto state = State{
		.session = host->sid,
		.seq = ++host->seq,
		.track = track,
		.position = steady ? sample.position : 0,
		.at = NowMs(),
		.duration = same ? sample.duration : 0,
		.playing = playing,
	};
	state.queue = hostQueue(track);
	host->title = hostTitleFor(track);
	host->desired = state;
	host->sending = state;

	const auto text = MakeControlText(VisibleText(host->title, false), state);
	auto action = Api::SendAction(session->data().history(host->peer));
	action.clearDraft = false;
	action.sendForwardDraft = false;
	auto message = Api::MessageToSend(action);
	message.textWithTags = TextWithTags{
		text.text,
		TextUtilities::ConvertEntitiesToTextTags(text.entities),
	};
	message.webPage.removed = true;
	host->localId = session->data().nextLocalMessageId();
	_hostStartTimer.callOnce(kStartTimeout);
	session->api().sendMessage(std::move(message), host->localId);

	// The first edit keeps the same distance from the message itself.
	host->throttle.sent(crl::now());
	host->throttle.done();
	changed();
}

// Compares what the player of the host does with what the listeners
// think it does (the state that is or will be in the message) and asks
// for an edit only when they differ: another track, play or pause,
// a position that is not where the time has brought it (a seek).
//
// A player that is between two states (a track is being loaded or waits
// for its data, has just finished, the player is empty, the host has
// gone to a song that is not in the chat) is given three seconds to
// settle: when a track ends the next one usually starts at once, and
// "paused at the end" followed by "the next track" would be two edits
// instead of one, the second one late by the two seconds between the
// edits. After that the listeners get a pause, with the reason if it is
// a song that the chat does not have.
void Manager::hostRefresh(bool settled) {
	using Kind = Sample::Kind;

	const auto host = _host.get();
	if (!host || host->phase != Host::Phase::Live) {
		return;
	}
	const auto track = hostResolveTrack();
	hostUpdateAway(track);
	const auto now = NowMs();
	const auto sample = hostSample();
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto playing = (sample.kind == Kind::Playing);
	const auto away = (host->away != 0);
	const auto steady = track.id
		&& NormalSpeed(current)
		&& ((sample.kind == Kind::Paused)
			|| (playing && host->progress.moving));
	auto next = host->desired;
	if (steady) {
		_hostSettleTimer.cancel();
		const auto switched = (next.track != track);
		if (HostMoved(
				next,
				track,
				playing,
				sample.position,
				sample.duration,
				now)) {
			next.track = track;
			next.position = sample.position;
			next.at = now;
			next.duration = sample.duration;
			next.playing = playing;
			next.away = false;
			next.queue = hostQueue(track);
			host->queueCheckedAt = crl::now();
			if (switched) {
				host->title = hostTitleFor(track);
			}
		} else if (crl::now() - host->queueCheckedAt >= kQueueCheckEach) {
			// What the player goes to next may change while the track
			// plays: the list of the music of the chat is loaded later
			// than the track starts, the host switches shuffle or
			// repeat, a new track is sent to the chat. The listeners
			// need to know it before this track ends.
			next.queue = hostQueue(track);
			host->queueCheckedAt = crl::now();
		}
	} else if (!settled) {
		if (!_hostSettleTimer.isActive()
			&& (next.playing || next.away != away)) {
			_hostSettleTimer.callOnce(kSettleDelay);
		}
	} else {
		if (next.playing) {
			// A track of the chat that says "playing" and stands still
			// waits for its data: the listeners stop where it stands,
			// not where the time would have brought it.
			const auto stalled = playing && (next.track == track);
			const auto expected = ExpectedPosition(next, now);
			next.position = stalled
				? std::min(expected, sample.position)
				: expected;
			next.at = now;
			next.playing = false;
			next.queue.clear();
		}
		if (away && !next.away) {
			hostTellAway();
		}
		next.away = away;
	}
	if (!SameContent(next, host->desired)) {
		host->desired = next;
		host->throttle.request();
		hostSchedule();
		changed();
	}
	if (host->desired.playing) {
		_hostIdleTimer.cancel();
	} else if (!_hostIdleTimer.isActive()) {
		_hostIdleTimer.callOnce(kHostPausedEnd);
	}
}

void Manager::hostSchedule() {
	const auto host = _host.get();
	if (!host) {
		return;
	} else if (host->throttle.broken()) {
		Toast(
			host->session.get(),
			tr::lng_oblivion_listen_error_edit(tr::now));
		hostClear();
		return;
	}
	const auto ending = (host->phase == Host::Phase::Ending);
	const auto delay = host->throttle.delay(crl::now(), ending);
	if (!delay) {
		return;
	} else if (*delay > 0) {
		_hostTimer.callOnce(*delay);
		if (ending && !host->quitting) {
			// The wait for the last edit starts when it can be sent.
			_hostEndTimer.callOnce(*delay + kEndTimeout);
		}
	} else {
		hostSend();
	}
}

void Manager::hostSend() {
	const auto host = _host.get();
	const auto session = host->session.get();
	if (!session || !host->message) {
		hostClear();
		return;
	}
	auto state = host->desired;
	state.session = host->sid;
	state.seq = ++host->seq;
	host->sending = state;
	host->throttle.sent(crl::now());

	probeClock(session);

	const auto sid = host->sid;
	const auto api = &session->api();
	const auto sent = WallMs();
	const auto weak = base::make_weak(this);
	host->requestId = api->request(EditRequest(
		session,
		host->peer,
		host->message.msg,
		MakeControlText(VisibleText(host->title, state.ended), state)
	)).done([=](const MTPUpdates &result, const MTP::Response &response) {
		ClockSample(sent, response.outerMsgId);
		api->applyUpdates(result);
		if (const auto strong = weak.get()) {
			strong->hostEditDone(sid);
		}
	}).fail([=](const MTP::Error &error) {
		if (const auto strong = weak.get()) {
			strong->hostEditFailed(sid, error.type());
		}
	}).handleFloodErrors().send();
}

void Manager::hostEditDone(uint64 sid) {
	const auto host = _host.get();
	if (!host || host->sid != sid) {
		return;
	}
	host->requestId = 0;
	host->throttle.done();
	if (host->sending.ended) {
		hostClear();
		return;
	} else if (!SameContent(host->sending, host->desired)) {
		host->throttle.request();
	}
	hostSchedule();
}

void Manager::hostEditFailed(uint64 sid, const QString &type) {
	const auto host = _host.get();
	if (!host || host->sid != sid) {
		return;
	}
	host->requestId = 0;
	if (type == u"MESSAGE_NOT_MODIFIED"_q) {
		hostEditDone(sid);
		return;
	}
	const auto ending = (host->phase == Host::Phase::Ending);
	const auto now = crl::now();
	if (MTP::IsFloodError(type)) {
		host->throttle.flood(now, FloodSeconds(type));
		if (ending && (host->quitting || FloodSeconds(type) > 15)) {
			hostClear();
			return;
		}
	} else if (ending
		|| type == u"MESSAGE_ID_INVALID"_q
		|| type == u"MESSAGE_EDIT_TIME_EXPIRED"_q
		|| type == u"MESSAGE_AUTHOR_REQUIRED"_q
		|| type == u"CHAT_WRITE_FORBIDDEN"_q
		|| type == u"CHAT_ADMIN_REQUIRED"_q
		|| type == u"CHANNEL_PRIVATE"_q
		|| type == u"USER_BANNED_IN_CHANNEL"_q
		|| type == u"PEER_ID_INVALID"_q) {
		if (!ending) {
			Toast(
				host->session.get(),
				tr::lng_oblivion_listen_error_edit(tr::now));
		}
		hostClear();
		return;
	} else {
		host->throttle.failed(now);
	}
	hostSchedule();
}

void Manager::hostEnd() {
	const auto host = _host.get();
	if (!host || host->phase == Host::Phase::Ending) {
		return;
	} else if (host->phase != Host::Phase::Live
		|| !host->message
		|| !host->session.get()) {
		hostClear();
		return;
	}
	const auto now = NowMs();
	host->phase = Host::Phase::Ending;
	host->desired.position = ExpectedPosition(host->desired, now);
	host->desired.at = now;
	host->desired.playing = false;
	host->desired.ended = true;
	host->desired.away = false;
	host->desired.queue.clear();
	host->throttle.request();
	hostShareCancel(host);
	_hostSettleTimer.cancel();
	_hostIdleTimer.cancel();
	_hostEndTimer.callOnce(kEndTimeout);

	// For this app the session is over at once, even if the last edit
	// does not get through: the own bar does not offer to join it,
	// whatever edit of the message comes later.
	markDead(host->sid);
	const auto i = _observed.find(
		Key(host->session.get(), host->message.peer));
	if (i != end(_observed) && i->second.message == host->message) {
		i->second.state.ended = true;
	}
	changed();
	hostSchedule();
}

void Manager::hostFail(const QString &text) {
	if (const auto host = _host.get()) {
		Toast(host->session.get(), text);
		hostClear();
	}
}

void Manager::hostClear() {
	const auto host = base::take(_host);
	if (!host) {
		return;
	}
	_hostTimer.cancel();
	_hostStartTimer.cancel();
	_hostSettleTimer.cancel();
	_hostIdleTimer.cancel();
	_hostEndTimer.cancel();
	hostShareCancel(host.get());
	if (const auto session = host->session.get()) {
		if (host->requestId) {
			session->api().request(host->requestId).cancel();
		}
		if (host->phase == Host::Phase::Posting && host->localId) {
			// The control message is on its way and can't be taken
			// back: it is ended when it comes, see orphanEnd().
			if (int(_orphans.size()) >= kMaxOrphans) {
				_orphans.erase(begin(_orphans));
			}
			_orphans.push_back({
				.session = host->session,
				.peer = host->peer->id,
				.sid = host->sid,
				.title = host->title,
			});
		}
		// However the session has been dropped, with the last edit or
		// without it, this app does not offer to join its own session.
		markDead(host->sid);
		const auto i = _observed.find(Key(session, host->peer->id));
		if (i != end(_observed) && i->second.state.session == host->sid) {
			i->second.state.ended = true;
		}
	}
	changed();
	if (host->quitting) {
		Core::App().quitPreventFinished();
	}
}

bool Manager::quitPrevent() {
	const auto host = _host.get();
	if (!host) {
		return false;
	}
	const auto session = host->session.get();
	const auto live = (host->phase == Host::Phase::Live)
		|| (host->phase == Host::Phase::Ending);
	if (host->quitting) {
		return true;
	} else if (!live
		|| !session
		|| !host->message
		|| host->throttle.flooded(crl::now())) {
		hostClear();
		return false;
	}
	// The app can't wait two seconds: the last edit, the "ended" one,
	// goes at once and replaces another one that may be on its way.
	// The quit goes on when it is answered (hostClear) or in a second
	// and a half anyway (kQuitPreventTimeoutMs in core/application.cpp).
	hostEnd();
	if (_host.get() != host) {
		return false;
	}
	host->quitting = true;
	if (host->requestId && host->sending.ended) {
		return true;
	}
	_hostTimer.cancel();
	if (const auto id = base::take(host->requestId)) {
		session->api().request(id).cancel();
	}
	host->throttle = Throttle();
	host->throttle.request();
	hostSend();
	return true;
}

void Manager::finishHosting() {
	hostEnd();
}

void Manager::join(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	if (!Get().listenTogether() || !PeerFits(peer)) {
		return;
	}
	const auto session = &peer->session();
	[[maybe_unused]] const auto found = rescan(peer);
	const auto i = _observed.find(Key(session, peer->id));
	if (i == end(_observed) || LiveLeft(i->second.state, NowMs()) <= 0) {
		controller->showToast(tr::lng_oblivion_listen_toast_stale(tr::now));
		changed();
		return;
	} else if (_host) {
		controller->showToast(
			tr::lng_oblivion_listen_toast_hosting(tr::now));
		return;
	}
	const auto observed = i->second;
	if (_listener) {
		if (_listener->message == observed.message) {
			return;
		}
		listenerFinish(Finish::Quiet);
	}
	watch(session);
	probeClock(session);
	_listener = std::make_unique<Listener>();
	_listener->serial = ++_listenerSerial;
	_listener->session = base::make_weak(session);
	_listener->peer = peer;
	_listener->message = observed.message;
	_listener->host = observed.host;
	_listener->book = listenerBook(session, peer, observed.message);
	FollowStream().fire({});
	listenerApply(observed.state);
	changed();
}

bool Manager::clicked(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const auto state = ParseItem(item);
	if (!state || !PeerFits(peer)) {
		return false;
	}
	const auto id = item->fullId();
	watch(&peer->session());
	if (_host && _host->message == id) {
		controller->showToast(tr::lng_oblivion_listen_toast_host(tr::now));
	} else if (_listener && _listener->message == id) {
		controller->showToast(
			tr::lng_oblivion_listen_toast_already(tr::now));
	} else if (LiveLeft(*state, NowMs()) <= 0) {
		controller->showToast(tr::lng_oblivion_listen_toast_stale(tr::now));
	} else {
		observe(item, *state, true);
		join(controller, peer);
	}
	return true;
}

void Manager::leave() {
	if (_listener) {
		listenerFinish(Finish::Quiet);
	}
}

void Manager::listenerApply(const State &state) {
	const auto listener = _listener.get();
	const auto known = (listener->state.session != 0);
	if (known
		&& state.session == listener->state.session
		&& state.seq <= listener->state.seq) {
		// The same edit once more (a message is "edited" for us when
		// a reaction or a reply to it changes as well), or the one we
		// have gone past by taking the next track of the queue.
		return;
	} else if (state.ended) {
		listenerFinish(Finish::Ended);
		return;
	}
	if (const auto session = listener->session.get()) {
		probeClock(session);
	}
	const auto sameTrack = known && (listener->state.track == state.track);
	listener->state = state;

	// The edit has come just now, so its moment can't be later than now:
	// with a clock that is behind the clock of the host the position
	// would stand still until the clock catches up.
	listener->state.at = std::min(state.at, NowMs());
	listener->predicted = false;
	listener->waiting = false;
	if (!sameTrack) {
		listener->track = FullMsgId();
		listener->unavailable = false;
		listener->loading = false;
	}
	listener->corrector.reset();
	listenerSync();
	listenerPreloadNext();
	listenerArm();
	changed();
}

// What is known about the track messages of the session of the control
// message in the chat peer. One book is kept for a chat, the one of the
// session that was joined there last: joining the same session again
// goes on with what its searches have told, another session starts with
// an empty one (the pace of the requests is common anyway).
std::shared_ptr<LookupBook> Manager::listenerBook(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		FullMsgId message) {
	const auto key = Key(session.get(), peer->id);
	const auto i = _books.find(key);
	if (i != end(_books) && i->second->message == message) {
		return i->second;
	}
	auto result = std::make_shared<LookupBook>();
	result->message = message;
	if (i != end(_books)) {
		i->second = result;
		return result;
	} else if (int(_books.size()) >= kMaxBooks) {
		_books.erase(begin(_books));
	}
	_books.emplace(key, result);
	return result;
}

// The message of the chat that is the track for this account, among
// the messages the app has: nothing is sent from here.
HistoryItem *Manager::listenerLocate(const Track &track) {
	const auto listener = _listener.get();
	const auto session = listener->session.get();
	if (!session || !track.document) {
		return nullptr;
	}
	auto &found = listener->book->found;
	const auto i = found.find(track.document);
	if (i != end(found)) {
		const auto item = session->data().message(
			listener->peer->id,
			i->second);
		const auto document = item ? ChatTrackDocument(item) : nullptr;
		if (document && document->id == track.document) {
			return item;
		}
		found.erase(i);
	}
	const auto item = LocateLoaded(listener->peer, track);
	if (item) {
		found.emplace(track.document, item->id);
	}
	return item;
}

// The app does not have the track message: the server is asked for it.
// In a supergroup the message has the same id as for the host, it is
// asked for by the id, once. In a private chat or a basic group the ids
// are different, the music of the chat around the date of the message
// is searched (listenerLookupSend), not more than twice for a file: the
// requests wait for their turn, the one for the track that the host
// plays now goes first.
Manager::Lookup Manager::listenerRequest(const Track &track, bool urgent) {
	const auto listener = _listener.get();
	const auto session = listener->session.get();
	const auto document = track.document;
	auto &book = *listener->book;
	if (!session || !document || book.missing.contains(document)) {
		return Lookup::Missing;
	}
	if (listener->peer->isChannel()) {
		const auto asked = book.asked.find(document);
		if (asked != end(book.asked)) {
			return (asked->second > 1) ? Lookup::Missing : Lookup::Pending;
		} else if (track.id <= 0) {
			return Lookup::Missing;
		}
		// The answer is for the session, not for this joining of it:
		// it is taken when the listener has left and joined again too.
		book.asked.emplace(document, 1);
		const auto weak = std::weak_ptr<LookupBook>(listener->book);
		session->api().requestMessageData(
			listener->peer,
			MsgId(track.id),
			crl::guard(this, [=] { listenerFetched(weak, track); }));
		return Lookup::Pending;
	} else if (listener->lookup.document == document) {
		return Lookup::Pending;
	}
	auto &wanted = listener->wanted;
	const auto i = ranges::find(wanted, document, &Track::document);
	if (i == end(wanted)) {
		if (LookupExhausted(book, track)) {
			// Everything has been asked: it was found and its message
			// is gone since.
			return Lookup::Missing;
		} else if (urgent) {
			wanted.push_front(track);
		} else {
			wanted.push_back(track);
		}
	} else if (urgent && i != begin(wanted)) {
		const auto copy = *i;
		wanted.erase(i);
		wanted.push_front(copy);
	}
	listenerLookupSchedule();
	return Lookup::Pending;
}

void Manager::listenerFetched(
		const std::weak_ptr<LookupBook> &weak,
		Track track) {
	const auto book = weak.lock();
	if (!book) {
		return;
	}
	book->asked[track.document] = kLookupStages;
	const auto listener = _listener.get();
	if (!listener || listener->book != book) {
		return;
	}
	if (!listenerLocate(track)) {
		book->missing.emplace(track.document);
	}
	if (listener->state.track.document == track.document) {
		listenerSync();
	}
	changed();
}

void Manager::listenerLookupSchedule() {
	const auto listener = _listener.get();
	if (!listener || listener->lookupId || listener->lookup.document) {
		return;
	}
	// Only what the host plays now or plays next is worth a request,
	// and only while it is not found: the answer about one track often
	// has the next ones as well.
	const auto &state = listener->state;
	std::erase_if(listener->wanted, [&](const Track &track) {
		const auto needed = (state.track.document == track.document)
			|| ranges::contains(
				state.queue,
				track.document,
				&Track::document);
		return !needed || listener->book->found.contains(track.document);
	});
	if (listener->wanted.empty()) {
		_listenerLookupTimer.cancel();
		return;
	}
	const auto delay = _lookupPace.delay(crl::now());
	if (delay > 0) {
		_listenerLookupTimer.callOnce(delay);
	} else {
		listenerLookupSend();
	}
}

void Manager::listenerLookupSend() {
	const auto listener = _listener.get();
	const auto session = listener->session.get();
	if (!session || listener->wanted.empty()) {
		return;
	}
	const auto track = listener->wanted.front();
	listener->wanted.pop_front();
	listener->lookup = track;
	const auto stage = LookupBegin(*listener->book, track);
	_lookupPace.sent(crl::now());

	const auto serial = listener->serial;
	const auto window = LookupWindow(track, stage);
	const auto sent = WallMs();
	const auto weak = base::make_weak(this);
	listener->lookupId = session->api().request(MTPmessages_Search(
		MTP_flags(0),
		listener->peer->input(),
		MTP_string(), // q
		MTP_inputPeerEmpty(), // from_id
		MTPInputPeer(), // saved_peer_id
		MTPVector<MTPReaction>(), // saved_reaction
		MTPint(), // top_msg_id
		MTP_inputMessagesFilterMusic(),
		MTP_int(int(window.from)), // min_date
		MTP_int(int(window.till)), // max_date
		MTP_int(0), // offset_id
		MTP_int(0), // add_offset
		MTP_int(kLookupLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](
			const MTPmessages_Messages &result,
			const MTP::Response &response) {
		ClockSample(sent, response.outerMsgId);
		if (const auto strong = weak.get()) {
			strong->listenerLookupDone(serial, result);
		}
	}).fail([=](const MTP::Error &error) {
		if (const auto strong = weak.get()) {
			strong->listenerLookupFailed(serial, error.type());
		}
	}).handleFloodErrors().send();
}

void Manager::listenerLookupDone(
		uint64 serial,
		const MTPmessages_Messages &result) {
	const auto listener = _listener.get();
	if (!listener || listener->serial != serial) {
		return;
	}
	listener->lookupId = 0;
	const auto session = listener->session.get();
	if (!session) {
		listenerLookupFinish(false);
		return;
	}
	const auto peerId = listener->peer->id;
	const auto wanted = listener->lookup;
	session->data().processExistingMessages(nullptr, result);

	auto list = std::vector<Candidate>();
	result.match([](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		for (const auto &message : data.vmessages().v) {
			if (PeerFromMessage(message) != peerId) {
				continue;
			}
			const auto item = session->data().message(
				peerId,
				IdFromMessage(message));
			const auto document = item ? ChatTrackDocument(item) : nullptr;
			if (document) {
				list.push_back({
					.id = item->id.bare,
					.document = document->id,
					.date = int64(item->date()),
				});
			}
		}
	});
	// The other music of the answer is remembered as well: the tracks
	// that the host plays next are often right there.
	auto &book = *listener->book;
	for (const auto &entry : list) {
		if (entry.document != wanted.document) {
			LookupAdopt(book, entry.document, MsgId(entry.id));
		}
	}
	const auto id = PickTrack(wanted, list, false);
	if (id) {
		book.found.emplace_or_assign(wanted.document, MsgId(id));
		book.missing.remove(wanted.document);
	}
	listenerLookupFinish(id != 0);
}

void Manager::listenerLookupFailed(uint64 serial, const QString &type) {
	const auto listener = _listener.get();
	if (!listener || listener->serial != serial) {
		return;
	}
	listener->lookupId = 0;
	const auto track = listener->lookup;
	auto &book = *listener->book;
	if (MTP::IsFloodError(type)) {
		// The server asks to wait: the wait is kept in full (by the
		// whole app, whoever joins what meanwhile), and the same request
		// is sent once more after it, not more than that.
		_lookupPace.flood(crl::now(), FloodSeconds(type));
		if (LookupFlooded(book, track)) {
			listener->lookup = Track();
			listener->wanted.push_front(track);
			listenerLookupSchedule();
			return;
		}
	} else {
		book.asked[track.document] = kLookupStages;
	}
	listenerLookupFinish(false);
}

void Manager::listenerLookupFinish(bool found) {
	const auto listener = _listener.get();
	const auto track = base::take(listener->lookup);
	if (!found && track.document && LookupGoesOn(*listener->book, track)) {
		// Not near the date of the host, see LookupWindow().
		listener->wanted.push_front(track);
	}
	if (listener->state.track.document == track.document) {
		listenerSync();
	}
	if (_listener.get() == listener) {
		listenerPreloadNext();
		listenerLookupSchedule();
	}
	changed();
}

// A message with a music file has come to a chat (or has changed). The
// chat has that file now, whatever the searches of a session there have
// told before, also while nobody is joined. And if it is the track that
// the listener could not find, it is here.
void Manager::tracksAdopt(not_null<HistoryItem*> item) {
	if (_books.empty() && !_listener) {
		return;
	}
	const auto document = ChatTrackDocument(item);
	if (!document) {
		return;
	}
	const auto id = document->id;
	const auto peer = item->history()->peer;
	const auto session = &peer->session();
	const auto i = _books.find(Key(session, peer->id));
	if (i != end(_books)) {
		LookupAdopt(*i->second, id, item->id);
	}
	const auto listener = _listener.get();
	if (!listener
		|| listener->peer != peer
		|| listener->session.get() != session) {
		return;
	}
	LookupAdopt(*listener->book, id, item->id);
	if (!listener->track && listener->state.track.document == id) {
		const auto serial = listener->serial;
		crl::on_main(this, [=] {
			if (_listener && _listener->serial == serial) {
				listenerSync();
				changed();
			}
		});
	}
}

// The player is not left playing a track of the session when the host
// does not play it: he is on pause, or he has gone to a track that this
// app has not found yet.
void Manager::listenerHold() {
	const auto listener = _listener.get();
	const auto player = Media::Player::instance();
	const auto current = player->current(SongType::Song);
	const auto state = player->getState(SongType::Song);
	const auto id = current.contextId();
	const auto ours = current.audio()
		&& ((listener->playing && id == listener->playing)
			|| (listener->track && id == listener->track));
	if (ours && state.id && Media::Player::ShowPauseIcon(state.state)) {
		listenerPause();
	}
}

void Manager::listenerSync() {
	const auto listener = _listener.get();
	const auto session = listener->session.get();
	if (!session) {
		return;
	}
	const auto track = listener->state.track;
	const auto item = listenerLocate(track);
	const auto document = item ? TrackDocument(item) : nullptr;
	if (!document) {
		const auto status = listenerRequest(track, true);
		listener->track = FullMsgId();
		listener->loading = (status == Lookup::Pending);
		listener->unavailable = (status == Lookup::Missing);
		listenerHold();
		return;
	}
	const auto id = item->fullId();
	listener->track = id;
	listener->unavailable = false;
	listener->loading = false;
	listener->title = TrackTitle(document);

	const auto player = Media::Player::instance();
	const auto current = player->current(SongType::Song);
	const auto state = player->getState(SongType::Song);
	const auto same = (current.audio() == document)
		&& (current.contextId() == id);
	const auto active = same
		&& state.id
		&& !Media::Player::IsStopped(state.state);
	if (!listener->state.playing) {
		listenerHold();
		return;
	}
	const auto duration = (listener->state.duration > 0)
		? listener->state.duration
		: int64(document->duration());
	const auto expected = ExpectedPosition(listener->state, NowMs());
	if (duration > 0 && expected >= duration - kEndMargin) {
		listener->waiting = true;
		return;
	} else if (listener->ownPause) {
		return;
	} else if (!active) {
		listenerPlay(document, id, expected);
	} else if (Media::Player::IsPausedOrPausing(state.state)) {
		listenerResume(expected);
	}
}

// The message of the track that the host plays next is looked for in
// advance, once, if the app does not have it: the player can go to it
// by itself only when it is found.
void Manager::listenerPreloadNext() {
	const auto listener = _listener.get();
	if (!listener || listener->state.queue.empty()) {
		return;
	}
	const auto next = listener->state.queue.front();
	if (!listenerLocate(next)) {
		[[maybe_unused]] const auto status = listenerRequest(next, false);
	}
}

void Manager::listenerPlay(
		not_null<DocumentData*> document,
		FullMsgId id,
		int64 position) {
	const auto listener = _listener.get();
	const auto from = (position < kEndMargin) ? int64(0) : position;
	listener->acting = true;
	listener->pausedByUs = false;
	listener->playing = id;
	PendingStart() = StartPosition{
		.document = document,
		.id = id,
		.position = crl::time(from),
	};
	Media::Player::instance()->play(AudioMsgId(document, id));
	PendingStart() = StartPosition();
	listener->acting = false;
	listener->corrector.started(crl::now(), from, true);
	FollowStream().fire({});
}

void Manager::listenerSeek(int64 target, bool measure) {
	const auto listener = _listener.get();
	const auto player = Media::Player::instance();
	const auto state = player->getState(SongType::Song);
	const auto length = ToMs(state.length, state.frequency);
	if (length <= 0) {
		return;
	}
	const auto to = std::clamp(target, int64(0), length);
	listener->acting = true;
	listener->pausedByUs = false;
	player->finishSeeking(SongType::Song, to / float64(length));
	listener->acting = false;
	if (measure) {
		listener->corrector.started(crl::now(), to, false);
	}
}

void Manager::listenerResume(int64 expected) {
	const auto listener = _listener.get();
	const auto player = Media::Player::instance();
	listenerSeek(expected + int64(listener->corrector.lead()), true);

	// The seek starts the playback, but it can't be done while the
	// length of the track is not known yet.
	const auto state = player->getState(SongType::Song);
	if (Media::Player::IsPausedOrPausing(state.state)) {
		listener->acting = true;
		listener->pausedByUs = false;
		player->play(SongType::Song);
		listener->acting = false;
	}
}

void Manager::listenerPause() {
	const auto listener = _listener.get();
	listener->acting = true;
	listener->pausedByUs = true;
	Media::Player::instance()->pause(SongType::Song);
	listener->acting = false;
}

void Manager::listenerPlayerUpdated() {
	const auto listener = _listener.get();
	const auto session = listener->session.get();
	if (listener->acting || !session) {
		return;
	}
	const auto player = Media::Player::instance();
	const auto current = player->current(SongType::Song);
	const auto id = listener->track;
	const auto document = current.audio();
	const auto state = player->getState(SongType::Song);
	if (!document || (&document->session() != session) || !state.id) {
		return;
	}
	const auto position = ToMs(state.position, state.frequency);
	const auto playing = Media::Player::ShowPauseIcon(state.state);
	const auto paused = Media::Player::IsPausedOrPausing(state.state);
	const auto shown = [](not_null<const Listener*> listener) {
		return std::tuple(
			listener->ownPause,
			listener->waiting,
			listener->corrector.loading());
	};
	const auto was = shown(listener);
	const auto guard = gsl::finally([&] {
		const auto raw = _listener.get();
		if (raw && was != shown(raw)) {
			changed();
		}
	});
	if (playing) {
		listener->pausedByUs = false;
	}
	if (!id || current.contextId() != id) {
		// The player still has the previous track of the session (it
		// was on pause when the host went on, or the new track is not
		// found yet) and the listener has pressed play: he is brought
		// to what the host plays now.
		if (playing
			&& listener->playing
			&& current.contextId() == listener->playing) {
			listener->ownPause = false;
			if (listener->state.playing) {
				listenerSync();
			} else {
				listenerPause();
			}
		}
		return;
	} else if (!listener->state.playing) {
		// Only the host controls the playback: while he is on pause
		// the track of the session stays on pause for everybody. The
		// listener has pressed play, so he wants to follow: when the
		// host goes on, so does he.
		if (playing) {
			listener->ownPause = false;
			listenerPause();
		}
		return;
	} else if (paused) {
		// The listener has paused his own player (or a call has): he is
		// brought back to the host when he presses play again.
		if (!listener->pausedByUs) {
			listener->ownPause = true;
		}
		return;
	}
	const auto duration = (listener->state.duration > 0)
		? listener->state.duration
		: ToMs(state.length, state.frequency);
	const auto expected = ExpectedPosition(listener->state, NowMs());
	const auto over = (duration > 0) && (expected >= duration - kEndMargin);
	if (Media::Player::IsStopped(state.state)) {
		if (state.state == PlayerState::StoppedAtEnd) {
			listener->waiting = true;
		}
		return;
	} else if (over) {
		// The host has finished the track. If our player is far from
		// its end (it has started the track again by "repeat one") it
		// waits for the host on pause, otherwise it plays to the end.
		if (playing && position < duration - kRestartMargin) {
			listener->waiting = true;
			listenerPause();
		}
		return;
	} else if (listener->ownPause) {
		listener->ownPause = false;
		listenerSeek(expected + int64(listener->corrector.lead()), true);
		return;
	} else if (!NormalSpeed(current)) {
		return;
	}
	const auto result = listener->corrector.update(
		crl::now(),
		expected,
		position);
	if (result.seek
		&& (duration <= 0 || result.target < duration - kSeekEndMargin)) {
		listenerSeek(result.target, false);
	}
}

bool Manager::follows(const AudioMsgId &current) const {
	const auto listener = _listener.get();
	const auto document = current.audio();
	return listener
		&& document
		&& listener->playing
		&& (listener->session.get() == &document->session())
		&& (current.contextId() == listener->playing);
}

bool Manager::followerMove(int delta, bool autonext) {
	const auto listener = _listener.get();
	if (!listener
		|| !autonext
		|| delta != 1
		|| !listener->state.playing
		|| listener->ownPause
		|| listener->state.queue.empty()
		|| !listener->track
		|| listener->playing != listener->track) {
		return false;
	}
	const auto next = listener->state.queue.front();
	const auto item = listenerLocate(next);
	const auto document = item ? TrackDocument(item) : nullptr;
	if (!document) {
		return false;
	}
	// The player of the host goes to this track now as well. His edit
	// with the exact position is on its way, till then the track starts
	// from the beginning here, as it does there.
	const auto id = item->fullId();
	auto &state = listener->state;
	state.track = next;
	state.position = 0;
	state.at = NowMs();
	state.duration = int64(document->duration());
	state.queue.erase(begin(state.queue));
	listener->track = id;
	listener->predicted = true;
	listener->waiting = false;
	listener->unavailable = false;
	listener->loading = false;
	listener->title = TrackTitle(document);
	listener->corrector.reset();
	listenerPlay(document, id, 0);
	listenerPreloadNext();
	listenerArm();
	changed();
	return true;
}

void Manager::listenerArm() {
	const auto listener = _listener.get();
	if (!listener) {
		_listenerTimer.cancel();
		return;
	}
	const auto left = listener->predicted
		? int64(kPredictedWait)
		: LiveLeft(listener->state, NowMs());
	_listenerTimer.callOnce(crl::time(std::max(left, int64(0))) + 100);
}

void Manager::listenerCheck() {
	const auto listener = _listener.get();
	if (!listener) {
		return;
	} else if (listener->predicted
		|| LiveLeft(listener->state, NowMs()) <= 0) {
		listenerFinish(Finish::Vanished);
	} else {
		listenerArm();
	}
}

void Manager::listenerFinish(Finish reason) {
	const auto listener = base::take(_listener);
	if (!listener) {
		return;
	}
	_listenerTimer.cancel();
	_listenerLookupTimer.cancel();
	if (listener->lookupId) {
		// The search is taken back with no answer: it does not count as
		// made (its place in the pace of the requests does).
		if (const auto session = listener->session.get()) {
			session->api().request(listener->lookupId).cancel();
		}
		LookupAborted(*listener->book, listener->lookup);
	}
	if (reason == Finish::Vanished) {
		// The last state of a host that has vanished is not offered to
		// join any more.
		const auto session = listener->session.get();
		const auto i = _observed.find(Key(session, listener->message.peer));
		if (i != end(_observed)
			&& i->second.message == listener->message) {
			_observed.erase(i);
		}
	}
	const auto text = (reason == Finish::Ended)
		? tr::lng_oblivion_listen_toast_ended(tr::now)
		: (reason == Finish::Vanished)
		? tr::lng_oblivion_listen_toast_vanished(tr::now)
		: (reason == Finish::Left)
		? tr::lng_oblivion_listen_toast_left(tr::now)
		: QString();
	Toast(listener->session.get(), text);
	FollowStream().fire({});
	changed();
}

struct Check {
	QStringList &log;
	int passed = 0;
	int failed = 0;

	void operator()(bool condition, const QString &what) {
		if (condition) {
			++passed;
		} else {
			++failed;
			log.push_back(u"listen: FAILED "_q + what);
		}
	}
};

[[nodiscard]] State SampleState(int index) {
	const auto big = std::numeric_limits<uint64>::max();
	switch (index) {
	case 0: return State{
		.session = 1,
		.seq = 1,
		.track = { .id = 1, .document = 1 },
	};
	case 1: return State{
		.session = 0x0000A1B2C3D4E5F6ULL & kSessionMask,
		.seq = 42,
		.track = {
			.id = 123456,
			.document = 5'377'412'908'104'993'221ULL,
			.date = 1'790'000'000,
		},
		.position = 73'250,
		.at = 1'790'000'000'123LL,
		.duration = 215'000,
		.playing = true,
		.queue = {
			{ 123460, 5'377'412'908'104'993'300ULL, 1'790'000'040 },
			{ 123471, 6'001'000'000'000'000'001ULL, 1'790'000'500 },
			{ 123459, 77ULL, 1'789'999'990 },
		},
	};
	case 2: return State{
		.session = kSessionMask,
		.seq = std::numeric_limits<uint32>::max(),
		.track = { .id = kMaxTrackId, .document = big, .date = kMaxDate },
		.position = kMaxTime,
		.at = std::numeric_limits<int64>::max() / 4,
		.duration = kMaxTime,
		.playing = true,
		.ended = true,
		.away = true,
		.queue = {
			{ 1, big, 0 },
			{ kMaxTrackId, 1, kMaxDate },
			{ 2, big - 1, 1 },
		},
	};
	}
	return State{
		.session = 0x123456789ABCULL,
		.seq = 7,
		.track = { .id = 2'000'000'000, .document = 9, .date = 5 },
		.position = 0,
		.at = 1'760'000'000'000LL,
		.duration = 0,
		.ended = true,
	};
}

void TestCodec(Check &check) {
	for (auto i = 0; i != 4; ++i) {
		const auto state = SampleState(i);
		const auto url = EncodeState(state);
		const auto name = u"codec %1: "_q.arg(i);
		const auto decoded = DecodeState(url);
		check(decoded && (*decoded == state), name + u"round trip"_q);
		check(url.startsWith(u"https://t.me/#oblivion-listen."_q)
			&& url.size() < 256,
			name + u"address"_q);
		auto clean = true;
		for (const auto ch : url.mid(int(u"https://t.me/#"_q.size()))) {
			const auto code = ch.unicode();
			clean = clean
				&& ((code >= 'A' && code <= 'Z')
					|| (code >= 'a' && code <= 'z')
					|| (code >= '0' && code <= '9')
					|| code == '-'
					|| code == '_'
					|| code == '.');
		}
		check(clean, name + u"unreserved characters only"_q);

		const auto text = MakeControlText(u"Listening: A - B"_q, state);
		const auto parsed = ParseControlText(text);
		check(parsed && (*parsed == state), name + u"message"_q);
		check(text.entities.size() == 1
			&& text.entities.front().offset() == 0
			&& text.entities.front().length() == 2
			&& text.text.startsWith(Headphones() + ' '),
			name + u"link over the emoji"_q);
		check(!DecodeState(url + u"&a=1"_q) && !DecodeState(url + u"/"_q),
			name + u"nothing may follow the state"_q);

		// The server or another app may put the scheme or the host
		// differently, only the fragment matters.
		const auto fragment = url.mid(url.indexOf('#'));
		const auto other = DecodeState(u"http://telegram.me/"_q + fragment);
		check(other && (*other == state), name + u"other host"_q);
	}

	// A pause with its reason, and a queue longer than the listeners
	// ever need: only the nearest tracks are written.
	auto away = SampleState(1);
	away.playing = false;
	away.away = true;
	const auto kept = away.queue;
	away.queue.push_back({ 123480, 81ULL, 1'790'001'000 });
	away.queue.push_back({ 123481, 82ULL, 1'790'002'000 });
	const auto cut = DecodeState(EncodeState(away));
	check(cut
		&& cut->away
		&& !cut->playing
		&& (int(kept.size()) == kMaxQueue)
		&& (cut->queue == kept)
		&& (cut->track == away.track),
		u"codec: a pause for a track that is not in the chat"_q);
}

void TestGarbage(Check &check) {
	const auto state = SampleState(1);
	const auto bytes = Serialize(state);
	const auto good = Deserialize(bytes);
	check(good && (*good == state), u"garbage: the sample itself"_q);

	auto truncated = 0;
	for (auto size = 0; size != int(bytes.size()); ++size) {
		if (Deserialize(bytes.left(size))) {
			++truncated;
		}
	}
	check(!truncated, u"garbage: truncated payloads"_q);

	auto flipped = 0;
	for (auto i = 0; i != int(bytes.size()) * 8; ++i) {
		auto copy = bytes;
		copy[i / 8] = char(uchar(copy[i / 8]) ^ uchar(1 << (i % 8)));
		if (Deserialize(copy)) {
			++flipped;
		}
	}
	check(!flipped, u"garbage: single bit errors"_q);

	const auto withCrc = [](QByteArray body) {
		const auto crc = Crc16(body, int(body.size()));
		body.push_back(char(uchar(crc & 0xFF)));
		body.push_back(char(uchar(crc >> 8)));
		return body;
	};
	const auto body = bytes.left(bytes.size() - 2);
	auto version = body;
	version[0] = char(kVersion + 1);
	check(!Deserialize(withCrc(version)), u"garbage: unknown version"_q);

	auto flags = body;
	flags[1] = char(uchar(flags[1]) | 0x80);
	const auto withFlags = Deserialize(withCrc(flags));
	check(withFlags && (*withFlags == state),
		u"garbage: unknown flags are skipped"_q);

	const auto withTail = Deserialize(withCrc(body + QByteArray(3, 'x')));
	check(withTail && (*withTail == state),
		u"garbage: unknown tail is skipped"_q);

	// The track 5 with the file 7 of the date 5, then seq, position, at
	// and duration, the queue follows.
	const auto head = [](uint64 document) {
		auto result = QByteArray();
		result.push_back(char(kVersion));
		result.push_back(char(0));
		result.append(QByteArray(kSessionBytes, char(1)));
		PutVarint(result, 5);
		PutFileId(result, document);
		for (auto i = 0; i != 5; ++i) {
			PutVarint(result, 5);
		}
		return result;
	};
	const auto entry = [](QByteArray &to, int64 shift, uint64 document) {
		PutVarint(to, ZigZag(shift));
		PutFileId(to, document);
		PutVarint(to, ZigZag(0));
	};
	const auto queue = head(7);
	auto one = queue;
	PutVarint(one, 1);
	entry(one, 1, 8);
	const auto withOne = Deserialize(withCrc(one));
	check(withOne
		&& (withOne->track == Track{ .id = 5, .document = 7, .date = 5 })
		&& (withOne->queue.size() == 1)
		&& (withOne->queue[0] == Track{ .id = 6, .document = 8, .date = 5 }),
		u"garbage: a payload made by hand"_q);

	auto empty = head(0);
	PutVarint(empty, 0);
	check(!Deserialize(withCrc(empty)), u"garbage: a track without a file"_q);

	auto tooLong = queue;
	PutVarint(tooLong, uint64(kMaxQueue + 1));
	for (auto i = 0; i != kMaxQueue + 1; ++i) {
		entry(tooLong, 1, 8);
	}
	check(!Deserialize(withCrc(tooLong)), u"garbage: a long queue"_q);

	auto missing = queue;
	PutVarint(missing, 3);
	entry(missing, 1, 8);
	check(!Deserialize(withCrc(missing)), u"garbage: a cut queue"_q);

	auto negative = queue;
	PutVarint(negative, 1);
	entry(negative, -10, 8);
	check(!Deserialize(withCrc(negative)), u"garbage: a negative id"_q);

	auto nameless = queue;
	PutVarint(nameless, 1);
	entry(nameless, 1, 0);
	check(!Deserialize(withCrc(nameless)),
		u"garbage: a queue track without a file"_q);

	auto leap = queue;
	PutVarint(leap, 1);
	entry(leap, std::numeric_limits<int64>::max(), 8);
	check(!Deserialize(withCrc(leap)), u"garbage: an endless id step"_q);

	auto endless = QByteArray();
	endless.push_back(char(kVersion));
	endless.push_back(char(0));
	endless.append(QByteArray(kSessionBytes, char(1)));
	endless.append(QByteArray(40, char(0xFF)));
	check(!Deserialize(withCrc(endless)), u"garbage: an endless number"_q);

	check(!Deserialize(QByteArray()), u"garbage: empty"_q);
	check(!Deserialize(QByteArray(1000, 'a')), u"garbage: too long"_q);

	const auto prefix = u"https://t.me/"_q + Marker();
	check(!DecodeState(QString()), u"garbage: empty address"_q);
	check(!DecodeState(prefix), u"garbage: no payload"_q);
	check(!DecodeState(prefix + u"!!!!"_q), u"garbage: bad characters"_q);
	check(!DecodeState(prefix + QString(5000, QChar('A'))),
		u"garbage: a huge payload"_q);
	check(!DecodeState(u"https://t.me/#something-else.AAAA"_q),
		u"garbage: another fragment"_q);

	// Random junk must be refused without a crash; a few random strings
	// of the right shape with a right checksum are possible in theory.
	auto seed = uint32(0x0B11F10D);
	const auto next = [&] {
		seed = seed * 1664525U + 1013904223U;
		return seed >> 8;
	};
	const auto alphabet = u"ABCDEFGHIJKLMNOPQRSTUVWXYZ"_q
		+ u"abcdefghijklmnopqrstuvwxyz"_q
		+ u"0123456789-_=+/.%"_q;
	auto accepted = 0;
	for (auto i = 0; i != 4000; ++i) {
		auto junk = QString();
		const auto length = int(next() % 120);
		for (auto j = 0; j != length; ++j) {
			junk.push_back(alphabet[int(next() % alphabet.size())]);
		}
		if (DecodeState(prefix + junk)) {
			++accepted;
		}
		auto raw = QByteArray();
		const auto size = int(next() % 80);
		for (auto j = 0; j != size; ++j) {
			raw.push_back(char(next() & 0xFF));
		}
		if (Deserialize(raw)) {
			++accepted;
		}
	}
	check(accepted <= 1, u"garbage: random junk"_q);

	auto plain = TextWithEntities{ u"Listening together: A - B"_q };
	check(!ParseControlText(plain), u"garbage: a plain text"_q);
	plain.entities.push_back(EntityInText(
		EntityType::CustomUrl,
		0,
		9,
		u"https://example.com/#listen"_q));
	plain.entities.push_back(EntityInText(EntityType::Bold, 0, 4));
	check(!ParseControlText(plain), u"garbage: another link is no state"_q);
}

[[nodiscard]] TextWithEntities LinkedText(
		const QString &text,
		int offset,
		int length,
		const QString &url) {
	auto result = TextWithEntities{ text };
	result.entities.push_back(EntityInText(
		EntityType::CustomUrl,
		offset,
		length,
		url));
	return result;
}

// What the "edited messages history" leaves out: only the edits that
// a host makes to the control message of his session.
void TestEdits(Check &check) {
	auto first = SampleState(1);
	auto second = first;
	second.seq = first.seq + 1;
	second.track = first.queue.front();
	second.queue.erase(begin(second.queue));
	auto ended = second;
	ended.seq = second.seq + 1;
	ended.playing = false;
	ended.ended = true;
	ended.queue.clear();

	const auto one = MakeControlText(u"Listening together: A - B"_q, first);
	const auto two = MakeControlText(u"Listening together: C - D"_q, second);
	const auto last = MakeControlText(u"Has ended: C - D"_q, ended);
	check(IsSessionEdit(one, two), u"edits: the next track"_q);
	check(IsSessionEdit(two, last), u"edits: the end of a session"_q);
	check(!IsSessionEdit(two, one), u"edits: a state that goes back"_q);
	check(!IsSessionEdit(one, one), u"edits: the same state"_q);
	check(!IsSessionEdit(last, two)
		&& !IsSessionEdit(last, MakeControlText(u"Again"_q, [&] {
			auto again = ended;
			++again.seq;
			return again;
		}())),
		u"edits: an ended session is not edited by its host"_q);

	auto other = second;
	other.session = first.session ^ 0x10;
	check(!IsSessionEdit(one, MakeControlText(u"Other"_q, other)),
		u"edits: another session"_q);

	// An ordinary message can't be hidden behind a link with a state:
	// neither when the link is added by the edit, nor when it was there
	// from the start, nor when the edit takes it away.
	const auto url = EncodeState(second);
	const auto headphones = Headphones();
	const auto plain = TextWithEntities{ u"I will pay tomorrow"_q };
	const auto word = LinkedText(u"I never said that"_q, 0, 1, url);
	check(!IsSessionEdit(plain, two), u"edits: a message turned control"_q);
	check(!IsSessionEdit(one, plain), u"edits: a control turned message"_q);
	check(!IsSessionEdit(plain, word)
		&& !IsSessionEdit(LinkedText(plain.text, 2, 4, EncodeState(first)), word)
		&& !ParseControlText(word),
		u"edits: a link with a state on a word"_q);
	check(!ParseControlText(LinkedText(
			u"x "_q + headphones + u" text"_q,
			2,
			int(headphones.size()),
			url)),
		u"edits: the headphones not at the start"_q);
	check(!ParseControlText(LinkedText(
			headphones + u" text"_q,
			0,
			int(headphones.size()) + 2,
			url)),
		u"edits: a link wider than the headphones"_q);
	check(!ParseControlText(LinkedText(
			headphones + u"text"_q,
			0,
			int(headphones.size()),
			url)),
		u"edits: no space after the headphones"_q);
	check(!ParseControlText(LinkedText(
			headphones + ' ' + QString(kMaxVisibleLength + 1, QChar('a')),
			0,
			int(headphones.size()),
			url)),
		u"edits: a text longer than a control message has"_q);
	check(!ParseControlText(LinkedText(
			headphones + u" text"_q,
			0,
			int(headphones.size()),
			u"https://example.com/?next=t.me/"_q + Marker() + u"Av8_"_q)),
		u"edits: a link that only has the marker"_q);

	// A message of a later version of this code is not understood, so
	// it is an ordinary message here and its edits are recorded.
	const auto prefix = u"https://t.me/"_q + Marker();
	const auto later = LinkedText(
		headphones + u" Listening"_q,
		0,
		int(headphones.size()),
		prefix + u"Av8_"_q);
	check(!ParseControlText(later) && !IsSessionEdit(later, later),
		u"edits: a later version"_q);
}

// The track of the host among the messages that a listener has, and
// the requests of a listener that looks for it.
void TestTracks(Check &check) {
	const auto track = Track{ .id = 500, .document = 77, .date = 1000 };
	const auto list = std::vector<Candidate>{
		{ .id = 10, .document = 76, .date = 1000 },
		{ .id = 11, .document = 77, .date = 400 },
		{ .id = 12, .document = 77, .date = 1003 },
		{ .id = 13, .document = 78, .date = 1000 },
		{ .id = 14, .document = 77, .date = 5000 },
		{ .id = 500, .document = 76, .date = 1000 },
	};
	check(PickTrack(track, list, false) == 12,
		u"tracks: the file with the closest date"_q);
	check(PickTrack(track, list, true) == 12,
		u"tracks: the id of the host with another file is not it"_q);
	check(PickTrack(
			Track{ .id = 14, .document = 77, .date = 1000 },
			list,
			true) == 14,
		u"tracks: the id of the host in a supergroup"_q);
	check(PickTrack(
			Track{ .id = 14, .document = 77, .date = 1000 },
			list,
			false) == 12,
		u"tracks: the id of the host means nothing in a private chat"_q);
	check(PickTrack(Track{ .id = 1, .document = 77 }, list, false) == 14,
		u"tracks: the newest one when the date is not known"_q);
	check(!PickTrack(Track{ .id = 12, .document = 99, .date = 1003 }, list, true)
		&& !PickTrack(Track{ .id = 12, .date = 1003 }, list, true)
		&& !PickTrack(track, {}, false),
		u"tracks: no message with the file"_q);
	const auto twins = std::vector<Candidate>{
		{ .id = 20, .document = 77, .date = 990 },
		{ .id = 21, .document = 77, .date = 1010 },
		{ .id = 0, .document = 77, .date = 1000 },
		{ .id = -5, .document = 77, .date = 1000 },
	};
	check(PickTrack(track, twins, false) == 21,
		u"tracks: the newer of two equally close ones"_q);

	const auto first = LookupWindow(track, 1);
	const auto second = LookupWindow(track, 2);
	check(first.from == 1000 - kLookupBefore
		&& first.till == 1000 + kLookupClose
		&& LookupStages(track) == kLookupStages,
		u"tracks: the first search is around the date"_q);
	check(second.from < first.till
		&& second.from > 1000
		&& second.till == 1000 + kLookupAfter,
		u"tracks: the second search goes on from the first"_q);
	const auto dateless = Track{ .id = 1, .document = 77 };
	const auto unknown = LookupWindow(dateless, 1);
	check(!unknown.from && !unknown.till && LookupStages(dateless) == 1,
		u"tracks: one search without dates for an unknown date"_q);
	const auto early = LookupWindow(
		Track{ .id = 1, .document = 77, .date = 5 },
		1);
	const auto late = LookupWindow(
		Track{ .id = 1, .document = 77, .date = kMaxDate },
		2);
	check(early.from == 1
		&& late.till == kMaxDate
		&& late.from <= late.till,
		u"tracks: the dates stay in their range"_q);

	{
		// A host that changes the track every two seconds for an hour,
		// none of the tracks is found without a request.
		auto pace = LookupPace();
		auto sends = std::vector<crl::time>();
		for (auto now = crl::time(0); now < 60 * kMinute; now += 100) {
			if (!(now % 2000) && pace.delay(now) == crl::time(0)) {
				pace.sent(now);
				sends.push_back(now);
			}
		}
		auto shortest = kMinute;
		auto most = 0;
		for (auto i = 0; i != int(sends.size()); ++i) {
			if (i) {
				shortest = std::min(shortest, sends[i] - sends[i - 1]);
			}
			auto inWindow = 0;
			for (auto j = i; j != int(sends.size()); ++j) {
				if (sends[j] < sends[i] + kLookupWindow) {
					++inWindow;
				}
			}
			most = std::max(most, inWindow);
		}
		check(shortest >= kLookupGap, u"tracks: the pause between lookups"_q);
		check(most <= kLookupsInWindow, u"tracks: lookups in five minutes"_q);
		check(int(sends.size()) >= 100 && int(sends.size()) <= 130,
			u"tracks: lookups in an hour"_q);
	}
	{
		auto pace = LookupPace();
		check(pace.delay(500) == crl::time(0),
			u"tracks: the first lookup goes at once"_q);
		pace.sent(500);
		check(pace.delay(600) == kLookupGap - 100,
			u"tracks: the second lookup waits"_q);
		pace.flood(1000, 20);
		check(pace.delay(1000) == crl::time(21'000)
			&& pace.delay(22'000) == crl::time(0),
			u"tracks: FLOOD_WAIT is waited out"_q);
	}
	{
		// What the searches have told is kept for the session: a
		// listener who leaves and joins again every half a second for
		// a minute, the chat does not have the file.
		auto book = LookupBook();
		auto pace = LookupPace();
		auto sends = std::vector<crl::time>();
		for (auto now = crl::time(0); now < kMinute; now += 500) {
			if (book.missing.contains(track.document)
				|| LookupExhausted(book, track)
				|| pace.delay(now) > 0) {
				continue;
			}
			const auto stage = LookupBegin(book, track);
			pace.sent(now);
			sends.push_back(now);
			check(stage == int(sends.size()),
				u"tracks: the searches of a file go one after another"_q);
			if (!LookupGoesOn(book, track)) {
				check(stage == LookupStages(track),
					u"tracks: given up after the last search only"_q);
			}
		}
		check(int(sends.size()) == LookupStages(track)
			&& (sends.back() - sends.front() >= kLookupGap)
			&& book.missing.contains(track.document),
			u"tracks: joining again asks nothing again"_q);

		LookupAdopt(book, track.document, MsgId(12));
		const auto i = book.found.find(track.document);
		check(!book.missing.contains(track.document)
			&& (i != end(book.found))
			&& (i->second == MsgId(12)),
			u"tracks: a file that has come to the chat is not missing"_q);
		LookupAdopt(book, track.document, MsgId(14));
		check(book.found.find(track.document)->second == MsgId(12),
			u"tracks: the message found first is kept"_q);
	}
	{
		// "Leave" while a search is on its way, again and again for ten
		// minutes: the search is repeated, at the pace of the requests.
		auto book = LookupBook();
		auto pace = LookupPace();
		auto sends = std::vector<crl::time>();
		for (auto now = crl::time(0); now < 10 * kMinute; now += 100) {
			if (pace.delay(now) > 0) {
				continue;
			}
			check(LookupBegin(book, track) == 1,
				u"tracks: a search taken back is made again"_q);
			pace.sent(now);
			sends.push_back(now);
			LookupAborted(book, track);
		}
		auto shortest = kMinute;
		for (auto i = 1; i < int(sends.size()); ++i) {
			shortest = std::min(shortest, sends[i] - sends[i - 1]);
		}
		check(shortest >= kLookupGap
			&& int(sends.size()) <= 2 * kLookupsInWindow
			&& !LookupExhausted(book, track),
			u"tracks: leaving and joining keeps the pace"_q);
		LookupAborted(book, Track{ .id = 1, .document = 99, .date = 5 });
		check(!book.asked.contains(99) && !book.missing.contains(99),
			u"tracks: nothing to take back for a file not asked about"_q);
	}
	{
		// FLOOD_WAIT: the wait belongs to the app and not to one joining,
		// the search is made once more after it and never a third time.
		auto book = LookupBook();
		auto pace = LookupPace();
		check(LookupBegin(book, track) == 1, u"tracks: the first search"_q);
		pace.sent(0);
		pace.flood(200, 30);
		check(LookupFlooded(book, track)
			&& !LookupExhausted(book, track)
			&& pace.delay(5000) == crl::time(26'200),
			u"tracks: the wait goes on for one who joins again"_q);
		check(LookupBegin(book, track) == 1,
			u"tracks: the same search after FLOOD_WAIT"_q);
		pace.sent(31'200);
		check(!LookupFlooded(book, track)
			&& LookupExhausted(book, track)
			&& !LookupGoesOn(book, track)
			&& book.missing.contains(track.document),
			u"tracks: given up after the second FLOOD_WAIT"_q);

		auto single = LookupBook();
		check(LookupBegin(single, dateless) == 1
			&& LookupExhausted(single, dateless)
			&& !LookupGoesOn(single, dateless),
			u"tracks: one search for a track without a date"_q);
	}
}

// What is sent to the chat by a click of the host and what the group or
// the server may say to that.
void TestSends(Check &check) {
	using Block = SlowmodeBlock;
	check(CheckSlowmode(false, 0, false, 1) == Block::None
		&& CheckSlowmode(false, 0, true, 2) == Block::None,
		u"sends: no slow mode"_q);
	check(CheckSlowmode(true, 0, false, 1) == Block::None,
		u"sends: one message in a slow group"_q);
	check(CheckSlowmode(true, 0, false, 2) == Block::TrackFirst,
		u"sends: the track and the control message in a slow group"_q);
	check(CheckSlowmode(true, 17, false, 1) == Block::Wait
		&& CheckSlowmode(true, 17, false, 2) == Block::Wait
		&& CheckSlowmode(true, 1, true, 1) == Block::Wait,
		u"sends: the countdown of the slow mode"_q);
	check(CheckSlowmode(true, 0, true, 1) == Block::Busy,
		u"sends: a message is being sent to a slow group"_q);

	check(SendRefusalSlowmode(u"SLOWMODE_WAIT_25"_q)
		&& !SendRefusalSlowmode(u"FLOOD_WAIT_25"_q)
		&& !SendRefusalExplained(u"SLOWMODE_WAIT_25"_q),
		u"sends: refused by the slow mode"_q);
	check(SendRefusalExplained(u"PEER_FLOOD"_q)
		&& SendRefusalExplained(u"USER_BANNED_IN_CHANNEL"_q)
		&& SendRefusalExplained(u"CHAT_FORWARDS_RESTRICTED"_q)
		&& SendRefusalExplained(u"ALLOW_PAYMENT_REQUIRED_50"_q),
		u"sends: the refusals that upstream explains"_q);
	check(!SendRefusalExplained(u"CHAT_SEND_AUDIOS_FORBIDDEN"_q)
		&& !SendRefusalExplained(u"MESSAGE_ID_INVALID"_q)
		&& !SendRefusalExplained(QString()),
		u"sends: the refusals that get the toast of the session"_q);
}

// The clock: see ClockSync.
void TestClock(Check &check) {
	const auto id = [](int64 serverMs) {
		// Only the seconds are given, the rest of a real id is noise.
		return (uint64(serverMs / 1000) << 32) | uint64(0xABCDEF01U);
	};
	{
		auto sync = ClockSync();
		check(!sync.ready() && sync.apply(1234) == 1234,
			u"clock: nothing is changed without answers"_q);
	}
	{
		// A right clock: whatever the answers say, it stays as it is.
		auto sync = ClockSync();
		const auto start = int64(1'790'000'000'000);
		for (auto i = 0; i != kProbeCount; ++i) {
			const auto sent = start + i * int64(kProbeGap);
			const auto server = sent + 60;
			sync.add(sent, sent + 120, id(server));
		}
		check(sync.ready() && sync.apply(0) == 0,
			u"clock: a right clock is left alone"_q);
	}
	for (const auto wrong : { int64(2300), int64(-1700), int64(40'000) }) {
		// The local clock is ahead of the server by wrong milliseconds
		// (the app knows only the whole three seconds of it or nothing).
		auto sync = ClockSync();
		const auto start = int64(1'790'000'000'000) + 137;
		for (auto i = 0; i != kProbeCount; ++i) {
			const auto sent = start + i * int64(kProbeGap);
			const auto server = (sent - wrong) + 45;
			sync.add(sent, sent + 90, id(server));
		}
		const auto prior = (std::abs(wrong) < 3000)
			? int64(0)
			: -(wrong / 1000) * 1000;
		const auto error = std::abs(sync.apply(prior) + wrong);
		check(error <= 450,
			u"clock: a clock wrong by %1 ms is corrected"_q.arg(wrong));
	}
	{
		// One answer alone bounds the difference to a second and the
		// round trip.
		auto sync = ClockSync();
		const auto sent = int64(1'790'000'000'000) + 900;
		sync.add(sent, sent + 200, id(sent + 100 + 5000));
		const auto offset = sync.apply(0);
		check(offset >= 5000 - 900 - 200 && offset <= 5000 + 100,
			u"clock: one answer"_q);
	}
	{
		// Slow answers, answers from the past and broken ids say
		// nothing; a clock that was set meanwhile starts over.
		auto sync = ClockSync();
		const auto sent = int64(1'790'000'000'000);
		sync.add(sent, sent + kProbeMaxTrip + 1, id(sent));
		sync.add(sent, sent - 10, id(sent));
		sync.add(sent, sent + 10, 0);
		check(!sync.ready(), u"clock: useless answers are skipped"_q);
		sync.add(sent, sent + 100, id(sent + 50));
		sync.add(sent + 2000, sent + 2100, id(sent + 2050 + 30'000));
		const auto offset = sync.apply(0);
		check(offset >= 28'000 && offset <= 31'000,
			u"clock: starts over when the clock is set"_q);
		sync.reset();
		check(!sync.ready() && sync.apply(7) == 7, u"clock: reset"_q);
	}
}

void TestSync(Check &check) {
	const auto track = Track{ .id = 10, .document = 70, .date = 1000 };
	const auto other = Track{ .id = 11, .document = 71, .date = 1100 };
	const auto copy = Track{ .id = 12, .document = 70, .date = 1200 };
	auto state = State{
		.session = 5,
		.seq = 1,
		.track = track,
		.position = 60'000,
		.at = 1'000'000,
		.duration = 200'000,
		.playing = true,
	};
	check(ExpectedPosition(state, 1'000'000) == 60'000,
		u"sync: the position at the moment of the state"_q);
	check(ExpectedPosition(state, 1'030'500) == 90'500,
		u"sync: the position grows with the time"_q);
	check(ExpectedPosition(state, 999'000) == 60'000,
		u"sync: a clock that is behind"_q);
	check(ExpectedPosition(state, 5'000'000) == 200'000,
		u"sync: not further than the end"_q);
	check(LiveLeft(state, 1'000'000) == 140'000 + kHostGrace,
		u"sync: live till the end of the track and the grace"_q);
	check(LiveLeft(state, 1'000'000 + 140'000 + kHostGrace) <= 0,
		u"sync: a vanished host"_q);
	state.playing = false;
	check(ExpectedPosition(state, 1'030'500) == 60'000,
		u"sync: a paused track stays"_q);
	check(LiveLeft(state, 1'000'000 + kPausedLive - 1) > 0
		&& LiveLeft(state, 1'000'000 + kPausedLive) <= 0,
		u"sync: a pause forgotten for hours"_q);
	state.ended = true;
	check(LiveLeft(state, 1'000'000) <= 0, u"sync: an ended session"_q);
	state.ended = false;
	state.at = 1'000'000 + kFutureSkew + 1;
	check(LiveLeft(state, 1'000'000) <= 0, u"sync: a state from the future"_q);

	// When the host has to edit the message: never for a track that
	// simply plays, the reports of the player jitter by a tenth of
	// a second.
	state.at = 1'000'000;
	state.playing = true;
	auto edits = 0;
	for (auto passed = int64(0); passed <= 139'000; passed += 100) {
		const auto jitter = ((passed / 100) % 7) * 40 - 120;
		edits += HostMoved(
			state,
			track,
			true,
			60'000 + passed + jitter,
			200'000,
			1'000'000 + passed) ? 1 : 0;
	}
	check(!edits, u"sync: no edits while the track plays"_q);
	check(HostMoved(state, track, true, 95'000, 200'000, 1'030'000)
		&& HostMoved(state, track, true, 85'000, 200'000, 1'030'000),
		u"sync: an edit after a seek of the host"_q);
	check(HostMoved(state, track, false, 90'000, 200'000, 1'030'000),
		u"sync: an edit after a pause"_q);
	check(HostMoved(state, other, true, 90'000, 200'000, 1'030'000)
		&& HostMoved(state, copy, true, 90'000, 200'000, 1'030'000),
		u"sync: an edit after the next track"_q);
	state.playing = false;
	check(!HostMoved(state, track, false, 60'100, 200'000, 1'500'000)
		&& HostMoved(state, track, false, 62'000, 200'000, 1'500'000)
		&& HostMoved(state, track, true, 60'000, 200'000, 1'500'000),
		u"sync: edits of a paused track"_q);
	state.away = true;
	check(HostMoved(state, track, false, 60'000, 200'000, 1'500'000),
		u"sync: an edit when the host is back to a track of the chat"_q);
	state.away = false;
	state.duration = 0;
	check(HostMoved(state, track, false, 60'000, 200'000, 1'500'000)
		&& !HostMoved(state, track, false, 60'000, 0, 1'500'000),
		u"sync: an edit when the length becomes known"_q);

	auto pausedAway = state;
	pausedAway.away = true;
	check(!SameContent(state, pausedAway),
		u"sync: the reason of a pause is a change"_q);

	{
		// What the player of the host reports and when its position is
		// trusted: a track that plays, a seek, a wait for data in the
		// middle of the track (the position stands still for a second
		// and a half while the reports go on), the track goes on.
		const auto peer = PeerId(uint64(1));
		const auto id = FullMsgId(peer, MsgId(10));
		auto progress = Progress();
		auto now = crl::time(1'000'000);
		auto position = int64(0);
		UpdateProgress(progress, id, true, true, position, now);
		check(!progress.moving, u"sync: a track that has just started"_q);
		auto dropped = 0;
		for (auto i = 0; i != 100; ++i) {
			now += 50;
			position += 50;
			UpdateProgress(progress, id, true, true, position, now);
			dropped += progress.moving ? 0 : 1;
		}
		check(!dropped, u"sync: a track that plays is moving"_q);

		position = 90'000;
		UpdateProgress(progress, id, true, true, position, now + 50);
		check(!progress.moving, u"sync: a seek is not a movement"_q);
		now += 100;
		position += 50;
		UpdateProgress(progress, id, true, true, position, now);
		check(progress.moving, u"sync: moving again after the seek"_q);

		auto stalledAfter = crl::time(0);
		const auto stalledAt = now;
		for (auto i = 0; i != 30; ++i) {
			now += 50;
			UpdateProgress(progress, id, true, true, position, now);
			if (!progress.moving && !stalledAfter) {
				stalledAfter = now - stalledAt;
			}
		}
		check(stalledAfter > kHostTolerance && stalledAfter <= 800,
			u"sync: a wait for data is seen in under a second"_q);
		check(!progress.moving && progress.position == position,
			u"sync: a track that waits for data is not moving"_q);
		now += 50;
		position += 50;
		UpdateProgress(progress, id, true, true, position, now);
		check(progress.moving, u"sync: moving again after the wait"_q);

		// A pause of the host is not a wait for data.
		for (auto i = 0; i != 100; ++i) {
			now += 50;
			UpdateProgress(progress, id, true, false, position, now);
		}
		check(progress.moving, u"sync: a pause keeps the flag"_q);

		const auto next = FullMsgId(peer, MsgId(11));
		UpdateProgress(progress, next, true, true, 0, now);
		check(!progress.moving, u"sync: another track starts over"_q);
		UpdateProgress(progress, FullMsgId(), false, false, 0, now + 50);
		check(!progress.moving, u"sync: an empty player"_q);
	}

	// The host is at 10 s at the moment 0 and plays on.
	const auto host = [](crl::time now) {
		return int64(10'000 + now);
	};
	{
		// The track has needed 0.8 s to load: under the threshold.
		auto corrector = Corrector();
		corrector.started(0, 10'000, true);
		check(corrector.loading(), u"sync: loading after a cold start"_q);
		auto seeks = 0;
		for (auto now = crl::time(100); now <= 20'000; now += 100) {
			const auto actual = (now < 800)
				? int64(10'000)
				: int64(10'000 + (now - 800));
			seeks += corrector.update(now, host(now), actual).seek ? 1 : 0;
		}
		check(!seeks, u"sync: no seek for a small drift"_q);
		check(!corrector.loading(), u"sync: loaded"_q);
	}
	{
		// The track has needed 3 s to load: one seek ahead of the host
		// by the lead, the seek itself takes 0.2 s, then all is quiet.
		auto corrector = Corrector();
		corrector.started(0, 10'000, true);
		auto seeks = 0;
		auto base = int64(10'000);
		auto from = crl::time(3000);
		auto worst = int64(0);
		for (auto now = crl::time(100); now <= 30'000; now += 100) {
			const auto actual = (now < from) ? base : (base + (now - from));
			const auto result = corrector.update(now, host(now), actual);
			if (result.seek) {
				++seeks;
				base = result.target;
				from = now + 200;
			} else if (now > 5000) {
				worst = std::max(worst, std::abs(actual - host(now)));
			}
		}
		check(seeks == 1, u"sync: one seek after a slow start"_q);
		check(worst <= 300, u"sync: in place after the seek"_q);
		check(corrector.lead() == (kLeadDefault + 200) / 2,
			u"sync: the lead follows the seek time"_q);
	}
	{
		// A player that can't keep up: the seeks become rare.
		auto corrector = Corrector();
		corrector.started(0, 10'000, true);
		auto seeks = 0;
		auto last = crl::time(0);
		auto shortest = crl::time(1'000'000);
		for (auto now = crl::time(100); now <= 120'000; now += 100) {
			const auto actual = int64(10'000 + now / 2);
			if (corrector.update(now, host(now), actual).seek) {
				if (seeks) {
					shortest = std::min(shortest, now - last);
				}
				last = now;
				++seeks;
			}
		}
		check(seeks >= 2 && seeks <= 6,
			u"sync: a few seeks for a player that lags"_q);
		check(shortest >= kSeekCooldown,
			u"sync: the pause between two seeks"_q);
	}
	{
		// The host has sought: the new state allows a seek at once.
		auto corrector = Corrector();
		corrector.started(0, 10'000, true);
		check(!corrector.update(500, 10'500, 10'450).seek,
			u"sync: steady before the seek of the host"_q);
		corrector.reset();
		const auto result = corrector.update(600, 100'000, 10'550);
		check(result.seek && result.target == 100'000 + kLeadDefault,
			u"sync: follows the seek of the host"_q);
		check(!corrector.update(700, 100'100, result.target).seek,
			u"sync: waits for the seek to work"_q);
	}
}

void TestThrottle(Check &check) {
	{
		auto throttle = Throttle();
		check(!throttle.delay(0), u"throttle: nothing to send"_q);
		throttle.request();
		check(throttle.delay(1000) == crl::time(0),
			u"throttle: the first edit goes at once"_q);
		throttle.sent(1000);
		throttle.request();
		throttle.request();
		check(!throttle.delay(1100), u"throttle: one request in flight"_q);
		throttle.done();
		check(throttle.delay(1300) == crl::time(1700),
			u"throttle: two seconds between the edits"_q);
		check(throttle.delay(3000) == crl::time(0),
			u"throttle: the coalesced edit goes after the gap"_q);
		throttle.sent(3000);
		throttle.done();
		check(!throttle.delay(9000), u"throttle: everything is sent"_q);
	}
	{
		// A host that never stops changing something: an edit is asked
		// for every 100 ms for two hours, each one takes 150 ms.
		auto throttle = Throttle();
		auto sends = std::vector<crl::time>();
		auto doneAt = crl::time(-1);
		for (auto now = crl::time(0); now < 120 * kMinute; now += 50) {
			if (doneAt >= 0 && now >= doneAt) {
				throttle.done();
				doneAt = -1;
			}
			if (!(now % 100)) {
				throttle.request();
			}
			if (throttle.delay(now) == crl::time(0)) {
				throttle.sent(now);
				sends.push_back(now);
				doneAt = now + 150;
			}
		}
		auto shortest = kMinute;
		auto most = 0;
		auto firstMinute = 0;
		auto firstTen = 0;
		auto secondHour = 0;
		for (auto i = 0; i != int(sends.size()); ++i) {
			if (i) {
				shortest = std::min(shortest, sends[i] - sends[i - 1]);
			}
			auto inMinute = 0;
			for (auto j = i; j != int(sends.size()); ++j) {
				if (sends[j] < sends[i] + kMinute) {
					++inMinute;
				}
			}
			most = std::max(most, inMinute);
			firstMinute += (sends[i] < kMinute) ? 1 : 0;
			firstTen += (sends[i] < 10 * kMinute) ? 1 : 0;
			secondHour += (sends[i] >= 60 * kMinute) ? 1 : 0;
		}
		check(shortest >= kMinEditGap, u"throttle: the shortest gap"_q);
		check(most <= kEditsPerMinute, u"throttle: edits in a minute"_q);
		check(firstMinute == kEditsPerMinute,
			u"throttle: a busy minute is allowed"_q);
		check(firstTen >= kEditBurst && firstTen <= kEditBurst + 35,
			u"throttle: edits in ten busy minutes"_q);
		check(secondHour >= 175 && secondHour <= 181,
			u"throttle: three edits in a minute over a long time"_q);

		// After half an hour of quiet the host may be busy again.
		const auto later = 150 * kMinute;
		auto burst = 0;
		for (auto now = later; now < later + kMinute; now += 50) {
			throttle.done();
			throttle.request();
			if (throttle.delay(now) == crl::time(0)) {
				throttle.sent(now);
				++burst;
			}
		}
		check(burst == kEditsPerMinute,
			u"throttle: the reserve comes back"_q);
	}
	{
		auto throttle = Throttle();
		throttle.request();
		throttle.sent(0);
		throttle.flood(100, 30);
		check(throttle.flooded(100) && throttle.dirty(),
			u"throttle: the edit is kept after FLOOD_WAIT"_q);
		check(throttle.delay(100) == crl::time(31'000),
			u"throttle: FLOOD_WAIT is waited out"_q);
		check(throttle.delay(31'100) == crl::time(0)
			&& !throttle.flooded(31'100),
			u"throttle: goes on after FLOOD_WAIT"_q);
		throttle.sent(31'100);
		throttle.done();
		throttle.request();
		check(throttle.delay(31'200) == crl::time(2 * kMinEditGap - 100),
			u"throttle: slower after FLOOD_WAIT"_q);
	}
	{
		auto throttle = Throttle();
		throttle.request();
		throttle.sent(0);
		throttle.failed(100);
		check(!throttle.broken()
			&& throttle.delay(100) == kFailDelay,
			u"throttle: a pause after an error"_q);
		throttle.sent(6000);
		throttle.failed(6100);
		check(throttle.delay(6100) == 2 * kFailDelay,
			u"throttle: a longer pause after the second error"_q);
		throttle.sent(17'000);
		throttle.failed(17'100);
		check(throttle.broken(), u"throttle: gives up after three"_q);
	}
	{
		auto throttle = Throttle();
		throttle.request();
		throttle.sent(0);
		throttle.failed(100);
		throttle.sent(6000);
		throttle.done();
		throttle.request();
		throttle.sent(9000);
		throttle.failed(9100);
		check(!throttle.broken() && throttle.delay(9100) == kFailDelay,
			u"throttle: a success forgets the errors"_q);
	}
	{
		// "End" right after a busy minute: a usual edit would wait
		// longer than the app waits for the last one, the last one
		// waits only for the two seconds and for FLOOD_WAIT.
		auto throttle = Throttle();
		auto now = crl::time(0);
		for (auto i = 0; i != kEditsPerMinute; ++i) {
			throttle.request();
			throttle.sent(now);
			throttle.done();
			now += kMinEditGap;
		}
		now -= kMinEditGap - 500;
		throttle.request();
		const auto usual = throttle.delay(now);
		check(usual && (*usual > kEndTimeout),
			u"throttle: a usual edit waits after a busy minute"_q);
		check(throttle.delay(now, true) == kMinEditGap - 500,
			u"throttle: the last edit waits only for the gap"_q);
		throttle.flood(now, 10);
		check(throttle.delay(now, true) == crl::time(11'000),
			u"throttle: the last edit waits out FLOOD_WAIT"_q);
	}
	check(FloodSeconds(u"FLOOD_WAIT_17"_q) == 17
		&& FloodSeconds(u"FLOOD_PREMIUM_WAIT_4"_q) == 4
		&& FloodSeconds(u"FLOOD_WAIT_"_q) == 1
		&& FloodSeconds(u"FLOOD"_q) == 1
		&& FloodSeconds(u"FLOOD_WAIT_99999999"_q) == 24 * 60 * 60,
		u"throttle: seconds of FLOOD_WAIT"_q);
}

} // namespace

void Start(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		FullMsgId track) {
	Manager::Instance().start(controller, peer, track);
}

bool IsControlEdit(
		not_null<const HistoryItem*> item,
		const TextWithEntities &was,
		const TextWithEntities &now) {
	return item->isRegular()
		&& !item->media()
		&& WrittenToChat(item)
		&& item->from()->isUser()
		&& IsSessionEdit(was, now);
}

bool RunSelfTest(QStringList &log) {
	auto check = Check{ log };
	TestCodec(check);
	TestGarbage(check);
	TestEdits(check);
	TestTracks(check);
	TestSends(check);
	TestClock(check);
	TestSync(check);
	TestThrottle(check);
	log.push_back(u"listen: %1 checks passed, %2 failed"_q
		.arg(check.passed)
		.arg(check.failed));
	return !check.failed;
}

void AddStartAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const auto manager = Manager::Existing();
	if (!Get().listenTogether()
		|| !TrackDocument(item)
		|| !item->isRegular()
		|| !IsServerMsgId(item->id)
		|| (&peer->session() != &controller->session())
		|| !PeerFits(peer)
		|| !Data::CanSendTexts(peer)
		|| (peer->starsPerMessageChecked() > 0)
		|| (manager && manager->hosting())) {
		return;
	}
	const auto id = item->fullId();
	const auto weak = base::make_weak(controller);
	menu->addAction(tr::lng_oblivion_listen_menu(tr::now), [=] {
		if (const auto strong = weak.get()) {
			Start(strong, peer, id);
		}
	}, &st::menuIconSoundOn);
}

void AddStartAction(
		const Ui::Menu::MenuCallback &addAction,
		not_null<Window::SessionController*> controller,
		PeerData *peer) {
	const auto manager = Manager::Existing();
	if (!peer
		|| !Get().listenTogether()
		|| (&peer->session() != &controller->session())
		|| !PeerFits(peer)
		|| !Data::CanSendTexts(peer)
		|| (peer->starsPerMessageChecked() > 0)
		|| (manager && manager->hosting())) {
		return;
	}
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto document = current.audio();
	const auto id = current.contextId();
	if (!document || !id || (&document->session() != &peer->session())) {
		return;
	}
	const auto item = peer->owner().message(id);
	if (!item || TrackDocument(item) != document || !item->isRegular()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	addAction(tr::lng_oblivion_listen_menu(tr::now), [=] {
		if (const auto strong = weak.get()) {
			Start(strong, peer, id);
		}
	}, &st::menuIconSoundOn);
}

bool OpenControlLink(const QString &url, const QVariant &context) {
	if (!url.contains(Marker()) || !Get().listenTogether()) {
		return false;
	}
	const auto my = context.value<ClickHandlerContext>();
	const auto controller = my.sessionWindow.get();
	if (!controller || !my.itemId) {
		return false;
	}
	const auto item = controller->session().data().message(my.itemId);
	return item && Manager::Instance().clicked(controller, item);
}

bool Follows(const AudioMsgId &current) {
	const auto manager = Manager::Existing();
	return manager && manager->follows(current);
}

bool FollowerMove(int delta, bool autonext) {
	const auto manager = Manager::Existing();
	return manager && manager->followerMove(delta, autonext);
}

rpl::producer<> FollowChanges() {
	return FollowStream().events();
}

crl::time TakeStartPosition(const AudioMsgId &audioId) {
	auto &pending = PendingStart();
	if (pending.position < 0
		|| !pending.document
		|| pending.document != audioId.audio()
		|| pending.id != audioId.contextId()) {
		return -1;
	}
	return std::exchange(pending.position, crl::time(-1));
}

bool IsQuitPrevent() {
	const auto manager = Manager::Existing();
	return manager && manager->quitPrevent();
}

bool ChatFits(not_null<PeerData*> peer) {
	return PeerFits(peer);
}

ChatView Lookup(not_null<PeerData*> peer) {
	return Manager::Instance().lookup(peer);
}

rpl::producer<> Changes() {
	return Manager::Instance().changes();
}

bool Rescan(not_null<PeerData*> peer) {
	return Manager::Instance().rescan(peer);
}

void Join(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	Manager::Instance().join(controller, peer);
}

void Leave() {
	if (const auto manager = Manager::Existing()) {
		manager->leave();
	}
}

void End() {
	if (const auto manager = Manager::Existing()) {
		manager->finishHosting();
	}
}

void SendTrack(uint64 sendId) {
	if (const auto manager = Manager::Existing()) {
		manager->sendTrack(sendId);
	}
}

} // namespace Oblivion::Listen
