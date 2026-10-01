/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_music_editor.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/platform/base_platform_file_utilities.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/file_utilities.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "oblivion/oblivion_audio.h"
#include "oblivion/oblivion_noise.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "settings/settings_common.h"
#include "storage/file_download.h"
#include "storage/localimageloader.h"
#include "ui/abstract_button.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtGui/QCursor>
#include <QtGui/QPainterPath>

#include <atomic>
#include <deque>
#include <numbers>

namespace Oblivion {
namespace {

constexpr auto kMaxTracks = 10;
constexpr auto kMaxTrackDuration = 20 * 60 * crl::time(1000);
constexpr auto kMaxTotalDuration = 60 * 60 * crl::time(1000);
constexpr auto kMaxDocumentSize = int64(512) * 1024 * 1024;
constexpr auto kMaxRate = 48000;
constexpr auto kMinSelection = crl::time(500);
constexpr auto kPeaksCount = 1000;
constexpr auto kRenderDelay = crl::time(300);
constexpr auto kJoinDelay = crl::time(250);
constexpr auto kCeiling = 0.97f;
constexpr auto kLoudnessTarget = -14.;
constexpr auto kFadeIn = crl::time(2000);
constexpr auto kFadeOut = crl::time(4000);
constexpr auto kLimiterLookahead = 5; // Milliseconds.
constexpr auto kLimiterRelease = 80; // Milliseconds.
constexpr auto kMaxCrossfadeHalfSeconds = 20;
constexpr auto kMinSpeed = 50;
constexpr auto kMaxSpeed = 200;
constexpr auto kMaxPitch = 12;
constexpr auto kMaxGain = 24; // Half decibels.
constexpr auto kMaxTitleLength = 128;
constexpr auto kTempMaxAge = 6 * 3600; // Seconds.
constexpr auto kBoxWidth = 460;
constexpr auto kWaveformHeight = 80;
constexpr auto kHandleWidth = 10;
constexpr auto kBarWidth = 2;
constexpr auto kBarGap = 1;
constexpr auto kPlaySize = 36;
constexpr auto kTrackPadding = 8;
constexpr auto kChipPadding = 12;
constexpr auto kChipVertical = 7;
constexpr auto kChipSkip = 8;

enum class Preset : uchar {
	Original,
	Slowed,
	SpedUp,
	Nightcore,
	Custom,
};

enum class SpeedMode : uchar {
	Vinyl,
	Tempo,
};

struct Effects {
	int speed = 100; // Percent.
	bool keepPitch = false;
	int pitch = 0; // Semitones.
	int reverb = 0; // Percent.
	int gain = 0; // Half decibels.
	bool normalize = false;
	bool fadeIn = false;
	bool fadeOut = false;
	bool denoise = false; // RNNoise, for speech (see oblivion_noise.h).

	friend inline bool operator==(const Effects &, const Effects &) = default;
};

struct Params {
	uint64 source = 0;
	crl::time from = 0;
	crl::time till = 0;
	Effects effects;

	friend inline bool operator==(const Params &, const Params &) = default;
};

[[nodiscard]] Effects ApplyPreset(Effects effects, Preset preset) {
	const auto set = [&](int speed, int reverb) {
		effects.speed = speed;
		effects.keepPitch = false;
		effects.pitch = 0;
		effects.reverb = reverb;
	};
	switch (preset) {
	case Preset::Original: set(100, 0); break;
	case Preset::Slowed: set(85, 75); break;
	case Preset::SpedUp: set(125, 0); break;
	case Preset::Nightcore: set(135, 0); break;
	case Preset::Custom: break;
	}
	return effects;
}

[[nodiscard]] Preset DetectPreset(const Effects &effects) {
	for (const auto preset : {
			Preset::Original,
			Preset::Slowed,
			Preset::SpedUp,
			Preset::Nightcore }) {
		if (ApplyPreset(effects, preset) == effects) {
			return preset;
		}
	}
	return Preset::Custom;
}

[[nodiscard]] float64 SpeedFactor(const Effects &effects) {
	return std::max(effects.speed, 1) / 100.;
}

// Processed (rendered) time -> time in the joined source.
[[nodiscard]] crl::time ToSource(const Params &params, crl::time position) {
	const auto shift = crl::time(std::round(
		std::max(position, crl::time(0)) * SpeedFactor(params.effects)));
	return std::clamp(params.from + shift, params.from, params.till);
}

// Time in the joined source -> processed (rendered) time.
[[nodiscard]] crl::time ToProcessed(const Params &params, crl::time position) {
	const auto clamped = std::clamp(position, params.from, params.till);
	return crl::time(std::round(
		(clamped - params.from) / SpeedFactor(params.effects)));
}

[[nodiscard]] crl::time ExpectedDuration(const Params &params) {
	return crl::time(std::round(
		(params.till - params.from) / SpeedFactor(params.effects)));
}

[[nodiscard]] Audio::ReverbParams ReverbFor(int percent) {
	const auto amount = std::clamp(percent, 0, 100) / 100.;
	return {
		.roomSize = 0.55 + 0.35 * amount,
		.damping = 0.5 - 0.1 * amount,
		.wet = 0.6 * amount,
		.dry = 0.8,
	};
}

[[nodiscard]] QString FormatDuration(crl::time ms) {
	const auto seconds = std::max(ms, crl::time(0)) / 1000;
	if (seconds >= 3600) {
		return u"%1:%2:%3"_q
			.arg(seconds / 3600)
			.arg((seconds / 60) % 60, 2, 10, QChar('0'))
			.arg(seconds % 60, 2, 10, QChar('0'));
	}
	return u"%1:%2"_q
		.arg(seconds / 60)
		.arg(seconds % 60, 2, 10, QChar('0'));
}

[[nodiscard]] QString FormatPrecise(crl::time ms) {
	const auto tenths = std::max(ms, crl::time(0)) / 100;
	const auto seconds = tenths / 10;
	return u"%1:%2%3%4"_q
		.arg(seconds / 60)
		.arg(seconds % 60, 2, 10, QChar('0'))
		.arg(QLocale().decimalPoint())
		.arg(tenths % 10);
}

[[nodiscard]] QString FormatPercent(float64 progress) {
	return QString::number(int(std::round(
		std::clamp(progress, 0., 1.) * 100))) + '%';
}

[[nodiscard]] QString FormatSigned(float64 value, int precision) {
	const auto text = QLocale().toString(std::abs(value), 'f', precision);
	return (value > 0.)
		? (u"+"_q + text)
		: (value < 0.)
		? (QString(QChar(0x2212)) + text)
		: text;
}

[[nodiscard]] QString FormatSpeed(int percent) {
	return QLocale().toString(percent / 100., 'f', 2) + QChar(0x00D7);
}

[[nodiscard]] QString FormatRate(int rate) {
	return QLocale().toString(rate / 1000., 'f', (rate % 1000) ? 1 : 0);
}

[[nodiscard]] QString EffectsTag(const Effects &effects) {
	if (DetectPreset(effects) == Preset::Nightcore) {
		return tr::lng_oblivion_music_tag_nightcore(tr::now);
	}
	auto parts = QStringList();
	if (effects.speed < 100) {
		parts.push_back(tr::lng_oblivion_music_tag_slowed(tr::now));
	} else if (effects.speed > 100) {
		parts.push_back(tr::lng_oblivion_music_tag_sped_up(tr::now));
	}
	if (effects.pitch) {
		parts.push_back(tr::lng_oblivion_music_tag_pitch(
			tr::now,
			lt_value,
			FormatSigned(effects.pitch, 0)));
	}
	if (effects.reverb > 0) {
		parts.push_back(tr::lng_oblivion_music_tag_reverb(tr::now));
	}
	return parts.join(u" + "_q);
}

[[nodiscard]] QString SafeFileName(QString name) {
	static const auto forbidden = u"/\\:*?\"<>|"_q;
	for (auto &ch : name) {
		if (ch.unicode() < 32 || forbidden.contains(ch)) {
			ch = QChar('_');
		}
	}
	name = name.trimmed();
	while (name.startsWith('.')) {
		name.remove(0, 1);
	}
	if (name.size() > 120) {
		name = name.left(120).trimmed();
	}

	// Windows: names like "aux" or "con.mp3" are devices, they get a "_"
	// prefix (as upstream does for downloads). Nothing to do elsewhere.
	return base::Platform::FileNameFromUserString(std::move(name));
}

[[nodiscard]] QString ExportFileName(
		const QString &title,
		const QString &performer,
		Audio::Format format) {
	auto base = SafeFileName(performer.isEmpty()
		? title
		: (performer + u" - "_q + title));
	if (base.isEmpty()) {
		base = SafeFileName(tr::lng_oblivion_music_default_name(tr::now));
	}
	return base + '.' + Audio::FormatExtension(format);
}

[[nodiscard]] QString FormatFilterName(Audio::Format format) {
	switch (format) {
	case Audio::Format::OggOpus:
		return tr::lng_oblivion_music_file_ogg(tr::now);
	case Audio::Format::Wav:
		return tr::lng_oblivion_music_file_wav(tr::now);
	case Audio::Format::M4a:
		break;
	}
	return tr::lng_oblivion_music_file_m4a(tr::now);
}

[[nodiscard]] QString FileFilter(const QString &name, const QString &mask) {
	return name + u" ("_q + mask + u")"_q;
}

[[nodiscard]] QString SuggestedPath(const QString &fileName) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(fileName, QString());
}

[[nodiscard]] QString TempRoot() {
	return QDir::tempPath() + u"/oblivion_music_editor"_q;
}

// Removes the old files downloaded or prepared for sending by the editor.
// Called once per launch before the editor creates any file, so it never
// touches a file this launch still uploads or reads.
void CleanupTemp() {
	const auto root = QDir(TempRoot());
	if (!root.exists()) {
		return;
	}
	const auto now = QDateTime::currentDateTime();
	const auto list = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
	for (const auto &info : list) {
		if (info.lastModified().secsTo(now) > kTempMaxAge) {
			QDir(info.absoluteFilePath()).removeRecursively();
		}
	}
}

// In a new folder, so that the file keeps the given name.
[[nodiscard]] QString TempFilePath(const QString &fileName) {
	return TempRoot()
		+ '/'
		+ QString::number(base::RandomValue<uint64>(), 16)
		+ '/'
		+ fileName;
}

// Creates the folder if needed, removes a partially written file.
[[nodiscard]] bool WriteToFile(
		const QString &path,
		const QByteArray &bytes) {
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	} else if (file.write(bytes) != bytes.size()) {
		file.close();
		file.remove();
		return false;
	}
	file.close();
	return true;
}

// Brick-wall peak limiter: 5 ms look-ahead, 80 ms release.
[[nodiscard]] Audio::Pcm Limit(Audio::Pcm pcm, float ceiling) {
	if (pcm.empty() || Audio::Peak(pcm) <= ceiling) {
		return pcm;
	}
	const auto channels = pcm.channels;
	const auto frames = pcm.frames();
	const auto window = std::max(
		int64(pcm.rate) * kLimiterLookahead / 1000,
		int64(1));
	const auto data = pcm.samples.data();

	// Gain that brings the frame down to the ceiling. Frames ahead of
	// the current one are not modified yet, so it reads the source.
	const auto need = [&](int64 frame) {
		auto peak = 0.f;
		for (auto c = 0; c != channels; ++c) {
			peak = std::max(peak, std::abs(data[frame * channels + c]));
		}
		return (peak > ceiling) ? (ceiling / peak) : 1.f;
	};

	// Sliding minimum of the required gain over [f, f + window),
	// the values increase from the front to the back.
	auto queue = std::deque<std::pair<int64, float>>();
	const auto push = [&](int64 frame) {
		const auto value = need(frame);
		while (!queue.empty() && queue.back().second >= value) {
			queue.pop_back();
		}
		queue.emplace_back(frame, value);
	};
	auto head = int64(0);
	for (; head < std::min(window - 1, frames); ++head) {
		push(head);
	}

	// Box smoothing over (f - window, f] ramps the gain down before
	// every peak, then the gain recovers exponentially.
	const auto release = float(std::exp(
		-1. / (kLimiterRelease / 1000. * pcm.rate)));
	auto ring = std::vector<float>(window, 1.f);
	auto sum = 0.;
	auto gain = 1.f;
	for (auto f = int64(); f != frames; ++f) {
		if (head < frames) {
			push(head++);
		}
		while (queue.front().first < f) {
			queue.pop_front();
		}
		const auto minimum = queue.front().second;
		auto &slot = ring[f % window];
		if (f >= window) {
			sum -= slot;
		}
		slot = minimum;
		sum += minimum;
		const auto target = float(sum / std::min(f + 1, window));
		gain = (target < gain)
			? target
			: (target + (gain - target) * release);
		for (auto c = 0; c != channels; ++c) {
			auto &sample = data[f * channels + c];
			sample = std::clamp(sample * gain, -ceiling, ceiling);
		}
	}
	return pcm;
}

