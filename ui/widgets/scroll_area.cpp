// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/widgets/scroll_area.h"

#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "base/invoke_queued.h"
#include "base/qt/qt_common_adapters.h"
#include "base/debug_log.h"

#include <QtWidgets/QScrollBar>
#include <QtWidgets/QScroller>
#include <QtWidgets/QApplication>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Ui {
namespace {

[[nodiscard]] int ComputeScrollTo(
		int toFrom,
		int toTill,
		int toMin,
		int toMax,
		int current,
		int size) {
	if (toFrom < toMin) {
		toFrom = toMin;
	} else if (toFrom > toMax) {
		toFrom = toMax;
	}
	const auto exact = (toTill < 0);

	const auto curBottom = current + size;
	auto scToFrom = toFrom;
	if (!exact && toFrom >= current) {
		if (toTill < toFrom) {
			toTill = toFrom;
		}
		if (toTill <= curBottom) {
			return current;
		}

		scToFrom = toTill - size;
		if (scToFrom > toFrom) {
			scToFrom = toFrom;
		}
		if (scToFrom == current) {
			return current;
		}
	} else {
		scToFrom = toFrom;
	}
	return scToFrom;
}

[[nodiscard]] bool IsMouseFromTouch(Qt::MouseEventSource source) {
	return source == Qt::MouseEventSynthesizedBySystem
		|| source == Qt::MouseEventSynthesizedByQt;
}

} // namespace

// flick scroll taken from http://qt-project.org/doc/qt-4.8/demos-embedded-anomaly-src-flickcharm-cpp.html

ScrollShadow::ScrollShadow(ScrollArea *parent, const style::ScrollArea *st)
: QWidget(parent)
, _st(st) {
	Expects(_st != nullptr);
	Expects(_st->shColor.get() != nullptr);

	setVisible(false);
}

void ScrollShadow::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.fillRect(rect(), _st->shColor);
}

void ScrollShadow::changeVisibility(bool shown) {
	setVisible(shown);
}

ScrollBar::ScrollBar(
	ScrollArea *parent,
	bool vert,
	const style::ScrollArea *st)
: RpWidget(parent)
, _st(st)
, _vertical(vert)
, _hiding(_st->hiding != 0)
, _connected(vert ? parent->verticalScrollBar() : parent->horizontalScrollBar())
, _scrollMax(_connected->maximum())
, _hideTimer([=] { hideTimer(); }) {
	recountSize();

	connect(_connected, &QAbstractSlider::valueChanged, [=] {
		area()->scrolled();
		updateBar();
	});
	connect(_connected, &QAbstractSlider::rangeChanged, [=] {
		area()->innerResized();
		updateBar();
	});

	updateBar();
}

void ScrollBar::recountSize() {
	setGeometry(_vertical
		? QRect(
			style::RightToLeft() ? 0 : (area()->width() - _st->width),
			_st->deltat,
			_st->width,
			area()->height() - _st->deltat - _st->deltab)
		: QRect(
			_st->deltat,
			area()->height() - _st->width,
			area()->width() - _st->deltat - _st->deltab,
			_st->width));
}

