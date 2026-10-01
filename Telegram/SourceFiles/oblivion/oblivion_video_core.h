/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QRect>
#include <QtGui/QImage>

#include <atomic>
#include <memory>
#include <vector>

// Video encoders that the FFmpeg build here doesn't have: WebM VP9 (through
// the statically linked libvpx, vpx_codec_vp9_cx, with a small Matroska
// muxer of its own) for Telegram video stickers and an animated GIF
// encoder. Plus an H.264 mp4 writer (with AAC audio or without any, which
// is what Telegram calls a GIF) and a sequential frame reader with
// trimming, cropping, scaling and rotation, both through FFmpeg.
//
// MakeVideoSticker(), MakeVideo(), MakeGifVideo() and MakeGif() take one
// fragment of a file or several of them joined together (ClipPart), so
// a video editor needs nothing else to export what it has.
//
// Everything is synchronous, keeps no global state and is safe to call
// from any thread, heavy calls go through crl::async with the result
// delivered by crl::on_main.
namespace Oblivion::VideoCore {

// Telegram video sticker limits: VP9 in WebM, no audio, one side exactly
// 512 (the other one up to 512), up to 3 seconds, up to 30 fps, up to
// 256 KB. Custom emoji: 100x100, they are kept within 64 KB here, which
// is plenty for that size and fits whichever limit the server has.
inline constexpr auto kStickerSide = 512;
inline constexpr auto kEmojiSide = 100;
inline constexpr auto kStickerMaxDuration = crl::time(3000);
inline constexpr auto kStickerMaxFps = 30;
inline constexpr auto kStickerMaxBytes = 256 * 1024;
inline constexpr auto kEmojiMaxBytes = 64 * 1024;

// Defaults of MakeVideo(), MakeGifVideo() and MakeGif().
inline constexpr auto kVideoMaxSide = 1920;
inline constexpr auto kVideoMaxFps = 30;
inline constexpr auto kGifVideoMaxSide = 1280;
inline constexpr auto kGifVideoMaxFps = 30;
inline constexpr auto kGifMaxSide = 480;
inline constexpr auto kGifMaxFps = 15;

using Cancel = std::shared_ptr<std::atomic<bool>>;

struct ClipInfo {
	crl::time duration = 0;
	QSize size; // Displayed size: pixel aspect ratio and rotation applied.
	int rotation = 0; // Clockwise degrees: 0, 90, 180 or 270.
	float64 fps = 0.; // 0 if unknown.
	bool hasAudio = false;
	bool hasAlpha = false; // The pixel format has it, frames may be opaque.
	QString codec; // FFmpeg name of the video codec: "h264", "vp9", "gif".
	QString container; // FFmpeg name of the demuxer: "matroska,webm".

	[[nodiscard]] bool valid() const {
		return (duration > 0) && !size.isEmpty();
	}
};

// Reads the headers (scans the packets where the container has no
// duration, like GIF). content is used when path is empty.
[[nodiscard]] ClipInfo ReadClipInfo(
	const QString &path,
	const QByteArray &content = QByteArray());

struct FrameOptions {
	QString path; // Any video or GIF the FFmpeg build here can decode.
	QByteArray content; // Used when path is empty.
	crl::time from = 0;
	crl::time till = 0; // 0 = the end.
	QRect crop; // Displayed source pixels, empty = the whole frame.
	QSize size; // Exact size of the frames, empty = the size of the crop.

