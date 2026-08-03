#include "UsageWindow.h"
#include "QuotaGauge.h"
#include "StatusDot.h"
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStyleOption>
#include <QVariantAnimation>
#include <QVBoxLayout>

namespace {
constexpr int kWidth         = 280;
constexpr int kTitleHeight   = 36;
constexpr int kSlideDuration = 200;

// 크레딧 게이지·트레이 링에 함께 쓰는 강조색.
const char *kAccentBlue = "#0a84ff";

const char *kCardStyle = R"(
    QFrame#settingsCard {
        background: #f5f5f7;
        border: 1px solid #e6e6ea;
        border-radius: 10px;
    }
)";

// 세그먼트 버튼 — 눌린 쪽만 흰 알약으로 떠오른다.
const char *kSegmentStyle = R"(
    QPushButton {
        background: transparent;
        border: none;
        border-radius: 7px;
        padding: 5px 4px;
        font-size: 11px;
        color: #666;
    }
    QPushButton:checked {
        background: white;
        color: #111;
        font-weight: bold;
    }
    QPushButton:hover:!checked { color: #222; }
)";

const char *kCheckStyle  = "font-size: 11px; font-weight: bold; color: #222;";
const char *kDescStyle   = "color: #7a7a80; font-size: 10px;";
const char *kCardTitle   = "font-weight: bold; font-size: 11px; color: #3a3a3f;";
} // namespace

UsageWindow::UsageWindow(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setFixedWidth(kWidth);
    setAttribute(Qt::WA_TranslucentBackground);

    {
        QSettings s("ClaudeTray", "ClaudeTray");
        m_viewMode       = s.value("viewMode", 0).toInt();
        m_alwaysOnTop    = s.value("alwaysOnTop", true).toBool();
        m_disableShimmer = s.value("disableShimmer", false).toBool();
        m_opacityEnabled = s.value("opacityEnabled", false).toBool();
        m_opacityPct     = qBound(40, s.value("windowOpacity", 85).toInt(), 100);
    }

    Qt::WindowFlags flags = windowFlags();
    if (m_alwaysOnTop)
        flags |= Qt::WindowStaysOnTopHint;
    else
        flags &= ~Qt::WindowStaysOnTopHint;
    setWindowFlags(flags);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    const QString btnBase = R"(
        QPushButton {
            background: transparent;
            color: #aaa;
            border: none;
            font-size: 13px;
        }
        QPushButton:hover { color: white; background: #555; border-radius: 3px; }
    )";

    buildTitleBar(btnBase);
    rootLayout->addWidget(m_titleBar);

    m_bodyHost = new QWidget;
    m_bodyHost->setFixedWidth(kWidth);
    rootLayout->addWidget(m_bodyHost);

    buildDashboardBody();
    buildSettingsBody();

    setObjectName("UsageWindow");
    setStyleSheet("#UsageWindow { background: white; border-radius: 8px; border: 1px solid #cccccc; }");

    m_slideAnim = new QVariantAnimation(this);
    m_slideAnim->setDuration(kSlideDuration);
    m_slideAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_slideAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_slidePos = v.toReal();
        applySlide();
    });

    m_panel5h->setDisableShimmer(m_disableShimmer);
    m_panel7d->setDisableShimmer(m_disableShimmer);
    m_extraBar->setDisableShimmer(m_disableShimmer);
    m_extraBar->setAccentColor(QColor(kAccentBlue));

    applyWindowOpacity();
    updateLayoutVisibility();
    refreshExecutionState();
}

// ── 타이틀바 ──────────────────────────────────────────────────────────────────
// 36px 고정. 슬라이드하지 않고, 안쪽 내용만 제자리에서 바뀐다.

