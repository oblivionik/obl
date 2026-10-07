/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud_sync.h"

#include "base/call_delayed.h"
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
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSysInfo>

namespace Oblivion::Sync {
namespace {

constexpr auto kBlobVersion = 1;
constexpr auto kKdfPbkdf2Sha256 = 1;
constexpr auto kIterations = 600'000;
constexpr auto kMinIterations = 1000;
constexpr auto kMaxIterations = 5'000'000;
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

[[nodiscard]] std::optional<BlobHeader> ParseHeader(const QByteArray &blob) {
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
	// A copy somebody has planted must not make the app count for hours.
	if (iterations < uint32(kMinIterations)
		|| iterations > uint32(kMaxIterations)) {
		return std::nullopt;
	}
	return BlobHeader{
		.iterations = int(iterations),
		.salt = blob.mid(10, kSaltSize),
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
	if (key.size() != 32 || !ParseHeader(blob)) {
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
	};
	const auto state = box->lifetime().make_state<State>();
	const auto send = std::move(args.send);
	const auto receive = std::move(args.receive);
	const auto forget = std::move(args.forget);
	const auto erase = std::move(args.erase);

	box->setTitle(tr::lng_oblivion_sync_title());
	box->setWidth(st::boxWideWidth);

	const auto content = box->verticalLayout();
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_oblivion_sync_about(),
			st::boxLabel),
		st::boxRowPadding + style::margins(0, 0, 0, st::boxMediumSkip));
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
			st::boxMediumSkip));

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
	const auto sendButton = ::Settings::AddButtonWithIcon(
		content,
		tr::lng_oblivion_sync_send(),
		st::settingsButton,
		{ &st::menuIconExport });
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
		{ &st::menuIconDownload }
	)->setClickedCallback([=] {
		const auto value = password();
		if (value && receive && !state->status.busy) {
			receive(*value);
		}
	});
	Ui::AddSkip(content);
	Ui::AddDividerText(content, tr::lng_oblivion_sync_not_synced());

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
			lines.push_back(status.remembered
				? tr::lng_oblivion_sync_auto_on(tr::now)
				: tr::lng_oblivion_sync_auto_needs(tr::now));
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

	const auto top = box->addTopButton(st::boxTitleMenu);
	top->setClickedCallback([=] {
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
	});

	box->setFocusCallback([=] {
		if (!state->status.remembered) {
			field->setFocusFast();
		}
	});
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
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
		QByteArray hash;
	};

	[[nodiscard]] Local local() const;
	void changed();
	void setBusy(bool busy);
	void fail(const QString &text);
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
	base::Timer _sendTimer;
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
, _sendTimer([=] { autoSend(); }) {
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

	// Once after the launch: what has another device sent meanwhile?
	account->readyValue(
	) | rpl::filter([](bool ready) {
		return ready;
	}) | rpl::take(1) | rpl::on_next([=] {
		base::call_delayed(kStartDelay, this, [=] {
			autoCheck();
		});
	}, _lifetime);
}

bool Syncer::automatic() const {
	const auto account = _account.get();
	return Get().cloudSettingsAutoSync()
		&& account
		&& account->ready()
		&& _remembered.valid();
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
	if (_busy != busy) {
		_busy = busy;
		changed();
	}
}

void Syncer::fail(const QString &text) {
	_busy = false;
	changed();
	_errors.fire_copy(text);
}

Syncer::Local Syncer::local() const {
	auto result = Local();
	result.all = QJsonDocument::fromJson(Get().syncSnapshot()).object();
	result.settings = PortableSettings(result.all);
	result.presets = Share::PresetLibrary().exportJson();
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
	_sender.request(
		Cloud::GetRequest(u"/v1/me/settings"_q),
		crl::guard(this, [=](const Cloud::Response &response) {
			applyRemote(response);
			done();
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			if (error.status == 404 && !error.is("feature_disabled")) {
				_remote = Remote();
				_remote.known = true;
				done();
			} else if (fail) {
				fail(error);
			}
		}));
}

