#include "usagemerge.h"
#include "usagescanner.h"

namespace {

constexpr qint64 SECS_5H = 5LL * 3600;
constexpr qint64 SECS_7D = 7LL * 24 * 3600;

// API 값(비율)을 토큰으로 되돌린 뒤 로컬 델타 토큰을 더한다.
// 리셋이 이미 일어났다면 구 API 토큰은 버리고 리셋 이후 델타만 센다.
QuotaInfo mergeQuota(const QuotaInfo &apiQuota, const QuotaInfo &deltaQuota,
                     qint64 limitTokens, bool resetOccurred)
{
    if (!apiQuota.valid)
        return deltaQuota;

    QuotaInfo merged = apiQuota;
    if (limitTokens <= 0)
        return merged;

    const qint64 apiTokens = resetOccurred
        ? 0
        : qRound64(apiQuota.utilization * static_cast<double>(limitTokens));
    const qint64 deltaTokens = qMax<qint64>(0, deltaQuota.rawTokens);

    merged.rawTokens   = apiTokens + deltaTokens;
    merged.limitTokens = limitTokens;
    merged.utilization = qMin(1.0, static_cast<double>(merged.rawTokens)
                                 / static_cast<double>(limitTokens));
    return merged;
}

} // namespace

UsageData UsageMerge::mergeWithLastApi(const UsageData &lastApi,
                                       const UsageData &delta,
                                       qint64 limit5h,
                                       qint64 limit7d,
                                       const QDateTime &nowUtc)
{
    UsageData merged = lastApi;
    merged.fromApi   = false;
    merged.fetchedAt = QDateTime::currentDateTime();

    // API 가 알려준 resetsAt 가 이미 과거면 그 사이 리셋이 일어난 것이다.
    const bool reset5hOccurred = lastApi.fiveHour.resetsAt.isValid()
                              && lastApi.fiveHour.resetsAt.toUTC() <= nowUtc;
    const bool reset7dOccurred = lastApi.sevenDay.resetsAt.isValid()
                              && lastApi.sevenDay.resetsAt.toUTC() <= nowUtc;

    merged.fiveHour = mergeQuota(lastApi.fiveHour, delta.fiveHour, limit5h, reset5hOccurred);
    merged.sevenDay = mergeQuota(lastApi.sevenDay, delta.sevenDay, limit7d, reset7dOccurred);

    // resetsAt 추정은 반드시 mergeQuota '뒤'에 덮어써야 한다.
    // mergeQuota 는 apiQuota 를 통째로 복사해 돌려주므로, 먼저 넣으면
    // 이미 지나간 옛 resetsAt 이 되살아나 리셋 직후 카운트다운이
    // "곧 초기화됩니다" 에서 영영 멈춘다.
    const QDateTime next5h =
        UsageScanner::estimateNextReset(lastApi.fiveHour.resetsAt, SECS_5H, nowUtc);
    const QDateTime next7d =
        UsageScanner::estimateNextReset(lastApi.sevenDay.resetsAt, SECS_7D, nowUtc);
    if (next5h.isValid())
        merged.fiveHour.resetsAt = next5h;
    if (next7d.isValid())
        merged.sevenDay.resetsAt = next7d;

    merged.recentModel = delta.recentModel.isEmpty() ? lastApi.recentModel
                                                     : delta.recentModel;

    // 추가 크레딧 표시 여부와 한도는 API 만이 안다. 켜져 있을 때만 델타 비용을 더한다.
    merged.extraUsage = lastApi.extraUsage;
    if (lastApi.extraUsage.enabled) {
        merged.extraUsage.usedCredits =
            lastApi.extraUsage.usedCredits + delta.extraUsage.usedCredits;
        if (merged.extraUsage.limitDollars > 0.0) {
            merged.extraUsage.utilization =
                qMin(1.0, merged.extraUsage.usedCredits / merged.extraUsage.limitDollars);
        }
    }

    return merged;
}