void UsageWindow::buildTitleBar(const QString &buttonStyle)
{
    m_titleBar = new QWidget;
    m_titleBar->setObjectName("titleBar");
    m_titleBar->setFixedHeight(kTitleHeight);
    m_titleBar->setStyleSheet("#titleBar { background: #2d2d2d; border-top-left-radius: 8px; border-top-right-radius: 8px; }");
    m_titleBar->setCursor(Qt::SizeAllCursor);
    m_titleBar->installEventFilter(this);

    auto *outer = new QVBoxLayout(m_titleBar);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleStack = new QStackedWidget;
    m_titleStack->setFixedHeight(kTitleHeight);
    outer->addWidget(m_titleStack);

    // ── 대시보드 모드: [점] [모델명] … [⚙] [−] ──────────────────────────────
    auto *dashTitle = new QWidget;
    auto *dashLayout = new QHBoxLayout(dashTitle);
    dashLayout->setContentsMargins(12, 0, 8, 0);
    dashLayout->setSpacing(0);

    m_statusDot = new StatusDot;

    m_recentModelLabel = new QLabel("--");
    m_recentModelLabel->setStyleSheet("color: white; font-weight: bold; font-size: 12px;");

    m_gearBtn = new QPushButton("⚙");
    m_gearBtn->setFixedSize(22, 22);
    m_gearBtn->setCursor(Qt::PointingHandCursor);
    m_gearBtn->setToolTip("설정");
    m_gearBtn->setStyleSheet(buttonStyle);
    connect(m_gearBtn, &QPushButton::clicked, this, [this]() { slideToPage(1); });

    m_minimizeBtn = new QPushButton("−");
    m_minimizeBtn->setFixedSize(22, 22);
    m_minimizeBtn->setCursor(Qt::PointingHandCursor);
    m_minimizeBtn->setToolTip("숨기기");
    m_minimizeBtn->setStyleSheet(buttonStyle);
    connect(m_minimizeBtn, &QPushButton::clicked, this, &UsageWindow::hideAndSavePos);

    dashLayout->addWidget(m_statusDot);
    dashLayout->addSpacing(7);
    dashLayout->addWidget(m_recentModelLabel);
    dashLayout->addStretch();
    dashLayout->addWidget(m_gearBtn);
    dashLayout->addWidget(m_minimizeBtn);

    // ── 설정 모드: [‹ 대시보드] … [설정] … [−] ──────────────────────────────
    auto *setTitle = new QWidget;
    auto *setLayout = new QHBoxLayout(setTitle);
    setLayout->setContentsMargins(8, 0, 8, 0);
    setLayout->setSpacing(0);

    m_backBtn = new QPushButton("‹  대시보드");
    m_backBtn->setCursor(Qt::PointingHandCursor);
    m_backBtn->setStyleSheet(R"(
        QPushButton {
            background: transparent; border: none;
            color: #9a9aa0; font-size: 11px; padding: 3px 6px;
        }
        QPushButton:hover { color: white; }
    )");
    connect(m_backBtn, &QPushButton::clicked, this, [this]() { slideToPage(0); });

    auto *settingsTitleLabel = new QLabel("설정");
    settingsTitleLabel->setStyleSheet("color: white; font-weight: bold; font-size: 12px;");

    m_minimizeBtnSettings = new QPushButton("−");
    m_minimizeBtnSettings->setFixedSize(22, 22);
    m_minimizeBtnSettings->setCursor(Qt::PointingHandCursor);
    m_minimizeBtnSettings->setToolTip("숨기기");
    m_minimizeBtnSettings->setStyleSheet(buttonStyle);
    connect(m_minimizeBtnSettings, &QPushButton::clicked, this, &UsageWindow::hideAndSavePos);

    setLayout->addWidget(m_backBtn);
    setLayout->addStretch();
    setLayout->addWidget(settingsTitleLabel);
    setLayout->addStretch();
    setLayout->addWidget(m_minimizeBtnSettings);

    m_titleStack->addWidget(dashTitle);
    m_titleStack->addWidget(setTitle);
}

// ── 대시보드 본문 ─────────────────────────────────────────────────────────────

