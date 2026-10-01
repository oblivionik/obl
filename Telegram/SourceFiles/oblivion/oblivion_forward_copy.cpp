/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_forward_copy.h"

#include "api/api_common.h"
#include "api/api_polls.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "base/base_file_utilities.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/mime_type.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_cloud_file.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_drafts.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_poll.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_deleted_store.h"
#include "oblivion/oblivion_ghost.h"
#include "oblivion/oblivion_interface.h"
#include "oblivion/oblivion_settings.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "storage/storage_account.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/image/image.h"
#include "ui/image/image_location.h"
#include "window/window_session_controller.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Oblivion {
namespace {

constexpr auto kTickInterval = crl::time(200);
constexpr auto kToastThrottle = crl::time(3) * 1000;
constexpr auto kLocalIdsScan = 256;
constexpr auto kMaxLoadAttempts = 3;
constexpr auto kTaskQueueStopTimeout = crl::time(5000);
constexpr auto kClearTempRetry = crl::time(10) * 1000;

enum class PartType : uchar {
	Forward,
	Text,
	Files,
	Voice,
	Sticker,
	Poll,
	Location,
	Contact,
};

enum class PartState : uchar {
	Preparing,
	Sending,
	Done,
};

enum class FileKind : uchar {
	Photo,
	Video,
	Animation,
	Music,
	Document,
};

enum class ToastType : uchar {
	Progress,
	Failed,
};

struct FileEntry {
	PhotoData *photo = nullptr;
	DocumentData *document = nullptr;
	std::shared_ptr<Data::PhotoMedia> photoView;
	std::shared_ptr<Data::DocumentMedia> documentView;
	Data::FileOrigin origin;
	FileKind kind = FileKind::Document;
	TextWithTags caption;
	bool spoiler = false;
	bool ready = false;
	bool failed = false;
	int attempts = 0;
};

struct Part {
	PartType type = PartType::Text;
	PartState state = PartState::Preparing;

	// Forward.
	std::vector<FullMsgId> forwardIds;

	// Text, poll text.
	TextWithTags text;
	TextWithEntities richText;
	Data::WebPageDraft webPage;

	// Files, voice, sticker.
	std::vector<FileEntry> files;
	bool invertCaption = false;
	bool round = false;

	// Poll, location, contact.
	PollData *poll = nullptr;
	float64 lat = 0.;
	float64 lon = 0.;
	QString phone;
	QString firstName;
	QString lastName;

	// Sticker file reference refresh.
	bool refreshing = false;
	bool refreshed = false;

	// Tracking of uploaded messages. Only the local messages created
	// for this part are tracked: the album items for several files,
	// the messages found by TrackedLoadTask for a single file or voice.
	std::shared_ptr<SendingAlbum> album;
	int expected = 0;
	int waiting = 0; // TrackedLoadTask-s not finished yet.
	base::flat_set<MsgId> localIds;
	base::flat_set<MsgId> finished;
	bool albumFailed = false;
};

// Lets the job know which local message the prepared file has created.
class TrackedLoadTask final : public Task {
public:
	TrackedLoadTask(
		std::unique_ptr<FileLoadTask> task,
		Fn<void(not_null<FileLoadTask*>)> finish)
	: _task(std::move(task))
	, _finish(std::move(finish)) {
	}