void ScrollBar::updateBar(bool force) {
	QRect newBar;
	if (_connected->maximum() != _scrollMax) {
		const auto oldMax = _scrollMax;
		const auto newMax = _connected->maximum();
		_scrollMax = newMax;
		area()->rangeChanged(oldMax, newMax, _vertical);
	}
	if (_vertical) {
		const auto sh = area()->scrollHeight();
		const auto rh = height();
		auto h = sh ? int32((rh * int64(area()->height())) / sh) : 0;
		if (_st->barHidden
			|| h >= rh
			|| !area()->scrollTopMax()
			|| rh < _st->minHeight) {
			if (!isHidden()) {
				hide();
			}
			const auto newTopSh = (_st->topsh < 0);
			const auto newBottomSh = (_st->bottomsh < 0);
			if (newTopSh != _topSh || force) {
				_shadowVisibilityChanged.fire({
					.type = ScrollShadow::Type::Top,
					.visible = (_topSh = newTopSh),
				});
			}
			if (newBottomSh != _bottomSh || force) {
				_shadowVisibilityChanged.fire({
					.type = ScrollShadow::Type::Bottom,
					.visible = (_bottomSh = newBottomSh),
				});
			}
			return;
		}

		if (h <= _st->minHeight) {
			h = _st->minHeight;
		}
		const auto stm = area()->scrollTopMax();
		const auto y = stm
			? std::min(
				int32(((rh - h) * int64(area()->scrollTop())) / stm),
				rh - h)
			: 0;

		newBar = QRect(_st->deltax, y, width() - 2 * _st->deltax, h);
	} else {
		const auto sw = area()->scrollWidth();
		const auto rw = width();
		auto w = sw ? int32((rw * int64(area()->width())) / sw) : 0;
		if (_st->barHidden
			|| w >= rw
			|| !area()->scrollLeftMax()
			|| rw < _st->minHeight) {
			if (!isHidden()) {
				hide();
			}
			return;
		}

		if (w <= _st->minHeight) {
			w = _st->minHeight;
		}
		const auto slm = area()->scrollLeftMax();
		const auto x = slm
			? std::min(
				int32(((rw - w) * int64(area()->scrollLeft())) / slm),
				rw - w)
			: 0;

		newBar = QRect(x, _st->deltax, w, height() - 2 * _st->deltax);
	}
	if (newBar != _bar) {
		_bar = newBar;
		update();
	}
	if (_vertical) {
		const auto newTopSh = (_st->topsh < 0)
			|| (area()->scrollTop() > _st->topsh);
		const auto newBottomSh = (_st->bottomsh < 0)
			|| (area()->scrollTop()
				< area()->scrollTopMax() - _st->bottomsh);
		if (newTopSh != _topSh || force) {
			_shadowVisibilityChanged.fire({
				.type = ScrollShadow::Type::Top,
				.visible = (_topSh = newTopSh),
			});
		}
		if (newBottomSh != _bottomSh || force) {
			_shadowVisibilityChanged.fire({
				.type = ScrollShadow::Type::Bottom,
				.visible = (_bottomSh = newBottomSh),
			});
		}
	}
	if (isHidden()) show();
}

void ScrollBar::hideTimer() {
	if (!_hiding) {
		_hiding = true;
		_a_opacity.start([this] { update(); }, 1., 0., _st->duration);
	}
}

ScrollArea *ScrollBar::area() {
	return static_cast<ScrollArea*>(parentWidget());
}

void ScrollBar::setOver(bool over) {
	if (_over != over) {
		auto wasOver = (_over || _moving);
		_over = over;
		auto nowOver = (_over || _moving);
		if (wasOver != nowOver) {
			_a_over.start([this] { update(); }, nowOver ? 0. : 1., nowOver ? 1. : 0., _st->duration);
		}
		if (nowOver && _hiding) {
			_hiding = false;
			_a_opacity.start([this] { update(); }, 0., 1., _st->duration);
		}
	}
}

void ScrollBar::setOverBar(bool overbar) {
	if (_overbar != overbar) {
		auto wasBarOver = (_overbar || _moving);
		_overbar = overbar;
		auto nowBarOver = (_overbar || _moving);
		if (wasBarOver != nowBarOver) {
			_a_barOver.start([this] { update(); }, nowBarOver ? 0. : 1., nowBarOver ? 1. : 0., _st->duration);
		}
	}
}

void ScrollBar::setMoving(bool moving) {
	if (_moving != moving) {
		auto wasOver = (_over || _moving);
		auto wasBarOver = (_overbar || _moving);
		_moving = moving;
		auto nowBarOver = (_overbar || _moving);
		if (wasBarOver != nowBarOver) {
			_a_barOver.start([this] { update(); }, nowBarOver ? 0. : 1., nowBarOver ? 1. : 0., _st->duration);
		}
		auto nowOver = (_over || _moving);
		if (wasOver != nowOver) {
			_a_over.start([this] { update(); }, nowOver ? 0. : 1., nowOver ? 1. : 0., _st->duration);
		}
		if (!nowOver && _st->hiding && !_hiding) {
			_hideTimer.callOnce(_hideIn);
		}
	}
}