	// On average not more frames than that for a second, the ones that
	// come faster are dropped. 0 = keep all.
	int maxFps = 0;
};

struct Frame {
	QImage image; // ARGB32_Premultiplied, upright.
	crl::time position = 0; // From FrameOptions::from, the first one is 0.
	crl::time duration = 0; // Till the next frame / the end, at least 1.
};

// Decodes the frames one by one, in order, and gives them to callback on
// the calling thread until it returns false. Returns false (with a
// technical description in error) if nothing could be read or if it was
// cancelled, true if the range was read to the end or stopped by callback.
[[nodiscard]] bool ReadFrames(
	const FrameOptions &options,
	Fn<bool(Frame &&frame)> callback,
	Cancel cancel = nullptr,
	QString *error = nullptr);

// The frame that is shown at options.from, null on errors. Opens the file
// every time: fine for a preview of the crop, too slow for a playback.
[[nodiscard]] QImage ReadFrame(const FrameOptions &options);

// A fragment of a file, for the results made of several ones.
struct ClipPart {
	QString path; // Any video or GIF the FFmpeg build here can decode.
	QByteArray content; // Used when path is empty.
	crl::time from = 0;
	crl::time till = 0; // 0 = the end.
	QRect crop; // Displayed source pixels, empty = the default one.
};

// The audio of a fragment (the crop is ignored) as interleaved float
// samples, 48 kHz stereo: exactly as many of them as the fragment takes,
// with silence where the track has nothing. Empty if the file can't be
// read or has no audio track. Opens the file every time, so a long
// fragment is better read in blocks of several seconds (the video editor
// changes the tempo of the blocks, oblivion_video_project.h).
[[nodiscard]] std::vector<float> ReadAudio(const ClipPart &part);

struct VideoStickerOptions {
	QString path; // Any video or GIF the FFmpeg build here can decode.
	crl::time from = 0;
	crl::time till = 0; // 0 = the end, clamped to from + kStickerMaxDuration.
	QRect crop; // Displayed source pixels, empty = the center square.
	int side = kStickerSide; // kEmojiSide for a custom emoji.
	QByteArray content; // Used when path is empty.

	// Not empty: these fragments are joined (everything after
	// kStickerMaxDuration is dropped) and path, content, from, till and
	// crop above are ignored. The first crop gives the proportions, the
	// other fragments are fitted into them on a transparent background.
	std::vector<ClipPart> parts;
};

struct VideoStickerResult {
	QByteArray webm; // Fits all the limits above when ok.
	bool ok = false;
	QString error; // Technical description for the log, not for the UI.
	QSize size;
	crl::time duration = 0;
	int frames = 0;
	bool alpha = false; // Has a transparency channel.
	int attempts = 0; // How many times it was encoded to fit the size.
};

// Heavy and synchronous, call it off the main thread. progress (0..1)
// is called on the calling thread, cancel (may be null) is polled all
// the time, a cancelled conversion returns ok == false.
//
// The longer side of the crop becomes exactly side, the other one keeps
// the proportions. A custom emoji (side == kEmojiSide) is always a square:
// a crop that isn't one is centered on a transparent background.
// Transparency of the source (GIF, WebM with alpha) is kept.
//
// Frames that come faster than kStickerMaxFps are dropped (60 fps become
// 30). A source that is only a bit faster (30 ms frames of a GIF) keeps
// all the frames and is shown slower by that bit instead. If the result
// doesn't fit the size at the lowest bitrate, every second frame goes.
[[nodiscard]] VideoStickerResult MakeVideoSticker(
	const VideoStickerOptions &options,
	Fn<void(float64)> progress,
	Cancel cancel);

// The same for frames in memory: the proportions of the first frame are
// used, frames after kStickerMaxDuration are dropped and so are the ones
// that come faster than kStickerMaxFps.
//
// Whole milliseconds can't tell the usual frame rates exactly, so 16 / 17
// are read as 60 fps, 33 / 34 as 30 fps, 41 / 42 as 24 fps and so on.
[[nodiscard]] VideoStickerResult EncodeVideoSticker(
	const std::vector<QImage> &frames,
	crl::time frameDuration,
	int side = kStickerSide,
	Fn<void(float64)> progress = nullptr,
	Cancel cancel = nullptr);

struct GifOptions {
	int loops = 0; // 0 = forever.
	int colors = 256; // Palette size, 2..256.
	bool dither = true;
};

// Streaming GIF89a encoder, for long clips that shouldn't be kept in
// memory as a whole. Frames of a different size are scaled to size.
//
// Every frame gets a palette of its own, only the changed part of a frame
// is written, pixels with alpha below 128 stay transparent. Frames that
// come faster than 50 fps and the ones that change nothing are merged
// with the previous one, the timing is kept.
class GifEncoder final {
public:
	GifEncoder(QSize size, GifOptions options = {});
	~GifEncoder();

