/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_unified_chats.h"

#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_send_action.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "dialogs/dialogs_three_state_icon.h"
#include "dialogs/ui/dialogs_layout.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings/settings_common.h"
#include "storage/storage_media_prepare.h"
#include "ui/empty_userpic.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/unread_badge_paint.h"
#include "ui/userpic_view.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "window/window_separate_id.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_dialogs.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtCore/QMimeData>
#include <QtGui/QtEvents>

#include <unordered_map>

namespace Oblivion::UnifiedChats {
namespace {

constexpr auto kTooltipDelay = crl::time(700);
constexpr auto kMaxHeaderAccounts = 6;
constexpr auto kOtherAccountOpacity = 0.55;
constexpr auto kScrollProperty = "_oblivion_unified_scroll";

// The position of an entry in the merged list.
//
// Every account keeps its own chats list sorted by sortKeyInChatList(),
// those keys are comparable between the accounts: for an unpinned chat it
// is (date << 32) | counter with one process-wide counter. The pinned and
// fixed on top chats of all the accounts go first, account by account in
// the order of the accounts list, so opening a chat of another account
// (which makes that account the active one) never reorders the list.
struct SortItem {
	uint64 key = 0; // Dialogs::Entry::sortKeyInChatList().
	int account = 0;
	bool top = false; // Pinned or fixed on top in its own account.
};

[[nodiscard]] bool SortBefore(const SortItem &a, const SortItem &b) {
	if (a.top != b.top) {
		return a.top;
	} else if (a.top && a.account != b.account) {
		return (a.account < b.account);
	} else if (a.key != b.key) {
		return (a.key > b.key);
	}
	return (a.account < b.account);
}

// A chat that stays the first one in the list of its own account gets
// a new key without any event about a change of its position.
[[nodiscard]] bool SortStale(const SortItem &cached, uint64 key, bool top) {
	return (cached.key != key) || (cached.top != top);
}

// The stories row above the list, expanded by a click: remembers where
// the list was at that moment, scrolling it down by more than a row from
// there collapses the stories (the same rule as in the usual list).
class StoriesCollapse final {
public:
	void heightChanged(int height, int scrollTop) {
		if (height == _height) {
			// The same height comes again when the hidden row is shown back.
			return;
		}
		const auto expanding = (height > _height);
		if (expanding && !_expanded) {
			_from = scrollTop;
		}
		_expanded = expanding;
		_height = height;
	}
	[[nodiscard]] bool scrolledAway(int scrollTop, int threshold) const {
		return _expanded && (scrollTop > _from + threshold);
	}

private:
	int _height = 0;
	int _from = 0;
	bool _expanded = false; // False while it collapses as well.

};

[[nodiscard]] QColor AccountColor(int index) {
	// Blue, green, orange, violet, red, cyan, pink.
	constexpr uint8 kOrder[] = { 5, 3, 1, 2, 0, 4, 6 };
	const auto count = int(std::size(kOrder));
	const auto color = kOrder[((index % count) + count) % count];
	return Ui::EmptyUserpic::UserpicColor(color).color2->c;
}

// The account userpic is painted in one size everywhere: a userpic view
// keeps a cache for a single size.
[[nodiscard]] int BadgeSize() {
	return style::ConvertScale(18);
}

[[nodiscard]] int BadgeStroke() {
	return style::ConvertScale(2);
}

[[nodiscard]] bool NarrowWidth(int width) {
	return (width < st::columnMinimalWidthLeft / 2);
}

[[nodiscard]] const style::color &RowBg(bool active, bool selected) {
	return active
		? st::dialogsBgActive
		: selected
		? st::dialogsBgOver
		: st::dialogsBg;
}

struct RowState {
	int width = 0;
	crl::time now = 0;
	bool active = false;
	bool selected = false;
	bool narrow = false;
	bool paused = false;
};

struct AccountInfo {
	QString name;
	bool current = false; // The account of this window.
};

// Everything the list widgets know about the rows, so the same widgets
// show the real chats (LiveDelegate) and the sample ones in a snapshot
// scene (SampleDelegate).
class Delegate {
public:
	virtual ~Delegate() = default;

	[[nodiscard]] virtual int accountsCount() const = 0;
	[[nodiscard]] virtual AccountInfo accountInfo(int account) const = 0;
	// For AccountColor(), an account keeps its color when the accounts
	// are reordered or one of them logs out.
	[[nodiscard]] virtual int accountColorIndex(int account) const {
		return account;
	}
	virtual void paintAccountUserpic(
		Painter &p,
		int account,
		int x,
		int y,
		int size) = 0;
	virtual void accountChosen(int account) {
	}

	[[nodiscard]] virtual int rowsCount() const = 0;
	[[nodiscard]] virtual uint64 rowId(int row) const = 0; // Never 0.
	[[nodiscard]] virtual int rowIndex(uint64 id) const = 0; // Or -1.
	[[nodiscard]] virtual int rowAccount(int row) const = 0;
	[[nodiscard]] virtual bool rowActive(int row) const = 0;

	// Paints the row in (0, 0, state.width, row height).
	virtual void paintRow(Painter &p, int row, const RowState &state) = 0;
	virtual void rowPressed(int row, QPoint point, QSize size) {
	}
	virtual void rowReleased(uint64 id) {
	}
	virtual void rowChosen(int row, bool newWindow) {
	}
	virtual void fillRowMenu(int row, not_null<Ui::PopupMenu*> menu) {
	}
	// The files of a drag are inspected in dataAcceptsDrop(), so that is
	// asked once for a drag and only the row check on each move.
	[[nodiscard]] virtual bool rowAcceptsDrop(int row) const {
		return false;
	}
	[[nodiscard]] virtual bool dataAcceptsDrop(
			not_null<const QMimeData*> data) const {
		return false;
	}
	virtual void rowDropped(int row, not_null<const QMimeData*> data) {
	}

	[[nodiscard]] virtual bool loading() const {
		return false;
	}

	// The set or the order of the rows or of the accounts has changed.
	[[nodiscard]] virtual rpl::producer<> listChanges() const {
		return rpl::never<>();
	}
	// Repaint a row, -1 for all of them.
	[[nodiscard]] virtual rpl::producer<int> rowUpdates() const {
		return rpl::never<int>();
	}
	[[nodiscard]] virtual rpl::producer<> accountUpdates() const {
		return rpl::never<>();
	}

};

void PaintAccountBadge(
		Painter &p,
		not_null<Delegate*> delegate,
		int account,
		const RowState &state,
		int rowHeight) {
	const auto &st = st::defaultDialogRow;

	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);

	// A stripe of the account color on the left edge. On the active row
	// it gets an outline: a blue stripe is lost on the blue background.
	const auto stripe = style::ConvertScale(3);
	const auto skip = st.padding.top();
	if (state.active) {
		const auto line = style::ConvertScale(1);
		p.setBrush(st::dialogsBg);
		p.drawRoundedRect(
			QRectF(
				-stripe,
				skip - line,
				2 * stripe + line,
				rowHeight - 2 * skip + 2 * line),
			stripe + line,
			stripe + line);
	}
	p.setBrush(AccountColor(delegate->accountColorIndex(account)));
	p.drawRoundedRect(
		QRectF(-stripe, skip, 2 * stripe, rowHeight - 2 * skip),
		stripe,
		stripe);

	// The account userpic over the bottom left corner of the chat one.
	// In the narrow list it moves to the top left corner: there the unread
	// counter, the mention and the reaction badges are painted over the
	// bottom of the userpic and with four digits they reach its left edge.
	const auto size = BadgeSize();
	const auto stroke = BadgeStroke();
	const auto x = st.padding.left() - stroke;
	const auto y = state.narrow
		? (st.padding.top() - stroke)
		: (st.padding.top() + st.photoSize - size + stroke);
	p.setBrush(RowBg(state.active, state.selected));
	p.drawEllipse(
		x - stroke,
		y - stroke,
		size + 2 * stroke,
		size + 2 * stroke);
	delegate->paintAccountUserpic(p, account, x, y, size);
}

// The rows.
class List final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	List(
		QWidget *parent,
		not_null<Delegate*> delegate,
		Fn<bool()> paused);
	~List();

	[[nodiscard]] rpl::producer<Ui::ScrollToRequest> scrollToRequests() const;
	void setMinHeight(int height);

	void selectSkip(int by);
	void selectSkipPage(int height, int direction);
	bool chooseSelected(bool newWindow);

	// dataAccepted is asked only over a row that takes drops.
	[[nodiscard]] int dragOver(QPoint global, Fn<bool()> dataAccepted);
	void dragLeft();

	// Ui::AbstractTooltipShower.
	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	int resizeGetHeight(int newWidth) override;
	void visibleTopBottomUpdated(
		int visibleTop,
		int visibleBottom) override;

	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	[[nodiscard]] int rowAt(QPoint point) const;
	[[nodiscard]] bool rowSelected(int row) const;
	void listChanged();
	void selectByMouse(QPoint global);
	void setSelected(int row, bool byMouse);
	void setDragRow(int row);
	void updateRow(int row);
	void updateTooltip(QPoint point);
	void paintEmpty(Painter &p);

	const not_null<Delegate*> _delegate;
	const Fn<bool()> _paused;
	const int _rowHeight = 0;

	int _minHeight = 0;
	int _visibleTop = 0;
	int _visibleBottom = 0;

	int _selected = -1;
	uint64 _selectedId = 0;
	bool _selectedByMouse = false;
	bool _mouseInside = false;
	uint64 _pressedId = 0;
	uint64 _menuId = 0;
	int _dragRow = -1;
	int _tooltipAccount = -1;

	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::event_stream<Ui::ScrollToRequest> _scrollToRequests;

};

List::List(
	QWidget *parent,
	not_null<Delegate*> delegate,
	Fn<bool()> paused)
