#include "usagescanner.h"
#include "credentialsreader.h"
#include <QDateTime>
#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QtConcurrent>
#include <algorithm>

namespace {
const QMap<QString, qint64> PLAN_LIMITS_5H = {
    {"pro", 18'000'000LL},
    {"max_5x", 90'000'000LL},
    {"max_20x", 360'000'000LL},
};

const QMap<QString, qint64> PLAN_LIMITS_7D = {
    {"pro", 144'000'000LL},
    {"max_5x", 720'000'000LL},
    {"max_20x", 2'880'000'000LL},
};

static constexpr int DEBOUNCE_MS      = 300;   // 파일 변경 디바운스 간격 (0.3초)
static constexpr int WATCHLIST_MS     = 5 * 60 * 1000;  // 감시 목록 갱신 간격
}

struct TokenRecord {
    QDateTime ts;
    qint64 tokens = 0;
};

UsageScanner::UsageScanner(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_debounceTimer(new QTimer(this))
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &UsageScanner::onDirectoryChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &UsageScanner::onFileChanged);

    // 디바운스: 2초 동안 추가 이벤트가 없을 때만 스캔 실행
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(DEBOUNCE_MS);
    connect(m_debounceTimer, &QTimer::timeout, this, &UsageScanner::doScan);

    refreshWatchList();

    // 5분마다 새 프로젝트 폴더 자동 감지 (QFileSystemWatcher 재귀 불가 우회)
    auto *watchTimer = new QTimer(this);
    connect(watchTimer, &QTimer::timeout, this, &UsageScanner::refreshWatchList);
    watchTimer->start(WATCHLIST_MS);
}

void UsageScanner::setWindowHints(const QDateTime &reset5h, const QDateTime &reset7d)
{
    if (reset5h.isValid())
        m_lastKnownReset5h = reset5h;
    if (reset7d.isValid())
        m_lastKnownReset7d = reset7d;
}

void UsageScanner::setDeltaStart(const QDateTime &since)
{
    m_deltaStart = since.toUTC();
}

// ── 파일 감시 이벤트 ──────────────────────────────────────────────────────────

void UsageScanner::onDirectoryChanged(const QString &)
{
    refreshWatchList();
    emit activityDetected();    // 즉시 알림 (투명도 제어용)
    m_debounceTimer->start();   // 타이머 리셋: 2초 후 스캔
}

void UsageScanner::onFileChanged(const QString &)
{
    emit activityDetected();    // 즉시 알림 (투명도 제어용)
    m_debounceTimer->start();   // 타이머 리셋: 2초 후 스캔
}

void UsageScanner::refreshWatchList()
{
    const QString projectsDir = CredentialsReader::claudeDir() + "/projects";

    if (!m_watcher->directories().contains(projectsDir))
        m_watcher->addPath(projectsDir);

    QDirIterator dirs(projectsDir, QDir::Dirs | QDir::NoDotAndDotDot);
    while (dirs.hasNext()) {
        const QString dir = dirs.next();
        if (!m_watcher->directories().contains(dir))
            m_watcher->addPath(dir);

        QDirIterator files(dir, {"*.jsonl"}, QDir::Files);
        while (files.hasNext()) {
            const QString filePath = files.next();
            if (!m_watchedFiles.contains(filePath)) {
                m_watcher->addPath(filePath);
                m_watchedFiles.insert(filePath);
            }
        }
    }
}

// ── 백그라운드 스캔 ───────────────────────────────────────────────────────────

void UsageScanner::doScan()
{
    // 디바운스 만료 = 파일 변경이 2초간 없었음 → 즉시 idle 전환
    emit activityStopped();

    if (m_scanPending) {
        // 이전 스캔이 아직 진행 중 → 완료 후 재시도
        m_debounceTimer->start();
        return;
    }
    m_scanPending = true;

    // 멤버 변수를 람다에 복사 (스레드 안전)
    const QDateTime hint5h     = m_lastKnownReset5h;
    const QDateTime hint7d     = m_lastKnownReset7d;
    const QDateTime deltaStart = m_deltaStart;
    const bool      hasDelta   = deltaStart.isValid();

    using ResultPair = QPair<UsageData, UsageData>;
    auto *watcher = new QFutureWatcher<ResultPair>(this);

    connect(watcher, &QFutureWatcher<ResultPair>::finished,
            this, [this, watcher, hasDelta]() {
        m_scanPending = false;
        const auto result = watcher->result();
        emit localUsageUpdated(result.first, result.second, hasDelta);
        watcher->deleteLater();
    });

    QFuture<ResultPair> future =
        QtConcurrent::run([hint5h, hint7d, deltaStart, hasDelta]() -> ResultPair {
            UsageData full  = calcUsageForRange(QDateTime(), hint5h, hint7d);
            UsageData delta = hasDelta
                ? calcUsageForRange(deltaStart, hint5h, hint7d)
                : full;
            return {full, delta};
        });

    watcher->setFuture(future);
}

// ── 플랜 한도 ────────────────────────────────────────────────────────────────

qint64 UsageScanner::planLimit5h()
{
    return PLAN_LIMITS_5H.value(CredentialsReader::subscriptionType(), 0);
}

