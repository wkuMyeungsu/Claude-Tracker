#include "usagepopup.h"
#include "quotapanel.h"
#include <QApplication>
#include <QDialog>
#include <QEasingCurve>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

// ... QuitConfirmDialog 클래스 내용은 동일하므로 중략 (실제로는 포함됨) ...

// ── 종료 확인 다이얼로그 (UsagePopup과 동일한 UI 스타일) ─────────────────────
class QuitConfirmDialog : public QDialog
{
public:
    explicit QuitConfirmDialog(QWidget *parent = nullptr)
        : QDialog(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    {
        setFixedWidth(260);
        setAttribute(Qt::WA_TranslucentBackground);
        setStyleSheet("QDialog { background: white; border: 1px solid #444; border-radius: 6px; }"
                      "QWidget#body { background: white; border-bottom-left-radius: 6px; border-bottom-right-radius: 6px; }");

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        // 타이틀바
        auto *titleBar = new QWidget;
        titleBar->setFixedHeight(36);
        titleBar->setStyleSheet(
            "background: #2d2d2d;"
            "border-top-left-radius: 6px;"
            "border-top-right-radius: 6px;");
        titleBar->installEventFilter(this);

        auto *titleRow = new QHBoxLayout(titleBar);
        titleRow->setContentsMargins(12, 0, 8, 0);

        auto *titleLabel = new QLabel("ClaudeTray 종료");
        titleLabel->setStyleSheet("color: white; font-weight: bold; font-size: 12px; background: transparent;");
        titleRow->addWidget(titleLabel);
        titleRow->addStretch();

        // 본문
        auto *body = new QWidget;
        body->setObjectName("body");

        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(16, 16, 16, 16);
        bodyLayout->setSpacing(16);

        auto *msgLabel = new QLabel("ClaudeTray를 종료하시겠습니까?");
        msgLabel->setStyleSheet("color: #333; font-size: 12px; background: transparent;");
        msgLabel->setWordWrap(true);

        auto *btnRow = new QHBoxLayout;
        btnRow->setSpacing(8);

        const QString btnBase =
            "QPushButton { padding: 5px 16px; border-radius: 4px; font-size: 11px; }";

        auto *cancelBtn = new QPushButton("취소");
        cancelBtn->setStyleSheet(btnBase +
            "QPushButton { background: white; color: #333; border: 1px solid #ccc; }"
            "QPushButton:hover { background: #f0f0f0; }");
        cancelBtn->setCursor(Qt::PointingHandCursor);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

        auto *confirmBtn = new QPushButton("종료");
        confirmBtn->setStyleSheet(btnBase +
            "QPushButton { background: #c0392b; color: white; border: none; }"
            "QPushButton:hover { background: #a93226; }");
        confirmBtn->setCursor(Qt::PointingHandCursor);
        connect(confirmBtn, &QPushButton::clicked, this, &QDialog::accept);

        btnRow->addStretch();
        btnRow->addWidget(cancelBtn);
        btnRow->addWidget(confirmBtn);

        bodyLayout->addWidget(msgLabel);
        bodyLayout->addLayout(btnRow);

        root->addWidget(titleBar);
        root->addWidget(body);

        m_titleBar = titleBar;
    }

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (obj == m_titleBar) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto *me = static_cast<QMouseEvent *>(event);
                if (me->button() == Qt::LeftButton) {
                    m_dragPos = me->globalPosition().toPoint() - frameGeometry().topLeft();
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove) {
                auto *me = static_cast<QMouseEvent *>(event);
                if (me->buttons() & Qt::LeftButton) {
                    move(me->globalPosition().toPoint() - m_dragPos);
                    return true;
                }
            }
        }
        return QDialog::eventFilter(obj, event);
    }

