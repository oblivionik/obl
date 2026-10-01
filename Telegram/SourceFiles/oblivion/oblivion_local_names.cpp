/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_local_names.h"

#include "boxes/peers/edit_peer_common.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "oblivion/oblivion_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace Oblivion {
namespace {

struct State {
	base::flat_map<not_null<const PeerData*>, QString> realNames;
	rpl::event_stream<not_null<PeerData*>> originalChanges;
};

[[nodiscard]] State &GetState() {
	static auto result = State();
	return result;
}

[[nodiscard]] bool IsExcluded(not_null<const PeerData*> peer) {
	return peer->isSelf()
		|| peer->isRepliesChat()
		|| peer->isVerifyCodes()
		|| peer->isSavedHiddenAuthor();
}

[[nodiscard]] PeerData *ResolveTarget(PeerData *peer) {
	if (!peer) {
		return nullptr;
	} else if (const auto to = peer->migrateTo()) {
		peer = to;
	}
	if (const auto broadcast = peer->monoforumBroadcast()) {
		peer = broadcast;
	}
	return IsExcluded(peer) ? nullptr : peer;
}

void Reapply(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		user->setName(
			user->firstName,
			user->lastName,
			user->nameOrPhone,
			user->editableUsername());
	} else if (const auto chat = peer->asChat()) {
		chat->setName(RealName(chat));
	} else if (const auto channel = peer->asChannel()) {
		channel->setName(RealName(channel), channel->editableUsername());
	}
}

void LocalNameBox(not_null<Ui::GenericBox*> box, not_null<PeerData*> peer) {
	box->setTitle(tr::lng_oblivion_local_name_title());

	const auto real = RealName(peer);
	const auto local = Get().localName(peer->id.value);

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_oblivion_local_name_about(),
			st::boxLabel),
		st::boxRowPadding + style::margins(0, 0, 0, st::boxLittleSkip));

	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(real),
			local.isEmpty() ? real : local));
	field->setMaxLength(Ui::EditPeer::kMaxGroupChannelTitle);
	box->setFocusCallback([=] {
		field->setFocusFast();
		field->selectAll();
	});

	const auto submit = [=] {
		SetLocalName(peer, field->getLastText());
		box->closeBox();
	};
	field->submits() | rpl::on_next(submit, box->lifetime());

	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

QString ApplyLocalName(not_null<PeerData*> peer, const QString &realName) {
	auto &state = GetState();

	// Local names are keyed by peer id across all accounts, so a name set
	// for a user in one account must not rename the same user where it is
	// the account's own self (or another excluded peer).
	const auto local = IsExcluded(peer)
		? QString()
		: Get().localName(peer->id.value);
	const auto i = state.realNames.find(peer);
	if (local.isEmpty()) {
		if (i != end(state.realNames)) {
			state.realNames.erase(i);
			state.originalChanges.fire_copy(peer);
		}
		return realName;
	} else if (i == end(state.realNames)) {
		state.realNames.emplace(peer, realName);
		state.originalChanges.fire_copy(peer);
	} else if (i->second != realName) {
		i->second = realName;
		state.originalChanges.fire_copy(peer);
	}
	return local;
}

void ForgetPeer(not_null<const PeerData*> peer) {
	GetState().realNames.remove(peer);
}

bool HasLocalName(not_null<const PeerData*> peer) {
	return GetState().realNames.contains(peer);
}

QString OriginalName(not_null<const PeerData*> peer) {
	const auto &realNames = GetState().realNames;
	const auto i = realNames.find(peer);
	return (i != end(realNames)) ? i->second : QString();
}

QString RealName(not_null<PeerData*> peer) {
	if (const auto to = peer->migrateTo()) {
		return RealName(to);
	} else if (const auto broadcast = peer->monoforumBroadcast()) {
		return RealName(broadcast);
	}
	const auto &realNames = GetState().realNames;
	const auto i = realNames.find(peer);
	return (i != end(realNames)) ? i->second : peer->name();
}

QString RealShortName(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return user->firstName.isEmpty() ? user->lastName : user->firstName;
	}
	return RealName(peer);
}

rpl::producer<QString> OriginalNameValue(not_null<PeerData*> peer) {
	return rpl::merge(
		peer->session().changes().peerFlagsValue(
			peer,
			Data::PeerUpdate::Flag::Name
		) | rpl::to_empty,
		GetState().originalChanges.events(
		) | rpl::filter([=](not_null<PeerData*> changed) {
			return (changed == peer);
		}) | rpl::to_empty
	) | rpl::map([=] {
		return OriginalName(peer);
	}) | rpl::distinct_until_changed();
}

void SetLocalName(not_null<PeerData*> peer, const QString &name) {
	const auto target = ResolveTarget(peer);
	if (!target) {
		return;
	}
	const auto trimmed = TextUtilities::SingleLine(name).trimmed();
	const auto value = (trimmed == RealName(target)) ? QString() : trimmed;
	Get().setLocalName(target->id.value, value);
	ReapplyLocalName(target->id.value);
}

void ReapplyLocalName(uint64 peerId) {
	if (!peerId) {
		return;
	}
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (!account->sessionExists()) {
			continue;
		}
		const auto &session = account->session();
		if (const auto peer = session.data().peerLoaded(PeerId(peerId))) {
			Reapply(peer);
		}
	}
}

void ShowLocalNameBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	if (const auto target = ResolveTarget(peer)) {
		controller->show(Box(LocalNameBox, not_null<PeerData*>(target)));
	}
}

void AddLocalNameActions(
		not_null<Window::SessionController*> controller,
		const Dialogs::Key &key,
		const Ui::Menu::MenuCallback &addAction) {
	const auto sublist = key.sublist();
	const auto target = key.topic()
		? nullptr
		: ResolveTarget(sublist
			? sublist->sublistPeer().get()
			: key.peer());
	if (!target) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto has = !Get().localName(target->id.value).isEmpty();
	addAction(
		(has
			? tr::lng_oblivion_local_name_edit
			: tr::lng_oblivion_local_name_set)(tr::now),
		[=] {
			if (const auto strong = weak.get()) {
				ShowLocalNameBox(strong, target);
			}
		},
		&st::menuIconTagRename);
	if (has) {
		addAction(
			tr::lng_oblivion_local_name_reset(tr::now),
			[=] { SetLocalName(target, QString()); },
			&st::menuIconTagRemove);
	}
}

} // namespace Oblivion
