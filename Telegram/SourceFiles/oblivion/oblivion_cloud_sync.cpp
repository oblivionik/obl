/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud_sync.h"

#include "base/timer.h"
#include "base/unique_qptr.h"
#include "base/unixtime.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_auth_key.h"
#include "oblivion/oblivion_cloud.h"
#include "oblivion/oblivion_cloud_share_ui.h"
#include "oblivion/oblivion_cloud_update.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings/settings_common.h"
#include "storage/storage_account.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "settings.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSysInfo>

namespace Oblivion::Sync {
namespace {

constexpr auto kBlobVersion = 1;
constexpr auto kKdfPbkdf2Sha256 = 1;
// What this build seals with.
constexpr auto kIterations = 600'000;
// The strength of the key is decided here and never by the bytes of the
// server: no build of Oblivion has sealed a copy with fewer rounds, so a
// header that claims fewer was not written by the app. Such a copy is
// not opened, its header is not reused and not remembered.
constexpr auto kMinIterations = kIterations;
constexpr auto kMaxIterations = 5'000'000;
// Only for the self-test, where the real count would take seconds.
constexpr auto kTestIterations = 1000;
constexpr auto kSaltSize = 16;
constexpr auto kHeaderSize = 4 + 1 + 1 + 4 + kSaltSize;
constexpr auto kSealOverhead = 12 + 16;
constexpr auto kMaxBlob = 1048576;
constexpr auto kMaxPlain = 16 * 1024 * 1024;
constexpr auto kPayloadVersion = 1;
constexpr auto kMinPassword = 6;
constexpr auto kMaxPassword = 256;
constexpr auto kMaxDeviceName = 64;
constexpr auto kSendDelay = crl::time(60'000);
constexpr auto kStartDelay = crl::time(8'000);
constexpr auto kRecheckDelay = crl::time(3'000);
constexpr auto kMaxRefused = 2;

// What belongs to this device or to an account of Telegram and is never
// put into the copy: the icon of the app, what depends on a model or on
// a microphone of this computer, the volumes, the state of the bio badge
// and of the ghost button, the status polling (it talks to Telegram, so
// it is switched on by hand on every device). Everything that starts
// with "cloud_" is left out as well: the connection to the cloud and what
// is published there is decided on every device, and the public part of
// it lives on the server anyway.
constexpr auto kDeviceKeys = std::array{
	"app_icon",
	"app_icon_finder",
	"app_icon_digest",
	"local_transcribe",
	"transcribe_language",
	"voice_noise_suppression",
	"room_music_volume",
	"room_video_volume",
	"badge_enabled",
	"badge_asked",
	"ghost_button_owned",
	"online_polling",
};
constexpr auto kDevicePrefix = "cloud_";

[[nodiscard]] QByteArray Magic() {
	return QByteArray("OBSY");
}

[[nodiscard]] bool DeviceKey(const QString &key) {
	if (key.startsWith(QLatin1String(kDevicePrefix))) {
		return true;
	}
	for (const auto name : kDeviceKeys) {
		if (key == QLatin1String(name)) {
			return true;
		}
	}
	return false;
}

// The part of oblivion.json that is sent.
[[nodiscard]] QJsonObject PortableSettings(const QJsonObject &all) {
	auto result = QJsonObject();
	for (auto i = all.begin(); i != all.end(); ++i) {
		if (!DeviceKey(i.key())) {
			result.insert(i.key(), i.value());
		}
	}
	return result;
}

// oblivion.json of this device with what came from another one: only
// the settings this build knows and only values of the same kind, the
// device part stays as it is.
[[nodiscard]] QJsonObject MergeSettings(
		const QJsonObject &current,
		const QJsonObject &received) {
	auto result = current;
	for (auto i = received.begin(); i != received.end(); ++i) {
		const auto key = i.key();
		const auto was = current.constFind(key);
		if (DeviceKey(key)
			|| was == current.constEnd()
			|| was.value().type() != i.value().type()) {
			continue;
		}
		result.insert(key, i.value());
	}
	return result;
}

[[nodiscard]] int CountDifferences(
		const QJsonObject &was,
		const QJsonObject &now) {
	auto result = 0;
	for (auto i = now.begin(); i != now.end(); ++i) {
		const auto old = was.constFind(i.key());
		if (old == was.constEnd() || old.value() != i.value()) {
			++result;
		}
	}
	return result;
}

struct BlobHeader {
	int iterations = 0;
	QByteArray salt;
};

[[nodiscard]] QByteArray SerializeHeader(const BlobHeader &header) {
	auto result = Magic();
	result.push_back(char(kBlobVersion));
	result.push_back(char(kKdfPbkdf2Sha256));
	const auto value = uint32(header.iterations);
	result.push_back(char((value >> 24) & 0xFF));
	result.push_back(char((value >> 16) & 0xFF));
	result.push_back(char((value >> 8) & 0xFF));
	result.push_back(char(value & 0xFF));
	result.append(header.salt);
	return result;
}

// The header of a copy, if its rounds are inside minIterations ..
// kMaxIterations. The minimum is kMinIterations everywhere a key is made
// from a password; a lower one is given only where the key is there
// already (OpenBlob: the seal itself proves who wrote the header) and by
// the self-test.
[[nodiscard]] std::optional<BlobHeader> ParseHeader(
		const QByteArray &blob,
		int minIterations = kMinIterations) {
	if (blob.size() < kHeaderSize + kSealOverhead
		|| blob.size() > kMaxBlob
		|| !blob.startsWith(Magic())
		|| uchar(blob[4]) != kBlobVersion
		|| uchar(blob[5]) != kKdfPbkdf2Sha256) {
		return std::nullopt;
	}
	const auto iterations = (uint32(uchar(blob[6])) << 24)
		| (uint32(uchar(blob[7])) << 16)
		| (uint32(uchar(blob[8])) << 8)
		| uint32(uchar(blob[9]));
	// A copy somebody has planted must neither make the key weak nor make
	// the app count for hours.
	if (iterations < uint32(std::max(minIterations, 1))
		|| iterations > uint32(kMaxIterations)) {
		return std::nullopt;
	}
	return BlobHeader{
		.iterations = int(iterations),
		.salt = blob.mid(10, kSaltSize),
	};
}

// What a new copy is sealed with. The salt of the copy of the server is
// kept, so that the devices with the same password go on with the same
// key, but only together with rounds that are not below the minimum of
// this build. Without such a copy (none at all, or a weak or unreadable
// header) it is the salt this device has used before, so that a copy
// made anew after «Удалить копию с сервера» still fits the other
// devices, or a fresh salt with the full count.
[[nodiscard]] BlobHeader HeaderForSend(
		const QByteArray &remoteBlob,
		const BlobHeader &used = BlobHeader()) {
	if (const auto parsed = ParseHeader(remoteBlob)) {
		return *parsed;
	} else if (used.salt.size() == kSaltSize
		&& used.iterations >= kMinIterations
		&& used.iterations <= kMaxIterations) {
		return used;
	}
	return BlobHeader{
		.iterations = kIterations,
		.salt = Cloud::RandomBytes(kSaltSize),
	};
}

// Slow on purpose: for crl::async.
[[nodiscard]] QByteArray DeriveKey(
		const QString &password,
		const BlobHeader &header) {
	return Cloud::SlowKey(
		password.toUtf8(),
		header.salt,
		header.iterations);
}

[[nodiscard]] QByteArray SealBlob(
		const QByteArray &key,
		const BlobHeader &header,
		const QByteArray &plain) {
	if (key.size() != 32 || header.salt.size() != kSaltSize) {
		return QByteArray();
	}
	const auto head = SerializeHeader(header);
	const auto sealed = Cloud::AesGcmSeal(key, qCompress(plain, 9), head);
	return sealed.isEmpty() ? QByteArray() : (head + sealed);
}

// Nothing for a wrong key and for a copy that was changed on the way.
[[nodiscard]] std::optional<QByteArray> OpenBlob(
		const QByteArray &key,
		const QByteArray &blob) {
	if (key.size() != 32 || !ParseHeader(blob, 1)) {
		return std::nullopt;
	}
	const auto packed = Cloud::AesGcmOpen(
		key,
		blob.mid(kHeaderSize),
		blob.left(kHeaderSize));
	if (!packed || packed->size() < 4) {
		return std::nullopt;
	}
	const auto size = (uint32(uchar((*packed)[0])) << 24)
		| (uint32(uchar((*packed)[1])) << 16)
		| (uint32(uchar((*packed)[2])) << 8)
		| uint32(uchar((*packed)[3]));
	if (size > uint32(kMaxPlain)) {
		return std::nullopt;
	}
	auto plain = qUncompress(*packed);
	if (plain.isEmpty() && size) {
		return std::nullopt;
	}
	return plain;
}

struct Payload {
	bool valid = false;
	QJsonObject settings;
	QJsonArray presets;
	// The presets that were deleted, { id, time }: a preset that is gone
	// on one device goes on the others too instead of coming back from
	// them. A build that does not know the field ignores it.
	QJsonArray removed;
	QString device;
	int64 savedAt = 0; // Unixtime.
	int appBuild = 0;
};

[[nodiscard]] QByteArray SerializePayload(const Payload &payload) {
	auto object = QJsonObject();
	object.insert(u"v"_q, kPayloadVersion);
	object.insert(u"app_build"_q, payload.appBuild);
	object.insert(u"device"_q, payload.device.left(kMaxDeviceName));
	object.insert(u"saved_at"_q, double(payload.savedAt));
	object.insert(u"settings"_q, payload.settings);
	object.insert(u"presets"_q, payload.presets);
	if (!payload.removed.isEmpty()) {
		object.insert(u"presets_removed"_q, payload.removed);
	}
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] Payload ParsePayload(const QByteArray &plain) {
	auto result = Payload();
	const auto document = QJsonDocument::fromJson(plain);
	if (!document.isObject()) {
		return result;
	}
	const auto object = document.object();
	const auto settings = object.value(u"settings"_q);
	if (object.value(u"v"_q).toInt() != kPayloadVersion
		|| !settings.isObject()) {
		return result;
	}
	result.valid = true;
	result.settings = settings.toObject();
	result.presets = object.value(u"presets"_q).toArray();
	result.removed = object.value(u"presets_removed"_q).toArray();
	result.device = Cloud::JsonText(
		object.value(u"device"_q),
		kMaxDeviceName);
	result.savedAt = std::max(
		Cloud::JsonInt(object.value(u"saved_at"_q)),
		int64(0));
	result.appBuild = int(std::clamp(
		Cloud::JsonInt(object.value(u"app_build"_q)),
		int64(0),
		int64(std::numeric_limits<int>::max())));
	return result;
}

// What tells whether this device has something that was not sent yet.
[[nodiscard]] QByteArray ContentHash(
		const QJsonObject &settings,
		const QJsonArray &presets) {
	auto object = QJsonObject();
	object.insert(u"settings"_q, settings);
	object.insert(u"presets"_q, presets);
	return Cloud::Sha256(
		QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// The same presets, in whatever order two devices keep them. The time of
// saving is not compared: a merge does not replace a preset for it.
[[nodiscard]] bool SamePresets(const QJsonArray &a, const QJsonArray &b) {
	if (a.size() != b.size()) {
		return false;
	}
	const auto clean = [](const QJsonValue &value) {
		auto object = value.toObject();
		object.remove(u"time"_q);
		return object;
	};
	auto known = QHash<QString, QJsonObject>();
	for (const auto &value : a) {
		const auto object = clean(value);
		known.insert(object.value(u"id"_q).toString(), object);
	}
	if (known.size() != a.size()) {
		// Something that is not a list of presets with ids.
		return (a == b);
	}
	for (const auto &value : b) {
		const auto object = clean(value);
		const auto i = known.find(object.value(u"id"_q).toString());
		if (i == known.end() || i.value() != object) {
			return false;
		}
		known.erase(i);
	}
	return true;
}

// The automatic mode has found a copy on the server that is not the one
// this device has seen last. What is done about it:
enum class Incoming {
	Apply, // This device has nothing unsent: the copy is taken.
	Same, // The copy brings nothing new and nothing here waits to be sent.
	Ahead, // It brings nothing new, this device has more: sent over it.
	Conflict, // Both have something the other has not: the user decides.
};

// unsent: this device has changed since its last send or receive.
// brings: taking the copy would change something on this device.
// same: this device would send exactly what the copy has.
//
// A copy that brings nothing is never a conflict: that is the case of
// two accounts of one installation (they share the settings, one of them
// has just applied the same change). And something is sent over a copy
// only by a device that has changes of its own, so two devices that see
// the settings differently (other builds) can't send in turns for ever.
[[nodiscard]] Incoming JudgeIncoming(bool unsent, bool brings, bool same) {
	return brings
		? (unsent ? Incoming::Conflict : Incoming::Apply)
		: (unsent && !same)
		? Incoming::Ahead
		: Incoming::Same;
}

// The key of the sync on this device, between the launches.
struct Remembered {
	BlobHeader header;
	QByteArray key;
	int64 rev = 0;
	QByteArray hash;
	int64 time = 0; // Unixtime of the last send or receive.

	[[nodiscard]] bool valid() const {
		return (key.size() == 32) && (header.salt.size() == kSaltSize);
	}
};

[[nodiscard]] QByteArray SerializeRemembered(
		const Remembered &value,
		const QByteArray &sealKey) {
	if (!value.valid() || sealKey.size() != 32) {
		return QByteArray();
	}
	const auto sealed = Cloud::AesGcmSeal(
		sealKey,
		value.key,
		QByteArray("oblivion sync key"));
	if (sealed.isEmpty()) {
		return QByteArray();
	}
	auto object = QJsonObject();
	object.insert(u"v"_q, 1);
	object.insert(
		u"salt"_q,
		QString::fromLatin1(value.header.salt.toBase64()));
	object.insert(u"iterations"_q, value.header.iterations);
	object.insert(u"key"_q, QString::fromLatin1(sealed.toBase64()));
	object.insert(u"rev"_q, double(value.rev));
	object.insert(u"hash"_q, QString::fromLatin1(value.hash.toHex()));
	object.insert(u"time"_q, double(value.time));
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] Remembered ParseRemembered(
		const QByteArray &bytes,
		const QByteArray &sealKey) {
	auto result = Remembered();
	const auto object = QJsonDocument::fromJson(bytes).object();
	if (object.value(u"v"_q).toInt() != 1 || sealKey.size() != 32) {
		return result;
	}
	const auto key = Cloud::AesGcmOpen(
		sealKey,
		QByteArray::fromBase64(object.value(u"key"_q).toString().toLatin1()),
		QByteArray("oblivion sync key"));
	const auto iterations = object.value(u"iterations"_q).toInt();
	if (!key
		|| key->size() != 32
		|| iterations < kMinIterations
		|| iterations > kMaxIterations) {
		return result;
	}
	result.key = *key;
	result.header.iterations = iterations;
	result.header.salt = QByteArray::fromBase64(
		object.value(u"salt"_q).toString().toLatin1());
	result.rev = std::max(Cloud::JsonInt(object.value(u"rev"_q)), int64(0));
	result.hash = QByteArray::fromHex(
		object.value(u"hash"_q).toString().toLatin1());
	result.time = std::max(Cloud::JsonInt(object.value(u"time"_q)), int64(0));
	if (!result.valid()) {
		result = Remembered();
	}
	return result;
}

[[nodiscard]] QString AccountFolder(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/oblivion/"_q
		+ (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare)
		+ '/';
}

// As hard to read as the Telegram session itself.
[[nodiscard]] QByteArray SealKeyFor(not_null<Main::Session*> session) {
	const auto key = session->local().peekLegacyLocalKey();
	if (!key) {
		return QByteArray();
	}
	const auto data = key->data();
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	hash.addData(QByteArray("oblivion cloud sync key"));
	hash.addData(QByteArray::fromRawData(
		reinterpret_cast<const char*>(data.data()),
		qsizetype(data.size())));
	return hash.result();
}

[[nodiscard]] QString DeviceName() {
	const auto host = QSysInfo::machineHostName().simplified();
	return (host.isEmpty() ? QSysInfo::prettyProductName() : host).left(
		kMaxDeviceName);
}

[[nodiscard]] QString WhenText(int64 unixtime) {
	return unixtime
		? langDateTime(QDateTime::fromSecsSinceEpoch(unixtime))
		: QString();
}

// ---- The box.

struct BoxStatus {
	bool loading = false;
	QString error;
	bool exists = false;
	int64 updated = 0; // Unixtime of the copy of the server.
	int64 size = 0;
	bool remembered = false;
	bool conflict = false;
	bool automatic = false;
	bool busy = false;
	int64 synced = 0; // Unixtime of the last send / receive here.
};

struct SyncBoxArgs {
	rpl::producer<BoxStatus> status;
	// An empty password: the one that is remembered on this device.
	Fn<void(const QString &password)> send;
	Fn<void(const QString &password)> receive;
	rpl::producer<QString> errors; // About the password, under the field.
	Fn<void()> forget;
	Fn<void()> erase;
};

void SyncBox(not_null<Ui::GenericBox*> box, SyncBoxArgs &&args) {
	struct State {
		BoxStatus status;
		rpl::variable<QString> title;
		rpl::variable<QString> about;
		rpl::variable<QString> placeholder;
		rpl::variable<QString> hint;
		base::unique_qptr<Ui::PopupMenu> menu;
		bool hintIsError = false;
		int buttons = -1;
		Fn<void()> rebuildButtons;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto send = std::move(args.send);
	const auto receive = std::move(args.receive);
	const auto forget = std::move(args.forget);
	const auto erase = std::move(args.erase);

	box->setTitle(tr::lng_oblivion_sync_title());
	box->setWidth(st::boxWideWidth);

	// What is on the server and what to do comes first, the long words
	// about the encryption are the small print at the bottom: a wall of
	// text above the only field pushed everything else out of sight.
	const auto content = box->verticalLayout();
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			state->title.value() | rpl::map(tr::bold),
			st::boxLabel),
		st::boxRowPadding);
	const auto about = content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			state->about.value(),
			st::boxDividerLabel),
		st::boxRowPadding + style::margins(
			0,
			st::boxLittleSkip / 2,
			0,
			st::boxLittleSkip / 2));

	state->placeholder = tr::lng_oblivion_sync_password(tr::now);
	// The password field is not an RpWidget: it lives in a holder.
	const auto holder = content->add(
		object_ptr<Ui::FixedHeightWidget>(
			content,
			st::defaultInputField.heightMin),
		st::boxRowPadding);
	const auto field = Ui::CreateChild<Ui::PasswordInput>(
		holder,
		st::defaultInputField,
		state->placeholder.value());
	field->setMaxLength(kMaxPassword);
	holder->widthValue() | rpl::on_next([=](int width) {
		field->resize(width, field->height());
		field->moveToLeft(0, 0, width);
	}, holder->lifetime());
	const auto hint = content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			state->hint.value(),
			st::boxDividerLabel),
		st::boxRowPadding + style::margins(
			0,
			st::boxLittleSkip,
			0,
			st::boxLittleSkip));
	const auto showHint = [=](const QString &error) {
		state->hintIsError = !error.isEmpty();
		if (error.isEmpty()) {
			hint->setTextColorOverride(std::nullopt);
			state->hint = state->status.remembered
				? tr::lng_oblivion_sync_password_kept(tr::now)
				: tr::lng_oblivion_sync_password_hint(tr::now);
		} else {
			hint->setTextColorOverride(st::boxTextFgError->c);
			state->hint = error;
		}
	};
	QObject::connect(field, &Ui::MaskedInputField::changed, box, [=] {
		showHint(QString());
	});

	const auto password = [=]() -> std::optional<QString> {
		const auto text = field->getLastText();
		if (text.isEmpty() && state->status.remembered) {
			return QString();
		} else if (text.size() < kMinPassword) {
			showHint(tr::lng_oblivion_sync_password_short(tr::now));
			field->showError();
			return std::nullopt;
		}
		return text;
	};
	// An arrow out of the tray and an arrow into it: a pair that reads as
	// "to the server" and "from the server".
	const auto sendButton = ::Settings::AddButtonWithIcon(
		content,
		tr::lng_oblivion_sync_send(),
		st::settingsButton,
		{ &st::menuIconExportTheme });
	sendButton->setClickedCallback([=] {
		const auto value = password();
		if (value && send && !state->status.busy) {
			send(*value);
		}
	});
	const auto receiveWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	::Settings::AddButtonWithIcon(
		receiveWrap->entity(),
		tr::lng_oblivion_sync_receive(),
		st::settingsButton,
		{ &st::menuIconImportTheme }
	)->setClickedCallback([=] {
		const auto value = password();
		if (value && receive && !state->status.busy) {
			receive(*value);
		}
	});
	Ui::AddSkip(content);
	Ui::AddDividerText(
		content,
		rpl::single(tr::lng_oblivion_sync_about(tr::now)
			+ u"\n\n"_q
			+ tr::lng_oblivion_sync_not_synced(tr::now)));

	const auto apply = [=](const BoxStatus &status) {
		state->status = status;
		const auto when = WhenText(status.updated);
		state->title = status.loading
			? tr::lng_oblivion_sync_state_loading(tr::now)
			: !status.error.isEmpty()
			? tr::lng_oblivion_sync_state_error(tr::now)
			: status.conflict
			? tr::lng_oblivion_sync_state_conflict(tr::now)
			: status.exists
			? tr::lng_oblivion_sync_state_copy(tr::now, lt_date, when)
			: tr::lng_oblivion_sync_state_none(tr::now);
		auto lines = QStringList();
		if (!status.loading && !status.error.isEmpty()) {
			lines.push_back(status.error);
		} else if (status.conflict) {
			lines.push_back(tr::lng_oblivion_sync_conflict_about(tr::now));
		} else if (status.exists) {
			lines.push_back(tr::lng_oblivion_sync_state_size(
				tr::now,
				lt_size,
				Ui::FormatSizeText(status.size)));
		} else if (!status.loading) {
			lines.push_back(tr::lng_oblivion_sync_state_none_about(tr::now));
		}
		if (status.synced) {
			lines.push_back(tr::lng_oblivion_sync_state_synced(
				tr::now,
				lt_date,
				WhenText(status.synced)));
		}
		if (status.automatic) {
			// Without a copy on the server nothing is sent by itself: a
			// copy the user has deleted does not come back.
			const auto absent = !status.exists
				&& !status.loading
				&& status.error.isEmpty();
			lines.push_back(!status.remembered
				? tr::lng_oblivion_sync_auto_needs(tr::now)
				: absent
				? tr::lng_oblivion_sync_auto_paused(tr::now)
				: tr::lng_oblivion_sync_auto_on(tr::now));
		}
		state->about = lines.join(QChar('\n'));
		about->setTextColorOverride(
			(!status.loading && !status.error.isEmpty())
				? std::make_optional(st::boxTextFgError->c)
				: std::nullopt);
		state->placeholder = status.remembered
			? tr::lng_oblivion_sync_password_same(tr::now)
			: tr::lng_oblivion_sync_password(tr::now);
		receiveWrap->toggle(
			status.exists || status.loading,
			anim::type::instant);
		// The neutral hint depends on the status, an error stays till the
		// field is edited.
		if (!state->hintIsError) {
			showHint(QString());
		}
		if (state->rebuildButtons) {
			// Not from inside of a click on one of the buttons.
			Ui::PostponeCall(box, [=] {
				state->rebuildButtons();
			});
		}
	};
	std::move(args.status) | rpl::on_next(apply, box->lifetime());
	if (args.errors) {
		std::move(args.errors) | rpl::on_next([=](const QString &error) {
			showHint(error);
			if (!error.isEmpty()) {
				field->showError();
			}
		}, box->lifetime());
	}
	if (state->hint.current().isEmpty()) {
		showHint(QString());
	}

	const auto showMenu = [=] {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		if (state->status.remembered && forget) {
			state->menu->addAction(
				tr::lng_oblivion_sync_forget(tr::now),
				forget,
				&st::menuIconLock);
		}
		if (state->status.exists && erase) {
			state->menu->addAction(
				tr::lng_oblivion_sync_erase(tr::now),
				erase,
				&st::menuIconDelete);
		}
		if (state->menu->empty()) {
			state->menu = nullptr;
		} else {
			state->menu->popup(QCursor::pos());
		}
	};
	// The dots are there only while their menu has something in it: with
	// no copy on the server and no remembered password a click on them
	// did nothing at all.
	state->rebuildButtons = [=] {
		const auto &status = state->status;
		const auto mark = ((status.remembered && forget)
			|| (status.exists && erase)) ? 1 : 0;
		if (std::exchange(state->buttons, mark) == mark) {
			return;
		}
		box->clearButtons();
		box->addButton(tr::lng_close(), [=] {
			box->closeBox();
		});
		if (mark) {
			box->addTopButton(st::boxTitleMenu, showMenu);
		}
	};
	state->rebuildButtons();

	box->setFocusCallback([=] {
		if (!state->status.remembered) {
			field->setFocusFast();
		}
	});
}

// ---- The sync of one account.

class Syncer final : public base::has_weak_ptr {
public:
	explicit Syncer(not_null<Main::Session*> session);

