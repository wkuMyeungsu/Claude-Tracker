#include "quotapanel.h"
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QHBoxLayout>
#include <QVBoxLayout>

// ── ThresholdBar ──────────────────────────────────────────────────────────────

ThresholdBar::ThresholdBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(12);
}

void ThresholdBar::setValue(int pct)
{
    m_value = qBound(0, pct, 100);
    update();
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

    // 채움 색상 (임계값 기준)
    QColor fillColor;
    if (m_value < USAGE_WARN_PCT)      fillColor = QColor("#28a745");
    else if (m_value < USAGE_CRIT_PCT) fillColor = QColor("#ffc107");
    else                               fillColor = QColor("#dc3545");

    // 채움 바
    if (m_value > 0) {
        const int fillW = qRound(r.width() * m_value / 100.0);
        p.setBrush(fillColor);
        p.drawRoundedRect(QRect(r.left(), r.top(), fillW, r.height()), radius, radius);
    }

    // 임계 실선 (희미하게)
    p.setRenderHint(QPainter::Antialiasing, false);

    struct Threshold { int pct; QColor color; };
    static const Threshold thresholds[] = {
        { USAGE_WARN_PCT, QColor(230, 126, 34, 140) },   // 주황
        { USAGE_CRIT_PCT, QColor(192, 57,  43, 140) },   // 빨강
    };

    for (const auto &t : thresholds) {
        const int x = qRound(r.width() * t.pct / 100.0);
        p.setPen(QPen(t.color, 1));
        p.drawLine(x, r.top(), x, r.bottom());
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