private:
    QWidget *m_titleBar = nullptr;
    QPoint   m_dragPos;
};

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
            background: #f39c12;
        }
        QPushButton:hover {
            background: #f39c12;
            opacity: 0.8;
        }
    )");

    connect(m_pinBtn, &QPushButton::toggled, this, [this](bool pinned) {
        Qt::WindowFlags flags = windowFlags();
        if (pinned)
            flags |= Qt::WindowStaysOnTopHint;
        else
            flags &= ~Qt::WindowStaysOnTopHint;
        setWindowFlags(flags);
        show();   // setWindowFlags() 호출 시 창이 숨겨지므로 재호출 필수
        raise();
        // setWindowFlags()가 hideEvent를 트리거해 opacity를 1.0으로 리셋함
        // idle 상태였다면 핀 ON 시에만 다시 투명화
        if (pinned && m_opacityAtIdle)
            QTimer::singleShot(300, this, [this]() {
                if (isVisible() && m_pinBtn->isChecked() && m_opacityAtIdle)
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

    auto *closeBtn = new QPushButton("x");
    closeBtn->setFixedSize(22, 22);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(btnBase + R"(
        QPushButton:hover { color: white; background: #c0392b; border-radius: 3px; }
    )");
    connect(closeBtn, &QPushButton::clicked, this, [this]() {
        QuitConfirmDialog dlg(this);
        // 부모 팝업 중앙에 배치
        const QPoint center = geometry().center();
        dlg.adjustSize();
        dlg.move(center.x() - dlg.width() / 2, center.y() - dlg.height() / 2);
        if (dlg.exec() == QDialog::Accepted)
            emit quitRequested();
    });

    titleRow->addWidget(m_pinBtn);
    titleRow->addSpacing(6);
    titleRow->addWidget(titleLabel);
    titleRow->addStretch();
    titleRow->addWidget(minimizeBtn);
    titleRow->addWidget(closeBtn);

    m_activityLine = new QWidget;
    m_activityLine->setFixedHeight(3);
    m_activityLine->setStyleSheet("background: #2ecc71;");
    m_lineEffect = new QGraphicsOpacityEffect(m_activityLine);
    m_lineEffect->setOpacity(0.0);
    m_activityLine->setGraphicsEffect(m_lineEffect);
    m_lineAnim = new QPropertyAnimation(m_lineEffect, "opacity", this);
    m_lineAnim->setEasingCurve(QEasingCurve::InOutQuad);

    auto *sep1 = new QFrame;
    sep1->setFrameShape(QFrame::HLine);
    sep1->setStyleSheet("color: #444;");

    m_panel5h = new QuotaPanel("5h 사용량");

    auto *sep2 = new QFrame;
    sep2->setFrameShape(QFrame::HLine);
    sep2->setStyleSheet("color: #ddd;");

    m_panel7d = new QuotaPanel("7d 사용량");

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

    footerLayout->addWidget(m_statusLabel);

    root->addWidget(m_titleBar);
    root->addWidget(m_activityLine);
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

    // 활성 라인 색상: 7d >= 5h → 5h 기준, 7d < 5h → 7d 기준
    const double dominant = (data.sevenDay.utilization >= data.fiveHour.utilization)
        ? data.fiveHour.utilization
        : data.sevenDay.utilization;

    QColor lineColor;
    const int pct = qRound(dominant * 100.0);
    if (pct < USAGE_WARN_PCT)      lineColor = QColor("#28a745");
    else if (pct < USAGE_CRIT_PCT) lineColor = QColor("#ffc107");
    else                           lineColor = QColor("#dc3545");

    m_activityLine->setStyleSheet(
        QString("background: %1;").arg(lineColor.name()));
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
}

bool UsagePopup::eventFilter(QObject *obj, QEvent *event)
{
    if (obj != m_titleBar)
        return QWidget::eventFilter(obj, event);

    if (event->type() == QEvent::MouseButtonPress) {
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
            if (m_wasOpacityIdleBeforeDrag && m_pinBtn->isChecked()) animateOpacityTo(0.6);  // 핀 고정 시만 복원
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

void UsagePopup::setActive()
{
    m_idleMode      = false;
    m_opacityAtIdle = false;

    // 라인 fade in
    m_lineAnim->stop();
    m_lineAnim->setDuration(300);
    m_lineAnim->setStartValue(m_lineEffect->opacity());
    m_lineAnim->setEndValue(1.0);
    m_lineAnim->start();

    // 이미 불투명이거나 불투명 방향으로 진행 중이면 스킵
    if (windowOpacity() >= 1.0 && m_opacityAnim->endValue().toDouble() >= 1.0)
        return;
    animateOpacityTo(1.0);
}

void UsagePopup::setIdle()
{
    if (m_idleMode) return;  // 이미 페이드 중이거나 idle 상태 → 중복 호출 무시
    m_idleMode = true;

    // 라인 2초 후 fade out
    QTimer::singleShot(2'000, this, [this]() {
        if (!m_idleMode) return;
        m_lineAnim->stop();
        m_lineAnim->setDuration(600);
        m_lineAnim->setStartValue(m_lineEffect->opacity());
        m_lineAnim->setEndValue(0.0);
        m_lineAnim->start();
    });

    // LED 페이드 완료 시점(10초)에 맞춰 직접 투명화 — 핀 고정 상태일 때만
    QTimer::singleShot(10'000, this, [this]() {
        if (!m_idleMode) return;  // 그 사이 setActive() 호출됐으면 무시
        if (!m_pinBtn->isChecked()) return;  // 고정 해제 모드엔 투명도 적용 안 함
        m_opacityAtIdle = true;
        if (isVisible())
            animateOpacityTo(0.6);
    });
}

void UsagePopup::hideEvent(QHideEvent *event)
{
    m_opacityAnim->stop();
    setWindowOpacity(1.0);  // 다음 show() 시 항상 불투명에서 시작
    QWidget::hideEvent(event);
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
    if (m_opacityAtIdle && m_pinBtn->isChecked())
        QTimer::singleShot(500, this, [this]() { if (isVisible() && m_pinBtn->isChecked()) animateOpacityTo(0.6); });
}