	[[nodiscard]] BoxStatus status() const;
	[[nodiscard]] rpl::producer<BoxStatus> statusValue() const;
	[[nodiscard]] rpl::producer<QString> errors() const;

	void refresh();
	void send(std::shared_ptr<Ui::Show> show, const QString &password);
	void receive(std::shared_ptr<Ui::Show> show, const QString &password);
	void forgetKey();
	void erase(std::shared_ptr<Ui::Show> show);

private:
	struct Remote {
		bool known = false;
		bool exists = false;
		int64 rev = 0;
		int64 updated = 0; // Unixtime.
		QByteArray blob;
	};
	struct Local {
		QJsonObject all; // The whole oblivion.json.
		QJsonObject settings; // The part that is sent.
		QJsonArray presets;
		QJsonArray removed; // The deleted presets, sent along.
		QByteArray hash; // Of the settings and the presets.
	};

	[[nodiscard]] Local local() const;
	[[nodiscard]] bool connected() const;
	void changed();
	void setBusy(bool busy);
	void release();
	void fail(const QString &text);
	void disconnected();
	void eraseConfirmed(std::shared_ptr<Ui::Show> show);
	void fetch(Fn<void()> done, Fn<void(const Cloud::Error &error)> fail);
	void applyRemote(const Cloud::Response &response);
	// done gets an empty key if the password is needed and was not given.
	void keyFor(
		const QString &password,
		Fn<void(QByteArray key, BlobHeader header)> done);
	void put(
		const QByteArray &key,
		const BlobHeader &header,
		Fn<void()> done,
		Fn<void(const Cloud::Error &error)> fail);
	void applyPayload(const Payload &payload, const Local &was);
	void remember(const QByteArray &key, const BlobHeader &header);
	void saveRemembered();

