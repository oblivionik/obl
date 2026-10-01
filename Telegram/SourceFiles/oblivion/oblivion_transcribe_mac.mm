/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_transcribe.h"

#include "base/platform/mac/base_utilities_mac.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>
#import <objc/runtime.h>

namespace Oblivion::Transcribe::details {

class Recognition;

} // namespace Oblivion::Transcribe::details

using OblivionRecognitionWeak = std::weak_ptr<
	Oblivion::Transcribe::details::Recognition>;

@interface OblivionSpeechDelegate : NSObject<SFSpeechRecognitionTaskDelegate> {
	OblivionRecognitionWeak _owner;
}

- (id)initWithOwner:(OblivionRecognitionWeak)owner;

@end // @interface OblivionSpeechDelegate

namespace Oblivion::Transcribe::details {
namespace {

using Platform::NS2QString;

// Apple limits server-based requests to about a minute of audio.
constexpr auto kServerMaxSeconds = 55;
constexpr auto kChunkMinSeconds = 35;
constexpr auto kChunkMaxSeconds = 50;
constexpr auto kQuietWindow = kSampleRate / 50; // 20 ms.
constexpr auto kQuietSpan = 10; // Windows, 200 ms.
constexpr auto kBufferFrames = kSampleRate / 2;
constexpr auto kInactivityTimeout = 30; // Seconds without any callback.
constexpr auto kResetTolerance = 0.3; // Seconds.
constexpr auto kNoSpeechErrorCode = 1110;

char kDelegateKey = 0;
char kRecognizerKey = 0;

[[nodiscard]] QString NormalizedText(const QString &text) {
	auto result = QString();
	result.reserve(text.size());
	auto space = true;
	for (const auto &ch : text) {
		if (ch.isLetterOrNumber()) {
			result.append(ch.toLower());
			space = false;
		} else if (!space) {
			result.append(' ');
			space = true;
		}
	}
	return result.trimmed();
}

[[nodiscard]] QStringList Words(const QString &text) {
	return NormalizedText(text).split(' ', Qt::SkipEmptyParts);
}

[[nodiscard]] QString Join(const QString &a, const QString &b) {
	const auto first = a.trimmed();
	const auto second = b.trimmed();
	return first.isEmpty()
		? second
		: second.isEmpty()
		? first
		: (first + ' ' + second);
}

// Some macOS versions restart the hypothesis after a pause instead of
// continuing it and then report only the last utterance as the final
// result. We detect such restarts (the new hypothesis starts where the
// previous one ended, or it is shorter and starts with another word) and
// keep the previous utterance, see Recognition::hypothesis().
[[nodiscard]] bool LooksLikeReset(
		const QString &was,
		double wasEnd,
		const QString &now,
		double nowStart) {
	if (was.isEmpty() || now.isEmpty()) {
		return false;
	} else if (wasEnd > 0. && nowStart > 0.) {
		return (nowStart >= wasEnd - kResetTolerance);
	}
	const auto wasWords = Words(was);
	const auto nowWords = Words(now);
	return (wasWords.size() >= 3)
		&& !nowWords.isEmpty()
		&& (nowWords.size() < wasWords.size())
		&& (nowWords.front() != wasWords.front());
}

// Final results may contain either the whole recognized audio or only
// the last utterance. If the final text starts (up to small corrections)
// with the utterances we already kept, it replaces them, otherwise it is
// appended. Short kept parts must match exactly, otherwise a new phrase
// starting with the same common word would swallow the previous one.
[[nodiscard]] bool Covers(const QString &full, const QString &part) {
	const auto fullWords = Words(full);
	const auto partWords = Words(part);
	if (partWords.isEmpty() || fullWords.size() < partWords.size()) {
		return false;
	} else if (partWords.size() < 3) {
		return (fullWords.mid(0, partWords.size()) == partWords);
	}
	const auto head = fullWords.mid(0, partWords.size() + 2);
	const auto matched = ranges::count_if(partWords, [&](const QString &w) {
		return head.contains(w);
	});
	return (matched * 3 >= partWords.size() * 2);
}

[[nodiscard]] bool IsNoSpeechError(NSError *error) {
	return error
		&& (error.code == kNoSpeechErrorCode)
		&& [error.domain isEqualToString:@"kAFAssistantErrorDomain"];
}

[[nodiscard]] double FirstSegmentStart(SFTranscription *transcription) {
	const auto segments = transcription.segments;
	return segments.count ? segments.firstObject.timestamp : 0.;
}

[[nodiscard]] double LastSegmentEnd(SFTranscription *transcription) {
	SFTranscriptionSegment *last = transcription.segments.lastObject;
	return last ? (last.timestamp + last.duration) : 0.;
}

[[nodiscard]] QString NormalizedLocale(const QString &identifier) {
	auto result = identifier.section('@', 0, 0);
	return result.replace('_', '-');
}

[[nodiscard]] QString RegionOf(const QString &identifier) {
	const auto parts = identifier.split('-', Qt::SkipEmptyParts);
	return (parts.size() > 1) ? parts.back().toLower() : QString();
}

[[nodiscard]] QString DefaultRegion(const QString &language) {
	static const auto kRegions = base::flat_map<QString, QString>{
		{ u"ar"_q, u"sa"_q },
		{ u"ca"_q, u"es"_q },
		{ u"cs"_q, u"cz"_q },
		{ u"da"_q, u"dk"_q },
		{ u"el"_q, u"gr"_q },
		{ u"en"_q, u"us"_q },
		{ u"he"_q, u"il"_q },
		{ u"hi"_q, u"in"_q },
		{ u"ja"_q, u"jp"_q },
		{ u"kk"_q, u"kz"_q },
		{ u"ko"_q, u"kr"_q },
		{ u"ms"_q, u"my"_q },
		{ u"nb"_q, u"no"_q },
		{ u"pt"_q, u"br"_q },
		{ u"sv"_q, u"se"_q },
		{ u"uk"_q, u"ua"_q },
		{ u"vi"_q, u"vn"_q },
		{ u"zh"_q, u"cn"_q },
	};
	const auto i = kRegions.find(language);
	return (i != end(kRegions)) ? i->second : language;
}

[[nodiscard]] NSLocale *FindSupportedLocale(const QString &requested) {
	const auto identifier = NormalizedLocale(requested);
	const auto language = identifier.section('-', 0, 0).toLower();
	if (language.isEmpty()) {
		return nil;
	}
	const auto system = NormalizedLocale(
		NS2QString([[NSLocale currentLocale] localeIdentifier]));
	const auto systemRegion = RegionOf(system);
	const auto defaultRegion = DefaultRegion(language);

	NSLocale *sameSystemRegion = nil;
	NSLocale *sameDefaultRegion = nil;
	NSLocale *any = nil;
	auto anyIdentifier = QString();
	for (NSLocale *locale in [SFSpeechRecognizer supportedLocales]) {
		const auto candidate = NormalizedLocale(
			NS2QString(locale.localeIdentifier));
		if (!candidate.compare(identifier, Qt::CaseInsensitive)) {
			return locale;
		} else if (candidate.section('-', 0, 0).toLower() != language) {
			continue;
		}
		const auto region = RegionOf(candidate);
		if (!sameSystemRegion
			&& !systemRegion.isEmpty()
			&& region == systemRegion) {
			sameSystemRegion = locale;
		}
		if (!sameDefaultRegion && region == defaultRegion) {
			sameDefaultRegion = locale;
		}
		if (!any || candidate < anyIdentifier) {
			any = locale;
			anyIdentifier = candidate;
		}
	}
	return sameSystemRegion
		? sameSystemRegion
		: sameDefaultRegion
		? sameDefaultRegion
		: any;
}

[[nodiscard]] NSLocale *ResolveLocale(
		const QString &requested,
		bool systemFallback) {
	if (const auto result = FindSupportedLocale(requested)) {
		return result;
	} else if (!systemFallback) {
		return nil;
	}
	const auto current = NS2QString(
		[[NSLocale currentLocale] localeIdentifier]);
	if (const auto result = FindSupportedLocale(current)) {
		return result;
	}
	for (NSString *preferred in [NSLocale preferredLanguages]) {
		if (const auto result = FindSupportedLocale(NS2QString(preferred))) {
			return result;
		}
	}
	return nil;
}

// Picks a pause near the end of [from, till) to split the audio there.
[[nodiscard]] int64 FindQuietPoint(
		const std::vector<int16> &samples,
		int64 from,
		int64 till) {
	const auto windows = int((till - from) / kQuietWindow);
	if (windows <= kQuietSpan) {
		return till;
	}
	auto energy = std::vector<int64>(windows, 0);
	for (auto i = 0; i != windows; ++i) {
		const auto start = from + int64(i) * kQuietWindow;
		auto sum = int64(0);
		for (auto j = start, end = start + kQuietWindow; j != end; ++j) {
			sum += std::abs(int(samples[j]));
		}
		energy[i] = sum;
	}
	auto span = int64(0);
	for (auto i = 0; i != kQuietSpan; ++i) {
		span += energy[i];
	}
	auto best = span;
	auto bestIndex = 0;
	for (auto i = kQuietSpan; i != windows; ++i) {
		span += energy[i] - energy[i - kQuietSpan];
		if (span <= best) {
			best = span;
			bestIndex = i - kQuietSpan + 1;
		}
	}
	return from + (int64(bestIndex) + kQuietSpan / 2) * kQuietWindow;
}

} // namespace

class Recognition final : public std::enable_shared_from_this<Recognition> {
public:
	Recognition(Request &&request, Fn<void(Update)> callback);
	~Recognition();

