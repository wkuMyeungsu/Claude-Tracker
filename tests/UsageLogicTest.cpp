// ClaudeTray 의 시간·집계·병합 로직 단위 테스트.
//
// 여기 담긴 것들은 모두 순수 함수(또는 디렉터리를 주입받는 함수)라 GUI 도
// 네트워크도 자격 증명도 필요 없다. 시간 의존 분기가 많아 회귀가 나기 쉬운
// 부분이므로 '지금 시각'은 항상 인자로 고정해 넣는다.

#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include <QTimeZone>

#include "HookBridge.h"
#include "UsageApiClient.h"
#include "QuotaCalibrator.h"
#include "UsageMerger.h"
#include "ModelPricing.h"
#include "SessionLogReader.h"
#include "UsageAggregator.h"

namespace {

constexpr qint64 SECS_5H = 5LL * 3600;
constexpr qint64 SECS_7D = 7LL * 24 * 3600;

QDateTime utc(const QString &iso)
{
    QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    dt.setTimeZone(QTimeZone::utc());
    return dt;
}

// 학습 전(prior) 계수. 예전의 "가중 토큰 / 플랜 한도" 공식과 동일한 값이라
// 기존 기대값을 그대로 쓸 수 있다.
CalibrationSet calib(qint64 limit5h = 1'000'000, qint64 limit7d = 10'000'000)
{
    return QuotaCalibrator::priorsFor(limit5h, limit7d);
}

TokenRecord rec(const QDateTime &ts, qint64 in, qint64 out,
                const QString &model = "claude-sonnet-5",
                qint64 cacheWrite = 0, qint64 cacheRead = 0)
{
    TokenRecord r;
    r.ts = ts; r.model = model;
    r.input = in; r.output = out;
    r.cacheWrite = cacheWrite; r.cacheRead = cacheRead;
    return r;
}

// JSONL 한 줄을 만든다. extraUsageFields 로 cache_creation / speed 등
// usage 하위 필드를 덧붙일 수 있다.
QByteArray jsonlLine(const QString &requestId, const QString &timestamp,
                     const QString &model, qint64 in, qint64 out,
                     qint64 cacheWrite = 0, qint64 cacheRead = 0,
                     const QString &extraUsageFields = {})
{
    return QString(R"({"type":"assistant","requestId":"%1","uuid":"u-%1",)"
                   R"("timestamp":"%2","message":{"model":"%3","usage":)"
                   R"({"input_tokens":%4,"output_tokens":%5,)"
                   R"("cache_creation_input_tokens":%6,)"
                   R"("cache_read_input_tokens":%7%8}}})"
                   "\n")
        .arg(requestId, timestamp, model)
        .arg(in).arg(out).arg(cacheWrite).arg(cacheRead)
        .arg(extraUsageFields)
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
    void aggregate_perModelFeaturesAreSeparated();

    // ── getPricingForModel / costOf ──────────────────────────────────────────
    void pricing_familyAndVersion_data();
    void pricing_familyAndVersion();
    void pricing_dateSuffixIsNotAVersion();
    void cost_oneHourCacheCostsMoreThanFiveMinute();
    void cost_fastModeDoublesOpusRates();
    void cost_webSearchIsBilled();

    // ── readRecords ──────────────────────────────────────────────────────────
    void readRecords_parsesAndDedupes();
    void readRecords_convertsOffsetTimestampsToUtc();
    void readRecords_filtersOldRecordsButKeepsRecentModel();
    void readRecords_parsesCacheCreationAndSpeed();
    void readRecords_ignoresIterationsArray();

    // ── HookBridge ───────────────────────────────────────────────────────────
    void hooks_installKeepsForeignSettingsAndHooks();
    void hooks_installIsIdempotent();
    void hooks_uninstallRestoresOriginal();

    // ── QuotaCalibrator ──────────────────────────────────────────────────────
    void calibrator_priorMatchesLegacyFormula();
    void calibrator_convergesTowardObservedUtilization();
    void calibrator_ignoresUnobservableSamples();
    void calibrator_learnsPerFamilyIndependently();
    void calibrator_learnsDownFasterThanUp();
    void calibrator_residualReportsUnexplainedUsage();
    void calibrator_neverPredictsOverFullQuota();
    void calibrator_revertsWhenWorseThanPrior();
    void calibrator_priorWeightsFamiliesByPrice();

    // ── 크레딧 보정 ──────────────────────────────────────────────────────────
    void creditCalib_convergesTowardObservedIncrement();
    void creditCalib_learnsDownFasterThanUp();
    void creditCalib_ignoresNoiseSizedIncrements();
    void creditCalib_revertsWhenWorseThanPrior();
    void creditCalib_scalesOnlyTheIncrementNotTheApiBase();

    // ── UsageMerger ───────────────────────────────────────────────────────────
    void merge_addsDeltaOnTopOfApi();
    void merge_clampsCombinedUtilizationAtOne();
    void merge_dropsStaleApiTokensAfterReset();
    void merge_refreshesResetsAtAfterReset();
    void merge_extraCreditOnlyWhenApiEnabledIt();
    void merge_extraCreditResetsOnMonthRollover();
    void merge_extraCreditKeepsBaselineWhenFetchedAtUnknown();

    // ── 추가 결제 크레딧 청구 비율 ────────────────────────────────────────────
    void charge_nothingWhileUnderQuota();
    void charge_onlyTheOveragePortion();
    void charge_weeklyQuotaAloneCanTriggerBilling();
    void charge_keepsPreResetOverageWhenWindowRolled();
    void charge_defaultsToFullWhenNoQuotaKnown();

    // ── 버전 비교 ─────────────────────────────────────────────────────────────
    void versionCompare_data();
    void versionCompare();
};

// ─────────────────────────────────────────────────────────────────────────────

void TestUsageLogic::estimateNextReset_futureIsKept()
{
    const QDateTime now  = utc("2026-08-02T12:00:00");
    const QDateTime next = utc("2026-08-02T15:00:00");
    QCOMPARE(UsageAggregator::estimateNextReset(next, SECS_5H, now), next);
}

void TestUsageLogic::estimateNextReset_pastRollsForward()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    // 06:00 리셋 + 5h 주기 → 11:00 도 지났으므로 16:00 이 다음 리셋
    QCOMPARE(UsageAggregator::estimateNextReset(utc("2026-08-02T06:00:00"), SECS_5H, now),
             utc("2026-08-02T16:00:00"));
    // 정확히 한 주기 전이면 바로 다음 주기
    QCOMPARE(UsageAggregator::estimateNextReset(utc("2026-08-02T07:00:00"), SECS_5H, now),
             utc("2026-08-02T12:00:00").addSecs(SECS_5H));
}