void UsageWindow::buildDashboardBody()
{
    m_dashboardBody = new QWidget(m_bodyHost);
    auto *layout = new QVBoxLayout(m_dashboardBody);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_panel5h = new QuotaGauge("5h 사용량");
    m_panel7d = new QuotaGauge("7d 사용량");

    m_sepQuota = new QFrame; m_sepQuota->setFrameShape(QFrame::HLine); m_sepQuota->setStyleSheet("color: #ddd;");
    m_sepExtra = new QFrame; m_sepExtra->setFrameShape(QFrame::HLine); m_sepExtra->setStyleSheet("color: #ddd;");

    m_extraWidget = new QWidget;
    m_extraWidget->setFixedWidth(250);
    auto *extraLayout = new QVBoxLayout(m_extraWidget);
    extraLayout->setContentsMargins(14, 8, 14, 8);
    extraLayout->setSpacing(4);

    auto *extraRow = new QHBoxLayout;
    m_extraTitleLabel = new QLabel("크레딧 사용량");
    m_extraTitleLabel->setStyleSheet("font-weight: bold; font-size: 12px;");
    m_extraPctLabel = new QLabel("--");
    m_extraPctLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_extraPctLabel->setStyleSheet("font-weight: bold; font-size: 12px;");
    extraRow->addWidget(m_extraTitleLabel);
    extraRow->addWidget(m_extraPctLabel);

    m_extraBar    = new ThresholdBar;
    m_extraDetail = new QLabel("--");
    m_extraDetail->setStyleSheet("color: #888; font-size: 10px;");

    extraLayout->addLayout(extraRow);
    extraLayout->addWidget(m_extraBar);
    extraLayout->addWidget(m_extraDetail);

    layout->addWidget(m_panel5h);
    layout->addWidget(m_sepQuota);
    layout->addWidget(m_panel7d);
    layout->addWidget(m_sepExtra);
    layout->addWidget(m_extraWidget);
}

// ── 설정 본문 ─────────────────────────────────────────────────────────────────

QWidget *UsageWindow::makeCard(const QString &title, QWidget **bodyOut)
{
    auto *wrapper = new QWidget;
    auto *wrapLayout = new QVBoxLayout(wrapper);
    wrapLayout->setContentsMargins(0, 0, 0, 0);
    wrapLayout->setSpacing(5);

    auto *heading = new QLabel(title);
    heading->setStyleSheet(kCardTitle);

    auto *card = new QFrame;
    card->setObjectName("settingsCard");
    card->setStyleSheet(kCardStyle);

    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(11, 10, 11, 11);
    cardLayout->setSpacing(5);

    wrapLayout->addWidget(heading);
    wrapLayout->addWidget(card);

    *bodyOut = card;
    return wrapper;
}

