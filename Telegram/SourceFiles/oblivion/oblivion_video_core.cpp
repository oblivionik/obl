/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_video_core.h"

#include "base/debug_log.h"
#include "ffmpeg/ffmpeg_bytes_io_wrap.h"
#include "ffmpeg/ffmpeg_utility.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtGui/QColor>
#include <QtGui/QImageReader>
#include <QtGui/QTransform>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <optional>
#include <set>
#include <thread>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
} // extern "C"

// On macOS the folder with the libvpx headers (Libraries/local/include) is
// on the include path of every target. On Windows it is there only for the
// targets that link desktop-app::external_vpx by themselves, the Telegram
// one gets libvpx through FFmpeg, as a library without the headers. The
// FFmpeg folder next to it is always on the path, the headers are found
// from it then.
#if __has_include(<vpx/vpx_encoder.h>)
#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>
#else // __has_include(<vpx/vpx_encoder.h>)
#include <../local/include/vpx/vpx_encoder.h>
#include <../local/include/vpx/vp8cx.h>
#endif // __has_include(<vpx/vpx_encoder.h>)

namespace Oblivion::VideoCore {
namespace {

using namespace FFmpeg;

constexpr auto kSecond = int64(1'000'000);

// Used when neither the frame nor the container knows how long the last
// frame is shown.
constexpr auto kDefaultFrameGap = kSecond / 25;

// The server may count kilobytes either way, the files stay below both.
constexpr auto kStickerTargetBytes = 255 * 1000;
constexpr auto kEmojiTargetBytes = 63 * 1000;

// libvpx: 0 is the slowest and the best, 5 is the fastest of the "good"
// deadline. A sticker is at most 90 frames 512x512, encoded several times
// to fit the size, see the self-test for the timing.
constexpr auto kVp9Speed = 2;
constexpr auto kVp9Lag = 25;
constexpr auto kVp9Threads = 4;

// Constrained quality: the level is the best quality the encoder goes
// for, so simple stickers don't take all the 256 KB, the bitrate is the
// limit for the complex ones.
constexpr auto kStickerQuality = 18;
constexpr auto kStickerAlphaQuality = 16;
constexpr auto kStickerAlphaShare = 0.3;
constexpr auto kStickerAttempts = 6;
constexpr auto kStickerMinKbps = 24;
constexpr auto kStickerHalvings = 2;

// A part of MakeVideoSticker() progress taken by reading the source.
constexpr auto kStickerReadShare = 0.2;

// Dropping every tenth frame of a video that is a bit faster than the
// limit (33.3 fps of a GIF with 30 ms frames against 30) is seen better
// than the extra frames are: up to this excess all the frames are kept,
// in a sticker they are shown a bit slower to stay within its limit.
constexpr auto kKeptRateExcess = 1.134;

// Browsers show frames with a delay below 20 ms for 100 ms.
constexpr auto kGifMinDuration = crl::time(20);
constexpr auto kGifMaxDelay = 0xFFFF;
constexpr auto kGifSameColor = 2; // Per channel.

constexpr auto kMp4AudioBitRate = 128 * 1024;
constexpr auto kMp4MinSide = 16;

[[nodiscard]] bool Cancelled(const Cancel &cancel) {
	return cancel && cancel->load();
}

// Input.

class Input final {
public:
	Input() = default;
	Input(const Input &other) = delete;
	Input &operator=(const Input &other) = delete;

	[[nodiscard]] bool open(const QString &path, const QByteArray &content);

	[[nodiscard]] AVFormatContext *format() const {
		return _format.get();
	}

	// Positions are in microseconds from the start of the file.
	[[nodiscard]] int64 position(int64 pts, not_null<AVStream*> stream) const;
	[[nodiscard]] int64 pts(int64 position, not_null<AVStream*> stream) const;

	[[nodiscard]] crl::time duration(not_null<AVStream*> stream) const;

private:
	static int Read(void *opaque, uint8_t *buffer, int bufferSize);
	static int64_t Seek(void *opaque, int64_t offset, int whence);

	std::unique_ptr<QFile> _file;
	QByteArray _bytes;
	int64 _offset = 0;
	FormatPointer _format; // After the IO data, it is destroyed first.
	int64 _origin = 0;

};

bool Input::open(const QString &path, const QByteArray &content) {
	if (!path.isEmpty()) {
		_file = std::make_unique<QFile>(path);
		if (!_file->open(QIODevice::ReadOnly)) {
			LOG(("Oblivion Video Error: Could not open '%1' for reading."
				).arg(path));
			return false;
		}
	} else if (!content.isEmpty()) {
		_bytes = content;
	} else {
		return false;
	}
	_format = MakeFormatPointer(
		static_cast<void*>(this),
		&Input::Read,
		nullptr,
		&Input::Seek);
	if (!_format) {
		return false;
	}
	const auto error = AvErrorWrap(avformat_find_stream_info(
		_format.get(),
		nullptr));
	if (error) {
		LogError(u"avformat_find_stream_info"_q, error);
		return false;
	}
	_origin = (_format->start_time != AV_NOPTS_VALUE)
		? _format->start_time
		: 0;
	return true;
}

int64 Input::position(int64 pts, not_null<AVStream*> stream) const {
	return av_rescale_q(pts, stream->time_base, kUniversalTimeBase)
		- _origin;
}

int64 Input::pts(int64 position, not_null<AVStream*> stream) const {
	return av_rescale_q(
		position + _origin,
		kUniversalTimeBase,
		stream->time_base);
}

crl::time Input::duration(not_null<AVStream*> stream) const {
	if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
		return PtsToTimeCeil(stream->duration, stream->time_base);
	} else if (_format->duration != AV_NOPTS_VALUE
		&& _format->duration > 0) {
		return (_format->duration + 999) / 1000;
	}
	return 0;
}

int Input::Read(void *opaque, uint8_t *buffer, int bufferSize) {
	const auto that = static_cast<Input*>(opaque);
	if (that->_file) {
		const auto read = that->_file->read(
			reinterpret_cast<char*>(buffer),
			bufferSize);
		return (read > 0)
			? int(read)
			: (read == 0)
			? AVERROR_EOF
			: AVERROR(EIO);
	}
	const auto available = int64(that->_bytes.size()) - that->_offset;
	const auto count = std::min(int64(bufferSize), available);
	if (count <= 0) {
		return AVERROR_EOF;
	}
	memcpy(buffer, that->_bytes.constData() + that->_offset, count);
	that->_offset += count;
	return int(count);
}

int64_t Input::Seek(void *opaque, int64_t offset, int whence) {
	const auto that = static_cast<Input*>(opaque);
	const auto size = that->_file
		? int64(that->_file->size())
		: int64(that->_bytes.size());
	const auto current = that->_file
		? int64(that->_file->pos())
		: that->_offset;
	switch (whence & ~AVSEEK_FORCE) {
	case AVSEEK_SIZE: return size;
	case SEEK_SET: break;
	case SEEK_CUR: offset += current; break;
	case SEEK_END: offset += size; break;
	default: return -1;
	}
	if (offset < 0 || offset > size) {
		return -1;
	} else if (that->_file) {
		return that->_file->seek(offset) ? offset : -1;
	}
	that->_offset = offset;
	return offset;
}

[[nodiscard]] int FindStream(
		not_null<AVFormatContext*> format,
		AVMediaType type) {
	const auto good = [&](int index) {
		const auto stream = format->streams[index];
		return (stream->codecpar->codec_type == type)
			&& !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)
			&& (avcodec_find_decoder(stream->codecpar->codec_id) != nullptr);
	};
	const auto best = av_find_best_stream(format, type, -1, -1, nullptr, 0);
	if (best >= 0 && good(best)) {
		return best;
	}
	for (auto i = 0; i != int(format->nb_streams); ++i) {
		if (good(i)) {
			return i;
		}
	}
	return -1;
}

[[nodiscard]] float64 PixelAspect(
		AVRational frame,
		not_null<AVStream*> stream) {
	const auto aspect = ValidateAspectRatio((frame.num > 0 && frame.den > 0)
		? frame
		: (stream->sample_aspect_ratio.num > 0
			&& stream->sample_aspect_ratio.den > 0)
		? stream->sample_aspect_ratio
		: stream->codecpar->sample_aspect_ratio);
	return float64(aspect.num) / aspect.den;
}

[[nodiscard]] QSize RoundedSize(QSizeF size) {
	return QSize(
		std::max(int(std::round(size.width())), 1),
		std::max(int(std::round(size.height())), 1));
}

// How the frames of a stream are shown: stretched by the pixel aspect
// ratio, then rotated clockwise.
[[nodiscard]] QSize DisplayedSize(QSize stored, float64 aspect, int rotation) {
	const auto unrotated = QSizeF(stored.width() * aspect, stored.height());
	return RoundedSize((rotation == 90 || rotation == 270)
		? unrotated.transposed()
		: unrotated);
}

[[nodiscard]] float64 StreamFps(not_null<AVStream*> stream) {
	const auto valid = [](AVRational value) {
		return (value.num > 0) && (value.den > 0);
	};
	const auto value = valid(stream->avg_frame_rate)
		? av_q2d(stream->avg_frame_rate)
		: valid(stream->r_frame_rate)
		? av_q2d(stream->r_frame_rate)
		: 0.;
	return (value > 0. && value < 1000.) ? value : 0.;
}

// The matrix of the decoded YUV. Untagged videos are BT.601, the way
// players (and tdesktop itself) show them.
[[nodiscard]] int FrameColorspace(not_null<const AVFrame*> frame) {
	switch (frame->colorspace) {
	case AVCOL_SPC_BT709: return SWS_CS_ITU709;
	case AVCOL_SPC_FCC: return SWS_CS_FCC;
	case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
	default: return SWS_CS_DEFAULT;
	}
}

[[nodiscard]] bool FrameFullRange(not_null<const AVFrame*> frame) {
	switch (frame->format) {
	case AV_PIX_FMT_YUVJ420P:
	case AV_PIX_FMT_YUVJ422P:
	case AV_PIX_FMT_YUVJ440P:
	case AV_PIX_FMT_YUVJ444P:
	case AV_PIX_FMT_YUVJ411P:
		return true;
	}
	return (frame->color_range == AVCOL_RANGE_JPEG);
}

[[nodiscard]] int64 FramePts(not_null<AVFrame*> frame) {
	return (frame->best_effort_timestamp != AV_NOPTS_VALUE)
		? frame->best_effort_timestamp
		: frame->pts;
}

// Decoded frame -> upright ARGB32_Premultiplied image: the crop is read
// right from the planes of the frame and scaled by swscale to the size
// of the result, then the image is rotated.
class FrameConverter final {
public:
	FrameConverter(not_null<AVStream*> stream, QRect crop, QSize size);

	// Null on errors.
	[[nodiscard]] QImage convert(not_null<AVFrame*> frame);

private:
	[[nodiscard]] bool prepared(not_null<AVFrame*> frame) const;
	[[nodiscard]] bool prepare(not_null<AVFrame*> frame);

	const not_null<AVStream*> _stream;
	const QRect _crop; // Displayed pixels, empty = everything.
	const QSize _size; // Of the result, empty = the size of the crop.
	const int _rotation = 0;

	// What the scaler below was made for.
	int _width = 0;
	int _height = 0;
	int _format = int(AV_PIX_FMT_NONE);
	AVRational _aspect = { 0, 1 };
	int _colorspace = 0;
	bool _fullRange = false;

	QRect _source; // Stored pixels.
	QSize _scaled; // Before the rotation.
	bool _direct = false;
	bool _alpha = false;
	SwscalePointer _swscale;

};

FrameConverter::FrameConverter(
	not_null<AVStream*> stream,
	QRect crop,
	QSize size)
: _stream(stream)
, _crop(crop)
, _size(size)
, _rotation(ReadRotationFromMetadata(stream)) {
}

bool FrameConverter::prepared(not_null<AVFrame*> frame) const {
	return _swscale
		&& (_width == frame->width)
		&& (_height == frame->height)
		&& (_format == frame->format)
		&& (_aspect.num == frame->sample_aspect_ratio.num)
		&& (_aspect.den == frame->sample_aspect_ratio.den)
		&& (_colorspace == FrameColorspace(frame))
		&& (_fullRange == FrameFullRange(frame));
}

