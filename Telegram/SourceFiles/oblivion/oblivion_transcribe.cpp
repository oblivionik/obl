/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_transcribe.h"

#include "api/api_transcribes.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_settings.h"
#include "storage/file_download.h"
#include "ui/layers/show.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "apiwrap.h"
#include "settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

#include <deque>

namespace Oblivion::Transcribe {
namespace {

constexpr auto kMaxDuration = 30 * 60 * crl::time(1000);
constexpr auto kMaxFileSize = 64 * 1024 * 1024;
constexpr auto kDownloadTimeout = 2 * 60 * crl::time(1000);
constexpr auto kPartialDelay = crl::time(300);
constexpr auto kToastDuration = crl::time(4000);
constexpr auto kCacheLimit = 2000;
constexpr auto kCacheVersion = 1;

struct Locale {
	QString identifier;
	bool systemFallback = false;

	[[nodiscard]] QString key() const {
		return systemFallback ? (identifier + '*') : identifier;
	}
};

[[nodiscard]] QString InterfaceLocale() {
	if (CurrentLanguageIsRussian()) {
		return u"ru-RU"_q;
	}
	static const auto kLocales = std::vector<std::pair<QString, QString>>{
		{ u"en"_q, u"en-US"_q },
		{ u"ru"_q, u"ru-RU"_q },
		{ u"uk"_q, u"uk-UA"_q },
		{ u"de"_q, u"de-DE"_q },
		{ u"es"_q, u"es-ES"_q },
		{ u"fr"_q, u"fr-FR"_q },
		{ u"it"_q, u"it-IT"_q },
		{ u"pt-br"_q, u"pt-BR"_q },
		{ u"pt"_q, u"pt-PT"_q },
		{ u"tr"_q, u"tr-TR"_q },
		{ u"kk"_q, u"kk-KZ"_q },
		{ u"nl"_q, u"nl-NL"_q },
		{ u"pl"_q, u"pl-PL"_q },
		{ u"ko"_q, u"ko-KR"_q },
		{ u"ja"_q, u"ja-JP"_q },
		{ u"zh-hans"_q, u"zh-CN"_q },
		{ u"zh-hant"_q, u"zh-TW"_q },
		{ u"ar"_q, u"ar-SA"_q },
		{ u"id"_q, u"id-ID"_q },
		{ u"ms"_q, u"ms-MY"_q },
		{ u"cs"_q, u"cs-CZ"_q },
		{ u"sk"_q, u"sk-SK"_q },
		{ u"ro"_q, u"ro-RO"_q },
		{ u"hu"_q, u"hu-HU"_q },
		{ u"el"_q, u"el-GR"_q },
		{ u"he"_q, u"he-IL"_q },
		{ u"sv"_q, u"sv-SE"_q },
		{ u"da"_q, u"da-DK"_q },
		{ u"fi"_q, u"fi-FI"_q },
		{ u"nb"_q, u"nb-NO"_q },
		{ u"no"_q, u"nb-NO"_q },
		{ u"ca"_q, u"ca-ES"_q },
		{ u"hr"_q, u"hr-HR"_q },
		{ u"vi"_q, u"vi-VN"_q },
		{ u"th"_q, u"th-TH"_q },
		{ u"hi"_q, u"hi-IN"_q },
	};
	const auto normalize = [](QString id) {
		id = id.toLower();
		if (id.endsWith(u"-raw"_q)) {
			id.chop(4);
		}
		return id;
	};
	const auto &lang = Lang::GetInstance();
	const auto ids = std::array{
		normalize(lang.isCustom() ? QString() : lang.id()),
		normalize(lang.baseId()),
	};
	for (const auto &id : ids) {
		for (const auto &[from, to] : kLocales) {
			if (id == from) {
				return to;
			}
		}
	}
	for (const auto &id : ids) {
		const auto language = id.section('-', 0, 0);
		if (language.size() == 2) {
			return language;
		}
	}
	return QString();
}

[[nodiscard]] Locale ChooseLocale() {
	const auto chosen = Get().transcribeLanguage();
	return chosen.isEmpty()
		? Locale{ InterfaceLocale(), true }
		: Locale{ chosen, false };
}

class Cache final {
public:
	[[nodiscard]] std::optional<QString> find(
		DocumentId documentId,
		const QString &locale);
	void put(
		DocumentId documentId,
		const QString &locale,
		const QString &text);

private:
	struct Entry {
		QString text;
		TimeId date = 0;
	};