void UsageWindow::buildSettingsBody()
{
    m_settingsBody = new QWidget(m_bodyHost);
    auto *body = new QVBoxLayout(m_settingsBody);
    body->setContentsMargins(12, 11, 12, 12);
    body->setSpacing(11);

    // 창 폭이 280 으로 고정이라 줄바꿈을 켜지 않으면 설명이 잘린다.
    const auto makeDesc = [](const QString &text) {
        auto *l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet(kDescStyle);
        return l;
    };

    // ── 🎨 화면 스타일 ───────────────────────────────────────────────────────
    QWidget *styleCard = nullptr;
    QWidget *styleWrap = makeCard("화면 스타일", &styleCard);
    auto *styleLayout = static_cast<QVBoxLayout *>(styleCard->layout());

    auto *segHolder = new QWidget;
    segHolder->setStyleSheet("background: #e4e4e9; border-radius: 8px;");
    auto *segLayout = new QHBoxLayout(segHolder);
    segLayout->setContentsMargins(2, 2, 2, 2);
    segLayout->setSpacing(2);

    m_segCompact = new QPushButton("컴팩트 뷰");
    m_segFull    = new QPushButton("전체 뷰");
    for (QPushButton *b : {m_segCompact, m_segFull}) {
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(kSegmentStyle);
        segLayout->addWidget(b);
    }
    (m_viewMode == 0 ? m_segCompact : m_segFull)->setChecked(true);

    connect(m_segCompact, &QPushButton::toggled, this, [this](bool on) {
        if (!on) return;
        m_viewMode = 0;
        QSettings("ClaudeTray", "ClaudeTray").setValue("viewMode", 0);
        updateLayoutVisibility();
    });
    connect(m_segFull, &QPushButton::toggled, this, [this](bool on) {
        if (!on) return;
        m_viewMode = 1;
        QSettings("ClaudeTray", "ClaudeTray").setValue("viewMode", 1);
        updateLayoutVisibility();
    });

    m_opacityCheck = new QCheckBox("창 투명도 사용");
    m_opacityCheck->setStyleSheet(kCheckStyle);
    m_opacityCheck->setChecked(m_opacityEnabled);

    auto *sliderRow = new QWidget;
    auto *sliderLayout = new QHBoxLayout(sliderRow);
    sliderLayout->setContentsMargins(0, 0, 0, 0);
    sliderLayout->setSpacing(8);

    m_opacitySlider = new QSlider(Qt::Horizontal);
    m_opacitySlider->setRange(40, 100);
    m_opacitySlider->setValue(m_opacityPct);
    m_opacitySlider->setEnabled(m_opacityEnabled);
    m_opacitySlider->setCursor(Qt::PointingHandCursor);

    m_opacityValueLabel = new QLabel(QString("%1%").arg(m_opacityPct));
    m_opacityValueLabel->setFixedWidth(32);
    m_opacityValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_opacityValueLabel->setStyleSheet("font-size: 10px; font-weight: bold; color: #444;");
    m_opacityValueLabel->setEnabled(m_opacityEnabled);

    sliderLayout->addWidget(m_opacitySlider);
    sliderLayout->addWidget(m_opacityValueLabel);

    connect(m_opacityCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_opacityEnabled = on;
        QSettings("ClaudeTray", "ClaudeTray").setValue("opacityEnabled", on);
        m_opacitySlider->setEnabled(on);
        m_opacityValueLabel->setEnabled(on);
        applyWindowOpacity();
    });
    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int v) {
        m_opacityPct = v;
        m_opacityValueLabel->setText(QString("%1%").arg(v));
        QSettings("ClaudeTray", "ClaudeTray").setValue("windowOpacity", v);
        applyWindowOpacity();   // 끄는 순간 되돌아가도록 실시간 반영
    });

    styleLayout->addWidget(segHolder);
    styleLayout->addWidget(makeDesc("컴팩트 뷰는 지금 당장 중요한 것만, 전체 뷰는 5h·7d 를 항상 함께 보여줍니다."));
    styleLayout->addSpacing(3);
    styleLayout->addWidget(m_opacityCheck);
    styleLayout->addWidget(makeDesc("창을 살짝 비쳐 보이게 해 뒤쪽 작업을 가리지 않습니다."));
    styleLayout->addWidget(sliderRow);

    // ── ⚡ 동작 및 효과 ──────────────────────────────────────────────────────
    QWidget *behaveCard = nullptr;
    QWidget *behaveWrap = makeCard("동작 및 효과", &behaveCard);
    auto *behaveLayout = static_cast<QVBoxLayout *>(behaveCard->layout());

    m_alwaysOnTopCheck = new QCheckBox("창을 항상 위에 고정 (Pin)");
    m_alwaysOnTopCheck->setStyleSheet(kCheckStyle);
    m_alwaysOnTopCheck->setChecked(m_alwaysOnTop);
    connect(m_alwaysOnTopCheck, &QCheckBox::toggled, this, [this](bool checked) {
        updateAlwaysOnTop(checked);
    });

    // 저장 키는 예전 그대로(disableShimmer) 두고 체크 의미만 긍정문으로 뒤집는다.
    m_shimmerCheck = new QCheckBox("클로드 작업 중 게이지 물결효과 표시");
    m_shimmerCheck->setStyleSheet(kCheckStyle);
    m_shimmerCheck->setChecked(!m_disableShimmer);
    connect(m_shimmerCheck, &QCheckBox::toggled, this, [this](bool show) {
        m_disableShimmer = !show;
        QSettings("ClaudeTray", "ClaudeTray").setValue("disableShimmer", m_disableShimmer);
        m_panel5h->setDisableShimmer(m_disableShimmer);
        m_panel7d->setDisableShimmer(m_disableShimmer);
        m_extraBar->setDisableShimmer(m_disableShimmer);
    });

    m_approvalDetectCheck = new QCheckBox("승인 대기 감지 사용");
    m_approvalDetectCheck->setStyleSheet(kCheckStyle);
    m_approvalDetectCheck->setChecked(HookBridge::hooksInstalled());
    m_approvalHint = makeDesc("Claude Code 설정에 훅을 등록해, 클로드가 승인을 기다리는 "
                              "순간을 노란 점으로 정확히 알려줍니다.");
    connect(m_approvalDetectCheck, &QCheckBox::toggled, this, [this](bool checked) {
        QString err;
        const bool ok = checked ? HookBridge::installHooks(&err)
                                : HookBridge::uninstallHooks(&err);
        if (!ok) {
            // 설정 파일을 못 건드렸으면 체크 상태가 거짓말을 하면 안 된다.
            const QSignalBlocker block(m_approvalDetectCheck);
            m_approvalDetectCheck->setChecked(!checked);
            m_approvalHint->setStyleSheet("color: #c0392b; font-size: 10px;");
            m_approvalHint->setText(err);
            updateLayoutVisibility();
            return;
        }
        m_approvalHint->setStyleSheet(kDescStyle);
        m_approvalHint->setText(checked
            ? "등록했습니다. 다음에 시작하는 Claude Code 세션부터 적용됩니다."
            : "해제했습니다. 승인 대기 표시가 꺼집니다.");
        updateLayoutVisibility();
        emit approvalDetectionChanged();
    });

    behaveLayout->addWidget(m_alwaysOnTopCheck);
    behaveLayout->addWidget(makeDesc("다른 프로그램을 쓰는 동안에도 대시보드가 위에 남습니다."));
    behaveLayout->addSpacing(3);
    behaveLayout->addWidget(m_shimmerCheck);
    behaveLayout->addWidget(makeDesc("클로드가 일하는 동안 게이지에 빛이 흐릅니다. 꺼 두면 완전히 정적입니다."));
    behaveLayout->addSpacing(3);
    behaveLayout->addWidget(m_approvalDetectCheck);
    behaveLayout->addWidget(m_approvalHint);

    body->addWidget(styleWrap);
    body->addWidget(behaveWrap);
}

