/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_round_video_convert.h"

#include "base/debug_log.h"
#include "ffmpeg/ffmpeg_bytes_io_wrap.h"
#include "ffmpeg/ffmpeg_utility.h"

#include <QtCore/QFile>
#include <QtGui/QColor>
#include <QtGui/QTransform>

#include <array>
#include <cerrno>
#include <cmath>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
} // extern "C"

namespace Oblivion::RoundVideo {
namespace {

using namespace FFmpeg;

// Encoding settings of Ui::RoundVideoRecorder.
constexpr auto kAudioFrequency = 48'000;
constexpr auto kAudioBitRate = 64 * 1024;
constexpr auto kVideoBitRate = 2 * 1024 * 1024;

// The recorder gets ~30 fps from a camera, faster videos drop frames.
constexpr auto kFrameRate = 30;
constexpr auto kSecond = int64(1'000'000);
constexpr auto kFrameGap = kSecond / kFrameRate;
constexpr auto kFrameGapMin = kFrameGap - (kFrameGap / 10);

// Long GOPs must not hang the preview.
constexpr auto kReaderDecodeLimit = 1200;

[[nodiscard]] int64 SamplesFor(int64 microseconds, int frequency) {
	return (microseconds * frequency) / kSecond;
}

class Input final {
public:
	Input() = default;
	Input(const Input &other) = delete;
	Input &operator=(const Input &other) = delete;