void TestUsageLogic::estimateNextReset_invalidInput()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    QVERIFY(!UsageAggregator::estimateNextReset(QDateTime(), SECS_5H, now).isValid());
    QVERIFY(!UsageAggregator::estimateNextReset(utc("2026-08-02T06:00:00"), 0, now).isValid());
}

// ─────────────────────────────────────────────────────────────────────────────

void TestUsageLogic::earliestRelevant_coversBillingCycle()
{
    // 월 하순이면 7d 윈도우(now-7d)보다 '이번 달 1일'이 더 이르다.
    // 추가 크레딧이 월 누적이므로 1일까지 읽어야 한다.
    const QDateTime now = utc("2026-08-28T12:00:00");
    const QDateTime earliest =
        UsageAggregator::earliestRelevant(now, QDateTime(), QDateTime());
    QCOMPARE(earliest, utc("2026-08-01T00:00:00"));
}

void TestUsageLogic::earliestRelevant_coversLongOfflineDelta()
{
    // 오래 오프라인이면 deltaStart 가 7d 윈도우보다도 이르다. 그때는
    // deltaStart 까지 거슬러 올라가야 증분이 누락되지 않는다.
    const QDateTime now        = utc("2026-08-20T12:00:00");
    const QDateTime deltaStart = utc("2026-07-25T00:00:00");
    const QDateTime earliest =
        UsageAggregator::earliestRelevant(now, deltaStart, QDateTime());
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

    const ScanResult r = UsageAggregator::aggregate(records, now, QDateTime(),
                                                 reset5h, reset7d,
                                                 calib());
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
    const ScanResult r = UsageAggregator::aggregate(records, now, QDateTime(),
                                                 QDateTime(), QDateTime(),
                                                 calib());
    // rawTokens 는 이제 가중치 없는 원시 합계다 (100+50+10+1000).
    QCOMPARE(r.full.fiveHour.rawTokens, 1160);
    // 할인은 utilization 쪽에서 일어난다.
    // prior 계수 = 1/1e6 (읽기만 0.1/1e6) → (100+50+10+100)/1e6
    QCOMPARE(r.full.fiveHour.utilization, 260.0 / 1'000'000.0);
}

void TestUsageLogic::aggregate_perModelFeaturesAreSeparated()
{
    // 보정기는 계열별로 계수를 따로 학습하므로 특징벡터도 계열별로 나뉘어야 한다.
    const QDateTime now = utc("2026-08-02T12:00:00");
    const QVector<TokenRecord> records{
        rec(utc("2026-08-02T11:00:00"), 100, 10, "claude-opus-5"),
        rec(utc("2026-08-02T11:10:00"),  70,  5, "claude-sonnet-5"),
        rec(utc("2026-08-02T11:20:00"),   3,  1, "claude-haiku-4-5"),
    };
    const ScanResult r = UsageAggregator::aggregate(records, now, QDateTime(),
                                                 QDateTime(), QDateTime(), calib());

    QCOMPARE(r.full5hFeatures.tokens[Calib::Opus][Calib::Input],   100);
    QCOMPARE(r.full5hFeatures.tokens[Calib::Sonnet][Calib::Input],  70);
    QCOMPARE(r.full5hFeatures.tokens[Calib::Haiku][Calib::Input],    3);
    QCOMPARE(r.full5hFeatures.tokens[Calib::Opus][Calib::Output],   10);

    // seven_day_sonnet 용 벡터에는 Sonnet 만 들어가야 한다.
    QCOMPARE(r.full7dSonnetFeatures.tokens[Calib::Sonnet][Calib::Input], 70);
    QCOMPARE(r.full7dSonnetFeatures.tokens[Calib::Opus][Calib::Input],    0);
    QCOMPARE(r.full7dSonnetFeatures.tokens[Calib::Haiku][Calib::Input],   0);
}

void TestUsageLogic::aggregate_utilizationClampedAtOne()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    const QVector<TokenRecord> records{
        rec(utc("2026-08-02T11:00:00"), 5'000'000, 0),
    };
    const ScanResult r = UsageAggregator::aggregate(records, now, QDateTime(),
                                                 QDateTime(), QDateTime(),
                                                 calib());
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

    const ScanResult r = UsageAggregator::aggregate(records, now, deltaStart,
                                                 reset5h, QDateTime(),
                                                 calib());
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

    const ScanResult r = UsageAggregator::aggregate(records, now, deltaStart,
                                                 reset5h, QDateTime(),
                                                 calib(1'000'000'000, 10'000'000'000));
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

    const ScanResult r = UsageAggregator::aggregate(records, now, QDateTime(),
                                                 QDateTime(), QDateTime(),
                                                 calib());
    QVERIFY(!r.full.extraUsage.enabled);
    QVERIFY(r.full.extraUsage.usedCredits > 0.0);   // 비용 자체는 채워야 한다
}

void TestUsageLogic::aggregate_noDeltaMeansDeltaEqualsFull()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    const QVector<TokenRecord> records{ rec(utc("2026-08-02T11:00:00"), 100, 50) };

    const ScanResult r = UsageAggregator::aggregate(records, now, QDateTime(),
                                                 QDateTime(), QDateTime(),
                                                 calib());
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
    QCOMPARE(ModelPricingTable::forModel(model).inputRate, expectedInputRate);
}

void TestUsageLogic::pricing_dateSuffixIsNotAVersion()
{
    // "20250805" 안의 '5' 가 버전 키 "5" 로 오인되면 Opus 4.1($15) 이
    // Opus 5 요율($5, 1/3 가격)로 계산된다.
    QCOMPARE(ModelPricingTable::forModel("claude-opus-4-1-20250805").inputRate, 15.00);
    // 날짜를 뗀 뒤 버전이 정확히 일치해야 한다(4.8 이 4.1 로 새지 않는지).
    QCOMPARE(ModelPricingTable::forModel("claude-opus-4-8-20260115").inputRate, 5.00);
    // 옛 명명 규칙(버전이 계열보다 앞)도 처리되어야 한다.
    QCOMPARE(ModelPricingTable::forModel("claude-3-haiku-20240307").inputRate, 0.25);
    QCOMPARE(ModelPricingTable::forModel("claude-3-5-haiku-20241022").inputRate, 0.80);
}

