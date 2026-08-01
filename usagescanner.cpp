#include "usagescanner.h"
#include "credentialsreader.h"
#include <QDateTime>
#include <QTimeZone>
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
    {"pro", 7'500'000LL},
    {"max_5x", 37'500'000LL},
    {"max_20x", 150'000'000LL},
};

const QMap<QString, qint64> PLAN_LIMITS_7D = {
    {"pro", 150'000'000LL},
    {"max_5x", 750'000'000LL},
    {"max_20x", 3'000'000'000LL},
};

static constexpr int DEBOUNCE_MS      = 300;   // 파일 변경 디바운스 간격 (0.3초)
static constexpr int WATCHLIST_MS     = 5 * 60 * 1000;  // 감시 목록 갱신 간격
}

struct TokenRecord {
    QDateTime ts;
    QString model;
    qint64 input = 0;
    qint64 output = 0;
    qint64 cacheWrite = 0;
    qint64 cacheRead = 0;
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

    // 앱 시작 시 기존 JSONL 파일 즉시 스캔 (파일 변경 이벤트 없이도 초기값 표시)
    QTimer::singleShot(200, this, &UsageScanner::doScan);

    // 5분마다 새 프로젝트 폴더 자동 감지 + 스캔 (QFileSystemWatcher 재귀 불가 우회,
    // Windows에서 watcher 이벤트 누락 시 폴백)
    auto *watchTimer = new QTimer(this);
    connect(watchTimer, &QTimer::timeout, this, [this]() {
        refreshWatchList();
        m_debounceTimer->start();
    });
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

void UsageScanner::onFileChanged(const QString &path)
{
    // Windows에서 fileChanged 이벤트 후 파일이 watch list에서 제거될 수 있어 재등록
    if (!path.isEmpty())
        m_watcher->addPath(path);
    emit activityDetected();    // 즉시 알림 (투명도 제어용)
    m_debounceTimer->start();   // 타이머 리셋: 0.3초 후 스캔
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
    QSet<QString> seenRequestIds;
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

            // requestId 기준 중복 제거 (같은 응답이 thinking/tool_use 등 여러 줄로 기록됨)
            const QString rid = obj["requestId"].toString();
            const QString uid = obj["uuid"].toString();
            const QString dedupKey = rid.isEmpty() ? uid : rid;
            if (!dedupKey.isEmpty()) {
                if (seenRequestIds.contains(dedupKey))
                    continue;
                seenRequestIds.insert(dedupKey);
            }

            QJsonObject usage = obj["usage"].toObject();
            if (usage.isEmpty()) {
                usage = obj["message"].toObject()["usage"].toObject();
            }
            if (usage.isEmpty())
                continue;

            const QString tsStr = obj["timestamp"].toString();
            QDateTime ts = QDateTime::fromString(tsStr, Qt::ISODateWithMs);
            if (!ts.isValid())
                ts = QDateTime::fromString(tsStr, Qt::ISODate);
            if (!ts.isValid())
                continue;
            ts.setTimeZone(QTimeZone::UTC);

            const QString model = obj["message"].toObject()["model"].toString();
            qint64 input = usage["input_tokens"].toVariant().toLongLong();
            qint64 output = usage["output_tokens"].toVariant().toLongLong();
            qint64 cacheWrite = usage["cache_creation_input_tokens"].toVariant().toLongLong();
            qint64 cacheRead = usage["cache_read_input_tokens"].toVariant().toLongLong();

            records.append({ts, model, input, output, cacheWrite, cacheRead});
        }
    }

    std::sort(records.begin(), records.end(),
              [](const TokenRecord &a, const TokenRecord &b) {
                  return a.ts < b.ts;
              });

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool deltaMode = rangeStartUtc.isValid();

    // 리셋이 스캔 범위 안에서 발생했으면 리셋 시각 이후 토큰만 집계
    // (리셋 전 토큰을 delta에 포함하면 mergeWithLastApi에서 이중 계산됨)
    QDateTime window5hStart;
    QDateTime window7dStart;

    // 마지막 리셋 시각이 이미 과거일 경우 주기를 더해 다음 리셋 시각 추정
    auto estimateNext = [](const QDateTime &last, qint64 periodSecs,
                           const QDateTime &now) -> QDateTime {
        if (!last.isValid()) return {};
        if (last.toUTC() > now) return last;
        const qint64 elapsed = last.toUTC().secsTo(now);
        return last.toUTC().addSecs((elapsed / periodSecs + 1) * periodSecs);
    };

    if (deltaMode) {
        const bool r5InRange = reset5h.isValid()
            && reset5h.toUTC() > rangeStartUtc
            && reset5h.toUTC() <= now;
        const bool r7InRange = reset7d.isValid()
            && reset7d.toUTC() > rangeStartUtc
            && reset7d.toUTC() <= now;
        window5hStart = r5InRange ? reset5h.toUTC() : rangeStartUtc;
        window7dStart = r7InRange ? reset7d.toUTC() : rangeStartUtc;
    } else {
        // full 모드: 리셋 시각(resetsAt) 힌트가 있으면 해당 주기의 진짜 시작 시점을 계산
        QDateTime nextReset5h = reset5h.isValid() ? estimateNext(reset5h, 5LL * 3600, now) : QDateTime();
        QDateTime nextReset7d = reset7d.isValid() ? estimateNext(reset7d, 7LL * 24 * 3600, now) : QDateTime();

        window5hStart = nextReset5h.isValid() ? nextReset5h.addSecs(-5LL * 3600) : now.addSecs(-5LL * 3600);
        window7dStart = nextReset7d.isValid() ? nextReset7d.addDays(-7) : now.addDays(-7);
    }

    qint64 rolling5hTokens = 0;
    qint64 rolling7dTokens = 0;
    double rolling5hCost = 0.0;
    double rolling7dCost = 0.0;

    // 추가 결제 크레딧 누적을 위해 당월 1일 UTC 시각 계산
    QDateTime billingCycleStart(QDate(now.date().year(), now.date().month(), 1), QTime(0, 0), QTimeZone::UTC);
    double monthlyAccumulatedCost = 0.0;

    for (const auto &record : records) {
        qint64 discountedTokens = record.input + record.output + record.cacheWrite + qRound64(record.cacheRead * 0.1);
        ModelPricing p = getPricingForModel(record.model);
        double cost = (record.input * p.inputRate +
                       record.output * p.outputRate +
                       record.cacheWrite * p.cacheWriteRate +
                       record.cacheRead * p.cacheReadRate) / 1000000.0;

        if (record.ts >= window7dStart) {
            rolling7dTokens += discountedTokens;
            rolling7dCost += cost;
        }
        if (record.ts >= window5hStart) {
            rolling5hTokens += discountedTokens;
            rolling5hCost += cost;
        }
        if (record.ts >= billingCycleStart) {
            monthlyAccumulatedCost += cost;
        }
    }

    const QString planType = CredentialsReader::subscriptionType();
    const qint64 limit5h = PLAN_LIMITS_5H.value(planType, 0);
    const qint64 limit7d = PLAN_LIMITS_7D.value(planType, 0);

    qDebug() << "[UsageScanner] plan=" << planType
             << "deltaMode=" << deltaMode
             << "rolling5h=" << rolling5hTokens
             << "rolling7d=" << rolling7dTokens
             << "limit5h=" << limit5h
             << "limit7d=" << limit7d
             << "rolling5hCost=" << rolling5hCost
             << "rolling7dCost=" << rolling7dCost
             << "monthlyAccumulatedCost=" << monthlyAccumulatedCost;

    UsageData data;
    data.fromApi  = false;
    data.fetchedAt = QDateTime::currentDateTime();
    data.recentModel = records.isEmpty() ? "" : records.last().model;

    data.extraUsage.enabled = true;
    data.extraUsage.usedCredits = deltaMode ? rolling5hCost : monthlyAccumulatedCost;

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

