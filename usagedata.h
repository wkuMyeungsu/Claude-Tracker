#ifndef USAGEDATA_H
#define USAGEDATA_H

// 사용량 경고 임계값 (공통)
#define USAGE_WARN_PCT 71   // 71% 이상 → 주황
#define USAGE_CRIT_PCT 86   // 86% 이상 → 빨강

#include <QDateTime>

struct QuotaInfo {
    double    utilization = 0.0;  // 0.0 ~ 1.0
    QDateTime resetsAt;
    // 아래 둘은 UsageMerge::mergeQuota 가 API 정확값에 로컬 델타를 더할 때 쓴다.
    // (화면에는 utilization 만 나가고 토큰 수 자체는 표시하지 않는다.)
    qint64    rawTokens  = 0;     // 가중 적용된 토큰 수
    qint64    limitTokens = 0;    // 비교 기준 한도
    bool      valid = false;
};

struct ExtraUsageInfo {
    bool   enabled = false;
    double limitDollars = 0.0;
    double usedCredits = 0.0;
    double utilization = 0.0; // 0.0 ~ 1.0
};

struct UsageData {
    QuotaInfo fiveHour;
    QuotaInfo sevenDay;
    // Sonnet 전용 주간 한도. API 가 seven_day 와 따로 주는 것 자체가
    // 모델별 미터링이 존재한다는 증거라, 보정기도 이 창을 따로 학습한다.
    QuotaInfo sevenDaySonnet;
    ExtraUsageInfo extraUsage;
    QString   recentModel;
    QDateTime fetchedAt;
    bool      fromApi = false;   // true=API 정확값, false=로컬 추정
};

#endif // USAGEDATA_H
