/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud_ui.h"

#include "base/call_delayed.h"
#include "base/timer.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_cloud_social.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Oblivion::Cloud {
namespace {

constexpr auto kNameLimit = 64;
constexpr auto kCodeLimit = 16;
constexpr auto kCodeMinimum = 8;
constexpr auto kCodeFontSize = 30;
constexpr auto kCodeSpacing = 3;
constexpr auto kCodeRadius = 10;
constexpr auto kBulletSize = 5;
constexpr auto kBulletIndent = 15;
constexpr auto kBulletSkip = 4;
constexpr auto kDotSize = 9;
constexpr auto kTick = crl::time(1000);
constexpr auto kConnectingToastDelay = crl::time(1500);

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] style::margins RowPadding() {
	return st::boxRowPadding + style::margins(0, 0, 0, st::boxLittleSkip);
}

void Toast(const std::shared_ptr<Ui::Show> &show, const QString &text) {
	if (show && show->valid() && !text.isEmpty()) {
		show->showToast(text);
	}
}

[[nodiscard]] QString FormatLeft(int64 milliseconds) {
	const auto seconds = std::max(milliseconds, int64(0)) / 1000;
	return u"%1:%2"_q.arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}

[[nodiscard]] QColor StateColor(const StatusInfo &info) {
	if (info.unavailable) {
		return st::windowSubTextFg->c;
	}
	switch (info.state) {
	// Green in every theme: «good» text is blue in the night ones and
	// could not be told from «Подключение…».
	case State::Online: return st::settingsIconBg2->c;
	case State::Connecting:
	case State::NeedsLink: return st::windowActiveTextFg->c;
	case State::Offline:
	case State::UpgradeRequired:
	case State::Banned: return st::boxTextFgError->c;
	case State::NoConsent:
	case State::Disconnected: break;
	}
	return st::windowSubTextFg->c;
}

class StatusRow final : public Ui::RpWidget {
public:
	StatusRow(QWidget *parent, rpl::producer<StatusInfo> info);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	const not_null<Ui::FlatLabel*> _title;
	const not_null<Ui::FlatLabel*> _about;
	StatusInfo _info;

};

StatusRow::StatusRow(QWidget *parent, rpl::producer<StatusInfo> info)
: RpWidget(parent)
, _title(Ui::CreateChild<Ui::FlatLabel>(this, st::boxLabel))
, _about(Ui::CreateChild<Ui::FlatLabel>(this, st::boxDividerLabel)) {
	std::move(
		info
	) | rpl::on_next([=](const StatusInfo &value) {
		_info = value;
		_title->setMarkedText(tr::bold(StatusTitle(value)));
		_about->setText(StatusAbout(value));
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}, lifetime());
}

int StatusRow::resizeGetHeight(int newWidth) {
	const auto &padding = st::defaultBoxDividerLabelPadding;
	const auto left = padding.left() + Scaled(kDotSize) + st::boxLittleSkip;
	const auto available = std::max(newWidth - left - padding.right(), 1);
	_title->resizeToWidth(available);
	_title->moveToLeft(left, padding.top(), newWidth);
	_about->resizeToWidth(available);
	_about->moveToLeft(
		left,
		_title->y() + _title->height() + (st::boxLittleSkip / 2),
		newWidth);
	return _about->y() + _about->height() + padding.top();
}

void StatusRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto &padding = st::defaultBoxDividerLabelPadding;
	const auto size = Scaled(kDotSize);
	const auto line = st::boxLabel.style.font->height;
	const auto x = rtl() ? (width() - padding.left() - size) : padding.left();
	p.setPen(Qt::NoPen);
	p.setBrush(StateColor(_info));
	p.drawEllipse(x, _title->y() + (line - size) / 2, size, size);
}

// The link code, large and spaced, so that it is easy to read aloud and
// to type on another device, on a plate of its own. The same plate shows
// «Получаем код…» and what went wrong.
class CodeView final : public Ui::RpWidget {
public:
	explicit CodeView(QWidget *parent);

	void showCode(const QString &code, bool expired);
	void showText(const QString &text, bool error);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] static const style::font &CodeFont();

	QString _code;
	QString _text;
	bool _expired = false;
	bool _error = false;

};

CodeView::CodeView(QWidget *parent)
: RpWidget(parent) {
}

const style::font &CodeView::CodeFont() {
	static const auto result = style::font(
		Scaled(kCodeFontSize),
		st::semiboldFont->flags(),
		st::semiboldFont->family());
	return result;
}

