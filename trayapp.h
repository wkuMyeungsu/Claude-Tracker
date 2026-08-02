#ifndef TRAYAPP_H
#define TRAYAPP_H

#include <QObject>
#include <QSystemTrayIcon>
#include "usagedata.h"
#include "usagescanner.h"

class QMenu;
class QTimer;
class UsageApiClient;
class UsagePopup;

class TrayApp : public QObject
{
    Q_OBJECT
public:
    explicit TrayApp(QObject *parent = nullptr);

private slots:
    void onUsageFetched(UsageData data);
    void onFetchFailed(QString reason, bool networkError);
    void onLocalUsage(ScanResult result);
    void updateCountdowns();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onActivityDetected();
    void onUpdateAvailable(const QString &latestVersion, const QString &downloadUrl);
    void onUpdateNotificationClicked();

private:
    void applyData(const UsageData &data);
    UsageData mergeWithLastApi(const UsageData &data) const;
    // 직전 API 정확값과 방금 계산한 로컬 특징벡터를 짝지어 보정 계수를 학습한다.
    void trainCalibration(const ScanResult &result);
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
    CalibrationSet m_calibration;   // 학습된 "토큰당 할당량" 계수
    CalibrationSet m_calibPriors;   // 계수 상한을 잡기 위한 기준값
    // API 응답이 막 도착해 아직 학습에 못 쓴 상태인가. 다음 로컬 스캔 결과가
    // 오면 그때의 특징벡터와 짝지어 관측 1건으로 소비한다.
    bool m_calibObservationPending = false;
    QTimer *m_countdownTimer  = nullptr;
    QDateTime m_lastSuccessfulApiFetchAt;
    QString m_lastFetchError;
    bool m_apiFailed      = false;
    bool m_hasLastApiData = false;
    bool m_isActive       = false;  // 토큰 사용 중 여부\r
    bool m_resetFetchRequested = false;  // 리셋 감지 후 API 재호출 요청 여부
    QString m_updateUrl;
};

#endif // TRAYAPP_H