	// duration in ms (GIF keeps 10 ms units, the error is carried over).
	void add(const QImage &frame, crl::time duration);

	// Empty on errors or if no frames were added.
	[[nodiscard]] QByteArray finish();

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

// The same for frames in memory, the size of the first frame is used.
[[nodiscard]] QByteArray EncodeGif(
	const std::vector<QImage> &frames,
	crl::time frameDuration,
	GifOptions options = {});

struct Mp4Options {
	int bitrate = 0; // Video, bits per second, 0 = by the size and fps.
	int fps = 30; // A hint for the rate control, frames keep their time.

	// An AAC track, both 0 = a silent video without any audio track.
	int audioRate = 0; // 44100 or 48000.
	int audioChannels = 0; // 1 or 2.
};

// Streaming H.264 (libopenh264) mp4 writer. Without the audio options
// it gives what Telegram calls a GIF: a video with no audio track.
// The sides are rounded down to even, frames of a different size are
// scaled, transparent pixels are shown on white.
class Mp4Encoder final {
public:
	explicit Mp4Encoder(QSize size, Mp4Options options = {});
	~Mp4Encoder();

	[[nodiscard]] bool valid() const;
	[[nodiscard]] QSize size() const;
	[[nodiscard]] crl::time duration() const; // Of the added frames.

	// Frames follow each other, duration in ms. False on errors.
	bool add(const QImage &frame, crl::time duration);

	// Interleaved float samples in [-1, 1], frames * audioChannels of
	// them. Add them as the video goes (after every frame the samples
	// for its duration), not all at once in the end.
	bool addAudio(const float *samples, int64 frames);

	// Empty on errors or if no frames were added.
	[[nodiscard]] QByteArray finish();

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

struct ClipOptions {
	QString path; // Any video or GIF the FFmpeg build here can decode.
	QByteArray content; // Used when path is empty.
	crl::time from = 0;
	crl::time till = 0; // 0 = the end.
	QRect crop; // Displayed source pixels, empty = the whole frame.
	int maxSide = 0; // The longer side, 0 = the default of the format.

	// 0 = the default of the format. Frames that come faster are dropped,
	// but a source that is less than 13% faster keeps all its frames.
	int maxFps = 0;

	// Not empty: these fragments are joined and path, content, from, till
	// and crop above are ignored. The first crop gives the size of the
	// result, the other fragments are fitted into it on black.
	std::vector<ClipPart> parts;
};

struct ClipResult {
	QByteArray content; // The whole file.
	bool ok = false;
	QString error; // Technical description for the log, not for the UI.
	QSize size;
	crl::time duration = 0;
	int frames = 0;
	bool audio = false; // Has an audio track.
};

struct VideoOptions {
	int bitrate = 0; // Video, bits per second, 0 = by the size and fps.

	// No audio track at all. Otherwise there is one (AAC, 48 kHz stereo)
	// if at least one of the fragments has audio, the other fragments
	// are silent in it.
	bool mute = false;
};

// Fragments of videos as an H.264 mp4: trimming, cropping and joining.
// The video is only scaled down, never up (but a crop with a side below
// 16 pixels is fitted into the 16 the encoder needs), the sides are
// rounded down to even. Heavy and synchronous, progress and cancel work
// the same way as in MakeVideoSticker().
[[nodiscard]] ClipResult MakeVideo(
	const ClipOptions &options,
	VideoOptions video,
	Fn<void(float64)> progress,
	Cancel cancel);

// The same without audio and with the defaults of a "Telegram GIF":
// it is sent as a GIF when the file has no audio track.
[[nodiscard]] ClipResult MakeGifVideo(
	const ClipOptions &options,
	Fn<void(float64)> progress,
	Cancel cancel);

// Fragments of videos as a real .gif file.
[[nodiscard]] ClipResult MakeGif(
	const ClipOptions &options,
	GifOptions gif,
	Fn<void(float64)> progress,
	Cancel cancel);

// Self-checks for OBLIVION_SELFTEST=video_core, see oblivion_selftest.h.
// Runs before Core::Application exists (no Core::App(), no session).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::VideoCore
