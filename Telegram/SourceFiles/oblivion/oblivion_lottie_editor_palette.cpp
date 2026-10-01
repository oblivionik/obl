/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie_editor_palette.h"

#include "lang/lang_keys.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/effects/animation_value.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/color_editor.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>

#include <cmath>

namespace Oblivion::LottieEdit {
namespace {

constexpr auto kPadding = 12;
constexpr auto kSwatch = 30;
constexpr auto kSwatchGap = 8;
constexpr auto kSwatchRadius = 8;
constexpr auto kCountSkip = 3;
constexpr auto kRowSkip = 8;
constexpr auto kActiveSwatch = 44;
constexpr auto kHexWidth = 96;
constexpr auto kHexHeight = 28;
constexpr auto kSliderHeight = 24;
constexpr auto kSliderHandle = 9;
constexpr auto kTooltipDelay = 800;

// A new undo step is assumed after this pause between previews (the
// controller merges them for 1.5 s, the estimate has to be an upper one).
constexpr auto kStepGuess = crl::time(1000);

// Undo steps rollback() takes beyond the estimate, only for commands of
// the session (a safety net, the estimate is an upper bound).
constexpr auto kExtraRollback = 64;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QString SignedText(int value, const QString &suffix) {
	const auto number = QString::number(std::abs(value));
	const auto sign = (value > 0)
		? u"+"_q
		: (value < 0)
		? QString(QChar(0x2212))
		: QString();
	return sign + number + suffix;
}

// The box button with the text inset equal to the panel padding: placed
// at the left edge, its text starts at the left edge of the labels.
[[nodiscard]] const style::RoundButton &ResetButtonStyle() {
	static const auto result = [] {
		auto st = st::defaultBoxButton;
		st.width = -2 * Scaled(kPadding);
		return st;
	}();
	return result;
}

// Depth-first search for the object with the id, returns the root with
// that object replaced (structurally shared) or nullopt if not found.
[[nodiscard]] std::optional<Json::Value> ReplaceNodeIn(
		const Json::Value &value,
		NodeId id,
		const Json::Value &replacement) {
	if (value.isObject()) {
		if (value.id() == id) {
			return replacement;
		}
		for (const auto &member : value.members()) {
			const auto &child = member.value;
			if (!child.isObject() && !child.isArray()) {
				continue;
			}
			if (auto replaced = ReplaceNodeIn(child, id, replacement)) {
				return value.with(member.key, std::move(*replaced));
			}
		}
	} else if (value.isArray()) {
		const auto &items = value.items();
		if (!items.empty() && items.front().isNumber()) {
			return std::nullopt;
		}
		for (auto i = 0; i != int(items.size()); ++i) {
			const auto &child = items[i];
			if (!child.isObject() && !child.isArray()) {
				continue;
			}
			if (auto replaced = ReplaceNodeIn(child, id, replacement)) {
				return value.withItem(i, std::move(*replaced));
			}
		}
	}
	return std::nullopt;
}

} // namespace

// Shared pieces.

const style::InputField &PanelFieldStyle() {
	static const auto result = [] {
		// At rest it looks like the inspector value fields (a filled
		// rounded rect, no border), focused like an input: the window
		// background and an accent border.
		auto st = st::defaultInputField;
		st.textBg = st::windowBgOver;
		st.textBgActive = st::windowBg;
		st.textMargins = QMargins(
			Scaled(8),
			Scaled(5),
			Scaled(8),
			Scaled(3));
		st.placeholderMargins = QMargins();
		st.placeholderScale = 0.;
		st.placeholderShift = 0;
		st.placeholderFont = st::normalFont;
		st.borderFg = st::windowBgOver;
		st.borderFgActive = st::activeLineFg;
		st.borderFgError = st::activeLineFgError;
		st.border = std::max(Scaled(1), 1);
		st.borderActive = st.border;
		st.borderRadius = Scaled(6);
		st.borderDenominator = 1;
		st.style = st::defaultTextStyle;
		st.width = 0;
		st.widthMin = 0;
		st.heightMin = Scaled(kHexHeight);
		st.heightMax = Scaled(kHexHeight);
		return st;
	}();
	return result;
}

const style::InputField &PanelSearchStyle() {
	static const auto result = [] {
		auto st = st::dialogsFilter;
		// The right margin stays, the clear button (dialogsCancelSearch)
		// is shown there.
		st.textMargins = QMargins(
			Scaled(12),
			st.textMargins.top(),
			st.textMargins.right(),
			st.textMargins.bottom());
		return st;
	}();
	return result;
}

const style::font &PanelSmallFont() {
	static const auto result = style::font(
		Scaled(11),
		st::normalFont->flags(),
		st::normalFont->family());
	return result;
}

QString BindShortWords(QString text) {
	const auto dash = [](QChar ch) {
		return (ch == QChar(0x2014)) || (ch == QChar(0x2013));
	};
	auto letters = 0; // Letters in the word right before the position.
	for (auto i = 0, count = int(text.size()); i != count; ++i) {
		const auto ch = text[i];
		if (ch.isLetter()) {
			++letters;
			continue;
		} else if (ch == QChar(' ')) {
			const auto next = (i + 1 < count) ? text[i + 1] : QChar();
			if ((letters > 0 && letters <= 2) || dash(next)) {
				text[i] = QChar::Nbsp;
			}
		}
		letters = 0;
	}
	return text;
}

void FieldHost::keyPressEvent(QKeyEvent *e) {
	const auto key = e->key();
	if (key == Qt::Key_Escape
		|| key == Qt::Key_Return
		|| key == Qt::Key_Enter) {
		e->accept();
		return;
	}
	RpWidget::keyPressEvent(e);
}

QString ColorHex(const QColor &color) {
	return u"#%1%2%3"_q
		.arg(color.red(), 2, 16, QChar('0'))
		.arg(color.green(), 2, 16, QChar('0'))
		.arg(color.blue(), 2, 16, QChar('0'))
		.toUpper();
}

QColor ParseColorHex(QString text) {
	text = text.trimmed().remove(' ');
	if (text.startsWith('#')) {
		text = text.mid(1);
	}
	if (text.size() == 3) {
		auto expanded = QString();
		for (const auto ch : text) {
			expanded += ch;
			expanded += ch;
		}
		text = expanded;
	}
	if (text.size() != 6) {
		return QColor();
	}
	auto ok = false;
	const auto value = text.toUInt(&ok, 16);
	return ok ? QColor::fromRgb(QRgb(0xFF000000U | value)) : QColor();
}

void PaintSwatch(
		QPainter &p,
		QRectF rect,
		const QColor &color,
		double radius,
		bool selected) {
	auto hq = PainterHighQualityEnabler(p);
	const auto line = double(st::lineWidth);
	if (selected) {
		const auto ring = double(Scaled(2));
		const auto outer = rect.marginsAdded(
			QMarginsF(ring * 1.5, ring * 1.5, ring * 1.5, ring * 1.5));
		auto pen = QPen(st::activeLineFg);
		pen.setWidthF(ring);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(
			outer.marginsRemoved(
				QMarginsF(ring / 2., ring / 2., ring / 2., ring / 2.)),
			radius + ring,
			radius + ring);
	}
	auto border = QPen(anim::with_alpha(st::windowFg->c, 0.16));
	border.setWidthF(line);
	p.setPen(border);
	p.setBrush(color);
	p.drawRoundedRect(
		rect.marginsRemoved(QMarginsF(line / 2., line / 2., line / 2., line / 2.)),
		radius,
		radius);
}

Edit SetNodeMember(
		const Document &document,
		NodeId id,
		const QByteArray &key,
		Json::Value value) {
	if (!document.valid() || !document.contains(id)) {
		auto result = Edit();
		result.error = u"SetNodeMember: node not found"_q;
		return result;
	}
	const auto &json = document.json(id);
	if (json.get(key) == value && json.has(key)) {
		return Edit{ .document = document };
	}
	auto replaced = ReplaceNodeIn(
		document.root(),
		id,
		json.with(key, std::move(value)));
	if (!replaced) {
		auto result = Edit();
		result.error = u"SetNodeMember: node path not found"_q;
		return result;
	}
	return Edit{
		.document = Document(std::move(*replaced)),
		.changed = { id },
	};
}

// LiveSession.

LiveSession::LiveSession(
	not_null<EditorController*> controller,
	Command command)
: _controller(controller)
, _command(command) {
	_controller->documentChanged(
	) | rpl::filter([=] {
		return _active && !_applying;
	}) | rpl::on_next([=] {
		end();
	}, _lifetime);
}

LiveSession::~LiveSession() = default;

bool LiveSession::active() const {
	return _active;
}

const Document &LiveSession::baseline() const {
	return _baseline;
}

void LiveSession::begin() {
	if (_active) {
		return;
	}
	static auto Counter = uint64();
	_controller->finishMerge();
	_key = "oblivion-live-" + QByteArray::number(++Counter);
	_baseline = _controller->document();
	_nodes.clear();
	_structural = false;
	_steps = 0;
	_lastApply = 0;
	_active = true;
}

bool LiveSession::apply(Edit &&edit) {
	if (!edit.ok()) {
		if (!edit.error.isEmpty()) {
			LOG(("Oblivion LottieEdit: %1").arg(edit.error));
		}
		return false;
	}
	begin();
	if (edit.document.sameAs(_baseline)) {
		rollback();
		return true;
	}
	for (const auto list : { &edit.changed, &edit.created, &edit.removed }) {
		for (const auto id : *list) {
			if (!ranges::contains(_nodes, id)) {
				_nodes.push_back(id);
			}
		}
	}
	_structural = _structural || edit.structural;
	const auto now = crl::now();
	if (!_steps || (now - _lastApply) >= kStepGuess) {
		++_steps;
	}
	_lastApply = now;
	_applying = true;
	const auto result = _controller->perform(
		_command,
		std::move(edit),
		_key);
	_applying = false;
	return result;
}

void LiveSession::rollback() {
	_applying = true;
	auto left = _steps + 1;
	while (left-- > 0
		&& !_controller->document().sameAs(_baseline)
		&& _controller->canUndo()) {
		_controller->undo();
	}
	auto extra = kExtraRollback;
	while (extra-- > 0
		&& !_controller->document().sameAs(_baseline)
		&& _controller->canUndo()
		&& _controller->undoCommand() == _command) {
		_controller->undo();
	}
	_applying = false;
	_steps = 0;
	_lastApply = 0;
}

void LiveSession::squash() {
	if (!_active) {
		return;
	}
	const auto result = _controller->document();
	if (result.sameAs(_baseline)) {
		_steps = 0;
		return;
	} else if (_steps <= 1) {
		_controller->finishMerge();
		_steps = 1;
		_lastApply = 0;
		return;
	}
	rollback();
	_applying = true;
	_controller->perform(_command, Edit{
		.document = result,
		.changed = _nodes,
		.structural = _structural,
	});
	_applying = false;
	_steps = 1;
	_lastApply = 0;
}

void LiveSession::finish() {
	squash();
	end();
}

void LiveSession::cancel() {
	if (!_active) {
		return;
	}
	rollback();
	end();
}

void LiveSession::end() {
	if (!_active) {
		return;
	}
	_active = false;
	_baseline = Document();
	_nodes.clear();
	_steps = 0;
	_ended.fire({});
}

bool LiveSession::changed() const {
	return _active && !_controller->document().sameAs(_baseline);
}

rpl::producer<> LiveSession::ended() const {
	return _ended.events();
}

// Color box.

void ColorBox(not_null<Ui::GenericBox*> box, ColorBoxArgs &&args) {
	struct State {
		QColor last;
		bool finished = false;
	};
	const auto state = box->lifetime().make_state<State>();
	state->last = args.color;
	const auto preview = std::move(args.preview);
	const auto done = std::move(args.done);
	const auto cancelled = std::move(args.cancelled);

	box->setTitle(rpl::single(args.title));
	const auto editor = box->addRow(
		object_ptr<ColorEditor>(
			box,
			ColorEditor::Mode::HSL,
			args.color),
		style::margins());
	box->setWidth(editor->width());

	editor->colorValue(
	) | rpl::on_next([=](QColor color) {
		color.setAlpha(255);
		if (color == state->last) {
			return;
		}
		state->last = color;
		if (preview) {
			preview(color);
		}
	}, editor->lifetime());

	const auto finish = [=] {
		if (state->finished) {
			return;
		}
		state->finished = true;
		auto color = editor->color();
		color.setAlpha(255);
		if (done) {
			done(color);
		}
		box->closeBox();
	};
	editor->submitRequests(
	) | rpl::on_next(finish, editor->lifetime());

	box->addButton(tr::lng_box_done(), finish);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->setFocusCallback([=] { editor->setInnerFocus(); });

	box->boxClosing() | rpl::on_next([=] {
		if (state->finished) {
			return;
		}
		state->finished = true;
		if (cancelled) {
			cancelled();
		}
	}, box->lifetime());
}

void ShowColorBox(std::shared_ptr<Ui::Show> show, ColorBoxArgs &&args) {
	if (show) {
		show->showBox(Box(ColorBox, std::move(args)));
	}
}

void EditColorWithPicker(
		not_null<EditorController*> controller,
		PropertyRef ref,
		QString title,
		QColor current,
		Fn<PropValue(QColor)> make) {
	const auto show = controller->uiShow();
	if (!show || !make) {
		return;
	}
	const auto session = std::make_shared<LiveSession>(
		controller,
		Command::Recolor);
	session->begin();
	const auto frame = controller->localFrame(ref.node);
	const auto apply = [=](QColor color) {
		session->begin();
		session->apply(SetValueAt(
			session->baseline(),
			ref,
			make(color),
			frame));
	};
	ShowColorBox(show, {
		.title = std::move(title),
		.color = current,
		.preview = apply,
		.done = [=](QColor color) {
			apply(color);
			session->finish();
		},
		.cancelled = [=] {
			session->cancel();
		},
	});
}

// ColorSlider.

ColorSlider::ColorSlider(QWidget *parent, Track track, int min, int max)
: RpWidget(parent)
, _track(track)
, _min(min)
, _max(std::max(max, min + 1)) {
	setCursor(style::cur_pointer);
	setMouseTracking(true);
	resize(width(), Scaled(kSliderHeight));
}

void ColorSlider::setValue(int value) {
	value = std::clamp(value, _min, _max);
	if (_value != value) {
		_value = value;
		update();
	}
}

int ColorSlider::value() const {
	return _value;
}

rpl::producer<int> ColorSlider::changes() const {
	return _changes.events();
}

rpl::producer<> ColorSlider::finishes() const {
	return _finishes.events();
}

QRect ColorSlider::trackRect() const {
	const auto handle = Scaled(kSliderHandle);
	const auto thickness = Scaled(6);
	return QRect(
		handle,
		(height() - thickness) / 2,
		std::max(width() - 2 * handle, 1),
		thickness);
}

int ColorSlider::valueAt(int x) const {
	const auto track = trackRect();
	const auto progress = std::clamp(
		(x - track.x()) / double(track.width()),
		0.,
		1.);
	return _min + int(std::round(progress * (_max - _min)));
}

void ColorSlider::change(int value) {
	value = std::clamp(value, _min, _max);
	if (_value == value) {
		return;
	}
	_value = value;
	update();
	_changes.fire_copy(value);
}

void ColorSlider::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto track = QRectF(trackRect());
	auto gradient = QLinearGradient(track.left(), 0., track.right(), 0.);
	switch (_track) {
	case Track::Hue:
		for (auto i = 0; i <= 12; ++i) {
			const auto progress = i / 12.;
			const auto hue = (int(std::round(progress * 360.)) + 180) % 360;
			gradient.setColorAt(progress, QColor::fromHsl(hue, 210, 128));
		}
		break;
	case Track::Saturation:
		gradient.setColorAt(0., QColor::fromHsl(205, 0, 140));
		gradient.setColorAt(0.5, QColor::fromHsl(205, 120, 135));
		gradient.setColorAt(1., QColor::fromHsl(205, 255, 128));
		break;
	case Track::Lightness:
		gradient.setColorAt(0., QColor(0, 0, 0));
		gradient.setColorAt(0.5, QColor(128, 128, 128));
		gradient.setColorAt(1., QColor(255, 255, 255));
		break;
	}
	auto border = QPen(anim::with_alpha(st::windowFg->c, 0.14));
	border.setWidthF(st::lineWidth);
	p.setPen(border);
	p.setBrush(gradient);
	const auto radius = track.height() / 2.;
	p.drawRoundedRect(track, radius, radius);