void CodeView::showCode(const QString &code, bool expired) {
	_code = code;
	_text = QString();
	_expired = expired;
	_error = false;

	// The plate may have grown for a long text shown before the code.
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void CodeView::showText(const QString &text, bool error) {
	_code = QString();
	_text = text;
	_error = error;
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

int CodeView::resizeGetHeight(int newWidth) {
	const auto skip = st::boxLittleSkip;
	const auto inner = std::max(newWidth - 2 * skip, 1);
	const auto text = _text.isEmpty()
		? 0
		: QFontMetrics(st::normalFont->f).boundingRect(
			QRect(0, 0, inner, 1 << 16),
			Qt::AlignHCenter | Qt::TextWordWrap,
			_text).height();
	return std::max(CodeFont()->height, text) + 2 * skip;
}

void CodeView::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	{
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = Scaled(kCodeRadius);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
	}
	if (!_code.isEmpty()) {
		auto font = CodeFont()->f;
		font.setLetterSpacing(QFont::AbsoluteSpacing, Scaled(kCodeSpacing));
		p.setFont(font);
		p.setPen(_expired ? st::windowSubTextFg : st::windowActiveTextFg);
		p.drawText(rect(), Qt::AlignCenter, _code);
	} else if (!_text.isEmpty()) {
		const auto skip = st::boxLittleSkip;
		p.setFont(st::normalFont);
		p.setPen(_error ? st::boxTextFgError : st::windowSubTextFg);
		p.drawText(
			rect().marginsRemoved({ skip, skip, skip, skip }),
			Qt::AlignCenter | Qt::TextWordWrap,
			_text);
	}
}

// A line of a list: a small dot and a text that wraps next to it, not
// under it. The consent box tells what is sent with such lines.
class BulletRow final : public Ui::RpWidget {
public:
	BulletRow(QWidget *parent, const TextWithEntities &text);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	const not_null<Ui::FlatLabel*> _label;

};

BulletRow::BulletRow(QWidget *parent, const TextWithEntities &text)
: RpWidget(parent)
, _label(Ui::CreateChild<Ui::FlatLabel>(this, st::defaultFlatLabel)) {
	_label->setMarkedText(text);
}

int BulletRow::resizeGetHeight(int newWidth) {
	const auto left = Scaled(kBulletIndent);
	_label->resizeToWidth(std::max(newWidth - left, 1));
	_label->moveToLeft(left, 0, newWidth);
	return _label->height();
}

void BulletRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto size = Scaled(kBulletSize);
	const auto line = st::defaultFlatLabel.style.font->height;
	const auto x = rtl() ? (width() - Scaled(2) - size) : Scaled(2);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowSubTextFg);
	p.drawEllipse(QRectF(x, (line - size) / 2. + Scaled(1), size, size));
}

// The lines of a text as separate texts, with their formatting kept.
[[nodiscard]] std::vector<TextWithEntities> SplitLines(
		const TextWithEntities &text) {
	auto result = std::vector<TextWithEntities>();
	const auto size = int(text.text.size());
	auto from = 0;
	while (from < size) {
		const auto found = int(text.text.indexOf(QChar('\n'), from));
		const auto till = (found < 0) ? size : found;
		if (till > from) {
			result.push_back(Ui::Text::Mid(text, from, till - from));
		}
		from = till + 1;
	}
	return result;
}

struct ConsentArgs {
	QString account;
	QString name;
	Fn<void(QString name)> agree;
	Fn<void()> cancel;
};

