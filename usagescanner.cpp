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
#include <iterator>

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

constexpr int DEBOUNCE_MS  = 300;             // 파일 변경 디바운스 간격 (0.3초)
constexpr int WATCHLIST_MS = 5 * 60 * 1000;   // 감시 목록 갱신 간격

constexpr qint64 SECS_5H = 5LL * 3600;
constexpr qint64 SECS_7D = 7LL * 24 * 3600;

// 캐시 읽기는 신규 입력보다 훨씬 저렴하므로 할당량 집계에서 10% 로 가중한다.
constexpr double CACHE_READ_WEIGHT = 0.1;

// 추가 결제 크레딧은 월 단위로 누적되므로 이번 달 1일(UTC)이 기준점이다.
QDateTime billingCycleStart(const QDateTime &nowUtc)
{
    return QDateTime(QDate(nowUtc.date().year(), nowUtc.date().month(), 1),
                     QTime(0, 0), QTimeZone::UTC);
}
}

UsageScanner::UsageScanner(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_debounceTimer(new QTimer(this))
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &UsageScanner::onDirectoryChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &UsageScanner::onFileChanged);

    // 디바운스: DEBOUNCE_MS 동안 추가 이벤트가 없을 때만 스캔 실행
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(DEBOUNCE_MS);
    connect(m_debounceTimer, &QTimer::timeout, this, &UsageScanner::onDebounceTimeout);

    refreshWatchList();

    // 앱 시작 시 기존 JSONL 파일 즉시 스캔 (파일 변경 이벤트 없이도 초기값 표시)
    QTimer::singleShot(200, this, &UsageScanner::requestScan);

    // 5분마다 새 프로젝트 폴더 자동 감지 + 스캔 (QFileSystemWatcher 재귀 불가 우회,
    // Windows에서 watcher 이벤트 누락 시 폴백)
    // 디바운스 타이머가 아니라 requestScan 을 직접 부른다. 디바운스를 거치면
    // 파일 변경이 없었는데도 activityStopped 가 튀어 idle 로 잘못 전환된다.
    auto *watchTimer = new QTimer(this);
    connect(watchTimer, &QTimer::timeout, this, [this]() {
        refreshWatchList();
        requestScan();
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
    m_debounceTimer->start();   // 타이머 리셋: DEBOUNCE_MS 후 스캔
}

void UsageScanner::onFileChanged(const QString &path)
{
    // Windows에서 fileChanged 이벤트 후 파일이 watch list에서 제거될 수 있어 재등록
    if (!path.isEmpty())
        m_watcher->addPath(path);
    emit activityDetected();    // 즉시 알림 (투명도 제어용)
    m_debounceTimer->start();   // 타이머 리셋: DEBOUNCE_MS 후 스캔
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

void UsageScanner::onDebounceTimeout()
{
    // 디바운스 만료 = DEBOUNCE_MS 동안 파일 변경이 없었음 → 즉시 idle 전환
    emit activityStopped();
    requestScan();
}

void UsageScanner::requestScan()
{
    if (m_scanPending) {
        // 진행 중인 스캔이 끝난 직후 한 번 더 돌린다. 예전처럼 디바운스 타이머를
        // 되감으면 그때마다 activityStopped 가 다시 나가는 부작용이 있었다.
        m_rescanQueued = true;
        return;
    }
    m_scanPending = true;

    // 멤버 변수를 람다에 복사 (스레드 안전)
    const QDateTime hint5h     = m_lastKnownReset5h;
    const QDateTime hint7d     = m_lastKnownReset7d;
    const QDateTime deltaStart = m_deltaStart;

    auto *watcher = new QFutureWatcher<ScanResult>(this);

    connect(watcher, &QFutureWatcher<ScanResult>::finished, this, [this, watcher]() {
        m_scanPending = false;
        const ScanResult result = watcher->result();
        watcher->deleteLater();

        emit localUsageUpdated(result.full, result.delta, result.hasDelta);

        if (m_rescanQueued) {
            m_rescanQueued = false;
            requestScan();
        }
    });

    watcher->setFuture(QtConcurrent::run([hint5h, hint7d, deltaStart]() {
        return scanLocal(deltaStart, hint5h, hint7d);
    }));
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

// ── 순수 계산 (파일 I/O 없음) ─────────────────────────────────────────────────

QDateTime UsageScanner::estimateNextReset(const QDateTime &last, qint64 periodSecs,
                                          const QDateTime &nowUtc)
{
    if (!last.isValid() || periodSecs <= 0)
        return {};
    const QDateTime lastUtc = last.toUTC();
    if (lastUtc > nowUtc)
        return lastUtc;
    const qint64 elapsed = lastUtc.secsTo(nowUtc);
    return lastUtc.addSecs((elapsed / periodSecs + 1) * periodSecs);
}

QDateTime UsageScanner::earliestRelevant(const QDateTime &nowUtc,
                                         const QDateTime &deltaStartUtc,
                                         const QDateTime &reset7d)
{
    const QDateTime next7d = estimateNextReset(reset7d, SECS_7D, nowUtc);
    QDateTime earliest = next7d.isValid() ? next7d.addSecs(-SECS_7D)
                                          : nowUtc.addSecs(-SECS_7D);

    // 추가 크레딧은 월 단위 누적이라 이번 달 1일까지 거슬러 올라가야 한다.
    const QDateTime billing = billingCycleStart(nowUtc);
    if (billing < earliest)
        earliest = billing;

    // 오래 오프라인이었다면 deltaStart 가 7d 윈도우보다 이를 수 있다.
    if (deltaStartUtc.isValid() && deltaStartUtc.toUTC() < earliest)
        earliest = deltaStartUtc.toUTC();

    return earliest;
}

ScanResult UsageScanner::aggregate(const QVector<TokenRecord> &records,
                                   const QDateTime &nowUtc,
                                   const QDateTime &deltaStartUtc,
                                   const QDateTime &reset5h,
                                   const QDateTime &reset7d,
                                   qint64 limit5h,
                                   qint64 limit7d)
{
    const bool      hasDelta   = deltaStartUtc.isValid();
    const QDateTime deltaStart = hasDelta ? deltaStartUtc.toUTC() : QDateTime();

    const QDateTime next5h = estimateNextReset(reset5h, SECS_5H, nowUtc);
    const QDateTime next7d = estimateNextReset(reset7d, SECS_7D, nowUtc);

    // full 윈도우: 다음 리셋에서 주기를 뺀 시점이 현재 윈도우의 시작이다.
    const QDateTime full5hStart = next5h.isValid() ? next5h.addSecs(-SECS_5H)
                                                   : nowUtc.addSecs(-SECS_5H);
    const QDateTime full7dStart = next7d.isValid() ? next7d.addSecs(-SECS_7D)
                                                   : nowUtc.addSecs(-SECS_7D);

    // delta 윈도우: 델타 구간 안에서 리셋이 일어났다면 리셋 이후만 센다.
    // (리셋 전 토큰까지 델타에 넣으면 mergeWithLastApi 에서 이중 계산된다)
    QDateTime delta5hStart = deltaStart;
    QDateTime delta7dStart = deltaStart;
    if (hasDelta) {
        if (reset5h.isValid() && reset5h.toUTC() > deltaStart && reset5h.toUTC() <= nowUtc)
            delta5hStart = reset5h.toUTC();
        if (reset7d.isValid() && reset7d.toUTC() > deltaStart && reset7d.toUTC() <= nowUtc)
            delta7dStart = reset7d.toUTC();
    }

    const QDateTime billing = billingCycleStart(nowUtc);

    qint64 full5hTok = 0, full7dTok = 0, delta5hTok = 0, delta7dTok = 0;
    double fullCost = 0.0, deltaCost = 0.0;

    // 파일을 두 번 읽는 대신 한 벌의 레코드로 full/delta 를 동시에 누산한다.
    for (const TokenRecord &r : records) {
        const qint64 tokens = r.input + r.output + r.cacheWrite
                            + qRound64(r.cacheRead * CACHE_READ_WEIGHT);
        const ModelPricing p = getPricingForModel(r.model);
        const double cost = (r.input      * p.inputRate
                           + r.output     * p.outputRate
                           + r.cacheWrite * p.cacheWriteRate
                           + r.cacheRead  * p.cacheReadRate) / 1'000'000.0;

        if (r.ts >= full5hStart)  full5hTok += tokens;
        if (r.ts >= full7dStart)  full7dTok += tokens;
        if (r.ts >= billing)      fullCost  += cost;

        if (hasDelta) {
            if (r.ts >= delta5hStart) delta5hTok += tokens;
            if (r.ts >= delta7dStart) delta7dTok += tokens;
            // 추가 크레딧은 월 단위라 5h 리셋과 무관하게 deltaStart 를 기준으로 한다.
            // delta5hStart 를 쓰면 5h 리셋이 낄 때마다 그 사이 비용이 통째로 사라진다.
            if (r.ts >= deltaStart && r.ts >= billing) deltaCost += cost;
        }
    }

    auto build = [&](qint64 tok5h, qint64 tok7d, double credits) {
        UsageData d;
        d.fromApi   = false;
        d.fetchedAt = QDateTime::currentDateTime();

        // extraUsage.enabled 는 API 만이 켤 수 있다. 스캐너가 켜 버리면 한도를
        // 모르는 채로 "추가 결제 크레딧 $x / $0.00" 패널이 떠 버린다.
        // 비용만 채우고, 표시 여부는 API 값을 가진 쪽(TrayApp)에 맡긴다.
        d.extraUsage.enabled     = false;
        d.extraUsage.usedCredits = credits;

        if (tok5h > 0 || limit5h > 0) {
            d.fiveHour.rawTokens   = tok5h;
            d.fiveHour.limitTokens = limit5h;
            d.fiveHour.resetsAt    = next5h;
            d.fiveHour.valid       = true;
            if (limit5h > 0)
                d.fiveHour.utilization =
                    qMin(1.0, static_cast<double>(tok5h) / static_cast<double>(limit5h));
        }
        if (tok7d > 0 || limit7d > 0) {
            d.sevenDay.rawTokens   = tok7d;
            d.sevenDay.limitTokens = limit7d;
            d.sevenDay.resetsAt    = next7d;
            d.sevenDay.valid       = true;
            if (limit7d > 0)
                d.sevenDay.utilization =
                    qMin(1.0, static_cast<double>(tok7d) / static_cast<double>(limit7d));
        }
        return d;
    };

    ScanResult result;
    result.hasDelta = hasDelta;
    result.full     = build(full5hTok, full7dTok, fullCost);
    result.delta    = hasDelta ? build(delta5hTok, delta7dTok, deltaCost) : result.full;
    return result;
}

// ── JSONL 파싱 ────────────────────────────────────────────────────────────────

QVector<TokenRecord> UsageScanner::readRecords(const QString &projectsDir,
                                               const QDateTime &earliestUtc,
                                               QString *recentModelOut)
{
    QVector<TokenRecord> records;
    QSet<QString> seenKeys;
    QDateTime latestTs;
    QString   latestModel;

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
            const QString dedupKey = rid.isEmpty() ? obj["uuid"].toString() : rid;
            if (!dedupKey.isEmpty()) {
                if (seenKeys.contains(dedupKey))
                    continue;
                seenKeys.insert(dedupKey);
            }

            const QJsonObject message = obj["message"].toObject();
            QJsonObject usage = obj["usage"].toObject();
            if (usage.isEmpty())
                usage = message["usage"].toObject();
            if (usage.isEmpty())
                continue;

            const QString tsStr = obj["timestamp"].toString();
            QDateTime ts = QDateTime::fromString(tsStr, Qt::ISODateWithMs);
            if (!ts.isValid())
                ts = QDateTime::fromString(tsStr, Qt::ISODate);
            if (!ts.isValid())
                continue;
            // setTimeZone() 은 날짜·시각 필드를 그대로 두고 존만 바꿔 '재해석'한다.
            // "+09:00" 오프셋이 붙은 값이 오면 가리키는 순간이 통째로 어긋나므로
            // 반드시 변환(toUTC)해야 한다.
            ts = ts.toUTC();

            const QString model = message["model"].toString();

            // 최근 모델은 집계 윈도우와 무관하게 '전체 중 최신'을 쓴다.
            // 이것만 있으면 되므로 예전처럼 전체 레코드를 정렬할 필요가 없다.
            if (!latestTs.isValid() || ts > latestTs) {
                latestTs    = ts;
                latestModel = model;
            }

            // 집계 대상 밖이면 담지 않는다 (메모리·순회 비용 절감)
            if (earliestUtc.isValid() && ts < earliestUtc)
                continue;

            records.append({ts, model,
                            usage["input_tokens"].toVariant().toLongLong(),
                            usage["output_tokens"].toVariant().toLongLong(),
                            usage["cache_creation_input_tokens"].toVariant().toLongLong(),
                            usage["cache_read_input_tokens"].toVariant().toLongLong()});
        }
    }

    if (recentModelOut)
        *recentModelOut = latestModel;
    return records;
}

// ── 스캔 오케스트레이션 (static: 멤버 변수 미접근 → 스레드 안전) ─────────────────

ScanResult UsageScanner::scanLocal(const QDateTime &deltaStartUtc,
                                   const QDateTime &reset5h,
                                   const QDateTime &reset7d)
{
    const QDateTime now      = QDateTime::currentDateTimeUtc();
    const QDateTime earliest = earliestRelevant(now, deltaStartUtc, reset7d);

    QString recentModel;
    const QVector<TokenRecord> records =
        readRecords(CredentialsReader::claudeDir() + "/projects", earliest, &recentModel);

    const qint64 limit5h = planLimit5h();
    const qint64 limit7d = planLimit7d();

    ScanResult result = aggregate(records, now, deltaStartUtc, reset5h, reset7d,
                                  limit5h, limit7d);
    result.full.recentModel  = recentModel;
    result.delta.recentModel = recentModel;

    qDebug() << "[UsageScanner] plan=" << CredentialsReader::subscriptionType()
             << "records=" << records.size()
             << "full5h=" << result.full.fiveHour.rawTokens
             << "full7d=" << result.full.sevenDay.rawTokens
             << "delta5h=" << result.delta.fiveHour.rawTokens
             << "limit5h=" << limit5h
             << "limit7d=" << limit7d
             << "monthCost=" << result.full.extraUsage.usedCredits
             << "deltaCost=" << result.delta.extraUsage.usedCredits;

    return result;
}

// 계열별·버전별 요율. QMap<FamilyName, QMap<VersionName, ModelPricing>>
using PricingTree = QMap<QString, QMap<QString, ModelPricing>>;

// 매직 스태틱: 초기화가 한 번만, 스레드 안전하게 일어남을 C++11 이 보장한다.
// (예전의 `static QMap + static bool loaded` 조합은 이 보장을 받지 못해
//  워커 스레드 스캔과 메인 스레드 호출이 겹치면 QMap 동시 쓰기로 UB 였다.)
static const PricingTree &pricingTree()
{
    static const PricingTree tree = []() {
        PricingTree t;
        QFile file(":/model_pricing.json");
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "[UsageScanner] model_pricing.json 을 열 수 없음 - 기본 요율 사용";
            return t;
        }
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        for (auto familyIt = root.constBegin(); familyIt != root.constEnd(); ++familyIt) {
            if (familyIt.key().startsWith('_'))   // "_comment" 등 메타 키는 계열이 아니다
                continue;
            const QJsonObject versionObj = familyIt.value().toObject();
            QMap<QString, ModelPricing> versionMap;
            for (auto versionIt = versionObj.constBegin();
                 versionIt != versionObj.constEnd(); ++versionIt) {
                const QJsonObject rateObj = versionIt.value().toObject();
                ModelPricing p;
                p.inputRate      = rateObj["input_rate"].toDouble();
                p.outputRate     = rateObj["output_rate"].toDouble();
                p.cacheWriteRate = rateObj["cache_write_rate"].toDouble();
                p.cacheReadRate  = rateObj["cache_read_rate"].toDouble();
                versionMap.insert(versionIt.key(), p);
            }
            t.insert(familyIt.key(), versionMap);
        }
        return t;
    }();
    return tree;
}

