#include "usagemerge.h"
#include "usagescanner.h"

namespace {

constexpr qint64 SECS_5H = 5LL * 3600;
constexpr qint64 SECS_7D = 7LL * 24 * 3600;

// API 정확값 위에 로컬 델타를 얹는다.
//
// 예전에는 API 비율을 하드코딩 플랜 한도로 곱해 토큰으로 되돌리고, 델타 토큰을
// 더한 뒤 다시 한도로 나눴다. 한도 추정이 틀리면 그 오차가 두 번 곱해졌다.
// 이제 스캐너가 보정 계수로 델타의 '비율'을 직접 내주므로 비율끼리 더하면 된다.
// 리셋이 이미 일어났다면 구 API 비율은 버리고 리셋 이후 델타만 센다.
QuotaInfo mergeQuota(const QuotaInfo &apiQuota, const QuotaInfo &deltaQuota,
                     bool resetOccurred)
{
    if (!apiQuota.valid)
        return deltaQuota;
    if (!deltaQuota.valid)
        return apiQuota;

    QuotaInfo merged = apiQuota;

    const double apiBase = resetOccurred ? 0.0 : apiQuota.utilization;
    merged.utilization   = qBound(0.0, apiBase + deltaQuota.utilization, 1.0);

    // 토큰 수는 디버그·참고용이다. 한도를 모르므로 API 쪽 토큰은 되돌릴 수 없고,
    // 리셋 후에는 델타만 유효하다.
    merged.rawTokens = (resetOccurred ? 0 : apiQuota.rawTokens) + deltaQuota.rawTokens;

    return merged;
}

} // namespace

UsageData UsageMerge::mergeWithLastApi(const UsageData &lastApi,
                                       const UsageData &delta,
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

    merged.fiveHour = mergeQuota(lastApi.fiveHour, delta.fiveHour, reset5hOccurred);
    merged.sevenDay = mergeQuota(lastApi.sevenDay, delta.sevenDay, reset7dOccurred);
    merged.sevenDaySonnet =
        mergeQuota(lastApi.sevenDaySonnet, delta.sevenDaySonnet, reset7dOccurred);

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
    if (next7d.isValid()) {
        merged.sevenDay.resetsAt       = next7d;
        merged.sevenDaySonnet.resetsAt = next7d;
    }

    merged.recentModel = delta.recentModel.isEmpty() ? lastApi.recentModel
                                                     : delta.recentModel;

    // 추가 크레딧 표시 여부와 한도는 API 만이 안다. 켜져 있을 때만 델타 비용을 더한다.
    merged.extraUsage = lastApi.extraUsage;
    if (lastApi.extraUsage.enabled) {
        // 추가 크레딧은 월 단위로 리셋된다. API 응답(lastApi)이 이전 달 것이면
        // 베이스라인 비용을 0으로 처리해야 전월 누적액이 이번 달 위에 쌓이지 않는다.
        //
        // fetchedAt 이 없으면 '언제 받은 값인지 모른다'는 뜻이다. 이때 롤오버로
        // 단정하면 멀쩡한 베이스라인을 0 으로 날려 표시액이 갑자기 줄어든다.
        // 모를 때는 롤오버가 아니라고 보는 쪽이 안전하다.
        const QDate apiMonth = lastApi.fetchedAt.toUTC().date();
        const QDate nowMonth = nowUtc.date();
        const bool monthRolledOver = apiMonth.isValid()
                                  && (apiMonth.year() != nowMonth.year()
                                   || apiMonth.month() != nowMonth.month());
        const double apiBaseCredits = monthRolledOver
            ? 0.0
            : lastApi.extraUsage.usedCredits;

        // 5시간 한도를 초과한 비율(utilization)만큼만 델타 비용(추가 결제 크레딧)에 반영한다.
        double chargeableRatio = 0.0;
        if (merged.fiveHour.valid) {
            const double apiUtil = reset5hOccurred ? 0.0 : lastApi.fiveHour.utilization;
            const double deltaUtil = delta.fiveHour.utilization;
            const double totalUtil = apiUtil + deltaUtil;
            
            if (totalUtil > 1.0 && deltaUtil > 0.0) {
                const double overageUtil = totalUtil - 1.0;
                chargeableRatio = qMin(1.0, overageUtil / deltaUtil);
            } else if (totalUtil <= 1.0) {
                chargeableRatio = 0.0; // 한도 미초과 시 무과금
            } else {
                chargeableRatio = 1.0; // 예외 상황 (deltaUtil <= 0)
            }
        } else {
            // 5시간 쿼터 자체가 없는 플랜(무제한 등)이면 일단 전액 과금으로 본다.
            chargeableRatio = 1.0;
        }

        merged.extraUsage.usedCredits = apiBaseCredits + (delta.extraUsage.usedCredits * chargeableRatio);
        if (merged.extraUsage.limitDollars > 0.0) {
            merged.extraUsage.utilization =
                qMin(1.0, merged.extraUsage.usedCredits / merged.extraUsage.limitDollars);
        }
    }

    return merged;
}