void ScrollBar::paintEvent(QPaintEvent *e) {
	if (!_bar.width() && !_bar.height()) {
		hide();
		return;
	}
	auto opacity = _a_opacity.value(_hiding ? 0. : 1.);
	if (opacity == 0.) return;

	QPainter p(this);
	auto deltal = _vertical ? _st->deltax : 0, deltar = _vertical ? _st->deltax : 0;
	auto deltat = _vertical ? 0 : _st->deltax, deltab = _vertical ? 0 : _st->deltax;
	p.setPen(Qt::NoPen);
	auto bg = anim::color(_st->bg, _st->bgOver, _a_over.value((_over || _moving) ? 1. : 0.));
	bg.setAlpha(anim::interpolate(0, bg.alpha(), opacity));
	auto bar = anim::color(_st->barBg, _st->barBgOver, _a_barOver.value((_overbar || _moving) ? 1. : 0.));
	bar.setAlpha(anim::interpolate(0, bar.alpha(), opacity));
	const auto outer = QRect(deltal, deltat, width() - deltal - deltar, height() - deltat - deltab);
	const auto radius = (_st->round < 0)
		? (std::min(outer.width(), outer.height()) / 2.)
		: _st->round;
	if (radius) {
		PainterHighQualityEnabler hq(p);
		p.setBrush(bg);
		p.drawRoundedRect(outer, radius, radius);
		p.setBrush(bar);
		p.drawRoundedRect(_bar, radius, radius);
	} else {
		p.fillRect(outer, bg);
		p.fillRect(_bar, bar);
	}
}

void ScrollBar::hideTimeout(crl::time dt) {
	if (_hiding && dt > 0) {
		_hiding = false;
		_a_opacity.start([this] { update(); }, 0., 1., _st->duration);
	}
	_hideIn = dt;
	if (!_moving) {
		_hideTimer.callOnce(_hideIn);
	}
}

void ScrollBar::enterEventHook(QEnterEvent *e) {
	_hideTimer.cancel();
	setMouseTracking(true);
	setOver(true);
}

void ScrollBar::leaveEventHook(QEvent *e) {
	if (!_moving) {
		setMouseTracking(false);
	}
	setOver(false);
	setOverBar(false);
	if (_st->hiding && !_hiding) {
		_hideTimer.callOnce(_hideIn);
	}
}

void ScrollBar::mouseMoveEvent(QMouseEvent *e) {
	setOverBar(_bar.contains(e->pos()));
	if (_moving) {
		int delta = 0, barDelta = _vertical ? (area()->height() - _bar.height()) : (area()->width() - _bar.width());
		if (barDelta > 0) {
			QPoint d = (e->globalPos() - _dragStart);
			delta = int32((_vertical ? (d.y() * int64(area()->scrollTopMax())) : (d.x() * int64(area()->scrollLeftMax()))) / barDelta);
		}
		_connected->setValue(_startFrom + delta);
	}
}

void ScrollBar::mousePressEvent(QMouseEvent *e) {
	if (!width() || !height()) return;

	_dragStart = e->globalPos();
	area()->setMovingByScrollBar(true);
	setMoving(true);
	if (_overbar) {
		_startFrom = _connected->value();
	} else {
		int32 val = _vertical ? e->pos().y() : e->pos().x(), div = _vertical ? height() : width();
		val = (val <= _st->deltat) ? 0 : (val - _st->deltat);
		div = (div <= _st->deltat + _st->deltab) ? 1 : (div - _st->deltat - _st->deltab);
		_startFrom = _vertical ? int32((val * int64(area()->scrollTopMax())) / div) : ((val * int64(area()->scrollLeftMax())) / div);
		_connected->setValue(_startFrom);
		setOverBar(true);
	}
}

void ScrollBar::mouseReleaseEvent(QMouseEvent *e) {
	if (_moving) {
		area()->setMovingByScrollBar(false);
		setMoving(false);
	}
	if (!_over) {
		setMouseTracking(false);
	}
}

void ScrollBar::resizeEvent(QResizeEvent *e) {
	updateBar();
}

void ScrollBar::wheelEvent(QWheelEvent *e) {
	static_cast<ScrollArea*>(parentWidget())->viewportEvent(e);
}

