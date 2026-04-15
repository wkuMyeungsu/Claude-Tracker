#ifndef STATUSLED_H
#define STATUSLED_H

#include <QColor>
#include <QWidget>

class QPropertyAnimation;
class QTimer;

// iOS 스타일 상태 LED
// - 활성: #34C759(iOS 그린) breathing 펄스
// - 종료: 현재 색 → #48484A(iOS 회색) 30초 페이드
class StatusLed : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(QColor ledColor READ ledColor WRITE setLedColor)

public:
    explicit StatusLed(QWidget *parent = nullptr);

    QColor ledColor() const { return m_color; }
    void   setLedColor(const QColor &c);

    void setActive();  // 펄스 시작
    void setIdle();    // 30초 페이드 → 회색

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void onPulseTick();

private:
    // ~2초 주기 breathing (33ms 간격 = 30fps, 2π/60 ≈ 0.105 rad/frame)
    static constexpr double PULSE_STEP = 0.105;

    QColor              m_color;
    double              m_pulsePhase = 0.0;
    QTimer             *m_pulseTimer;
    QPropertyAnimation *m_fadeAnim;
};

#endif // STATUSLED_H
