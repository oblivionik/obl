/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_settings.h"

#include "settings.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Oblivion {
namespace {

constexpr auto kAllowCopyProtected = "allow_copy_protected";
constexpr auto kAllowSaveProtected = "allow_save_protected";
constexpr auto kStoriesWithoutPremium = "stories_without_premium";
constexpr auto kShowPeerIds = "show_peer_ids";
constexpr auto kShowRegistrationDate = "show_registration_date";
constexpr auto kShowMessageDetails = "show_message_details";
constexpr auto kExportStickerJson = "export_sticker_json";
constexpr auto kFakePremium = "fake_premium";
constexpr auto kFakeStars = "fake_stars";
constexpr auto kFakeTon = "fake_ton";
constexpr auto kVisualGifts = "visual_gifts";
constexpr auto kVisualGifting = "visual_gifting";
constexpr auto kGhostRead = "ghost_read";
constexpr auto kGhostTyping = "ghost_typing";
constexpr auto kGhostOnline = "ghost_online";
constexpr auto kHideSponsored = "hide_sponsored";
constexpr auto kKeepDeleted = "keep_deleted";
constexpr auto kGhostStories = "ghost_stories";
constexpr auto kOfflineSend = "offline_send";
constexpr auto kReadOnSend = "read_on_send";
constexpr auto kGhostReadExceptions = "ghost_read_exceptions";
constexpr auto kKeepEditHistory = "keep_edit_history";
constexpr auto kSaveSelfDestructing = "save_self_destructing";
constexpr auto kForwardProtectedAsCopy = "forward_protected_copy";
constexpr auto kShowSeconds = "show_seconds";
constexpr auto kLocalNames = "local_names";
constexpr auto kHideStoriesBar = "hide_stories_bar";
constexpr auto kHidePremiumPromo = "hide_premium_promo";
constexpr auto kHideGiftPromo = "hide_gift_promo";
constexpr auto kLocalTranscribe = "local_transcribe";
constexpr auto kTranscribeLanguage = "transcribe_language";
constexpr auto kAppIcon = "app_icon";
constexpr auto kAppIconFinder = "app_icon_finder";
constexpr auto kAppIconDigest = "app_icon_digest";
constexpr auto kVoiceEffect = "voice_effect";
constexpr auto kPhotoExport = "photo_export";
constexpr auto kOnlineJournal = "online_journal";
constexpr auto kOnlineNotify = "online_notify";
constexpr auto kOnlinePolling = "online_polling";
constexpr auto kProfileHistory = "profile_history";
constexpr auto kGhostButton = "ghost_button";
constexpr auto kGhostPreset = "ghost_preset";
constexpr auto kGhostButtonOwned = "ghost_button_owned";
constexpr auto kUnifiedChats = "unified_chats";
constexpr auto kVoiceNoiseSuppression = "voice_noise_suppression";
constexpr auto kAttachTools = "attach_tools";
constexpr auto kBadgeEnabled = "badge_enabled";
constexpr auto kBadgeAsked = "badge_asked";
constexpr auto kListenTogether = "listen_together";
// Round 5: cloud.
constexpr auto kCloudLinks = "cloud_links";
// Round 5: cloud end.
// Round 5: rooms.
constexpr auto kCloudRooms = "cloud_rooms";
constexpr auto kRoomMusicVolume = "room_music_volume";
constexpr auto kRoomVideoVolume = "room_video_volume";
// Round 5: rooms end.
// Round 5: room extras.
constexpr auto kRoomReactions = "room_reactions";
// Round 5: room extras end.
// Round 5: social.
constexpr auto kCloudBadge = "cloud_badge";
constexpr auto kCloudBadgeShow = "cloud_badge_show";
constexpr auto kCloudProfileShow = "cloud_profile_show";
constexpr auto kCloudFriends = "cloud_friends";
constexpr auto kCloudProfileAudience = "cloud_profile_audience";
constexpr auto kCloudActivityAudience = "cloud_activity_audience";
constexpr auto kCloudChipListening = "cloud_chip_listening";
constexpr auto kCloudChipRoom = "cloud_chip_room";
constexpr auto kCloudChipOnline = "cloud_chip_online";
constexpr auto kCloudChosen = "cloud_chosen";
// Round 5: social end.
// Round 5: sync.
constexpr auto kCloudSettingsAutoSync = "cloud_settings_auto_sync";
// Round 5: sync end.
// Round 5: update.
constexpr auto kCloudUpdateCheck = "cloud_update_check";
constexpr auto kCloudUpdateLastCheck = "cloud_update_last_check";
constexpr auto kCloudUpdateSeenBuild = "cloud_update_seen_build";
// Round 5: update end.
// Round 5: send online.
constexpr auto kSendWhenOnline = "send_when_online";
constexpr auto kSendWhenOnlineHours = "send_when_online_hours";
// Round 5: send online end.
// Oblivion looks: core.
constexpr auto kLook = "look";
// Oblivion looks: core end.

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/oblivion.json"_q;
}

} // namespace