bool FrameConverter::prepare(not_null<AVFrame*> frame) {
	_swscale = nullptr;
	const auto format = AVPixelFormat(frame->format);
	const auto descriptor = av_pix_fmt_desc_get(format);
	if (!descriptor || frame->width <= 0 || frame->height <= 0) {
		return false;
	}
	const auto stored = QSize(frame->width, frame->height);
	const auto aspect = PixelAspect(frame->sample_aspect_ratio, _stream);
	const auto unrotated = QSizeF(stored.width() * aspect, stored.height());
	const auto swap = (_rotation == 90 || _rotation == 270);
	const auto whole = QRect(
		QPoint(),
		DisplayedSize(stored, aspect, _rotation));
	auto crop = _crop.isEmpty() ? whole : _crop.intersected(whole);
	if (crop.isEmpty()) {
		crop = whole;
	}

	// Displayed pixels -> stored pixels.
	const auto rotate = QTransform().rotate(_rotation);
	const auto bounds = rotate.mapRect(QRectF(QPointF(), unrotated));
	const auto toDisplayed = rotate
		* QTransform::fromTranslate(-bounds.x(), -bounds.y());
	const auto mapped = toDisplayed.inverted().mapRect(QRectF(crop));
	auto left = int(std::round(mapped.x() / aspect));
	auto top = int(std::round(mapped.y()));
	const auto right = int(std::round(
		(mapped.x() + mapped.width()) / aspect));
	const auto bottom = int(std::round(mapped.y() + mapped.height()));

	// Planes with subsampled chroma can be read only from even pixels.
	_direct = !(descriptor->flags
		& (AV_PIX_FMT_FLAG_BITSTREAM
			| AV_PIX_FMT_FLAG_HWACCEL
			| AV_PIX_FMT_FLAG_BAYER));
	if (_direct) {
		left -= left % (1 << descriptor->log2_chroma_w);
		top -= top % (1 << descriptor->log2_chroma_h);
	}
	_source = QRect(left, top, right - left, bottom - top).intersected(
		QRect(QPoint(), stored));
	if (_source.isEmpty()) {
		return false;
	}
	const auto size = _size.isEmpty() ? crop.size() : _size;
	_scaled = swap ? size.transposed() : size;
	_alpha = (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;

	// Unlike MakeSwscalePointer() the source range is set before the
	// init: it chooses the converters.
	const auto from = _direct ? _source.size() : stored;
	const auto to = _direct ? _scaled : stored;
	auto result = SwscalePointer(sws_alloc_context());
	if (!result) {
		LogError(u"sws_alloc_context"_q);
		return false;
	}
	const auto context = result.get();
	const auto set = [&](const char *name, int64 value) {
		return av_opt_set_int(context, name, value, 0) >= 0;
	};
	const auto fullRange = FrameFullRange(frame);
	if (!set("srcw", from.width())
		|| !set("srch", from.height())
		|| !set("src_format", frame->format)
		|| !set("src_range", fullRange ? 1 : 0)
		|| !set("dstw", to.width())
		|| !set("dsth", to.height())
		|| !set("dst_format", AV_PIX_FMT_BGRA)
		|| !set("sws_flags", SWS_BICUBIC | SWS_ACCURATE_RND)) {
		LogError(u"av_opt_set_int"_q, u"swscale"_q);
		return false;
	}
	const auto error = AvErrorWrap(sws_init_context(
		context,
		nullptr,
		nullptr));
	if (error) {
		LogError(u"sws_init_context"_q, error);
		return false;
	}
	// sws ignores the color properties of frames: BT.709 (HD and phone
	// videos) colors would shift and full range videos would lose their
	// shadows and highlights. Fails for RGB sources, they don't need it.
	sws_setColorspaceDetails(
		context,
		sws_getCoefficients(FrameColorspace(frame)),
		fullRange ? 1 : 0,
		sws_getCoefficients(SWS_CS_DEFAULT),
		0,
		0,
		1 << 16,
		1 << 16);

	_swscale = std::move(result);
	_width = frame->width;
	_height = frame->height;
	_format = frame->format;
	_aspect = frame->sample_aspect_ratio;
	_colorspace = FrameColorspace(frame);
	_fullRange = fullRange;
	return true;
}

QImage FrameConverter::convert(not_null<AVFrame*> frame) {
	if (!FrameHasData(frame)) {
		return QImage();
	} else if (!prepared(frame) && !prepare(frame)) {
		return QImage();
	}
	const auto descriptor = av_pix_fmt_desc_get(AVPixelFormat(_format));
	auto result = QImage(
		_direct ? _scaled : QSize(_width, _height),
		QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	const uint8_t *data[AV_NUM_DATA_POINTERS] = { nullptr };
	int linesize[AV_NUM_DATA_POINTERS] = { 0 };
	for (auto i = 0; i != AV_NUM_DATA_POINTERS; ++i) {
		data[i] = frame->data[i];
		linesize[i] = frame->linesize[i];
	}
	if (_direct) {
		// The same way av_frame_apply_cropping() moves the planes.
		for (auto i = 0; i != AV_NUM_DATA_POINTERS && data[i]; ++i) {
			if ((descriptor->flags & AV_PIX_FMT_FLAG_PAL) && i == 1) {
				break;
			}
			const auto chroma = (i == 1 || i == 2);
			const auto shiftX = chroma ? descriptor->log2_chroma_w : 0;
			const auto shiftY = chroma ? descriptor->log2_chroma_h : 0;
			auto step = 0;
			for (auto j = 0; j != descriptor->nb_components; ++j) {
				if (descriptor->comp[j].plane == i) {
					step = descriptor->comp[j].step;
					break;
				}
			}
			if (!step) {
				return QImage();
			}
			data[i] += (_source.y() >> shiftY) * int64(linesize[i])
				+ (_source.x() >> shiftX) * int64(step);
		}
	}
	uint8_t *to[AV_NUM_DATA_POINTERS] = { result.bits(), nullptr };
	int toLinesize[AV_NUM_DATA_POINTERS] = {
		int(result.bytesPerLine()),
		0,
	};
	const auto lines = sws_scale(
		_swscale.get(),
		data,
		linesize,
		0,
		_direct ? _source.height() : _height,
		to,
		toLinesize);
	if (lines <= 0) {
		return QImage();
	}
	if (_alpha) {
		for (auto y = 0, height = result.height(); y != height; ++y) {
			const auto line = reinterpret_cast<QRgb*>(result.scanLine(y));
			for (auto x = 0, width = result.width(); x != width; ++x) {
				if (qAlpha(line[x]) != 255) {
					line[x] = qPremultiply(line[x]);
				}
			}
		}
	}
	if (!_direct) {
		result = result.copy(_source).scaled(
			_scaled,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}
	if (_rotation) {
		result = result.transformed(QTransform().rotate(_rotation));
	}
	return (result.format() == QImage::Format_ARGB32_Premultiplied)
		? result
		: result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Images.

[[nodiscard]] QImage Prepared(const QImage &image, QSize size) {
	if (image.isNull() || size.isEmpty()) {
		return QImage();
	}
	auto result = (image.format() == QImage::Format_ARGB32_Premultiplied)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	if (result.size() != size) {
		result = result.scaled(
			size,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}
	return (result.format() == QImage::Format_ARGB32_Premultiplied)
		? result
		: result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// The image in the middle of a canvas filled with a color (premultiplied,
// transparent by default).
[[nodiscard]] QImage Centered(
		const QImage &image,
		QSize canvas,
		QRgb fill = 0) {
	if (image.size() == canvas) {
		return image;
	}
	auto result = QImage(canvas, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	result.fill(fill);
	const auto width = std::min(image.width(), canvas.width());
	const auto height = std::min(image.height(), canvas.height());
	const auto fromX = (image.width() - width) / 2;
	const auto fromY = (image.height() - height) / 2;
	const auto toX = (canvas.width() - width) / 2;
	const auto toY = (canvas.height() - height) / 2;
	for (auto y = 0; y != height; ++y) {
		memcpy(
			result.scanLine(toY + y) + toX * 4,
			image.constScanLine(fromY + y) + fromX * 4,
			width * 4);
	}
	return result;
}

// Colors under fully transparent pixels are never shown, but they are
// encoded: left black they give a hard edge around every shape, which
// takes bits and bleeds into the visible pixels through the subsampled
// chroma and the compression. Here they are filled with the colors of
// the nearest visible pixels (pull-push over a pyramid of averages).
//
// rgb: straight colors (0x00RRGGBB), anything where alpha is zero.
void FillTransparent(
		std::vector<uint32> &rgb,
		const std::vector<uint8_t> &alpha,
		int width,
		int height) {
	struct Level {
		int width = 0;
		int height = 0;
		std::vector<float> data; // r, g, b premultiplied and the weight.
	};
	auto levels = std::vector<Level>();
	{
		auto &first = levels.emplace_back();
		first.width = (width + 1) / 2;
		first.height = (height + 1) / 2;
		first.data.resize(size_t(first.width) * first.height * 4, 0.f);
		for (auto y = 0; y != height; ++y) {
			for (auto x = 0; x != width; ++x) {
				const auto index = size_t(y) * width + x;
				const auto a = alpha[index];
				if (!a) {
					continue;
				}
				const auto color = rgb[index];
				const auto weight = a / 255.f;
				const auto to = first.data.data()
					+ (size_t(y / 2) * first.width + (x / 2)) * 4;
				to[0] += ((color >> 16) & 0xFF) * weight;
				to[1] += ((color >> 8) & 0xFF) * weight;
				to[2] += (color & 0xFF) * weight;
				to[3] += weight;
			}
		}
		for (auto &value : first.data) {
			value /= 4.f;
		}
	}
	while (levels.back().width > 1 || levels.back().height > 1) {
		const auto &from = levels.back();
		auto next = Level();
		next.width = (from.width + 1) / 2;
		next.height = (from.height + 1) / 2;
		next.data.resize(size_t(next.width) * next.height * 4, 0.f);
		for (auto y = 0; y != from.height; ++y) {
			for (auto x = 0; x != from.width; ++x) {
				const auto source = from.data.data()
					+ (size_t(y) * from.width + x) * 4;
				const auto to = next.data.data()
					+ (size_t(y / 2) * next.width + (x / 2)) * 4;
				for (auto i = 0; i != 4; ++i) {
					to[i] += source[i] / 4.f;
				}
			}
		}
		levels.push_back(std::move(next));
	}
	// Now every level becomes fully opaque, from the top one.
	{
		auto &top = levels.back();
		const auto weight = top.data[3];
		for (auto i = 0; i != 3; ++i) {
			top.data[i] = (weight > 0.f) ? (top.data[i] / weight) : 0.f;
		}
	}
	for (auto i = int(levels.size()) - 2; i >= 0; --i) {
		const auto &parent = levels[i + 1];
		auto &level = levels[i];
		for (auto y = 0; y != level.height; ++y) {
			for (auto x = 0; x != level.width; ++x) {
				const auto to = level.data.data()
					+ (size_t(y) * level.width + x) * 4;
				const auto rest = 1.f - std::min(to[3], 1.f);
				if (rest <= 0.f) {
					continue;
				}
				const auto from = parent.data.data()
					+ (size_t(y / 2) * parent.width + (x / 2)) * 4;
				for (auto j = 0; j != 3; ++j) {
					to[j] += rest * from[j];
				}
			}
		}
	}
	// Almost transparent pixels have almost no color of their own left
	// after the premultiplication, they are mixed with the fill.
	constexpr auto kOwnColorFrom = 16;
	const auto &first = levels.front();
	for (auto y = 0; y != height; ++y) {
		for (auto x = 0; x != width; ++x) {
			const auto index = size_t(y) * width + x;
			const auto a = int(alpha[index]);
			if (a >= kOwnColorFrom) {
				continue;
			}
			const auto from = first.data.data()
				+ (size_t(y / 2) * first.width + (x / 2)) * 4;
			const auto color = rgb[index];
			const auto own = std::array<int, 3>{ {
				int((color >> 16) & 0xFF),
				int((color >> 8) & 0xFF),
				int(color & 0xFF),
			} };
			auto mixed = uint32(0);
			for (auto i = 0; i != 3; ++i) {
				const auto fill = std::clamp(int(from[i] + 0.5f), 0, 255);
				const auto value = (own[i] * a + fill * (kOwnColorFrom - a))
					/ kOwnColorFrom;
				mixed = (mixed << 8) | uint32(value);
			}
			rgb[index] = mixed;
		}
	}
}

// Straight colors of an ARGB32_Premultiplied image. Returns true and
// fills alpha if the image has transparent pixels.
[[nodiscard]] bool StraightColors(
		const QImage &image,
		std::vector<uint32> &rgb,
		std::vector<uint8_t> &alpha) {
	const auto width = image.width();
	const auto height = image.height();
	rgb.resize(size_t(width) * height);
	auto transparent = false;
	for (auto y = 0; y != height; ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			image.constScanLine(y));
		const auto to = rgb.data() + size_t(y) * width;
		for (auto x = 0; x != width; ++x) {
			const auto pixel = line[x];
			if (qAlpha(pixel) != 255) {
				transparent = true;
				to[x] = qUnpremultiply(pixel) & 0x00FFFFFFU;
			} else {
				to[x] = pixel & 0x00FFFFFFU;
			}
		}
	}
	if (!transparent) {
		alpha.clear();
		return false;
	}
	alpha.resize(size_t(width) * height);
	for (auto y = 0; y != height; ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			image.constScanLine(y));
		const auto to = alpha.data() + size_t(y) * width;
		for (auto x = 0; x != width; ++x) {
			to[x] = uint8_t(qAlpha(line[x]));
		}
	}
	FillTransparent(rgb, alpha, width, height);
	return true;
}

// Colors of an ARGB32_Premultiplied image shown on white.
void ColorsOnWhite(const QImage &image, std::vector<uint32> &rgb) {
	const auto width = image.width();
	const auto height = image.height();
	rgb.resize(size_t(width) * height);
	for (auto y = 0; y != height; ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			image.constScanLine(y));
		const auto to = rgb.data() + size_t(y) * width;
		for (auto x = 0; x != width; ++x) {
			const auto pixel = line[x];
			const auto rest = 255U - qAlpha(pixel);
			to[x] = rest
				? (((pixel & 0x00FFFFFFU) + rest * 0x010101U) & 0x00FFFFFFU)
				: (pixel & 0x00FFFFFFU);
		}
	}
}

// 0x00RRGGBB -> YUV 4:2:0, BT.601, limited range: what players assume
// for videos of these sizes without color tags. Chroma planes are
// (width + 1) / 2 x (height + 1) / 2.
void RgbToYuv(
		const std::vector<uint32> &rgb,
		int width,
		int height,
		uint8_t *y,
		int yStride,
		uint8_t *u,
		int uStride,
		uint8_t *v,
		int vStride) {
	for (auto row = 0; row != height; ++row) {
		const auto from = rgb.data() + size_t(row) * width;
		const auto to = y + size_t(row) * yStride;
		for (auto x = 0; x != width; ++x) {
			const auto color = from[x];
			const auto r = int((color >> 16) & 0xFF);
			const auto g = int((color >> 8) & 0xFF);
			const auto b = int(color & 0xFF);
			to[x] = uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
		}
	}
	const auto chromaWidth = (width + 1) / 2;
	const auto chromaHeight = (height + 1) / 2;
	for (auto row = 0; row != chromaHeight; ++row) {
		const auto top = rgb.data() + size_t(row) * 2 * width;
		const auto bottom = (row * 2 + 1 < height) ? (top + width) : top;
		const auto toU = u + size_t(row) * uStride;
		const auto toV = v + size_t(row) * vStride;
		for (auto x = 0; x != chromaWidth; ++x) {
			const auto left = x * 2;
			const auto right = (left + 1 < width) ? (left + 1) : left;
			const auto colors = std::array<uint32, 4>{ {
				top[left],
				top[right],
				bottom[left],
				bottom[right],
			} };
			auto r = 0;
			auto g = 0;
			auto b = 0;
			for (const auto color : colors) {
				r += int((color >> 16) & 0xFF);
				g += int((color >> 8) & 0xFF);
				b += int(color & 0xFF);
			}
			toU[x] = uint8_t(
				((-38 * r - 74 * g + 112 * b + 512) >> 10) + 128);
			toV[x] = uint8_t(
				((112 * r - 94 * g - 18 * b + 512) >> 10) + 128);
		}
	}
}

// WebM.

void EbmlId(QByteArray &to, uint32 id) {
	const auto bytes = (id > 0xFFFFFFU)
		? 4
		: (id > 0xFFFFU)
		? 3
		: (id > 0xFFU)
		? 2
		: 1;
	for (auto i = bytes - 1; i >= 0; --i) {
		to.append(char((id >> (8 * i)) & 0xFF));
	}
}

void EbmlSize(QByteArray &to, uint64 size) {
	auto bytes = 1;
	while (bytes < 8 && size >= ((uint64(1) << (7 * bytes)) - 1)) {
		++bytes;
	}
	const auto value = size | (uint64(1) << (7 * bytes));
	for (auto i = bytes - 1; i >= 0; --i) {
		to.append(char((value >> (8 * i)) & 0xFF));
	}
}

// bytes == 0 means as few as the value needs.
void EbmlUint(QByteArray &to, uint32 id, uint64 value, int bytes = 0) {
	if (!bytes) {
		bytes = 1;
		while (bytes < 8 && (value >> (8 * bytes))) {
			++bytes;
		}
	}
	EbmlId(to, id);
	EbmlSize(to, bytes);
	for (auto i = bytes - 1; i >= 0; --i) {
		to.append(char((value >> (8 * i)) & 0xFF));
	}
}

void EbmlSint(QByteArray &to, uint32 id, int64 value) {
	auto bytes = 1;
	while (bytes < 8) {
		const auto limit = int64(1) << (8 * bytes - 1);
		if (value >= -limit && value < limit) {
			break;
		}
		++bytes;
	}
	EbmlId(to, id);
	EbmlSize(to, bytes);
	for (auto i = bytes - 1; i >= 0; --i) {
		to.append(char((uint64(value) >> (8 * i)) & 0xFF));
	}
}

void EbmlFloat(QByteArray &to, uint32 id, float64 value) {
	auto bits = uint64(0);
	static_assert(sizeof(bits) == sizeof(value));
	memcpy(&bits, &value, sizeof(bits));
	EbmlId(to, id);
	EbmlSize(to, 8);
	for (auto i = 7; i >= 0; --i) {
		to.append(char((bits >> (8 * i)) & 0xFF));
	}
}

void EbmlBinary(QByteArray &to, uint32 id, const QByteArray &value) {
	EbmlId(to, id);
	EbmlSize(to, value.size());
	to.append(value);
}

void EbmlString(QByteArray &to, uint32 id, const char *value) {
	EbmlBinary(to, id, QByteArray(value));
}

struct WebmFrame {
	QByteArray data;
	QByteArray alpha; // The same frame of the alpha stream, if there is one.
	crl::time position = 0;
	bool key = false;
};

// One VP9 track, the way FFmpeg and libwebm write it: timestamps in
// milliseconds, SimpleBlocks (BlockGroups with BlockAdditions when there
// is an alpha stream), Cues before the Clusters.
[[nodiscard]] QByteArray MuxWebm(
		QSize size,
		const std::vector<WebmFrame> &frames,
		crl::time duration,
		bool alpha) {
	constexpr auto kIdEbml = 0x1A45DFA3U;
	constexpr auto kIdSegment = 0x18538067U;
	constexpr auto kIdSeekHead = 0x114D9B74U;
	constexpr auto kIdInfo = 0x1549A966U;
	constexpr auto kIdTracks = 0x1654AE6BU;
	constexpr auto kIdCues = 0x1C53BB6BU;
	constexpr auto kIdCluster = 0x1F43B675U;
	constexpr auto kTrack = 1;
	constexpr auto kClusterMaxLength = crl::time(30'000);

	if (frames.empty() || size.isEmpty()) {
		return QByteArray();
	}

	auto header = QByteArray();
	{
		auto ebml = QByteArray();
		EbmlUint(ebml, 0x4286, 1); // EBMLVersion
		EbmlUint(ebml, 0x42F7, 1); // EBMLReadVersion
		EbmlUint(ebml, 0x42F2, 4); // EBMLMaxIDLength
		EbmlUint(ebml, 0x42F3, 8); // EBMLMaxSizeLength
		EbmlString(ebml, 0x4282, "webm"); // DocType
		EbmlUint(ebml, 0x4287, alpha ? 4 : 2); // DocTypeVersion
		EbmlUint(ebml, 0x4285, 2); // DocTypeReadVersion
		EbmlBinary(header, kIdEbml, ebml);
	}

	auto info = QByteArray();
	{
		auto data = QByteArray();
		EbmlUint(data, 0x2AD7B1, 1'000'000); // TimecodeScale: 1 ms.
		EbmlFloat(data, 0x4489, float64(duration)); // Duration
		EbmlString(data, 0x4D80, "Oblivion"); // MuxingApp
		EbmlString(data, 0x5741, "Oblivion"); // WritingApp
		EbmlBinary(info, kIdInfo, data);
	}

	auto tracks = QByteArray();
	{
		auto video = QByteArray();
		EbmlUint(video, 0xB0, size.width()); // PixelWidth
		EbmlUint(video, 0xBA, size.height()); // PixelHeight
		if (alpha) {
			EbmlUint(video, 0x53C0, 1); // AlphaMode
		}
		auto entry = QByteArray();
		EbmlUint(entry, 0xD7, kTrack); // TrackNumber
		EbmlUint(entry, 0x73C5, kTrack); // TrackUID
		EbmlUint(entry, 0x9C, 0); // FlagLacing
		EbmlString(entry, 0x22B59C, "und"); // Language
		EbmlString(entry, 0x86, "V_VP9"); // CodecID
		EbmlUint(entry, 0x83, 1); // TrackType: video.
		EbmlUint( // DefaultDuration, nanoseconds per frame.
			entry,
			0x23E383,
			uint64((std::max(duration, crl::time(1)) * 1'000'000)
				/ int64(frames.size())));
		if (alpha) {
			EbmlUint(entry, 0x55EE, 1); // MaxBlockAdditionID
		}
		EbmlBinary(entry, 0xE0, video); // Video
		auto data = QByteArray();
		EbmlBinary(data, 0xAE, entry); // TrackEntry
		EbmlBinary(tracks, kIdTracks, data);
	}

	struct Cluster {
		QByteArray bytes;
		crl::time time = 0;
		bool key = false;
	};
	auto clusters = std::vector<Cluster>();
	{
		auto data = QByteArray();
		auto time = crl::time(0);
		auto key = false;
		const auto flush = [&] {
			if (data.isEmpty()) {
				return;
			}
			auto &cluster = clusters.emplace_back();
			cluster.time = time;
			cluster.key = key;
			EbmlBinary(cluster.bytes, kIdCluster, data);
			data = QByteArray();
		};
		auto previous = crl::time(0);
		for (const auto &frame : frames) {
			if (data.isEmpty()
				|| frame.key
				|| (frame.position - time > kClusterMaxLength)) {
				flush();
				time = frame.position;
				key = frame.key;
				EbmlUint(data, 0xE7, uint64(time)); // Timecode
			}
			const auto relative = int(frame.position - time);
			auto block = QByteArray();
			block.reserve(frame.data.size() + 4);
			block.append(char(0x80 | kTrack));
			block.append(char((relative >> 8) & 0xFF));
			block.append(char(relative & 0xFF));
			if (!alpha) {
				block.append(char(frame.key ? 0x80 : 0x00));
				block.append(frame.data);
				EbmlBinary(data, 0xA3, block); // SimpleBlock
			} else {
				block.append(char(0x00));
				block.append(frame.data);
				auto more = QByteArray();
				EbmlUint(more, 0xEE, 1); // BlockAddID: alpha.
				EbmlBinary(more, 0xA5, frame.alpha); // BlockAdditional
				auto additions = QByteArray();
				EbmlBinary(additions, 0xA6, more); // BlockMore
				auto group = QByteArray();
				EbmlBinary(group, 0xA1, block); // Block
				EbmlBinary(group, 0x75A1, additions); // BlockAdditions
				if (!frame.key) {
					// ReferenceBlock, makes it not a keyframe.
					EbmlSint(group, 0xFB, previous - frame.position);
				}
				EbmlBinary(data, 0xA0, group); // BlockGroup
			}
			previous = frame.position;
		}
		flush();
	}

	// Positions are from the start of the Segment data, they are written
	// with 8 bytes, so the sizes don't depend on them.
	const auto cues = [&](int64 start) {
		auto data = QByteArray();
		auto position = start;
		for (const auto &cluster : clusters) {
			if (cluster.key) {
				auto positions = QByteArray();
				EbmlUint(positions, 0xF7, kTrack); // CueTrack
				EbmlUint(positions, 0xF1, position, 8); // CueClusterPosition
				auto point = QByteArray();
				EbmlUint(point, 0xB3, uint64(cluster.time)); // CueTime
				EbmlBinary(point, 0xB7, positions); // CueTrackPositions
				EbmlBinary(data, 0xBB, point); // CuePoint
			}
			position += cluster.bytes.size();
		}
		auto result = QByteArray();
		EbmlBinary(result, kIdCues, data);
		return result;
	};
	const auto seekHead = [&](int64 info, int64 tracks, int64 cues) {
		const auto seek = [](QByteArray &to, uint32 id, int64 position) {
			auto binary = QByteArray();
			EbmlId(binary, id);
			auto data = QByteArray();
			EbmlBinary(data, 0x53AB, binary); // SeekID
			EbmlUint(data, 0x53AC, position, 8); // SeekPosition
			EbmlBinary(to, 0x4DBB, data); // Seek
		};
		auto data = QByteArray();
		seek(data, kIdInfo, info);
		seek(data, kIdTracks, tracks);
		seek(data, kIdCues, cues);
		auto result = QByteArray();
		EbmlBinary(result, kIdSeekHead, data);
		return result;
	};
	const auto infoPosition = int64(seekHead(0, 0, 0).size());
	const auto tracksPosition = infoPosition + info.size();
	const auto cuesPosition = tracksPosition + tracks.size();
	const auto clustersPosition = cuesPosition + cues(0).size();

	auto segment = QByteArray();
	segment.append(seekHead(infoPosition, tracksPosition, cuesPosition));
	segment.append(info);
	segment.append(tracks);
	segment.append(cues(clustersPosition));
	for (const auto &cluster : clusters) {
		segment.append(cluster.bytes);
	}

	auto result = header;
	EbmlBinary(result, kIdSegment, segment);
	return result;
}

// VP9.

struct StickerFrame {
	std::vector<uint8_t> y;
	std::vector<uint8_t> u;
	std::vector<uint8_t> v;
	std::vector<uint8_t> a; // Empty in opaque frames.
	crl::time position = 0;
	crl::time duration = 0;
};

struct Vp9Packet {
	QByteArray data;
	bool key = false;
};

struct Vp9Rate {
	int kbps = 0;
	int quality = 0; // 0 (the best) .. 63.
};

[[nodiscard]] int64 TotalSize(const std::vector<Vp9Packet> &packets) {
	auto result = int64(0);
	for (const auto &packet : packets) {
		result += packet.data.size();
	}
	return result;
}

// One pass over the frames: the first one only collects the statistics
// for the rate control (statsOut), the second one uses them (statsIn) and
// gives a packet for every frame. The alpha stream is a video of its own
// with the alpha plane as the luma.
//
// step(index) is called before every frame, false cancels the pass.
[[nodiscard]] bool RunVp9(
		const std::vector<StickerFrame> &frames,
		QSize size,
		bool alphaStream,
		Vp9Rate rate,
		QByteArray *statsOut,
		const QByteArray &statsIn,
		std::vector<Vp9Packet> *packets,
		const Fn<bool(int)> &step,
		QString *error) {
	const auto fail = [&](const QString &text) {
		if (error && error->isEmpty()) {
			*error = text;
		}
		return false;
	};
	const auto first = (statsOut != nullptr);
	const auto width = size.width();
	const auto height = size.height();
	const auto chromaWidth = (width + 1) / 2;
	const auto chromaHeight = (height + 1) / 2;
	// Not 'interface': that is a macro in the Windows headers.
	const auto iface = vpx_codec_vp9_cx();
	if (!iface) {
		return fail(u"No VP9 encoder."_q);
	}
	auto config = vpx_codec_enc_cfg_t();
	if (vpx_codec_enc_config_default(iface, &config, 0) != VPX_CODEC_OK) {
		return fail(u"vpx_codec_enc_config_default failed."_q);
	}
	config.g_w = width;
	config.g_h = height;
	config.g_timebase.num = 1;
	config.g_timebase.den = 1000;
	config.g_threads = std::clamp(
		int(std::thread::hardware_concurrency()),
		1,
		kVp9Threads);
	config.g_pass = first ? VPX_RC_FIRST_PASS : VPX_RC_LAST_PASS;
	config.g_lag_in_frames = kVp9Lag;
	config.g_error_resilient = 0;
	config.rc_end_usage = VPX_CQ;
	config.rc_target_bitrate = std::max(rate.kbps, 1);
	config.rc_min_quantizer = 0;
	config.rc_max_quantizer = 63;
	config.rc_dropframe_thresh = 0;
	config.rc_resize_allowed = 0;

	// The only keyframe is the first one: nobody seeks in three seconds
	// and both streams must have their keyframes in the same places.
	config.kf_mode = VPX_KF_AUTO;
	config.kf_min_dist = 9999;
	config.kf_max_dist = 9999;
	if (!first) {
		config.rc_twopass_stats_in.buf = const_cast<char*>(
			statsIn.constData());
		config.rc_twopass_stats_in.sz = statsIn.size();
	}

	auto codec = vpx_codec_ctx_t();
	if (vpx_codec_enc_init(&codec, iface, &config, 0) != VPX_CODEC_OK) {
		const auto text = vpx_codec_error(&codec);
		return fail(u"vpx_codec_enc_init: "_q + QString::fromUtf8(text));
	}
	const auto codecGuard = gsl::finally([&] {
		vpx_codec_destroy(&codec);
	});
	vpx_codec_control(&codec, VP8E_SET_CPUUSED, kVp9Speed);
	vpx_codec_control(
		&codec,
		VP8E_SET_CQ_LEVEL,
		(unsigned int)(std::clamp(rate.quality, 0, 63)));
	vpx_codec_control(&codec, VP8E_SET_ENABLEAUTOALTREF, 1U);
	vpx_codec_control(&codec, VP8E_SET_ARNR_MAXFRAMES, 7U);
	vpx_codec_control(&codec, VP8E_SET_ARNR_STRENGTH, 5U);
	vpx_codec_control(&codec, VP9E_SET_ROW_MT, 1);
	vpx_codec_control(&codec, VP9E_SET_TILE_COLUMNS, (width >= 512) ? 1 : 0);
	vpx_codec_control(&codec, VP9E_SET_FRAME_PARALLEL_DECODING, 0U);
	if (!alphaStream) {
		vpx_codec_control(&codec, VP9E_SET_COLOR_SPACE, int(VPX_CS_BT_601));
		vpx_codec_control(&codec, VP9E_SET_COLOR_RANGE, 0);
	}

	auto image = vpx_image_t();
	if (!vpx_img_alloc(&image, VPX_IMG_FMT_I420, width, height, 16)) {
		return fail(u"vpx_img_alloc failed."_q);
	}
	const auto imageGuard = gsl::finally([&] {
		vpx_img_free(&image);
	});
	const auto copy = [&](
			int plane,
			const std::vector<uint8_t> &from,
			int planeWidth,
			int planeHeight) {
		const auto to = image.planes[plane];
		const auto stride = image.stride[plane];
		for (auto y = 0; y != planeHeight; ++y) {
			memcpy(
				to + ptrdiff_t(y) * stride,
				from.data() + size_t(y) * planeWidth,
				planeWidth);
		}
	};
	if (alphaStream) {
		for (const auto plane : { VPX_PLANE_U, VPX_PLANE_V }) {
			for (auto y = 0; y != chromaHeight; ++y) {
				memset(
					image.planes[plane] + ptrdiff_t(y) * image.stride[plane],
					0x80,
					chromaWidth);
			}
		}
	}

	const auto collect = [&] {
		auto got = false;
		auto iterator = vpx_codec_iter_t(nullptr);
		while (const auto packet = vpx_codec_get_cx_data(&codec, &iterator)) {
			got = true;
			if (packet->kind == VPX_CODEC_STATS_PKT) {
				if (statsOut) {
					const auto &stats = packet->data.twopass_stats;
					statsOut->append(
						static_cast<const char*>(stats.buf),
						stats.sz);
				}
			} else if (packet->kind == VPX_CODEC_CX_FRAME_PKT) {
				if (packets) {
					auto &to = packets->emplace_back();
					to.data = QByteArray(
						static_cast<const char*>(packet->data.frame.buf),
						packet->data.frame.sz);
					to.key = (packet->data.frame.flags & VPX_FRAME_IS_KEY);
				}
			}
		}
		return got;
	};

	const auto count = int(frames.size());
	for (auto i = 0; i != count; ++i) {
		if (step && !step(i)) {
			return fail(u"Cancelled."_q);
		}
		const auto &frame = frames[i];
		if (alphaStream) {
			copy(VPX_PLANE_Y, frame.a, width, height);
		} else {
			copy(VPX_PLANE_Y, frame.y, width, height);
			copy(VPX_PLANE_U, frame.u, chromaWidth, chromaHeight);
			copy(VPX_PLANE_V, frame.v, chromaWidth, chromaHeight);
		}
		const auto result = vpx_codec_encode(
			&codec,
			&image,
			vpx_codec_pts_t(frame.position),
			(unsigned long)(std::max(frame.duration, crl::time(1))),
			0,
			VPX_DL_GOOD_QUALITY);
		if (result != VPX_CODEC_OK) {
			const auto text = vpx_codec_error(&codec);
			return fail(u"vpx_codec_encode: "_q + QString::fromUtf8(text));
		}
		collect();
	}
	while (true) {
		const auto result = vpx_codec_encode(
			&codec,
			nullptr,
			-1,
			1,
			0,
			VPX_DL_GOOD_QUALITY);
		if (result != VPX_CODEC_OK) {
			const auto text = vpx_codec_error(&codec);
			return fail(u"vpx_codec_encode: "_q + QString::fromUtf8(text));
		} else if (!collect()) {
			break;
		}
	}
	if (first) {
		return !statsOut->isEmpty()
			|| fail(u"No statistics of the first pass."_q);
	} else if (packets && int(packets->size()) != count) {
		return fail(u"%1 packets for %2 frames."_q
			.arg(packets->size())
			.arg(count));
	}
	return true;
}

// What the crop becomes and the size of the video.
struct StickerGeometry {
	QSize content;
	QSize canvas;
};

[[nodiscard]] StickerGeometry ComputeStickerGeometry(QSize crop, int side) {
	side = std::clamp(side, 16, 1024) & ~1;
	const auto longer = std::max(crop.width(), crop.height());
	const auto shorter = std::min(crop.width(), crop.height());
	if (longer <= 0 || shorter <= 0) {
		return {};
	}
	const auto emoji = (side == kEmojiSide);
	auto other = int(std::round(shorter * float64(side) / longer));
	if (!emoji) {
		other += (other & 1);
	}
	other = std::clamp(other, 2, side);
	const auto content = (crop.width() >= crop.height())
		? QSize(side, other)
		: QSize(other, side);
	return { content, emoji ? QSize(side, side) : content };
}

[[nodiscard]] int64 TargetBytes(int side) {
	return (side == kEmojiSide) ? kEmojiTargetBytes : kStickerTargetBytes;
}

[[nodiscard]] QRect CenterSquare(QSize size) {
	const auto side = std::min(size.width(), size.height());
	return QRect(
		(size.width() - side) / 2,
		(size.height() - side) / 2,
		side,
		side);
}

class StickerMaker final {
public:
	explicit StickerMaker(StickerGeometry geometry);

	[[nodiscard]] bool valid() const {
		return !_geometry.canvas.isEmpty();
	}
	[[nodiscard]] QSize content() const {
		return _geometry.content;
	}

	// An image of content() size, they follow each other.
	void add(const QImage &image, crl::time position, crl::time duration);

	[[nodiscard]] VideoStickerResult encode(
		Fn<void(float64)> progress,
		const Cancel &cancel,
		int64 maxBytes);

private:
	void thin();

	const StickerGeometry _geometry;
	std::vector<StickerFrame> _frames;
	std::vector<uint32> _rgb;

};

StickerMaker::StickerMaker(StickerGeometry geometry)
: _geometry(geometry) {
}

void StickerMaker::add(
		const QImage &image,
		crl::time position,
		crl::time duration) {
	if (!valid()) {
		return;
	} else if (!_frames.empty()) {
		position = std::max(position, _frames.back().position + 1);
	}
	if (position >= kStickerMaxDuration) {
		return;
	} else if (_frames.empty()) {
		position = 0;
	}
	const auto prepared = Centered(
		Prepared(image, _geometry.content),
		_geometry.canvas);
	if (prepared.isNull()) {
		return;
	}
	const auto width = _geometry.canvas.width();
	const auto height = _geometry.canvas.height();
	const auto chromaWidth = (width + 1) / 2;
	const auto chromaHeight = (height + 1) / 2;
	auto frame = StickerFrame();
	[[maybe_unused]] const auto transparent = StraightColors(
		prepared,
		_rgb,
		frame.a);
	frame.y.resize(size_t(width) * height);
	frame.u.resize(size_t(chromaWidth) * chromaHeight);
	frame.v.resize(size_t(chromaWidth) * chromaHeight);
	RgbToYuv(
		_rgb,
		width,
		height,
		frame.y.data(),
		width,
		frame.u.data(),
		chromaWidth,
		frame.v.data(),
		chromaWidth);
	if (!_frames.empty()) {
		auto &last = _frames.back();
		last.duration = position - last.position;
	}
	frame.position = position;
	frame.duration = std::clamp(
		duration,
		crl::time(1),
		kStickerMaxDuration - position);
	_frames.push_back(std::move(frame));
}

// One frame for a slot of the same grid as in ReadFrames(): the frames
// that stay keep their positions, so nothing is cut off the end.
void StickerMaker::thin() {
	const auto total = _frames.back().position + _frames.back().duration;
	const auto slot = 1000. / kStickerMaxFps;
	auto kept = size_t(0);
	auto lastSlot = int64(0);
	for (auto i = size_t(0), count = _frames.size(); i != count; ++i) {
		const auto index = int64(std::floor(
			(_frames[i].position + slot / 10.) / slot));
		if (i && index <= lastSlot) {
			continue;
		} else if (kept != i) {
			_frames[kept] = std::move(_frames[i]);
		}
		lastSlot = index;
		++kept;
	}
	_frames.erase(begin(_frames) + kept, end(_frames));
	for (auto i = size_t(0); i != kept; ++i) {
		const auto next = (i + 1 != kept) ? _frames[i + 1].position : total;
		_frames[i].duration = next - _frames[i].position;
	}
}

VideoStickerResult StickerMaker::encode(
		Fn<void(float64)> progress,
		const Cancel &cancel,
		int64 maxBytes) {
	auto result = VideoStickerResult();
	if (_frames.empty()) {
		result.error = u"No frames."_q;
		return result;
	}
	const auto size = _geometry.canvas;
	const auto area = size_t(size.width()) * size.height();
	const auto alpha = ranges::any_of(_frames, [](const StickerFrame &frame) {
		return !frame.a.empty();
	});
	if (alpha) {
		for (auto &frame : _frames) {
			if (frame.a.empty()) {
				frame.a.assign(area, 0xFF);
			}
		}
	}

	// Usually the first encoding fits, it takes most of the progress.
	// Every next one takes a half of what is left.
	auto progressFrom = 0.;
	auto progressSpan = 0.8;
	const auto encode = [&](
			bool alphaStream,
			Vp9Rate rate,
			const QByteArray &stats,
			std::vector<Vp9Packet> &packets,
			float64 part,
			float64 shift) {
		packets.clear();
		packets.reserve(_frames.size());
		const auto count = float64(_frames.size());
		return RunVp9(
			_frames,
			size,
			alphaStream,
			rate,
			nullptr,
			stats,
			&packets,
			[&](int index) {
				if (Cancelled(cancel)) {
					return false;
				} else if (progress) {
					progress(progressFrom
						+ progressSpan * (shift + part * (index / count)));
				}
				return true;
			},
			&result.error);
	};
	const auto analyze = [&](bool alphaStream, QByteArray &stats) {
		stats.clear();
		return RunVp9(
			_frames,
			size,
			alphaStream,
			Vp9Rate{ .kbps = 1000, .quality = kStickerQuality },
			&stats,
			QByteArray(),
			nullptr,
			[&](int) { return !Cancelled(cancel); },
			&result.error);
	};
	const auto mux = [&](
			const std::vector<Vp9Packet> &color,
			const std::vector<Vp9Packet> &mask,
			crl::time duration) {
		auto list = std::vector<WebmFrame>();
		list.reserve(_frames.size());
		for (auto i = 0, count = int(_frames.size()); i != count; ++i) {
			list.push_back({
				.data = color[i].data,
				.alpha = alpha ? mask[i].data : QByteArray(),
				.position = _frames[i].position,
				.key = color[i].key && (!alpha || mask[i].key),
			});
		}
		return MuxWebm(size, list, duration, alpha);
	};

	// The frame rate of the file is the count of frames over the duration.
	// A video that is a bit faster than the limit is slowed down to it
	// (and cut at the maximum duration), see kKeptRateExcess. A faster
	// one loses the extra frames: a part of a video with a variable frame
	// rate may be much faster than the whole video is on average, and that
	// average is all that is known when the frames are read.
	{
		const auto excess = [&] {
			const auto total = _frames.back().position
				+ _frames.back().duration;
			return (_frames.size() * 1000.)
				/ (kStickerMaxFps * float64(std::max(total, crl::time(1))));
		};

		// The rate the frames come at: the first and the last ones may be
		// cut by the ends of the fragment, in a short video that alone
		// makes the count of frames too big for the duration.
		const auto steady = [&] {
			const auto count = int(_frames.size());
			if (count < 3) {
				return excess();
			}
			const auto span = _frames[count - 1].position
				- _frames[1].position;
			return ((count - 2) * 1000.)
				/ (kStickerMaxFps * float64(std::max(span, crl::time(1))));
		};
		if (excess() > kKeptRateExcess && steady() > kKeptRateExcess) {
			thin();
		}
		const auto factor = excess();
		if (factor > 1.
			&& (factor <= kKeptRateExcess || steady() <= kKeptRateExcess)) {
			for (auto &frame : _frames) {
				const auto end = frame.position + frame.duration;
				frame.position = crl::time(std::llround(
					frame.position * factor));
				frame.duration = std::max(
					crl::time(std::llround(end * factor)) - frame.position,
					crl::time(1));
			}
			while (_frames.back().position >= kStickerMaxDuration) {
				_frames.pop_back();
			}
			auto &last = _frames.back();
			last.duration = std::min(
				last.duration,
				kStickerMaxDuration - last.position);
		}
	}

	// A short last frame (cut by the end of the fragment) is shown as long
	// as the other ones, frames that still come too fast are dropped.
	while (true) {
		auto &last = _frames.back();
		const auto count = int64(_frames.size());
		const auto enough = (count * 1000 + kStickerMaxFps - 1)
			/ kStickerMaxFps;
		if (last.position + last.duration >= enough) {
			break;
		} else if (enough <= kStickerMaxDuration || count == 1) {
			last.duration = enough - last.position;
			break;
		}
		_frames.pop_back();
		auto &previous = _frames.back();
		previous.duration = std::clamp(
			previous.duration,
			crl::time(1),
			kStickerMaxDuration - previous.position);
	}

	for (auto halvings = 0;; ++halvings) {
		const auto count = int(_frames.size());
		const auto duration = _frames.back().position
			+ _frames.back().duration;
		const auto seconds = std::max(duration, crl::time(1)) / 1000.;
		const auto kbpsFor = [&](float64 bytes) {
			return std::max(
				int(std::floor(bytes * 8. / seconds / 1000.)),
				kStickerMinKbps);
		};
		// The container takes up to ~30 bytes for a frame.
		const auto budget = std::max(
			maxBytes - 1024 - count * 32,
			maxBytes / 2);

		auto colorStats = QByteArray();
		auto alphaStats = QByteArray();
		if (!analyze(false, colorStats)
			|| (alpha && !analyze(true, alphaStats))) {
			return result;
		}

		auto color = std::vector<Vp9Packet>();
		auto mask = std::vector<Vp9Packet>();
		auto alphaKbps = kbpsFor(budget * kStickerAlphaShare);
		const auto encodeAlpha = [&](float64 part) {
			return encode(
				true,
				Vp9Rate{ alphaKbps, kStickerAlphaQuality },
				alphaStats,
				mask,
				part,
				0.);
		};
		if (alpha && !encodeAlpha(0.3)) {
			return result;
		}
		auto colorKbps = kbpsFor((budget - TotalSize(mask)) * 0.95);
		auto atMinimum = false;
		auto previous = int64(0);
		for (auto attempt = 0; attempt != kStickerAttempts; ++attempt) {
			const auto withAlpha = alpha && !attempt;
			const auto encoded = encode(
				false,
				Vp9Rate{ colorKbps, kStickerQuality },
				colorStats,
				color,
				withAlpha ? 0.7 : 1.,
				withAlpha ? 0.3 : 0.);
			if (!encoded) {
				return result;
			}
			++result.attempts;
			progressFrom += progressSpan;
			progressSpan = (1. - progressFrom) / 2.;

			auto webm = mux(color, mask, duration);
			const auto fits = [&] {
				if (webm.isEmpty() || webm.size() > maxBytes) {
					return false;
				}
				result.webm = std::move(webm);
				result.ok = true;
				result.size = size;
				result.duration = duration;
				result.frames = count;
				result.alpha = alpha;
				if (progress) {
					progress(1.);
				}
				return true;
			};
			if (webm.isEmpty()) {
				result.error = u"Could not write the WebM file."_q;
				return result;
			} else if (fits()) {
				return result;
			} else if (atMinimum
				|| (previous > 0 && webm.size() * 100 > previous * 97)) {
				// A lower bitrate doesn't help anymore: all the frames
				// already have the worst quality.
				break;
			}
			previous = webm.size();
			const auto colorSize = TotalSize(color);
			auto allowed = maxBytes - (webm.size() - colorSize);
			if (alpha && allowed < maxBytes / 2) {
				// The alpha stream took too much.
				alphaKbps = std::max((alphaKbps * 6) / 10, kStickerMinKbps);
				if (!encodeAlpha(1.)) {
					return result;
				}
				webm = mux(color, mask, duration);
				if (fits()) {
					return result;
				}
				allowed = maxBytes - (webm.size() - colorSize);
			}
			const auto ratio = std::clamp(
				float64(allowed) / std::max(colorSize, int64(1)),
				0.05,
				1.);
			const auto next = int(std::floor(colorKbps * ratio * 0.92));
			atMinimum = (next <= kStickerMinKbps);
			colorKbps = std::max(next, kStickerMinKbps);
		}
		if (halvings == kStickerHalvings || count < 4) {
			result.error = u"Could not fit the video into %1 bytes."_q
				.arg(maxBytes);
			return result;
		}
		// Even the worst quality is too big: twice less frames.
		auto kept = std::vector<StickerFrame>();
		kept.reserve((count + 1) / 2);
		for (auto i = 0; i < count; i += 2) {
			auto &frame = _frames[i];
			if (i + 1 < count) {
				frame.duration += _frames[i + 1].duration;
			}
			kept.push_back(std::move(frame));
		}
		_frames = std::move(kept);
	}
}

// GIF.

struct GifPalette {
	int count = 0;
	std::array<uint8_t, 256 * 3> colors = { { 0 } };
};

// Squared distance weights of the channels.
constexpr auto kGifWeightR = 3;
constexpr auto kGifWeightG = 4;
constexpr auto kGifWeightB = 2;

// Median cut over a histogram of 15 bit colors: the box with the biggest
// variance is cut at its mean along the axis of that variance.
class GifQuantizer final {
public:
	GifQuantizer();

	void reset();
	void add(uint32 color);
	void build(int maxColors, GifPalette &palette);

private:
	static constexpr auto kBins = 1 << 15;

	struct Entry {
		std::array<float, 3> color = { { 0.f, 0.f, 0.f } };
		float weight = 0.f;
	};
	struct Box {
		int begin = 0;
		int end = 0;
		int axis = 0;
		float64 score = 0.;
		float64 mean = 0.;
	};
	void measure(Box &box) const;

	std::vector<uint32> _count;
	std::vector<uint32> _sums;
	std::vector<int> _used;
	std::vector<Entry> _entries;

};

GifQuantizer::GifQuantizer()
: _count(kBins, 0)
, _sums(kBins * 3, 0) {
}

void GifQuantizer::reset() {
	for (const auto index : _used) {
		_count[index] = 0;
		_sums[index * 3] = _sums[index * 3 + 1] = _sums[index * 3 + 2] = 0;
	}
	_used.clear();
}

void GifQuantizer::add(uint32 color) {
	const auto r = (color >> 16) & 0xFF;
	const auto g = (color >> 8) & 0xFF;
	const auto b = color & 0xFF;
	const auto index = int(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
	if (!_count[index]++) {
		_used.push_back(index);
	}
	_sums[index * 3] += r;
	_sums[index * 3 + 1] += g;
	_sums[index * 3 + 2] += b;
}

void GifQuantizer::measure(Box &box) const {
	constexpr auto kWeights = std::array<float64, 3>{ {
		float64(kGifWeightR),
		float64(kGifWeightG),
		float64(kGifWeightB),
	} };
	box.score = 0.;
	box.axis = 0;
	box.mean = 0.;
	if (box.end - box.begin < 2) {
		return;
	}
	auto sum = std::array<float64, 3>{ { 0., 0., 0. } };
	auto squares = std::array<float64, 3>{ { 0., 0., 0. } };
	auto total = 0.;
	for (auto i = box.begin; i != box.end; ++i) {
		const auto &entry = _entries[i];
		total += entry.weight;
		for (auto c = 0; c != 3; ++c) {
			const auto value = float64(entry.color[c]);
			sum[c] += entry.weight * value;
			squares[c] += entry.weight * value * value;
		}
	}
	for (auto c = 0; c != 3; ++c) {
		const auto variance = (squares[c] - sum[c] * sum[c] / total)
			* kWeights[c];
		if (variance > box.score) {
			box.score = variance;
			box.axis = c;
			box.mean = sum[c] / total;
		}
	}
}

void GifQuantizer::build(int maxColors, GifPalette &palette) {
	palette.count = 0;
	_entries.clear();
	_entries.reserve(_used.size());
	for (const auto index : _used) {
		const auto count = float(_count[index]);
		_entries.push_back({
			.color = { {
				_sums[index * 3] / count,
				_sums[index * 3 + 1] / count,
				_sums[index * 3 + 2] / count,
			} },
			.weight = count,
		});
	}
	if (_entries.empty() || maxColors <= 0) {
		return;
	}
	maxColors = std::min(maxColors, 256);
	auto boxes = std::vector<Box>();
	boxes.reserve(maxColors);
	boxes.push_back({ .begin = 0, .end = int(_entries.size()) });
	measure(boxes.back());
	while (int(boxes.size()) < maxColors) {
		auto best = -1;
		for (auto i = 0, count = int(boxes.size()); i != count; ++i) {
			if (boxes[i].score > 0.
				&& (best < 0 || boxes[i].score > boxes[best].score)) {
				best = i;
			}
		}
		if (best < 0) {
			break;
		}
		auto &box = boxes[best];
		const auto axis = box.axis;
		const auto from = begin(_entries) + box.begin;
		const auto till = begin(_entries) + box.end;
		const auto middle = std::partition(from, till, [&](const Entry &e) {
			return e.color[axis] <= box.mean;
		});
		const auto split = std::clamp(
			int(middle - begin(_entries)),
			box.begin + 1,
			box.end - 1);
		auto second = Box{ .begin = split, .end = box.end };
		box.end = split;
		measure(box);
		measure(second);
		boxes.push_back(second);
	}
	for (const auto &box : boxes) {
		auto sum = std::array<float64, 3>{ { 0., 0., 0. } };
		auto total = 0.;
		for (auto i = box.begin; i != box.end; ++i) {
			const auto &entry = _entries[i];
			total += entry.weight;
			for (auto c = 0; c != 3; ++c) {
				sum[c] += entry.weight * entry.color[c];
			}
		}
		const auto to = palette.colors.data() + palette.count * 3;
		for (auto c = 0; c != 3; ++c) {
			to[c] = uint8_t(std::clamp(
				int(std::round(sum[c] / total)),
				0,
				255));
		}
		++palette.count;
	}
}

// The nearest color of a palette, remembered for 18 bit colors.
class GifMapper final {
public:
	GifMapper();

	void reset(const GifPalette &palette);
	[[nodiscard]] int map(int r, int g, int b);

private:
	static constexpr auto kCells = 1 << 18;

	const GifPalette *_palette = nullptr;
	std::vector<int16_t> _cache;

};

GifMapper::GifMapper() : _cache(kCells, int16_t(-1)) {
}

void GifMapper::reset(const GifPalette &palette) {
	_palette = &palette;
	std::fill(begin(_cache), end(_cache), int16_t(-1));
}

int GifMapper::map(int r, int g, int b) {
	const auto cell = ((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2);
	if (const auto cached = _cache[cell]; cached >= 0) {
		return cached;
	}
	auto best = 0;
	auto bestDistance = std::numeric_limits<int>::max();
	const auto colors = _palette->colors.data();
	for (auto i = 0, count = _palette->count; i != count; ++i) {
		const auto dr = r - int(colors[i * 3]);
		const auto dg = g - int(colors[i * 3 + 1]);
		const auto db = b - int(colors[i * 3 + 2]);
		const auto distance = kGifWeightR * dr * dr
			+ kGifWeightG * dg * dg
			+ kGifWeightB * db * db;
		if (distance < bestDistance) {
			bestDistance = distance;
			best = i;
		}
	}
	_cache[cell] = int16_t(best);
	return best;
}

// Variable code size LZW of GIF, with the image data sub-blocks.
class GifLzw final {
public:
	GifLzw();

	void encode(
		QByteArray &to,
		const uint8_t *indices,
		int count,
		int minCodeSize);

private:
	static constexpr auto kTableBits = 14;
	static constexpr auto kTableSize = 1 << kTableBits;
	static constexpr auto kMaxCode = 4095;

	std::vector<int32> _keys;
	std::vector<uint16> _codes;

};

GifLzw::GifLzw() : _keys(kTableSize, -1), _codes(kTableSize, 0) {
}

void GifLzw::encode(
		QByteArray &to,
		const uint8_t *indices,
		int count,
		int minCodeSize) {
	to.append(char(minCodeSize));

	auto block = std::array<uint8_t, 255>();
	auto blockSize = 0;
	auto bits = uint32(0);
	auto bitsCount = 0;
	const auto push = [&](uint8_t byte) {
		block[blockSize++] = byte;
		if (blockSize == 255) {
			to.append(char(255));
			to.append(reinterpret_cast<const char*>(block.data()), 255);
			blockSize = 0;
		}
	};
	const auto write = [&](uint32 code, int size) {
		bits |= (code << bitsCount);
		bitsCount += size;
		while (bitsCount >= 8) {
			push(uint8_t(bits & 0xFF));
			bits >>= 8;
			bitsCount -= 8;
		}
	};
	const auto reset = [&] {
		std::fill(begin(_keys), end(_keys), -1);
	};

	const auto clearCode = 1 << minCodeSize;
	auto codeSize = minCodeSize + 1;
	auto maxCode = clearCode + 1;
	reset();
	write(clearCode, codeSize);
	if (count > 0) {
		auto current = int(indices[0]);
		for (auto i = 1; i != count; ++i) {
			const auto next = int(indices[i]);
			const auto key = (current << 8) | next;
			auto slot = int((uint32(key) * 2654435761U) >> (32 - kTableBits));
			while (_keys[slot] != -1 && _keys[slot] != key) {
				slot = (slot + 1) & (kTableSize - 1);
			}
			if (_keys[slot] == key) {
				current = _codes[slot];
				continue;
			}
			write(current, codeSize);
			++maxCode;
			_keys[slot] = key;
			_codes[slot] = uint16(maxCode);
			if (maxCode >= (1 << codeSize)) {
				++codeSize;
			}
			if (maxCode == kMaxCode) {
				write(clearCode, codeSize);
				reset();
				codeSize = minCodeSize + 1;
				maxCode = clearCode + 1;
			}
			current = next;
		}
		write(current, codeSize);

		// A decoder adds a code to its table after this one as well,
		// so it may read the next one already with a bit more.
		if (maxCode + 1 >= (1 << codeSize) && codeSize < 12) {
			++codeSize;
		}
	}
	write(clearCode, codeSize);
	write(clearCode + 1, minCodeSize + 1);
	if (bitsCount > 0) {
		push(uint8_t(bits & 0xFF));
	}
	if (blockSize > 0) {
		to.append(char(blockSize));
		to.append(reinterpret_cast<const char*>(block.data()), blockSize);
	}
	to.append(char(0));
}

void AppendLE16(QByteArray &to, int value) {
	to.append(char(value & 0xFF));
	to.append(char((value >> 8) & 0xFF));
}

// MP4.

struct Mp4Descriptor {
	QSize size;
	int bitrate = 0;
	int fps = 30;
	int audioRate = 0;
	int audioChannels = 0;
	int rotation = 0; // Display matrix, used for self-test sources.
};

class Mp4Writer final {
public:
	explicit Mp4Writer(Mp4Descriptor descriptor);

	[[nodiscard]] bool valid() const {
		return !_failed;
	}
	[[nodiscard]] QSize size() const {
		return _descriptor.size;
	}
	[[nodiscard]] bool hasAudio() const {
		return (_audioCodec != nullptr);
	}

	// A writable YUV420P frame to fill before writeVideo().
	[[nodiscard]] AVFrame *prepareVideoFrame();
	[[nodiscard]] bool writeVideo(int64 position, int64 duration);

	// Interleaved samples, audioChannels per frame.
	[[nodiscard]] bool writeAudio(const float *samples, int64 frames);

	// A hole in the timestamps of the audio track, for self-test sources.
	void skipAudio(int64 frames) {
		_audioPts += frames;
	}

	[[nodiscard]] QByteArray finish();

private:
	[[nodiscard]] bool initVideo();
	[[nodiscard]] bool initAudio();
	[[nodiscard]] bool encodeAudio(int frames);
	[[nodiscard]] bool writeFrame(
		AVFrame *frame,
		const CodecPointer &codec,
		AVStream *stream);
	bool fail();

	Mp4Descriptor _descriptor;

	WriteBytesWrap _result; // Before _format, it is destroyed after it.
	FormatPointer _format;

	AVStream *_videoStream = nullptr;
	CodecPointer _videoCodec;
	FramePointer _videoFrame;

	AVStream *_audioStream = nullptr;
	CodecPointer _audioCodec;
	FramePointer _audioFrame;
	std::vector<float> _audioQueue;
	size_t _audioOffset = 0; // What is already encoded, till it is erased.
	int64 _audioPts = 0;

	bool _failed = false;

};

Mp4Writer::Mp4Writer(Mp4Descriptor descriptor)
: _descriptor(descriptor) {
	_descriptor.size = QSize(
		_descriptor.size.width() & ~1,
		_descriptor.size.height() & ~1);
	_descriptor.fps = std::clamp(_descriptor.fps, 1, 60);
	if (_descriptor.size.width() < kMp4MinSide
		|| _descriptor.size.height() < kMp4MinSide) {
		fail();
		return;
	}
	if (_descriptor.bitrate <= 0) {
		const auto pixels = int64(_descriptor.size.width())
			* _descriptor.size.height()
			* _descriptor.fps;
		_descriptor.bitrate = int(std::clamp(
			(pixels * 15) / 100,
			int64(400'000),
			int64(12'000'000)));
	}
	const auto audio = (_descriptor.audioRate > 0)
		&& (_descriptor.audioChannels > 0);
	_format = MakeWriteFormatPointer(
		static_cast<void*>(&_result),
		nullptr,
		&WriteBytesWrap::Write,
		&WriteBytesWrap::Seek,
		"mp4"_q);
	if (!_format || !initVideo() || (audio && !initAudio())) {
		fail();
		return;
	}
	const auto error = AvErrorWrap(avformat_write_header(
		_format.get(),
		nullptr));
	if (error) {
		LogError(u"avformat_write_header"_q, error);
		fail();
	}
}

bool Mp4Writer::fail() {
	_failed = true;
	return false;
}

bool Mp4Writer::initVideo() {
	auto codec = avcodec_find_encoder_by_name("libopenh264");
	if (!codec) {
		codec = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!codec) {
			LogError(u"avcodec_find_encoder"_q, u"AV_CODEC_ID_H264"_q);
			return false;
		}
	}
	_videoStream = avformat_new_stream(_format.get(), codec);
	if (!_videoStream) {
		LogError(u"avformat_new_stream"_q, u"video"_q);
		return false;
	}
	_videoCodec = CodecPointer(avcodec_alloc_context3(codec));
	if (!_videoCodec) {
		LogError(u"avcodec_alloc_context3"_q, u"video"_q);
		return false;
	}
	_videoCodec->codec_id = codec->id;
	_videoCodec->codec_type = AVMEDIA_TYPE_VIDEO;
	_videoCodec->width = _descriptor.size.width();
	_videoCodec->height = _descriptor.size.height();
	_videoCodec->time_base = AVRational{ 1, int(kSecond) };
	_videoCodec->framerate = AVRational{ _descriptor.fps, 1 };
	_videoCodec->pix_fmt = AV_PIX_FMT_YUV420P;
	_videoCodec->bit_rate = _descriptor.bitrate;
	_videoCodec->gop_size = _descriptor.fps * 2;

	// What RgbToYuv() gives. Without the tags players take HD videos
	// for BT.709 and shift the colors.
	_videoCodec->color_range = AVCOL_RANGE_MPEG;
	_videoCodec->colorspace = AVCOL_SPC_SMPTE170M;
	_videoCodec->color_primaries = AVCOL_PRI_BT709;
	_videoCodec->color_trc = AVCOL_TRC_BT709;
	if (_format->oformat->flags & AVFMT_GLOBALHEADER) {
		_videoCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	auto error = AvErrorWrap(avcodec_open2(
		_videoCodec.get(),
		codec,
		nullptr));
	if (error) {
		LogError(u"avcodec_open2"_q, error, u"video"_q);
		return false;
	}
	error = AvErrorWrap(avcodec_parameters_from_context(
		_videoStream->codecpar,
		_videoCodec.get()));
	if (error) {
		LogError(u"avcodec_parameters_from_context"_q, error);
		return false;
	}
	_videoStream->time_base = _videoCodec->time_base;
	_videoStream->avg_frame_rate = _videoCodec->framerate;
	if (_descriptor.rotation) {
		const auto data = av_packet_side_data_new(
			&_videoStream->codecpar->coded_side_data,
			&_videoStream->codecpar->nb_coded_side_data,
			AV_PKT_DATA_DISPLAYMATRIX,
			sizeof(int32_t) * 9,
			0);
		if (data) {
			// _set() takes a clockwise angle, _get() returns
			// a counterclockwise one that ReadRotationFromMetadata()
			// negates back, so the reader gets exactly this value.
			av_display_rotation_set(
				reinterpret_cast<int32_t*>(data->data),
				_descriptor.rotation);
		}
	}

	_videoFrame = MakeFramePointer();
	if (!_videoFrame) {
		return false;
	}
	_videoFrame->format = _videoCodec->pix_fmt;
	_videoFrame->width = _videoCodec->width;
	_videoFrame->height = _videoCodec->height;
	error = AvErrorWrap(av_frame_get_buffer(_videoFrame.get(), 0));
	if (error) {
		LogError(u"av_frame_get_buffer"_q, error, u"video"_q);
		return false;
	}
	return true;
}

bool Mp4Writer::initAudio() {
	const auto codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!codec) {
		LogError(u"avcodec_find_encoder"_q, u"AAC"_q);
		return false;
	}
	_audioStream = avformat_new_stream(_format.get(), codec);
	if (!_audioStream) {
		LogError(u"avformat_new_stream"_q, u"AAC"_q);
		return false;
	}
	_audioCodec = CodecPointer(avcodec_alloc_context3(codec));
	if (!_audioCodec) {
		LogError(u"avcodec_alloc_context3"_q, u"AAC"_q);
		return false;
	}
	_descriptor.audioChannels = std::clamp(_descriptor.audioChannels, 1, 2);
	_audioCodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
	_audioCodec->bit_rate = (kMp4AudioBitRate * _descriptor.audioChannels)
		/ 2;
	_audioCodec->sample_rate = _descriptor.audioRate;
	_audioCodec->time_base = AVRational{ 1, _descriptor.audioRate };
	av_channel_layout_default(
		&_audioCodec->ch_layout,
		_descriptor.audioChannels);
	if (_format->oformat->flags & AVFMT_GLOBALHEADER) {
		_audioCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	auto error = AvErrorWrap(avcodec_open2(
		_audioCodec.get(),
		codec,
		nullptr));
	if (error) {
		LogError(u"avcodec_open2"_q, error, u"AAC"_q);
		return false;
	}
	error = AvErrorWrap(avcodec_parameters_from_context(
		_audioStream->codecpar,
		_audioCodec.get()));
	if (error) {
		LogError(u"avcodec_parameters_from_context"_q, error, u"AAC"_q);
		return false;
	}

	_audioFrame = MakeFramePointer();
	if (!_audioFrame) {
		return false;
	}
	_audioFrame->nb_samples = _audioCodec->frame_size;
	_audioFrame->format = _audioCodec->sample_fmt;
	_audioFrame->sample_rate = _audioCodec->sample_rate;
	av_channel_layout_copy(&_audioFrame->ch_layout, &_audioCodec->ch_layout);
	error = AvErrorWrap(av_frame_get_buffer(_audioFrame.get(), 0));
	if (error) {
		LogError(u"av_frame_get_buffer"_q, error, u"AAC"_q);
		return false;
	}
	return true;
}

AVFrame *Mp4Writer::prepareVideoFrame() {
	if (_failed) {
		return nullptr;
	}
	const auto error = AvErrorWrap(av_frame_make_writable(
		_videoFrame.get()));
	if (error) {
		LogError(u"av_frame_make_writable"_q, error);
		fail();
		return nullptr;
	}
	return _videoFrame.get();
}

bool Mp4Writer::writeVideo(int64 position, int64 duration) {
	if (_failed) {
		return false;
	}
	_videoFrame->pts = position;
	_videoFrame->duration = duration;
	return writeFrame(_videoFrame.get(), _videoCodec, _videoStream);
}

bool Mp4Writer::writeAudio(const float *samples, int64 frames) {
	if (_failed || !_audioCodec) {
		return false;
	} else if (frames <= 0) {
		return true;
	}
	const auto channels = _descriptor.audioChannels;
	_audioQueue.insert(
		end(_audioQueue),
		samples,
		samples + frames * channels);

	// The encoded part is erased once for the whole call: erasing it frame
	// by frame moves all the rest every time.
	const auto frameSize = _audioCodec->frame_size;
	const auto block = size_t(frameSize) * channels;
	auto result = true;
	while (result && (_audioQueue.size() - _audioOffset >= block)) {
		result = encodeAudio(frameSize);
	}
	_audioQueue.erase(
		begin(_audioQueue),
		begin(_audioQueue) + _audioOffset);
	_audioOffset = 0;
	return result;
}

bool Mp4Writer::encodeAudio(int frames) {
	const auto error = AvErrorWrap(av_frame_make_writable(
		_audioFrame.get()));
	if (error) {
		LogError(u"av_frame_make_writable"_q, error, u"AAC"_q);
		return fail();
	}
	const auto channels = _descriptor.audioChannels;
	const auto from = _audioQueue.data() + _audioOffset;
	_audioFrame->nb_samples = frames;
	for (auto channel = 0; channel != channels; ++channel) {
		const auto to = reinterpret_cast<float*>(
			_audioFrame->data[channel]);
		for (auto i = 0; i != frames; ++i) {
			to[i] = from[i * channels + channel];
		}
	}
	_audioOffset += size_t(frames) * channels;
	_audioFrame->pts = _audioPts;
	_audioPts += frames;
	return writeFrame(_audioFrame.get(), _audioCodec, _audioStream);
}

bool Mp4Writer::writeFrame(
		AVFrame *frame,
		const CodecPointer &codec,
		AVStream *stream) {
	auto error = AvErrorWrap(avcodec_send_frame(codec.get(), frame));
	if (error) {
		LogError(u"avcodec_send_frame"_q, error);
		return fail();
	}
	auto packet = av_packet_alloc();
	const auto guard = gsl::finally([&] {
		av_packet_free(&packet);
	});
	while (true) {
		error = AvErrorWrap(avcodec_receive_packet(codec.get(), packet));
		if (error.code() == AVERROR(EAGAIN)
			|| error.code() == AVERROR_EOF) {
			return true;
		} else if (error) {
			LogError(u"avcodec_receive_packet"_q, error);
			return fail();
		}
		if (frame
			&& (packet->duration <= 0)
			&& (codec->codec_type == AVMEDIA_TYPE_VIDEO)) {
			// libopenh264 gives the packet of a frame at once, but without
			// the duration: the muxer would take the one of the frame rate
			// for the last frame.
			packet->duration = frame->duration;
		}
		packet->stream_index = stream->index;
		av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
		error = AvErrorWrap(av_interleaved_write_frame(
			_format.get(),
			packet));
		if (error) {
			LogError(u"av_interleaved_write_frame"_q, error);
			return fail();
		}
	}
}

QByteArray Mp4Writer::finish() {
	if (_failed) {
		return QByteArray();
	}
	if (_audioCodec) {
		const auto left = int(_audioQueue.size() - _audioOffset)
			/ _descriptor.audioChannels;
		if (left > 0 && !encodeAudio(left)) {
			return QByteArray();
		}
	}
	if (!writeFrame(nullptr, _videoCodec, _videoStream)
		|| (_audioCodec
			&& !writeFrame(nullptr, _audioCodec, _audioStream))) {
		return QByteArray();
	}
	const auto error = AvErrorWrap(av_write_trailer(_format.get()));
	if (error) {
		LogError(u"av_write_trailer"_q, error);
		fail();
		return QByteArray();
	}
	_format = nullptr;
	_failed = true; // Nothing more can be written.
	return base::take(_result.content);
}

// The size of a clip: the crop scaled down to fit maxSide.
[[nodiscard]] QSize ClipSize(QSize crop, int maxSide, bool even) {
	const auto longer = std::max(crop.width(), crop.height());
	if (longer <= 0) {
		return QSize();
	}
	const auto scale = (maxSide > 0 && longer > maxSide)
		? (maxSide / float64(longer))
		: 1.;
	auto width = std::max(int(std::round(crop.width() * scale)), 1);
	auto height = std::max(int(std::round(crop.height() * scale)), 1);
	if (even) {
		width = std::max(width & ~1, 2);
		height = std::max(height & ~1, 2);
	}
	return QSize(width, height);
}

// Writes an image as the next frame, positions in microseconds.
[[nodiscard]] bool WriteMp4Image(
		Mp4Writer &writer,
		const QImage &image,
		std::vector<uint32> &rgb,
		int64 position,
		int64 length) {
	const auto size = writer.size();
	const auto prepared = Prepared(image, size);
	const auto to = prepared.isNull() ? nullptr : writer.prepareVideoFrame();
	if (!to) {
		return false;
	}
	ColorsOnWhite(prepared, rgb);
	RgbToYuv(
		rgb,
		size.width(),
		size.height(),
		to->data[0],
		to->linesize[0],
		to->data[1],
		to->linesize[1],
		to->data[2],
		to->linesize[2]);
	return writer.writeVideo(position, length);
}

// Sequences of fragments.

struct SequencePart {
	ClipPart part; // from, till and crop are valid here.
	ClipInfo info;
	crl::time length = 0; // An estimate for the progress.
	int maxFps = 0;
};

[[nodiscard]] std::vector<ClipPart> PartsOf(
		const QString &path,
		const QByteArray &content,
		crl::time from,
		crl::time till,
		QRect crop,
		const std::vector<ClipPart> &parts) {
	if (!parts.empty()) {
		return parts;
	}
	return { ClipPart{
		.path = path,
		.content = content,
		.from = from,
		.till = till,
		.crop = crop,
	} };
}

// Empty with an error if one of the fragments can't be read.
[[nodiscard]] std::vector<SequencePart> PlanSequence(
		std::vector<ClipPart> parts,
		bool centerSquare,
		int maxFps,
		QString &error) {
	auto result = std::vector<SequencePart>();
	result.reserve(parts.size());
	for (auto &part : parts) {
		auto info = ReadClipInfo(part.path, part.content);
		if (!info.valid()) {
			error = u"Could not read the video."_q;
			return {};
		}
		const auto whole = QRect(QPoint(), info.size);
		const auto usual = centerSquare ? CenterSquare(info.size) : whole;
		part.crop = part.crop.isEmpty()
			? usual
			: part.crop.intersected(whole);
		if (part.crop.isEmpty()) {
			part.crop = usual;
		}
		part.from = std::max(part.from, crl::time(0));
		part.till = (part.till > part.from) ? part.till : crl::time(0);
		const auto end = part.till
			? std::min(part.till, info.duration)
			: info.duration;
		const auto length = std::max(end - part.from, crl::time(1));
		const auto keepRate = (info.fps > maxFps)
			&& (info.fps <= maxFps * kKeptRateExcess);
		result.push_back({
			.part = std::move(part),
			.info = std::move(info),
			.length = length,
			.maxFps = keepRate ? 0 : maxFps,
		});
	}
	if (result.empty()) {
		error = u"No fragments."_q;
	}
	return result;
}

[[nodiscard]] crl::time SequenceLength(
		const std::vector<SequencePart> &parts) {
	auto result = crl::time(0);
	for (const auto &part : parts) {
		result += part.length;
	}
	return std::max(result, crl::time(1));
}

// The size fitted into the box with its proportions kept.
[[nodiscard]] QSize FitInto(QSize size, QSize box) {
	if (size.isEmpty() || box.isEmpty()) {
		return box;
	}
	auto width = box.width();
	auto height = int(std::round(
		size.height() * float64(box.width()) / size.width()));
	if (height > box.height()) {
		height = box.height();
		width = int(std::round(
			size.width() * float64(box.height()) / size.height()));
	}
	// The same proportions with a rounding error take the whole box.
	return (std::abs(width - box.width()) <= 1
		&& std::abs(height - box.height()) <= 1)
		? box
		: QSize(
			std::clamp(width, 1, box.width()),
			std::clamp(height, 1, box.height()));
}

// Frames of all the fragments one after another, every one of the size
// given: a fragment with other proportions is centered on the fill color
// (premultiplied). Positions go on from one fragment to the next one.
// The callback gets the index of the fragment and returns false to stop.
[[nodiscard]] bool ReadSequence(
		const std::vector<SequencePart> &parts,
		QSize size,
		QRgb fill,
		const Fn<bool(Frame &&frame, int part)> &callback,
		const Cancel &cancel,
		QString *error) {
	auto offset = crl::time(0);
	for (auto i = 0, count = int(parts.size()); i != count; ++i) {
		const auto &part = parts[i].part;
		auto end = offset;
		auto stopped = false;
		const auto read = ReadFrames({
			.path = part.path,
			.content = part.content,
			.from = part.from,
			.till = part.till,
			.crop = part.crop,
			.size = FitInto(part.crop.size(), size),
			.maxFps = parts[i].maxFps,
		}, [&](Frame &&frame) {
			frame.image = Centered(frame.image, size, fill);
			frame.position += offset;
			end = frame.position + frame.duration;
			if (frame.image.isNull() || !callback(std::move(frame), i)) {
				stopped = true;
				return false;
			}
			return true;
		}, cancel, error);
		if (!read) {
			return false;
		} else if (stopped) {
			return true;
		}
		offset = end;
	}
	return true;
}

// Audio.

constexpr auto kAudioRate = 48000;
constexpr auto kAudioChannels = 2;

// A hole in the timestamps of the audio track is filled with silence, so
// the sound after it stays in step with the video. Timestamps are rounded
// and jitter a bit: only a hole big enough to be noticed (50 ms) counts.
// Samples are never dropped: timestamps that go back are mostly broken
// ones, the sound just goes on. A jump back by more than the limit (5 s),
// or forward beyond the end of the file by more than it, is taken for
// timestamps that start anew.
constexpr auto kAudioSyncTolerance = int64(kAudioRate / 20);
constexpr auto kAudioSyncLimit = int64(kAudioRate * 5);

// A long frame of the video gets its audio by pieces, so the memory taken
// doesn't depend on how long it is and the cancel flag is checked.
constexpr auto kAudioPiece = kSecond;

// The audio of a fragment as interleaved float samples, read in step
// with the video: every call gives the samples up to a position, exactly
// as many of them as the time takes. Silence where the track has nothing
// and when there is no track at all.
class AudioReader final {
public:
	explicit AudioReader(const SequencePart &part);

	// Appends the samples. The position is in microseconds from the
	// start of the fragment and only grows.
	void read(int64 till, std::vector<float> &to);

private:
	void decodeMore();
	void feed();
	void handle(not_null<AVFrame*> frame);
	void sync(int64 position);
	void append(not_null<AVFrame*> frame);
	void convert(const uint8_t **data, int samples);

	[[nodiscard]] int64 queued() const {
		return int64(_queue.size() / kAudioChannels);
	}

	Input _input;
	AVStream *_stream = nullptr;
	CodecPointer _codec;
	FramePointer _frame;
	Packet _packet;
	SwresamplePointer _swresample;
	AVChannelLayout _layout = {};

	std::vector<float> _buffer;
	std::vector<float> _queue; // Decoded and not given yet.
	const int64 _from = 0;
	const int64 _end = 0; // Of the file, in frames from _from, 0 if unknown.
	int64 _given = 0; // Frames.
	int64 _skip = 0; // Decoded frames before the start of the fragment.
	int64 _lead = 0; // Silence before the track starts.

	// Where the next converted sample goes, in frames from the start of
	// the fragment (it is before the start by _skip while that is not 0).
	int64 _tail = 0;
	int64 _gap = 0; // Silence after the queue, _frame waits till it is given.
	int64 _shift = 0; // Added to the timestamps after they started anew.
	bool _held = false;
	bool _started = false;
	bool _eof = false;
	bool _finished = true;

};

AudioReader::AudioReader(const SequencePart &part)
: _from(part.part.from * int64(1000))
, _end(av_rescale(
	std::max(part.info.duration * int64(1000) - _from, int64(0)),
	kAudioRate,
	kSecond)) {
	av_channel_layout_default(&_layout, kAudioChannels);
	if (!part.info.hasAudio
		|| !_input.open(part.part.path, part.part.content)) {
		return;
	}
	const auto format = _input.format();
	const auto index = FindStream(format, AVMEDIA_TYPE_AUDIO);
	if (index < 0) {
		return;
	}
	_stream = format->streams[index];
	_codec = MakeCodecPointer({ .stream = _stream });
	_frame = MakeFramePointer();
	if (!_codec || !_frame) {
		return;
	}
	if (_from > 0) {
		const auto error = AvErrorWrap(av_seek_frame(
			format,
			index,
			_input.pts(_from, _stream),
			AVSEEK_FLAG_BACKWARD));
		if (error) {
			// Decode from the beginning, earlier samples are skipped.
			LogError(u"av_seek_frame"_q, error, u"audio"_q);
		}
		avcodec_flush_buffers(_codec.get());
	}
	_finished = false;
}

void AudioReader::read(int64 till, std::vector<float> &to) {
	const auto target = av_rescale(
		std::max(till, int64(0)),
		kAudioRate,
		kSecond);
	auto need = target - _given;
	if (need <= 0) {
		return;
	}
	_given = target;
	while (!_finished && (_lead + queued() < need)) {
		if (_gap > 0) {
			// Only what is asked for: a hole may be of any length.
			const auto hole = std::min(_gap, need - _lead - queued());
			_gap -= hole;
			_queue.insert(end(_queue), hole * kAudioChannels, 0.f);
		} else {
			decodeMore();
		}
	}
	const auto silence = std::min(_lead, need);
	_lead -= silence;
	need -= silence;
	const auto take = std::min(queued(), need);
	need -= take;
	to.reserve(to.size() + (silence + take + need) * kAudioChannels);
	to.insert(end(to), silence * kAudioChannels, 0.f);
	to.insert(
		end(to),
		begin(_queue),
		begin(_queue) + take * kAudioChannels);
	_queue.erase(begin(_queue), begin(_queue) + take * kAudioChannels);
	to.insert(end(to), need * kAudioChannels, 0.f);
}

// One decoded frame for a call: the next one may have to wait for a hole
// before it to be given.
void AudioReader::decodeMore() {
	if (_held) {
		_held = false;
		append(_frame.get());
		av_frame_unref(_frame.get());
		return;
	}
	while (!_finished) {
		const auto received = AvErrorWrap(avcodec_receive_frame(
			_codec.get(),
			_frame.get()));
		if (!received) {
			handle(_frame.get());
			if (!_held) {
				av_frame_unref(_frame.get());
			}
			return;
		} else if (_eof) {
			// What the resampler still keeps.
			convert(nullptr, 0);
			_finished = true;
			return;
		}
		feed();
	}
}

// The next packet of the track goes to the decoder.
void AudioReader::feed() {
	const auto format = _input.format();
	auto &fields = _packet.fields();
	while (true) {
		if (av_read_frame(format, &fields) < 0) {
			_eof = true;
			avcodec_send_packet(_codec.get(), nullptr);
			return;
		}
		const auto mine = (fields.stream_index == _stream->index);
		if (mine) {
			// Broken packets are skipped.
			avcodec_send_packet(_codec.get(), &fields);
		}
		av_packet_unref(&fields);
		if (mine) {
			return;
		}
	}
}

void AudioReader::handle(not_null<AVFrame*> frame) {
	if (frame->nb_samples <= 0 || frame->sample_rate <= 0) {
		return;
	}
	const auto pts = FramePts(frame);
	const auto position = (pts != AV_NOPTS_VALUE)
		? (_input.position(pts, _stream) - _from)
		: int64(0);
	if (!_started) {
		const auto length = av_rescale(
			frame->nb_samples,
			kSecond,
			frame->sample_rate);
		if (position + length <= 0) {
			return;
		}
		_started = true;
		if (position < 0) {
			_skip = av_rescale(-position, kAudioRate, kSecond);
		} else {
			_lead = av_rescale(position, kAudioRate, kSecond);
			_tail = _lead;
		}
	} else if (pts != AV_NOPTS_VALUE) {
		sync(position);
		if (_gap > 0) {
			_held = true;
			return;
		}
	}
	append(frame);
}

// A hole in the timestamps before the frame at the position (microseconds
// from the start of the fragment) becomes silence, see kAudioSyncTolerance.
void AudioReader::sync(int64 position) {
	const auto expected = _shift + ((position < 0)
		? -av_rescale(-position, kAudioRate, kSecond)
		: av_rescale(position, kAudioRate, kSecond));
	const auto ahead = [&] {
		return expected - (_tail - _skip);
	};
	if (ahead() < -kAudioSyncLimit
		|| (ahead() > kAudioSyncTolerance
			&& _end > 0
			&& expected > _end + kAudioSyncLimit)) {
		_shift -= ahead();
	} else if (ahead() > kAudioSyncTolerance) {
		// What the resampler still keeps is from before the hole.
		convert(nullptr, 0);
		_swresample = nullptr;

		// A hole before the start of the fragment: less to skip.
		const auto skipped = std::clamp(ahead(), int64(0), _skip);
		_skip -= skipped;
		_gap = std::max(ahead(), int64(0));
		_tail += _gap;
	}
}

void AudioReader::append(not_null<AVFrame*> frame) {
	auto layout = AVChannelLayout();
	if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC
		|| frame->ch_layout.nb_channels <= 0) {
		av_channel_layout_default(
			&layout,
			std::max(frame->ch_layout.nb_channels, 1));
	} else {
		av_channel_layout_copy(&layout, &frame->ch_layout);
	}
	_swresample = MakeSwresamplePointer(
		&layout,
		AVSampleFormat(frame->format),
		frame->sample_rate,
		&_layout,
		AV_SAMPLE_FMT_FLT,
		kAudioRate,
		&_swresample);
	av_channel_layout_uninit(&layout);
	if (!_swresample) {
		_finished = true;
		return;
	}
	convert(
		const_cast<const uint8_t**>(frame->extended_data),
		frame->nb_samples);
}

// Without the data it gives what the resampler still keeps.
void AudioReader::convert(const uint8_t **data, int samples) {
	if (!_swresample) {
		return;
	}
	const auto limit = swr_get_out_samples(_swresample.get(), samples);
	if (limit <= 0) {
		return;
	}
	_buffer.resize(size_t(limit) * kAudioChannels);
	auto to = reinterpret_cast<uint8_t*>(_buffer.data());
	const auto got = swr_convert(
		_swresample.get(),
		&to,
		limit,
		data,
		samples);
	if (got <= 0) {
		return;
	}
	const auto skip = std::min(_skip, int64(got));
	_skip -= skip;
	_tail += got - skip;
	_queue.insert(
		end(_queue),
		begin(_buffer) + skip * kAudioChannels,
		begin(_buffer) + got * kAudioChannels);
}

// H.264 mp4 of the fragments, with the audio or without it.
[[nodiscard]] ClipResult MakeMp4(
		const ClipOptions &options,
		VideoOptions video,
		int defaultMaxSide,
		int defaultMaxFps,
		const Fn<void(float64)> &progress,
		const Cancel &cancel) {
	auto result = ClipResult();
	const auto maxFps = (options.maxFps > 0) ? options.maxFps : defaultMaxFps;
	const auto parts = PlanSequence(PartsOf(
		options.path,
		options.content,
		options.from,
		options.till,
		options.crop,
		options.parts), false, maxFps, result.error);
	if (parts.empty()) {
		return result;
	}
	const auto &first = parts.front();
	const auto fitted = ClipSize(
		first.part.crop.size(),
		(options.maxSide > 0) ? options.maxSide : defaultMaxSide,
		true);

	// The encoder takes nothing smaller, a tiny crop is fitted into it.
	const auto size = QSize(
		std::max(fitted.width(), kMp4MinSide),
		std::max(fitted.height(), kMp4MinSide));
	const auto audio = !video.mute
		&& ranges::any_of(parts, [](const SequencePart &part) {
			return part.info.hasAudio;
		});
	auto encoder = Mp4Encoder(size, {
		.bitrate = video.bitrate,
		.fps = (first.info.fps > 0. && first.info.fps < maxFps)
			? std::max(int(std::round(first.info.fps)), 1)
			: maxFps,
		.audioRate = audio ? kAudioRate : 0,
		.audioChannels = audio ? kAudioChannels : 0,
	});
	if (!encoder.valid()) {
		result.error = u"Could not create the H.264 encoder."_q;
		return result;
	}
	const auto length = SequenceLength(parts);
	auto failed = false;
	auto reader = std::unique_ptr<AudioReader>();
	auto readerPart = -1;
	auto readerStart = crl::time(0);
	auto readerGiven = int64(0);
	auto samples = std::vector<float>();
	const auto black = QRgb(0xFF000000U);
	const auto read = ReadSequence(parts, size, black, [&](
			Frame &&frame,
			int part) {
		const auto end = frame.position + frame.duration;
		if (!encoder.add(frame.image, frame.duration)) {
			failed = true;
			return false;
		}
		++result.frames;
		if (audio) {
			if (readerPart != part) {
				reader = std::make_unique<AudioReader>(parts[part]);
				readerPart = part;
				readerStart = frame.position;
				readerGiven = 0;
			}
			const auto till = (end - readerStart) * int64(1000);
			while (readerGiven < till) {
				if (Cancelled(cancel)) {
					return false;
				}
				readerGiven = std::min(readerGiven + kAudioPiece, till);
				samples.clear();
				reader->read(readerGiven, samples);
				if (!samples.empty()
					&& !encoder.addAudio(
						samples.data(),
						int64(samples.size() / kAudioChannels))) {
					failed = true;
					return false;
				}
			}
		}
		if (progress) {
			progress(std::clamp(end / float64(length), 0., 1.));
		}
		return true;
	}, cancel, &result.error);
	if (!read) {
		return result;
	} else if (failed) {
		result.error = u"Could not encode a frame."_q;
		return result;
	}
	result.size = encoder.size();
	result.duration = encoder.duration();
	result.content = encoder.finish();
	result.ok = !result.content.isEmpty();
	result.audio = result.ok && audio;
	if (!result.ok) {
		result.error = u"Could not write the mp4 file."_q;
	}
	return result;
}

} // namespace

ClipInfo ReadClipInfo(const QString &path, const QByteArray &content) {
	auto input = std::make_unique<Input>();
	if (!input->open(path, content)) {
		return {};
	}
	auto format = input->format();
	const auto index = FindStream(format, AVMEDIA_TYPE_VIDEO);
	if (index < 0) {
		return {};
	}
	auto stream = format->streams[index];
	const auto parameters = stream->codecpar;
	const auto rotation = ReadRotationFromMetadata(stream);
	const auto descriptor = av_pix_fmt_desc_get(
		AVPixelFormat(parameters->format));
	const auto alphaMode = av_dict_get(
		stream->metadata,
		"alpha_mode",
		nullptr,
		0);
	auto result = ClipInfo();
	result.size = DisplayedSize(
		QSize(parameters->width, parameters->height),
		PixelAspect(AVRational{ 0, 1 }, stream),
		rotation);
	if (parameters->width <= 0 || parameters->height <= 0) {
		return {};
	}
	result.rotation = rotation;
	result.fps = StreamFps(stream);
	result.hasAudio = (FindStream(format, AVMEDIA_TYPE_AUDIO) >= 0);
	result.hasAlpha = (descriptor
		&& (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA))
		|| (alphaMode && alphaMode->value && alphaMode->value[0] == '1');
	result.codec = QString::fromUtf8(avcodec_get_name(parameters->codec_id));
	result.container = QString::fromUtf8(format->iformat->name);
	result.duration = input->duration(stream);
	if (result.duration <= 0) {
		// No duration in the container, scan the packets (GIFs).
		auto packet = Packet();
		auto &fields = packet.fields();
		auto count = 0;
		while (av_read_frame(format, &fields) >= 0) {
			if (fields.stream_index == index
				&& fields.pts != AV_NOPTS_VALUE) {
				const auto end = input->position(
					fields.pts + std::max(fields.duration, int64(0)),
					stream);
				result.duration = std::max(
					result.duration,
					crl::time((end + 999) / 1000));
				++count;
			}
			av_packet_unref(&fields);
		}
		if (result.fps <= 0. && result.duration > 0) {
			result.fps = (count * 1000.) / result.duration;
		}
	}
	return result;
}

bool ReadFrames(
		const FrameOptions &options,
		Fn<bool(Frame &&frame)> callback,
		Cancel cancel,
		QString *error) {
	const auto fail = [&](const QString &text) {
		if (error) {
			*error = text;
		}
		return false;
	};
	if (!callback) {
		return fail(u"No callback."_q);
	}
	auto input = Input();
	if (!input.open(options.path, options.content)) {
		return fail(u"Could not open the video."_q);
	}
	const auto format = input.format();
	const auto index = FindStream(format, AVMEDIA_TYPE_VIDEO);
	if (index < 0) {
		return fail(u"No video stream."_q);
	}
	const auto stream = format->streams[index];
	const auto codec = MakeCodecPointer({ .stream = stream });
	const auto decoded = MakeFramePointer();
	const auto before = MakeFramePointer(); // The last one before 'from'.
	if (!codec || !decoded || !before) {
		return fail(u"Could not create the decoder."_q);
	}

	const auto fromUs = std::max(options.from, crl::time(0)) * int64(1000);
	const auto tillUs = (options.till > 0 && options.till * 1000 > fromUs)
		? (options.till * int64(1000))
		: int64(-1);
	const auto durationUs = input.duration(stream) * int64(1000);

	// The frame rate is limited by a grid of slots: one frame for a slot,
	// so on average there are never more than maxFps of them. A frame
	// may come a bit earlier than its slot starts: timestamps are rounded
	// in some containers (33, 34, 33 ms for 30 fps).
	const auto slotLength = (options.maxFps > 0)
		? (kSecond / options.maxFps)
		: int64(0);
	const auto slotFor = [&](int64 position) {
		return slotLength
			? ((position + slotLength / 10) / slotLength)
			: int64(0);
	};
	if (fromUs > 0) {
		const auto seekError = AvErrorWrap(av_seek_frame(
			format,
			index,
			input.pts(fromUs, stream),
			AVSEEK_FLAG_BACKWARD));
		if (seekError) {
			// Decode from the beginning, earlier frames are skipped.
			LogError(u"av_seek_frame"_q, seekError);
		}
		avcodec_flush_buffers(codec.get());
	}

	auto converter = FrameConverter(stream, options.crop, options.size);

	struct Held {
		QImage image;
		int64 position = 0;
		int64 length = 0; // 0 if unknown.
	};
	auto held = std::optional<Held>();
	auto hasBefore = false;
	auto beforePosition = int64(0);
	auto beforeLength = int64(0);
	auto started = false;
	auto finished = false; // Reached 'till'.
	auto stopped = false; // By the callback.
	auto broken = false;
	auto emitted = 0;
	auto lastPosition = int64(-1);
	auto lastLength = int64(0);
	auto lastSlot = int64(0);

	const auto toMs = [](int64 us) {
		return crl::time((us + 500) / 1000);
	};
	const auto emit = [&](int64 end) {
		auto frame = Frame{
			.image = std::move(held->image),
			.position = toMs(held->position),
			.duration = std::max(
				toMs(end) - toMs(held->position),
				crl::time(1)),
		};
		held = std::nullopt;
		++emitted;
		if (!callback(std::move(frame))) {
			stopped = true;
		}
	};
	const auto accept = [&](
			not_null<AVFrame*> frame,
			int64 position,
			int64 length) {
		auto image = converter.convert(frame);
		if (image.isNull()) {
			// Broken frames are skipped, the previous one stays longer.
			broken = true;
			return;
		}
		if (held) {
			emit(position);
			if (stopped) {
				return;
			}
		} else if (!emitted) {
			// The first frame is shown from the very start, even if the
			// video track begins a bit later than the audio one.
			position = 0;
		}
		held = Held{ std::move(image), position, length };
		lastSlot = slotFor(position);
	};
	const auto handle = [&](not_null<AVFrame*> frame) {
		const auto pts = FramePts(frame);
		const auto length = (frame->duration > 0)
			? av_rescale_q(
				frame->duration,
				stream->time_base,
				kUniversalTimeBase)
			: int64(0);
		const auto position = (pts != AV_NOPTS_VALUE)
			? input.position(pts, stream)
			: (lastPosition < 0)
			? int64(0)
			: (lastPosition + (lastLength ? lastLength : kDefaultFrameGap));
		lastPosition = position;
		lastLength = length;
		if (position < fromUs) {
			av_frame_unref(before.get());
			hasBefore = (av_frame_ref(before.get(), frame) >= 0);
			beforePosition = position;
			beforeLength = length;
			return;
		} else if (tillUs >= 0 && position >= tillUs) {
			finished = true;
			return;
		}
		if (!started) {
			started = true;
			if (hasBefore && position > fromUs) {
				// It is the one shown at 'from'.
				accept(before.get(), 0, 0);
			}
			av_frame_unref(before.get());
			hasBefore = false;
			if (stopped) {
				return;
			}
		}
		const auto at = position - fromUs;
		if (!held || !slotLength || (slotFor(at) > lastSlot)) {
			accept(frame, at, length);
		}
	};

	auto packet = Packet();
	auto &fields = packet.fields();
	auto eof = false;
	while (!finished && !stopped) {
		if (Cancelled(cancel)) {
			return fail(u"Cancelled."_q);
		}
		if (av_read_frame(format, &fields) < 0) {
			eof = true;
			avcodec_send_packet(codec.get(), nullptr);
		} else {
			if (fields.stream_index == index) {
				// Broken packets are skipped.
				avcodec_send_packet(codec.get(), &fields);
			}
			av_packet_unref(&fields);
		}
		while (!finished && !stopped) {
			const auto received = AvErrorWrap(avcodec_receive_frame(
				codec.get(),
				decoded.get()));
			if (received) {
				break;
			}
			handle(decoded.get());
			av_frame_unref(decoded.get());
		}
		if (eof) {
			break;
		}
	}
	if (!started && !stopped && hasBefore) {
		// 'from' is inside of the last frame.
		started = true;
		accept(before.get(), 0, beforeLength
			? std::max(beforePosition + beforeLength - fromUs, int64(0))
			: int64(0));
	}
	if (held && !stopped) {
		auto end = int64(0);
		if (finished) {
			end = tillUs - fromUs;
		} else {
			// The end of the last decoded frame: it may be not the held
			// one, but a frame dropped by the frame rate limit after it.
			const auto later = (lastPosition - fromUs > held->position);
			const auto last = later
				? (lastPosition - fromUs)
				: held->position;
			const auto length = later ? lastLength : held->length;
			const auto average = emitted
				? (held->position / emitted)
				: kDefaultFrameGap;
			end = last
				+ ((length > 0)
					? length
					: (durationUs - fromUs > last)
					? (durationUs - fromUs - last)
					: std::max(average, int64(1000)));
			if (tillUs >= 0) {
				end = std::min(end, tillUs - fromUs);
			}
		}
		emit(end);
	}
	if (Cancelled(cancel)) {
		return fail(u"Cancelled."_q);
	} else if (!emitted) {
		return fail(broken
			? u"Could not convert the frames."_q
			: u"No frames in the range."_q);
	}
	return true;
}

QImage ReadFrame(const FrameOptions &options) {
	auto single = options;
	single.till = 0;
	single.maxFps = 0;
	auto result = QImage();
	const auto read = ReadFrames(single, [&](Frame &&frame) {
		result = std::move(frame.image);
		return false;
	});
	return read ? result : QImage();
}

std::vector<float> ReadAudio(const ClipPart &part) {
	auto error = QString();
	const auto parts = PlanSequence({ part }, false, 0, error);
	if (parts.empty() || !parts.front().info.hasAudio) {
		return {};
	}
	const auto &planned = parts.front();
	auto result = std::vector<float>();
	auto reader = AudioReader(planned);
	reader.read(planned.length * int64(1000), result);
	return result;
}

VideoStickerResult MakeVideoSticker(
		const VideoStickerOptions &options,
		Fn<void(float64)> progress,
		Cancel cancel) {
	auto result = VideoStickerResult();
	auto single = PartsOf(
		options.path,
		options.content,
		options.from,
		options.till,
		options.crop,
		options.parts);
	const auto parts = PlanSequence(
		std::move(single),
		true,
		kStickerMaxFps,
		result.error);
	if (parts.empty()) {
		return result;
	}
	auto maker = StickerMaker(ComputeStickerGeometry(
		parts.front().part.crop.size(),
		options.side));
	if (!maker.valid()) {
		result.error = u"Bad crop."_q;
		return result;
	}
	const auto length = std::min(SequenceLength(parts), kStickerMaxDuration);
	const auto transparent = QRgb(0);
	const auto read = ReadSequence(
		parts,
		maker.content(),
		transparent,
		[&](Frame &&frame, int) {
			const auto end = frame.position + frame.duration;
			maker.add(frame.image, frame.position, frame.duration);
			if (progress) {
				progress(kStickerReadShare
					* std::clamp(end / float64(length), 0., 1.));
			}
			return (end < kStickerMaxDuration);
		},
		cancel,
		&result.error);
	if (!read) {
		return result;
	}
	return maker.encode(progress ? [&](float64 value) {
		progress(kStickerReadShare + (1. - kStickerReadShare) * value);
	} : Fn<void(float64)>(), cancel, TargetBytes(options.side));
}

VideoStickerResult EncodeVideoSticker(
		const std::vector<QImage> &frames,
		crl::time frameDuration,
		int side,
		Fn<void(float64)> progress,
		Cancel cancel) {
	auto result = VideoStickerResult();
	const auto first = ranges::find_if(frames, [](const QImage &frame) {
		return !frame.isNull();
	});
	if (first == end(frames) || frameDuration <= 0) {
		result.error = u"No frames."_q;
		return result;
	}
	auto maker = StickerMaker(ComputeStickerGeometry(first->size(), side));
	if (!maker.valid()) {
		result.error = u"Bad frame size."_q;
		return result;
	}
	constexpr auto kPrepareShare = 0.1;
	const auto count = int(frames.size());

	// Whole milliseconds can't tell the usual frame rates exactly (33 ms
	// is 30.3 fps, already over the limit), so they are read as the rates
	// they stand for. Frames that come up to a tenth faster than the limit
	// are just shown at the limit, the rest is dropped by the same grid
	// of slots as in ReadFrames().
	const auto slot = 1000. / kStickerMaxFps;
	const auto step = [&] {
		for (const auto fps : { 60, 30, 24, 15, 12 }) {
			if (frameDuration == 1000 / fps
				|| frameDuration == (1000 + fps - 1) / fps) {
				return 1000. / fps;
			}
		}
		return (frameDuration < slot && frameDuration * 10 >= slot * 9)
			? slot
			: float64(frameDuration);
	}();
	const auto end = std::min(
		crl::time(std::llround(count * step)),
		kStickerMaxDuration);

	// The frames that stay and when they are shown.
	auto kept = std::vector<std::pair<int, crl::time>>();
	auto lastSlot = int64(0);
	for (auto i = 0; i != count; ++i) {
		const auto at = i * step;
		const auto position = crl::time(std::llround(at));
		const auto index = int64(std::floor((at + slot / 10.) / slot));
		if (position >= kStickerMaxDuration) {
			break;
		} else if (frames[i].isNull()
			|| (!kept.empty() && index <= lastSlot)) {
			continue;
		}
		kept.push_back({ i, kept.empty() ? crl::time(0) : position });
		lastSlot = index;
	}
	for (auto i = 0, total = int(kept.size()); i != total; ++i) {
		if (Cancelled(cancel)) {
			result.error = u"Cancelled."_q;
			return result;
		}
		const auto position = kept[i].second;
		const auto next = (i + 1 < total) ? kept[i + 1].second : end;
		maker.add(frames[kept[i].first], position, next - position);
		if (progress) {
			progress(kPrepareShare * (i + 1) / total);
		}
	}
	return maker.encode(progress ? [&](float64 value) {
		progress(kPrepareShare + (1. - kPrepareShare) * value);
	} : Fn<void(float64)>(), cancel, TargetBytes(side));
}

struct GifEncoder::Private {
	Private(QSize size, GifOptions options);

	void add(const QImage &frame, crl::time duration);
	[[nodiscard]] QByteArray finish();

	void normalize(const QImage &frame, std::vector<uint32> &to) const;
	void flush(const std::vector<uint32> *next);
	void writeHeader();
	[[nodiscard]] int takeDelay(crl::time duration);
	void writeFrame(QRect rect, int delay, bool clear);

	const QSize size;
	const GifOptions options;
	const bool valid = false;

	QByteArray out;
	int frames = 0;
	int lastDelay = 0;
	qsizetype lastDelayOffset = -1;
	bool lastCleared = false;
	crl::time timeTotal = 0;
	int64 delaysTotal = 0;

	// 0 for transparent pixels, 0xFFRRGGBB for the other ones.
	std::vector<uint32> pending;
	std::vector<uint32> scratch;
	crl::time pendingDuration = 0;
	bool hasPending = false;

	// The colors pixels had when they were written the last time: frames
	// are compared with them, not with each other, so slow changes add up
	// and get written.
	std::vector<uint32> shown;
	std::vector<uint8_t> changed;
	std::vector<uint8_t> indices;
	std::vector<int> errors;

	GifQuantizer quantizer;
	GifPalette palette;
	GifMapper mapper;
	GifLzw lzw;

};

GifEncoder::Private::Private(QSize size, GifOptions options)
: size(size)
, options(GifOptions{
	.loops = std::max(options.loops, 0),
	.colors = std::clamp(options.colors, 2, 256),
	.dither = options.dither,
})
, valid(size.width() > 0
	&& size.height() > 0
	&& size.width() <= 0xFFFF
	&& size.height() <= 0xFFFF) {
	if (valid) {
		const auto count = size_t(size.width()) * size.height();
		shown.assign(count, 0);
		changed.assign(count, 0);
	}
}

void GifEncoder::Private::normalize(
		const QImage &frame,
		std::vector<uint32> &to) const {
	auto image = (frame.size() == size)
		? frame
		: frame.scaled(
			size,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	if (image.format() != QImage::Format_ARGB32
		&& image.format() != QImage::Format_RGB32) {
		image = image.convertToFormat(image.hasAlphaChannel()
			? QImage::Format_ARGB32
			: QImage::Format_RGB32);
	}
	const auto opaque = (image.format() == QImage::Format_RGB32);
	const auto width = size.width();
	const auto height = size.height();
	to.resize(size_t(width) * height);
	for (auto y = 0; y != height; ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			image.constScanLine(y));
		const auto result = to.data() + size_t(y) * width;
		for (auto x = 0; x != width; ++x) {
			const auto pixel = line[x];
			result[x] = (opaque || qAlpha(pixel) >= 128)
				? (0xFF000000U | pixel)
				: 0U;
		}
	}
}

void GifEncoder::Private::add(const QImage &frame, crl::time duration) {
	if (!valid || frame.isNull()) {
		return;
	}
	duration = std::max(duration, crl::time(0));
	if (hasPending && pendingDuration < kGifMinDuration) {
		// Too fast to be shown, the previous frame stays instead.
		pendingDuration += duration;
		return;
	}
	normalize(frame, scratch);
	if (hasPending) {
		flush(&scratch);
	}
	std::swap(pending, scratch);
	pendingDuration = duration;
	hasPending = true;
}

void GifEncoder::Private::writeHeader() {
	out.append("GIF89a", 6);
	AppendLE16(out, size.width());
	AppendLE16(out, size.height());

	// A global color table of two colors: every frame has a table of its
	// own, but some decoders don't expect a file without the global one.
	out.append(char(0xF0));
	out.append(char(0)); // Background color index.
	out.append(char(0)); // Pixel aspect ratio.
	out.append(QByteArray(3, char(0x00)));
	out.append(QByteArray(3, char(0xFF)));

	// The loop extension has the count of repeats after the first time.
	if (options.loops != 1) {
		out.append("\x21\xFF\x0BNETSCAPE2.0\x03\x01", 16);
		AppendLE16(out, options.loops ? (options.loops - 1) : 0);
		out.append(char(0));
	}
}

int GifEncoder::Private::takeDelay(crl::time duration) {
	// 10 ms units, the rounding error is carried to the next frames.
	timeTotal += duration;
	const auto wanted = (timeTotal + 5) / 10 - delaysTotal;
	const auto result = int(std::clamp(
		wanted,
		int64(kGifMinDuration / 10),
		int64(kGifMaxDelay)));
	delaysTotal += result;
	return result;
}

void GifEncoder::Private::flush(const std::vector<uint32> *next) {
	const auto width = size.width();
	const auto height = size.height();
	const auto count = size_t(width) * height;
	const auto same = [](uint32 a, uint32 b) {
		const auto close = [](uint32 a, uint32 b) {
			return std::abs(int(a & 0xFF) - int(b & 0xFF)) <= kGifSameColor;
		};
		return close(a >> 16, b >> 16)
			&& close(a >> 8, b >> 8)
			&& close(a, b);
	};

	// What has to be drawn over the frame that is shown now.
	auto left = width;
	auto top = height;
	auto right = -1;
	auto bottom = -1;
	for (auto y = 0; y != height; ++y) {
		const auto line = pending.data() + size_t(y) * width;
		const auto old = shown.data() + size_t(y) * width;
		const auto to = changed.data() + size_t(y) * width;
		for (auto x = 0; x != width; ++x) {
			const auto draw = line[x] && (!old[x] || !same(line[x], old[x]));
			to[x] = draw ? 1 : 0;
			if (draw) {
				left = std::min(left, x);
				right = std::max(right, x);
				top = std::min(top, y);
				bottom = std::max(bottom, y);
			}
		}
	}
	const auto any = (right >= left);

	// Pixels can't be erased by drawing over them: if the next frame is
	// transparent where this one is not, this one takes the whole canvas
	// and the canvas is cleared after it.
	auto clear = false;
	if (next) {
		for (auto i = size_t(0); i != count; ++i) {
			if (pending[i] && !(*next)[i]) {
				clear = true;
				break;
			}
		}
	}

	const auto delay = takeDelay(pendingDuration);
	if (!any && !clear && frames > 0 && !lastCleared) {
		// Nothing changed, the previous frame is shown longer.
		lastDelay = std::min(lastDelay + delay, kGifMaxDelay);
		// Through data(): QByteArray::operator[] of Qt 5 (the default Qt of
		// the Windows build) is ambiguous for a 64-bit index.
		const auto bytes = out.data() + lastDelayOffset;
		bytes[0] = char(lastDelay & 0xFF);
		bytes[1] = char((lastDelay >> 8) & 0xFF);
		return;
	}
	if (!frames) {
		writeHeader();
	}
	const auto rect = clear
		? QRect(0, 0, width, height)
		: any
		? QRect(left, top, right - left + 1, bottom - top + 1)
		: QRect(0, 0, 1, 1);
	writeFrame(rect, delay, clear);

	// After a cleared frame an empty one can't be merged with it: it is
	// written as a transparent pixel.
	lastCleared = clear;
	if (clear) {
		std::fill(begin(shown), end(shown), 0U);
	} else {
		for (auto i = size_t(0); i != count; ++i) {
			if (changed[i]) {
				shown[i] = pending[i];
			}
		}
	}
}

void GifEncoder::Private::writeFrame(QRect rect, int delay, bool clear) {
	const auto width = size.width();
	const auto rectWidth = rect.width();
	const auto rectHeight = rect.height();
	const auto area = size_t(rectWidth) * rectHeight;

	quantizer.reset();
	auto drawn = size_t(0);
	for (auto y = 0; y != rectHeight; ++y) {
		const auto offset = size_t(rect.y() + y) * width + rect.x();
		for (auto x = 0; x != rectWidth; ++x) {
			if (changed[offset + x]) {
				quantizer.add(pending[offset + x]);
				++drawn;
			}
		}
	}

	// A cleared frame always has a transparent color: without one some
	// decoders (Qt) clear to the background color, not to transparency.
	const auto transparent = clear || (drawn != area);
	quantizer.build(options.colors - (transparent ? 1 : 0), palette);
	mapper.reset(palette);
	const auto transparentIndex = palette.count;
	const auto total = palette.count + (transparent ? 1 : 0);
	auto bits = 1;
	while ((1 << bits) < total) {
		++bits;
	}

	indices.resize(area);
	if (options.dither && palette.count > 1) {
		// Floyd-Steinberg, the direction changes with every line.
		// The errors are kept multiplied by 16.
		const auto row = size_t(rectWidth + 2) * 3;
		errors.assign(row * 2, 0);
		for (auto y = 0; y != rectHeight; ++y) {
			const auto offset = size_t(rect.y() + y) * width + rect.x();
			const auto current = errors.data() + (y & 1) * row + 3;
			const auto below = errors.data() + ((y + 1) & 1) * row + 3;
			std::fill(below - 3, below - 3 + row, 0);
			const auto back = (y & 1) != 0;
			const auto step = back ? -1 : 1;
			for (auto i = 0; i != rectWidth; ++i) {
				const auto x = back ? (rectWidth - 1 - i) : i;
				if (!changed[offset + x]) {
					indices[size_t(y) * rectWidth + x] = uint8_t(
						transparentIndex);
					continue;
				}
				const auto color = pending[offset + x];
				const auto error = current + x * 3;
				const auto r = std::clamp(
					int((color >> 16) & 0xFF) + error[0] / 16,
					0,
					255);
				const auto g = std::clamp(
					int((color >> 8) & 0xFF) + error[1] / 16,
					0,
					255);
				const auto b = std::clamp(
					int(color & 0xFF) + error[2] / 16,
					0,
					255);
				const auto index = mapper.map(r, g, b);
				indices[size_t(y) * rectWidth + x] = uint8_t(index);
				const auto chosen = palette.colors.data() + index * 3;
				const auto diff = std::array<int, 3>{ {
					r - int(chosen[0]),
					g - int(chosen[1]),
					b - int(chosen[2]),
				} };
				for (auto c = 0; c != 3; ++c) {
					current[(x + step) * 3 + c] += diff[c] * 7;
					below[(x - step) * 3 + c] += diff[c] * 3;
					below[x * 3 + c] += diff[c] * 5;
					below[(x + step) * 3 + c] += diff[c];
				}
			}
		}
	} else {
		for (auto y = 0; y != rectHeight; ++y) {
			const auto offset = size_t(rect.y() + y) * width + rect.x();
			for (auto x = 0; x != rectWidth; ++x) {
				const auto color = pending[offset + x];
				indices[size_t(y) * rectWidth + x] = !changed[offset + x]
					? uint8_t(transparentIndex)
					: uint8_t(mapper.map(
						(color >> 16) & 0xFF,
						(color >> 8) & 0xFF,
						color & 0xFF));
			}
		}
	}

	// Graphic control extension.
	out.append("\x21\xF9\x04", 3);
	out.append(char(((clear ? 2 : 1) << 2) | (transparent ? 1 : 0)));
	lastDelay = delay;
	lastDelayOffset = out.size();
	AppendLE16(out, delay);
	out.append(char(transparent ? transparentIndex : 0));
	out.append(char(0));

	// Image descriptor with a local color table.
	out.append(char(0x2C));
	AppendLE16(out, rect.x());
	AppendLE16(out, rect.y());
	AppendLE16(out, rectWidth);
	AppendLE16(out, rectHeight);
	out.append(char(0x80 | (bits - 1)));
	const auto tableSize = (1 << bits) * 3;
	const auto tableFrom = out.size();
	out.append(QByteArray(tableSize, char(0)));
	memcpy(
		out.data() + tableFrom,
		palette.colors.data(),
		palette.count * 3);

	lzw.encode(out, indices.data(), int(area), std::max(bits, 2));
	++frames;
}

QByteArray GifEncoder::Private::finish() {
	if (!valid) {
		return QByteArray();
	}
	if (hasPending) {
		flush(nullptr);
		hasPending = false;
	}
	if (!frames) {
		return QByteArray();
	}
	out.append(char(0x3B));
	frames = 0;
	return base::take(out);
}

GifEncoder::GifEncoder(QSize size, GifOptions options)
: _private(std::make_unique<Private>(size, options)) {
}

GifEncoder::~GifEncoder() = default;

void GifEncoder::add(const QImage &frame, crl::time duration) {
	_private->add(frame, duration);
}

QByteArray GifEncoder::finish() {
	return _private->finish();
}

QByteArray EncodeGif(
		const std::vector<QImage> &frames,
		crl::time frameDuration,
		GifOptions options) {
	const auto first = ranges::find_if(frames, [](const QImage &frame) {
		return !frame.isNull();
	});
	if (first == end(frames)) {
		return QByteArray();
	}
	auto encoder = GifEncoder(first->size(), options);
	for (const auto &frame : frames) {
		encoder.add(frame, frameDuration);
	}
	return encoder.finish();
}

struct Mp4Encoder::Private {
	explicit Private(Mp4Descriptor descriptor) : writer(descriptor) {
	}

	Mp4Writer writer;
	std::vector<uint32> rgb;
	int64 time = 0;
	int frames = 0;
};

Mp4Encoder::Mp4Encoder(QSize size, Mp4Options options)
: _private(std::make_unique<Private>(Mp4Descriptor{
	.size = size,
	.bitrate = options.bitrate,
	.fps = options.fps,
	.audioRate = options.audioRate,
	.audioChannels = options.audioChannels,
})) {
}

Mp4Encoder::~Mp4Encoder() = default;

bool Mp4Encoder::valid() const {
	return _private->writer.valid();
}

QSize Mp4Encoder::size() const {
	return _private->writer.size();
}

crl::time Mp4Encoder::duration() const {
	return (_private->time + 500) / 1000;
}

bool Mp4Encoder::add(const QImage &frame, crl::time duration) {
	const auto length = std::max(duration, crl::time(1)) * int64(1000);
	const auto written = WriteMp4Image(
		_private->writer,
		frame,
		_private->rgb,
		_private->time,
		length);
	if (!written) {
		return false;
	}
	_private->time += length;
	++_private->frames;
	return true;
}

bool Mp4Encoder::addAudio(const float *samples, int64 frames) {
	return _private->writer.writeAudio(samples, frames);
}

QByteArray Mp4Encoder::finish() {
	return _private->frames ? _private->writer.finish() : QByteArray();
}

ClipResult MakeVideo(
		const ClipOptions &options,
		VideoOptions video,
		Fn<void(float64)> progress,
		Cancel cancel) {
	return MakeMp4(
		options,
		video,
		kVideoMaxSide,
		kVideoMaxFps,
		progress,
		cancel);
}

ClipResult MakeGifVideo(
		const ClipOptions &options,
		Fn<void(float64)> progress,
		Cancel cancel) {
	return MakeMp4(
		options,
		{ .mute = true },
		kGifVideoMaxSide,
		kGifVideoMaxFps,
		progress,
		cancel);
}

ClipResult MakeGif(
		const ClipOptions &options,
		GifOptions gif,
		Fn<void(float64)> progress,
		Cancel cancel) {
	auto result = ClipResult();
	const auto parts = PlanSequence(PartsOf(
		options.path,
		options.content,
		options.from,
		options.till,
		options.crop,
		options.parts),
		false,
		(options.maxFps > 0) ? options.maxFps : kGifMaxFps,
		result.error);
	if (parts.empty()) {
		return result;
	}
	const auto size = ClipSize(
		parts.front().part.crop.size(),
		(options.maxSide > 0) ? options.maxSide : kGifMaxSide,
		false);
	const auto length = SequenceLength(parts);
	const auto black = QRgb(0xFF000000U);
	auto encoder = GifEncoder(size, gif);
	const auto read = ReadSequence(
		parts,
		size,
		black,
		[&](Frame &&frame, int) {
			const auto end = frame.position + frame.duration;
			encoder.add(frame.image, frame.duration);
			++result.frames;
			result.duration = end;
			if (progress) {
				progress(std::clamp(end / float64(length), 0., 1.));
			}
			return true;
		},
		cancel,
		&result.error);
	if (!read) {
		return result;
	}
	result.size = size;
	result.content = encoder.finish();
	result.ok = !result.content.isEmpty();
	if (!result.ok) {
		result.error = u"Could not write the GIF file."_q;
	}
	return result;
}

// Self-test.

namespace {

constexpr auto kTestSquare = QRgb(0xFFDC2828U);
constexpr auto kTestCircle = QRgb(0xFF283CDCU);
constexpr auto kTestBlack = QRgb(0xFF000000U);
constexpr auto kTestTolerance = 32;
constexpr auto kTestTone = 2. * 3.14159265358979323846 * 440.;

// TestQuadrants(): top left, top right, bottom left, bottom right.
constexpr auto kTestQuadrants = std::array<QRgb, 4>{ {
	QRgb(0xFFE61E1EU),
	QRgb(0xFF1EC81EU),
	QRgb(0xFF1E1EE6U),
	QRgb(0xFFE6E61EU),
} };

[[nodiscard]] QRect TestSquareRect(QSize size, float64 progress) {
	const auto side = std::min(size.width(), size.height()) / 4;
	return QRect(
		int(progress * (size.width() - side)),
		size.height() / 8,
		side,
		side);
}

[[nodiscard]] int TestCircleRadius(QSize size) {
	return std::min(size.width(), size.height()) / 6;
}

[[nodiscard]] QPoint TestCircleCenter(QSize size, float64 progress) {
	const auto radius = TestCircleRadius(size);
	return QPoint(
		size.width() / 2,
		radius + int(progress * (size.height() - 2 * radius)));
}

// A moving gradient (or nothing, if transparent) with shapes of flat
// colors: a square that goes from the left to the right and a circle
// that goes down.
[[nodiscard]] QImage TestFrame(QSize size, float64 progress, bool transparent) {
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	const auto width = size.width();
	const auto height = size.height();
	const auto square = TestSquareRect(size, progress);
	const auto center = TestCircleCenter(size, progress);
	const auto radius = TestCircleRadius(size);
	const auto shift = int(progress * 255);
	for (auto y = 0; y != height; ++y) {
		const auto line = reinterpret_cast<QRgb*>(result.scanLine(y));
		for (auto x = 0; x != width; ++x) {
			const auto dx = x - center.x();
			const auto dy = y - center.y();
			if (square.contains(x, y)) {
				line[x] = kTestSquare;
			} else if (dx * dx + dy * dy <= radius * radius) {
				line[x] = kTestCircle;
			} else if (transparent) {
				line[x] = 0;
			} else {
				const auto wave = ((x * 255) / width + shift) % 510;
				line[x] = qRgb(
					(wave > 255) ? (510 - wave) : wave,
					(y * 255) / height,
					128);
			}
		}
	}
	return result;
}

[[nodiscard]] QImage TestQuadrants(QSize size) {
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	for (auto y = 0; y != size.height(); ++y) {
		const auto line = reinterpret_cast<QRgb*>(result.scanLine(y));
		for (auto x = 0; x != size.width(); ++x) {
			line[x] = kTestQuadrants[((y * 2 >= size.height()) ? 2 : 0)
				+ ((x * 2 >= size.width()) ? 1 : 0)];
		}
	}
	return result;
}

// Random blocks. With the count of colors they are taken from a table
// of colors that a GIF palette keeps exactly.
[[nodiscard]] QImage TestNoise(
		QSize size,
		int block,
		int colors,
		uint32 &seed) {
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	const auto columns = (size.width() + block - 1) / block;
	auto row = std::vector<QRgb>(columns);
	for (auto y = 0; y != size.height(); ++y) {
		if (!(y % block)) {
			for (auto &color : row) {
				seed = seed * 1664525U + 1013904223U;
				const auto value = (seed >> 8);
				const auto index = colors ? int(value % uint32(colors)) : 0;
				color = colors
					? qRgb(
						(index & 7) * 32 + 8,
						((index >> 3) & 7) * 32 + 8,
						((index >> 6) & 3) * 64 + 8)
					: (0xFF000000U | value);
			}
		}
		const auto line = reinterpret_cast<QRgb*>(result.scanLine(y));
		for (auto x = 0; x != size.width(); ++x) {
			line[x] = row[x / block];
		}
	}
	return result;
}

[[nodiscard]] QRgb TestPixel(const QImage &image, int x, int y) {
	return image.valid(x, y) ? image.pixel(x, y) : QRgb(0);
}

[[nodiscard]] bool TestNear(QRgb a, QRgb b, int tolerance = kTestTolerance) {
	return (std::abs(qRed(a) - qRed(b)) <= tolerance)
		&& (std::abs(qGreen(a) - qGreen(b)) <= tolerance)
		&& (std::abs(qBlue(a) - qBlue(b)) <= tolerance);
}

[[nodiscard]] QString TestColor(QRgb color) {
	return u"(%1,%2,%3,%4)"_q
		.arg(qRed(color))
		.arg(qGreen(color))
		.arg(qBlue(color))
		.arg(qAlpha(color));
}

[[nodiscard]] QString TestInfo(const ClipInfo &info) {
	return u"%1 in %2, %3x%4, rotation %5, %6 ms, %7 fps, audio %8, alpha %9"_q
		.arg(info.codec)
		.arg(info.container)
		.arg(info.size.width())
		.arg(info.size.height())
		.arg(info.rotation)
		.arg(info.duration)
		.arg(info.fps, 0, 'f', 3)
		.arg(info.hasAudio ? 1 : 0)
		.arg(info.hasAlpha ? 1 : 0);
}

// A source for the reader: an mp4 with a display matrix and, maybe,
// a 440 Hz tone.
[[nodiscard]] QByteArray TestVideo(
		QSize size,
		int rotation,
		int count,
		bool audio,
		const Fn<crl::time(int)> &duration,
		const Fn<QImage(int)> &image) {
	auto writer = Mp4Writer({
		.size = size,
		.audioRate = audio ? kAudioRate : 0,
		.audioChannels = audio ? kAudioChannels : 0,
		.rotation = rotation,
	});
	auto rgb = std::vector<uint32>();
	auto samples = std::vector<float>();
	auto time = int64(0);
	auto written = int64(0);
	for (auto i = 0; i != count; ++i) {
		const auto length = duration(i) * int64(1000);
		if (!WriteMp4Image(writer, image(i), rgb, time, length)) {
			return QByteArray();
		}
		time += length;
		if (audio) {
			const auto till = av_rescale(time, kAudioRate, kSecond);
			samples.clear();
			for (auto j = written; j != till; ++j) {
				const auto value = 0.5f * float(std::sin(
					j * kTestTone / kAudioRate));
				samples.insert(end(samples), kAudioChannels, value);
			}
			if (!writer.writeAudio(samples.data(), till - written)) {
				return QByteArray();
			}
			written = till;
		}
	}
	return writer.finish();
}

struct TestGif {
	std::vector<QImage> frames; // ARGB32, composed.
	std::vector<int> delays;
	int total = 0;
};

[[nodiscard]] TestGif TestReadGif(QByteArray bytes) {
	constexpr auto kLimit = 1000;
	auto result = TestGif();
	auto buffer = QBuffer(&bytes);
	if (!buffer.open(QIODevice::ReadOnly)) {
		return result;
	}
	auto reader = QImageReader(&buffer, "gif"_q);
	while (int(result.frames.size()) < kLimit) {
		auto image = reader.read();
		if (image.isNull()) {
			break;
		}
		result.frames.push_back(image.convertToFormat(QImage::Format_ARGB32));
		result.delays.push_back(reader.nextImageDelay());
		result.total += result.delays.back();
	}
	return result;
}

// The pixels of a GIF with one frame of the whole size, read the way
// a strict decoder does it: empty if the LZW data is anything but the
// codes of the pixels, clear codes and the end code right after the last
// pixel, every one of the size the table has at that point.
[[nodiscard]] std::vector<QRgb> TestStrictGif(
		const QByteArray &bytes,
		QSize size) {
	const auto data = reinterpret_cast<const uchar*>(bytes.constData());
	const auto total = int(bytes.size());
	auto at = 13;
	if (total < at || !bytes.startsWith("GIF89a"_q)) {
		return {};
	} else if (data[10] & 0x80) {
		at += 3 * (2 << (data[10] & 7));
	}
	while (at + 2 < total && data[at] == 0x21) {
		at += 2;
		while (at < total && data[at]) {
			at += data[at] + 1;
		}
		++at;
	}
	if (at + 10 > total || data[at] != 0x2C) {
		return {};
	}
	const auto width = int(data[at + 5]) | (int(data[at + 6]) << 8);
	const auto height = int(data[at + 7]) | (int(data[at + 8]) << 8);
	const auto flags = data[at + 9];
	const auto palette = data + at + 10;
	const auto colors = 2 << (flags & 7);
	at += 10 + 3 * colors;
	if (QSize(width, height) != size || !(flags & 0x80) || at >= total) {
		return {};
	}
	const auto minCodeSize = int(data[at++]);
	auto stream = std::vector<uchar>();
	while (at < total && data[at]) {
		const auto length = int(data[at]);
		if (at + 1 + length > total) {
			return {};
		}
		stream.insert(end(stream), data + at + 1, data + at + 1 + length);
		at += 1 + length;
	}
	if (at + 2 != total
		|| data[at + 1] != 0x3B
		|| minCodeSize < 2
		|| minCodeSize > 8) {
		return {};
	}

	constexpr auto kLimit = 4096;
	const auto area = size_t(width) * height;
	const auto clear = 1 << minCodeSize;
	auto prefix = std::vector<int>(kLimit, 0);
	auto suffix = std::vector<uchar>(kLimit, 0);
	auto indices = std::vector<uchar>();
	auto string = std::vector<uchar>();
	const auto expand = [&](int code) {
		string.clear();
		while (code >= clear) {
			string.push_back(suffix[code]);
			code = prefix[code];
		}
		string.push_back(uchar(code));
		std::reverse(begin(string), end(string));
	};
	auto codeSize = minCodeSize + 1;
	auto next = clear + 2;
	auto previous = -1;
	auto bit = size_t(0);
	while (true) {
		if (bit + codeSize > stream.size() * 8) {
			return {};
		}
		auto code = 0;
		for (auto i = 0; i != codeSize; ++i, ++bit) {
			code |= ((stream[bit / 8] >> (bit % 8)) & 1) << i;
		}
		if (code == clear) {
			codeSize = minCodeSize + 1;
			next = clear + 2;
			previous = -1;
			continue;
		} else if (code == clear + 1) {
			break;
		} else if (indices.size() >= area) {
			return {};
		} else if (previous < 0) {
			if (code > clear) {
				return {};
			}
			expand(code);
		} else if (code < next) {
			expand(code);
		} else if (code == next && next < kLimit) {
			expand(previous);
			string.push_back(string.front());
		} else {
			return {};
		}
		if (previous >= 0 && next < kLimit) {
			prefix[next] = previous;
			suffix[next] = string.front();
			++next;
			if (next == (1 << codeSize) && codeSize < 12) {
				++codeSize;
			}
		}
		indices.insert(end(indices), begin(string), end(string));
		previous = code;
	}
	if (indices.size() != area || (bit + 7) / 8 != stream.size()) {
		return {};
	}
	auto result = std::vector<QRgb>();
	result.reserve(area);
	for (const auto index : indices) {
		if (index >= colors) {
			return {};
		}
		const auto color = palette + index * 3;
		result.push_back(qRgb(color[0], color[1], color[2]));
	}
	return result;
}

// Root mean square of a part of interleaved samples.
[[nodiscard]] float64 TestLevel(
		const std::vector<float> &samples,
		crl::time from,
		crl::time till) {
	const auto begin = size_t(from * kAudioRate / 1000) * kAudioChannels;
	const auto end = std::min(
		size_t(till * kAudioRate / 1000) * kAudioChannels,
		samples.size());
	if (begin >= end) {
		return 0.;
	}
	auto sum = 0.;
	for (auto i = begin; i != end; ++i) {
		sum += float64(samples[i]) * samples[i];
	}
	return std::sqrt(sum / (end - begin));
}

} // namespace

bool RunSelfTest(QStringList &log) {
	const auto started = crl::now();
	auto failed = 0;
	const auto check = [&](
			bool ok,
			const QString &name,
			const QString &details = QString()) {
		log.push_back((ok ? u"ok   "_q : u"FAIL "_q)
			+ name
			+ (details.isEmpty() ? QString() : (u": "_q + details)));
		if (!ok) {
			++failed;
		}
		return ok;
	};
	const auto readAll = [](const QByteArray &content, FrameOptions options) {
		options.path = QString();
		options.content = content;
		auto result = std::vector<Frame>();
		const auto read = ReadFrames(options, [&](Frame &&frame) {
			result.push_back(std::move(frame));
			return true;
		});
		if (!read) {
			result.clear();
		}
		return result;
	};
	const auto colors = [&](
			const QImage &image,
			std::vector<std::pair<QPoint, QRgb>> expected,
			const QString &name,
			int tolerance = kTestTolerance) {
		auto wrong = QStringList();
		for (const auto &[point, color] : expected) {
			const auto real = TestPixel(image, point.x(), point.y());
			const auto ok = !qAlpha(color)
				? (qAlpha(real) <= 24)
				: (qAlpha(real) >= 231 && TestNear(real, color, tolerance));
			if (!ok) {
				wrong.push_back(u"(%1,%2) is %3, not %4"_q
					.arg(point.x())
					.arg(point.y())
					.arg(TestColor(real))
					.arg(TestColor(color)));
			}
		}
		return check(
			!image.isNull() && wrong.isEmpty(),
			name,
			image.isNull() ? u"no image"_q : wrong.join(u"; "_q));
	};
	const auto stickerSummary = [](const VideoStickerResult &sticker) {
		return sticker.ok
			? u"%1x%2, %3 ms, %4 frames, alpha %5, %6 bytes, %7 encodings"_q
				.arg(sticker.size.width())
				.arg(sticker.size.height())
				.arg(sticker.duration)
				.arg(sticker.frames)
				.arg(sticker.alpha ? 1 : 0)
				.arg(sticker.webm.size())
				.arg(sticker.attempts)
			: (u"error: "_q + sticker.error);
	};
	const auto clipSummary = [](const ClipResult &clip) {
		return clip.ok
			? u"%1x%2, %3 ms, %4 frames, audio %5, %6 bytes"_q
				.arg(clip.size.width())
				.arg(clip.size.height())
				.arg(clip.duration)
				.arg(clip.frames)
				.arg(clip.audio ? 1 : 0)
				.arg(clip.content.size())
			: (u"error: "_q + clip.error);
	};

	// A video sticker: two seconds of 30 fps.
	{
		const auto size = QSize(kStickerSide, kStickerSide);
		constexpr auto kCount = 60;
		auto frames = std::vector<QImage>();
		for (auto i = 0; i != kCount; ++i) {
			frames.push_back(TestFrame(size, i / float64(kCount), false));
		}
		const auto now = crl::now();
		const auto sticker = EncodeVideoSticker(frames, 33);
		check(
			sticker.ok
				&& (sticker.size == size)
				&& (sticker.frames == kCount)
				&& !sticker.alpha,
			u"sticker, encode"_q,
			stickerSummary(sticker) + u", %1 ms"_q.arg(crl::now() - now));
		check(
			!sticker.webm.isEmpty()
				&& (sticker.webm.size() <= kStickerMaxBytes)
				&& (sticker.duration == 2000),
			u"sticker, size and duration limits"_q);
		check(
			sticker.webm.startsWith("\x1A\x45\xDF\xA3"_q)
				&& sticker.webm.left(64).contains("webm"_q),
			u"sticker, EBML header"_q);
		const auto info = ReadClipInfo(QString(), sticker.webm);
		check(
			info.valid()
				&& (info.codec == u"vp9"_q)
				&& info.container.contains(u"webm"_q)
				&& (info.size == size)
				&& (info.duration == 2000)
				&& (info.fps > 29.9 && info.fps < 30.01)
				&& !info.hasAudio
				&& !info.hasAlpha,
			u"sticker, read by the matroska demuxer"_q,
			TestInfo(info));
		const auto decoded = readAll(sticker.webm, {});
		check(
			int(decoded.size()) == kCount
				&& (decoded[kCount / 2].position == 1000)
				&& (decoded.back().position + decoded.back().duration
					== 2000),
			u"sticker, all the frames decode"_q,
			u"%1 frames"_q.arg(decoded.size()));
		if (!decoded.empty()) {
			const auto last = (kCount - 1) / float64(kCount);
			colors(decoded.front().image, {
				{ TestCircleCenter(size, 0.), kTestCircle },
				{ TestSquareRect(size, 0.).center(), kTestSquare },
				{ QPoint(500, 500), qRgb(249, 249, 128) },
			}, u"sticker, the first frame"_q);
			colors(decoded.back().image, {
				{ TestCircleCenter(size, last), kTestCircle },
				{ TestSquareRect(size, last).center(), kTestSquare },
			}, u"sticker, the last frame"_q);
		}
	}

	// Transparency: a second stream of VP9 in the block additions.
	{
		const auto size = QSize(400, 300);
		constexpr auto kCount = 20;
		auto frames = std::vector<QImage>();
		for (auto i = 0; i != kCount; ++i) {
			frames.push_back(TestFrame(size, i / float64(kCount), true));
		}
		const auto sticker = EncodeVideoSticker(frames, 50);
		const auto result = QSize(512, 384);
		check(
			sticker.ok
				&& sticker.alpha
				&& (sticker.size == result)
				&& (sticker.duration == 1000)
				&& (sticker.webm.size() <= kStickerMaxBytes),
			u"alpha sticker, encode"_q,
			stickerSummary(sticker));
		const auto info = ReadClipInfo(QString(), sticker.webm);
		check(
			info.valid() && info.hasAlpha && (info.size == result),
			u"alpha sticker, read by the matroska demuxer"_q,
			TestInfo(info));
		const auto decoded = readAll(sticker.webm, {});
		check(
			int(decoded.size()) == kCount,
			u"alpha sticker, all the frames decode"_q,
			u"%1 frames"_q.arg(decoded.size()));
		if (!decoded.empty()) {
			const auto scale = [&](QPoint point) {
				return QPoint(
					(point.x() * result.width()) / size.width(),
					(point.y() * result.height()) / size.height());
			};
			colors(decoded.front().image, {
				{ scale(TestCircleCenter(size, 0.)), kTestCircle },
				{ scale(TestSquareRect(size, 0.).center()), kTestSquare },
				{ QPoint(4, 4), QRgb(0) },
				{ QPoint(256, 300), QRgb(0) },
				{ QPoint(507, 379), QRgb(0) },
			}, u"alpha sticker, the first frame"_q);
		}
	}

	// A custom emoji is a square, a wide source is centered in it.
	{
		const auto size = QSize(200, 100);
		auto frames = std::vector<QImage>();
		for (auto i = 0; i != 10; ++i) {
			frames.push_back(TestFrame(size, i / 10., true));
		}
		const auto emoji = EncodeVideoSticker(frames, 100, kEmojiSide);
		check(
			emoji.ok
				&& emoji.alpha
				&& (emoji.size == QSize(kEmojiSide, kEmojiSide))
				&& (emoji.duration == 1000),
			u"emoji, encode"_q,
			stickerSummary(emoji));
		const auto decoded = readAll(emoji.webm, {});
		if (check(decoded.size() == 10, u"emoji, all the frames decode"_q)) {
			const auto center = TestCircleCenter(size, 0.);
			colors(decoded.front().image, {
				{ QPoint(center.x() / 2, 25 + center.y() / 2), kTestCircle },
				{ QPoint(50, 10), QRgb(0) },
				{ QPoint(50, 90), QRgb(0) },
			}, u"emoji, the first frame"_q, 48);
		}
	}

	// Frames that come too fast and a single frame.
	{
		const auto size = QSize(128, 128);
		auto frames = std::vector<QImage>();
		for (auto i = 0; i != 100; ++i) {
			frames.push_back(TestFrame(size, i / 100., false));
		}
		const auto fast = EncodeVideoSticker(frames, 20, 128);
		const auto info = ReadClipInfo(QString(), fast.webm);
		check(
			fast.ok
				&& (fast.duration == 2000)
				&& (fast.frames > 50 && fast.frames <= 60)
				&& (info.fps > 25. && info.fps < 30.01),
			u"sticker, 50 fps become not more than 30"_q,
			stickerSummary(fast) + u", "_q + TestInfo(info));

		frames.resize(1);
		const auto single = EncodeVideoSticker(frames, 1000, 128);
		check(
			single.ok && (single.frames == 1) && (single.duration == 1000),
			u"sticker, a single frame"_q,
			stickerSummary(single));
	}

	// Noise doesn't fit at once: the bitrate goes down, then the frames
	// are dropped.
	{
		constexpr auto kLimit = int64(48 * 1024);
		auto seed = uint32(1);
		auto maker = StickerMaker(ComputeStickerGeometry(
			QSize(256, 256),
			256));
		for (auto i = 0; i != 30; ++i) {
			maker.add(
				TestNoise(QSize(256, 256), 4, 0, seed),
				i * 100,
				100);
		}
		const auto now = crl::now();
		const auto fitted = maker.encode(nullptr, nullptr, kLimit);
		check(
			fitted.ok
				&& (fitted.webm.size() <= kLimit)
				&& (fitted.attempts > 1)
				&& (fitted.duration == 3000),
			u"sticker, fitted into the size limit"_q,
			stickerSummary(fitted) + u", %1 ms"_q.arg(crl::now() - now));
		const auto decoded = readAll(fitted.webm, {});
		check(
			!decoded.empty() && (int(decoded.size()) == fitted.frames),
			u"sticker, fitted one decodes"_q,
			u"%1 frames"_q.arg(decoded.size()));
	}

	// A fast part of a video with a variable frame rate: the frames are
	// read without a limit, as the video is not that fast on average.
	// Every second one goes, not the second half of the sticker.
	{
		const auto size = QSize(128, 128);
		const auto geometry = ComputeStickerGeometry(size, 128);
		const auto make = [&](int count) {
			auto maker = StickerMaker(geometry);
			const auto position = [](int index) {
				return crl::time((index * 50 + 1) / 3);
			};
			for (auto i = 0; i != count; ++i) {
				maker.add(
					TestFrame(size, i / float64(count), false),
					position(i),
					position(i + 1) - position(i));
			}
			return maker.encode(nullptr, nullptr, kStickerMaxBytes);
		};
		const auto whole = make(180);
		const auto decoded = readAll(whole.webm, {});
		const auto last = decoded.empty() ? Frame() : decoded.back();
		check(
			whole.ok
				&& (whole.duration == 3000)
				&& (whole.frames > 85 && whole.frames <= 90)
				&& (int(decoded.size()) == whole.frames)
				&& (last.position > 2900)
				&& (last.position + last.duration == 3000),
			u"sticker, 60 fps frames are thinned evenly"_q,
			stickerSummary(whole) + u", the last frame at %1 ms"_q
				.arg(last.position));
		if (!decoded.empty()) {
			const auto shown = 178 / 180.;
			colors(last.image, {
				{ TestCircleCenter(size, shown), kTestCircle },
				{ TestSquareRect(size, shown).center(), kTestSquare },
			}, u"sticker, thinned to the last frame"_q);
		}

		const auto second = make(60);
		check(
			second.ok
				&& (second.duration == 1000)
				&& (second.frames > 27 && second.frames <= 30),
			u"sticker, a second of 60 fps frames stays a second"_q,
			stickerSummary(second));

		// Frames after the maximum duration are dropped, also the ones
		// that are moved there by the previous frame.
		auto maker = StickerMaker(geometry);
		maker.add(TestFrame(size, 0., false), 0, 2999);
		maker.add(TestFrame(size, 0.5, false), 2999, 1);
		maker.add(TestFrame(size, 1., false), 2999, 1);
		maker.add(TestFrame(size, 1., false), 3000, 1);
		const auto full = maker.encode(nullptr, nullptr, kStickerMaxBytes);
		check(
			full.ok && (full.frames == 2) && (full.duration == 3000),
			u"sticker, frames at the very end"_q,
			stickerSummary(full));
	}

	// H.264 mp4 without audio, frames of different durations.
	const auto moving = QSize(320, 240);
	auto silent = QByteArray();
	{
		constexpr auto kCount = 20;
		auto encoder = Mp4Encoder(moving);
		auto added = encoder.valid();
		for (auto i = 0; added && i != kCount; ++i) {
			added = encoder.add(
				TestFrame(moving, i / float64(kCount), false),
				(i % 2) ? 80 : 40);
		}
		const auto duration = encoder.duration();
		silent = encoder.finish();
		const auto info = ReadClipInfo(QString(), silent);
		check(
			added
				&& info.valid()
				&& (info.codec == u"h264"_q)
				&& info.container.contains(u"mp4"_q)
				&& (info.size == moving)
				&& (duration == 1200)
				&& (std::abs(info.duration - 1200) <= 2)
				&& !info.hasAudio,
			u"mp4, silent"_q,
			u"%1 bytes, "_q.arg(silent.size()) + TestInfo(info));
		const auto decoded = readAll(silent, {});
		auto timing = (int(decoded.size()) == kCount);
		for (auto i = 0; timing && i != kCount; ++i) {
			const auto position = (i / 2) * 120 + (i % 2) * 40;
			timing = (std::abs(decoded[i].position - position) <= 1)
				&& (std::abs(decoded[i].duration - ((i % 2) ? 80 : 40)) <= 1);
		}
		check(
			timing,
			u"mp4, variable frame rate is kept"_q,
			u"%1 frames"_q.arg(decoded.size()));
		if (!decoded.empty()) {
			colors(decoded.front().image, {
				{ TestCircleCenter(moving, 0.), kTestCircle },
				{ TestSquareRect(moving, 0.).center(), kTestSquare },
			}, u"mp4, the first frame"_q);
		}
	}

	// Reading: rotation, crop, trimming and the frame rate limit.
	// The source is stored 320x240 and shown 240x320, rotated clockwise.
	const auto rotated = TestVideo(QSize(320, 240), 90, 30, true, [](int i) {
		return crl::time(((i % 3) == 1) ? 34 : 33);
	}, [](int) {
		return TestQuadrants(QSize(320, 240));
	});
	const auto shown = std::array<QRgb, 4>{ {
		kTestQuadrants[2],
		kTestQuadrants[0],
		kTestQuadrants[3],
		kTestQuadrants[1],
	} };
	{
		const auto info = ReadClipInfo(QString(), rotated);
		check(
			info.valid()
				&& (info.size == QSize(240, 320))
				&& (info.rotation == 90)
				&& (std::abs(info.duration - 1000) <= 2)
				&& info.hasAudio,
			u"reader, rotated source"_q,
			TestInfo(info));

		const auto whole = readAll(rotated, {});
		check(whole.size() == 30, u"reader, all the frames"_q);
		if (!whole.empty()) {
			colors(whole.front().image, {
				{ QPoint(60, 80), shown[0] },
				{ QPoint(180, 80), shown[1] },
				{ QPoint(60, 240), shown[2] },
				{ QPoint(180, 240), shown[3] },
			}, u"reader, rotation"_q);
		}

		const auto corner = ReadFrame({
			.content = rotated,
			.from = 500,
			.crop = QRect(120, 0, 120, 160),
			.size = QSize(60, 80),
		});
		check(corner.size() == QSize(60, 80), u"reader, size of a crop"_q);
		colors(corner, {
			{ QPoint(3, 3), shown[1] },
			{ QPoint(56, 76), shown[1] },
		}, u"reader, crop of a quadrant"_q);

		const auto middle = ReadFrame({
			.content = rotated,
			.crop = QRect(60, 80, 120, 160),
		});
		check(middle.size() == QSize(120, 160), u"reader, size of a crop"_q);
		colors(middle, {
			{ QPoint(30, 40), shown[0] },
			{ QPoint(90, 40), shown[1] },
			{ QPoint(30, 120), shown[2] },
			{ QPoint(90, 120), shown[3] },
		}, u"reader, crop of the middle"_q);

		const auto trimmed = readAll(rotated, { .from = 300, .till = 700 });
		check(
			(trimmed.size() == 12)
				&& (trimmed.front().position == 0)
				&& (trimmed.back().position + trimmed.back().duration
					== 400),
			u"reader, trimming"_q,
			u"%1 frames"_q.arg(trimmed.size()));

		const auto slow = readAll(rotated, { .maxFps = 10 });
		check(
			(slow.size() == 10)
				&& (std::abs(slow[5].position - 500) <= 1)
				&& (std::abs(slow.back().position
					+ slow.back().duration
					- 1000) <= 2),
			u"reader, frame rate limit"_q,
			u"%1 frames"_q.arg(slow.size()));
	}

	// A video sticker from a file: trimming, the default crop, progress.
	{
		auto values = std::vector<float64>();
		const auto sticker = MakeVideoSticker({
			.from = 200,
			.till = 800,
			.content = rotated,
		}, [&](float64 value) {
			values.push_back(value);
		}, nullptr);
		const auto info = ReadClipInfo(QString(), sticker.webm);
		check(
			sticker.ok
				&& (sticker.size == QSize(512, 512))
				&& (sticker.duration == 600)
				&& (sticker.frames == 18)
				&& !sticker.alpha
				&& (sticker.webm.size() <= kStickerMaxBytes)
				&& info.valid()
				&& !info.hasAudio,
			u"sticker from a video"_q,
			stickerSummary(sticker));
		check(
			!values.empty()
				&& ranges::is_sorted(values)
				&& (values.front() >= 0.)
				&& (values.back() == 1.),
			u"sticker from a video, progress"_q,
			u"%1 values"_q.arg(values.size()));
		const auto decoded = readAll(sticker.webm, {});
		if (check(!decoded.empty(), u"sticker from a video, decodes"_q)) {
			// The square in the middle of 240x320.
			colors(decoded.front().image, {
				{ QPoint(128, 128), shown[0] },
				{ QPoint(384, 128), shown[1] },
				{ QPoint(128, 384), shown[2] },
				{ QPoint(384, 384), shown[3] },
			}, u"sticker from a video, the crop"_q);
		}

		const auto cancel = std::make_shared<std::atomic<bool>>(true);
		const auto cancelled = MakeVideoSticker({
			.content = rotated,
		}, nullptr, cancel);
		check(
			!cancelled.ok && cancelled.webm.isEmpty(),
			u"sticker from a video, cancelled"_q,
			cancelled.error);

		// Two fragments: a quadrant, then all of the moving video, which
		// is wider and gets transparent bars.
		const auto joined = MakeVideoSticker({ .parts = {
			{ .content = rotated, .till = 300, .crop = QRect(0, 0, 120, 160) },
			{ .content = silent, .from = 600 },
		} }, nullptr, nullptr);
		check(
			joined.ok
				&& joined.alpha
				&& (joined.size == QSize(384, 512))
				&& (joined.duration == 900),
			u"sticker from two fragments"_q,
			stickerSummary(joined));
		const auto both = readAll(joined.webm, {});
		if (check(!both.empty(), u"sticker from two fragments, decodes"_q)) {
			colors(both.front().image, {
				{ QPoint(10, 10), shown[0] },
				{ QPoint(374, 502), shown[0] },
			}, u"sticker from two fragments, the first one"_q);
			colors(both.back().image, {
				{ QPoint(192, 20), QRgb(0) },
				{ QPoint(192, 492), QRgb(0) },
				{ QPoint(192, 256), qRgb(141, 127, 128) },
			}, u"sticker from two fragments, the second one"_q, 48);
		}
	}

	// Video: fragments joined, audio kept and silence added.
	{
		const auto video = MakeVideo({ .parts = {
			{ .content = rotated, .from = 200, .till = 700 },
			{ .content = silent, .till = 400 },
		} }, {}, nullptr, nullptr);
		const auto info = ReadClipInfo(QString(), video.content);
		check(
			video.ok
				&& video.audio
				&& (video.size == QSize(240, 320))
				&& (video.duration == 900)
				&& info.valid()
				&& (info.codec == u"h264"_q)
				&& info.hasAudio
				&& (info.size == QSize(240, 320))
				&& (std::abs(info.duration - 900) <= 2),
			u"video from two fragments"_q,
			clipSummary(video) + u", "_q + TestInfo(info));
		const auto decoded = readAll(video.content, {});
		if (check(!decoded.empty(), u"video from two fragments, decodes"_q)) {
			colors(decoded.front().image, {
				{ QPoint(60, 80), shown[0] },
				{ QPoint(180, 240), shown[3] },
			}, u"video from two fragments, the first one"_q);
			// 320x240 in 240x320 is 240x180 with black bars.
			colors(decoded.back().image, {
				{ QPoint(120, 30), kTestBlack },
				{ QPoint(120, 290), kTestBlack },
			}, u"video from two fragments, the second one"_q);
		}
		auto reader = AudioReader({
			.part = { .content = video.content },
			.info = info,
		});
		auto samples = std::vector<float>();
		reader.read(900 * int64(1000), samples);
		const auto tone = TestLevel(samples, 50, 450);
		const auto silence = TestLevel(samples, 600, 850);
		check(
			(samples.size() == size_t(900 * 48 * kAudioChannels))
				&& (tone > 0.2 && tone < 0.5)
				&& (silence < 0.01),
			u"video from two fragments, audio"_q,
			u"%1 samples, levels %2 and %3"_q
				.arg(samples.size())
				.arg(tone, 0, 'f', 4)
				.arg(silence, 0, 'f', 4));

		const auto muted = MakeGifVideo({
			.content = rotated,
			.maxSide = 160,
		}, nullptr, nullptr);
		const auto mutedInfo = ReadClipInfo(QString(), muted.content);
		check(
			muted.ok
				&& !muted.audio
				&& (muted.size == QSize(120, 160))
				&& (muted.frames == 30)
				&& mutedInfo.valid()
				&& !mutedInfo.hasAudio
				&& (mutedInfo.size == QSize(120, 160))
				&& (std::abs(mutedInfo.duration - 1000) <= 2),
			u"GIF video, no audio track"_q,
			clipSummary(muted) + u", "_q + TestInfo(mutedInfo));
	}

	// A hole in the timestamps of the audio track: a second of a tone,
	// nothing for a second, a tone again. Two frames of video, so every
	// one of them gets its audio by several pieces.
	{
		const auto size = QSize(64, 48);
		constexpr auto kStep = kAudioRate / 10;
		auto writer = Mp4Writer({
			.size = size,
			.audioRate = kAudioRate,
			.audioChannels = kAudioChannels,
		});
		auto rgb = std::vector<uint32>();
		auto tone = std::vector<float>();
		auto written = writer.valid();
		for (auto i = 0; written && i != 30; ++i) {
			if (!(i % 15)) {
				written = WriteMp4Image(
					writer,
					TestQuadrants(size),
					rgb,
					i * (kSecond / 10),
					15 * (kSecond / 10));
			}
			if (i == 10) {
				writer.skipAudio(kAudioRate);
			} else if (written && (i < 10 || i >= 20)) {
				tone.clear();
				for (auto j = i * kStep; j != (i + 1) * kStep; ++j) {
					const auto value = 0.5f * float(std::sin(
						j * kTestTone / kAudioRate));
					tone.insert(end(tone), kAudioChannels, value);
				}
				written = writer.writeAudio(tone.data(), kStep);
			}
		}
		const auto holed = written ? writer.finish() : QByteArray();
		const auto info = ReadClipInfo(QString(), holed);
		const auto levels = [](const std::vector<float> &list) {
			return std::array<float64, 3>{ {
				TestLevel(list, 100, 900),
				TestLevel(list, 1100, 1900),
				TestLevel(list, 2100, 2900),
			} };
		};
		const auto good = [](const std::array<float64, 3> &values) {
			return (values[0] > 0.2 && values[0] < 0.5)
				&& (values[1] < 0.01)
				&& (values[2] > 0.2 && values[2] < 0.5);
		};
		const auto summary = [](
				const std::vector<float> &list,
				const std::array<float64, 3> &values) {
			return u"%1 samples, levels %2, %3 and %4"_q
				.arg(list.size())
				.arg(values[0], 0, 'f', 4)
				.arg(values[1], 0, 'f', 4)
				.arg(values[2], 0, 'f', 4);
		};
		const auto expected = [](const ClipInfo &clip) {
			return size_t(clip.duration * 48 * kAudioChannels);
		};

		const auto samples = ReadAudio({ .content = holed });
		const auto read = levels(samples);
		check(
			info.valid()
				&& info.hasAudio
				&& (std::abs(info.duration - 3000) <= 2)
				&& (samples.size() == expected(info))
				&& good(read),
			u"audio with a hole, silence at its place"_q,
			summary(samples, read));

		auto reader = AudioReader({
			.part = { .content = holed },
			.info = info,
		});
		auto pieces = std::vector<float>();
		for (auto till = crl::time(70); till < info.duration; till += 70) {
			reader.read(till * int64(1000), pieces);
		}
		reader.read(info.duration * int64(1000), pieces);
		check(
			!pieces.empty() && (pieces == samples),
			u"audio with a hole, read by pieces"_q,
			u"%1 samples"_q.arg(pieces.size()));

		const auto video = MakeVideo({ .content = holed }, {}, nullptr, nullptr);
		const auto videoInfo = ReadClipInfo(QString(), video.content);
		const auto again = ReadAudio({ .content = video.content });
		const auto kept = levels(again);
		check(
			video.ok
				&& video.audio
				&& (video.frames == 2)
				&& (std::abs(video.duration - 3000) <= 2)
				&& (again.size() == expected(videoInfo))
				&& good(kept),
			u"video with a hole in the audio"_q,
			clipSummary(video) + u", "_q + summary(again, kept));
	}

	// GIF.
	{
		const auto size = QSize(160, 120);
		constexpr auto kCount = 12;
		auto frames = std::vector<QImage>();
		for (auto i = 0; i != kCount; ++i) {
			frames.push_back(TestFrame(size, i / float64(kCount), false));
		}
		const auto gif = EncodeGif(frames, 100);
		const auto read = TestReadGif(gif);
		check(
			gif.startsWith("GIF89a"_q)
				&& gif.endsWith(';')
				&& gif.contains("NETSCAPE2.0\x03\x01\x00\x00"_q)
				&& (int(read.frames.size()) == kCount)
				&& (read.total == kCount * 100)
				&& ranges::all_of(read.frames, [&](const QImage &frame) {
					return frame.size() == size;
				})
				&& ranges::all_of(read.delays, [](int delay) {
					return delay == 100;
				}),
			u"GIF, read by Qt"_q,
			u"%1 bytes, %2 frames, %3 ms"_q
				.arg(gif.size())
				.arg(read.frames.size())
				.arg(read.total));
		if (int(read.frames.size()) == kCount) {
			const auto last = (kCount - 1) / float64(kCount);
			colors(read.frames.front(), {
				{ TestCircleCenter(size, 0.), kTestCircle },
				{ TestSquareRect(size, 0.).center(), kTestSquare },
			}, u"GIF, flat colors of the first frame"_q, 12);
			colors(read.frames.back(), {
				{ TestCircleCenter(size, last), kTestCircle },
				{ TestSquareRect(size, last).center(), kTestSquare },
			}, u"GIF, flat colors of the last frame"_q, 12);
			colors(read.frames.front(), {
				{ QPoint(156, 116), qRgb(248, 246, 128) },
			}, u"GIF, dithered gradient of the first frame"_q, 48);
			colors(read.frames.back(), {
				{ TestCircleCenter(size, 0.), qRgb(150, 42, 128) },
			}, u"GIF, dithered gradient of the last frame"_q, 48);
		}
		const auto info = ReadClipInfo(QString(), gif);
		check(
			info.valid()
				&& (info.codec == u"gif"_q)
				&& (info.size == size)
				&& (info.duration == kCount * 100),
			u"GIF, read by FFmpeg"_q,
			TestInfo(info));

		const auto thrice = EncodeGif(frames, 100, { .loops = 3 });
		const auto once = EncodeGif(frames, 100, { .loops = 1 });
		check(
			thrice.contains("NETSCAPE2.0\x03\x01\x02\x00"_q)
				&& !once.isEmpty()
				&& !once.contains("NETSCAPE"_q),
			u"GIF, loops"_q);
	}
	{
		// 256 colors of noise: every pixel is kept and the LZW table
		// is filled and cleared many times.
		const auto size = QSize(301, 199);
		auto seed = uint32(7);
		const auto noise = TestNoise(size, 1, 256, seed);
		const auto gif = EncodeGif({ noise }, 500, { .dither = false });
		const auto read = TestReadGif(gif);
		auto wrong = 0;
		if (read.frames.size() == 1 && read.frames.front().size() == size) {
			for (auto y = 0; y != size.height(); ++y) {
				for (auto x = 0; x != size.width(); ++x) {
					if (read.frames.front().pixel(x, y) != noise.pixel(x, y)) {
						++wrong;
					}
				}
			}
		} else {
			wrong = -1;
		}
		check(
			!wrong && (read.total == 500),
			u"GIF, lossless with 256 colors"_q,
			u"%1 bytes, %2 wrong pixels"_q.arg(gif.size()).arg(wrong));

		const auto few = EncodeGif({ noise }, 500, { .colors = 16 });
		const auto fewRead = TestReadGif(few);
		auto used = std::set<QRgb>();
		if (fewRead.frames.size() == 1) {
			const auto &image = fewRead.frames.front();
			for (auto y = 0; y != image.height(); ++y) {
				for (auto x = 0; x != image.width(); ++x) {
					used.insert(image.pixel(x, y));
				}
			}
		}
		check(
			!used.empty() && (used.size() <= 16),
			u"GIF, palette of 16 colors"_q,
			u"%1 colors"_q.arg(used.size()));
	}
	{
		// The last codes of a frame, when the table of codes grows right
		// before them. No two pixels go one after another twice here, so
		// every pixel is a code and the count of codes is known.
		auto wrong = QStringList();
		const auto counts = {
			1, 2, 3, 4, 7, 15, 31, 63, 127, 255, 256,
			767, 768, 1791, 1792, 3837, 3838, 3839, 3840,
		};
		for (const auto count : counts) {
			auto line = QImage(count, 1, QImage::Format_ARGB32_Premultiplied);
			const auto pixels = reinterpret_cast<QRgb*>(line.scanLine(0));
			auto index = 0;
			for (auto i = 0; i != count; ++i) {
				pixels[i] = qRgb(
					(index & 7) * 32 + 8,
					((index >> 3) & 7) * 32 + 8,
					((index >> 6) & 3) * 64 + 8);
				index = (index + 2 * (i / 256) + 1) & 0xFF;
			}
			const auto gif = EncodeGif({ line }, 100, { .dither = false });
			const auto read = TestStrictGif(gif, line.size());
			if (int(read.size()) != count
				|| !std::equal(begin(read), end(read), pixels)) {
				wrong.push_back(QString::number(count));
			}
		}
		check(
			wrong.isEmpty(),
			u"GIF, strict decoding to the end code"_q,
			wrong.isEmpty()
				? QString()
				: (u"wrong with %1 pixels"_q.arg(wrong.join(u", "_q))));
	}
	{
		// Frames that repeat and the ones that come too fast.
		const auto size = QSize(120, 90);
		const auto first = TestFrame(size, 0., false);
		const auto second = TestFrame(size, 0.5, false);
		const auto third = TestFrame(size, 1., false);
		const auto repeated = TestReadGif(EncodeGif(
			{ first, first, first, second, second },
			100));
		check(
			repeated.delays == std::vector<int>{ 300, 200 },
			u"GIF, repeated frames are merged"_q,
			u"%1 frames, %2 ms"_q
				.arg(repeated.frames.size())
				.arg(repeated.total));

		auto encoder = GifEncoder(size);
		encoder.add(first, 10);
		encoder.add(second, 10);
		encoder.add(third, 10);
		encoder.add(first, 70);
		const auto fast = TestReadGif(encoder.finish());
		check(
			fast.delays == std::vector<int>{ 20, 80 },
			u"GIF, too fast frames are dropped"_q,
			u"%1 frames, %2 ms"_q
				.arg(fast.frames.size())
				.arg(fast.total));
		if (fast.frames.size() == 2) {
			colors(fast.frames.back(), {
				{ TestCircleCenter(size, 1.), kTestCircle },
			}, u"GIF, the frame after the dropped ones"_q, 12);
		}
	}
	{
		// Transparency and pixels that become transparent again.
		const auto size = QSize(120, 90);
		const auto top = TestFrame(size, 0., true);
		const auto middle = TestFrame(size, 0.5, true);
		const auto bottom = TestFrame(size, 1., true);
		const auto gif = EncodeGif({ top, middle, bottom }, 200);
		const auto read = TestReadGif(gif);
		check(
			read.delays == std::vector<int>{ 200, 200, 200 },
			u"GIF, transparent frames"_q,
			u"%1 bytes, %2 frames, %3 ms"_q
				.arg(gif.size())
				.arg(read.frames.size())
				.arg(read.total));
		if (read.frames.size() == 3) {
			const auto was = QPoint(60, 5); // Only in the first circle.
			colors(read.frames[0], {
				{ was, kTestCircle },
				{ TestSquareRect(size, 0.).center(), kTestSquare },
				{ QPoint(2, 2), QRgb(0) },
				{ QPoint(117, 87), QRgb(0) },
			}, u"GIF, transparent background"_q, 8);
			colors(read.frames[1], {
				{ was, QRgb(0) },
				{ TestCircleCenter(size, 0.5), kTestCircle },
				{ TestSquareRect(size, 0.).center(), QRgb(0) },
				{ TestSquareRect(size, 0.5).center(), kTestSquare },
			}, u"GIF, pixels become transparent"_q, 8);
			colors(read.frames[2], {
				{ TestCircleCenter(size, 0.5), QRgb(0) },
				{ TestCircleCenter(size, 1.), kTestCircle },
				{ TestSquareRect(size, 1.).center(), kTestSquare },
			}, u"GIF, the last frame with transparency"_q, 8);
		}
	}
	{
		// 30 ms frames of a GIF are 33.3 fps: a sticker keeps them all
		// and shows them a bit slower.
		const auto size = QSize(128, 128);
		constexpr auto kCount = 12;
		auto frames = std::vector<QImage>();
		for (auto i = 0; i != kCount; ++i) {
			frames.push_back(TestFrame(size, i / float64(kCount), false));
		}
		const auto gif = EncodeGif(frames, 30);
		const auto sticker = MakeVideoSticker({
			.side = 128,
			.content = gif,
		}, nullptr, nullptr);
		const auto info = ReadClipInfo(QString(), sticker.webm);
		check(
			sticker.ok
				&& (sticker.size == size)
				&& (sticker.frames == kCount)
				&& (sticker.duration == 400)
				&& (info.fps > 29.9 && info.fps < 30.01),
			u"sticker from a 33.3 fps GIF"_q,
			stickerSummary(sticker) + u", "_q + TestInfo(info));
	}
	{
		const auto clip = MakeGif({
			.content = silent,
			.from = 120,
			.maxSide = 160,
		}, {}, nullptr, nullptr);
		const auto read = TestReadGif(clip.content);
		check(
			clip.ok
				&& (clip.size == QSize(160, 120))
				&& (clip.duration == 1080)
				&& !read.frames.empty()
				&& (int(read.frames.size()) <= clip.frames)
				&& (read.frames.front().size() == clip.size)
				&& (std::abs(read.total - 1080) <= 10),
			u"GIF from a video"_q,
			clipSummary(clip) + u", read %1 frames, %2 ms"_q
				.arg(read.frames.size())
				.arg(read.total));
	}

	log.push_back(u"video_core: %1 ms"_q.arg(crl::now() - started));
	return !failed;
}

} // namespace Oblivion::VideoCore