void ConsentBox(not_null<Ui::GenericBox*> box, ConsentArgs &&args) {
	box->setTitle(tr::lng_oblivion_cloud_title());
	box->setWidth(st::boxWideWidth);

	const auto paragraph = [&](TextWithEntities text) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(std::move(text)),
				st::boxLabel),
			RowPadding());
	};
	// A heading and a list under it: every line of the text is a row
	// with a dot, so that it is read at a glance, not as a wall of text.
	const auto listed = [&](const QString &title, TextWithEntities text) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(tr::bold(title)),
				st::boxLabel),
			st::boxRowPadding
				+ style::margins(0, 0, 0, Scaled(kBulletSkip) / 2));
		const auto lines = SplitLines(text);
		const auto count = int(lines.size());
		for (auto i = 0; i != count; ++i) {
			const auto last = (i + 1 == count);
			box->addRow(
				object_ptr<BulletRow>(box, lines[i]),
				st::boxRowPadding + style::margins(
					0,
					0,
					0,
					last ? st::boxLittleSkip : Scaled(kBulletSkip)));
		}
	};

	// Four facts in plain words, in the order a person asks them: what it
	// is and whose server, what goes there, what never does, how to leave.
	paragraph({ tr::lng_oblivion_cloud_consent_about(tr::now) });
	listed(
		tr::lng_oblivion_cloud_consent_sent_title(tr::now),
		tr::lng_oblivion_cloud_consent_sent(
			tr::now,
			lt_name,
			tr::bold(args.account.simplified()),
			tr::marked));
	listed(
		tr::lng_oblivion_cloud_consent_not_title(tr::now),
		{ tr::lng_oblivion_cloud_consent_not(tr::now) });
	paragraph({ tr::lng_oblivion_cloud_consent_off(tr::now) });

	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_cloud_consent_name(),
			args.name.left(kNameLimit)),
		RowPadding());
	field->setMaxLength(kNameLimit);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_oblivion_cloud_consent_name_about(),
			st::boxDividerLabel),
		RowPadding());

	const auto agreed = box->lifetime().make_state<bool>(false);
	const auto agree = std::move(args.agree);
	const auto cancel = std::move(args.cancel);
	const auto submit = [=] {
		if (*agreed) {
			return;
		}
		*agreed = true;
		const auto name = field->getLastText().simplified().left(kNameLimit);
		const auto callback = agree;
		box->closeBox();
		if (callback) {
			callback(name);
		}
	};
	field->submits() | rpl::on_next(submit, field->lifetime());
	box->boxClosing() | rpl::on_next([=] {
		if (!*agreed && cancel) {
			cancel();
		}
	}, box->lifetime());

	box->addButton(tr::lng_oblivion_cloud_consent_agree(), submit);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

struct LinkCodeArgs {
	Fn<void(Fn<void(const LinkCode &code)> done, Fail fail)> request;
	Fn<int64()> now;
};

void LinkCodeBox(not_null<Ui::GenericBox*> box, LinkCodeArgs &&args) {
	struct State {
		LinkCode code;
		QString error;
		rpl::variable<QString> expires;
		base::Timer timer;
		bool loading = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto request = std::move(args.request);
	const auto now = std::move(args.now);

	box->setTitle(tr::lng_oblivion_cloud_link_title());
	box->setWidth(st::boxWideWidth);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_oblivion_cloud_link_about(),
			st::boxLabel),
		RowPadding());
	const auto view = box->addRow(object_ptr<CodeView>(box), RowPadding());

	// How long the code lives: a caption in the middle, under the plate
	// with the code it is about.
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->expires.value(),
			st::defaultPeerListAbout),
		RowPadding(),
		style::al_justify);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_oblivion_cloud_link_warning(),
			st::boxDividerLabel),
		RowPadding());

	const auto refresh = [=] {
		if (state->loading) {
			view->showText(tr::lng_oblivion_cloud_link_loading(tr::now), false);
			state->expires = QString();
		} else if (!state->error.isEmpty()) {
			view->showText(state->error, true);
			state->expires = QString();
		} else if (!state->code.code.isEmpty()) {
			const auto left = state->code.expiresAt - (now ? now() : Now());
			view->showCode(state->code.code, left <= 0);
			state->expires = (left > 0)
				? tr::lng_oblivion_cloud_link_expires(
					tr::now,
					lt_time,
					FormatLeft(left))
				: tr::lng_oblivion_cloud_link_expired(tr::now);
		}
	};
	const auto load = [=] {
		if (state->loading || !request) {
			return;
		}
		state->loading = true;
		state->error = QString();
		state->code = LinkCode();
		refresh();
		request(crl::guard(box, [=](const LinkCode &code) {
			state->loading = false;
			state->code = code;
			refresh();
		}), crl::guard(box, [=](const Error &error) {
			state->loading = false;
			state->error = ErrorText(error);
			refresh();
		}));
	};
	state->timer.setCallback(refresh);
	state->timer.callEach(kTick);
	load();

	box->addButton(tr::lng_oblivion_cloud_link_copy(), [=] {
		if (state->code.code.isEmpty()) {
			return;
		}
		QGuiApplication::clipboard()->setText(state->code.code);
		box->uiShow()->showToast(tr::lng_oblivion_cloud_link_copied(tr::now));
	});
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
	box->addLeftButton(tr::lng_oblivion_cloud_link_new(), load);
}

