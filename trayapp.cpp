
#include "trayapp.h"
#include "usageapiclient.h"
#include "usagepopup.h"
#include "usagescanner.h"
#include <QApplication>
#include <QCursor>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QTimer>
#include <QDesktopServices>
#include <QUrl>

TrayApp::TrayApp(QObject *parent)
    : QObject(parent)
    , m_tray(new QSystemTrayIcon(this))
    , m_contextMenu(new QMenu)
    , m_popup(new UsagePopup)
    , m_apiClient(new UsageApiClient(this))
    , m_scanner(new UsageScanner(this))
    , m_countdownTimer(new QTimer(this))
{
    m_contextMenu->addAction("종료", qApp, &QApplication::quit);

    m_tray->setContextMenu(m_contextMenu);
    m_tray->setIcon(makeIcon(0.0));
    m_tray->setToolTip("ClaudeTray - 불러오는 중...");
    m_tray->show();

    connect(m_tray, &QSystemTrayIcon::activated,
            this, &TrayApp::onTrayActivated);

    connect(m_apiClient, &UsageApiClient::fetchStarted, this, [this]() {
        m_popup->setRefreshState(UsagePopup::RefreshState::Fetching);
    });
    connect(m_apiClient, &UsageApiClient::usageFetched,
            this, &TrayApp::onUsageFetched);
    connect(m_apiClient, &UsageApiClient::fetchFailed,
            this, &TrayApp::onFetchFailed);
    connect(m_scanner, &UsageScanner::localUsageUpdated,
            this, &TrayApp::onLocalUsage);
    connect(m_scanner, &UsageScanner::activityDetected,
            this, &TrayApp::onActivityDetected);
    // activityStopped → 즉시 LED 페이드 시작 (30초 후 회색 되면 투명도 적용)
    connect(m_scanner, &UsageScanner::activityStopped, this, [this]() {
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
            this, &TrayApp::updateCountdowns);
    m_countdownTimer->start(60 * 1000);

    connect(m_apiClient, &UsageApiClient::updateAvailable,
            this, &TrayApp::onUpdateAvailable);
    connect(m_tray, &QSystemTrayIcon::messageClicked,
            this, &TrayApp::onUpdateNotificationClicked);

    m_apiClient->checkForUpdates();
}

void TrayApp::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Context)
        return;

    if (m_popup->isVisible()) {
        m_popup->hideAndSavePos();
    } else {
        m_popup->showNearTray(QCursor::pos());
    }
}

void TrayApp::onUsageFetched(UsageData data)
{
    m_apiFailed = false;
    m_lastFetchError.clear();
    m_current = data;
    m_lastApiData = data;
    m_hasLastApiData = true;
    m_lastSuccessfulApiFetchAt = data.fetchedAt;

    m_scanner->setWindowHints(data.fiveHour.resetsAt, data.sevenDay.resetsAt);
    m_scanner->setDeltaStart(data.fetchedAt);

    // resetsAt 영속 저장 (앱 재시작 후에도 추정에 활용)
    QSettings s("ClaudeTray", "ClaudeTray");
    s.setValue("reset5h", data.fiveHour.resetsAt.toString(Qt::ISODate));
    s.setValue("reset7d",  data.sevenDay.resetsAt.toString(Qt::ISODate));

    applyData(data);
    m_popup->setRefreshState(UsagePopup::RefreshState::Refreshed,
                             m_lastSuccessfulApiFetchAt,
                             m_apiClient->nextScheduledFetchAt());
}

void TrayApp::onFetchFailed(QString reason, bool networkError)
{
    m_apiFailed = true;
    m_lastFetchError = reason;

    const UsageData local = m_hasLastApiData
        ? mergeWithLastApi(m_scanner->calcDeltaFromLocal(m_lastApiData.fetchedAt.toUTC()))
        : m_scanner->calcFromLocal();
    m_current = local;
    applyData(local);

    const auto state = networkError
        ? UsagePopup::RefreshState::NetworkError
        : UsagePopup::RefreshState::LocalFallback;
    m_popup->setRefreshState(state);
}

// full, delta 는 백그라운드 스캔에서 이미 계산된 결과 → 메인 스레드 재스캔 없음
void TrayApp::onLocalUsage(UsageData full, UsageData delta, bool hasDelta)
{
    const UsageData merged = (m_hasLastApiData && hasDelta)
        ? mergeWithLastApi(delta)
        : full;
    m_current = merged;
    applyData(merged);

    // API 실패 시에만 로컬 폴백 표시 (성공 후 로컬 증분은 🟢 유지)
    if (m_apiFailed)
        m_popup->setRefreshState(UsagePopup::RefreshState::LocalFallback);

    // 마지막 API resetsAt 가 이미 지났으면 즉시 재호출 → 정확한 새 리셋 시각 수신
    if (m_hasLastApiData) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (m_lastApiData.fiveHour.resetsAt.isValid() &&
            m_lastApiData.fiveHour.resetsAt.toUTC() <= now)
            m_apiClient->fetchUsage();
    }
}