// RMS per bin, for the waveform.
[[nodiscard]] std::vector<float> ComputePeaks(
		const Audio::Pcm &pcm,
		int count) {
	const auto frames = pcm.frames();
	if (frames <= 0 || count <= 0) {
		return {};
	}
	auto sums = std::vector<double>(count, 0.);
	auto counts = std::vector<int64>(count, 0);
	const auto channels = pcm.channels;
	const auto data = pcm.samples.data();
	for (auto f = int64(); f != frames; ++f) {
		const auto bin = int(f * count / frames);
		auto value = 0.;
		for (auto c = 0; c != channels; ++c) {
			const auto sample = double(data[f * channels + c]);
			value += sample * sample;
		}
		sums[bin] += value / channels;
		++counts[bin];
	}
	auto result = std::vector<float>(count, 0.f);
	for (auto i = 0; i != count; ++i) {
		if (counts[i]) {
			result[i] = float(std::sqrt(sums[i] / counts[i]));
		}
	}
	return result;
}

// Trim -> noise suppression -> speed / tempo -> pitch -> fades -> reverb
// -> loudness -> gain -> limiter. The noise goes first, while the sound
// is still as recorded. The fades go before the reverb, so they fade the
// music itself and the reverb tail decays from the faded sound.
// Returns an empty Pcm on failure or when stale() says so.
[[nodiscard]] Audio::Pcm RenderPcm(
		const Audio::Pcm &source,
		const Params &params,
		const Fn<bool()> &stale) {
	const auto &effects = params.effects;
	auto pcm = Audio::Trim(source, params.from, params.till);
	if (pcm.empty() || stale()) {
		return {};
	}
	if (effects.denoise) {
		pcm = Noise::Denoise(pcm, 1., stale);
		if (pcm.empty() || stale()) {
			return {};
		}
	}
	if (effects.speed != 100) {
		const auto factor = SpeedFactor(effects);
		pcm = effects.keepPitch
			? Audio::ChangeTempo(pcm, factor)
			: Audio::ChangeSpeed(pcm, factor);
		if (pcm.empty() || stale()) {
			return {};
		}
	}
	if (effects.pitch) {
		pcm = Audio::ShiftPitch(pcm, effects.pitch);
		if (pcm.empty() || stale()) {
			return {};
		}
	}
	if (effects.fadeIn || effects.fadeOut) {
		const auto duration = pcm.duration();
		pcm = Audio::Fade(
			pcm,
			effects.fadeIn ? std::min(kFadeIn, duration / 4) : 0,
			effects.fadeOut ? std::min(kFadeOut, duration / 3) : 0);
		if (pcm.empty() || stale()) {
			return {};
		}
	}
	if (effects.reverb > 0) {
		pcm = Audio::Reverb(pcm, ReverbFor(effects.reverb));
		if (pcm.empty() || stale()) {
			return {};
		}
	}
	if (effects.normalize) {
		pcm = Audio::NormalizeLoudness(pcm, kLoudnessTarget, kCeiling);
	}
	if (effects.gain) {
		pcm = Audio::Gain(pcm, effects.gain / 2.);
	}
	if (pcm.empty() || stale()) {
		return {};
	}
	return Limit(std::move(pcm), kCeiling);
}

struct Decoded {
	std::shared_ptr<const Audio::Pcm> pcm;
	QString title;
	QString performer;
	QImage cover;
	bool truncated = false;
};

[[nodiscard]] Decoded DecodeTrack(
		const QString &path,
		const QByteArray &bytes) {
	auto result = Decoded();
	auto information = Media::Player::PrepareForSending(path, bytes);
	using Song = Ui::PreparedFileInformation::Song;
	if (const auto song = std::get_if<Song>(&information.media)) {
		result.title = song->title.trimmed();
		result.performer = song->performer.trimmed();
		result.cover = std::move(song->cover);
	}
	const auto options = Audio::DecodeOptions{
		.maxDuration = kMaxTrackDuration,
	};
	auto decoded = path.isEmpty()
		? Audio::Decode(bytes, options)
		: Audio::Decode(path, options);
	if (!decoded || decoded->empty()) {
		return result;
	}
	auto pcm = std::move(*decoded);
	if (pcm.rate > kMaxRate) {
		auto resampled = Audio::Resample(pcm, kMaxRate);
		if (!resampled.empty()) {
			pcm = std::move(resampled);
		}
	}
	result.truncated = (pcm.duration() >= kMaxTrackDuration - 100);
	result.pcm = std::make_shared<const Audio::Pcm>(std::move(pcm));
	return result;
}

[[nodiscard]] bool IsEditableAudio(not_null<DocumentData*> document) {
	return document->isAudioFile() || document->isVoiceMessage();
}

[[nodiscard]] bool CheckCanSendMusic(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<Data::Thread*> thread) {
	const auto peer = thread->peer();
	const auto restriction = Data::RestrictionError(
		peer,
		ChatRestriction::SendMusic);
	if (restriction) {
		Data::ShowSendErrorToast(show, peer, restriction);
		return false;
	} else if (!Data::CanSend(thread, ChatRestriction::SendMusic)) {
		show->showToast(tr::lng_oblivion_music_restricted(tr::now));
		return false;
	}
	const auto error = GetErrorForSending(
		thread,
		{ .messagesCount = 1 });
	if (error) {
		Data::ShowSendErrorToast(show, peer, error);
		return false;
	}
	return true;
}

// Side paddings of all the content rows: the same as the subsection titles,
// the divider texts and the settings buttons have, so everything shares
// one left and one right edge.
[[nodiscard]] style::margins RowPadding() {
	const auto skip = st::defaultSubsectionTitlePadding.left();
	return { skip, 0, skip, 0 };
}

// The space between the last row of a section and the divider below it,
// it matches the space above a subsection title.
[[nodiscard]] int SectionEndSkip() {
	return st::boxLittleSkip + st::defaultVerticalListSkip;
}

// Slider with a title on the left and the current value on the right.
// Returns a setter that moves the slider without calling changed().
Fn<void(int)> AddValueSlider(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> title,
		int minimum,
		int maximum,
		int step,
		int current,
		Fn<QString(int)> format,
		Fn<void(int)> changed) {
	const auto header = container->add(
		object_ptr<Ui::RpWidget>(container),
		RowPadding() + QMargins(0, st::boxLittleSkip, 0, 0));
	const auto name = Ui::CreateChild<Ui::FlatLabel>(
		header,
		std::move(title),
		st::defaultFlatLabel);
	const auto value = Ui::CreateChild<Ui::FlatLabel>(
		header,
		format(current),
		st::settingsScaleLabel);
	rpl::combine(
		header->widthValue(),
		name->sizeValue(),
		value->sizeValue()
	) | rpl::on_next([=](int width, QSize nameSize, QSize valueSize) {
		const auto height = std::max(nameSize.height(), valueSize.height());
		if (header->height() != height) {
			header->resize(width, height);
		}
		name->moveToLeft(0, (height - nameSize.height()) / 2, width);
		value->moveToRight(0, (height - valueSize.height()) / 2, width);
	}, header->lifetime());

	const auto slider = container->add(
		object_ptr<Ui::MediaSliderWheelless>(container, st::settingsScale),
		RowPadding() + QMargins(0, st::boxLittleSkip / 2, 0, 0));
	slider->resize(slider->width(), st::settingsScale.seekSize.height());

	const auto sections = std::max((maximum - minimum) / step, 1);
	const auto toValue = [=](float64 position) {
		const auto index = int(std::round(
			std::clamp(position, 0., 1.) * sections));
		return minimum + index * step;
	};
	const auto toPosition = [=](int now) {
		return std::clamp(
			(now - minimum) / float64(sections * step),
			0.,
			1.);
	};
	const auto last = slider->lifetime().make_state<int>(current);
	slider->setAlwaysDisplayMarker(true);
	slider->setValue(toPosition(current));
	slider->setAdjustCallback([=](float64 position) {
		return toPosition(toValue(position));
	});
	const auto update = [=](float64 position) {
		const auto now = toValue(position);
		value->setText(format(now));
		if (*last != now) {
			*last = now;
			changed(now);
		}
	};
	slider->setChangeProgressCallback(update);
	slider->setChangeFinishedCallback(update);
	return [=](int now) {
		*last = now;
		slider->setValue(toPosition(now));
		value->setText(format(now));
	};
}

not_null<Ui::Checkbox*> AddCheckbox(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		bool checked) {
	return container->add(
		object_ptr<Ui::Checkbox>(
			container,
			text,
			checked,
			st::defaultBoxCheckbox),
		RowPadding() + QMargins(0, st::boxLittleSkip, 0, 0));
}

class TrackList final : public Ui::RpWidget {
public:
	struct Row {
		int number = 0;
		QString title;
		QString subtitle;
		QString right;
		bool error = false;
	};

	explicit TrackList(QWidget *parent);

	void setRows(std::vector<Row> rows);
	[[nodiscard]] rpl::producer<int> clicks() const;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] int rowHeight() const;
	[[nodiscard]] int rowAt(QPoint point) const;
	void setOver(int index);

	std::vector<Row> _rows;
	int _over = -1;
	int _pressed = -1;
	rpl::event_stream<int> _clicks;

};

TrackList::TrackList(QWidget *parent)
: RpWidget(parent) {
	setMouseTracking(true);
}

void TrackList::setRows(std::vector<Row> rows) {
	_rows = std::move(rows);
	if (_over >= int(_rows.size())) {
		setOver(-1);
	}
	_pressed = -1;
	resizeToWidth(width());
	update();
}

rpl::producer<int> TrackList::clicks() const {
	return _clicks.events();
}

int TrackList::rowHeight() const {
	const auto padding = style::ConvertScale(kTrackPadding);
	return padding
		+ st::semiboldFont->height
		+ style::ConvertScale(2)
		+ st::normalFont->height
		+ padding;
}

int TrackList::resizeGetHeight(int newWidth) {
	return int(_rows.size()) * rowHeight();
}

int TrackList::rowAt(QPoint point) const {
	if (point.x() < 0 || point.x() >= width() || point.y() < 0) {
		return -1;
	}
	const auto index = point.y() / rowHeight();
	return (index < int(_rows.size())) ? index : -1;
}

void TrackList::setOver(int index) {
	if (_over == index) {
		return;
	}
	_over = index;
	setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
	update();
}

void TrackList::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto height = rowHeight();
	const auto padding = style::ConvertScale(kTrackPadding);

	// The number badge is a circle of the same size and in the same place
	// as the round icon of the "Add track" button below, so they form one
	// column, the texts start where its text does.
	const auto badge = st::settingsIconAdd.width();
	const auto badgeLeft = st::settingsButton.iconLeft;
	const auto left = st::settingsButton.padding.left();
	const auto right = RowPadding().right();
	const auto outer = width();
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto &row = _rows[i];
		const auto top = i * height;
		if (i == _over) {
			p.fillRect(0, top, outer, height, st::windowBgOver);
		}
		{
			auto hq = PainterHighQualityEnabler(p);
			const auto rect = QRect(
				badgeLeft,
				top + (height - badge) / 2,
				badge,
				badge);
			p.setPen(Qt::NoPen);
			p.setBrush(row.error
				? st::attentionButtonBgOver
				: st::lightButtonBgOver);
			p.drawEllipse(rect);
			p.setFont(st::semiboldFont);
			p.setPen(row.error ? st::boxTextFgError : st::lightButtonFg);
			p.drawText(rect, Qt::AlignCenter, QString::number(row.number));
		}
		auto available = outer - left - right;
		if (!row.right.isEmpty()) {
			const auto width = st::normalFont->width(row.right);
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(
				outer - right - width,
				top + padding + (st::semiboldFont->height
					- st::normalFont->height) / 2,
				outer,
				row.right);
			available -= width + st::normalFont->spacew * 2;
		}
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawTextLeft(
			left,
			top + padding,
			outer,
			st::semiboldFont->elided(row.title, std::max(available, 0)));
		p.setFont(st::normalFont);
		p.setPen(row.error ? st::boxTextFgError : st::windowSubTextFg);
		p.drawTextLeft(
			left,
			top + padding + st::semiboldFont->height + style::ConvertScale(2),
			outer,
			st::normalFont->elided(
				row.subtitle,
				std::max(outer - left - right, 0)));
	}
}

void TrackList::mouseMoveEvent(QMouseEvent *e) {
	setOver(rowAt(e->pos()));
}

void TrackList::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton || e->button() == Qt::RightButton) {
		_pressed = rowAt(e->pos());
	}
}

void TrackList::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed == rowAt(e->pos())) {
		_clicks.fire_copy(pressed);
	}
}

void TrackList::leaveEventHook(QEvent *e) {
	setOver(-1);
}

class PresetChips final : public Ui::RpWidget {
public:
	struct Chip {
		Preset preset = Preset::Original;
		QString text;
	};

	PresetChips(QWidget *parent, std::vector<Chip> chips);

	void setSelected(Preset preset);
	[[nodiscard]] rpl::producer<Preset> chosen() const;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] int chipAt(QPoint point) const;
	void setOver(int index);

	std::vector<Chip> _chips;
	std::vector<QRect> _rects;
	Preset _selected = Preset::Original;
	int _over = -1;
	int _pressed = -1;
	rpl::event_stream<Preset> _chosen;

};