void TestUsageLogic::cost_oneHourCacheCostsMoreThanFiveMinute()
{
    // Claude Code 는 1시간 캐시를 주로 쓴다. 5분 요율(x1.25)로 뭉뚱그리면
    // 캐시 쓰기 비용이 구조적으로 과소 계산된다. Opus 5: 5m $6.25, 1h $10.
    TokenRecord r;
    r.model = "claude-opus-5";
    r.cacheWrite = 1'000'000;

    r.cacheWrite1h = 0;
    QCOMPARE(ModelPricingTable::costOf(r), 6.25);

    r.cacheWrite1h = 1'000'000;
    QCOMPARE(ModelPricingTable::costOf(r), 10.00);

    // 절반씩 섞이면 평균
    r.cacheWrite1h = 500'000;
    QCOMPARE(ModelPricingTable::costOf(r), (6.25 + 10.00) / 2.0);
}

void TestUsageLogic::cost_fastModeDoublesOpusRates()
{
    TokenRecord r;
    r.model = "claude-opus-5";
    r.input = 1'000'000;
    QCOMPARE(ModelPricingTable::costOf(r), 5.00);

    r.fastMode = true;
    QCOMPARE(ModelPricingTable::costOf(r), 10.00);   // fast mode: $10/MTok

    // fast mode 를 지원하지 않는 모델은 배수가 1.0 이라 값이 그대로다.
    TokenRecord s;
    s.model = "claude-sonnet-5";
    s.input = 1'000'000;
    s.fastMode = true;
    QCOMPARE(ModelPricingTable::costOf(s), 3.00);
}

void TestUsageLogic::cost_webSearchIsBilled()
{
    TokenRecord r;
    r.model = "claude-opus-5";
    r.webSearches = 3;                       // $10 / 1,000회
    QCOMPARE(ModelPricingTable::costOf(r), 0.03);
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
        SessionLogReader::readRecords(tmp.path(), QDateTime(), &recentModel);

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
        SessionLogReader::readRecords(tmp.path(), QDateTime(), nullptr);

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
        SessionLogReader::readRecords(tmp.path(), utc("2026-08-01T00:00:00"), &recentModel);

    // 오래된 레코드는 담기지 않지만(메모리 절감), 최근 모델 판정에는 참여한다
    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].input, 1);
    QCOMPARE(recentModel, QString("claude-sonnet-5"));
}

void TestUsageLogic::readRecords_parsesCacheCreationAndSpeed()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(QDir(tmp.path()).mkpath("proj-a"));

    QFile f(tmp.path() + "/proj-a/session.jsonl");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(jsonlLine("req_1", "2026-08-02T11:00:00.000Z", "claude-opus-5",
                      2, 309, 7988, 18044,
                      R"(,"cache_creation":{"ephemeral_1h_input_tokens":7988,)"
                      R"("ephemeral_5m_input_tokens":0},)"
                      R"("server_tool_use":{"web_search_requests":2},)"
                      R"("speed":"fast")"));
    f.close();

    const QVector<TokenRecord> records =
        SessionLogReader::readRecords(tmp.path(), QDateTime(), nullptr);

    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].cacheWrite,   7988);
    QCOMPARE(records[0].cacheWrite1h, 7988);   // 전량 1시간 캐시
    QCOMPARE(records[0].webSearches,  2);
    QVERIFY(records[0].fastMode);
}

void TestUsageLogic::readRecords_ignoresIterationsArray()
{
    // usage.iterations[] 는 시도별 내역이라 같은 토큰 필드를 그대로 반복한다.
    // 최상위 값이 이미 합계이므로 iterations 를 더하면 즉시 2배가 된다.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(QDir(tmp.path()).mkpath("proj-a"));

    QFile f(tmp.path() + "/proj-a/session.jsonl");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(jsonlLine("req_1", "2026-08-02T11:00:00.000Z", "claude-opus-5",
                      2, 309, 100, 500,
                      R"(,"iterations":[{"input_tokens":2,"output_tokens":309,)"
                      R"("cache_creation_input_tokens":100,)"
                      R"("cache_read_input_tokens":500,"type":"message"}])"));
    f.close();

    const QVector<TokenRecord> records =
        SessionLogReader::readRecords(tmp.path(), QDateTime(), nullptr);

    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].input,      2);      // 4 가 아니다
    QCOMPARE(records[0].output,     309);    // 618 이 아니다
    QCOMPARE(records[0].cacheWrite, 100);
    QCOMPARE(records[0].cacheRead,  500);
}

// ─────────────────────────────────────────────────────────────────────────────
// 사용자의 ~/.claude/settings.json 을 통째로 다시 쓰는 코드라, 남의 설정과
// 남의 훅이 살아남는지를 반드시 지켜야 한다.

namespace {

// 이미 다른 훅이 등록돼 있는, 흔한 형태의 settings.json
QJsonObject foreignSettings()
{
    return QJsonDocument::fromJson(R"({
        "model": "opus",
        "tui": "fullscreen",
        "hooks": {
            "PreToolUse": [
                { "matcher": "Bash",
                  "hooks": [ { "type": "command", "command": "guard.sh" } ] }
            ],
            "Stop": [
                { "hooks": [ { "type": "command", "command": "notify.sh" } ] }
            ]
        }
    })").object();
}

int handlerCount(const QJsonObject &root, const QString &event)
{
    int n = 0;
    for (const QJsonValue &g : root.value("hooks").toObject().value(event).toArray())
        n += g.toObject().value("hooks").toArray().size();
    return n;
}

bool hasOurHandler(const QJsonObject &root, const QString &event, const QString &exe)
{
    for (const QJsonValue &g : root.value("hooks").toObject().value(event).toArray()) {
        for (const QJsonValue &h : g.toObject().value("hooks").toArray()) {
            const QJsonObject o = h.toObject();
            if (o.value("command").toString() == exe
                && o.value("args").toArray().contains(QJsonValue("--claude-tracker-hook")))
                return true;
        }
    }
    return false;
}

} // namespace

