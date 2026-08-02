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

// 계수 상한: prior 의 이 배수.
// 플랜 한도가 대충이라도 맞다면 참값이 prior 의 몇 배씩 될 수는 없다.
// 예전 값 20 은 너무 헐거워서, 외부 사용이 섞이면 20배 과대 보고까지 허용했고
// 한 번 밀려 올라간 계수가 정상 사용만으로는 사실상 내려오지 않았다.
// 3 이면 로그가 절반쯤 누락되는 사용자(참값 ≈ 2×prior)도 수용하면서
// 최악의 피해를 3배로 묶는다.
constexpr double MAX_COEFF_RATIO = 3.0;

// 천장에 연속으로 이만큼 붙어 있으면 학습분을 버리고 prior 로 되돌린다.
// 천장에 눌려 있다는 건 선형 모델이 관측을 설명하지 못한다는 뜻이고,
// 그 상태의 계수는 prior 보다 나을 게 없다. 사용자가 알 필요 없이 스스로 낫는다.
constexpr int MAX_SATURATED_STREAK = 5;

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
        q.c[f][Calib::Input]      = perToken;
        q.c[f][Calib::Output]     = perToken;
        q.c[f][Calib::CacheWrite] = perToken;
        // 예전 CACHE_READ_WEIGHT = 0.1 과 동일한 초기값.
        q.c[f][Calib::CacheRead]  = perToken * 0.1;
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

    const double error = observedUtil - coeff.predict(features);
    if (residualOut)
        *residualOut = error;   // 양수 = 로컬 로그로 설명 안 되는 사용량(ε 추정)

    // 오염된 방향(error > 0)만 감속한다. 위 상수 주석의 유도 참고.
    const double rate = (error < 0.0) ? LEARNING_RATE
                                      : LEARNING_RATE * CONTAMINATED_RATE_FACTOR;

    bool hitCeiling = false;
    for (int f = 0; f < Calib::FamilyCount; ++f) {
        for (int k = 0; k < Calib::KindCount; ++k) {
            const double x = static_cast<double>(features.tokens[f][k]);
            if (x <= 0.0)
                continue;   // 관측되지 않은 조합은 건드리지 않는다
            coeff.c[f][k] += rate * error * x / norm;

            // 음수 방지 + 폭주 방지. prior 가 0 인 축(한도 미상)은 천장을 두지 않는다.
            if (coeff.c[f][k] < 0.0)
                coeff.c[f][k] = 0.0;
            const double ceiling = prior.c[f][k] * MAX_COEFF_RATIO;
            if (ceiling > 0.0 && coeff.c[f][k] > ceiling) {
                coeff.c[f][k] = ceiling;
                hitCeiling = true;
            }
        }
    }

    ++coeff.samples;

    // 자가 복구: 천장에 계속 눌려 있으면 학습분을 버린다.
    // 한 번은 지나가는 외란이지만, 연속이면 모델이 관측을 설명하지 못한다는
    // 뜻이라 prior 가 오히려 낫다. 사용자 개입도 GUI 도 필요 없다.
    coeff.saturatedStreak = hitCeiling ? coeff.saturatedStreak + 1 : 0;
    if (coeff.saturatedStreak >= MAX_SATURATED_STREAK) {
        const int seen = coeff.samples;
        coeff = prior;                  // samples·streak 도 함께 0 으로
        qWarning() << "[QuotaCalibrator] 계수가" << MAX_SATURATED_STREAK
                   << "회 연속 천장에 도달 — 학습분" << seen
                   << "건을 버리고 초기값으로 되돌림";
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
    o["samples"] = q.samples;
    o["saturated"] = q.saturatedStreak;
    for (int f = 0; f < Calib::FamilyCount; ++f) {
        QJsonArray row;
        for (int k = 0; k < Calib::KindCount; ++k)
            row.append(q.c[f][k]);
        o[FAMILY_KEYS[f]] = row;
    }
    return o;
}

// prior 를 함께 받아 검증한다. 저장분이 현재 천장을 넘으면 그 창은 통째로
// 버린다 — 플랜이 바뀌어 한도가 달라졌거나, 천장을 조이기 전 버전이 남긴
// 과대 계수이거나, 파일이 손상된 경우다. 어느 쪽이든 prior 가 낫다.
bool fromJson(const QJsonObject &o, QuotaCoefficients &q,
              const QuotaCoefficients &prior)
{
    if (o.isEmpty())
        return false;
    QuotaCoefficients loaded;
    loaded.samples         = o["samples"].toInt();
    loaded.saturatedStreak = o["saturated"].toInt();
    for (int f = 0; f < Calib::FamilyCount; ++f) {
        const QJsonArray row = o[FAMILY_KEYS[f]].toArray();
        if (row.size() != Calib::KindCount)
            return false;
        for (int k = 0; k < Calib::KindCount; ++k) {
            const double v = row.at(k).toDouble(-1.0);
            if (v < 0.0)
                return false;      // 손상된 값이면 통째로 버리고 prior 로 간다
            const double ceiling = prior.c[f][k] * MAX_COEFF_RATIO;
            if (ceiling > 0.0 && v > ceiling)
                return false;      // 현재 기준으로 말이 안 되는 값
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
    if (fromJson(root["5h"].toObject(), set.fiveHour, priors.fiveHour))
        ++restored;
    if (fromJson(root["7d"].toObject(), set.sevenDay, priors.sevenDay))
        ++restored;
    // 7d_sonnet 은 나중에 추가된 항목이라 없을 수 있다. 없으면 7d 값으로 시작한다.
    if (fromJson(root["7d_sonnet"].toObject(), set.sevenDaySonnet, priors.sevenDaySonnet))
        ++restored;
    else
        set.sevenDaySonnet = set.sevenDay;

    if (restored < 3) {
        qWarning() << "[QuotaCalibrator] 저장된 보정값 일부가 현재 기준을 벗어나"
                   << (3 - restored) << "개 창을 초기값으로 되돌림";
    }
    return restored > 0;
}

} // namespace QuotaCalibrator