PresetChips::PresetChips(QWidget *parent, std::vector<Chip> chips)
: RpWidget(parent)
, _chips(std::move(chips)) {
	setMouseTracking(true);
}

void PresetChips::setSelected(Preset preset) {
	if (_selected != preset) {
		_selected = preset;
		update();
	}
}

rpl::producer<Preset> PresetChips::chosen() const {
	return _chosen.events();
}

int PresetChips::resizeGetHeight(int newWidth) {
	const auto &font = st::normalFont;
	const auto padding = style::ConvertScale(kChipPadding);
	const auto height = font->height + 2 * style::ConvertScale(kChipVertical);
	const auto skip = style::ConvertScale(kChipSkip);
	const auto count = int(_chips.size());
	_rects.clear();
	if (!count || newWidth <= 0) {
		return 0;
	}

	// Equal chips filling the whole width, in the fewest rows where the
	// widest text fits, the rows filled evenly (4 chips: 4, 2 + 2, 1 x 4).
	auto widest = 0;
	for (const auto &chip : _chips) {
		widest = std::max(widest, font->width(chip.text) + 2 * padding);
	}
	auto columns = count;
	while (columns > 1
		&& (newWidth - (columns - 1) * skip) / columns < widest) {
		--columns;
	}
	const auto rows = (count + columns - 1) / columns;
	columns = (count + rows - 1) / rows;
	const auto edge = [&](int column) {
		return column * (newWidth + skip) / columns;
	};
	for (auto i = 0; i != count; ++i) {
		const auto column = i % columns;
		const auto left = edge(column);
		_rects.emplace_back(
			left,
			(i / columns) * (height + skip),
			edge(column + 1) - skip - left,
			height);
	}
	return rows * height + (rows - 1) * skip;
}

void PresetChips::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto padding = style::ConvertScale(kChipPadding);
	p.setFont(st::normalFont);
	const auto count = std::min(_chips.size(), _rects.size());
	for (auto i = 0; i != int(count); ++i) {
		const auto &rect = _rects[i];
		const auto selected = (_chips[i].preset == _selected);
		const auto radius = rect.height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(selected
			? st::activeButtonBg
			: (i == _over)
			? st::windowBgRipple
			: st::windowBgOver);
		p.drawRoundedRect(rect, radius, radius);
		p.setPen(selected ? st::activeButtonFg : st::windowFg);
		p.drawText(
			rect,
			Qt::AlignCenter,
			st::normalFont->elided(
				_chips[i].text,
				std::max(rect.width() - 2 * padding, 0)));
	}
}

int PresetChips::chipAt(QPoint point) const {
	for (auto i = 0; i != int(_rects.size()); ++i) {
		if (_rects[i].contains(point)) {
			return i;
		}
	}
	return -1;
}

void PresetChips::setOver(int index) {
	if (_over == index) {
		return;
	}
	_over = index;
	setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
	update();
}

void PresetChips::mouseMoveEvent(QMouseEvent *e) {
	setOver(chipAt(e->pos()));
}

void PresetChips::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = chipAt(e->pos());
	}
}

void PresetChips::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (e->button() == Qt::LeftButton
		&& pressed >= 0
		&& pressed == chipAt(e->pos())
		&& pressed < int(_chips.size())) {
		_chosen.fire_copy(_chips[pressed].preset);
	}
}

void PresetChips::leaveEventHook(QEvent *e) {
	setOver(-1);
}

class PlayButton final : public Ui::AbstractButton {
public:
	explicit PlayButton(QWidget *parent);

	void setPlaying(bool playing);

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	bool _playing = false;

};

PlayButton::PlayButton(QWidget *parent)
: AbstractButton(parent) {
	const auto size = style::ConvertScale(kPlaySize);
	resize(size, size);
}

void PlayButton::setPlaying(bool playing) {
	if (_playing != playing) {
		_playing = playing;
		update();
	}
}

void PlayButton::onStateChanged(State was, StateChangeSource source) {
	update();
}

void PlayButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	if (isDisabled()) {
		p.setOpacity(0.5);
	}
	p.setPen(Qt::NoPen);
	p.setBrush((isOver() && !isDisabled())
		? st::activeButtonBgOver
		: st::activeButtonBg);
	p.drawEllipse(rect());
	p.setBrush(st::activeButtonFg);
	const auto side = float64(width());
	if (_playing) {
		const auto bar = side * 0.12;
		const auto height = side * 0.4;
		const auto gap = side * 0.1;
		const auto top = (side - height) / 2.;
		const auto radius = bar / 3.;
		p.drawRoundedRect(
			QRectF(side / 2. - gap / 2. - bar, top, bar, height),
			radius,
			radius);
		p.drawRoundedRect(
			QRectF(side / 2. + gap / 2., top, bar, height),
			radius,
			radius);
	} else {
		const auto height = side * 0.42;
		const auto width = height * 0.87;
		const auto left = (side - width) / 2. + side * 0.04;
		const auto top = (side - height) / 2.;
		auto path = QPainterPath();
		path.moveTo(left, top);
		path.lineTo(left + width, side / 2.);
		path.lineTo(left, top + height);
		path.closeSubpath();
		p.drawPath(path);
	}
}

class WaveformEditor final : public Ui::RpWidget {
public:
	struct Range {
		crl::time from = 0;
		crl::time till = 0;
	};

	explicit WaveformEditor(QWidget *parent);

	void setStatus(const QString &status);
	void setData(std::vector<float> peaks, crl::time duration);
	void setRange(crl::time from, crl::time till);
	void setPlayhead(std::optional<crl::time> position);

	[[nodiscard]] rpl::producer<Range> rangeChanges() const;
	[[nodiscard]] rpl::producer<crl::time> seekRequests() const;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	enum class Drag : uchar {
		None,
		Start,
		End,
		Seek,
	};

	[[nodiscard]] int handle() const;
	[[nodiscard]] QRect area() const;
	[[nodiscard]] int xFor(crl::time time) const;
	[[nodiscard]] crl::time timeFor(int x) const;
	[[nodiscard]] Drag dragAt(int x) const;
	void updateCursor(int x);
	void dragTo(int x);

	std::vector<float> _peaks;
	float _maxPeak = 0.f;
	crl::time _duration = 0;
	crl::time _from = 0;
	crl::time _till = 0;
	std::optional<crl::time> _playhead;
	std::optional<crl::time> _seekPreview;
	QString _status;
	Drag _drag = Drag::None;
	int _grab = 0;
	rpl::event_stream<Range> _rangeChanges;
	rpl::event_stream<crl::time> _seekRequests;

};

WaveformEditor::WaveformEditor(QWidget *parent)
: RpWidget(parent) {
	setMouseTracking(true);
	resize(width(), style::ConvertScale(kWaveformHeight));
}

void WaveformEditor::setStatus(const QString &status) {
	if (_status != status) {
		_status = status;
		update();
	}
}

void WaveformEditor::setData(std::vector<float> peaks, crl::time duration) {
	_peaks = std::move(peaks);
	_duration = std::max(duration, crl::time(0));
	_maxPeak = 0.f;
	for (const auto peak : _peaks) {
		_maxPeak = std::max(_maxPeak, peak);
	}
	_drag = Drag::None;
	_seekPreview = std::nullopt;
	updateCursor(mapFromGlobal(QCursor::pos()).x());
	update();
}

void WaveformEditor::setRange(crl::time from, crl::time till) {
	_from = from;
	_till = till;
	update();
}

void WaveformEditor::setPlayhead(std::optional<crl::time> position) {
	if (_playhead != position) {
		_playhead = position;
		update();
	}
}

auto WaveformEditor::rangeChanges() const -> rpl::producer<Range> {
	return _rangeChanges.events();
}

rpl::producer<crl::time> WaveformEditor::seekRequests() const {
	return _seekRequests.events();
}

int WaveformEditor::resizeGetHeight(int newWidth) {
	return style::ConvertScale(kWaveformHeight);
}

int WaveformEditor::handle() const {
	return style::ConvertScale(kHandleWidth);
}

QRect WaveformEditor::area() const {
	return QRect(handle(), 0, std::max(width() - 2 * handle(), 1), height());
}

int WaveformEditor::xFor(crl::time time) const {
	const auto inner = area();
	return inner.x() + ((_duration > 0)
		? int(std::round(inner.width() * float64(time) / _duration))
		: 0);
}

crl::time WaveformEditor::timeFor(int x) const {
	if (_duration <= 0) {
		return 0;
	}
	const auto inner = area();
	const auto result = crl::time(std::round(
		(x - inner.x()) * float64(_duration) / inner.width()));
	return std::clamp(result, crl::time(0), _duration);
}

auto WaveformEditor::dragAt(int x) const -> Drag {
	if (_duration <= 0 || _peaks.empty()) {
		return Drag::None;
	}
	const auto left = xFor(_from);
	const auto right = xFor(_till);
	const auto size = handle();
	const auto slop = size / 2;
	const auto nearLeft = (x >= left - size - slop) && (x <= left + slop);
	const auto nearRight = (x >= right - slop)
		&& (x <= right + size + slop);
	if (nearLeft && nearRight) {
		return (std::abs(x - left) <= std::abs(x - right))
			? Drag::Start
			: Drag::End;
	} else if (nearLeft) {
		return Drag::Start;
	} else if (nearRight) {
		return Drag::End;
	} else if (x > left && x < right) {
		return Drag::Seek;
	}
	return Drag::None;
}

void WaveformEditor::updateCursor(int x) {
	const auto drag = (_drag != Drag::None) ? _drag : dragAt(x);
	setCursor((drag == Drag::Start || drag == Drag::End)
		? style::cur_sizehor
		: (drag == Drag::Seek)
		? style::cur_pointer
		: style::cur_default);
}

void WaveformEditor::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto x = e->pos().x();
	_drag = dragAt(x);
	switch (_drag) {
	case Drag::Start: _grab = x - xFor(_from); break;
	case Drag::End: _grab = x - xFor(_till); break;
	case Drag::Seek:
		_grab = 0;
		_seekPreview = std::clamp(timeFor(x), _from, _till);
		update();
		break;
	case Drag::None: break;
	}
	updateCursor(x);
}

void WaveformEditor::mouseMoveEvent(QMouseEvent *e) {
	if (_drag == Drag::None) {
		updateCursor(e->pos().x());
		return;
	}
	dragTo(e->pos().x());
}

void WaveformEditor::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || _drag == Drag::None) {
		return;
	}
	const auto x = e->pos().x();
	dragTo(x);
	const auto drag = std::exchange(_drag, Drag::None);
	const auto seek = std::exchange(_seekPreview, std::nullopt);
	updateCursor(x);
	update();
	if (drag == Drag::Seek && seek) {
		_seekRequests.fire_copy(*seek);
	}
}

void WaveformEditor::dragTo(int x) {
	switch (_drag) {
	case Drag::Seek:
		_seekPreview = std::clamp(timeFor(x), _from, _till);
		update();
		return;
	case Drag::Start:
	case Drag::End: {
		const auto minimum = std::min(kMinSelection, _duration);
		const auto at = timeFor(x - _grab);
		auto from = _from;
		auto till = _till;
		if (_drag == Drag::Start) {
			from = std::clamp(
				at,
				crl::time(0),
				std::max(_till - minimum, crl::time(0)));
		} else {
			till = std::clamp(
				at,
				std::min(_from + minimum, _duration),
				_duration);
		}
		if (from != _from || till != _till) {
			_from = from;
			_till = till;
			update();
			_rangeChanges.fire({ .from = _from, .till = _till });
		}
	} return;
	case Drag::None:
		return;
	}
}

