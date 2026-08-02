#include "UsageAggregator.h"
#include "ModelPricing.h"

#include <QMap>
#include <QTimeZone>

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

// 추가 결제 크레딧은 월 단위로 누적되므로 이번 달 1일(UTC)이 기준점이다.
QDateTime billingCycleStart(const QDateTime &nowUtc)
{
    return QDateTime(QDate(nowUtc.date().year(), nowUtc.date().month(), 1),
                     QTime(0, 0), QTimeZone::UTC);
}

} // namespace

namespace UsageAggregator {

qint64 planLimit5h(const QString &subscriptionType)
{
    return PLAN_LIMITS_5H.value(subscriptionType, 0);
}

qint64 planLimit7d(const QString &subscriptionType)
{
    return PLAN_LIMITS_7D.value(subscriptionType, 0);
}

QDateTime estimateNextReset(const QDateTime &last, qint64 periodSecs,
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

QDateTime earliestRelevant(const QDateTime &nowUtc,
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

ScanResult aggregate(const QVector<TokenRecord> &records,
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

    // 할당량은 "가중 토큰 합 ÷ 하드코딩 한도"로 구하지 않는다.
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
        const double cost       = ModelPricingTable::costOf(r);

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
        // 비용만 채우고, 표시 여부는 API 값을 가진 쪽(TrayController)에 맡긴다.
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

} // namespace UsageAggregator