: RpWidget(parent)
, _delegate(delegate)
, _paused(std::move(paused))
, _rowHeight(st::defaultDialogRow.height) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMouseTracking(true);

	_delegate->listChanges(
	) | rpl::on_next([=] {
		listChanged();
	}, lifetime());

	_delegate->rowUpdates(
	) | rpl::on_next([=](int row) {
		if (row < 0) {
			update();
		} else {
			updateRow(row);
		}
	}, lifetime());

	_delegate->accountUpdates(
	) | rpl::on_next([=] {
		update();
	}, lifetime());

	style::PaletteChanged(
	) | rpl::on_next([=] {
		update();
	}, lifetime());
}

List::~List() {
	// The callback of the menu must not run while the members die.
	if (const auto menu = _menu.get()) {
		QObject::disconnect(menu, &QObject::destroyed, this, nullptr);
	}
}

rpl::producer<Ui::ScrollToRequest> List::scrollToRequests() const {
	return _scrollToRequests.events();
}

void List::setMinHeight(int height) {
	if (_minHeight != height) {
		_minHeight = height;
		resizeToWidth(width());
	}
}

int List::resizeGetHeight(int newWidth) {
	return std::max(_delegate->rowsCount() * _rowHeight, _minHeight);
}

void List::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	const auto scrolled = (_visibleTop != visibleTop);
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;

	// The rows have moved under a cursor that stays in its place.
	if (scrolled
		&& _mouseInside
		&& _selectedByMouse
		&& !_menuId
		&& !_pressedId) {
		selectByMouse(QCursor::pos());
	}
}

int List::rowAt(QPoint point) const {
	if (point.x() < 0 || point.x() >= width() || point.y() < 0) {
		return -1;
	}
	const auto row = point.y() / _rowHeight;
	return (row < _delegate->rowsCount()) ? row : -1;
}

bool List::rowSelected(int row) const {
	return _menuId
		? (_delegate->rowId(row) == _menuId)
		: (_dragRow >= 0)
		? (row == _dragRow)
		: _pressedId
		? (_delegate->rowId(row) == _pressedId)
		: (row == _selected);
}

void List::updateRow(int row) {
	if (row >= 0) {
		update(0, row * _rowHeight, width(), _rowHeight);
	}
}

void List::listChanged() {
	_dragRow = -1;
	if (_selectedByMouse) {
		_selected = -1;
		_selectedId = 0;
		if (_mouseInside) {
			selectByMouse(QCursor::pos());
		}
	} else if (_selectedId) {
		_selected = _delegate->rowIndex(_selectedId);
		if (_selected < 0) {
			_selectedId = 0;
		}
	}
	resizeToWidth(width());
	update();
}

void List::setSelected(int row, bool byMouse) {
	const auto id = (row >= 0) ? _delegate->rowId(row) : uint64(0);
	_selectedByMouse = byMouse;
	if (_selected == row && _selectedId == id) {
		return;
	}
	updateRow(_selected);
	_selected = row;
	_selectedId = id;
	updateRow(_selected);
	if (!_menuId && !_pressedId) {
		setCursor((_selected >= 0) ? style::cur_pointer : style::cur_default);
	}
}

void List::selectByMouse(QPoint global) {
	const auto point = mapFromGlobal(global);
	setSelected(rowAt(point), true);
	updateTooltip(point);
}

void List::updateTooltip(QPoint point) {
	// The account name is shown over the userpic with the account badge
	// (that is the whole row in the narrow chats list).
	const auto account = (_selected >= 0
		&& !_menuId
		&& point.x() < st::defaultDialogRow.nameLeft)
		? _delegate->rowAccount(_selected)
		: -1;
	if (_tooltipAccount == account) {
		return;
	}
	_tooltipAccount = account;
	if (account >= 0) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	} else {
		Ui::Tooltip::Hide();
	}
}

QString List::tooltipText() const {
	return (isVisible()
		&& _tooltipAccount >= 0
		&& _tooltipAccount < _delegate->accountsCount())
		? tr::lng_oblivion_unified_account(
			tr::now,
			lt_name,
			_delegate->accountInfo(_tooltipAccount).name)
		: QString();
}

QPoint List::tooltipPos() const {
	return QCursor::pos();
}

bool List::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void List::selectSkip(int by) {
	const auto count = _delegate->rowsCount();
	if (!count || !by) {
		return;
	}
	auto row = _selected;
	if (row < 0) {
		if (by < 0) {
			return;
		}
		// Start from the first row that is fully visible.
		row = std::clamp(
			(_visibleTop + _rowHeight - 1) / _rowHeight,
			0,
			count - 1);
	} else {
		row = std::clamp(row + by, 0, count - 1);
	}
	setSelected(row, false);
	_scrollToRequests.fire({ row * _rowHeight, (row + 1) * _rowHeight });
}

void List::selectSkipPage(int height, int direction) {
	const auto rows = std::max(height / _rowHeight, 1);
	selectSkip(rows * direction);
}

bool List::chooseSelected(bool newWindow) {
	if (_selected < 0 || _selected >= _delegate->rowsCount()) {
		return false;
	}
	_delegate->rowChosen(_selected, newWindow);
	return true;
}

void List::setDragRow(int row) {
	if (_dragRow != row) {
		updateRow(_dragRow);
		_dragRow = row;
		updateRow(_dragRow);
	}
}

int List::dragOver(QPoint global, Fn<bool()> dataAccepted) {
	const auto row = rowAt(mapFromGlobal(global));
	const auto accepts = (row >= 0)
		&& _delegate->rowAcceptsDrop(row)
		&& dataAccepted
		&& dataAccepted();
	setDragRow(accepts ? row : -1);
	return _dragRow;
}

void List::dragLeft() {
	setDragRow(-1);
}

void List::paintEmpty(Painter &p) {
	if (NarrowWidth(width())) {
		return;
	}
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		QRect(0, 0, width(), std::min(height(), st::dialogsEmptyHeight)),
		(_delegate->loading()
			? tr::lng_oblivion_unified_loading(tr::now)
			: tr::lng_oblivion_unified_empty(tr::now)),
		style::al_center);
}

void List::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto clip = e->rect();
	const auto paused = _paused && _paused();
	p.setInactive(paused);

	const auto count = _delegate->rowsCount();
	const auto bottom = count * _rowHeight;
	if (clip.y() + clip.height() > bottom) {
		p.fillRect(
			clip.intersected(QRect(0, bottom, width(), height() - bottom)),
			st::dialogsBg);
	}
	if (!count) {
		paintEmpty(p);
		return;
	}
	const auto from = std::clamp(clip.y() / _rowHeight, 0, count);
	const auto till = std::clamp(
		(clip.y() + clip.height() + _rowHeight - 1) / _rowHeight,
		from,
		count);
	auto state = RowState{
		.width = width(),
		.now = crl::now(),
		.narrow = NarrowWidth(width()),
		.paused = paused,
	};
	p.translate(0, from * _rowHeight);
	for (auto row = from; row != till; ++row) {
		state.active = _delegate->rowActive(row);
		state.selected = rowSelected(row);
		_delegate->paintRow(p, row, state);
		p.setOpacity(1.);
		PaintAccountBadge(
			p,
			_delegate,
			_delegate->rowAccount(row),
			state,
			_rowHeight);
		p.translate(0, _rowHeight);
	}
}

void List::mouseMoveEvent(QMouseEvent *e) {
	_mouseInside = true;
	selectByMouse(e->globalPos());
}

void List::mousePressEvent(QMouseEvent *e) {
	_mouseInside = true;
	selectByMouse(e->globalPos());
	if (e->button() != Qt::LeftButton || _selected < 0) {
		return;
	}
	_pressedId = _selectedId;
	_delegate->rowPressed(
		_selected,
		e->pos() - QPoint(0, _selected * _rowHeight),
		QSize(width(), _rowHeight));
	updateRow(_selected);
}

void List::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = base::take(_pressedId);
	if (!pressed) {
		return;
	}
	_delegate->rowReleased(pressed);
	updateRow(_delegate->rowIndex(pressed));
	selectByMouse(e->globalPos());
	if (_selected >= 0 && _selectedId == pressed) {
		// The delegate may destroy the whole list from this call.
		_delegate->rowChosen(
			_selected,
			(e->modifiers() & Qt::ControlModifier));
	}
}

void List::leaveEventHook(QEvent *e) {
	_mouseInside = false;
	if (_selectedByMouse && !_menuId) {
		setSelected(-1, true);
	}
	_tooltipAccount = -1;
	Ui::Tooltip::Hide();
}

void List::contextMenuEvent(QContextMenuEvent *e) {
	_menu = nullptr;

	const auto fromMouse = (e->reason() == QContextMenuEvent::Mouse);
	if (fromMouse) {
		selectByMouse(e->globalPos());
	}
	if (_selected < 0) {
		return;
	}
	if (const auto pressed = base::take(_pressedId)) {
		_delegate->rowReleased(pressed);
	}
	_tooltipAccount = -1;
	Ui::Tooltip::Hide();

	const auto row = _selected;
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuExpandedSeparator);
	_delegate->fillRowMenu(row, _menu.get());
	if (_menu->empty()) {
		_menu = nullptr;
		return;
	}
	_menuId = _selectedId;
	updateRow(row);
	QObject::connect(_menu.get(), &QObject::destroyed, this, [=] {
		if (const auto id = base::take(_menuId)) {
			updateRow(_delegate->rowIndex(id));
		}
		if (_selectedByMouse) {
			_mouseInside = rect().contains(mapFromGlobal(QCursor::pos()));
			if (_mouseInside) {
				selectByMouse(QCursor::pos());
			} else {
				setSelected(-1, true);
			}
		}
	});
	_menu->popup(e->globalPos());
	e->accept();
}

// The title, the accounts and the button that turns the mode off.
class Header final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	Header(
		QWidget *parent,
		not_null<Delegate*> delegate,
		Fn<void()> turnOff);

	// Ui::AbstractTooltipShower.
	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void leaveToChildEvent(QEvent *e, QWidget *child) override;

private:
	[[nodiscard]] int shownCount() const;
	[[nodiscard]] int right() const;
	[[nodiscard]] QRect accountRect(int index) const;
	[[nodiscard]] int accountAt(QPoint point) const;
	void setOver(int index);

	const not_null<Delegate*> _delegate;
	const int _outer = 0;
	const int _ring = 0;
	const int _skip = 0;

	object_ptr<Ui::CrossButton> _close = { nullptr };
	int _over = -1;
	int _pressed = -1;

};