	void start();
	void cancel();

	void authorized();
	void denied();
	void hypothesis(
		SFSpeechRecognitionTask *task,
		SFTranscription *transcription);
	void recognized(
		SFSpeechRecognitionTask *task,
		SFSpeechRecognitionResult *result);
	void taskFinished(SFSpeechRecognitionTask *task, bool successfully);
	void timeoutCheck(int generation);
	void nextChunk();

private:
	void buildChunks();
	void scheduleChunk();
	void startChunk();
	void chunkFinished(bool successfully);
	void armTimeout();
	void reportPartial();
	void releaseTask();
	void finish(Status status, QString text = QString());

	[[nodiscard]] QString chunkText() const;

	std::vector<int16> _samples;
	const QString _locale;
	const bool _systemFallback = false;
	const bool _partial = true;
	Fn<void(Update)> _callback;

	SFSpeechRecognizer *_recognizer = nil;
	SFSpeechAudioBufferRecognitionRequest *_request = nil;
	SFSpeechRecognitionTask *_task = nil;
	bool _onDevice = false;

	std::vector<std::pair<int64, int64>> _chunks;
	int _chunkIndex = 0;
	QString _done; // Text of the finished chunks.
	QString _committed; // Finished utterances of the current chunk.
	QString _hypothesis; // Current utterance of the current chunk.
	double _hypothesisEnd = 0.;
	int _timeoutGeneration = 0;
	bool _lastChunkFailed = false;
	bool _incomplete = false; // Some chunk audio was not recognized.
	bool _finished = false;

};

[[nodiscard]] std::vector<std::shared_ptr<Recognition>> &Running() {
	static auto result = std::vector<std::shared_ptr<Recognition>>();
	return result;
}

} // namespace Oblivion::Transcribe::details

