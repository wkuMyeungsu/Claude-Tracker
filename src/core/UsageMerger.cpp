#include "UsageMerger.h"
#include "UsageAggregator.h"

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

double UsageMerger::chargeableRatioFor(const QuotaInfo &api, const QuotaInfo &delta,
                                      bool resetOccurred)
{
    if (!api.valid)
        return -1.0;                      // 이 창으로는 판단할 수 없다

    if (resetOccurred) {
        // 델타 구간 안에서 리셋이 일어났다. 델타 '비용'은 리셋 전 지출까지
        // 포함하는데(크레딧은 월 단위라 5h 리셋으로 자르면 안 된다) 델타
        // '비율'은 리셋 이후만 센다. 두 창이 어긋나 정확한 안분이 불가능하다.
        //
        // 리셋 직전에 한도를 이미 채운 상태였다면 그 지출은 실제로 크레딧에서
        // 나갔으므로 전액 청구한다. 여기서 0 을 돌려주면 리셋 직전 초과분이
        // 통째로 사라진다(aggregate 단계에서 일부러 살려둔 비용이다).
        return api.utilization >= 1.0 ? 1.0 : 0.0;
    }

    const double deltaUtil = delta.valid ? delta.utilization : 0.0;
    const double totalUtil = api.utilization + deltaUtil;

    if (totalUtil <= 1.0)
        return 0.0;                       // 한도 안 → 크레딧은 아직 안 쓴다
    if (deltaUtil <= 0.0)
        return 1.0;                       // 델타가 없는데 이미 초과 상태

    // 델타 중 100% 를 넘어선 몫만 청구한다.
    // 주의: 델타 구간 안에서 비용이 사용률에 비례해 고르게 발생했다고 보는
    // 선형 근사다. Opus 와 Haiku 가 섞이면 실제와 어긋나지만, 다음 API 응답이
    // 정확값으로 덮어쓰므로 오차는 폴링 간격(5분) 안으로 제한된다.
    return qMin(1.0, (totalUtil - 1.0) / deltaUtil);
}

double UsageMerger::chargeableRatio(const UsageData &lastApi, const UsageData &delta,
                                   bool reset5hOccurred, bool reset7dOccurred)
{
    // 5h 만 보면 안 된다. 주간 한도를 다 쓴 상태에서 5시간 창이 막 리셋돼
    // 30% 라면, 실제로는 크레딧이 나가는데 청구를 0 으로 막아버린다.
    const double r5h =
        chargeableRatioFor(lastApi.fiveHour, delta.fiveHour, reset5hOccurred);
    const double r7d =
        chargeableRatioFor(lastApi.sevenDay, delta.sevenDay, reset7dOccurred);

    const double best = qMax(r5h, r7d);
    return best < 0.0 ? 1.0 : best;       // 판단 가능한 창이 없으면 전액 청구
}

UsageData UsageMerger::mergeWithLastApi(const UsageData &lastApi,
                                       const UsageData &delta,
                                       const QDateTime &nowUtc,
                                       double creditScale)
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
        UsageAggregator::estimateNextReset(lastApi.fiveHour.resetsAt, SECS_5H, nowUtc);
    const QDateTime next7d =
        UsageAggregator::estimateNextReset(lastApi.sevenDay.resetsAt, SECS_7D, nowUtc);
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

        // 추가 결제 크레딧은 플랜 한도를 다 쓴 뒤에야 소모된다. 한도 안에서
        // 쓴 토큰까지 크레딧에 더하면 쓰지도 않은 돈이 올라간다.
        const double ratio =
            chargeableRatio(lastApi, delta, reset5hOccurred, reset7dOccurred);

        // 학습된 배율은 '증분'에만 곱한다. apiBaseCredits 는 API 실측값이라
        // 이미 정확하고, 거기에 배율을 먹이면 맞는 값을 틀리게 만든다.
        const double scale = (creditScale > 0.0 && qIsFinite(creditScale)) ? creditScale : 1.0;
        merged.extraUsage.usedCredits =
            apiBaseCredits + delta.extraUsage.usedCredits * ratio * scale;
        if (merged.extraUsage.limitDollars > 0.0) {
            merged.extraUsage.utilization =
                qMin(1.0, merged.extraUsage.usedCredits / merged.extraUsage.limitDollars);
        }
    }

    return merged;
}