Header::Header(
	QWidget *parent,
	not_null<Delegate*> delegate,
	Fn<void()> turnOff)
: RpWidget(parent)
, _delegate(delegate)
, _outer(BadgeSize() + 2 * BadgeStroke())
, _ring(BadgeStroke())
, _skip(style::ConvertScale(6)) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMouseTracking(true);

	if (turnOff) {
		_close.create(this, st::dialogsCancelSearch);
		_close->toggle(true, anim::type::instant);
		_close->setClickedCallback([callback = std::move(turnOff)] {
			Ui::Tooltip::Hide();
			callback();
		});
		_close->setAccessibleName(
			tr::lng_oblivion_unified_turn_off(tr::now));
		Ui::InstallTooltip(_close.data(), [] {
			return tr::lng_oblivion_unified_turn_off(tr::now);
		});
	}

	rpl::merge(
		_delegate->listChanges(),
		_delegate->accountUpdates(),
		style::PaletteChanged()
	) | rpl::on_next([=] {
		update();
	}, lifetime());
}

int Header::shownCount() const {
	return std::min(_delegate->accountsCount(), kMaxHeaderAccounts);
}

int Header::right() const {
	return _close
		? _close->x()
		: (width() - st::defaultDialogRow.padding.right());
}

QRect Header::accountRect(int index) const {
	const auto count = shownCount();
	const auto full = count * _outer + std::max(count - 1, 0) * _skip;
	return QRect(
		right() - full + index * (_outer + _skip),
		(height() - _outer) / 2,
		_outer,
		_outer);
}

int Header::accountAt(QPoint point) const {
	const auto count = shownCount();
	for (auto i = 0; i != count; ++i) {
		if (accountRect(i).contains(point)) {
			return i;
		}
	}
	return -1;
}

void Header::setOver(int index) {
	if (_over == index) {
		return;
	}
	_over = index;
	setCursor((_over >= 0) ? style::cur_pointer : style::cur_default);
	update();
	if (_over >= 0) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	} else {
		Ui::Tooltip::Hide();
	}
}

QString Header::tooltipText() const {
	return (isVisible() && _over >= 0 && _over < _delegate->accountsCount())
		? _delegate->accountInfo(_over).name
		: QString();
}

QPoint Header::tooltipPos() const {
	return QCursor::pos();
}

bool Header::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void Header::resizeEvent(QResizeEvent *e) {
	if (_close) {
		_close->moveToRight(0, (height() - _close->height()) / 2);
	}
}

void Header::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	p.fillRect(e->rect(), st::dialogsBg);

	const auto count = shownCount();
	const auto accountsLeft = count ? accountRect(0).x() : right();
	{
		auto hq = PainterHighQualityEnabler(p);
		for (auto i = 0; i != count; ++i) {
			const auto rect = accountRect(i);
			const auto dimmed = !_delegate->accountInfo(i).current
				&& (i != _over);
			p.setOpacity(dimmed ? kOtherAccountOpacity : 1.);
			p.setPen(Qt::NoPen);
			p.setBrush(AccountColor(_delegate->accountColorIndex(i)));
			p.drawEllipse(rect);
			if (dimmed) {
				// The ring is a filled circle. A faded userpic must fade
				// to the background and not to the ring color under it.
				p.setOpacity(1.);
				p.setBrush(st::dialogsBg);
				p.drawEllipse(
					rect.marginsRemoved({ _ring, _ring, _ring, _ring }));
				p.setOpacity(kOtherAccountOpacity);
			}
			_delegate->paintAccountUserpic(
				p,
				i,
				rect.x() + _ring,
				rect.y() + _ring,
				_outer - 2 * _ring);
		}
		p.setOpacity(1.);
	}

	const auto &font = st::semiboldFont;
	const auto left = st::defaultDialogRow.padding.left();
	const auto available = accountsLeft - _skip - left;
	if (available < style::ConvertScale(48)) {
		return;
	}
	const auto title = tr::lng_oblivion_unified_title(tr::now);
	p.setFont(font);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		left,
		(height() - font->height) / 2 + font->ascent,
		(font->width(title) > available)
			? font->elided(title, available)
			: title);
}

void Header::mouseMoveEvent(QMouseEvent *e) {
	setOver(accountAt(e->pos()));
}

void Header::mousePressEvent(QMouseEvent *e) {
	setOver(accountAt(e->pos()));
	if (e->button() == Qt::LeftButton) {
		_pressed = _over;
	}
}

void Header::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	setOver(accountAt(e->pos()));
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed == _over) {
		Ui::Tooltip::Hide();
		// The delegate may destroy the whole panel from this call.
		_delegate->accountChosen(pressed);
	}
}

void Header::leaveEventHook(QEvent *e) {
	setOver(-1);
}

void Header::leaveToChildEvent(QEvent *e, QWidget *child) {
	// The cursor went from an account straight to the cross. The tooltip
	// of the cross is already requested here, so it is not hidden.
	if (_over >= 0) {
		_over = -1;
		setCursor(style::cur_default);
		update();
	}
}

struct PanelArgs {
	Fn<void()> turnOff; // No button without it.
	Fn<bool()> paused;
};

// The header and the scrolled rows, covers the usual chats list.
class Panel final : public Ui::RpWidget {
public:
	Panel(
		QWidget *parent,
		std::unique_ptr<Delegate> delegate,
		PanelArgs &&args);

	[[nodiscard]] bool handleKey(not_null<QKeyEvent*> e);
	[[nodiscard]] int scrollTop() const;
	[[nodiscard]] rpl::producer<int> scrollTopChanges() const;
	void restoreScroll(int top);

	// The look of a row under the cursor, for a snapshot scene.
	void selectRow(int row);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dragMoveEvent(QDragMoveEvent *e) override;
	void dragLeaveEvent(QDragLeaveEvent *e) override;
	void dropEvent(QDropEvent *e) override;

private:
	void updateVisibleRange();
	void tryRestoreScroll();
	template <typename Event>
	[[nodiscard]] int dragRow(not_null<Event*> e);

	// Destroyed after the widgets that use it.
	const std::unique_ptr<Delegate> _delegate;
	object_ptr<Header> _header;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<List> _list;
	int _restoreScroll = 0;
	bool _laidOut = false;

	// Whether the data of the current drag may be dropped on a chat.
	std::optional<bool> _dragDataAccepted;

};

Panel::Panel(
	QWidget *parent,
	std::unique_ptr<Delegate> delegate,
	PanelArgs &&args)
: RpWidget(parent)
, _delegate(std::move(delegate))
, _header(this, _delegate.get(), std::move(args.turnOff))
, _scroll(this) {
	setAttribute(Qt::WA_OpaquePaintEvent);

	// Wheel and mouse events must not reach the covered list: it scrolls,
	// shows the stories on a pull and has swipe actions on its rows.
	setAttribute(Qt::WA_NoMousePropagation);

	// The same for drags: without this the covered list would choose its
	// own (invisible) row under the cursor.
	setAcceptDrops(true);

	_list = _scroll->setOwnedWidget(object_ptr<List>(
		this,
		_delegate.get(),
		std::move(args.paused)));

	_list->scrollToRequests(
	) | rpl::on_next([=](Ui::ScrollToRequest request) {
		_scroll->scrollToY(request.ymin, request.ymax);
	}, lifetime());

	rpl::merge(
		_scroll->scrolls(),
		_scroll->geometryChanged(),
		_list->heightValue() | rpl::to_empty
	) | rpl::on_next([=] {
		updateVisibleRange();
	}, lifetime());
}

void Panel::updateVisibleRange() {
	if (_list) {
		const auto top = _scroll->scrollTop();
		_list->setVisibleTopBottom(top, top + _scroll->height());
	}
}

int Panel::scrollTop() const {
	return _scroll->scrollTop();
}

rpl::producer<int> Panel::scrollTopChanges() const {
	return _scroll->scrollTopChanges();
}

void Panel::selectRow(int row) {
	if (_list) {
		_list->selectSkip(1);
		_list->selectSkip(row);
	}
}

void Panel::restoreScroll(int top) {
	_restoreScroll = top;
	tryRestoreScroll();
}

void Panel::tryRestoreScroll() {
	// Waits for the first resize: before it the rows have no geometry.
	if (_restoreScroll > 0 && _list && _laidOut) {
		_scroll->scrollToY(base::take(_restoreScroll));
	}
}

