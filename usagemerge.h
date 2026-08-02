#ifndef USAGEMERGE_H
#define USAGEMERGE_H

#include <QDateTime>
#include "usagedata.h"

// TrayApp 의 상태(트레이 아이콘·팝업)와 무관한 순수 병합 로직.
// 여기 분리해 둔 덕에 GUI 없이 테스트할 수 있다.
namespace UsageMerge {

// 마지막 API 정확값 위에 그 이후의 로컬 델타를 얹어 현재 추정치를 만든다.
//   lastApi         : 마지막으로 성공한 API 응답
//   delta           : lastApi.fetchedAt 이후 로컬 JSONL 에서 집계한 증분
//   limit5h/limit7d : 플랜 한도(토큰). 0 이면 비율을 만들 수 없어 API 값을 유지한다.
//   nowUtc          : 현재 시각 (테스트 주입용)
UsageData mergeWithLastApi(const UsageData &lastApi,
                           const UsageData &delta,
                           qint64 limit5h,
                           qint64 limit7d,
                           const QDateTime &nowUtc);

} // namespace UsageMerge

#endif // USAGEMERGE_H
