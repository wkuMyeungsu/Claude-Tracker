// ClaudeTray 의 시간·집계·병합 로직 단위 테스트.
//
// 여기 담긴 것들은 모두 순수 함수(또는 디렉터리를 주입받는 함수)라 GUI 도
// 네트워크도 자격 증명도 필요 없다. 시간 의존 분기가 많아 회귀가 나기 쉬운
// 부분이므로 '지금 시각'은 항상 인자로 고정해 넣는다.

#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include <QTimeZone>

#include "usageapiclient.h"
#include "usagemerge.h"
#include "usagescanner.h"

namespace {

constexpr qint64 SECS_5H = 5LL * 3600;
constexpr qint64 SECS_7D = 7LL * 24 * 3600;

QDateTime utc(const QString &iso)
{
    QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    dt.setTimeZone(QTimeZone::UTC);
    return dt;
}

TokenRecord rec(const QDateTime &ts, qint64 in, qint64 out,
                const QString &model = "claude-sonnet-5",
                qint64 cacheWrite = 0, qint64 cacheRead = 0)
{
    return TokenRecord{ts, model, in, out, cacheWrite, cacheRead};
}

// JSONL 한 줄을 만든다.
QByteArray jsonlLine(const QString &requestId, const QString &timestamp,
                     const QString &model, qint64 in, qint64 out,
                     qint64 cacheWrite = 0, qint64 cacheRead = 0)
{
    return QString(R"({"type":"assistant","requestId":"%1","uuid":"u-%1",)"
                   R"("timestamp":"%2","message":{"model":"%3","usage":)"
                   R"({"input_tokens":%4,"output_tokens":%5,)"
                   R"("cache_creation_input_tokens":%6,)"
                   R"("cache_read_input_tokens":%7}}})"
                   "\n")
        .arg(requestId, timestamp, model)
        .arg(in).arg(out).arg(cacheWrite).arg(cacheRead)
        .toUtf8();
}

} // namespace

class TestUsageLogic : public QObject
{
    Q_OBJECT

private slots:
    // ── estimateNextReset ────────────────────────────────────────────────────
    void estimateNextReset_futureIsKept();
    void estimateNextReset_pastRollsForward();
    void estimateNextReset_invalidInput();

    // ── earliestRelevant ─────────────────────────────────────────────────────
    void earliestRelevant_coversBillingCycle();
    void earliestRelevant_coversLongOfflineDelta();

    // ── aggregate ────────────────────────────────────────────────────────────
    void aggregate_windowBoundaries();
    void aggregate_cacheReadIsDiscounted();
    void aggregate_utilizationClampedAtOne();
    void aggregate_deltaExcludesPreResetTokens();
    void aggregate_extraCreditSurvivesFiveHourReset();
    void aggregate_scannerNeverEnablesExtraCredit();
    void aggregate_noDeltaMeansDeltaEqualsFull();

    // ── getPricingForModel ───────────────────────────────────────────────────
    void pricing_familyAndVersion_data();
    void pricing_familyAndVersion();
    void pricing_dateSuffixIsNotAVersion();

    // ── readRecords ──────────────────────────────────────────────────────────
    void readRecords_parsesAndDedupes();
    void readRecords_convertsOffsetTimestampsToUtc();
    void readRecords_filtersOldRecordsButKeepsRecentModel();

    // ── UsageMerge ───────────────────────────────────────────────────────────
    void merge_addsDeltaOnTopOfApi();
    void merge_dropsStaleApiTokensAfterReset();
    void merge_refreshesResetsAtAfterReset();
    void merge_extraCreditOnlyWhenApiEnabledIt();

    // ── 버전 비교 ─────────────────────────────────────────────────────────────
    void versionCompare_data();
    void versionCompare();
};

// ─────────────────────────────────────────────────────────────────────────────

void TestUsageLogic::estimateNextReset_futureIsKept()
{
    const QDateTime now  = utc("2026-08-02T12:00:00");
    const QDateTime next = utc("2026-08-02T15:00:00");
    QCOMPARE(UsageScanner::estimateNextReset(next, SECS_5H, now), next);
}