	const auto position = [&](int value) {
		return track.left()
			+ (value - _min) * track.width() / double(_max - _min);
	};
	const auto active = _pressed || _over;
	const auto size = Scaled(active ? 16 : 14);
	const auto center = QPointF(position(_value), track.center().y());

	// The zero mark above the track, hidden while the knob is over it
	// (it would stick out of the knob's top edge).
	if (_min < 0 && _max > 0) {
		const auto x = position(0);
		const auto radius = st::lineWidth * 1.5;
		if (std::abs(x - center.x()) > size / 2. + radius + st::lineWidth) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowSubTextFg);
			p.drawEllipse(
				QPointF(x, track.top() - Scaled(3) - st::lineWidth),
				radius,
				radius);
		}
	}

	auto ring = QPen(anim::with_alpha(st::windowFg->c, active ? 0.45 : 0.3));
	ring.setWidthF(Scaled(2) * 0.75);
	p.setPen(ring);
	p.setBrush(st::windowBg);
	p.drawEllipse(center, size / 2., size / 2.);
}

void ColorSlider::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = true;
	update();
	change(valueAt(e->pos().x()));
}

void ColorSlider::mouseMoveEvent(QMouseEvent *e) {
	if (_pressed) {
		change(valueAt(e->pos().x()));
	}
}

