
#include "TrayController.h"
#include "CredentialsReader.h"
#include "SessionLogWatcher.h"
#include "UsageAggregator.h"
#include "UsageApiClient.h"
#include "UsageMerger.h"
#include "UsageWindow.h"
#include <QApplication>
#include <QCursor>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QTimer>
#include <QDesktopServices>
#include <QUrl>

TrayController::TrayController(QObject *parent)
    : QObject(parent)
    , m_tray(new QSystemTrayIcon(this))
    , m_contextMenu(new QMenu)
    , m_popup(new UsageWindow)
    , m_apiClient(new UsageApiClient(this))
    , m_scanner(new SessionLogWatcher(this))
    , m_countdownTimer(new QTimer(this))
{
    // 보정 계수: 디스크에 학습분이 있으면 이어서 쓰고, 없으면 플랜 한도 prior 로
    // 시작한다. prior 는 예전 하드코딩 공식과 동일해서 첫 실행 표시가 변하지 않는다.
    const QString plan = CredentialsReader::subscriptionType();
    m_calibPriors = QuotaCalibrator::priorsFor(UsageAggregator::planLimit5h(plan),
                                               UsageAggregator::planLimit7d(plan));
    // loadFrom 이 priors 를 기준으로 검증해, 현재 천장을 벗어난 창은 알아서
    // 초기값으로 되돌린다(플랜 변경·구버전 과대 계수·손상 모두 여기서 걸린다).
    QuotaCalibrator::loadFrom("calibration", m_calibration, m_calibPriors);
    m_scanner->setCalibration(m_calibration);

    m_contextMenu->addAction("종료", qApp, &QApplication::quit);

    m_tray->setContextMenu(m_contextMenu);
    m_tray->setIcon(makeIcon(0.0));
    m_tray->setToolTip("ClaudeTray - 불러오는 중...");
    m_tray->show();

    connect(m_tray, &QSystemTrayIcon::activated,
            this, &TrayController::onTrayActivated);

    connect(m_apiClient, &UsageApiClient::fetchStarted, this, [this]() {
        m_popup->setRefreshState(UsageWindow::RefreshState::Fetching);
    });
    connect(m_apiClient, &UsageApiClient::usageFetched,
            this, &TrayController::onUsageFetched);
    connect(m_apiClient, &UsageApiClient::fetchFailed,
            this, &TrayController::onFetchFailed);
    connect(m_scanner, &SessionLogWatcher::localUsageUpdated,
            this, &TrayController::onLocalUsage);
    connect(m_scanner, &SessionLogWatcher::activityDetected,
            this, &TrayController::onActivityDetected);
    // activityStopped → 게이지바 shimmer 정지 + 10초 뒤 팝업 반투명화 예약
    connect(m_scanner, &SessionLogWatcher::activityStopped, this, [this]() {
        m_isActive = false;
        m_popup->setIdle();
    });

    // 앱 재시작 후 API 응답 전까지 마지막 resetsAt 로 추정
    QSettings s("ClaudeTray", "ClaudeTray");
    const QDateTime r5h = QDateTime::fromString(s.value("reset5h").toString(), Qt::ISODate);
    const QDateTime r7d = QDateTime::fromString(s.value("reset7d").toString(),  Qt::ISODate);
    if (r5h.isValid() || r7d.isValid())
        m_scanner->setWindowHints(r5h, r7d);

    connect(m_countdownTimer, &QTimer::timeout,
            this, &TrayController::updateCountdowns);
    m_countdownTimer->start(60 * 1000);

    connect(m_apiClient, &UsageApiClient::updateAvailable,
            this, &TrayController::onUpdateAvailable);
    connect(m_tray, &QSystemTrayIcon::messageClicked,
            this, &TrayController::onUpdateNotificationClicked);

    m_apiClient->checkForUpdates();
}

TrayController::~TrayController()
{
    // 트레이 아이콘이 이미 파괴된 메뉴를 가리키지 않도록 먼저 연결을 끊는다.
    m_tray->setContextMenu(nullptr);
    delete m_contextMenu;
    delete m_popup;
}

void TrayController::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Context)
        return;

    if (m_popup->isVisible()) {
        m_popup->hideAndSavePos();
    } else {
        m_popup->showNearTray(QCursor::pos());
    }
}