void TestUsageLogic::estimateNextReset_pastRollsForward()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    // 06:00 리셋 + 5h 주기 → 11:00 도 지났으므로 16:00 이 다음 리셋
    QCOMPARE(UsageScanner::estimateNextReset(utc("2026-08-02T06:00:00"), SECS_5H, now),
             utc("2026-08-02T16:00:00"));
    // 정확히 한 주기 전이면 바로 다음 주기
    QCOMPARE(UsageScanner::estimateNextReset(utc("2026-08-02T07:00:00"), SECS_5H, now),
             utc("2026-08-02T12:00:00").addSecs(SECS_5H));
}

void TestUsageLogic::estimateNextReset_invalidInput()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    QVERIFY(!UsageScanner::estimateNextReset(QDateTime(), SECS_5H, now).isValid());
    QVERIFY(!UsageScanner::estimateNextReset(utc("2026-08-02T06:00:00"), 0, now).isValid());
}

// ─────────────────────────────────────────────────────────────────────────────

void TestUsageLogic::earliestRelevant_coversBillingCycle()
{
    // 월 하순이면 7d 윈도우(now-7d)보다 '이번 달 1일'이 더 이르다.
    // 추가 크레딧이 월 누적이므로 1일까지 읽어야 한다.
    const QDateTime now = utc("2026-08-28T12:00:00");
    const QDateTime earliest =
        UsageScanner::earliestRelevant(now, QDateTime(), QDateTime());
    QCOMPARE(earliest, utc("2026-08-01T00:00:00"));
}

void TestUsageLogic::earliestRelevant_coversLongOfflineDelta()
{
    // 오래 오프라인이면 deltaStart 가 7d 윈도우보다도 이르다. 그때는
    // deltaStart 까지 거슬러 올라가야 증분이 누락되지 않는다.
    const QDateTime now        = utc("2026-08-20T12:00:00");
    const QDateTime deltaStart = utc("2026-07-25T00:00:00");
    const QDateTime earliest =
        UsageScanner::earliestRelevant(now, deltaStart, QDateTime());
    QCOMPARE(earliest, deltaStart);
}

// ─────────────────────────────────────────────────────────────────────────────

void TestUsageLogic::aggregate_windowBoundaries()
{
    const QDateTime now     = utc("2026-08-02T12:00:00");
    const QDateTime reset5h = utc("2026-08-02T15:00:00");   // 윈도우: 10:00~15:00
    const QDateTime reset7d = utc("2026-08-05T00:00:00");   // 윈도우: 07-29~08-05

    const QVector<TokenRecord> records{
        rec(utc("2026-07-28T12:00:00"), 1000, 0),   // 7d 밖
        rec(utc("2026-07-30T12:00:00"), 200, 0),    // 7d 안, 5h 밖
        rec(utc("2026-08-02T09:00:00"), 30, 0),     // 7d 안, 5h 밖
        rec(utc("2026-08-02T11:00:00"), 4, 1),      // 둘 다 안
    };

    const ScanResult r = UsageScanner::aggregate(records, now, QDateTime(),
                                                 reset5h, reset7d,
                                                 1'000'000, 10'000'000);
    QCOMPARE(r.full.fiveHour.rawTokens, 5);
    QCOMPARE(r.full.sevenDay.rawTokens, 235);
    QCOMPARE(r.full.fiveHour.resetsAt, reset5h);
    QCOMPARE(r.full.sevenDay.resetsAt, reset7d);
    QVERIFY(r.full.fiveHour.valid);
}

void TestUsageLogic::aggregate_cacheReadIsDiscounted()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    const QVector<TokenRecord> records{
        rec(utc("2026-08-02T11:00:00"), 100, 50, "claude-sonnet-5", 10, 1000),
    };
    const ScanResult r = UsageScanner::aggregate(records, now, QDateTime(),
                                                 QDateTime(), QDateTime(),
                                                 1'000'000, 10'000'000);
    // 100 + 50 + 10 + round(1000 * 0.1) = 260
    QCOMPARE(r.full.fiveHour.rawTokens, 260);
}

