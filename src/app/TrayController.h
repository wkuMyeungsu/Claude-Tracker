#ifndef TRAYCONTROLLER_H
#define TRAYCONTROLLER_H

#include <QObject>
#include <QSystemTrayIcon>
#include "UsageTypes.h"
#include "SessionLogWatcher.h"

class QMenu;
class QTimer;
class UsageApiClient;
class UsageWindow;

class TrayController : public QObject
{
    Q_OBJECT
public:
    explicit TrayController(QObject *parent = nullptr);
    // m_contextMenu 와 m_popup 은 최상위 QWidget 이라 QObject 인 TrayController 을
    // 부모로 삼을 수 없다. QSystemTrayIcon::setContextMenu 도 소유권을 가져가지
    // 않으므로 여기서 직접 정리해야 한다.
    ~TrayController() override;

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
    UsageWindow *m_popup = nullptr;
    UsageApiClient *m_apiClient = nullptr;
    SessionLogWatcher *m_scanner = nullptr;
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

#endif // TRAYCONTROLLER_H