	void localChanged();
	void autoSend();
	void remoteChanged(int64 rev);
	void autoCheck();
	[[nodiscard]] bool takeIncoming();
	[[nodiscard]] bool automatic() const;

	const not_null<Main::Session*> _session;
	const base::weak_ptr<Cloud::Account> _account;
	const QString _path;
	const QByteArray _sealKey;
	Cloud::Sender _sender;
	Remembered _remembered;
	Remote _remote;
	QString _error;
	bool _loading = false;
	bool _busy = false;
	bool _conflict = false;
	bool _applying = false;
	// The revision of an event that came while something was on the way
	// (0: the copy was deleted), -1 without one.
	int64 _missedRev = -1;
	// Grows when everything that was on the way is dropped.
	int _generation = 0;
	// Automatic sends in a row the server has refused with 412.
	int _refused = 0;
	base::Timer _sendTimer;
	base::Timer _checkTimer;
	rpl::event_stream<BoxStatus> _changes;
	rpl::event_stream<QString> _errors;
	rpl::lifetime _lifetime;

};

Syncer::Syncer(not_null<Main::Session*> session)
: _session(session)
, _account(base::make_weak(&Cloud::For(session)))
, _path(AccountFolder(session) + u"sync.json"_q)
, _sealKey(SealKeyFor(session))
, _sender(&Cloud::For(session))
, _sendTimer([=] { autoSend(); })
, _checkTimer([=] { autoCheck(); }) {
	auto file = QFile(_path);
	if (file.open(QIODevice::ReadOnly)) {
		_remembered = ParseRemembered(file.readAll(), _sealKey);
	}

	const auto account = &Cloud::For(session);
	rpl::merge(
		Get().changes(),
		Share::PresetLibrary().changes()
	) | rpl::on_next([=] {
		localChanged();
	}, _lifetime);

	account->events(
	) | rpl::filter([](const Cloud::Event &event) {
		return (event.type == u"me.settings"_q);
	}) | rpl::on_next([=](const Cloud::Event &event) {
		remoteChanged(Cloud::JsonInt(event.data.value(u"rev"_q)));
	}, _lifetime);

	// After the launch and after «Включить снова»: what has another
	// device sent meanwhile? And when the account is switched off (by the
	// user, by a ban, by a protocol that is too old) the core drops what
	// was on the way without calling back, so nothing here may go on
	// waiting for an answer.
	account->readyValue(
	) | rpl::on_next([=](bool ready) {
		if (ready) {
			_checkTimer.callOnce(kStartDelay);
		} else {
			disconnected();
		}
	}, _lifetime);
}

bool Syncer::connected() const {
	const auto account = _account.get();
	return account && account->ready();
}

bool Syncer::automatic() const {
	return Get().cloudSettingsAutoSync()
		&& connected()
		&& _remembered.valid();
}

void Syncer::disconnected() {
	++_generation;
	_sender.cancelAll();
	_sendTimer.cancel();
	_checkTimer.cancel();
	_missedRev = -1;
	if (_loading || _busy) {
		_loading = false;
		_busy = false;
		changed();
	}
}

BoxStatus Syncer::status() const {
	return {
		.loading = _loading,
		.error = _error,
		.exists = _remote.exists,
		.updated = _remote.updated,
		.size = int64(_remote.blob.size()),
		.remembered = _remembered.valid(),
		.conflict = _conflict,
		.automatic = Get().cloudSettingsAutoSync(),
		.busy = _busy,
		.synced = _remembered.time,
	};
}

rpl::producer<BoxStatus> Syncer::statusValue() const {
	return _changes.events_starting_with(status());
}

rpl::producer<QString> Syncer::errors() const {
	return _errors.events();
}

void Syncer::changed() {
	_changes.fire(status());
}

void Syncer::setBusy(bool busy) {
	if (_busy == busy) {
		return;
	} else if (busy) {
		_busy = true;
		changed();
	} else {
		release();
	}
}

// Whatever was on the way is over. An event of another device that came
// meanwhile was not looked at: it is now.
void Syncer::release() {
	_busy = false;
	changed();
	const auto missed = std::exchange(_missedRev, int64(-1));
	if (missed >= 0 && missed != _remembered.rev && automatic()) {
		_checkTimer.callOnce(kRecheckDelay);
	}
}

void Syncer::fail(const QString &text) {
	release();
	_errors.fire_copy(text);
}

Syncer::Local Syncer::local() const {
	auto result = Local();
	result.all = QJsonDocument::fromJson(Get().syncSnapshot()).object();
	result.settings = PortableSettings(result.all);
	result.presets = Share::PresetLibrary().exportJson();
	result.removed = Share::PresetLibrary().exportRemovedJson();
	result.hash = ContentHash(result.settings, result.presets);
	return result;
}

void Syncer::applyRemote(const Cloud::Response &response) {
	_remote = Remote();
	_remote.known = true;
	_remote.exists = !response.bytes.isEmpty();
	_remote.blob = response.bytes;
	_remote.rev = response.header("x-oblivion-rev").toLongLong();
	_remote.updated = response.header(
		"x-oblivion-updated-at").toLongLong() / 1000;
}

void Syncer::fetch(
		Fn<void()> done,
		Fn<void(const Cloud::Error &error)> fail) {
	const auto got = [=] {
		if (!_remote.exists && _remembered.rev) {
			// The copy this device was in sync with is gone: deleted
			// from another device or together with the data of the
			// account. Whatever appears there later is another copy.
			_remembered.rev = 0;
			_sendTimer.cancel();
			saveRemembered();
		}
		done();
	};
	_sender.request(
		Cloud::GetRequest(u"/v1/me/settings"_q),
		crl::guard(this, [=](const Cloud::Response &response) {
			applyRemote(response);
			got();
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			if (error.status == 404 && !error.is("feature_disabled")) {
				_remote = Remote();
				_remote.known = true;
				got();
			} else if (fail) {
				fail(error);
			}
		}));
}

void Syncer::refresh() {
	if (_loading) {
		return;
	}
	_error = QString();
	if (!connected()) {
		// A request that is not sent never answers.
		_error = Cloud::ErrorText({
			.type = Cloud::Error::Type::NotConnected,
		});
		changed();
		return;
	}
	_loading = true;
	changed();
	fetch([=] {
		_loading = false;
		if (!_remote.exists || _remote.rev == _remembered.rev) {
			_conflict = false;
		}
		changed();
	}, [=](const Cloud::Error &error) {
		_loading = false;
		_error = Cloud::ErrorText(error);
		changed();
	});
}

void Syncer::keyFor(
		const QString &password,
		Fn<void(QByteArray key, BlobHeader header)> done) {
	if (password.isEmpty()) {
		// The key this device remembers fits a copy made with the same
		// salt (by this device or by one that has received from it).
		const auto parsed = _remote.exists
			? ParseHeader(_remote.blob)
			: std::nullopt;
		const auto fits = _remembered.valid()
			&& (!parsed
				|| (parsed->salt == _remembered.header.salt
					&& parsed->iterations == _remembered.header.iterations));
		done(
			fits ? _remembered.key : QByteArray(),
			fits ? _remembered.header : BlobHeader());
		return;
	}
	// The salt of the copy of the server is kept (the devices that have
	// the same password go on with the same key), the number of rounds
	// is never taken from there below the minimum of this build.
	const auto header = HeaderForSend(
		_remote.exists ? _remote.blob : QByteArray(),
		_remembered.valid() ? _remembered.header : BlobHeader());
	const auto weak = base::make_weak(this);
	const auto generation = _generation;
	crl::async([=] {
		auto key = DeriveKey(password, header);
		crl::on_main(weak, [=, key = std::move(key)] {
			// Not after «Отключиться»: what has asked for the key is
			// dropped already.
			if (_generation == generation) {
				done(key, header);
			}
		});
	});
}

void Syncer::remember(const QByteArray &key, const BlobHeader &header) {
	_remembered.key = key;
	_remembered.header = header;
	_remembered.time = base::unixtime::now();
	saveRemembered();
}

void Syncer::saveRemembered() {
	const auto bytes = SerializeRemembered(_remembered, _sealKey);
	if (bytes.isEmpty()) {
		// No local key to seal with: the key lives till the app is closed.
		return;
	}
	QDir().mkpath(QFileInfo(_path).absolutePath());
	auto file = QSaveFile(_path);
	if (file.open(QIODevice::WriteOnly)) {
		file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
		file.write(bytes);
		file.commit();
	}
}

void Syncer::forgetKey() {
	_remembered = Remembered();
	_conflict = false;
	_sendTimer.cancel();
	QFile::remove(_path);
	changed();
}

void Syncer::put(
		const QByteArray &key,
		const BlobHeader &header,
		Fn<void()> done,
		Fn<void(const Cloud::Error &error)> fail) {
	if (header.iterations < kMinIterations) {
		// Never a copy with a key that is weaker than this build makes.
		if (fail) {
			fail({ .type = Cloud::Error::Type::Protocol });
		}
		return;
	}
	const auto now = local();
	const auto blob = SealBlob(key, header, SerializePayload({
		.valid = true,
		.settings = now.settings,
		.presets = now.presets,
		.removed = now.removed,
		.device = DeviceName(),
		.savedAt = base::unixtime::now(),
		.appBuild = Update::kOblivionBuild,
	}));
	const auto account = _account.get();
	const auto limit = account
		? account->limit("settings_blob_bytes", kMaxBlob)
		: int64(kMaxBlob);
	if (blob.isEmpty() || blob.size() > limit) {
		if (fail) {
			fail({ .type = Cloud::Error::Type::Http, .status = 413 });
		}
		return;
	}
	auto request = Cloud::PutRawRequest(
		u"/v1/me/settings"_q,
		blob,
		"application/octet-stream");
	// Only over the copy this device has seen: a newer one of another
	// device is never overwritten silently.
	request.headers.push_back({
		"If-Match",
		'"' + QByteArray::number(_remote.exists ? _remote.rev : 0) + '"',
	});
	const auto hash = now.hash;
	_sender.request(
		std::move(request),
		crl::guard(this, [=](const Cloud::Response &response) {
			_remote.known = true;
			_remote.exists = true;
			_remote.blob = blob;
			_remote.rev = Cloud::JsonInt(response.json.value(u"rev"_q));
			_remote.updated = Cloud::JsonInt(
				response.json.value(u"updated_at"_q)) / 1000;
			_remembered.rev = _remote.rev;
			_remembered.hash = hash;
			_conflict = false;
			remember(key, header);
			done();
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			if (fail) {
				fail(error);
			}
		}));
}

void Syncer::send(std::shared_ptr<Ui::Show> show, const QString &password) {
	if (_busy) {
		return;
	} else if (!connected()) {
		fail(Cloud::ErrorText({ .type = Cloud::Error::Type::NotConnected }));
		return;
	}
	setBusy(true);
	const auto generation = _generation;
	const auto putError = [=](const Cloud::Error &error) {
		if (error.status == 412) {
			// Another device was faster.
			fail(tr::lng_oblivion_sync_error_newer(tr::now));
			refresh();
		} else if (error.status == 413) {
			fail(tr::lng_oblivion_sync_error_large(tr::now));
		} else {
			fail(Cloud::ErrorText(error));
		}
	};
	fetch([=] {
		changed();
		keyFor(password, [=](QByteArray key, BlobHeader header) {
			if (key.isEmpty()) {
				fail(tr::lng_oblivion_sync_password_needed(tr::now));
				return;
			}
			const auto other = _remote.exists && !OpenBlob(key, _remote.blob);
			const auto when = WhenText(_remote.updated);
			auto text = !_remote.exists
				? tr::lng_oblivion_sync_send_first(tr::now)
				: tr::lng_oblivion_sync_send_sure(tr::now, lt_date, when);
			if (other) {
				text += u"\n\n"_q + tr::lng_oblivion_sync_send_other(tr::now);
			}
			if (!show->valid()) {
				// The window is gone while the key was counted.
				setBusy(false);
				return;
			}
			const auto weak = base::make_weak(this);
			const auto confirmed = std::make_shared<bool>(false);
			show->showBox(Ui::MakeConfirmBox({
				.text = text,
				.confirmed = [=](Fn<void()> close) {
					*confirmed = true;
					close();
					const auto strong = weak.get();
					if (!strong) {
						return;
					}
					// It may have been dropped while the box was shown.
					strong->setBusy(true);
					strong->put(key, header, crl::guard(strong, [=] {
						strong->setBusy(false);
						if (show->valid()) {
							show->showToast(
								tr::lng_oblivion_sync_sent(tr::now));
						}
					}), putError);
				},
				.cancelled = [=](Fn<void()> close) {
					close();
					if (const auto strong = weak.get()) {
						if (!*confirmed
							&& strong->_generation == generation) {
							strong->setBusy(false);
						}
					}
				},
				.confirmText = tr::lng_oblivion_sync_send_confirm(),
			}));
		});
	}, [=](const Cloud::Error &error) {
		fail(Cloud::ErrorText(error));
	});
}

void Syncer::applyPayload(const Payload &payload, const Local &was) {
	_applying = true;
	const auto merged = MergeSettings(was.all, payload.settings);
	if (merged != was.all) {
		Get().syncApply(
			QJsonDocument(merged).toJson(QJsonDocument::Indented));
	}
	Share::PresetLibrary().mergeJson(payload.presets, payload.removed);
	_applying = false;
	_remembered.rev = _remote.rev;
	_remembered.hash = local().hash;
	_conflict = false;
}

void Syncer::receive(
		std::shared_ptr<Ui::Show> show,
		const QString &password) {
	if (_busy) {
		return;
	} else if (!connected()) {
		fail(Cloud::ErrorText({ .type = Cloud::Error::Type::NotConnected }));
		return;
	}
	setBusy(true);
	const auto generation = _generation;
	fetch([=] {
		changed();
		if (!_remote.exists) {
			fail(tr::lng_oblivion_sync_error_none(tr::now));
			return;
		} else if (!ParseHeader(_remote.blob)) {
			fail(tr::lng_oblivion_sync_error_format(tr::now));
			return;
		}
		keyFor(password, [=](QByteArray key, BlobHeader header) {
			if (key.isEmpty()) {
				fail(tr::lng_oblivion_sync_password_needed(tr::now));
				return;
			}
			const auto plain = OpenBlob(key, _remote.blob);
			if (!plain) {
				fail(tr::lng_oblivion_sync_password_wrong(tr::now));
				return;
			}
			const auto payload = ParsePayload(*plain);
			if (!payload.valid) {
				fail(tr::lng_oblivion_sync_error_format(tr::now));
				return;
			}
			const auto was = local();
			const auto merged = MergeSettings(was.all, payload.settings);
			const auto count = CountDifferences(was.all, merged);
			// The saved presets that were deleted on the other device
			// and are still here: they go too, and the box says so.
			const auto dropped = Share::PresetLibrary().countRemovals(
				payload.removed);
			const auto weak = base::make_weak(this);
			const auto finish = [=] {
				const auto strong = weak.get();
				if (!strong) {
					return;
				}
				strong->applyPayload(payload, strong->local());
				strong->remember(key, header);
				strong->setBusy(false);
				if (show->valid()) {
					show->showToast((count || dropped)
						? tr::lng_oblivion_sync_received(tr::now)
						: tr::lng_oblivion_sync_received_same(tr::now));
				}
			};
			if (!count && !dropped) {
				// Nothing to replace or delete: nothing to ask about.
				finish();
				return;
			} else if (!show->valid()) {
				setBusy(false);
				return;
			}
			const auto device = payload.device.isEmpty()
				? tr::lng_oblivion_sync_device_unknown(tr::now)
				: payload.device;
			auto text = count
				? tr::lng_oblivion_sync_receive_sure(
					tr::now,
					lt_count,
					count,
					lt_date,
					WhenText(_remote.updated),
					lt_device,
					device)
				: QString();
			if (dropped) {
				if (!text.isEmpty()) {
					text += u"\n\n"_q;
				}
				text += tr::lng_oblivion_sync_receive_presets(
					tr::now,
					lt_count,
					dropped);
			}
			const auto confirmed = std::make_shared<bool>(false);
			show->showBox(Ui::MakeConfirmBox({
				.text = text,
				.confirmed = [=](Fn<void()> close) {
					*confirmed = true;
					close();
					finish();
				},
				.cancelled = [=](Fn<void()> close) {
					close();
					if (const auto strong = weak.get()) {
						if (!*confirmed
							&& strong->_generation == generation) {
							strong->setBusy(false);
						}
					}
				},
				.confirmText = tr::lng_oblivion_sync_receive_confirm(),
			}));
		});
	}, [=](const Cloud::Error &error) {
		fail(Cloud::ErrorText(error));
	});
}

void Syncer::erase(std::shared_ptr<Ui::Show> show) {
	const auto weak = base::make_weak(this);
	show->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_sync_erase_sure(),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (const auto strong = weak.get()) {
				strong->eraseConfirmed(show);
			}
		},
		.confirmText = tr::lng_box_delete(),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

void Syncer::eraseConfirmed(std::shared_ptr<Ui::Show> show) {
	if (!connected()) {
		Cloud::ShowError(show, { .type = Cloud::Error::Type::NotConnected });
		return;
	}
	// A send that is on the way (the automatic one) must not put the copy
	// back after it is deleted.
	++_generation;
	_sender.cancelAll();
	_sendTimer.cancel();
	_missedRev = -1;
	_loading = false;
	_busy = true;
	changed();

	const auto erased = [=] {
		_remote = Remote();
		_remote.known = true;
		_remembered.rev = 0;
		_conflict = false;
		_busy = false;
		_missedRev = -1;
		_sendTimer.cancel();
		saveRemembered();

		// «Удалить копию» means that the settings stay on the devices
		// only. With the automatic mode left on the very next change of
		// a setting would send them to the server again, so it is
		// switched off together with the copy (the toast says so). The
		// other devices of the user don't make a copy by themselves
		// either: see autoSend().
		const auto automatic = Get().cloudSettingsAutoSync();
		Get().setCloudSettingsAutoSync(false);
		changed();
		if (show && show->valid()) {
			show->showToast(automatic
				? tr::lng_oblivion_sync_erased_auto(tr::now)
				: tr::lng_oblivion_sync_erased(tr::now));
		}
	};
	_sender.request(
		Cloud::DeleteRequest(u"/v1/me/settings"_q),
		crl::guard(this, [=](const Cloud::Response &response) {
			erased();
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			if (error.status == 404 && !error.is("feature_disabled")) {
				// It is gone already (deleted from another device).
				erased();
				return;
			}
			release();
			Cloud::ShowError(show, error);
		}));
}

// ---- The automatic mode. Everything here is quiet: no boxes, no
// toasts, a failure waits for the next change or the next launch.
//
// It works only between a copy that is on the server and this device: a
// copy is never made by it where there is none. The first copy, and a
// new one after «Удалить копию с сервера» (here or on another device),
// is made by «Отправить» only.

void Syncer::localChanged() {
	if (_applying
		|| !automatic()
		|| (_remote.known && !_remote.exists)) {
		return;
	}
	_sendTimer.callOnce(kSendDelay);
}

void Syncer::autoSend() {
	if (!automatic()
		|| _busy
		|| _conflict
		|| (_remote.known && !_remote.exists)) {
		return;
	}
	const auto now = local();
	if (now.hash == _remembered.hash) {
		return;
	}
	_busy = true;
	const auto done = [=] {
		release();
	};
	fetch([=] {
		if (!_remote.exists) {
			// Deleted meanwhile: not made again from here.
			done();
			return;
		} else if (_remote.rev != _remembered.rev && !takeIncoming()) {
			// Another device has sent something this one has not seen:
			// it is taken, or it is the same already, or both have
			// changed and the user decides.
			done();
			return;
		}
		const auto sent = [=] {
			_refused = 0;
			done();
		};
		put(_remembered.key, _remembered.header, sent, [=](
				const Cloud::Error &error) {
			if (error.status == 412 && ++_refused >= kMaxRefused) {
				// The server keeps saying that its copy is not the one
				// that was just looked at. Not again and again: the box
				// asks the user.
				_refused = 0;
				_conflict = true;
			} else if (error.status == 412) {
				// Another device was faster by a moment. Its event may
				// have come while this one was sending: looked at soon.
				_missedRev = std::max(
					_missedRev,
					Cloud::JsonInt(error.details.value(u"rev"_q), 0));
			}
			done();
		});
	}, [=](const Cloud::Error &error) {
		done();
	});
}

void Syncer::remoteChanged(int64 rev) {
	if (!automatic()) {
		return;
	} else if (_busy) {
		// The own send tells about itself too: release() sorts it out.
		_missedRev = std::max(rev, int64(0));
		return;
	} else if (rev > 0 && rev == _remembered.rev) {
		return;
	}
	// A new copy, or (rev 0) the copy was deleted from another device.
	autoCheck();
}

// The copy of the server is not the one this device has seen last. True:
// it brings nothing new and this device has changes of its own that go
// over it. False: there is nothing to send (the copy was taken, or it is
// the same already, or the user has to decide).
bool Syncer::takeIncoming() {
	// A refused send is explained: there is a copy that was not seen.
	_refused = 0;
	const auto was = local();
	const auto parsed = ParseHeader(_remote.blob);
	const auto fits = parsed
		&& (parsed->salt == _remembered.header.salt)
		&& (parsed->iterations == _remembered.header.iterations);
	const auto plain = fits
		? OpenBlob(_remembered.key, _remote.blob)
		: std::nullopt;
	const auto payload = plain ? ParsePayload(*plain) : Payload();
	if (!payload.valid) {
		// Another password, or a copy this build can't read.
		_conflict = true;
		return false;
	}
	auto &library = Share::PresetLibrary();
	const auto unsent = (was.hash != _remembered.hash);
	const auto brings
		= (MergeSettings(was.all, payload.settings) != was.all)
		|| library.mergeChanges(payload.presets, payload.removed);
	const auto same = (was.settings == payload.settings)
		&& SamePresets(was.presets, payload.presets);
	const auto verdict = JudgeIncoming(unsent, brings, same);
	if (verdict == Incoming::Conflict) {
		_conflict = true;
		return false;
	} else if (verdict == Incoming::Apply) {
		applyPayload(payload, was);
		_remembered.time = base::unixtime::now();
		saveRemembered();
		return false;
	}
	// Nothing of it changes this device. What was deleted elsewhere is
	// still noted, so that it is not sent back from here later.
	_applying = true;
	library.mergeJson(QJsonArray(), payload.removed);
	_applying = false;
	_remembered.rev = _remote.rev;
	_conflict = false;
	if (verdict == Incoming::Same) {
		_remembered.hash = was.hash;
	}
	saveRemembered();
	return (verdict == Incoming::Ahead);
}

void Syncer::autoCheck() {
	if (!automatic() || _busy) {
		return;
	}
	_busy = true;
	const auto done = [=] {
		release();
	};
	fetch([=] {
		if (!_remote.exists) {
			// No copy (it was deleted): nothing to take and nothing is
			// sent by itself.
			done();
			return;
		}
		const auto unsent = (_remote.rev == _remembered.rev)
			|| takeIncoming();
		done();
		if (unsent) {
			// Changes made while the app was closed or offline, or what
			// this device has over the copy it has just looked at.
			localChanged();
		}
	}, [=](const Cloud::Error &error) {
		done();
	});
}

using SyncersMap = base::flat_map<
	not_null<Main::Session*>,
	std::unique_ptr<Syncer>>;

[[nodiscard]] SyncersMap &Syncers() {
	static const auto result = new SyncersMap();
	return *result;
}

void DestroySyncer(not_null<Main::Session*> session) {
	auto &map = Syncers();
	const auto i = map.find(session);
	if (i != end(map)) {
		const auto taken = std::move(i->second);
		map.erase(i);
	}
}

[[nodiscard]] Syncer &SyncerFor(not_null<Main::Session*> session) {
	auto &map = Syncers();
	const auto i = map.find(session);
	if (i != end(map)) {
		return *i->second;
	}
	auto syncer = std::make_unique<Syncer>(session);
	const auto raw = syncer.get();
	map.emplace(session, std::move(syncer));
	session->lifetime().add([=] {
		DestroySyncer(session);
	});
	return *raw;
}

// ---- Self-test.

class Checker final {
public:
	explicit Checker(QStringList &log);

