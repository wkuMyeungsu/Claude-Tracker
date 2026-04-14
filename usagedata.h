#ifndef USAGEDATA_H
#define USAGEDATA_H

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