void TestUsageLogic::hooks_installKeepsForeignSettingsAndHooks()
{
    const QString exe = "C:/apps/ClaudeTray.exe";
    const QJsonObject out = HookBridge::applyHooksToSettings(foreignSettings(), true, exe);

    // 훅과 무관한 최상위 키는 그대로다.
    QCOMPARE(out.value("model").toString(), QString("opus"));
    QCOMPARE(out.value("tui").toString(),   QString("fullscreen"));

    // 우리가 건드리지 않는 이벤트의 남의 훅은 손대지 않는다.
    QCOMPARE(handlerCount(out, "PreToolUse"), 1);

    // 우리도 쓰는 이벤트라면 남의 훅 옆에 나란히 들어간다.
    QCOMPARE(handlerCount(out, "Stop"), 2);
    QVERIFY(hasOurHandler(out, "Stop", exe));

    // 승인 대기를 알려주는 이벤트가 실제로 등록돼야 한다.
    QVERIFY(hasOurHandler(out, "PermissionRequest", exe));
}

void TestUsageLogic::hooks_installIsIdempotent()
{
    const QString exe = "C:/apps/ClaudeTray.exe";
    const QJsonObject once  = HookBridge::applyHooksToSettings(foreignSettings(), true, exe);
    const QJsonObject twice = HookBridge::applyHooksToSettings(once, true, exe);

    QCOMPARE(twice, once);
    QCOMPARE(handlerCount(twice, "Stop"), 2);

    // 경로가 바뀐 재설치는 옛 항목을 남기지 않고 갈아끼운다.
    const QString moved = "D:/apps/ClaudeTray.exe";
    const QJsonObject relocated = HookBridge::applyHooksToSettings(once, true, moved);
    QCOMPARE(handlerCount(relocated, "PermissionRequest"), 1);
    QVERIFY(hasOurHandler(relocated, "PermissionRequest", moved));
    QVERIFY(!hasOurHandler(relocated, "PermissionRequest", exe));
}

void TestUsageLogic::hooks_uninstallRestoresOriginal()
{
    const QString exe = "C:/apps/ClaudeTray.exe";
    const QJsonObject original = foreignSettings();

    const QJsonObject installed = HookBridge::applyHooksToSettings(original, true, exe);
    QVERIFY(installed != original);

    const QJsonObject removed = HookBridge::applyHooksToSettings(installed, false, exe);
    QCOMPARE(removed, original);

    // 훅이 하나도 없던 설정이라면 hooks 키 자체를 남기지 않는다.
    QJsonObject bare;
    bare["model"] = "opus";
    const QJsonObject roundTrip = HookBridge::applyHooksToSettings(
        HookBridge::applyHooksToSettings(bare, true, exe), false, exe);
    QCOMPARE(roundTrip, bare);
    QVERIFY(!roundTrip.contains("hooks"));
}

// ─────────────────────────────────────────────────────────────────────────────

void TestUsageLogic::calibrator_priorMatchesLegacyFormula()
{
    // prior 는 예전 하드코딩 공식과 정확히 같아야 한다. 그래야 학습 표본이
    // 0 건인 첫 실행에서 화면 숫자가 달라지지 않는다.
    const QuotaCoefficients p = QuotaCalibrator::priorFor(1'000'000);

    // Sonnet 은 계열 가중치 1.0 기준이라 예전 공식 그대로여야 한다.
    UsageFeatures f;
    f.add(Calib::Sonnet, Calib::Input,      100);
    f.add(Calib::Sonnet, Calib::Output,      50);
    f.add(Calib::Sonnet, Calib::CacheWrite,  10);
    f.add(Calib::Sonnet, Calib::CacheRead, 1000);   // 가중치 0.1 → 100

    QCOMPARE(p.predict(f), 260.0 / 1'000'000.0);

    // 한도가 커지면 토큰당 소모는 그만큼 작아진다 (선형).
    const QuotaCoefficients big = QuotaCalibrator::priorFor(10'000'000);
    QVERIFY(qAbs(big.predict(f) - p.predict(f) / 10.0) < 1e-15);

    // 한도를 모르면 계수가 0 이라 예측도 0 이다.
    QVERIFY(!QuotaCalibrator::priorFor(0).isValid());
}

