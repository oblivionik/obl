/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_vision.h"

#include "base/platform/mac/base_utilities_mac.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

// Apple Vision implementation. Vision.framework is already linked for
// every target (cmake/options_mac.cmake), the APIs newer than the
// deployment target (macOS 12.0) are guarded with @available.
//
// The target is built without ARC: everything created with alloc / Create
// is released by hand, and each call has its own autorelease pool, because
// it runs on crl::async workers that don't drain one for us.

namespace Oblivion::Vision {
namespace {

using Platform::NS2QString;

constexpr auto kLumaSampleSide = 64;

[[nodiscard]] QString ErrorText(NSError *error, const QString &fallback) {
	if (!error) {
		return fallback;
	}
	return u"%1 (%2, code %3)"_q.arg(
		NS2QString(error.localizedDescription),
		NS2QString(error.domain),
		QString::number(qlonglong(error.code)));
}

// Text in a picture with transparency (a sticker, a PNG logo) would be
// analyzed over black: put it over the background that contrasts with
// the visible pixels.
[[nodiscard]] QImage Flattened(const QImage &image) {
	if (!image.hasAlphaChannel()) {
		return image.convertToFormat(QImage::Format_RGB32);
	}
	const auto sample = image.scaled(
		QSize(kLumaSampleSide, kLumaSampleSide),
		Qt::IgnoreAspectRatio,
		Qt::FastTransformation
	).convertToFormat(QImage::Format_ARGB32_Premultiplied);
	auto luma = int64(0);
	auto alpha = int64(0);
	auto transparent = false;
	for (auto y = 0; y != sample.height(); ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			sample.constScanLine(y));
		for (auto x = 0; x != sample.width(); ++x) {
			// Premultiplied, so this is already weighted by alpha.
			luma += (qRed(line[x]) * 299
				+ qGreen(line[x]) * 587
				+ qBlue(line[x]) * 114) / 1000;
			alpha += qAlpha(line[x]);
			transparent = transparent || (qAlpha(line[x]) != 255);
		}
	}
	if (!transparent) {
		return image.convertToFormat(QImage::Format_RGB32);
	}
	const auto bright = (alpha > 0) && (luma * 255 / alpha > 127);
	auto result = QImage(image.size(), QImage::Format_RGB32);
	if (result.isNull()) {
		return result;
	}
	result.fill(bright ? QColor(24, 24, 24) : QColor(255, 255, 255));
	auto p = QPainter(&result);
	auto copy = image;
	copy.setDevicePixelRatio(1.);
	p.drawImage(0, 0, copy);
	p.end();
	return result;
}

// Russian first: the order is the priority of the language correction.
// Returns nil (system defaults) if nothing of that is supported.
[[nodiscard]] NSArray<NSString*> *PreferredLanguages(
		VNRecognizeTextRequest *request) {
	NSArray<NSString*> *supported = nil;
	if (@available(macOS 12.0, *)) {
		NSError *error = nil;
		supported = [request supportedRecognitionLanguagesAndReturnError:
			&error];
	}
	if (!supported.count) {
		return nil;
	}
	NSMutableArray<NSString*> *result = [NSMutableArray array];
	for (NSString *wanted in @[ @"ru-RU", @"en-US", @"uk-UA" ]) {
		NSString *prefix = [[wanted substringToIndex:2] stringByAppendingString:
			@"-"];
		NSString *found = nil;
		for (NSString *language in supported) {
			if ([language caseInsensitiveCompare:wanted] == NSOrderedSame) {
				found = language;
				break;
			} else if (!found
				&& [language.lowercaseString hasPrefix:prefix]) {
				found = language;
			}
		}
		if (found && ![result containsObject:found]) {
			[result addObject:found];
		}
	}
	return result.count ? result : nil;
}

[[nodiscard]] MaskResult RemoveBackgroundChecked(const QImage &image)
	API_AVAILABLE(macos(14.0));

MaskResult RemoveBackgroundChecked(const QImage &image) {
	auto result = MaskResult();
	const auto source = [&] {
		auto copy = image.convertToFormat(
			QImage::Format_ARGB32_Premultiplied);
		copy.setDevicePixelRatio(1.);
		return copy;
	}();
	if (source.isNull()) {
		result.error = u"Could not convert the image."_q;
		return result;
	}
	const auto cgImage = source.toCGImage();
	if (!cgImage) {
		result.error = u"Could not create a CGImage."_q;
		return result;
	}
	VNGenerateForegroundInstanceMaskRequest *request
		= [[VNGenerateForegroundInstanceMaskRequest alloc] init];
	VNImageRequestHandler *handler = [[VNImageRequestHandler alloc]
		initWithCGImage:cgImage
		options:@{}];

	NSError *error = nil;
	const auto success = [handler performRequests:@[ request ] error:&error];
	VNInstanceMaskObservation *observation = success
		? request.results.firstObject
		: nil;
	if (!success || error) {
		result.error = ErrorText(error, u"The mask request failed."_q);
	} else if (!observation || !observation.allInstances.count) {
		result.nothingFound = true;
		result.error = u"No foreground found."_q;
	} else {
		NSError *maskError = nil;
		const auto mask = [observation
			generateScaledMaskForImageForInstances:observation.allInstances
			fromRequestHandler:handler
			error:&maskError];
		if (!mask) {
			result.error = ErrorText(
				maskError,
				u"Could not generate the mask."_q);
		} else {
			CVPixelBufferLockBaseAddress(mask, kCVPixelBufferLock_ReadOnly);
			const auto base = static_cast<const uchar*>(
				CVPixelBufferGetBaseAddress(mask));
			const auto columns = int(CVPixelBufferGetWidth(mask));
			const auto rows = int(CVPixelBufferGetHeight(mask));
			const auto stride = qsizetype(CVPixelBufferGetBytesPerRow(mask));
			const auto format = CVPixelBufferGetPixelFormatType(mask);
			if (!base || columns <= 0 || rows <= 0) {
				result.error = u"Empty mask buffer."_q;
			} else if (format == kCVPixelFormatType_OneComponent32Float) {
				result.cutout = ApplyMask(
					source,
					reinterpret_cast<const float*>(base),
					columns,
					rows,
					stride);
			} else if (format == kCVPixelFormatType_OneComponent8) {
				auto converted = std::vector<float>(
					size_t(columns) * size_t(rows));
				for (auto y = 0; y != rows; ++y) {
					const auto from = base + y * stride;
					const auto to = converted.data() + size_t(y) * columns;
					for (auto x = 0; x != columns; ++x) {
						to[x] = from[x] / 255.f;
					}
				}
				result.cutout = ApplyMask(
					source,
					converted.data(),
					columns,
					rows,
					qsizetype(columns) * qsizetype(sizeof(float)));
			} else {
				result.error = u"Unexpected mask format %1."_q.arg(
					QString::number(uint(format), 16));
			}
			CVPixelBufferUnlockBaseAddress(
				mask,
				kCVPixelBufferLock_ReadOnly);
			CVPixelBufferRelease(mask);

			if (!result.cutout.isNull()) {
				result.bounds = ContentBounds(result.cutout);
				if (result.bounds.isEmpty()) {
					result.cutout = QImage();
					result.nothingFound = true;
					result.error = u"The mask is empty."_q;
				} else {
					result.ok = true;
				}
			} else if (result.error.isEmpty()) {
				result.error = u"Could not apply the mask."_q;
			}
		}
	}
	[handler release];
	[request release];
	CGImageRelease(cgImage);
	return result;
}

} // namespace