struct EnterCodeArgs {
	Fn<void(const QString &code, Fn<void()> done, Fail fail)> submit;
	Fn<void()> linked;
	Fn<void()> cancel;
	QString code;
	QString error;
	bool reserved = false;
};

// The server answers "already registered" both when another device holds
// the account and when no device is left at all (the account is kept, but
// the last device has logged out of Telegram). It marks the second case,
// and only the developer can give a code then.
[[nodiscard]] bool ReservedError(const Error &error) {
	return error.is("already_registered")
		&& error.details.value(u"reserved"_q).toBool();
}

[[nodiscard]] QString CodeErrorText(const Error &error) {
	return error.is("invalid_code")
		? tr::lng_oblivion_cloud_code_wrong(tr::now)
		: (error.is("limit_reached") && error.detail("limit") == u"devices"_q)
		? tr::lng_oblivion_cloud_code_devices(tr::now)
		: ErrorText(error);
}

void EnterCodeBox(not_null<Ui::GenericBox*> box, EnterCodeArgs &&args) {
	struct State {
		rpl::variable<QString> hint;
		bool sending = false;
		bool linked = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto submit = std::move(args.submit);
	const auto linked = std::move(args.linked);
	const auto cancel = std::move(args.cancel);
	const auto reserved = args.reserved;

	box->setTitle(tr::lng_oblivion_cloud_code_title());
	box->setWidth(st::boxWideWidth);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			(reserved
				? tr::lng_oblivion_cloud_code_reserved_about()
				: tr::lng_oblivion_cloud_code_about()),
			st::boxLabel),
		RowPadding());
	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_cloud_code_placeholder(),
			args.code.left(kCodeLimit)),
		RowPadding());
	field->setMaxLength(kCodeLimit);
	const auto hint = box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->hint.value(),
			st::boxDividerLabel),
		RowPadding());
	const auto showHint = [=](const QString &error) {
		if (error.isEmpty()) {
			hint->setTextColorOverride(std::nullopt);
			state->hint = reserved
				? QString()
				: tr::lng_oblivion_cloud_code_lost(tr::now);
		} else {
			hint->setTextColorOverride(st::boxTextFgError->c);
			state->hint = error;
		}
	};
	showHint(args.error);

	const auto send = [=] {
		const auto code = field->getLastText().trimmed();
		if (state->sending || state->linked) {
			return;
		} else if (code.size() < kCodeMinimum || !submit) {
			field->showError();
			return;
		}
		state->sending = true;
		showHint(QString());
		submit(code, crl::guard(box, [=] {
			state->sending = false;
			state->linked = true;
			const auto callback = linked;
			box->closeBox();
			if (callback) {
				callback();
			}
		}), crl::guard(box, [=](const Error &error) {
			state->sending = false;
			showHint(CodeErrorText(error));
			field->showError();
		}));
	};
	field->submits() | rpl::on_next(send, field->lifetime());
	field->changes() | rpl::on_next([=] {
		if (!state->sending) {
			showHint(QString());
		}
	}, field->lifetime());
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	box->boxClosing() | rpl::on_next([=] {
		if (!state->linked && cancel) {
			cancel();
		}
	}, box->lifetime());

	box->addButton(tr::lng_oblivion_cloud_code_submit(), send);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

void ShowEnterCodeFlow(
		base::weak_ptr<Account> weak,
		std::shared_ptr<Ui::Show> show,
		Fn<void()> done,
		Fn<void()> declined) {
	const auto strong = weak.get();
	show->showBox(Box(EnterCodeBox, EnterCodeArgs{
		.submit = [=](const QString &code, Fn<void()> linked, Fail fail) {
			if (const auto account = weak.get()) {
				account->redeemLinkCode(code, linked, fail);
			}
		},
		.linked = [=] {
			Toast(show, tr::lng_oblivion_cloud_code_done(tr::now));
			if (done) {
				done();
			}
		},
		.cancel = declined,
		.reserved = strong && ReservedError(strong->lastError()),
	}));
}