void WaveformEditor::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	// The background takes the whole row, like the other rows, the bars
	// are inside the handles: with everything selected the frame lies
	// exactly over the background edges.
	const auto inner = area();
	const auto radius = style::ConvertScale(6);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgOver);
	p.drawRoundedRect(rect(), radius, radius);
	if (_peaks.empty() || _duration <= 0) {
		if (!_status.isEmpty()) {
			const auto margin = style::ConvertScale(16);
			p.setPen(st::windowSubTextFg);
			p.setFont(st::normalFont);
			p.drawText(
				rect().marginsRemoved({ margin, 0, margin, 0 }),
				Qt::AlignCenter | Qt::TextWordWrap,
				_status);
		}
		return;
	}

	const auto left = xFor(_from);
	const auto right = xFor(_till);
	const auto bar = std::max(style::ConvertScale(kBarWidth), 1);
	const auto gap = std::max(style::ConvertScale(kBarGap), 1);
	const auto step = bar + gap;
	const auto count = std::max((inner.width() - gap) / step, 1);
	const auto shift = inner.x() + (inner.width() - (count * step - gap)) / 2;
	const auto padding = style::ConvertScale(10);
	const auto available = std::max(inner.height() - 2 * padding, bar);
	const auto center = inner.y() + inner.height() / 2.;
	const auto bins = int64(_peaks.size());
	const auto maxPeak = std::max(_maxPeak, 0.0001f);
	for (auto i = 0; i != count; ++i) {
		const auto binFrom = int64(i) * bins / count;
		const auto binTill = std::max(int64(i + 1) * bins / count, binFrom + 1);
		auto value = 0.f;
		for (auto j = binFrom; j < binTill && j < bins; ++j) {
			value = std::max(value, _peaks[j]);
		}
		const auto height = std::max(
			float64(bar),
			available * float64(value / maxPeak));
		const auto x = shift + i * step;
		const auto inside = (x + bar > left) && (x < right);
		p.setOpacity(inside ? 1. : 0.5);
		p.setBrush(inside ? st::windowActiveTextFg : st::windowSubTextFg);
		p.drawRoundedRect(
			QRectF(x, center - height / 2., bar, height),
			bar / 2.,
			bar / 2.);
	}
	p.setOpacity(1.);

	const auto border = style::ConvertScale(2);
	const auto size = handle();
	auto frame = QPainterPath();
	frame.setFillRule(Qt::OddEvenFill);
	frame.addRoundedRect(
		QRectF(left - size, 0, right - left + 2 * size, height()),
		radius,
		radius);
	frame.addRect(QRectF(left, border, right - left, height() - 2 * border));
	p.fillPath(frame, st::activeButtonBg);

	auto pen = QPen(st::activeButtonFg);
	pen.setWidth(style::ConvertScale(2));
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	const auto top = height() * 0.38;
	const auto bottom = height() * 0.62;
	p.drawLine(
		QPointF(left - size / 2., top),
		QPointF(left - size / 2., bottom));
	p.drawLine(
		QPointF(right + size / 2., top),
		QPointF(right + size / 2., bottom));

	const auto playhead = _seekPreview ? _seekPreview : _playhead;
	if (playhead) {
		const auto x = xFor(std::clamp(*playhead, crl::time(0), _duration));
		const auto line = std::max(style::ConvertScale(2), 1);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowFg);
		p.drawRoundedRect(
			QRectF(x - line / 2., border, line, height() - 2 * border),
			line / 2.,
			line / 2.);
	}
}

enum class TrackStatus : uchar {
	Downloading,
	Decoding,
	Ready,
	Failed,
};

struct Track {
	uint64 id = 0;
	QString title;
	QString performer;
	QImage cover;
	std::shared_ptr<const Audio::Pcm> pcm;
	TrackStatus status = TrackStatus::Decoding;
	float64 progress = 0.;
	QString error;
	bool preferTags = true;
	bool truncated = false;
};

[[nodiscard]] QString TrackDisplayName(const Track &track) {
	const auto title = track.title.isEmpty()
		? tr::lng_oblivion_music_default_name(tr::now)
		: track.title;
	return track.performer.isEmpty()
		? title
		: (track.performer + QString::fromUtf8(" \xE2\x80\x93 ") + title);
}

[[nodiscard]] QString TrackInfo(const Audio::Pcm &pcm) {
	const auto rate = FormatRate(pcm.rate);
	return (pcm.channels == 1)
		? tr::lng_oblivion_music_info_mono(tr::now, lt_rate, rate)
		: tr::lng_oblivion_music_info_stereo(tr::now, lt_rate, rate);
}

struct Exported {
	QString path; // The written file.
	int64 size = 0;
	crl::time duration = 0;
	Audio::Format format = Audio::Format::M4a;
};

class Editor;

// Everything the editor needs from a session, see MakeSessionHooks().
// Empty in the UI snapshots: sending to a chat does nothing then.
struct SessionHooks {
	// Shows the chat chooser, chosen() returns false to keep it open.
	Fn<void(FnMut<bool(not_null<Data::Thread*>)>)> chooseChat;

	// Shows the reason and returns false if music can't be sent there.
	Fn<bool(not_null<Data::Thread*>)> canSend;

	// SendPaymentHelper::check() for one message, resend(starsApproved).
	Fn<bool(
		not_null<Data::Thread*>,
		Api::SendOptions,
		Fn<void(int)>)> checkPayment;
};

// An already decoded track.
struct ReadyTrack {
	QString title;
	QString performer;
	std::shared_ptr<const Audio::Pcm> pcm;
};

struct EditorArgs {
	std::shared_ptr<Ui::Show> show;
	SessionHooks session;

	// The audio to open, downloaded first if needed.
	DocumentData *document = nullptr;
	Data::FileOrigin origin;

	// Decoded tracks and an initial state (used by the UI snapshots).
	std::vector<ReadyTrack> tracks;
	Effects effects;
	crl::time crossfade = 0;
	std::optional<std::pair<crl::time, crl::time>> range;
	Fn<void(not_null<Editor*>)> created;
};

class Editor final : public base::has_weak_ptr {
public:
	Editor(not_null<Ui::GenericBox*> box, EditorArgs &&args);
	~Editor();

	void setup();
	void addDocument(
		not_null<DocumentData*> document,
		Data::FileOrigin origin);

	// Nothing is being opened, joined or processed.
	[[nodiscard]] bool idle() const;

private:
	void setupTracks(not_null<Ui::VerticalLayout*> container);
	void setupPreview(not_null<Ui::VerticalLayout*> container);
	void setupEffects(not_null<Ui::VerticalLayout*> container);
	void setupExport(not_null<Ui::VerticalLayout*> container);
	void setupPlayer();

	[[nodiscard]] Track *findTrack(uint64 id);
	[[nodiscard]] const Track *firstTrack() const;
	[[nodiscard]] bool canAddTrack();
	[[nodiscard]] uint64 addTrack(QString title, QString performer);
	void addPath(const QString &path);
	void addContent(const QByteArray &content);
	void decodeTrack(uint64 id, QString path, QByteArray bytes);
	void applyDecoded(uint64 id, Decoded decoded);
	void failTrack(uint64 id, const QString &error);
	void moveTrack(uint64 id, int delta);
	void removeTrack(uint64 id);
	void showTrackMenu(int index);
	void chooseFiles();
	void cancelDownload();
	void tracksChanged();

	void requestJoin(bool delayed);
	void startJoin();
	void applyJoined(
		uint64 request,
		std::shared_ptr<const Audio::Pcm> joined,
		std::vector<float> peaks);

	[[nodiscard]] Params currentParams() const;
	void effectsChanged();
	void applyPreset(Preset preset);
	void scheduleRender();
	void startRender();
	void applyRendered(
		Params params,
		std::shared_ptr<const Audio::Pcm> rendered);

	void togglePlay();
	void seekTo(crl::time source);

	void refreshTrackList();
	void refreshCrossfade(anim::type animated = anim::type::normal);
	void refreshWaveformStatus();
	void refreshStatus();
	void refreshLabels();
	void refreshPlayhead();
	void refreshAutoFields();

	[[nodiscard]] QString exportTitle() const;
	[[nodiscard]] QString exportPerformer() const;
	[[nodiscard]] QImage exportCover() const;
	[[nodiscard]] bool checkExportPossible();
	// Encodes (with the title / performer tags) and writes the file
	// to path off the main thread.
	void exportAudio(
		Audio::Format format,
		const QString &path,
		const QString &title,
		const QString &performer,
		Fn<void(Exported)> done);
	void save();
	void send();
	void sendTo(not_null<Data::Thread*> thread);
	void finishSend(
		not_null<Data::Thread*> thread,
		Exported exported,
		QString title,
		QString performer,
		QImage cover,
		int starsApproved);

	const not_null<Ui::GenericBox*> _box;
	const std::shared_ptr<Ui::Show> _show;
	const SessionHooks _session;
	const std::shared_ptr<std::atomic<bool>> _cancelled;
	const std::shared_ptr<std::atomic<uint64>> _latestRender;
	const std::shared_ptr<std::atomic<uint64>> _latestJoin;
	const std::unique_ptr<Audio::PreviewPlayer> _player;

	std::vector<Track> _tracks;
	std::vector<ReadyTrack> _initialTracks;
	std::optional<std::pair<crl::time, crl::time>> _initialRange;
	rpl::variable<bool> _hasTracks = false;
	uint64 _trackIdCounter = 0;
	crl::time _crossfade = 0;

	DocumentData *_document = nullptr;
	uint64 _documentTrackId = 0;
	std::shared_ptr<Data::DocumentMedia> _documentMedia;
	bool _ownDownload = false;
	rpl::lifetime _downloadLifetime;

	uint64 _joinRequest = 0;
	uint64 _joinedId = 0;
	std::shared_ptr<const Audio::Pcm> _joined;
	crl::time _duration = 0;
	bool _joining = false;
	bool _joinRunning = false;
	bool _joinPending = false;
	base::Timer _joinTimer;

	crl::time _from = 0;
	crl::time _till = 0;
	Effects _effects;

	std::shared_ptr<const Audio::Pcm> _rendered;
	Params _renderedParams;
	std::optional<Params> _renderingParams;
	uint64 _renderRequest = 0;
	base::Timer _renderTimer;
	bool _renderRunning = false;
	bool _renderFailed = false;
	bool _playWhenReady = false;
	std::optional<crl::time> _pendingSeek;

	Audio::Format _format = Audio::Format::M4a;
	bool _exporting = false;

	QString _autoTitle;
	QString _autoPerformer;

	TrackList *_trackList = nullptr;
	Ui::SlideWrap<Ui::VerticalLayout> *_crossfadeWrap = nullptr;
	WaveformEditor *_waveform = nullptr;
	PlayButton *_playButton = nullptr;
	Ui::FlatLabel *_timeLabel = nullptr;
	Ui::FlatLabel *_statusLabel = nullptr;
	Ui::FlatLabel *_rangeLabel = nullptr;
	PresetChips *_presets = nullptr;
	std::vector<Fn<void()>> _effectsSetters;
	Ui::InputField *_titleField = nullptr;
	Ui::InputField *_performerField = nullptr;
	base::unique_qptr<Ui::PopupMenu> _menu;

	rpl::lifetime _lifetime;

};

Editor::Editor(not_null<Ui::GenericBox*> box, EditorArgs &&args)
: _box(box)
, _show(std::move(args.show))
, _session(std::move(args.session))
, _cancelled(std::make_shared<std::atomic<bool>>(false))
, _latestRender(std::make_shared<std::atomic<uint64>>(0))
, _latestJoin(std::make_shared<std::atomic<uint64>>(0))
, _player(std::make_unique<Audio::PreviewPlayer>())
, _initialTracks(std::move(args.tracks))
, _initialRange(args.range)
, _crossfade(std::clamp(
	args.crossfade,
	crl::time(0),
	kMaxCrossfadeHalfSeconds * crl::time(500)))
, _joinTimer([=] { startJoin(); })
, _effects(args.effects)
, _renderTimer([=] { startRender(); }) {
}

Editor::~Editor() {
	*_cancelled = true;
}

bool Editor::idle() const {
	const auto opening = ranges::any_of(_tracks, [](const Track &track) {
		return (track.status == TrackStatus::Downloading)
			|| (track.status == TrackStatus::Decoding);
	});
	return !opening
		&& !_joining
		&& !_joinRunning
		&& !_joinTimer.isActive()
		&& !_renderRunning
		&& !_renderingParams
		&& !_renderTimer.isActive()
		&& !_exporting;
}

void Editor::setup() {
	const auto content = _box->verticalLayout();
	setupTracks(content);
	setupPreview(content);
	setupEffects(content);
	setupExport(content);
	setupPlayer();

	for (auto &ready : base::take(_initialTracks)) {
		if (!ready.pcm
			|| ready.pcm->empty()
			|| int(_tracks.size()) >= kMaxTracks) {
			continue;
		}
		const auto id = addTrack(
			std::move(ready.title),
			std::move(ready.performer));
		const auto track = findTrack(id);
		track->pcm = std::move(ready.pcm);
		track->status = TrackStatus::Ready;
	}

	_box->addButton(tr::lng_oblivion_music_send(), [=] { send(); });
	_box->addButton(tr::lng_oblivion_music_save(), [=] { save(); });
	_box->addTopButton(st::boxTitleClose, [=] { _box->closeBox(); });

	_box->boxClosing() | rpl::on_next([=] {
		*_cancelled = true;
		_joinTimer.cancel();
		_renderTimer.cancel();
		_player->stop();
		cancelDownload();
	}, _lifetime);

	refreshTrackList();
	refreshCrossfade(anim::type::instant);
	refreshLabels();
	refreshStatus();
	refreshAutoFields();
	if (!_tracks.empty()) {
		requestJoin(false);
	}
}

