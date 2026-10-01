/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_app_icon.h"

#include "base/platform/mac/base_utilities_mac.h"

#import <AppKit/AppKit.h>

namespace Oblivion::internal {
namespace {

// NSImage understands everything the system does: ICNS, HEIC, TIFF...
[[nodiscard]] QImage Render(NSImage *image, int maxSide) {
	auto width = 0;
	auto height = 0;
	for (NSImageRep *rep in [image representations]) {
		if (int([rep pixelsWide]) > width) {
			width = int([rep pixelsWide]);
			height = int([rep pixelsHigh]);
		}
	}
	if (width <= 0 || height <= 0) {
		// Vector representations report no pixel size.
		width = int([image size].width);
		height = int([image size].height);
	}
	if (width <= 0 || height <= 0) {
		return QImage();
	}
	if (maxSide > 0 && std::max(width, height) > maxSide) {
		const auto scale = maxSide / double(std::max(width, height));
		width = std::max(int(width * scale), 1);
		height = std::max(int(height * scale), 1);
	}
	auto result = QImage(
		width,
		height,
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);

	const auto space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	const auto info = CGBitmapInfo(kCGImageAlphaPremultipliedFirst)
		| kCGBitmapByteOrder32Host;
	const auto context = CGBitmapContextCreate(
		result.bits(),
		width,
		height,
		8,
		result.bytesPerLine(),
		space,
		info);
	CGColorSpaceRelease(space);
	if (!context) {
		return QImage();
	}
	CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
	NSGraphicsContext *graphics = [NSGraphicsContext
		graphicsContextWithCGContext:context
		flipped:NO];
	[NSGraphicsContext saveGraphicsState];
	[NSGraphicsContext setCurrentContext:graphics];
	[image
		drawInRect:NSMakeRect(0, 0, width, height)
		fromRect:NSZeroRect
		operation:NSCompositingOperationCopy
		fraction:1.];
	[NSGraphicsContext restoreGraphicsState];
	CGContextRelease(context);
	return result;
}

} // namespace

QImage ReadIconImage(const QString &path, int maxSide) {
	@autoreleasepool {

	NSImage *image = [[NSImage alloc]
		initWithContentsOfFile:Platform::Q2NSString(path)];
	if (!image) {
		return QImage(path);
	}
	auto result = Render(image, maxSide);
	[image release];
	return result;

	}
}

QImage BundleIconImage(int size) {
	@autoreleasepool {

	NSString *path = [[[NSBundle mainBundle] resourcePath]
		stringByAppendingPathComponent:@"Icon.icns"];
	NSImage *image = [[NSImage alloc] initWithContentsOfFile:path];
	if (!image) {
		return QImage();
	}
	auto result = Render(image, size);
	[image release];
	return result;

	}
}

} // namespace Oblivion::internal