bool Panel::handleKey(not_null<QKeyEvent*> e) {
	if (!_list) {
		return false;
	}
	const auto key = e->key();
	const auto modifiers = e->modifiers();
	if (key == Qt::Key_Return || key == Qt::Key_Enter) {
		// Enter never reaches the covered list. Without a selected row
		// it opens the first one, as the usual list does.
		const auto newWindow = bool(modifiers & Qt::ControlModifier);
		if (!_list->chooseSelected(newWindow)) {
			_list->selectSkip(1);
			_list->chooseSelected(newWindow);
		}
		return true;
	} else if (modifiers
		& (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
		return false;
	} else if (key == Qt::Key_Down) {
		_list->selectSkip(1);
	} else if (key == Qt::Key_Up) {
		_list->selectSkip(-1);
	} else if (key == Qt::Key_PageDown) {
		_list->selectSkipPage(_scroll->height(), 1);
	} else if (key == Qt::Key_PageUp) {
		_list->selectSkipPage(_scroll->height(), -1);
	} else {
		return false;
	}
	return true;
}

void Panel::resizeEvent(QResizeEvent *e) {
	const auto narrow = NarrowWidth(width());
	const auto header = narrow ? 0 : st::dialogsImportantBarHeight;
	_header->setVisible(!narrow);
	_header->setGeometry(0, 0, width(), header);
	_scroll->setGeometry(0, header, width(), std::max(height() - header, 0));
	if (_list) {
		_list->resizeToWidth(width());
		_list->setMinHeight(_scroll->height());
	}
	_laidOut = true;
	tryRestoreScroll();
	updateVisibleRange();
}

void Panel::paintEvent(QPaintEvent *e) {
	QPainter(this).fillRect(e->rect(), st::dialogsBg);
}

template <typename Event>
int Panel::dragRow(not_null<Event*> e) {
	if (!_list) {
		return -1;
	}
	const auto data = e->mimeData();
	// pos() and not position(): Qt 5 (the default Qt of the Windows build)
	// has only this one, Dialogs::Widget uses it for drags as well.
	return _list->dragOver(mapToGlobal(e->pos()), [&] {
		if (!data) {
			return false;
		} else if (!_dragDataAccepted) {
			_dragDataAccepted = _delegate->dataAcceptsDrop(data);
		}
		return *_dragDataAccepted;
	});
}

void Panel::dragEnterEvent(QDragEnterEvent *e) {
	_dragDataAccepted = std::nullopt;
	e->setDropAction((dragRow(not_null(e)) >= 0)
		? Qt::CopyAction
		: Qt::IgnoreAction);
	e->accept();
}

void Panel::dragMoveEvent(QDragMoveEvent *e) {
	e->setDropAction((dragRow(not_null(e)) >= 0)
		? Qt::CopyAction
		: Qt::IgnoreAction);
	e->accept();
}

void Panel::dragLeaveEvent(QDragLeaveEvent *e) {
	_dragDataAccepted = std::nullopt;
	if (_list) {
		_list->dragLeft();
	}
	e->accept();
}

void Panel::dropEvent(QDropEvent *e) {
	const auto row = dragRow(not_null(e));
	_dragDataAccepted = std::nullopt;
	if (_list) {
		_list->dragLeft();
	}
	if (row >= 0) {
		e->setDropAction(Qt::CopyAction);
		e->accept();
		_delegate->rowDropped(row, e->mimeData());
	}
}

[[nodiscard]] Window::SeparateId SeparateIdFor(not_null<History*> history) {
	using Type = Window::SeparateType;
	const auto peer = history->peer;
	const auto channel = peer->asChannel();
	if (channel && channel->isCommunity()) {
		return Window::SeparateId(Type::Community, history);
	} else if (history->isForum() && !peer->useSubsectionTabs()) {
		return Window::SeparateId(Type::Forum, history);
	}
	return Window::SeparateId(Type::Chat, history);
}

// The Dialogs::Widget side: decides when the panel is shown.
class Host final : public base::has_weak_ptr {
public:
	explicit Host(AttachArgs &&args);
	~Host();

	[[nodiscard]] static Host *Find(not_null<QWidget*> dialogs);
	[[nodiscard]] static Host *Find(
		not_null<Window::SessionController*> controller);

	void refreshNow();
	[[nodiscard]] bool handleKey(not_null<QKeyEvent*> e);
	void choose(not_null<History*> history, bool newWindow);

private:
	void watchAccounts();
	void scheduleRefresh();
	void refresh(bool queued);
	void create();

	const not_null<QWidget*> _dialogs;
	const not_null<Window::SessionController*> _controller;
	const QPointer<Ui::RpWidget> _scroll;
	const QPointer<QWidget> _window;
	const Fn<bool()> _plain;
	const Fn<void(not_null<History*>, bool)> _choose;
	const Fn<void()> _collapseStories;

	base::unique_qptr<Panel> _panel;
	rpl::variable<int> _topInset = 0;
	StoriesCollapse _stories;
	bool _refreshScheduled = false;

	rpl::lifetime _accountsLifetime;
	rpl::lifetime _lifetime;

};

[[nodiscard]] std::vector<not_null<Host*>> &Hosts() {
	static auto Result = std::vector<not_null<Host*>>();
	return Result;
}

// Opens a chat or the archive of an account that is not shown in the
// window the click was in.
void OpenInAccount(not_null<Dialogs::Entry*> entry) {
	const auto session = &entry->session();
	const auto weak = base::make_weak(entry);

	// Switching the account destroys the list this call comes from.
	crl::on_main(session, [=] {
		const auto account = not_null(&session->account());
		auto activate = Fn<void()>(crl::guard(session, [=] {
			Core::App().domain().activate(account);
			const auto entry = weak.get();
			const auto window = entry
				? session->tryResolveWindow()
				: nullptr;
			if (!window) {
				return;
			} else if (const auto folder = entry->asFolder()) {
				window->openFolder(folder);
			} else if (const auto history = entry->asHistory()) {
				if (const auto host = Host::Find(window)) {
					host->choose(history, false);
				} else {
					window->showPeerHistory(
						history,
						Window::SectionShow::Way::ClearStack);
				}
			}
		}));
		// The same as in Main::Domain::maybeActivate().
		if (Core::App().separateWindowFor(account)) {
			activate();
		} else {
			Core::App().preventOrInvoke(std::move(activate));
		}
	});
}

// The chats of all the authorized accounts.
class LiveDelegate final
	: public Delegate
	, public base::has_weak_ptr {
public:
	LiveDelegate(
		not_null<Window::SessionController*> controller,
		Fn<void(not_null<History*>, bool)> chooseLocal);

	int accountsCount() const override;
	AccountInfo accountInfo(int account) const override;
	int accountColorIndex(int account) const override;
	void paintAccountUserpic(
		Painter &p,
		int account,
		int x,
		int y,
		int size) override;
	void accountChosen(int account) override;

	int rowsCount() const override;
	uint64 rowId(int row) const override;
	int rowIndex(uint64 id) const override;
	int rowAccount(int row) const override;
	bool rowActive(int row) const override;
	void paintRow(Painter &p, int row, const RowState &state) override;
	void rowPressed(int row, QPoint point, QSize size) override;
	void rowReleased(uint64 id) override;
	void rowChosen(int row, bool newWindow) override;
	void fillRowMenu(int row, not_null<Ui::PopupMenu*> menu) override;
	bool rowAcceptsDrop(int row) const override;
	bool dataAcceptsDrop(not_null<const QMimeData*> data) const override;
	void rowDropped(int row, not_null<const QMimeData*> data) override;
	bool loading() const override;

	rpl::producer<> listChanges() const override;
	rpl::producer<int> rowUpdates() const override;
	rpl::producer<> accountUpdates() const override;

private:
	struct Account {
		not_null<Main::Account*> account;
		not_null<Main::Session*> session;
		int colorIndex = 0;
		Ui::PeerUserpicView userpic;
		rpl::lifetime lifetime;
	};
	struct Entry {
		// A chat or the archive folder of an account.
		not_null<Dialogs::Entry*> entry;
		SortItem sort;
	};

	void refreshAccounts();
	void sessionChanged(
		not_null<Main::Account*> account,
		Main::Session *session);
	void subscribe(not_null<Account*> account);
	void scheduleRebuild();
	void rebuild();
	[[nodiscard]] bool applyAccountsOrder();
	void updateRow(not_null<Dialogs::Entry*> entry);
	void updateCornerBadge(not_null<History*> history);
	void choose(not_null<Dialogs::Entry*> entry, bool newWindow);
	[[nodiscard]] Dialogs::Entry *entryAt(int row) const;
	[[nodiscard]] not_null<Dialogs::Row*> rowFor(
		not_null<Dialogs::Entry*> entry);
	[[nodiscard]] bool local(not_null<Dialogs::Entry*> entry) const;

	// The ids of the rows are used only as keys, never dereferenced.
	[[nodiscard]] static uint64 ToId(Dialogs::Entry *entry);
	[[nodiscard]] static Dialogs::Entry *FromId(uint64 id);

	const not_null<Window::SessionController*> _controller;
	const Fn<void(not_null<History*>, bool)> _chooseLocal;

	std::vector<std::unique_ptr<Account>> _accounts;
	std::vector<Entry> _entries;
	std::unordered_map<Dialogs::Entry*, int> _indices;

	// Own rows for painting, the ones in the lists of the sessions come
	// and go together with the positions of the chats there.
	std::unordered_map<
		Dialogs::Entry*,
		std::unique_ptr<Dialogs::Row>> _rows;

	History *_active = nullptr;
	bool _rebuildScheduled = false;

	rpl::event_stream<> _listChanges;
	rpl::event_stream<int> _rowUpdates;
	rpl::event_stream<> _accountUpdates;

	rpl::lifetime _watchLifetime;
	rpl::lifetime _lifetime;

};

LiveDelegate::LiveDelegate(
	not_null<Window::SessionController*> controller,
	Fn<void(not_null<History*>, bool)> chooseLocal)
: _controller(controller)
, _chooseLocal(std::move(chooseLocal)) {
	Core::App().domain().accountsChanges(
	) | rpl::on_next([=] {
		// Removed accounts are already destroyed here.
		refreshAccounts();
	}, _lifetime);

	_controller->activeChatValue(
	) | rpl::on_next([=](Dialogs::Key key) {
		const auto was = std::exchange(_active, key.owningHistory());
		if (was != _active) {
			// The old one is used only as a key here.
			const auto i = _indices.find(was);
			if (i != end(_indices)) {
				_rowUpdates.fire_copy(i->second);
			}
			if (_active) {
				updateRow(_active);
			}
		}
	}, _lifetime);

	refreshAccounts();
}

uint64 LiveDelegate::ToId(Dialogs::Entry *entry) {
	return uint64(reinterpret_cast<quintptr>(entry));
}

Dialogs::Entry *LiveDelegate::FromId(uint64 id) {
	return reinterpret_cast<Dialogs::Entry*>(quintptr(id));
}

void LiveDelegate::refreshAccounts() {
	_watchLifetime.destroy();

	auto &domain = Core::App().domain();
	const auto colorIndex = [&](not_null<Main::Account*> account) {
		// The index of an account in the domain never changes.
		for (const auto &[index, value] : domain.accounts()) {
			if (value.get() == account.get()) {
				return index;
			}
		}
		return 0;
	};
	auto accounts = std::vector<std::unique_ptr<Account>>();
	for (const auto &account : domain.orderedAccounts()) {
		// Never destroys _watchLifetime from inside of its own callback.
		account->sessionChanges(
		) | rpl::on_next([=](Main::Session *session) {
			sessionChanged(account, session);
		}, _watchLifetime);

		if (!account->sessionExists()) {
			continue;
		}
		const auto session = &account->session();
		const auto i = ranges::find(
			_accounts,
			not_null(session),
			&Account::session);
		if (i != end(_accounts)) {
			accounts.push_back(std::move(*i));
			_accounts.erase(i);
		} else {
			accounts.push_back(std::make_unique<Account>(Account{
				.account = account,
				.session = session,
				.colorIndex = colorIndex(account),
			}));
			subscribe(accounts.back().get());
		}
	}
	_accounts = std::move(accounts);
	rebuild();
	_accountUpdates.fire({});
}

void LiveDelegate::sessionChanged(
		not_null<Main::Account*> account,
		Main::Session *session) {
	if (session) {
		// A new account has logged in.
		crl::on_main(this, [=] {
			refreshAccounts();
		});
		return;
	}
	// The session is still alive here, but only till the end of this
	// call: forget everything about its chats right now.
	const auto i = ranges::find(_accounts, account, &Account::account);
	if (i == end(_accounts)) {
		return;
	}
	_accounts.erase(i);
	rebuild();
	_accountUpdates.fire({});
}

void LiveDelegate::subscribe(not_null<Account*> account) {
	const auto session = account->session;
	const auto owner = &session->data();
	auto &lifetime = account->lifetime;

	session->user()->loadUserpic();

	rpl::merge(
		owner->chatsListChanges(),
		owner->chatsListLoadedEvents()
	) | rpl::on_next([=] {
		scheduleRebuild();
	}, lifetime);

	// Only the main list: not a folder and not the topics of a forum.
	using Refresh = Data::Session::ChatListEntryRefresh;
	owner->chatListEntryRefreshes(
	) | rpl::filter([](const Refresh &event) {
		return !event.filterId
			&& (event.key.history() || event.key.folder());
	}) | rpl::on_next([=] {
		scheduleRebuild();
	}, lifetime);

	session->changes().historyUpdates(
		Data::HistoryUpdate::Flag::IsPinned
		| Data::HistoryUpdate::Flag::Folder
		| Data::HistoryUpdate::Flag::TopPromoted
	) | rpl::on_next([=] {
		scheduleRebuild();
	}, lifetime);

	session->settings().archiveInMainMenuChanges(
	) | rpl::on_next([=] {
		scheduleRebuild();
	}, lifetime);

	session->changes().entryUpdates(
		Data::EntryUpdate::Flag::Repaint
	) | rpl::on_next([=](const Data::EntryUpdate &update) {
		updateRow(update.entry);
	}, lifetime);

	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::DialogRowRefresh
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		updateRow(update.item->history());
	}, lifetime);

	using PeerFlag = Data::PeerUpdate::Flag;
	session->changes().peerUpdates(
		PeerFlag::Name
		| PeerFlag::Photo
		| PeerFlag::EmojiStatus
		| PeerFlag::FullInfo
		| PeerFlag::StoriesState
		| PeerFlag::OnlineStatus
		| PeerFlag::GroupCall
		| PeerFlag::MessagesTTL
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		const auto peer = update.peer;
		if (peer->isSelf()
			&& (update.flags & (PeerFlag::Name | PeerFlag::Photo))) {
			_accountUpdates.fire({});
		}
		if (const auto history = peer->owner().historyLoaded(peer)) {
			if (update.flags
				& (PeerFlag::OnlineStatus
					| PeerFlag::GroupCall
					| PeerFlag::MessagesTTL)) {
				updateCornerBadge(history);
			}
			updateRow(history);
		}
	}, lifetime);

	owner->sendActionManager().animationUpdated(
	) | rpl::on_next([=](
			const Data::SendActionManager::AnimationUpdate &update) {
		updateRow(update.thread->owningHistory());
	}, lifetime);

	owner->sendActionManager().speakingAnimationUpdated(
	) | rpl::on_next([=](not_null<History*> history) {
		updateRow(history);
	}, lifetime);

	session->downloaderTaskFinished(
	) | rpl::on_next([=] {
		// The userpics are loaded.
		_rowUpdates.fire(-1);
		_accountUpdates.fire({});
	}, lifetime);
}

