#ifndef USAGEWINDOW_H
#define USAGEWINDOW_H

#include <QWidget>
#include <QDateTime>
#include "HookBridge.h"
#include "UsageTypes.h"

class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
class QSlider;
class QStackedWidget;
class QVariantAnimation;
class QuotaGauge;
class StatusDot;
class ThresholdBar;

class UsageWindow : public QWidget
{
    Q_OBJECT
public:
    enum class RefreshState { Fetching, Refreshed, LocalFallback, NetworkError };
    enum class ExecutionState { Idle, Running, PendingApproval };

    explicit UsageWindow(QWidget *parent = nullptr);

    void setData(const UsageData &data);
    void setCountdowns(const QString &c5h, const QString &c7d);
    void setRefreshState(RefreshState state, QDateTime lastFetch = {}, QDateTime nextFetch = {});
    void refreshNextFetch(QDateTime nextFetch);
    void showNearTray(const QPoint &trayPos);
    void hideAndSavePos();

    // 토큰 스캔이 알려주는 대략적 활동 여부. 훅이 꺼져 있을 때의 폴백이다.
    void setActive();
    void setIdle();
    // 훅이 알려주는 정확한 상태. 알려진 값이면 폴백보다 우선한다.
    void setAgentState(HookBridge::AgentState state);

signals:
    // 설정에서 승인 대기 감지를 켜고 끌 때. TrayController 가 감시를 다시 건다.
    void approvalDetectionChanged();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void buildTitleBar(const QString &buttonStyle);
    void buildDashboardBody();
    void buildSettingsBody();
    QWidget *makeCard(const QString &title, QWidget **bodyOut);

    void moveNear(const QPoint &trayPos);
    void slideToPage(int index);
    void applySlide();
    int  bodyHeight(QWidget *body);

    void applyDataInternal(const UsageData &data);
    void applyCountdownsInternal(const QString &c5h, const QString &c7d);
    void applyPending();
    void updateLayoutVisibility();
    // 크레딧 상세 문구의 유일한 생성 지점. 높이를 재기 '전'에 항상 최종 텍스트가
    // 확정돼 있어야 해서 한 곳으로 모았다.
    void updateCreditDetail();
    void updateAlwaysOnTop(bool pinned);
    void applyWindowOpacity();
    // 훅 상태와 폴백을 합쳐 최종 실행 상태를 정하고 점·shimmer 에 반영한다.
    void refreshExecutionState();
    void updateStatusIndicator();
    bool creditActive() const;

    // 타이틀바는 슬라이드에서 빠져 항상 제자리에 있다. 안쪽 내용만 스택으로
    // 바뀐다(두 페이지 모두 높이가 같아 스택 레이아웃의 크기 문제가 없다).
    QWidget        *m_titleBar   = nullptr;
    QStackedWidget *m_titleStack = nullptr;

    // 본문 호스트에는 레이아웃을 두지 않는다. 두 패널의 위치와 높이를 직접
    // 계산해야 슬라이드 도중 다른 갱신이 끼어들어도 흐트러지지 않는다.
    // 자식 위젯은 부모 사각형 밖으로 그려지지 않으므로 클리핑도 이것으로 된다.
    QWidget *m_bodyHost      = nullptr;
    QWidget *m_dashboardBody = nullptr;
    QWidget *m_settingsBody  = nullptr;

    QVariantAnimation *m_slideAnim = nullptr;
    qreal m_slidePos    = 0.0;   // 0.0 = 대시보드, 1.0 = 설정
    int   m_currentPage = 0;

    // 타이틀바 — 대시보드 모드
    StatusDot   *m_statusDot        = nullptr;
    QLabel      *m_recentModelLabel = nullptr;
    QPushButton *m_gearBtn          = nullptr;
    QPushButton *m_minimizeBtn      = nullptr;
    // 타이틀바 — 설정 모드
    QPushButton *m_backBtn             = nullptr;
    QPushButton *m_minimizeBtnSettings = nullptr;

    // 설정 컨트롤
    QPushButton *m_segCompact         = nullptr;
    QPushButton *m_segFull            = nullptr;
    QCheckBox   *m_opacityCheck       = nullptr;
    QSlider     *m_opacitySlider      = nullptr;
    QLabel      *m_opacityValueLabel  = nullptr;
    QCheckBox   *m_alwaysOnTopCheck   = nullptr;
    QCheckBox   *m_shimmerCheck       = nullptr;
    QCheckBox   *m_approvalDetectCheck = nullptr;
    QLabel      *m_approvalHint       = nullptr;

    // 대시보드 본문
    QuotaGauge   *m_panel5h          = nullptr;
    QuotaGauge   *m_panel7d          = nullptr;
    QFrame       *m_sepQuota         = nullptr;
    QFrame       *m_sepExtra         = nullptr;
    QWidget      *m_extraWidget      = nullptr;
    QLabel       *m_extraTitleLabel  = nullptr;
    QLabel       *m_extraPctLabel    = nullptr;
    ThresholdBar *m_extraBar         = nullptr;
    QLabel       *m_extraDetail      = nullptr;

    // 설정 영속값
    int  m_viewMode       = 0;      // 0 = 컴팩트(기본), 1 = 전체
    bool m_alwaysOnTop    = true;
    bool m_disableShimmer = false;
    bool m_opacityEnabled = false;
    int  m_opacityPct     = 85;     // 40 ~ 100

    // 실행 상태
    HookBridge::AgentState m_hookState = HookBridge::AgentState::Unknown;
    ExecutionState m_execState = ExecutionState::Idle;
    bool           m_idleMode  = true;
    QString        m_rawModelName;
    QString        m_c5hText;
    QString        m_c7dText;
    UsageData      m_currentData;

    // 창 위치
    QPoint m_dragPos;
    QPoint m_rememberedPos;
    bool   m_hasRememberedPos = false;

    // 갱신 상태
    RefreshState m_refreshState = RefreshState::Fetching;
    QDateTime    m_lastFetch;
    QDateTime    m_nextFetch;

    // 드래그 중 들어온 갱신은 놓았다가 한꺼번에 반영한다.
    bool         m_isDragging             = false;
    bool         m_hasPendingData         = false;
    UsageData    m_pendingData;
    bool         m_hasPendingCD           = false;
    QString      m_pendingC5h;
    QString      m_pendingC7d;
    bool         m_hasPendingRefreshState = false;
    RefreshState m_pendingRefreshState    = RefreshState::Fetching;
    QDateTime    m_pendingLastFetch;
    QDateTime    m_pendingNextFetch;
};

#endif // USAGEWINDOW_H