	void process() override {
		_task->process();
	}
	void finish() override {
		// Must call FileLoadTask::finish().
		_finish(_task.get());
	}

private:
	const std::unique_ptr<FileLoadTask> _task;
	const Fn<void(not_null<FileLoadTask*>)> _finish;

};

void CollectAlbumIds(Part &part) {
	if (const auto album = part.album.get()) {
		for (const auto &item : album->items) {
			if (item.msgId) {
				part.localIds.emplace(item.msgId.msg);
			}
		}
	}
}

// Files that may still be read from the disk before the upload starts.
[[nodiscard]] int WaitingTasks(const Part &part) {
	if (const auto album = part.album.get()) {
		// The item gets its message id when its FileLoadTask is finished,
		// the item of a rejected file is removed from the album.
		return int(ranges::count_if(album->items, [](
				const SendingAlbum::Item &item) {
			return !item.msgId;
		}));
	}
	return part.waiting;
}

[[nodiscard]] bool HasMediaId(not_null<HistoryItem*> item, uint64 id) {
	const auto media = item->media();
	if (!media) {
		return false;
	} else if (const auto photo = media->photo()) {
		return (photo->id == id);
	} else if (const auto document = media->document()) {
		return (document->id == id);
	}
	return false;
}

void ShowToast(
		not_null<PeerData*> peer,
		ToastType type,
		const QString &text) {
	static auto lastShown = base::flat_map<ToastType, crl::time>();
	const auto now = crl::now();
	auto &last = lastShown[type];
	if (last && (now - last < kToastThrottle)) {
		return;
	}
	last = now;
	if (const auto window = ExistingWindow(&peer->session(), peer)) {
		window->showToast(text);
	}
}

void ShowFailedToast(not_null<PeerData*> peer) {
	ShowToast(
		peer,
		ToastType::Failed,
		tr::lng_oblivion_forward_copy_failed(tr::now));
}

[[nodiscard]] TextWithTags ToTags(const TextWithEntities &text) {
	return TextWithTags{
		text.text,
		TextUtilities::ConvertEntitiesToTextTags(text.entities),
	};
}

[[nodiscard]] TextWithEntities PrepareRich(
		TextWithEntities text,
		not_null<PeerData*> to) {
	return DropDisallowedCustomEmoji(to, std::move(text));
}

[[nodiscard]] TextWithTags PrepareText(
		TextWithEntities text,
		not_null<PeerData*> to) {
	return ToTags(PrepareRich(std::move(text), to));
}

[[nodiscard]] TextWithEntities Description(not_null<HistoryItem*> item) {
	auto result = TextWithEntities();
	const auto media = item->media();
	if (const auto dice = dynamic_cast<Data::MediaDice*>(media)) {
		// Don't send the bare emoji, it would roll a new dice.
		result.append(dice->emoji())
			.append(QChar(' '))
			.append(QString::number(dice->value()));
	} else if (media) {
		result = media->clipboardText().rich;
	}
	const auto &text = item->originalText();
	if (!text.empty() && !(media && media->poll())) {
		if (!result.empty()) {
			result.append(QChar('\n'));
		}
		result.append(text);
	}
	return result;
}

[[nodiscard]] FileKind DocumentKind(not_null<DocumentData*> document) {
	if (document->isGifv() || document->isAnimation()) {
		return FileKind::Animation;
	} else if (document->isVideoFile()) {
		return FileKind::Video;
	} else if (document->isAudioFile()) {
		return FileKind::Music;
	}
	return FileKind::Document;
}

// Which files may be sent together as one album.
enum class AlbumClass : uchar {
	None,
	Media,
	Music,
	Files,
};

[[nodiscard]] AlbumClass ItemAlbumClass(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media || media->webpage()) {
		return AlbumClass::None;
	} else if (media->photo()) {
		return AlbumClass::Media;
	} else if (const auto document = media->document()) {
		if (document->sticker()
			|| document->isVoiceMessage()
			|| document->isVideoMessage()) {
			return AlbumClass::None;
		}
		switch (DocumentKind(document)) {
		case FileKind::Video: return AlbumClass::Media;
		case FileKind::Music: return AlbumClass::Music;
		case FileKind::Document: return AlbumClass::Files;
		case FileKind::Photo:
		case FileKind::Animation: return AlbumClass::None;
		}
	}
	return AlbumClass::None;
}

[[nodiscard]] QString TempFileName(not_null<DocumentData*> document) {
	auto name = document->filename().trimmed();
	name.replace(QChar('/'), QChar('_'));
	name.replace(QChar('\\'), QChar('_'));
	name.replace(QChar(':'), QChar('_'));
	if (!name.isEmpty() && !name.startsWith(QChar('.'))) {
		// Windows doesn't allow some more characters and names, a file
		// with such a name could not be written there at all.
		return base::FileNameFromUserString(name);
	}
	auto extension = name;
	if (extension.isEmpty()) {
		const auto patterns = Core::MimeTypeForName(
			document->mimeString()).globPatterns();
		if (!patterns.isEmpty()) {
			extension = patterns.front();
			extension.replace(QChar('*'), QString());
		}
	}
	const auto base = document->isVideoMessage()
		? u"round"_q
		: document->isVoiceMessage()
		? u"voice"_q
		: document->isAnimation()
		? u"animation"_q
		: document->isVideoFile()
		? u"video"_q
		: document->sticker()
		? u"sticker"_q
		: u"file"_q;
	return ::base::FileNameFromUserString(base + extension);
}

[[nodiscard]] bool TooLargeToSend(not_null<DocumentData*> document) {
	// Same limits as in FileLoadTask::finish().
	return (document->size > kFileSizePremiumLimit)
		|| ((document->size > kFileSizeLimit)
			&& !document->session().user()->isPremium());
}

[[nodiscard]] bool DocumentAvailable(const FileEntry &file) {
	if (!file.document->filepath(true).isEmpty()) {
		return true;
	}
	return file.documentView
		&& file.documentView->loaded()
		&& !file.documentView->bytes().isEmpty();
}

[[nodiscard]] QByteArray DocumentBytes(const FileEntry &file) {
	const auto path = file.document->filepath(true);
	if (!path.isEmpty()) {
		auto f = QFile(path);
		if (f.open(QIODevice::ReadOnly)) {
			return f.readAll();
		}
	}
	return file.documentView ? file.documentView->bytes() : QByteArray();
}