	void operator()(bool condition, const char *what);
	void section(const char *name);
	[[nodiscard]] int failed() const;

private:
	QStringList &_log;
	int _failed = 0;
	int _sectionPassed = 0;
	int _sectionFailed = 0;

};

Checker::Checker(QStringList &log)
: _log(log) {
}

void Checker::operator()(bool condition, const char *what) {
	if (condition) {
		++_sectionPassed;
	} else {
		++_failed;
		++_sectionFailed;
		_log.push_back(u"FAILED: "_q + QString::fromUtf8(what));
	}
}

void Checker::section(const char *name) {
	_log.push_back(u"%1: %2 passed, %3 failed"_q.arg(
		QString::fromUtf8(name),
		QString::number(_sectionPassed),
		QString::number(_sectionFailed)));
	_sectionPassed = _sectionFailed = 0;
}

int Checker::failed() const {
	return _failed;
}

void TestBlob(Checker &check) {
	// The mechanics are checked with a small number of rounds (the real
	// one takes a part of a second for every key), the rules about the
	// number itself are checked below without making a key.
	const auto header = BlobHeader{
		.iterations = kTestIterations,
		.salt = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f"),
	};
	const auto password = QString::fromUtf8("correct horse \xD0\xB6");
	const auto key = DeriveKey(password, header);
	check(key.size() == 32, "kdf: a key of 32 bytes");
	check(DeriveKey(password, header) == key, "kdf: the same every time");
	check(DeriveKey(password + 'x', header) != key,
		"kdf: another password, another key");
	auto otherSalt = header;
	otherSalt.salt[0] = char(0x7F);
	check(DeriveKey(password, otherSalt) != key,
		"kdf: another salt, another key");
	auto moreRounds = header;
	moreRounds.iterations = kTestIterations + 1;
	check(DeriveKey(password, moreRounds) != key,
		"kdf: other rounds, another key");

	const auto plain = QByteArray("{\"v\":1,\"settings\":{\"a\":true}}")
		+ QByteArray(5000, 'z');
	const auto blob = SealBlob(key, header, plain);
	check(!blob.isEmpty(), "blob: sealed");
	check(blob.startsWith(Magic()), "blob: starts with the magic");
	check(!blob.contains("settings") && !blob.contains("zzzzzzzz"),
		"blob: nothing of the plain text is seen");
	check(blob.size() < plain.size(), "blob: compressed");
	const auto parsed = ParseHeader(blob, kTestIterations);
	check(parsed
		&& parsed->salt == header.salt
		&& parsed->iterations == header.iterations,
		"blob: the header reads back");
	const auto opened = OpenBlob(key, blob);
	check(opened && *opened == plain, "blob: opens with the key");
	check(SealBlob(key, header, plain) != blob,
		"blob: a fresh nonce every time");

	const auto wrong = DeriveKey(u"wrong password"_q, header);
	check(!OpenBlob(wrong, blob), "blob: a wrong password does not open it");
	check(!OpenBlob(QByteArray(32, '\0'), blob),
		"blob: a zero key does not open it");
	check(!OpenBlob(key.left(16), blob), "blob: a short key is refused");

	auto flipped = blob;
	flipped[flipped.size() - 1] = char(flipped[flipped.size() - 1] ^ 1);
	check(!OpenBlob(key, flipped), "blob: a changed byte is noticed");
	auto header2 = blob;
	header2[12] = char(header2[12] ^ 1);
	check(!OpenBlob(key, header2), "blob: a changed salt is noticed");
	check(!OpenBlob(key, blob.left(blob.size() - 5)),
		"blob: a cut copy is refused");
	check(!ParseHeader(QByteArray(), 1), "header: empty");
	check(!ParseHeader(QByteArray(100, 'x'), 1), "header: no magic");
	auto newer = blob;
	newer[4] = char(kBlobVersion + 1);
	check(!ParseHeader(newer, 1), "header: a newer version is refused");
	auto slow = blob;
	slow[6] = char(0x7F);
	check(!ParseHeader(slow, 1), "header: too many rounds are refused");
	auto fast = blob;
	fast[6] = fast[7] = fast[8] = char(0);
	fast[9] = char(1);
	check(!ParseHeader(fast, kTestIterations),
		"header: too few rounds are refused");
	auto none = blob;
	none[6] = none[7] = none[8] = none[9] = char(0);
	check(!ParseHeader(none, 0), "header: no rounds at all are refused");
	check(!ParseHeader(blob + QByteArray(kMaxBlob, 'x'), 1),
		"header: a huge copy is refused");
	check(SealBlob(key.left(8), header, plain).isEmpty(),
		"blob: a bad key seals nothing");

	// The strength of the key is the business of this device, not of the
	// bytes the server gives back.
	check(kMinIterations >= 600'000 && kIterations >= kMinIterations,
		"kdf: never fewer than 600 000 rounds");
	check(!ParseHeader(blob),
		"kdf: a copy with a weak header is not opened with a password");
	const auto planted = HeaderForSend(blob);
	check(planted.iterations == kIterations
		&& planted.salt.size() == kSaltSize
		&& planted.salt != header.salt,
		"kdf: a planted weak header does not survive a send");
	const auto full = BlobHeader{
		.iterations = kIterations,
		.salt = QByteArray(kSaltSize, 'q'),
	};
	// Only the header matters here: the key is not the one of a password.
	const auto fullBlob = SealBlob(key, full, plain);
	check(ParseHeader(fullBlob).has_value(),
		"kdf: a copy with the full count is taken");
	const auto kept = HeaderForSend(fullBlob);
	check(kept.iterations == kIterations && kept.salt == full.salt,
		"kdf: the salt of a copy with the full count is kept");
	auto more = full;
	more.iterations = kIterations * 2;
	check(HeaderForSend(SealBlob(key, more, plain)).iterations
		== kIterations * 2,
		"kdf: more rounds of a newer build are kept");
	const auto fresh = HeaderForSend(QByteArray());
	check(fresh.iterations == kIterations
		&& fresh.salt.size() == kSaltSize
		&& HeaderForSend(QByteArray()).salt != fresh.salt,
		"kdf: no copy, the full count and a fresh salt every time");
	check(HeaderForSend(QByteArray(200, 'x')).iterations == kIterations,
		"kdf: garbage on the server, the full count");
	const auto again = HeaderForSend(QByteArray(), full);
	check(again.salt == full.salt && again.iterations == kIterations,
		"kdf: no copy, the salt this device has used before is kept");
	check(HeaderForSend(blob, full).salt == full.salt,
		"kdf: a weak copy, the salt this device has used before");
	auto other = full;
	other.salt = QByteArray(kSaltSize, 'w');
	check(HeaderForSend(fullBlob, other).salt == full.salt,
		"kdf: the salt of a good copy goes before the one used here");
	check(HeaderForSend(QByteArray(), header).iterations == kIterations
		&& HeaderForSend(QByteArray(), header).salt != header.salt,
		"kdf: a weak header used before is not used again");
	check.section("blob");
}

void TestSettings(Checker &check) {
	auto all = QJsonObject();
	all.insert(u"ghost_read"_q, true);
	all.insert(u"show_seconds"_q, false);
	all.insert(u"fake_stars"_q, 100.);
	all.insert(u"voice_effect"_q, u"robot"_q);
	all.insert(u"local_names"_q, QJsonObject{ { u"1"_q, u"Mom"_q } });
	all.insert(u"online_notify"_q, QJsonArray{ u"5"_q });
	all.insert(u"app_icon"_q, u"night"_q);
	all.insert(u"app_icon_digest"_q, u"123"_q);
	all.insert(u"room_music_volume"_q, 40.);
	all.insert(u"cloud_badge"_q, true);
	all.insert(u"cloud_update_last_check"_q, 1759800000.);
	all.insert(u"cloud_chosen"_q, QJsonArray{ u"7"_q });
	all.insert(u"badge_enabled"_q, true);
	all.insert(u"online_polling"_q, true);

	const auto portable = PortableSettings(all);
	check(portable.contains(u"ghost_read"_q)
		&& portable.contains(u"local_names"_q)
		&& portable.contains(u"voice_effect"_q),
		"portable: the settings are there");
	check(!portable.contains(u"app_icon"_q)
		&& !portable.contains(u"app_icon_digest"_q)
		&& !portable.contains(u"room_music_volume"_q)
		&& !portable.contains(u"badge_enabled"_q)
		&& !portable.contains(u"online_polling"_q),
		"portable: nothing of the device");
	auto cloud = false;
	for (auto i = portable.begin(); i != portable.end(); ++i) {
		cloud = cloud || i.key().startsWith(u"cloud_"_q);
	}
	check(!cloud, "portable: nothing of the cloud connection");

	auto received = QJsonObject();
	received.insert(u"ghost_read"_q, false);
	received.insert(u"show_seconds"_q, u"yes"_q); // A wrong kind.
	received.insert(u"fake_stars"_q, 5.);
	received.insert(u"app_icon"_q, u"evil"_q); // Of the device.
	received.insert(u"cloud_badge"_q, false); // Of the cloud.
	received.insert(u"from_the_future"_q, true); // Unknown here.
	received.insert(u"local_names"_q, QJsonObject{ { u"2"_q, u"Dad"_q } });
	const auto merged = MergeSettings(all, received);
	check(merged.value(u"ghost_read"_q) == QJsonValue(false),
		"merge: a setting is replaced");
	check(merged.value(u"fake_stars"_q).toDouble() == 5.,
		"merge: a number is replaced");
	check(merged.value(u"show_seconds"_q) == QJsonValue(false),
		"merge: a value of a wrong kind is ignored");
	check(merged.value(u"app_icon"_q).toString() == u"night"_q,
		"merge: the device part stays");
	check(merged.value(u"cloud_badge"_q) == QJsonValue(true),
		"merge: the cloud part stays");
	check(!merged.contains(u"from_the_future"_q),
		"merge: an unknown setting is not added");
	check(merged.value(u"local_names"_q).toObject().contains(u"2"_q),
		"merge: an object is replaced as a whole");
	check(merged.size() == all.size(), "merge: no keys appear or vanish");
	check(CountDifferences(all, merged) == 3, "merge: three differences");
	check(CountDifferences(all, MergeSettings(all, portable)) == 0,
		"merge: the own copy changes nothing");
	check(CountDifferences(all, MergeSettings(all, QJsonObject())) == 0,
		"merge: an empty copy changes nothing");

	const auto presets = QJsonArray{ QJsonObject{ { u"id"_q, u"1"_q } } };
	const auto hash = ContentHash(portable, presets);
	check(hash.size() == 32, "hash: 32 bytes");
	check(ContentHash(PortableSettings(all), presets) == hash,
		"hash: stable");
	auto device = all;
	device.insert(u"app_icon"_q, u"day"_q);
	device.insert(u"cloud_update_last_check"_q, 1.);
	check(ContentHash(PortableSettings(device), presets) == hash,
		"hash: a device setting does not change it");
	check(ContentHash(PortableSettings(merged), presets) != hash,
		"hash: a setting changes it");
	check(ContentHash(portable, QJsonArray()) != hash,
		"hash: a preset changes it");

	const auto removed = QJsonArray{ QJsonObject{
		{ u"id"_q, u"9"_q },
		{ u"time"_q, 1759800000. },
	} };
	const auto payload = Payload{
		.valid = true,
		.settings = portable,
		.presets = presets,
		.removed = removed,
		.device = u"MacBook"_q,
		.savedAt = 1759800000,
		.appBuild = 5000000,
	};
	const auto back = ParsePayload(SerializePayload(payload));
	check(back.valid
		&& back.settings == portable
		&& back.presets == presets
		&& back.removed == removed
		&& back.device == u"MacBook"_q
		&& back.savedAt == 1759800000
		&& back.appBuild == 5000000,
		"payload: reads back the same");
	auto older = payload;
	older.removed = QJsonArray();
	check(ParsePayload(SerializePayload(older)).valid
		&& ParsePayload(SerializePayload(older)).removed.isEmpty()
		&& !SerializePayload(older).contains("presets_removed"),
		"payload: a copy without deleted presets is a copy of before");
	check(!ParsePayload(QByteArray("[]")).valid, "payload: not an object");
	check(!ParsePayload(QByteArray("{\"v\":2,\"settings\":{}}")).valid,
		"payload: a newer version is refused");
	check(!ParsePayload(QByteArray("{\"v\":1,\"settings\":5}")).valid,
		"payload: the settings must be an object");
	check(!ParsePayload(QByteArray("garbage")).valid, "payload: garbage");

	// The whole way: this device seals, another one opens and merges.
	const auto header = BlobHeader{
		.iterations = kTestIterations,
		.salt = QByteArray(kSaltSize, 's'),
	};
	const auto key = DeriveKey(u"secret password"_q, header);
	const auto blob = SealBlob(key, header, SerializePayload(payload));
	const auto otherHeader = ParseHeader(blob, kTestIterations);
	const auto otherKey = DeriveKey(
		u"secret password"_q,
		otherHeader.value_or(BlobHeader()));
	const auto opened = OpenBlob(otherKey, blob);
	check(opened.has_value(), "round trip: the same password opens");
	if (opened) {
		auto other = all;
		other.insert(u"ghost_read"_q, false);
		other.insert(u"app_icon"_q, u"mine"_q);
		const auto result = MergeSettings(
			other,
			ParsePayload(*opened).settings);
		check(result.value(u"ghost_read"_q) == QJsonValue(true)
			&& result.value(u"app_icon"_q).toString() == u"mine"_q,
			"round trip: the settings arrive, the device part stays");
	}

	const auto sealKey = QByteArray(32, 'k');
	const auto remembered = Remembered{
		.header = { .iterations = kIterations, .salt = header.salt },
		.key = key,
		.rev = 12,
		.hash = hash,
		.time = 1759800000,
	};
	const auto stored = SerializeRemembered(remembered, sealKey);
	check(!stored.isEmpty(), "remembered: serialized");
	check(!stored.contains(key.toBase64()) && !stored.contains(key.toHex()),
		"remembered: the key is not in the file as it is");
	const auto restored = ParseRemembered(stored, sealKey);
	check(restored.valid()
		&& restored.key == key
		&& restored.header.salt == header.salt
		&& restored.header.iterations == kIterations
		&& restored.rev == 12
		&& restored.hash == hash
		&& restored.time == 1759800000,
		"remembered: reads back the same");
	check(!ParseRemembered(stored, QByteArray(32, 'x')).valid(),
		"remembered: another local key does not open it");
	check(!ParseRemembered(QByteArray("{}"), sealKey).valid(),
		"remembered: an empty file");
	check(SerializeRemembered(remembered, QByteArray()).isEmpty(),
		"remembered: nothing is written without a local key");
	auto weak = remembered;
	weak.header.iterations = kTestIterations;
	const auto weakStored = SerializeRemembered(weak, sealKey);
	check(!weakStored.isEmpty()
		&& !ParseRemembered(weakStored, sealKey).valid(),
		"remembered: a key of a weak header is not taken back");
	check.section("settings");
}

// The automatic mode: what is done with a copy of the server this device
// has not seen.
void TestIncoming(Checker &check) {
	check(JudgeIncoming(false, true, false) == Incoming::Apply,
		"incoming: nothing unsent here, the copy is taken");
	check(JudgeIncoming(true, true, false) == Incoming::Conflict,
		"incoming: both have changed, the user decides");
	check(JudgeIncoming(true, false, true) == Incoming::Same,
		"incoming: the same change made here already is not a conflict");
	check(JudgeIncoming(true, false, false) == Incoming::Ahead,
		"incoming: a copy that brings nothing is sent over");
	check(JudgeIncoming(false, false, false) == Incoming::Same
		&& JudgeIncoming(false, false, true) == Incoming::Same,
		"incoming: a device without changes of its own never sends");
	check(JudgeIncoming(false, true, true) == Incoming::Apply,
		"incoming: taken even if the two look the same");

	// Two accounts of one installation share oblivion.json: the first one
	// applies its copy, for the second one the same copy brings nothing.
	auto all = QJsonObject();
	all.insert(u"ghost_read"_q, true);
	all.insert(u"fake_stars"_q, 100.);
	all.insert(u"app_icon"_q, u"night"_q);
	auto received = QJsonObject();
	received.insert(u"ghost_read"_q, false);
	received.insert(u"fake_stars"_q, 100.);
	const auto first = MergeSettings(all, received);
	check(first != all, "incoming: the first account takes the change");
	check(MergeSettings(first, received) == first,
		"incoming: the same copy brings nothing to the second account");
	check(PortableSettings(first) == received,
		"incoming: and the second account has nothing to send");

	const auto a = QJsonObject{
		{ u"id"_q, u"1"_q },
		{ u"title"_q, u"A"_q },
		{ u"time"_q, 5. },
	};
	const auto b = QJsonObject{
		{ u"id"_q, u"2"_q },
		{ u"title"_q, u"B"_q },
		{ u"time"_q, 6. },
	};
	auto later = a;
	later.insert(u"time"_q, 99.);
	auto renamed = a;
	renamed.insert(u"title"_q, u"C"_q);
	check(SamePresets(QJsonArray{ a, b }, QJsonArray{ b, later }),
		"presets: the same in another order and saved at another time");
	check(!SamePresets(QJsonArray{ a, b }, QJsonArray{ a }),
		"presets: one is missing");
	check(!SamePresets(QJsonArray{ a, b }, QJsonArray{ renamed, b }),
		"presets: one is renamed");
	check(!SamePresets(QJsonArray{ a, b }, QJsonArray{ a, a }),
		"presets: one twice is not two");
	check(SamePresets(QJsonArray(), QJsonArray()), "presets: none and none");
	check.section("incoming");
}

// ---- Snapshot scenes.

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto size = QSize(st::boxWideWidth * 2, 0);
	const auto scene = [=](
			const QString &name,
			BoxStatus status,
			QString error = QString()) {
		RegisterBoxScene(name, size, [=](std::shared_ptr<Ui::Show> show) {
			auto errors = error.isEmpty()
				? rpl::producer<QString>(rpl::never<QString>())
				: rpl::producer<QString>(rpl::single(error));
			// With the actions of the menu: its dots are shown only when
			// there is something to choose there.
			return Box(SyncBox, SyncBoxArgs{
				.status = rpl::single(status),
				.errors = std::move(errors),
				.forget = [] {},
				.erase = [] {},
			});
		});
	};
	const auto when = int64(1791327935);
	scene(u"sync_box_first"_q, {});
	scene(u"sync_box_loading"_q, { .loading = true });
	scene(u"sync_box_copy"_q, {
		.exists = true,
		.updated = when,
		.size = 12'345,
	});
	scene(u"sync_box_remembered"_q, {
		.exists = true,
		.updated = when,
		.size = 12'345,
		.remembered = true,
		.automatic = true,
		.synced = when - 3600,
	});
	scene(u"sync_box_conflict"_q, {
		.exists = true,
		.updated = when,
		.size = 12'345,
		.remembered = true,
		.conflict = true,
		.automatic = true,
		.synced = when - 86400,
	});
	// The copy was deleted: the automatic mode waits for «Отправить».
	scene(u"sync_box_deleted"_q, {
		.remembered = true,
		.automatic = true,
		.synced = when - 86400,
	});
	scene(u"sync_box_wrong_password"_q, {
		.exists = true,
		.updated = when,
		.size = 12'345,
	}, tr::lng_oblivion_sync_password_wrong(tr::now));
	scene(u"sync_box_offline"_q, {
		.error = tr::lng_oblivion_cloud_error_network(tr::now),
	});
	RegisterBoxScene(u"sync_send_sure"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_sync_send_sure(
				tr::now,
				lt_date,
				WhenText(when))
				+ u"\n\n"_q
				+ tr::lng_oblivion_sync_send_other(tr::now),
			.confirmText = tr::lng_oblivion_sync_send_confirm(),
		});
	});
	RegisterBoxScene(u"sync_receive_sure"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_sync_receive_sure(
				tr::now,
				lt_count,
				7,
				lt_date,
				WhenText(when),
				lt_device,
				u"MacBook Air"_q),
			.confirmText = tr::lng_oblivion_sync_receive_confirm(),
		});
	});
	RegisterBoxScene(u"sync_receive_presets"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_sync_receive_sure(
				tr::now,
				lt_count,
				7,
				lt_date,
				WhenText(when),
				lt_device,
				u"MacBook Air"_q)
				+ u"\n\n"_q
				+ tr::lng_oblivion_sync_receive_presets(
					tr::now,
					lt_count,
					2),
			.confirmText = tr::lng_oblivion_sync_receive_confirm(),
		});
	});
	RegisterBoxScene(u"sync_erase_sure"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_sync_erase_sure(),
			.confirmText = tr::lng_box_delete(),
			.confirmStyle = &st::attentionBoxButton,
		});
	});
});

} // namespace

