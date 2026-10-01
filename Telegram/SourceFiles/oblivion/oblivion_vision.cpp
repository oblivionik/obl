/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_vision.h"

#include <QtGui/QPainter>

namespace Oblivion::Vision {
namespace {

constexpr auto kTestTextSize = 56;

[[nodiscard]] QImage TestTextImage(
		const QStringList &lines,
		QColor background,
		QColor foreground) {
	const auto step = kTestTextSize * 2;
	auto result = QImage(
		QSize(1400, step * int(lines.size()) + step),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(background);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	auto font = QFont(u"Helvetica Neue"_q);
	font.setPixelSize(kTestTextSize);
	p.setFont(font);
	p.setPen(foreground);
	auto top = step / 2;
	for (const auto &line : lines) {
		p.drawText(
			QRect(80, top, result.width() - 160, step),
			Qt::AlignLeft | Qt::AlignVCenter,
			line);
		top += step;
	}
	p.end();
	return result;
}

[[nodiscard]] QImage TestCircleImage() {
	auto result = QImage(QSize(800, 600), QImage::Format_ARGB32_Premultiplied);
	result.fill(QColor(0x33, 0x66, 0xcc));
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xf2, 0x4d, 0x33));
	p.drawEllipse(QPoint(400, 300), 150, 150);
	p.end();
	return result;
}

[[nodiscard]] QString OneLine(QString text) {
	return text.replace(QChar('\n'), u" | "_q);
}

} // namespace

#ifndef Q_OS_MAC

bool TextRecognitionSupported() {
	return false;
}

TextResult RecognizeText(const QImage &image) {
	return { .error = u"Text recognition is not supported."_q };
}

bool BackgroundRemovalSupported() {
	return false;
}

MaskResult RemoveBackground(const QImage &image) {
	return { .error = u"Background removal is not supported."_q };
}

#endif // !Q_OS_MAC

QRect ContentBounds(const QImage &image, int threshold) {
	if (image.isNull()) {
		return QRect();
	} else if (!image.hasAlphaChannel()) {
		return image.rect();
	}
	const auto format = image.format();
	const auto converted = (format == QImage::Format_ARGB32_Premultiplied
		|| format == QImage::Format_ARGB32)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const auto width = converted.width();
	const auto height = converted.height();
	const auto limit = std::clamp(threshold, 0, 254);
	auto left = width;
	auto right = -1;
	auto top = height;
	auto bottom = -1;
	for (auto y = 0; y != height; ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			converted.constScanLine(y));
		auto first = 0;
		while (first != width && qAlpha(line[first]) <= limit) {
			++first;
		}
		if (first == width) {
			continue;
		}
		auto last = width - 1;
		while (last > first && qAlpha(line[last]) <= limit) {
			--last;
		}
		left = std::min(left, first);
		right = std::max(right, last);
		top = std::min(top, y);
		bottom = y;
	}
	return (right < left)
		? QRect()
		: QRect(QPoint(left, top), QPoint(right, bottom));
}

QImage ApplyMask(
		const QImage &image,
		const float *mask,
		int columns,
		int rows,
		qsizetype stride) {
	if (image.isNull() || !mask || columns <= 0 || rows <= 0) {
		return QImage();
	}
	auto result = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	result.setDevicePixelRatio(1.);
	const auto width = result.width();
	const auto height = result.height();
	const auto same = (columns == width) && (rows == height);
	const auto bytes = reinterpret_cast<const uchar*>(mask);
	for (auto y = 0; y != height; ++y) {
		const auto maskY = same
			? y
			: std::min(int(int64(y) * rows / height), rows - 1);
		const auto from = reinterpret_cast<const float*>(
			bytes + maskY * stride);
		const auto to = reinterpret_cast<QRgb*>(result.scanLine(y));
		for (auto x = 0; x != width; ++x) {
			const auto value = from[same
				? x
				: std::min(int(int64(x) * columns / width), columns - 1)];
			// Written so that a NaN makes the pixel transparent.
			if (!(value > 0.002f)) {
				to[x] = 0;
			} else if (value < 0.998f) {
				const auto k = uint(value * 256.f + 0.5f);
				const auto pixel = to[x];
				to[x] = qRgba(
					(qRed(pixel) * k) >> 8,
					(qGreen(pixel) * k) >> 8,
					(qBlue(pixel) * k) >> 8,
					(qAlpha(pixel) * k) >> 8);
			}
		}
	}
	return result;
}

