#include "quotapanel.h"
#include <QLabel>
#include <QProgressBar>
#include <QHBoxLayout>
#include <QVBoxLayout>

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

    // 프로그레스 바
    m_bar = new QProgressBar;
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(12);
    m_bar->setStyleSheet(barStyle(0));

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
        m_bar->setStyleSheet(barStyle(0));
        return;
    }

    int pct = qRound(info.utilization * 100.0);
    pct = qBound(0, pct, 100);

    m_pctLabel->setText(QString("%1%").arg(pct));
    m_bar->setValue(pct);
    m_bar->setStyleSheet(barStyle(pct));

    // 실제 토큰 수 표시 (로컬 추정 모드에서 투명도 제공)
    if (info.rawTokens > 0) {
        auto fmt = [](qint64 n) -> QString {
            if (n >= 1'000'000) return QString("%1M").arg(n / 1'000'000.0, 0, 'f', 1);
            if (n >= 1'000)     return QString("%1K").arg(n / 1'000.0,     0, 'f', 1);
            return QString::number(n);
        };
        QString detail = fmt(info.rawTokens);
        if (info.limitTokens > 0)
            detail += " / " + fmt(info.limitTokens);
        m_resetLabel->setText(detail);  // 카운트다운 업데이트 전 기본값
    }
}

void QuotaPanel::setCountdown(const QString &text)
{
    m_resetLabel->setText(text);
}

QString QuotaPanel::barStyle(int pct)
{
    QString color;
    if (pct <= 70)      color = "#28a745"; // 녹색 (0~70%)
    else if (pct <= 85) color = "#ffc107"; // 노란색 (71~85%)
    else                color = "#dc3545"; // 빨간색 (86~100%)

    return QString(R"(
        QProgressBar {
            border: 1px solid #d0d0d0;
            border-radius: 4px;
            background: #e9ecef;
        }
        QProgressBar::chunk {
            border-radius: 4px;
            background: %1;
        }
    )").arg(color);
}