qint64 UsageScanner::planLimit7d()
{
    return PLAN_LIMITS_7D.value(CredentialsReader::subscriptionType(), 0);
}

// ── 메인 스레드 직접 호출용 (onFetchFailed 경로) ──────────────────────────────

UsageData UsageScanner::calcFromLocal() const
{
    return calcUsageForRange(QDateTime(), m_lastKnownReset5h, m_lastKnownReset7d);
}

UsageData UsageScanner::calcDeltaFromLocal(const QDateTime &sinceUtc) const
{
    return calcUsageForRange(sinceUtc.toUTC(), m_lastKnownReset5h, m_lastKnownReset7d);
}

// ── 핵심 스캔 로직 (static: 멤버 변수 미접근 → 스레드 안전) ─────────────────────

UsageData UsageScanner::calcUsageForRange(const QDateTime &rangeStartUtc,
                                           const QDateTime &reset5h,
                                           const QDateTime &reset7d)
{
    const QString projectsDir = CredentialsReader::claudeDir() + "/projects";
    QSet<QString> seenUuids;
    QVector<TokenRecord> records;

    QDirIterator it(projectsDir, {"*.jsonl"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (!file.open(QIODevice::ReadOnly))
            continue;

        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty())
                continue;

            const QJsonObject obj = QJsonDocument::fromJson(line).object();
            if (obj["type"].toString() != "assistant")
                continue;

            // uuid 로 중복 레코드 제거
            const QString uid = obj["uuid"].toString();
            if (!uid.isEmpty()) {
                if (seenUuids.contains(uid))
                    continue;
                seenUuids.insert(uid);
            }

            const QJsonObject usage = obj["message"].toObject()["usage"].toObject();
            if (usage.isEmpty())
                continue;

            const QString tsStr = obj["timestamp"].toString();
            QDateTime ts = QDateTime::fromString(tsStr, Qt::ISODateWithMs);
            if (!ts.isValid())
                ts = QDateTime::fromString(tsStr, Qt::ISODate);
            if (!ts.isValid())
                continue;

            const qint64 tokens =
                usage["input_tokens"].toVariant().toLongLong() +
                usage["output_tokens"].toVariant().toLongLong() +
                usage["cache_creation_input_tokens"].toVariant().toLongLong() +
                usage["cache_read_input_tokens"].toVariant().toLongLong();

            records.append({ts.toUTC(), tokens});
        }
    }

    std::sort(records.begin(), records.end(),
              [](const TokenRecord &a, const TokenRecord &b) {
                  return a.ts < b.ts;
              });

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool deltaMode = rangeStartUtc.isValid();
    const QDateTime window5hStart = deltaMode ? rangeStartUtc : now.addSecs(-5LL * 3600);
    const QDateTime window7dStart = deltaMode ? rangeStartUtc : now.addDays(-7);

    qint64 rolling5hTokens = 0;
    qint64 rolling7dTokens = 0;

    for (const auto &record : records) {
        if (record.ts >= window7dStart)
            rolling7dTokens += record.tokens;
        if (record.ts >= window5hStart)
            rolling5hTokens += record.tokens;
    }

    const QString planType = CredentialsReader::subscriptionType();
    const qint64 limit5h = PLAN_LIMITS_5H.value(planType, 0);
    const qint64 limit7d = PLAN_LIMITS_7D.value(planType, 0);

    qDebug() << "[UsageScanner] plan=" << planType
             << "deltaMode=" << deltaMode
             << "rolling5h=" << rolling5hTokens
             << "rolling7d=" << rolling7dTokens
             << "limit5h=" << limit5h
             << "limit7d=" << limit7d;

    UsageData data;
    data.fromApi  = false;
    data.fetchedAt = QDateTime::currentDateTime();

    // 마지막 리셋 시각이 이미 과거일 경우 주기를 더해 다음 리셋 시각 추정
    auto estimateNext = [](const QDateTime &last, qint64 periodSecs,
                           const QDateTime &now) -> QDateTime {
        if (!last.isValid()) return {};
        if (last.toUTC() > now) return last;
        const qint64 elapsed = last.toUTC().secsTo(now);
        return last.toUTC().addSecs((elapsed / periodSecs + 1) * periodSecs);
    };

    if (rolling5hTokens > 0 || limit5h > 0) {
        data.fiveHour.rawTokens   = rolling5hTokens;
        data.fiveHour.limitTokens = limit5h;
        data.fiveHour.resetsAt    = estimateNext(reset5h, 5LL * 3600, now);
        data.fiveHour.valid       = true;
        if (limit5h > 0)
            data.fiveHour.utilization = qMin(1.0, static_cast<double>(rolling5hTokens) /
                                                  static_cast<double>(limit5h));
    }

    if (rolling7dTokens > 0 || limit7d > 0) {
        data.sevenDay.rawTokens   = rolling7dTokens;
        data.sevenDay.limitTokens = limit7d;
        data.sevenDay.resetsAt    = estimateNext(reset7d, 7LL * 24 * 3600, now);
        data.sevenDay.valid       = true;
        if (limit7d > 0)
            data.sevenDay.utilization = qMin(1.0, static_cast<double>(rolling7dTokens) /
                                                  static_cast<double>(limit7d));
    }

    return data;
}
