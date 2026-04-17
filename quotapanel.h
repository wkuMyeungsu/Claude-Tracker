#ifndef QUOTAPANEL_H
#define QUOTAPANEL_H

#include <QWidget>
#include "usagedata.h"

class QLabel;

// 임계선이 포함된 커스텀 프로그레스 바
class ThresholdBar : public QWidget
{
    Q_OBJECT
public:
    explicit ThresholdBar(QWidget *parent = nullptr);
    void setValue(int pct);   // 0~100

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int m_value = 0;
};

class QuotaPanel : public QWidget
{
    Q_OBJECT
public:
    explicit QuotaPanel(const QString &title, QWidget *parent = nullptr);

    void setData(const QuotaInfo &info);
    void setCountdown(const QString &text);
    void setCompact(bool compact);

private:
    QLabel       *m_titleLabel;
    QLabel       *m_pctLabel;
    ThresholdBar *m_bar;
    QLabel       *m_resetLabel;
};

#endif // QUOTAPANEL_H