void TestUsageLogic::aggregate_utilizationClampedAtOne()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    const QVector<TokenRecord> records{
        rec(utc("2026-08-02T11:00:00"), 5'000'000, 0),
    };
    const ScanResult r = UsageScanner::aggregate(records, now, QDateTime(),
                                                 QDateTime(), QDateTime(),
                                                 1'000'000, 10'000'000);
    QCOMPARE(r.full.fiveHour.utilization, 1.0);
}

void TestUsageLogic::aggregate_deltaExcludesPreResetTokens()
{
    // 델타 구간(10:00~) 안에서 11:00 에 5h 리셋이 일어났다면, 델타는 11:00
    // 이후만 세야 한다. 리셋 전 토큰까지 넣으면 mergeWithLastApi 에서
    // 구 API 값과 겹쳐 이중 계산된다.
    const QDateTime now        = utc("2026-08-02T12:00:00");
    const QDateTime deltaStart = utc("2026-08-02T10:00:00");
    const QDateTime reset5h    = utc("2026-08-02T11:00:00");   // 이미 지남

    const QVector<TokenRecord> records{
        rec(utc("2026-08-02T10:30:00"), 100, 0),   // 리셋 전
        rec(utc("2026-08-02T11:30:00"),   7, 0),   // 리셋 후
    };

    const ScanResult r = UsageScanner::aggregate(records, now, deltaStart,
                                                 reset5h, QDateTime(),
                                                 1'000'000, 10'000'000);
    QVERIFY(r.hasDelta);
    QCOMPARE(r.delta.fiveHour.rawTokens, 7);
}

void TestUsageLogic::aggregate_extraCreditSurvivesFiveHourReset()
{
    // 추가 결제 크레딧은 '월' 단위 누적이다. 5h 리셋 때문에 잘라내면
    // 리셋 직전에 쓴 비용이 통째로 사라진다. (예전 동작의 회귀 방지)
    const QDateTime now        = utc("2026-08-02T12:00:00");
    const QDateTime deltaStart = utc("2026-08-02T10:00:00");
    const QDateTime reset5h    = utc("2026-08-02T11:00:00");

    const QVector<TokenRecord> records{
        rec(utc("2026-08-02T10:30:00"), 1'000'000, 0),   // 리셋 전
        rec(utc("2026-08-02T11:30:00"), 1'000'000, 0),   // 리셋 후
    };

    const ScanResult r = UsageScanner::aggregate(records, now, deltaStart,
                                                 reset5h, QDateTime(),
                                                 1'000'000'000, 10'000'000'000);
    // sonnet-5 input = $3/1M → 두 건 모두 세어 $6.00 이어야 한다.
    QVERIFY2(r.delta.extraUsage.usedCredits > 5.99 &&
             r.delta.extraUsage.usedCredits < 6.01,
             qPrintable(QString("usedCredits=%1").arg(r.delta.extraUsage.usedCredits)));
}

void TestUsageLogic::aggregate_scannerNeverEnablesExtraCredit()
{
    // 스캐너는 한도($)를 알 수 없다. enabled 를 켜면 "$x / $0.00" 패널이 뜬다.
    const QDateTime now = utc("2026-08-02T12:00:00");
    const QVector<TokenRecord> records{ rec(utc("2026-08-02T11:00:00"), 100, 50) };

    const ScanResult r = UsageScanner::aggregate(records, now, QDateTime(),
                                                 QDateTime(), QDateTime(),
                                                 1'000'000, 10'000'000);
    QVERIFY(!r.full.extraUsage.enabled);
    QVERIFY(r.full.extraUsage.usedCredits > 0.0);   // 비용 자체는 채워야 한다
}