Settings &Settings::Instance() {
	static auto result = Settings();
	return result;
}

Settings::Settings() {
	load();
}

void Settings::apply(bool &field, bool value) {
	if (field == value) {
		return;
	}
	field = value;
	save();
	_changes.fire({});
}

void Settings::apply(int64 &field, int64 value) {
	if (field == value) {
		return;
	}
	field = value;
	save();
	_changes.fire({});
}

void Settings::apply(QString &field, QString value) {
	if (field == value) {
		return;
	}
	field = std::move(value);
	changed();
}

void Settings::changed() {
	save();
	_changes.fire({});
}

void Settings::setGhostReadException(uint64 peerId, bool exception) {
	if (!peerId || (isGhostReadException(peerId) == exception)) {
		return;
	} else if (exception) {
		_ghostReadExceptions.emplace(peerId);
	} else {
		_ghostReadExceptions.remove(peerId);
	}
	changed();
}

void Settings::clearGhostReadExceptions() {
	if (_ghostReadExceptions.empty()) {
		return;
	}
	_ghostReadExceptions.clear();
	changed();
}

QString Settings::localName(uint64 peerId) const {
	const auto i = _localNames.find(peerId);
	return (i != end(_localNames)) ? i->second : QString();
}

void Settings::setLocalName(uint64 peerId, const QString &name) {
	const auto trimmed = name.trimmed();
	if (!peerId || (localName(peerId) == trimmed)) {
		return;
	} else if (trimmed.isEmpty()) {
		_localNames.remove(peerId);
	} else {
		_localNames[peerId] = trimmed;
	}
	changed();
}

void Settings::setAppIconDigest(uint64 value) {
	if (_appIconDigest == value) {
		return;
	}
	_appIconDigest = value;
	changed();
}

void Settings::setOnlineNotify(uint64 peerId, bool notify) {
	if (!peerId || (isOnlineNotify(peerId) == notify)) {
		return;
	} else if (notify) {
		_onlineNotify.emplace(peerId);
	} else {
		_onlineNotify.remove(peerId);
	}
	changed();
}

void Settings::setGhostPreset(GhostPreset value) {
	if (_ghostPreset == value) {
		return;
	}
	_ghostPreset = value;
	changed();
}

void Settings::setGhostButtonOwned(GhostPreset value) {
	if (_ghostButtonOwned == value) {
		return;
	}
	_ghostButtonOwned = value;
	changed();
}

// Round 5: social.
void Settings::setCloudChosen(uint64 userId, bool chosen) {
	if (!userId || (isCloudChosen(userId) == chosen)) {
		return;
	} else if (chosen) {
		_cloudChosen.emplace(userId);
	} else {
		_cloudChosen.remove(userId);
	}
	changed();
}
// Round 5: social end.

rpl::producer<> Settings::changes() const {
	return _changes.events();
}

// Round 5: sync.
QByteArray Settings::syncSnapshot() {
	const auto read = [] {
		auto file = QFile(FilePath());
		return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
	};
	// A file that was saved by an older build has no keys for the
	// settings added since, and the sync takes values only for the keys
	// the file of this device has: written anew once per launch, the
	// file has them all (with the values that are in use anyway).
	static auto Completed = false;
	if (!std::exchange(Completed, true)) {
		save();
	}
	auto result = read();
	if (!QJsonDocument::fromJson(result).isObject()) {
		// Nothing was changed since the installation: the defaults.
		save();
		result = read();
	}
	return result;
}

bool Settings::syncApply(const QByteArray &json) {
	if (!QJsonDocument::fromJson(json).isObject()) {
		return false;
	}
	auto file = QSaveFile(FilePath());
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(json);
	if (!file.commit()) {
		return false;
	}
	load();
	_changes.fire({});
	return true;
}
// Round 5: sync end.