auto ScrollBar::shadowVisibilityChanged() const
-> rpl::producer<ScrollBar::ShadowVisibility> {
	return _shadowVisibilityChanged.events();
}

ScrollArea::ScrollArea(QWidget *parent, const style::ScrollArea &st)
: Parent(parent)
, _st(st)
, _horizontalBar(this, false, &_st)
, _verticalBar(this, true, &_st)
, _topShadow(this, &_st)
, _bottomShadow(this, &_st)
, _scroller(QScroller::scroller(this))
, _touchTimer([=] {
	SendSynteticMouseEvent(
		this,
		QEvent::MouseButtonPress,
		Qt::LeftButton,
		_touchStart);
	_touchRightButton = true;
}) {
	setLayoutDirection(style::LayoutDirection());
	setFocusPolicy(Qt::NoFocus);

	_verticalBar->shadowVisibilityChanged(
	) | rpl::on_next([=](const ScrollBar::ShadowVisibility &data) {
		((data.type == ScrollShadow::Type::Top)
			? _topShadow
			: _bottomShadow)->changeVisibility(data.visible);
	}, lifetime());

	_verticalBar->updateBar(true);

	verticalScrollBar()->setSingleStep(style::ConvertScale(verticalScrollBar()->singleStep()));
	horizontalScrollBar()->setSingleStep(style::ConvertScale(horizontalScrollBar()->singleStep()));

	setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	setFrameStyle(int(QFrame::NoFrame) | QFrame::Plain);
	viewport()->setAutoFillBackground(false);

	_horizontalValue = horizontalScrollBar()->value();
	_verticalValue = verticalScrollBar()->value();

#ifdef Q_OS_WIN
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	SetProp((HWND)winId(), L"MicrosoftTabletPenServiceProperty", (HANDLE)1);
#endif // Q_OS_WIN
}

void ScrollArea::scrolled() {
	if (const auto inner = widget()) {
		SendPendingMoveResizeEvents(inner);
	}

	bool em = false;
	int horizontalValue = horizontalScrollBar()->value();
	int verticalValue = verticalScrollBar()->value();
	if (_horizontalValue != horizontalValue) {
		if (_disabled) {
			horizontalScrollBar()->setValue(_horizontalValue);
		} else {
			_horizontalValue = horizontalValue;
			if (_st.hiding) {
				_horizontalBar->hideTimeout(_st.hiding);
			}
			em = true;
		}
	}
	if (_verticalValue != verticalValue) {
		if (_disabled) {
			verticalScrollBar()->setValue(_verticalValue);
		} else {
			_verticalValue = verticalValue;
			if (_st.hiding) {
				_verticalBar->hideTimeout(_st.hiding);
			}
			em = true;
			_scrollTopUpdated.fire_copy(_verticalValue);
		}
	}
	if (em) {
		_scrolls.fire({});
		if (!_movingByScrollBar) {
			SendSynteticMouseEvent(this, QEvent::MouseMove, Qt::NoButton);
		}
	}
}

void ScrollArea::innerResized() {
	_innerResizes.fire({});
}

int ScrollArea::scrollWidth() const {
	QWidget *w(widget());
	return w ? qMax(w->width(), width()) : width();
}

int ScrollArea::scrollHeight() const {
	QWidget *w(widget());
	return w ? qMax(w->height(), height()) : height();
}

int ScrollArea::scrollLeftMax() const {
	return scrollWidth() - width();
}

int ScrollArea::scrollTopMax() const {
	return scrollHeight() - height();
}

int ScrollArea::scrollLeft() const {
	return _horizontalValue;
}

int ScrollArea::scrollTop() const {
	return _verticalValue;
}

bool ScrollArea::eventHook(QEvent *e) {
	const auto was = (e->type() == QEvent::LayoutRequest)
		? verticalScrollBar()->minimum()
		: 0;
	const auto result = RpWidgetBase<QScrollArea>::eventHook(e);
	if (was) {
		// Because LayoutRequest resets custom-set minimum allowed value.
		verticalScrollBar()->setMinimum(was);
	}
	return result;
}