void ColorSlider::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_pressed) {
		return;
	}
	_pressed = false;
	update();
	_finishes.fire({});
}

void ColorSlider::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = false;
	change(std::clamp(0, _min, _max));
	_finishes.fire({});
}

void ColorSlider::enterEventHook(QEnterEvent *e) {
	_over = true;
	update();
}

void ColorSlider::leaveEventHook(QEvent *e) {
	_over = false;
	update();
}

// PalettePanel.

PalettePanel::PalettePanel(
	QWidget *parent,
	not_null<EditorController*> controller)
: RpWidget(parent)
, _controller(controller)
, _hsl(controller, Command::Recolor) {
	setMouseTracking(true);
	_hint = BindShortWords(tr::lng_oblivion_lottie_palette_hint(tr::now));
	setupControls();

	_controller->documentChanged(
	) | rpl::on_next([=] {
		schedulePalette();
	}, lifetime());

	_controller->selectionChanged(
	) | rpl::on_next([=] {
		if (_selectionOnly->checked() && _hsl.active()) {
			_hsl.finish();
		}
	}, lifetime());

	_hsl.ended() | rpl::on_next([=] {
		_hslSquash = false;
		zeroHslSliders();
		refreshReset();
	}, lifetime());

	style::PaletteChanged() | rpl::on_next([=] {
		update();
	}, lifetime());

	computePalette();
}