[[nodiscard]] std::optional<Ui::PreparedFile> PrepareFile(
		const FileEntry &entry) {
	using Type = Ui::PreparedFile::Type;
	if (entry.photo) {
		const auto large = Data::PhotoSize::Large;
		auto bytes = entry.photoView->imageBytes(large);
		if (bytes.isEmpty()) {
			if (const auto image = entry.photoView->image(large)) {
				auto buffer = QBuffer(&bytes);
				image->original().save(&buffer, "JPG", 95);
			}
		}
		if (bytes.isEmpty()) {
			return std::nullopt;
		}
		auto result = Ui::PreparedFile(QString());
		result.size = bytes.size();
		result.information = FileLoadTask::ReadMediaInformation(
			QString(),
			bytes,
			u"image/jpeg"_q);
		result.content = std::move(bytes);
		result.type = Type::Photo;
		result.sendLargePhotos = true;
		result.caption = entry.caption;
		result.spoiler = entry.spoiler;
		return std::make_optional(std::move(result));
	}
	const auto document = entry.document;
	const auto path = document->filepath(true);
	auto result = Ui::PreparedFile(path);
	if (path.isEmpty()) {
		result.content = entry.documentView
			? entry.documentView->bytes()
			: QByteArray();
		if (result.content.isEmpty()) {
			return std::nullopt;
		}
		result.size = result.content.size();
	} else {
		result.size = QFileInfo(path).size();
	}
	const auto name = document->filename();
	if (!name.isEmpty()) {
		result.displayName = name;
	}
	switch (entry.kind) {
	case FileKind::Video:
	case FileKind::Animation:
		result.type = Type::Video;
		break;
	case FileKind::Music:
		result.type = Type::Music;
		break;
	case FileKind::Photo:
	case FileKind::Document:
		// A video sent as a file should stay a file (forceFile).
		result.type = document->mimeString().startsWith(
			u"video/"_q,
			Qt::CaseInsensitive) ? Type::Video : Type::File;
		break;
	}
	result.caption = entry.caption;
	result.spoiler = entry.spoiler;
	return std::make_optional(std::move(result));
}

[[nodiscard]] bool SimplePoll(not_null<PollData*> poll) {
	if (poll->attachedMedia || poll->solutionMedia) {
		return false;
	} else if (poll->answers.empty()) {
		return false;
	}
	auto hasCorrect = false;
	for (const auto &answer : poll->answers) {
		if (answer.media) {
			return false;
		} else if (answer.correct) {
			hasCorrect = true;
		}
	}
	return !poll->quiz() || hasCorrect;
}

[[nodiscard]] std::optional<Data::LocationPoint> ItemLocation(
		not_null<Data::Media*> media) {
	const auto cloud = media->location();
	if (!cloud) {
		return std::nullopt;
	}
	const auto &file = cloud->location().file();
	if (const auto geo = std::get_if<GeoPointLocation>(&file.data)) {
		return Data::LocationPoint(
			geo->lat,
			geo->lon,
			Data::LocationPoint::NoAccessHash);
	}
	return std::nullopt;
}

[[nodiscard]] FileEntry MakeFileEntry(
		not_null<HistoryItem*> item,
		TextWithTags caption) {
	const auto media = item->media();
	auto result = FileEntry();
	result.origin = Data::FileOrigin(Data::FileOriginMessage(item->fullId()));
	result.caption = std::move(caption);
	result.spoiler = media && media->hasSpoiler();
	if (const auto photo = media ? media->photo() : nullptr) {
		result.photo = photo;
		result.photoView = photo->createMediaView();
		result.kind = FileKind::Photo;
	} else if (const auto document = media ? media->document() : nullptr) {
		result.document = document;
		result.documentView = document->createMediaView();
		result.kind = DocumentKind(document);
	}
	return result;
}

void AppendText(
		std::vector<Part> &parts,
		TextWithEntities text,
		not_null<PeerData*> to,
		Data::WebPageDraft webPage) {
	if (text.empty()) {
		return;
	}
	auto part = Part();
	part.type = PartType::Text;
	part.text = PrepareText(std::move(text), to);
	part.webPage = webPage;
	parts.push_back(std::move(part));
}

void AppendDescription(
		std::vector<Part> &parts,
		not_null<HistoryItem*> item,
		not_null<PeerData*> to) {
	auto webPage = Data::WebPageDraft();
	webPage.removed = true;
	AppendText(parts, Description(item), to, webPage);
}

void AppendProtected(
		std::vector<Part> &parts,
		not_null<HistoryItem*> item,
		bool dropCaptions,
		not_null<PeerData*> to) {
	const auto media = item->media();
	const auto caption = [&] {
		return dropCaptions
			? TextWithTags()
			: PrepareText(item->originalText(), to);
	};
	if (!media || media->webpage()) {
		AppendText(
			parts,
			item->originalText(),
			to,
			Data::WebPageDraft::FromItem(item));
		return;
	} else if (media->photo()) {
		auto part = Part();
		part.type = PartType::Files;
		part.invertCaption = item->invertMedia();
		part.files.push_back(MakeFileEntry(item, caption()));
		parts.push_back(std::move(part));
		return;
	} else if (const auto document = media->document()) {
		auto part = Part();
		if (document->sticker()) {
			part.type = document->stickerSetOrigin()
				? PartType::Sticker
				: PartType::Files;
			part.files.push_back(MakeFileEntry(item, TextWithTags()));
			part.files.back().kind = FileKind::Document;
		} else if (document->isVoiceMessage()
			|| document->isVideoMessage()) {
			part.type = PartType::Voice;
			part.round = document->isVideoMessage();
			part.files.push_back(MakeFileEntry(item, caption()));
		} else {
			part.type = PartType::Files;
			part.invertCaption = item->invertMedia();
			part.files.push_back(MakeFileEntry(item, caption()));
		}
		parts.push_back(std::move(part));
		return;
	} else if (const auto poll = media->poll()) {
		if (!SimplePoll(poll)) {
			AppendDescription(parts, item, to);
			return;
		}
		auto part = Part();
		part.type = PartType::Poll;
		part.poll = poll;
		part.richText = PrepareRich(media->consumedMessageText(), to);
		parts.push_back(std::move(part));
		return;
	} else if (const auto point = ItemLocation(media)) {
		auto part = Part();
		part.type = PartType::Location;
		part.lat = point->lat();
		part.lon = point->lon();
		parts.push_back(std::move(part));
		AppendText(
			parts,
			item->originalText(),
			to,
			Data::WebPageDraft{ .removed = true });
		return;
	} else if (const auto contact = media->sharedContact()) {
		if (contact->phoneNumber.isEmpty()) {
			AppendDescription(parts, item, to);
			return;
		}
		auto part = Part();
		part.type = PartType::Contact;
		part.phone = contact->phoneNumber;
		part.firstName = contact->firstName;
		part.lastName = contact->lastName;
		parts.push_back(std::move(part));
		return;
	}
	AppendDescription(parts, item, to);
}

