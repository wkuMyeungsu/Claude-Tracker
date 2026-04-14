#ifndef USAGEPOPUP_H
#define USAGEPOPUP_H

#include <QWidget>
#include "usagedata.h"

class QLabel;
class QuotaPanel;
class QWidget;

class UsagePopup : public QWidget
{
    Q_OBJECT
public:
    explicit UsagePopup(QWidget *parent = nullptr);

    void setData(const UsageData &data);
    void setCountdowns(const QString &c5h, const QString &c7d);
    void setStatus(const QString &text);
    void setTimingText(const QString &text);
    void showNearTray(const QPoint &trayPos);

signals:
    void quitRequested();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QuotaPanel *m_panel5h = nullptr;
    QuotaPanel *m_panel7d = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_timingLabel = nullptr;
    QWidget *m_titleBar = nullptr;
    QPoint m_dragPos;
};

#endif // USAGEPOPUP_H
