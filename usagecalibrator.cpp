#include "usagecalibrator.h"

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

// 계수 상한: prior 의 이 배수. 로그가 일부 유실된 사용자(다른 PC 에서도
// Claude Code 를 쓰는 경우)는 계수가 위로 밀리는데, 그게 그 사용자에게는
// 오히려 맞는 보정이다. 다만 폭주는 막아야 하므로 천장을 둔다.
constexpr double MAX_COEFF_RATIO = 20.0;

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

// ── UsageCalibrator ──────────────────────────────────────────────────────────

namespace UsageCalibrator {

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
             double observedUtil, const QuotaCoefficients &prior)
{
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

    for (int f = 0; f < Calib::FamilyCount; ++f) {
        for (int k = 0; k < Calib::KindCount; ++k) {
            const double x = static_cast<double>(features.tokens[f][k]);
            if (x <= 0.0)
                continue;   // 관측되지 않은 조합은 건드리지 않는다
            coeff.c[f][k] += LEARNING_RATE * error * x / norm;

            // 음수 방지 + 폭주 방지. prior 가 0 인 축(한도 미상)은 천장을 두지 않는다.
            if (coeff.c[f][k] < 0.0)
                coeff.c[f][k] = 0.0;
            const double ceiling = prior.c[f][k] * MAX_COEFF_RATIO;
            if (ceiling > 0.0 && coeff.c[f][k] > ceiling)
                coeff.c[f][k] = ceiling;
        }
    }

    ++coeff.samples;
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
    for (int f = 0; f < Calib::FamilyCount; ++f) {
        QJsonArray row;
        for (int k = 0; k < Calib::KindCount; ++k)
            row.append(q.c[f][k]);
        o[FAMILY_KEYS[f]] = row;
    }
    return o;
}

bool fromJson(const QJsonObject &o, QuotaCoefficients &q)
{
    if (o.isEmpty())
        return false;
    q.samples = o["samples"].toInt();
    for (int f = 0; f < Calib::FamilyCount; ++f) {
        const QJsonArray row = o[FAMILY_KEYS[f]].toArray();
        if (row.size() != Calib::KindCount)
            return false;
        for (int k = 0; k < Calib::KindCount; ++k) {
            const double v = row.at(k).toDouble(-1.0);
            if (v < 0.0)
                return false;      // 손상된 값이면 통째로 버리고 prior 로 간다
            q.c[f][k] = v;
        }
    }
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

bool loadFrom(const QString &group, CalibrationSet &set)
{
    QSettings s("ClaudeTray", "ClaudeTray");
    const QString blob = s.value(group).toString();
    if (blob.isEmpty())
        return false;

    const QJsonObject root = QJsonDocument::fromJson(blob.toUtf8()).object();
    CalibrationSet loaded;
    if (!fromJson(root["5h"].toObject(), loaded.fiveHour))
        return false;
    if (!fromJson(root["7d"].toObject(), loaded.sevenDay))
        return false;
    // 7d_sonnet 은 나중에 추가된 항목이라 없을 수 있다. 없으면 7d 로 시작한다.
    if (!fromJson(root["7d_sonnet"].toObject(), loaded.sevenDaySonnet))
        loaded.sevenDaySonnet = loaded.sevenDay;

    set = loaded;
    return true;
}

} // namespace UsageCalibrator