@implementation OblivionSpeechDelegate

- (id)initWithOwner:(OblivionRecognitionWeak)owner {
	if (self = [super init]) {
		_owner = std::move(owner);
	}
	return self;
}

- (void)speechRecognitionTask:(SFSpeechRecognitionTask *)task
		didHypothesizeTranscription:(SFTranscription *)transcription {
	if (const auto strong = _owner.lock()) {
		strong->hypothesis(task, transcription);
	}
}

- (void)speechRecognitionTask:(SFSpeechRecognitionTask *)task
		didFinishRecognition:(SFSpeechRecognitionResult *)result {
	if (const auto strong = _owner.lock()) {
		strong->recognized(task, result);
	}
}

- (void)speechRecognitionTask:(SFSpeechRecognitionTask *)task
		didFinishSuccessfully:(BOOL)successfully {
	if (const auto strong = _owner.lock()) {
		strong->taskFinished(task, successfully);
	}
}

- (void)speechRecognitionTaskWasCancelled:(SFSpeechRecognitionTask *)task {
	if (const auto strong = _owner.lock()) {
		strong->taskFinished(task, false);
	}
}

@end // @implementation OblivionSpeechDelegate

namespace Oblivion::Transcribe {
namespace details {

Recognition::Recognition(Request &&request, Fn<void(Update)> callback)
: _samples(std::move(request.samples))
, _locale(request.locale)
, _systemFallback(request.systemFallback)
, _partial(request.partial)
, _callback(std::move(callback)) {
}

Recognition::~Recognition() {
	releaseTask();
	if (_recognizer) {
		[_recognizer release];
		_recognizer = nil;
	}
}

void Recognition::start() {
	NSString *usage = [[NSBundle mainBundle]
		objectForInfoDictionaryKey:@"NSSpeechRecognitionUsageDescription"];
	if (!usage) {
		// Without the key macOS kills the app on the authorization request.
		LOG(("Oblivion Transcribe Error: "
			"NSSpeechRecognitionUsageDescription is missing in Info.plist."));
		finish(Status::Unavailable);
		return;
	}
	switch ([SFSpeechRecognizer authorizationStatus]) {
	case SFSpeechRecognizerAuthorizationStatusAuthorized:
		authorized();
		return;
	case SFSpeechRecognizerAuthorizationStatusNotDetermined: {
		const auto weak = weak_from_this();
		[SFSpeechRecognizer requestAuthorization:^(
				SFSpeechRecognizerAuthorizationStatus status) {
			dispatch_async(dispatch_get_main_queue(), ^{
				if (const auto strong = weak.lock()) {
					if (status
						== SFSpeechRecognizerAuthorizationStatusAuthorized) {
						strong->authorized();
					} else {
						strong->denied();
					}
				}
			});
		}];
	} return;
	case SFSpeechRecognizerAuthorizationStatusDenied:
	case SFSpeechRecognizerAuthorizationStatusRestricted:
		break;
	}
	denied();
}

void Recognition::denied() {
	finish(Status::Denied);
}

void Recognition::authorized() {
	if (_finished) {
		return;
	}
	@autoreleasepool {
		NSLocale *locale = ResolveLocale(_locale, _systemFallback);
		if (!locale) {
			LOG(("Oblivion Transcribe Error: No recognizer for '%1'."
				).arg(_locale));
			finish(Status::Unavailable);
			return;
		}
		_recognizer = [[SFSpeechRecognizer alloc] initWithLocale:locale];
		if (!_recognizer) {
			finish(Status::Unavailable);
			return;
		} else if (![_recognizer isAvailable]) {
			LOG(("Oblivion Transcribe Error: "
				"Recognizer for '%1' is not available now."
				).arg(NS2QString(locale.localeIdentifier)));
			finish(Status::Failed);
			return;
		}
		_recognizer.defaultTaskHint = SFSpeechRecognitionTaskHintDictation;
		_onDevice = _recognizer.supportsOnDeviceRecognition;
	}
	buildChunks();
	startChunk();
}

void Recognition::buildChunks() {
	const auto total = int64(_samples.size());
	if (_onDevice || total <= int64(kServerMaxSeconds) * kSampleRate) {
		_chunks.emplace_back(0, total);
		return;
	}
	auto from = int64(0);
	while (total - from > int64(kServerMaxSeconds) * kSampleRate) {
		const auto till = FindQuietPoint(
			_samples,
			from + int64(kChunkMinSeconds) * kSampleRate,
			from + int64(kChunkMaxSeconds) * kSampleRate);
		_chunks.emplace_back(from, till);
		from = till;
	}
	_chunks.emplace_back(from, total);
}

void Recognition::startChunk() {
	Expects(_chunkIndex < int(_chunks.size()));

	_committed = QString();
	_hypothesis = QString();
	_hypothesisEnd = 0.;

	@autoreleasepool {
		_request = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
		_request.shouldReportPartialResults = YES;
		_request.taskHint = SFSpeechRecognitionTaskHintDictation;
		if (_onDevice) {
			_request.requiresOnDeviceRecognition = YES;
		}
		if (@available(macOS 13.0, *)) {
			_request.addsPunctuation = YES;
		}

		OblivionSpeechDelegate *delegate = [[OblivionSpeechDelegate alloc]
			initWithOwner:weak_from_this()];
		_task = [[_recognizer
			recognitionTaskWithRequest:_request
			delegate:delegate] retain];
		if (!_task) {
			[delegate release];
			chunkFinished(false);
			return;
		}

		// The task keeps its delegate and recognizer alive while it may
		// still call them, even after we release everything on our side.
		objc_setAssociatedObject(
			_task,
			&kDelegateKey,
			delegate,
			OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		objc_setAssociatedObject(
			_task,
			&kRecognizerKey,
			_recognizer,
			OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		[delegate release];

		AVAudioFormat *format = [[AVAudioFormat alloc]
			initWithCommonFormat:AVAudioPCMFormatFloat32
			sampleRate:double(kSampleRate)
			channels:1
			interleaved:NO];
		const auto [from, till] = _chunks[_chunkIndex];
		for (auto offset = from; offset < till; offset += kBufferFrames) {
			const auto count = std::min(int64(kBufferFrames), till - offset);
			AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc]
				initWithPCMFormat:format
				frameCapacity:AVAudioFrameCount(count)];
			if (!buffer) {
				break;
			}
			buffer.frameLength = AVAudioFrameCount(count);
			const auto data = buffer.floatChannelData[0];
			const auto source = _samples.data() + offset;
			for (auto i = int64(0); i != count; ++i) {
				data[i] = source[i] / 32768.f;
			}
			[_request appendAudioPCMBuffer:buffer];
			[buffer release];
		}
		[format release];
		[_request endAudio];
	}
	armTimeout();
}

void Recognition::hypothesis(
		SFSpeechRecognitionTask *task,
		SFTranscription *transcription) {
	if (_finished || task != _task) {
		return;
	}
	armTimeout();
	const auto text = NS2QString(transcription.formattedString);
	const auto start = FirstSegmentStart(transcription);
	if (LooksLikeReset(_hypothesis, _hypothesisEnd, text, start)) {
		_committed = Join(_committed, _hypothesis);
	}
	_hypothesis = text;
	_hypothesisEnd = LastSegmentEnd(transcription);
	reportPartial();
}

void Recognition::recognized(
		SFSpeechRecognitionTask *task,
		SFSpeechRecognitionResult *result) {
	if (_finished || task != _task) {
		return;
	}
	armTimeout();
	const auto text = NS2QString(result.bestTranscription.formattedString);
	_committed = Covers(text, _committed) ? text : Join(_committed, text);
	_hypothesis = QString();
	_hypothesisEnd = 0.;
	reportPartial();
}

void Recognition::taskFinished(
		SFSpeechRecognitionTask *task,
		bool successfully) {
	if (_finished || task != _task) {
		return;
	}
	NSError *error = successfully ? nil : task.error;
	if (error) {
		LOG(("Oblivion Transcribe Error: %1 (%2 %3)."
			).arg(NS2QString(error.localizedDescription)
			).arg(NS2QString(error.domain)
			).arg(error.code));
	}
	chunkFinished(successfully || IsNoSpeechError(error));
}

void Recognition::timeoutCheck(int generation) {
	if (_finished || generation != _timeoutGeneration || !_task) {
		return;
	}
	LOG(("Oblivion Transcribe Error: Recognition timeout."));
	chunkFinished(false);
}

void Recognition::chunkFinished(bool successfully) {
	const auto text = chunkText();
	releaseTask();
	if (successfully || !text.isEmpty()) {
		if (!successfully) {
			// The chunk failed midway, the rest of its audio is missing.
			_incomplete = true;
		}
		_done = Join(_done, text);
		_lastChunkFailed = !successfully;
	} else if (_onDevice && _done.isEmpty()) {
		LOG(("Oblivion Transcribe: Retrying with server recognition."));
		_onDevice = false;
		_chunks.clear();
		_chunkIndex = 0;
		buildChunks();
		scheduleChunk();
		return;
	} else if (_done.isEmpty()) {
		finish(Status::Failed);
		return;
	} else {
		// Keep the text recognized so far. A single failed server chunk
		// is skipped, if the next one fails too we stop with what we have.
		_incomplete = true;
		if (_lastChunkFailed) {
			finish(Status::Done, _done);
			return;
		}
		_lastChunkFailed = true;
	}
	if (++_chunkIndex < int(_chunks.size())) {
		scheduleChunk();
	} else {
		// Empty _done here means there was no speech in the audio.
		finish(Status::Done, _done);
	}
}

void Recognition::scheduleChunk() {
	const auto weak = weak_from_this();
	dispatch_async(dispatch_get_main_queue(), ^{
		if (const auto strong = weak.lock()) {
			strong->nextChunk();
		}
	});
}

void Recognition::nextChunk() {
	if (!_finished) {
		startChunk();
	}
}

QString Recognition::chunkText() const {
	return Join(_committed, _hypothesis);
}

void Recognition::reportPartial() {
	if (_partial && _callback) {
		_callback({
			.status = Status::Partial,
			.text = Join(_done, chunkText()),
		});
	}
}

void Recognition::armTimeout() {
	const auto generation = ++_timeoutGeneration;
	const auto weak = weak_from_this();
	dispatch_after(
		dispatch_time(DISPATCH_TIME_NOW, kInactivityTimeout * NSEC_PER_SEC),
		dispatch_get_main_queue(),
		^{
			if (const auto strong = weak.lock()) {
				strong->timeoutCheck(generation);
			}
		});
}

void Recognition::releaseTask() {
	++_timeoutGeneration;
	if (_task) {
		if (_task.state != SFSpeechRecognitionTaskStateCompleted) {
			[_task cancel];
		}
		[_task release];
		_task = nil;
	}
	if (_request) {
		[_request release];
		_request = nil;
	}
}

void Recognition::cancel() {
	_callback = nullptr;
	if (!_finished) {
		finish(Status::Failed);
	}
}

void Recognition::finish(Status status, QString text) {
	const auto self = shared_from_this();
	const auto callback = _finished
		? Fn<void(Update)>()
		: base::take(_callback);
	_finished = true;
	releaseTask();
	_samples = std::vector<int16>();

	auto &running = Running();
	running.erase(ranges::remove(running, self), end(running));

	if (callback) {
		callback({
			.status = status,
			.text = std::move(text),
			.incomplete = (status == Status::Done) && _incomplete,
		});
	}
}

} // namespace details

bool SystemRecognizerSupported() {
	return true;
}

Fn<void()> SystemRecognize(Request &&request, Fn<void(Update)> callback) {
	auto recognition = std::make_shared<details::Recognition>(
		std::move(request),
		std::move(callback));
	details::Running().push_back(recognition);
	recognition->start();
	return [weak = std::weak_ptr<details::Recognition>(recognition)] {
		if (const auto strong = weak.lock()) {
			strong->cancel();
		}
	};
}

void OpenSystemPrivacySettings() {
	NSURL *url = [NSURL URLWithString:@"x-apple.systempreferences:"
		"com.apple.preference.security?Privacy_SpeechRecognition"];
	if (url) {
		[[NSWorkspace sharedWorkspace] openURL:url];
	}
}

} // namespace Oblivion::Transcribe