[[nodiscard]] std::vector<Part> BuildParts(
		const HistoryItemsList &items,
		Data::ForwardOptions options,
		not_null<PeerData*> to) {
	const auto dropCaptions
		= (options == Data::ForwardOptions::NoNamesAndCaptions);
	auto result = std::vector<Part>();
	const auto count = int(items.size());
	for (auto i = 0; i != count;) {
		const auto item = items[i];
		if (item->isService()) {
			++i;
			continue;
		} else if (!ItemForwardProtected(item)) {
			if (result.empty() || result.back().type != PartType::Forward) {
				auto part = Part();
				part.type = PartType::Forward;
				result.push_back(std::move(part));
			}
			result.back().forwardIds.push_back(item->fullId());
			++i;
			continue;
		}
		const auto groupId = item->groupId();
		const auto albumClass = ItemAlbumClass(item);
		auto till = i + 1;
		if (groupId && albumClass != AlbumClass::None) {
			while (till != count
				&& items[till]->groupId() == groupId
				&& ItemForwardProtected(items[till])
				&& ItemAlbumClass(items[till]) == albumClass) {
				++till;
			}
		}
		if (till - i > 1) {
			auto part = Part();
			part.type = PartType::Files;
			part.invertCaption = item->invertMedia();
			for (auto j = i; j != till; ++j) {
				part.files.push_back(MakeFileEntry(
					items[j],
					(dropCaptions
						? TextWithTags()
						: PrepareText(items[j]->originalText(), to))));
			}
			result.push_back(std::move(part));
			i = till;
			continue;
		}
		AppendProtected(result, item, dropCaptions, to);
		++i;
	}
	return result;
}

class Job;

class Manager final : public base::has_weak_ptr {
public:
	explicit Manager(not_null<Main::Session*> session);
	~Manager();

	void start(
		Api::SendAction action,
		std::vector<Part> parts,
		Data::ForwardOptions options,
		FnMut<void()> done);

	[[nodiscard]] TaskQueue &taskQueue();
	[[nodiscard]] QString tempPath(not_null<DocumentData*> document);
	void jobFinished();

private:
	void clearTempWhenIdle();
	void clearTemp();

	const not_null<Main::Session*> _session;
	const QString _tempFolder;
	std::vector<std::unique_ptr<Job>> _jobs;
	std::unique_ptr<TaskQueue> _taskQueue;
	base::flat_set<not_null<DocumentData*>> _tempDocuments;
	base::Timer _clearTempTimer;

};

class Job final : public base::has_weak_ptr {
public:
	Job(
		not_null<Manager*> manager,
		not_null<Main::Session*> session,
		Api::SendAction action,
		std::vector<Part> parts,
		Data::ForwardOptions options,
		FnMut<void()> done);

	void start();
	[[nodiscard]] bool finished() const;

private:
	void tick();
	void tickDelayed();
	void advance();
	[[nodiscard]] bool prepared(Part &part);
	[[nodiscard]] bool checkLoaded(FileEntry &file);
	void startDocumentLoad(FileEntry &file);
	void send(Part &part);
	[[nodiscard]] bool sent(Part &part);
	[[nodiscard]] bool uploadsFinished(Part &part);
	void idChanged(const Data::Session::IdChange &change);
	void uploadFailed(FullMsgId itemId);
	void addLoadTask(Part &part, std::unique_ptr<FileLoadTask> task);
	void loadTaskFinished(int index, uint64 fileId, MsgId after);

	void sendForward(Part &part);
	void sendText(Part &part);
	void sendFiles(Part &part);
	void sendVoice(Part &part);
	void sendSticker(Part &part);
	void sendPoll(Part &part);
	void sendContact(Part &part);

	[[nodiscard]] Api::SendAction nextAction();
	void startTracking(Part &part, int expected);
	void finish();

