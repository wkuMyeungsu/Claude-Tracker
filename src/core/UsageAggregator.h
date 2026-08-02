#ifndef USAGEAGGREGATOR_H
#define USAGEAGGREGATOR_H

#include <QDateTime>
#include <QString>
#include <QVector>
#include "UsageTypes.h"
#include "QuotaCalibrator.h"

// 레코드 묶음을 5h/7d 윈도우로 갈라 사용률과 비용을 낸다.
// 파일 I/O 도 GUI 도 없는 순수 함수 모음이라 테스트가 직접 호출한다.
namespace UsageAggregator {

constexpr qint64 SECS_5H = 5LL * 3600;
constexpr qint64 SECS_7D = 7LL * 24 * 3600;

// 플랜별 토큰 한도. 보정기 prior 의 출발점일 뿐이며, 관측이 쌓이면
// 계수가 실측에 맞춰 이 값에서 멀어진다.
// 구독 종류를 인자로 받는다 — core 가 자격 증명 읽기(data)에 의존하지 않도록.
qint64 planLimit5h(const QString &subscriptionType);
qint64 planLimit7d(const QString &subscriptionType);

// 마지막 리셋 시각이 이미 과거면 주기를 더해 다음 리셋 시각을 추정한다.
QDateTime estimateNextReset(const QDateTime &last, qint64 periodSecs,
                            const QDateTime &nowUtc);

// 집계에 필요한 가장 이른 시각. 이보다 오래된 레코드는 파싱 단계에서 버린다.
QDateTime earliestRelevant(const QDateTime &nowUtc,
                           const QDateTime &deltaStartUtc,
                           const QDateTime &reset7d);

// 레코드만으로 집계한다.
// utilization 은 calibration 계수로 계산한다 (한도 나눗셈이 아니다).
ScanResult aggregate(const QVector<TokenRecord> &records,
                     const QDateTime &nowUtc,
                     const QDateTime &deltaStartUtc,
                     const QDateTime &reset5h,
                     const QDateTime &reset7d,
                     const CalibrationSet &calibration);

} // namespace UsageAggregator

#endif // USAGEAGGREGATOR_H
