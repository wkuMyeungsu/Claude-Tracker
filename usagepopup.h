#ifndef USAGEPOPUP_H
#define USAGEPOPUP_H

#include <QWidget>
#include "usagedata.h"

class QLabel;
class QPropertyAnimation;
class QuotaPanel;
class QWidget;
class QPushButton;

class UsagePopup : public QWidget
{
    Q_OBJECT
public:
    explicit UsagePopup(QWidget *parent = nullptr);

    void setData(const UsageData &data);
    void setCountdowns(const QString &c5h, const QString &c7d);
    void setStatus(const QString &text);
    void setTimingText(const QString &text);
    void showNearTray(const QPoint &trayPos);

    void setActive();   // 불투명(1.0)으로 페이드
    void setIdle();     // 반투명(0.6)으로 페이드 (visible 일 때만)

signals:
    void quitRequested();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void animateOpacityTo(double target);  // 투명도만 변경
    void applyDataInternal(const UsageData &data);
    void applyCountdownsInternal(const QString &c5h, const QString &c7d);
    void applyPending();    // 드래그 종료 시 밀린 업데이트 일괄 반영

    QuotaPanel *m_panel5h        = nullptr;
    QuotaPanel *m_panel7d        = nullptr;
    QLabel     *m_statusLabel    = nullptr;
    QLabel     *m_timingLabel    = nullptr;
    QWidget    *m_titleBar       = nullptr;
    QLabel     *m_activityPill   = nullptr;  // 타이틀 바 내 토큰 발생 pill
    QPushButton *m_pinBtn        = nullptr;
    QPoint      m_dragPos;
    QPoint      m_rememberedPos;
    bool        m_hasRememberedPos = false;  // 최소화로 닫혔을 때 위치 저장

    QPropertyAnimation *m_opacityAnim      = nullptr;
    bool                m_idleMode          = true;   // 시작 시 idle (활동 감지 전까지)
    bool                m_opacityAtIdle     = true;   // 시작 시 투명 상태 (활동 감지 전까지)
    bool                m_wasOpacityIdleBeforeDrag = false; // 드래그 직전 opacity idle 상태

    // 드래그 중 UI 업데이트 억제
    bool     m_isDragging       = false;
    bool     m_hasPendingData   = false;
    UsageData m_pendingData;
    bool     m_hasPendingCD     = false;   // countdowns pending
    QString  m_pendingC5h;
    QString  m_pendingC7d;
    bool     m_hasPendingStatus = false;
    QString  m_pendingStatus;
    bool     m_hasPendingTiming = false;
    QString  m_pendingTiming;
};

#endif // USAGEPOPUP_H