void LiveDelegate::scheduleRebuild() {
	if (_rebuildScheduled) {
		return;
	}
	_rebuildScheduled = true;
	crl::on_main(this, [=] {
		if (_rebuildScheduled) {
			rebuild();
		}
	});
}

bool LiveDelegate::applyAccountsOrder() {
	// Nothing tells about a new order of the accounts (they are dragged
	// in the main menu), so it is checked with every rebuild.
	const auto ordered = Core::App().domain().orderedAccounts();
	const auto position = [&](const std::unique_ptr<Account> &account) {
		return int(ranges::find(ordered, account->account) - begin(ordered));
	};
	if (ranges::is_sorted(_accounts, ranges::less(), position)) {
		return false;
	}
	ranges::stable_sort(_accounts, ranges::less(), position);
	return true;
}

void LiveDelegate::rebuild() {
	_rebuildScheduled = false;

	const auto reordered = applyAccountsOrder();

	auto total = 0;
	for (const auto &account : _accounts) {
		total += account->session->data().chatsList()->indexed()->size();
	}
	auto entries = std::vector<Entry>();
	entries.reserve(total);
	for (auto i = 0, count = int(_accounts.size()); i != count; ++i) {
		const auto session = _accounts[i]->session;
		const auto skipArchive = session->settings().archiveInMainMenu();
		for (const auto &row : *session->data().chatsList()->indexed()) {
			// The main list of an account has its chats and its archive.
			const auto entry = row->entry();
			if (skipArchive && entry->asFolder()) {
				continue;
			}
			entries.push_back({
				.entry = entry,
				.sort = {
					.key = entry->sortKeyInChatList(FilterId()),
					.account = i,
					.top = entry->isPinnedDialog(FilterId())
						|| (entry->fixedOnTopIndex() != 0),
				},
			});
		}
	}
	std::stable_sort(begin(entries), end(entries), [](
			const Entry &a,
			const Entry &b) {
		return SortBefore(a.sort, b.sort);
	});

	// Most of the rebuilds come from a new message in a chat that is on
	// its place already: the keys are remembered, nothing is repainted.
	const auto same = !reordered
		&& !entries.empty()
		&& ranges::equal(
			entries,
			_entries,
			[](const Entry &a, const Entry &b) {
				return (a.entry == b.entry)
					&& (a.sort.account == b.sort.account);
			});
	if (same) {
		_entries = std::move(entries);
		return;
	}

	_entries = std::move(entries);
	_indices.clear();
	_indices.reserve(_entries.size());
	for (auto i = 0, count = int(_entries.size()); i != count; ++i) {
		_indices.emplace(_entries[i].entry.get(), i);
	}
	for (auto i = begin(_rows); i != end(_rows);) {
		if (_indices.find(i->first) == end(_indices)) {
			i = _rows.erase(i);
		} else {
			++i;
		}
	}
	_listChanges.fire({});
	if (reordered) {
		_accountUpdates.fire({});
	}
}

int LiveDelegate::accountsCount() const {
	return int(_accounts.size());
}

AccountInfo LiveDelegate::accountInfo(int account) const {
	if (account < 0 || account >= accountsCount()) {
		return {};
	}
	const auto session = _accounts[account]->session;
	return {
		.name = session->user()->name(),
		.current = (session == &_controller->session()),
	};
}

int LiveDelegate::accountColorIndex(int account) const {
	return (account >= 0 && account < accountsCount())
		? _accounts[account]->colorIndex
		: 0;
}

void LiveDelegate::paintAccountUserpic(
		Painter &p,
		int account,
		int x,
		int y,
		int size) {
	if (account < 0 || account >= accountsCount()) {
		return;
	}
	const auto &data = _accounts[account];
	data->session->user()->paintUserpic(p, data->userpic, x, y, size, true);
}

void LiveDelegate::accountChosen(int account) {
	if (account < 0 || account >= accountsCount()) {
		return;
	}
	const auto session = _accounts[account]->session;
	if (session == &_controller->session()) {
		return;
	}
	// Switching the account destroys the header this call comes from.
	crl::on_main(session, [=] {
		Core::App().domain().maybeActivate(&session->account());
	});
}

int LiveDelegate::rowsCount() const {
	return int(_entries.size());
}

Dialogs::Entry *LiveDelegate::entryAt(int row) const {
	return (row >= 0 && row < rowsCount())
		? _entries[row].entry.get()
		: nullptr;
}

uint64 LiveDelegate::rowId(int row) const {
	return ToId(entryAt(row));
}

int LiveDelegate::rowIndex(uint64 id) const {
	const auto i = _indices.find(FromId(id));
	return (i != end(_indices)) ? i->second : -1;
}

int LiveDelegate::rowAccount(int row) const {
	return (row >= 0 && row < rowsCount()) ? _entries[row].sort.account : 0;
}

bool LiveDelegate::rowActive(int row) const {
	const auto entry = _active ? entryAt(row) : nullptr;
	return entry && (entry->asHistory() == _active);
}

bool LiveDelegate::local(not_null<Dialogs::Entry*> entry) const {
	return (&entry->session() == &_controller->session());
}

not_null<Dialogs::Row*> LiveDelegate::rowFor(
		not_null<Dialogs::Entry*> entry) {
	auto &row = _rows[entry.get()];
	if (!row) {
		row = std::make_unique<Dialogs::Row>(Dialogs::Key(entry), 0, 0);

		// All the rows are of the default height here (forums too).
		row->recountHeight(1., FilterId());
	}
	return row.get();
}

void LiveDelegate::paintRow(Painter &p, int row, const RowState &state) {
	const auto entry = entryAt(row);
	if (!entry) {
		return;
	}
	Dialogs::Ui::RowPainter::Paint(p, rowFor(entry), nullptr, {
		.st = &st::defaultDialogRow,
		.currentBg = st::dialogsBg,
		.now = state.now,
		.width = state.width,
		.active = state.active,
		.selected = state.selected,
		.paused = state.paused,
		.narrow = state.narrow,
	});
}

