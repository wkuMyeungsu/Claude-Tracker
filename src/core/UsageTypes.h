#ifndef USAGETYPES_H
#define USAGETYPES_H

// 사용량 경고 임계값 (공통)
#define USAGE_WARN_PCT 71   // 71% 이상 → 주황
#define USAGE_CRIT_PCT 86   // 86% 이상 → 빨강

#include <QDateTime>
#include <QString>
#include "QuotaCalibrator.h"

struct QuotaInfo {
    double    utilization = 0.0;  // 0.0 ~ 1.0
    QDateTime resetsAt;
    // 아래 둘은 UsageMerger::mergeQuota 가 API 정확값에 로컬 델타를 더할 때 쓴다.
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

// JSONL 한 줄에서 뽑아낸 assistant 응답 1건의 토큰 사용량.
// SessionLogReader 가 만들고, UsageAggregator·ModelPricing 이 소비한다.
struct TokenRecord {
    QDateTime ts;                  // 항상 UTC
    QString   model;
    qint64    input        = 0;
    qint64    output       = 0;
    qint64    cacheWrite   = 0;    // 캐시 쓰기 총량 (5분 + 1시간)
    qint64    cacheRead    = 0;
    // 위 cacheWrite 중 1시간 캐시분. 요율이 5분의 1.6배라 반드시 분리해야 한다.
    // usage.cache_creation 이 없는 옛 로그에서는 0 → 전량 5분 요율로 계산된다.
    qint64    cacheWrite1h = 0;
    qint64    webSearches  = 0;    // server_tool_use.web_search_requests
    bool      fastMode     = false; // usage.speed == "fast"
};

// 스캔 1회 결과. full 과 delta 는 시작 시각만 다를 뿐이라
// 레코드를 한 번 순회하며 두 누산기로 동시에 계산한다.
struct ScanResult {
    UsageData full;            // 현재 5h/7d 롤링 윈도우 전체
    UsageData delta;           // deltaStart 이후 증분 (hasDelta==false 면 full 과 동일)
    bool      hasDelta = false;

    // 보정기 학습용 특징벡터. full 윈도우 기준이며, API 가 알려주는
    // 같은 윈도우의 실제 utilization 과 짝지어 관측 1건이 된다.
    UsageFeatures full5hFeatures;
    UsageFeatures full7dFeatures;
    UsageFeatures full7dSonnetFeatures;   // Sonnet 계열만 (seven_day_sonnet 대응)
};

Q_DECLARE_METATYPE(ScanResult)

#endif // USAGETYPES_H