// Waits for the registration that is on its way: done() when the account
// is ready, the link code box when the id turned out to be bound to
// another device, a toast and declined() on anything else.
void WaitReady(
		base::weak_ptr<Account> weak,
		std::shared_ptr<Ui::Show> show,
		Fn<void()> done,
		Fn<void()> declined) {
	const auto account = weak.get();
	if (!account) {
		return;
	} else if (account->ready()) {
		if (done) {
			done();
		}
		return;
	}
	const auto lifetime = std::make_shared<rpl::lifetime>();
	const auto pending = std::make_shared<bool>(true);
	const auto finish = [=](Fn<void()> callback) {
		*pending = false;
		lifetime->destroy();
		if (callback) {
			callback();
		}
	};
	base::call_delayed(kConnectingToastDelay, [=] {
		if (*pending) {
			Toast(show, tr::lng_oblivion_cloud_status_connecting(tr::now));
		}
	});
	account->readyValue(
	) | rpl::filter(
		rpl::mappers::_1
	) | rpl::take(1) | rpl::on_next([=] {
		finish(done);
	}, *lifetime);
	account->connectFailed(
	) | rpl::take(1) | rpl::on_next([=](const Error &error) {
		const auto strong = weak.get();
		if (strong && strong->state() == State::NeedsLink) {
			*pending = false;
			lifetime->destroy();
			ShowEnterCodeFlow(weak, show, done, declined);
		} else {
			ShowError(show, error);
			finish(declined);
		}
	}, *lifetime);
	account->stateValue(
	) | rpl::filter([](State state) {
		return (state == State::NoConsent) || (state == State::Disconnected);
	}) | rpl::take(1) | rpl::on_next([=] {
		if (const auto strong = weak.get()) {
			ShowError(show, strong->lastError());
		}
		finish(declined);
	}, *lifetime);

	// The subscriptions above keep themselves alive through the lifetime
	// they are stored in, and only finish() lets go of it. If the account
	// goes away first (a logout while the box waits), its state is over
	// without any of them being called, so this is the one that releases
	// everything then. Nobody is told: the account is gone.
	account->stateValue(
	) | rpl::on_done([=] {
		finish(nullptr);
	}, *lifetime);
}

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

class StatusSamples final : public Ui::RpWidget {
public:
	explicit StatusSamples(QWidget *parent);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	std::vector<not_null<Ui::RpWidget*>> _rows;

};

StatusSamples::StatusSamples(QWidget *parent)
: RpWidget(parent) {
	const auto add = [&](StatusInfo info) {
		_rows.push_back(CreateStatusRow(
			this,
			rpl::single(std::move(info))).release());
	};
	const auto name = SampleText("Аня", "Anna");
	add({ .state = State::Online, .name = name });
	add({
		.state = State::Online,
		.motd = SampleText(
			"Сегодня ночью сервер перезапустится на пару минут.",
			"The server restarts for a couple of minutes tonight."),
	});
	add({ .state = State::Connecting });
	add({ .state = State::Offline, .name = name });
	add({ .state = State::NoConsent });
	add({ .state = State::Disconnected });
	add({ .state = State::NeedsLink });
	add({ .state = State::NeedsLink, .reserved = true });
	add({ .state = State::UpgradeRequired });
	add({ .state = State::Banned });
	add({ .state = State::NoConsent, .unavailable = true });
}

int StatusSamples::resizeGetHeight(int newWidth) {
	auto top = 0;
	for (const auto &row : _rows) {
		row->resizeToWidth(newWidth);
		row->moveToLeft(0, top, newWidth);
		top += row->height();
	}
	return top;
}

