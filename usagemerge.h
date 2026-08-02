#ifndef USAGEMERGE_H
#define USAGEMERGE_H

#include <QDateTime>
#include "usagedata.h"

// TrayApp 의 상태(트레이 아이콘·팝업)와 무관한 순수 병합 로직.
// 여기 분리해 둔 덕에 GUI 없이 테스트할 수 있다.
namespace UsageMerge {

// 마지막 API 정확값 위에 그 이후의 로컬 델타를 얹어 현재 추정치를 만든다.
//   lastApi : 마지막으로 성공한 API 응답
//   delta   : lastApi.fetchedAt 이후 로컬 JSONL 에서 집계한 증분.
//             utilization 은 스캐너가 보정 계수로 이미 계산해 둔 값이다.
//   nowUtc  : 현재 시각 (테스트 주입용)
//
// 플랜 한도 인자는 사라졌다. 한도는 이제 보정 계수 안에 흡수돼 있어서
// 여기서는 비율끼리 더하기만 하면 된다.
UsageData mergeWithLastApi(const UsageData &lastApi,
                           const UsageData &delta,
                           const QDateTime &nowUtc);

// 델타 비용 중 추가 결제 크레딧으로 청구할 비율을 창(5h 또는 7d) 하나 기준으로
// 구한다. 크레딧은 플랜 한도를 다 쓴 뒤에야 소모되므로, 한도 안이면 0 이다.
//
// 반환값:
//   [0.0, 1.0] : 그 창이 내린 판단
//   음수       : 판단 근거 없음(창이 invalid). 호출측이 다른 창을 보게 한다.
double chargeableRatioFor(const QuotaInfo &api, const QuotaInfo &delta,
                          bool resetOccurred);

// 5h·7d 중 '가장 많이 청구해야 한다'고 보는 창을 따른다. 어느 한쪽만 한도를
// 넘겨도 크레딧은 나가기 때문이다. 판단 가능한 창이 하나도 없으면 1.0
// (과소 보고보다 과대 보고가 안전하다 — 어차피 다음 API 응답이 정정한다).
double chargeableRatio(const UsageData &lastApi, const UsageData &delta,
                       bool reset5hOccurred, bool reset7dOccurred);

} // namespace UsageMerge

#endif // USAGEMERGE_H