namespace details {

TextResult ComposeText(std::vector<TextFragment> fragments) {
	for (auto &fragment : fragments) {
		fragment.text = fragment.text.simplified();
	}
	fragments.erase(
		ranges::remove_if(fragments, [](const TextFragment &fragment) {
			return fragment.text.isEmpty();
		}),
		end(fragments));
	ranges::stable_sort(fragments, [](
			const TextFragment &a,
			const TextFragment &b) {
		return a.box.center().y() < b.box.center().y();
	});

	// A fragment continues the row if its center is inside the first
	// fragment of the row and the other way around, so a tall fragment
	// next to two short lines doesn't swallow both of them.
	auto result = TextResult();
	auto rows = QStringList();
	auto row = std::vector<const TextFragment*>();
	const auto flush = [&] {
		if (row.empty()) {
			return;
		}
		ranges::stable_sort(row, [](
				const TextFragment *a,
				const TextFragment *b) {
			return a->box.left() < b->box.left();
		});
		auto line = QString();
		for (const auto fragment : row) {
			if (!line.isEmpty()) {
				line.append(QChar(' '));
			}
			line.append(fragment->text);
		}
		rows.push_back(line);
		row.clear();
	};
	const auto sameRow = [](const TextFragment &a, const TextFragment &b) {
		const auto first = a.box.center().y();
		const auto second = b.box.center().y();
		return (first >= b.box.top())
			&& (first <= b.box.bottom())
			&& (second >= a.box.top())
			&& (second <= a.box.bottom());
	};
	for (const auto &fragment : fragments) {
		if (!row.empty() && !sameRow(*row.front(), fragment)) {
			flush();
		}
		row.push_back(&fragment);
	}
	flush();

	result.lines = int(rows.size());
	result.text = rows.join(QChar('\n'));
	return result;
}

} // namespace details

