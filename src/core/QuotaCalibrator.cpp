#include "QuotaCalibrator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QtGlobal>

namespace {

// 학습률. 관측 1건마다 오차의 이 비율만큼 계수를 당긴다.
// 0.25 면 지배적인 특징 하나에 대해 4~5 표본이면 수렴한다.
// API 폴링이 5분 주기이므로 하루면 충분히 안정된다.
constexpr double LEARNING_RATE = 0.25;

// 오차가 '양수'일 때만 곱하는 감속 계수.
//
// 구독 할당량은 claude.ai 웹·데스크톱·모바일과 공유되는데 로컬 JSONL 에는
// Claude Code 사용분만 남는다. 즉 우리가 못 보는 사용량 ε ≥ 0 이 항상 존재하고,
//
//     observed = c_true·x + ε        (ε ≥ 0)
//     error    = observed − c·x = (c_true − c)·x + ε
//
// error < 0 이면 ε ≥ 0 이므로 반드시 c > c_true — 외부 사용으로는 절대 만들 수
// 없는 '깨끗한' 증거라 전속으로 내린다.
// error > 0 이면 c 가 작아서인지 ε 때문인지 구별할 수 없다. 이 방향으로 전속
// 학습하면 웹을 조금만 써도 계수가 천장까지 밀려 올라간다(실제로 겪은 문제).
constexpr double CONTAMINATED_RATE_FACTOR = 0.25;

// ── 자가 복구 판정 ────────────────────────────────────────────────────────────
// 아래 셋은 '얼마나 빨리 알아채는가'만 좌우한다. 정확도의 상한을 정하지
// 않으므로, 근거 없는 배수로 천장을 치던 예전 방식과 성격이 다르다.
constexpr double EWMA_ALPHA              = 0.2;   // 최근 5 관측 정도의 기억
constexpr int    MIN_SAMPLES_BEFORE_JUDGING = 20; // 학습에 기회를 준 뒤 판정
constexpr double WORSE_MARGIN            = 1.5;   // 잡음이 아니라 확실히 나쁠 때만

// 계열별 상대 가중치 (Sonnet = 1.0 기준).
//
// 근거: Anthropic 은 차감 가중치를 공개하지 않지만, "Fable 5 는 Opus 세션의
// 약 2배를 주간 한도에서 차감한다"는 공개 수치가 단가비(10:5 = 2.0)와 맞는다.
// 그래서 단가비를 초기 가설로 쓴다. Sonnet 을 기준으로 잡은 이유는 Pro 플랜이
// Sonnet 전용이라 한도 역산의 기준이었을 가능성이 높기 때문이다.
//
// 한계: Calib::familyOf 는 fable/mythos 를 Opus 로 묶으므로 Fable 사용자는
// 초기값이 2배 과소평가된다. 학습이 메우는 몫이다.
constexpr double FAMILY_WEIGHT[Calib::FamilyCount] = {
    5.0 / 3.0,   // Opus   (단가 $5 vs Sonnet $3)
    1.0,         // Sonnet (기준)
    1.0 / 3.0,   // Haiku  (단가 $1)
    1.0,         // 그 외  — 알 수 없으므로 기준값
};

// 관측을 신뢰할 수 있는 utilization 구간.
// 0 근처는 신호가 없고, 1.0 은 포화(클리핑)라 참값을 알 수 없다.
constexpr double MIN_OBSERVABLE_UTIL = 0.005;
constexpr double MAX_OBSERVABLE_UTIL = 0.98;

const char *FAMILY_KEYS[Calib::FamilyCount] = { "opus", "sonnet", "haiku", "other" };
const char *KIND_KEYS[Calib::KindCount]     = { "in", "out", "cw", "cr" };

} // namespace

namespace Calib {

Family familyOf(const QString &modelName)
{
    const QString m = modelName.toLower();
    // fable/mythos 는 Opus 상위 티어라 별도 계열을 두지 않고 Opus 로 묶는다.
    // (요율은 model_pricing.json 이 따로 처리하므로 여기선 할당량 관점만 본다)
    if (m.contains("opus") || m.contains("fable") || m.contains("mythos"))
        return Opus;
    if (m.contains("sonnet"))
        return Sonnet;
    if (m.contains("haiku"))
        return Haiku;
    return OtherFamily;
}

} // namespace Calib

// ── UsageFeatures ────────────────────────────────────────────────────────────

void UsageFeatures::add(Calib::Family f, Calib::Kind k, qint64 n)
{
    if (n <= 0)
        return;
    tokens[f][k] += n;
}

qint64 UsageFeatures::total() const
{
    qint64 sum = 0;
    for (int f = 0; f < Calib::FamilyCount; ++f)
        for (int k = 0; k < Calib::KindCount; ++k)
            sum += tokens[f][k];
    return sum;
}