PalettePanel::~PalettePanel() = default;

void PalettePanel::setupControls() {
	_hexHost = Ui::CreateChild<FieldHost>(this);
	_hex = Ui::CreateChild<Ui::InputField>(
		_hexHost,
		PanelFieldStyle(),
		Ui::InputField::Mode::SingleLine,
		nullptr,
		QString());
	_hex->setMaxLength(7);
	_hexHost->sizeValue() | rpl::on_next([=](QSize size) {
		_hex->setGeometry(QRect(QPoint(), size));
	}, _hexHost->lifetime());
	_hex->submits() | rpl::on_next([=] {
		commitHex();
	}, _hex->lifetime());
	_hex->focusedChanges() | rpl::filter(
		!rpl::mappers::_1
	) | rpl::on_next([=] {
		commitHex();
	}, _hex->lifetime());
	_hex->cancelled() | rpl::on_next([=] {
		if (_active) {
			_hex->setText(ColorHex(QColor::fromRgb(*_active)));
			_hex->selectAll();
		}
	}, _hex->lifetime());

	_edit = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_oblivion_lottie_palette_edit(tr::now));
	_edit->setClickedCallback([=] {
		if (_active) {
			editColor(*_active);
		}
	});
	_select = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_oblivion_lottie_palette_select(tr::now));
	_select->setClickedCallback([=] {
		if (_active) {
			selectOccurrences(*_active);
		}
	});

	const auto addSlider = [&](
			int index,
			ColorSlider::Track track,
			int min,
			int max,
			QString label) {
		auto &row = _sliders[index];
		row.label = std::move(label);
		row.slider = Ui::CreateChild<ColorSlider>(this, track, min, max);
		row.slider->changes() | rpl::on_next([=] {
			hslChanged();
		}, row.slider->lifetime());
		row.slider->finishes() | rpl::on_next([=] {
			hslFinished();
		}, row.slider->lifetime());
	};
	addSlider(
		0,
		ColorSlider::Track::Hue,
		-180,
		180,
		tr::lng_oblivion_lottie_palette_hue(tr::now));
	addSlider(
		1,
		ColorSlider::Track::Saturation,
		-100,
		100,
		tr::lng_oblivion_lottie_palette_saturation(tr::now));
	addSlider(
		2,
		ColorSlider::Track::Lightness,
		-100,
		100,
		tr::lng_oblivion_lottie_palette_lightness(tr::now));

	_selectionOnly = Ui::CreateChild<Ui::Checkbox>(
		this,
		BindShortWords(
			tr::lng_oblivion_lottie_palette_selection_only(tr::now)),
		false,
		st::defaultCheckbox);
	_selectionOnly->setAllowTextLines(0);
	_selectionOnly->checkedChanges() | rpl::on_next([=] {
		if (_hsl.active()) {
			_hsl.finish();
		}
	}, _selectionOnly->lifetime());

	_reset = Ui::CreateChild<Ui::RoundButton>(
		this,
		tr::lng_oblivion_lottie_palette_reset(),
		ResetButtonStyle());
	_reset->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_reset->setClickedCallback([=] {
		resetHsl();
	});
	refreshReset();
	setActive(std::nullopt);
}