void StatusSamples::paintEvent(QPaintEvent *e) {
	QPainter(this).fillRect(e->rect(), st::windowBg);
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto size = QSize(st::boxWideWidth * 2, 0);
	RegisterBoxScene(u"cloud_consent"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(ConsentBox, ConsentArgs{
			.account = SampleText("Аня Смирнова", "Anna Smirnova"),
			.name = SampleText("Аня", "Anna"),
		});
	});
	RegisterBoxScene(u"cloud_consent_long_name"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(ConsentBox, ConsentArgs{
			.account = SampleText(
				"Константин Константинопольский-Задунайский",
				"Constantine Constantinopolsky-Zadunaisky"),
			.name = SampleText("Константин", "Constantine"),
		});
	});

	const auto fixedNow = int64(1791327935000);
	const auto linkScene = [=](
			const QString &name,
			Fn<void(Fn<void(const LinkCode&)>, Fail)> request) {
		RegisterBoxScene(name, size, [=](std::shared_ptr<Ui::Show> show) {
			return Box(LinkCodeBox, LinkCodeArgs{
				.request = request,
				.now = [=] { return fixedNow; },
			});
		});
	};
	linkScene(u"cloud_link_code"_q, [=](
			Fn<void(const LinkCode&)> done,
			Fail fail) {
		done({ .code = u"K7QM-2XPA"_q, .expiresAt = fixedNow + 581'000 });
	});
	linkScene(u"cloud_link_code_expired"_q, [=](
			Fn<void(const LinkCode&)> done,
			Fail fail) {
		done({ .code = u"K7QM-2XPA"_q, .expiresAt = fixedNow - 1000 });
	});
	linkScene(u"cloud_link_code_loading"_q, [](
			Fn<void(const LinkCode&)> done,
			Fail fail) {
	});
	linkScene(u"cloud_link_code_error"_q, [](
			Fn<void(const LinkCode&)> done,
			Fail fail) {
		fail({ .type = Error::Type::Network });
	});

	RegisterBoxScene(u"cloud_enter_code"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(EnterCodeBox, EnterCodeArgs{});
	});
	RegisterBoxScene(u"cloud_enter_code_wrong"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(EnterCodeBox, EnterCodeArgs{
			.code = u"K7QM-2XPB"_q,
			.error = tr::lng_oblivion_cloud_code_wrong(tr::now),
		});
	});
	RegisterBoxScene(u"cloud_enter_code_reserved"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(EnterCodeBox, EnterCodeArgs{ .reserved = true });
	});

	RegisterBoxScene(u"cloud_enable_again"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_cloud_enable_again(),
			.confirmText = tr::lng_oblivion_cloud_enable(),
		});
	});
	RegisterBoxScene(u"cloud_confirm_off"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_cloud_off_sure(),
			.confirmText = tr::lng_oblivion_cloud_off_confirm(),
			.title = tr::lng_oblivion_cloud_off_title(),
		});
	});
	RegisterBoxScene(u"cloud_confirm_delete"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_cloud_delete_sure(),
			.confirmText = tr::lng_oblivion_cloud_delete_confirm(),
			.confirmStyle = &st::attentionBoxButton,
			.title = tr::lng_oblivion_cloud_delete_title(),
		});
	});
	RegisterBoxScene(u"cloud_delete_need"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeInformBox(tr::lng_oblivion_cloud_delete_need());
	});

	RegisterScene(
		u"cloud_status_rows"_q,
		QSize(st::boxWideWidth + st::boxLittleSkip * 4, 0),
		[](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<StatusSamples>(parent.get());
		});
});

} // namespace

QString ErrorText(const Error &error) {
	using Type = Error::Type;
	switch (error.type) {
	case Type::None:
	case Type::Cancelled:
		return QString();
	case Type::NotConnected:
		return tr::lng_oblivion_cloud_error_off(tr::now);
	case Type::Network:
	case Type::Timeout:
		return tr::lng_oblivion_cloud_error_network(tr::now);
	case Type::Tls:
		return tr::lng_oblivion_cloud_error_tls(tr::now);
	case Type::File:
		return tr::lng_oblivion_cloud_error_file(tr::now);
	case Type::Protocol:
		return tr::lng_oblivion_cloud_error_server(tr::now);
	case Type::Http:
		break;
	}
	const auto status = error.status;
	return (status == 429)
		? tr::lng_oblivion_cloud_error_rate(tr::now)
		: (status == 426)
		? tr::lng_oblivion_cloud_error_upgrade(tr::now)
		: error.is("banned")
		? tr::lng_oblivion_cloud_error_banned(tr::now)
		: error.is("invalid_code")
		? tr::lng_oblivion_cloud_code_wrong(tr::now)
		: ReservedError(error) // No device at all: not «на другом».
		? tr::lng_oblivion_cloud_status_reserved_about(tr::now)
		: (error.is("already_registered")
			|| error.is("verification_required"))
		? tr::lng_oblivion_cloud_error_bound(tr::now)
		: (status == 401)
		? tr::lng_oblivion_cloud_error_off(tr::now)
		: (status == 403)
		? tr::lng_oblivion_cloud_error_forbidden(tr::now)
		: (status == 404 || status == 410)
		? tr::lng_oblivion_cloud_error_not_found(tr::now)
		: (status == 413)
		? tr::lng_oblivion_cloud_error_too_large(tr::now)
		: (status == 507 || (status == 503 && error.is("limit_reached")))
		? tr::lng_oblivion_cloud_error_full(tr::now)
		: (error.is("limit_reached") || error.is("room_full"))
		? tr::lng_oblivion_cloud_error_limit(tr::now)
		: (status == 502 || status == 503 || status == 504)
		? tr::lng_oblivion_cloud_error_network(tr::now)
		: tr::lng_oblivion_cloud_error_server(tr::now);
}

