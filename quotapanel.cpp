#include "quotapanel.h"
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QLinearGradient>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPropertyAnimation>

// ── ThresholdBar ──────────────────────────────────────────────────────────────

ThresholdBar::ThresholdBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(12);

    m_shimmerAnim = new QPropertyAnimation(this, "shimmerPos", this);
    m_shimmerAnim->setStartValue(0.0f);
    m_shimmerAnim->setEndValue(1.0f);
    m_shimmerAnim->setDuration(1200);
    m_shimmerAnim->setLoopCount(-1);
    m_shimmerAnim->setEasingCurve(QEasingCurve::Linear);

    m_fadeAnim = new QPropertyAnimation(this, "shimmerAlpha", this);
    m_fadeAnim->setEndValue(0.0f);
    m_fadeAnim->setDuration(400);
    m_fadeAnim->setEasingCurve(QEasingCurve::Linear);
    connect(m_fadeAnim, &QPropertyAnimation::finished, this, [this]() {
        m_shimmerAnim->stop();
        m_shimmerPos = 0.0f;
        update();
    });
}

void ThresholdBar::setValue(int pct)
{
    m_value = qBound(0, pct, 100);
    update();
}

void ThresholdBar::setActive(bool active)
{
    if (m_active == active) return;
    m_active = active;

    if (active) {
        m_fadeAnim->stop();
        m_shimmerAlpha = 1.0f;
        m_shimmerAnim->setLoopCount(-1);
        if (m_shimmerAnim->state() != QAbstractAnimation::Running)
            m_shimmerAnim->start();
    } else {
        m_fadeAnim->setStartValue(m_shimmerAlpha);
        m_fadeAnim->start();
    }
}

void ThresholdBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRect  r      = rect();
    const double radius = 4.0;

    // 배경
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#e9ecef"));
    p.drawRoundedRect(r, radius, radius);

    // 채움 색상
    QColor fillColor;
    if (m_value < USAGE_WARN_PCT)      fillColor = QColor("#28a745");
    else if (m_value < USAGE_CRIT_PCT) fillColor = QColor("#ffc107");
    else                               fillColor = QColor("#dc3545");

    if (m_value > 0) {
        const int fillW = qRound(r.width() * m_value / 100.0);
        const QRect fillRect(r.left(), r.top(), fillW, r.height());

        // 유리 그라데이션 채움
        QLinearGradient glass(0, r.top(), 0, r.bottom());
        glass.setColorAt(0.0, fillColor.lighter(130));
        glass.setColorAt(0.5, fillColor);
        glass.setColorAt(1.0, fillColor.darker(110));

        p.setPen(Qt::NoPen);
        p.setBrush(glass);
        p.drawRoundedRect(fillRect, radius, radius);

        // shimmer 오버레이
        if (m_shimmerAlpha > 0.0f && m_shimmerPos > 0.0f) {
            const int cx = r.left() + qRound(fillW * m_shimmerPos);
            const int hw = 22;

            QLinearGradient sg(cx - hw, 0, cx + hw, 0);
            const int alpha = qRound(90 * m_shimmerAlpha);
            sg.setColorAt(0.0, QColor(255, 255, 255, 0));
            sg.setColorAt(0.5, QColor(255, 255, 255, alpha));
            sg.setColorAt(1.0, QColor(255, 255, 255, 0));

            p.setClipRect(fillRect);
            p.fillRect(r, sg);
            p.setClipping(false);
        }
    }

}

// ── QuotaPanel ────────────────────────────────────────────────────────────────

QuotaPanel::QuotaPanel(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    setFixedWidth(250);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 8, 14, 8);
    root->setSpacing(4);

    // 제목 + % 한 줄
    auto *row = new QHBoxLayout;
    m_titleLabel = new QLabel(title);
    m_titleLabel->setStyleSheet("font-weight: bold; font-size: 12px;");

    m_pctLabel = new QLabel("--");
    m_pctLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_pctLabel->setStyleSheet("font-weight: bold; font-size: 12px;");

    row->addWidget(m_titleLabel);
    row->addWidget(m_pctLabel);

    m_bar = new ThresholdBar;

    // 리셋 카운트다운
    m_resetLabel = new QLabel("--");
    m_resetLabel->setStyleSheet("color: #888; font-size: 10px;");

    root->addLayout(row);
    root->addWidget(m_bar);
    root->addWidget(m_resetLabel);
}

void QuotaPanel::setData(const QuotaInfo &info)
{
    if (!info.valid) {
        m_pctLabel->setText("--");
        m_bar->setValue(0);
        return;
    }

    int pct = qRound(info.utilization * 100.0);
    pct = qBound(0, pct, 100);

    m_pctLabel->setText(QString("%1%").arg(pct));
    m_bar->setValue(pct);

    if (info.rawTokens > 0) {
        auto fmt = [](qint64 n) -> QString {
            if (n >= 1'000'000) return QString("%1M").arg(n / 1'000'000.0, 0, 'f', 1);
            if (n >= 1'000)     return QString("%1K").arg(n / 1'000.0,     0, 'f', 1);
            return QString::number(n);
        };
        QString detail = fmt(info.rawTokens);
        if (info.limitTokens > 0)
            detail += " / " + fmt(info.limitTokens);
        m_resetLabel->setText(detail);
    }
}

void QuotaPanel::setCountdown(const QString &text)
{
    m_resetLabel->setText(text);
}

void QuotaPanel::setActive(bool active)
{
    m_bar->setActive(active);
}

void QuotaPanel::setCompact(bool compact)
{
    m_titleLabel->setVisible(!compact);
    m_pctLabel->setVisible(!compact);
    m_resetLabel->setVisible(!compact);

    auto *vbox = static_cast<QVBoxLayout *>(layout());
    if (compact)
        vbox->setContentsMargins(6, 3, 6, 3);
    else
        vbox->setContentsMargins(14, 8, 14, 8);
}