void TestUsageLogic::aggregate_noDeltaMeansDeltaEqualsFull()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    const QVector<TokenRecord> records{ rec(utc("2026-08-02T11:00:00"), 100, 50) };

    const ScanResult r = UsageScanner::aggregate(records, now, QDateTime(),
                                                 QDateTime(), QDateTime(),
                                                 1'000'000, 10'000'000);
    QVERIFY(!r.hasDelta);
    QCOMPARE(r.delta.fiveHour.rawTokens, r.full.fiveHour.rawTokens);
}

// ─────────────────────────────────────────────────────────────────────────────

void TestUsageLogic::pricing_familyAndVersion_data()
{
    QTest::addColumn<QString>("model");
    QTest::addColumn<double>("expectedInputRate");

    QTest::newRow("opus-5")        << "claude-opus-5"           <<  5.00;
    QTest::newRow("opus-4.8")      << "claude-opus-4-8"         <<  5.00;
    QTest::newRow("opus-4.5")      << "claude-opus-4-5"         <<  5.00;
    // Opus 4.1 이하는 요율이 3배다. 4.5 와 같은 값으로 뭉개지면 안 된다.
    QTest::newRow("opus-4.1")      << "claude-opus-4-1"         << 15.00;
    QTest::newRow("sonnet-5")      << "claude-sonnet-5"         <<  3.00;
    QTest::newRow("haiku-3")       << "claude-haiku-3"          <<  0.25;
    QTest::newRow("haiku-3.5")     << "claude-haiku-3-5"        <<  0.80;
    QTest::newRow("haiku-4.5")     << "claude-haiku-4-5"        <<  1.00;
    QTest::newRow("fable-5")       << "claude-fable-5"          << 10.00;
    // 계열을 못 찾으면 sonnet 5 로 폴백
    QTest::newRow("unknown-family") << "some-other-model"       <<  3.00;
    // 계열은 알지만 버전을 모르면 그 계열의 최신 요율로 폴백
    QTest::newRow("opus-unknown-ver")  << "claude-opus-9"       <<  5.00;
    QTest::newRow("haiku-unknown-ver") << "claude-haiku-9"      <<  1.00;
}

void TestUsageLogic::pricing_familyAndVersion()
{
    QFETCH(QString, model);
    QFETCH(double, expectedInputRate);
    QCOMPARE(UsageScanner::getPricingForModel(model).inputRate, expectedInputRate);
}

void TestUsageLogic::pricing_dateSuffixIsNotAVersion()
{
    // "20250805" 안의 '5' 가 버전 키 "5" 로 오인되면 Opus 4.1($15) 이
    // Opus 5 요율($5, 1/3 가격)로 계산된다.
    QCOMPARE(UsageScanner::getPricingForModel("claude-opus-4-1-20250805").inputRate, 15.00);
    // 날짜를 뗀 뒤 버전이 정확히 일치해야 한다(4.8 이 4.1 로 새지 않는지).
    QCOMPARE(UsageScanner::getPricingForModel("claude-opus-4-8-20260115").inputRate, 5.00);
    // 옛 명명 규칙(버전이 계열보다 앞)도 처리되어야 한다.
    QCOMPARE(UsageScanner::getPricingForModel("claude-3-haiku-20240307").inputRate, 0.25);
    QCOMPARE(UsageScanner::getPricingForModel("claude-3-5-haiku-20241022").inputRate, 0.80);
}

// ─────────────────────────────────────────────────────────────────────────────

void TestUsageLogic::readRecords_parsesAndDedupes()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(QDir(tmp.path()).mkpath("proj-a"));

    QFile f(tmp.path() + "/proj-a/session.jsonl");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(jsonlLine("req_1", "2026-08-02T11:00:00.000Z", "claude-sonnet-5", 100, 50, 10, 1000));
    // 같은 응답의 두 번째 줄(thinking/tool_use) — requestId 가 같으니 무시해야 한다
    f.write(jsonlLine("req_1", "2026-08-02T11:00:01.000Z", "claude-sonnet-5", 100, 50, 10, 1000));
    f.write(jsonlLine("req_2", "2026-08-02T11:05:00.000Z", "claude-opus-5", 7, 3));
    // assistant 가 아닌 줄은 무시
    f.write(QByteArray(R"({"type":"user","timestamp":"2026-08-02T11:06:00.000Z"})" "\n"));
    f.write("\n");   // 빈 줄
    f.close();

    QString recentModel;
    const QVector<TokenRecord> records =
        UsageScanner::readRecords(tmp.path(), QDateTime(), &recentModel);

    QCOMPARE(records.size(), 2);
    QCOMPARE(recentModel, QString("claude-opus-5"));

    QCOMPARE(records[0].input, 100);
    QCOMPARE(records[0].output, 50);
    QCOMPARE(records[0].cacheWrite, 10);
    QCOMPARE(records[0].cacheRead, 1000);
    QCOMPARE(records[0].ts, utc("2026-08-02T11:00:00"));
}

