#include "StatusDot.h"
#include <QPainter>
#include <QPropertyAnimation>

namespace {
constexpr qreal kCoreRadius = 4.5;   // 점 반지름 → 지름 9px
constexpr qreal kHaloExtra  = 3.0;   // 펄스가 가장 밝을 때 번지는 폭

QColor colorFor(StatusDot::Mode m)
{
    switch (m) {
    case StatusDot::Mode::Running:         return QColor("#0a84ff");
    case StatusDot::Mode::PendingApproval: return QColor("#ffd60a");
    case StatusDot::Mode::Idle:
    default:                               return QColor("#34c759");
    }
}

bool pulsesIn(StatusDot::Mode m)
{
    return m != StatusDot::Mode::Idle;
}
} // namespace

StatusDot::StatusDot(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(16, 16);
    setAttribute(Qt::WA_TransparentForMouseEvents);   // 헤더 드래그를 막지 않는다

    // 숨쉬듯 밝아졌다 어두워지는 왕복. QPropertyAnimation 은 되감기를 하지
    // 않으므로 키프레임으로 한 주기를 만들고 그걸 반복한다.
    m_anim = new QPropertyAnimation(this, "pulse", this);
    m_anim->setDuration(1600);
    m_anim->setKeyValueAt(0.0, 1.0);
    m_anim->setKeyValueAt(0.5, 0.30);
    m_anim->setKeyValueAt(1.0, 1.0);
    m_anim->setEasingCurve(QEasingCurve::InOutSine);
    m_anim->setLoopCount(-1);
}

void StatusDot::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;

    if (pulsesIn(mode)) {
        if (m_anim->state() != QAbstractAnimation::Running)
            m_anim->start();
    } else {
        m_anim->stop();
        m_pulse = 1.0;   // 대화 가능은 깜빡이지 않고 가만히 켜져 있다
    }
    update();
}

void StatusDot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);

    const QColor  base = colorFor(m_mode);
    const QPointF ctr  = QRectF(rect()).center();

    if (pulsesIn(m_mode)) {
        QColor halo = base;
        halo.setAlphaF(0.30 * m_pulse);
        p.setBrush(halo);
        const qreal r = kCoreRadius + kHaloExtra * m_pulse;
        p.drawEllipse(ctr, r, r);
    }

    QColor core = base;
    if (pulsesIn(m_mode))
        core.setAlphaF(0.55 + 0.45 * m_pulse);
    p.setBrush(core);
    p.drawEllipse(ctr, kCoreRadius, kCoreRadius);
}