void Editor::setupTracks(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_oblivion_music_tracks());
	_trackList = container->add(object_ptr<TrackList>(container));
	_trackList->clicks() | rpl::on_next([=](int index) {
		showTrackMenu(index);
	}, _trackList->lifetime());

	const auto add = ::Settings::AddButtonWithIcon(
		container,
		rpl::conditional(
			_hasTracks.value(),
			tr::lng_oblivion_music_add_track(),
			tr::lng_oblivion_music_open_file()),
		st::settingsButtonActive,
		{
			&st::settingsIconAdd,
			::Settings::IconType::Round,
			&st::windowBgActive,
		});
	add->setClickedCallback([=] { chooseFiles(); });

	_crossfadeWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	AddValueSlider(
		_crossfadeWrap->entity(),
		tr::lng_oblivion_music_crossfade(),
		0,
		kMaxCrossfadeHalfSeconds,
		1,
		int(_crossfade / 500),
		[](int value) {
			return value
				? tr::lng_oblivion_music_seconds(
					tr::now,
					lt_value,
					QLocale().toString(value / 2., 'f', 1))
				: tr::lng_oblivion_music_off(tr::now);
		},
		[=](int value) {
			_crossfade = value * crl::time(500);
			requestJoin(true);
		});
	// The "Add track" button has its own bottom padding, the slider needs
	// the rest of the section end skip.
	Ui::AddSkip(
		_crossfadeWrap->entity(),
		SectionEndSkip() - st::defaultVerticalListSkip);
	_crossfadeWrap->hide(anim::type::instant);

	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::lng_oblivion_music_tracks_about());
}

void Editor::setupPreview(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_oblivion_music_fragment());
	_waveform = container->add(
		object_ptr<WaveformEditor>(container),
		RowPadding());
	_waveform->rangeChanges(
	) | rpl::on_next([=](WaveformEditor::Range range) {
		_from = range.from;
		_till = range.till;
		refreshLabels();
		scheduleRender();
	}, _waveform->lifetime());
	_waveform->seekRequests(
	) | rpl::on_next([=](crl::time position) {
		seekTo(position);
	}, _waveform->lifetime());

	// [>]  0:00 / 0:44                     Processing...
	//      Selected 0:06.0 - 0:42.5 (0:36.5)
	// The position and the selection are two lines next to the button,
	// the status is on the right of the first line, where it has room.
	const auto row = container->add(
		object_ptr<Ui::RpWidget>(container),
		RowPadding() + QMargins(0, st::boxLittleSkip, 0, 0));
	_playButton = Ui::CreateChild<PlayButton>(row);
	_playButton->setClickedCallback([=] { togglePlay(); });
	_timeLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		QString(),
		st::defaultFlatLabel);
	_rangeLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		QString(),
		st::defaultSubTextLabel);
	_statusLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		QString(),
		st::defaultSubTextLabel);
	const auto firstLine = st::defaultFlatLabel.style.font->height;
	const auto lines = firstLine
		+ st::defaultSubTextLabel.style.font->height;
	const auto height = std::max(_playButton->height(), lines);
	row->resize(row->width(), height);
	rpl::combine(
		row->widthValue(),
		_timeLabel->sizeValue(),
		_rangeLabel->sizeValue(),
		_statusLabel->sizeValue()
	) | rpl::on_next([=](int width, QSize, QSize, QSize) {
		const auto left = _playButton->width() + st::boxLittleSkip;
		// Until there is a selection the position is the only line, it is
		// centered by the button instead of hanging above its middle.
		const auto single = (_rangeLabel->textMaxWidth() <= 0);
		const auto top = (height - (single ? firstLine : lines)) / 2;
		_playButton->moveToLeft(
			0,
			(height - _playButton->height()) / 2,
			width);
		_timeLabel->moveToLeft(left, top, width);
		_rangeLabel->moveToLeft(left, top + firstLine, width);
		_statusLabel->moveToRight(0, top, width);
	}, row->lifetime());

	Ui::AddSkip(container, SectionEndSkip());
	Ui::AddDividerText(container, tr::lng_oblivion_music_trim_about());
}

void Editor::setupEffects(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_oblivion_music_effects());
	_presets = container->add(
		object_ptr<PresetChips>(
			container,
			std::vector<PresetChips::Chip>{
				{
					Preset::Original,
					tr::lng_oblivion_music_preset_original(tr::now),
				},
				{
					Preset::Slowed,
					tr::lng_oblivion_music_preset_slowed(tr::now),
				},
				{
					Preset::SpedUp,
					tr::lng_oblivion_music_preset_sped_up(tr::now),
				},
				{
					Preset::Nightcore,
					tr::lng_oblivion_music_preset_nightcore(tr::now),
				},
			}),
		RowPadding() + QMargins(0, 0, 0, st::boxLittleSkip));
	_presets->setSelected(DetectPreset(_effects));
	_presets->chosen() | rpl::on_next([=](Preset preset) {
		applyPreset(preset);
	}, _presets->lifetime());

	const auto speed = AddValueSlider(
		container,
		tr::lng_oblivion_music_speed(),
		kMinSpeed,
		kMaxSpeed,
		1,
		_effects.speed,
		FormatSpeed,
		[=](int value) {
			_effects.speed = value;
			effectsChanged();
		});
	_effectsSetters.push_back([=] { speed(_effects.speed); });

	const auto mode = std::make_shared<Ui::RadioenumGroup<SpeedMode>>(
		_effects.keepPitch ? SpeedMode::Tempo : SpeedMode::Vinyl);
	const auto addMode = [&](SpeedMode value, const QString &text) {
		container->add(
			object_ptr<Ui::Radioenum<SpeedMode>>(
				container,
				mode,
				value,
				text,
				st::defaultBoxCheckbox),
			RowPadding() + QMargins(0, st::boxLittleSkip, 0, 0));
	};
	addMode(SpeedMode::Vinyl, tr::lng_oblivion_music_mode_vinyl(tr::now));
	addMode(SpeedMode::Tempo, tr::lng_oblivion_music_mode_tempo(tr::now));
	mode->setChangedCallback([=](SpeedMode value) {
		const auto keep = (value == SpeedMode::Tempo);
		if (_effects.keepPitch != keep) {
			_effects.keepPitch = keep;
			effectsChanged();
		}
	});
	_effectsSetters.push_back([=] {
		mode->setValue(_effects.keepPitch
			? SpeedMode::Tempo
			: SpeedMode::Vinyl);
	});

	const auto pitch = AddValueSlider(
		container,
		tr::lng_oblivion_music_pitch(),
		-kMaxPitch,
		kMaxPitch,
		1,
		_effects.pitch,
		[](int value) { return FormatSigned(value, 0); },
		[=](int value) {
			_effects.pitch = value;
			effectsChanged();
		});
	_effectsSetters.push_back([=] { pitch(_effects.pitch); });

	const auto reverb = AddValueSlider(
		container,
		tr::lng_oblivion_music_reverb(),
		0,
		100,
		5,
		_effects.reverb,
		[](int value) { return QString::number(value) + '%'; },
		[=](int value) {
			_effects.reverb = value;
			effectsChanged();
		});
	_effectsSetters.push_back([=] { reverb(_effects.reverb); });

	AddValueSlider(
		container,
		tr::lng_oblivion_music_volume(),
		-kMaxGain,
		kMaxGain,
		1,
		_effects.gain,
		[](int value) { return FormatSigned(value / 2., (value % 2) ? 1 : 0); },
		[=](int value) {
			_effects.gain = value;
			effectsChanged();
		});

	Ui::AddSkip(container, st::boxLittleSkip);
	const auto normalize = AddCheckbox(
		container,
		tr::lng_oblivion_music_normalize(tr::now),
		_effects.normalize);
	normalize->checkedChanges() | rpl::on_next([=](bool checked) {
		_effects.normalize = checked;
		effectsChanged();
	}, normalize->lifetime());
	const auto fadeIn = AddCheckbox(
		container,
		tr::lng_oblivion_music_fade_in(tr::now),
		_effects.fadeIn);
	fadeIn->checkedChanges() | rpl::on_next([=](bool checked) {
		_effects.fadeIn = checked;
		effectsChanged();
	}, fadeIn->lifetime());
	const auto fadeOut = AddCheckbox(
		container,
		tr::lng_oblivion_music_fade_out(tr::now),
		_effects.fadeOut);
	fadeOut->checkedChanges() | rpl::on_next([=](bool checked) {
		_effects.fadeOut = checked;
		effectsChanged();
	}, fadeOut->lifetime());
	const auto denoise = AddCheckbox(
		container,
		tr::lng_oblivion_music_denoise(tr::now),
		_effects.denoise);
	denoise->checkedChanges() | rpl::on_next([=](bool checked) {
		_effects.denoise = checked;
		effectsChanged();
	}, denoise->lifetime());

	Ui::AddSkip(container, SectionEndSkip());
	Ui::AddDivider(container);
}

void Editor::setupExport(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_oblivion_music_export());
	_titleField = container->add(
		object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			tr::lng_oblivion_music_title_field(),
			QString()),
		RowPadding());
	_titleField->setMaxLength(kMaxTitleLength);
	_performerField = container->add(
		object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			tr::lng_oblivion_music_performer_field(),
			QString()),
		RowPadding() + QMargins(0, st::boxLittleSkip, 0, 0));
	_performerField->setMaxLength(kMaxTitleLength);

	Ui::AddSkip(container, st::boxLittleSkip);
	const auto format = std::make_shared<Ui::RadioenumGroup<Audio::Format>>(
		_format);
	const auto addFormat = [&](Audio::Format value, const QString &text) {
		container->add(
			object_ptr<Ui::Radioenum<Audio::Format>>(
				container,
				format,
				value,
				text,
				st::defaultBoxCheckbox),
			RowPadding() + QMargins(0, st::boxLittleSkip, 0, 0));
	};
	addFormat(
		Audio::Format::M4a,
		tr::lng_oblivion_music_format_m4a(tr::now));
	addFormat(
		Audio::Format::OggOpus,
		tr::lng_oblivion_music_format_ogg(tr::now));
	addFormat(
		Audio::Format::Wav,
		tr::lng_oblivion_music_format_wav(tr::now));
	format->setChangedCallback([=](Audio::Format value) {
		_format = value;
	});
	Ui::AddSkip(container, SectionEndSkip());
}

void Editor::setupPlayer() {
	using State = Audio::PreviewPlayer::State;
	_player->stateValue() | rpl::on_next([=](State state) {
		_playButton->setPlaying(state == State::Playing);
		refreshPlayhead();
	}, _lifetime);
	_player->positionValue() | rpl::on_next([=] {
		refreshPlayhead();
	}, _lifetime);
}

Track *Editor::findTrack(uint64 id) {
	const auto i = ranges::find(_tracks, id, &Track::id);
	return (i != end(_tracks)) ? &*i : nullptr;
}

const Track *Editor::firstTrack() const {
	const auto i = ranges::find(_tracks, TrackStatus::Ready, &Track::status);
	return (i != end(_tracks))
		? &*i
		: _tracks.empty()
		? nullptr
		: &_tracks.front();
}

bool Editor::canAddTrack() {
	if (int(_tracks.size()) < kMaxTracks) {
		return true;
	}
	_show->showToast(tr::lng_oblivion_music_too_many(
		tr::now,
		lt_max,
		QString::number(kMaxTracks)));
	return false;
}

uint64 Editor::addTrack(QString title, QString performer) {
	_tracks.push_back(Track{
		.id = ++_trackIdCounter,
		.title = std::move(title),
		.performer = std::move(performer),
	});
	return _tracks.back().id;
}

void Editor::addPath(const QString &path) {
	if (!canAddTrack()) {
		return;
	}
	const auto id = addTrack(QFileInfo(path).completeBaseName(), QString());
	decodeTrack(id, path, QByteArray());
}

void Editor::addContent(const QByteArray &content) {
	if (!canAddTrack()) {
		return;
	}
	const auto id = addTrack(QString(), QString());
	decodeTrack(id, QString(), content);
}

void Editor::addDocument(
		not_null<DocumentData*> document,
		Data::FileOrigin origin) {
	if (!canAddTrack()) {
		return;
	}
	auto title = QString();
	auto performer = QString();
	if (const auto song = document->song()) {
		title = song->title.trimmed();
		performer = song->performer.trimmed();
	}
	const auto preferTags = title.isEmpty();
	if (title.isEmpty()) {
		const auto name = document->filename();
		title = document->isVoiceMessage()
			? tr::lng_in_dlg_audio(tr::now)
			: name.isEmpty()
			? QString()
			: QFileInfo(name).completeBaseName();
	}
	const auto id = addTrack(title, performer);
	findTrack(id)->preferTags = preferTags;
	if (document->size > kMaxDocumentSize) {
		failTrack(id, tr::lng_oblivion_music_too_large(tr::now));
		return;
	}
	cancelDownload();
	_document = document;
	_documentTrackId = id;
	_documentMedia = document->createMediaView();
	const auto media = _documentMedia;
	const auto started = std::make_shared<bool>(false);
	const auto ready = [=] {
		if (*started) {
			return true;
		} else if (!media->loaded(true)) {
			return false;
		}
		const auto bytes = media->bytes();
		const auto path = bytes.isEmpty()
			? document->filepath(true)
			: QString();
		if (bytes.isEmpty() && path.isEmpty()) {
			return false;
		}
		*started = true;
		decodeTrack(id, path, bytes);

		// The decoder keeps its own reference to the bytes.
		_documentMedia = nullptr;
		return true;
	};
	if (ready()) {
		return;
	}
	if (const auto track = findTrack(id)) {
		track->status = TrackStatus::Downloading;
		track->progress = document->progress();
	}
	refreshTrackList();
	document->session().data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> loaded) {
		return (loaded == document);
	}) | rpl::on_next([=] {
		if (document->loading()) {
			if (const auto track = findTrack(id)) {
				track->progress = document->progress();
				refreshTrackList();
			}
		} else if (ready()) {
			_downloadLifetime.destroy();
		} else {
			_downloadLifetime.destroy();
			failTrack(id, tr::lng_oblivion_music_download_failed(tr::now));
		}
	}, _downloadLifetime);
	if (!document->loading()) {
		// Without the "save file" dialog: into memory if the file is small
		// enough, FileLoader asserts on larger ones without a target file,
		// so those go to a temporary file.
		auto target = QString();
		if (document->size >= Storage::kMaxFileInMemory) {
			const auto name = SafeFileName(document->filename());
			target = TempFilePath(name.isEmpty() ? u"audio"_q : name);
			if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
				_downloadLifetime.destroy();
				failTrack(id, tr::lng_oblivion_music_download_failed(tr::now));
				return;
			}
		}
		_ownDownload = true;
		document->save(origin, target);
		if (!document->loading() && !ready()) {
			_downloadLifetime.destroy();
			failTrack(id, tr::lng_oblivion_music_download_failed(tr::now));
		}
	}
}