void TestUsageLogic::readRecords_convertsOffsetTimestampsToUtc()
{
    // setTimeZone() 은 시각을 재해석하므로 +09:00 이 붙으면 9시간 어긋난다.
    // toUTC() 로 '변환'해야 한다.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(QDir(tmp.path()).mkpath("proj-a"));

    QFile f(tmp.path() + "/proj-a/session.jsonl");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(jsonlLine("req_kst", "2026-08-02T19:00:00+09:00", "claude-sonnet-5", 1, 1));
    f.close();

    const QVector<TokenRecord> records =
        UsageScanner::readRecords(tmp.path(), QDateTime(), nullptr);

    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].ts, utc("2026-08-02T10:00:00"));
}

void TestUsageLogic::readRecords_filtersOldRecordsButKeepsRecentModel()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(QDir(tmp.path()).mkpath("proj-a/nested"));

    QFile f(tmp.path() + "/proj-a/nested/session.jsonl");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(jsonlLine("old", "2020-01-01T00:00:00.000Z", "claude-opus-3", 999, 999));
    f.write(jsonlLine("new", "2026-08-02T11:00:00.000Z", "claude-sonnet-5", 1, 1));
    f.close();

    QString recentModel;
    const QVector<TokenRecord> records =
        UsageScanner::readRecords(tmp.path(), utc("2026-08-01T00:00:00"), &recentModel);

    // 오래된 레코드는 담기지 않지만(메모리 절감), 최근 모델 판정에는 참여한다
    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].input, 1);
    QCOMPARE(recentModel, QString("claude-sonnet-5"));
}

// ─────────────────────────────────────────────────────────────────────────────

namespace {
UsageData makeApiData(double util5h, const QDateTime &reset5h,
                      double util7d = 0.0, const QDateTime &reset7d = {})
{
    UsageData d;
    d.fromApi = true;
    d.fiveHour.valid       = true;
    d.fiveHour.utilization = util5h;
    d.fiveHour.resetsAt    = reset5h;
    d.sevenDay.valid       = true;
    d.sevenDay.utilization = util7d;
    d.sevenDay.resetsAt    = reset7d;
    return d;
}
}

void TestUsageLogic::merge_addsDeltaOnTopOfApi()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    // API: 5h 50% (한도 1,000,000 → 500,000 토큰), 리셋은 미래
    const UsageData api = makeApiData(0.5, utc("2026-08-02T15:00:00"));

    UsageData delta;
    delta.fiveHour.rawTokens = 100'000;

    const UsageData merged =
        UsageMerge::mergeWithLastApi(api, delta, 1'000'000, 10'000'000, now);

    QCOMPARE(merged.fiveHour.rawTokens, 600'000);
    QCOMPARE(merged.fiveHour.utilization, 0.6);
    QVERIFY(!merged.fromApi);
}

void TestUsageLogic::merge_dropsStaleApiTokensAfterReset()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    // API 가 알려준 리셋 시각이 이미 지났다 → 구 API 사용량은 버려야 한다
    const UsageData api = makeApiData(0.9, utc("2026-08-02T11:00:00"));

    UsageData delta;
    delta.fiveHour.rawTokens = 20'000;

    const UsageData merged =
        UsageMerge::mergeWithLastApi(api, delta, 1'000'000, 10'000'000, now);

    QCOMPARE(merged.fiveHour.rawTokens, 20'000);   // 900,000 이 아니라 델타만
    QCOMPARE(merged.fiveHour.utilization, 0.02);
}