	const not_null<Manager*> _manager;
	const not_null<Main::Session*> _session;
	const not_null<PeerData*> _peer;
	const Api::SendAction _action;
	std::vector<Part> _parts;
	const Data::ForwardOptions _options;
	FnMut<void()> _done;
	base::Timer _timer;
	int _index = 0;
	bool _sentAny = false;
	bool _failed = false;
	bool _finished = false;
	bool _ticking = false;
	bool _tickAgain = false;
	rpl::lifetime _lifetime;

};

[[nodiscard]] base::flat_map<not_null<Main::Session*>, Manager*> &Managers() {
	static auto result = base::flat_map<not_null<Main::Session*>, Manager*>();
	return result;
}

[[nodiscard]] not_null<Manager*> ManagerFor(
		not_null<Main::Session*> session) {
	auto &managers = Managers();
	const auto i = managers.find(session);
	if (i != end(managers)) {
		return i->second;
	}
	const auto result = session->lifetime().make_state<Manager>(session);
	managers.emplace(session, result);
	return result;
}

Manager::Manager(not_null<Main::Session*> session)
: _session(session)
, _tempFolder(QDir(session->local().tempDirectory()).filePath(
	u"oblivion_copy"_q))
, _clearTempTimer([=] { clearTempWhenIdle(); }) {
	// Nothing uses the folder yet, remove what a crashed launch has left.
	QDir(_tempFolder).removeRecursively();
}

Manager::~Manager() {
	Managers().remove(_session);

	// Stop preparing the files before removing them.
	_taskQueue = nullptr;
	QDir(_tempFolder).removeRecursively();
}

void Manager::start(
		Api::SendAction action,
		std::vector<Part> parts,
		Data::ForwardOptions options,
		FnMut<void()> done) {
	_jobs.push_back(std::make_unique<Job>(
		this,
		_session,
		std::move(action),
		std::move(parts),
		options,
		std::move(done)));
	_jobs.back()->start();
}

TaskQueue &Manager::taskQueue() {
	if (!_taskQueue) {
		_taskQueue = std::make_unique<TaskQueue>(kTaskQueueStopTimeout);
	}
	return *_taskQueue;
}

QString Manager::tempPath(not_null<DocumentData*> document) {
	_tempDocuments.emplace(document);
	const auto folder = QDir(_tempFolder).filePath(
		QString::number(document->id));
	QDir().mkpath(folder);
	return QDir(folder).filePath(TempFileName(document));
}

void Manager::jobFinished() {
	crl::on_main(this, [=] {
		_jobs.erase(ranges::remove_if(_jobs, [](
				const std::unique_ptr<Job> &job) {
			return job->finished();
		}), end(_jobs));
		clearTempWhenIdle();
	});
}

void Manager::clearTempWhenIdle() {
	if (!_jobs.empty() || _tempDocuments.empty()) {
		// A running job will try again when it is finished.
		return;
	} else if (_session->uploader().currentUploadId()) {
		// Some upload may still read the files, try again later.
		_clearTempTimer.callOnce(kClearTempRetry);
		return;
	}
	clearTemp();
}

void Manager::clearTemp() {
	// All the jobs have their files prepared and their messages sent
	// or failed, and the uploader is idle: nothing reads the files now.
	QDir(_tempFolder).removeRecursively();
	for (const auto &document : base::take(_tempDocuments)) {
		// Forget the removed file, as if the user has removed it.
		(void)document->location(true);
		_session->data().requestDocumentViewRepaint(document);
	}
}

Job::Job(
	not_null<Manager*> manager,
	not_null<Main::Session*> session,
	Api::SendAction action,
	std::vector<Part> parts,
	Data::ForwardOptions options,
	FnMut<void()> done)
: _manager(manager)
, _session(session)
, _peer(action.history->peer)
, _action(std::move(action))
, _parts(std::move(parts))
, _options(options)
, _done(std::move(done))
, _timer([=] { tick(); }) {
}

bool Job::finished() const {
	return _finished;
}

void Job::start() {
	_session->data().itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		idChanged(change);
	}, _lifetime);

	auto &uploader = _session->uploader();
	rpl::merge(
		uploader.photoFailed(),
		uploader.documentFailed()
	) | rpl::on_next([=](FullMsgId itemId) {
		uploadFailed(itemId);
	}, _lifetime);

	_session->downloaderTaskFinished(
	) | rpl::on_next([=] {
		tickDelayed();
	}, _lifetime);

	auto uploads = false;
	for (auto &part : _parts) {
		if (part.type != PartType::Files && part.type != PartType::Voice) {
			continue;
		}
		uploads = true;
		for (auto &file : part.files) {
			(void)checkLoaded(file);
		}
	}
	if (uploads) {
		ShowToast(
			_peer,
			ToastType::Progress,
			tr::lng_oblivion_forward_copy_progress(tr::now));
	}
	_timer.callEach(kTickInterval);
	tick();
}

void Job::tickDelayed() {
	crl::on_main(this, [=] {
		tick();
	});
}

void Job::tick() {
	if (_finished) {
		return;
	} else if (_ticking) {
		_tickAgain = true;
		return;
	}
	_ticking = true;
	do {
		_tickAgain = false;
		advance();
	} while (_tickAgain && !_finished);
	_ticking = false;
}