void TestUsageLogic::calibrator_convergesTowardObservedUtilization()
{
    // 실제 한도가 prior 의 절반(=토큰당 소모가 2배)인 사용자를 흉내낸다.
    const QuotaCoefficients prior = QuotaCalibrator::priorFor(1'000'000);
    QuotaCoefficients coeff = prior;

    UsageFeatures f;
    f.add(Calib::Opus, Calib::Input, 200'000);
    // 참값이 prior 추정의 1.5배인 사용자 (prior 는 계열 가중치가 들어 있으므로
    // 기대값을 상수로 박지 않고 prior 에서 유도한다)
    const double truth = prior.predict(f) * 1.5;
    QVERIFY(truth < 1.0);

    // 상향(오염 가능) 방향이라 감속 학습이 걸려 예전보다 표본이 더 필요하다.
    // 그래도 API 폴링 5분 주기 기준 반나절이면 수렴한다.
    for (int i = 0; i < 150; ++i)
        QVERIFY(QuotaCalibrator::observe(coeff, f, truth, prior));

    QCOMPARE(coeff.samples, 150);            // 학습이 도움이 되므로 되돌아가지 않는다
    QVERIFY2(qAbs(coeff.predict(f) - truth) < 0.005,
             qPrintable(QString("predict=%1").arg(coeff.predict(f))));
}

void TestUsageLogic::calibrator_learnsDownFasterThanUp()
{
    // 외부 표면(claude.ai 등) 사용은 API 사용률을 '올리기만' 한다. 따라서
    // 과소 예측(error>0)은 계수 탓인지 외부 사용 탓인지 알 수 없지만,
    // 과대 예측(error<0)은 계수가 확실히 큰 것이다. 후자만 전속으로 내린다.
    const QuotaCoefficients prior = QuotaCalibrator::priorFor(1'000'000);

    UsageFeatures f;
    f.add(Calib::Opus, Calib::Input, 200'000);
    const double base = prior.predict(f);          // 0.20

    // 같은 크기의 오차를 위/아래로 한 번씩 준다.
    QuotaCoefficients up = prior;
    QuotaCalibrator::observe(up, f, base + 0.05, prior);
    const double movedUp = up.predict(f) - base;

    QuotaCoefficients down = prior;
    QuotaCalibrator::observe(down, f, base - 0.05, prior);
    const double movedDown = base - down.predict(f);

    QVERIFY(movedUp > 0.0);
    QVERIFY(movedDown > 0.0);
    // 내려가는 쪽이 확실히 빨라야 한다 (설계상 4배)
    QVERIFY2(movedDown > movedUp * 3.0,
             qPrintable(QString("up=%1 down=%2").arg(movedUp).arg(movedDown)));
}

void TestUsageLogic::calibrator_residualReportsUnexplainedUsage()
{
    // 잔차는 갱신 '전' 오차다. 양수면 로컬 로그로 설명되지 않는 사용량이고,
    // 이 값이 꾸준히 양수인지가 외부 표면 사용을 가려내는 근거가 된다.
    const QuotaCoefficients prior = QuotaCalibrator::priorFor(1'000'000);
    QuotaCoefficients coeff = prior;

    UsageFeatures f;
    f.add(Calib::Opus, Calib::Input, 100'000);
    const double observed = 0.30;
    const double expected = observed - prior.predict(f);   // 설명 안 되는 몫

    double residual = 0.0;
    QVERIFY(QuotaCalibrator::observe(coeff, f, observed, prior, &residual));
    QVERIFY2(qAbs(residual - expected) < 1e-9,
             qPrintable(QString("residual=%1 expected=%2").arg(residual).arg(expected)));

    // 학습이 일어나지 않는 관측에서는 0 으로 초기화된다.
    double none = 123.0;
    QVERIFY(!QuotaCalibrator::observe(coeff, UsageFeatures(), 0.5, prior, &none));
    QCOMPARE(none, 0.0);
}

void TestUsageLogic::calibrator_ignoresUnobservableSamples()
{
    const QuotaCoefficients prior = QuotaCalibrator::priorFor(1'000'000);
    QuotaCoefficients coeff = prior;

    UsageFeatures f;
    f.add(Calib::Opus, Calib::Input, 100'000);

    // 0% 는 신호가 없고, 100% 는 포화라 참값을 알 수 없다.
    QVERIFY(!QuotaCalibrator::observe(coeff, f, 0.0,  prior));
    QVERIFY(!QuotaCalibrator::observe(coeff, f, 1.0,  prior));
    // 토큰이 하나도 없으면 배울 게 없다.
    QVERIFY(!QuotaCalibrator::observe(coeff, UsageFeatures(), 0.5, prior));

    QCOMPARE(coeff.samples, 0);
}

void TestUsageLogic::calibrator_learnsPerFamilyIndependently()
{
    // Opus 만 쓴 관측으로는 Haiku 계수가 움직이면 안 된다.
    const QuotaCoefficients prior = QuotaCalibrator::priorFor(1'000'000);
    QuotaCoefficients coeff = prior;
    const double haikuBefore = coeff.c[Calib::Haiku][Calib::Input];

    UsageFeatures opusOnly;
    opusOnly.add(Calib::Opus, Calib::Input, 300'000);
    for (int i = 0; i < 20; ++i)
        QuotaCalibrator::observe(coeff, opusOnly, 0.60, prior);

    QVERIFY(coeff.c[Calib::Opus][Calib::Input] > prior.c[Calib::Opus][Calib::Input]);
    QCOMPARE(coeff.c[Calib::Haiku][Calib::Input], haikuBefore);
}

void TestUsageLogic::calibrator_neverPredictsOverFullQuota()
{
    // 상한은 추측한 배수가 아니라 정의에서 온다: utilization 은 100% 를 넘을 수
    // 없다. 로그가 크게 유실된 상황(토큰은 조금인데 API 는 90%)에서 계수가
    // 아무리 밀려도, 그 관측에 대한 예측이 1.0 을 넘어선 안 된다.
    const QuotaCoefficients prior = QuotaCalibrator::priorFor(1'000'000);
    QuotaCoefficients coeff = prior;

    UsageFeatures tiny;
    tiny.add(Calib::Opus, Calib::Input, 1'000);

    for (int i = 0; i < 200; ++i) {
        QuotaCalibrator::observe(coeff, tiny, 0.90, prior);
        QVERIFY2(coeff.predict(tiny) <= 1.0 + 1e-12,
                 qPrintable(QString("predict=%1 (i=%2)").arg(coeff.predict(tiny)).arg(i)));
        QVERIFY(coeff.c[Calib::Opus][Calib::Input] >= 0.0);
    }
}

void TestUsageLogic::calibrator_revertsWhenWorseThanPrior()
{
    // 자가 복구 기준은 '천장에 몇 번 붙었나'가 아니라 '실제로 더 잘 맞히는가'다.
    // 참 한도를 몰라도 두 모델의 예측 오차를 직접 비교하면 판정할 수 있다.
    const QuotaCoefficients prior = QuotaCalibrator::priorFor(1'000'000);

    UsageFeatures f;
    f.add(Calib::Opus, Calib::Input, 200'000);

    // ① 저장된 계수가 현실과 어긋나는 경우 — 예: 플랜을 Pro→Max 로 올려서
    //    한도가 커졌는데 옛 계수가 그대로 남아 크게 과대 예측하는 상황.
    //    (충분히 학습됐던 상태로 저장돼 있었으므로 samples 가 크다)
    QuotaCoefficients stale = prior;
    for (int fam = 0; fam < Calib::FamilyCount; ++fam)
        for (int k = 0; k < Calib::KindCount; ++k)
            stale.c[fam][k] *= 20.0;
    stale.samples = 100;

    bool reverted = false;
    for (int i = 0; i < 30; ++i) {
        QuotaCalibrator::observe(stale, f, prior.predict(f), prior);
        if (stale.samples == 0) { reverted = true; break; }
    }
    QVERIFY2(reverted, "prior 가 더 잘 맞히는데도 되돌아가지 않았다");
    QCOMPARE(stale.c[Calib::Opus][Calib::Input], prior.c[Calib::Opus][Calib::Input]);

    // ② 학습이 실제로 도움이 되는 경우에는 되돌리면 안 된다.
    //    참값이 prior 예측의 1.5배인 사용자 — 학습분이 계속 이긴다.
    QuotaCoefficients good = prior;
    const double truth = prior.predict(f) * 1.5;
    for (int i = 0; i < 300; ++i)
        QuotaCalibrator::observe(good, f, truth, prior);

    QCOMPARE(good.samples, 300);          // 한 번도 되돌아가지 않음
    QVERIFY2(qAbs(good.predict(f) - truth) < 0.005,
             qPrintable(QString("predict=%1 truth=%2").arg(good.predict(f)).arg(truth)));
}

// ── 크레딧 보정 ──────────────────────────────────────────────────────────────
// 5h/7d 와 달리 관측이 '증분 대 증분'이다. 요율표로 계산한 달러가 실제 크레딧
// 차감액과 어긋나는 배율 하나를 학습한다.

void TestUsageLogic::creditCalib_convergesTowardObservedIncrement()
{
    // 실제 차감액이 요율표 계산의 1.2 배인 사용자.
    CreditCoefficient c;
    QCOMPARE(c.k, 1.0);                       // prior = 요율표 그대로

    const double raw = 2.00;                  // 로컬 추정 증분 $2.00
    const double truth = raw * 1.2;           // 실제 $2.40

    for (int i = 0; i < 200; ++i)
        QVERIFY(QuotaCalibrator::observeCredit(c, raw, truth));

    QVERIFY2(qAbs(c.k - 1.2) < 0.01,
             qPrintable(QString("k=%1, 기대 1.2").arg(c.k)));
    QCOMPARE(c.samples, 200);
}

void TestUsageLogic::creditCalib_learnsDownFasterThanUp()
{
    // 크레딧도 claude.ai 등 외부 사용분이 API 값에만 얹힌다. 따라서 '실측이 더
    // 크다'(error>0)는 k 가 작아서인지 외부 사용 때문인지 구별할 수 없는
    // 오염된 방향이고, '실측이 더 작다'는 외부 사용으로는 만들 수 없는 증거다.
    // 오차 크기를 ±0.50 으로 대칭으로 맞춰야 '방향'만 비교하는 시험이 된다.
    const double raw = 1.00;

    CreditCoefficient up;                     // 실측 +0.50 → 올려야 함(오염 방향)
    QuotaCalibrator::observeCredit(up, raw, 1.50);

    CreditCoefficient down;                   // 실측 −0.50 → 내려야 함(깨끗)
    QuotaCalibrator::observeCredit(down, raw, 0.50);

    const double movedUp   = up.k - 1.0;      // +
    const double movedDown = 1.0 - down.k;    // +
    QVERIFY(movedUp > 0.0);
    QVERIFY(movedDown > 0.0);

    // 같은 크기(1.0)의 오차인데 내려가는 쪽이 확실히 빠르다.
    QVERIFY2(movedDown > movedUp * 3.0,
             qPrintable(QString("down=%1 up=%2").arg(movedDown).arg(movedUp)));
}

void TestUsageLogic::creditCalib_ignoresNoiseSizedIncrements()
{
    CreditCoefficient c;

    // 몇 센트짜리 증분은 API 의 센트 반올림이 신호보다 크다.
    QVERIFY(!QuotaCalibrator::observeCredit(c, 0.01, 0.03));
    QVERIFY(!QuotaCalibrator::observeCredit(c, 0.00, 1.00));
    // 음수 증분은 월 리셋·API 정정이라 학습 대상이 아니다.
    QVERIFY(!QuotaCalibrator::observeCredit(c, 1.00, -0.50));

    QCOMPARE(c.samples, 0);
    QCOMPARE(c.k, 1.0);
}

void TestUsageLogic::creditCalib_revertsWhenWorseThanPrior()
{
    // 저장분이 현실과 어긋나 있으면(예: 요율표가 갱신돼 이미 정확해졌는데
    // 옛 배율이 남은 경우) 요율표 그대로보다 못 맞히므로 되돌려야 한다.
    CreditCoefficient stale;
    stale.k       = 4.0;
    stale.samples = 100;

    const double raw = 1.50;
    bool reverted = false;
    for (int i = 0; i < 40; ++i) {
        QuotaCalibrator::observeCredit(stale, raw, raw);   // 참값 = 요율표 그대로
        if (stale.samples == 0) { reverted = true; break; }
    }
    QVERIFY2(reverted, "prior 가 더 잘 맞히는데도 되돌아가지 않았다");
    QCOMPARE(stale.k, 1.0);

    // 반대로 학습이 실제로 도움이 되면 되돌리지 않는다.
    CreditCoefficient good;
    for (int i = 0; i < 300; ++i)
        QuotaCalibrator::observeCredit(good, raw, raw * 1.35);
    QCOMPARE(good.samples, 300);
    QVERIFY(qAbs(good.k - 1.35) < 0.01);
}

void TestUsageLogic::creditCalib_scalesOnlyTheIncrementNotTheApiBase()
{
    // API 실측 베이스는 이미 정확하다. 거기에 배율을 먹이면 맞는 값을 틀리게
    // 만든다. 배율은 '마지막 응답 이후의 증분'에만 붙어야 한다.
    UsageData api;
    api.fetchedAt = QDateTime::currentDateTimeUtc();
    api.fiveHour  = { 1.0, api.fetchedAt.addSecs(3600), 0, 0, true };
    api.sevenDay  = { 1.0, api.fetchedAt.addSecs(86400), 0, 0, true };
    api.extraUsage.enabled      = true;
    api.extraUsage.limitDollars = 20.0;
    api.extraUsage.usedCredits  = 10.00;

    // 한도를 이미 채운 뒤의 델타라 전액이 크레딧으로 청구된다. 델타 비율까지
    // 있어야 chargeableRatioFor 가 (total−1)/delta = 1.0 을 낸다.
    UsageData delta;
    delta.fiveHour = { 0.05, QDateTime(), 0, 0, true };
    delta.extraUsage.usedCredits = 2.00;

    const QDateTime now = api.fetchedAt.addSecs(60);

    const UsageData plain  = UsageMerger::mergeWithLastApi(api, delta, now, 1.0);
    const UsageData scaled = UsageMerger::mergeWithLastApi(api, delta, now, 1.5);

    QVERIFY(qAbs(plain.extraUsage.usedCredits  - 12.00) < 1e-9);   // 10 + 2×1.0
    QVERIFY(qAbs(scaled.extraUsage.usedCredits - 13.00) < 1e-9);   // 10 + 2×1.5

    // 배율이 0 이나 비정상이면 무시하고 1.0 처럼 동작한다.
    const UsageData guarded = UsageMerger::mergeWithLastApi(api, delta, now, 0.0);
    QVERIFY(qAbs(guarded.extraUsage.usedCredits - 12.00) < 1e-9);
}

void TestUsageLogic::calibrator_priorWeightsFamiliesByPrice()
{
    // 계열 무관 prior 는 단가가 5배 차이나는 점에 비추어 확실히 틀린 가정이었다.
    // Sonnet 기준으로 Opus 5/3 배, Haiku 1/3 배.
    const QuotaCoefficients p = QuotaCalibrator::priorFor(1'000'000);
    const double sonnet = p.c[Calib::Sonnet][Calib::Input];

    QVERIFY(sonnet > 0.0);
    QVERIFY2(qAbs(p.c[Calib::Opus][Calib::Input] / sonnet - 5.0 / 3.0) < 1e-9,
             "Opus 가 Sonnet 단가비(5:3)만큼 무겁게 잡혀야 한다");
    QVERIFY2(qAbs(p.c[Calib::Haiku][Calib::Input] / sonnet - 1.0 / 3.0) < 1e-9,
             "Haiku 가 Sonnet 단가비(1:3)만큼 가볍게 잡혀야 한다");
    // 캐시 읽기는 계열과 무관하게 입력의 1/10
    QVERIFY(qAbs(p.c[Calib::Opus][Calib::CacheRead]
                 / p.c[Calib::Opus][Calib::Input] - 0.1) < 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────

namespace {
UsageData makeApiData(double util5h, const QDateTime &reset5h,
                      double util7d = 0.0, const QDateTime &reset7d = {})
{
    UsageData d;
    d.fromApi = true;
    // 실제 API 응답에는 항상 있는 값. 월 롤오버 판정이 여기에 걸린다.
    d.fetchedAt = utc("2026-08-02T11:55:00");
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
    // API: 5h 50%, 리셋은 미래. 델타는 스캐너가 보정 계수로 이미 비율로 만든 값.
    const UsageData api = makeApiData(0.5, utc("2026-08-02T15:00:00"));

    UsageData delta;
    delta.fiveHour.valid       = true;
    delta.fiveHour.utilization = 0.10;
    delta.fiveHour.rawTokens   = 100'000;

    const UsageData merged = UsageMerger::mergeWithLastApi(api, delta, now);

    // 한도 추정을 두 번 곱하지 않고 비율끼리 더한다.
    QCOMPARE(merged.fiveHour.utilization, 0.6);
    QVERIFY(!merged.fromApi);
}

void TestUsageLogic::merge_clampsCombinedUtilizationAtOne()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    const UsageData api = makeApiData(0.95, utc("2026-08-02T15:00:00"));

    UsageData delta;
    delta.fiveHour.valid       = true;
    delta.fiveHour.utilization = 0.40;

    const UsageData merged = UsageMerger::mergeWithLastApi(api, delta, now);
    QCOMPARE(merged.fiveHour.utilization, 1.0);
}

void TestUsageLogic::merge_dropsStaleApiTokensAfterReset()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    // API 가 알려준 리셋 시각이 이미 지났다 → 구 API 사용량은 버려야 한다
    const UsageData api = makeApiData(0.9, utc("2026-08-02T11:00:00"));

    UsageData delta;
    delta.fiveHour.valid       = true;
    delta.fiveHour.utilization = 0.02;
    delta.fiveHour.rawTokens   = 20'000;

    const UsageData merged = UsageMerger::mergeWithLastApi(api, delta, now);

    QCOMPARE(merged.fiveHour.utilization, 0.02);   // 0.92 가 아니라 델타만
    QCOMPARE(merged.fiveHour.rawTokens, 20'000);
}

void TestUsageLogic::merge_refreshesResetsAtAfterReset()
{
    // 리셋이 지난 뒤에도 옛 resetsAt 가 그대로 남으면 카운트다운이
    // "곧 초기화됩니다" 에서 멈춘다. 다음 주기로 갱신되어야 한다.
    const QDateTime now = utc("2026-08-02T12:00:00");
    const UsageData api = makeApiData(0.9, utc("2026-08-02T11:00:00"),
                                      0.3, utc("2026-07-30T00:00:00"));

    const UsageData merged =
        UsageMerger::mergeWithLastApi(api, UsageData(), now);

    QCOMPARE(merged.fiveHour.resetsAt, utc("2026-08-02T16:00:00"));
    QVERIFY(merged.fiveHour.resetsAt > now);
    QCOMPARE(merged.sevenDay.resetsAt, utc("2026-08-06T00:00:00"));
    QVERIFY(merged.sevenDay.resetsAt > now);
}

namespace {
// 한도를 이미 다 쓴 상태의 API 응답 (크레딧이 실제로 나가는 상황)
UsageData makeSaturatedApi(const QDateTime &reset5h)
{
    UsageData d = makeApiData(1.0, reset5h, 1.0, reset5h.addSecs(SECS_7D));
    return d;
}
// 델타도 한도를 넘긴 상태 → 전액 청구되도록
UsageData makeOverageDelta(double credits)
{
    UsageData d;
    d.fiveHour.valid       = true;
    d.fiveHour.utilization = 0.05;
    d.sevenDay.valid       = true;
    d.sevenDay.utilization = 0.01;
    d.extraUsage.usedCredits = credits;
    return d;
}
}

void TestUsageLogic::merge_extraCreditOnlyWhenApiEnabledIt()
{
    const QDateTime now = utc("2026-08-02T12:00:00");
    const UsageData delta = makeOverageDelta(1.50);  // 스캐너는 enabled 를 켜지 않는다

    // 1) API 가 꺼져 있으면 델타를 더하지 않고 꺼진 채로 둔다
    UsageData apiOff = makeSaturatedApi(utc("2026-08-02T15:00:00"));
    apiOff.extraUsage.enabled = false;
    const UsageData mergedOff =
        UsageMerger::mergeWithLastApi(apiOff, delta, now);
    QVERIFY(!mergedOff.extraUsage.enabled);
    QCOMPARE(mergedOff.extraUsage.usedCredits, 0.0);

    // 2) API 가 켜져 있고 한도를 이미 넘겼으면 API 값 + 델타
    UsageData apiOn = makeSaturatedApi(utc("2026-08-02T15:00:00"));
    apiOn.extraUsage.enabled      = true;
    apiOn.extraUsage.usedCredits  = 8.50;
    apiOn.extraUsage.limitDollars = 20.00;
    const UsageData mergedOn =
        UsageMerger::mergeWithLastApi(apiOn, delta, now);
    QVERIFY(mergedOn.extraUsage.enabled);
    QCOMPARE(mergedOn.extraUsage.usedCredits, 10.00);
    QCOMPARE(mergedOn.extraUsage.utilization, 0.5);
}

void TestUsageLogic::merge_extraCreditResetsOnMonthRollover()
{
    // 추가 크레딧은 달이 바뀌면 0 부터 다시 쌓인다. 전월 API 값을 베이스라인으로
    // 쓰면 이번 달 사용액 위에 지난달 누적액이 얹혀 두 배로 보인다.
    const QDateTime now = utc("2026-09-01T00:30:00");

    UsageData api = makeSaturatedApi(utc("2026-09-01T03:00:00"));
    api.fetchedAt                 = utc("2026-08-31T23:55:00");   // 지난달
    api.extraUsage.enabled        = true;
    api.extraUsage.usedCredits    = 18.00;
    api.extraUsage.limitDollars   = 20.00;

    const UsageData merged =
        UsageMerger::mergeWithLastApi(api, makeOverageDelta(0.75), now);
    QCOMPARE(merged.extraUsage.usedCredits, 0.75);   // 18.75 가 아니다
}

void TestUsageLogic::merge_extraCreditKeepsBaselineWhenFetchedAtUnknown()
{
    // fetchedAt 이 비어 있으면 '언제 값인지 모른다'는 뜻이다. 롤오버로 단정하면
    // 멀쩡한 베이스라인이 0 으로 날아가 표시액이 갑자기 줄어든다.
    const QDateTime now = utc("2026-08-02T12:00:00");

    UsageData api = makeSaturatedApi(utc("2026-08-02T15:00:00"));
    api.fetchedAt = QDateTime();                     // 알 수 없음
    api.extraUsage.enabled     = true;
    api.extraUsage.usedCredits = 4.00;

    const UsageData merged =
        UsageMerger::mergeWithLastApi(api, makeOverageDelta(1.00), now);
    QCOMPARE(merged.extraUsage.usedCredits, 5.00);
}

// ── 추가 결제 크레딧 청구 비율 ────────────────────────────────────────────────

void TestUsageLogic::charge_nothingWhileUnderQuota()
{
    // 한도를 다 쓰지도 않았는데 크레딧이 오르던 과다 청구 버그의 회귀 방지.
    QuotaInfo api;   api.valid = true;   api.utilization = 0.40;
    QuotaInfo delta; delta.valid = true; delta.utilization = 0.10;

    QCOMPARE(UsageMerger::chargeableRatioFor(api, delta, false), 0.0);
}

void TestUsageLogic::charge_onlyTheOveragePortion()
{
    // 95% 에서 델타 10% → 105%. 델타 중 한도를 넘어선 5%p 만 청구한다.
    QuotaInfo api;   api.valid = true;   api.utilization = 0.95;
    QuotaInfo delta; delta.valid = true; delta.utilization = 0.10;

    QCOMPARE(UsageMerger::chargeableRatioFor(api, delta, false), 0.5);

    // 이미 100% 였다면 델타 전액이 크레딧에서 나간다.
    api.utilization = 1.0;
    QCOMPARE(UsageMerger::chargeableRatioFor(api, delta, false), 1.0);
}

void TestUsageLogic::charge_weeklyQuotaAloneCanTriggerBilling()
{
    // 5h 는 막 리셋돼 여유롭지만 7d 를 다 쓴 상태. 5h 만 보면 청구가 0 이 되어
    // 실제로 나간 크레딧을 놓친다.
    UsageData api;
    api.fiveHour.valid       = true;  api.fiveHour.utilization = 0.20;
    api.sevenDay.valid       = true;  api.sevenDay.utilization = 1.00;

    UsageData delta;
    delta.fiveHour.valid       = true; delta.fiveHour.utilization = 0.05;
    delta.sevenDay.valid       = true; delta.sevenDay.utilization = 0.02;

    QCOMPARE(UsageMerger::chargeableRatioFor(api.fiveHour, delta.fiveHour, false), 0.0);
    QCOMPARE(UsageMerger::chargeableRatio(api, delta, false, false), 1.0);
}

void TestUsageLogic::charge_keepsPreResetOverageWhenWindowRolled()
{
    // 델타 구간 안에서 5h 리셋이 일어나면 델타 '비율'은 리셋 이후만 세는데
    // 델타 '비용'은 리셋 전 지출까지 포함한다. 리셋 직전 한도를 채운 상태였다면
    // 그 지출은 실제로 크레딧에서 나갔으므로 버리면 안 된다.
    QuotaInfo saturated; saturated.valid = true; saturated.utilization = 1.0;
    QuotaInfo delta;     delta.valid = true;     delta.utilization = 0.03;
    QCOMPARE(UsageMerger::chargeableRatioFor(saturated, delta, true), 1.0);

    // 반대로 리셋 직전에도 한도 안이었다면 청구할 게 없다.
    QuotaInfo relaxed; relaxed.valid = true; relaxed.utilization = 0.30;
    QCOMPARE(UsageMerger::chargeableRatioFor(relaxed, delta, true), 0.0);
}

void TestUsageLogic::charge_defaultsToFullWhenNoQuotaKnown()
{
    // 한도 정보가 아예 없으면(플랜 미상 등) 과소 보고보다 과대 보고가 안전하다.
    // 어차피 다음 API 응답이 정확값으로 덮어쓴다.
    QuotaInfo invalid;                       // valid == false
    QuotaInfo delta; delta.valid = true; delta.utilization = 0.10;
    QVERIFY(UsageMerger::chargeableRatioFor(invalid, delta, false) < 0.0);

    QCOMPARE(UsageMerger::chargeableRatio(UsageData(), UsageData(), false, false), 1.0);
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
#include "UsageLogicTest.moc"