void TrayController::onUsageFetched(UsageData data)
{
    m_apiFailed = false;
    m_lastFetchError.clear();
    m_resetFetchRequested = false;
    m_current = data;
    m_lastApiData = data;
    m_hasLastApiData = true;
    m_lastSuccessfulApiFetchAt = data.fetchedAt;

    m_scanner->setWindowHints(data.fiveHour.resetsAt, data.sevenDay.resetsAt);
    m_scanner->setDeltaStart(data.fetchedAt);

    // 방금 정답지를 받았다. 같은 윈도우를 로컬로 집계한 특징벡터가 있어야
    // 관측 1건이 완성되므로, 스캔을 요청해 두고 결과가 오면 학습한다.
    m_calibObservationPending = true;
    m_scanner->requestScan();

    // resetsAt 영속 저장 (앱 재시작 후에도 추정에 활용)
    QSettings s("ClaudeTray", "ClaudeTray");
    s.setValue("reset5h", data.fiveHour.resetsAt.toString(Qt::ISODate));
    s.setValue("reset7d",  data.sevenDay.resetsAt.toString(Qt::ISODate));

    applyData(data);
    m_popup->setRefreshState(UsageWindow::RefreshState::Refreshed,
                             m_lastSuccessfulApiFetchAt,
                             m_apiClient->nextScheduledFetchAt());
}

void TrayController::onFetchFailed(QString reason, bool networkError)
{
    m_apiFailed = true;
    m_lastFetchError = reason;

    // 예전에는 여기서 로컬 스캔을 동기 호출했다. 전체 JSONL 을 메인 스레드에서
    // 읽고 파싱하므로 네트워크가 끊긴 동안(30초 재시도 × N) UI 가 반복해서 멈췄다.
    // 백그라운드 스캔을 요청하고, 결과는 onLocalUsage 가 같은 병합 규칙으로 받는다.
    m_scanner->requestScan();

    const auto state = networkError
        ? UsageWindow::RefreshState::NetworkError
        : UsageWindow::RefreshState::LocalFallback;
    m_popup->setRefreshState(state);
}

// full, delta 는 백그라운드 스캔에서 이미 계산된 결과 → 메인 스레드 재스캔 없음
void TrayController::onLocalUsage(ScanResult result)
{
    trainCalibration(result);

    const UsageData merged = (m_hasLastApiData && result.hasDelta)
        ? mergeWithLastApi(result.delta)
        : result.full;
    m_current = merged;
    applyData(merged);

    // API 실패 시에만 로컬 폴백 표시 (성공 후 로컬 증분은 🟢 유지)
    if (m_apiFailed)
        m_popup->setRefreshState(UsageWindow::RefreshState::LocalFallback);

    // 마지막 API resetsAt 가 이미 지났으면 즉시 재호출 → 정확한 새 리셋 시각 수신
    // 단, 이미 요청했으면 중복 호출하지 않는다. 매 스캔마다 fetchUsage 를 부르면
    // 스트리밍 중 초당 수십 건의 요청이 발생해 API 스팸이 된다.
    if (m_hasLastApiData && !m_resetFetchRequested) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (m_lastApiData.fiveHour.resetsAt.isValid() &&
            m_lastApiData.fiveHour.resetsAt.toUTC() <= now) {
            m_resetFetchRequested = true;
            m_apiClient->fetchUsage();
        }
    }
}

void TrayController::applyData(const UsageData &data)
{
    m_popup->setData(data);
    m_popup->setCountdowns(
        formatCountdown(data.fiveHour.resetsAt),
        formatCountdown(data.sevenDay.resetsAt));

    // 5h·7d 중 '더 많이 찬' 쪽을 아이콘에 쓴다. 한눈에 봐야 하는 정보는
    // "얼마나 여유로운가"가 아니라 "얼마나 임박했는가"이기 때문이다.
    //
    // 예전에는 작은 쪽(min)을 썼다. 그러면 주간 한도를 다 써서 추가 크레딧이
    // 나가는 중인데도 5시간 창이 막 리셋돼 20% 면 아이콘이 초록으로 표시돼,
    // 과금 로직(UsageMerger::chargeableRatio 는 max 기준)과 정반대 신호를 줬다.
    const double dominant = qMax(data.fiveHour.utilization,
                                 data.sevenDay.utilization);
    m_tray->setIcon(makeIcon(dominant));
    updateTooltip();
}