void Settings::load() {
	auto file = QFile(FilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto data = file.readAll();
	file.close();
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(data, &error);
	if (!document.isObject()) {
		LOG(("Oblivion Error: Could not parse settings: %1."
			).arg(error.errorString()));
		if (!data.isEmpty()) {
			// Keep the unreadable copy aside, so that the next save()
			// with the defaults does not destroy the only copy of it.
			const auto backup = FilePath() + u".bad"_q;
			QFile::remove(backup);
			QFile::rename(FilePath(), backup);
		}
		return;
	}
	const auto object = document.object();
	const auto read = [&](const char *key, bool &field) {
		const auto value = object.value(QString::fromUtf8(key));
		if (value.isBool()) {
			field = value.toBool();
		}
	};
	read(kAllowCopyProtected, _allowCopyProtected);
	read(kAllowSaveProtected, _allowSaveProtected);
	read(kStoriesWithoutPremium, _storiesWithoutPremium);
	read(kShowPeerIds, _showPeerIds);
	read(kShowRegistrationDate, _showRegistrationDate);
	read(kShowMessageDetails, _showMessageDetails);
	read(kExportStickerJson, _exportStickerJson);
	read(kFakePremium, _fakePremium);
	read(kVisualGifting, _visualGifting);
	read(kGhostRead, _ghostRead);
	read(kGhostTyping, _ghostTyping);
	read(kGhostOnline, _ghostOnline);
	read(kHideSponsored, _hideSponsored);
	read(kKeepDeleted, _keepDeleted);
	read(kGhostStories, _ghostStories);
	read(kOfflineSend, _offlineSend);
	read(kReadOnSend, _readOnSend);
	read(kKeepEditHistory, _keepEditHistory);
	read(kSaveSelfDestructing, _saveSelfDestructing);
	read(kForwardProtectedAsCopy, _forwardProtectedAsCopy);
	read(kShowSeconds, _showSeconds);
	read(kHideStoriesBar, _hideStoriesBar);
	read(kHidePremiumPromo, _hidePremiumPromo);
	read(kHideGiftPromo, _hideGiftPromo);
	read(kLocalTranscribe, _localTranscribe);

	const auto language = object.value(
		QString::fromUtf8(kTranscribeLanguage));
	if (language.isString()) {
		_transcribeLanguage = language.toString();
	}

	read(kAppIconFinder, _appIconFinder);
	_appIcon = object.value(QString::fromUtf8(kAppIcon)).toString();
	_appIconDigest = object.value(
		QString::fromUtf8(kAppIconDigest)).toString().toULongLong();
	_voiceEffect = object.value(QString::fromUtf8(kVoiceEffect)).toString();
	_photoExport = object.value(QString::fromUtf8(kPhotoExport)).toString();

	_ghostReadExceptions.clear();
	const auto exceptions = object.value(
		QString::fromUtf8(kGhostReadExceptions)).toArray();
	for (const auto &value : exceptions) {
		if (const auto peerId = value.toString().toULongLong()) {
			_ghostReadExceptions.emplace(peerId);
		}
	}

	_localNames.clear();
	const auto names = object.value(QString::fromUtf8(kLocalNames)).toObject();
	for (auto i = names.begin(); i != names.end(); ++i) {
		const auto peerId = i.key().toULongLong();
		const auto name = i.value().toString().trimmed();
		if (peerId && !name.isEmpty()) {
			_localNames.emplace(peerId, name);
		}
	}

	const auto readNumber = [&](const char *key, int64 &field) {
		const auto value = object.value(QString::fromUtf8(key));
		if (value.isDouble()) {
			field = int64(value.toDouble());
		}
	};
	readNumber(kFakeStars, _fakeStars);
	readNumber(kFakeTon, _fakeTon);

	_visualGifts.clear();
	const auto list = object.value(QString::fromUtf8(kVisualGifts)).toArray();
	for (const auto &value : list) {
		const auto entry = value.toObject();
		auto gift = VisualGift();
		gift.giftId = entry.value(u"gift_id"_q).toString().toULongLong();
		gift.modelDocumentId
			= entry.value(u"model_id"_q).toString().toULongLong();
		gift.patternDocumentId
			= entry.value(u"pattern_id"_q).toString().toULongLong();
		gift.title = entry.value(u"title"_q).toString();
		gift.ownerName = entry.value(u"owner"_q).toString();
		gift.modelName = entry.value(u"model_name"_q).toString();
		gift.patternName = entry.value(u"pattern_name"_q).toString();
		gift.backdropName = entry.value(u"backdrop_name"_q).toString();
		gift.modelRarity = entry.value(u"model_rarity"_q).toInt();
		gift.patternRarity = entry.value(u"pattern_rarity"_q).toInt();
		gift.backdropRarity = entry.value(u"backdrop_rarity"_q).toInt();
		gift.centerColor = uint32(entry.value(u"center"_q).toDouble());
		gift.edgeColor = uint32(entry.value(u"edge"_q).toDouble());
		gift.patternColor = uint32(entry.value(u"pattern"_q).toDouble());
		gift.textColor = uint32(entry.value(u"text"_q).toDouble());
		gift.number = entry.value(u"number"_q).toInt();
		gift.limitedLeft = entry.value(u"limited_left"_q).toInt();
		gift.limitedCount = entry.value(u"limited_count"_q).toInt();
		gift.valuePrice = entry.value(u"value_price"_q).toString().toLongLong();
		gift.valueCurrency = entry.value(u"value_currency"_q).toString();
		gift.forPeerId = entry.value(u"peer"_q).toString().toULongLong();
		if (gift.giftId) {
			_visualGifts.push_back(std::move(gift));
		}
	}

	read(kOnlineJournal, _onlineJournal);
	_onlineNotify.clear();
	const auto notify = object.value(
		QString::fromUtf8(kOnlineNotify)).toArray();
	for (const auto &value : notify) {
		if (const auto peerId = value.toString().toULongLong()) {
			_onlineNotify.emplace(peerId);
		}
	}
	read(kOnlinePolling, _onlinePolling);
	read(kProfileHistory, _profileHistory);
	read(kGhostButton, _ghostButton);
	const auto preset = object.value(
		QString::fromUtf8(kGhostPreset)).toObject();
	const auto readPreset = [&](const QString &key, bool &field) {
		const auto value = preset.value(key);
		if (value.isBool()) {
			field = value.toBool();
		}
	};
	readPreset(u"read"_q, _ghostPreset.read);
	readPreset(u"typing"_q, _ghostPreset.typing);
	readPreset(u"online"_q, _ghostPreset.online);
	readPreset(u"stories"_q, _ghostPreset.stories);
	readPreset(u"offline_send"_q, _ghostPreset.offlineSend);
	const auto owned = object.value(
		QString::fromUtf8(kGhostButtonOwned)).toObject();
	_ghostButtonOwned.read = owned.value(u"read"_q).toBool();
	_ghostButtonOwned.typing = owned.value(u"typing"_q).toBool();
	_ghostButtonOwned.online = owned.value(u"online"_q).toBool();
	_ghostButtonOwned.stories = owned.value(u"stories"_q).toBool();
	_ghostButtonOwned.offlineSend = owned.value(u"offline_send"_q).toBool();
	read(kUnifiedChats, _unifiedChats);
	read(kVoiceNoiseSuppression, _voiceNoiseSuppression);
	read(kAttachTools, _attachTools);
	read(kBadgeEnabled, _badgeEnabled);
	read(kBadgeAsked, _badgeAsked);
	read(kListenTogether, _listenTogether);

	// Round 5: a number that must stay inside [low, high].
	const auto readRange = [&](
			const char *key,
			int64 &field,
			int64 low,
			int64 high) {
		const auto value = object.value(QString::fromUtf8(key));
		if (value.isDouble()) {
			field = std::clamp(int64(value.toDouble()), low, high);
		}
	};
	// Round 5: cloud.
	read(kCloudLinks, _cloudLinks);
	// Round 5: cloud end.
	// Round 5: rooms.
	read(kCloudRooms, _cloudRooms);
	readRange(kRoomMusicVolume, _roomMusicVolume, 0, 100);
	readRange(kRoomVideoVolume, _roomVideoVolume, 0, 100);
	// Round 5: rooms end.
	// Round 5: room extras.
	read(kRoomReactions, _roomReactions);
	// Round 5: room extras end.
	// Round 5: social.
	read(kCloudBadge, _cloudBadge);
	read(kCloudBadgeShow, _cloudBadgeShow);
	read(kCloudProfileShow, _cloudProfileShow);
	read(kCloudFriends, _cloudFriends);
	readRange(kCloudProfileAudience, _cloudProfileAudience, 0, 2);
	readRange(kCloudActivityAudience, _cloudActivityAudience, 0, 2);
	read(kCloudChipListening, _cloudChipListening);
	read(kCloudChipRoom, _cloudChipRoom);
	read(kCloudChipOnline, _cloudChipOnline);
	_cloudChosen.clear();
	const auto chosen = object.value(QString::fromUtf8(kCloudChosen)).toArray();
	for (const auto &value : chosen) {
		if (const auto userId = value.toString().toULongLong()) {
			_cloudChosen.emplace(userId);
		}
	}
	// Round 5: social end.
	// Round 5: sync.
	read(kCloudSettingsAutoSync, _cloudSettingsAutoSync);
	// Round 5: sync end.
	// Round 5: update.
	read(kCloudUpdateCheck, _cloudUpdateCheck);
	readNumber(kCloudUpdateLastCheck, _cloudUpdateLastCheck);
	readNumber(kCloudUpdateSeenBuild, _cloudUpdateSeenBuild);
	// Round 5: update end.
	// Round 5: send online.
	read(kSendWhenOnline, _sendWhenOnline);
	readRange(kSendWhenOnlineHours, _sendWhenOnlineHours, 1, 168);
	// Round 5: send online end.
	// Oblivion looks: core.
	// A number that is not a look (a file of a newer build) is the plain
	// Telegram, not the nearest look.
	if (const auto value = object.value(QString::fromUtf8(kLook))
		; value.isDouble()) {
		const auto look = int64(value.toDouble());
		_look = (look >= 0 && look <= 3) ? look : 0;
	}
	// Oblivion looks: core end.
}

void Settings::save() {
	auto object = QJsonObject();
	object.insert(QString::fromUtf8(kAllowCopyProtected), _allowCopyProtected);
	object.insert(QString::fromUtf8(kAllowSaveProtected), _allowSaveProtected);
	object.insert(
		QString::fromUtf8(kStoriesWithoutPremium),
		_storiesWithoutPremium);
	object.insert(QString::fromUtf8(kShowPeerIds), _showPeerIds);
	object.insert(
		QString::fromUtf8(kShowRegistrationDate),
		_showRegistrationDate);
	object.insert(
		QString::fromUtf8(kShowMessageDetails),
		_showMessageDetails);
	object.insert(QString::fromUtf8(kExportStickerJson), _exportStickerJson);
	object.insert(QString::fromUtf8(kFakePremium), _fakePremium);
	object.insert(QString::fromUtf8(kVisualGifting), _visualGifting);
	object.insert(QString::fromUtf8(kGhostRead), _ghostRead);
	object.insert(QString::fromUtf8(kGhostTyping), _ghostTyping);
	object.insert(QString::fromUtf8(kGhostOnline), _ghostOnline);
	object.insert(QString::fromUtf8(kHideSponsored), _hideSponsored);
	object.insert(QString::fromUtf8(kKeepDeleted), _keepDeleted);
	object.insert(QString::fromUtf8(kGhostStories), _ghostStories);
	object.insert(QString::fromUtf8(kOfflineSend), _offlineSend);
	object.insert(QString::fromUtf8(kReadOnSend), _readOnSend);
	object.insert(QString::fromUtf8(kKeepEditHistory), _keepEditHistory);
	object.insert(
		QString::fromUtf8(kSaveSelfDestructing),
		_saveSelfDestructing);
	object.insert(
		QString::fromUtf8(kForwardProtectedAsCopy),
		_forwardProtectedAsCopy);
	object.insert(QString::fromUtf8(kShowSeconds), _showSeconds);
	object.insert(QString::fromUtf8(kHideStoriesBar), _hideStoriesBar);
	object.insert(QString::fromUtf8(kHidePremiumPromo), _hidePremiumPromo);
	object.insert(QString::fromUtf8(kHideGiftPromo), _hideGiftPromo);
	object.insert(QString::fromUtf8(kLocalTranscribe), _localTranscribe);
	object.insert(
		QString::fromUtf8(kTranscribeLanguage),
		_transcribeLanguage);
	object.insert(QString::fromUtf8(kAppIcon), _appIcon);
	object.insert(QString::fromUtf8(kAppIconFinder), _appIconFinder);
	object.insert(
		QString::fromUtf8(kAppIconDigest),
		QString::number(_appIconDigest));
	object.insert(QString::fromUtf8(kVoiceEffect), _voiceEffect);
	object.insert(QString::fromUtf8(kPhotoExport), _photoExport);

	auto exceptions = QJsonArray();
	for (const auto peerId : _ghostReadExceptions) {
		exceptions.push_back(QString::number(peerId));
	}
	object.insert(QString::fromUtf8(kGhostReadExceptions), exceptions);

	auto names = QJsonObject();
	for (const auto &[peerId, name] : _localNames) {
		names.insert(QString::number(peerId), name);
	}
	object.insert(QString::fromUtf8(kLocalNames), names);

	object.insert(QString::fromUtf8(kFakeStars), double(_fakeStars));
	object.insert(QString::fromUtf8(kFakeTon), double(_fakeTon));

	auto list = QJsonArray();
	for (const auto &gift : _visualGifts) {
		auto entry = QJsonObject();
		entry.insert(u"gift_id"_q, QString::number(gift.giftId));
		entry.insert(u"model_id"_q, QString::number(gift.modelDocumentId));
		entry.insert(u"pattern_id"_q, QString::number(gift.patternDocumentId));
		entry.insert(u"title"_q, gift.title);
		entry.insert(u"owner"_q, gift.ownerName);
		entry.insert(u"model_name"_q, gift.modelName);
		entry.insert(u"pattern_name"_q, gift.patternName);
		entry.insert(u"backdrop_name"_q, gift.backdropName);
		entry.insert(u"model_rarity"_q, gift.modelRarity);
		entry.insert(u"pattern_rarity"_q, gift.patternRarity);
		entry.insert(u"backdrop_rarity"_q, gift.backdropRarity);
		entry.insert(u"center"_q, double(gift.centerColor));
		entry.insert(u"edge"_q, double(gift.edgeColor));
		entry.insert(u"pattern"_q, double(gift.patternColor));
		entry.insert(u"text"_q, double(gift.textColor));
		entry.insert(u"number"_q, gift.number);
		entry.insert(u"limited_left"_q, gift.limitedLeft);
		entry.insert(u"limited_count"_q, gift.limitedCount);
		entry.insert(u"value_price"_q, QString::number(gift.valuePrice));
		entry.insert(u"value_currency"_q, gift.valueCurrency);
		entry.insert(u"peer"_q, QString::number(gift.forPeerId));
		list.push_back(entry);
	}
	object.insert(QString::fromUtf8(kVisualGifts), list);

	object.insert(QString::fromUtf8(kOnlineJournal), _onlineJournal);
	auto notify = QJsonArray();
	for (const auto peerId : _onlineNotify) {
		notify.push_back(QString::number(peerId));
	}
	object.insert(QString::fromUtf8(kOnlineNotify), notify);
	object.insert(QString::fromUtf8(kOnlinePolling), _onlinePolling);
	object.insert(QString::fromUtf8(kProfileHistory), _profileHistory);
	object.insert(QString::fromUtf8(kGhostButton), _ghostButton);
	auto preset = QJsonObject();
	preset.insert(u"read"_q, _ghostPreset.read);
	preset.insert(u"typing"_q, _ghostPreset.typing);
	preset.insert(u"online"_q, _ghostPreset.online);
	preset.insert(u"stories"_q, _ghostPreset.stories);
	preset.insert(u"offline_send"_q, _ghostPreset.offlineSend);
	object.insert(QString::fromUtf8(kGhostPreset), preset);
	auto owned = QJsonObject();
	owned.insert(u"read"_q, _ghostButtonOwned.read);
	owned.insert(u"typing"_q, _ghostButtonOwned.typing);
	owned.insert(u"online"_q, _ghostButtonOwned.online);
	owned.insert(u"stories"_q, _ghostButtonOwned.stories);
	owned.insert(u"offline_send"_q, _ghostButtonOwned.offlineSend);
	object.insert(QString::fromUtf8(kGhostButtonOwned), owned);
	object.insert(QString::fromUtf8(kUnifiedChats), _unifiedChats);
	object.insert(
		QString::fromUtf8(kVoiceNoiseSuppression),
		_voiceNoiseSuppression);
	object.insert(QString::fromUtf8(kAttachTools), _attachTools);
	object.insert(QString::fromUtf8(kBadgeEnabled), _badgeEnabled);
	object.insert(QString::fromUtf8(kBadgeAsked), _badgeAsked);
	object.insert(QString::fromUtf8(kListenTogether), _listenTogether);

	// Round 5: cloud.
	object.insert(QString::fromUtf8(kCloudLinks), _cloudLinks);
	// Round 5: cloud end.
	// Round 5: rooms.
	object.insert(QString::fromUtf8(kCloudRooms), _cloudRooms);
	object.insert(
		QString::fromUtf8(kRoomMusicVolume),
		double(_roomMusicVolume));
	object.insert(
		QString::fromUtf8(kRoomVideoVolume),
		double(_roomVideoVolume));
	// Round 5: rooms end.
	// Round 5: room extras.
	object.insert(QString::fromUtf8(kRoomReactions), _roomReactions);
	// Round 5: room extras end.
	// Round 5: social.
	object.insert(QString::fromUtf8(kCloudBadge), _cloudBadge);
	object.insert(QString::fromUtf8(kCloudBadgeShow), _cloudBadgeShow);
	object.insert(QString::fromUtf8(kCloudProfileShow), _cloudProfileShow);
	object.insert(QString::fromUtf8(kCloudFriends), _cloudFriends);
	object.insert(
		QString::fromUtf8(kCloudProfileAudience),
		double(_cloudProfileAudience));
	object.insert(
		QString::fromUtf8(kCloudActivityAudience),
		double(_cloudActivityAudience));
	object.insert(
		QString::fromUtf8(kCloudChipListening),
		_cloudChipListening);
	object.insert(QString::fromUtf8(kCloudChipRoom), _cloudChipRoom);
	object.insert(QString::fromUtf8(kCloudChipOnline), _cloudChipOnline);
	auto chosen = QJsonArray();
	for (const auto userId : _cloudChosen) {
		chosen.push_back(QString::number(userId));
	}
	object.insert(QString::fromUtf8(kCloudChosen), chosen);
	// Round 5: social end.
	// Round 5: sync.
	object.insert(
		QString::fromUtf8(kCloudSettingsAutoSync),
		_cloudSettingsAutoSync);
	// Round 5: sync end.
	// Round 5: update.
	object.insert(QString::fromUtf8(kCloudUpdateCheck), _cloudUpdateCheck);
	object.insert(
		QString::fromUtf8(kCloudUpdateLastCheck),
		double(_cloudUpdateLastCheck));
	object.insert(
		QString::fromUtf8(kCloudUpdateSeenBuild),
		double(_cloudUpdateSeenBuild));
	// Round 5: update end.
	// Round 5: send online.
	object.insert(QString::fromUtf8(kSendWhenOnline), _sendWhenOnline);
	object.insert(
		QString::fromUtf8(kSendWhenOnlineHours),
		double(_sendWhenOnlineHours));
	// Round 5: send online end.
	// Oblivion looks: core.
	object.insert(QString::fromUtf8(kLook), double(_look));
	// Oblivion looks: core end.

	// QSaveFile replaces the old file only after a complete write
	// (a failed write() is remembered and makes commit() discard it),
	// so a failed save can't leave an empty or truncated oblivion.json.
	auto file = QSaveFile(FilePath());
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Oblivion Error: Could not open settings for writing."));
		return;
	}
	file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	if (!file.commit()) {
		LOG(("Oblivion Error: Could not save settings: %1."
			).arg(file.errorString()));
	}
}

std::vector<VisualGift> Settings::visualGiftsFor(uint64 peerId) const {
	auto result = std::vector<VisualGift>();
	for (const auto &gift : _visualGifts) {
		if (gift.forPeerId == peerId) {
			result.push_back(gift);
		}
	}
	return result;
}

void Settings::addVisualGift(VisualGift gift) {
	const auto i = ranges::find_if(_visualGifts, [&](const VisualGift &v) {
		return (v.giftId == gift.giftId)
			&& (v.forPeerId == gift.forPeerId);
	});
	if (i != end(_visualGifts)) {
		return;
	}
	_visualGifts.push_back(std::move(gift));
	save();
	_changes.fire({});
}

void Settings::removeVisualGift(uint64 giftId, uint64 forPeerId) {
	const auto i = ranges::find_if(_visualGifts, [&](const VisualGift &v) {
		return (v.giftId == giftId) && (v.forPeerId == forPeerId);
	});
	if (i == end(_visualGifts)) {
		return;
	}
	_visualGifts.erase(i);
	save();
	_changes.fire({});
}

Settings &Get() {
	return Settings::Instance();
}

} // namespace Oblivion
