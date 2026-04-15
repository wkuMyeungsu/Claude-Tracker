#ifndef USAGEDATA_H
#define USAGEDATA_H

// 사용량 경고 임계값 (공통)
#define USAGE_WARN_PCT 71   // 71% 이상 → 주황
#define USAGE_CRIT_PCT 86   // 86% 이상 → 빨강

#include <QDateTime>

struct QuotaInfo {
    double    utilization = 0.0;  // 0.0 ~ 1.0
    QDateTime resetsAt;
    qint64    rawTokens  = 0;     // 실제 토큰 수 (표시용)
    qint64    limitTokens = 0;    // 비교 기준 한도 (표시용)
    bool      valid = false;
};

struct UsageData {
    QuotaInfo fiveHour;
    QuotaInfo sevenDay;
    QuotaInfo sevenDaySonnet;
    QDateTime fetchedAt;
    bool      fromApi = false;   // true=API 정확값, false=로컬 추정
};

#endif // USAGEDATA_H