void PalettePanel::schedulePalette() {
	if (_paletteRunning) {
		_paletteDirty = true;
	} else {
		computePalette();
	}
}

void PalettePanel::computePalette() {
	_paletteRunning = true;
	_paletteDirty = false;
	const auto document = _controller->document();
	const auto weak = QPointer<PalettePanel>(this);
	crl::async([=] {
		auto entries = document.valid()
			? document.palette()
			: std::vector<PaletteEntry>();
		crl::on_main(weak, [=, entries = std::move(entries)]() mutable {
			_paletteRunning = false;
			applyPalette(std::move(entries));
			if (_paletteDirty) {
				computePalette();
			}
		});
	});
}

void PalettePanel::applyPalette(std::vector<PaletteEntry> entries) {
	_entries = std::move(entries);
	_paletteReady = true;
	_over = _pressed = -1;
	if (const auto index = base::take(_activateIndex)) {
		if (*index >= 0 && *index < int(_entries.size())) {
			_active = _entries[*index].color.rgb();
		}
	}
	if (_active && !entry(*_active)) {
		_active = std::nullopt;
	}
	setActive(_active);
}

void PalettePanel::activateEntry(int index) {
	if (!_paletteReady || _paletteRunning) {
		_activateIndex = index;
	} else if (index >= 0 && index < int(_entries.size())) {
		setActive(_entries[index].color.rgb());
	}
}

const PaletteEntry *PalettePanel::entry(QRgb color) const {
	for (const auto &entry : _entries) {
		if (entry.color.rgb() == color) {
			return &entry;
		}
	}
	return nullptr;
}

void PalettePanel::setActive(std::optional<QRgb> color) {
	_active = (color && entry(*color)) ? color : std::nullopt;
	const auto shown = _active.has_value();
	_hexHost->setVisible(shown);
	_edit->setVisible(shown);
	_select->setVisible(shown);
	updateHexField();
	if (_layoutWidth > 0) {
		resizeToWidth(_layoutWidth);
	}
	update();
}

void PalettePanel::updateHexField() {
	if (_active && !Ui::InFocusChain(_hex)) {
		_hex->setText(ColorHex(QColor::fromRgb(*_active)));
	}
}

void PalettePanel::commitHex() {
	if (!_active) {
		return;
	}
	const auto color = ParseColorHex(_hex->getLastText());
	if (!color.isValid()) {
		_hex->setText(ColorHex(QColor::fromRgb(*_active)));
		return;
	}
	replaceActive(color);
}

void PalettePanel::replaceActive(const QColor &to) {
	if (!_active) {
		return;
	}
	const auto from = QColor::fromRgb(*_active);
	if (from.rgb() == to.rgb()) {
		return;
	}
	if (_hsl.active()) {
		_hsl.finish();
	}
	if (_controller->replaceColor(from, to)) {
		_active = to.rgb();
		_hex->setText(ColorHex(to));
	}
}

void PalettePanel::selectOccurrences(QRgb color) {
	const auto found = entry(color);
	if (!found) {
		return;
	}
	auto nodes = std::vector<NodeId>();
	for (const auto &occurrence : found->occurrences) {
		const auto id = occurrence.property.node;
		if (id && !ranges::contains(nodes, id)) {
			nodes.push_back(id);
		}
	}
	_controller->setSelection(std::move(nodes));
}