void LiveDelegate::updateRow(not_null<Dialogs::Entry*> entry) {
	const auto i = _indices.find(entry.get());
	if (i == end(_indices)) {
		return;
	}
	_rowUpdates.fire_copy(i->second);

	const auto top = entry->isPinnedDialog(FilterId())
		|| (entry->fixedOnTopIndex() != 0);
	if (SortStale(
			_entries[i->second].sort,
			entry->sortKeyInChatList(FilterId()),
			top)) {
		scheduleRebuild();
	}
}

void LiveDelegate::updateCornerBadge(not_null<History*> history) {
	const auto i = _rows.find(static_cast<Dialogs::Entry*>(history.get()));
	if (i != end(_rows)) {
		// The callback lives in the row, so not longer than this object.
		i->second->updateCornerBadgeShown(history->peer, [=] {
			updateRow(history);
		});
	}
}

void LiveDelegate::rowPressed(int row, QPoint point, QSize size) {
	if (const auto entry = entryAt(row)) {
		rowFor(entry)->addRipple(point, size, [=] {
			updateRow(entry);
		});
	}
}

void LiveDelegate::rowReleased(uint64 id) {
	const auto i = _rows.find(FromId(id));
	if (i != end(_rows)) {
		i->second->stopLastRipple();
	}
}

void LiveDelegate::rowChosen(int row, bool newWindow) {
	if (const auto entry = entryAt(row)) {
		choose(entry, newWindow);
	}
}

void LiveDelegate::choose(not_null<Dialogs::Entry*> entry, bool newWindow) {
	const auto history = entry->asHistory();
	const auto folder = entry->asFolder();
	if (!history && !folder) {
		return;
	} else if (local(entry)) {
		if (history) {
			if (_chooseLocal) {
				_chooseLocal(history, newWindow);
			}
		} else if (newWindow) {
			_controller->showInNewWindow(Window::SeparateId(
				Window::SeparateType::Archive,
				&_controller->session()));
		} else {
			_controller->openFolder(folder);
		}
		return;
	} else if (newWindow
		&& (folder || history->peer->computeUnavailableReason().isEmpty())) {
		const auto weak = base::make_weak(entry);
		crl::on_main(&entry->session(), [=] {
			const auto entry = weak.get();
			if (!entry) {
				return;
			} else if (const auto history = entry->asHistory()) {
				Core::App().ensureSeparateWindowFor(SeparateIdFor(history));
			} else {
				Core::App().ensureSeparateWindowFor(Window::SeparateId(
					Window::SeparateType::Archive,
					&entry->session()));
			}
		});
		return;
	}
	OpenInAccount(entry);
}

void LiveDelegate::fillRowMenu(int row, not_null<Ui::PopupMenu*> menu) {
	const auto entry = entryAt(row);
	if (!entry) {
		return;
	} else if (local(entry)) {
		Window::FillDialogsEntryMenu(
			_controller,
			Dialogs::EntryState{
				.key = Dialogs::Key(not_null(entry)),
				.section = Dialogs::EntryState::Section::ContextMenu,
			},
			Ui::Menu::CreateAddActionCallback(menu));
		return;
	}
	// The usual menu needs a window of that account.
	const auto weak = base::make_weak(entry);
	const auto open = [=](bool newWindow) {
		return crl::guard(this, [=] {
			if (const auto entry = weak.get()) {
				choose(entry, newWindow);
			}
		});
	};
	menu->addAction(
		tr::lng_oblivion_unified_open_in(
			tr::now,
			lt_name,
			entry->session().user()->name()),
		open(false),
		&st::menuIconShowInChat);
	menu->addAction(
		tr::lng_context_new_window(tr::now),
		open(true),
		&st::menuIconNewWindow);
}

bool LiveDelegate::rowAcceptsDrop(int row) const {
	// Messages and files of this window can't go to another account.
	const auto entry = entryAt(row);
	return entry && entry->asHistory() && local(entry);
}

bool LiveDelegate::dataAcceptsDrop(not_null<const QMimeData*> data) const {
	return data->hasFormat(u"application/x-td-forward"_q)
		|| (Storage::ComputeMimeDataState(data)
			!= Storage::MimeDataState::None);
}

void LiveDelegate::rowDropped(int row, not_null<const QMimeData*> data) {
	const auto entry = entryAt(row);
	const auto history = entry ? entry->asHistory() : nullptr;
	if (!history || !local(entry)) {
		return;
	}
	_controller->content()->filesOrForwardDrop(history, data);
	_controller->widget()->raise();
	_controller->widget()->activateWindow();
}

bool LiveDelegate::loading() const {
	for (const auto &account : _accounts) {
		if (!account->session->data().chatsList()->loaded()) {
			return true;
		}
	}
	return false;
}

rpl::producer<> LiveDelegate::listChanges() const {
	return _listChanges.events();
}

rpl::producer<int> LiveDelegate::rowUpdates() const {
	return _rowUpdates.events();
}

rpl::producer<> LiveDelegate::accountUpdates() const {
	return _accountUpdates.events();
}

Host::Host(AttachArgs &&args)
: _dialogs(args.dialogs)
, _controller(args.controller)
, _scroll(args.scroll.get())
, _window(args.controller->widget().get())
, _plain(std::move(args.plain))
, _choose(std::move(args.choose))
, _collapseStories(std::move(args.collapseStories)) {
	Hosts().push_back(this);

	if (args.topInset) {
		_topInset = std::move(args.topInset);
	}
	if (args.storiesExpanded) {
		std::move(
			args.storiesExpanded
		) | rpl::on_next([=](int height) {
			_stories.heightChanged(
				height,
				_panel ? _panel->scrollTop() : 0);
		}, _lifetime);
	}

	Core::App().domain().accountsChanges(
	) | rpl::on_next([=] {
		watchAccounts();
		scheduleRefresh();
	}, _lifetime);
	watchAccounts();

	rpl::merge(
		Get().changes(),
		_controller->activeChatsFilter() | rpl::skip(1) | rpl::to_empty,
		(args.plainChanges
			? std::move(args.plainChanges)
			: rpl::producer<>(rpl::never<>()))
	) | rpl::on_next([=] {
		refreshNow();
	}, _lifetime);
}

Host::~Host() {
	auto &hosts = Hosts();
	hosts.erase(ranges::remove(hosts, not_null(this)), end(hosts));
}

Host *Host::Find(not_null<QWidget*> dialogs) {
	for (const auto &host : Hosts()) {
		if (host->_dialogs == dialogs) {
			return host;
		}
	}
	return nullptr;
}

Host *Host::Find(not_null<Window::SessionController*> controller) {
	for (const auto &host : Hosts()) {
		if (host->_controller == controller) {
			return host;
		}
	}
	return nullptr;
}

void Host::watchAccounts() {
	_accountsLifetime.destroy();
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		account->sessionChanges(
		) | rpl::on_next([=] {
			scheduleRefresh();
		}, _accountsLifetime);
	}
}

void Host::scheduleRefresh() {
	if (_refreshScheduled) {
		return;
	}
	_refreshScheduled = true;
	crl::on_main(this, [=] {
		_refreshScheduled = false;
		refresh(true);
	});
}

void Host::refreshNow() {
	// The panel is shown and hidden right away, so the grabs for the slide
	// animations of the chats list have the right content. The state is
	// checked once more in a queued call, when the change is complete.
	refresh(false);
	scheduleRefresh();
}

void Host::refresh(bool queued) {
	// Only the windows with the main chats list of an account: a separate
	// window of the archive, of a forum or of a community never has it.
	if (!_scroll || !_controller->isPrimary() || !Active()) {
		if (!_panel) {
			return;
		} else if (queued) {
			_panel = nullptr;
		} else {
			// This may be a call from an event handler of the panel,
			// it is destroyed in the queued call.
			_panel->hide();
		}
		return;
	}
	// While the mode is on the panel is kept in the other states of the
	// chats list (search, archive, folder, forum), only hidden.
	const auto shown = !_controller->activeChatsFilterCurrent()
		&& (!_plain || _plain());
	if (!_panel) {
		if (!shown) {
			return;
		}
		create();
	}
	if (_panel->isHidden() == shown) {
		_panel->setVisible(shown);
	}
}

void Host::create() {
	const auto controller = _controller;
	auto delegate = std::make_unique<LiveDelegate>(
		controller,
		crl::guard(this, [=](not_null<History*> history, bool newWindow) {
			choose(history, newWindow);
		}));
	_panel = base::make_unique_q<Panel>(
		_scroll.data(),
		std::move(delegate),
		PanelArgs{
			.turnOff = [=] {
				Get().setUnifiedChats(false);
				controller->showToast(
					tr::lng_oblivion_unified_off_toast(tr::now));
			},
			.paused = [=] {
				return controller->isGifPausedAtLeastFor(
					Window::GifPauseReason::Any);
			},
		});
	const auto raw = _panel.get();
	rpl::combine(
		_scroll->sizeValue(),
		_topInset.value()
	) | rpl::on_next([=](QSize size, int inset) {
		const auto top = std::clamp(inset, 0, size.height());
		raw->setGeometry(0, top, size.width(), size.height() - top);
	}, raw->lifetime());

	// Over the scroll bar and the jump to top button of the covered list.
	raw->raise();

	// The window stays when its account is switched (a click on a chat of
	// another account does that), the new list continues from the place
	// where the previous one was.
	const auto window = _window;
	const auto saved = window
		? window->property(kScrollProperty).toInt()
		: 0;
	raw->scrollTopChanges(
	) | rpl::on_next([=](int top) {
		if (window) {
			window->setProperty(kScrollProperty, top);
		}
		if (_collapseStories
			&& _stories.scrolledAway(top, st::dialogsRowHeight)) {
			_collapseStories();
		}
	}, raw->lifetime());
	raw->restoreScroll(saved);
}

bool Host::handleKey(not_null<QKeyEvent*> e) {
	return _panel && _panel->isVisible() && _panel->handleKey(e);
}

void Host::choose(not_null<History*> history, bool newWindow) {
	if (_choose) {
		_choose(history, newWindow);
	}
}

// The sample chats for the snapshot scenes.
class SampleDelegate final : public Delegate {
public:
	// Without the chats it shows the empty list.
	explicit SampleDelegate(bool chats = true);

