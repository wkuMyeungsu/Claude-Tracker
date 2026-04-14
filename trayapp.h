#ifndef TRAYAPP_H
#define TRAYAPP_H

#include <QObject>
#include <QSystemTrayIcon>
#include "usagedata.h"

class QMenu;
class QTimer;
class UsageApiClient;
class UsageScanner;
class UsagePopup;

class TrayApp : public QObject
{
    Q_OBJECT
public:
    explicit TrayApp(QObject *parent = nullptr);

private slots:
    void onUsageFetched(UsageData data);
    void onFetchFailed(QString reason);
    void onLocalUsage(UsageData data);
    void updateCountdowns();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

private:
    void applyData(const UsageData &data);
    UsageData mergeWithLastApi(const UsageData &data) const;
    void updateTooltip();
    QIcon makeIcon(double utilization);
    QString formatCountdown(const QDateTime &resetsAt) const;
    QString formatClockTime(const QDateTime &timestamp) const;
    QString buildTimingText() const;

    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_contextMenu = nullptr;
    UsagePopup *m_popup = nullptr;
    UsageApiClient *m_apiClient = nullptr;
    UsageScanner *m_scanner = nullptr;
    UsageData m_current;
    UsageData m_lastApiData;
    QTimer *m_countdownTimer = nullptr;
    QDateTime m_lastSuccessfulApiFetchAt;
    QString m_lastFetchError;
    bool m_apiFailed = false;
    bool m_hasLastApiData = false;
};

#endif // TRAYAPP_H
