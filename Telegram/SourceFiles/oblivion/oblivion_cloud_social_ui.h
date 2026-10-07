/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Round 5: social, the UI part: the activity chips and the «Oblivion»
// block of a profile, the editor «Мой профиль Oblivion», the boxes
// «Друзья в Oblivion» and «Выбранные люди», the switches of what is
// public about the account. Everything other modules call is declared
// in oblivion_cloud_social.h and implemented in
// oblivion_cloud_social_ui.cpp; the widgets and the boxes themselves take
// plain data and callbacks (no session), so the snapshot scenes at the
// bottom of that file show them as they are.
//
// The looks («Тема Oblivion», oblivion_look.h). With the plain look
// everything here is painted and laid out the way it always was. With a
// look on the chips, the block of a profile and the rows of the lists
// take its shapes (see ChipsView and ProfileCard): they are laid out
// again on Oblivion::Look::Updates(), and the scenes social_chips,
// social_profile_block, social_profile_block_no_accent, social_friends
// and social_editor are rendered once more for every look, as
// <scene>_look1, _look2 and _look3.
namespace Oblivion::Social {

} // namespace Oblivion::Social