// ── 페이지 전환 ───────────────────────────────────────────────────────────────

int UsageWindow::bodyHeight(QWidget *body)
{
    QLayout *l = body->layout();
    if (!l)
        return body->sizeHint().height();

    // 라벨 텍스트가 바뀌면 QWidget::updateGeometry 는 LayoutRequest 를 '게시'만
    // 하므로, 바로 이어서 재면 아직 예전 캐시가 나온다(크레딧 상세가 두 줄로
    // 늘어나도 창이 안 커져서 글자가 잘렸다). 밀린 요청을 먼저 흘려보낸다.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    for (QLayout *nested : body->findChildren<QLayout *>())
        nested->invalidate();
    l->invalidate();
    l->activate();
    // 설명 라벨이 줄바꿈되므로 폭에 따라 높이가 달라진다.
    return l->hasHeightForWidth() ? l->heightForWidth(kWidth)
                                  : l->sizeHint().height();
}

void UsageWindow::applySlide()
{
    const int hDash = bodyHeight(m_dashboardBody);
    const int hSet  = bodyHeight(m_settingsBody);
    const int h = qRound(hDash + (hSet - hDash) * m_slidePos);

    m_dashboardBody->setGeometry(qRound(-m_slidePos * kWidth), 0, kWidth, hDash);
    m_settingsBody->setGeometry(qRound((1.0 - m_slidePos) * kWidth), 0, kWidth, hSet);

    // 창 높이 = 고정 타이틀바 + 지금 보이는 본문. 남는 여백이 생기지 않는다.
    m_bodyHost->setFixedHeight(h);
    setFixedHeight(kTitleHeight + h);
}

void UsageWindow::slideToPage(int index)
{
    const qreal target = (index == 0) ? 0.0 : 1.0;
    if (m_currentPage == index && m_slideAnim->state() != QAbstractAnimation::Running)
        return;

    m_currentPage = index;
    // 타이틀바는 미끄러지지 않고 내용만 그 자리에서 바뀐다.
    m_titleStack->setCurrentIndex(index);

    m_slideAnim->stop();
    m_slideAnim->setStartValue(m_slidePos);
    m_slideAnim->setEndValue(target);
    m_slideAnim->start();
}