void PalettePanel::editColor(QRgb color) {
	const auto show = _controller->uiShow();
	if (!show || !entry(color)) {
		return;
	}
	if (_hsl.active()) {
		_hsl.finish();
	}
	const auto from = QColor::fromRgb(color);
	const auto session = std::make_shared<LiveSession>(
		_controller,
		Command::ReplaceColor);
	session->begin();
	const auto apply = [=](QColor to) {
		session->begin();
		session->apply(ReplaceColor(session->baseline(), from, to));
	};
	ShowColorBox(show, {
		.title = tr::lng_oblivion_lottie_palette_replace_title(tr::now),
		.color = from,
		.preview = apply,
		.done = crl::guard(this, [=](QColor to) {
			apply(to);
			session->finish();
			_active = to.rgb();
			_hex->setText(ColorHex(to));
			schedulePalette();
		}),
		.cancelled = [=] {
			session->cancel();
		},
	});
}

void PalettePanel::showSwatchMenu(QRgb color, QPoint globalPosition) {
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	_menu->addAction(
		tr::lng_oblivion_lottie_palette_edit_menu(tr::now),
		[=] { editColor(color); },
		&st::menuIconChangeColors);
	_menu->addAction(
		tr::lng_oblivion_lottie_palette_select(tr::now),
		[=] { selectOccurrences(color); },
		&st::menuIconSelect);
	_menu->addAction(
		tr::lng_oblivion_lottie_palette_copy(tr::now),
		[=] {
			QGuiApplication::clipboard()->setText(
				ColorHex(QColor::fromRgb(color)));
		},
		&st::menuIconCopy);
	_menu->popup(globalPosition);
}

QRect PalettePanel::swatchRect(int index) const {
	const auto padding = Scaled(kPadding);
	const auto size = Scaled(kSwatch);
	const auto columns = std::max(_columns, 1);
	const auto column = index % columns;
	const auto row = index / columns;
	const auto cellHeight = size
		+ Scaled(kCountSkip)
		+ PanelSmallFont()->height
		+ Scaled(kRowSkip);
	// The columns are spread over the whole width (the leftover pixels
	// go to the gaps), so the grid ends where the count above it ends.
	const auto step = (columns > 1)
		? (std::max(_gridInner - size, 0) / double(columns - 1))
		: 0.;
	return QRect(
		padding + int(std::round(column * step)),
		_gridTop + row * cellHeight,
		size,
		size);
}

int PalettePanel::swatchAt(QPoint position) const {
	for (auto i = 0; i != int(_entries.size()); ++i) {
		const auto rect = swatchRect(i).marginsAdded(
			QMargins(Scaled(2), Scaled(2), Scaled(2), Scaled(2)));
		if (rect.contains(position)) {
			return i;
		}
	}
	return -1;
}

int PalettePanel::resizeGetHeight(int newWidth) {
	return relayout(newWidth);
}

void PalettePanel::resizeEvent(QResizeEvent *e) {
	if (_layoutWidth != width()) {
		relayout(width());
	}
}

int PalettePanel::relayout(int width) {
	_layoutWidth = width;
	const auto padding = Scaled(kPadding);
	const auto inner = std::max(width - 2 * padding, 1);
	auto top = padding + st::semiboldFont->height + Scaled(10);

	_gridTop = top;
	const auto size = Scaled(kSwatch);
	const auto gap = Scaled(kSwatchGap);
	_columns = std::max((inner + gap) / (size + gap), 1);
	_gridInner = inner;
	const auto count = int(_entries.size());
	const auto rows = (count + _columns - 1) / _columns;
	const auto cellHeight = size
		+ Scaled(kCountSkip)
		+ PanelSmallFont()->height
		+ Scaled(kRowSkip);
	_gridHeight = count
		? (rows * cellHeight - Scaled(kRowSkip))
		: st::normalFont->height;
	top += _gridHeight + Scaled(16);

	if (_active) {
		_activeTop = top;
		const auto swatch = Scaled(kActiveSwatch);
		const auto left = padding + swatch + Scaled(12);
		_hexHost->setGeometry(
			left,
			top,
			Scaled(kHexWidth),
			Scaled(kHexHeight));
		const auto linksTop = top + Scaled(kHexHeight) + Scaled(6);
		_edit->moveToLeft(left + Scaled(2), linksTop, width);
		_select->moveToLeft(
			_edit->x() + _edit->width() + Scaled(16),
			linksTop,
			width);
		top += std::max(
			swatch,
			linksTop - top + _edit->height()) + Scaled(14);
	}

	_hintTop = top;
	_hintHeight = QFontMetrics(PanelSmallFont()->f).boundingRect(
		QRect(0, 0, inner, 1 << 20),
		Qt::TextWordWrap,
		_hint).height();
	top += _hintHeight + Scaled(18);

	_adjustTop = top;
	top += Scaled(14) + st::semiboldFont->height + Scaled(10);
	const auto handle = Scaled(kSliderHandle);
	for (auto &row : _sliders) {
		row.top = top;
		row.slider->setGeometry(
			padding - handle,
			top + st::normalFont->height + Scaled(2),
			inner + 2 * handle,
			Scaled(kSliderHeight));
		top += st::normalFont->height
			+ Scaled(2)
			+ Scaled(kSliderHeight)
			+ Scaled(8);
	}
	top += Scaled(2);
	_selectionOnly->resizeToWidth(inner);
	_selectionOnly->moveToLeft(padding, top, width);
	top += _selectionOnly->heightNoMargins() + Scaled(10);
	// The button text (centered, st.width < 0 is the padding around it)
	// as close to the left edge of the labels as the panel allows.
	_reset->moveToLeft(
		std::max(padding + ResetButtonStyle().width / 2, 0),
		top,
		width);
	top += _reset->height() + Scaled(12);
	return top;
}