void ShowError(std::shared_ptr<Ui::Show> show, const Error &error) {
	const auto text = ErrorText(error);
	if (show && show->valid() && !text.isEmpty()) {
		show->showToast(text);
	}
}

rpl::producer<StatusInfo> StatusValue(not_null<Account*> account) {
	const auto weak = base::make_weak(account);
	return rpl::merge(
		account->stateValue() | rpl::to_empty,
		account->meUpdated()
	) | rpl::map([=] {
		auto result = StatusInfo();
		const auto strong = weak.get();
		if (!strong) {
			return result;
		}
		result.state = strong->state();
		result.name = strong->me().valid()
			? strong->me().name
			: strong->chosenName();
		result.unavailable = !strong->available();
		result.reserved = (result.state == State::NeedsLink)
			&& ReservedError(strong->lastError());
		const auto &hello = strong->hello();
		result.motd = CurrentLanguageIsRussian()
			? hello.motdRu
			: hello.motdEn;
		return result;
	});
}

QString StatusTitle(const StatusInfo &info) {
	if (info.unavailable) {
		return tr::lng_oblivion_cloud_status_none(tr::now);
	}
	switch (info.state) {
	case State::NoConsent:
		return tr::lng_oblivion_cloud_status_none(tr::now);
	case State::Disconnected:
		return tr::lng_oblivion_cloud_status_off(tr::now);
	case State::NeedsLink:
		return tr::lng_oblivion_cloud_status_link(tr::now);
	case State::Connecting:
		return tr::lng_oblivion_cloud_status_connecting(tr::now);
	case State::Online:
		return tr::lng_oblivion_cloud_status_online(tr::now);
	case State::Offline:
		return tr::lng_oblivion_cloud_status_offline(tr::now);
	case State::UpgradeRequired:
		return tr::lng_oblivion_cloud_status_upgrade(tr::now);
	case State::Banned:
		return tr::lng_oblivion_cloud_status_banned(tr::now);
	}
	return QString();
}

QString StatusAbout(const StatusInfo &info) {
	if (info.unavailable) {
		return tr::lng_oblivion_cloud_error_test(tr::now);
	}
	const auto named = [&] {
		return info.name.isEmpty()
			? tr::lng_oblivion_cloud_status_online_noname(tr::now)
			: tr::lng_oblivion_cloud_status_online_as(
				tr::now,
				lt_name,
				info.name);
	};
	switch (info.state) {
	case State::NoConsent:
		return tr::lng_oblivion_cloud_status_none_about(tr::now);
	case State::Disconnected:
		return tr::lng_oblivion_cloud_status_off_about(tr::now);
	case State::NeedsLink:
		return info.reserved
			? tr::lng_oblivion_cloud_status_reserved_about(tr::now)
			: tr::lng_oblivion_cloud_status_link_about(tr::now);
	case State::Connecting:
		return tr::lng_oblivion_cloud_status_connecting_about(tr::now);
	case State::Online:
		return info.motd.isEmpty() ? named() : (named() + '\n' + info.motd);
	case State::Offline:
		return tr::lng_oblivion_cloud_status_offline_about(tr::now);
	case State::UpgradeRequired:
		return tr::lng_oblivion_cloud_error_upgrade(tr::now);
	case State::Banned:
		return tr::lng_oblivion_cloud_error_banned(tr::now);
	}
	return QString();
}

object_ptr<Ui::RpWidget> CreateStatusRow(
		QWidget *parent,
		rpl::producer<StatusInfo> info) {
	return object_ptr<StatusRow>(parent, std::move(info));
}

void RequireConsent(
		not_null<Window::SessionController*> controller,
		Fn<void()> done,
		Fn<void()> declined) {
	RequireConsent(
		&controller->session(),
		controller->uiShow(),
		std::move(done),
		std::move(declined));
}

