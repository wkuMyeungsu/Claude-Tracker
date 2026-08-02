#ifndef QUOTACALIBRATOR_H
#define QUOTACALIBRATOR_H

#include <QString>

// 로컬 토큰 집계를 API 가 알려주는 실제 utilization 에 맞춰 스스로 보정한다.
//
// 배경: 예전에는 "토큰 = input + output + cacheWrite + 0.1*cacheRead" 를
// 하드코딩된 플랜 한도로 나눠 비율을 만들었다. 가중치 0.1 도, 플랜 한도도
// 근거 없는 추정값이었고 모델별 차이(Opus vs Haiku)는 아예 무시했다.
//
// 여기서는 그 관계를 선형 모델로 두고 계수를 관측으로 학습한다.
//
//     utilization ≈ Σ  c[계열][종류] × 토큰수[계열][종류]
//
// c 는 "토큰 1개가 할당량의 몇 %를 먹는가"이다. API 응답이 도착할 때마다
// (로컬 집계 특징벡터, API 실제 utilization) 한 쌍을 관측으로 얻어
// 정규화 LMS 로 c 를 조금씩 당긴다. 쓸수록 정확해지는 구조이며,
// 초기값(prior)을 기존 하드코딩 공식과 똑같이 두었으므로 학습 표본이
// 0 건일 때의 동작은 예전과 완전히 동일하다.

namespace Calib {

enum Family { Opus = 0, Sonnet, Haiku, OtherFamily, FamilyCount };
enum Kind   { Input = 0, Output, CacheWrite, CacheRead, KindCount };

// 모델명에서 계열을 판정한다. fable/mythos 는 Opus 급으로 묶는다.
Family familyOf(const QString &modelName);

} // namespace Calib

// 한 윈도우(5h / 7d / 7d-sonnet)에 대한 토큰 집계. [계열][종류]
struct UsageFeatures {
    qint64 tokens[Calib::FamilyCount][Calib::KindCount] = {};

    void   add(Calib::Family f, Calib::Kind k, qint64 n);
    qint64 total() const;
    bool   isEmpty() const { return total() == 0; }
};

// 한 윈도우의 계수 집합.
struct QuotaCoefficients {
    double c[Calib::FamilyCount][Calib::KindCount] = {};
    int    samples = 0;      // 학습에 반영된 관측 수 (신뢰도 표시용)

    double predict(const UsageFeatures &f) const;
    bool   isValid() const;  // 계수가 하나라도 양수인가
};

struct CalibrationSet {
    QuotaCoefficients fiveHour;
    QuotaCoefficients sevenDay;
    QuotaCoefficients sevenDaySonnet;
};

namespace QuotaCalibrator {

// 하드코딩 플랜 한도에서 출발하는 초기 계수.
// input/output/cacheWrite 는 1/limit, cacheRead 는 0.1/limit —
// 예전 CACHE_READ_WEIGHT 공식과 정확히 같은 값이다.
QuotaCoefficients priorFor(qint64 limitTokens);
CalibrationSet    priorsFor(qint64 limit5h, qint64 limit7d);

// 관측 1건으로 계수를 갱신한다 (정규화 LMS).
//   observedUtil : API 가 알려준 그 윈도우의 실제 utilization
//   prior        : 계수 상·하한을 잡기 위한 기준값
// 학습에 반영했으면 true. 신호가 없는 관측(빈 특징벡터, 포화된 100%,
// 0% 등)은 건드리지 않고 false 를 돌려준다.
bool observe(QuotaCoefficients &coeff, const UsageFeatures &features,
             double observedUtil, const QuotaCoefficients &prior);

// QSettings 영속화 (키 접두사별로 저장).
void saveTo(const QString &group, const CalibrationSet &set);
bool loadFrom(const QString &group, CalibrationSet &set);

} // namespace QuotaCalibrator

#endif // QUOTACALIBRATOR_H