UsageData TrayController::mergeWithLastApi(const UsageData &data) const
{
    if (!m_hasLastApiData)
        return data;

    return UsageMerger::mergeWithLastApi(m_lastApiData, data,
                                        QDateTime::currentDateTimeUtc());
}

// API 응답 1건 = 정답지 1장. 같은 시점의 로컬 특징벡터와 짝지어 계수를 당긴다.
// 관측이 쌓일수록 "토큰 1개가 할당량의 몇 %를 먹는가"가 이 사용자의 실제
// 사용 패턴(모델 구성·캐시 비율·플랜)에 맞춰 정확해진다.
void TrayController::trainCalibration(const ScanResult &result)
{
    if (!m_calibObservationPending || !m_hasLastApiData)
        return;
    m_calibObservationPending = false;

    bool learned = false;
    double residual5h = 0.0, residual7d = 0.0;
    auto tryObserve = [&](QuotaCoefficients &coeff, const UsageFeatures &features,
                          const QuotaInfo &truth, const QuotaCoefficients &prior,
                          double *residualOut) {
        if (!truth.valid)
            return;
        if (QuotaCalibrator::observe(coeff, features, truth.utilization, prior,
                                     residualOut))
            learned = true;
    };

    tryObserve(m_calibration.fiveHour, result.full5hFeatures,
               m_lastApiData.fiveHour, m_calibPriors.fiveHour, &residual5h);
    tryObserve(m_calibration.sevenDay, result.full7dFeatures,
               m_lastApiData.sevenDay, m_calibPriors.sevenDay, &residual7d);
    tryObserve(m_calibration.sevenDaySonnet, result.full7dSonnetFeatures,
               m_lastApiData.sevenDaySonnet, m_calibPriors.sevenDaySonnet, nullptr);

    if (!learned)
        return;

    m_scanner->setCalibration(m_calibration);
    QuotaCalibrator::saveTo("calibration", m_calibration);

    // 잔차(양수 = 로컬 로그로 설명되지 않는 사용량)를 남긴다. 이 값이 꾸준히
    // 양수라면 claude.ai 등 외부 표면에서 할당량을 쓰고 있다는 뜻이고,
    // 나중에 '외란 vs 실제 한도 변경' 판별 임계값을 정할 때의 근거가 된다.
    qDebug() << "[TrayController] 보정 samples 5h=" << m_calibration.fiveHour.samples
             << "7d=" << m_calibration.sevenDay.samples
             << "7dSonnet=" << m_calibration.sevenDaySonnet.samples
             << "| 잔차 5h=" << QString::number(residual5h * 100.0, 'f', 3) + "%p"
             << "7d=" << QString::number(residual7d * 100.0, 'f', 3) + "%p";
}

void TrayController::updateCountdowns()
{
    if (!m_current.fetchedAt.isValid())
        return;

    m_popup->setCountdowns(
        formatCountdown(m_current.fiveHour.resetsAt),
        formatCountdown(m_current.sevenDay.resetsAt));
    m_popup->refreshNextFetch(m_apiClient->nextScheduledFetchAt());
}

void TrayController::updateTooltip()
{
    auto pct = [](const QuotaInfo &quota) -> QString {
        return quota.valid ? QString("%1%").arg(qRound(quota.utilization * 100.0)) : "--";
    };

    QString tip = QString("Claude Code Usage\n5h: %1  |  7d: %2")
                      .arg(pct(m_current.fiveHour))
                      .arg(pct(m_current.sevenDay));

    if (m_current.extraUsage.enabled) {
        tip += QString("\n추가 크레딧: $%1 / $%2 (%3%)")
                   .arg(m_current.extraUsage.usedCredits, 0, 'f', 2)
                   .arg(m_current.extraUsage.limitDollars, 0, 'f', 2)
                   .arg(qRound(m_current.extraUsage.utilization * 100.0));
    }

    if (!m_current.recentModel.isEmpty()) {
        tip += QString("\n최근 모델: %1").arg(m_current.recentModel);
    }

    tip += "\n" + formatCountdown(m_current.fiveHour.resetsAt);
    m_tray->setToolTip(tip);
}