void Job::advance() {
	while (_index < int(_parts.size())) {
		auto &part = _parts[_index];
		if (part.state == PartState::Preparing) {
			if (!prepared(part)) {
				return;
			}
			part.state = PartState::Sending;
			send(part);
		}
		if (part.state == PartState::Sending) {
			if (!sent(part)) {
				return;
			}
			part.state = PartState::Done;
		}
		++_index;
	}
	finish();
}

bool Job::prepared(Part &part) {
	switch (part.type) {
	case PartType::Files:
	case PartType::Voice: {
		auto result = true;
		for (auto &file : part.files) {
			if (!checkLoaded(file)) {
				result = false;
			}
		}
		return result;
	}
	case PartType::Sticker: {
		// Make sure the sticker is sent with a file reference taken
		// from its sticker set, not from the protected message.
		if (part.refreshed) {
			return true;
		}
		const auto origin = part.files.front().document->stickerSetOrigin();
		if (!origin) {
			return true;
		} else if (!part.refreshing) {
			part.refreshing = true;
			const auto index = _index;
			_session->api().refreshFileReference(origin, crl::guard(this, [=](
					const Data::UpdatedFileReferences &) {
				_parts[index].refreshed = true;
				tickDelayed();
			}));
		}
		return part.refreshed;
	}
	case PartType::Forward:
	case PartType::Text:
	case PartType::Poll:
	case PartType::Location:
	case PartType::Contact:
		return true;
	}
	return true;
}

bool Job::checkLoaded(FileEntry &file) {
	if (file.ready || file.failed) {
		return true;
	}
	if (const auto photo = file.photo) {
		const auto large = Data::PhotoSize::Large;
		if (file.photoView->loaded()) {
			file.ready = true;
		} else if (!photo->loading(large)) {
			if (file.attempts >= kMaxLoadAttempts) {
				file.failed = true;
			} else {
				++file.attempts;
				file.photoView->wanted(large, file.origin);
				file.ready = file.photoView->loaded();
			}
		}
		return file.ready || file.failed;
	} else if (!file.document) {
		file.failed = true;
		return true;
	}
	if (DocumentAvailable(file)) {
		file.ready = true;
	} else if (TooLargeToSend(file.document)) {
		// It would be rejected after the download, don't do it in vain.
		file.failed = true;
	} else if (!file.document->loading()) {
		if (file.attempts >= kMaxLoadAttempts) {
			file.failed = true;
		} else {
			++file.attempts;
			startDocumentLoad(file);
			file.ready = DocumentAvailable(file);
		}
	}
	return file.ready || file.failed;
}

void Job::startDocumentLoad(FileEntry &file) {
	file.document->save(file.origin, _manager->tempPath(file.document));
}

void Job::send(Part &part) {
	switch (part.type) {
	case PartType::Forward: sendForward(part); break;
	case PartType::Text: sendText(part); break;
	case PartType::Files: sendFiles(part); break;
	case PartType::Voice: sendVoice(part); break;
	case PartType::Sticker: sendSticker(part); break;
	case PartType::Poll: sendPoll(part); break;
	case PartType::Location:
		Api::SendLocation(nextAction(), part.lat, part.lon);
		break;
	case PartType::Contact: sendContact(part); break;
	}
}

bool Job::sent(Part &part) {
	return ((part.type != PartType::Files)
		&& (part.type != PartType::Voice))
		|| uploadsFinished(part);
}

Api::SendAction Job::nextAction() {
	auto result = _action;
	result.clearDraft = false;
	result.generateLocal = true;

	// Parts are sent long after the user action, they must not send
	// a forward draft the user has prepared in the thread meanwhile.
	result.sendForwardDraft = false;
	if (_sentAny) {
		// A message effect should be applied only once.
		result.options.effectId = 0;
	}
	_sentAny = true;
	return result;
}

void Job::sendForward(Part &part) {
	auto items = HistoryItemsList();
	items.reserve(part.forwardIds.size());
	for (const auto &id : part.forwardIds) {
		if (const auto item = _session->data().message(id)) {
			items.push_back(item);
		}
	}
	if (items.empty()) {
		return;
	}
	auto action = nextAction();
	action.generateLocal = _action.generateLocal;
	_session->api().forwardMessages(
		Data::ResolvedForwardDraft{
			.items = std::move(items),
			.options = _options,
		},
		std::move(action));
}

void Job::sendText(Part &part) {
	auto message = Api::MessageToSend(nextAction());
	message.textWithTags = part.text;
	message.webPage = part.webPage;
	_session->api().sendMessage(std::move(message));
}

void Job::startTracking(Part &part, int expected) {
	part.expected = expected;
}

void Job::addLoadTask(Part &part, std::unique_ptr<FileLoadTask> task) {
	const auto index = int(&part - _parts.data());
	const auto fileId = task->fileid();
	const auto session = _session;
	const auto weak = base::make_weak(this);
	++part.waiting;
	_manager->taskQueue().addTask(std::make_unique<TrackedLoadTask>(
		std::move(task),
		[=](not_null<FileLoadTask*> loaded) {
			// SendConfirmedFile() takes the next local id for the message.
			const auto after = weak.get()
				? session->data().nextLocalMessageId()
				: MsgId();
			loaded->finish();
			if (const auto strong = weak.get()) {
				strong->loadTaskFinished(index, fileId, after);
			}
		}));
}

