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
    void onLocalUsage(UsageData full, UsageData delta, bool hasDelta);
    void updateCountdowns();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onActivityDetected();

private:
    void applyData(const UsageData &data);
    UsageData mergeWithLastApi(const UsageData &data) const;
    void updateTooltip();
    QIcon makeIcon(double utilization);
    QString formatCountdown(const QDateTime &resetsAt) const;

    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_contextMenu = nullptr;
    UsagePopup *m_popup = nullptr;
    UsageApiClient *m_apiClient = nullptr;
    UsageScanner *m_scanner = nullptr;
    UsageData m_current;
    UsageData m_lastApiData;
    QTimer *m_countdownTimer  = nullptr;
    QDateTime m_lastSuccessfulApiFetchAt;
    QString m_lastFetchError;
    bool m_apiFailed      = false;
    bool m_hasLastApiData = false;
    bool m_isActive       = false;  // 토큰 사용 중 여부
};

#endif // TRAYAPP_H