	[[nodiscard]] static QString Path();
	[[nodiscard]] static QString Key(
		DocumentId documentId,
		const QString &locale);

	void load();
	void write() const;

	base::flat_map<QString, Entry> _entries;
	bool _loaded = false;

};

QString Cache::Path() {
	return cWorkingDir() + u"tdata/oblivion/transcribes.json"_q;
}

QString Cache::Key(DocumentId documentId, const QString &locale) {
	return QString::number(documentId) + '|' + locale;
}

std::optional<QString> Cache::find(
		DocumentId documentId,
		const QString &locale) {
	load();
	const auto i = _entries.find(Key(documentId, locale));
	if (i == end(_entries)) {
		return std::nullopt;
	}
	return i->second.text;
}

void Cache::put(
		DocumentId documentId,
		const QString &locale,
		const QString &text) {
	load();
	_entries[Key(documentId, locale)] = Entry{
		.text = text,
		.date = base::unixtime::now(),
	};
	while (_entries.size() > kCacheLimit) {
		auto oldest = begin(_entries);
		for (auto i = begin(_entries); i != end(_entries); ++i) {
			if (i->second.date < oldest->second.date) {
				oldest = i;
			}
		}
		_entries.erase(oldest);
	}
	write();
}

void Cache::load() {
	if (_loaded) {
		return;
	}
	_loaded = true;

	auto file = QFile(Path());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	file.close();
	const auto object = document.object();
	if (object.value(u"version"_q).toInt() != kCacheVersion) {
		return;
	}
	for (const auto &value : object.value(u"items"_q).toArray()) {
		const auto item = value.toObject();
		const auto documentId = item.value(
			u"document"_q).toString().toULongLong();
		const auto locale = item.value(u"locale"_q).toString();
		const auto text = item.value(u"text"_q).toString();
		if (!documentId || text.isEmpty()) {
			continue;
		}
		_entries[Key(documentId, locale)] = Entry{
			.text = text,
			.date = TimeId(item.value(u"date"_q).toInteger()),
		};
	}
}

void Cache::write() const {
	auto items = QJsonArray();
	for (const auto &[key, entry] : _entries) {
		items.append(QJsonObject{
			{ u"document"_q, key.section('|', 0, 0) },
			{ u"locale"_q, key.section('|', 1) },
			{ u"text"_q, entry.text },
			{ u"date"_q, qint64(entry.date) },
		});
	}
	const auto content = QJsonDocument(QJsonObject{
		{ u"version"_q, kCacheVersion },
		{ u"items"_q, items },
	}).toJson(QJsonDocument::Compact);

	const auto path = Path();
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Oblivion Transcribe Error: Could not write '%1'.").arg(path));
		return;
	}
	file.write(content);
	if (!file.commit()) {
		LOG(("Oblivion Transcribe Error: Could not save '%1'.").arg(path));
	}
}

[[nodiscard]] Cache &Results() {
	static auto result = Cache();
	return result;
}

struct Reader {
	const QByteArray *data = nullptr;
	int64 offset = 0;
};

int ReadBytes(void *opaque, uint8_t *buffer, int bufferSize) {
	const auto reader = static_cast<Reader*>(opaque);
	const auto available = int64(reader->data->size()) - reader->offset;
	const auto count = int(std::min(int64(bufferSize), available));
	if (count <= 0) {
		return AVERROR_EOF;
	}
	memcpy(buffer, reader->data->constData() + reader->offset, count);
	reader->offset += count;
	return count;
}

int64_t SeekBytes(void *opaque, int64_t offset, int whence) {
	const auto reader = static_cast<Reader*>(opaque);
	const auto size = int64(reader->data->size());
	switch (whence & ~AVSEEK_FORCE) {
	case AVSEEK_SIZE: return size;
	case SEEK_SET: break;
	case SEEK_CUR: offset += reader->offset; break;
	case SEEK_END: offset += size; break;
	default: return -1;
	}
	if (offset < 0 || offset > size) {
		return -1;
	}
	reader->offset = offset;
	return offset;
}

// Decodes the first audio stream (Ogg/Opus voice or the AAC track
// of a round video) to kSampleRate mono int16. Runs off the main thread.
[[nodiscard]] std::vector<int16> DecodeAudio(const QByteArray &bytes) {
	using namespace FFmpeg;

	auto reader = Reader{ .data = &bytes };
	auto format = MakeFormatPointer(&reader, &ReadBytes, nullptr, &SeekBytes);
	if (!format) {
		return {};
	}
	if (const auto error = AvErrorWrap(
			avformat_find_stream_info(format.get(), nullptr))) {
		LogError(u"avformat_find_stream_info"_q, error);
		return {};
	}
	const auto streamId = av_find_best_stream(
		format.get(),
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (streamId < 0) {
		LogError(u"av_find_best_stream"_q, AvErrorWrap(streamId));
		return {};
	}
	auto codec = MakeCodecPointer({ .stream = format->streams[streamId] });
	if (!codec) {
		return {};
	}

	auto result = std::vector<int16>();
	auto frame = MakeFramePointer();
	auto packet = Packet();
	auto resampler = SwresamplePointer();
	AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
	const auto maxSamples = int64(kSampleRate) * (kMaxDuration / 1000 + 5);

	const auto append = [&](const uint8_t **data, int count) {
		const auto available = swr_get_out_samples(resampler.get(), count);
		if (available <= 0) {
			return true;
		}
		const auto offset = result.size();
		result.resize(offset + available);
		auto out = reinterpret_cast<uint8_t*>(result.data() + offset);
		const auto converted = swr_convert(
			resampler.get(),
			&out,
			available,
			data,
			count);
		if (converted < 0) {
			result.resize(offset);
			LogError(u"swr_convert"_q, AvErrorWrap(converted));
			return false;
		}
		result.resize(offset + converted);
		return (int64(result.size()) < maxSamples);
	};
	const auto resample = [&](not_null<AVFrame*> decoded) {
		if (!decoded->ch_layout.nb_channels
			|| decoded->format < 0
			|| decoded->sample_rate <= 0) {
			return false;
		}
		resampler = MakeSwresamplePointer(
			&decoded->ch_layout,
			AVSampleFormat(decoded->format),
			decoded->sample_rate,
			&mono,
			AV_SAMPLE_FMT_S16,
			kSampleRate,
			&resampler);
		if (!resampler) {
			return false;
		}
		return append(
			const_cast<const uint8_t**>(decoded->extended_data),
			decoded->nb_samples);
	};
	const auto receive = [&] {
		while (true) {
			const auto error = AvErrorWrap(
				avcodec_receive_frame(codec.get(), frame.get()));
			if (error.code() == AVERROR(EAGAIN)
				|| error.code() == AVERROR_EOF) {
				return true;
			} else if (error) {
				LogError(u"avcodec_receive_frame"_q, error);
				return false;
			}
			const auto good = resample(frame.get());
			av_frame_unref(frame.get());
			if (!good) {
				return false;
			}
		}
	};

	auto good = true;
	while (good) {
		auto &fields = packet.fields();
		const auto error = AvErrorWrap(av_read_frame(format.get(), &fields));
		if (error) {
			if (error.code() != AVERROR_EOF) {
				LogError(u"av_read_frame"_q, error);
			}
			break;
		}
		if (fields.stream_index == streamId) {
			if (const auto error = AvErrorWrap(
					avcodec_send_packet(codec.get(), &fields))) {
				LogError(u"avcodec_send_packet"_q, error);
				good = false;
			} else {
				good = receive();
			}
		}
		av_packet_unref(&fields);
	}
	if (good && !AvErrorWrap(avcodec_send_packet(codec.get(), nullptr))) {
		good = receive();
	}
	if (good && resampler) {
		append(nullptr, 0);
	}
	return result;
}

struct Job final : base::has_weak_ptr {
	Job(
		uint64 id,
		not_null<Main::Session*> session,
		FullMsgId itemId,
		not_null<DocumentData*> document,
		Locale locale)
	: id(id)
	, session(session)
	, itemId(itemId)
	, document(document)
	, locale(std::move(locale))
	, round(document->isVideoMessage()) {
	}

	const uint64 id = 0;
	const not_null<Main::Session*> session;
	const FullMsgId itemId;
	const not_null<DocumentData*> document;
	const Locale locale;
	const bool round = false;

	std::shared_ptr<Data::DocumentMedia> media;
	std::vector<int16> samples;
	rpl::lifetime downloadLifetime;
	base::Timer downloadTimeout;
	Fn<void()> cancel;
	crl::time lastPartialAt = 0;
	bool decoding = false;
	bool finished = false;
};

class Manager final {
public:
	void start(not_null<HistoryItem*> item);

private:
	[[nodiscard]] bool running(
		not_null<Main::Session*> session,
		FullMsgId itemId) const;
	void track(not_null<Main::Session*> session);
	void drop(not_null<Main::Session*> session);
	void remove(uint64 id);

	void download(not_null<Job*> job);
	bool tryDecode(not_null<Job*> job);
	void decoded(not_null<Job*> job, std::vector<int16> &&samples);
	void scheduleNext();
	void recognizeNext();
	void updated(not_null<Job*> job, Update &&update);
	void finish(not_null<Job*> job, Status status, QString text = {});

	std::vector<std::unique_ptr<Job>> _jobs;
	std::deque<base::weak_ptr<Job>> _queue;
	Job *_recognizing = nullptr;
	base::flat_set<not_null<Main::Session*>> _tracked;
	uint64 _autoincrement = 0;
	bool _nextScheduled = false;

};

[[nodiscard]] Manager &Instance() {
	static auto result = Manager();
	return result;
}

void Deliver(
		not_null<Main::Session*> session,
		FullMsgId itemId,
		const QString &text,
		bool pending,
		bool failed,
		bool toolong = false) {
	session->api().transcribes().applyLocal(
		itemId,
		text,
		pending,
		failed,
		toolong);
}

void ShowToast(const QString &text) {
	if (const auto window = Core::App().activeWindow()) {
		window->uiShow()->showToast(text, kToastDuration);
	}
}

void ShowError(Status status) {
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	if (status == Status::Denied) {
		window->uiShow()->showToast(Ui::Toast::Config{
			.text = tr::link(tr::lng_oblivion_transcribe_denied(tr::now)),
			.filter = [](const auto &...) {
				OpenSystemPrivacySettings();
				return false;
			},
			.duration = kToastDuration,
		});
	} else if (status == Status::Unavailable) {
		ShowToast(tr::lng_oblivion_transcribe_unavailable(tr::now));
	} else {
		ShowToast(tr::lng_oblivion_transcribe_failed(tr::now));
	}
}

void Manager::start(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto itemId = item->fullId();
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document
		|| (!document->isVoiceMessage() && !document->isVideoMessage())) {
		Deliver(session, itemId, QString(), false, true);
		return;
	} else if (document->duration() > kMaxDuration) {
		Deliver(session, itemId, QString(), false, true, true);
		return;
	} else if (running(session, itemId)) {
		return;
	}
	const auto locale = ChooseLocale();
	if (const auto cached = Results().find(document->id, locale.key())) {
		Deliver(session, itemId, *cached, false, false);
		return;
	} else if (!SystemRecognizerSupported()) {
		Deliver(session, itemId, QString(), false, true);
		ShowError(Status::Unavailable);
		return;
	}
	track(session);
	_jobs.push_back(std::make_unique<Job>(
		++_autoincrement,
		session,
		itemId,
		document,
		locale));
	download(_jobs.back().get());
}

bool Manager::running(
		not_null<Main::Session*> session,
		FullMsgId itemId) const {
	return ranges::any_of(_jobs, [&](const std::unique_ptr<Job> &job) {
		return (job->session == session)
			&& (job->itemId == itemId)
			&& !job->finished;
	});
}

void Manager::track(not_null<Main::Session*> session) {
	if (_tracked.contains(session)) {
		return;
	}
	_tracked.emplace(session);
	session->lifetime().add([=] {
		Instance().drop(session);
	});
}

void Manager::drop(not_null<Main::Session*> session) {
	_tracked.remove(session);
	for (auto i = begin(_jobs); i != end(_jobs);) {
		const auto job = i->get();
		if (job->session != session) {
			++i;
			continue;
		}
		if (_recognizing == job) {
			_recognizing = nullptr;
			scheduleNext();
		}
		if (const auto cancel = base::take(job->cancel)) {
			cancel();
		}
		i = _jobs.erase(i);
	}
}

void Manager::remove(uint64 id) {
	const auto i = ranges::find(_jobs, id, [](const auto &job) {
		return job->id;
	});
	if (i == end(_jobs)) {
		return;
	}
	if (_recognizing == i->get()) {
		_recognizing = nullptr;
		scheduleNext();
	}
	if (const auto cancel = base::take((*i)->cancel)) {
		cancel();
	}
	_jobs.erase(i);
}

void Manager::download(not_null<Job*> job) {
	job->media = job->document->createMediaView();
	if (tryDecode(job)) {
		return;
	} else if (job->document->size >= Storage::kMaxFileInMemory) {
		// DocumentData::save() asserts on such files without a target.
		finish(job, Status::Failed);
		return;
	}
	const auto document = job->document;
	job->session->data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> loaded) {
		return (loaded == document);
	}) | rpl::on_next([=] {
		if (job->decoding || job->finished || tryDecode(job)) {
			return;
		} else if (!document->loading()) {
			finish(job, Status::Failed);
		} else {
			// Fail only if the download makes no progress for a while.
			job->downloadTimeout.callOnce(kDownloadTimeout);
		}
	}, job->downloadLifetime);

	job->downloadTimeout.setCallback([=] {
		if (!job->decoding) {
			finish(job, Status::Failed);
		}
	});
	job->downloadTimeout.callOnce(kDownloadTimeout);

	document->save(Data::FileOriginMessage(job->itemId), QString());
	if (!job->decoding && !job->finished) {
		tryDecode(job);
	}
}