void Job::loadTaskFinished(int index, uint64 fileId, MsgId after) {
	auto &part = _parts[index];
	--part.waiting;

	// A file rejected before the upload (too large, etc) doesn't get
	// any local message, uploadsFinished() will count it as failed.
	const auto peerId = _peer->id;
	auto &owner = _session->data();
	for (auto i = 1; i <= kLocalIdsScan; ++i) {
		const auto id = MsgId(after.bare + i);
		const auto item = owner.message(peerId, id);
		if (item && HasMediaId(item, fileId)) {
			part.localIds.emplace(id);
			break;
		}
	}
	tickDelayed();
}

void Job::sendFiles(Part &part) {
	auto list = Ui::PreparedList();
	auto media = true;
	for (const auto &file : part.files) {
		if (file.failed) {
			_failed = true;
			continue;
		}
		auto prepared = PrepareFile(file);
		if (!prepared) {
			_failed = true;
			continue;
		}
		if (file.kind != FileKind::Photo
			&& file.kind != FileKind::Video
			&& file.kind != FileKind::Animation) {
			media = false;
		}
		list.files.push_back(std::move(*prepared));
	}
	if (list.files.empty()) {
		return;
	}
	const auto count = int(list.files.size());
	auto action = nextAction();
	action.options.invertCaption = part.invertCaption;
	const auto type = media ? SendMediaType::Photo : SendMediaType::File;
	startTracking(part, count);
	if (count > 1) {
		// The album items tell which messages belong to this part.
		part.album = std::make_shared<SendingAlbum>();
		_session->api().sendFiles(
			std::move(list),
			type,
			part.album,
			std::move(action));
		return;
	}

	// Same as ApiWrap::sendFiles() does without an album, but the task
	// is tracked to know the message created for this file.
	auto &file = list.files.front();
	const auto forceFile = (type == SendMediaType::File)
		&& (file.type == Ui::PreparedFile::Type::Video);
	addLoadTask(part, std::make_unique<FileLoadTask>(FileLoadTask::Args{
		.session = _session,
		.filepath = file.path,
		.content = file.content,
		.information = std::move(file.information),
		.videoCover = nullptr,
		.type = type,
		.to = FileLoadTo(
			_peer->id,
			action.options,
			action.replyTo,
			action.replaceMediaOf),
		.caption = std::move(file.caption),
		.spoiler = file.spoiler,
		.album = nullptr,
		.forceFile = forceFile,
		.sendLargePhotos = file.sendLargePhotos,
		.idOverride = 0,
		.displayName = file.displayName,
	}));
}

void Job::sendVoice(Part &part) {
	const auto &file = part.files.front();
	if (file.failed) {
		_failed = true;
		return;
	}
	auto bytes = DocumentBytes(file);
	if (bytes.isEmpty()) {
		_failed = true;
		return;
	}
	const auto document = file.document;
	auto waveform = VoiceWaveform();
	if (const auto voice = document->voice()) {
		if (!voice->waveform.isEmpty() && voice->waveform[0] >= 0) {
			waveform = voice->waveform;
		}
	}
	const auto action = nextAction();
	startTracking(part, 1);
	_session->api().sendAction(action);
	addLoadTask(part, std::make_unique<FileLoadTask>(
		FileLoadTask::VoiceArgs{
			.session = _session,
			.voice = std::move(bytes),
			.duration = document->duration(),
			.waveform = std::move(waveform),
			.video = part.round,
			.to = FileLoadTo(
				_peer->id,
				action.options,
				action.replyTo,
				action.replaceMediaOf),
			.caption = file.caption,
		}));
}

void Job::sendSticker(Part &part) {
	Api::SendExistingDocument(
		Api::MessageToSend(nextAction()),
		part.files.front().document);
}

void Job::sendPoll(Part &part) {
	const auto original = part.poll;
	auto copy = PollData(&_session->data(), base::RandomValue<PollId>());
	copy.question = original->question;
	copy.solution = original->solution;
	copy.answers.reserve(original->answers.size());
	for (const auto &answer : original->answers) {
		auto added = PollAnswer();
		added.text = answer.text;
		added.option = answer.option;
		added.correct = answer.correct;
		copy.answers.push_back(std::move(added));
	}
	using Flag = PollData::Flag;
	auto flags = original->flags() & (Flag::MultiChoice
		| Flag::Quiz
		| Flag::ShuffleAnswers
		| Flag::RevotingDisabled
		| Flag::HideResultsUntilClose
		| Flag::OpenAnswers
		| Flag::PublicVotes);
	if (_peer->isBroadcast()) {
		flags &= ~PollData::Flags(Flag::PublicVotes);
	}
	copy.setFlags(flags);
	const auto peer = _peer;
	_session->api().polls().create(
		copy,
		part.richText,
		nextAction(),
		[] {},
		[=](bool) { ShowFailedToast(peer); });
}

void Job::sendContact(Part &part) {
	const auto peer = _peer;
	_session->api().shareContact(
		part.phone,
		part.firstName,
		part.lastName,
		nextAction(),
		[=](bool done) {
			if (!done) {
				ShowFailedToast(peer);
			}
		});
}

