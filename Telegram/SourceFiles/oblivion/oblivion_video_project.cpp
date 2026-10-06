/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_video_project.h"

#include "oblivion/oblivion_audio.h"
#include "oblivion/oblivion_video_editor.h"

#include <QtGui/QPainter>
#include <QtGui/QTransform>

#include <cmath>

namespace Oblivion::VideoEdit {
namespace {

// What VideoCore::ReadAudio() gives and VideoCore::Mp4Encoder takes.
constexpr auto kAudioRate = 48000;
constexpr auto kAudioChannels = 2;

// The audio is read (and its tempo is changed) by blocks, so a long clip
// never has all its samples in memory. A short rest joins the last block.
constexpr auto kAudioBlock = crl::time(30'000);
constexpr auto kAudioBlockRest = crl::time(3'000);

// A block is read with some audio around it: a decoder gives nothing good
// right after a seek and the tempo filter needs what goes on at the end.
// The blocks are joined with a short crossfade, at the place (around the
// expected one) where the next block looks most like the end of the
// previous one, as the tempo filter itself joins its pieces.
constexpr auto kAudioMargin = crl::time(300);
constexpr auto kAudioFade = int64(480); // Frames.
constexpr auto kAudioSearch = int64(240);

// The same as in the video core: a source that is only a bit faster than
// the limit keeps all its frames.
constexpr auto kKeptRateExcess = 1.134;

// A part of the sticker progress taken by reading the frames.
constexpr auto kStickerReadShare = 0.2;

constexpr auto kMp4MinSide = 16;
constexpr auto kMp4AudioBitrate = 128 * 1024;
constexpr auto kDefaultFps = 30.;

[[nodiscard]] bool IsSticker(Format format) {
	return (format == Format::Sticker) || (format == Format::Emoji);
}

[[nodiscard]] bool IsMp4(Format format) {
	return (format == Format::Mp4) || (format == Format::GifVideo);
}

[[nodiscard]] float64 SpeedFactor(const State &state) {
	return std::clamp(state.speed, kMinSpeed, kMaxSpeed) / 100.;
}

[[nodiscard]] int NormalizedRotation(int rotation) {
	return ((rotation % 360) + 360) % 360 / 90 * 90;
}

[[nodiscard]] const Source *ClipSource(
		const Project &project,
		const Clip &clip) {
	return (clip.source >= 0 && clip.source < int(project.sources.size()))
		? &project.sources[clip.source]
		: nullptr;
}

// The frame rate of the result when nothing is dropped.
[[nodiscard]] float64 ResultFps(const Project &project) {
	if (project.state.clips.empty()) {
		return kDefaultFps;
	}
	const auto source = ClipSource(project, project.state.clips.front());
	const auto fps = (source && source->info.fps > 0.)
		? source->info.fps
		: kDefaultFps;
	return fps * SpeedFactor(project.state);
}

[[nodiscard]] QImage Filled(QSize size, QRgb fill) {
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	result.fill(QColor::fromRgba(fill));
	return result;
}

// A frame read by the placement as a frame of the result.
[[nodiscard]] QImage Compose(
		QImage image,
		const Placement &placement,
		QSize output,
		int rotation,
		QRgb fill) {
	if (placement.empty() || image.isNull()) {
		return Filled(output, fill);
	}
	if (rotation) {
		image = image.transformed(QTransform().rotate(rotation));
	}
	if (image.format() != QImage::Format_ARGB32_Premultiplied) {
		image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	const auto whole = QRect(QPoint(), output);
	if (placement.target == whole) {
		return (image.size() == output)
			? image
			: image.scaled(
				output,
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
	}
	auto result = Filled(output, fill);
	{
		auto p = QPainter(&result);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		if (image.size() != placement.target.size()) {
			p.setRenderHint(QPainter::SmoothPixmapTransform);
		}
		p.drawImage(placement.target, image);
	}
	return result;
}

// The sound of a clip with the speed applied, in step with its video:
// every call gives exactly the count of frames asked, silence where the
// clip has no sound (or when it has ended).
class ClipAudio final {
public:
	ClipAudio(const Source &source, const Clip &clip, float64 speed);

	void read(int64 frames, std::vector<float> &to);

private:
	void loadMore();

	const Source &_source;
	const crl::time _till = 0;
	const float64 _speed = 1.;
	crl::time _next = 0;
	std::vector<float> _queue;
	std::vector<float> _tail; // What goes after the end of the last block.
	size_t _offset = 0;
	bool _finished = false;

};

ClipAudio::ClipAudio(const Source &source, const Clip &clip, float64 speed)
: _source(source)
, _till(std::min(clip.till, source.info.duration))
, _speed(speed)
, _next(clip.from)
, _finished(!source.info.hasAudio) {
}

void ClipAudio::read(int64 frames, std::vector<float> &to) {
	auto need = size_t(std::max(frames, int64(0))) * kAudioChannels;
	to.reserve(to.size() + need);
	while (need > 0) {
		if (_offset >= _queue.size()) {
			_queue.clear();
			_offset = 0;
			if (_finished) {
				break;
			}
			loadMore();
			continue;
		}
		const auto take = std::min(need, _queue.size() - _offset);
		to.insert(
			end(to),
			begin(_queue) + _offset,
			begin(_queue) + _offset + take);
		_offset += take;
		need -= take;
	}
	to.insert(end(to), need, 0.f);
}

void ClipAudio::loadMore() {
	if (_next >= _till) {
		_finished = true;
		return;
	}
	const auto from = _next;
	auto till = std::min(from + kAudioBlock, _till);
	if (_till - till < kAudioBlockRest) {
		till = _till;
	}
	const auto before = std::min(kAudioMargin, from);
	const auto after = std::clamp(
		_source.info.duration - till,
		crl::time(0),
		kAudioMargin);
	auto samples = VideoCore::ReadAudio({
		.path = _source.path,
		.content = _source.content,
		.from = from - before,
		.till = till + after,
	});
	_next = till;
	if (samples.empty()) {
		_finished = true;
		return;
	}
	if (std::abs(_speed - 1.) > 0.001) {
		auto pcm = Audio::Pcm{
			.samples = std::move(samples),
			.channels = kAudioChannels,
			.rate = kAudioRate,
		};
		const auto frames = int64(std::llround(pcm.frames() / _speed));
		auto changed = Audio::ChangeTempo(pcm, _speed);
		if (changed.empty()
			|| changed.channels != kAudioChannels
			|| changed.rate != kAudioRate) {
			// Silence keeps the rest of the sound at its place.
			samples.assign(size_t(frames) * kAudioChannels, 0.f);
		} else {
			samples = std::move(changed.samples);
		}
	}
	const auto total = int64(samples.size() / kAudioChannels);
	const auto frames = [&](crl::time duration) {
		return int64(std::llround(duration * (kAudioRate / 1000.) / _speed));
	};
	auto start = std::min(frames(before), total);
	const auto end = std::min(start + frames(till - from), total);
	const auto joined = std::min(
		int64(_tail.size() / kAudioChannels),
		end - start);
	if (joined > 0) {
		// Where the block goes on from the end of the previous one.
		const auto search = std::min({
			kAudioSearch,
			start,
			end - start - joined,
		});
		const auto score = [&](int64 shift) {
			const auto data = samples.data() + (start + shift) * kAudioChannels;
			auto product = 0.;
			auto energy = 0.;
			for (auto i = int64(0); i != joined * kAudioChannels; ++i) {
				product += double(_tail[i]) * data[i];
				energy += double(data[i]) * data[i];
			}
			return product / (std::sqrt(energy) + 1e-9);
		};
		auto best = int64(0);
		auto bestScore = score(0);
		for (auto shift = -search; shift <= search; ++shift) {
			const auto value = score(shift);
			if (value > bestScore + 1e-9) {
				bestScore = value;
				best = shift;
			}
		}
		start += best;
		for (auto i = int64(0); i != joined; ++i) {
			const auto ratio = float((i + 0.5) / joined);
			for (auto channel = 0; channel != kAudioChannels; ++channel) {
				auto &sample = samples[(start + i) * kAudioChannels + channel];
				sample = _tail[i * kAudioChannels + channel] * (1.f - ratio)
					+ sample * ratio;
			}
		}
	}
	const auto fade = (till < _till)
		? std::min({ kAudioFade, total - end, end - start })
		: int64(0);
	_tail.assign(
		begin(samples) + end * kAudioChannels,
		begin(samples) + (end + fade) * kAudioChannels);
	_queue.assign(
		begin(samples) + start * kAudioChannels,
		begin(samples) + end * kAudioChannels);
	_offset = 0;
}

// The size scaled down to fit the longer side.
[[nodiscard]] QSize ScaledDown(QSize size, int maxSide) {
	const auto longer = std::max(size.width(), size.height());
	if (longer <= 0) {
		return QSize();
	}
	const auto scale = (maxSide > 0 && longer > maxSide)
		? (maxSide / float64(longer))
		: 1.;
	return QSize(
		std::max(int(std::round(size.width() * scale)), 1),
		std::max(int(std::round(size.height() * scale)), 1));
}

// The same geometry as the video core uses for a sticker.
[[nodiscard]] QSize StickerContent(QSize crop, int side, bool emoji) {
	const auto longer = std::max(crop.width(), crop.height());
	const auto shorter = std::min(crop.width(), crop.height());
	if (longer <= 0 || shorter <= 0) {
		return QSize();
	}
	auto other = int(std::round(shorter * float64(side) / longer));
	if (!emoji) {
		other += (other & 1);
	}
	other = std::clamp(other, 2, side);
	return (crop.width() >= crop.height())
		? QSize(side, other)
		: QSize(other, side);
}

[[nodiscard]] int Mp4Bitrate(QSize size, int fps) {
	const auto pixels = int64(size.width()) * size.height() * fps;
	return int(std::clamp(
		(pixels * 15) / 100,
		int64(400'000),
		int64(12'000'000)));
}

} // namespace

QSize Rotated(QSize size, int rotation) {
	return (NormalizedRotation(rotation) % 180) ? size.transposed() : size;
}

QSize CanvasSize(const Project &project) {
	if (project.state.clips.empty()) {
		return QSize();
	}
	const auto source = ClipSource(project, project.state.clips.front());
	return source
		? Rotated(source->info.size, project.state.rotation)
		: QSize();
}

QRect CropRect(QSize canvas, QRectF crop) {
	if (canvas.isEmpty()) {
		return QRect();
	}
	const auto width = canvas.width();
	const auto height = canvas.height();
	const auto minWidth = std::min(width, 2);
	const auto minHeight = std::min(height, 2);
	auto left = int(std::round(crop.left() * width));
	auto top = int(std::round(crop.top() * height));
	auto right = int(std::round(crop.right() * width));
	auto bottom = int(std::round(crop.bottom() * height));
	left = std::clamp(left, 0, width - minWidth);
	top = std::clamp(top, 0, height - minHeight);
	right = std::clamp(right, left + minWidth, width);
	bottom = std::clamp(bottom, top + minHeight, height);
	return QRect(left, top, right - left, bottom - top);
}

QRectF FitRect(QSizeF size, QSizeF canvas) {
	const auto whole = QRectF(QPointF(), canvas);
	if (size.isEmpty() || canvas.isEmpty()) {
		return whole;
	}
	const auto scale = std::min(
		canvas.width() / size.width(),
		canvas.height() / size.height());
	const auto width = size.width() * scale;
	const auto height = size.height() * scale;

	// The same proportions with a rounding error take the whole canvas.
	if (std::abs(width - canvas.width()) <= 1.
		&& std::abs(height - canvas.height()) <= 1.) {
		return whole;
	}
	return QRectF(
		(canvas.width() - width) / 2.,
		(canvas.height() - height) / 2.,
		width,
		height);
}

float64 AspectRatio(Aspect aspect) {
	switch (aspect) {
	case Aspect::Square: return 1.;
	case Aspect::Portrait: return 4. / 5.;
	case Aspect::Story: return 9. / 16.;
	case Aspect::Wide: return 16. / 9.;
	case Aspect::Original:
	case Aspect::Free: break;
	}
	return 0.;
}

QRectF AspectCrop(QSize canvas, Aspect aspect, QRectF current) {
	const auto whole = QRectF(0., 0., 1., 1.);
	if (aspect == Aspect::Original || canvas.isEmpty()) {
		return whole;
	}
	current = current.intersected(whole);
	if (current.width() <= 0. || current.height() <= 0.) {
		current = whole;
	}
	const auto ratio = AspectRatio(aspect);
	if (ratio <= 0.) {
		return current;
	}
	const auto canvasRatio = canvas.width() / float64(canvas.height());
	const auto width = (canvasRatio > ratio) ? (ratio / canvasRatio) : 1.;
	const auto height = (canvasRatio > ratio) ? 1. : (canvasRatio / ratio);
	const auto center = current.center();
	return QRectF(
		std::clamp(center.x() - width / 2., 0., 1. - width),
		std::clamp(center.y() - height / 2., 0., 1. - height),
		width,
		height);
}

QRectF RotatedCrop(QRectF crop) {
	return QRectF(
		1. - crop.bottom(),
		crop.left(),
		crop.height(),
		crop.width());
}

crl::time SourceDuration(const State &state) {
	auto result = crl::time(0);
	for (const auto &clip : state.clips) {
		result += std::max(clip.length(), crl::time(0));
	}
	return result;
}

crl::time OutputDuration(const State &state) {
	return crl::time(std::llround(
		SourceDuration(state) / SpeedFactor(state)));
}

int UnusedSource(const Project &project, const std::vector<State> &history) {
	const auto count = int(project.sources.size());
	auto used = std::vector<bool>(count, false);
	const auto mark = [&](const State &state) {
		for (const auto &clip : state.clips) {
			if (clip.source >= 0 && clip.source < count) {
				used[clip.source] = true;
			}
		}
	};
	mark(project.state);
	for (const auto &state : history) {
		mark(state);
	}
	for (auto i = 0; i != count; ++i) {
		if (!used[i]) {
			return i;
		}
	}
	return -1;
}

Placement ComputePlacement(
		QSize source,
		QSize canvas,
		QRect crop,
		QSize output,
		int rotation) {
	rotation = NormalizedRotation(rotation);
	if (source.isEmpty()
		|| canvas.isEmpty()
		|| crop.isEmpty()
		|| output.isEmpty()) {
		return {};
	}
	const auto rotated = Rotated(source, rotation);
	const auto fitted = FitRect(QSizeF(rotated), QSizeF(canvas));
	const auto visible = fitted.intersected(QRectF(crop));
	if (visible.width() < 1. || visible.height() < 1.) {
		return {};
	}

	// The visible part in the result.
	const auto scaleX = output.width() / float64(crop.width());
	const auto scaleY = output.height() / float64(crop.height());
	const auto edge = [](float64 value, int max) {
		return std::clamp(int(std::round(value)), 0, max);
	};
	auto targetLeft = edge(
		(visible.left() - crop.left()) * scaleX,
		output.width() - 1);
	auto targetTop = edge(
		(visible.top() - crop.top()) * scaleY,
		output.height() - 1);
	auto targetRight = std::max(
		edge((visible.right() - crop.left()) * scaleX, output.width()),
		targetLeft + 1);
	auto targetBottom = std::max(
		edge((visible.bottom() - crop.top()) * scaleY, output.height()),
		targetTop + 1);
	const auto target = QRect(
		targetLeft,
		targetTop,
		targetRight - targetLeft,
		targetBottom - targetTop);

	// The visible part in the rotated frame of the source.
	const auto ratioX = rotated.width() / fitted.width();
	const auto ratioY = rotated.height() / fitted.height();
	const auto left = edge(
		(visible.left() - fitted.left()) * ratioX,
		rotated.width() - 1);
	const auto top = edge(
		(visible.top() - fitted.top()) * ratioY,
		rotated.height() - 1);
	const auto right = std::max(
		edge((visible.right() - fitted.left()) * ratioX, rotated.width()),
		left + 1);
	const auto bottom = std::max(
		edge((visible.bottom() - fitted.top()) * ratioY, rotated.height()),
		top + 1);
	const auto width = right - left;
	const auto height = bottom - top;

	// And before the rotation: QTransform::rotate() turns clockwise.
	auto part = QRect();
	switch (rotation) {
	case 90:
		part = QRect(top, source.height() - right, height, width);
		break;
	case 180:
		part = QRect(
			source.width() - right,
			source.height() - bottom,
			width,
			height);
		break;
	case 270:
		part = QRect(source.width() - bottom, left, height, width);
		break;
	default:
		part = QRect(left, top, width, height);
		break;
	}
	return {
		.source = part.intersected(QRect(QPoint(), source)),
		.read = Rotated(target.size(), rotation),
		.target = target,
	};
}

QString FormatExtension(Format format) {
	switch (format) {
	case Format::Mp4:
	case Format::GifVideo: return u"mp4"_q;
	case Format::Gif: return u"gif"_q;
	case Format::Sticker:
	case Format::Emoji: return u"webm"_q;
	}
	return u"mp4"_q;
}

QSize PreviewSize(QSize crop, int maxSide) {
	return ScaledDown(crop, maxSide);
}

QImage ComposePreview(
		const QImage &frame,
		QSize canvas,
		QRect crop,
		QSize output,
		int rotation) {
	if (output.isEmpty()) {
		return QImage();
	}
	auto result = Filled(output, QRgb(0xFF000000U));
	if (frame.isNull() || canvas.isEmpty() || crop.isEmpty()) {
		return result;
	}
	rotation = NormalizedRotation(rotation);
	const auto rotated = rotation
		? frame.transformed(QTransform().rotate(rotation))
		: frame;

	// Where the frame is in the canvas, then in the result.
	const auto fitted = FitRect(QSizeF(rotated.size()), QSizeF(canvas));
	const auto scaleX = output.width() / float64(crop.width());
	const auto scaleY = output.height() / float64(crop.height());
	const auto target = QRectF(
		(fitted.x() - crop.x()) * scaleX,
		(fitted.y() - crop.y()) * scaleY,
		fitted.width() * scaleX,
		fitted.height() * scaleY);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.drawImage(target, rotated);
	return result;
}

int ExportMaxSide(const ExportOptions &options) {
	switch (options.format) {
	case Format::Sticker: return VideoCore::kStickerSide;
	case Format::Emoji: return VideoCore::kEmojiSide;
	case Format::Mp4:
		return (options.maxSide > 0)
			? options.maxSide
			: VideoCore::kVideoMaxSide;
	case Format::GifVideo:
		return (options.maxSide > 0)
			? options.maxSide
			: VideoCore::kGifVideoMaxSide;
	case Format::Gif:
		return (options.maxSide > 0)
			? options.maxSide
			: VideoCore::kGifMaxSide;
	}
	return VideoCore::kVideoMaxSide;
}

int ExportMaxFps(const ExportOptions &options) {
	switch (options.format) {
	case Format::Sticker:
	case Format::Emoji: return VideoCore::kStickerMaxFps;
	case Format::Mp4:
		return (options.maxFps > 0)
			? options.maxFps
			: VideoCore::kVideoMaxFps;
	case Format::GifVideo:
		return (options.maxFps > 0)
			? options.maxFps
			: VideoCore::kGifVideoMaxFps;
	case Format::Gif:
		return (options.maxFps > 0)
			? options.maxFps
			: VideoCore::kGifMaxFps;
	}
	return VideoCore::kVideoMaxFps;
}

QSize OutputSize(const Project &project, const ExportOptions &options) {
	const auto canvas = CanvasSize(project);
	if (canvas.isEmpty()) {
		return QSize();
	}
	const auto crop = CropRect(canvas, project.state.crop).size();
	const auto side = ExportMaxSide(options);
	if (IsSticker(options.format)) {
		return StickerContent(
			crop,
			side,
			(options.format == Format::Emoji));
	}
	const auto scaled = ScaledDown(crop, side);
	if (!IsMp4(options.format)) {
		return scaled;
	}
	return QSize(
		std::max(scaled.width() & ~1, kMp4MinSide),
		std::max(scaled.height() & ~1, kMp4MinSide));
}

bool OutputHasAudio(const Project &project, const ExportOptions &options) {
	if (options.format != Format::Mp4 || project.state.mute) {
		return false;
	}
	for (const auto &clip : project.state.clips) {
		const auto source = ClipSource(project, clip);
		if (source && source->info.hasAudio) {
			return true;
		}
	}
	return false;
}

int64 EstimateSize(const Project &project, const ExportOptions &options) {
	if (!IsMp4(options.format)) {
		return 0;
	}
	const auto size = OutputSize(project, options);
	if (size.isEmpty()) {
		return 0;
	}
	const auto fps = std::clamp(
		int(std::round(ResultFps(project))),
		1,
		ExportMaxFps(options));
	const auto bitrate = int64(Mp4Bitrate(size, fps))
		+ (OutputHasAudio(project, options) ? kMp4AudioBitrate : 0);
	return bitrate * OutputDuration(project.state) / 8000;
}

ExportResult Export(
		const Project &project,
		const ExportOptions &options,
		Fn<void(float64)> progress,
		VideoCore::Cancel cancel) {
	auto result = ExportResult();
	const auto &state = project.state;
	if (state.clips.empty()) {
		result.error = u"No clips."_q;
		return result;
	}
	for (const auto &clip : state.clips) {
		const auto source = ClipSource(project, clip);
		if (!source || !source->info.valid() || clip.till <= clip.from) {
			result.error = u"Bad clip."_q;
			return result;
		}
	}
	const auto format = options.format;
	const auto sticker = IsSticker(format);
	const auto canvas = CanvasSize(project);
	const auto crop = CropRect(canvas, state.crop);
	const auto output = OutputSize(project, options);
	if (output.isEmpty()) {
		result.error = u"Bad size."_q;
		return result;
	}
	const auto speed = SpeedFactor(state);
	const auto rotation = NormalizedRotation(state.rotation);
	const auto whole = OutputDuration(state);
	const auto total = std::max(
		sticker ? std::min(whole, VideoCore::kStickerMaxDuration) : whole,
		crl::time(1));
	if (!sticker
		&& (whole > ((format == Format::Gif)
			? kMaxGifDuration
			: kMaxVideoDuration))) {
		result.error = u"Too long."_q;
		return result;
	}
	const auto maxFps = ExportMaxFps(options);
	const auto audio = OutputHasAudio(project, options);
	const auto fill = sticker ? QRgb(0) : QRgb(0xFF000000U);
	const auto resultFps = std::clamp(
		int(std::round(ResultFps(project))),
		1,
		maxFps);

	auto mp4 = std::unique_ptr<VideoCore::Mp4Encoder>();
	auto gif = std::unique_ptr<VideoCore::GifEncoder>();
	auto grid = std::vector<QImage>(); // A sticker: frames at a fixed rate.
	const auto gridFps = (resultFps >= 28) ? 30 : resultFps;
	const auto gridStep = 1000. / gridFps;
	if (IsMp4(format)) {
		mp4 = std::make_unique<VideoCore::Mp4Encoder>(
			output,
			VideoCore::Mp4Options{
				.fps = resultFps,
				.audioRate = audio ? kAudioRate : 0,
				.audioChannels = audio ? kAudioChannels : 0,
			});
		if (!mp4->valid()) {
			result.error = u"Could not create the H.264 encoder."_q;
			return result;
		}
	} else if (format == Format::Gif) {
		gif = std::make_unique<VideoCore::GifEncoder>(output);
	}

	// One for the whole result: the effects that remember the frames go
	// on over the joints of the clips.
	const auto effects = VideoFx::HasEnabled(state.fx);
	VideoFx::Processor processor(effects ? state.fx : VideoFx::Stack());

	const auto readShare = sticker ? kStickerReadShare : 1.;
	auto outEnd = crl::time(0); // The end of the last frame of the result.
	auto audioGiven = int64(0);
	auto samples = std::vector<float>();
	auto failed = false;
	auto full = false;
	for (const auto &clip : state.clips) {
		const auto &source = *ClipSource(project, clip);
		const auto placement = ComputePlacement(
			source.info.size,
			canvas,
			crop,
			output,
			rotation);

		// A clip that is not seen at all still takes its time.
		const auto visible = !placement.empty();
		const auto sourceFps = source.info.fps;
		const auto keepRate = (sourceFps > 0.)
			&& (sourceFps * speed <= maxFps * kKeptRateExcess);
		const auto readFps = keepRate
			? 0
			: std::max(int(std::floor(maxFps / speed)), 1);
		const auto clipStart = outEnd;
		auto feeder = audio
			? std::make_unique<ClipAudio>(source, clip, speed)
			: nullptr;
		const auto read = VideoCore::ReadFrames({
			.path = source.path,
			.content = source.content,
			.from = clip.from,
			.till = clip.till,
			.crop = visible ? placement.source : QRect(),
			.size = visible ? placement.read : QSize(2, 2),
			.maxFps = readFps,
		}, [&](VideoCore::Frame &&frame) {
			const auto end = clipStart + crl::time(std::llround(
				(frame.position + frame.duration) / speed));
			const auto duration = end - outEnd;
			if (duration <= 0) {
				return true;
			}
			auto image = Compose(
				std::move(frame.image),
				placement,
				output,
				rotation,
				fill);
			if (image.isNull()) {
				failed = true;
				return false;
			}
			if (effects) {
				// outEnd is still the time this frame starts at.
				image = processor.process(std::move(image), outEnd);
				if (image.isNull()) {
					failed = true;
					return false;
				}
			}
			if (mp4) {
				if (!mp4->add(image, duration)) {
					failed = true;
					return false;
				}
			} else if (gif) {
				gif->add(image, duration);
			} else {
				while (grid.size() * gridStep < end) {
					if (grid.size() * gridStep
						>= VideoCore::kStickerMaxDuration) {
						full = true;
						break;
					}
					grid.push_back(image);
				}
			}
			outEnd = end;
			++result.frames;
			if (feeder) {
				// A frame may be shown for minutes: its sound goes by
				// pieces of a second, so it can be cancelled in between.
				const auto target = outEnd * int64(kAudioRate) / 1000;
				while (audioGiven < target) {
					if (cancel && cancel->load()) {
						return false;
					}
					const auto piece = std::min(
						target - audioGiven,
						int64(kAudioRate));
					samples.clear();
					feeder->read(piece, samples);
					audioGiven += piece;
					if (!samples.empty()
						&& !mp4->addAudio(
							samples.data(),
							int64(samples.size() / kAudioChannels))) {
						failed = true;
						return false;
					}
				}
			}
			if (progress) {
				progress(readShare
					* std::clamp(outEnd / float64(total), 0., 1.));
			}
			return !full;
		}, cancel, &result.error);
		if (cancel && cancel->load()) {
			result.cancelled = true;
			result.error = u"Cancelled."_q;
			return result;
		} else if (!read) {
			return result;
		} else if (failed) {
			result.error = u"Could not encode a frame."_q;
			return result;
		} else if (full) {
			break;
		}
	}

	if (sticker) {
		const auto step = std::max(
			crl::time(std::llround(gridStep)),
			crl::time(1));
		auto made = VideoCore::EncodeVideoSticker(
			grid,
			step,
			ExportMaxSide(options),
			progress ? [&](float64 value) {
				progress(readShare + (1. - readShare) * value);
			} : Fn<void(float64)>(),
			cancel);
		result.cancelled = cancel && cancel->load();
		result.content = std::move(made.webm);
		result.ok = made.ok && !result.cancelled;
		result.error = made.error;
		result.size = made.size;
		result.duration = made.duration;
		result.frames = made.frames;
		return result;
	}
	result.size = output;
	result.duration = outEnd;
	result.content = mp4 ? mp4->finish() : gif->finish();
	result.ok = !result.content.isEmpty();
	result.audio = result.ok && audio;
	if (!result.ok) {
		result.error = u"Could not write the file."_q;
	}
	return result;
}

// Self-test.

namespace {

constexpr auto kTestSize = QSize(160, 120);
constexpr auto kTestDuration = crl::time(1000);
constexpr auto kTestFps = 20;
constexpr auto kTestTolerance = 48;

// Top left, top right, bottom left, bottom right.
constexpr auto kTestColors = std::array<QRgb, 4>{ {
	QRgb(0xFFDC2828U),
	QRgb(0xFF28B43CU),
	QRgb(0xFF283CDCU),
	QRgb(0xFFE6D232U),
} };

[[nodiscard]] QImage TestQuadrants(QSize size) {
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	const auto half = QSize(size.width() / 2, size.height() / 2);
	auto p = QPainter(&result);
	p.fillRect(0, 0, half.width(), half.height(), QColor(kTestColors[0]));
	p.fillRect(
		half.width(),
		0,
		size.width() - half.width(),
		half.height(),
		QColor(kTestColors[1]));
	p.fillRect(
		0,
		half.height(),
		half.width(),
		size.height() - half.height(),
		QColor(kTestColors[2]));
	p.fillRect(
		half.width(),
		half.height(),
		size.width() - half.width(),
		size.height() - half.height(),
		QColor(kTestColors[3]));
	return result;
}

// Quadrants and a 440 Hz tone, in the first half of the video only or all
// the way.
[[nodiscard]] QByteArray TestVideo(
		QSize size,
		bool audio,
		crl::time duration = kTestDuration,
		bool wholeTone = false) {
	auto encoder = VideoCore::Mp4Encoder(size, {
		.fps = kTestFps,
		.audioRate = audio ? kAudioRate : 0,
		.audioChannels = audio ? kAudioChannels : 0,
	});
	if (!encoder.valid()) {
		return QByteArray();
	}
	const auto image = TestQuadrants(size);
	const auto frames = int(duration * kTestFps / 1000);
	const auto step = crl::time(1000 / kTestFps);
	const auto perFrame = kAudioRate / kTestFps;
	auto samples = std::vector<float>(size_t(perFrame) * kAudioChannels);
	auto phase = int64(0);
	for (auto i = 0; i != frames; ++i) {
		if (!encoder.add(image, step)) {
			return QByteArray();
		}
		if (audio) {
			const auto loud = wholeTone || (i < frames / 2);
			for (auto j = 0; j != perFrame; ++j, ++phase) {
				const auto value = loud
					? float(0.5 * std::sin(
						2. * M_PI * 440. * phase / kAudioRate))
					: 0.f;
				samples[j * 2] = samples[j * 2 + 1] = value;
			}
			if (!encoder.addAudio(samples.data(), perFrame)) {
				return QByteArray();
			}
		}
	}
	return encoder.finish();
}

[[nodiscard]] bool TestNear(QRgb a, QRgb b) {
	return (std::abs(qRed(a) - qRed(b)) <= kTestTolerance)
		&& (std::abs(qGreen(a) - qGreen(b)) <= kTestTolerance)
		&& (std::abs(qBlue(a) - qBlue(b)) <= kTestTolerance);
}

[[nodiscard]] QString TestColor(QRgb color) {
	return u"#%1"_q.arg(color & 0xFFFFFFU, 6, 16, QChar('0'));
}

// The middles of the four quarters of the first frame of a result.
[[nodiscard]] std::array<QRgb, 4> TestCorners(const QByteArray &content) {
	const auto frame = VideoCore::ReadFrame({ .content = content });
	if (frame.isNull()) {
		return {};
	}
	const auto w = frame.width();
	const auto h = frame.height();
	return { {
		frame.pixel(w / 4, h / 4),
		frame.pixel(w * 3 / 4, h / 4),
		frame.pixel(w / 4, h * 3 / 4),
		frame.pixel(w * 3 / 4, h * 3 / 4),
	} };
}

[[nodiscard]] float64 TestLevel(
		const std::vector<float> &samples,
		float64 from,
		float64 till) {
	const auto frames = int64(samples.size() / kAudioChannels);
	const auto begin = int64(frames * from);
	const auto end = int64(frames * till);
	if (end <= begin) {
		return 0.;
	}
	auto sum = 0.;
	for (auto i = begin; i != end; ++i) {
		const auto value = samples[i * kAudioChannels];
		sum += value * value;
	}
	return std::sqrt(sum / (end - begin));
}

} // namespace

bool RunSelfTest(QStringList &log) {
	auto passed = true;
	const auto check = [&](bool ok, const QString &name, const QString &info) {
		log.push_back((ok ? u"ok   "_q : u"FAIL "_q)
			+ name
			+ (info.isEmpty() ? QString() : (u": "_q + info)));
		passed = passed && ok;
	};
	const auto rect = [](QRect r) {
		return u"%1,%2 %3x%4"_q
			.arg(r.x())
			.arg(r.y())
			.arg(r.width())
			.arg(r.height());
	};
	const auto size = [](QSize s) {
		return u"%1x%2"_q.arg(s.width()).arg(s.height());
	};
	const auto colors = [](const std::array<QRgb, 4> &list) {
		return TestColor(list[0])
			+ ' '
			+ TestColor(list[1])
			+ ' '
			+ TestColor(list[2])
			+ ' '
			+ TestColor(list[3]);
	};
	// Not "near": that is an empty macro in the Windows headers.
	const auto nearly = [](QRectF a, QRectF b) {
		return (std::abs(a.x() - b.x()) < 1e-6)
			&& (std::abs(a.y() - b.y()) < 1e-6)
			&& (std::abs(a.width() - b.width()) < 1e-6)
			&& (std::abs(a.height() - b.height()) < 1e-6);
	};

	// Geometry.
	{
		check(
			Rotated(QSize(160, 120), 90) == QSize(120, 160)
				&& Rotated(QSize(160, 120), 180) == QSize(160, 120)
				&& Rotated(QSize(160, 120), 270) == QSize(120, 160)
				&& Rotated(QSize(160, 120), 360) == QSize(160, 120),
			u"rotated size"_q,
			QString());

		const auto crop = CropRect(
			QSize(1920, 1080),
			QRectF(0.25, 0., 0.5, 1.));
		check(
			crop == QRect(480, 0, 960, 1080),
			u"crop rect"_q,
			rect(crop));
		const auto tiny = CropRect(QSize(1920, 1080), QRectF(2., 2., 0., 0.));
		check(
			tiny.width() == 2
				&& tiny.height() == 2
				&& QRect(0, 0, 1920, 1080).contains(tiny),
			u"crop rect is never empty"_q,
			rect(tiny));

		const auto story = AspectCrop(
			QSize(1920, 1080),
			Aspect::Story,
			QRectF(0., 0., 1., 1.));
		const auto storyRect = CropRect(QSize(1920, 1080), story);
		check(
			storyRect.height() == 1080
				&& std::abs(storyRect.width() - 608) <= 1
				&& std::abs(storyRect.center().x() - 960) <= 1,
			u"9:16 crop of 16:9"_q,
			rect(storyRect));
		const auto wide = AspectCrop(
			QSize(1080, 1920),
			Aspect::Wide,
			QRectF(0., 0.6, 1., 0.4));
		const auto wideRect = CropRect(QSize(1080, 1920), wide);
		check(
			wideRect.width() == 1080
				&& std::abs(wideRect.height() - 608) <= 1
				&& wideRect.bottom() <= 1920
				&& wideRect.center().y() > 1300,
			u"16:9 crop of 9:16 keeps the place"_q,
			rect(wideRect));
		check(
			nearly(
				AspectCrop(QSize(100, 100), Aspect::Original, story),
				QRectF(0., 0., 1., 1.))
				&& nearly(
					AspectCrop(QSize(100, 100), Aspect::Free, story),
					story),
			u"original and free crops"_q,
			QString());

		auto turned = QRectF(0.1, 0.2, 0.3, 0.4);
		const auto once = RotatedCrop(turned);
		for (auto i = 0; i != 4; ++i) {
			turned = RotatedCrop(turned);
		}
		check(
			nearly(once, QRectF(0.4, 0.1, 0.4, 0.3))
				&& nearly(turned, QRectF(0.1, 0.2, 0.3, 0.4)),
			u"rotated crop"_q,
			u"%1,%2 %3x%4"_q
				.arg(once.x())
				.arg(once.y())
				.arg(once.width())
				.arg(once.height()));

		const auto fit = FitRect(QSizeF(1080, 1920), QSizeF(1920, 1080));
		check(
			std::abs(fit.width() - 607.5) < 0.01
				&& std::abs(fit.height() - 1080.) < 0.01
				&& std::abs(fit.x() - 656.25) < 0.01,
			u"fit rect"_q,
			u"%1,%2 %3x%4"_q
				.arg(fit.x())
				.arg(fit.y())
				.arg(fit.width())
				.arg(fit.height()));
		check(
			FitRect(QSizeF(1280, 720), QSizeF(1920, 1080))
				== QRectF(0, 0, 1920, 1080),
			u"fit rect of the same proportions"_q,
			QString());

		const auto plain = ComputePlacement(
			QSize(1920, 1080),
			QSize(1920, 1080),
			QRect(0, 0, 1920, 1080),
			QSize(1280, 720),
			0);
		check(
			plain.source == QRect(0, 0, 1920, 1080)
				&& plain.read == QSize(1280, 720)
				&& plain.target == QRect(0, 0, 1280, 720),
			u"placement of the whole frame"_q,
			rect(plain.source) + u" -> "_q + rect(plain.target));

		// The right half of the turned frame is the bottom half of
		// the source.
		const auto turnedHalf = ComputePlacement(
			QSize(1920, 1080),
			QSize(1080, 1920),
			QRect(0, 0, 540, 1920),
			QSize(540, 1920),
			90);
		check(
			turnedHalf.source == QRect(0, 540, 1920, 540)
				&& turnedHalf.read == QSize(1920, 540)
				&& turnedHalf.target == QRect(0, 0, 540, 1920),
			u"placement after 90 degrees"_q,
			rect(turnedHalf.source) + u" -> "_q + rect(turnedHalf.target));
		const auto back = ComputePlacement(
			QSize(1920, 1080),
			QSize(1080, 1920),
			QRect(0, 0, 540, 1920),
			QSize(540, 1920),
			270);
		check(
			back.source == QRect(0, 0, 1920, 540),
			u"placement after 270 degrees"_q,
			rect(back.source));
		const auto flipped = ComputePlacement(
			QSize(1920, 1080),
			QSize(1920, 1080),
			QRect(0, 0, 960, 540),
			QSize(960, 540),
			180);
		check(
			flipped.source == QRect(960, 540, 960, 540),
			u"placement after 180 degrees"_q,
			rect(flipped.source));

		// A portrait clip in a landscape canvas: bars on the sides.
		const auto bars = ComputePlacement(
			QSize(1080, 1920),
			QSize(1920, 1080),
			QRect(0, 0, 1920, 1080),
			QSize(1920, 1080),
			0);
		check(
			bars.source == QRect(0, 0, 1080, 1920)
				&& std::abs(bars.target.x() - 656) <= 1
				&& std::abs(bars.target.width() - 608) <= 1
				&& bars.target.height() == 1080,
			u"placement with bars"_q,
			rect(bars.source) + u" -> "_q + rect(bars.target));
		const auto hidden = ComputePlacement(
			QSize(1080, 1920),
			QSize(1920, 1080),
			QRect(0, 0, 400, 1080),
			QSize(400, 1080),
			0);
		check(hidden.empty(), u"placement out of the crop"_q, QString());
	}

	// Sources.
	{
		const auto clip = [](int source) {
			return Clip{ source, 0, 1000 };
		};
		auto sample = Project{ .sources = std::vector<Source>(3) };
		sample.state.clips = { clip(0), clip(2), clip(7) };
		const auto unused = UnusedSource(sample, {});
		const auto kept = UnusedSource(
			sample,
			{ State(), State{ .clips = { clip(1) } } });
		sample.state.clips = { clip(2) };
		const auto first = UnusedSource(sample, { State() });
		const auto none = UnusedSource(Project(), {});
		check(
			(unused == 1) && (kept == -1) && (first == 0) && (none == -1),
			u"unused source"_q,
			u"%1 %2 %3 %4"_q.arg(unused).arg(kept).arg(first).arg(none));
	}

	// Export.
	const auto video = TestVideo(kTestSize, true);
	const auto silent = TestVideo(QSize(120, 160), false);
	auto info = VideoCore::ClipInfo();
	auto silentInfo = VideoCore::ClipInfo();
	if (!video.isEmpty()) {
		info = VideoCore::ReadClipInfo(QString(), video);
	}
	if (!silent.isEmpty()) {
		silentInfo = VideoCore::ReadClipInfo(QString(), silent);
	}
	check(
		info.valid()
			&& info.hasAudio
			&& info.size == kTestSize
			&& silentInfo.valid()
			&& !silentInfo.hasAudio,
		u"test videos"_q,
		u"%1 ms, %2, %3 bytes"_q
			.arg(info.duration)
			.arg(size(info.size))
			.arg(video.size()));
	if (!info.valid() || !silentInfo.valid()) {
		return false;
	}
	const auto project = [&](State state) {
		if (state.clips.empty()) {
			state.clips.push_back({ 0, 0, info.duration });
		}
		return Project{
			.sources = {
				Source{ .content = video, .info = info },
				Source{ .content = silent, .info = silentInfo },
			},
			.state = std::move(state),
		};
	};
	const auto run = [&](State state, ExportOptions options = {}) {
		return Export(project(std::move(state)), options, nullptr, nullptr);
	};
	const auto timed = [&](const ExportResult &result, crl::time duration) {
		const auto read = VideoCore::ReadClipInfo(QString(), result.content);
		return result.ok
			&& (std::abs(result.duration - duration) <= 60)
			&& (std::abs(read.duration - duration) <= 110);
	};
	const auto describe = [&](const ExportResult &result) {
		return result.ok
			? u"%1 ms, %2, %3 frames, %4 bytes%5"_q
				.arg(result.duration)
				.arg(size(result.size))
				.arg(result.frames)
				.arg(result.content.size())
				.arg(result.audio ? u", audio"_q : QString())
			: (u"error: "_q + result.error);
	};
	{
		const auto started = crl::now();
		const auto result = run({});
		const auto corners = TestCorners(result.content);
		const auto read = VideoCore::ReadClipInfo(QString(), result.content);
		check(
			timed(result, kTestDuration)
				&& result.size == kTestSize
				&& result.audio
				&& read.hasAudio
				&& read.size == kTestSize
				&& TestNear(corners[0], kTestColors[0])
				&& TestNear(corners[1], kTestColors[1])
				&& TestNear(corners[2], kTestColors[2])
				&& TestNear(corners[3], kTestColors[3]),
			u"mp4 as it is"_q,
			describe(result)
				+ u", "_q
				+ colors(corners)
				+ u", %1 ms"_q.arg(crl::now() - started));
	}
	{
		const auto result = run({ .rotation = 90 });
		const auto corners = TestCorners(result.content);
		check(
			result.ok
				&& result.size == kTestSize.transposed()
				&& TestNear(corners[0], kTestColors[2])
				&& TestNear(corners[1], kTestColors[0])
				&& TestNear(corners[2], kTestColors[3])
				&& TestNear(corners[3], kTestColors[1]),
			u"mp4 turned by 90 degrees"_q,
			describe(result) + u", "_q + colors(corners));
	}
	{
		const auto result = run({ .rotation = 180 });
		const auto corners = TestCorners(result.content);
		check(
			result.ok
				&& result.size == kTestSize
				&& TestNear(corners[0], kTestColors[3])
				&& TestNear(corners[1], kTestColors[2])
				&& TestNear(corners[2], kTestColors[1])
				&& TestNear(corners[3], kTestColors[0]),
			u"mp4 turned by 180 degrees"_q,
			describe(result) + u", "_q + colors(corners));
	}
	{
		// The left half of the frame turned by 270 degrees is the top
		// half of the source.
		const auto result = run({
			.crop = QRectF(0., 0., 0.5, 1.),
			.aspect = Aspect::Free,
			.rotation = 270,
		});
		const auto corners = TestCorners(result.content);
		check(
			result.ok
				&& result.size == QSize(60, 160)
				&& TestNear(corners[0], kTestColors[1])
				&& TestNear(corners[1], kTestColors[1])
				&& TestNear(corners[2], kTestColors[0])
				&& TestNear(corners[3], kTestColors[0]),
			u"mp4 turned by 270 degrees and cropped"_q,
			describe(result) + u", "_q + colors(corners));
	}
	{
		const auto result = run({
			.crop = QRectF(0.5, 0., 0.5, 1.),
			.aspect = Aspect::Free,
		});
		const auto corners = TestCorners(result.content);
		check(
			result.ok
				&& result.size == QSize(80, 120)
				&& TestNear(corners[0], kTestColors[1])
				&& TestNear(corners[1], kTestColors[1])
				&& TestNear(corners[2], kTestColors[3])
				&& TestNear(corners[3], kTestColors[3]),
			u"mp4 cropped"_q,
			describe(result) + u", "_q + colors(corners));
	}
	for (const auto speed : { 200, 50, 150 }) {
		const auto result = run({ .speed = speed });
		const auto expected = kTestDuration * 100 / speed;
		const auto samples = VideoCore::ReadAudio({
			.content = result.content,
		});
		const auto loud = TestLevel(samples, 0.1, 0.4);
		const auto quiet = TestLevel(samples, 0.65, 0.95);
		check(
			timed(result, expected)
				&& result.audio
				&& (std::abs(int64(samples.size() / kAudioChannels)
					- expected * kAudioRate / 1000) < kAudioRate / 8)
				&& loud > 0.15
				&& quiet < 0.02,
			u"mp4 at %1%"_q.arg(speed),
			describe(result)
				+ u", sound %1 then %2"_q
					.arg(loud, 0, 'f', 3)
					.arg(quiet, 0, 'f', 3));
	}
	{
		// The second half, then the first one: the tone moves to the end.
		const auto half = info.duration / 2;
		const auto result = run({
			.clips = {
				{ 0, half, info.duration },
				{ 0, 0, half },
			},
		});
		const auto samples = VideoCore::ReadAudio({
			.content = result.content,
		});
		const auto first = TestLevel(samples, 0.1, 0.4);
		const auto second = TestLevel(samples, 0.6, 0.9);
		check(
			timed(result, kTestDuration) && first < 0.02 && second > 0.15,
			u"mp4 of two reordered clips"_q,
			describe(result)
				+ u", sound %1 then %2"_q
					.arg(first, 0, 'f', 3)
					.arg(second, 0, 'f', 3));
	}
	{
		// A portrait clip without sound after the landscape one: bars on
		// the sides and silence.
		const auto result = run({
			.clips = {
				{ 0, 0, info.duration / 2 },
				{ 1, 0, silentInfo.duration },
			},
		});
		const auto expected = info.duration / 2 + silentInfo.duration;
		const auto last = VideoCore::ReadFrame({
			.content = result.content,
			.from = expected - 200,
		});
		const auto samples = VideoCore::ReadAudio({
			.content = result.content,
		});
		const auto ok = !last.isNull()
			&& TestNear(last.pixel(8, 60), QRgb(0xFF000000U))
			&& TestNear(last.pixel(152, 60), QRgb(0xFF000000U))
			&& TestNear(last.pixel(60, 30), kTestColors[0])
			&& TestNear(last.pixel(100, 90), kTestColors[3]);
		check(
			timed(result, expected)
				&& result.size == kTestSize
				&& result.audio
				&& ok
				&& TestLevel(samples, 0.05, 0.25) > 0.15
				&& TestLevel(samples, 0.5, 0.95) < 0.02,
			u"mp4 of two different videos"_q,
			describe(result)
				+ (last.isNull()
					? QString()
					: (u", "_q
						+ TestColor(last.pixel(8, 60))
						+ ' '
						+ TestColor(last.pixel(60, 30))
						+ ' '
						+ TestColor(last.pixel(100, 90)))));
	}
	{
		// Longer than a block of the audio: the blocks are joined without
		// clicks and gaps, the sound stays as long as the video is.
		const auto length = kAudioBlock + 2 * kAudioBlockRest;
		const auto longVideo = TestVideo(QSize(64, 48), true, length, true);
		const auto longInfo = VideoCore::ReadClipInfo(QString(), longVideo);
		for (const auto speed : { 100, 150 }) {
			const auto started = crl::now();
			const auto result = Export({
				.sources = { Source{ .content = longVideo, .info = longInfo } },
				.state = {
					.clips = { { 0, 500, longInfo.duration - 500 } },
					.speed = speed,
				},
			}, {}, nullptr, nullptr);
			const auto samples = VideoCore::ReadAudio({
				.content = result.content,
			});
			const auto window = kAudioRate / 100;
			const auto count = int64(samples.size() / kAudioChannels) / window;
			auto lowest = 1.;
			auto highest = 0.;
			for (auto i = int64(10); i + 10 < count; ++i) {
				auto sum = 0.;
				for (auto j = int64(0); j != window; ++j) {
					const auto index = (i * window + j) * kAudioChannels;
					const auto value = samples[index];
					sum += value * value;
				}
				const auto level = std::sqrt(sum / window);
				lowest = std::min(lowest, level);
				highest = std::max(highest, level);
			}
			const auto expected = (longInfo.duration - 1000) * 100 / speed;
			check(
				longInfo.valid()
					&& result.ok
					&& result.audio
					&& std::abs(result.duration - expected) <= 60
					&& count > 100
					&& lowest > 0.25
					&& highest < 0.45,
				u"sound of a long clip at %1%"_q.arg(speed),
				describe(result)
					+ u", level %1..%2, %3 ms"_q
						.arg(lowest, 0, 'f', 3)
						.arg(highest, 0, 'f', 3)
						.arg(crl::now() - started));
		}
	}
	{
		const auto result = run({ .mute = true });
		const auto read = VideoCore::ReadClipInfo(QString(), result.content);
		check(
			result.ok && !result.audio && !read.hasAudio,
			u"mp4 without sound"_q,
			describe(result));
	}
	{
		const auto result = run({}, { .format = Format::GifVideo });
		const auto read = VideoCore::ReadClipInfo(QString(), result.content);
		check(
			timed(result, kTestDuration) && !result.audio && !read.hasAudio,
			u"Telegram GIF"_q,
			describe(result));
	}
	{
		const auto result = run(
			{ .rotation = 90 },
			{ .format = Format::Gif, .maxSide = 80 });
		const auto read = VideoCore::ReadClipInfo(QString(), result.content);
		const auto corners = TestCorners(result.content);
		check(
			result.ok
				&& result.content.startsWith("GIF89a")
				&& result.size == QSize(60, 80)
				&& read.size == QSize(60, 80)
				&& std::abs(read.duration - kTestDuration) <= 110
				&& TestNear(corners[0], kTestColors[2])
				&& TestNear(corners[3], kTestColors[1]),
			u"GIF file"_q,
			describe(result) + u", "_q + colors(corners));
	}
	{
		const auto started = crl::now();
		const auto result = run(
			{ .speed = 50 },
			{ .format = Format::Sticker });
		const auto read = VideoCore::ReadClipInfo(QString(), result.content);
		check(
			result.ok
				&& result.size == QSize(512, 384)
				&& read.size == QSize(512, 384)
				&& read.codec == u"vp9"_q
				&& !read.hasAudio
				&& result.content.size() <= VideoCore::kStickerMaxBytes
				&& std::abs(result.duration - 2000) <= 110,
			u"video sticker"_q,
			describe(result) + u", %1 ms"_q.arg(crl::now() - started));
	}
	{
		// Four seconds of the result: a sticker takes the first three.
		const auto result = run({
			.clips = {
				{ 0, 0, info.duration },
				{ 0, 0, info.duration },
			},
			.crop = QRectF(0., 0., 0.5, 1.),
			.aspect = Aspect::Free,
			.speed = 50,
		}, { .format = Format::Emoji });
		const auto read = VideoCore::ReadClipInfo(QString(), result.content);
		check(
			result.ok
				&& read.size == QSize(100, 100)
				&& result.content.size() <= VideoCore::kEmojiMaxBytes
				&& result.duration <= VideoCore::kStickerMaxDuration
				&& result.duration >= 2800,
			u"video emoji"_q,
			describe(result));
	}
	{
		const auto cancel = std::make_shared<std::atomic<bool>>(true);
		const auto result = Export(project({}), {}, nullptr, cancel);
		check(
			!result.ok && result.cancelled && result.content.isEmpty(),
			u"cancelled"_q,
			result.error);
	}
	{
		const auto result = Export(Project(), {}, nullptr, nullptr);
		auto broken = project({});
		broken.state.clips = { { 5, 0, 100 } };
		const auto bad = Export(broken, {}, nullptr, nullptr);
		check(
			!result.ok && !bad.ok && !result.cancelled,
			u"errors"_q,
			result.error + ' ' + bad.error);
	}
	{
		auto calls = 0;
		auto last = 0.;
		auto ordered = true;
		const auto result = Export(project({}), {}, [&](float64 value) {
			ordered = ordered && (value >= last) && (value <= 1.);
			last = value;
			++calls;
		}, nullptr);
		check(
			result.ok && calls > 5 && ordered && last > 0.95,
			u"progress"_q,
			u"%1 calls, the last one %2"_q.arg(calls).arg(last));
	}

	// Effects.
	const auto effect = [](
			VideoFx::Type type,
			std::initializer_list<std::pair<const char*, float64>> values) {
		auto result = VideoFx::MakeEntry(type);
		for (const auto &[id, value] : values) {
			const auto index = VideoFx::FindParam(
				type,
				QString::fromLatin1(id));
			if (index >= 0 && index < int(result.values.size())) {
				result.values[index] = value;
			}
		}
		return VideoFx::Sanitized(std::move(result));
	};

	// The right half is reflected to the left (green over yellow on both
	// sides), then what is brighter than the green becomes white and the
	// rest black: a black top and a white bottom.
	const auto twoEffects = VideoFx::Stack{
		effect(VideoFx::Type::Mirror, { { "mode", 1. } }),
		effect(VideoFx::Type::Threshold, {
			{ "level", 63. },
			{ "softness", 0. },
		}),
	};
	const auto black = QRgb(0xFF000000U);
	const auto white = QRgb(0xFFFFFFFFU);
	const auto blackOverWhite = [&](const std::array<QRgb, 4> &corners) {
		return TestNear(corners[0], black)
			&& TestNear(corners[1], black)
			&& TestNear(corners[2], white)
			&& TestNear(corners[3], white);
	};
	{
		auto first = State{ .clips = { { 0, 0, 500 } }, .speed = 150 };
		first.fx = twoEffects;
		const auto copy = first;
		auto changed = first;
		changed.fx[1].values[0] -= 1.;
		auto off = first;
		off.fx[0].enabled = false;
		auto reordered = first;
		std::swap(reordered.fx[0], reordered.fx[1]);
		auto plain = first;
		plain.fx.clear();

		// The way a kept stack comes back.
		auto restored = plain;
		restored.fx = VideoFx::Deserialize(VideoFx::Serialize(first.fx));
		check(
			(copy == first)
				&& (changed != first)
				&& (off != first)
				&& (reordered != first)
				&& (plain != first)
				&& (restored == first)
				&& VideoFx::HasEnabled(first.fx)
				&& !VideoFx::HasEnabled(plain.fx),
			u"effects in the state of a project"_q,
			u"%1 effects, %2 bytes"_q
				.arg(first.fx.size())
				.arg(VideoFx::Serialize(first.fx).size()));
	}
	{
		// The left half of the frame turned by 90 degrees: the bottom half
		// of the source, its left end up.
		const auto frame = TestQuadrants(QSize(320, 240));
		const auto whole = ComposePreview(
			frame,
			kTestSize,
			QRect(QPoint(), kTestSize),
			QSize(80, 60),
			0);
		const auto canvas = kTestSize.transposed();
		const auto crop = CropRect(canvas, QRectF(0., 0., 0.5, 1.));
		const auto output = PreviewSize(crop.size(), 80);
		const auto turned = ComposePreview(frame, canvas, crop, output, 90);
		const auto ok = (whole.size() == QSize(80, 60))
			&& TestNear(whole.pixel(20, 15), kTestColors[0])
			&& TestNear(whole.pixel(60, 45), kTestColors[3])
			&& (crop == QRect(0, 0, 60, 160))
			&& (output == QSize(30, 80))
			&& (turned.size() == output)
			&& TestNear(turned.pixel(15, 20), kTestColors[2])
			&& TestNear(turned.pixel(15, 60), kTestColors[3])
			&& (PreviewSize(QSize(60, 160), 1000) == QSize(60, 160))
			&& ComposePreview(frame, canvas, crop, QSize(), 90).isNull();

		// A portrait frame in the landscape canvas: bars on the sides.
		const auto bars = ComposePreview(
			TestQuadrants(QSize(120, 160)),
			kTestSize,
			QRect(QPoint(), kTestSize),
			kTestSize,
			0);
		check(
			ok
				&& TestNear(bars.pixel(8, 60), black)
				&& TestNear(bars.pixel(152, 60), black)
				&& TestNear(bars.pixel(60, 30), kTestColors[0])
				&& TestNear(bars.pixel(100, 90), kTestColors[3]),
			u"frames for the preview of the effects"_q,
			size(output)
				+ ' '
				+ (turned.isNull()
					? QString()
					: (TestColor(turned.pixel(15, 20))
						+ ' '
						+ TestColor(turned.pixel(15, 60)))));
	}
	{
		const auto clean = run({});
		const auto result = run({ .fx = twoEffects });
		const auto corners = TestCorners(result.content);
		const auto was = VideoCore::ReadFrame({ .content = clean.content });
		const auto now = VideoCore::ReadFrame({ .content = result.content });
		auto difference = 0.;
		if (!was.isNull() && was.size() == now.size()) {
			auto sum = int64(0);
			for (auto y = 0; y != was.height(); ++y) {
				for (auto x = 0; x != was.width(); ++x) {
					const auto a = was.pixel(x, y);
					const auto b = now.pixel(x, y);
					sum += std::abs(qRed(a) - qRed(b))
						+ std::abs(qGreen(a) - qGreen(b))
						+ std::abs(qBlue(a) - qBlue(b));
				}
			}
			difference = sum / (3. * was.width() * was.height());
		}
		check(
			timed(result, kTestDuration)
				&& result.size == clean.size
				&& result.frames == clean.frames
				&& result.audio
				&& blackOverWhite(corners)
				&& difference > 40.,
			u"mp4 with two effects"_q,
			describe(result)
				+ u", "_q
				+ colors(corners)
				+ u", differs by %1"_q.arg(difference, 0, 'f', 1));
	}
	{
		// Switched off, the effects change nothing.
		auto stack = twoEffects;
		for (auto &entry : stack) {
			entry.enabled = false;
		}
		const auto result = run({ .fx = stack });
		const auto corners = TestCorners(result.content);
		check(
			result.ok
				&& TestNear(corners[0], kTestColors[0])
				&& TestNear(corners[1], kTestColors[1])
				&& TestNear(corners[2], kTestColors[2])
				&& TestNear(corners[3], kTestColors[3]),
			u"effects that are switched off"_q,
			describe(result) + u", "_q + colors(corners));
	}
	{
		// The effects are applied after the turn and the crop: the left
		// half of the turned frame is the bottom half of the source, the
		// blue over the yellow. Its bottom is reflected up, so the blue is
		// gone, and the yellow is brighter than the level. Before the turn
		// the same effects would leave the blue, black after the threshold.
		const auto result = run({
			.crop = QRectF(0., 0., 0.5, 1.),
			.aspect = Aspect::Free,
			.rotation = 90,
			.fx = {
				effect(VideoFx::Type::Mirror, { { "mode", 3. } }),
				twoEffects[1],
			},
		});
		const auto corners = TestCorners(result.content);
		check(
			result.ok
				&& result.size == QSize(60, 160)
				&& TestNear(corners[0], white)
				&& TestNear(corners[1], white)
				&& TestNear(corners[2], white)
				&& TestNear(corners[3], white),
			u"effects after the turn and the crop"_q,
			describe(result) + u", "_q + colors(corners));
	}
	{
		// A strobe, black for the first quarter of a second out of every
		// half: the second clip starts at 300 ms of the result, where the
		// time of the effects must go on and not start again.
		const auto piece = crl::time(300);
		const auto result = run({
			.clips = { { 0, 0, piece }, { 0, 0, piece } },
			.fx = { effect(VideoFx::Type::Strobe, {
				{ "frequency", 2. },
				{ "duty", 50. },
				{ "mode", 0. },
			}) },
		});
		const auto at = [&](crl::time position) {
			const auto frame = VideoCore::ReadFrame({
				.content = result.content,
				.from = position,
			});
			return frame.isNull()
				? QRgb(0xFF808080U)
				: frame.pixel(frame.width() / 4, frame.height() / 4);
		};
		const auto dark = at(110);
		const auto shown = at(410);
		const auto darkAgain = at(560);
		check(
			timed(result, 2 * piece)
				&& TestNear(dark, black)
				&& TestNear(shown, kTestColors[0])
				&& TestNear(darkAgain, black),
			u"effects get the time of the result"_q,
			describe(result)
				+ u", "_q
				+ TestColor(dark)
				+ ' '
				+ TestColor(shown)
				+ ' '
				+ TestColor(darkAgain));
	}
	{
		const auto telegram = run(
			{ .fx = twoEffects },
			{ .format = Format::GifVideo });
		const auto file = run(
			{ .fx = twoEffects },
			{ .format = Format::Gif, .maxSide = 80 });
		const auto fileCorners = TestCorners(file.content);
		check(
			telegram.ok
				&& !telegram.audio
				&& blackOverWhite(TestCorners(telegram.content))
				&& file.ok
				&& file.content.startsWith("GIF89a")
				&& file.size == QSize(80, 60)
				&& blackOverWhite(fileCorners),
			u"GIF with effects"_q,
			describe(file) + u", "_q + colors(fileCorners));
	}
	{
		const auto sticker = run(
			{ .clips = { { 0, 0, 400 } }, .fx = twoEffects },
			{ .format = Format::Sticker });
		const auto emoji = run(
			{ .clips = { { 0, 0, 400 } }, .fx = twoEffects },
			{ .format = Format::Emoji });
		const auto stickerCorners = TestCorners(sticker.content);
		const auto emojiCorners = TestCorners(emoji.content);
		check(
			sticker.ok
				&& sticker.size == QSize(512, 384)
				&& sticker.content.size() <= VideoCore::kStickerMaxBytes
				&& blackOverWhite(stickerCorners)
				&& emoji.ok
				&& emoji.content.size() <= VideoCore::kEmojiMaxBytes
				&& blackOverWhite(emojiCorners),
			u"sticker and emoji with effects"_q,
			describe(sticker)
				+ u", "_q
				+ colors(stickerCorners)
				+ u"; "_q
				+ describe(emoji)
				+ u", "_q
				+ colors(emojiCorners));
	}
	{
		const auto wide = Project{
			.sources = { Source{
				.info = {
					.duration = 10'000,
					.size = QSize(3840, 2160),
					.fps = 60.,
					.hasAudio = true,
				},
			} },
			.state = { .clips = { { 0, 0, 10'000 } }, .speed = 200 },
		};
		const auto mp4 = OutputSize(wide, {});
		const auto limited = OutputSize(wide, { .maxSide = 854 });
		const auto gif = OutputSize(wide, { .format = Format::Gif });
		const auto sticker = OutputSize(wide, { .format = Format::Sticker });
		const auto estimate = EstimateSize(wide, {});
		check(
			mp4 == QSize(1920, 1080)
				&& limited == QSize(854, 480)
				&& gif == QSize(480, 270)
				&& sticker == QSize(512, 288)
				&& OutputDuration(wide.state) == 5000
				&& OutputHasAudio(wide, {})
				&& !OutputHasAudio(wide, { .format = Format::GifVideo })
				&& estimate > 4'000'000
				&& estimate < 8'000'000,
			u"sizes of the results"_q,
			size(mp4)
				+ ' '
				+ size(limited)
				+ ' '
				+ size(gif)
				+ ' '
				+ size(sticker)
				+ u", about %1 bytes"_q.arg(estimate));
	}

	// What the editor itself decides, without its widgets.
	const auto editor = VideoEditorSelfTest(log);
	return passed && editor;
}

} // namespace Oblivion::VideoEdit
