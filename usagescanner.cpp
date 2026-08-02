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

// 웹 검색 과금: $10 / 1,000회.
constexpr double WEB_SEARCH_COST = 10.0 / 1000.0;

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
    qRegisterMetaType<ScanResult>("ScanResult");

    // 학습된 계수가 주입되기 전까지는 플랜 한도에서 유도한 prior 로 동작한다.
    // prior 는 예전 하드코딩 공식과 동일하므로 첫 실행 화면이 달라지지 않는다.
    m_calibration = UsageCalibrator::priorsFor(planLimit5h(), planLimit7d());

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

void UsageScanner::setCalibration(const CalibrationSet &calibration)
{
    m_calibration = calibration;
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
    // Windows 에서 fileChanged 이벤트 후 파일이 watch list 에서 제거될 수 있어 재등록.
    // 단, '삭제'된 경우에도 같은 시그널이 오므로 존재 확인이 필요하다. 없는 경로에
    // addPath 를 부르면 실패하면서 Qt 경고만 남는다(삭제된 세션마다 반복 발생).
    if (!path.isEmpty() && QFile::exists(path))
        m_watcher->addPath(path);
    emit activityDetected();    // 즉시 알림 (투명도 제어용)
    m_debounceTimer->start();   // 타이머 리셋: DEBOUNCE_MS 후 스캔
}

