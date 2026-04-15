#include "usagepopup.h"
#include "quotapanel.h"
#include "statusled.h"
#include <QApplication>
#include <QEasingCurve>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
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

    m_led = new StatusLed;

    auto *titleLabel = new QLabel("Claude Code Usage");
    titleLabel->setStyleSheet("color: white; font-weight: bold; font-size: 12px;");

    auto *closeBtn = new QPushButton("x");
    closeBtn->setFixedSize(22, 22);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(R"(
        QPushButton {
            background: transparent;
            color: #aaa;
            border: none;
            font-size: 13px;
        }
        QPushButton:hover {
            color: white;
            background: #c0392b;
            border-radius: 3px;
        }
    )");
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::hide);

    titleRow->addWidget(m_led);
    titleRow->addSpacing(6);
    titleRow->addWidget(titleLabel);
    titleRow->addStretch();
    titleRow->addWidget(closeBtn);

    auto *sep1 = new QFrame;
    sep1->setFrameShape(QFrame::HLine);
    sep1->setStyleSheet("color: #444;");

    m_panel5h = new QuotaPanel("5시간 사용량");

    auto *sep2 = new QFrame;
    sep2->setFrameShape(QFrame::HLine);
    sep2->setStyleSheet("color: #ddd;");

    m_panel7d = new QuotaPanel("7일 사용량");

    auto *sep3 = new QFrame;
    sep3->setFrameShape(QFrame::HLine);
    sep3->setStyleSheet("color: #ddd;");

    auto *footer = new QWidget;
    footer->setStyleSheet("background: #f8f9fa;");

    auto *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(14, 8, 14, 10);
    footerLayout->setSpacing(6);

    m_statusLabel = new QLabel("불러오는 중...");
    m_statusLabel->setStyleSheet("color: #666; font-size: 10px;");

    m_timingLabel = new QLabel("--");
    m_timingLabel->setStyleSheet("color: #666; font-size: 10px;");

    auto *btnRow = new QHBoxLayout;
    auto *quitBtn = new QPushButton("종료");

    const QString btnStyle = R"(
        QPushButton {
            padding: 4px 14px;
            border-radius: 4px;
            font-size: 11px;
        }
    )";
    quitBtn->setStyleSheet(btnStyle +
        "QPushButton { background: white; color: #333; border: 1px solid #ccc; }"
        "QPushButton:hover { background: #f0f0f0; }");

    connect(quitBtn, &QPushButton::clicked, this, &UsagePopup::quitRequested);

    btnRow->addStretch();
    btnRow->addWidget(quitBtn);

    footerLayout->addWidget(m_statusLabel);
    footerLayout->addWidget(m_timingLabel);
    footerLayout->addLayout(btnRow);

    root->addWidget(m_titleBar);
    root->addWidget(sep1);
    root->addWidget(m_panel5h);
    root->addWidget(sep2);
    root->addWidget(m_panel7d);
    root->addWidget(sep3);
    root->addWidget(footer);

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

void UsagePopup::setStatus(const QString &text)
{
    if (m_isDragging) {
        m_pendingStatus     = text;
        m_hasPendingStatus  = true;
        return;
    }
    m_statusLabel->setText(text);
}

void UsagePopup::setTimingText(const QString &text)
{
    if (m_isDragging) {
        m_pendingTiming     = text;
        m_hasPendingTiming  = true;
        return;
    }
    m_timingLabel->setText(text);
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
    if (m_hasPendingStatus) {
        m_statusLabel->setText(m_pendingStatus);
        m_hasPendingStatus = false;
    }
    if (m_hasPendingTiming) {
        m_timingLabel->setText(m_pendingTiming);
        m_hasPendingTiming = false;
    }
}

bool UsagePopup::eventFilter(QObject *obj, QEvent *event)
{
    if (obj != m_titleBar)
        return QWidget::eventFilter(obj, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            m_dragPos           = me->globalPosition().toPoint() - frameGeometry().topLeft();
            m_isDragging        = true;
            m_wasIdleBeforeDrag = m_idleMode;
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
            if (m_wasIdleBeforeDrag) animateOpacityTo(0.6);  // 투명도만 복원
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

void UsagePopup::setActive()
{
    m_idleMode = false;
    m_led->setActive();          // 팝업 비가시 상태에서도 LED 상태 유지
    m_opacityAnim->stop();
    m_opacityAnim->setStartValue(windowOpacity());
    m_opacityAnim->setEndValue(1.0);
    m_opacityAnim->start();
}

void UsagePopup::setIdle()
{
    m_idleMode = true;
    m_led->setIdle();            // 팝업 비가시 상태에서도 30초 페이드 시작
    if (!isVisible()) return;    // 투명도는 보일 때만
    m_opacityAnim->stop();
    m_opacityAnim->setStartValue(windowOpacity());
    m_opacityAnim->setEndValue(0.6);
    m_opacityAnim->start();
}

void UsagePopup::hideEvent(QHideEvent *event)
{
    m_opacityAnim->stop();
    setWindowOpacity(1.0);  // 다음 show() 시 항상 불투명에서 시작
    QWidget::hideEvent(event);
}

void UsagePopup::showNearTray(const QPoint &trayPos)
{
    QScreen *screen = QApplication::screenAt(trayPos);
    if (!screen)
        screen = QApplication::primaryScreen();

    const QRect avail = screen->availableGeometry();

    adjustSize();
    const int w = width();
    const int h = height();

    int x = trayPos.x() - w / 2;
    int y = trayPos.y() - h - 8;

    x = qBound(avail.left(), x, avail.right() - w);
    y = qBound(avail.top(), y, avail.bottom() - h);

    setWindowOpacity(1.0);  // 항상 불투명하게 시작
    move(x, y);
    show();
    raise();
    activateWindow();
}