ModelPricing UsageScanner::getPricingForModel(const QString &modelName)
{
    // 계열별 버전별 요율 저장 맵
    // QMap<FamilyName, QMap<VersionName, ModelPricing>>
    static QMap<QString, QMap<QString, ModelPricing>> pricingTree;
    static bool loaded = false;

    if (!loaded) {
        QFile file(":/model_pricing.json");
        if (file.open(QIODevice::ReadOnly)) {
            QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            for (auto familyIt = root.begin(); familyIt != root.end(); ++familyIt) {
                QString family = familyIt.key();
                QJsonObject versionObj = familyIt.value().toObject();
                QMap<QString, ModelPricing> versionMap;
                
                for (auto versionIt = versionObj.begin(); versionIt != versionObj.end(); ++versionIt) {
                    QString version = versionIt.key();
                    QJsonObject rateObj = versionIt.value().toObject();
                    
                    ModelPricing p;
                    p.inputRate = rateObj["input_rate"].toDouble();
                    p.outputRate = rateObj["output_rate"].toDouble();
                    p.cacheWriteRate = rateObj["cache_write_rate"].toDouble();
                    p.cacheReadRate = rateObj["cache_read_rate"].toDouble();
                    
                    versionMap[version] = p;
                }
                pricingTree[family] = versionMap;
            }
            loaded = true;
        }
    }

    // 하드코딩 디폴트 요율 (Sonnet 5 기준)
    ModelPricing defaultPricing;
    defaultPricing.inputRate = 3.00;
    defaultPricing.outputRate = 15.00;
    defaultPricing.cacheWriteRate = 3.75;
    defaultPricing.cacheReadRate = 0.30;

    const QString lowerName = modelName.toLower();
    
    // 1단계: 계열(Family) 매칭
    QString matchedFamily;
    for (auto it = pricingTree.begin(); it != pricingTree.end(); ++it) {
        if (lowerName.contains(it.key())) {
            matchedFamily = it.key();
            break;
        }
    }

    if (matchedFamily.isEmpty()) {
        if (pricingTree.contains("sonnet") && pricingTree["sonnet"].contains("5")) {
            return pricingTree["sonnet"]["5"];
        }
        return defaultPricing;
    }

    // 2단계: 버전(Version) 매칭 (Longest match 기법)
    const QMap<QString, ModelPricing> &versionMap = pricingTree[matchedFamily];
    QString bestVersionKey;
    int bestVersionLength = 0;

    for (auto it = versionMap.begin(); it != versionMap.end(); ++it) {
        const QString &version = it.key();
        if (lowerName.contains(version)) {
            if (version.length() > bestVersionLength) {
                bestVersionKey = version;
                bestVersionLength = version.length();
            }
        }
    }

    if (bestVersionLength > 0) {
        return versionMap.value(bestVersionKey);
    }

    // 버전을 찾지 못한 경우의 폴백
    if (!versionMap.isEmpty()) {
        if (versionMap.contains("5")) return versionMap.value("5");
        if (versionMap.contains("3.5")) return versionMap.value("3.5");
        return versionMap.begin().value();
    }

    return defaultPricing;
}
