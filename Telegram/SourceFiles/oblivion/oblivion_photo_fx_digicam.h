/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_fx.h"

// Photo editor: the "digicam" pack, the looks of digicamfx.com (cheap
// cameras and phones of the 2000s, the photo filters of the 2010s) written
// for the registry of oblivion_photo_fx.h. The effects live in the
// FxGroup::Lofi group next to the "bad camera" pack and are registered
// from the .cpp through an FxRegistrar, the ids are stable (stored in
// documents):
//
//   digicam.jpeg        a small JPEG photo: the resolution, the quality,
//                       the noise (added or overlaid, colored or not),
//                       the 3 x 3 sharpening, the "pixels x2" enlargement;
//   digicam.ccd         a CCD compact: a soft lens, the CCD colors, a tinted
//                       bloom of the highlights, a violet lens reflection,
//                       sharpening, noise, JPEG, pixels x2;
//   digicam.nokia       a phone of 2005: its colors, green corners,
//                       sharpening, noise in the shadows, JPEG;
//   digicam.webcam      a 1/4" sensor of a laptop camera: its colors, a blue
//                       bloom, noise, JPEG and the noise reduction over it;
//   digicam.iphone      a phone of 2009: two wide blooms, its colors, one
//                       more bloom, a coarse colored grain;
//   digicam.soap        the soft "washed" look: a bloom, smoothing of the
//                       even areas, a warm center and greenish edges, noise
//                       in the shadows, JPEG;
//   digicam.look        the color filters (twenty of them) with their
//                       vignette and fine grain;
//   digicam.glow        the tinted bloom alone;
//   digicam.reflection  the lens reflection alone (ten variants);
//   digicam.vignette    the color vignette alone (a tint of the center and
//                       a tint of the edges).
//
// The presets (FxPreset, they only set parameters): digicam.preset_jpeg_low,
// digicam.preset_jpeg_medium, digicam.preset_jpeg_high.
//
// The cameras work the way the old digital camera of the "bad camera" pack
// does: the photo is taken down to the resolution of the sensor (in the
// preview too), everything happens there and the small picture is brought
// back to the size of the layer, so the preview is the export, only
// smaller. The effects that work at the size of the layer measure their
// lengths in source pixels (FxContext::scale) and keep their grain on the
// pixel grid of a 1080 pixel picture. Random parts depend only on the Seed
// parameter and FxContext::seed.
//
// The filters are formulas (a tone curve for every channel, a mix of the
// channels with their maximum and minimum, one more curve), not tables of
// colors: DigicamLookColor() is the whole of it.
namespace Oblivion::Photo {

inline constexpr auto kDigicamLookCount = 20;
inline constexpr auto kDigicamReflectionCount = 10;

// The color of a filter (the index of the "look" parameter of
// digicam.look, 0 .. kDigicamLookCount - 1) for a straight color, without
// the vignette and the grain. intensity is 0 .. 1. Any thread.
[[nodiscard]] FxRgba DigicamLookColor(int look, FxRgba color, float intensity);

} // namespace Oblivion::Photo