QIcon TrayController::makeIcon(double utilization)
{
    // tools/gen_icon.py 가 만드는 appicon.ico 와 동일한 도형이다.
    // 한쪽 수치를 바꾸면 다른 쪽도 반드시 맞출 것.
    constexpr double UNIT        = 64.0;   // 논리 좌표계 한 변
    constexpr double TILE_RADIUS = 15.0;
    constexpr double RING_INSET  = 12.0;
    constexpr double RING_WIDTH  = 9.5;
    constexpr int    SCALE       = 4;      // 논리 단위당 물리 픽셀

    QPixmap px(int(UNIT) * SCALE, int(UNIT) * SCALE);
    px.fill(Qt::transparent);
    // DPR 을 설정하면 이후 좌표는 논리 단위(0..64)로 다룰 수 있다.
    px.setDevicePixelRatio(SCALE);

    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing, true);

    // ── 타일 ──────────────────────────────────────────
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x26, 0x26, 0x2D));
    p.drawRoundedRect(QRectF(0, 0, UNIT, UNIT), TILE_RADIUS, TILE_RADIUS);

    const QRectF ringRect(RING_INSET, RING_INSET,
                          UNIT - 2 * RING_INSET, UNIT - 2 * RING_INSET);

    // ── 빈 구간(트랙) ─────────────────────────────────
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0x55, 0x55, 0x5A), RING_WIDTH));
    p.drawEllipse(ringRect);

    // ── 사용량 구간 ───────────────────────────────────
    // 색은 quotapanel 과 동일한 팔레트를 쓴다.
    const double v = qBound(0.0, utilization, 1.0);
    if (v > 0.0) {
        QColor c;
        if (utilization < USAGE_WARN_PCT / 100.0)
            c = QColor("#28a745");
        else if (utilization < USAGE_CRIT_PCT / 100.0)
            c = QColor("#ffc107");
        else
            c = QColor("#dc3545");

        QPen arcPen(c, RING_WIDTH);
        arcPen.setCapStyle(Qt::RoundCap);
        p.setPen(arcPen);
        // QPainter 각도 단위는 1/16도, 3시가 0도, 반시계가 양수.
        // 12시(90도)에서 시계방향으로 진행하므로 span 은 음수다.
        p.drawArc(ringRect, 90 * 16, -qRound(v * 360.0) * 16);
    }

    p.end();
    return QIcon(px);
}

QString TrayController::formatCountdown(const QDateTime &resetsAt) const
{
    if (!resetsAt.isValid())
        return "초기화 시각 정보 없음";

    const qint64 secs = QDateTime::currentDateTimeUtc().secsTo(resetsAt.toUTC());
    if (secs <= 0)
        return "곧 초기화됩니다";

    const qint64 days  = secs / 86400;
    const qint64 hours = (secs % 86400) / 3600;
    const qint64 mins  = (secs % 3600) / 60;

    const QDateTime local = resetsAt.toLocalTime();
    const QString clock   = local.toString("HH:mm");

    if (days > 0) {
        const QString fullClock = QLocale::system().toString(local, "M/d ddd HH:mm");
        return QString("%1d %2h 후 초기화 (%3)").arg(days).arg(hours).arg(fullClock);
    }
    if (hours > 0)
        return QString("%1h %2m 후 초기화 (%3)").arg(hours).arg(mins).arg(clock);
    return QString("%1m 후 초기화 (%2)").arg(mins).arg(clock);
}


void TrayController::onActivityDetected()
{
    m_isActive = true;
    m_popup->setActive();
}

void TrayController::onUpdateAvailable(const QString &latestVersion, const QString &downloadUrl)
{
    m_updateUrl = downloadUrl;
    m_tray->showMessage(
        "ClaudeTray 업데이트 가능",
        QString("새로운 버전 %1이 출시되었습니다. 클릭하여 다운로드 페이지로 이동합니다.").arg(latestVersion),
        QSystemTrayIcon::Information,
        10000
    );
}

void TrayController::onUpdateNotificationClicked()
{
    if (!m_updateUrl.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_updateUrl));
    }
}

