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

    // 자가 복구 판정용. 학습한 계수와 prior 각각의 제곱오차 EWMA 를 나란히
    // 들고 있다가, 학습분이 prior 보다 확실히 나쁘면 되돌린다.
    // "천장에 몇 번 붙었나" 같은 임의 기준 대신 '실제로 더 잘 맞히는가'를
    // 직접 재는 것이라, 참 한도를 몰라도 판정할 수 있다.
    double errLearned = 0.0;
    double errPrior   = 0.0;

    double predict(const UsageFeatures &f) const;
    bool   isValid() const;  // 계수가 하나라도 양수인가
};

// 추가 결제 크레딧 보정 계수.
//
// 5h/7d 와 달리 계열·종류별 벡터를 두지 않는다. 크레딧 추정치는 이미
// ModelPricingTable 이 모델·캐시 티어·fast 모드까지 반영해 '달러'로 계산한
// 값이라, 남는 미지수는 그 달러가 실제 크레딧으로 환산되는 '배율' 하나다.
//
//     크레딧 = API 실측값 + (로컬 델타비용 × 청구비율) × k
//
// k 가 흡수하는 오차는 두 갈래다.
//   (1) 요율표와 Anthropic 실제 과금의 차이(반올림·미공개 할증)
//   (2) UsageMerger::chargeableRatio 의 선형 근사 오차
// 둘 다 증분에 곱으로 붙으므로 스칼라 하나로 맞출 수 있다.
//
// 스칼라를 고른 실질적인 이유가 하나 더 있다: 크레딧 관측은 플랜 한도를 다
// 쓴 뒤에만 생겨서 표본이 매우 귀하다. 16개짜리 벡터는 수렴할 기회가 없다.
struct CreditCoefficient {
    double k       = 1.0;    // 1.0 = 요율표 그대로 (prior)
    int    samples = 0;

    // QuotaCoefficients 와 같은 자가 복구용. 다만 이쪽은 증분 크기가 관측마다
    // 크게 달라서(몇 센트 ~ 몇 달러) 절대 오차 대신 '상대 오차'를 누적한다.
    double errLearned = 0.0;
    double errPrior   = 0.0;

    bool isValid() const;
};

struct CalibrationSet {
    QuotaCoefficients fiveHour;
    QuotaCoefficients sevenDay;
    QuotaCoefficients sevenDaySonnet;
    CreditCoefficient credit;
};

namespace QuotaCalibrator {

// 플랜 한도에서 출발하는 초기 계수.
//
// Anthropic 은 모델별 할당량 차감 가중치를 공개하지 않는다. 다만 "Fable 5 는
// Opus 세션의 약 2배를 주간 한도에서 차감한다"는 공개 수치가 단가비(10:5=2.0)와
// 일치하므로, '차감량이 단가에 비례한다'를 초기 가설로 삼아 계열별로 차등한다.
// 예전처럼 모든 계열을 동일하게 두는 것은 단가가 5배 차이나는 점에 비추어
// 확실히 틀린 가정이었다. 가설이 빗나가도 학습이 교정한다.
QuotaCoefficients priorFor(qint64 limitTokens);
CalibrationSet    priorsFor(qint64 limit5h, qint64 limit7d);

// 관측 1건으로 계수를 갱신한다 (정규화 LMS).
//   observedUtil : API 가 알려준 그 윈도우의 실제 utilization
//   prior        : 계수 상·하한과 자가 복구의 기준값
//   residualOut  : 갱신 '전' 잔차 (observedUtil - predict). 양수면 로컬 로그로
//                  설명되지 않는 사용량 — claude.ai 등 외부 표면 사용의 추정치다.
// 상태를 바꿨으면 true (저장 필요). 신호가 없는 관측(빈 특징벡터, 포화된 100%,
// 0% 등)은 건드리지 않고 false 를 돌려준다.
bool observe(QuotaCoefficients &coeff, const UsageFeatures &features,
             double observedUtil, const QuotaCoefficients &prior,
             double *residualOut = nullptr);

// 크레딧 관측 1건으로 배율을 갱신한다.
//   rawIncrement    : 보정 '전' 예측 증가분 (델타비용 × 청구비율, 달러)
//   actualIncrement : 같은 구간의 API 실측 증가분 (달러)
// 신호가 없는 관측(증분이 반올림 잡음 수준, 음수)은 무시하고 false 를 돌려준다.
bool observeCredit(CreditCoefficient &coeff, double rawIncrement,
                   double actualIncrement, double *residualOut = nullptr);

// QSettings 영속화 (키 접두사별로 저장).
void saveTo(const QString &group, const CalibrationSet &set);
// priors 를 함께 받아 불러온 값을 검증한다. 플랜이 바뀌어 한도가 달라졌거나
// 저장분이 손상됐으면 그 창은 prior 로 되돌린다.
bool loadFrom(const QString &group, CalibrationSet &set, const CalibrationSet &priors);

} // namespace QuotaCalibrator

#endif // QUOTACALIBRATOR_H
