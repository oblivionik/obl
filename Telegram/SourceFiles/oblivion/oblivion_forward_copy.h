/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_types.h"

class HistoryItem;
class PeerData;

namespace Api {
struct SendAction;
struct SendOptions;
} // namespace Api

namespace Data {
class Thread;
} // namespace Data

namespace Oblivion {

// Real forwarding restrictions, ignoring the "allow copying" override
// that PeerData::allowsForwarding() / HistoryItem::forbidsForward() apply.
[[nodiscard]] bool PeerAllowsForwardingReal(not_null<const PeerData*> peer);
[[nodiscard]] bool ItemForwardProtected(not_null<const HistoryItem*> item);

// Whether the "Forward" action may be offered for the item: protected
// messages are offered only while forwarding them as a copy is enabled,
// messages kept after deletion (see IsKeptDeleted) are never offered.
[[nodiscard]] bool ItemForwardAllowed(not_null<const HistoryItem*> item);

// Hook for ApiWrap::forwardMessages(). If the draft contains protected
// messages and forwarding as a copy is enabled, takes the draft and the
// callback over (non-protected messages are still forwarded normally, in
// order) and returns true. Otherwise leaves everything untouched.
[[nodiscard]] bool ForwardAsCopyIfProtected(
	Data::ResolvedForwardDraft &draft,
	const Api::SendAction &action,
	FnMut<void()> &successCallback);

// Hook for the share box forward callback. Sends the comment and copies
// of the messages to every chosen thread, returns false if nothing needs
// to be copied (the regular forwarding should be used then).
[[nodiscard]] bool ShareAsCopyIfProtected(
	const HistoryItemsList &items,
	const std::vector<not_null<Data::Thread*>> &threads,
	const TextWithTags &comment,
	const Api::SendOptions &options,
	Data::ForwardOptions forwardOptions);

} // namespace Oblivion