void Editor::decodeTrack(uint64 id, QString path, QByteArray bytes) {
	if (const auto track = findTrack(id)) {
		track->status = TrackStatus::Decoding;
	}
	refreshTrackList();
	const auto weak = base::make_weak(this);
	crl::async([=, path = std::move(path), bytes = std::move(bytes)] {
		auto decoded = DecodeTrack(path, bytes);
		crl::on_main(weak, [=, decoded = std::move(decoded)]() mutable {
			applyDecoded(id, std::move(decoded));
		});
	});
}

void Editor::applyDecoded(uint64 id, Decoded decoded) {
	const auto track = findTrack(id);
	if (!track) {
		return;
	} else if (!decoded.pcm) {
		failTrack(id, tr::lng_oblivion_music_open_failed(tr::now));
		return;
	}

	// Every processing step keeps the whole timeline in memory.
	auto total = decoded.pcm->duration();
	for (const auto &other : _tracks) {
		if (other.status == TrackStatus::Ready && other.pcm) {
			total += other.pcm->duration();
		}
	}
	if (total > kMaxTotalDuration) {
		failTrack(id, tr::lng_oblivion_music_too_long(
			tr::now,
			lt_duration,
			FormatDuration(kMaxTotalDuration)));
		return;
	}
	track->pcm = std::move(decoded.pcm);
	track->status = TrackStatus::Ready;
	track->truncated = decoded.truncated;
	if (track->preferTags && !decoded.title.isEmpty()) {
		track->title = decoded.title;
		track->performer = decoded.performer;
	} else if (track->performer.isEmpty()) {
		track->performer = decoded.performer;
	}
	if (!decoded.cover.isNull()) {
		track->cover = std::move(decoded.cover);
	}
	tracksChanged();
}

void Editor::failTrack(uint64 id, const QString &error) {
	if (const auto track = findTrack(id)) {
		track->status = TrackStatus::Failed;
		track->error = error;
		track->pcm = nullptr;
		tracksChanged();
	}
}

void Editor::moveTrack(uint64 id, int delta) {
	const auto i = ranges::find(_tracks, id, &Track::id);
	if (i == end(_tracks)) {
		return;
	}
	const auto index = int(i - begin(_tracks));
	const auto to = index + delta;
	if (to < 0 || to >= int(_tracks.size())) {
		return;
	}
	std::swap(_tracks[index], _tracks[to]);
	tracksChanged();
}

void Editor::removeTrack(uint64 id) {
	const auto i = ranges::find(_tracks, id, &Track::id);
	if (i == end(_tracks)) {
		return;
	}
	if (id == _documentTrackId) {
		cancelDownload();
	}
	_tracks.erase(i);
	tracksChanged();
}

void Editor::showTrackMenu(int index) {
	if (index < 0 || index >= int(_tracks.size())) {
		return;
	}
	const auto id = _tracks[index].id;
	const auto count = int(_tracks.size());
	_menu = base::make_unique_q<Ui::PopupMenu>(
		_box.get(),
		st::popupMenuWithIcons);
	if (index > 0) {
		_menu->addAction(
			tr::lng_oblivion_music_move_up(tr::now),
			[=] { moveTrack(id, -1); },
			&st::menuIconAbove);
	}
	if (index + 1 < count) {
		_menu->addAction(
			tr::lng_oblivion_music_move_down(tr::now),
			[=] { moveTrack(id, 1); },
			&st::menuIconBelow);
	}
	_menu->addAction(
		tr::lng_oblivion_music_remove(tr::now),
		[=] { removeTrack(id); },
		&st::menuIconDelete);
	_menu->popup(QCursor::pos());
}

void Editor::chooseFiles() {
	if (!canAddTrack()) {
		return;
	}
	const auto filter = tr::lng_oblivion_music_filter_media(tr::now)
		+ u" (*.mp3 *.m4a *.aac *.ogg *.oga *.opus *.wav *.flac"_q
		+ u" *.mp4 *.mov *.m4v *.mkv *.webm);;"_q
		+ tr::lng_oblivion_music_filter_all(tr::now)
		+ u" (*)"_q;
	const auto weak = base::make_weak(this);
	FileDialog::GetOpenPaths(
		_box.get(),
		tr::lng_oblivion_music_choose(tr::now),
		filter,
		crl::guard(weak, [=](FileDialog::OpenResult &&result) {
			if (!result.paths.isEmpty()) {
				for (const auto &path : result.paths) {
					if (!canAddTrack()) {
						break;
					}
					addPath(path);
				}
			} else if (!result.remoteContent.isEmpty()) {
				addContent(result.remoteContent);
			}
		}));
}

void Editor::cancelDownload() {
	_downloadLifetime.destroy();
	if (_document && _ownDownload && _document->loading()) {
		_document->cancel();
	}
	_ownDownload = false;
	_documentTrackId = 0;
}

void Editor::tracksChanged() {
	refreshTrackList();
	refreshCrossfade();
	refreshAutoFields();
	requestJoin(false);
}

void Editor::requestJoin(bool delayed) {
	if (delayed) {
		_joining = true;
		_joinTimer.callOnce(kJoinDelay);
		refreshStatus();
	} else {
		startJoin();
	}
}

void Editor::startJoin() {
	_joinTimer.cancel();
	const auto request = ++_joinRequest;
	*_latestJoin = request;
	auto list = std::vector<std::shared_ptr<const Audio::Pcm>>();
	auto options = Audio::ConcatOptions{ .crossfade = _crossfade };
	for (const auto &track : _tracks) {
		if (track.status == TrackStatus::Ready && track.pcm) {
			list.push_back(track.pcm);

			// The best format of all the tracks, so that a mono or a low
			// rate track doesn't make the whole result mono or low rate.
			options.rate = std::max(options.rate, track.pcm->rate);
			options.channels = std::max(options.channels, track.pcm->channels);
		}
	}
	if (list.empty()) {
		_joinPending = false;
		applyJoined(request, nullptr, {});
		return;
	}
	_joining = true;
	refreshStatus();
	refreshWaveformStatus();
	if (_joinRunning) {
		// One join at a time: the running one is stale now, it stops
		// as soon as it can and then the current tracks are joined.
		_joinPending = true;
		return;
	}
	_joinRunning = true;
	options.rate = std::min(options.rate, kMaxRate);
	options.channels = std::min(options.channels, 2);
	const auto latest = _latestJoin;
	const auto cancelled = _cancelled;
	const auto weak = base::make_weak(this);
	crl::async([=, list = std::move(list)] {
		const auto stale = [=] {
			return cancelled->load() || (latest->load() != request);
		};
		auto joined = std::shared_ptr<const Audio::Pcm>();
		if (list.size() == 1) {
			joined = list.front();
		} else if (!stale()) {
			auto items = std::vector<not_null<const Audio::Pcm*>>();
			items.reserve(list.size());
			for (const auto &pcm : list) {
				items.push_back(pcm.get());
			}
			auto result = Audio::Concat(items, options);
			if (!result.empty()) {
				joined = std::make_shared<const Audio::Pcm>(std::move(result));
			}
		}
		auto peaks = (joined && !stale())
			? ComputePeaks(*joined, kPeaksCount)
			: std::vector<float>();
		crl::on_main(weak, [=, peaks = std::move(peaks)]() mutable {
			_joinRunning = false;
			if (cancelled->load()) {
				return;
			} else if (base::take(_joinPending)) {
				startJoin();
			} else if (_joinRequest == request) {
				applyJoined(request, joined, std::move(peaks));
			}
		});
	});
}

void Editor::applyJoined(
		uint64 request,
		std::shared_ptr<const Audio::Pcm> joined,
		std::vector<float> peaks) {
	_joining = false;
	const auto oldDuration = _duration;
	const auto wasFull = (_from == 0) && (_till == oldDuration);
	_joined = std::move(joined);
	_joinedId = request;
	_duration = _joined ? _joined->duration() : 0;
	const auto initial = _duration
		? base::take(_initialRange)
		: std::nullopt;
	if (!_duration) {
		_from = _till = 0;
	} else if (initial
		&& initial->first >= 0
		&& initial->second <= _duration
		&& initial->second - initial->first >= kMinSelection) {
		_from = initial->first;
		_till = initial->second;
	} else if (wasFull) {
		_from = 0;
		_till = _duration;
	} else {
		_till = std::min(_till, _duration);
		_from = std::min(_from, _till);
		if (_till - _from < std::min(kMinSelection, _duration)) {
			_from = 0;
			_till = _duration;
		}
	}
	_waveform->setData(std::move(peaks), _duration);
	_waveform->setRange(_from, _till);

	// The timeline has changed, positions of the old one are meaningless.
	_player->setPcm(Audio::Pcm());
	_rendered = nullptr;
	_renderedParams = Params();
	_renderingParams = std::nullopt;
	_pendingSeek = std::nullopt;

	refreshWaveformStatus();
	refreshLabels();
	startRender();
}

Params Editor::currentParams() const {
	return {
		.source = _joinedId,
		.from = _from,
		.till = _till,
		.effects = _effects,
	};
}

void Editor::effectsChanged() {
	_presets->setSelected(DetectPreset(_effects));
	refreshAutoFields();
	refreshLabels();
	scheduleRender();
}

void Editor::applyPreset(Preset preset) {
	if (preset == Preset::Custom) {
		return;
	}
	_effects = ApplyPreset(_effects, preset);
	for (const auto &setter : _effectsSetters) {
		setter();
	}
	effectsChanged();
}

void Editor::scheduleRender() {
	if (!_joined) {
		return;
	}
	_renderTimer.callOnce(kRenderDelay);
	refreshStatus();
}

void Editor::startRender() {
	_renderTimer.cancel();
	if (!_joined) {
		++_renderRequest;
		*_latestRender = _renderRequest;
		_renderingParams = std::nullopt;
		_rendered = nullptr;
		_renderedParams = Params();
		_player->setPcm(Audio::Pcm());
		refreshStatus();
		refreshPlayhead();
		return;
	}
	const auto params = currentParams();
	if ((_rendered && _renderedParams == params)
		|| (_renderingParams && *_renderingParams == params)) {
		refreshStatus();
		return;
	}
	const auto request = ++_renderRequest;
	*_latestRender = request;
	_renderingParams = params;
	_renderFailed = false;
	refreshStatus();
	if (_renderRunning) {
		// One render at a time: the running one is stale now, it stops
		// after its current step and then these params are rendered.
		return;
	}
	_renderRunning = true;

	const auto source = _joined;
	const auto latest = _latestRender;
	const auto cancelled = _cancelled;
	const auto weak = base::make_weak(this);
	crl::async([=] {
		const auto stale = [=] {
			return cancelled->load() || (latest->load() != request);
		};
		auto result = RenderPcm(*source, params, stale);
		auto rendered = result.empty()
			? nullptr
			: std::make_shared<const Audio::Pcm>(std::move(result));
		crl::on_main(weak, [=, rendered = std::move(rendered)]() mutable {
			_renderRunning = false;
			if (_renderRequest == request) {
				applyRendered(params, std::move(rendered));
			} else if (_renderingParams) {
				_renderingParams = std::nullopt;
				startRender();
			}
		});
	});
}

void Editor::applyRendered(
		Params params,
		std::shared_ptr<const Audio::Pcm> rendered) {
	using State = Audio::PreviewPlayer::State;
	_renderingParams = std::nullopt;
	if (!rendered) {
		_renderFailed = true;
		_playWhenReady = false;
		_pendingSeek = std::nullopt;
		refreshStatus();
		return;
	}
	_renderFailed = false;
	const auto state = _player->state();
	const auto position = _player->position();
	const auto keep = _rendered
		&& (_renderedParams.source == params.source)
		&& ((state != State::Stopped) || (position > 0));
	const auto source = keep
		? ToSource(_renderedParams, position)
		: params.from;
	_rendered = std::move(rendered);
	_renderedParams = params;
	_player->setPcm(_rendered);
	if (const auto seek = base::take(_pendingSeek)) {
		_player->seek(ToProcessed(params, *seek));
	} else if (keep) {
		_player->seek(ToProcessed(params, source));
		if (state == State::Playing) {
			_player->play();
		}
	}
	if (_playWhenReady) {
		_playWhenReady = false;
		if (_player->state() != State::Playing) {
			_player->play();
		}
	}
	refreshStatus();
	refreshPlayhead();
}

