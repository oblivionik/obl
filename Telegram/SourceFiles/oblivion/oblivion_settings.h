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

	rpl::event_stream<> _changes;

};

[[nodiscard]] Settings &Get();

} // namespace Oblivion