bool Job::uploadsFinished(Part &part) {
	if (part.expected <= 0) {
		return true;
	}
	CollectAlbumIds(part);
	const auto peerId = _peer->id;
	auto &owner = _session->data();
	for (const auto &id : part.localIds) {
		if (part.finished.contains(id)) {
			continue;
		} else if (const auto item = owner.message(peerId, id)) {
			if (item->hasFailed()) {
				part.finished.emplace(id);
				_failed = true;
			}
		} else {
			// Sent (the local id was replaced) or removed.
			part.finished.emplace(id);
		}
	}
	if (WaitingTasks(part) > 0) {
		// Some files are still being prepared, they read the disk.
		return false;
	} else if (!part.albumFailed
		&& (part.finished.size() < part.localIds.size())) {
		return false;
	} else if (int(part.localIds.size()) < part.expected) {
		// A file rejected before the upload (too large, etc) doesn't get
		// any local message, it is removed from the album if there is one.
		_failed = true;
	}
	return true;
}

void Job::idChanged(const Data::Session::IdChange &change) {
	if (_finished || _index >= int(_parts.size())) {
		return;
	}
	auto &part = _parts[_index];
	if (part.state != PartState::Sending
		|| part.expected <= 0
		|| change.newId.peer != _peer->id) {
		return;
	}
	CollectAlbumIds(part);
	if (part.localIds.contains(change.oldId)
		&& part.finished.emplace(change.oldId).second) {
		tickDelayed();
	}
}

void Job::uploadFailed(FullMsgId itemId) {
	if (_finished || _index >= int(_parts.size())) {
		return;
	}
	auto &part = _parts[_index];
	if (part.state != PartState::Sending
		|| part.expected <= 0
		|| itemId.peer != _peer->id) {
		return;
	}
	CollectAlbumIds(part);
	if (!part.localIds.contains(itemId.msg)) {
		return;
	} else if (!_session->data().message(itemId)) {
		// The upload is cancelled because the message is being removed,
		// uploadsFinished() counts it as finished without a failure.
		return;
	}
	_failed = true;
	part.finished.emplace(itemId.msg);
	if (part.album) {
		// The album won't be sent without this file, don't wait for it.
		part.albumFailed = true;
	}
	tickDelayed();
}

void Job::finish() {
	if (_finished) {
		return;
	}
	_finished = true;
	_timer.cancel();
	if (_failed) {
		ShowFailedToast(_peer);
	}
	if (auto done = base::take(_done)) {
		done();
	}
	_manager->jobFinished();
}

void StartCopy(
		const HistoryItemsList &items,
		Data::ForwardOptions options,
		Api::SendAction action,
		FnMut<void()> done) {
	const auto history = action.history;
	auto parts = BuildParts(items, options, history->peer);
	ManagerFor(&history->session())->start(
		std::move(action),
		std::move(parts),
		options,
		std::move(done));
}

[[nodiscard]] bool NeedsCopy(const HistoryItemsList &items) {
	return Get().forwardProtectedAsCopy()
		&& ranges::any_of(items, [](not_null<HistoryItem*> item) {
			return !item->isService() && ItemForwardProtected(item);
		});
}

} // namespace

bool PeerAllowsForwardingReal(not_null<const PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return user->allowsForwarding();
	} else if (const auto channel = peer->asChannel()) {
		return channel->allowsForwarding();
	} else if (const auto chat = peer->asChat()) {
		return chat->allowsForwarding();
	}
	return false;
}

bool ItemForwardProtected(not_null<const HistoryItem*> item) {
	return item->forbidsForwardReal()
		|| !PeerAllowsForwardingReal(item->history()->peer);
}

bool ItemForwardAllowed(not_null<const HistoryItem*> item) {
	return !IsKeptDeleted(item)
		&& (Get().forwardProtectedAsCopy() || !ItemForwardProtected(item));
}

bool ForwardAsCopyIfProtected(
		Data::ResolvedForwardDraft &draft,
		const Api::SendAction &action,
		FnMut<void()> &successCallback) {
	if (!NeedsCopy(draft.items)) {
		return false;
	}
	StartCopy(
		base::take(draft.items),
		draft.options,
		action,
		base::take(successCallback));
	return true;
}

bool ShareAsCopyIfProtected(
		const HistoryItemsList &items,
		const std::vector<not_null<Data::Thread*>> &threads,
		const TextWithTags &comment,
		const Api::SendOptions &options,
		Data::ForwardOptions forwardOptions) {
	if (threads.empty() || !NeedsCopy(items)) {
		return false;
	}
	for (const auto &thread : threads) {
		auto action = Api::SendAction(thread, options);
		action.clearDraft = false;
		if (!comment.text.isEmpty()) {
			auto message = Api::MessageToSend(action);
			message.textWithTags = comment;
			thread->owningHistory()->session().api().sendMessage(
				std::move(message));
		} else {
			ReadOnSendFor(thread, options);
		}
		StartCopy(items, forwardOptions, std::move(action), nullptr);
	}
	return true;
}

} // namespace Oblivion
