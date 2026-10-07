/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Oblivion {

struct VisualGift {
	uint64 giftId = 0;
	uint64 modelDocumentId = 0;
	uint64 patternDocumentId = 0;
	QString title;
	QString ownerName;
	QString modelName;
	QString patternName;
	QString backdropName;
	int modelRarity = 0;
	int patternRarity = 0;
	int backdropRarity = 0;
	uint32 centerColor = 0;
	uint32 edgeColor = 0;
	uint32 patternColor = 0;
	uint32 textColor = 0;
	int number = 0;
	int limitedLeft = 0;
	int limitedCount = 0;
	int64 valuePrice = 0;
	QString valueCurrency;
	uint64 forPeerId = 0;

	friend inline bool operator==(
		const VisualGift &,
		const VisualGift &) = default;
};

// Which ghost mode toggles the ghost button in the chats list header
// switches together (ghostRead, ghostTyping, ghostOnline, ghostStories,
// offlineSend).
struct GhostPreset {
	bool read = true;
	bool typing = true;
	bool online = true;
	bool stories = true;
	bool offlineSend = true;

	[[nodiscard]] bool empty() const {
		return !read && !typing && !online && !stories && !offlineSend;
	}

	friend inline bool operator==(
		const GhostPreset &,
		const GhostPreset &) = default;
};

class Settings final {
public:
	[[nodiscard]] static Settings &Instance();

	[[nodiscard]] bool allowCopyProtected() const {
		return _allowCopyProtected;
	}
	void setAllowCopyProtected(bool value) {
		apply(_allowCopyProtected, value);
	}

	[[nodiscard]] bool allowSaveProtected() const {
		return _allowSaveProtected;
	}
	void setAllowSaveProtected(bool value) {
		apply(_allowSaveProtected, value);
	}

	[[nodiscard]] bool storiesWithoutPremium() const {
		return _storiesWithoutPremium;
	}
	void setStoriesWithoutPremium(bool value) {
		apply(_storiesWithoutPremium, value);
	}

	[[nodiscard]] bool showPeerIds() const {
		return _showPeerIds;
	}
	void setShowPeerIds(bool value) {
		apply(_showPeerIds, value);
	}

	[[nodiscard]] bool showRegistrationDate() const {
		return _showRegistrationDate;
	}
	void setShowRegistrationDate(bool value) {
		apply(_showRegistrationDate, value);
	}

	[[nodiscard]] bool showMessageDetails() const {
		return _showMessageDetails;
	}
	void setShowMessageDetails(bool value) {
		apply(_showMessageDetails, value);
	}

	[[nodiscard]] bool exportStickerJson() const {
		return _exportStickerJson;
	}
	void setExportStickerJson(bool value) {
		apply(_exportStickerJson, value);
	}

	[[nodiscard]] bool fakePremium() const {
		return _fakePremium;
	}
	void setFakePremium(bool value) {
		apply(_fakePremium, value);
	}

	[[nodiscard]] int64 fakeStars() const {
		return _fakeStars;
	}
	void setFakeStars(int64 value) {
		apply(_fakeStars, value);
	}

	[[nodiscard]] int64 fakeTon() const {
		return _fakeTon;
	}
	void setFakeTon(int64 value) {
		apply(_fakeTon, value);
	}

	[[nodiscard]] bool visualGifting() const {
		return _visualGifting;
	}
	void setVisualGifting(bool value) {
		apply(_visualGifting, value);
	}

	[[nodiscard]] bool ghostRead() const {
		return _ghostRead;
	}
	void setGhostRead(bool value) {
		apply(_ghostRead, value);
	}

	[[nodiscard]] bool ghostTyping() const {
		return _ghostTyping;
	}
	void setGhostTyping(bool value) {
		apply(_ghostTyping, value);
	}

	[[nodiscard]] bool ghostOnline() const {
		return _ghostOnline;
	}
	void setGhostOnline(bool value) {
		apply(_ghostOnline, value);
	}

	[[nodiscard]] bool hideSponsored() const {
		return _hideSponsored;
	}
	void setHideSponsored(bool value) {
		apply(_hideSponsored, value);
	}

	[[nodiscard]] bool keepDeleted() const {
		return _keepDeleted;
	}
	void setKeepDeleted(bool value) {
		apply(_keepDeleted, value);
	}

	[[nodiscard]] bool ghostStories() const {
		return _ghostStories;
	}
	void setGhostStories(bool value) {
		apply(_ghostStories, value);
	}

	[[nodiscard]] bool offlineSend() const {
		return _offlineSend;
	}
	void setOfflineSend(bool value) {
		apply(_offlineSend, value);
	}

	[[nodiscard]] bool readOnSend() const {
		return _readOnSend;
	}
	void setReadOnSend(bool value) {
		apply(_readOnSend, value);
	}

	[[nodiscard]] bool ghostReadFor(uint64 peerId) const {
		return (_ghostRead != isGhostReadException(peerId));
	}
	[[nodiscard]] bool isGhostReadException(uint64 peerId) const {
		return _ghostReadExceptions.contains(peerId);
	}
	void setGhostReadException(uint64 peerId, bool exception);
	[[nodiscard]] const base::flat_set<uint64> &ghostReadExceptions() const {
		return _ghostReadExceptions;
	}
	void clearGhostReadExceptions();

	[[nodiscard]] bool keepEditHistory() const {
		return _keepEditHistory;
	}
	void setKeepEditHistory(bool value) {
		apply(_keepEditHistory, value);
	}

	[[nodiscard]] bool saveSelfDestructing() const {
		return _saveSelfDestructing;
	}
	void setSaveSelfDestructing(bool value) {
		apply(_saveSelfDestructing, value);
	}

	[[nodiscard]] bool forwardProtectedAsCopy() const {
		return _forwardProtectedAsCopy;
	}
	void setForwardProtectedAsCopy(bool value) {
		apply(_forwardProtectedAsCopy, value);
	}

	[[nodiscard]] bool showSeconds() const {
		return _showSeconds;
	}
	void setShowSeconds(bool value) {
		apply(_showSeconds, value);
	}

	[[nodiscard]] QString localName(uint64 peerId) const;
	void setLocalName(uint64 peerId, const QString &name);
	[[nodiscard]] const base::flat_map<uint64, QString> &localNames() const {
		return _localNames;
	}

	[[nodiscard]] bool hideStoriesBar() const {
		return _hideStoriesBar;
	}
	void setHideStoriesBar(bool value) {
		apply(_hideStoriesBar, value);
	}

	[[nodiscard]] bool hidePremiumPromo() const {
		return _hidePremiumPromo;
	}
	void setHidePremiumPromo(bool value) {
		apply(_hidePremiumPromo, value);
	}

	[[nodiscard]] bool hideGiftPromo() const {
		return _hideGiftPromo;
	}
	void setHideGiftPromo(bool value) {
		apply(_hideGiftPromo, value);
	}

	[[nodiscard]] bool localTranscribe() const {
		return _localTranscribe;
	}
	void setLocalTranscribe(bool value) {
		apply(_localTranscribe, value);
	}

	[[nodiscard]] QString transcribeLanguage() const {
		return _transcribeLanguage;
	}
	void setTranscribeLanguage(const QString &value) {
		apply(_transcribeLanguage, value);
	}

	// Empty for the bundle (Oblivion) icon, else "telegram" or "custom".
	[[nodiscard]] QString appIcon() const {
		return _appIcon;
	}
	void setAppIcon(const QString &value) {
		apply(_appIcon, value);
	}
	[[nodiscard]] bool appIconFinder() const {
		return _appIconFinder;
	}
	void setAppIconFinder(bool value) {
		apply(_appIconFinder, value);
	}
	[[nodiscard]] uint64 appIconDigest() const {
		return _appIconDigest;
	}
	void setAppIconDigest(uint64 value);

	// Effect id for outgoing voice messages, empty for none.
	[[nodiscard]] QString voiceEffect() const {
		return _voiceEffect;
	}
	void setVoiceEffect(const QString &value) {
		apply(_voiceEffect, value);
	}

	// Photo editor: the last export format and quality, like "jpg:92".
	[[nodiscard]] QString photoExport() const {
		return _photoExport;
	}
	void setPhotoExport(const QString &value) {
		apply(_photoExport, value);
	}

	[[nodiscard]] const std::vector<VisualGift> &visualGifts() const {
		return _visualGifts;
	}
	[[nodiscard]] std::vector<VisualGift> visualGiftsFor(uint64 peerId) const;
	void addVisualGift(VisualGift gift);
	void removeVisualGift(uint64 giftId, uint64 forPeerId);

	// Extras: record online / offline intervals of the users whose
	// status updates arrive (oblivion_online.h).
	[[nodiscard]] bool onlineJournal() const {
		return _onlineJournal;
	}
	void setOnlineJournal(bool value) {
		apply(_onlineJournal, value);
	}

	// Extras: users (PeerId values) to notify about when they come online.
	[[nodiscard]] bool isOnlineNotify(uint64 peerId) const {
		return _onlineNotify.contains(peerId);
	}
	void setOnlineNotify(uint64 peerId, bool notify);
	[[nodiscard]] const base::flat_set<uint64> &onlineNotifyList() const {
		return _onlineNotify;
	}

	// Extras: ask the server for the statuses of the users from the notify
	// list about once a minute or two (oblivion_online.h). With this off
	// no such request is ever sent.
	[[nodiscard]] bool onlinePolling() const {
		return _onlinePolling;
	}
	void setOnlinePolling(bool value) {
		apply(_onlinePolling, value);
	}

	// Extras: name / username / bio / photo changes of the contacts
	// (oblivion_profile_history.h).
	[[nodiscard]] bool profileHistory() const {
		return _profileHistory;
	}
	void setProfileHistory(bool value) {
		apply(_profileHistory, value);
	}

	// Extras: the ghost mode toggle in the chats list header.
	[[nodiscard]] bool ghostButton() const {
		return _ghostButton;
	}
	void setGhostButton(bool value) {
		apply(_ghostButton, value);
	}
	[[nodiscard]] GhostPreset ghostPreset() const {
		return _ghostPreset;
	}
	void setGhostPreset(GhostPreset value);
	// The toggles that the ghost button has turned on by itself (they
	// were off before the click), its next click turns only them off.
	[[nodiscard]] GhostPreset ghostButtonOwned() const {
		return _ghostButtonOwned;
	}
	void setGhostButtonOwned(GhostPreset value);

	// Extras: chats of all the accounts in one chats list.
	[[nodiscard]] bool unifiedChats() const {
		return _unifiedChats;
	}
	void setUnifiedChats(bool value) {
		apply(_unifiedChats, value);
	}

	// Extras: RNNoise for outgoing voice messages (oblivion_noise.h).
	[[nodiscard]] bool voiceNoiseSuppression() const {
		return _voiceNoiseSuppression;
	}
	void setVoiceNoiseSuppression(bool value) {
		apply(_voiceNoiseSuppression, value);
	}

	// Round 4: the row of tool buttons in the send files box
	// (oblivion_attach_tools.h).
	[[nodiscard]] bool attachTools() const {
		return _attachTools;
	}
	void setAttachTools(bool value) {
		apply(_attachTools, value);
	}

	// Round 4: the Oblivion badge (oblivion_badge.h). Since round 5 the
	// badge is published through Oblivion Cloud and these two only tell
	// about the past: badgeEnabled mirrors "the old invisible marker is
	// still in the bio of some account" (kept by oblivion_badge.cpp),
	// badgeAsked that the round 4 consent was answered once. Nothing reads
	// them to decide anything.
	[[nodiscard]] bool badgeEnabled() const {
		return _badgeEnabled;
	}
	void setBadgeEnabled(bool value) {
		apply(_badgeEnabled, value);
	}
	[[nodiscard]] bool badgeAsked() const {
		return _badgeAsked;
	}
	void setBadgeAsked(bool value) {
		apply(_badgeAsked, value);
	}

	// Round 4: listening to music together (oblivion_listen.h).
	[[nodiscard]] bool listenTogether() const {
		return _listenTogether;
	}
	void setListenTogether(bool value) {
		apply(_listenTogether, value);
	}

	// Round 5. App-wide switches of the Oblivion Cloud features and of
	// the local features of the round. Whether an account talks to the
	// cloud at all is not here: that is the consent of the account, kept
	// with its key (oblivion_cloud.h). These switches say what the user
	// wants for every connected account; the modules follow changes()
	// and push what is public (the badge, the audiences, the chips) to
	// the server with Cloud::Account::patchMe(). Everything that shows
	// the user to other people is off by default. Each feature adds its
	// own fields inside its own block.
	// Round 5: cloud.
	// Room / playlist / preset links of Oblivion Cloud clicked in a chat
	// are opened in the app (off: in the browser, like any other link).
	[[nodiscard]] bool cloudLinks() const {
		return _cloudLinks;
	}
	void setCloudLinks(bool value) {
		apply(_cloudLinks, value);
	}
	// Round 5: cloud end.
	// Round 5: rooms.
	// The entry points of the rooms (settings, menus, links). A room is
	// only ever created or joined by a click.
	[[nodiscard]] bool cloudRooms() const {
		return _cloudRooms;
	}
	void setCloudRooms(bool value) {
		apply(_cloudRooms, value);
	}
	// The own volume of the room players on this device, 0..100.
	[[nodiscard]] int roomMusicVolume() const {
		return int(_roomMusicVolume);
	}
	void setRoomMusicVolume(int value) {
		apply(_roomMusicVolume, int64(std::clamp(value, 0, 100)));
	}
	[[nodiscard]] int roomVideoVolume() const {
		return int(_roomVideoVolume);
	}
	void setRoomVideoVolume(int value) {
		apply(_roomVideoVolume, int64(std::clamp(value, 0, 100)));
	}
	// Round 5: rooms end.
	// Round 5: room extras.
	// Reactions and stickers of other members are shown over the room.
	[[nodiscard]] bool roomReactions() const {
		return _roomReactions;
	}
	void setRoomReactions(bool value) {
		apply(_roomReactions, value);
	}
	// Round 5: room extras end.
	// Round 5: social.
	// What the app shows of other people is set here: cloudBadgeShow,
	// cloudProfileShow, cloudFriends. What is public about the own account
	// (cloudBadge, the two audiences, the three chips, cloudChosen) is NOT
	// taken from here: it belongs to one Telegram account, is kept by the
	// server and is read / changed through Oblivion::Social (FlagValue,
	// AudienceValue, ToggleFlag...), so a switch flipped in one account
	// publishes nothing about the others. Those fields of the scaffold are
	// left only so that an oblivion.json with them still loads.
	[[nodiscard]] bool cloudBadge() const {
		return _cloudBadge;
	}
	void setCloudBadge(bool value) {
		apply(_cloudBadge, value);
	}
	// Badges of other people from the cloud list are shown.
	[[nodiscard]] bool cloudBadgeShow() const {
		return _cloudBadgeShow;
	}
	void setCloudBadgeShow(bool value) {
		apply(_cloudBadgeShow, value);
	}
	// The «Oblivion» block and the activity chips are shown in the
	// profiles of other people.
	[[nodiscard]] bool cloudProfileShow() const {
		return _cloudProfileShow;
	}
	void setCloudProfileShow(bool value) {
		apply(_cloudProfileShow, value);
	}
	// «Друзья в Oblivion» in the main menu.
	[[nodiscard]] bool cloudFriends() const {
		return _cloudFriends;
	}
	void setCloudFriends(bool value) {
		apply(_cloudFriends, value);
	}
	// Who sees the own Oblivion profile and the own activity chips:
	// 0 nobody (default), 1 the chosen people, 2 everyone in Oblivion.
	[[nodiscard]] int cloudProfileAudience() const {
		return int(_cloudProfileAudience);
	}
	void setCloudProfileAudience(int value) {
		apply(_cloudProfileAudience, int64(std::clamp(value, 0, 2)));
	}
	[[nodiscard]] int cloudActivityAudience() const {
		return int(_cloudActivityAudience);
	}
	void setCloudActivityAudience(int value) {
		apply(_cloudActivityAudience, int64(std::clamp(value, 0, 2)));
	}
	// The activity chips, each with its own switch.
	[[nodiscard]] bool cloudChipListening() const {
		return _cloudChipListening;
	}
	void setCloudChipListening(bool value) {
		apply(_cloudChipListening, value);
	}
	[[nodiscard]] bool cloudChipRoom() const {
		return _cloudChipRoom;
	}
	void setCloudChipRoom(bool value) {
		apply(_cloudChipRoom, value);
	}
	[[nodiscard]] bool cloudChipOnline() const {
		return _cloudChipOnline;
	}
	void setCloudChipOnline(bool value) {
		apply(_cloudChipOnline, value);
	}
	// «Выбранные люди»: Telegram user ids picked by hand, never filled
	// from the contacts automatically.
	[[nodiscard]] bool isCloudChosen(uint64 userId) const {
		return _cloudChosen.contains(userId);
	}
	void setCloudChosen(uint64 userId, bool chosen);
	[[nodiscard]] const base::flat_set<uint64> &cloudChosen() const {
		return _cloudChosen;
	}
	// Round 5: social end.
	// Round 5: sync.
	// The settings are sent and received by themselves (off: only by
	// «Отправить» / «Получить»).
	[[nodiscard]] bool cloudSettingsAutoSync() const {
		return _cloudSettingsAutoSync;
	}
	void setCloudSettingsAutoSync(bool value) {
		apply(_cloudSettingsAutoSync, value);
	}
	// For the settings sync (oblivion_cloud_sync.h): oblivion.json as it
	// is saved now, and the way to put another one in its place. The
	// new one is written to the disk, every setting is read from it again
	// and changes() fires. The sync itself decides what of a received
	// copy may get here, this only replaces the file. False: the json is
	// not an object or could not be written, nothing has changed then.
	[[nodiscard]] QByteArray syncSnapshot();
	bool syncApply(const QByteArray &json);
	// Round 5: sync end.
	// Round 5: update.
	// The update manifest is checked once a day (only while some account
	// is connected to the cloud).
	[[nodiscard]] bool cloudUpdateCheck() const {
		return _cloudUpdateCheck;
	}
	void setCloudUpdateCheck(bool value) {
		apply(_cloudUpdateCheck, value);
	}
	// Unixtime of the last check and the build the user was told about.
	[[nodiscard]] int64 cloudUpdateLastCheck() const {
		return _cloudUpdateLastCheck;
	}
	void setCloudUpdateLastCheck(int64 value) {
		apply(_cloudUpdateLastCheck, value);
	}
	[[nodiscard]] int64 cloudUpdateSeenBuild() const {
		return _cloudUpdateSeenBuild;
	}
	void setCloudUpdateSeenBuild(int64 value) {
		apply(_cloudUpdateSeenBuild, value);
	}
	// Round 5: update end.
	// Round 5: send online.
	// «Отправить, когда будет в сети» in the send menu of a private chat
	// and how long a message waits, in hours (1..168).
	[[nodiscard]] bool sendWhenOnline() const {
		return _sendWhenOnline;
	}
	void setSendWhenOnline(bool value) {
		apply(_sendWhenOnline, value);
	}
	[[nodiscard]] int sendWhenOnlineHours() const {
		return int(_sendWhenOnlineHours);
	}
	void setSendWhenOnlineHours(int value) {
		apply(_sendWhenOnlineHours, int64(std::clamp(value, 1, 168)));
	}
	// Round 5: send online end.

	[[nodiscard]] rpl::producer<> changes() const;

private:
	Settings();

	void apply(bool &field, bool value);
	void apply(int64 &field, int64 value);
	void apply(QString &field, QString value);
	void changed();
	void load();
	void save();

	bool _allowCopyProtected = true;
	bool _allowSaveProtected = true;
	bool _storiesWithoutPremium = true;
	bool _showPeerIds = true;
	bool _showRegistrationDate = true;
	bool _showMessageDetails = true;
	bool _exportStickerJson = true;
	bool _fakePremium = false;
	bool _visualGifting = false;
	bool _ghostRead = false;
	bool _ghostTyping = false;
	bool _ghostOnline = false;
	bool _hideSponsored = false;
	bool _keepDeleted = false;
	bool _ghostStories = false;
	bool _offlineSend = false;
	bool _readOnSend = true;
	bool _keepEditHistory = true;
	bool _saveSelfDestructing = false;
	bool _forwardProtectedAsCopy = true;
	bool _showSeconds = false;
	bool _hideStoriesBar = false;
	bool _hidePremiumPromo = false;
	bool _hideGiftPromo = false;
	bool _localTranscribe = true;
	int64 _fakeStars = 0;
	int64 _fakeTon = 0;
	QString _transcribeLanguage;
	QString _appIcon;
	bool _appIconFinder = false;
	uint64 _appIconDigest = 0;
	QString _voiceEffect;
	QString _photoExport;
	base::flat_set<uint64> _ghostReadExceptions;
	base::flat_map<uint64, QString> _localNames;
	std::vector<VisualGift> _visualGifts;
	bool _onlineJournal = true;
	base::flat_set<uint64> _onlineNotify;
	bool _onlinePolling = true;
	bool _profileHistory = true;
	bool _ghostButton = true;
	GhostPreset _ghostPreset;
	bool _unifiedChats = false;
	bool _voiceNoiseSuppression = false;
	GhostPreset _ghostButtonOwned = { false, false, false, false, false };
	bool _attachTools = true;
	bool _badgeEnabled = false;
	bool _badgeAsked = false;
	bool _listenTogether = true;
	// Round 5: cloud.
	bool _cloudLinks = true;
	// Round 5: cloud end.
	// Round 5: rooms.
	bool _cloudRooms = true;
	int64 _roomMusicVolume = 100;
	int64 _roomVideoVolume = 100;
	// Round 5: rooms end.
	// Round 5: room extras.
	bool _roomReactions = true;
	// Round 5: room extras end.
	// Round 5: social.
	bool _cloudBadge = false;
	bool _cloudBadgeShow = true;
	bool _cloudProfileShow = true;
	bool _cloudFriends = true;
	int64 _cloudProfileAudience = 0;
	int64 _cloudActivityAudience = 0;
	bool _cloudChipListening = false;
	bool _cloudChipRoom = false;
	bool _cloudChipOnline = false;
	base::flat_set<uint64> _cloudChosen;
	// Round 5: social end.
	// Round 5: sync.
	bool _cloudSettingsAutoSync = false;
	// Round 5: sync end.
	// Round 5: update.
	bool _cloudUpdateCheck = true;
	int64 _cloudUpdateLastCheck = 0;
	int64 _cloudUpdateSeenBuild = 0;
	// Round 5: update end.
	// Round 5: send online.
	bool _sendWhenOnline = true;
	int64 _sendWhenOnlineHours = 24;
	// Round 5: send online end.

	rpl::event_stream<> _changes;

};

[[nodiscard]] Settings &Get();

} // namespace Oblivion