void UsageScanner::refreshWatchList()
{
    const QString projectsDir = CredentialsReader::claudeDir() + "/projects";

    // 감시 대상은 readRecords 가 읽는 범위와 정확히 같아야 한다.
    // 예전에는 projects/<프로젝트> 한 단계만 훑었는데, 서브에이전트 로그는
    // projects/<프로젝트>/<세션UUID>/subagents/*.jsonl 에 쌓인다.
    // 읽기는 되지만 감시가 안 돼서 서브에이전트 사용량이 최대 5분(watchTimer
    // 주기)까지 화면에 안 나타났다. 이제 하위 디렉터리를 전부 따라간다.
    QSet<QString> wantDirs{projectsDir};
    QDirIterator dirs(projectsDir, QDir::Dirs | QDir::NoDotAndDotDot,
                      QDirIterator::Subdirectories);
    while (dirs.hasNext())
        wantDirs.insert(dirs.next());

    QSet<QString> wantFiles;
    QDirIterator files(projectsDir, {"*.jsonl"}, QDir::Files,
                       QDirIterator::Subdirectories);
    while (files.hasNext())
        wantFiles.insert(files.next());

    // 예전에는 추가만 하고 제거를 하지 않았다. 프로젝트를 지우거나 세션 로그가
    // 정리돼도 m_watcher 와 m_watchedFiles 에 경로가 영원히 남아, 장기 실행 시
    // 감시 목록이 단조 증가했다. QFileSystemWatcher 는 경로마다 OS 핸들을
    // 소비하므로 한도를 넘기면 '새' 파일 감시가 조용히 실패하기 시작한다.
    // 이제 존재하는 경로 집합과 양방향으로 동기화한다.
    auto sync = [this](const QStringList &watched, const QSet<QString> &want) {
        QStringList stale;
        for (const QString &path : watched) {
            if (!want.contains(path))
                stale.append(path);
        }
        if (!stale.isEmpty())
            m_watcher->removePaths(stale);

        const QSet<QString> current(watched.constBegin(), watched.constEnd());
        QStringList fresh;
        for (const QString &path : want) {
            if (!current.contains(path))
                fresh.append(path);
        }
        if (!fresh.isEmpty())
            m_watcher->addPaths(fresh);
    };

    sync(m_watcher->directories(), wantDirs);
    sync(m_watcher->files(),       wantFiles);
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
    const QDateTime      hint5h      = m_lastKnownReset5h;
    const QDateTime      hint7d      = m_lastKnownReset7d;
    const QDateTime      deltaStart  = m_deltaStart;
    const CalibrationSet calibration = m_calibration;

    auto *watcher = new QFutureWatcher<ScanResult>(this);

    connect(watcher, &QFutureWatcher<ScanResult>::finished, this, [this, watcher]() {
        m_scanPending = false;
        const ScanResult result = watcher->result();
        watcher->deleteLater();

        emit localUsageUpdated(result);

        if (m_rescanQueued) {
            m_rescanQueued = false;
            requestScan();
        }
    });

    watcher->setFuture(QtConcurrent::run([hint5h, hint7d, deltaStart, calibration]() {
        return scanLocal(deltaStart, hint5h, hint7d, calibration);
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

double UsageScanner::costOf(const TokenRecord &r)
{
    ModelPricing p = getPricingForModel(r.model);
    if (r.fastMode && p.fastMultiplier > 1.0) {
        p.inputRate        *= p.fastMultiplier;
        p.outputRate       *= p.fastMultiplier;
        p.cacheWriteRate   *= p.fastMultiplier;
        p.cacheWrite1hRate *= p.fastMultiplier;
        p.cacheReadRate    *= p.fastMultiplier;
    }

    // 1시간 캐시는 5분 캐시의 1.6배다. Claude Code 는 1시간 캐시를 주로 쓰므로
    // 둘을 뭉뚱그리면 캐시 쓰기 비용이 구조적으로 과소 계산된다.
    const qint64 write1h = qBound<qint64>(0, r.cacheWrite1h, r.cacheWrite);
    const qint64 write5m = r.cacheWrite - write1h;

    return (r.input     * p.inputRate
          + r.output    * p.outputRate
          + write5m     * p.cacheWriteRate
          + write1h     * p.cacheWrite1hRate
          + r.cacheRead * p.cacheReadRate) / 1'000'000.0
         + r.webSearches * WEB_SEARCH_COST;
}

ScanResult UsageScanner::aggregate(const QVector<TokenRecord> &records,
                                   const QDateTime &nowUtc,
                                   const QDateTime &deltaStartUtc,
                                   const QDateTime &reset5h,
                                   const QDateTime &reset7d,
                                   const CalibrationSet &calibration)
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

    // 할당량은 더 이상 "가중 토큰 합 ÷ 하드코딩 한도"로 구하지 않는다.
    // 계열·종류별로 토큰을 나눠 담고(특징벡터), 학습된 계수로 비율을 만든다.
    UsageFeatures full5h, full7d, full7dSonnet;
    UsageFeatures delta5h, delta7d, delta7dSonnet;
    double fullCost = 0.0, deltaCost = 0.0;

    auto accumulate = [](UsageFeatures &f, const TokenRecord &r, Calib::Family fam) {
        f.add(fam, Calib::Input,      r.input);
        f.add(fam, Calib::Output,     r.output);
        f.add(fam, Calib::CacheWrite, r.cacheWrite);
        f.add(fam, Calib::CacheRead,  r.cacheRead);
    };

    // 파일을 두 번 읽는 대신 한 벌의 레코드로 full/delta 를 동시에 누산한다.
    for (const TokenRecord &r : records) {
        const Calib::Family fam = Calib::familyOf(r.model);
        const bool isSonnet     = (fam == Calib::Sonnet);
        const double cost       = costOf(r);

        if (r.ts >= full5hStart) accumulate(full5h, r, fam);
        if (r.ts >= full7dStart) {
            accumulate(full7d, r, fam);
            if (isSonnet) accumulate(full7dSonnet, r, fam);
        }
        if (r.ts >= billing) fullCost += cost;

        if (hasDelta) {
            if (r.ts >= delta5hStart) accumulate(delta5h, r, fam);
            if (r.ts >= delta7dStart) {
                accumulate(delta7d, r, fam);
                if (isSonnet) accumulate(delta7dSonnet, r, fam);
            }
            // 추가 크레딧은 월 단위라 5h 리셋과 무관하게 deltaStart 를 기준으로 한다.
            // delta5hStart 를 쓰면 5h 리셋이 낄 때마다 그 사이 비용이 통째로 사라진다.
            if (r.ts >= deltaStart && r.ts >= billing) deltaCost += cost;
        }
    }

    auto build = [&](const UsageFeatures &f5h, const UsageFeatures &f7d,
                     const UsageFeatures &f7dSonnet, double credits) {
        UsageData d;
        d.fromApi   = false;
        d.fetchedAt = QDateTime::currentDateTime();

        // extraUsage.enabled 는 API 만이 켤 수 있다. 스캐너가 켜 버리면 한도를
        // 모르는 채로 "추가 결제 크레딧 $x / $0.00" 패널이 떠 버린다.
        // 비용만 채우고, 표시 여부는 API 값을 가진 쪽(TrayApp)에 맡긴다.
        d.extraUsage.enabled     = false;
        d.extraUsage.usedCredits = credits;

        auto fill = [](QuotaInfo &q, const UsageFeatures &f,
                       const QuotaCoefficients &c, const QDateTime &resetsAt) {
            if (!c.isValid() && f.isEmpty())
                return;                       // 계수도 없고 토큰도 없으면 표시할 게 없다
            q.rawTokens   = f.total();        // 표시용이 아니라 디버그·병합 참고용
            q.limitTokens = 0;                // 한도 개념은 계수에 흡수됐다
            q.resetsAt    = resetsAt;
            q.valid       = true;
            q.utilization = qBound(0.0, c.predict(f), 1.0);
        };

        fill(d.fiveHour,       f5h,       calibration.fiveHour,       next5h);
        fill(d.sevenDay,       f7d,       calibration.sevenDay,       next7d);
        fill(d.sevenDaySonnet, f7dSonnet, calibration.sevenDaySonnet, next7d);
        return d;
    };

    ScanResult result;
    result.hasDelta             = hasDelta;
    result.full                 = build(full5h, full7d, full7dSonnet, fullCost);
    result.delta                = hasDelta
        ? build(delta5h, delta7d, delta7dSonnet, deltaCost)
        : result.full;
    result.full5hFeatures       = full5h;
    result.full7dFeatures       = full7d;
    result.full7dSonnetFeatures = full7dSonnet;
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

            // ⚠ usage.iterations[] 는 폴백/재시도 등 시도별 내역이며 같은 토큰
            // 필드를 그대로 반복한다(실제 로그에서 input_tokens 가 정확히 2배로
            // 잡힌다). usage 최상위 값이 이미 그 시도들의 합계이므로
            // iterations 를 재귀 합산하면 즉시 2배 이상으로 부풀어 오른다.
            // 여기서는 반드시 최상위 값만 읽는다.
            TokenRecord rec;
            rec.ts         = ts;
            rec.model      = model;
            rec.input      = usage["input_tokens"].toVariant().toLongLong();
            rec.output     = usage["output_tokens"].toVariant().toLongLong();
            rec.cacheWrite = usage["cache_creation_input_tokens"].toVariant().toLongLong();
            rec.cacheRead  = usage["cache_read_input_tokens"].toVariant().toLongLong();

            // 캐시 쓰기 요율은 5분(x1.25)과 1시간(x2)이 다르다.
            // cache_creation 이 있으면 1시간분을 분리하고, 없는 옛 로그는
            // cacheWrite1h == 0 이라 전량 5분 요율로 계산된다.
            const QJsonObject cacheCreation = usage["cache_creation"].toObject();
            rec.cacheWrite1h =
                cacheCreation["ephemeral_1h_input_tokens"].toVariant().toLongLong();

            rec.webSearches =
                usage["server_tool_use"].toObject()["web_search_requests"]
                    .toVariant().toLongLong();
            rec.fastMode = (usage["speed"].toString() == "fast");

            records.append(rec);
        }
    }

    if (recentModelOut)
        *recentModelOut = latestModel;
    return records;
}

// ── 스캔 오케스트레이션 (static: 멤버 변수 미접근 → 스레드 안전) ─────────────────

ScanResult UsageScanner::scanLocal(const QDateTime &deltaStartUtc,
                                   const QDateTime &reset5h,
                                   const QDateTime &reset7d,
                                   const CalibrationSet &calibration)
{
    const QDateTime now      = QDateTime::currentDateTimeUtc();
    const QDateTime earliest = earliestRelevant(now, deltaStartUtc, reset7d);

    QString recentModel;
    const QVector<TokenRecord> records =
        readRecords(CredentialsReader::claudeDir() + "/projects", earliest, &recentModel);

    ScanResult result = aggregate(records, now, deltaStartUtc, reset5h, reset7d,
                                  calibration);
    result.full.recentModel  = recentModel;
    result.delta.recentModel = recentModel;

    qDebug() << "[UsageScanner] plan=" << CredentialsReader::subscriptionType()
             << "records=" << records.size()
             << "full5h%=" << qRound(result.full.fiveHour.utilization * 100.0)
             << "full7d%=" << qRound(result.full.sevenDay.utilization * 100.0)
             << "delta5h%=" << qRound(result.delta.fiveHour.utilization * 100.0)
             << "calibSamples5h=" << calibration.fiveHour.samples
             << "calibSamples7d=" << calibration.sevenDay.samples
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
                // 1시간 캐시 요율이 없는 옛 파일이면 5분 요율로 폴백한다
                // (예전과 같은 동작 = 과소 계산이지만 크래시보다는 낫다).
                p.cacheWrite1hRate =
                    rateObj["cache_write_1h_rate"].toDouble(p.cacheWriteRate);
                // 값이 없으면 1.0 = fast mode 미지원 모델.
                p.fastMultiplier = rateObj["fast_multiplier"].toDouble(1.0);
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
    defaultPricing.inputRate        = 3.00;
    defaultPricing.outputRate       = 15.00;
    defaultPricing.cacheWriteRate   = 3.75;
    defaultPricing.cacheWrite1hRate = 6.00;
    defaultPricing.cacheReadRate    = 0.30;

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