bool ScrollArea::eventFilter(QObject *obj, QEvent *e) {
	auto result = false;
	switch (e->type()) {
	case QEvent::MouseButtonPress: {
		const auto ev = static_cast<QMouseEvent*>(e);
		if (IsMouseFromTouch(ev->source())
			&& ev->button() == Qt::LeftButton) {
			_touchMaybePressing = true;
			_touchStart = ev->globalPos();
			_touchTimer.callOnce(QApplication::startDragTime());
			result = true;
		}
	} break;
	case QEvent::MouseMove: {
		const auto ev = static_cast<QMouseEvent*>(e);
		if (IsMouseFromTouch(ev->source())
			&& (ev->buttons() & Qt::LeftButton)) {
			if (_touchTimer.isActive()
				&& (_touchStart - ev->globalPos()).manhattanLength()
					>= QApplication::startDragDistance()) {
				_scroller->handleInput(
					QScroller::InputPress,
					mapFromGlobal(_touchStart),
					crl::now());
				_touchMaybePressing = false;
				_touchTimer.cancel();
				result = true;
			} else if (!_touchMaybePressing.current()) {
				_scroller->handleInput(
					QScroller::InputMove,
					mapFromGlobal(ev->globalPos()),
					crl::now());
				result = true;
			}
		}
	} break;
	case QEvent::MouseButtonRelease: {
		const auto ev = static_cast<QMouseEvent*>(e);
		if (IsMouseFromTouch(ev->source())
			&& ev->button() == Qt::LeftButton) {
			if (_touchRightButton) {
				InvokeQueued(this, [=] {
					SendSynteticMouseEvent(
						this,
						QEvent::MouseMove,
						Qt::NoButton,
						_touchStart);
					SendSynteticMouseEvent(
						this,
						QEvent::MouseButtonPress,
						Qt::RightButton,
						_touchStart);
					SendSynteticMouseEvent(
						this,
						QEvent::MouseButtonRelease,
						Qt::RightButton,
						_touchStart);
					if (auto windowHandle = window()->windowHandle()) {
						QContextMenuEvent ev(
							QContextMenuEvent::Mouse,
							windowHandle->mapFromGlobal(_touchStart),
							_touchStart,
							QGuiApplication::keyboardModifiers());
						ev.setTimestamp(crl::now());
						QGuiApplication::sendEvent(windowHandle, &ev);
					}
				});
			} else if (!_touchMaybePressing.current()) {
				_scroller->handleInput(
					QScroller::InputRelease,
					mapFromGlobal(ev->globalPos()),
					crl::now());
				result = true;
			} else {
				SendSynteticMouseEvent(
					this,
					QEvent::MouseButtonPress,
					Qt::LeftButton,
					_touchStart);
			}
			_touchRightButton = false;
			_touchMaybePressing = false;
			_touchTimer.cancel();
		}
	} break;
	}
	return result || QScrollArea::eventFilter(obj, e);
}

bool ScrollArea::viewportEvent(QEvent *e) {
	if (e->type() == QEvent::Wheel) {
		if (_customWheelProcess
			&& _customWheelProcess(static_cast<QWheelEvent*>(e))) {
			return true;
		}
	}
	return QScrollArea::viewportEvent(e);
}

void ScrollArea::disableScroll(bool dis) {
	_disabled = dis;
	if (_disabled && _st.hiding) {
		_horizontalBar->hideTimeout(0);
		_verticalBar->hideTimeout(0);
	}
}

void ScrollArea::scrollContentsBy(int dx, int dy) {
	if (_disabled) {
		return;
	}
	QScrollArea::scrollContentsBy(dx, dy);
}

void ScrollArea::resizeEvent(QResizeEvent *e) {
	QScrollArea::resizeEvent(e);
	_horizontalBar->recountSize();
	_verticalBar->recountSize();
	_topShadow->setGeometry(QRect(0, 0, width(), qAbs(_st.topsh)));
	_bottomShadow->setGeometry(QRect(0, height() - qAbs(_st.bottomsh), width(), qAbs(_st.bottomsh)));
	_geometryChanged.fire({});
}