void Start(not_null<Main::Session*> session) {
	// The watcher of the automatic mode. Nothing is sent by it while the
	// account has not agreed, has no remembered key or the mode is off.
	(void)SyncerFor(session);
}

void Forget(not_null<Main::Session*> session) {
	if (const auto i = Syncers().find(session); i != end(Syncers())) {
		i->second->forgetKey();
	}
	DestroySyncer(session);
	QFile::remove(AccountFolder(session) + u"sync.json"_q);
}

void ShowBox(not_null<Window::SessionController*> controller) {
	Cloud::RequireConsent(controller, crl::guard(controller, [=] {
		const auto syncer = &SyncerFor(&controller->session());
		const auto weak = base::make_weak(syncer);
		const auto show = controller->uiShow();
		syncer->refresh();
		controller->show(Box(SyncBox, SyncBoxArgs{
			.status = syncer->statusValue(),
			.send = [=](const QString &password) {
				if (const auto strong = weak.get()) {
					strong->send(show, password);
				}
			},
			.receive = [=](const QString &password) {
				if (const auto strong = weak.get()) {
					strong->receive(show, password);
				}
			},
			.errors = syncer->errors(),
			.forget = [=] {
				if (const auto strong = weak.get()) {
					strong->forgetKey();
					show->showToast(tr::lng_oblivion_sync_forgot(tr::now));
				}
			},
			.erase = [=] {
				if (const auto strong = weak.get()) {
					strong->erase(show);
				}
			},
		}));
	}));
}

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestBlob(check);
	TestSettings(check);
	TestIncoming(check);
	const auto update = Update::RunSelfTest(log);
	return !check.failed() && update;
}

} // namespace Oblivion::Sync