bool RunSelfTest(QStringList &log) {
	auto passed = true;
	const auto check = [&](bool condition, const QString &what) {
		if (!condition) {
			passed = false;
			log.push_back(u"FAIL: "_q + what);
		}
		return condition;
	};

	// The portable parts.
	{
		using details::TextFragment;
		const auto composed = details::ComposeText({
			TextFragment{ u"12:30"_q, QRectF(700, 12, 80, 30) },
			TextFragment{ u"third  line"_q, QRectF(20, 130, 300, 34) },
			TextFragment{ u"Name"_q, QRectF(20, 10, 120, 32) },
			TextFragment{ u"  "_q, QRectF(20, 300, 10, 10) },
			TextFragment{ u"second line"_q, QRectF(22, 70, 280, 30) },
		});
		check(
			(composed.text == u"Name 12:30\nsecond line\nthird line"_q)
				&& (composed.lines == 3),
			u"ComposeText order: "_q + OneLine(composed.text));
		check(
			details::ComposeText({}).text.isEmpty(),
			u"ComposeText of nothing"_q);

		auto image = QImage(QSize(8, 6), QImage::Format_ARGB32_Premultiplied);
		image.fill(QColor(200, 100, 50));
		auto mask = std::vector<float>(4 * 3, 0.f);
		mask[1 * 4 + 1] = 1.f;
		mask[1 * 4 + 2] = 0.5f;
		const auto cut = ApplyMask(image, mask.data(), 4, 3, 4 * sizeof(float));
		check(
			(cut.size() == image.size())
				&& (cut.format() == QImage::Format_ARGB32_Premultiplied),
			u"ApplyMask size / format"_q);
		if (!cut.isNull()) {
			check(
				(cut.pixel(0, 0) == 0)
					&& (cut.pixel(2, 2) == image.pixel(2, 2))
					&& (qAlpha(cut.pixel(5, 3)) == 127),
				u"ApplyMask values"_q);
			check(
				ContentBounds(cut) == QRect(2, 2, 4, 2),
				u"ContentBounds of a cutout"_q);
		}
		check(
			ContentBounds(image.convertToFormat(QImage::Format_RGB32))
				== image.rect(),
			u"ContentBounds of an opaque image"_q);
		auto empty = image;
		empty.fill(Qt::transparent);
		check(ContentBounds(empty).isEmpty(), u"ContentBounds of nothing"_q);
		log.push_back(u"vision: portable helpers checked"_q);
	}

	// Text recognition.
	if (!TextRecognitionSupported()) {
		log.push_back(
			u"vision: text recognition is not supported here, skipped"_q);
	} else {
		const auto lines = QStringList{
			u"Привет Oblivion 2026"_q,
			u"The quick brown fox jumps"_q,
		};
		const auto expect = [&](
				const QString &name,
				const QImage &image) {
			const auto started = crl::now();
			const auto result = RecognizeText(image);
			const auto elapsed = crl::now() - started;
			log.push_back(u"vision: %1: %2 in %3 ms, %4 lines: %5"_q.arg(
				name,
				result.ok ? u"ok"_q : (u"ERROR "_q + result.error),
				QString::number(elapsed),
				QString::number(result.lines),
				OneLine(result.text)));
			if (!check(result.ok, name + u" failed: "_q + result.error)) {
				return;
			}
			const auto lower = result.text.toLower();
			for (const auto &word : {
				u"привет"_q,
				u"oblivion"_q,
				u"2026"_q,
				u"quick brown fox"_q,
			}) {
				check(
					lower.contains(word),
					name + u": '"_q + word + u"' not found"_q);
			}
			check(
				lower.indexOf(u"привет"_q) < lower.indexOf(u"quick"_q),
				name + u": wrong line order"_q);
		};
		expect(
			u"dark on white"_q,
			TestTextImage(lines, Qt::white, Qt::black));
		expect(
			u"white on dark"_q,
			TestTextImage(lines, QColor(24, 26, 32), Qt::white));
		expect(
			u"dark on transparent"_q,
			TestTextImage(lines, Qt::transparent, QColor(16, 16, 16)));
		expect(
			u"white on transparent"_q,
			TestTextImage(lines, Qt::transparent, Qt::white));

		auto blank = QImage(QSize(640, 480), QImage::Format_RGB32);
		blank.fill(QColor(0x33, 0x66, 0xcc));
		const auto nothing = RecognizeText(blank);
		check(
			nothing.ok && nothing.text.isEmpty() && !nothing.lines,
			u"a blank image must give ok and no text, got: "_q
				+ (nothing.ok ? OneLine(nothing.text) : nothing.error));
		const auto null = RecognizeText(QImage());
		check(
			!null.ok && !null.error.isEmpty(),
			u"a null image must give an error"_q);
	}

	// Background removal: a synthetic picture may have "no foreground",
	// the call only has to return cleanly either way.
	if (!BackgroundRemovalSupported()) {
		log.push_back(
			u"vision: background removal is not supported here, skipped"_q);
	} else {
		const auto image = TestCircleImage();
		const auto started = crl::now();
		const auto result = RemoveBackground(image);
		const auto elapsed = crl::now() - started;
		if (result.ok) {
			const auto &cutout = result.cutout;
			log.push_back(
				u"vision: background removed in %1 ms, bounds %2,%3 %4x%5"_q
					.arg(QString::number(elapsed))
					.arg(result.bounds.x())
					.arg(result.bounds.y())
					.arg(result.bounds.width())
					.arg(result.bounds.height()));
			const auto valid = check(
				(cutout.size() == image.size())
					&& (cutout.format()
						== QImage::Format_ARGB32_Premultiplied),
				u"cutout size / format"_q);
			check(
				!result.bounds.isEmpty()
					&& image.rect().contains(result.bounds),
				u"cutout bounds"_q);
			if (valid) {
				log.push_back(u"vision: cutout alpha center %1, corner %2"_q
					.arg(qAlpha(cutout.pixel(400, 300)))
					.arg(qAlpha(cutout.pixel(2, 2))));
				auto visible = 0;
				for (auto y = 0; y < cutout.height(); y += 4) {
					for (auto x = 0; x < cutout.width(); x += 4) {
						visible += (qAlpha(cutout.pixel(x, y)) > 127) ? 1 : 0;
					}
				}
				check(visible > 0, u"the cutout has no visible pixels"_q);
			}
		} else {
			log.push_back(u"vision: no cutout in %1 ms (%2): %3"_q.arg(
				QString::number(elapsed),
				result.nothingFound ? u"nothing found"_q : u"error"_q,
				result.error));
			check(
				!result.error.isEmpty() && result.cutout.isNull(),
				u"a failed removal must have an error and no image"_q);
		}
		const auto null = RemoveBackground(QImage());
		check(
			!null.ok && !null.error.isEmpty(),
			u"a null image must give an error"_q);
	}
	return passed;
}

} // namespace Oblivion::Vision