void Editor::togglePlay() {
	using State = Audio::PreviewPlayer::State;
	if (_player->state() == State::Playing) {
		_player->pause();
		return;
	} else if (!_joined) {
		return;
	} else if (!_rendered) {
		_playWhenReady = true;
		startRender();
		return;
	}
	_player->play();
}

void Editor::seekTo(crl::time source) {
	using State = Audio::PreviewPlayer::State;
	if (!_rendered) {
		if (_joined) {
			_pendingSeek = source;
			_playWhenReady = true;
			startRender();
		}
		return;
	}
	_player->seek(ToProcessed(_renderedParams, source));
	if (_player->state() != State::Playing) {
		_player->play();
	}
	refreshPlayhead();
}

void Editor::refreshTrackList() {
	auto rows = std::vector<TrackList::Row>();
	rows.reserve(_tracks.size());
	auto index = 0;
	for (const auto &track : _tracks) {
		auto row = TrackList::Row{
			.number = ++index,
			.title = TrackDisplayName(track),
		};
		switch (track.status) {
		case TrackStatus::Downloading:
			row.subtitle = tr::lng_oblivion_music_downloading(
				tr::now,
				lt_percent,
				FormatPercent(track.progress));
			break;
		case TrackStatus::Decoding:
			row.subtitle = tr::lng_oblivion_music_decoding(tr::now);
			break;
		case TrackStatus::Failed:
			row.subtitle = track.error;
			row.error = true;
			break;
		case TrackStatus::Ready:
			row.subtitle = track.truncated
				? tr::lng_oblivion_music_truncated(
					tr::now,
					lt_duration,
					FormatDuration(kMaxTrackDuration))
				: TrackInfo(*track.pcm);
			row.right = FormatDuration(track.pcm->duration());
			break;
		}
		rows.push_back(std::move(row));
	}
	_trackList->setRows(std::move(rows));
	_hasTracks = !_tracks.empty();
	refreshWaveformStatus();
}

void Editor::refreshCrossfade(anim::type animated) {
	const auto ready = ranges::count(
		_tracks,
		TrackStatus::Ready,
		&Track::status);
	_crossfadeWrap->toggle(ready > 1, animated);
}

void Editor::refreshWaveformStatus() {
	auto status = QString();
	if (_tracks.empty()) {
		status = tr::lng_oblivion_music_empty(tr::now);
	} else if (!_joined) {
		const auto downloading = ranges::find(
			_tracks,
			TrackStatus::Downloading,
			&Track::status);
		const auto pending = _joining
			|| (downloading != end(_tracks))
			|| ranges::any_of(_tracks, [](const Track &track) {
				return (track.status == TrackStatus::Decoding);
			});
		status = (downloading != end(_tracks))
			? tr::lng_oblivion_music_downloading(
				tr::now,
				lt_percent,
				FormatPercent(downloading->progress))
			: pending
			? tr::lng_oblivion_music_decoding(tr::now)
			: tr::lng_oblivion_music_open_failed(tr::now);
	}
	_waveform->setStatus(status);
}

void Editor::refreshStatus() {
	const auto processing = _joining
		|| _renderingParams.has_value()
		|| _renderTimer.isActive();
	_statusLabel->setText(_exporting
		? tr::lng_oblivion_music_encoding(tr::now)
		: processing
		? tr::lng_oblivion_music_processing(tr::now)
		: _renderFailed
		? tr::lng_oblivion_music_process_failed(tr::now)
		: QString());
}

void Editor::refreshLabels() {
	_rangeLabel->setText(_duration
		? tr::lng_oblivion_music_range(
			tr::now,
			lt_from,
			FormatPrecise(_from),
			lt_till,
			FormatPrecise(_till),
			lt_duration,
			FormatPrecise(_till - _from))
		: QString());
	refreshPlayhead();
}

void Editor::refreshPlayhead() {
	using State = Audio::PreviewPlayer::State;
	const auto state = _player->state();
	const auto position = _player->position();
	const auto active = _rendered
		&& ((state != State::Stopped) || (position > 0));
	_waveform->setPlayhead(active
		? std::make_optional(ToSource(_renderedParams, position))
		: std::nullopt);
	const auto total = _rendered
		? _rendered->duration()
		: _joined
		? ExpectedDuration(currentParams())
		: crl::time(0);
	// Without a file the disabled button still has its "0:00 / 0:00" next
	// to it, not an empty space as if something failed to load.
	_timeLabel->setText(FormatDuration(active ? position : 0)
		+ u" / "_q
		+ FormatDuration(total));
	_playButton->setDisabled(!_joined);
}

void Editor::refreshAutoFields() {
	const auto track = firstTrack();
	const auto base = track ? track->title : QString();
	const auto tag = EffectsTag(_effects);
	const auto title = tag.isEmpty()
		? base
		: base.isEmpty()
		? tag
		: (base + u" ("_q + tag + u")"_q);
	const auto performer = track ? track->performer : QString();
	if (_titleField->getLastText() == _autoTitle && title != _autoTitle) {
		_titleField->setText(title);
	}
	_autoTitle = title;
	if (_performerField->getLastText() == _autoPerformer
		&& performer != _autoPerformer) {
		_performerField->setText(performer);
	}
	_autoPerformer = performer;
}

QString Editor::exportTitle() const {
	const auto result = _titleField->getLastText().trimmed();
	if (!result.isEmpty()) {
		return result;
	}
	const auto track = firstTrack();
	return (track && !track->title.isEmpty())
		? track->title
		: tr::lng_oblivion_music_default_name(tr::now);
}

QString Editor::exportPerformer() const {
	return _performerField->getLastText().trimmed();
}

QImage Editor::exportCover() const {
	for (const auto &track : _tracks) {
		if (track.status == TrackStatus::Ready && !track.cover.isNull()) {
			return track.cover;
		}
	}
	return QImage();
}

bool Editor::checkExportPossible() {
	if (_exporting) {
		_show->showToast(tr::lng_oblivion_music_busy(tr::now));
		return false;
	} else if (!_joined) {
		_show->showToast(tr::lng_oblivion_music_nothing(tr::now));
		return false;
	}
	return true;
}

void Editor::exportAudio(
		Audio::Format format,
		const QString &path,
		const QString &title,
		const QString &performer,
		Fn<void(Exported)> done) {
	if (!checkExportPossible()) {
		return;
	}
	const auto params = currentParams();
	const auto ready = (_rendered && _renderedParams == params)
		? _rendered
		: nullptr;
	const auto source = _joined;
	const auto cancelled = _cancelled;
	const auto tags = Audio::EncodeOptions{
		.title = title,
		.performer = performer,
	};
	_exporting = true;
	refreshStatus();
	_box->showLoading(true);

	const auto weak = base::make_weak(this);
	crl::async([=] {
		auto rendered = ready;
		if (!rendered) {
			auto result = RenderPcm(*source, params, [=] {
				return cancelled->load();
			});
			if (!result.empty()) {
				rendered = std::make_shared<const Audio::Pcm>(
					std::move(result));
			}
		}
		const auto bytes = (rendered && !cancelled->load())
			? Audio::Encode(*rendered, format, tags)
			: QByteArray();
		const auto duration = rendered ? rendered->duration() : crl::time(0);
		const auto size = int64(bytes.size());
		const auto written = !bytes.isEmpty()
			&& !cancelled->load()
			&& WriteToFile(path, bytes);
		crl::on_main(weak, [=] {
			_exporting = false;
			_box->showLoading(false);
			refreshStatus();
			if (cancelled->load()) {
				return;
			} else if (!size) {
				_show->showToast(tr::lng_oblivion_music_encode_failed(tr::now));
				return;
			} else if (!written) {
				_show->showToast(tr::lng_oblivion_write_failed(tr::now));
				return;
			}
			done({
				.path = path,
				.size = size,
				.duration = duration,
				.format = format,
			});
		});
	});
}

void Editor::save() {
	if (!checkExportPossible()) {
		return;
	}
	const auto format = _format;
	const auto title = exportTitle();
	const auto performer = exportPerformer();
	const auto name = ExportFileName(title, performer, format);
	const auto filter = FileFilter(
		FormatFilterName(format),
		u"*."_q + Audio::FormatExtension(format));
	const auto weak = base::make_weak(this);
	FileDialog::GetWritePath(
		_box.get(),
		tr::lng_oblivion_music_save_title(tr::now),
		filter,
		SuggestedPath(name),
		crl::guard(weak, [=](QString &&result) {
			if (result.isEmpty()) {
				return;
			}
			const auto saved = [=](Exported exported) {
				_show->showToast(tr::lng_oblivion_saved_to(
					tr::now,
					lt_path,
					QDir::toNativeSeparators(exported.path)));
			};
			exportAudio(format, result, title, performer, saved);
		}));
}

void Editor::send() {
	if (!_session.chooseChat
		|| !_session.canSend
		|| !_session.checkPayment
		|| !checkExportPossible()) {
		return;
	}
	const auto weak = base::make_weak(this);
	const auto canSend = _session.canSend;
	_session.chooseChat([=](not_null<Data::Thread*> thread) {
		const auto strong = weak.get();
		if (!strong) {
			return true;
		} else if (!canSend(thread)) {
			return false;
		}
		strong->sendTo(thread);
		return true;
	});
}

void Editor::sendTo(not_null<Data::Thread*> thread) {
	const auto weakThread = base::make_weak(thread);
	const auto title = exportTitle();
	const auto performer = exportPerformer();
	const auto cover = exportCover();
	const auto format = _format;
	const auto path = TempFilePath(ExportFileName(title, performer, format));
	exportAudio(format, path, title, performer, [=](Exported exported) {
		if (const auto strong = weakThread.get()) {
			finishSend(strong, std::move(exported), title, performer, cover, 0);
		}
	});
}

void Editor::finishSend(
		not_null<Data::Thread*> thread,
		Exported exported,
		QString title,
		QString performer,
		QImage cover,
		int starsApproved) {
	if (!_session.canSend || !_session.canSend(thread)) {
		return;
	}
	const auto options = Api::SendOptions{
		.starsApproved = starsApproved,
	};
	const auto weak = base::make_weak(this);
	const auto weakThread = base::make_weak(thread);
	const auto resend = [=](int approved) {
		const auto strong = weak.get();
		const auto target = weakThread.get();
		if (strong && target) {
			strong->finishSend(
				target,
				exported,
				title,
				performer,
				cover,
				approved);
		}
	};
	if (!_session.checkPayment
		|| !_session.checkPayment(thread, options, resend)) {
		return;
	}
	auto file = Ui::PreparedFile(exported.path);
	file.size = exported.size;
	file.type = Ui::PreparedFile::Type::Music;
	file.information = std::make_unique<Ui::PreparedFileInformation>();
	file.information->filemime = Audio::FormatMimeType(exported.format);
	file.information->media = Ui::PreparedFileInformation::Song{
		.duration = exported.duration,
		.title = title,
		.performer = performer,
		.cover = cover,
	};
	auto list = Ui::PreparedList();
	list.files.push_back(std::move(file));

	auto action = Api::SendAction(thread, options);
	action.clearDraft = false;
	action.sendForwardDraft = false;
	if (!action.replyTo.monoforumPeerId) {
		action.replyTo.monoforumPeerId = thread->monoforumPeerId();
	}
	thread->session().api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		action);
	_show->showToast(tr::lng_oblivion_music_sent(
		tr::now,
		lt_chat,
		thread->peer()->name()));
}

void MusicEditorBox(not_null<Ui::GenericBox*> box, EditorArgs &&args) {
	box->setTitle(tr::lng_oblivion_tools_music_editor());
	box->setWidth(style::ConvertScale(kBoxWidth));
	const auto document = args.document;
	const auto origin = args.origin;
	const auto created = std::move(args.created);
	const auto editor = box->lifetime().make_state<Editor>(
		box,
		std::move(args));
	editor->setup();
	if (document) {
		editor->addDocument(document, origin);
	}
	if (created) {
		created(editor);
	}
}