void TrayApp::applyData(const UsageData &data)
{
    m_popup->setData(data);
    m_popup->setCountdowns(
        formatCountdown(data.fiveHour.resetsAt),
        formatCountdown(data.sevenDay.resetsAt));

    // 7d >= 5h → 5h 기준, 7d < 5h → 7d 기준 (라인 색상과 동일 로직)
    const double dominant = (data.sevenDay.utilization >= data.fiveHour.utilization)
        ? data.fiveHour.utilization
        : data.sevenDay.utilization;
    m_tray->setIcon(makeIcon(dominant));
    updateTooltip();
}

UsageData TrayApp::mergeWithLastApi(const UsageData &data) const
{
    if (!m_hasLastApiData)
        return data;

    UsageData merged = m_lastApiData;
    const QDateTime now = QDateTime::currentDateTimeUtc();

    // API resetsAt 가 이미 과거(리셋 발생)이면 주기로 다음 리셋 시각 추정
    auto estimateNext = [](const QDateTime &last, qint64 periodSecs) -> QDateTime {
        if (!last.isValid()) return {};
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (last.toUTC() > now) return last;
        const qint64 elapsed = last.toUTC().secsTo(now);
        return last.toUTC().addSecs((elapsed / periodSecs + 1) * periodSecs);
    };
    merged.fiveHour.resetsAt  = estimateNext(m_lastApiData.fiveHour.resetsAt,  5LL * 3600);
    merged.sevenDay.resetsAt  = estimateNext(m_lastApiData.sevenDay.resetsAt,  7LL * 24 * 3600);
    merged.fromApi = false;
    merged.fetchedAt = QDateTime::currentDateTime();

    // 리셋이 발생한 경우 구 API 토큰을 0으로 처리 (리셋 후 신규 토큰만 집계)
    const bool reset5hOccurred = m_lastApiData.fiveHour.resetsAt.isValid() &&
                                  m_lastApiData.fiveHour.resetsAt.toUTC() <= now;
    const bool reset7dOccurred = m_lastApiData.sevenDay.resetsAt.isValid() &&
                                  m_lastApiData.sevenDay.resetsAt.toUTC() <= now;

    auto mergeQuota = [](const QuotaInfo &apiQuota, const QuotaInfo &deltaQuota,
                         qint64 limitTokens, bool resetOccurred) {
        if (!apiQuota.valid)
            return deltaQuota;

        QuotaInfo mergedQuota = apiQuota;
        if (limitTokens <= 0)
            return mergedQuota;

        // 리셋 발생 시 구 API 값 무시 → 리셋 후 델타만 반영
        const qint64 apiTokens = resetOccurred
            ? 0
            : qRound64(apiQuota.utilization * static_cast<double>(limitTokens));
        const qint64 deltaTokens = qMax<qint64>(0, deltaQuota.rawTokens);
        mergedQuota.rawTokens = apiTokens + deltaTokens;
        mergedQuota.limitTokens = limitTokens;
        mergedQuota.utilization = qMin(1.0, static_cast<double>(mergedQuota.rawTokens) / static_cast<double>(limitTokens));
        return mergedQuota;
    };

    merged.fiveHour = mergeQuota(m_lastApiData.fiveHour, data.fiveHour, UsageScanner::planLimit5h(), reset5hOccurred);
    merged.sevenDay = mergeQuota(m_lastApiData.sevenDay, data.sevenDay, UsageScanner::planLimit7d(), reset7dOccurred);
    merged.sevenDaySonnet = m_lastApiData.sevenDaySonnet;
    merged.recentModel = data.recentModel.isEmpty() ? m_lastApiData.recentModel : data.recentModel;

    if (m_lastApiData.extraUsage.enabled) {
        merged.extraUsage.enabled = true;
        merged.extraUsage.usedCredits = m_lastApiData.extraUsage.usedCredits + data.extraUsage.usedCredits;
        merged.extraUsage.limitDollars = m_lastApiData.extraUsage.limitDollars;
        if (merged.extraUsage.limitDollars > 0.0) {
            merged.extraUsage.utilization = qMin(1.0, merged.extraUsage.usedCredits / merged.extraUsage.limitDollars);
        }
    } else {
        merged.extraUsage = m_lastApiData.extraUsage;
    }

    return merged;
}

void TrayApp::updateCountdowns()
{
    if (!m_current.fetchedAt.isValid())
        return;

    m_popup->setCountdowns(
        formatCountdown(m_current.fiveHour.resetsAt),
        formatCountdown(m_current.sevenDay.resetsAt));
    m_popup->refreshNextFetch(m_apiClient->nextScheduledFetchAt());
}

void TrayApp::updateTooltip()
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

QIcon TrayApp::makeIcon(double utilization)
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

QString TrayApp::formatCountdown(const QDateTime &resetsAt) const
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


void TrayApp::onActivityDetected()
{
    m_isActive = true;
    m_popup->setActive();
}

void TrayApp::onUpdateAvailable(const QString &latestVersion, const QString &downloadUrl)
{
    m_updateUrl = downloadUrl;
    m_tray->showMessage(
        "ClaudeTray 업데이트 가능",
        QString("새로운 버전 %1이 출시되었습니다. 클릭하여 다운로드 페이지로 이동합니다.").arg(latestVersion),
        QSystemTrayIcon::Information,
        10000
    );
}

void TrayApp::onUpdateNotificationClicked()
{
    if (!m_updateUrl.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_updateUrl));
    }
}