// ── QuotaCoefficients ────────────────────────────────────────────────────────

double QuotaCoefficients::predict(const UsageFeatures &f) const
{
    double u = 0.0;
    for (int i = 0; i < Calib::FamilyCount; ++i)
        for (int k = 0; k < Calib::KindCount; ++k)
            u += c[i][k] * static_cast<double>(f.tokens[i][k]);
    return u;
}

bool QuotaCoefficients::isValid() const
{
    for (int i = 0; i < Calib::FamilyCount; ++i)
        for (int k = 0; k < Calib::KindCount; ++k)
            if (c[i][k] > 0.0)
                return true;
    return false;
}

// ── QuotaCalibrator ──────────────────────────────────────────────────────────

namespace QuotaCalibrator {

QuotaCoefficients priorFor(qint64 limitTokens)
{
    QuotaCoefficients q;
    if (limitTokens <= 0)
        return q;   // 한도를 모르면 계수 0 — predict() 가 0 을 돌려준다

    const double perToken = 1.0 / static_cast<double>(limitTokens);
    for (int f = 0; f < Calib::FamilyCount; ++f) {
        const double w = FAMILY_WEIGHT[f];
        q.c[f][Calib::Input]      = perToken * w;
        q.c[f][Calib::Output]     = perToken * w;
        q.c[f][Calib::CacheWrite] = perToken * w;
        // 캐시 읽기는 단가가 입력의 1/10 이다 (전 계열 공통 비율).
        q.c[f][Calib::CacheRead]  = perToken * w * 0.1;
    }
    return q;
}

CalibrationSet priorsFor(qint64 limit5h, qint64 limit7d)
{
    CalibrationSet s;
    s.fiveHour       = priorFor(limit5h);
    s.sevenDay       = priorFor(limit7d);
    s.sevenDaySonnet = priorFor(limit7d);
    return s;
}

bool observe(QuotaCoefficients &coeff, const UsageFeatures &features,
             double observedUtil, const QuotaCoefficients &prior,
             double *residualOut)
{
    if (residualOut)
        *residualOut = 0.0;

    if (observedUtil < MIN_OBSERVABLE_UTIL || observedUtil > MAX_OBSERVABLE_UTIL)
        return false;
    if (features.isEmpty())
        return false;

    // 정규화 LMS: Δc_i = μ · error · x_i / Σx_j²
    // 토큰 수가 수백만이라 제곱합이 커지지만 double 범위에서는 여유롭다.
    double norm = 0.0;
    for (int f = 0; f < Calib::FamilyCount; ++f) {
        for (int k = 0; k < Calib::KindCount; ++k) {
            const double x = static_cast<double>(features.tokens[f][k]);
            norm += x * x;
        }
    }
    if (norm <= 0.0)
        return false;

    const double error      = observedUtil - coeff.predict(features);
    const double errorPrior = observedUtil - prior.predict(features);
    if (residualOut)
        *residualOut = error;   // 양수 = 로컬 로그로 설명 안 되는 사용량(ε 추정)

    // 학습분과 prior 의 성능을 나란히 누적한다. 갱신 '전' 오차로 비교해야
    // 같은 관측을 두 모델이 똑같이 맞히는 조건이 된다.
    coeff.errLearned = EWMA_ALPHA * error * error
                     + (1.0 - EWMA_ALPHA) * coeff.errLearned;
    coeff.errPrior   = EWMA_ALPHA * errorPrior * errorPrior
                     + (1.0 - EWMA_ALPHA) * coeff.errPrior;

    // 오염된 방향(error > 0)만 감속한다. 위 상수 주석의 유도 참고.
    const double rate = (error < 0.0) ? LEARNING_RATE
                                      : LEARNING_RATE * CONTAMINATED_RATE_FACTOR;

    for (int f = 0; f < Calib::FamilyCount; ++f) {
        for (int k = 0; k < Calib::KindCount; ++k) {
            const double x = static_cast<double>(features.tokens[f][k]);
            if (x <= 0.0)
                continue;   // 관측되지 않은 조합은 건드리지 않는다
            coeff.c[f][k] += rate * error * x / norm;
            if (coeff.c[f][k] < 0.0)
                coeff.c[f][k] = 0.0;   // 사용률을 깎는 토큰은 존재하지 않는다
        }
    }

    // 상한은 '추측한 배수'가 아니라 정의에서 나온다: utilization 은 100% 를
    // 넘을 수 없다. 이 관측에 대해 100% 초과를 예측하는 계수 조합은 관측과
    // 모순이므로 그만큼만 되돌린다.
    //
    // predict 는 x>0 인 축들의 합이므로, 그 축들만 같은 비율로 줄이면 예측이
    // 정확히 그 비율로 줄어든다.
    const double predicted = coeff.predict(features);
    if (predicted > 1.0) {
        const double scale = 1.0 / predicted;
        for (int f = 0; f < Calib::FamilyCount; ++f)
            for (int k = 0; k < Calib::KindCount; ++k)
                if (features.tokens[f][k] > 0)
                    coeff.c[f][k] *= scale;
    }

    ++coeff.samples;

    // 자가 복구: 학습분이 prior 보다 확실히 못 맞히면 되돌린다.
    // 참 한도를 몰라도 '어느 쪽이 더 잘 맞히는가'는 직접 잴 수 있다.
    // 사용자 개입도 GUI 도 필요 없다.
    if (coeff.samples >= MIN_SAMPLES_BEFORE_JUDGING
        && coeff.errLearned > coeff.errPrior * WORSE_MARGIN) {
        const int seen = coeff.samples;
        coeff = prior;                  // samples·EWMA 도 함께 초기화
        qWarning() << "[QuotaCalibrator] 학습분이 초기값보다 예측이 나빠"
                   << seen << "건을 버리고 되돌림";
    }

    return true;
}

// ── 영속화 ───────────────────────────────────────────────────────────────────
// QSettings 에 계열/종류별 키를 따로 만들면 키가 48 개로 불어난다.
// JSON 한 덩어리로 직렬화해 문자열 하나에 넣는다.

namespace {

QJsonObject toJson(const QuotaCoefficients &q)
{
    QJsonObject o;
    o["samples"]     = q.samples;
    o["errLearned"]  = q.errLearned;
    o["errPrior"]    = q.errPrior;
    for (int f = 0; f < Calib::FamilyCount; ++f) {
        QJsonArray row;
        for (int k = 0; k < Calib::KindCount; ++k)
            row.append(q.c[f][k]);
        o[FAMILY_KEYS[f]] = row;
    }
    return o;
}

// 손상 여부만 본다. '값이 너무 크다'는 판정은 여기서 하지 않는다 — 그러려면
// 근거 없는 배수가 다시 필요해지기 때문이다. 과대한 계수는 첫 관측에서
// predict ≤ 1.0 제약이 즉시 끌어내리고, 그래도 나쁘면 자가 복구가 되돌린다.
bool fromJson(const QJsonObject &o, QuotaCoefficients &q)
{
    if (o.isEmpty())
        return false;
    QuotaCoefficients loaded;
    loaded.samples    = o["samples"].toInt();
    loaded.errLearned = o["errLearned"].toDouble(0.0);
    loaded.errPrior   = o["errPrior"].toDouble(0.0);
    for (int f = 0; f < Calib::FamilyCount; ++f) {
        const QJsonArray row = o[FAMILY_KEYS[f]].toArray();
        if (row.size() != Calib::KindCount)
            return false;
        for (int k = 0; k < Calib::KindCount; ++k) {
            const double v = row.at(k).toDouble(-1.0);
            if (v < 0.0 || !qIsFinite(v))
                return false;      // 손상된 값이면 통째로 버리고 prior 로 간다
            loaded.c[f][k] = v;
        }
    }
    q = loaded;
    return true;
}

} // namespace

void saveTo(const QString &group, const CalibrationSet &set)
{
    QJsonObject root;
    root["5h"]        = toJson(set.fiveHour);
    root["7d"]        = toJson(set.sevenDay);
    root["7d_sonnet"] = toJson(set.sevenDaySonnet);

    QSettings s("ClaudeTray", "ClaudeTray");
    s.setValue(group, QString::fromUtf8(
                   QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

bool loadFrom(const QString &group, CalibrationSet &set, const CalibrationSet &priors)
{
    set = priors;   // 어떤 창이든 검증에 실패하면 그 창은 prior 로 남는다

    QSettings s("ClaudeTray", "ClaudeTray");
    const QString blob = s.value(group).toString();
    if (blob.isEmpty())
        return false;

    const QJsonObject root = QJsonDocument::fromJson(blob.toUtf8()).object();

    // 창마다 독립적으로 판단한다. 5h 가 망가졌다고 7d 학습분까지 버릴 이유가 없다.
    int restored = 0;
    if (fromJson(root["5h"].toObject(), set.fiveHour))
        ++restored;
    if (fromJson(root["7d"].toObject(), set.sevenDay))
        ++restored;
    // 7d_sonnet 은 나중에 추가된 항목이라 없을 수 있다. 없으면 7d 값으로 시작한다.
    if (fromJson(root["7d_sonnet"].toObject(), set.sevenDaySonnet))
        ++restored;
    else
        set.sevenDaySonnet = set.sevenDay;

    if (restored < 3) {
        qWarning() << "[QuotaCalibrator] 저장된 보정값" << (3 - restored)
                   << "개 창이 손상돼 초기값으로 되돌림";
    }
    return restored > 0;
}

} // namespace QuotaCalibrator