bool Manager::tryDecode(not_null<Job*> job) {
	if (job->decoding) {
		return true;
	}
	auto bytes = job->media->bytes();
	if (bytes.isEmpty()) {
		const auto &location = job->document->location(true);
		if (location.isEmpty() || !location.accessEnable()) {
			return false;
		}
		auto file = QFile(location.name());
		if (file.size() <= kMaxFileSize && file.open(QIODevice::ReadOnly)) {
			bytes = file.readAll();
			file.close();
		}
		location.accessDisable();
		if (bytes.isEmpty()) {
			return false;
		}
	}
	job->decoding = true;
	job->downloadTimeout.cancel();

	const auto weak = base::make_weak(job.get());
	crl::async([=, bytes = std::move(bytes)] {
		auto samples = DecodeAudio(bytes);
		crl::on_main(weak, [=, samples = std::move(samples)]() mutable {
			Instance().decoded(weak.get(), std::move(samples));
		});
	});
	return true;
}

void Manager::decoded(not_null<Job*> job, std::vector<int16> &&samples) {
	if (job->finished) {
		return;
	} else if (samples.empty()) {
		LOG(("Oblivion Transcribe Error: Could not decode audio."));
		finish(job, Status::Failed);
		return;
	}
	job->media = nullptr;
	job->samples = std::move(samples);
	_queue.push_back(base::make_weak(job.get()));
	recognizeNext();
}