	[[nodiscard]] bool open(const Source &source);

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

bool Input::open(const Source &source) {
	if (!source.path.isEmpty()) {
		_file = std::make_unique<QFile>(source.path);
		if (!_file->open(QIODevice::ReadOnly)) {
			LOG(("Oblivion Round Error: Could not open '%1' for reading."
				).arg(source.path));
			return false;
		}
	} else if (!source.content.isEmpty()) {
		_bytes = source.content;
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

// How a decoded frame is shown: stretched by the pixel aspect ratio,
// then rotated clockwise.
struct Geometry {
	QSize stored;
	float64 aspect = 1.;
	int rotation = 0;

	[[nodiscard]] QSizeF unrotated() const {
		return QSizeF(stored.width() * aspect, stored.height());
	}
	[[nodiscard]] QSizeF displayed() const {
		const auto size = unrotated();
		return (rotation == 90 || rotation == 270)
			? size.transposed()
			: size;
	}

	// The crop square in stored frame pixels.
	[[nodiscard]] QRect crop(float64 position) const {
		const auto rotate = QTransform().rotate(rotation);
		const auto bounds = rotate.mapRect(QRectF(QPointF(), unrotated()));
		const auto toDisplayed = rotate
			* QTransform::fromTranslate(-bounds.x(), -bounds.y());
		const auto square = toDisplayed.inverted().mapRect(
			CropSquare(displayed(), position));
		const auto left = int(std::round(square.x() / aspect));
		const auto top = int(std::round(square.y()));
		const auto right = int(std::round(
			(square.x() + square.width()) / aspect));
		const auto bottom = int(std::round(square.y() + square.height()));
		return QRect(left, top, right - left, bottom - top).intersected(
			QRect(QPoint(), stored));
	}
};

[[nodiscard]] QSize RoundedSize(QSizeF size) {
	return QSize(
		std::max(int(std::round(size.width())), 1),
		std::max(int(std::round(size.height())), 1));
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

// sws ignores the color properties of frames: BT.709 (HD and phone
// videos) or BT.2020 colors would shift and full range videos would lose
// their shadows and highlights. A YUV result is BT.601 limited range,
// what Ui::RoundVideoRecorder writes and players assume for an untagged
// video of this size. HDR transfers (PQ, HLG) are not tone mapped.
bool ApplyColorspace(
		not_null<SwsContext*> context,
		not_null<const AVFrame*> frame) {
	return sws_setColorspaceDetails(
		context,
		sws_getCoefficients(FrameColorspace(frame)),
		FrameFullRange(frame) ? 1 : 0,
		sws_getCoefficients(SWS_CS_DEFAULT),
		0,
		0,
		1 << 16,
		1 << 16) >= 0;
}

// Frame -> YUV420P of the given size. Unlike MakeSwscalePointer() the
// source range is set before the init: it chooses the converters, and
// an unscaled copy would skip the range conversion.
[[nodiscard]] SwscalePointer MakeYuvScaler(
		not_null<const AVFrame*> frame,
		QSize size) {
	auto result = SwscalePointer(sws_alloc_context());
	if (!result) {
		LogError(u"sws_alloc_context"_q);
		return nullptr;
	}
	const auto context = result.get();
	const auto set = [&](const char *name, int64 value) {
		return av_opt_set_int(context, name, value, 0) >= 0;
	};
	if (!set("srcw", frame->width)
		|| !set("srch", frame->height)
		|| !set("src_format", frame->format)
		|| !set("src_range", FrameFullRange(frame) ? 1 : 0)
		|| !set("dstw", size.width())
		|| !set("dsth", size.height())
		|| !set("dst_format", AV_PIX_FMT_YUV420P)
		|| !set("dst_range", 0)) {
		LogError(u"av_opt_set_int"_q, u"swscale"_q);
		return nullptr;
	}
	const auto error = AvErrorWrap(sws_init_context(
		context,
		nullptr,
		nullptr));
	if (error) {
		LogError(u"sws_init_context"_q, error);
		return nullptr;
	} else if (!ApplyColorspace(context, frame)) {
		// The default BT.601 matrix stays, the video is still converted.
		LogError(u"sws_setColorspaceDetails"_q);
	}
	return result;
}

// Copies a square plane rotating it clockwise.
void CopyPlane(
		const uint8_t *from,
		int fromStride,
		uint8_t *to,
		int toStride,
		int size,
		int rotation) {
	for (auto y = 0; y != size; ++y) {
		const auto line = to + y * toStride;
		switch (rotation) {
		case 90:
			for (auto x = 0; x != size; ++x) {
				line[x] = from[(size - 1 - x) * fromStride + y];
			}
			break;
		case 180: {
			const auto source = from + (size - 1 - y) * fromStride;
			for (auto x = 0; x != size; ++x) {
				line[x] = source[size - 1 - x];
			}
		} break;
		case 270:
			for (auto x = 0; x != size; ++x) {
				line[x] = from[x * fromStride + (size - 1 - y)];
			}
			break;
		default:
			memcpy(line, from + y * fromStride, size);
			break;
		}
	}
}

// Decoded frame -> the kSide x kSide YUV420P square, upright.
//
// The whole frame is scaled so that the crop square becomes exactly
// kSide x kSide (per axis, so non-square pixels are fixed too), then
// the square is copied out of it, rotated if the video has to be.
class Cropper final {
public:
	Cropper(not_null<AVStream*> stream, float64 position);

	[[nodiscard]] bool process(
		not_null<AVFrame*> frame,
		not_null<AVFrame*> to);

private:
	[[nodiscard]] bool prepare(not_null<AVFrame*> frame);

	const not_null<AVStream*> _stream;
	const float64 _position = 0.5;
	const int _rotation = 0;
	Geometry _geometry;
	int _format = AV_PIX_FMT_NONE;
	int _colorspace = SWS_CS_DEFAULT;
	bool _fullRange = false;
	QSize _scaled;
	QPoint _offset;
	FramePointer _frame;
	SwscalePointer _swscale;

};

Cropper::Cropper(not_null<AVStream*> stream, float64 position)
: _stream(stream)
, _position(std::clamp(position, 0., 1.))
, _rotation(ReadRotationFromMetadata(stream)) {
}

bool Cropper::prepare(not_null<AVFrame*> frame) {
	const auto geometry = Geometry{
		.stored = QSize(frame->width, frame->height),
		.aspect = PixelAspect(frame->sample_aspect_ratio, _stream),
		.rotation = _rotation,
	};
	const auto colorspace = FrameColorspace(frame);
	const auto fullRange = FrameFullRange(frame);
	if (_frame
		&& geometry.stored == _geometry.stored
		&& geometry.aspect == _geometry.aspect
		&& frame->format == _format
		&& colorspace == _colorspace
		&& fullRange == _fullRange) {
		return true;
	}
	_geometry = geometry;
	_format = frame->format;
	_colorspace = colorspace;
	_fullRange = fullRange;
	_frame = nullptr;
	_swscale = nullptr;

	const auto crop = geometry.crop(_position);
	if (crop.width() < 2 || crop.height() < 2) {
		LOG(("Oblivion Round Error: Bad crop for %1x%2."
			).arg(frame->width
			).arg(frame->height));
		return false;
	}
	const auto scaled = [](int full, int part) {
		const auto result = int(std::round(full * float64(kSide) / part));
		return (std::max(result, kSide) + 1) & ~1;
	};
	_scaled = QSize(
		scaled(geometry.stored.width(), crop.width()),
		scaled(geometry.stored.height(), crop.height()));
	const auto offset = [](int start, int part, int scaled) {
		const auto result = int(std::round(start * float64(kSide) / part));
		return std::clamp(result, 0, scaled - kSide) & ~1;
	};
	_offset = QPoint(
		offset(crop.x(), crop.width(), _scaled.width()),
		offset(crop.y(), crop.height(), _scaled.height()));

	_swscale = MakeYuvScaler(frame, _scaled);
	if (!_swscale) {
		return false;
	}
	_frame = MakeFramePointer();
	if (!_frame) {
		return false;
	}
	_frame->format = AV_PIX_FMT_YUV420P;
	_frame->width = _scaled.width();
	_frame->height = _scaled.height();
	const auto error = AvErrorWrap(av_frame_get_buffer(_frame.get(), 0));
	if (error) {
		LogError(u"av_frame_get_buffer"_q, error);
		_frame = nullptr;
		return false;
	}
	return true;
}

bool Cropper::process(not_null<AVFrame*> frame, not_null<AVFrame*> to) {
	if (!FrameHasData(frame) || !prepare(frame)) {
		return false;
	}
	sws_scale(
		_swscale.get(),
		frame->data,
		frame->linesize,
		0,
		frame->height,
		_frame->data,
		_frame->linesize);
	for (auto plane = 0; plane != 3; ++plane) {
		const auto shift = plane ? 1 : 0;
		const auto stride = _frame->linesize[plane];
		const auto from = _frame->data[plane]
			+ (_offset.y() >> shift) * stride
			+ (_offset.x() >> shift);
		CopyPlane(
			from,
			stride,
			to->data[plane],
			to->linesize[plane],
			kSide >> shift,
			_geometry.rotation);
	}
	return true;
}

struct EncoderDescriptor {
	int width = kSide;
	int height = kSide;
	int audioFrequency = kAudioFrequency;
	int audioChannels = 1;
	int rotation = 0; // Display matrix, used for self-test sources.
	bool audio = true;
};

// Same streams as Ui::RoundVideoRecorder::Private writes.
class Encoder final {
public:
	explicit Encoder(EncoderDescriptor descriptor);

	[[nodiscard]] bool valid() const {
		return !_failed;
	}

	// A writable YUV420P frame to fill before writeVideo().
	[[nodiscard]] AVFrame *prepareVideoFrame();
	[[nodiscard]] bool writeVideo(int64 microseconds);

	// Interleaved samples, audioChannels per frame.
	[[nodiscard]] bool writeAudio(const float *samples, int64 frames);
	[[nodiscard]] bool writeSilence(int64 frames);
	[[nodiscard]] int64 audioFrames() const {
		return _audioFrames;
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

	const EncoderDescriptor _descriptor;

	WriteBytesWrap _result; // Before _format, it is destroyed after it.
	FormatPointer _format;

	AVStream *_videoStream = nullptr;
	CodecPointer _videoCodec;
	FramePointer _videoFrame;

	AVStream *_audioStream = nullptr;
	CodecPointer _audioCodec;
	FramePointer _audioFrame;
	std::vector<float> _audioQueue;
	int64 _audioPts = 0;
	int64 _audioFrames = 0;

	bool _failed = false;

};

Encoder::Encoder(EncoderDescriptor descriptor)
: _descriptor(descriptor) {
	_format = MakeWriteFormatPointer(
		static_cast<void*>(&_result),
		nullptr,
		&WriteBytesWrap::Write,
		&WriteBytesWrap::Seek,
		"mp4"_q);
	if (!_format
		|| !initVideo()
		|| (_descriptor.audio && !initAudio())) {
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

bool Encoder::fail() {
	_failed = true;
	return false;
}

bool Encoder::initVideo() {
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
	_videoCodec->width = _descriptor.width;
	_videoCodec->height = _descriptor.height;
	_videoCodec->time_base = AVRational{ 1, int(kSecond) };
	_videoCodec->framerate = AVRational{ kFrameRate, 1 };
	_videoCodec->pix_fmt = AV_PIX_FMT_YUV420P;
	_videoCodec->bit_rate = kVideoBitRate;

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

bool Encoder::initAudio() {
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
	_audioCodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
	_audioCodec->bit_rate = kAudioBitRate;
	_audioCodec->sample_rate = _descriptor.audioFrequency;
	_audioCodec->time_base = AVRational{ 1, _descriptor.audioFrequency };
	av_channel_layout_default(
		&_audioCodec->ch_layout,
		_descriptor.audioChannels);

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

AVFrame *Encoder::prepareVideoFrame() {
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

bool Encoder::writeVideo(int64 microseconds) {
	if (_failed) {
		return false;
	}
	_videoFrame->pts = microseconds;
	_videoFrame->duration = kFrameGap;
	return writeFrame(_videoFrame.get(), _videoCodec, _videoStream);
}

bool Encoder::writeAudio(const float *samples, int64 frames) {
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
	_audioFrames += frames;

	const auto frameSize = _audioCodec->frame_size;
	while (int64(_audioQueue.size()) >= int64(frameSize) * channels) {
		if (!encodeAudio(frameSize)) {
			return false;
		}
	}
	return true;
}

bool Encoder::writeSilence(int64 frames) {
	if (frames <= 0) {
		return !_failed;
	}
	const auto zeros = std::vector<float>(
		frames * _descriptor.audioChannels,
		0.f);
	return writeAudio(zeros.data(), frames);
}

bool Encoder::encodeAudio(int frames) {
	const auto error = AvErrorWrap(av_frame_make_writable(
		_audioFrame.get()));
	if (error) {
		LogError(u"av_frame_make_writable"_q, error, u"AAC"_q);
		return fail();
	}
	const auto channels = _descriptor.audioChannels;
	_audioFrame->nb_samples = frames;
	for (auto channel = 0; channel != channels; ++channel) {
		const auto to = reinterpret_cast<float*>(
			_audioFrame->data[channel]);
		for (auto i = 0; i != frames; ++i) {
			to[i] = _audioQueue[i * channels + channel];
		}
	}
	_audioQueue.erase(
		begin(_audioQueue),
		begin(_audioQueue) + frames * channels);
	_audioFrame->pts = _audioPts;
	_audioPts += frames;
	return writeFrame(_audioFrame.get(), _audioCodec, _audioStream);
}

bool Encoder::writeFrame(
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

QByteArray Encoder::finish() {
	if (_failed) {
		return QByteArray();
	}
	if (_audioCodec) {
		const auto left = int(_audioQueue.size())
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
	return base::take(_result.content);
}

// Decodes packets of one stream, the caller feeds them.
class Decoder final {
public:
	Decoder(not_null<AVStream*> stream, CodecPointer codec)
	: _stream(stream)
	, _codec(std::move(codec))
	, _frame(MakeFramePointer()) {
	}

	[[nodiscard]] not_null<AVStream*> stream() const {
		return _stream;
	}
	[[nodiscard]] AVCodecContext *codec() const {
		return _codec.get();
	}

	// Calls handler for every decoded frame, stops when it returns false.
	template <typename Handler>
	bool send(const AVPacket *packet, Handler &&handler) {
		while (true) {
			const auto error = AvErrorWrap(avcodec_send_packet(
				_codec.get(),
				packet));
			if (error.code() == AVERROR(EAGAIN)) {
				if (!drain(handler)) {
					return false;
				}
				continue;
			} else if (error && error.code() != AVERROR_EOF) {
				// Broken packets are skipped.
				return true;
			}
			return drain(handler);
		}
	}

	void flush() {
		avcodec_flush_buffers(_codec.get());
	}

private:
	template <typename Handler>
	bool drain(Handler &&handler) {
		while (true) {
			const auto error = AvErrorWrap(avcodec_receive_frame(
				_codec.get(),
				_frame.get()));
			if (error) {
				return true;
			}
			const auto proceed = handler(_frame.get());
			av_frame_unref(_frame.get());
			if (!proceed) {
				return false;
			}
		}
	}

	const not_null<AVStream*> _stream;
	const CodecPointer _codec;
	const FramePointer _frame;

};

[[nodiscard]] int64 FramePts(not_null<AVFrame*> frame) {
	return (frame->best_effort_timestamp != AV_NOPTS_VALUE)
		? frame->best_effort_timestamp
		: frame->pts;
}

// Any audio -> interleaved 48 kHz mono float. Unlike
// MakeSwresamplePointer() the downmix is normalized: for float output
// libswresample keeps the stereo -> mono matrix at 0.71 L + 0.71 R, so
// loud stereo sources would clip.
[[nodiscard]] SwresamplePointer MakeMonoResampler(
		const AVChannelLayout *layout,
		AVSampleFormat format,
		int rate,
		SwresamplePointer *existing) {
	if (*existing) {
		const auto &deleter = existing->get_deleter();
		if (deleter.srcChannels == layout->nb_channels
			&& deleter.srcFormat == format
			&& deleter.srcRate == rate) {
			return std::move(*existing);
		}
	}
	auto mono = AVChannelLayout();
	av_channel_layout_default(&mono, 1);
	auto context = (SwrContext*)nullptr;
	auto error = AvErrorWrap(swr_alloc_set_opts2(
		&context,
		&mono,
		AV_SAMPLE_FMT_FLT,
		kAudioFrequency,
		layout,
		format,
		rate,
		0,
		nullptr));
	if (error || !context) {
		LogError(u"swr_alloc_set_opts2"_q, error);
		return SwresamplePointer();
	}
	auto result = SwresamplePointer(context, {
		.srcFormat = format,
		.srcRate = rate,
		.srcChannels = layout->nb_channels,
		.dstFormat = AV_SAMPLE_FMT_FLT,
		.dstRate = kAudioFrequency,
		.dstChannels = 1,
	});
	error = AvErrorWrap(av_opt_set_double(
		context,
		"rematrix_maxval",
		1.,
		0));
	if (error) {
		LogError(u"av_opt_set_double"_q, error, u"rematrix_maxval"_q);
		return SwresamplePointer();
	}
	error = AvErrorWrap(swr_init(context));
	if (error) {
		LogError(u"swr_init"_q, error);
		return SwresamplePointer();
	}
	return result;
}

} // namespace

QRectF CropSquare(QSizeF size, float64 position) {
	const auto side = std::min(size.width(), size.height());
	position = std::clamp(position, 0., 1.);
	return (size.width() > size.height())
		? QRectF((size.width() - side) * position, 0., side, side)
		: QRectF(0., (size.height() - side) * position, side, side);
}

struct Reader::Private {
	explicit Private(Source source);

	[[nodiscard]] bool open();
	[[nodiscard]] bool seek(int64 position);
	[[nodiscard]] bool next();
	[[nodiscard]] int64 position(not_null<AVFrame*> frame) const;
	[[nodiscard]] QImage image(not_null<AVFrame*> frame, int maxSide);

	const Source source;
	std::unique_ptr<Input> input;
	AVStream *stream = nullptr;
	CodecPointer codec;
	FramePointer frame;
	SwscalePointer swscale;
	Packet packet;
	Info info;
	int64 last = 0;
	bool eof = false;

};

Reader::Private::Private(Source source)
: source(std::move(source)) {
	if (!open()) {
		info = Info();
		return;
	}
	auto result = Info();
	const auto format = input->format();
	const auto par = stream->codecpar;
	const auto geometry = Geometry{
		.stored = QSize(par->width, par->height),
		.aspect = PixelAspect(AVRational{ 0, 1 }, stream),
		.rotation = ReadRotationFromMetadata(stream),
	};
	result.size = RoundedSize(geometry.displayed());
	result.rotation = geometry.rotation;
	result.hasAudio = (FindStream(format, AVMEDIA_TYPE_AUDIO) >= 0);
	result.duration = input->duration(stream);
	if (result.duration <= 0) {
		// No duration in the container, scan the packets (GIFs).
		auto &fields = packet.fields();
		while (av_read_frame(format, &fields) >= 0) {
			if (fields.stream_index == stream->index
				&& fields.pts != AV_NOPTS_VALUE) {
				const auto end = input->position(
					fields.pts + std::max(fields.duration, int64(0)),
					stream);
				result.duration = std::max(
					result.duration,
					crl::time((end + 999) / 1000));
			}
			av_packet_unref(&fields);
		}
		if (!open()) {
			return;
		}
	}
	info = result;
}

bool Reader::Private::open() {
	input = std::make_unique<Input>();
	stream = nullptr;
	codec = nullptr;
	eof = false;
	if (!input->open(source)) {
		return false;
	}
	const auto format = input->format();
	const auto index = FindStream(format, AVMEDIA_TYPE_VIDEO);
	if (index < 0) {
		LOG(("Oblivion Round Error: No video stream."));
		return false;
	}
	stream = format->streams[index];
	codec = MakeCodecPointer({ .stream = stream });
	frame = MakeFramePointer();
	return (codec != nullptr) && (frame != nullptr);
}

bool Reader::Private::seek(int64 position) {
	if (!codec) {
		return false;
	}
	const auto error = AvErrorWrap(av_seek_frame(
		input->format(),
		stream->index,
		input->pts(std::max(position, int64(0)), stream),
		AVSEEK_FLAG_BACKWARD));
	if (error) {
		// Some formats can't seek, start from the beginning instead.
		return open();
	}
	avcodec_flush_buffers(codec.get());
	eof = false;
	return true;
}

bool Reader::Private::next() {
	while (true) {
		const auto received = AvErrorWrap(avcodec_receive_frame(
			codec.get(),
			frame.get()));
		if (!received) {
			return true;
		} else if (received.code() != AVERROR(EAGAIN) || eof) {
			return false;
		}
		auto &fields = packet.fields();
		while (true) {
			if (av_read_frame(input->format(), &fields) < 0) {
				eof = true;
				avcodec_send_packet(codec.get(), nullptr);
				break;
			}
			const auto ours = (fields.stream_index == stream->index);
			if (ours) {
				avcodec_send_packet(codec.get(), &fields);
			}
			av_packet_unref(&fields);
			if (ours) {
				break;
			}
		}
	}
}

int64 Reader::Private::position(not_null<AVFrame*> frame) const {
	const auto pts = FramePts(frame);
	return (pts != AV_NOPTS_VALUE)
		? input->position(pts, stream)
		: (last + kFrameGap);
}

QImage Reader::Private::image(not_null<AVFrame*> frame, int maxSide) {
	if (!FrameHasData(frame)) {
		return QImage();
	}
	const auto unrotated = QSizeF(
		frame->width * PixelAspect(frame->sample_aspect_ratio, stream),
		frame->height);
	const auto scale = std::min(
		1.,
		maxSide / std::max(unrotated.width(), unrotated.height()));
	const auto size = RoundedSize(unrotated * scale);
	auto storage = CreateFrameStorage(size);
	if (storage.isNull()) {
		return QImage();
	}
	swscale = MakeSwscalePointer(frame, size, &swscale);
	if (!swscale) {
		return QImage();
	}
	// The preview shows the colors the round video will have. For RGB
	// the range works after the init as well, through the tables.
	ApplyColorspace(swscale.get(), frame);
	uint8_t *data[AV_NUM_DATA_POINTERS] = { storage.bits(), nullptr };
	int linesize[AV_NUM_DATA_POINTERS] = {
		int(storage.bytesPerLine()),
		0,
	};
	sws_scale(
		swscale.get(),
		frame->data,
		frame->linesize,
		0,
		frame->height,
		data,
		linesize);
	const auto descriptor = av_pix_fmt_desc_get(
		AVPixelFormat(frame->format));
	if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA)) {
		PremultiplyInplace(storage);
	}
	return info.rotation
		? storage.transformed(QTransform().rotate(info.rotation))
		: storage;
}

Reader::Reader(Source source)
: _private(std::make_unique<Private>(std::move(source))) {
}

Reader::~Reader() = default;

const Info &Reader::info() const {
	return _private->info;
}

QImage Reader::frame(crl::time position, int maxSide) {
	auto &d = *_private;
	if (!d.info.valid() || !d.seek(position * 1000)) {
		return QImage();
	}
	const auto target = position * 1000 - kFrameGap / 2;
	auto chosen = MakeFramePointer();
	auto has = false;
	for (auto i = 0; i != kReaderDecodeLimit && d.next(); ++i) {
		const auto at = d.position(d.frame.get());
		d.last = at;
		av_frame_unref(chosen.get());
		av_frame_move_ref(chosen.get(), d.frame.get());
		has = true;
		if (at >= target) {
			break;
		}
	}
	return has ? d.image(chosen.get(), maxSide) : QImage();
}

std::vector<QImage> Reader::thumbnails(int count, int side) {
	auto &d = *_private;
	auto result = std::vector<QImage>(std::max(count, 0));
	if (!d.info.valid() || side <= 0) {
		return result;
	}
	const auto duration = d.info.duration * int64(1000);
	for (auto i = 0; i != count; ++i) {
		const auto position = (duration * (2 * i + 1)) / (2 * count);
		if (!d.seek(position) || !d.next()) {
			continue;
		}
		const auto image = d.image(d.frame.get(), side * 2);
		av_frame_unref(d.frame.get());
		if (image.isNull()) {
			continue;
		}
		const auto square = std::min(image.width(), image.height());
		result[i] = image.copy(
			(image.width() - square) / 2,
			(image.height() - square) / 2,
			square,
			square
		).scaled(
			side,
			side,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}
	return result;
}

Result Convert(
		const Request &request,
		const std::atomic<bool> &cancelled,
		Fn<void(float64)> progress) {
	auto input = Input();
	if (!input.open(request.source)) {
		return {};
	}
	const auto format = input.format();
	const auto videoIndex = FindStream(format, AVMEDIA_TYPE_VIDEO);
	if (videoIndex < 0) {
		LOG(("Oblivion Round Error: No video stream to convert."));
		return {};
	}
	const auto videoStream = format->streams[videoIndex];
	auto videoCodec = MakeCodecPointer({ .stream = videoStream });
	if (!videoCodec) {
		return {};
	}
	auto video = Decoder(videoStream, std::move(videoCodec));

	const auto audioIndex = request.mute
		? -1
		: FindStream(format, AVMEDIA_TYPE_AUDIO);
	auto audioCodec = (audioIndex >= 0)
		? MakeCodecPointer({ .stream = format->streams[audioIndex] })
		: CodecPointer();
	auto audio = std::optional<Decoder>();
	if (audioCodec) {
		audio.emplace(format->streams[audioIndex], std::move(audioCodec));
	}

	const auto duration = input.duration(videoStream);
	const auto from = (duration > 0)
		? std::clamp(request.from, crl::time(0), duration)
		: std::max(request.from, crl::time(0));
	auto till = (request.till > from) ? request.till : (from + kMaxDuration);
	till = std::min(till, from + kMaxDuration);
	if (duration > 0) {
		till = std::min(till, std::max(duration, from + kMinDuration));
	}
	const auto fromUs = from * int64(1000);
	const auto tillUs = till * int64(1000);
	const auto lengthUs = tillUs - fromUs;
	const auto neededSamples = SamplesFor(lengthUs, kAudioFrequency);

	auto encoder = Encoder(EncoderDescriptor());
	if (!encoder.valid()) {
		return {};
	}
	auto cropper = Cropper(videoStream, request.position);

	if (fromUs > 0) {
		const auto error = AvErrorWrap(av_seek_frame(
			format,
			videoIndex,
			input.pts(fromUs, videoStream),
			AVSEEK_FLAG_BACKWARD));
		if (error) {
			// Decode from the beginning, earlier frames are skipped.
			LogError(u"av_seek_frame"_q, error);
		}
		video.flush();
		if (audio) {
			audio->flush();
		}
	}

	auto failed = false;
	auto videoDone = false;
	auto audioDone = !audio;
	auto written = false;
	auto lastWritten = int64(0);
	auto lastPosition = int64(0);
	auto lastProgress = -1;
	auto pending = MakeFramePointer(); // The last frame before 'from'.
	auto hasPending = false;

	const auto encode = [&](not_null<AVFrame*> frame, int64 at) {
		const auto to = encoder.prepareVideoFrame();
		if (!to) {
			failed = true;
			return;
		} else if (!cropper.process(frame, to)) {
			// Broken frames are skipped, the previous one stays longer.
			return;
		} else if (!encoder.writeVideo(at)) {
			failed = true;
			return;
		}
		written = true;
		lastWritten = at;
		if (!audio) {
			// Keep the silent track interleaved with the video.
			const auto samples = SamplesFor(at, kAudioFrequency)
				- encoder.audioFrames();
			if (samples > 0 && !encoder.writeSilence(samples)) {
				failed = true;
				return;
			}
		}
		const auto percent = int((at * 100) / std::max(lengthUs, int64(1)));
		if (progress && percent != lastProgress) {
			lastProgress = percent;
			progress(std::clamp(at / float64(lengthUs), 0., 1.));
		}
	};
	const auto handleVideo = [&](not_null<AVFrame*> frame) {
		if (videoDone || failed) {
			return !failed;
		}
		const auto pts = FramePts(frame);
		const auto position = (pts != AV_NOPTS_VALUE)
			? input.position(pts, videoStream)
			: (lastPosition + kFrameGap);
		lastPosition = position;
		if (position < fromUs) {
			av_frame_unref(pending.get());
			hasPending = (av_frame_ref(pending.get(), frame) >= 0);
			return true;
		} else if (position >= tillUs) {
			videoDone = true;
			return true;
		}
		if (!written && hasPending && position > fromUs) {
			encode(pending.get(), 0);
		}
		av_frame_unref(pending.get());
		hasPending = false;
		if (!written) {
			encode(frame, 0);
		} else {
			const auto at = position - fromUs;
			if (at - lastWritten >= kFrameGapMin) {
				encode(frame, at);
			}
		}
		return !failed;
	};

	auto swresample = SwresamplePointer();
	auto audioStarted = false;
	auto audioSkip = int64(0);
	auto converted = std::vector<float>();
	const auto handleAudio = [&](not_null<AVFrame*> frame) {
		if (audioDone || failed) {
			return !failed;
		}
		const auto stream = audio->stream();
		const auto rate = frame->sample_rate;
		if (rate <= 0 || frame->nb_samples <= 0) {
			return true;
		}
		const auto pts = FramePts(frame);
		const auto position = (pts != AV_NOPTS_VALUE)
			? input.position(pts, stream)
			: fromUs;
		const auto end = position
			+ (int64(frame->nb_samples) * kSecond) / rate;
		if (!audioStarted && end <= fromUs) {
			return true;
		}
		auto layout = AVChannelLayout();
		if (frame->ch_layout.nb_channels > 0) {
			av_channel_layout_copy(&layout, &frame->ch_layout);
		} else {
			av_channel_layout_default(
				&layout,
				std::max(audio->codec()->ch_layout.nb_channels, 1));
		}
		swresample = MakeMonoResampler(
			&layout,
			AVSampleFormat(frame->format),
			rate,
			&swresample);
		av_channel_layout_uninit(&layout);
		if (!swresample) {
			failed = true;
			return false;
		}
		const auto capacity = swr_get_out_samples(
			swresample.get(),
			frame->nb_samples);
		if (capacity <= 0) {
			return true;
		}
		converted.resize(capacity);
		auto out = reinterpret_cast<uint8_t*>(converted.data());
		const auto count = swr_convert(
			swresample.get(),
			&out,
			capacity,
			const_cast<const uint8_t**>(frame->extended_data),
			frame->nb_samples);
		if (count < 0) {
			LogError(u"swr_convert"_q, AvErrorWrap(count));
			failed = true;
			return false;
		}
		auto data = converted.data();
		auto left = int64(count);
		if (!audioStarted) {
			audioStarted = true;
			const auto offset = SamplesFor(position - fromUs, kAudioFrequency);
			if (offset > 0) {
				if (!encoder.writeSilence(std::min(offset, neededSamples))) {
					failed = true;
					return false;
				}
			} else {
				audioSkip = -offset;
			}
		}
		const auto skip = std::min(audioSkip, left);
		audioSkip -= skip;
		data += skip;
		left -= skip;
		left = std::min(left, neededSamples - encoder.audioFrames());
		if (left > 0 && !encoder.writeAudio(data, left)) {
			failed = true;
			return false;
		}
		if (encoder.audioFrames() >= neededSamples) {
			audioDone = true;
		}
		return true;
	};

	auto packet = Packet();
	auto &fields = packet.fields();
	while (!failed && (!videoDone || !audioDone)) {
		if (cancelled) {
			return {};
		}
		if (av_read_frame(format, &fields) < 0) {
			if (!videoDone) {
				video.send(nullptr, handleVideo);
			}
			if (!audioDone && audio) {
				audio->send(nullptr, handleAudio);
			}
			break;
		}
		const auto index = fields.stream_index;
		if (index == videoIndex && !videoDone) {
			video.send(&fields, handleVideo);
		} else if (index == audioIndex && !audioDone && audio) {
			audio->send(&fields, handleAudio);
		}
		av_packet_unref(&fields);
	}
	if (failed || cancelled) {
		return {};
	} else if (!written) {
		LOG(("Oblivion Round Error: No frames in %1..%2."
			).arg(from
			).arg(till));
		return {};
	}

	// Audio covers exactly the video, with silence where it is missing.
	const auto videoEnd = std::min(lengthUs, lastWritten + kFrameGap);
	const auto audioEnd = SamplesFor(videoEnd, kAudioFrequency);
	if (encoder.audioFrames() < audioEnd
		&& !encoder.writeSilence(audioEnd - encoder.audioFrames())) {
		return {};
	}
	const auto audioDuration = (encoder.audioFrames() * kSecond)
		/ kAudioFrequency;
	auto result = Result{
		.content = encoder.finish(),
		.duration = (std::max(videoEnd, audioDuration) + 999) / 1000,
	};
	if (result.content.isEmpty() || result.duration < kMinDuration) {
		return {};
	}
	if (progress) {
		progress(1.);
	}
	return result;
}

namespace {

struct Rgb {
	int r = 0;
	int g = 0;
	int b = 0;
};

constexpr auto kRed = Rgb{ 255, 0, 0 };
constexpr auto kGreen = Rgb{ 0, 255, 0 };
constexpr auto kBlue = Rgb{ 0, 0, 255 };

[[nodiscard]] uint8_t Clamped(float64 value) {
	return uint8_t(std::clamp(int(std::lround(value)), 0, 255));
}

// BT.601, limited range: what sws expects from YUV420P by default.
[[nodiscard]] std::array<uint8_t, 3> ToYuv(Rgb c) {
	return {
		Clamped(16. + 0.257 * c.r + 0.504 * c.g + 0.098 * c.b),
		Clamped(128. - 0.148 * c.r - 0.291 * c.g + 0.439 * c.b),
		Clamped(128. + 0.439 * c.r - 0.368 * c.g - 0.071 * c.b),
	};
}

// Vertical bands: red, green, blue, from left to right.
void FillBands(not_null<AVFrame*> frame) {
	const auto width = frame->width;
	const auto colors = std::array<std::array<uint8_t, 3>, 3>{
		ToYuv(kRed),
		ToYuv(kGreen),
		ToYuv(kBlue),
	};
	const auto band = [&](int x) {
		return std::min((x * 3) / width, 2);
	};
	for (auto plane = 0; plane != 3; ++plane) {
		const auto shift = plane ? 1 : 0;
		const auto w = width >> shift;
		const auto h = frame->height >> shift;
		for (auto y = 0; y != h; ++y) {
			const auto line = frame->data[plane] + y * frame->linesize[plane];
			for (auto x = 0; x != w; ++x) {
				line[x] = colors[band(x << shift)][plane];
			}
		}
	}
}

[[nodiscard]] QByteArray MakeTestVideo(
		QSize size,
		int rotation,
		bool audio,
		crl::time duration) {
	constexpr auto kFrequency = 44'100;
	constexpr auto kChannels = 2;
	auto encoder = Encoder({
		.width = size.width(),
		.height = size.height(),
		.audioFrequency = kFrequency,
		.audioChannels = kChannels,
		.rotation = rotation,
		.audio = audio,
	});
	if (!encoder.valid()) {
		return QByteArray();
	}
	const auto frames = int((duration * kFrameRate) / 1000);
	auto samples = std::vector<float>();
	auto written = int64(0);
	for (auto i = 0; i != frames; ++i) {
		const auto frame = encoder.prepareVideoFrame();
		if (!frame) {
			return QByteArray();
		}
		FillBands(frame);
		if (!encoder.writeVideo(i * kFrameGap)) {
			return QByteArray();
		}
		if (audio) {
			const auto till = (int64(i + 1) * kFrequency) / kFrameRate;
			samples.clear();
			for (auto s = written; s != till; ++s) {
				const auto value = float(0.5 * std::sin(
					2. * M_PI * 440. * s / kFrequency));
				for (auto c = 0; c != kChannels; ++c) {
					samples.push_back(value);
				}
			}
			if (!encoder.writeAudio(samples.data(), till - written)) {
				return QByteArray();
			}
			written = till;
		}
	}
	return encoder.finish();
}

struct Probe {
	bool opened = false;
	QSize size;
	AVCodecID videoCodec = AV_CODEC_ID_NONE;
	AVCodecID audioCodec = AV_CODEC_ID_NONE;
	int audioFrequency = 0;
	int audioChannels = 0;
	crl::time duration = 0;
	int rotation = 0;
	QImage first;
};

[[nodiscard]] Probe ProbeVideo(const QByteArray &content) {
	auto result = Probe();
	auto input = Input();
	if (!input.open({ .content = content })) {
		return result;
	}
	const auto format = input.format();
	const auto video = FindStream(format, AVMEDIA_TYPE_VIDEO);
	if (video < 0) {
		return result;
	}
	const auto stream = format->streams[video];
	result.opened = true;
	result.size = QSize(stream->codecpar->width, stream->codecpar->height);
	result.videoCodec = stream->codecpar->codec_id;
	result.rotation = ReadRotationFromMetadata(stream);
	result.duration = (format->duration != AV_NOPTS_VALUE)
		? (format->duration / 1000)
		: 0;
	const auto audio = FindStream(format, AVMEDIA_TYPE_AUDIO);
	if (audio >= 0) {
		const auto par = format->streams[audio]->codecpar;
		result.audioCodec = par->codec_id;
		result.audioFrequency = par->sample_rate;
		result.audioChannels = par->ch_layout.nb_channels;
	}
	auto reader = Reader({ .content = content });
	result.first = reader.frame(0, kSide);
	return result;
}

// The amplitude of a sine as loud as the audio track (its RMS * sqrt(2)),
// negative if it can't be read.
//
// Not the loudest sample: the AAC encoder (64 kbit/s, as the recorder
// uses) overshoots on pure tones now and then, a 0.5 sine encoded with
// no conversion at all comes back with single peaks up to 0.96, which
// says nothing about the level of the downmix.
[[nodiscard]] float64 AudioLevel(const QByteArray &content) {
	auto input = Input();
	if (!input.open({ .content = content })) {
		return -1.;
	}
	const auto format = input.format();
	const auto index = FindStream(format, AVMEDIA_TYPE_AUDIO);
	if (index < 0) {
		return -1.;
	}
	const auto stream = format->streams[index];
	auto codec = MakeCodecPointer({ .stream = stream });
	if (!codec) {
		return -1.;
	}
	auto decoder = Decoder(stream, std::move(codec));
	auto squares = 0.;
	auto samplesCount = int64(0);
	auto failed = false;
	const auto handler = [&](not_null<AVFrame*> frame) {
		const auto planar = (frame->format == AV_SAMPLE_FMT_FLTP);
		if (!planar && frame->format != AV_SAMPLE_FMT_FLT) {
			failed = true;
			return false;
		}
		const auto channels = frame->ch_layout.nb_channels;
		const auto planes = planar ? channels : 1;
		const auto count = frame->nb_samples * (planar ? 1 : channels);
		for (auto plane = 0; plane != planes; ++plane) {
			const auto samples = reinterpret_cast<const float*>(
				frame->extended_data[plane]);
			for (auto i = 0; i != count; ++i) {
				squares += float64(samples[i]) * samples[i];
			}
			samplesCount += count;
		}
		return true;
	};
	auto packet = Packet();
	auto &fields = packet.fields();
	while (!failed && av_read_frame(format, &fields) >= 0) {
		if (fields.stream_index == index) {
			decoder.send(&fields, handler);
		}
		av_packet_unref(&fields);
	}
	if (!failed) {
		decoder.send(nullptr, handler);
	}
	return (failed || !samplesCount)
		? -1.
		: std::sqrt(2. * squares / samplesCount);
}

[[nodiscard]] bool Near(QColor color, Rgb expected) {
	constexpr auto kTolerance = 80;
	return (std::abs(color.red() - expected.r) <= kTolerance)
		&& (std::abs(color.green() - expected.g) <= kTolerance)
		&& (std::abs(color.blue() - expected.b) <= kTolerance);
}

[[nodiscard]] QString Describe(QColor color) {
	return u"rgb(%1,%2,%3)"_q
		.arg(color.red())
		.arg(color.green())
		.arg(color.blue());
}

} // namespace

bool RunSelfTest(QStringList &log) {
	auto passed = 0;
	auto failed = 0;
	const auto check = [&](bool ok, const QString &name, QString details) {
		log.push_back((ok ? u"ok   "_q : u"FAIL "_q)
			+ name
			+ (details.isEmpty() ? QString() : (u": "_q + details)));
		(ok ? passed : failed) += 1;
		return ok;
	};
	const auto pixel = [](const QImage &image, int x, int y) {
		return (x < image.width() && y < image.height())
			? QColor(image.pixel(x, y))
			: QColor(Qt::transparent);
	};
	const auto checkColor = [&](
			const QImage &image,
			QPoint point,
			Rgb expected,
			const QString &name) {
		const auto color = pixel(image, point.x(), point.y());
		check(
			Near(color, expected),
			name,
			u"(%1,%2) is %3"_q
				.arg(point.x())
				.arg(point.y())
				.arg(Describe(color)));
	};
	const auto never = std::atomic<bool>(false);

	check(
		CropSquare(QSizeF(640, 360), 0.5) == QRectF(140, 0, 360, 360),
		u"crop square, landscape middle"_q,
		QString());
	check(
		CropSquare(QSizeF(360, 640), 1.) == QRectF(0, 280, 360, 360),
		u"crop square, portrait bottom"_q,
		QString());

	// Landscape 640x360, stereo 44.1 kHz audio.
	const auto landscape = MakeTestVideo(QSize(640, 360), 0, true, 3000);
	if (!check(!landscape.isEmpty(), u"encode landscape source"_q, {})) {
		return false;
	}
	{
		auto reader = Reader({ .content = landscape });
		const auto &info = reader.info();
		check(
			info.valid()
				&& info.size == QSize(640, 360)
				&& info.rotation == 0
				&& info.hasAudio
				&& std::abs(info.duration - 3000) <= 150,
			u"reader info, landscape"_q,
			u"%1x%2, rotation %3, audio %4, %5 ms"_q
				.arg(info.size.width())
				.arg(info.size.height())
				.arg(info.rotation)
				.arg(info.hasAudio ? 1 : 0)
				.arg(info.duration));
		const auto frame = reader.frame(1000, 320);
		check(
			frame.size() == QSize(320, 180),
			u"reader frame size"_q,
			u"%1x%2"_q.arg(frame.width()).arg(frame.height()));
		checkColor(frame, { 20, 90 }, kRed, u"reader frame, left"_q);
		checkColor(frame, { 160, 90 }, kGreen, u"reader frame, middle"_q);
		checkColor(frame, { 300, 90 }, kBlue, u"reader frame, right"_q);
		const auto thumbnails = reader.thumbnails(4, 32);
		check(
			thumbnails.size() == 4
				&& ranges::all_of(thumbnails, [](const QImage &image) {
					return image.size() == QSize(32, 32);
				}),
			u"reader thumbnails"_q,
			QString());
	}
	const auto checkResult = [&](
			const Result &result,
			const QString &name,
			crl::time duration,
			Rgb nearColor, // 'near' and 'far' are macros in <windows.h>.
			Rgb farColor,
			bool vertical) {
		if (!check(!result.empty(), name + u", converted"_q, {})) {
			return;
		}
		const auto probe = ProbeVideo(result.content);
		check(
			probe.opened
				&& probe.size == QSize(kSide, kSide)
				&& probe.videoCodec == AV_CODEC_ID_H264
				&& probe.rotation == 0,
			name + u", video stream"_q,
			u"%1x%2, codec %3, rotation %4"_q
				.arg(probe.size.width())
				.arg(probe.size.height())
				.arg(int(probe.videoCodec))
				.arg(probe.rotation));
		check(
			probe.audioCodec == AV_CODEC_ID_AAC
				&& probe.audioFrequency == kAudioFrequency
				&& probe.audioChannels == 1,
			name + u", audio stream"_q,
			u"codec %1, %2 Hz, %3 channels"_q
				.arg(int(probe.audioCodec))
				.arg(probe.audioFrequency)
				.arg(probe.audioChannels));
		check(
			std::abs(result.duration - duration) <= 150
				&& std::abs(probe.duration - duration) <= 200,
			name + u", duration"_q,
			u"result %1 ms, file %2 ms, expected %3 ms"_q
				.arg(result.duration)
				.arg(probe.duration)
				.arg(duration));
		const auto nearPoint = vertical ? QPoint(200, 40) : QPoint(40, 200);
		const auto farPoint = vertical ? QPoint(200, 370) : QPoint(370, 200);
		checkColor(
			probe.first,
			nearPoint,
			nearColor,
			name + u", start side"_q);
		checkColor(probe.first, farPoint, farColor, name + u", end side"_q);
	};

	{
		const auto result = Convert({
			.source = { .content = landscape },
			.from = 500,
			.till = 2500,
			.position = 0.,
		}, never);
		checkResult(
			result,
			u"landscape, left crop"_q,
			2000,
			kRed,
			kGreen,
			false);

		// The same 0.5 sine in both channels: the mono downmix keeps it,
		// an unnormalized one gives 0.71 and clips loud sources.
		const auto level = AudioLevel(result.content);
		check(
			level > 0.35 && level < 0.62,
			u"landscape, left crop, downmix level"_q,
			QString::number(level));
	}
	checkResult(
		Convert({
			.source = { .content = landscape },
			.from = 1000,
			.till = 1000 + 90'000,
			.position = 1.,
			.mute = true,
		}, never),
		u"landscape, right crop, muted, long range"_q,
		2000,
		kGreen,
		kBlue,
		false);

	// Stored 640x360, shown rotated clockwise: 360x640 with the red band
	// on top, no audio track at all.
	const auto rotated = MakeTestVideo(QSize(640, 360), 90, false, 2000);
	if (!check(!rotated.isEmpty(), u"encode rotated source"_q, {})) {
		return false;
	}
	{
		auto reader = Reader({ .content = rotated });
		const auto &info = reader.info();
		check(
			info.valid()
				&& info.size == QSize(360, 640)
				&& info.rotation == 90
				&& !info.hasAudio,
			u"reader info, rotated"_q,
			u"%1x%2, rotation %3, audio %4"_q
				.arg(info.size.width())
				.arg(info.size.height())
				.arg(info.rotation)
				.arg(info.hasAudio ? 1 : 0));
		const auto frame = reader.frame(500, 320);
		check(
			frame.size() == QSize(180, 320),
			u"reader rotated frame size"_q,
			u"%1x%2"_q.arg(frame.width()).arg(frame.height()));
		checkColor(frame, { 90, 20 }, kRed, u"reader rotated, top"_q);
		checkColor(frame, { 90, 300 }, kBlue, u"reader rotated, bottom"_q);
	}
	checkResult(
		Convert({
			.source = { .content = rotated },
			.from = 0,
			.till = 1500,
			.position = 0.,
		}, never),
		u"rotated, top crop, silent track"_q,
		1500,
		kRed,
		kGreen,
		true);
	checkResult(
		Convert({
			.source = { .content = rotated },
			.from = 200,
			.till = 2000,
			.position = 1.,
		}, never),
		u"rotated, bottom crop"_q,
		1800,
		kGreen,
		kBlue,
		true);

	// Upside down: blue on the left. Counterclockwise: blue on top.
	for (const auto rotation : { 180, 270 }) {
		const auto name = u"rotation %1"_q.arg(rotation);
		const auto source = MakeTestVideo(
			QSize(640, 360),
			rotation,
			true,
			2000);
		if (!check(!source.isEmpty(), name + u", encode source"_q, {})) {
			continue;
		}
		const auto vertical = (rotation == 270);
		auto reader = Reader({ .content = source });
		check(
			reader.info().rotation == rotation,
			name + u", read back"_q,
			QString::number(reader.info().rotation));
		const auto frame = reader.frame(500, 320);
		checkColor(
			frame,
			vertical ? QPoint(90, 20) : QPoint(20, 90),
			kBlue,
			name + u", reader start side"_q);
		checkColor(
			frame,
			vertical ? QPoint(90, 300) : QPoint(300, 90),
			kRed,
			name + u", reader end side"_q);
		checkResult(
			Convert({
				.source = { .content = source },
				.till = 1500,
				.position = 0.,
			}, never),
			name + u", start crop"_q,
			1500,
			kBlue,
			kGreen,
			vertical);
		checkResult(
			Convert({
				.source = { .content = source },
				.till = 1500,
				.position = 1.,
			}, never),
			name + u", end crop"_q,
			1500,
			kGreen,
			kRed,
			vertical);
	}

	{
		const auto cancelled = std::atomic<bool>(true);
		const auto result = Convert({
			.source = { .content = landscape },
			.till = 3000,
		}, cancelled);
		check(result.empty(), u"cancelled conversion"_q, QString());
	}
	{
		auto reports = 0;
		auto last = 0.;
		const auto result = Convert({
			.source = { .content = landscape },
			.till = 3000,
		}, never, [&](float64 value) {
			++reports;
			last = value;
		});
		check(
			!result.empty() && reports > 2 && last == 1.,
			u"progress reports"_q,
			u"%1 reports, last %2"_q.arg(reports).arg(last));
	}
	check(
		Convert({
			.source = { .content = QByteArray("not a video") },
		}, never).empty(),
		u"garbage input"_q,
		QString());

	log.push_back(u"%1 passed, %2 failed"_q.arg(passed).arg(failed));
	return !failed;
}

} // namespace Oblivion::RoundVideo
