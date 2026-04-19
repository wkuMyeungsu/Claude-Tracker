#include "toggleswitch.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

static constexpr int   TRACK_H   = 10;
static constexpr int   KNOB_D    = 14;
static constexpr int   ANIM_MS   = 150;
static const QColor    CLR_ON    { 0x4A, 0x9E, 0xFF };  // 파란색 계열
static const QColor    CLR_OFF   { 0xC0, 0xC0, 0xC0 };

ToggleSwitch::ToggleSwitch(QWidget *parent)
    : QWidget(parent)
    , m_anim(new QPropertyAnimation(this, "knobPos", this))
{
    m_anim->setDuration(ANIM_MS);
    m_anim->setEasingCurve(QEasingCurve::InOutQuad);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
}

void ToggleSwitch::setChecked(bool checked)
{
    if (m_checked == checked)
        return;
    m_checked = checked;
    m_anim->stop();
    m_anim->setStartValue(m_knobPos);
    m_anim->setEndValue(checked ? 1.0 : 0.0);
    m_anim->start();
}

void ToggleSwitch::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_checked = !m_checked;
        m_anim->stop();
        m_anim->setStartValue(m_knobPos);
        m_anim->setEndValue(m_checked ? 1.0 : 0.0);
        m_anim->start();
        emit toggled(m_checked);
    }
    QWidget::mousePressEvent(event);
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    const int w = width();
    const int h = height();

    // 트랙 (pill 형태)
    const int trackY = (h - TRACK_H) / 2;
    const QColor trackColor = m_checked
        ? CLR_ON.lighter(100 + static_cast<int>(m_knobPos * 0))  // 애니메이션 중 색 보간
        : CLR_OFF;

    // knobPos 기반 색상 보간 (0.0=off색, 1.0=on색)
    QColor blended;
    blended.setRedF  (CLR_OFF.redF()   + m_knobPos * (CLR_ON.redF()   - CLR_OFF.redF()));
    blended.setGreenF(CLR_OFF.greenF() + m_knobPos * (CLR_ON.greenF() - CLR_OFF.greenF()));
    blended.setBlueF (CLR_OFF.blueF()  + m_knobPos * (CLR_ON.blueF()  - CLR_OFF.blueF()));
    (void)trackColor;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // 트랙
    QPainterPath track;
    track.addRoundedRect(0, trackY, w, TRACK_H, TRACK_H / 2.0, TRACK_H / 2.0);
    p.fillPath(track, blended);

    // 노브 (흰 원)
    const int knobTravel = w - KNOB_D;
    const int knobX      = static_cast<int>(m_knobPos * knobTravel);
    const int knobY      = (h - KNOB_D) / 2;

    p.setPen(QPen(QColor(0, 0, 0, 30), 1));
    p.setBrush(Qt::white);
    p.drawEllipse(knobX, knobY, KNOB_D, KNOB_D);
}