// 크레딧 상세는 두 조각(금액 + 리셋 시각)으로 이뤄지는데, 예전에는 금액을
// applyDataInternal 이, 리셋 시각을 applyCountdownsInternal 이 따로 써 넣었다.
// TrayController::applyData 가 setData → setCountdowns 순으로 부르므로 갱신마다
// '1줄로 줄였다가 2줄로 늘리는' 왕복이 생겼고, 그 사이에 그려지면 글자가 잘렸다.
// 이제 여기 한 곳에서만 만들고, 호출측은 반드시 측정 전에 부른다.
void UsageWindow::updateCreditDetail()
{
    if (!creditVisible())
        return;

    QString text = QString("$%1 / $%2")
                       .arg(m_currentData.extraUsage.usedCredits, 0, 'f', 2)
                       .arg(m_currentData.extraUsage.limitDollars, 0, 'f', 2);

    // 컴팩트 뷰에서는 5h 게이지가 숨겨져 리셋 시각을 볼 곳이 여기뿐이다.
    // 전체 뷰에는 5h 게이지가 그대로 있으므로 중복해서 붙이지 않는다.
    // 한 줄에 몰면 250px 를 넘겨 잘리므로 줄을 나눈다.
    if (m_viewMode == 0 && !m_c5hText.isEmpty())
        text += "\n" + m_c5hText;

    m_extraDetail->setText(text);
}

void UsageWindow::updateLayoutVisibility()
{
    // 높이를 재기 전에 문구부터 확정한다. 순서가 뒤집히면 창이 한 줄 크기로
    // 잡힌 뒤 두 줄이 그려져 잘린다.
    updateCreditDetail();

    const bool extra = creditVisible();

    if (m_viewMode == 0) {
        // 컴팩트: 크레딧이 '실제로 나가는 중'일 때만 100% 로 꽉 찬 게이지 대신
        // 크레딧을 세운다. 추가 결제가 켜져 있다는 이유만으로 바꾸면 아직 5h 가
        // 남아 있는 첫 화면부터 크레딧이 자리를 차지한다.
        const bool metering = creditMetering();
        m_panel5h->setVisible(!metering);
        m_sepQuota->setVisible(false);
        m_panel7d->setVisible(false);
        m_sepExtra->setVisible(false);
        m_extraWidget->setVisible(metering);
    } else {
        m_panel5h->setVisible(true);
        m_sepQuota->setVisible(true);
        m_panel7d->setVisible(true);
        m_sepExtra->setVisible(extra);
        m_extraWidget->setVisible(extra);
    }

    applySlide();
}

void UsageWindow::applyWindowOpacity()
{
    setWindowOpacity(m_opacityEnabled ? m_opacityPct / 100.0 : 1.0);
}

// ── 데이터 반영 ───────────────────────────────────────────────────────────────

// 크레딧 카드를 보여줄 수 있는가 = 계정에 추가 결제가 켜져 있는가.
bool UsageWindow::creditVisible() const
{
    return m_currentData.extraUsage.enabled;
}

// 지금 크레딧이 나가는 중인가. 컴팩트 뷰에서 5h 대신 크레딧을 세우는 기준이다.
bool UsageWindow::creditMetering() const
{
    return isCreditMetering(m_currentData);
}

void UsageWindow::setData(const UsageData &data)
{
    if (m_isDragging) {
        m_pendingData    = data;
        m_hasPendingData = true;
        return;
    }
    applyDataInternal(data);
}

void UsageWindow::setCountdowns(const QString &c5h, const QString &c7d)
{
    if (m_isDragging) {
        m_pendingC5h   = c5h;
        m_pendingC7d   = c7d;
        m_hasPendingCD = true;
        return;
    }
    applyCountdownsInternal(c5h, c7d);
}

void UsageWindow::setRefreshState(RefreshState state, QDateTime lastFetch, QDateTime nextFetch)
{
    if (m_isDragging) {
        m_hasPendingRefreshState = true;
        m_pendingRefreshState    = state;
        m_pendingLastFetch       = lastFetch;
        m_pendingNextFetch       = nextFetch;
        return;
    }

    m_refreshState = state;
    if (lastFetch.isValid()) m_lastFetch = lastFetch;
    if (nextFetch.isValid()) m_nextFetch = nextFetch;
}

void UsageWindow::refreshNextFetch(QDateTime nextFetch)
{
    m_nextFetch = nextFetch;
}

