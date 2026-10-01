/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtGui/QImage>

#include <atomic>
#include <vector>

// FFmpeg side of "send a video as a round video message": reading frames
// for the editor preview and converting a fragment of the video to what
// Ui::RoundVideoRecorder produces (ui/controls/round_video_recorder.cpp):
// 400x400 H.264 (libopenh264, 2 Mbit/s) + AAC (48 kHz mono, 64 kbit/s)
// in mp4, at most 60 seconds long.
namespace Oblivion::RoundVideo {

// Same limits as Ui::RoundVideoRecorder.
inline constexpr auto kSide = 400;
inline constexpr auto kMaxDuration = crl::time(60'000);
inline constexpr auto kMinDuration = crl::time(200);

struct Source {
	QString path; // Preferred when both are set.
	QByteArray content;

	[[nodiscard]] bool empty() const {
		return path.isEmpty() && content.isEmpty();
	}
};

struct Info {
	crl::time duration = 0;
	QSize size; // Displayed size: pixel aspect ratio and rotation applied.
	int rotation = 0; // Clockwise degrees: 0, 90, 180 or 270.
	bool hasAudio = false;

	[[nodiscard]] bool valid() const {
		return (duration > 0) && !size.isEmpty();
	}
};

// The square that becomes the round video: its side is the shorter side
// of the displayed frame, position (0..1) moves it along the longer side.
[[nodiscard]] QRectF CropSquare(QSizeF size, float64 position);

// Frames for the editor. Not thread-safe: use it from one thread (queue).
class Reader final {
public:
	explicit Reader(Source source);
	~Reader();

	[[nodiscard]] const Info &info() const;

	// Displayed frame (aspect ratio and rotation applied) at the position
	// in ms, fitted into maxSide x maxSide. Null on errors.
	[[nodiscard]] QImage frame(crl::time position, int maxSide);

	// Keyframes spread over the whole video, center-cropped to squares
	// side x side. Null images where decoding failed.
	[[nodiscard]] std::vector<QImage> thumbnails(int count, int side);

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

struct Request {
	Source source;
	crl::time from = 0;
	crl::time till = 0; // Clamped to from + kMaxDuration.
	float64 position = 0.5; // See CropSquare().
	bool mute = false; // The result gets a silent audio track.
};

struct Result {
	QByteArray content; // mp4, empty on errors and when cancelled.
	crl::time duration = 0;

	[[nodiscard]] bool empty() const {
		return content.isEmpty();
	}
};

// Heavy and synchronous, call it off the main thread. progress (0..1) is
// called on the calling thread, cancelled is polled all the time.
[[nodiscard]] Result Convert(
	const Request &request,
	const std::atomic<bool> &cancelled,
	Fn<void(float64)> progress = nullptr);

// Encodes synthetic videos and converts them (crop, rotation, trimming,
// silent tracks), see oblivion_selftest.h. No Core::App() needed.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::RoundVideo
