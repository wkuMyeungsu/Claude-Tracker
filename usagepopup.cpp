#include "usagepopup.h"
#include "quotapanel.h"
#include <QApplication>
#include "toggleswitch.h"
#include <QEasingCurve>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QDebug>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>


UsagePopup::UsagePopup(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setFixedWidth(280);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_titleBar = new QWidget;
    m_titleBar->setFixedHeight(36);
    m_titleBar->setStyleSheet("background: #2d2d2d;");
    m_titleBar->setCursor(Qt::SizeAllCursor);
    m_titleBar->installEventFilter(this);

    auto *titleRow = new QHBoxLayout(m_titleBar);
    titleRow->setContentsMargins(12, 0, 8, 0);

    m_pinBtn = new QPushButton;
    m_pinBtn->setFixedSize(14, 14);
    m_pinBtn->setCheckable(true);
    m_pinBtn->setChecked(true);   // 초기값: 핀 ON (WindowStaysOnTopHint와 동일)
    m_pinBtn->setCursor(Qt::PointingHandCursor);
    m_pinBtn->setToolTip("항상 위에 표시");
    m_pinBtn->setStyleSheet(R"(
        QPushButton {
            background: #666;
            border: none;
            border-radius: 7px;
        }
        QPushButton:checked {
            background: #eaeaea;
        }
        QPushButton:hover {
            background: #ffffff;
            opacity: 0.8;
        }
    )");

    connect(m_pinBtn, &QPushButton::toggled, this, [this](bool pinned) {
        const QPoint oldPos = pos();  // setWindowFlags() 전에 저장 (재생성 후 OS가 위치 변경)

        Qt::WindowFlags flags = windowFlags();
        if (pinned)
            flags |= Qt::WindowStaysOnTopHint;
        else
            flags &= ~Qt::WindowStaysOnTopHint;
        setWindowFlags(flags);
        show();   // setWindowFlags() 호출 시 창이 숨겨지므로 재호출 필수
        raise();
        activateWindow();

        // oldPos 기준으로 availableGeometry 내에 clamp (작업표시줄 침범 방지)
        QScreen *screen = QApplication::screenAt(oldPos);
        if (!screen) screen = QApplication::primaryScreen();
        const QRect avail = screen->availableGeometry();
        const int x = qBound(avail.left(), oldPos.x(), avail.right()  - width());
        const int y = qBound(avail.top(),  oldPos.y(), avail.bottom() - height());
        move(x, y);
        // setWindowFlags()가 hideEvent를 트리거해 opacity를 1.0으로 리셋함
        // idle 상태였다면 핀 ON 시에만 다시 투명화
        if (pinned && m_autoFade && m_opacityAtIdle)
            QTimer::singleShot(300, this, [this]() {
                if (isVisible() && m_pinBtn->isChecked() && m_autoFade && m_opacityAtIdle)
                    animateOpacityTo(0.6);
            });
    });

    auto *titleLabel = new QLabel("Claude Code Usage");
    titleLabel->setStyleSheet("color: white; font-weight: bold; font-size: 12px;");

    const QString btnBase = R"(
        QPushButton {
            background: transparent;
            color: #aaa;
            border: none;
            font-size: 13px;
        }
    )";

    auto *minimizeBtn = new QPushButton("−");
    minimizeBtn->setFixedSize(22, 22);
    minimizeBtn->setCursor(Qt::PointingHandCursor);
    minimizeBtn->setStyleSheet(btnBase + R"(
        QPushButton:hover { color: white; background: #555; border-radius: 3px; }
    )");
    connect(minimizeBtn, &QPushButton::clicked, this, [this]() {
        m_rememberedPos    = pos();   // 현재 위치 저장
        m_hasRememberedPos = true;
        hide();
    });

    titleRow->addWidget(m_pinBtn);
    titleRow->addSpacing(6);
    titleRow->addWidget(titleLabel);
    titleRow->addStretch();
    titleRow->addWidget(minimizeBtn);

    m_panel5h = new QuotaPanel("5h 사용량");
    m_panel7d = new QuotaPanel("7d 사용량");

    m_sep1 = new QFrame; m_sep1->setFrameShape(QFrame::HLine); m_sep1->setStyleSheet("color: #444;");
    m_sep2 = new QFrame; m_sep2->setFrameShape(QFrame::HLine); m_sep2->setStyleSheet("color: #ddd;");
    m_sep3 = new QFrame; m_sep3->setFrameShape(QFrame::HLine); m_sep3->setStyleSheet("color: #ddd;");

    // QSettings에서 autoFade 상태 복원
    {
        QSettings s("ClaudeTray", "ClaudeTray");
        m_autoFade = s.value("autoFade", true).toBool();
    }

    m_footer = new QWidget;
    m_footer->setStyleSheet("background: #f8f9fa;");
    auto *footerLayout = new QVBoxLayout(m_footer);
    footerLayout->setContentsMargins(14, 6, 8, 8);
    footerLayout->setSpacing(0);

    // 상태줄 + 톱니바퀴 버튼 행
    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    m_statusLine = new QLabel("🔄 갱신 중...");
    m_statusLine->setStyleSheet("color: #666; font-size: 10px;");

    m_gearBtn = new QPushButton("⚙");
    m_gearBtn->setFixedSize(18, 18);
    m_gearBtn->setCheckable(true);
    m_gearBtn->setCursor(Qt::PointingHandCursor);
    m_gearBtn->setToolTip("설정");
    m_gearBtn->setStyleSheet(R"(
        QPushButton {
            background: transparent;
            color: #aaa;
            border: none;
            font-size: 11px;
            padding: 0;
        }
        QPushButton:hover { color: #555; }
        QPushButton:checked { color: #333; }
    )");

    statusRow->addWidget(m_statusLine);
    statusRow->addStretch();
    statusRow->addWidget(m_gearBtn);
    footerLayout->addLayout(statusRow);

    // 설정 패널 (기본 숨김)
    m_settingsPanel = new QWidget;
    m_settingsPanel->setStyleSheet("background: #f0f0f0; border-top: 1px solid #ddd;");
    auto *settingsLayout = new QVBoxLayout(m_settingsPanel);
    settingsLayout->setContentsMargins(6, 6, 6, 6);

    auto *fadeRow   = new QHBoxLayout;
    auto *fadeLabel = new QLabel("핀 고정 시 자동 투명화");
    fadeLabel->setStyleSheet("font-size: 10px; color: #444;");
    m_autoFadeCheck = new ToggleSwitch;
    m_autoFadeCheck->setChecked(m_autoFade);
    fadeRow->addWidget(fadeLabel);
    fadeRow->addStretch();
    fadeRow->addWidget(m_autoFadeCheck);
    settingsLayout->addLayout(fadeRow);
    m_settingsPanel->setVisible(false);

    connect(m_gearBtn, &QPushButton::toggled, this, [this](bool open) {
        if (open) {
            m_settingsPanel->setMaximumHeight(QWIDGETSIZE_MAX);
            m_settingsPanel->show();
        } else {
            m_settingsPanel->setMaximumHeight(0);
            m_settingsPanel->hide();
        }
        m_collapsingBody->layout()->invalidate();
        m_collapsingBody->adjustSize();
        adjustSize();
    });

    connect(m_autoFadeCheck, &ToggleSwitch::toggled, this, [this](bool checked) {
        m_autoFade = checked;
        QSettings s("ClaudeTray", "ClaudeTray");
        s.setValue("autoFade", checked);
        if (!checked && m_opacityAtIdle) {
            m_opacityAtIdle = false;
            animateOpacityTo(1.0);
        } else if (checked && m_pinBtn->isChecked() && m_idleMode) {
            m_opacityAtIdle = true;
            animateOpacityTo(0.6);
        }
    });

    m_justRefreshedTimer = new QTimer(this);
    m_justRefreshedTimer->setSingleShot(true);
    connect(m_justRefreshedTimer, &QTimer::timeout, this, &UsagePopup::updateStatusLine);

    m_fadeTimer = new QTimer(this);
    m_fadeTimer->setSingleShot(true);
    m_fadeTimer->setInterval(10'000);
    connect(m_fadeTimer, &QTimer::timeout, this, [this]() {
        if (!m_idleMode) return;
        if (!m_pinBtn->isChecked()) return;
        if (!m_autoFade) return;
        m_opacityAtIdle = true;
        if (isVisible())
            animateOpacityTo(0.6);
    });

    m_collapsingBody = new QWidget;
    auto *bodyLayout = new QVBoxLayout(m_collapsingBody);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(m_sep1);
    bodyLayout->addWidget(m_panel5h);
    bodyLayout->addWidget(m_sep2);
    bodyLayout->addWidget(m_panel7d);
    bodyLayout->addWidget(m_sep3);
    bodyLayout->addWidget(m_footer);
    bodyLayout->addWidget(m_settingsPanel);

    root->addWidget(m_titleBar);
    root->addWidget(m_collapsingBody);

    setStyleSheet("QWidget { background: white; }");

    m_opacityAnim = new QPropertyAnimation(this, "windowOpacity", this);
    m_opacityAnim->setDuration(300);
    m_opacityAnim->setEasingCurve(QEasingCurve::InOutQuad);

}

void UsagePopup::setData(const UsageData &data)
{
    if (m_isDragging) {
        m_pendingData     = data;
        m_hasPendingData  = true;
        return;
    }
    applyDataInternal(data);
}

void UsagePopup::setCountdowns(const QString &c5h, const QString &c7d)
{
    if (m_isDragging) {
        m_pendingC5h  = c5h;
        m_pendingC7d  = c7d;
        m_hasPendingCD = true;
        return;
    }
    applyCountdownsInternal(c5h, c7d);
}

void UsagePopup::setRefreshState(RefreshState state, QDateTime lastFetch, QDateTime nextFetch)
{
    if (m_isDragging) {
        m_hasPendingRefreshState = true;
        m_pendingRefreshState    = state;
        m_pendingLastFetch       = lastFetch;
        m_pendingNextFetch       = nextFetch;
        return;
    }

    const bool stateChanged = (m_refreshState != state);
    m_refreshState = state;
    if (lastFetch.isValid()) m_lastFetch = lastFetch;
    if (nextFetch.isValid()) m_nextFetch = nextFetch;

    if (stateChanged && state == RefreshState::Refreshed)
        m_justRefreshedTimer->start(60 * 1000);
    else if (state != RefreshState::Refreshed)
        m_justRefreshedTimer->stop();

    updateStatusLine();
}

void UsagePopup::refreshNextFetch(QDateTime nextFetch)
{
    m_nextFetch = nextFetch;
    updateStatusLine();
}

static QString fmtAgo(const QDateTime &dt)
{
    if (!dt.isValid()) return "-";
    const qint64 secs = dt.secsTo(QDateTime::currentDateTime());
    if (secs < 60)    return "방금";
    if (secs < 3600)  return QString("%1m 전").arg(secs / 60);
    if (secs < 86400) return QString("%1h 전").arg(secs / 3600);
    return QString("%1d 전").arg(secs / 86400);
}

static QString fmtIn(const QDateTime &dt)
{
    if (!dt.isValid()) return "-";
    const qint64 secs = QDateTime::currentDateTime().secsTo(dt);
    if (secs < 30)    return "곧";
    if (secs < 3600)  return QString("%1m 후").arg(secs / 60);
    if (secs < 86400) return QString("%1h 후").arg(secs / 3600);
    return QString("%1d 후").arg(secs / 86400);
}


void UsagePopup::updateStatusLine()
{
    QString text;
    switch (m_refreshState) {
    case RefreshState::Fetching:
        text = "🔄 갱신 중...";
        break;
    case RefreshState::Refreshed: {
        const qint64 secs = m_lastFetch.isValid()
            ? m_lastFetch.secsTo(QDateTime::currentDateTime()) : 999;
        if (secs < 60) {
            text = "🟢 방금 갱신";
        } else {
            const QString t = m_nextFetch.isValid()
                ? m_nextFetch.toString("HH:mm")
                : m_lastFetch.toString("HH:mm");
            text = QString("🟢 %1 갱신 예정").arg(t);
        }
        break;
    }
    case RefreshState::LocalFallback:
        text = "🟡 로컬 추적중";
        break;
    case RefreshState::NetworkError:
        text = "🔴 연결 오류";
        break;
    }
    m_statusLine->setText(text);
}

void UsagePopup::animateOpacityTo(double target)
{
    m_opacityAnim->stop();
    m_opacityAnim->setStartValue(windowOpacity());
    m_opacityAnim->setEndValue(target);
    m_opacityAnim->start();
}

void UsagePopup::applyDataInternal(const UsageData &data)
{
    m_panel5h->setData(data.fiveHour);
    m_panel7d->setData(data.sevenDay);
}

void UsagePopup::applyCountdownsInternal(const QString &c5h, const QString &c7d)
{
    m_panel5h->setCountdown(c5h);
    m_panel7d->setCountdown(c7d);
}

void UsagePopup::applyPending()
{
    if (m_hasPendingData) {
        applyDataInternal(m_pendingData);
        m_hasPendingData = false;
    }
    if (m_hasPendingCD) {
        applyCountdownsInternal(m_pendingC5h, m_pendingC7d);
        m_hasPendingCD = false;
    }
    if (m_hasPendingRefreshState) {
        const bool stateChanged = (m_refreshState != m_pendingRefreshState);
        m_refreshState = m_pendingRefreshState;
        if (m_pendingLastFetch.isValid()) m_lastFetch = m_pendingLastFetch;
        if (m_pendingNextFetch.isValid()) m_nextFetch = m_pendingNextFetch;
        if (stateChanged && m_refreshState == RefreshState::Refreshed)
            m_justRefreshedTimer->start(60 * 1000);
        else if (m_refreshState != RefreshState::Refreshed)
            m_justRefreshedTimer->stop();
        updateStatusLine();
        m_hasPendingRefreshState = false;
    }
}

bool UsagePopup::eventFilter(QObject *obj, QEvent *event)
{
    if (obj != m_titleBar)
        return QWidget::eventFilter(obj, event);

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            m_dragPos                  = me->globalPosition().toPoint() - frameGeometry().topLeft();
            m_isDragging               = true;
            m_wasOpacityIdleBeforeDrag = m_opacityAtIdle;
            animateOpacityTo(1.0);  // 투명도만 변경, LED 건드리지 않음
            return true;
        }
    } else if (event->type() == QEvent::MouseMove) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->buttons() & Qt::LeftButton) {
            move(me->globalPosition().toPoint() - m_dragPos);
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            m_isDragging = false;
            applyPending();
            if (m_wasOpacityIdleBeforeDrag && m_pinBtn->isChecked() && m_autoFade) animateOpacityTo(0.6);
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

void UsagePopup::setActive()
{
    m_fadeTimer->stop();  // 대기 중인 투명화 타이머 취소
    m_idleMode      = false;
    m_opacityAtIdle = false;

    m_panel5h->setActive(true);
    m_panel7d->setActive(true);

    if (windowOpacity() >= 1.0 && m_opacityAnim->endValue().toDouble() >= 1.0)
        return;
    animateOpacityTo(1.0);
}

void UsagePopup::setIdle()
{
    if (m_idleMode) return;
    m_idleMode = true;

    m_panel5h->setActive(false);
    m_panel7d->setActive(false);

    m_fadeTimer->start();  // 10s 후 투명화 (이미 대기 중이면 재시작)
}

void UsagePopup::hideEvent(QHideEvent *event)
{
    m_opacityAnim->stop();
    setWindowOpacity(1.0);  // 다음 show() 시 항상 불투명에서 시작
    QWidget::hideEvent(event);
}

void UsagePopup::toggleCompact()
{
    // TODO: 컴팩트 모드 미구현 — 직접 구현 예정
    qDebug() << "[UsagePopup] toggleCompact() called — not yet implemented";
}

void UsagePopup::showNearTray(const QPoint &trayPos)
{
    adjustSize();
    const int w = width();
    const int h = height();

    if (m_hasRememberedPos) {
        // 최소화(−)로 닫혔을 때 → 마지막 위치로 복원
        QScreen *screen = QApplication::screenAt(m_rememberedPos);
        if (!screen)
            screen = QApplication::primaryScreen();
        const QRect avail = screen->availableGeometry();
        const int x = qBound(avail.left(), m_rememberedPos.x(), avail.right()  - w);
        const int y = qBound(avail.top(),  m_rememberedPos.y(), avail.bottom() - h);
        move(x, y);
    } else {
        // 닫기(x)로 닫혔거나 첫 오픈 → 트레이 아이콘 위쪽
        QScreen *screen = QApplication::screenAt(trayPos);
        if (!screen)
            screen = QApplication::primaryScreen();
        const QRect avail = screen->availableGeometry();
        const int x = qBound(avail.left(), trayPos.x() - w / 2, avail.right()  - w);
        const int y = qBound(avail.top(),  trayPos.y() - h - 8, avail.bottom() - h);
        move(x, y);
    }

    setWindowOpacity(1.0);  // 항상 불투명하게 시작
    show();
    raise();
    activateWindow();

    // 팝업이 닫혀 있는 사이에 LED가 이미 회색이 된 경우 → 핀 고정 시만 0.5초 후 투명 적용
    if (m_opacityAtIdle && m_pinBtn->isChecked() && m_autoFade)
        QTimer::singleShot(500, this, [this]() { if (isVisible() && m_pinBtn->isChecked() && m_autoFade) animateOpacityTo(0.6); });
}

void UsagePopup::hideAndSavePos()
{
    m_rememberedPos    = pos();
    m_hasRememberedPos = true;
    hide();
}