void UsageWindow::applyDataInternal(const UsageData &data)
{
    m_currentData  = data;
    m_rawModelName = data.recentModel;

    m_panel5h->setData(data.fiveHour);
    m_panel7d->setData(data.sevenDay);

    if (creditVisible()) {
        int pct = qRound(data.extraUsage.utilization * 100.0);
        pct = qBound(0, pct, 100);

        m_extraPctLabel->setText(QString("%1%").arg(pct));
        m_extraBar->setValue(pct);
    }

    // 새 사용률이 한도에 닿았거나 반대로 리셋됐을 수 있다. 물결 대상은
    // 데이터에 달려 있으므로 여기서도 다시 정한다.
    updateShimmerTargets();

    // 문구 확정 → 표시 여부 → 높이 측정 순서를 지킨다.
    updateLayoutVisibility();
    updateStatusIndicator();
}

void UsageWindow::applyCountdownsInternal(const QString &c5h, const QString &c7d)
{
    m_c5hText = c5h;
    m_c7dText = c7d;
    m_panel5h->setCountdown(c5h);
    m_panel7d->setCountdown(c7d);

    // 크레딧 모드에서는 이 값이 상세 문구의 두 번째 줄이 되므로 높이가 달라진다.
    // 크레딧이 아니면 게이지의 한 줄짜리 라벨만 바뀌어 높이는 그대로다.
    updateLayoutVisibility();
}

void UsageWindow::applyPending()
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
        m_refreshState = m_pendingRefreshState;
        if (m_pendingLastFetch.isValid()) m_lastFetch = m_pendingLastFetch;
        if (m_pendingNextFetch.isValid()) m_nextFetch = m_pendingNextFetch;
        m_hasPendingRefreshState = false;
    }
}

// ── 실행 상태 ─────────────────────────────────────────────────────────────────

void UsageWindow::setAgentState(HookBridge::AgentState state)
{
    if (m_hookState == state) return;
    m_hookState = state;
    refreshExecutionState();
}

void UsageWindow::setActive()
{
    m_idleMode = false;
    refreshExecutionState();
}

void UsageWindow::setIdle()
{
    m_idleMode = true;
    refreshExecutionState();
}

void UsageWindow::refreshExecutionState()
{
    using AS = HookBridge::AgentState;

    // 훅이 켜져 있으면 그쪽이 정확하다. 꺼져 있을 때만(Unknown) 토큰 스캔 기반
    // 활동 감지로 되돌아간다. 그 경우 승인 대기는 알 수 없어 표시하지 않는다.
    ExecutionState next;
    switch (m_hookState) {
    case AS::PendingApproval: next = ExecutionState::PendingApproval; break;
    case AS::Running:         next = ExecutionState::Running;         break;
    case AS::Idle:            next = ExecutionState::Idle;            break;
    case AS::Unknown:
    default:
        next = m_idleMode ? ExecutionState::Idle : ExecutionState::Running;
        break;
    }

    m_execState = next;
    updateShimmerTargets();
    updateStatusIndicator();
}

// 물결(shimmer)은 "이 막대가 지금 차오르고 있다"는 신호다. 그래서 작업 중인지
// 만으로는 부족하고, 그 창이 실제로 사용량을 받아내고 있어야 한다.
//
//   5h·7d — 한도를 채우고 나면 더는 차지 않는다. 초과분은 크레딧으로 넘어가는데
//           100% 로 멈춰 선 막대가 계속 물결치면 아직 오르는 중으로 읽힌다.
//   크레딧 — 플랜 한도가 남아 있는 동안에는 한 푼도 나가지 않는다. 추가 결제를
//           켜 뒀다는 이유만으로 물결이 돌면 돈이 나가는 것처럼 보인다.
//
// 조건이 실행 상태(refreshExecutionState)와 데이터(applyDataInternal) 양쪽에서
// 바뀌므로 두 곳 모두에서 부른다. ThresholdBar::setActive 는 값이 같으면 즉시
// 돌아오므로 중복 호출은 비용이 없다.
void UsageWindow::updateShimmerTargets()
{
    const bool running = (m_execState == ExecutionState::Running);

    m_panel5h->setActive(running && !isQuotaSaturated(m_currentData.fiveHour));
    m_panel7d->setActive(running && !isQuotaSaturated(m_currentData.sevenDay));
    m_extraBar->setActive(running && isCreditMetering(m_currentData));
}

