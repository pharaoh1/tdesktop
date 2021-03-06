/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#include "media/player/media_player_panel.h"

#include "media/player/media_player_cover.h"
#include "media/player/media_player_instance.h"
#include "info/media/info_media_list_widget.h"
#include "history/history_media.h"
#include "data/data_document.h"
#include "ui/widgets/shadow.h"
#include "ui/widgets/scroll_area.h"
#include "mainwindow.h"
#include "styles/style_overview.h"
#include "styles/style_widgets.h"
#include "styles/style_media_player.h"
#include "styles/style_info.h"

namespace Media {
namespace Player {
namespace {

using ListWidget = Info::Media::ListWidget;

constexpr auto kPlaylistIdsLimit = 32;
constexpr auto kDelayedHideTimeout = TimeMs(3000);

} // namespace

Panel::Panel(
	QWidget *parent,
	not_null<Window::Controller*> window,
	Layout layout)
: RpWidget(parent)
, AbstractController(window)
, _layout(layout)
, _showTimer([this] { startShow(); })
, _hideTimer([this] { startHideChecked(); })
, _scroll(this, st::mediaPlayerScroll) {
	hide();
	updateSize();
}

bool Panel::overlaps(const QRect &globalRect) {
	if (isHidden() || _a_appearance.animating()) return false;

	auto marginLeft = rtl() ? contentRight() : contentLeft();
	auto marginRight = rtl() ? contentLeft() : contentRight();
	return rect().marginsRemoved(QMargins(marginLeft, contentTop(), marginRight, contentBottom())).contains(QRect(mapFromGlobal(globalRect.topLeft()), globalRect.size()));
}

void Panel::windowActiveChanged() {
	if (!App::wnd()->windowHandle()->isActive() && !isHidden()) {
		leaveEvent(nullptr);
	}
}

void Panel::resizeEvent(QResizeEvent *e) {
	updateControlsGeometry();
}

void Panel::listHeightUpdated(int newHeight) {
	if (newHeight > emptyInnerHeight() || _cover) {
		updateSize();
	} else {
		_hideTimer.callOnce(0);
	}
}

bool Panel::contentTooSmall() const {
	const auto innerHeight = _scroll->widget()
		? _scroll->widget()->height()
		: emptyInnerHeight();
	return (innerHeight <= emptyInnerHeight() && !_cover);
}

int Panel::emptyInnerHeight() const {
	return st::infoMediaMargin.top()
		+ st::overviewFileLayout.songPadding.top()
		+ st::overviewFileLayout.songThumbSize
		+ st::overviewFileLayout.songPadding.bottom()
		+ st::infoMediaMargin.bottom();
}

bool Panel::preventAutoHide() const {
	if (const auto list = static_cast<ListWidget*>(_scroll->widget())) {
		return list->preventAutoHide();
	}
	return false;
}

void Panel::updateControlsGeometry() {
	auto scrollTop = contentTop();
	auto width = contentWidth();
	if (_cover) {
		_cover->resizeToWidth(width);
		_cover->moveToRight(contentRight(), scrollTop);
		scrollTop += _cover->height();
		if (_scrollShadow) {
			_scrollShadow->resize(width, st::mediaPlayerScrollShadow.extend.bottom());
			_scrollShadow->moveToRight(contentRight(), scrollTop);
		}
	}
	auto scrollHeight = qMax(height() - scrollTop - contentBottom() - scrollMarginBottom(), 0);
	if (scrollHeight > 0) {
		_scroll->setGeometryToRight(contentRight(), scrollTop, width, scrollHeight);
	}
	if (auto widget = static_cast<TWidget*>(_scroll->widget())) {
		widget->resizeToWidth(width);
	}
}

int Panel::bestPositionFor(int left) const {
	left -= contentLeft();
	left -= st::mediaPlayerFileLayout.songPadding.left();
	left -= st::mediaPlayerFileLayout.songThumbSize / 2;
	return left;
}

void Panel::scrollPlaylistToCurrentTrack() {
	if (const auto list = static_cast<ListWidget*>(_scroll->widget())) {
		const auto rect = list->getCurrentSongGeometry();
		_scroll->scrollToY(rect.y() - st::infoMediaMargin.top());
	}
}

void Panel::updateSize() {
	auto width = contentLeft() + st::mediaPlayerPanelWidth + contentRight();
	auto height = contentTop();
	if (_cover) {
		height += _cover->height();
	}
	auto listHeight = 0;
	if (auto widget = _scroll->widget()) {
		listHeight = widget->height();
	}
	auto scrollVisible = (listHeight > 0);
	auto scrollHeight = scrollVisible ? (qMin(listHeight, st::mediaPlayerListHeightMax) + st::mediaPlayerListMarginBottom) : 0;
	height += scrollHeight + contentBottom();
	resize(width, height);
	_scroll->setVisible(scrollVisible);
	if (_scrollShadow) {
		_scrollShadow->setVisible(scrollVisible);
	}
}

void Panel::paintEvent(QPaintEvent *e) {
	Painter p(this);

	if (!_cache.isNull()) {
		bool animating = _a_appearance.animating(getms());
		if (animating) {
			p.setOpacity(_a_appearance.current(_hiding ? 0. : 1.));
		} else if (_hiding || isHidden()) {
			hideFinished();
			return;
		}
		p.drawPixmap(0, 0, _cache);
		if (!animating) {
			showChildren();
			_cache = QPixmap();
		}
		return;
	}

	// draw shadow
	auto shadowedRect = myrtlrect(contentLeft(), contentTop(), contentWidth(), contentHeight());
	auto shadowedSides = (rtl() ? RectPart::Right : RectPart::Left) | RectPart::Bottom;
	if (_layout != Layout::Full) {
		shadowedSides |= (rtl() ? RectPart::Left : RectPart::Right) | RectPart::Top;
	}
	Ui::Shadow::paint(p, shadowedRect, width(), st::defaultRoundShadow, shadowedSides);
	auto parts = RectPart::Full;
	App::roundRect(p, shadowedRect, st::menuBg, MenuCorners, nullptr, parts);
}

void Panel::enterEventHook(QEvent *e) {
	if (_ignoringEnterEvents || contentTooSmall()) return;

	_hideTimer.cancel();
	if (_a_appearance.animating(getms())) {
		startShow();
	} else {
		_showTimer.callOnce(0);
	}
	return TWidget::enterEventHook(e);
}

void Panel::leaveEventHook(QEvent *e) {
	if (preventAutoHide()) {
		return;
	}
	_showTimer.cancel();
	if (_a_appearance.animating(getms())) {
		startHide();
	} else {
		_hideTimer.callOnce(300);
	}
	return TWidget::leaveEventHook(e);
}

void Panel::showFromOther() {
	_hideTimer.cancel();
	if (_a_appearance.animating(getms())) {
		startShow();
	} else {
		_showTimer.callOnce(300);
	}
}

void Panel::hideFromOther() {
	_showTimer.cancel();
	if (_a_appearance.animating(getms())) {
		startHide();
	} else {
		_hideTimer.callOnce(0);
	}
}

void Panel::ensureCreated() {
	if (_scroll->widget()) return;

	if (_layout == Layout::Full) {
		_cover.create(this);
		setPinCallback(std::move(_pinCallback));
		setCloseCallback(std::move(_closeCallback));

		_scrollShadow.create(this, st::mediaPlayerScrollShadow, RectPart::Bottom);
	}
	_refreshListLifetime = instance()->playlistChanges(
		AudioMsgId::Type::Song
	) | rpl::start_with_next([this] {
		refreshList();
	});
	refreshList();

	if (cPlatform() == dbipMac || cPlatform() == dbipMacOld) {
		if (const auto window = App::wnd()) {
			connect(
				window->windowHandle(),
				&QWindow::activeChanged,
				this,
				&Panel::windowActiveChanged);
		}
	}

	_ignoringEnterEvents = false;
}

void Panel::refreshList() {
	const auto current = instance()->current(AudioMsgId::Type::Song);
	const auto contextId = current.contextId();
	const auto peer = [&]() -> PeerData* {
		const auto item = contextId ? App::histItemById(contextId) : nullptr;
		const auto media = item ? item->getMedia() : nullptr;
		const auto document = media ? media->getDocument() : nullptr;
		if (!document || !document->isSharedMediaMusic()) {
			return nullptr;
		}
		const auto result = item->history()->peer;
		if (const auto migrated = result->migrateTo()) {
			return migrated;
		}
		return result;
	}();
	const auto migrated = peer ? peer->migrateFrom() : nullptr;
	if (_listPeer != peer || _listMigratedPeer != migrated) {
		_scroll->takeWidget<QWidget>().destroy();
		_listPeer = _listMigratedPeer = nullptr;
	}
	if (peer && !_listPeer) {
		_listPeer = peer;
		_listMigratedPeer = migrated;
		auto list = object_ptr<ListWidget>(this, infoController());

		const auto weak = _scroll->setOwnedWidget(std::move(list));

		updateSize();
		updateControlsGeometry();

		weak->checkForHide(
		) | rpl::start_with_next([this] {
			if (!rect().contains(mapFromGlobal(QCursor::pos()))) {
				_hideTimer.callOnce(kDelayedHideTimeout);
			}
		}, weak->lifetime());

		weak->heightValue(
		) | rpl::start_with_next([this](int newHeight) {
			listHeightUpdated(newHeight);
		}, weak->lifetime());

		weak->scrollToRequests(
		) | rpl::start_with_next([this](int newScrollTop) {
			_scroll->scrollToY(newScrollTop);
		}, weak->lifetime());

		using namespace rpl::mappers;
		rpl::combine(
			_scroll->scrollTopValue(),
			_scroll->heightValue(),
			tuple(_1, _1 + _2)
		) | rpl::start_with_next([=](int top, int bottom) {
			weak->setVisibleTopBottom(top, bottom);
		}, weak->lifetime());

		auto memento = Info::Media::Memento(
			peerId(),
			migratedPeerId(),
			section().mediaType());
		memento.setAroundId(contextId);
		memento.setIdsLimit(kPlaylistIdsLimit);
		memento.setScrollTopItem(contextId);
		memento.setScrollTopShift(-st::infoMediaMargin.top());
		weak->restoreState(&memento);
	}
}

void Panel::performDestroy() {
	if (!_scroll->widget()) return;

	_cover.destroy();
	_scroll->takeWidget<QWidget>().destroy();
	_listPeer = _listMigratedPeer = nullptr;
	_refreshListLifetime.destroy();

	if (cPlatform() == dbipMac || cPlatform() == dbipMacOld) {
		if (const auto window = App::wnd()) {
			disconnect(
				window->windowHandle(),
				&QWindow::activeChanged,
				this,
				&Panel::windowActiveChanged);
		}
	}
}

void Panel::setPinCallback(ButtonCallback &&callback) {
	_pinCallback = std::move(callback);
	if (_cover) {
		_cover->setPinCallback(ButtonCallback(_pinCallback));
	}
}

void Panel::setCloseCallback(ButtonCallback &&callback) {
	_closeCallback = std::move(callback);
	if (_cover) {
		_cover->setCloseCallback(ButtonCallback(_closeCallback));
	}
}

not_null<PeerData*> Panel::peer() const {
	return _listPeer;
}

PeerData *Panel::migrated() const {
	return _listMigratedPeer;
}

Info::Section Panel::section() const {
	return Info::Section(Info::Section::MediaType::MusicFile);
}

void Panel::startShow() {
	ensureCreated();
	if (contentTooSmall()) {
		return;
	}

	if (isHidden()) {
		scrollPlaylistToCurrentTrack();
		show();
	} else if (!_hiding) {
		return;
	}
	_hiding = false;
	startAnimation();
}

void Panel::hideIgnoringEnterEvents() {
	_ignoringEnterEvents = true;
	if (isHidden()) {
		hideFinished();
	} else {
		startHide();
	}
}

void Panel::startHideChecked() {
	if (!contentTooSmall() && preventAutoHide()) {
		return;
	}
	if (isHidden()) {
		hideFinished();
	} else {
		startHide();
	}
}

void Panel::startHide() {
	if (_hiding || isHidden()) return;

	_hiding = true;
	startAnimation();
}

void Panel::startAnimation() {
	auto from = _hiding ? 1. : 0.;
	auto to = _hiding ? 0. : 1.;
	if (_cache.isNull()) {
		showChildren();
		_cache = myGrab(this);
	}
	hideChildren();
	_a_appearance.start([this] { appearanceCallback(); }, from, to, st::defaultInnerDropdown.duration);
}

void Panel::appearanceCallback() {
	if (!_a_appearance.animating() && _hiding) {
		_hiding = false;
		hideFinished();
	} else {
		update();
	}
}

void Panel::hideFinished() {
	hide();
	_cache = QPixmap();
	performDestroy();
}

int Panel::contentLeft() const {
	return st::mediaPlayerPanelMarginLeft;
}

int Panel::contentTop() const {
	return (_layout == Layout::Full) ? 0 : st::mediaPlayerPanelMarginLeft;
}

int Panel::contentRight() const {
	return (_layout == Layout::Full) ? 0 : st::mediaPlayerPanelMarginLeft;
}

int Panel::contentBottom() const {
	return st::mediaPlayerPanelMarginBottom;
}

int Panel::scrollMarginBottom() const {
	return 0;// st::mediaPlayerPanelMarginBottom;
}

} // namespace Player
} // namespace Media