[[nodiscard]] SessionHooks MakeSessionHooks(
		not_null<Window::SessionController*> controller) {
	const auto show = controller->uiShow();

	// Owned by the editor through these callbacks. The resend callback
	// that the helper keeps must not own it back.
	const auto payment = std::make_shared<SendPaymentHelper>();
	const auto weakPayment = std::weak_ptr<SendPaymentHelper>(payment);
	return {
		.chooseChat = [=](FnMut<bool(not_null<Data::Thread*>)> chosen) {
			Window::ShowChooseRecipientBox(
				controller,
				std::move(chosen),
				tr::lng_oblivion_music_send_title());
		},
		.canSend = [=](not_null<Data::Thread*> thread) {
			return CheckCanSendMusic(show, thread);
		},
		.checkPayment = [=](
				not_null<Data::Thread*> thread,
				Api::SendOptions options,
				Fn<void(int)> resend) {
			return payment->check(
				controller,
				thread->peer(),
				options,
				1,
				[=](int approved) {
					if (const auto strong = weakPayment.lock()) {
						strong->clear();
					}
					resend(approved);
				});
		},
	};
}

// UI snapshots (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).

constexpr auto kSceneWidth = 720;
constexpr auto kSceneWait = crl::time(6000);
constexpr auto kSampleRate = 44100;
constexpr auto kSampleCrossfade = crl::time(2000);
constexpr auto kSampleFrom = crl::time(6000);
constexpr auto kSampleTill = crl::time(42500);

struct SampleChord {
	int root = 0; // MIDI notes.
	std::array<int, 3> notes = {};
};

struct SampleSection {
	float64 till = 0.; // Seconds.
	float64 levelFrom = 1.;
	float64 levelTo = 1.;
	bool drums = false;
	bool bass = false;
	bool hats = false;
};

struct SampleSong {
	std::vector<SampleChord> chords; // One per bar.
	int bpm = 100;
	float64 seconds = 0.;
	std::vector<SampleSection> sections;
	uint32 seed = 1;
};

// A deterministic synthetic song: a pad playing a chord progression, an
// arpeggio, a bass line, a kick, a snare and hi-hats, with sections of
// different loudness, so that the waveform looks like music.
[[nodiscard]] Audio::Pcm GenerateSong(const SampleSong &song) {
	Expects(!song.chords.empty());
	Expects(!song.sections.empty());

	constexpr auto kTwoPi = 2. * std::numbers::pi;
	constexpr auto kPattern = std::array{ 0, 1, 2, 1 };
	static const auto kFrequencies = [] {
		auto result = std::array<float64, 128>();
		for (auto note = 0; note != int(result.size()); ++note) {
			result[note] = 440. * std::pow(2., (note - 69) / 12.);
		}
		return result;
	}();
	const auto noteFrequency = [&](int note) {
		return kFrequencies[std::clamp(note, 0, 127)];
	};
	const auto rate = float64(kSampleRate);
	const auto frames = int64(song.seconds * rate);
	auto result = Audio::Pcm{ .channels = 2, .rate = kSampleRate };
	result.samples.resize(frames * 2);

	const auto beat = 60. / song.bpm;
	const auto bar = beat * 4.;
	const auto step = beat / 2.;
	auto seed = song.seed;
	const auto noise = [&] {
		seed = seed * 1664525u + 1013904223u;
		return (int((seed >> 8) & 0xFFFF) - 32768) / 32768.;
	};
	const auto advance = [](float64 &phase, float64 delta) {
		phase += delta;
		phase -= std::floor(phase);
		return phase;
	};
	auto pad = std::array<float64, 3>();
	auto arp = 0.;
	auto bass = 0.;
	auto kick = 0.;
	auto snare = 0.;
	auto section = size_t(0);
	auto sectionFrom = 0.;
	auto out = result.samples.data();
	for (auto f = int64(); f != frames; ++f) {
		const auto t = f / rate;
		while (section + 1 < song.sections.size()
			&& t >= song.sections[section].till) {
			sectionFrom = song.sections[section].till;
			++section;
		}
		const auto &part = song.sections[section];
		const auto length = std::max(part.till - sectionFrom, 0.001);
		const auto progress = std::clamp((t - sectionFrom) / length, 0., 1.);
		const auto level = part.levelFrom
			+ (part.levelTo - part.levelFrom) * progress;

		const auto barIndex = int(t / bar);
		const auto &chord = song.chords[barIndex % song.chords.size()];
		const auto inBar = t - barIndex * bar;
		const auto beatIndex = int(t / beat);
		const auto inBeat = t - beatIndex * beat;
		const auto stepIndex = int(t / step);
		const auto inStep = t - stepIndex * step;

		auto padValue = 0.;
		for (auto i = 0; i != 3; ++i) {
			const auto phase = advance(
				pad[i],
				noteFrequency(chord.notes[i]) / rate);
			padValue += std::sin(kTwoPi * phase)
				+ 0.3 * std::sin(2. * kTwoPi * phase);
		}
		padValue *= std::min(inBar / 0.08, 1.) * 0.1;

		const auto arpNote = chord.notes[kPattern[stepIndex % 4]] + 12;
		const auto arpValue = std::sin(kTwoPi * advance(
			arp,
			noteFrequency(arpNote) / rate))
			* std::min(inStep / 0.005, 1.)
			* std::exp(-inStep * 9.)
			* 0.22;
		const auto arpPan = (stepIndex % 2) ? 0.3 : -0.3;

		auto bassValue = 0.;
		if (part.bass) {
			const auto phase = advance(bass, noteFrequency(chord.root) / rate);
			bassValue = (std::sin(kTwoPi * phase)
				+ 0.25 * std::sin(3. * kTwoPi * phase))
				* (0.55 + 0.45 * std::exp(-inBeat * 4.))
				* std::min(inBeat / 0.01, 1.)
				* 0.3;
		}

		auto drumsValue = 0.;
		if (part.drums) {
			if (inBeat * rate < 1.) {
				kick = 0.;
			}
			const auto frequency = 45. + 70. * std::exp(-inBeat * 30.);
			drumsValue += std::sin(kTwoPi * advance(kick, frequency / rate))
				* std::exp(-inBeat * 9.)
				* 0.75;
			if (beatIndex % 2) {
				drumsValue += (noise() * 0.7
					+ std::sin(kTwoPi * advance(snare, 185. / rate)) * 0.3)
					* std::exp(-inBeat * 18.)
					* 0.35;
			}
		}
		auto hatsValue = 0.;
		if (part.hats && (stepIndex % 2)) {
			hatsValue = noise() * std::exp(-inStep * 70.) * 0.12;
		}

		const auto center = (padValue + bassValue + drumsValue) * level;
		const auto left = center
			+ (arpValue * (1. - arpPan) + hatsValue * 0.7) * level;
		const auto right = center
			+ (arpValue * (1. + arpPan) + hatsValue * 1.3) * level;
		*out++ = float(left);
		*out++ = float(right);
	}
	return Audio::Normalize(result, 0.89);
}

[[nodiscard]] std::vector<ReadyTrack> SampleTracks() {
	static const auto result = [] {
		const auto make = [](
				QString title,
				QString performer,
				const SampleSong &song) {
			return ReadyTrack{
				.title = std::move(title),
				.performer = std::move(performer),
				.pcm = std::make_shared<const Audio::Pcm>(GenerateSong(song)),
			};
		};

		// Am - F - C - G, 100 BPM, 30 seconds.
		const auto first = SampleSong{
			.chords = {
				{ 45, { 57, 60, 64 } },
				{ 41, { 57, 60, 65 } },
				{ 48, { 55, 60, 64 } },
				{ 43, { 55, 59, 62 } },
			},
			.bpm = 100,
			.seconds = 30.,
			.sections = {
				{ .till = 4.8, .levelFrom = 0.3, .levelTo = 0.55 },
				{
					.till = 14.4,
					.levelFrom = 0.7,
					.levelTo = 0.75,
					.drums = true,
					.bass = true,
				},
				{
					.till = 16.8,
					.levelFrom = 0.5,
					.levelTo = 0.45,
					.bass = true,
				},
				{
					.till = 26.4,
					.levelFrom = 1.,
					.levelTo = 1.,
					.drums = true,
					.bass = true,
					.hats = true,
				},
				{ .till = 30., .levelFrom = 0.8, .levelTo = 0.1, .bass = true },
			},
			.seed = 7,
		};

		// Dm - Bb - F - C, 88 BPM, 20 seconds.
		const auto second = SampleSong{
			.chords = {
				{ 38, { 57, 62, 65 } },
				{ 34, { 58, 62, 65 } },
				{ 41, { 57, 60, 65 } },
				{ 36, { 55, 60, 64 } },
			},
			.bpm = 88,
			.seconds = 20.,
			.sections = {
				{ .till = 2.73, .levelFrom = 0.35, .levelTo = 0.6 },
				{
					.till = 10.9,
					.levelFrom = 0.9,
					.levelTo = 0.9,
					.drums = true,
					.bass = true,
					.hats = true,
				},
				{
					.till = 13.6,
					.levelFrom = 0.5,
					.levelTo = 0.6,
					.bass = true,
				},
				{
					.till = 20.,
					.levelFrom = 0.95,
					.levelTo = 0.1,
					.drums = true,
					.bass = true,
					.hats = true,
				},
			},
			.seed = 13,
		};
		return std::vector<ReadyTrack>{
			make(u"Midnight Avenue"_q, u"Lumen Drive"_q, first),
			make(
				QString::fromUtf8("\xD0\xA1\xD0\xB5\xD0\xB2\xD0\xB5\xD1\x80"
					"\xD0\xBD\xD0\xBE\xD0\xB5 \xD1\x81\xD0\xB8\xD1\x8F"
					"\xD0\xBD\xD0\xB8\xD0\xB5"),
				u"Aurora Lane"_q,
				second),
		};
	}();
	return result;
}

[[nodiscard]] EditorArgs SampleEditorArgs(
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<base::weak_ptr<Editor>> handle) {
	auto effects = ApplyPreset(Effects(), Preset::Slowed);
	effects.normalize = true;
	effects.fadeOut = true;
	return {
		.show = std::move(show),
		.tracks = SampleTracks(),
		.effects = effects,
		.crossfade = kSampleCrossfade,
		.range = std::make_pair(kSampleFrom, kSampleTill),
		.created = [=](not_null<Editor*> editor) {
			*handle = base::make_weak(editor.get());
		},
	};
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	// Shared by the create() and ready() callbacks of one scene, scenes
	// are rendered one by one.
	const auto handle = std::make_shared<base::weak_ptr<Editor>>();
	const auto idle = [=] {
		const auto strong = handle->get();
		return strong && strong->idle();
	};
	const auto size = QSize(style::ConvertScale(kSceneWidth), 0);

	// Two tracks joined with a crossfade and trimmed, Slowed + reverb.
	RegisterScene({
		.name = u"music_editor"_q,
		.size = size,
		.box = [=](std::shared_ptr<Ui::Show> show) {
			return Box(MusicEditorBox, SampleEditorArgs(show, handle));
		},
		.ready = [=](not_null<QWidget*>) { return idle(); },
		.wait = kSceneWait,
	});

	// The same, scrolled to the export settings at the bottom.
	RegisterScene({
		.name = u"music_editor_export"_q,
		.size = size,
		.box = [=](std::shared_ptr<Ui::Show> show) {
			return Box(MusicEditorBox, SampleEditorArgs(show, handle));
		},
		.ready = [=](not_null<QWidget*> widget) {
			if (!idle()) {
				return false;
			}
			static_cast<Ui::BoxContent*>(widget.get())->scrollToY(
				QWIDGETSIZE_MAX);
			return true;
		},
		.wait = kSceneWait,
	});

	// Nothing opened yet.
	RegisterScene({
		.name = u"music_empty"_q,
		.size = size,
		.box = [=](std::shared_ptr<Ui::Show> show) {
			return Box(MusicEditorBox, EditorArgs{
				.show = std::move(show),
				.created = [=](not_null<Editor*> editor) {
					*handle = base::make_weak(editor.get());
				},
			});
		},
		.ready = [=](not_null<QWidget*>) { return idle(); },
	});
});

} // namespace

void ShowMusicEditor(
		not_null<Window::SessionController*> controller,
		DocumentData *document,
		Data::FileOrigin origin) {
	// Before the document location is checked: a file removed here
	// is downloaded again instead of failing to open.
	static auto cleaned = false;
	if (!std::exchange(cleaned, true)) {
		CleanupTemp();
	}
	controller->show(Box(MusicEditorBox, EditorArgs{
		.show = controller->uiShow(),
		.session = MakeSessionHooks(controller),
		.document = document,
		.origin = origin,
	}));
}

void AddMusicEditorAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	// The Downloads list shows items of all accounts, while the action
	// looks the message up again in the controller's session.
	if (!document
		|| (&item->history()->session() != &controller->session())
		|| media->ttlSeconds()
		|| item->forbidsSaving()
		|| item->isSending()
		|| item->hasFailed()
		|| !IsEditableAudio(document)) {
		return;
	}
	const auto itemId = item->fullId();
	menu->addAction(tr::lng_oblivion_music_open_in(tr::now), crl::guard(
		controller,
		[=] {
			const auto item = controller->session().data().message(itemId);
			const auto media = item ? item->media() : nullptr;
			if (const auto document = media ? media->document() : nullptr) {
				ShowMusicEditor(
					controller,
					document,
					Data::FileOriginMessage(itemId));
			}
		}), &st::menuIconSoundSelect);
}

} // namespace Oblivion