void PalettePanel::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);

	const auto padding = Scaled(kPadding);
	const auto inner = std::max(width() - 2 * padding, 1);
	const auto &small = PanelSmallFont();

	// Title and count.
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		padding,
		padding + st::semiboldFont->ascent,
		tr::lng_oblivion_lottie_palette_colors(tr::now));
	if (!_entries.empty()) {
		const auto count = QString::number(_entries.size());
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			width() - padding - st::normalFont->width(count),
			padding + st::semiboldFont->ascent,
			count);
	}

	// Swatches.
	if (_entries.empty()) {
		if (_paletteReady) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(padding, _gridTop, inner, _gridHeight),
				Qt::AlignLeft | Qt::AlignVCenter,
				st::normalFont->elided(
					tr::lng_oblivion_lottie_palette_empty(tr::now),
					inner));
		}
	} else {
		p.setFont(small);
		for (auto i = 0; i != int(_entries.size()); ++i) {
			const auto &entry = _entries[i];
			const auto rect = swatchRect(i);
			const auto extra = Scaled(kSwatchGap);
			const auto full = rect.marginsAdded(QMargins(
				extra,
				extra,
				extra,
				Scaled(kCountSkip) + small->height));
			if (!full.intersects(e->rect())) {
				continue;
			}
			const auto active = _active && (*_active == entry.color.rgb());
			if (_over == i && !active) {
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver);
				auto hq = PainterHighQualityEnabler(p);
				const auto extra = Scaled(3);
				p.drawRoundedRect(
					QRectF(rect).marginsAdded(
						QMarginsF(extra, extra, extra, extra)),
					Scaled(kSwatchRadius) + extra,
					Scaled(kSwatchRadius) + extra);
			}
			PaintSwatch(
				p,
				QRectF(rect),
				entry.color,
				Scaled(kSwatchRadius),
				active);
			const auto count = QChar(0x00D7)
				+ QString::number(entry.occurrences.size());
			p.setFont(small);
			p.setPen(active ? st::windowActiveTextFg : st::windowSubTextFg);
			p.drawText(
				QRect(
					rect.x() - Scaled(kSwatchGap) / 2,
					rect.y() + rect.height() + Scaled(kCountSkip),
					rect.width() + Scaled(kSwatchGap),
					small->height),
				Qt::AlignHCenter | Qt::AlignTop,
				count);
		}
	}

	// Active color.
	if (_active) {
		if (const auto found = entry(*_active)) {
			const auto swatch = Scaled(kActiveSwatch);
			PaintSwatch(
				p,
				QRectF(padding, _activeTop, swatch, swatch),
				found->color,
				Scaled(10));
			const auto left = _hexHost->x() + _hexHost->width() + Scaled(10);
			const auto uses = tr::lng_oblivion_lottie_palette_uses(
				tr::now,
				lt_value,
				QString::number(found->occurrences.size()));
			const auto available = width() - padding - left;
			if (available > 0) {
				p.setFont(small);
				p.setPen(st::windowSubTextFg);
				p.drawText(
					QRect(left, _activeTop, available, Scaled(kHexHeight)),
					Qt::AlignLeft | Qt::AlignVCenter,
					small->elided(uses, available));
			}
		}
	}

	// Hint.
	p.setFont(small);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		QRect(padding, _hintTop, inner, _hintHeight),
		Qt::TextWordWrap,
		_hint);

	// Correction.
	p.fillRect(
		padding,
		_adjustTop,
		inner,
		st::lineWidth,
		st::shadowFg);
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		padding,
		_adjustTop + Scaled(14) + st::semiboldFont->ascent,
		tr::lng_oblivion_lottie_palette_adjust(tr::now));
	p.setFont(st::normalFont);
	for (auto i = 0; i != int(_sliders.size()); ++i) {
		const auto &row = _sliders[i];
		const auto value = row.slider->value();
		const auto valueText = SignedText(
			value,
			(i == 0) ? QString(QChar(0x00B0)) : QString());
		const auto valueWidth = st::normalFont->width(valueText);
		p.setPen(st::windowFg);
		p.drawText(
			padding,
			row.top + st::normalFont->ascent,
			st::normalFont->elided(
				row.label,
				std::max(inner - valueWidth - Scaled(8), 0)));
		p.setPen(value ? st::windowActiveTextFg : st::windowSubTextFg);
		p.drawText(
			padding + inner - valueWidth,
			row.top + st::normalFont->ascent,
			valueText);
	}
}

void PalettePanel::mouseMoveEvent(QMouseEvent *e) {
	const auto over = swatchAt(e->pos());
	if (_over == over) {
		return;
	}
	_over = over;
	setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
	update();
	if (over >= 0) {
		const auto &entry = _entries[over];
		_tooltip = ColorHex(entry.color)
			+ u" · "_q
			+ tr::lng_oblivion_lottie_palette_uses(
				tr::now,
				lt_value,
				QString::number(entry.occurrences.size()));
		Ui::Tooltip::Show(kTooltipDelay, this);
	} else {
		_tooltip = QString();
		Ui::Tooltip::Hide();
	}
}

void PalettePanel::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = swatchAt(e->pos());
	}
}

