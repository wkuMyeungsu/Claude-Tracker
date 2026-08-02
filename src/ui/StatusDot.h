#ifndef STATUSDOT_H
#define STATUSDOT_H

#include <QWidget>

class QPropertyAnimation;

// iOS 상태 표시등 느낌의 작은 원형 점. 이모지를 쓰면 폰트에 따라 크기·기준선이
// 제각각이라 헤더 정렬이 흔들려서 QPainter 로 직접 그린다.
class StatusDot : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal pulse READ pulse WRITE setPulse)
public:
    enum class Mode { Idle, Running, PendingApproval };

    explicit StatusDot(QWidget *parent = nullptr);

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    qreal pulse() const      { return m_pulse; }
    void  setPulse(qreal v)  { m_pulse = v; update(); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Mode                m_mode  = Mode::Idle;
    qreal               m_pulse = 1.0;   // 1.0 = 가장 밝은 순간
    QPropertyAnimation *m_anim  = nullptr;
};

#endif // STATUSDOT_H