void TestUsageLogic::merge_refreshesResetsAtAfterReset()
{
    // 리셋이 지난 뒤에도 옛 resetsAt 가 그대로 남으면 카운트다운이
    // "곧 초기화됩니다" 에서 멈춘다. 다음 주기로 갱신되어야 한다.
    const QDateTime now = utc("2026-08-02T12:00:00");
    const UsageData api = makeApiData(0.9, utc("2026-08-02T11:00:00"),
                                      0.3, utc("2026-07-30T00:00:00"));

    const UsageData merged =
        UsageMerge::mergeWithLastApi(api, UsageData(), 1'000'000, 10'000'000, now);

    QCOMPARE(merged.fiveHour.resetsAt, utc("2026-08-02T16:00:00"));
    QVERIFY(merged.fiveHour.resetsAt > now);
    QCOMPARE(merged.sevenDay.resetsAt, utc("2026-08-06T00:00:00"));
    QVERIFY(merged.sevenDay.resetsAt > now);
}

void TestUsageLogic::merge_extraCreditOnlyWhenApiEnabledIt()
{
    const QDateTime now = utc("2026-08-02T12:00:00");

    UsageData delta;
    delta.extraUsage.usedCredits = 1.50;   // 스캐너는 enabled 를 켜지 않는다

    // 1) API 가 꺼져 있으면 델타를 더하지 않고 꺼진 채로 둔다
    UsageData apiOff = makeApiData(0.1, utc("2026-08-02T15:00:00"));
    apiOff.extraUsage.enabled = false;
    const UsageData mergedOff =
        UsageMerge::mergeWithLastApi(apiOff, delta, 1'000'000, 10'000'000, now);
    QVERIFY(!mergedOff.extraUsage.enabled);
    QCOMPARE(mergedOff.extraUsage.usedCredits, 0.0);

    // 2) API 가 켜져 있으면 API 값 + 델타
    UsageData apiOn = makeApiData(0.1, utc("2026-08-02T15:00:00"));
    apiOn.extraUsage.enabled      = true;
    apiOn.extraUsage.usedCredits  = 8.50;
    apiOn.extraUsage.limitDollars = 20.00;
    const UsageData mergedOn =
        UsageMerge::mergeWithLastApi(apiOn, delta, 1'000'000, 10'000'000, now);
    QVERIFY(mergedOn.extraUsage.enabled);
    QCOMPARE(mergedOn.extraUsage.usedCredits, 10.00);
    QCOMPARE(mergedOn.extraUsage.utilization, 0.5);
}

// ─────────────────────────────────────────────────────────────────────────────

void TestUsageLogic::versionCompare_data()
{
    QTest::addColumn<QString>("candidate");
    QTest::addColumn<QString>("current");
    QTest::addColumn<bool>("expected");

    QTest::newRow("newer patch")   << "1.1.1" << "1.1.0" << true;
    QTest::newRow("newer minor")   << "1.2.0" << "1.1.9" << true;
    QTest::newRow("newer major")   << "2.0.0" << "1.9.9" << true;
    QTest::newRow("same")          << "1.1.0" << "1.1.0" << false;
    QTest::newRow("older")         << "1.0.9" << "1.1.0" << false;
    QTest::newRow("short form")    << "1.2"   << "1.1.0" << true;
    QTest::newRow("prerelease")    << "1.2.0-rc1" << "1.1.0" << true;
    QTest::newRow("prerelease eq") << "1.1.0-rc1" << "1.1.0" << false;
}

void TestUsageLogic::versionCompare()
{
    QFETCH(QString, candidate);
    QFETCH(QString, current);
    QFETCH(bool, expected);
    QCOMPARE(UsageApiClient::isNewerVersion(candidate, current), expected);
}

QTEST_GUILESS_MAIN(TestUsageLogic)
#include "tst_usagelogic.moc"
