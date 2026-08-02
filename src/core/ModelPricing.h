#ifndef MODELPRICING_H
#define MODELPRICING_H

#include <QString>
#include "UsageTypes.h"

// 모델별 단가($/1M 토큰). resources/model_pricing.json 에서 읽어온다.
struct ModelPricing {
    double inputRate        = 0.0;
    double outputRate       = 0.0;
    double cacheWriteRate   = 0.0;   // 5분 캐시 쓰기 (입력가 x1.25)
    double cacheWrite1hRate = 0.0;   // 1시간 캐시 쓰기 (입력가 x2)
    double cacheReadRate    = 0.0;
    // usage.speed == "fast" 일 때 전 항목에 곱하는 배수.
    // 1.0 이면 fast mode 미지원 모델이라는 뜻이다.
    double fastMultiplier   = 1.0;
};

// 요율표 조회와 비용 계산. 스캔·감시와 아무 관련이 없어 따로 두었다.
namespace ModelPricingTable {

// 모델 ID 로 요율을 찾는다. 계열(opus/sonnet/...) → 버전 순으로 매칭하며,
// 요율표에 없는 신모델은 그 계열의 최신 요율로 폴백한다.
ModelPricing forModel(const QString &modelName);

// 레코드 1건의 비용($). fast mode 배수와 1시간 캐시 요율을 반영한다.
double costOf(const TokenRecord &record);

} // namespace ModelPricingTable

#endif // MODELPRICING_H