void Manager::scheduleNext() {
	if (_nextScheduled) {
		return;
	}
	_nextScheduled = true;
	crl::on_main([] {
		auto &manager = Instance();
		manager._nextScheduled = false;
		manager.recognizeNext();
	});
}

void Manager::recognizeNext() {
	while (!_recognizing && !_queue.empty()) {
		const auto job = _queue.front().get();
		_queue.pop_front();
		if (!job || job->finished) {
			continue;
		}
		_recognizing = job;
		const auto weak = base::make_weak(job);
		auto cancel = SystemRecognize({
			.samples = base::take(job->samples),
			.locale = job->locale.identifier,
			.systemFallback = job->locale.systemFallback,
			.partial = !job->round,
		}, [=](Update update) {
			if (const auto strong = weak.get()) {
				Instance().updated(strong, std::move(update));
			}
		});
		if (const auto strong = weak.get()) {
			if (!strong->finished) {
				strong->cancel = std::move(cancel);
			}
		}
	}
}

void Manager::updated(not_null<Job*> job, Update &&update) {
	if (job->finished) {
		return;
	} else if (update.status == Status::Partial) {
		const auto now = crl::now();
		if (job->round
			|| update.text.isEmpty()
			|| (job->lastPartialAt
				&& now - job->lastPartialAt < kPartialDelay)) {
			return;
		}
		job->lastPartialAt = now;
		Deliver(job->session, job->itemId, update.text, true, false);
		return;
	}
	job->cancel = nullptr;
	if (update.status == Status::Done) {
		// Empty and incomplete results are not cached,
		// they are recognized again when requested after a restart.
		if (update.text.isEmpty()) {
			ShowToast(tr::lng_oblivion_transcribe_no_speech(tr::now));
		} else if (update.incomplete) {
			ShowToast(tr::lng_oblivion_transcribe_partial(tr::now));
		} else {
			Results().put(job->document->id, job->locale.key(), update.text);
		}
	}
	finish(job, update.status, std::move(update.text));
}

void Manager::finish(not_null<Job*> job, Status status, QString text) {
	if (job->finished) {
		return;
	}
	job->finished = true;
	job->downloadTimeout.cancel();
	if (_recognizing == job) {
		_recognizing = nullptr;
		scheduleNext();
	}
	if (status == Status::Done) {
		Deliver(job->session, job->itemId, text, false, false);
	} else {
		Deliver(job->session, job->itemId, QString(), false, true);
		ShowError(status);
	}

	// We may be inside the job's own rpl subscription or timer callback.
	const auto id = job->id;
	crl::on_main([=] {
		Instance().remove(id);
	});
}

} // namespace

bool Active(not_null<Main::Session*> session) {
	return Get().localTranscribe()
		&& !session->user()->isPremium()
		&& SystemRecognizerSupported();
}

void Start(not_null<HistoryItem*> item) {
	Instance().start(item);
}

#ifndef Q_OS_MAC

bool SystemRecognizerSupported() {
	return false;
}

Fn<void()> SystemRecognize(Request &&request, Fn<void(Update)> callback) {
	callback({ .status = Status::Unavailable });
	return nullptr;
}

void OpenSystemPrivacySettings() {
}

#endif // !Q_OS_MAC

} // namespace Oblivion::Transcribe