void ScrollArea::moveEvent(QMoveEvent *e) {
	QScrollArea::moveEvent(e);
	_geometryChanged.fire({});
}

void ScrollArea::keyPressEvent(QKeyEvent *e) {
	if ((e->key() == Qt::Key_Up || e->key() == Qt::Key_Down)
		&& (e->modifiers().testFlag(Qt::AltModifier)
			|| e->modifiers().testFlag(Qt::ControlModifier))) {
		e->ignore();
	} else if(e->key() == Qt::Key_Escape || e->key() == Qt::Key_Back) {
		((QObject*)widget())->event(e);
	} else {
		QScrollArea::keyPressEvent(e);
	}
}

void ScrollArea::enterEventHook(QEnterEvent *e) {
	if (_disabled) return;
	if (_st.hiding) {
		_horizontalBar->hideTimeout(_st.hiding);
		_verticalBar->hideTimeout(_st.hiding);
	}
	return QScrollArea::enterEvent(e);
}

void ScrollArea::leaveEventHook(QEvent *e) {
	if (_st.hiding) {
		_horizontalBar->hideTimeout(0);
		_verticalBar->hideTimeout(0);
	}
	return QScrollArea::leaveEvent(e);
}

void ScrollArea::scrollTo(ScrollToRequest request) {
	scrollToY(request.ymin, request.ymax);
}

void ScrollArea::scrollToWidget(not_null<QWidget*> widget) {
	if (auto local = this->widget()) {
		auto globalPosition = widget->mapToGlobal(QPoint(0, 0));
		auto localPosition = local->mapFromGlobal(globalPosition);
		auto localTop = localPosition.y();
		auto localBottom = localTop + widget->height();
		scrollToY(localTop, localBottom);
	}
}

int ScrollArea::computeScrollToX(int toLeft, int toRight) {
	if (const auto inner = widget()) {
		SendPendingMoveResizeEvents(inner);
	}
	SendPendingMoveResizeEvents(this);
	return ComputeScrollTo(
		toLeft,
		toRight,
		0,
		scrollLeftMax(),
		scrollLeft(),
		width());
}

int ScrollArea::computeScrollToY(int toTop, int toBottom) {
	if (const auto inner = widget()) {
		SendPendingMoveResizeEvents(inner);
	}
	SendPendingMoveResizeEvents(this);
	return ComputeScrollTo(
		toTop,
		toBottom,
		0,
		scrollTopMax(),
		scrollTop(),
		height());
}

void ScrollArea::scrollToX(int toLeft, int toRight) {
	horizontalScrollBar()->setValue(computeScrollToX(toLeft, toRight));
}

void ScrollArea::scrollToY(int toTop, int toBottom) {
	verticalScrollBar()->setValue(computeScrollToY(toTop, toBottom));
}

void ScrollArea::doSetOwnedWidget(object_ptr<QWidget> w) {
	_widget = std::move(w);
	QScrollArea::setWidget(_widget);
	if (_widget) {
		_widget->setAutoFillBackground(false);
	}
}

object_ptr<QWidget> ScrollArea::doTakeWidget() {
	QScrollArea::takeWidget();
	return std::move(_widget);
}

void ScrollArea::rangeChanged(int oldMax, int newMax, bool vertical) {
}

void ScrollArea::updateBars() {
	_horizontalBar->updateBar(true);
	_verticalBar->updateBar(true);
}

bool ScrollArea::focusNextPrevChild(bool next) {
	if (QWidget::focusNextPrevChild(next)) {
//		if (QWidget *fw = focusWidget())
//			ensureWidgetVisible(fw);
		return true;
	}
	return false;
}

void ScrollArea::setMovingByScrollBar(bool movingByScrollBar) {
	_movingByScrollBar = movingByScrollBar;
}

rpl::producer<> ScrollArea::scrolls() const {
	return _scrolls.events();
}

rpl::producer<> ScrollArea::innerResizes() const {
	return _innerResizes.events();
}

rpl::producer<> ScrollArea::geometryChanged() const {
	return _geometryChanged.events();
}

rpl::producer<bool> ScrollArea::touchMaybePressing() const {
	return _touchMaybePressing.value();
}

} // namespace Ui