	int accountsCount() const override;
	AccountInfo accountInfo(int account) const override;
	void paintAccountUserpic(
		Painter &p,
		int account,
		int x,
		int y,
		int size) override;

	int rowsCount() const override;
	uint64 rowId(int row) const override;
	int rowIndex(uint64 id) const override;
	int rowAccount(int row) const override;
	bool rowActive(int row) const override;
	void paintRow(Painter &p, int row, const RowState &state) override;

private:
	enum class Type : uchar {
		User,
		Group,
		Channel,
	};
	struct Account {
		QString name;
		std::unique_ptr<Ui::EmptyUserpic> userpic;
	};
	struct Row {
		int account = 0;
		Type type = Type::User;
		QString date;
		int unread = 0;
		bool muted = false;
		bool pinned = false;
		bool online = false;
		bool active = false;
		std::unique_ptr<Ui::EmptyUserpic> userpic;
		Ui::Text::String name;
		Ui::Text::String text;
	};

	void addAccount(const QString &name, uint8 color);
	void addRow(
		int account,
		Type type,
		uint8 color,
		const QString &name,
		const QString &from,
		const QString &text,
		const QString &date,
		int unread = 0,
		bool muted = false,
		bool pinned = false,
		bool online = false,
		bool active = false);

	std::vector<Account> _accounts;
	std::vector<Row> _rows;

};

SampleDelegate::SampleDelegate(bool chats) {
	// Not the colors of AccountColor(): a real userpic rarely matches the
	// color of its account, so the ring around it in the header is seen.
	addAccount(u"Матвей Орлов"_q, 6);
	addAccount(u"Студия «Север»"_q, 0);
	addAccount(u"Второй номер"_q, 2);
	if (!chats) {
		return;
	}

	// The nine rows of the scenes have: the pinned icon, the online badge
	// with and without a counter (the narrow list hides it under the
	// counter), a counter of four digits (the widest badges of the narrow
	// list), a muted one, the active chat, a sender name, a name and a
	// text that don't fit.
	using enum Type;
	addRow(0, User, 4, u"Мама"_q, QString(),
		u"Позвони, как освободишься"_q, u"12:41"_q, 0, false, true, true);
	addRow(1, Group, 2, u"Команда дизайна"_q, u"Аня"_q,
		u"Макеты обновила, посмотрите"_q, u"12:30"_q, 12, false, true);
	addRow(2, Channel, 6, u"Новости Oblivion"_q, QString(),
		u"Вышла сборка 7.0.9 с общим списком чатов"_q, u"12:18"_q, 1248,
		true);
	addRow(0, User, 0, u"Илья Смирнов"_q, QString(),
		u"Хорошо, тогда до завтра"_q, u"11:57"_q, 0, false, false, false,
		true);
	addRow(1, User, 5, u"Ольга, бухгалтерия"_q, QString(),
		u"Счёт отправила на почту"_q, u"11:20"_q, 2, false, false, true);
	addRow(0, Group, 3, u"Поездка в Петербург"_q, u"Вы"_q,
		u"Билеты купил, выезжаем в пятницу"_q, u"10:48"_q);
	addRow(2, User, 5, u"Служба доставки"_q, QString(),
		u"Курьер приедет с 14:00 до 16:00"_q, u"09:15"_q, 1);
	addRow(1, Channel, 4, u"Вакансии дизайн-студии «Север» и партнёров"_q,
		QString(), u"Ищем моушн-дизайнера в команду"_q, u"08:02"_q, 0, true);
	addRow(0, User, 2, u"Лена"_q, QString(),
		u"Спасибо, всё получила!"_q, u"Вт"_q, 0, false, false, true);
	addRow(2, Group, 0, u"Соседи, дом 12"_q, u"Пётр"_q,
		u"Завтра с 10 до 12 отключат воду"_q, u"Вт"_q, 27, true);
	addRow(1, User, 4, u"Дмитрий, заказчик"_q, QString(),
		u"Правки пришлю вечером"_q, u"Пн"_q);
	addRow(0, Channel, 1, u"Книжный клуб"_q, QString(),
		u"Читаем в октябре: «Пикник на обочине»"_q, u"Пн"_q, 5, true);
}

void SampleDelegate::addAccount(const QString &name, uint8 color) {
	_accounts.push_back({
		.name = name,
		.userpic = std::make_unique<Ui::EmptyUserpic>(
			Ui::EmptyUserpic::UserpicColor(color),
			name),
	});
}

void SampleDelegate::addRow(
		int account,
		Type type,
		uint8 color,
		const QString &name,
		const QString &from,
		const QString &text,
		const QString &date,
		int unread,
		bool muted,
		bool pinned,
		bool online,
		bool active) {
	auto row = Row{
		.account = account,
		.type = type,
		.date = date,
		.unread = unread,
		.muted = muted,
		.pinned = pinned,
		.online = online,
		.active = active,
		.userpic = std::make_unique<Ui::EmptyUserpic>(
			Ui::EmptyUserpic::UserpicColor(color),
			name),
	};
	row.name.setText(st::semiboldTextStyle, name, Ui::NameTextOptions());
	auto marked = TextWithEntities();
	if (!from.isEmpty()) {
		marked.append(Ui::Text::Colorized(from + ':')).append(' ');
	}
	marked.append(text);
	row.text.setMarkedText(
		st::dialogsTextStyle,
		marked,
		Ui::DialogTextOptions());
	_rows.push_back(std::move(row));
}

int SampleDelegate::accountsCount() const {
	return int(_accounts.size());
}

AccountInfo SampleDelegate::accountInfo(int account) const {
	return (account >= 0 && account < accountsCount())
		? AccountInfo{
			.name = _accounts[account].name,
			.current = (account == 0),
		}
		: AccountInfo();
}

void SampleDelegate::paintAccountUserpic(
		Painter &p,
		int account,
		int x,
		int y,
		int size) {
	if (account >= 0 && account < accountsCount()) {
		_accounts[account].userpic->paintCircle(p, x, y, x + size, size);
	}
}

int SampleDelegate::rowsCount() const {
	return int(_rows.size());
}

uint64 SampleDelegate::rowId(int row) const {
	return uint64(row + 1);
}

int SampleDelegate::rowIndex(uint64 id) const {
	return (id > 0 && id <= uint64(_rows.size())) ? int(id - 1) : -1;
}

int SampleDelegate::rowAccount(int row) const {
	return (row >= 0 && row < rowsCount()) ? _rows[row].account : 0;
}

bool SampleDelegate::rowActive(int row) const {
	return (row >= 0 && row < rowsCount()) && _rows[row].active;
}

// A simplified copy of the layout of Dialogs::Ui::RowPainter, which needs
// a real chat of a real session.
void SampleDelegate::paintRow(Painter &p, int index, const RowState &state) {
	if (index < 0 || index >= rowsCount()) {
		return;
	}
	const auto &row = _rows[index];
	const auto &st = st::defaultDialogRow;
	const auto active = state.active;
	const auto selected = state.selected;
	const auto width = state.width;

	p.fillRect(0, 0, width, st.height, RowBg(active, selected));
	row.userpic->paintCircle(
		p,
		st.padding.left(),
		st.padding.top(),
		width,
		st.photoSize);
	// The same geometry as in Dialogs::Row::PaintCornerBadgeFrame: the
	// stroke is cut out of the badge, half inside and half outside of it.
	// In the narrow list the unread badge replaces the online one.
	if (row.online && !(state.narrow && row.unread)) {
		auto hq = PainterHighQualityEnabler(p);
		const auto size = st::dialogsOnlineBadgeSize;
		const auto stroke = st::dialogsOnlineBadgeStroke;
		const auto skip = st::dialogsOnlineBadgeSkip;
		auto pen = QPen(RowBg(active, selected)->c);
		pen.setWidthF(stroke);
		p.setPen(pen);
		p.setBrush(active
			? st::dialogsOnlineBadgeFgActive
			: st::dialogsOnlineBadgeFg);
		p.drawEllipse(QRectF(
			st.padding.left() + st.photoSize - skip.x() - size,
			st.padding.top() + st.photoSize - skip.y() - size,
			size,
			size));
	}

	auto badge = Ui::UnreadBadgeStyle();
	badge.active = active;
	badge.selected = selected;
	badge.muted = row.muted;
	const auto counter = QString::number(row.unread);
	if (state.narrow) {
		// The whole counter, as in Dialogs::Ui::PaintNarrowCounter.
		if (row.unread) {
			Ui::PaintUnreadBadge(
				p,
				counter,
				st.padding.left() + st.photoSize,
				st.padding.top() + st.photoSize - st::dialogsUnreadHeight,
				badge);
		}
		return;
	}

	const auto nameLeft = st.nameLeft;
	const auto nameWidth = width - nameLeft - st.padding.right();
	auto nameRect = QRect(
		nameLeft,
		st.nameTop,
		nameWidth,
		st::semiboldFont->height);

	// The date.
	const auto dateWidth = st::dialogsDateFont->width(row.date);
	nameRect.setWidth(nameRect.width() - dateWidth - st::dialogsDateSkip);
	p.setFont(st::dialogsDateFont);
	p.setPen(active
		? st::dialogsDateFgActive
		: selected
		? st::dialogsDateFgOver
		: st::dialogsDateFg);
	p.drawText(
		nameRect.x() + nameRect.width() + st::dialogsDateSkip,
		nameRect.y() + st::semiboldFont->height - st::normalFont->descent,
		row.date);

	// The chat type icon and the name.
	if (row.type != Type::User) {
		const auto &icon = Dialogs::ThreeStateIcon(
			(row.type == Type::Channel)
				? st::dialogsChannelIcon
				: st::dialogsChatIcon,
			active,
			selected);
		icon.paint(p, nameRect.topLeft(), width);
		nameRect.setLeft(nameRect.x()
			+ icon.width()
			+ st::dialogsChatTypeSkip);
	}
	p.setPen(active
		? st::dialogsNameFgActive
		: selected
		? st::dialogsNameFgOver
		: st::dialogsNameFg);
	row.name.draw(p, {
		.position = nameRect.topLeft(),
		.availableWidth = nameRect.width(),
		.elisionLines = 1,
	});

	// The unread badge or the pinned icon, the last message.
	auto textWidth = nameWidth;
	if (row.unread) {
		const auto top = st.textTop
			+ st::dialogsTextFont->ascent
			- st::dialogsUnreadFont->ascent
			- (st::dialogsUnreadHeight - st::dialogsUnreadFont->height) / 2;
		const auto rect = Ui::PaintUnreadBadge(
			p,
			counter,
			width - st.padding.right(),
			top,
			badge);
		textWidth -= rect.width() + badge.padding;
	} else if (row.pinned) {
		const auto &icon = Dialogs::ThreeStateIcon(
			st::dialogsPinnedIcon,
			active,
			selected);
		icon.paint(
			p,
			width - st.padding.right() - icon.width(),
			st.textTop,
			width);
		textWidth -= icon.width() + st::dialogsUnreadPadding;
	}
	p.setFont(st::dialogsTextFont);
	p.setPen(active
		? st::dialogsTextFgActive
		: selected
		? st::dialogsTextFgOver
		: st::dialogsTextFg);
	row.text.draw(p, {
		.position = QPoint(nameLeft, st.textTop),
		.availableWidth = textWidth,
		.palette = &(active
			? st::dialogsTextPaletteActive
			: selected
			? st::dialogsTextPaletteOver
			: st::dialogsTextPalette),
		.now = state.now,
		.elisionLines = 1,
	});
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto make = [](bool chats) {
		return [=](not_null<Ui::RpWidget*> parent) -> QWidget* {
			return Ui::CreateChild<Panel>(
				parent.get(),
				std::make_unique<SampleDelegate>(chats),
				PanelArgs{ .turnOff = [] {} });
		};
	};
	const auto create = make(true);
	const auto rows = st::dialogsImportantBarHeight
		+ 9 * st::defaultDialogRow.height;

	// The chats of three accounts in the usual chats list column.
	RegisterScene(
		u"unified_list"_q,
		QSize(style::ConvertScale(380), rows),
		create);

	// The same with a row under the cursor.
	RegisterScene(
		u"unified_list_hover"_q,
		QSize(style::ConvertScale(380), rows),
		create,
		[](not_null<QWidget*> widget) {
			static_cast<Panel*>(widget.get())->selectRow(1);
		});

	// The same in the narrow column: only the userpics, no header.
	const auto &row = st::defaultDialogRow;
	RegisterScene(
		u"unified_list_narrow"_q,
		QSize(row.padding.left() + row.photoSize + row.padding.left(), rows),
		create);

	// No chats in any of the accounts: the header and the empty list.
	RegisterScene(
		u"unified_list_empty"_q,
		QSize(
			style::ConvertScale(380),
			st::dialogsImportantBarHeight + st::dialogsEmptyHeight + row.height),
		make(false));
});

} // namespace

