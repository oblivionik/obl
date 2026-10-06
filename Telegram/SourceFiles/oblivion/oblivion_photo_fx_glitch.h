/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Photo editor: the glitch pack, the effects of the FxGroup::Glitch group
// of oblivion_photo_fx.h. Everything is registered from the .cpp through
// an FxRegistrar, the ids are stable (stored in documents):
//
//   glitch.rgb     per-channel RGB shift (x, y for every channel);
//   glitch.slices  slice displacement (count, amplitude);
//   glitch.blocks  block corruption (moved, recolored, inverted, noisy
//                  and stretched blocks on a grid);
//   glitch.sort    pixel sorting (thresholds, direction, key);
//   glitch.smear   pixel stretching;
//   glitch.mosh    the datamosh look: blocks drag the picture along;
//   glitch.jitter  scanline jitter;
//   glitch.wave    wave (sine, triangle, steps, noise);
//   glitch.bands   noise bands (static, color static, dropouts);
//   glitch.crush   bit crush (truncate, rotate, XOR);
//   glitch.bend    databend: what a JPEG file with damaged bytes looks
//                  like (the rest of the picture shifted by blocks, the
//                  colors drifting, garbage blocks at the breaks).
//
// The presets: glitch.preset_signal, glitch.preset_file,
// glitch.preset_mosh, glitch.preset_vapor.
//
// Every effect has a Seed parameter (the "randomize" button of the
// panel): the picture depends only on it and on FxContext::seed, never on
// the time or the thread. Everything random is placed in the normalized
// coordinates of the layer and lengths follow FxContext::scale, so the
// preview is the export, only smaller.
namespace Oblivion::Photo {

} // namespace Oblivion::Photo