void Syncer::refresh() {
	if (_loading) {
		return;
	}
	_loading = true;
	_error = QString();
	changed();
	fetch([=] {
		_loading = false;
		if (_remote.rev <= _remembered.rev) {
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
	const auto parsed = _remote.exists
		? ParseHeader(_remote.blob)
		: std::nullopt;
	if (password.isEmpty()) {
		// The key this device remembers fits a copy made with the same
		// salt (by this device or by one that has received from it).
		const auto fits = _remembered.valid()
			&& (!parsed
				|| (parsed->salt == _remembered.header.salt
					&& parsed->iterations == _remembered.header.iterations));
		done(
			fits ? _remembered.key : QByteArray(),
			fits ? _remembered.header : BlobHeader());
		return;
	}
	// The salt of the copy of the server is kept: the devices that have
	// the same password go on with the same key.
	const auto header = parsed
		? *parsed
		: BlobHeader{
			.iterations = kIterations,
			.salt = Cloud::RandomBytes(kSaltSize),
		};
	const auto weak = base::make_weak(this);
	crl::async([=] {
		auto key = DeriveKey(password, header);
		crl::on_main(weak, [=, key = std::move(key)] {
			done(key, header);
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
	const auto now = local();
	const auto blob = SealBlob(key, header, SerializePayload({
		.valid = true,
		.settings = now.settings,
		.presets = now.presets,
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
	}
	setBusy(true);
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
						if (!*confirmed) {
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
	Share::PresetLibrary().mergeJson(payload.presets);
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
	}
	setBusy(true);
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
					show->showToast(count
						? tr::lng_oblivion_sync_received(tr::now)
						: tr::lng_oblivion_sync_received_same(tr::now));
				}
			};
			if (!count) {
				// Nothing to replace: nothing to ask about.
				finish();
				return;
			} else if (!show->valid()) {
				setBusy(false);
				return;
			}
			const auto device = payload.device.isEmpty()
				? tr::lng_oblivion_sync_device_unknown(tr::now)
				: payload.device;
			const auto confirmed = std::make_shared<bool>(false);
			show->showBox(Ui::MakeConfirmBox({
				.text = tr::lng_oblivion_sync_receive_sure(
					tr::now,
					lt_count,
					count,
					lt_date,
					WhenText(_remote.updated),
					lt_device,
					device),
				.confirmed = [=](Fn<void()> close) {
					*confirmed = true;
					close();
					finish();
				},
				.cancelled = [=](Fn<void()> close) {
					close();
					if (const auto strong = weak.get()) {
						if (!*confirmed) {
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
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			strong->_sender.request(
				Cloud::DeleteRequest(u"/v1/me/settings"_q),
				crl::guard(strong, [=](const Cloud::Response &response) {
					strong->_remote = Remote();
					strong->_remote.known = true;
					strong->_remembered.rev = 0;
					strong->_remembered.hash = QByteArray();
					strong->_conflict = false;
					strong->saveRemembered();
					strong->changed();
					if (show->valid()) {
						show->showToast(
							tr::lng_oblivion_sync_erased(tr::now));
					}
				}),
				crl::guard(strong, [=](const Cloud::Error &error) {
					Cloud::ShowError(show, error);
				}));
		},
		.confirmText = tr::lng_box_delete(),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

// ---- The automatic mode. Everything here is quiet: no boxes, no
// toasts, a failure waits for the next change or the next launch.

void Syncer::localChanged() {
	if (_applying || !automatic()) {
		return;
	}
	_sendTimer.callOnce(kSendDelay);
}

void Syncer::autoSend() {
	if (!automatic() || _busy || _conflict) {
		return;
	}
	const auto now = local();
	if (now.hash == _remembered.hash) {
		return;
	}
	_busy = true;
	const auto done = [=] {
		_busy = false;
		changed();
	};
	const auto proceed = [=] {
		if (_remote.exists && _remote.rev > _remembered.rev) {
			// Another device has sent something this one has not seen,
			// and this one has changes of its own: the user decides.
			_conflict = true;
			done();
			return;
		}
		put(_remembered.key, _remembered.header, done, [=](
				const Cloud::Error &error) {
			if (error.status == 412) {
				_conflict = true;
			}
			done();
		});
	};
	fetch(proceed, [=](const Cloud::Error &error) {
		done();
	});
}

void Syncer::remoteChanged(int64 rev) {
	if (!automatic() || _busy || rev <= _remembered.rev) {
		return;
	}
	autoCheck();
}

void Syncer::autoCheck() {
	if (!automatic() || _busy) {
		return;
	}
	_busy = true;
	const auto done = [=] {
		_busy = false;
		changed();
	};
	fetch([=] {
		if (!_remote.exists || _remote.rev <= _remembered.rev) {
			done();
			// Changes made while the app was closed or offline.
			localChanged();
			return;
		}
		const auto was = local();
		const auto parsed = ParseHeader(_remote.blob);
		const auto fits = parsed
			&& (parsed->salt == _remembered.header.salt)
			&& (parsed->iterations == _remembered.header.iterations);
		const auto plain = fits
			? OpenBlob(_remembered.key, _remote.blob)
			: std::nullopt;
		const auto payload = plain ? ParsePayload(*plain) : Payload();
		if (!payload.valid || was.hash != _remembered.hash) {
			// Another password, or both devices have changed something.
			_conflict = true;
			done();
			return;
		}
		applyPayload(payload, was);
		_remembered.time = base::unixtime::now();
		saveRemembered();
		done();
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
	const auto header = BlobHeader{
		.iterations = kMinIterations,
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
	moreRounds.iterations = kMinIterations + 1;
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
	const auto parsed = ParseHeader(blob);
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
	check(!ParseHeader(QByteArray()), "header: empty");
	check(!ParseHeader(QByteArray(100, 'x')), "header: no magic");
	auto newer = blob;
	newer[4] = char(kBlobVersion + 1);
	check(!ParseHeader(newer), "header: a newer version is refused");
	auto slow = blob;
	slow[6] = char(0x7F);
	check(!ParseHeader(slow), "header: too many rounds are refused");
	auto fast = blob;
	fast[6] = fast[7] = fast[8] = char(0);
	fast[9] = char(1);
	check(!ParseHeader(fast), "header: too few rounds are refused");
	check(!ParseHeader(blob + QByteArray(kMaxBlob, 'x')),
		"header: a huge copy is refused");
	check(SealBlob(key.left(8), header, plain).isEmpty(),
		"blob: a bad key seals nothing");
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

	const auto payload = Payload{
		.valid = true,
		.settings = portable,
		.presets = presets,
		.device = u"MacBook"_q,
		.savedAt = 1759800000,
		.appBuild = 5000000,
	};
	const auto back = ParsePayload(SerializePayload(payload));
	check(back.valid
		&& back.settings == portable
		&& back.presets == presets
		&& back.device == u"MacBook"_q
		&& back.savedAt == 1759800000
		&& back.appBuild == 5000000,
		"payload: reads back the same");
	check(!ParsePayload(QByteArray("[]")).valid, "payload: not an object");
	check(!ParsePayload(QByteArray("{\"v\":2,\"settings\":{}}")).valid,
		"payload: a newer version is refused");
	check(!ParsePayload(QByteArray("{\"v\":1,\"settings\":5}")).valid,
		"payload: the settings must be an object");
	check(!ParsePayload(QByteArray("garbage")).valid, "payload: garbage");

	// The whole way: this device seals, another one opens and merges.
	const auto header = BlobHeader{
		.iterations = kMinIterations,
		.salt = QByteArray(kSaltSize, 's'),
	};
	const auto key = DeriveKey(u"secret password"_q, header);
	const auto blob = SealBlob(key, header, SerializePayload(payload));
	const auto otherKey = DeriveKey(u"secret password"_q, *ParseHeader(blob));
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
		.header = header,
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
		&& restored.header.iterations == header.iterations
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
	check.section("settings");
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
			return Box(SyncBox, SyncBoxArgs{
				.status = rpl::single(status),
				.errors = std::move(errors),
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
});

} // namespace

void Start(not_null<Main::Session*> session) {
	// The watcher of the automatic mode. Nothing is sent by it while the
	// account has not agreed, has no remembered key or the mode is off.
	SyncerFor(session);
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
	const auto update = Update::RunSelfTest(log);
	return !check.failed() && update;
}

} // namespace Oblivion::Sync