bool TextRecognitionSupported() {
	// VNRecognizeTextRequest is macOS 10.15+, below the deployment target.
	return true;
}

TextResult RecognizeText(const QImage &image) {
	auto result = TextResult();
	if (image.isNull()) {
		result.error = u"Empty image."_q;
		return result;
	}
	@autoreleasepool {
		const auto source = Flattened(image);
		const auto cgImage = source.isNull() ? nullptr : source.toCGImage();
		if (!cgImage) {
			result.error = u"Could not create a CGImage."_q;
			return result;
		}
		const auto width = float64(source.width());
		const auto height = float64(source.height());

		VNRecognizeTextRequest *request
			= [[VNRecognizeTextRequest alloc] init];
		request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
		request.usesLanguageCorrection = YES;
		if (NSArray<NSString*> *languages = PreferredLanguages(request)) {
			request.recognitionLanguages = languages;
		}
		VNImageRequestHandler *handler = [[VNImageRequestHandler alloc]
			initWithCGImage:cgImage
			options:@{}];

		NSError *error = nil;
		const auto success = [handler
			performRequests:@[ request ]
			error:&error];
		if (!success || error) {
			result.error = ErrorText(error, u"The text request failed."_q);
		} else {
			auto fragments = std::vector<details::TextFragment>();
			for (VNRecognizedTextObservation *observation
				in request.results) {
				VNRecognizedText *best = [observation
					topCandidates:1].firstObject;
				if (!best) {
					continue;
				}
				// Normalized, the origin is the bottom left corner.
				const auto box = observation.boundingBox;
				fragments.push_back({
					.text = NS2QString(best.string),
					.box = QRectF(
						box.origin.x * width,
						(1. - box.origin.y - box.size.height) * height,
						box.size.width * width,
						box.size.height * height),
				});
			}
			result = details::ComposeText(std::move(fragments));
			result.ok = true;
		}
		[handler release];
		[request release];
		CGImageRelease(cgImage);
	}
	return result;
}

bool BackgroundRemovalSupported() {
	if (@available(macOS 14.0, *)) {
		return true;
	}
	return false;
}

MaskResult RemoveBackground(const QImage &image) {
	auto result = MaskResult();
	if (image.isNull()) {
		result.error = u"Empty image."_q;
		return result;
	}
	if (@available(macOS 14.0, *)) {
		@autoreleasepool {
			result = RemoveBackgroundChecked(image);
		}
	} else {
		result.error = u"Background removal needs macOS 14."_q;
	}
	return result;
}

} // namespace Oblivion::Vision