bool Active() {
	if (!Get().unifiedChats()) {
		return false;
	}
	auto authorized = 0;
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (account->sessionExists()) {
			++authorized;
		}
	}
	return (authorized > 1);
}

void Attach(AttachArgs &&args) {
	const auto dialogs = args.dialogs;
	const auto host = dialogs->lifetime().make_state<Host>(std::move(args));
	host->refreshNow();
}

void Refresh(not_null<QWidget*> dialogs) {
	if (const auto host = Host::Find(dialogs)) {
		host->refreshNow();
	}
}

bool HandleKey(not_null<QWidget*> dialogs, not_null<QKeyEvent*> e) {
	const auto host = Host::Find(dialogs);
	return host && host->handleKey(e);
}

void AddMainMenuToggle(not_null<Ui::VerticalLayout*> container) {
	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			container,
			::Settings::CreateButtonWithIcon(
				container,
				tr::lng_oblivion_unified_menu(),
				st::mainMenuButton,
				{ &st::menuIconChats })));
	wrap->setDuration(0);
	wrap->toggleOn(rpl::single(
		rpl::empty
	) | rpl::then(
		Core::App().domain().accountsChanges()
	) | rpl::map([] {
		return (Core::App().domain().accountsAuthedCount() > 1);
	}));

	const auto button = wrap->entity();
	button->toggleOn(rpl::single(
		rpl::empty
	) | rpl::then(
		Get().changes()
	) | rpl::map([] {
		return Get().unifiedChats();
	}));
	button->toggledChanges(
	) | rpl::filter([](bool value) {
		return (value != Get().unifiedChats());
	}) | rpl::on_next([](bool value) {
		Get().setUnifiedChats(value);
	}, button->lifetime());
}

bool RunSelfTest(QStringList &log) {
	auto failed = 0;
	const auto check = [&](bool condition, const QString &name) {
		log.push_back(u"unified_chats: "_q
			+ (condition ? u"OK   "_q : u"FAIL "_q)
			+ name);
		if (!condition) {
			++failed;
		}
	};
	const auto sorted = [](std::vector<SortItem> list) {
		std::stable_sort(begin(list), end(list), SortBefore);
		return list;
	};
	const auto same = [](
			const std::vector<SortItem> &list,
			const std::vector<std::pair<int, uint64>> &expected) {
		if (list.size() != expected.size()) {
			return false;
		}
		for (auto i = 0, count = int(list.size()); i != count; ++i) {
			if (list[i].account != expected[i].first
				|| list[i].key != expected[i].second) {
				return false;
			}
		}
		return true;
	};

	// The keys as Dialogs::Entry makes them.
	const auto pinned = [](int index) {
		return 0xFFFFFFFF000000FFULL - index;
	};
	const auto dated = [](uint32 date, uint32 counter) {
		return (uint64(date) << 32) | counter;
	};

	// Three accounts, each list is sorted the way its session keeps it.
	const auto merged = sorted({
		{ pinned(1), 0, true },
		{ pinned(2), 0, true },
		{ dated(1000, 7), 0, false },
		{ dated(400, 2), 0, false },
		{ pinned(1), 1, true },
		{ dated(900, 9), 1, false },
		{ dated(900, 3), 1, false },
		{ dated(1200, 11), 2, false },
		{ dated(100, 1), 2, false },
	});
	check(same(merged, {
		{ 0, pinned(1) },
		{ 0, pinned(2) },
		{ 1, pinned(1) },
		{ 2, dated(1200, 11) },
		{ 0, dated(1000, 7) },
		{ 1, dated(900, 9) },
		{ 1, dated(900, 3) },
		{ 0, dated(400, 2) },
		{ 2, dated(100, 1) },
	}), u"pinned by accounts, then by date"_q);

	// The pinned chats of an account keep their place whichever account
	// is the active one: the order depends only on the accounts order.
	const auto swapped = sorted({
		{ pinned(1), 1, true },
		{ dated(900, 9), 1, false },
		{ pinned(1), 0, true },
		{ dated(1000, 7), 0, false },
	});
	check(same(swapped, {
		{ 0, pinned(1) },
		{ 1, pinned(1) },
		{ 0, dated(1000, 7) },
		{ 1, dated(900, 9) },
	}), u"the order doesn't depend on the input order"_q);

	// A chat without messages (the zero key) goes last, equal keys keep
	// the accounts order.
	const auto edge = sorted({
		{ 0, 1, false },
		{ dated(500, 5), 1, false },
		{ dated(500, 5), 0, false },
		{ 0, 0, false },
	});
	check(same(edge, {
		{ 0, dated(500, 5) },
		{ 1, dated(500, 5) },
		{ 0, 0 },
		{ 1, 0 },
	}), u"equal and empty keys"_q);

	// Unread on top (the experimental option) sets the high bit of the
	// key, such chats stay below the pinned ones of all the accounts.
	const auto unreadOnTop = dated(300, 4) | 0x8000000000000000ULL;
	const auto option = sorted({
		{ dated(1000, 7), 0, false },
		{ unreadOnTop, 1, false },
		{ pinned(3), 1, true },
	});
	check(same(option, {
		{ 1, pinned(3) },
		{ 1, unreadOnTop },
		{ 0, dated(1000, 7) },
	}), u"unread on top keys"_q);

	check(!SortBefore({ 5, 0, false }, { 5, 0, false }),
		u"the order is strict"_q);

	// A new message in the newest chat of an account: the chat keeps its
	// place in the list of that account, only its key tells about it.
	const auto waiting = SortItem{ dated(1230, 4), 1, false };
	check(!SortStale(waiting, dated(1230, 4), false)
		&& SortStale(waiting, dated(1300, 12), false)
		&& SortStale(waiting, dated(1230, 4), true),
		u"a changed key or pin is noticed"_q);
	const auto lifted = sorted({
		{ dated(1250, 8), 0, false },
		{ dated(1240, 6), 0, false },
		{ dated(1300, 12), 1, false },
	});
	check(same(lifted, {
		{ 1, dated(1300, 12) },
		{ 0, dated(1250, 8) },
		{ 0, dated(1240, 6) },
	}), u"a new message lifts the chat of another account"_q);

	// The stories expanded by a click are collapsed by scrolling the list
	// down by more than a row from the place where they were expanded.
	const auto row = 62;
	auto stories = StoriesCollapse();
	stories.heightChanged(0, 0);
	const auto collapsedStays = !stories.scrolledAway(500, row);
	stories.heightChanged(30, 500);
	stories.heightChanged(77, 500);
	check(collapsedStays
		&& !stories.scrolledAway(500 + row, row)
		&& !stories.scrolledAway(100, row)
		&& stories.scrolledAway(500 + row + 1, row),
		u"stories collapse after a row of scrolling down"_q);
	stories.heightChanged(77, 800);
	check(!stories.scrolledAway(500 + row, row)
		&& stories.scrolledAway(500 + row + 1, row),
		u"the same stories height sent again changes nothing"_q);
	stories.heightChanged(40, 600);
	const auto collapsing = !stories.scrolledAway(900, row);
	stories.heightChanged(77, 900);
	check(collapsing
		&& !stories.scrolledAway(900 + row, row)
		&& stories.scrolledAway(900 + row + 1, row),
		u"stories expanded again start from the new place"_q);

	return !failed;
}

} // namespace Oblivion::UnifiedChats