void UsageWindow::updateStatusIndicator()
{
    // 타이틀바에는 모델명만 둔다. 상태는 왼쪽 점 하나가 전부 말해 준다.
    QString name = m_rawModelName;
    if (name.startsWith("claude-"))
        name = name.mid(7);

    QStringList parts = name.split('-');
    if (parts.size() > 2 && parts.last().length() >= 8 && parts.last().toLongLong() > 0) {
        parts.removeLast();
        name = parts.join('-');
    }
    if (name.isEmpty())
        name = "Claude";

    StatusDot::Mode dot;
    QString tip;
    switch (m_execState) {
    case ExecutionState::PendingApproval:
        dot = StatusDot::Mode::PendingApproval;
        tip = "승인 대기 중";
        break;
    case ExecutionState::Running:
        dot = StatusDot::Mode::Running;
        tip = "작업 중";
        break;
    case ExecutionState::Idle:
    default:
        dot = StatusDot::Mode::Idle;
        tip = "대화 가능";
        break;
    }

    m_statusDot->setMode(dot);
    m_statusDot->setToolTip(tip);
    m_recentModelLabel->setText("Model : " + name);
    m_recentModelLabel->setToolTip(QString("%1\n최근 사용 모델: %2").arg(tip, m_rawModelName));
}

// ── 창 동작 ───────────────────────────────────────────────────────────────────

void UsageWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

bool UsageWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj != m_titleBar)
        return QWidget::eventFilter(obj, event);

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            m_dragPos    = me->globalPosition().toPoint() - frameGeometry().topLeft();
            m_isDragging = true;
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
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

void UsageWindow::updateAlwaysOnTop(bool pinned)
{
    if (m_alwaysOnTop == pinned) return;
    m_alwaysOnTop = pinned;

    QSettings("ClaudeTray", "ClaudeTray").setValue("alwaysOnTop", pinned);

    const QPoint oldPos = pos();
    Qt::WindowFlags flags = windowFlags();
    if (pinned)
        flags |= Qt::WindowStaysOnTopHint;
    else
        flags &= ~Qt::WindowStaysOnTopHint;

    // setWindowFlags 는 창을 숨기므로 다시 띄우고 화면 안으로 되돌린다.
    setWindowFlags(flags);
    show();
    raise();
    activateWindow();
    applyWindowOpacity();   // 플래그를 다시 세우면 투명도가 풀린다

    QScreen *screen = QApplication::screenAt(oldPos);
    if (!screen) screen = QApplication::primaryScreen();
    const QRect avail = screen->availableGeometry();
    move(qBound(avail.left(), oldPos.x(), avail.right()  - width()),
         qBound(avail.top(),  oldPos.y(), avail.bottom() - height()));
}

void UsageWindow::moveNear(const QPoint &trayPos)
{
    const int w = width();
    const int h = height();

    const QPoint anchor = m_hasRememberedPos ? m_rememberedPos : trayPos;
    QScreen *screen = QApplication::screenAt(anchor);
    if (!screen)
        screen = QApplication::primaryScreen();
    const QRect avail = screen->availableGeometry();

    if (m_hasRememberedPos) {
        move(qBound(avail.left(), m_rememberedPos.x(), avail.right()  - w),
             qBound(avail.top(),  m_rememberedPos.y(), avail.bottom() - h));
    } else {
        move(qBound(avail.left(), trayPos.x() - w / 2, avail.right()  - w),
             qBound(avail.top(),  trayPos.y() - h - 8, avail.bottom() - h));
    }
}

// 한 번도 표시되지 않은 창의 레이아웃 높이는 믿을 수 없다. 자식 위젯은 부모가
// 처음 show 될 때까지 WA_WState_Hidden 이라, QWidgetItem::isEmpty() 가 참이 되어
// 레이아웃 계산에서 통째로 빠진다. 그래서 크레딧 상세가 두 줄인데도 한 줄
// 높이(102px)로 창이 잡혀 글자가 잘렸다. 실제로 보인 뒤 다시 잰다.
void UsageWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    applySlide();
}

void UsageWindow::showNearTray(const QPoint &trayPos)
{
    applySlide();
    moveNear(trayPos);      // 우선 배치 — 화면 밖에서 뜨지 않게

    show();                 // showEvent 가 정확한 높이로 다시 잡는다
    moveNear(trayPos);      // 높이가 바뀌었을 수 있으니 위치를 보정

    raise();
    activateWindow();
}

void UsageWindow::hideAndSavePos()
{
    m_rememberedPos    = pos();
    m_hasRememberedPos = true;
    hide();
}
