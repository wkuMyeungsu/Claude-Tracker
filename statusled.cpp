#include "statusled.h"
#include <QEasingCurve>
#include <QPainter>
#include <QPropertyAnimation>
#include <QTimer>
#include <cmath>

static const QColor COLOR_ACTIVE_BRIGHT(52,  199, 89);   // #34C759 iOS 그린 (밝음)
static const QColor COLOR_ACTIVE_DIM   (20,  100, 40);   // 어두운 초록 (breathing 저점)
static const QColor COLOR_IDLE         (160, 160, 160);  // 밝은 회색 (다크 배경에서 선명하게 보임)

StatusLed::StatusLed(QWidget *parent)
    : QWidget(parent)
    , m_color(COLOR_IDLE)
    , m_pulseTimer(new QTimer(this))
    , m_fadeAnim(new QPropertyAnimation(this, "ledColor", this))
{
    setFixedSize(10, 10);

    m_pulseTimer->setInterval(33);   // 30 fps
    connect(m_pulseTimer, &QTimer::timeout, this, &StatusLed::onPulseTick);

    m_fadeAnim->setDuration(10'000); // 10초
    m_fadeAnim->setEasingCurve(QEasingCurve::OutQuad);
}

void StatusLed::setLedColor(const QColor &c)
{
    m_color = c;
    update();
}

void StatusLed::setActive()
{
    m_fadeAnim->stop();
    m_pulsePhase = 0.0;
    if (!m_pulseTimer->isActive())
        m_pulseTimer->start();
}

void StatusLed::setIdle()
{
    m_pulseTimer->stop();
    m_fadeAnim->setStartValue(m_color);   // 현재 위치에서부터 페이드
    m_fadeAnim->setEndValue(COLOR_IDLE);
    m_fadeAnim->start();
}

void StatusLed::onPulseTick()
{
    m_pulsePhase += PULSE_STEP;
    const double t = (std::sin(m_pulsePhase) + 1.0) / 2.0;  // 0.0 ~ 1.0

    m_color = QColor(
        COLOR_ACTIVE_DIM.red()   + qRound(t * (COLOR_ACTIVE_BRIGHT.red()   - COLOR_ACTIVE_DIM.red())),
        COLOR_ACTIVE_DIM.green() + qRound(t * (COLOR_ACTIVE_BRIGHT.green() - COLOR_ACTIVE_DIM.green())),
        COLOR_ACTIVE_DIM.blue()  + qRound(t * (COLOR_ACTIVE_BRIGHT.blue()  - COLOR_ACTIVE_DIM.blue()))
    );
    update();
}

void StatusLed::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(m_color);
    // 1px 여백: 안티앨리어싱이 테두리에서 잘리지 않도록
    p.drawEllipse(rect().adjusted(1, 1, -1, -1));
}