void RequireConsent(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		Fn<void()> done,
		Fn<void()> declined) {
	const auto account = &For(session);
	if (account->ready()) {
		if (done) {
			done();
		}
		return;
	}
	const auto weak = base::make_weak(account);
	const auto refuse = [=](const QString &text) {
		if (show && show->valid() && !text.isEmpty()) {
			show->showToast(text);
		}
		if (declined) {
			declined();
		}
	};
	if (!show || !show->valid()) {
		if (declined) {
			declined();
		}
		return;
	} else if (!account->available()) {
		refuse(tr::lng_oblivion_cloud_error_test(tr::now));
		return;
	}
	const auto wait = [=] {
		WaitReady(weak, show, done, declined);
	};
	switch (account->state()) {
	case State::NoConsent: {
		const auto user = session->user();
		show->showBox(Box(ConsentBox, ConsentArgs{
			.account = user->name(),
			.name = user->firstName,
			.agree = [=](QString name) {
				if (const auto strong = weak.get()) {
					strong->agree(name);
					wait();
				}
			},
			.cancel = declined,
		}));
	} break;
	case State::Disconnected:
		show->showBox(Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_cloud_enable_again(),
			.confirmed = [=](Fn<void()> close) {
				close();
				if (const auto strong = weak.get()) {
					strong->switchOn();
					wait();
				}
			},
			.cancelled = [=](Fn<void()> close) {
				close();
				if (declined) {
					declined();
				}
			},
			.confirmText = tr::lng_oblivion_cloud_enable(),
		}));
		break;
	case State::NeedsLink:
		ShowEnterCodeFlow(weak, show, done, declined);
		break;
	case State::Connecting:
		wait();
		break;
	case State::Online:
	case State::Offline:
		// Not ready and not connecting: the last attempt has failed
		// and the next one waits for its turn. A click is a reason to
		// try right now.
		account->switchOn();
		wait();
		break;
	case State::UpgradeRequired:
		refuse(tr::lng_oblivion_cloud_error_upgrade(tr::now));
		break;
	case State::Banned:
		refuse(tr::lng_oblivion_cloud_error_banned(tr::now));
		break;
	}
}

rpl::producer<bool> EnabledValue(not_null<Main::Session*> session) {
	return For(session).stateValue() | rpl::map([](State state) {
		return (state != State::NoConsent) && (state != State::Disconnected);
	}) | rpl::distinct_until_changed();
}

void ToggleFromSettings(not_null<Window::SessionController*> controller) {
	const auto account = &For(&controller->session());
	const auto state = account->state();
	const auto weak = base::make_weak(account);
	const auto session = base::make_weak(&controller->session());
	const auto show = controller->uiShow();
	if (state == State::NoConsent || state == State::Disconnected) {
		RequireConsent(controller, [=] {
			Toast(show, tr::lng_oblivion_cloud_on_done(tr::now));
		});
		return;
	}
	show->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_cloud_off_sure(),
		.confirmed = [=](Fn<void()> close) {
			close();
			const auto alive = session.get();
			if (!alive) {
				return;
			}
			// The track that was told to the audience («слушает») is
			// taken back first: after switchOff() nothing can be sent.
			// The callback comes from the event loop, in 1.2 seconds at
			// the latest, see Social::StopPublishing().
			Social::StopPublishing(alive, [=] {
				if (const auto strong = weak.get()) {
					strong->switchOff();
					Toast(show, tr::lng_oblivion_cloud_off_done(tr::now));
				}
			});
		},
		.confirmText = tr::lng_oblivion_cloud_off_confirm(),
		.title = tr::lng_oblivion_cloud_off_title(),
	}));
}

void ShowLinkDevice(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(&For(&controller->session()));
	const auto show = controller->uiShow();
	RequireConsent(controller, [=] {
		show->showBox(Box(LinkCodeBox, LinkCodeArgs{
			.request = [=](Fn<void(const LinkCode&)> done, Fail fail) {
				if (const auto strong = weak.get()) {
					strong->createLinkCode(done, fail);
				}
			},
		}));
	});
}

void ShowEnterCode(not_null<Window::SessionController*> controller) {
	const auto show = controller->uiShow();
	if (For(&controller->session()).ready()) {
		Toast(show, tr::lng_oblivion_cloud_on_done(tr::now));
		return;
	}
	RequireConsent(controller, nullptr);
}

void ShowDeleteData(not_null<Window::SessionController*> controller) {
	const auto account = &For(&controller->session());
	const auto weak = base::make_weak(account);
	const auto show = controller->uiShow();
	if (!account->ready()) {
		show->showBox(
			Ui::MakeInformBox(tr::lng_oblivion_cloud_delete_need()));
		return;
	}
	show->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_cloud_delete_sure(),
		.confirmed = [=](Fn<void()> close) {
			close();
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			strong->deleteData([=] {
				Toast(show, tr::lng_oblivion_cloud_delete_done(tr::now));
			}, [=](const Error &error) {
				ShowError(show, error);
			});
		},
		.confirmText = tr::lng_oblivion_cloud_delete_confirm(),
		.confirmStyle = &st::attentionBoxButton,
		.title = tr::lng_oblivion_cloud_delete_title(),
	}));
}

} // namespace Oblivion::Cloud
