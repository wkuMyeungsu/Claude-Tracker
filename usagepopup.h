#ifndef USAGEPOPUP_H
#define USAGEPOPUP_H

#include <QWidget>
#include <QDateTime>
#include "usagedata.h"

class ToggleSwitch;
class QFrame;
class QLabel;
class QPropertyAnimation;
class QGraphicsOpacityEffect;
class QuotaPanel;
class QWidget;
class QPushButton;
class QTimer;

class UsagePopup : public QWidget
{
    Q_OBJECT
public:
    enum class RefreshState { Fetching, Refreshed, LocalFallback, NetworkError };

    explicit UsagePopup(QWidget *parent = nullptr);

    void setData(const UsageData &data);
    void setCountdowns(const QString &c5h, const QString &c7d);
    void setRefreshState(RefreshState state, QDateTime lastFetch = {}, QDateTime nextFetch = {});
    void refreshNextFetch(QDateTime nextFetch);   // 상태 변경 없이 "다음 Xm 후" 갱신
    void showNearTray(const QPoint &trayPos);
    void hideAndSavePos();

    void setActive();
    void setIdle();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void animateOpacityTo(double target);
    void applyDataInternal(const UsageData &data);
    void applyCountdownsInternal(const QString &c5h, const QString &c7d);
    void updateStatusLine();
    void applyPending();

    void toggleCompact();

    QuotaPanel  *m_panel5h          = nullptr;
    QuotaPanel  *m_panel7d          = nullptr;
    QLabel      *m_statusLine       = nullptr;
    QWidget     *m_titleBar         = nullptr;
    QWidget     *m_collapsingBody   = nullptr;
    QFrame      *m_sep1             = nullptr;
    QFrame      *m_sep2             = nullptr;
    QFrame      *m_sep3             = nullptr;
    QWidget     *m_footer           = nullptr;
    QPushButton *m_pinBtn           = nullptr;
    QPushButton *m_gearBtn          = nullptr;
    QWidget     *m_settingsPanel    = nullptr;
    ToggleSwitch *m_autoFadeCheck   = nullptr;
    bool         m_autoFade         = true;
    QPoint      m_dragPos;
    QPoint      m_rememberedPos;
    bool        m_hasRememberedPos = false;

    QPropertyAnimation *m_opacityAnim             = nullptr;
    bool                m_idleMode                 = true;
    bool                m_opacityAtIdle            = true;
    bool                m_wasOpacityIdleBeforeDrag = false;

    // 갱신 상태
    RefreshState m_refreshState  = RefreshState::Fetching;
    QDateTime    m_lastFetch;
    QDateTime    m_nextFetch;
    QTimer      *m_justRefreshedTimer = nullptr;  // 1분 후 방금→시간 전환
    QTimer      *m_fadeTimer          = nullptr;  // setIdle 후 10s 투명화 딜레이

    // 드래그 중 UI 업데이트 억제
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

#endif // USAGEPOPUP_H
