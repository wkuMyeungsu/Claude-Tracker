#ifndef QUOTAPANEL_H
#define QUOTAPANEL_H

#include <QWidget>
#include "usagedata.h"

class QLabel;
class QProgressBar;

// QMenu 안에 QWidgetAction으로 삽입되는 사용량 패널
class QuotaPanel : public QWidget
{
    Q_OBJECT
public:
    explicit QuotaPanel(const QString &title, QWidget *parent = nullptr);

    void setData(const QuotaInfo &info);
    void setCountdown(const QString &text);

private:
    static QString barStyle(int pct);

    QLabel       *m_titleLabel;
    QLabel       *m_pctLabel;
    QProgressBar *m_bar;
    QLabel       *m_resetLabel;
};

#endif // QUOTAPANEL_H