void PalettePanel::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed == swatchAt(e->pos())) {
		const auto color = _entries[pressed].color.rgb();
		setActive(color);
		selectOccurrences(color);
	}
}

void PalettePanel::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto index = swatchAt(e->pos());
	if (index >= 0) {
		const auto color = _entries[index].color.rgb();
		setActive(color);
		editColor(color);
	}
}

void PalettePanel::contextMenuEvent(QContextMenuEvent *e) {
	const auto index = swatchAt(e->pos());
	if (index < 0) {
		return;
	}
	const auto color = _entries[index].color.rgb();
	setActive(color);
	showSwatchMenu(color, e->globalPos());
}

void PalettePanel::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
	}
	Ui::Tooltip::Hide();
}

QString PalettePanel::tooltipText() const {
	return _tooltip;
}

QPoint PalettePanel::tooltipPos() const {
	return QCursor::pos();
}

bool PalettePanel::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

PalettePanel::Hsl PalettePanel::wantedHsl() const {
	auto result = Hsl{
		.hue = _sliders[0].slider->value(),
		.saturation = _sliders[1].slider->value(),
		.lightness = _sliders[2].slider->value(),
		.limited = _selectionOnly->checked(),
	};
	if (result.limited) {
		result.scope = _controller->selection();
	}
	return result;
}

void PalettePanel::hslChanged() {
	update();
	_hsl.begin();
	if (!_hslRunning) {
		startHslJob();
	}
}

void PalettePanel::startHslJob() {
	const auto wanted = wantedHsl();
	_hslRunning = wanted;
	const auto baseline = _hsl.baseline();
	const auto weak = QPointer<PalettePanel>(this);
	crl::async([=] {
		auto edit = (wanted.limited && wanted.scope.empty())
			? Edit{ .document = baseline }
			: AdjustHsl(
				baseline,
				wanted.hue,
				wanted.saturation,
				wanted.lightness,
				wanted.scope);
		crl::on_main(weak, [=, edit = std::move(edit)]() mutable {
			_hslRunning = std::nullopt;
			if (_hsl.active() && _hsl.baseline().sameAs(baseline)) {
				_hsl.apply(std::move(edit));
			}
			if (!_hsl.active()) {
				_hslSquash = false;
			} else if (!(wantedHsl() == wanted)) {
				startHslJob();
			} else if (base::take(_hslSquash)) {
				_hsl.squash();
			}
			refreshReset();
		});
	});
}

void PalettePanel::hslFinished() {
	if (_hslRunning) {
		_hslSquash = true;
	} else if (_hsl.active()) {
		_hsl.squash();
	}
	refreshReset();
}

void PalettePanel::resetHsl() {
	_hslSquash = false;
	zeroHslSliders();
	_hsl.cancel();
	refreshReset();
}

void PalettePanel::zeroHslSliders() {
	for (const auto &row : _sliders) {
		row.slider->setValue(0);
	}
	update();
}

void PalettePanel::refreshReset() {
	const auto available = _hsl.changed() || _hslRunning.has_value();
	_reset->setDisabled(!available);
	_reset->setTextFgOverride(available
		? std::nullopt
		: std::make_optional(st::windowSubTextFg->c));
}

// Snapshot helpers.

PanelSceneHost::PanelSceneHost(
	QWidget *parent,
	const QString &resource,
	Factory create)
: RpWidget(parent)
, _controller(std::make_unique<EditorController>()) {
	auto file = QFile(resource);
	const auto bytes = file.open(QIODevice::ReadOnly)
		? file.readAll()
		: QByteArray();
	auto document = Document::FromData(bytes);
	if (!document.valid()) {
		document = Document::Blank();
	}
	_controller->load(
		std::move(document),
		QFileInfo(resource).completeBaseName());
	_controller->setShow(SelfTest::SceneShow(this));
	_panel.reset(create(this, _controller.get()).get());
	sizeValue() | rpl::on_next([=](QSize size) {
		_panel->setGeometry(QRect(QPoint(), size));
	}, lifetime());
}

PanelSceneHost::~PanelSceneHost() {
	_panel = nullptr;
}

not_null<EditorController*> PanelSceneHost::controller() const {
	return _controller.get();
}

not_null<Ui::RpWidget*> PanelSceneHost::panel() const {
	return _panel.get();
}

NodeId FindNodeByName(
		const Document &document,
		const QString &name,
		std::optional<NodeKind> kind) {
	for (const auto &node : document.nodes()) {
		if (node.name == name && (!kind || node.kind == *kind)) {
			return node.id;
		}
	}
	return 0;
}

NodeId FindShapeOfType(const Document &document, ShapeType type, int skip) {
	for (const auto &node : document.nodes()) {
		if (node.kind == NodeKind::Shape && node.shapeType == type) {
			if (!skip--) {
				return node.id;
			}
		}
	}
	return 0;
}

// Snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).

namespace {

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	RegisterBoxScene(u"lottie_color_box"_q, QSize(480, 0), [](
			std::shared_ptr<Ui::Show> show) {
		return Box(ColorBox, ColorBoxArgs{
			.title = tr::lng_oblivion_lottie_palette_replace_title(tr::now),
			.color = QColor(157, 32, 239),
		});
	});
});

} // namespace

} // namespace Oblivion::LottieEdit