// 모델 ID 에서 버전 문자열을 뽑는다.
//   "claude-opus-4-1-20250805"   → "4.1"
//   "claude-3-5-sonnet-20241022" → "3.5"
//   "claude-opus-5"              → "5"
// 날짜 꼬리(8자리 이상 숫자)를 반드시 떼야 한다. 예전에는 모델 ID 전체를
// contains() 로 훑어서 "20250805" 안의 '5' 가 버전 키 "5" 에 걸렸고,
// 그 결과 Opus 4.1 이 Opus 5 요율(1/3 가격)로 계산됐다.
static QString extractVersion(const QString &lowerName)
{
    QStringList nums;
    const QStringList tokens = lowerName.split('-', Qt::SkipEmptyParts);
    for (const QString &tok : tokens) {
        bool allDigits = true;
        for (const QChar &c : tok) {
            if (!c.isDigit()) { allDigits = false; break; }
        }
        if (!allDigits || tok.length() >= 8)   // 비숫자 토큰 / 날짜 꼬리는 제외
            continue;
        nums.append(tok);
    }
    return nums.join('.');
}

ModelPricing UsageScanner::getPricingForModel(const QString &modelName)
{
    // 요율 파일을 못 읽었을 때의 최종 폴백 (Sonnet 5 기준)
    ModelPricing defaultPricing;
    defaultPricing.inputRate      = 3.00;
    defaultPricing.outputRate     = 15.00;
    defaultPricing.cacheWriteRate = 3.75;
    defaultPricing.cacheReadRate  = 0.30;

    const PricingTree &tree = pricingTree();
    const QString lowerName = modelName.toLower();

    // 1단계: 계열(Family) 매칭
    QString matchedFamily;
    for (auto it = tree.constBegin(); it != tree.constEnd(); ++it) {
        if (lowerName.contains(it.key())) {
            matchedFamily = it.key();
            break;
        }
    }

    if (matchedFamily.isEmpty())
        return tree.value("sonnet").value("5", defaultPricing);

    // 2단계: 버전(Version) 매칭
    const QMap<QString, ModelPricing> versionMap = tree.value(matchedFamily);
    const QString version = extractVersion(lowerName);

    if (versionMap.contains(version))
        return versionMap.value(version);

    // 정확히 없으면 가장 긴 접두 일치 ("4.5.1" → "4.5", "3.5" 가 "3" 보다 우선)
    QString bestVersionKey;
    for (auto it = versionMap.constBegin(); it != versionMap.constEnd(); ++it) {
        if (version.startsWith(it.key()) && it.key().length() > bestVersionKey.length())
            bestVersionKey = it.key();
    }
    if (!bestVersionKey.isEmpty())
        return versionMap.value(bestVersionKey);

    // 계열은 맞췄지만 버전을 못 찾은 경우(= 아직 요율표에 없는 신모델)의 폴백.
    // QMap 은 키 정렬 상태이고 버전 키는 "3" < "3.5" < "4" < "4.8" < "5" 처럼
    // 문자열 순서가 곧 버전 순서이므로, 마지막 키가 그 계열의 최신 요율이다.
    // (두 자리 메이저 버전 "10" 이 등장하면 "3" 보다 앞서므로 그때 손봐야 한다.)
    if (!versionMap.isEmpty())
        return std::prev(versionMap.constEnd()).value();

    return defaultPricing;
}
